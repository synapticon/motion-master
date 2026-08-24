#include "node/ss1_trace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace mm::node {
namespace {

/// The activation bit, read from the controlword as SENT. Activation is inverted on the wire
/// (ETG.6100.2 Table 3), so a CLEAR bit is a request.
bool ss1RequestedFrom(uint8_t controlword) {
    return (controlword & static_cast<uint8_t>(1u << mm::etg::kSdpControlSs1Bit)) == 0u;
}

int32_t addSaturating(int32_t base, uint32_t add) {
    const int64_t sum = static_cast<int64_t>(base) + static_cast<int64_t>(add);
    return sum > INT32_MAX ? INT32_MAX : static_cast<int32_t>(sum);
}

}  // namespace

void Ss1Recorder::allocate(uint16_t safeInputsLength) {
    safeInputsLength_ = safeInputsLength;
    for (Slot& slot : slots_) {
        slot.meta = Ss1Trace{};
        slot.meta.samples.assign(kSs1TraceCapacity, Ss1TraceSample{});
        slot.count = 0;
    }
    preRollCount_ = 0;
    preRollHead_ = 0;
    prevSs1Requested_ = true;  // see the field's comment: the boot state must not read as an edge
    capturing_.store(false, std::memory_order_release);
    generation_.store(0, std::memory_order_release);
    allocated_ = true;
}

void Ss1Recorder::observe(const Cycle& cycle, uint32_t dtUs) {
    if (!allocated_) {
        return;
    }

    Ss1TraceSample sample;
    sample.controlword = cycle.controlword;
    sample.fsoeState = cycle.fsoeState;
    sample.flags = static_cast<uint8_t>((cycle.inputsValid ? kSs1FlagInputsValid : 0u) |
                                        (cycle.bound ? kSs1FlagBound : 0u) |
                                        (cycle.processData ? kSs1FlagProcessData : 0u));
    const size_t copy = std::min(cycle.safeInputs.size(), kSs1TraceSafeInputs);
    if (copy > 0) {
        std::memcpy(sample.safeInputs.data(), cycle.safeInputs.data(), copy);
    }

    const bool requested = ss1RequestedFrom(cycle.controlword);
    /* The trigger is gated on a live process-data connection, not just on the bit. An all-zero
       SafeOutputs frame decodes as every stop function requested, so a connection sitting in
       FailSafeData - where the drive is already holding torque off - would otherwise record a
       "stop" on the cycle it came up. */
    const bool armed = cycle.bound && cycle.processData;
    const bool rising = armed && requested && !prevSs1Requested_;
    prevSs1Requested_ = armed ? requested : true;

    const bool wasCapturing = capturing_.load(std::memory_order_relaxed);

    if (rising) {
        /* A rising edge while capturing is a genuinely new stop: the drive latches a run once
           begun, so the request must have been released for this edge to exist. Newest wins - the
           run in flight is published as it stands rather than discarded, because it is evidence,
           and then this cycle starts the new one. */
        if (wasCapturing) {
            finish(Ss1TraceEnd::Retriggered, false);
        }
        beginCapture(cycle);
    }

    if (capturing_.load(std::memory_order_relaxed)) {
        Slot& slot = writeSlot();
        elapsedUs_ = addSaturating(elapsedUs_, dtUs);
        dtSumUs_ += dtUs;
        ++dtCount_;
        sample.tUs = elapsedUs_;

        /* The anchor comes from the cycle AFTER the trigger - see Ss1Trace::anchorMilliRpm. The
           skip is what makes it "after": beginCapture runs inside this same observe() call. */
        if (awaitingAnchor_ && anchorSkip_ > 0) {
            --anchorSkip_;
        } else if (awaitingAnchor_) {
            const auto values = mm::etg::sdpDecodeProcessValues(
                std::span(sample.safeInputs).first(std::min<size_t>(safeInputsLength_, kSs1TraceSafeInputs)));
            slot.meta.anchorMilliRpm = std::abs(values.velocityMilliRpm);
            slot.meta.anchorValid = values.velocityValid && ((sample.flags & kSs1FlagInputsValid) != 0u);
            awaitingAnchor_ = false;
        }

        const auto status = mm::etg::sdpDecodeStatus(
            std::span(sample.safeInputs).first(std::min<size_t>(safeInputsLength_, kSs1TraceSafeInputs)));
        const bool trustworthy = (sample.flags & kSs1FlagInputsValid) != 0u;
        if (trustworthy && status.stoActive && slot.meta.stoActiveTUs == kSs1TraceNever) {
            slot.meta.stoActiveTUs = sample.tUs;
            postRollLeft_ = kSs1TracePostRoll;  // keep going, so the coast after torque is visible
        }
        if (trustworthy && status.error && slot.meta.errorTUs == kSs1TraceNever) {
            slot.meta.errorTUs = sample.tUs;
        }
        if (!requested && slot.meta.requestReleasedTUs == kSs1TraceNever) {
            slot.meta.requestReleasedTUs = sample.tUs;
        }

        if (slot.count < kSs1TraceCapacity) {
            slot.meta.samples[slot.count] = sample;
            ++slot.count;
        }

        if (slot.count >= kSs1TraceCapacity) {
            finish(Ss1TraceEnd::BufferFull, false);
        } else if (!cycle.bound) {
            /* The connection stopped driving the frame. Record the truth and stop, rather than let
               the trace stall silently and read as an axis that held its speed. */
            finish(Ss1TraceEnd::Unbound, false);
        } else if (slot.meta.stoActiveTUs != kSs1TraceNever) {
            if (postRollLeft_ == 0) {
                finish(Ss1TraceEnd::StoObserved, true);
            } else {
                --postRollLeft_;
            }
        }
    }

    /* The pre-roll ring is written on EVERY cycle, capturing or not. That is what lets a retrigger
       still get a correct pre-roll, and it costs one 20-byte store. */
    sample.tUs = 0;  // pre-roll timestamps are assigned relative to the trigger, in beginCapture
    preRoll_[preRollHead_] = sample;
    preRollHead_ = (preRollHead_ + 1) % kSs1TracePreRoll;
    if (preRollCount_ < kSs1TracePreRoll) {
        ++preRollCount_;
    }
}

