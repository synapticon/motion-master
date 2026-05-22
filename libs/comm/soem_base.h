#pragma once

#include <soem/soem.h>

#include <cstdint>
#include <string>

namespace mm::comm::soem {

/// @brief Owns an SOEM EtherCAT master context together with all its required
///        storage buffers.
///
/// SOEM's @c ecx_contextt holds raw pointers into several large fixed-size
/// buffers (slave list, group list, ESI buffer, etc.).  Bundling context and
/// buffers in a single struct keeps those pointers valid for the full lifetime
/// of the object and avoids scattered heap allocations.
///
/// Because the struct is large (several hundred kilobytes at the default
/// EC_MAXSLAVE of 200), it **must** be heap-allocated — use
/// @c std::make_unique<SoemMaster>() and never place it on the stack.
///
/// After construction, initialise by calling @c ecx_init() with
/// @c &master.ctx and @c master.iface.c_str().  All subsequent SOEM API calls
/// operate through @c ctx.
struct SoemMaster {
  ecx_contextt ctx;   ///< SOEM master context; all SOEM API calls use this handle.
  std::string iface;  ///< OS network interface name (e.g. @c "eth0", @c "enp3s0").
  uint8 group;        ///< EtherCAT group index used for PDO exchange.
  int roundtripTime;  ///< Last measured PDO round-trip time in microseconds.

  /// @name SOEM storage — pointed to by @c ctx; all members must outlive @c ctx.
  /// @{
  uint8 map[4096];                   ///< PDO I/O map buffer; @c ctx.iomap points here.
  ecx_portt port;                    ///< SOEM low-level port and socket state.
  ec_slavet slavelist[EC_MAXSLAVE];  ///< Per-slave config and state; indexed 1..slavecount.
  int slavecount;                    ///< Number of EtherCAT slaves found during network init.
  ec_groupt grouplist[EC_MAXGROUP];  ///< Per-group PDO configuration.
  uint8 esibuf[EC_MAXEEPBUF];        ///< ESI (EEPROM) read buffer.
  uint32 esimap[EC_MAXEEPBITMAP];    ///< ESI bitmap; tracks which EEPROM words have been read.
  ec_eringt elist;                   ///< EtherCAT error ring buffer.
  ec_idxstackT idxstack;             ///< Frame index stack for tracking outstanding frames.
  boolean ecaterror;                 ///< Set to @c TRUE when an EtherCAT error is detected.
  int64 DCtime;                      ///< Distributed Clock reference time in nanoseconds.
  ec_SMcommtypet SMcommtype[EC_MAX_MAPT];  ///< Sync Manager comm-type scratch buffer (PDO mapping).
  ec_PDOassignt PDOassign[EC_MAX_MAPT];    ///< PDO assignment scratch buffer (PDO mapping).
  ec_PDOdesct PDOdesc[EC_MAX_MAPT];        ///< PDO descriptor scratch buffer (PDO mapping).
  ec_eepromSMt eepSM;                      ///< EEPROM Sync Manager configuration scratch buffer.
  ec_eepromFMMUt eepFMMU;                  ///< EEPROM FMMU configuration scratch buffer.
  /// @}
};

}  // namespace mm::comm::soem
