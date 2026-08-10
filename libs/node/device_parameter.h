#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace mm::node {

// Values are stored and exchanged as little-endian wire bytes and read back by memcpy, here
// and in decodeSdoBytes. A big-endian host would need every one of those to byte-swap, so it
// must fail here rather than silently return reversed numbers.
static_assert(std::endian::native == std::endian::little,
              "parameter values are stored as little-endian wire bytes");

/// @brief Decoded value of a single device parameter (CoE object dictionary entry).
///
/// The variant alternatives cover the standard ETG.1020 data types that can be
/// transferred via SDO upload. Use @c std::visit to dispatch on the active type.
using DeviceParameterValue = std::variant<int8_t, int16_t, int32_t, int64_t,      //
                                          uint8_t, uint16_t, uint32_t, uint64_t,  //
                                          float, double,                          //
                                          std::string, std::vector<uint8_t>>;

/// @brief Packs an object dictionary @p index and @p subindex into a single 32-bit key.
///
/// Layout: @c (index << 8) | subindex. Used as the key of @c Device::parameters().
constexpr uint32_t makeParameterKey(uint16_t index, uint8_t subindex) {
  return (static_cast<uint32_t>(index) << 8) | static_cast<uint32_t>(subindex);
}

/// @brief The address of one object-dictionary entry, carrying the type that entry holds.
///
/// The typed counterpart of @c makeParameterKey: an index and a subindex, plus the C++ type the
/// object's declared ETG.1020 data type maps to. It names a location in *any* dictionary — it holds
/// no device and no pointer, which is why it is an address rather than a reference or a handle.
///
/// Its point is that the three things easiest to get wrong about an object — its index, its type,
/// and (in the trailing comment beside each generated constant) its unit — travel together instead
/// of being retyped at every call site. @c Device's overloads take one in place of an
/// index/subindex pair, so the type argument disappears from the call:
///
/// @code
/// device.value(somanet::objects::kDriveTemperatureMeasuredTemperature);  // std::optional<int32_t>
/// device.readValue(profile::objects::kManufacturerSoftwareVersion);      //
/// expected<std::string,…>
/// @endcode
///
/// A whole dictionary's worth of these is generated from a vendor's ESI — see
/// @c cia402_drive_objects.h and its siblings — but there is nothing generated about the type
/// itself: writing one by hand for an object you care about is a one-liner.
template <typename T>
struct ObjectAddress {
  uint16_t index{};    ///< CoE object index.
  uint8_t subindex{};  ///< CoE object subindex.
};

/// @brief Returns a zero-equivalent value for the given ETG.1020 @p dataType.
///
/// The variant alternative a value of this type is reported as, holding its type-appropriate zero.
/// Unknown data types fall back to @c std::vector<uint8_t>{}.
DeviceParameterValue defaultValueForDataType(uint16_t dataType);

/// @brief Whether a value of @p dataType is stored in @c DeviceParameter::bits.
///
/// True for every arithmetic ETG.1020 type — all of which fit in eight bytes — and false for
/// strings and for the composite / unknown types that fall through to raw bytes. This is what
/// decides which of a parameter's two storage fields holds its value, and because @c dataType is
/// immutable once the object dictionary is enumerated, the choice never changes: a value has
/// exactly one home and the two fields can never disagree.
bool isScalarDataType(uint16_t dataType);

/// @brief Width in bytes of a value of @p dataType, or @c 0 when it is not a scalar.
///
/// The declared width, which is what an SDO transfer carries — not @c bitLength, which describes
/// the object's slot in the process image and may be narrower.
size_t scalarByteWidth(uint16_t dataType);

/// @brief Packs up to eight raw little-endian wire bytes into a @c uint64_t (zero-extended).
uint64_t packLeBits(std::span<const uint8_t> bytes);

/// @brief Unpacks @p bits into @p out, little-endian. The inverse of @c packLeBits.
void unpackLeBits(uint64_t bits, std::span<uint8_t> out);

/// @brief Coerces a @c DeviceParameterValue to a @c double, when it holds a number.
///
/// @return The value as a @c double for any arithmetic alternative, or
///         @c std::nullopt for @c std::string / @c std::vector<uint8_t>.
std::optional<double> numericValue(const DeviceParameterValue& value);

