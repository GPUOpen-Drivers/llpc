/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Gfx9Chip.h
 * @brief LLPC header file: contains various definitions for Gfx9 chips.
 ***********************************************************************************************************************
 */
#pragma once

#include "ConfigBuilderBase.h"
#include "lgc/state/TargetInfo.h"
#include <cstdint>
#include <unordered_map>

namespace lgc {

namespace Gfx9 {

#undef CS_ENABLE

#include "chip/gfx9/gfx9_plus_merged_offset.h"
#include "chip/gfx9/gfx9_plus_merged_registers.h"
#include "chip/gfx9/gfx9_plus_merged_typedef.h"

using namespace Pal::Gfx9::Chip;

// =====================================================================================================================
// Helper macros to operate registers

// Defines fields: register ID (byte-based) and its value
#define DEF_REG(_reg)                                                                                                  \
  unsigned _reg##_ID;                                                                                                  \
  reg##_reg _reg##_VAL;

// Defines GFX-dependent fields: register ID (byte-based) and its value
#define DEF_REG_ID(_reg) unsigned _reg##_ID;
#define DEF_REG_VAL(_reg)                                                                                              \
  struct {                                                                                                             \
    unsigned u32All;                                                                                                   \
  } _reg##_VAL;

// Initializes register ID and its value
#define INIT_REG(_reg)                                                                                                 \
  {                                                                                                                    \
    _reg##_ID = mm##_reg;                                                                                              \
    _reg##_VAL.u32All = 0;                                                                                             \
  }

// Initializes register to invalid ID and value
#define INIT_REG_TO_INVALID(_reg)                                                                                      \
  {                                                                                                                    \
    _reg##_ID = InvalidMetadataKey;                                                                                    \
    _reg##_VAL.u32All = InvalidMetadataValue;                                                                          \
  }

// Initializes GFX-dependent register ID and its value
// GFX10 plus
#define INIT_REG_GFX10_PLUS(_gfx, _reg)                                                                                \
  {                                                                                                                    \
    if (_gfx >= 10) {                                                                                                  \
      _reg##_ID = Gfx10Plus::mm##_reg;                                                                                 \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// Apu09.1x plus
#define INIT_REG_APU09_1X_PLUS(_gfx, _reg)                                                                             \
  {                                                                                                                    \
    if (_gfx >= 10) {                                                                                                  \
      _reg##_ID = Apu09_1xPlus::mm##_reg;                                                                              \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// GFX9 only
#define INIT_REG_GFX9(_gfx, _reg)                                                                                      \
  {                                                                                                                    \
    if (_gfx == 9) {                                                                                                   \
      _reg##_ID = Gfx09::mm##_reg;                                                                                     \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// GFX10 only
#define INIT_REG_GFX10(_gfx, _reg)                                                                                     \
  {                                                                                                                    \
    if (_gfx == 10) {                                                                                                  \
      _reg##_ID = Pal::Gfx9::Chip::Gfx10::mm##_reg;                                                                    \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// GFX11 only
#define INIT_REG_GFX11(_gfx, _reg)                                                                                     \
  {                                                                                                                    \
    if (_gfx == 11) {                                                                                                  \
      _reg##_ID = Pal::Gfx9::Chip::Gfx11::mm##_reg;                                                                    \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// GFX9-GFX10 only
#define INIT_REG_GFX9_10(_gfx, _reg)                                                                                   \
  {                                                                                                                    \
    if (_gfx == 9 || _gfx == 10) {                                                                                     \
      _reg##_ID = Gfx09_10::mm##_reg;                                                                                  \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// HasHwVs only
#define INIT_REG_HAS_HW_VS(_gfx, _reg)                                                                                 \
  {                                                                                                                    \
    if (_gfx == 9 || _gfx == 10) {                                                                                     \
      _reg##_ID = HasHwVs::mm##_reg;                                                                                   \
      _reg##_VAL.u32All = 0;                                                                                           \
    } else {                                                                                                           \
      INIT_REG_TO_INVALID(_reg);                                                                                       \
    }                                                                                                                  \
  }

// Case label for switch, set register value
#define CASE_SET_REG(_stage, _reg, _val)                                                                               \
  case (mm##_reg * 4): {                                                                                               \
    (_stage)->_reg##_VAL.u32All = (_val);                                                                              \
    break;                                                                                                             \
  }

// Gets register value
#define GET_REG(_stage, _reg) ((_stage)->_reg##_VAL.u32All)

// Sets register value
#define SET_REG(_stage, _reg, _val) (_stage)->_reg##_VAL.u32All = (_val);

// Invalidate register, set it to invalid ID and value
#define INVALIDATE_REG(_stage, _reg)                                                                                   \
  {                                                                                                                    \
    (_stage)->_reg##_ID = InvalidMetadataKey;                                                                          \
    (_stage)->_reg##_VAL.u32All = InvalidMetadataValue;                                                                \
  }

// Gets register field value
#define GET_REG_FIELD(_stage, _reg, _field) ((_stage)->_reg##_VAL.bits._field)

// Sets register field value
#define SET_REG_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.bits._field = (_val);

// Sets register most field value
#define SET_REG_MOST_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.most._field = (_val);

// Gets register core field value
#define GET_REG_CORE_FIELD(_stage, _reg, _field) ((_stage)->_reg##_VAL.core._field)

// Sets register core field value
#define SET_REG_CORE_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.core._field = (_val);

// Gets GFX-dependent register field value
#define GET_REG_GFX9_FIELD(_stage, _reg, _field) ((_stage)->_reg##_VAL.gfx09._field)
#define GET_REG_GFX10_FIELD(_stage, _reg, _field) ((_stage)->_reg##_VAL.gfx10._field)

// Sets GFX-dependent register field value
#define SET_REG_GFX9_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx09._field = (_val);
#define SET_REG_GFX09_1X_PLUS_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx09_1xPlus._field = (_val);
#define SET_REG_GFX10_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx10._field = (_val);
#define SET_REG_GFX9_10_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx09_10._field = (_val);
#define SET_REG_GFX10_PLUS_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx10Plus._field = (_val);
#define SET_REG_GFX10_1_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx101._field = (_val);
#define SET_REG_GFX10_3_PLUS_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx103Plus._field = (_val);
#define SET_REG_GFX10_3_PLUS_EXCLUSIVE_FIELD(_stage, _reg, _field, _val)                                               \
  (_stage)->_reg##_VAL.gfx103PlusExclusive._field = (_val);
#define SET_REG_GFX10_4_PLUS_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx104Plus._field = (_val);
#define SET_REG_GFX11_FIELD(_stage, _reg, _field, _val) (_stage)->_reg##_VAL.gfx11._field = (_val);

// Preferred number of GS primitives per ES thread.
constexpr unsigned GsPrimsPerEsThread = 256;

// Preferred number of GS threads per VS thread.
constexpr unsigned GsThreadsPerVsThread = 2;

// Preferred number of HS threads per subgroup.
constexpr unsigned MaxHsThreadsPerSubgroup = 256;

// Preferred number of GS threads per subgroup.
constexpr unsigned MaxGsThreadsPerSubgroup = 256;

// Max number of threads per subgroup in NGG mode.
constexpr unsigned NggMaxThreadsPerSubgroup = 256;

// Max number of waves per subgroup in NGG mode.
constexpr unsigned NggMaxWavesPerSubgroup = NggMaxThreadsPerSubgroup / 32;

// Max size of primitives per subgroup for adjacency primitives or when GS instancing is used. This restriction is
// applicable only when onchip GS is used.
constexpr unsigned OnChipGsMaxPrimPerSubgroup = 255;
constexpr unsigned OnChipGsMaxPrimPerSubgroupAdj = 127;
constexpr unsigned OnChipGsMaxEsVertsPerSubgroup = 255;

// Default value for the maximum LDS size per GS subgroup, in dword's.
constexpr unsigned DefaultLdsSizePerSubgroup = 8192;

constexpr unsigned EsVertsOffchipGsOrTess = 250;
constexpr unsigned GsPrimsOffchipGsOrTess = 126;

// The register headers don't specify an enum for the values of VGT_GS_MODE.ONCHIP.
enum VGT_GS_MODE_ONCHIP_TYPE : unsigned {
  VGT_GS_MODE_ONCHIP_OFF = 1,
  VGT_GS_MODE_ONCHIP_ON = 3,
};

// The register headers don't specify an enum for the values of PA_STEREO_CNTL.STEREO_MODE.
enum StereoMode : unsigned {
  SHADER_STEREO_X = 0,
  STATE_STEREO_X = 1,
  SHADER_STEREO_XYZW = 2,
};

namespace Gfx10 {
constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_GS = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_GS;
constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_HS = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_HS;
constexpr unsigned int mmSPI_SHADER_PGM_CHKSUM_PS = Apu09_1xPlus::mmSPI_SHADER_PGM_CHKSUM_PS;
}; // namespace Gfx10

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware vertex shader.
struct VsRegConfig {
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
  DEF_REG(SPI_SHADER_PGM_CHKSUM_VS);

  VsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware local-hull merged shader.
struct LsHsRegConfig {
  DEF_REG(SPI_SHADER_PGM_RSRC1_HS);
  DEF_REG(SPI_SHADER_PGM_RSRC2_HS);
  DEF_REG(SPI_SHADER_PGM_RSRC4_HS);
  DEF_REG(VGT_LS_HS_CONFIG);
  DEF_REG(VGT_HOS_MIN_TESS_LEVEL);
  DEF_REG(VGT_HOS_MAX_TESS_LEVEL);
  DEF_REG(VGT_TF_PARAM);
  DEF_REG(SPI_SHADER_PGM_CHKSUM_HS);

  LsHsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware export-geometry merged shader.
struct EsGsRegConfig {
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
  DEF_REG(GE_MAX_OUTPUT_PER_SUBGROUP);
  DEF_REG(SPI_SHADER_PGM_CHKSUM_GS);

  DEF_REG(GE_NGG_SUBGRP_CNTL);
  DEF_REG(SPI_SHADER_IDX_FORMAT);

  EsGsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware primitive shader (NGG).
struct PrimShaderRegConfig {
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

  PrimShaderRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of static registers relevant to hardware pixel shader.
struct PsRegConfig {
  DEF_REG(SPI_SHADER_PGM_RSRC1_PS);
  DEF_REG(SPI_SHADER_PGM_RSRC2_PS);
  DEF_REG(SPI_SHADER_PGM_RSRC4_PS);
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
  DEF_REG(PA_STEREO_CNTL);
  DEF_REG(GE_STEREO_CNTL);
  DEF_REG(GE_USER_VGPR_EN);
  DEF_REG(SPI_SHADER_PGM_CHKSUM_PS);

  PsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-FS).
struct PipelineVsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  VsRegConfig vsRegs; // VS -> hardware VS
  PsRegConfig psRegs; // FS -> hardware PS
  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(VGT_GS_ONCHIP_CNTL);
  DEF_REG(IA_MULTI_VGT_PARAM);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineVsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-TS-FS).
struct PipelineVsTsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  LsHsRegConfig lsHsRegs; // VS-TCS -> hardware LS-HS
  VsRegConfig vsRegs;     // TES    -> hardware VS
  PsRegConfig psRegs;     // FS     -> hardware PS

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);
  DEF_REG(VGT_GS_ONCHIP_CNTL);

  PipelineVsTsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-GS-FS).
struct PipelineVsGsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  EsGsRegConfig esGsRegs; // VS-GS -> hardware ES-GS
  VsRegConfig vsRegs;     // Copy shader -> hardware VS
  PsRegConfig psRegs;     // FS -> hardware PS

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineVsGsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (VS-TS-GS-FS).
struct PipelineVsTsGsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  LsHsRegConfig lsHsRegs; // VS-TCS -> hardware LS-HS
  EsGsRegConfig esGsRegs; // TES-GS -> hardware ES-GS
  VsRegConfig vsRegs;     // Copy shader -> hardware VS
  PsRegConfig psRegs;     // FS -> hardware PS

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineVsTsGsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-FS).
struct PipelineNggVsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  PrimShaderRegConfig primShaderRegs; // VS -> hardware primitive shader (NGG, ES-GS)
  PsRegConfig psRegs;                 // FS -> hardware PS
  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineNggVsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-TS-FS).
struct PipelineNggVsTsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  LsHsRegConfig lsHsRegs;             // VS-TCS -> hardware LS-HS
  PrimShaderRegConfig primShaderRegs; // TES    -> hardware primitive shader (NGG, ES-GS)
  PsRegConfig psRegs;                 // FS     -> hardware PS

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineNggVsTsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-GS-FS).
struct PipelineNggVsGsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  PrimShaderRegConfig primShaderRegs; // VS-GS -> hardware primitive shader (NGG, ES-GS)
  PsRegConfig psRegs;                 // FS    -> hardware PS

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineNggVsGsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (NGG, VS-TS-GS-FS).
struct PipelineNggVsTsGsFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  LsHsRegConfig lsHsRegs;             // VS-TCS -> hardware LS-HS
  PrimShaderRegConfig primShaderRegs; // TES-GS -> hardware primitive shader (NGG, ES-GS)
  PsRegConfig psRegs;                 // FS     -> hardware PS

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

  PipelineNggVsTsGsFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to compute shader.
struct CsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  DEF_REG(COMPUTE_PGM_RSRC1);
  DEF_REG(COMPUTE_PGM_RSRC2);
  DEF_REG(COMPUTE_NUM_THREAD_X);
  DEF_REG(COMPUTE_NUM_THREAD_Y);
  DEF_REG(COMPUTE_NUM_THREAD_Z);
  DEF_REG(COMPUTE_PGM_RSRC3);
  DEF_REG(COMPUTE_SHADER_CHKSUM);

  CsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to mesh shader.
struct MeshRegConfig {
  DEF_REG(SPI_SHADER_PGM_CHKSUM_GS);

  DEF_REG(VGT_SHADER_STAGES_EN);
  DEF_REG(IA_MULTI_VGT_PARAM_PIPED);

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

  DEF_REG(SPI_SHADER_POS_FORMAT);
  DEF_REG(SPI_VS_OUT_CONFIG);
  DEF_REG(PA_CL_VS_OUT_CNTL);
  DEF_REG(PA_CL_CLIP_CNTL);
  DEF_REG(PA_CL_VTE_CNTL);
  DEF_REG(PA_SU_VTX_CNTL);
  DEF_REG(VGT_PRIMITIVEID_EN);
  DEF_REG(VGT_REUSE_OFF);
  DEF_REG(VGT_DRAW_PAYLOAD_CNTL);

  DEF_REG(GE_NGG_SUBGRP_CNTL);
  DEF_REG(SPI_SHADER_IDX_FORMAT);

  DEF_REG(SPI_SHADER_GS_MESHLET_DIM);
  DEF_REG(SPI_SHADER_GS_MESHLET_EXP_ALLOC);

  MeshRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (Mesh-FS).
struct PipelineMeshFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  MeshRegConfig meshRegs; // Mesh -> hardware primitive shader (NGG, ES-GS)
  PsRegConfig psRegs;     // FS   -> hardware PS

  PipelineMeshFsRegConfig(GfxIpVersion gfxIp);
};

// =====================================================================================================================
// Represents configuration of registers relevant to graphics pipeline (Task-Mesh-FS).
struct PipelineTaskMeshFsRegConfig {
  static constexpr bool ContainsPalAbiMetadataOnly = true;

  CsRegConfig taskRegs;   // Task -> hardware CS
  MeshRegConfig meshRegs; // Mesh -> hardware primitive shader (NGG, ES-GS)
  PsRegConfig psRegs;     // FS   -> hardware PS

  PipelineTaskMeshFsRegConfig(GfxIpVersion gfxIp);
};

} // namespace Gfx9

} // namespace lgc
