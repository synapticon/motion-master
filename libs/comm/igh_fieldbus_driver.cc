#include "comm/igh_fieldbus_driver.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "comm/sdo_abort_codes.h"
#include "comm/sdo_log.h"
#include "master/ioctl.h"

namespace mm::comm::igh {

namespace {

// IgH addresses a slave with a 16-bit position, so this many slots cover every position.
constexpr std::size_t kStateSlots = std::size_t{std::numeric_limits<uint16_t>::max()} + 1;

// The same cap the SOEM driver uses. IgH refuses an object larger than the buffer rather than
// truncating it, so the cap cannot return short data.
constexpr std::size_t kSdoBufferBytes = 4096;

// How long scan() waits for the master to start the scan it requested. The master's state machine
// checks for a rescan request in every cycle of its idle loop, so it starts within milliseconds
// when the link is up. When it does not start at all, there is nothing to scan.
constexpr auto kScanStartWait = std::chrono::seconds(1);
constexpr auto kScanTimeout = std::chrono::seconds(30);
constexpr auto kScanPollInterval = std::chrono::milliseconds(10);

// How often transitionToState reads the states of the slaves it waits for.
constexpr auto kStatePollInterval = std::chrono::milliseconds(10);

// The master reads a dictionary entry by entry and reports no end. One stalled entry costs it one
// CoE dictionary timeout, which is 1 s (EC_FSM_COE_DICT_TIMEOUT in master/fsm_coe.c). So the entry
// count must stay the same for longer than that before the read counts as finished.
constexpr auto kDictionaryPollInterval = std::chrono::seconds(1);
constexpr int kDictionaryStableReads = 2;

// How long a refusal of a probe past the reported subindex count may take before probing stops for
// that slave. An SDO abort comes back in a few milliseconds, and a slave that does not answer costs
// a whole mailbox timeout. The threshold only has to sit between the two.
constexpr auto kProbeRefusalBudget = std::chrono::milliseconds(100);

// The AL Status register (0x0130) and the AL Status Code register (0x0134), read in one transfer.
constexpr uint16_t kAlStatusRegister = 0x0130;
constexpr std::size_t kAlStatusBytes = 6;

constexpr uint16_t kMailboxCoe = 0x0004;

// The pump sends only after the RT loop has not exchanged for this long, so the two rarely send in
// the same cycle. The kernel serialises them when they do.
constexpr auto kPumpIdleThreshold = std::chrono::milliseconds(3);
constexpr auto kPumpInterval = std::chrono::milliseconds(1);
// How often the pump compares each slave's state with the state it was asked for.
constexpr auto kStateKeeperInterval = std::chrono::milliseconds(10);

// After activation the master requests OP for every configured slave once, from its state machine,
// after it writes the DC system times (master/fsm_master.c). That request comes some cycles after
// the activation and replaces the state the driver requested. So the driver requests each slave's
// state again for at least this long. It is an estimate from the source, not a measurement.
constexpr auto kActivationRequestWindow = std::chrono::milliseconds(500);
constexpr auto kRestoreInterval = std::chrono::milliseconds(2);
constexpr auto kRestoreTimeout = std::chrono::seconds(10);

// How long the driver watches for a rescan after it releases the master. The master's idle thread
// starts a scheduled rescan within milliseconds. Measured on a SOMANET Circulo, the lost frame and
// the rescan request came within the same second as the release.
constexpr auto kSettleWindow = std::chrono::milliseconds(300);

constexpr int kPdoReadAttempts = 3;
constexpr auto kPdoReadRetryDelay = std::chrono::milliseconds(100);

// ESC Sync Manager control register (ETG.1000.4): bits 1:0 are the mode, 0 buffered and 2
// mailbox. Bits 3:2 are the direction, 0 read by EtherCAT (inputs) and 1 written by EtherCAT
// (outputs).
constexpr uint8_t kSmModeMask = 0x03;
constexpr uint8_t kSmModeBuffered = 0x00;
constexpr uint8_t kSmDirectionShift = 2;
constexpr uint8_t kSmDirectionWrite = 0x01;

int64_t steadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Orders the states the way a slave climbs them. BOOT is outside that order.
int stateRank(EtherCatState state) {
  switch (state) {
    case EtherCatState::Init:
      return 1;
    case EtherCatState::PreOp:
      return 2;
    case EtherCatState::SafeOp:
      return 3;
    case EtherCatState::Op:
      return 4;
    case EtherCatState::Boot:
      return 0;
  }
  return 0;
}

// Runs one ioctl and returns 0 or the errno it failed with. A signal can interrupt the kernel while
// a request waits in the queue. The kernel then removes the request, so a retry is safe.
// The request type is whatever the C library declares for ioctl(), so it is a template parameter.
template <typename Request, typename T>
int callIoctl(int fd, Request request, T* argument) {
  while (::ioctl(fd, request, argument) == -1) {
    if (errno != EINTR) {
      return errno;
    }
  }
  return 0;
}

template <typename Request>
int callIoctl(int fd, Request request) {
  while (::ioctl(fd, request) == -1) {
    if (errno != EINTR) {
      return errno;
    }
  }
  return 0;
}

std::string errnoText(int error) { return std::generic_category().message(error); }

// The reason a mailbox transfer failed. The kernel copies the abort code out when the slave
// answered with an SDO abort, and leaves it 0 otherwise.
std::string sdoFailureSuffix(int error, uint32_t abortCode) {
  if (abortCode != 0) {
    const std::string_view reason = sdoAbortCodeDescription(abortCode);
    return reason.empty() ? std::format(" (SDO abort 0x{:08X})", abortCode)
                          : std::format(" (SDO abort 0x{:08X}: {})", abortCode, reason);
  }
  return std::format(" ({})", errnoText(error));
}

// IgH holds strings in fixed arrays of signed bytes, which need not end in a NUL.
template <std::size_t N>
std::string fixedString(const int8_t (&chars)[N]) {
  const auto* begin = reinterpret_cast<const char*>(chars);
  return {begin, strnlen(begin, N)};
}

template <std::size_t N>
std::string fixedString(const char (&chars)[N]) {
  return {chars, strnlen(chars, N)};
}

// An object whose layout ETG.1000.6 fixes, so an entry the master did not describe can be filled
// in from the standard rather than from the slave.
struct StandardObject {
  uint16_t objectCode = 0;
  uint16_t entryDataType = 0;  // Every subindex above 0.
  uint16_t entryBitLength = 0;
};

constexpr uint16_t kObjectCodeArray = 0x0008;
constexpr uint16_t kObjectCodeRecord = 0x0009;
constexpr uint16_t kDataTypeUnsigned16 = 0x0006;
constexpr uint16_t kDataTypeUnsigned32 = 0x0007;

// ETG.1000.6 Tables 74 and 75 (Receive and Transmit PDO Mapping) and Table 77 (Sync Manager
// Channel 0-31). These are the objects whose size SOMANET firmware reports short, because their
// subindex 0 holds a count that changes at runtime.
std::optional<StandardObject> standardObject(uint16_t index) {
  if ((index >= 0x1600 && index <= 0x17FF) || (index >= 0x1A00 && index <= 0x1BFF)) {
    return StandardObject{.objectCode = kObjectCodeRecord,
                          .entryDataType = kDataTypeUnsigned32,
                          .entryBitLength = 32};
  }
  if (index >= 0x1C10 && index <= 0x1C2F) {
    return StandardObject{
        .objectCode = kObjectCodeArray, .entryDataType = kDataTypeUnsigned16, .entryBitLength = 16};
  }
  return std::nullopt;
}

uint16_t readLe16(std::span<const uint8_t> bytes) {
  return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

}  // namespace

PdoLayout buildIghLayout(std::span<const IghSlaveWindow> windows, uint32_t outputDomainBytes,
                         uint32_t inputDomainBytes) {
  PdoLayout layout;
  layout.outputBytes = outputDomainBytes;
  layout.inputBytes = inputDomainBytes;
  for (const IghSlaveWindow& window : windows) {
    // A direction with fewer than 8 mapped bits reports no bytes, as the interface asks. Its sync
    // manager still answers the datagram, so it still counts toward the working counter.
    const uint32_t outputBytes = window.hasOutputs ? window.outputBits / 8 : 0;
    const uint32_t inputBytes = window.hasInputs ? window.inputBits / 8 : 0;
    layout.slaves.push_back(SlaveIo{
        .slavePosition = window.slavePosition,
        .outputOffset = outputBytes > 0 ? window.outputOffset : 0,
        .outputBytes = outputBytes,
        .inputOffset = inputBytes > 0 ? window.inputOffset : 0,
        .inputBytes = inputBytes,
    });
    layout.expectedWkc += (window.hasOutputs ? 2 : 0) + (window.hasInputs ? 1 : 0);
  }
  return layout;
}

struct IghFieldbusDriver::SyncManagerPdos {
  struct Pdo {
    uint16_t index = 0;
    std::vector<ec_pdo_entry_info_t> entries;
  };
  uint8_t index = 0;
  ec_direction_t direction = EC_DIR_INVALID;
  std::vector<Pdo> pdos;
};

IghFieldbusDriver::IghFieldbusDriver(IghFieldbusDriverConfig config)
    : config_(config),
      slaveStates_(std::make_unique<std::atomic<uint16_t>[]>(kStateSlots)),
      requestedStates_(std::make_unique<std::atomic<uint8_t>[]>(kStateSlots)) {}

IghFieldbusDriver::~IghFieldbusDriver() { shutdown(); }

std::expected<void, std::string> IghFieldbusDriver::init() {
  if (fd_ != -1) {
    return std::unexpected("the IgH master is already open");
  }
  const std::string path = std::format("/dev/EtherCAT{}", config_.masterIndex);
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd == -1) {
    const int error = errno;
    return std::unexpected(std::format("cannot open {}: {}", path, errnoText(error)));
  }
  ec_ioctl_module_t module{};
  if (const int error = callIoctl(fd, EC_IOCTL_MODULE, &module); error != 0) {
    ::close(fd);
    return std::unexpected(
        std::format("cannot read the module version from {}: {}", path, errnoText(error)));
  }
  if (module.ioctl_version_magic != EC_IOCTL_VERSION_MAGIC) {
    ::close(fd);
    return std::unexpected(std::format(
        "the loaded IgH module speaks ioctl version {}, and Motion Master was built against "
        "version {}. Build Motion Master against the source of the loaded module.",
        module.ioctl_version_magic, EC_IOCTL_VERSION_MAGIC));
  }
  // ec_ioctl_master_t holds one record per Ethernet device, so its size depends on how the module
  // was configured. A different size gives a different request number, which the module rejects.
  // Reading it here finds that mismatch at init, not at the first scan.
  ec_ioctl_master_t master{};
  if (const int error = callIoctl(fd, EC_IOCTL_MASTER, &master); error != 0) {
    ::close(fd);
    return std::unexpected(std::format(
        "cannot read master {}: {}. When the error is about an inappropriate ioctl, "
        "MM_IGH_MAX_NUM_DEVICES does not match the --with-devices option of the module.",
        config_.masterIndex, errnoText(error)));
  }
  if (ecrt_version_magic() != ECRT_VERSION_MAGIC) {
    ::close(fd);
    return std::unexpected(std::format(
        "the IgH library speaks application interface version 0x{:04X}, and Motion Master was "
        "built against version 0x{:04X}",
        ecrt_version_magic(), ECRT_VERSION_MAGIC));
  }
  fd_ = fd;
  pump_ = std::jthread([this](const std::stop_token& stopToken) { runPump(stopToken); });
  spdlog::info("IgH master {}: {} slave(s), ioctl version {}", config_.masterIndex,
               master.slave_count, module.ioctl_version_magic);
  return {};
}

std::expected<void, std::string> IghFieldbusDriver::requireOpen() const {
  if (fd_ == -1) {
    return std::unexpected("no driver — call init() first");
  }
  return {};
}

std::expected<IghFieldbusDriver::Slave, std::string> IghFieldbusDriver::readSlave(
    uint16_t position) const {
  ec_ioctl_slave_t data{};
  data.position = static_cast<uint16_t>(position - 1);
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE, &data); error != 0) {
    return std::unexpected(std::format("cannot read slave {}: {}", position, errnoText(error)));
  }
  slaveStates_[position].store(data.al_state, std::memory_order_relaxed);
  return Slave{
      .info =
          SlaveInfo{
              .name = fixedString(data.name),
              .vendorId = data.vendor_id,
              .productCode = data.product_code,
              .revisionNumber = data.revision_number,
              .serialNumber = data.serial_number,
          },
      .mailboxProtocols = data.mailbox_protocols,
      // The master reads a dictionary unless the general category of the SII disables SDO
      // Information, which is the rule master/fsm_master.c applies.
      .sdoInfo = (data.mailbox_protocols & kMailboxCoe) != 0 &&
                 (data.has_general_category == 0 || data.coe_details.enable_sdo_info != 0),
      .sdoCount = data.sdo_count,
  };
}

