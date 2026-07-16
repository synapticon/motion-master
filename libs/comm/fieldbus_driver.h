#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mm::comm {

/// @brief Maximum size in bytes of the combined process-data image (all outputs + all inputs).
///
/// Sizes the driver's IOmap buffer and bounds the process-data snapshots layered on top
/// of it.  At 32 fully-loaded SOMANET axes (~160 bytes per direction) the image is ~10 KB;
/// 32 KB leaves ample headroom.  A bus whose mapped image exceeds this is rejected by
/// @c configureProcessData().  This is not a practical limit: on 100 Mbit EtherCAT an image
/// that large already forces a many-millisecond cycle, so the cap sits well clear of any
/// realistic configuration.
inline constexpr std::size_t kMaxProcessImageBytes = 32768;

/// @brief EtherCAT Application Layer state values.
///
/// Numeric values match the EtherCAT standard AL control/status register
/// encoding (ETG.1000.6 §6.4.1).
enum class EtherCatState : uint16_t {
  Init = 0x01,    ///< INIT — reset state, no communication.
  PreOp = 0x02,   ///< PRE-OPERATIONAL — mailbox communication active.
  Boot = 0x03,    ///< BOOT — firmware download mode.
  SafeOp = 0x04,  ///< SAFE-OPERATIONAL — inputs only; outputs ignored.
  Op = 0x08,      ///< OPERATIONAL — full PDO exchange.
};

/// @brief Extracts the AL state (bits 3:0) from a raw AL Status register value (0x0130).
///
/// The single place the AL Status register layout is decoded: the low nibble is the current
/// state, bit 4 is the error indicator (see @c alHasError). Callers compare the result against
/// @c EtherCatState rather than re-masking the raw register.
inline EtherCatState alState(uint16_t alStatus) {
  return static_cast<EtherCatState>(alStatus & 0x000Fu);
}

/// @brief Returns whether the AL Status error indicator (bit 4 of register 0x0130) is set.
inline bool alHasError(uint16_t alStatus) { return (alStatus & 0x0010u) != 0; }

/// @brief Human-readable name of an AL state, for log and error messages.
inline std::string_view toString(EtherCatState state) {
  switch (state) {
    case EtherCatState::Init:
      return "INIT";
    case EtherCatState::PreOp:
      return "PRE-OP";
    case EtherCatState::Boot:
      return "BOOT";
    case EtherCatState::SafeOp:
      return "SAFE-OP";
    case EtherCatState::Op:
      return "OP";
  }
  return "UNKNOWN";
}

/// @brief Working-counter contribution of one slave for a given AL state and PDO presence.
///
/// EtherCAT increments the working counter per successful datagram access: a slave exchanging
/// outputs contributes 2 (the process-data LRW both reads and writes its output region) and
/// inputs contribute 1.  Process data flows only in SAFE-OP (inputs only) and OP (outputs +
/// inputs); below that a slave does not contribute.  Summed across the bus this is the WKC a
/// (possibly partially operational) bus is expected to produce — the figure a health check
/// compares the live working counter against.  The protocol rule lives here, with the other AL
/// vocabulary, rather than in the transport-agnostic node layer.
///
/// @param state       Current AL state of the slave.
/// @param hasOutputs  Whether the slave has any mapped output (RxPDO) bits.
/// @param hasInputs   Whether the slave has any mapped input (TxPDO) bits.
inline int workingCounterContribution(EtherCatState state, bool hasOutputs, bool hasInputs) {
  switch (state) {
    case EtherCatState::Op:
      return (hasOutputs ? 2 : 0) + (hasInputs ? 1 : 0);
    case EtherCatState::SafeOp:
      return hasInputs ? 1 : 0;
    default:
      return 0;
  }
}

/// @brief Immutable identity fields read from a slave's EEPROM during configuration.
struct SlaveInfo {
  std::string name;             ///< Human-readable name from SII.
  uint32_t vendorId = 0;        ///< Vendor ID (EEprom manufacturer field).
  uint32_t productCode = 0;     ///< Product code (EEprom ID field).
  uint32_t revisionNumber = 0;  ///< Revision number.
  uint32_t serialNumber = 0;    ///< Serial number.
};

/// @brief Schema of a single object dictionary entry uploaded from a slave.
///
/// Populated by @c FieldbusDriver::readObjectDictionary, one entry per @c (index,
/// subindex) pair. Holds the immutable description only — no current value.
///
/// @c unit / @c defaultValue / @c minValue / @c maxValue carry the raw bytes for
/// the optional metadata when a source populates them; the raw byte form is
/// intentional — decoding into a typed variant is done at the @c node layer where
/// @c dataType is interpreted. The SOEM driver currently leaves them empty: it
/// reads entries with the basic-info "Get Entry Description" request only, because
/// SOMANET firmware echoes the extended ValueInfo bits without the matching
/// payload, which corrupts the trailing name. If these are ever needed, source
/// them from the object-dictionary description metadata rather than this service.
struct OdEntry {
  uint16_t index = 0;            ///< CoE object index.
  uint8_t subindex = 0;          ///< CoE object subindex.
  uint16_t objectCode = 0;       ///< OTYPE_VAR / OTYPE_ARRAY / OTYPE_RECORD (ETG.1000.6 §5).
  uint16_t dataType = 0;         ///< ETG.1020 data type code (e.g. 0x0007 = UNSIGNED32).
  uint16_t bitLength = 0;        ///< Bit length of the entry.
  uint16_t access = 0;           ///< ObjAccess bitfield (read/write per AL state).
  std::string name;              ///< Textual description.
  std::optional<uint32_t> unit;  ///< ETG.1004 unit code, if available.
  std::optional<std::vector<uint8_t>> defaultValue;  ///< Raw default-value bytes, if available.
  std::optional<std::vector<uint8_t>> minValue;      ///< Raw minimum-value bytes, if available.
  std::optional<std::vector<uint8_t>> maxValue;      ///< Raw maximum-value bytes, if available.
};

