/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Initializes GFX-dependent register ID and its value
#define INIT_REG_GFX9_PLUS(_gfx, _reg) \
{ \
    if (_gfx == 9) \
    { \
        _reg##_ID         = Gfx09::mm##_reg; \
        _reg##_VAL.u32All = 0; \
    } \
    else \
    { \
        _reg##_ID         = InvalidMetadataKey; \
        _reg##_VAL.u32All = InvalidMetadataValue; \
    } \
}

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
        _reg##_ID         = InvalidMetadataKey; \
        _reg##_VAL.u32All = InvalidMetadataValue; \
    } \
}

// Case label for switch, set register value
#define CASE_SET_REG(_stage, _reg, _val)   case (mm##_reg * 4): { (_stage)->_reg##_VAL.u32All = (_val); break; }

// Adds an entry for the map from register ID to its name string
#define ADD_REG_MAP(_reg)           RegNameMap[mm##_reg * 4] = #_reg;

#define ADD_REG_MAP_GFX9(_reg)      RegNameMapGfx9[Gfx09::mm##_reg * 4] = #_reg;

// Gets register value
#define GET_REG(_stage, _reg)                      ((_stage)->_reg##_VAL.u32All)

// Sets register value
#define SET_REG(_stage, _reg, _val)                (_stage)->_reg##_VAL.u32All = (_val);

// Adds and sets dynamic register value
#define SET_DYN_REG(_pipeline, _reg, _val) \
    LLPC_ASSERT((_pipeline)->m_dynRegCount < _pipeline->MaxDynamicRegs); \
    (_pipeline)->m_dynRegs[(_pipeline)->m_dynRegCount].key = (_reg);     \
    (_pipeline)->m_dynRegs[(_pipeline)->m_dynRegCount++].value = (_val);

// Gets register field value
#define GET_REG_FIELD(_stage, _reg, _field)        ((_stage)->_reg##_VAL.bits._field)

// Sets register field value
#define SET_REG_FIELD(_stage, _reg, _field, _val)  (_stage)->_reg##_VAL.bits._field = (_val);

// Gets GFX-dependent register field value
#define GET_REG_GFX9_FIELD(_stage, _reg, _field)    ((_stage)->_reg##_VAL.gfx09._field)

// Sets GFX-dependent register field value
#define SET_REG_GFX9_FIELD(_stage, _reg, _field, _val)      (_stage)->_reg##_VAL.gfx09._field = (_val);

// Preferred number of GS primitives per ES thread.
constexpr uint32_t GsPrimsPerEsThread = 256;

// Preferred number of GS threads per VS thread.
constexpr uint32_t GsThreadsPerVsThread = 2;

// Max number of threads per subgroup in NGG mode.
constexpr uint32_t NggGsMaxThreadsPerSubgroup = 256;

// Max number of GS output primitives in NGG mode.
constexpr uint32_t NggGsMaxOutPrims = 256;

// Max size of primitives per subgroup for adjacency primitives or when GS instancing is used. This restriction is
// applicable only when onchip GS is used.
constexpr uint32_t OnChipGsMaxPrimPerSubgroup    = 255;
constexpr uint32_t OnChipGsMaxPrimPerSubgroupAdj = 127;
constexpr uint32_t OnChipGsMaxEsVertsPerSubgroup = 255;

// Default value for the maximum LDS size per GS subgroup, in DWORD's.
constexpr uint32_t DefaultLdsSizePerSubgroup = 8192;

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
    DEF_REG(VS_SCRATCH_BYTE_SIZE);
    DEF_REG(VS_NUM_USED_VGPRS);
    DEF_REG(VS_NUM_USED_SGPRS);
    DEF_REG(VS_NUM_AVAIL_VGPRS);
    DEF_REG(VS_NUM_AVAIL_SGPRS);
    DEF_REG(USES_VIEWPORT_ARRAY_INDEX);

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware local-hull merged shader.
struct LsHsRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_HS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_HS);
    DEF_REG(HS_SCRATCH_BYTE_SIZE);
    DEF_REG(HS_NUM_USED_VGPRS);
    DEF_REG(HS_NUM_USED_SGPRS);
    DEF_REG(HS_NUM_AVAIL_VGPRS);
    DEF_REG(HS_NUM_AVAIL_SGPRS);
    DEF_REG(VGT_LS_HS_CONFIG);
    DEF_REG(VGT_HOS_MIN_TESS_LEVEL);
    DEF_REG(VGT_HOS_MAX_TESS_LEVEL);
    DEF_REG(VGT_TF_PARAM);

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware export-geometry merged shader.
struct EsGsRegConfig
{
    DEF_REG(SPI_SHADER_PGM_RSRC1_GS);
    DEF_REG(SPI_SHADER_PGM_RSRC2_GS);
    DEF_REG(SPI_SHADER_PGM_RSRC4_GS);
    DEF_REG(GS_SCRATCH_BYTE_SIZE);
    DEF_REG(GS_NUM_USED_VGPRS);
    DEF_REG(GS_NUM_USED_SGPRS);
    DEF_REG(GS_NUM_AVAIL_VGPRS);
    DEF_REG(GS_NUM_AVAIL_SGPRS);
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

