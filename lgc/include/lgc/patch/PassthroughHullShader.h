/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PassthroughHullShader.h
 * @brief LLPC header file: contains declaration of class lgc::PassthroughHullShader.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/LgcLowering.h"
#include "lgc/state/PipelineShaders.h"
#include "llvm/IR/PassManager.h"

namespace lgc {

// =====================================================================================================================
// Pass to generate tessellation control pass-through shader
class PassthroughHullShader : public llvm::PassInfoMixin<PassthroughHullShader> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Pass-through hull shader generation"; }
  void updatePipelineState(llvm::Module &module, PipelineState *pipelineState) const;
  llvm::Function *generatePassthroughHullShader(llvm::Module &module, PipelineShadersResult &pipelineShaders,
                                                PipelineState *pipelineState);
  llvm::Function *generateTcsPassthroughEntryPoint(llvm::Module &module, PipelineState *pipelineState);
  void generatePassthroughHullShaderBody(llvm::Module &module, PipelineShadersResult &pipelineShaders,
                                         PipelineState *pipelineState, llvm::Function *entryPoint);
};

} // namespace lgc
