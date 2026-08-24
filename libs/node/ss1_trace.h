#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include <string>

#include "etg/safety_drive_profile.h"

namespace mm::node {

/// @brief Octets of SafeInputs one sample carries: the ETG.6100.2 ch. 5.4 layout.
inline constexpr size_t kSs1TraceSafeInputs = 12;

/// @brief Samples one captured stop can hold.
///
/// At the 1 ms safety cycle a SIM device runs, this is four seconds - longer than any t_SS1 a
/// UNSIGNED16 millisecond object can express at a useful deceleration. On AM2612 the FSoE exchange
/// rate is nearer 84 Hz, where it is most of a minute. Fixed at compile time because the recorder
/// runs on the cycle thread and may not allocate.
inline constexpr size_t kSs1TraceCapacity = 4096;

/// @brief Cycles of context kept from BEFORE the trigger.
///
/// Without it the plot starts at the instant of the request and cannot show the speed the axis was
/// actually holding, nor how long the drive took to react - and the reaction dead time is one of the
/// two numbers a stopping-distance calculation needs.
inline constexpr size_t kSs1TracePreRoll = 256;

/// @brief Cycles kept AFTER torque is removed, so the coast is visible.
inline constexpr size_t kSs1TracePostRoll = 128;

/// @brief Speed magnitude, in milli-rpm, below which a stop has nothing to show.
///
/// A stop requested while the axis is already still is a real event and a legitimate capture, but
/// there is no deceleration in it: every sample sits inside the standstill window and the plot is
/// noise around zero. One such trigger - and during commissioning they are common, because
/// verifying that SS1 fires at all is the obvious first thing to try - would otherwise overwrite
/// the informative trace somebody was reading.
///
/// A fixed floor rather than the configured n_Zero_SS1, because the recorder runs on the cycle
/// thread and has no access to the object dictionary. One rpm is far below any plausible standstill
/// window and far above measurement noise, so the classification does not depend on tuning.
inline constexpr uint32_t kSs1TraceMotionFloorMrpm = 1000;

/// @brief One recorded cycle. POD, 20 bytes, no decoding on the cycle thread.
///
/// The SafeInputs are stored as RAW OCTETS rather than as decoded values on purpose: decoding is
/// pure and cheap but it belongs off the real-time thread, and keeping the octets means a later
/// change to the profile layout can reinterpret an old trace rather than invalidate it.
struct Ss1TraceSample {
    int32_t tUs = 0;  ///< signed microseconds from the trigger cycle; negative is pre-roll
    std::array<uint8_t, kSs1TraceSafeInputs> safeInputs{};
    uint8_t controlword = 0;  ///< SafeOutputs octet 0 as SENT, in wire logic (activation inverted)
    uint8_t flags = 0;        ///< see kSs1Flag*
    uint8_t fsoeState = 0;    ///< mm::etg::FsoeState
    uint8_t reserved = 0;
};

inline constexpr uint8_t kSs1FlagInputsValid = 1u << 0;
inline constexpr uint8_t kSs1FlagBound = 1u << 1;
inline constexpr uint8_t kSs1FlagProcessData = 1u << 2;

/// @brief Why a capture stopped.
enum class Ss1TraceEnd : uint8_t {
    None = 0,        ///< still capturing
    StoObserved = 1, ///< torque was removed and the post-roll ran out - the normal ending
    BufferFull = 2,  ///< the stop outlasted the buffer
    Retriggered = 3, ///< a new stop began before this one finished
    Unbound = 4,     ///< the connection stopped driving the frame mid-stop
};

/// @brief One captured stop, as a reader sees it.
struct Ss1Trace {
    uint64_t traceId = 0;  ///< monotonic; 0 means nothing has ever been captured
    bool complete = false;
    bool truncated = false;
    Ss1TraceEnd endReason = Ss1TraceEnd::None;
    uint16_t safeInputsLength = 0;
    uint64_t triggeredAtUnixNs = 0;
    uint32_t measuredCyclePeriodUs = 0;  ///< mean dt across the capture, so a slow bus is visible

    /// @brief |safe velocity| latched from the cycle AFTER the trigger, in milli-rpm.
    ///
    /// After, not on, and the reason is a one-cycle skew that would otherwise shift the whole
    /// deceleration-limit line: the controlword built during cycle N is not on the wire until N+1,
    /// so the input arriving with the trigger is the drive's answer to the PREVIOUS frame and cannot
    /// yet reflect the request. Latched here, once, rather than left for every reader to rediscover.
    int32_t anchorMilliRpm = 0;
    bool anchorValid = false;

    /// Microseconds from the trigger, or kSs1TraceNever.
    int32_t stoActiveTUs = 0;
    int32_t errorTUs = 0;
    int32_t requestReleasedTUs = 0;

