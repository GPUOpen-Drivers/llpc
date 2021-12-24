/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
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
 * @file  llpcPipelineBuilder.h
 * @brief LLPC header file: pipeline compilation logic for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcCompilationUtils.h"
#include "vkgcDefs.h"
#include "llvm/ADT/Optional.h"

namespace Llpc {
namespace StandaloneCompiler {

// Performs pipeline compilation. Dumps compiled pipelines when requested.
class PipelineBuilder {
public:
  // Creates a PipelineBuilder.
  //
  // @param compiler : LLPC compiler object.
  // @param [in/out] compileInfo : Compilation info of LLPC standalone tool. This will be modified by `build()`.
  // @param dumpOptions : Pipeline dump options. Pipeline dumps are disabled when llvm::None is passed.
  // @param printPipelineInfo : Whether to print pipeline info (hash, filenames) before compilation.
  PipelineBuilder(ICompiler &compiler, CompileInfo &compileInfo, llvm::Optional<Vkgc::PipelineDumpOptions> dumpOptions,
                  bool printPipelineInfo)
      : m_compiler(compiler), m_compileInfo(compileInfo), m_dumpOptions(std::move(dumpOptions)),
        m_printPipelineInfo(printPipelineInfo) {}

  // Compiles the pipeline and performs linking.
  LLPC_NODISCARD Result build();

private:
  // Returns true iff pipeline dumps are requested.
  LLPC_NODISCARD bool shouldDumpPipelines() const { return m_dumpOptions.hasValue(); }

  // Builds graphics pipeline and does linking.
  LLPC_NODISCARD Result buildGraphicsPipeline(Vkgc::BinaryData &outPipeline);

  // Builds compute pipeline and does linking.
  LLPC_NODISCARD Result buildComputePipeline(Vkgc::BinaryData &outPipeline);

  // Runs optional pre-build code (pipeline dumping, pipeline info printing).
  LLPC_NODISCARD void *runPreBuildActions(Vkgc::PipelineBuildInfo buildInfo);

  // Runs post-build cleanup code. Must be called after `runPrebuildActions`.
  void runPostBuildActions(void *pipelineDumpHandle, llvm::SmallVector<BinaryData, 1>& pipelines);

  // Prints pipeline dump hash code and filenames.
  void printPipelineInfo(Vkgc::PipelineBuildInfo buildInfo);

  ICompiler &m_compiler;
  CompileInfo &m_compileInfo;
  llvm::Optional<Vkgc::PipelineDumpOptions> m_dumpOptions = llvm::None;
  bool m_printPipelineInfo = false;
};

} // namespace StandaloneCompiler
} // namespace Llpc
