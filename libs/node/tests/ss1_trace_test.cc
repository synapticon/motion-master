#include "node/ss1_trace.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {

using mm::node::kSs1TraceCapacity;
using mm::node::kSs1TraceNever;
using mm::node::kSs1TracePostRoll;
using mm::node::kSs1TracePreRoll;
using mm::node::Ss1Recorder;
using mm::node::Ss1Trace;
using mm::node::Ss1TraceEnd;

constexpr uint32_t kDtUs = 1000;

/// Safety controlword octets, in WIRE logic: a clear activation bit is a request.
constexpr uint8_t kCwRunBothOff = 0x03;  // STO deactivated, SS1 deactivated
constexpr uint8_t kCwSs1Active = 0x01;   // STO deactivated, SS1 ACTIVATED

/// A 12-octet SafeInputs frame in the ETG.6100.2 ch. 5.4 layout: statusword, validity, then
/// position, velocity and torque.
std::array<uint8_t, 12> safeInputs(int32_t velocityMilliRpm, bool stoActive, bool error,
                                   bool velocityValid = true) {
    std::array<uint8_t, 12> in{};
    in[0] = static_cast<uint8_t>((stoActive ? 0x01 : 0x00) | (error ? 0x80 : 0x00));
    in[1] = static_cast<uint8_t>(velocityValid ? (1u << 1) : 0u);  // kSdpValidVelocityBit
    const auto raw = static_cast<uint32_t>(velocityMilliRpm);
    in[6] = static_cast<uint8_t>(raw & 0xFF);
    in[7] = static_cast<uint8_t>((raw >> 8) & 0xFF);
    in[8] = static_cast<uint8_t>((raw >> 16) & 0xFF);
    in[9] = static_cast<uint8_t>((raw >> 24) & 0xFF);
    return in;
}

/// One live process-data cycle.
Ss1Recorder::Cycle cycle(const std::array<uint8_t, 12>& in, uint8_t controlword) {
    return {.safeInputs = std::span(in),
            .controlword = controlword,
            .fsoeState = 5,  // Data
            .inputsValid = true,
            .bound = true,
            .processData = true};
}

void run(Ss1Recorder& r, size_t cycles, const std::array<uint8_t, 12>& in, uint8_t cw) {
    for (size_t i = 0; i < cycles; ++i) {
        r.observe(cycle(in, cw), kDtUs);
    }
}

/* ===================== nothing captured until something happens ================== */

TEST(Ss1TraceTest, NothingIsPublishedBeforeAnyStop) {
    Ss1Recorder r;
    r.allocate(12);
    Ss1Trace t;
    EXPECT_FALSE(r.snapshot(t));
    EXPECT_FALSE(r.capturing());
}

TEST(Ss1TraceTest, AnUnallocatedRecorderIsInertRatherThanCrashing) {
    Ss1Recorder r;  // no allocate()
    const auto in = safeInputs(600000, false, false);
    r.observe(cycle(in, kCwSs1Active), kDtUs);
    Ss1Trace t;
    EXPECT_FALSE(r.snapshot(t));
}

TEST(Ss1TraceTest, TheBootStateIsNotMistakenForAStop) {
    /* SafeOutputs start all-zero and ETG.6100 activation is inverted, so an unconfigured frame
       decodes as SS1 REQUESTED. If the recorder seeded its edge detector false, every connection
       would record a phantom stop on its first cycle. */
    Ss1Recorder r;
    r.allocate(12);
    const auto in = safeInputs(0, true, false);
    run(r, 50, in, 0x00);  // all-zero controlword: every stop function requested
    Ss1Trace t;
    EXPECT_FALSE(r.snapshot(t)) << "an all-zero boot frame must not read as a rising edge";
}

