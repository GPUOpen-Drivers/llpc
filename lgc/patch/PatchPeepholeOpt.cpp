/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchPeepholeOpt.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchPeepholeOpt.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchPeepholeOpt.h"
#include "lgc/patch/Patch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-patch-peephole-opt"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Define static members (no initializer needed as LLVM only cares about the address of ID, never its value).
char LegacyPatchPeepholeOpt::ID;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for peephole optimizations.
FunctionPass *createLegacyPatchPeepholeOpt() {
  return new LegacyPatchPeepholeOpt();
}

LegacyPatchPeepholeOpt::LegacyPatchPeepholeOpt() : FunctionPass(ID) {
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will peephole optimize.
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchPeepholeOpt::runOnFunction(Function &function) {
  return m_impl.runImpl(function);
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will peephole optimize.
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchPeepholeOpt::run(Function &function, FunctionAnalysisManager &analysisManager) {
  if (runImpl(function))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will peephole optimize.
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchPeepholeOpt::runImpl(Function &function) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Peephole-Opt\n");

  visit(function);

  const bool changed = !m_instsToErase.empty();

  for (Instruction *const inst : m_instsToErase) {
    // Lastly delete any instructions we replaced.
    inst->eraseFromParent();
  }
  m_instsToErase.clear();

  return changed;
}

// =====================================================================================================================
// Specify what analysis passes this pass depends on.
//
// @param [in/out] analysisUsage : The place to record our analysis pass usage requirements.
void LegacyPatchPeepholeOpt::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.setPreservesCFG();
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
void PatchPeepholeOpt::visitIntToPtr(IntToPtrInst &intToPtr) {
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
  // TODO: Remove this when LLPC will switch fully to opaque pointers.
  assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(intToPtr.getType(), elementType));
  const uint64_t size = dataLayout.getTypeAllocSize(elementType);
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

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for peephole optimizations.
INITIALIZE_PASS(LegacyPatchPeepholeOpt, DEBUG_TYPE, "Patch LLVM for peephole optimizations", false, false)
