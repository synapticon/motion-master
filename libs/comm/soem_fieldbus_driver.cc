#include "comm/soem_fieldbus_driver.h"

#include <soem/soem.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mm::comm::soem {

SoemFieldbusDriver::SoemFieldbusDriver(std::string ifname) : ifname_(std::move(ifname)) {}

SoemFieldbusDriver::~SoemFieldbusDriver() {}

std::expected<void, std::string> SoemFieldbusDriver::init() {
  ctx_ = std::make_unique<ecx_contextt>();
  if (!ecx_init(ctx_.get(), ifname_.c_str())) {
    ctx_.reset();
    return std::unexpected("ecx_init failed on " + ifname_ + ": " + std::strerror(errno));
  }
  return {};
}

std::expected<int, std::string> SoemFieldbusDriver::scan() {
  ctx_->manualstatechange = 1;
  int found = ecx_config_init(ctx_.get());
  if (found <= 0) {
    return std::unexpected("ecx_config_init found no slaves on " + ifname_);
  }
  return found;
}

void SoemFieldbusDriver::exchangeProcessData() {}

void SoemFieldbusDriver::stop() {}

SlaveInfo SoemFieldbusDriver::slaveInfo(uint16_t position) const {
  const auto& s = ctx_->slavelist[position];
  return {
      .name = std::string(s.name),
      .vendorId = s.eep_man,
      .productCode = s.eep_id,
      .revisionNumber = s.eep_rev,
      .serialNumber = s.eep_ser,
  };
}

int SoemFieldbusDriver::slaveCount() const { return ctx_ ? ctx_->slavecount : 0; }

std::expected<std::vector<uint16_t>, std::string> SoemFieldbusDriver::readStates(
    const std::vector<uint16_t>& positions) {
  ecx_readstate(ctx_.get());
  std::vector<uint16_t> result;
  result.reserve(positions.size());
  std::ranges::transform(positions, std::back_inserter(result),
                         [this](uint16_t pos) { return ctx_->slavelist[pos].state; });
  return result;
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readSdo(uint16_t slavePosition,
                                                                             uint16_t index,
                                                                             uint8_t subindex) {
  std::vector<uint8_t> data(4096, 0);
  int size = static_cast<int>(data.size());
  int wkc = ecx_SDOread(ctx_.get(), slavePosition, index, subindex, FALSE, &size, data.data(),
                        EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg =
        std::format("SDOread slave {} 0x{:04X}:{:02X} failed", slavePosition, index, subindex);
    ec_errort err{};
    if (ecx_poperror(ctx_.get(), &err)) {
      switch (err.Etype) {
        case EC_ERR_TYPE_SDO_ERROR:
          msg += std::format(" (SDO abort 0x{:08X})", static_cast<uint32_t>(err.AbortCode));
          break;
        case EC_ERR_TYPE_MBX_ERROR:
          msg += " (mailbox error)";
          break;
        case EC_ERR_TYPE_PACKET_ERROR:
          msg += " (packet/timeout error)";
          break;
        default:
          msg += std::format(" (etype {})", static_cast<int>(err.Etype));
          break;
      }
    }
    return std::unexpected(msg);
  }
  data.resize(size);
  return data;
}

std::expected<void, std::string> SoemFieldbusDriver::readRegister(uint16_t slavePosition,
                                                                  uint16_t address,
                                                                  std::span<uint8_t> data) {
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  int wkc = ecx_FPRD(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     data.data(), EC_TIMEOUTRET);
  if (wkc != 1) {
    return std::unexpected("FPRD slave " + std::to_string(slavePosition) +
                           ": wkc=" + std::to_string(wkc));
  }
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::writeRegister(uint16_t slavePosition,
                                                                   uint16_t address,
                                                                   std::span<const uint8_t> data) {
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  // ecx_FPWR takes void*, not const void*
  int wkc = ecx_FPWR(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     const_cast<uint8_t*>(data.data()), EC_TIMEOUTRET);
  if (wkc != 1) {
    return std::unexpected("FPWR slave " + std::to_string(slavePosition) +
                           ": wkc=" + std::to_string(wkc));
  }
  return {};
}

// BOOT and PRE-OP use different mailbox sizes (e.g. 1024 vs 128 bytes on Integro
// devices). After a firmware download the slave context still holds BOOT SM parameters;
// without reprogramming them here an INIT→PRE-OP transition would reuse stale BOOT
// values and break mailbox communication. SOEM does not do this automatically.
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

    ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0, sizeof(ec_smt),
             &ctx->slavelist[slave].SM[0], EC_TIMEOUTRET);
    ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM1, sizeof(ec_smt),
             &ctx->slavelist[slave].SM[1], EC_TIMEOUTRET);

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

    ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0, sizeof(ec_smt),
             &ctx->slavelist[slave].SM[0], EC_TIMEOUTRET);
    ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM1, sizeof(ec_smt),
             &ctx->slavelist[slave].SM[1], EC_TIMEOUTRET);
  }
}

