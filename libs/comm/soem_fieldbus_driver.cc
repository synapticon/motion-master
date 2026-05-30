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
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/al_status_codes.h"

namespace mm::comm::soem {

SoemFieldbusDriver::SoemFieldbusDriver(std::string ifname) : ifname_(std::move(ifname)) {}

SoemFieldbusDriver::~SoemFieldbusDriver() {}

std::expected<void, std::string> SoemFieldbusDriver::init() {
  std::lock_guard<std::mutex> lock(socketMutex_);
  // SOEM has no auto-detect — an empty interface name would reach ecx_init and
  // fail with a cryptic "No such device". Reject it up front with a clear error.
  if (ifname_.empty()) {
    return std::unexpected(
        "no network adapter specified — SOEM requires a NIC name or MAC address");
  }
  ctx_ = std::make_unique<ecx_contextt>();
  if (!ecx_init(ctx_.get(), ifname_.c_str())) {
    ctx_.reset();
    return std::unexpected("ecx_init failed on " + ifname_ + ": " + std::strerror(errno));
  }
  spdlog::debug("SOEM init on adapter '{}'", ifname_);
  return {};
}

std::expected<int, std::string> SoemFieldbusDriver::scan() {
  std::lock_guard<std::mutex> lock(socketMutex_);
  // ecx_config_init reprograms every slave's mailbox SMs to PRE-OP sizes and hands
  // EEPROM back to the PDI, so any prior BOOT-SM tracking is now stale.
  bootMailboxSlaves_.clear();
  ctx_->manualstatechange = 1;
  int found = ecx_config_init(ctx_.get());
  if (found <= 0) {
    return std::unexpected("ecx_config_init found no slaves on " + ifname_);
  }
  spdlog::debug("SOEM scan found {} slave(s) on '{}'", found, ifname_);
  return found;
}

// Intentionally lock-free: the RT PDO cycle relies on SOEM's internally
// thread-safe port layer rather than socketMutex_, so a slow control-plane
// transfer can never stall process-data exchange. See FieldbusDriver class doc.
void SoemFieldbusDriver::exchangeProcessData() {}

void SoemFieldbusDriver::stop() {}

SlaveInfo SoemFieldbusDriver::slaveInfo(uint16_t position) const {
  std::lock_guard<std::mutex> lock(socketMutex_);
  const auto& s = ctx_->slavelist[position];
  return {
      .name = std::string(s.name),
      .vendorId = s.eep_man,
      .productCode = s.eep_id,
      .revisionNumber = s.eep_rev,
      .serialNumber = s.eep_ser,
  };
}

int SoemFieldbusDriver::slaveCount() const {
  std::lock_guard<std::mutex> lock(socketMutex_);
  return ctx_ ? ctx_->slavecount : 0;
}

std::expected<std::vector<FieldbusDriver::SlaveStateRaw>, std::string>
SoemFieldbusDriver::readStates(const std::vector<uint16_t>& positions) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  ecx_readstate(ctx_.get());
  std::vector<SlaveStateRaw> result;
  result.reserve(positions.size());
  std::ranges::transform(positions, std::back_inserter(result), [this](uint16_t pos) {
    return SlaveStateRaw{.alStatus = ctx_->slavelist[pos].state,
                         .alStatusCode = ctx_->slavelist[pos].ALstatuscode};
  });
  return result;
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readSdo(uint16_t slavePosition,
                                                                             uint16_t index,
                                                                             uint8_t subindex) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  spdlog::debug("SDOread slave {} 0x{:04X}:{:02X}", slavePosition, index, subindex);
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
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  data.resize(size);
  spdlog::debug("SDOread slave {} 0x{:04X}:{:02X} ok ({} bytes)", slavePosition, index, subindex,
                data.size());
  return data;
}

