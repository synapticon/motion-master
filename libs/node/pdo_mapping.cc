#include "node/pdo_mapping.h"

#include <nlohmann/json.hpp>

namespace mm::node {

void to_json(nlohmann::json& j, const PdoMappingEntry& e) {
  j = {{"index", e.index},
       {"subindex", e.subindex},
       {"bitLength", e.bitLength},
       {"bitOffset", e.bitOffset}};
}

void to_json(nlohmann::json& j, const PdoMappingObject& o) {
  j = {{"pdoIndex", o.pdoIndex}, {"entries", o.entries}};
}

void to_json(nlohmann::json& j, const PdoMapping& m) {
  j = {{"outputs", m.outputs}, {"inputs", m.inputs}};
}

}  // namespace mm::node
