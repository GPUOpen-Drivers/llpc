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
 * @file  Gfx6Chip.cpp
 * @brief LLPC header file: contains implementations for Gfx6 chips.
 ***********************************************************************************************************************
 */
#include "Gfx6Chip.h"

#define DEBUG_TYPE "lgc-gfx6-chip"

namespace lgc {

namespace Gfx6 {

#include "chip/gfx6/si_ci_vi_merged_enum.h"
#include "chip/gfx6/si_ci_vi_merged_offset.h"

using namespace Pal::Gfx6::Chip;

// =====================================================================================================================
// Initializer
VsRegConfig::VsRegConfig() {
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
  INIT_REG(VGT_VERTEX_REUSE_BLOCK_CNTL);
  INIT_REG(VGT_STRMOUT_CONFIG);
  INIT_REG(VGT_STRMOUT_BUFFER_CONFIG);
  INIT_REG(VGT_STRMOUT_VTX_STRIDE_0);
  INIT_REG(VGT_STRMOUT_VTX_STRIDE_1);
  INIT_REG(VGT_STRMOUT_VTX_STRIDE_2);
  INIT_REG(VGT_STRMOUT_VTX_STRIDE_3);
}

// =====================================================================================================================
// Initializer
HsRegConfig::HsRegConfig() {
  INIT_REG(SPI_SHADER_PGM_RSRC1_HS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_HS);
  INIT_REG(VGT_LS_HS_CONFIG);
  INIT_REG(VGT_HOS_MIN_TESS_LEVEL);
  INIT_REG(VGT_HOS_MAX_TESS_LEVEL);
}

// =====================================================================================================================
// Initializer
EsRegConfig::EsRegConfig() {
  INIT_REG(SPI_SHADER_PGM_RSRC1_ES);
  INIT_REG(SPI_SHADER_PGM_RSRC2_ES);
  INIT_REG(VGT_ESGS_RING_ITEMSIZE);
}

// =====================================================================================================================
// Initializer
LsRegConfig::LsRegConfig() {
  INIT_REG(SPI_SHADER_PGM_RSRC1_LS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_LS);
}

// =====================================================================================================================
// Initializer
GsRegConfig::GsRegConfig() {
  INIT_REG(SPI_SHADER_PGM_RSRC1_GS);
  INIT_REG(SPI_SHADER_PGM_RSRC2_GS);
  INIT_REG(VGT_GS_MAX_VERT_OUT);
  INIT_REG(VGT_GS_ONCHIP_CNTL__CI__VI);
  INIT_REG(VGT_ES_PER_GS);
  INIT_REG(VGT_GS_VERT_ITEMSIZE);
  INIT_REG(VGT_GS_INSTANCE_CNT);
  INIT_REG(VGT_GS_PER_VS);
  INIT_REG(VGT_GS_OUT_PRIM_TYPE);
  INIT_REG(VGT_GSVS_RING_ITEMSIZE);
  INIT_REG(VGT_GS_PER_ES);
  INIT_REG(VGT_GS_VERT_ITEMSIZE_1);
  INIT_REG(VGT_GS_VERT_ITEMSIZE_2);
  INIT_REG(VGT_GS_VERT_ITEMSIZE_3);
  INIT_REG(VGT_GSVS_RING_OFFSET_1);
  INIT_REG(VGT_GSVS_RING_OFFSET_2);
  INIT_REG(VGT_GSVS_RING_OFFSET_3);
  INIT_REG(VGT_GS_MODE);
}

// =====================================================================================================================
// Initializer
PsRegConfig::PsRegConfig() {
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
}

// =====================================================================================================================
// Gets the starting register ID of SPI_PS_INPUT_CNTL.
unsigned PsRegConfig::getPsInputCntlStart() {
  return mmSPI_PS_INPUT_CNTL_0;
}

// =====================================================================================================================
// Gets the starting register ID of SPI_SHADER_USER_DATA_PS.
unsigned PsRegConfig::getPsUserDataStart() {
  return mmSPI_SHADER_USER_DATA_PS_0;
}

// =====================================================================================================================
// Initializer
PipelineVsFsRegConfig::PipelineVsFsRegConfig() {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG(IA_MULTI_VGT_PARAM);
}

// =====================================================================================================================
// Initializer
PipelineVsTsFsRegConfig::PipelineVsTsFsRegConfig() {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG(IA_MULTI_VGT_PARAM);
  INIT_REG(VGT_TF_PARAM);
}

// =====================================================================================================================
// Initializer
PipelineVsGsFsRegConfig::PipelineVsGsFsRegConfig() {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG(IA_MULTI_VGT_PARAM);
}

// =====================================================================================================================
// Initializer
PipelineVsTsGsFsRegConfig::PipelineVsTsGsFsRegConfig() {
  INIT_REG(VGT_SHADER_STAGES_EN);
  INIT_REG(IA_MULTI_VGT_PARAM);
  INIT_REG(VGT_TF_PARAM);
}

// =====================================================================================================================
// Initializer
CsRegConfig::CsRegConfig() {
  INIT_REG(COMPUTE_PGM_RSRC1);
  INIT_REG(COMPUTE_PGM_RSRC2);
  INIT_REG(COMPUTE_NUM_THREAD_X);
  INIT_REG(COMPUTE_NUM_THREAD_Y);
  INIT_REG(COMPUTE_NUM_THREAD_Z);
}

} // namespace Gfx6

} // namespace lgc
