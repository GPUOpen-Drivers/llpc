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
 * @file  llpcSpirvLowerResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerResourceCollect.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include "SPIRVInternal.h"
#include "llpcBuilder.h"
#include "llpcContext.h"
#include "llpcShaderModes.h"
#include "llpcSpirvLowerResourceCollect.h"

#define DEBUG_TYPE "llpc-spirv-lower-resource-collect"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerResourceCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering opertions for resource collecting
ModulePass* CreateSpirvLowerResourceCollect(
    bool collectDetailUsage) // Whether to collect detailed usages of resource node datas and FS output infos
{
    return new SpirvLowerResourceCollect(collectDetailUsage);
}

// =====================================================================================================================
SpirvLowerResourceCollect::SpirvLowerResourceCollect(
    bool collectDetailUsage) // Whether to collect detailed usages of resource node datas and FS output infos
    :
    SpirvLower(ID),
    m_collectDetailUsage(collectDetailUsage),
    m_pushConstSize(0),
    m_detailUsageValid(false)
{
    initializeSpirvLowerResourceCollectPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Collect resource node data
void SpirvLowerResourceCollect::CollectResourceNodeData(
    const GlobalVariable* pGlobal)       // [in] Global variable to collect resource node data
{
    auto pGlobalTy = pGlobal->getType()->getContainedType(0);

    MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::Resource);
    auto descSet = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(0))->getZExtValue();
    auto binding = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(1))->getZExtValue();
    auto spvOpCode = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(2))->getZExtValue();

    // Map the SPIR-V opcode to descriptor type.
    ResourceMappingNodeType nodeType = ResourceMappingNodeType::Unknown;
    switch (spvOpCode)
    {
    case OpTypeSampler:
        {
            // Sampler descriptor.
            nodeType = ResourceMappingNodeType::DescriptorSampler;
            break;
        }
    case OpTypeImage:
        {
            nodeType = ResourceMappingNodeType::DescriptorResource;
            // Image descriptor.
            Type* pImageType = pGlobalTy->getPointerElementType();
            std::string imageTypeName = pImageType->getStructName();
            // Format of image opaque type: ...[.SampledImage.<date type><dim>]...
            if (imageTypeName.find(".SampledImage") != std::string::npos)
            {
                auto pos = imageTypeName.find("_");
                LLPC_ASSERT(pos != std::string::npos);

                ++pos;
                Dim dim = static_cast<Dim>(imageTypeName[pos] - '0');
                nodeType = (dim == DimBuffer) ?
                           ResourceMappingNodeType::DescriptorTexelBuffer :
                           ResourceMappingNodeType::DescriptorResource;
            }
            break;
        }
    case OpTypeSampledImage:
        {
            // Combined image and sampler descriptors.
            nodeType = ResourceMappingNodeType::DescriptorCombinedTexture;
            break;
        }
    default:
        {
            // Normal buffer.
            nodeType = ResourceMappingNodeType::DescriptorBuffer;
            break;
        }
    }

    ResourceNodeDataKey nodeData = {};

    nodeData.value.set = descSet;
    nodeData.value.binding = binding;
    nodeData.value.arraySize = GetFlattenArrayElementCount(pGlobalTy);
    auto result = m_resNodeDatas.insert(std::pair<ResourceNodeDataKey, ResourceMappingNodeType>(nodeData, nodeType));

    // Check if the node already had a different pair of node data/type. A DescriptorResource/DescriptorTexelBuffer
    // and a DescriptorSampler can use the same set/binding, in which case it is
    // DescriptorCombinedTexture.
    if (result.second == false)
    {
        LLPC_ASSERT(((nodeType == ResourceMappingNodeType::DescriptorCombinedTexture) ||
                     (nodeType == ResourceMappingNodeType::DescriptorResource) ||
                     (nodeType == ResourceMappingNodeType::DescriptorTexelBuffer) ||
                     (nodeType == ResourceMappingNodeType::DescriptorSampler)) &&
                    ((result.first->second == ResourceMappingNodeType::DescriptorCombinedTexture) ||
                     (result.first->second == ResourceMappingNodeType::DescriptorResource) ||
                     (result.first->second == ResourceMappingNodeType::DescriptorTexelBuffer) ||
                     (result.first->second == ResourceMappingNodeType::DescriptorSampler)));
        result.first->second = ResourceMappingNodeType::DescriptorCombinedTexture;
    }

}
// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerResourceCollect::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Resource-Collect\n");

    SpirvLower::Init(&module);

    // Collect unused globals and remove them
    std::unordered_set<GlobalVariable*> removedGlobals;
    for (auto pGlobal = m_pModule->global_begin(), pEnd = m_pModule->global_end(); pGlobal != pEnd; ++pGlobal)
    {
        if (pGlobal->user_empty())
        {
            Value* pInitializer = nullptr;
            if (pGlobal->hasInitializer())
            {
                pInitializer = pGlobal->getInitializer();
            }

            if ((pInitializer == nullptr) || isa<UndefValue>(pInitializer))
            {
                removedGlobals.insert(&*pGlobal);
            }
        }
    }

    for (auto pGlobal : removedGlobals)
    {
        pGlobal->dropAllReferences();
        pGlobal->eraseFromParent();
    }

    // Collect resource usages from globals
    for (auto pGlobal = m_pModule->global_begin(), pEnd = m_pModule->global_end(); pGlobal != pEnd; ++pGlobal)
    {
        auto addrSpace = pGlobal->getType()->getAddressSpace();
        switch (addrSpace)
        {
        case SPIRAS_Constant:
            {
                if (pGlobal->hasMetadata(gSPIRVMD::PushConst))
                {
                    // Push constant
                    MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::PushConst);
                    m_pushConstSize = mdconst::dyn_extract<ConstantInt>(pMetaNode->getOperand(0))->getZExtValue();
                }
                else
                {
                    // Only collect resource node data when requested
                    if (m_collectDetailUsage == true)
                    {
                        CollectResourceNodeData(&*pGlobal);
                    }
                }
                break;
            }
        case SPIRAS_Private:
        case SPIRAS_Global:
        case SPIRAS_Local:
        case SPIRAS_Input:
            {
                break;
            }
        case SPIRAS_Output:
            {
                // Only collect FS out info when requested.
                Type* pGlobalTy = pGlobal->getType()->getContainedType(0);
                if (m_collectDetailUsage == false || pGlobalTy->isSingleValueType() == false)
                {
                    break;
                }

                FsOutInfo fsOutInfo = {};
                MDNode* pMetaNode = pGlobal->getMetadata(gSPIRVMD::InOut);
                auto pMeta = mdconst::dyn_extract<Constant>(pMetaNode->getOperand(0));

                ShaderInOutMetadata inOutMeta = {};
                Constant* pInOutMetaConst = cast<Constant>(pMeta);
                inOutMeta.U64All[0] = cast<ConstantInt>(pInOutMetaConst->getOperand(0))->getZExtValue();
                inOutMeta.U64All[1] = cast<ConstantInt>(pInOutMetaConst->getOperand(1))->getZExtValue();

                const uint32_t location = inOutMeta.Value;
                const uint32_t index = inOutMeta.Index;

                // Collect basic types of fragment outputs
                BasicType basicTy = BasicType::Unknown;

                const auto pCompTy = pGlobalTy->isVectorTy() ? pGlobalTy->getVectorElementType() : pGlobalTy;
                const uint32_t bitWidth = pCompTy->getScalarSizeInBits();
                const bool signedness = (inOutMeta.Signedness != 0);

                if (pCompTy->isIntegerTy())
                {
                    // Integer type
                    if (bitWidth == 8)
                    {
                        basicTy = signedness ? BasicType::Int8 : BasicType::Uint8;
                    }
                    else if (bitWidth == 16)
                    {
                        basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
                    }
                    else
                    {
                        LLPC_ASSERT(bitWidth == 32);
                        basicTy = signedness ? BasicType::Int : BasicType::Uint;
                    }
                }
                else if (pCompTy->isFloatingPointTy())
                {
                    // Floating-point type
                    if (bitWidth == 16)
                    {
                        basicTy = BasicType::Float16;
                    }
                    else
                    {
                        LLPC_ASSERT(bitWidth == 32);
                        basicTy = BasicType::Float;
                    }
                }
                else
                {
                    LLPC_NEVER_CALLED();
                }

                fsOutInfo.location = location;
                fsOutInfo.location = index;
                fsOutInfo.componentCount = pGlobalTy->isVectorTy() ? pGlobalTy->getVectorNumElements() : 1;;
                fsOutInfo.basicType = basicTy;
                m_fsOutInfos.push_back(fsOutInfo);
                break;
            }
        case SPIRAS_Uniform:
            {
                // Only collect resource node data when requested
                if (m_collectDetailUsage == true)
                {
                    CollectResourceNodeData(&*pGlobal);
                }
                break;
            }
        default:
            {
                LLPC_NEVER_CALLED();
                break;
            }
        }
    }
    if (!m_fsOutInfos.empty() || !m_resNodeDatas.empty())
    {
        m_detailUsageValid = true;
    }

    return true;
}

