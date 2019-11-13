/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcGraphicsContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::GraphicsContext.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-graphics-context"

#include "llvm/Support/Format.h"

#include "SPIRVInternal.h"
#include "llpcCompiler.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"
#include "llpcGraphicsContext.h"
#if LLPC_BUILD_GFX10
#include "llpcNggLdsManager.h"
#endif

#include "llpcInternal.h"

using namespace llvm;
using namespace SPIRV;

namespace llvm
{

namespace cl
{

// -enable-tess-offchip: enable tessellation off-chip mode
opt<bool> EnableTessOffChip("enable-tess-offchip",
                            desc("Enable tessellation off-chip mode"),
                            init(false));

// -disable-gs-onchip: disable geometry shader on-chip mode
opt<bool> DisableGsOnChip("disable-gs-onchip",
                          desc("Disable geometry shader on-chip mode"),
                          init(false));

// -pack-in-out: pack input/output
opt<bool> PackInOut("pack-in-out", desc("Pack input/output"), init(true));

#if LLPC_BUILD_GFX10
extern opt<int> SubgroupSize;
#endif

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
GraphicsContext::GraphicsContext(
    GfxIpVersion                     gfxIp,            // Graphics Ip version info
    const GpuProperty*               pGpuProp,         // [in] GPU Property
    const WorkaroundFlags*           pGpuWorkarounds,  // [in] GPU workarounds
    const GraphicsPipelineBuildInfo* pPipelineInfo,    // [in] Graphics pipeline build info
    MetroHash::Hash*                 pPipelineHash,    // [in] Pipeline hash code
    MetroHash::Hash*                 pCacheHash)       // [in] Cache hash code
    :
    PipelineContext(gfxIp, pGpuProp, pGpuWorkarounds, pPipelineHash, pCacheHash),
    m_pPipelineInfo(pPipelineInfo),
    m_stageMask(0),
    m_activeStageCount(0),
    m_tessOffchip(cl::EnableTessOffChip),
    m_gsOnChip(false),
    m_packInOut(cl::PackInOut)
{
    if (gfxIp.major >= 9)
    {
        // For GFX9+, always enable tessellation off-chip mode
        m_tessOffchip = true;
    }

#if LLPC_BUILD_GFX10
    memset(&m_nggControl, 0, sizeof(m_nggControl));

    if (gfxIp.major >= 10)
    {
        // NOTE: All fields of NGG controls are determined by the pass of resource collecting in patching. Here, we still
        // set NGG enablement early. The field is used when deciding if we need extra optimizations after NGG primitive
        // shader creation. At that time, the pass of resource collecting has not been run.
        m_nggControl.enableNgg = pPipelineInfo->nggState.enableNgg;
    }
#endif

    const PipelineShaderInfo* shaderInfo[ShaderStageGfxCount] =
    {
        &pPipelineInfo->vs,
        &pPipelineInfo->tcs,
        &pPipelineInfo->tes,
        &pPipelineInfo->gs,
        &pPipelineInfo->fs,
    };

    for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
    {
        if (shaderInfo[stage]->pModuleData != nullptr)
        {
            m_stageMask |= ShaderStageToMask(static_cast<ShaderStage>(stage));
            ++m_activeStageCount;

            if (stage == ShaderStageGeometry)
            {
                m_stageMask |= ShaderStageToMask(ShaderStageCopyShader);
                ++m_activeStageCount;
            }
        }
    }

    for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
    {
        ShaderStage shaderStage = static_cast<ShaderStage>(stage);
        InitShaderResourceUsage(shaderStage, GetShaderResourceUsage(shaderStage));
        InitShaderInterfaceData(GetShaderInterfaceData(shaderStage));
    }
}

// =====================================================================================================================
GraphicsContext::~GraphicsContext()
{
}

// =====================================================================================================================
// Gets resource usage of the specified shader stage.
ResourceUsage* GraphicsContext::GetShaderResourceUsage(
    ShaderStage shaderStage) // Shader stage
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);
    return &m_resUsages[shaderStage];
}

// =====================================================================================================================
// Gets interface data of the specified shader stage.
InterfaceData* GraphicsContext::GetShaderInterfaceData(
    ShaderStage shaderStage)  // Shader stage
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);
    return &m_intfData[shaderStage];
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
const PipelineShaderInfo* GraphicsContext::GetPipelineShaderInfo(
    ShaderStage shaderStage // Shader stage
    ) const
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);

    const PipelineShaderInfo* pShaderInfo = nullptr;
    switch (shaderStage)
    {
    case Llpc::ShaderStageVertex:
        pShaderInfo = &m_pPipelineInfo->vs;
        break;
    case Llpc::ShaderStageTessControl:
        pShaderInfo = &m_pPipelineInfo->tcs;
        break;
    case Llpc::ShaderStageTessEval:
        pShaderInfo = &m_pPipelineInfo->tes;
        break;
    case Llpc::ShaderStageGeometry:
        pShaderInfo = &m_pPipelineInfo->gs;
        break;
    case Llpc::ShaderStageFragment:
        pShaderInfo = &m_pPipelineInfo->fs;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return pShaderInfo;
}

// =====================================================================================================================
// Gets the previous active shader stage in this pipeline
ShaderStage GraphicsContext::GetPrevShaderStage(
    ShaderStage shaderStage // Current shader stage
    ) const
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);

    ShaderStage prevStage = ShaderStageInvalid;

    for (int32_t stage = shaderStage - 1; stage >= 0; --stage)
    {
        if ((m_stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage))) != 0)
        {
            prevStage = static_cast<ShaderStage>(stage);
            break;
        }
    }

    return prevStage;
}

