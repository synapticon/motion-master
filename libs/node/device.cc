#include "node/device.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "node/device_parameter.h"
#include "node/process_data.h"

namespace mm::node {

Device::Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver, ProcessData* processData)
    : slavePosition_(slavePosition),
      driver_(driver),
      processData_(processData),
      parametersMutex_(std::make_unique<std::mutex>()) {
  auto info = driver_.slaveInfo(slavePosition);
  name_ = std::move(info.name);
  vendorId_ = info.vendorId;
  productCode_ = info.productCode;
  revisionNumber_ = info.revisionNumber;
  serialNumber_ = info.serialNumber;
}

uint16_t Device::slavePosition() const { return slavePosition_; }
const std::string& Device::name() const { return name_; }
uint32_t Device::vendorId() const { return vendorId_; }
uint32_t Device::productCode() const { return productCode_; }
uint32_t Device::revisionNumber() const { return revisionNumber_; }
uint32_t Device::serialNumber() const { return serialNumber_; }

bool Device::mailboxActive() const {
  using mm::comm::EtherCatState;
  const uint16_t status = driver_.slaveState(slavePosition_);
  const EtherCatState state = mm::comm::alState(status);
  // Mailbox (CoE/SDO) communication is available in PRE-OP and above, per the EtherCAT state
  // machine — independent of the AL error indicator: a device in SAFE-OP+error still answers
  // mailbox requests; the error is surfaced separately via the AL status. INIT has no mailbox,
  // and BOOT's mailbox is FoE-only, so neither counts here.
  return state == EtherCatState::PreOp || state == EtherCatState::SafeOp ||
         state == EtherCatState::Op;
}

bool Device::exchangesProcessData() const {
  using mm::comm::EtherCatState;
  const uint16_t status = driver_.slaveState(slavePosition_);
  const EtherCatState state = mm::comm::alState(status);
  return !mm::comm::alHasError(status) &&
         (state == EtherCatState::SafeOp || state == EtherCatState::Op);
}

std::expected<std::vector<uint8_t>, std::string> Device::upload(uint16_t index,
                                                                uint8_t subindex) const {
  return driver_.readSdo(slavePosition_, index, subindex);
}

std::expected<void, std::string> Device::download(uint16_t index, uint8_t subindex,
                                                  std::span<const uint8_t> data) const {
  return driver_.writeSdo(slavePosition_, index, subindex, data);
}

std::expected<std::vector<uint8_t>, std::string> Device::readFile(
    const std::string& filename) const {
  return driver_.readFile(slavePosition_, filename);
}

std::expected<void, std::string> Device::writeFile(const std::string& filename,
                                                   std::span<const uint8_t> data) const {
  return driver_.writeFile(slavePosition_, filename, data);
}

std::expected<void, std::string> Device::readRegister(uint16_t address,
                                                      std::span<uint8_t> data) const {
  return driver_.readRegister(slavePosition_, address, data);
}

std::expected<void, std::string> Device::writeRegister(uint16_t address,
                                                       std::span<const uint8_t> data) const {
  return driver_.writeRegister(slavePosition_, address, data);
}

std::expected<std::vector<uint8_t>, std::string> Device::readSii() const {
  return driver_.readSii(slavePosition_);
}

std::expected<void, std::string> Device::writeSii(std::span<const uint8_t> data) const {
  return driver_.writeSii(slavePosition_, data);
}

