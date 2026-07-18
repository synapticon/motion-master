#include "node/device.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/util.h"
#include "node/device_parameter.h"
#include "node/parameter_cache.h"
#include "node/process_data.h"
#include "node/process_image.h"
#include "node/synapticon.h"

namespace mm::node {

Device::Device(uint16_t slavePosition, mm::comm::FieldbusDriver& driver, ProcessData* processData,
               const ParameterCache* parameterCache)
    : slavePosition_(slavePosition),
      driver_(driver),
      processData_(processData),
      parameterCache_(parameterCache),
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
std::string Device::productName() const {
  if (vendorId_ == kSynapticonVendorId) {
    if (const std::string_view resolved = somanetProductName(productCode_); !resolved.empty()) {
      return std::string(resolved);
    }
  }
  return name_;
}
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

std::expected<std::vector<uint8_t>, std::string> Device::readSdo(uint16_t index,
                                                                 uint8_t subindex) const {
  return driver_.readSdo(slavePosition_, index, subindex);
}

std::expected<void, std::string> Device::writeSdo(uint16_t index, uint8_t subindex,
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

std::expected<void, std::string> Device::initializeParameters(bool readValues,
                                                              bool useCompleteAccess) {
  // 1. Obtain the parameter *definitions*: a cache hit (no bus I/O), or a live SDO-Info
  //    enumeration. The cache holds the static schema only — never live values — so on a hit each
  //    definition's value is the type default and is filled in by the value-read pass below.
  std::optional<std::vector<DeviceParameter>> definitions;
  if (parameterCache_) {
    definitions = parameterCache_->load(vendorId_, productCode_, revisionNumber_);
  }
  if (!definitions) {
    auto entries = driver_.readObjectDictionary(slavePosition_);
    if (!entries) {
      return std::unexpected(entries.error());
    }
    std::vector<DeviceParameter> built;
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

      auto decodeRawBytes = [&](const std::optional<std::vector<uint8_t>>& raw)
          -> std::optional<DeviceParameterValue> {
        if (!raw) {
          return std::nullopt;
        }
        auto decoded = decodeSdoBytes(e.dataType, *raw);
        if (!decoded) {
          spdlog::warn("Device {}: decode 0x{:04X}:{:02X} bound failed: {}", slavePosition_,
                       e.index, e.subindex, decoded.error());
          return std::nullopt;
        }
        return std::move(*decoded);
      };
      p.defaultValue = decodeRawBytes(e.defaultValue);
      p.minValue = decodeRawBytes(e.minValue);
      p.maxValue = decodeRawBytes(e.maxValue);

      built.push_back(std::move(p));
    }
    // Persist the freshly enumerated definitions before reading values, so the (possibly slow)
    // value pass below is never on the cache-population critical path.
    if (parameterCache_) {
      parameterCache_->store(vendorId_, productCode_, revisionNumber_, built);
    }
    definitions = std::move(built);
  }

  // 2. Fill in live values (if requested). The per-entry uploads are slow and must not hold the
  //    cache lock (that would stall a concurrent sampler cached-read for the whole enumeration), so
  //    mutate the local `definitions` and swap the built map in under the lock in one move at the
  //    end. Every value starts at its type default / Unknown; a successful read overwrites it.
  auto& defs = *definitions;
  for (auto& p : defs) {
    p.value = defaultValueForDataType(p.dataType);
    p.syncState = SyncState::Unknown;
  }
  if (readValues) {
    readParameterValues(defs, useCompleteAccess);
  }

  std::unordered_map<uint32_t, DeviceParameter> built;
  built.reserve(defs.size());
  for (auto& p : defs) {
    built.emplace(p.key(), std::move(p));
  }

  // Publish the freshly-built map in one move under the lock.
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  parameters_ = std::move(built);
  return {};
}

namespace {

// CoE object codes (ETG.1000-6 §5). Only ARRAY and RECORD have multiple subindices worth reading
// as one Complete Access transfer; a VAR is a single value and gains nothing.
constexpr uint16_t kOtypeArray = 0x0008;
constexpr uint16_t kOtypeRecord = 0x0009;

// Whether a run of an object's subindex entries (in subindex order) can be read as one Complete
// Access transfer: a multi-subindex ARRAY/RECORD, contiguous from subindex 0, with every entry
// byte-aligned. Contiguity + byte-alignment let the blob be sliced by walking bit offsets, never
// guessing at sub-byte packing or a gap left by a missing subindex.
bool completeAccessEligible(std::span<DeviceParameter* const> subs) {
  if (subs.size() < 2) {
    return false;
  }
  const uint16_t objectCode = subs.front()->objectCode;
  if (objectCode != kOtypeArray && objectCode != kOtypeRecord) {
    return false;
  }
  for (size_t k = 0; k < subs.size(); ++k) {
    const DeviceParameter* p = subs[k];
    if (p->subindex != static_cast<uint8_t>(k) || p->bitLength == 0 || (p->bitLength % 8u) != 0) {
      return false;
    }
  }
  return true;
}

// Decodes a Complete Access blob into @p subs (subindex order), assigning each entry's value and
// marking it synced. The blob layout (ETG): subindex 0 as a 16-bit value (1 data byte + 1 alignment
// pad), then subindices 1..N concatenated at their native bit lengths. Callers guarantee the run is
// eligible (see @c completeAccessEligible). Returns false (leaving any partially-applied values to
// be overwritten by the per-subindex fallback) if the blob is shorter than the layout implies or
// any entry fails to decode.
bool decodeCompleteAccess(std::span<DeviceParameter* const> subs,
                          const std::vector<uint8_t>& blob) {
  const uint32_t totalBits = static_cast<uint32_t>(blob.size()) * 8u;
  uint32_t cursor = 0;
  for (size_t k = 0; k < subs.size(); ++k) {
    DeviceParameter* p = subs[k];
    if (cursor + p->bitLength > totalBits) {
      return false;
    }
    const std::vector<uint8_t> slice =
        extractBits(std::span<const uint8_t>(blob.data(), blob.size()), cursor, p->bitLength);
    auto decoded = decodeSdoBytes(p->dataType, slice);
    if (!decoded) {
      return false;
    }
    p->value = std::move(*decoded);
    p->syncState = SyncState::Synced;
    // Subindex 0 occupies a padded 16-bit slot; every later entry follows at its native width.
    cursor += (k == 0) ? 16u : p->bitLength;
  }
  return true;
}

// Tracks Complete Access support across one read pass: probe once, then per-object fallback. CA is
// optional in CoE, so the first eligible object is a probe — if the slave rejects it, CA is
// disabled for the whole pass. After a CA read has succeeded, a later per-object failure falls back
// for that object only (support can vary per object).
class CompleteAccessProbe {
 public:
  explicit CompleteAccessProbe(bool useCompleteAccess)
      : state_(useCompleteAccess ? State::Unknown : State::Unsupported) {}
  // Whether to attempt CA for the next object (false once a probe proved it unsupported).
  bool enabled() const { return state_ != State::Unsupported; }
  void recordSuccess() { state_ = State::Supported; }
  // Records a failed CA read. Returns true when this failure disabled CA pass-wide (the probe),
  // false when it is a per-object failure after CA had already worked — the caller logs
  // accordingly.
  bool recordFailure() {
    if (state_ == State::Unknown) {
      state_ = State::Unsupported;
      return true;
    }
    return false;
  }

