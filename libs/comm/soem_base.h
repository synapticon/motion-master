#pragma once

#include <cstdint>
#include <string>

#include <soem/soem.h>

namespace mm::comm::soem {

/**
 * @brief Structure representing the SOEM EtherCAT master and all its
 * associated state and storage buffers.
 *
 * This structure contains the necessary components to interact with the
 * EtherCAT network, including the context, network interface, slave and group
 * information, and other configurations related to EtherCAT communication.
 *
 * @note The context (`ecx_contextt`) holds pointers into the storage members
 * of this struct (slavelist, grouplist, esibuf, etc.) — they must remain
 * valid for the lifetime of the master.  Heap-allocate this struct via
 * `std::unique_ptr` to avoid stack overflow; the struct is large.
 *
 * The `iface` is the name of the network interface used for EtherCAT
 * communication.  Other members store the network configuration, error
 * states, and EtherCAT protocol-specific data.
 */
struct SoemMaster {
  ecx_contextt ctx;
  std::string iface;
  uint8 group;
  int roundtripTime;

  /* Storage pointed to by ctx */
  uint8 map[4096];
  ecx_portt port;
  ec_slavet slavelist[EC_MAXSLAVE];
  int slavecount;
  ec_groupt grouplist[EC_MAXGROUP];
  uint8 esibuf[EC_MAXEEPBUF];
  uint32 esimap[EC_MAXEEPBITMAP];
  ec_eringt elist;
  ec_idxstackT idxstack;
  boolean ecaterror;
  int64 DCtime;
  ec_SMcommtypet SMcommtype[EC_MAX_MAPT];
  ec_PDOassignt PDOassign[EC_MAX_MAPT];
  ec_PDOdesct PDOdesc[EC_MAX_MAPT];
  ec_eepromSMt eepSM;
  ec_eepromFMMUt eepFMMU;
};

}  // namespace mm::comm::soem
