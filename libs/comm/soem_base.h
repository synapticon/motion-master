#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <soem/soem.h>

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
  uint8 map[4096];                       ///< PDO I/O map buffer; @c ctx.iomap points here.
  ecx_portt port;                        ///< SOEM low-level port and socket state.
  ec_slavet slavelist[EC_MAXSLAVE];      ///< Per-slave config and state; indexed 1..slavecount.
  int slavecount;                        ///< Number of EtherCAT slaves found during network init.
  ec_groupt grouplist[EC_MAXGROUP];      ///< Per-group PDO configuration.
  uint8 esibuf[EC_MAXEEPBUF];            ///< ESI (EEPROM) read buffer.
  uint32 esimap[EC_MAXEEPBITMAP];        ///< ESI bitmap; tracks which EEPROM words have been read.
  ec_eringt elist;                       ///< EtherCAT error ring buffer.
  ec_idxstackT idxstack;                 ///< Frame index stack for tracking outstanding frames.
  boolean ecaterror;                     ///< Set to @c TRUE when an EtherCAT error is detected.
  int64 DCtime;                          ///< Distributed Clock reference time in nanoseconds.
  ec_SMcommtypet SMcommtype[EC_MAX_MAPT]; ///< Sync Manager comm-type scratch buffer (PDO mapping).
  ec_PDOassignt PDOassign[EC_MAX_MAPT];  ///< PDO assignment scratch buffer (PDO mapping).
  ec_PDOdesct PDOdesc[EC_MAX_MAPT];      ///< PDO descriptor scratch buffer (PDO mapping).
  ec_eepromSMt eepSM;                    ///< EEPROM Sync Manager configuration scratch buffer.
  ec_eepromFMMUt eepFMMU;                ///< EEPROM FMMU configuration scratch buffer.
  /// @}
};

/// @brief Enumerates all network adapters and returns a map from MAC address
///        to OS interface name.
///
/// Scans every network adapter visible to the OS and builds a lookup table
/// that lets callers select a specific NIC by its hardware address rather than
/// by its OS-assigned name, which can change across reboots or driver updates.
///
/// **Platform behaviour**
/// - **Linux** — iterates the adapter list returned by SOEM's
///   @c ec_find_adapters() and queries each interface with @c SIOCGIFHWADDR
///   via a temporary @c SOCK_DGRAM socket.  Adapters for which the @c ioctl
///   call fails are silently skipped.
/// - **Windows** — calls @c GetAdaptersAddresses() (IPHLPAPI) to enumerate
///   adapters.  Interface names are formatted as WinPcap/Npcap NPF paths:
///   @c "\\Device\\NPF_<AdapterName>".
///
/// **MAC address format** — keys are uppercase colon-separated hex octets,
/// e.g. @c "AA:BB:CC:DD:EE:FF", consistent across platforms.
///
/// @return A @c std::map<std::string, std::string> where:
///   - **key**   = MAC address string, e.g. @c "00:1A:2B:3C:4D:5E"
///   - **value** = OS interface name, e.g. @c "eth0" (Linux) or
///                 @c "\\Device\\NPF_{GUID}" (Windows)
///
/// Returns an empty map if adapter enumeration fails (e.g. insufficient
/// privileges or no adapters present).
///
/// @note Call once at startup to resolve a configured MAC address to the
///       interface name required by @c SoemFieldbusDriver.  Do not call in a
///       tight loop — OS adapter enumeration is slow.
std::map<std::string, std::string> mapMacAddressesToInterfaces();

}  // namespace mm::comm::soem
