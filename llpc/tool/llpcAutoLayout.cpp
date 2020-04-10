/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcUtil.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "llpc-auto-layout"

using namespace llvm;
using namespace Llpc;
using namespace SPIRV;

struct ResourceNodeSet
{
    std::vector<ResourceMappingNode> nodes;   // Vector of resource mapping nodes
    std::map<unsigned, unsigned> bindingMap;  // Map from binding to index in nodes vector
};

// -auto-layout-desc: automatically create descriptor layout based on resource usages
//
// NOTE: This option is deprecated and will be ignored, and is present only for compatibility.
static cl::opt<bool> AutoLayoutDesc("auto-layout-desc",
                                    cl::desc("Automatically create descriptor layout based on resource usages"));

// Offset in Node will be a fixed number, which will be conveniently to identify offset in auto-layout.
static const unsigned OffsetStrideInDwords = 12;

// =====================================================================================================================
// Get the storage size in bytes of a SPIR-V type.
// This does not need to be completely accurate, as it is only used to fake up a push constant user data node.
static unsigned getTypeDataSize(
    const SPIRVType*  ty)  // [in] Type to determine the data size of
{
    switch (ty->getOpCode())
    {
    case OpTypeVector:
        return getTypeDataSize(ty->getVectorComponentType()) * ty->getVectorComponentCount();
    case OpTypeMatrix:
        return getTypeDataSize(ty->getMatrixColumnType()) * ty->getMatrixColumnCount();
    case OpTypeArray:
        return getTypeDataSize(ty->getArrayElementType()) * ty->getArrayLength();
    case OpTypeStruct:
        {
            unsigned totalSize = 0;
            for (unsigned memberIdx = 0; memberIdx < ty->getStructMemberCount(); ++memberIdx)
                totalSize += getTypeDataSize(ty->getStructMemberType(memberIdx));
            return totalSize;
        }
    default:
        return (ty->getBitWidth() + 7) / 8;
    }
}

// =====================================================================================================================
// Find VaPtr userDataNode with specified set.
static const ResourceMappingNode* findDescriptorTableVaPtr(
    PipelineShaderInfo*         shaderInfo,                 // [in] Shader info, will have user data nodes added to it
    unsigned set)                                            // [in] Accroding this set to find ResourceMappingNode
{
    const ResourceMappingNode* descriptorTableVaPtr = nullptr;

    for (unsigned k = 0; k < shaderInfo->userDataNodeCount; ++k)
    {
        const ResourceMappingNode* userDataNode = &shaderInfo->pUserDataNodes[k];

        if (userDataNode->type == Llpc::ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            if (userDataNode->tablePtr.pNext[0].srdRange.set == set)
            {
                descriptorTableVaPtr = userDataNode;
                break;
            }
        }
    }

    return descriptorTableVaPtr;
}

// =====================================================================================================================
// Find userDataNode with specified set and binding. And return Node index.
static const ResourceMappingNode* findResourceNode(
    const ResourceMappingNode* userDataNode, // [in] ResourceMappingNode pointer
    unsigned                   nodeCount,     // [in] User data node count
    unsigned                   set,           // [in] find same set in node array
    unsigned                   binding,       // [in] find same binding in node array
    unsigned*                  index)         // [out] Return node position in node array
{
    const ResourceMappingNode* resourceNode = nullptr;

    for (unsigned j = 0; j < nodeCount; ++j)
    {
        const ResourceMappingNode* next = &userDataNode[j];

        if(set == next->srdRange.set && binding == next->srdRange.binding)
        {
            resourceNode = next;
            *index = j;
            break;
        }
    }

    return resourceNode;
}

