#include "node/firmware_package.h"

#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

constexpr std::string_view kPackageDescription = "package";
constexpr size_t kPackageNameFieldCount = 5;

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  return std::ranges::equal(text.substr(text.size() - suffix.size()), suffix, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

/// Removes a Windows duplicate-download suffix — the " (1)" a browser appends before the extension
/// when the same package is downloaded twice.
std::string withoutDuplicateSuffix(std::string_view stem) {
  if (stem.empty() || stem.back() != ')') {
    return std::string(stem);
  }
  const size_t open = stem.rfind(" (");
  if (open == std::string_view::npos) {
    return std::string(stem);
  }
  const std::string_view digits = stem.substr(open + 2, stem.size() - open - 3);
  if (digits.empty() || !std::ranges::all_of(digits, [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    return std::string(stem);
  }
  return std::string(stem.substr(0, open));
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (true) {
    const size_t at = text.find(separator, start);
    if (at == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, at - start));
    start = at + 1;
  }
}

bool isDigits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  });
}

/// Decodes the numeric `<firmwareId>-<firmwareVersion>[-<keyId>[-<fieldbusProtocol>]]` form of a
/// full firmware descriptor, leaving every field empty for a descriptor that does not follow it
/// (which the specification permits). All-or-nothing on the mandatory pair: a descriptor whose
/// first two fields are not both numbers is not the numeric form at all, and decoding one anyway
/// would dress `MyProduct-v25-key3-ecat` up as a firmware id and a hardware revision.
///
/// The two optional tails are kept as written even when they are not numbers — a real package
/// carries the key id "A" — because the point of decoding is to show a user which part is which,
/// and the whole descriptor is what a compatibility check compares.
void decodeFullFirmwareDescriptor(std::string_view descriptor, FirmwarePackageName& name) {
  const std::vector<std::string_view> parts = split(descriptor, '-');
  if (parts.size() < 2 || !isDigits(parts[0]) || !isDigits(parts[1])) {
    return;
  }
  name.firmwareId = std::string(parts[0]);
  name.firmwareVersion = std::string(parts[1]);
  if (parts.size() >= 3) {
    name.keyId = std::string(parts[2]);
  }
  if (parts.size() >= 4) {
    name.fieldbusProtocol = std::string(parts[3]);
  }
}

/// libzip handles are C resources freed by their own functions; these give them scope-bound
/// lifetimes so no path through the extraction leaks one.
struct ArchiveDeleter {
  void operator()(zip_t* archive) const { zip_close(archive); }
};
struct FileDeleter {
  void operator()(zip_file_t* file) const { zip_fclose(file); }
};

}  // namespace

std::expected<FirmwarePackageName, std::string> parseFirmwarePackageName(
    std::string_view filename) {
  constexpr std::string_view kExtension = ".zip";
  if (!endsWithIgnoringCase(filename, kExtension)) {
    return std::unexpected(
        std::format("'{}' is not a firmware package name: it does not end in .zip", filename));
  }
  const std::string stem =
      withoutDuplicateSuffix(filename.substr(0, filename.size() - kExtension.size()));

  const std::vector<std::string_view> fields = split(stem, '_');
  if (fields.size() != kPackageNameFieldCount) {
    return std::unexpected(std::format(
        "'{}' is not a firmware package name: it has {} underscore-separated fields, not {} "
        "(package_<hardware>_<fullFirmwareDescriptor>_<software>_v<version>.zip)",
        filename, fields.size(), kPackageNameFieldCount));
  }
  const bool anyFieldEmpty =
      std::ranges::any_of(fields, [](std::string_view f) { return f.empty(); });
  if (anyFieldEmpty) {
    return std::unexpected(
        std::format("'{}' is not a firmware package name: one of its fields is empty", filename));
  }
  if (fields[0] != kPackageDescription) {
    return std::unexpected(
        std::format("'{}' is not a firmware package name: it starts with '{}' rather than '{}'",
                    filename, fields[0], kPackageDescription));
  }
  if (!fields[4].starts_with('v')) {
    return std::unexpected(
        std::format("'{}' is not a firmware package name: the version field '{}' does not start "
                    "with 'v'",
                    filename, fields[4]));
  }

  FirmwarePackageName name;
  name.description = std::string(fields[0]);
  name.hardwareName = std::string(fields[1]);
  name.fullFirmwareDescriptor = std::string(fields[2]);
  name.softwareName = std::string(fields[3]);
  name.softwareVersion = std::string(fields[4]);
  decodeFullFirmwareDescriptor(name.fullFirmwareDescriptor, name);
  return name;
}

