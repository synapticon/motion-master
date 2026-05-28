#include "comm/object_data_types.h"

#include <nlohmann/json.hpp>

namespace mm::comm {

void to_json(nlohmann::json& j, const ObjectDataTypeInfo& info) {
  j = nlohmann::json{
      {"code", info.code},
      {"name", info.name},
      {"bitSize", info.bitSize},
  };
}

}  // namespace mm::comm