std::expected<void, std::string> Device::initializeParameters(bool readValues) {
  auto entries = driver_.readObjectDictionary(slavePosition_);
  if (!entries) {
    return std::unexpected(entries.error());
  }

  // Build into a local map (the per-entry SDO uploads below are slow and must not hold the
  // cache lock — that would stall a concurrent sampler cached-read for the whole enumeration),
  // then swap it in under the lock in one move at the end.
  std::unordered_map<uint32_t, DeviceParameter> built;
  built.reserve(entries->size());

  for (const auto& e : *entries) {
    DeviceParameter p{
        .index = e.index,
        .subindex = e.subindex,
        .name = e.name,
        .objectCode = e.objectCode,
        .dataType = e.dataType,
        .bitLength = e.bitLength,
        .access = e.access,
        .value = defaultValueForDataType(e.dataType),
        .unit = e.unit,
        .defaultValue = std::nullopt,
        .minValue = std::nullopt,
        .maxValue = std::nullopt,
    };

    auto decodeRawBytes =
        [&](const std::optional<std::vector<uint8_t>>& raw) -> std::optional<DeviceParameterValue> {
      if (!raw) {
        return std::nullopt;
      }
      auto decoded = decodeSdoBytes(e.dataType, *raw);
      if (!decoded) {
        spdlog::warn("Device {}: decode 0x{:04X}:{:02X} bound failed: {}", slavePosition_, e.index,
                     e.subindex, decoded.error());
        return std::nullopt;
      }
      return std::move(*decoded);
    };
    p.defaultValue = decodeRawBytes(e.defaultValue);
    p.minValue = decodeRawBytes(e.minValue);
    p.maxValue = decodeRawBytes(e.maxValue);

    if (readValues) {
      auto bytes = upload(e.index, e.subindex);
      if (bytes) {
        auto decoded = decodeSdoBytes(e.dataType, *bytes);
        if (decoded) {
          p.value = std::move(*decoded);
          p.syncState = SyncState::Synced;
        } else {
          spdlog::warn("Device {}: decode 0x{:04X}:{:02X} failed: {}", slavePosition_, e.index,
                       e.subindex, decoded.error());
        }
      } else {
        spdlog::warn("Device {}: SDO upload 0x{:04X}:{:02X} failed: {}", slavePosition_, e.index,
                     e.subindex, bytes.error());
      }
    }

    built.emplace(p.key(), std::move(p));
  }

  // Publish the freshly-built map in one move under the lock.
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  parameters_ = std::move(built);
  return {};
}

const std::unordered_map<uint32_t, DeviceParameter>& Device::parameters() const {
  return parameters_;
}

namespace {

// Little-endian integer readers over a raw SDO byte buffer. A short buffer reads as if
// zero-padded — defensive against a slave returning fewer bytes than the type implies.
uint8_t readU8(const std::vector<uint8_t>& b) { return b.empty() ? uint8_t{0} : b[0]; }

uint16_t readU16(const std::vector<uint8_t>& b) {
  uint16_t v = 0;
  for (size_t i = 0; i < 2 && i < b.size(); ++i) {
    v |= static_cast<uint16_t>(b[i]) << (8 * i);
  }
  return v;
}

uint32_t readU32(const std::vector<uint8_t>& b) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4 && i < b.size(); ++i) {
    v |= static_cast<uint32_t>(b[i]) << (8 * i);
  }
  return v;
}

}  // namespace