/// @brief One slave's input and output windows within the process-data images.
///
/// @c outputOffset is relative to the start of the output image; @c inputOffset is relative
/// to the start of the input image (the two images are exchanged separately — see
/// @c exchangeProcessData).  A direction with fewer than 8 mapped bits reports a byte count
/// of 0 and an offset of 0.
struct SlaveIo {
  uint16_t slavePosition = 0;  ///< 1-based bus position.
  uint32_t outputOffset = 0;   ///< Byte offset of this slave's outputs within the output image.
  uint32_t outputBytes = 0;    ///< Output byte count for this slave.
  uint32_t inputOffset = 0;    ///< Byte offset of this slave's inputs within the input image.
  uint32_t inputBytes = 0;     ///< Input byte count for this slave.
};

/// @brief Layout of the process-data images, produced by @c configureProcessData.
///
/// Transport-independent description of how mapped objects are placed within the flat
/// output and input images the caller exchanges each cycle.  The output image (master→slave)
/// is @c outputBytes long and the input image (slave→master) is @c inputBytes long; each
/// slave's window within them is given by @c slaves.  The node layer combines these per-slave
/// windows with the per-object bit offsets it reads from the PDO assignment to locate
/// individual values.  The driver exposes no caller-visible memory — the images themselves
/// are owned by the caller and passed to @c exchangeProcessData, so the abstraction holds
/// equally for a memory-mapped master (SOEM, IgH) and a message-based transport (SPoE).
struct PdoLayout {
  uint32_t outputBytes = 0;     ///< Size of the output image (master→slave).
  uint32_t inputBytes = 0;      ///< Size of the input image (slave→master).
  std::vector<SlaveIo> slaves;  ///< Per-slave windows, in bus order.
  int expectedWkc = 0;  ///< Working counter when every slave exchanges (outputs×2 + inputs).
};

/// @brief One Sync Manager's configuration as programmed on a slave's ESC.
///
/// A Sync Manager guards a window of the slave's physical memory and governs how the master
/// and the slave's application exchange it (mailbox handshake or buffered process data).
struct SyncManagerConfig {
  uint8_t index = 0;           ///< SM number (0..7).
  uint16_t physicalStart = 0;  ///< Physical ESC memory start address the SM guards.
  uint16_t length = 0;         ///< Length of the guarded window in bytes.
  uint32_t flags = 0;  ///< Raw SM control/flags register (buffer mode, direction, watchdog).
  uint8_t type = 0;    ///< 0=unused, 1=MbxOut, 2=MbxIn, 3=Outputs, 4=Inputs.
};

/// @brief One FMMU's configuration as programmed on a slave's ESC.
///
/// A Fieldbus Memory Management Unit maps a span of the bus-wide logical address space onto a
/// window of the slave's physical memory (typically the process-data Sync Manager), translating
/// the master's logical reads/writes into local accesses.
struct FmmuConfig {
  uint8_t index = 0;             ///< FMMU number (0..3).
  uint32_t logicalStart = 0;     ///< Logical (bus-wide) start address.
  uint16_t length = 0;           ///< Mapped length in bytes.
  uint8_t logicalStartBit = 0;   ///< Start bit within the first logical byte.
  uint8_t logicalEndBit = 0;     ///< End bit within the last logical byte.
  uint16_t physicalStart = 0;    ///< Physical ESC start address (ties the FMMU to a Sync Manager).
  uint8_t physicalStartBit = 0;  ///< Start bit within the first physical byte.
  uint8_t type = 0;              ///< ESC FMMU type: 1=read (inputs/TxPDO), 2=write (outputs/RxPDO).
  uint8_t active = 0;            ///< Non-zero when the FMMU is active.
};

/// @brief Mailbox configuration for a slave (the CoE/FoE/EoE/SoE transport windows).
struct MailboxConfig {
  uint16_t writeLength = 0;  ///< Write (master→slave) mailbox length in bytes; 0 if no mailbox.
  uint16_t writeOffset = 0;  ///< Write mailbox physical ESC offset.
  uint16_t readLength = 0;   ///< Read (slave→master) mailbox length in bytes.
  uint16_t readOffset = 0;   ///< Read mailbox physical ESC offset.
  uint16_t protocols = 0;    ///< Supported-protocol bits: 0x01 AoE, 0x02 EoE, 0x04 CoE, 0x08 FoE,
                             ///< 0x10 SoE, 0x20 VoE.
  // Per-protocol capability detail bytes as advertised in EEPROM (ECT_*DETAILS), refining the
  // supported-protocol bits above. Raw bytes — the meaning of each bit is decoded by the client
  // (CoE has a rich flag set; FoE/EoE are essentially an enable bit; SoE is a channel count). An
  // advertisement, not a guarantee (see the CoE Complete-Access note in the parameter path).
  uint8_t coeDetails = 0;  ///< CoE details (ECT_COEDET_*: SDO/Info/PDO-Assign/Config/Upload/CA).
  uint8_t foeDetails = 0;  ///< FoE details (bit 0 = enabled).
  uint8_t eoeDetails = 0;  ///< EoE details (bit 0 = enabled).
  uint8_t soeDetails = 0;  ///< SoE details / channel count.
};

