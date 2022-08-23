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

  // Remove those instructions that are replaced by this lower pass
  for (auto inst : m_preRemoveInsts) {
    assert(inst->user_empty());
    inst->dropAllReferences();
    inst->eraseFromParent();
  }
  m_preRemoveInsts.clear();

  for (unsigned i = 0; i < m_storeExpandInfo.size(); i++) {
    StoreExpandInfo *expandInfo = &m_storeExpandInfo[i];
    expandStoreInst(expandInfo->storeInst, expandInfo->getElemPtrs, expandInfo->dynIndex);
  }
  m_storeExpandInfo.clear();

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

      m_preRemoveInsts.insert(&extractElementInst);
      m_removeInsts.insert(loadInst);
    }
  }
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtrInst : "GetElementPtr" instruction
void SpirvLowerMemoryOp::visitGetElementPtrInst(GetElementPtrInst &getElemPtrInst) {
  unsigned operandIndex = InvalidValue;
  unsigned dynIndexBound = 0;

  if (needExpandDynamicIndex(&getElemPtrInst, &operandIndex, &dynIndexBound)) {
    SmallVector<GetElementPtrInst *, 1> getElemPtrs;
    auto dynIndex = getElemPtrInst.getOperand(operandIndex);
    bool isType64 = (dynIndex->getType()->getPrimitiveSizeInBits() == 64);

    // Create "getelementptr" instructions with constant indices
    for (unsigned i = 0; i < dynIndexBound; ++i) {
      auto getElemPtr = cast<GetElementPtrInst>(getElemPtrInst.clone());
      auto constIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_context), i)
                                 : ConstantInt::get(Type::getInt32Ty(*m_context), i);
      getElemPtr->setOperand(operandIndex, constIndex);
      getElemPtrs.push_back(getElemPtr);
      getElemPtr->insertBefore(&getElemPtrInst);
    }

    // Copy users, ExpandStoreInst/ExpandLoadInst change getElemPtrInst's user
    std::vector<User *> users;
    for (auto user : getElemPtrInst.users())
      users.push_back(user);

    // Replace the original "getelementptr" instructions with a group of newly-created "getelementptr" instructions
    for (auto user : users) {
      auto loadInst = dyn_cast<LoadInst>(user);
      auto storeInst = dyn_cast<StoreInst>(user);

      if (loadInst)
        expandLoadInst(loadInst, getElemPtrs, dynIndex);
      else if (storeInst)
        recordStoreExpandInfo(storeInst, getElemPtrs, dynIndex);
      else
        llvm_unreachable("Should never be called!");
    }

    // Collect replaced instructions that will be removed
    m_removeInsts.insert(&getElemPtrInst);
  }
}

// =====================================================================================================================
// Checks whether the specified "getelementptr" instruction contains dynamic index and is therefore able to be expanded.
//
// @param getElemPtr : "GetElementPtr" instruction
// @param [out] operandIndexOut : Index of the operand that represents a dynamic index
// @param [out] dynIndexBound : Upper bound of dynamic index
bool SpirvLowerMemoryOp::needExpandDynamicIndex(GetElementPtrInst *getElemPtr, unsigned *operandIndexOut,
                                                unsigned *dynIndexBound) const {
  static const unsigned MaxDynIndexBound = 8;

  std::vector<Value *> idxs;
  unsigned operandIndex = InvalidValue;
  bool needExpand = false;
  bool allowExpand = true;
  auto ptrVal = getElemPtr->getPointerOperand();

  // NOTE: We only handle local variables.
  if (ptrVal->getType()->getPointerAddressSpace() != SPIRAS_Private)
    allowExpand = false;

  for (unsigned i = 1, operandCount = getElemPtr->getNumOperands(); allowExpand && i < operandCount; ++i) {
    auto index = getElemPtr->getOperand(i);
    if (!isa<Constant>(index)) {
      // Find the operand that represents a dynamic index
      if (operandIndex == InvalidValue) {
        // This is the first operand that represents a dynamic index
        operandIndex = i;
        needExpand = true;

        auto indexedTy = getElemPtr->getIndexedType(getElemPtr->getSourceElementType(), idxs);
        if (indexedTy) {
          // Check the upper bound of dynamic index
          if (isa<ArrayType>(indexedTy)) {
            auto arrayTy = dyn_cast<ArrayType>(indexedTy);
            if (arrayTy->getNumElements() > MaxDynIndexBound) {
              // Skip expand if array size greater than threshold
              allowExpand = false;
            } else
              *dynIndexBound = arrayTy->getNumElements();
          } else if (isa<VectorType>(indexedTy)) {
            // Always expand for vector
            auto vectorTy = dyn_cast<FixedVectorType>(indexedTy);
            *dynIndexBound = vectorTy->getNumElements();
          } else {
            llvm_unreachable("Should never be called!");
            allowExpand = false;
          }
        } else {
          llvm_unreachable("Should never be called!");
          allowExpand = false;
        }
      } else {
        // Skip expand if there are more than one dynamic indices
        allowExpand = false;
      }
    } else
      idxs.push_back(index);
  }

  if (needExpand && allowExpand) {
    // Skip expand if the user of "getelementptr" is neither "load" nor "store"
    for (auto user : getElemPtr->users()) {
      if (!isa<LoadInst>(user) && !isa<StoreInst>(user)) {
        allowExpand = false;
        break;
      }
    }
  }

  *operandIndexOut = operandIndex;
  return needExpand && allowExpand;
}

