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
 * @file  llpcPipelineContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PipelineContext.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/CommandLine.h"

#include "SPIRVInternal.h"
#include "llpcBuilder.h"
#include "llpcCompiler.h"
#include "llpcDebug.h"
#include "llpcPipelineContext.h"
#include "llpcPipeline.h"

#define DEBUG_TYPE "llpc-pipeline-context"

namespace llvm
{

namespace cl
{

extern opt<bool> EnablePipelineDump;

} // cl

} // llvm

using namespace llvm;

// -include-llvm-ir: include LLVM IR as a separate section in the ELF binary
static cl::opt<bool> IncludeLlvmIr("include-llvm-ir",
                                   cl::desc("Include LLVM IR as a separate section in the ELF binary"),
                                   cl::init(false));

// -vgpr-limit: maximum VGPR limit for this shader
static cl::opt<uint32_t> VgprLimit("vgpr-limit", cl::desc("Maximum VGPR limit for this shader"), cl::init(0));

// -sgpr-limit: maximum SGPR limit for this shader
static cl::opt<uint32_t> SgprLimit("sgpr-limit", cl::desc("Maximum SGPR limit for this shader"), cl::init(0));

// -waves-per-eu: the maximum number of waves per EU for this shader
static cl::opt<uint32_t> WavesPerEu("waves-per-eu",
                                    cl::desc("Maximum number of waves per EU for this shader"),
                                    cl::init(0));

// -enable-load-scalarizer: Enable the optimization for load scalarizer.
static cl::opt<bool> EnableScalarLoad("enable-load-scalarizer",
                                      cl::desc("Enable the optimization for load scalarizer."),
                                      cl::init(false));

// The max threshold of load scalarizer.
static const uint32_t MaxScalarThreshold = 0xFFFFFFFF;

// -scalar-threshold: Set the vector size threshold for load scalarizer.
static cl::opt<unsigned> ScalarThreshold("scalar-threshold",
                                         cl::desc("The threshold for load scalarizer"),
                                         cl::init(MaxScalarThreshold));

// -enable-si-scheduler: enable target option si-scheduler
static cl::opt<bool> EnableSiScheduler("enable-si-scheduler",
                                       cl::desc("Enable target option si-scheduler"),
                                       cl::init(false));

// -subgroup-size: sub-group size exposed via Vulkan API.
static cl::opt<int> SubgroupSize("subgroup-size", cl::desc("Sub-group size exposed via Vulkan API"), cl::init(64));

namespace Llpc
{

// =====================================================================================================================
PipelineContext::PipelineContext(
    GfxIpVersion           gfxIp,           // Graphics IP version info
    MetroHash::Hash*       pPipelineHash,   // [in] Pipeline hash code
    MetroHash::Hash*       pCacheHash)      // [in] Cache hash code
    :
    m_gfxIp(gfxIp),
    m_pipelineHash(*pPipelineHash),
    m_cacheHash(*pCacheHash)
{

}

// =====================================================================================================================
PipelineContext::~PipelineContext()
{
}

// =====================================================================================================================
// Gets the name string of GPU target according to graphics IP version info.
void PipelineContext::GetGpuNameString(
    GfxIpVersion  gfxIp,    // Graphics IP version info
    std::string&  gpuName)  // [out] LLVM GPU name
{
    // A GfxIpVersion from PAL is three decimal numbers for major, minor and stepping. This function
    // converts that to an LLVM target name, whith is "gfx" followed by the three decimal numbers with
    // no separators, e.g. "gfx1010" for 10.1.0. A high stepping number 0xFFFA..0xFFFF denotes an
    // experimental target, and that is represented by the final hexadecimal digit, e.g. "gfx101A"
    // for 10.1.0xFFFA.
    gpuName.clear();
    raw_string_ostream gpuNameStream(gpuName);
    gpuNameStream << "gfx" << gfxIp.major << gfxIp.minor;
    if (gfxIp.stepping >= 0xFFFA)
    {
        gpuNameStream << char(gfxIp.stepping - 0xFFFA + 'A');
    }
    else
    {
        gpuNameStream << gfxIp.stepping;
    }
}

// =====================================================================================================================
// Gets the name string of the abbreviation for GPU target according to graphics IP version info.
const char* PipelineContext::GetGpuNameAbbreviation(
    GfxIpVersion gfxIp)  // Graphics IP version info
{
    const char* pNameAbbr = nullptr;
    switch (gfxIp.major)
    {
    case 6:
        pNameAbbr = "SI";
        break;
    case 7:
        pNameAbbr = "CI";
        break;
    case 8:
        pNameAbbr = "VI";
        break;
    case 9:
        pNameAbbr = "GFX9";
        break;
    default:
        pNameAbbr = "UNKNOWN";
        break;
    }

    return pNameAbbr;
}

// =====================================================================================================================
// Gets the hash code of input shader with specified shader stage.
ShaderHash PipelineContext::GetShaderHashCode(
    ShaderStage stage       // Shader stage
) const
{
    auto pShaderInfo = GetPipelineShaderInfo(stage);
    assert(pShaderInfo != nullptr);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
    if((pShaderInfo->options.clientHash.upper != 0) &&
       (pShaderInfo->options.clientHash.lower != 0))
    {
        return pShaderInfo->options.clientHash;
    }
    else
    {
        ShaderHash hash = {};
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

        if(pModuleData != nullptr)
        {
            hash.lower = MetroHash::Compact64(reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash));
            hash.upper = 0;
        }
        return hash;
    }
#else
    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

