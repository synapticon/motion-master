// Shows on a real drive what the application firmware of a SOMANET drive does when the master
// gives up on an FoE read part-way through and starts a new one.
//
// The program talks to the drive through SOEM directly, not through SoemFieldbusDriver, because it
// has to stop in the middle of a read on purpose. The driver never does that: it gives up only
// when a frame does not come back.
//
// Steps:
//   1. Put the drive into PRE-OP and read the file in full. This gives the size the drive serves.
//   2. Start a new read of the same file, acknowledge the first packets, and stop with one data
//      packet unacknowledged. That is where the driver stands when the frame of a data packet
//      comes back too late.
//   3. Send the FoE abort, the FoE ERROR request the driver sends after a failed transfer. With
//      --no-abort, send nothing.
//   4. Wait, then read the file in full. The driver waits this long before it asks again.
//   5. Wait 3 s, and read the file in full once more.
//
// The program ends with WEDGED when the last read gets FoE error 0x800B "File System is busy",
// with WRONG BYTES when it gets a file whose bytes are not those of step 1, with OK when it gets
// the file of step 1, and with FAIL for anything else. A wedged drive needs a
// power cycle. The drive is left in PRE-OP.
//
// The sequence of issue #131, seen on a Circulo under late frames. Measured on a Circulo with
// firmware v5.6.11: with the last packet of the file unacknowledged (--packets 4 for a file of five
// packets) the drive is WEDGED; with a packet in the middle of the file unacknowledged it is OK. So
// the state that matters is the drive's "last ACK pending", and --packets must leave the last
// packet unacknowledged to reproduce it.

#include <soem/soem.h>

#include <array>
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
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/soem_mailbox.h"
#include "comm/vendor_ids.h"

namespace {

using mm::comm::EtherCatState;

constexpr int kReadTimeoutUs = 6'000'000;
constexpr int kStateTimeoutUs = 10'000'000;
constexpr uint8_t kFoeOpcodeRead = 0x01;
constexpr uint8_t kFoeOpcodeData = 0x03;
constexpr uint8_t kFoeOpcodeAck = 0x04;
constexpr uint8_t kFoeOpcodeError = 0x05;
constexpr uint32_t kFoeErrorFileSystemBusy = 0x800B;

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  std::string file = "config.csv";
  int packets = 2;
  int pauseMs = 200;
  bool abort = true;
};

void printUsage() {
  std::cerr << "usage: foe_read_abort --ifname <interface> [--slave <position>] [--file <name>]\n"
               "                      [--packets <acknowledged>] [--pause <ms>] [--no-abort]\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--no-abort") {
      options.abort = false;
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
    } else if (arg == "--packets") {
      options.packets = std::stoi(argv[++i]);
    } else if (arg == "--pause") {
      options.pauseMs = std::stoi(argv[++i]);
    } else {
      return false;
    }
  }
  return !options.ifname.empty() && options.packets >= 0;
}

struct ReadResult {
  int wkc = 0;
  int size = 0;
  std::chrono::milliseconds duration{0};
  std::vector<std::string> errors;
  uint32_t foeErrorCode = 0;
  std::vector<uint8_t> bytes;
};

// Reads one file in full and collects the errors SOEM queued for the read.
ReadResult readFile(ecx_contextt* ctx, uint16_t slave, const std::string& file) {
  std::vector<uint8_t> buffer(1024 * 1024);
  ReadResult result;
  result.size = static_cast<int>(buffer.size());
  std::string name = file;  // ecx_FOEread takes non-const char*
  const auto start = std::chrono::steady_clock::now();
  result.wkc = ecx_FOEread(ctx, slave, name.data(), 0, &result.size, buffer.data(), kReadTimeoutUs);
  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  if (result.wkc > 0) {
    result.bytes.assign(buffer.begin(), buffer.begin() + result.size);
  }
  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
    if (err.Etype == EC_ERR_TYPE_MBX_ERROR) {
      result.errors.push_back(std::format("mailbox error 0x{:04X}", err.ErrorCode));
    } else if (err.Etype == EC_ERR_TYPE_FOE_ERROR) {
      result.foeErrorCode = static_cast<uint32_t>(err.AbortCode);
      result.errors.push_back(std::format("FoE error 0x{:04X} \"{}\"", result.foeErrorCode,
                                          ctx->FOEerrortext[slave]));
    } else {
      result.errors.push_back(std::format("error type {}", static_cast<int>(err.Etype)));
    }
  }
  return result;
}

// Prints one read. When a baseline is given, says whether the bytes are the baseline's, because a
// drive can serve a file of the right size with the wrong content: the first packet as zeros.
void printRead(std::string_view step, const ReadResult& result,
               const ReadResult* baseline = nullptr) {
  std::cout << std::format("{}: wkc {}, {} ms", step, result.wkc, result.duration.count());
  if (result.wkc > 0) {
    std::cout << std::format(", {} bytes", result.size);
    if (baseline != nullptr) {
      std::cout << (result.bytes == baseline->bytes ? ", same bytes" : ", WRONG BYTES");
    }
  }
  for (const std::string& error : result.errors) {
    std::cout << ", " << error;
  }
  std::cout << "\n";
}