/// @brief Distributed-clock configuration for a slave.
///
/// Populated by @c configureProcessData (which runs @c ecx_configdc). @c active is false for
/// the SM-synchronous / free-run bring-up this driver uses — DC is measured but no SYNC0 pulse
/// is generated.
struct DcConfig {
  bool capable = false;          ///< Slave has distributed-clock hardware.
  bool active = false;           ///< SYNC0 generation enabled.
  int32_t propagationDelay = 0;  ///< Measured propagation delay (ns).
  int32_t cycleTime = 0;         ///< DC cycle time (ns).
  int32_t shift = 0;             ///< Shift from the cycle-modulus boundary (ns).
};

/// @brief Static ESC configuration snapshot for one slave, captured by @c configureProcessData.
///
/// Describes what the master programmed into the slave's EtherCAT Slave Controller: its address,
/// process-data sizes, mailbox, distributed clock, and active Sync Managers and FMMUs. This is an
/// EtherCAT-ESC concept; transports without an ESC (e.g. SPoE) report no slaves (see
/// @c FieldbusDriver::busConfig).
struct SlaveConfig {
  uint16_t slavePosition = 0;      ///< 1-based bus position.
  uint16_t configuredAddress = 0;  ///< Station (configured) address assigned during scan.
  uint16_t aliasAddress = 0;       ///< Configured station alias from EEPROM.
  uint16_t outputBits = 0;         ///< Mapped output (master→slave) bits.
  uint16_t inputBits = 0;          ///< Mapped input (slave→master) bits.
  MailboxConfig mailbox;           ///< Mailbox transport windows + advertised protocol details.
  DcConfig dc;                     ///< Distributed-clock configuration.
  std::vector<SyncManagerConfig> syncManagers;  ///< Configured Sync Managers, by index.
  std::vector<FmmuConfig> fmmus;                ///< Configured FMMUs, by index.
};

/// @brief Per-port link state and error counters decoded from a slave's ESC.
///
/// Each EtherCAT Slave Controller has up to four physical ports; the counters are 8-bit and
/// saturate at 255 (they do not wrap) and are cleared only by a power cycle or an explicit
/// write — so callers interpret them as a monotonic "errors since reset" figure and watch for
/// a rising delta rather than an absolute value. Link state is decoded from DL Status (0x0110);
/// the counters come from the error-counter block (0x0300–0x0313).
struct PortDiagnostics {
  bool linkUp = false;      ///< Physical link detected on this port (DL Status link bit).
  bool loopClosed = false;  ///< Loop closed on this port (no downstream slave, or port disabled).
  bool communication = false;  ///< Stable communication established on this port (DL Status).
  uint8_t invalidFrame = 0;  ///< Invalid-frame counter: frames with a bad FCS/structure (0x0300+).
  uint8_t rxError = 0;       ///< Physical-layer RX error counter: RX_ER from the PHY (0x0301+).
  uint8_t forwardedError = 0;  ///< Forwarded RX error counter: errors flagged by an upstream ESC
                               ///< (0x0308+). Pinpoints the segment where corruption began.
  uint8_t lostLink = 0;        ///< Lost-link counter: link-down events on this port (0x0310+).
};

/// @brief Decoded health diagnostics for one slave, read live from its ESC registers.
///
/// A point-in-time snapshot of the link-quality and watchdog counters that surface a degrading
/// bus before it drops out of OP. Produced by @c FieldbusDriver::readDiagnostics via FPRD reads
/// (unlike @c busConfig, which is a cached snapshot — these are live). The per-port counters
/// localise a fault to a specific cable/connector; the watchdog counters distinguish a slave
/// that stopped receiving process data from a master-side problem.
struct SlaveDiagnostics {
  uint16_t slavePosition = 0;            ///< 1-based bus position.
  std::array<PortDiagnostics, 4> ports;  ///< Per-port link state and error counters (ports 0–3).
  uint8_t processingUnitError = 0;       ///< ECAT processing-unit error counter (0x030C): datagrams
                                         ///< that reached the processing unit malformed.
  uint8_t pdiError = 0;             ///< PDI error counter (0x030D): problems on the slave-local
                                    ///< process-data interface.
  uint8_t processDataWatchdog = 0;  ///< Process-data (SM) watchdog expirations (0x0442): the
                                    ///< slave stopped seeing fresh outputs.
  uint8_t pdiWatchdog = 0;          ///< PDI watchdog expirations (0x0443).
};

/// @brief Live distributed-clock synchronisation status for one slave, read from its ESC.
///
/// A point-in-time snapshot of how tightly each slave's distributed-clock unit is tracking the
/// bus reference clock. Produced by @c FieldbusDriver::readDcSync via FPRD reads of the DC
/// registers (system-time delay 0x0928 and system-time difference 0x092C) — live, not cached.
///
/// The reference clock (the first DC-capable slave) defines bus time; every other DC slave
/// continuously corrects its local clock toward it. @c systemTimeDifference is the live
/// deviation: it converges toward zero once the bus has been exchanging process data long
/// enough for the slaves' drift-compensation loops to settle, and a value that stays large or
/// grows means a slave is not locked to the reference. The figure is only meaningful while the
/// bus is exchanging in SAFE-OP/OP, since the master distributes the reference time in the
/// cyclic frame. Non-DC slaves report @c dcCapable false and zeroed values.
struct DcSyncDiagnostics {
  uint16_t slavePosition = 0;    ///< 1-based bus position.
  bool dcCapable = false;        ///< Slave has distributed-clock hardware (cached @c hasdc).
  bool referenceClock = false;   ///< This slave is the DC reference clock (first DC-capable).
  int32_t propagationDelay = 0;  ///< System-time delay / propagation delay (0x0928), ns.
  int32_t systemTimeDifference =
      0;  ///< Signed deviation of the local system time from the reference
          ///< (0x092C), ns. Positive = local clock ahead of the reference,
          ///< negative = behind; zero on the reference clock itself.
};

