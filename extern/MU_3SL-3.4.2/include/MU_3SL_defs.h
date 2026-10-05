/*
 * Software and its documentation is provided by iC-Haus GmbH or contributors "AS IS" and is
 * subject to the ZVEI General Conditions for the Supply of Products and Services with iC-Haus
 * amendments and the ZVEI Software clause with iC-Haus amendments (http://www.ichaus.de/EULA).
 */

#ifndef MU_3SL_DEFS_H
#define MU_3SL_DEFS_H

#include <stdint.h>


typedef struct MU_Handle_* MU_Handle;


typedef struct {
    uint32_t mt;
    uint32_t st;
    uint32_t err;
    uint32_t warn;
    uint32_t rawMaster;
    uint32_t rawNonius;
} ReadSensStruct;

typedef struct {
    uint32_t singleTurnPosition;
    uint32_t internalSingleTurnPosition;
    uint32_t normalizedExternalMultiTurnSyncBits;
    uint32_t normalizedInternalMultiTurnSyncBits;
    uint32_t externalMultiTurnPosition;
    uint32_t multiTurnPosition;

    uint64_t singleCycleData;
} MU_MtSyncData;


enum MU_ErrorEnum {
    MU_OK,
    MU_INVALID_HANDLE,
    MU_INTERFACEDRIVER_NOT_FOUND,
    MU_INTERFACE_NOT_FOUND,
    MU_INVALID_INTERFACE,
    MU_NO_INTERFACE_SELECTED,
    MU_INVALID_PARAMETER,
    MU_INVALID_ADDRESS,
    MU_INVALID_VALUE,
    MU_USB_ERROR,
    MU_FILE_NOT_FOUND,
    MU_INVALID_FILE,
    MU_SPI_ERROR,
    MU_SPI_DISMISS,
    MU_SPI_FAIL,
    MU_SPI_BUSY_TIMEOUT,
    MU_VERIFY_FAILED,
    MU_MASTERCOMM_FAILED,
    MU_BISSCOMM_FAILED,
    MU_INVALID_BISSMASTER,
    MU_USB_HIGHSPEED_WARNING,
    MU_FAST_ROTATION,
    MU_SLOW_ROTATION,
    MU_FILE_ACCESS_DENIED,
    MU_READPARAM_SSI,
    MU_SSIRING_ERROR,
    MU_SEMI_ROTATION,
    MU_BISS_REGERROR,
    MU_INTERNAL_CALIB_ERROR,
    MU_INVALID_CONFIGURATION,
    MU_CALIBRATION_FAILED,
    MU_ACQUISITION_FAILED,
    MU_GAIN_LIMIT,
    MU_OFFSET_LIMIT,
    MU_PHASE_LIMIT,
    MU_BAD_CAL_DATA,
    MU_I2C_COMM_FAILED,
    MU_USB_DATA_LOSS,
    MU_MT_SYNC_FAILED,
    MU_UNKNOWN_ERROR,
    MU_UNKNOWN_REVISION,
    MU_UNSUPPORTED_REVISION,
    MU_UNKNOWN_PARAMETER,
    MU_PARAMETER_NOT_IN_REVISION,
    MU_REVISION_NOT_SET,
    MU_UNSUPPORTED_CHIP,
    MU_CONTRADICTORY_REVISIONS,
    MU_FRAME_RATE_NOT_SUPPORTED,
    MU_CLOCK_FREQUENCY_NOT_SUPPORTED,
    MU_FRAME_CYCLE_TIME_TO_SHORT,
    MU_NO_BISS_PROFILE_ID,
    MU_ADAPTER_FIRMWARE_INCOMPATIBLE
};
typedef enum MU_ErrorEnum MU_Error;

enum MU_InterfaceEnum {
    MU_NO_INTERFACE,
    MU_MB3U_SPI,
    MU_MB3U_BISS,
    MU_MB4U,
    MU_MB5U,
    MU_MB8U_SPI,
    MU_MB8U_BISS
};
typedef enum MU_InterfaceEnum MU_Interface;

