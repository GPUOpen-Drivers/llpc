/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderReplayer.h
 * @brief LLPC header file: contains declaration of class lgc::BuilderReplayer
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "llvm/IR/PassManager.h"
#include <map>

namespace lgc {

class BuilderImpl;
class PipelineState;

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class BuilderReplayer final : public llvm::PassInfoMixin<BuilderReplayer> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Replay LLPC builder calls"; }

private:
  void replayCall(unsigned opcode, llvm::CallInst *call);

  llvm::Value *processCall(unsigned opcode, llvm::CallInst *call);

  BuilderImpl *m_builder = nullptr;                             // The LLPC builder that the builder
                                                                //  calls are being replayed on.
  std::map<llvm::Function *, ShaderStageEnum> m_shaderStageMap; // Map function -> shader stage
  llvm::Function *m_enclosingFunc = nullptr;                    // Last function written with current
                                                                //  shader stage
};

} // namespace lgc