    return (pModuleData == nullptr) ? 0 :
        MetroHash::Compact64(reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash));
#endif
}

// =====================================================================================================================
// Set pipeline state in Pipeline object for middle-end
void PipelineContext::SetPipelineState(
    Pipeline*    pPipeline) const   // [in/out] Middle-end pipeline object
{
    // Give the shader stage mask to the middle-end.
    uint32_t stageMask = GetShaderStageMask();
    pPipeline->SetShaderStageMask(stageMask);

    // Give the pipeline options to the middle-end.
    SetOptionsInPipeline(pPipeline);

    // Give the user data nodes to the middle-end.
    SetUserDataInPipeline(pPipeline);

    if (IsGraphics())
    {
        // Set vertex input descriptions to the middle-end.
        SetVertexInputDescriptions(pPipeline);

        // Give the color export state to the middle-end.
        SetColorExportState(pPipeline);

        // Give the graphics pipeline state to the middle-end.
        SetGraphicsStateInPipeline(pPipeline);
    }
    else
    {
        pPipeline->SetDeviceIndex(static_cast<const ComputePipelineBuildInfo*>(GetPipelineBuildInfo())->deviceIndex);
    }
}

// =====================================================================================================================
// Give the pipeline options to the middle-end.
void PipelineContext::SetOptionsInPipeline(
    Pipeline*    pPipeline) const   // [in/out] Middle-end pipeline object
{
    Options options = {};
    options.hash[0] = GetPiplineHashCode();
    options.hash[1] = GetCacheHashCode();

    options.includeDisassembly = (cl::EnablePipelineDump || EnableOuts() || GetPipelineOptions()->includeDisassembly);
    options.reconfigWorkgroupLayout = GetPipelineOptions()->reconfigWorkgroupLayout;
    options.includeIr = (IncludeLlvmIr || GetPipelineOptions()->includeIr);

    if (IsGraphics() && (GetGfxIpVersion().major >= 10))
    {
        // Only set NGG options for a GFX10+ graphics pipeline.
        auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo());
        const auto& nggState = pPipelineInfo->nggState;
        if (nggState.enableNgg == false)
        {
            options.nggFlags |= NggFlagDisable;
        }
        else
        {
            options.nggFlags =
                  (nggState.enableGsUse ? NggFlagEnableGsUse : 0) |
                  (nggState.forceNonPassthrough ? NggFlagForceNonPassthrough : 0) |
                  (nggState.alwaysUsePrimShaderTable ? 0 : NggFlagDontAlwaysUsePrimShaderTable) |
                  (nggState.compactMode == NggCompactSubgroup ? NggFlagCompactSubgroup : 0) |
                  (nggState.enableFastLaunch ? NggFlagEnableFastLaunch : 0) |
                  (nggState.enableVertexReuse ? NggFlagEnableVertexReuse : 0) |
                  (nggState.enableBackfaceCulling ? NggFlagEnableBackfaceCulling : 0) |
                  (nggState.enableFrustumCulling ? NggFlagEnableFrustumCulling : 0) |
                  (nggState.enableBoxFilterCulling ? NggFlagEnableBoxFilterCulling : 0) |
                  (nggState.enableSphereCulling ? NggFlagEnableSphereCulling : 0) |
                  (nggState.enableSmallPrimFilter ? NggFlagEnableSmallPrimFilter : 0) |
                  (nggState.enableCullDistanceCulling ? NggFlagEnableCullDistanceCulling : 0);
            options.nggBackfaceExponent = nggState.backfaceExponent;

            // Use a static cast from Vkgc NggSubgroupSizingType to LGC NggSubgroupSizing, and static assert that
            // that is valid.
            static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::Auto) ==
                                                         NggSubgroupSizing::Auto, "mismatch");
            static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::MaximumSize) ==
                                                         NggSubgroupSizing::MaximumSize, "mismatch");
            static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::HalfSize) ==
                                                         NggSubgroupSizing::HalfSize, "mismatch");
            static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::OptimizeForVerts) ==
                                                         NggSubgroupSizing::OptimizeForVerts, "mismatch");
            static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::OptimizeForPrims) ==
                                                         NggSubgroupSizing::OptimizeForPrims, "mismatch");
            static_assert(static_cast<NggSubgroupSizing>(NggSubgroupSizingType::Explicit) ==
                                                         NggSubgroupSizing::Explicit, "mismatch");
            options.nggSubgroupSizing = static_cast<NggSubgroupSizing>(nggState.subgroupSizing);

            options.nggVertsPerSubgroup = nggState.vertsPerSubgroup;
            options.nggPrimsPerSubgroup = nggState.primsPerSubgroup;
        }
    }

    pPipeline->SetOptions(options);

    // Give the shader options (including the hash) to the middle-end.
    uint32_t stageMask = GetShaderStageMask();
    for (uint32_t stage = 0; stage <= ShaderStageCompute; ++stage)
    {
        if (stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage)))
        {
            ShaderOptions shaderOptions = {};

            ShaderHash hash = GetShaderHashCode(static_cast<ShaderStage>(stage));
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            // 128-bit hash
            shaderOptions.hash[0] = hash.lower;
            shaderOptions.hash[1] = hash.upper;
#else
            // 64-bit hash
            shaderOptions.hash[0] = hash;
#endif

            const PipelineShaderInfo* pShaderInfo = GetPipelineShaderInfo(static_cast<ShaderStage>(stage));
            shaderOptions.trapPresent = pShaderInfo->options.trapPresent;
            shaderOptions.debugMode = pShaderInfo->options.debugMode;
            shaderOptions.allowReZ = pShaderInfo->options.allowReZ;

            if ((pShaderInfo->options.vgprLimit != 0) && (pShaderInfo->options.vgprLimit != UINT_MAX))
            {
                shaderOptions.vgprLimit = pShaderInfo->options.vgprLimit;
            }
            else
            {
                shaderOptions.vgprLimit = VgprLimit;
            }

            if ((pShaderInfo->options.sgprLimit != 0) && (pShaderInfo->options.sgprLimit != UINT_MAX))
            {
                shaderOptions.sgprLimit = pShaderInfo->options.sgprLimit;
            }
            else
            {
                shaderOptions.sgprLimit = SgprLimit;
            }

            if (pShaderInfo->options.maxThreadGroupsPerComputeUnit != 0)
            {
                shaderOptions.maxThreadGroupsPerComputeUnit = pShaderInfo->options.maxThreadGroupsPerComputeUnit;
            }
            else
            {
                shaderOptions.maxThreadGroupsPerComputeUnit = WavesPerEu;
            }

            shaderOptions.waveSize = pShaderInfo->options.waveSize;
            shaderOptions.wgpMode = pShaderInfo->options.wgpMode;
            if (pShaderInfo->options.allowVaryWaveSize == false)
            {
                // allowVaryWaveSize is disabled, so use -subgroup-size (default 64) to override the wave
                // size for a shader that uses gl_SubgroupSize.
                shaderOptions.subgroupSize = SubgroupSize;
            }

            // Use a static cast from Vkgc WaveBreakSize to LGC WaveBreak, and static assert that
            // that is valid.
            static_assert(static_cast<WaveBreak>(WaveBreakSize::None) == WaveBreak::None, "mismatch");
            static_assert(static_cast<WaveBreak>(WaveBreakSize::_8x8) == WaveBreak::_8x8, "mismatch");
            static_assert(static_cast<WaveBreak>(WaveBreakSize::_16x16) == WaveBreak::_16x16, "mismatch");
            static_assert(static_cast<WaveBreak>(WaveBreakSize::_32x32) == WaveBreak::_32x32, "mismatch");
            static_assert(static_cast<WaveBreak>(WaveBreakSize::DrawTime) == WaveBreak::DrawTime, "mismatch");
            shaderOptions.waveBreakSize = static_cast<WaveBreak>(pShaderInfo->options.waveBreakSize);

            shaderOptions.loadScalarizerThreshold = 0;
            if (EnableScalarLoad)
            {
                shaderOptions.loadScalarizerThreshold = ScalarThreshold;
            }
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
            if (pShaderInfo->options.enableLoadScalarizer)
            {
                if (pShaderInfo->options.scalarThreshold != 0)
                {
                    shaderOptions.loadScalarizerThreshold = pShaderInfo->options.scalarThreshold;
                }
                else
                {
                    shaderOptions.loadScalarizerThreshold = MaxScalarThreshold;
                }
            }
#endif

            shaderOptions.useSiScheduler = EnableSiScheduler || pShaderInfo->options.useSiScheduler;
            shaderOptions.updateDescInElf = pShaderInfo->options.updateDescInElf;
            shaderOptions.unrollThreshold = pShaderInfo->options.unrollThreshold;

            pPipeline->SetShaderOptions(static_cast<ShaderStage>(stage), shaderOptions);
        }
    }
}

