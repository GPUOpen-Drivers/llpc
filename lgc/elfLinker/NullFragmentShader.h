/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC All Rights Reserved.
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
 * @file  NullFragmentShader.h
 * @brief LGC header file: The class to generate the null fragment shader when linking.
 ***********************************************************************************************************************
 */
#pragma once

#include "GlueShader.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"

namespace lgc {

// =====================================================================================================================
// The class to generate the null fragment shader when linking.
class NullFragmentShader : public GlueShader {
public:
  NullFragmentShader(PipelineState *pipelineState) : GlueShader(pipelineState) {}

  // Get the string for this glue shader. This is some encoding or hash of the inputs to the create*Shader function
  // that the front-end client can use as a cache key to avoid compiling the same glue shader more than once.
  llvm::StringRef getString() override { return "null"; }

  // Get the symbol name of the main shader that this glue shader is prolog or epilog for.
  llvm::StringRef getMainShaderName() override {
    return getEntryPointName(llvm::CallingConv::AMDGPU_PS, /*isFetchlessVs=*/false);
  }

  // Get the symbol name of the glue shader.
  llvm::StringRef getGlueShaderName() override {
    return getEntryPointName(llvm::CallingConv::AMDGPU_PS, /*isFetchlessVs=*/false);
  }

  // Get whether this glue shader is a prolog (rather than epilog) for its main shader.
  bool isProlog() override { return false; }

  // Get the name of this glue shader.
  llvm::StringRef getName() const override { return "null fragment shader"; }

  // Update the entries in the PAL metadata that require both the pipeline state and export info.  So far, no entries
  // seem to be needed for the null fragment shader.
  void updatePalMetadata(PalMetadata &palMetadata) override {}

protected:
  llvm::Module *generate() override;
  llvm::Module *generateEmptyModule() const;
  void addDummyExportIfNecessary(llvm::Function *entryPoint) const;
};

} // namespace lgc
