#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "etg/fsoe_frame.h"

namespace mm::etg {

/// @brief One FSoE connection, as the safety configurator defines it (ETG.5100 Table 32).
///
/// Every field has to agree with the slave's own configuration. A mismatch does not degrade the
/// connection, it refuses it: the slave answers with a Reset PDU that names the reason.
struct FsoeMasterConfig {
  /// SafeData octets the master sends each cycle. The slave calls these its SafeOutputs.
  uint16_t safeOutputsLen = 0;

  /// SafeData octets the slave sends each cycle. The slave calls these its SafeInputs.
  ///
  /// The two directions are independent, and an asymmetric pair is normal. A Synapticon drive with
  /// the safe-sensor option takes 8 octets and returns 12, which is a 19-octet master frame against
  /// a 27-octet slave frame.
  uint16_t safeInputsLen = 0;

  /// FSoE Slave Address of the peer. The slave compares it against its own switch or parameter and
  /// rejects any other value, so a cable swap cannot silently move a connection to another device.
  uint16_t slaveAddress = 0;

  /// Connection ID. It has to be non-zero, and unique among the connections on the bus.
  uint16_t connectionId = 1;

  /// FSoE watchdog in ms. It bounds the whole round trip, so it has to exceed the bus cycle time
  /// with margin. The slave checks it against its own accepted range and refuses a value outside.
  uint16_t watchdogMs = 100;

  /// The application half of the SafePara. It is empty for a device that takes no application
  /// parameters. The library prefixes the three communication parameters itself.
  std::vector<uint8_t> applicationParameters;

  /// First session ID, incremented on every new session.
  ///
  /// ETG.5100 calls for a random session ID, because that is what makes a replayed recording of a
  /// whole connection fail. A counter is deterministic and therefore testable, which is the right
  /// trade for a master that is not safety-rated. A certified master must draw this from a real
  /// random source.
  uint16_t initialSessionId = 1;
};

/// @brief What one call to @c FsoeMaster::cycle did.
struct FsoeCycleResult {
  /// The state after the cycle.
  FsoeState state = FsoeState::Reset;

  /// True when the master built a new PDU. Read it from @c FsoeMaster::txPdu.
  bool txUpdated = false;

  /// True when a received PDU passed every check and drove a transition.
  bool rxAccepted = false;

  /// The fault the master reported this cycle, or @c FsoeError::None. A fault always sends the
  /// connection back to Reset.
  FsoeError fault = FsoeError::None;
};

/// @brief The FSoE master state machine for one connection (ETG.5100 ch. 8.4).
///
/// @warning **This is not a safety master.** It implements the protocol, not the integrity of the
/// device that runs it. A safety master needs certified hardware and a certified stack: two
/// redundant channels, cross-checked, on a platform assessed to IEC 61508. This one runs in a
/// normal process on a normal PC, which can compute the right frames and still be wrong in a way
/// nothing detects. Use it to operate, commission and diagnose a safe drive from a tool. Do not
/// use it as the safety function of a machine.
///
/// The slave is the one that stays safe. It authenticates every frame, and it drops its outputs to
/// the safe state when the frames stop or fail. That property does not depend on the quality of
/// this master, which is the reason a tool-side master is useful at all.
///
/// **Shape.** The class is pure: no sockets, no threads, no clock, and no allocation after
/// construction. One call per bus cycle hands in the octets that arrived and hands back the octets
/// to send. Where those octets come from is the transport's problem, and FSoE does not care - the
/// PDU rides inside cyclic process data on EtherCAT, and the SIM carries the same octets over TCP.
///
/// **The transport owes this class three things.**
///   1. The received PDU, sliced out of the input image at exactly @c rxPduSize octets.
///   2. The elapsed time since the last call, in microseconds, for the watchdog.
///   3. The current @c txPdu octets, copied into the output image every cycle - including the
///      cycles where @c FsoeCycleResult::txUpdated is false, because a fieldbus keeps sending the
///      last image and the peer has to keep seeing it.
///
/// **Repeated frames are not events.** ETG.5100 defines the frame-received event as a Safety PDU
/// in which at least one bit changed. A cyclic bus re-presents the same input octets until the
/// peer writes new ones, so this class compares each received PDU against the last one and ignores
/// a repeat. The comparison is safe because every legal successor frame differs: the sequence
/// number advances, which changes every CRC.
///
/// Typical use:
/// @code
/// auto master = FsoeMaster::create({.safeOutputsLen = 8,
///                                   .safeInputsLen = 12,
///                                   .slaveAddress = 3,
///                                   .connectionId = 7,
///                                   .watchdogMs = 100});
/// if (!master) { return std::unexpected(master.error()); }
/// // Each bus cycle:
/// master->setSafeOutputs(outputs);
/// const auto result = master->cycle(inputImage.subspan(offset, master->rxPduSize()), cycleUs);
/// std::ranges::copy(master->txPdu(), outputImage.begin() + outOffset);
/// @endcode
class FsoeMaster {
 public:
  /// @brief Builds a master and opens the connection.
  ///
  /// A new instance is already in Reset with the first Reset PDU built, because ETG.5100 requires
  /// the Reset Connection event on power-on. There is no separate "start" step to forget.
  ///
  /// Both SafeData lengths may be 1 or any even number, which is the whole envelope the standard
  /// allows. Be aware of what is exercised: a length below 4 octets makes the session ID and the
  /// connection data span several frames, and those two rows of the state table
  /// (@c SESSION_STAY1 and @c CONN_STAY1) have no test, because the slave this master was measured
  /// against does not implement them either. Their arithmetic is shared with the multi-cycle
  /// SafePara transfer, which is tested. A drive is 4 octets or more; a narrow safety terminal may
  /// not be.
  ///
  /// @return the master, or a message naming the field that is wrong.
  static std::expected<FsoeMaster, std::string> create(FsoeMasterConfig config);

