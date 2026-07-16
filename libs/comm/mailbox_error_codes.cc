#include "comm/mailbox_error_codes.h"

#include <nlohmann/json.hpp>

namespace mm::comm {

void to_json(nlohmann::json& j, const MailboxErrorCode& e) {
  j = nlohmann::json{
      {"code", e.code},
      {"name", e.name},
      {"description", e.description},
  };
}

}  // namespace mm::comm
