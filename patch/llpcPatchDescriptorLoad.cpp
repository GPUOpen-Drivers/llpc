/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchDescriptorLoad.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchDescriptorLoad.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-descriptor-load"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcContext.h"
#include "llpcPatchDescriptorLoad.h"

#include "SPIRVInternal.h"

using namespace llvm;
using namespace Llpc;

namespace llvm
{

namespace cl
{

extern opt<bool> EnableShadowDescriptorTable;

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchDescriptorLoad::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for descriptor load
ModulePass* CreatePatchDescriptorLoad()
{
    return new PatchDescriptorLoad();
}

// =====================================================================================================================
PatchDescriptorLoad::PatchDescriptorLoad()
    :
    Patch(ID)
{
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializePatchDescriptorLoadPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchDescriptorLoad::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Descriptor-Load\n");

    Patch::Init(&module);
    m_changed = false;

    // Invoke handling of "call" instruction
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStage(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = ShaderStage(shaderStage);
            visit(*m_pEntryPoint);
        }
    }

    // Remove unnecessary descriptor load calls
    for (auto pCallInst : m_descLoadCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_descLoadCalls.clear();

    // Remove unnecessary descriptor load functions
    for (auto pFunc : m_descLoadFuncs)
    {
        if (pFunc->user_empty())
        {
            pFunc->dropAllReferences();
            pFunc->eraseFromParent();
        }
    }
    m_descLoadFuncs.clear();

    m_pipelineSysValues.Clear();
    return m_changed;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchDescriptorLoad::visitCallInst(
    CallInst& callInst)   // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto mangledName = pCallee->getName();

    std::string descLoadPrefix = LlpcName::DescriptorLoadPrefix;
    bool isDescLoad = (strncmp(mangledName.data(),descLoadPrefix.c_str(), descLoadPrefix.size()) == 0);

    if (isDescLoad == false)
    {
        return; // Not descriptor load call
    }

    // Descriptor loading should be inlined and stay in shader entry-point
    LLPC_ASSERT(callInst.getParent()->getParent() == m_pEntryPoint);

    m_changed = true;
    Type* pDescPtrTy = nullptr;
    ResourceMappingNodeType nodeType1 = ResourceMappingNodeType::Unknown;
    ResourceMappingNodeType nodeType2 = ResourceMappingNodeType::Unknown;

    bool loadSpillTable = false;

    // TODO: The address space ID 2 is a magic number. We have to replace it with defined LLPC address space ID.
    if (mangledName == LlpcName::DescriptorLoadResource)
    {
        pDescPtrTy = m_pContext->Int32x8Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorResource;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorLoadSampler)
    {
        pDescPtrTy = m_pContext->Int32x4Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorSampler;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorLoadFmask)
    {
        pDescPtrTy = m_pContext->Int32x8Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorFmask;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorLoadBuffer)
    {
        pDescPtrTy = m_pContext->Int32x4Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorBuffer;
        nodeType2 = ResourceMappingNodeType::PushConst;
    }
    else if (mangledName == LlpcName::DescriptorLoadAddress)
    {
        nodeType1 = ResourceMappingNodeType::PushConst;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorLoadTexelBuffer)
    {
        pDescPtrTy = m_pContext->Int32x4Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorTexelBuffer;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorLoadSpillTable)
    {
        loadSpillTable = true;
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    if (loadSpillTable)
    {
        // Do not do this if the llpc.descriptor.load.spilltable is unused, as no spill table pointer has
        // been set up in PatchEntryPointMutate. That happens if PatchPushConst has unspilled all push constants.
        if (callInst.use_empty() == false)
        {
            auto pSpilledPushConstTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetSpilledPushConstTablePtr();
            callInst.replaceAllUsesWith(pSpilledPushConstTablePtr);
        }
        m_descLoadCalls.push_back(&callInst);
        m_descLoadFuncs.insert(pCallee);
    }
    else
    {
        LLPC_ASSERT(nodeType1 != ResourceMappingNodeType::Unknown);

        // Calculate descriptor offset (in bytes)
        auto pDescSet = cast<ConstantInt>(callInst.getOperand(0));
        auto pBinding = cast<ConstantInt>(callInst.getOperand(1));
        auto pArrayOffset = callInst.getOperand(2); // Offset for arrayed resource (index)

        uint32_t descOffset = 0;
        uint32_t descSize   = 0;
        uint32_t dynDescIdx = InvalidValue;
        Value*   pDesc = nullptr;
        auto  pDescRangeValue = GetDescriptorRangeValue(nodeType1, pDescSet->getZExtValue(), pBinding->getZExtValue());

        if (pDescRangeValue != nullptr)
        {
            // Descriptor range value (immutable sampler in Vulkan)
            LLPC_ASSERT(nodeType1 == ResourceMappingNodeType::DescriptorSampler);

            uint32_t descSizeInDword = pDescPtrTy->getPointerElementType()->getVectorNumElements();

            if ((pDescRangeValue->arraySize == 1) || isa<ConstantInt>(pArrayOffset))
            {
                // Array size is 1 or array offset is constant
                uint32_t arrayOffset = 0;
                if (isa<ConstantInt>(pArrayOffset))
                {
                    arrayOffset = cast<ConstantInt>(pArrayOffset)->getZExtValue();
                }

                const uint32_t* pDescValue = pDescRangeValue->pValue + arrayOffset * descSizeInDword;

                std::vector<Constant*> descElems;
                for (uint32_t i = 0; i < descSizeInDword; ++i)
                {
                    descElems.push_back(ConstantInt::get(m_pContext->Int32Ty(), pDescValue[i]));
                }
                pDesc = ConstantVector::get(descElems);
            }
            else
            {
                // Array size is greater than 1 and array offset is non-constant
                GlobalVariable* pDescs = nullptr;

                if (m_descs.find(pDescRangeValue) == m_descs.end())
                {
                    std::vector<Constant*> descs;
                    for (uint32_t i = 0; i < pDescRangeValue->arraySize; ++i)
                    {
                        const uint32_t* pDescValue = pDescRangeValue->pValue + i * descSizeInDword;

                        std::vector<Constant*> descElems;
                        for (uint32_t j = 0; j < descSizeInDword; ++j)
                        {
                            descElems.push_back(ConstantInt::get(m_pContext->Int32Ty(), pDescValue[j]));
                        }

                        descs.push_back(ConstantVector::get(descElems));
                    }

                    auto pDescsTy = ArrayType::get(VectorType::get(m_pContext->Int32Ty(), descSizeInDword),
                                                   pDescRangeValue->arraySize);

                    pDescs = new GlobalVariable(*m_pModule,
                                                pDescsTy,
                                                true, // isConstant
                                                GlobalValue::InternalLinkage,
                                                ConstantArray::get(pDescsTy, descs),
                                                "",
                                                nullptr,
                                                GlobalValue::NotThreadLocal,
                                                ADDR_SPACE_CONST);

                    m_descs[pDescRangeValue] = pDescs;
                }
                else
                {
                    pDescs = m_descs[pDescRangeValue];
                }

                std::vector<Value*> idxs;
                idxs.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                idxs.push_back(pArrayOffset);

                auto pDescPtr = GetElementPtrInst::Create(nullptr, pDescs, idxs, "", &callInst);
                pDesc = new LoadInst(pDescPtr, "", &callInst);
            }
        }

        if (pDesc == nullptr)
        {
            auto foundNodeType = CalcDescriptorOffsetAndSize(nodeType1,
                                                             nodeType2,
                                                             pDescSet->getZExtValue(),
                                                             pBinding->getZExtValue(),
                                                             &descOffset,
                                                             &descSize,
                                                             &dynDescIdx);
            if (foundNodeType == ResourceMappingNodeType::PushConst)
            {
                // Handle the case of an inline const node when we were expecting a buffer descriptor.
                nodeType1 = foundNodeType;
            }

            uint32_t descSizeInDword = descSize / sizeof(uint32_t);
            if (dynDescIdx != InvalidValue)
            {
                // Dynamic descriptors
                pDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetDynamicDesc(dynDescIdx);
                if (pDesc != nullptr)
                {
                    auto pDescTy = VectorType::get(m_pContext->Int32Ty(), descSizeInDword);
                    if (pDesc->getType() != pDescTy)
                    {
                        // Array dynamic descriptor
                        Value* pDynDesc = UndefValue::get(pDescTy);
                        auto pDescStride = ConstantInt::get(m_pContext->Int32Ty(), descSizeInDword);
                        auto pIndex = BinaryOperator::CreateMul(pArrayOffset, pDescStride, "", &callInst);
                        for (uint32_t i = 0; i < descSizeInDword; ++i)
                        {
                            auto pDescElem = ExtractElementInst::Create(pDesc, pIndex, "", &callInst);
                            pDynDesc = InsertElementInst::Create(pDynDesc,
                                                                 pDescElem,
                                                                 ConstantInt::get(m_pContext->Int32Ty(), i),
                                                                 "",
                                                                 &callInst);
                            pIndex = BinaryOperator::CreateAdd(pIndex,
                                                               ConstantInt::get(m_pContext->Int32Ty(), 1),
                                                               "",
                                                               &callInst);
                        }
                        pDesc = pDynDesc;
                    }

                    // Extract compact buffer descriptor
                    if (descSizeInDword == DescriptorSizeBufferCompact / sizeof(uint32_t))
                    {
                        // Extract compact buffer descriptor
                        Value* pDescElem0 = ExtractElementInst::Create(pDesc,
                            ConstantInt::get(m_pContext->Int32Ty(), 0),
                            "",
                            &callInst);

                        Value* pDescElem1 = ExtractElementInst::Create(pDesc,
                            ConstantInt::get(m_pContext->Int32Ty(), 1),
                            "",
                            &callInst);

                        // Build normal buffer descriptor
                        auto pBufDescTy = m_pContext->Int32x4Ty();
                        Value* pBufDesc = UndefValue::get(pBufDescTy);

                        // DWORD0
                        pBufDesc = InsertElementInst::Create(pBufDesc,
                            pDescElem0,
                            ConstantInt::get(m_pContext->Int32Ty(), 0),
                            "",
                            &callInst);

                        // DWORD1
                        SqBufRsrcWord1 sqBufRsrcWord1 = {};
                        sqBufRsrcWord1.bits.BASE_ADDRESS_HI = UINT16_MAX;

                        pDescElem1 = BinaryOperator::CreateAnd(pDescElem1,
                            ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord1.u32All),
                            "",
                            &callInst);
                        pBufDesc = InsertElementInst::Create(pBufDesc,
                            pDescElem1,
                            ConstantInt::get(m_pContext->Int32Ty(), 1),
                            "",
                            &callInst);

                        // DWORD2
                        SqBufRsrcWord2 sqBufRsrcWord2 = {};
                        sqBufRsrcWord2.bits.NUM_RECORDS = UINT32_MAX;

                        pBufDesc = InsertElementInst::Create(pBufDesc,
                            ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord2.u32All),
                            ConstantInt::get(m_pContext->Int32Ty(), 2),
                            "",
                            &callInst);

                        // DWORD3
                        {
                            SqBufRsrcWord3 sqBufRsrcWord3 = {};
                            sqBufRsrcWord3.bits.DST_SEL_X = BUF_DST_SEL_X;
                            sqBufRsrcWord3.bits.DST_SEL_Y = BUF_DST_SEL_Y;
                            sqBufRsrcWord3.bits.DST_SEL_Z = BUF_DST_SEL_Z;
                            sqBufRsrcWord3.bits.DST_SEL_W = BUF_DST_SEL_W;
                            sqBufRsrcWord3.gfx6.NUM_FORMAT = BUF_NUM_FORMAT_UINT;
                            sqBufRsrcWord3.gfx6.DATA_FORMAT = BUF_DATA_FORMAT_32;
                            LLPC_ASSERT(sqBufRsrcWord3.u32All == 0x24FAC);

                            pBufDesc = InsertElementInst::Create(pBufDesc,
                                ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord3.u32All),
                                ConstantInt::get(m_pContext->Int32Ty(), 3),
                                "",
                                &callInst);
                        }

                        pDesc = pBufDesc;
                    }

                }
                else
                {
                    LLPC_NEVER_CALLED();
                }
            }
            else if (nodeType1 == ResourceMappingNodeType::PushConst)
            {
                auto pDescTablePtr =
                    m_pipelineSysValues.Get(m_pEntryPoint)->GetDescTablePtr(pDescSet->getZExtValue());

                Value* pDescTableAddr = new PtrToIntInst(pDescTablePtr,
                                                         m_pContext->Int64Ty(),
                                                         "",
                                                         &callInst);

                pDescTableAddr = new BitCastInst(pDescTableAddr, m_pContext->Int32x2Ty(), "", &callInst);

                // Extract descriptor table address
                Value* pDescElem0 = ExtractElementInst::Create(pDescTableAddr,
                    ConstantInt::get(m_pContext->Int32Ty(), 0),
                    "",
                    &callInst);

                auto pDescOffset = ConstantInt::get(m_pContext->Int32Ty(), descOffset);

                pDescElem0 = BinaryOperator::CreateAdd(pDescElem0, pDescOffset, "", &callInst);

                if (pDescPtrTy == nullptr)
                {
                    // Load the address of inline constant buffer
                    pDesc = InsertElementInst::Create(pDescTableAddr,
                        pDescElem0,
                        ConstantInt::get(m_pContext->Int32Ty(), 0),
                        "",
                        &callInst);
                }
                else
                {
                    // Build buffer descriptor from inline constant buffer address
                    SqBufRsrcWord1 sqBufRsrcWord1 = {};
                    SqBufRsrcWord2 sqBufRsrcWord2 = {};
                    SqBufRsrcWord3 sqBufRsrcWord3 = {};

                    sqBufRsrcWord1.bits.BASE_ADDRESS_HI = UINT16_MAX;
                    sqBufRsrcWord2.bits.NUM_RECORDS = UINT32_MAX;

                    sqBufRsrcWord3.bits.DST_SEL_X = BUF_DST_SEL_X;
                    sqBufRsrcWord3.bits.DST_SEL_Y = BUF_DST_SEL_Y;
                    sqBufRsrcWord3.bits.DST_SEL_Z = BUF_DST_SEL_Z;
                    sqBufRsrcWord3.bits.DST_SEL_W = BUF_DST_SEL_W;
                    sqBufRsrcWord3.gfx6.NUM_FORMAT = BUF_NUM_FORMAT_UINT;
                    sqBufRsrcWord3.gfx6.DATA_FORMAT = BUF_DATA_FORMAT_32;
                    LLPC_ASSERT(sqBufRsrcWord3.u32All == 0x24FAC);

                    Value* pDescElem1 = ExtractElementInst::Create(pDescTableAddr,
                        ConstantInt::get(m_pContext->Int32Ty(), 1),
                        "",
                        &callInst);

                    auto pBufDescTy = m_pContext->Int32x4Ty();
                    pDesc = UndefValue::get(pBufDescTy);

                    // DWORD0
                    pDesc = InsertElementInst::Create(pDesc,
                        pDescElem0,
                        ConstantInt::get(m_pContext->Int32Ty(), 0),
                        "",
                        &callInst);

                    // DWORD1
                    pDescElem1 = BinaryOperator::CreateAnd(pDescElem1,
                        ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord1.u32All),
                        "",
                        &callInst);
                    pDesc = InsertElementInst::Create(pDesc,
                        pDescElem1,
                        ConstantInt::get(m_pContext->Int32Ty(), 1),
                        "",
                        &callInst);

                    // DWORD2
                    pDesc = InsertElementInst::Create(pDesc,
                        ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord2.u32All),
                        ConstantInt::get(m_pContext->Int32Ty(), 2),
                        "",
                        &callInst);

                    // DWORD3
                    pDesc = InsertElementInst::Create(pDesc,
                        ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord3.u32All),
                        ConstantInt::get(m_pContext->Int32Ty(), 3),
                        "",
                        &callInst);
                }
            }
            else
            {
                auto pDescOffset = ConstantInt::get(m_pContext->Int32Ty(), descOffset);
                auto pDescSize   = ConstantInt::get(m_pContext->Int32Ty(), descSize, 0);

                Value* pOffset = BinaryOperator::CreateMul(pArrayOffset, pDescSize, "", &callInst);
                pOffset = BinaryOperator::CreateAdd(pOffset, pDescOffset, "", &callInst);

                pOffset = CastInst::CreateZExtOrBitCast(pOffset, m_pContext->Int64Ty(), "", &callInst);

                // Get descriptor address
                std::vector<Value*> idxs;
                idxs.push_back(ConstantInt::get(m_pContext->Int64Ty(), 0, false));
                idxs.push_back(pOffset);

                Value* pDescTablePtr = nullptr;
                uint32_t descSet = pDescSet->getZExtValue();

                if (descSet == InternalResourceTable)
                {
                    pDescTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetInternalGlobalTablePtr();
                }
                else if (descSet == InternalPerShaderTable)
                {
                    pDescTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetInternalPerShaderTablePtr();
                }
                else if ((cl::EnableShadowDescriptorTable) && (nodeType1 == ResourceMappingNodeType::DescriptorFmask))
                {
                    pDescTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetShadowDescTablePtr(descSet);
                }
                else
                {
                    pDescTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetDescTablePtr(descSet);
                }
                auto pDescPtr = GetElementPtrInst::Create(nullptr, pDescTablePtr, idxs, "", &callInst);
                auto pCastedDescPtr = CastInst::Create(Instruction::BitCast, pDescPtr, pDescPtrTy, "", &callInst);

                // Load descriptor
                pCastedDescPtr->setMetadata(m_pContext->MetaIdUniform(), m_pContext->GetEmptyMetadataNode());
                pDesc = new LoadInst(pCastedDescPtr, "", &callInst);
                cast<LoadInst>(pDesc)->setAlignment(16);
            }
        }

        if (pDesc != nullptr)
        {
            callInst.replaceAllUsesWith(pDesc);
            m_descLoadCalls.push_back(&callInst);
            m_descLoadFuncs.insert(pCallee);
        }
    }
}

