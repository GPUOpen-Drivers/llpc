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
 * @file  llpcGfx9Chip.cpp
 * @brief LLPC header file: contains implementations for Gfx9 chips.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-gfx9-chip"

#include "llpcGfx9Chip.h"

namespace Llpc
{

namespace Gfx9
{
#include "gfx9_plus_merged_enum.h"
#include "gfx9_plus_merged_offset.h"

// =====================================================================================================================
// Initializer
void VsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
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
    INIT_REG(VS_SCRATCH_BYTE_SIZE);
    INIT_REG(VS_NUM_USED_VGPRS);
    INIT_REG(VS_NUM_USED_SGPRS);
    INIT_REG(VS_NUM_AVAIL_VGPRS);
    INIT_REG(VS_NUM_AVAIL_SGPRS);
    INIT_REG(USES_VIEWPORT_ARRAY_INDEX);
}

// =====================================================================================================================
// Initializer
void LsHsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    INIT_REG(SPI_SHADER_PGM_RSRC1_HS);
    INIT_REG(SPI_SHADER_PGM_RSRC2_HS);
    INIT_REG(HS_SCRATCH_BYTE_SIZE);
    INIT_REG(HS_NUM_USED_VGPRS);
    INIT_REG(HS_NUM_USED_SGPRS);
    INIT_REG(HS_NUM_AVAIL_VGPRS);
    INIT_REG(HS_NUM_AVAIL_SGPRS);
    INIT_REG(VGT_LS_HS_CONFIG);
    INIT_REG(VGT_HOS_MIN_TESS_LEVEL);
    INIT_REG(VGT_HOS_MAX_TESS_LEVEL);
    INIT_REG(VGT_TF_PARAM);
}

// =====================================================================================================================
// Initializer
void EsGsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    INIT_REG(SPI_SHADER_PGM_RSRC1_GS);
    INIT_REG(SPI_SHADER_PGM_RSRC2_GS);
    INIT_REG(SPI_SHADER_PGM_RSRC4_GS);
    INIT_REG(GS_SCRATCH_BYTE_SIZE);
    INIT_REG(GS_NUM_USED_VGPRS);
    INIT_REG(GS_NUM_USED_SGPRS);
    INIT_REG(GS_NUM_AVAIL_VGPRS);
    INIT_REG(GS_NUM_AVAIL_SGPRS);
    INIT_REG(VGT_GS_MAX_VERT_OUT);
    INIT_REG(VGT_GS_ONCHIP_CNTL);
    INIT_REG(VGT_GS_VERT_ITEMSIZE);
    INIT_REG(VGT_GS_INSTANCE_CNT);
    INIT_REG(VGT_GS_PER_VS);
    INIT_REG(VGT_GS_OUT_PRIM_TYPE);
    INIT_REG(VGT_GSVS_RING_ITEMSIZE);
    INIT_REG(VGT_GS_VERT_ITEMSIZE_1);
    INIT_REG(VGT_GS_VERT_ITEMSIZE_2);
    INIT_REG(VGT_GS_VERT_ITEMSIZE_3);
    INIT_REG(VGT_GSVS_RING_OFFSET_1);
    INIT_REG(VGT_GSVS_RING_OFFSET_2);
    INIT_REG(VGT_GSVS_RING_OFFSET_3);
    INIT_REG(VGT_GS_MODE);
    INIT_REG(VGT_ESGS_RING_ITEMSIZE);
    INIT_REG_GFX9(gfxIp.major, VGT_GS_MAX_PRIMS_PER_SUBGROUP);
}

// =====================================================================================================================
// Initializer
void PsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
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
    INIT_REG(PS_USES_UAVS);
    INIT_REG(PS_SCRATCH_BYTE_SIZE);
    INIT_REG(PS_NUM_USED_VGPRS);
    INIT_REG(PS_NUM_USED_SGPRS);
    INIT_REG(PS_NUM_AVAIL_VGPRS);
    INIT_REG(PS_NUM_AVAIL_SGPRS);
    INIT_REG(PA_SC_AA_CONFIG);
    INIT_REG(PA_SC_SHADER_CONTROL);
    INIT_REG(PA_SC_CONSERVATIVE_RASTERIZATION_CNTL);
}

// =====================================================================================================================
// Initializer
void PipelineRegConfig::Init()
{
    INIT_REG(USER_DATA_LIMIT);
    INIT_REG(SPILL_THRESHOLD);
    INIT_REG(PIPELINE_HASH_LO);
    INIT_REG(PIPELINE_HASH_HI);
    INIT_REG(API_HW_SHADER_MAPPING_LO);
    INIT_REG(API_HW_SHADER_MAPPING_HI);
    SET_REG(this, SPILL_THRESHOLD, UINT32_MAX);
}

