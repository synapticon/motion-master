#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mm::etg {

/// @brief EtherCAT Network Information (ENI) — the configuration a master replays to start a bus.
///
/// An ESI file describes one device family; an ENI describes one assembled network, and it is what
/// a third-party master consumes. The two documents are not the same kind of thing. An ESI is
/// declarative, and a configuration tool reads it to decide what to do. An ENI is imperative:
/// every configuration step is written as an EtherCAT datagram (@c EniEcatCmd) or a CoE transfer
/// (@c EniCoeCmd), tagged with the AL-state transition it belongs to, and a master brings the bus
/// up by replaying them in order. So this library does not describe a network to a master. It
/// hands the master a script.
///
/// The model below mirrors the ENI XML Schema 1.7 element by element, in schema order, because
/// every ENI complex type is an @c xs:sequence — element order is part of the contract, and a
/// document with the right elements in the wrong order does not validate. Reference: **ETG.2100**
/// (EtherCAT Network Information Specification) is the prose authority, and @c EtherCATConfig.xsd
/// is the exact one.
///
/// Three conventions run through the format and are handled once, here:
///
///   - **@c xs:hexBinary** — raw bytes in EtherCAT wire order, hex-encoded. An ESC register value
///     is little-endian, so a write of 0x0002 to AL Control is @c "0200". Every payload is carried
///     as @c std::vector<uint8_t> in wire order and encoded once, by @c writeEni.
///   - **Optionality is the schema's, not a preference.** A field held in @c std::optional, or an
///     empty string or vector, is one the schema marks optional, and @c writeEni omits the element
///     rather than writing a zero. The difference matters: @c <Cnt>0</Cnt> tells a master to expect
///     a working counter of zero, which no successful datagram returns.
///   - **Direction is written from the master's side.** In @c EniProcessData, @c send is the
///     master's output image and @c recv is its input image (ETG.2100 Table 14). A device's
///     TxPDOs therefore land in @c recv.
///
/// @c writeEni is a pure transform with no fieldbus in it. A caller in @c mm::node fills the model
/// from a live bus, and this library never learns that a bus exists.

/// @brief An AL-state transition an init command runs on (ENI @c TransitionType).
///
/// Two letters, the state left and the state entered: @c IP is INIT to PRE-OP. The pairs with a
/// repeated letter are the states a command may be re-sent in without a transition.
enum class EniTransition : std::uint8_t {
  II,  ///< INIT to INIT.
  IP,  ///< INIT to PRE-OP.
  PP,  ///< PRE-OP to PRE-OP.
  PO,  ///< PRE-OP to OP.
  PS,  ///< PRE-OP to SAFE-OP.
  PI,  ///< PRE-OP to INIT.
  SS,  ///< SAFE-OP to SAFE-OP.
  SP,  ///< SAFE-OP to PRE-OP.
  SO,  ///< SAFE-OP to OP.
  SI,  ///< SAFE-OP to INIT.
  OS,  ///< OP to SAFE-OP.
  OP,  ///< OP to PRE-OP.
  OI,  ///< OP to INIT.
  IB,  ///< INIT to BOOT.
  BI,  ///< BOOT to INIT.
};

/// @brief EtherCAT command type of an init or cyclic command (ENI @c Cmd, ETG.2100 Table 21).
///
/// The addressing mode is part of the command: @c Aprd and @c Apwr address by position in the ring,
/// @c Fprd and @c Fpwr by configured station address, the @c B commands address every device at
/// once, and the @c L commands address logical memory. A read-write command performs its read
/// before its write.
enum class EniCmd : std::uint8_t {
  Nop = 0,    ///< No operation.
  Aprd = 1,   ///< Auto-increment physical read.
  Apwr = 2,   ///< Auto-increment physical write.
  Aprw = 3,   ///< Auto-increment physical read-write.
  Fprd = 4,   ///< Configured-address physical read.
  Fpwr = 5,   ///< Configured-address physical write.
  Fprw = 6,   ///< Configured-address physical read-write.
  Brd = 7,    ///< Broadcast read.
  Bwr = 8,    ///< Broadcast write.
  Brw = 9,    ///< Broadcast read-write.
  Lrd = 10,   ///< Logical-memory read.
  Lwr = 11,   ///< Logical-memory write.
  Lrw = 12,   ///< Logical-memory read-write.
  Armw = 13,  ///< Auto-increment physical read, multiple write.
  Frmw = 14,  ///< Configured-address physical read, multiple write.
};

