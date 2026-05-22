#include "comm/soem_base.h"

#include <map>
#include <string>

#ifdef _WIN32
#include <iomanip>
#include <sstream>
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "IPHLPAPI.lib")
#else
#include <cstring>
#include <linux/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mm::comm::soem {

std::map<std::string, std::string> mapMacAddressesToInterfaces() {
  std::map<std::string, std::string> map;
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
      return map;
    }

    dwRetVal = GetAdaptersAddresses(ulFamily, ulFlags, nullptr, pAddresses, &ulBufLen);
    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
      HeapFree(hHeap, 0x00, pAddresses);
      pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(HeapAlloc(hHeap, 0x00, ulBufLen));
    }

    if (pAddresses == nullptr) {
      return map;
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
        map[ss.str()] = "\\Device\\NPF_" + std::string(pCurrAddresses->AdapterName);
        pCurrAddresses = pCurrAddresses->Next;
      }
    }

    if (pAddresses) {
      HeapFree(hHeap, 0x00, pAddresses);
    }
  }
#else
  ec_adaptert* adapter = ec_find_adapters();
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
    auto* hw = reinterpret_cast<unsigned char*>(ifr.ifr_hwaddr.sa_data);
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
    map[mac] = adapter->name;

    adapter = adapter->next;
  }
  ec_free_adapters(adapter);
#endif
  return map;
}

}  // namespace mm::comm::soem