/// @brief Process-data (sync-manager) watchdog configuration of one slave.
///
/// Decoded from the watchdog divider (0x0400) and process-data watchdog time (0x0420) ESC
/// registers. The divider is the common time base for both the process-data and PDI watchdogs;
/// the time register scales it. A zero time register disables the watchdog. Unlike
/// @c SlaveDiagnostics::processDataWatchdog (an expiration counter), this is the configured
/// timeout itself. Produced/consumed by @c FieldbusDriver::processDataWatchdog and
/// @c setProcessDataWatchdog.
struct ProcessDataWatchdogConfig {
  bool enabled = false;                 ///< False when the time register (0x0420) is zero.
  bool running = false;                 ///< Live status (0x0440 bit 0): true = counting, false =
                                        ///< expired or disabled. Meaningful only while @c enabled.
  std::chrono::nanoseconds timeout{0};  ///< Decoded timeout: ticks × 40 ns × (divider + 2).
  uint16_t divider = 0;                 ///< Raw 0x0400 divider (shared with the PDI watchdog).
  uint16_t ticks = 0;                   ///< Raw 0x0420 process-data watchdog time register.
};

/// @brief Abstract interface for an EtherCAT fieldbus driver.
///
/// Concrete implementations: @c SoemFieldbusDriver (SOEM), @c SpoeFieldbusDriver (SPoE, planned).
/// The composition root (@c main.cc) constructs exactly one and injects it into @c DeviceManager
/// via @c DeviceManager::init. @c GameLoop never references the driver — it runs @c CyclicTasks
/// (e.g. @c ProcessDataCyclicTask) that reach the bus only through @c DeviceManager.
///
/// The driver owns @c socketMutex_, which serialises the control-plane
/// operations (mailbox/SDO, FoE, ESC register, and state access) amongst
/// non-RT callers. The real-time PDO path (@c exchangeProcessData) runs
/// lock-free: SOEM's port layer is internally thread-safe (per-datagram index
/// allocation plus tx/rx mutexes held only for a single non-blocking poll), and
/// PDO touches disjoint state (the process-data IOmap) from the control plane
/// (mailbox pool, slave state). Keeping PDO out of the mutex keeps the RT cycle
/// unbounded by a slow SDO or object-dictionary enumeration.
///
/// @note Precondition on @c slavePosition/@c position: every slave-indexed method here trusts
/// its position argument and indexes a fixed-size slave table @b without bounds-checking. Callers
/// must pass a valid, discovered bus position (1..number-of-slaves) — an unknown or out-of-range
/// value is undefined behaviour (an out-of-bounds access, and for a write a datagram to a bogus
/// station). This is deliberate: the driver is a thin transport, and validating the position is
/// the caller's responsibility. @c DeviceManager does this for every call (it rejects an unknown
/// position with a 404 before reaching the driver); a program using a driver directly must
/// validate positions itself. The same "caller owns the preconditions" contract applies to
/// lifecycle ordering (@c init → @c scan → @c configureProcessData) and slave AL state.
class FieldbusDriver {
 public:
  virtual ~FieldbusDriver() = default;

  /// @brief Opens the network interface and initialises the master context.
  ///
  /// Must be called before any other driver method.
  ///
  /// @return Void on success, or an error string describing the failure.
  virtual std::expected<void, std::string> init() = 0;

  /// @brief Scans the bus for slaves and configures their sync managers and FMMUs.
  ///
  /// Must be called after a successful @c init(). Slaves remain in INIT state —
  /// state transitions are left entirely to the caller.
  ///
  /// @return Number of slaves found (0 is a valid result — an empty/unpowered bus, which the master
  ///         cannot distinguish from a disconnected one), or an error string if the scan fails.
  virtual std::expected<int, std::string> scan() = 0;

  /// @brief Returns the immutable identity fields for the slave at @p position.
  /// @param position  1-based slave position on the bus.
  virtual SlaveInfo slaveInfo(uint16_t position) const = 0;

  /// @brief Returns the last-known AL status for a slave without any bus I/O.
  ///
  /// Returns the cached AL Status register (bits 3:0 = state, bit 4 = error) from the most
  /// recent @c readStates() or state transition — no network round trip. Returns 0 before any
  /// state is known. Call @c readStates() to refresh the cache from the hardware. Lets callers
  /// (e.g. @c Device) derive online / exchanging status without a redundant cached copy.
  virtual uint16_t slaveState(uint16_t position) const = 0;

  /// @brief Maps the process data and lays out the IOmap (one-time, control-plane).
  ///
  /// Reads each slave's active PDO assignment, computes the process image, programs the
  /// FMMUs, and fills the driver-owned IOmap.  Slaves must be in PRE-OP (mailbox active)
  /// so the assignment can be read.  Run again to re-map after the slave set changes (e.g.
  /// a device returning from a firmware download) — the previous @c PdoLayout is invalidated.
  /// Serialised with other control-plane operations via the socket mutex. This is the one
  /// operation that must @b not overlap @c exchangeProcessData — it rewrites the IOmap the RT
  /// cycle reads — and, since the lock-free PDO path does not take the socket mutex, that
  /// exclusion is enforced by the caller (@c DeviceManager drains exchange and unpublishes the
  /// process image before re-mapping), not by the mutex.
  ///
  /// @return Void on success, or an error string if no driver is initialised, no process
  ///         data is mapped, or the image exceeds @c kMaxProcessImageBytes.
  virtual std::expected<void, std::string> configureProcessData() = 0;