// =====================================================================================================================
// Initializer
void PipelineVsFsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    m_vsRegs.Init(gfxIp);
    m_psRegs.Init(gfxIp);
    PipelineRegConfig::Init();

    INIT_REG(VGT_SHADER_STAGES_EN);
    INIT_REG(API_VS_HASH_DWORD0);
    INIT_REG(API_VS_HASH_DWORD1);
    INIT_REG(API_PS_HASH_DWORD0);
    INIT_REG(API_PS_HASH_DWORD1);
    INIT_REG(INDIRECT_TABLE_ENTRY);
    INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);

    m_dynRegCount = 0;
}

// =====================================================================================================================
// Initializer
void PipelineVsTsFsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    m_lsHsRegs.Init(gfxIp);
    m_vsRegs.Init(gfxIp);
    m_psRegs.Init(gfxIp);
    PipelineRegConfig::Init();

    INIT_REG(VGT_SHADER_STAGES_EN);
    INIT_REG(API_VS_HASH_DWORD0);
    INIT_REG(API_VS_HASH_DWORD1);
    INIT_REG(API_HS_HASH_DWORD0);
    INIT_REG(API_HS_HASH_DWORD1);
    INIT_REG(API_DS_HASH_DWORD0);
    INIT_REG(API_DS_HASH_DWORD1);
    INIT_REG(API_PS_HASH_DWORD0);
    INIT_REG(API_PS_HASH_DWORD1);
    INIT_REG(INDIRECT_TABLE_ENTRY);
    INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);

    m_dynRegCount = 0;
}

// =====================================================================================================================
// Initializer
void PipelineVsGsFsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    m_esGsRegs.Init(gfxIp);
    m_vsRegs.Init(gfxIp);
    m_psRegs.Init(gfxIp);
    PipelineRegConfig::Init();

    INIT_REG(VGT_SHADER_STAGES_EN);
    INIT_REG(API_VS_HASH_DWORD0);
    INIT_REG(API_VS_HASH_DWORD1);
    INIT_REG(API_GS_HASH_DWORD0);
    INIT_REG(API_GS_HASH_DWORD1);
    INIT_REG(API_PS_HASH_DWORD0);
    INIT_REG(API_PS_HASH_DWORD1);
    INIT_REG(INDIRECT_TABLE_ENTRY);
    INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);

    m_dynRegCount = 0;
}

// =====================================================================================================================
// Initializer
void PipelineVsTsGsFsRegConfig::Init(GfxIpVersion gfxIp)
{
    m_lsHsRegs.Init(gfxIp);
    m_esGsRegs.Init(gfxIp);
    m_psRegs.Init(gfxIp);
    m_vsRegs.Init(gfxIp);
    PipelineRegConfig::Init();

    INIT_REG(VGT_SHADER_STAGES_EN);
    INIT_REG(API_VS_HASH_DWORD0);
    INIT_REG(API_VS_HASH_DWORD1);
    INIT_REG(API_HS_HASH_DWORD0);
    INIT_REG(API_HS_HASH_DWORD1);
    INIT_REG(API_DS_HASH_DWORD0);
    INIT_REG(API_DS_HASH_DWORD1);
    INIT_REG(API_GS_HASH_DWORD0);
    INIT_REG(API_GS_HASH_DWORD1);
    INIT_REG(API_PS_HASH_DWORD0);
    INIT_REG(API_PS_HASH_DWORD1);
    INIT_REG(INDIRECT_TABLE_ENTRY);
    INIT_REG_GFX9(gfxIp.major, IA_MULTI_VGT_PARAM);

    m_dynRegCount = 0;
}

// =====================================================================================================================
// Initializer
void CsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    INIT_REG(COMPUTE_PGM_RSRC1);
    INIT_REG(COMPUTE_PGM_RSRC2);
    INIT_REG(COMPUTE_NUM_THREAD_X);
    INIT_REG(COMPUTE_NUM_THREAD_Y);
    INIT_REG(COMPUTE_NUM_THREAD_Z);
    INIT_REG(CS_SCRATCH_BYTE_SIZE);
    INIT_REG(CS_NUM_USED_VGPRS);
    INIT_REG(CS_NUM_USED_SGPRS);
    INIT_REG(CS_NUM_AVAIL_VGPRS);
    INIT_REG(CS_NUM_AVAIL_SGPRS);
}

// =====================================================================================================================
// Initializer
void PipelineCsRegConfig::Init(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    m_csRegs.Init(gfxIp);
    PipelineRegConfig::Init();

    INIT_REG(API_CS_HASH_DWORD0);
    INIT_REG(API_CS_HASH_DWORD1);

    m_dynRegCount = 0;
}

