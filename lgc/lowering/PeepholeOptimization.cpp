/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PeepholeOptimization.cpp
 * @brief LLPC source file: contains implementation of class lgc::PeepholeOptimization.
 ***********************************************************************************************************************
 */
#include "lgc/lowering/PeepholeOptimization.h"
#include "lgc/Builder.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-peephole-optimization"

using namespace lgc;
using namespace llvm;
using namespace llvm::PatternMatch;

namespace {

class PeepholeOptimizer : public llvm::InstVisitor<PeepholeOptimizer> {

public:
  PeepholeOptimizer(const ShaderOptions *shaderOptions) : m_changed(false), m_shaderOptions(shaderOptions) {}

  bool run(Function &function);

  void visitIntToPtr(IntToPtrInst &intToPtr);
  void visitCallInst(CallInst &callInst);

private:
  bool m_changed;
  const ShaderOptions *m_shaderOptions;
  llvm::SmallVector<llvm::Instruction *, 8> m_instsToErase;
};

} // anonymous namespace

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will peephole optimize.
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PeepholeOptimization::run(Function &function, FunctionAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Peephole-Optimization\n");

  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();
  auto shaderStage = getShaderStage(&function);
  const ShaderOptions *shaderOptions = nullptr;
  if (shaderStage)
    shaderOptions = &pipelineState->getShaderOptions(shaderStage.value());

  PeepholeOptimizer pho(shaderOptions);
  bool changed = pho.run(function);
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Apply peephole optimizations to the function
//
// @param [in/out] function : Function that we will peephole optimize.
// @returns : true if any change was made
bool PeepholeOptimizer::run(Function &function) {
  visit(function);

  const bool changed = m_changed || !m_instsToErase.empty();

  for (Instruction *const inst : m_instsToErase) {
    // Lastly delete any instructions we replaced.
    inst->eraseFromParent();
  }
  m_instsToErase.clear();

  return changed;
}

// =====================================================================================================================
// Visit an inttoptr instruction.
//
// Change inttoptr ( add x, const ) -> gep ( inttoptr x, const ) to improve value tracking and load/store vectorization.
//
// Note: we decided to implement this transformation here and not in LLVM. From the point of view of alias analysis, the
// pointer returned by inttoptr ( add x, const ) is different from the pointer returned by gep ( inttoptr x, const ):
// the former is associated with whatever x AND const point to; the latter is associated ONLY with whatever x points to.
//
// In LLPC/LGC, we can assume that const does not point to any object (which makes this transformation valid) but that's
// not an assumption that can be made in general in LLVM with all its different front-ends.
//
// Reference: https://groups.google.com/g/llvm-dev/c/x4K7ppGLbg8/m/f_3NySRhjlcJ

// @param intToPtr: The "inttoptr" instruction to visit.
void PeepholeOptimizer::visitIntToPtr(IntToPtrInst &intToPtr) {
  // Check if we are using add to do pointer arithmetic.
  auto *const binaryOperator = dyn_cast<BinaryOperator>(intToPtr.getOperand(0));
  if (!binaryOperator || binaryOperator->getOpcode() != Instruction::Add)
    return;

  // Check that we have a constant offset.
  const auto *const constOffset = dyn_cast<ConstantInt>(binaryOperator->getOperand(1));
  if (!constOffset)
    return;

  if (intToPtr.user_empty())
    return;

  auto *const user = cast<Instruction>(intToPtr.user_back());

  Type *elementType = nullptr;
  if (auto loadInst = dyn_cast<LoadInst>(user)) {
    elementType = loadInst->getType();

  } else if (auto getElemPtr = dyn_cast<GetElementPtrInst>(user)) {
    elementType = getElemPtr->getSourceElementType();

  } else {
    // Exit if user is not Load or GEP instruction
    // (right now only these two instructions are used).
    return;
  }

  // Create a getelementptr instruction (using offset / size).
  const DataLayout &dataLayout = intToPtr.getModule()->getDataLayout();
  const uint64_t size = dataLayout.getTypeAllocSize(elementType);
  if (size == 0)
    return;
  APInt index = constOffset->getValue().udiv(size);
  if (constOffset->getValue().urem(size) != 0)
    return;

  // Change inttoptr ( add x, const ) -> gep ( inttoptr x, const ).
  auto *const newIntToPtr = new IntToPtrInst(binaryOperator->getOperand(0), intToPtr.getType());
  newIntToPtr->insertAfter(binaryOperator);

  auto *const getElementPtr =
      GetElementPtrInst::Create(elementType, newIntToPtr, ConstantInt::get(newIntToPtr->getContext(), index));
  getElementPtr->insertAfter(newIntToPtr);

  // Set every instruction to use the newly calculated pointer.
  intToPtr.replaceAllUsesWith(getElementPtr);

  // If the add instruction has no other users then mark to erase.
  if (binaryOperator->getNumUses() == 0)
    m_instsToErase.push_back(binaryOperator);
}

// =====================================================================================================================
// Visit a call instruction.
//
// Peephole relevant argument to call such that const +/- x -> max(0.0, const +/- x)
// where the argument is X for log2(X) or pow(X, Y).
// This addresses a potential precision underflow in applications intolerant to
// in-spec math reordering.
// This has to be enabled per app or shader based on forceUnderflowPrevention option.
//
// @param callInst: The call instruction to visit.
void PeepholeOptimizer::visitCallInst(CallInst &callInst) {
  // Only apply this peephole when explicitly requested via option
  if (!(m_shaderOptions && m_shaderOptions->forceUnderflowPrevention))
    return;

  switch (callInst.getIntrinsicID()) {
  case Intrinsic::log2:
  case Intrinsic::pow:
    break;
  default:
    return;
  }

  Value *V = callInst.getOperand(0);

  if (!(match(V, m_FSub(m_Constant(), m_Value())) || match(V, m_FSub(m_Value(), m_Constant())) ||
        match(V, m_FAdd(m_Constant(), m_Value())) || match(V, m_FAdd(m_Value(), m_Constant()))))
    return;

  // Do not touch instructions marked as potentially having NaNs, as this would not be a legal transform.
  Instruction *srcInst = cast<Instruction>(V);
  if (!srcInst->hasNoNaNs())
    return;

  IRBuilder<> builder(callInst.getContext());
  builder.setFastMathFlags(srcInst->getFastMathFlags());
  builder.SetInsertPoint(&callInst);
  Value *newSrc = builder.CreateMaxNum(ConstantFP::getZero(V->getType()), V);
  callInst.setOperand(0, newSrc);

  m_changed = true;
}