std::expected<void, std::string> SoemFieldbusDriver::writeSdo(uint16_t slavePosition,
                                                              uint16_t index, uint8_t subindex,
                                                              std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  spdlog::debug("SDOwrite slave {} 0x{:04X}:{:02X} ({} bytes)", slavePosition, index, subindex,
                data.size());
  // ecx_SDOwrite takes void*, not const void*.
  int wkc =
      ecx_SDOwrite(ctx_.get(), slavePosition, index, subindex, FALSE, static_cast<int>(data.size()),
                   const_cast<uint8_t*>(data.data()), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg =
        std::format("SDOwrite slave {} 0x{:04X}:{:02X} failed", slavePosition, index, subindex);
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
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  spdlog::debug("SDOwrite slave {} 0x{:04X}:{:02X} ok", slavePosition, index, subindex);
  return {};
}

namespace {

// SOEM's SDO Info path occasionally times out on slaves that buffer slowly;
// retry with a short back-off rather than failing the whole enumeration.
constexpr int kSdoInfoMaxRetries = 10;
constexpr auto kSdoInfoRetryDelay = std::chrono::milliseconds(50);

template <typename F>
int retrySdoInfo(F&& call) {
  int result = call();
  for (int i = 0; result <= 0 && i < kSdoInfoMaxRetries; ++i) {
    std::this_thread::sleep_for(kSdoInfoRetryDelay);
    result = call();
  }
  return result;
}

}  // namespace

std::expected<std::vector<OdEntry>, std::string> SoemFieldbusDriver::readObjectDictionary(
    uint16_t slavePosition) {
  // Fine-grained locking: socketMutex_ is taken per individual SDO Info
  // transaction (and released during retrySdoInfo's back-off sleeps), so this
  // multi-second enumeration never blocks another control-plane caller for more
  // than a single transfer.
  spdlog::debug("readObjectDictionary slave {}", slavePosition);
  ec_ODlistt odList{};
  if (retrySdoInfo([&] {
        std::lock_guard<std::mutex> lock(socketMutex_);
        return ecx_readODlist(ctx_.get(), slavePosition, &odList);
      }) <= 0) {
    return std::unexpected(std::format("readODlist slave {} failed after retries", slavePosition));
  }

  std::vector<OdEntry> entries;
  entries.reserve(odList.Entries);

  for (uint16_t i = 0; i < odList.Entries; ++i) {
    if (retrySdoInfo([&] {
          std::lock_guard<std::mutex> lock(socketMutex_);
          return ecx_readODdescription(ctx_.get(), i, &odList);
        }) <= 0) {
      return std::unexpected(std::format("readODdescription slave {} index 0x{:04X} failed",
                                         slavePosition, odList.Index[i]));
    }

    // SOEM's ecx_readOEsingle issues a basic-info "Get Entry Description"
    // request (access/category/PDO only) and reads the name directly — it never
    // asks for Unit/Default/Min/Max, sidestepping the SOMANET firmware quirk
    // where those bits are echoed without payload and corrupt the name. Those
    // fields stay empty here; they are not available from this service.
    ec_OElistt oeList{};
    for (uint8_t sub = 0; sub <= odList.MaxSub[i]; ++sub) {
      if (retrySdoInfo([&] {
            std::lock_guard<std::mutex> lock(socketMutex_);
            return ecx_readOEsingle(ctx_.get(), i, sub, &odList, &oeList);
          }) <= 0) {
        spdlog::warn("Device {}: readOEsingle 0x{:04X}:{:02X} failed", slavePosition,
                     odList.Index[i], sub);
        continue;
      }
      entries.push_back(OdEntry{
          .index = odList.Index[i],
          .subindex = sub,
          .objectCode = odList.ObjectCode[i],
          .dataType = oeList.DataType[sub],
          .bitLength = oeList.BitLength[sub],
          .access = oeList.ObjAccess[sub],
          .name = oeList.Name[sub],
          .unit = std::nullopt,
          .defaultValue = std::nullopt,
          .minValue = std::nullopt,
          .maxValue = std::nullopt,
      });
    }
  }

  spdlog::debug("readObjectDictionary slave {} ok ({} entries)", slavePosition, entries.size());
  return entries;
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readFile(
    uint16_t slavePosition, const std::string& filename) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  spdlog::debug("FOEread slave {} '{}'", slavePosition, filename);
  constexpr int kMaxSize = 10 * 1024 * 1024;
  std::vector<uint8_t> data(kMaxSize);
  int size = kMaxSize;
  std::string name = filename;  // ecx_FOEread takes non-const char*
  int wkc =
      ecx_FOEread(ctx_.get(), slavePosition, name.data(), 0, &size, data.data(), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg = std::format("FOEread slave {} '{}' failed", slavePosition, filename);
    ec_errort err{};
    if (ecx_poperror(ctx_.get(), &err)) {
      switch (err.Etype) {
        case EC_ERR_TYPE_FOE_ERROR:
          msg += std::format(" (FoE error 0x{:08X})", static_cast<uint32_t>(err.AbortCode));
          break;
        case EC_ERR_TYPE_FOE_BUF2SMALL:
          msg += " (buffer too small)";
          break;
        case EC_ERR_TYPE_FOE_PACKETNUMBER:
          msg += " (packet number mismatch)";
          break;
        case EC_ERR_TYPE_FOE_FILE_NOTFOUND:
          msg += " (file not found)";
          break;
        default:
          msg += std::format(" (etype {})", static_cast<int>(err.Etype));
          break;
      }
    }
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  data.resize(size);
  spdlog::debug("FOEread slave {} '{}' ok ({} bytes)", slavePosition, filename, data.size());
  return data;
}

std::expected<void, std::string> SoemFieldbusDriver::writeFile(uint16_t slavePosition,
                                                               const std::string& filename,
                                                               std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  spdlog::debug("FOEwrite slave {} '{}' ({} bytes)", slavePosition, filename, data.size());
  std::string name = filename;  // ecx_FOEwrite takes non-const char*
  int wkc = ecx_FOEwrite(ctx_.get(), slavePosition, name.data(), 0, static_cast<int>(data.size()),
                         const_cast<uint8_t*>(data.data()), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg = std::format("FOEwrite slave {} '{}' failed", slavePosition, filename);
    ec_errort err{};
    if (ecx_poperror(ctx_.get(), &err)) {
      switch (err.Etype) {
        case EC_ERR_TYPE_FOE_ERROR:
          msg += std::format(" (FoE error 0x{:08X})", static_cast<uint32_t>(err.AbortCode));
          break;
        case EC_ERR_TYPE_FOE_BUF2SMALL:
          msg += " (buffer too small)";
          break;
        case EC_ERR_TYPE_FOE_PACKETNUMBER:
          msg += " (packet number mismatch)";
          break;
        case EC_ERR_TYPE_FOE_FILE_NOTFOUND:
          msg += " (file not found)";
          break;
        default:
          msg += std::format(" (etype {})", static_cast<int>(err.Etype));
          break;
      }
    }
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  spdlog::debug("FOEwrite slave {} '{}' ok", slavePosition, filename);
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::readRegister(uint16_t slavePosition,
                                                                  uint16_t address,
                                                                  std::span<uint8_t> data) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  spdlog::debug("FPRD slave {} 0x{:04X} ({} bytes)", slavePosition, address, data.size());
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  int wkc = ecx_FPRD(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     data.data(), EC_TIMEOUTRET);
  if (wkc != 1) {
    std::string msg =
        "FPRD slave " + std::to_string(slavePosition) + ": wkc=" + std::to_string(wkc);
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::writeRegister(uint16_t slavePosition,
                                                                   uint16_t address,
                                                                   std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(socketMutex_);
  spdlog::debug("FPWR slave {} 0x{:04X} ({} bytes)", slavePosition, address, data.size());
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  // ecx_FPWR takes void*, not const void*
  int wkc = ecx_FPWR(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     const_cast<uint8_t*>(data.data()), EC_TIMEOUTRET);
  if (wkc != 1) {
    std::string msg =
        "FPWR slave " + std::to_string(slavePosition) + ": wkc=" + std::to_string(wkc);
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
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
  }

  // Both EEPROM read paths above (the combined ecx_readeeprom in the BOOT branch and the
  // split ecx_readeeprom1/2 in the PRE-OP branch) call ecx_eeprom2master and leave EEPROM
  // control with the master — neither restores it. Some slaves' PDI needs EEPROM access to
  // complete the state change and will silently stay in INIT (AL status 0x0000) if locked
  // out, so hand control back to the PDI before the caller issues writestate. This mirrors
  // ecx_config_init, which also leaves EEPROM with the PDI before requesting a state change.
  ecx_eeprom2pdi(ctx, slave);
}

void SoemFieldbusDriver::transitionToState(const std::vector<uint16_t>& positions,
                                           std::optional<EtherCatState> requiredState,
                                           EtherCatState targetState,
                                           std::chrono::steady_clock::duration timeout,
                                           std::chrono::steady_clock::duration resendInterval,
                                           std::function<void()> tick,
                                           std::function<bool()> shouldAbort) {
  const auto targetRaw = static_cast<uint16_t>(targetState);

  // socketMutex_ is taken only around the discrete socket transactions below and
  // is never held across the poll sleep or the tick()/shouldAbort() callbacks, so
  // a multi-second transition does not block other control-plane callers (and the
  // PDO tick, being lock-free, never contends here).
  std::set<uint16_t> pending;
  {
    std::lock_guard<std::mutex> lock(socketMutex_);
    ecx_readstate(ctx_.get());
    for (uint16_t pos : positions) {
      uint16_t state = ctx_->slavelist[pos].state;
      uint16_t stateClean = state & 0x000Fu;
      if (!requiredState || stateClean == static_cast<uint16_t>(*requiredState)) {
        // Mailbox sync managers only need reprogramming when leaving INIT, and only in
        // two cases: entering BOOT (which uses larger BOOT-specific mailbox SMs), or
        // entering PRE-OP from a slave that still holds stale BOOT SMs (i.e. one we drove
        // into BOOT earlier, e.g. for a firmware download). On a plain fresh-scan
        // INIT→PRE-OP the SMs ecx_config_init already programmed are correct, and touching
        // them here is both redundant and harmful (it seizes EEPROM from the slave's PDI).
        if (stateClean == static_cast<uint16_t>(EtherCatState::Init)) {
          if (targetState == EtherCatState::Boot) {
            updateMailboxSyncManagers(ctx_.get(), pos, EtherCatState::Boot);
            bootMailboxSlaves_.insert(pos);
          } else if (targetState == EtherCatState::PreOp && bootMailboxSlaves_.contains(pos)) {
            updateMailboxSyncManagers(ctx_.get(), pos, EtherCatState::PreOp);
            bootMailboxSlaves_.erase(pos);
          }
        }
        ctx_->slavelist[pos].state = targetRaw;
        ecx_writestate(ctx_.get(), pos);
        pending.insert(pos);
      }
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

    // Locked for the rest of this iteration (state read, evaluation, resend) —
    // released at the loop-body scope exit before the next tick()/sleep.
    std::lock_guard<std::mutex> lock(socketMutex_);
    ecx_readstate(ctx_.get());

    for (auto it = pending.begin(); it != pending.end();) {
      uint16_t pos = *it;
      uint16_t state = ctx_->slavelist[pos].state;
      uint16_t alStatusCode = ctx_->slavelist[pos].ALstatuscode;
      // Exact match required: OP+ERROR (0x18) must not pass as OP (0x08).
      if (state == targetRaw) {
        spdlog::info("Device {}: reached state 0x{:02X}", pos, targetRaw);
        it = pending.erase(it);
      } else if ((state & EC_STATE_ERROR) && isAlStatusCodeTerminal(alStatusCode)) {
        // Slave reported a terminal AL status code — retrying the same writestate
        // cannot succeed. Drop it from the pending set immediately so the caller
        // gets fast feedback instead of spinning until timeout.
        spdlog::warn("Device {}: terminal AL status 0x{:04X}; cannot reach state 0x{:02X}", pos,
                     alStatusCode, targetRaw);
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
    std::lock_guard<std::mutex> lock(socketMutex_);
    for (uint16_t pos : pending) {
      spdlog::error("Device {}: failed to reach state 0x{:02X} (AL status: 0x{:04X})", pos,
                    targetRaw, ctx_->slavelist[pos].ALstatuscode);
    }
  }
}

}  // namespace mm::comm::soem
