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
// - Change inttoptr ( add x, const ) -> gep ( inttoptr x, const ) to improve value tracking and load/store
//   vectorization.
//
class PatchPeepholeOpt final : public llvm::FunctionPass, public llvm::InstVisitor<PatchPeepholeOpt> {
public:
  PatchPeepholeOpt();

  bool runOnFunction(llvm::Function &function) override;

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override;

  void visitIntToPtr(llvm::IntToPtrInst &intToPtr);

  static char ID; // ID of this pass

private:
  PatchPeepholeOpt(const PatchPeepholeOpt &) = delete;
  PatchPeepholeOpt &operator=(const PatchPeepholeOpt &) = delete;

  llvm::SmallVector<llvm::Instruction *, 8> m_instsToErase;
};

} // namespace lgc
