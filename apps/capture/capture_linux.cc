#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

#include "capture.h"
#include "capture/ethercat_frame.h"
#include "core/platform.h"

namespace mm::capture {

namespace {

/// Nanoseconds since the epoch from the clock pcap and the summary use.
int64_t nowNs() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

}  // namespace

std::expected<void, std::string> captureFrames(const std::string& adapter,
                                               std::chrono::milliseconds duration,
                                               const FrameSink& sink) {
  // A raw socket for every Ethernet type, bound to the adapter. The kernel hands a frame this host
  // sends only to a packet socket opened for every type; a socket opened for one type, the way
  // SOEM opens its own, gets incoming frames alone, and a capture of returns with no sends says
  // nothing. The EtherCAT frames are picked out below, so nothing else reaches the file.
  const int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (sock < 0) {
    return std::unexpected("cannot open a raw socket (CAP_NET_RAW is needed): " +
                           mm::core::errnoMessage(errno));
  }
  const unsigned ifindex = if_nametoindex(adapter.c_str());
  if (ifindex == 0) {
    close(sock);
    return std::unexpected("no adapter named " + adapter);
  }
  sockaddr_ll address{};
  address.sll_family = AF_PACKET;
  address.sll_protocol = htons(ETH_P_ALL);
  address.sll_ifindex = static_cast<int>(ifindex);
  if (bind(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close(sock);
    return std::unexpected("cannot bind to " + adapter + ": " + mm::core::errnoMessage(errno));
  }
  // The kernel stamps each frame when the driver hands it over, which is the time the summary
  // wants: independent of when this program, or SOEM, gets round to reading the socket.
  const int on = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)) != 0) {
    close(sock);
    return std::unexpected("cannot ask the kernel for timestamps: " +
                           mm::core::errnoMessage(errno));
  }

  std::array<uint8_t, 65536> frame{};
  std::array<uint8_t, 256> control{};
  const auto deadline = std::chrono::steady_clock::now() + duration;
  for (;;) {
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::milliseconds{0}) {
      break;
    }
    pollfd pfd{sock, POLLIN, 0};
    const int waitMs =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count()) + 1;
    const int ready = poll(&pfd, 1, waitMs);
    if (ready < 0 && errno != EINTR) {
      close(sock);
      return std::unexpected("poll failed: " + mm::core::errnoMessage(errno));
    }
    if (ready <= 0) {
      continue;
    }
    iovec iov{frame.data(), frame.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    const ssize_t received = recvmsg(sock, &msg, MSG_DONTWAIT);
    if (received < 14) {
      continue;
    }
    // Ethernet type is big-endian on the wire. Everything that is not EtherCAT is dropped here.
    const uint16_t etherType = static_cast<uint16_t>((frame[12] << 8) | frame[13]);
    if (etherType != kEtherCatEtherType) {
      continue;
    }
    int64_t timestampNs = 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPNS) {
        timespec ts{};
        std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
        timestampNs = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
      }
    }
    if (timestampNs == 0) {
      timestampNs = nowNs();  // the kernel gave none, which it does not do, but a file needs one
    }
    sink(timestampNs, std::span<const uint8_t>(frame.data(), static_cast<std::size_t>(received)));
  }
  close(sock);
  return {};
}

}  // namespace mm::capture