 private:
  enum class State { Unknown, Supported, Unsupported };
  State state_;
};

// Reads one object via a single Complete Access upload and decodes it into @p subs (subindex
// order), updating the probe. Returns true if the object was filled; false means the caller should
// fall back to per-subindex reads. Logging is keyed off the probe so an unsupported slave logs once
// rather than per object. @p subs must be eligible (see @c completeAccessEligible).
bool readCompleteInto(mm::comm::FieldbusDriver& driver, uint16_t slavePosition,
                      std::span<DeviceParameter* const> subs, CompleteAccessProbe& ca) {
  const uint16_t index = subs.front()->index;
  auto blob = driver.readSdoComplete(slavePosition, index);
  if (!blob) {
    if (ca.recordFailure()) {
      spdlog::debug("Device {}: complete access unsupported ({}); using per-subindex reads",
                    slavePosition, blob.error());
    } else {
      spdlog::debug("Device {}: complete-access read of 0x{:04X} failed ({}); per-subindex",
                    slavePosition, index, blob.error());
    }
    return false;
  }
  ca.recordSuccess();
  if (decodeCompleteAccess(subs, *blob)) {
    return true;
  }
  spdlog::warn("Device {}: complete-access layout for 0x{:04X} inconsistent; per-subindex reads",
               slavePosition, index);
  return false;
}

}  // namespace

void Device::readParameterValues(std::vector<DeviceParameter>& defs, bool useCompleteAccess) {
  // Reads one subindex the classic way: an individual SDO upload, decoded in place. Used for VARs,
  // for objects whose layout is not Complete-Access-decodable, and as the fallback when a CA read
  // or decode fails.
  auto readOne = [&](DeviceParameter& p) {
    auto bytes = readSdo(p.index, p.subindex);
    if (!bytes) {
      spdlog::warn("Device {}: SDO upload 0x{:04X}:{:02X} failed: {}", slavePosition_, p.index,
                   p.subindex, bytes.error());
      return;
    }
    auto decoded = decodeSdoBytes(p.dataType, *bytes);
    if (!decoded) {
      spdlog::warn("Device {}: decode 0x{:04X}:{:02X} failed: {}", slavePosition_, p.index,
                   p.subindex, decoded.error());
      return;
    }
    p.value = std::move(*decoded);
    p.syncState = SyncState::Synced;
  };

  // Complete Access support is discovered once per device (the "probe"): the first eligible object
  // is read with CA; if the slave rejects it (SDO abort), CA is disabled for the rest of this pass
  // and everything falls back to per-subindex reads. Once a CA read has succeeded we keep using it,
  // but a per-object failure only falls back for that object (support can vary per object).
  CompleteAccessProbe ca(useCompleteAccess);

  for (size_t start = 0; start < defs.size();) {
    // Group the contiguous run of entries sharing this object index.
    size_t end = start + 1;
    while (end < defs.size() && defs[end].index == defs[start].index) {
      ++end;
    }
    std::vector<DeviceParameter*> subs;
    subs.reserve(end - start);
    for (size_t k = start; k < end; ++k) {
      subs.push_back(&defs[k]);
    }

    if (!(ca.enabled() && completeAccessEligible(subs) &&
          readCompleteInto(driver_, slavePosition_, subs, ca))) {
      for (DeviceParameter* p : subs) {
        readOne(*p);
      }
    }
    start = end;
  }
}

const std::unordered_map<uint32_t, DeviceParameter>& Device::parameters() const {
  return parameters_;
}

namespace {

// EtherCAT SDO abort code for "object does not exist in the object dictionary" (ETG.1000.6
// Table 39). The FieldbusDriver surfaces SDO failures only as text (no structured abort code
// crosses the interface boundary yet — see the "error strings throughout for now" note in
// CLAUDE.md), so we match the formatted code the driver appends to its message; the format is
// fixed at "0x{:08X}" in SoemFieldbusDriver::readSdo.
constexpr uint32_t kSdoAbortObjectDoesNotExist = 0x06020000u;

bool isObjectDoesNotExistAbort(const std::string& error) {
  return error.find(std::format("0x{:08X}", kSdoAbortObjectDoesNotExist)) != std::string::npos;
}

}  // namespace

std::expected<std::vector<PdoMappingObject>, std::string> Device::readPdoAssignment(
    uint16_t assignmentIndex) {
  std::vector<PdoMappingObject> objects;
  uint32_t totalBits = 0;

  // Subindex 0 of the assignment object is the count of assigned PDO mapping objects. A
  // device with no PDOs in this direction may not implement the object at all — treat a
  // failed read (or a zero count) as "no entries here", which is not an error.
  auto countBytes = readSdo(assignmentIndex, 0);
  if (!countBytes) {
    return objects;
  }
  const uint8_t pdoCount = core::fromBytes<uint8_t>(*countBytes);

  // The loop counter is wider than pdoCount's uint8_t on purpose: a device reporting 255 here
  // would make a `uint8_t i <= 255` guard permanently true (i wraps 255->0), spinning forever.
  // The subindex argument is narrowed back to uint8_t at the call.
  for (unsigned i = 1; i <= pdoCount; ++i) {
    auto pdoIndexBytes = readSdo(assignmentIndex, static_cast<uint8_t>(i));
    if (!pdoIndexBytes) {
      return std::unexpected(std::format("0x{:04X}:{:02X} (PDO assignment) read failed: {}",
                                         assignmentIndex, i, pdoIndexBytes.error()));
    }
    const uint16_t mappingIndex = core::fromBytes<uint16_t>(*pdoIndexBytes);
    if (mappingIndex == 0) {
      continue;  // unused assignment slot
    }

    // Subindex 0 of the mapping object is the count of mapped entries.
    auto entryCountBytes = readSdo(mappingIndex, 0);
    if (!entryCountBytes) {
      // An assignment slot may point at a mapping object the firmware does not implement in its
      // CoE dictionary. TwinCAT-generated ESIs append an alignment-padding PDO (e.g. 0x1701) to
      // round a SyncManager's PDO set up to a 32-bit boundary and describe it only in the ESI's
      // MDP section — never as a real object — so an SDO upload aborts with "object does not
      // exist" (0x06020000). That is a legitimate, ESI-configured padding reference, not a fault:
      // an ESI-driven master takes the mapping from the XML and never reads the pad over CoE. Skip
      // it. SOEM already accounts for the pad bytes in the SM window length (it falls back to the
      // SII/SM-register size when its own CoE mapping read fails), and buildProcessImage derives
      // byte boundaries from that driver layout, so the padding needs no entry in our logical view.
      //
      // This skip is correct only because such padding is *trailing* — the last assignment slot,
      // as 0x1C12:05 is here — so dropping it shifts no real object's bit offset. If a future
      // (modular) device ever placed an alignment pad *between* real PDOs, skipping it without
      // advancing `totalBits` would shift every later entry's offset; handling that would mean
      // reconciling our summed bits against SOEM's per-slave Obits/Ibits (the true mapped totals)
      // or reading the pad's bit size from the SII PDO category (parseSii decodes it). Neither is
      // needed today, so we deliberately do not implement it.
      if (isObjectDoesNotExistAbort(entryCountBytes.error())) {
        spdlog::debug(
            "Device {}: PDO assignment 0x{:04X}:{:02X} references mapping object 0x{:04X} which "
            "the "
            "device does not implement (SDO abort 0x{:08X}) — treating as alignment padding, "
            "skipping",
            slavePosition_, assignmentIndex, i, mappingIndex, kSdoAbortObjectDoesNotExist);
        continue;
      }
      return std::unexpected(std::format("0x{:04X}:00 (PDO mapping) read failed: {}", mappingIndex,
                                         entryCountBytes.error()));
    }
    const uint8_t entryCount = core::fromBytes<uint8_t>(*entryCountBytes);

    // Wider counter for the same reason as the outer loop: entryCount == 255 must still terminate.
    PdoMappingObject obj;
    obj.pdoIndex = mappingIndex;
    for (unsigned e = 1; e <= entryCount; ++e) {
      auto entryBytes = readSdo(mappingIndex, static_cast<uint8_t>(e));
      if (!entryBytes) {
        return std::unexpected(std::format("0x{:04X}:{:02X} (PDO mapping entry) read failed: {}",
                                           mappingIndex, e, entryBytes.error()));
      }
      PdoMappingEntry entry = unpackMappingEntry(core::fromBytes<uint32_t>(*entryBytes));
      entry.bitOffset = totalBits;  // derived from the running offset, not the packed word
      totalBits += entry.bitLength;
      obj.entries.push_back(entry);
    }
    objects.push_back(std::move(obj));
  }
  return objects;
}

std::expected<PdoMapping, std::string> Device::readPdoMapping() {
  PdoMapping mapping;
  auto outputs = readPdoAssignment(0x1C12);
  if (!outputs) {
    return std::unexpected(outputs.error());
  }
  auto inputs = readPdoAssignment(0x1C13);
  if (!inputs) {
    return std::unexpected(inputs.error());
  }
  mapping.outputs = std::move(*outputs);
  mapping.inputs = std::move(*inputs);
  return mapping;
}

std::expected<void, std::string> Device::readFlatPdoMapping() {
  // The flat view is the grouped mapping with object boundaries dropped: entries keep their
  // (already derived) bitOffset, and each direction's total is the sum of its entry widths.
  auto grouped = readPdoMapping();
  if (!grouped) {
    return std::unexpected(grouped.error());
  }
  FlatPdoMapping flat;
  for (const auto& obj : grouped->outputs) {
    for (const auto& e : obj.entries) {
      flat.outputs.push_back(e);
      flat.outputBits += e.bitLength;
    }
  }
  for (const auto& obj : grouped->inputs) {
    for (const auto& e : obj.entries) {
      flat.inputs.push_back(e);
      flat.inputBits += e.bitLength;
    }
  }
  flatPdoMapping_ = std::move(flat);
  spdlog::debug("Device {}: PDO mapping - {} output entries ({} bits), {} input entries ({} bits)",
                slavePosition_, flatPdoMapping_.outputs.size(), flatPdoMapping_.outputBits,
                flatPdoMapping_.inputs.size(), flatPdoMapping_.inputBits);
  return {};
}

const FlatPdoMapping& Device::flatPdoMapping() const { return flatPdoMapping_; }

namespace {

// Confirms one direction's read-back mapping matches the request exactly — same entries
// (index/subindex/bitLength) in the same order. @p dir names the direction for the error message.
std::expected<void, std::string> matchesRequest(const std::vector<PdoMappingEntry>& readBack,
                                                const std::vector<PdoMappingObject>& requested,
                                                std::string_view dir) {
  size_t r = 0;
  for (const auto& obj : requested) {
    for (const auto& want : obj.entries) {
      if (r >= readBack.size()) {
        return std::unexpected(
            std::format("{} mapping: device reports fewer entries ({}) than the "
                        "{} written",
                        dir, readBack.size(), r + 1));
      }
      const PdoMappingEntry& got = readBack[r];
      if (got.index != want.index || got.subindex != want.subindex ||
          got.bitLength != want.bitLength) {
        return std::unexpected(std::format(
            "{} mapping entry {}: wrote 0x{:04X}:{:02X}/{} bit(s) but read back 0x{:04X}:{:02X}/{} "
            "bit(s)",
            dir, r, want.index, want.subindex, want.bitLength, got.index, got.subindex,
            got.bitLength));
      }
      ++r;
    }
  }
  if (r != readBack.size()) {
    return std::unexpected(
        std::format("{} mapping: device reports more entries ({}) than the {} written", dir,
                    readBack.size(), r));
  }
  return {};
}

// Writes one direction's PDO mapping: clears the sync manager's assignment, rewrites the referenced
// mapping objects, then re-assigns them (see Device::writePdoMapping). @p assignmentIndex is
// 0x1C12 (outputs/RxPDO) or 0x1C13 (inputs/TxPDO). A free function taking the device rather than a
// member, as it is a pure implementation detail of writePdoMapping and needs only the public
// writeSdo.
std::expected<void, std::string> writePdoDirection(const Device& device, uint16_t assignmentIndex,
                                                   const std::vector<PdoMappingObject>& objects) {
  // 1. Clear the sync manager's PDO assignment count. With nothing assigned, the mapping objects it
  //    referenced become writable — a slave rejects a write to a mapping object while it is
  //    assigned to a sync manager. This also deassigns any object from the previous configuration
  //    that is absent from `objects`, so those need no separate clear.
  if (auto w = device.writeSdo(assignmentIndex, 0, core::toBytes<uint8_t>(0)); !w) {
    return std::unexpected(std::format("0x{:04X}:00 (PDO assignment clear) write failed: {}",
                                       assignmentIndex, w.error()));
  }

  // 2. Rewrite each mapping object: clear its entry count, write the packed entries to subindices
  //    1..N, then restore the entry count (the CoE order — count must be zero before entries are
  //    written).
  for (const PdoMappingObject& obj : objects) {
    if (auto w = device.writeSdo(obj.pdoIndex, 0, core::toBytes<uint8_t>(0)); !w) {
      return std::unexpected(
          std::format("0x{:04X}:00 (PDO mapping clear) write failed: {}", obj.pdoIndex, w.error()));
    }
    for (size_t e = 0; e < obj.entries.size(); ++e) {
      const uint32_t packed = packMappingEntry(obj.entries[e]);
      if (auto w =
              device.writeSdo(obj.pdoIndex, static_cast<uint8_t>(e + 1), core::toBytes(packed));
          !w) {
        return std::unexpected(std::format("0x{:04X}:{:02X} (PDO mapping entry) write failed: {}",
                                           obj.pdoIndex, e + 1, w.error()));
      }
    }
    if (auto w = device.writeSdo(obj.pdoIndex, 0,
                                 core::toBytes<uint8_t>(static_cast<uint8_t>(obj.entries.size())));
        !w) {
      return std::unexpected(
          std::format("0x{:04X}:00 (PDO mapping count) write failed: {}", obj.pdoIndex, w.error()));
    }
  }

  // 3. Assign the mapping objects to the sync manager (subindices 1..N in order), then write the
  //    assignment count last — enabling the sync manager with the new PDO set.
  for (size_t i = 0; i < objects.size(); ++i) {
    if (auto w = device.writeSdo(assignmentIndex, static_cast<uint8_t>(i + 1),
                                 core::toBytes(objects[i].pdoIndex));
        !w) {
      return std::unexpected(std::format("0x{:04X}:{:02X} (PDO assignment) write failed: {}",
                                         assignmentIndex, i + 1, w.error()));
    }
  }
  if (auto w = device.writeSdo(assignmentIndex, 0,
                               core::toBytes<uint8_t>(static_cast<uint8_t>(objects.size())));
      !w) {
    return std::unexpected(std::format("0x{:04X}:00 (PDO assignment count) write failed: {}",
                                       assignmentIndex, w.error()));
  }
  return {};
}

}  // namespace

std::expected<void, std::string> Device::writePdoMapping(const PdoMapping& mapping) {
  using mm::comm::EtherCatState;
  const EtherCatState state = mm::comm::alState(driver_.slaveState(slavePosition_));
  if (state != EtherCatState::PreOp) {
    return std::unexpected(
        std::format("device {}: PDO mapping can only be written in PRE-OP (device is in {})",
                    slavePosition_, mm::comm::toString(state)));
  }

  // The assignment count, each object's entry count, and every object/entry position are
  // single-byte SDO subindices, so at most 255 objects per sync manager and 255 entries per object.
  // (bitLength is already a uint8_t, so it cannot overflow the packed 7..0 field.)
  auto validate = [](const std::vector<PdoMappingObject>& objs,
                     std::string_view dir) -> std::expected<void, std::string> {
    if (objs.size() > 255) {
      return std::unexpected(
          std::format("too many {} PDO objects ({}, max 255)", dir, objs.size()));
    }
    const auto overfull = std::ranges::find_if(
        objs, [](const PdoMappingObject& o) { return o.entries.size() > 255; });
    if (overfull != objs.end()) {
      return std::unexpected(std::format("0x{:04X}: too many entries ({}, max 255)",
                                         overfull->pdoIndex, overfull->entries.size()));
    }
    return {};
  };
  if (auto r = validate(mapping.outputs, "output"); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = validate(mapping.inputs, "input"); !r) {
    return std::unexpected(r.error());
  }

  // Apply both directions, read the mapping back, and confirm it matches — retrying the whole
  // sequence on any transient SDO failure or mismatch. A single dropped mailbox frame mid-burst
  // would otherwise silently leave a half-written mapping that the later re-map would treat as
  // authoritative; the apply is idempotent, so re-running is safe.
  constexpr int kMaxAttempts = 3;
  std::string lastError;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    auto applied = [&]() -> std::expected<void, std::string> {
      if (auto r = writePdoDirection(*this, 0x1C12, mapping.outputs); !r) {
        return r;
      }
      if (auto r = writePdoDirection(*this, 0x1C13, mapping.inputs); !r) {
        return r;
      }
      // Read back (this also refreshes flatPdoMapping()) and verify it matches the request.
      if (auto r = readFlatPdoMapping(); !r) {
        return std::unexpected("read-back failed: " + r.error());
      }
      if (auto r = matchesRequest(flatPdoMapping_.outputs, mapping.outputs, "output"); !r) {
        return r;
      }
      return matchesRequest(flatPdoMapping_.inputs, mapping.inputs, "input");
    }();
    if (applied) {
      spdlog::info("Device {}: PDO mapping written ({} output object(s), {} input object(s))",
                   slavePosition_, mapping.outputs.size(), mapping.inputs.size());
      return {};
    }
    lastError = std::move(applied).error();
    spdlog::warn("Device {}: PDO mapping attempt {}/{} failed: {}", slavePosition_, attempt,
                 kMaxAttempts, lastError);
  }
  return std::unexpected(std::format("device {}: PDO mapping failed after {} attempts: {}",
                                     slavePosition_, kMaxAttempts, lastError));
}

