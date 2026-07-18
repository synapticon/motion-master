#include "comm/soem_fieldbus_driver.h"

#include <soem/soem.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "comm/al_status_codes.h"
#include "comm/mailbox_error_codes.h"
#include "comm/sdo_abort_codes.h"
#include "comm/sdo_log.h"

namespace mm::comm::soem {

SoemFieldbusDriver::SoemFieldbusDriver(SoemFieldbusDriverConfig config)
    : ifname_(std::move(config.ifname)), mailboxStatusFmmu_(config.mailboxStatusFmmu) {}

SoemFieldbusDriver::~SoemFieldbusDriver() { closeContext(); }

std::expected<void, std::string> SoemFieldbusDriver::init() {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  // SOEM has no auto-detect — an empty interface name would reach ecx_init and
  // fail with a cryptic "No such device". Reject it up front with a clear error.
  if (ifname_.empty()) {
    return std::unexpected("no network adapter specified — SOEM requires a NIC name");
  }
  // Allocate a fresh, value-initialized (zeroed) master context. Every ecx_* call takes this ctx
  // pointer as its first argument — that is what makes SOEM's modern API reentrant/multi-instance
  // instead of relying on one file-scope global. The zero-init matters: SOEM's mappers assume a
  // clean context and count on fields like slavelist[].FMMUunused starting at 0. Held behind a
  // unique_ptr for RAII cleanup (freed by ctx_.reset() below on failure, and by closeContext()).
  ctx_ = std::make_unique<ecx_contextt>();
  if (!ecx_init(ctx_.get(), ifname_.c_str())) {
    ctx_.reset();
    return std::unexpected("ecx_init failed on " + ifname_ + ": " + std::strerror(errno));
  }
  spdlog::debug("SOEM init on adapter '{}'", ifname_);
  return {};
}

std::expected<int, std::string> SoemFieldbusDriver::scan() {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  // ecx_config_init reprograms every slave's mailbox SMs to PRE-OP sizes and hands
  // EEPROM back to the PDI, so any prior BOOT-SM tracking is now stale.
  bootMailboxSlaves_.clear();
  // Take full manual control of AL state transitions. With the default (0), SOEM's config/map
  // helpers auto-request state changes (e.g. driving slaves toward PRE-OP) as a side effect. In
  // Motion Master, AL state is the user's job (POST /api/state → DeviceManager::transitionToState),
  // so we never want SOEM moving slaves on its own — every ecx_writestate is issued explicitly.
  ctx_->manualstatechange = 1;
  // ecx_config_init returns the broadcast-read working counter: > 0 is the slave count; a
  // non-positive value means no slaves answered. Negative values are SOEM transport codes
  // (EC_NOFRAME = -1, etc.); 0 is a returned frame that no slave incremented.
  int found = ecx_config_init(ctx_.get());
  if (found > 0) {
    spdlog::debug("SOEM scan found {} slave(s) on '{}'", found, ifname_);
    return found;
  }

  // Too many slaves is the one genuinely distinguishable failure — a real misconfiguration — so it
  // is an error. Every other non-positive result is an empty/unpowered/disconnected bus the master
  // cannot tell apart, and which the user recovers from by powering devices on and rescanning; log
  // the bus-level reason and report a successful scan of an empty bus (0 slaves).
  if (found == EC_SLAVECOUNTEXCEEDED) {
    spdlog::error("SOEM scan on '{}': too many slaves on the bus (exceeds EC_MAXSLAVE)", ifname_);
    return std::unexpected("too many slaves on the bus (exceeds EC_MAXSLAVE) on " + ifname_);
  }
  switch (found) {
    case EC_NOFRAME:
      spdlog::warn("SOEM scan on '{}': no frame returned — bus empty, unpowered, or disconnected",
                   ifname_);
      break;
    case EC_OTHERFRAME:
      spdlog::warn(
          "SOEM scan on '{}': unexpected frame during slave detection — possible cabling or "
          "interference issue",
          ifname_);
      break;
    case EC_ERROR:
      spdlog::warn("SOEM scan on '{}': transport error during slave detection", ifname_);
      break;
    default:
      spdlog::warn("SOEM scan on '{}': no slaves responded (working counter {})", ifname_, found);
      break;
  }
  return 0;
}

namespace {

// Estimates the smallest standard GameLoop cycle period that keeps EtherCAT wire
// utilisation under ~50%, leaving headroom for scheduling jitter, the master's own
// per-cycle work, and mailbox/SDO traffic interleaved between process-data frames.
// Pure wire-physics estimate for 100 Mbit EtherCAT — informational only.
std::chrono::microseconds recommendedCyclePeriod(uint32_t processBytes, int slaveCount) {
  if (processBytes == 0) {
    return std::chrono::microseconds(0);
  }
  // Usable process data per standard Ethernet frame after EtherCAT + datagram headers.
  constexpr uint32_t kUsablePerFrame = 1486;
  // Per-frame wire overhead: preamble + Ethernet header/FCS + inter-frame gap + framing.
  constexpr uint32_t kFrameOverhead = 50;
  const uint32_t frames = (processBytes + kUsablePerFrame - 1) / kUsablePerFrame;
  const uint32_t wireBytes = processBytes + frames * kFrameOverhead;
  // 100 Mbit/s = 100 bits/µs, so (wire bytes × 8) / 100 = microseconds on the wire.
  const double txUs = (wireBytes * 8.0) / 100.0;
  // Frame forwarding through the slave chain, both directions (~0.6 µs per slave).
  const double ringUs = slaveCount * 0.6;
  // Divide the on-wire time by the 0.5 utilisation target: the frame should occupy at most
  // half the cycle, leaving the other half free for jitter, master work, and interleaved
  // SDO/mailbox traffic. Worked example: 30 drives x 83 process bytes = 2490 bytes -> 2 frames
  // -> 2590 wire bytes -> 207 us tx + 18 us ring = 225 us on the wire; /0.5 = 450 us needed,
  // which snaps up to the 500 us step below -- so the bus wants a >= 0.5 ms cycle and runs at
  // ~25% utilisation at 1 ms.
  const double neededUs = (txUs + ringUs) / 0.5;  // target <= 50% bus utilisation
  for (int64_t step : {125, 250, 500, 1000, 2000, 4000, 8000}) {
    if (neededUs <= static_cast<double>(step)) {
      return std::chrono::microseconds(step);
    }
  }
  // Beyond the largest standard step — round up to the next whole millisecond.
  return std::chrono::microseconds(static_cast<int64_t>((neededUs + 999.0) / 1000.0) * 1000);
}

// Pops the most recent SOEM error (if any) after a failed SDO transfer and renders it as a short
// human-readable suffix (" (SDO abort 0x...)", " (mailbox error)", ...). Empty when no error was
// queued. Called with controlPlaneMutex_ held (it touches the SOEM context error stack).
std::string sdoErrorSuffix(ecx_contextt* ctx) {
  ec_errort err{};
  if (!ecx_poperror(ctx, &err)) {
    return {};
  }
  switch (err.Etype) {
    case EC_ERR_TYPE_SDO_ERROR: {
      const auto code = static_cast<uint32_t>(err.AbortCode);
      const std::string_view reason = sdoAbortCodeDescription(code);
      return reason.empty() ? std::format(" (SDO abort 0x{:08X})", code)
                            : std::format(" (SDO abort 0x{:08X}: {})", code, reason);
    }
    case EC_ERR_TYPE_MBX_ERROR: {
      const auto code = static_cast<uint16_t>(err.ErrorCode);
      const std::string_view reason = mailboxErrorCodeDescription(code);
      return reason.empty() ? std::format(" (mailbox error 0x{:04X})", code)
                            : std::format(" (mailbox error 0x{:04X}: {})", code, reason);
    }
    case EC_ERR_TYPE_PACKET_ERROR:
      return " (packet/timeout error)";
    default:
      return std::format(" (etype {})", static_cast<int>(err.Etype));
  }
}

}  // namespace

std::expected<void, std::string> SoemFieldbusDriver::configureProcessData() {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("configureProcessData: no driver — call init() first");
  }
  // ecx_config_map_group only assigns each slave's outputs/inputs IOmap pointer when it is still
  // null (`if (!slavelist[slave].outputs)`); it recomputes Obits/Ibits/Obytes and the group
  // pointers unconditionally, but the per-slave pointers stick. On a re-map (a device returning
  // from a firmware download re-runs this without an intervening scan/ecx_config_init, which is
  // the only thing that memsets the slavelist) those pointers therefore retain their first-map
  // values, so processDataLayout()'s `slave.outputs - group.outputs` would yield offsets for the
  // old layout if the new firmware's PDO mapping changed size or order. Null them first so SOEM
  // recomputes them against the freshly mapped IOmap — exactly what a clean scan would do.
  uint8_t zeroFmmus[sizeof(ec_fmmut) * EC_MAXFMMU] = {};
  for (int i = 1; i <= ctx_->slavecount; ++i) {
    ctx_->slavelist[i].outputs = nullptr;
    ctx_->slavelist[i].inputs = nullptr;
    // ecx_config_map_group (via ecx_map_coe_soe) invokes the per-slave PO->SO config hook
    // `if (slavelist[i].PO2SOconfig)` before reading the PDO mapping. We never register such a
    // hook, and SOEM never assigns it — ecx_init_context's memset zeroes it once, but only on a
    // fresh ecx_config_init, which a re-map does not run. A stray write that flips this field to a
    // non-null value (observed after a BOOT/firmware excursion) therefore turns the map into a call
    // through a garbage function pointer and segfaults the whole process. Force it null on every
    // map so SOEM's guard always short-circuits — defence in depth for the same reason the
    // outputs/inputs pointers above are reset rather than trusted across re-maps.
    ctx_->slavelist[i].PO2SOconfig = nullptr;
    // Reset each slave's FMMU bookkeeping before the map. ecx_config_create_{output,input}_mappings
    // and the mailbox-status mapper all begin at `FMMUunused` and append, relying on
    // ecx_config_init having memset the slavelist (FMMUunused=0, FMMU[] cleared) — which a re-map
    // does NOT run. Left unreset, FMMUunused stays at its previous value (e.g. 3) so the output
    // mapper writes a *new* Outputs FMMU at index 3 (a byte-identical duplicate of FMMU0), then the
    // input/mailbox mappers run with FMMUc == EC_MAXFMMU (4) and write past the end of the
    // EC_MAXFMMU-sized FMMU[] array — corrupting the adjacent FMMU*func/mbx_*/…/PO2SOconfig fields
    // of ec_slavet. That OOB write is the likely root cause behind the duplicate-Outputs-FMMU
    // symptom, the broken mailbox sizes that make a subsequent SAFE-OP fail, and the stray non-null
    // PO2SOconfig guarded above. Zero the in-memory array + counter so SOEM re-derives FMMU0/1/2
    // from scratch exactly as a clean scan would, and clear all EC_MAXFMMU FMMU registers on the
    // ESC so a slave already carrying a stale duplicate from a pre-fix re-map self-heals once SOEM
    // reprograms the live ones during the map.
    std::memset(ctx_->slavelist[i].FMMU, 0, sizeof(ctx_->slavelist[i].FMMU));
    ctx_->slavelist[i].FMMUunused = 0;
    if (ecx_FPWR(&ctx_->port, ctx_->slavelist[i].configadr, ECT_REG_FMMU0, sizeof(zeroFmmus),
                 zeroFmmus, EC_TIMEOUTRET3) != 1) {
      // Best-effort: the in-memory memset above already fixes the map SOEM is about to build, and
      // the live FMMU0/1/2 are reprogrammed during it. A failure here only means a slave carrying a
      // stale duplicate FMMU (index >= 3) in its ESC will not self-heal this pass — log it rather
      // than let the one deliberately-unchecked register write read as an oversight.
      spdlog::debug("Slave {}: FMMU register clear (FPWR 0x0600) failed; stale FMMUs may persist",
                    i);
    }
  }
  // TI PRU-ICSS ESCs cannot process SOEM's combined read+write LRW process-data datagram; mark
  // them so the map aggregates split LRD/LWR instead. Must run before ecx_config_map_group, which
  // reads each slave's blockLRW to build the group's send plan.
  if (auto r = blockLrwOnPruIcssSlaves(); !r) {
    return std::unexpected(std::move(r.error()));
  }
  const int usedSize = ecx_config_map_group(ctx_.get(), map_, 0);
  if (usedSize <= 0) {
    return std::unexpected("ecx_config_map_group mapped no process data");
  }
  if (static_cast<size_t>(usedSize) > sizeof(map_)) {
    return std::unexpected(
        std::format("process image {} bytes exceeds IOmap capacity {} bytes — reduce mapped PDOs",
                    usedSize, sizeof(map_)));
  }
  // Turn off SOEM 2.0's mailbox-status FMMU (unless explicitly kept) before any AL transition into
  // an exchange state — it is mapped by the ecx_config_map_group above and is fatal on TI ESCs.
  if (auto r = deactivateMailboxStatusFmmus(); !r) {
    return std::unexpected(std::move(r.error()));
  }
  // Initialise the Distributed Clocks system: measure propagation delays and elect a reference
  // clock. We deliberately do not call ecx_dcsync0, so process data stays SM-synchronous
  // (free-run), driven by the GameLoop's software cycle — DC is initialised but no SYNC0 pulse
  // is generated, matching how the bus was brought up previously.
  const bool dc = ecx_configdc(ctx_.get());
  spdlog::info("Distributed clock configured; DC-capable slaves present: {}", dc ? "yes" : "no");
  const auto& grp = ctx_->grouplist[0];
  const auto period = recommendedCyclePeriod(grp.Obytes + grp.Ibytes, ctx_->slavecount);
  spdlog::info(
      "Process data mapped: {} bytes (out {}, in {}) across {} slave(s); "
      "recommended GameLoop cycle >= {} us ({:.1f} ms)",
      usedSize, grp.Obytes, grp.Ibytes, ctx_->slavecount, period.count(), period.count() / 1000.0);
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::deactivateMailboxStatusFmmus() {
  // SOEM 2.0's ecx_config_create_mbxstatus_mappings() programs an extra *input* FMMU on every
  // mailbox-capable slave that maps the SM1 mailbox-status register (ECT_REG_SM1STAT == 0x080D)
  // into the cyclic logical image, so the master can spot a waiting mailbox message without a
  // separate acyclic read. SOEM 1.x had no such FMMU. It is an optimisation we do not use — we
  // never read slavelist[].mbxstatus; SDO/FoE poll the mailbox directly. On TI PRU-ICSS ESCs
  // (ESC type 0x90) a register-space FMMU inside an LRW is unsupported: every cyclic frame dies in
  // the processing unit (error counter 0x030C saturates at 255), the working counter stays 0, and
  // SAFE-OP -> OP fails with AL status 0x001B (SM watchdog). Deactivating this one FMMU restores
  // cyclic exchange; it is harmless on every other ESC because nothing consumes it. Gated off only
  // when the config keeps it active for hardware that both needs and supports it.
  if (mailboxStatusFmmu_) {
    return {};
  }
  for (int i = 1; i <= ctx_->slavecount; ++i) {
    ec_slavet& s = ctx_->slavelist[i];
    for (int j = 0; j < EC_MAXFMMU; ++j) {
      // The mailbox-status FMMU is the only one mapping physical address 0x080D. The mapper assigns
      // PhysStart raw (no byte-swap), so this direct compare is correct on our little-endian
      // targets.
      if (s.FMMU[j].PhysStart != ECT_REG_SM1STAT || s.FMMU[j].FMMUactive == 0) {
        continue;
      }
      s.FMMU[j].FMMUactive = 0;  // keep the cached snapshot (processDataLayout / busConfig) honest
      uint8_t inactive = 0;
      const uint16_t activateReg = static_cast<uint16_t>(ECT_REG_FMMU0 + sizeof(ec_fmmut) * j +
                                                         offsetof(ec_fmmut, FMMUactive));
      if (ecx_FPWR(&ctx_->port, s.configadr, activateReg, sizeof(inactive), &inactive,
                   EC_TIMEOUTRET3) != 1) {
        return std::unexpected(
            std::format("failed to deactivate mailbox-status FMMU on slave {} (FMMU{})", i, j));
      }
      // WKC bookkeeping: the mapper only bumped grouplist[0].inputsWKC for a slave that had no
      // other input read (Ibytes == 0), because the working counter increments once per slave per
      // read datagram, not per FMMU. Mirror that reversal so processDataLayout's expected WKC stays
      // consistent and the bus is not permanently flagged unhealthy. Slaves with input PDOs (all
      // real drives) never bumped it, so this branch is a no-op for them.
      if (s.Ibytes == 0 && ctx_->grouplist[0].inputsWKC > 0) {
        ctx_->grouplist[0].inputsWKC--;
      }
      spdlog::debug("Deactivated SOEM mailbox-status FMMU{} on slave {}", j, i);
      break;  // at most one such FMMU per slave
    }
  }
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::blockLrwOnPruIcssSlaves() {
  // The ESC type lives in register 0x0000; its low byte identifies the controller family. 0x90 is
  // the TI PRU-ICSS soft-ESC (e.g. SOMANET "Jasper"). Reading it live per slave costs one FPRD each
  // and only happens on a (re)map, not on the RT cycle.
  for (int i = 1; i <= ctx_->slavecount; ++i) {
    ec_slavet& s = ctx_->slavelist[i];
    uint16_t escType = 0;
    if (ecx_FPRD(&ctx_->port, s.configadr, ECT_REG_TYPE, sizeof(escType), &escType,
                 EC_TIMEOUTRET3) != 1) {
      return std::unexpected(std::format("failed to read ESC type register of slave {}", i));
    }
    if ((escType & 0x00ffu) == 0x90u) {
      s.blockLRW = 1;  // ecx_config_map_group folds this into grouplist[0].blockLRW
      spdlog::info(
          "Slave {}: TI PRU-ICSS ESC (type 0x90) detected; using split LRD/LWR process data "
          "instead of combined LRW",
          i);
    }
  }
  return {};
}

PdoLayout SoemFieldbusDriver::processDataLayout() {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  PdoLayout layout;
  if (!ctx_) {
    return layout;
  }
  const auto& grp = ctx_->grouplist[0];
  layout.outputBytes = grp.Obytes;
  layout.inputBytes = grp.Ibytes;
  layout.expectedWkc = grp.outputsWKC * 2 + grp.inputsWKC;
  layout.slaves.reserve(static_cast<size_t>(ctx_->slavecount));
  for (int i = 1; i <= ctx_->slavecount; ++i) {
    const auto& s = ctx_->slavelist[i];
    // SOEM never populates the per-slave Ooffset/Ioffset fields — only the outputs/inputs
    // pointers into the IOmap — so reading those offsets yields 0 and a single slave's inputs
    // wrap to -Obytes. Derive each window's offset within its direction's image from the
    // pointers instead: the group's outputs pointer is the output image base and its inputs
    // pointer is the input image base (IOmap laid out as [all outputs | all inputs]). A
    // sub-byte-only direction reports 0 bytes; its pointer is never set, so it gets a 0 offset.
    // outputs/inputs are uint8_t*, so each pointer difference is already the byte offset into its
    // direction's image (the [all outputs | all inputs] IOmap) — no element scaling.
    layout.slaves.push_back(SlaveIo{
        .slavePosition = static_cast<uint16_t>(i),
        .outputOffset = s.Obytes ? static_cast<uint32_t>(s.outputs - grp.outputs) : 0,
        .outputBytes = s.Obytes,
        .inputOffset = s.Ibytes ? static_cast<uint32_t>(s.inputs - grp.inputs) : 0,
        .inputBytes = s.Ibytes,
    });
  }
  return layout;
}

std::vector<SlaveConfig> SoemFieldbusDriver::busConfig() const {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  std::vector<SlaveConfig> out;
  if (!ctx_) {
    return out;
  }
  // All of this is cached in the slavelist by ecx_config_init / ecx_config_map_group — what the
  // master programmed into each ESC. No bus I/O; a pure read of SOEM's in-memory configuration.
  out.reserve(static_cast<size_t>(ctx_->slavecount));
  for (int i = 1; i <= ctx_->slavecount; ++i) {
    const auto& s = ctx_->slavelist[i];
    SlaveConfig c{};
    c.slavePosition = static_cast<uint16_t>(i);
    c.configuredAddress = s.configadr;
    c.aliasAddress = s.aliasadr;
    c.outputBits = s.Obits;
    c.inputBits = s.Ibits;
    c.mailbox = {.writeLength = s.mbx_l,
                 .writeOffset = s.mbx_wo,
                 .readLength = s.mbx_rl,
                 .readOffset = s.mbx_ro,
                 .protocols = s.mbx_proto,
                 // Advertised protocol detail bytes (EEPROM, no bus I/O) — decoded by the client.
                 .coeDetails = s.CoEdetails,
                 .foeDetails = s.FoEdetails,
                 .eoeDetails = s.EoEdetails,
                 .soeDetails = s.SoEdetails};
    c.dc = {.capable = s.hasdc != 0,
            .active = s.DCactive != 0,
            .propagationDelay = s.pdelay,
            .cycleTime = s.DCcycle,
            .shift = s.DCshift};
    // Skip wholly-unused SMs/FMMUs (no window, no type) so the snapshot lists only what is
    // actually configured; the index field preserves the real SM/FMMU number for the reader.
    for (int j = 0; j < EC_MAXSM; ++j) {
      if (s.SM[j].SMlength == 0 && s.SMtype[j] == 0) {
        continue;
      }
      c.syncManagers.push_back(SyncManagerConfig{
          .index = static_cast<uint8_t>(j),
          .physicalStart = s.SM[j].StartAddr,
          .length = s.SM[j].SMlength,
          .flags = s.SM[j].SMflags,
          .type = s.SMtype[j],
      });
    }
    for (int j = 0; j < EC_MAXFMMU; ++j) {
      if (s.FMMU[j].LogLength == 0 && s.FMMU[j].FMMUactive == 0) {
        continue;
      }
      c.fmmus.push_back(FmmuConfig{
          .index = static_cast<uint8_t>(j),
          .logicalStart = s.FMMU[j].LogStart,
          .length = s.FMMU[j].LogLength,
          .logicalStartBit = s.FMMU[j].LogStartbit,
          .logicalEndBit = s.FMMU[j].LogEndbit,
          .physicalStart = s.FMMU[j].PhysStart,
          .physicalStartBit = s.FMMU[j].PhysStartBit,
          .type = s.FMMU[j].FMMUtype,
          .active = s.FMMU[j].FMMUactive,
      });
    }
    out.push_back(std::move(c));
  }
  return out;
}

// Intentionally lock-free: the RT PDO cycle relies on SOEM's internally
// thread-safe port layer rather than controlPlaneMutex_, so a slow control-plane
// transfer can never stall process-data exchange. See FieldbusDriver class doc.
int SoemFieldbusDriver::exchangeProcessData(std::span<const uint8_t> outputs,
                                            std::span<uint8_t> inputs) {
  if (!ctx_) {
    return 0;
  }
  const auto& grp = ctx_->grouplist[0];
  // Copy the caller's output image into the IOmap, exchange, copy the input image back out.
  // std::min guards against a caller buffer that disagrees with the mapped size.
  if (!outputs.empty() && grp.Obytes > 0) {
    std::memcpy(map_, outputs.data(), std::min<size_t>(outputs.size(), grp.Obytes));
  }
  ecx_send_processdata(ctx_.get());
  const int wkc = ecx_receive_processdata(ctx_.get(), EC_TIMEOUTRET);
  if (!inputs.empty() && grp.Ibytes > 0) {
    std::memcpy(inputs.data(), map_ + grp.Obytes, std::min<size_t>(inputs.size(), grp.Ibytes));
  }
  return wkc;
}

void SoemFieldbusDriver::stop() { closeContext(); }

void SoemFieldbusDriver::closeContext() {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  // ecx_init opened the raw socket; ecx_close releases it (via ecx_closenic) and takes the NIC
  // out of promiscuous mode. Without it every reset()/re-init cycle leaks the socket fd.
  // Idempotent by construction: ctx_.reset() below is std::unique_ptr::reset — it frees the
  // context and nulls the pointer — so the first call (from stop()) establishes ctx_ == nullptr
  // and the second (from the destructor) sees the guard fail and does nothing; ecx_close never
  // runs twice. The two are sequential, not concurrent — DeviceManager::reset() runs stop() fully
  // (and stopExchange() before it, so no lock-free exchangeProcessData is in flight against ctx_)
  // before driver_.reset() fires the destructor.
  if (ctx_) {
    ecx_close(ctx_.get());
    ctx_.reset();  // unique_ptr::reset: destroy the ecx_context, leave ctx_ holding nullptr
  }
}

SlaveInfo SoemFieldbusDriver::slaveInfo(uint16_t position) const {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return {};
  }
  const auto& s = ctx_->slavelist[position];
  return {
      .name = std::string(s.name),
      .vendorId = s.eep_man,
      .productCode = s.eep_id,
      .revisionNumber = s.eep_rev,
      .serialNumber = s.eep_ser,
  };
}

int SoemFieldbusDriver::slaveCount() const {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  return ctx_ ? ctx_->slavecount : 0;
}

uint16_t SoemFieldbusDriver::slaveState(uint16_t position) const {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  // SOEM caches the AL status in slavelist[].state, refreshed by ecx_readstate (via
  // readStates) and ecx_writestate (via transitionToState). No bus I/O here.
  return ctx_ ? ctx_->slavelist[position].state : 0;
}

std::expected<std::vector<FieldbusDriver::SlaveStateRaw>, std::string>
SoemFieldbusDriver::readStates(const std::vector<uint16_t>& positions) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  ecx_readstate(ctx_.get());
  std::vector<SlaveStateRaw> result;
  result.reserve(positions.size());
  std::ranges::transform(positions, std::back_inserter(result), [this](uint16_t pos) {
    return SlaveStateRaw{.alStatus = ctx_->slavelist[pos].state,
                         .alStatusCode = ctx_->slavelist[pos].ALstatuscode};
  });
  return result;
}

