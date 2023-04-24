/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Gfx9Chip.cpp
 * @brief LLPC header file: contains implementations for Gfx9 chips.
 ***********************************************************************************************************************
 */
#include "Gfx9Chip.h"
#include "llvm/Support/ErrorHandling.h"

#define DEBUG_TYPE "lgc-gfx9-chip"

namespace lgc {

namespace Gfx9 {
#include "chip/gfx9/gfx9_plus_merged_enum.h"
#include "chip/gfx9/gfx9_plus_merged_offset.h"

// NOTE: This register only exist in GFX9 and GFX10, but its values are still useful for programming other registers in
// PAL, so always leave it in the ELF.
const unsigned mmVGT_GS_ONCHIP_CNTL = Pal::Gfx9::Gfx09_10::mmVGT_GS_ONCHIP_CNTL;

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
VsRegConfig::VsRegConfig(GfxIpVersion gfxIp) {
  INIT_REG(SPI_SHADER_PGM_RSRC1_VS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_VS);
  INIT_REG(SPI_SHADER_POS_FORMAT);
  INIT_REG(SPI_VS_OUT_CONFIG);
  INIT_REG(PA_CL_VS_OUT_CNTL);
  INIT_REG(PA_CL_CLIP_CNTL);
  INIT_REG(PA_CL_VTE_CNTL);
  INIT_REG(PA_SU_VTX_CNTL);
  INIT_REG(VGT_PRIMITIVEID_EN);
  INIT_REG(VGT_REUSE_OFF);

  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_STRMOUT_CONFIG);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_STRMOUT_BUFFER_CONFIG);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_STRMOUT_VTX_STRIDE_0);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_STRMOUT_VTX_STRIDE_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_STRMOUT_VTX_STRIDE_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_STRMOUT_VTX_STRIDE_3);

  INIT_REG_GFX10(gfxIp.major, SPI_SHADER_PGM_CHKSUM_VS);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
