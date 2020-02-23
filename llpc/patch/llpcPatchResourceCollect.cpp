/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"
#include "llpcIntrinsDefs.h"
#include "llpcNggLdsManager.h"
#include "llpcPatchResourceCollect.h"
#include "llpcPipelineShaders.h"
#include "llpcTargetInfo.h"
#include <algorithm>
#include <functional>

#define DEBUG_TYPE "llpc-patch-resource-collect"

using namespace llvm;
using namespace Llpc;

// -disable-gs-onchip: disable geometry shader on-chip mode
cl::opt<bool> DisableGsOnChip("disable-gs-onchip",
                              cl::desc("Disable geometry shader on-chip mode"),
                              cl::init(false));

// -pack-in-out: pack input/output
static cl::opt<bool> PackInOut("pack-in-out", cl::desc("Pack input/output"), cl::init(false));

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchResourceCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for resource collecting
ModulePass* CreatePatchResourceCollect()
{
    return new PatchResourceCollect();
}

// =====================================================================================================================
PatchResourceCollect::PatchResourceCollect()
    :
    Patch(ID),
    m_hasPushConstOp(false),
    m_hasDynIndexedInput(false),
    m_hasDynIndexedOutput(false),
    m_pResUsage(nullptr)
{
    m_pLocationMapManager.reset(new InOutLocationMapManager);
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchResourceCollect::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Resource-Collect\n");

    Patch::Init(&module);
    m_pPipelineShaders = &getAnalysis<PipelineShaders>();
    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);

    // If packing final vertex stage outputs and FS inputs, scalarize those outputs and inputs now.
    if (CanPackInOut())
    {
        ScalarizeForInOutPacking(&module);
    }

    // Process each shader stage, in reverse order.
    for (int32_t shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage)
    {
        m_pEntryPoint = m_pPipelineShaders->GetEntryPoint(static_cast<ShaderStage>(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = static_cast<ShaderStage>(shaderStage);
            ProcessShader();
        }
    }

    if (m_pPipelineState->IsGraphics())
    {
        // Set NGG control settings
        SetNggControl();

        // Determine whether or not GS on-chip mode is valid for this pipeline
        bool hasGs = m_pPipelineState->HasShaderStage(ShaderStageGeometry);
        bool checkGsOnChip = hasGs || m_pPipelineState->GetNggControl()->enableNgg;

        if (checkGsOnChip)
        {
            bool gsOnChip = CheckGsOnChipValidity();
            m_pPipelineState->SetGsOnChip(gsOnChip);
        }
    }

    return true;
}

// =====================================================================================================================
// Sets NGG control settings
void PatchResourceCollect::SetNggControl()
{
    // For GFX10+, initialize NGG control settings
    if (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major < 10)
    {
        return;
    }

    uint32_t stageMask = m_pPipelineState->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                        ShaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    // Check the use of cull distance for NGG primitive shader
    bool useCullDistance = false;
    bool enableXfb = false;
    if (hasGs)
    {
        const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
        enableXfb = pResUsage->inOutUsage.enableXfb;
    }
    else
    {
        if (hasTs)
        {
            const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval);
            const auto& builtInUsage = pResUsage->builtInUsage.tes;
            useCullDistance = (builtInUsage.cullDistance > 0);
            enableXfb = pResUsage->inOutUsage.enableXfb;
        }
        else
        {
            const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex);
            const auto& builtInUsage = pResUsage->builtInUsage.vs;
            useCullDistance = (builtInUsage.cullDistance > 0);
            enableXfb = pResUsage->inOutUsage.enableXfb;
        }
    }

    const auto& options = m_pPipelineState->GetOptions();
    NggControl& nggControl = *m_pPipelineState->GetNggControl();

    bool enableNgg = (options.nggFlags & NggFlagDisable) == 0;
    if (enableXfb)
    {
        // TODO: If transform feedback is enabled, disable NGG.
        enableNgg = false;
    }

    if (hasGs && ((options.nggFlags & NggFlagEnableGsUse) == 0))
    {
        // NOTE: NGG used on GS is disabled by default
        enableNgg = false;
    }

    if (m_pPipelineState->GetTargetInfo().GetGpuWorkarounds().gfx10.waNggDisabled)
    {
        enableNgg = false;
    }

    nggControl.enableNgg                  = enableNgg;
    nggControl.enableGsUse                = (options.nggFlags & NggFlagEnableGsUse);
    nggControl.alwaysUsePrimShaderTable   = (options.nggFlags & NggFlagDontAlwaysUsePrimShaderTable) == 0;
    nggControl.compactMode                = (options.nggFlags & NggFlagCompactSubgroup) ?
                                             NggCompactSubgroup : NggCompactVertices;

    nggControl.enableFastLaunch           = (options.nggFlags & NggFlagEnableFastLaunch);
    nggControl.enableVertexReuse          = (options.nggFlags & NggFlagEnableVertexReuse);
    nggControl.enableBackfaceCulling      = (options.nggFlags & NggFlagEnableBackfaceCulling);
    nggControl.enableFrustumCulling       = (options.nggFlags & NggFlagEnableFrustumCulling);
    nggControl.enableBoxFilterCulling     = (options.nggFlags & NggFlagEnableBoxFilterCulling);
    nggControl.enableSphereCulling        = (options.nggFlags & NggFlagEnableSphereCulling);
    nggControl.enableSmallPrimFilter      = (options.nggFlags & NggFlagEnableSmallPrimFilter);
    nggControl.enableCullDistanceCulling  = ((options.nggFlags & NggFlagEnableCullDistanceCulling) &&
                                            useCullDistance);

    nggControl.backfaceExponent           = options.nggBackfaceExponent;
    nggControl.subgroupSizing             = options.nggSubgroupSizing;
    nggControl.primsPerSubgroup           = std::min(options.nggPrimsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup);
    nggControl.vertsPerSubgroup           = std::min(options.nggVertsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup);

    if (nggControl.enableNgg)
    {
        if (options.nggFlags & NggFlagForceNonPassthrough)
        {
            nggControl.passthroughMode = false;
        }
        else
        {
            nggControl.passthroughMode = (nggControl.enableVertexReuse == false) &&
                                           (nggControl.enableBackfaceCulling == false) &&
                                           (nggControl.enableFrustumCulling == false) &&
                                           (nggControl.enableBoxFilterCulling == false) &&
                                           (nggControl.enableSphereCulling == false) &&
                                           (nggControl.enableSmallPrimFilter == false) &&
                                           (nggControl.enableCullDistanceCulling == false);
        }

        // NOTE: Further check if we have to turn on pass-through mode forcibly.
        if (nggControl.passthroughMode == false)
        {
            // NOTE: Further check if pass-through mode should be enabled
            const auto topology = m_pPipelineState->GetInputAssemblyState().topology;
            if ((topology == PrimitiveTopology::PointList) ||
                (topology == PrimitiveTopology::LineList)  ||
                (topology == PrimitiveTopology::LineStrip) ||
                (topology == PrimitiveTopology::LineListWithAdjacency) ||
                (topology == PrimitiveTopology::LineStripWithAdjacency))
            {
                // NGG runs in pass-through mode for non-triangle primitives
                nggControl.passthroughMode = true;
            }
            else if (topology == PrimitiveTopology::PatchList)
            {
                // NGG runs in pass-through mode for non-triangle tessellation output
                LLPC_ASSERT(hasTs);

                const auto& tessMode = m_pPipelineState->GetShaderModes()->GetTessellationMode();
                if (tessMode.pointMode || (tessMode.primitiveMode == PrimitiveMode::Isolines))
                {
                    nggControl.passthroughMode = true;
                }
            }

            const auto polygonMode = m_pPipelineState->GetRasterizerState().polygonMode;
            if ((polygonMode == PolygonModeLine) || (polygonMode == PolygonModePoint))
            {
                // NGG runs in pass-through mode for non-fill polygon mode
                nggControl.passthroughMode = true;
            }

            if (hasGs)
            {
                const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
                if (geometryMode.outputPrimitive != OutputPrimitives::TriangleStrip)
                {
                    // If GS output primitive type is not triangle strip, NGG runs in "pass-through"
                    // (actual no culling) mode
                    nggControl.passthroughMode = true;
                }
            }
        }

        // Build NGG culling-control registers
        BuildNggCullingControlRegister(nggControl);

        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC NGG control settings results\n\n");

        // Control option
        LLPC_OUTS("EnableNgg                    = " << nggControl.enableNgg << "\n");
        LLPC_OUTS("EnableGsUse                  = " << nggControl.enableGsUse << "\n");
        LLPC_OUTS("AlwaysUsePrimShaderTable     = " << nggControl.alwaysUsePrimShaderTable << "\n");
        LLPC_OUTS("PassthroughMode              = " << nggControl.passthroughMode << "\n");
        LLPC_OUTS("CompactMode                  = ");
        switch (nggControl.compactMode)
        {
        case NggCompactSubgroup:
            LLPC_OUTS("Subgroup\n");
            break;
        case NggCompactVertices:
            LLPC_OUTS("Vertices\n");
            break;
        default:
            break;
        }
        LLPC_OUTS("EnableFastLaunch             = " << nggControl.enableFastLaunch << "\n");
        LLPC_OUTS("EnableVertexReuse            = " << nggControl.enableVertexReuse << "\n");
        LLPC_OUTS("EnableBackfaceCulling        = " << nggControl.enableBackfaceCulling << "\n");
        LLPC_OUTS("EnableFrustumCulling         = " << nggControl.enableFrustumCulling << "\n");
        LLPC_OUTS("EnableBoxFilterCulling       = " << nggControl.enableBoxFilterCulling << "\n");
        LLPC_OUTS("EnableSphereCulling          = " << nggControl.enableSphereCulling << "\n");
        LLPC_OUTS("EnableSmallPrimFilter        = " << nggControl.enableSmallPrimFilter << "\n");
        LLPC_OUTS("EnableCullDistanceCulling    = " << nggControl.enableCullDistanceCulling << "\n");
        LLPC_OUTS("BackfaceExponent             = " << nggControl.backfaceExponent << "\n");
        LLPC_OUTS("SubgroupSizing               = ");
        switch (nggControl.subgroupSizing)
        {
        case NggSubgroupSizingType::Auto:
            LLPC_OUTS("Auto\n");
            break;
        case NggSubgroupSizingType::MaximumSize:
            LLPC_OUTS("MaximumSize\n");
            break;
        case NggSubgroupSizingType::HalfSize:
            LLPC_OUTS("HalfSize\n");
            break;
        case NggSubgroupSizingType::OptimizeForVerts:
            LLPC_OUTS("OptimizeForVerts\n");
            break;
        case NggSubgroupSizingType::OptimizeForPrims:
            LLPC_OUTS("OptimizeForPrims\n");
            break;
        case NggSubgroupSizingType::Explicit:
            LLPC_OUTS("Explicit\n");
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }
        LLPC_OUTS("PrimsPerSubgroup             = " << nggControl.primsPerSubgroup << "\n");
        LLPC_OUTS("VertsPerSubgroup             = " << nggControl.vertsPerSubgroup << "\n");
        LLPC_OUTS("\n");
    }
}

