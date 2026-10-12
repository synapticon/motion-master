#pragma once

// Decodes the ESC register blocks behind the bus diagnostics, the DC sync status and the
// process-data watchdog. Every driver with an ESC reads the same registers, each through its own
// transport, and decodes them here, so no two drivers decode them differently.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "comm/fieldbus_driver.h"

namespace mm::comm {

inline constexpr uint16_t kDlStatusRegister = 0x0110;  ///< 2 bytes.
/// The error-counter block, 0x0300 to 0x0313, read as one transfer.
inline constexpr uint16_t kErrorCounterRegisters = 0x0300;
inline constexpr std::size_t kErrorCounterBytes = 20;
/// The watchdog status and expiration counters, 0x0440 to 0x0443, read as one transfer.
inline constexpr uint16_t kWatchdogCounterRegisters = 0x0440;
inline constexpr std::size_t kWatchdogCounterBytes = 4;
/// The system-time delay (0x0928) and system-time difference (0x092C), read as one transfer.
inline constexpr uint16_t kDcSyncRegisters = 0x0928;
inline constexpr std::size_t kDcSyncBytes = 8;

/// The watchdog divider (0x0400). It is the time base of the process-data and the PDI watchdog.
inline constexpr uint16_t kWatchdogDividerRegister = 0x0400;
/// The process-data watchdog time (0x0420), in ticks of the divider. Zero disables the watchdog.
inline constexpr uint16_t kWatchdogTimePdRegister = 0x0420;
/// The process-data watchdog status (0x0440). Bit 0 is 1 while the watchdog runs.
inline constexpr uint16_t kWatchdogStatusPdRegister = 0x0440;

/// @brief Decodes one slave's DL status, error counters and watchdog counters.
SlaveDiagnostics decodeSlaveDiagnostics(
    uint16_t slavePosition, uint16_t dlStatus,
    std::span<const uint8_t, kErrorCounterBytes> errorCounters,
    std::span<const uint8_t, kWatchdogCounterBytes> watchdogCounters);

/// @brief Decodes one DC-capable slave's system-time delay and system-time difference.
DcSyncDiagnostics decodeDcSync(uint16_t slavePosition, bool referenceClock,
                               std::span<const uint8_t, kDcSyncBytes> registers);

/// @brief Decodes the process-data watchdog registers.
ProcessDataWatchdogConfig decodeWatchdog(uint16_t divider, uint16_t ticks, uint16_t status);

/// @brief Returns the watchdog time register value for @p timeout at @p divider.
///
/// Rounds to the nearest tick. A zero timeout gives zero ticks, which disables the watchdog. A
/// timeout above zero gives at least one tick, so a small timeout never disables it. A timeout that
/// needs more ticks than the register holds is an error.
std::expected<uint16_t, std::string> watchdogTicks(uint16_t divider,
                                                   std::chrono::nanoseconds timeout);

}  // namespace mm::comm
