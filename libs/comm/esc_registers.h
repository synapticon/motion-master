#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace mm::comm {

/// @brief Metadata for a single ESC register entry.
struct EscRegister {
  uint16_t address;       ///< Register address in the ESC address space.
  uint8_t length;         ///< Width in bytes (1, 2, 4, or 8).
  std::string_view name;  ///< Short snake_case identifier.
  std::string_view
      description;  ///< Human-readable description from ETG.1000.4 / Beckhoff ESC datasheet.
};

/// @brief Serialises an EscRegister to JSON.
///
/// Produces an object with keys @c address, @c length, @c name, @c description.
/// Participates in nlohmann ADL so that @c nlohmann::json(kEscRegisters) works automatically.
///
/// @param j  Output JSON value.
/// @param r  Register entry to serialise.
void to_json(nlohmann::json& j, const EscRegister& r);

/// @brief Catalogue of well-known ESC registers.
///
/// Addresses and sizes follow the Beckhoff EtherCAT ESC datasheet (Section II)
/// and ETG.1000.4.  FMMU and SyncManager entries list the full block for the
/// first instance; subsequent instances follow at fixed strides (FMMU: 16 bytes,
/// SM: 8 bytes).
inline constexpr auto kEscRegisters = std::to_array<EscRegister>({
    // ── ESC Information ─────────────────────────────────────────────────────
    {0x0000, 1, "type", "ESC type identifier"},
    {0x0001, 1, "revision", "ESC revision number"},
    {0x0002, 2, "build", "ESC build number"},
    {0x0004, 1, "fmmu_supported", "Number of supported FMMUs"},
    {0x0005, 1, "sync_manager_count", "Number of supported SyncManagers"},
    {0x0006, 1, "ram_size", "Internal process data RAM size in kB"},
    {0x0007, 1, "port_descriptor",
     "Port type per port (2 bits each: 0=not impl, 1=not configured, 2=EBUS, 3=MII/RMII)"},
    {0x0008, 2, "esc_features", "Supported feature flags (DC, FMMU bit ops, DC sync, etc.)"},

    // ── Station Addressing ──────────────────────────────────────────────────
    {0x0010, 2, "configured_station_address",
     "Node address used for FPRD/FPWR commands (set by master)"},
    {0x0012, 2, "configured_station_alias",
     "Alias address; loaded from EEPROM word 0x0004 at reset"},

    // ── Data Link Layer ─────────────────────────────────────────────────────
    {0x0100, 4, "dl_control", "DL control: loop port control, RX FIFO size, station alias enable"},
    {0x0108, 2, "physical_rw_offset", "Offset between read and write addresses for R/W commands"},
    {0x0110, 2, "dl_status",
     "DL status: EEPROM load ok, link detected, communication established per port"},

    // ── Application Layer ───────────────────────────────────────────────────
    {0x0120, 2, "al_control",
     "AL control: state request (1=Init, 2=Pre-Op, 4=Safe-Op, 8=Op) + error ack"},
    {0x0130, 2, "al_status", "AL status: current EtherCAT state + error indicator"},
    {0x0134, 2, "al_status_code", "AL status error code; non-zero when a state transition fails"},

    // ── PDI ─────────────────────────────────────────────────────────────────
    {0x0140, 1, "pdi_control",
     "Process data interface type (0=EEPROM, 4=SPI, 5=EtherCAT Network Controller, 8=on-chip bus)"},
    {0x0141, 1, "esc_configuration",
     "ESC configuration: AL status auto-update, enhanced link detect, DC sync/latch"},

    // ── Interrupt / Event ───────────────────────────────────────────────────
    {0x0200, 2, "ecat_event_mask", "Mask for ECAT-side IRQ events"},
    {0x0204, 4, "al_event_mask", "Mask for PDI-side (AL) IRQ events"},
    {0x0210, 2, "ecat_event_request",
     "Pending ECAT-side events (DC latch, DL/AL status change, SM events)"},
    {0x0220, 4, "al_event_request",
     "Pending PDI-side events (state change, DC, SyncManager, EEPROM, watchdog)"},

    // ── Error Counters ──────────────────────────────────────────────────────
    {0x0300, 8, "rx_error_counter",
     "Invalid frame and RX error counts for ports 0–3 (1 byte each, pairs per port)"},
    {0x0308, 4, "previous_error_counter", "Previous error counts for ports 0–3 (1 byte each)"},
    {0x030C, 1, "malformat_frame_counter", "Counts frames with wrong EtherCAT datagram structure"},
    {0x030D, 1, "local_problem_counter", "Counts communication problems local to the slave"},
    {0x0310, 4, "lost_link_counter", "Lost link counts for ports 0–3 (1 byte per port)"},

    // ── Watchdog ────────────────────────────────────────────────────────────
    {0x0400, 2, "watchdog_divider",
     "Watchdog clock prescaler (basic tick = 40 ns × (divider + 2))"},
    {0x0410, 2, "watchdog_time_pdi", "PDI watchdog timeout in watchdog clock ticks"},
    {0x0420, 2, "watchdog_time_process_data",
     "Process data (SM) watchdog timeout in watchdog clock ticks"},
    {0x0440, 2, "watchdog_status_process_data",
     "Process data watchdog status (bit 0: 0=expired, 1=running)"},
    {0x0442, 1, "watchdog_counter_process_data", "Number of process data watchdog expirations"},
    {0x0443, 1, "watchdog_counter_pdi", "Number of PDI watchdog expirations"},

    // ── EEPROM ──────────────────────────────────────────────────────────────
    {0x0500, 1, "eeprom_configuration", "EEPROM: PDI access enable, force ECAT access"},
    {0x0502, 2, "eeprom_control_status",
     "EEPROM command register, write-enable, busy/error status"},
    {0x0504, 4, "eeprom_address", "EEPROM word address for read/write operations"},
    {0x0508, 8, "eeprom_data", "EEPROM data buffer (64-bit; 32-bit for older ESCs)"},

    // ── FMMU 0 (stride 16 bytes for FMMU 1–7) ──────────────────────────────
    {0x0600, 16, "fmmu0",
     "FMMU 0: logical addr (4), length (2), start bit (1), stop bit (1), physical addr (2), "
     "physical start bit (1), type (1), activate (1), reserved (3)"},

    // ── SyncManager 0 (stride 8 bytes for SM 1–7) ──────────────────────────
    {0x0800, 8, "sm0",
     "SyncManager 0: physical addr (2), length (2), control (1), status (1), activate (1), PDI "
     "control (1)"},

    // ── Distributed Clocks ──────────────────────────────────────────────────
    {0x0900, 4, "dc_receive_time_port0",
     "DC: local time at port 0 when the last frame arrived (ns, 32-bit)"},
    {0x0904, 4, "dc_receive_time_port1",
     "DC: local time at port 1 when the last frame arrived (ns, 32-bit)"},
    {0x0908, 4, "dc_receive_time_port2",
     "DC: local time at port 2 when the last frame arrived (ns, 32-bit)"},
    {0x090C, 4, "dc_receive_time_port3",
     "DC: local time at port 3 when the last frame arrived (ns, 32-bit)"},
    {0x0910, 8, "dc_system_time",
     "DC: local system time copy latched at start-of-frame (ns, 64-bit)"},
    {0x0918, 8, "dc_receive_time_ecat_unit",
     "DC: local time at the processing unit when the last frame arrived (ns, 64-bit)"},
    {0x0920, 8, "dc_system_time_offset",
     "DC: offset added to local time to derive system time (ns, 64-bit)"},
    {0x0928, 4, "dc_system_time_delay",
     "DC: measured propagation delay from reference clock to this ESC (ns)"},
    {0x092C, 4, "dc_system_time_difference",
     "DC: filtered difference between local and received system time (ns)"},
    {0x0930, 2, "dc_speed_counter_start",
     "DC: speed counter start value for clock drift compensation"},
    {0x0932, 2, "dc_speed_counter_diff", "DC: current clock period deviation"},
    {0x0980, 1, "dc_cyclic_unit_control", "DC: selects SYNC/LATCH control source (ECAT or PDI)"},
    {0x0981, 1, "dc_activation", "DC: activate SYNC out unit and pulse generation"},
    {0x0982, 2, "dc_sync_pulse_length", "DC: SYNC0/SYNC1 pulse duration in units of 10 ns"},
    {0x0990, 8, "dc_start_time_cyclic",
     "DC: system time for first SYNC0 pulse (written by master)"},
    {0x0998, 8, "dc_next_sync1_pulse", "DC: system time of the next SYNC1 pulse"},
    {0x09A0, 4, "dc_sync0_cycle_time", "DC: SYNC0 cycle period in ns (0 = single-shot)"},
    {0x09A4, 4, "dc_sync1_cycle_time", "DC: SYNC1 offset from SYNC0 in ns (0 = same as SYNC0)"},
    {0x09A8, 1, "dc_latch0_control",
     "DC: Latch0 mode (0=continuous, 1=single event on positive edge, 2=negative)"},
    {0x09A9, 1, "dc_latch1_control", "DC: Latch1 mode"},
    {0x09AE, 1, "dc_latch0_status", "DC: Latch0 edge detection and current pin state"},
    {0x09AF, 1, "dc_latch1_status", "DC: Latch1 edge detection and current pin state"},
    {0x09B0, 8, "dc_latch0_time_pos",
     "DC: system time captured at Latch0 positive edge (ns, 64-bit)"},
    {0x09B8, 8, "dc_latch0_time_neg",
     "DC: system time captured at Latch0 negative edge (ns, 64-bit)"},
    {0x09C0, 8, "dc_latch1_time_pos",
     "DC: system time captured at Latch1 positive edge (ns, 64-bit)"},
    {0x09C8, 8, "dc_latch1_time_neg",
     "DC: system time captured at Latch1 negative edge (ns, 64-bit)"},
});

}  // namespace mm::comm
