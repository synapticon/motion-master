#include "node/ic_haus_registers.h"

#include <array>
#include <nlohmann/json.hpp>

namespace mm::node::somanet {

namespace {

// Shorthand for the ordinary single-address row, so the tables below read as the datasheets print
// them rather than as C++ with a repeated address.
constexpr IcHausRegister reg(uint8_t address, std::string_view fields) {
  return IcHausRegister{
      .address = address, .lastAddress = address, .fields = fields, .reserved = false};
}

// A row the datasheet prints once for a span of addresses.
constexpr IcHausRegister range(uint8_t first, uint8_t last, std::string_view fields) {
  return IcHausRegister{.address = first, .lastAddress = last, .fields = fields, .reserved = false};
}

constexpr IcHausRegister reserved(uint8_t first, uint8_t last) {
  return IcHausRegister{.address = first, .lastAddress = last, .fields = {}, .reserved = true};
}

// The SPI-only tail of the iC-MU's static part. Same rows as any other, flagged so a client can
// show them without offering them: OS command 0 speaks BiSS and cannot reach them.
constexpr IcHausRegister spiReg(uint8_t address, std::string_view fields) {
  return IcHausRegister{.address = address,
                        .lastAddress = address,
                        .fields = fields,
                        .reserved = false,
                        .spiOnly = true};
}

constexpr IcHausRegister spiReserved(uint8_t first, uint8_t last) {
  return IcHausRegister{
      .address = first, .lastAddress = last, .fields = {}, .reserved = true, .spiOnly = true};
}

// iC-MU Series datasheet Rev B1, Table 90.
constexpr std::array kIcMuBank0 = {
    reg(0x00, "GC_M(1:0) | GF_M(5:0)"),
    reg(0x01, "GX_M(6:0)"),
    reg(0x02, "VOSS_M(6:0)"),
    reg(0x03, "VOSC_M(6:0)"),
    reg(0x04, "PHR_M | PH_M(6:0)"),
    reg(0x05, "ENAC | CIBM(3:0)"),
    reg(0x06, "GC_N(1:0) | GF_N(5:0)"),
    reg(0x07, "GX_N(6:0)"),
    reg(0x08, "VOSS_N(6:0)"),
    reg(0x09, "VOSC_N(6:0)"),
    reg(0x0A, "PHR_N | PH_N(6:0)"),
    reg(0x0B, "MODEB(2:0) | NTOA | MODEA(2:0)"),
    reg(0x0C, "CFGEW(7:0)"),
    reg(0x0D, "ACC_STAT | NCHK_CRC | NCHK_NON | ACRM_RES | EMTD(2:0)"),
    reg(0x0E, "ESSI_MT(1:0) | ROT_MT | LIN | FILT(2:0)"),
    reg(0x0F, "SPO_MT(3:0) | MPC(3:0)"),
    reg(0x10, "GET_MT | CHK_MT | SBL_MT(1:0) | MODE_MT(3:0)"),
    reg(0x11, "OUT_ZERO(2:0) | OUT_MSB(4:0)"),
    reg(0x12, "GSSI | RSSI | MODE_ST(1:0) | OUT_LSB(3:0)"),
    reg(0x13, "RESABZ(7:0)"),
    reg(0x14, "RESABZ(15:8)"),
    reg(0x15, "ROT_ALL | SS_AB(1:0) | ENIF_AUTO | FRQAB(2:0)"),
    reg(0x16, "LENZ(1:0) | CHYS_AB(1:0) | PP60UVW | INV_A | INV_B | INV_Z"),
    reg(0x17, "RPL(1:0) | PPUVW(5:0)"),
    reg(0x18, "TEST(7:0)"),
    reserved(0x19, 0x1D),
    reg(0x1E, "OFF_ABZ(3:0) | RESERVED | ROT_POS"),
    reg(0x1F, "OFF_ABZ(11:4)"),
    reg(0x20, "OFF_POS(19:12)"),
    reg(0x21, "OFF_POS(27:20)"),
    reg(0x22, "OFF_POS(35:28)"),
    reg(0x23, "OFF_COM(3:0) | RESERVED"),
    reg(0x24, "OFF_COM(11:4)"),
    reg(0x25, "PA0_CONF(7:0)"),
    reserved(0x26, 0x2A),
    reg(0x2B, "RESERVED | ACGAIN_M(1:0) | AFGAIN_M(2:0)"),
    reserved(0x2C, 0x2E),
    reg(0x2F, "RESERVED | ACGAIN_N(1:0) | AFGAIN_N(2:0)"),
    reserved(0x30, 0x3F),
};

// iC-MU Series datasheet Rev B1, Table 91.
constexpr std::array kIcMuStatic = {
    reg(0x40, "BANKSEL(4:0)"),
    reg(0x41, "EDSBANK(7:0)"),
    reg(0x42, "PROFILE_ID(15:8)"),
    reg(0x43, "PROFILE_ID(7:0)"),
    reg(0x44, "SERIAL(31:24)"),
    reg(0x45, "SERIAL(23:16)"),
    reg(0x46, "SERIAL(15:8)"),
    reg(0x47, "SERIAL(7:0)"),
    reg(0x48, "OFF_ABZ(19:12)"),
    reg(0x49, "OFF_ABZ(27:20)"),
    reg(0x4A, "OFF_ABZ(35:28)"),
    reg(0x4B, "OFF_UVW(3:0) | RESERVED"),
    reg(0x4C, "OFF_UVW(11:4)"),
    reg(0x4D, "PRES_POS(3:0) | RESERVED"),
    reg(0x4E, "PRES_POS(11:4)"),
    reg(0x4F, "PRES_POS(19:12)"),
    reg(0x50, "PRES_POS(27:20)"),
    reg(0x51, "PRES_POS(35:28)"),
    reg(0x52, "SPO_0(3:0) | SPO_BASE(3:0)"),
    reg(0x53, "SPO_2(3:0) | SPO_1(3:0)"),
    reg(0x54, "SPO_4(3:0) | SPO_3(3:0)"),
    reg(0x55, "SPO_6(3:0) | SPO_5(3:0)"),
    reg(0x56, "SPO_8(3:0) | SPO_7(3:0)"),
    reg(0x57, "SPO_10(3:0) | SPO_9(3:0)"),
    reg(0x58, "SPO_12(3:0) | SPO_11(3:0)"),
    reg(0x59, "SPO_14(3:0) | SPO_13(3:0)"),
    reg(0x5A, "RPL_RESET(7:0)"),
    reg(0x5B, "I2C_DEV_START(7:0)"),
    reg(0x5C, "I2C_RAM_START(7:0)"),
    reg(0x5D, "I2C_RAM_END(7:0)"),
    reg(0x5E, "I2C_DEVID(7:0)"),
    reg(0x5F, "I2C_RETRY(7:0)"),
    range(0x60, 0x6F, "USER_EXCHANGE_REGISTERS"),
    reserved(0x70, 0x72),
    reg(0x73, "EVENT_COUNT(7:0)"),
    reg(0x74, "HARD_REV(7:0)"),
    reg(0x75, "CMD_MU(7:0)"),
    reg(0x76, "STATUS0(7:0)"),
    reg(0x77, "STATUS1(7:0)"),
    reg(0x78, "DEV_ID(47:40)"),
    reg(0x79, "DEV_ID(39:32)"),
    reg(0x7A, "DEV_ID(31:24)"),
    reg(0x7B, "DEV_ID(23:16)"),
    reg(0x7C, "DEV_ID(15:8)"),
    reg(0x7D, "DEV_ID(7:0)"),
    reg(0x7E, "MFG_ID(15:8)"),
    reg(0x7F, "MFG_ID(7:0)"),
    spiReg(0x80, "CRC16(7:0)"),
    spiReg(0x81, "CRC16(15:8)"),
    spiReg(0x82, "CRC8(7:0)"),
    spiReserved(0x83, 0xAF),
};

// iC-PVL datasheet Rev F2, Table 5.
constexpr std::array kIcPvlEeprom = {
    reg(0x00, "EN_PAR | EN_ERR | DIR | ST_GRAY | MT_GRAY | INT_MODE"),
    reg(0x01, "OS | MT_BW"),
    reg(0x02, "PCR"),
    reg(0x03, "EN_WRN | BAT_MON | A_MAX | IBIAS"),
    reg(0x04, "0 | NOMAG | ATHR | ONAX | POLEWID"),
    reg(0x05, "I2C_POS | PCR_OUT | SYNC_BW | BAT_THR | HYS | ABQUAD"),
    reg(0x06, "CRC_CFG(7:0)"),
    reg(0x07, "MT_PREL(7:0)"),
    reg(0x08, "MT_PREL(15:8)"),
    reg(0x09, "MT_PREL(23:16)"),
    reg(0x0A, "MT_PREL(31:24)"),
    reg(0x0B, "MT_PREL(39:32)"),
    reg(0x0C, "CRC_CTR(7:0)"),
};

// iC-PVL datasheet Rev F2, Table 6. The multiturn count registers carry two layouts because the
// datasheet prints two, selected by PCR_OUT — a client that shows one of them silently would be
// showing the wrong one half the time.
constexpr std::array kIcPvlI2cFull = {
    reg(0x00, "EN_PAR | EN_ERR | DIR | ST_GRAY | MT_GRAY | INT_MODE"),
    reg(0x01, "OS | MT_BW"),
    reg(0x02, "PCR"),
    reg(0x03, "EN_WRN | BAT_MON | A_MAX | IBIAS"),
    reg(0x04, "0 | NOMAG | ATHR | ONAX | POLEWID"),
    reg(0x05, "I2C_POS | PCR_OUT | SYNC_BW | BAT_THR | HYS | ABQUAD"),
    reg(0x06, "CRC_CFG(7:0)"),
    reg(0x07, "MT_COUNT(7:0) with PCR_OUT = 0; Current PCR(7:0) with PCR_OUT = 1"),
    reg(0x08, "MT_COUNT(15:8) with PCR_OUT = 0; MT_COUNT(7:0) with PCR_OUT = 1"),
    reg(0x09, "MT_COUNT(23:16) with PCR_OUT = 0; MT_COUNT(15:8) with PCR_OUT = 1"),
    reg(0x0A, "MT_COUNT(31:24) with PCR_OUT = 0; MT_COUNT(23:16) with PCR_OUT = 1"),
    reg(0x0B, "MT_COUNT(39:32) with PCR_OUT = 0; MT_COUNT(31:24) with PCR_OUT = 1"),
    reg(0x0C, "CRC_CTR(7:0)"),
    reg(0x0D, "SYNC(2:0)"),
    reserved(0x0E, 0x0E),
    reg(0x0F, "CHIP_REL"),
    reg(0x10, "PRESET | PDR | BAT_WRN | BAT_ERR | POS_ERR | CTR_ERR | CFG_ERR | STUP_ERR"),
    reg(0x11, "CMD(7:0)"),
    reg(0x12, "SLEEP_ST | NOMAG_ST | ACTIVE_ST | POWON_ST | Unused | NOMAG_L | MAG_ERR | AMPL_ERR"),
};

// iC-PVL datasheet Rev F2, Table 7.
constexpr std::array kIcPvlI2cLimited = {
    reg(0x00, "PRESET | PDR | BAT_WRN | BAT_ERR | POS_ERR | CTR_ERR | CFG_ERR | STUP_ERR"),
    reg(0x01, "CMD(7:0)"),
    reg(0x02, "SLEEP_ST | NOMAG_ST | ACTIVE_ST | POWON_ST | Unused | NOMAG_L | MAG_ERR | AMPL_ERR"),
};

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

void to_json(nlohmann::json& j, const IcHausRegister& r) {
  j = nlohmann::json{
      {"address", r.address},   {"lastAddress", r.lastAddress}, {"fields", r.fields},
      {"reserved", r.reserved}, {"spiOnly", r.spiOnly},
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
