/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerTranslator.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvLowerTranslator
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {

// =====================================================================================================================
// Pass to translate the SPIR-V modules and generate an IR module for the whole pipeline
class SpirvLowerTranslator : public SpirvLower, public llvm::PassInfoMixin<SpirvLowerTranslator> {
public:
  SpirvLowerTranslator() {}

  //
  // @param stage : Shader stage
  // @param shaderInfo : Shader info for this shader
  SpirvLowerTranslator(ShaderStage stage, const PipelineShaderInfo *shaderInfo) : m_shaderInfo(shaderInfo) {}

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  bool runImpl(llvm::Module &module);

  static llvm::StringRef name() { return "LLPC translate SPIR-V binary to LLVM IR"; }

private:
  void translateSpirvToLlvm(const PipelineShaderInfo *shaderInfo, llvm::Module *module);

  // -----------------------------------------------------------------------------------------------------------------

  const PipelineShaderInfo *m_shaderInfo; // Input shader info
};

} // namespace Llpc
