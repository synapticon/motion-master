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

// ValueInfo flags for the SDO Info "Get Entry Description" request, ETG.1000.6 §5.6.3.3.
constexpr uint8_t kValueInfoAccess = 0x01;
constexpr uint8_t kValueInfoCategory = 0x02;
constexpr uint8_t kValueInfoPdoMappable = 0x04;
constexpr uint8_t kValueInfoUnit = 0x08;
constexpr uint8_t kValueInfoDefault = 0x10;
constexpr uint8_t kValueInfoMinimum = 0x20;
constexpr uint8_t kValueInfoMaximum = 0x40;

constexpr uint8_t kValueInfoBasic = kValueInfoAccess | kValueInfoCategory | kValueInfoPdoMappable;
constexpr uint8_t kValueInfoExtended = kValueInfoBasic | kValueInfoUnit |
                                       kValueInfoDefault |  // NOLINT(whitespace/indent_namespace)
                                       kValueInfoMinimum |  // NOLINT(whitespace/indent_namespace)
                                       kValueInfoMaximum;   // NOLINT(whitespace/indent_namespace)

/// Parsed result of one "Get Entry Description" exchange.
struct EntryDescription {
  uint16_t dataType;
  uint16_t bitLength;
  uint16_t access;
  std::string name;
  std::optional<uint32_t> unit;
  std::optional<std::vector<uint8_t>> defaultValue;
  std::optional<std::vector<uint8_t>> minValue;
  std::optional<std::vector<uint8_t>> maxValue;
};

/// Error categories distinguished by @c readEntryDescriptionOnce — only @c kSdoInfoError
/// is recoverable by retrying with a reduced ValueInfo mask.
enum class OeError {
  kMailboxFailure,
  kSdoInfoError,
  kUnexpectedResponse,
  kResponseTruncated,
};

/// Drops any unread mailbox content so the next exchange starts clean.
void drainMailbox(ecx_contextt* ctx, uint16_t slave) {
  ec_mbxbuft* stale = nullptr;
  ecx_mbxreceive(ctx, slave, &stale, 0);
  if (stale) {
    ecx_dropmbx(ctx, stale);
  }
}

