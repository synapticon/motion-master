#pragma once

#include <soem/soem.h>

#include <cstdint>
#include <vector>

#include "comm/fieldbus_driver.h"

namespace mm::comm::soem {

/// @brief Discards whatever is sitting in a slave's read mailbox, ignoring the contents.
///
/// A transfer that failed can leave its late reply in the mailbox. The next request would then
/// read that reply as its own answer and fail. Reading the mailbox out with a zero timeout before
/// each transfer prevents that. The cost is one FPRD datagram: with a zero timeout,
/// @c ecx_mbxreceive checks the sync manager status once and returns without touching the mailbox
/// when nothing is waiting.
///
/// Draining removes a late reply. It does not end the transfer on the slave. A SOMANET bootloader
/// that is still in the middle of an FoE read rejects every new request until the transfer ends,
/// and @c abortFoeTransfer is what ends it.
///
/// Ownership: @c ecx_mbxreceive allocates from the context's mailbox pool and transfers the buffer
/// to the caller on success, so each drained message goes back to the pool. This holds because
/// Motion Master never enables the cyclic mailbox handler (@c ecx_slavembxcyclic), whose path
/// assigns a buffer outside the pool. The count is bounded so that a slave which keeps sending
/// cannot hold the caller here.
///
/// Call with the driver's control-plane mutex held.
void drainMailbox(ecx_contextt* ctx, uint16_t slavePosition);

/// @brief Logs the mailbox writes to a slave that got no frame back, and resets their count.
///
/// The SOEM port writes each mailbox message to a SOMANET slave once and counts the writes without
/// a frame in @c mbxwriteunknowns. Such a write is not repeated, because the slave may have
/// received it, and the caller waits for the answer. A count above 0 in a support log shows that
/// frames come back late or not at all. The count is per slave and only grows, so this logs each
/// write once.
///
/// Call with the driver's lock for the slave held.
void reportUnknownMailboxWrites(ecx_contextt* ctx, uint16_t slavePosition);

/// @brief Builds an FoE ERROR request, which aborts a slave's FoE transfer.
///
/// The request is a complete mailbox message: the 6-byte mailbox header, the FoE opcode ERROR, a
/// reserved byte, the error code 0x8000 ("not defined") and a short error text. All fields are
/// little-endian.
///
/// @param counter  Mailbox counter (1 to 7) for the mailbox header. Only the low three bits are
///                 used.
/// @return The bytes of the request.
std::vector<uint8_t> makeFoeAbortRequest(uint8_t counter);

/// @brief Ends the FoE transfer a slave may still have in progress.
///
/// SOEM gives up on an FoE transfer without telling the slave. The FoE layer of a SOMANET
/// bootloader (@c lib_ethercat @c foe.xc) then stays in the state of that transfer. In the READ
/// state it answers every new READ and WRITE request with mailbox error 0x0008, and only an FoE
/// ERROR request from the master or a power cycle ends that state. AL state transitions do not.
///
/// Sends the request, waits up to 100 ms for the slave's answer, and discards the answer and the
/// errors SOEM queued for it. The answer depends on the state the request meets, and none of the
/// answers is of use: a slave in the READ state answers with mailbox error 0x0008, and an idle
/// slave answers with an FoE error. Take the reason for the failed transfer off SOEM's error list
/// before the call, because the call empties it.
///
/// Call with the driver's control-plane mutex held.
void abortFoeTransfer(ecx_contextt* ctx, uint16_t slavePosition);

/// @brief Programs the mailbox sync managers of a slave for @p targetState.
///
/// BOOT uses the bootstrap mailbox from the SII, which is larger than the standard one, and
/// PRE-OP uses the standard mailbox. Call before the transition out of INIT. Any other target
/// state leaves the sync managers unchanged.
///
/// Call with the driver's control-plane mutex held.
void updateMailboxSyncManagers(ecx_contextt* ctx, uint16_t slave, EtherCatState targetState);

}  // namespace mm::comm::soem
