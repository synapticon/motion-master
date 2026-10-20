// Shows on a real drive what the drive does with a mailbox request that carries counter 0.
//
// ETG.1000.4 Table 29 reserves counter 0. The XMOS EtherCAT stack of a SOMANET drive accepts it
// and stores it as the last counter. What the netX stack of an Integro does with it is what this
// program measures. Issue #113: an Integro left many requests unanswered after the master started
// to send counter 0 after a mailbox write that got no frame back.
//
// The program talks to the drive through SOEM directly, not through SoemFieldbusDriver, because it
// sets the counter of each request itself.
//
// Every request is an SDO upload of 0x1000:00. A request is written with one datagram, and the
// answer is awaited for EC_TIMEOUTRXM. When no answer comes, the SyncManager 0 status tells whether
// the request is still in the mailbox (STUCK) or the drive took it and did not answer (DROPPED).
// A stuck mailbox is freed with INIT followed by PRE-OP before the next round.
//
// Patterns, each run for --rounds rounds:
//   after   a request with the next counter, then counter 0, then counter 1. This is the sequence
//           Motion Master 6.0.0-alpha.92 sends after a write with no frame back. The first counter
//           of each round moves through 1..7, so every preceding counter is covered.
//   double  a request with the next counter, then counter 0 twice, then the next counter.
//   first   INIT, PRE-OP, then counter 0 as the first request, then the next counter.
//   init    two requests with the next counters, INIT, PRE-OP, counter 0, then the same two
//           counters again. This shows whether INIT clears what the drive remembers.
//   skip    counters c, c+2, c+1, c+3, c+2, c+3, c+4. A drive that checks the sequence drops c+2
//           and the first c+3. A drive that remembers the last two accepted counters drops the
//           second c+3.
//   all     every pattern in that order. This is the default.
//
// The drive is left in PRE-OP.

#include <soem/soem.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "comm/fieldbus_driver.h"

namespace {

using mm::comm::EtherCatState;

constexpr int kStateTimeoutUs = 10'000'000;
// Bit 3 of the SyncManager status: the mailbox holds a message that the slave has not taken.
constexpr uint8_t kMailboxFull = 0x08;

struct Options {
  std::string ifname;
  uint16_t slave = 1;
  int rounds = 50;
  std::string pattern = "all";
};

void printUsage() {
  std::cerr << "usage: mailbox_counter0 --ifname <interface> [--slave <position>] [--rounds <n>] "
               "[--pattern after|double|first|init|skip|all]\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view arg = argv[i];
    if (arg == "--ifname") {
      options.ifname = argv[i + 1];
    } else if (arg == "--slave") {
      options.slave = static_cast<uint16_t>(std::stoi(argv[i + 1]));
    } else if (arg == "--rounds") {
      options.rounds = std::stoi(argv[i + 1]);
    } else if (arg == "--pattern") {
      options.pattern = argv[i + 1];
    } else {
      return false;
    }
  }
  const bool knownPattern = options.pattern == "after" || options.pattern == "double" ||
                            options.pattern == "first" || options.pattern == "init" ||
                            options.pattern == "skip" || options.pattern == "all";
  return !options.ifname.empty() && options.rounds > 0 && knownPattern;
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

enum class Outcome { Answered, Dropped, Stuck, Refused };

std::string_view describe(Outcome outcome) {
  switch (outcome) {
    case Outcome::Answered:
      return "answered";
    case Outcome::Dropped:
      return "dropped";
    case Outcome::Stuck:
      return "stuck";
    case Outcome::Refused:
      return "refused";
  }
  return "?";
}

// One request of a pattern, named by its place in the round.
struct Step {
  std::string name;
  uint8_t counter;
  Outcome outcome;
  std::chrono::microseconds latency;
};

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

bool requestState(ecx_contextt* ctx, uint16_t slave, EtherCatState state) {
  const auto raw = static_cast<uint16_t>(state);
  ctx->slavelist[slave].state = raw;
  ecx_writestate(ctx, slave);
  return ecx_statecheck(ctx, slave, raw, kStateTimeoutUs) == raw;
}

void popErrors(ecx_contextt* ctx) {
  ec_errort err{};
  while (ecx_poperror(ctx, &err)) {
  }
}

// Writes one request with the given counter and waits for its answer. The outcome tells what the
// drive did with it.
Step sendRequest(ecx_contextt* ctx, uint16_t slave, std::string name, uint8_t counter) {
  Step step{std::move(name), counter, Outcome::Refused, {}};
  const ec_slavet& s = ctx->slavelist[slave];
  MailboxBuffer request = makeUploadRequest(counter);
  const auto start = std::chrono::steady_clock::now();
  const int wrote =
      ecx_FPWR(&ctx->port, s.configadr, s.mbx_wo, s.mbx_l, request.data(), EC_TIMEOUTRET3);
  if (wrote <= 0) {
    return step;
  }
  ec_mbxbuft* in = nullptr;
  const int wkc = ecx_mbxreceive(ctx, slave, &in, EC_TIMEOUTRXM);
  step.latency = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);
  if (in != nullptr) {
    ecx_dropmbx(ctx, in);
  }
  popErrors(ctx);
  if (wkc > 0) {
    step.outcome = Outcome::Answered;
  } else {
    step.outcome = sm0Full(sm0Status(ctx, slave)) ? Outcome::Stuck : Outcome::Dropped;
  }
  return step;
}

