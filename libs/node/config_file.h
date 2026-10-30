#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "node/device_parameter.h"

namespace mm::node {

/// @file
/// @brief A SOMANET configuration file, @c config.csv: how the drive's own parser reads it, how it
///        is written back, and which of its rows the drive loads.
///
/// Pure transform over text, so every rule is unit-testable without a drive. The rules follow the
/// drive's parser, @c sc_base/module_storage/src/config_parser.xc in somanet_software, and its
/// writer, @c storage_service.xc. Where that parser accepts something silently, this one accepts it
/// too and reports it as a problem, so that a client can say before a load what the drive will do.

/// @brief The name of the configuration file on a SOMANET drive.
inline constexpr std::string_view kConfigFileName = "config.csv";

/// @brief One object row: @c 0xIIII, @c S, @c value.
struct ConfigFileRow {
  std::size_t line = 0;  ///< 1-based line number in the file.
  uint16_t index{};
  uint8_t subindex{};
  /// The value as the file holds it, between the second comma and the end of the line or a @c #.
  /// Leading and trailing spaces are kept, because a string value keeps its trailing ones.
  std::string value;

  /// @brief The value without leading and trailing white space, which is how a number is read.
  std::string_view trimmedValue() const;
};

/// @brief A line that the drive ignores, or reads in a way that is probably not meant.
struct ConfigFileProblem {
  std::size_t line = 0;  ///< 1-based line number in the file.
  std::string message;
};

/// @brief A parsed configuration file.
///
/// @c lines keeps the file exactly as it was read, each line with its terminator, so that a file
/// written back unchanged is byte-for-byte the same. @c rows and the rest are views of it.
struct ConfigFile {
  std::vector<std::string> lines;
  /// The value of @c META,FORMAT. The drive takes a file without it as format 0, which reads
  /// REAL32 values differently.
  int format = 0;
  /// The text of each line that starts with @c #, without the @c # and one space. The drive writes
  /// 0x1008 and then 0x100A there, and never loads them.
  std::vector<std::string> comments;
  std::vector<ConfigFileRow> rows;
  std::vector<ConfigFileProblem> problems;

  /// @brief The firmware version the drive wrote in a comment, such as "v5.6.11", or nothing.
  std::optional<std::string> firmwareVersion() const;

  /// @brief The device name the drive wrote in the first comment, from 0x1008, or nothing.
  std::optional<std::string> deviceName() const;
};

/// @brief Parses a configuration file. Never fails: a line the drive would ignore is a problem.
ConfigFile parseConfigFile(std::string_view text);

/// @brief The file as text, which is @c lines joined.
std::string writeConfigFile(const ConfigFile& file);

/// @brief One row as the drive writes it: @c "0x%04x, %2d,  value" and a line feed.
std::string formatConfigFileRow(uint16_t index, uint8_t subindex, std::string_view value);

/// @brief The file with only the rows at @p keep. Every other line stays.
ConfigFile selectConfigFileRows(const ConfigFile& file,
                                const std::vector<std::pair<uint16_t, uint8_t>>& keep);

/// @brief @p base with each row of @p update in it: a row at an address that @p base has replaces
///        that row's value in place, and a row at a new address is added at the end.
ConfigFile mergeConfigFiles(const ConfigFile& base, const ConfigFile& update);

/// @brief A row whose value differs between two files.
struct ConfigFileDifference {
  uint16_t index{};
  uint8_t subindex{};
  std::string first;   ///< The value in the first file.
  std::string second;  ///< The value in the second file.
};

/// @brief How the rows of two files differ, by address. A later row at one address wins.
struct ConfigFileComparison {
  std::vector<ConfigFileRow> onlyInFirst;
  std::vector<ConfigFileRow> onlyInSecond;
  std::vector<ConfigFileDifference> differing;  ///< In both, with values that are not the same.
};

/// @brief Compares the rows of two files. Two values are the same when they are the same number,
///        so that "1.50" and "1.5" match, or else the same text.
ConfigFileComparison compareConfigFileRows(const std::vector<ConfigFileRow>& first,
                                           const std::vector<ConfigFileRow>& second);

/// @brief What the drive does with a row.
enum class ConfigRowOutcome {
  kLoads,          ///< The drive writes the value to the object.
  kUnknownObject,  ///< The object dictionary has no such object. The drive skips the row.
  kNotLoadable,    ///< The object exists, but the drive skips the row. See @c configRowOutcome.
};

/// @brief The outcome as a client reads it: @c loads, @c unknown-object or @c not-loadable.
std::string_view toString(ConfigRowOutcome outcome);

/// @brief What the drive does with a row at @p index, for the object @p object, or for no object.
///
/// The drive skips a row, with no error and no report, when:
/// - the object does not exist;
/// - the index is below 0x2000 and is not one of 0x1600-0x1603, 0x1A00-0x1A03 and 0x1C10-0x1C13;
/// - the index is 0x2000 or above, and the entry has no backup flag, ObjAccess bit 8;
/// - the entry's data type is not BOOLEAN, an 8-, 16- or 32-bit integer, REAL32 or
///   VISIBLE_STRING, because the drive's parser has no case for it.
ConfigRowOutcome configRowOutcome(uint16_t index, const DeviceParameter* object);

/// @brief Reads a row's value as the drive does, into the type of @p object.
///
/// Stricter than the drive in one way: the drive writes 0 for a number it cannot read, and this
/// refuses it, so that a load never writes a value nobody wrote. A value out of the type's range is
/// refused for the same reason. @p format picks how a REAL32 value is read.
std::expected<DeviceParameterValue, std::string> parseConfigFileValue(const DeviceParameter& object,
                                                                      std::string_view value,
                                                                      int format);

/// @brief Whether @p index is in a safety area, 0x2600-0x26FF or 0x6600-0x67EF.
///
/// A drive with a safety module keeps those objects on the module, which has its own load path.
bool isSafetyConfigIndex(uint16_t index);

}  // namespace mm::node
