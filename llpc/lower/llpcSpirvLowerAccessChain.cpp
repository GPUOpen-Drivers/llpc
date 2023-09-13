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
 * @file  llpcSpirvLowerAccessChain.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerAccessChain.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerAccessChain.h"
#include "SPIRVInternal.h"
#include "lgc/Builder.h"
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

  // Invoke handling of "getelementptr", "load" and "store" instructions
  visit(m_module);

  return true;
}

// =====================================================================================================================
// Checks if pointer operand of getelementptr is a global value and if types are the same.
// If types are different (which may happen for opaque pointers) then this function is adding the missing zero-index
// elements to the gep instruction.
//
// One of the examples may be a type in which we have a multiple nested structures.
// { { [4 x float] } }
//
// @param gep : Getelementptr instruction.
void SpirvLowerAccessChain::tryToAddMissingIndicesBetweenGVandGEP(GEPOperator *gep) {

  // We are interested only in address spaces which are used while doing global value lowering for store and load.
  const unsigned addrSpace = gep->getType()->getPointerAddressSpace();
  if (addrSpace != SPIRAS_Input && addrSpace != SPIRAS_Output)
    return;

  GlobalValue *gv = dyn_cast<GlobalValue>(gep->getPointerOperand());
  if (!gv)
    return;

  // No missing indices, types are the same.
  if (gep->getSourceElementType() == gv->getValueType())
    return;

  SmallVector<Value *, 8> idxs;
  idxs.push_back(m_builder->getInt32(0));
  appendZeroIndexToMatchTypes(idxs, gep->getSourceElementType(), gv->getValueType());

  for (unsigned i = 2; i != gep->getNumOperands(); ++i)
    idxs.push_back(gep->getOperand(i));

  Value *newGep = m_builder->CreateGEP(gv->getValueType(), gv, idxs);
  gep->replaceAllUsesWith(newGep);
  if (Instruction *inst = dyn_cast<Instruction>(gep))
    inst->eraseFromParent();
}

// =====================================================================================================================
// Visits "load" instruction
//
// @param loadInst : "Load" instruction
void SpirvLowerAccessChain::visitLoadInst(LoadInst &loadInst) {
  if (GEPOperator *gep = dyn_cast<GEPOperator>(loadInst.getPointerOperand())) {
    m_builder->SetInsertPoint(&loadInst);
    tryToAddMissingIndicesBetweenGVandGEP(gep);
  }
}

// =====================================================================================================================
// Visits "store" instruction
//
// @param storeInst : "Store" instruction
void SpirvLowerAccessChain::visitStoreInst(StoreInst &storeInst) {
  if (GEPOperator *gep = dyn_cast<GEPOperator>(storeInst.getPointerOperand())) {
    m_builder->SetInsertPoint(&storeInst);
    tryToAddMissingIndicesBetweenGVandGEP(gep);
  }
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
//
// @param getElemPtrInst : "Getelementptr" instruction
void SpirvLowerAccessChain::visitGetElementPtrInst(GetElementPtrInst &getElemPtrInst) {
  // NOTE: Here, we try to coalesce chained "getelementptr" instructions (created from multi-level access chain).
  // Because the metadata is always decorated on top-level pointer value (actually a global variable).
  const unsigned addrSpace = getElemPtrInst.getType()->getPointerAddressSpace();
  if (addrSpace == SPIRAS_Private || addrSpace == SPIRAS_Input || addrSpace == SPIRAS_Output) {
    GetElementPtrInst *gep = tryToCoalesceChain(&getElemPtrInst, addrSpace);
    if (GEPOperator *gepOp = dyn_cast<GEPOperator>(gep)) {
      m_builder->SetInsertPoint(gep);
      tryToAddMissingIndicesBetweenGVandGEP(gepOp);
    }
  }
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

  std::stack<GEPOperator *> chainedInsts;       // Order: from top to bottom
  std::stack<GetElementPtrInst *> removedInsts; // Order: from bottom to top

  // Collect chained "getelementptr" instructions and constants from bottom to top.
  auto ptrVal = cast<GEPOperator>(getElemPtr);
  for (;;) {
    chainedInsts.push(ptrVal);
    ptrVal = dyn_cast<GEPOperator>(ptrVal->getOperand(0));
    if (!ptrVal)
      break;
  }

  // If there are more than one "getelementptr" instructions/constants, do coalescing
  if (chainedInsts.size() > 1) {
    SmallVector<Value *, 8> idxs;
    unsigned startOperand = 1;
    Value *basePtr = nullptr;
    Type *coalescedType = nullptr;

    do {
      ptrVal = chainedInsts.top();
      chainedInsts.pop();

      if (coalescedType) {
        Type *currentLevelGEPSourceType = ptrVal->getSourceElementType();
        Type *oneLevelAboveGEPRetType = GetElementPtrInst::getIndexedType(coalescedType, idxs);
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
          appendZeroIndexToMatchTypes(idxs, currentLevelGEPSourceType, oneLevelAboveGEPRetType);
        }
      }

      for (unsigned i = startOperand; i != ptrVal->getNumOperands(); ++i)
        idxs.push_back(ptrVal->getOperand(i));
      // NOTE: For subsequent "getelementptr" instructions/constants, we skip the first two operands. The first
      // operand is the pointer value from which the element pointer is constructed. And the second one is always
      // 0 to dereference the pointer value.
      startOperand = 2;

      if (!basePtr) {
        basePtr = ptrVal->getOperand(0);
        coalescedType = ptrVal->getSourceElementType();
      }

      if (auto inst = dyn_cast<GetElementPtrInst>(ptrVal))
        removedInsts.push(inst);
    } while (!chainedInsts.empty());

    // Create the coalesced "getelementptr" instruction (do combining)
    coalescedGetElemPtr = GetElementPtrInst::Create(coalescedType, basePtr, idxs, "", getElemPtr);
    getElemPtr->replaceAllUsesWith(coalescedGetElemPtr);

    // Remove dead "getelementptr" instructions where possible.
    while (!removedInsts.empty()) {
      GetElementPtrInst *inst = removedInsts.top();
      if (inst->user_empty()) {
        if (inst == getElemPtr) {
          // We cannot remove the current instruction that InstWalker is on. Just stop it using its
          // pointer operand, and it will be DCEd later.
          auto &operand = inst->getOperandUse(0);
          operand = PoisonValue::get(operand->getType());
        } else
          inst->eraseFromParent();
      }
      removedInsts.pop();
    }
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
void SpirvLowerAccessChain::appendZeroIndexToMatchTypes(SmallVectorImpl<Value *> &indexOperands, Type *typeToMatch,
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
