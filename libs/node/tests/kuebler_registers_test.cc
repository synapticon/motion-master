#include "node/kuebler_registers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace {

using mm::node::somanet::findKueblerRegister;
using mm::node::somanet::kKueblerRegisters;
using mm::node::somanet::kMaxKueblerRegisterBytes;
using mm::node::somanet::KueblerAccess;
using mm::node::somanet::KueblerFormat;

TEST(KueblerRegisters, TranscribesTheWholeDraft) {
  // The vendor's draft table has 27 rows. Pinned because this was transcribed by hand from a CSV,
  // and a dropped row is invisible otherwise.
  EXPECT_EQ(kKueblerRegisters.size(), 27u);
}

TEST(KueblerRegisters, AreAscendingAndUnique) {
  EXPECT_TRUE(std::ranges::is_sorted(kKueblerRegisters, {},
                                     [](const auto& entry) { return entry.address; }));
  for (size_t i = 1; i < kKueblerRegisters.size(); ++i) {
    EXPECT_NE(kKueblerRegisters[i].address, kKueblerRegisters[i - 1].address);
  }
}

TEST(KueblerRegisters, EveryWidthIsAWidthTheEncoderUses) {
  for (const auto& entry : kKueblerRegisters) {
    EXPECT_TRUE(entry.bits == 8 || entry.bits == 16 || entry.bits == 32 || entry.bits == 64)
        << entry.name << " has " << static_cast<int>(entry.bits) << " bits";
    EXPECT_FALSE(entry.name.empty());
    EXPECT_FALSE(entry.definition.empty()) << entry.name;
  }
}

TEST(KueblerRegisters, TheFirmwareVersionIsOutOfReachOfOneCommand) {
  // 0x04 is 64-bit and the command's length byte caps at 4, so it cannot be transferred at all.
  // The one register for which that is true, and the reason the table carries a width rather than
  // assuming everything fits.
  auto firmwareVersion = findKueblerRegister(0x04);
  ASSERT_TRUE(firmwareVersion.has_value());
  EXPECT_EQ(firmwareVersion->bits, 64);
  EXPECT_GT(firmwareVersion->bits / 8, kMaxKueblerRegisterBytes);
  for (const auto& entry : kKueblerRegisters) {
    if (entry.address != 0x04) {
      EXPECT_LE(entry.bits / 8, kMaxKueblerRegisterBytes) << entry.name;
    }
  }
}

TEST(KueblerRegisters, CarriesTheFormatsThatCsvProseCannotBeParsedFor) {
  // The two rows a mechanical reading of the draft gets wrong, which is why the table is curated.
  // Velocity is signed and says so only as "+/- FS".
  auto velocity = findKueblerRegister(0x38);
  ASSERT_TRUE(velocity.has_value());
  EXPECT_EQ(velocity->format, KueblerFormat::kSigned) << velocity->definition;

  // Analog value A & B is two signed 16-bit halves, not one signed 32-bit value.
  auto analog = findKueblerRegister(0x3C);
  ASSERT_TRUE(analog.has_value());
  EXPECT_EQ(analog->format, KueblerFormat::kSignedHalves);
  EXPECT_EQ(analog->bits, 32);

  // A POA register writes "Bit 15 - 0: signed 16 bit integer" — one clause spanning the register,
  // so a scalar rather than a bit field.
  auto poaOffset = findKueblerRegister(0x10);
  ASSERT_TRUE(poaOffset.has_value());
  EXPECT_EQ(poaOffset->format, KueblerFormat::kSigned);

  // Where several bits are named, it really is a bit field.
  auto poaStatus = findKueblerRegister(0x24);
  ASSERT_TRUE(poaStatus.has_value());
  EXPECT_EQ(poaStatus->format, KueblerFormat::kBitField);
}

TEST(KueblerRegisters, MarksTheOnesTheEncoderDoesNotImplement) {
  // Seven are documented but not implemented, and a client must be able to say so rather than offer
  // them as working.
  const auto notImplemented = std::ranges::count_if(
      kKueblerRegisters, [](const auto& entry) { return !entry.implemented; });
  EXPECT_EQ(notImplemented, 7);
  for (const uint8_t address : {0x00, 0x04, 0x60, 0x61, 0x62, 0x66, 0x6A}) {
    auto entry = findKueblerRegister(address);
    ASSERT_TRUE(entry.has_value()) << address;
    EXPECT_FALSE(entry->implemented) << entry->name;
  }
}

TEST(KueblerRegisters, RecordsAccessSoAWriteOnlyOneIsNotOfferedForReading) {
  auto poaControl = findKueblerRegister(0x25);
  ASSERT_TRUE(poaControl.has_value());
  EXPECT_EQ(poaControl->access, KueblerAccess::kWriteOnly);

  auto correctionControl = findKueblerRegister(0x50);
  ASSERT_TRUE(correctionControl.has_value());
  EXPECT_EQ(correctionControl->access, KueblerAccess::kReadWrite);

  auto position = findKueblerRegister(0x30);
  ASSERT_TRUE(position.has_value());
  EXPECT_EQ(position->access, KueblerAccess::kReadOnly);
}

TEST(KueblerRegisters, AnUndocumentedAddressIsSimplyUnknown) {
  // Not an error: the draft is preliminary and the command addresses any byte.
  EXPECT_FALSE(findKueblerRegister(0x7F).has_value());
  EXPECT_FALSE(findKueblerRegister(0xFF).has_value());
}

TEST(KueblerRegistersToJson, CarriesWhatAPickerNeeds) {
  nlohmann::json j = nlohmann::json(kKueblerRegisters);
  ASSERT_TRUE(j.is_array());
  EXPECT_EQ(j.size(), kKueblerRegisters.size());

  const auto position = std::ranges::find_if(
      j, [](const nlohmann::json& row) { return row.at("address").get<int>() == 0x30; });
  ASSERT_NE(position, j.end());
  EXPECT_EQ(position->at("name").get<std::string>(), "Absolute position ST");
  EXPECT_EQ(position->at("bits").get<int>(), 32);
  EXPECT_EQ(position->at("access").get<std::string>(), "ro");
  EXPECT_TRUE(position->at("implemented").get<bool>());
  EXPECT_EQ(position->at("format").get<std::string>(), "unsigned");
  EXPECT_TRUE(position->at("readableInOneCommand").get<bool>());

  // The one a picker must grey out for reading.
  const auto firmwareVersion = std::ranges::find_if(
      j, [](const nlohmann::json& row) { return row.at("address").get<int>() == 0x04; });
  ASSERT_NE(firmwareVersion, j.end());
  EXPECT_FALSE(firmwareVersion->at("readableInOneCommand").get<bool>());
}

}  // namespace
