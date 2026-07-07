#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "comm/fieldbus_driver.h"

struct ecx_context;

namespace mm::comm::soem {

/// @brief Construction-time configuration for @c SoemFieldbusDriver.
///
/// Groups the SOEM/EtherCAT-specific knobs so they stay off the @c FieldbusDriver interface and off
/// other drivers (a future @c SpoeDriver takes IP addresses and has no concept of a mailbox-status
/// FMMU). The composition root (@c main.cc) fills this from the SOEM slice of the config file.
struct SoemFieldbusDriverConfig {
  /// Resolved OS network interface name (e.g. @c "eth0", @c "enp3s0").
  std::string ifname;
  /// Keep SOEM 2.0's mailbox-status FMMU active — the extra input FMMU it maps the SM1 mailbox-
  /// status register (0x080D) into the cyclic image on every mailbox slave, letting the master
  /// notice a waiting mailbox message without a separate read. Motion Master does not use that
  /// optimisation, and on TI PRU-ICSS ESCs a register-space FMMU inside an LRW is fatal (every
  /// cyclic frame is dropped, SAFE-OP → OP fails). Default false ⇒ the FMMU is deactivated after
  /// mapping. Set true only for hardware that both needs and supports it.
  bool mailboxStatusFmmu = false;
};

/// @brief SOEM-backed EtherCAT fieldbus driver.
///
/// Owns one @c ecx_contextt master context and its PDO I/O map.  @c App
/// creates exactly one instance and injects it into @c DeviceManager and
/// @c GameLoop.
class SoemFieldbusDriver : public FieldbusDriver {
 public:
  /// @brief Constructs the driver from its SOEM-specific configuration.
  /// @param config  Network interface plus SOEM tuning knobs (see @c SoemFieldbusDriverConfig).
  explicit SoemFieldbusDriver(SoemFieldbusDriverConfig config);

  /// @brief Closes the NIC if @c init() succeeded.
  ~SoemFieldbusDriver() override;

  SoemFieldbusDriver(const SoemFieldbusDriver&) = delete;
  SoemFieldbusDriver& operator=(const SoemFieldbusDriver&) = delete;

  /// @brief Opens the NIC and initialises the SOEM master context.
  ///
  /// Must be called before any other driver method.  Device discovery and state
  /// transitions are performed by separate functions after a successful init.
  ///
  /// @return Void on success, or an error string if the interface cannot be opened.
  std::expected<void, std::string> init() override;

  /// @brief Scans the bus for slaves and configures their sync managers and FMMUs.
  ///
  /// Sets @c manualstatechange so slaves remain in INIT after the scan —
  /// state transitions are left to the caller.
  ///
  /// @return Number of slaves found (0 is a valid result — an empty/unpowered bus), or an error
  ///         string if the underlying scan fails.
  std::expected<int, std::string> scan() override;

  /// @brief Returns the immutable identity fields for the slave at @p position.
  /// @param position  1-based slave position on the bus.
  SlaveInfo slaveInfo(uint16_t position) const override;

  /// @copydoc FieldbusDriver::slaveState
  uint16_t slaveState(uint16_t position) const override;

  /// @copydoc FieldbusDriver::configureProcessData
  std::expected<void, std::string> configureProcessData() override;

  /// @copydoc FieldbusDriver::processDataLayout
  PdoLayout processDataLayout() override;

  /// @copydoc FieldbusDriver::busConfig
  std::vector<SlaveConfig> busConfig() const override;

  /// @brief Copies @p outputs into the IOmap, sends and receives, copies inputs back out.
  ///
  /// Called once per @c GameLoop cycle.  Must not be called before
  /// @c configureProcessData() or after @c stop().
  ///
  /// @param outputs  Output image to send; size must equal @c PdoLayout::outputBytes.
  /// @param inputs   Buffer receiving the input image; size must equal @c PdoLayout::inputBytes.
  /// @return The transaction working counter, or 0 if not initialised.
  int exchangeProcessData(std::span<const uint8_t> outputs, std::span<uint8_t> inputs) override;

  /// @brief Closes the NIC and releases all driver resources.
  void stop() override;

  /// @copydoc FieldbusDriver::readStates
  std::expected<std::vector<FieldbusDriver::SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) override;

  /// @copydoc FieldbusDriver::readDiagnostics
  std::expected<std::vector<SlaveDiagnostics>, std::string> readDiagnostics(
      const std::vector<uint16_t>& positions) override;

  /// @copydoc FieldbusDriver::readDcSync
  std::expected<std::vector<DcSyncDiagnostics>, std::string> readDcSync(
      const std::vector<uint16_t>& positions) override;

