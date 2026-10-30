#include "comm/soem_station_address.h"

#include <spdlog/spdlog.h>

#include <cstdint>

#include "comm/soem_mailbox.h"

namespace mm::comm::soem {

AddressState restoreStationAddress(ecx_contextt* ctx, uint16_t slave) {
  const uint16_t configadr = ctx->slavelist[slave].configadr;
  uint16_t probe = 0;
  if (ecx_FPRD(&ctx->port, configadr, ECT_REG_ALSTAT, sizeof(probe), &probe, kLateFrameTimeoutUs) >
      0) {
    return AddressState::Answering;
  }
  // Auto-increment address: 0 for slave 1, -1 for slave 2, and so on (ec_config.c's ADPh).
  const uint16_t autoIncrement = static_cast<uint16_t>(1 - slave);
  ecx_APWRw(&ctx->port, autoIncrement, ECT_REG_STADR, htoes(configadr), EC_TIMEOUTRET3);
  const bool reachable = ecx_FPRD(&ctx->port, configadr, ECT_REG_ALSTAT, sizeof(probe), &probe,
                                  kLateFrameTimeoutUs) > 0;
  spdlog::info(
      "Device {}: it had reset and lost its station address; re-assigned 0x{:04X} at its "
      "wire position — {}",
      slave, configadr, reachable ? "it is answering again" : "it is still not answering");
  return reachable ? AddressState::Reassigned : AddressState::Unreachable;
}

}  // namespace mm::comm::soem