TEST(Ss1TraceTest, ARequestOutsideProcessDataIsNotAStop) {
    /* A connection in FailSafeData has the drive already holding torque off. A request arriving
       there is not a stop anyone can learn from, and recording it would bury the real one. */
    Ss1Recorder r;
    r.allocate(12);
    const auto in = safeInputs(0, true, false);
    for (int i = 0; i < 20; ++i) {
        r.observe({.safeInputs = std::span(in),
                   .controlword = kCwRunBothOff,
                   .fsoeState = 5,
                   .inputsValid = true,
                   .bound = true,
                   .processData = false},
                  kDtUs);
    }
    for (int i = 0; i < 20; ++i) {
        r.observe({.safeInputs = std::span(in),
                   .controlword = kCwSs1Active,
                   .fsoeState = 5,
                   .inputsValid = true,
                   .bound = true,
                   .processData = false},
                  kDtUs);
    }
    Ss1Trace t;
    EXPECT_FALSE(r.snapshot(t));
}

/* ===================== a normal stop ============================================ */

TEST(Ss1TraceTest, ACompleteStopIsCapturedAndPublished) {
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);

    run(r, 100, spinning, kCwRunBothOff);  // fills the pre-roll
    EXPECT_FALSE(r.capturing());

    r.observe(cycle(spinning, kCwSs1Active), kDtUs);  // the rising edge
    EXPECT_TRUE(r.capturing());
    Ss1Trace mid;
    EXPECT_FALSE(r.snapshot(mid)) << "a capture in flight must not be published";

    run(r, 50, spinning, kCwSs1Active);                        // decelerating
    const auto stopped = safeInputs(0, true, false);           // STO asserted
    run(r, kSs1TracePostRoll + 2, stopped, kCwSs1Active);      // post-roll runs out

    EXPECT_FALSE(r.capturing());
    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.traceId, 1u);
    EXPECT_TRUE(t.complete);
    EXPECT_FALSE(t.truncated);
    EXPECT_EQ(t.endReason, Ss1TraceEnd::StoObserved);
    EXPECT_EQ(t.safeInputsLength, 12);
    EXPECT_EQ(t.measuredCyclePeriodUs, kDtUs);
    EXPECT_NE(t.stoActiveTUs, kSs1TraceNever);
    EXPECT_EQ(t.errorTUs, kSs1TraceNever) << "a clean stop reports no error";
}

TEST(Ss1TraceTest, ThePreRollCarriesTheSpeedBeforeTheRequest) {
    /* Without pre-roll the plot starts at the request and cannot show the speed the axis was
       holding, nor how long the drive took to react - and the reaction dead time is one of the two
       numbers a stopping-distance calculation needs. */
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    run(r, kSs1TracePreRoll + 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);
    const auto stopped = safeInputs(0, true, false);
    run(r, kSs1TracePostRoll + 2, stopped, kCwSs1Active);

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    ASSERT_GE(t.samples.size(), kSs1TracePreRoll);
    EXPECT_LT(t.samples.front().tUs, 0) << "the trace must begin before the request";
    EXPECT_EQ(t.samples.front().tUs, -static_cast<int32_t>(kSs1TracePreRoll * kDtUs));

    // Timestamps must be monotonic across the pre-roll/capture seam.
    for (size_t i = 1; i < t.samples.size(); ++i) {
        EXPECT_GE(t.samples[i].tUs, t.samples[i - 1].tUs) << "at sample " << i;
    }
}

TEST(Ss1TraceTest, TheAnchorComesFromTheCycleAfterTheTrigger) {
    /* The controlword built during cycle N is not on the wire until N+1, so the input arriving with
       the trigger is the drive's answer to the previous frame. Anchoring on it would shift the whole
       deceleration-limit line. */
    Ss1Recorder r;
    r.allocate(12);
    const auto before = safeInputs(111000, false, false);
    const auto after = safeInputs(222000, false, false);

    run(r, 20, before, kCwRunBothOff);
    r.observe(cycle(before, kCwSs1Active), kDtUs);  // trigger; this frame still shows 111000
    r.observe(cycle(after, kCwSs1Active), kDtUs);   // the first frame that could reflect it
    const auto stopped = safeInputs(0, true, false);
    run(r, kSs1TracePostRoll + 2, stopped, kCwSs1Active);

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.anchorMilliRpm, 222000) << "the anchor must be the cycle AFTER the trigger";
    EXPECT_TRUE(t.anchorValid);
}

