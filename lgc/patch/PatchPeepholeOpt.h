/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchPeepholeOpt.h
 * @brief LLPC header file: contains declaration of class lgc::PatchPeepholeOpt.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/util/Internal.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Pass.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of LLVM patching operations for peephole optimizations, with the following patterns covered:
//
// - Combine multiple identical bit casts (LLVM treats these as no-ops, but since we use the scalarizer they can become
//   costly when transforming <4 x i8> <-> i32/float).
// - Change bitcast( shufflevector x, y ) -> shufflevector( bitcast x ), ( bitcast y ).
// - Change bitcast( phi [x, foo], [y, bar] ) -> phi [( bitcast x ), foo], [( bitcast y ), bar].
// - Change integer comparisons for unsigned greater than such that x > y, y is a constant -> x < y + 1.
// - Remove extract elements when they are extracting from an insert (forward the extract to the inserted element).
// - Combine multiple extract elements that extract the same element.
// - Aggressively scalarize PHI nodes that use 32-bit or wider types.
// - Remove PHI nodes whose incoming values can be proven to be identical in the parent blocks (most commonly seen with
//   multiple extract elements that are identical).
// - Optimize PHI nodes that are confusingly non-PHIs by deducing these complicated cases and removing the PHIs.
// - Change inttoptr ( add x, const ) -> gep ( inttoptr x, const ) to improve value tracking and load/store
//   vectorization.
//
class PatchPeepholeOpt final : public llvm::FunctionPass, public llvm::InstVisitor<PatchPeepholeOpt> {
public:
  PatchPeepholeOpt();

  bool runOnFunction(llvm::Function &function) override;

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override;

  void visitBitCast(llvm::BitCastInst &bitCast);
  void visitICmp(llvm::ICmpInst &iCmp);
  void visitExtractElement(llvm::ExtractElementInst &extractElement);
  void visitPHINode(llvm::PHINode &phiNode);
  void visitIntToPtr(llvm::IntToPtrInst &intToPtr);

  void moveAfter(llvm::Instruction &move, llvm::Instruction &after) const;
  void insertAfter(llvm::Instruction &insert, llvm::Instruction &after) const;

  static char ID; // ID of this pass

private:
  PatchPeepholeOpt(const PatchPeepholeOpt &) = delete;
  PatchPeepholeOpt &operator=(const PatchPeepholeOpt &) = delete;

  llvm::SmallVector<llvm::Instruction *, 8> m_instsToErase;
};

} // namespace lgc
