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
// Process data and FoE are not supported yet.

#include <atomic>
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

namespace mm::comm::igh {

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

 private:
  // What scan() reads once per slave and the accessors serve without bus I/O.
  struct Slave {
    SlaveInfo info;
    uint16_t mailboxProtocols = 0;
    bool sdoInfo = false;   ///< Whether the master reads the slave's object dictionary.
    uint16_t sdoCount = 0;  ///< Objects in the master's copy of the dictionary when read.
  };

  std::expected<void, std::string> requireOpen() const;
  // Reads one slave from the master and stores its AL state for slaveState().
  std::expected<Slave, std::string> readSlave(uint16_t position) const;
  std::expected<void, std::string> requestState(uint16_t position, EtherCatState state) const;
  // Waits until the master's copy of the dictionary stops growing, and returns its object count.
  std::expected<uint16_t, std::string> waitForDictionary(uint16_t position) const;
  void closeDevice();

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
};

}  // namespace mm::comm::igh
