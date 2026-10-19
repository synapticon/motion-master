// Shows on a real drive that a SOMANET drive stops taking mailbox requests after it receives one
// request twice with the same mailbox counter, and that INIT followed by PRE-OP frees it.
//
// The program talks to the drive through SOEM directly, not through SoemFieldbusDriver, because it
// has to write one request twice on purpose. The driver never does that.
//
// Steps:
//   1. Put the drive into PRE-OP and read 0x1000:00. This shows that the drive answers.
//   2. Write an SDO upload request of 0x1000:00 into the receive mailbox and read the answer.
//   3. Write the same request again, with the same counter.
//   4. Read the SyncManager 0 status. The mailbox-full bit stays set, because the drive does not
//      take the request out of the mailbox.
//   5. Read 0x1000:00. It fails, because the mailbox is full.
//   6. Put the drive into INIT and then PRE-OP, and read 0x1000:00 again. It works.
//
// With --boot, the same test runs against the bootloader, which has its own copy of the EtherCAT
// layer. The drive goes into BOOT, and the request is an FoE read of a file. The bootloader does
// not answer a second read while the first one is open, so the result tells three cases apart: the
// duplicate locks the mailbox, the bootloader discards it, or the bootloader handles it as a new
// request. The program then aborts the read, and puts the drive into INIT and PRE-OP.
//
// The drive is left in PRE-OP.

#include <soem/soem.h>

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

#include "comm/fieldbus_driver.h"
#include "comm/soem_mailbox.h"

namespace {

using mm::comm::EtherCatState;

constexpr int kStateTimeoutUs = 10'000'000;
// Bit 3 of the SyncManager status: the mailbox holds a message that the slave has not taken.
constexpr uint8_t kMailboxFull = 0x08;

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  bool boot = false;
  std::string file = ".hardware_description";
};

void printUsage() {
  std::cerr << "usage: mailbox_duplicate --ifname <interface> [--slave <position>] [--boot] "
               "[--file <name>]\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--boot") {
      options.boot = true;
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
    } else {
      return false;
    }
  }
  return !options.ifname.empty();
}

using MailboxBuffer = std::array<uint8_t, sizeof(ec_mbxbuft)>;

OSAL_PACKED_BEGIN
struct OSAL_PACKED SdoUploadRequest {
  ec_mbxheadert header;
  uint16 canOpen;
  uint8 command;
  uint16 index;
  uint8 subIndex;
  uint32 data;
};
OSAL_PACKED_END

// Builds an SDO upload request of 0x1000:00 that carries the given mailbox counter.
MailboxBuffer makeUploadRequest(uint8_t counter) {
  MailboxBuffer buffer{};
  auto* request = reinterpret_cast<SdoUploadRequest*>(buffer.data());
  request->header.length = htoes(sizeof(SdoUploadRequest) - sizeof(ec_mbxheadert));
  request->header.address = htoes(0x0000);
  request->header.priority = 0x00;
  request->header.mbxtype = ECT_MBXT_COE + MBX_HDR_SET_CNT(counter);
  request->canOpen = htoes(0x000 + (ECT_COES_SDOREQ << 12));
  request->command = ECT_SDO_UP_REQ;
  request->index = htoes(0x1000);
  request->subIndex = 0x00;
  return buffer;
}

// Builds an FoE READ request of a file that carries the given mailbox counter.
MailboxBuffer makeFoeReadRequest(uint8_t counter, std::string_view file) {
  MailboxBuffer buffer{};
  // FoE header: opcode, reserved byte, 32-bit password. The file name follows it.
  const auto length = static_cast<uint16_t>(6 + file.size());
  buffer[0] = static_cast<uint8_t>(length & 0xFF);
  buffer[1] = static_cast<uint8_t>(length >> 8);
  buffer[5] = static_cast<uint8_t>(ECT_MBXT_FOE + MBX_HDR_SET_CNT(counter));
  buffer[6] = ECT_FOE_READ;
  for (size_t i = 0; i < file.size(); ++i) {
    buffer[12 + i] = static_cast<uint8_t>(file[i]);
  }
  return buffer;
}

// Writes the request into the receive mailbox with one datagram, so it is sent exactly once.
// Returns the working counter: 1 when the slave took the write, 0 when the mailbox was full.
int writeOnce(ecx_contextt* ctx, uint16_t slave, MailboxBuffer& buffer) {
  const ec_slavet& s = ctx->slavelist[slave];
  return ecx_FPWR(&ctx->port, s.configadr, s.mbx_wo, s.mbx_l, buffer.data(), EC_TIMEOUTRET);
}

// Waits for an answer in the send mailbox and drops it. Returns the working counter.
int receive(ecx_contextt* ctx, uint16_t slave, int timeoutUs) {
  ec_mbxbuft* in = nullptr;
  const int wkc = ecx_mbxreceive(ctx, slave, &in, timeoutUs);
  if (in != nullptr) {
    ecx_dropmbx(ctx, in);
  }
  return wkc;
}

