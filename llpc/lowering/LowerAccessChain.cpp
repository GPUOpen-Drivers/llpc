/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerAccessChain.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerAccessChain.
 ***********************************************************************************************************************
 */
#include "LowerAccessChain.h"
#include "SPIRVInternal.h"
#include "llpcDialect.h"
#include "lgc/Builder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <stack>

#define DEBUG_TYPE "lower-access-chain"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerAccessChain::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Access-Chain\n");

  SpirvLower::init(&module);

  // Invoke handling of "getelementptr", "load" and "store" instructions
  visit(m_module);

  // Remove dead "getelementptr" and custom "gep"
  for (auto *inst : m_removeGeps)
    inst->eraseFromParent();
  m_removeGeps.clear();

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Checks if pointer operand of getelementptr is a global value and if types are the same.
// If types are different (which may happen for opaque pointers) then this function is adding the missing zero-index
// elements to the gep instruction.
//
// One of the examples may be a type in which we have a multiple nested structures.
// { { [4 x float] } }
//
// @param gep : Custom structural gep instruction.
void LowerAccessChain::tryToAddMissingIndicesBetweenGVandGEP(CallInst *callInst) {
  auto *gep = cast<StructuralGepOp>(callInst);

  // We are interested only in address spaces which are used while doing global value lowering for store and load.
  Value *base = gep->getBasePointer();
  [[maybe_unused]] const unsigned addrSpace = base->getType()->getPointerAddressSpace();
  assert(addrSpace == SPIRAS_Input || addrSpace == SPIRAS_Output);

  GlobalValue *gv = dyn_cast<GlobalValue>(base);
  if (!gv)
    return;

  // No missing indices, types are the same.
  Type *baseType = gep->getBaseType();
  if (baseType == gv->getValueType())
    return;

  SmallVector<Value *, 8> idxs;
  idxs.push_back(m_builder->getInt32(0));
  appendZeroIndexToMatchTypes(idxs, baseType, gv->getValueType());

  for (auto *idx : gep->getIndices())
    idxs.push_back(idx);

  Value *newGep = m_builder->create<StructuralGepOp>(gv, gv->getValueType(), gep->getInbound(), idxs);
  gep->replaceAllUsesWith(newGep);
  m_removeGeps.emplace_back(gep);
}

// =====================================================================================================================
// Visits "load" instruction
//
// @param loadInst : "Load" instruction
void LowerAccessChain::visitLoadInst(LoadInst &loadInst) {
  if (auto *gep = dyn_cast<StructuralGepOp>(loadInst.getPointerOperand())) {
    m_builder->SetInsertPoint(&loadInst);
    tryToAddMissingIndicesBetweenGVandGEP(gep);
  }
}