// =====================================================================================================================
// Builds NGG culling-control registers (fill part of compile-time primitive shader table).
void PatchResourceCollect::BuildNggCullingControlRegister(
    NggControl& nggControl)   // [in/out] NggControl struct
{
    const auto& vpState = m_pPipelineState->GetViewportState();
    const auto& rsState = m_pPipelineState->GetRasterizerState();

    auto& pipelineState = nggControl.primShaderTable.pipelineStateCb;

    //
    // Program register PA_SU_SC_MODE_CNTL
    //
    PaSuScModeCntl paSuScModeCntl;
    paSuScModeCntl.u32All = 0;

    paSuScModeCntl.bits.POLY_OFFSET_FRONT_ENABLE = rsState.depthBiasEnable;
    paSuScModeCntl.bits.POLY_OFFSET_BACK_ENABLE  = rsState.depthBiasEnable;
    paSuScModeCntl.bits.MULTI_PRIM_IB_ENA        = true;

    paSuScModeCntl.bits.POLY_MODE            = (rsState.polygonMode != PolygonModeFill);

    if (rsState.polygonMode == PolygonModeFill)
    {
        paSuScModeCntl.bits.POLYMODE_BACK_PTYPE  = POLY_MODE_TRIANGLES;
        paSuScModeCntl.bits.POLYMODE_FRONT_PTYPE = POLY_MODE_TRIANGLES;
    }
    else if (rsState.polygonMode == PolygonModeLine)
    {
        paSuScModeCntl.bits.POLYMODE_BACK_PTYPE  = POLY_MODE_LINES;
        paSuScModeCntl.bits.POLYMODE_FRONT_PTYPE = POLY_MODE_LINES;
    }
    else if (rsState.polygonMode == PolygonModePoint)
    {
        paSuScModeCntl.bits.POLYMODE_BACK_PTYPE  = POLY_MODE_POINTS;
        paSuScModeCntl.bits.POLYMODE_FRONT_PTYPE = POLY_MODE_POINTS;
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    paSuScModeCntl.bits.CULL_FRONT = ((rsState.cullMode & CullModeFront) != 0);
    paSuScModeCntl.bits.CULL_BACK  = ((rsState.cullMode & CullModeBack) != 0);

    paSuScModeCntl.bits.FACE = rsState.frontFaceClockwise;

    pipelineState.paSuScModeCntl = paSuScModeCntl.u32All;

    //
    // Program register PA_CL_CLIP_CNTL
    //
    PaClClipCntl paClClipCntl;
    LLPC_ASSERT((rsState.usrClipPlaneMask & ~0x3F) == 0);
    paClClipCntl.u32All = rsState.usrClipPlaneMask;

    paClClipCntl.bits.DX_CLIP_SPACE_DEF = true;
    paClClipCntl.bits.DX_LINEAR_ATTR_CLIP_ENA = true;

    if (vpState.depthClipEnable == false)
    {
        paClClipCntl.bits.ZCLIP_NEAR_DISABLE = true;
        paClClipCntl.bits.ZCLIP_FAR_DISABLE  = true;
    }

    if (rsState.rasterizerDiscardEnable)
    {
        paClClipCntl.bits.DX_RASTERIZATION_KILL = true;
    }

    pipelineState.paClClipCntl = paClClipCntl.u32All;

    //
    // Program register PA_CL_VTE_CNTL
    //
    PaClVteCntl paClVteCntl;
    paClVteCntl.u32All = 0;

    paClVteCntl.bits.VPORT_X_SCALE_ENA  = true;
    paClVteCntl.bits.VPORT_X_OFFSET_ENA = true;
    paClVteCntl.bits.VPORT_Y_SCALE_ENA  = true;
    paClVteCntl.bits.VPORT_Y_OFFSET_ENA = true;
    paClVteCntl.bits.VPORT_Z_SCALE_ENA  = true;
    paClVteCntl.bits.VPORT_Z_OFFSET_ENA = true;
    paClVteCntl.bits.VTX_W0_FMT         = true;

    pipelineState.paClVteCntl = paClVteCntl.u32All;
}

// =====================================================================================================================
// Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
bool PatchResourceCollect::CheckGsOnChipValidity()
{
    bool gsOnChip = true;

    uint32_t stageMask = m_pPipelineState->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
    auto pGsResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);

    uint32_t inVertsPerPrim = 0;
    bool useAdjacency = false;
    switch (geometryMode.inputPrimitive)
    {
    case InputPrimitives::Points:
        inVertsPerPrim = 1;
        break;
    case InputPrimitives::Lines:
        inVertsPerPrim = 2;
        break;
    case InputPrimitives::LinesAdjacency:
        useAdjacency = true;
        inVertsPerPrim = 4;
        break;
    case InputPrimitives::Triangles:
        inVertsPerPrim = 3;
        break;
    case InputPrimitives::TrianglesAdjacency:
        useAdjacency = true;
        inVertsPerPrim = 6;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    pGsResUsage->inOutUsage.gs.calcFactor.inputVertices = inVertsPerPrim;

    uint32_t outVertsPerPrim = 0;
    switch (geometryMode.outputPrimitive)
    {
    case OutputPrimitives::Points:
        outVertsPerPrim = 1;
        break;
    case OutputPrimitives::LineStrip:
        outVertsPerPrim = 2;
        break;
    case OutputPrimitives::TriangleStrip:
        outVertsPerPrim = 3;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    if (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major <= 8)
    {
        uint32_t gsPrimsPerSubgroup = m_pPipelineState->GetTargetInfo().GetGpuProperty().gsOnChipDefaultPrimsPerSubgroup;

        const uint32_t esGsRingItemSize = 4 * std::max(1u, pGsResUsage->inOutUsage.inputMapLocCount);
        const uint32_t gsInstanceCount  = geometryMode.invocations;
        const uint32_t gsVsRingItemSize = 4 * std::max(1u,
                                                       (pGsResUsage->inOutUsage.outputMapLocCount *
                                                        geometryMode.outputVertices));

        uint32_t esGsRingItemSizeOnChip = esGsRingItemSize;
        uint32_t gsVsRingItemSizeOnChip = gsVsRingItemSize;

        // Optimize ES -> GS ring and GS -> VS ring layout for bank conflicts
        esGsRingItemSizeOnChip |= 1;
        gsVsRingItemSizeOnChip |= 1;

        uint32_t gsVsRingItemSizeOnChipInstanced = gsVsRingItemSizeOnChip * gsInstanceCount;

        uint32_t esMinVertsPerSubgroup = inVertsPerPrim;

        // If the primitive has adjacency half the number of vertices will be reused in multiple primitives.
        if (useAdjacency)
        {
            esMinVertsPerSubgroup >>= 1;
        }

        // There is a hardware requirement for gsPrimsPerSubgroup * gsInstanceCount to be capped by
        // GsOnChipMaxPrimsPerSubgroup for adjacency primitive or when GS instanceing is used.
        if (useAdjacency || (gsInstanceCount > 1))
        {
            gsPrimsPerSubgroup = std::min(gsPrimsPerSubgroup, (Gfx6::GsOnChipMaxPrimsPerSubgroup / gsInstanceCount));
        }

        // Compute GS-VS LDS size based on target GS primitives per subgroup
        uint32_t gsVsLdsSize = (gsVsRingItemSizeOnChipInstanced * gsPrimsPerSubgroup);

        // Compute ES-GS LDS size based on the worst case number of ES vertices needed to create the target number of
        // GS primitives per subgroup.
        const uint32_t reuseOffMultiplier = IsVertexReuseDisabled() ? gsInstanceCount : 1;
        uint32_t worstCaseEsVertsPerSubgroup = esMinVertsPerSubgroup * gsPrimsPerSubgroup * reuseOffMultiplier;
        uint32_t esGsLdsSize = esGsRingItemSizeOnChip * worstCaseEsVertsPerSubgroup;

        // Total LDS use per subgroup aligned to the register granularity
        uint32_t gsOnChipLdsSize =
            alignTo((esGsLdsSize + gsVsLdsSize),
                      static_cast<uint32_t>((1 << m_pPipelineState->GetTargetInfo().GetGpuProperty()
                                            .ldsSizeDwordGranularityShift)));

        // Use the client-specified amount of LDS space per subgroup. If they specified zero, they want us to choose a
        // reasonable default. The final amount must be 128-DWORD aligned.

        uint32_t maxLdsSize = m_pPipelineState->GetTargetInfo().GetGpuProperty().gsOnChipDefaultLdsSizePerSubgroup;

        // TODO: For BONAIRE A0, GODAVARI and KALINDI, set maxLdsSize to 1024 due to SPI barrier management bug

        // If total LDS usage is too big, refactor partitions based on ratio of ES-GS and GS-VS item sizes.
        if (gsOnChipLdsSize > maxLdsSize)
        {
            const uint32_t esGsItemSizePerPrim = esGsRingItemSizeOnChip * esMinVertsPerSubgroup * reuseOffMultiplier;
            const uint32_t itemSizeTotal       = esGsItemSizePerPrim + gsVsRingItemSizeOnChipInstanced;

            esGsLdsSize = alignTo((esGsItemSizePerPrim * maxLdsSize) / itemSizeTotal, esGsItemSizePerPrim);
            gsVsLdsSize = alignDown(maxLdsSize - esGsLdsSize, gsVsRingItemSizeOnChipInstanced);

            gsOnChipLdsSize = maxLdsSize;
        }

        // Based on the LDS space, calculate how many GS prims per subgroup and ES vertices per subgroup can be dispatched.
        gsPrimsPerSubgroup          = (gsVsLdsSize / gsVsRingItemSizeOnChipInstanced);
        uint32_t esVertsPerSubgroup = (esGsLdsSize / (esGsRingItemSizeOnChip * reuseOffMultiplier));

        LLPC_ASSERT(esVertsPerSubgroup >= esMinVertsPerSubgroup);

        // Vertices for adjacency primitives are not always reused. According to
        // hardware engineers, we must restore esMinVertsPerSubgroup for ES_VERTS_PER_SUBGRP.
        if (useAdjacency)
        {
            esMinVertsPerSubgroup = inVertsPerPrim;
        }

        // For normal primitives, the VGT only checks if they are past the ES verts per sub-group after allocating a full
        // GS primitive and if they are, kick off a new sub group. But if those additional ES vertices are unique
        // (e.g. not reused) we need to make sure there is enough LDS space to account for those ES verts beyond
        // ES_VERTS_PER_SUBGRP.
        esVertsPerSubgroup -= (esMinVertsPerSubgroup - 1);

        // TODO: Accept GsOffChipDefaultThreshold from panel option
        // TODO: Value of GsOffChipDefaultThreshold should be 64, due to an issue it's changed to 32 in order to test
        // on-chip GS code generation before fixing that issue.
        // The issue is because we only remove unused builtin output till final GS output store generation, when
        // determining onchip/offchip mode, unused builtin output like PointSize and Clip/CullDistance is factored in
        // LDS usage and deactivates onchip GS when GsOffChipDefaultThreshold  is 64. To fix this we will probably
        // need to clear unused builtin ouput before determining onchip/offchip GS mode.
        constexpr uint32_t GsOffChipDefaultThreshold = 32;

        bool disableGsOnChip = DisableGsOnChip;
        if (hasTs || (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major == 6))
        {
            // GS on-chip is not supportd with tessellation, and is not supportd on GFX6
            disableGsOnChip = true;
        }

        if (disableGsOnChip ||
            ((gsPrimsPerSubgroup * gsInstanceCount) < GsOffChipDefaultThreshold) ||
            (esVertsPerSubgroup == 0))
        {
            gsOnChip = false;
            pGsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup   = 0;
            pGsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup   = 0;
            pGsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize          = 0;
            pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize      = 0;

            pGsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize     = esGsRingItemSize;
            pGsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize     = gsVsRingItemSize;
        }
        else
        {
            pGsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup   = esVertsPerSubgroup;
            pGsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup   = gsPrimsPerSubgroup;
            pGsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize          = esGsLdsSize;
            pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize      = gsOnChipLdsSize;

            pGsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize     = esGsRingItemSizeOnChip;
            pGsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize     = gsVsRingItemSizeOnChip;
        }
    }
    else
    {
        const auto pNggControl = m_pPipelineState->GetNggControl();

        if (pNggControl->enableNgg)
        {
            // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
            const uint32_t esGsRingItemSize = hasGs ? ((4 * std::max(1u,
                                                                     pGsResUsage->inOutUsage.inputMapLocCount)) | 1) :
                                                      4; // Always 4 components for NGG when GS is not present

            const uint32_t gsVsRingItemSize = hasGs ? std::max(1u,
                                                               4 * pGsResUsage->inOutUsage.outputMapLocCount
                                                                 * geometryMode.outputVertices) : 0;

            const uint32_t esExtraLdsSize = NggLdsManager::CalcEsExtraLdsSize(m_pPipelineState) / 4; // In DWORDs
            const uint32_t gsExtraLdsSize = NggLdsManager::CalcGsExtraLdsSize(m_pPipelineState) / 4; // In DWORDs

            // NOTE: Primitive amplification factor must be at least 1. If the maximum number of GS output vertices
            // is too small to form a complete primitive, set the factor to 1.
            uint32_t primAmpFactor = 1;
            if (hasGs && (geometryMode.outputVertices > (outVertsPerPrim - 1)))
            {
                // primAmpFactor = outputVertices - (outVertsPerPrim - 1)
                primAmpFactor = geometryMode.outputVertices - (outVertsPerPrim - 1);
            }

            const uint32_t vertsPerPrimitive = GetVerticesPerPrimitive();

            const bool needsLds = (hasGs ||
                                   (pNggControl->passthroughMode == false) ||
                                   (esExtraLdsSize > 0) || (gsExtraLdsSize > 0));

            uint32_t esVertsPerSubgroup = 0;
            uint32_t gsPrimsPerSubgroup = 0;

            // It is expected that regular launch NGG will be the most prevalent, so handle its logic first.
            if (pNggControl->enableFastLaunch == false)
            {
                // The numbers below come from hardware guidance and most likely require further tuning.
                switch (pNggControl->subgroupSizing)
                {
                case NggSubgroupSizingType::HalfSize:
                    esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
                    gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
                    break;
                case NggSubgroupSizingType::OptimizeForVerts:
                    esVertsPerSubgroup = (hasTs)             ? 128 : 126;
                    gsPrimsPerSubgroup = (hasTs || needsLds) ? 192 : Gfx9::NggMaxThreadsPerSubgroup;
                    break;
                case NggSubgroupSizingType::OptimizeForPrims:
                    esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
                    gsPrimsPerSubgroup = 128;
                    break;
                case NggSubgroupSizingType::Explicit:
                    esVertsPerSubgroup = pNggControl->vertsPerSubgroup;
                    gsPrimsPerSubgroup = pNggControl->primsPerSubgroup;
                    break;
                default:
                case NggSubgroupSizingType::Auto:
                    esVertsPerSubgroup = 126;
                    gsPrimsPerSubgroup = 128;
                    break;
                case NggSubgroupSizingType::MaximumSize:
                    esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
                    gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
                    break;
                }
            }
            else
            {
                // Fast launch NGG launches like a compute shader and bypasses most of the fixed function hardware.
                // As such, the values of esVerts and gsPrims have to be accurate for the primitive type
                // (and vertsPerPrimitive) to avoid hanging.
                switch (pNggControl->subgroupSizing)
                {
                case NggSubgroupSizingType::HalfSize:
                    esVertsPerSubgroup = alignDown((Gfx9::NggMaxThreadsPerSubgroup / 2u), vertsPerPrimitive);
                    gsPrimsPerSubgroup = esVertsPerSubgroup / vertsPerPrimitive;
                    break;
                case NggSubgroupSizingType::OptimizeForVerts:
                    // Currently the programming of OptimizeForVerts is an inverse of MaximumSize. OptimizeForVerts is
                    // not expected to be a performant choice for fast launch, and as such MaximumSize, HalfSize, or
                    // Explicit should be chosen, with Explicit being optimal for non-point topologies.
                    gsPrimsPerSubgroup = alignDown(Gfx9::NggMaxThreadsPerSubgroup, vertsPerPrimitive);
                    esVertsPerSubgroup = gsPrimsPerSubgroup / vertsPerPrimitive;
                    break;
                case NggSubgroupSizingType::Explicit:
                    esVertsPerSubgroup = pNggControl->vertsPerSubgroup;
                    gsPrimsPerSubgroup = pNggControl->primsPerSubgroup;
                    break;
                case NggSubgroupSizingType::OptimizeForPrims:
                    // Currently the programming of OptimizeForPrims is the same as MaximumSize, it is possible that
                    // this might change in the future. OptimizeForPrims is not expected to be a performant choice for
                    // fast launch, and as such MaximumSize, HalfSize, or Explicit should be chosen, with Explicit
                    // being optimal for non-point topologies.
                    // Fallthrough intentional.
                case NggSubgroupSizingType::Auto:
                case NggSubgroupSizingType::MaximumSize:
                default:
                    esVertsPerSubgroup = alignDown(Gfx9::NggMaxThreadsPerSubgroup, vertsPerPrimitive);
                    gsPrimsPerSubgroup = esVertsPerSubgroup / vertsPerPrimitive;
                    break;
                }
            }

            uint32_t gsInstanceCount = std::max(1u, geometryMode.invocations);
            bool enableMaxVertOut = false;

            if (hasGs)
            {
                // NOTE: If primitive amplification is active and the currently calculated gsPrimsPerSubgroup multipled
                // by the amplification factor is larger than the supported number of primitives within a subgroup, we
                // need to shrimp the number of gsPrimsPerSubgroup down to a reasonable level to prevent
                // over-allocating LDS.
                uint32_t maxVertOut = hasGs ? geometryMode.outputVertices : 1;

                LLPC_ASSERT(maxVertOut >= primAmpFactor);

                if ((gsPrimsPerSubgroup * maxVertOut) > Gfx9::NggMaxThreadsPerSubgroup)
                {
                    gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / maxVertOut;
                }

                // Let's take into consideration instancing:
                LLPC_ASSERT(gsInstanceCount >= 1);
                if (gsPrimsPerSubgroup < gsInstanceCount)
                {
                    // NOTE: If supported number of GS primitives within a subgroup is too small to allow GS
                    // instancing, we enable maximum vertex output per GS instance. This will set the register field
                    // EN_MAX_VERT_OUT_PER_GS_INSTANCE and turn off vertex reuse, restricting 1 input GS input
                    // primitive per subgroup and create 1 subgroup per GS instance.
                    enableMaxVertOut = true;
                    gsInstanceCount = 1;
                    gsPrimsPerSubgroup = 1;
                }
                else
                {
                    gsPrimsPerSubgroup /= gsInstanceCount;
                }
                esVertsPerSubgroup = gsPrimsPerSubgroup * maxVertOut;
            }
            else
            {
                // If GS is not present, instance count must be 1
                LLPC_ASSERT(gsInstanceCount == 1);
            }

            // Make sure that we have at least one primitive
            LLPC_ASSERT(gsPrimsPerSubgroup >= 1);

            uint32_t       expectedEsLdsSize = esVertsPerSubgroup * esGsRingItemSize + esExtraLdsSize;
            const uint32_t expectedGsLdsSize = gsPrimsPerSubgroup * gsInstanceCount * gsVsRingItemSize + gsExtraLdsSize;

            if (expectedGsLdsSize == 0)
            {
                LLPC_ASSERT(hasGs == false);

                expectedEsLdsSize = (Gfx9::NggMaxThreadsPerSubgroup * esGsRingItemSize) + esExtraLdsSize;
            }

            const uint32_t ldsSizeDwords =
                alignTo(expectedEsLdsSize + expectedGsLdsSize,
                          static_cast<uint32_t>(1 << m_pPipelineState->GetTargetInfo().GetGpuProperty()
                                                .ldsSizeDwordGranularityShift));

            // Make sure we don't allocate more than what can legally be allocated by a single subgroup on the hardware.
            LLPC_ASSERT(ldsSizeDwords <= 16384);

            pGsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup   = esVertsPerSubgroup;
            pGsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup   = gsPrimsPerSubgroup;

            // EsGsLdsSize is passed in a user data SGPR to the merged shader so that the API GS knows where to start
            // reading out of LDS. EsGsLdsSize is unnecessary when there is no API GS.
            pGsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize          = hasGs ? expectedEsLdsSize : 0;
            pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize      = needsLds ? ldsSizeDwords : 0;

            pGsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize     = esGsRingItemSize;
            pGsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize     = gsVsRingItemSize;

            pGsResUsage->inOutUsage.gs.calcFactor.primAmpFactor        = primAmpFactor;
            pGsResUsage->inOutUsage.gs.calcFactor.enableMaxVertOut     = enableMaxVertOut;

            gsOnChip = true; // In NGG mode, GS is always on-chip since copy shader is not present.
        }
        else
        {
            uint32_t ldsSizeDwordGranularity =
                static_cast<uint32_t>(1 << m_pPipelineState->GetTargetInfo().GetGpuProperty().ldsSizeDwordGranularityShift);

            // gsPrimsPerSubgroup shouldn't be bigger than wave size.
            uint32_t gsPrimsPerSubgroup =
                std::min(m_pPipelineState->GetTargetInfo().GetGpuProperty().gsOnChipDefaultPrimsPerSubgroup,
                         m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry));

            // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
            const uint32_t esGsRingItemSize = (4 * std::max(1u, pGsResUsage->inOutUsage.inputMapLocCount)) | 1;

            const uint32_t gsVsRingItemSize = 4 * std::max(1u,
                                                           (pGsResUsage->inOutUsage.outputMapLocCount *
                                                            geometryMode.outputVertices));

            // NOTE: Make gsVsRingItemSize odd by "| 1", to optimize GS -> VS ring layout for LDS bank conflicts.
            const uint32_t gsVsRingItemSizeOnChip = gsVsRingItemSize | 1;

            const uint32_t gsInstanceCount  = geometryMode.invocations;

            // TODO: Confirm no ES-GS extra LDS space used.
            const uint32_t esGsExtraLdsDwords  = 0;
            const uint32_t maxEsVertsPerSubgroup = Gfx9::OnChipGsMaxEsVertsPerSubgroup;

            uint32_t esMinVertsPerSubgroup = inVertsPerPrim;

            // If the primitive has adjacency half the number of vertices will be reused in multiple primitives.
            if (useAdjacency)
            {
                esMinVertsPerSubgroup >>= 1;
            }

            uint32_t maxGsPrimsPerSubgroup = Gfx9::OnChipGsMaxPrimPerSubgroup;

            // There is a hardware requirement for gsPrimsPerSubgroup * gsInstanceCount to be capped by
            // OnChipGsMaxPrimPerSubgroup for adjacency primitive or when GS instanceing is used.
            if (useAdjacency || (gsInstanceCount > 1))
            {
                maxGsPrimsPerSubgroup = (Gfx9::OnChipGsMaxPrimPerSubgroupAdj / gsInstanceCount);
            }

            gsPrimsPerSubgroup = std::min(gsPrimsPerSubgroup, maxGsPrimsPerSubgroup);

            const uint32_t reuseOffMultiplier = IsVertexReuseDisabled() ? gsInstanceCount : 1;
            uint32_t worstCaseEsVertsPerSubgroup =
                std::min(esMinVertsPerSubgroup * gsPrimsPerSubgroup * reuseOffMultiplier, maxEsVertsPerSubgroup);

            uint32_t esGsLdsSize = (esGsRingItemSize * worstCaseEsVertsPerSubgroup);

            // Total LDS use per subgroup aligned to the register granularity.
            uint32_t gsOnChipLdsSize = alignTo(esGsLdsSize + esGsExtraLdsDwords, ldsSizeDwordGranularity);

            // Use the client-specified amount of LDS space per sub-group. If they specified zero, they want us to
            // choose a reasonable default. The final amount must be 128-DWORD aligned.
            // TODO: Accept DefaultLdsSizePerSubgroup from panel setting
            uint32_t maxLdsSize = Gfx9::DefaultLdsSizePerSubgroup;

            // If total LDS usage is too big, refactor partitions based on ratio of ES-GS item sizes.
            if (gsOnChipLdsSize > maxLdsSize)
            {
                // Our target GS primitives per sub-group was too large

                // Calculate the maximum number of GS primitives per sub-group that will fit into LDS, capped
                // by the maximum that the hardware can support.
                uint32_t availableLdsSize   = maxLdsSize - esGsExtraLdsDwords;
                gsPrimsPerSubgroup          = std::min((availableLdsSize / (esGsRingItemSize * esMinVertsPerSubgroup)),
                                                       maxGsPrimsPerSubgroup);
                worstCaseEsVertsPerSubgroup = std::min(esMinVertsPerSubgroup * gsPrimsPerSubgroup * reuseOffMultiplier,
                                                       maxEsVertsPerSubgroup);

                LLPC_ASSERT(gsPrimsPerSubgroup > 0);

                esGsLdsSize     = (esGsRingItemSize * worstCaseEsVertsPerSubgroup);
                gsOnChipLdsSize = alignTo(esGsLdsSize + esGsExtraLdsDwords, ldsSizeDwordGranularity);

                LLPC_ASSERT(gsOnChipLdsSize <= maxLdsSize);
            }

            if (hasTs || DisableGsOnChip)
            {
                gsOnChip = false;
            }
            else
            {
                // Now let's calculate the onchip GSVS info and determine if it should be on or off chip.
                uint32_t gsVsItemSize = gsVsRingItemSizeOnChip * gsInstanceCount;

                // Compute GSVS LDS size based on target GS prims per subgroup.
                uint32_t gsVsLdsSize = gsVsItemSize * gsPrimsPerSubgroup;

                // Start out with the assumption that our GS prims per subgroup won't change.
                uint32_t onchipGsPrimsPerSubgroup = gsPrimsPerSubgroup;

                // Total LDS use per subgroup aligned to the register granularity to keep ESGS and GSVS data on chip.
                uint32_t onchipEsGsVsLdsSize = alignTo(esGsLdsSize + gsVsLdsSize, ldsSizeDwordGranularity);
                uint32_t onchipEsGsLdsSizeOnchipGsVs = esGsLdsSize;

                if (onchipEsGsVsLdsSize > maxLdsSize)
                {
                    // TODO: This code only allocates the minimum required LDS to hit the on chip GS prims per subgroup
                    //       threshold. This leaves some LDS space unused. The extra space could potentially be used to
                    //       increase the GS Prims per subgroup.

                    // Set the threshold at the minimum to keep things on chip.
                    onchipGsPrimsPerSubgroup = maxGsPrimsPerSubgroup;

                    if (onchipGsPrimsPerSubgroup > 0)
                    {
                        worstCaseEsVertsPerSubgroup =
                            std::min(esMinVertsPerSubgroup * onchipGsPrimsPerSubgroup * reuseOffMultiplier,
                                     maxEsVertsPerSubgroup);

                        // Calculate the LDS sizes required to hit this threshold.
                        onchipEsGsLdsSizeOnchipGsVs = alignTo(esGsRingItemSize * worstCaseEsVertsPerSubgroup,
                                                                ldsSizeDwordGranularity);
                        gsVsLdsSize = gsVsItemSize * onchipGsPrimsPerSubgroup;
                        onchipEsGsVsLdsSize = onchipEsGsLdsSizeOnchipGsVs + gsVsLdsSize;

                        if (onchipEsGsVsLdsSize > maxLdsSize)
                        {
                            // LDS isn't big enough to hit the target GS prim per subgroup count for on chip GSVS.
                            gsOnChip = false;
                        }
                    }
                    else
                    {
                        // With high GS instance counts, it is possible that the number of on chip GS prims
                        // calculated is zero. If this is the case, we can't expect to use on chip GS.
                        gsOnChip = false;
                    }
                }

                // If on chip GSVS is optimal, update the ESGS parameters with any changes that allowed for GSVS data.
                if (gsOnChip)
                {
                    gsOnChipLdsSize    = onchipEsGsVsLdsSize;
                    esGsLdsSize        = onchipEsGsLdsSizeOnchipGsVs;
                    gsPrimsPerSubgroup = onchipGsPrimsPerSubgroup;
                }
            }

            uint32_t esVertsPerSubgroup =
                std::min(esGsLdsSize / (esGsRingItemSize * reuseOffMultiplier), maxEsVertsPerSubgroup);

            LLPC_ASSERT(esVertsPerSubgroup >= esMinVertsPerSubgroup);

            // Vertices for adjacency primitives are not always reused (e.g. in the case of shadow volumes). Acording
            // to hardware engineers, we must restore esMinVertsPerSubgroup for ES_VERTS_PER_SUBGRP.
            if (useAdjacency)
            {
                esMinVertsPerSubgroup = inVertsPerPrim;
            }

            // For normal primitives, the VGT only checks if they are past the ES verts per sub group after allocating
            // a full GS primitive and if they are, kick off a new sub group.  But if those additional ES verts are
            // unique (e.g. not reused) we need to make sure there is enough LDS space to account for those ES verts
            // beyond ES_VERTS_PER_SUBGRP.
            esVertsPerSubgroup -= (esMinVertsPerSubgroup - 1);

            pGsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup   = esVertsPerSubgroup;
            pGsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup   = gsPrimsPerSubgroup;
            pGsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize          = esGsLdsSize;
            pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize      = gsOnChipLdsSize;

            pGsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize     = esGsRingItemSize;
            pGsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize     = gsOnChip ?
                                                                         gsVsRingItemSizeOnChip :
                                                                         gsVsRingItemSize;

            if ((m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major == 10) && hasTs && (gsOnChip == false))
            {
                uint32_t esVertsNum = Gfx9::EsVertsOffchipGsOrTess;
                uint32_t onChipGsLdsMagicSize = alignTo((esVertsNum * esGsRingItemSize) + esGsExtraLdsDwords,
                            static_cast<uint32_t>((1 << m_pPipelineState->GetTargetInfo().GetGpuProperty().ldsSizeDwordGranularityShift)));

                // If the new size is greater than the size we previously set
                // then we need to either increase the size or decrease the verts
                if (onChipGsLdsMagicSize > gsOnChipLdsSize)
                {
                    if (onChipGsLdsMagicSize > maxLdsSize)
                    {
                        // Decrease the verts
                        esVertsNum = (maxLdsSize - esGsExtraLdsDwords) / esGsRingItemSize;
                        pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = maxLdsSize;
                    }
                    else
                    {
                        // Increase the size
                        pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = onChipGsLdsMagicSize;
                    }
                }
                // Support multiple GS instances
                uint32_t gsPrimsNum = Gfx9::GsPrimsOffchipGsOrTess / gsInstanceCount;

                pGsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsNum;
                pGsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsNum;
            }
        }
    }

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC geometry calculation factor results\n\n");
    LLPC_OUTS("ES vertices per sub-group: " << pGsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup << "\n");
    LLPC_OUTS("GS primitives per sub-group: " << pGsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup << "\n");
    LLPC_OUTS("\n");
    LLPC_OUTS("ES-GS LDS size: " << pGsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize << "\n");
    LLPC_OUTS("On-chip GS LDS size: " << pGsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize << "\n");
    LLPC_OUTS("\n");
    LLPC_OUTS("ES-GS ring item size: " << pGsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize << "\n");
    LLPC_OUTS("GS-VS ring item size: " << pGsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize << "\n");
    LLPC_OUTS("\n");

    LLPC_OUTS("GS stream item size:\n");
    for (uint32_t i = 0; i < MaxGsStreams; ++i)
    {
        uint32_t streamItemSize = pGsResUsage->inOutUsage.gs.outLocCount[i] *
                                    geometryMode.outputVertices * 4;
        LLPC_OUTS("    stream " << i << " = " << streamItemSize);

        if (pGsResUsage->inOutUsage.enableXfb)
        {
            LLPC_OUTS(", XFB buffer = ");
            for (uint32_t j = 0; j < MaxTransformFeedbackBuffers; ++j)
            {
                if ((pGsResUsage->inOutUsage.streamXfbBuffers[i] & (1 << j)) != 0)
                {
                    LLPC_OUTS(j);
                    if (j != MaxTransformFeedbackBuffers - 1)
                    {
                        LLPC_OUTS(", ");
                    }
                }
            }
        }

        LLPC_OUTS("\n");
    }
    LLPC_OUTS("\n");

    if (gsOnChip || (m_pPipelineState->GetTargetInfo().GetGfxIpVersion().major >= 9))
    {
        if (m_pPipelineState->GetNggControl()->enableNgg)
        {
            LLPC_OUTS("GS primitive amplification factor: "
                      << pGsResUsage->inOutUsage.gs.calcFactor.primAmpFactor
                      << "\n");
            LLPC_OUTS("GS enable max output vertices per instance: "
                      << (pGsResUsage->inOutUsage.gs.calcFactor.enableMaxVertOut ? "true" : "false")
                      << "\n");
            LLPC_OUTS("\n");

            LLPC_OUTS("GS is on-chip (NGG)\n");
        }
        else
        {
            LLPC_OUTS("GS is " << (gsOnChip ? "on-chip" : "off-chip") << "\n");
        }
    }
    else
    {
        LLPC_OUTS("GS is off-chip\n");
    }
    LLPC_OUTS("\n");

    return gsOnChip;
}

// =====================================================================================================================
// Gets the count of vertices per primitive
uint32_t PatchResourceCollect::GetVerticesPerPrimitive() const
{
    uint32_t vertsPerPrim = 1;

    switch (m_pPipelineState->GetInputAssemblyState().topology)
    {
    case PrimitiveTopology::PointList:
        vertsPerPrim = 1;
        break;
    case PrimitiveTopology::LineList:
        vertsPerPrim = 2;
        break;
    case PrimitiveTopology::LineStrip:
        vertsPerPrim = 2;
        break;
    case PrimitiveTopology::TriangleList:
        vertsPerPrim = 3;
        break;
    case PrimitiveTopology::TriangleStrip:
        vertsPerPrim = 3;
        break;
    case PrimitiveTopology::TriangleFan:
        vertsPerPrim = 3;
        break;
    case PrimitiveTopology::LineListWithAdjacency:
        vertsPerPrim = 4;
        break;
    case PrimitiveTopology::LineStripWithAdjacency:
        vertsPerPrim = 4;
        break;
    case PrimitiveTopology::TriangleListWithAdjacency:
        vertsPerPrim = 6;
        break;
    case PrimitiveTopology::TriangleStripWithAdjacency:
        vertsPerPrim = 6;
        break;
    case PrimitiveTopology::PatchList:
        vertsPerPrim = m_pPipelineState->GetInputAssemblyState().patchControlPoints;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return vertsPerPrim;
}

// =====================================================================================================================
// Process a single shader
void PatchResourceCollect::ProcessShader()
{
    m_hasPushConstOp = false;
    m_hasDynIndexedInput = false;
    m_hasDynIndexedOutput = false;
    m_pResUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage);

    // Invoke handling of "call" instruction
    visit(m_pEntryPoint);

    // Disable push constant if not used
    if (m_hasPushConstOp == false)
    {
        m_pResUsage->pushConstSizeInBytes = 0;
    }

    ClearInactiveInput();
    ClearInactiveOutput();

    if (m_pPipelineState->IsGraphics())
    {
        MatchGenericInOut();
        MapBuiltInToGenericInOut();
    }

    if (m_shaderStage == ShaderStageFragment)
    {
        if (m_pResUsage->builtInUsage.fs.fragCoord ||
            m_pResUsage->builtInUsage.fs.pointCoord ||
            m_pResUsage->builtInUsage.fs.sampleMaskIn)
        {
            if (m_pPipelineState->GetRasterizerState().perSampleShading)
            {
                m_pResUsage->builtInUsage.fs.runAtSampleRate = true;
            }
        }
    }
    else if (m_shaderStage == ShaderStageVertex)
    {
        // Collect resource usages from vertex input create info
        // TODO: In the future, we might check if the corresponding vertex attribute is active in vertex shader
        // and set the usage based on this info.
        for (const auto& vertexInput : m_pPipelineState->GetVertexInputDescriptions())
        {
            if (vertexInput.inputRate == VertexInputRateVertex)
            {
                m_pResUsage->builtInUsage.vs.vertexIndex = true;
                m_pResUsage->builtInUsage.vs.baseVertex = true;
            }
            else
            {
                // TODO: We probably don't need instanceIndex for VertexInputRateNone.
                m_pResUsage->builtInUsage.vs.instanceIndex = true;
                m_pResUsage->builtInUsage.vs.baseInstance = true;
            }
        }
    }

    // Remove dead calls
    for (auto pCall : m_deadCalls)
    {
        LLPC_ASSERT(pCall->user_empty());
        pCall->dropAllReferences();
        pCall->eraseFromParent();
    }
    m_deadCalls.clear();
}

// =====================================================================================================================
// Check whether vertex reuse should be disabled.
bool PatchResourceCollect::IsVertexReuseDisabled()
{
    const bool hasGs = m_pPipelineState->HasShaderStage(ShaderStageGeometry);
    const bool hasTs = (m_pPipelineState->HasShaderStage(ShaderStageTessControl) ||
                        m_pPipelineState->HasShaderStage(ShaderStageTessEval));
    const bool hasVs = m_pPipelineState->HasShaderStage(ShaderStageVertex);

    bool disableVertexReuse = m_pPipelineState->GetInputAssemblyState().disableVertexReuse;

    bool useViewportIndex = false;
    if (hasGs)
    {
        useViewportIndex = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs.viewportIndex;
    }
    else if (hasTs)
    {
        useViewportIndex = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval)->
                              builtInUsage.tes.viewportIndex;
    }
    else if (hasVs)
    {
        useViewportIndex = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.viewportIndex;
    }

    disableVertexReuse |= useViewportIndex;

    return disableVertexReuse;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchResourceCollect::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    bool isDeadCall = callInst.user_empty();

    auto mangledName = pCallee->getName();

    if (mangledName.startswith(LlpcName::PushConstLoad) ||
        mangledName.startswith(LlpcName::DescriptorLoadSpillTable))
    {
        // Push constant operations
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            m_hasPushConstOp = true;
        }
    }
    else if (mangledName.startswith(LlpcName::DescriptorLoadBuffer) ||
             mangledName.startswith(LlpcName::DescriptorGetTexelBufferPtr) ||
             mangledName.startswith(LlpcName::DescriptorGetResourcePtr) ||
             mangledName.startswith(LlpcName::DescriptorGetFmaskPtr) ||
             mangledName.startswith(LlpcName::DescriptorGetSamplerPtr))
    {
        uint32_t descSet = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        uint32_t binding = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
        DescriptorPair descPair = { descSet, binding };
        m_pResUsage->descPairs.insert(descPair.u64All);
    }
    else if (mangledName.startswith(LlpcName::BufferLoad))
    {
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
    }
    else if (mangledName.startswith(LlpcName::InputImportGeneric))
    {
        // Generic input import
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            auto pInputTy = callInst.getType();
            LLPC_ASSERT(pInputTy->isSingleValueType());

            auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

            if ((m_shaderStage == ShaderStageTessControl) || (m_shaderStage == ShaderStageTessEval))
            {
                auto pLocOffset = callInst.getOperand(1);
                auto pCompIdx = callInst.getOperand(2);

                if (isa<ConstantInt>(pLocOffset))
                {
                    // Location offset is constant
                    auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                    loc += locOffset;

                    auto bitWidth = pInputTy->getScalarSizeInBits();
                    if (bitWidth == 64)
                    {
                        if (isa<ConstantInt>(pCompIdx))
                        {
                            auto compIdx = cast<ConstantInt>(pCompIdx)->getZExtValue();

                            m_activeInputLocs.insert(loc);
                            if (compIdx >= 2)
                            {
                                // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
                                // occupied locations are two.
                                m_activeInputLocs.insert(loc + 1);
                            }
                        }
                        else
                        {
                            // NOTE: If vector component index is not constant, we treat this as dynamic indexing.
                            m_hasDynIndexedInput = true;
                        }
                    }
                    else
                    {
                        // NOTE: For non 64-bit vector/scalar, one location is sufficient regardless of vector component
                        // addressing.
                        LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
                        m_activeInputLocs.insert(loc);
                    }
                }
                else
                {
                    // NOTE: If location offset is not constant, we treat this as dynamic indexing.
                    m_hasDynIndexedInput = true;
                }
            }
            else
            {
                m_activeInputLocs.insert(loc);
                if (pInputTy->getPrimitiveSizeInBits() > (8 * SizeOfVec4))
                {
                    LLPC_ASSERT(pInputTy->getPrimitiveSizeInBits() <= (8 * 2 * SizeOfVec4));
                    m_activeInputLocs.insert(loc + 1);
                }
            }
        }
    }
    else if (mangledName.startswith(LlpcName::InputImportInterpolant))
    {
        // Interpolant input import
        LLPC_ASSERT(m_shaderStage == ShaderStageFragment);

        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            LLPC_ASSERT(callInst.getType()->isSingleValueType());

            auto pLocOffset = callInst.getOperand(1);
            if (isa<ConstantInt>(pLocOffset))
            {
                // Location offset is constant
                auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
                auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
                loc += locOffset;

                LLPC_ASSERT(callInst.getType()->getPrimitiveSizeInBits() <= (8 * SizeOfVec4));
                m_activeInputLocs.insert(loc);
            }
            else
            {
                // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
                m_hasDynIndexedInput = true;
            }
        }
    }
    else if (mangledName.startswith(LlpcName::InputImportBuiltIn))
    {
        // Built-in input import
        if (isDeadCall)
        {
            m_deadCalls.insert(&callInst);
        }
        else
        {
            uint32_t builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
            m_activeInputBuiltIns.insert(builtInId);
        }
    }
    else if (mangledName.startswith(LlpcName::OutputImportGeneric))
    {
        // Generic output import
        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

        auto pOutputTy = callInst.getType();
        LLPC_ASSERT(pOutputTy->isSingleValueType());

        auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        auto pLocOffset = callInst.getOperand(1);
        auto pCompIdx = callInst.getOperand(2);

        if (isa<ConstantInt>(pLocOffset))
        {
            // Location offset is constant
            auto locOffset = cast<ConstantInt>(pLocOffset)->getZExtValue();
            loc += locOffset;

            auto bitWidth = pOutputTy->getScalarSizeInBits();
            if (bitWidth == 64)
            {
                if (isa<ConstantInt>(pCompIdx))
                {
                    auto compIdx = cast<ConstantInt>(pCompIdx)->getZExtValue();

                    m_importedOutputLocs.insert(loc);
                    if (compIdx >= 2)
                    {
                        // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
                        // occupied locations are two.
                        m_importedOutputLocs.insert(loc + 1);
                    }
                }
                else
                {
                    // NOTE: If vector component index is not constant, we treat this as dynamic indexing.
                    m_hasDynIndexedOutput = true;
                }
            }
            else
            {
                // NOTE: For non 64-bit vector/scalar, one location is sufficient regardless of vector component
                // addressing.
                LLPC_ASSERT((bitWidth == 8) || (bitWidth == 16) || (bitWidth == 32));
                m_importedOutputLocs.insert(loc);
            }
        }
        else
        {
            // NOTE: If location offset is not constant, we treat this as dynamic indexing.
            m_hasDynIndexedOutput = true;
        }
    }
    else if (mangledName.startswith(LlpcName::OutputImportBuiltIn))
    {
        // Built-in output import
        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);

        uint32_t builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        m_importedOutputBuiltIns.insert(builtInId);
    }
    else if (mangledName.startswith(LlpcName::OutputExportGeneric))
    {
        // Generic output export
        if (m_shaderStage == ShaderStageTessControl)
        {
            auto pOutput = callInst.getOperand(callInst.getNumArgOperands() - 1);
            auto pOutputTy = pOutput->getType();
            LLPC_ASSERT(pOutputTy->isSingleValueType());

            auto pLocOffset = callInst.getOperand(1);
            auto pCompIdx = callInst.getOperand(2);

            if (isa<ConstantInt>(pLocOffset))
            {
                // Location offset is constant
                auto bitWidth = pOutputTy->getScalarSizeInBits();
                if ((bitWidth == 64) && (isa<ConstantInt>(pCompIdx) == false))
                {
                    // NOTE: If vector component index is not constant and it is vector component addressing for
                    // 64-bit vector, we treat this as dynamic indexing.
                    m_hasDynIndexedOutput = true;
                }
            }
            else
            {
                // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
                m_hasDynIndexedOutput = true;
            }
        }
    }
    else if (mangledName.startswith(LlpcName::OutputExportBuiltIn))
    {
        // NOTE: If output value is undefined one, we can safely drop it and remove the output export call.
        // Currently, do this for geometry shader.
        if (m_shaderStage == ShaderStageGeometry)
        {
            auto* pOutputValue = callInst.getArgOperand(callInst.getNumArgOperands() - 1);
            if (isa<UndefValue>(pOutputValue))
            {
                m_deadCalls.insert(&callInst);
            }
            else
            {
                uint32_t builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
                m_activeOutputBuiltIns.insert(builtInId);
            }
        }
    }

    if (CanPackInOut())
    {
        if ((m_shaderStage == ShaderStageFragment) && (isDeadCall == false))
        {
            // Collect LocationSpans according to each FS' input call
            bool isInput = m_pLocationMapManager->AddSpan(&callInst);
            if (isInput)
            {
                m_inOutCalls.push_back(&callInst);
                m_deadCalls.insert(&callInst);
            }
        }
        else if ((m_shaderStage == ShaderStageVertex) && mangledName.startswith(LlpcName::OutputExportGeneric))
        {
            m_inOutCalls.push_back(&callInst);
            m_deadCalls.insert(&callInst);
        }
    }
}

