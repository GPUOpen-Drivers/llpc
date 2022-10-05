/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchInvariantLoads.h
 * @brief LLPC header file: contains declaration of class lgc::PatchInvariantLoads.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/state/PipelineState.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of LLVM patching operations for image operations
class PatchInvariantLoads : public llvm::PassInfoMixin<PatchInvariantLoads> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  bool runImpl(llvm::Function &function, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch metadata for invariant loads"; }
};

// =====================================================================================================================
// Represents the pass of LLVM patching operations for image operations
class LegacyPatchInvariantLoads : public llvm::FunctionPass {
public:
  LegacyPatchInvariantLoads();

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override;
  bool runOnFunction(llvm::Function &function) override;

  static char ID; // ID of this pass

private:
  LegacyPatchInvariantLoads(const LegacyPatchInvariantLoads &) = delete;
  LegacyPatchInvariantLoads &operator=(const LegacyPatchInvariantLoads &) = delete;

  PatchInvariantLoads m_impl;
};

} // namespace lgc
