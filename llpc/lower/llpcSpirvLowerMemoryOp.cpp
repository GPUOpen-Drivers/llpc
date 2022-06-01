/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerMemoryOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerMemoryOp.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerMemoryOp.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-spirv-lower-memory-op"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Initializes static members.
char LegacySpirvLowerMemoryOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering memory operations.
ModulePass *createLegacySpirvLowerMemoryOp() {
  return new LegacySpirvLowerMemoryOp();
}

// =====================================================================================================================
LegacySpirvLowerMemoryOp::LegacySpirvLowerMemoryOp() : ModulePass(ID) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerMemoryOp::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerMemoryOp::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Memory-Op\n");

  SpirvLower::init(&module);

  visit(m_module);

  for (auto inst : m_removeInsts) {
    assert(inst->user_empty());
    inst->dropAllReferences();
    inst->eraseFromParent();
  }
  m_removeInsts.clear();

  LLVM_DEBUG(dbgs() << "After the pass Spirv-Lower-Memory-Op " << module);

  return true;
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool LegacySpirvLowerMemoryOp::runOnModule(Module &module) {
  return Impl.runImpl(module);
}

// =====================================================================================================================
// Visits "extractelement" instruction.
//
// @param extractElementInst : "ExtractElement" instruction
void SpirvLowerMemoryOp::visitExtractElementInst(ExtractElementInst &extractElementInst) {
  auto src = extractElementInst.getOperand(0);
  if (src->getType()->isVectorTy() && isa<LoadInst>(src) && src->hasOneUse()) {
    // NOTE: Optimize loading vector component for local variable and memory block
    // Original pattern:
    // %1 = load <4 x float> addrspace(7)* %0
    // %2 = extractelement <4 x float> %1, i32 0
    // after transform:
    // %1 = getelementptr <4 x float>, <4 x float> addrspace(7)* %0, i32 0, i32 0
    // %2 = load float addrspace(7)* %1

    auto loadInst = cast<LoadInst>(src);
    auto loadPtr = loadInst->getOperand(0);
    auto addrSpace = loadPtr->getType()->getPointerAddressSpace();

    if (addrSpace == SPIRAS_Local || addrSpace == SPIRAS_Uniform) {
      Value *idxs[] = {ConstantInt::get(Type::getInt32Ty(*m_context), 0), extractElementInst.getOperand(1)};
      auto elementPtr = GetElementPtrInst::Create(src->getType(), loadPtr, idxs, "", &extractElementInst);
      auto elementTy = elementPtr->getResultElementType();
      auto newLoad = new LoadInst(elementTy, elementPtr, "", &extractElementInst);
      extractElementInst.replaceAllUsesWith(newLoad);

      m_removeInsts.insert(&extractElementInst);
      m_removeInsts.insert(loadInst);
    }
  }
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering the memory operations.
INITIALIZE_PASS(LegacySpirvLowerMemoryOp, DEBUG_TYPE, "Lower SPIR-V memory operations", false, false)
