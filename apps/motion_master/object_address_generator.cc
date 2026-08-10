#include "object_address_generator.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "etg/esi.h"
#include "etg/esi_data_type.h"
#include "etg/esi_entry.h"

namespace mm {

namespace {

using mm::etg::EsiEntry;

/// One output file: where its addresses go, and which entries belong in it.
struct Bucket {
  const char* fileName = nullptr;
  const char* nameSpace = nullptr;
  const char* title = nullptr;
  std::vector<const EsiEntry*> entries;
};

/// Which header an object belongs in, by index.
///
/// 0x1xxx is the CiA 301 communication area and 0x6xxx the CiA 402 drive profile, so both are
/// generic. Everything else here is either vendor-specific (0x2xxx) or a standard profile that only
/// shows up on these devices: the MDP objects (0xF000/0xF030/0xF050, ETG.5001 — the ones
/// reconcileDetectedModules already writes) are generic and go with the communication area, while
/// FSoE (0xE901, 0xF980) ships on the safety variant and goes with the vendor's.
std::size_t bucketFor(uint16_t index) {
  if (index >= 0x1000 && index <= 0x1FFF) {
    return 0;  // profile
  }
  if (index >= 0x6000 && index <= 0x6FFF) {
    return 1;  // cia402
  }
  if (index == 0xF000 || index == 0xF030 || index == 0xF050) {
    return 0;  // MDP — standard, so it belongs with the communication area
  }
  return 2;  // somanet: 0x2xxx-0x5xxx, FSoE, and anything else a vendor invents
}

/// Escapes a doc comment: entry names are vendor text and may contain anything.
std::string sanitiseComment(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '\n' || c == '\r' || c == '\t') {
      out += ' ';
    } else if (c == '*' && !out.empty() && out.back() == '/') {
      out += ' ';  // would open a nested comment
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

std::string objectIdentifier(std::string_view objectName, std::string_view entryName,
                             uint8_t subindex, bool composite) {
  const auto pascal = [](std::string_view s) {
    std::string out;
    bool upper = true;
    for (char c : s) {
      if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
      } else {
        upper = true;
      }
    }
    return out;
  };

  std::string name = pascal(objectName);
  if (composite && subindex == 0) {
    // Subindex 0 of a composite is the entry count, not a value. Naming it after the object would
    // collide with whichever subindex repeats the object's own name.
    name += "Count";
  } else if (subindex != 0 && entryName != objectName) {
    name += pascal(entryName);
  }
  if (name.empty()) {
    name = "Object";
  }
  if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    name.insert(name.begin(), 'N');
  }
  return "k" + name;
}

std::expected<ObjectAddressGeneratorSummary, std::string> generateObjectAddresses(
    const std::string& esiPath, const std::string& outDir) {
  auto file = mm::etg::parseEsiFile(esiPath);
  if (!file) {
    return std::unexpected(std::format("parsing {}: {}", esiPath, file.error()));
  }

  ObjectAddressGeneratorSummary summary;
  summary.warnings = file->warnings;

  // Merge every device's dictionary into one table keyed by address. The devices in a family share
  // most of their objects, so the union is mostly agreement; where two disagree the first wins and
  // the difference is reported rather than silently resolved.
  std::map<uint32_t, EsiEntry> merged;
  for (const auto& device : file->devices) {
    auto table = mm::etg::buildDeviceEntries(*file, device, {});
    if (!table) {
      return std::unexpected(std::format("building '{}': {}", device.type, table.error()));
    }
    for (auto& warning : table->warnings) {
      summary.warnings.push_back(std::format("[{}] {}", device.type, warning));
    }
    for (auto& entry : table->entries) {
      auto [it, inserted] = merged.try_emplace(entry.key(), entry);
      if (!inserted && it->second.valueKind != entry.valueKind) {
        summary.warnings.push_back(
            std::format("0x{:04X}:{:02X} is {} on '{}' but {} elsewhere; keeping the first",
                        entry.index, entry.subindex, mm::etg::cxxTypeName(entry.valueKind),
                        device.type, mm::etg::cxxTypeName(it->second.valueKind)));
      }
    }
  }

  std::vector<Bucket> buckets{
      {"profile_device_objects.h",
       "mm::node::profile::objects",
       "CiA 301 communication area (0x1xxx) and the standard MDP objects",
       {}},
      {"cia402_drive_objects.h", "mm::node::cia402::objects", "CiA 402 drive profile (0x6xxx)", {}},
      {"somanet_drive_objects.h",
       "mm::node::somanet::objects",
       "SOMANET vendor area (0x2xxx) and FSoE",
       {}},
  };
  for (const auto& [key, entry] : merged) {
    buckets[bucketFor(entry.index)].entries.push_back(&entry);
  }

  const std::string source = std::filesystem::path(esiPath).filename().string();
  for (auto& bucket : buckets) {
    // Identifiers are unique per file, not globally: the three namespaces are distinct, and a name
    // that appears in two of them is not a clash.
    std::map<std::string, int> used;
    std::string body;
    for (const EsiEntry* entry : bucket.entries) {
      std::string id = objectIdentifier(entry->objectName, entry->entryName, entry->subindex,
                                        entry->objectCode != mm::etg::ObjectCode::Var);
      if (const int seen = used[id]++; seen > 0) {
        // Should not happen with the naming rule above, but a duplicate would be a compile error in
        // the generated file, so disambiguate deterministically and say so.
        summary.warnings.push_back(std::format("duplicate identifier {} at 0x{:04X}:{:02X}", id,
                                               entry->index, entry->subindex));
        id += std::format("At{:04X}{:02X}", entry->index, entry->subindex);
      }

      std::string note = sanitiseComment(entry->objectName);
      if (entry->entryName != entry->objectName) {
        note += " / " + sanitiseComment(entry->entryName);
      }
      note += std::format(" — {}", mm::etg::accessModeName(entry->access.mode));
      if (!entry->unitSymbol.empty()) {
        note += ", " + sanitiseComment(entry->unitSymbol);
      }

      body += std::format(
          "/// {}\ninline constexpr ObjectAddress<{}> {}{{0x{:04X}, 0x{:02X}}};\n\n", note,
          mm::etg::cxxTypeName(entry->valueKind), id, entry->index, entry->subindex);
      ++summary.rows;
    }

    const std::filesystem::path path = std::filesystem::path(outDir) / bucket.fileName;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return std::unexpected(std::format("cannot write {}", path.string()));
    }
    out << std::format(
        "// Generated by `motion-master generate-object-addresses` from {} — do not edit.\n"
        "// Regenerating is a two-step job: run that command, then `tools/format.sh` — "
        "clang-format\n"
        "// wraps the declarations that overrun 100 columns, and it does so context-sensitively\n"
        "// enough that reproducing its choices here would be guesswork.\n"
        "//\n"
        "// {} — {} addresses.\n"
        "//\n"
        "// Each constant carries an object's index, subindex and the C++ type its declared\n"
        "// ETG.1020 type maps to, so a call site cannot disagree with the object it names:\n"
        "//\n"
        "//     device.value(objects::kStatusword)        // std::optional<uint16_t>\n"
        "//     device.readValue(objects::kDeviceName)    // std::expected<std::string, ...>\n"
        "//\n"
        "// The union of every device in the ESI is emitted: which module is fitted is unknowable\n"
        "// offline, so an address that does not apply to the drive in front of you simply fails\n"
        "// to resolve at runtime, exactly as a hand-written index would. The trailing comment on\n"
        "// each line is the ESI's own name, access mode and unit — the unit especially, since a\n"
        "// temperature in milli-degrees looks exactly like one in degrees until a motor stops.\n"
        "\n#pragma once\n\n#include <cstdint>\n#include <string>\n#include <vector>\n\n"
        "#include \"node/device_parameter.h\"\n\nnamespace {} {{\n\n{}}}  // namespace {}\n",
        source, bucket.title, bucket.entries.size(), bucket.nameSpace, body, bucket.nameSpace);
    summary.files.push_back(path.string());
  }
  return summary;
}

}  // namespace mm
