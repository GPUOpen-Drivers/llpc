/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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
 * @file  llpcGraphicsPipelineBuilder.cpp
 * @brief LLPC source file: contains the implementation LLPC graphics pipeline compilation logic for standalone LLPC
 *        compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcGraphicsPipelineBuilder.h"
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
Error GraphicsPipelineBuilder::build() {
  CompileInfo &compileInfo = getCompileInfo();

  Expected<BinaryData> pipelineOrErr = buildGraphicsPipeline();
  if (Error err = pipelineOrErr.takeError())
    return err;

  if (compileInfo.gfxPipelineInfo.enableColorExportShader) {
    // Have outputted elf for each stage. No full pipeline ELF.
    return Error::success();
  }

  Result result = decodePipelineBinary(&*pipelineOrErr, &compileInfo);
  if (result != Result::Success)
    return createResultError(result, "Failed to decode pipeline");

  return Error::success();
}

// =====================================================================================================================
// Build the graphics pipeline.
//
// @returns : Pipeline binary data on success, `llpc::ResultError` on failure.
Expected<BinaryData> GraphicsPipelineBuilder::buildGraphicsPipeline() {
  CompileInfo &compileInfo = getCompileInfo();
  GraphicsPipelineBuildInfo *pipelineInfo = &compileInfo.gfxPipelineInfo;

  // Fill pipeline shader info.
  // clang-format off
  PipelineShaderInfo *shaderInfos[ShaderStageGfxCount] = {
      &pipelineInfo->task,
      &pipelineInfo->vs,
      &pipelineInfo->tcs,
      &pipelineInfo->tes,
      &pipelineInfo->gs,
      &pipelineInfo->mesh,
      &pipelineInfo->fs,
  };
  // clang-format on
  ResourceMappingNodeMap nodeSets;
  unsigned pushConstSize = 0;
  for (StandaloneCompiler::ShaderModuleData &moduleData : compileInfo.shaderModuleDatas) {
    const ShaderStage stage = moduleData.shaderStage;
    PipelineShaderInfo *shaderInfo = shaderInfos[stage];
    const ShaderModuleBuildOut *shaderOut = &moduleData.shaderOut;

    // If entry target is not specified, use the one from command line option.
    if (!shaderInfo->pEntryTarget)
      shaderInfo->pEntryTarget = moduleData.entryPoint.c_str();

    shaderInfo->pModuleData = shaderOut->pModuleData;
    shaderInfo->entryStage = stage;

    // If not compiling from pipeline, lay out user data now.
    if (compileInfo.doAutoLayout)
      doAutoLayoutDesc(stage, moduleData.spirvBin, pipelineInfo, shaderInfo, nodeSets, pushConstSize,
                       /*autoLayoutDesc = */ compileInfo.autoLayoutDesc, false);
  }

  if (compileInfo.doAutoLayout)
    buildTopLevelMapping(compileInfo.stageMask, nodeSets, pushConstSize, &pipelineInfo->resourceMapping,
                         compileInfo.autoLayoutDesc);

  pipelineInfo->pInstance = nullptr; // Placeholder, unused.
  pipelineInfo->pUserData = &compileInfo;
  pipelineInfo->pfnOutputAlloc = allocateBuffer;
  pipelineInfo->unlinked = compileInfo.unlinked;

  if (compileInfo.robustBufferAccess.has_value())
    pipelineInfo->options.robustBufferAccess = *compileInfo.robustBufferAccess;
  if (compileInfo.relocatableShaderElf.has_value())
    pipelineInfo->options.enableRelocatableShaderElf = *compileInfo.relocatableShaderElf;
  if (compileInfo.scalarBlockLayout.has_value())
    pipelineInfo->options.scalarBlockLayout = *compileInfo.scalarBlockLayout;
  if (compileInfo.scratchAccessBoundsChecks.has_value())
    pipelineInfo->options.enableScratchAccessBoundsChecks = *compileInfo.scratchAccessBoundsChecks;
  if (compileInfo.enableImplicitInvariantExports.has_value())
    pipelineInfo->options.enableImplicitInvariantExports = *compileInfo.enableImplicitInvariantExports;
  if (compileInfo.optimizationLevel.has_value()) {
    pipelineInfo->options.optimizationLevel = static_cast<uint32_t>(compileInfo.optimizationLevel.value());
  }
  pipelineInfo->options.internalRtShaders = compileInfo.internalRtShaders;
  pipelineInfo->enableColorExportShader |= compileInfo.enableColorExportShader;

  PipelineBuildInfo localPipelineInfo = {};
  localPipelineInfo.pGraphicsInfo = pipelineInfo;
  void *pipelineDumpHandle = runPreBuildActions(localPipelineInfo);
  auto onExit = make_scope_exit([&] {
    std::vector<BinaryData> binaries;
    for (const auto &out : compileInfo.gfxPipelineOut)
      binaries.push_back(out.pipelineBin);
    runPostBuildActions(pipelineDumpHandle, binaries);
  });

  if (compileInfo.isGraphicsLibrary) {
    Result result = Result::Success;
    GraphicsPipelineBuildOut pipelineOut = {};
    if (compileInfo.stageMask == 0) {
      result = getCompiler().BuildColorExportShader(pipelineInfo, compileInfo.fsOutputs.data(), &pipelineOut,
                                                    pipelineDumpHandle);

    } else if (compileInfo.stageMask & Vkgc::ShaderStageBit::ShaderStageFragmentBit) {
      result =
          getCompiler().buildGraphicsShaderStage(pipelineInfo, &pipelineOut, UnlinkedStageFragment, pipelineDumpHandle);
    } else {
      result = getCompiler().buildGraphicsShaderStage(pipelineInfo, &pipelineOut, UnlinkedStageVertexProcess,
                                                      pipelineDumpHandle);
    }

    if (result != Result::Success)
      return createResultError(result, "Graphics pipeline compilation failed");

    compileInfo.gfxPipelineOut.emplace_back(pipelineOut);
    return pipelineOut.pipelineBin;
  }

  if (pipelineInfo->enableColorExportShader) {
    GraphicsPipelineBuildOut pipelineOut = {};
    Result result = getCompiler().buildGraphicsShaderStage(pipelineInfo, &pipelineOut, UnlinkedStageVertexProcess,
                                                           pipelineDumpHandle);
    compileInfo.gfxPipelineOut.emplace_back(pipelineOut);

    if (result == Result::Success) {
      pipelineOut = {};
      result =
          getCompiler().buildGraphicsShaderStage(pipelineInfo, &pipelineOut, UnlinkedStageFragment, pipelineDumpHandle);
      compileInfo.gfxPipelineOut.emplace_back(pipelineOut);
    }

    if (result == Result::Success && pipelineOut.fsOutputMetaData != nullptr) {
      void *fsOutputMetadata = pipelineOut.fsOutputMetaData;

      pipelineOut = {};
      result = getCompiler().BuildColorExportShader(pipelineInfo, fsOutputMetadata, &pipelineOut, pipelineDumpHandle);
      compileInfo.gfxPipelineOut.emplace_back(pipelineOut);
    }

    if (result != Result::Success)
      return createResultError(result, "Graphics pipeline compilation failed");

    return pipelineOut.pipelineBin;
  }

  GraphicsPipelineBuildOut pipelineOut = {};
  Result result = getCompiler().BuildGraphicsPipeline(pipelineInfo, &pipelineOut, pipelineDumpHandle);
  if (result != Result::Success)
    return createResultError(result, "Graphics pipeline compilation failed");

  compileInfo.gfxPipelineOut.emplace_back(pipelineOut);
  return pipelineOut.pipelineBin;
}

// =====================================================================================================================
// Calculates the pipeline hash.
//
// @param buildInfo : Pipeline build info.
// @returns : Calculated pipeline hash.
uint64_t GraphicsPipelineBuilder::getPipelineHash(Vkgc::PipelineBuildInfo buildInfo) {
  return IPipelineDumper::GetPipelineHash(buildInfo.pGraphicsInfo);
}

// =====================================================================================================================
// Output LLPC resulting binaries
//
// @param suppliedOutFile : Name of the file to output ELF binary
// @returns : `llvm::ErrorSuccess` on success, `llpc::ResultError` on failure.
Error GraphicsPipelineBuilder::outputElfs(const StringRef suppliedOutFile) {
  CompileInfo &compileInfo = getCompileInfo();
  const InputSpec &firstInput = compileInfo.inputSpecs.front();

  for (const auto &pipelineOut : compileInfo.gfxPipelineOut) {
    if (Error err = outputElf(pipelineOut.pipelineBin, suppliedOutFile, firstInput.filename))
      return err;
  }

  return Error::success();
}

} // namespace StandaloneCompiler
} // namespace Llpc
