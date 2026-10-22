#pragma once

#include <expected>
#include <filesystem>
#include <string>

/// @file
/// @brief Runs the capture program for the `POST /api/captures` route.

namespace mm {

/// @brief The file name of the capture program beside the server binary, with the platform's
///        executable suffix.
std::filesystem::path captureBinaryName();

/// @brief Runs @p binary to record @p adapter for @p seconds into a new file under @p dir, and
///        waits for it to exit.
///
/// The file is named after the time and the adapter. The program's own output goes to a `.log`
/// beside it, which is removed when the program succeeds, so a failure leaves what it said. A
/// program that runs past its time by more than ten seconds is killed.
///
/// @return The path of the pcap file, or an error string: a missing program, a refused adapter,
///         or a non-zero exit, with the program's last output line when it has one.
std::expected<std::filesystem::path, std::string> runCapture(const std::filesystem::path& binary,
                                                             const std::string& adapter,
                                                             int seconds,
                                                             const std::filesystem::path& dir);

}  // namespace mm
