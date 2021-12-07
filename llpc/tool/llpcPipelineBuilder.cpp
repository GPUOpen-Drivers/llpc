/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcCompilationUtils.cpp
 * @brief LLPC source file: contains the implementation LLPC pipeline compilation logic for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcPipelineBuilder.h"
#include "llpcAutoLayout.h"
#include "llpcDebug.h"
#include "llpcInputUtils.h"
#include "llpcUtil.h"
#include "vkgcUtil.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Builds pipeline using the provided build info and performs linking.
//
// @returns : Result::Success on success, other status on failure.
Result PipelineBuilder::build() {
  // Even though `build*Pipeline` produces a single `BinaryData` today, future pipeline types (e.g., raytracing) will
  // result multiple outputs. We use a vector instead of a single variable to prepare for that.
  SmallVector<BinaryData, 1> pipelines;
  pipelines.push_back({});
  const bool isGraphics = isGraphicsPipeline(m_compileInfo.stageMask);
  const bool isCompute = isComputePipeline(m_compileInfo.stageMask);
  assert(!(isGraphics && isCompute) && "Bad stage mask");

  if (isGraphics) {
    Result result = buildGraphicsPipeline(pipelines[0]);
    if (result != Result::Success)
      return result;
  } else if (isCompute) {
    Result result = buildComputePipeline(pipelines[0]);
    if (result != Result::Success)
      return result;
  } else {
    return Result::ErrorInvalidShader;
  }

  for (const auto &pipeline : pipelines) {
    Result result = decodePipelineBinary(&pipeline, &m_compileInfo);
    if (result != Result::Success)
      return result;
  }
  return Result::Success;
}

// =====================================================================================================================
// Build the graphics pipeline.
//
// @param [in/out] outBinaryData : The compiled pipeline. The caller must value-initialize this parameter.
// @returns : Result::Success on success, other status on failure.
Result PipelineBuilder::buildGraphicsPipeline(BinaryData &outBinaryData) {
  GraphicsPipelineBuildInfo *pipelineInfo = &m_compileInfo.gfxPipelineInfo;
  GraphicsPipelineBuildOut *pipelineOut = &m_compileInfo.gfxPipelineOut;

  // Fill pipeline shader info.
  PipelineShaderInfo *shaderInfos[ShaderStageGfxCount] = {
      &pipelineInfo->vs, &pipelineInfo->tcs, &pipelineInfo->tes, &pipelineInfo->gs, &pipelineInfo->fs,
  };

  ResourceMappingNodeMap nodeSets;
  unsigned pushConstSize = 0;
  for (StandaloneCompiler::ShaderModuleData &moduleData : m_compileInfo.shaderModuleDatas) {
    const ShaderStage stage = moduleData.shaderStage;
    PipelineShaderInfo *shaderInfo = shaderInfos[stage];
    const ShaderModuleBuildOut *shaderOut = &moduleData.shaderOut;

    // If entry target is not specified, use the one from command line option.
    if (!shaderInfo->pEntryTarget)
      shaderInfo->pEntryTarget = m_compileInfo.entryTarget.c_str();

    shaderInfo->pModuleData = shaderOut->pModuleData;
    shaderInfo->entryStage = stage;

    // If not compiling from pipeline, lay out user data now.
    if (m_compileInfo.doAutoLayout)
      doAutoLayoutDesc(stage, moduleData.spirvBin, pipelineInfo, shaderInfo, nodeSets, pushConstSize,
                       /*autoLayoutDesc = */ m_compileInfo.autoLayoutDesc);
  }

  if (m_compileInfo.doAutoLayout)
    buildTopLevelMapping(m_compileInfo.stageMask, nodeSets, pushConstSize, &pipelineInfo->resourceMapping,
                         m_compileInfo.autoLayoutDesc);

  pipelineInfo->pInstance = nullptr; // Placeholder, unused.
  pipelineInfo->pUserData = &m_compileInfo.pipelineBuf;
  pipelineInfo->pfnOutputAlloc = allocateBuffer;
  pipelineInfo->unlinked = m_compileInfo.unlinked;

  // NOTE: If number of patch control points is not specified, we set it to 3.
  if (pipelineInfo->iaState.patchControlPoints == 0)
    pipelineInfo->iaState.patchControlPoints = 3;

  pipelineInfo->options.robustBufferAccess = m_compileInfo.robustBufferAccess;
  pipelineInfo->options.enableRelocatableShaderElf = m_compileInfo.relocatableShaderElf;
  pipelineInfo->options.scalarBlockLayout = m_compileInfo.scalarBlockLayout;
  pipelineInfo->options.enableScratchAccessBoundsChecks = m_compileInfo.scratchAccessBoundsChecks;

  PipelineBuildInfo localPipelineInfo = {};
  localPipelineInfo.pGraphicsInfo = pipelineInfo;
  void *pipelineDumpHandle = runPreBuildActions(localPipelineInfo);
  auto onExit = make_scope_exit([&] { runPostBuildActions(pipelineDumpHandle, outBinaryData); });

  Result result = m_compiler.BuildGraphicsPipeline(pipelineInfo, pipelineOut, pipelineDumpHandle);
  if (result != Result::Success)
    return result;

  outBinaryData = pipelineOut->pipelineBin;
  return Result::Success;
}

