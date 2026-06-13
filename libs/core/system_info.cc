#include "core/system_info.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
// clang-format on
#include <cstdlib>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <cstddef>
#else  // Linux and other POSIX
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace mm::core {
namespace {

// Fills @p out with the capacity and unprivileged-available space of the filesystem holding the
// current working directory (where the binary runs from). Best-effort: leaves both zero on error.
void collectDisk(SystemInfo& out) {
  std::error_code ec;
  const auto path = std::filesystem::current_path(ec);
  if (ec) {
    return;
  }
  const auto space = std::filesystem::space(path, ec);
  if (ec) {
    return;
  }
  out.diskTotalBytes = space.capacity;
  out.diskFreeBytes = space.available;
}

// Detects whether the process is running inside a container and which runtime, via the well-known
// marker files and the init process's cgroup hints. Returns "" on bare metal (the markers simply do
// not exist there, including on Windows/macOS). The cgroup paths only exist on Linux; an absent
// file yields no lines, so the scan is harmless on other platforms.
std::string detectContainer() {
  std::error_code ec;
  if (std::filesystem::exists("/.dockerenv", ec)) {
    return "docker";
  }
  if (std::filesystem::exists("/run/.containerenv", ec)) {
    return "podman";
  }
  std::ifstream cgroup("/proc/1/cgroup");
  std::string line;
  while (std::getline(cgroup, line)) {
    if (line.find("docker") != std::string::npos) {
      return "docker";
    }
    if (line.find("podman") != std::string::npos) {
      return "podman";
    }
    if (line.find("kubepods") != std::string::npos) {
      return "kubernetes";
    }
    if (line.find("containerd") != std::string::npos) {
      return "containerd";
    }
    if (line.find("lxc") != std::string::npos) {
      return "lxc";
    }
  }
  return {};
}

// Runs `docker --version` on the host and returns the bare version it reports (e.g. "27.1.1"), or
// "" if docker is not installed/on PATH. `docker --version` is a pure client call — it never
// contacts the daemon, so it cannot hang. Inside Motion Master's own minimal runtime image the
// docker CLI is absent, so this returns "" there even though detectContainer() reports "docker".
std::string detectDockerVersion() {
#if defined(_WIN32)
  std::FILE* pipe = _popen("docker --version 2>NUL", "r");
#else
  std::FILE* pipe = popen("docker --version 2>/dev/null", "r");
#endif
  if (!pipe) {
    return {};
  }
  std::string output;
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  // Expected: "Docker version 27.1.1, build 6312585". Take the token after "version " up to the
  // next comma or whitespace.
  const std::string marker = "version ";
  const auto pos = output.find(marker);
  if (pos == std::string::npos) {
    return {};
  }
  const auto start = pos + marker.size();
  auto end = output.find_first_of(", \t\r\n", start);
  if (end == std::string::npos) {
    end = output.size();
  }
  return output.substr(start, end - start);
}

#if defined(_WIN32)

void collectPlatform(SystemInfo& out) {
  out.osName = "Windows";

  // RtlGetVersion reports the true OS version even when the app lacks a version manifest (unlike
  // the deprecated GetVersionEx). Resolved dynamically so no ntdll import library is needed.
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (rtlGetVersion && rtlGetVersion(&vi) == 0) {
      out.kernel = std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) +
                   "." + std::to_string(vi.dwBuildNumber);
    }
  }

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      out.architecture = "x86_64";
      break;
    case PROCESSOR_ARCHITECTURE_ARM64:
      out.architecture = "aarch64";
      break;
    case PROCESSOR_ARCHITECTURE_INTEL:
      out.architecture = "x86";
      break;
    default:
      break;
  }

  char hostname[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD len = sizeof(hostname);
  if (GetComputerNameExA(ComputerNameDnsHostname, hostname, &len)) {
    out.hostname.assign(hostname, len);
  }

  // The CPU brand string lives in the registry; PROCESSOR_IDENTIFIER is a close, link-free proxy.
  if (const char* ident = std::getenv("PROCESSOR_IDENTIFIER")) {
    out.cpuModel = ident;
  }

  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    out.totalMemoryBytes = mem.ullTotalPhys;
  }
}

