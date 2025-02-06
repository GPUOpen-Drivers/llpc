/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerInstMetaRemove.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerInstMetaRemove.
 ***********************************************************************************************************************
 */
#include "LowerInstMetaRemove.h"
#include "SPIRVInternal.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lower-inst-meta-remove"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
LowerInstMetaRemove::LowerInstMetaRemove() {
}

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerInstMetaRemove::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Inst-Meta-Remove\n");

  SpirvLower::init(&module);
  bool changed = false;

  // Remove calls to functions whose names start with "spirv.NonUniform".
  SmallVector<CallInst *, 8> callsToRemove;
  for (auto &func : *m_module) {
    if (func.getName().starts_with(gSPIRVName::NonUniform)) {
      for (auto &use : func.uses()) {
        if (auto *callInst = dyn_cast<CallInst>(use.getUser())) {
          if (callInst->isCallee(&use))
            callsToRemove.push_back(callInst);
        }
      }
    }
  }
  for (auto *callInst : callsToRemove) {
    callInst->dropAllReferences();
    callInst->eraseFromParent();
    changed = true;
  }

  // Remove any named metadata in the module that starts "spirv.".
  SmallVector<NamedMDNode *, 8> nodesToRemove;
  for (auto &namedMdNode : m_module->named_metadata()) {
    if (namedMdNode.getName().starts_with(gSPIRVMD::Prefix))
      nodesToRemove.push_back(&namedMdNode);
  }
  for (NamedMDNode *namedMdNode : nodesToRemove) {
    namedMdNode->eraseFromParent();
    changed = true;
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace Llpc
