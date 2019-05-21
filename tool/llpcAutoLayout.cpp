/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcAutoLayout.cpp
 * @brief LLPC source file: auto layout of pipeline state when compiling a single shader with AMDLLPC
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
    // NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
    #define NOMINMAX
#endif

#include "amdllpc.h"

#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVModule.h"
#include "SPIRVType.h"

#include "vfx.h"

#include "llpcDebug.h"
#include "llpcInternal.h"

#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-auto-layout"

using namespace llvm;
using namespace Llpc;
using namespace SPIRV;

struct ResourceNodeSet
{
    std::vector<ResourceMappingNode> nodes;   // Vector of resource mapping nodes
    std::map<uint32_t, uint32_t> bindingMap;  // Map from binding to index in nodes vector
};

// -auto-layout-desc: automatically create descriptor layout based on resource usages
//
// NOTE: This option is deprecated and will be ignored, and is present only for compatibility.
static cl::opt<bool> AutoLayoutDesc("auto-layout-desc",
                                    cl::desc("Automatically create descriptor layout based on resource usages"));

// =====================================================================================================================
// Get the storage size in bytes of a SPIR-V type.
// This does not need to be completely accurate, as it is only used to fake up a push constant user data node.
static uint32_t GetTypeDataSize(
    const SPIRVType*  pTy)  // [in] Type to determine the data size of
{
    switch (pTy->getOpCode())
    {
    case OpTypeVector:
        return GetTypeDataSize(pTy->getVectorComponentType()) * pTy->getVectorComponentCount();
    case OpTypeMatrix:
        return GetTypeDataSize(pTy->getMatrixColumnType()) * pTy->getMatrixColumnCount();
    case OpTypeArray:
        return GetTypeDataSize(pTy->getArrayElementType()) * pTy->getArrayLength();
    case OpTypeStruct:
        {
            uint32_t totalSize = 0;
            for (uint32_t memberIdx = 0; memberIdx < pTy->getStructMemberCount(); ++memberIdx)
            {
                totalSize += GetTypeDataSize(pTy->getStructMemberType(memberIdx));
            }
            return totalSize;
        }
    default:
        return (pTy->getBitWidth() + 7) / 8;
    }
}

