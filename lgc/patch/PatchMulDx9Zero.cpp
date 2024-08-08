/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchMulDx9Zero.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchMulDx9Zero.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchMulDx9Zero.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-patch-mul-dx9-zero"

using namespace lgc;
using namespace llvm;
using namespace PatternMatch;

namespace lgc {
// =====================================================================================================================
PatchMulDx9Zero::PatchMulDx9Zero() : m_changed(false) {
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : LLVM function to be run on, following patterns will be detected in the function
// ((b==0.0 ? 0.0 : a) * (a==0.0 ? 0.0 : b)) or
// ((b==0.0 ? 0.0 : a) * (a==0.0 ? 0.0 : b)) or
// fma((b==0.0 ? 0.0 : a), (a==0.0 ? 0.0 : b), c)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchMulDx9Zero::run(Function &function, FunctionAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Mul-Dx9Zero-Opt\n");

  m_builder = std::make_unique<IRBuilder<>>(function.getContext());

  visit(function);

  return m_changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Visits call instruction.
//
// @param callInst : Call instruction
void PatchMulDx9Zero::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  // Replace fma with amdgcn_fma_legacy intrinsic when detect patterns like:
  // fma((b==0.0 ? 0.0 : a), (a==0.0 ? 0.0 : b), c)
  if (callee->isIntrinsic() && callee->getIntrinsicID() == Intrinsic::fma) {
    Value *src1 = callInst.getArgOperand(0);
    Value *src2 = callInst.getArgOperand(1);
    auto matchValue = isMulDx9Zero(src1, src2);
    if (matchValue != std::nullopt) {
      m_builder->SetInsertPoint(&callInst);
      m_builder->setFastMathFlags(callInst.getFastMathFlags());
      Value *transformSrc1 = matchValue->first;
      Value *transformSrc2 = matchValue->second;
      Value *src3 = callInst.getArgOperand(2);
      Value *ffmazResult =
          m_builder->CreateIntrinsic(Intrinsic::amdgcn_fma_legacy, {}, {transformSrc1, transformSrc2, src3});
      m_changed = true;
      callInst.replaceAllUsesWith(ffmazResult);
      callInst.dropAllReferences();
      callInst.eraseFromParent();
    }
  }
}

// =====================================================================================================================
// Visits binary operator instruction.
//
// @param binaryOp : Binary operator instruction
void PatchMulDx9Zero::visitBinaryOperator(BinaryOperator &binaryOp) {
  Instruction::BinaryOps opCode = binaryOp.getOpcode();

  // Replace mul with amdgcn_fmul_legacy intrinsic when detect patterns like:
  // ((b==0.0 ? 0.0 : a) * (a==0.0 ? 0.0 : b))
  if (opCode == Instruction::FMul) {
    auto src1 = binaryOp.getOperand(0);
    auto src2 = binaryOp.getOperand(1);
    auto matchValue = isMulDx9Zero(src1, src2);
    if (matchValue != std::nullopt) {
      m_builder->SetInsertPoint(&binaryOp);
      m_builder->setFastMathFlags(binaryOp.getFastMathFlags());
      Value *transformSrc1 = matchValue->first;
      Value *transformSrc2 = matchValue->second;
      Value *fmulzResult =
          m_builder->CreateIntrinsic(Intrinsic::amdgcn_fmul_legacy, {}, {transformSrc1, transformSrc2});
      m_changed = true;
      binaryOp.replaceAllUsesWith(fmulzResult);
      binaryOp.dropAllReferences();
      binaryOp.eraseFromParent();
    }
  }
}

// =====================================================================================================================
// Checks whether a multiply of lhs with rhs using the given fast-math flags can be transformed into a multiply
// with DX9 zero semantics. If so, returns a pair of operands for the new multiply.
// @param lhs : left operand for the operation
// @param rhs:  right operand for the operation
std::optional<std::pair<Value *, Value *>> PatchMulDx9Zero::isMulDx9Zero(Value *lhs, Value *rhs) {
  Value *lhsCmpValue = nullptr;
  Value *lhsFalseValue = nullptr;
  Value *rhsCmpValue = nullptr;
  Value *rhsFalseValue = nullptr;
  FCmpInst::Predicate pred = FCmpInst::FCMP_OEQ;

  // Only transform for float32.
  if (!(lhs->getType()->isFloatTy() && rhs->getType()->isFloatTy()))
    return std::nullopt;

  // Detect whether A = (b==0.0 ? 0.0 : a) and parse out b and a
  bool lhsMatch =
      match(lhs, m_Select(m_FCmp(pred, m_Value(lhsCmpValue), m_AnyZeroFP()), m_Zero(), m_Value(lhsFalseValue)));
  // Detect whether B = (a'==0.0 ? 0.0 : b') and output a' and b'
  bool rhsMatch =
      match(rhs, m_Select(m_FCmp(pred, m_Value(rhsCmpValue), m_AnyZeroFP()), m_Zero(), m_Value(rhsFalseValue)));

  // If b == b' && a == a' then use fmul_legacy(a,b) instead of fmul(A,B)
  if (lhsMatch && rhsMatch && (lhsCmpValue == rhsFalseValue) && (rhsCmpValue == lhsFalseValue)) {
    return std::make_pair(lhsFalseValue, rhsFalseValue);
  }
  if (lhsMatch && (lhsCmpValue == rhs)) {
    if (auto *constLhsFalseValue = dyn_cast<ConstantFP>(lhsFalseValue);
        constLhsFalseValue && !constLhsFalseValue->isZero()) {
      // Detect pattern: ((b==0.0 ? 0.0 : a) * b) when a is constant but not zero.
      return std::make_pair(lhsFalseValue, rhs);
    }
  }
  if (rhsMatch && (lhs == rhsCmpValue)) {
    if (auto *constRhsFalseValue = dyn_cast<ConstantFP>(rhsFalseValue);
        constRhsFalseValue && !constRhsFalseValue->isZero()) {
      // Detect pattern: (a * (a==0.0 ? 0.0 : b)) when b is constant but not zero.
      return std::make_pair(lhs, rhsFalseValue);
    }
  }
  return std::nullopt;
}
} // namespace lgc
