#include "comm/fieldbus_driver.h"

#include <nlohmann/json.hpp>

namespace mm::comm {

void to_json(nlohmann::json& j, const CoeCapabilities& c) {
  j = nlohmann::json{
      {"sdo", c.sdo},
      {"sdoInfo", c.sdoInfo},
      {"pdoAssign", c.pdoAssign},
      {"pdoConfig", c.pdoConfig},
      {"uploadAtStartup", c.uploadAtStartup},
      {"completeAccess", c.completeAccess},
  };
}

}  // namespace mm::comm