// =====================================================================================================================
// Gets the descriptor value of the specified descriptor.
const DescriptorRangeValue* PatchDescriptorLoad::GetDescriptorRangeValue(
    ResourceMappingNodeType   nodeType,   // Type of the resource mapping node
    uint32_t                  descSet,    // ID of descriptor set
    uint32_t                  binding     // ID of descriptor binding
    ) const
{
    auto pShaderInfo = m_pContext->GetPipelineShaderInfo(m_shaderStage);
    const DescriptorRangeValue* pDescRangValue = nullptr;
    for (uint32_t i = 0; i < pShaderInfo->descriptorRangeValueCount; ++i)
    {
        auto pRangeValue = &pShaderInfo->pDescriptorRangeValues[i];
        if ((pRangeValue->type == nodeType) &&
            (pRangeValue->set == descSet) &&
            (pRangeValue->binding == binding))
        {
            pDescRangValue = pRangeValue;
            break;
        }
    }
    return pDescRangValue;

}
// =====================================================================================================================
// Calculates the offset and size for the specified descriptor.
//
// Returns the type actually found, or ResourceMappingNodeType::Unknown if not found.
ResourceMappingNodeType PatchDescriptorLoad::CalcDescriptorOffsetAndSize(
    ResourceMappingNodeType   nodeType1,      // The first resource node type for calculation
    ResourceMappingNodeType   nodeType2,      // The second resource node type for calculation (alternative)
    uint32_t                  descSet,        // ID of descriptor set
    uint32_t                  binding,        // ID of descriptor binding
    uint32_t*                 pOffset,        // [out] Calculated offset of the descriptor
    uint32_t*                 pSize,          // [out] Calculated size of the descriptor
    uint32_t*                 pDynDescIdx     // [out] Calculated index of dynamic descriptor
    ) const
{
    auto pShaderInfo = m_pContext->GetPipelineShaderInfo(m_shaderStage);
    bool exist = false;
    ResourceMappingNodeType foundNodeType = ResourceMappingNodeType::Unknown;

    *pDynDescIdx = InvalidValue;
    *pOffset = 0;
    *pSize = 0;

    uint32_t dynDescIdx = 0;

    // Load descriptor from internal tables
    if ((descSet == InternalResourceTable) || (descSet == InternalPerShaderTable))
    {
        *pOffset = binding * DescriptorSizeBuffer;
        *pSize = DescriptorSizeBuffer;
        exist = true;
    }

    if (cl::EnableShadowDescriptorTable && (nodeType1 == ResourceMappingNodeType::DescriptorFmask))
    {
        // NOTE: When shadow descriptor table is enable, we need get F-Mask descriptor node from
        // associated multi-sampled texture resource node. So we have to change nodeType1 to
        // DescriptorResource during the search.
        nodeType1 = ResourceMappingNodeType::DescriptorResource;
    }

    for (uint32_t i = 0; (i < pShaderInfo->userDataNodeCount) && (exist == false); ++i)
    {
        auto pSetNode = &pShaderInfo->pUserDataNodes[i];
        if  ((pSetNode->type == ResourceMappingNodeType::DescriptorResource) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorSampler) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorTexelBuffer) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorFmask) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorBuffer) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorBufferCompact))
        {
            if ((descSet == pSetNode->srdRange.set) &&
                (binding == pSetNode->srdRange.binding) &&
                ((nodeType1 == pSetNode->type) ||
                 (nodeType2 == pSetNode->type) ||
                 ((nodeType1 == ResourceMappingNodeType::DescriptorBuffer) &&
                 (pSetNode->type == ResourceMappingNodeType::DescriptorBufferCompact))))
            {
                *pOffset = pSetNode->offsetInDwords;
                if ((pSetNode->type == ResourceMappingNodeType::DescriptorResource) ||
                    (pSetNode->type == ResourceMappingNodeType::DescriptorFmask))
                {
                    *pSize = DescriptorSizeResource;
                }
                else if (pSetNode->type == ResourceMappingNodeType::DescriptorSampler)
                {
                    *pSize = DescriptorSizeSampler;
                }
                else if ((pSetNode->type == ResourceMappingNodeType::DescriptorBuffer) ||
                         (pSetNode->type == ResourceMappingNodeType::DescriptorTexelBuffer))
                {
                    *pSize = DescriptorSizeBuffer;
                }
                else
                {
                    LLPC_ASSERT(pSetNode->type == ResourceMappingNodeType::DescriptorBufferCompact);
                    *pSize = DescriptorSizeBufferCompact;
                }

                *pDynDescIdx = dynDescIdx;
                exist = true;
                foundNodeType = pSetNode->type;
            }
            ++dynDescIdx;
        }
        else if (pSetNode->type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            for (uint32_t j = 0; (j < pSetNode->tablePtr.nodeCount) && (exist == false); ++j)
            {
                auto pNode = &pSetNode->tablePtr.pNext[j];
                switch (pNode->type)
                {
                case ResourceMappingNodeType::DescriptorResource:
                case ResourceMappingNodeType::DescriptorSampler:
                case ResourceMappingNodeType::DescriptorFmask:
                case ResourceMappingNodeType::DescriptorTexelBuffer:
                case ResourceMappingNodeType::DescriptorBuffer:
                case ResourceMappingNodeType::PushConst:
                    {
                        if ((pNode->srdRange.set == descSet) &&
                            (pNode->srdRange.binding == binding) &&
                            ((nodeType1 == pNode->type) ||
                             (nodeType2 == pNode->type)))
                        {
                            exist = true;
                            foundNodeType = pNode->type;

                            if (pNode->type == ResourceMappingNodeType::DescriptorResource)
                            {
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t);
                                *pSize = DescriptorSizeResource;
                            }
                            else if (pNode->type == ResourceMappingNodeType::DescriptorSampler)
                            {
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t);
                                *pSize = DescriptorSizeSampler;
                            }
                            else if (pNode->type == ResourceMappingNodeType::DescriptorFmask)
                            {
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t);
                                *pSize = DescriptorSizeResource;
                            }
                            else if (pNode->type == ResourceMappingNodeType::PushConst)
                            {
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t);
                                *pSize = pNode->sizeInDwords * sizeof(uint32_t);
                            }
                            else
                            {
                                LLPC_ASSERT((pNode->type == ResourceMappingNodeType::DescriptorBuffer) ||
                                             (pNode->type == ResourceMappingNodeType::DescriptorTexelBuffer));
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t);
                                *pSize = DescriptorSizeBuffer;
                            }
                        }

                        break;
                    }
                case ResourceMappingNodeType::DescriptorCombinedTexture:
                    {
                        // TODO: Check descriptor binding in Vulkan API call to make sure sampler and texture are
                        // bound in this way.
                        if ((pNode->srdRange.set == descSet) &&
                            (pNode->srdRange.binding == binding) &&
                            ((nodeType1 == ResourceMappingNodeType::DescriptorResource) ||
                            (nodeType1 == ResourceMappingNodeType::DescriptorSampler)))
                        {
                            exist = true;
                            foundNodeType = pNode->type;

                            if (nodeType1 == ResourceMappingNodeType::DescriptorResource)
                            {
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t);
                                *pSize   = DescriptorSizeResource + DescriptorSizeSampler;
                            }
                            else
                            {
                                LLPC_ASSERT(nodeType1 == ResourceMappingNodeType::DescriptorSampler);
                                *pOffset = pNode->offsetInDwords * sizeof(uint32_t) + DescriptorSizeResource;
                                *pSize   = DescriptorSizeResource + DescriptorSizeSampler;
                            }
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
        }
    }

    // TODO: We haven't removed the dead code, so we might load inactive descriptors sometimes.
    // Currently, disable this assert.
    //LLPC_ASSERT(exist);

    return foundNodeType;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching opertions for descriptor load.
INITIALIZE_PASS(PatchDescriptorLoad, DEBUG_TYPE,
                "Patch LLVM for descriptor load operations", false, false)