// =====================================================================================================================
// Compare if pAutoLayoutUserDataNodes is subset of pUserDataNodes.
bool checkShaderInfoComptible(
    PipelineShaderInfo*         shaderInfo,                 // [in/out] Shader info, will have user data nodes added to it
    unsigned                    autoLayoutUserDataNodeCount, // [in] UserData Node count
    const ResourceMappingNode*  autoLayoutUserDataNodes)    // [in] ResourceMappingNode
{
    bool hit = false;

    if (autoLayoutUserDataNodeCount == 0)
        hit = true;
    else if (shaderInfo->pDescriptorRangeValues || shaderInfo->pSpecializationInfo->dataSize != 0)
        hit = false;
    else if (shaderInfo->userDataNodeCount >= autoLayoutUserDataNodeCount)
    {
        for (unsigned n = 0; n < autoLayoutUserDataNodeCount; ++n)
        {
            const ResourceMappingNode* autoLayoutUserDataNode = &autoLayoutUserDataNodes[n];

            // Multiple levels
            if (autoLayoutUserDataNode->type == Llpc::ResourceMappingNodeType::DescriptorTableVaPtr)
            {
                unsigned set = autoLayoutUserDataNode->tablePtr.pNext[0].srdRange.set;
                const ResourceMappingNode* userDataNode = findDescriptorTableVaPtr(shaderInfo, set);

                if (userDataNode )
                {
                    bool hitNode = false;
                    for (unsigned i = 0; i < autoLayoutUserDataNode->tablePtr.nodeCount; ++i)
                    {
                        const ResourceMappingNode* autoLayoutNext = &autoLayoutUserDataNode->tablePtr.pNext[i];

                        unsigned index = 0;
                        const ResourceMappingNode* node = findResourceNode(
                                                            userDataNode->tablePtr.pNext,
                                                            userDataNode->tablePtr.nodeCount,
                                                            autoLayoutNext->srdRange.set,
                                                            autoLayoutNext->srdRange.binding,
                                                            &index);

                        if (node )
                        {
                            if (autoLayoutNext->type == node->type &&
                                autoLayoutNext->sizeInDwords == node->sizeInDwords &&
                                autoLayoutNext->sizeInDwords <= OffsetStrideInDwords &&
                                autoLayoutNext->offsetInDwords == (index * OffsetStrideInDwords))
                            {
                                hitNode = true;
                                continue;
                            }
                            else
                            {
                                outs() << "AutoLayoutNode:"
                                       << "\n ->type                    : "
                                       << format("0x%016" PRIX64, static_cast<unsigned>(autoLayoutNext->type))
                                       << "\n ->sizeInDwords            : " << autoLayoutNext->sizeInDwords
                                       << "\n ->offsetInDwords          : " << autoLayoutNext->offsetInDwords;

                                outs() << "\nShaderInfoNode:"
                                       << "\n ->type                    : "
                                       << format("0x%016" PRIX64, static_cast<unsigned>(node->type))
                                       << "\n ->sizeInDwords            : " << node->sizeInDwords
                                       << "\n OffsetStrideInDwords      : " << OffsetStrideInDwords
                                       << "\n index*OffsetStrideInDwords: " << (index * OffsetStrideInDwords) << "\n";
                                hitNode = false;
                                break;
                            }
                        }
                        else
                            break;
                    }
                    if (!hitNode)
                    {
                        hit = false;
                        break;
                    }
                    else
                        hit = true;
                }
                else
                {
                    hit = false;
                    break;
                }
            }
            // Single level
            else
            {
                unsigned index = 0;
                const ResourceMappingNode* node = findResourceNode(
                                                            shaderInfo->pUserDataNodes,
                                                            shaderInfo->userDataNodeCount,
                                                            autoLayoutUserDataNode->srdRange.set,
                                                            autoLayoutUserDataNode->srdRange.binding,
                                                            &index);
                if (node && autoLayoutUserDataNode->sizeInDwords == node->sizeInDwords)
                {
                    hit = true;
                    continue;
                }
                else
                {
                    hit = false;
                    break;
                }
            }
        }
    }

    return hit;
}