// =====================================================================================================================
// Clears inactive (those actually unused) inputs.
void PatchResourceCollect::ClearInactiveInput()
{
    bool buildingRelocatableElf = m_pPipelineState->GetBuilderContext()->BuildingRelocatableElf();
    // Clear those inactive generic inputs, remove them from location mappings
    if (m_pPipelineState->IsGraphics() && (m_hasDynIndexedInput == false) && (m_shaderStage != ShaderStageTessEval)
        && !buildingRelocatableElf)
    {
        // TODO: Here, we keep all generic inputs of tessellation evaluation shader. This is because corresponding
        // generic outputs of tessellation control shader might involve in output import and dynamic indexing, which
        // is easy to cause incorrectness of location mapping.

        // Clear normal inputs
        std::unordered_set<uint32_t> unusedLocs;
        for (auto locMap : m_pResUsage->inOutUsage.inputLocMap)
        {
            uint32_t loc = locMap.first;
            if (m_activeInputLocs.find(loc) == m_activeInputLocs.end())
            {
                 unusedLocs.insert(loc);
            }
        }

        for (auto loc : unusedLocs)
        {
            m_pResUsage->inOutUsage.inputLocMap.erase(loc);
        }

        // Clear per-patch inputs
        if (m_shaderStage == ShaderStageTessEval)
        {
            unusedLocs.clear();
            for (auto locMap : m_pResUsage->inOutUsage.perPatchInputLocMap)
            {
                uint32_t loc = locMap.first;
                if (m_activeInputLocs.find(loc) == m_activeInputLocs.end())
                {
                     unusedLocs.insert(loc);
                }
            }

            for (auto loc : unusedLocs)
            {
                m_pResUsage->inOutUsage.perPatchInputLocMap.erase(loc);
            }
        }
        else
        {
            // For other stages, must be empty
            LLPC_ASSERT(m_pResUsage->inOutUsage.perPatchInputLocMap.empty());
        }
    }

    // Clear those inactive built-in inputs (some are not checked, whose usage flags do not rely on their
    // actual uses)
    auto& builtInUsage = m_pResUsage->builtInUsage;

    // Check per-stage built-in usage
    if (m_shaderStage == ShaderStageVertex)
    {
        if (builtInUsage.vs.drawIndex &&
            (m_activeInputBuiltIns.find(BuiltInDrawIndex) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.vs.drawIndex = false;
        }
    }
    else if (m_shaderStage == ShaderStageTessControl)
    {
        if (builtInUsage.tcs.pointSizeIn &&
            (m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.pointSizeIn = false;
        }

        if (builtInUsage.tcs.positionIn &&
            (m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.positionIn = false;
        }

        if ((builtInUsage.tcs.clipDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.clipDistanceIn = 0;
        }

        if ((builtInUsage.tcs.cullDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.cullDistanceIn = 0;
        }

        if (builtInUsage.tcs.patchVertices &&
            (m_activeInputBuiltIns.find(BuiltInPatchVertices) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.patchVertices = false;
        }

        if (builtInUsage.tcs.primitiveId &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.primitiveId = false;
        }

        if (builtInUsage.tcs.invocationId &&
            (m_activeInputBuiltIns.find(BuiltInInvocationId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tcs.invocationId = false;
        }
    }
    else if (m_shaderStage == ShaderStageTessEval)
    {
        if (builtInUsage.tes.pointSizeIn &&
            (m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.pointSizeIn = false;
        }

        if (builtInUsage.tes.positionIn &&
            (m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.positionIn = false;
        }

        if ((builtInUsage.tes.clipDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.clipDistanceIn = 0;
        }

        if ((builtInUsage.tes.cullDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.cullDistanceIn = 0;
        }

        if (builtInUsage.tes.patchVertices &&
            (m_activeInputBuiltIns.find(BuiltInPatchVertices) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.patchVertices = false;
        }

        if (builtInUsage.tes.primitiveId &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.primitiveId = false;
        }

        if (builtInUsage.tes.tessCoord &&
            (m_activeInputBuiltIns.find(BuiltInTessCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.tessCoord = false;
        }

        if (builtInUsage.tes.tessLevelOuter &&
            (m_activeInputBuiltIns.find(BuiltInTessLevelOuter) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.tessLevelOuter = false;
        }

        if (builtInUsage.tes.tessLevelInner &&
            (m_activeInputBuiltIns.find(BuiltInTessLevelInner) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.tes.tessLevelInner = false;
        }
    }
    else if (m_shaderStage == ShaderStageGeometry)
    {
        if (builtInUsage.gs.pointSizeIn &&
            (m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.pointSizeIn = false;
        }

        if (builtInUsage.gs.positionIn &&
            (m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.positionIn = false;
        }

        if ((builtInUsage.gs.clipDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.clipDistanceIn = 0;
        }

        if ((builtInUsage.gs.cullDistanceIn > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.cullDistanceIn = 0;
        }

        if (builtInUsage.gs.primitiveIdIn &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.primitiveIdIn = false;
        }

        if (builtInUsage.gs.invocationId &&
            (m_activeInputBuiltIns.find(BuiltInInvocationId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.gs.invocationId = false;
        }
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        if (builtInUsage.fs.fragCoord &&
            (m_activeInputBuiltIns.find(BuiltInFragCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.fragCoord = false;
        }

        if (builtInUsage.fs.frontFacing &&
            (m_activeInputBuiltIns.find(BuiltInFrontFacing) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.frontFacing = false;
        }

        if (builtInUsage.fs.fragCoord &&
            (m_activeInputBuiltIns.find(BuiltInFragCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.fragCoord = false;
        }

        if ((builtInUsage.fs.clipDistance > 0) &&
            (m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.clipDistance = 0;
        }

        if ((builtInUsage.fs.cullDistance > 0) &&
            (m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.cullDistance = 0;
        }

        if (builtInUsage.fs.pointCoord &&
            (m_activeInputBuiltIns.find(BuiltInPointCoord) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.pointCoord = false;
        }

        if (builtInUsage.fs.primitiveId &&
            (m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.primitiveId = false;
        }

        if (builtInUsage.fs.sampleId &&
            (m_activeInputBuiltIns.find(BuiltInSampleId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.sampleId = false;
        }

        if (builtInUsage.fs.samplePosition &&
            (m_activeInputBuiltIns.find(BuiltInSamplePosition) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.samplePosition = false;
        }

        if (builtInUsage.fs.sampleMaskIn &&
            (m_activeInputBuiltIns.find(BuiltInSampleMask) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.sampleMaskIn = false;
        }

        if (builtInUsage.fs.layer &&
            (m_activeInputBuiltIns.find(BuiltInLayer) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.layer = false;
        }

        if (builtInUsage.fs.viewIndex &&
            (m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.viewIndex = false;
        }

        if (builtInUsage.fs.viewportIndex &&
            (m_activeInputBuiltIns.find(BuiltInViewportIndex) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.viewportIndex = false;
        }

        if (builtInUsage.fs.helperInvocation &&
            (m_activeInputBuiltIns.find(BuiltInHelperInvocation) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.helperInvocation = false;
        }

        if (builtInUsage.fs.baryCoordNoPersp &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordNoPersp) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPersp = false;
        }

        if (builtInUsage.fs.baryCoordNoPerspCentroid &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspCentroid) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPerspCentroid = false;
        }

        if (builtInUsage.fs.baryCoordNoPerspSample &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspSample) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPerspSample = false;
        }

        if (builtInUsage.fs.baryCoordSmooth &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordSmooth) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordSmooth = false;
        }

        if (builtInUsage.fs.baryCoordSmoothCentroid &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothCentroid) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordSmoothCentroid = false;
        }

        if (builtInUsage.fs.baryCoordSmoothSample &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothSample) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordNoPerspSample = false;
        }

        if (builtInUsage.fs.baryCoordPullModel &&
            (m_activeInputBuiltIns.find(BuiltInBaryCoordPullModel) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.fs.baryCoordPullModel = false;
        }
    }
    else if (m_shaderStage == ShaderStageCompute)
    {
        if (builtInUsage.cs.numWorkgroups &&
            (m_activeInputBuiltIns.find(BuiltInNumWorkgroups) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.cs.numWorkgroups = false;
        }

        if (builtInUsage.cs.localInvocationId &&
            ((m_activeInputBuiltIns.find(BuiltInLocalInvocationId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInGlobalInvocationId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInLocalInvocationIndex) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())))
        {
            builtInUsage.cs.localInvocationId = false;
        }

        if (builtInUsage.cs.workgroupId &&
            ((m_activeInputBuiltIns.find(BuiltInWorkgroupId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInGlobalInvocationId) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInLocalInvocationIndex) == m_activeInputBuiltIns.end()) &&
                (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())))
        {
            builtInUsage.cs.workgroupId = false;
        }

        if (builtInUsage.cs.subgroupId &&
            (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.cs.subgroupId = false;
        }

        if (builtInUsage.cs.numSubgroups &&
            (m_activeInputBuiltIns.find(BuiltInNumSubgroups) == m_activeInputBuiltIns.end()))
        {
            builtInUsage.cs.numSubgroups = false;
        }
    }

    // Check common built-in usage
    if (builtInUsage.common.subgroupSize &&
        ((m_activeInputBuiltIns.find(BuiltInSubgroupSize) == m_activeInputBuiltIns.end()) &&
            (m_activeInputBuiltIns.find(BuiltInNumSubgroups) == m_activeInputBuiltIns.end()) &&
            (m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())))
    {
        builtInUsage.common.subgroupSize = false;
    }

    if (builtInUsage.common.subgroupLocalInvocationId &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupLocalInvocationId) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupLocalInvocationId = false;
    }

    if (builtInUsage.common.subgroupEqMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupEqMask) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupEqMask = false;
    }

    if (builtInUsage.common.subgroupGeMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupGeMask) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupGeMask = false;
    }

    if (builtInUsage.common.subgroupGtMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupGtMask) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupGtMask = false;
    }

    if (builtInUsage.common.subgroupLeMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupLeMask) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupLeMask = false;
    }

    if (builtInUsage.common.subgroupLtMask &&
        (m_activeInputBuiltIns.find(BuiltInSubgroupLtMask) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.subgroupLtMask = false;
    }

    if (builtInUsage.common.deviceIndex &&
        (m_activeInputBuiltIns.find(BuiltInDeviceIndex) == m_activeInputBuiltIns.end()))
    {
        builtInUsage.common.deviceIndex = false;
    }
}

// =====================================================================================================================
// Clears inactive (those actually unused) outputs.
void PatchResourceCollect::ClearInactiveOutput()
{
    // Clear inactive output builtins
    if (m_shaderStage == ShaderStageGeometry)
    {
        auto& builtInUsage = m_pResUsage->builtInUsage.gs;

        if (builtInUsage.position &&
            (m_activeOutputBuiltIns.find(BuiltInPosition) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.position = false;
        }

        if (builtInUsage.pointSize &&
            (m_activeOutputBuiltIns.find(BuiltInPointSize) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.pointSize = false;
        }

        if (builtInUsage.clipDistance &&
            (m_activeOutputBuiltIns.find(BuiltInClipDistance) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.clipDistance = false;
        }

        if (builtInUsage.cullDistance &&
            (m_activeOutputBuiltIns.find(BuiltInCullDistance) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.cullDistance = false;
        }

        if (builtInUsage.primitiveId &&
            (m_activeOutputBuiltIns.find(BuiltInPrimitiveId) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.primitiveId = false;
        }

        if (builtInUsage.layer &&
            (m_activeOutputBuiltIns.find(BuiltInLayer) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.layer = false;
        }

        if (builtInUsage.viewportIndex &&
            (m_activeOutputBuiltIns.find(BuiltInViewportIndex) == m_activeOutputBuiltIns.end()))
        {
            builtInUsage.viewportIndex = false;
        }
    }
}

// =====================================================================================================================
// Does generic input/output matching and does location mapping afterwards.
//
// NOTE: This function should be called after the cleanup work of inactive inputs is done.
void PatchResourceCollect::MatchGenericInOut()
{
    LLPC_ASSERT(m_pPipelineState->IsGraphics());
    auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage;

    auto& inLocMap  = inOutUsage.inputLocMap;
    auto& outLocMap = inOutUsage.outputLocMap;

    auto& perPatchInLocMap  = inOutUsage.perPatchInputLocMap;
    auto& perPatchOutLocMap = inOutUsage.perPatchOutputLocMap;

    // Do input/output matching
    if (!m_pPipelineState->GetBuilderContext()->BuildingRelocatableElf() && m_shaderStage != ShaderStageFragment)
    {
        const auto nextStage = m_pPipelineState->GetNextShaderStage(m_shaderStage);

        // Do normal input/output matching
        if (nextStage != ShaderStageInvalid)
        {
            const auto pNextResUsage = m_pPipelineState->GetShaderResourceUsage(nextStage);
            const auto& nextInLocMap = pNextResUsage->inOutUsage.inputLocMap;

            uint32_t availInMapLoc = pNextResUsage->inOutUsage.inputMapLocCount;

            // Collect locations of those outputs that are not used by next shader stage
            std::vector<uint32_t> unusedLocs;
            for (auto& locMap : outLocMap)
            {
                uint32_t loc = locMap.first;
                bool outputXfb = false;
                if (m_shaderStage == ShaderStageGeometry)
                {
                    uint32_t outLocInfo = locMap.first;
                    loc = reinterpret_cast<GsOutLocInfo*>(&outLocInfo)->location;
                    outputXfb = inOutUsage.gs.xfbOutsInfo.find(outLocInfo) != inOutUsage.gs.xfbOutsInfo.end();
                }

                if ((nextInLocMap.find(loc) == nextInLocMap.end()) && (outputXfb == false))
                {
                    if (m_hasDynIndexedOutput || (m_importedOutputLocs.find(loc) != m_importedOutputLocs.end()))
                    {
                        // NOTE: If either dynamic indexing of generic outputs exists or the generic output involve in
                        // output import, we have to mark it as active. The assigned location must not overlap with
                        // those used by inputs of next shader stage.
                        LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
                        locMap.second = availInMapLoc++;
                    }
                    else
                    {
                        unusedLocs.push_back(loc);
                    }
                }
            }

            // Remove those collected locations
            for (auto loc : unusedLocs)
            {
                outLocMap.erase(loc);
            }
        }

        // Do per-patch input/output matching
        if (m_shaderStage == ShaderStageTessControl)
        {
            if (nextStage != ShaderStageInvalid)
            {
                const auto pNextResUsage = m_pPipelineState->GetShaderResourceUsage(nextStage);
                const auto& nextPerPatchInLocMap = pNextResUsage->inOutUsage.perPatchInputLocMap;

                uint32_t availPerPatchInMapLoc = pNextResUsage->inOutUsage.perPatchInputMapLocCount;

                // Collect locations of those outputs that are not used by next shader stage
                std::vector<uint32_t> unusedLocs;
                for (auto& locMap : perPatchOutLocMap)
                {
                    const uint32_t loc = locMap.first;
                    if (nextPerPatchInLocMap.find(loc) == nextPerPatchInLocMap.end())
                    {
                        // NOTE: If either dynamic indexing of generic outputs exists or the generic output involve in
                        // output import, we have to mark it as active. The assigned location must not overlap with
                        // those used by inputs of next shader stage.
                        if (m_hasDynIndexedOutput || (m_importedOutputLocs.find(loc) != m_importedOutputLocs.end()))
                        {
                            LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
                            locMap.second = availPerPatchInMapLoc++;
                        }
                        else
                        {
                            unusedLocs.push_back(loc);
                        }
                    }
                }

                // Remove those collected locations
                for (auto loc : unusedLocs)
                {
                    perPatchOutLocMap.erase(loc);
                }
            }
        }
        else
        {
            // For other stages, must be empty
            LLPC_ASSERT(perPatchOutLocMap.empty());
        }
    }

    if (CanPackInOut())
    {
        // Do packing input/output
        PackInOutLocation();
    }

    // Do location mapping
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC location input/output mapping results (" << GetShaderStageName(m_shaderStage)
              << " shader)\n\n");
    uint32_t nextMapLoc = 0;
    if (inLocMap.empty() == false)
    {
        LLPC_ASSERT(inOutUsage.inputMapLocCount == 0);
        for (auto& locMap : inLocMap)
        {
            LLPC_ASSERT(locMap.second == InvalidValue ||
                        m_pPipelineState->GetBuilderContext()->BuildingRelocatableElf());
            // NOTE: For vertex shader, the input location mapping is actually trivial.
            locMap.second = (m_shaderStage == ShaderStageVertex) ? locMap.first : nextMapLoc++;
            inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, locMap.second + 1);
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  loc = "
                          << locMap.first << "  =>  Mapped = " << locMap.second << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (outLocMap.empty() == false)
    {
        auto& outOrigLocs = inOutUsage.fs.outputOrigLocs;
        if (m_shaderStage == ShaderStageFragment)
        {
            memset(&outOrigLocs, InvalidValue, sizeof(inOutUsage.fs.outputOrigLocs));
        }

        nextMapLoc = 0;
        LLPC_ASSERT(inOutUsage.outputMapLocCount == 0);
        for (auto locMapIt = outLocMap.begin(); locMapIt != outLocMap.end();)
        {
            auto& locMap = *locMapIt;
            if (m_shaderStage == ShaderStageFragment)
            {
                uint32_t location = locMap.first;
                if (m_pPipelineState->GetColorExportState().dualSourceBlendEnable && (location == 1))
                {
                    location = 0;
                }
                if (m_pPipelineState->GetColorExportFormat(location).dfmt == BufDataFormatInvalid)
                {
                    locMapIt = outLocMap.erase(locMapIt);
                    continue;
                }
            }

            if (m_shaderStage == ShaderStageGeometry)
            {
                if (locMap.second == InvalidValue)
                {
                    uint32_t outLocInfo = locMap.first;
                    MapGsGenericOutput(*(reinterpret_cast<GsOutLocInfo*>(&outLocInfo)));
                }
            }
            else
            {
                if (locMap.second == InvalidValue)
                {
                    // Only do location mapping if the output has not been mapped
                    locMap.second = nextMapLoc++;
                }
                else
                {
                    LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
                }
                inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, locMap.second + 1);
                LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output: loc = "
                    << locMap.first << "  =>  Mapped = " << locMap.second << "\n");

                if (m_shaderStage == ShaderStageFragment)
                {
                    outOrigLocs[locMap.second] = locMap.first;
                }
            }

            ++locMapIt;
        }
        LLPC_OUTS("\n");
    }

    if (perPatchInLocMap.empty() == false)
    {
        nextMapLoc = 0;
        LLPC_ASSERT(inOutUsage.perPatchInputMapLocCount == 0);
        for (auto& locMap : perPatchInLocMap)
        {
            LLPC_ASSERT(locMap.second == InvalidValue);
            locMap.second = nextMapLoc++;
            inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, locMap.second + 1);
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  loc = "
                          << locMap.first << "  =>  Mapped = " << locMap.second << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (perPatchOutLocMap.empty() == false)
    {
        nextMapLoc = 0;
        LLPC_ASSERT(inOutUsage.perPatchOutputMapLocCount == 0);
        for (auto& locMap : perPatchOutLocMap)
        {
            if (locMap.second == InvalidValue)
            {
                // Only do location mapping if the per-patch output has not been mapped
                locMap.second = nextMapLoc++;
            }
            else
            {
                LLPC_ASSERT(m_shaderStage == ShaderStageTessControl);
            }
            inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, locMap.second + 1);
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): loc = "
                          << locMap.first << "  =>  Mapped = " << locMap.second << "\n");
        }
        LLPC_OUTS("\n");
    }

    LLPC_OUTS("// LLPC location count results (after input/output matching) \n\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  loc count = "
                  << inOutUsage.inputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output: loc count = "
                  << inOutUsage.outputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  loc count = "
                  << inOutUsage.perPatchInputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): loc count = "
                  << inOutUsage.perPatchOutputMapLocCount << "\n");
    LLPC_OUTS("\n");
}

// =====================================================================================================================
// Maps special built-in input/output to generic ones.
//
// NOTE: This function should be called after generic input/output matching is done.
void PatchResourceCollect::MapBuiltInToGenericInOut()
{
    LLPC_ASSERT(m_pPipelineState->IsGraphics());

    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage);

    auto& builtInUsage = pResUsage->builtInUsage;
    auto& inOutUsage = pResUsage->inOutUsage;

    const auto nextStage = m_pPipelineState->GetNextShaderStage(m_shaderStage);
    auto pNextResUsage =
        (nextStage != ShaderStageInvalid) ? m_pPipelineState->GetShaderResourceUsage(nextStage) : nullptr;

    LLPC_ASSERT(inOutUsage.builtInInputLocMap.empty()); // Should be empty
    LLPC_ASSERT(inOutUsage.builtInOutputLocMap.empty());

    // NOTE: The rules of mapping built-ins to generic inputs/outputs are as follow:
    //       (1) For built-in outputs, if next shader stager is valid and has corresponding built-in input used,
    //           get the mapped location from next shader stage inout usage and use it. If next shader stage
    //           is absent or it does not have such input used, we allocate the mapped location.
    //       (2) For built-on inputs, we always allocate the mapped location based its actual usage.
    if (m_shaderStage == ShaderStageVertex)
    {
        // VS  ==>  XXX
        uint32_t availOutMapLoc = inOutUsage.outputMapLocCount;

        // Map built-in outputs to generic ones
        if (nextStage == ShaderStageFragment)
        {
            // VS  ==>  FS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.clipDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
            }

            if (nextBuiltInUsage.primitiveId)
            {
                // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
                builtInUsage.vs.primitiveId = true;

                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
                inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
            }

            if (nextBuiltInUsage.layer)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
            }

            if (nextBuiltInUsage.viewIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                    nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
            }

            if (nextBuiltInUsage.viewportIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
            }
        }
        else if (nextStage == ShaderStageTessControl)
        {
            // VS  ==>  TCS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.tcs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.position = false;
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.pointSize = false;
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.clipDistance = 0;
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.cullDistance = 0;
            }

            builtInUsage.vs.layer = false;
            builtInUsage.vs.viewportIndex = false;
        }
        else if (nextStage == ShaderStageGeometry)
        {
            // VS  ==>  GS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.gs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.position = false;
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.vs.pointSize = false;
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.clipDistance = 0;
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.vs.cullDistance = 0;
            }

            builtInUsage.vs.layer = false;
            builtInUsage.vs.viewportIndex = false;
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // VS only
            if ((builtInUsage.vs.clipDistance > 0) || (builtInUsage.vs.cullDistance > 0))
            {
                uint32_t mapLoc = availOutMapLoc++;
                if (builtInUsage.vs.clipDistance + builtInUsage.vs.cullDistance > 4)
                {
                    LLPC_ASSERT(builtInUsage.vs.clipDistance +
                                builtInUsage.vs.cullDistance <= MaxClipCullDistanceCount);
                    ++availOutMapLoc; // Occupy two locations
                }

                if (builtInUsage.vs.clipDistance > 0)
                {
                    inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                }

                if (builtInUsage.vs.cullDistance > 0)
                {
                    if (builtInUsage.vs.clipDistance >= 4)
                    {
                        ++mapLoc;
                    }
                    inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                }
            }

            if (builtInUsage.vs.viewportIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;
            }

            if (builtInUsage.vs.layer)
            {
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;
            }

            if (builtInUsage.vs.viewIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
            }
        }

        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
    }
    else if (m_shaderStage == ShaderStageTessControl)
    {
        // TCS  ==>  XXX
        uint32_t availInMapLoc = inOutUsage.inputMapLocCount;
        uint32_t availOutMapLoc = inOutUsage.outputMapLocCount;

        uint32_t availPerPatchOutMapLoc = inOutUsage.perPatchOutputMapLocCount;

        // Map built-in inputs to generic ones
        if (builtInUsage.tcs.positionIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;
        }

        if (builtInUsage.tcs.pointSizeIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;
        }

        if (builtInUsage.tcs.clipDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
            if (builtInUsage.tcs.clipDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.tcs.cullDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
            if (builtInUsage.tcs.cullDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        // Map built-in outputs to generic ones
        if (nextStage == ShaderStageTessEval)
        {
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.tes;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            // NOTE: For tessellation control shadder, those built-in outputs that involve in output import have to
            // be mapped to generic ones even if they do not have corresponding built-in inputs used in next shader
            // stage.
            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInPosition) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInPosition] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.position = false;
                }
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInPointSize) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInPointSize] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.pointSize = false;
                }
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInClipDistance) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.clipDistance = 0;
                }
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                if (m_importedOutputBuiltIns.find(BuiltInCullDistance) != m_importedOutputBuiltIns.end())
                {
                    inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = InvalidValue;
                }
                else
                {
                    builtInUsage.tcs.cullDistance = 0;
                }
            }

            if (nextBuiltInUsage.tessLevelOuter)
            {
                LLPC_ASSERT(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelOuter) !=
                            nextInOutUsage.perPatchBuiltInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter];
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = mapLoc;
                availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
            }
            else
            {
                // NOTE: We have to map gl_TessLevelOuter to generic per-patch output as long as it is used.
                if (builtInUsage.tcs.tessLevelOuter)
                {
                    inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = InvalidValue;
                }
            }

            if (nextBuiltInUsage.tessLevelInner)
            {
                LLPC_ASSERT(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelInner) !=
                            nextInOutUsage.perPatchBuiltInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner];
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = mapLoc;
                availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
            }
            else
            {
                // NOTE: We have to map gl_TessLevelInner to generic per-patch output as long as it is used.
                if (builtInUsage.tcs.tessLevelInner)
                {
                    inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = InvalidValue;
                }
            }

            // Revisit built-in outputs and map those unmapped to generic ones
            if ((inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInPosition] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;
            }

            if ((inOutUsage.builtInOutputLocMap.find(BuiltInPointSize) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInPointSize] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;
            }

            if ((inOutUsage.builtInOutputLocMap.find(BuiltInClipDistance) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInClipDistance] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;
            }

            if ((inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end()) &&
                (inOutUsage.builtInOutputLocMap[BuiltInCullDistance] == InvalidValue))
            {
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;
            }

            if ((inOutUsage.perPatchBuiltInOutputLocMap.find(BuiltInTessLevelOuter) !=
                 inOutUsage.perPatchBuiltInOutputLocMap.end()) &&
                (inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] == InvalidValue))
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = availPerPatchOutMapLoc++;
            }

            if ((inOutUsage.perPatchBuiltInOutputLocMap.find(BuiltInTessLevelInner) !=
                 inOutUsage.perPatchBuiltInOutputLocMap.end()) &&
                (inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] == InvalidValue))
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = availPerPatchOutMapLoc++;
            }
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // TCS only
            if (builtInUsage.tcs.position)
            {
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;
            }

            if (builtInUsage.tcs.pointSize)
            {
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;
            }

            if (builtInUsage.tcs.clipDistance > 0)
            {
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;
                if (builtInUsage.tcs.clipDistance > 4)
                {
                    ++availOutMapLoc;
                }
            }

            if (builtInUsage.tcs.cullDistance > 0)
            {
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;
                if (builtInUsage.tcs.cullDistance > 4)
                {
                    ++availOutMapLoc;
                }
            }

            if (builtInUsage.tcs.tessLevelOuter)
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = availPerPatchOutMapLoc++;
            }

            if (builtInUsage.tcs.tessLevelInner)
            {
                inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = availPerPatchOutMapLoc++;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
        inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, availPerPatchOutMapLoc);
    }
    else if (m_shaderStage == ShaderStageTessEval)
    {
        // TES  ==>  XXX
        uint32_t availInMapLoc = inOutUsage.inputMapLocCount;
        uint32_t availOutMapLoc = inOutUsage.outputMapLocCount;

        uint32_t availPerPatchInMapLoc = inOutUsage.perPatchInputMapLocCount;

        // Map built-in inputs to generic ones
        if (builtInUsage.tes.positionIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;
        }

        if (builtInUsage.tes.pointSizeIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;
        }

        if (builtInUsage.tes.clipDistanceIn > 0)
        {
            uint32_t clipDistanceCount = builtInUsage.tes.clipDistanceIn;

            // NOTE: If gl_in[].gl_ClipDistance is used, we have to check the usage of gl_out[].gl_ClipDistance in
            // tessellation control shader. The clip distance is the maximum of the two. We do this to avoid
            // incorrectness of location assignment during builtin-to-generic mapping.
            const auto prevStage = m_pPipelineState->GetPrevShaderStage(m_shaderStage);
            if (prevStage == ShaderStageTessControl)
            {
                const auto& prevBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(prevStage)->builtInUsage.tcs;
                clipDistanceCount = std::max(clipDistanceCount, prevBuiltInUsage.clipDistance);
            }

            inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
            if (clipDistanceCount > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.tes.cullDistanceIn > 0)
        {
            uint32_t cullDistanceCount = builtInUsage.tes.cullDistanceIn;

            const auto prevStage = m_pPipelineState->GetPrevShaderStage(m_shaderStage);
            if (prevStage == ShaderStageTessControl)
            {
                const auto& prevBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(prevStage)->builtInUsage.tcs;
                cullDistanceCount = std::max(cullDistanceCount, prevBuiltInUsage.clipDistance);
            }

            inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
            if (cullDistanceCount > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.tes.tessLevelOuter)
        {
            inOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter] = availPerPatchInMapLoc++;
        }

        if (builtInUsage.tes.tessLevelInner)
        {
            inOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner] = availPerPatchInMapLoc++;
        }

        // Map built-in outputs to generic ones
        if (nextStage == ShaderStageFragment)
        {
            // TES  ==>  FS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.clipDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
            }

            if (nextBuiltInUsage.primitiveId)
            {
                // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
                builtInUsage.tes.primitiveId = true;

                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
                inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
            }

            if (nextBuiltInUsage.layer)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
            }

            if (nextBuiltInUsage.viewIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                    nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
            }

            if (nextBuiltInUsage.viewportIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
            }
        }
        else if (nextStage == ShaderStageGeometry)
        {
            // TES  ==>  GS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.gs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.positionIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
                inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.tes.position = false;
            }

            if (nextBuiltInUsage.pointSizeIn)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
                inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
            }
            else
            {
                builtInUsage.tes.pointSize = false;
            }

            if (nextBuiltInUsage.clipDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.clipDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.tes.clipDistance = 0;
            }

            if (nextBuiltInUsage.cullDistanceIn > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                availOutMapLoc = std::max(availOutMapLoc, mapLoc + ((nextBuiltInUsage.cullDistanceIn > 4) ? 2u : 1u));
            }
            else
            {
                builtInUsage.tes.cullDistance = 0;
            }

            builtInUsage.tes.layer = false;
            builtInUsage.tes.viewportIndex = false;
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // TES only
            if ((builtInUsage.tes.clipDistance > 0) || (builtInUsage.tes.cullDistance > 0))
            {
                uint32_t mapLoc = availOutMapLoc++;
                if (builtInUsage.tes.clipDistance + builtInUsage.tes.cullDistance > 4)
                {
                    LLPC_ASSERT(builtInUsage.tes.clipDistance +
                                builtInUsage.tes.cullDistance <= MaxClipCullDistanceCount);
                    ++availOutMapLoc; // Occupy two locations
                }

                if (builtInUsage.tes.clipDistance > 0)
                {
                    inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
                }

                if (builtInUsage.tes.cullDistance > 0)
                {
                    if (builtInUsage.tes.clipDistance >= 4)
                    {
                        ++mapLoc;
                    }
                    inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
                }
            }

            if (builtInUsage.tes.viewportIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;
            }

            if (builtInUsage.tes.layer)
            {
                inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;
            }

            if (builtInUsage.tes.viewIndex)
            {
                inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);

        inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, availPerPatchInMapLoc);
    }
    else if (m_shaderStage == ShaderStageGeometry)
    {
        // GS  ==>  XXX
        uint32_t availInMapLoc  = inOutUsage.inputMapLocCount;

        // Map built-in inputs to generic ones
        if (builtInUsage.gs.positionIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;
        }

        if (builtInUsage.gs.pointSizeIn)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;
        }

        if (builtInUsage.gs.clipDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
            if (builtInUsage.gs.clipDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        if (builtInUsage.gs.cullDistanceIn > 0)
        {
            inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
            if (builtInUsage.gs.cullDistanceIn > 4)
            {
                ++availInMapLoc;
            }
        }

        // Map built-in outputs to generic ones (for GS)
        if (builtInUsage.gs.position)
        {
            MapGsBuiltInOutput(BuiltInPosition, 1);
        }

        if (builtInUsage.gs.pointSize)
        {
            MapGsBuiltInOutput(BuiltInPointSize, 1);
        }

        if (builtInUsage.gs.clipDistance > 0)
        {
            MapGsBuiltInOutput(BuiltInClipDistance, builtInUsage.gs.clipDistance);
        }

        if (builtInUsage.gs.cullDistance > 0)
        {
            MapGsBuiltInOutput(BuiltInCullDistance, builtInUsage.gs.cullDistance);
        }

        if (builtInUsage.gs.primitiveId)
        {
            MapGsBuiltInOutput(BuiltInPrimitiveId, 1);
        }

        if (builtInUsage.gs.layer)
        {
            MapGsBuiltInOutput(BuiltInLayer, 1);
        }

        if (builtInUsage.gs.viewIndex)
        {
            MapGsBuiltInOutput(BuiltInViewIndex, 1);
        }

        if (builtInUsage.gs.viewportIndex)
        {
            MapGsBuiltInOutput(BuiltInViewportIndex, 1);
        }

        // Map built-in outputs to generic ones (for copy shader)
        auto& builtInOutLocs = inOutUsage.gs.builtInOutLocs;

        if (nextStage == ShaderStageFragment)
        {
            // GS  ==>  FS
            const auto& nextBuiltInUsage = pNextResUsage->builtInUsage.fs;
            auto& nextInOutUsage = pNextResUsage->inOutUsage;

            if (nextBuiltInUsage.clipDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
                builtInOutLocs[BuiltInClipDistance] = mapLoc;
            }

            if (nextBuiltInUsage.cullDistance > 0)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
                builtInOutLocs[BuiltInCullDistance] = mapLoc;
            }

            if (nextBuiltInUsage.primitiveId)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
                builtInOutLocs[BuiltInPrimitiveId] = mapLoc;
            }

            if (nextBuiltInUsage.layer)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
                builtInOutLocs[BuiltInLayer] = mapLoc;
            }

            if (nextBuiltInUsage.viewIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) !=
                    nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
                builtInOutLocs[BuiltInViewIndex] = mapLoc;
            }

            if (nextBuiltInUsage.viewportIndex)
            {
                LLPC_ASSERT(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) !=
                            nextInOutUsage.builtInInputLocMap.end());
                const uint32_t mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
                builtInOutLocs[BuiltInViewportIndex] = mapLoc;
            }
        }
        else if (nextStage == ShaderStageInvalid)
        {
            // GS only
            uint32_t availOutMapLoc = inOutUsage.outputLocMap.size(); // Reset available location

            if ((builtInUsage.gs.clipDistance > 0) || (builtInUsage.gs.cullDistance > 0))
            {
                uint32_t mapLoc = availOutMapLoc++;
                if (builtInUsage.gs.clipDistance + builtInUsage.gs.cullDistance > 4)
                {
                    LLPC_ASSERT(builtInUsage.gs.clipDistance +
                                builtInUsage.gs.cullDistance <= MaxClipCullDistanceCount);
                    ++availOutMapLoc; // Occupy two locations
                }

                if (builtInUsage.gs.clipDistance > 0)
                {
                    builtInOutLocs[BuiltInClipDistance] = mapLoc;
                }

                if (builtInUsage.gs.cullDistance > 0)
                {
                    if (builtInUsage.gs.clipDistance >= 4)
                    {
                        ++mapLoc;
                    }
                    builtInOutLocs[BuiltInCullDistance] = mapLoc;
                }
            }

            if (builtInUsage.gs.primitiveId)
            {
                builtInOutLocs[BuiltInPrimitiveId] = availOutMapLoc++;
            }

            if (builtInUsage.gs.viewportIndex)
            {
                builtInOutLocs[BuiltInViewportIndex] = availOutMapLoc++;
            }

            if (builtInUsage.gs.layer)
            {
                builtInOutLocs[BuiltInLayer] = availOutMapLoc++;
            }

            if (builtInUsage.gs.viewIndex)
            {
                builtInOutLocs[BuiltInViewIndex] = availOutMapLoc++;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
    }
    else if (m_shaderStage == ShaderStageFragment)
    {
        // FS
        uint32_t availInMapLoc = inOutUsage.inputMapLocCount;

        if (builtInUsage.fs.pointCoord)
        {
            inOutUsage.builtInInputLocMap[BuiltInPointCoord] = availInMapLoc++;
        }

        if (builtInUsage.fs.primitiveId)
        {
            inOutUsage.builtInInputLocMap[BuiltInPrimitiveId] = availInMapLoc++;
        }

        if (builtInUsage.fs.layer)
        {
            inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;
        }

        if (builtInUsage.fs.viewIndex)
        {
            inOutUsage.builtInInputLocMap[BuiltInViewIndex] = availInMapLoc++;
        }

        if (builtInUsage.fs.viewportIndex)
        {
            inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;
        }

        if ((builtInUsage.fs.clipDistance > 0) || (builtInUsage.fs.cullDistance > 0))
        {
            uint32_t mapLoc = availInMapLoc++;
            if (builtInUsage.fs.clipDistance + builtInUsage.fs.cullDistance > 4)
            {
                LLPC_ASSERT(builtInUsage.fs.clipDistance +
                            builtInUsage.fs.cullDistance <= MaxClipCullDistanceCount);
                ++availInMapLoc; // Occupy two locations
            }

            if (builtInUsage.fs.clipDistance > 0)
            {
                inOutUsage.builtInInputLocMap[BuiltInClipDistance] = mapLoc;
            }

            if (builtInUsage.fs.cullDistance > 0)
            {
                if (builtInUsage.fs.clipDistance >= 4)
                {
                    ++mapLoc;
                }
                inOutUsage.builtInInputLocMap[BuiltInCullDistance] = mapLoc;
            }
        }

        inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
    }

    // Do builtin-to-generic mapping
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC builtin-to-generic mapping results (" << GetShaderStageName(m_shaderStage)
              << " shader)\n\n");
    if (inOutUsage.builtInInputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.builtInInputLocMap)
        {
            const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
            const uint32_t loc = builtInMap.second;
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  builtin = "
                          << BuilderImplInOut::GetBuiltInName(builtInId)
                          << "  =>  Mapped = " << loc << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (inOutUsage.builtInOutputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.builtInOutputLocMap)
        {
            const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
            const uint32_t loc = builtInMap.second;

            if (m_shaderStage == ShaderStageGeometry)
            {
                LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true)
                    << ") Output: stream = " << inOutUsage.gs.rasterStream << " , "
                    << "builtin = " << BuilderImplInOut::GetBuiltInName(builtInId)
                    << "  =>  Mapped = " << loc << "\n");
            }
            else
            {
                LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true)
                    << ") Output: builtin = "
                    << BuilderImplInOut::GetBuiltInName(builtInId)
                    << "  =>  Mapped = " << loc << "\n");
            }
        }
        LLPC_OUTS("\n");
    }

    if (inOutUsage.perPatchBuiltInInputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.perPatchBuiltInInputLocMap)
        {
            const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
            const uint32_t loc = builtInMap.second;
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  builtin = "
                          << BuilderImplInOut::GetBuiltInName(builtInId)
                          << "  =>  Mapped = " << loc << "\n");
        }
        LLPC_OUTS("\n");
    }

    if (inOutUsage.perPatchBuiltInOutputLocMap.empty() == false)
    {
        for (const auto& builtInMap : inOutUsage.perPatchBuiltInOutputLocMap)
        {
            const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
            const uint32_t loc = builtInMap.second;
            LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): builtin = "
                          << BuilderImplInOut::GetBuiltInName(builtInId)
                          << "  =>  Mapped = " << loc << "\n");
        }
        LLPC_OUTS("\n");
    }

    LLPC_OUTS("// LLPC location count results (after builtin-to-generic mapping)\n\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input:  loc count = "
                  << inOutUsage.inputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output: loc count = "
                  << inOutUsage.outputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Input (per-patch):  loc count = "
                  << inOutUsage.perPatchInputMapLocCount << "\n");
    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true) << ") Output (per-patch): loc count = "
                  << inOutUsage.perPatchOutputMapLocCount << "\n");
    LLPC_OUTS("\n");
}

// =====================================================================================================================
// Map locations of generic outputs of geometry shader to tightly packed ones.
void PatchResourceCollect::MapGsGenericOutput(
    GsOutLocInfo outLocInfo)             // GS output location info
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    uint32_t streamId = outLocInfo.streamId;
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    auto& inOutUsage = pResUsage->inOutUsage.gs;

    pResUsage->inOutUsage.outputLocMap[outLocInfo.u32All] = inOutUsage.outLocCount[streamId]++;

    uint32_t assignedLocCount = inOutUsage.outLocCount[0] +
                            inOutUsage.outLocCount[1] +
                            inOutUsage.outLocCount[2] +
                            inOutUsage.outLocCount[3];

    pResUsage->inOutUsage.outputMapLocCount = std::max(pResUsage->inOutUsage.outputMapLocCount, assignedLocCount);

    LLPC_OUTS("(" << GetShaderStageAbbreviation(m_shaderStage, true)
                << ") Output: stream = " << outLocInfo.streamId << ", "
                << " loc = " << outLocInfo.location
                << "  =>  Mapped = "
                << pResUsage->inOutUsage.outputLocMap[outLocInfo.u32All] << "\n");
}