    std::vector<Ss1TraceSample> samples;
};

/// @brief Marker value for "this never happened during the capture".
inline constexpr int32_t kSs1TraceNever = INT32_MIN;

/**
 * @brief Records one Safe Stop 1 stop per activation, newest kept.
 *
 * Written by the FSoE cycle thread through @c observe and read by HTTP handlers through
 * @c snapshot. Allocation happens only in @c allocate, which the connection calls from @c open -
 * @c observe does no allocation, takes no lock and performs no I/O, per the CyclicTask contract.
 *
 * Two slots and a generation counter, rather than a lock: the writer fills the slot the published
 * generation does NOT name, so a reader copying the published slot cannot be overwritten underneath
 * it, and re-reading the generation catches the one case that theory allows - a second stop
 * completing while a reader is still copying. Stops are seconds apart and a copy is microseconds, so
 * that retry is unreachable in practice; it exists so the code does not depend on that being true.
 *
 * Only the most recent COMPLETED stop is published. A capture in progress is invisible, which is
 * deliberate: half a stop plotted against a deadline invites a conclusion the data cannot support.
 */
class Ss1Recorder {
  public:
    /// @brief Size the storage. Call once, off the cycle thread, before any @c observe.
    void allocate(uint16_t safeInputsLength);

    /// @brief One cycle as the recorder needs to see it.
    ///
    /// Explicit fields rather than FsoeConnectionState, for two reasons: the connection has to
    /// include this header to hold a recorder, so depending on its state type would be circular;
    /// and a recorder that takes only what it uses can be unit-tested by feeding it synthetic
    /// cycles, with no connection, no device manager and no bus.
    struct Cycle {
        std::span<const uint8_t> safeInputs;  ///< as received; shorter than 12 octets is honoured
        uint8_t controlword = 0;              ///< SafeOutputs octet 0 as SENT, wire logic
        uint8_t fsoeState = 0;
        bool inputsValid = false;
        bool bound = false;
        bool processData = false;
        /// @brief A new SafeInputs frame arrived on this cycle.
        ///
        /// FSoE is a ping-pong and each direction costs a bus cycle, so a new frame lands roughly
        /// every third one. Recording every cycle stored each value three times over and made the
        /// trace claim a resolution the transport does not have - it looked like a staircase because
        /// it WAS one. Cycles without a fresh frame now only accumulate time.
        bool freshFrame = false;
    };

    /// @brief Advance by one cycle. CYCLE THREAD ONLY. No allocation, no lock, no I/O.
    void observe(const Cycle& cycle, uint32_t dtUs);

    /// @brief Copy the most recent completed stop.
    /// @return false when nothing has been captured yet, or the storage was never allocated.
    bool snapshot(Ss1Trace& out) const;

    /// @brief Whether a stop is being recorded right now.
    [[nodiscard]] bool capturing() const { return capturing_.load(std::memory_order_acquire); }

    /// @brief Stops that began at standstill and were therefore NOT published.
    ///
    /// Reported rather than dropped silently: the event happened, and a user who triggered SS1 and
    /// saw the plot not change is owed an explanation better than nothing.
    [[nodiscard]] uint32_t standstillStops() const {
        return standstillStops_.load(std::memory_order_relaxed);
    }

    /// @brief Wall clock of the most recent such stop, or 0.
    [[nodiscard]] uint64_t lastStandstillStopUnixNs() const {
        return lastStandstillNs_.load(std::memory_order_relaxed);
    }

  private:
    struct Slot {
        Ss1Trace meta;
        size_t count = 0;
    };

    void beginCapture(const Cycle& cycle);
    void finish(Ss1TraceEnd reason, bool complete);
    [[nodiscard]] Slot& writeSlot() { return slots_[(generation_.load(std::memory_order_relaxed) + 1u) & 1u]; }

    std::array<Slot, 2> slots_{};
    std::array<Ss1TraceSample, kSs1TracePreRoll> preRoll_{};
    size_t preRollCount_ = 0;
    size_t preRollHead_ = 0;

    std::atomic<uint64_t> generation_{0};  ///< even/odd selects the published slot; 0 = none yet
    std::atomic<bool> capturing_{false};
    std::atomic<uint32_t> standstillStops_{0};
    std::atomic<uint64_t> lastStandstillNs_{0};

    bool allocated_ = false;
    uint16_t safeInputsLength_ = 0;

    /// Seeded TRUE, which is the whole defence against a phantom stop at every boot: SafeOutputs
    /// start all-zero and ETG.6100 activation is inverted, so an unconfigured frame decodes as SS1
    /// REQUESTED. Seeding false would make the first real cycle look like a rising edge.
    bool prevSs1Requested_ = true;

    int32_t elapsedUs_ = 0;
    uint64_t dtSumUs_ = 0;
    size_t dtCount_ = 0;
    bool awaitingAnchor_ = false;
    uint32_t pendingUs_ = 0;  ///< time accumulated on cycles that carried no new frame
    /// Cycles still to skip before latching the anchor. One, because beginCapture runs inside the
    /// observe() call for the trigger cycle itself, and that cycle's input is the drive's answer to
    /// the PREVIOUS frame - it cannot yet reflect the request.
    uint8_t anchorSkip_ = 0;
    size_t postRollLeft_ = 0;
    uint64_t nextTraceId_ = 1;
};

/// @brief Column names for the row-oriented rendering, in the order @c ss1TraceRow emits.
[[nodiscard]] std::span<const char* const> ss1TraceColumns();

/// @brief Decode one sample into the numeric row the columns describe.
[[nodiscard]] std::array<double, 13> ss1TraceRow(const Ss1TraceSample& s, uint16_t safeInputsLength);

/// @brief The trace as CSV: a columns header then one row per sample.
[[nodiscard]] std::string ss1TraceToCsv(const Ss1Trace& trace);

/// @brief Name for @c Ss1TraceEnd, for JSON.
[[nodiscard]] const char* ss1TraceEndName(Ss1TraceEnd reason);

}  // namespace mm::node
