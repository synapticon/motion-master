#include "node/eni_request.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "comm/esc_registers.h"
#include "comm/fieldbus_driver.h"
#include "core/util.h"
#include "etg/eni.h"
#include "etg/eni_reader.h"

namespace mm::node {

namespace {

/// An ESC holds sixteen of each block (ETG.1000.4 Tables 57 and 59).
constexpr std::uint16_t kBlockInstances = 16;

/// AL Control and AL Status, the two registers a state request and a state wait address.
constexpr std::uint16_t kRegAlControl = 0x0120;
constexpr std::uint16_t kRegAlStatus = 0x0130;

/// @brief What an @c Ado selects, and which instance of it.
///
/// The register catalogue names the first instance of a repeating block — @c sm0 at 0x0800, @c
/// fmmu0 at 0x0600 — and the rest follow at a fixed stride. So an address inside a block resolves
/// to that block's name plus the index, which is what a reader needs to tell "sync manager 2" from
/// "sync manager 0".
struct RegisterMatch {
  std::string name;                      ///< Catalogue name, e.g. @c "sm2".
  std::string description;               ///< Catalogue description of the block.
  std::optional<std::uint8_t> instance;  ///< Which channel or entity, for a repeating block.
  bool syncManager = false;              ///< The address is a Sync Manager block.
  bool fmmu = false;                     ///< The address is an FMMU block.
};

/// Sync managers and FMMUs repeat; every other register in the catalogue is looked up as it stands.
std::optional<RegisterMatch> matchRegister(std::uint16_t ado) {
  const auto blockIndex = [ado](std::uint16_t base,
                                std::uint16_t width) -> std::optional<std::uint8_t> {
    if (ado < base || ado >= base + kBlockInstances * width) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>((ado - base) / width);
  };

  RegisterMatch match;
  if (const auto index =
          blockIndex(mm::comm::kSyncManagerRegisterBase, mm::comm::kSyncManagerRegisterBytes);
      index) {
    match.name = std::format("sm{}", *index);
    match.description =
        "Sync manager channel: physical start, length, control, status and activation";
    match.instance = index;
    match.syncManager = true;
    return match;
  }
  if (const auto index = blockIndex(mm::comm::kFmmuRegisterBase, mm::comm::kFmmuRegisterBytes);
      index) {
    match.name = std::format("fmmu{}", *index);
    match.description = "FMMU entity: logical window, physical window and the service it serves";
    match.instance = index;
    match.fmmu = true;
    return match;
  }
  const auto entry =
      std::ranges::find(mm::comm::kEscRegisters, ado, &mm::comm::EscRegister::address);
  if (entry == mm::comm::kEscRegisters.end()) {
    return std::nullopt;
  }
  match.name = entry->name;
  match.description = entry->description;
  return match;
}

/// The AL Control and AL Status registers carry a state in their low nibble, so a write or a
/// retry-until read is really "go to SAFE-OP" or "wait for SAFE-OP" — which is the single most
/// useful thing to say about an init command.
std::optional<std::string> alStateName(std::span<const std::uint8_t> data) {
  if (data.empty()) {
    return std::nullopt;
  }
  switch (data[0] & 0x0F) {
    case 0x01:
      return "INIT";
    case 0x02:
      return "PRE-OP";
    case 0x03:
      return "BOOT";
    case 0x04:
      return "SAFE-OP";
    case 0x08:
      return "OP";
    default:
      return std::nullopt;
  }
}

/// Decodes an init command's payload where the payload is a register block this project already
/// knows how to encode. Anything else keeps its hex and says nothing further, which is honest.
nlohmann::json decodePayload(const RegisterMatch& match, std::span<const std::uint8_t> data) {
  if (match.syncManager) {
    if (const auto config = mm::comm::decodeSyncManager(match.instance.value_or(0), data); config) {
      return nlohmann::json{{"physicalStart", config->physicalStart},
                            {"length", config->length},
                            {"controlByte", config->flags & 0xFFu},
                            {"enabled", ((config->flags >> 16) & 0x01u) != 0}};
    }
    return nlohmann::json{{"error", "the payload is shorter than a sync manager block"}};
  }
  if (match.fmmu) {
    if (const auto config = mm::comm::decodeFmmu(match.instance.value_or(0), data); config) {
      return nlohmann::json{{"logicalStart", config->logicalStart},
                            {"length", config->length},
                            {"logicalStartBit", config->logicalStartBit},
                            {"logicalEndBit", config->logicalEndBit},
                            {"physicalStart", config->physicalStart},
                            {"physicalStartBit", config->physicalStartBit},
                            {"reads", (config->type & 0x01u) != 0},
                            {"writes", (config->type & 0x02u) != 0},
                            {"active", config->active != 0}};
    }
    return nlohmann::json{{"error", "the payload is shorter than an FMMU block"}};
  }
  return nlohmann::json{};
}

/// Adds a @c "register" object to one already-serialised init command. The command keeps everything
/// the reader gave it; this only says what the address and the payload mean.
void annotate(nlohmann::json& command) {
  if (!command.contains("ado")) {
    return;
  }
  const auto ado = command["ado"].get<std::uint16_t>();
  const auto match = matchRegister(ado);
  if (!match) {
    return;
  }
  nlohmann::json annotation{{"name", match->name}, {"description", match->description}};
  if (match->instance.has_value()) {
    annotation["instance"] = *match->instance;
  }

  std::vector<std::uint8_t> data;
  if (command.contains("data")) {
    if (auto bytes = mm::core::fromHex(command["data"]["hex"].get<std::string>()); bytes) {
      data = std::move(*bytes);
    }
  }
  if (!data.empty()) {
    if (const nlohmann::json decoded = decodePayload(*match, data); !decoded.empty()) {
      annotation["decoded"] = decoded;
    }
  }

  // A state request is a write to AL Control; a wait is a read of AL Status with a Validate, where
  // the state being waited for is in the validate data rather than the payload.
  if (ado == kRegAlControl && !data.empty()) {
    if (const auto state = alStateName(data); state) {
      annotation["requestsState"] = *state;
    }
  }
  if (ado == kRegAlStatus && command.contains("validate")) {
    if (auto bytes = mm::core::fromHex(command["validate"]["data"]["hex"].get<std::string>());
        bytes) {
      if (const auto state = alStateName(*bytes); state) {
        annotation["waitsForState"] = *state;
      }
    }
  }
  command["register"] = annotation;
}

void annotateAll(nlohmann::json& commands) {
  for (nlohmann::json& command : commands) {
    annotate(command);
  }
}

}  // namespace

std::expected<nlohmann::json, std::string> buildEniResponse(std::string_view xml) {
  auto read = mm::etg::readEni(xml);
  if (!read) {
    return std::unexpected(read.error());
  }

  nlohmann::json network = read->network;
  annotateAll(network["master"]["initCmds"]);
  std::size_t datagrams = network["master"]["initCmds"].size();
  std::size_t coeTransfers = 0;
  for (nlohmann::json& slave : network["slaves"]) {
    annotateAll(slave["initCmds"]);
    datagrams += slave["initCmds"].size();
    if (slave.contains("mailbox")) {
      coeTransfers += slave["mailbox"]["coeInitCmds"].size();
    }
  }

  return nlohmann::json{{"network", std::move(network)},
                        {"warnings", read->warnings},
                        {"summary",
                         {{"devices", read->network.slaves.size()},
                          {"datagrams", datagrams},
                          {"coeTransfers", coeTransfers}}}};
}

}  // namespace mm::node
