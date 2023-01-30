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
 * @file  GlueShader.h
 * @brief LGC header file: The abstract class for glue shaders (fetch shader, parameter/color export shader) generated
 *        while linking.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/LgcContext.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"

namespace lgc {

class LgcContext;

// =====================================================================================================================
// Base class for a glue shader (a fetch shader or parameter/color export shader generated during linking)
class GlueShader {
public:
  virtual ~GlueShader() {}

  // Create a fetch shader
  static std::unique_ptr<GlueShader> createFetchShader(PipelineState *pipelineState,
                                                       llvm::ArrayRef<VertexFetchInfo> fetches,
                                                       const VsEntryRegInfo &vsEntryRegInfo);
  // Create a color export shader
  static std::unique_ptr<GlueShader> createColorExportShader(PipelineState *pipelineState,
                                                             llvm::ArrayRef<ColorExportInfo> exports);

  // Create a null fragment shader
  static std::unique_ptr<GlueShader> createNullFragmentShader(PipelineState *pipelineState);

  // Get the string for this glue shader. This is some encoding or hash of the inputs to the create*Shader function
  // that the front-end client can use as a cache key to avoid compiling the same glue shader more than once.
  virtual llvm::StringRef getString() = 0;

  // Get the ELF blob for this glue shader, compiling if not already compiled.
  llvm::StringRef getElfBlob() {
    if (m_elfBlob.empty()) {
      llvm::raw_svector_ostream outStream(m_elfBlob);
      compile(outStream);
    }
    return m_elfBlob;
  }

  // Set the ELF for this glue shader so that it does not have to be compiled.
  void setElfBlob(llvm::StringRef elfBlob) { m_elfBlob = elfBlob; }

  // Get the symbol name of the main shader that this glue shader is prolog or epilog for
  virtual llvm::StringRef getMainShaderName() = 0;

  // Get the symbol name of the glue shader.
  virtual llvm::StringRef getGlueShaderName() = 0;

  // Get whether this glue shader is a prolog (rather than epilog) for its main shader.
  virtual bool isProlog() { return false; }

  // Get the name of this glue shader.
  virtual llvm::StringRef getName() const = 0;

  // Update the PAL metadata entries that require the glue code data and the
  // pipeline state.
  virtual void updatePalMetadata(PalMetadata &palMetadata) = 0;

protected:
  GlueShader(LgcContext *lgcContext) : m_lgcContext(lgcContext) {}

  // Compile the glue shader
  void compile(llvm::raw_pwrite_stream &outStream);

  // Generate the IR module for the glue shader
  virtual llvm::Module *generate() = 0;

  llvm::LLVMContext &getContext() const { return m_lgcContext->getContext(); }

  LgcContext *m_lgcContext;

private:
  llvm::SmallString<0> m_elfBlob;
};

} // namespace lgc
