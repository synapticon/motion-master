#include "comm/esc_diagnostics.h"

#include <format>
#include <string>

namespace mm::comm {

namespace {

// The ESC clock that feeds the watchdog divider (ETG.1000.4). One tick is 40 ns × (divider + 2).
constexpr int64_t kEscClockNs = 40;

int64_t watchdogTickNs(uint16_t divider) {
  return kEscClockNs * (static_cast<int64_t>(divider) + 2);
}

uint32_t readLe32(std::span<const uint8_t> bytes) {
  return bytes[0] | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace

SlaveDiagnostics decodeSlaveDiagnostics(
    uint16_t slavePosition, uint16_t dlStatus,
    std::span<const uint8_t, kErrorCounterBytes> errorCounters,
    std::span<const uint8_t, kWatchdogCounterBytes> watchdogCounters) {
  // DL Status (0x0110): bits 4–7 are the link on ports 0–3. For port p, bit (8 + 2p) is loop
  // closed and bit (9 + 2p) is communication established.
  //
  // Error counters (0x0300–0x0313):
  //   [0..7]   RX error counter, per port: byte 2p invalid frame, byte 2p+1 RX error
  //   [8..11]  forwarded RX error, 1 byte per port
  //   [12]     processing-unit error (0x030C)   [13] PDI error (0x030D)   [14,15] reserved
  //   [16..19] lost-link counter, 1 byte per port
  //
  // Watchdog counters (0x0440–0x0443): [0,1] PD watchdog status, [2] PD watchdog expirations
  // (0x0442), [3] PDI watchdog expirations (0x0443).
  SlaveDiagnostics d{};
  d.slavePosition = slavePosition;
  for (std::size_t p = 0; p < d.ports.size(); ++p) {
    d.ports[p] = PortDiagnostics{
        .linkUp = (dlStatus & (1u << (4 + p))) != 0,
        .loopClosed = (dlStatus & (1u << (8 + 2 * p))) != 0,
        .communication = (dlStatus & (1u << (9 + 2 * p))) != 0,
        .invalidFrame = errorCounters[2 * p],
        .rxError = errorCounters[2 * p + 1],
        .forwardedError = errorCounters[8 + p],
        .lostLink = errorCounters[16 + p],
    };
  }
  d.processingUnitError = errorCounters[12];
  d.pdiError = errorCounters[13];
  d.processDataWatchdog = watchdogCounters[2];
  d.pdiWatchdog = watchdogCounters[3];
  return d;
}

DcSyncDiagnostics decodeDcSync(uint16_t slavePosition, bool referenceClock,
                               std::span<const uint8_t, kDcSyncBytes> registers) {
  const uint32_t delay = readLe32(registers.subspan(0, 4));
  const uint32_t differenceRaw = readLe32(registers.subspan(4, 4));
  // System-time difference (0x092C): bits 0–30 are the mean deviation in ns. Bit 31 is set when
  // the local copy of the system time is smaller than the reference, so the local clock is behind.
  // The result is positive when the local clock is ahead and negative when it is behind.
  const auto magnitude = static_cast<int32_t>(differenceRaw & 0x7FFFFFFFu);
  return DcSyncDiagnostics{
      .slavePosition = slavePosition,
      .dcCapable = true,
      .referenceClock = referenceClock,
      .propagationDelay = static_cast<int32_t>(delay),
      .systemTimeDifference = (differenceRaw & 0x80000000u) != 0 ? -magnitude : magnitude,
  };
}

ProcessDataWatchdogConfig decodeWatchdog(uint16_t divider, uint16_t ticks, uint16_t status) {
  return {.enabled = ticks != 0,
          .running = (status & 0x0001u) != 0,
          .timeout = std::chrono::nanoseconds(watchdogTickNs(divider) * ticks),
          .divider = divider,
          .ticks = ticks};
}

std::expected<uint16_t, std::string> watchdogTicks(uint16_t divider,
                                                   std::chrono::nanoseconds timeout) {
  if (timeout < std::chrono::nanoseconds::zero()) {
    return std::unexpected("watchdog timeout must not be negative");
  }
  const int64_t tickNs = watchdogTickNs(divider);
  int64_t ticks = (timeout.count() + tickNs / 2) / tickNs;
  if (ticks > 0xFFFF) {
    return std::unexpected(std::format(
        "watchdog timeout {} ns exceeds the maximum {} ns representable with this device's divider "
        "({} ns per tick)",
        timeout.count(), tickNs * 0xFFFF, tickNs));
  }
  if (timeout.count() > 0 && ticks == 0) {
    ticks = 1;
  }
  return static_cast<uint16_t>(ticks);
}

}  // namespace mm::comm
