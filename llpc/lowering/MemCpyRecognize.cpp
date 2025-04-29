/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MemCpyRecognize.cpp
 * @brief LLPC source file: contains implementation of class Llpc::MemCpyRecognize.
 ***********************************************************************************************************************
 */
#include "MemCpyRecognize.h"
#include "compilerutils/CompilerUtils.h"
#include "lgc/LgcDialect.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/AMDGPUAddrSpace.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <optional>

#define DEBUG_TYPE "memcpy-recognize"

using namespace llvm;
using namespace Llpc;

namespace Llpc {

// We only merge load/store pairs into memcpy if the copied bytes greater than this value.
const unsigned minMergeableCopyBytes = 16;

// The pair of the load and store instruction can be possibly merged into memcpy.
struct LoadStorePair {
  llvm::LoadInst *load;
  llvm::StoreInst *store;
  int64_t srcOffset; // The offset against the base of load's pointer.
};

SmallVector<MemTransferInst *> mergeLoadStorePairs(SmallVector<LoadStorePair> &candidates) {
  if (candidates.size() <= 1)
    return {};

  llvm::sort(candidates,
             [](const LoadStorePair &lhs, const LoadStorePair &rhs) { return lhs.srcOffset < rhs.srcOffset; });

  SmallVector<MemTransferInst *> insertedCopies;
  SmallVector<Instruction *> toBeMerged;

  LoadStorePair *leader = &candidates.front();
  // They need to be put in the right order so that store will be finally erased before the load.
  toBeMerged.append({leader->store, leader->load});

  auto eraseDeadInstrs = [&]() {
    for (auto *dead : toBeMerged) {
      auto *ptr = getLoadStorePointerOperand(dead);
      dead->eraseFromParent();
      // Erase trivially dead gep instruction (not a constant expression).
      if (ptr->use_empty() && isa<Instruction>(ptr))
        cast<Instruction>(ptr)->eraseFromParent();
    }
  };

  const DataLayout &dl = leader->load->getDataLayout();
  IRBuilder<> builder(leader->load->getContext());

  unsigned mergedBytes = dl.getTypeStoreSize(leader->load->getType());
  // Keep record of the offset to detect whether the intervals have holes or overlap.
  int64_t srcOffsetBegin = leader->srcOffset;

  // We only combine into memcpy if there are at least two pairs of load/store (note we put both load and store into the
  // toBeMerged array) and the bytes to be merged is bigger enough.
  auto isProfitableToMerge = [&]() -> bool { return toBeMerged.size() >= 4 && mergedBytes > minMergeableCopyBytes; };

  for (auto &cand : llvm::drop_begin(candidates)) {
    unsigned candBytes = dl.getTypeSizeInBits(cand.load->getType()) / 8;
    assert(dl.getTypeSizeInBits(cand.load->getType()) % 8 == 0);
    if (srcOffsetBegin + mergedBytes == cand.srcOffset) {
      // Move on as the intervals are contiguous.
      mergedBytes += candBytes;
      toBeMerged.append({cand.store, cand.load});
    } else {
      if (srcOffsetBegin + mergedBytes < cand.srcOffset && isProfitableToMerge()) {
        // It seems not worth to transform small load/store pairs into memcpy.
        builder.SetInsertPoint(leader->store);
        auto *copy = builder.CreateMemCpy(leader->store->getPointerOperand(), leader->store->getAlign(),
                                          leader->load->getPointerOperand(), leader->load->getAlign(), mergedBytes);

        insertedCopies.push_back(cast<MemTransferInst>(copy));
        eraseDeadInstrs();
      }

      toBeMerged.clear();

      leader = &cand;
      toBeMerged.append({cand.store, cand.load});
      mergedBytes = candBytes;
      srcOffsetBegin = cand.srcOffset;
    }
  }
  // We have visited all the candidates, see if we have pending memcpy to be formed.
  if (isProfitableToMerge()) {
    builder.SetInsertPoint(leader->store);
    auto *copy = builder.CreateMemCpy(leader->store->getPointerOperand(), leader->store->getAlign(),
                                      leader->load->getPointerOperand(), leader->load->getAlign(), mergedBytes);
    insertedCopies.push_back(cast<MemTransferInst>(copy));
    eraseDeadInstrs();
  }

  return insertedCopies;
}

struct MergeState {
  llvm::Value *storePtrBase = nullptr;
  llvm::Value *loadPtrBase = nullptr;
  llvm::SmallVector<LoadStorePair> candidates;
  llvm::DenseMap<llvm::StoreInst *, int64_t> pendingStores;

  void setBasePointers(llvm::Value *loadPtr, llvm::Value *storePtr) {
    loadPtrBase = loadPtr;
    storePtrBase = storePtr;
  }