// Gives SOEM's counter for the slave its next value and returns it.
uint8_t nextCounter(ecx_contextt* ctx, uint16_t slave) {
  const uint8_t counter = ec_nextmbxcnt(ctx->slavelist[slave].mbx_cnt);
  ctx->slavelist[slave].mbx_cnt = counter;
  return counter;
}

bool cycleToPreOp(ecx_contextt* ctx, uint16_t slave) {
  return requestState(ctx, slave, EtherCatState::Init) &&
         requestState(ctx, slave, EtherCatState::PreOp);
}

// Frees a stuck mailbox, so the next round starts from a drive that answers.
bool recover(ecx_contextt* ctx, uint16_t slave, const std::vector<Step>& round) {
  const bool stuck =
      std::ranges::any_of(round, [](const Step& s) { return s.outcome == Outcome::Stuck; });
  if (!stuck) {
    return true;
  }
  std::cout << "   mailbox stuck, INIT then PRE-OP\n";
  return cycleToPreOp(ctx, slave);
}

std::vector<Step> runAfter(ecx_contextt* ctx, uint16_t slave) {
  std::vector<Step> round;
  round.push_back(sendRequest(ctx, slave, "before", nextCounter(ctx, slave)));
  round.push_back(sendRequest(ctx, slave, "zero", 0));
  // Motion Master 6.0.0-alpha.92 stores 0 after a counter-0 request, so the next counter is 1.
  ctx->slavelist[slave].mbx_cnt = 0;
  round.push_back(sendRequest(ctx, slave, "after", nextCounter(ctx, slave)));
  return round;
}

std::vector<Step> runDouble(ecx_contextt* ctx, uint16_t slave) {
  std::vector<Step> round;
  round.push_back(sendRequest(ctx, slave, "before", nextCounter(ctx, slave)));
  round.push_back(sendRequest(ctx, slave, "zero", 0));
  round.push_back(sendRequest(ctx, slave, "zero again", 0));
  round.push_back(sendRequest(ctx, slave, "after", nextCounter(ctx, slave)));
  return round;
}

std::vector<Step> runFirst(ecx_contextt* ctx, uint16_t slave) {
  std::vector<Step> round;
  if (!cycleToPreOp(ctx, slave)) {
    std::cout << "   the slave did not reach PRE-OP\n";
    return round;
  }
  round.push_back(sendRequest(ctx, slave, "zero first", 0));
  round.push_back(sendRequest(ctx, slave, "after", nextCounter(ctx, slave)));
  return round;
}

std::vector<Step> runInit(ecx_contextt* ctx, uint16_t slave) {
  std::vector<Step> round;
  const uint8_t first = nextCounter(ctx, slave);
  round.push_back(sendRequest(ctx, slave, "before 1", first));
  const uint8_t second = nextCounter(ctx, slave);
  round.push_back(sendRequest(ctx, slave, "before 2", second));
  if (!cycleToPreOp(ctx, slave)) {
    std::cout << "   the slave did not reach PRE-OP\n";
    return {};
  }
  round.push_back(sendRequest(ctx, slave, "zero", 0));
  round.push_back(sendRequest(ctx, slave, "same 1", first));
  round.push_back(sendRequest(ctx, slave, "same 2", second));
  return round;
}

std::vector<Step> runSkip(ecx_contextt* ctx, uint16_t slave) {
  std::vector<Step> round;
  const uint8_t c = nextCounter(ctx, slave);
  std::array<uint8_t, 5> next{};
  next[0] = ec_nextmbxcnt(c);
  for (size_t i = 1; i < next.size(); ++i) {
    next[i] = ec_nextmbxcnt(next[i - 1]);
  }
  round.push_back(sendRequest(ctx, slave, "c", c));
  round.push_back(sendRequest(ctx, slave, "c+2", next[1]));
  round.push_back(sendRequest(ctx, slave, "c+1", next[0]));
  round.push_back(sendRequest(ctx, slave, "c+3", next[2]));
  round.push_back(sendRequest(ctx, slave, "c+2 again", next[1]));
  round.push_back(sendRequest(ctx, slave, "c+3 again", next[2]));
  round.push_back(sendRequest(ctx, slave, "c+4", next[3]));
  ctx->slavelist[slave].mbx_cnt = next[3];
  return round;
}

