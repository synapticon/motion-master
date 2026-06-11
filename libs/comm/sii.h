#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm::comm {

/// @brief SII (Slave Information Interface) — the structured EEPROM image of an EtherCAT slave.
///
/// Every EtherCAT slave carries a non-volatile EEPROM wired to its ESC; the ESC auto-loads its
/// first words at power-on to configure itself before the master communicates. The SII is the
/// standardised layout of that EEPROM (ETG.1000.6 §5.4): a fixed 128-byte header (identity +
/// mailbox configuration) followed by a sequence of variable-length, self-describing categories
/// (strings, general info, FMMU/Sync-Manager defaults, default PDO mappings, distributed-clock
/// settings). The raw bytes are read off the bus with @c FieldbusDriver::readSii; @c parseSii
/// decodes them into the structures below, which serialise 1:1 to JSON for the SII UI page.
///
/// All multi-byte fields are little-endian. The parser is a pure, hardware-independent transform
/// over a byte span — it is unit-tested against a captured EEPROM image with no fieldbus present.

/// @brief Fixed-header (first 128 bytes) fields of an SII image.
///
/// String-index fields elsewhere (e.g. @c SiiCategoryGeneral::nameIdx) reference 1-based entries
/// of the STRINGS category. @c mailboxProtocol is a bitfield (0x01 AoE, 0x02 EoE, 0x04 CoE,
/// 0x08 FoE, 0x10 SoE, 0x20 VoE). @c size is the raw EEPROM-size word (ETG: size in KiBit − 1).
struct SiiInfo {
  uint16_t pdiControl = 0;
  uint16_t pdiConfiguration = 0;
  uint16_t syncImpulseLen = 0;
  uint16_t pdiConfiguration2 = 0;
  uint16_t configuredStationAlias = 0;
  uint16_t checksum = 0;
  uint32_t vendorId = 0;
  uint32_t productCode = 0;
  uint32_t revisionNumber = 0;
  uint32_t serialNumber = 0;
  uint16_t bootstrapReceiveMailboxOffset = 0;
  uint16_t bootstrapReceiveMailboxSize = 0;
  uint16_t bootstrapSendMailboxOffset = 0;
  uint16_t bootstrapSendMailboxSize = 0;
  uint16_t standardReceiveMailboxOffset = 0;
  uint16_t standardReceiveMailboxSize = 0;
  uint16_t standardSendMailboxOffset = 0;
  uint16_t standardSendMailboxSize = 0;
  uint16_t mailboxProtocol = 0;
  uint16_t size = 0;
  uint16_t version = 0;
};

/// @brief General device information (SII category 30 / 0x1E).
struct SiiCategoryGeneral {
  uint8_t groupIdx = 0;               ///< STRINGS index of the group name.
  uint8_t imgIdx = 0;                 ///< STRINGS index of the image name.
  uint8_t orderIdx = 0;               ///< STRINGS index of the order number.
  uint8_t nameIdx = 0;                ///< STRINGS index of the device name.
  uint8_t coeDetails = 0;             ///< CoE capability flags.
  uint8_t foeDetails = 0;             ///< FoE capability flags.
  uint8_t eoeDetails = 0;             ///< EoE capability flags.
  uint8_t soeChannels = 0;            ///< SoE channel count.
  uint8_t ds402Channels = 0;          ///< DS402 channel count.
  uint8_t sysmanClass = 0;            ///< Sync-manager class.
  uint8_t flags = 0;                  ///< General device flags.
  uint8_t currentOnEBus = 0;          ///< E-Bus current consumption (mA).
  int16_t physicalPort = 0;           ///< Physical-port configuration (per-port nibbles).
  uint8_t physicalMemoryAddress = 0;  ///< Physical-memory address (low byte; see parseSii note).
};

/// @brief One Sync-Manager default from SII category 41 (0x29).
struct SiiCategorySyncManagerElement {
  uint16_t physicalStartAddress = 0;  ///< Start address in ESC DPRAM.
  uint16_t length = 0;                ///< Window length in bytes.
  uint8_t controlRegister = 0;        ///< SM control register value.
  uint8_t statusRegister = 0;         ///< SM status register value.
  uint8_t enableSyncManager = 0;      ///< Non-zero when enabled.
  uint8_t syncManagerType = 0;        ///< 1=MbxOut, 2=MbxIn, 3=Outputs, 4=Inputs.
};

/// @brief One mapped object within a default PDO (SII categories 50/51).
struct SiiCategoryPdoEntryElement {
  uint16_t entryIndex = 0;   ///< Object dictionary index.
  uint8_t subindex = 0;      ///< Object dictionary subindex.
  uint8_t entryNameIdx = 0;  ///< STRINGS index of the entry name.
  uint8_t dataType = 0;      ///< ETG.1020 data-type code.
  uint8_t bitLen = 0;        ///< Entry bit length.
  uint16_t flags = 0;        ///< Entry flags.
};