  /// @brief Runs one bus cycle.
  ///
  /// @param rx    the received Safety Slave PDU, or an empty span when no input is available yet.
  ///              A span of the wrong size is a transport defect, not a protocol fault, so it
  ///              returns an error instead of failing the connection.
  /// @param dtUs  microseconds since the previous call, for the watchdog.
  std::expected<FsoeCycleResult, std::string> cycle(std::span<const uint8_t> rx, uint32_t dtUs);

  /// @brief Drops the connection and restarts it (the Reset Connection event).
  ///
  /// SafeInputs go fail-safe at once. The next cycles run the handshake again from Reset.
  void resetConnection();

  /// @brief Chooses the command sent in the Data state (the Set Data Command event).
  ///
  /// @c FsoeCommand::FailSafeData tells the slave to hold its outputs in the safe state while the
  /// connection stays up, which is how a master releases and re-applies a safety function without
  /// a new handshake. Any other command than these two is refused.
  /// @return false when @p command is neither ProcessData nor FailSafeData.
  bool setDataCommand(FsoeCommand command);

  /// @brief Stages the SafeOutputs sent from the next Data-state frame on.
  /// @return false when @p data is not exactly @c FsoeMasterConfig::safeOutputsLen octets, in
  ///         which case nothing is copied.
  bool setSafeOutputs(std::span<const uint8_t> data);

  /// @brief Returns the PDU to place in the output image. It is valid from construction on.
  [[nodiscard]] std::span<const uint8_t> txPdu() const { return tx_; }

  /// @brief Returns the exact octet count @c cycle expects in a received PDU.
  [[nodiscard]] uint16_t rxPduSize() const { return rxLayout_.size(); }

  [[nodiscard]] FsoeState state() const { return state_; }

  /// @brief Returns whether SafeInputs hold process data from the slave.
  ///
  /// False until the first ProcessData frame is accepted in the Data state, and false again the
  /// moment the connection leaves Data or the slave switches to FailSafeData.
  [[nodiscard]] bool inputsValid() const { return inputsValid_; }

  /// @brief Returns the received SafeInputs.
  ///
  /// The octets read all-zero, the fail-safe value, whenever @c inputsValid is false. The buffer
  /// is cleared on every transition that invalidates it, so a caller that forgets to read the flag
  /// still gets fail-safe data and never acts on a stale value.
  [[nodiscard]] std::span<const uint8_t> safeInputs() const { return safeInputs_; }

  /// @brief Returns the reason the master last sent a Reset PDU.
  [[nodiscard]] FsoeError faultReason() const { return faultReason_; }

  /// @brief Returns the code from the last Reset PDU the **slave** sent.
  ///
  /// This is the diagnostic that says why the peer refused the connection, and it is the first
  /// thing to read when a handshake will not complete. Codes up to 11 are @c FsoeError. Codes 0x80
  /// to 0xFF are device-specific SafePara faults, whose meaning is in the device documentation.
  [[nodiscard]] uint8_t peerFaultCode() const { return peerFaultCode_; }

