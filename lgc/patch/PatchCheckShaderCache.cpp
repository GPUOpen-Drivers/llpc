/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchCheckShaderCache.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchCheckShaderCache.
 ***********************************************************************************************************************
 */
#include "PatchCheckShaderCache.h"
#include "lgc/state/PipelineShaders.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-check-shader-cache"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
// Initializes static members.
char PatchCheckShaderCache::ID = 0;

namespace lgc {

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for checking shader cache
PatchCheckShaderCache *createPatchCheckShaderCache() {
  return new PatchCheckShaderCache();
}

} // namespace lgc

namespace {

// =====================================================================================================================
// Stream each map key and value for later inclusion in a hash
template <class MapType>
//
// @param map : Map to stream
// @param [in/out] stream : Stream to output map entries to
static void streamMapEntries(MapType &map, raw_ostream &stream) {
  size_t mapCount = map.size();
  stream << StringRef(reinterpret_cast<const char *>(&mapCount), sizeof(mapCount));
  for (auto mapIt : map) {
    stream << StringRef(reinterpret_cast<const char *>(&mapIt.first), sizeof(mapIt.first));
    stream << StringRef(reinterpret_cast<const char *>(&mapIt.second), sizeof(mapIt.second));
  }
}

} // namespace

// =====================================================================================================================
PatchCheckShaderCache::PatchCheckShaderCache() : Patch(ID) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchCheckShaderCache::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Check-Shader-Cache\n");

  if (m_callbackFunc == nullptr) {
    // No shader cache in use.
    return false;
  }

  Patch::init(&module);

  // NOTE: Global constant are added to the end of pipeline binary. we can't merge ELF binaries if global constant
  // is used in non-fragment shader stages.
  for (auto &global : module.globals()) {
    if (auto globalVar = dyn_cast<GlobalVariable>(&global)) {
      if (globalVar->isConstant()) {
        SmallVector<const Value *, 4> vals;
        vals.push_back(globalVar);
        for (unsigned i = 0; i != vals.size(); ++i) {
          for (auto user : vals[i]->users()) {
            if (isa<Constant>(user)) {
              vals.push_back(user);
              continue;
            }
            if (getShaderStage(cast<Instruction>(user)->getFunction()) != ShaderStageFragment)
              return false;
          }
        }
      }
    }
  }

  std::string inOutUsageStreams[ShaderStageGfxCount];
  ArrayRef<uint8_t> inOutUsageValues[ShaderStageGfxCount];
  PipelineState *pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  auto stageMask = pipelineState->getShaderStageMask();

  // Build input/output layout hash per shader stage
  for (auto stage = ShaderStageVertex; stage < ShaderStageGfxCount; stage = static_cast<ShaderStage>(stage + 1)) {
    if ((stageMask & shaderStageToMask(stage)) == 0)
      continue;

    auto resUsage = pipelineState->getShaderResourceUsage(stage);
    raw_string_ostream stream(inOutUsageStreams[stage]);

    // Update input/output usage
    streamMapEntries(resUsage->inOutUsage.inputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.outputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.inOutLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchOutputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.builtInInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.builtInOutputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchBuiltInInputLocMap, stream);
    streamMapEntries(resUsage->inOutUsage.perPatchBuiltInOutputLocMap, stream);

    if (stage == ShaderStageGeometry) {
      // NOTE: For geometry shader, copy shader will use this special map info (from built-in outputs to
      // locations of generic outputs). We have to add it to shader hash calculation.
      streamMapEntries(resUsage->inOutUsage.gs.builtInOutLocs, stream);
    }

    // Store the result of the hash for this shader stage.
    stream.flush();
    inOutUsageValues[stage] = ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(inOutUsageStreams[stage].data()),
                                                inOutUsageStreams[stage].size());
  }

  // Ask callback function if it wants to remove any shader stages.
  unsigned modifiedStageMask = m_callbackFunc(&module, stageMask, inOutUsageValues);
  if (modifiedStageMask == stageMask)
    return false;

  // "Remove" a shader stage by making its entry-point function internal, so it gets removed later.
  for (auto &func : module) {
    if (!func.empty() && func.getLinkage() != GlobalValue::InternalLinkage) {
      auto stage = getShaderStage(&func);
      if (stage != ShaderStageInvalid && (shaderStageToMask(stage) & ~modifiedStageMask) != 0)
        func.setLinkage(GlobalValue::InternalLinkage);
    }
  }
  return true;
}

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for checking shader cache
INITIALIZE_PASS(PatchCheckShaderCache, DEBUG_TYPE, "Patch LLVM for checking shader cache", false, false)
