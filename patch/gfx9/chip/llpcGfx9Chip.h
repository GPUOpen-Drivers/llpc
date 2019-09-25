/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcGfx9Chip.h
 * @brief LLPC header file: contains various definitions for Gfx9 chips.
 ***********************************************************************************************************************
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include "llpcAbiMetadata.h"

namespace Llpc
{

    namespace Gfx9
    {

#undef CS_ENABLE

#include "gfx9_plus_merged_offset.h"
#include "gfx9_plus_merged_registers.h"
#include "gfx9_plus_merged_typedef.h"

// =====================================================================================================================
// Helper macros to operate registers

// Defines fields: register ID (byte-based) and its value
#define DEF_REG(_reg)               uint32_t _reg##_ID; reg##_reg _reg##_VAL;

// Defines GFX-dependent fields: register ID (byte-based) and its value
#define DEF_REG_ID(_reg)            uint32_t            _reg##_ID;
#define DEF_REG_VAL(_reg)           struct { uint32_t u32All; } _reg##_VAL;

// Initializes register ID and its value
#define INIT_REG(_reg)            { _reg##_ID = mm##_reg; _reg##_VAL.u32All = 0; }

// Initializes register to invalid ID and value
#define INIT_REG_TO_INVALID(_reg) { _reg##_ID = InvalidMetadataKey; _reg##_VAL.u32All = InvalidMetadataValue; }

// Initializes GFX-dependent register ID and its value
#if LLPC_BUILD_GFX10
// GFX9 plus
#define INIT_REG_GFX9_PLUS(_gfx, _reg) \
{ \
    if (_gfx == 9) \
    { \
        _reg##_ID         = Gfx09::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else if (_gfx == 10) \
    { \
        _reg##_ID         = Gfx10::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        INIT_REG_TO_INVALID(_reg); \
    } \
}

// GFX10 plus
#define INIT_REG_GFX10_PLUS(_gfx, _reg) \
{ \
    if (_gfx == 10) \
    { \
        _reg##_ID         = Gfx10::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        INIT_REG_TO_INVALID(_reg); \
    } \
}

// GFX10.1 plus
#define INIT_REG_GFX10_1_PLUS(_gfxMajor, _gfxMinor, _reg) \
{ \
    if ((_gfxMajor == 10) && (_gfxMinor > 0)) \
    { \
        _reg##_ID         = Gfx101Plus::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        INIT_REG_TO_INVALID(_reg); \
    } \
}
#else
#define INIT_REG_GFX9_PLUS(_gfx, _reg) \
{ \
    if (_gfx == 9) \
    { \
        _reg##_ID         = Gfx09::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        INIT_REG_TO_INVALID(_reg); \
    } \
}
#endif

// GFX9 only
#define INIT_REG_GFX9(_gfx, _reg) \
{ \
    if (_gfx == 9) \
    { \
        _reg##_ID         = Gfx09::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        INIT_REG_TO_INVALID(_reg); \
    } \
}

#if LLPC_BUILD_GFX10
// GFX10 only
#define INIT_REG_GFX10(_gfx, _reg) \
{ \
    if (_gfx == 10) \
    { \
        _reg##_ID         = Gfx10::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        INIT_REG_TO_INVALID(_reg); \
    } \
}
#endif

// Case label for switch, set register value
#define CASE_SET_REG(_stage, _reg, _val)   case (mm##_reg * 4): { (_stage)->_reg##_VAL.u32All = (_val); break; }

// Adds an entry for the map from register ID to its name string
#define ADD_REG_MAP(_reg)               RegNameMap[mm##_reg * 4] = #_reg;

#define ADD_REG_MAP_GFX9(_reg)          RegNameMapGfx9[Gfx09::mm##_reg * 4] = #_reg;
#if LLPC_BUILD_GFX10
#define ADD_REG_MAP_GFX10(_reg)         RegNameMapGfx10[Gfx10::mm##_reg * 4] = #_reg;
#define ADD_REG_MAP_GFX10_1_PLUS(_reg)  RegNameMapGfx10[Gfx101Plus::mm##_reg * 4] = #_reg;
#endif

// Gets register value
#define GET_REG(_stage, _reg)                      ((_stage)->_reg##_VAL.u32All)

// Sets register value
#define SET_REG(_stage, _reg, _val)                (_stage)->_reg##_VAL.u32All = (_val);

// Invalidate register, set it to invalid ID and value
#define INVALIDATE_REG(_stage, _reg) \
    { (_stage)->_reg##_ID = InvalidMetadataKey; (_stage)->_reg##_VAL.u32All = InvalidMetadataValue; }

// Adds and sets dynamic register value
#define SET_DYN_REG(_pipeline, _reg, _val) \
    LLPC_ASSERT((_pipeline)->m_dynRegCount < _pipeline->MaxDynamicRegs); \
    (_pipeline)->m_dynRegs[(_pipeline)->m_dynRegCount].key = (_reg);     \
    (_pipeline)->m_dynRegs[(_pipeline)->m_dynRegCount++].value = (_val);

// Gets register field value
#define GET_REG_FIELD(_stage, _reg, _field)        ((_stage)->_reg##_VAL.bits._field)

// Sets register field value
#define SET_REG_FIELD(_stage, _reg, _field, _val)  (_stage)->_reg##_VAL.bits._field = (_val);

// Gets register core field value
#define GET_REG_CORE_FIELD(_stage, _reg, _field)        ((_stage)->_reg##_VAL.core._field)

// Sets register core field value
#define SET_REG_CORE_FIELD(_stage, _reg, _field, _val)  (_stage)->_reg##_VAL.core._field = (_val);

// Gets GFX-dependent register field value
#define GET_REG_GFX9_FIELD(_stage, _reg, _field)    ((_stage)->_reg##_VAL.gfx09._field)
#if LLPC_BUILD_GFX10
#define GET_REG_GFX10_FIELD(_stage, _reg, _field)   ((_stage)->_reg##_VAL.gfx10._field)
#endif

// Sets GFX-dependent register field value
#define SET_REG_GFX9_FIELD(_stage, _reg, _field, _val)      (_stage)->_reg##_VAL.gfx09._field = (_val);
#if LLPC_BUILD_GFX10
#define SET_REG_GFX10_FIELD(_stage, _reg, _field, _val)     (_stage)->_reg##_VAL.gfx10._field = (_val);
#define SET_REG_GFX10_1_FIELD(_stage, _reg, _field, _val)   (_stage)->_reg##_VAL.gfx101._field = (_val);
#endif

// Preferred number of GS primitives per ES thread.
constexpr uint32_t GsPrimsPerEsThread = 256;

// Preferred number of GS threads per VS thread.
constexpr uint32_t GsThreadsPerVsThread = 2;

// Preferred number of GS threads per subgroup.
constexpr uint32_t MaxGsThreadsPerSubgroup = 256;

// Max number of threads per subgroup in NGG mode.
constexpr uint32_t NggMaxThreadsPerSubgroup = 256;

// Max number of waves per subgroup in NGG mode.
constexpr uint32_t NggMaxWavesPerSubgroup = NggMaxThreadsPerSubgroup / 32;

// Max size of primitives per subgroup for adjacency primitives or when GS instancing is used. This restriction is
// applicable only when onchip GS is used.
constexpr uint32_t OnChipGsMaxPrimPerSubgroup    = 255;
constexpr uint32_t OnChipGsMaxPrimPerSubgroupAdj = 127;
constexpr uint32_t OnChipGsMaxEsVertsPerSubgroup = 255;

// Default value for the maximum LDS size per GS subgroup, in DWORD's.
constexpr uint32_t DefaultLdsSizePerSubgroup = 8192;

constexpr uint32_t EsVertsOffchipGsOrTess = 250;
constexpr uint32_t GsPrimsOffchipGsOrTess = 126;

// The register headers don't specify an enum for the values of VGT_GS_MODE.ONCHIP.
enum VGT_GS_MODE_ONCHIP_TYPE : uint32_t
{
    VGT_GS_MODE_ONCHIP_OFF         = 1,
    VGT_GS_MODE_ONCHIP_ON          = 3,
};

// The register headers don't specify an enum for the values of PA_STEREO_CNTL.STEREO_MODE.
enum StereoMode : uint32_t
{
    SHADER_STEREO_X                = 0,
    STATE_STEREO_X                 = 1,
    SHADER_STEREO_XYZW             = 2,
};

#if LLPC_BUILD_GFX10
namespace Gfx10
{
    constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_GS               = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_GS;
    constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_HS               = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_HS;
    constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_PS               = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_PS;
    constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_VS               = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_VS;
};
#endif

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware vertex shader.
struct VsRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_VS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_VS);
    DEF_REG(SPI_SHADER_POS_FORMAT);
    DEF_REG(SPI_VS_OUT_CONFIG);
    DEF_REG(PA_CL_VS_OUT_CNTL);
    DEF_REG(PA_CL_CLIP_CNTL);
    DEF_REG(PA_CL_VTE_CNTL);
    DEF_REG(PA_SU_VTX_CNTL);
    DEF_REG(VGT_PRIMITIVEID_EN);
    DEF_REG(VGT_REUSE_OFF);
    DEF_REG(VGT_STRMOUT_CONFIG);
    DEF_REG(VGT_STRMOUT_BUFFER_CONFIG);
    DEF_REG(VGT_STRMOUT_VTX_STRIDE_0);
    DEF_REG(VGT_STRMOUT_VTX_STRIDE_1);
    DEF_REG(VGT_STRMOUT_VTX_STRIDE_2);
    DEF_REG(VGT_STRMOUT_VTX_STRIDE_3);
#if LLPC_BUILD_GFX10
    DEF_REG(SPI_SHADER_PGM_CHKSUM_VS);
    DEF_REG(SPI_SHADER_USER_ACCUM_VS_0);
    DEF_REG(SPI_SHADER_USER_ACCUM_VS_1);
    DEF_REG(SPI_SHADER_USER_ACCUM_VS_2);
    DEF_REG(SPI_SHADER_USER_ACCUM_VS_3);
#endif

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware local-hull merged shader.
struct LsHsRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_HS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_HS);
    DEF_REG(VGT_LS_HS_CONFIG);
    DEF_REG(VGT_HOS_MIN_TESS_LEVEL);
    DEF_REG(VGT_HOS_MAX_TESS_LEVEL);
    DEF_REG(VGT_TF_PARAM);
#if LLPC_BUILD_GFX10
    DEF_REG(SPI_SHADER_PGM_CHKSUM_HS);
    DEF_REG(SPI_SHADER_USER_ACCUM_LSHS_0);
    DEF_REG(SPI_SHADER_USER_ACCUM_LSHS_1);
    DEF_REG(SPI_SHADER_USER_ACCUM_LSHS_2);
    DEF_REG(SPI_SHADER_USER_ACCUM_LSHS_3);
#endif

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware export-geometry merged shader.
struct EsGsRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_GS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_GS);
    DEF_REG(SPI_SHADER_PGM_RSRC4_GS);
    DEF_REG(VGT_GS_MAX_VERT_OUT);
    DEF_REG(VGT_GS_ONCHIP_CNTL);
    DEF_REG(VGT_GS_VERT_ITEMSIZE);
    DEF_REG(VGT_GS_INSTANCE_CNT);
    DEF_REG(VGT_GS_PER_VS);
    DEF_REG(VGT_GS_OUT_PRIM_TYPE);
    DEF_REG(VGT_GSVS_RING_ITEMSIZE);
    DEF_REG(VGT_GS_VERT_ITEMSIZE_1);
    DEF_REG(VGT_GS_VERT_ITEMSIZE_2);
    DEF_REG(VGT_GS_VERT_ITEMSIZE_3);
    DEF_REG(VGT_GSVS_RING_OFFSET_1);
    DEF_REG(VGT_GSVS_RING_OFFSET_2);
    DEF_REG(VGT_GSVS_RING_OFFSET_3);
    DEF_REG(VGT_GS_MODE);
    DEF_REG(VGT_ESGS_RING_ITEMSIZE);
    DEF_REG(VGT_GS_MAX_PRIMS_PER_SUBGROUP);
#if LLPC_BUILD_GFX10
    DEF_REG(GE_MAX_OUTPUT_PER_SUBGROUP);
    DEF_REG(SPI_SHADER_PGM_CHKSUM_GS);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_0);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_1);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_2);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_3);

    DEF_REG(GE_NGG_SUBGRP_CNTL);
    DEF_REG(SPI_SHADER_IDX_FORMAT);
