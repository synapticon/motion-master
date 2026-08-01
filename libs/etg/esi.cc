#include "etg/esi.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/util.h"

namespace mm::etg {

namespace {

/// Every recoverable problem lands here rather than aborting the parse. The path is an
/// XPath-ish breadcrumb (@c "Device[2]/Profile/Dictionary/Object[#x1018]") so a warning names a
/// spot in the file a human can actually find.
class Warnings {
 public:
  void add(std::string_view path, std::string_view what) {
    messages_.push_back(std::format("{}: {}", path, what));
  }

  std::vector<std::string> take() { return std::move(messages_); }

 private:
  std::vector<std::string> messages_;
};

/// pugixml preserves the whitespace an ESI's indentation puts around element text, and
/// mm::core::parseHexOrDec rejects a trailing space rather than skipping it. Every text read goes
/// through here.
std::string_view trimmed(std::string_view s) {
  const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

std::string_view nodeText(const pugi::xml_node& node) { return trimmed(node.text().as_string()); }

/// Text of @p node's @p name child, or an empty view when the child is absent. pugixml's
/// null-node pattern makes the absent case fall out without an explicit check.
std::string_view childText(const pugi::xml_node& node, const char* name) {
  return trimmed(node.child(name).text().as_string());
}

bool hasChild(const pugi::xml_node& node, const char* name) {
  return static_cast<bool>(node.child(name));
}

/// Parses a HexDecValue (`402` or `#x1A00`) into T. Returns nullopt for absent-or-malformed; the
/// caller decides whether that is a warning or a legitimate default.
template <typename T>
std::optional<T> parseHexDec(std::string_view s) {
  if (s.empty()) {
    return std::nullopt;
  }
  // HexDecValue permits a leading sign on the decimal branch. mm::core::parseHexOrDec parses into
  // an unsigned type via std::from_chars, which rejects '-', so signed values are handled here by
  // parsing the magnitude and negating.
  if (s.front() == '-') {
    const auto magnitude = mm::core::parseHexOrDec<uint64_t>(s.substr(1));
    if (!magnitude) {
      return std::nullopt;
    }
    const auto negated = -static_cast<int64_t>(*magnitude);
    if (std::cmp_less(negated, std::numeric_limits<T>::min())) {
      return std::nullopt;
    }
    return static_cast<T>(negated);
  }
  if (s.front() == '+') {
    s.remove_prefix(1);
  }
  const auto value = mm::core::parseHexOrDec<uint64_t>(s);
  if (!value || std::cmp_greater(*value, std::numeric_limits<T>::max())) {
    return std::nullopt;
  }
  return static_cast<T>(*value);
}

template <typename T>
std::optional<T> childHexDec(const pugi::xml_node& node, const char* name) {
  return parseHexDec<T>(childText(node, name));
}

template <typename T>
std::optional<T> attrHexDec(const pugi::xml_node& node, const char* name) {
  const pugi::xml_attribute a = node.attribute(name);
  return a ? parseHexDec<T>(trimmed(a.as_string())) : std::nullopt;
}

/// ESI booleans are written `0`/`1`, but the XSD type is xs:boolean, which also permits
/// `true`/`false`. pugixml's as_bool() handles both.
bool attrBool(const pugi::xml_node& node, const char* name, bool fallback = false) {
  const pugi::xml_attribute a = node.attribute(name);
  return a ? a.as_bool(fallback) : fallback;
}

/// Decodes xs:hexBinary. Byte order is the ESI's own: the FIRST byte is the LEAST significant, so
/// "92010200" yields {0x92, 0x01, 0x02, 0x00} == 0x00020192. An odd digit count or a non-hex
/// character means the value is unusable; the caller warns.
std::optional<std::vector<uint8_t>> parseHexBinary(std::string_view s) {
  if (s.empty()) {
    return std::vector<uint8_t>{};
  }
  if (s.size() % 2 != 0) {
    return std::nullopt;
  }
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  std::vector<uint8_t> out;
  out.reserve(s.size() / 2);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    const int hi = digit(s[i]);
    const int lo = digit(s[i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

/// Reads a hexBinary child into @p out, warning (and leaving @p out empty) on a malformed value.
void readHexBinary(const pugi::xml_node& node, const char* name, std::vector<uint8_t>& out,
                   std::string_view path, Warnings& warnings) {
  if (!hasChild(node, name)) {
    return;
  }
  auto bytes = parseHexBinary(childText(node, name));
  if (!bytes) {
    warnings.add(path,
                 std::format("<{}> is not valid hexBinary: '{}'", name, childText(node, name)));
    return;
  }
  out = std::move(*bytes);
}

/// Reads every `name` child as a localised Text.
std::vector<Text> readTexts(const pugi::xml_node& node, const char* name) {
  std::vector<Text> out;
  for (const pugi::xml_node& child : node.children(name)) {
    out.push_back(Text{
        .value = std::string(nodeText(child)),
        .lcId = child.attribute("LcId").as_uint(0),
    });
  }
  return out;
}

/// Reads <Properties><Property> children. Callers that need the bare, unwrapped form (only
/// DataType/SubItem uses it) call readBareProperties instead.
std::vector<Property> readBareProperties(const pugi::xml_node& node) {
  std::vector<Property> out;
  for (const pugi::xml_node& p : node.children("Property")) {
    out.push_back(Property{
        .name = std::string(childText(p, "Name")),
        .value = std::string(p.child("Value").text().as_string()),
    });
  }
  return out;
}

std::vector<Property> readProperties(const pugi::xml_node& node) {
  // <Properties></Properties> occurs empty in real files; pugixml returns a null node for an
  // absent wrapper and an empty range for an empty one, so both fall out with no special case.
  return readBareProperties(node.child("Properties"));
}

std::optional<AccessMode> parseAccessMode(std::string_view s) {
  if (s == "ro") {
    return AccessMode::Ro;
  }
  if (s == "rw") {
    return AccessMode::Rw;
  }
  if (s == "wo") {
    return AccessMode::Wo;
  }
  return std::nullopt;
}

std::string toLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

/// ETG.2000 requires a tool to accept the legacy "PreOp" spelling alongside "PreOP", so the whole
/// comparison is case-folded rather than special-casing that one token.
StateRestriction parseRestriction(std::string_view s) {
  const std::string v = toLower(s);
  if (v == "preop") {
    return StateRestriction::PreOp;
  }
  if (v == "preop_safeop") {
    return StateRestriction::PreOpSafeOp;
  }
  if (v == "preop_op") {
    return StateRestriction::PreOpOp;
  }
  if (v == "safeop") {
    return StateRestriction::SafeOp;
  }
  if (v == "safeop_op") {
    return StateRestriction::SafeOpOp;
  }
  if (v == "op") {
    return StateRestriction::Op;
  }
  return StateRestriction::None;
}

std::optional<Category> parseCategory(std::string_view s) {
  if (s == "m") {
    return Category::Mandatory;
  }
  if (s == "o") {
    return Category::Optional;
  }
  if (s == "c") {
    return Category::Conditional;
  }
  return std::nullopt;
}

/// "T R TR RT t r tr rt" — neither case nor order is significant.
std::optional<PdoMapping> parsePdoMapping(std::string_view s) {
  const std::string v = toLower(s);
  if (v.empty()) {
    return std::nullopt;
  }
  const bool tx = v.find('t') != std::string::npos;
  const bool rx = v.find('r') != std::string::npos;
  if (tx && rx) {
    return PdoMapping::TxRx;
  }
  if (tx) {
    return PdoMapping::Tx;
  }
  if (rx) {
    return PdoMapping::Rx;
  }
  return std::nullopt;
}

std::optional<SafetyMapping> parseSafetyMapping(std::string_view s) {
  const std::string v = toLower(s);
  if (v == "si") {
    return SafetyMapping::SafeIn;
  }
  if (v == "so") {
    return SafetyMapping::SafeOut;
  }
  if (v == "sio") {
    return SafetyMapping::SafeInOut;
  }
  if (v == "sp") {
    return SafetyMapping::SafeParam;
  }
  return std::nullopt;
}

/// Reads an <Access> element. An <Access/> with empty text counts as absent — a vendor toolchain
/// emits that for "no override", and treating it as a value would wrongly shadow an inherited flag.
std::optional<Access> readAccess(const pugi::xml_node& flags) {
  const pugi::xml_node node = flags.child("Access");
  if (!node) {
    return std::nullopt;
  }
  const auto mode = parseAccessMode(nodeText(node));
  if (!mode) {
    return std::nullopt;
  }
  return Access{
      .mode = *mode,
      .readRestrictions = parseRestriction(trimmed(node.attribute("ReadRestrictions").as_string())),
      .writeRestrictions =
          parseRestriction(trimmed(node.attribute("WriteRestrictions").as_string())),
  };
}

/// The flag members common to Object/Flags and SubItem/Flags. Templated over the two Flags types
/// rather than sharing one struct, because the schema genuinely differs (SubItem has no
/// SdoAccess, no Transition) and a shared type would advertise fields that cannot occur.
template <typename FlagsT>
void readCommonFlags(const pugi::xml_node& node, FlagsT& out) {
  out.access = readAccess(node);
  if (hasChild(node, "Category")) {
    out.category = parseCategory(childText(node, "Category"));
  }
  if (hasChild(node, "PdoMapping")) {
    out.pdoMapping = parsePdoMapping(childText(node, "PdoMapping"));
  }
  if (hasChild(node, "SafetyMapping")) {
    out.safetyMapping = parseSafetyMapping(childText(node, "SafetyMapping"));
  }
  if (hasChild(node, "Attribute")) {
    out.attribute = childHexDec<uint32_t>(node, "Attribute");
  }
  if (hasChild(node, "Backup")) {
    out.backup = childHexDec<int32_t>(node, "Backup");
  }
  if (hasChild(node, "Setting")) {
    out.setting = childHexDec<int32_t>(node, "Setting");
  }
}

std::optional<EsiObject::Flags> readObjectFlags(const pugi::xml_node& object) {
  const pugi::xml_node node = object.child("Flags");
  if (!node) {
    return std::nullopt;
  }
  EsiObject::Flags flags;
  readCommonFlags(node, flags);
  if (hasChild(node, "Transition")) {
    flags.transition = std::string(childText(node, "Transition"));
  }
  if (hasChild(node, "SdoAccess")) {
    const std::string_view v = childText(node, "SdoAccess");
    flags.sdoAccess = v == "CompleteAccess" ? SdoAccess::CompleteAccess : SdoAccess::SubIndexAccess;
  }
  return flags;
}

std::optional<EsiDataType::SubItem::Flags> readSubItemFlags(const pugi::xml_node& subItem) {
  const pugi::xml_node node = subItem.child("Flags");
  if (!node) {
    return std::nullopt;
  }
  EsiDataType::SubItem::Flags flags;
  readCommonFlags(node, flags);
  return flags;
}

/// ObjectInfoType is recursive by schema — an Info holds SubItems, each holding another Info — so
/// the reader recurses with it. A real file nests two deep (Object/Info/SubItem/Info) because that
/// is all an object dictionary can express, but the schema puts no limit on it and this parser
/// reads whatever an HTTP client uploads. Without a cap, a file nesting a few tens of thousands of
/// SubItems overflows the stack and takes the whole process down, RT loop included. Eight is far
/// past anything meaningful and nowhere near the stack.
constexpr int kMaxInfoDepth = 8;

EsiObject::Info readObjectInfo(const pugi::xml_node& node, std::string_view path,
                               Warnings& warnings, int depth = 0);

EsiObject::Info::SubItem readInfoSubItem(const pugi::xml_node& node, std::string_view path,
                                         Warnings& warnings, int depth) {
  return EsiObject::Info::SubItem{
      .name = std::string(childText(node, "Name")),
      .info = readObjectInfo(node.child("Info"), path, warnings, depth),
  };
}

EsiObject::Info readObjectInfo(const pugi::xml_node& node, std::string_view path,
                               Warnings& warnings, int depth) {
  EsiObject::Info info;
  if (!node) {
    return info;
  }
  if (depth > kMaxInfoDepth) {
    warnings.add(path, std::format("<Info> nests deeper than {} levels; the rest is ignored",
                                   kMaxInfoDepth));
    return info;
  }
  readHexBinary(node, "MinData", info.minData, path, warnings);
  readHexBinary(node, "MaxData", info.maxData, path, warnings);
  readHexBinary(node, "DefaultData", info.defaultData, path, warnings);
  if (hasChild(node, "MinValue")) {
    info.minValue = childHexDec<int64_t>(node, "MinValue");
  }
  if (hasChild(node, "MaxValue")) {
    info.maxValue = childHexDec<int64_t>(node, "MaxValue");
  }
  if (hasChild(node, "DefaultValue")) {
    info.defaultValue = childHexDec<int64_t>(node, "DefaultValue");
  }
  if (hasChild(node, "DefaultString")) {
    info.defaultString = std::string(node.child("DefaultString").text().as_string());
  }
  if (hasChild(node, "DisplayName")) {
    info.displayName = std::string(childText(node, "DisplayName"));
  }
  if (hasChild(node, "Unit")) {
    info.unit = childHexDec<uint32_t>(node, "Unit");
  }
  info.hasScaling = hasChild(node, "Scaling");
  for (const pugi::xml_node& sub : node.children("SubItem")) {
    info.subItems.push_back(readInfoSubItem(sub, path, warnings, depth + 1));
  }
  return info;
}

EsiDataType::SubItem readDataTypeSubItem(const pugi::xml_node& node, std::string_view path,
                                         Warnings& warnings) {
  EsiDataType::SubItem sub;
  if (hasChild(node, "SubIdx")) {
    sub.subIdx = childHexDec<uint8_t>(node, "SubIdx");
  }
  sub.name = std::string(childText(node, "Name"));
  sub.displayNames = readTexts(node, "DisplayName");
  sub.type = std::string(childText(node, "Type"));
  sub.comments = readTexts(node, "Comment");
  sub.bitSize = node.child("BitSize").text().as_int(0);
  sub.bitOffs = node.child("BitOffs").text().as_int(0);
  readHexBinary(node, "DefaultData", sub.defaultData, path, warnings);
  if (hasChild(node, "MinValue")) {
    sub.minValue = childHexDec<int64_t>(node, "MinValue");
  }
  if (hasChild(node, "MaxValue")) {
    sub.maxValue = childHexDec<int64_t>(node, "MaxValue");
  }
  if (hasChild(node, "DefaultValue")) {
    sub.defaultValue = childHexDec<int64_t>(node, "DefaultValue");
  }
  if (hasChild(node, "DefaultString")) {
    sub.defaultString = std::string(node.child("DefaultString").text().as_string());
  }
  sub.flags = readSubItemFlags(node);
  // DataType/SubItem carries bare <Property> children, with no <Properties> wrapper — an
  // asymmetry in the XSD relative to Object and DataType.
  sub.properties = readBareProperties(node);
  return sub;
}

std::optional<EsiDataType> readDataType(const pugi::xml_node& node, std::string_view path,
                                        Warnings& warnings) {
  EsiDataType type;
  type.name = std::string(childText(node, "Name"));
  if (type.name.empty()) {
    warnings.add(path, "<DataType> has no <Name>; skipped");
    return std::nullopt;
  }
  const std::string typePath = std::format("{}/DataType[{}]", path, type.name);
  if (hasChild(node, "Index")) {
    type.index = childHexDec<uint32_t>(node, "Index");
  }
  if (hasChild(node, "BaseType")) {
    type.baseType = std::string(childText(node, "BaseType"));
  }
  type.comments = readTexts(node, "Comment");
  type.bitSize = node.child("BitSize").text().as_int(0);
  for (const pugi::xml_node& a : node.children("ArrayInfo")) {
    type.arrayInfo.push_back(EsiDataType::ArrayInfo{
        .lBound = a.child("LBound").text().as_llong(0),
        .elements = a.child("Elements").text().as_llong(0),
    });
  }
  for (const pugi::xml_node& s : node.children("SubItem")) {
    type.subItems.push_back(readDataTypeSubItem(s, typePath, warnings));
  }
  for (const pugi::xml_node& e : node.children("EnumInfo")) {
    type.enumInfo.push_back(EsiDataType::EnumInfo{
        .texts = readTexts(e, "Text"),
        .value = childHexDec<int64_t>(e, "Enum").value_or(0),
    });
  }
  type.properties = readProperties(node);
  return type;
}

std::optional<EsiObject> readObject(const pugi::xml_node& node, std::string_view path,
                                    Warnings& warnings) {
  const pugi::xml_node indexNode = node.child("Index");
  const auto index = parseHexDec<uint32_t>(nodeText(indexNode));
  if (!index) {
    warnings.add(path, std::format("<Object> has a missing or malformed <Index> ('{}'); skipped",
                                   nodeText(indexNode)));
    return std::nullopt;
  }

  EsiObject object;
  object.index = *index;
  // These three live on <Index>, not on <Object> — a placement that is easy to get wrong and
  // silently lose the modular-device semantics they carry.
  object.dependOnSlot = attrBool(indexNode, "DependOnSlot");
  object.dependOnSlotGroup = attrBool(indexNode, "DependOnSlotGroup");
  object.overwrittenByModule = attrBool(indexNode, "OverwrittenByModule");

  const std::string objectPath = std::format("{}/Object[#x{:04X}]", path, object.index);
  object.names = readTexts(node, "Name");
  object.comments = readTexts(node, "Comment");
  object.type = std::string(childText(node, "Type"));
  if (object.type.empty()) {
    warnings.add(objectPath, "<Object> has no <Type>; skipped");
    return std::nullopt;
  }
  object.bitSize = node.child("BitSize").text().as_int(0);
  object.info = readObjectInfo(node.child("Info"), objectPath, warnings);
  object.flags = readObjectFlags(node);
  object.properties = readProperties(node);
  return object;
}

EsiDictionary readDictionary(const pugi::xml_node& node, std::string_view path,
                             Warnings& warnings) {
  EsiDictionary dict;
  for (const pugi::xml_node& u : node.child("UnitTypes").children("UnitType")) {
    const auto notation = childHexDec<uint8_t>(u, "NotationIndex");
    if (!notation) {
      warnings.add(path, "<UnitType> has a missing or malformed <NotationIndex>; skipped");
      continue;
    }
    dict.unitTypes.push_back(UnitType{
        .notationIndex = *notation,
        .index = childHexDec<uint32_t>(u, "Index").value_or(0),
        .name = std::string(childText(u, "Name")),
        .symbol = std::string(childText(u, "Symbol")),
    });
  }
  for (const pugi::xml_node& d : node.child("DataTypes").children("DataType")) {
    if (auto type = readDataType(d, path, warnings)) {
      dict.dataTypes.push_back(std::move(*type));
    }
  }
  for (const pugi::xml_node& o : node.child("Objects").children("Object")) {
    if (auto object = readObject(o, path, warnings)) {
      dict.objects.push_back(std::move(*object));
    }
  }
  return dict;
}

EsiProfile readProfile(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EsiProfile profile;
  if (hasChild(node, "ProfileNo")) {
    profile.profileNo = childHexDec<int32_t>(node, "ProfileNo");
  }
  if (hasChild(node, "AddInfo")) {
    profile.addInfo = childHexDec<int32_t>(node, "AddInfo");
  }
  if (hasChild(node, "SubAddInfo")) {
    profile.subAddInfo = childHexDec<int32_t>(node, "SubAddInfo");
  }
  if (hasChild(node, "DictionaryFile")) {
    profile.dictionaryFile = std::string(childText(node, "DictionaryFile"));
    warnings.add(path,
                 std::format("<DictionaryFile>{}</DictionaryFile> is not followed — parseEsi "
                             "reads one document and does no file I/O; its objects are absent",
                             *profile.dictionaryFile));
  }
  if (hasChild(node, "Dictionary")) {
    profile.dictionary = readDictionary(node.child("Dictionary"), path, warnings);
  }
  return profile;
}

EsiPdo readPdo(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EsiPdo pdo;
  pdo.index = parseHexDec<uint32_t>(nodeText(node.child("Index"))).value_or(0);
  pdo.names = readTexts(node, "Name");
  for (const pugi::xml_node& e : node.children("Exclude")) {
    if (const auto v = parseHexDec<uint32_t>(nodeText(e))) {
      pdo.excludes.push_back(*v);
    }
  }
  pdo.sm =
      node.attribute("Sm") ? std::optional<int32_t>(node.attribute("Sm").as_int()) : std::nullopt;
  pdo.su =
      node.attribute("Su") ? std::optional<int32_t>(node.attribute("Su").as_int()) : std::nullopt;
  pdo.fixed = attrBool(node, "Fixed");
  pdo.mandatory = attrBool(node, "Mandatory");
  pdo.isVirtual = attrBool(node, "Virtual");
  pdo.overwrittenByModule = attrBool(node.child("Index"), "OverwrittenByModule");

  for (const pugi::xml_node& e : node.children("Entry")) {
    EsiPdo::Entry entry;
    const auto index = parseHexDec<uint32_t>(nodeText(e.child("Index")));
    if (!index) {
      warnings.add(std::format("{}/Pdo[#x{:04X}]", path, pdo.index),
                   "<Entry> has a missing or malformed <Index>; skipped");
      continue;
    }
    entry.index = *index;
    // A padding entry (index 0) legitimately has neither SubIndex nor DataType — the schema makes
    // both optional for exactly this case — so their absence is never a warning.
    entry.subIndex = childHexDec<uint8_t>(e, "SubIndex").value_or(0);
    entry.bitLen = e.child("BitLen").text().as_int(0);
    entry.names = readTexts(e, "Name");
    if (hasChild(e, "DataType")) {
      entry.dataType = std::string(childText(e, "DataType"));
    }
    pdo.entries.push_back(std::move(entry));
  }
  return pdo;
}

std::optional<MailboxCoe> readMailboxCoe(const pugi::xml_node& parent) {
  const pugi::xml_node coe = parent.child("Mailbox").child("CoE");
  if (!coe) {
    return std::nullopt;
  }
  return MailboxCoe{
      .sdoInfo = attrBool(coe, "SdoInfo"),
      .pdoAssign = attrBool(coe, "PdoAssign"),
      .pdoConfig = attrBool(coe, "PdoConfig"),
      .pdoUpload = attrBool(coe, "PdoUpload"),
      .completeAccess = attrBool(coe, "CompleteAccess"),
  };
}

EsiSlots readSlots(const pugi::xml_node& node) {
  EsiSlots slots;
  if (!node) {
    return slots;
  }
  slots.slotPdoIncrement = attrHexDec<uint32_t>(node, "SlotPdoIncrement").value_or(0);
  slots.slotIndexIncrement = attrHexDec<uint32_t>(node, "SlotIndexIncrement").value_or(0);
  slots.maxSlotCount = attrHexDec<uint32_t>(node, "MaxSlotCount");
  slots.downloadModuleIdentList = attrBool(node, "DownloadModuleIdentList");

  for (const pugi::xml_node& s : node.children("Slot")) {
    EsiSlots::Slot slot;
    slot.names = readTexts(s, "Name");
    slot.minInstances = attrHexDec<uint32_t>(s, "MinInstances").value_or(0);
    slot.maxInstances = attrHexDec<uint32_t>(s, "MaxInstances").value_or(0);
    slot.slotGroup = attrHexDec<uint32_t>(s, "SlotGroup");
    slot.slotPdoIncrement = attrHexDec<uint32_t>(s, "SlotPdoIncrement");
    slot.slotIndexIncrement = attrHexDec<uint32_t>(s, "SlotIndexIncrement");
    for (const pugi::xml_node& m : s.children("ModuleIdent")) {
      const auto ident = parseHexDec<uint32_t>(nodeText(m));
      if (!ident) {
        continue;
      }
      slot.moduleIdents.push_back(*ident);
      if (attrBool(m, "Default") && !slot.defaultModuleIdent) {
        slot.defaultModuleIdent = *ident;
      }
    }
    slots.slots.push_back(std::move(slot));
  }

  for (const pugi::xml_node& g : node.children("ModulePdoGroup")) {
    slots.modulePdoGroups.push_back(EsiSlots::ModulePdoGroup{
        .alignment = g.attribute("Alignment")
                         ? std::optional<int32_t>(g.attribute("Alignment").as_int())
                         : std::nullopt,
        .rxPdo = attrHexDec<uint32_t>(g, "RxPdo"),
        .txPdo = attrHexDec<uint32_t>(g, "TxPdo"),
    });
  }
  return slots;
}

EsiDevice readDevice(const pugi::xml_node& node, std::size_t ordinal, Warnings& warnings) {
  EsiDevice device;
  const pugi::xml_node typeNode = node.child("Type");
  device.type = std::string(nodeText(typeNode));
  device.productCode = attrHexDec<uint32_t>(typeNode, "ProductCode");
  device.revisionNo = attrHexDec<uint32_t>(typeNode, "RevisionNo");
  device.serialNo = attrHexDec<uint32_t>(typeNode, "SerialNo");

  const std::string path =
      std::format("Device[{}]{}", ordinal, device.type.empty() ? "" : " " + device.type);

  device.names = readTexts(node, "Name");
  device.comments = readTexts(node, "Comment");
  device.groupType = std::string(childText(node, "GroupType"));
  device.physics = std::string(trimmed(node.attribute("Physics").as_string()));
  device.invisible = attrBool(node, "Invisible");
  device.coe = readMailboxCoe(node);

  for (const pugi::xml_node& p : node.children("Profile")) {
    device.profiles.push_back(readProfile(p, path, warnings));
  }
  for (const pugi::xml_node& p : node.children("RxPdo")) {
    device.rxPdos.push_back(readPdo(p, path, warnings));
  }
  for (const pugi::xml_node& p : node.children("TxPdo")) {
    device.txPdos.push_back(readPdo(p, path, warnings));
  }
  device.slots = readSlots(node.child("Slots"));
  return device;
}

std::optional<EsiModule> readModule(const pugi::xml_node& node, std::size_t ordinal,
                                    Warnings& warnings) {
  const pugi::xml_node typeNode = node.child("Type");
  const auto ident = attrHexDec<uint32_t>(typeNode, "ModuleIdent");
  if (!ident) {
    warnings.add(std::format("Module[{}]", ordinal),
                 "<Type> has a missing or malformed ModuleIdent; skipped");
    return std::nullopt;
  }

  EsiModule module;
  module.moduleIdent = *ident;
  module.type = std::string(nodeText(typeNode));
  if (typeNode.attribute("ModuleClass")) {
    module.moduleClass = std::string(trimmed(typeNode.attribute("ModuleClass").as_string()));
  }
  if (typeNode.attribute("ModulePdoGroup")) {
    module.modulePdoGroup = typeNode.attribute("ModulePdoGroup").as_int();
  }

  const std::string path = std::format("Module[#x{:08X}]", module.moduleIdent);
  module.names = readTexts(node, "Name");
  module.coe = readMailboxCoe(node);
  if (hasChild(node, "Profile")) {
    module.profile = readProfile(node.child("Profile"), path, warnings);
  }
  for (const pugi::xml_node& p : node.children("RxPdo")) {
    module.rxPdos.push_back(readPdo(p, path, warnings));
  }
  for (const pugi::xml_node& p : node.children("TxPdo")) {
    module.txPdos.push_back(readPdo(p, path, warnings));
  }
  for (const pugi::xml_node& p : node.children("SafetyParaMapping")) {
    module.safetyParaMappings.push_back(readPdo(p, path, warnings));
  }
  return module;
}

}  // namespace

std::string_view esiText(const std::vector<Text>& texts, uint32_t lcId) {
  if (texts.empty()) {
    return {};
  }
  const auto exact =
      std::find_if(texts.begin(), texts.end(), [lcId](const Text& t) { return t.lcId == lcId; });
  if (exact != texts.end()) {
    return exact->value;
  }
  // An element with no LcId is the unlocalised default and is preferred over another locale's.
  const auto unlocalised =
      std::find_if(texts.begin(), texts.end(), [](const Text& t) { return t.lcId == 0; });
  if (unlocalised != texts.end()) {
    return unlocalised->value;
  }
  return texts.front().value;
}

std::expected<EsiFile, std::string> parseEsi(std::string_view xml) {
  pugi::xml_document doc;
  // parse_default keeps the document tidy (no PI/comment/declaration nodes) and, crucially,
  // auto-detects the encoding — which is what makes a UTF-8 BOM a non-event.
  const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
  if (!result) {
    return std::unexpected(
        std::format("XML parse error at offset {}: {}", result.offset, result.description()));
  }

  const pugi::xml_node root = doc.child("EtherCATInfo");
  if (!root) {
    const pugi::xml_node actual = doc.first_child();
    return std::unexpected(
        std::format("not an ESI file: expected a root <EtherCATInfo> element, found <{}>",
                    actual ? actual.name() : "(empty document)"));
  }

  const pugi::xml_node descriptions = root.child("Descriptions");
  if (!descriptions || !descriptions.child("Devices")) {
    return std::unexpected("not an ESI file: <EtherCATInfo> has no <Descriptions><Devices>");
  }

  Warnings warnings;
  EsiFile file;
  if (root.attribute("Version")) {
    file.version = std::string(trimmed(root.attribute("Version").as_string()));
  }

  const pugi::xml_node vendor = root.child("Vendor");
  file.vendor = EsiFile::Vendor{
      .id = childHexDec<uint32_t>(vendor, "Id").value_or(0),
      .names = readTexts(vendor, "Name"),
      .urls = readTexts(vendor, "URL"),
  };

  std::size_t ordinal = 0;
  for (const pugi::xml_node& d : descriptions.child("Devices").children("Device")) {
    file.devices.push_back(readDevice(d, ordinal++, warnings));
  }
  ordinal = 0;
  for (const pugi::xml_node& m : descriptions.child("Modules").children("Module")) {
    if (auto module = readModule(m, ordinal++, warnings)) {
      file.modules.push_back(std::move(*module));
    }
  }

  file.warnings = warnings.take();
  return file;
}

std::expected<EsiFile, std::string> parseEsiFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::format("cannot open '{}'", path.string()));
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return std::unexpected(std::format("cannot read '{}'", path.string()));
  }
  const std::string xml = buffer.str();
  if (xml.empty()) {
    return std::unexpected(std::format("'{}' is empty", path.string()));
  }
  return parseEsi(xml);
}

const EsiDevice* findEsiDevice(const EsiFile& file, std::string_view type) {
  const auto it = std::find_if(file.devices.begin(), file.devices.end(),
                               [type](const EsiDevice& d) { return d.type == type; });
  return it != file.devices.end() ? &*it : nullptr;
}

const EsiDevice* findEsiDeviceByProductCode(const EsiFile& file, uint32_t productCode,
                                            std::optional<uint32_t> revisionNo) {
  const EsiDevice* best = nullptr;
  for (const EsiDevice& device : file.devices) {
    if (device.productCode != productCode) {
      continue;
    }
    if (revisionNo) {
      if (device.revisionNo == revisionNo) {
        return &device;
      }
      continue;
    }
    if (best == nullptr || device.revisionNo.value_or(0) > best->revisionNo.value_or(0)) {
      best = &device;
    }
  }
  return best;
}

const EsiModule* findEsiModule(const EsiFile& file, uint32_t moduleIdent) {
  const auto it =
      std::find_if(file.modules.begin(), file.modules.end(),
                   [moduleIdent](const EsiModule& m) { return m.moduleIdent == moduleIdent; });
  return it != file.modules.end() ? &*it : nullptr;
}

std::vector<uint32_t> esiSlotModuleIdents(const EsiDevice& device) {
  std::vector<uint32_t> idents;
  for (const EsiSlots::Slot& slot : device.slots.slots) {
    for (const uint32_t ident : slot.moduleIdents) {
      if (std::find(idents.begin(), idents.end(), ident) == idents.end()) {
        idents.push_back(ident);
      }
    }
  }
  return idents;
}

// -----------------------------------------------------------------------------------------------
// JSON serialisation. Absent optionals are omitted rather than emitted as null, matching the
// convention in libs/node/device_parameter.cc.
// -----------------------------------------------------------------------------------------------

namespace {

/// Emits j[key] = *v only when v holds a value.
template <typename T>
void emitOptional(nlohmann::json& j, const char* key, const std::optional<T>& v) {
  if (v) {
    j[key] = *v;
  }
}

/// Emits j[key] = v only when v is non-empty, keeping the common all-defaults object small.
template <typename T>
void emitNonEmpty(nlohmann::json& j, const char* key, const T& v) {
  if (!v.empty()) {
    j[key] = v;
  }
}

}  // namespace

void to_json(nlohmann::json& j, const Text& v) {
  j = nlohmann::json{{"value", v.value}};
  if (v.lcId != 0) {
    j["lcId"] = v.lcId;
  }
}

void to_json(nlohmann::json& j, const Property& v) {
  j = nlohmann::json{{"name", v.name}, {"value", v.value}};
}

void to_json(nlohmann::json& j, const Access& v) {
  j = nlohmann::json{{"mode", accessModeName(v.mode)}};
  if (v.readRestrictions != StateRestriction::None) {
    j["readRestrictions"] = stateRestrictionName(v.readRestrictions);
  }
  if (v.writeRestrictions != StateRestriction::None) {
    j["writeRestrictions"] = stateRestrictionName(v.writeRestrictions);
  }
}

void to_json(nlohmann::json& j, const MailboxCoe& v) {
  j = nlohmann::json{
      {"sdoInfo", v.sdoInfo},     {"pdoAssign", v.pdoAssign},           {"pdoConfig", v.pdoConfig},
      {"pdoUpload", v.pdoUpload}, {"completeAccess", v.completeAccess},
  };
}

void to_json(nlohmann::json& j, const EsiObject::Flags& v) {
  j = nlohmann::json::object();
  emitOptional(j, "access", v.access);
  if (v.category) {
    j["category"] = categoryName(*v.category);
  }
  if (v.pdoMapping) {
    j["pdoMapping"] = pdoMappingName(*v.pdoMapping);
  }
  if (v.safetyMapping) {
    j["safetyMapping"] = safetyMappingName(*v.safetyMapping);
  }
  emitOptional(j, "attribute", v.attribute);
  emitOptional(j, "transition", v.transition);
  if (v.sdoAccess) {
    j["sdoAccess"] = sdoAccessName(*v.sdoAccess);
  }
  emitOptional(j, "backup", v.backup);
  emitOptional(j, "setting", v.setting);
}

void to_json(nlohmann::json& j, const EsiDataType::SubItem::Flags& v) {
  j = nlohmann::json::object();
  emitOptional(j, "access", v.access);
  if (v.category) {
    j["category"] = categoryName(*v.category);
  }
  if (v.pdoMapping) {
    j["pdoMapping"] = pdoMappingName(*v.pdoMapping);
  }
  if (v.safetyMapping) {
    j["safetyMapping"] = safetyMappingName(*v.safetyMapping);
  }
  emitOptional(j, "attribute", v.attribute);
  emitOptional(j, "backup", v.backup);
  emitOptional(j, "setting", v.setting);
}

void to_json(nlohmann::json& j, const EsiObject::Info::SubItem& v) {
  j = nlohmann::json{{"name", v.name}, {"info", v.info}};
}

void to_json(nlohmann::json& j, const EsiObject::Info& v) {
  j = nlohmann::json::object();
  emitNonEmpty(j, "minData", v.minData);
  emitNonEmpty(j, "maxData", v.maxData);
  emitNonEmpty(j, "defaultData", v.defaultData);
  emitOptional(j, "minValue", v.minValue);
  emitOptional(j, "maxValue", v.maxValue);
  emitOptional(j, "defaultValue", v.defaultValue);
  emitOptional(j, "defaultString", v.defaultString);
  emitOptional(j, "displayName", v.displayName);
  emitOptional(j, "unit", v.unit);
  if (v.hasScaling) {
    j["hasScaling"] = true;
  }
  emitNonEmpty(j, "subItems", v.subItems);
}

void to_json(nlohmann::json& j, const EsiObject& v) {
  j = nlohmann::json{
      {"index", v.index},
      {"name", esiText(v.names)},
      {"type", v.type},
      {"bitSize", v.bitSize},
  };
  if (v.dependOnSlot) {
    j["dependOnSlot"] = true;
  }
  if (v.dependOnSlotGroup) {
    j["dependOnSlotGroup"] = true;
  }
  if (v.overwrittenByModule) {
    j["overwrittenByModule"] = true;
  }
  const nlohmann::json info = v.info;
  if (!info.empty()) {
    j["info"] = info;
  }
  emitOptional(j, "flags", v.flags);
  emitNonEmpty(j, "properties", v.properties);
}

void to_json(nlohmann::json& j, const EsiDataType::SubItem& v) {
  j = nlohmann::json{
      {"name", v.name},
      {"type", v.type},
      {"bitSize", v.bitSize},
      {"bitOffs", v.bitOffs},
  };
  emitOptional(j, "subIdx", v.subIdx);
  emitNonEmpty(j, "displayNames", v.displayNames);
  emitNonEmpty(j, "defaultData", v.defaultData);
  emitOptional(j, "minValue", v.minValue);
  emitOptional(j, "maxValue", v.maxValue);
  emitOptional(j, "defaultValue", v.defaultValue);
  emitOptional(j, "defaultString", v.defaultString);
  emitOptional(j, "flags", v.flags);
  emitNonEmpty(j, "properties", v.properties);
}

void to_json(nlohmann::json& j, const EsiDataType::ArrayInfo& v) {
  j = nlohmann::json{{"lBound", v.lBound}, {"elements", v.elements}};
}

void to_json(nlohmann::json& j, const EsiDataType::EnumInfo& v) {
  j = nlohmann::json{{"text", esiText(v.texts)}, {"value", v.value}};
}

void to_json(nlohmann::json& j, const EsiDataType& v) {
  j = nlohmann::json{{"name", v.name}, {"bitSize", v.bitSize}};
  emitOptional(j, "index", v.index);
  emitOptional(j, "baseType", v.baseType);
  emitNonEmpty(j, "arrayInfo", v.arrayInfo);
  emitNonEmpty(j, "subItems", v.subItems);
  emitNonEmpty(j, "enumInfo", v.enumInfo);
  emitNonEmpty(j, "properties", v.properties);
}

void to_json(nlohmann::json& j, const EsiDictionary& v) {
  j = nlohmann::json{{"dataTypes", v.dataTypes}, {"objects", v.objects}};
  emitNonEmpty(j, "unitTypes", v.unitTypes);
}

void to_json(nlohmann::json& j, const EsiProfile& v) {
  j = nlohmann::json::object();
  emitOptional(j, "profileNo", v.profileNo);
  emitOptional(j, "addInfo", v.addInfo);
  emitOptional(j, "subAddInfo", v.subAddInfo);
  emitOptional(j, "dictionaryFile", v.dictionaryFile);
  emitOptional(j, "dictionary", v.dictionary);
}

void to_json(nlohmann::json& j, const EsiPdo::Entry& v) {
  j = nlohmann::json{
      {"index", v.index},
      {"subIndex", v.subIndex},
      {"bitLen", v.bitLen},
      {"name", esiText(v.names)},
  };
  emitOptional(j, "dataType", v.dataType);
  if (v.isPadding()) {
    j["padding"] = true;
  }
}

void to_json(nlohmann::json& j, const EsiPdo& v) {
  j = nlohmann::json{
      {"index", v.index}, {"name", esiText(v.names)}, {"entries", v.entries},
      {"fixed", v.fixed}, {"mandatory", v.mandatory},
  };
  emitOptional(j, "sm", v.sm);
  emitOptional(j, "su", v.su);
  emitNonEmpty(j, "excludes", v.excludes);
  if (v.isVirtual) {
    j["virtual"] = true;
  }
  if (v.overwrittenByModule) {
    j["overwrittenByModule"] = true;
  }
}

void to_json(nlohmann::json& j, const EsiSlots::Slot& v) {
  j = nlohmann::json{
      {"moduleIdents", v.moduleIdents},
      {"minInstances", v.minInstances},
      {"maxInstances", v.maxInstances},
  };
  emitNonEmpty(j, "name", std::string(esiText(v.names)));
  emitOptional(j, "defaultModuleIdent", v.defaultModuleIdent);
  emitOptional(j, "slotGroup", v.slotGroup);
  emitOptional(j, "slotPdoIncrement", v.slotPdoIncrement);
  emitOptional(j, "slotIndexIncrement", v.slotIndexIncrement);
}

void to_json(nlohmann::json& j, const EsiSlots::ModulePdoGroup& v) {
  j = nlohmann::json::object();
  emitOptional(j, "alignment", v.alignment);
  emitOptional(j, "rxPdo", v.rxPdo);
  emitOptional(j, "txPdo", v.txPdo);
}

void to_json(nlohmann::json& j, const EsiSlots& v) {
  j = nlohmann::json{
      {"slots", v.slots},
      {"slotPdoIncrement", v.slotPdoIncrement},
      {"slotIndexIncrement", v.slotIndexIncrement},
  };
  emitNonEmpty(j, "modulePdoGroups", v.modulePdoGroups);
  emitOptional(j, "maxSlotCount", v.maxSlotCount);
  if (v.downloadModuleIdentList) {
    j["downloadModuleIdentList"] = true;
  }
}

void to_json(nlohmann::json& j, const EsiDevice& v) {
  j = nlohmann::json{
      {"type", v.type},
      {"name", esiText(v.names)},
      {"groupType", v.groupType},
  };
  emitOptional(j, "productCode", v.productCode);
  emitOptional(j, "revisionNo", v.revisionNo);
  emitOptional(j, "serialNo", v.serialNo);
  emitNonEmpty(j, "physics", v.physics);
  if (v.invisible) {
    j["invisible"] = true;
  }
  emitOptional(j, "coe", v.coe);
  emitNonEmpty(j, "profiles", v.profiles);
  emitNonEmpty(j, "rxPdos", v.rxPdos);
  emitNonEmpty(j, "txPdos", v.txPdos);
  if (!v.slots.slots.empty()) {
    j["slots"] = v.slots;
  }
}

void to_json(nlohmann::json& j, const EsiModule& v) {
  j = nlohmann::json{
      {"moduleIdent", v.moduleIdent},
      {"type", v.type},
      {"name", esiText(v.names)},
  };
  emitOptional(j, "moduleClass", v.moduleClass);
  emitOptional(j, "modulePdoGroup", v.modulePdoGroup);
  emitOptional(j, "coe", v.coe);
  emitOptional(j, "profile", v.profile);
  emitNonEmpty(j, "rxPdos", v.rxPdos);
  emitNonEmpty(j, "txPdos", v.txPdos);
  emitNonEmpty(j, "safetyParaMappings", v.safetyParaMappings);
}

void to_json(nlohmann::json& j, const EsiFile::Vendor& v) {
  j = nlohmann::json{{"id", v.id}, {"name", esiText(v.names)}};
  emitNonEmpty(j, "url", std::string(esiText(v.urls)));
}

void to_json(nlohmann::json& j, const EsiFile& v) {
  j = nlohmann::json{
      {"vendor", v.vendor},
      {"devices", v.devices},
      {"modules", v.modules},
  };
  emitOptional(j, "version", v.version);
  emitNonEmpty(j, "warnings", v.warnings);
}

}  // namespace mm::etg