LsHsRegConfig::LsHsRegConfig(GfxIpVersion gfxIp) {
  INIT_REG(SPI_SHADER_PGM_RSRC1_HS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_HS);
  INIT_REG(SPI_SHADER_PGM_RSRC4_HS);
  INIT_REG(VGT_LS_HS_CONFIG);
  INIT_REG(VGT_HOS_MIN_TESS_LEVEL);
  INIT_REG(VGT_HOS_MAX_TESS_LEVEL);
  INIT_REG(VGT_TF_PARAM);
  INIT_REG_APU09_1X_PLUS(gfxIp.major, SPI_SHADER_PGM_CHKSUM_HS);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
EsGsRegConfig::EsGsRegConfig(GfxIpVersion gfxIp) {
  INIT_REG(SPI_SHADER_PGM_RSRC1_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC4_GS);
  INIT_REG(VGT_GS_MAX_VERT_OUT);
  INIT_REG(VGT_GS_INSTANCE_CNT);
  INIT_REG(VGT_ESGS_RING_ITEMSIZE);

  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_PER_VS);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_ITEMSIZE);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_3);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_3);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_MODE);

  INIT_REG_GFX9_10(gfxIp.major, VGT_GS_ONCHIP_CNTL);
  INIT_REG_GFX9_10(gfxIp.major, VGT_GS_OUT_PRIM_TYPE);

  INIT_REG_GFX9(gfxIp.major, VGT_GS_MAX_PRIMS_PER_SUBGROUP);
  INIT_REG_GFX10_PLUS(gfxIp.major, GE_MAX_OUTPUT_PER_SUBGROUP);
  INIT_REG_APU09_1X_PLUS(gfxIp.major, SPI_SHADER_PGM_CHKSUM_GS);

  INIT_REG_GFX10_PLUS(gfxIp.major, GE_NGG_SUBGRP_CNTL);
  INIT_REG_GFX10_PLUS(gfxIp.major, SPI_SHADER_IDX_FORMAT);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PrimShaderRegConfig::PrimShaderRegConfig(GfxIpVersion gfxIp) {
  INIT_REG(SPI_SHADER_PGM_RSRC1_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC4_GS);
  INIT_REG(VGT_GS_MAX_VERT_OUT);
  INIT_REG(VGT_GS_INSTANCE_CNT);
  INIT_REG(VGT_ESGS_RING_ITEMSIZE);
  INIT_REG(VGT_GS_ONCHIP_CNTL);

  // Special registers, having different register IDs
  if (gfxIp.major == 9 || gfxIp.major == 10) {
    INIT_REG_GFX9_10(gfxIp.major, VGT_GS_OUT_PRIM_TYPE);
  } else if (gfxIp.major == 11) {
    INIT_REG_GFX11(gfxIp.major, VGT_GS_OUT_PRIM_TYPE);
  } else {
    llvm_unreachable("Not implemented!");
  }

  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_PER_VS);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_ITEMSIZE);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_3);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_3);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_MODE);

  INIT_REG_GFX10_PLUS(gfxIp.major, GE_MAX_OUTPUT_PER_SUBGROUP);
  INIT_REG_APU09_1X_PLUS(gfxIp.major, SPI_SHADER_PGM_CHKSUM_GS);

  INIT_REG(SPI_SHADER_POS_FORMAT);
  INIT_REG(SPI_VS_OUT_CONFIG);
  INIT_REG(PA_CL_VS_OUT_CNTL);
  INIT_REG(PA_CL_CLIP_CNTL);
  INIT_REG(PA_CL_VTE_CNTL);
  INIT_REG(PA_SU_VTX_CNTL);
  INIT_REG(VGT_PRIMITIVEID_EN);
  INIT_REG(VGT_REUSE_OFF);

  INIT_REG_GFX10_PLUS(gfxIp.major, GE_NGG_SUBGRP_CNTL);
  INIT_REG_GFX10_PLUS(gfxIp.major, SPI_SHADER_IDX_FORMAT);

  INIT_REG(SPI_SHADER_PGM_LO_GS);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PsRegConfig::PsRegConfig(GfxIpVersion gfxIp) {
  INIT_REG(SPI_SHADER_PGM_RSRC1_PS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_PS);
  INIT_REG(SPI_SHADER_Z_FORMAT);
  INIT_REG(SPI_SHADER_COL_FORMAT);
  INIT_REG(SPI_BARYC_CNTL);
  INIT_REG(SPI_PS_IN_CONTROL);
  INIT_REG(SPI_PS_INPUT_ENA);
  INIT_REG(SPI_PS_INPUT_ADDR);
  INIT_REG(SPI_INTERP_CONTROL_0);
  INIT_REG(PA_SC_MODE_CNTL_1);
  INIT_REG(DB_SHADER_CONTROL);
  INIT_REG(CB_SHADER_MASK);
  INIT_REG(PA_SC_AA_CONFIG);
  INIT_REG(PA_SC_SHADER_CONTROL);
  INIT_REG_GFX10_PLUS(gfxIp.major, PA_STEREO_CNTL);
  INIT_REG_GFX10_PLUS(gfxIp.major, GE_STEREO_CNTL);
  INIT_REG_APU09_1X_PLUS(gfxIp.major, SPI_SHADER_PGM_CHKSUM_PS);

  INIT_REG_GFX10_PLUS(gfxIp.major, SPI_SHADER_PGM_RSRC4_PS);

  INIT_REG_GFX10_PLUS(gfxIp.major, GE_USER_VGPR_EN);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineVsFsRegConfig::PipelineVsFsRegConfig(GfxIpVersion gfxIp) : vsRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
  INIT_REG_GFX9_10(gfxIp.major, VGT_GS_ONCHIP_CNTL);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineVsTsFsRegConfig::PipelineVsTsFsRegConfig(GfxIpVersion gfxIp) : lsHsRegs(gfxIp), vsRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
  INIT_REG_GFX9_10(gfxIp.major, VGT_GS_ONCHIP_CNTL);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineVsGsFsRegConfig::PipelineVsGsFsRegConfig(GfxIpVersion gfxIp) : esGsRegs(gfxIp), vsRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
}