// =====================================================================================================================
// Compare if neccessary pipeline state is same.
bool checkPipelineStateCompatible(
        const ICompiler*                  compiler,                // [in] LLPC compiler object
        Llpc::GraphicsPipelineBuildInfo*  pipelineInfo,            // [in] Graphics pipeline info
        Llpc::GraphicsPipelineBuildInfo*  autoLayoutPipelineInfo,  // [in] layout pipeline info
        Llpc::GfxIpVersion                gfxIp)                    // Graphics IP version
{
    bool compatible = true;

    auto cbState = &pipelineInfo->cbState;
    auto autoLayoutCbState = &autoLayoutPipelineInfo->cbState;

    for (unsigned i = 0; i < MaxColorTargets; ++i)
    {
        if (cbState->target[i].format != VK_FORMAT_UNDEFINED)
        {
            // NOTE: Alpha-to-coverage only take effect for output from color target 0.
            const bool enableAlphaToCoverage = cbState->alphaToCoverageEnable && i == 0;
            unsigned exportFormat = compiler->ConvertColorBufferFormatToExportFormat(
                                                            &cbState->target[i],
                                                            enableAlphaToCoverage);
            const bool autoLayoutEnableAlphaToCoverage = autoLayoutCbState->alphaToCoverageEnable && i == 0;
            unsigned autoLayoutExportFormat = compiler->ConvertColorBufferFormatToExportFormat(
                                                            &autoLayoutCbState->target[i],
                                                            autoLayoutEnableAlphaToCoverage);

            if (exportFormat != autoLayoutExportFormat)
            {
                outs()  << "pPipelineInfo->cbState.target[" << i << "] export format:"
                        << format("0x%016" PRIX64, exportFormat)
                        << "\n"
                        << "pAutoLayoutPipelineInfo->cbState.target[" << i << "] export format:"
                        << format("0x%016" PRIX64, autoLayoutExportFormat)
                        << "\n";

                compatible = false;
                break;
            }
        }
    }
    // TODO: RsState and other members in CbState except target[].format

    return compatible;
}

