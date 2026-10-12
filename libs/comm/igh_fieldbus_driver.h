#pragma once

// The fieldbus driver for the IgH EtherCAT Master. The master runs in the Linux kernel, and this
// driver talks to it through the ioctl interface of its character device, /dev/EtherCAT<n>, as the
// `ethercat` command-line tool does.
//
// The ioctl interface is internal to IgH and changes with the module version. init() compares the
// module's ioctl version with the one this driver was built against and refuses a mismatch.
//
// IgH owns the AL state of every slave. It holds a requested state per slave and its state machine
// drives the slave to that state, and drives it back after the slave leaves it. transitionToState
// sets the requested state. So a state reached here is held by the master, which SOEM does not do.
// After a scan the master requests PRE-OP for every slave, so a scanned slave does not stay in
// INIT.
//
// The ioctl interface drops the object code and the PDO-mappable and backup access bits of every
// dictionary entry. So a dictionary read here is never cached, and a cache written by another
// driver serves the full description.
//
// Process data runs through two domains, one for outputs and one for inputs. A domain that carries
// both directions sends LRW, which a TI PRU-ICSS ESC cannot process.
//
// IgH changes the process-data layout only while it is not active, so configureProcessData
// releases and requests it again first, and every slave drops to PRE-OP. Activation requests OP for
// every configured slave. The driver then requests again the state each slave had been asked for,
// and waits until each slave is back in that state.
//
// While the master is active, its state machine moves only when the application sends and receives.
// SDO, state and register requests would wait for ever while the RT loop does not exchange. A pump
// thread sends and receives in that time, and pulls back a slave that the master pushed above the
// state it was asked for.
//
// The driver holds the master only between configureProcessData and the next scan() or stop(),
// because a held master that is not active ignores every rescan request.
//
// SII comes from the copy the master read at the last scan, not from the EEPROM. A write goes to
// the EEPROM, and the master does not update its copy, so readSii returns the old image until the
// next scan.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "comm/fieldbus_driver.h"

struct ec_master;
struct ec_domain;

namespace mm::comm::igh {

/// Where one slave's process data sits in the output domain and the input domain.
struct IghSlaveWindow {
  uint16_t slavePosition = 0;
  bool hasOutputs = false;    ///< The slave has an output sync manager in the output domain.
  uint32_t outputOffset = 0;  ///< Byte offset in the output domain.
  uint32_t outputBits = 0;    ///< Mapped output bits.
  bool hasInputs = false;     ///< The slave has an input sync manager in the input domain.
  uint32_t inputOffset = 0;   ///< Byte offset in the input domain.
  uint32_t inputBits = 0;     ///< Mapped input bits.
};

/// @brief Builds the layout from the slaves' domain windows. The output image is the output
///        domain and the input image is the input domain, byte for byte.
///
/// The expected working counter counts an output slave twice and an input slave once. That is
/// what an LRW datagram gives, and exchangeProcessData weighs the LWR and LRD counters the same
/// way, so the node layer's per-state expectation applies unchanged.
PdoLayout buildIghLayout(std::span<const IghSlaveWindow> windows, uint32_t outputDomainBytes,
                         uint32_t inputDomainBytes);

struct IghFieldbusDriverConfig {
  /// The IgH master to use, as in /dev/EtherCAT<masterIndex>.
  unsigned masterIndex = 0;
  /// The longest time readObjectDictionary waits for the master to finish reading a dictionary.
  /// The master starts that read on its own, 3 s after a slave reaches PRE-OP.
  std::chrono::milliseconds dictionaryTimeout{60000};
};

class IghFieldbusDriver : public FieldbusDriver {
 public:
  explicit IghFieldbusDriver(IghFieldbusDriverConfig config);
  ~IghFieldbusDriver() override;

  IghFieldbusDriver(const IghFieldbusDriver&) = delete;
  IghFieldbusDriver& operator=(const IghFieldbusDriver&) = delete;

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
  std::expected<std::vector<SlaveDiagnostics>, std::string> readDiagnostics(
      const std::vector<uint16_t>& positions) override;
  std::expected<std::vector<DcSyncDiagnostics>, std::string> readDcSync(
      const std::vector<uint16_t>& positions) override;

  std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slavePosition, uint16_t index,
                                                           uint8_t subindex) override;
  std::expected<void, std::string> writeSdo(uint16_t slavePosition, uint16_t index,
                                            uint8_t subindex,
                                            std::span<const uint8_t> data) override;
  std::expected<OdRead, std::string> readObjectDictionary(uint16_t slavePosition) override;

  std::expected<std::vector<uint8_t>, std::string> readSii(uint16_t slavePosition) override;
  std::expected<void, std::string> writeSii(uint16_t slavePosition,
                                            std::span<const uint8_t> data) override;

