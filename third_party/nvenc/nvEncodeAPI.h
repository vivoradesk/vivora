// Minimal NVENC API declarations from NVIDIA Video Codec SDK 12.2
// Structures, enums, and function typedefs required for hardware H.265 encoding.
// Full SDK: https://developer.nvidia.com/video-codec-sdk
// License: MIT (NVIDIA Video Codec SDK headers are redistributable)

#pragma once

#include <stdint.h>

// GUID is provided by <guiddef.h> on the Windows toolchain.  On Linux/macOS
// it doesn't exist, so define the identical layout ourselves (this mirrors
// the fallback in the real NVIDIA nvEncodeAPI.h).  Likewise neutralise
// __stdcall: it is a Windows-only calling-convention keyword — on the SysV
// x86-64 ABI the NVENC entry points use the default convention, so the
// attribute must compile to nothing.
#if defined(_WIN32)
#include <guiddef.h>
#else
#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID;
#endif
#ifndef __stdcall
#define __stdcall
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
#define NVENCAPI_MAJOR_VERSION 12
#define NVENCAPI_MINOR_VERSION 2
#define NVENCAPI_VERSION       (NVENCAPI_MAJOR_VERSION | (NVENCAPI_MINOR_VERSION << 24))

#define NVENCAPI_STRUCT_VERSION(typeName, ver) \
    ((uint32_t)NVENCAPI_VERSION | ((ver) << 16) | (0x7 << 28))

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_SUCCESS                     = 0,
    NV_ENC_ERR_NO_ENCODE_DEVICE        = 1,
    NV_ENC_ERR_UNSUPPORTED_DEVICE      = 2,
    NV_ENC_ERR_INVALID_ENCODERDEVICE   = 3,
    NV_ENC_ERR_INVALID_DEVICE          = 4,
    NV_ENC_ERR_DEVICE_NOT_EXIST        = 5,
    NV_ENC_ERR_INVALID_PTR             = 6,
    NV_ENC_ERR_INVALID_EVENT           = 7,
    NV_ENC_ERR_INVALID_PARAM           = 8,
    NV_ENC_ERR_INVALID_CALL            = 9,
    NV_ENC_ERR_OUT_OF_MEMORY           = 10,
    NV_ENC_ERR_ENCODER_NOT_INITIALIZED = 11,
    NV_ENC_ERR_UNSUPPORTED_PARAM       = 12,
    NV_ENC_ERR_LOCK_BUSY               = 13,
    NV_ENC_ERR_NOT_ENOUGH_BUFFER       = 14,
    NV_ENC_ERR_INVALID_VERSION         = 15,
    NV_ENC_ERR_MAP_FAILED              = 16,
    NV_ENC_ERR_NEED_MORE_INPUT         = 17,
    NV_ENC_ERR_ENCODER_BUSY            = 18,
    NV_ENC_ERR_EVENT_NOT_REGISTERD     = 19,
    NV_ENC_ERR_GENERIC                 = 20,
    NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY = 21,
    NV_ENC_ERR_UNIMPLEMENTED           = 22,
    NV_ENC_ERR_RESOURCE_REGISTER_FAILED = 23,
    NV_ENC_ERR_RESOURCE_NOT_REGISTERED = 24,
    NV_ENC_ERR_RESOURCE_NOT_MAPPED     = 25,
} NVENCSTATUS;

// ---------------------------------------------------------------------------
// Device type
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_DEVICE_TYPE_DIRECTX    = 0,
    NV_ENC_DEVICE_TYPE_CUDA       = 1,
    NV_ENC_DEVICE_TYPE_OPENGL     = 2,
} NV_ENC_DEVICE_TYPE;

// ---------------------------------------------------------------------------
// Codec GUIDs
// ---------------------------------------------------------------------------
// {6BC82762-4E63-4CA4-AA85-1E50F321F6BF}
static const GUID NV_ENC_CODEC_H264_GUID =
    { 0x6BC82762, 0x4E63, 0x4CA4, { 0xAA, 0x85, 0x1E, 0x50, 0xF3, 0x21, 0xF6, 0xBF } };
// {790CDC88-4522-4D7B-9425-BDA9975F7603}
static const GUID NV_ENC_CODEC_HEVC_GUID =
    { 0x790CDC88, 0x4522, 0x4D7B, { 0x94, 0x25, 0xBD, 0xA9, 0x97, 0x5F, 0x76, 0x03 } };

