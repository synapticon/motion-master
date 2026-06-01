#pragma once

#include <cstdint>
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
struct PdoMappingEntry {
  uint16_t index;      ///< CoE object index; @c 0x0000 marks a padding gap (no object).
  uint8_t subindex;    ///< CoE object subindex.
  uint16_t bitLength;  ///< Width of the entry in bits.
  uint32_t bitOffset;  ///< Bit offset from the start of this slave's direction window.
};

/// @brief A device's complete PDO mapping across both directions.
///
/// @c outputs is the RxPDO content (master→slave, assigned via @c 0x1C12 from @c 0x16xx
/// mapping objects); @c inputs is the TxPDO content (slave→master, assigned via @c 0x1C13
/// from @c 0x1Axx).  Entries within each direction are in process-image order, so their
/// @c bitOffset values are strictly non-decreasing.  Empty until @c Device::readPdoMappings.
struct PdoMappings {
  std::vector<PdoMappingEntry> outputs;  ///< RxPDO entries (master→slave), in window order.
  std::vector<PdoMappingEntry> inputs;   ///< TxPDO entries (slave→master), in window order.
  uint32_t outputBits = 0;               ///< Total mapped output bits (sum of entry widths).
  uint32_t inputBits = 0;                ///< Total mapped input bits.
};

}  // namespace mm::node