/// @brief Whether an init command needs a frame or a cycle of its own (ENI @c <Requires>).
enum class EniRequires : std::uint8_t {
  None,   ///< No requirement; the element is not written and the command may share a frame.
  Frame,  ///< The command requires a separate frame.
  Cycle,  ///< The command requires a separate cycle.
};

/// @brief An AL state a cyclic command is sent in (ENI @c Cyclic/Frame/Cmd/State).
enum class EniState : std::uint8_t {
  Init,    ///< INIT.
  PreOp,   ///< PRE-OP.
  SafeOp,  ///< SAFE-OP.
  Op,      ///< OP.
};

/// @brief What a Sync Manager carries (ENI @c SyncManagerSettings/Type).
enum class EniSyncManagerType : std::uint8_t {
  MailboxOut,  ///< Master-to-device mailbox.
  MailboxIn,   ///< Device-to-master mailbox.
  Outputs,     ///< Master-to-device process data.
  Inputs,      ///< Device-to-master process data.
};

/// @brief A mailbox protocol a device supports (ENI @c Mailbox/Protocol).
enum class EniMailboxProtocol : std::uint8_t {
  Aoe,  ///< ADS over EtherCAT.
  Eoe,  ///< Ethernet over EtherCAT.
  Coe,  ///< CANopen over EtherCAT.
  Soe,  ///< Servo Profile over EtherCAT.
  Foe,  ///< File Access over EtherCAT.
  Voe,  ///< Vendor-specific over EtherCAT.
};

/// @brief The CoE command specifier of a mailbox init command (ENI @c Ccs).
///
/// The values are CoE's own, from ETG.1000.6: 1 initiates a download, which writes to the device.
/// **ETG.2100 Table 20 states the opposite** — it labels 1 as an upload and 2 as a download. The
/// coding here follows ETG.1000.6, which owns the CoE protocol, and the ENI files ETG ships as
/// samples agree with it: each one carries a payload under a @c Ccs of 1, and an upload request
/// has no payload to carry.
enum class EniCoeCommandSpecifier : std::uint8_t {
  Download = 1,  ///< SDO initiate download: the master writes @c data to the object.
  Upload = 2,    ///< SDO initiate upload: the master reads the object.
};

/// @brief The value a read init command must return before the master goes on (ENI @c Validate).
///
/// A master re-sends the command until the read data matches, or until @c timeoutMs runs out. This
/// is how an ENI expresses "wait for the device to reach PRE-OP" without a wait primitive.
struct EniValidate {
  std::vector<std::uint8_t> data;      ///< The value the read must return.
  std::vector<std::uint8_t> dataMask;  ///< Bits of @c data that must match; empty compares all.
  std::uint32_t timeoutMs = 0;         ///< How long to keep re-sending.
};

/// @brief One EtherCAT datagram the master sends at a transition (ENI @c ECatCmdType).
///
/// The address is one of two shapes, and exactly one must be filled: @c ado with an optional
/// @c adp for the physical commands, or @c addr for the logical ones. The payload is the same
/// choice: @c data for a write, or @c dataLength alone for a read, which says how many bytes to
/// make room for. @c writeEni rejects a command that fills both sides of either choice.
struct EniEcatCmd {
  std::vector<EniTransition> transitions;  ///< Transitions to send this command at.
  bool beforeSlave = false;                ///< Send before the addressed device's own commands.
  std::string comment;                     ///< Free text a master may log. Empty is not written.
  EniRequires requirement = EniRequires::None;  ///< Frame or cycle isolation.
  EniCmd cmd = EniCmd::Nop;           ///< The datagram type, which also fixes the addressing mode.
  std::optional<std::uint16_t> adp;   ///< Device address, with @c ado. Excludes @c addr.
  std::optional<std::uint16_t> ado;   ///< Offset in the device's ESC memory. Excludes @c addr.
  std::optional<std::uint32_t> addr;  ///< Logical start address. Excludes @c adp and @c ado.
  std::vector<std::uint8_t> data;     ///< Bytes to write. Excludes @c dataLength.
  std::optional<std::uint32_t> dataLength;  ///< Bytes to read. Excludes @c data.
  std::optional<std::uint32_t> cnt;         ///< Working counter a successful datagram returns.
  std::optional<std::uint32_t> retries;     ///< Re-sends allowed before the command fails.
  std::optional<EniValidate> validate;      ///< Retry-until condition. Excludes @c timeoutMs.
  std::optional<std::uint32_t> timeoutMs;   ///< Plain timeout. Excludes @c validate.
};