struct Tally {
  std::map<Outcome, int> counts;
  std::vector<std::chrono::microseconds> answeredLatencies;
  // Preceding counter → outcomes of this step, to show whether the preceding counter matters.
  std::map<uint8_t, std::map<Outcome, int>> byPrecedingCounter;
};

void printTally(std::string_view pattern, const std::map<std::string, Tally>& tallies,
                const std::vector<std::string>& order) {
  std::cout << std::format("\n=== {} ===\n", pattern);
  for (const auto& name : order) {
    const Tally& tally = tallies.at(name);
    std::string counts;
    for (const auto& [outcome, count] : tally.counts) {
      counts += std::format(" {} {}", describe(outcome), count);
    }
    std::string median = "-";
    if (!tally.answeredLatencies.empty()) {
      std::vector<std::chrono::microseconds> sorted = tally.answeredLatencies;
      std::ranges::sort(sorted);
      median =
          std::format("{:.1f} ms", static_cast<double>(sorted[sorted.size() / 2].count()) / 1000.0);
    }
    std::cout << std::format("{:<12}{} | median answer {}\n", name, counts, median);
    bool mixed = false;
    for (const auto& [counter, outcomes] : tally.byPrecedingCounter) {
      if (outcomes.size() > 1) {
        mixed = true;
      }
    }
    if (tally.counts.size() > 1 || mixed) {
      for (const auto& [counter, outcomes] : tally.byPrecedingCounter) {
        std::string line;
        for (const auto& [outcome, count] : outcomes) {
          line += std::format(" {} {}", describe(outcome), count);
        }
        std::cout << std::format("             after counter {}:{}\n", counter, line);
      }
    }
  }
}

bool runPattern(ecx_contextt* ctx, uint16_t slave, std::string_view pattern, int rounds) {
  std::map<std::string, Tally> tallies;
  std::vector<std::string> order;
  for (int i = 0; i < rounds; ++i) {
    std::vector<Step> round;
    if (pattern == "after") {
      round = runAfter(ctx, slave);
    } else if (pattern == "double") {
      round = runDouble(ctx, slave);
    } else if (pattern == "init") {
      round = runInit(ctx, slave);
    } else if (pattern == "skip") {
      round = runSkip(ctx, slave);
    } else {
      round = runFirst(ctx, slave);
    }
    if (round.empty()) {
      return false;
    }
    std::string line = std::format("{} round {:>3}:", pattern, i + 1);
    uint8_t preceding = 0;
    bool first = true;
    for (const Step& step : round) {
      line += std::format(" {}({})={}", step.name, step.counter, describe(step.outcome));
      Tally& tally = tallies[step.name];
      if (std::ranges::find(order, step.name) == order.end()) {
        order.push_back(step.name);
      }
      tally.counts[step.outcome]++;
      if (step.outcome == Outcome::Answered) {
        tally.answeredLatencies.push_back(step.latency);
      }
      if (!first) {
        tally.byPrecedingCounter[preceding][step.outcome]++;
      }
      preceding = step.counter;
      first = false;
    }
    std::cout << line << "\n";
    if (!recover(ctx, slave, round)) {
      std::cout << "   the slave did not recover\n";
      return false;
    }
  }
  printTally(pattern, tallies, order);
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
  ctx->manualstatechange = 1;
  const int found = ecx_config_init(ctx.get());
  if (found < options.slave) {
    std::cerr << std::format("found {} slave(s), slave {} is not on the bus\n", found,
                             options.slave);
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const uint16_t slave = options.slave;
  if (!cycleToPreOp(ctx.get(), slave)) {
    std::cerr << "the slave did not reach PRE-OP\n";
    ecx_close(ctx.get());
    return EXIT_FAILURE;
  }
  const ec_slavet& device = ctx->slavelist[slave];
  std::cout << std::format("slave {} ({}, product 0x{:08X}, revision 0x{:08X}) is in PRE-OP\n",
                           slave, device.name, device.eep_id, device.eep_rev);

  std::vector<std::string> patterns;
  if (options.pattern == "all") {
    patterns = {"after", "double", "first", "init", "skip"};
  } else {
    patterns = {options.pattern};
  }
  bool ok = true;
  for (const auto& pattern : patterns) {
    ok = runPattern(ctx.get(), slave, pattern, options.rounds) && ok;
  }
  ecx_close(ctx.get());
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