  /// @copydoc FieldbusDriver::readSdo
  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slavePosition, uint16_t index,
                                                           uint8_t subindex) override;

  /// @copydoc FieldbusDriver::readSdoComplete
  std::expected<std::vector<uint8_t>, std::string> readSdoComplete(uint16_t slavePosition,
                                                                   uint16_t index) override;

  /// @copydoc FieldbusDriver::writeSdo
  std::expected<void, std::string> writeSdo(uint16_t slavePosition, uint16_t index,
                                            uint8_t subindex,
                                            std::span<const uint8_t> data) override;

  /// @copydoc FieldbusDriver::readObjectDictionary
  std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(
      uint16_t slavePosition) override;

  /// @copydoc FieldbusDriver::readSii
  std::expected<std::vector<uint8_t>, std::string> readSii(uint16_t slavePosition) override;

  /// @copydoc FieldbusDriver::writeSii
  std::expected<void, std::string> writeSii(uint16_t slavePosition,
                                            std::span<const uint8_t> data) override;

  /// @copydoc FieldbusDriver::readFile
  std::expected<std::vector<uint8_t>, std::string> readFile(uint16_t slavePosition,
                                                            const std::string& filename) override;

  /// @copydoc FieldbusDriver::writeFile
  std::expected<void, std::string> writeFile(uint16_t slavePosition, const std::string& filename,
                                             std::span<const uint8_t> data) override;

  /// @brief Reads bytes from an ESC register via FPRD.
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address.
  /// @param data           Output buffer; its size determines how many bytes are read.
  /// @return Void on success, or an error string if the working counter is not 1.
  std::expected<void, std::string> readRegister(uint16_t slavePosition, uint16_t address,
                                                std::span<uint8_t> data) override;

  /// @brief Writes bytes to an ESC register via FPWR.
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address.
  /// @param data           Bytes to write.
  /// @return Void on success, or an error string if the working counter is not 1.
  std::expected<void, std::string> writeRegister(uint16_t slavePosition, uint16_t address,
                                                 std::span<const uint8_t> data) override;

  /// @copydoc FieldbusDriver::processDataWatchdog
  std::expected<ProcessDataWatchdogConfig, std::string> processDataWatchdog(
      uint16_t slavePosition) override;

  /// @copydoc FieldbusDriver::setProcessDataWatchdog
  std::expected<ProcessDataWatchdogConfig, std::string> setProcessDataWatchdog(
      uint16_t slavePosition, std::chrono::nanoseconds timeout) override;

  /// @brief Returns the number of discovered slaves, or 0 before discovery.
  int slaveCount() const;

  /// @copydoc FieldbusDriver::transitionToState
  void transitionToState(
      const std::vector<uint16_t>& positions, std::optional<EtherCatState> requiredState,
      EtherCatState targetState, std::chrono::steady_clock::duration timeout,
      std::chrono::steady_clock::duration resendInterval = std::chrono::seconds(2),
      std::function<void()> tick = nullptr, std::function<bool()> shouldAbort = nullptr) override;

 private:
  /// @brief Deactivates SOEM 2.0's mailbox-status FMMU on every slave after a successful map.
  ///
  /// SOEM's @c ecx_config_create_mbxstatus_mappings programs an extra input FMMU mapping the SM1
  /// mailbox-status register (@c ECT_REG_SM1STAT, 0x080D) into the cyclic logical image on each
  /// mailbox-capable slave. Motion Master never consumes it (SDO/FoE poll the mailbox directly),
  /// and on TI PRU-ICSS ESCs a register-space FMMU inside an LRW kills every cyclic frame. Clears
  /// the FMMU's active bit both in the cached slavelist and on the ESC, reversing SOEM's WKC
  /// bookkeeping for input-less slaves so @c processDataLayout's expected WKC stays consistent.
  /// A no-op when @c mailboxStatusFmmu_ is set. Caller must hold @c socketMutex_.
  std::expected<void, std::string> deactivateMailboxStatusFmmus();

  std::string ifname_;
  // Keep SOEM 2.0's mailbox-status FMMU active (see the constructor). Default false: the FMMU is
  // deactivated after every map by deactivateMailboxStatusFmmus().
  bool mailboxStatusFmmu_ = false;
  // ecx_contextt is several hundred KB (EC_MAXSLAVE slave entries) — heap-
  // allocated and null until init() succeeds.
  std::unique_ptr<ecx_context> ctx_;
  // EtherCAT IOmap: ecx_config_map_group lays the whole bus's process data out here as
  // [all outputs | all inputs]. Sized to kMaxProcessImageBytes so it matches the cap
  // configureProcessData() enforces and the ProcessBuffer snapshots layered on top — the
  // three must agree, or a bus the rest of the stack accepts would overflow or be rejected
  // here. At ~160 bytes per direction per SOMANET axis this holds ~100 fully-loaded axes.
  uint8_t map_[kMaxProcessImageBytes]{};
  // 1-based positions whose context currently holds BOOT-sized mailbox sync
  // managers (set when we drive a slave into BOOT). ecx_config_init programs the
  // correct PRE-OP mailbox SMs for every slave during scan(), so a fresh-scan
  // INIT→PRE-OP needs no reprogramming; only a slave returning from BOOT (e.g.
  // after a firmware download) carries stale BOOT SMs that must be reset.
  // Guarded by socketMutex_. Cleared by scan() (which re-runs ecx_config_init).
  std::set<uint16_t> bootMailboxSlaves_;
};

}  // namespace mm::comm::soem
