#include "comm/sii.h"

#include <algorithm>
#include <cstddef>
#include <format>
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
  // Byte offsets per ETG.2010 Table 2 (SII Area); reserved gaps at 10–13, 32–39, 58–123 are
  // skipped.
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
  // ETG.2010 Table 7 General-category layout (payload-relative). A reserved byte
  // sits at offset 4 between nameIdx and the mailbox-detail bytes, so CoE Details is at offset 5 —
  // not 4, as the original TypeScript parser had it (which shifted every field from coeDetails on
  // down by one, leaving coeDetails reading the reserved 0x00). SOEM confirms this base: it reads
  // CoEdetails at siifind()+0x07 and Ebuscurrent at siifind()+0x0e, and siifind returns the
  // category's size-word address, so its payload base is +2 → CoE at payload offset 5 and
  // currentOnEBus (Integer16) at payload offset 12. A further 2-byte gap follows currentOnEBus — a
  // duplicate GroupIdx (0x0e) and a reserved byte (0x0f) — so Physical Port is at offset 16 and
  // Physical Memory Address at offset 18, both Unsigned16.
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
      .currentOnEBus = rdI16(p, 12),
      // offset 14: duplicate GroupIdx; offset 15: reserved
      .physicalPort = rdU16(p, 16),
      .physicalMemoryAddress = rdU16(p, 18),
  };
}

std::vector<uint8_t> parseFmmu(std::span<const uint8_t> p) {
  // ETG.2010 Table 9: one Unsigned8 per FMMU (0x00/0xFF unused, 0x01 Outputs, 0x02 Inputs,
  // 0x03 SyncM status, 0x04 dynamic Outputs, 0x05 dynamic Inputs) — a byte array, not 16-bit
  // words. SOEM's ecx_siiFMMU reads each FMMUn with a single ecx_siigetbyte. A trailing pad byte
  // keeps the category word-aligned when the FMMU count is odd.
  std::vector<uint8_t> fmmus;
  fmmus.reserve(p.size());
  for (size_t i = 0; i < p.size(); ++i) {
    fmmus.push_back(rdU8(p, i));
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
  // The remaining categories are single values rather than ranges. Switching over the enum rather
  // than matching against a table is deliberate: the switch is exhaustiveness-checked, so a
  // category added to SiiCategoryType fails the build here (-Wswitch, and warnings are errors)
  // instead of silently never resolving. The enum lives in the header and this resolver does not,
  // so nothing else would prompt whoever extends it — and the failure would be quiet, a real
  // category read as unknown.
  //
  // Widening @p value into the enum before knowing it is an enumerator reads worse than it is, and
  // is safe: SiiCategoryType fixes its underlying type (: uint16_t), so the conversion is well
  // defined and value-preserving for every input, and an unmatched word falls through to nullopt.
  // Only an enum *without* a fixed underlying type would make this undefined, so this does not need
  // "fixing" into a table.
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

std::expected<void, std::string> validateSiiImage(std::span<const uint8_t> buffer) {
  // The checksum byte lives at offset 14, covering the 14 bytes before it; the category section
  // starts at word 0x40, so an image must hold at least that word's header to be walkable.
  constexpr size_t kChecksumOffset = 14;
  constexpr size_t kCategoryStartWord = 0x40;
  constexpr size_t kMinWords = kCategoryStartWord + 1;
  constexpr uint16_t kEndMarker = 0xFFFF;

  if (buffer.size() % 2 != 0) {
    return std::unexpected(std::string("SII image has an odd length (") +
                           std::to_string(buffer.size()) +
                           " bytes) — the EEPROM is addressed in 16-bit words");
  }
  const size_t words = buffer.size() / 2;
  if (words < kMinWords) {
    return std::unexpected(std::string("SII image too small: ") + std::to_string(words) +
                           " words (need at least " + std::to_string(kMinWords) +
                           " for the fixed header and the first category)");
  }

  // ETG CRC-8: polynomial x^8 + x^2 + x + 1, initial value 0xFF, no final XOR.
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < kChecksumOffset; ++i) {
    crc ^= buffer[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = static_cast<uint8_t>((crc & 0x80) != 0 ? (crc << 1) ^ 0x07 : crc << 1);
    }
  }
  if (crc != buffer[kChecksumOffset]) {
    return std::unexpected(std::string("SII checksum mismatch: the image carries 0x") +
                           std::format("{:02X}", buffer[kChecksumOffset]) +
                           " but words 0-6 hash to 0x" + std::format("{:02X}", crc));
  }

  // Walk the category chain. Each entry is a type word, a size word (in words), then that many
  // payload words. Every step is bounds-checked against the image, so a size field inflated by
  // corruption is caught here rather than read past the end.
  for (size_t word = kCategoryStartWord;;) {
    if (word >= words) {
      return std::unexpected(
          "SII category chain runs past the end of the image without an end marker");
    }
    const uint16_t type = static_cast<uint16_t>(buffer[word * 2] | (buffer[word * 2 + 1] << 8));
    if (type == kEndMarker) {
      return {};
    }
    // Only a real category has a size word; the end marker above is a lone word, and on a
    // well-formed image it is routinely the very last one (it is in the Circulo fixture), so
    // demanding a size word before checking for the marker would reject a valid image.
    if (word + 1 >= words) {
      return std::unexpected(std::string("SII category at word 0x") + std::format("{:04X}", word) +
                             " has no size word — the image ends mid-header");
    }
    const size_t sizeWord = word + 1;
    const uint16_t size =
        static_cast<uint16_t>(buffer[sizeWord * 2] | (buffer[sizeWord * 2 + 1] << 8));
    const size_t next = word + 2 + size;
    if (next <= word || next > words) {
      return std::unexpected(std::string("SII category at word 0x") + std::format("{:04X}", word) +
                             " declares " + std::to_string(size) + " words, which runs past the " +
                             std::to_string(words) + "-word image");
    }
    word = next;
  }
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