std::expected<void, std::string> Device::setValue(uint16_t index, uint8_t subindex,
                                                  DeviceParameterValue newValue) {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  DeviceParameter* p = findParameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  if (auto set = p->setValue(std::move(newValue)); !set) {
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

std::optional<DeviceParameter> Device::parameterCopy(uint16_t index, uint8_t subindex) const {
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  const DeviceParameter* p = parameter(index, subindex);
  if (!p) {
    return std::nullopt;
  }
  return *p;
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
  auto bytes = readSdo(index, subindex);
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

std::expected<void, std::string> Device::readAllParameters(bool useCompleteAccess) {
  // Snapshot the readable subindices grouped by object index under the lock, then read with it
  // released — so this multi-mailbox sweep never holds parametersMutex_ across more than one
  // object's transfer (a concurrent cached read waits at most that). Write-only objects are
  // skipped: an SDO upload of one would abort and only add a spurious failure log.
  std::map<uint16_t, std::vector<uint8_t>> objects;
  {
    std::lock_guard<std::mutex> lock(*parametersMutex_);
    if (parameters_.empty()) {
      return std::unexpected(std::format(
          "device {}: no parameters loaded — initialise the parameter list first", slavePosition_));
    }
    for (const auto& [key, p] : parameters_) {
      if (p.isReadable()) {
        objects[p.index].push_back(p.subindex);
      }
    }
  }
  for (auto& [index, subindices] : objects) {
    std::ranges::sort(
        subindices);  // completeAccessEligible needs subindex order, contiguous from 0
  }

  // Fills one object via a single Complete Access upload, returning true on success. Runs the CA
  // read + decode under parametersMutex_ — one mailbox round-trip, matching readParameter's own
  // lock contract — so the re-found parameter pointers stay valid across the transfer.
  CompleteAccessProbe ca(useCompleteAccess);
  auto tryComplete = [&](uint16_t index, const std::vector<uint8_t>& subindices) -> bool {
    std::lock_guard<std::mutex> lock(*parametersMutex_);
    std::vector<DeviceParameter*> subs;
    subs.reserve(subindices.size());
    for (uint8_t si : subindices) {
      DeviceParameter* p = findParameter(index, si);
      if (!p) {
        return false;  // map re-enumerated under us — fall back to the per-subindex path
      }
      subs.push_back(p);
    }
    if (!completeAccessEligible(subs)) {
      return false;
    }
    // Prefer the live process image while exchanging: if any subindex is PDO-served, let the
    // per-subindex path (readParameter) read it from the image rather than issuing an SDO here.
    if (processData_ && exchangesProcessData()) {
      const bool anyImageServed = std::ranges::any_of(subs, [&](const DeviceParameter* p) {
        return processData_->readPdo(slavePosition_, index, p->subindex).has_value();
      });
      if (anyImageServed) {
        return false;
      }
    }
    return readCompleteInto(driver_, slavePosition_, subs, ca);
  };

  // Best-effort, like initializeParameters(readValues=true): a per-entry failure keeps that entry's
  // cached value and is logged, and the sweep still succeeds so one bad object never blocks the
  // rest. Multi-subindex objects try Complete Access first; single-subindex objects and any CA
  // fallthrough go through the PDO-aware per-subindex readParameter.
  for (const auto& [index, subindices] : objects) {
    if (subindices.size() >= 2 && ca.enabled() && tryComplete(index, subindices)) {
      continue;
    }
    for (uint8_t si : subindices) {
      if (auto r = readParameter(index, si); !r) {
        spdlog::warn("Device {}: read 0x{:04X}:{:02X} failed: {}", slavePosition_, index, si,
                     r.error());
      }
    }
  }
  return {};
}

std::expected<void, std::string> Device::writeParameter(uint16_t index, uint8_t subindex,
                                                        DeviceParameterValue newValue) {
  // Held across the download below (one mailbox round-trip), like readParameter.
  std::lock_guard<std::mutex> lock(*parametersMutex_);
  DeviceParameter* p = findParameter(index, subindex);
  if (!p) {
    return std::unexpected(std::format("device {}: parameter 0x{:04X}:{:02X} not found",
                                       slavePosition_, index, subindex));
  }
  // Cache-first: coerce and store into the cached parameter before any bus access, so the
  // cache always reflects the latest intended value regardless of online state.
  if (auto set = p->setValue(newValue); !set) {
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
  if (auto w = writeSdo(index, subindex, *bytes); !w) {
    p->syncState = SyncState::Pending;
    return std::unexpected(w.error());
  }
  p->syncState = SyncState::Synced;
  return {};
}

void to_json(nlohmann::json& j, const Device& d) {
  j = nlohmann::json{
      {"slavePosition", d.slavePosition()}, {"name", d.name()},
      {"productName", d.productName()},     {"vendorId", d.vendorId()},
      {"productCode", d.productCode()},     {"revisionNumber", d.revisionNumber()},
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
  auto count = device.readSdo(kDetectedModuleIdentList, 0x00);
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
    auto detected = device.readSdo(kDetectedModuleIdentList, static_cast<uint8_t>(sub));
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
    auto configured = device.readSdo(kConfiguredModuleIdentList, static_cast<uint8_t>(sub));
    if (configured && *configured == *detected) {
      continue;
    }
    if (auto w = device.writeSdo(kConfiguredModuleIdentList, static_cast<uint8_t>(sub), *detected);
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