std::expected<int, std::string> IghFieldbusDriver::scan() {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  // A held master that is not active ignores a rescan request (master/fsm_master.c), and a scan
  // replaces every device anyway. Releasing it deactivates it, and every slave drops to PRE-OP.
  releaseMaster();
  if (const int error = callIoctl(fd_, EC_IOCTL_MASTER_RESCAN); error != 0) {
    return std::unexpected(std::format("cannot request a rescan: {}", errnoText(error)));
  }
  // The request only sets a flag. The scan itself runs later in the master's state machine, so
  // wait until it is seen to start and then to finish.
  const auto started = std::chrono::steady_clock::now();
  bool seenBusy = false;
  ec_ioctl_master_t master{};
  while (true) {
    if (const int error = callIoctl(fd_, EC_IOCTL_MASTER, &master); error != 0) {
      return std::unexpected(std::format("cannot read the master: {}", errnoText(error)));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (master.scan_busy != 0) {
      seenBusy = true;
    } else if (seenBusy || elapsed > kScanStartWait) {
      break;
    }
    if (elapsed > kScanTimeout) {
      return std::unexpected(
          std::format("the IgH master did not finish the scan within {} s", kScanTimeout.count()));
    }
    std::this_thread::sleep_for(kScanPollInterval);
  }

  std::vector<Slave> slaves;
  slaves.reserve(master.slave_count);
  for (uint32_t index = 0; index < master.slave_count; ++index) {
    auto slave = readSlave(static_cast<uint16_t>(index + 1));
    if (!slave) {
      return std::unexpected(slave.error());
    }
    slaves.push_back(std::move(*slave));
  }
  std::size_t previousCount = 0;
  {
    const std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    previousCount = slaves_.size();
    slaves_ = std::move(slaves);
  }
  // A position the new scan did not find has no state. The master requests PRE-OP for every slave
  // it finds.
  for (std::size_t position = master.slave_count + 1; position <= previousCount; ++position) {
    slaveStates_[position].store(0, std::memory_order_relaxed);
    requestedStates_[position].store(0);
  }
  for (std::size_t position = 1; position <= master.slave_count; ++position) {
    requestedStates_[position].store(static_cast<uint8_t>(EtherCatState::PreOp));
  }
  return static_cast<int>(master.slave_count);
}

SlaveInfo IghFieldbusDriver::slaveInfo(uint16_t position) const {
  const std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (position == 0 || position > slaves_.size()) {
    return {};
  }
  return slaves_[position - 1].info;
}

uint16_t IghFieldbusDriver::slaveState(uint16_t position) const {
  return slaveStates_[position].load(std::memory_order_relaxed);
}

uint16_t IghFieldbusDriver::mailboxProtocols(uint16_t position) const {
  const std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (position == 0 || position > slaves_.size()) {
    return 0;
  }
  return slaves_[position - 1].mailboxProtocols;
}

std::expected<std::vector<IghFieldbusDriver::SyncManagerPdos>, std::string>
IghFieldbusDriver::readPdoConfiguration(uint16_t position, uint16_t protocols) {
  ec_ioctl_slave_t slave{};
  slave.position = static_cast<uint16_t>(position - 1);
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE, &slave); error != 0) {
    return std::unexpected(std::format("cannot read slave {}: {}", position, errnoText(error)));
  }
  const auto readUnsigned =
      [this, position](uint16_t index, uint8_t subindex) -> std::expected<uint32_t, std::string> {
    auto bytes = readSdo(position, index, subindex);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    uint32_t value = 0;
    for (std::size_t i = 0; i < bytes->size() && i < sizeof(value); ++i) {
      value |= static_cast<uint32_t>((*bytes)[i]) << (8 * i);
    }
    return value;
  };

  std::vector<SyncManagerPdos> syncManagers;
  for (uint32_t smIndex = 0; smIndex < slave.sync_count; ++smIndex) {
    ec_ioctl_slave_sync_t sync{};
    sync.slave_position = slave.position;
    sync.sync_index = smIndex;
    if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_SYNC, &sync); error != 0) {
      return std::unexpected(std::format("cannot read sync manager {} of slave {}: {}", smIndex,
                                         position, errnoText(error)));
    }
    if ((sync.control_register & kSmModeMask) != kSmModeBuffered) {
      continue;
    }
    SyncManagerPdos sm{
        .index = static_cast<uint8_t>(smIndex),
        .direction = ((sync.control_register >> kSmDirectionShift) & 0x03) == kSmDirectionWrite
                         ? EC_DIR_OUTPUT
                         : EC_DIR_INPUT,
        .pdos = {},
    };
    if ((protocols & kMailboxCoe) != 0) {
      // The assignment and the mapping as the slave holds them now. A mapping written since the
      // scan is in the slave, not in the master's copy.
      const auto assignIndex = static_cast<uint16_t>(0x1C10 + smIndex);
      auto count = readUnsigned(assignIndex, 0);
      if (!count) {
        return std::unexpected(count.error());
      }
      for (uint32_t i = 1; i <= *count; ++i) {
        auto pdoIndex = readUnsigned(assignIndex, static_cast<uint8_t>(i));
        if (!pdoIndex) {
          return std::unexpected(pdoIndex.error());
        }
        SyncManagerPdos::Pdo pdo{.index = static_cast<uint16_t>(*pdoIndex), .entries = {}};
        auto entryCount = readUnsigned(pdo.index, 0);
        if (!entryCount) {
          return std::unexpected(entryCount.error());
        }
        for (uint32_t e = 1; e <= *entryCount; ++e) {
          auto entry = readUnsigned(pdo.index, static_cast<uint8_t>(e));
          if (!entry) {
            return std::unexpected(entry.error());
          }
          pdo.entries.push_back(ec_pdo_entry_info_t{
              .index = static_cast<uint16_t>(*entry >> 16),
              .subindex = static_cast<uint8_t>((*entry >> 8) & 0xFF),
              .bit_length = static_cast<uint8_t>(*entry & 0xFF),
          });
        }
        sm.pdos.push_back(std::move(pdo));
      }
    } else {
      // A slave without CoE has the fixed PDOs its SII declares, which the master read at the scan.
      for (uint32_t p = 0; p < sync.pdo_count; ++p) {
        ec_ioctl_slave_sync_pdo_t pdoInfo{};
        pdoInfo.slave_position = slave.position;
        pdoInfo.sync_index = smIndex;
        pdoInfo.pdo_pos = p;
        if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_SYNC_PDO, &pdoInfo); error != 0) {
          return std::unexpected(
              std::format("cannot read PDO {} of sync manager {} of slave {}: {}", p, smIndex,
                          position, errnoText(error)));
        }
        SyncManagerPdos::Pdo pdo{.index = pdoInfo.index, .entries = {}};
        for (uint32_t e = 0; e < pdoInfo.entry_count; ++e) {
          ec_ioctl_slave_sync_pdo_entry_t entryInfo{};
          entryInfo.slave_position = slave.position;
          entryInfo.sync_index = smIndex;
          entryInfo.pdo_pos = p;
          entryInfo.entry_pos = e;
          if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_SYNC_PDO_ENTRY, &entryInfo);
              error != 0) {
            return std::unexpected(
                std::format("cannot read a PDO entry of slave {}: {}", position, errnoText(error)));
          }
          pdo.entries.push_back(ec_pdo_entry_info_t{.index = entryInfo.index,
                                                    .subindex = entryInfo.subindex,
                                                    .bit_length = entryInfo.bit_length});
        }
        sm.pdos.push_back(std::move(pdo));
      }
    }
    syncManagers.push_back(std::move(sm));
  }
  return syncManagers;
}