// =====================================================================================================================
// Map built-in outputs of geometry shader to tightly packed locations.
void PatchResourceCollect::MapGsBuiltInOutput(
    uint32_t builtInId,         // Built-in ID
    uint32_t elemCount)         // Element count of this built-in
{
    LLPC_ASSERT(m_shaderStage == ShaderStageGeometry);
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    auto& inOutUsage = pResUsage->inOutUsage.gs;
    uint32_t streamId = inOutUsage.rasterStream;

    pResUsage->inOutUsage.builtInOutputLocMap[builtInId] = inOutUsage.outLocCount[streamId]++;

    if (elemCount > 4)
    {
        inOutUsage.outLocCount[streamId]++;
    }

    uint32_t assignedLocCount = inOutUsage.outLocCount[0] +
                            inOutUsage.outLocCount[1] +
                            inOutUsage.outLocCount[2] +
                            inOutUsage.outLocCount[3];

    pResUsage->inOutUsage.outputMapLocCount = std::max(pResUsage->inOutUsage.outputMapLocCount, assignedLocCount);
}

// =====================================================================================================================
// Determine whether the requirements of packing input/output is satisfied in patch phase
bool PatchResourceCollect::CanPackInOut() const
{
    // Pack input/output requirements:
    // 1) -pack-in-out option is on
    // 2) It is a VS-FS pipeline
    return PackInOut &&
           (m_pPipelineState->GetShaderStageMask() ==
            (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageFragment)));
}

