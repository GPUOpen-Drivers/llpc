/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcAbi.h
 * @brief LLPC header file: contains declarations for parts of PAL pipeline ABI
 ***********************************************************************************************************************
 */
#pragma once

#include <stdint.h>

namespace lgc
{

// Internal resource table's virtual bindings
static const uint32_t SI_DRV_TABLE_SCRATCH_GFX_SRD_OFFS = 0;
static const uint32_t SI_DRV_TABLE_SCRATCH_CS_SRD_OFFS  = 1;
static const uint32_t SI_DRV_TABLE_ES_RING_OUT_OFFS     = 2;
static const uint32_t SI_DRV_TABLE_GS_RING_IN_OFFS      = 3;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT0_OFFS    = 4;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT1_OFFS    = 5;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT2_OFFS    = 6;
static const uint32_t SI_DRV_TABLE_GS_RING_OUT3_OFFS    = 7;
static const uint32_t SI_DRV_TABLE_VS_RING_IN_OFFS      = 8;
static const uint32_t SI_DRV_TABLE_TF_BUFFER_OFFS       = 9;
static const uint32_t SI_DRV_TABLE_HS_BUFFER0_OFFS      = 10;
static const uint32_t SI_DRV_TABLE_OFF_CHIP_PARAM_CACHE = 11;
static const uint32_t SI_DRV_TABLE_SAMPLEPOS            = 12;

static const uint32_t SI_STREAMOUT_TABLE_OFFS           = 0;

namespace Util
{

namespace Abi
{

// Name prefix of the section where our pipeline binaries store extra information e.g. LLVM IR.
static constexpr char AmdGpuCommentName[] = ".AMDGPU.comment.";

// Symbol names for shader entry-points
static constexpr char AmdGpuLsEntryName[] = "_amdgpu_ls_main";
static constexpr char AmdGpuHsEntryName[] = "_amdgpu_hs_main";
static constexpr char AmdGpuEsEntryName[] = "_amdgpu_es_main";
static constexpr char AmdGpuGsEntryName[] = "_amdgpu_gs_main";
static constexpr char AmdGpuVsEntryName[] = "_amdgpu_vs_main";
static constexpr char AmdGpuPsEntryName[] = "_amdgpu_ps_main";
static constexpr char AmdGpuCsEntryName[] = "_amdgpu_cs_main";

/// Maximum number of viewports.
constexpr uint32_t MaxViewports = 16;

/// Constant buffer used by primitive shader generation for per-submit register controls of culling.
struct PrimShaderPsoCb
{
    uint32_t gsAddressLo;              ///< Low 32-bits of GS address used for a jump from ES.
    uint32_t gsAddressHi;              ///< High 32-bits of GS address used for a jump from ES.
    uint32_t paClVteCntl;              ///< Viewport transform control.
    uint32_t paSuVtxCntl;              ///< Controls for float to fixed vertex conversion.
    uint32_t paClClipCntl;             ///< Clip space controls.
    uint32_t paScWindowOffset;         ///< Offset for vertices in screen space.
    uint32_t paSuHardwareScreenOffset; ///< Offset for guardband.
    uint32_t paSuScModeCntl;           ///< Culling controls.
    uint32_t paClGbHorzClipAdj;        ///< Frustrum horizontal adjacent culling control.
    uint32_t paClGbVertClipAdj;        ///< Frustrum vertical adjacent culling control.
    uint32_t paClGbHorzDiscAdj;        ///< Frustrum horizontal discard culling control.
    uint32_t paClGbVertDiscAdj;        ///< Frustrum vertical discard culling control.
    uint32_t vgtPrimitiveType;         ///< Runtime handling of primitive type
};

/// Constant buffer used by primitive shader generation for per-submit register controls of viewport transform.
struct PrimShaderVportCb
{
    struct
    {
        /// Viewport transform scale and offset for x, y, z components
        uint32_t paClVportXscale;
        uint32_t paClVportXoffset;
        uint32_t paClVportYscale;
        uint32_t paClVportYoffset;
        uint32_t paClVportZscale;
        uint32_t paClVportZoffset;
    } vportControls[MaxViewports];
};

/// Constant buffer used by primitive shader generation for per-submit register controls of bounding boxes.
struct PrimShaderScissorCb
{
    struct
    {
        /// Viewport scissor that defines a bounding box
        uint32_t paScVportScissorTL;
        uint32_t paScVportScissorBR;
    } scissorControls[MaxViewports];
};

/// Constant buffer used by the primitive shader generation for various render state not known until draw time
struct PrimShaderRenderCb
{
    uint32_t primitiveRestartEnable; ///< Enable resetting of a triangle strip using a special index.
    uint32_t primitiveRestartIndex;  ///< Value used to determine if a primitive restart is triggered
    uint32_t matchAllBits;           ///< When comparing restart indices, this limits number of bits
};

/// This struct defines the expected layout in memory when 'contiguousCbs' is set
struct PrimShaderCbLayout
{
    PrimShaderPsoCb     pipelineStateCb;
    PrimShaderVportCb   viewportStateCb;
    PrimShaderScissorCb scissorStateCb;
    PrimShaderRenderCb  renderStateCb;
};

} // Abi

} // Util

} // lgc