void SoemFieldbusDriver::transitionToState(const std::vector<uint16_t>& positions,
                                           std::optional<EtherCatState> requiredState,
                                           EtherCatState targetState,
                                           std::chrono::steady_clock::duration timeout,
                                           std::chrono::steady_clock::duration resendInterval,
                                           std::function<void()> tick,
                                           std::function<bool()> shouldAbort) {
  const auto targetRaw = static_cast<uint16_t>(targetState);

  ecx_readstate(ctx_.get());

  std::set<uint16_t> pending;
  for (uint16_t pos : positions) {
    uint16_t state = ctx_->slavelist[pos].state;
    uint16_t stateClean = state & 0x000Fu;
    if (!requiredState || stateClean == static_cast<uint16_t>(*requiredState)) {
      if (stateClean == static_cast<uint16_t>(EtherCatState::Init) &&
          (targetState == EtherCatState::Boot || targetState == EtherCatState::PreOp)) {
        updateMailboxSyncManagers(ctx_.get(), pos, targetState);
      }
      ctx_->slavelist[pos].state = targetRaw;
      ecx_writestate(ctx_.get(), pos);
      pending.insert(pos);
    }
  }

  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto lastResend = std::chrono::steady_clock::now();
  bool aborted = false;

  while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
    if (shouldAbort && shouldAbort()) {
      aborted = true;
      break;
    }

    // When a tick is provided (e.g. SAFE-OP → OP), call it at ~1 ms intervals
    // so process data keeps flowing and the SM watchdog does not fire.
    if (tick) {
      auto pollEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      auto nextTick = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() < pollEnd) {
        tick();
        nextTick += std::chrono::milliseconds(1);
        auto now = std::chrono::steady_clock::now();
        if (nextTick > now) {
          std::this_thread::sleep_for(nextTick - now);
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ecx_readstate(ctx_.get());

    for (auto it = pending.begin(); it != pending.end();) {
      uint16_t pos = *it;
      // Exact match required: OP+ERROR (0x18) must not pass as OP (0x08).
      if (ctx_->slavelist[pos].state == targetRaw) {
        spdlog::info("Device {}: reached state 0x{:02X}", pos, targetRaw);
        it = pending.erase(it);
      } else {
        ++it;
      }
    }

    if (std::chrono::steady_clock::now() - lastResend > resendInterval) {
      for (uint16_t pos : pending) {
        uint16_t state = ctx_->slavelist[pos].state;
        if (state & EC_STATE_ERROR) {
          ctx_->slavelist[pos].state = (state & 0x000Fu) | EC_STATE_ACK;
          ecx_writestate(ctx_.get(), pos);
        }
        ctx_->slavelist[pos].state = targetRaw;
        ecx_writestate(ctx_.get(), pos);
      }
      lastResend = std::chrono::steady_clock::now();
    }
  }

  if (!aborted) {
    for (uint16_t pos : pending) {
      spdlog::error("Device {}: failed to reach state 0x{:02X} (AL status: 0x{:04X})", pos,
                    targetRaw, ctx_->slavelist[pos].ALstatuscode);
    }
  }
}

}  // namespace mm::comm::soem