#elif defined(__APPLE__)

// Reads a string sysctl by name into @p out, returning empty on failure.
std::string sysctlString(const char* name) {
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return {};
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return {};
  }
  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value;
}

// Reads a 64-bit integer sysctl by name, returning 0 on failure.
uint64_t sysctlUint64(const char* name) {
  uint64_t value = 0;
  std::size_t size = sizeof(value);
  if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
    return 0;
  }
  return value;
}

void collectPlatform(SystemInfo& out) {
  utsname uts{};
  if (uname(&uts) == 0) {
    out.kernel = uts.release;
    out.architecture = uts.machine;
    out.hostname = uts.nodename;
  }
  const std::string product = sysctlString("kern.osproductversion");
  out.osName = product.empty() ? "macOS" : "macOS " + product;
  out.cpuModel = sysctlString("machdep.cpu.brand_string");
  out.totalMemoryBytes = sysctlUint64("hw.memsize");

  const uint64_t cores = sysctlUint64("hw.logicalcpu");
  out.cpuCores = cores ? static_cast<unsigned>(cores) : std::thread::hardware_concurrency();
}

#else  // Linux

// Returns the value of @p key from a shell-style key=value file (e.g. /etc/os-release), with any
// surrounding double quotes stripped. Empty if the file or key is absent.
std::string readKeyValue(const char* path, const std::string& key) {
  std::ifstream file(path);
  std::string line;
  const std::string prefix = key + "=";
  while (std::getline(file, line)) {
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    std::string value = line.substr(prefix.size());
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  }
  return {};
}

// Returns the first "model name" (x86) or "Hardware"/"Model" (ARM) entry from /proc/cpuinfo.
std::string readCpuModel() {
  std::ifstream file("/proc/cpuinfo");
  std::string line;
  while (std::getline(file, line)) {
    for (const char* key : {"model name", "Model", "Hardware"}) {
      const std::string k = key;
      if (line.rfind(k, 0) != 0) {
        continue;
      }
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      auto start = line.find_first_not_of(" \t", colon + 1);
      if (start != std::string::npos) {
        return line.substr(start);
      }
    }
  }
  return {};
}

void collectPlatform(SystemInfo& out) {
  utsname uts{};
  if (uname(&uts) == 0) {
    out.kernel = uts.release;
    out.architecture = uts.machine;
    out.hostname = uts.nodename;
  }

  std::string pretty = readKeyValue("/etc/os-release", "PRETTY_NAME");
  out.osName = pretty.empty() ? std::string(uts.sysname) : pretty;
  out.cpuModel = readCpuModel();

  const auto cores = sysconf(_SC_NPROCESSORS_ONLN);
  out.cpuCores = cores > 0 ? static_cast<unsigned>(cores) : std::thread::hardware_concurrency();

  const auto pages = sysconf(_SC_PHYS_PAGES);
  const auto pageSize = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && pageSize > 0) {
    out.totalMemoryBytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
  }
}

#endif

}  // namespace

SystemInfo collectSystemInfo() {
  SystemInfo info;
  collectPlatform(info);
  if (info.cpuCores == 0) {
    info.cpuCores = std::thread::hardware_concurrency();
  }
  collectDisk(info);
  info.container = detectContainer();
  info.dockerVersion = detectDockerVersion();
  return info;
}

void to_json(nlohmann::json& j, const SystemInfo& info) {
  j = {{"osName", info.osName},
       {"kernel", info.kernel},
       {"architecture", info.architecture},
       {"hostname", info.hostname},
       {"cpuModel", info.cpuModel},
       {"cpuCores", info.cpuCores},
       {"totalMemoryBytes", info.totalMemoryBytes},
       {"diskTotalBytes", info.diskTotalBytes},
       {"diskFreeBytes", info.diskFreeBytes},
       {"container", info.container},
       {"dockerVersion", info.dockerVersion}};
}

}  // namespace mm::core
