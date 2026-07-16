#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace mm::comm {

/// @brief Metadata for a single CoE mailbox error code entry.
struct MailboxErrorCode {
  uint16_t code = 0;             ///< 16-bit Detail code from the mailbox error reply.
  std::string_view name;         ///< Symbolic name (e.g. @c "MBXERR_SYNTAX").
  std::string_view description;  ///< Human-readable meaning of the error.
};

/// @brief Serialises a MailboxErrorCode to JSON.
///
/// Produces an object with keys @c code, @c name, @c description.  Participates in nlohmann ADL
/// so that @c nlohmann::json(kMailboxErrorCodes) works automatically.
void to_json(nlohmann::json& j, const MailboxErrorCode& e);

/// @brief Catalogue of CoE mailbox error codes.
///
/// A faithful transcription of ETG.1000.4 Table 30 ("Error Reply Service Data"). A slave returns
/// one of these in a mailbox error reply when the mailbox layer (below any specific protocol like
/// CoE/FoE) rejects a transfer. SOEM carries the same set in its @c ec_mbxerrorlist but stops at
/// 0x0008 — this table also lists the spec's 0x0009. The @c FieldbusDriver surfaces mailbox errors
/// as text embedding the raw @c 0x... code, so this table lets a caller decode it to a reason.
inline constexpr auto kMailboxErrorCodes = std::to_array<MailboxErrorCode>({
    {0x0001, "MBXERR_SYNTAX", "Syntax of the 6-octet mailbox header is wrong"},
    {0x0002, "MBXERR_UNSUPPORTEDPROTOCOL", "The mailbox protocol is not supported"},
    {0x0003, "MBXERR_INVALIDCHANNEL",
     "Channel field contains a wrong value (a slave may ignore it)"},
    {0x0004, "MBXERR_SERVICENOTSUPPORTED", "The service in the mailbox protocol is not supported"},
    {0x0005, "MBXERR_INVALIDHEADER",
     "The mailbox protocol header is wrong (excluding the 6-octet mailbox header)"},
    {0x0006, "MBXERR_SIZETOOSHORT", "Length of received mailbox data is too short"},
    {0x0007, "MBXERR_NOMOREMEMORY",
     "Mailbox protocol cannot be processed because of limited resources"},
    {0x0008, "MBXERR_INVALIDSIZE", "The length of data is inconsistent"},
    {0x0009, "MBXERR_SERVICEINWORK", "Mailbox service already in use"},
});

/// @brief Returns the description of @p code, or an empty view if @p code is not a known
///        mailbox error code (vendor-specific, or a stale/garbage read).
constexpr std::string_view mailboxErrorCodeDescription(uint16_t code) {
  const auto it = std::find_if(kMailboxErrorCodes.begin(), kMailboxErrorCodes.end(),
                               [code](const MailboxErrorCode& e) { return e.code == code; });
  return it != kMailboxErrorCodes.end() ? it->description : std::string_view{};
}

}  // namespace mm::comm
