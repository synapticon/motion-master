#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "core/seqlock.h"
#include "node/process_image.h"

namespace mm::node {

/// @brief The live process-data runtime: the published image, the cross-thread exchange buffers,
///        and per-object PDO access over them.
///
/// Owns the seqlock buffers that carry the output (master→slave) and input (slave→master) images
/// across the RT / non-RT boundary, the pointer to the currently published @c ProcessImage layout,
/// and the working-counter health. @c DeviceManager owns one of these and drives the whole-buffer
/// exchange each cycle; it also hands a pointer to it to every @c Device so a device can read and
/// write its own PDO-mapped objects (@c readPdo / @c writePdo) without knowing how the image is
/// stored. That single seam is what lets @c Device::readParameter / @c Device::writeParameter serve
/// the live IO-map value while exchanging — identically from an HTTP handler or an RT task — and
/// fall back to SDO otherwise.
///
/// The object accessors are lock-free on the read side (the RT writer is wait-free via the
/// seqlock); @c writePdo takes @c stagingMutex only to serialise non-RT writers among themselves.
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
  // Cross-thread exchange buffers. outputStaging: non-RT writers stage setpoints, the RT loop
  // loads and sends them. inputSnapshot: the RT loop publishes captured inputs, readers load them.
  mm::core::SeqLock<ProcessBuffer> outputStaging;
  mm::core::SeqLock<ProcessBuffer> inputSnapshot;
  // RT-thread scratch for the exchange (touched only on the RT thread).
  ProcessBuffer outScratch;
  ProcessBuffer inScratch;
  // Serialises non-RT writers doing read-modify-write on outputStaging (the RT reader is
  // wait-free via the seqlock and does not take this lock).
  std::mutex stagingMutex;
  // Set by the RT thread while it is inside a driver exchange; lets stopExchange() drain an
  // in-flight cycle before a re-map or teardown mutates the IOmap.
  std::atomic<bool> exchanging{false};
  // Working-counter health. lastWkc is written by the RT thread each cycle; expectedWkc is
  // recomputed by the control plane from current device states. healthy = last >= expected.
  std::atomic<int> lastWkc{0};
  std::atomic<int> expectedWkc{0};

  /// @brief True when an image is published and the last working counter meets the expectation.
  ///
  /// A read off a published-but-unhealthy bus would serve stale snapshot bytes (a short working
  /// counter means the driver left the prior cycle's bytes in the IOmap), so @c readPdo gates on
  /// this and the caller falls back to the authoritative SDO upload.
  bool healthy() const {
    return image.load(std::memory_order_acquire) != nullptr &&
           lastWkc.load(std::memory_order_relaxed) >= expectedWkc.load(std::memory_order_relaxed);
  }

  /// @brief Reads a PDO-mapped object's current bytes from the live image.
  ///
  /// Outputs come from the staging buffer (what is being sent), inputs from the latest snapshot.
  /// Returns @c nullopt when no image is published, the bus is unhealthy, or the object is not
  /// PDO-mapped — in each case the caller reads it over SDO instead. Bytes are little-endian and
  /// LSB-aligned. Lock-free.
  std::optional<std::vector<uint8_t>> readPdo(uint16_t slavePosition, uint16_t index,
                                              uint8_t subindex) const;

  /// @brief Stages a PDO output object's bytes into the output image, sent on the next cycle.
  ///
  /// @return @c true if the object is output-mapped and was staged; @c false otherwise (no image,
  ///         or the object is an input / not mapped — the caller then writes it over SDO). No
  ///         health gate: staging is always safe, the value is simply sent next cycle.
  bool writePdo(uint16_t slavePosition, uint16_t index, uint8_t subindex,
                std::span<const uint8_t> bytes);
};

}  // namespace mm::node