std::expected<void, std::string> Device::readPdoAssignment(uint16_t assignmentIndex,
                                                           std::vector<PdoMappingEntry>& out,
                                                           uint32_t& totalBits) {
  out.clear();
  totalBits = 0;

  // Subindex 0 of the assignment object is the count of assigned PDO mapping objects. A
  // device with no PDOs in this direction may not implement the object at all — treat a
  // failed read (or a zero count) as "no entries here", which is not an error.
  auto countBytes = upload(assignmentIndex, 0);
  if (!countBytes) {
    return {};
  }
  const uint8_t pdoCount = readU8(*countBytes);

  // The loop counter is wider than pdoCount's uint8_t on purpose: a device reporting 255 here
  // would make a `uint8_t i <= 255` guard permanently true (i wraps 255->0), spinning forever.
  // The subindex argument is narrowed back to uint8_t at the call.
  for (unsigned i = 1; i <= pdoCount; ++i) {
    auto pdoIndexBytes = upload(assignmentIndex, static_cast<uint8_t>(i));
    if (!pdoIndexBytes) {
      return std::unexpected(std::format("0x{:04X}:{:02X} (PDO assignment) read failed: {}",
                                         assignmentIndex, i, pdoIndexBytes.error()));
    }
    const uint16_t mappingIndex = readU16(*pdoIndexBytes);
    if (mappingIndex == 0) {
      continue;  // unused assignment slot
    }

    // Subindex 0 of the mapping object is the count of mapped entries.
    auto entryCountBytes = upload(mappingIndex, 0);
    if (!entryCountBytes) {
      return std::unexpected(std::format("0x{:04X}:00 (PDO mapping) read failed: {}", mappingIndex,
                                         entryCountBytes.error()));
    }
    const uint8_t entryCount = readU8(*entryCountBytes);

    // Wider counter for the same reason as the outer loop: entryCount == 255 must still terminate.
    for (unsigned e = 1; e <= entryCount; ++e) {
      auto entryBytes = upload(mappingIndex, static_cast<uint8_t>(e));
      if (!entryBytes) {
        return std::unexpected(std::format("0x{:04X}:{:02X} (PDO mapping entry) read failed: {}",
                                           mappingIndex, e, entryBytes.error()));
      }
      // Packed entry (ETG.1000.6 §5.6.7.4.7): bits 31..16 = object index,
      // 15..8 = subindex, 7..0 = bit length. An index of 0 is an alignment gap.
      const uint32_t packed = readU32(*entryBytes);
      PdoMappingEntry entry{
          .index = static_cast<uint16_t>(packed >> 16),
          .subindex = static_cast<uint8_t>((packed >> 8) & 0xFF),
          .bitLength = static_cast<uint16_t>(packed & 0xFF),
          .bitOffset = totalBits,
      };
      totalBits += entry.bitLength;
      out.push_back(entry);
    }
  }
  return {};
}

std::expected<void, std::string> Device::readPdoMappings() {
  PdoMappings mappings;
  if (auto r = readPdoAssignment(0x1C12, mappings.outputs, mappings.outputBits); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = readPdoAssignment(0x1C13, mappings.inputs, mappings.inputBits); !r) {
    return std::unexpected(r.error());
  }
  pdoMappings_ = std::move(mappings);
  spdlog::debug("Device {}: PDO mapping - {} output entries ({} bits), {} input entries ({} bits)",
                slavePosition_, pdoMappings_.outputs.size(), pdoMappings_.outputBits,
                pdoMappings_.inputs.size(), pdoMappings_.inputBits);
  return {};
}

const PdoMappings& Device::pdoMappings() const { return pdoMappings_; }

std::expected<void, std::string> Device::setValue(uint16_t index, uint8_t subindex,
                                                  DeviceParameterValue value) {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  DeviceParameter* p = findParameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  if (auto set = p->setValue(std::move(value)); !set) {
    return std::unexpected(set.error());
  }
  p->syncState = SyncState::Synced;
  return {};
}

std::expected<DeviceParameterValue, std::string> Device::setValueFromBytes(
    uint16_t index, uint8_t subindex, std::span<const uint8_t> bytes) {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  DeviceParameter* p = findParameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  auto decoded = decodeSdoBytes(p->dataType, bytes);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  if (auto set = p->setValue(*decoded); !set) {
    return std::unexpected(set.error());
  }
  p->syncState = SyncState::Synced;
  return p->value;
}

std::expected<std::vector<uint8_t>, std::string> Device::valueAsBytes(uint16_t index,
                                                                      uint8_t subindex) const {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  const DeviceParameter* p = parameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  return encodeSdoBytes(p->dataType, p->value);
}

