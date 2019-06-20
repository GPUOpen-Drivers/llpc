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
    m_gsOnChip(false)
{
    if (gfxIp.major >= 9)
    {
        // For GFX9+, always enable tessellation off-chip mode
        m_tessOffchip = true;
    }

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
        InitShaderResourceUsage(static_cast<ShaderStage>(stage));
        InitShaderInterfaceData(static_cast<ShaderStage>(stage));
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

    auto pEsResUsage = GetShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);
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

        const uint32_t esGsRingItemSize = 4 * std::max(1u, pEsResUsage->inOutUsage.outputMapLocCount);
        const uint32_t gsInstanceCount  = pGsResUsage->builtInUsage.gs.invocations;
        const uint32_t gsVsRingItemSize = 4 * std::max(1u,
                                                       (pGsResUsage->inOutUsage.outputMapLocCount *
                                                        pGsResUsage->builtInUsage.gs.outputVertices));

        uint32_t esGsRingItemSizeOnChip = esGsRingItemSize;
        uint32_t gsVsRingItemSizeOnChip = gsVsRingItemSize;

        // optimize ES -> GS ring and GS -> VS ring layout for bank conflicts
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
        {
            uint32_t ldsSizeDwordGranularity = static_cast<uint32_t>(1 << m_pGpuProperty->ldsSizeDwordGranularityShift);

            // gsPrimsPerSubgroup shouldn't be bigger than wave size.
            uint32_t gsPrimsPerSubgroup = std::min(m_pGpuProperty->gsOnChipDefaultPrimsPerSubgroup,
                                                   GetShaderWaveSize(ShaderStageGeometry));

            // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
            const uint32_t esGsRingItemSize = (4 * std::max(1u, pEsResUsage->inOutUsage.outputMapLocCount)) | 1;

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
    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
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
    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
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
        m_allocDescriptorRangeValues = make_unique<SmallVector<DescriptorRangeValue, 8>>();
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
    for (uint32_t stage = 0; stage < ShaderStageCount; ++stage)
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
    m_allocUserDataNodes.push_back(make_unique<SmallVector<ResourceMappingNode, 8>>());
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

// =====================================================================================================================
// Gets float control settings of the specified shader stage for the provide floating-point type.
FloatControl GraphicsContext::GetShaderFloatControl(
    ShaderStage shaderStage,    // Shader stage
    uint32_t    bitWidth        // Bit width of the floating-point type
    ) const
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    LLPC_ASSERT(shaderStage < ShaderStageGfxCount);

    FloatControl floatControl = {};
    const auto& commonUsage = m_resUsages[shaderStage].builtInUsage.common;

    switch (bitWidth)
    {
    case 16:
        floatControl.denormPerserve             = ((commonUsage.denormPerserve & SPIRVTW_16Bit) != 0);
        floatControl.denormFlushToZero          = ((commonUsage.denormFlushToZero & SPIRVTW_16Bit) != 0);
        floatControl.signedZeroInfNanPreserve   = ((commonUsage.signedZeroInfNanPreserve & SPIRVTW_16Bit) != 0);
        floatControl.roundingModeRTE            = ((commonUsage.roundingModeRTE & SPIRVTW_16Bit) != 0);
        floatControl.roundingModeRTZ            = ((commonUsage.roundingModeRTZ & SPIRVTW_16Bit) != 0);
        break;
    case 32:
        floatControl.denormPerserve             = ((commonUsage.denormPerserve & SPIRVTW_32Bit) != 0);
        floatControl.denormFlushToZero          = ((commonUsage.denormFlushToZero & SPIRVTW_32Bit) != 0);
        floatControl.signedZeroInfNanPreserve   = ((commonUsage.signedZeroInfNanPreserve & SPIRVTW_32Bit) != 0);
        floatControl.roundingModeRTE            = ((commonUsage.roundingModeRTE & SPIRVTW_32Bit) != 0);
        floatControl.roundingModeRTZ            = ((commonUsage.roundingModeRTZ & SPIRVTW_32Bit) != 0);
        break;
    case 64:
        floatControl.denormPerserve             = ((commonUsage.denormPerserve & SPIRVTW_64Bit) != 0);
        floatControl.denormFlushToZero          = ((commonUsage.denormFlushToZero & SPIRVTW_64Bit) != 0);
        floatControl.signedZeroInfNanPreserve   = ((commonUsage.signedZeroInfNanPreserve & SPIRVTW_64Bit) != 0);
        floatControl.roundingModeRTE            = ((commonUsage.roundingModeRTE & SPIRVTW_64Bit) != 0);
        floatControl.roundingModeRTZ            = ((commonUsage.roundingModeRTZ & SPIRVTW_64Bit) != 0);
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return floatControl;
}

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

    return waveSize;
}

} // Llpc
