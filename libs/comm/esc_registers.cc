#include "comm/esc_registers.h"

#include <nlohmann/json.hpp>

namespace mm::comm {

void to_json(nlohmann::json& j, const EscRegister& r) {
  j = nlohmann::json{
      {"address", r.address},
      {"length", r.length},
      {"name", r.name},
      {"description", r.description},
  };
}

}  // namespace mm::comm
