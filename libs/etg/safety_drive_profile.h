#pragma once

#include <cstdint>
#include <span>

namespace mm::etg {

/// @brief Safety controlword bit positions, SafeData octet 0 master to slave (ETG.6100.2 Table 3).
inline constexpr uint8_t kSdpControlStoBit = 0;
inline constexpr uint8_t kSdpControlErrorAckBit = 7;

/// @brief Safety statusword bit positions, SafeData octet 0 slave to master (ETG.6100.2 Table 5).
inline constexpr uint8_t kSdpStatusStoActiveBit = 0;
inline constexpr uint8_t kSdpStatusErrorBit = 7;

/// @brief SafeInputs octets a drive publishes when the safe process values are mapped.
inline constexpr uint16_t kSdpSafeInputsWithProcessValues = 12;

/// @brief Validity bits in the manufacturer statusword, SafeInputs octet 1.
///
/// ETG.6100 defines no "this value is not usable" flag. Its only hook for a failed sensor channel
/// is statusword bit 7 plus a vendor-chosen reaction, so every vendor surveyed — Synapticon and
/// Kollmorgen among them — puts validity bits in the statusword octets past the first. A master
/// that ignores these and uses the values anyway reads zeros when a channel fails, because the
/// device encodes an invalid value as zero rather than holding the last good reading.
inline constexpr uint8_t kSdpValidPositionBit = 0;
inline constexpr uint8_t kSdpValidVelocityBit = 1;
inline constexpr uint8_t kSdpValidTorqueBit = 2;
inline constexpr uint8_t kSdpValidCrossCheckBit = 3;

/// @brief Set when the safe position has an established **absolute** origin.
///
/// Stronger than @c kSdpValidPositionBit, and the distinction matters. The position bit says the
/// measurement can be trusted; this one says the drive knows where the axis actually is. A user of
/// the position against absolute limits — Safe Limited Position above all — must gate on this one.
/// Direction and distance travelled need only the position bit.
inline constexpr uint8_t kSdpValidPositionReferencedBit = 4;

/// @brief The safety controlword, in positive logic.
///
/// The wire inverts the activation bits: controlword bit 0 is **0** to request Safe Torque Off and
/// 1 to permit torque. That inversion is removed here, so @c stoRequested reads the way it sounds.
/// Fail-safe follows from it — an all-zero frame, which is what a lost or fail-safe frame carries,
/// requests STO.
struct SdpControl {
  bool stoRequested = true;       ///< Request Safe Torque Off. The safe default.
  bool errorAcknowledge = false;  ///< Raw level of bit 7; the drive acts on the rising edge.
};

/// @brief The safety statusword, in positive logic.
struct SdpStatus {
  bool stoActive = true;  ///< Safe Torque Off is active: the drive cannot produce torque.
  bool error = false;     ///< At least one safety error is present.
};

/// @brief Safe process values and their validity (ETG.6100.2 ch. 5.4).
///
/// Units come from the device's read-only unit objects (@c 0x6601 position, @c 0x6602 velocity,
/// @c 0x6604 torque, in ETG.1004 notation), because the profile mandates no resolution. A
/// Synapticon drive declares 8.24 fixed-point revolutions of the output shaft, milli-RPM, and
/// milli-newton-metres, which is what the field names below say.
struct SdpProcessValues {
  int32_t positionFixedPoint = 0;       ///< @c 0x6611, 8.24 fixed-point output-shaft revolutions.
  int32_t velocityMilliRpm = 0;         ///< @c 0x6613.
  int16_t torqueMillinewtonMetres = 0;  ///< @c 0x6616.

  bool positionValid = false;
  bool positionReferenced = false;  ///< See @c kSdpValidPositionReferencedBit.
  bool velocityValid = false;
  bool torqueValid = false;
  bool crossCheckOk = false;  ///< Two sensor channels are configured and agree.
};

/// @brief Writes a safety controlword into SafeData octet 0.
///
/// Only octet 0 is touched; the caller owns the rest of the SafeOutputs. An empty span is ignored.
void sdpEncodeControl(SdpControl control, std::span<uint8_t> safeOutputs);

/// @brief Reads the safety controlword back out of SafeData octet 0.
///
/// An empty span decodes as "STO requested", which is the safe reading of an absent frame.
SdpControl sdpDecodeControl(std::span<const uint8_t> safeOutputs);

/// @brief Reads the safety statusword from SafeData octet 0.
///
/// An empty span decodes as "STO active", which is the safe reading of an absent frame.
SdpStatus sdpDecodeStatus(std::span<const uint8_t> safeInputs);

/// @brief Reads the safe process values from a SafeInputs span.
///
/// Fail-safe: a span shorter than @c kSdpSafeInputsWithProcessValues decodes to all zero and all
/// invalid, rather than to a partially decoded number. A truncated safe position read as a real
/// one is worse than no position at all.
SdpProcessValues sdpDecodeProcessValues(std::span<const uint8_t> safeInputs);

/// @brief Converts the 8.24 fixed-point safe position to revolutions of the output shaft.
///
/// For display and for a client that wants a number rather than a scaling rule. The fixed-point
/// value is what the drive sends; this is the same quantity, and no more precise.
constexpr double sdpPositionRevolutions(int32_t positionFixedPoint) {
  return static_cast<double>(positionFixedPoint) / static_cast<double>(1 << 24);
}

}  // namespace mm::etg