// ---------------------------------------------------------------------------
// Preset GUIDs (SDK 12.x)
// ---------------------------------------------------------------------------
// P1 (fastest)
// {FC0A8D3E-45F8-4CF8-80C7-298871590EBF}
static const GUID NV_ENC_PRESET_P1_GUID =
    { 0xFC0A8D3E, 0x45F8, 0x4CF8, { 0x80, 0xC7, 0x29, 0x88, 0x71, 0x59, 0x0E, 0xBF } };
// P4 (balanced)
// {B514C39A-09A3-4571-1082-4181856C3484}
static const GUID NV_ENC_PRESET_P4_GUID =
    { 0xB514C39A, 0x09A3, 0x4571, { 0x10, 0x82, 0x41, 0x81, 0x85, 0x6C, 0x34, 0x84 } };

// ---------------------------------------------------------------------------
// Profile GUIDs
// ---------------------------------------------------------------------------
// HEVC Main
// {B514C39A-09A3-4571-1082-66FE904458D5}
static const GUID NV_ENC_HEVC_PROFILE_MAIN_GUID =
    { 0xB514C39A, 0x09A3, 0x4571, { 0x10, 0x82, 0x66, 0xFE, 0x90, 0x44, 0x58, 0xD5 } };
// HEVC Main10
// {FA4D2B6C-3A5B-411A-8018-0A3F5E3C9BE5}
static const GUID NV_ENC_HEVC_PROFILE_MAIN10_GUID =
    { 0xFA4D2B6C, 0x3A5B, 0x411A, { 0x80, 0x18, 0x0A, 0x3F, 0x5E, 0x3C, 0x9B, 0xE5 } };
// H.264 Baseline
// {0727BCAA-78C4-4C83-8C2F-EF3DFF267C6A}
static const GUID NV_ENC_H264_PROFILE_BASELINE_GUID =
    { 0x0727BCAA, 0x78C4, 0x4C83, { 0x8C, 0x2F, 0xEF, 0x3D, 0xFF, 0x26, 0x7C, 0x6A } };
// H.264 Main
// {60B5C1D4-67FE-4790-94D5-C4726D7B6E6D}
static const GUID NV_ENC_H264_PROFILE_MAIN_GUID =
    { 0x60B5C1D4, 0x67FE, 0x4790, { 0x94, 0xD5, 0xC4, 0x72, 0x6D, 0x7B, 0x6E, 0x6D } };
// H.264 High
// {E7CBC309-4F7A-4B89-AF2A-D537C92BE310}
static const GUID NV_ENC_H264_PROFILE_HIGH_GUID =
    { 0xE7CBC309, 0x4F7A, 0x4B89, { 0xAF, 0x2A, 0xD5, 0x37, 0xC9, 0x2B, 0xE3, 0x10 } };

// ---------------------------------------------------------------------------
// Tuning info
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_TUNING_INFO_UNDEFINED           = 0,
    NV_ENC_TUNING_INFO_HIGH_QUALITY        = 1,
    NV_ENC_TUNING_INFO_LOW_LATENCY         = 2,
    NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY   = 3,
    NV_ENC_TUNING_INFO_LOSSLESS            = 4,
} NV_ENC_TUNING_INFO;

// ---------------------------------------------------------------------------
// Rate control mode
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_PARAMS_RC_CONSTQP    = 0x0,
    NV_ENC_PARAMS_RC_VBR        = 0x1,
    NV_ENC_PARAMS_RC_CBR        = 0x2,
} NV_ENC_PARAMS_RC_MODE;

// ---------------------------------------------------------------------------
// Buffer formats
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_BUFFER_FORMAT_UNDEFINED          = 0x00000000,
    NV_ENC_BUFFER_FORMAT_NV12               = 0x00000001,
    NV_ENC_BUFFER_FORMAT_YV12               = 0x00000010,
    NV_ENC_BUFFER_FORMAT_IYUV               = 0x00000100,
    NV_ENC_BUFFER_FORMAT_YUV444             = 0x00001000,
    NV_ENC_BUFFER_FORMAT_YUV420_10BIT       = 0x00010000,
    NV_ENC_BUFFER_FORMAT_YUV444_10BIT       = 0x00100000,
    NV_ENC_BUFFER_FORMAT_ARGB               = 0x01000000,
    NV_ENC_BUFFER_FORMAT_ARGB10             = 0x02000000,
    NV_ENC_BUFFER_FORMAT_AYUV               = 0x04000000,
    NV_ENC_BUFFER_FORMAT_ABGR               = 0x10000000,
    NV_ENC_BUFFER_FORMAT_ABGR10             = 0x20000000,
} NV_ENC_BUFFER_FORMAT;

