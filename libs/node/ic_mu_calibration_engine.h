#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "node/ic_mu_calibration.h"
#include "node/somanet_drive.h"

struct MU_Calibration_;
struct MU_CalibrationAnalyzeResult_;

namespace mm::node::somanet {

/// @file
/// @brief The iC-Haus MU_3SL library behind one owning type. Built only where @c MM_HAVE_MU_3SL is
///        set, because iC-Haus builds the library for Linux x86_64 and Windows x64 only.

/// @brief What one analysis of a recording reports.
struct IcMuAnalysis {
  /// The error that remains on each track, in LSB of its registers, with the phase range of the
  /// adjustments the recording was made with.
  IcMuTrackResidual master;
  IcMuTrackResidual nonius;

  /// The revolutions of the ring the library found in the recording.
  double revolutions = 0;

  /// What the library reports about the recording itself: the master periods it calculated, and
  /// the fewest samples it saw in one master period. Reported, not judged: the library does not
  /// say which values it takes as sound.
  int calculatedMasterPeriods = 0;
  double minimalSamplesPerMasterPeriod = 0;

  /// Whether the nonius analysis is valid. The curves and the margins below are empty or 0
  /// otherwise.
  bool noniusValid = false;

  /// The nonius phase error and the nonius phase margin, one value per curve sample.
  std::vector<int64_t> phaseError;
  std::vector<int64_t> phaseMargin;

  int64_t phaseRangeLimit = 0;  ///< The permissible phase error, in the unit of the curves.
  double lowerMarginPercent = 0;
  double upperMarginPercent = 0;
};

/// @brief One iC-MU calibration in MU_3SL: the model of the encoder and the last analysis.
///
/// The model holds the adjustments and the nonius table the encoder holds. An analysis compares a
/// recording against the model; @c adjust then moves the model of each named track to the values
/// that remove the error, and returns them for the encoder.
///
/// Move-only. It frees the library objects on every path. Not thread-safe: one procedure body owns
/// one.
class IcMuCalibrationEngine {
 public:
  /// @brief A calibration for the part @p hardRev names, with @p masterPolePairs on the ring.
  static std::expected<IcMuCalibrationEngine, std::string> create(uint8_t hardRev,
                                                                  uint8_t masterPolePairs);

  IcMuCalibrationEngine(IcMuCalibrationEngine&&) noexcept;
  IcMuCalibrationEngine& operator=(IcMuCalibrationEngine&&) noexcept;
  IcMuCalibrationEngine(const IcMuCalibrationEngine&) = delete;
  IcMuCalibrationEngine& operator=(const IcMuCalibrationEngine&) = delete;
  ~IcMuCalibrationEngine();

  /// @brief Sets the model to the adjustments the encoder holds.
  std::expected<void, std::string> setAdjustments(const IcMuAnalogAdjustments& master,
                                                  const IcMuAnalogAdjustments& nonius);

  /// @brief Sets the model to the nonius table the encoder holds.
  std::expected<void, std::string> setNoniusTable(const IcMuNoniusOffsetTable& table);

  /// @brief The adjustments of @p track in the model.
  IcMuAnalogAdjustments adjustments(IcMuTrack track) const;

  /// @brief Analyses a recording and keeps the result for @c adjust and @c optimizeNoniusTable.
  ///
  /// Fails with the log of the library when it rejects the analog analysis.
  std::expected<IcMuAnalysis, std::string> analyze(std::span<const HrdEncoderSample> samples);

  /// @brief Moves the model of the named tracks to the adjustments the last analysis asks for, and
  ///        keeps the other track as it was.
  ///
  /// Fails with the message of the library when it cannot apply the analysis, for example because
  /// a correction is out of the range of a register.
  std::expected<void, std::string> adjust(bool master, bool nonius);

  /// @brief Sets the model to the nonius table the last analysis optimised, and returns it.
  std::expected<IcMuNoniusOffsetTable, std::string> optimizeNoniusTable();

 private:
  struct CalibrationDeleter {
    void operator()(MU_Calibration_* calibration) const;
  };
  struct ResultDeleter {
    void operator()(MU_CalibrationAnalyzeResult_* result) const;
  };

  explicit IcMuCalibrationEngine(MU_Calibration_* calibration);

  std::unique_ptr<MU_Calibration_, CalibrationDeleter> calibration_;
  std::unique_ptr<MU_CalibrationAnalyzeResult_, ResultDeleter> result_;
};

}  // namespace mm::node::somanet
