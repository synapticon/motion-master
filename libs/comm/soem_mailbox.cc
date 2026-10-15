#include "comm/soem_mailbox.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace mm::comm::soem {

void drainMailbox(ecx_contextt* ctx, uint16_t slavePosition) {
  constexpr int kMaxDrained = 8;
  for (int i = 0; i < kMaxDrained; ++i) {
    ec_mbxbuft* mbx = nullptr;
    if (ecx_mbxreceive(ctx, slavePosition, &mbx, 0) <= 0) {
      return;
    }
    spdlog::debug("drained a stale mailbox message from slave {}", slavePosition);
    if (mbx != nullptr) {
      ecx_dropmbx(ctx, mbx);
    }
  }
}

std::vector<uint8_t> makeFoeAbortRequest(uint8_t counter) {
  constexpr uint8_t kFoeOpcodeError = 0x05;
  constexpr uint32_t kFoeErrorNotDefined = 0x8000;
  constexpr std::string_view kErrorText = "Aborted by the master";

  // FoE header: opcode, reserved byte, 32-bit error code. The text follows it.
  const auto foeLength = static_cast<uint16_t>(6 + kErrorText.size());

  std::vector<uint8_t> request;
  request.reserve(6 + foeLength);

  // Mailbox header: length, address, channel and priority, then type and counter.
  request.push_back(static_cast<uint8_t>(foeLength & 0xFF));
  request.push_back(static_cast<uint8_t>(foeLength >> 8));
  request.push_back(0x00);
  request.push_back(0x00);
  request.push_back(0x00);
  request.push_back(static_cast<uint8_t>(ECT_MBXT_FOE | MBX_HDR_SET_CNT(counter & 0x07)));

  request.push_back(kFoeOpcodeError);
  request.push_back(0x00);
  for (int shift = 0; shift < 32; shift += 8) {
    request.push_back(static_cast<uint8_t>((kFoeErrorNotDefined >> shift) & 0xFF));
  }
  request.insert(request.end(), kErrorText.begin(), kErrorText.end());
  return request;
}

void abortFoeTransfer(ecx_contextt* ctx, uint16_t slavePosition) {
  constexpr int kReplyTimeoutUs = 100'000;

  const uint8_t counter = ec_nextmbxcnt(ctx->slavelist[slavePosition].mbx_cnt);
  ctx->slavelist[slavePosition].mbx_cnt = counter;
  const std::vector<uint8_t> request = makeFoeAbortRequest(counter);

  ec_mbxbuft* out = ecx_getmbx(ctx);
  if (out == nullptr) {
    spdlog::warn("Device {}: no mailbox buffer free for the FoE abort", slavePosition);
    return;
  }
  ec_clearmbx(out);
  std::memcpy(out, request.data(), request.size());

  // ecx_mbxsend returns the buffer to the pool whatever the outcome.
  if (ecx_mbxsend(ctx, slavePosition, out, EC_TIMEOUTTXM) <= 0) {
    spdlog::warn("Device {}: failed to send the FoE abort", slavePosition);
  } else {
    spdlog::info("Device {}: aborted the FoE transfer", slavePosition);
    ec_mbxbuft* in = nullptr;
    ecx_mbxreceive(ctx, slavePosition, &in, kReplyTimeoutUs);
    if (in != nullptr) {
      ecx_dropmbx(ctx, in);
    }
  }

  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
    // The errors the abort produced describe its answer, which is of no use to the caller.
  }
  drainMailbox(ctx, slavePosition);
}

