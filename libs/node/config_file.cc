#include "node/config_file.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "comm/object_data_types.h"

namespace mm::node {

namespace {

using mm::comm::ObjectDataType;

constexpr std::string_view kWhiteSpace = " \t\v\f";
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

// The drive writes REAL32 values with this many fractional digits, and reads no more than that.
constexpr int kFractionalDigits = 10;

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(kWhiteSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(kWhiteSpace);
  return text.substr(first, last - first + 1);
}

std::string_view trimLeft(std::string_view text) {
  const auto first = text.find_first_not_of(kWhiteSpace);
  return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

std::vector<std::string_view> splitFields(std::string_view text) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const auto comma = text.find(',', start);
    fields.push_back(
        text.substr(start, comma == std::string_view::npos ? text.npos : comma - start));
    if (comma == std::string_view::npos) {
      return fields;
    }
    start = comma + 1;
  }
}

template <typename T>
std::optional<T> parseWhole(std::string_view text, int base) {
  T value{};
  const auto* first = std::to_address(text.begin());
  const auto* end = std::to_address(text.end());
  const auto [ptr, ec] = std::from_chars(first, end, value, base);
  if (ec != std::errc() || ptr != end || text.empty()) {
    return std::nullopt;
  }
  return value;
}

std::optional<uint16_t> parseIndex(std::string_view text) {
  text = trim(text);
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  return parseWhole<uint16_t>(text, 16);
}

std::optional<uint8_t> parseSubindex(std::string_view text) {
  return parseWhole<uint8_t>(trim(text), 10);
}

bool isWhitelistedCommunicationIndex(uint16_t index) {
  return (index >= 0x1600 && index <= 0x1603) || (index >= 0x1A00 && index <= 0x1A03) ||
         (index >= 0x1C10 && index <= 0x1C13);
}

bool isLoadableDataType(uint16_t dataType) {
  switch (static_cast<ObjectDataType>(dataType)) {
    case ObjectDataType::BOOLEAN:
    case ObjectDataType::INTEGER8:
    case ObjectDataType::INTEGER16:
    case ObjectDataType::INTEGER32:
    case ObjectDataType::UNSIGNED8:
    case ObjectDataType::UNSIGNED16:
    case ObjectDataType::UNSIGNED32:
    case ObjectDataType::REAL32:
    case ObjectDataType::VISIBLE_STRING:
      return true;
    default:
      return false;
  }
}

template <typename T>
std::expected<DeviceParameterValue, std::string> parseInteger(std::string_view text) {
  text = trim(text);
  if (text.starts_with('+')) {
    text.remove_prefix(1);
  }
  const auto value = parseWhole<int64_t>(text, 10);
  if (!value) {
    return std::unexpected(std::format("'{}' is not a whole number", text));
  }
  if (!std::in_range<T>(*value)) {
    return std::unexpected(
        std::format("{} is outside the range of the object's data type", *value));
  }
  return static_cast<T>(*value);
}

// The drive's safe_strtof, for files of format 1 and newer: an optional minus sign, digits, and an
// optional point with digits. Any other character makes the drive read 0, which is refused here.
std::expected<DeviceParameterValue, std::string> parseReal(std::string_view text) {
  text = trimLeft(text);
  if (text.empty()) {
    return std::unexpected("the value is empty");
  }
  const bool negative = text.starts_with('-');
  if (negative) {
    text.remove_prefix(1);
  }
  uint64_t integerPart = 0;
  uint64_t decimalPart = 0;
  int decimalDigits = 0;
  bool point = false;
  bool digits = false;
  for (const char c : text) {
    if (c == '.' && !point) {
      point = true;
    } else if (c >= '0' && c <= '9') {
      digits = true;
      if (!point) {
        integerPart = integerPart * 10 + static_cast<uint64_t>(c - '0');
      } else if (decimalDigits < kFractionalDigits) {
        decimalPart = decimalPart * 10 + static_cast<uint64_t>(c - '0');
        ++decimalDigits;
      }
    } else {
      return std::unexpected(std::format(
          "'{}' holds '{}'; the drive reads a REAL32 value with any character other than a "
          "digit, a point or a leading minus as 0",
          text, c));
    }
  }
  if (!digits) {
    return std::unexpected(std::format("'{}' is not a number", text));
  }
  uint64_t divisor = 1;
  for (int i = 0; i < decimalDigits; ++i) {
    divisor *= 10;
  }
  // The same arithmetic as the drive, so that the value read back compares equal.
  float result = static_cast<float>(integerPart) +
                 static_cast<float>(decimalPart) / static_cast<float>(divisor);
  return negative ? -result : result;
}

// Files of format 0 were written with the leading zeros of the fraction left out, and the drive
// reads them back with a fixed number of fractional digits: "0.10000" is 0.001.
std::expected<DeviceParameterValue, std::string> parseRealFormat0(std::string_view text) {
  text = trim(text);
  const auto point = text.find('.');
  const auto whole = parseWhole<int64_t>(text.substr(0, point), 10);
  const auto fraction = point == std::string_view::npos
                            ? std::optional<int64_t>{0}
                            : parseWhole<int64_t>(text.substr(point + 1), 10);
  if (!whole || !fraction) {
    return std::unexpected(std::format("'{}' is not a number", text));
  }
  float result = static_cast<float>(std::llabs(*whole)) +
                 static_cast<float>(static_cast<double>(*fraction) * 1e-10);
  return *whole < 0 ? -result : result;
}

}  // namespace

