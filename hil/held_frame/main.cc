// Measures whether the host holds a returned frame until another frame comes back, and whether a
// NOP datagram makes the host hand up such a frame (issue #138).
//
// On the PC of #138 the return of one mailbox read reached SOEM 100 ms late. Its kernel timestamp
// is that of the return of the next frame, which left the PC 24 us before it. The reading of that
// capture is that the drive answered at once, and the return stayed in the receive path of the
// host until another frame came back. This program tests that reading, and it tests the remedy:
// a NOP sent after 2 ms with no return. Every slave passes a NOP on and acts on nothing in it.
//
// The program talks to the slave through SOEM directly, because it has to control which frames go
// out and when. Each frame is one FPRD of 128 bytes from ESC register 0x0000, the size of the
// mailbox read in #138. A read of those registers changes nothing on the slave. The frame is sent
// once and never sent again, as the port sends a mailbox datagram.
//
// Two modes run in turns, one block of frames each, so a change on the host over the run reaches
// both modes:
//   plain  Wait up to 100 ms for the return, as the port does today. When no return comes, send
//          the next frame and look for the old return beside its return. A return found there was
//          held by the host until another frame came back.
//   nudge  Wait 2 ms for the return. When none comes, send a NOP, wait up to 2 ms for it, and look
//          for the return again. Repeat until 100 ms. A return found right after a NOP was
//          released by the NOP.
//
// The result is a table for each mode and one line:
//   THE NOP RELEASES A HELD FRAME  plain mode saw returns that did not come, and in nudge mode a
//                                  NOP released every late return.
//   NO HELD FRAME SEEN             plain mode saw none; run longer, or with --gap.
//   NOT CONFIRMED                  anything else; the table says what.
//
// The held return of #138 came about once in 100,000 mailbox frames, so a run needs about a
// million frames in each mode to see one. A frame takes about 30 us, so that is about a minute.
// Whether the gaps between frames matter on the host is not known. --gap sets the pause between
// two frames.
//
// ecx_config_init puts every slave into INIT. The program changes no other state, so the slaves
// are left in INIT.

#include <soem/soem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace {

// The wait the port gives a mailbox datagram for its frame, EC_TIMEOUTLATE in ports/soem.
constexpr int kLateUs = 100'000;
// The wait before a NOP, EC_TIMEOUTRET.
constexpr int kNudgeAfterUs = EC_TIMEOUTRET;
// A return later than this is late, the wait a register datagram gets before SOEM sends it again.
constexpr int kLateThresholdUs = EC_TIMEOUTRET;
constexpr uint16_t kRegister = 0x0000;
constexpr uint16_t kLength = 128;

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  uint64_t frames = 2'000'000;
  uint64_t block = 10'000;
  int gapUs = 0;
};

void printUsage() {
  std::cerr << "usage: held_frame --ifname <interface> [--slave <position>] [--frames <count>] "
               "[--block <count>] [--gap <us>]\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argc) {
      return false;
    }
    if (arg == "--ifname") {
      options.ifname = argv[++i];
    } else if (arg == "--slave") {
      options.slave = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (arg == "--frames") {
      options.frames = std::stoull(argv[++i]);
    } else if (arg == "--block") {
      options.block = std::stoull(argv[++i]);
    } else if (arg == "--gap") {
      options.gapUs = std::stoi(argv[++i]);
    } else {
      return false;
    }
  }
  return !options.ifname.empty() && options.block > 0;
}

enum class Mode { Plain, Nudge };

std::string_view modeName(Mode mode) { return mode == Mode::Plain ? "plain" : "nudge"; }

struct Tally {
  uint64_t sent = 0;
  // Returns within kLateThresholdUs.
  uint64_t onTime = 0;
  // Returns later than kLateThresholdUs and within kLateUs.
  uint64_t late = 0;
  // No return within kLateUs.
  uint64_t noReturn = 0;
  // Plain: a missing return that came back beside the return of the next frame.
  uint64_t heldToNext = 0;
  // Nudge: NOPs sent.
  uint64_t nops = 0;
  // Nudge: late returns found right after a NOP.
  uint64_t releasedByNop = 0;
  // The latest return within kLateUs.
  int64_t latestUs = 0;
};

using Clock = std::chrono::steady_clock;

int64_t microsecondsSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

// Sends one NOP and waits up to EC_TIMEOUTRET for it. Its receive stores any other return that
// comes up with it under that return's own index.
void sendNop(ecx_portt* port, uint16_t adp) {
  uint8 nothing = 0;
  const uint8 idx = ecx_getindex(port);
  ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_NOP, idx, adp, 0, sizeof(nothing), &nothing);
  ecx_outframe_red(port, idx);
  (void)ecx_waitinframe(port, idx, EC_TIMEOUTRET);
  ecx_setbufstat(port, idx, EC_BUF_EMPTY);
}

// Sends the register read once and leaves its index in use. Returns the index.
uint8 sendRead(ecx_portt* port, uint16_t adp, std::array<uint8_t, kLength>& data) {
  const uint8 idx = ecx_getindex(port);
  ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPRD, idx, adp, kRegister, kLength,
                    data.data());
  ecx_outframe_red(port, idx);
  return idx;
}

// The index of a plain read that got no return, kept in use so its return can still be stored.
struct Missing {
  bool pending = false;
  uint8 idx = 0;
};