// =====================================================================================================================
// Give the user data nodes and descriptor range values to the middle-end.
// The user data nodes have been merged so they are the same in each shader stage. Get them from
// the first active stage.
void PipelineContext::SetUserDataInPipeline(
    Pipeline*    pPipeline) const   // [in/out] Middle-end pipeline object
{
    const PipelineShaderInfo* pShaderInfo = nullptr;
    uint32_t stageMask = GetShaderStageMask();
    {
        pShaderInfo = GetPipelineShaderInfo(ShaderStage(countTrailingZeros(stageMask)));
    }

    // Translate the resource nodes into the LGC format expected by Pipeline::SetUserDataNodes.
    ArrayRef<ResourceMappingNode> nodes(pShaderInfo->pUserDataNodes, pShaderInfo->userDataNodeCount);
    ArrayRef<DescriptorRangeValue> descriptorRangeValues(pShaderInfo->pDescriptorRangeValues,
                                                         pShaderInfo->descriptorRangeValueCount);

    // First, create a map of immutable nodes.
    ImmutableNodesMap immutableNodesMap;
    for (auto& rangeValue : descriptorRangeValues)
    {
        immutableNodesMap[{ rangeValue.set, rangeValue.binding }] = &rangeValue;
    }

    // Count how many user data nodes we have, and allocate the buffer.
    uint32_t nodeCount = nodes.size();
    for (auto& node : nodes)
    {
        if (node.type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            nodeCount += node.tablePtr.nodeCount;
        }
    }
    auto allocUserDataNodes = std::make_unique<ResourceNode[]>(nodeCount);

    // Copy nodes in.
    ResourceNode* pDestTable = allocUserDataNodes.get();
    ResourceNode* pDestInnerTable = pDestTable + nodeCount;
    auto userDataNodes = ArrayRef<ResourceNode>(pDestTable, nodes.size());
    SetUserDataNodesTable(pPipeline->GetContext(), nodes, immutableNodesMap, pDestTable, pDestInnerTable);
    assert(pDestInnerTable == pDestTable + nodes.size());

    // Give the table to the LGC Pipeline interface.
    pPipeline->SetUserDataNodes(userDataNodes);
}