#endif

    void Init(GfxIpVersion gfxIp);
};

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Represents configuration of static registers relevant to hardware primitive shader (NGG).
struct PrimShaderRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_GS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_GS);
    DEF_REG(SPI_SHADER_PGM_RSRC4_GS);
    DEF_REG(VGT_GS_MAX_VERT_OUT);
    DEF_REG(VGT_GS_ONCHIP_CNTL);
    DEF_REG(VGT_GS_VERT_ITEMSIZE);
    DEF_REG(VGT_GS_INSTANCE_CNT);
    DEF_REG(VGT_GS_PER_VS);
    DEF_REG(VGT_GS_OUT_PRIM_TYPE);
    DEF_REG(VGT_GSVS_RING_ITEMSIZE);
    DEF_REG(VGT_GS_VERT_ITEMSIZE_1);
    DEF_REG(VGT_GS_VERT_ITEMSIZE_2);
    DEF_REG(VGT_GS_VERT_ITEMSIZE_3);
    DEF_REG(VGT_GSVS_RING_OFFSET_1);
    DEF_REG(VGT_GSVS_RING_OFFSET_2);
    DEF_REG(VGT_GSVS_RING_OFFSET_3);
    DEF_REG(VGT_GS_MODE);
    DEF_REG(VGT_ESGS_RING_ITEMSIZE);
    DEF_REG(GE_MAX_OUTPUT_PER_SUBGROUP);
    DEF_REG(SPI_SHADER_PGM_CHKSUM_GS);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_0);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_1);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_2);
    DEF_REG(SPI_SHADER_USER_ACCUM_ESGS_3);

    DEF_REG(SPI_SHADER_POS_FORMAT);
    DEF_REG(SPI_VS_OUT_CONFIG);
    DEF_REG(PA_CL_VS_OUT_CNTL);
    DEF_REG(PA_CL_CLIP_CNTL);
    DEF_REG(PA_CL_VTE_CNTL);
    DEF_REG(PA_SU_VTX_CNTL);
    DEF_REG(VGT_PRIMITIVEID_EN);
    DEF_REG(VGT_REUSE_OFF);

    DEF_REG(GE_NGG_SUBGRP_CNTL);
    DEF_REG(SPI_SHADER_IDX_FORMAT);

    DEF_REG(SPI_SHADER_PGM_LO_GS);

    void Init(GfxIpVersion gfxIp);
};
#endif

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware pixel shader.
struct PsRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_PS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_PS);
    DEF_REG(SPI_SHADER_Z_FORMAT);
    DEF_REG(SPI_SHADER_COL_FORMAT);
    DEF_REG(SPI_BARYC_CNTL);
    DEF_REG(SPI_PS_IN_CONTROL);
    DEF_REG(SPI_PS_INPUT_ENA);
    DEF_REG(SPI_PS_INPUT_ADDR);
    DEF_REG(SPI_INTERP_CONTROL_0);
    DEF_REG(PA_SC_MODE_CNTL_1);
    DEF_REG(DB_SHADER_CONTROL);
    DEF_REG(CB_SHADER_MASK);
    DEF_REG(PA_SC_AA_CONFIG);
    DEF_REG(PA_SC_SHADER_CONTROL);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 460
    DEF_REG(PA_SC_CONSERVATIVE_RASTERIZATION_CNTL);