/// @brief One CoE transfer the master runs at a transition (ENI @c CoE/InitCmds/InitCmd).
///
/// This is where a device's own configuration goes: the PDO assignment objects, and any vendor
/// object a device needs written before it will leave PRE-OP.
struct EniCoeCmd {
  std::vector<EniTransition> transitions;  ///< At least one; the schema requires it.
  std::string comment;                     ///< Free text a master may log.
  std::uint32_t timeoutMs = 0;             ///< Timeout for this transfer.
  EniCoeCommandSpecifier ccs = EniCoeCommandSpecifier::Download;  ///< Read or write.
  std::uint16_t index = 0;                                        ///< Object index.
  std::uint8_t subindex = 0;                                      ///< Object subindex.
  std::vector<std::uint8_t> data;  ///< Payload of a download; empty for an upload.
  bool disabled = false;           ///< Written to the file but not to be sent.
};

/// @brief Where one device's process data sits in the master's image (ENI @c ProcessData/Send
///        or @c ProcessData/Recv).
struct EniProcessDataWindow {
  std::uint32_t bitStart = 0;   ///< Offset of the window within the image, in bits.
  std::uint32_t bitLength = 0;  ///< Length of the window, in bits.
};

/// @brief One Sync Manager of a device (ENI @c SyncManagerSettings).
///
/// @c index is not written as text. It selects which of the schema's fixed @c Sm0 to @c Sm15
/// elements carries this Sync Manager, so two entries may not share one.
struct EniSyncManager {
  std::uint8_t index = 0;                                    ///< Sync Manager number, 0 to 15.
  EniSyncManagerType type = EniSyncManagerType::MailboxOut;  ///< What it carries.
  std::uint16_t startAddress = 0;  ///< Start of the guarded window in the device's ESC memory.
  std::uint8_t controlByte = 0;    ///< SM control register: buffer mode, direction, watchdog.
  bool enable = false;             ///< Whether the master enables it.
  std::optional<std::uint32_t> minSize;      ///< Smallest window the device accepts, in bytes.
  std::optional<std::uint32_t> maxSize;      ///< Largest window the device accepts, in bytes.
  std::optional<std::uint32_t> defaultSize;  ///< Window size before any PDO re-assignment.
  std::optional<std::uint32_t> watchdog;     ///< Process-data watchdog time.
};

/// @brief A device's process-data description (ENI @c Slave/ProcessData).
struct EniProcessData {
  std::optional<EniProcessDataWindow> send;  ///< Window in the master's *output* image.
  std::optional<EniProcessDataWindow> recv;  ///< Window in the master's *input* image.
  std::vector<EniSyncManager> syncManagers;  ///< Sync Managers, in any order; @c index places them.
};

/// @brief One mailbox window of a device (ENI @c MailboxSendInfoType / @c MailboxRecvInfoType).
struct EniMailboxWindow {
  std::uint16_t start = 0;                     ///< Offset in the device's ESC memory.
  std::uint16_t length = 0;                    ///< Window length in bytes.
  std::optional<std::uint32_t> pollTime;       ///< How often the master polls for a reply.
  std::optional<std::uint32_t> statusBitAddr;  ///< Mailbox-status bit the master may poll instead.
};

/// @brief A device's mailbox configuration (ENI @c Slave/Mailbox).
///
/// @c bootstrapSend and @c bootstrapRecv are the windows the device uses in BOOT, where a firmware
/// download runs. They are usually at different offsets from the standard pair and must both be
/// present or both absent.
struct EniMailbox {
  EniMailboxWindow send;                          ///< Master-to-device window.
  EniMailboxWindow recv;                          ///< Device-to-master window.
  std::optional<EniMailboxWindow> bootstrapSend;  ///< Master-to-device window in BOOT.
  std::optional<EniMailboxWindow> bootstrapRecv;  ///< Device-to-master window in BOOT.
  std::vector<EniMailboxProtocol> protocols;      ///< Protocols the device supports.
  std::vector<EniCoeCmd> coeInitCmds;             ///< CoE transfers, under @c Mailbox/CoE.
};

/// @brief A device's identity (ENI @c Slave/Info).
struct EniSlaveInfo {
  std::string name;               ///< Device name, for a human reading the file.
  std::uint16_t physAddr = 0;     ///< Configured station address the master assigns.
  std::uint16_t autoIncAddr = 0;  ///< Auto-increment address: 0 for the first device, then -1,
                                  ///< -2 and so on as a 16-bit value.
  std::string physics;            ///< One character per port; see @c eniPhysics.
  std::uint32_t vendorId = 0;     ///< Vendor ID from the device's SII.
  std::uint32_t productCode = 0;  ///< Product code from the device's SII.
  std::uint32_t revisionNo = 0;   ///< Revision number from the device's SII.
  std::uint32_t serialNo = 0;     ///< Serial number from the device's SII.
};