// =====================================================================================================================
// Set one user data table, and its inner tables. Used by SetUserDataInPipeline above, and recursively calls
// itself for an inner table. This translates from a Vkgc ResourceMappingNode to an LGC ResourceNode.
void PipelineContext::SetUserDataNodesTable(
    LLVMContext&                  context,                // LLVM context
    ArrayRef<ResourceMappingNode> nodes,                  // The resource mapping nodes
    const ImmutableNodesMap&      immutableNodesMap,      // [in] Map of immutable nodes
    ResourceNode*                 pDestTable,             // [out] Where to write nodes
    ResourceNode*&                pDestInnerTable) const  // [in/out] End of space available for inner tables
{
    for (uint32_t idx = 0; idx != nodes.size(); ++idx)
    {
        auto& node = nodes[idx];
        auto& destNode = pDestTable[idx];

        destNode.sizeInDwords = node.sizeInDwords;
        destNode.offsetInDwords = node.offsetInDwords;

        switch (node.type)
        {
        case ResourceMappingNodeType::DescriptorTableVaPtr:
            {
                // Process an inner table.
                destNode.type = ResourceNodeType::DescriptorTableVaPtr;
                pDestInnerTable -= node.tablePtr.nodeCount;
                destNode.innerTable = ArrayRef<ResourceNode>(pDestInnerTable, node.tablePtr.nodeCount);
                SetUserDataNodesTable(context,
                                      ArrayRef<ResourceMappingNode>(node.tablePtr.pNext, node.tablePtr.nodeCount),
                                      immutableNodesMap,
                                      pDestInnerTable,
                                      pDestInnerTable);
                break;
            }
        case ResourceMappingNodeType::IndirectUserDataVaPtr:
            {
                // Process an indirect pointer.
                destNode.type = ResourceNodeType::IndirectUserDataVaPtr;
                destNode.indirectSizeInDwords = node.userDataPtr.sizeInDwords;
                break;
            }
        case ResourceMappingNodeType::StreamOutTableVaPtr:
            {
                // Process an indirect pointer.
                destNode.type = ResourceNodeType::StreamOutTableVaPtr;
                destNode.indirectSizeInDwords = node.userDataPtr.sizeInDwords;
                break;
            }
        default:
            {
                // Process an SRD. First check that a static_cast works to convert a Vkgc ResourceMappingNodeType
                // to an LGC ResourceNodeType (with the exception of DescriptorCombinedBvhBuffer, whose value
                // accidentally depends on LLPC version).
                static_assert(ResourceNodeType::DescriptorResource ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorResource),
                              "mismatch");
                static_assert(ResourceNodeType::DescriptorSampler ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorSampler),
                              "mismatch");
                static_assert(ResourceNodeType::DescriptorCombinedTexture ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorCombinedTexture),
                              "mismatch");
                static_assert(ResourceNodeType::DescriptorTexelBuffer ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorTexelBuffer),
                              "mismatch");
                static_assert(ResourceNodeType::DescriptorFmask ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorFmask),
                              "mismatch");
                static_assert(ResourceNodeType::DescriptorBuffer ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorBuffer),
                              "mismatch");
                static_assert(ResourceNodeType::PushConst ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::PushConst),
                              "mismatch");
                static_assert(ResourceNodeType::DescriptorBufferCompact ==
                              static_cast<ResourceNodeType>(ResourceMappingNodeType::DescriptorBufferCompact),
                              "mismatch");
                if (node.type == ResourceMappingNodeType::DescriptorYCbCrSampler)
                {
                    destNode.type = ResourceNodeType::DescriptorYCbCrSampler;
                }
                else
                {
                    destNode.type = static_cast<ResourceNodeType>(node.type);
                }

                destNode.set = node.srdRange.set;
                destNode.binding = node.srdRange.binding;
                destNode.pImmutableValue = nullptr;

                auto it = immutableNodesMap.find(std::pair<uint32_t, uint32_t>(destNode.set, destNode.binding));
                if (it != immutableNodesMap.end())
                {
                    // This set/binding is (or contains) an immutable value. The value can only be a sampler, so we
                    // can assume it is four dwords.
                    auto& immutableNode = *it->second;

                    IRBuilder<> builder(context);
                    SmallVector<Constant*, 8> values;

                    if (immutableNode.arraySize != 0)
                    {
                        const uint32_t samplerDescriptorSize =
                            (node.type != ResourceMappingNodeType::DescriptorYCbCrSampler) ? 4 : 8;

                        for (uint32_t compIdx = 0; compIdx < immutableNode.arraySize; ++compIdx)
                        {
                            Constant* compValues[8] = {};
                            for (uint32_t i = 0; i < samplerDescriptorSize; ++i)
                            {
                                compValues[i] =
                                    builder.getInt32(immutableNode.pValue[compIdx * samplerDescriptorSize + i]);
                            }
                            for (uint32_t i = samplerDescriptorSize; i < 8; ++i)
                            {
                                compValues[i] = builder.getInt32(0);
                            }
                            values.push_back(ConstantVector::get(compValues));
                        }
                        destNode.pImmutableValue = ConstantArray::get(ArrayType::get(values[0]->getType(),
                                                                                     values.size()),
                                                                      values);
                    }
                }
                break;
            }
        }
    }
}

