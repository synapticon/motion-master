#pragma once

// A SPoE server on loopback that answers the way the netX firmware of a SOMANET drive answers. The
// SPoE driver tests run against it, so a test can produce the transport faults that a real drive
// produces only by accident.
//
// The behaviour comes from the firmware source, `somanet_software`,
// `arm_application/Components/cifXApplicationSockIf/Sources/AppSockIf_MessageHandler.c`. Where the
// specification and the firmware disagree, this fake follows the firmware.
//
// The fake encodes and decodes every frame by hand. It shares no code with the driver on purpose: a
// codec bug that both sides shared would make the tests pass against a drive that rejects the
// bytes.
//
// The file messages follow `AppSockIf_ReadFile` and `AppSockIf_WriteFile`. A read answers its first
// packet with no data and then sends 512-byte chunks. A file the drive does not have reads as an
// empty one, so the driver's check of the file list runs; what the SoC really answers for a missing
// file is not in the firmware source. The first packet of `com_firmware.bin` in BOOT answers 0x00
// rather than ACK, because the firmware returns what `storage_prepare_for_writing` returned. The
// firmware update answers, then goes silent until its reset, drops the connection, refuses
// connections while it restarts, and comes back in PRE-OP. The watchdog value is stored and does
// not fault the device.
//
// A request whose firmware answer cannot be modelled closes the connection and is counted in
// `unmodelledRequests()`, so a test that reaches one fails clearly instead of passing on an
// invented answer. Today that is an empty batch read. A message type the firmware does not know
// gets an empty reply, as the firmware sends.
//
// The parameter list follows `AppSockIf_ReadObjectInfo` and `AppSockIf_GetObjectInfoBuf`. Each
// entry is the firmware's `struct _sdoinfo_entry_description` copied as it lies in memory: 68
// bytes, laid out by the natural alignment of its members. That layout is not confirmed on
// hardware.
//
// A second client gets a connection and no answer, while the first client keeps the drive. The
// firmware keeps its listen socket in the poll set and accepts every new connection. Then
// `Sock_SaveHandle` finds its single client slot full and drops the new handle, so nothing reads,
// answers or closes that connection. The fake keeps such a connection open and silent until the
// fake is destroyed. Whether the netX TCP stack completes the accept is not confirmed on hardware.
//
// The input buffer fills in OP only. `sqi_cyclic_rx_handler` pushes a frame only when the device
// is in OP, so in any other state a process-data reply carries no inputs.
//
// The idle timeout is not modelled. `AppSockIf_AcyclicHandler` aborts a connection that has no
// request for 5000 of its ticks, and how long a tick is was not found.
//
// Thread-safety: the server runs its own I/O thread. Every public method is safe to call from the
// test thread while a client is connected.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mm::comm::testing {

/// The SPoE message types the firmware handles.
enum class SpoeMessage : uint8_t {
  kSdoRead = 0x01,
  kSdoWrite = 0x02,
  kPdoFrame = 0x03,
  kPdoControl = 0x04,
  kSdoBatchRead = 0x06,
  kFirmwareUpdate = 0x0B,
  kFileRead = 0x0C,
  kFileWrite = 0x0D,
  kStateControl = 0x0E,
  kStateRead = 0x0F,
  kParamList = 0x10,
  kParamDesc = 0x11,
  kParamSubDesc = 0x12,
  kParamFullDesc = 0x13,
  kServerInfo = 0x20,
  kDeviceLocate = 0x21,
  kWatchdogTimeout = 0x22,
};

/// Message id u8, sequence id u16, status u16, data length u16. Little-endian, no padding.
constexpr std::size_t kSpoeHeaderSize = 7;

/// `MAX_FRAME_SIZE` in the firmware: the largest data part of one frame.
constexpr std::size_t kSpoeMaxDataSize = 512;

/// The protocol version the firmware reports in the server information, 1.2.
constexpr uint16_t kSpoeProtocolVersion = 0x0102;

/// `ETHERNET_PDO_MODE_*` in the firmware.
constexpr uint8_t kSpoePdoModeNone = 0x00;
constexpr uint8_t kSpoePdoModeMonitor = 0x01;
constexpr uint8_t kSpoePdoModeControl = 0x02;

/// `ESM_*_STATE` in the firmware. The values are the EtherCAT AL state codes.
constexpr uint8_t kSpoeStateInit = 1;
constexpr uint8_t kSpoeStatePreOp = 2;
constexpr uint8_t kSpoeStateBoot = 3;
constexpr uint8_t kSpoeStateSafeOp = 4;
constexpr uint8_t kSpoeStateOp = 8;

/// `SDO_REQUEST_ERROR_*` in the firmware. An SDO status is one of these, never a CoE abort code.
constexpr uint16_t kSpoeSdoNotFound = 2;
constexpr uint16_t kSpoeSdoSubNotFound = 13;