std::string_view ConfigFileRow::trimmedValue() const { return trim(value); }

std::optional<std::string> ConfigFile::firmwareVersion() const {
  for (const auto& comment : comments) {
    const auto text = trim(comment);
    if (text.size() > 1 && text[0] == 'v' && text[1] >= '0' && text[1] <= '9') {
      return std::string(text);
    }
  }
  return std::nullopt;
}

std::optional<std::string> ConfigFile::deviceName() const {
  if (comments.empty() || firmwareVersion() == std::string(trim(comments.front()))) {
    return std::nullopt;
  }
  return std::string(trim(comments.front()));
}

ConfigFile parseConfigFile(std::string_view text) {
  ConfigFile file;
  for (std::size_t pos = 0; pos < text.size();) {
    const auto feed = text.find('\n', pos);
    const auto end = feed == std::string_view::npos ? text.size() : feed + 1;
    file.lines.emplace_back(text.substr(pos, end - pos));
    pos = end;
  }

  bool rowsStarted = false;
  for (std::size_t i = 0; i < file.lines.size(); ++i) {
    const std::size_t number = i + 1;
    std::string_view line = file.lines[i];
    if (i == 0 && line.starts_with(kByteOrderMark)) {
      line.remove_prefix(kByteOrderMark.size());
    }
    while (line.ends_with('\n') || line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (trim(line).empty()) {
      continue;
    }
    if (line.starts_with('#')) {
      line.remove_prefix(1);
      if (line.starts_with(' ')) {
        line.remove_prefix(1);
      }
      file.comments.emplace_back(line);
      continue;
    }
    // A '#' ends the line, so a value cannot hold one.
    const auto content = line.substr(0, line.find('#'));
    const auto fields = splitFields(content);

    if (fields[0] == "META") {
      if (rowsStarted) {
        file.problems.push_back(
            {number, "a META line after the first row; the drive ignores this line"});
      } else if (fields.size() != 3 || trim(fields[1]) != "FORMAT") {
        file.problems.push_back({number, "an unknown META line; the drive ignores this line"});
      } else if (const auto format = parseWhole<int>(trim(fields[2]), 10); format) {
        file.format = *format;
      } else {
        file.problems.push_back({number, "META,FORMAT has no whole number; the drive reads 0"});
      }
      continue;
    }
    if (fields.size() < 3) {
      file.problems.push_back(
          {number, "a row needs an index, a subindex and a value; the drive ignores this line"});
      continue;
    }
    if (fields.size() > 3) {
      file.problems.push_back({number, "a value cannot hold a comma; the drive ignores this line"});
      continue;
    }
    const auto index = parseIndex(fields[0]);
    if (!index || *index < 0x1000) {
      file.problems.push_back(
          {number, std::format("'{}' is not an object index from 0x1000 to 0xFFFF; the drive "
                               "ignores this line",
                               trim(fields[0]))});
      continue;
    }
    const auto subindex = parseSubindex(fields[1]);
    if (!subindex) {
      file.problems.push_back(
          {number, std::format("'{}' is not a subindex from 0 to 255", trim(fields[1]))});
      continue;
    }
    rowsStarted = true;
    file.rows.push_back(ConfigFileRow{
        .line = number, .index = *index, .subindex = *subindex, .value = std::string(fields[2])});
  }
  return file;
}

std::string writeConfigFile(const ConfigFile& file) {
  std::string text;
  for (const auto& line : file.lines) {
    text += line;
  }
  return text;
}

std::string formatConfigFileRow(uint16_t index, uint8_t subindex, std::string_view value) {
  return std::format("0x{:04x}, {:2d},  {}\n", index, subindex, value);
}

ConfigFile selectConfigFileRows(const ConfigFile& file,
                                const std::vector<std::pair<uint16_t, uint8_t>>& keep) {
  std::vector<bool> drop(file.lines.size(), false);
  for (const auto& row : file.rows) {
    const bool kept = std::ranges::find(keep, std::pair{row.index, row.subindex}) != keep.end();
    drop[row.line - 1] = !kept;
  }
  std::string text;
  for (std::size_t i = 0; i < file.lines.size(); ++i) {
    if (!drop[i]) {
      text += file.lines[i];
    }
  }
  return parseConfigFile(text);
}

ConfigFile mergeConfigFiles(const ConfigFile& base, const ConfigFile& update) {
  std::vector<std::string> lines = base.lines;
  std::map<std::pair<uint16_t, uint8_t>, std::vector<std::size_t>> linesAt;
  for (const auto& row : base.rows) {
    linesAt[{row.index, row.subindex}].push_back(row.line - 1);
  }
  std::string added;
  for (const auto& row : update.rows) {
    const auto formatted = formatConfigFileRow(row.index, row.subindex, row.trimmedValue());
    if (const auto it = linesAt.find({row.index, row.subindex}); it != linesAt.end()) {
      for (const auto i : it->second) {
        lines[i] = formatted;
      }
    } else {
      added += formatted;
    }
  }
  std::string text;
  for (const auto& line : lines) {
    text += line;
  }
  if (!added.empty() && !text.empty() && !text.ends_with('\n')) {
    text += '\n';
  }
  return parseConfigFile(text + added);
}

std::string_view toString(ConfigRowOutcome outcome) {
  switch (outcome) {
    case ConfigRowOutcome::kLoads:
      return "loads";
    case ConfigRowOutcome::kUnknownObject:
      return "unknown-object";
    case ConfigRowOutcome::kNotLoadable:
      return "not-loadable";
  }
  return "unknown";
}

ConfigRowOutcome configRowOutcome(uint16_t index, const DeviceParameter* object) {
  if (object == nullptr) {
    return ConfigRowOutcome::kUnknownObject;
  }
  if (index < 0x2000 && !isWhitelistedCommunicationIndex(index)) {
    return ConfigRowOutcome::kNotLoadable;
  }
  constexpr uint16_t kBackupFlag = 0x0100;
  if (index >= 0x2000 && (object->access & kBackupFlag) == 0) {
    return ConfigRowOutcome::kNotLoadable;
  }
  if (!isLoadableDataType(object->dataType)) {
    return ConfigRowOutcome::kNotLoadable;
  }
  return ConfigRowOutcome::kLoads;
}

std::expected<DeviceParameterValue, std::string> parseConfigFileValue(const DeviceParameter& object,
                                                                      std::string_view value,
                                                                      int format) {
  switch (static_cast<ObjectDataType>(object.dataType)) {
    case ObjectDataType::BOOLEAN: {
      auto parsed = parseInteger<uint8_t>(value);
      if (parsed && std::get<uint8_t>(*parsed) > 1) {
        return std::unexpected("a BOOLEAN value is 0 or 1");
      }
      return parsed;
    }
    case ObjectDataType::INTEGER8:
      return parseInteger<int8_t>(value);
    case ObjectDataType::INTEGER16:
      return parseInteger<int16_t>(value);
    case ObjectDataType::INTEGER32:
      return parseInteger<int32_t>(value);
    case ObjectDataType::UNSIGNED8:
      return parseInteger<uint8_t>(value);
    case ObjectDataType::UNSIGNED16:
      return parseInteger<uint16_t>(value);
    case ObjectDataType::UNSIGNED32:
      return parseInteger<uint32_t>(value);
    case ObjectDataType::REAL32:
      return format == 0 ? parseRealFormat0(value) : parseReal(value);
    case ObjectDataType::VISIBLE_STRING:
      return std::string(trimLeft(value));
    default:
      return std::unexpected(
          std::format("the drive does not load data type 0x{:04X} from a file", object.dataType));
  }
}

namespace {

// Two values are the same when they are the same number, so that "1.50" and "1.5" match, or else
// the same text.
bool sameValue(std::string_view first, std::string_view second) {
  if (first == second) {
    return true;
  }
  const std::string a(first);
  const std::string b(second);
  char* endA = nullptr;
  char* endB = nullptr;
  const double x = std::strtod(a.c_str(), &endA);
  const double y = std::strtod(b.c_str(), &endB);
  const bool numbers = !a.empty() && !b.empty() && *endA == '\0' && *endB == '\0';
  return numbers && x == y;
}

std::map<std::pair<uint16_t, uint8_t>, const ConfigFileRow*> byAddress(
    const std::vector<ConfigFileRow>& rows) {
  // A later row at the same address wins, as it does on the drive.
  std::map<std::pair<uint16_t, uint8_t>, const ConfigFileRow*> map;
  for (const auto& row : rows) {
    map[{row.index, row.subindex}] = &row;
  }
  return map;
}

}  // namespace

ConfigFileComparison compareConfigFileRows(const std::vector<ConfigFileRow>& first,
                                           const std::vector<ConfigFileRow>& second) {
  const auto a = byAddress(first);
  const auto b = byAddress(second);
  ConfigFileComparison result;
  for (const auto& [address, row] : a) {
    const auto other = b.find(address);
    if (other == b.end()) {
      result.onlyInFirst.push_back(*row);
    } else if (!sameValue(row->trimmedValue(), other->second->trimmedValue())) {
      result.differing.push_back({.index = address.first,
                                  .subindex = address.second,
                                  .first = std::string(row->trimmedValue()),
                                  .second = std::string(other->second->trimmedValue())});
    }
  }
  for (const auto& [address, row] : b) {
    if (!a.contains(address)) {
      result.onlyInSecond.push_back(*row);
    }
  }
  return result;
}

namespace {

std::string liveText(const DeviceParameterValue& live) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return v;
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          return {};
        } else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>) {
          return std::to_string(static_cast<int>(v));
        } else {
          return std::format("{}", v);
        }
      },
      live);
}

}  // namespace