#endif
#if LLPC_BUILD_GFX10
    DEF_REG(PA_STEREO_CNTL);
    DEF_REG(GE_STEREO_CNTL);
    DEF_REG(GE_USER_VGPR_EN);
    DEF_REG(SPI_SHADER_PGM_CHKSUM_PS);
    DEF_REG(SPI_SHADER_USER_ACCUM_PS_0);
    DEF_REG(SPI_SHADER_USER_ACCUM_PS_1);
    DEF_REG(SPI_SHADER_USER_ACCUM_PS_2);
    DEF_REG(SPI_SHADER_USER_ACCUM_PS_3);
#endif

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents the common configuration of registers relevant to all pipeline.
struct PipelineRegConfig
{
    void Init();
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-FS).
struct PipelineVsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_VS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~31
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    VsRegConfig m_vsRegs;   // VS -> hardware VS
    PsRegConfig m_psRegs;   // FS -> hardware PS
    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(VGT_GS_ONCHIP_CNTL);
    DEF_REG(IA_MULTI_VGT_PARAM);
#if LLPC_BUILD_GFX10
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);
#endif

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineVsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-TS-FS).
struct PipelineVsTsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_LS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_VS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~31
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    LsHsRegConfig m_lsHsRegs; // VS-TCS -> hardware LS-HS
    VsRegConfig   m_vsRegs;   // TES    -> hardware VS
    PsRegConfig   m_psRegs;   // FS     -> hardware PS

    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM);
