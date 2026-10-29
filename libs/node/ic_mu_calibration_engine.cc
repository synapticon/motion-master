#include "node/ic_mu_calibration_engine.h"

#include <MU_3SL_interface.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mm::node::somanet {

namespace {

MU_Calibration_AnalogTrackAdjustments toLibrary(const IcMuAnalogAdjustments& adjustments) {
  return {.cosineGain = adjustments.gain,
          .sineOffset = adjustments.sineOffset,
          .cosineOffset = adjustments.cosineOffset,
          .phase = adjustments.phase.phase,
          .phaseRange = adjustments.phase.phaseRange};
}

IcMuAnalogAdjustments fromLibrary(const MU_Calibration_AnalogTrackAdjustments& adjustments) {
  return {.gain = adjustments.cosineGain,
          .sineOffset = adjustments.sineOffset,
          .cosineOffset = adjustments.cosineOffset,
          .phase = {.phase = adjustments.phase, .phaseRange = adjustments.phaseRange}};
}

IcMuTrackResidual residual(const MU_Calibration_RelativeAnalogTrackAdjustments& relative,
                           uint8_t phaseRange) {
  return {.sineOffset = relative.sineOffset_lsb,
          .cosineOffset = relative.cosineOffset_lsb,
          .gain = relative.cosineGain_lsb,
          .phase = relative.phase_lsb,
          .phaseRange = phaseRange};
}

// The library hands out its curves as C arrays of long.
// NOLINTNEXTLINE(runtime/int)
std::vector<int64_t> curve(const long* values, size_t count) {
  if (values == nullptr) {
    return {};
  }
  return {values, values + count};
}

// The text in @p buffer up to its first NUL, or the whole buffer. The library writes at most the
// buffer size, and its header does not say that a cut text still ends in a NUL.
template <size_t N>
std::string bufferText(const std::array<char, N>& buffer) {
  const std::string_view text(buffer.data(), buffer.size());
  return std::string(text.substr(0, text.find('\0')));
}

// The log of an analysis. The library writes at most the buffer size, so a long log is cut.
std::string analyzeLog(const MU_CalibrationAnalyzeResult* result) {
  std::array<char, 1024> buffer{};
  MU_Calibration_getAnalyzeResultLog(result, buffer.data(), buffer.size(),
                                     MU_CALIBRATION_ANALYZE_RESULT_LOG_ALL);
  return bufferText(buffer);
}

}  // namespace

void IcMuCalibrationEngine::CalibrationDeleter::operator()(MU_Calibration_* calibration) const {
  MU_Calibration_delete(calibration);
}

void IcMuCalibrationEngine::ResultDeleter::operator()(MU_CalibrationAnalyzeResult_* result) const {
  MU_CalibrationAnalyzeResult_delete(result);
}

IcMuCalibrationEngine::IcMuCalibrationEngine(MU_Calibration_* calibration)
    : calibration_(calibration) {}

IcMuCalibrationEngine::IcMuCalibrationEngine(IcMuCalibrationEngine&&) noexcept = default;
IcMuCalibrationEngine& IcMuCalibrationEngine::operator=(IcMuCalibrationEngine&&) noexcept = default;
IcMuCalibrationEngine::~IcMuCalibrationEngine() = default;

std::expected<IcMuCalibrationEngine, std::string> IcMuCalibrationEngine::create(
    uint8_t hardRev, uint8_t masterPolePairs) {
  auto* calibration = MU_createCalibration(hardRev);
  if (calibration == nullptr) {
    return std::unexpected(
        std::format("MU_3SL has no calibration for the part with HARD_REV 0x{:02X}", hardRev));
  }
  IcMuCalibrationEngine engine(calibration);
  if (!MU_Calibration_preconfigureNumberOfMasterPeriods(calibration, masterPolePairs)) {
    return std::unexpected(
        std::format("MU_3SL refused {} master periods per revolution", masterPolePairs));
  }
  return engine;
}

std::expected<void, std::string> IcMuCalibrationEngine::setAdjustments(
    const IcMuAnalogAdjustments& master, const IcMuAnalogAdjustments& nonius) {
  const auto m = toLibrary(master);
  const auto n = toLibrary(nonius);
  if (!MU_Calibration_setCurrentAnalogTrackAdjustments(calibration_.get(), &m, &n)) {
    return std::unexpected("MU_3SL refused the analog adjustments the encoder holds");
  }
  return {};
}

std::expected<void, std::string> IcMuCalibrationEngine::setNoniusTable(
    const IcMuNoniusOffsetTable& table) {
  MU_NoniusTrackOffsetTableParameters parameters{};
  parameters.spoBase = table.base;
  std::ranges::copy(table.offsets, std::begin(parameters.spoN));
  MU_Calibration_NoniusTrackOffsetTable libraryTable{};
  MU_getNoniusTrackOffsetTableByParameters(&parameters, &libraryTable);
  if (!MU_Calibration_setCurrentNoniusTrackOffsetTable(calibration_.get(), &libraryTable)) {
    return std::unexpected("MU_3SL refused the nonius table the encoder holds");
  }
  return {};
}

IcMuAnalogAdjustments IcMuCalibrationEngine::adjustments(IcMuTrack track) const {
  MU_Calibration_AnalogTrackAdjustments adjustments{};
  if (track == IcMuTrack::kMaster) {
    MU_Calibration_getAnalogMasterTrackAdjustments(calibration_.get(), &adjustments);
  } else {
    MU_Calibration_getAnalogNoniusTrackAdjustments(calibration_.get(), &adjustments);
  }
  return fromLibrary(adjustments);
}