// =====================================================================================================================
// Gets the previous active shader stage in this pipeline
ShaderStage GraphicsContext::GetNextShaderStage(
    ShaderStage shaderStage // Current shader stage
    ) const
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);

    ShaderStage nextStage = ShaderStageInvalid;

    for (uint32_t stage = shaderStage + 1; stage < ShaderStageGfxCount; ++stage)
    {
        if ((m_stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage))) != 0)
        {
            nextStage = static_cast<ShaderStage>(stage);
            break;
        }
    }

    return nextStage;
}

// =====================================================================================================================
// Initializes shader info for null fragment shader.
void GraphicsContext::InitShaderInfoForNullFs()
{
    auto pResUsage = GetShaderResourceUsage(ShaderStageFragment);
    m_stageMask |= ShaderStageToMask(ShaderStageFragment);
    ++m_activeStageCount;

    // Add usage info for dummy input
    FsInterpInfo interpInfo = { 0, false, false, false };
    pResUsage->builtInUsage.fs.smooth = true;
    pResUsage->inOutUsage.inputLocMap[0] = InvalidValue;
    pResUsage->inOutUsage.fs.interpInfo.push_back(interpInfo);

    // Add usage info for dummy output
    pResUsage->inOutUsage.fs.cbShaderMask = 0;
    pResUsage->inOutUsage.fs.dummyExport = true;
    pResUsage->inOutUsage.outputLocMap[0] = InvalidValue;
}

// =====================================================================================================================
// Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
bool GraphicsContext::CheckGsOnChipValidity()
{
    bool gsOnChip = true;

    uint32_t stageMask = GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);