// =====================================================================================================================
// Give the graphics pipeline state to the middle-end.
void PipelineContext::SetGraphicsStateInPipeline(
    Pipeline*    pPipeline   // [in/out] Middle-end pipeline object
) const
{
    const auto& inputIaState = static_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo())->iaState;
    pPipeline->SetDeviceIndex(inputIaState.deviceIndex);

    InputAssemblyState inputAssemblyState = {};
    // PrimitiveTopology happens to have the same values as the corresponding Vulkan enum.
    inputAssemblyState.topology = static_cast<PrimitiveTopology>(inputIaState.topology);
    inputAssemblyState.patchControlPoints = inputIaState.patchControlPoints;
    inputAssemblyState.disableVertexReuse = inputIaState.disableVertexReuse;
    inputAssemblyState.switchWinding = inputIaState.switchWinding;
    inputAssemblyState.enableMultiView = inputIaState.enableMultiView;

    const auto& inputVpState = static_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo())->vpState;
    ViewportState viewportState = {};
    viewportState.depthClipEnable = inputVpState.depthClipEnable;

    const auto& inputRsState = static_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo())->rsState;
    RasterizerState rasterizerState = {};
    rasterizerState.rasterizerDiscardEnable = inputRsState.rasterizerDiscardEnable;
    rasterizerState.innerCoverage = inputRsState.innerCoverage;
    rasterizerState.perSampleShading = inputRsState.perSampleShading;
    rasterizerState.numSamples = inputRsState.numSamples;
    rasterizerState.samplePatternIdx = inputRsState.samplePatternIdx;
    rasterizerState.usrClipPlaneMask = inputRsState.usrClipPlaneMask;
    // PolygonMode and CullModeFlags happen to have the same values as their Vulkan equivalents.
    rasterizerState.polygonMode = static_cast<PolygonMode>(inputRsState.polygonMode);
    rasterizerState.cullMode = static_cast<CullModeFlags>(inputRsState.cullMode);
    rasterizerState.frontFaceClockwise = (inputRsState.frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE);
    rasterizerState.depthBiasEnable = inputRsState.depthBiasEnable;

    pPipeline->SetGraphicsState(inputAssemblyState, viewportState, rasterizerState);
}

// =====================================================================================================================
// Set vertex input descriptions in middle-end Pipeline object
void PipelineContext::SetVertexInputDescriptions(
    Pipeline*   pPipeline   // [in] Pipeline object
) const
{
    auto pVertexInput = static_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo())->pVertexInput;
    if (pVertexInput == nullptr)
    {
        return;
    }

    // Gather the bindings.
    SmallVector<VertexInputDescription, 8> bindings;
    for (uint32_t i = 0; i < pVertexInput->vertexBindingDescriptionCount; ++i)
    {
        auto pBinding = &pVertexInput->pVertexBindingDescriptions[i];
        uint32_t idx = pBinding->binding;
        if (idx >= bindings.size())
        {
            bindings.resize(idx + 1);
        }
        bindings[idx].binding = pBinding->binding;
        bindings[idx].stride = pBinding->stride;
        switch (pBinding->inputRate)
        {
        case VK_VERTEX_INPUT_RATE_VERTEX:
            bindings[idx].inputRate = VertexInputRateVertex;
            break;
        case VK_VERTEX_INPUT_RATE_INSTANCE:
            bindings[idx].inputRate = VertexInputRateInstance;
            break;
        default:
            llvm_unreachable("Should never be called!");
        }
    }

    // Check for divisors.
    auto pVertexDivisor = FindVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        pVertexInput->pNext);
    if (pVertexDivisor)
    {
        for (uint32_t i = 0;i < pVertexDivisor->vertexBindingDivisorCount; ++i)
        {
            auto pDivisor = &pVertexDivisor->pVertexBindingDivisors[i];
            if (pDivisor->binding <= bindings.size())
            {
                bindings[pDivisor->binding].inputRate = pDivisor->divisor;
            }
        }
    }

    // Gather the vertex inputs.
    SmallVector<VertexInputDescription, 8> descriptions;
    for (uint32_t i = 0; i < pVertexInput->vertexAttributeDescriptionCount; ++i)
    {
        auto pAttrib = &pVertexInput->pVertexAttributeDescriptions[i];
        if (pAttrib->binding >= bindings.size())
        {
            continue;
        }
        auto pBinding = &bindings[pAttrib->binding];
        if (pBinding->binding != pAttrib->binding)
        {
            continue;
        }

        auto dfmt = BufDataFormatInvalid;
        auto nfmt = BufNumFormatUnorm;
        std::tie(dfmt, nfmt) = MapVkFormat(pAttrib->format, /*isColorExport=*/false);

        if (dfmt != BufDataFormatInvalid)
        {
            descriptions.push_back({
                                      pAttrib->location,
                                      pAttrib->binding,
                                      pAttrib->offset,
                                      pBinding->stride,
                                      dfmt,
                                      nfmt,
                                      pBinding->inputRate,
                                   });
        }
    }

    // Give the vertex input descriptions to the middle-end Pipeline object.
    pPipeline->SetVertexInputDescriptions(descriptions);
}

