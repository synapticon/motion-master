#pragma once

// The fieldbus driver for SPoE: one TCP connection to the netX of each drive.
//
// Where the SPoE specification and the firmware disagree, this driver follows the firmware. The
// differences are listed in issue #36.
//
// A position is the place of an address in the configured list, and it never changes. A drive that
// does not answer keeps its position with no state known, so the positions after it stay where
// they are.
//
// There is no ESC, so SII, ESC registers, DC and the ESC diagnostics answer "not supported".

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/spoe_frame.h"

namespace mm::comm::spoe {

/// `ETHERNET_PDO_MODE_*` in the firmware.
enum class PdoMode : uint8_t {
  kNone = 0x00,
  kMonitor = 0x01,
  kControl = 0x02,
};

/// Who owns the drive's state. In Monitor mode a PLC or another master owns it, and Motion Master
/// only parametrises and observes. In Control mode Motion Master owns it.
enum class SpoeMode : uint8_t {
  kMonitor,
  kControl,
};

struct SpoeFieldbusDriverConfig {
  /// One IPv4 or IPv6 address per drive. The list order is the position order.
  std::vector<std::string> hosts;
  uint16_t port = 8080;
  SpoeMode mode = SpoeMode::kMonitor;
  /// The Control-mode watchdog. The firmware raises a smaller value to 50 ms.
  uint32_t watchdogMs = 75;
  /// The time a drive has to answer one request. The specification suggests 1 s.
  std::chrono::milliseconds requestTimeout{1000};
  std::chrono::milliseconds connectTimeout{1000};
};

/// The SPoE protocol versions this driver speaks. The firmware reports 0x0102.
constexpr uint16_t kSupportedProtocolVersion = 0x0102;

/// The size of one parameter-list entry, the firmware's `struct _sdoinfo_entry_description` as it
/// lies in memory. The layout follows the natural alignment of its members, which is not confirmed
/// on hardware.
constexpr std::size_t kParameterEntrySize = 68;

/// One entry of the parameter list.
struct ParameterEntry {
  OdEntry entry;
  /// The subindex count, which the firmware reports on subindex 0 only.
  uint32_t subindexCount = 0;
};

/// Decodes whole parameter-list entries and ignores a short tail. An entry whose index is zero is
/// skipped, because the firmware clears a description before it fills it, and sends it whether
/// the entry exists or not.
std::vector<ParameterEntry> decodeParameterEntries(std::span<const uint8_t> bytes);

/// Names an SPoE SDO status, which is one of the firmware's `SDO_REQUEST_ERROR_*` codes.
std::string sdoStatusName(uint16_t status);

class SpoeFieldbusDriver : public FieldbusDriver {
 public:
  explicit SpoeFieldbusDriver(SpoeFieldbusDriverConfig config);
  ~SpoeFieldbusDriver() override;

  SpoeFieldbusDriver(const SpoeFieldbusDriver&) = delete;
  SpoeFieldbusDriver& operator=(const SpoeFieldbusDriver&) = delete;

  std::expected<void, std::string> init() override;
  std::expected<int, std::string> scan() override;
  SlaveInfo slaveInfo(uint16_t position) const override;
  uint16_t slaveState(uint16_t position) const override;
  uint16_t mailboxProtocols(uint16_t position) const override;

  std::expected<void, std::string> configureProcessData() override;
  PdoLayout processDataLayout() override;
  int exchangeProcessData(std::span<const uint8_t> outputs, std::span<uint8_t> inputs) override;
  void stop() override;

  std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override;

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slavePosition, uint16_t index,
                                                           uint8_t subindex) override;
  std::expected<void, std::string> writeSdo(uint16_t slavePosition, uint16_t index,
                                            uint8_t subindex,
                                            std::span<const uint8_t> data) override;
  std::expected<OdRead, std::string> readObjectDictionary(uint16_t slavePosition) override;

  std::expected<std::vector<uint8_t>, FoeError> readFile(uint16_t slavePosition,
                                                         const std::string& filename) override;
  std::expected<void, FoeError> writeFile(uint16_t slavePosition, const std::string& filename,
                                          std::span<const uint8_t> data) override;

  std::expected<void, std::string> readRegister(uint16_t slavePosition, uint16_t address,
                                                std::span<uint8_t> data) override;
  std::expected<void, std::string> writeRegister(uint16_t slavePosition, uint16_t address,
                                                 std::span<const uint8_t> data) override;

  void transitionToState(const std::vector<uint16_t>& positions,
                         std::optional<EtherCatState> requiredState, EtherCatState targetState,
                         std::chrono::steady_clock::duration timeout,
                         std::chrono::steady_clock::duration resendInterval,
                         std::function<void()> tick, std::function<bool()> shouldAbort) override;

  std::optional<std::string> stateChangeRefusal() const override;

 private:
  struct Drive;

  Drive* driveAt(uint16_t position) const;
  void closeAll();
  std::expected<Frame, std::string> request(Drive& drive, MessageType type,
                                            std::span<const uint8_t> data, uint16_t status = 0);
  std::expected<std::vector<uint8_t>, std::string> readSdoFrom(Drive& drive, uint16_t index,
                                                               uint8_t subindex);
  void connectAndIdentify(Drive& drive);

  SpoeFieldbusDriverConfig config_;
  // Sized once in the constructor and never resized, so a position indexes it without a lock.
  std::vector<std::unique_ptr<Drive>> drives_;
};

}  // namespace mm::comm::spoe