/// Issues a single CoE SDO Info "Get Entry Description" request and parses the response.
///
/// Layout of the request/response payload (offsets relative to the start of the
/// mailbox buffer; mailbox header is 6 bytes):
///   6..7    CANOpen header (Service = ECT_COES_SDOINFO in bits 12..15)
///   8       Opcode (REQ = 0x05, RES = 0x06, SDOINFO_ERROR = 0x07)
///   9       Reserved
///   10..11  Fragments
///   12..13  Index
///   14      SubIndex
///   15      ValueInfo (request: requested bits; response: bits the slave honoured)
///   16..17  DataType         (response only)
///   18..19  BitLength        (response only)
///   20..21  ObjAccess        (response only)
///   22..    Optional fields, then Name. Optional fields appear in this order, each
///           present only when its bit is set in the response ValueInfo:
///             Unit (4 bytes)         when bit 0x08
///             Default (valueSize)    when bit 0x10
///             Min     (valueSize)    when bit 0x20
///             Max     (valueSize)    when bit 0x40
///           where valueSize = ceil(BitLength / 8).
std::expected<EntryDescription, OeError> readEntryDescriptionOnce(ecx_contextt* ctx, uint16_t slave,
                                                                  uint16_t index, uint8_t subindex,
                                                                  uint8_t requestedValueInfo,
                                                                  std::mutex& mtx) {
  // One atomic mailbox transaction: the drain + getmbx + send + receive + parse
  // must not interleave with another control-plane mailbox op (the manual
  // mbx_cnt update and request/response pairing below are not SOEM-guarded).
  std::lock_guard<std::mutex> lock(mtx);
  drainMailbox(ctx, slave);

  ec_mbxbuft* tx = ecx_getmbx(ctx);
  if (!tx) {
    return std::unexpected(OeError::kMailboxFailure);
  }
  ec_clearmbx(tx);

  const uint8_t cnt = ec_nextmbxcnt(ctx->slavelist[slave].mbx_cnt);
  ctx->slavelist[slave].mbx_cnt = cnt;

  uint8_t* tbuf = reinterpret_cast<uint8_t*>(tx);
  // Mailbox header: length=10 payload bytes, address=0, priority=0, type=COE|(cnt<<4).
  tbuf[0] = 0x0A;
  tbuf[1] = 0x00;
  tbuf[2] = 0x00;
  tbuf[3] = 0x00;
  tbuf[4] = 0x00;
  tbuf[5] = static_cast<uint8_t>(ECT_MBXT_COE | (cnt << 4));
  // CoE header: Service = SDOINFO in bits 12..15 → 0x8000 little-endian.
  tbuf[6] = 0x00;
  tbuf[7] = static_cast<uint8_t>(ECT_COES_SDOINFO << 4);
  tbuf[8] = ECT_GET_OE_REQ;
  tbuf[9] = 0x00;
  tbuf[10] = 0x00;
  tbuf[11] = 0x00;
  tbuf[12] = static_cast<uint8_t>(index & 0xFFu);
  tbuf[13] = static_cast<uint8_t>((index >> 8) & 0xFFu);
  tbuf[14] = subindex;
  tbuf[15] = requestedValueInfo;

  int wkc = ecx_mbxsend(ctx, slave, tx, EC_TIMEOUTTXM);
  if (wkc <= 0) {
    return std::unexpected(OeError::kMailboxFailure);
  }
  // ecx_mbxsend takes ownership of tx on success.

  ec_mbxbuft* rx = nullptr;
  wkc = ecx_mbxreceive(ctx, slave, &rx, EC_TIMEOUTRXM);
  if (wkc <= 0 || !rx) {
    if (rx) {
      ecx_dropmbx(ctx, rx);
    }
    return std::unexpected(OeError::kMailboxFailure);
  }

  struct MbxGuard {
    ecx_contextt* ctx;
    ec_mbxbuft* mbx;
    ~MbxGuard() {
      if (mbx) {
        ecx_dropmbx(ctx, mbx);
      }
    }
  } guard{ctx, rx};

  const uint8_t* rbuf = reinterpret_cast<const uint8_t*>(rx);
  const uint16_t mbxLen = static_cast<uint16_t>(rbuf[0] | (rbuf[1] << 8));
  const uint8_t mbxtype = rbuf[5] & 0x0Fu;
  const uint8_t opcode = rbuf[8] & 0x7Fu;

  if (mbxtype != ECT_MBXT_COE) {
    return std::unexpected(OeError::kUnexpectedResponse);
  }
  if (opcode == ECT_SDOINFO_ERROR) {
    return std::unexpected(OeError::kSdoInfoError);
  }
  if (opcode != ECT_GET_OE_RES) {
    return std::unexpected(OeError::kUnexpectedResponse);
  }

  // Clamp the past-the-end offset to the physical receive buffer. mbxLen is
  // slave-supplied; without this clamp a large/garbage length lets the field
  // reads below (whose only bound is `end`) run past the ec_mbxbuft buffer.
  const size_t end = std::min(static_cast<size_t>(6u) + mbxLen, sizeof(ec_mbxbuft));
  if (end < 22u) {
    return std::unexpected(OeError::kResponseTruncated);
  }

  EntryDescription desc{
      .dataType = static_cast<uint16_t>(rbuf[16] | (rbuf[17] << 8)),
      .bitLength = static_cast<uint16_t>(rbuf[18] | (rbuf[19] << 8)),
      .access = static_cast<uint16_t>(rbuf[20] | (rbuf[21] << 8)),
      .name = {},
      .unit = std::nullopt,
      .defaultValue = std::nullopt,
      .minValue = std::nullopt,
      .maxValue = std::nullopt,
  };

  const uint8_t responseValueInfo = rbuf[15];
  const size_t valueSize = static_cast<size_t>((desc.bitLength + 7u) / 8u);
  size_t offset = 22;

  if (responseValueInfo & kValueInfoUnit) {
    if (offset + 4u > end) {
      return std::unexpected(OeError::kResponseTruncated);
    }
    desc.unit = static_cast<uint32_t>(rbuf[offset]) |
                (static_cast<uint32_t>(rbuf[offset + 1]) << 8) |
                (static_cast<uint32_t>(rbuf[offset + 2]) << 16) |
                (static_cast<uint32_t>(rbuf[offset + 3]) << 24);
    offset += 4u;
  }

  auto readSlice = [&](std::optional<std::vector<uint8_t>>& dst) -> bool {
    if (offset + valueSize > end) {
      return false;
    }
    dst = std::vector<uint8_t>(rbuf + offset, rbuf + offset + valueSize);
    offset += valueSize;
    return true;
  };

  if ((responseValueInfo & kValueInfoDefault) && valueSize > 0u) {
    if (!readSlice(desc.defaultValue)) {
      return std::unexpected(OeError::kResponseTruncated);
    }
  }
  if ((responseValueInfo & kValueInfoMinimum) && valueSize > 0u) {
    if (!readSlice(desc.minValue)) {
      return std::unexpected(OeError::kResponseTruncated);
    }
  }
  if ((responseValueInfo & kValueInfoMaximum) && valueSize > 0u) {
    if (!readSlice(desc.maxValue)) {
      return std::unexpected(OeError::kResponseTruncated);
    }
  }

  if (offset < end) {
    size_t nameLen = end - offset;
    if (nameLen > EC_MAXNAME) {
      nameLen = EC_MAXNAME;
    }
    desc.name.assign(reinterpret_cast<const char*>(rbuf + offset), nameLen);
  }

  return desc;
}

