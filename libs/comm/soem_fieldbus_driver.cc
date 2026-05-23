#include "comm/soem_fieldbus_driver.h"

#include <soem/soem.h>

#include <cerrno>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
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

std::expected<int, std::string> SoemFieldbusDriver::scan() {
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

std::expected<void, std::string> SoemFieldbusDriver::readRegister(uint16_t slavePosition,
                                                                  uint16_t address,
                                                                  std::span<uint8_t> data) {
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  int wkc = ecx_FPRD(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     data.data(), EC_TIMEOUTRET);
  if (wkc != 1) {
    return std::unexpected("FPRD slave " + std::to_string(slavePosition) +
                           ": wkc=" + std::to_string(wkc));
  }
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::writeRegister(uint16_t slavePosition,
                                                                   uint16_t address,
                                                                   std::span<const uint8_t> data) {
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  // ecx_FPWR takes void*, not const void*
  int wkc = ecx_FPWR(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     const_cast<uint8_t*>(data.data()), EC_TIMEOUTRET);
  if (wkc != 1) {
    return std::unexpected("FPWR slave " + std::to_string(slavePosition) +
                           ": wkc=" + std::to_string(wkc));
  }
  return {};
}

}  // namespace mm::comm::soem
