/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PassManagerCache.cpp
 * @brief LGC source file: Pass manager creator and cache
 ***********************************************************************************************************************
 */
#include "lgc/state/PassManagerCache.h"
#include "lgc/LgcContext.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Information on how to create a pass manager. This is used as the key in the pass manager cache.
struct PassManagerInfo {
  bool isGlue;
};

} // namespace lgc

// =====================================================================================================================
// Get pass manager for glue shader compilation
//
// @param outStream : Stream to output ELF info
lgc::PassManager &PassManagerCache::getGlueShaderPassManager(raw_pwrite_stream &outStream) {
  PassManagerInfo info = {};
  info.isGlue = true;
  return getPassManager(info, outStream);
}

// =====================================================================================================================
// Get pass manager given a PassManagerInfo
//
// @param info : PassManagerInfo to direct how to create the pass manager
// @param outStream : Stream to output ELF info
lgc::PassManager &PassManagerCache::getPassManager(const PassManagerInfo &info, raw_pwrite_stream &outStream) {
  // Set our single proxy stream to use the provided stream.
  m_proxyStream.setUnderlyingStream(&outStream);

  // Check the cache.
  std::unique_ptr<lgc::PassManager> &passManager =
      m_cache[StringRef(reinterpret_cast<const char *>(&info), sizeof(info))];
  if (passManager)
    return *passManager;

  // Need to create the pass manager.
  // TODO: Creation of a normal compilation pass manager, not just one for a glue shader.
  assert(info.isGlue && "Non-glue shader compilation not implemented yet");

  passManager.reset(PassManager::Create());
  passManager->add(createTargetTransformInfoWrapperPass(m_lgcContext->getTargetMachine()->getTargetIRAnalysis()));

  // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
  m_lgcContext->preparePassManager(&*passManager);

  // Add a few optimizations.
  passManager->add(createInstructionCombiningPass(5));
  passManager->add(createInstSimplifyLegacyPass());
  passManager->add(createEarlyCSEPass(true));

  // Dump the result
  if (raw_ostream *outs = LgcContext::getLgcOuts()) {
    passManager->add(
        createPrintModulePass(*outs, "===============================================================================\n"
                                     "// LGC glue shader results\n"));
  }

  // Code generation.
  m_lgcContext->addTargetPasses(*passManager, nullptr, m_proxyStream);

  return *passManager;
}

// =====================================================================================================================
// Removes references to the cached stream.  This must be called before the cached stream has been destroyed.
//
void PassManagerCache::resetStream() {
  m_proxyStream.setUnderlyingStream(nullptr);
}
