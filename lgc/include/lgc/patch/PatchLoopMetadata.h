/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchLoopMetadata.h
 * @brief LLPC header file: contains declaration of class lgc::PatchLoopMetadata.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"

namespace lgc {

// =====================================================================================================================
// Represents the LLVM pass for patching loop metadata.
class PatchLoopMetadata : public llvm::PassInfoMixin<PatchLoopMetadata> {
public:
  PatchLoopMetadata();
  llvm::PreservedAnalyses run(llvm::Loop &loop, llvm::LoopAnalysisManager &analysisManager,
                              llvm::LoopStandardAnalysisResults &loopAnalysisResults, llvm::LPMUpdater &);

  bool runImpl(llvm::Loop &loop, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Set or amend metadata to control loop unrolling"; }

  llvm::MDNode *updateMetadata(llvm::MDNode *loopId, llvm::ArrayRef<llvm::StringRef> prefixesToRemove,
                               llvm::Metadata *addMetadata, bool conditional);

private:
  llvm::LLVMContext *m_context;       // Associated LLVM context of the LLVM module that passes run on
  unsigned m_forceLoopUnrollCount;    // Force loop unroll count
  bool m_disableLoopUnroll;           // Forcibly disable loop unroll
  unsigned m_disableLicmThreshold;    // Disable LLVM LICM pass loop block count threshold
  unsigned m_unrollHintThreshold;     // Unroll hint threshold
  unsigned m_dontUnrollHintThreshold; // DontUnroll hint threshold
  GfxIpVersion m_gfxIp;
};

} // namespace lgc
