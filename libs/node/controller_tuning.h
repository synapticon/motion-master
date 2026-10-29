#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "node/device_parameter.h"
#include "node/somanet_drive.h"

/// @file
/// @brief Controller auto-tuning: the gains of the position or the velocity controller, computed
///        from the plant model, and the pure functions around that computation.
///
/// The computation runs in the auto-tuning program, and the client that talks to it lives outside
/// @c mm::node. So this header names only the shape of that call, @c ControllerTuningFn, as
/// @c plant_identification.h does for the fit. Everything else here is a pure function: the drive
/// configuration the program reads, the velocity noise, and the gains in the units the drive
/// takes.

namespace mm::node {

/// @brief The controller a tuning computes gains for.
enum class TunedController : uint8_t {
  kPosition,  ///< The cascaded position controller, 0x2012.
  kVelocity,  ///< The velocity controller, 0x2011.
};

constexpr std::string_view toString(TunedController controller) {
  return controller == TunedController::kPosition ? "position" : "velocity";
}

/// @brief The structure of the cascaded position controller that the gains are computed for.
///
/// The drive has one cascaded structure. The choice changes which loop the program gives the
/// integral action to, and so the gains it computes.
enum class CascadedControllerType : uint8_t {
  kPiP,  ///< "PI-P": PI position loop, P velocity loop.
  kPPi,  ///< "P-PI": P position loop, PI velocity loop.
};

/// @brief The name the auto-tuning program takes, "PI-P" or "P-PI".
constexpr std::string_view toString(CascadedControllerType type) {
  return type == CascadedControllerType::kPiP ? "PI-P" : "P-PI";
}

/// @brief The targets of a tuning that is not full: what the closed loop must do.
///
/// The ranges are the ones the program clips to (`sncn_auto_tuning_library.py`), so a value the
/// procedure accepts is the value the program uses.
struct ControllerTuningTargets {
  double dampingRatio = 1.0;   ///< 0.4 to 2.
  double settlingTimeS = 0.2;  ///< Position only. 0.02 s to 1 s.
  double bandwidthHz = 5.0;    ///< Velocity only. 0.2 Hz to 100 Hz.
};

/// @brief The drive configuration that the full functions read, in the program's units.
///
/// Only the fields the program reads for firmware 5 and newer. The program reads the phase
/// resistance and inductance, the DC link voltage, the current ratio, the switching frequency and
/// the torque controller gains only for firmware 4 (`build_torque_ctrl_tf` in
/// `sncn_full_auto_tuning_library.py`), and the procedure refuses firmware older than 5.1.0. A
/// field the program does not get keeps the program's default.
struct TuningDriveConfiguration {
  uint8_t velocityFilterType = 0;            ///< 0x2021:01. 0 off, 1 to 3 the filter order.
  uint32_t velocityFilterCutoffHz = 0;       ///< 0x2021:02.
  uint8_t positionFilterType = 0;            ///< 0x2022:01.
  uint32_t positionFilterCutoffHz = 0;       ///< 0x2022:02.
  bool notchEnabled = false;                 ///< 0x2023:01.
  uint16_t notchCenterHz = 0;                ///< 0x2023:02.
  uint16_t notchWidthHz = 0;                 ///< 0x2023:03.
  std::optional<uint16_t> notchDepthDb;      ///< 0x2023:04. Empty where the dictionary lacks it.
  int16_t velocityFeedForwardPermil = 0;     ///< 0x2015:01.
  uint16_t velocityFeedForwardCutoffHz = 0;  ///< 0x2015:02.
  uint16_t torqueSettlingTimeUs = 0;         ///< 0x2010:0A.
  uint16_t torqueDampingRatioPermil = 0;     ///< 0x2010:0B.
  std::string firmwareVersion;               ///< 0x100A, as the drive reports it.
  uint32_t ratedTorqueMilliNm = 0;           ///< 0x6076.
  double velocityNoiseRpm = 0.0;             ///< From @c velocityNoise.
};

/// @brief Renders @p configuration as the program's @c drive_config object.
void to_json(nlohmann::json& j, const TuningDriveConfiguration& configuration);

/// @brief One loop's gains.
struct LoopGains {
  double kp = 0.0;
  double ki = 0.0;
  double kd = 0.0;
};

/// @brief The gains the program returns, in SI units.
///
/// Position loop in (rad/s)/rad and (rad/s)/(rad·s), velocity loop in Nm/(rad/s) and
/// Nm/(rad/s·s), as the program's docstrings of `pos_ctrl_loop_evaluate_state` and
/// `vel_ctrl_loop_evaluate_state` state. A velocity tuning returns zeros for the position loop.
struct TunedGains {
  LoopGains position;
  LoopGains velocity;
};

/// @brief One call to the auto-tuning program.
struct ControllerTuningCall {
  TunedController controller = TunedController::kPosition;
  CascadedControllerType controllerType = CascadedControllerType::kPiP;  ///< Position only.
  std::vector<double> numerator;    ///< The plant model, highest order first.
  std::vector<double> denominator;  ///< The plant model, highest order first.
  /// The drive configuration for a full tuning. Empty for a tuning to @c targets.
  std::optional<TuningDriveConfiguration> driveConfiguration;
  ControllerTuningTargets targets;  ///< Read only when @c driveConfiguration is empty.
};

/// @brief What the program returns.
struct ControllerTuningReply {
  TunedGains gains;
  /// What the program tuned for. A full tuning chooses these; otherwise they are the targets.
  ControllerTuningTargets used;
  /// The stability margins and other figures a full tuning reports, as the program renders them.
  /// Carried to the result untouched; nothing here reads it.
  nlohmann::json state;
};

/// @brief Computes gains for one call.
///
/// @return The reply, or why there is none. A missing auto-tuning program is one such reason,
///         worded so that a user knows what to start.
using ControllerTuningFn =
    std::function<std::expected<ControllerTuningReply, std::string>(const ControllerTuningCall&)>;

/// @brief Why a drive with firmware @p version cannot be tuned, or nothing when it can.
///
/// Refuses firmware older than 5.1.0 and a version that does not parse. A leading "v" is allowed.
std::optional<std::string> controllerTuningFirmwareError(std::string_view version);

/// @brief Mean absolute velocity, in rpm, above which the noise recording is taken as a moving
///        motor.
inline constexpr double kNoiseMaxMeanAbsVelocityRpm = 4.0;

/// @brief The velocity noise of a motor at rest, for the program's @c max_vel_noise.
struct VelocityNoise {
  double noiseRpm = 0.0;               ///< 0 when @c warning is set.
  std::optional<std::string> warning;  ///< Why the noise is 0.
};

/// @brief Three standard deviations of the change in velocity from one sample to the next.
///
/// The velocity is the unfiltered velocity of the velocity-control encoder that the HRD stream of
/// system identification data records. A recording with fewer than three samples, or with a mean
/// absolute velocity above @c kNoiseMaxMeanAbsVelocityRpm, gives a noise of 0 and a warning,
/// because the program then tunes as if the encoder had no noise. The standard deviation is the
/// sample standard deviation.
VelocityNoise velocityNoise(std::span<const HrdSystemIdentificationSample> samples);

/// @brief One gain object and the value for it, in the drive's units.
struct GainWrite {
  ObjectAddress<float> address;
  float value = 0.0F;
};

/// @brief The gain writes of one tuning.
struct GainPlan {
  std::vector<GainWrite> writes;              ///< In the order they are written.
  std::vector<ObjectAddress<float>> skipped;  ///< Kd objects the dictionary does not have.
};

/// @brief Converts @p gains to the drive's units and lists the objects to write.
///
/// A position tuning writes 0x2012:01 to :03 and 0x2012:05 to :07. A velocity tuning writes
/// 0x2011:01 to :03. Kd is 0. The position loop gains are × 60 / @p positionEncoderResolution, to
/// rpm/inc and rpm/(inc·s). The velocity loop gains are × 2000π / 60, to mNm/rpm and
/// mNm/(rpm·s).
///
/// @param controller                 The tuned controller.
/// @param gains                      The gains in SI units.
/// @param positionEncoderResolution  Counts per revolution of the position encoder. Read only for
///                                   a position tuning.
/// @param hasObject                  Whether the drive's dictionary has an object. A Kd object it
///                                   does not have is skipped.
/// @return The plan, or why there is none: a missing Kp or Ki object, or a resolution of 0.
std::expected<GainPlan, std::string> controllerGainPlan(
    TunedController controller, const TunedGains& gains, uint32_t positionEncoderResolution,
    const std::function<bool(ObjectAddress<float>)>& hasObject);

}  // namespace mm::node
