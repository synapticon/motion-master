#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "node/process_data_ring.h"
#include "node/process_image.h"

namespace mm::node {

/// @brief The live process-data runtime: the published image, the cross-thread exchange buffers,
///        and per-object PDO access over them.
///
/// Carries the output (master→slave) and input (slave→master) images across the RT / non-RT
/// boundary, the pointer to the currently published @c ProcessImage layout, and the working-counter
/// health. @c DeviceManager owns one of these and drives the whole-buffer exchange each cycle; it
/// also hands a pointer to it to every @c Device so a device can read and write its own PDO-mapped
/// objects (@c readPdo / @c writePdo) without knowing how the image is stored. That single seam is
/// what lets @c Device::readParameter / @c Device::writeParameter serve the live IO-map value while
/// exchanging — identically from an HTTP handler or an RT task — and fall back to SDO otherwise.
///
/// Both accessors are lock-free (so an RT task can read a value or set a setpoint without ever
/// blocking on a non-RT writer). The output path is single-composer by construction: each output
/// object's value lives in its own @c DeviceParameter cell, which any number of writers store into
/// independently, and the RT loop is the sole thread that composes those cells into the packed wire
/// image each cycle (@c DeviceManager::exchangeProcessData). Composing on one thread is what makes
/// bit-packed objects that share a byte safe without a lock — see NEXTGEN.md (Design B).
///
/// Defined in a header (rather than pimpl'd) so both @c device.cc and @c device_manager.cc can use
/// it; the recorder ring and fixed buffers are heavy, but only those two translation units pull
/// them in (the public @c device.h / @c device_manager.h only forward-declare it).
struct ProcessData {
  // The currently published image layout, or nullptr when no image is published (exchange is then
  // a no-op). Loaded lock-free by readers; published with release ordering by the control plane.
  std::atomic<const ProcessImage*> image{nullptr};
  // Every image ever published since the last reset(), retained so a lock-free reader can never
  // dereference a freed image after a re-map republishes a new one.
  std::vector<std::shared_ptr<const ProcessImage>> generations;
  // The lossless flight-data recorder: the RT loop appends one record per cycle (raw input + output
  // IOmap, timestamp, working counter), the source for both the live monitoring stream and point
  // reads of the freshest value (head()-1). Allocated at configureProcessData (sizes known then),
  // re-allocated on a layout-changing re-map, retained across teardown, freed by reset()/scan().
  ProcessDataRing ring;
  // RT-thread scratch for the exchange (touched only on the RT thread).
  ProcessBuffer outScratch;
  ProcessBuffer inScratch;
  // How deep the RT thread currently is inside work that reads the device set or the IOmap; lets
  // stopExchange() drain an in-flight cycle before a re-map or teardown mutates either.
  //
  // A depth counter rather than a flag because it is raised at two nesting levels. GameLoop takes a
  // DeviceManager::CycleGuard around the whole task list, because a task resolves devices and
  // parameters of its own and must not run while the device set is being replaced. Inside that,
  // exchangeProcessData raises it again around the exchange, so it stays self-contained for a
  // caller that invokes it directly. Only the RT thread raises it, so the count is small and
  // bounded by nesting.
  std::atomic<int> inCycle{0};
  // Working-counter health. lastWkc is written by the RT thread each cycle; expectedWkc is
  // recomputed by the control plane from current device states. healthy = last >= expected.
  std::atomic<int> lastWkc{0};
  std::atomic<int> expectedWkc{0};
  // Cumulative record of the cycles the bus did not fully answer, written by the RT thread and
  // cleared only by DeviceManager::reset (they run for the life of one bus session).
  //
  // Deliberately not per image: a re-map happens whenever anyone brings a device into or out of
  // SAFE-OP/OP, so clearing per generation erased the history at the moment someone was chasing
  // a fault, and left the figure describing a window nobody chose.
  //
  // A boolean sampled off the RT path cannot see a fault that arrived and cleared between two
  // samples, and nothing in this process samples continuously — so a count, plus the epoch
  // nanoseconds of the first and last bad cycle, is what survives to be read afterwards. The
  // timestamps are the point: they are what lets a process-data fault be lined up against whatever
  // else the log shows at that moment, which a bare count cannot answer.
  std::atomic<uint64_t> shortWkcCycles{0};
  std::atomic<uint64_t> firstShortWkcNs{0};
  std::atomic<uint64_t> lastShortWkcNs{0};

  /// @brief Control plane: unpublishes the image and waits out the RT cycle already in flight.
  ///
  /// The pause every mutation of shared state needs when it cannot take a lock the RT thread also
  /// takes — replacing a device's parameter map, rebuilding the device set, re-mapping the image.
  /// Unpublishing is what stops a *new* cycle starting (@c DeviceManager::CycleGuard and
  /// @c exchangeProcessData both back out on a null image); the wait covers the one already
  /// running. Together they mean no RT thread is inside the cycle body when this returns.
  ///
  /// Bounded, so a stalled or absent RT loop can never hang a control-plane call; giving up is
  /// logged rather than silent.
  ///
  /// @return The image that was published, to hand back to @c resumeCycle. @c nullptr if none was.
  const ProcessImage* pauseCycle();

  /// @brief Control plane: republishes @p previous, letting RT work resume.
  ///
  /// Pass back exactly what @c pauseCycle returned. A permanent stop (teardown, re-map, rescan)
  /// simply never calls this — it publishes a freshly built image instead, or nothing at all.
  void resumeCycle(const ProcessImage* previous);

  /// @brief True when an image is published and the last working counter meets the expectation.
  ///
  /// A read of an *input* off a published-but-unhealthy bus would serve stale snapshot bytes (a
  /// short working counter means the driver left the prior cycle's bytes in the IOmap), so the
  /// input branch of @c readPdo gates on this and the caller falls back to the authoritative SDO
  /// upload. Outputs are not gated — an output cell is always our own valid setpoint.
  bool healthy() const {
    return image.load(std::memory_order_acquire) != nullptr &&
           lastWkc.load(std::memory_order_relaxed) >= expectedWkc.load(std::memory_order_relaxed);
  }

  /// @brief Reads a PDO-mapped object's current bytes from the live image. Lock-free.
  ///
  /// Outputs come from the parameter's own cell (our current setpoint — always valid); inputs from
  /// the newest recorded cycle (@c ring head()-1), gated on @c healthy(). Returns @c nullopt when
  /// no image is published, nothing has been recorded yet, the object is not PDO-mapped, its owning
  /// parameter is unknown, or (inputs only) the bus is unhealthy — in each case the caller reads it
  /// over SDO instead. Bytes are little-endian and LSB-aligned.
  std::optional<std::vector<uint8_t>> readPdo(uint16_t slavePosition, uint16_t index,
                                              uint8_t subindex) const;

  /// @brief Whether an object is output-mapped in the published image, and so driven cyclically.
  ///
  /// Asks a question rather than staging a value, because there is nowhere left to stage one: the
  /// parameter's own cell is what the RT loop composes the wire image from, and a writer has
  /// already stored there by the time it asks. So a @c true answer means "your value will go out on
  /// the next cycle"; @c false means the caller must reach the object over SDO instead.
  ///
  /// Lock-free. No health gate — an output is our own setpoint, always valid to send.
  bool isOutputMapped(uint16_t slavePosition, uint16_t index, uint8_t subindex) const;
};

}  // namespace mm::node
