/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ApplyWorkarounds.h
 * @brief LLPC header file: contains declaration of class lgc::ApplyWorkarounds.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/lowering/LgcLowering.h"
#include "lgc/util/Internal.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of LGC lowering operations for applying workarounds:
//
// - fix up issues when buffer descriptor is incorrectly given when it should be an image descriptor. Some architectures
//   require a fix so the hardware will ignore this difference (actually an app error, but common enough to require
//   handling)
//
class ApplyWorkarounds final : public LgcLowering, public llvm::PassInfoMixin<ApplyWorkarounds> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Apply workarounds"; }

private:
  std::unique_ptr<llvm::IRBuilder<>> m_builder; // The IRBuilder.
  PipelineState *m_pipelineState;               // The pipeline state

  llvm::SmallPtrSet<llvm::Value *, 8> m_processed; // Track rsrc-desc args already processed

  bool m_changed;

  void applyImageDescWorkaround(void);
  void processImageDescWorkaround(llvm::CallInst &callInst, bool isLastUse);
};

} // namespace lgc