/// `SQI_REPLY_STATUS_*` in the firmware: the low status byte of a parameter-list reply.
constexpr uint8_t kSpoeReplyAck = 0x58;
constexpr uint8_t kSpoeReplyBusy = 0x28;
constexpr uint8_t kSpoeReplyError = 0x63;

/// The packet states of a segmented transfer. A request carries one in the low status byte, and a
/// reply in the high status byte.
constexpr uint8_t kSpoePacketFirst = 0x80;
constexpr uint8_t kSpoePacketMiddle = 0xC0;
constexpr uint8_t kSpoePacketLast = 0x40;

/// `MAX_INDEX_LIST` in the firmware: the most objects the index list and the parameter list hold.
constexpr std::size_t kSpoeMaxIndexList = 200;

/// The size of one parameter-list entry, `struct _sdoinfo_entry_description`.
constexpr std::size_t kSpoeEntrySize = 68;

/// `AppUtil_GetParameter` and `AppUtil_SetParameter` answer this in INIT and BOOT.
constexpr uint16_t kSpoeSdoNotAllowedInState = 0xFFFF;

/// A transport fault that the fake applies to the reply of one request.
enum class SpoeFault {
  /// Send the reply in several writes, so that it arrives in several TCP segments.
  kSplitReply,
  /// Do not send this reply. Send it in one write together with the reply to the next request.
  /// The client sees a late reply and the reply it waits for in one TCP segment.
  kHoldReply,
  /// Send the reply with a sequence id that does not match the request.
  kWrongSequenceId,
  /// Send a header whose data length is larger than `kSpoeMaxDataSize`.
  kOversizeLength,
  /// Send nothing.
  kNoReply,
  /// Close the connection instead of a reply.
  kDropConnection,
  /// Answer a middle packet of the parameter list with status BUSY and no entries, while the list
  /// still moves past them. The firmware does this when the uninitialised status of a middle
  /// packet happens to equal BUSY, and the entries of that packet are lost.
  kLoseParamListPacket,
};

/// One dictionary entry, as the firmware's object dictionary describes it.
struct FakeSpoeEntry {
  uint16_t dataType = 0;
  uint8_t objectCode = 0x07;
  uint16_t bitLength = 0;
  uint16_t access = 0;
  std::string name;
};

/// A loopback SPoE server that holds one device.
class FakeSpoeServer {
 public:
  /// Binds 127.0.0.1 on a free port and starts the I/O thread.
  FakeSpoeServer();
  ~FakeSpoeServer();

  FakeSpoeServer(const FakeSpoeServer&) = delete;
  FakeSpoeServer& operator=(const FakeSpoeServer&) = delete;

  uint16_t port() const;

  // The device.

  void setObject(uint16_t index, uint16_t subindex, std::vector<uint8_t> value);

  void setFile(const std::string& name, std::vector<uint8_t> content);
  std::optional<std::vector<uint8_t>> file(const std::string& name) const;

  int firmwareUpdates() const;

  /// Sets how long after its answer the firmware update resets the drive, and how long the drive
  /// then refuses connections. The firmware resets after 1000 ms; tests use less.
  void setRestartTiming(std::chrono::milliseconds resetDelay,
                        std::chrono::milliseconds restartDuration);

  /// Adds an entry to the dictionary the parameter list reports. An object reports its highest
  /// described subindex as its subindex count.
  void describeEntry(uint16_t index, uint8_t subindex, FakeSpoeEntry entry);
  std::optional<std::vector<uint8_t>> object(uint16_t index, uint16_t subindex) const;

  uint8_t state() const;
  void setState(uint8_t alState);

  /// Makes every later state change fail, as the firmware does when the variant check fails.
  void setRefuseStateChanges(bool refuse);

  uint8_t pdoMode() const;
  bool locating() const;
  std::optional<uint32_t> watchdogTimeoutMs() const;

  /// False after a request of a type the firmware does not know. The firmware clears its
  /// "SPoE active" flag in that case.
  bool spoeActive() const;

  /// Adds one input frame to the drive's buffer of process data, as the drive does in its cycle.
  /// The frame is dropped unless the device is in OP.
  void pushInputFrame(const std::vector<uint8_t>& frame);

  /// The outputs of the last process-data frame the drive accepted.
  std::vector<uint8_t> lastOutputs() const;

  // Faults and counters.

  /// Applies @p fault to the reply of the next request.
  void injectFault(SpoeFault fault);

  /// Lets @p skip requests pass, then applies @p fault to the reply of the request after them.
  void injectFaultAfter(int skip, SpoeFault fault);

  /// Holds every later reply back by @p delay before it is sent.
  void setReplyDelay(std::chrono::milliseconds delay);

  int connectionCount() const;

  /// Connections that arrived while another client held the drive, and got no answer.
  int ignoredConnectionCount() const;
  int requestCount() const;
  int unmodelledRequests() const;

  /// Closes the connection of the current client, as a drive that resets does. A client counts as
  /// current once the fake accepted it, which a reply to its first request proves.
  void dropClient();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mm::comm::testing