bool sameConfigValue(const DeviceParameter& object, std::string_view value, int format,
                     const DeviceParameterValue& live) {
  const auto parsed = parseConfigFileValue(object, value, format);
  if (!parsed) {
    return sameValue(trim(value), liveText(live));
  }
  if (const auto* a = std::get_if<float>(&*parsed); a != nullptr) {
    if (const auto* b = std::get_if<float>(&live); b != nullptr) {
      const double scale = std::max({1.0, std::fabs(double{*a}), std::fabs(double{*b})});
      return std::fabs(double{*a} - double{*b}) <= 1e-6 * scale;
    }
  }
  return *parsed == live;
}

namespace {

// Two values of files are the same when the drive reads them as the same value. A value the drive
// cannot read is compared as a number or as text.
bool sameFileValues(const DeviceParameter* object, std::string_view a, int formatA,
                    std::string_view b, int formatB) {
  if (object != nullptr) {
    const auto parsedB = parseConfigFileValue(*object, b, formatB);
    if (parsedB) {
      return sameConfigValue(*object, a, formatA, *parsedB);
    }
  }
  return sameValue(trim(a), trim(b));
}

}  // namespace

std::vector<ConfigCompareRow> compareConfiguration(
    const ConfigFile& first, const ConfigFile* second,
    const std::map<std::pair<uint16_t, uint8_t>, ConfigLiveObject>& objects) {
  const auto a = byAddress(first.rows);
  const auto b = second != nullptr ? byAddress(second->rows)
                                   : std::map<std::pair<uint16_t, uint8_t>, const ConfigFileRow*>{};
  const int secondFormat = second != nullptr ? second->format : first.format;
  std::set<std::pair<uint16_t, uint8_t>> addresses;
  std::ranges::copy(a | std::views::keys, std::inserter(addresses, addresses.end()));
  std::ranges::copy(b | std::views::keys, std::inserter(addresses, addresses.end()));
  std::vector<ConfigCompareRow> rows;
  rows.reserve(addresses.size());
  for (const auto& address : addresses) {
    const auto inA = a.find(address);
    const auto inB = b.find(address);
    const ConfigFileRow* rowA = inA != a.end() ? inA->second : nullptr;
    const ConfigFileRow* rowB = inB != b.end() ? inB->second : nullptr;
    ConfigCompareRow out;
    out.index = address.first;
    out.subindex = address.second;
    if (rowA != nullptr) {
      out.first = std::string(rowA->trimmedValue());
    }
    if (rowB != nullptr) {
      out.second = std::string(rowB->trimmedValue());
    }
    const auto object = objects.find(address);
    const DeviceParameter* definition =
        object != objects.end() ? &object->second.definition : nullptr;
    out.outcome = configRowOutcome(address.first, definition);
    if (rowA != nullptr && rowB != nullptr) {
      out.differsBetween =
          !sameFileValues(definition, rowA->value, first.format, rowB->value, secondFormat);
    }
    if (definition != nullptr) {
      out.name = definition->name;
      out.live = object->second.live;
      if (out.live) {
        const auto* row = rowA != nullptr ? rowA : rowB;
        const int format = rowA != nullptr ? first.format : secondFormat;
        out.differsFromLive = !sameConfigValue(*definition, row->value, format, *out.live);
      }
    }
    rows.push_back(std::move(out));
  }
  return rows;
}

