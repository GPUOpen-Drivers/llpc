/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  PatchCheckShaderCache.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchCheckShaderCache.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchCheckShaderCache.h"
#include "lgc/CommonDefs.h"
#include "lgc/state/PipelineShaders.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-check-shader-cache"

using namespace llvm;
using namespace lgc;

namespace {

// =====================================================================================================================
// Stream each map key and value for later inclusion in a hash
//
// @param map : Map to stream
// @param [in/out] stream : Stream to output map entries to
template <class MapType> static void streamMapEntries(MapType &map, raw_ostream &stream) {
  size_t mapCount = map.size();
  stream << StringRef(reinterpret_cast<const char *>(&mapCount), sizeof(mapCount));
  for (auto mapIt : map) {
    stream << StringRef(reinterpret_cast<const char *>(&mapIt.first), sizeof(mapIt.first));
    stream << StringRef(reinterpret_cast<const char *>(&mapIt.second), sizeof(mapIt.second));
  }
}

} // namespace

// =====================================================================================================================
PatchCheckShaderCache::PatchCheckShaderCache(Pipeline::CheckShaderCacheFunc callbackFunc)
    : m_callbackFunc(std::move(callbackFunc)) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchCheckShaderCache::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();

  LLVM_DEBUG(dbgs() << "Run the pass Patch-Check-Shader-Cache\n");

  if (m_callbackFunc == nullptr) {
    // No shader cache in use.
    return PreservedAnalyses::all();
  }

  Patch::init(&module);

  std::string inOutUsageStreams[ShaderStage::GfxCount];
  ArrayRef<uint8_t> inOutUsageValues[ShaderStage::GfxCount];
  auto stageMask = pipelineState->getShaderStageMask();

  // Build input/output layout hash per shader stage
  for (const ShaderStageEnum stage : enumRange(ShaderStage::GfxCount)) {
    if (!stageMask.contains(stage))
      continue;

    auto resUsage = pipelineState->getShaderResourceUsage(stage);
    raw_string_ostream stream(inOutUsageStreams[stage]);

    // Update input/output usage
    streamMapEntries(resUsage->inOutUsage.inputLocInfoMap, stream);
    streamMapEntries(resUsage->inOutUsage.outputLocInfoMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchOutputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPrimitiveInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPrimitiveOutputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.builtInInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.builtInOutputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchBuiltInInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchBuiltInOutputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPrimitiveBuiltInInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPrimitiveBuiltInOutputLocMap, stream);

    if (stage == ShaderStage::Geometry) {
      // NOTE: For geometry shader, copy shader will use this special map info (from built-in outputs to
      // locations of generic outputs). We have to add it to shader hash calculation.
      streamMapEntries(resUsage->inOutUsage.gs.builtInOutLocs, stream);
    } else if (stage == ShaderStage::Mesh) {
      // NOTE: For mesh shader, those two special maps are used to export vertex/primitive attributes.
      streamMapEntries(resUsage->inOutUsage.mesh.vertexOutputComponents, stream);
      streamMapEntries(resUsage->inOutUsage.mesh.primitiveOutputComponents, stream);
    }

    // Store the result of the hash for this shader stage.
    stream.flush();
    inOutUsageValues[stage] = ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(inOutUsageStreams[stage].data()),
                                                inOutUsageStreams[stage].size());
  }

  // Ask callback function if it wants to remove any shader stages.
  auto stagesLeftToCompile = m_callbackFunc(&module, stageMask, inOutUsageValues);
  if (stagesLeftToCompile == stageMask)
    return PreservedAnalyses::all();

  // "Remove" a shader stage by making its entry-point function an external but not DLLExport declaration, so further
  // passes no longer treat it as an entry point (based on the DLL storage class) and don't attempt to compile any code
  // for it (because it contains no code).
  for (auto &func : module) {
    if (isShaderEntryPoint(&func)) {
      auto stage = getShaderStage(&func);
      if (stage && !stagesLeftToCompile.contains(stage.value())) {
        func.deleteBody();
        func.setDLLStorageClass(GlobalValue::DefaultStorageClass);
      }
    }
  }
  return PreservedAnalyses::none();
}
