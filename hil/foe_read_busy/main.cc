// Shows on a real drive what follows an FoE BUSY that the drive sends as its answer to a read
// request.
//
// An Integro answers a read request in BOOT with FoE BUSY, 0.5 s after the request, and SOEM's
// ecx_FOEread takes BUSY for a packet it does not expect. ETG.1000.5 defines BUSY only for a write.
// The program talks to the drive through SOEM directly, so that it can keep waiting after the BUSY
// instead of giving up on the read.
//
// Each round:
//   1. Put the drive into INIT, then into the state of --state, BOOT or PRE-OP. With --via-preop,
//      the drive goes INIT, PRE-OP, INIT first, because an Integro answers BUSY only on the first
//      read after it enters BOOT from PRE-OP.
//   2. Wait --delay milliseconds, then send one read request for the file.
//   3. Print every mailbox message the drive sends, with the time since the request, until the
//      last data packet, an FoE error, or --wait seconds without the file. Each data packet gets
//      an ACK.
//
// --after-busy picks what the master does when the drive answers BUSY: wait, abort, send the read
// request again, abort and send it again 200 ms later as the driver does, or send an FoE ACK of
// packet 0, which is what the Beckhoff slave stack code waits for after its own BUSY on a read.
// With --until-file, an FoE error is followed by the read request again 200 ms later. --gap is a
// pause after each round, between the last ACK and the next state request.
//
// A round ends with DATA AFTER BUSY when the drive sent BUSY and then the file, with NO BUSY when
// it sent the file without BUSY, and with NO DATA AFTER BUSY or FAIL for anything else. The drive is
// left in PRE-OP. It needs raw socket access, and no other master may use the adapter.

#include <soem/soem.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "comm/fieldbus_driver.h"
#include "comm/soem_mailbox.h"
#include "comm/soem_station_address.h"

namespace {

using mm::comm::EtherCatState;
using Clock = std::chrono::steady_clock;

constexpr int kStateTimeoutUs = 10'000'000;
// How long one state request waits before it is written again.
constexpr int kStateRetryUs = 1'000'000;
constexpr int kReceiveTimeoutUs = 100'000;
constexpr uint8_t kFoeOpcodeRead = 0x01;
constexpr uint8_t kFoeOpcodeData = 0x03;
constexpr uint8_t kFoeOpcodeAck = 0x04;
constexpr uint8_t kFoeOpcodeError = 0x05;
constexpr uint8_t kFoeOpcodeBusy = 0x06;

// What the master does when the drive answers BUSY.
enum class AfterBusy : uint8_t {
  kWait,         // Nothing. It waits for the file.
  kAbort,        // Sends the FoE abort, and nothing more.
  kReread,       // Sends the read request again, with no abort.
  kAbortReread,  // Sends the FoE abort, and the read request again 200 ms later.
  kAck0,         // Sends an FoE ACK of packet 0, and waits for the file.
};

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  std::string file = ".hardware_description";
  EtherCatState state = EtherCatState::Boot;
  int rounds = 3;
  int waitS = 10;
  AfterBusy afterBusy = AfterBusy::kWait;
  bool viaPreOp = false;
  int delayMs = 0;
  bool untilFile = false;
  int gapMs = 0;
};