ConfigFileView describeConfigFile(
    const ConfigFile& file,
    const std::function<std::optional<DeviceParameter>(uint16_t, uint8_t)>& lookup,
    const std::function<std::optional<DeviceParameterValue>(uint16_t, uint8_t)>& read) {
  ConfigFileView view{.format = file.format,
                      .deviceName = file.deviceName(),
                      .firmwareVersion = file.firmwareVersion(),
                      .rows = {},
                      .problems = file.problems};
  view.rows.reserve(file.rows.size());
  for (const auto& row : file.rows) {
    const auto object = lookup(row.index, row.subindex);
    ConfigFileViewRow out{.line = row.line,
                          .index = row.index,
                          .subindex = row.subindex,
                          .name = object ? object->name : std::string(),
                          .value = std::string(row.trimmedValue()),
                          .outcome = configRowOutcome(row.index, object ? &*object : nullptr),
                          .live = std::nullopt,
                          .differs = false};
    if (object && read) {
      out.live = read(row.index, row.subindex);
      if (out.live) {
        out.differs = !sameConfigValue(*object, row.value, file.format, *out.live);
      }
    }
    view.rows.push_back(std::move(out));
  }
  return view;
}

void to_json(nlohmann::json& j, const ConfigFileProblem& problem) {
  j = nlohmann::json{{"line", problem.line}, {"message", problem.message}};
}

void to_json(nlohmann::json& j, const ConfigFileViewRow& row) {
  j = nlohmann::json{
      {"line", row.line},
      {"index", row.index},
      {"subindex", row.subindex},
      {"name", row.name},
      {"value", row.value},
      {"outcome", toString(row.outcome)},
      {"live", row.live ? std::visit([](const auto& v) -> nlohmann::json { return v; }, *row.live)
                        : nlohmann::json()},
      {"differs", row.differs}};
}

void to_json(nlohmann::json& j, const ConfigFileView& view) {
  const auto optional = [](const std::optional<std::string>& text) {
    return text ? nlohmann::json(*text) : nlohmann::json();
  };
  j = nlohmann::json{{"format", view.format},
                     {"deviceName", optional(view.deviceName)},
                     {"firmwareVersion", optional(view.firmwareVersion)},
                     {"rows", view.rows},
                     {"problems", view.problems}};
}

bool isSafetyConfigIndex(uint16_t index) {
  return (index >= 0x2600 && index <= 0x26FF) || (index >= 0x6600 && index <= 0x67EF);
}

}  // namespace mm::node
