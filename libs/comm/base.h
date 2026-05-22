#pragma once

#include <expected>
#include <map>
#include <string>

namespace mm::comm {

/// Network adapter identity with all three representations populated.
///
/// Constructed by @c resolveNetworkAdapter() from a @c --adapter CLI argument
/// that may be given as a MAC address (either Linux colon-separated or Windows
/// dash-separated format) or as an OS interface name.  All three fields are
/// always populated — the adapter map is consulted to resolve the missing form.
struct NetworkAdapter {
  std::string macLinux;     ///< Colon-separated uppercase MAC, e.g. @c "AA:BB:CC:DD:EE:FF".
  std::string macWindows;   ///< Dash-separated uppercase MAC, e.g. @c "AA-BB-CC-DD-EE-FF".
  std::string adapterName;  ///< OS interface name, e.g. @c "eth0" or @c "\\Device\\NPF_{GUID}".
};

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
/// Extracts the six hex-octet pairs from @p raw (which must have passed
/// @c isMacAddress()) and rebuilds the string in uppercase with @p sep as the
/// field delimiter.
/// @param raw  Source MAC string (colon- or dash-separated, any case).
/// @param sep  Output separator character, typically @c ':' or @c '-'.
/// @return Normalised MAC string of the form @c "XX<sep>XX<sep>…<sep>XX".
/// @pre @c isMacAddress(raw) == true
std::string normalizeMac(const std::string& raw, char sep);

/// @brief Enumerates all network adapters and returns a map from MAC address
///        to OS interface name.
///
/// Scans every network adapter visible to the OS and builds a lookup table
/// that lets callers select a specific NIC by its hardware address rather than
/// by its OS-assigned name, which can change across reboots or driver updates.
///
/// **Platform behaviour**
/// - **Linux** — iterates the adapter list returned by SOEM's
///   @c ec_find_adapters() and queries each interface with @c SIOCGIFHWADDR
///   via a temporary @c SOCK_DGRAM socket.  Adapters for which the @c ioctl
///   call fails are silently skipped.
/// - **Windows** — calls @c GetAdaptersAddresses() (IPHLPAPI) to enumerate
///   adapters.  Interface names are formatted as WinPcap/Npcap NPF paths:
///   @c "\\Device\\NPF_<AdapterName>".
///
/// **MAC address format** — keys are uppercase colon-separated hex octets,
/// e.g. @c "AA:BB:CC:DD:EE:FF", consistent across platforms.
///
/// @return A @c std::map<std::string, std::string> where:
///   - **key**   = MAC address string, e.g. @c "00:1A:2B:3C:4D:5E"
///   - **value** = OS interface name, e.g. @c "eth0" (Linux) or
///                 @c "\\Device\\NPF_{GUID}" (Windows)
///
/// Returns an empty map if adapter enumeration fails (e.g. insufficient
/// privileges or no adapters present).
///
/// @note Call once at startup to resolve a configured MAC address to the
///       interface name required by a fieldbus driver.  Do not call in a
///       tight loop — OS adapter enumeration is slow.
std::map<std::string, std::string> mapMacAddressesToInterfaces();

/// @brief Resolves a network adapter specifier to a fully populated @c NetworkAdapter.
///
/// @p input may be any of the three accepted forms:
///   - Linux MAC address:   @c "AA:BB:CC:DD:EE:FF"
///   - Windows MAC address: @c "AA-BB-CC-DD-EE-FF"
///   - OS interface name:   @c "eth0", @c "\\Device\\NPF_{GUID}"
///
/// Calls @c mapMacAddressesToInterfaces() once to resolve the form that was
/// not supplied.
/// @param input  User-supplied adapter specifier string.
/// @return Fully populated @c NetworkAdapter, or an error string if the adapter
///         cannot be found in the OS adapter map.
std::expected<NetworkAdapter, std::string> resolveNetworkAdapter(const std::string& input);

}  // namespace mm::comm
