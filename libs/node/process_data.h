#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/seqlock.h"
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
/// Both accessors are lock-free, including writes (so an RT task can stage a setpoint without ever
/// blocking on a non-RT writer). The output path is single-writer-per-buffer by construction: each
/// output object owns an atomic staging slot that any number of writers store into independently,
/// and the RT loop is the sole thread that composes those slots into the packed wire image each
/// cycle (@c DeviceManager::exchangeProcessData). Composing on one thread is what makes bit-packed
/// objects that share a byte safe without a lock — see NEXTGEN.md (Design B).
///
/// Defined in a header (rather than pimpl'd) so both @c device.cc and @c device_manager.cc can use
/// it; the @c SeqLock members and fixed buffers are heavy, but only those two translation units
/// pull them in (the public @c device.h / @c device_manager.h only forward-declare it).
struct ProcessData {
  // The currently published image layout, or nullptr when no image is published (exchange is then
  // a no-op). Loaded lock-free by readers; published with release ordering by the control plane.
  std::atomic<const ProcessImage*> image{nullptr};
  // Every image ever published since the last reset(), retained so a lock-free reader can never
  // dereference a freed image after a re-map republishes a new one.
  std::vector<std::shared_ptr<const ProcessImage>> generations;
  // Per-output-object staging slots, one per entry in image->outputs (rebuilt and seeded at each
  // re-map). A writer stores its object's latest wire bytes (≤8, packed little-endian into the u64)
  // here lock-free — writers to different objects never contend; the same object is last-writer-
  // wins. The RT loop reads every slot each cycle and composes the output image. Replaces the old
  // shared output-staging seqlock + mutex (a single packed buffer that every writer had to lock).
  std::vector<std::atomic<uint64_t>> outputSlots;
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "output staging slots must be lock-free so the RT path never blocks");
  // The output image the RT loop last composed and sent, published for non-RT readers (monitoring).
  // Single-writer (RT) like inputSnapshot, hence lock-free; outputSlots is the write target, this
  // is the read-back of what actually went on the wire.
  mm::core::SeqLock<ProcessBuffer> outputSnapshot;
  // The latest received input image: the RT loop publishes it, non-RT readers load it.
  mm::core::SeqLock<ProcessBuffer> inputSnapshot;
  // RT-thread scratch for the exchange (touched only on the RT thread).
  ProcessBuffer outScratch;
  ProcessBuffer inScratch;
  // Set by the RT thread while it is inside a driver exchange; lets stopExchange() drain an
  // in-flight cycle before a re-map or teardown mutates the IOmap.
  std::atomic<bool> exchanging{false};
  // Working-counter health. lastWkc is written by the RT thread each cycle; expectedWkc is
  // recomputed by the control plane from current device states. healthy = last >= expected.
  std::atomic<int> lastWkc{0};
  std::atomic<int> expectedWkc{0};

  /// @brief True when an image is published and the last working counter meets the expectation.
  ///
  /// A read of an *input* off a published-but-unhealthy bus would serve stale snapshot bytes (a
  /// short working counter means the driver left the prior cycle's bytes in the IOmap), so the
  /// input branch of @c readPdo gates on this and the caller falls back to the authoritative SDO
  /// upload. Outputs are not gated — a staging slot is always our own valid setpoint.
  bool healthy() const {
    return image.load(std::memory_order_acquire) != nullptr &&
           lastWkc.load(std::memory_order_relaxed) >= expectedWkc.load(std::memory_order_relaxed);
  }

  /// @brief Reads a PDO-mapped object's current bytes from the live image. Lock-free.
  ///
  /// Outputs come from their staging slot (our current setpoint — always valid); inputs from the
  /// latest received snapshot, gated on @c healthy(). Returns @c nullopt when no image is
  /// published, the object is not PDO-mapped, or (inputs only) the bus is unhealthy — in each case
  /// the caller reads it over SDO instead. Bytes are little-endian and LSB-aligned.
  std::optional<std::vector<uint8_t>> readPdo(uint16_t slavePosition, uint16_t index,
                                              uint8_t subindex) const;

  /// @brief Stages a PDO output object's bytes into its slot, sent on the next cycle. Lock-free.
  ///
  /// @return @c true if the object is output-mapped and was staged; @c false otherwise (no image,
  ///         or the object is an input / not mapped — the caller then writes it over SDO). No
  ///         health gate: staging is always safe, the value is simply sent next cycle.
  bool writePdo(uint16_t slavePosition, uint16_t index, uint8_t subindex,
                std::span<const uint8_t> bytes);
};

}  // namespace mm::node
