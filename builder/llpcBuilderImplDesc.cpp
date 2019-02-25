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
 * @file  llpcBuilderImplDesc.cpp
 * @brief LLPC source file: implementation of Builder methods for descriptor loads and waterfall loops
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcContext.h"
#include "llpcInternal.h"

#include "llvm/IR/Intrinsics.h"

#define DEBUG_TYPE "llpc-builder-impl-desc"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.
// This does not use the current insert point; new code is inserted before and after pNonUniformInst.
Instruction* BuilderImplDesc::CreateWaterfallLoop(
    Instruction*        pNonUniformInst,    // [in] The instruction to put in a waterfall loop
    ArrayRef<uint32_t>  operandIdxs,        // The operand index/indices for non-uniform inputs that need to be uniform
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    LLPC_ASSERT(operandIdxs.empty() == false);

    // For each non-uniform input, try and trace back through a descriptor load to find the non-uniform index
    // used in it. If that fails, we just use the operand value as the index.
    SmallVector<Value*, 2> nonUniformIndices;
    for (uint32_t operandIdx : operandIdxs)
    {
        Value* pNonUniformVal = pNonUniformInst->getOperand(operandIdx);
        for (;;)
        {
            if (auto pGetElemPtr = dyn_cast<GetElementPtrInst>(pNonUniformVal))
            {
                pNonUniformVal = pGetElemPtr->getPointerOperand();
                continue;
            }

            if (auto pCall = dyn_cast<CallInst>(pNonUniformVal))
            {
                if (auto pCalledFunc = pCall->getCalledFunction())
                {
                    if (pCalledFunc->getName().startswith(LlpcName::DescriptorLoadPrefix))
                    {
                        pNonUniformVal = pCall->getArgOperand(2); // The index operand.
                        break;
                    }
                }
            }

            break;
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
        LLPC_ASSERT(nonUniformIndices.size() == 2);
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
                LLPC_ASSERT((pVecTy->getNumElements() % 4) == 0);
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
// Create a load of a buffer descriptor.
// TODO: Currently supports returning non-fat-pointer <4 x i32> descriptor when pPointeeTy is nullptr. It is intended
// to remove that functionality once LLPC has switched to fat pointers.
Value* BuilderImplDesc::CreateLoadBufferDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pBlockOffset,     // [in] Buffer block offset
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    Type*         pPointeeTy,       // [in] Type that the returned pointer should point to (nullptr to return a
                                    //    non-fat-pointer <4 x i32> descriptor)
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    LLPC_ASSERT(pPointeeTy == nullptr && "Fat pointers not supported yet");
    Instruction* pInsertPos = &*GetInsertPoint();

    if ((isa<Constant>(pBlockOffset) == false) && (isNonUniform == false) &&
        (getContext().GetGfxIpVersion().major > 6))
    {
        // NOTE: GFX6 encounters GPU hang with this optimization enabled. So we should skip it.
        pBlockOffset = CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pBlockOffset);
    }

    // TODO: This currently creates a call to the llpc.descriptor.* function. A future commit will change it to
    // look up the descSet/binding and generate the code directly. Note that we always set isNonUniform here
    // to false because nothing now uses it in patching. Waterfall code is now added by lowering calling
    // Builder::CreateWaterfallLoop.
    auto pBufDescLoadCall = EmitCall(pInsertPos->getModule(),
                                     LlpcName::DescriptorLoadBuffer,
                                     VectorType::get(getInt32Ty(), 4),
                                     {
                                         getInt32(descSet),
                                         getInt32(binding),
                                         pBlockOffset,
                                         getInt1(false), // isNonUniform
                                     },
                                     NoAttrib,
                                     pInsertPos);
    pBufDescLoadCall->setName(instName);
    return pBufDescLoadCall;
}

// =====================================================================================================================
// Create a load of the spill table pointer for push constants.
Value* BuilderImplDesc::CreateLoadSpillTablePtr(
    Type*         pSpillTableTy,    // [in] Type of the spill table that the returned pointer will point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    auto pSpillTablePtrTy = PointerType::get(pSpillTableTy, ADDR_SPACE_CONST);
    // TODO: This currently creates a call to the llpc.descriptor.* function. A future commit will change it to
    // generate the code directly.
    Instruction* pInsertPos = &*GetInsertPoint();
    auto pSpillTableLoadCall = EmitCall(pInsertPos->getModule(),
                                        LlpcName::DescriptorLoadSpillTable,
                                        pSpillTablePtrTy,
                                        {},
                                        NoAttrib,
                                        pInsertPos);
    pSpillTableLoadCall->setName(instName);
    return pSpillTableLoadCall;
}