void Ss1Recorder::beginCapture(const Cycle& cycle) {
    (void)cycle;
    Slot& slot = writeSlot();
    slot.count = 0;
    slot.meta.traceId = nextTraceId_++;
    slot.meta.complete = false;
    slot.meta.truncated = false;
    slot.meta.endReason = Ss1TraceEnd::None;
    slot.meta.safeInputsLength = safeInputsLength_;
    slot.meta.measuredCyclePeriodUs = 0;
    slot.meta.anchorMilliRpm = 0;
    slot.meta.anchorValid = false;
    slot.meta.stoActiveTUs = kSs1TraceNever;
    slot.meta.errorTUs = kSs1TraceNever;
    slot.meta.requestReleasedTUs = kSs1TraceNever;
    slot.meta.triggeredAtUnixNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    /* Lay the pre-roll down oldest-first with negative timestamps. The interval between pre-roll
       samples is not known individually - only the cycles that were captured carry their own dt -
       so they are spaced by the nominal period seen so far, or 1 ms before anything is known. This
       is context, not measurement, and the plot labels it as such. */
    const int32_t spacing = (dtCount_ > 0) ? static_cast<int32_t>(dtSumUs_ / dtCount_) : 1000;
    const size_t n = preRollCount_;
    for (size_t i = 0; i < n; ++i) {
        const size_t src = (preRollHead_ + kSs1TracePreRoll - n + i) % kSs1TracePreRoll;
        Ss1TraceSample s = preRoll_[src];
        s.tUs = -static_cast<int32_t>((n - i) * static_cast<size_t>(spacing));
        slot.meta.samples[slot.count] = s;
        ++slot.count;
    }

    elapsedUs_ = 0;
    dtSumUs_ = 0;
    dtCount_ = 0;
    awaitingAnchor_ = true;
    anchorSkip_ = 1;
    postRollLeft_ = 0;
    capturing_.store(true, std::memory_order_release);
}

void Ss1Recorder::finish(Ss1TraceEnd reason, bool complete) {
    Slot& slot = writeSlot();
    slot.meta.endReason = reason;
    slot.meta.complete = complete;
    slot.meta.truncated = (reason == Ss1TraceEnd::BufferFull);
    slot.meta.measuredCyclePeriodUs =
        (dtCount_ > 0) ? static_cast<uint32_t>(dtSumUs_ / dtCount_) : 0;
    slot.meta.samples.resize(kSs1TraceCapacity);  // capacity is fixed; count says what is live

    capturing_.store(false, std::memory_order_release);
    /* Publishing is the generation bump, and it must be the LAST store: until it lands, a reader
       sees the previous trace, which is whole. Release so everything written above is visible to a
       reader that acquires the new generation. */
    generation_.fetch_add(1, std::memory_order_release);
}