// =====================================================================================================================
// Adds entries to register name map.
void InitRegisterNameMap(
    GfxIpVersion gfxIp) // Graphics IP version info
{
    LLPC_ASSERT((gfxIp.major == 9) || (gfxIp.major == 10));

    ADD_REG_MAP(SPI_SHADER_PGM_RSRC1_VS);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC2_VS);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC1_HS);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC2_HS);
    ADD_REG_MAP(SPI_SHADER_POS_FORMAT);
    ADD_REG_MAP(SPI_VS_OUT_CONFIG);
    ADD_REG_MAP(PA_CL_VS_OUT_CNTL);
    ADD_REG_MAP(PA_CL_CLIP_CNTL);
    ADD_REG_MAP(PA_CL_VTE_CNTL);
    ADD_REG_MAP(PA_SU_VTX_CNTL);
    ADD_REG_MAP(PA_SC_MODE_CNTL_1);
    ADD_REG_MAP(VGT_PRIMITIVEID_EN);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC1_GS);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC2_GS);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC4_GS);
    ADD_REG_MAP(COMPUTE_PGM_RSRC1);
    ADD_REG_MAP(COMPUTE_PGM_RSRC2);
    ADD_REG_MAP(COMPUTE_TMPRING_SIZE);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC1_PS);
    ADD_REG_MAP(SPI_SHADER_PGM_RSRC2_PS);
    ADD_REG_MAP(SPI_PS_INPUT_ENA);
    ADD_REG_MAP(SPI_PS_INPUT_ADDR);
    ADD_REG_MAP(SPI_INTERP_CONTROL_0);
    ADD_REG_MAP(SPI_TMPRING_SIZE);
    ADD_REG_MAP(SPI_SHADER_Z_FORMAT);
    ADD_REG_MAP(SPI_SHADER_COL_FORMAT);
    ADD_REG_MAP(DB_SHADER_CONTROL);
    ADD_REG_MAP(CB_SHADER_MASK);
    ADD_REG_MAP(SPI_PS_IN_CONTROL);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_0);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_1);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_2);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_3);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_4);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_5);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_6);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_7);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_8);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_9);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_10);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_11);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_12);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_13);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_14);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_15);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_16);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_17);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_18);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_19);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_20);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_21);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_22);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_23);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_24);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_25);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_26);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_27);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_28);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_29);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_30);
    ADD_REG_MAP(SPI_PS_INPUT_CNTL_31);

    ADD_REG_MAP(VGT_GS_MAX_VERT_OUT);
    ADD_REG_MAP(VGT_ESGS_RING_ITEMSIZE);
    ADD_REG_MAP(VGT_GS_MODE);
    ADD_REG_MAP(VGT_GS_ONCHIP_CNTL);
    ADD_REG_MAP(VGT_GS_VERT_ITEMSIZE);
    ADD_REG_MAP(VGT_GS_VERT_ITEMSIZE_1);
    ADD_REG_MAP(VGT_GS_VERT_ITEMSIZE_2);
    ADD_REG_MAP(VGT_GS_VERT_ITEMSIZE_3);
    ADD_REG_MAP(VGT_GSVS_RING_OFFSET_1);
    ADD_REG_MAP(VGT_GSVS_RING_OFFSET_2);
    ADD_REG_MAP(VGT_GSVS_RING_OFFSET_3);

    ADD_REG_MAP(VGT_GS_INSTANCE_CNT);
    ADD_REG_MAP(VGT_GS_PER_VS);
    ADD_REG_MAP(VGT_GS_OUT_PRIM_TYPE);
    ADD_REG_MAP(VGT_GSVS_RING_ITEMSIZE);

    ADD_REG_MAP(VGT_SHADER_STAGES_EN);
    ADD_REG_MAP(VGT_REUSE_OFF);
    ADD_REG_MAP(SPI_BARYC_CNTL);

    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_0);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_1);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_2);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_3);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_4);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_5);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_6);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_7);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_8);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_9);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_10);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_11);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_12);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_13);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_14);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_15);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_16);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_17);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_18);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_19);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_20);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_21);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_22);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_23);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_24);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_25);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_26);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_27);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_28);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_29);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_30);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_VS_31);

    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_0);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_1);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_2);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_3);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_4);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_5);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_6);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_7);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_8);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_9);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_10);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_11);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_12);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_13);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_14);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_ES_15);

    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_0);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_1);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_2);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_3);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_4);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_5);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_6);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_7);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_8);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_9);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_10);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_11);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_12);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_13);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_14);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_15);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_16);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_17);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_18);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_19);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_20);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_21);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_22);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_23);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_24);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_25);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_26);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_27);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_28);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_29);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_30);
    ADD_REG_MAP(SPI_SHADER_USER_DATA_PS_31);

    ADD_REG_MAP(COMPUTE_USER_DATA_0);
    ADD_REG_MAP(COMPUTE_USER_DATA_1);
    ADD_REG_MAP(COMPUTE_USER_DATA_2);
    ADD_REG_MAP(COMPUTE_USER_DATA_3);
    ADD_REG_MAP(COMPUTE_USER_DATA_4);
    ADD_REG_MAP(COMPUTE_USER_DATA_5);
    ADD_REG_MAP(COMPUTE_USER_DATA_6);
    ADD_REG_MAP(COMPUTE_USER_DATA_7);
    ADD_REG_MAP(COMPUTE_USER_DATA_8);
    ADD_REG_MAP(COMPUTE_USER_DATA_9);
    ADD_REG_MAP(COMPUTE_USER_DATA_10);
    ADD_REG_MAP(COMPUTE_USER_DATA_11);
    ADD_REG_MAP(COMPUTE_USER_DATA_12);
    ADD_REG_MAP(COMPUTE_USER_DATA_13);
    ADD_REG_MAP(COMPUTE_USER_DATA_14);
    ADD_REG_MAP(COMPUTE_USER_DATA_15);

    ADD_REG_MAP(COMPUTE_NUM_THREAD_X);
    ADD_REG_MAP(COMPUTE_NUM_THREAD_Y);
    ADD_REG_MAP(COMPUTE_NUM_THREAD_Z);
    ADD_REG_MAP(VGT_GS_MODE);
    ADD_REG_MAP(VGT_TF_PARAM);
    ADD_REG_MAP(VGT_LS_HS_CONFIG);
    ADD_REG_MAP(VGT_HOS_MIN_TESS_LEVEL);
    ADD_REG_MAP(VGT_HOS_MAX_TESS_LEVEL);
    ADD_REG_MAP(PA_SC_AA_CONFIG);
    ADD_REG_MAP(PA_SC_SHADER_CONTROL);
    ADD_REG_MAP(PA_SC_CONSERVATIVE_RASTERIZATION_CNTL);

    if (gfxIp.major == 9)
    {
        // GFX9 specific
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_16);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_17);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_18);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_19);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_20);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_21);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_22);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_23);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_24);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_25);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_26);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_27);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_28);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_29);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_30);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_ES_31);

        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_0);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_1);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_2);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_3);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_4);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_5);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_6);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_7);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_8);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_9);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_10);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_11);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_12);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_13);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_14);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_15);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_16);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_17);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_18);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_19);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_20);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_21);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_22);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_23);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_24);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_25);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_26);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_27);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_28);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_29);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_30);
        ADD_REG_MAP_GFX9(SPI_SHADER_USER_DATA_LS_31);

        ADD_REG_MAP_GFX9(IA_MULTI_VGT_PARAM);
        ADD_REG_MAP_GFX9(VGT_GS_MAX_PRIMS_PER_SUBGROUP);
    }
    else
    {
    }
}