  // Merge the candidate load/store pairs and reset the state.
  SmallVector<MemTransferInst *> flush() {
    auto insertedCopies = mergeLoadStorePairs(candidates);
    loadPtrBase = nullptr;
    storePtrBase = nullptr;
    candidates.clear();
    pendingStores.clear();
    return insertedCopies;
  }
};

bool addrspacesMayAlias(unsigned addrspaceA, unsigned addrspaceB) {
  // Flat address spaces may alias with any other.
  if (!addrspaceA || !addrspaceB)
    return true;

  if (addrspaceB < addrspaceA)
    std::swap(addrspaceA, addrspaceB);

  // Unknown address spaces may alias.
  if (addrspaceA > AMDGPUAS::BUFFER_STRIDED_POINTER || addrspaceB > AMDGPUAS::BUFFER_STRIDED_POINTER)
    return true;

  if (addrspaceA == AMDGPUAS::GLOBAL_ADDRESS &&
      (addrspaceB == AMDGPUAS::BUFFER_FAT_POINTER || addrspaceB == AMDGPUAS::BUFFER_STRIDED_POINTER))
    return true;

  if (addrspaceA == AMDGPUAS::BUFFER_FAT_POINTER && addrspaceB == AMDGPUAS::BUFFER_STRIDED_POINTER)
    return true;

  return addrspaceA == addrspaceB;
}

// Check whether the load instruction is a mergeable candidate, and return the matched store optionally.
std::optional<StoreInst *> isMergeCandidate(const MergeState &mergeState, LoadInst *load) {
  if (!load->hasOneUse() || !load->isSimple())
    return std::nullopt;

  User *storeCand = load->getUniqueUndroppableUser();
  if (!isa<StoreInst>(storeCand))
    return std::nullopt;

  StoreInst *store = cast<StoreInst>(storeCand);
  // We can only combine locally.
  if (store->getParent() != load->getParent())
    return std::nullopt;

  if (!store->isSimple())
    return std::nullopt;

  unsigned srcAddrSpace = load->getPointerAddressSpace();
  unsigned dstAddrSpace = store->getPointerAddressSpace();

  // This is a cheap check that helps make sure that the destination memory never alias with the source memory.
  if (addrspacesMayAlias(srcAddrSpace, dstAddrSpace))
    return std::nullopt;

  // Forming memcpy is proved to be more beneficial if one of the address spaces is private memory.
  if (srcAddrSpace != AMDGPUAS::PRIVATE_ADDRESS && dstAddrSpace != AMDGPUAS::PRIVATE_ADDRESS)
    return std::nullopt;

  return store;
}

SmallVector<MemTransferInst *> processLoad(MergeState &mergeState, llvm::LoadInst *load) {
  SmallVector<MemTransferInst *> insertedCopies;

  auto matchedStore = isMergeCandidate(mergeState, load);
  if (!matchedStore.has_value()) {
    insertedCopies = mergeState.flush();
    return insertedCopies;
  }

  StoreInst *store = matchedStore.value();
  Value *loadPtr = load->getPointerOperand();
  if (mergeState.loadPtrBase) {
    // Can be merged with existing load (have constant offset against found base)?
    const DataLayout &dl = load->getDataLayout();
    auto srcOffset = loadPtr->getPointerOffsetFrom(mergeState.loadPtrBase, dl);
    auto dstOffset = store->getPointerOperand()->getPointerOffsetFrom(mergeState.storePtrBase, dl);
    // If pointers are compatible with base pointers, return early.
    if (srcOffset.has_value() && srcOffset == dstOffset) {
      mergeState.pendingStores.insert({store, srcOffset.value()});
      return insertedCopies;
    }

    // Merge any existing candidates if the new load/store pair is not mergeable with them.
    insertedCopies = mergeState.flush();
  }

  mergeState.setBasePointers(loadPtr, store->getPointerOperand());
  mergeState.pendingStores.insert({store, 0});
  return insertedCopies;
}

SmallVector<MemTransferInst *> processStore(MergeState &mergeState, StoreInst *store) {
  SmallVector<MemTransferInst *> insertedCopies;

  auto storeIt = mergeState.pendingStores.find(store);
  if (storeIt != mergeState.pendingStores.end()) {
    // Ok, this is a pending store we cares, erase from the pendingStores and insert into candidate list.
    mergeState.pendingStores.erase(store);
    LoadInst *load = cast<LoadInst>(store->getValueOperand());
    mergeState.candidates.push_back(LoadStorePair{load, store, storeIt->second});
  } else {
    insertedCopies = mergeState.flush();
  }
  return insertedCopies;
}

// Check whether it is safe to replace the destination pointer with source pointer for the memcpy instruction.
bool isSafeToReplacePointer(MemTransferInst *copy, DominatorTree &dt) {
  // Check if the source is constant.
  bool isSrcConstant = false;
  auto *basePtr = copy->getSource();
  // We always attach the invariant.start call to the base pointer, so we need to trace back here.
  while (isa<GetElementPtrInst, lgc::BufferIndexOp>(basePtr)) {
    if (isa<GetElementPtrInst>(basePtr)) {
      basePtr = cast<GetElementPtrInst>(basePtr)->getPointerOperand();
    } else {
      basePtr = cast<lgc::BufferIndexOp>(basePtr)->getPtr();
    }
  }

  for (auto *inst : basePtr->users()) {
    if (!match(inst, PatternMatch::m_Intrinsic<Intrinsic::invariant_start>()))
      continue;
    // The pointed memory is constant if there is no invariant_end
    if (inst->use_empty() && dt.dominates(inst, copy))
      isSrcConstant = true;
  }
  if (!isSrcConstant)
    return false;

  // Check the destination memory is not modified.
  SmallVector<Use *> worklist(make_pointer_range(copy->getDest()->uses()));
  while (!worklist.empty()) {
    Use *ptrUse = worklist.pop_back_val();
    Instruction *inst = cast<Instruction>(ptrUse->getUser());
    LLVM_DEBUG(dbgs() << "Visiting " << *inst << '\n');
    auto usesRange = make_pointer_range(inst->uses());
    switch (inst->getOpcode()) {
    default:
      // Giveup on unknown instruction
      return false;
    case Instruction::Call: {
      if (inst->isLifetimeStartOrEnd())
        break;
      // We found the target memcpy, it is ok.
      if (copy == inst)
        continue;

      return false;
    }
    case Instruction::Load: {
      LoadInst *load = cast<LoadInst>(inst);
      if (!load->isSimple())
        return false;
      // no further visiting the users of the loaded value
      continue;
    }
    case Instruction::Store:
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
      return false;
    case Instruction::GetElementPtr:
      break;
    case Instruction::Select:
    case Instruction::PHI:
      // TODO: The target case of the pointer replacement is for pointers in different address spaces, support phi and
      // select needs more careful work.
      return false;
    }

    worklist.append(usesRange.begin(), usesRange.end());
  }
  return true;
}

SmallVector<MemTransferInst *> combineLoadStoreIntoMemcpy(Function &f) {
  SmallVector<MemTransferInst *> copies;
  for (BasicBlock &bb : f) {
    MergeState mergeState;

    // Do block level scan to check successive load/store.
    for (Instruction &inst : bb) {
      if (isa<LoadInst>(inst)) {
        copies.append(processLoad(mergeState, cast<LoadInst>(&inst)));
      } else if (isa<StoreInst>(inst)) {
        copies.append(processStore(mergeState, cast<StoreInst>(&inst)));
      } else {
        // Stop the search if there is any other memory access. We can enhance this, but it needs complex memory
        // analysis.
        if (inst.mayReadFromMemory() || inst.mayWriteToMemory()) {
          // We can still merge existing candidates.
          copies.append(mergeState.flush());
        }
      }
    }
    // We have gathered all the candidates that can be transformed into memcpy.
    copies.append(mergeLoadStorePairs(mergeState.candidates));
  }

  return copies;
}

// =====================================================================================================================
// Executes this lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses MemCpyRecognize::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass memcpy recognizer\n");

  FunctionAnalysisManager &fam = analysisManager.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
  bool changed = false;
  for (Function &f : module.functions()) {
    if (f.isDeclaration())
      continue;
    auto insertedCopies = combineLoadStoreIntoMemcpy(f);

    if (insertedCopies.empty())
      continue;

    changed = true;
    DominatorTree &dt = fam.getResult<DominatorTreeAnalysis>(f);
    for (MemTransferInst *copy : insertedCopies) {
      if (isa<AllocaInst>(copy->getDest()) && isSafeToReplacePointer(copy, dt)) {
        auto *src = copy->getSource();
        auto *dst = copy->getDest();
        // Erase the copy so the later replaceAllPointerUses don't need to handle it.
        copy->eraseFromParent();

        SmallVector<Instruction *> toBeRemoved;
        CompilerUtils::replaceAllPointerUses(dst, src, toBeRemoved);
        for (auto *dead : toBeRemoved) {
          dead->dropAllReferences();
          dead->eraseFromParent();
        }
      }
    }
  }

  return changed ? PreservedAnalyses::allInSet<CFGAnalyses>() : PreservedAnalyses::all();
}

} // namespace Llpc
