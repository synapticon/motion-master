#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace mm::comm {

/// @brief CoE object dictionary data type codes.
///
/// Maps the 16-bit DataType field returned by @c ecx_readOE to its symbolic name.
/// Base numbering follows SOEM (@c ec_type.h) / CiA 301 — what the wire actually
/// reports — extended with the record/array/DEFTYPE codes from ETG.1020 v1.6.0
/// (§26 Base Data Types, Tables 119-121) plus ETG.5001 / ETG.5120 cross-references.
/// Where SOEM and ETG.1020 disagree (notably 0x000B, see below) SOEM's numbering wins.
enum class ObjectDataType : uint16_t {
  UNSPECIFIED = 0x0000,  ///< Undefined or unknown data type.

  BOOLEAN = 0x0001,  ///< Boolean value (true or false), transported as a byte.
  BYTE = 0x001E,     ///< 8-bit unsigned integer.
  WORD = 0x001F,     ///< 16-bit unsigned integer.
  DWORD = 0x0020,    ///< 32-bit unsigned integer.

  BIT1 = 0x0030,   ///< 1-bit field.
  BIT2 = 0x0031,   ///< 2-bit field.
  BIT3 = 0x0032,   ///< 3-bit field.
  BIT4 = 0x0033,   ///< 4-bit field.
  BIT5 = 0x0034,   ///< 5-bit field.
  BIT6 = 0x0035,   ///< 6-bit field.
  BIT7 = 0x0036,   ///< 7-bit field.
  BIT8 = 0x0037,   ///< 8-bit field.
  BIT9 = 0x0038,   ///< 9-bit field.
  BIT10 = 0x0039,  ///< 10-bit field.
  BIT11 = 0x003A,  ///< 11-bit field.
  BIT12 = 0x003B,  ///< 12-bit field.
  BIT13 = 0x003C,  ///< 13-bit field.
  BIT14 = 0x003D,  ///< 14-bit field.
  BIT15 = 0x003E,  ///< 15-bit field.
  BIT16 = 0x003F,  ///< 16-bit field.

  BITARR8 = 0x002D,   ///< Array of 8 bits.
  BITARR16 = 0x002E,  ///< Array of 16 bits.
  BITARR32 = 0x002F,  ///< Array of 32 bits.

  INTEGER8 = 0x0002,   ///< 8-bit signed integer.
  INTEGER16 = 0x0003,  ///< 16-bit signed integer.
  INTEGER24 = 0x0010,  ///< 24-bit signed integer.
  INTEGER32 = 0x0004,  ///< 32-bit signed integer.
  INTEGER40 = 0x0012,  ///< 40-bit signed integer.
  INTEGER48 = 0x0013,  ///< 48-bit signed integer.
  INTEGER56 = 0x0014,  ///< 56-bit signed integer.
  INTEGER64 = 0x0015,  ///< 64-bit signed integer.

  UNSIGNED8 = 0x0005,   ///< 8-bit unsigned integer.
  UNSIGNED16 = 0x0006,  ///< 16-bit unsigned integer.
  UNSIGNED24 = 0x0016,  ///< 24-bit unsigned integer.
  UNSIGNED32 = 0x0007,  ///< 32-bit unsigned integer.
  UNSIGNED40 = 0x0018,  ///< 40-bit unsigned integer.
  UNSIGNED48 = 0x0019,  ///< 48-bit unsigned integer.
  UNSIGNED56 = 0x001A,  ///< 56-bit unsigned integer.
  UNSIGNED64 = 0x001B,  ///< 64-bit unsigned integer.

  REAL32 = 0x0008,  ///< 32-bit IEEE 754 floating point.
  REAL64 = 0x0011,  ///< 64-bit IEEE 754 floating point.

  GUID = 0x001D,  ///< 128-bit globally unique identifier.

  VISIBLE_STRING = 0x0009,  ///< Null-terminated ASCII string.
  OCTET_STRING = 0x000A,    ///< Variable-length byte array.
  // 0x000B is UNICODE_STRING per SOEM (ec_type.h) / CiA 301, which is what ecx_readOE and
  // SOMANET drives actually report.  ETG.1020 v1.6.0 instead lists 0x000B as ARRAY_OF_UINT and
  // moves UNICODE_STRING to 0x0268 — deliberately not followed here; we decode the wire.
  UNICODE_STRING = 0x000B,  ///< UCS-2 string (SOEM/CiA numbering; see note above).