// =====================================================================================================================
// The process of packing input/output
void PatchResourceCollect::PackInOutLocation()
{
    if (m_shaderStage == ShaderStageFragment)
    {
        m_pLocationMapManager->BuildLocationMap();

        ReviseInputImportCalls();

        m_inOutCalls.clear(); // It will hold XX' output calls
    }
    else if (m_shaderStage == ShaderStageVertex)
    {
        ReassembleOutputExportCalls();

        // For computing the shader hash
        m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage.inOutLocMap =
            m_pPipelineState->GetShaderResourceUsage(ShaderStageFragment)->inOutUsage.inOutLocMap;
    }
    else
    {
        // TODO: Pack input/output in other stages is not supported
        LLPC_NOT_IMPLEMENTED();
    }
}

// =====================================================================================================================
// Revise the location and element index fields of the fragment shaders input import functions
void PatchResourceCollect::ReviseInputImportCalls()
{
    if (m_inOutCalls.empty())
    {
        return;
    }

    LLPC_ASSERT(m_shaderStage == ShaderStageFragment);

    auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage;
    auto& inputLocMap = inOutUsage.inputLocMap;
    inputLocMap.clear();

    BuilderBase builder(*m_pContext);

    for (auto pCall : m_inOutCalls)
    {
        auto argCount = pCall->arg_size();
        const bool isInterpolant = (argCount == 5);
        uint32_t compIdx = 1;
        uint32_t locOffset = 0;
        if (isInterpolant)
        {
            compIdx = 2;
            locOffset = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        }

        // Construct original InOutLocation from the location and elemIdx operands of the FS' input import call
        InOutLocation origInLoc = {};
        origInLoc.locationInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue() + locOffset;
        origInLoc.locationInfo.component = cast<ConstantInt>(pCall->getOperand(compIdx))->getZExtValue();
        origInLoc.locationInfo.half = false;

        // Get the packed InOutLocation from locationMap
        const InOutLocation* pNewInLoc = nullptr;
        m_pLocationMapManager->FindMap(origInLoc, pNewInLoc);
        LLPC_ASSERT(m_pLocationMapManager->FindMap(origInLoc, pNewInLoc));

        // TODO: inputLocMap can be removed
        inputLocMap[pNewInLoc->locationInfo.location] = InvalidValue;
        inOutUsage.inOutLocMap[origInLoc.AsIndex()] = pNewInLoc->AsIndex();

        // Re-write the input import call by using the new InOutLocation
        SmallVector<Value*, 5> args;
        std::string callName;
        if (isInterpolant == false)
        {
            args.push_back(builder.getInt32(pNewInLoc->locationInfo.location));
            args.push_back(builder.getInt32(pNewInLoc->locationInfo.component));
            args.push_back(pCall->getOperand(2));
            args.push_back(pCall->getOperand(3));

            callName = LlpcName::InputImportGeneric;
        }
        else
        {
            args.push_back(builder.getInt32(pNewInLoc->locationInfo.location));
            args.push_back(builder.getInt32(0));
            args.push_back(builder.getInt32(pNewInLoc->locationInfo.component));
            args.push_back(pCall->getOperand(3));
            args.push_back(pCall->getOperand(4));

            callName = LlpcName::InputImportInterpolant;
        }

        // Previous stage converts non-float type to float type when outputs
        Type* pReturnTy = builder.getFloatTy();
        AddTypeMangling(pReturnTy, args, callName);
        Value* pOutValue = EmitCall(callName,
                                    pReturnTy,
                                    args,
                                    {},
                                    pCall);

        // Restore float type to original type
        builder.SetInsertPoint(pCall);

        auto pCallee = pCall->getCalledFunction();
        Type* pOrigReturnTy = pCallee->getReturnType();
        if (pOrigReturnTy->isIntegerTy())
        {
            // float -> i32
            pOutValue = builder.CreateBitCast(pOutValue, builder.getInt32Ty());
            if (pOrigReturnTy->getScalarSizeInBits() < 32)
            {
                // i32 -> i16 or i8
                pOutValue = builder.CreateTrunc(pOutValue, pOrigReturnTy);
            }
        }
        else if (pOrigReturnTy->isHalfTy())
        {
            // float -> f16
            pOutValue = builder.CreateFPTrunc(pOutValue, pOrigReturnTy);
        }

        pCall->replaceAllUsesWith(pOutValue);
    }
}

