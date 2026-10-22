#pragma once

#include <expected>
#include <optional>
#include <string>

#include "auto_tuning/client.h"
#include "node/plant_identification.h"

/// @file
/// @brief The plant model fit as the auto-tuning program offers it, in the shape the procedure
///        registry takes.
///
/// Everything that knows the program's request and reply formats for `identify_plant_model` is
/// here, so that @c mm::node knows neither. The three pure functions are the testable parts. The
/// fourth binds them to a client, or to the absence of one.

namespace mm {

/// @brief Renders a measurement as the CSV `identify_plant_model` reads.
///
/// Three columns and no header: time in seconds, torque in Nm, velocity in rad/s. The program
/// assumes 1000 Hz, so the time column is the sample index divided by 1000.
std::string plantMeasurementCsv(const node::PlantMeasurement& measurement);

/// @brief Builds the request body of one `identify_plant_model` call.
std::string identifyPlantModelRequest(const node::PlantMeasurement& measurement,
                                      double startFrequencyHz, double endFrequencyHz);

/// @brief Reads the program's reply.
///
/// The program answers a refused input with status 200 and an @c error property, so both the
/// status and the body decide. A reply without the two coefficient arrays is refused too, because
/// a model with one of them missing cannot be checked for stability or written to the drive.
std::expected<node::PlantModelFit, std::string> parseIdentifyPlantModelReply(
    int status, const std::string& body);

/// @brief The fit callback for the procedure registry.
///
/// @param client  The running auto-tuning program, or nothing when none runs. Borrowed: the
///                optional must outlive the returned function. Without a client, the function
///                refuses every call with a message that names the missing program.
node::PlantModelFitFn makePlantModelFit(const std::optional<auto_tuning::Client>& client);

}  // namespace mm