std::expected<std::vector<SlaveDiagnostics>, std::string> SoemFieldbusDriver::readDiagnostics(
    const std::vector<uint16_t>& positions) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  std::vector<SlaveDiagnostics> result;
  result.reserve(positions.size());
  for (uint16_t pos : positions) {
    const uint16_t configAddr = ctx_->slavelist[pos].configadr;

    // DL Status (0x0110): link/loop/communication per port. Bits 4–7 = link on ports 0–3; for
    // port p, bit (8 + 2p) = loop closed, bit (9 + 2p) = communication established.
    uint16_t dlStatus = 0;
    if (ecx_FPRD(&ctx_->port, configAddr, 0x0110, sizeof(dlStatus), &dlStatus, EC_TIMEOUTRET) !=
        1) {
      return std::unexpected(std::format("FPRD slave {} DL status (0x0110) failed", pos));
    }

    // Error-counter block (0x0300–0x0313), read contiguously in one datagram:
    //   [0..7]   RX error counter   — per port: byte 2p = invalid frame, byte 2p+1 = RX error
    //   [8..11]  forwarded RX error — 1 byte per port
    //   [12]     processing-unit error (0x030C)   [13] PDI error (0x030D)   [14,15] reserved
    //   [16..19] lost-link counter  — 1 byte per port
    std::array<uint8_t, 20> errs{};
    if (ecx_FPRD(&ctx_->port, configAddr, 0x0300, static_cast<uint16_t>(errs.size()), errs.data(),
                 EC_TIMEOUTRET) != 1) {
      return std::unexpected(std::format("FPRD slave {} error counters (0x0300) failed", pos));
    }

    // Watchdog block (0x0440–0x0443): [0,1] PD watchdog status, [2] PD watchdog expirations
    // (0x0442), [3] PDI watchdog expirations (0x0443).
    std::array<uint8_t, 4> wd{};
    if (ecx_FPRD(&ctx_->port, configAddr, 0x0440, static_cast<uint16_t>(wd.size()), wd.data(),
                 EC_TIMEOUTRET) != 1) {
      return std::unexpected(std::format("FPRD slave {} watchdog (0x0440) failed", pos));
    }

    SlaveDiagnostics d{};
    d.slavePosition = pos;
    for (int p = 0; p < 4; ++p) {
      d.ports[p] = PortDiagnostics{
          .linkUp = (dlStatus & (1u << (4 + p))) != 0,
          .loopClosed = (dlStatus & (1u << (8 + 2 * p))) != 0,
          .communication = (dlStatus & (1u << (9 + 2 * p))) != 0,
          .invalidFrame = errs[2 * p],
          .rxError = errs[2 * p + 1],
          .forwardedError = errs[8 + p],
          .lostLink = errs[16 + p],
      };
    }
    d.processingUnitError = errs[12];
    d.pdiError = errs[13];
    d.processDataWatchdog = wd[2];
    d.pdiWatchdog = wd[3];
    result.push_back(d);
  }
  return result;
}

