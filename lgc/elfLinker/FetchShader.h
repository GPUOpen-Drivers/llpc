/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  FetchShader.h
 * @brief LGC header file: The class to generate the fetch shader used when linking a pipeline.
 ***********************************************************************************************************************
 */
#pragma once

#include "GlueShader.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"

namespace lgc {

class LgcContext;

// =====================================================================================================================
// A fetch shader
class FetchShader : public GlueShader {
public:
  FetchShader(PipelineState *pipelineState, llvm::ArrayRef<VertexFetchInfo> fetches,
              const VsEntryRegInfo &vsEntryRegInfo);

  // Get the string for this glue shader. This is some encoding or hash of the inputs to the create*Shader function
  // that the front-end client can use as a cache key to avoid compiling the same glue shader more than once.
  llvm::StringRef getString() override;

  // Get the symbol name of the main shader that this glue shader is prolog or epilog for.
  llvm::StringRef getMainShaderName() override;

  // Get the symbol name of the glue shader.
  llvm::StringRef getGlueShaderName() override {
    return getEntryPointName(m_vsEntryRegInfo.callingConv, /*isFetchlessVs=*/false);
  }

  // Get whether this glue shader is a prolog (rather than epilog) for its main shader.
  bool isProlog() override { return true; }

  // Get the name of this glue shader.
  llvm::StringRef getName() const override { return "fetch shader"; }

  // No PAL metadata entries need updating for the fetch shader.
  void updatePalMetadata(PalMetadata &) override { return; }

protected:
  // Generate the glue shader to IR module
  llvm::Module *generate() override;

private:
  llvm::Function *createFetchFunc();
  void generateFetchShaderBody(llvm::Function *fetchFunc);

  void replaceShaderInputBuiltInFunctions(llvm::Function *fetchFunc) const;
  llvm::Value *getReplacementForVertexBufferTableBuiltIn(llvm::CallInst *call) const;
  llvm::Value *getReplacementForInputBuiltIn(llvm::CallInst *call) const;
  llvm::Value *getReplacementForVertexIdBuiltIn(llvm::CallInst *call) const;
  llvm::Value *getReplacementForInstanceIdBuiltIn(llvm::CallInst *call) const;
  llvm::Value *getVgprArgumentAsAnInt32(unsigned vgpr, llvm::Function *function) const;
  llvm::Value *getVpgrArgument(unsigned vgpr, BuilderBase &builder) const;
  bool mustFixLsVgprInput() const;

  // The information stored here is all that is needed to generate the fetch shader. We deliberately do not
  // have access to PipelineState, so we can hash the information here and let the front-end use it as the
  // key for a cache of glue shaders.
  llvm::SmallVector<VertexFetchInfo, 8> m_fetches;
  VsEntryRegInfo m_vsEntryRegInfo;
  llvm::SmallVector<const VertexInputDescription *, 8> m_fetchDescriptions;
  // The encoded or hashed (in some way) single string version of the above.
  std::string m_shaderString;

  // True if the fetch shader must work around the hardware sometimes shifting the vgpr inputs by two.
  bool m_fixLsVgprInput = false;
};

} // namespace lgc
