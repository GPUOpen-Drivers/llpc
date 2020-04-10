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
#include "llpcPatchPushConstOp.h"
#include "llpcIntrinsDefs.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"
#include "lgc/llpcBuilder.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-patch-push-const"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchPushConstOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for push constant operations
ModulePass *createPatchPushConstOp() {
  return new PatchPushConstOp();
}

// =====================================================================================================================
PatchPushConstOp::PatchPushConstOp() : Patch(ID) {
}

// =====================================================================================================================
// Get the analysis usage of this pass.
//
// @param [out] analysisUsage : The analysis usage.
void PatchPushConstOp::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.addRequired<PipelineStateWrapper>();
  analysisUsage.addRequired<PipelineShaders>();
  analysisUsage.addPreserved<PipelineShaders>();
  analysisUsage.setPreservesCFG();
}

// =====================================================================================================================
// Executes this SPIR-V patching pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchPushConstOp::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Push-Const-Op\n");

  Patch::init(&module);

  SmallVector<Function *, 4> spillTableFuncs;
  for (auto &func : module) {
    if (func.getName().startswith(lgcName::DescriptorLoadSpillTable))
      spillTableFuncs.push_back(&func);
  }

  // If there was no spill table load, bail.
  if (spillTableFuncs.empty())
    return false;

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  const PipelineShaders &pipelineShaders = getAnalysis<PipelineShaders>();
  for (unsigned shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage) {
    m_entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(shaderStage));

    // If we don't have an entry point for the shader stage, bail.
    if (!m_entryPoint)
      continue;

    m_shaderStage = static_cast<ShaderStage>(shaderStage);

    for (Function *func : spillTableFuncs) {
      for (User *const user : func->users()) {
        CallInst *const call = dyn_cast<CallInst>(user);

        // If the user is not a call, bail.
        if (!call)
          continue;

        // If the call is not in the entry point, bail.
        if (call->getFunction() != m_entryPoint)
          continue;

        visitCallInst(*call);
      }
    }
  }

  const bool changed = (!m_instsToRemove.empty());

  // Remove unnecessary instructions.
  while (!m_instsToRemove.empty()) {
    Instruction *const inst = m_instsToRemove.pop_back_val();
    inst->dropAllReferences();
    inst->eraseFromParent();
  }

  for (Function *func : spillTableFuncs) {
    if (func->user_empty())
      func->eraseFromParent();
  }

  return changed;
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void PatchPushConstOp::visitCallInst(CallInst &callInst) {
  Function *const callee = callInst.getCalledFunction();
  assert(callee);
  assert(callee->getName().startswith(lgcName::DescriptorLoadSpillTable));
  (void(callee)); // unused

  auto intfData = m_pipelineState->getShaderInterfaceData(m_shaderStage);
  unsigned pushConstNodeIdx = intfData->pushConst.resNodeIdx;
  assert(pushConstNodeIdx != InvalidValue);
  auto pushConstNode = &m_pipelineState->getUserDataNodes()[pushConstNodeIdx];

  if (pushConstNode->offsetInDwords < intfData->spillTable.offsetInDwords) {
    auto pushConst = getFunctionArgument(m_entryPoint, intfData->entryArgIdxs.resNodeValues[pushConstNodeIdx]);

    IRBuilder<> builder(*m_context);
    builder.SetInsertPoint(callInst.getFunction()->getEntryBlock().getFirstNonPHI());

    Value *pushConstPointer = builder.CreateAlloca(pushConst->getType());
    builder.CreateStore(pushConst, pushConstPointer);

    Type *const castType = callInst.getType()->getPointerElementType()->getPointerTo(ADDR_SPACE_PRIVATE);

    pushConstPointer = builder.CreateBitCast(pushConstPointer, castType);

    ValueMap<Value *, Value *> valueMap;

    valueMap[&callInst] = pushConstPointer;

    SmallVector<Value *, 8> workList;

    for (User *const user : callInst.users())
      workList.push_back(user);

    m_instsToRemove.push_back(&callInst);

    while (!workList.empty()) {
      Instruction *const inst = dyn_cast<Instruction>(workList.pop_back_val());

      // If the value is not an instruction, bail.
      if (!inst)
        continue;

      m_instsToRemove.push_back(inst);

      if (BitCastInst *const bitCast = dyn_cast<BitCastInst>(inst)) {
        assert(valueMap.count(bitCast->getOperand(0)) > 0);

        Type *const castType = bitCast->getType();
        assert(castType->isPointerTy());
        assert(castType->getPointerAddressSpace() == ADDR_SPACE_CONST);

        Type *const newType = castType->getPointerElementType()->getPointerTo(ADDR_SPACE_PRIVATE);

        builder.SetInsertPoint(bitCast);
        valueMap[bitCast] = builder.CreateBitCast(valueMap[bitCast->getOperand(0)], newType);

        for (User *const user : bitCast->users())
          workList.push_back(user);
      } else if (GetElementPtrInst *const getElemPtr = dyn_cast<GetElementPtrInst>(inst)) {
        assert(valueMap.count(getElemPtr->getPointerOperand()) > 0);

        SmallVector<Value *, 8> indices;

        for (Value *const index : getElemPtr->indices())
          indices.push_back(index);

        builder.SetInsertPoint(getElemPtr);
        valueMap[getElemPtr] = builder.CreateInBoundsGEP(valueMap[getElemPtr->getPointerOperand()], indices);

        for (User *const user : getElemPtr->users())
          workList.push_back(user);
      } else if (LoadInst *const load = dyn_cast<LoadInst>(inst)) {
        assert(valueMap.count(load->getPointerOperand()) > 0);

        builder.SetInsertPoint(load);

        LoadInst *const newLoad = builder.CreateLoad(valueMap[load->getPointerOperand()]);

        valueMap[load] = newLoad;

        load->replaceAllUsesWith(newLoad);
      } else
        llvm_unreachable("Should never be called!");
    }
  }
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for push constant operations.
INITIALIZE_PASS_BEGIN(PatchPushConstOp, DEBUG_TYPE, "Patch LLVM for push constant operations", false, false)
INITIALIZE_PASS_DEPENDENCY(PipelineShaders)
INITIALIZE_PASS_END(PatchPushConstOp, DEBUG_TYPE, "Patch LLVM for push constant operations", false, false)
