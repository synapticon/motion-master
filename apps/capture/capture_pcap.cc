// clang-format off
#include <pcap.h>
// clang-format on

#include <array>
#include <string>

#include "capture.h"
#include "capture/ethercat_frame.h"

namespace mm::capture {

std::expected<void, std::string> captureFrames(const std::string& adapter,
                                               std::chrono::milliseconds duration,
                                               const FrameSink& sink) {
  std::array<char, PCAP_ERRBUF_SIZE> error{};
  // Immediate mode, so a frame reaches this program when it arrives rather than when the capture
  // buffer fills. The read timeout bounds each wait, so the deadline is checked often.
  pcap_t* handle = pcap_create(adapter.c_str(), error.data());
  if (handle == nullptr) {
    return std::unexpected("cannot open " + adapter + ": " + error.data());
  }
  pcap_set_snaplen(handle, 65535);
  pcap_set_promisc(handle, 0);
  pcap_set_timeout(handle, 100);
  pcap_set_immediate_mode(handle, 1);
  pcap_set_tstamp_precision(handle, PCAP_TSTAMP_PRECISION_NANO);
  if (pcap_activate(handle) != 0) {
    const std::string why = pcap_geterr(handle);
    pcap_close(handle);
    return std::unexpected("cannot capture on " + adapter + ": " + why);
  }
  // Only EtherCAT frames, the way the Linux socket is opened for that Ethernet type alone.
  bpf_program filter{};
  if (pcap_compile(handle, &filter, "ether proto 0x88a4", 1, PCAP_NETMASK_UNKNOWN) == 0) {
    pcap_setfilter(handle, &filter);
    pcap_freecode(&filter);
  }

  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    pcap_pkthdr* header = nullptr;
    const u_char* data = nullptr;
    const int rc = pcap_next_ex(handle, &header, &data);
    if (rc < 0) {
      break;  // the capture ended or failed; what was read is kept
    }
    if (rc == 0) {
      continue;  // the read timeout, with nothing to read
    }
    const int64_t timestampNs =
        static_cast<int64_t>(header->ts.tv_sec) * 1'000'000'000 + header->ts.tv_usec;
    sink(timestampNs, std::span<const uint8_t>(data, header->caplen));
  }
  pcap_close(handle);
  return {};
}

}  // namespace mm::capture