// =====================================================================================================================
// Re-assemble output export functions based on the locationMap
void PatchResourceCollect::ReassembleOutputExportCalls()
{
    if (m_inOutCalls.empty())
    {
        return;
    }

    auto& inOutUsage = m_pPipelineState->GetShaderResourceUsage(m_shaderStage)->inOutUsage;

    // Collect the components of a vector exported from each packed location
    // Assume each location exports a vector with four components
    std::vector<std::array<Value*, 4>> packedComponents(m_inOutCalls.size());
    for (auto pCall : m_inOutCalls)
    {
        InOutLocation origOutLoc = {};
        origOutLoc.locationInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
        origOutLoc.locationInfo.component = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        origOutLoc.locationInfo.half = false;

        const InOutLocation* pNewInLoc = nullptr;
        const bool isFound = m_pLocationMapManager->FindMap(origOutLoc, pNewInLoc);
        if (isFound == false)
        {
            continue;
        }

        auto& components = packedComponents[pNewInLoc->locationInfo.location];
        components[pNewInLoc->locationInfo.component] = pCall->getOperand(2);
    }

    // Re-assamble XX' output export calls for each packed location
    BuilderBase builder(*m_pContext);
    builder.SetInsertPoint(m_inOutCalls.back());

    auto& outputLocMap = inOutUsage.outputLocMap;
    outputLocMap.clear();

    Value* args[3] = {};
    uint32_t consectiveLocation = 0;
    for (auto components : packedComponents)
    {
        uint32_t compCount = 0;
        for (auto pComp : components)
        {
            if (pComp != nullptr)
            {
                ++compCount;
            }
        }

        if (compCount == 0)
        {
            break;
        }

        // Construct the output vector
        Value* pOutValue = (compCount == 1) ? components[0] :
                           UndefValue::get(VectorType::get(builder.getFloatTy(), compCount));
        for (auto compIdx = 0; compIdx < compCount; ++compIdx)
        {
            // Type conversion from non-float to float
            Value* pComp = components[compIdx];
            Type* pCompTy = pComp->getType();
            if (pCompTy->isIntegerTy())
            {
                // i8/i16 -> i32
                if (pCompTy->getScalarSizeInBits() < 32)
                {
                    pComp = builder.CreateZExt(pComp, builder.getInt32Ty());
                }
                // i32 -> float
                pComp = builder.CreateBitCast(pComp, builder.getFloatTy());
            }
            else if (pCompTy->isHalfTy())
            {
                // f16 -> float
                pComp = builder.CreateFPExt(pComp, builder.getFloatTy());
            }

            if (compCount > 1)
            {
                pOutValue = builder.CreateInsertElement(pOutValue, pComp, compIdx);
            }
            else
            {
                pOutValue = pComp;
            }
        }

        args[0] = builder.getInt32(consectiveLocation);
        args[1] = builder.getInt32(0);
        args[2] = pOutValue;

        std::string callName(LlpcName::OutputExportGeneric);
        AddTypeMangling(builder.getVoidTy(), args, callName);

        builder.CreateNamedCall(callName, builder.getVoidTy(), args, {});

        outputLocMap[consectiveLocation] = InvalidValue;
        ++consectiveLocation;
    }
}

