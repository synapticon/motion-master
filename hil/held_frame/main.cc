// Measures whether the host holds a returned frame until another frame comes back, and whether a
// NOP datagram makes the host hand up such a frame (issue #138).
//
// On the PC of #138 the return of one mailbox read reached SOEM 100 ms late, during an FoE read of
// config.csv. Its kernel timestamp is that of the return of the next frame, which left the PC 24 us
// before it. The reading of that capture is that the drive answered at once, and the return stayed
// in the receive path of the host until another frame came back. This program tests that reading,
// and it tests the remedy: a NOP sent after 2 ms with no return. Every slave passes a NOP on and
// acts on nothing in it.
//
// The program reads one file over FoE again and again, with the frames Motion Master sends for it:
// a mailbox write for the request and for each acknowledgement, a read of the SyncManager 1 status
// every 200 us until the mailbox is full, and a read of the mailbox. The mailbox write and the
// mailbox read go out once and are never sent again, as the SOEM port sends them. The status read
// is a register datagram, so SOEM sends it again every EC_TIMEOUTRET, as it does in Motion Master.
// The program talks to the slave through SOEM directly, because it has to control how it waits for
// each frame.
//
// Two modes run in turns, one block of file reads each, so a change on the host over the run
// reaches both modes. They differ only in how they wait for the return of a mailbox write or a
// mailbox read:
//   plain  Wait up to 100 ms, as the port does today. When no return comes, send the SyncManager 1
//          status read, which is the next frame Motion Master sends, and look for the old return
//          beside its return. Motion Master gives up on the transfer at that point. The program
//          uses the return when it came up, so the transfer goes on and the drive keeps a clean
//          state.
//   nudge  Wait 2 ms. When no return comes, send a NOP, wait up to 2 ms for it, and look for the
//          return again. Repeat until 100 ms.
//
// Each file read is compared with the first read of the run. A read that differs is reported.
//
// The result is a table for each mode and one line:
//   THE NOP RELEASES A HELD FRAME  in plain mode some returns came up only beside the status read,
//                                  in nudge mode a NOP released at least one return, and every
//                                  return came within 20 ms.
//   NO HELD FRAME SEEN             plain mode saw none; run longer.
//   NOT CONFIRMED                  anything else; the table says what.
//
// The held return of #138 came about once in 100,000 mailbox frames. A read of config.csv on an
// Integro is about 40 packets and a few hundred frames, so a run needs a few thousand reads to see
// a few held returns.
//
// The program puts the drive into INIT and then PRE-OP, and leaves it in PRE-OP.

#include <soem/soem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/soem_mailbox.h"

namespace {

using mm::comm::EtherCatState;

// The wait the port gives a mailbox datagram for its frame, EC_TIMEOUTLATE in ports/soem.
constexpr int kLateUs = 100'000;
// The wait before a NOP, and the line above which a return counts as late.
constexpr int kNudgeAfterUs = EC_TIMEOUTRET;
// In nudge mode a return later than this was not released by a NOP.
constexpr int64_t kReleasedWithinUs = 20'000;
// The pause between two reads of the SyncManager 1 status, EC_LOCALDELAY in SOEM.
constexpr auto kPollInterval = std::chrono::microseconds(200);
// How long the program waits for an answer in the mailbox, kFoeReadTimeout of the driver.
constexpr auto kAnswerTimeout = std::chrono::seconds(6);
// How long the program waits for the slave to empty its receive mailbox before a write.
constexpr auto kWriteTimeout = std::chrono::milliseconds(20);
constexpr int kStateTimeoutUs = 10'000'000;
// Bit 3 of the SyncManager status: the mailbox holds a message.
constexpr uint8_t kMailboxFull = 0x08;
// The mailbox header and the FoE header, 6 bytes each.
constexpr std::size_t kHeaders = 12;

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  std::string file = "config.csv";
  int reads = 2000;
  int block = 1;
};