    void Init(GfxIpVersion gfxIp);
};

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
    DEF_REG(PS_USES_UAVS);
    DEF_REG(PS_SCRATCH_BYTE_SIZE);
    DEF_REG(PS_NUM_USED_VGPRS);
    DEF_REG(PS_NUM_USED_SGPRS);
    DEF_REG(PS_NUM_AVAIL_VGPRS);
    DEF_REG(PS_NUM_AVAIL_SGPRS);
    DEF_REG(PA_SC_AA_CONFIG);
    DEF_REG(PA_SC_SHADER_CONTROL);
    DEF_REG(PA_SC_CONSERVATIVE_RASTERIZATION_CNTL);

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents the common configuration of registers relevant to all pipeline.
struct PipelineRegConfig
{
    DEF_REG(USER_DATA_LIMIT);
    DEF_REG(SPILL_THRESHOLD);
    DEF_REG(PIPELINE_HASH_LO);
    DEF_REG(PIPELINE_HASH_HI);
    DEF_REG(API_HW_SHADER_MAPPING_LO);
    DEF_REG(API_HW_SHADER_MAPPING_HI);

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
    DEF_REG(API_VS_HASH_DWORD0);
    DEF_REG(API_VS_HASH_DWORD1);
    DEF_REG(API_PS_HASH_DWORD0);
    DEF_REG(API_PS_HASH_DWORD1);
    DEF_REG(INDIRECT_TABLE_ENTRY);
    DEF_REG(IA_MULTI_VGT_PARAM);

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
    DEF_REG(API_VS_HASH_DWORD0);
    DEF_REG(API_VS_HASH_DWORD1);
    DEF_REG(API_HS_HASH_DWORD0);
    DEF_REG(API_HS_HASH_DWORD1);
    DEF_REG(API_DS_HASH_DWORD0);
    DEF_REG(API_DS_HASH_DWORD1);
    DEF_REG(API_PS_HASH_DWORD0);
    DEF_REG(API_PS_HASH_DWORD1);
    DEF_REG(INDIRECT_TABLE_ENTRY);
    DEF_REG(IA_MULTI_VGT_PARAM);

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
    DEF_REG(API_VS_HASH_DWORD0);
    DEF_REG(API_VS_HASH_DWORD1);
    DEF_REG(API_GS_HASH_DWORD0);
    DEF_REG(API_GS_HASH_DWORD1);
    DEF_REG(API_PS_HASH_DWORD0);
    DEF_REG(API_PS_HASH_DWORD1);
    DEF_REG(INDIRECT_TABLE_ENTRY);
    DEF_REG(IA_MULTI_VGT_PARAM);

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
    DEF_REG(API_VS_HASH_DWORD0);
    DEF_REG(API_VS_HASH_DWORD1);
    DEF_REG(API_HS_HASH_DWORD0);
    DEF_REG(API_HS_HASH_DWORD1);
    DEF_REG(API_DS_HASH_DWORD0);
    DEF_REG(API_DS_HASH_DWORD1);
    DEF_REG(API_GS_HASH_DWORD0);
    DEF_REG(API_GS_HASH_DWORD1);
    DEF_REG(API_PS_HASH_DWORD0);
    DEF_REG(API_PS_HASH_DWORD1);
    DEF_REG(INDIRECT_TABLE_ENTRY);
    DEF_REG(IA_MULTI_VGT_PARAM);

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

// =====================================================================================================================
// Represents configuration of registers relevant to compute shader.
struct CsRegConfig
{
    DEF_REG(COMPUTE_PGM_RSRC1);
    DEF_REG(COMPUTE_PGM_RSRC2);
    DEF_REG(COMPUTE_NUM_THREAD_X);
    DEF_REG(COMPUTE_NUM_THREAD_Y);
    DEF_REG(COMPUTE_NUM_THREAD_Z);
    DEF_REG(CS_SCRATCH_BYTE_SIZE);
    DEF_REG(CS_NUM_USED_VGPRS);
    DEF_REG(CS_NUM_USED_SGPRS);
    DEF_REG(CS_NUM_AVAIL_VGPRS);
    DEF_REG(CS_NUM_AVAIL_SGPRS);

    void Init(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to compute pipeline.
struct PipelineCsRegConfig: public PipelineRegConfig
{
    static const uint32_t MaxDynamicRegs = 16;  // mmCOMPUTE_USER_DATA_0~15

    CsRegConfig   m_csRegs;
    DEF_REG(API_CS_HASH_DWORD0);
    DEF_REG(API_CS_HASH_DWORD1);

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

// Adds entries to register name map.
void InitRegisterNameMap(GfxIpVersion gfxIp);

// Gets the name string from byte-based ID of the register
const char* GetRegisterNameString(GfxIpVersion gfxIp, uint32_t regId);

} // Gfx9

} // Llpc
