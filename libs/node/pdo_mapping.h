#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace mm::node {

/// @brief One object mapped into a slave's process data, with its position in the window.
///
/// Produced by walking the PDO assignment (@c 0x1C12 / @c 0x1C13) and the mapping objects
/// (@c 0x16xx / @c 0x1Axx).  @c bitOffset is measured from the start of this slave's
/// direction window (its output region for an RxPDO entry, its input region for a TxPDO
/// entry), so it composes with the per-slave window offset from
/// @c mm::comm::PdoLayout to give the object's absolute position in the process image.
///
/// A mapping object may contain padding entries (@c index == 0) inserted purely for
/// byte alignment; these are kept so the running offset stays correct, but they bind to
/// no object and consumers skip them.
/// The three packed fields (@c index, @c subindex, @c bitLength) mirror the 32-bit CoE mapping
/// entry exactly — @c index<<16 | subindex<<8 | bitLength — so a @c PdoMappingEntry is both what a
/// mapping-object read decodes into and what @c Device::writePdoMapping composes onto the wire.
/// @c bitOffset is derived metadata: populated by @c readFlatPdoMapping from the running offset,
/// and left at its default (ignored) when the entry describes a mapping to be *written* (the write
/// derives each object's position from the entry order).
struct PdoMappingEntry {
  uint16_t index = 0;     ///< CoE object index; @c 0x0000 marks a padding gap (no object).
  uint8_t subindex = 0;   ///< CoE object subindex.
  uint8_t bitLength = 0;  ///< Width of the entry in bits (the packed 7..0 field; max 255).
  uint32_t bitOffset =
      0;  ///< Bit offset from the slave's direction window (read-only; 0 on write).
};

/// @brief Packs an entry into its 32-bit CoE mapping word (ETG.1000.6 §5.6.7.4.7):
///        @c index<<16 | subindex<<8 | bitLength.
///
/// @c bitOffset is not part of the wire word — it is derived from the running offset by the reader.
/// This is the word @c Device::writePdoMapping writes to each mapping-object subindex.
inline uint32_t packMappingEntry(const PdoMappingEntry& e) {
  return (static_cast<uint32_t>(e.index) << 16) | (static_cast<uint32_t>(e.subindex) << 8) |
         static_cast<uint32_t>(e.bitLength);
}

/// @brief Unpacks a 32-bit CoE mapping word into an entry — the inverse of @c packMappingEntry.
///
/// The returned entry's @c bitOffset is left at its default; the reader fills it from the running
/// offset. An @c index of @c 0x0000 denotes an alignment gap (no bound object).
inline PdoMappingEntry unpackMappingEntry(uint32_t packed) {
  return PdoMappingEntry{
      .index = static_cast<uint16_t>(packed >> 16),
      .subindex = static_cast<uint8_t>((packed >> 8) & 0xFF),
      .bitLength = static_cast<uint8_t>(packed & 0xFF),
  };
}

/// @brief A device's complete PDO mapping across both directions.
///
/// @c outputs is the RxPDO content (master→slave, assigned via @c 0x1C12 from @c 0x16xx
/// mapping objects); @c inputs is the TxPDO content (slave→master, assigned via @c 0x1C13
/// from @c 0x1Axx).  Entries within each direction are in process-image order, so their
/// @c bitOffset values are strictly non-decreasing.  Empty until @c Device::readFlatPdoMapping.
struct FlatPdoMapping {
  std::vector<PdoMappingEntry> outputs;  ///< RxPDO entries (master→slave), in window order.
  std::vector<PdoMappingEntry> inputs;   ///< TxPDO entries (slave→master), in window order.
  uint32_t outputBits = 0;               ///< Total mapped output bits (sum of entry widths).
  uint32_t inputBits = 0;                ///< Total mapped input bits.
};

/// @brief One PDO mapping object (@c 0x16xx RxPDO / @c 0x1Axx TxPDO) and its ordered entries.
///
/// The grouping the flat @c FlatPdoMapping lacks: which mapping object each entry belongs to,
/// needed to write the mapping (each entry goes to a subindex of @c pdoIndex). Entry order is the
/// order the objects occupy in the process-data window. Only the entries' packed fields (@c index,
/// @c subindex, @c bitLength) are written; their @c bitOffset is ignored here.
struct PdoMappingObject {
  uint16_t pdoIndex = 0;                 ///< Mapping-object index (@c 0x16xx or @c 0x1Axx).
  std::vector<PdoMappingEntry> entries;  ///< Entries in the order they are mapped.
};

/// @brief A device's desired PDO configuration to write and assign — the write-side input to
///        @c Device::writePdoMapping, and the grouped counterpart of the read-side @c
///        FlatPdoMapping.
///
/// @c outputs are RxPDO objects (master→slave) assigned to sync manager 2 via @c 0x1C12;
/// @c inputs are TxPDO objects (slave→master) assigned to sync manager 3 via @c 0x1C13 (named for
/// the master's perspective, as everywhere else in the node layer, rather than the ambiguous
/// rx/tx). The vector order is the sync-manager assignment order — the order objects are written to
/// @c 0x1C12 / @c 0x1C13 subindices 1..N — which fixes their position in the process image.
struct PdoMapping {
  std::vector<PdoMappingObject> outputs;  ///< RxPDO objects, assigned to @c 0x1C12.
  std::vector<PdoMappingObject> inputs;   ///< TxPDO objects, assigned to @c 0x1C13.
};

/// @brief Serialises a single entry: @c index, @c subindex, @c bitLength, @c bitOffset.
void to_json(nlohmann::json& j, const PdoMappingEntry& e);

/// @brief Serialises one mapping object: @c pdoIndex and its ordered @c entries.
void to_json(nlohmann::json& j, const PdoMappingObject& o);

/// @brief Serialises a device's grouped mapping: @c outputs / @c inputs object arrays. The response
///        shape of the PDO-mapping routes (each entry carries its derived @c bitOffset).
void to_json(nlohmann::json& j, const PdoMapping& m);

}  // namespace mm::node