TEST(Ss1TraceTest, AnUntrustedVelocityAtTheAnchorIsReportedAsSuch) {
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    const auto blind = safeInputs(600000, false, false, /*velocityValid=*/false);
    run(r, 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);
    r.observe(cycle(blind, kCwSs1Active), kDtUs);
    const auto stopped = safeInputs(0, true, false);
    run(r, kSs1TracePostRoll + 2, stopped, kCwSs1Active);

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_FALSE(t.anchorValid) << "no believable speed at the anchor means no ramp can be drawn";
}

TEST(Ss1TraceTest, TheErrorAndReleaseMarkersAreLatched) {
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    run(r, 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);
    run(r, 10, spinning, kCwSs1Active);
    const auto tripped = safeInputs(400000, true, true);  // STO with the Error bit
    run(r, 5, tripped, kCwSs1Active);
    // The master releases the request, then the post-roll runs out.
    run(r, kSs1TracePostRoll + 2, tripped, kCwRunBothOff);

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_NE(t.errorTUs, kSs1TraceNever);
    EXPECT_NE(t.requestReleasedTUs, kSs1TraceNever);
    EXPECT_GE(t.requestReleasedTUs, t.stoActiveTUs)
        << "the request was released after torque came off, in this scenario";
}

/* ===================== the awkward endings ====================================== */

TEST(Ss1TraceTest, ASecondStopSupersedesTheFirstRatherThanBeingDropped) {
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    run(r, 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);  // stop 1
    run(r, 10, spinning, kCwSs1Active);
    run(r, 5, spinning, kCwRunBothOff);              // released without finishing
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);  // stop 2 begins

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t)) << "the superseded run is published, not discarded - it is evidence";
    EXPECT_EQ(t.traceId, 1u);
    EXPECT_EQ(t.endReason, Ss1TraceEnd::Retriggered);
    EXPECT_FALSE(t.complete);
    EXPECT_TRUE(r.capturing()) << "and the new run is already recording";

    const auto stopped = safeInputs(0, true, false);
    run(r, kSs1TracePostRoll + 2, stopped, kCwSs1Active);
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.traceId, 2u) << "the newer run replaces it once complete";
    EXPECT_EQ(t.endReason, Ss1TraceEnd::StoObserved);
}

TEST(Ss1TraceTest, ARetriggeredRunStillGetsItsPreRoll) {
    /* The pre-roll ring is written on every cycle, capturing or not, which is what makes this work. */
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    run(r, 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);
    run(r, 40, spinning, kCwSs1Active);
    run(r, 5, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);  // retrigger
    const auto stopped = safeInputs(0, true, false);
    run(r, kSs1TracePostRoll + 2, stopped, kCwSs1Active);

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.traceId, 2u);
    EXPECT_LT(t.samples.front().tUs, 0) << "the second run has context too";
}

TEST(Ss1TraceTest, AStopLongerThanTheBufferIsTruncatedAndSaysSo) {
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    run(r, 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);
    run(r, kSs1TraceCapacity + 100, spinning, kCwSs1Active);  // never stops

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.endReason, Ss1TraceEnd::BufferFull);
    EXPECT_TRUE(t.truncated);
    EXPECT_FALSE(t.complete);
    EXPECT_EQ(t.samples.size(), kSs1TraceCapacity);
}

TEST(Ss1TraceTest, LosingTheFrameMidStopEndsTheTraceHonestly) {
    /* If the trace simply stopped producing samples, the plot would read as an axis that held its
       speed - the worst available lie about a safety function. */
    Ss1Recorder r;
    r.allocate(12);
    const auto spinning = safeInputs(600000, false, false);
    run(r, 20, spinning, kCwRunBothOff);
    r.observe(cycle(spinning, kCwSs1Active), kDtUs);
    run(r, 10, spinning, kCwSs1Active);
    r.observe({.safeInputs = {},
               .controlword = 0,
               .fsoeState = 0,
               .inputsValid = false,
               .bound = false,
               .processData = false},
              kDtUs);

    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.endReason, Ss1TraceEnd::Unbound);
    EXPECT_FALSE(t.complete);
    EXPECT_FALSE(r.capturing());
}