std::expected<std::vector<IghSlaveWindow>, std::string> IghFieldbusDriver::configureSlaves(
    const std::vector<Slave>& slaves) {
  std::vector<IghSlaveWindow> windows;
  configuredPositions_.clear();
  for (std::size_t i = 0; i < slaves.size(); ++i) {
    const auto position = static_cast<uint16_t>(i + 1);
    // A slave without a live mailbox cannot be read, and the master would move a configured slave
    // to OP. So only a slave in PRE-OP or above that was not asked to go below joins the layout.
    auto current = readSlave(position);
    if (!current) {
      return std::unexpected(current.error());
    }
    const auto state = alState(slaveStates_[position].load(std::memory_order_relaxed));
    const auto requested = static_cast<EtherCatState>(requestedStates_[position].load());
    if (stateRank(state) < stateRank(EtherCatState::PreOp) ||
        stateRank(requested) < stateRank(EtherCatState::PreOp)) {
      continue;
    }
    // Deactivation stops the master's OP thread and starts its idle thread. A mailbox transfer in
    // that moment loses its frame: on a SOMANET Circulo the first upload after deactivation failed
    // with "Reception of CoE upload response failed: No response" in the kernel log. So the read
    // is tried again.
    auto syncManagers = readPdoConfiguration(position, slaves[i].mailboxProtocols);
    for (int attempt = 1; !syncManagers && attempt < kPdoReadAttempts; ++attempt) {
      spdlog::debug("IgH: reading the PDO configuration of slave {} again: {}", position,
                    syncManagers.error());
      std::this_thread::sleep_for(kPdoReadRetryDelay);
      syncManagers = readPdoConfiguration(position, slaves[i].mailboxProtocols);
    }
    if (!syncManagers) {
      return std::unexpected(syncManagers.error());
    }

    ec_slave_config_t* config =
        ecrt_master_slave_config(master_, 0, static_cast<uint16_t>(position - 1),
                                 slaves[i].info.vendorId, slaves[i].info.productCode);
    if (config == nullptr) {
      return std::unexpected(std::format("cannot configure slave {}", position));
    }
    // The library copies every array into the kernel before it returns, so these only have to live
    // until ecrt_slave_config_pdos returns.
    std::vector<std::vector<ec_pdo_info_t>> pdoInfos;
    std::vector<ec_sync_info_t> syncInfos;
    pdoInfos.reserve(syncManagers->size());
    for (const SyncManagerPdos& sm : *syncManagers) {
      auto& infos = pdoInfos.emplace_back();
      std::ranges::transform(sm.pdos, std::back_inserter(infos), [](const auto& pdo) {
        return ec_pdo_info_t{.index = pdo.index,
                             .n_entries = static_cast<unsigned>(pdo.entries.size()),
                             .entries = pdo.entries.data()};
      });
      syncInfos.push_back(ec_sync_info_t{.index = sm.index,
                                         .dir = sm.direction,
                                         .n_pdos = static_cast<unsigned>(infos.size()),
                                         .pdos = infos.data(),
                                         .watchdog_mode = EC_WD_DEFAULT});
    }
    if (const int error = ecrt_slave_config_pdos(config, static_cast<unsigned>(syncInfos.size()),
                                                 syncInfos.data());
        error != 0) {
      return std::unexpected(
          std::format("cannot configure the PDOs of slave {}: {}", position, errnoText(-error)));
    }

    IghSlaveWindow window{.slavePosition = position};
    for (const SyncManagerPdos& sm : *syncManagers) {
      // Registering the first entry of a sync manager puts the whole sync manager in the domain,
      // and returns where it starts. The sync managers of one slave are registered one after the
      // other, so the domain places them next to each other.
      uint32_t bits = 0;
      for (const auto& pdo : sm.pdos) {
        bits = std::accumulate(
            pdo.entries.begin(), pdo.entries.end(), bits,
            [](uint32_t sum, const ec_pdo_entry_info_t& entry) { return sum + entry.bit_length; });
      }
      const auto firstPdo = std::ranges::find_if(
          sm.pdos, [](const SyncManagerPdos::Pdo& pdo) { return !pdo.entries.empty(); });
      if (firstPdo == sm.pdos.end()) {
        continue;
      }
      const auto first =
          std::pair{static_cast<unsigned>(std::distance(sm.pdos.begin(), firstPdo)), 0U};
      const bool output = sm.direction == EC_DIR_OUTPUT;
      ec_domain_t* domain = output ? outputDomain_ : inputDomain_;
      unsigned bitPosition = 0;
      const int offset = ecrt_slave_config_reg_pdo_entry_pos(config, sm.index, first.first,
                                                             first.second, domain, &bitPosition);
      if (offset < 0) {
        return std::unexpected(std::format("cannot register sync manager {} of slave {}: {}",
                                           sm.index, position, errnoText(-offset)));
      }
      if (output) {
        if (!window.hasOutputs) {
          window.outputOffset = static_cast<uint32_t>(offset);
        }
        window.hasOutputs = true;
        window.outputBits += bits;
      } else {
        if (!window.hasInputs) {
          window.inputOffset = static_cast<uint32_t>(offset);
        }
        window.hasInputs = true;
        window.inputBits += bits;
      }
    }
    configuredPositions_.push_back(position);
    windows.push_back(window);
  }
  return windows;
}

