/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcComputePipelineBuilder.cpp
 * @brief LLPC source file: contains the implementation LLPC compute pipeline compilation logic for standalone LLPC
 *        compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcComputePipelineBuilder.h"
#include "llpcAutoLayout.h"
#include "llpcCompilationUtils.h"
#include "llpcUtil.h"
#include "vkgcUtil.h"
#include "llvm/ADT/ScopeExit.h"

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Builds pipeline using the provided build info and performs linking.
//
// @returns : `llvm::ErrorSuccess` on success, `llpc::ResultError` on failure.
Error ComputePipelineBuilder::build() {
  CompileInfo &compileInfo = getCompileInfo();

  Expected<BinaryData> pipelineOrErr = buildComputePipeline();
  if (Error err = pipelineOrErr.takeError())
    return err;

  Result result = decodePipelineBinary(&*pipelineOrErr, &compileInfo);
  if (result != Result::Success)
    return createResultError(result, "Failed to decode pipeline");

  return Error::success();
}

// =====================================================================================================================
// Build the compute pipeline.
//
// @returns : Pipeline binary data on success, `llvm::ResultError` on failure.
Expected<BinaryData> ComputePipelineBuilder::buildComputePipeline() {
  CompileInfo &compileInfo = getCompileInfo();
  assert(compileInfo.shaderModuleDatas.size() == 1);

  StandaloneCompiler::ShaderModuleData &moduleData = compileInfo.shaderModuleDatas[0];
  assert(moduleData.shaderStage == ShaderStageCompute);

  ComputePipelineBuildInfo *pipelineInfo = &compileInfo.compPipelineInfo;
  ComputePipelineBuildOut *pipelineOut = &compileInfo.compPipelineOut;

  PipelineShaderInfo *shaderInfo = &pipelineInfo->cs;
  const ShaderModuleBuildOut *shaderOut = &moduleData.shaderOut;

  // If entry target is not specified, use the one from command line option.
  if (!shaderInfo->pEntryTarget)
    shaderInfo->pEntryTarget = moduleData.entryPoint.c_str();

  shaderInfo->entryStage = ShaderStageCompute;
  shaderInfo->pModuleData = shaderOut->pModuleData;

  // If not compiling from pipeline, lay out user data now.
  if (compileInfo.doAutoLayout) {
    ResourceMappingNodeMap nodeSets;
    unsigned pushConstSize = 0;
    doAutoLayoutDesc(ShaderStageCompute, moduleData.spirvBin, nullptr, shaderInfo, nodeSets, pushConstSize,
                     /*autoLayoutDesc =*/compileInfo.autoLayoutDesc,
                     compileInfo.compPipelineInfo.options.reverseThreadGroup);

    buildTopLevelMapping(ShaderStageComputeBit, nodeSets, pushConstSize, &pipelineInfo->resourceMapping,
                         compileInfo.autoLayoutDesc | compileInfo.compPipelineInfo.options.reverseThreadGroup);
  }

  pipelineInfo->pInstance = nullptr; // Placeholder, unused.
  pipelineInfo->pUserData = &compileInfo.pipelineBuf;
  pipelineInfo->pfnOutputAlloc = allocateBuffer;
  pipelineInfo->unlinked = compileInfo.unlinked;
  pipelineInfo->options.robustBufferAccess = compileInfo.robustBufferAccess;
  pipelineInfo->options.enableRelocatableShaderElf = compileInfo.relocatableShaderElf;
  pipelineInfo->options.scalarBlockLayout = compileInfo.scalarBlockLayout;
  pipelineInfo->options.enableScratchAccessBoundsChecks = compileInfo.scratchAccessBoundsChecks;
  pipelineInfo->options.forceCsThreadIdSwizzling = compileInfo.compPipelineInfo.options.forceCsThreadIdSwizzling;
  pipelineInfo->options.overrideThreadGroupSizeX = compileInfo.compPipelineInfo.options.overrideThreadGroupSizeX;
  pipelineInfo->options.overrideThreadGroupSizeY = compileInfo.compPipelineInfo.options.overrideThreadGroupSizeY;
  pipelineInfo->options.overrideThreadGroupSizeZ = compileInfo.compPipelineInfo.options.overrideThreadGroupSizeZ;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 53
  if (compileInfo.optimizationLevel.has_value()) {
    pipelineInfo->options.optimizationLevel = compileInfo.optimizationLevel.value();
  }
#endif
  pipelineInfo->options.threadGroupSwizzleMode = compileInfo.compPipelineInfo.options.threadGroupSwizzleMode;
  pipelineInfo->options.reverseThreadGroup = compileInfo.compPipelineInfo.options.reverseThreadGroup;
#if VKI_RAY_TRACING
  pipelineInfo->options.internalRtShaders = compileInfo.internalRtShaders;
#endif

  PipelineBuildInfo localPipelineInfo = {};
  localPipelineInfo.pComputeInfo = pipelineInfo;
  void *pipelineDumpHandle = runPreBuildActions(localPipelineInfo);
  auto onExit = make_scope_exit([&] { runPostBuildActions(pipelineDumpHandle, {pipelineOut->pipelineBin}); });

  Result result = getCompiler().BuildComputePipeline(pipelineInfo, pipelineOut, pipelineDumpHandle);
  if (result != Result::Success)
    return createResultError(result, "Compute pipeline compilation failed");

  return pipelineOut->pipelineBin;
}

// =====================================================================================================================
// Calculates the pipeline hash.
//
// @param buildInfo : Pipeline build info.
// @returns : Calculated pipeline hash.
uint64_t ComputePipelineBuilder::getPipelineHash(Vkgc::PipelineBuildInfo buildInfo) {
  return IPipelineDumper::GetPipelineHash(buildInfo.pComputeInfo);
}

// =====================================================================================================================
// Output LLPC resulting binaries
//
// @param suppliedOutFile : Name of the file to output ELF binary
// @returns : `llvm::ErrorSuccess` on success, `llpc::ResultError` on failure.
Error ComputePipelineBuilder::outputElfs(const StringRef suppliedOutFile) {
  CompileInfo &compileInfo = getCompileInfo();
  const InputSpec &firstInput = compileInfo.inputSpecs.front();
  return outputElf(compileInfo.compPipelineOut.pipelineBin, suppliedOutFile, firstInput.filename);
}

} // namespace StandaloneCompiler
} // namespace Llpc
