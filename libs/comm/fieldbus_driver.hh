#pragma once

#include <expected>
#include <string>

namespace mm::comm {

class IFieldbusDriver {
 public:
  virtual ~IFieldbusDriver() = default;

  virtual std::expected<void, std::string> init() = 0;
  virtual void exchangeProcessData() = 0;
  virtual void stop() = 0;
};

}  // namespace mm::comm
