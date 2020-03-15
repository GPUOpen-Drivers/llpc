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
 * @file  llpcBuilderImpl.cpp
 * @brief LLPC source file: implementation of Llpc::BuilderImpl
 ***********************************************************************************************************************
 */
#include "llvm/IR/IntrinsicsAMDGPU.h"

#include "llpcBuilderImpl.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
BuilderImpl::BuilderImpl(
    BuilderContext* pBuilderContext,  // [in] BuilderContext
    Pipeline*       pPipeline)        // [in] PipelineState (as public superclass Pipeline)
    : BuilderImplBase(pBuilderContext),
      BuilderImplArith(pBuilderContext),
      BuilderImplDesc(pBuilderContext),
      BuilderImplImage(pBuilderContext),
      BuilderImplInOut(pBuilderContext),
      BuilderImplMatrix(pBuilderContext),
      BuilderImplMisc(pBuilderContext),
      BuilderImplSubgroup(pBuilderContext)
{
    m_pPipelineState = reinterpret_cast<PipelineState*>(pPipeline);
    m_pPipelineState->SetNoReplayer();
}

// =====================================================================================================================
// Get the ShaderModes object.
ShaderModes* BuilderImplBase::GetShaderModes()
{
    return m_pPipelineState->GetShaderModes();
}

