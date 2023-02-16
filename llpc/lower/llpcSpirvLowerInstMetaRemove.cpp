/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerInstMetaRemove.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerInstMetaRemove.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerInstMetaRemove.h"
#include "SPIRVInternal.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-spirv-lower-inst-meta-remove"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
SpirvLowerInstMetaRemove::SpirvLowerInstMetaRemove() : m_changed(false) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerInstMetaRemove::run(Module &module, ModuleAnalysisManager &analysisManager) {
  if (runImpl(module))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerInstMetaRemove::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Inst-Meta-Remove\n");

  SpirvLower::init(&module);
  m_changed = false;

  // Remove calls to functions whose names start with "spirv.NonUniform".
  SmallVector<CallInst *, 8> callsToRemove;
  for (auto &func : *m_module) {
    if (func.getName().startswith(gSPIRVName::NonUniform)) {
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
    m_changed = true;
  }

  // Remove any named metadata in the module that starts "spirv.".
  SmallVector<NamedMDNode *, 8> nodesToRemove;
  for (auto &namedMdNode : m_module->named_metadata()) {
    if (namedMdNode.getName().startswith(gSPIRVMD::Prefix))
      nodesToRemove.push_back(&namedMdNode);
  }
  for (NamedMDNode *namedMdNode : nodesToRemove) {
    namedMdNode->eraseFromParent();
    m_changed = true;
  }

  return m_changed;
}

} // namespace Llpc