// =====================================================================================================================
// Expands "load" instruction with constant-index "getelementptr" instructions.
//
// @param loadInst : "Load" instruction
// @param getElemPtrs : A group of "getelementptr" with constant indices
// @param dynIndex : Dynamic index
void SpirvLowerMemoryOp::expandLoadInst(LoadInst *loadInst, ArrayRef<GetElementPtrInst *> getElemPtrs,
                                        Value *dynIndex) {
  // Expand is something like this:
  //
  //   firstValue  = load getElemPtrs[0]
  //
  //   secondValue = load getElemPtrs[1]
  //   firstValue  = (dynIndex == 1) ?  secondValue : firstValue
  //
  //   secondValue = load getElemPtrs[2]
  //   firstValue  = (dynIndex == 2) ?  secondValue : firstValue
  //   ...
  //   secondValue = load getElemPtrs[upperBound -2]
  //   firstValue  = (dynIndex == upperBound - 2) ? secondValue : firstValue
  //   secondValue = load getElemPtrs[upperBound - 1]
  //   firstValue  = (dynIndex == upperBound - 1) ? secondValue : firstValue
  //
  //   loadValue   = firstValue

  bool isType64 = (dynIndex->getType()->getPrimitiveSizeInBits() == 64);
  auto loadTy = loadInst->getType();
  Instruction *firstLoadValue = new LoadInst(loadTy, getElemPtrs[0], "", false, loadInst);

  for (unsigned i = 1, getElemPtrCount = getElemPtrs.size(); i < getElemPtrCount; ++i) {
    auto constIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_context), i)
                               : ConstantInt::get(Type::getInt32Ty(*m_context), i);

    auto secondLoadValue = new LoadInst(loadTy, getElemPtrs[i], "", false, loadInst);
    auto cond = new ICmpInst(loadInst, ICmpInst::ICMP_EQ, dynIndex, constIndex);
    firstLoadValue = SelectInst::Create(cond, secondLoadValue, firstLoadValue, "", loadInst);
  }

  loadInst->replaceAllUsesWith(firstLoadValue);
  m_preRemoveInsts.insert(loadInst);
}

// =====================================================================================================================
// Record store expansion info after visit, because splitBasicBlock will disturb the visit.
//
// @param storeInst : "Store" instruction
// @param getElemPtrs : A group of "getelementptr" with constant indices
// @param dynIndex : Dynamic index
void SpirvLowerMemoryOp::recordStoreExpandInfo(StoreInst *storeInst, ArrayRef<GetElementPtrInst *> getElemPtrs,
                                               Value *dynIndex) {
  StoreExpandInfo expandInfo = {};
  expandInfo.storeInst = storeInst;
  expandInfo.dynIndex = dynIndex;

  for (unsigned i = 0; i < getElemPtrs.size(); ++i)
    expandInfo.getElemPtrs.push_back(getElemPtrs[i]);

  m_storeExpandInfo.push_back(expandInfo);
}

