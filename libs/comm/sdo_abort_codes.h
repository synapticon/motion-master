#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace mm::comm {

/// @brief Metadata for a single CoE SDO abort code entry.
struct SdoAbortCode {
  uint32_t code = 0;             ///< 32-bit code from the Abort SDO Transfer request.
  std::string_view description;  ///< Human-readable meaning of the abort.
};

/// @brief Serialises an SdoAbortCode to JSON.
///
/// Produces an object with keys @c code and @c description.  Participates in nlohmann ADL so
/// that @c nlohmann::json(kSdoAbortCodes) works automatically.
void to_json(nlohmann::json& j, const SdoAbortCode& e);

/// @brief Catalogue of CoE SDO abort codes.
///
/// A faithful transcription of ETG.1000.6 §5.6.2.7.2, Table 41 (which reproduces the CANopen
/// CiA 301 SDO abort transfer codes). A slave returns one of these in the Abort SDO Transfer
/// request when a mailbox SDO read/write fails. SOEM carries the identical set in its
/// @c ec_sdoerrorlist; the @c FieldbusDriver surfaces SDO failures only as text, embedding the
/// raw @c 0x... code, so this table lets a caller decode that code to a reason.
inline constexpr auto kSdoAbortCodes = std::to_array<SdoAbortCode>({
    {0x05030000, "Toggle bit not changed"},
    {0x05040000, "SDO protocol timeout"},
    {0x05040001, "Client/Server command specifier not valid or unknown"},
    {0x05040005, "Out of memory"},
    {0x06010000, "Unsupported access to an object"},
    {0x06010001, "Attempt to read a write-only object"},
    {0x06010002, "Attempt to write a read-only object"},
    {0x06010003, "Subindex cannot be written; SI0 must be 0 for write access"},
    {0x06010004, "SDO Complete Access not supported for variable-length objects such as ENUM"},
    {0x06010005, "Object length exceeds mailbox size"},
    {0x06010006, "Object mapped to RxPDO, SDO download blocked"},
    {0x06020000, "The object does not exist in the object dictionary"},
    {0x06040041, "The object cannot be mapped into the PDO"},
    {0x06040042, "The number and length of the objects to be mapped would exceed the PDO length"},
    {0x06040043, "General parameter incompatibility reason"},
    {0x06040047, "General internal incompatibility in the device"},
    {0x06060000, "Access failed due to a hardware error"},
    {0x06070010, "Data type mismatch: length of service parameter does not match"},
    {0x06070012, "Data type mismatch: length of service parameter too high"},
    {0x06070013, "Data type mismatch: length of service parameter too low"},
    {0x06090011, "Subindex does not exist"},
    {0x06090030, "Value range of parameter exceeded (write access only)"},
    {0x06090031, "Value of parameter written too high"},
    {0x06090032, "Value of parameter written too low"},
    {0x06090036, "Maximum value is less than minimum value"},
    {0x08000000, "General error"},
    {0x08000020, "Data cannot be transferred or stored to the application"},
    {0x08000021,
     "Data cannot be transferred or stored to the application because of local control"},
    {0x08000022,
     "Data cannot be transferred or stored to the application because of the present device state"},
    {0x08000023, "Object dictionary dynamic generation failed or no object dictionary is present"},
});

/// @brief Returns the description of @p code, or an empty view if @p code is not a known
///        SDO abort code (vendor-specific, or a stale/garbage read).
constexpr std::string_view sdoAbortCodeDescription(uint32_t code) {
  const auto it = std::find_if(kSdoAbortCodes.begin(), kSdoAbortCodes.end(),
                               [code](const SdoAbortCode& e) { return e.code == code; });
  return it != kSdoAbortCodes.end() ? it->description : std::string_view{};
}

}  // namespace mm::comm