// =====================================================================================================================
// Visits "store" instruction
//
// @param storeInst : "Store" instruction
void LowerAccessChain::visitStoreInst(StoreInst &storeInst) {
  if (auto *gep = dyn_cast<StructuralGepOp>(storeInst.getPointerOperand())) {
    m_builder->SetInsertPoint(&storeInst);
    tryToAddMissingIndicesBetweenGVandGEP(gep);
  }
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtrInst : "Getelementptr" instruction
void LowerAccessChain::visitGetElementPtrInst(GetElementPtrInst &getElemPtrInst) {
  // NOTE: Here, we try to coalesce chained "getelementptr" instructions (created from multi-level access chain).
  // Because the metadata is always decorated on top-level pointer value (actually a global variable).
  const unsigned addrSpace = getElemPtrInst.getType()->getPointerAddressSpace();
  assert(addrSpace != SPIRAS_Input && addrSpace != SPIRAS_Output);
  if (addrSpace == SPIRAS_Private) {
    m_builder->SetInsertPoint(&getElemPtrInst);
    tryToCoalesceChain(&getElemPtrInst);
  }
}

// =====================================================================================================================
// Visits custom "getelementptr" instruction.
//
// @param getElemPtrInst : Custom "Getelementptr" instruction
void LowerAccessChain::visitCallInst(CallInst &callInst) {
  auto *structuralGep = dyn_cast<StructuralGepOp>(&callInst);
  if (!structuralGep)
    return;
  [[maybe_unused]] const unsigned addrSpace = structuralGep->getBasePointer()->getType()->getPointerAddressSpace();
  assert(addrSpace == SPIRAS_Input || addrSpace == SPIRAS_Output);
  m_builder->SetInsertPoint(&callInst);
  auto *gep = tryToCoalesceChain(structuralGep);
  tryToAddMissingIndicesBetweenGVandGEP(cast<StructuralGepOp>(gep));
}

// =====================================================================================================================
// Tries to coalesce chained custom GEP or "gelelementptr" instructions (created from multi-level access chain) from
// bottom to top in the type hierarchy.
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
// @param getElemPtr : "getelementptr" or custom "gep" instruction in the bottom to do coalescing
Instruction *LowerAccessChain::tryToCoalesceChain(Instruction *getElemPtr) {
  const bool isCustomGep = isa<StructuralGepOp>(getElemPtr);
  auto getBasePointer = [=](Operator *gep) {
    return isCustomGep ? cast<StructuralGepOp>(gep)->getBasePointer() : cast<GEPOperator>(gep)->getPointerOperand();
  };
  auto getBaseType = [=](Operator *gep) {
    return isCustomGep ? cast<StructuralGepOp>(gep)->getBaseType() : cast<GEPOperator>(gep)->getSourceElementType();
  };
  auto getIndices = [=]<typename UnaryFunc>(Operator *gep, UnaryFunc &&stlRangeOp) {
    using IterTy = decltype(std::declval<StructuralGepOp>().getIndices().begin());
    auto range = isCustomGep ? cast<StructuralGepOp>(gep)->getIndices()
                             : llvm::make_range(static_cast<IterTy>(cast<GEPOperator>(gep)->indices().begin()),
                                                static_cast<IterTy>(cast<GEPOperator>(gep)->indices().end()));
    return stlRangeOp(range);
  };

  std::stack<Operator *> chainedInsts;    // Order: from top to bottom
  std::stack<Instruction *> removedInsts; // Order: from bottom to top

  // Collect chained "getelementptr" or custom "gep" instructions and constants from bottom to top.
  auto *ptrVal = cast<Operator>(getElemPtr);
  for (;;) {
    chainedInsts.push(ptrVal);
    auto *basePointer = getBasePointer(ptrVal);
    if (!isa<StructuralGepOp>(basePointer) && !isa<GEPOperator>(basePointer))
      break;
    assert((isa<StructuralGepOp>(basePointer) && isCustomGep) || (isa<GEPOperator>(basePointer) && !isCustomGep));
    ptrVal = cast<Operator>(basePointer);
  }

  if (chainedInsts.size() <= 1)
    return getElemPtr;

  // If there are more than one "getelementptr" instructions/constants, do coalescing
  SmallVector<Value *, 8> indices;
  Value *basePtr = getBasePointer(chainedInsts.top());
  Type *coalescedType = getBaseType(chainedInsts.top());

  while (!chainedInsts.empty()) {
    ptrVal = chainedInsts.top();
    chainedInsts.pop();

    Type *currentLevelGEPSourceType = getBaseType(ptrVal);
    Type *oneLevelAboveGEPRetType = GetElementPtrInst::getIndexedType(coalescedType, indices);
    if (currentLevelGEPSourceType != oneLevelAboveGEPRetType) {
      // For Opaque Pointers some of GEPs (all zero-index) will be removed and since Source Type of the coalesced
      // GEP is equal to the top of chained GEPs, this will lead to accessing wrong place in memory.
      //
      // Example:
      // %1 = getelementptr { i64, [3 x [4 x { <3 x i32>, <3 x i32> }]], [3 x [4 x i32]] }, ptr
      // addrspace(5) %381, i32 0, i32 1
      //
      // %2 = getelementptr [3 x [4 x { <3 x i32>, <3 x i32> }]], ptr addrspace(5) %1, i32 0, i32 0
      // ^^^ all zero-index GEP, lack of this instruction for opaque pointers
      //
      // %3 = getelementptr [4 x { <3 x i32>, <3 x i32> }], ptraddrspace(5) %2, i32 0, i32 0
      // ^^^ all zero-index GEP, lack of this instruction for opaque pointers
      //
      // %4 = getelementptr { <3 x i32>, <3 x i32> }, ptr addrspace(5) %3, i32 0, i32 1
      //
      //
      // Result after Lower Access Chain:
      //
      // In case of non opaque pointers
      // %5 = getelementptr { i64, [3 x [4 x { <3 x i32>, <3 x i32> }]], [3 x [4 x i32]] }, ptr
      // addrspace(5) %381, i32 0, i32 1, i32 0, i32 0, i32 1
      //
      // For opaque pointers
      // %5 = getelementptr { i64, [3 x [4 x { <3 x i32>, <3 x i32> }]], [3 x [4 x i32]] }, ptr
      // addrspace(5) %381, i32 0, i32 1, i32 1
      //
      // We need to compare two chained GEP instructions and see if return Type of one is the same as Source
      // Type of the other. If Types are not the same than we need to add
      // missing zero-index elements to the "idxs" which are used to create new (coalesced) GEP instruction.
      appendZeroIndexToMatchTypes(indices, currentLevelGEPSourceType, oneLevelAboveGEPRetType);
    }

    // NOTE: For subsequent "getelementptr" instructions/constants, we skip the first index due to it's always 0 to
    // dereference the pointer value.
    const unsigned skipCount = basePtr == getBasePointer(ptrVal) ? 0 : 1;
    for (auto *idx : getIndices(ptrVal, [=](auto range) {
           assert(llvm::range_size(range) > 0);
           return llvm::drop_begin(range, skipCount);
         }))
      indices.emplace_back(idx);

    assert(isa<GetElementPtrInst>(ptrVal) || isa<StructuralGepOp>(ptrVal));
    removedInsts.push(cast<Instruction>(ptrVal));
  }

  // Create the coalesced "getelementptr" instruction (do combining)
  auto *coalescedGetElemPtr =
      isCustomGep ? cast<Instruction>(m_builder->create<StructuralGepOp>(basePtr, coalescedType, false, indices))
                  : cast<Instruction>(m_builder->CreateGEP(coalescedType, basePtr, indices));
  getElemPtr->replaceAllUsesWith(coalescedGetElemPtr);

  // Remove dead "getelementptr" instructions where possible.
  while (!removedInsts.empty()) {
    Instruction *inst = removedInsts.top();
    if (inst->user_empty()) {
      auto *poison = PoisonValue::get(getBasePointer(cast<Operator>(inst))->getType());
      inst->setOperand(0, poison);
      m_removeGeps.emplace_back(inst);
    }
    removedInsts.pop();
  }

  return coalescedGetElemPtr;
}

// =====================================================================================================================
// Append zero-index elements to "indexOperands" vector while unpacking "baseType" to match "typeToMatch"
//
// This function is used to workaround the elimination of zero-index GEP instructions which is taking place
// when opaque pointers are enabled.
//
// @param indexOperands : vector to which zero-index elements will be added
// @param typeToMatch : type used as destination of unpacking "baseType"
// @param baseType : packed type which will be unpacked.
void LowerAccessChain::appendZeroIndexToMatchTypes(SmallVectorImpl<Value *> &indexOperands, Type *typeToMatch,
                                                   Type *baseType) {
  Type *unpackType = baseType;
  Value *zero = ConstantInt::get(Type::getInt32Ty(m_module->getContext()), 0);
  while (unpackType != typeToMatch) {
    // append zero-index
    indexOperands.push_back(zero);
    if (unpackType->isStructTy())
      unpackType = unpackType->getStructElementType(0);
    else if (unpackType->isArrayTy())
      unpackType = unpackType->getArrayElementType();
    else if (unpackType->isVectorTy())
      unpackType = cast<VectorType>(unpackType)->getElementType();
    else
      llvm_unreachable("Should never be called!");
  }
}

} // namespace Llpc