// =====================================================================================================================
// Set color export state in middle-end Pipeline object
void PipelineContext::SetColorExportState(
    Pipeline*          pPipeline   // [in] Pipeline object
) const
{
    const auto& cbState = static_cast<const GraphicsPipelineBuildInfo*>(GetPipelineBuildInfo())->cbState;
    ColorExportState state = {};
    SmallVector<ColorExportFormat, MaxColorTargets> formats;

    state.alphaToCoverageEnable = cbState.alphaToCoverageEnable;
    state.dualSourceBlendEnable = cbState.dualSourceBlendEnable;

    for (uint32_t targetIndex = 0; targetIndex < MaxColorTargets; ++targetIndex)
    {
        if (cbState.target[targetIndex].format != VK_FORMAT_UNDEFINED)
        {
            auto dfmt = BufDataFormatInvalid;
            auto nfmt = BufNumFormatUnorm;
            std::tie(dfmt, nfmt) = MapVkFormat(cbState.target[targetIndex].format, true);
            formats.resize(targetIndex + 1);
            formats[targetIndex].dfmt = dfmt;
            formats[targetIndex].nfmt = nfmt;
            formats[targetIndex].blendEnable = cbState.target[targetIndex].blendEnable;
            formats[targetIndex].blendSrcAlphaToColor = cbState.target[targetIndex].blendSrcAlphaToColor;
        }
    }

    pPipeline->SetColorExportState(formats, state);
}

