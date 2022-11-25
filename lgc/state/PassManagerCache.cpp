/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#include "llvm/IR/IRPrintingPasses.h"
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
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
std::pair<lgc::PassManager &, LegacyPassManager &>
PassManagerCache::getGlueShaderPassManager(raw_pwrite_stream &outStream) {
  PassManagerInfo info = {};
  info.isGlue = true;
  return getPassManager(info, outStream);
}

// =====================================================================================================================
// Get pass manager given a PassManagerInfo
//
// @param info : PassManagerInfo to direct how to create the pass manager
// @param outStream : Stream to output ELF info
std::pair<lgc::PassManager &, LegacyPassManager &> PassManagerCache::getPassManager(const PassManagerInfo &info,
                                                                                    raw_pwrite_stream &outStream) {
  // Set our single proxy stream to use the provided stream.
  m_proxyStream.setUnderlyingStream(&outStream);

  // Check the cache.
  std::pair<std::unique_ptr<lgc::PassManager>, std::unique_ptr<LegacyPassManager>> &passManagers =
      m_cache[StringRef(reinterpret_cast<const char *>(&info), sizeof(info))];
  if (passManagers.first)
    return {*passManagers.first, *passManagers.second};

  // Need to create the pass manager.
  // TODO: Creation of a normal compilation pass manager, not just one for a glue shader.
  assert(info.isGlue && "Non-glue shader compilation not implemented yet");

  passManagers.first.reset(PassManager::Create(m_lgcContext));
  passManagers.first->registerFunctionAnalysis([&] { return m_lgcContext->getTargetMachine()->getTargetIRAnalysis(); });

  // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
  m_lgcContext->preparePassManager(*passManagers.first);

  // Add a few optimizations.
  FunctionPassManager fpm;
  fpm.addPass(InstCombinePass(5));
  fpm.addPass(InstSimplifyPass());
  fpm.addPass(EarlyCSEPass(true));
  passManagers.first->addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  // Add one last pass that does nothing, but invalidates all the analyses.
  // This is required to avoid the pass manager to use results of analyses from
  // previous runs which is causing random crashes.
  passManagers.first->addPass(InvalidateAllAnalysesPass());
  // Dump the result
  if (raw_ostream *outs = LgcContext::getLgcOuts()) {
    passManagers.first->addPass(
        PrintModulePass(*outs, "===============================================================================\n"
                               "// LGC glue shader results\n"));
  }

  // Code generation.
  passManagers.second.reset(LegacyPassManager::Create());
  m_lgcContext->addTargetPasses(*passManagers.second, nullptr, m_proxyStream);

  return {*passManagers.first, *passManagers.second};
}

// =====================================================================================================================
// Removes references to the cached stream.  This must be called before the cached stream has been destroyed.
//
void PassManagerCache::resetStream() {
  m_proxyStream.setUnderlyingStream(nullptr);
}
