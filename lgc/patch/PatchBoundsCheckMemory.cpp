/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchBoundsCheckMemory.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchBoundsCheckMemory.
 *
 * This pass adds bounds checks to all stack/scratch accesses with dynamic indices. The pass looks at every
 * getelementptr instruction, checking which of the used indices are not constants. For non-constant indices, it checks
 * if they index into either a FixedVectorType or an ArrayType and gets their element count, which is the upper bound of
 * the index.
 *
 * Example:
 * We have a getelementptr, followed by a load.
 *
 * %elemPtr = getelementptr [16 x float], [16 x float] addrspace(5)* %array, i32 0, i32 %index
 * %value = load float, float addrspace(5)* %elemPtr, align 4
 *
 * Behind the getelementptr, we will compute the condition if all dynamic indices are in-bounds:
 *
 * %elemPtr = getelementptr [16 x float], [16 x float] addrspace(5)* %array, i32 0, i32 %index
 * %inBounds = icmp ult i32 %index, 16
 *
 * %value = load float, float addrspace(5)* %elemPtr, align 4
 *
 * We look at all users of the getelementptr and guard loads and stores with the in-bounds condition. Stores are skipped
 * if out-of-bounds, loads will return zero.
 *
 * %elemPtr = getelementptr [16 x float], [16 x float] addrspace(5)* %array, i32 0, i32 %index
 * %inBounds = icmp ult i32 %index, 16
 *
 * br i1 %inBounds, label %inBoundsBB, label %continueBB
 *
 * inBoundsBB:
 * %loadValue = load float, float addrspace(5)* %elemPtr, align 4
 * br label %continueBB
 *
 * continueBB:
 * %value = phi float [ %loadValue, %inBoundsBB ], [ 0.000000e+00, %.entry ]
 *
 ***********************************************************************************************************************
 */
#include "PatchBoundsCheckMemory.h"
#include "lgc/state/IntrinsDefs.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "lgc-bounds-check-memory"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchBoundsCheckMemory::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of bounds check memory operations.
ModulePass *createPatchBoundsCheckMemory() {
  return new PatchBoundsCheckMemory();
}

static Value *getZeroConstant(Type *ty) {
  if (ty->isIntegerTy()) {
    return ConstantInt::get(ty, 0);
  }
  if (ty->isFloatingPointTy()) {
    return ConstantFP::get(ty, 0.0);
  }
  return ConstantAggregateZero::get(ty);
}

// =====================================================================================================================
PatchBoundsCheckMemory::PatchBoundsCheckMemory() : Patch(ID) {
}

// =====================================================================================================================
// Executes this pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchBoundsCheckMemory::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass llpc-bounds-check-memory\n");

  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);

  if (!m_pipelineState->getOptions().enableScratchBoundsCheck)
    return false;

  m_context = &module.getContext();
  m_builder = std::make_unique<IRBuilder<>>(*m_context);

  visit(module);

  bool changed = false;
  for (auto &inst : m_getElementPtrInsts)
    changed |= addBoundsCheck(inst);
  m_getElementPtrInsts.clear();

  return changed;
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtr : "GetElementPtr" instruction
void PatchBoundsCheckMemory::visitGetElementPtrInst(GetElementPtrInst &getElemPtr) {
  auto pointerTy = getElemPtr.getPointerOperand()->getType();
  if (pointerTy->getPointerAddressSpace() != ADDR_SPACE_PRIVATE)
    return;

  // Search for dynamic indices in the instruction and collect them in a GetElemPtrInfo struct
  llvm::SmallVector<std::pair<Value *, uint64_t>, 1> dynIndices;
  std::vector<Value *> indices;
  for (const auto &index : getElemPtr.indices()) {
    if (!isa<Constant>(index)) {
      auto indexedTy = GetElementPtrInst::getIndexedType(pointerTy->getPointerElementType(), indices);
      // Save the index value its the upper bound
      if (auto vectorTy = dyn_cast<FixedVectorType>(indexedTy)) {
        dynIndices.push_back({index, vectorTy->getNumElements()});
      } else if (auto arrayTy = dyn_cast<ArrayType>(indexedTy)) {
        dynIndices.push_back({index, arrayTy->getNumElements()});
      } else {
        llvm_unreachable("Unsupported indexed type for bounds checking");
      }
    }
    indices.push_back(index);
  }
  if (dynIndices.empty())
    return;
  m_getElementPtrInsts.push_back({&getElemPtr, dynIndices});
}

// =====================================================================================================================
// Checks whether each dynamic index in the specified "getelementptr" instruction is lower than its allowed upper bound.
//
// @param info : "GetElementPtr" instruction and its dynamic indices
// @returns : True if the code was changed or false if unmodified
bool PatchBoundsCheckMemory::addBoundsCheck(GetElemPtrInfo &info) {
  auto &getElemPtr = *info.getElemPtr;

  // Copy users
  std::vector<Instruction *> users;
  for (auto user : getElemPtr.users()) {
    Instruction *inst = dyn_cast<Instruction>(user);
    if (!inst)
      continue;
    // Look through bitcasts
    if (isa<BitCastInst>(user)) {
      for (auto user : inst->users()) {
        if (Instruction *inst = dyn_cast<Instruction>(user))
          users.push_back(inst);
      }
    } else {
      users.push_back(inst);
    }
  }

  if (users.empty())
    return false;

  // Insert after getElemPtr
  m_builder->SetInsertPoint(getElemPtr.getParent(), ++getElemPtr.getIterator());

  // Get condition for in-bounds pointer
  Value *inBounds = nullptr;
  // Iterate through all dynamic indices and check that each one is in bounds
  for (auto indexBoundPair : info.dynIndices) {
    auto indexTy = cast<IntegerType>(indexBoundPair.first->getType());
    auto curInBounds = m_builder->CreateCmp(CmpInst::ICMP_ULT, indexBoundPair.first,
                                            m_builder->getIntN(indexTy->getScalarSizeInBits(), indexBoundPair.second));
    inBounds = inBounds ? m_builder->CreateAnd(inBounds, curInBounds) : curInBounds;
  }

  // Only execute loads and stores if in-bounds
  for (auto user : users) {
    BasicBlock *instBB = user->getParent();
    Instruction *inBoundsTerminator = SplitBlockAndInsertIfThen(inBounds, user, false);
    user->moveBefore(inBoundsTerminator);
    BasicBlock *inBoundsBB = inBoundsTerminator->getParent();
    BasicBlock *continueBB = cast<BranchInst>(inBoundsTerminator)->getSuccessor(0);

    if (auto loadInst = dyn_cast<LoadInst>(user)) {
      // Load returns zero if out-of-bounds
      auto loadTy = loadInst->getType();
      m_builder->SetInsertPoint(&continueBB->front());
      auto *phi = m_builder->CreatePHI(loadTy, 2);
      loadInst->replaceAllUsesWith(phi);
      phi->addIncoming(loadInst, inBoundsBB);
      phi->addIncoming(getZeroConstant(loadTy), instBB);
    } else if (!isa<StoreInst>(user)) {
      llvm_unreachable("Expected a load or store instruction");
    }
  }
  return true;
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of bounds check memory operations.
INITIALIZE_PASS(PatchBoundsCheckMemory, DEBUG_TYPE, "Patch LLVM for memory operation bounds checks", false, false)