// ---------------------------------------------------------------------------
// Picture type
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_PIC_TYPE_P               = 0,
    NV_ENC_PIC_TYPE_B               = 1,
    NV_ENC_PIC_TYPE_I               = 2,
    NV_ENC_PIC_TYPE_IDR             = 3,
    NV_ENC_PIC_TYPE_BI              = 4,
    NV_ENC_PIC_TYPE_SKIPPED         = 5,
    NV_ENC_PIC_TYPE_INTRA_REFRESH   = 6,
    NV_ENC_PIC_TYPE_NONREF_P        = 7,
    NV_ENC_PIC_TYPE_UNKNOWN         = 0xFF,
} NV_ENC_PIC_TYPE;

// ---------------------------------------------------------------------------
// Picture flags
// ---------------------------------------------------------------------------
#define NV_ENC_PIC_FLAG_FORCEINTRA    0x01
#define NV_ENC_PIC_FLAG_FORCEIDR      0x02
#define NV_ENC_PIC_FLAG_OUTPUT_SPSPPS 0x04
#define NV_ENC_PIC_FLAG_EOS           0x08

// ---------------------------------------------------------------------------
// Input resource type
// ---------------------------------------------------------------------------
typedef enum {
    NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX     = 0x0,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR = 0x1,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY     = 0x2,
    NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX    = 0x3,
} NV_ENC_INPUT_RESOURCE_TYPE;

// ---------------------------------------------------------------------------
// Forward declarations for opaque types
// ---------------------------------------------------------------------------
typedef void* NV_ENC_INPUT_PTR;
typedef void* NV_ENC_OUTPUT_PTR;
typedef void* NV_ENC_REGISTERED_PTR;

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------

typedef struct _NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    uint32_t            version;
    NV_ENC_DEVICE_TYPE  deviceType;
    void*               device;
    void*               reserved;
    uint32_t            apiVersion;
    uint32_t            reserved1[253];
    void*               reserved2[64];
} NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS;
#define NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER \
    NVENCAPI_STRUCT_VERSION(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS, 1)

typedef struct _NV_ENC_RC_PARAMS {
    uint32_t                version;
    NV_ENC_PARAMS_RC_MODE   rateControlMode;
    uint32_t                constQP_I;             // unused for CBR
    uint32_t                constQP_P;
    uint32_t                constQP_B;
    uint32_t                averageBitRate;
    uint32_t                maxBitRate;
    uint32_t                vbvBufferSize;
    uint32_t                vbvInitialDelay;
    uint32_t                enableMinQP     : 1;
    uint32_t                enableMaxQP     : 1;
    uint32_t                enableInitialRCQP : 1;
    uint32_t                enableAQ        : 1;
    uint32_t                reservedBitField1 : 1;
    uint32_t                enableLookahead : 1;
    uint32_t                disableIadapt   : 1;
    uint32_t                disableBadapt   : 1;
    uint32_t                enableTemporalAQ : 1;
    uint32_t                zeroReorderDelay : 1;
    uint32_t                enableNonRefP   : 1;
    uint32_t                strictGOPTarget : 1;
    uint32_t                aqStrength      : 4;
    uint32_t                reservedBitFields : 16;
    uint32_t                minQP_I;
    uint32_t                minQP_P;
    uint32_t                minQP_B;
    uint32_t                maxQP_I;
    uint32_t                maxQP_P;
    uint32_t                maxQP_B;
    uint32_t                initialRCQP_I;
    uint32_t                initialRCQP_P;
    uint32_t                initialRCQP_B;
    uint32_t                temporallayerIdxMask;
    uint8_t                 temporalLayerQP[8];
    uint16_t                targetQuality;
    uint16_t                targetQualityLSB;
    uint16_t                lookaheadDepth;
    uint16_t                lowDelayKeyFrameScale;
    uint32_t                reserved1[14];
} NV_ENC_RC_PARAMS;
#define NV_ENC_RC_PARAMS_VER NVENCAPI_STRUCT_VERSION(NV_ENC_RC_PARAMS, 1)

