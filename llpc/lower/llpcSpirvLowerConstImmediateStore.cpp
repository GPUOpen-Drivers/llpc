/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerConstImmediateStore.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerConstImmediateStore.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerConstImmediateStore.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include <vector>

#define DEBUG_TYPE "llpc-spirv-lower-const-immediate-store"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerConstImmediateStore::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Const-Immediate-Store\n");

  SpirvLower::init(&module);

  // Process "alloca" instructions to see if they can be optimized to a read-only global
  // variable.
  bool changed = false;
  m_allocToGlobals.clear();
  for (auto &func : module.functions()) {
    if (!func.empty()) {
      if (processAllocaInsts(&func))
        changed |= true;
    }
  }

  return changed ? PreservedAnalyses::allInSet<CFGAnalyses>() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Processes "alloca" instructions at the beginning of the given non-empty function to see if they
// can be optimized to a read-only global variable.
//
// @param func : Function to process
bool SpirvLowerConstImmediateStore::processAllocaInsts(Function *func) {
  // NOTE: We only visit the entry block on the basis that SPIR-V translator puts all "alloca"
  // instructions there.
  bool changed = false;
  SmallVector<AllocaInst *> candidates;
  for (auto &inst : func->getEntryBlock()) {
    if (auto allocaInst = dyn_cast<AllocaInst>(&inst)) {
      if (allocaInst->getAllocatedType()->isAggregateType())
        candidates.push_back(allocaInst);
    }
  }
  for (auto *alloca : candidates) {
    if (tryProcessAlloca(alloca))
      changed |= true;
  }
  return changed;
}

// =====================================================================================================================
// For a single alloca, try to replace it by a constant global variable.
//
// @param allocaInst : The "alloca" instruction to process
// @return true if the alloca was replaced
bool SpirvLowerConstImmediateStore::tryProcessAlloca(AllocaInst *allocaInst) {
  // LLVM IR allocas can have an "arrayness" where multiple elements of the allocated type are allocated at once.
  // SPIR-V doesn't have this (because it only has OpVariable and not a "true" alloca), but let's guard against it
  // anyway just in case.
  if (allocaInst->isArrayAllocation())
    return false;

  auto *allocatedTy = allocaInst->getAllocatedType();
  auto *arrayTy = dyn_cast<ArrayType>(allocatedTy);

  StoreInst *aggregateStore = nullptr;
  DenseMap<uint64_t, StoreInst *> elementStores;

  SmallVector<Instruction *> toErase;
  SmallVector<GetElementPtrInst *> geps;

  // Step 1: Determine if the alloca can be converted and find the relevant store(s)
  SmallVector<std::pair<Value *, std::optional<uint64_t>>> pointers;
  pointers.emplace_back(allocaInst, 0);
  do {
    auto [pointer, index] = pointers.pop_back_val();
    for (Use &use : pointer->uses()) {
      auto user = cast<Instruction>(use.getUser());
      if (auto storeInst = dyn_cast<StoreInst>(user)) {
        if (use.getOperandNo() != storeInst->getPointerOperandIndex() || !index.has_value()) {
          // Pointer escapes by being stored, or we store to a dynamically indexed (or otherwise complex) pointer.
          return false;
        }

        Value *storeValue = storeInst->getValueOperand();
        if (!isa<Constant>(storeValue))
          return false;

        // We already have a store of the entire variable. Multiple stores means it's not an overall constant.
        if (aggregateStore)
          return false;

        if (storeValue->getType() == allocatedTy) {
          if (index.value() != 0) {
            // This store is out-of-bounds, which makes it UB if it is ever executed (it might be in control flow that
            // is unreachable for some reason). Remember the store as to-be-erased and ignore it otherwise.
            toErase.push_back(storeInst);
            continue;
          }

          if (!elementStores.empty())
            return false;
          aggregateStore = storeInst;
          continue;
        }

        if (arrayTy && storeValue->getType() == arrayTy->getArrayElementType()) {
          if (index.value() >= arrayTy->getArrayNumElements()) {
            // This store is out-of-bounds, which makes it UB if it is ever executed (it might be in control flow that
            // is unreachable for some reason). Remember the store as to-be-erased and ignore it otherwise.
            toErase.push_back(storeInst);
            continue;
          }

          if (!elementStores.try_emplace(index.value(), storeInst).second)
            return false;
          continue;
        }

        return false;
      }

      if (auto gep = dyn_cast<GetElementPtrInst>(user)) {
        geps.push_back(gep);

        if (index.has_value() && arrayTy && gep->getSourceElementType() == allocatedTy &&
            gep->hasAllConstantIndices() && gep->getNumIndices() == 2 &&
            cast<ConstantInt>(gep->getOperand(1))->isNullValue()) {
          int64_t gepIdx = cast<ConstantInt>(gep->getOperand(2))->getSExtValue();
          if (gepIdx >= 0) {
            pointers.emplace_back(gep, index.value() + gepIdx);
            continue;
          }
        }

        pointers.emplace_back(gep, std::nullopt);
        continue;
      }

      if (isa<LoadInst>(user))
        continue;

      if (isAssumeLikeIntrinsic(user)) {
        toErase.push_back(user);
        continue;
      }

      // Pointer escapes by being used in some way other than "load/store/getelementptr".
      return false;
    }
  } while (!pointers.empty());

  // Step 2: Extract or build the initializer constant
  Constant *initializer = nullptr;

  if (aggregateStore) {
    initializer = cast<Constant>(aggregateStore->getValueOperand());
  } else if (!elementStores.empty()) {
    // Give up if the array is 4x larger than the number of element stores. This is a fairly arbitrary heuristic to
    // prevent a super-linear blow-up of the size of IR. (Imagine input IR that defines a giant array and writes to
    // only a single element of it.)
    if (arrayTy->getArrayNumElements() / 4 > elementStores.size())
      return false;

    std::vector<Constant *> elements;
    elements.resize(arrayTy->getArrayNumElements());

    for (uint64_t index = 0; index < elements.size(); ++index) {
      if (auto *store = elementStores.lookup(index))
        elements[index] = cast<Constant>(store->getValueOperand());
      else
        elements[index] = PoisonValue::get(arrayTy->getElementType());
    }

    initializer = ConstantArray::get(arrayTy, elements);
  } else {
    initializer = PoisonValue::get(allocatedTy);
  }

  // Step 3: Create the global variable and replace the alloca
  GlobalVariable *&global = m_allocToGlobals[initializer];
  if (!global) {
    global = new GlobalVariable(*m_module, allocatedTy,
                                true, // isConstant
                                GlobalValue::InternalLinkage, initializer, "", nullptr, GlobalValue::NotThreadLocal,
                                SPIRAS_Constant);
  }
  global->takeName(allocaInst);

  for (Use &use : llvm::make_early_inc_range(allocaInst->uses()))
    use.set(global);

  for (auto *gep : geps)
    gep->mutateType(global->getType());

  for (auto *inst : toErase)
    inst->eraseFromParent();
  if (aggregateStore)
    aggregateStore->eraseFromParent();
  for (auto [index, store] : elementStores) {
    if (store)
      store->eraseFromParent();
  }
  allocaInst->eraseFromParent();

  return true;
}

} // namespace Llpc
