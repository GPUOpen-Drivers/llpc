/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerCooperativeMatrix.cpp
 * @brief LLPC source file: pass that lower SPIR-V specific cooperative matrix operations
 *
 * This currently only handles spirv.cooperative.matrix.proxy, which is used to proxy pointers to cooperative matrix
 * values for component load/store.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerCooperativeMatrix.h"
#include "llpcDialect.h"
#include "lgc/BuilderCommon.h"
#include "llvm/IR/Instructions.h"

#define DEBUG_TYPE "llpc-spirv-lower-cooperative-matrix"

using namespace llvm;
using namespace lgc;
using namespace Llpc;

namespace {

// =====================================================================================================================
// Implementation class of the pass, hidden from external access.
class LowerCooperativeMatrix {
public:
  LowerCooperativeMatrix(Module &module) : m_module(module), m_builder(module.getContext()) {}

  PreservedAnalyses run();

private:
  void visitProxy(CallInst &call);
  void visitPointerUsers(Value *ptr, BuilderCommon::CooperativeMatrixElementType elemTypeEnum,
                         BuilderCommon::CooperativeMatrixLayout layout, Type *elemType, Value *matrixPtr, Value *index);

  Module &m_module;
  BuilderCommon m_builder;
  SmallVector<Instruction *, 8> m_toDelete;
};

} // anonymous namespace

// =====================================================================================================================
// Run the lowering implementation.
PreservedAnalyses LowerCooperativeMatrix::run() {
  bool changed = false;

  for (Function &function : m_module.functions()) {
    if (function.isDeclaration() && function.getName().startswith(LlpcName::SpirvCooperativeMatrixProxy)) {
      for (User *user : function.users()) {
        if (auto *call = dyn_cast<CallInst>(user)) {
          assert(call->getCalledOperand() == &function);
          visitProxy(*call);
          changed = true;
        }
      }
    }
  }

  for (Instruction *inst : reverse(m_toDelete))
    inst->eraseFromParent();

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Handle one call to spirv.cooperative.matrix.proxy.
//
// @param call : the call instruction
// @returns true if a change was made
void LowerCooperativeMatrix::visitProxy(CallInst &call) {
  Value *ptr = call.getArgOperand(0);
  auto elemTypeEnum =
      (BuilderCommon::CooperativeMatrixElementType)(cast<ConstantInt>(call.getArgOperand(1))->getZExtValue());
  Type *elemType = m_builder.transCooperativeMatrixElementType(elemTypeEnum);
  auto layout = (BuilderCommon::CooperativeMatrixLayout)(cast<ConstantInt>(call.getArgOperand(2))->getZExtValue());

  m_toDelete.push_back(&call);
  visitPointerUsers(&call, elemTypeEnum, layout, elemType, ptr, m_builder.getInt32(0));
}

// =====================================================================================================================
// Handle all users of a pointer defined directly or indirectly via spirv.cooperative.matrix.proxy.
//
// @param ptr : the pointer whose users should be handled
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @param matrixPtr : the pointer to the underlying proxied matrix
// @param index : the 32-bit index of the matrix that @p ptr points to
void LowerCooperativeMatrix::visitPointerUsers(Value *ptr, BuilderCommon::CooperativeMatrixElementType elemTypeEnum,
                                               BuilderCommon::CooperativeMatrixLayout layout, Type *elemType,
                                               Value *matrixPtr, Value *index) {
  for (User *user : ptr->users()) {
    Instruction *inst = cast<Instruction>(user);
    m_builder.SetInsertPoint(inst);

    m_toDelete.push_back(inst);

    if (auto *load = dyn_cast<LoadInst>(inst)) {
      assert(load->getPointerOperand() == ptr);
      assert(load->getType() == elemType);

      Type *matrixType = m_builder.getCooperativeMatrixTy(elemTypeEnum, layout);
      Value *matrix = m_builder.CreateLoad(matrixType, matrixPtr);
      Value *element = m_builder.CreateCooperativeMatrixExtract(matrix, index, elemTypeEnum, layout);
      load->replaceAllUsesWith(element);
    } else if (auto *store = dyn_cast<StoreInst>(inst)) {
      assert(store->getPointerOperand() == ptr);
      assert(store->getValueOperand()->getType() == elemType);

      Type *matrixType = m_builder.getCooperativeMatrixTy(elemTypeEnum, layout);
      Value *matrix = m_builder.CreateLoad(matrixType, matrixPtr);
      matrix = m_builder.CreateCooperativeMatrixInsert(matrix, store->getValueOperand(), index, elemTypeEnum, layout);
      m_builder.CreateStore(matrix, matrixPtr);
    } else if (auto *gep = dyn_cast<GetElementPtrInst>(inst)) {
      assert(gep->getPointerOperand() == ptr);
      assert(gep->getSourceElementType() == elemType);
      assert(gep->getNumIndices() == 1);

      Value *gepIndex = gep->indices().begin()->get();
      gepIndex = m_builder.CreateZExtOrTrunc(gepIndex, m_builder.getInt32Ty());

      bool baseIsZero = false;
      if (auto *constIndex = dyn_cast<ConstantInt>(index))
        baseIsZero = constIndex->getZExtValue() == 0;
      if (!baseIsZero)
        gepIndex = m_builder.CreateAdd(index, gepIndex);

      visitPointerUsers(gep, elemTypeEnum, layout, elemType, matrixPtr, gepIndex);
    } else {
      llvm_unreachable("indirect users of spirv.cooperative.matrix.proxy pointer");
    }
  }
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerCooperativeMatrix::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LowerCooperativeMatrix impl{module};
  return impl.run();
}