typedef struct _NV_ENC_CONFIG_HEVC {
    uint32_t level;
    uint32_t tier;
    uint32_t minCUSize;
    uint32_t maxCUSize;
    uint32_t useConstrainedIntraPred : 1;
    uint32_t disableDeblockAcrossSliceBoundary : 1;
    uint32_t outputBufferingPeriodSEI : 1;
    uint32_t outputPictureTimingSEI : 1;
    uint32_t outputAUD : 1;
    uint32_t enableLTR : 1;
    uint32_t disableSPSPPS : 1;
    uint32_t repeatSPSPPS : 1;
    uint32_t enableIntraRefresh : 1;
    uint32_t chromaFormatIDC : 2;
    uint32_t pixelBitDepthMinus8 : 3;
    uint32_t enableFillerDataInsertion : 1;
    uint32_t enableConstrainedEncoding : 1;
    uint32_t enableAlphaLayerEncoding : 1;
    uint32_t singleSliceIntraRefresh  : 1;
    uint32_t reservedBitFields : 14;
    uint32_t idrPeriod;
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t maxNumRefFramesInDPB;
    uint32_t ltrNumFrames;
    uint32_t vpsId;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t maxTemporalLayersMinus1;
    uint32_t hevcVUIParameters[28];  // NV_ENC_CONFIG_HEVC_VUI_PARAMETERS (13 fields + 15 reserved)
    uint32_t ltrTrustMode;
    uint32_t reserved1[187];
    void*    reserved2[64];
} NV_ENC_CONFIG_HEVC;

typedef struct _NV_ENC_CONFIG_H264 {
    uint32_t enableTemporalSVC : 1;
    uint32_t enableStereoMVC : 1;
    uint32_t hierarchicalPFrames : 1;
    uint32_t hierarchicalBFrames : 1;
    uint32_t outputBufferingPeriodSEI : 1;
    uint32_t outputPictureTimingSEI : 1;
    uint32_t outputAUD : 1;
    uint32_t disableSPSPPS : 1;
    uint32_t outputFramePackingSEI : 1;
    uint32_t outputRecoveryPointSEI : 1;
    uint32_t enableIntraRefresh : 1;          // bit 10
    uint32_t enableConstrainedEncoding : 1;
    uint32_t repeatSPSPPS : 1;                 // bit 12
    uint32_t enableVFR : 1;
    uint32_t enableLTR : 1;
    uint32_t qpPrimeYZeroTransformBypassFlag : 1;
    uint32_t useConstrainedIntraPred : 1;
    uint32_t enableFillerDataInsertion : 1;
    uint32_t disableSVCPrefixNalu : 1;
    uint32_t enableScalabilityInfoSEI : 1;
    uint32_t singleSliceIntraRefresh : 1;
    uint32_t enableAlphaLayerEncoding : 1;
    uint32_t reservedBitFields : 10;
    uint32_t level;
    uint32_t idrPeriod;
    uint32_t separateColourPlaneFlag;
    uint32_t disableDeblockingFilterIDC;
    uint32_t numTemporalLayers;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t adaptiveTransformMode;            // NV_ENC_H264_ADAPTIVE_TRANSFORM_MODE
    uint32_t fmoMode;                          // NV_ENC_H264_FMO_MODE
    uint32_t bdirectMode;                      // NV_ENC_H264_BDIRECT_MODE
    uint32_t entropyCodingMode;                // NV_ENC_H264_ENTROPY_CODING_MODE
    uint32_t stereoMode;                       // NV_ENC_STEREO_PACKING_MODE
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t maxNumRefFrames;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t h264VUIParameters[30];            // NV_ENC_CONFIG_H264_VUI_PARAMETERS
    uint32_t ltrNumFrames;
    uint32_t ltrTrustMode;
    uint32_t chromaFormatIDC;
    uint32_t maxTemporalLayers;
    uint32_t useBFramesAsRef;                  // NV_ENC_BFRAME_REF_MODE
    uint32_t numRefL0;                         // NV_ENC_NUM_REF_FRAMES
    uint32_t numRefL1;                         // NV_ENC_NUM_REF_FRAMES
    uint32_t reserved1[267];
    void*    reserved2[64];
} NV_ENC_CONFIG_H264;

typedef union _NV_ENC_CODEC_CONFIG {
    NV_ENC_CONFIG_H264 h264Config;
    NV_ENC_CONFIG_HEVC hevcConfig;
    uint32_t reserved[512];
} NV_ENC_CODEC_CONFIG;

typedef struct _NV_ENC_CONFIG {
    uint32_t             version;
    GUID                 profileGUID;
    uint32_t             gopLength;
    int32_t              frameIntervalP;
    uint32_t             monoChromeEncoding;
    uint32_t             frameFieldMode;
    uint32_t             mvPrecision;
    NV_ENC_RC_PARAMS     rcParams;
    NV_ENC_CODEC_CONFIG  encodeCodecConfig;
    uint32_t             reserved[278];
    void*                reserved2[64];
} NV_ENC_CONFIG;
#define NV_ENC_CONFIG_VER (NVENCAPI_STRUCT_VERSION(NV_ENC_CONFIG, 9) | (1<<31))

