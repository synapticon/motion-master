#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "etg/eni.h"

namespace mm::etg::testing {

// The reference network below mirrors one real bus: a single SOMANET Circulo in position 1, read
// off the bench with `GET /api/bus-config` and `GET /api/devices/1/sii`. Measured values rather
// than invented ones mean the document this test writes is also what the schema-validation test
// checks, and a reader can hold it against hardware.
constexpr std::uint32_t kSynapticonVendorId = 8914;
constexpr std::uint32_t kCirculoProductCode = 769;
constexpr std::uint32_t kCirculoRevision = 285212674;
constexpr std::uint16_t kFirstStationAddress = 0x1001;

/// ESC register offsets the init commands address (ETG.1000.4, register description).
constexpr std::uint16_t kRegAlControl = 0x0120;
constexpr std::uint16_t kRegAlStatus = 0x0130;
constexpr std::uint16_t kRegSyncManager0 = 0x0800;
constexpr std::uint16_t kRegFmmu0 = 0x0600;

/// AL states as the AL Control register carries them.
constexpr std::uint8_t kAlStatePreOp = 0x02;
constexpr std::uint8_t kAlStateSafeOp = 0x04;
constexpr std::uint8_t kAlStateOp = 0x08;

inline EniEcatCmd writeRegister(EniTransition transition, std::uint16_t station, std::uint16_t reg,
                                std::vector<std::uint8_t> data, std::string comment) {
  EniEcatCmd command;
  command.transitions = {transition};
  command.comment = std::move(comment);
  command.requirement = EniRequires::Cycle;
  command.cmd = EniCmd::Fpwr;
  command.adp = station;
  command.ado = reg;
  command.data = std::move(data);
  command.cnt = 1;
  command.retries = 3;
  return command;
}

inline EniEcatCmd checkState(EniTransition transition, std::uint16_t station, std::uint8_t state) {
  EniEcatCmd command;
  command.transitions = {transition};
  command.comment = "check the device reached the state";
  command.requirement = EniRequires::Cycle;
  command.cmd = EniCmd::Fprd;
  command.adp = station;
  command.ado = kRegAlStatus;
  command.dataLength = 2;
  command.cnt = 1;
  command.retries = 3;
  command.validate =
      EniValidate{.data = {state, 0x00}, .dataMask = {0x0F, 0x00}, .timeoutMs = 10000};
  return command;
}

inline EniSyncManager syncManager(std::uint8_t index, EniSyncManagerType type,
                                  std::uint16_t startAddress, std::uint8_t controlByte,
                                  std::optional<std::uint32_t> defaultSize = std::nullopt) {
  EniSyncManager manager;
  manager.index = index;
  manager.type = type;
  manager.startAddress = startAddress;
  manager.controlByte = controlByte;
  manager.enable = true;
  manager.defaultSize = defaultSize;
  return manager;
}

inline EniMailboxWindow mailboxWindow(std::uint16_t start, std::uint16_t length) {
  EniMailboxWindow window;
  window.start = start;
  window.length = length;
  return window;
}

inline EniCoeCmd coeDownload(std::uint16_t index, std::uint8_t subindex,
                             std::vector<std::uint8_t> data, std::string comment) {
  EniCoeCmd command;
  command.transitions = {EniTransition::PS};
  command.comment = std::move(comment);
  command.timeoutMs = 1000;
  command.ccs = EniCoeCommandSpecifier::Download;
  command.index = index;
  command.subindex = subindex;
  command.data = std::move(data);
  return command;
}

inline EniVariable variable(std::string name, std::string dataType, std::uint32_t bitSize,
                            std::uint32_t bitOffs) {
  EniVariable entry;
  entry.name = std::move(name);
  entry.dataType = std::move(dataType);
  entry.bitSize = bitSize;
  entry.bitOffs = bitOffs;
  return entry;
}

inline EniNetwork referenceNetwork() {
  EniNetwork network;
  network.master.name = "Motion Master";
  network.master.destination = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  network.master.source = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  EniMailboxStates mailboxStates;
  mailboxStates.startAddr = 0x40000;
  mailboxStates.count = 2;
  network.master.mailboxStates = mailboxStates;
  EniEoe eoe;
  eoe.maxPorts = 3;
  eoe.maxFrames = 100;
  eoe.maxMacs = 100;
  network.master.eoe = eoe;

  EniSlave slave;
  slave.info.name = "SOMANET Circulo CiA402 Drive";
  slave.info.physAddr = kFirstStationAddress;
  slave.info.autoIncAddr = 0;
  slave.info.physics = eniPhysics(0x0011);
  slave.info.vendorId = kSynapticonVendorId;
  slave.info.productCode = kCirculoProductCode;
  slave.info.revisionNo = kCirculoRevision;
  slave.info.serialNo = 0;

  EniProcessData processData;
  processData.send = EniProcessDataWindow{.bitStart = 0, .bitLength = 280};
  processData.recv = EniProcessDataWindow{.bitStart = 0, .bitLength = 376};
  processData.syncManagers = {
      syncManager(0, EniSyncManagerType::MailboxOut, 0x1000, 0x26),
      syncManager(1, EniSyncManagerType::MailboxIn, 0x1400, 0x22),
      syncManager(2, EniSyncManagerType::Outputs, 0x1800, 0x64, 35),
      syncManager(3, EniSyncManagerType::Inputs, 0x1C00, 0x20, 47),
  };
  const auto pdoEntry = [](std::uint16_t index, std::uint8_t subindex, std::uint16_t bitLen,
                           std::string name, std::string dataType) {
    EniPdoEntry entry;
    entry.index = index;
    entry.subindex = subindex;
    entry.bitLen = bitLen;
    entry.name = std::move(name);
    entry.dataType = std::move(dataType);
    return entry;
  };

  EniPdo rxPdo;
  rxPdo.index = 0x1600;
  rxPdo.name = "RxPDO 0x1600";
  rxPdo.syncManager = 2;
  rxPdo.entries = {
      pdoEntry(0x6040, 0, 16, "Controlword", "UINT"),
      pdoEntry(0x607A, 0, 32, "Target position", "DINT"),
      // Padding: it occupies the window and addresses nothing, so it carries neither.
      pdoEntry(0, 0, 8, "", ""),
  };
  EniPdo txPdo;
  txPdo.index = 0x1A00;
  txPdo.name = "TxPDO 0x1A00";
  txPdo.syncManager = 3;
  txPdo.entries = {
      pdoEntry(0x6041, 0, 16, "Statusword", "UINT"),
      pdoEntry(0x6064, 0, 32, "Position actual value", "DINT"),
  };
  processData.rxPdos = {rxPdo};
  processData.txPdos = {txPdo};
  slave.processData = processData;

  EniMailbox mailbox;
  mailbox.send = mailboxWindow(0x1000, 1024);
  mailbox.recv = mailboxWindow(0x1400, 1024);
  mailbox.bootstrapSend = mailboxWindow(0x1000, 1024);
  mailbox.bootstrapRecv = mailboxWindow(0x1400, 1024);
  mailbox.protocols = {EniMailboxProtocol::Coe, EniMailboxProtocol::Foe};
  mailbox.coeInitCmds = {
      coeDownload(0x1C12, 0, {0x00}, "clear the RxPDO assignment of sync manager 2"),
      coeDownload(0x1C12, 1, {0x00, 0x16}, "assign 0x1600 to sync manager 2"),
  };
  slave.mailbox = mailbox;

  slave.initCmds = {
      writeRegister(EniTransition::IP, kFirstStationAddress, kRegAlControl, {kAlStatePreOp, 0x00},
                    "set the device to PRE-OP"),
      checkState(EniTransition::IP, kFirstStationAddress, kAlStatePreOp),
      writeRegister(EniTransition::PS, kFirstStationAddress, kRegSyncManager0 + 0x10,
                    {0x00, 0x18, 0x23, 0x00, 0x64, 0x00, 0x01, 0x00}, "set sync manager 2"),
      writeRegister(EniTransition::PS, kFirstStationAddress, kRegFmmu0,
                    // Logical start 0, length 0x23, bits 0 to 7, physical start 0x1800 bit 0, then
                    // the two single bytes an FMMU ends with: write enable at offset 0x0B, active
                    // at 0x0C. mm::comm::decodeFmmu is what says which is which.
                    {0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x07, 0x00, 0x18, 0x00, 0x02, 0x01,
                     0x00, 0x00, 0x00},
                    "set FMMU 0 for the outputs"),
      writeRegister(EniTransition::PS, kFirstStationAddress, kRegAlControl, {kAlStateSafeOp, 0x00},
                    "set the device to SAFE-OP"),
      checkState(EniTransition::PS, kFirstStationAddress, kAlStateSafeOp),
      writeRegister(EniTransition::SO, kFirstStationAddress, kRegAlControl, {kAlStateOp, 0x00},
                    "set the device to OP"),
      checkState(EniTransition::SO, kFirstStationAddress, kAlStateOp),
  };
  network.slaves.push_back(slave);

  // A second device, only so the reference document exercises the two elements a first device never
  // carries: it is plugged into port 1 of the drive above, and its clock is disciplined.
  EniSlave downstream;
  downstream.info.name = "SOMANET Circulo CiA402 Drive";
  downstream.info.physAddr = kFirstStationAddress + 1;
  downstream.info.autoIncAddr = 0xFFFF;
  downstream.info.physics = eniPhysics(0x0011);
  downstream.info.vendorId = kSynapticonVendorId;
  downstream.info.productCode = kCirculoProductCode;
  downstream.info.revisionNo = kCirculoRevision;
  EniPreviousPort previousPort;
  previousPort.port = EniPort::B;
  previousPort.selected = true;
  previousPort.physAddr = kFirstStationAddress;
  downstream.previousPorts = {previousPort};
  EniDc dc;
  dc.potentialReferenceClock = true;
  dc.referenceClock = false;
  dc.cycleTime0Ns = 1000000;
  dc.cycleTime1Ns = 0;
  dc.shiftTimeNs = 250000;
  downstream.dc = dc;
  network.slaves.push_back(downstream);

  EniCyclicCmd exchange;
  exchange.states = {EniState::SafeOp, EniState::Op};
  exchange.comment = "exchange the whole process image";
  exchange.cmd = EniCmd::Lrw;
  exchange.addr = 0;
  exchange.dataLength = 82;
  exchange.cnt = 3;
  exchange.inputOffs = 0;
  exchange.outputOffs = 0;

  EniCyclic cyclic;
  cyclic.comment = "process data";
  cyclic.cycleTimeUs = 1000;
  cyclic.taskId = "process-data";
  cyclic.frames = {EniFrame{.comment = "one LRW for the whole bus", .cmds = {exchange}}};
  network.cyclic = cyclic;

  EniProcessImage image;
  image.inputs =
      EniProcessImageArea{.byteSize = 35,
                          .variables = {variable("Drive 1.Statusword", "UINT", 16, 0),
                                        variable("Drive 1.Position actual value", "DINT", 32, 16)}};
  image.outputs =
      EniProcessImageArea{.byteSize = 47,
                          .variables = {variable("Drive 1.Controlword", "UINT", 16, 0),
                                        variable("Drive 1.Target position", "DINT", 32, 16)}};
  network.processImage = image;

  return network;
}

}  // namespace mm::etg::testing
