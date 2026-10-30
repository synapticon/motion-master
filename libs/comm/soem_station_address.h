#pragma once

#include <soem/soem.h>

#include <cstdint>

namespace mm::comm::soem {

/// @brief Outcome of a check of a slave's station address.
///
/// It decides what the caller does next: only a slave that had to be re-addressed also lost the
/// sync managers the master programmed into it.
enum class AddressState {
  Answering,    ///< Reachable at its configured address; nothing was changed.
  Reassigned,   ///< It had lost the address, and now answers again.
  Unreachable,  ///< It answers at neither address.
};

/// @brief Gives a slave that restarted its station address back.
///
/// A restart clears the configured station address (0x0010) along with everything else the master
/// programmed, and every FPRD and FPWR of the master is addressed by that value. A SOMANET
/// bootloader restarts the device on the step from BOOT to PRE-OP. A Jasper-based drive restarts
/// on the step from BOOT to INIT after a commit, and on the step from INIT to PRE-OP after a BOOT
/// session with no commit.
///
/// Recovery needs no scan. Auto-increment addressing reaches a slave by its position on the wire
/// with no configuration, which is how @c ecx_config_init assigns the address in the first place.
/// So one APWR puts it back, for this one device, without touching the rest of the bus. The
/// position is taken from the slavelist, so a chain that changed since the scan addresses a
/// different device.
///
/// Costs one register read when the slave answers at its address. In @c SoemFieldbusDriver, call it
/// with the bus lock held exclusive.
AddressState restoreStationAddress(ecx_contextt* ctx, uint16_t slave);

}  // namespace mm::comm::soem
