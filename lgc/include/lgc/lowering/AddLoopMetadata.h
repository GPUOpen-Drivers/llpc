/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  AddLoopMetadata.h
 * @brief LLPC header file: contains declaration of class lgc::AddLoopMetadata.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"

namespace lgc {

// =====================================================================================================================
// Represents the LLVM pass for lowering loop metadata.
class AddLoopMetadata : public llvm::PassInfoMixin<AddLoopMetadata> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Set or amend metadata to control loop unrolling"; }

  llvm::MDNode *updateMetadata(llvm::MDNode *loopId, llvm::ArrayRef<llvm::StringRef> prefixesToRemove,
                               llvm::Metadata *addMetadata, bool conditional);

private:
  llvm::LLVMContext *m_context; // Associated LLVM context of the LLVM module that passes run on
};

} // namespace lgc