// =====================================================================================================================
// Build the compute pipeline.
//
// @param [in/out] outBinaryData : The compiled pipeline. The caller must value-initialize this parameter.
// @returns : Result::Success on success, other status on failure.
Result PipelineBuilder::buildComputePipeline(BinaryData &outBinaryData) {
  assert(m_compileInfo.shaderModuleDatas.size() == 1);
  assert(m_compileInfo.shaderModuleDatas[0].shaderStage == ShaderStageCompute);

  ComputePipelineBuildInfo *pipelineInfo = &m_compileInfo.compPipelineInfo;
  ComputePipelineBuildOut *pipelineOut = &m_compileInfo.compPipelineOut;

  PipelineShaderInfo *shaderInfo = &pipelineInfo->cs;
  const ShaderModuleBuildOut *shaderOut = &m_compileInfo.shaderModuleDatas[0].shaderOut;

  // If entry target is not specified, use the one from command line option.
  if (!shaderInfo->pEntryTarget)
    shaderInfo->pEntryTarget = m_compileInfo.entryTarget.c_str();

  shaderInfo->entryStage = ShaderStageCompute;
  shaderInfo->pModuleData = shaderOut->pModuleData;

  // If not compiling from pipeline, lay out user data now.
  if (m_compileInfo.doAutoLayout) {
    ResourceMappingNodeMap nodeSets;
    unsigned pushConstSize = 0;
    doAutoLayoutDesc(ShaderStageCompute, m_compileInfo.shaderModuleDatas[0].spirvBin, nullptr, shaderInfo, nodeSets,
                     pushConstSize, /*autoLayoutDesc =*/m_compileInfo.autoLayoutDesc);

    buildTopLevelMapping(ShaderStageComputeBit, nodeSets, pushConstSize, &pipelineInfo->resourceMapping,
                         m_compileInfo.autoLayoutDesc);
  }

  pipelineInfo->pInstance = nullptr; // Placeholder, unused.
  pipelineInfo->pUserData = &m_compileInfo.pipelineBuf;
  pipelineInfo->pfnOutputAlloc = allocateBuffer;
  pipelineInfo->unlinked = m_compileInfo.unlinked;
  pipelineInfo->options.robustBufferAccess = m_compileInfo.robustBufferAccess;
  pipelineInfo->options.enableRelocatableShaderElf = m_compileInfo.relocatableShaderElf;
  pipelineInfo->options.scalarBlockLayout = m_compileInfo.scalarBlockLayout;
  pipelineInfo->options.enableScratchAccessBoundsChecks = m_compileInfo.scratchAccessBoundsChecks;

  PipelineBuildInfo localPipelineInfo = {};
  localPipelineInfo.pComputeInfo = pipelineInfo;
  void *pipelineDumpHandle = runPreBuildActions(localPipelineInfo);
  auto onExit = make_scope_exit([&] { runPostBuildActions(pipelineDumpHandle, outBinaryData); });

  Result result = m_compiler.BuildComputePipeline(pipelineInfo, pipelineOut, pipelineDumpHandle);
  if (result != Result::Success)
    return result;

  outBinaryData = pipelineOut->pipelineBin;
  return Result::Success;
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
    pipelineDumpHandle = IPipelineDumper::BeginPipelineDump(m_dumpOptions.getPointer(), buildInfo);

  if (m_printPipelineInfo)
    printPipelineInfo(buildInfo);

  return pipelineDumpHandle;
}

// =====================================================================================================================
// Runs post-compilation actions: finalizes the pipeline dump, if started. Must be called after `runPreBuildActions`.
//
// @param pipelineDumpHandle : Handle to the started pipeline dump.
// @param pipeline : The compiled pipeline.
void PipelineBuilder::runPostBuildActions(void *pipelineDumpHandle, BinaryData pipeline) {
  if (!pipelineDumpHandle)
    return;

  if (pipeline.pCode)
    IPipelineDumper::DumpPipelineBinary(pipelineDumpHandle, m_compileInfo.gfxIp, &pipeline);

  IPipelineDumper::EndPipelineDump(pipelineDumpHandle);
}

// =====================================================================================================================
// Prints pipeline dump hash code and filenames. Can be called before pipeline compilation.
//
// @param buildInfo : Build info of the pipeline.
void PipelineBuilder::printPipelineInfo(PipelineBuildInfo buildInfo) {
  uint64_t hash = 0;
  if (isGraphicsPipeline(m_compileInfo.stageMask))
    hash = IPipelineDumper::GetPipelineHash(buildInfo.pGraphicsInfo);
  else if (isComputePipeline(m_compileInfo.stageMask))
    hash = IPipelineDumper::GetPipelineHash(buildInfo.pComputeInfo);
  else
    llvm_unreachable("Unhandled pipeline kind");

  LLPC_OUTS("LLPC PipelineHash: " << format("0x%016" PRIX64, hash) << " Files: " << join(m_compileInfo.fileNames, " ")
                                  << "\n");
}

} // namespace StandaloneCompiler
} // namespace Llpc
