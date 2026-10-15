// Shows on a real drive that a SOMANET bootloader stays stuck after the master gives up on an FoE
// read, and that an FoE abort from the master frees it.
//
// The program talks to the drive through SOEM directly, not through SoemFieldbusDriver, because it
// has to give up on a read on purpose: it reads with a timeout of 0. The driver never does that,
// and it aborts every transfer it gives up on, so it cannot produce the stuck state.
//
// Steps:
//   1. Put the drive into BOOT and read the file with a normal timeout. This shows that the
//      bootloader answers.
//   2. Read the file with a timeout of 0. The master gives up before the bootloader answers.
//   3. Wait, and drain the late answer from the mailbox.
//   4. Read the file again. The bootloader is still in the read from step 2, so it answers with
//      mailbox error 0x0008.
//   5. Send an FoE abort.
//   6. Read the file again. It works.
//
// The drive is left in INIT. It needs a power cycle, or a transition to PRE-OP, to start its
// application again.

#include <soem/soem.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/soem_mailbox.h"

namespace {

using mm::comm::EtherCatState;

constexpr int kReadTimeoutUs = 10'000'000;
constexpr int kStateTimeoutUs = 10'000'000;

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  std::string file = ".hardware_description";
};

void printUsage() {
  std::cerr << "usage: foe_abort --ifname <interface> [--slave <position>] [--file <name>]\n";
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
    } else {
      return false;
    }
  }
  return !options.ifname.empty();
}

struct ReadResult {
  int wkc = 0;
  int size = 0;
  std::chrono::milliseconds duration{0};
  std::vector<std::string> errors;
  bool mailboxError0008 = false;
};

// Reads one file and collects the errors SOEM queued for the read.
ReadResult readFile(ecx_contextt* ctx, uint16_t slave, const std::string& file, int timeoutUs) {
  std::vector<uint8_t> buffer(1024 * 1024);
  ReadResult result;
  result.size = static_cast<int>(buffer.size());
  std::string name = file;  // ecx_FOEread takes non-const char*
  const auto start = std::chrono::steady_clock::now();
  result.wkc = ecx_FOEread(ctx, slave, name.data(), 0, &result.size, buffer.data(), timeoutUs);
  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
    if (err.Etype == EC_ERR_TYPE_MBX_ERROR) {
      result.errors.push_back(std::format("mailbox error 0x{:04X}", err.ErrorCode));
      if (err.ErrorCode == 0x0008) {
        result.mailboxError0008 = true;
      }
    } else if (err.Etype == EC_ERR_TYPE_FOE_ERROR) {
      result.errors.push_back(std::format("FoE error 0x{:08X}", err.AbortCode));
    } else {
      result.errors.push_back(std::format("error type {}", static_cast<int>(err.Etype)));
    }
  }
  return result;
}

void printRead(std::string_view step, const ReadResult& result) {
  std::cout << std::format("{}: wkc {}, {} ms", step, result.wkc, result.duration.count());
  if (result.wkc > 0) {
    std::cout << std::format(", {} bytes", result.size);
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
  // Without this, ecx_config_init asks every slave for PRE-OP, and a bootloader that gets PRE-OP
  // starts the application.
  ctx->manualstatechange = 1;
  const int found = ecx_config_init(ctx.get());
  if (found < options.slave) {
    std::cerr << std::format("found {} slave(s), slave {} is not on the bus\n", found,
                             options.slave);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const uint16_t slave = options.slave;

  if (!requestState(ctx.get(), slave, EtherCatState::Init)) {
    std::cerr << "the slave did not reach INIT\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  mm::comm::soem::updateMailboxSyncManagers(ctx.get(), slave, EtherCatState::Boot);
  if (!requestState(ctx.get(), slave, EtherCatState::Boot)) {
    std::cerr << "the slave did not reach BOOT\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  std::cout << "slave " << slave << " is in BOOT\n";
  std::this_thread::sleep_for(std::chrono::seconds(2));

  bool passed = true;

  const ReadResult baseline = readFile(ctx.get(), slave, options.file, kReadTimeoutUs);
  printRead("1. read", baseline);
  if (baseline.wkc <= 0) {
    std::cout << "   FAIL: the bootloader does not answer a normal read, so the test cannot run\n";
    requestState(ctx.get(), slave, EtherCatState::Init);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }

  const ReadResult abandoned = readFile(ctx.get(), slave, options.file, 0);
  printRead("2. read with timeout 0", abandoned);
  if (abandoned.wkc > 0) {
    std::cout << "   the bootloader answered within the timeout of 0, so no read was given up\n";
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));
  mm::comm::soem::drainMailbox(ctx.get(), slave);
  std::cout << "3. waited 1 s and drained the late answer\n";

  const ReadResult stuck = readFile(ctx.get(), slave, options.file, kReadTimeoutUs);
  printRead("4. read", stuck);
  if (stuck.wkc <= 0 && stuck.mailboxError0008) {
    std::cout << "   the bootloader is stuck in the read from step 2\n";
  } else {
    std::cout << "   FAIL: expected mailbox error 0x0008\n";
    passed = false;
  }

  mm::comm::soem::abortFoeTransfer(ctx.get(), slave);
  std::cout << "5. sent the FoE abort\n";

  const ReadResult recovered = readFile(ctx.get(), slave, options.file, kReadTimeoutUs);
  printRead("6. read", recovered);
  if (recovered.wkc > 0 && recovered.size == baseline.size) {
    std::cout << "   the abort freed the bootloader\n";
  } else {
    std::cout << "   FAIL: expected the same file as in step 1\n";
    passed = false;
  }

  requestState(ctx.get(), slave, EtherCatState::Init);
  ecx_close(ctx.get());
  std::cout << (passed ? "PASS\n" : "FAIL\n");
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
