/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchLoadScalarizer.h
 * @brief LLPC header file: contains declaration of class lgc::PatchLoadScalarizer.
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
// Represents the pass of LLVM patching operations for scalarize load.
class PatchLoadScalarizer final : public llvm::InstVisitor<PatchLoadScalarizer>,
                                  public llvm::PassInfoMixin<PatchLoadScalarizer> {
public:
  explicit PatchLoadScalarizer();

  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  bool runImpl(llvm::Function &function, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch LLVM for load scalarizer optimization"; }

  void visitLoadInst(llvm::LoadInst &loadInst);

private:
  llvm::SmallVector<llvm::Instruction *, 8> m_instsToErase; // Instructions to erase
  std::unique_ptr<llvm::IRBuilder<>> m_builder;             // The IRBuilder.
  unsigned m_scalarThreshold;                               // The threshold for load scalarizer
};

} // namespace lgc