void printUsage() {
  std::cerr << "usage: held_frame --ifname <interface> [--slave <position>] [--file <name>] "
               "[--reads <count>] [--block <count>]\n";
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
    } else if (arg == "--file") {
      options.file = argv[++i];
    } else if (arg == "--reads") {
      options.reads = std::stoi(argv[++i]);
    } else if (arg == "--block") {
      options.block = std::stoi(argv[++i]);
    } else {
      return false;
    }
  }
  return !options.ifname.empty() && !options.file.empty() && options.reads > 0 && options.block > 0;
}

enum class Mode { Plain, Nudge };

std::string_view modeName(Mode mode) { return mode == Mode::Plain ? "plain" : "nudge"; }

// The frames a file read sends.
enum class Kind { Write, Read, Status };
constexpr std::size_t kKinds = 3;

std::string_view kindName(Kind kind) {
  switch (kind) {
    case Kind::Write:
      return "mailbox write";
    case Kind::Read:
      return "mailbox read";
    case Kind::Status:
      return "status read";
  }
  return "frame";
}

struct KindTally {
  uint64_t sent = 0;
  // Returns later than kNudgeAfterUs.
  uint64_t late = 0;
  // Late returns that came up beside the return of another frame: the status read in plain mode,
  // a NOP in nudge mode.
  uint64_t released = 0;
  // No return within kLateUs, also not beside the return of another frame.
  uint64_t lost = 0;
  int64_t latestUs = 0;
};

struct Tally {
  std::array<KindTally, kKinds> kinds{};
  uint64_t nops = 0;
  int reads = 0;
  int same = 0;
  int different = 0;
  int failed = 0;

  KindTally& of(Kind kind) { return kinds[static_cast<std::size_t>(kind)]; }
  const KindTally& of(Kind kind) const { return kinds[static_cast<std::size_t>(kind)]; }
};

using Clock = std::chrono::steady_clock;

int64_t microsecondsSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

struct Session {
  ecx_contextt* ctx = nullptr;
  uint16_t slave = 0;
  Mode mode = Mode::Plain;
  Tally* tally = nullptr;

  ecx_portt* port() const { return &ctx->port; }
  ec_slavet& device() const { return ctx->slavelist[slave]; }
  uint16_t adp() const { return device().configadr; }
};

void record(Session& s, Kind kind, int64_t us, bool returned, bool released) {
  KindTally& k = s.tally->of(kind);
  if (!returned) {
    k.lost++;
    std::cout << std::format("  {}: {} got no return within {} ms\n", modeName(s.mode),
                             kindName(kind), kLateUs / 1000);
    return;
  }
  k.latestUs = std::max(k.latestUs, us);
  if (released) {
    k.released++;
  }
  if (us > kNudgeAfterUs) {
    k.late++;
    std::cout << std::format("  {}: {} returned {} us late{}\n", modeName(s.mode), kindName(kind),
                             us, released ? ", beside the return of the frame after it" : "");
  }
}

// Reads the SyncManager 1 status. A register datagram: SOEM sends it again every EC_TIMEOUTRET
// until the frame comes back, as in Motion Master.
int readSm1Status(Session& s, uint8_t& status) {
  s.tally->of(Kind::Status).sent++;
  const auto start = Clock::now();
  const int wkc = ecx_FPRD(s.port(), s.adp(), ECT_REG_SM1STAT, sizeof(status), &status, kLateUs);
  record(s, Kind::Status, microsecondsSince(start), wkc > EC_NOFRAME, false);
  return wkc;
}

// Sends one NOP and waits up to EC_TIMEOUTRET for it. Its receive stores any other return that
// comes up with it under the index of that return.
void sendNop(Session& s) {
  uint8 nothing = 0;
  ecx_portt* port = s.port();
  const uint8 idx = ecx_getindex(port);
  ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_NOP, idx, s.adp(), 0, sizeof(nothing),
                    &nothing);
  ecx_outframe_red(port, idx);
  (void)ecx_waitinframe(port, idx, EC_TIMEOUTRET);
  ecx_setbufstat(port, idx, EC_BUF_EMPTY);
  s.tally->nops++;
}