std::vector<DeviceParameter> Device::parametersOrdered() const {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  std::vector<DeviceParameter> ordered;
  ordered.reserve(parameters_.size());
  for (const auto& [key, p] : parameters_) {
    ordered.push_back(p);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const DeviceParameter& a, const DeviceParameter& b) { return a.key() < b.key(); });
  return ordered;
}

const DeviceParameter* Device::parameter(uint16_t index, uint8_t subindex) const {
  auto it = parameters_.find(makeParameterKey(index, subindex));
  return it != parameters_.end() ? &it->second : nullptr;
}

std::optional<DeviceParameterValue> Device::value(uint16_t index, uint8_t subindex) const {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  const DeviceParameter* p = parameter(index, subindex);
  if (!p) {
    return std::nullopt;
  }
  return p->value;
}

std::optional<uint16_t> Device::dataType(uint16_t index, uint8_t subindex) const {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  const DeviceParameter* p = parameter(index, subindex);
  if (!p) {
    return std::nullopt;
  }
  return p->dataType;
}

DeviceParameter* Device::findParameter(uint16_t index, uint8_t subindex) {
  auto it = parameters_.find(makeParameterKey(index, subindex));
  return it != parameters_.end() ? &it->second : nullptr;
}

std::expected<DeviceParameterValue, std::string> Device::readParameter(uint16_t index,
                                                                       uint8_t subindex) {
  // Held across the upload below: one mailbox round-trip, so a concurrent cached read of this
  // device waits at most that long. The map cannot be rehashed (by initializeParameters) while
  // we hold it, so the p pointer stays valid across the bus call.
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  DeviceParameter* p = findParameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  // Prefer the live PDO image while exchanging: readPdo returns the staged/snapshot bytes when the
  // object is PDO-mapped and the bus is healthy, and nullopt otherwise (not mapped / unhealthy /
  // no image) — in which case we fall through to the authoritative SDO upload below. Decode inline
  // rather than via setValueFromBytes, which would re-take parametersMutex_ that we already hold.
  if (processData_ && exchangesProcessData()) {
    if (auto bytes = processData_->readPdo(slavePosition_, index, subindex)) {
      auto decoded = decodeSdoBytes(p->dataType, *bytes);
      if (!decoded) {
        return std::unexpected(decoded.error());
      }
      p->value = std::move(*decoded);
      p->syncState = SyncState::Synced;
      return p->value;
    }
  }
  if (!mailboxActive()) {
    return p->value;  // no mailbox: serve the cached value, never touch the bus
  }
  auto bytes = upload(index, subindex);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  auto decoded = decodeSdoBytes(p->dataType, *bytes);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  p->value = std::move(*decoded);
  p->syncState = SyncState::Synced;
  return p->value;
}

std::expected<void, std::string> Device::writeParameter(uint16_t index, uint8_t subindex,
                                                        DeviceParameterValue value) {
  // Held across the download below (one mailbox round-trip), like readParameter.
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  DeviceParameter* p = findParameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  // Cache-first: coerce and store into the cached parameter before any bus access, so the
  // cache always reflects the latest intended value regardless of online state.
  if (auto set = p->setValue(value); !set) {
    return std::unexpected(set.error());
  }
  // While exchanging, stage an output-mapped object into the process image (sent next cycle)
  // instead of an SDO download. writePdo returns false when the object is not output-mapped (or no
  // image is published), in which case we fall through to the SDO/offline paths below. Encode
  // inline rather than via valueAsBytes, which would re-take parametersMutex_ that we already hold.
  // No health gate here (unlike the read path): staging is always safe — the value is simply sent
  // on the next cycle.
  if (processData_ && exchangesProcessData()) {
    auto bytes = encodeSdoBytes(p->dataType, p->value);
    if (bytes && processData_->writePdo(slavePosition_, index, subindex, *bytes)) {
      p->syncState = SyncState::Synced;
      return {};
    }
  }
  if (!mailboxActive()) {
    // No mailbox: hold the change in the cache, to be flushed when the device returns.
    p->syncState = SyncState::Pending;
    return {};
  }
  auto bytes = encodeSdoBytes(p->dataType, p->value);
  if (!bytes) {
    p->syncState = SyncState::Pending;
    return std::unexpected(bytes.error());
  }
  if (auto w = download(index, subindex, *bytes); !w) {
    p->syncState = SyncState::Pending;
    return std::unexpected(w.error());
  }
  p->syncState = SyncState::Synced;
  return {};
}

