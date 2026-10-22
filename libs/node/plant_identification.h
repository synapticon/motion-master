#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "node/somanet_drive.h"

/// @file
/// @brief Plant identification: the model of a motor and its load that auto-tuning computes gains
///        from, and the pure functions around its measurement, its fit and its file.
///
/// The fit itself runs in the auto-tuning program, and the client that talks to it lives outside
/// @c mm::node. So this header names only the shape of that call, @c PlantModelFitFn. The
/// composition root supplies the function, and the procedure body calls it without knowing what
/// stands behind it. Everything else here is a pure function: a transform from one value to
/// another, with no device involved, which is what makes it testable without one.

namespace mm::node {

/// @brief The recorded response of a drive to a chirp, in the units the fit takes.
///
/// Both vectors hold one sample per millisecond, the rate of the drive's HRD stream, and are the
/// same length.
struct PlantMeasurement {
  std::vector<double> torquesNm;               ///< Actual torque, in Nm.
  std::vector<double> velocitiesRadPerSecond;  ///< Velocity of the velocity-control encoder.
};

/// @brief A plant model as the fit returns it: a transfer function from torque to velocity.
struct PlantModelFit {
  std::vector<double> numerator;    ///< Numerator coefficients, highest order first.
  std::vector<double> denominator;  ///< Denominator coefficients, highest order first.
  /// Bode data of the model, keyed by column name, as the auto-tuning program renders it. Carried
  /// through to the procedure result untouched, because a client plots it and nothing here reads
  /// it.
  nlohmann::json bode;
};

/// @brief Fits a plant model to a measurement.
///
/// @param measurement        The recorded response.
/// @param startFrequencyHz   Where the chirp began.
/// @param endFrequencyHz     Where it ended.
/// @return The model, or why the fit was refused. A missing auto-tuning program is one such
///         reason, worded so that a user knows what to install.
using PlantModelFitFn = std::function<std::expected<PlantModelFit, std::string>(
    const PlantMeasurement& measurement, double startFrequencyHz, double endFrequencyHz)>;

/// @brief The file on the drive that holds the identified plant model.
inline constexpr std::string_view kPlantModelFile = "plant_model.csv";

/// @brief Recording time added around the chirp.
///
/// The drive centres the chirp in the HRD stream and splits the rest of the stream time before
/// and after it, so this is 100 ms of rest on each side. It is what lets the fit see the plant at
/// rest before the excitation starts.
inline constexpr std::chrono::milliseconds kChirpPaddingMs{200};

/// @brief Mean absolute torque, in per mille of rated torque, below which a recording is refused.
inline constexpr double kMinMeanAbsTorquePermil = 10.0;

/// @brief Mean absolute velocity, in rad/s, below which a recording is refused.
inline constexpr double kMinMeanAbsVelocityRadPerSecond = 0.5;

/// @brief Mean absolute velocity, in rad/s, below which a recording is accepted with a warning,
///        because a higher amplitude would give a better model.
inline constexpr double kLowMeanAbsVelocityRadPerSecond = 6.0;

/// @brief The chirp a plant model was identified with, stored beside the model.
struct PlantModelChirp {
  double startFrequencyHz = 0.0;  ///< Where the sweep began.
  double endFrequencyHz = 0.0;    ///< Where it ended.
  uint32_t amplitudePermil = 0;   ///< Peak excitation, per mille of rated torque.
  uint32_t durationMs = 0;        ///< How long the sweep took.
  somanet::ChirpSignalType signalType{somanet::ChirpSignalType::kLogarithmic};
};
void to_json(nlohmann::json& j, const PlantModelChirp& chirp);

/// @brief An identified plant model: the transfer function from torque in Nm to velocity in rad/s,
///        and the chirp it came from.
///
/// The coefficients are ordered highest power first, and the auto-tuning program normalises them
/// so that the numerator's last coefficient is 1. The last two denominator coefficients are then
/// the effective inertia and the effective viscous friction of the reduced model, which is how the
/// program reads them when it computes gains.
struct PlantModel {
  std::vector<double> numerator;    ///< Numerator coefficients, highest order first.
  std::vector<double> denominator;  ///< Denominator coefficients, highest order first.
  PlantModelChirp chirp;            ///< What excited the plant.

  /// @brief The effective inertia: the denominator coefficient before the last.
  /// @return Nothing when the denominator has fewer than two coefficients.
  std::optional<double> inertia() const;

  /// @brief The effective viscous friction: the last denominator coefficient.
  /// @return Nothing when the denominator is empty.
  std::optional<double> friction() const;
};
void to_json(nlohmann::json& j, const PlantModel& model);

/// @brief How long the HRD stream must run for a chirp of @p transitionTime.
///
/// The chirp plus @c kChirpPaddingMs, when that fits what the drive records for system
/// identification data. A chirp that does not fit is refused here, before anything moves, with
/// the longest duration that would.
std::expected<std::chrono::milliseconds, std::string> plantIdentificationStreamDuration(
    std::chrono::milliseconds transitionTime);

/// @brief Converts a recording into the units the fit takes.
///
/// Velocity from rpm to rad/s, in the frame of the velocity-control encoder the drive recorded,
/// and torque from per mille of rated torque to Nm with @p ratedTorqueMilliNm, the value of
/// 0x6076.
PlantMeasurement convertPlantMeasurement(std::span<const HrdSystemIdentificationSample> samples,
                                         uint32_t ratedTorqueMilliNm);

/// @brief Checks that the recording excited the plant enough for a fit.
///
/// Refuses a recording whose mean absolute torque is below @c kMinMeanAbsTorquePermil, which means
/// the chirp never ran, or whose mean absolute velocity is below
/// @c kMinMeanAbsVelocityRadPerSecond, which means the motor did not move. Accepts one whose mean
/// absolute velocity is below @c kLowMeanAbsVelocityRadPerSecond with a warning, because the fit
/// works and a higher amplitude would improve it.
///
/// @param measurement          The converted recording.
/// @param ratedTorqueMilliNm   The value of 0x6076, to express the torque threshold in Nm.
/// @return A warning to carry in the result, nothing when there is none, or why the recording is
///         refused.
std::expected<std::optional<std::string>, std::string> checkPlantMeasurementAmplitudes(
    const PlantMeasurement& measurement, uint32_t ratedTorqueMilliNm);

/// @brief Whether a model with this denominator is stable.
///
/// A necessary condition of the Routh–Hurwitz criterion: a denominator with a negative coefficient
/// has a pole in the right half-plane, and a controller tuned on that model is unusable. A model
/// that passes may still be unstable in theory; the program's fits do not produce such models in
/// practice.
bool isStablePlantModel(std::span<const double> denominator);

/// @brief Renders a model as the text of @c kPlantModelFile.
///
/// Three comma-separated rows: the numerator, the denominator, and the chirp as start frequency
/// in Hz, end frequency in Hz, amplitude in per mille, duration in seconds, and signal type as its
/// number. Values are written with every digit that reads back to the same double.
std::string plantModelCsv(const PlantModel& model);

/// @brief Reads the text of @c kPlantModelFile.
///
/// Tolerant of what older writers produced: a value in exponent notation, a missing third row, and
/// a trailing newline or none. The first two rows are required and must hold at least one number
/// each.
std::expected<PlantModel, std::string> parsePlantModelCsv(std::string_view text);

}  // namespace mm::node