// Returns the SyncManager 0 status register, or -1 when the read fails.
int sm0Status(ecx_contextt* ctx, uint16_t slave) {
  uint8_t status = 0;
  if (ecx_FPRD(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0STAT, sizeof(status),
               &status, EC_TIMEOUTRET) <= 0) {
    return -1;
  }
  return status;
}

bool sm0Full(int status) { return status >= 0 && (status & kMailboxFull) != 0; }

// Reads 0x1000:00 the way the driver does. Returns the working counter.
int readDeviceType(ecx_contextt* ctx, uint16_t slave) {
  uint32_t value = 0;
  int size = sizeof(value);
  const int wkc = ecx_SDOread(ctx, slave, 0x1000, 0x00, FALSE, &size, &value, EC_TIMEOUTRXM);
  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
  }
  if (wkc > 0) {
    std::cout << std::format(" (0x{:08X})", value);
  }
  return wkc;
}

bool requestState(ecx_contextt* ctx, uint16_t slave, EtherCatState state) {
  const auto raw = static_cast<uint16_t>(state);
  ctx->slavelist[slave].state = raw;
  ecx_writestate(ctx, slave);
  return ecx_statecheck(ctx, slave, raw, kStateTimeoutUs) == raw;
}

// Sends one request with counter 0. The firmware never ignores counter 0 and stores it as the last
// counter, so the next request is accepted whatever its counter is.
bool resetCounter(ecx_contextt* ctx, uint16_t slave) {
  mm::comm::soem::drainMailbox(ctx, slave);
  MailboxBuffer request = makeUploadRequest(0);
  return writeOnce(ctx, slave, request) > 0 && receive(ctx, slave, EC_TIMEOUTRXM) > 0;
}

// Returns the working counter of one mailbox answer and the errors SOEM queued for it.
int receiveAndDescribe(ecx_contextt* ctx, uint16_t slave, int timeoutUs, std::string& errors) {
  const int wkc = receive(ctx, slave, timeoutUs);
  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
    if (err.Etype == EC_ERR_TYPE_MBX_ERROR) {
      errors += std::format(" mailbox error 0x{:04X}", err.ErrorCode);
    } else if (err.Etype == EC_ERR_TYPE_FOE_ERROR) {
      errors += std::format(" FoE error 0x{:08X}", err.AbortCode);
    } else {
      errors += std::format(" error type {}", static_cast<int>(err.Etype));
    }
  }
  return wkc;
}

