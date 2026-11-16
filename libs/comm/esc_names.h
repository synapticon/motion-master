#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace mm::comm {

/// @brief The names of the mailbox protocols whose bits are set in a protocol word, in the order of
///        their bits: AoE, EoE, CoE, FoE, SoE, VoE (ETG.2010 Table 4).
std::vector<std::string_view> mailboxProtocolNames(uint16_t protocols);

/// @brief The names of the CoE features whose bits are set in a CoE details byte: SDO, SDO Info,
///        PDO Assign, PDO Config, Upload, Complete Access (ETG.2010 Table 7). A device advertises
///        them in its SII; they are a claim, not a test.
std::vector<std::string_view> coeDetailNames(uint8_t coeDetails);

/// @brief The name of a sync manager type, such as "Mailbox Out" for 1 (ETG.2010 Table 11), or
///        @c std::nullopt for a value it does not define.
std::optional<std::string_view> syncManagerTypeName(uint8_t type);

/// @brief What the SII says an FMMU is used for, such as "Outputs" for 1 (ETG.2010 Table 9), or
///        @c std::nullopt for a reserved value. This is the usage byte of the SII, not the type
///        register of the ESC, which numbers the other way round.
std::optional<std::string_view> fmmuUsageName(uint8_t usage);

/// @brief What a configured FMMU maps, from its type register (0x06yB): bit 0 maps read accesses,
///        which carry the inputs, and bit 1 write accesses, which carry the outputs. An FMMU whose
///        physical start lies in the sync manager registers (0x0800 to 0x087F) maps the status of
///        a mailbox, which is not process data, and is "Mailbox state". @c std::nullopt for a type
///        the register does not define.
std::optional<std::string_view> fmmuTypeName(uint8_t type, uint16_t physicalStart);

/// @brief A name as JSON: the string, or null for @c std::nullopt.
nlohmann::json nameJson(std::optional<std::string_view> name);

/// @brief Names as a JSON array of strings.
nlohmann::json namesJson(const std::vector<std::string_view>& names);

}  // namespace mm::comm