// =====================================================================================================================
// Scalarize last vertex processing stage outputs and FS inputs ready for packing.
void PatchResourceCollect::ScalarizeForInOutPacking(
    Module* pModule)    // [in/out] Module
{
    // First gather the input/output calls that need scalarizing.
    SmallVector<CallInst*, 4> vsOutputCalls;
    SmallVector<CallInst*, 4> fsInputCalls;
    for (Function& func : *pModule)
    {
        if (func.getName().startswith(LlpcName::InputImportGeneric) ||
            func.getName().startswith(LlpcName::InputImportInterpolant))
        {
            // This is a generic (possibly interpolated) input. Find its uses in FS.
            for (User* pUser : func.users())
            {
                auto pCall = cast<CallInst>(pUser);
                if (m_pPipelineShaders->GetShaderStage(pCall->getFunction()) != ShaderStageFragment)
                {
                    continue;
                }
                // We have a use in FS. See if it needs scalarizing.
                if (isa<VectorType>(pCall->getType()) || (pCall->getType()->getPrimitiveSizeInBits() == 64))
                {
                    fsInputCalls.push_back(pCall);
                }
            }
        }
        else if (func.getName().startswith(LlpcName::OutputExportGeneric))
        {
            // This is a generic output. Find its uses in the last vertex processing stage.
            for (User* pUser : func.users())
            {
                auto pCall = cast<CallInst>(pUser);
                if (m_pPipelineShaders->GetShaderStage(pCall->getFunction()) !=
                      m_pPipelineState->GetLastVertexProcessingStage())
                {
                    continue;
                }
                // We have a use the last vertex processing stage. See if it needs scalarizing. The output value is
                // always the final argument.
                Type* pValueTy = pCall->getArgOperand(pCall->getNumArgOperands() - 1)->getType();
                if (isa<VectorType>(pValueTy) || (pValueTy->getPrimitiveSizeInBits() == 64))
                {
                    vsOutputCalls.push_back(pCall);
                }
            }
        }
    }

    // Scalarize the gathered inputs and outputs.
    for (CallInst* pCall : fsInputCalls)
    {
        ScalarizeGenericInput(pCall);
    }
    for (CallInst* pCall : vsOutputCalls)
    {
        ScalarizeGenericOutput(pCall);
    }
}

