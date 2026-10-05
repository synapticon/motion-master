/*
 * Software and its documentation is provided by iC-Haus GmbH or contributors "AS IS" and is
 * subject to the ZVEI General Conditions for the Supply of Products and Services with iC-Haus
 * amendments and the ZVEI Software clause with iC-Haus amendments (http://www.ichaus.de/EULA).
 */

#ifndef MU_3SL_INTERFACE_H
#define MU_3SL_INTERFACE_H


#if defined _WIN32 || defined __CYGWIN__
    #if defined BUILDING_MU_DLL
        #ifdef __GNUC__
            #define MU_PUBLIC __attribute__((dllexport))
        #else
            #define MU_PUBLIC __declspec(dllexport)
        #endif
    #elif defined MU_STATIC_LIB
        #define MU_PUBLIC
    #else
        #ifdef __GNUC__
            #define MU_PUBLIC __attribute__((dllimport))
        #else
            #define MU_PUBLIC __declspec(dllimport)
        #endif
    #endif
#else
    #if __GNUC__ >= 4
        #define MU_PUBLIC __attribute__((visibility("default")))
    #else
        #define MU_PUBLIC
    #endif
#endif


#include "MU_3SL_defs.h"

#ifdef __cplusplus
    #include <cstddef>
    #include <cstdint>
#else
    #include <stdbool.h>
    #include <stddef.h>
    #include <stdint.h>
#endif