int runBoot(ecx_contextt* ctx, uint16_t slave, const std::string& file) {
  if (!requestState(ctx, slave, EtherCatState::Init)) {
    std::cerr << "the slave did not reach INIT\n";
    return EXIT_FAILURE;
  }
  mm::comm::soem::updateMailboxSyncManagers(ctx, slave, EtherCatState::Boot);
  if (!requestState(ctx, slave, EtherCatState::Boot)) {
    std::cerr << "the slave did not reach BOOT\n";
    return EXIT_FAILURE;
  }
  std::cout << std::format("slave {} is in BOOT\n", slave);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  mm::comm::soem::drainMailbox(ctx, slave);

  const uint8_t counter = ec_nextmbxcnt(ctx->slavelist[slave].mbx_cnt);
  ctx->slavelist[slave].mbx_cnt = counter;
  MailboxBuffer request = makeFoeReadRequest(counter, file);

  std::string errors;
  const int firstWrite = writeOnce(ctx, slave, request);
  const int firstAnswer = receiveAndDescribe(ctx, slave, 6'000'000, errors);
  std::cout << std::format("1. FoE read of {} with counter {}: write wkc {}, answer wkc {}{}\n",
                           file, counter, firstWrite, firstAnswer, errors);
  if (firstWrite <= 0 || firstAnswer <= 0) {
    std::cout << "   FAIL: the bootloader did not answer the first copy, so the test cannot run\n";
    return EXIT_FAILURE;
  }

  errors.clear();
  const int secondWrite = writeOnce(ctx, slave, request);
  const int secondAnswer = receiveAndDescribe(ctx, slave, 1'000'000, errors);
  const int status = sm0Status(ctx, slave);
  std::cout << std::format(
      "2. wrote the same request again: write wkc {}, answer wkc {}{}, SM0 status 0x{:02X}\n",
      secondWrite, secondAnswer, errors, status);
  if (sm0Full(status)) {
    std::cout << "   LOCKED: the bootloader did not take the second copy out of its mailbox\n";
  } else if (secondAnswer > 0 || !errors.empty()) {
    std::cout << "   HANDLED: the bootloader took the second copy and answered it\n";
  } else {
    std::cout << "   DISCARDED: the bootloader took the second copy and did not answer it\n";
  }

  if (!sm0Full(status)) {
    mm::comm::soem::abortFoeTransfer(ctx, slave);
  }
  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
  }
  const bool init = requestState(ctx, slave, EtherCatState::Init);
  mm::comm::soem::updateMailboxSyncManagers(ctx, slave, EtherCatState::PreOp);
  const bool preOp = init && requestState(ctx, slave, EtherCatState::PreOp);
  const bool counterReset = preOp && resetCounter(ctx, slave);
  std::cout << "3. INIT, then PRE-OP, then read 0x1000:00";
  const int read = counterReset ? readDeviceType(ctx, slave) : 0;
  std::cout << std::format(": INIT {}, PRE-OP {}, wkc {}\n", init, preOp, read);
  return read > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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
  // Without this, ecx_config_init asks every slave for PRE-OP, and in --boot mode a bootloader that
  // gets PRE-OP starts the application.
  ctx->manualstatechange = 1;
  const int found = ecx_config_init(ctx.get());
  if (found < options.slave) {
    std::cerr << std::format("found {} slave(s), slave {} is not on the bus\n", found,
                             options.slave);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const uint16_t slave = options.slave;

  if (options.boot) {
    const int result = runBoot(ctx.get(), slave, options.file);
    ecx_close(ctx.get());
    return result;
  }

  if (!requestState(ctx.get(), slave, EtherCatState::Init) ||
      !requestState(ctx.get(), slave, EtherCatState::PreOp)) {
    std::cerr << "the slave did not reach PRE-OP\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  std::cout << std::format("slave {} ({}) is in PRE-OP\n", slave, ctx->slavelist[slave].name);
  if (!resetCounter(ctx.get(), slave)) {
    std::cout << "FAIL: the slave did not answer a request with counter 0\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  bool passed = true;

  std::cout << "1. read 0x1000:00";
  const int baseline = readDeviceType(ctx.get(), slave);
  std::cout << std::format(": wkc {}\n", baseline);
  if (baseline <= 0) {
    std::cout << "   FAIL: the slave does not answer a normal read, so the test cannot run\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  // The next counter after the one SOEM used last, so the first copy is accepted.
  const uint8_t counter = ec_nextmbxcnt(ctx->slavelist[slave].mbx_cnt);
  ctx->slavelist[slave].mbx_cnt = counter;
  MailboxBuffer request = makeUploadRequest(counter);

  const int firstWrite = writeOnce(ctx.get(), slave, request);
  const int firstAnswer = receive(ctx.get(), slave, EC_TIMEOUTRXM);
  std::cout << std::format("2. wrote the request with counter {}: write wkc {}, answer wkc {}\n",
                           counter, firstWrite, firstAnswer);
  if (firstWrite <= 0 || firstAnswer <= 0) {
    std::cout << "   FAIL: the first copy was not answered, so the test cannot run\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  const int secondWrite = writeOnce(ctx.get(), slave, request);
  std::cout << std::format("3. wrote the same request again: write wkc {}\n", secondWrite);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const int status = sm0Status(ctx.get(), slave);
  const int secondAnswer = receive(ctx.get(), slave, 100'000);
  std::cout << std::format("4. after 500 ms: SM0 status 0x{:02X}, answer wkc {}\n", status,
                           secondAnswer);
  if (sm0Full(status)) {
    std::cout << "   the slave has not taken the second copy out of its receive mailbox\n";
  } else {
    std::cout << "   FAIL: expected the mailbox-full bit to stay set\n";
    passed = false;
  }

  std::cout << "5. read 0x1000:00";
  const int stuck = readDeviceType(ctx.get(), slave);
  const int stuckStatus = sm0Status(ctx.get(), slave);
  std::cout << std::format(": wkc {}, SM0 status 0x{:02X}\n", stuck, stuckStatus);
  if (stuck <= 0 && sm0Full(stuckStatus)) {
    std::cout << "   the slave refuses every new request\n";
  } else {
    std::cout << "   FAIL: expected the read to fail on a full mailbox\n";
    passed = false;
  }

  const bool recoveredState = requestState(ctx.get(), slave, EtherCatState::Init) &&
                              requestState(ctx.get(), slave, EtherCatState::PreOp);
  const bool recoveredCounter = recoveredState && resetCounter(ctx.get(), slave);
  std::cout << "6. INIT, then PRE-OP, then read 0x1000:00";
  const int recovered = recoveredCounter ? readDeviceType(ctx.get(), slave) : 0;
  std::cout << std::format(": wkc {}, SM0 status 0x{:02X}\n", recovered,
                           sm0Status(ctx.get(), slave));
  if (recovered > 0) {
    std::cout << "   INIT freed the mailbox\n";
  } else {
    std::cout << "   FAIL: expected the read to work after INIT\n";
    passed = false;
  }

  ecx_close(ctx.get());
  std::cout << (passed ? "PASS\n" : "FAIL\n");
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