// =====================================================================================================================
// Map a VkFormat to a {BufDataFormat, BufNumFormat}. Returns BufDataFormatInvalid if the
// VkFormat is not supported for vertex input.
std::pair<BufDataFormat, BufNumFormat> PipelineContext::MapVkFormat(
    VkFormat  format,         // Vulkan API format code
    bool      isColorExport)  // True for looking up color export format, false for vertex input format
{
    static const struct FormatEntry
    {
#ifndef NDEBUG
        VkFormat       format;
#endif
        BufDataFormat  dfmt;
        BufNumFormat   nfmt;
        uint32_t       validVertexFormat :1;
        uint32_t       validExportFormat  :1;
    }
    formatTable[] =
    {
#ifndef NDEBUG
#define INVALID_FORMAT_ENTRY(format) \
      { format, BufDataFormatInvalid, BufNumFormatUnorm, false, false }
#define VERTEX_FORMAT_ENTRY(format, dfmt, nfmt) { format, dfmt, nfmt, true, false }
#define COLOR_FORMAT_ENTRY( format, dfmt, nfmt) { format, dfmt, nfmt, false, true }
#define BOTH_FORMAT_ENTRY(format, dfmt, nfmt)   { format, dfmt, nfmt, true, true }
#else
#define INVALID_FORMAT_ENTRY(format) \
      { BufDataFormatInvalid, BufNumFormatUnorm, false, false }
#define VERTEX_FORMAT_ENTRY(format, dfmt, nfmt) { dfmt, nfmt, true, false }
#define COLOR_FORMAT_ENTRY( format, dfmt, nfmt) { dfmt, nfmt, false, true }
#define BOTH_FORMAT_ENTRY(format, dfmt, nfmt)   { dfmt, nfmt, true, true }
#endif
        INVALID_FORMAT_ENTRY( VK_FORMAT_UNDEFINED),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R4G4_UNORM_PACK8,           BufDataFormat4_4,             BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R4G4B4A4_UNORM_PACK16,      BufDataFormat4_4_4_4,         BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B4G4R4A4_UNORM_PACK16,      BufDataFormat4_4_4_4_Bgra,    BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R5G6B5_UNORM_PACK16,        BufDataFormat5_6_5,           BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B5G6R5_UNORM_PACK16,        BufDataFormat5_6_5_Bgr,       BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R5G5B5A1_UNORM_PACK16,      BufDataFormat5_6_5_1,         BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B5G5R5A1_UNORM_PACK16,      BufDataFormat5_6_5_1_Bgra,    BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_A1R5G5B5_UNORM_PACK16,      BufDataFormat1_5_6_5,         BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8_UNORM,                   BufDataFormat8,               BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8_SNORM,                   BufDataFormat8,               BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8_USCALED,                 BufDataFormat8,               BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8_SSCALED,                 BufDataFormat8,               BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8_UINT,                    BufDataFormat8,               BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8_SINT,                    BufDataFormat8,               BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8_SRGB,                    BufDataFormat8,               BufNumFormatSrgb),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8_UNORM,                 BufDataFormat8_8,             BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8_SNORM,                 BufDataFormat8_8,             BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8_USCALED,               BufDataFormat8_8,             BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8_SSCALED,               BufDataFormat8_8,             BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8_UINT,                  BufDataFormat8_8,             BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8_SINT,                  BufDataFormat8_8,             BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8_SRGB,                  BufDataFormat8_8,             BufNumFormatSrgb),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_UNORM,               BufDataFormat8_8_8,           BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_SNORM,               BufDataFormat8_8_8,           BufNumFormatSnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_USCALED,             BufDataFormat8_8_8,           BufNumFormatUscaled),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_SSCALED,             BufDataFormat8_8_8,           BufNumFormatSscaled),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_UINT,                BufDataFormat8_8_8,           BufNumFormatUint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_SINT,                BufDataFormat8_8_8,           BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8_SRGB,                BufDataFormat8_8_8,           BufNumFormatSrgb),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_UNORM,               BufDataFormat8_8_8_Bgr,       BufNumFormatUnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_SNORM,               BufDataFormat8_8_8_Bgr,       BufNumFormatSnorm),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_USCALED,             BufDataFormat8_8_8_Bgr,       BufNumFormatUscaled),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_SSCALED,             BufDataFormat8_8_8_Bgr,       BufNumFormatSscaled),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_UINT,                BufDataFormat8_8_8_Bgr,       BufNumFormatUint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_SINT,                BufDataFormat8_8_8_Bgr,       BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8_SRGB,                BufDataFormat8_8_8_Bgr,       BufNumFormatSrgb),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8B8A8_UNORM,             BufDataFormat8_8_8_8,         BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8B8A8_SNORM,             BufDataFormat8_8_8_8,         BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8B8A8_USCALED,           BufDataFormat8_8_8_8,         BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8B8A8_SSCALED,           BufDataFormat8_8_8_8,         BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8B8A8_UINT,              BufDataFormat8_8_8_8,         BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R8G8B8A8_SINT,              BufDataFormat8_8_8_8,         BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_R8G8B8A8_SRGB,              BufDataFormat8_8_8_8,         BufNumFormatSrgb),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B8G8R8A8_UNORM,             BufDataFormat8_8_8_8_Bgra,    BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B8G8R8A8_SNORM,             BufDataFormat8_8_8_8_Bgra,    BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B8G8R8A8_USCALED,           BufDataFormat8_8_8_8_Bgra,    BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B8G8R8A8_SSCALED,           BufDataFormat8_8_8_8_Bgra,    BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B8G8R8A8_UINT,              BufDataFormat8_8_8_8_Bgra,    BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B8G8R8A8_SINT,              BufDataFormat8_8_8_8_Bgra,    BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_B8G8R8A8_SRGB,              BufDataFormat8_8_8_8_Bgra,    BufNumFormatSrgb),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A8B8G8R8_UNORM_PACK32,      BufDataFormat8_8_8_8,         BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A8B8G8R8_SNORM_PACK32,      BufDataFormat8_8_8_8,         BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A8B8G8R8_USCALED_PACK32,    BufDataFormat8_8_8_8,         BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A8B8G8R8_SSCALED_PACK32,    BufDataFormat8_8_8_8,         BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A8B8G8R8_UINT_PACK32,       BufDataFormat8_8_8_8,         BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A8B8G8R8_SINT_PACK32,       BufDataFormat8_8_8_8,         BufNumFormatSint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_A8B8G8R8_SRGB_PACK32,       BufDataFormat8_8_8_8,         BufNumFormatSrgb),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2R10G10B10_UNORM_PACK32,   BufDataFormat2_10_10_10_Bgra, BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2R10G10B10_SNORM_PACK32,   BufDataFormat2_10_10_10_Bgra, BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2R10G10B10_USCALED_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2R10G10B10_SSCALED_PACK32, BufDataFormat2_10_10_10_Bgra, BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2R10G10B10_UINT_PACK32,    BufDataFormat2_10_10_10_Bgra, BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2R10G10B10_SINT_PACK32,    BufDataFormat2_10_10_10_Bgra, BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2B10G10R10_UNORM_PACK32,   BufDataFormat2_10_10_10,      BufNumFormatUnorm),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_A2B10G10R10_SNORM_PACK32,   BufDataFormat2_10_10_10,      BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2B10G10R10_USCALED_PACK32, BufDataFormat2_10_10_10,      BufNumFormatUscaled),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_A2B10G10R10_SSCALED_PACK32, BufDataFormat2_10_10_10,      BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_A2B10G10R10_UINT_PACK32,    BufDataFormat2_10_10_10,      BufNumFormatUint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_A2B10G10R10_SINT_PACK32,    BufDataFormat2_10_10_10,      BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_UNORM,                  BufDataFormat16,              BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_SNORM,                  BufDataFormat16,              BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_USCALED,                BufDataFormat16,              BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_SSCALED,                BufDataFormat16,              BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_UINT,                   BufDataFormat16,              BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_SINT,                   BufDataFormat16,              BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16_SFLOAT,                 BufDataFormat16,              BufNumFormatFloat),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_UNORM,               BufDataFormat16_16,           BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_SNORM,               BufDataFormat16_16,           BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_USCALED,             BufDataFormat16_16,           BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_SSCALED,             BufDataFormat16_16,           BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_UINT,                BufDataFormat16_16,           BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_SINT,                BufDataFormat16_16,           BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16_SFLOAT,              BufDataFormat16_16,           BufNumFormatFloat),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_UNORM),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_SNORM),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_USCALED),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_SSCALED),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_UINT),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_SINT),
        INVALID_FORMAT_ENTRY( VK_FORMAT_R16G16B16_SFLOAT),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_UNORM,         BufDataFormat16_16_16_16,     BufNumFormatUnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_SNORM,         BufDataFormat16_16_16_16,     BufNumFormatSnorm),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_USCALED,       BufDataFormat16_16_16_16,     BufNumFormatUscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_SSCALED,       BufDataFormat16_16_16_16,     BufNumFormatSscaled),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_UINT,          BufDataFormat16_16_16_16,     BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_SINT,          BufDataFormat16_16_16_16,     BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R16G16B16A16_SFLOAT,        BufDataFormat16_16_16_16,     BufNumFormatFloat),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32_UINT,                   BufDataFormat32,              BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32_SINT,                   BufDataFormat32,              BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32_SFLOAT,                 BufDataFormat32,              BufNumFormatFloat),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32_UINT,                BufDataFormat32_32,           BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32_SINT,                BufDataFormat32_32,           BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32_SFLOAT,              BufDataFormat32_32,           BufNumFormatFloat),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32B32_UINT,             BufDataFormat32_32_32,        BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32B32_SINT,             BufDataFormat32_32_32,        BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32B32_SFLOAT,           BufDataFormat32_32_32,        BufNumFormatFloat),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32B32A32_UINT,          BufDataFormat32_32_32_32,     BufNumFormatUint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32B32A32_SINT,          BufDataFormat32_32_32_32,     BufNumFormatSint),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_R32G32B32A32_SFLOAT,        BufDataFormat32_32_32_32,     BufNumFormatFloat),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64_UINT,                   BufDataFormat64,              BufNumFormatUint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64_SINT,                   BufDataFormat64,              BufNumFormatSint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64_SFLOAT,                 BufDataFormat64,              BufNumFormatFloat),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64_UINT,                BufDataFormat64_64,           BufNumFormatUint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64_SINT,                BufDataFormat64_64,           BufNumFormatSint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64_SFLOAT,              BufDataFormat64_64,           BufNumFormatFloat),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64B64_UINT,             BufDataFormat64_64_64,        BufNumFormatUint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64B64_SINT,             BufDataFormat64_64_64,        BufNumFormatSint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64B64_SFLOAT,           BufDataFormat64_64_64,        BufNumFormatFloat),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64B64A64_UINT,          BufDataFormat64_64_64_64,     BufNumFormatUint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64B64A64_SINT,          BufDataFormat64_64_64_64,     BufNumFormatSint),
        VERTEX_FORMAT_ENTRY(  VK_FORMAT_R64G64B64A64_SFLOAT,        BufDataFormat64_64_64_64,     BufNumFormatFloat),
        BOTH_FORMAT_ENTRY(    VK_FORMAT_B10G11R11_UFLOAT_PACK32,    BufDataFormat10_11_11,        BufNumFormatFloat),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,     BufDataFormat5_9_9_9,         BufNumFormatFloat),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_D16_UNORM,                  BufDataFormat16,              BufNumFormatUnorm),
        INVALID_FORMAT_ENTRY( VK_FORMAT_X8_D24_UNORM_PACK32),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_D32_SFLOAT,                 BufDataFormat32,              BufNumFormatFloat),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_S8_UINT,                    BufDataFormat8,               BufNumFormatUint),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_D16_UNORM_S8_UINT,          BufDataFormat16,              BufNumFormatFloat),
        INVALID_FORMAT_ENTRY( VK_FORMAT_D24_UNORM_S8_UINT),
        COLOR_FORMAT_ENTRY(   VK_FORMAT_D32_SFLOAT_S8_UINT,         BufDataFormat32,              BufNumFormatFloat),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC1_RGB_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC1_RGB_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC2_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC2_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC3_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC3_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC4_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC4_SNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC5_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC5_SNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC6H_UFLOAT_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC6H_SFLOAT_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC7_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_BC7_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_EAC_R11_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_EAC_R11_SNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_EAC_R11G11_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_EAC_R11G11_SNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_4x4_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_4x4_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_5x4_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_5x4_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_5x5_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_5x5_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_6x5_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_6x5_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_6x6_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_6x6_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_8x5_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_8x5_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_8x6_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_8x6_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_8x8_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_8x8_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x5_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x5_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x6_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x6_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x8_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x8_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x10_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_10x10_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_12x10_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_12x10_SRGB_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_12x12_UNORM_BLOCK),
        INVALID_FORMAT_ENTRY( VK_FORMAT_ASTC_12x12_SRGB_BLOCK),
    };

    BufDataFormat dfmt = BufDataFormatInvalid;
    BufNumFormat nfmt = BufNumFormatUnorm;
    if (format < ArrayRef<FormatEntry>(formatTable).size())
    {
        assert(format == formatTable[format].format);
        if ((isColorExport && formatTable[format].validExportFormat) ||
            ((isColorExport == false) && formatTable[format].validVertexFormat))
        {
            dfmt = formatTable[format].dfmt;
            nfmt = formatTable[format].nfmt;
        }
    }
    return { dfmt, nfmt };
}

} // Llpc