std::expected<void, std::string> IghFieldbusDriver::configureProcessData() {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  std::vector<Slave> slaves;
  {
    const std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    slaves = slaves_;
  }

  const std::lock_guard<std::mutex> lock(masterMutex_);
  // A new layout needs a master that is not active. It is released, not deactivated. Both request
  // PRE-OP for every slave and hand the bus back to the master's idle thread. But at that switch
  // the master can lose a frame, count no responding slave and schedule a rescan. A held master
  // runs that rescan only after the next activation, where it rebuilds the slaves under the new
  // layout. A released master runs it at once, so the driver can wait for it here.
  const bool wasHeld = master_ != nullptr;
  releaseMasterLocked();
  if (wasHeld) {
    if (auto settled = waitForSettledBus(); !settled) {
      return std::unexpected(settled.error());
    }
  }
  // Requesting the master requests PRE-OP for every slave. restoreRequestedStates below returns
  // each slave to the state it was asked for.
  master_ = ecrt_request_master(config_.masterIndex);
  if (master_ == nullptr) {
    return std::unexpected(std::format(
        "cannot request IgH master {}; another application may hold it", config_.masterIndex));
  }

  outputDomain_ = ecrt_master_create_domain(master_);
  inputDomain_ = ecrt_master_create_domain(master_);
  if (outputDomain_ == nullptr || inputDomain_ == nullptr) {
    return std::unexpected("cannot create the IgH domains");
  }
  auto windows = configureSlaves(slaves);
  if (!windows) {
    return std::unexpected(windows.error());
  }
  if (windows->empty()) {
    return std::unexpected("no slave in PRE-OP or above has process data to map");
  }
  if (const int error = ecrt_master_activate(master_); error != 0) {
    return std::unexpected(std::format("cannot activate the IgH master: {}", errnoText(-error)));
  }
  outputData_ = ecrt_domain_data(outputDomain_);
  inputData_ = ecrt_domain_data(inputDomain_);
  const auto outputBytes = static_cast<uint32_t>(ecrt_domain_size(outputDomain_));
  const auto inputBytes = static_cast<uint32_t>(ecrt_domain_size(inputDomain_));
  if (std::size_t{outputBytes} + inputBytes > kMaxProcessImageBytes) {
    return std::unexpected(std::format("the process image needs {} bytes, more than the {} allowed",
                                       outputBytes + inputBytes, kMaxProcessImageBytes));
  }
  layout_ = buildIghLayout(*windows, outputBytes, inputBytes);
  lastExchangeNs_.store(0);
  active_.store(true);
  spdlog::info("IgH: {} slave(s) mapped, {} output and {} input bytes, expected WKC {}",
               windows->size(), outputBytes, inputBytes, layout_.expectedWkc);

  std::vector<uint16_t> positions(slaves.size());
  for (std::size_t i = 0; i < slaves.size(); ++i) {
    positions[i] = static_cast<uint16_t>(i + 1);
  }
  restoreRequestedStates(positions);
  return {};
}

