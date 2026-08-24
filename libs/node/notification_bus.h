#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/// @file
/// @brief One off-RT thread that turns real-time state changes into WebSocket notifications.

namespace mm::node {

/// @brief The one topic every notification is published to.
///
/// One topic rather than one per source, so a client subscribes once and keeps receiving events
/// added after it was written. See @c NotificationBus.
inline constexpr std::string_view kNotificationTopic = "notifications";

/// @brief Polls a fixed set of sources and broadcasts a notification when one of them changes.
///
/// **Why a pump and not a callback.** A real-time task cannot call a WebSocket server: a publish
/// allocates, uWebSockets is only safely callable from its own loop, and both can block. But the RT
/// thread is the only code that sees a fault at the moment it happens. So the RT side stores plain
/// scalars into storage that already exists and moves on, and something off the RT thread notices
/// and formats. This is that something, written once instead of once per feature.
///
/// **The change signal is a version counter, not a flag.** A source's @c revision is a cheap read
/// of a counter the producer only ever increases; this class keeps a private last-seen copy and
/// never writes back. A counter has no clear step to lose an update against, and several bumps
/// between two polls coalesce into one message carrying the current state — which is the wanted
/// behaviour, not a compromise. Only inequality is used, so a producer that restarts its counter
/// at zero is reported rather than ignored.
///
/// The producer should bump its counter with @c std::memory_order_release *after* writing the
/// scalars that describe the change, and a @c render should read the counter with
/// @c std::memory_order_acquire before those scalars. That pairing is what stops a message pairing
/// a fresh counter with stale fields.
///
/// **This class names no server type.** The seam is a @c PublishFn wired at the composition root,
/// the same shape as @c MonitoringManager::setPublish.
///
/// **Every notification goes to one topic**, @c kNotificationTopic, so a client subscribes once and
/// receives every event — including ones added in a later version. A topic per source would look
/// tidier and would break that: uWebSockets 20.77 matches topic names exactly, with no wildcard, so
/// "give me all notifications" would not be expressible and a new source would silently reach no
/// existing client. Clients tell events apart by @c data.event, which is in the payload either way.
/// Per-topic fan-out is for the monitoring streams, where a client chooses what it pays for.
///
/// Not for per-cycle telemetry. Latency here is the poll cadence, which is right for a state
/// change and wrong for a signal trace — that is a monitoring subscription on the recorder ring.
class NotificationBus {
 public:
  /// @brief One feature's contribution: when to speak, and what to say.
  struct Source {
    /// Cheap read of the producer's version counter. Called every poll, so it must not lock or
    /// allocate. Only compared for inequality against the previous reading.
    std::function<std::uint64_t()> revision;
    /// Builds the @c data object of the message, off the RT thread. Called only when @c revision
    /// has changed and the quiet window has passed, so it may do real work — including logging what
    /// it found, which is how a source reaches a support log as well as a client.
    /// @return The payload, or @c nullopt to say nothing after all. A source that has gone idle
    ///         between the counter read and this call answers @c nullopt rather than inventing an
    ///         event.
    std::function<std::optional<nlohmann::json>()> render;
    /// How often this source is looked at, which is both its latency floor and its ceiling on
    /// messages. The two are the same thing: a source read once a second cannot speak more often
    /// than that, so a producer bumping its counter every real-time cycle needs no separate rate
    /// limit.
    /// Nothing is lost to a slow cadence — the counter is cumulative and the last-seen mark
    /// advances only when a message is sent, so a change is reported whole at the next reading.
    ///
    /// Pick it from what the event is, and from how fast its producer bumps the counter. A
    /// lifecycle change a person is waiting on wants tens of milliseconds; a counter the real-time
    /// loop raises every cycle wants about a second, or it reports at the rate of the fault. Each
    /// source pays only for its own cadence: the bus sleeps until the earliest one is due.
    std::chrono::milliseconds interval{20};
  };

  /// @brief Receives a finished message and the topic to put it on. Wired to
  ///        @c WebSocketServer::publish at the root, the same shape as
  ///        @c MonitoringManager::setPublish.
  using PublishFn = std::function<void(std::string topic, std::string json)>;

  NotificationBus();

  /// @brief Requests the stop and joins.
  ~NotificationBus();

  NotificationBus(const NotificationBus&) = delete;
  NotificationBus& operator=(const NotificationBus&) = delete;
  NotificationBus(NotificationBus&&) = delete;
  NotificationBus& operator=(NotificationBus&&) = delete;

  /// @brief Sets where finished messages go. Call before @c start.
  ///
  /// Leaving it unset is supported: every source still renders, so one that logs keeps logging.
  void setPublish(PublishFn publish);

  /// @brief Registers a source. Call before @c start.
  ///
  /// Membership is fixed once the thread runs, the same rule @c CyclicTask follows, which is what
  /// lets the poll loop read the registry with no lock.
  void addSource(Source source);

  /// @brief Starts the poll thread.
  ///
  /// Each source's last-seen mark is taken here, so a producer that was already counting before
  /// this call raises no message for what it had already recorded.
  void start();

  /// @brief Stops the poll thread and joins it. Returns immediately if it is not running, and does
  ///        not wait out the poll interval.
  void stop();

 private:
  void run(std::stop_token stopToken);

  /// @brief Wraps a source's payload in the protocol's notification envelope.
  static std::string envelope(const nlohmann::json& data);

  /// @brief When the next source falls due, or a second out when there are none.
  std::chrono::steady_clock::time_point nextDeadline() const;

  PublishFn publish_;
  std::vector<Source> sources_;
  std::vector<std::uint64_t> lastSeen_;
  std::vector<std::chrono::steady_clock::time_point> nextDue_;
  // Last on purpose: everything above is fully built before the thread that reads it starts, and
  // the thread is joined before any of it goes away.
  std::jthread thread_;
};

}  // namespace mm::node
