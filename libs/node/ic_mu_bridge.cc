#include "node/ic_mu_bridge.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace mm::node {

namespace {

constexpr auto kEncoderRegisterTimeout = std::chrono::seconds(5);
constexpr auto kEncoderRegisterPollInterval = std::chrono::milliseconds(20);
constexpr auto kIcMuCommandTimeout = std::chrono::seconds(5);
constexpr auto kIcMuCommandPollInterval = std::chrono::milliseconds(20);

}  // namespace

std::expected<uint8_t, std::string> readIcMuRegister(SomanetDrive& drive,
                                                     somanet::EncoderOrdinal encoder,
                                                     uint8_t registerAddress,
                                                     const std::stop_token& stop) {
  auto result = drive.readEncoderRegister(encoder, registerAddress,
                                          {.timeout = kEncoderRegisterTimeout,
                                           .pollInterval = kEncoderRegisterPollInterval,
                                           .stop = stop});
  if (!result) {
    return std::unexpected(result.error());
  }
  if (!result->value) {
    return std::unexpected(std::format("{}: the drive reported no value for register 0x{:02X}",
                                       somanet::toString(encoder), registerAddress));
  }
  return *result->value;
}

std::expected<void, std::string> checkIcMuBank(SomanetDrive& drive, somanet::EncoderOrdinal encoder,
                                               const std::stop_token& stop) {
  constexpr uint8_t kBankSelectRegister = 0x40;
  auto bank = readIcMuRegister(drive, encoder, kBankSelectRegister, stop);
  if (!bank) {
    return std::unexpected(bank.error());
  }
  if (*bank != 0) {
    return std::unexpected(std::format(
        "register 0x40 (BANKSEL) of the iC-MU holds 0x{:02X}, not 0, so its configuration is not "
        "in reach; power the encoder off and on, which sets it to 0",
        *bank));
  }
  return {};
}

std::expected<void, std::string> writeIcMuRegister(SomanetDrive& drive,
                                                   somanet::EncoderOrdinal encoder,
                                                   somanet::IcMuRegisterWrite write,
                                                   const std::stop_token& stop) {
  if (stop.stop_requested()) {
    return std::unexpected("cancelled");
  }
  auto result = drive.writeEncoderRegister(encoder, write.address, write.value,
                                           {.timeout = kEncoderRegisterTimeout,
                                            .pollInterval = kEncoderRegisterPollInterval,
                                            .stop = stop});
  if (!result) {
    return std::unexpected(result.error());
  }
  if (write.address == somanet::kIcMuCommandRegister) {
    return {};
  }
  if (result->value != write.value) {
    return std::unexpected(std::format(
        "{}: wrote 0x{:02X} into register 0x{:02X}, and the encoder reports {}",
        somanet::toString(encoder), write.value, write.address,
        result->value ? std::format("0x{:02X}", *result->value) : std::string("no value")));
  }
  return {};
}

std::expected<void, std::string> writeIcMuRegisters(
    SomanetDrive& drive, somanet::EncoderOrdinal encoder,
    std::span<const somanet::IcMuRegisterWrite> writes, const std::stop_token& stop) {
  for (const auto& write : writes) {
    if (auto written = writeIcMuRegister(drive, encoder, write, stop); !written) {
      return written;
    }
  }
  return {};
}

somanet::IcMuRegisterWrite icMuCommand(somanet::IcMuCommand command) {
  return {somanet::kIcMuCommandRegister, static_cast<uint8_t>(command)};
}

