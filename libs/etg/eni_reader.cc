#include "etg/eni_reader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/util.h"

namespace mm::etg {

namespace {

/// Every recoverable problem lands here rather than aborting the read. The path is an XPath-ish
/// breadcrumb (@c "Slave[2]/InitCmds/InitCmd[7]") so a warning names a spot in the file a human can
/// find, the same shape the ESI parser's warnings take.
class Warnings {
 public:
  void add(std::string_view path, std::string_view what) {
    messages_.push_back(std::format("{}: {}", path, what));
  }

  /// Names an element the document carries and this model does not, once per path.
  void notModelled(std::string_view path, const char* element) {
    add(path, std::format("<{}> is not modelled and was dropped", element));
  }

  std::vector<std::string> take() { return std::move(messages_); }

 private:
  std::vector<std::string> messages_;
};

bool hasChild(const pugi::xml_node& node, const char* name) {
  return static_cast<bool>(node.child(name));
}

std::string childText(const pugi::xml_node& node, const char* name) {
  return node.child(name).text().as_string();
}

/// Reads an integer child. The schema types every numeric element as `xs:int`, so a value that will
/// not parse is a malformed document rather than a different notation.
template <typename T>
std::optional<T> childNumber(const pugi::xml_node& node, const char* name, std::string_view path,
                             Warnings& warnings) {
  if (!hasChild(node, name)) {
    return std::nullopt;
  }
  const std::string text = childText(node, name);
  const auto value = mm::core::parseHexOrDec<T>(text);
  if (!value) {
    warnings.add(path, std::format("<{}> is not a number: '{}'", name, text));
    return std::nullopt;
  }
  return value;
}

/// Reads a signed integer child, which the DC times need — `CycleTime1` is a derived figure and can
/// be negative. `parseHexOrDec` is unsigned-only, so this parses the sign itself.
std::optional<std::int32_t> childSigned(const pugi::xml_node& node, const char* name,
                                        std::string_view path, Warnings& warnings) {
  if (!hasChild(node, name)) {
    return std::nullopt;
  }
  std::string text = childText(node, name);
  const bool negative = text.starts_with('-');
  if (negative || text.starts_with('+')) {
    text.erase(0, 1);
  }
  const auto magnitude = mm::core::parseHexOrDec<std::uint32_t>(text);
  if (!magnitude) {
    warnings.add(path, std::format("<{}> is not a number: '{}'", name, childText(node, name)));
    return std::nullopt;
  }
  const auto value = static_cast<std::int32_t>(*magnitude);
  return negative ? -value : value;
}

std::optional<bool> childBool(const pugi::xml_node& node, const char* name) {
  if (!hasChild(node, name)) {
    return std::nullopt;
  }
  const std::string text = childText(node, name);
  return text == "1" || text == "true";
}

std::vector<std::uint8_t> childHex(const pugi::xml_node& node, const char* name,
                                   std::string_view path, Warnings& warnings) {
  if (!hasChild(node, name)) {
    return {};
  }
  const std::string text = childText(node, name);
  auto bytes = mm::core::fromHex(text);
  if (!bytes) {
    warnings.add(path, std::format("<{}> is not hexBinary: '{}'", name, text));
    return {};
  }
  return std::move(*bytes);
}

// ---------------------------------------------------------------------------------------------
// Name to enum.
//
// Each of these is the inverse of a name function in eni.cc. An unrecognised name warns and takes
// the fallback, because a document written against a later version of the format is still worth
// reading for everything else it says.
// ---------------------------------------------------------------------------------------------

constexpr std::array<std::pair<std::string_view, EniTransition>, 15> kTransitions = {{
    {"II", EniTransition::II},
    {"IP", EniTransition::IP},
    {"PP", EniTransition::PP},
    {"PO", EniTransition::PO},
    {"PS", EniTransition::PS},
    {"PI", EniTransition::PI},
    {"SS", EniTransition::SS},
    {"SP", EniTransition::SP},
    {"SO", EniTransition::SO},
    {"SI", EniTransition::SI},
    {"OS", EniTransition::OS},
    {"OP", EniTransition::OP},
    {"OI", EniTransition::OI},
    {"IB", EniTransition::IB},
    {"BI", EniTransition::BI},
}};

constexpr std::array<std::pair<std::string_view, EniState>, 4> kStates = {{
    {"INIT", EniState::Init},
    {"PREOP", EniState::PreOp},
    {"SAFEOP", EniState::SafeOp},
    {"OP", EniState::Op},
}};

constexpr std::array<std::pair<std::string_view, EniSyncManagerType>, 4> kSyncManagerTypes = {{
    {"MBoxOut", EniSyncManagerType::MailboxOut},
    {"MBoxIn", EniSyncManagerType::MailboxIn},
    {"Outputs", EniSyncManagerType::Outputs},
    {"Inputs", EniSyncManagerType::Inputs},
}};

constexpr std::array<std::pair<std::string_view, EniMailboxProtocol>, 6> kMailboxProtocols = {{
    {"AoE", EniMailboxProtocol::Aoe},
    {"EoE", EniMailboxProtocol::Eoe},
    {"CoE", EniMailboxProtocol::Coe},
    {"SoE", EniMailboxProtocol::Soe},
    {"FoE", EniMailboxProtocol::Foe},
    {"VoE", EniMailboxProtocol::Voe},
}};

/// Port A is here and absent from `writeEni`'s output on purpose: ETG.2100 Table 29 allows all
/// four, ENI Schema 1.7 enumerates B, C and D only, and a reader takes what the file says.
constexpr std::array<std::pair<std::string_view, EniPort>, 4> kPorts = {{
    {"A", EniPort::A},
    {"B", EniPort::B},
    {"C", EniPort::C},
    {"D", EniPort::D},
}};

template <typename T, std::size_t N>
std::optional<T> lookUp(const std::array<std::pair<std::string_view, T>, N>& table,
                        std::string_view name) {
  const auto it = std::ranges::find(table, name, &std::pair<std::string_view, T>::first);
  return it == table.end() ? std::nullopt : std::optional<T>{it->second};
}

template <typename T, std::size_t N>
T lookUpOr(const std::array<std::pair<std::string_view, T>, N>& table, std::string_view name,
           T fallback, const char* what, std::string_view path, Warnings& warnings) {
  if (const auto found = lookUp(table, name); found) {
    return *found;
  }
  warnings.add(path, std::format("unknown {} '{}'", what, name));
  return fallback;
}

// ---------------------------------------------------------------------------------------------
// Element readers, each the inverse of its counterpart in eni.cc.
// ---------------------------------------------------------------------------------------------

std::vector<EniTransition> readTransitions(const pugi::xml_node& node, std::string_view path,
                                           Warnings& warnings) {
  std::vector<EniTransition> transitions;
  for (const pugi::xml_node child : node.children("Transition")) {
    transitions.push_back(lookUpOr(kTransitions, child.text().as_string(), EniTransition::IP,
                                   "transition", path, warnings));
  }
  return transitions;
}

EniEcatCmd readEcatCmd(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EniEcatCmd command;
  command.transitions = readTransitions(node, path, warnings);
  command.beforeSlave = childBool(node, "BeforeSlave").value_or(false);
  command.comment = childText(node, "Comment");
  if (const std::string requires_ = childText(node, "Requires"); !requires_.empty()) {
    if (requires_ == "frame") {
      command.requirement = EniRequires::Frame;
    } else if (requires_ == "cycle") {
      command.requirement = EniRequires::Cycle;
    } else {
      warnings.add(path, std::format("unknown <Requires> '{}'", requires_));
    }
  }
  const auto cmd = childNumber<std::uint8_t>(node, "Cmd", path, warnings);
  if (cmd && *cmd <= static_cast<std::uint8_t>(EniCmd::Frmw)) {
    command.cmd = static_cast<EniCmd>(*cmd);
  } else if (cmd) {
    warnings.add(path, std::format("<Cmd> {} is reserved", *cmd));
  }
  command.adp = childNumber<std::uint16_t>(node, "Adp", path, warnings);
  command.ado = childNumber<std::uint16_t>(node, "Ado", path, warnings);
  command.addr = childNumber<std::uint32_t>(node, "Addr", path, warnings);
  command.data = childHex(node, "Data", path, warnings);
  command.dataLength = childNumber<std::uint32_t>(node, "DataLength", path, warnings);
  command.cnt = childNumber<std::uint32_t>(node, "Cnt", path, warnings);
  command.retries = childNumber<std::uint32_t>(node, "Retries", path, warnings);
  if (const pugi::xml_node validate = node.child("Validate"); validate) {
    EniValidate value;
    value.data = childHex(validate, "Data", path, warnings);
    value.dataMask = childHex(validate, "DataMask", path, warnings);
    value.timeoutMs = childNumber<std::uint32_t>(validate, "Timeout", path, warnings).value_or(0);
    command.validate = value;
  } else {
    command.timeoutMs = childNumber<std::uint32_t>(node, "Timeout", path, warnings);
  }
  return command;
}

std::vector<EniEcatCmd> readEcatCmds(const pugi::xml_node& parent, std::string_view path,
                                     Warnings& warnings) {
  std::vector<EniEcatCmd> commands;
  std::size_t index = 0;
  for (const pugi::xml_node node : parent.child("InitCmds").children("InitCmd")) {
    commands.push_back(readEcatCmd(node, std::format("{}/InitCmd[{}]", path, index++), warnings));
  }
  return commands;
}

EniCoeCmd readCoeCmd(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EniCoeCmd command;
  command.transitions = readTransitions(node, path, warnings);
  command.comment = childText(node, "Comment");
  command.timeoutMs = childNumber<std::uint32_t>(node, "Timeout", path, warnings).value_or(0);
  const auto ccs = childNumber<std::uint8_t>(node, "Ccs", path, warnings);
  if (ccs && (*ccs == 1 || *ccs == 2)) {
    command.ccs = static_cast<EniCoeCommandSpecifier>(*ccs);
  } else if (ccs) {
    warnings.add(path, std::format("<Ccs> {} is neither a download nor an upload", *ccs));
  }
  command.index = childNumber<std::uint16_t>(node, "Index", path, warnings).value_or(0);
  command.subindex = childNumber<std::uint8_t>(node, "SubIndex", path, warnings).value_or(0);
  command.data = childHex(node, "Data", path, warnings);
  command.disabled = childBool(node, "Disabled").value_or(false);
  return command;
}

EniMailboxWindow readMailboxWindow(const pugi::xml_node& node, std::string_view path,
                                   Warnings& warnings) {
  EniMailboxWindow window;
  window.start = childNumber<std::uint16_t>(node, "Start", path, warnings).value_or(0);
  window.length = childNumber<std::uint16_t>(node, "Length", path, warnings).value_or(0);
  window.pollTime = childNumber<std::uint32_t>(node, "PollTime", path, warnings);
  window.statusBitAddr = childNumber<std::uint32_t>(node, "StatusBitAddr", path, warnings);
  return window;
}

EniMailbox readMailbox(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EniMailbox mailbox;
  mailbox.send = readMailboxWindow(node.child("Send"), path, warnings);
  mailbox.recv = readMailboxWindow(node.child("Recv"), path, warnings);
  if (const pugi::xml_node bootstrap = node.child("BootStrap"); bootstrap) {
    mailbox.bootstrapSend = readMailboxWindow(bootstrap.child("Send"), path, warnings);
    mailbox.bootstrapRecv = readMailboxWindow(bootstrap.child("Recv"), path, warnings);
  }
  for (const pugi::xml_node protocol : node.children("Protocol")) {
    if (const auto found = lookUp(kMailboxProtocols, protocol.text().as_string()); found) {
      mailbox.protocols.push_back(*found);
    } else {
      warnings.add(path, std::format("unknown <Protocol> '{}'", protocol.text().as_string()));
    }
  }
  std::size_t index = 0;
  for (const pugi::xml_node cmd : node.child("CoE").child("InitCmds").children("InitCmd")) {
    mailbox.coeInitCmds.push_back(
        readCoeCmd(cmd, std::format("{}/CoE/InitCmd[{}]", path, index++), warnings));
  }
  for (const char* element : {"SoE", "AoE", "EoE", "FoE", "VoE"}) {
    if (hasChild(node, element)) {
      warnings.notModelled(path, element);
    }
  }
  if (hasChild(node.child("CoE"), "Profile")) {
    warnings.notModelled(path, "CoE/Profile");
  }
  return mailbox;
}

EniProcessData readProcessData(const pugi::xml_node& node, std::string_view path,
                               Warnings& warnings) {
  EniProcessData processData;
  const auto window = [&](const char* name) -> std::optional<EniProcessDataWindow> {
    const pugi::xml_node child = node.child(name);
    if (!child) {
      return std::nullopt;
    }
    return EniProcessDataWindow{
        .bitStart = childNumber<std::uint32_t>(child, "BitStart", path, warnings).value_or(0),
        .bitLength = childNumber<std::uint32_t>(child, "BitLength", path, warnings).value_or(0)};
  };
  processData.send = window("Send");
  processData.recv = window("Recv");

  for (std::uint8_t index = 0; index <= 15; ++index) {
    const std::string name = std::format("Sm{}", index);
    const pugi::xml_node child = node.child(name.c_str());
    if (!child) {
      continue;
    }
    EniSyncManager syncManager;
    syncManager.index = index;
    syncManager.type =
        lookUpOr(kSyncManagerTypes, childText(child, "Type"), EniSyncManagerType::MailboxOut,
                 "sync manager type", path, warnings);
    syncManager.startAddress =
        childNumber<std::uint16_t>(child, "StartAddress", path, warnings).value_or(0);
    syncManager.controlByte =
        childNumber<std::uint8_t>(child, "ControlByte", path, warnings).value_or(0);
    syncManager.enable = childBool(child, "Enable").value_or(false);
    syncManager.minSize = childNumber<std::uint32_t>(child, "MinSize", path, warnings);
    syncManager.maxSize = childNumber<std::uint32_t>(child, "MaxSize", path, warnings);
    syncManager.defaultSize = childNumber<std::uint32_t>(child, "DefaultSize", path, warnings);
    syncManager.watchdog = childNumber<std::uint32_t>(child, "Watchdog", path, warnings);
    processData.syncManagers.push_back(syncManager);
  }

  const auto readPdos = [&](const char* element) {
    std::vector<EniPdo> pdos;
    for (const pugi::xml_node child : node.children(element)) {
      EniPdo pdo;
      pdo.index = childNumber<std::uint16_t>(child, "Index", path, warnings).value_or(0);
      pdo.name = childText(child, "Name");
      if (const pugi::xml_attribute sm = child.attribute("Sm"); sm) {
        pdo.syncManager = static_cast<std::uint8_t>(sm.as_int());
      }
      pdo.fixed = child.attribute("Fixed").as_bool(false);
      pdo.mandatory = child.attribute("Mandatory").as_bool(false);
      for (const pugi::xml_node entryNode : child.children("Entry")) {
        EniPdoEntry entry;
        entry.index = childNumber<std::uint16_t>(entryNode, "Index", path, warnings).value_or(0);
        entry.subindex =
            childNumber<std::uint8_t>(entryNode, "SubIndex", path, warnings).value_or(0);
        entry.bitLen = childNumber<std::uint16_t>(entryNode, "BitLen", path, warnings).value_or(0);
        entry.name = childText(entryNode, "Name");
        entry.dataType = childText(entryNode, "DataType");
        entry.comment = childText(entryNode, "Comment");
        pdo.entries.push_back(std::move(entry));
      }
      pdos.push_back(std::move(pdo));
    }
    return pdos;
  };
  processData.rxPdos = readPdos("RxPdo");
  processData.txPdos = readPdos("TxPdo");
  return processData;
}

EniSlave readSlave(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EniSlave slave;
  const pugi::xml_node info = node.child("Info");
  slave.info.name = childText(info, "Name");
  slave.info.physAddr = childNumber<std::uint16_t>(info, "PhysAddr", path, warnings).value_or(0);
  slave.info.autoIncAddr =
      childNumber<std::uint16_t>(info, "AutoIncAddr", path, warnings).value_or(0);
  slave.info.physics = childText(info, "Physics");
  slave.info.vendorId = childNumber<std::uint32_t>(info, "VendorId", path, warnings).value_or(0);
  slave.info.productCode =
      childNumber<std::uint32_t>(info, "ProductCode", path, warnings).value_or(0);
  slave.info.revisionNo =
      childNumber<std::uint32_t>(info, "RevisionNo", path, warnings).value_or(0);
  slave.info.serialNo = childNumber<std::uint32_t>(info, "SerialNo", path, warnings).value_or(0);
  if (hasChild(info, "Identification")) {
    warnings.notModelled(path, "Info/Identification");
  }

  if (hasChild(node, "ProcessData")) {
    slave.processData = readProcessData(node.child("ProcessData"), path, warnings);
  }
  if (hasChild(node, "Mailbox")) {
    slave.mailbox = readMailbox(node.child("Mailbox"), path, warnings);
  }
  slave.initCmds = readEcatCmds(node, path, warnings);

  for (const pugi::xml_node previous : node.children("PreviousPort")) {
    EniPreviousPort previousPort;
    previousPort.port = lookUpOr(kPorts, previous.child("Port").text().as_string(), EniPort::B,
                                 "port", path, warnings);
    // The attribute defaults to "0" per the schema, so an absent one is not the selected port.
    const std::string_view selected = previous.attribute("Selected").as_string("0");
    previousPort.selected = selected == "1";
    previousPort.physAddr = childNumber<std::uint16_t>(previous, "PhysAddr", path, warnings);
    previousPort.deviceId = childNumber<std::uint32_t>(previous, "DeviceId", path, warnings);
    slave.previousPorts.push_back(previousPort);
  }

  if (const pugi::xml_node dc = node.child("DC"); dc) {
    EniDc value;
    value.potentialReferenceClock = childBool(dc, "PotentialReferenceClock");
    value.referenceClock = childBool(dc, "ReferenceClock");
    value.cycleTime0Ns = childSigned(dc, "CycleTime0", path, warnings);
    value.cycleTime1Ns = childSigned(dc, "CycleTime1", path, warnings);
    value.shiftTimeNs = childSigned(dc, "ShiftTime", path, warnings);
    slave.dc = value;
  }

  if (hasChild(node, "HotConnect")) {
    warnings.notModelled(path, "HotConnect");
  }
  return slave;
}

EniCyclic readCyclic(const pugi::xml_node& node, std::string_view path, Warnings& warnings) {
  EniCyclic cyclic;
  cyclic.comment = childText(node, "Comment");
  cyclic.cycleTimeUs = childNumber<std::uint32_t>(node, "CycleTime", path, warnings);
  cyclic.priority = childNumber<std::uint32_t>(node, "Priority", path, warnings);
  cyclic.taskId = childText(node, "TaskId");
  std::size_t frameIndex = 0;
  for (const pugi::xml_node frameNode : node.children("Frame")) {
    const std::string framePath = std::format("{}/Frame[{}]", path, frameIndex++);
    EniFrame frame;
    frame.comment = childText(frameNode, "Comment");
    std::size_t cmdIndex = 0;
    for (const pugi::xml_node cmdNode : frameNode.children("Cmd")) {
      const std::string cmdPath = std::format("{}/Cmd[{}]", framePath, cmdIndex++);
      EniCyclicCmd command;
      for (const pugi::xml_node state : cmdNode.children("State")) {
        command.states.push_back(
            lookUpOr(kStates, state.text().as_string(), EniState::Op, "state", cmdPath, warnings));
      }
      command.comment = childText(cmdNode, "Comment");
      const auto cmd = childNumber<std::uint8_t>(cmdNode, "Cmd", cmdPath, warnings);
      if (cmd && *cmd <= static_cast<std::uint8_t>(EniCmd::Frmw)) {
        command.cmd = static_cast<EniCmd>(*cmd);
      }
      command.adp = childNumber<std::uint16_t>(cmdNode, "Adp", cmdPath, warnings);
      command.ado = childNumber<std::uint16_t>(cmdNode, "Ado", cmdPath, warnings);
      command.addr = childNumber<std::uint32_t>(cmdNode, "Addr", cmdPath, warnings);
      command.data = childHex(cmdNode, "Data", cmdPath, warnings);
      command.dataLength = childNumber<std::uint32_t>(cmdNode, "DataLength", cmdPath, warnings);
      command.cnt = childNumber<std::uint32_t>(cmdNode, "Cnt", cmdPath, warnings);
      command.inputOffs =
          childNumber<std::uint32_t>(cmdNode, "InputOffs", cmdPath, warnings).value_or(0);
      command.outputOffs =
          childNumber<std::uint32_t>(cmdNode, "OutputOffs", cmdPath, warnings).value_or(0);
      if (hasChild(cmdNode, "CopyInfos")) {
        warnings.notModelled(cmdPath, "CopyInfos");
      }
      frame.cmds.push_back(std::move(command));
    }
    cyclic.frames.push_back(std::move(frame));
  }
  return cyclic;
}

EniProcessImageArea readProcessImageArea(const pugi::xml_node& node, std::string_view path,
                                         Warnings& warnings) {
  EniProcessImageArea area;
  area.byteSize = childNumber<std::uint32_t>(node, "ByteSize", path, warnings).value_or(0);
  for (const pugi::xml_node child : node.children("Variable")) {
    EniVariable variable;
    variable.name = childText(child, "Name");
    variable.comment = childText(child, "Comment");
    variable.dataType = childText(child, "DataType");
    variable.bitSize = childNumber<std::uint32_t>(child, "BitSize", path, warnings).value_or(0);
    variable.bitOffs = childNumber<std::uint32_t>(child, "BitOffs", path, warnings).value_or(0);
    area.variables.push_back(std::move(variable));
  }
  return area;
}

}  // namespace

std::expected<EniRead, std::string> readEni(std::string_view xml) {
  pugi::xml_document doc;
  const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
  if (!parsed) {
    return std::unexpected(std::format("the document will not parse: {} at offset {}",
                                       parsed.description(), parsed.offset));
  }
  const pugi::xml_node root = doc.child("EtherCATConfig");
  if (!root) {
    return std::unexpected("the root element is not <EtherCATConfig>");
  }
  const pugi::xml_node config = root.child("Config");
  if (!config) {
    return std::unexpected("<EtherCATConfig> has no <Config>");
  }

  Warnings warnings;
  EniRead result;

  const pugi::xml_node master = config.child("Master");
  const pugi::xml_node masterInfo = master.child("Info");
  result.network.master.name = childText(masterInfo, "Name");
  result.network.master.destination = childHex(masterInfo, "Destination", "Master", warnings);
  result.network.master.source = childHex(masterInfo, "Source", "Master", warnings);
  if (hasChild(masterInfo, "EtherType")) {
    // Little-endian like every other payload, so "a488" is 0x88A4.
    const auto bytes = childHex(masterInfo, "EtherType", "Master", warnings);
    if (bytes.size() == 2) {
      result.network.master.etherType = mm::core::fromBytes<std::uint16_t>(bytes);
    } else {
      warnings.add("Master", "<EtherType> is not two bytes");
    }
  }
  if (const pugi::xml_node states = master.child("MailboxStates"); states) {
    EniMailboxStates value;
    value.startAddr =
        childNumber<std::uint32_t>(states, "StartAddr", "Master", warnings).value_or(0);
    value.count = childNumber<std::uint32_t>(states, "Count", "Master", warnings).value_or(0);
    result.network.master.mailboxStates = value;
  }
  if (const pugi::xml_node eoe = master.child("EoE"); eoe) {
    EniEoe value;
    value.maxPorts = childNumber<std::uint32_t>(eoe, "MaxPorts", "Master", warnings).value_or(0);
    value.maxFrames = childNumber<std::uint32_t>(eoe, "MaxFrames", "Master", warnings).value_or(0);
    value.maxMacs = childNumber<std::uint32_t>(eoe, "MaxMACs", "Master", warnings).value_or(0);
    result.network.master.eoe = value;
  }
  result.network.master.initCmds = readEcatCmds(master, "Master", warnings);

  std::size_t index = 0;
  for (const pugi::xml_node node : config.children("Slave")) {
    result.network.slaves.push_back(readSlave(node, std::format("Slave[{}]", index++), warnings));
  }

  // The schema allows several cyclic tasks. The model holds one, because one is what a master
  // exchanging a single process image runs; a document with more keeps its first and says so.
  std::size_t cyclicCount = 0;
  for (const pugi::xml_node node : config.children("Cyclic")) {
    if (cyclicCount++ == 0) {
      result.network.cyclic = readCyclic(node, "Cyclic", warnings);
    }
  }
  if (cyclicCount > 1) {
    warnings.add("Cyclic", std::format("{} cyclic tasks; only the first is modelled", cyclicCount));
  }

  if (const pugi::xml_node image = config.child("ProcessImage"); image) {
    EniProcessImage processImage;
    if (hasChild(image, "Inputs")) {
      processImage.inputs =
          readProcessImageArea(image.child("Inputs"), "ProcessImage/Inputs", warnings);
    }
    if (hasChild(image, "Outputs")) {
      processImage.outputs =
          readProcessImageArea(image.child("Outputs"), "ProcessImage/Outputs", warnings);
    }
    result.network.processImage = processImage;
  }

  result.warnings = warnings.take();
  return result;
}

}  // namespace mm::etg