#ifdef __cplusplus
extern "C" {
#endif


MU_PUBLIC MU_Error MU_Open(MU_Handle* handle);

MU_PUBLIC MU_Error MU_Close(MU_Handle handle);

MU_PUBLIC MU_Error
MU_SetInterface(MU_Handle handle, MU_Interface interfaceType, const char* interfaceOption);

MU_PUBLIC MU_Error MU_GetInterface(MU_Handle handle, MU_Interface* interfaceType);

MU_PUBLIC MU_Error MU_SetConfig(MU_Handle handle, MU_ConfigData config, uint32_t value);

MU_PUBLIC MU_Error MU_GetConfig(MU_Handle handle, MU_ConfigData config, uint32_t* value);

MU_PUBLIC MU_Error MU_SwitchToBiss(MU_Handle handle, uint32_t* modea, uint32_t* modeb);

MU_PUBLIC MU_Error MU_SwitchToBiss_ex(MU_Handle handle, uint32_t* modea, bool* switched);

MU_PUBLIC MU_Error MU_EnableGetMT(MU_Handle handle, uint32_t* modea, bool* switched);

MU_PUBLIC MU_Error MU_DisableGetMT(MU_Handle handle);

MU_PUBLIC MU_Error MU_Initialize(MU_Handle handle);

MU_PUBLIC MU_Error MU_SetParam(
        MU_Handle handle,
        MU_Param parameter,
        uint32_t valueHigh,
        uint32_t valueLow,
        MU_WriteVerify writeVerify);

MU_PUBLIC MU_Error
MU_GetParam(MU_Handle handle, MU_Param parameter, uint32_t* valueHigh, uint32_t* valueLow);

MU_PUBLIC MU_Error
MU_IsParameterAvailableInUsedRevision(MU_Handle handle, MU_Param parameter, bool* isInRevision);

MU_PUBLIC MU_Error MU_WriteParams(MU_Handle handle, bool verify, bool* valid);

MU_PUBLIC MU_Error MU_ReadParams(MU_Handle handle);

MU_PUBLIC MU_Error MU_ReadStatus(MU_Handle handle);

MU_PUBLIC MU_Error MU_ReadGain(MU_Handle handle);

MU_PUBLIC MU_Error MU_WriteEeprom(MU_Handle handle, MU_E2PArea eepromArea);

MU_PUBLIC MU_Error MU_WriteCmdRegister(MU_Handle handle, MU_Command command);

MU_PUBLIC MU_Error MU_WriteSwitchCommand(MU_Handle handle, uint32_t modeaNew, uint32_t rplNew);

MU_PUBLIC MU_Error MU_ReadSens(MU_Handle handle, ReadSensStruct* data);

MU_PUBLIC MU_Error MU_SaveParams(MU_Handle handle, const char* filepath);

MU_PUBLIC MU_Error
MU_ReadChipRevisionOfParameterFile(MU_Handle handle, const char* filepath, uint8_t* revisionCode);

MU_PUBLIC MU_Error MU_LoadParams(MU_Handle handle, const char* filepath);

MU_PUBLIC MU_Error MU_GetLastError(
        MU_Handle handle,
        MU_Error* lastError,
        MU_ErrorType* lastErrorType,
        char* lastErrorText);

MU_PUBLIC MU_Error MU_CalcPRES_POS(
        MU_Handle handle,
        uint32_t valueHigh,
        uint32_t valueLow,
        MU_WriteVerify writeVerify);

MU_PUBLIC MU_Error MU_GetPRES_POS(MU_Handle handle, uint32_t* valueHigh, uint32_t* valueLow);

MU_PUBLIC MU_Error MU_UseRevision(MU_Handle handle, uint8_t revisionId);

MU_PUBLIC MU_Error MU_ValueOfUsedRevision(MU_Handle handle, uint8_t* revisionId);

MU_PUBLIC MU_Error MU_ReadChipRevision(MU_Handle handle, uint8_t* revisionId);


/*
 * Optional Functions
 */

MU_PUBLIC MU_Error
MU_SetRegister(MU_Handle handle, uint32_t address, uint32_t value, MU_WriteVerify writeVerify);

MU_PUBLIC MU_Error MU_GetRegister(MU_Handle handle, uint32_t address, uint32_t* value);

MU_PUBLIC MU_Error
MU_WriteRegister(MU_Handle handle, uint32_t address, uint32_t* count, uint32_t* values);

MU_PUBLIC MU_Error
MU_ReadRegister(MU_Handle handle, uint32_t address, uint32_t* count, uint32_t* values);

MU_PUBLIC MU_Error
MU_WriteRegisterSSI(MU_Handle handle, uint32_t address, uint32_t* count, uint32_t* values);

MU_PUBLIC MU_Error MU_SaveRegister(
        MU_Handle handle,
        const char* filepath,
        uint32_t address,
        uint32_t* count,
        uint32_t* values);

MU_PUBLIC MU_Error MU_LoadRegister(
        MU_Handle handle,
        const char* filepath,
        uint32_t address,
        uint32_t* count,
        uint32_t* values);

MU_PUBLIC MU_Error MU_SpiActivate(MU_Handle handle, uint32_t data);

MU_PUBLIC MU_Error
MU_GetInterfaceInfo(MU_Handle handle, MU_InterfaceInfo informationType, char* information);

MU_PUBLIC MU_Error MU_WriteRegister_I2C(
        MU_Handle handle,
        uint32_t registerAddress,
        uint32_t registerCount,
        uint32_t i2cSlaveAddress,
        const uint32_t* values);

MU_PUBLIC MU_Error MU_WriteRegister_I2C_Ex(
        MU_Handle handle,
        uint32_t registerCount,
        uint32_t i2cSlaveAddress,
        const uint32_t* values,
        const bool* valid);

MU_PUBLIC MU_Error MU_ReadRegister_I2C(
        MU_Handle handle,
        uint32_t registerAddress,
        uint32_t* registerCount,
        uint32_t i2cSlaveAddress,
        uint32_t* values);

MU_PUBLIC MU_Error MU_SaveRegisterEx(
        MU_Handle handle,
        const char* filepath,
        uint32_t registerAddress,
        uint32_t registerCount,
        uint32_t* values);

MU_PUBLIC MU_Error MU_LoadRegisterEx(
        MU_Handle handle,
        const char* filepath,
        uint32_t registerCount,
        uint32_t* values,
        bool* valid);

MU_PUBLIC MU_Error MU_generateBissIdentifier(MU_Handle handle, uint64_t* deviceID, uint16_t* mfgID);

MU_PUBLIC MU_Error MU_generateBissProfileId(
        MU_Handle handle,
        MU_BissProfile profile,
        uint16_t* bissProfileId);



/*
 * Calibration Functions
 */


MU_PUBLIC MU_Error MU_acquireRawData(
        MU_Handle handle,
        uint16_t* masterRawData,
        uint16_t* noniusRawData,
        size_t nSamples,
        uint32_t slaveId,
        double frameCycleTime_s,
        double clockFreq_hz);

MU_PUBLIC MU_Error MU_activateCalibrationConfig(MU_Handle handle);

MU_PUBLIC MU_Error MU_deactivateCalibrationConfig(MU_Handle handle);


enum MU_Calibration_Unit_ {
    MU_CALIBRATION_UNIT_RESOLUTION,
    MU_CALIBRATION_UNIT_TURN,
    MU_CALIBRATION_UNIT_DEGREE,
    MU_CALIBRATION_UNIT_RAD
};
typedef enum MU_Calibration_Unit_ MU_Calibration_Unit;

struct MU_Calibration_RelativeAnalogTrackAdjustments_ {
    double cosineGain_lsb;
    double sineOffset_lsb;
    double cosineOffset_lsb;
    double phase_lsb;
};
typedef struct MU_Calibration_RelativeAnalogTrackAdjustments_
        MU_Calibration_RelativeAnalogTrackAdjustments;

struct MU_Calibration_AnalogTrackAdjustments_ {
    uint8_t cosineGain;   // GX_{M; N}
    uint8_t sineOffset;   // VOSS_{M; N}
    uint8_t cosineOffset; // VOSC_{M; N}
    uint8_t phase;        // PH_{M; N}
    uint8_t phaseRange;   // PHR_{M; N} (with iC-MU{128} not used; 0 only)
};
typedef struct MU_Calibration_AnalogTrackAdjustments_ MU_Calibration_AnalogTrackAdjustments;

struct MU_Calibration_NoniusTrackOffsetTable_ {
    int8_t spoBase;
    int8_t spoN[16];
};
typedef struct MU_Calibration_NoniusTrackOffsetTable_ MU_Calibration_NoniusTrackOffsetTable;


typedef struct MU_Calibration_ MU_Calibration;
typedef struct MU_CalibrationAnalyzeResult_ MU_CalibrationAnalyzeResult;


MU_PUBLIC MU_Calibration* MU_getCalibration(MU_Handle handle);

MU_PUBLIC MU_Error MU_setCalibration(MU_Handle handle, const MU_Calibration* calibration);


MU_PUBLIC MU_Calibration* MU_createCalibration(uint8_t revisionCode);

MU_PUBLIC void MU_Calibration_delete(MU_Calibration* calibration);


MU_PUBLIC bool MU_Calibration_preconfigureNumberOfMasterPeriods(
        MU_Calibration* calibration,
        unsigned int nMasterPeriods);


MU_PUBLIC MU_CalibrationAnalyzeResult* MU_Calibration_analyzeRawData(
        const MU_Calibration* calibration,
        const uint16_t* masterTrackRawData,
        const uint16_t* noniusTrackRawData,
        size_t nRawDataSamples);

MU_PUBLIC void MU_CalibrationAnalyzeResult_delete(MU_CalibrationAnalyzeResult* analyzeResult);


#define MU_CALIBRATION_ANALYZE_RESULT_LOG_ERRORS   1u
#define MU_CALIBRATION_ANALYZE_RESULT_LOG_WARNINGS 2u
#define MU_CALIBRATION_ANALYZE_RESULT_LOG_NOTE     4u
#define MU_CALIBRATION_ANALYZE_RESULT_LOG_ALL                                                      \
    (MU_CALIBRATION_ANALYZE_RESULT_LOG_ERRORS | MU_CALIBRATION_ANALYZE_RESULT_LOG_WARNINGS         \
     | MU_CALIBRATION_ANALYZE_RESULT_LOG_NOTE)

MU_PUBLIC size_t MU_Calibration_getAnalyzeResultLog(
        const MU_CalibrationAnalyzeResult* analyzeResult,
        char* logDest,
        size_t logDestSize,
        unsigned int logLevel);

MU_PUBLIC bool
MU_Calibration_isAnalogAnalysesValid(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC bool
MU_Calibration_isNoniusAnalysesValid(const MU_CalibrationAnalyzeResult* analyzeResult);


MU_PUBLIC void MU_Calibration_getRelativeMasterTrackAdjustments(
        const MU_CalibrationAnalyzeResult* analyzeResult,
        MU_Calibration_RelativeAnalogTrackAdjustments* dest);

MU_PUBLIC void MU_Calibration_getRelativeNoniusTrackAdjustments(
        const MU_CalibrationAnalyzeResult* analyzeResult,
        MU_Calibration_RelativeAnalogTrackAdjustments* dest);

MU_PUBLIC int
MU_Calibration_numberOfCalculatedMasterPeriods(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC double
MU_Calibration_numberOfRevolutions(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC double
MU_Calibration_numberOfAcquiredMasterPeriods(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC double
MU_Calibration_numberOfAcquiredNoniusPeriods(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC double MU_Calibration_minimalNumberOfSamplesPerMasterPeriod(
        const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC void MU_Calibration_getOptimizedNoniusTrackOffsetTable(
        const MU_CalibrationAnalyzeResult* analyzeResult,
        MU_Calibration_NoniusTrackOffsetTable* dest);

MU_PUBLIC void MU_Calibration_getOptimizedNoniusTrackOffsetTableBaseOnly(
        const MU_CalibrationAnalyzeResult* analyzeResult,
        MU_Calibration_NoniusTrackOffsetTable* dest);

MU_PUBLIC size_t
MU_Calibration_numberOfNoniusCurveSamples(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC const long*
MU_Calibration_noniusPhaseError(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC const long*
MU_Calibration_noniusTrackOffsetCurve(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC const long*
MU_Calibration_noniusPhaseMargin(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC size_t MU_Calibration_calculateNoniusPosition(
        const MU_CalibrationAnalyzeResult* analyzeResult,
        double* dest,
        size_t destBufferSize,
        MU_Calibration_Unit unit,
        bool continuous);

MU_PUBLIC long
MU_Calibration_noniusPhaseMarginMax(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC long
MU_Calibration_noniusPhaseMarginMin(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC long
MU_Calibration_noniusPhaseRangeLimit(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC double
MU_Calibration_noniusUpperPhaseMargin(const MU_CalibrationAnalyzeResult* analyzeResult);

MU_PUBLIC double
MU_Calibration_noniusLowerPhaseMargin(const MU_CalibrationAnalyzeResult* analyzeResult);


MU_PUBLIC bool MU_Calibration_setCurrentAnalogTrackAdjustments(
        MU_Calibration* calibration,
        const MU_Calibration_AnalogTrackAdjustments* masterAdjustments,
        const MU_Calibration_AnalogTrackAdjustments* noniusAdjustments);


MU_PUBLIC bool MU_Calibration_setCurrentNoniusTrackOffsetTable(
        MU_Calibration* calibration,
        const MU_Calibration_NoniusTrackOffsetTable* noniusTrackOffsetTable);

MU_PUBLIC bool MU_Calibration_adjustAnalogByAnalyzeResult(
        MU_Calibration* calibration,
        const MU_CalibrationAnalyzeResult* analyzeResult);


#define MU_ADJUSTMENT_MESSAGE_LOG_ERRORS   1u
#define MU_ADJUSTMENT_MESSAGE_LOG_WARNINGS 2u
#define MU_ADJUSTMENT_MESSAGE_LOG_NOTE     4u
#define MU_ADJUSTMENT_MESSAGE_LOG_ALL                                                              \
    (MU_ADJUSTMENT_MESSAGE_LOG_ERRORS | MU_ADJUSTMENT_MESSAGE_LOG_WARNINGS                         \
     | MU_ADJUSTMENT_MESSAGE_LOG_NOTE)

MU_PUBLIC bool MU_Calibration_isAnalogAnalyzeResultAdjustable(
        const MU_Calibration* calibration,
        const MU_CalibrationAnalyzeResult* analyzeResult,
        char* adjustmentMessage,
        size_t adjustmentMessageBufferSize,
        unsigned int adjustmentMessageLogLevel);


MU_PUBLIC void MU_Calibration_getAnalogMasterTrackAdjustments(
        const MU_Calibration* calibration,
        MU_Calibration_AnalogTrackAdjustments* dest);

MU_PUBLIC void MU_Calibration_getAnalogNoniusTrackAdjustments(
        const MU_Calibration* calibration,
        MU_Calibration_AnalogTrackAdjustments* dest);

MU_PUBLIC void MU_Calibration_getNoniusTrackOffsetTable(
        const MU_Calibration* calibration,
        MU_Calibration_NoniusTrackOffsetTable* dest);


struct MU_NoniusTrackOffsetTableParameters_ {
    uint8_t spoBase;
    uint8_t spoN[15];
};
typedef struct MU_NoniusTrackOffsetTableParameters_ MU_NoniusTrackOffsetTableParameters;

MU_PUBLIC void MU_getNoniusTrackOffsetTableParameters(
        const MU_Calibration_NoniusTrackOffsetTable* noniusTrackOffsetTable,
        MU_NoniusTrackOffsetTableParameters* dest);

MU_PUBLIC void MU_getNoniusTrackOffsetTableByParameters(
        const MU_NoniusTrackOffsetTableParameters* trackOffsetTableParameters,
        MU_Calibration_NoniusTrackOffsetTable* dest);


MU_PUBLIC MU_Error MU_acquireMtSyncData(MU_Handle handle, MU_MtSyncData* syncData, size_t nSamples);

MU_PUBLIC MU_Error MU_activateMtSyncConfig(MU_Handle handle);

MU_PUBLIC MU_Error MU_deactivateMtSyncConfig(MU_Handle handle);


MU_PUBLIC MU_Error
MU_acquire3TrackMtSyncData(MU_Handle handle, MU_MtSyncData* syncData, size_t nSamples);

MU_PUBLIC MU_Error MU_activate3TrackMtSyncConfig(MU_Handle handle);

MU_PUBLIC MU_Error MU_deactivate3TrackMtSyncConfig(MU_Handle handle);


typedef struct MU_MtSync_ MU_MtSync;
typedef struct MU_MtAnalyzeResult_ MU_MtAnalyzeResult;


MU_PUBLIC MU_MtSync* MU_getMtSync(MU_Handle handle);

MU_PUBLIC MU_Error MU_updateMtSync(MU_Handle handle, const MU_MtAnalyzeResult* analyzeResult);

MU_PUBLIC MU_MtSync* MU_createMtSync(
        const MU_Calibration* muCalibration,
        size_t nMtSyncBits,
        bool mtMovementIsReverse);

MU_PUBLIC void MU_MtSync_delete(MU_MtSync* mtSync);


MU_PUBLIC size_t MU_MtSync_calculatePosition(
        const MU_MtSync* mtSync,
        double* dest,
        const MU_MtSyncData* syncData,
        size_t nDataSamples,
        uint8_t mtTrackOffset,
        uint64_t resolution,
        MU_Calibration_Unit unit,
        bool continuous);


MU_PUBLIC MU_MtAnalyzeResult* MU_MtSync_analyzeData(
        const MU_MtSync* mtSync,
        const MU_MtSyncData* syncData,
        size_t nDataSamples);

MU_PUBLIC void MU_MtAnalyzeResult_delete(MU_MtAnalyzeResult* analyzeResult);


MU_PUBLIC uint8_t MU_MtAnalyzeResult_optimalSpoMt(const MU_MtAnalyzeResult* analyzeResult);

MU_PUBLIC size_t MU_MtAnalyzeResult_calculateOffsetError(
        const MU_MtAnalyzeResult* analyzeResult,
        double* dest,
        size_t destBufferSize,
        uint8_t mtTrackOffset);

MU_PUBLIC double MU_MtAnalyzeResult_maxOffsetError(
        const MU_MtAnalyzeResult* analyzeResult,
        uint8_t mtTrackOffset);

MU_PUBLIC double MU_MtAnalyzeResult_minOffsetError(
        const MU_MtAnalyzeResult* analyzeResult,
        uint8_t mtTrackOffset);

MU_PUBLIC double MU_MtAnalyzeResult_offsetErrorRangeLimit(const MU_MtAnalyzeResult* analyzeResult);

MU_PUBLIC double MU_MtAnalyzeResult_upperOffsetErrorMargin(
        const MU_MtAnalyzeResult* analyzeResult,
        uint8_t mtTrackOffset);

MU_PUBLIC double MU_MtAnalyzeResult_lowerOffsetErrorMargin(
        const MU_MtAnalyzeResult* analyzeResult,
        uint8_t mtTrackOffset);


MU_PUBLIC double MU_Interface_nextPossibleFrameCycleTime(
        MU_Interface interfaceType,
        double frameCycleTime_s);

MU_PUBLIC double MU_Interface_nearestPossibleFrameCycleTime(
        MU_Interface interfaceType,
        double frameCycleTime_s);

MU_PUBLIC double
MU_Interface_nextPossibleClockFreq(MU_Interface interfaceType, double clockFreq_hz);

MU_PUBLIC double
MU_Interface_nearestPossibleClockFreq(MU_Interface interfaceType, double clockFreq_hz);



MU_PUBLIC MU_Error MU_GetDLLVersion(uint32_t* version);

MU_PUBLIC const char* MU_GetVersionString(void);

MU_PUBLIC uint16_t MU_GetVersionMajor(void);
MU_PUBLIC uint16_t MU_GetVersionMinor(void);
MU_PUBLIC uint16_t MU_GetVersionPatch(void);
MU_PUBLIC uint16_t MU_GetVersionBuild(void);

MU_PUBLIC const char* MU_GetVersionSuffixString(void);


#ifdef __cplusplus
}
#endif

#endif // MU_3SL_INTERFACE_H