// Sends one mailbox datagram once and waits for its return the way the mode says. Returns the
// working counter, or EC_NOFRAME. A read copies the data of its return into data.
int sendOnce(Session& s, Kind kind, uint8 command, uint16 ado, uint16 length, void* data) {
  ecx_portt* port = s.port();
  s.tally->of(kind).sent++;
  const uint8 idx = ecx_getindex(port);
  ecx_setupdatagram(port, &(port->txbuf[idx]), command, idx, s.adp(), ado, length, data);
  const auto start = Clock::now();
  ecx_outframe_red(port, idx);
  int wkc = EC_NOFRAME;
  bool released = false;
  if (s.mode == Mode::Plain) {
    wkc = ecx_waitinframe(port, idx, kLateUs);
    if (wkc <= EC_NOFRAME) {
      // The frame Motion Master sends next. A zero wait then reads the status that its receive
      // left at the index of the missing return.
      uint8_t status = 0;
      readSm1Status(s, status);
      wkc = ecx_waitinframe(port, idx, 0);
      released = wkc > EC_NOFRAME;
    }
  } else {
    wkc = ecx_waitinframe(port, idx, kNudgeAfterUs);
    while (wkc <= EC_NOFRAME && microsecondsSince(start) < kLateUs) {
      sendNop(s);
      wkc = ecx_waitinframe(port, idx, 0);
      if (wkc > EC_NOFRAME) {
        released = true;
        break;
      }
      wkc = ecx_waitinframe(port, idx, kNudgeAfterUs);
    }
  }
  const int64_t us = microsecondsSince(start);
  if (wkc > 0 && command == EC_CMD_FPRD) {
    std::memcpy(data, &(port->rxbuf[idx][EC_HEADERSIZE]), length);
  }
  ecx_setbufstat(port, idx, EC_BUF_EMPTY);
  record(s, kind, us, wkc > EC_NOFRAME, released);
  return wkc;
}

using Mailbox = std::array<uint8_t, sizeof(ec_mbxbuft)>;

// Takes the next mailbox counter. Each request takes a new one, so no counter is sent twice.
uint8_t nextCounter(const Session& s) {
  const uint8_t counter = ec_nextmbxcnt(s.device().mbx_cnt);
  s.device().mbx_cnt = counter;
  return counter;
}

void setHeader(Mailbox& m, uint16_t length, uint8_t type, uint8_t counter) {
  m[0] = static_cast<uint8_t>(length & 0xFF);
  m[1] = static_cast<uint8_t>(length >> 8);
  m[5] = static_cast<uint8_t>(type | MBX_HDR_SET_CNT(counter));
}