/// @brief Tracks how a cached parameter value relates to the device.
///
/// A device may be edited while offline (powered off, or in INIT with no mailbox);
/// this flag lets callers tell a freshly-read value from a stale or pending one
/// (see NEXTGEN.md — "tag offline values with a freshness flag").
enum class SyncState : uint8_t {
  Unknown,  ///< Never read; @c value is the type-appropriate default.
  Synced,   ///< @c value matches the device (last successful read or write).
  Pending,  ///< @c value was set locally while offline / after a failed write.
};

/// @brief Returns the lowercase string form of @p state (@c "unknown" etc.).
std::string_view syncStateName(SyncState state);

/// @brief Where a parameter's definition (schema/metadata) came from.
///
/// A CoE slave enumerates its object dictionary over SDO-Info (@c ObjectDictionary). A
/// mailbox-less slave (an EtherCAT coupler / simple I/O terminal) has no object dictionary; its
/// process-data objects are instead described by its SII EEPROM (@c Sii), which supplies
/// index/subindex/data type and bit length but carries no live SDO access — such a parameter's
/// value is only ever the process-image value, never an SDO upload.
enum class ParameterOrigin : uint8_t {
  ObjectDictionary,  ///< Enumerated over the CoE object dictionary (SDO-Info).
  Sii,               ///< Derived from the SII EEPROM PDO categories (no CoE mailbox).
};

/// @brief Returns the string form of @p origin (@c "objectDictionary" / @c "sii").
std::string_view parameterOriginName(ParameterOrigin origin);

/// @brief A single object dictionary entry held by a @c Device.
///
/// Combines immutable schema (index, subindex, name, data type, bit length,
/// access flags, object code) with a @c value that is overwritten by SDO upload
/// or PDO exchange. @c value is initialised to the type-appropriate zero so
/// callers can @c std::visit it directly without checking for a read.
struct DeviceParameter {
  uint16_t index{};       ///< CoE object index.
  uint8_t subindex{};     ///< CoE object subindex.
  std::string name;       ///< Textual description from @c ecx_readOE.
  uint16_t objectCode{};  ///< OTYPE_VAR / OTYPE_ARRAY / OTYPE_RECORD (ETG.1000.6 §5).
  uint16_t dataType{};    ///< ETG.1020 data type code (e.g. 0x0007 = UNSIGNED32).
  uint16_t bitLength{};   ///< Bit length of the value.
  uint16_t access{};      ///< ObjAccess bitfield (read/write per-state flags).
  ParameterOrigin origin{ParameterOrigin::ObjectDictionary};  ///< Where this definition came from.

  // --- value -------------------------------------------------------------------------------
  // The last-known value, stored as its raw little-endian wire bytes — the same encoding an SDO
  // transfer carries. @c isScalarDataType(dataType) picks the field; zero (an empty @c rawValue)
  // is the type-appropriate default before the first read.
  //
  // The scalar cell is what a cyclic task reads: one relaxed atomic load, no lock, no allocation,
  // and no way to tell whether the RT exchange or a background SDO poll put the value there.
  //
  // It is a plain @c uint64_t accessed through @c std::atomic_ref rather than a
  // @c std::atomic<uint64_t> member, which would be neither copyable nor movable — and
  // @c DeviceParameter is both (@c Device::parameter and @c parametersOrdered hand out copies, and
  // entries are moved into the map and into growing vectors). That would force all five special
  // members to be written by hand, and *those* are the real hazard: this struct gains fields over
  // time, and a field added but forgotten in a hand-written copy constructor loses data silently
  // with nothing to catch it. Compiler-generated copies never forget. @c atomic_ref puts the
  // lock-free guarantee on the access instead of the storage, which is the guarantee that matters;
  // @c loadBits / @c storeBits below are the only way the field is ever touched. @c mutable because
  // a load is a const operation but @c std::atomic_ref needs a non-const lvalue.
  mutable uint64_t bits{0};         ///< Scalar value, LSB-aligned little-endian, zero-extended.
  std::vector<uint8_t> rawValue{};  ///< Non-scalar value (string / byte array) as wire bytes.
  // The two properties the cell's whole contract rests on. Lock-freedom is what makes a read safe
  // from the RT loop at all; the alignment precondition is atomic_ref's, and a platform where a
  // uint64_t member does not satisfy it must fail here rather than degrade silently.
  static_assert(std::atomic_ref<uint64_t>::is_always_lock_free,
                "the parameter cell must be lock-free so the RT path never blocks");
  static_assert(alignof(uint64_t) >= std::atomic_ref<uint64_t>::required_alignment,
                "the parameter cell must satisfy std::atomic_ref's alignment requirement");
  SyncState syncState{SyncState::Unknown};  ///< Freshness of the value relative to the device.
  std::optional<uint32_t> unit;             ///< ETG.1004 unit code, when reported.
  std::optional<DeviceParameterValue> defaultValue;  ///< Slave-reported default, when available.
  std::optional<DeviceParameterValue> minValue;      ///< Slave-reported minimum, when available.
  std::optional<DeviceParameterValue> maxValue;      ///< Slave-reported maximum, when available.

