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
 * @file  GlueShader.cpp
 * @brief LGC source file: The abstract class for glue shaders (fetch shader, parameter/color export shader) generated
 *        while linking.
 ***********************************************************************************************************************
 */
#include "GlueShader.h"
#include "ColorExportShader.h"
#include "FetchShader.h"
#include "NullFragmentShader.h"
#include "lgc/state/PassManagerCache.h"
#include "llvm-dialects/Dialect/Dialect.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Compile the glue shader
//
// @param [in/out] outStream : Stream to write ELF to
void GlueShader::compile(raw_pwrite_stream &outStream) {
  // Generate the glue shader IR module.
  std::unique_ptr<Module> module(generate());

  // For explicit color export shader, some registers are required.
  if (m_pipelineState->getOptions().enableColorExportShader) {
    m_pipelineState->getPalMetadata()->updateDbShaderControl();
  }

  // Record pipeline state
  m_pipelineState->record(&*module);

  // For explicit color export shader, meta data have added some registers.
  if (!m_pipelineState->getOptions().enableColorExportShader) {
    // Add empty PAL metadata, to ensure that the back-end writes its PAL metadata in MsgPack format.
    PalMetadata *palMetadata = new PalMetadata(nullptr, m_pipelineState->useRegisterFieldFormat());
    palMetadata->record(&*module);
    delete palMetadata;
  }

  // Get the pass managers and run them on the module, generating ELF.
  std::pair<lgc::PassManager &, LegacyPassManager &> passManagers =
      m_lgcContext->getPassManagerCache()->getGlueShaderPassManager(outStream);
  // Run IR passes
  passManagers.first.run(*module);
  // Run Codegen Passes
  passManagers.second.run(*module);

  m_lgcContext->getPassManagerCache()->resetStream();
}

// =====================================================================================================================
// Create a fetch shader object
std::unique_ptr<GlueShader> GlueShader::createFetchShader(PipelineState *pipelineState,
                                                          ArrayRef<VertexFetchInfo> fetches,
                                                          const VsEntryRegInfo &vsEntryRegInfo) {
  return std::make_unique<FetchShader>(pipelineState, fetches, vsEntryRegInfo);
}

// =====================================================================================================================
// Create a color export shader object
std::unique_ptr<GlueShader> GlueShader::createColorExportShader(PipelineState *pipelineState,
                                                                ArrayRef<ColorExportInfo> exports) {
  return std::make_unique<ColorExportShader>(pipelineState, exports);
}

// =====================================================================================================================
// Create a null fragment shader object
std::unique_ptr<GlueShader> GlueShader::createNullFragmentShader(PipelineState *pipelineState) {
  return std::make_unique<NullFragmentShader>(pipelineState);
}