// =====================================================================================================================
// Gets element count if the specified type is an array (flattened for multi-dimension array).
uint32_t SpirvLowerResourceCollect::GetFlattenArrayElementCount(
    const Type* pTy // [in] Type to check
    ) const
{
    uint32_t elemCount = 1;

    auto pArrayTy = dyn_cast<ArrayType>(pTy);
    while (pArrayTy != nullptr)
    {
        elemCount *= pArrayTy->getArrayNumElements();
        pArrayTy = dyn_cast<ArrayType>(pArrayTy->getArrayElementType());
    }
    return elemCount;
}

// =====================================================================================================================
// Gets element type if the specified type is an array (flattened for multi-dimension array).
const Type* SpirvLowerResourceCollect::GetFlattenArrayElementType(
    const Type* pTy // [in] Type to check
    ) const
{
    const Type* pElemType = pTy;

    auto pArrayTy = dyn_cast<ArrayType>(pTy);
    while (pArrayTy != nullptr)
    {
        pElemType = pArrayTy->getArrayElementType();
        pArrayTy = dyn_cast<ArrayType>(pElemType);
    }
    return pElemType;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for resource collecting.
INITIALIZE_PASS(SpirvLowerResourceCollect, DEBUG_TYPE,
                "Lower SPIR-V resource collecting", false, false)