#if LLPC_BUILD_GFX10
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);
    DEF_REG(VGT_GS_ONCHIP_CNTL);
#endif

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineVsTsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-GS-FS).
struct PipelineVsGsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_ES_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_VS_0~31
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    EsGsRegConfig m_esGsRegs;   // VS-GS -> hardware ES-GS
    PsRegConfig   m_psRegs;     // FS -> hardware PS
    VsRegConfig   m_vsRegs;     // Copy shader -> hardware VS

    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM);
#if LLPC_BUILD_GFX10
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);
#endif

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineVsGsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-TS-GS-FS).
struct PipelineVsTsGsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_LS_0~32
                                           32 + // mmSPI_SHADER_USER_DATA_ES_0~32
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~32
                                           32 + // mmSPI_SHADER_USER_DATA_VS_0~32
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    LsHsRegConfig m_lsHsRegs;   // VS-TCS -> hardware LS-HS
    EsGsRegConfig m_esGsRegs;   // TES-GS -> hardware ES-GS
    PsRegConfig   m_psRegs;     // FS -> hardware PS
    VsRegConfig   m_vsRegs;     // Copy shader -> hardware VS

    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM);
#if LLPC_BUILD_GFX10
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);
#endif

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineVsTsGsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-FS).
struct PipelineNggVsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_GS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~31
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    PrimShaderRegConfig m_primShaderRegs; // VS -> hardware primitive shader (NGG, ES-GS)
    PsRegConfig         m_psRegs;         // FS -> hardware PS
    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineNggVsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-TS-FS).
