#include "comm/sii.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mm::comm {

namespace {

// Little-endian field readers over a byte span. Each returns 0 when the requested field would run
// past the end of the span, so a truncated category can never read out of bounds.
uint8_t rdU8(std::span<const uint8_t> b, size_t off) { return off < b.size() ? b[off] : 0; }

uint16_t rdU16(std::span<const uint8_t> b, size_t off) {
  if (off + 2 > b.size()) {
    return 0;
  }
  return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

int16_t rdI16(std::span<const uint8_t> b, size_t off) {
  return static_cast<int16_t>(rdU16(b, off));
}

uint32_t rdU32(std::span<const uint8_t> b, size_t off) {
  if (off + 4 > b.size()) {
    return 0;
  }
  return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

constexpr size_t kHeaderBytes = 128;

SiiInfo parseHeader(std::span<const uint8_t> b) {
  // Byte offsets per ETG.1000.6 §5.4; reserved gaps at 10–13, 32–39, 58–123 are skipped.
  return SiiInfo{
      .pdiControl = rdU16(b, 0),
      .pdiConfiguration = rdU16(b, 2),
      .syncImpulseLen = rdU16(b, 4),
      .pdiConfiguration2 = rdU16(b, 6),
      .configuredStationAlias = rdU16(b, 8),
      .checksum = rdU16(b, 14),
      .vendorId = rdU32(b, 16),
      .productCode = rdU32(b, 20),
      .revisionNumber = rdU32(b, 24),
      .serialNumber = rdU32(b, 28),
      .bootstrapReceiveMailboxOffset = rdU16(b, 40),
      .bootstrapReceiveMailboxSize = rdU16(b, 42),
      .bootstrapSendMailboxOffset = rdU16(b, 44),
      .bootstrapSendMailboxSize = rdU16(b, 46),
      .standardReceiveMailboxOffset = rdU16(b, 48),
      .standardReceiveMailboxSize = rdU16(b, 50),
      .standardSendMailboxOffset = rdU16(b, 52),
      .standardSendMailboxSize = rdU16(b, 54),
      .mailboxProtocol = rdU16(b, 56),
      .size = rdU16(b, 124),
      .version = rdU16(b, 126),
  };
}

std::vector<std::string> parseStrings(std::span<const uint8_t> p) {
  std::vector<std::string> strings;
  if (p.empty()) {
    return strings;
  }
  const uint8_t count = p[0];
  size_t cursor = 1;
  for (uint8_t i = 0; i < count; ++i) {
    if (cursor >= p.size()) {
      break;
    }
    const uint8_t len = p[cursor++];
    const size_t take = std::min<size_t>(len, p.size() - cursor);
    strings.emplace_back(reinterpret_cast<const char*>(p.data() + cursor), take);
    cursor += len;
  }
  return strings;
}

SiiCategoryGeneral parseGeneral(std::span<const uint8_t> p) {
  // ETG.2000 General-category layout (payload-relative): a reserved byte sits at offset 4 between
  // nameIdx and the mailbox-detail bytes, so CoE Details is at offset 5 — not 4, as the original
  // TypeScript parser had it (which shifted every field from coeDetails on down by one, leaving
  // coeDetails reading the reserved 0x00). SOEM confirms the layout: it reads CoEdetails at
  // siifind()+0x07, and siifind returns the category's size-word address, so its payload base is
  // +2 → CoE at payload offset 5. currentOnEBus and physicalMemoryAddress are 16-bit words.
  return SiiCategoryGeneral{
      .groupIdx = rdU8(p, 0),
      .imgIdx = rdU8(p, 1),
      .orderIdx = rdU8(p, 2),
      .nameIdx = rdU8(p, 3),
      // offset 4: reserved
      .coeDetails = rdU8(p, 5),
      .foeDetails = rdU8(p, 6),
      .eoeDetails = rdU8(p, 7),
      .soeChannels = rdU8(p, 8),
      .ds402Channels = rdU8(p, 9),
      .sysmanClass = rdU8(p, 10),
      .flags = rdU8(p, 11),
      .currentOnEBus = rdU16(p, 12),
      .physicalPort = rdI16(p, 14),
      .physicalMemoryAddress = rdU16(p, 16),
  };
}

std::vector<uint16_t> parseFmmu(std::span<const uint8_t> p) {
  std::vector<uint16_t> fmmus;
  for (size_t i = 0; i + 2 <= p.size(); i += 2) {
    fmmus.push_back(rdU16(p, i));
  }
  return fmmus;
}

std::vector<SiiCategorySyncManagerElement> parseSyncManagers(std::span<const uint8_t> p) {
  std::vector<SiiCategorySyncManagerElement> elements;
  for (size_t i = 0; i + 8 <= p.size(); i += 8) {
    elements.push_back(SiiCategorySyncManagerElement{
        .physicalStartAddress = rdU16(p, i),
        .length = rdU16(p, i + 2),
        .controlRegister = rdU8(p, i + 4),
        .statusRegister = rdU8(p, i + 5),
        .enableSyncManager = rdU8(p, i + 6),
        .syncManagerType = rdU8(p, i + 7),
    });
  }
  return elements;
}

std::vector<SiiCategoryPdoElement> parsePdo(std::span<const uint8_t> p) {
  std::vector<SiiCategoryPdoElement> pdos;
  size_t offset = 0;
  while (offset + 8 <= p.size()) {
    SiiCategoryPdoElement pdo{
        .pdoIndex = rdU16(p, offset),
        .nEntry = rdU8(p, offset + 2),
        .syncM = rdU8(p, offset + 3),
        .synchronization = rdU8(p, offset + 4),
        .nameIdx = rdU8(p, offset + 5),
        .flags = rdU16(p, offset + 6),
        .entries = {},
    };
    offset += 8;
    for (uint8_t i = 0; i < pdo.nEntry; ++i) {
      if (offset + 8 > p.size()) {
        break;  // Truncated payload: keep nEntry but stop emitting entries.
      }
      pdo.entries.push_back(SiiCategoryPdoEntryElement{
          .entryIndex = rdU16(p, offset),
          .subindex = rdU8(p, offset + 2),
          .entryNameIdx = rdU8(p, offset + 3),
          .dataType = rdU8(p, offset + 4),
          .bitLen = rdU8(p, offset + 5),
          .flags = rdU16(p, offset + 6),
      });
      offset += 8;
    }
    pdos.push_back(std::move(pdo));
  }
  return pdos;
}

std::vector<SiiCategoryDistributedClockElement> parseDc(std::span<const uint8_t> p) {
  std::vector<SiiCategoryDistributedClockElement> elements;
  for (size_t i = 0; i + 24 <= p.size(); i += 24) {
    elements.push_back(SiiCategoryDistributedClockElement{
        .cycleTime0 = rdU32(p, i),
        .shiftTime0 = rdU32(p, i + 4),
        .shiftTime1 = rdU32(p, i + 8),
        .sync1CycleFactor = rdI16(p, i + 12),
        .assignActivate = rdU16(p, i + 14),
        .sync0CycleFactor = rdI16(p, i + 16),
        .nameIdx = rdU8(p, i + 18),
        .descIdx = rdU8(p, i + 19),
    });
  }
  return elements;
}

}  // namespace

std::optional<SiiCategoryType> resolveSiiCategoryType(uint16_t value) {
  if (value >= 1 && value <= 9) {
    return SiiCategoryType::DeviceSpecific;
  }
  if (value >= 0x0800 && value <= 0x1FFF) {
    return SiiCategoryType::VendorSpecific;
  }
  if (value >= 0x2000 && value <= 0x2FFF) {
    return SiiCategoryType::ApplicationSpecific;
  }
  if (value >= 0x3000 && value <= 0xFFFE) {
    return SiiCategoryType::VendorSpecific;
  }
  switch (static_cast<SiiCategoryType>(value)) {
    case SiiCategoryType::Nop:
    case SiiCategoryType::Strings:
    case SiiCategoryType::DataTypes:
    case SiiCategoryType::General:
    case SiiCategoryType::Fmmu:
    case SiiCategoryType::SyncM:
    case SiiCategoryType::FmmuX:
    case SiiCategoryType::SyncUnit:
    case SiiCategoryType::TxPdo:
    case SiiCategoryType::RxPdo:
    case SiiCategoryType::Dc:
    case SiiCategoryType::Timeouts:
    case SiiCategoryType::Dictionary:
    case SiiCategoryType::Hardware:
    case SiiCategoryType::VendorInformation:
    case SiiCategoryType::Images:
    case SiiCategoryType::End:
      return static_cast<SiiCategoryType>(value);
    // DeviceSpecific / VendorSpecific / ApplicationSpecific are handled by the ranges above.
    case SiiCategoryType::DeviceSpecific:
    case SiiCategoryType::VendorSpecific:
    case SiiCategoryType::ApplicationSpecific:
      break;
  }
  return std::nullopt;
}

std::expected<SlaveInformationInterface, std::string> parseSii(std::span<const uint8_t> buffer) {
  if (buffer.size() < kHeaderBytes) {
    return std::unexpected(std::string("SII image too small: ") + std::to_string(buffer.size()) +
                           " bytes (need at least 128 for the fixed header)");
  }

  SlaveInformationInterface sii;
  sii.info = parseHeader(buffer);

  std::span<const uint8_t> categories = buffer.subspan(kHeaderBytes);
  size_t offset = 0;
  while (offset + 4 <= categories.size()) {
    const uint16_t typeValue = rdU16(categories, offset);
    if (typeValue == static_cast<uint16_t>(SiiCategoryType::End)) {
      break;
    }
    const uint16_t wordSize = rdU16(categories, offset + 2);
    const std::optional<SiiCategoryType> type = resolveSiiCategoryType(typeValue);

    // Payload is wordSize 16-bit words, clamped to whatever remains in the buffer.
    const size_t payloadStart = offset + 4;
    const size_t declared = static_cast<size_t>(wordSize) * 2;
    const size_t available = categories.size() - payloadStart;
    const std::span<const uint8_t> payload =
        categories.subspan(payloadStart, std::min(declared, available));

    if (type == SiiCategoryType::Strings) {
      sii.category.strings = parseStrings(payload);
    } else if (type == SiiCategoryType::General) {
      sii.category.general = parseGeneral(payload);
    } else if (type == SiiCategoryType::Fmmu) {
      sii.category.fmmus = parseFmmu(payload);
    } else if (type == SiiCategoryType::SyncM) {
      sii.category.syncManagers = parseSyncManagers(payload);
    } else if (type == SiiCategoryType::RxPdo) {
      auto pdos = parsePdo(payload);
      sii.category.rxPdos.insert(sii.category.rxPdos.end(), pdos.begin(), pdos.end());
    } else if (type == SiiCategoryType::TxPdo) {
      auto pdos = parsePdo(payload);
      sii.category.txPdos.insert(sii.category.txPdos.end(), pdos.begin(), pdos.end());
    } else if (type == SiiCategoryType::Dc) {
      sii.category.distributedClocks = parseDc(payload);
    }

    if (type == SiiCategoryType::Nop || type == SiiCategoryType::End) {
      break;
    }
    offset = payloadStart + declared;
  }

  return sii;
}

void to_json(nlohmann::json& j, const SiiInfo& v) {
  j = nlohmann::json{
      {"pdiControl", v.pdiControl},
      {"pdiConfiguration", v.pdiConfiguration},
      {"syncImpulseLen", v.syncImpulseLen},
      {"pdiConfiguration2", v.pdiConfiguration2},
      {"configuredStationAlias", v.configuredStationAlias},
      {"checksum", v.checksum},
      {"vendorId", v.vendorId},
      {"productCode", v.productCode},
      {"revisionNumber", v.revisionNumber},
      {"serialNumber", v.serialNumber},
      {"bootstrapReceiveMailboxOffset", v.bootstrapReceiveMailboxOffset},
      {"bootstrapReceiveMailboxSize", v.bootstrapReceiveMailboxSize},
      {"bootstrapSendMailboxOffset", v.bootstrapSendMailboxOffset},
      {"bootstrapSendMailboxSize", v.bootstrapSendMailboxSize},
      {"standardReceiveMailboxOffset", v.standardReceiveMailboxOffset},
      {"standardReceiveMailboxSize", v.standardReceiveMailboxSize},
      {"standardSendMailboxOffset", v.standardSendMailboxOffset},
      {"standardSendMailboxSize", v.standardSendMailboxSize},
      {"mailboxProtocol", v.mailboxProtocol},
      {"size", v.size},
      {"version", v.version},
  };
}

void to_json(nlohmann::json& j, const SiiCategoryGeneral& v) {
  j = nlohmann::json{
      {"groupIdx", v.groupIdx},
      {"imgIdx", v.imgIdx},
      {"orderIdx", v.orderIdx},
      {"nameIdx", v.nameIdx},
      {"coeDetails", v.coeDetails},
      {"foeDetails", v.foeDetails},
      {"eoeDetails", v.eoeDetails},
      {"soeChannels", v.soeChannels},
      {"ds402Channels", v.ds402Channels},
      {"sysmanClass", v.sysmanClass},
      {"flags", v.flags},
      {"currentOnEBus", v.currentOnEBus},
      {"physicalPort", v.physicalPort},
      {"physicalMemoryAddress", v.physicalMemoryAddress},
  };
}

void to_json(nlohmann::json& j, const SiiCategorySyncManagerElement& v) {
  j = nlohmann::json{
      {"physicalStartAddress", v.physicalStartAddress}, {"length", v.length},
      {"controlRegister", v.controlRegister},           {"statusRegister", v.statusRegister},
      {"enableSyncManager", v.enableSyncManager},       {"syncManagerType", v.syncManagerType},
  };
}

void to_json(nlohmann::json& j, const SiiCategoryPdoEntryElement& v) {
  j = nlohmann::json{
      {"entryIndex", v.entryIndex}, {"subindex", v.subindex}, {"entryNameIdx", v.entryNameIdx},
      {"dataType", v.dataType},     {"bitLen", v.bitLen},     {"flags", v.flags},
  };
}

void to_json(nlohmann::json& j, const SiiCategoryPdoElement& v) {
  j = nlohmann::json{
      {"pdoIndex", v.pdoIndex}, {"nEntry", v.nEntry},
      {"syncM", v.syncM},       {"synchronization", v.synchronization},
      {"nameIdx", v.nameIdx},   {"flags", v.flags},
      {"entries", v.entries},
  };
}

void to_json(nlohmann::json& j, const SiiCategoryDistributedClockElement& v) {
  j = nlohmann::json{
      {"cycleTime0", v.cycleTime0},
      {"shiftTime0", v.shiftTime0},
      {"shiftTime1", v.shiftTime1},
      {"sync1CycleFactor", v.sync1CycleFactor},
      {"assignActivate", v.assignActivate},
      {"sync0CycleFactor", v.sync0CycleFactor},
      {"nameIdx", v.nameIdx},
      {"descIdx", v.descIdx},
  };
}

void to_json(nlohmann::json& j, const SiiCategorySection& v) {
  j = nlohmann::json{
      {"strings", v.strings},
      {"general", v.general},
      {"fmmus", v.fmmus},
      {"syncManagers", v.syncManagers},
      {"rxPdos", v.rxPdos},
      {"txPdos", v.txPdos},
      {"distributedClocks", v.distributedClocks},
  };
}

void to_json(nlohmann::json& j, const SlaveInformationInterface& v) {
  j = nlohmann::json{{"info", v.info}, {"category", v.category}};
}

}  // namespace mm::comm
