#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#include "node/process_data_ring.h"

namespace mm::node {

/// @file
/// The `.mmpd` binary dump of the process-data recorder: exactly what the recorder ring holds
/// (full raw inputs + outputs for every cycle in a frozen span) **plus the process image embedded
/// as a header**, so a run decodes fully offline — no running Motion Master and no live bus.
///
/// Layout (all multi-byte integers little-endian):
///
///   File prefix (fixed 40 bytes — @c rowCount sits at a fixed offset so it can be patched after
///   the rows are streamed):
///     0   char  magic[4]      = 'M','M','P','D'
///     4   u16   formatVersion = kDumpFormatVersion
///     6   u16   flags         = 0 (reserved)
///     8   u32   cyclePeriodUs
///     12  u64   startSequence (sequence of the first row; oldest cycle in the span)
///     20  u64   rowCount      (number of rows actually written)        <- patched at the end
///     28  u32   inputBytes    (per-row input region size)
///     32  u32   outputBytes   (per-row output region size)
///     36  u32   deviceCount
///   Device table (deviceCount entries):
///     u16 slavePosition
///     u32 vendorId · u32 productCode · u32 revisionNumber · u32 serialNumber
///     str name                              (u16 length + UTF-8 bytes)
///     u32 entryCount
///       per PDO entry:
///         u16 index · u8 subindex · u8 direction (1 = output/RxPDO, 0 = input/TxPDO)
///         u16 dataType (0 = unknown) · u16 bitLength · u32 bitOffset
///         str name                          (empty when the object dictionary is not enumerated)
///   Rows (rowCount entries, fixed stride 16 + inputBytes + outputBytes):
///     u64 sequence · u64 timestampNs · u8 inputs[inputBytes] · u8 outputs[outputBytes]
///
/// A row's input/output regions are padded with zeros (or truncated) to the header sizes, so the
/// stride is fixed and a reader can index any cycle directly. The timestamp keeps full epoch-ns
/// precision (unlike the live monitoring row, which reduces it to microseconds for JavaScript).

/// @brief Magic at the very start of every dump file.
inline constexpr std::array<char, 4> kDumpMagic = {'M', 'M', 'P', 'D'};

/// @brief Current dump format version. A reader rejects an unknown major shape; bump on layout
///        changes so old files stay identifiable.
inline constexpr uint16_t kDumpFormatVersion = 1;

/// @brief Byte offset of the @c rowCount field within the fixed prefix. The writer streams rows
///        first, then seeks here to patch the final count — so the stream must be seekable.
inline constexpr std::streamoff kDumpRowCountOffset = 20;

/// @brief One PDO-mapped object as recorded in the dump header.
struct DumpPdoEntry {
  uint16_t index = 0;
  uint8_t subindex = 0;
  bool isOutput = false;   ///< True for an RxPDO output, false for a TxPDO input.
  uint16_t dataType = 0;   ///< ETG.1020 data-type code, or 0 when the OD was not enumerated.
  uint16_t bitLength = 0;  ///< Width of the value in bits.
  uint32_t bitOffset = 0;  ///< Absolute bit offset within its direction's image.
  std::string name;        ///< Object name, or empty when the OD was not enumerated.
};

/// @brief One device's identity and the PDO objects it contributes to the image.
struct DumpDevice {
  uint16_t slavePosition = 0;
  uint32_t vendorId = 0;
  uint32_t productCode = 0;
  uint32_t revisionNumber = 0;
  uint32_t serialNumber = 0;
  std::string name;
  std::vector<DumpPdoEntry> entries;
};

/// @brief The embedded process-image header: everything needed to decode the rows offline.
struct DumpHeader {
  uint32_t cyclePeriodUs = 0;  ///< GameLoop cycle period; lets a reader place rows on a time axis.
  uint32_t inputBytes = 0;     ///< Per-row input region size (the published image's input size).
  uint32_t outputBytes = 0;    ///< Per-row output region size.
  std::vector<DumpDevice> devices;
};

/// @brief Reads the record for @p seq into @p out; returns @c false if it is no longer available
///        (lapped or a torn read). Wraps @c ProcessDataRing::readRecord.
using DumpRecordReader = std::function<bool(uint64_t seq, ProcessDataRing::Record& out)>;

/// @brief Serialises the frozen span @c [startSeq, endSeq) to @p out as a `.mmpd` dump.
///
/// Streams each row directly from @p read (no whole-span buffer), padding/truncating its
/// input/output regions to the header sizes for a fixed stride. A record @p read cannot return
/// (lapped while the producer is still running, or a torn read) is skipped — every row carries its
/// own sequence, so a gap is self-describing and needs no marker; at a seconds-deep ring versus a
/// sub-second bulk write this never actually happens. @c rowCount is patched at @c
/// kDumpRowCountOffset once the real count is known, so @p out must be seekable (a file or string
/// stream).
///
/// @return The number of rows written, or an error string if a stream write/seek failed.
std::expected<uint64_t, std::string> writeProcessDataDump(std::ostream& out,
                                                          const DumpHeader& header,
                                                          uint64_t startSeq, uint64_t endSeq,
                                                          const DumpRecordReader& read);

}  // namespace mm::node