/// @brief One default PDO (RxPDO category 51 / 0x33, or TxPDO category 50 / 0x32).
struct SiiCategoryPdoElement {
  uint16_t pdoIndex = 0;                            ///< PDO object index (0x16xx / 0x1Axx).
  uint8_t nEntry = 0;                               ///< Declared number of mapped entries.
  uint8_t syncM = 0;                                ///< Assigned Sync Manager.
  uint8_t synchronization = 0;                      ///< Synchronisation type.
  uint8_t nameIdx = 0;                              ///< STRINGS index of the PDO name.
  uint16_t flags = 0;                               ///< PDO flags.
  std::vector<SiiCategoryPdoEntryElement> entries;  ///< Mapped entries (may be < @c nEntry if
                                                    ///< the category payload was truncated).
};

/// @brief One distributed-clock setting from SII category 60 (0x3C).
struct SiiCategoryDistributedClockElement {
  uint32_t cycleTime0 = 0;       ///< SYNC0 cycle time (ns).
  uint32_t shiftTime0 = 0;       ///< SYNC0 shift time (ns).
  uint32_t shiftTime1 = 0;       ///< SYNC1 shift time (ns).
  int16_t sync1CycleFactor = 0;  ///< SYNC1 cycle factor.
  uint16_t assignActivate = 0;   ///< DC assign/activate register value.
  int16_t sync0CycleFactor = 0;  ///< SYNC0 cycle factor.
  uint8_t nameIdx = 0;           ///< STRINGS index of the name.
  uint8_t descIdx = 0;           ///< STRINGS index of the description.
};

/// @brief The decoded category section of an SII image.
struct SiiCategorySection {
  std::vector<std::string> strings;                         ///< STRINGS table (category 10).
  SiiCategoryGeneral general;                               ///< GENERAL (category 30).
  std::vector<uint16_t> fmmus;                              ///< FMMU defaults (category 40).
  std::vector<SiiCategorySyncManagerElement> syncManagers;  ///< SYNC_M defaults (category 41).
  std::vector<SiiCategoryPdoElement> rxPdos;                ///< Default RxPDOs (category 51).
  std::vector<SiiCategoryPdoElement> txPdos;                ///< Default TxPDOs (category 50).
  std::vector<SiiCategoryDistributedClockElement> distributedClocks;  ///< DC (category 60).
};

/// @brief A fully parsed SII image: fixed header plus decoded categories.
struct SlaveInformationInterface {
  SiiInfo info;
  SiiCategorySection category;
};

/// @brief Recognised SII category type identifiers (ETG.1000.6 §5.4, Table 17).
///
/// Several spec ranges collapse to a single representative (e.g. all vendor-specific values map to
/// @c VendorSpecific); see @c resolveSiiCategoryType.
enum class SiiCategoryType : uint16_t {
  Nop = 0,
  DeviceSpecific = 1,
  Strings = 10,
  DataTypes = 20,
  General = 30,
  Fmmu = 40,
  SyncM = 41,
  FmmuX = 42,
  SyncUnit = 43,
  TxPdo = 50,
  RxPdo = 51,
  Dc = 60,
  Timeouts = 70,
  Dictionary = 80,
  Hardware = 90,
  VendorInformation = 100,
  Images = 110,
  VendorSpecific = 0x0800,
  ApplicationSpecific = 0x2000,
  End = 0xFFFF,
};

/// @brief Normalises a raw 16-bit category-type word into a @c SiiCategoryType.
///
/// Range-based values are folded to a single representative (DeviceSpecific for 0x0001–0x0009,
/// VendorSpecific for 0x0800–0x1FFF and 0x3000–0xFFFE, ApplicationSpecific for 0x2000–0x2FFF);
/// the original numeric value is not preserved. Fixed IDs (Strings, General, …) and the End marker
/// resolve directly. Returns @c std::nullopt for unrecognised values, which the caller skips.
std::optional<SiiCategoryType> resolveSiiCategoryType(uint16_t value);

/// @brief Parses a raw SII EEPROM image into a @c SlaveInformationInterface.
///
/// Decodes the fixed 128-byte header, then walks the category section starting at offset 128
/// (each category is a 16-bit type, a 16-bit word-size, then @c wordSize × 2 payload bytes),
/// dispatching recognised categories to their decoders and stopping at the End (0xFFFF) or Nop
/// marker. Unrecognised categories are skipped; multiple PDO categories accumulate. The walk is
/// bounds-checked: a category claiming more bytes than remain is clamped, never read past the end.
///
/// @note Faithful port of the original TypeScript parser, including its treatment of the GENERAL
///       category's physical-memory address as a single byte. ETG.1000.6 defines that field as a
///       16-bit word; the two agree whenever its high byte is zero (the usual case).
///
/// @param buffer  Raw SII bytes (at least 128 for a valid header).
/// @return The parsed structure, or an error string if @p buffer is shorter than the fixed header.
std::expected<SlaveInformationInterface, std::string> parseSii(std::span<const uint8_t> buffer);

void to_json(nlohmann::json& j, const SiiInfo& v);
void to_json(nlohmann::json& j, const SiiCategoryGeneral& v);
void to_json(nlohmann::json& j, const SiiCategorySyncManagerElement& v);
void to_json(nlohmann::json& j, const SiiCategoryPdoEntryElement& v);
void to_json(nlohmann::json& j, const SiiCategoryPdoElement& v);
void to_json(nlohmann::json& j, const SiiCategoryDistributedClockElement& v);
void to_json(nlohmann::json& j, const SiiCategorySection& v);
void to_json(nlohmann::json& j, const SlaveInformationInterface& v);

}  // namespace mm::comm