typedef struct _NV_ENC_PRESET_CONFIG {
    uint32_t       version;
    NV_ENC_CONFIG  presetCfg;
    uint32_t       reserved1[255];
    void*          reserved2[64];
} NV_ENC_PRESET_CONFIG;
#define NV_ENC_PRESET_CONFIG_VER (NVENCAPI_STRUCT_VERSION(NV_ENC_PRESET_CONFIG, 5) | (1<<31))

typedef struct _NV_ENC_INITIALIZE_PARAMS {
    uint32_t            version;
    GUID                encodeGUID;
    GUID                presetGUID;
    uint32_t            encodeWidth;
    uint32_t            encodeHeight;
    uint32_t            darWidth;
    uint32_t            darHeight;
    uint32_t            frameRateNum;
    uint32_t            frameRateDen;
    uint32_t            enableEncodeAsync;     // full uint32_t, NOT bitfield
    uint32_t            enablePTD;             // full uint32_t, NOT bitfield
    uint32_t            bitfields;             // all 32 bits of enable* bit-fields (keep zero)
    uint32_t            privDataSize;
    uint32_t            reserved_priv;         // explicit reserved field (SDK 12.2)
    void*               privData;
    NV_ENC_CONFIG*      encodeConfig;
    uint32_t            maxEncodeWidth;
    uint32_t            maxEncodeHeight;
    // In real SDK 12.2: NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE[2] = 2 * 16 bytes
    uint32_t            maxMEHintCountsPerBlock[8];
    NV_ENC_TUNING_INFO  tuningInfo;
    uint32_t            bufferFormat;          // NV_ENC_BUFFER_FORMAT
    uint32_t            numStateBuffers;
    uint32_t            outputStatsLevel;      // NV_ENC_OUTPUT_STATS_LEVEL
    uint32_t            reserved[284];
    void*               reserved2[64];
} NV_ENC_INITIALIZE_PARAMS;
#define NV_ENC_INITIALIZE_PARAMS_VER (NVENCAPI_STRUCT_VERSION(NV_ENC_INITIALIZE_PARAMS, 7) | (1<<31))

typedef struct _NV_ENC_REGISTER_RESOURCE {
    uint32_t                   version;
    NV_ENC_INPUT_RESOURCE_TYPE resourceType;
    uint32_t                   width;
    uint32_t                   height;
    uint32_t                   pitch;
    uint32_t                   subResourceIndex;
    void*                      resourceToRegister;
    NV_ENC_REGISTERED_PTR      registeredResource;
    NV_ENC_BUFFER_FORMAT       bufferFormat;
    uint32_t                   bufferUsage;
    void*                      pInputFencePoint;    // D3D12 only (we keep NULL)
    uint32_t                   chromaOffset[2];     // [out]
    uint32_t                   reserved[246];
    void*                      reserved2[61];
} NV_ENC_REGISTER_RESOURCE;
#define NV_ENC_REGISTER_RESOURCE_VER NVENCAPI_STRUCT_VERSION(NV_ENC_REGISTER_RESOURCE, 5)

typedef struct _NV_ENC_MAP_INPUT_RESOURCE {
    uint32_t              version;
    uint32_t              subResourceIndex;
    uint32_t              reserved;
    NV_ENC_REGISTERED_PTR registeredResource;
    NV_ENC_INPUT_PTR      mappedResource;
    NV_ENC_BUFFER_FORMAT  mappedBufferFmt;
    uint32_t              reserved1[62];
    void*                 reserved2[63];
} NV_ENC_MAP_INPUT_RESOURCE;
#define NV_ENC_MAP_INPUT_RESOURCE_VER NVENCAPI_STRUCT_VERSION(NV_ENC_MAP_INPUT_RESOURCE, 4)

typedef struct _NV_ENC_CREATE_BITSTREAM_BUFFER {
    uint32_t          version;
    uint32_t          size;                // deprecated
    uint32_t          memoryHeap;          // deprecated
    uint32_t          reserved;
    NV_ENC_OUTPUT_PTR bitstreamBuffer;
    void*             bitstreamBufferPtr;  // deprecated
    uint32_t          reserved1[58];
    void*             reserved2[64];
} NV_ENC_CREATE_BITSTREAM_BUFFER;
#define NV_ENC_CREATE_BITSTREAM_BUFFER_VER NVENCAPI_STRUCT_VERSION(NV_ENC_CREATE_BITSTREAM_BUFFER, 1)

