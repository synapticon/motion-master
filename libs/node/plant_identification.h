#pragma once

#include <expected>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

/// @file
/// @brief Plant identification: the model of a motor and its load that auto-tuning computes gains
///        from, and the door through which the fit reaches a procedure.
///
/// The fit itself runs in the auto-tuning program, and the client that talks to it lives outside
/// @c mm::node. So this header names only the shape of the call. The composition root supplies the
/// function, and the procedure body calls it without knowing what stands behind it.

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

}  // namespace mm::node
