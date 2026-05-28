#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace mm::comm {

/// @brief Metadata for a single AL Status Code entry.
struct AlStatusCode {
  uint16_t code;                 ///< AL Status Code value (ETG.1000.6 Table 11).
  std::string_view name;         ///< Short human-readable name.
  std::string_view description;  ///< Full description of the error condition.
  bool terminal = false;         ///< True if a slave reporting this code cannot reach the
                                 ///< requested state by retrying — i.e. the master must
                                 ///< change something (re-init, reflash, power cycle).
                                 ///< Used by FieldbusDriver::transitionToState to abandon
                                 ///< the slave immediately instead of waiting for timeout.
};

/// @brief Serialises an AlStatusCode to JSON.
///
/// Produces an object with keys @c code, @c name, @c description, @c terminal.
/// Participates in nlohmann ADL so that @c nlohmann::json(kAlStatusCodes) works automatically.
///
/// @param j  Output JSON value.
/// @param c  AL Status Code entry to serialise.
void to_json(nlohmann::json& j, const AlStatusCode& c);

/// @brief Catalogue of AL Status Codes defined in ETG.1000.6 Table 11.
inline constexpr auto kAlStatusCodes = std::to_array<AlStatusCode>({
    {0x0000, "No error", "No error"},
    {0x0001, "Unspecified error", "Unspecified error"},
    {0x0002, "No Memory", "Slave has insufficient memory to complete the requested operation",
     true},
    {0x0003, "Invalid Device Setup", "The device setup is invalid", true},
    {0x0005, "Reserved", "Reserved due to compatibility reasons"},
    {0x0011, "Invalid requested state change", "The requested state transition is not allowed",
     true},
    {0x0012, "Unknown requested state", "The requested state is not a valid EtherCAT AL state",
     true},
    {0x0013, "Bootstrap not supported", "The slave does not support the Bootstrap state", true},
    {0x0014, "No valid firmware", "No valid firmware is present — the slave needs firmware flashed",
     true},
    {0x0015, "Invalid mailbox config (BOOT)", "Mailbox configuration is invalid for BOOT state",
     true},
    {0x0016, "Invalid mailbox config (PRE-OP)", "Mailbox configuration is invalid for PRE-OP state",
     true},
    {0x0017, "Invalid sync manager config", "Sync manager configuration is invalid", true},
    {0x0018, "No valid inputs", "No valid input data is available"},
    {0x0019, "No valid outputs", "No valid output data is available"},
    {0x001A, "Synchronization error", "A synchronization error has occurred"},
    {0x001B, "Sync manager watchdog", "The sync manager watchdog has expired"},
    {0x001C, "Invalid SM types", "Invalid sync manager types", true},
    {0x001D, "Invalid output config", "Invalid output configuration", true},
    {0x001E, "Invalid input config", "Invalid input configuration", true},
    {0x001F, "Invalid watchdog config", "Invalid watchdog configuration", true},
    {0x0020, "Needs cold start", "The slave requires a cold start (power cycle)", true},
    {0x0021, "Needs INIT", "The slave must be in INIT state before this transition", true},
    {0x0022, "Needs PRE-OP", "The slave must be in PRE-OP state before this transition", true},
    {0x0023, "Needs SAFE-OP", "The slave must be in SAFE-OP state before this transition", true},
    {0x0024, "Invalid input mapping", "The FMMU input mapping is invalid", true},
    {0x0025, "Invalid output mapping", "The FMMU output mapping is invalid", true},
    {0x0026, "Inconsistent settings", "Application and mailbox settings are inconsistent", true},
    {0x0027, "FreeRun not supported", "FreeRun mode is not supported by this slave", true},
    {0x0028, "SyncMode not supported", "Synchronous mode is not supported by this slave", true},
    {0x0029, "FreeRun needs 3buffer mode", "FreeRun mode requires 3-buffer SyncManager mode", true},
    {0x002A, "Background watchdog", "Background watchdog has expired"},
    {0x002B, "No valid I/O data", "No valid input and output data are available"},
    {0x002C, "Fatal sync error", "A fatal synchronization error has occurred"},
    {0x002D, "No Sync Error", "No synchronization error"},
    {0x0030, "Invalid DC SYNC config", "Invalid Distributed Clocks SYNC configuration", true},
    {0x0031, "Invalid DC latch config", "Invalid Distributed Clocks latch configuration", true},
    {0x0032, "PLL error", "Distributed Clocks PLL error"},
    {0x0033, "DC sync IO error", "Distributed Clocks sync IO error"},
    {0x0034, "DC sync timeout", "Distributed Clocks sync timeout error"},
    {0x0035, "Invalid DC sync cycle time", "Invalid Distributed Clocks sync cycle time", true},
    {0x0036, "DC sync0 cycle time", "Invalid SYNC0 cycle time", true},
    {0x0037, "DC sync1 cycle time", "Invalid SYNC1 cycle time", true},
    {0x0041, "MBX_AoE error", "ADS over EtherCAT mailbox error"},
    {0x0042, "MBX_EoE error", "Ethernet over EtherCAT mailbox error"},
    {0x0043, "MBX_CoE error", "CANopen over EtherCAT mailbox error"},
    {0x0044, "MBX_FoE error", "File access over EtherCAT mailbox error"},
    {0x0045, "MBX_SoE error", "Servo Drive Profile over EtherCAT mailbox error"},
    {0x004F, "MBX_VoE error", "Vendor specific over EtherCAT mailbox error"},
    {0x0050, "EEPROM no access", "The EEPROM cannot be accessed", true},
    {0x0051, "EEPROM Error", "An EEPROM error has occurred", true},
    {0x0060, "Slave restarted locally", "The slave has restarted locally"},
    {0x0061, "Device Identification value updated", "Device identification value has been updated"},
    {0x00F0, "Application controller available", "An application controller is available"},
});

/// @brief Returns true if @p code indicates a state-transition failure that cannot
///        be resolved by retrying the same writestate.
///
/// A terminal code means the polling loop in @ref FieldbusDriver::transitionToState
/// should abandon the slave immediately rather than waiting for the timeout — the
/// master must change something (re-init, reflash, power cycle) before another
/// transition attempt can succeed. Unknown codes are treated as non-terminal.
constexpr bool isAlStatusCodeTerminal(uint16_t code) {
  const auto it = std::find_if(kAlStatusCodes.begin(), kAlStatusCodes.end(),
                               [code](const AlStatusCode& entry) { return entry.code == code; });
  return it != kAlStatusCodes.end() && it->terminal;
}

}  // namespace mm::comm