  /// @brief Returns the current process-data layout established by @c configureProcessData.
  ///
  /// The returned spans alias the driver-owned IOmap and remain valid until the next
  /// @c configureProcessData or @c stop.  Before a successful @c configureProcessData the
  /// layout is empty (zero-sized spans, no slaves).
  virtual PdoLayout processDataLayout() = 0;

  /// @brief Returns the static ESC configuration of every slave, as last programmed by the master.
  ///
  /// A diagnostic snapshot (Sync Managers, FMMUs, mailbox, distributed clock, addresses) of what
  /// the master programmed into each slave's EtherCAT Slave Controller. Read from cached state — no
  /// bus I/O — so it mirrors what the master believes it programmed, not a live ESC read. It is
  /// built up across control-plane operations, not one: @c scan() sets identity, addresses, the
  /// mailbox SMs and DC capability; @c configureProcessData() adds the process-data SMs, FMMUs and
  /// DC timing (and rebuilds the FMMUs on a re-map); and a BOOT state transition transiently
  /// reprograms the mailbox SMs (reset on the return to PRE-OP). A meaningful snapshot therefore
  /// needs @c scan(), plus @c configureProcessData() for the process-data SMs/FMMUs. Optional
  /// capability: the default returns an empty vector for transports without an ESC (e.g. SPoE); the
  /// SOEM driver overrides it.
  ///
  /// @return Per-slave configuration in bus order, or an empty vector if unsupported / unscanned.
  virtual std::vector<SlaveConfig> busConfig() const { return {}; }

  /// @brief Exchanges one cycle of process data: sends @p outputs, receives into @p inputs.
  ///
  /// The caller owns both images; the driver translates them to and from its transport (an
  /// IOmap copy for SOEM/IgH, message (de)serialisation for SPoE).  @p outputs.size() must
  /// equal @c PdoLayout::outputBytes and @p inputs.size() must equal @c PdoLayout::inputBytes.
  /// Called once per @c GameLoop cycle.  Runs lock-free (does not take the socket mutex) —
  /// see the class-level note.  Must complete within the cycle budget; timing jitter here
  /// propagates directly to control latency.  Must not be called before
  /// @c configureProcessData or after @c stop.
  ///
  /// @param outputs  Output image to send (master→slave); not retained past the call.
  /// @param inputs   Buffer that receives the input image (slave→master).
  /// @return The working counter for the transaction.  Compare against
  ///         @c PdoLayout::expectedWkc: a lower value means one or more slaves did not
  ///         contribute this cycle.  Returns 0 when no driver is initialised.
  virtual int exchangeProcessData(std::span<const uint8_t> outputs, std::span<uint8_t> inputs) = 0;

  /// @brief Closes the network interface, releasing the master context.
  ///
  /// Does @b not command any AL state change — it only closes the transport (for SOEM,
  /// @c ecx_close, which just destroys the mailbox-pool mutex and closes the raw socket; nothing
  /// is sent on the wire). Slaves left in SAFE-OP/OP are not driven back to INIT by the master;
  /// once process-data frames stop arriving, each slave's own sync-manager watchdog trips and it
  /// drops out of OP on its own (the safety guarantee for comms loss, independent of any graceful
  /// shutdown). After @c stop() returns, @c exchangeProcessData() must not be called again.
  virtual void stop() = 0;

  /// @brief One slave's AL Status and AL Status Code registers — the "where" and the "why".
  ///
  /// They answer different questions, from two ESC registers (ETG.1000.6 §6.4.1):
  ///   - @c alStatus (register 0x0130): the slave's current situation — the AL state in bits 3:0
  ///     (1=INIT, 2=PRE-OP, 3=BOOT, 4=SAFE-OP, 8=OP) plus a bit-4 error indicator, set when the
  ///     slave rejects a requested state change or faults. Decode via @c alState / @c alHasError.
  ///   - @c alStatusCode (register 0x0134): *why* an error was raised — a diagnostic code for a
  ///     failed transition. Meaningful only while @c alStatus's error bit is set (else 0x0000).
  ///     Decode via @c alStatusCodeName / @c isAlStatusCodeTerminal.
  ///
  /// Examples:
  ///   - Healthy in OP: @c alStatus 0x0008 (OP, error bit clear), @c alStatusCode 0x0000.
  ///   - Couldn't hold OP after outputs stopped: @c alStatus 0x0014 (SAFE-OP | error bit),
  ///     @c alStatusCode 0x001B ("Sync manager watchdog").
  ///   - Illegal INIT->OP jump rejected: @c alStatus 0x0011 (INIT | error bit), @c alStatusCode
  ///     0x0011 ("Invalid requested state change"). The two share the value 0x0011 only by
  ///     coincidence and mean completely different things — which is why they are separate.
  struct SlaveStateRaw {
    uint16_t alStatus = 0;      ///< AL Status register 0x0130: state (bits 3:0) + error bit 4.
    uint16_t alStatusCode = 0;  ///< AL Status Code register 0x0134: error reason; 0 when no error.
  };

  /// @brief Reads the current AL Status for each slave in @p positions.
  ///
  /// Refreshes slave state from the hardware in one pass, then returns the raw
  /// AL Status register and AL Status Code register for each requested position.
  /// AL Status bits 3:0 encode the current state (1=Init, 2=PreOp, 3=Boot,
  /// 4=SafeOp, 8=Op); bit 4 is the error indicator.  AL Status Code is non-zero
  /// when an error is present and identifies the cause (ETG.1000.6 §6.4.1).
  ///
  /// @param positions  1-based slave positions to read.
  /// @return Raw state per position in the same order as @p positions, or an
  ///         error string if the hardware read fails.
  virtual std::expected<std::vector<SlaveStateRaw>, std::string> readStates(
      const std::vector<uint16_t>& positions) = 0;