void IghFieldbusDriver::restoreRequestedStates(const std::vector<uint16_t>& positions) {
  // Runs with masterMutex_ held, so the pump does not send. This loop sends instead, because the
  // master's state machine moves only when something does.
  const auto started = std::chrono::steady_clock::now();
  while (true) {
    ecrt_master_receive(master_);
    ecrt_master_send(master_);
    bool allThere = true;
    for (const uint16_t position : positions) {
      const auto requested = static_cast<EtherCatState>(requestedStates_[position].load());
      if (stateRank(requested) == 0 && requested != EtherCatState::Boot) {
        continue;
      }
      if (auto slave = readSlave(position); !slave) {
        allThere = false;
        continue;
      }
      const uint16_t status = slaveStates_[position].load(std::memory_order_relaxed);
      if (alState(status) != requested || alHasError(status)) {
        allThere = false;
      }
      // Requested again on every pass, because the master's own request for OP can arrive at any
      // time inside the window.
      static_cast<void>(requestState(position, requested));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (allThere && elapsed >= kActivationRequestWindow) {
      return;
    }
    if (elapsed >= kRestoreTimeout) {
      for (const uint16_t position : positions) {
        const uint16_t status = slaveStates_[position].load(std::memory_order_relaxed);
        const auto requested = static_cast<EtherCatState>(requestedStates_[position].load());
        if (alState(status) != requested || alHasError(status)) {
          spdlog::warn("IgH: slave {} did not return to {} after the re-map (AL status 0x{:04X})",
                       position, toString(requested), status);
        }
      }
      return;
    }
    std::this_thread::sleep_for(kRestoreInterval);
  }
}

PdoLayout IghFieldbusDriver::processDataLayout() {
  const std::lock_guard<std::mutex> lock(masterMutex_);
  return layout_;
}

int IghFieldbusDriver::exchangeProcessData(std::span<const uint8_t> outputs,
                                           std::span<uint8_t> inputs) {
  // No lock: the caller drains this call before configureProcessData, scan() or stop() changes the
  // master, as the interface requires.
  if (!active_.load(std::memory_order_acquire)) {
    return 0;
  }
  // The frame sent in the previous call is received here, so the inputs are one cycle old.
  ecrt_master_receive(master_);
  ecrt_domain_process(outputDomain_);
  ecrt_domain_process(inputDomain_);
  ec_domain_state_t outputState{};
  ec_domain_state_t inputState{};
  ecrt_domain_state(outputDomain_, &outputState);
  ecrt_domain_state(inputDomain_, &inputState);
  if (inputData_ != nullptr) {
    std::memcpy(inputs.data(), inputData_,
                std::min<std::size_t>(inputs.size(), layout_.inputBytes));
  }
  if (outputData_ != nullptr) {
    std::memcpy(outputData_, outputs.data(),
                std::min<std::size_t>(outputs.size(), layout_.outputBytes));
  }
  ecrt_domain_queue(outputDomain_);
  ecrt_domain_queue(inputDomain_);
  ecrt_master_send(master_);
  lastExchangeNs_.store(steadyNowNs(), std::memory_order_relaxed);
  // Weighed as one LRW datagram would count it. See buildIghLayout.
  return static_cast<int>(outputState.working_counter * 2 + inputState.working_counter);
}

void IghFieldbusDriver::runPump(const std::stop_token& stopToken) {
  auto nextStateCheck = std::chrono::steady_clock::now();
  // The rank each slave had at the previous check. Only a slave that moved up is pulled back. A
  // slave above its requested state on the way down is doing what it was asked.
  std::unordered_map<uint16_t, int> previousRanks;
  while (!stopToken.stop_requested()) {
    std::this_thread::sleep_for(kPumpInterval);
    if (!active_.load(std::memory_order_acquire)) {
      continue;
    }
    const std::unique_lock lock(masterMutex_, std::try_to_lock);
    if (!lock.owns_lock() || master_ == nullptr || !active_.load()) {
      continue;
    }
    const auto idleFor = std::chrono::nanoseconds(steadyNowNs() - lastExchangeNs_.load());
    if (idleFor >= kPumpIdleThreshold) {
      ecrt_master_receive(master_);
      ecrt_master_send(master_);
    }
    // The master requests OP for every configured slave again when its configuration changes, as
    // after a topology change. A slave that climbs above the state it was asked for is pulled back.
    if (std::chrono::steady_clock::now() < nextStateCheck) {
      continue;
    }
    nextStateCheck = std::chrono::steady_clock::now() + kStateKeeperInterval;
    for (const uint16_t position : configuredPositions_) {
      if (auto slave = readSlave(position); !slave) {
        continue;
      }
      const auto current = alState(slaveStates_[position].load(std::memory_order_relaxed));
      const auto requested = static_cast<EtherCatState>(requestedStates_[position].load());
      const int rank = stateRank(current);
      const auto previous = previousRanks.find(position);
      const bool climbed = previous != previousRanks.end() && rank > previous->second;
      previousRanks[position] = rank;
      if (climbed && stateRank(requested) > 0 && rank > stateRank(requested)) {
        spdlog::warn("IgH: slave {} went to {} above the requested {}; requesting {} again",
                     position, toString(current), toString(requested), toString(requested));
        static_cast<void>(requestState(position, requested));
      }
    }
  }
}

void IghFieldbusDriver::releaseMaster() {
  const std::lock_guard<std::mutex> lock(masterMutex_);
  releaseMasterLocked();
}

void IghFieldbusDriver::releaseMasterLocked() {
  const bool wasActive = active_.exchange(false);
  if (master_ != nullptr) {
    if (wasActive) {
      // Collect the answer to the last frame sent, so no frame is in flight when the master
      // changes threads.
      ecrt_master_receive(master_);
      std::this_thread::sleep_for(kPumpInterval);
      ecrt_master_receive(master_);
    }
    // Releasing deactivates the master, and every slave drops to PRE-OP.
    ecrt_release_master(master_);
    master_ = nullptr;
  }
  outputDomain_ = nullptr;
  inputDomain_ = nullptr;
  outputData_ = nullptr;
  inputData_ = nullptr;
  configuredPositions_.clear();
  layout_ = {};
}

std::expected<void, std::string> IghFieldbusDriver::waitForSettledBus() const {
  const auto started = std::chrono::steady_clock::now();
  bool seenBusy = false;
  while (true) {
    ec_ioctl_master_t master{};
    if (const int error = callIoctl(fd_, EC_IOCTL_MASTER, &master); error != 0) {
      return std::unexpected(std::format("cannot read the master: {}", errnoText(error)));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (master.scan_busy != 0) {
      if (!seenBusy) {
        spdlog::info("IgH: the master rescans the bus after its release; waiting for it");
      }
      seenBusy = true;
    } else if (elapsed >= kSettleWindow) {
      return {};
    }
    if (elapsed >= kScanTimeout) {
      return std::unexpected(
          std::format("the IgH master did not finish the scan within {} s", kScanTimeout.count()));
    }
    std::this_thread::sleep_for(kScanPollInterval);
  }
}

void IghFieldbusDriver::stop() { shutdown(); }

void IghFieldbusDriver::shutdown() {
  if (pump_.joinable()) {
    pump_.request_stop();
    pump_.join();
  }
  releaseMaster();
  if (fd_ != -1) {
    ::close(fd_);
    fd_ = -1;
  }
}

std::expected<std::vector<FieldbusDriver::SlaveStateRaw>, std::string>
IghFieldbusDriver::readStates(const std::vector<uint16_t>& positions) {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  // The registers, not the state the master keeps, because the master refreshes its copy only when
  // its state machine next visits the slave, and it has no copy of the AL Status Code.
  std::vector<SlaveStateRaw> states;
  states.reserve(positions.size());
  std::array<uint8_t, kAlStatusBytes> bytes{};
  for (const uint16_t position : positions) {
    if (auto read = readRegister(position, kAlStatusRegister, bytes); !read) {
      return std::unexpected(read.error());
    }
    const SlaveStateRaw state{
        .alStatus = readLe16(std::span(bytes).subspan(0, 2)),
        .alStatusCode = readLe16(std::span(bytes).subspan(4, 2)),
    };
    slaveStates_[position].store(state.alStatus, std::memory_order_relaxed);
    states.push_back(state);
  }
  return states;
}

std::expected<std::vector<uint8_t>, std::string> IghFieldbusDriver::readSdo(uint16_t slavePosition,
                                                                            uint16_t index,
                                                                            uint8_t subindex) {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  const auto sdoLevel = sdoLogQuiet ? spdlog::level::trace : spdlog::level::debug;
  spdlog::log(sdoLevel, "SDOread slave {} 0x{:04X}:{:02X}", slavePosition, index, subindex);
  std::vector<uint8_t> data(kSdoBufferBytes, 0);
  ec_ioctl_slave_sdo_upload_t upload{};
  upload.slave_position = static_cast<uint16_t>(slavePosition - 1);
  upload.sdo_index = index;
  upload.sdo_entry_subindex = subindex;
  upload.target_size = data.size();
  upload.target = data.data();
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_SDO_UPLOAD, &upload); error != 0) {
    std::string message = std::format("SDOread slave {} 0x{:04X}:{:02X} failed{}", slavePosition,
                                      index, subindex, sdoFailureSuffix(error, upload.abort_code));
    spdlog::log(sdoLevel, "{}", message);
    return std::unexpected(std::move(message));
  }
  data.resize(upload.data_size);
  return data;
}

