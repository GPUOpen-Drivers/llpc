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

// -pack-in-out: pack input/output
opt<bool> PackInOut("pack-in-out", desc("Pack input/output"), init(false));

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
    m_gsOnChip(false),
    m_packInOut(cl::PackInOut)
{
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

            if ((pModuleData != nullptr) && pModuleData->usage.useSubgroupSize
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

// =====================================================================================================================
// Determine whether the requirements of packing input/output is satisfied
bool GraphicsContext::CanPackInOut(
    ShaderStage shaderStage,    // Current shader stage
    bool        isOutput        // Whether it is to pack an output
    ) const
{
    // Pack input/output requirements:
    // 1) Both cl::PackInOut and m_packInOut are enabled.
    // 2) It is a XX-FS pipeline.
    // 3) It is XX' output or FS'input.
    bool canPackInOut = cl::PackInOut && m_packInOut;
    if (canPackInOut)
    {
        const uint32_t validStageMask = ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageFragment);
        canPackInOut = (GetShaderStageMask() == validStageMask);

        if (canPackInOut)
        {
            canPackInOut = (((shaderStage == ShaderStageVertex) && isOutput) ||             // It's XX' output
                           ((shaderStage == ShaderStageFragment) && (isOutput == false))); // It's FS' input
        }
    }

    return canPackInOut;
}

} // Llpc
