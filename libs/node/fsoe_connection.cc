#include "node/fsoe_connection.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "node/device.h"
#include "node/device_manager.h"

namespace mm::node {
namespace {

/// Octets in a Safety PDU carrying @p safeDataOctets SafeData octets, or 0 if that length is not
/// one the standard allows.
uint16_t frameOctets(uint16_t safeDataOctets) {
  return mm::etg::FsoeFrameLayout(safeDataOctets).size();
}

/// The SafeData length a Safety PDU of @p octets carries, or 0 if no legal length gives that size.
uint16_t safeDataOctetsOf(uint16_t octets) {
  if (octets == mm::etg::kFsoeMinPduSize) {
    return 1;
  }
  if (octets < mm::etg::kFsoeMinPduSize || (octets % 2) == 0) {
    return 0;
  }
  const auto safeData = static_cast<uint16_t>((octets - 3) / 2);
  return frameOctets(safeData) == octets ? safeData : 0;
}

/// Sums the bit widths of one PDO's entries.
uint32_t mappedBits(const PdoMappingObject& pdo) {
  return std::accumulate(
      pdo.entries.begin(), pdo.entries.end(), uint32_t{0},
      [](uint32_t sum, const PdoMappingEntry& entry) { return sum + entry.bitLength; });
}

const PdoMappingObject* findPdo(const std::vector<PdoMappingObject>& pdos, uint16_t index) {
  const auto it = std::ranges::find_if(
      pdos, [index](const PdoMappingObject& pdo) { return pdo.pdoIndex == index; });
  return it == pdos.end() ? nullptr : &*it;
}

}  // namespace

FsoeConnection::FsoeConnection(FsoeConnectionConfig config, mm::etg::FsoeMaster master,
                               std::vector<FrameField> outputFields, uint32_t inputByteOffset,
                               uint64_t processImageGeneration, uint64_t topologyGeneration)
    : config_(std::move(config)),
      master_(std::move(master)),
      outputFields_(std::move(outputFields)),
      inputByteOffset_(inputByteOffset),
      inputByteLength_(master_.rxPduSize()),
      processImageGeneration_(processImageGeneration),
      topologyGeneration_(topologyGeneration) {
  state_.safeInputsLength = master_.config().safeInputsLen;
  state_.safeOutputsLength = master_.config().safeOutputsLen;
}

std::expected<std::unique_ptr<FsoeConnection>, std::string> FsoeConnection::open(
    DeviceManager& deviceManager, const FsoeConnectionConfig& config) {
  const auto device = deviceManager.deviceAt(config.slavePosition);
  if (!device) {
    return std::unexpected(std::format("device {} not found", config.slavePosition));
  }
  if (!deviceManager.processDataConfigured()) {
    return std::unexpected(
        "process data is not configured; map the bus before opening an FSoE connection");
  }

  // Read the mapping fresh rather than use the cached flat view: the flat view does not say which
  // PDO an entry belongs to, and the safety frame is defined by its PDO, not by its objects.
  auto mapping = device->readPdoMapping();
  if (!mapping) {
    return std::unexpected(std::format("cannot read the PDO mapping of device {}: {}",
                                       config.slavePosition, mapping.error()));
  }

  const PdoMappingObject* master = findPdo(mapping->outputs, config.rxPdoIndex);
  const PdoMappingObject* slave = findPdo(mapping->inputs, config.txPdoIndex);
  if (master == nullptr || slave == nullptr) {
    return std::unexpected(std::format(
        "device {} does not carry PDO {:#06x} out and {:#06x} back; it is either not a safe drive "
        "or its safety PDOs are not assigned",
        config.slavePosition, config.rxPdoIndex, config.txPdoIndex));
  }

  const uint32_t masterBits = mappedBits(*master);
  const uint32_t slaveBits = mappedBits(*slave);
  if ((masterBits % 8) != 0 || (slaveBits % 8) != 0) {
    return std::unexpected(
        std::format("PDO {:#06x} / {:#06x} are {} / {} bits; a Safety PDU is a "
                    "whole number of octets",
                    config.rxPdoIndex, config.txPdoIndex, masterBits, slaveBits));
  }
  const uint16_t safeOutputsLen = safeDataOctetsOf(static_cast<uint16_t>(masterBits / 8));
  const uint16_t safeInputsLen = safeDataOctetsOf(static_cast<uint16_t>(slaveBits / 8));
  if (safeOutputsLen == 0 || safeInputsLen == 0) {
    return std::unexpected(std::format(
        "PDO {:#06x} / {:#06x} are {} / {} octets, which is not the size of any Safety PDU "
        "(a frame is 1 + 2n + 2 octets)",
        config.rxPdoIndex, config.txPdoIndex, masterBits / 8, slaveBits / 8));
  }
  if (safeInputsLen > kFsoeMaxSafeDataOctets || safeOutputsLen > kFsoeMaxSafeDataOctets) {
    return std::unexpected(
        std::format("this connection carries {} / {} SafeData octets; at most "
                    "{} are supported here",
                    safeOutputsLen, safeInputsLen, kFsoeMaxSafeDataOctets));
  }
  if (safeOutputsLen > kFsoeMaxSettableSafeOutputs) {
    return std::unexpected(
        std::format("SafeOutputs of {} octets cannot be set through this API yet; the limit is {}",
                    safeOutputsLen, kFsoeMaxSettableSafeOutputs));
  }

  // Locate every object of the master frame, in frame order. The offsets come from the published
  // image, so what is bound here is where the octets will actually go.
  std::vector<FrameField> fields;
  fields.reserve(master->entries.size());
  uint16_t frameOffset = 0;
  for (const PdoMappingEntry& entry : master->entries) {
    if (entry.index == 0) {
      return std::unexpected(std::format(
          "PDO {:#06x} has an alignment gap at octet {}; every octet of a master frame has to be "
          "writable, so the whole frame must be mapped to objects",
          config.rxPdoIndex, frameOffset));
    }
    if ((entry.bitLength % 8) != 0 || entry.bitLength > 16) {
      return std::unexpected(std::format(
          "object {:#06x}:{:02x} of PDO {:#06x} is {} bits; the master frame must be mapped as 8- "
          "or 16-bit objects",
          entry.index, entry.subindex, config.rxPdoIndex, entry.bitLength));
    }
    const auto spec =
        deviceManager.pdoSampleSpec(config.slavePosition, entry.index, entry.subindex);
    if (!spec || !spec->isOutput) {
      return std::unexpected(
          std::format("object {:#06x}:{:02x} of PDO {:#06x} is not in the "
                      "published output image",
                      entry.index, entry.subindex, config.rxPdoIndex));
    }
    const auto typeDefault = defaultValueForDataType(spec->dataType);
    const bool isSigned =
        std::holds_alternative<int8_t>(typeDefault) || std::holds_alternative<int16_t>(typeDefault);
    const bool isUnsigned = std::holds_alternative<uint8_t>(typeDefault) ||
                            std::holds_alternative<uint16_t>(typeDefault);
    if (!isSigned && !isUnsigned) {
      return std::unexpected(std::format(
          "object {:#06x}:{:02x} of PDO {:#06x} has data type {:#06x}; the master frame must be "
          "mapped as 8- or 16-bit integers",
          entry.index, entry.subindex, config.rxPdoIndex, spec->dataType));
    }
    fields.push_back(FrameField{
        .index = entry.index,
        .subindex = entry.subindex,
        .frameOffset = frameOffset,
        .octets = static_cast<uint8_t>(entry.bitLength / 8),
        .isSigned = isSigned,
    });
    frameOffset = static_cast<uint16_t>(frameOffset + entry.bitLength / 8);
  }

  // The slave frame is read straight out of the input image, so all it needs is where it starts.
  const auto firstInput = slave->entries.empty() ? PdoMappingEntry{} : slave->entries.front();
  const auto inputSpec =
      deviceManager.pdoSampleSpec(config.slavePosition, firstInput.index, firstInput.subindex);
  if (!inputSpec || inputSpec->isOutput) {
    return std::unexpected(
        std::format("the first object of PDO {:#06x} is not in the published "
                    "input image",
                    config.txPdoIndex));
  }
  if ((inputSpec->bitOffset % 8) != 0) {
    return std::unexpected(
        std::format("PDO {:#06x} starts at bit {}; a Safety PDU has to be "
                    "byte-aligned in the process image",
                    config.txPdoIndex, inputSpec->bitOffset));
  }

  auto master_ = mm::etg::FsoeMaster::create({
      .safeOutputsLen = safeOutputsLen,
      .safeInputsLen = safeInputsLen,
      .slaveAddress = config.slaveAddress,
      .connectionId = config.connectionId,
      .watchdogMs = config.watchdogMs,
      .applicationParameters = config.applicationParameters,
      .initialSessionId = config.initialSessionId,
  });
  if (!master_) {
    return std::unexpected(master_.error());
  }

  // Not std::make_unique: the constructor is private, and it stays private so a connection can only
  // come from a successful open.
  return std::unique_ptr<FsoeConnection>(new FsoeConnection(
      config, std::move(*master_), std::move(fields), inputSpec->bitOffset / 8,
      deviceManager.processImageGeneration(), deviceManager.topologyGeneration()));
}

void FsoeConnection::step(DeviceManager& deviceManager, uint32_t dtUs) {
  if (!active_.load(std::memory_order_acquire)) {
    return;
  }

  // The offsets were resolved against one image and one device set. Either changing means they may
  // now name something else, so the connection stops driving the frame and says so. The drive's own
  // watchdog takes its outputs to the safe state, which is the right outcome for a master that no
  // longer knows where its frame is.
  const bool bound = deviceManager.processImageGeneration() == processImageGeneration_ &&
                     deviceManager.topologyGeneration() == topologyGeneration_;
  Device* device = bound ? deviceManager.findDevice(config_.slavePosition) : nullptr;
  if (device == nullptr) {
    FsoeConnectionState unbound = state_;
    unbound.bound = false;
    unbound.inputsValid = false;
    unbound.safeInputs = {};
    publish(unbound);
    return;
  }

  if (resetRequested_.exchange(false, std::memory_order_relaxed)) {
    master_.resetConnection();
  }
  master_.setDataCommand(
      static_cast<mm::etg::FsoeCommand>(dataCommand_.load(std::memory_order_relaxed)));

  std::array<uint8_t, kFsoeMaxSettableSafeOutputs> outputs{};
  const uint64_t staged = safeOutputs_.load(std::memory_order_relaxed);
  std::memcpy(outputs.data(), &staged, outputs.size());
  master_.setSafeOutputs(std::span(outputs).first(master_.config().safeOutputsLen));

  const std::span<const uint8_t> image = deviceManager.cycleInputs();
  const bool haveFrame = static_cast<size_t>(inputByteOffset_) + inputByteLength_ <= image.size();
  const std::span<const uint8_t> rx =
      haveFrame ? image.subspan(inputByteOffset_, inputByteLength_) : std::span<const uint8_t>{};

  const auto result = master_.cycle(rx, dtUs);
  ++cycles_;
  if (result) {
    if (result->rxAccepted) {
      ++framesAccepted_;
    }
    if (result->fault != mm::etg::FsoeError::None) {
      ++faults_;
    }
  }

  writeFrame(*device);

  FsoeConnectionState fresh;
  fresh.state = master_.state();
  fresh.bound = true;
  fresh.inputsValid = master_.inputsValid();
  fresh.fault = master_.faultReason();
  fresh.peerFaultCode = master_.peerFaultCode();
  fresh.sessionId = master_.sessionId();
  fresh.dataCommand =
      static_cast<mm::etg::FsoeCommand>(dataCommand_.load(std::memory_order_relaxed));
  fresh.cycles = cycles_;
  fresh.framesAccepted = framesAccepted_;
  fresh.faults = faults_;
  fresh.safeInputsLength = master_.config().safeInputsLen;
  fresh.safeOutputsLength = master_.config().safeOutputsLen;
  std::ranges::copy(master_.safeInputs(), fresh.safeInputs.begin());
  std::ranges::copy(std::span(outputs).first(fresh.safeOutputsLength), fresh.safeOutputs.begin());
  publish(fresh);
}

void FsoeConnection::writeFrame(Device& device) {
  const std::span<const uint8_t> frame = master_.txPdu();
  for (const FrameField& field : outputFields_) {
    if (static_cast<size_t>(field.frameOffset) + field.octets > frame.size()) {
      continue;
    }
    uint16_t value = frame[field.frameOffset];
    if (field.octets == 2) {
      value = static_cast<uint16_t>(value | (frame[field.frameOffset + 1] << 8));
    }
    // The declared type has to match exactly: setValue refuses a value of the wrong alternative
    // rather than coercing it, which is what keeps an RT write free of the variant machinery.
    if (field.octets == 1) {
      if (field.isSigned) {
        device.setValue<int8_t>(field.index, field.subindex, static_cast<int8_t>(value));
      } else {
        device.setValue<uint8_t>(field.index, field.subindex, static_cast<uint8_t>(value));
      }
    } else {
      if (field.isSigned) {
        device.setValue<int16_t>(field.index, field.subindex, static_cast<int16_t>(value));
      } else {
        device.setValue<uint16_t>(field.index, field.subindex, value);
      }
    }
  }
}

bool FsoeConnection::setDataCommand(mm::etg::FsoeCommand command) {
  if (command != mm::etg::FsoeCommand::ProcessData &&
      command != mm::etg::FsoeCommand::FailSafeData) {
    return false;
  }
  dataCommand_.store(static_cast<uint8_t>(command), std::memory_order_relaxed);
  return true;
}

bool FsoeConnection::setSafeOutputs(std::span<const uint8_t> data) {
  if (data.size() > kFsoeMaxSettableSafeOutputs || data.size() > master_.config().safeOutputsLen) {
    return false;
  }
  std::array<uint8_t, kFsoeMaxSettableSafeOutputs> octets{};
  std::ranges::copy(data, octets.begin());
  uint64_t packed = 0;
  std::memcpy(&packed, octets.data(), octets.size());
  safeOutputs_.store(packed, std::memory_order_relaxed);
  return true;
}

bool FsoeConnection::setControl(mm::etg::SdpControl control) {
  std::array<uint8_t, kFsoeMaxSettableSafeOutputs> octets{};
  uint64_t packed = safeOutputs_.load(std::memory_order_relaxed);
  std::memcpy(octets.data(), &packed, octets.size());
  mm::etg::sdpEncodeControl(control, octets);
  std::memcpy(&packed, octets.data(), octets.size());
  safeOutputs_.store(packed, std::memory_order_relaxed);
  return true;
}

void FsoeConnection::publish(const FsoeConnectionState& fresh) {
  const uint32_t seq = stateSeq_.load(std::memory_order_relaxed);
  stateSeq_.store(seq + 1, std::memory_order_release);  // odd: a copy in progress
  std::atomic_thread_fence(std::memory_order_release);
  state_ = fresh;
  stateSeq_.store(seq + 2, std::memory_order_release);  // even again: the copy is whole
}

FsoeConnectionState FsoeConnection::state() const {
  for (int attempt = 0;; ++attempt) {
    const uint32_t before = stateSeq_.load(std::memory_order_acquire);
    if ((before % 2) == 0) {
      FsoeConnectionState copy = state_;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (stateSeq_.load(std::memory_order_relaxed) == before) {
        return copy;
      }
    }
    // The writer is one memcpy long and runs once per cycle, so this retries at most once in
    // practice. Yielding after a few attempts bounds the cost if the reader is unlucky or the
    // machine is loaded.
    if (attempt > 8) {
      std::this_thread::yield();
    }
  }
}

}  // namespace mm::node