// =====================================================================================================================
// Expands "store" instruction with fixed indexed "getelementptr" instructions.
//
// @param storeInst : "Store" instruction
// @param getElemPtrs : A group of "getelementptr" with constant indices
// @param dynIndex : Dynamic index
void SpirvLowerMemoryOp::expandStoreInst(StoreInst *storeInst, ArrayRef<GetElementPtrInst *> getElemPtrs,
                                         Value *dynIndex) {
  const bool robustBufferAccess = m_context->getPipelineContext()->getPipelineOptions()->robustBufferAccess;
  const unsigned getElemPtrCount = getElemPtrs.size();
  bool isType64 = (dynIndex->getType()->getPrimitiveSizeInBits() == 64);
  Value *firstStoreDest = getElemPtrs[0];

  if (robustBufferAccess) {
    // The .entry will be split into three blocks, .entry, .store and .endStore
    //
    // Expand is something like this:
    //
    // .entry
    //   ...
    //   if (dynIndex < upperBound) goto .store
    //   else goto .endStore
    //
    // .store
    //   firstPtr  = getElemPtrs[0]
    //
    //   secondPtr = getElemPtrs[1]
    //   firstPtr  = (dynIndex == 1) ? secondPtr : firstPtr
    //
    //   secondPtr = getElemPtrs[2]
    //   firstPtr  = (dynIndex == 2) ? secondPtr : firstPtr
    //   ...
    //   secondPtr = getElemPtrs[upperBound - 2]
    //   firstPtr  = (dynIndex == upperBound - 2) ? secondPtr : firstPtr
    //
    //   secondPtr = getElemPtrs[upperBound - 1]
    //   firstPtr  = (dynIndex == upperBound - 1) ? secondPtr : firstPtr
    //
    //   store storeValue, firstPtr
    //   goto .endStore
    //
    // .endStore
    //   ...
    //   ret

    auto checkStoreBlock = storeInst->getParent();
    auto storeBlock = checkStoreBlock->splitBasicBlock(storeInst);
    auto endStoreBlock = storeBlock->splitBasicBlock(storeInst);

    Instruction *checkStoreInsertPos = &checkStoreBlock->getInstList().back();
    Instruction *storeInsertPos = &storeBlock->getInstList().front();

    auto getElemPtrCountVal = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_context), getElemPtrCount)
                                       : ConstantInt::get(Type::getInt32Ty(*m_context), getElemPtrCount);

    auto doStore = new ICmpInst(checkStoreInsertPos, ICmpInst::ICMP_ULT, dynIndex, getElemPtrCountVal);
    BranchInst::Create(storeBlock, endStoreBlock, doStore, checkStoreInsertPos);

    for (unsigned i = 1; i < getElemPtrCount; ++i) {
      auto secondStoreDest = getElemPtrs[i];
      auto constIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_context), i)
                                 : ConstantInt::get(Type::getInt32Ty(*m_context), i);
      auto cond = new ICmpInst(storeInsertPos, ICmpInst::ICMP_EQ, dynIndex, constIndex);
      firstStoreDest = SelectInst::Create(cond, secondStoreDest, firstStoreDest, "", storeInsertPos);
    }

    Value *storeValue = storeInst->getOperand(0);
    new StoreInst(storeValue, firstStoreDest, storeInsertPos);

    checkStoreInsertPos->eraseFromParent();

    assert(storeInst->user_empty());
    storeInst->dropAllReferences();
    storeInst->eraseFromParent();
  } else {
    // .entry
    //   ...
    //   firstPtr  = getElemPtrs[0]
    //
    //   secondPtr = getElemPtrs[1]
    //   firstPtr  = (dynIndex == 1) ? secondPtr : firstPtr
    //
    //   secondPtr = getElemPtrs[2]
    //   firstPtr  = (dynIndex == 2) ? secondPtr : firstPtr
    //   ...
    //   secondPtr = getElemPtrs[upperBound - 2]
    //   firstPtr  = (dynIndex == upperBound - 2) ? secondPtr : firstPtr
    //
    //   secondPtr = getElemPtrs[upperBound - 1]
    //   firstPtr  = (dynIndex == upperBound - 1) ? secondPtr : firstPtr
    //
    //   store storeValue, firstPtr
    //   ...
    //   ret

    for (unsigned i = 1; i < getElemPtrCount; ++i) {
      auto secondStoreDest = getElemPtrs[i];
      auto constIndex = isType64 ? ConstantInt::get(Type::getInt64Ty(*m_context), i)
                                 : ConstantInt::get(Type::getInt32Ty(*m_context), i);
      auto cond = new ICmpInst(storeInst, ICmpInst::ICMP_EQ, dynIndex, constIndex);
      firstStoreDest = SelectInst::Create(cond, secondStoreDest, firstStoreDest, "", storeInst);
    }

    storeInst->setOperand(1, firstStoreDest);
  }
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering the memory operations.
INITIALIZE_PASS(LegacySpirvLowerMemoryOp, DEBUG_TYPE, "Lower SPIR-V memory operations", false, false)
