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
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerConstImmediateStore::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Const-Immediate-Store\n");

  SpirvLower::init(&module);

  // Process "alloca" instructions to see if they can be optimized to a read-only global
  // variable.
  for (auto funcIt = module.begin(), funcItEnd = module.end(); funcIt != funcItEnd; ++funcIt) {
    if (auto func = dyn_cast<Function>(&*funcIt)) {
      if (!func->empty())
        processAllocaInsts(func);
    }
  }

  return true;
}

// =====================================================================================================================
// Processes "alloca" instructions at the beginning of the given non-empty function to see if they
// can be optimized to a read-only global variable.
//
// @param func : Function to process
void SpirvLowerConstImmediateStore::processAllocaInsts(Function *func) {
  // NOTE: We only visit the entry block on the basis that SPIR-V translator puts all "alloca"
  // instructions there.
  auto entryBlock = &func->front();
  for (auto instIt = entryBlock->begin(), instItEnd = entryBlock->end(); instIt != instItEnd; ++instIt) {
    auto inst = &*instIt;
    if (auto allocaInst = dyn_cast<AllocaInst>(inst)) {
      if (allocaInst->getAllocatedType()->isAggregateType()) {
        tryCombineStores(allocaInst);
        // Got an "alloca" instruction of aggregate type.
        auto storeInst = findSingleStore(allocaInst);
        if (storeInst && isa<Constant>(storeInst->getValueOperand())) {
          // Got an aggregate "alloca" with a single store to the whole type.
          // Do the optimization.
          convertAllocaToReadOnlyGlobal(storeInst);
        }
      }
    }
  }
}

