#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace mm::comm {

/// @brief Metadata for a single FoE error code entry.
struct FoeErrorCode {
  uint32_t code;                 ///< 32-bit FoE error code as sent in the FoE ERROR packet.
  std::string_view name;         ///< Short human-readable name.
  std::string_view description;  ///< Full description of the error condition.
};

/// @brief Serialises a FoeErrorCode to JSON.
///
/// Produces an object with keys @c code, @c name, @c description.
/// Participates in nlohmann ADL so that @c nlohmann::json(kFoeErrorCodes) works automatically.
///
/// @param j  Output JSON value.
/// @param e  FoE error code entry to serialise.
void to_json(nlohmann::json& j, const FoeErrorCode& e);

/// @brief Catalogue of FoE error codes defined in ETG.1000.6 §5.8.5, Table 93.
///
/// Codes in the 0x8000 range are the standard EtherCAT FoE error codes.
/// Vendor-specific codes are outside this range and are not listed here.
inline constexpr auto kFoeErrorCodes = std::to_array<FoeErrorCode>({
    {0x00000000, "Undefined", "Unspecified or vendor-specific FoE error"},
    {0x00008000, "Not defined", "General FoE error with no further classification"},
    {0x00008001, "Not found", "The requested file was not found on the slave"},
    {0x00008002, "Access denied", "Access to the requested file is denied"},
    {0x00008003, "Disk full", "The slave has insufficient storage to complete the transfer"},
    {0x00008004, "Illegal", "Illegal FoE operation"},
    {0x00008005, "Packet number wrong", "Received a data packet with an unexpected sequence number"},  // NOLINT
    {0x00008006, "Already exists", "The file already exists on the slave"},
    {0x00008007, "No user", "No user is logged in; authentication required"},
    {0x00008008, "Bootstrap only", "This operation is only available in Bootstrap state"},
    {0x00008009, "Not Bootstrap", "This operation is not available in Bootstrap state"},
    {0x0000800A, "No rights", "Insufficient rights to perform the requested operation"},
    {0x0000800B, "Program error", "A program error occurred on the slave"},
});

}  // namespace mm::comm
