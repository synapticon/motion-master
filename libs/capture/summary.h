#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>

#include "capture/pcap.h"

/// @file
/// @brief How late the frames of a capture came back, in the bands of the waits the port uses.

namespace mm::capture {

/// @brief The counts for one kind of frame.
struct KindSummary {
  uint32_t sent = 0;       ///< Frames the master sent. A frame sent again counts once.
  uint32_t resent = 0;     ///< Frames sent again with the same index and header while the first
                           ///< copy had no answer yet. A register datagram goes out again every
                           ///< 2 ms, so a late one shows here as well as in a band.
  uint32_t returned = 0;   ///< Sent frames that came back.
  uint32_t lost = 0;       ///< Sent frames with no return before the capture ended.
  uint32_t over2Ms = 0;    ///< Returns later than 2 ms after the first send.
  uint32_t over20Ms = 0;   ///< Returns later than 20 ms. Counted in over2Ms as well.
  uint32_t over100Ms = 0;  ///< Returns later than 100 ms. Counted in the two above as well.
  int64_t latestUs = 0;    ///< The latest return, in microseconds after its first send.
};

/// @brief The summary of one capture.
///
/// A frame is matched to its return by the rule the SOEM port uses: the index, the command, the
/// offset and the length of its first datagram, and its position for every command the slaves
/// do not count up. The first later frame from the slaves with that header is the return. The
/// kind is the first datagram's command: a logical read or write is the cyclic frame of the RT
/// loop, everything else is the control plane. The bands are the three waits the port uses, and
/// they are cumulative.
struct Summary {
  uint64_t frames = 0;            ///< Every record in the capture.
  uint64_t notEtherCat = 0;       ///< Records that are not an EtherCAT command frame.
  uint64_t unmatchedReturns = 0;  ///< Frames from the slaves with no send to match, such as the
                                  ///< second answer to a frame sent twice.
  int64_t firstNs = 0;            ///< Timestamp of the first record.
  int64_t lastNs = 0;             ///< Timestamp of the last record.
  KindSummary control;            ///< Mailbox and register datagrams.
  KindSummary cyclic;             ///< The process-data frame.
};

/// @brief Summarises @p records, which must be in time order.
Summary summarise(std::span<const Record> records);

/// @brief Serialises a summary, camelCase keys, times in microseconds.
void to_json(nlohmann::json& j, const Summary& s);

}  // namespace mm::capture