// =====================================================================================================================
// Gets the name string from byte-based ID of the register
const char* GetRegisterNameString(
    GfxIpVersion gfxIp, // Graphics IP version info
    uint32_t     regId) // ID (byte-based) of the register
{
    LLPC_ASSERT((gfxIp.major == 9) || (gfxIp.major == 10));

    const char* pNameString = nullptr;

    if (RegNameMap.empty())
    {
        InitRegisterNameMap(gfxIp);
    }

    if ((regId / 4 >= Util::Abi::PipelineMetadataBase) &&
        (regId / 4 <= Util::Abi::PipelineMetadataBase + static_cast<uint32_t>(Util::Abi::PipelineMetadataType::Count)))
    {
        pNameString = Util::Abi::PipelineMetadataNameStrings[regId / 4 - Util::Abi::PipelineMetadataBase];
    }
    else if (RegNameMap.find(regId) == RegNameMap.end())
    {
        // Not found, search the GFX-dependent map table
        if (gfxIp.major == 9)
        {
            if (RegNameMapGfx9.find(regId) != RegNameMapGfx9.end())
            {
                pNameString = RegNameMapGfx9[regId];
            }
        }
        else
        {
            LLPC_NOT_IMPLEMENTED();
        }
    }
    else
    {
        pNameString = RegNameMap[regId];
    }

    if (pNameString == nullptr)
    {
        static char unknownRegNameBuf[256] = {};
        int32_t length = snprintf(unknownRegNameBuf, 256, "UNKNOWN(0x%08X)", regId);
        LLPC_UNUSED(length);
        pNameString = unknownRegNameBuf;
    }

    return pNameString;
}

} // Gfx9

} // Llpc