bool requestState(ecx_contextt* ctx, uint16_t slave, EtherCatState state) {
  const auto raw = static_cast<uint16_t>(state);
  ctx->slavelist[slave].state = raw;
  ecx_writestate(ctx, slave);
  return ecx_statecheck(ctx, slave, raw, kStateTimeoutUs) == raw;
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

// Starts a read and acknowledges `packets` data packets, then returns with the next data packet
// received and not acknowledged. Returns false when the drive does not play along.
bool readPartOfFile(ecx_contextt* ctx, uint16_t slave, const std::string& file, int packets) {
  mm::comm::soem::drainMailbox(ctx, slave);
  ec_mbxbuft* request = makeFoeRequest(ctx, slave, kFoeOpcodeRead, 0, file);
  if (request == nullptr || ecx_mbxsend(ctx, slave, request, EC_TIMEOUTTXM) <= 0) {
    std::cout << "   the READ request was not written\n";
    return false;
  }
  for (int expected = 1; expected <= packets + 1; ++expected) {
    ec_mbxbuft* in = nullptr;
    const int wkc = ecx_mbxreceive(ctx, slave, &in, kReadTimeoutUs);
    if (wkc <= 0 || in == nullptr) {
      std::cout << std::format("   no data packet {} (wkc {})\n", expected, wkc);
      return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(in);
    const uint8_t opcode = bytes[6];
    const uint32_t packet =
        static_cast<uint32_t>(bytes[8]) | (static_cast<uint32_t>(bytes[9]) << 8) |
        (static_cast<uint32_t>(bytes[10]) << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
    const uint16_t length = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    ecx_dropmbx(ctx, in);
    if (opcode != kFoeOpcodeData || packet != static_cast<uint32_t>(expected)) {
      std::cout << std::format("   expected data packet {}, got opcode 0x{:02X} packet {}\n",
                               expected, opcode, packet);
      return false;
    }
    std::cout << std::format("   data packet {} ({} bytes)", packet, length - 6);
    if (expected > packets) {
      std::cout << ", not acknowledged\n";
      return true;
    }
    ec_mbxbuft* ack = makeFoeRequest(ctx, slave, kFoeOpcodeAck, packet, {});
    if (ack == nullptr || ecx_mbxsend(ctx, slave, ack, EC_TIMEOUTTXM) <= 0) {
      std::cout << ", the ACK was not written\n";
      return false;
    }
    std::cout << ", acknowledged\n";
  }
  return true;
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
  // The same rule as the driver: a SOMANET device gets each mailbox request once.
  ctx->slavelist[slave].mbxwriteonce = mm::comm::isSomanetDevice(ctx->slavelist[slave].eep_man);

  if (!requestState(ctx.get(), slave, EtherCatState::Init) ||
      !requestState(ctx.get(), slave, EtherCatState::PreOp)) {
    std::cerr << "the slave did not reach PRE-OP\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const ec_slavet& device = ctx->slavelist[slave];
  std::cout << std::format("slave {} ({}, product 0x{:08X}, revision 0x{:08X}) is in PRE-OP\n",
                           slave, device.name, device.eep_id, device.eep_rev);

  const ReadResult baseline = readFile(ctx.get(), slave, options.file);
  printRead("1. read", baseline);
  if (baseline.wkc <= 0) {
    std::cout << "   FAIL: the drive does not serve the file, so the test cannot run\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  std::cout << std::format("2. read again, acknowledge {} packet(s), stop\n", options.packets);
  if (!readPartOfFile(ctx.get(), slave, options.file, options.packets)) {
    std::cout << "   FAIL: the partial read did not get that far\n";
    requestState(ctx.get(), slave, EtherCatState::Init);
    requestState(ctx.get(), slave, EtherCatState::PreOp);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  if (options.abort) {
    mm::comm::soem::abortFoeTransfer(ctx.get(), slave);
    std::cout << "3. sent the FoE abort\n";
  } else {
    std::cout << "3. sent no abort\n";
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(options.pauseMs));
  const ReadResult first = readFile(ctx.get(), slave, options.file);
  printRead(std::format("4. read after {} ms", options.pauseMs), first, &baseline);

  std::this_thread::sleep_for(std::chrono::seconds(3));
  const ReadResult second = readFile(ctx.get(), slave, options.file);
  printRead("5. read after 3 s", second, &baseline);

  std::string verdict = "FAIL";
  if (second.wkc > 0 && second.bytes == baseline.bytes) {
    verdict = "OK";
  } else if (second.wkc > 0) {
    verdict = "WRONG BYTES";
  } else if (second.foeErrorCode == kFoeErrorFileSystemBusy) {
    verdict = "WEDGED";
  }
  ecx_close(ctx.get());
  std::cout << verdict << "\n";
  return verdict == "OK" ? EXIT_SUCCESS : EXIT_FAILURE;
}
