/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcRayTracingPipelineBuilder.cpp
 * @brief LLPC source file: contains the implementation LLPC ray tracing pipeline compilation logic for standalone LLPC
 *        compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcRayTracingPipelineBuilder.h"
#include "llpcAutoLayout.h"
#include "llpcCompilationUtils.h"
#include "llpcUtil.h"
#include "vkgcUtil.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Builds pipeline using the provided build info and performs linking.
//
// @returns : `llvm::ErrorSuccess` on success, `llpc::ResultError` on failure.
Error RayTracingPipelineBuilder::build() {
  CompileInfo &compileInfo = getCompileInfo();

  Expected<SmallVector<BinaryData>> pipelineOrErr = buildRayTracingPipeline();
  if (Error err = pipelineOrErr.takeError())
    return err;

  for (const auto &pipeline : *pipelineOrErr) {
    Result result = decodePipelineBinary(&pipeline, &compileInfo);
    if (result != Result::Success)
      return createResultError(result, "Failed to decode pipeline");
  }

  return Error::success();
}

// =====================================================================================================================
// Build the ray tracing pipeline.
//
// @returns : Pipeline binary data on success, `llpc::ResultError` on failure.
Expected<SmallVector<BinaryData>> RayTracingPipelineBuilder::buildRayTracingPipeline() {
  CompileInfo &compileInfo = getCompileInfo();
  RayTracingPipelineBuildInfo *pipelineInfo = &compileInfo.rayTracePipelineInfo;
  RayTracingPipelineBuildOut *pipelineOut = &compileInfo.rayTracingPipelineOut;

  for (unsigned i = 0; i < compileInfo.shaderModuleDatas.size(); ++i) {
    PipelineShaderInfo *shaderInfo = &pipelineInfo->pShaders[i];
    const ShaderModuleBuildOut *shaderOut = &(compileInfo.shaderModuleDatas[i].shaderOut);
    shaderInfo->pModuleData = shaderOut->pModuleData;
    if (!shaderInfo->pEntryTarget) {
      // If entry target is not specified, use the one from command line option
      shaderInfo->pEntryTarget = compileInfo.shaderModuleDatas[i].entryPoint.c_str();
    }
  }

  pipelineInfo->pInstance = nullptr; // Dummy, unused
  pipelineInfo->pUserData = &compileInfo.pipelineBuf;
  pipelineInfo->pfnOutputAlloc = allocateBuffer;
  pipelineInfo->options.robustBufferAccess = compileInfo.robustBufferAccess;
  pipelineInfo->rtState.nodeStrideShift = Log2_32(compileInfo.bvhNodeStride);

  PipelineBuildInfo localPipelineInfo = {};
  localPipelineInfo.pRayTracingInfo = pipelineInfo;
  void *pipelineDumpHandle = runPreBuildActions(localPipelineInfo);

  Result result = getCompiler().BuildRayTracingPipeline(pipelineInfo, pipelineOut, pipelineDumpHandle);

  SmallVector<BinaryData> pipelines(pipelineOut->pipelineBins,
                                    pipelineOut->pipelineBins + pipelineOut->pipelineBinCount);
  runPostBuildActions(pipelineDumpHandle, pipelines);

  if (result != Result::Success)
    return createResultError(result, "Ray tracing pipeline compilation failed");

  return pipelines;
}

// =====================================================================================================================
// Calculates the pipeline hash.
//
// @param buildInfo : Pipeline build info.
// @returns : Calculated pipeline hash.
uint64_t RayTracingPipelineBuilder::getPipelineHash(Vkgc::PipelineBuildInfo buildInfo) {
  return IPipelineDumper::GetPipelineHash(buildInfo.pRayTracingInfo);
}

// =====================================================================================================================
// Output LLPC resulting binaries
//
// @param suppliedOutFile : Name of the file to output ELF binary
// @returns : `llvm::ErrorSuccess` on success, `llpc::ResultError` on failure.
Error RayTracingPipelineBuilder::outputElfs(const StringRef suppliedOutFile) {
  CompileInfo &compileInfo = getCompileInfo();
  for (unsigned i = 0; i < compileInfo.rayTracingPipelineOut.pipelineBinCount; ++i) {
    const BinaryData &pipelineBin = compileInfo.rayTracingPipelineOut.pipelineBins[i];
    const InputSpec &firstInput = compileInfo.inputSpecs.front();
    SmallString<64> outFileName(suppliedOutFile);
    if (outFileName != "-" && firstInput.filename != "-") {
      StringRef ext;
      if (outFileName.empty()) {
        StringLiteral extLit = fileExtFromBinary(pipelineBin);
        ext = extLit;
        outFileName = sys::path::filename(firstInput.filename);
        sys::path::replace_extension(outFileName, ext);
      } else {
        ext = sys::path::extension(outFileName);
      }

      if (compileInfo.rayTracingPipelineOut.pipelineBinCount > 1)
        sys::path::replace_extension(outFileName, Twine(".") + Twine(i) + ext);
    }
    Error err = outputElf(pipelineBin, outFileName, firstInput.filename);
    if (errorToResult(std::move(err)) != Result::Success)
      return err;
  }
  return Error::success();
}

} // namespace StandaloneCompiler
} // namespace Llpc