/// @brief A port of the device upstream of this one (ENI @c Slave/PreviousPort @c Port).
///
/// The letters are the ENI's own spelling of port numbers 0 to 3. **@c A is spec-legal and
/// schema-invalid**: ETG.2100 Table 29 allows all four, and ENI Schema 1.7 enumerates only @c B,
/// @c C and @c D. @c writeEni therefore refuses @c A, while a reader should accept it — reading a
/// document somebody else wrote is the case that needs the tolerance.
enum class EniPort : std::uint8_t {
  A,  ///< Port 0. Rejected by the schema; see above.
  B,  ///< Port 1.
  C,  ///< Port 2.
  D,  ///< Port 3.
};

/// @brief Where this device sits in the ring (ENI @c Slave/PreviousPort).
///
/// Names the port of the upstream device that this one is plugged into, which is how a master
/// learns the physical layout and not only the logical order. A device may carry several: the one
/// it is actually connected to has @c selected set, and any other port it could be moved to without
/// changing the order of the devices may be listed alongside with @c selected clear.
///
/// The element is absent for the first device on the bus, which has no previous device.
struct EniPreviousPort {
  EniPort port = EniPort::B;              ///< The upstream device's port.
  bool selected = false;                  ///< This is the connection, not merely a possible one.
  std::optional<std::uint16_t> physAddr;  ///< Station address of the upstream device.
  std::optional<std::uint32_t> deviceId;  ///< Deprecated by the schema; kept so a read document
                                          ///< survives being written back out.
};

/// @brief A device's distributed-clock configuration (ENI @c Slave/DC).
///
/// Present only for a device the master synchronises. Motion Master runs the bus in free-run and
/// writes no @c DC element, so today this is what a *reader* finds in somebody else's document.
///
/// @c cycleTime1Ns is not the SYNC1 cycle time. ETG.2100 Table 32 defines it as
/// `SYNC1 cycle − SYNC0 cycle + SYNC0 shift`, a derived value, so it can be neither read nor
/// written as if it were the raw register figure.
struct EniDc {
  std::optional<bool> potentialReferenceClock;  ///< The device has the registers to be one.
  std::optional<bool> referenceClock;           ///< The device *is* the reference clock.
  std::optional<std::int32_t> cycleTime0Ns;     ///< SYNC0 cycle time, in nanoseconds.
  std::optional<std::int32_t> cycleTime1Ns;     ///< The derived SYNC1 figure above, in nanoseconds.
  std::optional<std::int32_t> shiftTimeNs;      ///< SYNC0 shift time, in nanoseconds.
};

/// @brief One device on the bus (ENI @c Config/Slave).
struct EniSlave {
  EniSlaveInfo info;                           ///< Identity and address.
  std::optional<EniProcessData> processData;   ///< Absent for a device with no process data.
  std::optional<EniMailbox> mailbox;           ///< Absent for a device with no mailbox.
  std::vector<EniEcatCmd> initCmds;            ///< Datagrams, under @c Slave/InitCmds.
  std::vector<EniPreviousPort> previousPorts;  ///< Where the device sits in the ring.
  std::optional<EniDc> dc;                     ///< Distributed clocks; absent means free-run.
};

/// @brief One datagram of a cyclic frame (ENI @c Cyclic/Frame/Cmd).
///
/// @c inputOffs and @c outputOffs are both required by the schema, so a frame that only reads still
/// writes an output offset of zero.
struct EniCyclicCmd {
  std::vector<EniState> states;  ///< AL states to send this frame in; at least one, at most four.
  std::string comment;           ///< Free text a master may log.
  EniCmd cmd = EniCmd::Lrw;      ///< Usually @c Lrw: one datagram exchanges the whole image.
  std::optional<std::uint16_t> adp;   ///< Device address, with @c ado. Excludes @c addr.
  std::optional<std::uint16_t> ado;   ///< Offset in the device's ESC memory. Excludes @c addr.
  std::optional<std::uint32_t> addr;  ///< Logical start address. Excludes @c adp and @c ado.
  std::vector<std::uint8_t> data;     ///< Bytes to write. Excludes @c dataLength.
  std::optional<std::uint32_t> dataLength;  ///< Bytes to exchange. Excludes @c data.
  std::optional<std::uint32_t> cnt;         ///< Working counter a healthy bus returns.
  std::uint32_t inputOffs = 0;              ///< Where the read data lands in the input image.
  std::uint32_t outputOffs = 0;             ///< Where the written data comes from in the output
                                            ///< image.
};

