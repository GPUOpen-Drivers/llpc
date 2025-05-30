/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerTranslator.h
 * @brief LLPC header file: contains declaration of Llpc::LowerTranslator
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {

// =====================================================================================================================
// Pass to translate the SPIR-V modules and generate an IR module for the whole pipeline
class LowerTranslator : public Lowering, public llvm::PassInfoMixin<LowerTranslator> {
public:
  LowerTranslator() {}

  //
  // @param stage : Shader stage
  // @param shaderInfo : Shader info for this shader
  LowerTranslator(ShaderStage stage, const PipelineShaderInfo *shaderInfo, llvm::StringRef globalVarPrefix = {})
      : m_shaderInfo(shaderInfo), m_globalVarPrefix(globalVarPrefix) {}

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "LLPC translate SPIR-V to LLVM IR"; }

private:
  void translateSpirvToLlvm(const PipelineShaderInfo *shaderInfo, llvm::Module *module);

  // -----------------------------------------------------------------------------------------------------------------

  const PipelineShaderInfo *m_shaderInfo; // Input shader info
  llvm::StringRef m_globalVarPrefix;
};

} // namespace Llpc
