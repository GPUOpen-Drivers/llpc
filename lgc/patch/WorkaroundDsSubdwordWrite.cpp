/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  WorkaroundDsSubdwordWrite.cpp
 * @brief LLPC source file: contains implementation of class lgc::WorkaroundDsSubdwordWrite.
 ***********************************************************************************************************************
 */

#include "lgc/patch/WorkaroundDsSubdwordWrite.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-workaround-ds-subdword-write"

using namespace lgc;
using namespace llvm;

static cl::opt<bool> WorkaroundSubdwordWrite("workaround-subdword-write",
                                             cl::desc("Waterfall loop around ds_write of subdword size"),
                                             cl::init(false));

namespace lgc {

// =====================================================================================================================
// Executes the WorkaroundDsSubdwordWrite LLVM pass on the specified LLVM function.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses WorkaroundDsSubdwordWrite::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass WorkaroundDsSubdwordWrite\n");
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  bool workaroundSubdwordWrite = 0;
  if (WorkaroundSubdwordWrite.getNumOccurrences())
    workaroundSubdwordWrite = WorkaroundSubdwordWrite.getValue();
  auto gfxIp = pipelineState->getTargetInfo().getGfxIpVersion();
  if (!workaroundSubdwordWrite || gfxIp.major != 11 || gfxIp.minor != 5)
    return PreservedAnalyses::all();
  bool isChanged = false;
  for (Function &func : module.getFunctionList()) {
    for (BasicBlock &block : func) {
      for (Instruction &inst : block) {
        StoreInst *SI = dyn_cast<StoreInst>(&inst);
        if (!SI)
          continue;
        if (SI->getPointerAddressSpace() != ADDR_SPACE_LOCAL)
          continue;
        if (SI->getValueOperand()->getType()->getScalarSizeInBits() >= 32)
          continue;
        LLVM_DEBUG(dbgs() << "Inserting waterfall loop workaround for sub-dword store to DS memory:\n");
        LLVM_DEBUG(dbgs() << SI);
        LLVM_DEBUG(dbgs() << "\n");
        BuilderImpl builderImpl(pipelineState);
        builderImpl.createWaterfallLoop(SI, /*ptr must be uniform*/ 1, false, /*useVgprForOperands*/ true, "");
        isChanged = true;
      }
    }
  }
  return isChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // namespace lgc
