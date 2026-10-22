#include "capture/summary.h"

#include <deque>
#include <map>
#include <nlohmann/json.hpp>
#include <tuple>

#include "capture/ethercat_frame.h"

namespace mm::capture {

namespace {

/// The header of a frame's first datagram, as the port matches it. The position is left out for
/// a command the slaves count up, because the return carries another one.
using MatchKey = std::tuple<uint8_t, uint8_t, uint16_t, uint16_t, uint16_t>;

MatchKey keyOf(const Datagram& d) {
  return {d.index, d.command, isPositionCountedUp(d.command) ? uint16_t{0} : d.adp, d.ado,
          d.length};
}

/// A send with the same key within this long of an unanswered one is the same datagram sent
/// again, not a new one. The port keeps the first copy's stamp, and so does this.
constexpr int64_t kResendWindowNs = 500'000'000;

void record(KindSummary& k, int64_t lateNs) {
  const int64_t lateUs = lateNs / 1000;
  k.returned++;
  if (lateUs > k.latestUs) {
    k.latestUs = lateUs;
  }
  if (lateUs > 2'000) {
    k.over2Ms++;
  }
  if (lateUs > 20'000) {
    k.over20Ms++;
  }
  if (lateUs > 100'000) {
    k.over100Ms++;
  }
}

}  // namespace

Summary summarise(std::span<const Record> records) {
  Summary s;
  // Every unanswered send, by its key, oldest first. A kind is fixed by the key's command.
  std::map<MatchKey, std::deque<int64_t>> pending;
  for (const Record& r : records) {
    s.frames++;
    if (s.frames == 1) {
      s.firstNs = r.timestampNs;
    }
    s.lastNs = r.timestampNs;
    const auto frame = parseEtherCatFrame(r.bytes);
    if (!frame) {
      s.notEtherCat++;
      continue;
    }
    const Datagram& first = frame->datagrams.front();
    KindSummary& kind = isLogicalCommand(first.command) ? s.cyclic : s.control;
    const MatchKey key = keyOf(first);
    std::deque<int64_t>& sends = pending[key];
    if (frame->fromMaster) {
      if (!sends.empty() && r.timestampNs - sends.back() < kResendWindowNs) {
        kind.resent++;
        continue;
      }
      kind.sent++;
      sends.push_back(r.timestampNs);
      continue;
    }
    if (sends.empty()) {
      s.unmatchedReturns++;
      continue;
    }
    record(kind, r.timestampNs - sends.front());
    sends.pop_front();
  }
  for (const auto& [key, sends] : pending) {
    KindSummary& kind = isLogicalCommand(std::get<1>(key)) ? s.cyclic : s.control;
    kind.lost += static_cast<uint32_t>(sends.size());
  }
  return s;
}

void to_json(nlohmann::json& j, const Summary& s) {
  const auto kind = [](const KindSummary& k) {
    return nlohmann::json{{"sent", k.sent},           {"resent", k.resent},
                          {"returned", k.returned},   {"lost", k.lost},
                          {"over2Ms", k.over2Ms},     {"over20Ms", k.over20Ms},
                          {"over100Ms", k.over100Ms}, {"latestUs", k.latestUs}};
  };
  j = {{"frames", s.frames},
       {"notEtherCat", s.notEtherCat},
       {"unmatchedReturns", s.unmatchedReturns},
       {"firstUs", s.firstNs / 1000},
       {"lastUs", s.lastNs / 1000},
       {"control", kind(s.control)},
       {"cyclic", kind(s.cyclic)}};
}

}  // namespace mm::capture