#if LLPC_BUILD_GFX10
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);
#endif

    auto pGsResUsage = GetShaderResourceUsage(ShaderStageGeometry);

    uint32_t inVertsPerPrim = 0;
    bool useAdjacency = false;
    switch (pGsResUsage->builtInUsage.gs.inputPrimitive)
    {
    case InputPoints:
        inVertsPerPrim = 1;
        break;
    case InputLines:
        inVertsPerPrim = 2;
        break;
    case InputLinesAdjacency:
        useAdjacency = true;
        inVertsPerPrim = 4;
        break;
    case InputTriangles:
        inVertsPerPrim = 3;
        break;
    case InputTrianglesAdjacency:
        useAdjacency = true;
        inVertsPerPrim = 6;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    pGsResUsage->inOutUsage.gs.calcFactor.inputVertices = inVertsPerPrim;

    uint32_t outVertsPerPrim = 0;
    switch (pGsResUsage->builtInUsage.gs.outputPrimitive)
    {
    case OutputPoints:
        outVertsPerPrim = 1;
        break;
    case OutputLineStrip:
        outVertsPerPrim = 2;
        break;
    case OutputTriangleStrip:
        outVertsPerPrim = 3;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    if (m_gfxIp.major <= 8)
    {
        uint32_t gsPrimsPerSubgroup = m_pGpuProperty->gsOnChipDefaultPrimsPerSubgroup;

        const uint32_t esGsRingItemSize = 4 * std::max(1u, pGsResUsage->inOutUsage.inputMapLocCount);
        const uint32_t gsInstanceCount  = pGsResUsage->builtInUsage.gs.invocations;
        const uint32_t gsVsRingItemSize = 4 * std::max(1u,
                                                       (pGsResUsage->inOutUsage.outputMapLocCount *
                                                        pGsResUsage->builtInUsage.gs.outputVertices));

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

        // There is a hardware requirement for gsPrimsPerSubgroup * gsInstanceCount to be capped by GsOnChipMaxPrimsPerSubgroup
        // for adjacency primitive or when GS instanceing is used.
        if (useAdjacency || (gsInstanceCount > 1))
        {
            gsPrimsPerSubgroup = std::min(gsPrimsPerSubgroup, (Gfx6::GsOnChipMaxPrimsPerSubgroup / gsInstanceCount));
        }

        // Compute GS-VS LDS size based on target GS primitives per subgroup
        uint32_t gsVsLdsSize = (gsVsRingItemSizeOnChipInstanced * gsPrimsPerSubgroup);

        // Compute ES-GS LDS size based on the worst case number of ES vertices needed to create the target number of
        // GS primitives per subgroup.
        uint32_t esGsLdsSize = esGsRingItemSizeOnChip * esMinVertsPerSubgroup * gsPrimsPerSubgroup;

        // Total LDS use per subgroup aligned to the register granularity
        uint32_t gsOnChipLdsSize = Pow2Align((esGsLdsSize + gsVsLdsSize),
                                             static_cast<uint32_t>((1 << m_pGpuProperty->ldsSizeDwordGranularityShift)));

        // Use the client-specified amount of LDS space per subgroup. If they specified zero, they want us to choose a
        // reasonable default. The final amount must be 128-DWORD aligned.

        uint32_t maxLdsSize = m_pGpuProperty->gsOnChipDefaultLdsSizePerSubgroup;

        // TODO: For BONAIRE A0, GODAVARI and KALINDI, set maxLdsSize to 1024 due to SPI barrier management bug

        // If total LDS usage is too big, refactor partitions based on ratio of ES-GS and GS-VS item sizes.
        if (gsOnChipLdsSize > maxLdsSize)
        {
            const uint32_t esGsItemSizePerPrim = esGsRingItemSizeOnChip * esMinVertsPerSubgroup;
            const uint32_t itemSizeTotal       = esGsItemSizePerPrim + gsVsRingItemSizeOnChipInstanced;

            esGsLdsSize = RoundUpToMultiple((esGsItemSizePerPrim * maxLdsSize) / itemSizeTotal, esGsItemSizePerPrim);
            gsVsLdsSize = RoundDownToMultiple(maxLdsSize - esGsLdsSize, gsVsRingItemSizeOnChipInstanced);

            gsOnChipLdsSize = maxLdsSize;
        }

        // Based on the LDS space, calculate how many GS prims per subgroup and ES vertices per subgroup can be dispatched.
        gsPrimsPerSubgroup          = (gsVsLdsSize / gsVsRingItemSizeOnChipInstanced);
        uint32_t esVertsPerSubgroup = (esGsLdsSize / esGsRingItemSizeOnChip);

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

        bool disableGsOnChip = cl::DisableGsOnChip;
        if (hasTs || (m_gfxIp.major == 6))
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
#if LLPC_BUILD_GFX10
        const auto pNggControl = GetNggControl();

        if (pNggControl->enableNgg)
        {
            // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
            const uint32_t esGsRingItemSize = hasGs ? ((4 * std::max(1u,
                                                                     pGsResUsage->inOutUsage.inputMapLocCount)) | 1) :
                                                      4; // Always 4 components for NGG when GS is not present

            const uint32_t gsVsRingItemSize = hasGs ? std::max(1u,
                                                               4 * pGsResUsage->inOutUsage.outputMapLocCount
                                                                 * pGsResUsage->builtInUsage.gs.outputVertices) : 0;

            const uint32_t esExtraLdsSize = NggLdsManager::CalcEsExtraLdsSize(this) / 4; // In DWORDs
            const uint32_t gsExtraLdsSize = NggLdsManager::CalcGsExtraLdsSize(this) / 4; // In DWORDs

            // primAmpFactor = outputVertices - (outVertsPerPrim - 1)
            const uint32_t primAmpFactor =
                hasGs ? pGsResUsage->builtInUsage.gs.outputVertices - (outVertsPerPrim - 1) : 0;

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
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 26
                case NggSubgroupSizingType::Auto:
                    esVertsPerSubgroup = 126;
                    gsPrimsPerSubgroup = 128;
                    break;
#endif
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
                    esVertsPerSubgroup = RoundDownToMultiple((Gfx9::NggMaxThreadsPerSubgroup / 2u), vertsPerPrimitive);
                    gsPrimsPerSubgroup = esVertsPerSubgroup / vertsPerPrimitive;
                    break;
                case NggSubgroupSizingType::OptimizeForVerts:
                    // Currently the programming of OptimizeForVerts is an inverse of MaximumSize. OptimizeForVerts is
                    // not expected to be a performant choice for fast launch, and as such MaximumSize, HalfSize, or
                    // Explicit should be chosen, with Explicit being optimal for non-point topologies.
                    gsPrimsPerSubgroup = RoundDownToMultiple(Gfx9::NggMaxThreadsPerSubgroup, vertsPerPrimitive);
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
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 26
                case NggSubgroupSizingType::Auto:
#endif
                case NggSubgroupSizingType::MaximumSize:
                default:
                    esVertsPerSubgroup = RoundDownToMultiple(Gfx9::NggMaxThreadsPerSubgroup, vertsPerPrimitive);
                    gsPrimsPerSubgroup = esVertsPerSubgroup / vertsPerPrimitive;
                    break;
                }
            }

            if (hasGs)
            {
                // NOTE: If primitive amplification is active and the currently calculated gsPrimsPerSubgroup multipled
                // by the amplification factor is larger than the supported number of primitives within a subgroup, we
                // need to shrimp the number of gsPrimsPerSubgroup down to a reasonable level to prevent
                // over-allocating LDS.
                uint32_t maxVertOut = hasGs ? pGsResUsage->builtInUsage.gs.outputVertices : 1;

                LLPC_ASSERT(maxVertOut >= primAmpFactor);

                if ((gsPrimsPerSubgroup * maxVertOut) > Gfx9::NggMaxThreadsPerSubgroup)
                {
                    gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / maxVertOut;
                }

                // Let's take into consideration instancing:
                const uint32_t gsInstanceCount = pGsResUsage->builtInUsage.gs.invocations;
                LLPC_ASSERT(gsInstanceCount >= 1);
                gsPrimsPerSubgroup /= gsInstanceCount;
                esVertsPerSubgroup = gsPrimsPerSubgroup * maxVertOut;
            }

            // Make sure that we have at least one primitive
            gsPrimsPerSubgroup = std::max(1u, gsPrimsPerSubgroup);

            uint32_t       expectedEsLdsSize = esVertsPerSubgroup * esGsRingItemSize + esExtraLdsSize;
            const uint32_t expectedGsLdsSize = gsPrimsPerSubgroup * gsVsRingItemSize + gsExtraLdsSize;

            if (expectedGsLdsSize == 0)
            {
                LLPC_ASSERT(hasGs == false);

                expectedEsLdsSize = (Gfx9::NggMaxThreadsPerSubgroup * esGsRingItemSize) + esExtraLdsSize;
            }

            const uint32_t ldsSizeDwords =
                Pow2Align(expectedEsLdsSize + expectedGsLdsSize,
                          static_cast<uint32_t>(1 << m_pGpuProperty->ldsSizeDwordGranularityShift));

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

            gsOnChip = true; // In NGG mode, GS is always on-chip since copy shader is not present.
        }
        else
#endif
        {
            uint32_t ldsSizeDwordGranularity = static_cast<uint32_t>(1 << m_pGpuProperty->ldsSizeDwordGranularityShift);

            // gsPrimsPerSubgroup shouldn't be bigger than wave size.
            uint32_t gsPrimsPerSubgroup = std::min(m_pGpuProperty->gsOnChipDefaultPrimsPerSubgroup,
                                                   GetShaderWaveSize(ShaderStageGeometry));

            // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
            const uint32_t esGsRingItemSize = (4 * std::max(1u, pGsResUsage->inOutUsage.inputMapLocCount)) | 1;

            const uint32_t gsVsRingItemSize = 4 * std::max(1u,
                                                           (pGsResUsage->inOutUsage.outputMapLocCount *
                                                            pGsResUsage->builtInUsage.gs.outputVertices));

            // NOTE: Make gsVsRingItemSize odd by "| 1", to optimize GS -> VS ring layout for LDS bank conflicts.
            const uint32_t gsVsRingItemSizeOnChip = gsVsRingItemSize | 1;

            const uint32_t gsInstanceCount  = pGsResUsage->builtInUsage.gs.invocations;

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

            uint32_t worstCaseEsVertsPerSubgroup = std::min(esMinVertsPerSubgroup * gsPrimsPerSubgroup,
                                                            maxEsVertsPerSubgroup);

            uint32_t esGsLdsSize = (esGsRingItemSize * worstCaseEsVertsPerSubgroup);

            // Total LDS use per subgroup aligned to the register granularity.
            uint32_t gsOnChipLdsSize = RoundUpToMultiple(esGsLdsSize + esGsExtraLdsDwords, ldsSizeDwordGranularity);

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
                worstCaseEsVertsPerSubgroup = std::min(esMinVertsPerSubgroup * gsPrimsPerSubgroup,
                                                       maxEsVertsPerSubgroup);

                LLPC_ASSERT(gsPrimsPerSubgroup > 0);

                esGsLdsSize     = (esGsRingItemSize * worstCaseEsVertsPerSubgroup);
                gsOnChipLdsSize = RoundUpToMultiple(esGsLdsSize + esGsExtraLdsDwords, ldsSizeDwordGranularity);

                LLPC_ASSERT(gsOnChipLdsSize <= maxLdsSize);
            }

            if (hasTs || cl::DisableGsOnChip)
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
                uint32_t onchipEsGsVsLdsSize = RoundUpToMultiple(esGsLdsSize + gsVsLdsSize, ldsSizeDwordGranularity);
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
                        worstCaseEsVertsPerSubgroup = std::min(esMinVertsPerSubgroup * onchipGsPrimsPerSubgroup,
                                                               maxEsVertsPerSubgroup);

                        // Calculate the LDS sizes required to hit this threshold.
                        onchipEsGsLdsSizeOnchipGsVs = Pow2Align(esGsRingItemSize * worstCaseEsVertsPerSubgroup,
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

            uint32_t esVertsPerSubgroup = std::min(esGsLdsSize / esGsRingItemSize, maxEsVertsPerSubgroup);

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

#if LLPC_BUILD_GFX10
            if ((m_gfxIp.major == 10) && hasTs && (gsOnChip == false))
            {
                uint32_t esVertsNum = Gfx9::EsVertsOffchipGsOrTess;
                uint32_t onChipGsLdsMagicSize = Pow2Align((esVertsNum * esGsRingItemSize) + esGsExtraLdsDwords,
                                                          static_cast<uint32_t>((1 << m_pGpuProperty->ldsSizeDwordGranularityShift)));

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
#endif
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
                                    pGsResUsage->builtInUsage.gs.outputVertices * 4;
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

    if (gsOnChip || (m_gfxIp.major >= 9))
    {
#if LLPC_BUILD_GFX10
        if (GetNggControl()->enableNgg)
        {
            LLPC_OUTS("GS primitive amplification factor: "
                      << pGsResUsage->inOutUsage.gs.calcFactor.primAmpFactor
                      << "\n");
            LLPC_OUTS("\n");

            LLPC_OUTS("GS is on-chip (NGG)\n");
        }
        else
#endif
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
// Does user data node merging for all shader stages
void GraphicsContext::DoUserDataNodeMerge()
{
    uint32_t stageMask = GetShaderStageMask();
    SmallVector<ResourceMappingNode, 8> allNodes;

    // No need to merge if there is only one shader stage.
    if (isPowerOf2_32(stageMask))
    {
        return;
    }

    // Collect user data nodes from all shader stages into one big table.
    for (uint32_t stage = 0; stage < ShaderStageNativeStageCount; ++stage)
    {
        if ((stageMask >> stage) & 1)
        {
            auto pShaderInfo = GetPipelineShaderInfo(ShaderStage(stage));
            for (const ResourceMappingNode& node : ArrayRef<ResourceMappingNode>(pShaderInfo->pUserDataNodes,
                                                                                 pShaderInfo->userDataNodeCount))
            {
                allNodes.push_back(node);
            }
        }
    }

    // Sort and merge.
    ArrayRef<ResourceMappingNode> mergedNodes = MergeUserDataNodeTable(allNodes);

    // Collect descriptor range values (immutable descriptors) from all shader stages into one big table.
    SmallVector<DescriptorRangeValue, 8> allRangeValues;
    for (uint32_t stage = 0; stage < ShaderStageNativeStageCount; ++stage)
    {
        if ((stageMask >> stage) & 1)
        {
            auto pShaderInfo = GetPipelineShaderInfo(ShaderStage(stage));
            for (const DescriptorRangeValue& rangeValue :
                        ArrayRef<DescriptorRangeValue>(pShaderInfo->pDescriptorRangeValues,
                                                       pShaderInfo->descriptorRangeValueCount))
            {
                allRangeValues.push_back(rangeValue);
            }
        }
    }

    // Sort them by set and binding, so we can spot duplicates.
    std::sort(allRangeValues.begin(),
              allRangeValues.end(),
              [](const DescriptorRangeValue& left, const DescriptorRangeValue& right)
              {
                  if (left.set != right.set)
                  {
                      return left.set < right.set;
                  }
                  return left.binding < right.binding;
              });

    if (allRangeValues.empty() == false)
    {
        // Create a new table with merged duplicates.
        m_allocDescriptorRangeValues = std::make_unique<SmallVector<DescriptorRangeValue, 8>>();
        auto &mergedRangeValues = *m_allocDescriptorRangeValues;
        ArrayRef<DescriptorRangeValue> rangeValues = allRangeValues;

        while (rangeValues.empty() == false)
        {
            // Find the next block of duplicate rangeValues.
            uint32_t duplicateCount = 1;
            for (; duplicateCount != rangeValues.size(); ++duplicateCount)
            {
                if ((rangeValues[0].set != rangeValues[duplicateCount].set) || (rangeValues[0].binding != rangeValues[duplicateCount].binding))
                {
                    break;
                }
                LLPC_ASSERT((rangeValues[0].type == rangeValues[duplicateCount].type) &&
                            "Descriptor range value merge conflict: type");
                LLPC_ASSERT((rangeValues[0].arraySize == rangeValues[duplicateCount].arraySize) &&
                            "Descriptor range value merge conflict: arraySize");
                LLPC_ASSERT((memcmp(rangeValues[0].pValue,
                                    rangeValues[duplicateCount].pValue,
                                    rangeValues[0].arraySize * sizeof(uint32_t)) == 0) &&
                            "Descriptor range value merge conflict: value");
            }

            // Keep the merged range.
            mergedRangeValues.push_back(rangeValues[0]);
            rangeValues = rangeValues.slice(duplicateCount);
        }
    }

    // Point each shader stage at the merged user data nodes and descriptor range values.
    for (uint32_t stage = 0; stage < ShaderStageNativeStageCount; ++stage)
    {
        if ((stageMask >> stage) & 1)
        {
            auto pShaderInfo = const_cast<PipelineShaderInfo*>(GetPipelineShaderInfo(ShaderStage(stage)));
            pShaderInfo->pUserDataNodes = mergedNodes.data();
            pShaderInfo->userDataNodeCount = mergedNodes.size();
            if (m_allocDescriptorRangeValues)
            {
                pShaderInfo->pDescriptorRangeValues = m_allocDescriptorRangeValues->data();
                pShaderInfo->descriptorRangeValueCount = m_allocDescriptorRangeValues->size();
            }
        }
    }
}

// =====================================================================================================================
// Merge user data nodes that have been collected into one big table
ArrayRef<ResourceMappingNode> GraphicsContext::MergeUserDataNodeTable(
    SmallVectorImpl<ResourceMappingNode>& allNodes)   // Table of nodes
{
    // Sort the nodes by offset, so we can spot duplicates.
    std::sort(allNodes.begin(),
              allNodes.end(),
              [](const ResourceMappingNode& left, const ResourceMappingNode& right)
              {
                  return left.offsetInDwords < right.offsetInDwords;
              });

    // Merge duplicates.
    m_allocUserDataNodes.push_back(std::make_unique<SmallVector<ResourceMappingNode, 8>>());
    auto& mergedNodes = *m_allocUserDataNodes.back();
    ArrayRef<ResourceMappingNode> nodes = allNodes;

    while (nodes.empty() == false)
    {
        // Find the next block of duplicate nodes.
        uint32_t duplicatesCount = 1;
        for (; duplicatesCount != nodes.size(); ++duplicatesCount)
        {
            if (nodes[0].offsetInDwords != nodes[duplicatesCount].offsetInDwords)
            {
                break;
            }
            LLPC_ASSERT((nodes[0].type == nodes[duplicatesCount].type) && "User data merge conflict: type");
            LLPC_ASSERT((nodes[0].sizeInDwords == nodes[duplicatesCount].sizeInDwords) &&
                        "User data merge conflict: size");
            LLPC_ASSERT((nodes[0].type != ResourceMappingNodeType::IndirectUserDataVaPtr) &&
                        "User data merge conflict: only one shader stage expected to have vertex buffer");
            LLPC_ASSERT((nodes[0].type != ResourceMappingNodeType::StreamOutTableVaPtr) &&
                        "User data merge conflict: only one shader stage expected to have stream out");
            if (nodes[0].type != ResourceMappingNodeType::DescriptorTableVaPtr)
            {
                LLPC_ASSERT((nodes[0].srdRange.set == nodes[duplicatesCount].srdRange.set) &&
                            (nodes[0].srdRange.binding == nodes[duplicatesCount].srdRange.binding) &&
                            "User data merge conflict: set or binding");
            }
        }

        if ((duplicatesCount == 1) || (nodes[0].type != ResourceMappingNodeType::DescriptorTableVaPtr))
        {
            // Keep the merged node.
            mergedNodes.push_back(nodes[0]);
        }
        else
        {
            // Merge the inner tables too. First collect nodes from all inner tables.
            SmallVector<ResourceMappingNode, 8> allInnerNodes;

            for (uint32_t i = 0; i != duplicatesCount; ++i)
            {
                const auto& node = nodes[0];
                ArrayRef<ResourceMappingNode> innerTable(node.tablePtr.pNext, node.tablePtr.nodeCount);
                allInnerNodes.insert(allInnerNodes.end(), innerTable.begin(), innerTable.end());
            }

            // Call recursively to sort and merge.
            auto mergedInnerNodes = MergeUserDataNodeTable(allInnerNodes);

            // Finished merging the inner tables. Keep the merged DescriptorTableVaPtr node.
            ResourceMappingNode modifiedNode = nodes[0];
            modifiedNode.tablePtr.nodeCount = mergedInnerNodes.size();
            modifiedNode.tablePtr.pNext = &mergedInnerNodes[0];
            mergedNodes.push_back(modifiedNode);
        }

        nodes = nodes.slice(duplicatesCount);
    }
    return mergedNodes;
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Sets NGG control settings
//
// NOTE: Need to be called before after LLVM preliminary patch work and LLVM main patch work.
void GraphicsContext::SetNggControl()
{
    // For GFX10+, initialize NGG control settings
    if (m_gfxIp.major < 10)
    {
        return;
    }

    const bool hasTs = ((m_stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                        ShaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((m_stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    // Check the use of cull distance for NGG primitive shader
    bool useCullDistance = false;
    bool enableXfb = false;
    if (hasGs)
    {
        const auto pResUsage = GetShaderResourceUsage(ShaderStageGeometry);
        enableXfb = pResUsage->inOutUsage.enableXfb;
    }
    else
    {
        if (hasTs)
        {
            const auto pResUsage = GetShaderResourceUsage(ShaderStageTessEval);
            const auto& builtInUsage = pResUsage->builtInUsage.tes;
            useCullDistance = (builtInUsage.cullDistance > 0);
            enableXfb = pResUsage->inOutUsage.enableXfb;
        }
        else
        {
            const auto pResUsage = GetShaderResourceUsage(ShaderStageVertex);
            const auto& builtInUsage = pResUsage->builtInUsage.vs;
            useCullDistance = (builtInUsage.cullDistance > 0);
            enableXfb = pResUsage->inOutUsage.enableXfb;
        }
    }

    const auto& nggState = m_pPipelineInfo->nggState;

    bool enableNgg = nggState.enableNgg;
    if (enableXfb)
    {
        // TODO: If transform feedback is enabled, disable NGG.
        enableNgg = false;
    }

    if (hasGs && (nggState.enableGsUse == false))
    {
        // NOTE: NGG used on GS is disabled by default
        enableNgg = false;
    }

    if (m_pGpuWorkarounds->gfx10.waNggDisabled)
    {
        enableNgg = false;
    }

    m_nggControl.enableNgg                  = enableNgg;
    m_nggControl.enableGsUse                = nggState.enableGsUse;
    m_nggControl.alwaysUsePrimShaderTable   = nggState.alwaysUsePrimShaderTable;
    m_nggControl.compactMode                = nggState.compactMode;

    m_nggControl.enableFastLaunch           = nggState.enableFastLaunch;
    m_nggControl.enableVertexReuse          = nggState.enableVertexReuse;
    m_nggControl.enableBackfaceCulling      = nggState.enableBackfaceCulling;
    m_nggControl.enableFrustumCulling       = nggState.enableFrustumCulling;
    m_nggControl.enableBoxFilterCulling     = nggState.enableBoxFilterCulling;
    m_nggControl.enableSphereCulling        = nggState.enableSphereCulling;
    m_nggControl.enableSmallPrimFilter      = nggState.enableSmallPrimFilter;
    m_nggControl.enableCullDistanceCulling  = (nggState.enableCullDistanceCulling && useCullDistance);

    m_nggControl.backfaceExponent           = nggState.backfaceExponent;
    m_nggControl.subgroupSizing             = nggState.subgroupSizing;
    m_nggControl.primsPerSubgroup           = std::min(nggState.primsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup);
    m_nggControl.vertsPerSubgroup           = std::min(nggState.vertsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup);

    if (nggState.enableNgg)
    {
        if (nggState.forceNonPassthrough)
        {
            m_nggControl.passthroughMode = false;
        }
        else
        {
            m_nggControl.passthroughMode = (m_nggControl.enableVertexReuse == false) &&
                                           (m_nggControl.enableBackfaceCulling == false) &&
                                           (m_nggControl.enableFrustumCulling == false) &&
                                           (m_nggControl.enableBoxFilterCulling == false) &&
                                           (m_nggControl.enableSphereCulling == false) &&
                                           (m_nggControl.enableSmallPrimFilter == false) &&
                                           (m_nggControl.enableCullDistanceCulling == false);
        }

        // NOTE: Further check if we have to turn on pass-through mode forcibly.
        if (m_nggControl.passthroughMode == false)
        {
            // NOTE: Further check if pass-through mode should be enabled
            const auto topology = m_pPipelineInfo->iaState.topology;
            if ((topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST) ||
                (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST)  ||
                (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) ||
                (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY) ||
                (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY))
            {
                // NGG runs in pass-through mode for non-triangle primitives
                m_nggControl.passthroughMode = true;
            }
            else if (topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
            {
                // NGG runs in pass-through mode for non-triangle tessellation output
                LLPC_ASSERT(hasTs);

                const auto& builtInUsage = GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
                if (builtInUsage.pointMode || (builtInUsage.primitiveMode == Isolines))
                {
                    m_nggControl.passthroughMode = true;
                }
            }

            const auto polygonMode = m_pPipelineInfo->rsState.polygonMode;
            if ((polygonMode == VK_POLYGON_MODE_LINE) || (polygonMode == VK_POLYGON_MODE_POINT))
            {
                // NGG runs in pass-through mode for non-fill polygon mode
                m_nggControl.passthroughMode = true;
            }

            if (hasGs)
            {
                const auto& builtInUsage = GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;
                if (builtInUsage.outputPrimitive != OutputTriangleStrip)
                {
                    // If GS output primitive type is not triangle strip, NGG runs in "pass-through"
                    // (actual no culling) mode
                    m_nggControl.passthroughMode = true;
                }
            }
        }

        // Build NGG culling-control registers
        BuildNggCullingControlRegister();

        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC NGG control settings results\n\n");

        // Control option
        LLPC_OUTS("EnableNgg                    = " << m_nggControl.enableNgg << "\n");
        LLPC_OUTS("EnableGsUse                  = " << m_nggControl.enableGsUse << "\n");
        LLPC_OUTS("AlwaysUsePrimShaderTable     = " << m_nggControl.alwaysUsePrimShaderTable << "\n");
        LLPC_OUTS("PassthroughMode              = " << m_nggControl.passthroughMode << "\n");
        LLPC_OUTS("CompactMode                  = ");
        switch (m_nggControl.compactMode)
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
        LLPC_OUTS("EnableFastLaunch             = " << m_nggControl.enableFastLaunch << "\n");
        LLPC_OUTS("EnableVertexReuse            = " << m_nggControl.enableVertexReuse << "\n");
        LLPC_OUTS("EnableBackfaceCulling        = " << m_nggControl.enableBackfaceCulling << "\n");
        LLPC_OUTS("EnableFrustumCulling         = " << m_nggControl.enableFrustumCulling << "\n");
        LLPC_OUTS("EnableBoxFilterCulling       = " << m_nggControl.enableBoxFilterCulling << "\n");
        LLPC_OUTS("EnableSphereCulling          = " << m_nggControl.enableSphereCulling << "\n");
        LLPC_OUTS("EnableSmallPrimFilter        = " << m_nggControl.enableSmallPrimFilter << "\n");
        LLPC_OUTS("EnableCullDistanceCulling    = " << m_nggControl.enableCullDistanceCulling << "\n");
        LLPC_OUTS("BackfaceExponent             = " << m_nggControl.backfaceExponent << "\n");
        LLPC_OUTS("SubgroupSizing               = ");
        switch (m_nggControl.subgroupSizing)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 26
        case NggSubgroupSizingType::Auto:
            LLPC_OUTS("Auto\n");
            break;
#endif
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
        LLPC_OUTS("PrimsPerSubgroup             = " << m_nggControl.primsPerSubgroup << "\n");
        LLPC_OUTS("VertsPerSubgroup             = " << m_nggControl.vertsPerSubgroup << "\n");
        LLPC_OUTS("\n");
    }
}

// =====================================================================================================================
// Gets WGP mode enablement for the specified shader stage
bool GraphicsContext::GetShaderWgpMode(
    ShaderStage shaderStage // Shader stage
    ) const
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);

    bool wgpMode = false;

    switch (shaderStage)
    {
    case ShaderStageVertex:
        wgpMode = m_pPipelineInfo->vs.options.wgpMode;
        break;
    case ShaderStageTessControl:
        wgpMode = m_pPipelineInfo->tcs.options.wgpMode;
        break;
    case ShaderStageTessEval:
        wgpMode = m_pPipelineInfo->tes.options.wgpMode;
        break;
    case ShaderStageGeometry:
        wgpMode = m_pPipelineInfo->gs.options.wgpMode;
        break;
    case ShaderStageFragment:
        wgpMode = m_pPipelineInfo->fs.options.wgpMode;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return wgpMode;
}
#endif

// =====================================================================================================================
// Gets the count of vertices per primitive
uint32_t GraphicsContext::GetVerticesPerPrimitive() const
{
    uint32_t vertsPerPrim = 1;

    switch (m_pPipelineInfo->iaState.topology)
    {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        vertsPerPrim = 1;
        break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
        vertsPerPrim = 2;
        break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
        vertsPerPrim = 2;
        break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        vertsPerPrim = 3;
        break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        vertsPerPrim = 3;
        break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
        vertsPerPrim = 3;
        break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
        vertsPerPrim = 4;
        break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        vertsPerPrim = 4;
        break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
        vertsPerPrim = 6;
        break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
        vertsPerPrim = 6;
        break;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        vertsPerPrim = m_pPipelineInfo->iaState.patchControlPoints;
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return vertsPerPrim;
}

// =====================================================================================================================
// Gets wave size for the specified shader stage
//
// NOTE: Need to be called after PatchResourceCollect pass, so usage of subgroupSize is confirmed.
uint32_t GraphicsContext::GetShaderWaveSize(
    ShaderStage stage)  // Shader stage
{
    if (stage == ShaderStageCopyShader)
    {
       // Treat copy shader as part of geometry shader
       stage = ShaderStageGeometry;
    }

    LLPC_ASSERT(stage < ShaderStageGfxCount);

    uint32_t waveSize = m_pGpuProperty->waveSize;

#if LLPC_BUILD_GFX10
    if (m_gfxIp.major == 10)
    {
        // NOTE: GPU property wave size is used in shader, unless:
        //  1) A stage-specific default is preferred.
        //  2) If specified by tuning option, use the specified wave size.
        //  3) If gl_SubgroupSize is used in shader, use the specified subgroup size when required.

        if (stage == ShaderStageFragment)
        {
            // Per programming guide, it's recommended to use wave64 for fragment shader.
            waveSize = 64;
        }
        else if ((m_stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0)
        {
            // NOTE: Hardware path for GS wave32 is not tested, use wave64 instead
            waveSize = 64;
        }

        switch (stage)
        {
        case ShaderStageVertex:
            if (m_pPipelineInfo->vs.options.waveSize != 0)
            {
                waveSize = m_pPipelineInfo->vs.options.waveSize;
            }
            break;
        case ShaderStageTessControl:
            if (m_pPipelineInfo->tcs.options.waveSize != 0)
            {
                waveSize = m_pPipelineInfo->tcs.options.waveSize;
            }
            break;
        case ShaderStageTessEval:
            if (m_pPipelineInfo->tes.options.waveSize != 0)
            {
                waveSize = m_pPipelineInfo->tes.options.waveSize;
            }
            break;
        case ShaderStageGeometry:
            // NOTE: For NGG, GS could be absent and VS/TES acts as part of it in the merged shader.
            // In such cases, we check the property of VS or TES.
            if ((m_stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0)
            {
                if (m_pPipelineInfo->gs.options.waveSize != 0)
                {
                    waveSize = m_pPipelineInfo->gs.options.waveSize;
                }
            }
            else if ((m_stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0)
            {
                waveSize = GetShaderWaveSize(ShaderStageTessEval);
            }
            else
            {
                waveSize = GetShaderWaveSize(ShaderStageVertex);
            }
            break;
        case ShaderStageFragment:
            if (m_pPipelineInfo->fs.options.waveSize != 0)
            {
                waveSize = m_pPipelineInfo->fs.options.waveSize;
            }
            break;
        default:
            LLPC_NEVER_CALLED();
            break;
        }

        // Check is subgroup size used in shader. If it's used, use the specified subgroup size as wave size.
        for (uint32_t i = ShaderStageVertex; i < ShaderStageGfxCount; ++i)
        {
            const PipelineShaderInfo* pShaderInfo = GetPipelineShaderInfo(static_cast<ShaderStage>(i));
            const ShaderModuleData* pModuleData =
                reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

            if ((pModuleData != nullptr) && pModuleData->moduleInfo.useSubgroupSize
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 31
             && (pShaderInfo->options.allowVaryWaveSize == false)
#endif
               )
            {
                waveSize = cl::SubgroupSize;
                break;
            }
        }

        LLPC_ASSERT((waveSize == 32) || (waveSize == 64));
    }
    else if (m_gfxIp.major > 10)
    {
        LLPC_NOT_IMPLEMENTED();
    }
#endif
    return waveSize;
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Builds NGG culling-control registers (fill part of compile-time primitive shader table).
void GraphicsContext::BuildNggCullingControlRegister()
{
    const auto& vpState = m_pPipelineInfo->vpState;
    const auto& rsState = m_pPipelineInfo->rsState;

    auto& pipelineState = m_nggControl.primShaderTable.pipelineStateCb;

    //
    // Program register PA_SU_SC_MODE_CNTL
    //
    PaSuScModeCntl paSuScModeCntl;
    paSuScModeCntl.u32All = 0;

    paSuScModeCntl.bits.POLY_OFFSET_FRONT_ENABLE = rsState.depthBiasEnable;
    paSuScModeCntl.bits.POLY_OFFSET_BACK_ENABLE  = rsState.depthBiasEnable;
    paSuScModeCntl.bits.MULTI_PRIM_IB_ENA        = true;

    paSuScModeCntl.bits.POLY_MODE            = (rsState.polygonMode != VK_POLYGON_MODE_FILL);

    if (rsState.polygonMode == VK_POLYGON_MODE_FILL)
    {
        paSuScModeCntl.bits.POLYMODE_BACK_PTYPE  = POLY_MODE_TRIANGLES;
        paSuScModeCntl.bits.POLYMODE_FRONT_PTYPE = POLY_MODE_TRIANGLES;
    }
    else if (rsState.polygonMode == VK_POLYGON_MODE_LINE)
    {
        paSuScModeCntl.bits.POLYMODE_BACK_PTYPE  = POLY_MODE_LINES;
        paSuScModeCntl.bits.POLYMODE_FRONT_PTYPE = POLY_MODE_LINES;
    }
    else if (rsState.polygonMode == VK_POLYGON_MODE_POINT)
    {
        paSuScModeCntl.bits.POLYMODE_BACK_PTYPE  = POLY_MODE_POINTS;
        paSuScModeCntl.bits.POLYMODE_FRONT_PTYPE = POLY_MODE_POINTS;
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    paSuScModeCntl.bits.CULL_FRONT = ((rsState.cullMode == VK_CULL_MODE_FRONT_BIT) ||
                                      (rsState.cullMode == VK_CULL_MODE_FRONT_AND_BACK));
    paSuScModeCntl.bits.CULL_BACK  = ((rsState.cullMode == VK_CULL_MODE_BACK_BIT) ||
                                      (rsState.cullMode == VK_CULL_MODE_FRONT_AND_BACK));

    paSuScModeCntl.bits.FACE = rsState.frontFace;

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
#endif

// =====================================================================================================================
// Determine whether pack io is valid. Current VS output and FS input in VS-FS pipeline is packable
bool GraphicsContext::CheckPackInOutValidity(
    ShaderStage shaderStage,    // Current shader stage
    bool        isOutput        // Whether it is to pack an output
    ) const
{
    // Pack in/out requirements:
    // 1) Both cl::PackInOut and m_packInOut are enabled..
    // 2) It is a VS-FS pipeline.
    // 3) It is VS' output or FS'input.
    bool isPackInOut = cl::PackInOut && m_packInOut;
    if (isPackInOut)
    {
        const uint32_t supportedMask = ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageFragment);
        isPackInOut = (GetShaderStageMask() == supportedMask);

        if (isPackInOut)
        {
            isPackInOut = (((shaderStage == ShaderStageVertex) && isOutput) ||             // It's VS' output
                           ((shaderStage == ShaderStageFragment) && (isOutput == false))); // It's FS' input
        }
    }

    return isPackInOut;
}

} // Llpc
