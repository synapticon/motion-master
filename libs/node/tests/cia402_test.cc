#include "node/cia402.h"

#include <gtest/gtest.h>

namespace {

using mm::node::cia402::decodeState;
using mm::node::cia402::isFaulted;
using mm::node::cia402::OperationMode;
using mm::node::cia402::State;
using mm::node::cia402::toString;

// The canonical statusword bit patterns for each state (the values a drive reports). decodeState
// must classify these and, crucially, must still classify them correctly with the don't-care
// bits (warning bit 7, voltage-enabled bit 4, manufacturer bits 8+) set in any combination.
TEST(Cia402DecodeState, RecognisesCanonicalPatterns) {
  EXPECT_EQ(decodeState(0x0000), State::kNotReadyToSwitchOn);
  EXPECT_EQ(decodeState(0x0040), State::kSwitchOnDisabled);
  EXPECT_EQ(decodeState(0x0021), State::kReadyToSwitchOn);
  EXPECT_EQ(decodeState(0x0023), State::kSwitchedOn);
  EXPECT_EQ(decodeState(0x0027), State::kOperationEnabled);
  EXPECT_EQ(decodeState(0x0007), State::kQuickStopActive);
  EXPECT_EQ(decodeState(0x000F), State::kFaultReactionActive);
  EXPECT_EQ(decodeState(0x0008), State::kFault);
}

TEST(Cia402DecodeState, IgnoresDontCareBits) {
  // Operation enabled is (sw & 0x6F) == 0x27; setting warning (bit 7), voltage (already in 0x27),
  // and manufacturer-specific bits (0xF000) must not change the classification.
  EXPECT_EQ(decodeState(0x0027 | 0x0080 | 0xF000), State::kOperationEnabled);
  // Switch-on disabled is (sw & 0x4F) == 0x40 — quick-stop bit 5 is a don't-care here.
  EXPECT_EQ(decodeState(0x0040 | 0x0020), State::kSwitchOnDisabled);
  // A real SOMANET statusword in OP with warning + internal-limit-active bits set.
  EXPECT_EQ(decodeState(0x1237 & ~0x0008), State::kOperationEnabled);
}

TEST(Cia402DecodeState, FaultPredicate) {
  EXPECT_TRUE(isFaulted(0x0008));
  EXPECT_TRUE(isFaulted(0x000F));
  EXPECT_FALSE(isFaulted(0x0027));
  EXPECT_FALSE(isFaulted(0x0040));
}

TEST(Cia402ToString, StatesAndModes) {
  EXPECT_EQ(toString(State::kOperationEnabled), "OperationEnabled");
  EXPECT_EQ(toString(State::kSwitchOnDisabled), "SwitchOnDisabled");
  EXPECT_EQ(toString(OperationMode::kCyclicSyncPosition), "CyclicSyncPosition");
  EXPECT_EQ(toString(OperationMode::kProfileTorque), "ProfileTorque");
}

TEST(Cia402ToOperationMode, AcceptsKnownRejectsUnknown) {
  using mm::node::cia402::toOperationMode;
  // Every mode the profile defines, including the three this codebase cannot itself drive: whether
  // a drive supports one is what its 0x6502 field answers, and a mode rejected here could never be
  // named even on a drive that advertises it.
  EXPECT_EQ(toOperationMode(0), OperationMode::kNoMode);
  EXPECT_EQ(toOperationMode(1), OperationMode::kProfilePosition);
  EXPECT_EQ(toOperationMode(2), OperationMode::kVelocity);
  EXPECT_EQ(toOperationMode(3), OperationMode::kProfileVelocity);
  EXPECT_EQ(toOperationMode(4), OperationMode::kProfileTorque);
  EXPECT_EQ(toOperationMode(6), OperationMode::kHoming);
  EXPECT_EQ(toOperationMode(7), OperationMode::kInterpolatedPosition);
  EXPECT_EQ(toOperationMode(8), OperationMode::kCyclicSyncPosition);
  EXPECT_EQ(toOperationMode(9), OperationMode::kCyclicSyncVelocity);
  EXPECT_EQ(toOperationMode(10), OperationMode::kCyclicSyncTorque);
  EXPECT_EQ(toOperationMode(11), OperationMode::kCyclicSyncTorqueCommutationAngle);
  // Unassigned / out-of-range values are rejected so an API boundary can 400 them.
  EXPECT_FALSE(toOperationMode(5).has_value());    // the one value the profile reserves
  EXPECT_FALSE(toOperationMode(12).has_value());   // past the end of the profile's modes
  EXPECT_FALSE(toOperationMode(200).has_value());  // beyond INT8 range of the enum
  // Out-of-INT8-range values must be rejected before the narrowing cast — otherwise 264 (0x108)
  // would alias to 8 (CSP) and slip past validation.
  EXPECT_FALSE(toOperationMode(264).has_value());  // ≡ 8 mod 256, but out of INT8 range
  EXPECT_FALSE(toOperationMode(256).has_value());  // ≡ 0 mod 256
  // The negative half belongs to the vendor, so no value in it is a *standard* mode — including the
  // ones SOMANET defines. Naming those is somanet::OperationMode's job, and joining the two halves
  // is operation_modes.h's.
  EXPECT_FALSE(toOperationMode(-2).has_value());   // SOMANET diagnostics, but not CiA402's
  EXPECT_FALSE(toOperationMode(-56).has_value());  // in-range but not a mode
}

// --- The FSA next-hop table ----------------------------------------------------------------------

using mm::node::cia402::Command;
using mm::node::cia402::FsaAction;
using mm::node::cia402::isCommandableState;
using mm::node::cia402::nextFsaTransition;

constexpr State kEveryState[] = {State::kNotReadyToSwitchOn,  State::kSwitchOnDisabled,
                                 State::kReadyToSwitchOn,     State::kSwitchedOn,
                                 State::kOperationEnabled,    State::kQuickStopActive,
                                 State::kFaultReactionActive, State::kFault};

constexpr State kCommandableStates[] = {State::kSwitchOnDisabled, State::kReadyToSwitchOn,
                                        State::kSwitchedOn, State::kOperationEnabled,
                                        State::kQuickStopActive};

TEST(Cia402CommandableState, OnlyTheFiveAMasterCanReach) {
  EXPECT_FALSE(isCommandableState(State::kNotReadyToSwitchOn));
  EXPECT_FALSE(isCommandableState(State::kFaultReactionActive));
  EXPECT_FALSE(isCommandableState(State::kFault));
  for (const auto state : kCommandableStates) {
    EXPECT_TRUE(isCommandableState(state)) << toString(state);
  }
}

TEST(Cia402NextFsaTransition, ArrivingIsTheOnlyFixedPoint) {
  for (const auto state : kCommandableStates) {
    EXPECT_EQ(nextFsaTransition(state, state, true).action, FsaAction::kArrived) << toString(state);
  }
}

TEST(Cia402NextFsaTransition, AnUncommandableTargetIsUnreachableFromEverywhere) {
  for (const auto from : kEveryState) {
    for (const auto target :
         {State::kNotReadyToSwitchOn, State::kFaultReactionActive, State::kFault}) {
      EXPECT_EQ(nextFsaTransition(from, target, true).action, FsaAction::kUnreachable)
          << toString(from) << " -> " << toString(target);
    }
  }
}

TEST(Cia402NextFsaTransition, TheAutomaticStatesAreWaitedOut) {
  // No command enters or leaves either: the drive moves on by itself (transitions 1 and 14), so
  // issuing anything is at best ignored.
  for (const auto target : kCommandableStates) {
    EXPECT_EQ(nextFsaTransition(State::kNotReadyToSwitchOn, target, true).action, FsaAction::kWait)
        << toString(target);
    EXPECT_EQ(nextFsaTransition(State::kFaultReactionActive, target, true).action, FsaAction::kWait)
        << toString(target);
  }
}

TEST(Cia402NextFsaTransition, AFaultIsAlwaysResetFirst) {
  for (const auto target : kCommandableStates) {
    const auto step = nextFsaTransition(State::kFault, target, true);
    EXPECT_EQ(step.action, FsaAction::kCommand) << toString(target);
    EXPECT_EQ(step.command, Command::kCmdFaultReset) << toString(target);
  }
}

TEST(Cia402NextFsaTransition, EveryHopMatchesTheStateMachine) {
  // The whole table, spelled out against ETG.6010 Figure 2 and the firmware's get_next_state. A
  // transposed or mistyped row is the kind of error that looks plausible in code and moves a drive
  // the wrong way on hardware, so it is pinned rather than exercised.
  struct Case {
    State from;
    State target;
    uint16_t command;
  };
  constexpr Case kCases[] = {
      // Climbing: everything below the target advances one state at a time.
      {State::kSwitchOnDisabled, State::kReadyToSwitchOn, Command::kCmdShutdown},
      {State::kSwitchOnDisabled, State::kSwitchedOn, Command::kCmdShutdown},
      {State::kSwitchOnDisabled, State::kOperationEnabled, Command::kCmdShutdown},
      {State::kSwitchOnDisabled, State::kQuickStopActive, Command::kCmdShutdown},
      {State::kReadyToSwitchOn, State::kSwitchedOn, Command::kCmdSwitchOn},
      {State::kReadyToSwitchOn, State::kOperationEnabled, Command::kCmdSwitchOn},
      {State::kReadyToSwitchOn, State::kQuickStopActive, Command::kCmdSwitchOn},
      {State::kSwitchedOn, State::kOperationEnabled, Command::kCmdEnableOperation},
      // Quick Stop Active is only reachable through Operation Enabled (transition 11).
      {State::kSwitchedOn, State::kQuickStopActive, Command::kCmdEnableOperation},
      {State::kOperationEnabled, State::kQuickStopActive, Command::kCmdQuickStop},
      // Descending: each state has its own way down, and they are not interchangeable.
      {State::kReadyToSwitchOn, State::kSwitchOnDisabled, Command::kCmdDisableVoltage},
      {State::kSwitchedOn, State::kSwitchOnDisabled, Command::kCmdDisableVoltage},
      {State::kSwitchedOn, State::kReadyToSwitchOn, Command::kCmdShutdown},
      {State::kOperationEnabled, State::kSwitchOnDisabled, Command::kCmdDisableVoltage},
      {State::kOperationEnabled, State::kReadyToSwitchOn, Command::kCmdShutdown},
      {State::kOperationEnabled, State::kSwitchedOn, Command::kCmdSwitchOn},
      // Out of a quick stop downward is always through Switch On Disabled (transition 12).
      {State::kQuickStopActive, State::kSwitchOnDisabled, Command::kCmdDisableVoltage},
      {State::kQuickStopActive, State::kReadyToSwitchOn, Command::kCmdDisableVoltage},
      {State::kQuickStopActive, State::kSwitchedOn, Command::kCmdDisableVoltage},
  };
  for (const auto& c : kCases) {
    const auto step = nextFsaTransition(c.from, c.target, true);
    EXPECT_EQ(step.action, FsaAction::kCommand) << toString(c.from) << " -> " << toString(c.target);
    EXPECT_EQ(step.command, c.command) << toString(c.from) << " -> " << toString(c.target);
  }
}

TEST(Cia402NextFsaTransition, LeavingAQuickStopUpwardNeedsTheOverride) {
  // The one hop the table refuses on policy rather than on topology: transition 16 exists on this
  // firmware, but overriding a deliberate stop must be something a caller asked for.
  const auto refused = nextFsaTransition(State::kQuickStopActive, State::kOperationEnabled, false);
  EXPECT_EQ(refused.action, FsaAction::kUnreachable);

  const auto allowed = nextFsaTransition(State::kQuickStopActive, State::kOperationEnabled, true);
  EXPECT_EQ(allowed.action, FsaAction::kCommand);
  EXPECT_EQ(allowed.command, Command::kCmdEnableOperation);

  // Every other hop is unaffected by the flag — it is not a general permission.
  for (const auto from : kEveryState) {
    for (const auto target : kCommandableStates) {
      if (from == State::kQuickStopActive && target == State::kOperationEnabled) {
        continue;
      }
      EXPECT_EQ(nextFsaTransition(from, target, false).action,
                nextFsaTransition(from, target, true).action)
          << toString(from) << " -> " << toString(target);
    }
  }
}

TEST(Cia402NextFsaTransition, EveryPairEitherActsOrExplainsItself) {
  // No pair may fall through to a default: an unhandled combination that silently returned
  // "unreachable" would look like a policy decision rather than a gap.
  for (const auto from : kEveryState) {
    for (const auto target : kCommandableStates) {
      const auto step = nextFsaTransition(from, target, true);
      if (from == target) {
        EXPECT_EQ(step.action, FsaAction::kArrived);
        continue;
      }
      // Not asserting anything about the command value: disable voltage *is* 0x0000, so "no
      // command" and "the command that clears every bit" are the same bits. Which one it is comes
      // from the action, which is what this checks.
      EXPECT_NE(step.action, FsaAction::kUnreachable)
          << toString(from) << " -> " << toString(target);
    }
  }
}

TEST(Cia402QuickStopHolds, OnlyTheProfilesHoldingBand) {
  using mm::node::cia402::quickStopHolds;
  // 0-4 end the quick stop in Switch On Disabled; 5-8 stay in Quick Stop Active.
  for (int16_t code = 0; code <= 4; ++code) {
    EXPECT_FALSE(quickStopHolds(code)) << code;
  }
  for (int16_t code = 5; code <= 8; ++code) {
    EXPECT_TRUE(quickStopHolds(code)) << code;
  }
}

TEST(Cia402QuickStopHolds, EverySomanetOptionCode) {
  using mm::node::cia402::quickStopHolds;
  // The four this vendor implements, from its ESI and its published object documentation — a
  // sparse subset, not the profile's contiguous range. 9 is SOMANET's own and passes through via
  // an active short circuit, which the closed 5-8 band answers correctly; pinned so that widening
  // that band later cannot silently make a short-circuit stop look like one that holds.
  EXPECT_FALSE(quickStopHolds(0)) << "disable drive function";
  EXPECT_FALSE(quickStopHolds(2)) << "ramp down, transit into switch on disabled";
  EXPECT_TRUE(quickStopHolds(6)) << "ramp down, stay in quick stop active";
  EXPECT_FALSE(quickStopHolds(9)) << "active short circuit, then switch on disabled";
}

TEST(Cia402QuickStopHolds, TreatsAnUnplaceableCodeAsPassingThrough) {
  using mm::node::cia402::quickStopHolds;
  EXPECT_FALSE(quickStopHolds(-1));
  EXPECT_FALSE(quickStopHolds(10));
  EXPECT_FALSE(quickStopHolds(1000));
}

TEST(Cia402ParseState, RoundTripsEveryName) {
  for (const auto state : kEveryState) {
    EXPECT_EQ(mm::node::cia402::parseState(toString(state)), state) << toString(state);
  }
  EXPECT_FALSE(mm::node::cia402::parseState("").has_value());
  EXPECT_FALSE(mm::node::cia402::parseState("operationEnabled").has_value());
  EXPECT_FALSE(mm::node::cia402::parseState("Unknown").has_value());
}

}  // namespace
