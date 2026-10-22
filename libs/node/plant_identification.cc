#include "node/plant_identification.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <numbers>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node {

namespace {

constexpr double kRpmToRadPerSecond = 2.0 * std::numbers::pi / 60.0;

double meanAbsolute(std::span<const double> values) {
  if (values.empty()) {
    return 0.0;
  }
  const double sum = std::transform_reduce(values.begin(), values.end(), 0.0, std::plus<>{},
                                           [](double value) { return std::abs(value); });
  return sum / static_cast<double>(values.size());
}

std::string joinRow(std::span<const double> values) {
  std::string row;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      row += ',';
    }
    row += std::format("{}", values[i]);
  }
  return row;
}

// One CSV row as numbers. A field that is not a number names itself in the error.
std::expected<std::vector<double>, std::string> parseRow(std::string_view row, int rowNumber) {
  std::vector<double> values;
  size_t start = 0;
  while (start <= row.size()) {
    const size_t comma = row.find(',', start);
    std::string_view field =
        row.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) {
      field.remove_prefix(1);
    }
    while (!field.empty() &&
           (field.back() == ' ' || field.back() == '\t' || field.back() == '\r')) {
      field.remove_suffix(1);
    }
    // The JSON parser rather than std::from_chars, which libc++ does not offer for a double
    // before macOS 26. Every form a writer of this file produces is a JSON number.
    const auto value = nlohmann::json::parse(field, nullptr, false);
    if (!value.is_number()) {
      return std::unexpected(std::format("row {} field {} is not a number: '{}'", rowNumber,
                                         values.size() + 1, field));
    }
    values.push_back(value.get<double>());
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return values;
}

}  // namespace

void to_json(nlohmann::json& j, const PlantModelChirp& chirp) {
  j = nlohmann::json{
      {"startFrequencyHz", chirp.startFrequencyHz},        {"endFrequencyHz", chirp.endFrequencyHz},
      {"amplitudePermil", chirp.amplitudePermil},          {"durationMs", chirp.durationMs},
      {"signalType", somanet::toString(chirp.signalType)},
  };
}

std::optional<double> PlantModel::inertia() const {
  if (denominator.size() < 2) {
    return std::nullopt;
  }
  return denominator[denominator.size() - 2];
}

std::optional<double> PlantModel::friction() const {
  if (denominator.empty()) {
    return std::nullopt;
  }
  return denominator.back();
}

void to_json(nlohmann::json& j, const PlantModel& model) {
  j = nlohmann::json{
      {"numerator", model.numerator},
      {"denominator", model.denominator},
      {"inertia", model.inertia() ? nlohmann::json(*model.inertia()) : nlohmann::json()},
      {"friction", model.friction() ? nlohmann::json(*model.friction()) : nlohmann::json()},
      {"chirp", model.chirp},
  };
}

std::expected<std::chrono::milliseconds, std::string> plantIdentificationStreamDuration(
    std::chrono::milliseconds transitionTime) {
  const auto limit = somanet::maxHrdStreamDuration(somanet::HrdData::kSystemIdentificationData);
  const auto duration = transitionTime + kChirpPaddingMs;
  if (duration > limit) {
    return std::unexpected(std::format(
        "a chirp of {} ms plus {} ms of padding does not fit in the {} ms the drive records; the "
        "longest chirp is {} ms",
        transitionTime.count(), kChirpPaddingMs.count(), limit.count(),
        (limit - kChirpPaddingMs).count()));
  }
  return duration;
}

PlantMeasurement convertPlantMeasurement(std::span<const HrdSystemIdentificationSample> samples,
                                         uint32_t ratedTorqueMilliNm) {
  PlantMeasurement measurement;
  measurement.torquesNm.reserve(samples.size());
  measurement.velocitiesRadPerSecond.reserve(samples.size());
  const double permilToNm = static_cast<double>(ratedTorqueMilliNm) / 1'000'000.0;
  for (const auto& sample : samples) {
    measurement.torquesNm.push_back(static_cast<double>(sample.torquePermil) * permilToNm);
    measurement.velocitiesRadPerSecond.push_back(sample.velocityRpm * kRpmToRadPerSecond);
  }
  return measurement;
}

