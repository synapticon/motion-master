#include "etg/eni.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <pugixml.hpp>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/util.h"

namespace mm::etg {

namespace {

constexpr std::size_t kMacBytes = 6;
constexpr std::uint8_t kMaxSyncManagerIndex = 15;
constexpr std::size_t kMaxCyclicStates = 4;

constexpr std::string_view transitionName(EniTransition transition) {
  switch (transition) {
    case EniTransition::II:
      return "II";
    case EniTransition::IP:
      return "IP";
    case EniTransition::PP:
      return "PP";
    case EniTransition::PO:
      return "PO";
    case EniTransition::PS:
      return "PS";
    case EniTransition::PI:
      return "PI";
    case EniTransition::SS:
      return "SS";
    case EniTransition::SP:
      return "SP";
    case EniTransition::SO:
      return "SO";
    case EniTransition::SI:
      return "SI";
    case EniTransition::OS:
      return "OS";
    case EniTransition::OP:
      return "OP";
    case EniTransition::OI:
      return "OI";
    case EniTransition::IB:
      return "IB";
    case EniTransition::BI:
      return "BI";
  }
  return "";
}

constexpr std::string_view stateName(EniState state) {
  switch (state) {
    case EniState::Init:
      return "INIT";
    case EniState::PreOp:
      return "PREOP";
    case EniState::SafeOp:
      return "SAFEOP";
    case EniState::Op:
      return "OP";
  }
  return "";
}

constexpr std::string_view requiresName(EniRequires requirement) {
  switch (requirement) {
    case EniRequires::None:
      return "";
    case EniRequires::Frame:
      return "frame";
    case EniRequires::Cycle:
      return "cycle";
  }
  return "";
}

constexpr std::string_view syncManagerTypeName(EniSyncManagerType type) {
  switch (type) {
    case EniSyncManagerType::MailboxOut:
      return "MBoxOut";
    case EniSyncManagerType::MailboxIn:
      return "MBoxIn";
    case EniSyncManagerType::Outputs:
      return "Outputs";
    case EniSyncManagerType::Inputs:
      return "Inputs";
  }
  return "";
}

constexpr std::string_view portName(EniPort port) {
  switch (port) {
    case EniPort::A:
      return "A";
    case EniPort::B:
      return "B";
    case EniPort::C:
      return "C";
    case EniPort::D:
      return "D";
  }
  return "";
}

constexpr std::string_view mailboxProtocolName(EniMailboxProtocol protocol) {
  switch (protocol) {
    case EniMailboxProtocol::Aoe:
      return "AoE";
    case EniMailboxProtocol::Eoe:
      return "EoE";
    case EniMailboxProtocol::Coe:
      return "CoE";
    case EniMailboxProtocol::Soe:
      return "SoE";
    case EniMailboxProtocol::Foe:
      return "FoE";
    case EniMailboxProtocol::Voe:
      return "VoE";
  }
  return "";
}

// ---------------------------------------------------------------------------------------------
// Element writers.
//
// Every element is appended in schema order. The ENI complex types are all `xs:sequence`, so the
// call order in this file *is* the contract — moving one append_child below its neighbour produces
// a document that no longer validates.
// ---------------------------------------------------------------------------------------------

void addText(pugi::xml_node parent, const char* name, std::string_view value) {
  parent.append_child(name).text().set(std::string(value).c_str());
}

void addUint(pugi::xml_node parent, const char* name, std::uint64_t value) {
  addText(parent, name, std::to_string(value));
}

void addInt(pugi::xml_node parent, const char* name, std::int64_t value) {
  addText(parent, name, std::to_string(value));
}

void addBool(pugi::xml_node parent, const char* name, bool value) {
  addText(parent, name, value ? "1" : "0");
}

void addHex(pugi::xml_node parent, const char* name, std::span<const std::uint8_t> bytes) {
  addText(parent, name, mm::core::toHex(bytes));
}

/// Writes the address and payload choices shared by an init command and a cyclic command.
///
/// Both are `xs:choice`, so exactly one branch of each is written. `validateCommand` has already
/// established which, so this makes no decision the caller could disagree with.
void addAddressAndPayload(pugi::xml_node cmd, const std::optional<std::uint16_t>& adp,
                          const std::optional<std::uint16_t>& ado,
                          const std::optional<std::uint32_t>& addr,
                          std::span<const std::uint8_t> data,
                          const std::optional<std::uint32_t>& dataLength) {
  if (addr.has_value()) {
    addUint(cmd, "Addr", *addr);
  } else {
    if (adp.has_value()) {
      addUint(cmd, "Adp", *adp);
    }
    addUint(cmd, "Ado", ado.value_or(0));
  }
  if (dataLength.has_value()) {
    addUint(cmd, "DataLength", *dataLength);
  } else {
    addHex(cmd, "Data", data);
  }
}

void addEcatCmd(pugi::xml_node parent, const EniEcatCmd& command) {
  pugi::xml_node cmd = parent.append_child("InitCmd");
  for (const EniTransition transition : command.transitions) {
    addText(cmd, "Transition", transitionName(transition));
  }
  if (command.beforeSlave) {
    addBool(cmd, "BeforeSlave", true);
  }
  if (!command.comment.empty()) {
    addText(cmd, "Comment", command.comment);
  }
  if (command.requirement != EniRequires::None) {
    addText(cmd, "Requires", requiresName(command.requirement));
  }
  addUint(cmd, "Cmd", static_cast<std::uint64_t>(command.cmd));
  addAddressAndPayload(cmd, command.adp, command.ado, command.addr, command.data,
                       command.dataLength);
  if (command.cnt.has_value()) {
    addUint(cmd, "Cnt", *command.cnt);
  }
  if (command.retries.has_value()) {
    addUint(cmd, "Retries", *command.retries);
  }
  if (command.validate.has_value()) {
    pugi::xml_node validate = cmd.append_child("Validate");
    addHex(validate, "Data", command.validate->data);
    if (!command.validate->dataMask.empty()) {
      addHex(validate, "DataMask", command.validate->dataMask);
    }
    addUint(validate, "Timeout", command.validate->timeoutMs);
  } else if (command.timeoutMs.has_value()) {
    addUint(cmd, "Timeout", *command.timeoutMs);
  }
}

void addEcatCmds(pugi::xml_node parent, const std::vector<EniEcatCmd>& commands) {
  if (commands.empty()) {
    return;
  }
  pugi::xml_node initCmds = parent.append_child("InitCmds");
  for (const EniEcatCmd& command : commands) {
    addEcatCmd(initCmds, command);
  }
}

void addCoeCmd(pugi::xml_node parent, const EniCoeCmd& command) {
  pugi::xml_node cmd = parent.append_child("InitCmd");
  for (const EniTransition transition : command.transitions) {
    addText(cmd, "Transition", transitionName(transition));
  }
  if (!command.comment.empty()) {
    addText(cmd, "Comment", command.comment);
  }
  addUint(cmd, "Timeout", command.timeoutMs);
  addUint(cmd, "Ccs", static_cast<std::uint64_t>(command.ccs));
  addUint(cmd, "Index", command.index);
  addUint(cmd, "SubIndex", command.subindex);
  if (!command.data.empty()) {
    addHex(cmd, "Data", command.data);
  }
  if (command.disabled) {
    addBool(cmd, "Disabled", true);
  }
}

void addMailboxWindow(pugi::xml_node parent, const char* name, const EniMailboxWindow& window,
                      bool writeRecvFields) {
  pugi::xml_node node = parent.append_child(name);
  addUint(node, "Start", window.start);
  addUint(node, "Length", window.length);
  if (!writeRecvFields) {
    return;
  }
  if (window.pollTime.has_value()) {
    addUint(node, "PollTime", *window.pollTime);
  }
  if (window.statusBitAddr.has_value()) {
    addUint(node, "StatusBitAddr", *window.statusBitAddr);
  }
}

void addMailbox(pugi::xml_node parent, const EniMailbox& mailbox) {
  pugi::xml_node node = parent.append_child("Mailbox");
  addMailboxWindow(node, "Send", mailbox.send, false);
  addMailboxWindow(node, "Recv", mailbox.recv, true);
  if (mailbox.bootstrapSend.has_value() && mailbox.bootstrapRecv.has_value()) {
    pugi::xml_node bootstrap = node.append_child("BootStrap");
    addMailboxWindow(bootstrap, "Send", *mailbox.bootstrapSend, false);
    addMailboxWindow(bootstrap, "Recv", *mailbox.bootstrapRecv, true);
  }
  for (const EniMailboxProtocol protocol : mailbox.protocols) {
    addText(node, "Protocol", mailboxProtocolName(protocol));
  }
  if (!mailbox.coeInitCmds.empty()) {
    pugi::xml_node coe = node.append_child("CoE");
    pugi::xml_node initCmds = coe.append_child("InitCmds");
    for (const EniCoeCmd& command : mailbox.coeInitCmds) {
      addCoeCmd(initCmds, command);
    }
  }
}

void addSyncManager(pugi::xml_node parent, const EniSyncManager& syncManager) {
  const std::string name = std::format("Sm{}", syncManager.index);
  pugi::xml_node node = parent.append_child(name.c_str());
  addText(node, "Type", syncManagerTypeName(syncManager.type));
  if (syncManager.minSize.has_value()) {
    addUint(node, "MinSize", *syncManager.minSize);
  }
  if (syncManager.maxSize.has_value()) {
    addUint(node, "MaxSize", *syncManager.maxSize);
  }
  if (syncManager.defaultSize.has_value()) {
    addUint(node, "DefaultSize", *syncManager.defaultSize);
  }
  addUint(node, "StartAddress", syncManager.startAddress);
  addUint(node, "ControlByte", syncManager.controlByte);
  addBool(node, "Enable", syncManager.enable);
  if (syncManager.watchdog.has_value()) {
    addUint(node, "Watchdog", *syncManager.watchdog);
  }
}

void addProcessData(pugi::xml_node parent, const EniProcessData& processData) {
  pugi::xml_node node = parent.append_child("ProcessData");
  if (processData.send.has_value()) {
    pugi::xml_node send = node.append_child("Send");
    addUint(send, "BitStart", processData.send->bitStart);
    addUint(send, "BitLength", processData.send->bitLength);
  }
  if (processData.recv.has_value()) {
    pugi::xml_node recv = node.append_child("Recv");
    addUint(recv, "BitStart", processData.recv->bitStart);
    addUint(recv, "BitLength", processData.recv->bitLength);
  }
  // The schema names Sm0 to Sm15 as distinct elements in ascending order, so the entries are
  // sorted by index rather than written in the order the caller supplied them.
  std::vector<EniSyncManager> ordered = processData.syncManagers;
  std::ranges::sort(ordered, {}, &EniSyncManager::index);
  for (const EniSyncManager& syncManager : ordered) {
    addSyncManager(node, syncManager);
  }
}

void addPreviousPort(pugi::xml_node parent, const EniPreviousPort& previousPort) {
  pugi::xml_node node = parent.append_child("PreviousPort");
  // The only attribute this format puts on an element the writer emits. It is what separates the
  // port the device is actually plugged into from the ports it could be moved to.
  node.append_attribute("Selected") = previousPort.selected ? "1" : "0";
  if (previousPort.deviceId.has_value()) {
    addUint(node, "DeviceId", *previousPort.deviceId);
  }
  addText(node, "Port", portName(previousPort.port));
  if (previousPort.physAddr.has_value()) {
    addUint(node, "PhysAddr", *previousPort.physAddr);
  }
}

void addDc(pugi::xml_node parent, const EniDc& dc) {
  pugi::xml_node node = parent.append_child("DC");
  if (dc.potentialReferenceClock.has_value()) {
    addBool(node, "PotentialReferenceClock", *dc.potentialReferenceClock);
  }
  if (dc.referenceClock.has_value()) {
    addBool(node, "ReferenceClock", *dc.referenceClock);
  }
  if (dc.cycleTime0Ns.has_value()) {
    addInt(node, "CycleTime0", *dc.cycleTime0Ns);
  }
  if (dc.cycleTime1Ns.has_value()) {
    addInt(node, "CycleTime1", *dc.cycleTime1Ns);
  }
  if (dc.shiftTimeNs.has_value()) {
    addInt(node, "ShiftTime", *dc.shiftTimeNs);
  }
}

void addSlave(pugi::xml_node parent, const EniSlave& slave) {
  pugi::xml_node node = parent.append_child("Slave");
  pugi::xml_node info = node.append_child("Info");
  addText(info, "Name", slave.info.name);
  addUint(info, "PhysAddr", slave.info.physAddr);
  addUint(info, "AutoIncAddr", slave.info.autoIncAddr);
  addText(info, "Physics", slave.info.physics);
  addUint(info, "VendorId", slave.info.vendorId);
  addUint(info, "ProductCode", slave.info.productCode);
  addUint(info, "RevisionNo", slave.info.revisionNo);
  addUint(info, "SerialNo", slave.info.serialNo);
  if (slave.processData.has_value()) {
    addProcessData(node, *slave.processData);
  }
  if (slave.mailbox.has_value()) {
    addMailbox(node, *slave.mailbox);
  }
  addEcatCmds(node, slave.initCmds);
  for (const EniPreviousPort& previousPort : slave.previousPorts) {
    addPreviousPort(node, previousPort);
  }
  if (slave.dc.has_value()) {
    addDc(node, *slave.dc);
  }
}

void addCyclic(pugi::xml_node parent, const EniCyclic& cyclic) {
  pugi::xml_node node = parent.append_child("Cyclic");
  if (!cyclic.comment.empty()) {
    addText(node, "Comment", cyclic.comment);
  }
  if (cyclic.cycleTimeUs.has_value()) {
    addUint(node, "CycleTime", *cyclic.cycleTimeUs);
  }
  if (cyclic.priority.has_value()) {
    addUint(node, "Priority", *cyclic.priority);
  }
  if (!cyclic.taskId.empty()) {
    addText(node, "TaskId", cyclic.taskId);
  }
  for (const EniFrame& frame : cyclic.frames) {
    pugi::xml_node frameNode = node.append_child("Frame");
    if (!frame.comment.empty()) {
      addText(frameNode, "Comment", frame.comment);
    }
    for (const EniCyclicCmd& command : frame.cmds) {
      pugi::xml_node cmd = frameNode.append_child("Cmd");
      for (const EniState state : command.states) {
        addText(cmd, "State", stateName(state));
      }
      if (!command.comment.empty()) {
        addText(cmd, "Comment", command.comment);
      }
      addUint(cmd, "Cmd", static_cast<std::uint64_t>(command.cmd));
      addAddressAndPayload(cmd, command.adp, command.ado, command.addr, command.data,
                           command.dataLength);
      if (command.cnt.has_value()) {
        addUint(cmd, "Cnt", *command.cnt);
      }
      addUint(cmd, "InputOffs", command.inputOffs);
      addUint(cmd, "OutputOffs", command.outputOffs);
    }
  }
}

void addProcessImageArea(pugi::xml_node parent, const char* name, const EniProcessImageArea& area) {
  pugi::xml_node node = parent.append_child(name);
  addUint(node, "ByteSize", area.byteSize);
  for (const EniVariable& variable : area.variables) {
    pugi::xml_node entry = node.append_child("Variable");
    addText(entry, "Name", variable.name);
    if (!variable.comment.empty()) {
      addText(entry, "Comment", variable.comment);
    }
    if (!variable.dataType.empty()) {
      addText(entry, "DataType", variable.dataType);
    }
    addUint(entry, "BitSize", variable.bitSize);
    addUint(entry, "BitOffs", variable.bitOffs);
  }
}

// ---------------------------------------------------------------------------------------------
// Validation.
//
// Everything the schema expresses as a choice or a bound is checked before a single element is
// written, so `writeEni` either returns a valid document or an error naming the field. A partly
// written document is never returned.
// ---------------------------------------------------------------------------------------------

std::expected<void, std::string> validateCommand(std::string_view where,
                                                 const std::optional<std::uint16_t>& adp,
                                                 const std::optional<std::uint16_t>& ado,
                                                 const std::optional<std::uint32_t>& addr,
                                                 std::span<const std::uint8_t> data,
                                                 const std::optional<std::uint32_t>& dataLength) {
  if (addr.has_value() && (adp.has_value() || ado.has_value())) {
    return std::unexpected(
        std::format("{}: addr excludes adp and ado; the schema offers one or the other", where));
  }
  if (!addr.has_value() && !ado.has_value()) {
    return std::unexpected(std::format("{}: needs either ado or addr", where));
  }
  if (!data.empty() && dataLength.has_value()) {
    return std::unexpected(
        std::format("{}: data excludes dataLength; the schema offers one or the other", where));
  }
  if (data.empty() && !dataLength.has_value()) {
    return std::unexpected(std::format("{}: needs either data or dataLength", where));
  }
  return {};
}

std::expected<void, std::string> validateEcatCmds(std::string_view where,
                                                  const std::vector<EniEcatCmd>& commands) {
  for (std::size_t i = 0; i < commands.size(); ++i) {
    const EniEcatCmd& command = commands[i];
    const std::string at = std::format("{} init command {}", where, i);
    if (auto ok = validateCommand(at, command.adp, command.ado, command.addr, command.data,
                                  command.dataLength);
        !ok) {
      return ok;
    }
    if (command.validate.has_value() && command.timeoutMs.has_value()) {
      return std::unexpected(std::format("{}: validate excludes timeoutMs", at));
    }
  }
  return {};
}

std::expected<void, std::string> validateSlave(std::size_t position, const EniSlave& slave) {
  const std::string where = std::format("slave {}", position);
  if (slave.processData.has_value()) {
    std::set<std::uint8_t> used;
    for (const EniSyncManager& syncManager : slave.processData->syncManagers) {
      if (syncManager.index > kMaxSyncManagerIndex) {
        return std::unexpected(std::format("{}: sync manager index {} is above {}", where,
                                           syncManager.index, kMaxSyncManagerIndex));
      }
      if (!used.insert(syncManager.index).second) {
        return std::unexpected(
            std::format("{}: sync manager index {} is used twice", where, syncManager.index));
      }
    }
  }
  if (std::ranges::any_of(slave.previousPorts,
                          [](const EniPreviousPort& p) { return p.port == EniPort::A; })) {
    return std::unexpected(
        std::format("{}: previous port A is spec-legal but not in ENI Schema 1.7, which enumerates "
                    "B, C and D only — a reader may accept it, a writer cannot emit it",
                    where));
  }
  if (slave.mailbox.has_value()) {
    const EniMailbox& mailbox = *slave.mailbox;
    if (mailbox.bootstrapSend.has_value() != mailbox.bootstrapRecv.has_value()) {
      return std::unexpected(
          std::format("{}: the bootstrap mailbox needs both a send and a receive window", where));
    }
    for (std::size_t i = 0; i < mailbox.coeInitCmds.size(); ++i) {
      if (mailbox.coeInitCmds[i].transitions.empty()) {
        return std::unexpected(
            std::format("{}: CoE init command {} names no transition", where, i));
      }
    }
  }
  return validateEcatCmds(where, slave.initCmds);
}

std::expected<void, std::string> validateCyclic(const EniCyclic& cyclic) {
  if (cyclic.frames.empty()) {
    return std::unexpected("cyclic: needs at least one frame");
  }
  for (std::size_t f = 0; f < cyclic.frames.size(); ++f) {
    const EniFrame& frame = cyclic.frames[f];
    if (frame.cmds.empty()) {
      return std::unexpected(std::format("cyclic frame {}: needs at least one command", f));
    }
    for (std::size_t c = 0; c < frame.cmds.size(); ++c) {
      const EniCyclicCmd& command = frame.cmds[c];
      const std::string at = std::format("cyclic frame {} command {}", f, c);
      if (command.states.empty() || command.states.size() > kMaxCyclicStates) {
        return std::unexpected(std::format("{}: names {} states; the schema allows 1 to {}", at,
                                           command.states.size(), kMaxCyclicStates));
      }
      if (auto ok = validateCommand(at, command.adp, command.ado, command.addr, command.data,
                                    command.dataLength);
          !ok) {
        return ok;
      }
    }
  }
  return {};
}

std::expected<void, std::string> validateNetwork(const EniNetwork& network) {
  if (network.master.destination.size() != kMacBytes) {
    return std::unexpected(std::format("master: destination MAC is {} bytes, needs {}",
                                       network.master.destination.size(), kMacBytes));
  }
  if (network.master.source.size() != kMacBytes) {
    return std::unexpected(std::format("master: source MAC is {} bytes, needs {}",
                                       network.master.source.size(), kMacBytes));
  }
  if (auto ok = validateEcatCmds("master", network.master.initCmds); !ok) {
    return ok;
  }
  for (std::size_t i = 0; i < network.slaves.size(); ++i) {
    if (auto ok = validateSlave(i + 1, network.slaves[i]); !ok) {
      return ok;
    }
  }
  if (network.cyclic.has_value()) {
    if (auto ok = validateCyclic(*network.cyclic); !ok) {
      return ok;
    }
  }
  return {};
}

}  // namespace

std::string eniPhysics(std::uint16_t physicalPort) {
  std::string physics;
  for (int port = 0; port < 4; ++port) {
    const auto code = static_cast<std::uint8_t>((physicalPort >> (4 * port)) & 0x0Fu);
    switch (code) {
      case 0x00:
        physics += ' ';
        break;
      case 0x01:
        physics += 'Y';
        break;
      case 0x03:
        physics += 'K';
        break;
      case 0x04:
        physics += 'B';
        break;
      default:
        physics += ' ';
        break;
    }
  }
  // A port that is not in use contributes a space, and the schema pattern permits one, but a
  // trailing run of them says nothing. Trim it so a two-port device reads "YY".
  while (!physics.empty() && physics.back() == ' ') {
    physics.pop_back();
  }
  return physics;
}

std::expected<std::string, std::string> writeEni(const EniNetwork& network) {
  if (auto ok = validateNetwork(network); !ok) {
    return std::unexpected(ok.error());
  }

  pugi::xml_document doc;
  pugi::xml_node declaration = doc.append_child(pugi::node_declaration);
  declaration.append_attribute("version") = "1.0";
  declaration.append_attribute("encoding") = "UTF-8";

  pugi::xml_node config = doc.append_child("EtherCATConfig").append_child("Config");

  pugi::xml_node master = config.append_child("Master");
  pugi::xml_node masterInfo = master.append_child("Info");
  addText(masterInfo, "Name", network.master.name);
  addHex(masterInfo, "Destination", network.master.destination);
  addHex(masterInfo, "Source", network.master.source);
  if (network.master.etherType.has_value()) {
    // EtherType is hexBinary rather than an integer, and it is written little-endian like every
    // other payload in the format: the ENI files ETG ships write 0x88A4 as "a488".
    addHex(masterInfo, "EtherType", mm::core::toBytes<std::uint16_t>(*network.master.etherType));
  }
  addEcatCmds(master, network.master.initCmds);

  for (const EniSlave& slave : network.slaves) {
    addSlave(config, slave);
  }

  if (network.cyclic.has_value()) {
    addCyclic(config, *network.cyclic);
  }

  if (network.processImage.has_value()) {
    pugi::xml_node image = config.append_child("ProcessImage");
    if (network.processImage->inputs.has_value()) {
      addProcessImageArea(image, "Inputs", *network.processImage->inputs);
    }
    if (network.processImage->outputs.has_value()) {
      addProcessImageArea(image, "Outputs", *network.processImage->outputs);
    }
  }

  std::ostringstream out;
  doc.save(out, "  ");
  return out.str();
}

}  // namespace mm::etg