std::expected<IcMuAnalysis, std::string> IcMuCalibrationEngine::analyze(
    std::span<const HrdEncoderSample> samples) {
  std::vector<uint16_t> master(samples.size());
  std::vector<uint16_t> nonius(samples.size());
  std::ranges::transform(samples, master.begin(), &HrdEncoderSample::masterCount);
  std::ranges::transform(samples, nonius.begin(), &HrdEncoderSample::noniusCount);

  result_.reset(MU_Calibration_analyzeRawData(calibration_.get(), master.data(), nonius.data(),
                                              samples.size()));
  if (!result_) {
    return std::unexpected("MU_3SL could not analyse the recording");
  }
  if (!MU_Calibration_isAnalogAnalysesValid(result_.get())) {
    auto log = analyzeLog(result_.get());
    result_.reset();
    return std::unexpected(std::format("MU_3SL rejected the recording: {}", log));
  }

  MU_Calibration_RelativeAnalogTrackAdjustments relativeMaster{};
  MU_Calibration_RelativeAnalogTrackAdjustments relativeNonius{};
  MU_Calibration_getRelativeMasterTrackAdjustments(result_.get(), &relativeMaster);
  MU_Calibration_getRelativeNoniusTrackAdjustments(result_.get(), &relativeNonius);

  IcMuAnalysis analysis;
  analysis.master = residual(relativeMaster, adjustments(IcMuTrack::kMaster).phase.phaseRange);
  analysis.nonius = residual(relativeNonius, adjustments(IcMuTrack::kNonius).phase.phaseRange);
  analysis.revolutions = MU_Calibration_numberOfRevolutions(result_.get());
  analysis.calculatedMasterPeriods = MU_Calibration_numberOfCalculatedMasterPeriods(result_.get());
  analysis.minimalSamplesPerMasterPeriod =
      MU_Calibration_minimalNumberOfSamplesPerMasterPeriod(result_.get());
  analysis.noniusValid = MU_Calibration_isNoniusAnalysesValid(result_.get());
  if (analysis.noniusValid) {
    const size_t count = MU_Calibration_numberOfNoniusCurveSamples(result_.get());
    analysis.phaseError = curve(MU_Calibration_noniusPhaseError(result_.get()), count);
    analysis.phaseMargin = curve(MU_Calibration_noniusPhaseMargin(result_.get()), count);
    analysis.phaseRangeLimit = MU_Calibration_noniusPhaseRangeLimit(result_.get());
    analysis.lowerMarginPercent = MU_Calibration_noniusLowerPhaseMargin(result_.get());
    analysis.upperMarginPercent = MU_Calibration_noniusUpperPhaseMargin(result_.get());
  }
  return analysis;
}

std::expected<void, std::string> IcMuCalibrationEngine::adjust(bool master, bool nonius) {
  if (!result_) {
    return std::unexpected("no analysis to adjust by");
  }
  std::array<char, 1024> message{};
  if (!MU_Calibration_isAnalogAnalyzeResultAdjustable(calibration_.get(), result_.get(),
                                                      message.data(), message.size(),
                                                      MU_ADJUSTMENT_MESSAGE_LOG_ALL)) {
    return std::unexpected(
        std::format("MU_3SL cannot apply the analysis: {}", bufferText(message)));
  }
  // The library moves both tracks. A track that is not named keeps what the encoder holds, so the
  // model and the encoder stay the same.
  const auto masterBefore = adjustments(IcMuTrack::kMaster);
  const auto noniusBefore = adjustments(IcMuTrack::kNonius);
  if (!MU_Calibration_adjustAnalogByAnalyzeResult(calibration_.get(), result_.get())) {
    return std::unexpected("MU_3SL could not apply the analysis");
  }
  return setAdjustments(master ? adjustments(IcMuTrack::kMaster) : masterBefore,
                        nonius ? adjustments(IcMuTrack::kNonius) : noniusBefore);
}

std::expected<IcMuNoniusOffsetTable, std::string> IcMuCalibrationEngine::optimizeNoniusTable() {
  if (!result_ || !MU_Calibration_isNoniusAnalysesValid(result_.get())) {
    return std::unexpected("MU_3SL has no valid nonius analysis to optimise the table from");
  }
  MU_Calibration_NoniusTrackOffsetTable libraryTable{};
  MU_Calibration_getOptimizedNoniusTrackOffsetTable(result_.get(), &libraryTable);
  if (!MU_Calibration_setCurrentNoniusTrackOffsetTable(calibration_.get(), &libraryTable)) {
    return std::unexpected("MU_3SL refused the optimised nonius table");
  }
  MU_NoniusTrackOffsetTableParameters parameters{};
  MU_getNoniusTrackOffsetTableParameters(&libraryTable, &parameters);
  // Each value is a nibble of 0x52-0x59. Packing a larger one would cut it without a word.
  constexpr uint8_t kMaxNibble = 0x0F;
  if (parameters.spoBase > kMaxNibble ||
      std::ranges::any_of(parameters.spoN, [](uint8_t v) { return v > kMaxNibble; })) {
    return std::unexpected("MU_3SL optimised a nonius table with a value above 15");
  }
  IcMuNoniusOffsetTable table;
  table.base = parameters.spoBase;
  std::ranges::copy(parameters.spoN, table.offsets.begin());
  return table;
}

}  // namespace mm::node::somanet
