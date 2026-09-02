#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "etg/eni.h"

namespace mm::etg {

/// @brief A network read out of an ENI document, and what the document held that this model does
///        not.
///
/// A warning is never a failure. The whole point of reading an ENI is to read one somebody else's
/// tool wrote, and such a document routinely carries elements this library has no room for — a
/// hot-connect group, an SoE init command, a PDO declaration. Dropping them silently would let a
/// reader believe it had seen everything, so each one is named once and the rest of the document
/// is returned intact.
struct EniRead {
  EniNetwork network;                 ///< Everything the model covers.
  std::vector<std::string> warnings;  ///< One per element seen and not modelled, or not decodable.
};

/// @brief Reads an ENI document into a network.
///
/// Tolerant by design, and the inverse of @c writeEni over everything the model covers. Only three
/// things fail a read: XML that will not parse, a root element that is not @c EtherCATConfig, and
/// a missing @c Config. A malformed value inside the document costs that value and a warning.
///
/// **Written to be more permissive than @c writeEni, on purpose.** A previous port of @c A is
/// accepted here and refused there, because ETG.2100 allows it and ENI Schema 1.7 does not — a
/// reader takes what the file says, a writer emits only what validates. The same asymmetry applies
/// to the sample documents ETG itself ships, which omit elements their own schema marks mandatory.
///
/// @param xml  The complete ENI document.
/// @return The network and any warnings, or an error naming what made the document unreadable.
std::expected<EniRead, std::string> readEni(std::string_view xml);

}  // namespace mm::etg