  /// @brief Reads live link-quality and watchdog diagnostics for each slave in @p positions.
  ///
  /// Performs FPRD reads of the DL Status (0x0110), error-counter (0x0300–0x0313), and watchdog
  /// (0x0440–0x0443) ESC register blocks for every requested slave and returns the decoded
  /// counters. The counters are monotonic since the last clear (power cycle / explicit write), so
  /// callers compare successive snapshots and alert on a rising delta — a single non-zero value is
  /// historical, a climbing one is an active fault. Serialised with other control-plane operations
  /// via the socket mutex; runs concurrently with the lock-free @c exchangeProcessData and never
  /// blocks the RT cycle.
  ///
  /// Optional capability: the default returns an error for transports without an ESC (e.g. SPoE);
  /// the SOEM driver overrides it.
  ///
  /// @param positions  1-based slave positions to read.
  /// @return Decoded diagnostics per position in the same order as @p positions, or an error
  ///         string if the transport has no ESC or a register read fails.
  virtual std::expected<std::vector<SlaveDiagnostics>, std::string> readDiagnostics(
      const std::vector<uint16_t>& /*positions*/) {
    return std::unexpected("diagnostics not supported by this transport");
  }

  /// @brief Reads live distributed-clock synchronisation status for each slave in @p positions.
  ///
  /// Performs FPRD reads of the DC system-time delay (0x0928) and system-time difference (0x092C)
  /// registers for every DC-capable slave and decodes the signed deviation of each slave's local
  /// clock from the bus reference. The reference clock is the first DC-capable slave; its own
  /// difference is zero. Values are meaningful only while the bus is exchanging process data in
  /// SAFE-OP/OP (the reference time is distributed in the cyclic frame), and the difference
  /// converges toward zero as the slaves' drift-compensation loops settle — poll this and watch
  /// for one that stays large or grows. Serialised with other control-plane operations via the
  /// socket mutex; runs concurrently with the lock-free @c exchangeProcessData and never blocks
  /// the RT cycle.
  ///
  /// Optional capability: the default returns an error for transports without an ESC (e.g. SPoE);
  /// the SOEM driver overrides it.
  ///
  /// @param positions  1-based slave positions to read.
  /// @return Decoded DC sync status per position in the same order as @p positions, or an error
  ///         string if the transport has no ESC or a register read fails.
  virtual std::expected<std::vector<DcSyncDiagnostics>, std::string> readDcSync(
      const std::vector<uint16_t>& /*positions*/) {
    return std::unexpected("distributed-clock diagnostics not supported by this transport");
  }

  /// @brief Reads an object dictionary entry via CoE SDO upload.
  ///
  /// Allocates up to 4096 bytes, performs a mailbox SDO upload, and returns the exact
  /// bytes the slave sent (resized to the actual transfer size).
  /// Called from HTTP handler threads; serialised with other control-plane operations via the
  /// socket mutex, but runs concurrently with the lock-free @c exchangeProcessData — a slow SDO
  /// never blocks the RT cycle (the reason the PDO path stays out of the mutex).
  ///
  /// Returns an owning @c std::vector, not a @c std::span, deliberately: the bytes are produced
  /// by this call and their ownership must transfer to the caller, which a non-owning view cannot
  /// do (a span would have to alias driver state that the next call clobbers, or storage nothing
  /// frees). Nor can the caller lend a sized buffer the way @c readRegister does — an SDO object's
  /// size is unknown until the transfer completes (expedited vs. segmented), which is exactly why
  /// the implementation over-allocates and resizes. The return is moved out, so this costs no copy.
  /// (@c std::span is used elsewhere in this interface only for borrowed *inputs* — e.g.
  /// @c writeSdo / @c exchangeProcessData — never to hand back freshly produced data.)
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @return The bytes transferred on success, or an error string if the mailbox transfer fails.
  virtual std::expected<std::vector<uint8_t>, std::string> readSdo(uint16_t slavePosition,
                                                                   uint16_t index,
                                                                   uint8_t subindex) = 0;

  /// @brief Reads an entire object in one CoE SDO upload using Complete Access.
  ///
  /// Uploads every subindex of @p index in a single mailbox transfer (SDO command with the
  /// complete-access bit set, starting at subindex 0), replacing one @c readSdo per subindex.
  /// The returned blob is what the slave sends: subindex 0 as a 16-bit value (1 data byte + 1
  /// alignment pad), then subindices 1..N concatenated at their native bit lengths. The caller
  /// slices it back into per-subindex values from the object's known entry layout.
  ///
  /// Complete access is optional in CoE — a slave, or an individual object, that does not
  /// support it answers with an SDO abort. Callers must therefore treat any error as "fall back
  /// to per-subindex @c readSdo", not as a hard failure. The default implementation reports it
  /// unsupported; drivers backed by a real CoE mailbox override this.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param index          CoE object index.
  /// @return The raw complete-access blob on success, or an error string (including when the
  ///         driver or the slave does not support complete access).
  virtual std::expected<std::vector<uint8_t>, std::string> readSdoComplete(
      uint16_t /*slavePosition*/, uint16_t /*index*/) {
    return std::unexpected("complete access not supported by this driver");
  }

