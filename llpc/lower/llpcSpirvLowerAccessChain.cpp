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
 * @file  llpcSpirvLowerAccessChain.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerAccessChain.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerAccessChain.h"
#include "SPIRVInternal.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <stack>

#define DEBUG_TYPE "llpc-spirv-lower-access-chain"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Initializes static members.
char LegacySpirvLowerAccessChain::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for access chain
ModulePass *createLegacySpirvLowerAccessChain() {
  return new LegacySpirvLowerAccessChain();
}

// =====================================================================================================================
LegacySpirvLowerAccessChain::LegacySpirvLowerAccessChain() : ModulePass(ID) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool LegacySpirvLowerAccessChain::runOnModule(Module &module) {
  return Impl.runImpl(module);
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerAccessChain::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerAccessChain::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Access-Chain\n");

  SpirvLower::init(&module);

  // Invoke handling of "getelementptr" instruction
  visit(m_module);

  return true;
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtrInst : "Getelementptr" instruction
void SpirvLowerAccessChain::visitGetElementPtrInst(GetElementPtrInst &getElemPtrInst) {
  // NOTE: Here, we try to coalesce chained "getelementptr" instructions (created from multi-level access chain).
  // Because the metadata is always decorated on top-level pointer value (actually a global variable).
  const unsigned addrSpace = getElemPtrInst.getType()->getPointerAddressSpace();
  if (addrSpace == SPIRAS_Private || addrSpace == SPIRAS_Input || addrSpace == SPIRAS_Output)
    tryToCoalesceChain(&getElemPtrInst, addrSpace);
}

// =====================================================================================================================
// Tries to coalesce chained "getelementptr" instructions (created from multi-level access chain) from bottom to top
// in the type hierarchy.
//
// e.g.
//      %x = getelementptr %blockType, %blockType addrspace(N)* @block, i32 0, i32 L, i32 M
//      %y = getelementptr %fieldType, %fieldType addrspace(N)* %x, i32 0, i32 N
//
//      =>
//
//      %y = getelementptr %blockType, %blockType addrspace(N)* @block, i32 0, i32 L, i32 M, i32 N
//
//
// @param getElemPtr : "getelementptr" instruction in the bottom to do coalescing
// @param addrSpace : Address space of the pointer value of "getelementptr"
GetElementPtrInst *SpirvLowerAccessChain::tryToCoalesceChain(GetElementPtrInst *getElemPtr, unsigned addrSpace) {
  GetElementPtrInst *coalescedGetElemPtr = getElemPtr;

  std::stack<User *> chainedInsts;              // Order: from top to bottom
  std::stack<GetElementPtrInst *> removedInsts; // Order: from bottom to top

  // Collect chained "getelementptr" instructions and constants from bottom to top.
  auto ptrVal = cast<User>(getElemPtr);
  for (;;) {
    chainedInsts.push(ptrVal);
    auto next = ptrVal->getOperand(0);
    if (isa<GetElementPtrInst>(next)) {
      ptrVal = cast<User>(next);
      continue;
    }
    auto constant = dyn_cast<ConstantExpr>(next);
    if (!constant || constant->getOpcode() != Instruction::GetElementPtr)
      break;
    ptrVal = cast<User>(next);
  }

  // If there are more than one "getelementptr" instructions/constants, do coalescing
  if (chainedInsts.size() > 1) {
    std::vector<Value *> idxs;
    unsigned startOperand = 1;
    Value *blockPtr = nullptr;
    Type *coalescedType = nullptr;

    do {
      ptrVal = chainedInsts.top();
      chainedInsts.pop();

      for (unsigned i = startOperand; i != ptrVal->getNumOperands(); ++i)
        idxs.push_back(ptrVal->getOperand(i));
      // NOTE: For subsequent "getelementptr" instructions/constants, we skip the first two operands. The first
      // operand is the pointer value from which the element pointer is constructed. And the second one is always
      // 0 to dereference the pointer value.
      startOperand = 2;

      auto inst = dyn_cast<GetElementPtrInst>(ptrVal);

      if (!blockPtr && inst) {
        blockPtr = ptrVal->getOperand(0);
        coalescedType = inst->getSourceElementType();
      }

      if (inst)
        removedInsts.push(inst);
    } while (!chainedInsts.empty());

    // TODO: Remove this when LLPC will switch fully to opaque pointers.
    assert(cast<PointerType>(blockPtr->getType())->isOpaqueOrPointeeTypeMatches(coalescedType));

    // Create the coalesced "getelementptr" instruction (do combining)
    coalescedGetElemPtr = GetElementPtrInst::Create(coalescedType, blockPtr, idxs, "", getElemPtr);
    getElemPtr->replaceAllUsesWith(coalescedGetElemPtr);

    // Remove dead "getelementptr" instructions where possible.
    while (!removedInsts.empty()) {
      GetElementPtrInst *inst = removedInsts.top();
      if (inst->user_empty()) {
        if (inst == getElemPtr) {
          // We cannot remove the current instruction that InstWalker is on. Just stop it using its
          // pointer operand, and it will be DCEd later.
          auto &operand = inst->getOperandUse(0);
          operand = UndefValue::get(operand->getType());
        } else
          inst->eraseFromParent();
      }
      removedInsts.pop();
    }
  }

  return coalescedGetElemPtr;
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering operations for access chain.
INITIALIZE_PASS(LegacySpirvLowerAccessChain, DEBUG_TYPE, "Lower SPIR-V access chain", false, false)
