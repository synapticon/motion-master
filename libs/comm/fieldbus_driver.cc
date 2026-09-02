#include "comm/fieldbus_driver.h"

#include "core/util.h"

namespace mm::comm {

std::array<uint8_t, kSyncManagerRegisterBytes> encodeSyncManager(const SyncManagerConfig& config) {
  const auto start = mm::core::toBytes<uint16_t>(config.physicalStart);
  const auto length = mm::core::toBytes<uint16_t>(config.length);
  return {start[0],
          start[1],
          length[0],
          length[1],
          static_cast<uint8_t>(config.flags & 0xFFu),
          0x00,
          static_cast<uint8_t>((config.flags >> 16) & 0xFFu),
          0x00};
}

std::optional<SyncManagerConfig> decodeSyncManager(uint8_t index, std::span<const uint8_t> bytes) {
  if (bytes.size() < kSyncManagerRegisterBytes) {
    return std::nullopt;
  }
  SyncManagerConfig config;
  config.index = index;
  config.physicalStart = mm::core::fromBytes<uint16_t>(bytes.subspan(0, 2));
  config.length = mm::core::fromBytes<uint16_t>(bytes.subspan(2, 2));
  // The same 32-bit view of offsets 4 to 7 that encodeSyncManager takes apart.
  config.flags = mm::core::fromBytes<uint32_t>(bytes.subspan(4, 4));
  return config;
}

std::array<uint8_t, kFmmuRegisterBytes> encodeFmmu(const FmmuConfig& config) {
  const auto logicalStart = mm::core::toBytes<uint32_t>(config.logicalStart);
  const auto length = mm::core::toBytes<uint16_t>(config.length);
  const auto physicalStart = mm::core::toBytes<uint16_t>(config.physicalStart);
  return {logicalStart[0],
          logicalStart[1],
          logicalStart[2],
          logicalStart[3],
          length[0],
          length[1],
          config.logicalStartBit,
          config.logicalEndBit,
          physicalStart[0],
          physicalStart[1],
          config.physicalStartBit,
          config.type,
          config.active,
          0x00,
          0x00,
          0x00};
}

std::optional<FmmuConfig> decodeFmmu(uint8_t index, std::span<const uint8_t> bytes) {
  if (bytes.size() < kFmmuRegisterBytes) {
    return std::nullopt;
  }
  FmmuConfig config;
  config.index = index;
  config.logicalStart = mm::core::fromBytes<uint32_t>(bytes.subspan(0, 4));
  config.length = mm::core::fromBytes<uint16_t>(bytes.subspan(4, 2));
  config.logicalStartBit = bytes[6];
  config.logicalEndBit = bytes[7];
  config.physicalStart = mm::core::fromBytes<uint16_t>(bytes.subspan(8, 2));
  config.physicalStartBit = bytes[10];
  config.type = bytes[11];
  config.active = bytes[12];
  return config;
}

}  // namespace mm::comm
