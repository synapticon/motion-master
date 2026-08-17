#include "node/ic_haus_registers.h"

#include <array>
#include <nlohmann/json.hpp>
#include <string>

namespace mm::node::somanet {

namespace {

// One field, named and described as its datasheet does.
constexpr IcHausField f(std::string_view name, std::string_view bits,
                        std::string_view description) {
  return IcHausField{.name = name, .bits = bits, .description = description};
}

// A register row and the fields it names, so the tables below read as the datasheets print them.
template <typename... Fields>
constexpr IcHausRegister reg(uint8_t address, Fields... fields) {
  IcHausRegister r{.address = address, .lastAddress = address};
  ((r.fields[r.fieldCount++] = fields), ...);
  return r;
}

// A row the datasheet prints once for a span of addresses.
template <typename... Fields>
constexpr IcHausRegister range(uint8_t first, uint8_t last, Fields... fields) {
  IcHausRegister r = reg(first, fields...);
  r.lastAddress = last;
  return r;
}

constexpr IcHausRegister reserved(uint8_t first, uint8_t last) {
  return IcHausRegister{.address = first, .lastAddress = last, .reserved = true};
}

// Marks a row SPI-only: same row as any other, flagged so a client can show it without offering it,
// since OS command 0 speaks BiSS and cannot reach it.
constexpr IcHausRegister spiOnly(IcHausRegister r) {
  r.spiOnly = true;
  return r;
}

// Fields several rows share, so a multi-byte value is described once rather than once per byte.
constexpr std::string_view kReservedField = "Reserved";
constexpr std::string_view kOffAbz = "Absolute position offset for the ABZ calculation engine";
constexpr std::string_view kOffPos =
    "Serial address: absolute position offset for the ABZ calculation engine, changed by "
    "Nonius/multiturn";
constexpr std::string_view kOffCom =
    "Serial address: absolute position offset for the UVW calculation engine, changed by Nonius";
constexpr std::string_view kOffUvw = "Commutation signal start angle";
constexpr std::string_view kPresPos = "Preset position for the ABZ section";
constexpr std::string_view kResAbz =
    "Incremental interface resolution for ABZ, STEP/DIR and CW/CCW";
constexpr std::string_view kSpo = "Offset of Nonius to master";
constexpr std::string_view kDevId = "Device ID";
constexpr std::string_view kMfgId = "Manufacturer ID";
constexpr std::string_view kSerial = "Serial number";
constexpr std::string_view kProfileId = "Profile ID";
constexpr std::string_view kCrc16 = "EEPROM configuration data checksum";
constexpr std::string_view kMtPrel = "Multiturn counter preload value";
constexpr std::string_view kMtCount =
    "Multiturn counter current count with PCR_OUT = 0; shifted one byte up with PCR_OUT = 1";

// iC-MU Series datasheet Rev B1, Table 90.
constexpr std::array kIcMuBank0 = {
    reg(0x00, f("GC_M", "1:0", "Master gain range selection"), f("GF_M", "5:0", "Master gain")),
    reg(0x01, f("GX_M", "6:0", "Master cosine signal gain adjustment")),
    reg(0x02, f("VOSS_M", "6:0", "Master sine offset adjustment")),
    reg(0x03, f("VOSC_M", "6:0", "Master cosine offset adjustment")),
    reg(0x04, f("PHR_M", "", "Master phase adjustment range"),
        f("PH_M", "6:0", "Master phase adjustment")),
    reg(0x05, f("ENAC", "", "Amplitude control unit activation"),
        f("CIBM", "3:0", "Bias current settings")),
    reg(0x06, f("GC_N", "1:0", "Nonius gain range selection"), f("GF_N", "5:0", "Nonius gain")),
    reg(0x07, f("GX_N", "6:0", "Nonius cosine signal gain adjustment")),
    reg(0x08, f("VOSS_N", "6:0", "Nonius sine offset adjustment")),
    reg(0x09, f("VOSC_N", "6:0", "Nonius cosine offset adjustment")),
    reg(0x0A, f("PHR_N", "", "Nonius phase adjustment range"),
        f("PH_N", "6:0", "Nonius phase adjustment")),
    reg(0x0B, f("MODEB", "2:0", "I/O port B configuration"), f("NTOA", "", "Adaptive timeout"),
        f("MODEA", "2:0", "I/O port A configuration")),
    reg(0x0C, f("CFGEW", "7:0", "Error and warning bit configuration")),
    reg(0x0D, f("ACC_STAT", "", "Output configuration of the status register"),
        f("NCHK_CRC", "", "Cyclic check of CRC16 and CRC8"),
        f("NCHK_NON", "", "Cyclic check of the Nonius value (low active)"),
        f("ACRM_RES", "", "Automatic reset with master track amplitude errors"),
        f("EMTD", "2:0", "Minimum error message duration")),
    reg(0x0E, f("ESSI_MT", "1:0", "Error bit external multiturn"),
        f("ROT_MT", "", "Code direction external multiturn"), f("LIN", "", "Linear scanning"),
        f("FILT", "2:0", "Digital filter settings")),
    reg(0x0F, f("SPO_MT", "3:0", "Offset external multiturn"),
        f("MPC", "3:0", "Master period count")),
    reg(0x10, f("GET_MT", "", "Multiturn interface daisy chain"),
        f("CHK_MT", "", "Cyclic check of the multiturn value"),
        f("SBL_MT", "1:0", "Multiturn synchronization bit length"),
        f("MODE_MT", "3:0", "Multiturn mode")),
    reg(0x11,
        f("OUT_ZERO", "2:0",
          "Output shift register configuration: zeros inserted after the used bits and before an "
          "error or warning"),
        f("OUT_MSB", "4:0", "Output shift register configuration: MSB used bits")),
    reg(0x12, f("GSSI", "", "Gray or binary data format"), f("RSSI", "", "Ring operation"),
        f("MODE_ST", "1:0", "Data output"),
        f("OUT_LSB", "3:0", "Output shift register configuration: LSB used bits")),
    reg(0x13, f("RESABZ", "7:0", kResAbz)),
    reg(0x14, f("RESABZ", "15:8", kResAbz)),
    reg(0x15, f("ROT_ALL", "", "Code direction"), f("SS_AB", "1:0", "System AB step size"),
        f("ENIF_AUTO", "", "Incremental interface enable"),
        f("FRQAB", "2:0", "AB output frequency")),
    reg(0x16, f("LENZ", "1:0", "Index pulse length"), f("CHYS_AB", "1:0", "Converter hysteresis"),
        f("PP60UVW", "", "Commutation signal phase position"),
        f("INV_A", "", "A, STEP or CW signal inversion"),
        f("INV_B", "", "B, DIR or CCW signal inversion"),
        f("INV_Z", "", "Z or NCLR signal inversion")),
    reg(0x17, f("RPL", "1:0", "Register access control"),
        f("PPUVW", "5:0", "Number of commutation signal pole pairs")),
    reg(0x18, f("TEST", "7:0", "Adjustment modes and iC-Haus test modes")),
    reserved(0x19, 0x1D),
    reg(0x1E, f("OFF_ABZ", "3:0", kOffAbz), f("RESERVED", "", kReservedField),
        f("ROT_POS", "",
          "Code direction for the ABZ calculation engine and the serial absolute interface")),
    reg(0x1F, f("OFF_ABZ", "11:4", kOffAbz)),
    reg(0x20, f("OFF_POS", "19:12", kOffPos)),
    reg(0x21, f("OFF_POS", "27:20", kOffPos)),
    reg(0x22, f("OFF_POS", "35:28", kOffPos)),
    reg(0x23, f("OFF_COM", "3:0", kOffCom), f("RESERVED", "", kReservedField)),
    reg(0x24, f("OFF_COM", "11:4", kOffCom)),
    reg(0x25, f("PA0_CONF", "7:0", "Configurable commands on pin PA0")),
    reserved(0x26, 0x2A),
    reg(0x2B, f("RESERVED", "", kReservedField),
        f("ACGAIN_M", "1:0", "Master gain range currently set by the amplitude control unit"),
        f("AFGAIN_M", "2:0", "Master gain factor currently set by the amplitude control unit")),
    reserved(0x2C, 0x2E),
    reg(0x2F, f("RESERVED", "", kReservedField),
        f("ACGAIN_N", "1:0", "Nonius gain range currently set by the amplitude control unit"),
        f("AFGAIN_N", "2:0", "Nonius gain factor currently set by the amplitude control unit")),
    reserved(0x30, 0x3F),
};

// iC-MU Series datasheet Rev B1, Table 91.
constexpr std::array kIcMuStatic = {
    reg(0x40, f("BANKSEL", "4:0", "Serial access: bank register")),
    reg(0x41, f("EDSBANK", "7:0", "EDS bank")),
    reg(0x42, f("PROFILE_ID", "15:8", kProfileId)),
    reg(0x43, f("PROFILE_ID", "7:0", kProfileId)),
    reg(0x44, f("SERIAL", "31:24", kSerial)),
    reg(0x45, f("SERIAL", "23:16", kSerial)),
    reg(0x46, f("SERIAL", "15:8", kSerial)),
    reg(0x47, f("SERIAL", "7:0", kSerial)),
    reg(0x48, f("OFF_ABZ", "19:12", kOffAbz)),
    reg(0x49, f("OFF_ABZ", "27:20", kOffAbz)),
    reg(0x4A, f("OFF_ABZ", "35:28", kOffAbz)),
    reg(0x4B, f("OFF_UVW", "3:0", kOffUvw), f("RESERVED", "", kReservedField)),
    reg(0x4C, f("OFF_UVW", "11:4", kOffUvw)),
    reg(0x4D, f("PRES_POS", "3:0", kPresPos), f("RESERVED", "", kReservedField)),
    reg(0x4E, f("PRES_POS", "11:4", kPresPos)),
    reg(0x4F, f("PRES_POS", "19:12", kPresPos)),
    reg(0x50, f("PRES_POS", "27:20", kPresPos)),
    reg(0x51, f("PRES_POS", "35:28", kPresPos)),
    reg(0x52, f("SPO_0", "3:0", kSpo), f("SPO_BASE", "3:0", kSpo)),
    reg(0x53, f("SPO_2", "3:0", kSpo), f("SPO_1", "3:0", kSpo)),
    reg(0x54, f("SPO_4", "3:0", kSpo), f("SPO_3", "3:0", kSpo)),
    reg(0x55, f("SPO_6", "3:0", kSpo), f("SPO_5", "3:0", kSpo)),
    reg(0x56, f("SPO_8", "3:0", kSpo), f("SPO_7", "3:0", kSpo)),
    reg(0x57, f("SPO_10", "3:0", kSpo), f("SPO_9", "3:0", kSpo)),
    reg(0x58, f("SPO_12", "3:0", kSpo), f("SPO_11", "3:0", kSpo)),
    reg(0x59, f("SPO_14", "3:0", kSpo), f("SPO_13", "3:0", kSpo)),
    reg(0x5A, f("RPL_RESET", "7:0",
                "Serial access: register that resets the register access restriction")),
    reg(0x5B, f("I2C_DEV_START", "7:0",
                "Start address in the I²C device the transfer reads from or writes to")),
    reg(0x5C, f("I2C_RAM_START", "7:0", "Start address of the exchange data area in internal RAM")),
    reg(0x5D, f("I2C_RAM_END", "7:0", "End address of the exchange data area in internal RAM")),
    reg(0x5E, f("I2C_DEVID", "7:0", "I²C device id, including the read/write bit")),
    reg(0x5F, f("I2C_RETRY", "7:0", "I²C communication retries")),
    range(0x60, 0x6F,
          f("USER_EXCHANGE_REGISTERS", "", "Data exchanged with the addressed I²C device")),
    reserved(0x70, 0x72),
    reg(0x73, f("EVENT_COUNT", "7:0", "Serial access: event counter")),
    reg(0x74, f("HARD_REV", "7:0", "Serial address: chip type and hardware revision code")),
    reg(0x75, f("CMD_MU", "7:0", "Serial address: command register")),
    reg(0x76, f("STATUS0", "7:0", "Serial address: status register 0")),
    reg(0x77, f("STATUS1", "7:0", "Serial address: status register 1")),
    reg(0x78, f("DEV_ID", "47:40", kDevId)),
    reg(0x79, f("DEV_ID", "39:32", kDevId)),
    reg(0x7A, f("DEV_ID", "31:24", kDevId)),
    reg(0x7B, f("DEV_ID", "23:16", kDevId)),
    reg(0x7C, f("DEV_ID", "15:8", kDevId)),
    reg(0x7D, f("DEV_ID", "7:0", kDevId)),
    reg(0x7E, f("MFG_ID", "15:8", kMfgId)),
    reg(0x7F, f("MFG_ID", "7:0", kMfgId)),
    spiOnly(reg(0x80, f("CRC16", "7:0", kCrc16))),
    spiOnly(reg(0x81, f("CRC16", "15:8", kCrc16))),
    spiOnly(reg(0x82, f("CRC8", "7:0", "EEPROM offset and preset data checksum"))),
    spiOnly(reserved(0x83, 0xAF)),
};

// The iC-PVL's configuration block, identical in its EEPROM and its full I²C window (Rev F2,
// Tables 5 and 6) — described once so the two cannot drift apart.
constexpr std::array kIcPvlConfig = {
    reg(0x00, f("EN_PAR", "", "Parity bit transmission enable"),
        f("EN_ERR", "", "Error bit transmission enable"), f("DIR", "", "Code direction"),
        f("ST_GRAY", "", "Singleturn input data format"),
        f("MT_GRAY", "", "Multiturn output data format"),
        f("INT_MODE", "", "Serial interface operating mode")),
    reg(0x01, f("OS", "", "Electrical offset, multiturn to singleturn"),
        f("MT_BW", "", "Bit width of the multiturn data and counter")),
    reg(0x02, f("PCR", "", "Period count per revolution")),
    reg(0x03, f("EN_WRN", "", "Low battery warning enable"),
        f("BAT_MON", "", "Battery monitoring enable"), f("A_MAX", "", "Maximum angle acceleration"),
        f("IBIAS", "", "Bias current and oscillator frequency calibration")),
    reg(0x04, f("0", "", "Fixed zero"), f("NOMAG", "", "NoMagnet detection"),
        f("ATHR", "", "Field amplitude threshold value"),
        f("ONAX", "", "On-axis magnetic scanning"),
        f("POLEWID", "", "Pole size of magnetic scale")),
    reg(0x05, f("I2C_POS", "", "Enable I²C position readout"), f("PCR_OUT", "", "PCR output mode"),
        f("SYNC_BW", "", "Synchronization bit width"),
        f("BAT_THR", "", "Battery monitor thresholds"), f("HYS", "", "Hysteresis"),
        f("ABQUAD", "", "AB quadrature output")),
    reg(0x06, f("CRC_CFG", "7:0", "Checksum for the chip configuration, 0x00-0x05")),
};

// iC-PVL datasheet Rev F2, Table 5.
constexpr std::array kIcPvlEepromTail = {
    reg(0x07, f("MT_PREL", "7:0", kMtPrel)),
    reg(0x08, f("MT_PREL", "15:8", kMtPrel)),
    reg(0x09, f("MT_PREL", "23:16", kMtPrel)),
    reg(0x0A, f("MT_PREL", "31:24", kMtPrel)),
    reg(0x0B, f("MT_PREL", "39:32", kMtPrel)),
    reg(0x0C, f("CRC_CTR", "7:0", "Checksum for MT_PREL, 0x07-0x0B")),
};

// iC-PVL datasheet Rev F2, Table 6 — the count registers onward. The datasheet prints two layouts
// for 0x07-0x0B because PCR_OUT shifts them, and both are carried: a client showing one of them
// silently would be showing the wrong one half the time.
constexpr std::array kIcPvlI2cFullTail = {
    reg(0x07, f("MT_COUNT / Current PCR", "7:0", kMtCount)),
    reg(0x08, f("MT_COUNT", "15:8 or 7:0", kMtCount)),
    reg(0x09, f("MT_COUNT", "23:16 or 15:8", kMtCount)),
    reg(0x0A, f("MT_COUNT", "31:24 or 23:16", kMtCount)),
    reg(0x0B, f("MT_COUNT", "39:32 or 31:24", kMtCount)),
    reg(0x0C, f("CRC_CTR", "7:0", "Checksum for the multiturn counter")),
    reg(0x0D, f("SYNC", "2:0", "Synchronization bits, read only")),
    reserved(0x0E, 0x0E),
    reg(0x0F, f("CHIP_REL", "7:0", "Chip release, read only")),
    reg(0x10, f("PRESET", "", "Pin preset detected, I²C REBOOT detected, or sleep mode activated"),
        f("PDR", "", "Power down reset detected"), f("BAT_WRN", "", "Battery early warning"),
        f("BAT_ERR", "", "Battery error"), f("POS_ERR", "", "Position error"),
        f("CTR_ERR", "", "Internal counter error"),
        f("CFG_ERR", "", "Internal configuration error"), f("STUP_ERR", "", "Startup error")),
    reg(0x11, f("CMD", "7:0", "Command register, write only")),
    reg(0x12, f("SLEEP_ST", "", "In the Sleep working state"),
        f("NOMAG_ST", "", "In the NoMagnet working state"),
        f("ACTIVE_ST", "", "In the Active working state"),
        f("POWON_ST", "", "In the PowerOn working state"), f("Unused", "", "Unused"),
        f("NOMAG_L", "", "NoMagnet working state, latched"), f("MAG_ERR", "", "Magnet error"),
        f("AMPL_ERR", "", "Amplitude error")),
};

// iC-PVL datasheet Rev F2, Table 7 — the same chip through a three-register window, so the two
// status rows are the ones from Table 6 at their own addresses.
constexpr std::array kIcPvlI2cLimited = {
    reg(0x00, f("PRESET", "", "Pin preset detected, I²C REBOOT detected, or sleep mode activated"),
        f("PDR", "", "Power down reset detected"), f("BAT_WRN", "", "Battery early warning"),
        f("BAT_ERR", "", "Battery error"), f("POS_ERR", "", "Position error"),
        f("CTR_ERR", "", "Internal counter error"),
        f("CFG_ERR", "", "Internal configuration error"), f("STUP_ERR", "", "Startup error")),
    reg(0x01, f("CMD", "7:0", "Command register, write only")),
    reg(0x02, f("SLEEP_ST", "", "In the Sleep working state"),
        f("NOMAG_ST", "", "In the NoMagnet working state"),
        f("ACTIVE_ST", "", "In the Active working state"),
        f("POWON_ST", "", "In the PowerOn working state"), f("Unused", "", "Unused"),
        f("NOMAG_L", "", "NoMagnet working state, latched"), f("MAG_ERR", "", "Magnet error"),
        f("AMPL_ERR", "", "Amplitude error")),
};

// Joins the shared configuration block to a space's own tail, so each iC-PVL space is one
// contiguous table without its first seven rows being written twice.
template <size_t Tail>
constexpr auto icPvlSpace(const std::array<IcHausRegister, Tail>& tail) {
  std::array<IcHausRegister, kIcPvlConfig.size() + Tail> all{};
  size_t i = 0;
  for (const auto& r : kIcPvlConfig) {
    all[i++] = r;
  }
  for (const auto& r : tail) {
    all[i++] = r;
  }
  return all;
}

constexpr auto kIcPvlEeprom = icPvlSpace(kIcPvlEepromTail);
constexpr auto kIcPvlI2cFull = icPvlSpace(kIcPvlI2cFullTail);

constexpr std::array kSpaces = {
    IcHausRegisterSpace{
        .chip = IcHausChip::kIcMu,
        .name = "CONF, bank 0, addresses 0x00-0x3F",
        .addressing = "The iC-MU's own register file, read and written directly by OS command 0.",
        .registers = kIcMuBank0,
    },
    IcHausRegisterSpace{
        .chip = IcHausChip::kIcMu,
        .name = "Static part, addresses 0x40-0xBF",
        .addressing = "The iC-MU's own register file. Addresses 0x40-0xBF repeat across banks "
                      "0-31, selected by BANKSEL (0x40), so an address names a register only "
                      "together with the bank in force. 0x80 upwards is SPI-only.",
        .registers = kIcMuStatic,
    },
    IcHausRegisterSpace{
        .chip = IcHausChip::kIcPvl,
        .name = "EEPROM",
        .addressing = "The iC-PVL's non-volatile configuration, not addressable as a register "
                      "space over the drive's register communication service.",
        .registers = kIcPvlEeprom,
    },
    IcHausRegisterSpace{
        .chip = IcHausChip::kIcPvl,
        .name = "I²C slave mode, ID 0b1100001",
        .addressing = "Reached over I²C through the iC-MU, with device id 0xC2 to write and 0xC3 "
                      "to read — the ids the firmware calls ICMU_DEVID_WRITE_ICPVL and "
                      "ICMU_DEVID_READ_ICPVL.",
        .registers = kIcPvlI2cFull,
    },
    IcHausRegisterSpace{
        .chip = IcHausChip::kIcPvl,
        .name = "I²C slave mode, ID 0b1100000",
        .addressing = "The same chip through a three-register window: device id 0xC0 to write and "
                      "0xC1 to read, the firmware's ICMU_DEVID_WRITE_ICPVL_LIMITED and "
                      "ICMU_DEVID_READ_ICPVL_LIMITED.",
        .registers = kIcPvlI2cLimited,
    },
};

}  // namespace

std::string IcHausRegister::layout() const {
  if (reserved) {
    return "RESERVED";
  }
  std::string joined;
  for (const auto& field : namedFields()) {
    if (!joined.empty()) {
      joined += " | ";
    }
    joined += field.name;
    if (!field.bits.empty()) {
      joined += '(';
      joined += field.bits;
      joined += ')';
    }
  }
  return joined;
}

void to_json(nlohmann::json& j, const IcHausField& field) {
  j = nlohmann::json{
      {"name", field.name},
      {"bits", field.bits},
      {"description", field.description},
  };
}

void to_json(nlohmann::json& j, const IcHausRegister& r) {
  j = nlohmann::json{
      {"address", r.address}, {"lastAddress", r.lastAddress}, {"reserved", r.reserved},
      {"spiOnly", r.spiOnly}, {"layout", r.layout()},         {"fields", r.namedFields()},
  };
}

void to_json(nlohmann::json& j, const IcHausRegisterSpace& space) {
  j = nlohmann::json{
      {"chip", toString(space.chip)},
      {"name", space.name},
      {"addressing", space.addressing},
      {"registers", space.registers},
  };
}

std::span<const IcHausRegisterSpace> icHausRegisterSpaces() { return kSpaces; }

}  // namespace mm::node::somanet
