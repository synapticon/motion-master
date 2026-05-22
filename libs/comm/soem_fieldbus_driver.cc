#include "comm/soem_fieldbus_driver.h"

#include <soem/soem.h>

#include <cerrno>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace mm::comm::soem {

SoemFieldbusDriver::SoemFieldbusDriver(std::string ifname) : ifname_(std::move(ifname)) {}

SoemFieldbusDriver::~SoemFieldbusDriver() {}

std::expected<void, std::string> SoemFieldbusDriver::init() {
  ctx_ = std::make_unique<ecx_contextt>();
  if (!ecx_init(ctx_.get(), ifname_.c_str())) {
    ctx_.reset();
    return std::unexpected("ecx_init failed on " + ifname_ + ": " + std::strerror(errno));
  }
  return {};
}

std::expected<int, std::string> SoemFieldbusDriver::configure() {
  ctx_->manualstatechange = 1;
  int found = ecx_config_init(ctx_.get());
  if (found <= 0) {
    return std::unexpected("ecx_config_init found no slaves on " + ifname_);
  }
  return found;
}

void SoemFieldbusDriver::exchangeProcessData() {}

void SoemFieldbusDriver::stop() {}

SlaveInfo SoemFieldbusDriver::slaveInfo(uint16_t position) const {
  const auto& s = ctx_->slavelist[position];
  return {
      .name = std::string(s.name),
      .vendorId = s.eep_man,
      .productCode = s.eep_id,
      .revisionNumber = s.eep_rev,
      .serialNumber = s.eep_ser,
  };
}

int SoemFieldbusDriver::slaveCount() const { return ctx_ ? ctx_->slavecount : 0; }

}  // namespace mm::comm::soem
