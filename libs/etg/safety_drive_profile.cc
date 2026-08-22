#include "etg/safety_drive_profile.h"

#include <cstddef>

namespace mm::etg {
namespace {

constexpr bool bitSet(uint8_t octet, uint8_t bit) { return (octet & (1u << bit)) != 0; }

constexpr int32_t readLe32(std::span<const uint8_t> src, size_t offset) {
  return static_cast<int32_t>(static_cast<uint32_t>(src[offset]) |
                              (static_cast<uint32_t>(src[offset + 1]) << 8) |
                              (static_cast<uint32_t>(src[offset + 2]) << 16) |
                              (static_cast<uint32_t>(src[offset + 3]) << 24));
}

constexpr int16_t readLe16(std::span<const uint8_t> src, size_t offset) {
  return static_cast<int16_t>(static_cast<uint16_t>(src[offset]) |
                              (static_cast<uint16_t>(src[offset + 1]) << 8));
}

}  // namespace

void sdpEncodeControl(SdpControl control, std::span<uint8_t> safeOutputs) {
  if (safeOutputs.empty()) {
    return;
  }
  uint8_t octet = 0;
  // Inverted on the wire: the bit is set to *permit* torque, so zero is the safe value.
  if (!control.stoRequested) {
    octet |= static_cast<uint8_t>(1u << kSdpControlStoBit);
  }
  if (control.errorAcknowledge) {
    octet |= static_cast<uint8_t>(1u << kSdpControlErrorAckBit);
  }
  safeOutputs[0] = octet;
}

SdpControl sdpDecodeControl(std::span<const uint8_t> safeOutputs) {
  if (safeOutputs.empty()) {
    return SdpControl{};
  }
  return SdpControl{
      .stoRequested = !bitSet(safeOutputs[0], kSdpControlStoBit),
      .errorAcknowledge = bitSet(safeOutputs[0], kSdpControlErrorAckBit),
  };
}

SdpStatus sdpDecodeStatus(std::span<const uint8_t> safeInputs) {
  if (safeInputs.empty()) {
    return SdpStatus{};
  }
  return SdpStatus{
      .stoActive = bitSet(safeInputs[0], kSdpStatusStoActiveBit),
      .error = bitSet(safeInputs[0], kSdpStatusErrorBit),
  };
}

SdpProcessValues sdpDecodeProcessValues(std::span<const uint8_t> safeInputs) {
  if (safeInputs.size() < kSdpSafeInputsWithProcessValues) {
    return SdpProcessValues{};
  }
  const uint8_t validity = safeInputs[1];
  return SdpProcessValues{
      .positionFixedPoint = readLe32(safeInputs, 2),
      .velocityMilliRpm = readLe32(safeInputs, 6),
      .torqueMillinewtonMetres = readLe16(safeInputs, 10),
      .positionValid = bitSet(validity, kSdpValidPositionBit),
      .positionReferenced = bitSet(validity, kSdpValidPositionReferencedBit),
      .velocityValid = bitSet(validity, kSdpValidVelocityBit),
      .torqueValid = bitSet(validity, kSdpValidTorqueBit),
      .crossCheckOk = bitSet(validity, kSdpValidCrossCheckBit),
  };
}

}  // namespace mm::etg