// =====================================================================================================================
// Lay out dummy descriptors and other information for one shader stage. This is used when running amdllpc on a single
// SPIR-V or GLSL shader, rather than on a .pipe file. Memory allocated here may be leaked, but that does not
// matter because we are running a short-lived command-line utility.
void doAutoLayoutDesc(
    ShaderStage                 shaderStage,    // Shader stage
    BinaryData                  spirvBin,       // SPIR-V binary
    GraphicsPipelineBuildInfo*  pipelineInfo,  // [in/out] Graphics pipeline info, will have dummy information filled
                                                //   in. nullptr if not a graphics pipeline.
    PipelineShaderInfo*         shaderInfo,    // [in/out] Shader info, will have user data nodes added to it
    unsigned&                   topLevelOffset, // [in/out] User data offset; ensures that multiple shader stages use
                                                //    disjoint offsets
    bool                        checkAutoLayoutCompatible) // [in] if check AutoLayout Compatiple
{
    // Read the SPIR-V.
    std::string spirvCode(static_cast<const char*>(spirvBin.pCode), spirvBin.codeSize);
    std::istringstream spirvStream(spirvCode);
    std::unique_ptr<SPIRVModule> module(SPIRVModule::createSPIRVModule());
    spirvStream >> *module;

    // Find the entry target.
    SPIRVEntryPoint* entryPoint = nullptr;
    SPIRVFunction* func = nullptr;
    for (unsigned i = 0, funcCount = module->getNumFunctions(); i < funcCount; ++i)
    {
        func = module->getFunction(i);
        entryPoint = module->getEntryPoint(func->getId());
        if (entryPoint &&
            entryPoint->getExecModel() == SPIRVExecutionModelKind(shaderStage) &&
            entryPoint->getName() == shaderInfo->pEntryTarget)
            break;
        func = nullptr;
    }
    if (!entryPoint )
        return;

    // Shader stage specific processing
    auto inOuts = entryPoint->getInOuts();
    if (shaderStage == ShaderStageVertex)
    {
        // Create dummy vertex info
        auto vertexBindings = new std::vector<VkVertexInputBindingDescription>;
        auto vertexAttribs = new std::vector<VkVertexInputAttributeDescription>;

        for (auto varId : ArrayRef<SPIRVWord>(inOuts.first, inOuts.second))
        {
            auto var = static_cast<SPIRVVariable*>(module->getValue(varId));
            if (var->getStorageClass() == StorageClassInput)
            {
                SPIRVWord location = SPIRVID_INVALID;
                if (var->hasDecorate(DecorationLocation, 0, &location))
                {
                    auto varElemTy = var->getType()->getPointerElementType();
                    if (varElemTy->getOpCode() == OpTypeArray)
                        varElemTy = varElemTy->getArrayElementType();

                    if (varElemTy->getOpCode() == OpTypeMatrix)
                        varElemTy = varElemTy->getMatrixColumnType();

                    if (varElemTy->getOpCode() == OpTypeVector)
                        varElemTy = varElemTy->getVectorComponentType();

                    VkFormat format = VK_FORMAT_UNDEFINED;
                    switch (varElemTy->getOpCode())
                    {
                    case OpTypeInt:
                        {
                            bool isSigned = reinterpret_cast<SPIRVTypeInt*>(varElemTy)->isSigned();
                            switch (varElemTy->getIntegerBitWidth())
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
                            switch (varElemTy->getFloatBitWidth())
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
                    assert(format != VK_FORMAT_UNDEFINED);

                    VkVertexInputBindingDescription vertexBinding = {};
                    vertexBinding.binding   = location;
                    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
                    vertexBinding.stride = SizeOfVec4;

                    VkVertexInputAttributeDescription vertexAttrib = {};
                    vertexAttrib.binding    = location;
                    vertexAttrib.location   = location;
                    vertexAttrib.offset     = 0;
                    vertexAttrib.format     = format;

                    vertexBindings->push_back(vertexBinding);
                    vertexAttribs->push_back(vertexAttrib);
                }
            }
        }

        auto vertexInputState = new VkPipelineVertexInputStateCreateInfo;
        pipelineInfo->pVertexInput = vertexInputState;
        vertexInputState->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputState->pNext = nullptr;
        vertexInputState->vertexBindingDescriptionCount = vertexBindings->size();
        vertexInputState->pVertexBindingDescriptions = vertexBindings->data();
        vertexInputState->vertexAttributeDescriptionCount = vertexAttribs->size();
        vertexInputState->pVertexAttributeDescriptions = vertexAttribs->data();

        // Set primitive topology
        pipelineInfo->iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    else if (shaderStage == ShaderStageTessControl || shaderStage == ShaderStageTessEval)
    {
        // Set primitive topology and patch control points
        pipelineInfo->iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pipelineInfo->iaState.patchControlPoints = 3;
    }
    else if (shaderStage == ShaderStageGeometry)
    {
        // Set primitive topology
        auto topology = VkPrimitiveTopology(0);
        if (func->getExecutionMode(ExecutionModeInputPoints))
            topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        else if (func->getExecutionMode(ExecutionModeInputLines))
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        else if (func->getExecutionMode(ExecutionModeInputLinesAdjacency))
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        else if (func->getExecutionMode(ExecutionModeTriangles))
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        else if (func->getExecutionMode(ExecutionModeInputTrianglesAdjacency))
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        else
            llvm_unreachable("Should never be called!");
        pipelineInfo->iaState.topology = topology;
    }
    else if (shaderStage == ShaderStageFragment)
    {
        // Set dummy color formats for fragment outputs
        for (auto varId : ArrayRef<SPIRVWord>(inOuts.first, inOuts.second))
        {
            auto var = static_cast<SPIRVVariable*>(module->getValue(varId));
            if (var->getStorageClass() != StorageClassOutput)
                continue;

            SPIRVWord location = SPIRVID_INVALID;
            if (!var->hasDecorate(DecorationLocation, 0, &location))
                continue;

            SPIRVType* varElemTy = var->getType()->getPointerElementType();
            unsigned elemCount = 1;
            if (varElemTy->getOpCode() == OpTypeVector)
            {
                elemCount = varElemTy->getVectorComponentCount();
                varElemTy = varElemTy->getVectorComponentType();
            }
            static const VkFormat UndefinedFormatTable[] =
            {
                VK_FORMAT_UNDEFINED,
                VK_FORMAT_UNDEFINED,
                VK_FORMAT_UNDEFINED,
                VK_FORMAT_UNDEFINED,
            };
            const VkFormat* formatTable = UndefinedFormatTable;

            switch (varElemTy->getOpCode())
            {
            case OpTypeInt:
                {
                    switch (varElemTy->getIntegerBitWidth())
                    {
                    case 8:
                        {
                            if (reinterpret_cast<SPIRVTypeInt*>(varElemTy)->isSigned())
                            {
                                static const VkFormat FormatTable[] =
                                {
                                    VK_FORMAT_R8_SINT,
                                    VK_FORMAT_R8G8_SINT,
                                    VK_FORMAT_R8G8B8_SINT,
                                    VK_FORMAT_R8G8B8A8_SINT,
                                };
                                formatTable = FormatTable;
                            }
                            else
                            {
                                static const VkFormat FormatTable[] =
                                {
                                    VK_FORMAT_R8_UINT,
                                    VK_FORMAT_R8G8_UINT,
                                    VK_FORMAT_R8G8B8_UINT,
                                    VK_FORMAT_R8G8B8A8_UINT,
                                };
                                formatTable = FormatTable;
                            }
                            break;
                        }
                    case 16:
                        {
                            if (reinterpret_cast<SPIRVTypeInt*>(varElemTy)->isSigned())
                            {
                                static const VkFormat FormatTable[] =
                                {
                                    VK_FORMAT_R16_SINT,
                                    VK_FORMAT_R16G16_SINT,
                                    VK_FORMAT_R16G16B16_SINT,
                                    VK_FORMAT_R16G16B16A16_SINT,
                                };
                                formatTable = FormatTable;
                            }
                            else
                            {
                                static const VkFormat FormatTable[] =
                                {
                                    VK_FORMAT_R16_UINT,
                                    VK_FORMAT_R16G16_UINT,
                                    VK_FORMAT_R16G16B16_UINT,
                                    VK_FORMAT_R16G16B16A16_UINT,
                                };
                                formatTable = FormatTable;
                            }
                            break;
                        }
                    case 32:
                        {
                            if (reinterpret_cast<SPIRVTypeInt*>(varElemTy)->isSigned())
                            {
                                static const VkFormat FormatTable[] =
                                {
                                    VK_FORMAT_R32_SINT,
                                    VK_FORMAT_R32G32_SINT,
                                    VK_FORMAT_R32G32B32_SINT,
                                    VK_FORMAT_R32G32B32A32_SINT,
                                };
                                formatTable = FormatTable;
                            }
                            else
                            {
                                static const VkFormat FormatTable[] =
                                {
                                    VK_FORMAT_R32_UINT,
                                    VK_FORMAT_R32G32_UINT,
                                    VK_FORMAT_R32G32B32_UINT,
                                    VK_FORMAT_R32G32B32A32_UINT,
                                };
                                formatTable = FormatTable;
                            }
                            break;
                        }
                    }
                    break;
                }

            case OpTypeFloat:
                {
                    switch (varElemTy->getFloatBitWidth())
                    {
                    case 16:
                        {
                            static const VkFormat FormatTable[] =
                            {
                                VK_FORMAT_R16_SFLOAT,
                                VK_FORMAT_R16G16_SFLOAT,
                                VK_FORMAT_R16G16B16_SFLOAT,
                                VK_FORMAT_R16G16B16A16_SFLOAT,
                            };
                            formatTable = FormatTable;
                        }
                        break;
                    case 32:
                        {
                            static const VkFormat FormatTable[] =
                            {
                                VK_FORMAT_R32_SFLOAT,
                                VK_FORMAT_R32G32_SFLOAT,
                                VK_FORMAT_R32G32B32_SFLOAT,
                                VK_FORMAT_R32G32B32A32_SFLOAT,
                            };
                            formatTable = FormatTable;
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

            assert(elemCount <= 4);
            VkFormat format = formatTable[elemCount - 1];
            assert(format != VK_FORMAT_UNDEFINED);

            assert(location < MaxColorTargets);
            auto colorTarget = &pipelineInfo->cbState.target[location];
            colorTarget->format = format;
            colorTarget->channelWriteMask = (1U << elemCount) - 1;
        }
    }

    // Collect ResourceMappingNode entries in sets.
    std::map<unsigned, ResourceNodeSet> resNodeSets;
    std::map<std::pair<unsigned, unsigned>, unsigned> bindingIndex;
    unsigned pushConstSize = 0;
    for (unsigned i = 0, varCount = module->getNumVariables(); i < varCount; ++i)
    {
        auto var = module->getVariable(i);
        switch (var->getStorageClass())
        {
        case StorageClassFunction:
            {
                break;
            }

        case StorageClassPushConstant:
            {
                // Push constant: Get the size of the data and add to the total.
                auto varElemTy = var->getType()->getPointerElementType();
                pushConstSize += (getTypeDataSize(varElemTy) + 3) / 4;
                break;
            }

        default:
            {
                SPIRVWord binding = SPIRVID_INVALID;
                SPIRVWord descSet = 0;
                if (var->hasDecorate(DecorationBinding, 0, &binding))
                {
                    // Test shaderdb/OpDecorationGroup_TestGroupAndGroupMember_lit.spvasm
                    // defines a variable with a binding but no set. Handle that case.
                    var->hasDecorate(DecorationDescriptorSet, 0, &descSet);

                    // Find/create the node entry for this set and binding.
                    ResourceNodeSet& resNodeSet = resNodeSets[descSet];
                    auto iteratorAndCreated = resNodeSet.bindingMap.insert({binding, resNodeSet.nodes.size()});
                    unsigned nodesIndex = iteratorAndCreated.first->second;
                    if (iteratorAndCreated.second)
                    {
                        resNodeSet.nodes.push_back({});
                        resNodeSet.nodes.back().type = ResourceMappingNodeType::Unknown;
                    }
                    ResourceMappingNode* node = &resNodeSet.nodes[nodesIndex];

                    // Get the element type and array size.
                    auto varElemTy = var->getType()->getPointerElementType();
                    unsigned arraySize = 1;
                    while (varElemTy->isTypeArray())
                    {
                        arraySize *= varElemTy->getArrayLength();
                        varElemTy = varElemTy->getArrayElementType();
                    }

                    // Map the SPIR-V opcode to descriptor type and size.
                    ResourceMappingNodeType nodeType;
                    unsigned sizeInDwords;
                    switch (varElemTy->getOpCode())
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
                            auto imageType = static_cast<SPIRVTypeImage*>(varElemTy);
                            nodeType = imageType->getDescriptor().Dim == spv::DimBuffer ?
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
                    if (node->type == ResourceMappingNodeType::Unknown)
                        node->type = nodeType;
                    else if (node->type != nodeType)
                    {
                        {
                            assert(nodeType == ResourceMappingNodeType::DescriptorCombinedTexture ||
                                         nodeType == ResourceMappingNodeType::DescriptorResource ||
                                         nodeType == ResourceMappingNodeType::DescriptorTexelBuffer ||
                                         nodeType == ResourceMappingNodeType::DescriptorSampler);
                        }
                        {
                            assert(node->type == ResourceMappingNodeType::DescriptorCombinedTexture ||
                                   node->type == ResourceMappingNodeType::DescriptorResource ||
                                   node->type == ResourceMappingNodeType::DescriptorTexelBuffer ||
                                   node->type == ResourceMappingNodeType::DescriptorSampler);
                        }

                        node->type = ResourceMappingNodeType::DescriptorCombinedTexture;
                        sizeInDwords = 12 * arraySize;
                    }

                    // Fill out the rest of the node.
                    node->sizeInDwords = sizeInDwords;
                    node->srdRange.set = descSet;
                    node->srdRange.binding = binding;
                }
                break;
            }
        }
    }

    // Allocate dword offset to each node.
    for (auto& it : resNodeSets)
    {
        ResourceNodeSet& resNodeSet = it.second;
        unsigned offsetInDwords = 0;
        for (auto& node : resNodeSet.nodes)
        {
            if (checkAutoLayoutCompatible)
                node.offsetInDwords = node.srdRange.binding * OffsetStrideInDwords;
            else
            {
                node.offsetInDwords = offsetInDwords;
                offsetInDwords += node.sizeInDwords;
            }
        }
    }

    // Add up how much memory we need and allocate it.
    unsigned topLevelCount = resNodeSets.size();
    topLevelCount += 3; // Allow one for push consts, one for XFB and one for vertex buffer.
    unsigned resNodeCount = topLevelCount;
    for (const auto& resNodeSet : resNodeSets)
        resNodeCount += resNodeSet.second.nodes.size();
    auto resNodes = new ResourceMappingNode[resNodeCount];
    auto nextTable = resNodes + topLevelCount;
    auto resNode = resNodes;

    // Add a node for each set.
    for (const auto& resNodeSet : resNodeSets)
    {
        resNode->type = ResourceMappingNodeType::DescriptorTableVaPtr;
        resNode->sizeInDwords = 1;
        resNode->offsetInDwords = topLevelOffset;
        topLevelOffset += resNode->sizeInDwords;
        resNode->tablePtr.nodeCount = resNodeSet.second.nodes.size();
        resNode->tablePtr.pNext = nextTable;
        for (auto& resNode : resNodeSet.second.nodes)
            *nextTable++ = resNode;
        ++resNode;
    }

    if (shaderStage == ShaderStageVertex)
    {
        // Add a node for vertex buffer.
        resNode->type = ResourceMappingNodeType::IndirectUserDataVaPtr;
        resNode->sizeInDwords = 1;
        resNode->offsetInDwords = topLevelOffset;
        topLevelOffset += resNode->sizeInDwords;
        resNode->userDataPtr.sizeInDwords = 256;
        ++resNode;
    }

    if (shaderStage == ShaderStageVertex || shaderStage == ShaderStageTessEval || shaderStage == ShaderStageGeometry)
    {
        // Add a node for XFB.
        resNode->type = ResourceMappingNodeType::StreamOutTableVaPtr;
        resNode->sizeInDwords = 1;
        resNode->offsetInDwords = topLevelOffset;
        topLevelOffset += resNode->sizeInDwords;
        ++resNode;
    }

    if (pushConstSize != 0)
    {
        // Add a node for push consts.
        resNode->type = ResourceMappingNodeType::PushConst;
        resNode->sizeInDwords = pushConstSize;
        resNode->offsetInDwords = topLevelOffset;
        topLevelOffset += resNode->sizeInDwords;
        ++resNode;
    }

    assert(resNode - resNodes <= topLevelCount);
    assert(nextTable - resNodes <= resNodeCount);

    // Write pointer/size into PipelineShaderInfo.
    shaderInfo->userDataNodeCount = resNode - resNodes;
    shaderInfo->pUserDataNodes = resNodes;
}