TEST(Ss1TraceTest, AShortSafeInputsSpanIsHonouredRatherThanOverread) {
    /* A connection without the safe process values carries fewer octets. Decoding 12 out of 2 would
       manufacture a velocity. */
    Ss1Recorder r;
    r.allocate(2);
    std::array<uint8_t, 2> in{0x00, 0x00};
    for (int i = 0; i < 20; ++i) {
        r.observe({.safeInputs = std::span(in),
                   .controlword = kCwRunBothOff,
                   .fsoeState = 5,
                   .inputsValid = true,
                   .bound = true,
                   .processData = true},
                  kDtUs);
    }
    r.observe({.safeInputs = std::span(in),
               .controlword = kCwSs1Active,
               .fsoeState = 5,
               .inputsValid = true,
               .bound = true,
               .processData = true},
              kDtUs);
    std::array<uint8_t, 2> stopped{0x01, 0x00};
    for (size_t i = 0; i < kSs1TracePostRoll + 2; ++i) {
        r.observe({.safeInputs = std::span(stopped),
                   .controlword = kCwSs1Active,
                   .fsoeState = 5,
                   .inputsValid = true,
                   .bound = true,
                   .processData = true},
                  kDtUs);
    }
    Ss1Trace t;
    ASSERT_TRUE(r.snapshot(t));
    EXPECT_EQ(t.safeInputsLength, 2)
        << "the length is published so a reader can refuse to plot a velocity that does not exist";
    EXPECT_EQ(t.anchorMilliRpm, 0);
    EXPECT_FALSE(t.anchorValid);
}

/* ===================== the renderings =========================================== */

TEST(Ss1TraceTest, TheRowDecodesTheOctetsIntoTheDeclaredColumns) {
    const auto columns = mm::node::ss1TraceColumns();
    mm::node::Ss1TraceSample s;
    s.tUs = 1234;
    s.safeInputs = safeInputs(-600000, true, true);
    s.controlword = kCwSs1Active;
    s.flags = mm::node::kSs1FlagInputsValid | mm::node::kSs1FlagBound;
    const auto row = mm::node::ss1TraceRow(s, 12);
    ASSERT_EQ(row.size(), columns.size());
    EXPECT_EQ(row[0], 1234.0);
    EXPECT_EQ(row[1], 600000.0) << "the magnitude is what the firmware compares";
    EXPECT_EQ(row[2], -600000.0) << "and the signed value stays available";
    EXPECT_EQ(row[7], 1.0) << "stoActive";
    EXPECT_EQ(row[8], 1.0) << "error";
    EXPECT_EQ(row[9], 1.0) << "ss1Requested, in positive logic";
}

TEST(Ss1TraceTest, TheCsvHasOneHeaderAndOneRowPerSample) {
    Ss1Trace t;
    t.safeInputsLength = 12;
    t.samples.resize(3);
    const std::string csv = mm::node::ss1TraceToCsv(t);
    const size_t lines = static_cast<size_t>(std::count(csv.begin(), csv.end(), '\n'));
    EXPECT_EQ(lines, 4u);
    EXPECT_EQ(csv.find("tUs"), 0u);
}

TEST(Ss1TraceTest, EveryEndReasonHasAName) {
    for (auto r : {Ss1TraceEnd::None, Ss1TraceEnd::StoObserved, Ss1TraceEnd::BufferFull,
                   Ss1TraceEnd::Retriggered, Ss1TraceEnd::Unbound}) {
        EXPECT_STRNE(mm::node::ss1TraceEndName(r), "Unknown");
    }
}

}  // namespace
