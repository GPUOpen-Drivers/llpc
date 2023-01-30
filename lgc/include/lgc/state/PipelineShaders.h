/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PipelineShaders.h
 * @brief LLPC header file: contains declaration of class Llpc::PipelineShaders
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include <map>

namespace lgc {

class PipelineShadersResult {
  friend class PipelineShaders;

public:
  PipelineShadersResult();
  llvm::Function *getEntryPoint(ShaderStage shaderStage) const;
  ShaderStage getShaderStage(const llvm::Function *func) const;

private:
  llvm::Function *m_entryPoints[ShaderStageCountInternal];       // The entry-point for each shader stage.
  std::map<const llvm::Function *, ShaderStage> m_entryPointMap; // Map from shader entry-point to shader stage.
};

// =====================================================================================================================
// Simple analysis pass that finds the shaders in the pipeline module
class PipelineShaders : public llvm::AnalysisInfoMixin<PipelineShaders> {
public:
  using Result = PipelineShadersResult;
  PipelineShadersResult run(llvm::Module &module, llvm::ModuleAnalysisManager &);
  PipelineShadersResult runImpl(llvm::Module &module);
  static llvm::AnalysisKey Key;
};

} // namespace lgc