  /// @brief Returns the packed @c (index, subindex) key used in the parameter map.
  uint32_t key() const { return makeParameterKey(index, subindex); }

  /// @brief Loads the scalar cell. Lock-free, non-allocating, relaxed — safe from the RT loop.
  ///
  /// Relaxed is the whole requirement: the cell is a self-contained value with no companion state
  /// to order against, and a reader is by definition unsynchronised with the writer that published
  /// it. What it guarantees — that a torn or invented value is impossible, and that the last store
  /// becomes visible — is exactly what a cyclic task needs.
  uint64_t loadBits() const {
    return std::atomic_ref<uint64_t>(bits).load(std::memory_order_relaxed);
  }

  /// @brief Stores the scalar cell. Lock-free, non-allocating, relaxed — safe from the RT loop.
  void storeBits(uint64_t v) const {
    std::atomic_ref<uint64_t>(bits).store(v, std::memory_order_relaxed);
  }

  /// @brief Returns the current value as a @c DeviceParameterValue, built on the spot.
  ///
  /// The value is stored as wire bytes, so the variant is *reconstructed* here rather than held:
  /// one switch on the immutable @c dataType turns the bytes into the right alternative — the same
  /// mapping @c defaultValueForDataType has always used to choose it. That is what keeps a value in
  /// exactly one home, so the HTTP view and a cyclic task's view can never disagree.
  ///
  /// Off-RT only, by cost rather than by safety: it may allocate (a string or byte-array
  /// parameter). A cyclic task reads @c loadBits instead.
  DeviceParameterValue currentValue() const;

  /// @brief Reads the value as @p T straight out of the cell. Lock-free, non-allocating.
  ///
  /// The RT-callable read: one relaxed atomic load and a copy, with no lookup, no decode into a
  /// variant and no error string to build. @p T must be the alternative @c dataType maps to —
  /// @c int32_t for an INTEGER32 object, @c uint8_t for a BOOLEAN — and a mismatch, or a
  /// non-scalar parameter, reads as @c nullopt rather than as a wrong number.
  ///
  /// A parameter that has never been written reads as zero, not @c nullopt: @c syncState is where
  /// "never read" is recorded, and a control loop wants a number it can act on.
  template <typename T>
  std::optional<T> scalar() const {
    static_assert(std::is_arithmetic_v<T>, "the cell holds arithmetic types only");
    static_assert(sizeof(T) <= sizeof(uint64_t), "the cell holds at most eight bytes");
    // Constructing the type's zero is how the alternative is named; for every scalar it is a
    // register-sized value, and for the string/bytes cases an empty container — no allocation
    // either way, so this stays RT-safe.
    if (!std::holds_alternative<T>(defaultValueForDataType(dataType))) {
      return std::nullopt;
    }
    const uint64_t v = loadBits();
    T out{};
    std::memcpy(&out, &v, sizeof(T));
    return out;
  }

  /// @brief Returns the value's raw wire bytes at the object's declared width.
  ///
  /// The storage-level getter, and the inverse of @c setRawValue: a scalar is unpacked from the
  /// cell, a non-scalar returned as stored. Prefer it over @c encodeSdoBytes(dataType,
  /// currentValue()) — storage is already bytes, so that route decodes and re-encodes for nothing.
  ///
  /// One difference worth knowing for strings: this returns what the slave actually sent, padding
  /// included, where @c currentValue() strips the trailing NUL/space padding ETG.1000.6 allows.
  std::vector<uint8_t> rawValueBytes() const;

  /// @brief Replaces the stored value with @p bytes, its raw little-endian wire encoding.
  ///
  /// The storage-level setter: no coercion, no decoding, no @c syncState change — it takes the
  /// bytes an SDO upload or the process image already produced and puts them where
  /// @c isScalarDataType says they belong. @c setValue is the typed counterpart.
  void setRawValue(std::span<const uint8_t> bytes);

