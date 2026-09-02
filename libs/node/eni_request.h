#pragma once

#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace mm::node {

/// @brief Reads an ENI document and renders it as JSON, with each datagram explained.
///
/// The document alone is not a view. An ENI is a script of EtherCAT datagrams, so a page that only
/// listed them would show a reader `FPWR to 2064, data 0018060064000100` and leave the work
/// undone. This names the register that address selects and decodes the payload where the payload
/// is a register block, so the same command reads as *sync manager 2: start 0x1800, length 6,
/// enabled*.
///
/// **The decode lives here rather than in a client**, because it is the same ESC knowledge
/// `mm::comm` already holds and `mm::node::collectEni` already uses to build these commands. A
/// second decoder in TypeScript would drift against the encoder it is supposed to mirror.
///
/// Offline, like the ESI and SII parsers this sits beside. No bus is touched and none is needed —
/// the point is reading a document somebody else's configuration tool wrote.
///
/// The result carries the network under @c "network", the reader's warnings under @c "warnings",
/// and a count of devices and commands under @c "summary".
///
/// @param xml  The complete ENI document.
/// @return The rendered JSON, or an error when the document will not read.
std::expected<nlohmann::json, std::string> buildEniResponse(std::string_view xml);

}  // namespace mm::node
