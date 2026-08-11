#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "node/device_parameter.h"

namespace mm::node {

class Device;

/// @brief Fixed-capacity byte buffer for one direction of the process image.
///
/// The RT-thread scratch the exchange composes into (outputs) and receives into (inputs) each
/// cycle before the cycle is appended to the recorder ring. Sized to
/// @c mm::comm::kMaxProcessImageBytes; @c size is the number of valid leading bytes for the
/// current mapping.
struct ProcessBuffer {
  uint32_t size = 0;
  std::array<uint8_t, mm::comm::kMaxProcessImageBytes> bytes{};
};

/// @brief Locates one mapped object within the flat process image.
///
/// @c bitOffset is absolute within its direction's image (the output image for an output
/// entry, the input image for an input entry) — i.e. the slave's window offset plus the
/// object's offset inside that window, already combined.  Used by the node layer to decode a
/// value out of the input image into its parameter's cell, or to insert one from that cell into
/// the output image.
struct ProcessImageEntry {
  uint16_t slavePosition = 0;  ///< 1-based bus position of the owning device.
  uint16_t index = 0;          ///< CoE object index.
  uint8_t subindex = 0;        ///< CoE object subindex.
  uint16_t bitLength = 0;      ///< Width of the value in bits.
  uint32_t bitOffset = 0;      ///< Absolute bit offset within the direction's image.

  /// @brief The owning device's parameter, resolved once when the image is built.
  ///
  /// **Why it is here rather than looked up per cycle.** The RT loop decodes every mapped object
  /// into its cell after each exchange; a hash lookup per object is nothing for one device and
  /// 60–100 µs against a 1 ms grid for fifty of them. Resolving at publish time makes the decode
  /// loop a walk over contiguous entries with no lookups at all.
  ///
  /// Valid for as long as the image generation is: @c initializeParameters replaces the parameter
  /// map, and @c scan / @c reset destroy the @c Device — but both pause the RT cycle across the
  /// change (@c ProcessData::pauseCycle), and a re-map rebuilds every entry here. @c nullptr when
  /// the object dictionary had not been enumerated at build time, which is legal: the object is
  /// still exchanged, it simply has nowhere to decode into.
  ///
  /// Const because the cell is written through @c DeviceParameter::storeBits, which is itself const
  /// — the value is a @c mutable atomic, like a lock. That keeps @c buildProcessImage taking the
  /// devices by const reference.
  const DeviceParameter* parameter = nullptr;
};

/// @brief The whole bus's process-data layout, resolved to absolute positions.
///
/// Built by @c buildProcessImage from the driver's @c PdoLayout (per-slave windows + sizes)
/// and each device's @c FlatPdoMapping (per-object offsets within its window).  Immutable once
/// built and published behind an atomic pointer; the RT loop reads only @c outputBytes /
/// @c inputBytes / @c expectedWkc, while @c outputs / @c inputs serve non-RT value access.
struct ProcessImage {
  std::vector<ProcessImageEntry> outputs;  ///< Master→slave objects, located in the output image.
  std::vector<ProcessImageEntry> inputs;   ///< Slave→master objects, located in the input image.
  uint32_t outputBytes = 0;                ///< Size of the output image.
  uint32_t inputBytes = 0;                 ///< Size of the input image.
  int expectedWkc = 0;                     ///< Working counter when every slave exchanges.

  /// @brief Where a mapped object sits in the image, and which direction it belongs to.
  struct Location {
    uint32_t bitOffset;  ///< Absolute bit offset within the direction's image.
    uint16_t bitLength;  ///< Width of the value in bits.
    bool isOutput;       ///< True for an RxPDO output, false for a TxPDO input.
    size_t entryIndex;   ///< Index within @c outputs (output) or @c inputs (input), which is where
                         ///< the entry's owning @c DeviceParameter — and so its cell — is reached.
  };

  /// @brief Locates a mapped object by bus position and CoE address.
  /// @return Its location, or @c nullopt if the object is not mapped in this image.
  std::optional<Location> find(uint16_t slavePosition, uint16_t index, uint8_t subindex) const;
};

/// @brief Extracts @p bitLength bits at @p bitOffset from @p src into a fresh LSB-aligned
///        little-endian byte vector (@c ceil(bitLength/8) bytes), as the SDO encoding would.
///
/// Byte-aligned extents are a plain copy; sub-byte extents are assembled bit by bit. Allocates —
/// use the @p out overload on any path that must not.
std::vector<uint8_t> extractBits(std::span<const uint8_t> src, uint32_t bitOffset,
                                 uint16_t bitLength);

/// @brief Extracts @p bitLength bits at @p bitOffset from @p src into the caller's @p out buffer.
///
/// The non-allocating form of @c extractBits, and the primitive the vector-returning overload is
/// written over — the RT decode loop runs this once per mapped object per cycle into a stack
/// buffer, where a heap allocation is not permitted.
///
/// @p out is **zeroed in full** before anything is written, so a buffer wider than the value (the
/// expected case: an 8-byte scratch for a 2-byte object) is left with defined zero padding rather
/// than the caller's stale bytes. At most @c ceil(bitLength/8) bytes are then written, clamped to
/// @c out.size(): an @p out too small to hold the value is filled as far as it goes rather than
/// overrun, matching how a @p src too short to supply the bits leaves the remainder zero.
///
/// @param src        Source image bytes.
/// @param bitOffset  Absolute bit offset within @p src.
/// @param bitLength  Width of the value in bits.
/// @param out        Destination buffer; zeroed, then filled LSB-aligned little-endian.
void extractBits(std::span<const uint8_t> src, uint32_t bitOffset, uint16_t bitLength,
                 std::span<uint8_t> out);

/// @brief Inserts @p bitLength bits of @p value (LSB-first) into @p dst at @p bitOffset.
///
/// The inverse of @c extractBits. Bits of @p value beyond what it provides are treated as 0.
void insertBits(std::span<uint8_t> dst, uint32_t bitOffset, uint16_t bitLength,
                std::span<const uint8_t> value);

/// @brief Assembles a @c ProcessImage from the driver layout and per-device PDO mappings.
///
/// Matches each device to its window in @p layout by bus position and combines the window
/// offset with the device's per-object bit offsets to produce absolute positions.  Padding
/// entries (@c index == 0) are dropped — they bind to no object.  Devices must have had
/// @c Device::readFlatPdoMapping called already.
///
/// @param layout   Per-slave windows and image sizes from @c FieldbusDriver::processDataLayout.
/// @param devices  Devices whose @c flatPdoMapping() supply the per-object offsets.
/// @return The assembled image, or an error string if a device has a mapping but no matching
///         window in @p layout, or its mapped width overflows the window the driver reserved.
std::expected<ProcessImage, std::string> buildProcessImage(const mm::comm::PdoLayout& layout,
                                                           const std::vector<Device>& devices);

}  // namespace mm::node
