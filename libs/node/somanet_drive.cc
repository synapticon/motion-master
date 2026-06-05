#include "node/somanet_drive.h"

#include <format>
#include <string>

namespace mm::node {

std::expected<SomanetDrive, std::string> createSomanetDrive(Device& device) {
  if (device.vendorId() != kSynapticonVendorId) {
    return std::unexpected(
        std::format("device {} is not a SOMANET drive (vendor 0x{:08X}, expected 0x{:08X})",
                    device.slavePosition(), device.vendorId(), kSynapticonVendorId));
  }
  // A SOMANET drive must also be a CiA402 drive; reuse that check rather than duplicating it.
  if (auto cia = createCia402Drive(device); !cia) {
    return std::unexpected(cia.error());
  }
  return SomanetDrive(device);
}

}  // namespace mm::node