  std::expected<std::vector<uint8_t>, FoeError> readFile(uint16_t slavePosition,
                                                         const std::string& filename) override;
  std::expected<void, FoeError> writeFile(uint16_t slavePosition, const std::string& filename,
                                          std::span<const uint8_t> data) override;

  std::expected<void, std::string> readRegister(uint16_t slavePosition, uint16_t address,
                                                std::span<uint8_t> data) override;
  std::expected<void, std::string> writeRegister(uint16_t slavePosition, uint16_t address,
                                                 std::span<const uint8_t> data) override;

  std::expected<ProcessDataWatchdogConfig, std::string> processDataWatchdog(
      uint16_t slavePosition) override;
  std::expected<ProcessDataWatchdogConfig, std::string> setProcessDataWatchdog(
      uint16_t slavePosition, std::chrono::nanoseconds timeout) override;

  void transitionToState(const std::vector<uint16_t>& positions,
                         std::optional<EtherCatState> requiredState, EtherCatState targetState,
                         std::chrono::steady_clock::duration timeout,
                         std::chrono::steady_clock::duration resendInterval,
                         std::function<void()> tick, std::function<bool()> shouldAbort) override;

 private:
  // What scan() reads once per slave and the accessors serve without bus I/O.
  struct Slave {
    SlaveInfo info;
    uint16_t mailboxProtocols = 0;
    bool sdoInfo = false;   ///< Whether the master reads the slave's object dictionary.
    uint16_t sdoCount = 0;  ///< Objects in the master's copy of the dictionary when read.
  };

  // One sync manager that carries process data, with the PDOs assigned to it.
  struct SyncManagerPdos;

  std::expected<void, std::string> requireOpen() const;
  std::expected<std::vector<SyncManagerPdos>, std::string> readPdoConfiguration(uint16_t position,
                                                                                uint16_t protocols);
  std::expected<std::vector<IghSlaveWindow>, std::string> configureSlaves(
      const std::vector<Slave>& slaves);
  void restoreRequestedStates(const std::vector<uint16_t>& positions);
  void runPump(const std::stop_token& stopToken);
  void releaseMaster();
  // Stops the pump, releases the master and closes the device. Not virtual, so the destructor can
  // call it.
  void shutdown();
  // Releases the master with masterMutex_ held.
  void releaseMasterLocked();
  // Waits for a rescan the master may start after its release, and for its end.
  std::expected<void, std::string> waitForSettledBus() const;
  // Reads one slave from the master and stores its AL state for slaveState().
  std::expected<Slave, std::string> readSlave(uint16_t position) const;
  std::expected<void, std::string> requestState(uint16_t position, EtherCatState state) const;
  // Waits until the master's copy of the dictionary stops growing, and returns its object count.
  std::expected<uint16_t, std::string> waitForDictionary(uint16_t position) const;

  IghFieldbusDriverConfig config_;
  // Opened by init() and closed by stop(). The kernel serialises every request on it, so the
  // driver takes no lock around an ioctl.
  int fd_ = -1;
  // Replaced by scan(). Guarded by controlPlaneMutex_.
  std::vector<Slave> slaves_;
  // The last AL status read for each position, indexed by position. slaveState() must not block,
  // so it reads this without a lock. Allocated once at the largest position IgH can address and
  // never reallocated, so a reader can index it at any time.
  std::unique_ptr<std::atomic<uint16_t>[]> slaveStates_;
  // The state Motion Master last asked for, per position, sized like slaveStates_. The master
  // overrides a requested state when it activates, and this is what the driver restores.
  // Sequentially consistent, so the pump never sees a state a slave reached together with the
  // request before it, which would make it pull back a transition Motion Master asked for.
  std::unique_ptr<std::atomic<uint8_t>[]> requestedStates_;

  // Guards master_, the domains, configuredPositions_ and layout_ against the pump thread. Held
  // by the pump for one send and receive, and by configureProcessData, scan() and stop() while they
  // change the master.
  mutable std::mutex masterMutex_;
  ec_master* master_ = nullptr;  // Held from configureProcessData until releaseMaster().
  ec_domain* outputDomain_ = nullptr;
  ec_domain* inputDomain_ = nullptr;
  uint8_t* outputData_ = nullptr;  // The output domain's memory, mapped after activation.
  uint8_t* inputData_ = nullptr;
  std::vector<uint16_t> configuredPositions_;
  PdoLayout layout_;
  // True between activation and the next deactivation. The RT thread reads it without a lock.
  std::atomic<bool> active_ = false;
  // steady_clock nanoseconds of the last exchangeProcessData. The pump sends only when the RT loop
  // has stopped.
  std::atomic<int64_t> lastExchangeNs_ = 0;
  std::jthread pump_;
};

}  // namespace mm::comm::igh