/// @brief One cyclic frame (ENI @c Cyclic/Frame).
struct EniFrame {
  std::string comment;             ///< Free text a master may log.
  std::vector<EniCyclicCmd> cmds;  ///< At least one; the schema requires it.
};

/// @brief The cyclic task that exchanges process data (ENI @c Config/Cyclic).
struct EniCyclic {
  std::string comment;                       ///< Free text a master may log.
  std::optional<std::uint32_t> cycleTimeUs;  ///< Cycle period in microseconds.
  std::optional<std::uint32_t> priority;     ///< Task priority, as the master reads it.
  std::string taskId;                        ///< Task name, as the master reads it.
  std::vector<EniFrame> frames;              ///< At least one; the schema requires it.
};

/// @brief One named value in the master's process image (ENI @c ProcessImage @c Variable).
struct EniVariable {
  std::string name;           ///< Variable name; a master shows it to an operator.
  std::string comment;        ///< Free text.
  std::string dataType;       ///< Type name, as an ESI writes it: @c "INT", @c "UDINT", @c "BOOL".
  std::uint32_t bitSize = 0;  ///< Length in bits.
  std::uint32_t bitOffs = 0;  ///< Offset within its half of the image, in bits.
};

/// @brief One half of the master's process image (ENI @c ProcessImage/Inputs or @c /Outputs).
struct EniProcessImageArea {
  std::uint32_t byteSize = 0;          ///< Total size of this half, in bytes.
  std::vector<EniVariable> variables;  ///< Named values in it; may be empty.
};

/// @brief The master's process image (ENI @c Config/ProcessImage).
struct EniProcessImage {
  std::optional<EniProcessImageArea> inputs;   ///< Device-to-master half.
  std::optional<EniProcessImageArea> outputs;  ///< Master-to-device half.
};

/// @brief The master itself (ENI @c Config/Master).
///
/// @c destination and @c source are the Ethernet MAC addresses of the cyclic frames, so both are
/// six bytes. A master that fills in its own source address still needs the element present.
struct EniMaster {
  std::string name;                        ///< Master name, for a human reading the file.
  std::vector<std::uint8_t> destination;   ///< Destination MAC, six bytes.
  std::vector<std::uint8_t> source;        ///< Source MAC, six bytes.
  std::optional<std::uint16_t> etherType;  ///< EtherType; unset means the EtherCAT default.
                                           ///< Written little-endian, so 0x88A4 reads @c "A488".
  std::vector<EniEcatCmd> initCmds;        ///< Bus-wide datagrams, under @c Master/InitCmds.
};

/// @brief A complete network configuration (ENI @c EtherCATConfig/Config).
struct EniNetwork {
  EniMaster master;                             ///< The master and its bus-wide init commands.
  std::vector<EniSlave> slaves;                 ///< Devices in bus order.
  std::optional<EniCyclic> cyclic;              ///< The process-data task.
  std::optional<EniProcessImage> processImage;  ///< The image the cyclic task exchanges.
};

/// @brief Renders the ENI @c Physics string for a device from its SII physical-port word.
///
/// The word holds one nibble per port, low nibble first, coded by ETG.2010 §Physical Port: 0 not
/// used, 1 MII, 3 EBUS, 4 Fast Hot Connect. The ENI writes one character per port instead — @c 'Y'
/// for MII, @c 'K' for EBUS, @c 'B' for Fast Hot Connect and a space for a port that is present but
/// none of these. Unused trailing ports are dropped, so a two-port device reads @c "YY".
///
/// @param physicalPort The SII physical-port word (EEPROM address 0x0010).
/// @return The @c Physics string, empty when no port is in use.
std::string eniPhysics(std::uint16_t physicalPort);

/// @brief Renders a network as an ENI document.
///
/// The output is indented UTF-8 XML with an XML declaration, and it validates against ENI Schema
/// 1.7. Byte payloads are written as uppercase @c xs:hexBinary, which is the canonical form; the
/// sample files ETG ships use lowercase, and a master must accept either.
///
/// @param network The network to render.
/// @return The document, or an error naming the first field that cannot be written — a command
///         that fills both halves of an either-or choice, a MAC that is not six bytes, or a Sync
///         Manager index that is out of range or used twice.
std::expected<std::string, std::string> writeEni(const EniNetwork& network);

}  // namespace mm::etg
