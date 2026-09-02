#include "node/eni_collector.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "comm/fieldbus_driver.h"
#include "comm/object_data_types.h"
#include "comm/sii.h"
#include "core/util.h"
#include "node/device.h"
#include "node/device_manager.h"
#include "node/pdo_mapping.h"

namespace mm::node {

namespace {

using mm::etg::EniCmd;
using mm::etg::EniTransition;

/// ESC register addresses the init commands write. ETG.1000.4 §6 is the authority for each.
constexpr std::uint16_t kRegConfiguredAddress = 0x0010;
constexpr std::uint16_t kRegAlControl = 0x0120;
constexpr std::uint16_t kRegAlStatus = 0x0130;
constexpr std::uint16_t kRegDcSystemTime = 0x0910;
constexpr std::uint16_t kRegDcCycleConfig = 0x0970;

/// The broadcast clears at INIT cover every channel the register block can hold, which is what the
/// master must do before it programs the ones this bus uses.
constexpr std::uint32_t kFmmuBlockBytes = 256;
constexpr std::uint32_t kSyncManagerBlockBytes = 256;
constexpr std::uint32_t kDcBlockBytes = 32;

/// AL states as the AL Control register carries them (ETG.1000.6 §6.4.1).
constexpr std::uint8_t kAlStateInit = 0x01;
constexpr std::uint8_t kAlStatePreOp = 0x02;
constexpr std::uint8_t kAlStateSafeOp = 0x04;
constexpr std::uint8_t kAlStateOp = 0x08;

/// The AL Status state field is the low nibble; the high bits carry the error indicator, which a
/// state check must not compare against.
constexpr std::uint8_t kAlStateMask = 0x0F;

/// FMMU byte 0x0B, which selects the service the entity serves (ETG.1000.4 Table 56).
constexpr std::uint8_t kFmmuRead = 0x01;
constexpr std::uint8_t kFmmuWrite = 0x02;

constexpr std::uint32_t kStateTimeoutMs = 10000;
constexpr std::uint32_t kCoeTimeoutMs = 1000;
constexpr std::uint32_t kRetries = 3;
constexpr std::size_t kMacBytes = 6;

/// PDO assignment objects: 0x1C12 lists the RxPDOs of a Sync Manager, 0x1C13 the TxPDOs.
constexpr std::uint16_t kRxPdoAssign = 0x1C12;
constexpr std::uint16_t kTxPdoAssign = 0x1C13;

/// @brief Names a mapped object's type the way an ESI spells it, for @c ProcessImage/DataType.
///
/// A consuming master matches this against the type names in the device's ESI, so the IEC
/// spellings are what it expects rather than the ETG.1020 ones. A type with no IEC name of a fixed
/// width — a string, a record, anything the process image should not be carrying — yields an empty
/// name, and the element is then omitted rather than guessed at.
constexpr std::string_view iecTypeName(std::uint16_t dataType) {
  switch (static_cast<mm::comm::ObjectDataType>(dataType)) {
    case mm::comm::ObjectDataType::BOOLEAN:
      return "BOOL";
    case mm::comm::ObjectDataType::INTEGER8:
      return "SINT";
    case mm::comm::ObjectDataType::INTEGER16:
      return "INT";
    case mm::comm::ObjectDataType::INTEGER32:
      return "DINT";
    case mm::comm::ObjectDataType::INTEGER64:
      return "LINT";
    case mm::comm::ObjectDataType::UNSIGNED8:
      return "USINT";
    case mm::comm::ObjectDataType::UNSIGNED16:
      return "UINT";
    case mm::comm::ObjectDataType::UNSIGNED32:
      return "UDINT";
    case mm::comm::ObjectDataType::UNSIGNED64:
      return "ULINT";
    case mm::comm::ObjectDataType::REAL32:
      return "REAL";
    case mm::comm::ObjectDataType::REAL64:
      return "LREAL";
    default:
      return "";
  }
}

/// The register codecs return a fixed-width array, which is what says how wide the block is; an
/// ENI payload is a vector, because most of them are not register blocks at all.
template <std::size_t N>
std::vector<std::uint8_t> toVector(const std::array<std::uint8_t, N>& bytes) {
  return {bytes.begin(), bytes.end()};
}

mm::etg::EniEcatCmd registerWrite(EniTransition transition, EniCmd cmd, std::uint16_t address,
                                  std::uint16_t reg, std::vector<std::uint8_t> data,
                                  std::string comment) {
  mm::etg::EniEcatCmd command;
  command.transitions = {transition};
  command.comment = std::move(comment);
  command.requirement = mm::etg::EniRequires::Cycle;
  command.cmd = cmd;
  command.adp = address;
  command.ado = reg;
  command.data = std::move(data);
  command.cnt = 1;
  command.retries = kRetries;
  return command;
}

/// @brief Builds the read that holds the master at a transition until the device has arrived.
///
/// An ENI has no wait primitive. A read with a @c Validate is the wait: the master re-sends it
/// until the AL Status state field matches, or until the timeout runs out.
mm::etg::EniEcatCmd stateCheck(EniTransition transition, EniCmd cmd, std::uint16_t address,
                               std::uint8_t state, std::string comment) {
  mm::etg::EniEcatCmd command;
  command.transitions = {transition};
  command.comment = std::move(comment);
  command.requirement = mm::etg::EniRequires::Cycle;
  command.cmd = cmd;
  command.adp = address;
  command.ado = kRegAlStatus;
  command.dataLength = 2;
  command.cnt = 1;
  command.retries = kRetries;
  command.validate = mm::etg::EniValidate{
      .data = {state, 0x00}, .dataMask = {kAlStateMask, 0x00}, .timeoutMs = kStateTimeoutMs};
  return command;
}

mm::etg::EniEcatCmd broadcastClear(std::uint16_t reg, std::uint32_t bytes, std::string comment) {
  mm::etg::EniEcatCmd command;
  command.transitions = {EniTransition::IP};
  command.beforeSlave = true;
  command.comment = std::move(comment);
  command.requirement = mm::etg::EniRequires::Cycle;
  command.cmd = EniCmd::Bwr;
  command.adp = 0;
  command.ado = reg;
  command.dataLength = bytes;
  command.retries = kRetries;
  return command;
}

/// @brief The bus-wide commands the master runs before it touches any one device.
///
/// A device's own init commands assume the register blocks start empty, so the clears belong here
/// rather than in each device's list — which is what @c beforeSlave says.
std::vector<mm::etg::EniEcatCmd> masterInitCmds() {
  return {
      broadcastClear(mm::comm::kFmmuRegisterBase, kFmmuBlockBytes, "clear every FMMU"),
      broadcastClear(mm::comm::kSyncManagerRegisterBase, kSyncManagerBlockBytes,
                     "clear every sync manager"),
      broadcastClear(kRegDcSystemTime, kDcBlockBytes, "clear the distributed-clock system time"),
      broadcastClear(kRegDcCycleConfig, kDcBlockBytes,
                     "clear the distributed-clock cycle configuration"),
  };
}

std::vector<mm::etg::EniMailboxProtocol> decodeProtocols(std::uint16_t bits) {
  // Bit values from MailboxConfig::protocols, which mirrors the SII mailbox-protocol word.
  constexpr std::pair<std::uint16_t, mm::etg::EniMailboxProtocol> kBits[] = {
      {0x01, mm::etg::EniMailboxProtocol::Aoe}, {0x02, mm::etg::EniMailboxProtocol::Eoe},
      {0x04, mm::etg::EniMailboxProtocol::Coe}, {0x08, mm::etg::EniMailboxProtocol::Foe},
      {0x10, mm::etg::EniMailboxProtocol::Soe}, {0x20, mm::etg::EniMailboxProtocol::Voe},
  };
  std::vector<mm::etg::EniMailboxProtocol> protocols;
  for (const auto& [bit, protocol] : kBits) {
    if ((bits & bit) != 0) {
      protocols.push_back(protocol);
    }
  }
  return protocols;
}

mm::etg::EniSyncManagerType syncManagerType(std::uint8_t type) {
  // SyncManagerConfig::type: 1=MbxOut, 2=MbxIn, 3=Outputs, 4=Inputs.
  switch (type) {
    case 1:
      return mm::etg::EniSyncManagerType::MailboxOut;
    case 2:
      return mm::etg::EniSyncManagerType::MailboxIn;
    case 3:
      return mm::etg::EniSyncManagerType::Outputs;
    default:
      return mm::etg::EniSyncManagerType::Inputs;
  }
}

bool carriesProcessData(const mm::comm::SyncManagerConfig& syncManager) {
  return syncManager.type == 3 || syncManager.type == 4;
}

/// @brief Writes one CoE object's subindex, as the PDO assignment sequence needs it.
mm::etg::EniCoeCmd coeDownload(std::uint16_t index, std::uint8_t subindex,
                               std::vector<std::uint8_t> data, std::string comment) {
  mm::etg::EniCoeCmd command;
  command.transitions = {EniTransition::PS};
  command.comment = std::move(comment);
  command.timeoutMs = kCoeTimeoutMs;
  command.ccs = mm::etg::EniCoeCommandSpecifier::Download;
  command.index = index;
  command.subindex = subindex;
  command.data = std::move(data);
  return command;
}

/// @brief Builds the CoE downloads that reproduce one direction's PDO mapping.
///
/// The order is the one the standard requires and the one a device enforces: clear the count, fill
/// the entries, then write the count back. A mapping object is rewritten the same way before it is
/// assigned, so a device whose mapping this master changed is configured by the document rather
/// than left on its own defaults.
void appendPdoAssignment(std::vector<mm::etg::EniCoeCmd>& commands, std::uint16_t assignIndex,
                         const std::vector<PdoMappingObject>& objects) {
  const std::string direction = assignIndex == kRxPdoAssign ? "RxPDO" : "TxPDO";
  for (const PdoMappingObject& object : objects) {
    commands.push_back(coeDownload(object.pdoIndex, 0, {0x00},
                                   std::format("clear the entries of {:#06x}", object.pdoIndex)));
    std::uint8_t subindex = 0;
    for (const PdoMappingEntry& entry : object.entries) {
      const auto packed = mm::core::toBytes<std::uint32_t>(packMappingEntry(entry));
      commands.push_back(coeDownload(object.pdoIndex, ++subindex, {packed.begin(), packed.end()},
                                     std::format("map {:#06x}:{:02} into {:#06x}", entry.index,
                                                 entry.subindex, object.pdoIndex)));
    }
    commands.push_back(coeDownload(object.pdoIndex, 0, {subindex},
                                   std::format("set the entry count of {:#06x}", object.pdoIndex)));
  }

  commands.push_back(
      coeDownload(assignIndex, 0, {0x00}, std::format("clear the {} assignment", direction)));
  std::uint8_t subindex = 0;
  for (const PdoMappingObject& object : objects) {
    const auto pdo = mm::core::toBytes<std::uint16_t>(object.pdoIndex);
    commands.push_back(coeDownload(assignIndex, ++subindex, {pdo.begin(), pdo.end()},
                                   std::format("assign {:#06x}", object.pdoIndex)));
  }
  commands.push_back(coeDownload(assignIndex, 0, {subindex},
                                 std::format("set the {} assignment count", direction)));
}

/// @brief Declares one mapping object as an ENI PDO.
///
/// This is the declarative half of the process data. The CoE commands say how to *configure* the
/// mapping; this says what the mapping *is*, which is the only place in an ENI an object address
/// for a mapped value can live. Without it a reader of the document can see that a value sits at
/// bit 16 and is called "Target position", and cannot learn that it is 0x607A:00.
///
/// Names and types come from the device's enumerated dictionary. Where that has not been
/// enumerated, an entry falls back to its own address as a name, because ETG.2100 makes a name
/// mandatory for every entry that addresses something.
mm::etg::EniPdo declarePdo(const PdoMappingObject& object, const DeviceHandle& device,
                           std::optional<std::uint8_t> syncManager, bool isOutput) {
  mm::etg::EniPdo pdo;
  pdo.index = object.pdoIndex;
  pdo.name = std::format("{}PDO {:#06x}", isOutput ? "Rx" : "Tx", object.pdoIndex);
  pdo.syncManager = syncManager;
  for (const PdoMappingEntry& entry : object.entries) {
    mm::etg::EniPdoEntry declared;
    declared.index = entry.index;
    declared.subindex = entry.subindex;
    declared.bitLen = entry.bitLength;
    if (entry.index == 0) {
      // Padding: it occupies the bits and addresses nothing, so it has no name and no type.
      pdo.entries.push_back(std::move(declared));
      continue;
    }
    declared.name = std::format("{:#06x}:{:02}", entry.index, entry.subindex);
    if (device) {
      if (const auto parameter = device->parameter(entry.index, entry.subindex); parameter) {
        if (!parameter->name.empty()) {
          declared.name = parameter->name;
        }
        declared.dataType = iecTypeName(parameter->dataType);
      }
    }
    pdo.entries.push_back(std::move(declared));
  }
  return pdo;
}

/// The Sync Manager a direction's process data travels on, from what the master programmed.
std::optional<std::uint8_t> processDataSyncManager(const mm::comm::SlaveConfig& config,
                                                   bool isOutput) {
  const std::uint8_t wanted = isOutput ? 3 : 4;  // SyncManagerConfig::type: 3 Outputs, 4 Inputs.
  const auto found =
      std::ranges::find(config.syncManagers, wanted, &mm::comm::SyncManagerConfig::type);
  if (found == config.syncManagers.end()) {
    return std::nullopt;
  }
  return found->index;
}

/// @brief The auto-increment address of a device, which counts down from zero along the ring.
///
/// The first device is addressed with 0, the second with 0xFFFF, the third with 0xFFFE. Each
/// device increments the field as the datagram passes, so the one that reads zero is the one that
/// acts on it.
std::uint16_t autoIncrementAddress(std::uint16_t slavePosition) {
  return static_cast<std::uint16_t>(-static_cast<int>(slavePosition - 1));
}

}  // namespace

std::expected<EniCollection, std::string> collectEni(DeviceManager& manager,
                                                     const EniCollectorOptions& options) {
  if (options.sourceMac.size() != kMacBytes) {
    return std::unexpected(
        std::format("the source MAC is {} bytes, needs {}", options.sourceMac.size(), kMacBytes));
  }

  const ProcessImageInfo image = manager.processImageInfo();
  if (!image.configured) {
    return std::unexpected(
        "the bus has no published process image, so there is no mapping to describe — bring it to "
        "SAFE-OP or OP first");
  }

  const std::vector<SlaveConfigInfo> busConfig = manager.busConfig();
  if (busConfig.empty()) {
    return std::unexpected("the bus has no devices");
  }

  EniCollection collection;
  collection.network.master.name = options.masterName;
  collection.network.master.destination.assign(kMacBytes, 0xFF);
  collection.network.master.source = options.sourceMac;
  collection.network.master.initCmds = masterInitCmds();

  // Logical addressing for the cyclic frame. The output FMMUs and the input FMMUs each occupy one
  // contiguous logical range, so the lowest address of each is where its datagram starts, and the
  // number of FMMUs in it is the working counter that datagram returns.
  std::uint32_t outputBase = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t inputBase = std::numeric_limits<std::uint32_t>::max();
  int outputFmmus = 0;
  int inputFmmus = 0;

  for (const SlaveConfigInfo& info : busConfig) {
    const mm::comm::SlaveConfig& config = info.config;
    const std::uint16_t station = config.configuredAddress;
    const std::uint16_t autoInc = autoIncrementAddress(config.slavePosition);

    mm::etg::EniSlave slave;
    slave.info.name = info.deviceName.empty() ? info.productName : info.deviceName;
    slave.info.physAddr = station;
    slave.info.autoIncAddr = autoInc;
    slave.info.vendorId = info.vendorId;
    slave.info.productCode = info.productCode;
    slave.info.revisionNo = info.revisionNumber;
    slave.info.serialNo = info.serialNumber;

    // The port layout and the bootstrap mailbox live only in the SII, so this costs one EEPROM read
    // per device. Neither is load-bearing: without them the document still brings the bus up, so a
    // failure is a warning and the elements are left out.
    std::optional<mm::comm::SlaveInformationInterface> sii;
    const DeviceHandle device = manager.deviceAt(config.slavePosition);
    if (device) {
      if (const auto raw = device->readSii(); raw) {
        if (auto parsed = mm::comm::parseSii(*raw); parsed) {
          sii = std::move(*parsed);
        } else {
          collection.warnings.push_back(std::format("device {}: the SII will not parse ({})",
                                                    config.slavePosition, parsed.error()));
        }
      } else {
        collection.warnings.push_back(std::format("device {}: the SII will not read ({})",
                                                  config.slavePosition, raw.error()));
      }
    }
    if (sii) {
      slave.info.physics = mm::etg::eniPhysics(sii->category.general.physicalPort);
    }

    // Read once and used twice: to declare what the mapping is under ProcessData, and to write the
    // commands that configure it under the mailbox. A second read would cost another burst of SDO
    // uploads for an answer that cannot have changed.
    std::optional<PdoMapping> mapping;
    if ((config.mailbox.protocols & mm::comm::MailboxConfig::kProtocolCoe) != 0 && device) {
      if (auto read = device->readPdoMapping(); read) {
        mapping = std::move(*read);
      } else {
        collection.warnings.push_back(std::format(
            "device {}: the PDO mapping will not read, so the document neither declares "
            "it nor configures it ({})",
            config.slavePosition, read.error()));
      }
    }

    // Process data: the window each direction occupies in the master's image, every Sync Manager
    // this master programmed, and what each direction's PDOs actually carry.
    mm::etg::EniProcessData processData;
    if (mapping) {
      for (const PdoMappingObject& object : mapping->outputs) {
        processData.rxPdos.push_back(
            declarePdo(object, device, processDataSyncManager(config, true), true));
      }
      for (const PdoMappingObject& object : mapping->inputs) {
        processData.txPdos.push_back(
            declarePdo(object, device, processDataSyncManager(config, false), false));
      }
    }
    for (const mm::comm::SyncManagerConfig& syncManager : config.syncManagers) {
      mm::etg::EniSyncManager entry;
      entry.index = syncManager.index;
      entry.type = syncManagerType(syncManager.type);
      entry.startAddress = syncManager.physicalStart;
      entry.controlByte = static_cast<std::uint8_t>(syncManager.flags & 0xFFu);
      entry.enable = ((syncManager.flags >> 16) & 0x01u) != 0;
      if (carriesProcessData(syncManager)) {
        entry.defaultSize = syncManager.length;
      }
      processData.syncManagers.push_back(entry);
    }

    for (const mm::comm::FmmuConfig& fmmu : config.fmmus) {
      if (fmmu.active == 0) {
        continue;
      }
      const bool isOutput = (fmmu.type & kFmmuWrite) != 0;
      const std::uint32_t bitLength = static_cast<std::uint32_t>(fmmu.length) * 8;
      const mm::etg::EniProcessDataWindow window{
          .bitStart = fmmu.logicalStart * 8 + fmmu.logicalStartBit, .bitLength = bitLength};
      if (isOutput) {
        processData.send = window;
        outputBase = std::min(outputBase, fmmu.logicalStart);
        ++outputFmmus;
      } else if ((fmmu.type & kFmmuRead) != 0) {
        processData.recv = window;
        inputBase = std::min(inputBase, fmmu.logicalStart);
        ++inputFmmus;
      }
    }
    if (processData.send.has_value() || processData.recv.has_value() ||
        !processData.syncManagers.empty()) {
      slave.processData = processData;
    }

    // Mailbox.
    if (config.mailbox.writeLength != 0 || config.mailbox.readLength != 0) {
      mm::etg::EniMailbox mailbox;
      mailbox.send.start = config.mailbox.writeOffset;
      mailbox.send.length = config.mailbox.writeLength;
      mailbox.recv.start = config.mailbox.readOffset;
      mailbox.recv.length = config.mailbox.readLength;
      mailbox.protocols = decodeProtocols(config.mailbox.protocols);
      if (sii && sii->info.bootstrapReceiveMailboxSize != 0 &&
          sii->info.bootstrapSendMailboxSize != 0) {
        mm::etg::EniMailboxWindow bootstrapSend;
        bootstrapSend.start = sii->info.bootstrapReceiveMailboxOffset;
        bootstrapSend.length = sii->info.bootstrapReceiveMailboxSize;
        mm::etg::EniMailboxWindow bootstrapRecv;
        bootstrapRecv.start = sii->info.bootstrapSendMailboxOffset;
        bootstrapRecv.length = sii->info.bootstrapSendMailboxSize;
        mailbox.bootstrapSend = bootstrapSend;
        mailbox.bootstrapRecv = bootstrapRecv;
      }

      if (mapping) {
        appendPdoAssignment(mailbox.coeInitCmds, kRxPdoAssign, mapping->outputs);
        appendPdoAssignment(mailbox.coeInitCmds, kTxPdoAssign, mapping->inputs);
      }
      slave.mailbox = mailbox;
    }

    // Init commands. The mailbox Sync Managers are programmed before PRE-OP, because the mailbox
    // has to work in PRE-OP for the CoE downloads above to reach the device. The process-data ones
    // and the FMMUs wait for SAFE-OP, which is when the process image starts to exist.
    slave.initCmds.push_back(registerWrite(EniTransition::IP, EniCmd::Apwr, autoInc, kRegAlControl,
                                           {kAlStateInit, 0x00}, "request INIT"));
    slave.initCmds.push_back(
        stateCheck(EniTransition::IP, EniCmd::Aprd, autoInc, kAlStateInit, "wait for INIT"));
    const auto stationBytes = mm::core::toBytes<std::uint16_t>(station);
    slave.initCmds.push_back(registerWrite(
        EniTransition::IP, EniCmd::Apwr, autoInc, kRegConfiguredAddress,
        {stationBytes.begin(), stationBytes.end()}, "assign the configured station address"));
    for (const mm::comm::SyncManagerConfig& syncManager : config.syncManagers) {
      const auto transition =
          carriesProcessData(syncManager) ? EniTransition::PS : EniTransition::IP;
      slave.initCmds.push_back(registerWrite(
          transition, EniCmd::Fpwr, station,
          static_cast<std::uint16_t>(mm::comm::kSyncManagerRegisterBase +
                                     syncManager.index * mm::comm::kSyncManagerRegisterBytes),
          toVector(mm::comm::encodeSyncManager(syncManager)),
          std::format("set sync manager {}", syncManager.index)));
    }
    slave.initCmds.push_back(registerWrite(EniTransition::IP, EniCmd::Fpwr, station, kRegAlControl,
                                           {kAlStatePreOp, 0x00}, "request PRE-OP"));
    slave.initCmds.push_back(
        stateCheck(EniTransition::IP, EniCmd::Fprd, station, kAlStatePreOp, "wait for PRE-OP"));

    for (const mm::comm::FmmuConfig& fmmu : config.fmmus) {
      if (fmmu.active == 0) {
        continue;
      }
      slave.initCmds.push_back(registerWrite(
          EniTransition::PS, EniCmd::Fpwr, station,
          static_cast<std::uint16_t>(mm::comm::kFmmuRegisterBase +
                                     fmmu.index * mm::comm::kFmmuRegisterBytes),
          toVector(mm::comm::encodeFmmu(fmmu)), std::format("set FMMU {}", fmmu.index)));
    }
    slave.initCmds.push_back(registerWrite(EniTransition::PS, EniCmd::Fpwr, station, kRegAlControl,
                                           {kAlStateSafeOp, 0x00}, "request SAFE-OP"));
    slave.initCmds.push_back(
        stateCheck(EniTransition::PS, EniCmd::Fprd, station, kAlStateSafeOp, "wait for SAFE-OP"));
    slave.initCmds.push_back(registerWrite(EniTransition::SO, EniCmd::Fpwr, station, kRegAlControl,
                                           {kAlStateOp, 0x00}, "request OP"));
    slave.initCmds.push_back(
        stateCheck(EniTransition::SO, EniCmd::Fprd, station, kAlStateOp, "wait for OP"));

    collection.network.slaves.push_back(std::move(slave));
  }

  // The cyclic frame is one logical write for the outputs and one logical read for the inputs,
  // rather than a single LRW over both. A read-write datagram is only correct where a device's
  // input and output FMMUs share a logical address, and this master lays the two ranges out
  // disjointly.
  mm::etg::EniCyclic cyclic;
  cyclic.comment = "process data";
  cyclic.taskId = "process-data";
  if (options.cycleTimeUs != 0) {
    cyclic.cycleTimeUs = options.cycleTimeUs;
  }
  mm::etg::EniFrame frame;
  frame.comment = "one logical write and one logical read for the whole bus";
  if (image.outputBytes != 0 && outputFmmus != 0) {
    mm::etg::EniCyclicCmd write;
    write.states = {mm::etg::EniState::SafeOp, mm::etg::EniState::Op};
    write.comment = "write the output image";
    write.cmd = EniCmd::Lwr;
    write.addr = outputBase;
    write.dataLength = image.outputBytes;
    write.cnt = outputFmmus;
    write.inputOffs = 0;
    write.outputOffs = 0;
    frame.cmds.push_back(write);
  }
  if (image.inputBytes != 0 && inputFmmus != 0) {
    mm::etg::EniCyclicCmd read;
    read.states = {mm::etg::EniState::SafeOp, mm::etg::EniState::Op};
    read.comment = "read the input image";
    read.cmd = EniCmd::Lrd;
    read.addr = inputBase;
    read.dataLength = image.inputBytes;
    read.cnt = inputFmmus;
    read.inputOffs = 0;
    read.outputOffs = 0;
    frame.cmds.push_back(read);
  }
  if (!frame.cmds.empty()) {
    cyclic.frames.push_back(std::move(frame));
    collection.network.cyclic = std::move(cyclic);
  }

  // The process image, named. Every variable a master can show an operator comes from here.
  const auto area = [&manager](std::uint32_t byteSize,
                               const std::vector<ProcessImageObjectInfo>& objects) {
    mm::etg::EniProcessImageArea result;
    result.byteSize = byteSize;
    for (const ProcessImageObjectInfo& object : objects) {
      mm::etg::EniVariable variable;
      variable.name = object.name.empty()
                          ? std::format("Device {}.{:#06x}:{:02}", object.slavePosition,
                                        object.index, object.subindex)
                          : std::format("Device {}.{}", object.slavePosition, object.name);
      variable.bitSize = object.bitLength;
      variable.bitOffs = object.bitOffset;
      if (const DeviceHandle device = manager.deviceAt(object.slavePosition); device) {
        if (const auto parameter = device->parameter(object.index, object.subindex); parameter) {
          variable.dataType = iecTypeName(parameter->dataType);
        }
      }
      result.variables.push_back(std::move(variable));
    }
    return result;
  };
  mm::etg::EniProcessImage processImage;
  processImage.inputs = area(image.inputBytes, image.inputs);
  processImage.outputs = area(image.outputBytes, image.outputs);
  collection.network.processImage = std::move(processImage);

  return collection;
}

}  // namespace mm::node
