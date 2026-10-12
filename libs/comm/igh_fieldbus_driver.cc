#include "comm/igh_fieldbus_driver.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
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

IghFieldbusDriver::IghFieldbusDriver(IghFieldbusDriverConfig config)
    : config_(config), slaveStates_(std::make_unique<std::atomic<uint16_t>[]>(kStateSlots)) {}

IghFieldbusDriver::~IghFieldbusDriver() { closeDevice(); }

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
  fd_ = fd;
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
  // A position the new scan did not find has no state.
  for (std::size_t position = master.slave_count + 1; position <= previousCount; ++position) {
    slaveStates_[position].store(0, std::memory_order_relaxed);
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

std::expected<void, std::string> IghFieldbusDriver::configureProcessData() {
  return std::unexpected("process data over the IgH master is not supported yet");
}

PdoLayout IghFieldbusDriver::processDataLayout() { return {}; }

int IghFieldbusDriver::exchangeProcessData(std::span<const uint8_t> /*outputs*/,
                                           std::span<uint8_t> /*inputs*/) {
  return 0;
}

void IghFieldbusDriver::stop() { closeDevice(); }

void IghFieldbusDriver::closeDevice() {
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
