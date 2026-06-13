#ifndef LIBS_CORE_SYSTEM_INFO_H_
#define LIBS_CORE_SYSTEM_INFO_H_

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace mm::core {

// A snapshot of the host OS and hardware Motion Master is running on. Collected on demand for the
// Connection page's "System" panel — cheap, uncached. Every field is best-effort: one that cannot
// be determined on the current platform is left empty (or zero) rather than failing the snapshot.
struct SystemInfo {
  std::string osName;        // friendly OS name, e.g. "Ubuntu 24.04 LTS", "Windows", "macOS 14.5"
  std::string kernel;        // kernel/OS build, e.g. "6.8.0-31-generic", "10.0.22631"
  std::string architecture;  // machine architecture, e.g. "x86_64", "aarch64"
  std::string hostname;      // network host name
  std::string cpuModel;      // CPU brand string, e.g. "Intel(R) Core(TM) i7-1185G7"
  unsigned cpuCores = 0;     // logical processor count
  uint64_t totalMemoryBytes = 0;  // total physical RAM in bytes (0 if unknown)
  uint64_t diskTotalBytes = 0;    // capacity of the filesystem holding the working directory
  uint64_t diskFreeBytes = 0;     // space available to an unprivileged process on that filesystem
  std::string container;      // container runtime when running in one ("docker"/"podman"), else ""
  std::string dockerVersion;  // host Docker version from `docker --version`, else ""
};

// Collects a SystemInfo snapshot for the current host. Never throws; unknown fields stay
// empty/zero.
SystemInfo collectSystemInfo();

// Serialises a snapshot to the JSON shape served by GET /api/system-info (nlohmann ADL hook).
void to_json(nlohmann::json& j, const SystemInfo& info);

}  // namespace mm::core

#endif  // LIBS_CORE_SYSTEM_INFO_H_