std::expected<std::vector<DcSyncDiagnostics>, std::string> SoemFieldbusDriver::readDcSync(
    const std::vector<uint16_t>& positions) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }

  // The reference clock is the first DC-capable slave in bus order — SOEM elects it during
  // ecx_configdc and distributes its system time to the rest in the cyclic frame. Find it once so
  // each queried slave can be flagged; its own system-time difference is zero by definition.
  uint16_t referencePos = 0;
  for (int i = 1; i <= ctx_->slavecount; ++i) {
    if (ctx_->slavelist[i].hasdc != 0) {
      referencePos = static_cast<uint16_t>(i);
      break;
    }
  }

  std::vector<DcSyncDiagnostics> result;
  result.reserve(positions.size());
  for (uint16_t pos : positions) {
    DcSyncDiagnostics d{};
    d.slavePosition = pos;
    d.dcCapable = ctx_->slavelist[pos].hasdc != 0;
    d.referenceClock = (pos == referencePos);
    if (!d.dcCapable) {
      // Non-DC slave: no DC unit, so the registers carry no meaningful time — report zeroed.
      result.push_back(d);
      continue;
    }

    // System-time delay (0x0928, 4 bytes) and system-time difference (0x092C, 4 bytes) are
    // contiguous — read both in one FPRD. Both are little-endian on the wire.
    const uint16_t configAddr = ctx_->slavelist[pos].configadr;
    std::array<uint8_t, 8> dc{};
    if (ecx_FPRD(&ctx_->port, configAddr, 0x0928, static_cast<uint16_t>(dc.size()), dc.data(),
                 EC_TIMEOUTRET) != 1) {
      return std::unexpected(std::format("FPRD slave {} DC sync registers (0x0928) failed", pos));
    }

    const uint32_t delay = dc[0] | (static_cast<uint32_t>(dc[1]) << 8) |
                           (static_cast<uint32_t>(dc[2]) << 16) |
                           (static_cast<uint32_t>(dc[3]) << 24);
    const uint32_t diffRaw = dc[4] | (static_cast<uint32_t>(dc[5]) << 8) |
                             (static_cast<uint32_t>(dc[6]) << 16) |
                             (static_cast<uint32_t>(dc[7]) << 24);

    // System-time difference (0x092C): bits 0–30 are the mean deviation magnitude in ns; bit 31 is
    // the sign — set when the local copy of the system time is smaller than the reference (the
    // local clock is behind). Map to a signed figure where positive = ahead, negative = behind.
    const int32_t magnitude = static_cast<int32_t>(diffRaw & 0x7FFFFFFFu);
    d.propagationDelay = static_cast<int32_t>(delay);
    d.systemTimeDifference = (diffRaw & 0x80000000u) ? -magnitude : magnitude;
    result.push_back(d);
  }
  return result;
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readSdo(uint16_t slavePosition,
                                                                             uint16_t index,
                                                                             uint8_t subindex) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  // The background ParameterRefresher polls SDOs continuously; demote its per-read traces to trace
  // so they don't flood the log, while a direct (user-initiated) read keeps its debug trace.
  const auto sdoLevel = sdoLogQuiet ? spdlog::level::trace : spdlog::level::debug;
  spdlog::log(sdoLevel, "SDOread slave {} 0x{:04X}:{:02X}", slavePosition, index, subindex);
  // 4096-byte upload buffer: ample for every scalar/string parameter a SOMANET drive exposes at a
  // single subindex. ecx_SDOread clamps the transfer to the buffer and reports the bytes read with
  // a positive wkc, giving no truncation flag — so rather than hand back silently-short data for an
  // over-cap object, the read fails loudly below when the returned size fills the buffer.
  constexpr int kSdoBufferBytes = 4096;
  std::vector<uint8_t> data(kSdoBufferBytes, 0);
  int size = static_cast<int>(data.size());
  int wkc = ecx_SDOread(ctx_.get(), slavePosition, index, subindex, FALSE, &size, data.data(),
                        EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg =
        std::format("SDOread slave {} 0x{:04X}:{:02X} failed", slavePosition, index, subindex);
    msg += sdoErrorSuffix(ctx_.get());
    spdlog::log(sdoLevel, "{}", msg);
    return std::unexpected(msg);
  }
  if (size >= kSdoBufferBytes) {
    return std::unexpected(std::format(
        "SDOread slave {} 0x{:04X}:{:02X} filled the {}-byte read buffer — the object is larger "
        "than the SDO read cap; raise kSdoBufferBytes",
        slavePosition, index, subindex, kSdoBufferBytes));
  }
  // ecx_SDOread's psize is in/out: passed in as the 4096-byte capacity, it comes back set to the
  // actual bytes read (SOEM overwrites *psize). Shrink the buffer from the capacity to those bytes.
  data.resize(size);
  // Show the returned bytes (wire order, little-endian) so the value is visible. The driver has no
  // data type to decode it — that happens in the node layer — so the raw bytes are the faithful
  // view. `{:n}` keeps to_hex on one line (no offset column / newlines); the range is capped first
  // so a large object (string/array) can't run away.
  constexpr size_t kMaxHexBytes = 16;
  const auto hexEnd =
      data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), kMaxHexBytes));
  spdlog::log(sdoLevel, "SDOread slave {} 0x{:04X}:{:02X} ok ({} bytes): {:n}{}", slavePosition,
              index, subindex, data.size(), spdlog::to_hex(data.begin(), hexEnd),
              data.size() > kMaxHexBytes ? " ..." : "");
  return data;
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readSdoComplete(
    uint16_t slavePosition, uint16_t index) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  const auto sdoLevel = sdoLogQuiet ? spdlog::level::trace : spdlog::level::debug;
  spdlog::log(sdoLevel, "SDOread(CA) slave {} 0x{:04X}", slavePosition, index);
  // 64 KiB upload buffer. Complete Access returns *every* subindex of an object in one (internally
  // segmented) transfer, so unlike the single-subindex readSdo this must hold a whole ARRAY/RECORD
  // — potentially hundreds of entries. 64 KiB covers every object a SOMANET drive exposes with wide
  // margin while remaining a cheap transient allocation off the RT path. As in readSdo, ecx_SDOread
  // clamps to the buffer and gives no truncation flag, so an over-cap object is caught below rather
  // than returned as a silently-short blob (which the node layer would then mis-slice).
  constexpr int kCompleteAccessBufferBytes = 64 * 1024;
  std::vector<uint8_t> data(kCompleteAccessBufferBytes, 0);
  int size = static_cast<int>(data.size());
  // Complete access: start at subindex 0 with the CA flag TRUE, so SOEM sets the complete-access
  // bit and the slave returns every subindex in one transfer (segmented internally if it exceeds
  // the mailbox). The blob is sub0 as a 16-bit value (1 data byte + 1 pad) followed by sub1..N at
  // their native widths — the node layer slices it back using the object's known entry layout.
  int wkc =
      ecx_SDOread(ctx_.get(), slavePosition, index, 0x00, TRUE, &size, data.data(), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg = std::format("SDOread(CA) slave {} 0x{:04X} failed", slavePosition, index);
    msg += sdoErrorSuffix(ctx_.get());
    spdlog::log(sdoLevel, "{}", msg);
    return std::unexpected(msg);
  }
  if (size >= kCompleteAccessBufferBytes) {
    return std::unexpected(std::format(
        "SDOread(CA) slave {} 0x{:04X} filled the {}-byte read buffer — the object is larger than "
        "the complete-access read cap; raise kCompleteAccessBufferBytes",
        slavePosition, index, kCompleteAccessBufferBytes));
  }
  data.resize(size);
  spdlog::log(sdoLevel, "SDOread(CA) slave {} 0x{:04X} ok ({} bytes)", slavePosition, index,
              data.size());
  return data;
}