// NVENC-allocated host-accessible input surface.  Used by the Linux/CUDA
// path which feeds CPU BGRx frames (from PipeWire SHM) — Lock gives a
// writable pointer, we memcpy the rows, Unlock, then encode.  The driver
// handles the host→device upload internally, so no CUDA memcpy is needed.
typedef struct _NV_ENC_CREATE_INPUT_BUFFER {
    uint32_t             version;
    uint32_t             width;
    uint32_t             height;
    uint32_t             memoryHeap;          // deprecated NV_ENC_MEMORY_HEAP
    NV_ENC_BUFFER_FORMAT bufferFmt;
    uint32_t             reserved;
    NV_ENC_INPUT_PTR     inputBuffer;         // [out]
    void*                pSysMemBuffer;
    uint32_t             reserved1[57];
    void*                reserved2[63];
} NV_ENC_CREATE_INPUT_BUFFER;
#define NV_ENC_CREATE_INPUT_BUFFER_VER NVENCAPI_STRUCT_VERSION(NV_ENC_CREATE_INPUT_BUFFER, 1)

typedef struct _NV_ENC_LOCK_INPUT_BUFFER {
    uint32_t          version;
    uint32_t          doNotWait : 1;
    uint32_t          reservedBitFields : 31;
    NV_ENC_INPUT_PTR  inputBuffer;
    void*             bufferDataPtr;          // [out] CPU-writable pixels
    uint32_t          pitch;                  // [out] row stride in bytes
    uint32_t          reserved1[251];
    void*             reserved2[64];
} NV_ENC_LOCK_INPUT_BUFFER;
#define NV_ENC_LOCK_INPUT_BUFFER_VER NVENCAPI_STRUCT_VERSION(NV_ENC_LOCK_INPUT_BUFFER, 1)

typedef struct _NV_ENC_PIC_PARAMS {
    uint32_t             version;
    uint32_t             inputWidth;
    uint32_t             inputHeight;
    uint32_t             inputPitch;
    uint32_t             encodePicFlags;
    uint32_t             frameIdx;
    uint64_t             inputTimeStamp;      // opaque, matches LockBitstream.outputTimeStamp
    uint64_t             inputDuration;
    NV_ENC_INPUT_PTR     inputBuffer;
    NV_ENC_OUTPUT_PTR    outputBitstream;
    void*                completionEvent;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    uint32_t             pictureStruct;       // NV_ENC_PIC_STRUCT
    uint32_t             pictureType;         // NV_ENC_PIC_TYPE
    // NV_ENC_CODEC_PIC_PARAMS union = uint32_t reserved[256] = 1024 bytes
    uint32_t             codecPicParams[256];
    // NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE meHintCountsPerBlock[2] = 2 * 16 bytes
    uint32_t             meHintCountsPerBlock[8];
    void*                meExternalHints;
    uint32_t             reserved2[7];
    void*                reserved5[2];
    int8_t*              qpDeltaMap;
    uint32_t             qpDeltaMapSize;
    uint32_t             reservedBitFields;
    uint16_t             meHintRefPicDist[2];
    uint32_t             reserved4;
    NV_ENC_INPUT_PTR     alphaBuffer;
    void*                meExternalSbHints;
    uint32_t             meSbHintsCount;
    uint32_t             stateBufferIdx;
    NV_ENC_OUTPUT_PTR    outputReconBuffer;
    uint32_t             reserved3[284];
    void*                reserved6[57];
} NV_ENC_PIC_PARAMS;
#define NV_ENC_PIC_PARAMS_VER (NVENCAPI_STRUCT_VERSION(NV_ENC_PIC_PARAMS, 7) | (1<<31))

typedef struct _NV_ENC_LOCK_BITSTREAM {
    uint32_t            version;
    uint32_t            doNotWait          : 1;
    uint32_t            ltrFrame           : 1;
    uint32_t            getRCStats         : 1;
    uint32_t            reservedBitFields  : 29;
    void*               outputBitstream;
    uint32_t*           sliceOffsets;
    uint32_t            frameIdx;
    uint32_t            hwEncodeStatus;
    uint32_t            numSlices;
    uint32_t            bitstreamSizeInBytes;
    uint64_t            outputTimeStamp;
    uint64_t            outputDuration;
    void*               bitstreamBufferPtr;
    NV_ENC_PIC_TYPE     pictureType;
    uint32_t            pictureStruct;        // NV_ENC_PIC_STRUCT
    uint32_t            frameAvgQP;
    uint32_t            frameSatd;
    uint32_t            ltrFrameIdx;
    uint32_t            ltrFrameBitmap;
    uint32_t            temporalId;
    uint32_t            intraMBCount;
    uint32_t            interMBCount;
    int32_t             averageMVX;
    int32_t             averageMVY;
    uint32_t            alphaLayerSizeInBytes;
    uint32_t            outputStatsPtrSize;
    uint32_t            reserved;
    void*               outputStatsPtr;
    uint32_t            frameIdxDisplay;
    uint32_t            reserved1[219];
    void*               reserved2[63];
    uint32_t            reservedInternal[8];
} NV_ENC_LOCK_BITSTREAM;
#define NV_ENC_LOCK_BITSTREAM_VER (NVENCAPI_STRUCT_VERSION(NV_ENC_LOCK_BITSTREAM, 2) | (1<<31))