std::expected<void, std::string> IghFieldbusDriver::writeSdo(uint16_t slavePosition, uint16_t index,
                                                             uint8_t subindex,
                                                             std::span<const uint8_t> data) {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  spdlog::debug("SDOwrite slave {} 0x{:04X}:{:02X} ({} bytes)", slavePosition, index, subindex,
                data.size());
  // The request carries a pointer to mutable bytes, although the kernel only reads them.
  std::vector<uint8_t> bytes(data.begin(), data.end());
  ec_ioctl_slave_sdo_download_t download{};
  download.slave_position = static_cast<uint16_t>(slavePosition - 1);
  download.sdo_index = index;
  download.sdo_entry_subindex = subindex;
  download.complete_access = 0;
  download.data_size = bytes.size();
  download.data = bytes.data();
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_SDO_DOWNLOAD, &download); error != 0) {
    std::string message =
        std::format("SDOwrite slave {} 0x{:04X}:{:02X} failed{}", slavePosition, index, subindex,
                    sdoFailureSuffix(error, download.abort_code));
    spdlog::debug("{}", message);
    return std::unexpected(std::move(message));
  }
  spdlog::debug("SDOwrite slave {} 0x{:04X}:{:02X} ok", slavePosition, index, subindex);
  return {};
}

std::expected<uint16_t, std::string> IghFieldbusDriver::waitForDictionary(uint16_t position) const {
  // The count of entries the master holds, which grows while it reads the dictionary.
  const auto countEntries = [this, position](uint16_t sdoCount) {
    std::size_t entries = 0;
    for (uint16_t sdoPosition = 0; sdoPosition < sdoCount; ++sdoPosition) {
      ec_ioctl_slave_sdo_t sdo{};
      sdo.slave_position = static_cast<uint16_t>(position - 1);
      sdo.sdo_position = sdoPosition;
      if (callIoctl(fd_, EC_IOCTL_SLAVE_SDO, &sdo) != 0) {
        continue;
      }
      for (unsigned sub = 0; sub <= sdo.max_subindex; ++sub) {
        ec_ioctl_slave_sdo_entry_t entry{};
        entry.slave_position = static_cast<uint16_t>(position - 1);
        entry.sdo_spec = sdo.sdo_index;
        entry.sdo_entry_subindex = static_cast<uint8_t>(sub);
        if (callIoctl(fd_, EC_IOCTL_SLAVE_SDO_ENTRY, &entry) == 0) {
          ++entries;
        }
      }
    }
    return entries;
  };

  const auto deadline = std::chrono::steady_clock::now() + config_.dictionaryTimeout;
  std::size_t lastEntries = 0;
  int stableReads = 0;
  while (true) {
    auto slave = readSlave(position);
    if (!slave) {
      return std::unexpected(slave.error());
    }
    if (slave->sdoCount > 0) {
      const std::size_t entries = countEntries(slave->sdoCount);
      stableReads = entries == lastEntries ? stableReads + 1 : 0;
      lastEntries = entries;
      if (stableReads >= kDictionaryStableReads) {
        return slave->sdoCount;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::unexpected(std::format(
          "the IgH master did not finish reading the object dictionary of slave {} within {} s. "
          "It starts that read 3 s after the slave reaches PRE-OP.",
          position,
          std::chrono::duration_cast<std::chrono::seconds>(config_.dictionaryTimeout).count()));
    }
    std::this_thread::sleep_for(kDictionaryPollInterval);
  }
}

std::expected<OdRead, std::string> IghFieldbusDriver::readObjectDictionary(uint16_t slavePosition) {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  auto slave = readSlave(slavePosition);
  if (!slave) {
    return std::unexpected(slave.error());
  }
  if (!slave->sdoInfo) {
    return std::unexpected(std::format("slave {} does not support SDO Information", slavePosition));
  }
  // The master reads every dictionary itself, and the ioctl interface serves its copy. There is no
  // request that sends SDO Information to the slave directly.
  auto sdoCount = waitForDictionary(slavePosition);
  if (!sdoCount) {
    return std::unexpected(sdoCount.error());
  }
  const auto started = std::chrono::steady_clock::now();
  const auto igh = static_cast<uint16_t>(slavePosition - 1);
  // The master keeps the object code and the access bits above write-in-OP to itself, so no entry
  // read here is a full description.
  OdRead read;
  read.fullDescriptions = false;
  int probes = 0;
  int undescribed = 0;
  int filled = 0;
  bool probeDeclaredSize = true;

  for (uint16_t sdoPosition = 0; sdoPosition < *sdoCount; ++sdoPosition) {
    ec_ioctl_slave_sdo_t sdo{};
    sdo.slave_position = igh;
    sdo.sdo_position = sdoPosition;
    if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_SDO, &sdo); error != 0) {
      return std::unexpected(std::format("cannot read object {} of slave {}: {}", sdoPosition,
                                         slavePosition, errnoText(error)));
    }
    const std::optional<StandardObject> standard = standardObject(sdo.sdo_index);
    // ETG.1000.6 gives subindex 0 and the entries of a standard object the same access rule, so
    // the access read for subindex 0 is the access of an entry the master did not describe.
    std::optional<uint16_t> countAccess;
    for (unsigned sub = 0; sub <= sdo.max_subindex; ++sub) {
      ec_ioctl_slave_sdo_entry_t entry{};
      entry.slave_position = igh;
      entry.sdo_spec = sdo.sdo_index;
      entry.sdo_entry_subindex = static_cast<uint8_t>(sub);
      if (callIoctl(fd_, EC_IOCTL_SLAVE_SDO_ENTRY, &entry) != 0) {
        // The master gave up on this entry while it read the dictionary, and it does not try again
        // until the next scan.
        ++read.missingEntries;
        continue;
      }
      // The master keeps only the read and write bits of the access word, which are its low six
      // bits in the same order (master/fsm_coe.c).
      uint16_t access = 0;
      for (unsigned state = 0; state < EC_SDO_ENTRY_ACCESS_COUNT; ++state) {
        if (entry.read_access[state] != 0) {
          access |= static_cast<uint16_t>(1u << state);
        }
        if (entry.write_access[state] != 0) {
          access |= static_cast<uint16_t>(1u << (EC_SDO_ENTRY_ACCESS_COUNT + state));
        }
      }
      if (sub == 0) {
        countAccess = access;
      }
      read.entries.push_back(OdEntry{
          .index = sdo.sdo_index,
          .subindex = static_cast<uint8_t>(sub),
          // The master reads the object code but the ioctl interface does not carry it. Outside the
          // standard objects, 0 names no object code, so the node layer treats the object as
          // neither an array nor a record.
          .objectCode = standard ? standard->objectCode : uint16_t{0},
          .dataType = entry.data_type,
          .bitLength = entry.bit_length,
          .access = access,
          .name = fixedString(entry.description),
          .unit = std::nullopt,
          .defaultValue = std::nullopt,
          .minValue = std::nullopt,
          .maxValue = std::nullopt,
      });
    }

    // SOMANET firmware reports the runtime value of subindex 0 as the size of an object, so an
    // unmapped 0x1603 reports no entries while its storage holds several. The master reads only up
    // to that size, and nothing here can ask the slave for the description of the rest. An upload
    // past the reported size shows whether more entries exist. One attempt each, because a refusal
    // is the answer that ends the probe.
    //
    // Only the standard objects are probed. SOMANET firmware answers an upload of any subindex of a
    // VAR with the value of the VAR: 0x6080:C8 returns the 4 bytes of 0x6080:00. The ioctl
    // interface does not carry the object code, so a probe of any other object cannot tell a VAR
    // from a record.
    //
    // An entry found is filled in from the standard. The upload must return exactly the width the
    // standard gives, or the slave does not follow it, and the entry is counted as missing so the
    // node layer does not cache a dictionary that is short of it.
    for (unsigned sub = sdo.max_subindex + 1u; standard && probeDeclaredSize && sub <= 0xFF;
         ++sub) {
      ++probes;
      std::array<uint8_t, kSdoBufferBytes> target{};
      ec_ioctl_slave_sdo_upload_t upload{};
      upload.slave_position = igh;
      upload.sdo_index = sdo.sdo_index;
      upload.sdo_entry_subindex = static_cast<uint8_t>(sub);
      upload.target_size = target.size();
      upload.target = target.data();
      const auto probeStarted = std::chrono::steady_clock::now();
      if (callIoctl(fd_, EC_IOCTL_SLAVE_SDO_UPLOAD, &upload) != 0) {
        const auto elapsed = std::chrono::steady_clock::now() - probeStarted;
        if (elapsed > kProbeRefusalBudget) {
          spdlog::debug(
              "Device {}: 0x{:04X}:{:02X} took {} ms to refuse, so the remaining objects are not "
              "probed; their unused subindices are not counted",
              slavePosition, sdo.sdo_index, sub,
              std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
          probeDeclaredSize = false;
          // The probe that stopped cannot say whether more entries exist.
          ++read.missingEntries;
        }
        break;
      }
      if (countAccess && upload.data_size * 8 == standard->entryBitLength) {
        read.entries.push_back(OdEntry{
            .index = sdo.sdo_index,
            .subindex = static_cast<uint8_t>(sub),
            .objectCode = standard->objectCode,
            .dataType = standard->entryDataType,
            .bitLength = standard->entryBitLength,
            .access = *countAccess,
            // The firmware names a mapped entry after the object it maps, so an unused entry has no
            // name to read. This is the pattern the firmware uses for subindex 0.
            .name = std::format("SubIndex {:03}", sub),
            .unit = std::nullopt,
            .defaultValue = std::nullopt,
            .minValue = std::nullopt,
            .maxValue = std::nullopt,
        });
        ++filled;
        continue;
      }
      ++undescribed;
      ++read.missingEntries;
    }
  }

  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
  spdlog::log(read.missingEntries > 0 ? spdlog::level::warn : spdlog::level::debug,
              "Device {}: object dictionary has {} entries from {} object(s) in {} ms; {} filled "
              "from the standard past the size the slave reports; {} missing, of which {} exist "
              "past that size ({} probes)",
              slavePosition, read.entries.size(), *sdoCount, elapsedMs, filled, read.missingEntries,
              undescribed, probes);
  return read;
}