// =====================================================================================================================
// Try to combine multiple store instructions into one.
//
// Finds multiple store instructions which stores constant values into the same allocated memory.
// If all found stores are filling all allocated fields in the memory, then multiple stores are replaced with
// single store. This single store later may be promoted to Global Variable (in `convertAllocaToReadOnlyGlobal`).
//
// This may happen when user is not initializing variables at compile time, but rather is
// doing it at runtime, but still using constant values.
//
// int a[10];
// main() {
//  a[0] = 1;
//  a[1] = 2;
//  a[2] = 3;
//  ...
// }
//
// @param allocaInst : The "alloca" instruction to process
void SpirvLowerConstImmediateStore::tryCombineStores(AllocaInst *allocaInst) {
  if (allocaInst->getAddressSpace() != SPIRAS_Private)
    return;

  auto *ArrayTy = dyn_cast<ArrayType>(allocaInst->getAllocatedType());
  if (!ArrayTy)
    return;

  // Handle one and two-dimensional arrays. [54 x float] or [50 x <2 x float>]
  auto *VecTy = dyn_cast<FixedVectorType>(ArrayTy->getElementType());
  if (VecTy && (!VectorType::isValidElementType(VecTy->getElementType()) || VecTy->getNumElements() == 0))
    return;

  if (!VecTy && (!VectorType::isValidElementType(ArrayTy->getElementType()) || ArrayTy->getNumElements() == 0))
    return;

  SmallVector<Use *, 20> Uses;
  for (Use &U : allocaInst->uses())
    Uses.push_back(&U);

  SmallVector<StoreInst *, 20> storeList;
  while (!Uses.empty()) {
    Use *U = Uses.pop_back_val();
    Instruction *Inst = cast<Instruction>(U->getUser());

    if (auto *storeInst = dyn_cast<StoreInst>(Inst)) {
      // Skip if any of the stores is not storing constant value or if store is atomic/volatile.
      if (!isa<Constant>(storeInst->getValueOperand()) ||
          (storeInst->getValueOperand()->getType() != ArrayTy->getElementType()) || (!storeInst->isSimple())) {
        return;
      }

      storeList.push_back(storeInst);
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
      if (GEP->hasAllConstantIndices() && GEP->getNumIndices() == 2) {
        // Find more store instructions in GEP uses.
        for (Use &GEPUse : Inst->uses()) {
          Uses.push_back(&GEPUse);
        }
      }
    } else if (!isa<LoadInst>(Inst) && !isAssumeLikeIntrinsic(Inst)) {
      return;
    }
  }

  // No stores or only one found. Nothing to do.
  if (storeList.size() < 2)
    return;

  BasicBlock *parentBB = storeList.back()->getParent();
  std::map<int, Constant *> idxConstMap;
  // Iterate through all store instructions correlated with Alloca and check if
  // we have enough data to create single store instruction which will initialize
  // whole Alloca memory.
  for (auto *storeInst : storeList) {
    // For simplicity all stores have to be in the same Basic Block.
    if (storeInst->getParent() != parentBB)
      return;

    Value *Ptr = storeInst->getPointerOperand()->stripPointerCasts();
    // by default idx is set to 0 since zero index GEPs may be removed due to
    // opaque pointers. All other accesses have to be made by GEPs.
    int idx = 0;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
      ConstantInt *GEPIdx = cast<ConstantInt>(GEP->getOperand(2));
      idx = GEPIdx->getZExtValue();
    }

    auto I = idxConstMap.find(idx);
    // Check if we are not trying to add the same idx. This may happen when the
    // same place in table is initialized twice or more.
    // a[0] = 10;
    // a[1] = 11;
    // a[1] = 12;
    // a[2] = 13;
    if (I != idxConstMap.end())
      return;
    idxConstMap[idx] = cast<Constant>(storeInst->getValueOperand());
  }

  // Found more or less stores then Alloca memory size.
  if (idxConstMap.size() != ArrayTy->getNumElements())
    return;

  SmallVector<Constant *, 20> NewInitVal;
  for (unsigned i = 0; i < idxConstMap.size(); ++i) {
    auto I = idxConstMap.find(i);
    assert(I != idxConstMap.end() && "Must have entry!");
    NewInitVal.push_back(I->second);
  }

  Constant *newInitArray = ConstantArray::get(ArrayTy, NewInitVal);

  IRBuilder<> Builder(storeList.back());
  Builder.CreateAlignedStore(newInitArray, allocaInst, allocaInst->getAlign());

  // Remove no longer needed store and GEP instructions.
  while (!storeList.empty()) {
    StoreInst *storeInst = storeList.pop_back_val();
    GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(storeInst->getPointerOperand());
    bool hasOneUser = GEP ? GEP->hasOneUser() : false;

    storeInst->eraseFromParent();
    if (hasOneUser)
      GEP->eraseFromParent();
  }
}

// =====================================================================================================================
// Finds the single "store" instruction storing to this pointer.
//
// Returns nullptr if no "store", multiple "store", or partial "store" instructions (store only part
// of the memory) are found.
//
// NOTE: This is conservative in that it returns nullptr if the pointer escapes by being used in anything
// other than "store" (as the pointer), "load" or "getelementptr" instruction.
//
// @param allocaInst : The "alloca" instruction to process
StoreInst *SpirvLowerConstImmediateStore::findSingleStore(AllocaInst *allocaInst) {
  std::vector<Instruction *> pointers;
  bool isOuterPointer = true;
  StoreInst *storeInstFound = nullptr;
  Instruction *pointer = allocaInst;
  while (true) {
    for (auto useIt = pointer->use_begin(), useItEnd = pointer->use_end(); useIt != useItEnd; ++useIt) {
      auto user = cast<Instruction>(useIt->getUser());
      if (auto storeInst = dyn_cast<StoreInst>(user)) {
        if (pointer == storeInst->getValueOperand() || storeInstFound || !isOuterPointer) {
          // Pointer escapes by being stored, or we have already found a "store"
          // instruction, or this is a partial "store" instruction.
          return nullptr;
        }
        storeInstFound = storeInst;
      } else if (auto getElemPtrInst = dyn_cast<GetElementPtrInst>(user))
        pointers.push_back(getElemPtrInst);
      else if (!isa<LoadInst>(user) && !isAssumeLikeIntrinsic(user)) {
        // Pointer escapes by being used in some way other than "load/store/getelementptr".
        return nullptr;
      }
    }

    if (pointers.empty())
      break;

    pointer = pointers.back();
    pointers.pop_back();
    isOuterPointer = false;
  }

  return storeInstFound;
}

