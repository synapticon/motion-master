#include "comm/sdo_abort_codes.h"

#include <nlohmann/json.hpp>

namespace mm::comm {

void to_json(nlohmann::json& j, const SdoAbortCode& e) {
  j = nlohmann::json{
      {"code", e.code},
      {"description", e.description},
  };
}

}  // namespace mm::comm