bool Ss1Recorder::snapshot(Ss1Trace& out) const {
    if (!allocated_) {
        return false;
    }
    for (int attempt = 0;; ++attempt) {
        const uint64_t before = generation_.load(std::memory_order_acquire);
        if (before == 0) {
            return false;  // nothing has ever completed
        }
        const Slot& slot = slots_[before & 1u];
        out = slot.meta;
        out.samples.resize(std::min(slot.count, kSs1TraceCapacity));
        std::copy_n(slot.meta.samples.begin(), out.samples.size(), out.samples.begin());
        std::atomic_thread_fence(std::memory_order_acquire);
        if (generation_.load(std::memory_order_relaxed) == before) {
            return true;
        }
        /* Only reachable if a second stop completed while this copy was in flight. Stops are
           seconds apart and a copy is microseconds, so this is theory rather than practice - it
           exists so the code does not rely on that. */
        if (attempt > 4) {
            std::this_thread::yield();
        }
    }
}

std::span<const char* const> ss1TraceColumns() {
    static constexpr const char* kColumns[] = {
        "tUs",           "safeSpeedMilliRpm", "safeVelocityMilliRpm", "safePositionRevolutions",
        "safeTorqueMillinewtonMetres", "velocityValid", "positionValid", "stoActive",
        "error",         "ss1Requested",      "stoRequested",         "inputsValid",
        "bound",
    };
    return std::span<const char* const>(kColumns, std::size(kColumns));
}

std::array<double, 13> ss1TraceRow(const Ss1TraceSample& s, uint16_t safeInputsLength) {
    const size_t n = std::min<size_t>(safeInputsLength, kSs1TraceSafeInputs);
    const std::span<const uint8_t> in = std::span(s.safeInputs).first(n);
    const auto values = mm::etg::sdpDecodeProcessValues(in);
    const auto status = mm::etg::sdpDecodeStatus(in);
    const auto control = mm::etg::sdpDecodeControl(std::span(&s.controlword, 1));
    /* One increment of the safe position is 2^-24 of a revolution (0x6601 declares the unit). */
    constexpr double kIncrementsPerRev = 16777216.0;
    return {
        static_cast<double>(s.tUs),
        static_cast<double>(std::abs(values.velocityMilliRpm)),
        static_cast<double>(values.velocityMilliRpm),
        static_cast<double>(values.positionFixedPoint) / kIncrementsPerRev,
        static_cast<double>(values.torqueMillinewtonMetres),
        values.velocityValid ? 1.0 : 0.0,
        values.positionValid ? 1.0 : 0.0,
        status.stoActive ? 1.0 : 0.0,
        status.error ? 1.0 : 0.0,
        control.ss1Requested ? 1.0 : 0.0,
        control.stoRequested ? 1.0 : 0.0,
        ((s.flags & kSs1FlagInputsValid) != 0u) ? 1.0 : 0.0,
        ((s.flags & kSs1FlagBound) != 0u) ? 1.0 : 0.0,
    };
}

const char* ss1TraceEndName(Ss1TraceEnd reason) {
    switch (reason) {
        case Ss1TraceEnd::None: return "None";
        case Ss1TraceEnd::StoObserved: return "StoObserved";
        case Ss1TraceEnd::BufferFull: return "BufferFull";
        case Ss1TraceEnd::Retriggered: return "Retriggered";
        case Ss1TraceEnd::Unbound: return "Unbound";
    }
    return "Unknown";
}

std::string ss1TraceToCsv(const Ss1Trace& trace) {
    std::string out;
    out.reserve(trace.samples.size() * 96 + 256);
    const auto columns = ss1TraceColumns();
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += columns[i];
    }
    out += '\n';
    for (const Ss1TraceSample& s : trace.samples) {
        const auto row = ss1TraceRow(s, trace.safeInputsLength);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) {
                out += ',';
            }
            /* Integers as integers: a CSV of "512678.000000" is harder to read and three times the
               bytes, and only the position column is genuinely fractional. */
            if (i == 3) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.6f", row[i]);
                out += buf;
            } else {
                out += std::to_string(static_cast<long long>(row[i]));
            }
        }
        out += '\n';
    }
    return out;
}

}  // namespace mm::node