void to_json(nlohmann::json& j, const Device& d) {
  j = nlohmann::json{
      {"slavePosition", d.slavePosition()},
      {"name", d.name()},
      {"vendorId", d.vendorId()},
      {"productCode", d.productCode()},
      {"revisionNumber", d.revisionNumber()},
      {"serialNumber", d.serialNumber()},
  };
}

namespace {

// CoE Modular Device Profile object indices (ETG.5001).
constexpr uint16_t kDetectedModuleIdentList = 0xF050;    // detected (actual) modules
constexpr uint16_t kConfiguredModuleIdentList = 0xF030;  // configured (expected) modules

// Decodes the subindex-0 "number of entries" of an MDP array. Slaves encode this
// count as either UNSIGNED8 (1 byte) or UNSIGNED16 (2 bytes, little-endian).
uint16_t decodeEntryCount(std::span<const uint8_t> bytes) {
  if (bytes.size() >= 2) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
  }
  if (bytes.size() == 1) {
    return bytes[0];
  }
  return 0;
}

}  // namespace

std::expected<int, std::string> reconcileDetectedModules(const Device& device) {
  // 0xF050:00 — number of detected module slots. A slave that has no detected-module
  // list is simply not modular; treat that as "nothing to reconcile" rather than an error.
  auto count = device.upload(kDetectedModuleIdentList, 0x00);
  if (!count) {
    spdlog::debug("Device {}: no detected-module list (0xF050); not a modular device",
                  device.slavePosition());
    return 0;
  }
  const uint16_t slots = decodeEntryCount(*count);

  int written = 0;
  std::string failures;
  // CoE subindices are 8-bit, so there are at most 255 module slots. Capping the (wider) counter
  // at 255 both terminates — a `uint16_t sub <= 0xFFFF` guard would wrap and spin forever on a
  // device reporting 65535 — and avoids truncating the subindex argument for sub > 255.
  const unsigned slotCount = std::min<unsigned>(slots, 0xFFu);
  for (unsigned sub = 1; sub <= slotCount; ++sub) {
    auto detected = device.upload(kDetectedModuleIdentList, static_cast<uint8_t>(sub));
    if (!detected || detected->size() != sizeof(uint32_t)) {
      continue;
    }
    // An all-zero ident means the slot is empty — nothing detected to configure.
    bool slotEmpty =
        std::all_of(detected->begin(), detected->end(), [](uint8_t b) { return b == 0; });
    if (slotEmpty) {
      continue;
    }
    // Skip the write when the configured list already matches — keeps this idempotent.
    auto configured = device.upload(kConfiguredModuleIdentList, static_cast<uint8_t>(sub));
    if (configured && *configured == *detected) {
      continue;
    }
    if (auto w = device.download(kConfiguredModuleIdentList, static_cast<uint8_t>(sub), *detected);
        !w) {
      if (!failures.empty()) {
        failures += "; ";
      }
      failures += std::format("slot {}: {}", sub, w.error());
      continue;
    }
    const uint32_t ident = (*detected)[0] | ((*detected)[1] << 8) | ((*detected)[2] << 16) |
                           (static_cast<uint32_t>((*detected)[3]) << 24);
    spdlog::info("Device {}: module slot {} configured to detected ident {:#010x}",
                 device.slavePosition(), sub, ident);
    ++written;
  }

  if (!failures.empty()) {
    return std::unexpected(failures);
  }
  return written;
}

}  // namespace mm::node