void runPlain(ecx_portt* port, uint16_t adp, Tally& tally, Missing& missing) {
  std::array<uint8_t, kLength> data{};
  const auto start = Clock::now();
  const uint8 idx = sendRead(port, adp, data);
  tally.sent++;
  const int wkc = ecx_waitinframe(port, idx, kLateUs);
  const int64_t us = microsecondsSince(start);
  // The wait for this frame also received the frames that came up beside it. A zero wait reads
  // the status the receive left at the index of the missing return.
  if (missing.pending) {
    if (ecx_waitinframe(port, missing.idx, 0) > EC_NOFRAME) {
      tally.heldToNext++;
      std::cout << "  plain: the missing return came back beside the next frame's\n";
    }
    ecx_setbufstat(port, missing.idx, EC_BUF_EMPTY);
    missing.pending = false;
  }
  if (wkc <= EC_NOFRAME) {
    tally.noReturn++;
    std::cout << std::format("  plain: no return within {} ms\n", kLateUs / 1000);
    missing = {.pending = true, .idx = idx};
    return;
  }
  ecx_setbufstat(port, idx, EC_BUF_EMPTY);
  tally.latestUs = std::max(tally.latestUs, us);
  if (us > kLateThresholdUs) {
    tally.late++;
    std::cout << std::format("  plain: return {} us late\n", us);
  } else {
    tally.onTime++;
  }
}

void runNudge(ecx_portt* port, uint16_t adp, Tally& tally) {
  std::array<uint8_t, kLength> data{};
  const auto start = Clock::now();
  const uint8 idx = sendRead(port, adp, data);
  tally.sent++;
  int wkc = ecx_waitinframe(port, idx, kNudgeAfterUs);
  int nops = 0;
  while (wkc <= EC_NOFRAME && microsecondsSince(start) < kLateUs) {
    sendNop(port, adp);
    nops++;
    wkc = ecx_waitinframe(port, idx, 0);
    if (wkc > EC_NOFRAME) {
      tally.releasedByNop++;
      break;
    }
    wkc = ecx_waitinframe(port, idx, kNudgeAfterUs);
  }
  const int64_t us = microsecondsSince(start);
  ecx_setbufstat(port, idx, EC_BUF_EMPTY);
  tally.nops += static_cast<uint64_t>(nops);
  if (wkc <= EC_NOFRAME) {
    tally.noReturn++;
    std::cout << std::format("  nudge: no return within {} ms, after {} NOPs\n", kLateUs / 1000,
                             nops);
    return;
  }
  tally.latestUs = std::max(tally.latestUs, us);
  if (us > kLateThresholdUs) {
    tally.late++;
    std::cout << std::format("  nudge: return {} us late, after {} NOPs\n", us, nops);
  } else {
    tally.onTime++;
  }
}

void printTally(Mode mode, const Tally& t) {
  std::cout << std::format("{}: sent {}, on time {}, late {}, no return {}, latest {} us",
                           modeName(mode), t.sent, t.onTime, t.late, t.noReturn, t.latestUs);
  if (mode == Mode::Plain) {
    std::cout << std::format(", came back beside the next frame {}\n", t.heldToNext);
  } else {
    std::cout << std::format(", NOPs {}, released by a NOP {}\n", t.nops, t.releasedByNop);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    printUsage();
    return EXIT_FAILURE;
  }

  // Zeroed, like the driver's context: SOEM counts on a clean context.
  auto ctx = std::make_unique<ecx_contextt>();
  if (ecx_init(ctx.get(), options.ifname.c_str()) <= 0) {
    std::cerr << "ecx_init failed on " << options.ifname << "\n";
    return EXIT_FAILURE;
  }
  ctx->manualstatechange = 1;
  const int found = ecx_config_init(ctx.get());
  if (found < options.slave) {
    std::cerr << std::format("found {} slave(s), slave {} is not on the bus\n", found,
                             options.slave);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const ec_slavet& device = ctx->slavelist[options.slave];
  std::cout << std::format(
      "slave {} ({}, product 0x{:08X}), {} frames in blocks of {}, gap {} us\n", options.slave,
      device.name, device.eep_id, options.frames, options.block, options.gapUs);

  Tally plain;
  Tally nudge;
  Missing missing;
  for (uint64_t i = 0; i < options.frames; ++i) {
    const Mode mode = (i / options.block) % 2 == 0 ? Mode::Plain : Mode::Nudge;
    if (mode == Mode::Plain) {
      runPlain(&ctx->port, device.configadr, plain, missing);
    } else {
      runNudge(&ctx->port, device.configadr, nudge);
      // A plain read that got no return at the end of its block gets the first nudge read as its
      // next frame. That frame came back, so the check is the same as in plain mode.
      if (missing.pending) {
        if (ecx_waitinframe(&ctx->port, missing.idx, 0) > EC_NOFRAME) {
          plain.heldToNext++;
          std::cout << "  plain: the missing return came back beside the next frame's\n";
        }
        ecx_setbufstat(&ctx->port, missing.idx, EC_BUF_EMPTY);
        missing.pending = false;
      }
    }
    if (options.gapUs > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(options.gapUs));
    }
  }
  if (missing.pending) {
    ecx_setbufstat(&ctx->port, missing.idx, EC_BUF_EMPTY);
  }
  ecx_close(ctx.get());

  printTally(Mode::Plain, plain);
  printTally(Mode::Nudge, nudge);
  // In plain mode nothing else sends a frame, so a held return shows as no return at all.
  if (plain.noReturn == 0) {
    std::cout << "NO HELD FRAME SEEN\n";
  } else if (nudge.noReturn == 0 && nudge.releasedByNop > 0 && nudge.releasedByNop == nudge.late) {
    std::cout << "THE NOP RELEASES A HELD FRAME\n";
  } else {
    std::cout << "NOT CONFIRMED\n";
  }
  return EXIT_SUCCESS;
}
