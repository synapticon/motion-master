#pragma once

#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "etg/esi_entry.h"

namespace mm::etg {

/// @brief What a caller wants back from an ESI document.
///
/// A plain value type with no transport in it: a caller fills it from an HTTP query, a CLI flag or
/// a literal, and this library never learns which.
struct EsiParseRequest {
  /// @brief Restricts the module merge; empty means every ident a device's slots reference.
  ///
  /// Applied to every device, by intersection with the idents that device actually references —
  /// so naming @c {0x04020001, 0x22D20001} leaves a device whose only slot holds @c 0x04020001
  /// untouched while pinning a safe-motion device to one FSoE variant.
  std::vector<uint32_t> moduleIdents;
};

/// @brief Parses a comma-separated list of hex-or-decimal values, as a query parameter carries it.
///
/// Accepts the ESI @c "#x" prefix, a C-style @c "0x" prefix or plain decimal, and tolerates spaces
/// around the separators. An empty input yields an empty list.
///
/// @return The parsed values, or an error naming the element that failed.
std::expected<std::vector<uint32_t>, std::string> parseIdentList(std::string_view csv);

/// @brief Parses an ESI document and renders it as JSON.
///
/// The result is the vendor, every module, and **every device with its own assembled entry
/// table** — each device object carries its identity, slots, counts, its flat @c "entries" and
/// the @c "warnings" raised while assembling it. Document-level warnings sit at the top level.
///
/// Returning every device is affordable because object-level annotation is stored once, on
/// subindex 0, rather than repeated onto each subindex (see @c EsiEntry::properties). A device
/// that would otherwise cost megabytes of duplicated description HTML costs a few hundred
/// kilobytes.
///
/// @param xml     The complete ESI document.
/// @param request Module selection, if any.
/// @param options Flattening options, applied to every device.
/// @return The rendered JSON, or an error when the document will not parse.
std::expected<nlohmann::json, std::string> buildEsiResponse(std::string_view xml,
                                                            const EsiParseRequest& request,
                                                            const EsiEntryOptions& options = {});

}  // namespace mm::etg
