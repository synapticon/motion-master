#include "plant_model_fit.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mm {

namespace {

// How long one fit may take. The procedure thread is not real-time, so the program's numerical
// workers get the cores to themselves and a fit of a six-second recording completes in seconds.
// The limit only fires when the program stops answering.
constexpr std::chrono::seconds kFitTimeout{120};

constexpr const char* kProgramNotRunning =
    "the auto-tuning program is not running; GET /api/auto-tuning says why";

}  // namespace

std::string plantMeasurementCsv(const node::PlantMeasurement& measurement) {
  std::string csv;
  const size_t count = measurement.velocitiesRadPerSecond.size();
  csv.reserve(count * 48);
  for (size_t i = 0; i < count; ++i) {
    csv += std::format("{},{},{}\n", static_cast<double>(i) / 1000.0, measurement.torquesNm[i],
                       measurement.velocitiesRadPerSecond[i]);
  }
  return csv;
}

std::string identifyPlantModelRequest(const node::PlantMeasurement& measurement,
                                      double startFrequencyHz, double endFrequencyHz) {
  const nlohmann::json request = {
      {"run", "identify_plant_model"},
      {"data",
       {{"csv", plantMeasurementCsv(measurement)},
        {"f0", startFrequencyHz},
        {"f1", endFrequencyHz}}},
  };
  return request.dump();
}

std::expected<node::PlantModelFit, std::string> parseIdentifyPlantModelReply(
    int status, const std::string& body) {
  const auto reply = nlohmann::json::parse(body, nullptr, false);
  if (status != 200) {
    return std::unexpected(
        std::format("the auto-tuning program answered {}: {}", status, body.substr(0, 200)));
  }
  if (reply.is_discarded() || !reply.is_object()) {
    return std::unexpected("the auto-tuning program answered with something other than JSON");
  }
  if (reply.contains("error")) {
    const auto& error = reply.at("error");
    return std::unexpected(
        std::format("the auto-tuning program refused the measurement: {}",
                    error.is_string() ? error.get<std::string>() : error.dump()));
  }
  const auto numerators = reply.find("numerators");
  const auto denominators = reply.find("denominators");
  if (numerators == reply.end() || !numerators->is_array() || denominators == reply.end() ||
      !denominators->is_array()) {
    return std::unexpected(
        "the auto-tuning program answered without the model's numerators and denominators");
  }
  node::PlantModelFit fit;
  fit.numerator = numerators->get<std::vector<double>>();
  fit.denominator = denominators->get<std::vector<double>>();
  fit.bode = reply.value("bode", nlohmann::json::object());
  return fit;
}

node::PlantModelFitFn makePlantModelFit(const std::optional<auto_tuning::Client>& client) {
  return [&client](const node::PlantMeasurement& measurement, double startFrequencyHz,
                   double endFrequencyHz) -> std::expected<node::PlantModelFit, std::string> {
    if (!client) {
      return std::unexpected(kProgramNotRunning);
    }
    auto reply = client->run(
        identifyPlantModelRequest(measurement, startFrequencyHz, endFrequencyHz), kFitTimeout);
    if (!reply) {
      return std::unexpected(reply.error());
    }
    return parseIdentifyPlantModelReply(reply->status, reply->body);
  };
}

}  // namespace mm