std::expected<std::vector<uint8_t>, FoeError> IghFieldbusDriver::readFile(
    uint16_t /*slavePosition*/, const std::string& /*filename*/) {
  return std::unexpected(FoeError{.kind = FoeErrorKind::Protocol,
                                  .retry = Retry::Permanent,
                                  .message = "FoE over the IgH master is not supported yet"});
}

std::expected<void, FoeError> IghFieldbusDriver::writeFile(uint16_t /*slavePosition*/,
                                                           const std::string& /*filename*/,
                                                           std::span<const uint8_t> /*data*/) {
  return std::unexpected(FoeError{.kind = FoeErrorKind::Protocol,
                                  .retry = Retry::Permanent,
                                  .message = "FoE over the IgH master is not supported yet"});
}

std::expected<void, std::string> IghFieldbusDriver::readRegister(uint16_t slavePosition,
                                                                 uint16_t address,
                                                                 std::span<uint8_t> data) {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  ec_ioctl_slave_reg_t reg{};
  reg.slave_position = static_cast<uint16_t>(slavePosition - 1);
  reg.emergency = 0;
  reg.address = address;
  reg.size = data.size();
  reg.data = data.data();
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_REG_READ, &reg); error != 0) {
    return std::unexpected(std::format("register read slave {} 0x{:04X} ({} bytes) failed: {}",
                                       slavePosition, address, data.size(), errnoText(error)));
  }
  return {};
}