  /// @brief Returns the session ID of the current session.
  [[nodiscard]] uint16_t sessionId() const { return sessionId_; }

  /// @brief Returns the SafePara as it goes on the wire: three communication parameters, then the
  /// application parameters.
  ///
  /// The communication half is @c CommParaLen (always 2), the watchdog in ms, and @c AppParaLen.
  [[nodiscard]] std::span<const uint8_t> safePara() const { return safePara_; }

  [[nodiscard]] const FsoeMasterConfig& config() const { return config_; }

 private:
  explicit FsoeMaster(FsoeMasterConfig config);

  // Protocol primitives. These mirror the functions and macros of ETG.5100 Tables 31 and 33.
  uint16_t sendFrame(FsoeCommand command, std::span<const uint8_t> data, uint16_t inheritCrc,
                     uint16_t connId, bool calculateNewCrc);
  bool checkRecv(std::span<const uint8_t> rx, uint16_t inheritCrc, bool calculateNewCrc);
  bool isSafeDataCorrect() const;
  void updateBytesToBeSent(uint16_t remaining);

  // Composite transitions, each used by several table rows.
  void resetVars();
  void goSafe();
  FsoeError emitReset(FsoeError reason);
  void startSession(bool resetFirst);
  void sendChunk(FsoeCommand command, std::span<const uint8_t> source, uint16_t total,
                 uint16_t offset, uint16_t inheritCrc, uint16_t connId);
  void startWatchdog();

  // One handler per state, for the frame-received event.
  FsoeCycleResult onFrame(std::span<const uint8_t> rx);
  FsoeCycleResult handleReset(std::span<const uint8_t> rx);
  FsoeCycleResult handleSession(std::span<const uint8_t> rx);
  FsoeCycleResult handleConnection(std::span<const uint8_t> rx);
  FsoeCycleResult handleParameter(std::span<const uint8_t> rx);
  FsoeCycleResult handleData(std::span<const uint8_t> rx);

  FsoeMasterConfig config_;
  FsoeFrameLayout txLayout_{0};  ///< Layout of the PDUs this master sends.
  FsoeFrameLayout rxLayout_{0};  ///< Layout of the PDUs the slave sends.
  uint16_t chunkLen_ = 0;        ///< min(in, out): payload octets per handshake cycle.

  // Buffers, all sized once in create.
  std::vector<uint8_t> tx_;          ///< The PDU to send.
  std::vector<uint8_t> txSafeData_;  ///< SafeData of the PDU we sent, for the echo check.
  std::vector<uint8_t> rxSafeData_;  ///< SafeData of the PDU we received.
  std::vector<uint8_t> lastRx_;      ///< The previous received PDU, for change detection.
  std::vector<uint8_t> safeOutputs_;
  std::vector<uint8_t> safeInputs_;
  std::vector<uint8_t> safePara_;
  std::vector<uint8_t> connData_;  ///< Connection ID and slave address, 4 octets.

  // Protocol variables (ETG.5100 Table 32).
  FsoeState state_ = FsoeState::Reset;
  uint16_t lastCrc_ = 0;        ///< CRC_0 of the last PDU sent, or received, whichever came last.
  uint16_t oldMasterCrc_ = 0;   ///< CRC_0 of the last PDU we sent. Collision base for a send.
  uint16_t oldSlaveCrc_ = 0;    ///< CRC_0 of the last PDU we received. Collision base for a check.
  uint16_t masterSeq_ = 1;      ///< Sequence number for the next PDU we send.
  uint16_t slaveSeq_ = 1;       ///< Expected sequence number of the next PDU we receive.
  uint16_t sessionId_ = 0;      ///< The session ID in use.
  uint16_t nextSessionId_ = 0;  ///< The session ID the next session will use.
  std::array<uint8_t, 2> sessionIdBytes_{};  ///< sessionId_ on the wire, for a chunked resend.
  uint16_t bytesToBeSent_ = 0;
  FsoeCommand dataCommand_ = FsoeCommand::FailSafeData;
  FsoeError faultReason_ = FsoeError::None;
  uint8_t peerFaultCode_ = 0;
  bool secondSessionFrameSent_ = false;
  bool inputsValid_ = false;
  bool lastRxValid_ = false;

  uint32_t watchdogElapsedUs_ = 0;
  bool watchdogRunning_ = false;
};

}  // namespace mm::etg