enum MU_ParamEnum {
    MU_GF_M,
    MU_GC_M,
    MU_GX_M,
    MU_VOSS_M,
    MU_VOSC_M,
    MU_PH_M,
    MU_PHR_M,
    MU_CIBM,
    MU_ENAC,
    MU_GF_N,
    MU_GC_N,
    MU_GX_N,
    MU_VOSS_N,
    MU_VOSC_N,
    MU_PH_N,
    MU_PHR_N,
    MU_MODEA,
    MU_NTOA,
    MU_MODEB,
    MU_CFGEW,
    MU_EMTD,
    MU_ACRM_RES,
    MU_NCHK_NON,
    MU_NCHK_CRC,
    MU_ACC_STAT,
    MU_FILT,
    MU_LIN,
    MU_ESSI_MT,
    MU_ROT_MT,
    MU_MPC,
    MU_SPO_MT,
    MU_MODE_MT,
    MU_SBL_MT,
    MU_CHK_MT,
    MU_GET_MT,
    MU_OUT_MSB,
    MU_OUT_ZERO,
    MU_OUT_LSB,
    MU_MODE_ST,
    MU_RSSI,
    MU_GSSI,
    MU_RESABZ,
    MU_FRQAB,
    MU_ENIF_AUTO,
    MU_SS_AB,
    MU_ROT_ALL,
    MU_INV_Z,
    MU_INV_B,
    MU_INV_A,
    MU_PP60UVW,
    MU_CHYS_AB,
    MU_LENZ,
    MU_PPUVW,
    MU_RPL,
    MU_TEST,
    MU_ROT_POS,
    MU_OFF_ABZ,
    MU_OFF_POS,
    MU_OFF_COM,
    MU_PA0_CONF,
    MU_AFGAIN_M,
    MU_ACGAIN_M,
    MU_AFGAIN_N,
    MU_ACGAIN_N,
    MU_EDSBANK,
    MU_PROFILE_ID,
    MU_SERIAL,
    MU_OFF_UVW,
    MU_PRES_POS,
    MU_SPO_BASE,
    MU_SPO_0,
    MU_SPO_1,
    MU_SPO_2,
    MU_SPO_3,
    MU_SPO_4,
    MU_SPO_5,
    MU_SPO_6,
    MU_SPO_7,
    MU_SPO_8,
    MU_SPO_9,
    MU_SPO_10,
    MU_SPO_11,
    MU_SPO_12,
    MU_SPO_13,
    MU_SPO_14,
    MU_SPO_15,
    MU_RPL_RESET,
    MU_I2C_DEV_START,
    MU_I2C_RAM_START,
    MU_I2C_RAM_END,
    MU_I2C_DEVID,
    MU_I2C_RETRY,
    MU_HARD_REV,
    MU_DEV_ID0,
    MU_DEV_ID1,
    MU_DEV_ID2,
    MU_DEV_ID3,
    MU_DEV_ID4,
    MU_DEV_ID5,
    MU_MFG_ID0,
    MU_MFG_ID1,
    MU_CRC16,
    MU_CRC8,
    MU_AM_MIN,
    MU_AM_MAX,
    MU_AN_MIN,
    MU_AN_MAX,
    MU_STUP,
    MU_CMD_EXE,
    MU_FRQ_CNV,
    MU_FRQ_ABZ,
    MU_NON_CTR,
    MU_MT_CTR,
    MU_MT_ERR,
    MU_EPR_ERR,
    MU_CRC_ERR
};
typedef enum MU_ParamEnum MU_Param;

enum MU_CommandEnum {
    MU_CMD_NO_FUNCTION,
    MU_CMD_WRITE_ALL,
    MU_CMD_WRITE_OFF,
    MU_CMD_ABS_RESET,
    MU_CMD_NON_VER,
    MU_CMD_MT_RESET,
    MU_CMD_MT_VER,
    MU_CMD_SOFT_RESET,
    MU_CMD_SOFT_PRES,
    MU_CMD_SOFT_E2P_PRES,
    MU_CMD_I2C_COM,
    MU_CMD_EVENT_COUNT,
    MU_CMD_SWITCH,
    MU_CMD_CRC_VER,
    MU_CMD_CRC_CALC,
    MU_CMD_SET_MTC,
    MU_CMD_RES_MTC,
    MU_CMD_RESERVED0,
    MU_CMD_MODEA_SPI,
    MU_CMD_ROT_POS,
    MU_CMD_ROT_POS_E2P
};
typedef enum MU_CommandEnum MU_Command;

enum MU_WriteVerifyEnum { MU_SETONLY, MU_WRITE, MU_VERIFY };
typedef enum MU_WriteVerifyEnum MU_WriteVerify;

enum MU_ConfigDataEnum {
    MU_RESERVED0,
    MU_FREQ_SPI,
    MU_MASTERVER,
    MU_MASTERREV,
    MU_SLAVE_ID,
    MU_FREQ_SCD,
    MU_CLKENI,
    MU_FREQ_AGS,
    MU_SLAVE_COUNT,
    MU_START_CONTROL_FRAME,
    MU_REG_END,
    MU_FREQ_SSI,
    MU_READ_STATUS_ENABLE,
    MU_USB_PERFORMANCE,
    MU_READ_GAIN_ENABLE,
    MU_BP,
    MU_UPDATE_BISSID_ENABLE,
    MU_ENABLE_TTL
};
typedef enum MU_ConfigDataEnum MU_ConfigData;

enum MU_ErrorTypeEnum {
    MU_NONE,
    MU_HINT,
    MU_WARNING,
    MU_PROGRAMMING_ERROR,
    MU_OPERATING_ERROR,
    MU_COMMUNICATION_ERROR
};
typedef enum MU_ErrorTypeEnum MU_ErrorType;

enum MU_E2PAreaEnum { MU_E2P_CFG };
typedef enum MU_E2PAreaEnum MU_E2PArea;

enum MU_InterfaceInfoEnum { MU_SERIAL_NUMBER, MU_DRIVER_VERSION };
typedef enum MU_InterfaceInfoEnum MU_InterfaceInfo;

enum MU_BissProfile_ { MU_BISS_PROFILE_BP1, MU_BISS_PROFILE_BP3 };
typedef enum MU_BissProfile_ MU_BissProfile;


#define MU_REV_NONE     0x00
#define MU_REV_MU_Y     0x05
#define MU_REV_MU_Y1    0x06
#define MU_REV_MU_Y2    0x07
#define MU_REV_MU_Y2H   0x07
#define MU_REV_MU128_X  0x09
#define MU_REV_MU128_X1 0x0A
#define MU_REV_MU150_0  0x10
#define MU_REV_MU150_1  0x11
#define MU_REV_MU150_3  0x13
#define MU_REV_MU150_X  0x14
#define MU_REV_MU150_X1 0x15
#define MU_REV_MU200_0  0x20
#define MU_REV_MU200_X  0x23
#define MU_REV_MU200_X1 0x24


#endif // MU_3SL_DEFS_H
