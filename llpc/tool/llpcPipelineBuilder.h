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

#include "llpc.h"
#include "llpcCompilationUtils.h"
#include "llpcError.h"
#include "vkgcDefs.h"
#include "llvm/ADT/Optional.h"
#include <memory>

namespace Llpc {
namespace StandaloneCompiler {

class PipelineBuilder;

// Factory function that returns a `PipelineBuilder` appropriate for the given pipeline type (e.g., graphics, compute).
std::unique_ptr<PipelineBuilder> createPipelineBuilder(ICompiler &compiler, CompileInfo &compileInfo,
                                                       llvm::Optional<Vkgc::PipelineDumpOptions> dumpOptions,
                                                       bool printPipelineInfo);

// Base class for pipeline compilation. Dumps compiled pipelines when requested.
//
// Note: We make all key functions virtual to give experimental implementations maximum freedom.
class PipelineBuilder {
public:
  // Initializes PipelineBuilder. Use `createPipelineBuilder` to create concrete instances of this class.
  //
  // @param compiler : LLPC compiler object.
  // @param [in/out] compileInfo : Compilation info of LLPC standalone tool. This will be modified by `build()`.
  // @param dumpOptions : Pipeline dump options. Pipeline dumps are disabled when `llvm::None` is passed.
  // @param printPipelineInfo : Whether to print pipeline info (hash, filenames) before compilation.
  PipelineBuilder(ICompiler &compiler, CompileInfo &compileInfo, llvm::Optional<Vkgc::PipelineDumpOptions> dumpOptions,
                  bool printPipelineInfo)
      : m_compiler(compiler), m_compileInfo(compileInfo), m_dumpOptions(std::move(dumpOptions)),
        m_printPipelineInfo(printPipelineInfo) {}

  virtual ~PipelineBuilder() = default;

  // Compiles the pipeline and performs linking.
  // The implementations should call `runPreBuildActions` before performing compilation with `m_compiler` and
  // call `runPostBuildActions` after.
  //
  // @returns : Calculated pipeline hash.
  virtual llvm::Error build() = 0;

  // Output LLPC resulting binaries
  //
  // @param suppliedOutFile : Name of the file to output ELF binary
  virtual llvm::Error outputElfs(const llvm::StringRef suppliedOutFile) = 0;

  // Calculates the hash of the compiled pipeline. This is used by `printPipelineInfo` to produce verbose logs.
  //
  // @param buildInfo : Pipeline build information.
  // @returns : Calculated pipeline hash.
  LLPC_NODISCARD virtual uint64_t getPipelineHash(Vkgc::PipelineBuildInfo buildInfo) = 0;

  // Returns the compiler.
  //
  // @returns : Compiler handle.
  ICompiler &getCompiler() { return m_compiler; }

  // Returns the compile info.
  //
  // @returns : Compile info.
  CompileInfo &getCompileInfo() { return m_compileInfo; }

  // Returns the pipeline dump options.
  //
  // @returns : `PipelineDumpOptions` or `llpc::None` if pipeline dumps were not requested.
  llvm::Optional<Vkgc::PipelineDumpOptions> &getDumpOptions() { return m_dumpOptions; }

  // Returns true iff pipeline dumps are requested.
  //
  // @returns : `true` is pipeline dumps were requested, `false` if not.
  LLPC_NODISCARD bool shouldDumpPipelines() const { return m_dumpOptions.has_value(); }

  // Runs optional pre-build code (pipeline dumping, pipeline info printing).
  LLPC_NODISCARD void *runPreBuildActions(Vkgc::PipelineBuildInfo buildInfo);

  // Runs post-build cleanup code. Must be called after `runPrebuildActions`.
  void runPostBuildActions(void *pipelineDumpHandle, llvm::MutableArrayRef<BinaryData> pipelines);

  // Prints pipeline dump hash code and filenames.
  void printPipelineInfo(Vkgc::PipelineBuildInfo buildInfo);

  // Output LLPC single one elf ((ELF binary, ISA assembly text, or LLVM bitcode)) of pipeline binaries to the specified
  // target file.
  llvm::Error outputElf(const BinaryData &pipelineBin, const llvm::StringRef suppliedOutFile,
                        llvm::StringRef firstInFile);

private:
  ICompiler &m_compiler;
  CompileInfo &m_compileInfo;
  llvm::Optional<Vkgc::PipelineDumpOptions> m_dumpOptions = llvm::None;
  bool m_printPipelineInfo = false;
};

} // namespace StandaloneCompiler
} // namespace Llpc