  /// @brief Writes an object dictionary entry to a slave (CoE SDO download).
  ///
  /// Performs a mailbox SDO download of @p data to (@p index, @p subindex). The slave
  /// must be in PRE-OP, SAFE-OP, or OP (mailbox communication active).
  /// Called from non-RT (HTTP handler) threads; serialized with other control-plane
  /// operations via the driver's socket mutex, and runs concurrently with the lock-free
  /// @c exchangeProcessData (never blocks the RT cycle).
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param index          CoE object index.
  /// @param subindex       CoE object subindex.
  /// @param data           Bytes to write; size must match the object's length.
  /// @return Void on success, or an error string if the mailbox transfer fails.
  virtual std::expected<void, std::string> writeSdo(uint16_t slavePosition, uint16_t index,
                                                    uint8_t subindex,
                                                    std::span<const uint8_t> data) = 0;

  /// @brief Enumerates the entire CoE object dictionary of a slave via SDO Info.
  ///
  /// Performs the "Get Object List" → "Get Object Description" → "Get Entry Description"
  /// sequence (ETG.1000.6 §5.6) and returns one @c OdEntry per @c (index, subindex) pair.
  /// The slave must be in PRE-OP, SAFE-OP, or OP — i.e. mailbox communication enabled.
  /// Many transfers are issued; expect the call to take seconds on a fully populated drive.
  ///
  /// Slaves that do not implement the SDO Info service return an error.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @return All OD entries on success, or an error string if any phase of the enumeration
  ///         fails after the driver's internal retries.
  virtual std::expected<std::vector<OdEntry>, std::string> readObjectDictionary(
      uint16_t slavePosition) = 0;

  /// @brief Reads the raw Slave Information Interface (SII / EEPROM) image of a slave.
  ///
  /// Reads the slave's EEPROM through the ESC's EEPROM-control registers and returns the raw
  /// byte image — a fixed 128-byte header followed by the self-describing category section
  /// (strings, general info, FMMU/Sync-Manager and PDO defaults, distributed-clock settings).
  /// Decode it with @c mm::comm::parseSii. EEPROM access is a control-plane operation serialised
  /// on the socket mutex per transaction (it runs concurrently with the lock-free
  /// @c exchangeProcessData); the real constraint is slave state — it is most reliable while the
  /// slave is in INIT or PRE-OP. The driver hands EEPROM control back to the slave's PDI before
  /// returning.
  ///
  /// Optional capability: the default returns an error for transports without an ESC EEPROM
  /// (e.g. SPoE); the SOEM driver overrides it.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @return The raw SII image on success, or an error string if the transport has no EEPROM,
  ///         the position is out of range, or the driver is not initialised.
  virtual std::expected<std::vector<uint8_t>, std::string> readSii(uint16_t /*slavePosition*/) {
    return std::unexpected("SII (EEPROM) read not supported by this transport");
  }

  /// @brief Writes a raw SII (EEPROM) image to a slave.
  ///
  /// Writes @p data to the slave's EEPROM through the ESC's EEPROM-control registers, one 16-bit
  /// word at a time from word address 0. @p data must be a whole number of 16-bit words (even
  /// length). This is a destructive control-plane operation: a malformed image can leave the slave
  /// unidentifiable until re-flashed. EEPROM access takes the socket mutex (running concurrently
  /// with the lock-free @c exchangeProcessData) and is only safe while the slave is in INIT or
  /// PRE-OP. The slave does not adopt the new contents until its ESC reloads the EEPROM — i.e.
  /// after a power cycle.
  ///
  /// Optional capability: the default returns an error for transports without an ESC EEPROM
  /// (e.g. SPoE); the SOEM driver overrides it.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param data           Raw SII image to write (even length).
  /// @return Void on success, or an error string if the transport has no EEPROM, the position is
  ///         out of range, @p data has an odd length, or a word write fails.
  virtual std::expected<void, std::string> writeSii(uint16_t /*slavePosition*/,
                                                    std::span<const uint8_t> /*data*/) {
    return std::unexpected("SII (EEPROM) write not supported by this transport");
  }

  /// @brief Reads a file from the slave via File over EtherCAT (FoE).
  ///
  /// Sends an FoE read request for @p filename and collects all data packets from the slave.
  /// FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is
  /// responsible for ensuring the device is in a suitable state.
  /// Called from HTTP handler threads; serialised with other control-plane operations via the
  /// socket mutex, and runs concurrently with the lock-free @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param filename       FoE filename as recognised by the slave firmware.
  /// @return File bytes on success, or an error string if the transfer fails.
  virtual std::expected<std::vector<uint8_t>, std::string> readFile(
      uint16_t slavePosition, const std::string& filename) = 0;

  /// @brief Writes a file to the slave via File over EtherCAT (FoE).
  ///
  /// Sends an FoE write request for @p filename and streams @p data to the slave.
  /// FoE is available in Boot, Pre-Op, Safe-Op, and Op states (device-dependent); the caller is
  /// responsible for ensuring the device is in a suitable state.
  /// Called from non-RT (HTTP handler) threads; serialized with other control-plane
  /// operations via the driver's socket mutex.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param filename       FoE filename as recognised by the slave firmware.
  /// @param data           File bytes to write.
  /// @return Void on success, or an error string if the transfer fails.
  virtual std::expected<void, std::string> writeFile(uint16_t slavePosition,
                                                     const std::string& filename,
                                                     std::span<const uint8_t> data) = 0;

  /// @brief Reads bytes from an ESC register via a Configured-Address Read (FPRD) datagram.
  ///
  /// @p data.size() bytes are read from register @p address of the slave at @p slavePosition.
  /// Called from HTTP handler threads; serialised with other control-plane operations via the
  /// socket mutex, and runs concurrently with the lock-free @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address (e.g. @c 0x0130 for DL Status).
  /// @param data           Output buffer; its size determines how many bytes are read.
  /// @return Void on success, or an error string if no slave responded.
  virtual std::expected<void, std::string> readRegister(uint16_t slavePosition, uint16_t address,
                                                        std::span<uint8_t> data) = 0;