std::expected<void, std::string> SoemFieldbusDriver::writeSdo(uint16_t slavePosition,
                                                              uint16_t index, uint8_t subindex,
                                                              std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  spdlog::debug("SDOwrite slave {} 0x{:04X}:{:02X} ({} bytes)", slavePosition, index, subindex,
                data.size());
  // ecx_SDOwrite takes void*, not const void*.
  int wkc =
      ecx_SDOwrite(ctx_.get(), slavePosition, index, subindex, FALSE, static_cast<int>(data.size()),
                   const_cast<uint8_t*>(data.data()), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    std::string msg =
        std::format("SDOwrite slave {} 0x{:04X}:{:02X} failed", slavePosition, index, subindex);
    msg += sdoErrorSuffix(ctx_.get());
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  spdlog::debug("SDOwrite slave {} 0x{:04X}:{:02X} ok", slavePosition, index, subindex);
  return {};
}

namespace {

// SOEM's SDO Info path occasionally times out on slaves that buffer slowly;
// retry with a short back-off rather than failing the whole enumeration.
constexpr int kSdoInfoMaxRetries = 10;
constexpr auto kSdoInfoRetryDelay = std::chrono::milliseconds(50);

template <typename F>
int retrySdoInfo(F&& call) {
  int result = call();
  for (int i = 0; result <= 0 && i < kSdoInfoMaxRetries; ++i) {
    std::this_thread::sleep_for(kSdoInfoRetryDelay);
    result = call();
  }
  return result;
}

// Decodes an ecx_FOEread/ecx_FOEwrite failure return value into a human-readable suffix. SOEM
// reports the FoE error kind as a negated ec_err_type in the return value (not via the error
// list, and without the wire error code), so we classify from -wkc.
std::string foeErrorDetail(int wkc) {
  switch (-wkc) {
    case EC_ERR_TYPE_FOE_FILE_NOTFOUND:
      return " (file not found)";
    case EC_ERR_TYPE_FOE_BUF2SMALL:
      return " (buffer too small)";
    case EC_ERR_TYPE_FOE_PACKETNUMBER:
      return " (packet number mismatch)";
    case EC_ERR_TYPE_FOE_ERROR:
      return " (FoE error)";
    default:
      return std::format(" (wkc {})", wkc);
  }
}

}  // namespace

std::expected<std::vector<OdEntry>, std::string> SoemFieldbusDriver::readObjectDictionary(
    uint16_t slavePosition) {
  // Fine-grained locking: controlPlaneMutex_ is taken per individual SDO Info
  // transaction (and released during retrySdoInfo's back-off sleeps), so this
  // multi-second enumeration never blocks another control-plane caller for more
  // than a single transfer.
  //
  // Accepted caveat — this method does NOT hold ctx_ stable for its whole duration. Because
  // controlPlaneMutex_ is dropped between transactions, a concurrent scan()/reset()/stop() landing
  // in one of those gaps frees ctx_, and the next transaction then dereferences a dangling context:
  // a use-after-free. Within Motion Master this cannot happen — DeviceManager serialises every
  // control-plane operation on its busMutex_, held for this call's entire duration, so no
  // scan/reset can interleave. An embedder driving SoemFieldbusDriver directly MUST provide the
  // same guarantee: do not call scan()/reset()/stop() while a readObjectDictionary() is in flight
  // on another thread. This is not a wart unique to this method — it is the driver's uniform
  // lifetime contract: exchangeProcessData() carries the identical one on the RT path, reading ctx_
  // lock-free (no controlPlaneMutex_) and so relying on the caller not tearing the context down
  // mid-cycle (see closeContext's note on stopExchange ordering). "Do not destroy the context while
  // an operation is in flight" holds for every operation, not just this enumeration. The up-front
  // ctx_ check below is only a pre-init guard, not protection against this race (which is why it is
  // not re-checked inside the loop).
  {
    std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    if (!ctx_) {
      return std::unexpected("no driver — call init() first");
    }
  }
  spdlog::debug("readObjectDictionary slave {}", slavePosition);
  ec_ODlistt odList{};
  if (retrySdoInfo([&] {
        std::lock_guard<std::mutex> lock(controlPlaneMutex_);
        return ecx_readODlist(ctx_.get(), slavePosition, &odList);
      }) <= 0) {
    return std::unexpected(std::format("readODlist slave {} failed after retries", slavePosition));
  }

  std::vector<OdEntry> entries;
  entries.reserve(odList.Entries);

  for (uint16_t i = 0; i < odList.Entries; ++i) {
    if (retrySdoInfo([&] {
          std::lock_guard<std::mutex> lock(controlPlaneMutex_);
          return ecx_readODdescription(ctx_.get(), i, &odList);
        }) <= 0) {
      return std::unexpected(std::format("readODdescription slave {} index 0x{:04X} failed",
                                         slavePosition, odList.Index[i]));
    }

    // SOEM's ecx_readOEsingle issues a basic-info "Get Entry Description"
    // request (access/category/PDO only) and reads the name directly — it never
    // asks for Unit/Default/Min/Max, sidestepping the SOMANET firmware quirk
    // where those bits are echoed without payload and corrupt the name. Those
    // fields stay empty here; they are not available from this service.
    ec_OElistt oeList{};
    for (uint8_t sub = 0; sub <= odList.MaxSub[i]; ++sub) {
      if (retrySdoInfo([&] {
            std::lock_guard<std::mutex> lock(controlPlaneMutex_);
            return ecx_readOEsingle(ctx_.get(), i, sub, &odList, &oeList);
          }) <= 0) {
        spdlog::warn("Device {}: readOEsingle 0x{:04X}:{:02X} failed", slavePosition,
                     odList.Index[i], sub);
        continue;
      }
      entries.push_back(OdEntry{
          .index = odList.Index[i],
          .subindex = sub,
          .objectCode = odList.ObjectCode[i],
          .dataType = oeList.DataType[sub],
          .bitLength = oeList.BitLength[sub],
          .access = oeList.ObjAccess[sub],
          .name = oeList.Name[sub],
          .unit = std::nullopt,
          .defaultValue = std::nullopt,
          .minValue = std::nullopt,
          .maxValue = std::nullopt,
      });
    }
  }

  spdlog::debug("readObjectDictionary slave {} ok ({} entries)", slavePosition, entries.size());
  return entries;
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readSii(
    uint16_t slavePosition) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  // No position bounds check: the node layer (DeviceManager) validates every slavePosition against
  // the live device set before any driver call and 404s an unknown one, so the driver trusts the
  // position and indexes slavelist directly — the same contract every other slave-indexed accessor
  // here relies on (see DeviceManager::validatePositions and the single-device findDevice guards).
  // SOMANET EEPROMs report a large declared size (the 'size' word, ETG.1000.6 §5.4) but populate
  // only the first few hundred bytes, terminated by the END (0xFFFF) category. Reading a fixed
  // conservative window keeps the read bounded and always covers the real content; the parser
  // stops at END. ecx_siigetbyte caches a 128-byte EEPROM page internally, so this is ~64 EEPROM
  // transactions rather than one per byte. EEPROM control is handed back to the PDI afterwards.
  // Caveat: ecx_siigetbyte has no error channel — a failed/NAK'd page read returns 0x00, so a
  // mid-read fault surfaces as zero bytes (a premature END or a gap) under this success return, not
  // as an error. Acceptable here because the content the parser needs sits in the first pages.
  constexpr uint16_t kSiiBytes = 0x2000;  // 8 KiB — conservative upper bound for SII content.
  std::vector<uint8_t> sii(kSiiBytes);
  for (uint16_t i = 0; i < kSiiBytes; ++i) {
    sii[i] = ecx_siigetbyte(ctx_.get(), slavePosition, i);
  }
  // Hand EEPROM control back to the slave's PDI (writes ESC EEPROM-config register 0x0500).
  // ecx_siigetbyte took control for the master (ecx_eeprom2master) to read the EEPROM; releasing it
  // here keeps the slave's own logic from being locked out of its EEPROM — the ESC reloads SII on
  // AL state changes.
  ecx_eeprom2pdi(ctx_.get(), slavePosition);
  spdlog::debug("readSii slave {} ({} bytes)", slavePosition, sii.size());
  return sii;
}

std::expected<void, std::string> SoemFieldbusDriver::writeSii(uint16_t slavePosition,
                                                              std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  // No position bounds check: see readSii — the caller (DeviceManager) validates slavePosition
  // against the live device set first, and every slave-indexed accessor here trusts that.
  if (data.size() % 2 != 0) {
    return std::unexpected(
        std::format("SII image length {} is odd; EEPROM is written in 16-bit words", data.size()));
  }
  // Take EEPROM control from the slave's PDI, then write word-by-word from address 0. Each
  // ecx_writeeeprom is a full EEPROM transaction (mailbox-paced); a few thousand words takes a
  // moment, which is fine off the RT loop. Control is handed back to the PDI afterwards. The slave
  // adopts the new image only when its ESC reloads the EEPROM (power cycle).
  ecx_eeprom2master(ctx_.get(), slavePosition);
  const size_t words = data.size() / 2;
  for (size_t i = 0; i < words; ++i) {
    const uint16_t word = static_cast<uint16_t>(data[2 * i] | (data[2 * i + 1] << 8));
    const int wkc =
        ecx_writeeeprom(ctx_.get(), slavePosition, static_cast<uint16_t>(i), word, EC_TIMEOUTEEP);
    if (wkc <= 0) {
      ecx_eeprom2pdi(ctx_.get(), slavePosition);
      return std::unexpected(
          std::format("EEPROM write to slave {} failed at word 0x{:04X} ({} of {} words written)",
                      slavePosition, i, i, words));
    }
  }
  ecx_eeprom2pdi(ctx_.get(), slavePosition);
  spdlog::info("writeSii slave {} ({} bytes) ok — power-cycle the device to apply", slavePosition,
               data.size());
  return {};
}

std::expected<std::vector<uint8_t>, std::string> SoemFieldbusDriver::readFile(
    uint16_t slavePosition, const std::string& filename) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  spdlog::debug("FOEread slave {} '{}'", slavePosition, filename);
  constexpr int kMaxSize = 10 * 1024 * 1024;
  std::vector<uint8_t> data(kMaxSize);
  int size = kMaxSize;
  std::string name = filename;  // ecx_FOEread takes non-const char*
  int wkc =
      ecx_FOEread(ctx_.get(), slavePosition, name.data(), 0, &size, data.data(), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    // SOEM signals the FoE error kind through the negated return value, not the error list: the
    // FoE path never calls ecx_pusherror, so ecx_poperror would return nothing here. It also
    // discards the wire error code (0x800x), keeping only file-not-found vs. generic. Decode -wkc.
    std::string msg = std::format("FOEread slave {} '{}' failed", slavePosition, filename);
    msg += foeErrorDetail(wkc);
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  data.resize(size);
  spdlog::debug("FOEread slave {} '{}' ok ({} bytes)", slavePosition, filename, data.size());
  return data;
}

std::expected<void, std::string> SoemFieldbusDriver::writeFile(uint16_t slavePosition,
                                                               const std::string& filename,
                                                               std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  spdlog::debug("FOEwrite slave {} '{}' ({} bytes)", slavePosition, filename, data.size());
  std::string name = filename;  // ecx_FOEwrite takes non-const char*
  int wkc = ecx_FOEwrite(ctx_.get(), slavePosition, name.data(), 0, static_cast<int>(data.size()),
                         const_cast<uint8_t*>(data.data()), EC_TIMEOUTRXM);
  if (wkc <= 0) {
    // See readFile: the FoE error kind is in the negated return value, not the error list.
    std::string msg = std::format("FOEwrite slave {} '{}' failed", slavePosition, filename);
    msg += foeErrorDetail(wkc);
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  spdlog::debug("FOEwrite slave {} '{}' ok", slavePosition, filename);
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::readRegister(uint16_t slavePosition,
                                                                  uint16_t address,
                                                                  std::span<uint8_t> data) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  spdlog::debug("FPRD slave {} 0x{:04X} ({} bytes)", slavePosition, address, data.size());
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  int wkc = ecx_FPRD(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     data.data(), EC_TIMEOUTRET);
  if (wkc != 1) {
    std::string msg =
        "FPRD slave " + std::to_string(slavePosition) + ": wkc=" + std::to_string(wkc);
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  return {};
}

std::expected<void, std::string> SoemFieldbusDriver::writeRegister(uint16_t slavePosition,
                                                                   uint16_t address,
                                                                   std::span<const uint8_t> data) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  spdlog::debug("FPWR slave {} 0x{:04X} ({} bytes)", slavePosition, address, data.size());
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  // ecx_FPWR takes void*, not const void*
  int wkc = ecx_FPWR(&ctx_->port, configAddr, address, static_cast<uint16_t>(data.size()),
                     const_cast<uint8_t*>(data.data()), EC_TIMEOUTRET);
  if (wkc != 1) {
    std::string msg =
        "FPWR slave " + std::to_string(slavePosition) + ": wkc=" + std::to_string(wkc);
    spdlog::debug("{}", msg);
    return std::unexpected(msg);
  }
  return {};
}

namespace {
// ESC watchdog registers (ETG.1000.4 §Table 32). The divider (0x0400) is the common time base
// for both the process-data (0x0420) and PDI (0x0410) watchdogs; the time register counts ticks
// of that base. One tick = 40 ns × (divider + 2). A zero time register disables the watchdog.
constexpr uint16_t kWatchdogDividerReg = 0x0400;
constexpr uint16_t kWatchdogTimePdReg = 0x0420;
constexpr uint16_t kWatchdogStatusPdReg = 0x0440;  // bit 0: 1 = running, 0 = expired.
constexpr int64_t kEscClockNs = 40;  // ESC reference clock period feeding the watchdog divider.

int64_t watchdogTickNs(uint16_t divider) {
  return kEscClockNs * (static_cast<int64_t>(divider) + 2);
}

mm::comm::ProcessDataWatchdogConfig decodeWatchdog(uint16_t divider, uint16_t ticks,
                                                   uint16_t status) {
  return {.enabled = ticks != 0,
          .running = (status & 0x0001u) != 0,
          .timeout = std::chrono::nanoseconds(watchdogTickNs(divider) * ticks),
          .divider = divider,
          .ticks = ticks};
}
}  // namespace

std::expected<ProcessDataWatchdogConfig, std::string> SoemFieldbusDriver::processDataWatchdog(
    uint16_t slavePosition) {
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  uint16_t divider = 0;
  uint16_t ticks = 0;
  // ESC registers are little-endian on the wire; reading straight into a uint16_t is correct on
  // the x86 host (matches the readDiagnostics DL-status read above).
  if (ecx_FPRD(&ctx_->port, configAddr, kWatchdogDividerReg, sizeof(divider), &divider,
               EC_TIMEOUTRET) != 1) {
    return std::unexpected(
        std::format("FPRD slave {} watchdog divider (0x0400) failed", slavePosition));
  }
  if (ecx_FPRD(&ctx_->port, configAddr, kWatchdogTimePdReg, sizeof(ticks), &ticks, EC_TIMEOUTRET) !=
      1) {
    return std::unexpected(
        std::format("FPRD slave {} PD watchdog time (0x0420) failed", slavePosition));
  }
  uint16_t status = 0;
  if (ecx_FPRD(&ctx_->port, configAddr, kWatchdogStatusPdReg, sizeof(status), &status,
               EC_TIMEOUTRET) != 1) {
    return std::unexpected(
        std::format("FPRD slave {} PD watchdog status (0x0440) failed", slavePosition));
  }
  return decodeWatchdog(divider, ticks, status);
}

std::expected<ProcessDataWatchdogConfig, std::string> SoemFieldbusDriver::setProcessDataWatchdog(
    uint16_t slavePosition, std::chrono::nanoseconds timeout) {
  if (timeout < std::chrono::nanoseconds::zero()) {
    return std::unexpected("watchdog timeout must not be negative");
  }
  std::lock_guard<std::mutex> lock(controlPlaneMutex_);
  if (!ctx_) {
    return std::unexpected("no driver — call init() first");
  }
  uint16_t configAddr = ctx_->slavelist[slavePosition].configadr;
  // Read the existing divider and leave it untouched — it is shared with the PDI watchdog, so we
  // only scale the process-data time register against whatever time base the device already uses.
  uint16_t divider = 0;
  if (ecx_FPRD(&ctx_->port, configAddr, kWatchdogDividerReg, sizeof(divider), &divider,
               EC_TIMEOUTRET) != 1) {
    return std::unexpected(
        std::format("FPRD slave {} watchdog divider (0x0400) failed", slavePosition));
  }
  const int64_t tickNs = watchdogTickNs(divider);
  // Round to the nearest tick. A zero timeout maps to zero ticks (watchdog disabled).
  int64_t ticks64 = (timeout.count() + tickNs / 2) / tickNs;
  if (ticks64 > 0xFFFF) {
    return std::unexpected(std::format(
        "watchdog timeout {} ns exceeds the maximum {} ns representable with this device's divider "
        "({} ns per tick)",
        timeout.count(), tickNs * 0xFFFF, tickNs));
  }
  // A non-zero request that rounds to zero would silently disable the watchdog — floor it to one
  // tick so "set a tiny timeout" never reads as "disable".
  if (timeout.count() > 0 && ticks64 == 0) {
    ticks64 = 1;
  }
  auto ticks = static_cast<uint16_t>(ticks64);
  if (ecx_FPWR(&ctx_->port, configAddr, kWatchdogTimePdReg, sizeof(ticks), &ticks, EC_TIMEOUTRET) !=
      1) {
    return std::unexpected(
        std::format("FPWR slave {} PD watchdog time (0x0420) failed", slavePosition));
  }
  spdlog::info("Device {}: process-data watchdog set to {} ns ({} ticks @ {} ns/tick)",
               slavePosition, tickNs * ticks, ticks, tickNs);
  // Read back the status so the caller sees whether the freshly programmed watchdog is running
  // (it counts down only once process data is flowing).
  uint16_t status = 0;
  if (ecx_FPRD(&ctx_->port, configAddr, kWatchdogStatusPdReg, sizeof(status), &status,
               EC_TIMEOUTRET) != 1) {
    return std::unexpected(
        std::format("FPRD slave {} PD watchdog status (0x0440) failed", slavePosition));
  }
  return decodeWatchdog(divider, ticks, status);
}

namespace {

// BOOT and PRE-OP use different mailbox sizes (e.g. 1024 vs 128 bytes on Integro
// devices). After a firmware download the slave context still holds BOOT SM parameters;
// without reprogramming them here an INIT→PRE-OP transition would reuse stale BOOT
// values and break mailbox communication. SOEM does not do this automatically.
void updateMailboxSyncManagers(ecx_contextt* ctx, uint16_t slave, EtherCatState targetState) {
  if (targetState == EtherCatState::Boot) {
    uint32_t data = ecx_readeeprom(ctx, slave, ECT_SII_BOOTRXMBX, EC_TIMEOUTEEP);
    ctx->slavelist[slave].SM[0].StartAddr = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].SM[0].SMlength = static_cast<uint16_t>(HI_WORD(data));
    ctx->slavelist[slave].mbx_wo = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].mbx_l = static_cast<uint16_t>(HI_WORD(data));

    data = ecx_readeeprom(ctx, slave, ECT_SII_BOOTTXMBX, EC_TIMEOUTEEP);
    ctx->slavelist[slave].SM[1].StartAddr = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].SM[1].SMlength = static_cast<uint16_t>(HI_WORD(data));
    ctx->slavelist[slave].mbx_ro = static_cast<uint16_t>(LO_WORD(data));
    ctx->slavelist[slave].mbx_rl = static_cast<uint16_t>(HI_WORD(data));

    spdlog::info("Device {}: BOOT mailbox - write 0x{:04X}/{}, read 0x{:04X}/{}", slave,
                 ctx->slavelist[slave].mbx_wo, ctx->slavelist[slave].mbx_l,
                 ctx->slavelist[slave].mbx_ro, ctx->slavelist[slave].mbx_rl);

    int wkc0 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[0], EC_TIMEOUTRET);
    int wkc1 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM1, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[1], EC_TIMEOUTRET);
    if (wkc0 != 1 || wkc1 != 1) {
      // A failed SM write leaves stale/invalid mailbox config in the ESC; the slave then
      // refuses the state change — often silently (AL status 0x0000) rather than with a
      // mailbox-config error code. Surface it so it is not mistaken for a slave-side fault.
      spdlog::warn(
          "Device {}: BOOT mailbox SM write incomplete (SM0 wkc={}, SM1 wkc={}) — "
          "slave may refuse the transition",
          slave, wkc0, wkc1);
    }

    // Reprogramming the mailbox sync managers re-initializes the slave's mailbox, which resets
    // the slave's expected mailbox sequence counter. The master's counter (mbx_cnt) must be
    // reset to match, otherwise the first FoE exchange on the fresh BOOT mailbox is rejected and
    // every subsequent read desyncs (seen as wkc 0x5 then 0x0/0x3 "unexpected mailbox"). On a
    // first flash after a bus scan the counter happens to line up, but on a re-entry into BOOT it
    // carries a stale PRE-OP value and wedges the mailbox, breaking repeated enter/exit BOOT.
    ctx->slavelist[slave].mbx_cnt = 0;
  } else if (targetState == EtherCatState::PreOp) {
    // PRE-OP SMs come from the standard SII mailbox entries, not the BOOT entries.
    ecx_readeeprom1(ctx, slave, ECT_SII_RXMBXADR);
    uint32_t eedat = ecx_readeeprom2(ctx, slave, EC_TIMEOUTEEP);
    ctx->slavelist[slave].mbx_wo = static_cast<uint16_t>(LO_WORD(etohl(eedat)));
    ctx->slavelist[slave].mbx_l = static_cast<uint16_t>(HI_WORD(etohl(eedat)));
    ctx->slavelist[slave].SM[0].StartAddr = ctx->slavelist[slave].mbx_wo;
    ctx->slavelist[slave].SM[0].SMlength = ctx->slavelist[slave].mbx_l;

    ecx_readeeprom1(ctx, slave, ECT_SII_TXMBXADR);
    eedat = ecx_readeeprom2(ctx, slave, EC_TIMEOUTEEP);
    ctx->slavelist[slave].mbx_ro = static_cast<uint16_t>(LO_WORD(etohl(eedat)));
    ctx->slavelist[slave].mbx_rl = static_cast<uint16_t>(HI_WORD(etohl(eedat)));
    if (ctx->slavelist[slave].mbx_rl == 0) {
      ctx->slavelist[slave].mbx_rl = ctx->slavelist[slave].mbx_l;
    }
    ctx->slavelist[slave].SM[1].StartAddr = ctx->slavelist[slave].mbx_ro;
    ctx->slavelist[slave].SM[1].SMlength = ctx->slavelist[slave].mbx_rl;

    spdlog::info("Device {}: PRE-OP mailbox - write 0x{:04X}/{}, read 0x{:04X}/{}", slave,
                 ctx->slavelist[slave].mbx_wo, ctx->slavelist[slave].mbx_l,
                 ctx->slavelist[slave].mbx_ro, ctx->slavelist[slave].mbx_rl);

    int wkc0 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM0, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[0], EC_TIMEOUTRET);
    int wkc1 = ecx_FPWR(&ctx->port, ctx->slavelist[slave].configadr, ECT_REG_SM1, sizeof(ec_smt),
                        &ctx->slavelist[slave].SM[1], EC_TIMEOUTRET);
    if (wkc0 != 1 || wkc1 != 1) {
      // A failed SM write leaves stale/invalid mailbox config in the ESC; the slave then
      // refuses the state change — often silently (AL status 0x0000) rather than with a
      // mailbox-config error code. Surface it so it is not mistaken for a slave-side fault.
      spdlog::warn(
          "Device {}: PRE-OP mailbox SM write incomplete (SM0 wkc={}, SM1 wkc={}) — "
          "slave may refuse the transition",
          slave, wkc0, wkc1);
    }

    // Same reset as the BOOT branch: this PRE-OP reprogramming only runs for a slave we earlier
    // drove into BOOT (firmware download), so mbx_cnt still holds the counter advanced by the
    // BOOT-mode FoE transfers. Reprogramming the SMs re-initializes the slave's mailbox and
    // resets its expected sequence counter, so the master must reset to match — otherwise the
    // first CoE SDO on the fresh PRE-OP mailbox desyncs.
    ctx->slavelist[slave].mbx_cnt = 0;
  }

  // Both EEPROM read paths above (the combined ecx_readeeprom in the BOOT branch and the
  // split ecx_readeeprom1/2 in the PRE-OP branch) call ecx_eeprom2master and leave EEPROM
  // control with the master — neither restores it. Some slaves' PDI needs EEPROM access to
  // complete the state change and will silently stay in INIT (AL status 0x0000) if locked
  // out, so hand control back to the PDI before the caller issues writestate. This mirrors
  // ecx_config_init, which also leaves EEPROM with the PDI before requesting a state change.
  ecx_eeprom2pdi(ctx, slave);
}

}  // namespace