// =====================================================================================================================
// Create scalar from dot product of scalar or vector FP type. (The dot product of two scalars is their product.)
Value* BuilderImplBase::CreateDotProduct(
    Value* const pVector1,            // [in] The float vector 1
    Value* const pVector2,            // [in] The float vector 2
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Value* pProduct = CreateFMul(pVector1, pVector2);
    if (isa<VectorType>(pProduct->getType()) == false)
    {
        return pProduct;
    }

    const uint32_t compCount = pProduct->getType()->getVectorNumElements();
    Value* pScalar = CreateExtractElement(pProduct, uint64_t(0));

    for (uint32_t i = 1; i < compCount; ++i)
    {
        pScalar = CreateFAdd(pScalar, CreateExtractElement(pProduct, i));
    }

    pScalar->setName(instName);
    return pScalar;
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP operations.
bool BuilderImplBase::SupportDpp() const
{
    return GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major >= 8;
}

// =====================================================================================================================
// Get whether the context we are building in support the bpermute operation.
bool BuilderImplBase::SupportBPermute() const
{
    auto gfxIp = GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major;
    auto supportBPermute = (gfxIp == 8) || (gfxIp == 9);
    auto waveSize = GetPipelineState()->GetShaderWaveSize(GetShaderStageFromFunction(GetInsertBlock()->getParent()));
    supportBPermute = supportBPermute || ((gfxIp == 10) && (waveSize == 32));
    return supportBPermute;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane DPP operations.
bool BuilderImplBase::SupportPermLaneDpp() const
{
    return GetPipelineState()->GetTargetInfo().GetGfxIpVersion().major >= 10;
}

// =====================================================================================================================
// Create an "if..endif" or "if..else..endif" structure. The current basic block becomes the "endif" block, and all
// instructions in that block before the insert point are moved to the "if" block. The insert point is moved to
// the start of the "then" block; the caller can save the insert point before calling this method then restore it
// afterwards to restore the insert point to where it was just after the endif, and still keep its debug location.
// The method returns the branch instruction, whose first branch target is the "then" block and second branch
// target is the "else" block, or "endif" block if no "else" block.
BranchInst* BuilderImplBase::CreateIf(
    Value*        pCondition,   // [in] The "if" condition
    bool          wantElse,     // Whether to generate an "else" block
    const Twine&  instName)     // Base of name for new basic blocks
{
    // Create "if" block and move instructions in current block to it.
    BasicBlock* pEndIfBlock = GetInsertBlock();
    BasicBlock* pIfBlock = BasicBlock::Create(getContext(), "", pEndIfBlock->getParent(), pEndIfBlock);
    pIfBlock->takeName(pEndIfBlock);
    pEndIfBlock->setName(instName + ".endif");
    pIfBlock->getInstList().splice(pIfBlock->end(),
                                   pEndIfBlock->getInstList(),
                                   pEndIfBlock->begin(),
                                   GetInsertPoint());

    // Replace non-phi uses of the original block with the new "if" block.
    SmallVector<Use*, 4> nonPhiUses;
    for (auto& use : pEndIfBlock->uses())
    {
        if (isa<PHINode>(use.getUser()) == false)
        {
            nonPhiUses.push_back(&use);
        }
    }
    for (auto pUse : nonPhiUses)
    {
        pUse->set(pIfBlock);
    }

    // Create "then" and "else" blocks.
    BasicBlock* pThenBlock = BasicBlock::Create(getContext(),
                                                instName + ".then",
                                                pEndIfBlock->getParent(),
                                                pEndIfBlock);
    BasicBlock* pElseBlock = nullptr;
    if (wantElse)
    {
        pElseBlock = BasicBlock::Create(getContext(),
                                        instName + ".else",
                                        pEndIfBlock->getParent(),
                                        pEndIfBlock);
    }

    // Create the branches.
    BranchInst* pBranch = BranchInst::Create(pThenBlock,
                                             pElseBlock != nullptr ? pElseBlock : pEndIfBlock,
                                             pCondition,
                                             pIfBlock);
    pBranch->setDebugLoc(getCurrentDebugLocation());
    BranchInst::Create(pEndIfBlock, pThenBlock)->setDebugLoc(getCurrentDebugLocation());
    if (pElseBlock != nullptr)
    {
        BranchInst::Create(pEndIfBlock, pElseBlock)->setDebugLoc(getCurrentDebugLocation());
    }

    // Set Builder's insert point to the branch at the end of the "then" block.
    SetInsertPoint(pThenBlock->getTerminator());
    return pBranch;
}

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.
// This does not use the current insert point; new code is inserted before and after pNonUniformInst.
Instruction* BuilderImplBase::CreateWaterfallLoop(
    Instruction*        pNonUniformInst,    // [in] The instruction to put in a waterfall loop
    ArrayRef<uint32_t>  operandIdxs,        // The operand index/indices for non-uniform inputs that need to be uniform
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    assert(operandIdxs.empty() == false);

    // For each non-uniform input, try and trace back through a descriptor load to find the non-uniform index
    // used in it. If that fails, we just use the operand value as the index.
    SmallVector<Value*, 2> nonUniformIndices;
    for (uint32_t operandIdx : operandIdxs)
    {
        Value* pNonUniformVal = pNonUniformInst->getOperand(operandIdx);
        if (auto pCall = dyn_cast<CallInst>(pNonUniformVal))
        {
            if (auto pCalledFunc = pCall->getCalledFunction())
            {
                if (pCalledFunc->getName().startswith(LlpcName::DescriptorLoadFromPtr))
                {
                    pCall = dyn_cast<CallInst>(pCall->getArgOperand(0)); // The descriptor pointer
                    if ((pCall != nullptr) &&
                        pCall->getCalledFunction()->getName().startswith(LlpcName::DescriptorIndex))
                    {
                        pNonUniformVal = pCall->getArgOperand(1); // The index operand
                    }
                }
            }
        }
        nonUniformIndices.push_back(pNonUniformVal);
    }

    // Save Builder's insert point, and set it to insert new code just before pNonUniformInst.
    auto savedInsertPoint = saveIP();
    SetInsertPoint(pNonUniformInst);

    // Get the waterfall index. If there are two indices (image resource+sampler case), join them into
    // a single struct.
    Value* pWaterfallIndex = nonUniformIndices[0];
    if (nonUniformIndices.size() > 1)
    {
        assert(nonUniformIndices.size() == 2);
        SmallVector<Type*, 2> indexTys;
        for (Value* pNonUniformIndex : nonUniformIndices)
        {
            indexTys.push_back(pNonUniformIndex->getType());
        }
        auto pWaterfallIndexTy = StructType::get(getContext(), indexTys);
        pWaterfallIndex = UndefValue::get(pWaterfallIndexTy);
        for (uint32_t structIndex = 0; structIndex < nonUniformIndices.size(); ++structIndex)
        {
            pWaterfallIndex = CreateInsertValue(pWaterfallIndex, nonUniformIndices[structIndex], structIndex);
        }
    }

    // Start the waterfall loop using the waterfall index.
    Value* pWaterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin,
                                             pWaterfallIndex->getType(),
                                             pWaterfallIndex,
                                             nullptr,
                                             instName);

    // Scalarize each non-uniform operand of the instruction.
    for (uint32_t operandIdx : operandIdxs)
    {
        Value* pDesc = pNonUniformInst->getOperand(operandIdx);
        auto pDescTy = pDesc->getType();
        pDesc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane,
                                { pDescTy, pDescTy },
                                { pWaterfallBegin, pDesc },
                                nullptr,
                                instName);
        if (pNonUniformInst->getType()->isVoidTy())
        {
            // The buffer/image operation we are waterfalling is a store with no return value. Use
            // llvm.amdgcn.waterfall.last.use on the descriptor.
            pDesc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_last_use,
                                    pDescTy,
                                    { pWaterfallBegin, pDesc },
                                    nullptr,
                                    instName);
        }
        // Replace the descriptor operand in the buffer/image operation.
        pNonUniformInst->setOperand(operandIdx, pDesc);
    }

    Instruction* pResultValue = pNonUniformInst;

    // End the waterfall loop (as long as pNonUniformInst is not a store with no result).
    if (pNonUniformInst->getType()->isVoidTy() == false)
    {
        SetInsertPoint(pNonUniformInst->getNextNode());
        SetCurrentDebugLocation(pNonUniformInst->getDebugLoc());

        Use* pUseOfNonUniformInst = nullptr;
        Type* pWaterfallEndTy = pResultValue->getType();
        if (auto pVecTy = dyn_cast<VectorType>(pWaterfallEndTy))
        {
            if (pVecTy->getElementType()->isIntegerTy(8))
            {
                // ISel does not like waterfall.end with vector of i8 type, so cast if necessary.
                assert((pVecTy->getNumElements() % 4) == 0);
                pWaterfallEndTy = getInt32Ty();
                if (pVecTy->getNumElements() != 4)
                {
                    pWaterfallEndTy = VectorType::get(getInt32Ty(), pVecTy->getNumElements() / 4);
                }
                pResultValue = cast<Instruction>(CreateBitCast(pResultValue, pWaterfallEndTy, instName));
                pUseOfNonUniformInst = &pResultValue->getOperandUse(0);
            }
        }
        pResultValue = CreateIntrinsic(Intrinsic::amdgcn_waterfall_end,
                                  pWaterfallEndTy,
                                  { pWaterfallBegin, pResultValue },
                                  nullptr,
                                  instName);
        if (pUseOfNonUniformInst == nullptr)
        {
            pUseOfNonUniformInst = &pResultValue->getOperandUse(1);
        }
        if (pWaterfallEndTy != pNonUniformInst->getType())
        {
            pResultValue = cast<Instruction>(CreateBitCast(pResultValue, pNonUniformInst->getType(), instName));
        }

        // Replace all uses of pNonUniformInst with the result of this code.
        *pUseOfNonUniformInst = UndefValue::get(pNonUniformInst->getType());
        pNonUniformInst->replaceAllUsesWith(pResultValue);
        *pUseOfNonUniformInst = pNonUniformInst;
    }

    // Restore Builder's insert point.
    restoreIP(savedInsertPoint);
    return pResultValue;
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector unary operation
Value* BuilderImplBase::Scalarize(
    Value*                        pValue,     // [in] Input value
    std::function<Value*(Value*)> callback)   // [in] Callback function
{
    if (auto pVecTy = dyn_cast<VectorType>(pValue->getType()))
    {
        Value* pResult0 = callback(CreateExtractElement(pValue, uint64_t(0)));
        Value* pResult = UndefValue::get(VectorType::get(pResult0->getType(), pVecTy->getNumElements()));
        pResult = CreateInsertElement(pResult, pResult0, uint64_t(0));
        for (uint32_t idx = 1, end = pVecTy->getNumElements(); idx != end; ++idx)
        {
            pResult = CreateInsertElement(pResult, callback(CreateExtractElement(pValue, idx)), idx);
        }
        return pResult;
    }
    Value* pResult = callback(pValue);
    return pResult;
}

