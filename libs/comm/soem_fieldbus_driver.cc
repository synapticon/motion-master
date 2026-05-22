#include "comm/soem_fieldbus_driver.h"

#include <soem/soem.h>

#include <string>
#include <utility>

namespace mm::comm::soem {

SoemFieldbusDriver::SoemFieldbusDriver(std::string ifname)
    : ifname_(std::move(ifname)), ctx_(std::make_unique<ecx_contextt>()) {}

SoemFieldbusDriver::~SoemFieldbusDriver() {}

std::expected<void, std::string> SoemFieldbusDriver::init() {
  return {};
}

void SoemFieldbusDriver::exchangeProcessData() {}

void SoemFieldbusDriver::stop() {}

int SoemFieldbusDriver::slaveCount() const { return slave_count_; }

}  // namespace mm::comm::soem