struct PipelineNggVsTsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_HS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_GS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~31
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    LsHsRegConfig       m_lsHsRegs;       // VS-TCS -> hardware LS-HS
    PrimShaderRegConfig m_primShaderRegs; // TES    -> hardware primitive shader (NGG, ES-GS)
    PsRegConfig         m_psRegs;         // FS     -> hardware PS

    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineNggVsTsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-GS-FS).
struct PipelineNggVsGsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_GS_0~31
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~31
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    PrimShaderRegConfig m_primShaderRegs; // VS-GS -> hardware primitive shader (NGG, ES-GS)
    PsRegConfig         m_psRegs;         // FS    -> hardware PS

    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineNggVsGsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-TS-GS-FS).
struct PipelineNggVsTsGsFsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 32 + // mmSPI_SHADER_USER_DATA_HS_0~32
                                           32 + // mmSPI_SHADER_USER_DATA_GS_0~32
                                           32 + // mmSPI_SHADER_USER_DATA_PS_0~32
                                           32;  // mmSPI_PS_INPUT_CNTL_0~31

    LsHsRegConfig       m_lsHsRegs;       // VS-TCS -> hardware LS-HS
    PrimShaderRegConfig m_primShaderRegs; // TES-GS -> hardware primitive shader (NGG, ES-GS)
    PsRegConfig         m_psRegs;         // FS     -> hardware PS

    DEF_REG(VGT_SHADER_STAGES_EN);
    DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineNggVsTsGsFsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};
#endif

// =====================================================================================================================
// Represents configuration of registers relevant to compute shader.
struct CsRegConfig
{
    DEF_REG(COMPUTE_PGM_RSRC1);
    DEF_REG(COMPUTE_PGM_RSRC2);
    DEF_REG(COMPUTE_NUM_THREAD_X);
    DEF_REG(COMPUTE_NUM_THREAD_Y);
    DEF_REG(COMPUTE_NUM_THREAD_Z);
    DEF_REG(COMPUTE_DISPATCH_INITIATOR);
#if LLPC_BUILD_GFX10
    DEF_REG(COMPUTE_PGM_RSRC3);
    DEF_REG(COMPUTE_SHADER_CHKSUM);
    DEF_REG(COMPUTE_USER_ACCUM_0);
    DEF_REG(COMPUTE_USER_ACCUM_1);
    DEF_REG(COMPUTE_USER_ACCUM_2);
    DEF_REG(COMPUTE_USER_ACCUM_3);
#endif

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to compute pipeline.
struct PipelineCsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 16;  // mmCOMPUTE_USER_DATA_0~15

    CsRegConfig   m_csRegs;
    Util::Abi::PalMetadataNoteEntry m_dynRegs[MaxDynamicRegs];  // Dynamic registers configuration
    uint32_t m_dynRegCount;                                     // Count of dynamic registers

    void Init(GfxIpVersion gfxIp);

    // Get total register's count of this pipeline
    uint32_t GetRegCount() const
    {
        return  offsetof(PipelineCsRegConfig, m_dynRegs) / sizeof(Util::Abi::PalMetadataNoteEntry) +
                m_dynRegCount;
    }
};

// Map from register ID to its name string
static std::unordered_map<uint32_t, const char*>    RegNameMap;
static std::unordered_map<uint32_t, const char*>    RegNameMapGfx9;  // GFX9 specific
#if LLPC_BUILD_GFX10
static std::unordered_map<uint32_t, const char*>    RegNameMapGfx10; // GFX10 specific
#endif

// Adds entries to register name map.
void InitRegisterNameMap(GfxIpVersion gfxIp);

// Gets the name string from byte-based ID of the register
const char* GetRegisterNameString(GfxIpVersion gfxIp, uint32_t regId);

} // Gfx9

} // Llpc
