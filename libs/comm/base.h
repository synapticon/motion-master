#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mm::comm {

/// Network adapter identity with all three representations populated.
///
/// Constructed by @c resolveNetworkAdapter() from a @c --adapter CLI argument
/// that may be given as a MAC address (either Linux colon-separated or Windows
/// dash-separated format) or as an OS interface name.  All three fields are
/// always populated — the adapter map is consulted to resolve the missing form.
struct NetworkAdapter {
  std::string macLinux;    ///< Colon-separated uppercase MAC, e.g. @c "AA:BB:CC:DD:EE:FF".
  std::string macWindows;  ///< Dash-separated uppercase MAC, e.g. @c "AA-BB-CC-DD-EE-FF".
  std::string name;        ///< OS interface name, e.g. @c "eth0" or @c "\\Device\\NPF_{GUID}".
  /// Human-readable hardware description, e.g. @c "Realtek USB GbE Family Controller".
  ///
  /// Populated on Windows, where @c name is an NPF device path built from a GUID and identifies
  /// nothing to a reader. Empty on Linux and macOS, where the interface name is the identification
  /// (SOEM fills its own adapter description with the interface name there, so it carries nothing
  /// extra) — so a consumer must treat this as optional and fall back to @c name.
  std::string description;
};

/// @brief Serialises a NetworkAdapter to JSON.
///
/// Produces an object with keys @c macLinux, @c macWindows, @c name, @c description.
/// Participates in nlohmann ADL so that @c nlohmann::json(adapters) serialises a
/// @c std::vector<NetworkAdapter> automatically.
/// @param j  Output JSON value.
/// @param a  Adapter to serialise.
void to_json(nlohmann::json& j, const NetworkAdapter& a);

/// @brief Returns true if @p s is a MAC address in either Linux (@c :) or
///        Windows (@c -) separator format.
///
/// Accepts exactly 17 characters: six pairs of hex digits separated by
/// consistent colon or dash delimiters, e.g. @c "AA:BB:CC:DD:EE:FF" or
/// @c "aa-bb-cc-dd-ee-ff".  Mixed separators are rejected.
/// @param s  String to test.
/// @return @c true when @p s matches the pattern; @c false otherwise.
bool isMacAddress(const std::string& s);

/// @brief Normalises a MAC address string to uppercase with the given separator.
///
/// Extracts the six hex-octet pairs from @p raw (which must pass
/// @c isMacAddress()) and rebuilds the string in uppercase with @p sep as the
/// field delimiter.
/// @param raw  Source MAC string (colon- or dash-separated, any case).
/// @param sep  Output separator character, typically @c ':' or @c '-'.
/// @return Normalised MAC string of the form @c "XX<sep>XX<sep>…<sep>XX".
/// @pre @c isMacAddress(raw) == true
std::string normalizeMac(const std::string& raw, char sep);

/// @brief Decodes a MAC address string into its six bytes, in wire order.
///
/// The string form is what an adapter carries and what a human types; a protocol that puts a MAC on
/// the wire — an ENI @c Source, an EoE frame — needs the bytes. @c isMacAddress decides what is
/// acceptable, so both separators and either case are.
///
/// @param s  Source MAC string.
/// @return The six bytes, most significant first, or @c std::nullopt when @p s is not a MAC
/// address.
std::optional<std::array<uint8_t, 6>> parseMac(const std::string& s);

/// @brief Enumerates all network adapters visible to the OS.
///
/// Scans every network adapter and returns a fully populated @c NetworkAdapter
/// per NIC, so callers can select a specific one by its hardware address rather
/// than by its OS-assigned name, which can change across reboots or driver
/// updates.  All three representations are populated at enumeration time.
///
/// **Platform behaviour**
/// - **Linux** — iterates the adapter list returned by SOEM's
///   @c ec_find_adapters() and queries each interface with @c SIOCGIFHWADDR
///   via a temporary @c SOCK_DGRAM socket.  Adapters for which the @c ioctl
///   call fails are silently skipped.
/// - **macOS** — reads link-layer (@c AF_LINK) addresses from @c getifaddrs().
/// - **Windows** — calls @c GetAdaptersAddresses() (IPHLPAPI) to enumerate
///   adapters.  Interface names are formatted as WinPcap/Npcap NPF paths:
///   @c "\\Device\\NPF_<AdapterName>".
///
/// **MAC address format** — @c macLinux is uppercase colon-separated hex
/// octets, e.g. @c "AA:BB:CC:DD:EE:FF"; @c macWindows is the dash-separated
/// equivalent.  Both are consistent across platforms.
///
/// Returns an empty vector if adapter enumeration fails (e.g. insufficient
/// privileges or no adapters present).
///
/// @note Call once at startup to resolve a configured MAC address to the
///       interface name required by a fieldbus driver.  Do not call in a
///       tight loop — OS adapter enumeration is slow.
std::vector<NetworkAdapter> enumerateNetworkAdapters();

/// @brief Resolves a network adapter specifier to a fully populated @c NetworkAdapter.
///
/// @p input may be any of the three accepted forms:
///   - Linux MAC address:   @c "AA:BB:CC:DD:EE:FF"
///   - Windows MAC address: @c "AA-BB-CC-DD-EE-FF"
///   - OS interface name:   @c "eth0", @c "\\Device\\NPF_{GUID}"
///
/// Calls @c enumerateNetworkAdapters() once to resolve the form that was
/// not supplied.
/// @param input  User-supplied adapter specifier string.
/// @return Fully populated @c NetworkAdapter, or an error string if the adapter
///         cannot be found in the OS adapter map.
std::expected<NetworkAdapter, std::string> resolveNetworkAdapter(const std::string& input);

}  // namespace mm::comm
