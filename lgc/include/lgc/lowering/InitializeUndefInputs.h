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
 * @file  InitializeUndefInputs.h
 * @brief LLPC header file: contains declaration of class lgc::InitializeUndefInputs.
 ***********************************************************************************************************************
 */

#pragma once
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include <set>
namespace lgc {

// =====================================================================================================================
// Represents the pass of setting the default value to uninitialized variables.
class InitializeUndefInputs final : public LgcLowering, public llvm::PassInfoMixin<InitializeUndefInputs> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Initialize undefined variables"; }

private:
  bool getUndefinedInputs(llvm::Module *module);
  void setUndefinedInputsToZero(llvm::Module *module);

  union LocCompInfo {
    struct {
      unsigned location : 12;
      unsigned component : 4;
      unsigned reserved : 16;
    };
    unsigned u32All;
  };

  PipelineState *m_pipelineState = nullptr;
  PipelineShadersResult *m_pipelineShaders = nullptr;
  ShaderStageMap<std::set<unsigned>> m_undefInputs;
};
} // namespace lgc