// =====================================================================================================================
// Converts an "alloca" instruction with a single constant store into a read-only global variable.
//
// NOTE: This erases the "store" instruction (so it will not be lowered by a later lowering pass
// any more) but not the "alloca" or replaced "getelementptr" instruction (they will be removed
// later by DCE pass).
//
// @param storeInst : The single constant store into the "alloca"
void SpirvLowerConstImmediateStore::convertAllocaToReadOnlyGlobal(StoreInst *storeInst) {
  auto allocaInst = cast<AllocaInst>(storeInst->getPointerOperand());
  auto globalType = allocaInst->getAllocatedType();
  auto initVal = cast<Constant>(storeInst->getValueOperand());

  if (globalType != initVal->getType())
    return;

  auto global = new GlobalVariable(*m_module, globalType,
                                   true, // isConstant
                                   GlobalValue::InternalLinkage, initVal, "", nullptr, GlobalValue::NotThreadLocal,
                                   SPIRAS_Constant);
  global->takeName(allocaInst);
  // Change all uses of allocaInst to use global. We need to do it manually, as there is a change
  // of address space, and we also need to recreate "getelementptr"s.
  std::vector<std::pair<Instruction *, Value *>> allocaToGlobalMap;
  allocaToGlobalMap.push_back(std::pair<Instruction *, Value *>(allocaInst, global));
  do {
    auto allocaInst = allocaToGlobalMap.back().first;
    auto global = allocaToGlobalMap.back().second;
    allocaToGlobalMap.pop_back();
    while (!allocaInst->use_empty()) {
      auto useIt = allocaInst->use_begin();
      if (auto origGetElemPtrInst = dyn_cast<GetElementPtrInst>(useIt->getUser())) {
        // This use is a "getelementptr" instruction. Create a replacement one with the new address space.
        SmallVector<Value *, 4> indices;
        for (auto idxIt = origGetElemPtrInst->idx_begin(), idxItEnd = origGetElemPtrInst->idx_end(); idxIt != idxItEnd;
             ++idxIt)
          indices.push_back(*idxIt);
        auto newGetElemPtrInst = GetElementPtrInst::Create(globalType, global, indices, "", origGetElemPtrInst);
        newGetElemPtrInst->takeName(origGetElemPtrInst);
        newGetElemPtrInst->setIsInBounds(origGetElemPtrInst->isInBounds());
        newGetElemPtrInst->copyMetadata(*origGetElemPtrInst);
        // Remember that we need to replace the uses of the original "getelementptr" with the new one.
        allocaToGlobalMap.push_back(std::pair<Instruction *, Value *>(origGetElemPtrInst, newGetElemPtrInst));
        // Remove the use from the original "getelementptr".
        *useIt = PoisonValue::get(allocaInst->getType());
        continue;
      }

      if (auto *intrinsic = dyn_cast<IntrinsicInst>(useIt->getUser())) {
        switch (intrinsic->getIntrinsicID()) {
        case Intrinsic::lifetime_start:
        case Intrinsic::lifetime_end:
          // Lifetime markers are only useful for allocas, not for globals, and if we did not erase them we would have
          // to change their name mangling because of the change of address space.
          intrinsic->eraseFromParent();
          continue;
        }
      }

      *useIt = global;
    }
    // Visit next map pair.
  } while (!allocaToGlobalMap.empty());
  storeInst->eraseFromParent();
}

} // namespace Llpc