// =====================================================================================================================
// Helper method to scalarize in pairs a possibly vector unary operation. The callback function is called
// with vec2 input, even if the input here is scalar.
Value* BuilderImplBase::ScalarizeInPairs(
    Value*                        pValue,     // [in] Input value
    std::function<Value*(Value*)> callback)   // [in] Callback function
{
    if (auto pVecTy = dyn_cast<VectorType>(pValue->getType()))
    {
        Value* pInComps = CreateShuffleVector(pValue, pValue, { 0, 1 });
        Value* pResultComps = callback(pInComps);
        Value* pResult = UndefValue::get(VectorType::get(pResultComps->getType()->getScalarType(),
                                                         pVecTy->getNumElements()));
        pResult = CreateInsertElement(pResult, CreateExtractElement(pResultComps, uint64_t(0)), uint64_t(0));
        if (pVecTy->getNumElements() > 1)
        {
            pResult = CreateInsertElement(pResult, CreateExtractElement(pResultComps, 1), 1);
        }

        for (uint32_t idx = 2, end = pVecTy->getNumElements(); idx < end; idx += 2)
        {
            uint32_t indices[2] = { idx, idx + 1 };
            pInComps = CreateShuffleVector(pValue, pValue, indices);
            pResultComps = callback(pInComps);
            pResult = CreateInsertElement(pResult, CreateExtractElement(pResultComps, uint64_t(0)), idx);
            if (idx + 1 < end)
            {
                pResult = CreateInsertElement(pResult, CreateExtractElement(pResultComps, 1), idx + 1);
            }
        }
        return pResult;
    }

    // For the scalar case, we need to create a vec2.
    Value* pInComps = UndefValue::get(VectorType::get(pValue->getType(), 2));
    pInComps = CreateInsertElement(pInComps, pValue, uint64_t(0));
    pInComps = CreateInsertElement(pInComps, Constant::getNullValue(pValue->getType()), 1);
    Value* pResult = callback(pInComps);
    return CreateExtractElement(pResult, uint64_t(0));
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector binary operation
Value* BuilderImplBase::Scalarize(
    Value*                                pValue0,    // [in] Input value 0
    Value*                                pValue1,    // [in] Input value 1
    std::function<Value*(Value*, Value*)> callback)   // [in] Callback function
{
    if (auto pVecTy = dyn_cast<VectorType>(pValue0->getType()))
    {
        Value* pResult0 = callback(CreateExtractElement(pValue0, uint64_t(0)),
                                   CreateExtractElement(pValue1, uint64_t(0)));
        Value* pResult = UndefValue::get(VectorType::get(pResult0->getType(), pVecTy->getNumElements()));
        pResult = CreateInsertElement(pResult, pResult0, uint64_t(0));
        for (uint32_t idx = 1, end = pVecTy->getNumElements(); idx != end; ++idx)
        {
            pResult = CreateInsertElement(pResult,
                                          callback(CreateExtractElement(pValue0, idx),
                                                   CreateExtractElement(pValue1, idx)),
                                          idx);
        }
        return pResult;
    }
    Value* pResult = callback(pValue0, pValue1);
    return pResult;
}

// =====================================================================================================================
// Helper method to scalarize a possibly vector trinary operation
Value* BuilderImplBase::Scalarize(
    Value*                                        pValue0,    // [in] Input value 0
    Value*                                        pValue1,    // [in] Input value 1
    Value*                                        pValue2,    // [in] Input value 2
    std::function<Value*(Value*, Value*, Value*)> callback)   // [in] Callback function
{
    if (auto pVecTy = dyn_cast<VectorType>(pValue0->getType()))
    {
        Value* pResult0 = callback(CreateExtractElement(pValue0, uint64_t(0)),
                                   CreateExtractElement(pValue1, uint64_t(0)),
                                   CreateExtractElement(pValue2, uint64_t(0)));
        Value* pResult = UndefValue::get(VectorType::get(pResult0->getType(), pVecTy->getNumElements()));
        pResult = CreateInsertElement(pResult, pResult0, uint64_t(0));
        for (uint32_t idx = 1, end = pVecTy->getNumElements(); idx != end; ++idx)
        {
            pResult = CreateInsertElement(pResult,
                                          callback(CreateExtractElement(pValue0, idx),
                                                   CreateExtractElement(pValue1, idx),
                                                   CreateExtractElement(pValue2, idx)),
                                          idx);
        }
        return pResult;
    }
    Value* pResult = callback(pValue0, pValue1, pValue2);
    return pResult;
}