std::expected<uint8_t, std::string> runIcMuCommand(SomanetDrive& drive,
                                                   somanet::EncoderOrdinal encoder,
                                                   somanet::IcMuCommand command,
                                                   const std::stop_token& stop) {
  if (auto before = readIcMuRegister(drive, encoder, kIcMuStatus1Register, stop); !before) {
    return std::unexpected(before.error());
  }
  if (auto r = writeIcMuRegister(drive, encoder, icMuCommand(command), stop); !r) {
    return std::unexpected(r.error());
  }
  const auto code = static_cast<uint8_t>(command);
  const auto deadline = std::chrono::steady_clock::now() + kIcMuCommandTimeout;
  bool started = false;
  uint8_t status1 = kIcMuCommandInProgress;
  while (!started || (status1 & kIcMuCommandInProgress) != 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      return std::unexpected(
          std::format("{}: the encoder did not finish command 0x{:02X} within 5 s",
                      somanet::toString(encoder), code));
    }
    std::this_thread::sleep_for(kIcMuCommandPollInterval);
    if (stop.stop_requested()) {
      return std::unexpected("cancelled");
    }
    if (!started) {
      auto queued = readIcMuRegister(drive, encoder, somanet::kIcMuCommandRegister, stop);
      if (!queued) {
        return std::unexpected(queued.error());
      }
      started = *queued == 0;
    }
    if (started) {
      auto value = readIcMuRegister(drive, encoder, kIcMuStatus1Register, stop);
      if (!value) {
        return std::unexpected(value.error());
      }
      status1 = *value;
    }
  }
  if ((status1 & kIcMuEepromError) != 0) {
    return std::unexpected(std::format(
        "{}: the encoder reports an I²C error (EPR_ERR) after command 0x{:02X}: STATUS1 = 0x{:02X}",
        somanet::toString(encoder), code, status1));
  }
  return status1;
}

std::expected<uint8_t, std::string> saveIcMuEeprom(SomanetDrive& drive,
                                                   somanet::EncoderOrdinal encoder,
                                                   const std::stop_token& stop) {
  return runIcMuCommand(drive, encoder, somanet::IcMuCommand::kWriteAll, stop)
      .and_then([&](uint8_t status1) -> std::expected<uint8_t, std::string> {
        if ((status1 & kIcMuCrcError) != 0) {
          return std::unexpected(std::format(
              "{}: the encoder reports a CRC error (CRC_ERR) after WRITE_ALL: STATUS1 = 0x{:02X}",
              somanet::toString(encoder), status1));
        }
        return status1;
      });
}

std::expected<void, std::string> runIcMuI2cTransfer(
    SomanetDrive& drive, somanet::EncoderOrdinal encoder,
    std::span<const somanet::IcMuRegisterWrite> writes, const std::stop_token& stop) {
  return writeIcMuRegisters(drive, encoder, writes.first(writes.size() - 1), stop).and_then([&] {
    return runIcMuCommand(drive, encoder, somanet::IcMuCommand::kI2cCom, stop)
        .transform([](uint8_t) {});
  });
}

std::expected<std::vector<uint8_t>, std::string> readIcMuI2c(SomanetDrive& drive,
                                                             somanet::EncoderOrdinal encoder,
                                                             somanet::IcMuI2cTarget target,
                                                             uint8_t deviceStart, uint8_t count,
                                                             const std::stop_token& stop) {
  const auto setup =
      somanet::icMuI2cTransferSetup(somanet::icMuI2cDeviceId(target, false), deviceStart, count);
  auto run = writeIcMuRegisters(drive, encoder, setup, stop).and_then([&] {
    return runIcMuCommand(drive, encoder, somanet::IcMuCommand::kI2cCom, stop);
  });
  if (!run) {
    return std::unexpected(run.error());
  }
  std::vector<uint8_t> bytes;
  for (uint8_t i = 0; i < count; ++i) {
    auto value = readIcMuRegister(drive, encoder,
                                  static_cast<uint8_t>(somanet::kIcMuExchangeRegister + i), stop);
    if (!value) {
      return std::unexpected(value.error());
    }
    bytes.push_back(*value);
  }
  return bytes;
}

std::expected<void, std::string> writeIcMuI2c(SomanetDrive& drive, somanet::EncoderOrdinal encoder,
                                              somanet::IcMuI2cTarget target, uint8_t deviceStart,
                                              std::span<const uint8_t> bytes,
                                              const std::stop_token& stop) {
  auto writes = somanet::icMuI2cTransferSetup(somanet::icMuI2cDeviceId(target, true), deviceStart,
                                              static_cast<uint8_t>(bytes.size()));
  std::ranges::transform(bytes, std::back_inserter(writes),
                         [address = somanet::kIcMuExchangeRegister](uint8_t value) mutable {
                           return somanet::IcMuRegisterWrite{address++, value};
                         });
  writes.push_back(icMuCommand(somanet::IcMuCommand::kI2cCom));
  return runIcMuI2cTransfer(drive, encoder, writes, stop);
}

}  // namespace mm::node