  /// @brief Returns the value as the requested type @p T.
  ///
  /// Type-exact: @p T must be the alternative @c dataType maps to (e.g. @c uint32_t for an
  /// UNSIGNED32 parameter — booleans are stored as @c uint8_t, see @c defaultValueForDataType).
  /// Use @c numeric() when you only need a number and do not care about the exact width.
  ///
  /// @return The value, or an error string if @p T is not the parameter's alternative.
  template <typename T>
  std::expected<T, std::string> getValue() const {
    const DeviceParameterValue v = currentValue();
    if (const auto* p = std::get_if<T>(&v)) {
      return *p;
    }
    return std::unexpected(
        std::format("parameter 0x{:04X}:{:02X} holds a different type", index, subindex));
  }

  /// @brief Returns the value coerced to a @c double, for any numeric parameter.
  ///
  /// The "don't worry about the type" accessor — handy because most parameters are
  /// numbers of varying width.
  ///
  /// @return The numeric value, or an error string if @c value is a string / raw bytes.
  std::expected<double, std::string> numeric() const;

  /// @brief Sets @c value, coercing @p v into the parameter's declared data type.
  ///
  /// Numeric inputs are @c static_cast into the alternative matching @c dataType, so
  /// @c setValue(5) works regardless of whether the parameter is UNSIGNED8 or
  /// UNSIGNED32 — unlike a bare variant assignment, which would store the wrong
  /// alternative. Does not touch @c syncState; the caller decides that.
  ///
  /// @return Void on success, or an error string when @p v cannot be coerced (e.g. a
  ///         string into a numeric parameter, or a number into a string parameter).
  std::expected<void, std::string> setValue(const DeviceParameterValue& v);

  /// @brief Strongly-typed @c setValue overload; wraps @p v and coerces as above.
  template <typename T>
  std::expected<void, std::string> setValue(const T& v) {
    return setValue(DeviceParameterValue{v});
  }

  /// @brief Whether the object is readable in any state (ETG.1000.6 ObjAccess read bits 0-2).
  ///
  /// Write-only objects (e.g. a command/trigger sub-object) report no read bits; an SDO upload of
  /// one aborts, so a bulk value refresh should skip them rather than log a spurious failure.
  bool isReadable() const { return (access & 0x07) != 0; }

  /// @brief Whether @p v lies within @c [minValue, maxValue].
  ///
  /// Bounds are compared via numeric coercion. Returns @c true when @p v is
  /// non-numeric or when a bound is unset/non-numeric (i.e. no constraint to violate).
  bool inRange(const DeviceParameterValue& v) const;

  /// @brief Returns @p v clamped to @c [minValue, maxValue].
  ///
  /// Clamps numerically and returns a value of the same alternative as @p v. Returns
  /// @p v unchanged when it is non-numeric or no numeric bounds apply.
  DeviceParameterValue clampToRange(const DeviceParameterValue& v) const;
};

/// @brief Decodes a raw SDO byte sequence according to an ETG.1020 data type code.
///
/// @param dataType  ETG.1020 data type code (e.g. @c 0x0007 = UNSIGNED32).
/// @param bytes     Bytes returned by an SDO upload.
/// @return The decoded value on success, or an error string if @p dataType is
///         unsupported or @p bytes is too short for the type.
std::expected<DeviceParameterValue, std::string> decodeSdoBytes(uint16_t dataType,
                                                                std::span<const uint8_t> bytes);

/// @brief Serialises a @c DeviceParameterValue to raw SDO bytes — the inverse of
///        @c decodeSdoBytes.
///
/// The active variant alternative must match @p dataType (numeric types expect the
/// scalar of the corresponding width; string types expect @c std::string). Strings are
/// written as their raw bytes with **no trailing NUL** so that
/// @c decodeSdoBytes(t, encodeSdoBytes(t, v)) round-trips exactly. Unknown / composite
/// types expect a @c std::vector<uint8_t> and are copied verbatim.
///
/// @param dataType  ETG.1020 data type code.
/// @param value     Value to serialise.
/// @return The encoded bytes on success, or an error string if @p value's alternative
///         does not match @p dataType.
std::expected<std::vector<uint8_t>, std::string> encodeSdoBytes(uint16_t dataType,
                                                                const DeviceParameterValue& value);

/// @brief Serialises a DeviceParameter to JSON.
///
/// Produces an object with keys `index`, `subindex`, `name`, `objectCode`,
/// `dataType`, `bitLength`, `access`, and `value`.
void to_json(nlohmann::json& j, const DeviceParameter& p);

}  // namespace mm::node
