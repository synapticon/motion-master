#include "etg/esi_request.h"

#include <cctype>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "core/util.h"
#include "etg/esi.h"

namespace mm::etg {

namespace {

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

/// The per-device header of the summary: enough to choose a device without expanding any of them.
nlohmann::json describeDevice(const EsiDevice& device, std::size_t ordinal) {
  std::size_t objectCount = 0;
  std::optional<int32_t> profileNo;
  for (const EsiProfile& profile : device.profiles) {
    if (profile.profileNo && !profileNo) {
      profileNo = profile.profileNo;
    }
    if (profile.dictionary) {
      objectCount += profile.dictionary->objects.size();
    }
  }

  nlohmann::json j{
      {"ordinal", ordinal},
      {"type", device.type},
      {"name", esiText(device.names)},
      {"groupType", device.groupType},
      {"objectCount", objectCount},
      {"rxPdoCount", device.rxPdos.size()},
      {"txPdoCount", device.txPdos.size()},
      {"moduleIdents", esiSlotModuleIdents(device)},
  };
  if (device.productCode) {
    j["productCode"] = *device.productCode;
  }
  if (device.revisionNo) {
    j["revisionNo"] = *device.revisionNo;
  }
  if (profileNo) {
    j["profileNo"] = *profileNo;
  }
  if (!device.slots.slots.empty()) {
    j["slots"] = device.slots;
  }
  return j;
}

nlohmann::json describeModule(const EsiModule& module) {
  nlohmann::json j{
      {"moduleIdent", module.moduleIdent},
      {"type", module.type},
      {"name", esiText(module.names)},
  };
  if (module.profile && module.profile->dictionary) {
    j["objectCount"] = module.profile->dictionary->objects.size();
  }
  return j;
}

}  // namespace

std::expected<std::vector<uint32_t>, std::string> parseIdentList(std::string_view csv) {
  std::vector<uint32_t> out;
  std::string_view rest = trimmed(csv);
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    const std::string_view token = trimmed(rest.substr(0, comma));
    if (!token.empty()) {
      const auto value = mm::core::parseHexOrDec<uint32_t>(token);
      if (!value) {
        return std::unexpected(
            std::format("'{}' is not a valid hexadecimal or decimal value", token));
      }
      out.push_back(*value);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    rest = rest.substr(comma + 1);
  }
  return out;
}

std::expected<nlohmann::json, std::string> buildEsiResponse(std::string_view xml,
                                                            const EsiParseRequest& request,
                                                            const EsiEntryOptions& options) {
  auto file = parseEsi(xml);
  if (!file) {
    return std::unexpected(file.error());
  }

  EsiEntryOptions effective = options;
  effective.moduleIdents = request.moduleIdents;

  // Every device is assembled and returned. That is affordable only because object-level
  // annotation lives on subindex 0 rather than on every subindex — repeating it made a single
  // device's JSON 4.7 MB, of which 83% was duplicated description HTML.
  nlohmann::json devices = nlohmann::json::array();
  for (std::size_t i = 0; i < file->devices.size(); ++i) {
    nlohmann::json device = describeDevice(file->devices[i], i);
    auto table = buildDeviceEntries(*file, file->devices[i], effective);
    if (table) {
      device["entries"] = table->entries;
      if (!table->warnings.empty()) {
        // Per device, and kept apart from the document-level warnings: these describe one
        // assembly, and a different module selection would produce a different list.
        device["warnings"] = table->warnings;
      }
    } else {
      // A device with no dictionary at all is a structural fact about that device, not a reason
      // to fail the whole file — the others still have theirs.
      device["entries"] = nlohmann::json::array();
      device["warnings"] = nlohmann::json::array({table.error()});
    }
    devices.push_back(std::move(device));
  }

  nlohmann::json modules = nlohmann::json::array();
  for (const EsiModule& module : file->modules) {
    modules.push_back(describeModule(module));
  }

  nlohmann::json response{
      {"vendor", file->vendor},
      {"devices", std::move(devices)},
      {"modules", std::move(modules)},
  };
  if (file->version) {
    response["version"] = *file->version;
  }
  if (!file->warnings.empty()) {
    response["warnings"] = file->warnings;
  }

  return response;
}

}  // namespace mm::etg