void updateMailboxSyncManagers(ecx_contextt* ctx, uint16_t slave, EtherCatState targetState) {
  if (targetState == EtherCatState::Boot) {
    uint32_t data = ecx_readeeprom(ctx, slave, ECT_SII_BOOTRXMBX, EC_TIMEOUTEEP);
    ctx->slavelist[slave].SM[0].StartAddr = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].SM[0].SMlength = static_cast<uint16_t>(HI_WORD(data));
    ctx->slavelist[slave].mbx_wo = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].mbx_l = static_cast<uint16_t>(HI_WORD(data));

    data = ecx_readeeprom(ctx, slave, ECT_SII_BOOTTXMBX, EC_TIMEOUTEEP);
    ctx->slavelist[slave].SM[1].StartAddr = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].SM[1].SMlength = static_cast<uint16_t>(HI_WORD(data));
    ctx->slavelist[slave].mbx_ro = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].mbx_rl = static_cast<uint16_t>(HI_WORD(data));

    spdlog::info("Device {}: BOOT mailbox - write 0x{:04X}/{}, read 0x{:04X}/{}", slave,
                 ctx->slavelist[slave].mbx_wo, ctx->slavelist[slave].mbx_l,
                 ctx->slavelist[slave].mbx_ro, ctx->slavelist[slave].mbx_rl);

    int wkc0 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[0], EC_TIMEOUTRET);
    int wkc1 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM1, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[1], EC_TIMEOUTRET);
    if (wkc0 != 1 || wkc1 != 1) {
      // A failed SM write leaves stale/invalid mailbox config in the ESC; the slave then
      // refuses the state change — often silently (AL status 0x0000) rather than with a
      // mailbox-config error code. Surface it so it is not mistaken for a slave-side fault.
      spdlog::warn(
          "Device {}: BOOT mailbox SM write incomplete (SM0 wkc={}, SM1 wkc={}) — "
          "slave may refuse the transition",
          slave, wkc0, wkc1);
    }

    // SOMANET firmware keeps its stored mailbox counter when the SMs are reprogrammed. The device
    // restarts into the bootloader when BOOT is requested, and a restarted device stores counter
    // 0, so it accepts the next request whatever its counter is. This reset starts the master's
    // counter from a known point. Without it, a second entry into BOOT wedged the mailbox: the
    // first FoE exchange failed, and later reads failed with wkc 0x5, then 0x0 or 0x3 ("unexpected
    // mailbox"). The firmware source does not explain that failure, so its cause is not confirmed.
    ctx->slavelist[slave].mbx_cnt = 0;
  } else if (targetState == EtherCatState::PreOp) {
    // PRE-OP SMs come from the standard SII mailbox entries, not the BOOT entries.
    ecx_readeeprom1(ctx, slave, ECT_SII_RXMBXADR);
    uint32_t eedat = ecx_readeeprom2(ctx, slave, EC_TIMEOUTEEP);
    ctx->slavelist[slave].mbx_wo = static_cast<uint16_t>(LO_WORD(etohl(eedat)));
    ctx->slavelist[slave].mbx_l = static_cast<uint16_t>(HI_WORD(etohl(eedat)));
    ctx->slavelist[slave].SM[0].StartAddr = ctx->slavelist[slave].mbx_wo;
    ctx->slavelist[slave].SM[0].SMlength = ctx->slavelist[slave].mbx_l;

    ecx_readeeprom1(ctx, slave, ECT_SII_TXMBXADR);
    eedat = ecx_readeeprom2(ctx, slave, EC_TIMEOUTEEP);
    ctx->slavelist[slave].mbx_ro = static_cast<uint16_t>(LO_WORD(etohl(eedat)));
    ctx->slavelist[slave].mbx_rl = static_cast<uint16_t>(HI_WORD(etohl(eedat)));
    if (ctx->slavelist[slave].mbx_rl == 0) {
      ctx->slavelist[slave].mbx_rl = ctx->slavelist[slave].mbx_l;
    }
    ctx->slavelist[slave].SM[1].StartAddr = ctx->slavelist[slave].mbx_ro;
    ctx->slavelist[slave].SM[1].SMlength = ctx->slavelist[slave].mbx_rl;

    spdlog::info("Device {}: PRE-OP mailbox - write 0x{:04X}/{}, read 0x{:04X}/{}", slave,
                 ctx->slavelist[slave].mbx_wo, ctx->slavelist[slave].mbx_l,
                 ctx->slavelist[slave].mbx_ro, ctx->slavelist[slave].mbx_rl);

    int wkc0 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[0], EC_TIMEOUTRET);
    int wkc1 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM1, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[1], EC_TIMEOUTRET);
    if (wkc0 != 1 || wkc1 != 1) {
      // A failed SM write leaves stale/invalid mailbox config in the ESC; the slave then
      // refuses the state change — often silently (AL status 0x0000) rather than with a
      // mailbox-config error code. Surface it so it is not mistaken for a slave-side fault.
      spdlog::warn(
          "Device {}: PRE-OP mailbox SM write incomplete (SM0 wkc={}, SM1 wkc={}) — "
          "slave may refuse the transition",
          slave, wkc0, wkc1);
    }

    // This branch runs only for a slave that this driver drove into BOOT, so mbx_cnt holds the
    // counter of the BOOT-mode FoE transfers. Leaving BOOT for PRE-OP restarts the device into
    // the application, and a restarted device stores counter 0. So, as in the BOOT branch, this
    // reset starts the master's counter from a known point. When a SOMANET slave reaches PRE-OP,
    // transitionToState also sends it a request with counter 0.
    ctx->slavelist[slave].mbx_cnt = 0;
  }

  // Both EEPROM read paths above (the combined ecx_readeeprom in the BOOT branch and the
  // split ecx_readeeprom1/2 in the PRE-OP branch) call ecx_eeprom2master and leave EEPROM
  // control with the master — neither restores it. Some slaves' PDI needs EEPROM access to
  // complete the state change and will silently stay in INIT (AL status 0x0000) if locked
  // out, so hand control back to the PDI before the caller issues writestate. This mirrors
  // ecx_config_init, which also leaves EEPROM with the PDI before requesting a state change.
  ecx_eeprom2pdi(ctx, slave);
}

}  // namespace mm::comm::soem
