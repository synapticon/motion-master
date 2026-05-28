#include "comm/al_status_codes.h"

#include <nlohmann/json.hpp>

namespace mm::comm {

void to_json(nlohmann::json& j, const AlStatusCode& c) {
  j = nlohmann::json{
      {"code", c.code},
      {"name", c.name},
      {"description", c.description},
      {"terminal", c.terminal},
  };
}

}  // namespace mm::comm
