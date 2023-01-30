/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Google LLC. All Rights Reserved.
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
 * @file  llpcPipelineBuilder.cpp
 * @brief LLPC source file: contains the implementation LLPC pipeline compilation logic for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcPipelineBuilder.h"
#include "llpcCompilationUtils.h"
#include "llpcComputePipelineBuilder.h"
#include "llpcDebug.h"
#include "llpcGraphicsPipelineBuilder.h"
#if VKI_RAY_TRACING
#include "llpcRayTracingPipelineBuilder.h"
#endif
#include "llpcUtil.h"
#include "vkgcUtil.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Factory function that returns a `PipelineBuilder` appropriate for the given pipeline type (e.g., graphics, compute).
// To support new pipeline types, create a new class deriving from `PipelineBuilder` and register its constructor here.
//
// @param compiler : LLPC compiler object.
// @param [in/out] compileInfo : Compilation info of LLPC standalone tool.
// @param dumpOptions : Pipeline dump options. Pipeline dumps are disabled when `std::nullopt` is passed.
// @param printPipelineInfo : Whether to print pipeline info (hash, filenames) before compilation.
// @returns : Concrete `PipelineBuilder` object for this pipeline type.
std::unique_ptr<PipelineBuilder> createPipelineBuilder(ICompiler &compiler, CompileInfo &compileInfo,
                                                       std::optional<Vkgc::PipelineDumpOptions> dumpOptions,
                                                       bool printPipelineInfo) {
  const unsigned stageMask = compileInfo.stageMask;

#if VKI_RAY_TRACING
  assert(!(isGraphicsPipeline(stageMask) && isComputePipeline(stageMask) && isRayTracingPipeline(stageMask)) &&
         "Invalid stage mask");
#else
  assert(!(isGraphicsPipeline(stageMask) && isComputePipeline(stageMask)) && "Invalid stage mask");
#endif

  if (isGraphicsPipeline(stageMask))
    return std::make_unique<GraphicsPipelineBuilder>(compiler, compileInfo, dumpOptions, printPipelineInfo);

  if (isComputePipeline(stageMask))
    return std::make_unique<ComputePipelineBuilder>(compiler, compileInfo, dumpOptions, printPipelineInfo);

#if VKI_RAY_TRACING
  if (isRayTracingPipeline(stageMask))
    return std::make_unique<RayTracingPipelineBuilder>(compiler, compileInfo, dumpOptions, printPipelineInfo);
#endif

  llvm_unreachable("Unknown pipeline kind");
  return nullptr;
}

// =====================================================================================================================
// Runs pre-compilation actions: starts a pipeline dump (if requested) and prints pipeline info (if requested).
// The caller must call `runPostBuildActions` after calling this function to perform the necessary cleanup.
//
// @param buildInfo : Build info of the pipeline.
// @returns : Handle to the started pipeline dump, nullptr if pipeline dump was not started.
void *PipelineBuilder::runPreBuildActions(PipelineBuildInfo buildInfo) {
  void *pipelineDumpHandle = nullptr;
  if (shouldDumpPipelines())
    pipelineDumpHandle = IPipelineDumper::BeginPipelineDump(&m_dumpOptions.value(), buildInfo);

  if (m_printPipelineInfo)
    printPipelineInfo(buildInfo);

  return pipelineDumpHandle;
}

// =====================================================================================================================
// Runs post-compilation actions: finalizes the pipeline dump, if started. Must be called after `runPreBuildActions`.
//
// @param pipelineDumpHandle : Handle to the started pipeline dump.
// @param pipeline : The compiled pipeline.
void PipelineBuilder::runPostBuildActions(void *pipelineDumpHandle, MutableArrayRef<BinaryData> pipelines) {
  if (!pipelineDumpHandle)
    return;

  for (const auto &pipeline : pipelines)
    if (pipeline.pCode)
      IPipelineDumper::DumpPipelineBinary(pipelineDumpHandle, m_compileInfo.gfxIp, &pipeline);

  IPipelineDumper::EndPipelineDump(pipelineDumpHandle);
}

// =====================================================================================================================
// Prints pipeline dump hash code and filenames. Can be called before pipeline compilation.
//
// @param buildInfo : Build info of the pipeline.
void PipelineBuilder::printPipelineInfo(PipelineBuildInfo buildInfo) {
  const uint64_t hash = getPipelineHash(buildInfo);
  LLPC_OUTS("LLPC PipelineHash: " << format("0x%016" PRIX64, hash) << " Files: "
                                  << join(map_range(m_compileInfo.inputSpecs,
                                                    [](const InputSpec &spec) { return spec.filename; }),
                                          " ")
                                  << "\n");
}

// =====================================================================================================================
// Output LLPC single one elf ((ELF binary, ISA assembly text, or LLVM bitcode)) of pipeline binaries to the specified
// target file.
//
// @param pipelineBin : Output elf pipeline binary
// @param suppliedOutFile : Name of the file to output ELF binary (specify "" to use the base name of first input file
// with appropriate extension; specify "-" to use stdout)
// @param firstInFile : Name of first input file
// @returns : `ErrorSuccess` on success, `ResultError` on failure
Error PipelineBuilder::outputElf(const BinaryData &pipelineBin, const StringRef suppliedOutFile,
                                 StringRef firstInFile) {
  SmallString<64> outFileName(suppliedOutFile);
  if (outFileName.empty()) {
    // Detect the data type as we are unable to access the values of the options "-filetype" and "-emit-llvm".
    StringLiteral ext = fileExtFromBinary(pipelineBin);
    outFileName = sys::path::filename(firstInFile);
    sys::path::replace_extension(outFileName, ext);
  }

  return writeFile(pipelineBin, outFileName);
}

} // namespace StandaloneCompiler
} // namespace Llpc
