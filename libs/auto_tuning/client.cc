#include "auto_tuning/client.h"

#include <string>

namespace mm::auto_tuning {

std::expected<mm::net::Response, std::string> Client::run(const std::string& requestBody,
                                                          std::chrono::seconds timeout) const {
  return mm::net::httpPost(baseUrl_ + "/api/run", requestBody, "application/json", timeout);
}

std::expected<mm::net::Response, std::string> Client::spec() const {
  // Short: the document is bundled into the executable, so this is a read from memory, not work.
  return mm::net::httpGet(baseUrl_ + "/api/swagger.yml", std::chrono::seconds{5});
}

}  // namespace mm::auto_tuning
