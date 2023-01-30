/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerTerminator.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerTerminator.
 * @details This pass removes trailing instructions after known terminators.
 *          These dead instructions can occur when functions calling terminators, such as OpKill, are inlined.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerTerminator.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcSpirvLower.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-spirv-lower-terminator"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerTerminator::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerTerminator::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Terminator\n");

  SpirvLower::init(&module);

  // Kills are only valid in fragment shader model.
  if (m_shaderStage != ShaderStageFragment)
    return false;

  // Invoke handling of "kill" instructions.
  visit(m_module);

  // Remove any dead instructions.
  bool changed = !m_removalStack.empty();
  while (!m_removalStack.empty()) {
    auto deadInst = m_removalStack.pop_back_val();
    LLVM_DEBUG(dbgs() << "remove: " << *deadInst << "\n");
    deadInst->dropAllReferences();
    deadInst->eraseFromParent();
  }
  m_instsForRemoval.clear();

  return changed;
}

// =====================================================================================================================
// Visits "call" instruction.
//
// Look for kills followed by instructions other than a return.
// If found, mark dead instructions for removal and add a return immediately following the kill.
//
// @param callInst : "Call" instruction
void SpirvLowerTerminator::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  auto mangledName = callee->getName();
  if (mangledName != "lgc.create.kill")
    return;

  // Already marked for removal?
  if (m_instsForRemoval.count(&callInst))
    return;

  BasicBlock *parentBlock = callInst.getParent();
  auto instIter = std::next(callInst.getIterator());
  assert(instIter != parentBlock->end() && "Should not be at end of block; illegal IR?");

  // Already has a return?
  if (isa<ReturnInst>(*instIter))
    return;

  // If terminator branches to block containing a PHI node which references
  // this block then instructions following kill may be part of a return
  // value sequence. In which case we cannot safely modify the instructions.
  auto blockTerm = parentBlock->getTerminator();
  if (!blockTerm)
    return;
  if (BranchInst *branch = dyn_cast<BranchInst>(blockTerm)) {
    assert(branch->isUnconditional());
    BasicBlock *targetBlock = branch->getSuccessor(0);
    for (PHINode &phiNode : targetBlock->phis()) {
      for (BasicBlock *incomingBlock : phiNode.blocks()) {
        if (incomingBlock == parentBlock)
          return;
      }
    }
  }

  // Add return
  ReturnInst::Create(*m_context, nullptr, &*instIter);

  // Mark all other instructions for removal
  while (instIter != parentBlock->end()) {
    if (m_instsForRemoval.insert(&*instIter).second)
      m_removalStack.emplace_back(&*instIter);
    ++instIter;
  }
}

} // namespace Llpc
