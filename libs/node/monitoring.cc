#include "node/monitoring.h"

#include <nlohmann/json.hpp>

namespace mm::node {

void to_json(nlohmann::json& j, const MonitoredParameter& p) {
  j = nlohmann::json{
      {"devicePosition", p.devicePosition},
      {"index", p.index},
      {"subindex", p.subindex},
  };
}

void to_json(nlohmann::json& j, const Monitoring& m) {
  j = nlohmann::json{
      {"topic", m.topic},
      {"interval", m.interval.count()},
      {"parameters", m.parameters},
  };
  if (m.name) {
    j["name"] = *m.name;
  }
}

}  // namespace mm::node