  TIME_OF_DAY = 0x000C,      ///< Time of day (ETG.1020).
  TIME_DIFFERENCE = 0x000D,  ///< Time difference (ETG.1020).
  DOMAIN = 0x000F,           ///< Application-specific domain (ETG.1020).

  ARRAY_OF_INT = 0x0260,       ///< Sequence of INT (IEC 61131-3).
  ARRAY_OF_SINT = 0x0261,      ///< Sequence of SINT.
  ARRAY_OF_DINT = 0x0262,      ///< Sequence of DINT.
  ARRAY_OF_UDINT = 0x0263,     ///< Sequence of UDINT.
  ARRAY_OF_BITARR8 = 0x0264,   ///< Sequence of BITARR8.
  ARRAY_OF_BITARR16 = 0x0265,  ///< Sequence of BITARR16.
  ARRAY_OF_BITARR32 = 0x0266,  ///< Sequence of BITARR32.
  ARRAY_OF_USINT = 0x0267,     ///< Sequence of USINT.
  ARRAY_OF_REAL = 0x0269,      ///< Sequence of REAL.
  ARRAY_OF_LREAL = 0x026A,     ///< Sequence of LREAL.

  PDO_MAPPING = 0x0021,             ///< PDO mapping (ETG.1000).
  IDENTITY = 0x0023,                ///< Identity object (ETG.1000).
  COMMAND_PAR = 0x0025,             ///< Command parameter (ETG.1000).
  PDO_PARAMETER = 0x0027,           ///< PDO parameter (ETG.1020).
  ENUM = 0x0028,                    ///< Enumeration (ETG.1020).
  SM_SYNCHRONISATION = 0x0029,      ///< Sync manager synchronisation (ETG.1000).
  RECORD = 0x002A,                  ///< Generic record structure.
  BACKUP_PARAMETER = 0x002B,        ///< Backup parameter (ETG.1020).
  MODULAR_DEVICE_PROFILE = 0x002C,  ///< Modular device profile (ETG.5001).

  ERROR_SETTING = 0x0281,           ///< Error setting (ETG.1020).
  DIAGNOSIS_HISTORY = 0x0282,       ///< Diagnosis history (ETG.1020).
  EXTERNAL_SYNC_STATUS = 0x0283,    ///< External sync status (ETG.1020).
  EXTERNAL_SYNC_SETTINGS = 0x0284,  ///< External sync settings (ETG.1020).
  DEFTYPE_FSOEFRAME = 0x0285,       ///< FSoE frame (ETG.5120).
  DEFTYPE_FSOECOMMPAR = 0x0286,     ///< FSoE communication parameters (ETG.5120).

  UTYPE_START = 0x0800,  ///< Start of user-defined type range.
  UTYPE_END = 0x0FFF,    ///< End of user-defined type range.
};

/// @brief Metadata for a single object dictionary data type.
struct ObjectDataTypeInfo {
  uint16_t code = 0;      ///< ETG.1020 data type code.
  std::string_view name;  ///< Symbolic name (e.g. @c "UNSIGNED32").
  uint16_t bitSize = 0;   ///< Bit width of one element; @c 0 for variable-length types.
};

/// @brief Serialises an ObjectDataTypeInfo to JSON.
///
/// Produces an object with keys @c code, @c name, @c bitSize.  Participates in
/// nlohmann ADL so that @c nlohmann::json(kObjectDataTypes) works automatically.
void to_json(nlohmann::json& j, const ObjectDataTypeInfo& info);