std::expected<std::optional<std::string>, std::string> checkPlantMeasurementAmplitudes(
    const PlantMeasurement& measurement, uint32_t ratedTorqueMilliNm) {
  const double meanTorqueNm = meanAbsolute(measurement.torquesNm);
  const double meanTorquePermil =
      ratedTorqueMilliNm == 0 ? 0.0 : meanTorqueNm * 1'000'000.0 / ratedTorqueMilliNm;
  if (meanTorquePermil < kMinMeanAbsTorquePermil) {
    return std::unexpected(std::format(
        "the recorded torque is too small to identify a plant: its mean absolute value is {:.1f} "
        "per mille of rated torque, and at least {:.0f} is needed; the chirp did not run",
        meanTorquePermil, kMinMeanAbsTorquePermil));
  }
  const double meanVelocity = meanAbsolute(measurement.velocitiesRadPerSecond);
  if (meanVelocity < kMinMeanAbsVelocityRadPerSecond) {
    return std::unexpected(std::format(
        "the motor did not move enough to identify a plant: the mean absolute velocity is {:.2f} "
        "rad/s, and at least {:.1f} rad/s is needed; raise the amplitude, and check that the "
        "shaft is free to turn",
        meanVelocity, kMinMeanAbsVelocityRadPerSecond));
  }
  if (meanVelocity < kLowMeanAbsVelocityRadPerSecond) {
    return std::optional<std::string>(std::format(
        "the motion was small: the mean absolute velocity is {:.2f} rad/s; a higher amplitude "
        "would give a better model",
        meanVelocity));
  }
  return std::optional<std::string>();
}

bool isStablePlantModel(std::span<const double> denominator) {
  return std::ranges::none_of(denominator, [](double coefficient) { return coefficient < 0.0; });
}

std::string plantModelCsv(const PlantModel& model) {
  return std::format("{}\n{}\n{},{},{},{},{}\n", joinRow(model.numerator),
                     joinRow(model.denominator), model.chirp.startFrequencyHz,
                     model.chirp.endFrequencyHz, model.chirp.amplitudePermil,
                     static_cast<double>(model.chirp.durationMs) / 1000.0,
                     static_cast<unsigned>(model.chirp.signalType));
}

std::expected<PlantModel, std::string> parsePlantModelCsv(std::string_view text) {
  std::vector<std::string_view> rows;
  size_t start = 0;
  while (start < text.size()) {
    const size_t newline = text.find('\n', start);
    rows.push_back(text.substr(
        start, newline == std::string_view::npos ? std::string_view::npos : newline - start));
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
  if (rows.size() < 2) {
    return std::unexpected(std::format("{} needs a numerator row and a denominator row; found {}",
                                       kPlantModelFile, rows.size()));
  }
  auto numerator = parseRow(rows[0], 1);
  if (!numerator) {
    return std::unexpected(numerator.error());
  }
  auto denominator = parseRow(rows[1], 2);
  if (!denominator) {
    return std::unexpected(denominator.error());
  }
  PlantModel model;
  model.numerator = std::move(*numerator);
  model.denominator = std::move(*denominator);
  if (rows.size() < 3) {
    return model;
  }
  auto chirp = parseRow(rows[2], 3);
  if (!chirp) {
    return std::unexpected(chirp.error());
  }
  if (chirp->size() != 5) {
    return std::unexpected(
        std::format("row 3 holds the chirp as five values; found {}", chirp->size()));
  }
  const double signalType = (*chirp)[4];
  if (signalType != static_cast<double>(somanet::ChirpSignalType::kLogarithmic) &&
      signalType != static_cast<double>(somanet::ChirpSignalType::kLinear)) {
    return std::unexpected(std::format("row 3 names an unknown signal type: {}", signalType));
  }
  model.chirp.startFrequencyHz = (*chirp)[0];
  model.chirp.endFrequencyHz = (*chirp)[1];
  model.chirp.amplitudePermil = static_cast<uint32_t>(std::lround((*chirp)[2]));
  model.chirp.durationMs = static_cast<uint32_t>(std::lround((*chirp)[3] * 1000.0));
  model.chirp.signalType = static_cast<somanet::ChirpSignalType>(signalType);
  return model;
}

}  // namespace mm::node