typedef struct _NV_ENC_RECONFIGURE_PARAMS {
    uint32_t                    version;
    NV_ENC_INITIALIZE_PARAMS    reInitEncodeParams;
    uint32_t                    resetEncoder : 1;
    uint32_t                    forceIDR     : 1;
    uint32_t                    reserved     : 30;
    uint32_t                    reserved2[255];
} NV_ENC_RECONFIGURE_PARAMS;
#define NV_ENC_RECONFIGURE_PARAMS_VER (NVENCAPI_STRUCT_VERSION(NV_ENC_RECONFIGURE_PARAMS, 2) | (1<<31))

// ---------------------------------------------------------------------------
// Function pointer table — order MUST match the NVIDIA Video Codec SDK 12.2.
// The driver fills these by offset; any mismatch → wrong function called.
// ---------------------------------------------------------------------------
typedef struct _NV_ENCODE_API_FUNCTION_LIST {
    uint32_t    version;                                                    // +0
    uint32_t    reserved;                                                   // +4

    // Slot  0: deprecated (old session open)
    void*       nvEncOpenEncodeSession;
    // Slot  1:
    NVENCSTATUS (__stdcall *nvEncGetEncodeGUIDCount)(void* encoder, uint32_t* count);
    // Slot  2:
    NVENCSTATUS (__stdcall *nvEncGetEncodeProfileGUIDCount)(void* encoder, GUID encodeGUID, uint32_t* count);
    // Slot  3:
    NVENCSTATUS (__stdcall *nvEncGetEncodeProfileGUIDs)(void* encoder, GUID encodeGUID, GUID* guids, uint32_t count, uint32_t* actual);
    // Slot  4:
    NVENCSTATUS (__stdcall *nvEncGetEncodeGUIDs)(void* encoder, GUID* guids, uint32_t count, uint32_t* actual);
    // Slot  5:
    NVENCSTATUS (__stdcall *nvEncGetInputFormatCount)(void* encoder, GUID encodeGUID, uint32_t* count);
    // Slot  6:
    NVENCSTATUS (__stdcall *nvEncGetInputFormats)(void* encoder, GUID encodeGUID, NV_ENC_BUFFER_FORMAT* fmts, uint32_t count, uint32_t* actual);
    // Slot  7:
    NVENCSTATUS (__stdcall *nvEncGetEncodeCaps)(void* encoder, GUID encodeGUID, void* capsParam, int* capsVal);
    // Slot  8:
    NVENCSTATUS (__stdcall *nvEncGetEncodePresetCount)(void* encoder, GUID encodeGUID, uint32_t* count);
    // Slot  9:
    NVENCSTATUS (__stdcall *nvEncGetEncodePresetGUIDs)(void* encoder, GUID encodeGUID, GUID* guids, uint32_t count, uint32_t* actual);
    // Slot 10: deprecated (non-Ex preset config, no tuning info)
    void*       nvEncGetEncodePresetConfig;
    // Slot 11:
    NVENCSTATUS (__stdcall *nvEncInitializeEncoder)(void* encoder, NV_ENC_INITIALIZE_PARAMS* params);
    // Slot 12:
    NVENCSTATUS (__stdcall *nvEncCreateInputBuffer)(void* encoder, void* createInputBufferParams);
    // Slot 13:
    NVENCSTATUS (__stdcall *nvEncDestroyInputBuffer)(void* encoder, NV_ENC_INPUT_PTR inputBuffer);
    // Slot 14:
    NVENCSTATUS (__stdcall *nvEncCreateBitstreamBuffer)(void* encoder, NV_ENC_CREATE_BITSTREAM_BUFFER* params);
    // Slot 15:
    NVENCSTATUS (__stdcall *nvEncDestroyBitstreamBuffer)(void* encoder, NV_ENC_OUTPUT_PTR bitstreamBuffer);
    // Slot 16:
    NVENCSTATUS (__stdcall *nvEncEncodePicture)(void* encoder, NV_ENC_PIC_PARAMS* params);
    // Slot 17:
    NVENCSTATUS (__stdcall *nvEncLockBitstream)(void* encoder, NV_ENC_LOCK_BITSTREAM* lockBitstreamParams);
    // Slot 18:
    NVENCSTATUS (__stdcall *nvEncUnlockBitstream)(void* encoder, NV_ENC_OUTPUT_PTR bitstreamBuffer);
    // Slot 19:
    NVENCSTATUS (__stdcall *nvEncLockInputBuffer)(void* encoder, void* lockInputBufferParams);
    // Slot 20:
    NVENCSTATUS (__stdcall *nvEncUnlockInputBuffer)(void* encoder, NV_ENC_INPUT_PTR inputBuffer);
    // Slot 21:
    NVENCSTATUS (__stdcall *nvEncGetEncodeStats)(void* encoder, void* encodeStats);
    // Slot 22:
    NVENCSTATUS (__stdcall *nvEncGetSequenceParams)(void* encoder, void* sequenceParams);
    // Slot 23:
    NVENCSTATUS (__stdcall *nvEncRegisterAsyncEvent)(void* encoder, void* eventParams);
    // Slot 24:
    NVENCSTATUS (__stdcall *nvEncUnregisterAsyncEvent)(void* encoder, void* eventParams);
    // Slot 25:
    NVENCSTATUS (__stdcall *nvEncMapInputResource)(void* encoder, NV_ENC_MAP_INPUT_RESOURCE* params);
    // Slot 26:
    NVENCSTATUS (__stdcall *nvEncUnmapInputResource)(void* encoder, NV_ENC_INPUT_PTR mappedResource);
    // Slot 27:
    NVENCSTATUS (__stdcall *nvEncDestroyEncoder)(void* encoder);
    // Slot 28:
    NVENCSTATUS (__stdcall *nvEncInvalidateRefFrames)(void* encoder, uint64_t invalidRefFrameTimeStamp);
    // Slot 29: current session open API
    NVENCSTATUS (__stdcall *nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params, void** encoder);
    // Slot 30:
    NVENCSTATUS (__stdcall *nvEncRegisterResource)(void* encoder, NV_ENC_REGISTER_RESOURCE* params);
    // Slot 31:
    NVENCSTATUS (__stdcall *nvEncUnregisterResource)(void* encoder, NV_ENC_REGISTERED_PTR resource);
    // Slot 32:
    NVENCSTATUS (__stdcall *nvEncReconfigureEncoder)(void* encoder, NV_ENC_RECONFIGURE_PARAMS* params);
    // Slot 33: reserved
    void*       reserved1;
    // Slot 34:
    NVENCSTATUS (__stdcall *nvEncCreateMVBuffer)(void* encoder, void* params);
    // Slot 35:
    NVENCSTATUS (__stdcall *nvEncDestroyMVBuffer)(void* encoder, void* buffer);
    // Slot 36:
    void*       nvEncRunMotionEstimationOnly;
    // Slot 37:
    const char* (__stdcall *nvEncGetLastErrorString)(void* encoder);
    // Slot 38:
    NVENCSTATUS (__stdcall *nvEncSetIOCudaStreams)(void* encoder, void* inputStream, void* outputStream);
    // Slot 39: preset config with tuning info
    NVENCSTATUS (__stdcall *nvEncGetEncodePresetConfigEx)(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_TUNING_INFO tuningInfo, NV_ENC_PRESET_CONFIG* presetConfig);
    // Slot 40:
    NVENCSTATUS (__stdcall *nvEncGetSequenceParamEx)(void* encoder, void* params);
    // Slot 41 (SDK 12.2):
    void*       nvEncRestoreEncoderState;
    // Slot 42 (SDK 12.2):
    void*       nvEncLookaheadPicture;
    // Slots 43+:
    void*       reserved2[275];
} NV_ENCODE_API_FUNCTION_LIST;
#define NV_ENCODE_API_FUNCTION_LIST_VER NVENCAPI_STRUCT_VERSION(NV_ENCODE_API_FUNCTION_LIST, 2)

// ---------------------------------------------------------------------------
// Entry point typedefs
// ---------------------------------------------------------------------------
typedef NVENCSTATUS (__stdcall *NvEncodeAPICreateInstance_t)(NV_ENCODE_API_FUNCTION_LIST* functionList);
typedef NVENCSTATUS (__stdcall *NvEncodeAPIGetMaxSupportedVersion_t)(uint32_t* version);

#ifdef __cplusplus
}
#endif
