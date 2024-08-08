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
 * @file  PatchMulDx9Zero.h
 * @brief LLPC header file: contains declaration of class lgc::PatchMulDx9Zero.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Builder.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "llvm/IR/InstVisitor.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of LLVM patching operations when detect muldx9zero pattern like:
// ((b==0.0 ? 0.0 : a) * (a==0.0 ? 0.0 : b)) or
// ((b==0.0 ? 0.0 : a) * (a==0.0 ? 0.0 : b)) or
// fma((b==0.0 ? 0.0 : a), (a==0.0 ? 0.0 : b), c)
class PatchMulDx9Zero final : public llvm::InstVisitor<PatchMulDx9Zero>, public llvm::PassInfoMixin<PatchMulDx9Zero> {
public:
  explicit PatchMulDx9Zero();

  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  static llvm::StringRef name() {
    return "Run the pass to lower fmul or fma following Dx9 rules where 0 times anything produces 0.0\n";
  }

  virtual void visitCallInst(llvm::CallInst &callInst);

  virtual void visitBinaryOperator(llvm::BinaryOperator &binaryOp);

  std::optional<std::pair<llvm::Value *, llvm::Value *>> isMulDx9Zero(llvm::Value *lhs, llvm::Value *rhs);

private:
  bool m_changed;

  std::unique_ptr<llvm::IRBuilder<>> m_builder;
};

} // namespace lgc