std::optional<std::string> FirmwarePackageName::buildDescriptor() const {
  if (!firmwareId || !firmwareVersion) {
    return std::nullopt;
  }
  return std::format("{}-{}", *firmwareId, *firmwareVersion);
}

std::expected<FirmwarePackage, std::string> openFirmwarePackage(
    std::span<const uint8_t> zip, std::span<const std::string> skipFiles) {
  zip_error_t error;
  zip_error_init(&error);
  // The source borrows the buffer (freep = 0): `zip` must outlive this call, which it does — the
  // archive is closed before returning.
  zip_source_t* source = zip_source_buffer_create(zip.data(), zip.size(), 0, &error);
  if (source == nullptr) {
    std::string reason = zip_error_strerror(&error);
    zip_error_fini(&error);
    return std::unexpected(std::format("could not read the firmware package: {}", reason));
  }
  std::unique_ptr<zip_t, ArchiveDeleter> archive(zip_open_from_source(source, ZIP_RDONLY, &error));
  if (!archive) {
    // Ownership of the source only transfers on success, so a failed open must free it here.
    zip_source_free(source);
    std::string reason = zip_error_strerror(&error);
    zip_error_fini(&error);
    return std::unexpected(std::format("could not read the firmware package: {}", reason));
  }
  zip_error_fini(&error);

  FirmwarePackage package;
  bool sawAppBinary = false;

  const zip_int64_t entries = zip_get_num_entries(archive.get(), 0);
  for (zip_int64_t i = 0; i < entries; ++i) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat_index(archive.get(), i, 0, &stat) != 0) {
      return std::unexpected(std::format("could not read entry {} of the firmware package: {}", i,
                                         zip_strerror(archive.get())));
    }
    const std::string name = stat.name != nullptr ? stat.name : "";
    if (name.empty() || name.back() == '/') {
      continue;  // A directory entry carries no content.
    }

    const bool isAppBinary = name.starts_with("app_") && endsWithIgnoringCase(name, ".bin");
    sawAppBinary = sawAppBinary || isAppBinary;

    if (std::ranges::find(skipFiles, name) != skipFiles.end()) {
      package.skipped.push_back(name);
      continue;  // Deliberately before the read: a skipped entry is never decompressed.
    }

    std::unique_ptr<zip_file_t, FileDeleter> file(zip_fopen_index(archive.get(), i, 0));
    if (!file) {
      return std::unexpected(std::format("could not open '{}' in the firmware package: {}", name,
                                         zip_strerror(archive.get())));
    }
    std::vector<uint8_t> content(stat.size);
    if (stat.size > 0) {
      const zip_int64_t read = zip_fread(file.get(), content.data(), stat.size);
      if (read < 0 || static_cast<zip_uint64_t>(read) != stat.size) {
        return std::unexpected(std::format(
            "'{}' in the firmware package is truncated: {} of {} bytes could be decompressed", name,
            read < 0 ? 0 : read, stat.size));
      }
    }

    FirmwarePackageFile entry{name, std::move(content)};
    if (isAppBinary) {
      package.appBinary = std::move(entry);
    } else if (name.starts_with("com_") && endsWithIgnoringCase(name, ".bin")) {
      package.comBinary = std::move(entry);
    } else if (endsWithIgnoringCase(name, ".sii")) {
      package.sii = std::move(entry);
    } else {
      package.extras.push_back(std::move(entry));
    }
  }

  if (!sawAppBinary) {
    return std::unexpected(
        "the firmware package holds no app_*.bin application firmware — it is not a SOMANET "
        "firmware package");
  }
  return package;
}

void to_json(nlohmann::json& j, const FirmwarePackageName& name) {
  j = nlohmann::json{
      {"description", name.description},
      {"hardwareName", name.hardwareName},
      {"fullFirmwareDescriptor", name.fullFirmwareDescriptor},
      {"softwareName", name.softwareName},
      {"softwareVersion", name.softwareVersion},
  };
  if (name.firmwareId) {
    j["firmwareId"] = *name.firmwareId;
  }
  if (name.firmwareVersion) {
    j["firmwareVersion"] = *name.firmwareVersion;
  }
  if (name.keyId) {
    j["keyId"] = *name.keyId;
  }
  if (name.fieldbusProtocol) {
    j["fieldbusProtocol"] = *name.fieldbusProtocol;
  }
  if (const auto build = name.buildDescriptor()) {
    j["buildDescriptor"] = *build;
  }
}

