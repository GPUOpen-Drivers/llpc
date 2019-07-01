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

#include "llvm/IR/IRBuilder.h"
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
    initializePipelineStateWrapperPass(*PassRegistry::getPassRegistry());
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

    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);

    // Invoke handling of "call" instruction
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(static_cast<ShaderStage>(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = static_cast<ShaderStage>(shaderStage);
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

    // Remove dead llpc.descriptor.point* and llpc.descriptor.index calls that were not
    // processed by the code above. That happens if they were never used in llpc.descriptor.load.from.ptr.
    SmallVector<Function*, 4> deadDescFuncs;
    for (Function& func : *m_pModule)
    {
        if (func.isDeclaration() && (func.getName().startswith(LlpcName::DescriptorGetPtrPrefix) ||
                                     func.getName().startswith(LlpcName::DescriptorIndex)))
        {
            deadDescFuncs.push_back(&func);
        }
    }
    for (Function* pFunc : deadDescFuncs)
    {
        while (pFunc->use_empty() == false)
        {
            pFunc->use_begin()->set(UndefValue::get(pFunc->getType()));
        }
        pFunc->eraseFromParent();
    }

    m_pipelineSysValues.Clear();
    return m_changed;
}

// =====================================================================================================================
// Process "llpc.descriptor.load.from.ptr" call.
// Currently we assume that everything is inlined, so here we can trace the pointer back through any
// "llpc.descriptor.index" calls back to an "llpc.descriptor.point.*" call.
// In the future, we want to remove the assumption that everything is inlined. To do that, we will need to
// do some internal rearrangement to the descriptor load code. That will probably happen at the same time
// as moving descriptor load code up into the builder.
void PatchDescriptorLoad::ProcessLoadDescFromPtr(
    CallInst* pLoadFromPtr)   // [in] Call to llpc.descriptor.load.from.ptr
{
    m_pEntryPoint = pLoadFromPtr->getFunction();
    IRBuilder<> builder(*m_pContext);
    builder.SetInsertPoint(pLoadFromPtr);
    Value* pIndex = builder.getInt32(0);

    auto pLoadPtr = cast<CallInst>(pLoadFromPtr->getOperand(0));
    while (pLoadPtr->getCalledFunction()->getName().startswith(LlpcName::DescriptorIndex))
    {
        pIndex = builder.CreateAdd(pIndex, pLoadPtr->getOperand(1));
        pLoadPtr = cast<CallInst>(pLoadPtr->getOperand(0));
    }

    LLPC_ASSERT(pLoadPtr->getCalledFunction()->getName().startswith(LlpcName::DescriptorGetPtrPrefix));

    uint32_t descSet = cast<ConstantInt>(pLoadPtr->getOperand(0))->getZExtValue();
    uint32_t binding = cast<ConstantInt>(pLoadPtr->getOperand(1))->getZExtValue();
    Value* pDesc = LoadDescriptor(*pLoadPtr, descSet, binding, pIndex, pLoadFromPtr);

    pLoadFromPtr->replaceAllUsesWith(pDesc);

    Instruction* pDeadInst = pLoadFromPtr;
    while ((pDeadInst != nullptr) && pDeadInst->use_empty())
    {
        Instruction* pNextDeadInst = nullptr;
        if ((isa<PHINode>(pDeadInst) == false) && (pDeadInst->getNumOperands() >= 1))
        {
            pNextDeadInst = dyn_cast<Instruction>(pDeadInst->getOperand(0));
        }
        pDeadInst->eraseFromParent();
        pDeadInst = pNextDeadInst;
    }
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

    StringRef mangledName = pCallee->getName();

    std::string descLoadPrefix = LlpcName::DescriptorLoadPrefix;
    bool isDescLoad = mangledName.startswith(LlpcName::DescriptorLoadPrefix);
    if (isDescLoad == false)
    {
        return; // Not descriptor load
    }

    if (mangledName.startswith(LlpcName::DescriptorGetPtrPrefix) ||
        mangledName.startswith(LlpcName::DescriptorIndex))
    {
        // Ignore llpc.descriptor.point.* calls and llpc.descriptor.index calls, as they
        // get processed at llpc.descriptor.load.from.ptr.
        return;
    }

    if (mangledName.startswith(LlpcName::DescriptorLoadFromPtr))
    {
        ProcessLoadDescFromPtr(&callInst);
        return;
    }

    // Descriptor loading should be inlined and stay in shader entry-point
    LLPC_ASSERT(callInst.getParent()->getParent() == m_pEntryPoint);

    m_changed = true;

    // Create the descriptor load (unless the call has no uses).
    if (callInst.use_empty() == false)
    {
        Value* pDesc = nullptr;
        if (mangledName == LlpcName::DescriptorLoadSpillTable)
        {
            pDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetSpilledPushConstTablePtr(m_pPipelineState);
        }
        else
        {
            uint32_t descSet = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
            uint32_t binding = cast<ConstantInt>(callInst.getOperand(1))->getZExtValue();
            Value* pArrayOffset = callInst.getOperand(2); // Offset for arrayed resource (index)
            pDesc = LoadDescriptor(callInst, descSet, binding, pArrayOffset, &callInst);
        }

        // Replace the call with the loaded descriptor.
        callInst.replaceAllUsesWith(pDesc);
    }

    m_descLoadCalls.push_back(&callInst);
    m_descLoadFuncs.insert(pCallee);
}

// =====================================================================================================================
// Generate the code for the descriptor load
Value* PatchDescriptorLoad::LoadDescriptor(
    CallInst&     callInst,       // [in] The llpc.descriptor.load.* or llpc.descriptor.point.* call being replaced
    uint32_t      descSet,        // Descriptor set
    uint32_t      binding,        // Binding
    Value*        pArrayOffset,   // [in] Index in descriptor array
    Instruction*  pInsertPoint)   // [in] Insert point
{
    StringRef mangledName = callInst.getCalledFunction()->getName();
    Type* pDescPtrTy = nullptr;
    ResourceMappingNodeType nodeType1 = ResourceMappingNodeType::Unknown;
    ResourceMappingNodeType nodeType2 = ResourceMappingNodeType::Unknown;

    // TODO: The address space ID 2 is a magic number. We have to replace it with defined LLPC address space ID.
    if (mangledName == LlpcName::DescriptorGetResourcePtr)
    {
        pDescPtrTy = m_pContext->Int32x8Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorResource;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorGetSamplerPtr)
    {
        pDescPtrTy = m_pContext->Int32x4Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorSampler;
        nodeType2 = nodeType1;
    }
    else if (mangledName == LlpcName::DescriptorGetFmaskPtr)
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
    else if (mangledName == LlpcName::DescriptorGetTexelBufferPtr)
    {
        pDescPtrTy = m_pContext->Int32x4Ty()->getPointerTo(ADDR_SPACE_CONST);
        nodeType1 = ResourceMappingNodeType::DescriptorTexelBuffer;
        nodeType2 = nodeType1;
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    LLPC_ASSERT(nodeType1 != ResourceMappingNodeType::Unknown);

    // Calculate descriptor offset (in bytes)
    uint32_t descOffset = 0;
    uint32_t descSize   = 0;
    uint32_t dynDescIdx = InvalidValue;
    Value*   pDesc = nullptr;
    Constant* pDescRangeValue = nullptr;

    if (nodeType1 == ResourceMappingNodeType::DescriptorSampler)
    {
        pDescRangeValue = GetDescriptorRangeValue(nodeType1, descSet, binding);
    }

    if (pDescRangeValue != nullptr)
    {
        // Descriptor range value (immutable sampler in Vulkan). pDescRangeValue is a constant array of
        // <4 x i32> descriptors.
        IRBuilder<> builder(*m_pContext);
        builder.SetInsertPoint(pInsertPoint);

        if (pDescRangeValue->getType()->getArrayNumElements() == 1)
        {
            // Immutable descriptor array is size 1, so we can assume index 0.
            pDesc = builder.CreateExtractValue(pDescRangeValue, 0);
        }
        else if (auto pConstArrayOffset = dyn_cast<ConstantInt>(pArrayOffset))
        {
            // Array index is constant.
            uint32_t index = pConstArrayOffset->getZExtValue();
            pDesc = builder.CreateExtractValue(pDescRangeValue, {index});
        }
        else
        {
            // Array index is variable.
            GlobalVariable* pDescs = new GlobalVariable(*m_pModule,
                                                        pDescRangeValue->getType(),
                                                        true, // isConstant
                                                        GlobalValue::InternalLinkage,
                                                        pDescRangeValue,
                                                        "",
                                                        nullptr,
                                                        GlobalValue::NotThreadLocal,
                                                        ADDR_SPACE_CONST);

            auto pDescPtr = builder.CreateGEP(pDescs, {builder.getInt32(0), pArrayOffset});
            pDesc = builder.CreateLoad(pDescPtr);
        }
    }

    if (pDesc == nullptr)
    {
        auto foundNodeType = CalcDescriptorOffsetAndSize(nodeType1,
                                                         nodeType2,
                                                         descSet,
                                                         binding,
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
            pDesc = m_pipelineSysValues.Get(m_pEntryPoint)->GetDynamicDesc(m_pPipelineState, dynDescIdx);
            if (pDesc != nullptr)
            {
                auto pDescTy = VectorType::get(m_pContext->Int32Ty(), descSizeInDword);
                if (pDesc->getType() != pDescTy)
                {
                    // Array dynamic descriptor
                    Value* pDynDesc = UndefValue::get(pDescTy);
                    auto pDescStride = ConstantInt::get(m_pContext->Int32Ty(), descSizeInDword);
                    auto pIndex = BinaryOperator::CreateMul(pArrayOffset, pDescStride, "", pInsertPoint);
                    for (uint32_t i = 0; i < descSizeInDword; ++i)
                    {
                        auto pDescElem = ExtractElementInst::Create(pDesc, pIndex, "", pInsertPoint);
                        pDynDesc = InsertElementInst::Create(pDynDesc,
                                                             pDescElem,
                                                             ConstantInt::get(m_pContext->Int32Ty(), i),
                                                             "",
                                                             pInsertPoint);
                        pIndex = BinaryOperator::CreateAdd(pIndex,
                                                           ConstantInt::get(m_pContext->Int32Ty(), 1),
                                                           "",
                                                           pInsertPoint);
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
                        pInsertPoint);

                    Value* pDescElem1 = ExtractElementInst::Create(pDesc,
                        ConstantInt::get(m_pContext->Int32Ty(), 1),
                        "",
                        pInsertPoint);

                    // Build normal buffer descriptor
                    auto pBufDescTy = m_pContext->Int32x4Ty();
                    Value* pBufDesc = UndefValue::get(pBufDescTy);

                    // DWORD0
                    pBufDesc = InsertElementInst::Create(pBufDesc,
                        pDescElem0,
                        ConstantInt::get(m_pContext->Int32Ty(), 0),
                        "",
                        pInsertPoint);

                    // DWORD1
                    SqBufRsrcWord1 sqBufRsrcWord1 = {};
                    sqBufRsrcWord1.bits.BASE_ADDRESS_HI = UINT16_MAX;

                    pDescElem1 = BinaryOperator::CreateAnd(pDescElem1,
                        ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord1.u32All),
                        "",
                        pInsertPoint);
                    pBufDesc = InsertElementInst::Create(pBufDesc,
                        pDescElem1,
                        ConstantInt::get(m_pContext->Int32Ty(), 1),
                        "",
                        pInsertPoint);

                    // DWORD2
                    SqBufRsrcWord2 sqBufRsrcWord2 = {};
                    sqBufRsrcWord2.bits.NUM_RECORDS = UINT32_MAX;

                    pBufDesc = InsertElementInst::Create(pBufDesc,
                        ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord2.u32All),
                        ConstantInt::get(m_pContext->Int32Ty(), 2),
                        "",
                        pInsertPoint);

                    // DWORD3
#if LLPC_BUILD_GFX10
                    const GfxIpVersion gfxIp = m_pContext->GetGfxIpVersion();
                    if (gfxIp.major < 10)
#endif
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
#if LLPC_BUILD_GFX10
                    else if (gfxIp.major == 10)
                    {
                        SqBufRsrcWord3 sqBufRsrcWord3 = {};
                        sqBufRsrcWord3.bits.DST_SEL_X = BUF_DST_SEL_X;
                        sqBufRsrcWord3.bits.DST_SEL_Y = BUF_DST_SEL_Y;
                        sqBufRsrcWord3.bits.DST_SEL_Z = BUF_DST_SEL_Z;
                        sqBufRsrcWord3.bits.DST_SEL_W = BUF_DST_SEL_W;
                        sqBufRsrcWord3.gfx10.FORMAT = BUF_FORMAT_32_UINT;
                        sqBufRsrcWord3.gfx10.RESOURCE_LEVEL = 1;
                        sqBufRsrcWord3.gfx10.OOB_SELECT = 2;
                        LLPC_ASSERT(sqBufRsrcWord3.u32All == 0x21014FAC);

                        pBufDesc = InsertElementInst::Create(pBufDesc,
                            ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord3.u32All),
                            ConstantInt::get(m_pContext->Int32Ty(), 3),
                            "",
                            &callInst);
                    }
                    else
                    {
                        LLPC_NOT_IMPLEMENTED();
                    }
#endif

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
                m_pipelineSysValues.Get(m_pEntryPoint)->GetDescTablePtr(m_pPipelineState, descSet);

            Value* pDescTableAddr = new PtrToIntInst(pDescTablePtr,
                                                     m_pContext->Int64Ty(),
                                                     "",
                                                     pInsertPoint);

            pDescTableAddr = new BitCastInst(pDescTableAddr, m_pContext->Int32x2Ty(), "", pInsertPoint);

            // Extract descriptor table address
            Value* pDescElem0 = ExtractElementInst::Create(pDescTableAddr,
                ConstantInt::get(m_pContext->Int32Ty(), 0),
                "",
                pInsertPoint);

            auto pDescOffset = ConstantInt::get(m_pContext->Int32Ty(), descOffset);

            pDescElem0 = BinaryOperator::CreateAdd(pDescElem0, pDescOffset, "", pInsertPoint);

            if (pDescPtrTy == nullptr)
            {
                // Load the address of inline constant buffer
                pDesc = InsertElementInst::Create(pDescTableAddr,
                    pDescElem0,
                    ConstantInt::get(m_pContext->Int32Ty(), 0),
                    "",
                    pInsertPoint);
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
                    pInsertPoint);

                auto pBufDescTy = m_pContext->Int32x4Ty();
                pDesc = UndefValue::get(pBufDescTy);

                // DWORD0
                pDesc = InsertElementInst::Create(pDesc,
                    pDescElem0,
                    ConstantInt::get(m_pContext->Int32Ty(), 0),
                    "",
                    pInsertPoint);

                // DWORD1
                pDescElem1 = BinaryOperator::CreateAnd(pDescElem1,
                    ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord1.u32All),
                    "",
                    pInsertPoint);
                pDesc = InsertElementInst::Create(pDesc,
                    pDescElem1,
                    ConstantInt::get(m_pContext->Int32Ty(), 1),
                    "",
                    pInsertPoint);

                // DWORD2
                pDesc = InsertElementInst::Create(pDesc,
                    ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord2.u32All),
                    ConstantInt::get(m_pContext->Int32Ty(), 2),
                    "",
                    pInsertPoint);

                // DWORD3
                pDesc = InsertElementInst::Create(pDesc,
                    ConstantInt::get(m_pContext->Int32Ty(), sqBufRsrcWord3.u32All),
                    ConstantInt::get(m_pContext->Int32Ty(), 3),
                    "",
                    pInsertPoint);
            }
        }
        else
        {
            auto pDescOffset = ConstantInt::get(m_pContext->Int32Ty(), descOffset);
            auto pDescSize   = ConstantInt::get(m_pContext->Int32Ty(), descSize, 0);

            Value* pOffset = BinaryOperator::CreateMul(pArrayOffset, pDescSize, "", pInsertPoint);
            pOffset = BinaryOperator::CreateAdd(pOffset, pDescOffset, "", pInsertPoint);

            pOffset = CastInst::CreateZExtOrBitCast(pOffset, m_pContext->Int64Ty(), "", pInsertPoint);

            // Get descriptor address
            std::vector<Value*> idxs;
            idxs.push_back(ConstantInt::get(m_pContext->Int64Ty(), 0, false));
            idxs.push_back(pOffset);

            Value* pDescTablePtr = nullptr;

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
                pDescTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->
                                  GetShadowDescTablePtr(m_pPipelineState, descSet);
            }
            else
            {
                pDescTablePtr = m_pipelineSysValues.Get(m_pEntryPoint)->GetDescTablePtr(m_pPipelineState, descSet);
            }
            auto pDescPtr = GetElementPtrInst::Create(nullptr, pDescTablePtr, idxs, "", pInsertPoint);
            auto pCastedDescPtr = CastInst::Create(Instruction::BitCast, pDescPtr, pDescPtrTy, "", pInsertPoint);

            // Load descriptor
            pCastedDescPtr->setMetadata(m_pContext->MetaIdUniform(), m_pContext->GetEmptyMetadataNode());
            pDesc = new LoadInst(pCastedDescPtr, "", pInsertPoint);
            cast<LoadInst>(pDesc)->setAlignment(16);
        }
    }

    return pDesc;
}