/// Reads one entry's description, asking for default/min/max where the slave can
/// honour it; falls back to the basic mask on @c SDOINFO_ERROR so a slave that
/// rejects the extended request still yields access/dataType/bitLength/name.
std::expected<EntryDescription, std::string> readEntryDescription(ecx_contextt* ctx, uint16_t slave,
                                                                  uint16_t index, uint8_t subindex,
                                                                  std::mutex& mtx) {
  // mtx is taken per transaction inside readEntryDescriptionOnce; the retry
  // sleeps below run unlocked so other control-plane ops can interleave.
  auto callWithRetries = [&](uint8_t mask) -> std::expected<EntryDescription, OeError> {
    auto r = readEntryDescriptionOnce(ctx, slave, index, subindex, mask, mtx);
    for (int i = 0; !r && r.error() == OeError::kMailboxFailure && i < kSdoInfoMaxRetries; ++i) {
      std::this_thread::sleep_for(kSdoInfoRetryDelay);
      r = readEntryDescriptionOnce(ctx, slave, index, subindex, mask, mtx);
    }
    return r;
  };

  auto result = callWithRetries(kValueInfoExtended);
  if (result) {
    return std::move(*result);
  }

  // SDOINFO_ERROR is a definitive "I don't support this mask" — retry once with
  // the basic mask so we still get access/dataType/bitLength/name.
  if (result.error() == OeError::kSdoInfoError) {
    auto fallback = callWithRetries(kValueInfoBasic);
    if (fallback) {
      return std::move(*fallback);
    }
  }

  return std::unexpected(
      std::format("GetEntryDescription slave {} 0x{:04X}:{:02X} failed", slave, index, subindex));
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

    for (uint8_t sub = 0; sub <= odList.MaxSub[i]; ++sub) {
      auto desc =
          readEntryDescription(ctx_.get(), slavePosition, odList.Index[i], sub, socketMutex_);
      if (!desc) {
        spdlog::warn("Device {}: {}", slavePosition, desc.error());
        continue;
      }
      entries.push_back(OdEntry{
          .index = odList.Index[i],
          .subindex = sub,
          .objectCode = odList.ObjectCode[i],
          .dataType = desc->dataType,
          .bitLength = desc->bitLength,
          .access = desc->access,
          .name = std::move(desc->name),
          .unit = desc->unit,
          .defaultValue = std::move(desc->defaultValue),
          .minValue = std::move(desc->minValue),
          .maxValue = std::move(desc->maxValue),
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
        if (stateClean == static_cast<uint16_t>(EtherCatState::Init) &&
            (targetState == EtherCatState::Boot || targetState == EtherCatState::PreOp)) {
          updateMailboxSyncManagers(ctx_.get(), pos, targetState);
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
