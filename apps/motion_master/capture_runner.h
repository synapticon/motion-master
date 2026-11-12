#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "core/child_process.h"

/// @file
/// @brief Runs the capture program for the `/api/captures` routes, one capture at a time.

namespace mm {

/// @brief The file name of the capture program beside the server binary, with the platform's
///        executable suffix.
std::filesystem::path captureBinaryName();

/// @brief Why a capture could not run. The route answers each kind with its own status.
struct CaptureError {
  enum class Kind : uint8_t {
    kBusy,          ///< Another capture runs. 409.
    kNotRunning,    ///< A stop with no started capture. 409.
    kNotSupported,  ///< A started capture on a platform with no way to stop it cleanly. 501.
    kFailed,        ///< The program is missing, refused the adapter, or failed. 500.
  };
  Kind kind = Kind::kFailed;
  std::string message;
};

/// @brief The one capture the server runs: a capture of a fixed length, or a capture that is
///        started and later stopped.
///
/// One at a time, whichever kind: a second request while a capture runs gets @c kBusy. The
/// capture program runs as its own process on the adapter it is given, so nothing the device
/// manager does, a reset or a scan, ends it. Every file goes into the directory given at
/// construction, named after the time and the adapter. The program's own output goes to a `.log`
/// beside the file, which is removed when the capture succeeds, so a failure leaves what it said.
///
/// Thread-safe. The blocking @c run holds no lock while it waits.
class CaptureRunner {
 public:
  /// @param binary  The capture program.
  /// @param dir     The directory the files go into. Created when it is missing.
  CaptureRunner(std::filesystem::path binary, std::filesystem::path dir);

  /// @brief Stops a started capture, so no capture program outlives the server.
  ~CaptureRunner();

  CaptureRunner(const CaptureRunner&) = delete;
  CaptureRunner& operator=(const CaptureRunner&) = delete;

  /// @brief Records @p adapter for @p seconds and waits for the program to exit.
  ///
  /// A program that runs past its time by more than ten seconds is killed.
  ///
  /// @return The pcap file, or why there is none.
  std::expected<std::filesystem::path, CaptureError> run(const std::string& adapter, int seconds);

  /// @brief Starts a capture of @p adapter that runs until @c stop, and at most @p maxSeconds.
  ///
  /// The upper limit is for a client that never stops the capture: the program then ends by
  /// itself, and its file stays in the directory. A start after that drops the ended capture.
  ///
  /// Returns once the capture runs: the program creates its file when the adapter is open, and
  /// this waits up to 5 s for it. So a stop right after a start still gets a complete file, and a
  /// refused adapter is reported here.
  ///
  /// @return The pcap file the capture writes, or why it did not start. On Windows always
  ///         @c kNotSupported: Windows has no signal for a program without a console, and a kill
  ///         would leave the file cut off with no summary.
  std::expected<std::filesystem::path, CaptureError> start(const std::string& adapter,
                                                           int maxSeconds);

  /// @brief Ends the started capture and waits for the program to write its file.
  ///
  /// @return The pcap file, or why there is none.
  std::expected<std::filesystem::path, CaptureError> stop();

 private:
  struct Started {
    mm::core::Child child;
    std::filesystem::path file;
    std::filesystem::path log;
  };

  /// The paths of a new capture of @p adapter, or why the directory cannot hold one.
  std::expected<Started, CaptureError> prepare(const std::string& adapter) const;

  std::filesystem::path binary_;
  std::filesystem::path dir_;

  std::mutex mutex_;
  bool busy_ = false;  ///< A blocking run or a stop is in progress.
  std::optional<Started> started_;
};

}  // namespace mm