/// @brief Catalogue of ETG.1020 object dictionary data types.
inline constexpr auto kObjectDataTypes = std::to_array<ObjectDataTypeInfo>({
    {0x0000, "UNSPECIFIED", 0},
    {0x0001, "BOOLEAN", 1},
    {0x0002, "INTEGER8", 8},
    {0x0003, "INTEGER16", 16},
    {0x0004, "INTEGER32", 32},
    {0x0005, "UNSIGNED8", 8},
    {0x0006, "UNSIGNED16", 16},
    {0x0007, "UNSIGNED32", 32},
    {0x0008, "REAL32", 32},
    {0x0009, "VISIBLE_STRING", 0},
    {0x000A, "OCTET_STRING", 0},
    {0x000B, "UNICODE_STRING", 0},
    {0x000C, "TIME_OF_DAY", 48},
    {0x000D, "TIME_DIFFERENCE", 48},
    {0x000F, "DOMAIN", 0},
    {0x0010, "INTEGER24", 24},
    {0x0011, "REAL64", 64},
    {0x0012, "INTEGER40", 40},
    {0x0013, "INTEGER48", 48},
    {0x0014, "INTEGER56", 56},
    {0x0015, "INTEGER64", 64},
    {0x0016, "UNSIGNED24", 24},
    {0x0018, "UNSIGNED40", 40},
    {0x0019, "UNSIGNED48", 48},
    {0x001A, "UNSIGNED56", 56},
    {0x001B, "UNSIGNED64", 64},
    {0x001D, "GUID", 128},
    {0x001E, "BYTE", 8},
    {0x001F, "WORD", 16},
    {0x0020, "DWORD", 32},
    {0x0021, "PDO_MAPPING", 0},
    {0x0023, "IDENTITY", 0},
    {0x0025, "COMMAND_PAR", 0},
    {0x0027, "PDO_PARAMETER", 0},
    {0x0028, "ENUM", 0},
    {0x0029, "SM_SYNCHRONISATION", 0},
    {0x002A, "RECORD", 0},
    {0x002B, "BACKUP_PARAMETER", 0},
    {0x002C, "MODULAR_DEVICE_PROFILE", 0},
    {0x002D, "BITARR8", 8},
    {0x002E, "BITARR16", 16},
    {0x002F, "BITARR32", 32},
    {0x0030, "BIT1", 1},
    {0x0031, "BIT2", 2},
    {0x0032, "BIT3", 3},
    {0x0033, "BIT4", 4},
    {0x0034, "BIT5", 5},
    {0x0035, "BIT6", 6},
    {0x0036, "BIT7", 7},
    {0x0037, "BIT8", 8},
    {0x0038, "BIT9", 9},
    {0x0039, "BIT10", 10},
    {0x003A, "BIT11", 11},
    {0x003B, "BIT12", 12},
    {0x003C, "BIT13", 13},
    {0x003D, "BIT14", 14},
    {0x003E, "BIT15", 15},
    {0x003F, "BIT16", 16},
    {0x0260, "ARRAY_OF_INT", 0},
    {0x0261, "ARRAY_OF_SINT", 0},
    {0x0262, "ARRAY_OF_DINT", 0},
    {0x0263, "ARRAY_OF_UDINT", 0},
    {0x0264, "ARRAY_OF_BITARR8", 0},
    {0x0265, "ARRAY_OF_BITARR16", 0},
    {0x0266, "ARRAY_OF_BITARR32", 0},
    {0x0267, "ARRAY_OF_USINT", 0},
    {0x0269, "ARRAY_OF_REAL", 0},
    {0x026A, "ARRAY_OF_LREAL", 0},
    {0x0281, "ERROR_SETTING", 0},
    {0x0282, "DIAGNOSIS_HISTORY", 0},
    {0x0283, "EXTERNAL_SYNC_STATUS", 0},
    {0x0284, "EXTERNAL_SYNC_SETTINGS", 0},
    {0x0285, "DEFTYPE_FSOEFRAME", 0},
    {0x0286, "DEFTYPE_FSOECOMMPAR", 0},
});

/// @brief Returns the symbolic name of @p code, or @c "UNKNOWN" if it is not in the table.
constexpr std::string_view objectDataTypeName(uint16_t code) {
  const auto it = std::find_if(kObjectDataTypes.begin(), kObjectDataTypes.end(),
                               [code](const ObjectDataTypeInfo& e) { return e.code == code; });
  return it != kObjectDataTypes.end() ? it->name : std::string_view{"UNKNOWN"};
}

/// @brief Returns the declared bit width of @p code, or @c 0 if unknown / variable-length.
constexpr uint16_t objectDataTypeBitSize(uint16_t code) {
  const auto it = std::find_if(kObjectDataTypes.begin(), kObjectDataTypes.end(),
                               [code](const ObjectDataTypeInfo& e) { return e.code == code; });
  return it != kObjectDataTypes.end() ? it->bitSize : uint16_t{0};
}

}  // namespace mm::comm
