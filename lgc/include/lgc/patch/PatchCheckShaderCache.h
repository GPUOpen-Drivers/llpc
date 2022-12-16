/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchCheckShaderCache.h
 * @brief LLPC header file: contains declaration of class lgc::PatchCheckShaderCache
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of LLVM patching operations for checking shader cache
class PatchCheckShaderCache : public Patch, public llvm::PassInfoMixin<PatchCheckShaderCache> {
public:
  PatchCheckShaderCache() {}

  PatchCheckShaderCache(Pipeline::CheckShaderCacheFunc callbackFunc);

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch LLVM for checking shader cache"; }

  // Set the callback function that this pass uses to ask the front-end whether it wants to remove
  // any shader stages. The function takes the LLVM IR module and a per-shader-stage array of input/output
  // usage checksums, and it returns the shader stage mask with bits removed for shader stages that it wants
  // removed.
  void setCallbackFunction(Pipeline::CheckShaderCacheFunc callbackFunc) { m_callbackFunc = callbackFunc; }

private:
  Pipeline::CheckShaderCacheFunc m_callbackFunc;
};

} // namespace lgc