void SoemFieldbusDriver::transitionToState(const std::vector<uint16_t>& positions,
                                           std::optional<EtherCatState> requiredState,
                                           EtherCatState targetState,
                                           std::chrono::steady_clock::duration timeout,
                                           std::chrono::steady_clock::duration resendInterval,
                                           std::function<void()> tick,
                                           std::function<bool()> shouldAbort) {
  {
    std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    if (!ctx_) {
      spdlog::error("transitionToState: no driver — call init() first");
      return;
    }
  }
  const auto targetRaw = static_cast<uint16_t>(targetState);

  // controlPlaneMutex_ is taken only around the discrete socket transactions below and
  // is never held across the poll sleep or the tick()/shouldAbort() callbacks, so
  // a multi-second transition does not block other control-plane callers (and the
  // PDO tick, being lock-free, never contends here).

  // A slave sitting in INIT can silently ignore the next state request — staying in INIT with the
  // error bit clear and AL status code 0x0000 — when its PDI has latched a stale internal state
  // (e.g. it lost its previous master to a cable unplugged mid-cycle and never cleanly recovered).
  // An explicit INIT+ACK cycle re-engages the PDI before the target state is requested; it is
  // harmless to a healthy slave, which just re-affirms INIT. Only meaningful when leaving INIT, so
  // skip it when INIT itself is the target. The resend loop below only ACKs slaves that have raised
  // the error bit, so a cleanly-latched INIT (no error bit) would otherwise never get this kick.
  // Issued under the lock, then released so the slave can settle without the lock held across the
  // sleep.
  if (targetState != EtherCatState::Init) {
    std::vector<uint16_t> kicked;
    {
      std::lock_guard<std::mutex> lock(controlPlaneMutex_);
      ecx_readstate(ctx_.get());
      for (uint16_t pos : positions) {
        const uint16_t stateClean = ctx_->slavelist[pos].state & 0x000Fu;
        if ((!requiredState || stateClean == static_cast<uint16_t>(*requiredState)) &&
            stateClean == static_cast<uint16_t>(EtherCatState::Init)) {
          ctx_->slavelist[pos].state = static_cast<uint16_t>(EtherCatState::Init) | EC_STATE_ACK;
          ecx_writestate(ctx_.get(), pos);
          kicked.push_back(pos);
        }
      }
    }
    if (!kicked.empty()) {
      spdlog::debug("Issued INIT+ACK to {} slave(s) in INIT before requesting state 0x{:02X}",
                    kicked.size(), targetRaw);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
  }

  std::set<uint16_t> pending;
  {
    std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    ecx_readstate(ctx_.get());
    for (uint16_t pos : positions) {
      uint16_t state = ctx_->slavelist[pos].state;
      uint16_t stateClean = state & 0x000Fu;
      if (!requiredState || stateClean == static_cast<uint16_t>(*requiredState)) {
        // Mailbox sync managers only need reprogramming when leaving INIT, and only in
        // two cases: entering BOOT (which uses larger BOOT-specific mailbox SMs), or
        // entering PRE-OP from a slave that still holds stale BOOT SMs (i.e. one we drove
        // into BOOT earlier, e.g. for a firmware download). On a plain fresh-scan
        // INIT→PRE-OP the SMs ecx_config_init already programmed are correct, and touching
        // them here is both redundant and harmful (it seizes EEPROM from the slave's PDI).
        if (stateClean == static_cast<uint16_t>(EtherCatState::Init)) {
          if (targetState == EtherCatState::Boot) {
            updateMailboxSyncManagers(ctx_.get(), pos, EtherCatState::Boot);
            bootMailboxSlaves_.insert(pos);
          } else if (targetState == EtherCatState::PreOp && bootMailboxSlaves_.contains(pos)) {
            updateMailboxSyncManagers(ctx_.get(), pos, EtherCatState::PreOp);
            bootMailboxSlaves_.erase(pos);
          }
        }
        ctx_->slavelist[pos].state = targetRaw;
        ecx_writestate(ctx_.get(), pos);
        pending.insert(pos);
      }
    }
  }

  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto lastResend = std::chrono::steady_clock::now();
  bool aborted = false;
  // Slaves whose error bit we have already reported. The AL status code a slave latches the
  // instant it raises the error bit is often more specific than the one it settles on by the
  // timeout, so log it once on first sight rather than only at the end.
  std::set<uint16_t> errorReported;

  while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
    if (shouldAbort && shouldAbort()) {
      aborted = true;
      break;
    }

    // When a tick is provided (e.g. SAFE-OP → OP), call it at ~1 ms intervals
    // so process data keeps flowing and the SM watchdog does not fire.
    if (tick) {
      auto pollEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      auto nextTick = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() < pollEnd) {
        tick();
        nextTick += std::chrono::milliseconds(1);
        auto now = std::chrono::steady_clock::now();
        if (nextTick > now) {
          std::this_thread::sleep_for(nextTick - now);
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Locked for the rest of this iteration (state read, evaluation, resend) —
    // released at the loop-body scope exit before the next tick()/sleep.
    std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    ecx_readstate(ctx_.get());

    for (auto it = pending.begin(); it != pending.end();) {
      uint16_t pos = *it;
      uint16_t state = ctx_->slavelist[pos].state;
      uint16_t alStatusCode = ctx_->slavelist[pos].ALstatuscode;
      // First time we see the error bit for this slave, report the AL status + code immediately
      // (the transient code is the most diagnostic). Logged once so a slow transition does not
      // spam the same line every poll.
      if ((state & EC_STATE_ERROR) && !errorReported.contains(pos)) {
        errorReported.insert(pos);
        std::string_view name = alStatusCodeName(alStatusCode);
        spdlog::warn(
            "Device {}: error bit set while reaching state 0x{:02X} — AL status 0x{:04X}, "
            "code 0x{:04X} ({})",
            pos, targetRaw, state, alStatusCode,
            name.empty() ? "unknown — not a known AL status code" : name);
      }
      // Exact match required: OP+ERROR (0x18) must not pass as OP (0x08).
      if (state == targetRaw) {
        spdlog::info("Device {}: reached state 0x{:02X}", pos, targetRaw);
        it = pending.erase(it);
      } else if ((state & EC_STATE_ERROR) && isAlStatusCodeTerminal(alStatusCode)) {
        // Slave reported a terminal AL status code — retrying the same writestate
        // cannot succeed. Drop it from the pending set immediately so the caller
        // gets fast feedback instead of spinning until timeout.
        spdlog::warn("Device {}: terminal AL status 0x{:04X}; cannot reach state 0x{:02X}", pos,
                     alStatusCode, targetRaw);
        it = pending.erase(it);
      } else {
        ++it;
      }
    }

    if (std::chrono::steady_clock::now() - lastResend > resendInterval) {
      for (uint16_t pos : pending) {
        uint16_t state = ctx_->slavelist[pos].state;
        if (state & EC_STATE_ERROR) {
          ctx_->slavelist[pos].state = (state & 0x000Fu) | EC_STATE_ACK;
          ecx_writestate(ctx_.get(), pos);
        }
        ctx_->slavelist[pos].state = targetRaw;
        ecx_writestate(ctx_.get(), pos);
      }
      lastResend = std::chrono::steady_clock::now();
    }
  }

  if (!aborted) {
    std::lock_guard<std::mutex> lock(controlPlaneMutex_);
    for (uint16_t pos : pending) {
      // Report both registers: the AL Status (0x0130 — actual state + error bit, so the slave's
      // real position is visible rather than just the requested target) and the AL Status Code
      // (0x0134). Decode the code's name, flagging it as unknown when it falls outside the
      // standard table — a code with no name is usually vendor-specific or a stale read, which
      // is itself diagnostic and must not be mistaken for a defined error.
      uint16_t alStatus = ctx_->slavelist[pos].state;
      uint16_t alStatusCode = ctx_->slavelist[pos].ALstatuscode;
      std::string_view name = alStatusCodeName(alStatusCode);
      spdlog::error(
          "Device {}: failed to reach state 0x{:02X} — AL status 0x{:04X}, code 0x{:04X} ({})", pos,
          targetRaw, alStatus, alStatusCode,
          name.empty() ? "unknown — not a known AL status code" : name);
    }
  }
}

}  // namespace mm::comm::soem
