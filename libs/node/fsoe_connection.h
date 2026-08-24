#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "etg/fsoe_master.h"
#include "node/ss1_trace.h"
#include "etg/safety_drive_profile.h"

namespace mm::node {

class DeviceManager;

/// @brief Largest SafeData length this layer carries, in octets, per direction.
///
/// The protocol allows more. This bound is what lets a connection's published state be a fixed-size
/// snapshot with no allocation on the cycle path, and it is comfortably above the 8 and 12 octets a
/// safe drive uses.
inline constexpr uint16_t kFsoeMaxSafeDataOctets = 32;

/// @brief Largest SafeOutputs a caller can set through this layer, in octets.
///
/// The setpoint crosses to the cycle thread inside one 64-bit atomic, which is what makes the write
/// tear-free without a lock. Eight octets covers every safe drive connection; a wider one would
/// need a second mechanism, and @c FsoeConnection::open refuses it rather than truncating.
inline constexpr uint16_t kFsoeMaxSettableSafeOutputs = 8;

/// @brief How one FSoE connection is configured (ETG.5100 Table 32).
struct FsoeConnectionConfig {
  /// Bus position of the drive.
  uint16_t slavePosition = 0;

  /// PDO that carries the master frame (an @c 0x16nn RxPDO of the slave).
  ///
  /// ETG.5001.4 leaves the index to the device, so it cannot be inferred. @c 0x1604 / @c 0x1A04 is
  /// what a Synapticon safe drive uses. The frame length is **not** configured: it comes from the
  /// device's own mapping, so the two directions cannot silently disagree.
  uint16_t rxPdoIndex = 0x1604;

  /// PDO that carries the slave frame (an @c 0x1Ann TxPDO of the slave).
  uint16_t txPdoIndex = 0x1A04;

  /// FSoE Slave Address of the drive. It has to match what the drive is configured with, which on
  /// a Synapticon drive is a stored parameter that takes effect after a power cycle.
  uint16_t slaveAddress = 0;

  uint16_t connectionId = 1;      ///< Non-zero, and unique among connections on the bus.
  uint16_t watchdogMs = 100;      ///< Bounds the whole round trip; must exceed the bus cycle time.
  uint16_t initialSessionId = 1;  ///< See @c mm::etg::FsoeMasterConfig::initialSessionId.
  std::vector<uint8_t> applicationParameters;  ///< Application half of the SafePara.
};

/// @brief Everything an off-cycle reader can learn about a connection, captured in one cycle.
///
/// One consistent snapshot rather than a set of independently readable fields: a display that
/// showed a safe position from one cycle beside a validity flag from another would be a display
/// that can say "valid" about a value that was not.
struct FsoeConnectionState {
  mm::etg::FsoeState state = mm::etg::FsoeState::Reset;
  bool bound = false;        ///< The frame is located in the current process image.
  bool inputsValid = false;  ///< SafeInputs hold process data, not fail-safe data.
  mm::etg::FsoeError fault = mm::etg::FsoeError::None;  ///< The master's last reported fault.
  uint8_t peerFaultCode = 0;  ///< Code from the last Reset PDU the drive sent.
  uint16_t sessionId = 0;
  mm::etg::FsoeCommand dataCommand = mm::etg::FsoeCommand::FailSafeData;

  uint64_t cycles = 0;          ///< Cycles this connection has run.
  uint64_t framesAccepted = 0;  ///< Slave PDUs that passed every check.
  uint64_t faults = 0;          ///< Faults since the connection was opened.

  uint16_t safeInputsLength = 0;
  uint16_t safeOutputsLength = 0;
  std::array<uint8_t, kFsoeMaxSafeDataOctets> safeInputs{};
  std::array<uint8_t, kFsoeMaxSafeDataOctets> safeOutputs{};

  /// @brief The safe process values decoded from @c safeInputs (ETG.6100.2 ch. 5.4).
  [[nodiscard]] mm::etg::SdpProcessValues processValues() const {
    return mm::etg::sdpDecodeProcessValues(std::span(safeInputs).first(safeInputsLength));
  }

  /// @brief The safety statusword decoded from @c safeInputs.
  [[nodiscard]] mm::etg::SdpStatus safetyStatus() const {
    return mm::etg::sdpDecodeStatus(std::span(safeInputs).first(safeInputsLength));
  }

  /// @brief The safety controlword currently staged, decoded (ETG.6100.2 Table 3).
  ///
  /// Exists so a caller that owns ONE activation bit can change it without disturbing the others:
  /// decode, set its own field, encode. Two endpoints each writing the whole octet from their own
  /// defaults would otherwise fight, and the loser would be silently re-requesting a stop
  /// function - which, given the inverted activation, is the direction that stops the axis.
  [[nodiscard]] mm::etg::SdpControl safetyControl() const {
    return mm::etg::sdpDecodeControl(std::span(safeOutputs).first(safeOutputsLength));
  }
};

/// @brief One FSoE connection to one drive, driven from the cycle.
///
/// Holds the protocol master (@c mm::etg::FsoeMaster) plus everything needed to move its octets
/// through the process image: where the frame sits, and which objects carry it.
///
/// **Three threads meet here, and each has one job.**
/// - The control plane opens the connection (@c open), which is where every lookup, allocation and
///   SDO read happens.
/// - The cycle thread calls @c step once per bus cycle. It allocates nothing, takes no lock, and
///   touches no map it did not resolve at open time apart from the parameter lookups @c setValue
///   makes, which the node layer already sanctions from a cyclic task.
/// - Any thread may read @c state() and set a request. Requests cross as atomics — latest wins,
///   nothing queues — and the state crosses as a sequence-locked snapshot.
///
/// **The frame travels as ordinary mapped objects.** The cycle composes the output image from each
/// object's parameter cell, so a raw write into that image would be overwritten on the next cycle.
/// Every octet of the master frame is a mapped object (an FSoE command octet, SafeData octets, the
/// CRCs, the connection ID), so @c step writes the built frame back through those cells and the
/// composer does the rest.
///
/// **The input direction is read raw**, from the image the cycle just captured. It has to be: a
/// SafeData value wider than two octets is split by the interleaved CRCs, and the ESI maps the
/// second half as an alignment gap with no object behind it. There is no object to read it from,
/// and reading a "safe position" object would in any case hand out a number that no CRC, sequence
/// number or watchdog had yet vouched for.
class FsoeConnection {
 public:
  /// @brief Opens a connection: locates the frame, checks it, and builds the master.
  ///
  /// Runs on the control plane. It reads the drive's PDO mapping over SDO, so the device must be
  /// reachable (PRE-OP or later) and process data must already be configured — the frame is located
  /// in the published process image, which does not exist before that.
  ///
  /// @return the connection, or a message naming what did not add up.
  static std::expected<std::unique_ptr<FsoeConnection>, std::string> open(
      DeviceManager& deviceManager, const FsoeConnectionConfig& config);

