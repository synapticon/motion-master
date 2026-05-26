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

/// @brief Catalogue of FoE error codes defined in ETG.1000.6 §5.5.
///
/// Codes in the 0x8000 range are the standard EtherCAT FoE error codes.
/// Vendor-specific codes are outside this range and are not listed here.
inline constexpr auto kFoeErrorCodes = std::to_array<FoeErrorCode>({
    {0x00000000, "Undefined error", "Unspecified FoE error"},
    {0x00008000, "Not defined", "General FoE error with no further classification"},
    {0x00008001, "Access denied", "The requested file operation is not permitted"},
    {0x00008002, "File not found", "The requested file does not exist on the slave"},
    {0x00008003, "Date changed", "The file was modified since the transfer started"},
    {0x00008004, "Could not delete", "The slave could not delete the file"},
    {0x00008005, "No access", "Access to the file system is not available"},
    {0x00008006, "Wrong packet number", "Received a data packet with an unexpected sequence number"},
    {0x00008007, "Unexpected opcode", "Received an opcode that was not expected at this point"},
    {0x00008008, "No password", "A password is required but was not provided"},
    {0x00008009, "No disk space", "The slave has insufficient storage to complete the transfer"},
    {0x0000800A, "Bootstrap only", "This operation is only available in Bootstrap state"},
    {0x0000800B, "No valid firmware", "The slave holds no valid firmware image"},
    {0x0000800C, "Checksum error", "The received file failed its integrity check"},
    {0x0000800D, "Wrong file header", "The file header does not match what the slave expects"},
    {0x0000800E, "File type mismatch", "The file type is not compatible with this slave"},
});

}  // namespace mm::comm