// ── Compatibility ──────────────────────────────────────────────────────────────────────────────

FullFirmwareDescriptors fullFirmwareDescriptors(const HardwareDescription& description,
                                                const IntegroVariant* variant) {
  // Both tails come from the device however the head was chosen: the key id because assemblies have
  // none of their own (§3.4.1), the fieldbus because it describes the device's communication
  // firmware and an assembly has no say in it.
  std::string tail;
  if (!description.device.keyId.empty()) {
    tail += std::format("-{}", description.device.keyId);
  }
  if (variant != nullptr) {
    if (const auto fieldbus = variantFieldbusProtocol(*variant)) {
      // Hexadecimal, per §3.4.2.1's "1 means EtherCAT" and the same base parseFirmwarePackageName
      // reads it in. Every fieldbus option is a single digit, where hex and decimal agree.
      tail += std::format("-{:X}", *fieldbus);
    }
  }

  FullFirmwareDescriptors descriptors;
  descriptors.device = description.device.buildDescriptor() + tail;
  if (description.assembly) {
    descriptors.assembly = description.assembly->buildDescriptor() + tail;
  }
  return descriptors;
}

std::string_view toString(FirmwareMatch match) {
  switch (match) {
    case FirmwareMatch::kNone:
      return "none";
    case FirmwareMatch::kAssembly:
      return "assembly";
    case FirmwareMatch::kDevice:
      return "device";
  }
  return "none";
}

FirmwareCompatibility checkFirmwareCompatibility(const HardwareDescription& description,
                                                 const FirmwarePackageName& packageName,
                                                 const IntegroVariant* variant) {
  FirmwareCompatibility compatibility;
  compatibility.packageName = packageName;
  compatibility.deviceDescriptors = fullFirmwareDescriptors(description, variant);

  const std::string& descriptor = compatibility.packageName.fullFirmwareDescriptor;
  const FullFirmwareDescriptors& accepted = compatibility.deviceDescriptors;

  // Assembly first because that is the one to prefer when both match — which they do not, since the
  // two descriptors differ in their build descriptor whenever an assembly exists.
  if (accepted.assembly && descriptor == *accepted.assembly) {
    compatibility.match = FirmwareMatch::kAssembly;
    compatibility.explanation =
        std::format("This package is built for {}, which is this device's assembly.", descriptor);
    return compatibility;
  }
  if (descriptor == accepted.device) {
    compatibility.match = FirmwareMatch::kDevice;
    compatibility.explanation =
        accepted.assembly
            ? std::format(
                  "This package is built for {}, which is this device. It is the generic build for "
                  "the hardware rather than one customised for its assembly {}.",
                  descriptor, *accepted.assembly)
            : std::format("This package is built for {}, which is this device.", descriptor);
    return compatibility;
  }

  compatibility.explanation =
      accepted.assembly
          ? std::format(
                "This package is built for {}. This device accepts {} (the device) or {} "
                "(its assembly).",
                descriptor, accepted.device, *accepted.assembly)
          : std::format("This package is built for {}. This device accepts {}.", descriptor,
                        accepted.device);
  return compatibility;
}

std::expected<FirmwareCompatibility, std::string> checkFirmwareCompatibility(
    std::string_view hardwareDescriptionContent, std::string_view packageFilename,
    const IntegroVariant* variant) {
  auto name = parseFirmwarePackageName(packageFilename);
  if (!name) {
    return std::unexpected(name.error());
  }
  auto description = parseHardwareDescription(hardwareDescriptionContent);
  if (!description) {
    return std::unexpected(description.error());
  }
  return checkFirmwareCompatibility(*description, *name, variant);
}

void to_json(nlohmann::json& j, const FullFirmwareDescriptors& descriptors) {
  j = nlohmann::json{{"device", descriptors.device}};
  if (descriptors.assembly) {
    j["assembly"] = *descriptors.assembly;
  }
}

void to_json(nlohmann::json& j, const FirmwareCompatibility& compatibility) {
  j = nlohmann::json{
      {"compatible", compatibility.compatible()},
      {"match", toString(compatibility.match)},
      {"packageName", compatibility.packageName},
      {"deviceDescriptors", compatibility.deviceDescriptors},
      {"explanation", compatibility.explanation},
  };
}

}  // namespace mm::node