// =====================================================================================================================
// Scalarize a generic input.
// This is known to be an FS generic or interpolant input that is either a vector or 64 bit.
void PatchResourceCollect::ScalarizeGenericInput(
    CallInst* pCall)  // [in] Call that represents importing the generic or interpolant input
{
    BuilderBase builder(pCall->getContext());
    builder.SetInsertPoint(pCall);

    // FS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
    //      @llpc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx,
    //                                            i32 interpMode, <2 x float> | i32 auxInterpValue)
    SmallVector<Value*, 5> args;
    for (uint32_t i = 0, end = pCall->getNumArgOperands(); i != end; ++i)
    {
        args.push_back(pCall->getArgOperand(i));
    }

    bool isInterpolant = args.size() != 4;
    uint32_t elemIdxArgIdx = isInterpolant ? 2 : 1;
    uint32_t elemIdx = cast<ConstantInt>(args[elemIdxArgIdx])->getZExtValue();
    Type* pResultTy = pCall->getType();

    if (!isa<VectorType>(pResultTy))
    {
        // Handle the case of splitting a 64 bit scalar in two.
        LLPC_ASSERT(pResultTy->getPrimitiveSizeInBits() == 64);
        std::string callName = isInterpolant ? LlpcName::InputImportInterpolant : LlpcName::InputImportGeneric;
        AddTypeMangling(builder.getInt32Ty(), args, callName);
        Value* pResult = UndefValue::get(VectorType::get(builder.getInt32Ty(), 2));
        for (uint32_t i = 0; i != 2; ++i)
        {
            args[elemIdxArgIdx] = builder.getInt32(elemIdx * 2 + i);
            pResult = builder.CreateInsertElement(pResult,
                                                  builder.CreateNamedCall(callName,
                                                                          builder.getInt32Ty(),
                                                                          args,
                                                                          Attribute::ReadOnly),
                                                  i);
        }
        pResult = builder.CreateBitCast(pResult, pCall->getType());
        pCall->replaceAllUsesWith(pResult);
        pCall->eraseFromParent();
        return;
    }

    // Now we know we're reading a vector.
    Type* pElementTy = pResultTy->getVectorElementType();
    uint32_t scalarizeBy = pResultTy->getVectorNumElements();

    // Find trivially unused elements.
    // This is not quite as good as the previous version of this code that scalarized in the
    // front-end before running some LLVM optimizations that removed unused inputs. In the future,
    // we can fix this properly by doing the whole of generic input/output assignment later on in
    // the middle-end, somewhere in the LLVM middle-end optimization pass flow.
    static const uint32_t MaxScalarizeBy = 4;
    LLPC_ASSERT(scalarizeBy <= MaxScalarizeBy);
    bool elementUsed[MaxScalarizeBy] = {};
    bool unknownElementsUsed = false;
    for (User* pUser : pCall->users())
    {
        if (auto pExtract = dyn_cast<ExtractElementInst>(pUser))
        {
            uint32_t idx = cast<ConstantInt>(pExtract->getIndexOperand())->getZExtValue();
            LLPC_ASSERT(idx < scalarizeBy);
            elementUsed[idx] = true;
            continue;
        }
        if (auto pShuffle = dyn_cast<ShuffleVectorInst>(pUser))
        {
            SmallVector<int, 4> mask;
            pShuffle->getShuffleMask(mask);
            for (int maskElement : mask)
            {
                if (maskElement >= 0)
                {
                    if (maskElement < scalarizeBy)
                    {
                        if (pShuffle->getOperand(0) == pCall)
                        {
                            elementUsed[maskElement] = true;
                        }
                    }
                    else
                    {
                        LLPC_ASSERT(maskElement < 2 * scalarizeBy);
                        if (pShuffle->getOperand(1) == pCall)
                        {
                            elementUsed[maskElement - scalarizeBy] = true;
                        }
                    }
                }
            }
            continue;
        }
        unknownElementsUsed = true;
        break;
    }

    // Load the individual elements and insert into a vector.
    Value* pResult = UndefValue::get(pResultTy);
    std::string callName = isInterpolant ? LlpcName::InputImportInterpolant : LlpcName::InputImportGeneric;
    AddTypeMangling(pElementTy, args, callName);
    for (uint32_t i = 0; i != scalarizeBy; ++i)
    {
        if (!unknownElementsUsed && !elementUsed[i])
        {
            continue; // Omit trivially unused element
        }
        args[elemIdxArgIdx] = builder.getInt32(elemIdx + i);

        CallInst* pElement = builder.CreateNamedCall(callName, pElementTy, args, Attribute::ReadOnly);
        pResult = builder.CreateInsertElement(pResult, pElement, i);
        if (pElementTy->getPrimitiveSizeInBits() == 64)
        {
            // If scalarizing with 64 bit elements, further split each element.
            ScalarizeGenericInput(pElement);
        }
    }

    pCall->replaceAllUsesWith(pResult);
    pCall->eraseFromParent();
}

// =====================================================================================================================
// Scalarize a generic output.
// This is known to be a last vertex processing stage (VS/TES/GS) generic output that is either a vector or 64 bit.
void PatchResourceCollect::ScalarizeGenericOutput(
    CallInst* pCall)  // [in] Call that represents exporting the generic output
{
    BuilderBase builder(pCall->getContext());
    builder.SetInsertPoint(pCall);

    // VS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    // TES: @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    // GS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
    SmallVector<Value*, 5> args;
    for (uint32_t i = 0, end = pCall->getNumArgOperands(); i != end; ++i)
    {
        args.push_back(pCall->getArgOperand(i));
    }

    static const uint32_t ElemIdxArgIdx = 1;
    uint32_t valArgIdx = pCall->getNumArgOperands() - 1;
    uint32_t elemIdx = cast<ConstantInt>(args[ElemIdxArgIdx])->getZExtValue();
    Value* pOutputVal = pCall->getArgOperand(valArgIdx);
    Type* pElementTy = pOutputVal->getType();
    uint32_t scalarizeBy = 1;
    if (auto pVectorTy = dyn_cast<VectorType>(pElementTy))
    {
        scalarizeBy = pVectorTy->getNumElements();
        pElementTy = pVectorTy->getElementType();
    }

    // For a 64-bit element type, split each element in two. (We're assuming no interpolation for 64 bit.)
    if (pElementTy->getPrimitiveSizeInBits() == 64)
    {
        scalarizeBy *= 2;
        elemIdx *= 2;
        pElementTy = builder.getInt32Ty();
    }

    // Bitcast the original value to the vector type if necessary.
    pOutputVal = builder.CreateBitCast(pOutputVal, VectorType::get(pElementTy, scalarizeBy));

    // Extract and store the individual elements.
    std::string callName;
    for (uint32_t i = 0; i != scalarizeBy; ++i)
    {
        args[ElemIdxArgIdx] = builder.getInt32(elemIdx + i);
        args[valArgIdx] = builder.CreateExtractElement(pOutputVal, i);
        if (i == 0)
        {
            callName = LlpcName::OutputExportGeneric;
            AddTypeMangling(nullptr, args, callName);
        }
        builder.CreateNamedCall(callName, builder.getVoidTy(), args, {});
    }

    pCall->eraseFromParent();
}

// =====================================================================================================================
// Fill the locationSpan container by constructing a LocationSpan from each input import call
bool InOutLocationMapManager::AddSpan(
    CallInst*   pCall)  // [in] Call to process
{
    auto pCallee = pCall->getCalledFunction();
    auto mangledName = pCallee->getName();
    bool isInput = false;
    if (mangledName.startswith(LlpcName::InputImportGeneric))
    {
        LocationSpan span = {};

        span.firstLocation.locationInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
        span.firstLocation.locationInfo.component = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
        span.firstLocation.locationInfo.half = false;

        const uint32_t bitWidth = pCallee->getReturnType()->getScalarSizeInBits();
        span.compatibilityInfo.halfComponentCount = bitWidth < 64 ? 2 : 4;

        span.compatibilityInfo.isFlat =
            (cast<ConstantInt>(pCall->getOperand(2))->getZExtValue() == InOutInfo::InterpModeFlat);
        span.compatibilityInfo.is16Bit = false;
        span.compatibilityInfo.isCustom =
            (cast<ConstantInt>(pCall->getOperand(2))->getZExtValue() == InOutInfo::InterpModeCustom);

        LLPC_ASSERT(std::find(m_locationSpans.begin(), m_locationSpans.end(), span) == m_locationSpans.end());
        m_locationSpans.push_back(span);

        isInput = true;
    }
    if (mangledName.startswith(LlpcName::InputImportInterpolant))
    {
        auto pLocOffset = pCall->getOperand(1);
        LLPC_ASSERT(isa<ConstantInt>(pLocOffset));

        LocationSpan span = {};

        span.firstLocation.locationInfo.location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue() +
                                        cast<ConstantInt>(pLocOffset)->getZExtValue();
        span.firstLocation.locationInfo.component = cast<ConstantInt>(pCall->getOperand(2))->getZExtValue();
        span.firstLocation.locationInfo.half = false;

        const uint32_t bitWidth = pCallee->getReturnType()->getScalarSizeInBits();
        span.compatibilityInfo.halfComponentCount = bitWidth < 64 ? 2 : 4;

        span.compatibilityInfo.isFlat =
            (cast<ConstantInt>(pCall->getOperand(3))->getZExtValue() == InOutInfo::InterpModeFlat);
        span.compatibilityInfo.is16Bit = false;
        span.compatibilityInfo.isCustom =
            (cast<ConstantInt>(pCall->getOperand(3))->getZExtValue() == InOutInfo::InterpModeCustom);

        if (std::find(m_locationSpans.begin(), m_locationSpans.end(), span) == m_locationSpans.end())
        {
            m_locationSpans.push_back(span);
        }

        isInput = true;
    }

    return isInput;
}

// =====================================================================================================================
// Build the map between orignal InOutLocation and packed InOutLocation based on sorted locaiton spans
void InOutLocationMapManager::BuildLocationMap()
{
    // Sort m_locationSpans based on LocationSpan::GetCompatibilityKey() and InOutLocation::AsIndex()
    std::sort(m_locationSpans.begin(), m_locationSpans.end());

    // Map original InOutLocation to new InOutLocation
    uint32_t consectiveLocation = 0;
    uint32_t compIdx = 0;
    for (auto spanIt = m_locationSpans.begin(); spanIt != m_locationSpans.end(); ++spanIt)
    {
        // Increase consectiveLocation when halfComponentCount is up to 8 or the span isn't compatible to previous
        // Otherwise, increase the compIdx in a packed vector
        if (spanIt != m_locationSpans.begin())
        {
            const auto& prevSpan = *(--spanIt);
            ++spanIt;
            if ((isCompatible(prevSpan, *spanIt) == false) || (compIdx == 3))
            {
                ++consectiveLocation;
                compIdx = 0;
            }
            else if (spanIt->compatibilityInfo.halfComponentCount > 1)
            {
                compIdx += spanIt->compatibilityInfo.halfComponentCount / 2;
            }
            else if (spanIt->firstLocation.locationInfo.half)
            {
                // 16-bit attribute
                compIdx += 1;
            }
        }

        InOutLocation newLocation = {};
        newLocation.locationInfo.location = consectiveLocation;
        newLocation.locationInfo.component = compIdx;
        newLocation.locationInfo.half = false;

        InOutLocation& origLocation = spanIt->firstLocation;
        m_locationMap[origLocation] = newLocation;
    }

    // Exists temporarily for computing m_locationMap
    m_locationSpans.clear();
}

// =====================================================================================================================
// Output a mapped InOutLocation from a given InOutLocation if the mapping exists
bool InOutLocationMapManager::FindMap(
    const InOutLocation& originalLocation,  // [in] The original InOutLocation
    const InOutLocation*& pNewLocation)     // [out] The new InOutLocation
{
    auto it = m_locationMap.find(originalLocation);
    if (it == m_locationMap.end())
    {
        return false;
    }

    pNewLocation = &(it->second);
    return true;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for resource collecting.
INITIALIZE_PASS(PatchResourceCollect, DEBUG_TYPE,
                "Patch LLVM for resource collecting", false, false)