// =====================================================================================================================
// Initializer
PipelineVsTsGsFsRegConfig::PipelineVsTsGsFsRegConfig(GfxIpVersion gfxIp)
    : lsHsRegs(gfxIp), esGsRegs(gfxIp), vsRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineNggVsFsRegConfig::PipelineNggVsFsRegConfig(GfxIpVersion gfxIp) : primShaderRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineNggVsTsFsRegConfig::PipelineNggVsTsFsRegConfig(GfxIpVersion gfxIp)
    : lsHsRegs(gfxIp), primShaderRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineNggVsGsFsRegConfig::PipelineNggVsGsFsRegConfig(GfxIpVersion gfxIp) : primShaderRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineNggVsTsGsFsRegConfig::PipelineNggVsTsGsFsRegConfig(GfxIpVersion gfxIp)
    : lsHsRegs(gfxIp), primShaderRegs(gfxIp), psRegs(gfxIp) {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
CsRegConfig::CsRegConfig(GfxIpVersion gfxIp) {
  INIT_REG(COMPUTE_PGM_RSRC1);
  INIT_REG(COMPUTE_PGM_RSRC2);
  INIT_REG(COMPUTE_NUM_THREAD_X);
  INIT_REG(COMPUTE_NUM_THREAD_Y);
  INIT_REG(COMPUTE_NUM_THREAD_Z);
  INIT_REG_GFX10_PLUS(gfxIp.major, COMPUTE_SHADER_CHKSUM);
  INIT_REG_GFX10_PLUS(gfxIp.major, COMPUTE_PGM_RSRC3);
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
MeshRegConfig::MeshRegConfig(GfxIpVersion gfxIp) {
  assert(gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
  INIT_REG_APU09_1X_PLUS(gfxIp.major, SPI_SHADER_PGM_CHKSUM_GS);

  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG_GFX10(gfxIp.major, IA_MULTI_VGT_PARAM_PIPED);

  INIT_REG(SPI_SHADER_PGM_RSRC1_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC4_GS);
  INIT_REG(VGT_GS_MAX_VERT_OUT);
  INIT_REG(VGT_GS_INSTANCE_CNT);
  INIT_REG(VGT_ESGS_RING_ITEMSIZE);
  INIT_REG(VGT_GS_ONCHIP_CNTL);

  // Special registers, having different register IDs
  if (gfxIp.major == 10) {
    INIT_REG_GFX9_10(gfxIp.major, VGT_GS_OUT_PRIM_TYPE);
  } else if (gfxIp.major == 11) {
    INIT_REG_GFX11(gfxIp.major, VGT_GS_OUT_PRIM_TYPE);
  } else {
    llvm_unreachable("Not implemented!");
  }

  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_PER_VS);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_ITEMSIZE);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_VERT_ITEMSIZE_3);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_1);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_2);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GSVS_RING_OFFSET_3);
  INIT_REG_HAS_HW_VS(gfxIp.major, VGT_GS_MODE);

  INIT_REG_GFX10_PLUS(gfxIp.major, GE_MAX_OUTPUT_PER_SUBGROUP);

  INIT_REG(SPI_SHADER_POS_FORMAT);
  INIT_REG(SPI_VS_OUT_CONFIG);
  INIT_REG(PA_CL_VS_OUT_CNTL);
  INIT_REG(PA_CL_CLIP_CNTL);
  INIT_REG(PA_CL_VTE_CNTL);
  INIT_REG(PA_SU_VTX_CNTL);
  INIT_REG(VGT_PRIMITIVEID_EN);
  INIT_REG(VGT_REUSE_OFF);
  INIT_REG(VGT_DRAW_PAYLOAD_CNTL);

  INIT_REG_GFX10_PLUS(gfxIp.major, GE_NGG_SUBGRP_CNTL);
  INIT_REG_GFX10_PLUS(gfxIp.major, SPI_SHADER_IDX_FORMAT);

  if (gfxIp.major <= 11) {
    INIT_REG_GFX11(gfxIp.major, SPI_SHADER_GS_MESHLET_DIM);
    INIT_REG_GFX11(gfxIp.major, SPI_SHADER_GS_MESHLET_EXP_ALLOC);
  } else {
    llvm_unreachable("Not implemented!");
  }
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineMeshFsRegConfig::PipelineMeshFsRegConfig(GfxIpVersion gfxIp) : meshRegs(gfxIp), psRegs(gfxIp) {
}

// =====================================================================================================================
// Initializer
//
// @param gfxIp : Graphics IP version info
PipelineTaskMeshFsRegConfig::PipelineTaskMeshFsRegConfig(GfxIpVersion gfxIp)
    : taskRegs(gfxIp), meshRegs(gfxIp), psRegs(gfxIp) {
}

} // namespace Gfx9

} // namespace lgc