std::expected<void, std::string> IghFieldbusDriver::writeRegister(uint16_t slavePosition,
                                                                  uint16_t address,
                                                                  std::span<const uint8_t> data) {
  if (auto open = requireOpen(); !open) {
    return std::unexpected(open.error());
  }
  // The request carries a pointer to mutable bytes, although the kernel only reads them.
  std::vector<uint8_t> bytes(data.begin(), data.end());
  ec_ioctl_slave_reg_t reg{};
  reg.slave_position = static_cast<uint16_t>(slavePosition - 1);
  reg.emergency = 0;
  reg.address = address;
  reg.size = bytes.size();
  reg.data = bytes.data();
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_REG_WRITE, &reg); error != 0) {
    return std::unexpected(std::format("register write slave {} 0x{:04X} ({} bytes) failed: {}",
                                       slavePosition, address, data.size(), errnoText(error)));
  }
  return {};
}

std::expected<void, std::string> IghFieldbusDriver::requestState(uint16_t position,
                                                                 EtherCatState state) const {
  ec_ioctl_slave_state_t request{};
  request.slave_position = static_cast<uint16_t>(position - 1);
  request.al_state = static_cast<uint8_t>(state);
  if (const int error = callIoctl(fd_, EC_IOCTL_SLAVE_STATE, &request); error != 0) {
    return std::unexpected(std::format("cannot request {} for slave {}: {}", toString(state),
                                       position, errnoText(error)));
  }
  return {};
}

void IghFieldbusDriver::transitionToState(const std::vector<uint16_t>& positions,
                                          std::optional<EtherCatState> requiredState,
                                          EtherCatState targetState,
                                          std::chrono::steady_clock::duration timeout,
                                          std::chrono::steady_clock::duration resendInterval,
                                          std::function<void()> tick,
                                          std::function<bool()> shouldAbort) {
  if (auto open = requireOpen(); !open) {
    spdlog::error("{}", open.error());
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto initial = readStates(positions);
  if (!initial) {
    spdlog::error("state change to {}: {}", toString(targetState), initial.error());
    return;
  }
  std::vector<uint16_t> pending;
  for (std::size_t i = 0; i < positions.size(); ++i) {
    if (!requiredState || alState((*initial)[i].alStatus) == *requiredState) {
      pending.push_back(positions[i]);
      requestedStates_[positions[i]].store(static_cast<uint8_t>(targetState));
    }
  }

  // A new request also clears the error flag the master sets when a slave refuses a state. Without
  // it the master would not try that slave again.
  const auto request = [this, targetState](const std::vector<uint16_t>& targets) {
    for (const uint16_t position : targets) {
      if (auto requested = requestState(position, targetState); !requested) {
        spdlog::error("{}", requested.error());
      }
    }
  };
  request(pending);
  auto lastRequest = std::chrono::steady_clock::now();

  while (!pending.empty()) {
    if (shouldAbort && shouldAbort()) {
      return;
    }
    for (auto waited = std::chrono::steady_clock::duration::zero(); waited < kStatePollInterval;
         waited += std::chrono::milliseconds(1)) {
      if (tick) {
        tick();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto states = readStates(pending);
    if (!states) {
      spdlog::error("state change to {}: {}", toString(targetState), states.error());
      return;
    }
    std::vector<uint16_t> lagging;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      const uint16_t status = (*states)[i].alStatus;
      if (alState(status) != targetState || alHasError(status)) {
        lagging.push_back(pending[i]);
      }
    }
    pending = std::move(lagging);
    if (pending.empty()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      // The master keeps the request. It goes on driving each slave to the target until the slave
      // refuses it, so a slave can still reach the target after this returns.
      for (const uint16_t position : pending) {
        spdlog::warn("Slave {} did not reach {} in time (AL status 0x{:04X})", position,
                     toString(targetState), slaveState(position));
      }
      return;
    }
    if (now - lastRequest >= resendInterval) {
      request(pending);
      lastRequest = now;
    }
  }
}

}  // namespace mm::comm::igh