// =====================================================================================================================
// Lay out dummy descriptors and other information for one shader stage. This is used when running amdllpc on a single
// SPIR-V or GLSL shader, rather than on a .pipe file. Memory allocated here may be leaked, but that does not
// matter because we are running a short-lived command-line utility.
void DoAutoLayoutDesc(
    ShaderStage                 shaderStage,    // Shader stage
    BinaryData                  spirvBin,       // SPIR-V binary
    GraphicsPipelineBuildInfo*  pPipelineInfo,  // [in/out] Graphics pipeline info, will have dummy information filled
                                                //   in. nullptr if not a graphics pipeline.
    PipelineShaderInfo*         pShaderInfo,    // [in/out] Shader info, will have user data nodes added to it
    uint32_t&                   topLevelOffset) // [in/out] User data offset; ensures that multiple shader stages use
                                                //    disjoint offsets
{
    // Read the SPIR-V.
    std::string spirvCode(static_cast<const char*>(spirvBin.pCode), spirvBin.codeSize);
    std::istringstream spirvStream(spirvCode);
    std::unique_ptr<SPIRVModule> module(SPIRVModule::createSPIRVModule());
    spirvStream >> *module;

    // Find the entry target.
    SPIRVEntryPoint* pEntryPoint = nullptr;
    SPIRVFunction* pFunc = nullptr;
    for (uint32_t i = 0, funcCount = module->getNumFunctions(); i < funcCount; ++i)
    {
        pFunc = module->getFunction(i);
        pEntryPoint = module->getEntryPoint(pFunc->getId());
        if ((pEntryPoint != nullptr) &&
            (pEntryPoint->getExecModel() == SPIRVExecutionModelKind(shaderStage)) &&
            (pEntryPoint->getName() == pShaderInfo->pEntryTarget))
        {
            break;
        }
        pFunc = nullptr;
    }
    if (pEntryPoint == nullptr)
    {
        return;
    }

    // Shader stage specific processing
    auto inOuts = pEntryPoint->getInOuts();
    if (shaderStage == ShaderStageVertex)
    {
        // Create dummy vertex info
        auto pVertexBindings = new std::vector<VkVertexInputBindingDescription>;
        auto pVertexAttribs = new std::vector<VkVertexInputAttributeDescription>;

        for (auto varId : ArrayRef<SPIRVWord>(inOuts.first, inOuts.second))
        {
            auto pVar = static_cast<SPIRVVariable*>(module->getValue(varId));
            if (pVar->getStorageClass() == StorageClassInput)
            {
                SPIRVWord location = SPIRVID_INVALID;
                if (pVar->hasDecorate(DecorationLocation, 0, &location))
                {
                    auto pVarElemTy = pVar->getType()->getPointerElementType();
                    if (pVarElemTy->getOpCode() == OpTypeArray)
                    {
                        pVarElemTy = pVarElemTy->getArrayElementType();
                    }

                    if (pVarElemTy->getOpCode() == OpTypeMatrix)
                    {
                        pVarElemTy = pVarElemTy->getMatrixColumnType();
                    }

                    if (pVarElemTy->getOpCode() == OpTypeVector)
                    {
                        pVarElemTy = pVarElemTy->getVectorComponentType();
                    }

                    VkFormat format = VK_FORMAT_UNDEFINED;
                    switch (pVarElemTy->getOpCode())
                    {
                    case OpTypeInt:
                        {
                            bool isSigned = reinterpret_cast<SPIRVTypeInt*>(pVarElemTy)->isSigned();
                            switch (pVarElemTy->getIntegerBitWidth())
                            {
                            case 8:
                                format = isSigned ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
                                break;
                            case 16:
                                format = isSigned ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
                                break;
                            case 32:
                                format = isSigned ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
                                break;
                            case 64:
                                format = isSigned ? VK_FORMAT_R64G64B64A64_SINT : VK_FORMAT_R64G64B64A64_UINT;
                                break;
                            }
                            break;
                        }
                    case OpTypeFloat:
                        {
                            switch (pVarElemTy->getFloatBitWidth())
                            {
                            case 16:
                                format = VK_FORMAT_R16G16B16A16_SFLOAT;
                                break;
                            case 32:
                                format = VK_FORMAT_R32G32B32A32_SFLOAT;
                                break;
                            case 64:
                                format = VK_FORMAT_R64G64_SFLOAT;
                                break;
                            }
                            break;
                        }
                    default:
                        {
                            break;
                        }
                    }
                    LLPC_ASSERT(format != VK_FORMAT_UNDEFINED);

                    VkVertexInputBindingDescription vertexBinding = {};
                    vertexBinding.binding   = location;
                    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                    vertexBinding.stride = SizeOfVec4;

                    VkVertexInputAttributeDescription vertexAttrib = {};
                    vertexAttrib.binding    = location;
                    vertexAttrib.location   = location;
                    vertexAttrib.offset     = 0;
                    vertexAttrib.format     = format;

                    pVertexBindings->push_back(vertexBinding);
                    pVertexAttribs->push_back(vertexAttrib);
                }
            }
        }

        auto pVertexInputState = new VkPipelineVertexInputStateCreateInfo;
        pPipelineInfo->pVertexInput = pVertexInputState;
        pVertexInputState->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        pVertexInputState->pNext = nullptr;
        pVertexInputState->vertexBindingDescriptionCount = pVertexBindings->size();
        pVertexInputState->pVertexBindingDescriptions = pVertexBindings->data();
        pVertexInputState->vertexAttributeDescriptionCount = pVertexAttribs->size();
        pVertexInputState->pVertexAttributeDescriptions = pVertexAttribs->data();

        // Set primitive topology
        pPipelineInfo->iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    else if ((shaderStage == ShaderStageTessControl) || (shaderStage == ShaderStageTessEval))
    {
        // Set primitive topology and patch control points
        pPipelineInfo->iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pPipelineInfo->iaState.patchControlPoints = 3;
    }
    else if (shaderStage == ShaderStageGeometry)
    {
        // Set primitive topology
        auto topology = VkPrimitiveTopology(0);
        if (pFunc->getExecutionMode(ExecutionModeInputPoints))
        {
            topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        }
        else if (pFunc->getExecutionMode(ExecutionModeInputLines))
        {
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        }
        else if (pFunc->getExecutionMode(ExecutionModeInputLinesAdjacency))
        {
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        }
        else if (pFunc->getExecutionMode(ExecutionModeTriangles))
        {
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
        else if (pFunc->getExecutionMode(ExecutionModeInputTrianglesAdjacency))
        {
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        }
        else
        {
            LLPC_NEVER_CALLED();
        }
        pPipelineInfo->iaState.topology = topology;
    }
    else if (shaderStage == ShaderStageFragment)
    {
        // Set dummy color formats for fragment outputs
        for (auto varId : ArrayRef<SPIRVWord>(inOuts.first, inOuts.second))
        {
            auto pVar = static_cast<SPIRVVariable*>(module->getValue(varId));
            if (pVar->getStorageClass() != StorageClassOutput)
            {
                continue;
            }

            SPIRVWord location = SPIRVID_INVALID;
            if (pVar->hasDecorate(DecorationLocation, 0, &location) == false)
            {
                continue;
            }

            SPIRVType* pVarElemTy = pVar->getType()->getPointerElementType();
            uint32_t elemCount = 1;
            if (pVarElemTy->getOpCode() == OpTypeVector)
            {
                elemCount = pVarElemTy->getVectorComponentCount();
                pVarElemTy = pVarElemTy->getVectorComponentType();
            }
            static const VkFormat undefinedFormatTable[] =
            {
                VK_FORMAT_UNDEFINED,
                VK_FORMAT_UNDEFINED,
                VK_FORMAT_UNDEFINED,
                VK_FORMAT_UNDEFINED,
            };
            const VkFormat* pFormatTable = undefinedFormatTable;

            switch (pVarElemTy->getOpCode())
            {
            case OpTypeInt:
                {
                    switch (pVarElemTy->getIntegerBitWidth())
                    {
                    case 8:
                        {
                            if (reinterpret_cast<SPIRVTypeInt*>(pVarElemTy)->isSigned())
                            {
                                static const VkFormat formatTable[] =
                                {
                                    VK_FORMAT_R8_SINT,
                                    VK_FORMAT_R8G8_SINT,
                                    VK_FORMAT_R8G8B8_SINT,
                                    VK_FORMAT_R8G8B8A8_SINT,
                                };
                                pFormatTable = formatTable;
                            }
                            else
                            {
                                static const VkFormat formatTable[] =
                                {
                                    VK_FORMAT_R8_UINT,
                                    VK_FORMAT_R8G8_UINT,
                                    VK_FORMAT_R8G8B8_UINT,
                                    VK_FORMAT_R8G8B8A8_UINT,
                                };
                                pFormatTable = formatTable;
                            }
                            break;
                        }
                    case 16:
                        {
                            if (reinterpret_cast<SPIRVTypeInt*>(pVarElemTy)->isSigned())
                            {
                                static const VkFormat formatTable[] =
                                {
                                    VK_FORMAT_R16_SINT,
                                    VK_FORMAT_R16G16_SINT,
                                    VK_FORMAT_R16G16B16_SINT,
                                    VK_FORMAT_R16G16B16A16_SINT,
                                };
                                pFormatTable = formatTable;
                            }
                            else
                            {
                                static const VkFormat formatTable[] =
                                {
                                    VK_FORMAT_R16_UINT,
                                    VK_FORMAT_R16G16_UINT,
                                    VK_FORMAT_R16G16B16_UINT,
                                    VK_FORMAT_R16G16B16A16_UINT,
                                };
                                pFormatTable = formatTable;
                            }
                            break;
                        }
                    case 32:
                        {
                            if (reinterpret_cast<SPIRVTypeInt*>(pVarElemTy)->isSigned())
                            {
                                static const VkFormat formatTable[] =
                                {
                                    VK_FORMAT_R32_SINT,
                                    VK_FORMAT_R32G32_SINT,
                                    VK_FORMAT_R32G32B32_SINT,
                                    VK_FORMAT_R32G32B32A32_SINT,
                                };
                                pFormatTable = formatTable;
                            }
                            else
                            {
                                static const VkFormat formatTable[] =
                                {
                                    VK_FORMAT_R32_UINT,
                                    VK_FORMAT_R32G32_UINT,
                                    VK_FORMAT_R32G32B32_UINT,
                                    VK_FORMAT_R32G32B32A32_UINT,
                                };
                                pFormatTable = formatTable;
                            }
                            break;
                        }
                    }
                    break;
                }

            case OpTypeFloat:
                {
                    switch (pVarElemTy->getFloatBitWidth())
                    {
                    case 16:
                        {
                            static const VkFormat formatTable[] =
                            {
                                VK_FORMAT_R16_SFLOAT,
                                VK_FORMAT_R16G16_SFLOAT,
                                VK_FORMAT_R16G16B16_SFLOAT,
                                VK_FORMAT_R16G16B16A16_SFLOAT,
                            };
                            pFormatTable = formatTable;
                        }
                        break;
                    case 32:
                        {
                            static const VkFormat formatTable[] =
                            {
                                VK_FORMAT_R32_SFLOAT,
                                VK_FORMAT_R32G32_SFLOAT,
                                VK_FORMAT_R32G32B32_SFLOAT,
                                VK_FORMAT_R32G32B32A32_SFLOAT,
                            };
                            pFormatTable = formatTable;
                        }
                        break;
                    }
                    break;
                }

            default:
                {
                    break;
                }
            }

            LLPC_ASSERT(elemCount <= 4);
            VkFormat format = pFormatTable[elemCount - 1];
            LLPC_ASSERT(format != VK_FORMAT_UNDEFINED);

            LLPC_ASSERT(location < MaxColorTargets);
            auto pColorTarget = &pPipelineInfo->cbState.target[location];
            pColorTarget->format = format;
            pColorTarget->channelWriteMask = (1U << elemCount) - 1;
        }
    }

    // Collect ResourceMappingNode entries in sets.
    std::map<uint32_t, ResourceNodeSet> resNodeSets;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> bindingIndex;
    uint32_t pushConstSize = 0;
    for (uint32_t i = 0, varCount = module->getNumVariables(); i < varCount; ++i)
    {
        auto pVar = module->getVariable(i);
        switch (pVar->getStorageClass())
        {
        case StorageClassFunction:
            {
                break;
            }

        case StorageClassPushConstant:
            {
                // Push constant: Get the size of the data and add to the total.
                auto pVarElemTy = pVar->getType()->getPointerElementType();
                pushConstSize += (GetTypeDataSize(pVarElemTy) + 3) / 4;
                break;
            }

        default:
            {
                SPIRVWord binding = SPIRVID_INVALID;
                SPIRVWord descSet = 0;
                if (pVar->hasDecorate(DecorationBinding, 0, &binding))
                {
                    // Test shaderdb/OpDecorationGroup_TestGroupAndGroupMember_lit.spvas
                    // defines a variable with a binding but no set. Handle that case.
                    pVar->hasDecorate(DecorationDescriptorSet, 0, &descSet);

                    // Find/create the node entry for this set and binding.
                    ResourceNodeSet& resNodeSet = resNodeSets[descSet];
                    auto iteratorAndCreated = resNodeSet.bindingMap.insert({binding, resNodeSet.nodes.size()});
                    uint32_t nodesIndex = iteratorAndCreated.first->second;
                    if (iteratorAndCreated.second)
                    {
                        resNodeSet.nodes.push_back({});
                        resNodeSet.nodes.back().type = ResourceMappingNodeType::Unknown;
                    }
                    ResourceMappingNode* pNode = &resNodeSet.nodes[nodesIndex];

                    // Get the element type and array size.
                    auto pVarElemTy = pVar->getType()->getPointerElementType();
                    uint32_t arraySize = 1;
                    while (pVarElemTy->isTypeArray())
                    {
                        arraySize *= pVarElemTy->getArrayLength();
                        pVarElemTy = pVarElemTy->getArrayElementType();
                    }

                    // Map the SPIR-V opcode to descriptor type and size.
                    ResourceMappingNodeType nodeType;
                    uint32_t sizeInDwords;
                    switch (pVarElemTy->getOpCode())
                    {
                    case OpTypeSampler:
                        {
                            // Sampler descriptor.
                            nodeType = ResourceMappingNodeType::DescriptorSampler;
                            sizeInDwords = 4 * arraySize;
                            break;
                        }
                    case OpTypeImage:
                        {
                            // Image descriptor.
                            auto pImageType = static_cast<SPIRVTypeImage*>(pVarElemTy);
                            nodeType = (pImageType->getDescriptor().Dim == spv::DimBuffer) ?
                                ResourceMappingNodeType::DescriptorTexelBuffer :
                                ResourceMappingNodeType::DescriptorResource;
                            sizeInDwords = 8 * arraySize;
                            break;
                        }
                    case OpTypeSampledImage:
                        {
                            // Combined image and sampler descriptors.
                            nodeType = ResourceMappingNodeType::DescriptorCombinedTexture;
                            sizeInDwords = 12 * arraySize;
                            break;
                        }
                    default:
                        {
                            // Normal buffer.
                            nodeType = ResourceMappingNodeType::DescriptorBuffer;
                            sizeInDwords = 4 * arraySize;
                            break;
                        }
                    }

                    // Check if the node already had a different type set. A DescriptorResource/DescriptorTexelBuffer
                    // and a DescriptorSampler can use the same set/binding, in which case it is
                    // DescriptorCombinedTexture.
                    if (pNode->type == ResourceMappingNodeType::Unknown)
                    {
                        pNode->type = nodeType;
                    }
                    else if (pNode->type != nodeType)
                    {
                        LLPC_ASSERT(((nodeType == ResourceMappingNodeType::DescriptorCombinedTexture) ||
                                     (nodeType == ResourceMappingNodeType::DescriptorResource) ||
                                     (nodeType == ResourceMappingNodeType::DescriptorTexelBuffer) ||
                                     (nodeType == ResourceMappingNodeType::DescriptorSampler)) &&
                                    ((pNode->type == ResourceMappingNodeType::DescriptorCombinedTexture) ||
                                     (pNode->type == ResourceMappingNodeType::DescriptorResource) ||
                                     (pNode->type == ResourceMappingNodeType::DescriptorTexelBuffer) ||
                                     (pNode->type == ResourceMappingNodeType::DescriptorSampler)));
                        pNode->type = ResourceMappingNodeType::DescriptorCombinedTexture;
                        sizeInDwords = 12 * arraySize;
                    }

                    // Fill out the rest of the node.
                    pNode->sizeInDwords = sizeInDwords;
                    pNode->srdRange.set = descSet;
                    pNode->srdRange.binding = binding;
                }
                break;
            }
        }
    }

    // Allocate dword offset to each node.
    for (auto& it : resNodeSets)
    {
        ResourceNodeSet& resNodeSet = it.second;
        uint32_t offsetInDwords = 0;
        for (auto& node : resNodeSet.nodes)
        {
            node.offsetInDwords = offsetInDwords;
            offsetInDwords += node.sizeInDwords;
        }
    }

    // Add up how much memory we need and allocate it.
    uint32_t topLevelCount = resNodeSets.size();
    topLevelCount += 3; // Allow one for push consts, one for XFB and one for vertex buffer.
    uint32_t resNodeCount = topLevelCount;
    for (const auto& resNodeSet : resNodeSets)
    {
        resNodeCount += resNodeSet.second.nodes.size();
    }
    auto pResNodes = new ResourceMappingNode[resNodeCount];
    auto pNextTable = pResNodes + topLevelCount;
    auto pResNode = pResNodes;

    // Add a node for each set.
    for (const auto& resNodeSet : resNodeSets)
    {
        pResNode->type = ResourceMappingNodeType::DescriptorTableVaPtr;
        pResNode->sizeInDwords = 1;
        pResNode->offsetInDwords = topLevelOffset;
        topLevelOffset += pResNode->sizeInDwords;
        pResNode->tablePtr.nodeCount = resNodeSet.second.nodes.size();
        pResNode->tablePtr.pNext = pNextTable;
        for (auto& resNode : resNodeSet.second.nodes)
        {
            *pNextTable++ = resNode;
        }
        ++pResNode;
    }

    if (shaderStage == ShaderStageVertex)
    {
        // Add a node for vertex buffer.
        pResNode->type = ResourceMappingNodeType::IndirectUserDataVaPtr;
        pResNode->sizeInDwords = 1;
        pResNode->offsetInDwords = topLevelOffset;
        topLevelOffset += pResNode->sizeInDwords;
        pResNode->userDataPtr.sizeInDwords = 256;
        ++pResNode;
    }

    if ((shaderStage == ShaderStageVertex) || (shaderStage == ShaderStageTessEval) || (shaderStage == ShaderStageGeometry))
    {
        // Add a node for XFB.
        pResNode->type = ResourceMappingNodeType::StreamOutTableVaPtr;
        pResNode->sizeInDwords = 1;
        pResNode->offsetInDwords = topLevelOffset;
        topLevelOffset += pResNode->sizeInDwords;
        ++pResNode;
    }

    if (pushConstSize != 0)
    {
        // Add a node for push consts.
        pResNode->type = ResourceMappingNodeType::PushConst;
        pResNode->sizeInDwords = pushConstSize;
        pResNode->offsetInDwords = topLevelOffset;
        topLevelOffset += pResNode->sizeInDwords;
        ++pResNode;
    }

    LLPC_ASSERT(pResNode - pResNodes <= topLevelCount);
    LLPC_ASSERT(pNextTable - pResNodes <= resNodeCount);

    // Write pointer/size into PipelineShaderInfo.
    pShaderInfo->userDataNodeCount = pResNode - pResNodes;
    pShaderInfo->pUserDataNodes = pResNodes;
}

