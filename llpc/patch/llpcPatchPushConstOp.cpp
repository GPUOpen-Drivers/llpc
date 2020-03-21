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
 * @file  llpcPatchPushConstOp.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchPushConstOp.
 ***********************************************************************************************************************
 */
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcBuilder.h"
#include "llpcIntrinsDefs.h"
#include "llpcPatchPushConstOp.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"

#define DEBUG_TYPE "llpc-patch-push-const"

using namespace llvm;
using namespace lgc;

namespace lgc
{

// =====================================================================================================================
// Initializes static members.
char PatchPushConstOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for push constant operations
ModulePass* CreatePatchPushConstOp()
{
    return new PatchPushConstOp();
}

// =====================================================================================================================
PatchPushConstOp::PatchPushConstOp()
    :
    Patch(ID)
{
}

// =====================================================================================================================
// Get the analysis usage of this pass.
void PatchPushConstOp::getAnalysisUsage(
    AnalysisUsage& analysisUsage // [out] The analysis usage.
    ) const
{
    analysisUsage.addRequired<PipelineStateWrapper>();
    analysisUsage.addRequired<PipelineShaders>();
    analysisUsage.addPreserved<PipelineShaders>();
    analysisUsage.setPreservesCFG();
}

// =====================================================================================================================
// Executes this SPIR-V patching pass on the specified LLVM module.
bool PatchPushConstOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Push-Const-Op\n");

    Patch::Init(&module);

    SmallVector<Function*, 4> spillTableFuncs;
    for (auto& func : module)
    {
        if (func.getName().startswith(lgcName::DescriptorLoadSpillTable))
        {
            spillTableFuncs.push_back(&func);
        }
    }

    // If there was no spill table load, bail.
    if (spillTableFuncs.empty())
    {
        return false;
    }

    m_pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    const PipelineShaders& pipelineShaders = getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pipelineShaders.GetEntryPoint(static_cast<ShaderStage>(shaderStage));

        // If we don't have an entry point for the shader stage, bail.
        if (m_pEntryPoint == nullptr)
        {
            continue;
        }

        m_shaderStage = static_cast<ShaderStage>(shaderStage);

        for (Function* pFunc : spillTableFuncs)
        {
            for (User* const pUser : pFunc->users())
            {
                CallInst* const pCall = dyn_cast<CallInst>(pUser);

                // If the user is not a call, bail.
                if (pCall == nullptr)
                {
                    continue;
                }

                // If the call is not in the entry point, bail.
                if (pCall->getFunction() != m_pEntryPoint)
                {
                    continue;
                }

                visitCallInst(*pCall);
            }
        }
    }

    const bool changed = (m_instsToRemove.empty() == false);

    // Remove unnecessary instructions.
    while (m_instsToRemove.empty() == false)
    {
        Instruction* const pInst = m_instsToRemove.pop_back_val();
        pInst->dropAllReferences();
        pInst->eraseFromParent();
    }

    for (Function* pFunc : spillTableFuncs)
    {
        if (pFunc->user_empty())
        {
            pFunc->eraseFromParent();
        }
    }

    return changed;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchPushConstOp::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    Function* const pCallee = callInst.getCalledFunction();
    assert(pCallee != nullptr);
    assert(pCallee->getName().startswith(lgcName::DescriptorLoadSpillTable));
    (void(pCallee)); // unused

    auto pIntfData = m_pPipelineState->GetShaderInterfaceData(m_shaderStage);
    uint32_t pushConstNodeIdx = pIntfData->pushConst.resNodeIdx;
    assert(pushConstNodeIdx != InvalidValue);
    auto pPushConstNode = &m_pPipelineState->GetUserDataNodes()[pushConstNodeIdx];

    if (pPushConstNode->offsetInDwords < pIntfData->spillTable.offsetInDwords)
    {
        auto pPushConst = GetFunctionArgument(m_pEntryPoint, pIntfData->entryArgIdxs.resNodeValues[pushConstNodeIdx]);

        IRBuilder<> builder(*m_pContext);
        builder.SetInsertPoint(callInst.getFunction()->getEntryBlock().getFirstNonPHI());

        Value* pPushConstPointer = builder.CreateAlloca(pPushConst->getType());
        builder.CreateStore(pPushConst, pPushConstPointer);

        Type* const pCastType = callInst.getType()->getPointerElementType()->getPointerTo(ADDR_SPACE_PRIVATE);

        pPushConstPointer = builder.CreateBitCast(pPushConstPointer, pCastType);

        ValueMap<Value*, Value*> valueMap;

        valueMap[&callInst] = pPushConstPointer;

        SmallVector<Value*, 8> workList;

        for (User* const pUser : callInst.users())
        {
            workList.push_back(pUser);
        }

        m_instsToRemove.push_back(&callInst);

        while (workList.empty() == false)
        {
            Instruction* const pInst = dyn_cast<Instruction>(workList.pop_back_val());

            // If the value is not an instruction, bail.
            if (pInst == nullptr)
            {
                continue;
            }

            m_instsToRemove.push_back(pInst);

            if (BitCastInst* const pBitCast = dyn_cast<BitCastInst>(pInst))
            {
                assert(valueMap.count(pBitCast->getOperand(0)) > 0);

                Type* const pCastType = pBitCast->getType();
                assert(pCastType->isPointerTy());
                assert(pCastType->getPointerAddressSpace() == ADDR_SPACE_CONST);

                Type* const pNewType = pCastType->getPointerElementType()->getPointerTo(ADDR_SPACE_PRIVATE);

                builder.SetInsertPoint(pBitCast);
                valueMap[pBitCast] = builder.CreateBitCast(valueMap[pBitCast->getOperand(0)], pNewType);

                for (User* const pUser : pBitCast->users())
                {
                    workList.push_back(pUser);
                }
            }
            else if (GetElementPtrInst* const pGetElemPtr = dyn_cast<GetElementPtrInst>(pInst))
            {
                assert(valueMap.count(pGetElemPtr->getPointerOperand()) > 0);

                SmallVector<Value*, 8> indices;

                for (Value* const pIndex : pGetElemPtr->indices())
                {
                    indices.push_back(pIndex);
                }

                builder.SetInsertPoint(pGetElemPtr);
                valueMap[pGetElemPtr] = builder.CreateInBoundsGEP(valueMap[pGetElemPtr->getPointerOperand()],
                                                                    indices);

                for (User* const pUser : pGetElemPtr->users())
                {
                    workList.push_back(pUser);
                }
            }
            else if (LoadInst* const pLoad = dyn_cast<LoadInst>(pInst))
            {
                assert(valueMap.count(pLoad->getPointerOperand()) > 0);

                builder.SetInsertPoint(pLoad);

                LoadInst* const pNewLoad = builder.CreateLoad(valueMap[pLoad->getPointerOperand()]);

                valueMap[pLoad] = pNewLoad;

                pLoad->replaceAllUsesWith(pNewLoad);
            }
            else
            {
                llvm_unreachable("Should never be called!");
            }
        }
    }
}

} // lgc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for push constant operations.
INITIALIZE_PASS_BEGIN(PatchPushConstOp, DEBUG_TYPE, "Patch LLVM for push constant operations", false, false)
INITIALIZE_PASS_DEPENDENCY(PipelineShaders)
INITIALIZE_PASS_END(PatchPushConstOp, DEBUG_TYPE, "Patch LLVM for push constant operations", false, false)