void putU32(Mailbox& m, std::size_t at, uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    m[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

uint32_t getU32(const Mailbox& m, std::size_t at) {
  uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(m[at + i]) << (8 * i);
  }
  return value;
}

// Writes a message into the receive mailbox. A working counter of 0 means that the ESC refused
// the write because the mailbox was still full. The slave did not get it, so it is written again.
bool writeMailbox(Session& s, Mailbox& message) {
  const auto start = Clock::now();
  for (;;) {
    const int wkc =
        sendOnce(s, Kind::Write, EC_CMD_FPWR, s.device().mbx_wo, s.device().mbx_l, message.data());
    if (wkc > 0) {
      return true;
    }
    if (wkc <= EC_NOFRAME || Clock::now() - start > kWriteTimeout) {
      return false;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
}

// Waits until the send mailbox is full and reads it. Returns the reason when it fails.
std::optional<std::string> readMailbox(Session& s, Mailbox& message) {
  const auto start = Clock::now();
  for (;;) {
    uint8_t status = 0;
    if (readSm1Status(s, status) > 0 && (status & kMailboxFull) != 0) {
      break;
    }
    if (Clock::now() - start > kAnswerTimeout) {
      return "no answer within 6 s";
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  message.fill(0);
  const int wkc =
      sendOnce(s, Kind::Read, EC_CMD_FPRD, s.device().mbx_ro, s.device().mbx_rl, message.data());
  if (wkc <= 0) {
    return std::format("the mailbox read returned wkc {}", wkc);
  }
  return std::nullopt;
}

// Reads a file over FoE. Returns the bytes, or nothing with the reason in why.
std::optional<std::vector<uint8_t>> readFile(Session& s, std::string_view file, std::string& why) {
  // SOEM takes a packet of this size as full and expects another one after it.
  const std::size_t maxData = s.device().mbx_l - kHeaders;
  Mailbox request{};
  setHeader(request, static_cast<uint16_t>(6 + file.size()), ECT_MBXT_FOE, nextCounter(s));
  request[6] = ECT_FOE_READ;
  std::copy(file.begin(), file.end(), request.begin() + kHeaders);
  if (!writeMailbox(s, request)) {
    why = "the read request was not written";
    return std::nullopt;
  }
  std::vector<uint8_t> content;
  for (uint32_t expected = 1;; ++expected) {
    Mailbox answer{};
    if (const auto failed = readMailbox(s, answer)) {
      why = std::format("packet {}: {}", expected, *failed);
      return std::nullopt;
    }
    const auto length = static_cast<std::size_t>(answer[0] | (answer[1] << 8));
    if ((answer[5] & 0x0F) != ECT_MBXT_FOE) {
      why = std::format("packet {}: a mailbox message of type {}", expected, answer[5] & 0x0F);
      return std::nullopt;
    }
    if (answer[6] == ECT_FOE_ERROR) {
      why = std::format("packet {}: FoE error 0x{:08X}", expected, getU32(answer, 8));
      return std::nullopt;
    }
    const uint32_t packet = getU32(answer, 8);
    if (answer[6] != ECT_FOE_DATA || packet != expected || length < 6 ||
        length - 6 > answer.size() - kHeaders) {
      why = std::format("packet {}: opcode {}, packet {}, length {}", expected, answer[6], packet,
                        length);
      return std::nullopt;
    }
    const std::size_t data = length - 6;
    content.insert(content.end(), answer.begin() + kHeaders,
                   answer.begin() + static_cast<std::ptrdiff_t>(kHeaders + data));
    Mailbox ack{};
    setHeader(ack, 6, ECT_MBXT_FOE, nextCounter(s));
    ack[6] = ECT_FOE_ACK;
    putU32(ack, 8, packet);
    if (!writeMailbox(s, ack)) {
      why = std::format("packet {}: the acknowledgement was not written", expected);
      return std::nullopt;
    }
    if (data < maxData) {
      return content;
    }
  }
}

bool requestState(ecx_contextt* ctx, uint16_t slave, EtherCatState state) {
  const auto raw = static_cast<uint16_t>(state);
  ctx->slavelist[slave].state = raw;
  ecx_writestate(ctx, slave);
  return ecx_statecheck(ctx, slave, raw, kStateTimeoutUs) == raw;
}

// Sends an SDO upload of 0x1000:00 with counter 0 and reads the answer. Both SOMANET stacks accept
// counter 0 whatever came before it, so the counters of the run start from a known point.
bool resetCounter(Session& s) {
  mm::comm::soem::drainMailbox(s.ctx, s.slave);
  Mailbox request{};
  // CoE header, SDO command, index 0x1000 and subindex 0, then 4 bytes of data.
  setHeader(request, 10, ECT_MBXT_COE, 0);
  request[7] = static_cast<uint8_t>(ECT_COES_SDOREQ << 4);
  request[8] = ECT_SDO_UP_REQ;
  request[9] = 0x00;
  request[10] = 0x10;
  s.device().mbx_cnt = 0;
  Mailbox answer{};
  return writeMailbox(s, request) && !readMailbox(s, answer);
}

void printTally(Mode mode, const Tally& t) {
  std::cout << std::format("{}: {} reads, {} same as the first, {} different, {} failed\n",
                           modeName(mode), t.reads, t.same, t.different, t.failed);
  for (const Kind kind : {Kind::Write, Kind::Read, Kind::Status}) {
    const KindTally& k = t.of(kind);
    std::cout << std::format(
        "  {:<13}  sent {:>9}  late {:>4}  released {:>4}  lost {:>4}  latest {:>6} us\n",
        kindName(kind), k.sent, k.late, k.released, k.lost, k.latestUs);
  }
  if (mode == Mode::Nudge) {
    std::cout << std::format("  NOPs sent {}\n", t.nops);
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
  const uint16_t slave = options.slave;
  if (options.file.size() > ctx->slavelist[slave].mbx_l - kHeaders) {
    std::cerr << "the file name does not fit in the mailbox\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  // The abort after a failed read goes through ecx_mbxsend. These two flags keep it to one write
  // and no Repeat Request, as the driver sets them for a SOMANET device.
  ctx->slavelist[slave].mbxwriteonce = TRUE;
  ctx->slavelist[slave].mbxnorepeat = TRUE;
  if (!requestState(ctx.get(), slave, EtherCatState::Init) ||
      !requestState(ctx.get(), slave, EtherCatState::PreOp)) {
    std::cerr << "the slave did not reach PRE-OP\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  Tally plain;
  Tally nudge;
  Session session{.ctx = ctx.get(), .slave = slave, .mode = Mode::Plain, .tally = &plain};
  if (!resetCounter(session)) {
    std::cerr << "the slave did not answer a request with counter 0\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  // The reset is not part of the measurement.
  plain = Tally{};
  const ec_slavet& device = ctx->slavelist[slave];
  std::cout << std::format(
      "slave {} ({}, product 0x{:08X}) in PRE-OP, '{}' read {} times in blocks of {}\n", slave,
      device.name, device.eep_id, options.file, options.reads, options.block);

  std::optional<std::vector<uint8_t>> first;
  for (int i = 0; i < options.reads; ++i) {
    const Mode mode = (i / options.block) % 2 == 0 ? Mode::Plain : Mode::Nudge;
    Tally& tally = mode == Mode::Plain ? plain : nudge;
    session.mode = mode;
    session.tally = &tally;
    tally.reads++;
    std::string why;
    const auto content = readFile(session, options.file, why);
    if (!content) {
      tally.failed++;
      std::cout << std::format("  {}: read {} failed: {}\n", modeName(mode), i + 1, why);
      // Ends the transfer on the drive, as the driver does after a failed read.
      mm::comm::soem::abortFoeTransfer(ctx.get(), slave);
      continue;
    }
    if (!first) {
      first = *content;
      std::cout << std::format("  the first read is {} bytes\n", first->size());
    }
    if (*content == *first) {
      tally.same++;
    } else {
      tally.different++;
      std::cout << std::format("  {}: read {} is {} bytes and differs from the first\n",
                               modeName(mode), i + 1, content->size());
    }
    if ((i + 1) % 500 == 0) {
      std::cout << std::format("  {} reads done\n", i + 1);
    }
  }
  ecx_close(ctx.get());

  printTally(Mode::Plain, plain);
  printTally(Mode::Nudge, nudge);
  const uint64_t heldInPlain = plain.of(Kind::Write).released + plain.of(Kind::Read).released;
  const uint64_t releasedInNudge = nudge.of(Kind::Write).released + nudge.of(Kind::Read).released;
  const bool nudgeInTime = releasedInNudge > 0 &&
                           nudge.of(Kind::Write).lost + nudge.of(Kind::Read).lost == 0 &&
                           nudge.of(Kind::Write).latestUs <= kReleasedWithinUs &&
                           nudge.of(Kind::Read).latestUs <= kReleasedWithinUs;
  if (heldInPlain == 0) {
    std::cout << "NO HELD FRAME SEEN\n";
  } else if (nudgeInTime) {
    std::cout << "THE NOP RELEASES A HELD FRAME\n";
  } else {
    std::cout << "NOT CONFIRMED\n";
  }
  return EXIT_SUCCESS;
}