  /// @brief Writes bytes to an ESC register via a Configured-Address Write (FPWR) datagram.
  ///
  /// @p data.size() bytes are written to register @p address of the slave at @p slavePosition.
  /// Called from HTTP handler threads; serialised with other control-plane operations via the
  /// socket mutex, and runs concurrently with the lock-free @c exchangeProcessData.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param address        ESC register address.
  /// @param data           Bytes to write.
  /// @return Void on success, or an error string if no slave responded.
  virtual std::expected<void, std::string> writeRegister(uint16_t slavePosition, uint16_t address,
                                                         std::span<const uint8_t> data) = 0;

  /// @brief Reads the process-data (sync-manager) watchdog configuration of one slave.
  ///
  /// FPRD-reads the watchdog divider (0x0400), process-data watchdog time (0x0420), and
  /// process-data watchdog status (0x0440) ESC registers, decoding the timeout as
  /// @c ticks × 40 ns × (divider + 2) plus the live running/expired status. A zero time register
  /// means the watchdog is disabled. Unlike @c readDiagnostics (which returns the expiration
  /// counter), this returns the configured timeout and its current run state. Serialised via the
  /// socket mutex; runs concurrently with the lock-free @c exchangeProcessData.
  ///
  /// Optional capability: the default returns an error for transports without an ESC (e.g. SPoE);
  /// the SOEM driver overrides it.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @return The decoded watchdog configuration, or an error string if the transport has no ESC
  ///         or a register read fails.
  virtual std::expected<ProcessDataWatchdogConfig, std::string> processDataWatchdog(
      uint16_t /*slavePosition*/) {
    return std::unexpected("process-data watchdog not supported by this transport");
  }

  /// @brief Sets the process-data (sync-manager) watchdog timeout of one slave.
  ///
  /// Reads the slave's current watchdog divider (0x0400) — left untouched, since it is shared with
  /// the PDI watchdog — computes the nearest tick count for @p timeout, and FPWR-writes the
  /// process-data watchdog time register (0x0420). A @p timeout of zero disables the watchdog.
  /// Because the tick resolution is the divider's time base, the achieved timeout is rounded; the
  /// returned config reports what was actually programmed. Fails if @p timeout exceeds what the
  /// current divider can represent (more than 65535 ticks) — the error names the maximum.
  ///
  /// The write persists across re-maps and re-scans (SOEM never reprograms these registers) until
  /// the ESC reloads EEPROM (power cycle / explicit reload). Serialised via the socket mutex; runs
  /// concurrently with the lock-free @c exchangeProcessData.
  ///
  /// Optional capability: the default returns an error for transports without an ESC (e.g. SPoE);
  /// the SOEM driver overrides it.
  ///
  /// @param slavePosition  1-based slave position on the bus.
  /// @param timeout        Desired watchdog timeout; zero disables the watchdog.
  /// @return The watchdog configuration actually programmed (timeout rounded to the tick base),
  ///         or an error string if the transport has no ESC, @p timeout is unrepresentable, or a
  ///         register access fails.
  virtual std::expected<ProcessDataWatchdogConfig, std::string> setProcessDataWatchdog(
      uint16_t /*slavePosition*/, std::chrono::nanoseconds /*timeout*/) {
    return std::unexpected("process-data watchdog not supported by this transport");
  }

  /// @brief Commands a set of devices to @p targetState and blocks until all arrive or
  ///        @p timeout elapses.
  ///
  /// Devices whose current state (error bit masked) does not match @p requiredState are
  /// skipped; pass @c std::nullopt to command all @p positions unconditionally.
  ///
  /// The call polls at ~100 ms intervals, re-sending the command to lagging devices every
  /// @p resendInterval.  Devices that do not arrive in time are logged at error level; no
  /// exception is thrown.  The outcome per device is not returned here — read it back via
  /// @c readStates after the call (this is what @c DeviceManager::transitionToState does).
  ///
  /// @param positions       1-based device positions to target.
  /// @param requiredState   Pre-filter: only command devices whose current state equals this
  ///                        value.  @c std::nullopt skips filtering and commands all positions.
  /// @param targetState     Desired state.
  /// @param timeout         Maximum time to wait for all devices.
  /// @param resendInterval  How often to re-send the command to lagging devices.
  /// @param tick            Optional callback invoked at ~1 ms intervals while waiting.
  ///                        Pass a PDO sender when targeting @c EtherCatState::Op so the
  ///                        sync-manager watchdog does not fire during the wait.
  /// @param shouldAbort     Optional predicate; when it returns @c true the wait is abandoned
  ///                        early without logging failures for pending devices.
  virtual void transitionToState(
      const std::vector<uint16_t>& positions, std::optional<EtherCatState> requiredState,
      EtherCatState targetState, std::chrono::steady_clock::duration timeout,
      std::chrono::steady_clock::duration resendInterval = std::chrono::seconds(2),
      std::function<void()> tick = nullptr, std::function<bool()> shouldAbort = nullptr) = 0;

 protected:
  /// Serialises control-plane access to the underlying fieldbus context (mailbox
  /// pool, slave state, manual mailbox sequence counters) amongst non-RT callers.
  /// Held only for the duration of a single socket transaction — never across a
  /// sleep, a blocking wait, or a user callback. The PDO path does not take this
  /// lock (see the class-level note). @c mutable so the @c const accessors
  /// (@c slaveInfo, @c slaveCount) can lock.
  mutable std::mutex socketMutex_;
};

}  // namespace mm::comm