  /// @brief Runs one bus cycle. **Cycle thread only**, from a @c CyclicTask::execute.
  ///
  /// @param deviceManager  resolves the device afresh, as a cyclic task must.
  /// @param dtUs           microseconds since the previous call, for the FSoE watchdog.
  void step(DeviceManager& deviceManager, uint32_t dtUs);

  /// @brief The last published snapshot. Any thread.
  [[nodiscard]] FsoeConnectionState state() const;

  /// @brief Chooses the command sent in the Data state.
  ///
  /// A connection starts in @c FailSafeData and returns to it after every fault, so this has to be
  /// set to @c ProcessData to leave the safe state — and set again after a fault. That is the
  /// standard's default, not a choice made here.
  /// @return false if @p command is neither ProcessData nor FailSafeData.
  bool setDataCommand(mm::etg::FsoeCommand command);

  /// @brief Stages the SafeOutputs to send from the next cycle on.
  /// @return false if @p data is longer than the connection's SafeOutputs.
  bool setSafeOutputs(std::span<const uint8_t> data);

  /// @brief Applies a safety controlword to SafeData octet 0, leaving the other octets alone.
  bool setControl(mm::etg::SdpControl control);

  /// @brief Requests a local reset: the connection drops and the handshake starts again.
  void requestReset() { resetRequested_.store(true, std::memory_order_relaxed); }

  /// @brief Stops driving the frame. The drive's watchdog then takes its outputs to the safe state.
  void close() { active_.store(false, std::memory_order_release); }

  [[nodiscard]] bool active() const { return active_.load(std::memory_order_acquire); }
  [[nodiscard]] const FsoeConnectionConfig& config() const { return config_; }

  /// @brief The most recent completed Safe Stop 1 stop, recorded on the cycle thread.
  ///
  /// Lives here rather than in a caller because only this class sees every cycle: a stop completes
  /// in a couple of hundred milliseconds and the finalizing STO is visible for a single cycle, so
  /// anything sampling from outside would miss it.
  [[nodiscard]] const Ss1Recorder& ss1Recorder() const { return ss1Recorder_; }

  /// @brief Where the master frame's octets live: one entry per mapped object of the frame PDO.
  struct FrameField {
    uint16_t index = 0;
    uint8_t subindex = 0;
    uint16_t frameOffset = 0;  ///< Octet offset of this field within the Safety PDU.
    uint8_t octets = 0;        ///< 1 or 2.
    bool isSigned = false;     ///< The object's declared type, which @c setValue must match.
  };

 private:
  FsoeConnection(FsoeConnectionConfig config, mm::etg::FsoeMaster master,
                 std::vector<FrameField> outputFields, uint32_t inputByteOffset,
                 uint64_t processImageGeneration, uint64_t topologyGeneration);

  void publish(const FsoeConnectionState& fresh);
  void writeFrame(class Device& device);

  FsoeConnectionConfig config_;
  mm::etg::FsoeMaster master_;
  std::vector<FrameField> outputFields_;  ///< Fixed at open; read by the cycle thread.
  uint32_t inputByteOffset_ = 0;          ///< Start of the slave frame in the input image.
  uint16_t inputByteLength_ = 0;
  uint64_t processImageGeneration_ = 0;
  uint64_t topologyGeneration_ = 0;

  // Requests from other threads. Latest wins; nothing queues.
  std::atomic<bool> active_{true};
  std::atomic<bool> resetRequested_{false};
  std::atomic<uint8_t> dataCommand_{static_cast<uint8_t>(mm::etg::FsoeCommand::FailSafeData)};
  std::atomic<uint64_t> safeOutputs_{0};  ///< Up to eight octets, little-endian.

  Ss1Recorder ss1Recorder_;  ///< written by the cycle thread; read by HTTP handlers

  // Counters owned by the cycle thread.
  uint64_t cycles_ = 0;
  uint64_t framesAccepted_ = 0;
  uint64_t faults_ = 0;

  // A sequence lock, the same shape as the recorder ring's: the cycle thread makes the sequence
  // odd, writes, and makes it even again; a reader copies between two matching even reads. The
  // payload copies race by construction, and that is the mechanism — a reader that was overtaken
  // sees the sequence move and reads again.
  mutable std::atomic<uint32_t> stateSeq_{0};
  FsoeConnectionState state_{};
};

}  // namespace mm::node