// =====================================================================================================================
// Gets the descriptor value of the specified descriptor.
Constant* PatchDescriptorLoad::GetDescriptorRangeValue(
    ResourceMappingNodeType   nodeType,   // Type of the resource mapping node
    uint32_t                  descSet,    // ID of descriptor set
    uint32_t                  binding     // ID of descriptor binding
    ) const
{
    auto userDataNodes = m_pPipelineState->GetUserDataNodes();
    for (const ResourceNode& node : userDataNodes)
    {
        if (node.type == ResourceMappingNodeType::DescriptorTableVaPtr)
        {
            for (const ResourceNode& innerNode : node.innerTable)
            {
                if (((innerNode.type == ResourceMappingNodeType::DescriptorSampler) ||
                     (innerNode.type == ResourceMappingNodeType::DescriptorCombinedTexture)) &&
                    (innerNode.set == descSet) &&
                    (innerNode.binding == binding))
                {
                    return innerNode.pImmutableValue;
                }
            }
        }
        else if (((node.type == ResourceMappingNodeType::DescriptorSampler) ||
                  (node.type == ResourceMappingNodeType::DescriptorCombinedTexture)) &&
                 (node.set == descSet) &&
                 (node.binding == binding))
        {
            return node.pImmutableValue;
        }
    }
    return nullptr;
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

    auto userDataNodes = m_pPipelineState->GetUserDataNodes();
    for (uint32_t i = 0; (i < userDataNodes.size()) && (exist == false); ++i)
    {
        auto pSetNode = &userDataNodes[i];
        if  ((pSetNode->type == ResourceMappingNodeType::DescriptorResource) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorSampler) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorTexelBuffer) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorFmask) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorBuffer) ||
             (pSetNode->type == ResourceMappingNodeType::DescriptorBufferCompact))
        {
            if ((descSet == pSetNode->set) &&
                (binding == pSetNode->binding) &&
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
            for (uint32_t j = 0; (j < pSetNode->innerTable.size()) && (exist == false); ++j)
            {
                auto pNode = &pSetNode->innerTable[j];
                switch (pNode->type)
                {
                case ResourceMappingNodeType::DescriptorResource:
                case ResourceMappingNodeType::DescriptorSampler:
                case ResourceMappingNodeType::DescriptorFmask:
                case ResourceMappingNodeType::DescriptorTexelBuffer:
                case ResourceMappingNodeType::DescriptorBuffer:
                case ResourceMappingNodeType::PushConst:
                    {
                        if ((pNode->set == descSet) &&
                            (pNode->binding == binding) &&
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
                        if ((pNode->set == descSet) &&
                            (pNode->binding == binding) &&
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
