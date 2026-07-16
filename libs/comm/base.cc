#include "comm/base.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
// clang-format off
// Include order is significant on Windows: <winsock2.h> must precede <windows.h>
// (which otherwise pulls in the conflicting winsock v1), and <iphlpapi.h> needs
// the winsock2 types. clang-format would otherwise sort these alphabetically and
// break the build, so the ordering is pinned here.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

#include <iomanip>
#include <sstream>
#pragma comment(lib, "IPHLPAPI.lib")
#elif defined(__APPLE__)
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <soem/soem.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#else
#include <linux/if.h>
#include <netinet/in.h>
#include <soem/soem.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#endif

namespace mm::comm {

void to_json(nlohmann::json& j, const NetworkAdapter& a) {
  j = nlohmann::json{
      {"macLinux", a.macLinux},
      {"macWindows", a.macWindows},
      {"name", a.name},
  };
}

bool isMacAddress(const std::string& s) {
  // Six hex octets separated by a single, consistent delimiter: the first separator is captured and
  // the backreference (\1) forces the remaining four to match it, so mixed colon/dash forms
  // (e.g. "AA:BB-CC:...") are rejected. Built once (static): std::regex construction is expensive.
  static const std::regex kMacRe{R"(^[0-9A-Fa-f]{2}([:-])(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}$)"};
  return std::regex_match(s, kMacRe);
}

std::string normalizeMac(const std::string& raw, char sep) {
  std::string result;
  result.reserve(17);
  for (int i = 0; i < 6; ++i) {
    if (i > 0) {
      result += sep;
    }
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i * 3])));
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i * 3 + 1])));
  }
  return result;
}

std::vector<NetworkAdapter> enumerateNetworkAdapters() {
  std::vector<NetworkAdapter> adapters;
#ifdef _WIN32
  {
    int nAddressCount = 0;
    ULONG ulFlags = GAA_FLAG_INCLUDE_ALL_COMPARTMENTS;
    ULONG ulFamily = AF_UNSPEC;
    unsigned char* pszBuff = nullptr;
    unsigned char** pszAddress = &pszBuff;
    PIP_ADAPTER_ADDRESSES pCurrAddresses = nullptr;
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    DWORD dwRetVal = 0;
    ULONG ulBufLen = sizeof(IP_ADAPTER_ADDRESSES);
    HANDLE hHeap = GetProcessHeap();

    pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(HeapAlloc(hHeap, 0x00, ulBufLen));
    if (pAddresses == nullptr) {
      return adapters;
    }

    dwRetVal = GetAdaptersAddresses(ulFamily, ulFlags, nullptr, pAddresses, &ulBufLen);
    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
      HeapFree(hHeap, 0x00, pAddresses);
      pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(HeapAlloc(hHeap, 0x00, ulBufLen));
    }

    if (pAddresses == nullptr) {
      return adapters;
    }

    dwRetVal = GetAdaptersAddresses(ulFamily, ulFlags, nullptr, pAddresses, &ulBufLen);
    if (dwRetVal == NO_ERROR) {
      pCurrAddresses = pAddresses;
      while (pCurrAddresses) {
        pCurrAddresses = pCurrAddresses->Next;
        ++nAddressCount;
      }

      *pszAddress = reinterpret_cast<unsigned char*>(
          HeapAlloc(hHeap, 0x00, MAX_ADAPTER_ADDRESS_LENGTH * nAddressCount));
      pCurrAddresses = pAddresses;
      nAddressCount = 0;
      while (pCurrAddresses) {
        RtlCopyMemory(*pszAddress + (MAX_ADAPTER_ADDRESS_LENGTH * nAddressCount++),
                      pCurrAddresses->PhysicalAddress, MAX_ADAPTER_ADDRESS_LENGTH);

        std::stringstream ss;
        ss << std::hex << std::uppercase;
        constexpr int kMacLen = 6;
        for (int i = 0; i < kMacLen; i++) {
          ss << std::setw(2) << std::setfill('0')
             << static_cast<int>(pCurrAddresses->PhysicalAddress[i]);
          if (i < kMacLen - 1) {
            ss << ":";
          }
        }
        std::string mac = ss.str();
        std::string name = "\\Device\\NPF_" + std::string(pCurrAddresses->AdapterName);
        adapters.push_back({.macLinux = mac, .macWindows = normalizeMac(mac, '-'), .name = name});
        pCurrAddresses = pCurrAddresses->Next;
      }
    }

    if (pAddresses) {
      HeapFree(hHeap, 0x00, pAddresses);
    }
  }
#elif defined(__APPLE__)
  // macOS has no SIOCGIFHWADDR; link-layer (MAC) addresses come from
  // getifaddrs() via the AF_LINK / sockaddr_dl entries.
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == 0) {
    for (const struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_LINK) {
        continue;
      }
      const auto* sdl = reinterpret_cast<const struct sockaddr_dl*>(ifa->ifa_addr);
      if (sdl->sdl_alen != 6) {
        continue;
      }
      const auto* hw = reinterpret_cast<const unsigned char*>(LLADDR(sdl));
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", hw[0], hw[1], hw[2], hw[3], hw[4],
               hw[5]);
      adapters.push_back(
          {.macLinux = mac, .macWindows = normalizeMac(mac, '-'), .name = ifa->ifa_name});
    }
    freeifaddrs(ifaddr);
  }
#else
  ec_adaptert* adaptersHead = ec_find_adapters();
  ec_adaptert* adapter = adaptersHead;
  while (adapter != nullptr) {
    int fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
      adapter = adapter->next;
      continue;
    }

    struct ifreq ifr{};
    memcpy(ifr.ifr_name, adapter->name, sizeof(ifr.ifr_name) - 1);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
      close(fd);
      adapter = adapter->next;
      continue;
    }
    close(fd);

    char mac[18];
    const auto* hw = reinterpret_cast<const unsigned char*>(ifr.ifr_hwaddr.sa_data);
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", hw[0], hw[1], hw[2], hw[3], hw[4],
             hw[5]);
    adapters.push_back(
        {.macLinux = mac, .macWindows = normalizeMac(mac, '-'), .name = adapter->name});

    adapter = adapter->next;
  }
  ec_free_adapters(adaptersHead);
#endif
  return adapters;
}

std::expected<NetworkAdapter, std::string> resolveNetworkAdapter(const std::string& input) {
  auto adapters = enumerateNetworkAdapters();

  if (isMacAddress(input)) {
    std::string macLinux = normalizeMac(input, ':');
    auto it = std::find_if(adapters.begin(), adapters.end(),
                           [&](const NetworkAdapter& a) { return a.macLinux == macLinux; });
    if (it == adapters.end()) {
      return std::unexpected("no network adapter found with MAC " + macLinux);
    }
    return *it;
  }

  // Reverse lookup by interface name.
  auto it = std::find_if(adapters.begin(), adapters.end(),
                         [&](const NetworkAdapter& a) { return a.name == input; });
  if (it == adapters.end()) {
    return std::unexpected("network adapter '" + input + "' not found");
  }
  return *it;
}

}  // namespace mm::comm