void printUsage() {
  std::cerr << "usage: foe_read_busy --ifname <interface> [--slave <position>] [--file <name>]\n"
               "                     [--state boot|preop] [--rounds <n>] [--wait <s>]\n"
               "                     [--after-busy wait|abort|reread|abort-reread|ack0]\n"
               "                     [--via-preop] [--delay <ms>] [--until-file] [--gap <ms>]\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--via-preop") {
      options.viaPreOp = true;
      continue;
    }
    if (arg == "--until-file") {
      options.untilFile = true;
      continue;
    }
    if (i + 1 >= argc) {
      return false;
    }
    if (arg == "--ifname") {
      options.ifname = argv[++i];
    } else if (arg == "--slave") {
      options.slave = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (arg == "--file") {
      options.file = argv[++i];
    } else if (arg == "--state") {
      const std::string_view state = argv[++i];
      if (state == "boot") {
        options.state = EtherCatState::Boot;
      } else if (state == "preop") {
        options.state = EtherCatState::PreOp;
      } else {
        return false;
      }
    } else if (arg == "--rounds") {
      options.rounds = std::stoi(argv[++i]);
    } else if (arg == "--gap") {
      options.gapMs = std::stoi(argv[++i]);
    } else if (arg == "--delay") {
      options.delayMs = std::stoi(argv[++i]);
    } else if (arg == "--wait") {
      options.waitS = std::stoi(argv[++i]);
    } else if (arg == "--after-busy") {
      const std::string_view action = argv[++i];
      if (action == "wait") {
        options.afterBusy = AfterBusy::kWait;
      } else if (action == "abort") {
        options.afterBusy = AfterBusy::kAbort;
      } else if (action == "reread") {
        options.afterBusy = AfterBusy::kReread;
      } else if (action == "abort-reread") {
        options.afterBusy = AfterBusy::kAbortReread;
      } else if (action == "ack0") {
        options.afterBusy = AfterBusy::kAck0;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return !options.ifname.empty() && options.rounds > 0 && options.waitS > 0;
}

// A drive can restart on its way out of BOOT, and the restart clears its station address: a
// SOMANET drive on the step to PRE-OP, and a Jasper-based drive on the step to INIT after a commit
// or on the step to PRE-OP without one. So the request is written again every second until
// kStateTimeoutUs, and a drive found without its address gets it back, and on the step to PRE-OP
// its mailbox sync managers too.
bool requestState(ecx_contextt* ctx, uint16_t slave, EtherCatState state) {
  const auto raw = static_cast<uint16_t>(state);
  const auto deadline = Clock::now() + std::chrono::microseconds(kStateTimeoutUs);
  for (;;) {
    ctx->slavelist[slave].state = raw;
    ecx_writestate(ctx, slave);
    if (ecx_statecheck(ctx, slave, raw, kStateRetryUs) == raw) {
      return true;
    }
    if (Clock::now() >= deadline) {
      return false;
    }
    if (mm::comm::soem::restoreStationAddress(ctx, slave) ==
            mm::comm::soem::AddressState::Reassigned &&
        state == EtherCatState::PreOp) {
      mm::comm::soem::updateMailboxSyncManagers(ctx, slave, EtherCatState::PreOp);
    }
  }
}

// Builds an FoE request with the next mailbox counter: the 6-byte mailbox header, the opcode, a
// reserved byte, a 32-bit word, and the payload. The word is the password of a READ and the
// packet number of an ACK.
ec_mbxbuft* makeFoeRequest(ecx_contextt* ctx, uint16_t slave, uint8_t opcode, uint32_t word,
                           std::string_view payload) {
  ec_mbxbuft* out = ecx_getmbx(ctx);
  if (out == nullptr) {
    return nullptr;
  }
  ec_clearmbx(out);
  auto* bytes = reinterpret_cast<uint8_t*>(out);
  const auto length = static_cast<uint16_t>(6 + payload.size());
  bytes[0] = static_cast<uint8_t>(length & 0xFF);
  bytes[1] = static_cast<uint8_t>(length >> 8);
  const uint8_t counter = ec_nextmbxcnt(ctx->slavelist[slave].mbx_cnt);
  ctx->slavelist[slave].mbx_cnt = counter;
  bytes[5] = static_cast<uint8_t>(ECT_MBXT_FOE + MBX_HDR_SET_CNT(counter));
  bytes[6] = opcode;
  for (int shift = 0; shift < 32; shift += 8) {
    bytes[8 + shift / 8] = static_cast<uint8_t>((word >> shift) & 0xFF);
  }
  std::memcpy(&bytes[12], payload.data(), payload.size());
  return out;
}

uint32_t word32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

// Sends one read request and prints every answer until the file is complete, the drive sends an FoE
// error, or the wait runs out. Returns the verdict of the round.
std::string readAndListen(ecx_contextt* ctx, uint16_t slave, const std::string& file, int waitS,
                          AfterBusy afterBusy, bool untilFile) {
  mm::comm::soem::drainMailbox(ctx, slave);
  const auto sendRead = [&] {
    ec_mbxbuft* request = makeFoeRequest(ctx, slave, kFoeOpcodeRead, 0, file);
    return request != nullptr && ecx_mbxsend(ctx, slave, request, EC_TIMEOUTTXM) > 0;
  };
  if (!sendRead()) {
    std::cout << "   the READ request was not written\n";
    return "FAIL";
  }
  const auto start = Clock::now();
  const auto since = [&start] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  };
  // A data packet shorter than the mailbox allows is the last one.
  const int maxData = ctx->slavelist[slave].mbx_l - 12;
  bool sawBusy = false;
  int bytesRead = 0;
  while (Clock::now() - start < std::chrono::seconds(waitS)) {
    ec_mbxbuft* in = nullptr;
    const int wkc = ecx_mbxreceive(ctx, slave, &in, kReceiveTimeoutUs);
    ec_errort err{};
    while (ecx_poperror(ctx, &err)) {
      std::cout << std::format("   {:5} ms  error type {}, code 0x{:04X}\n", since(),
                               static_cast<int>(err.Etype), err.ErrorCode);
    }
    if (wkc <= 0 || in == nullptr) {
      continue;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(in);
    const auto length = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    const int type = bytes[5] & 0x0F;
    const uint8_t opcode = bytes[6];
    const uint32_t word = word32(&bytes[8]);
    const uint16_t done = static_cast<uint16_t>(bytes[8] | (bytes[9] << 8));
    const uint16_t entire = static_cast<uint16_t>(bytes[10] | (bytes[11] << 8));
    ecx_dropmbx(ctx, in);
    if (type != ECT_MBXT_FOE) {
      std::cout << std::format("   {:5} ms  mailbox type {}, {} bytes\n", since(), type, length);
      continue;
    }
    if (opcode == kFoeOpcodeBusy) {
      sawBusy = true;
      std::cout << std::format("   {:5} ms  FoE BUSY {}/{}\n", since(), done, entire);
      if (afterBusy == AfterBusy::kAbort || afterBusy == AfterBusy::kAbortReread) {
        mm::comm::soem::abortFoeTransfer(ctx, slave);
        std::cout << std::format("   {:5} ms  sent the FoE abort\n", since());
      }
      if (afterBusy == AfterBusy::kAbort) {
        return "ABORTED AFTER BUSY";
      }
      if (afterBusy == AfterBusy::kAck0) {
        ec_mbxbuft* ack = makeFoeRequest(ctx, slave, kFoeOpcodeAck, 0, {});
        if (ack == nullptr || ecx_mbxsend(ctx, slave, ack, EC_TIMEOUTTXM) <= 0) {
          std::cout << "   the ACK was not written\n";
          return "FAIL";
        }
        std::cout << std::format("   {:5} ms  sent FoE ACK 0\n", since());
      }
      if (afterBusy == AfterBusy::kAbortReread) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (afterBusy == AfterBusy::kReread || afterBusy == AfterBusy::kAbortReread) {
        if (!sendRead()) {
          std::cout << "   the READ request was not written\n";
          return "FAIL";
        }
        std::cout << std::format("   {:5} ms  sent the READ request again\n", since());
      }
      continue;
    }
    if (opcode == kFoeOpcodeError) {
      std::cout << std::format("   {:5} ms  FoE ERROR 0x{:08X}\n", since(), word);
      if (untilFile) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!sendRead()) {
          std::cout << "   the READ request was not written\n";
          return "FAIL";
        }
        std::cout << std::format("   {:5} ms  sent the READ request again\n", since());
        continue;
      }
      return sawBusy ? "NO DATA AFTER BUSY" : "FAIL";
    }
    if (opcode != kFoeOpcodeData) {
      std::cout << std::format("   {:5} ms  FoE opcode 0x{:02X}\n", since(), opcode);
      continue;
    }
    const int data = length - 6;
    bytesRead += data;
    std::cout << std::format("   {:5} ms  FoE DATA packet {}, {} bytes\n", since(), word, data);
    ec_mbxbuft* ack = makeFoeRequest(ctx, slave, kFoeOpcodeAck, word, {});
    if (ack == nullptr || ecx_mbxsend(ctx, slave, ack, EC_TIMEOUTTXM) <= 0) {
      std::cout << "   the ACK was not written\n";
      return "FAIL";
    }
    if (data < maxData) {
      std::cout << std::format("   {:5} ms  file complete, {} bytes\n", since(), bytesRead);
      return sawBusy ? "DATA AFTER BUSY" : "NO BUSY";
    }
  }
  std::cout << std::format("   {:5} ms  no file within {} s\n", since(), waitS);
  // Leaves no transfer open on the drive for the next round.
  mm::comm::soem::abortFoeTransfer(ctx, slave);
  return sawBusy ? "NO DATA AFTER BUSY" : "FAIL";
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
  // Without this, ecx_config_init asks every slave for PRE-OP on its own.
  ctx->manualstatechange = 1;
  const int found = ecx_config_init(ctx.get());
  if (found < options.slave) {
    std::cerr << std::format("found {} slave(s), slave {} is not on the bus\n", found,
                             options.slave);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const uint16_t slave = options.slave;
  const ec_slavet& device = ctx->slavelist[slave];
  std::cout << std::format("slave {} ({}, product 0x{:08X}, revision 0x{:08X})\n", slave,
                           device.name, device.eep_id, device.eep_rev);

  const auto programStart = Clock::now();
  const auto elapsed = [&programStart] {
    return std::chrono::duration<double>(Clock::now() - programStart).count();
  };
  const auto enter = [&](EtherCatState state) {
    if (state != EtherCatState::Init) {
      mm::comm::soem::updateMailboxSyncManagers(ctx.get(), slave, state);
    }
    if (requestState(ctx.get(), slave, state)) {
      return true;
    }
    std::cout << std::format("{:7.2f} s  the slave did not reach state 0x{:02X}\n", elapsed(),
                             static_cast<int>(state));
    return false;
  };

  bool passed = true;
  for (int round = 1; round <= options.rounds; ++round) {
    bool entered = enter(EtherCatState::Init);
    if (entered && options.viaPreOp) {
      entered = enter(EtherCatState::PreOp) && enter(EtherCatState::Init);
    }
    entered = entered && enter(options.state);
    if (!entered) {
      passed = false;
      break;
    }
    if (options.delayMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.delayMs));
    }
    std::cout << std::format("{:7.2f} s  round {}: read '{}' {} ms after the state\n", elapsed(),
                             round, options.file, options.delayMs);
    const std::string verdict =
        readAndListen(ctx.get(), slave, options.file, options.waitS, options.afterBusy,
                      options.untilFile);
    std::cout << "   " << verdict << "\n";
    passed = passed && (verdict == "DATA AFTER BUSY" || verdict == "NO BUSY");
    if (options.gapMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.gapMs));
    }
  }

  // The check that matters after an abort: a drive that hangs after a BUSY no longer leaves BOOT.
  if (requestState(ctx.get(), slave, EtherCatState::Init)) {
    std::cout << "INIT reached\n";
  } else {
    std::cout << "INIT NOT REACHED\n";
    passed = false;
  }
  mm::comm::soem::updateMailboxSyncManagers(ctx.get(), slave, EtherCatState::PreOp);
  requestState(ctx.get(), slave, EtherCatState::PreOp);
  ecx_close(ctx.get());
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
