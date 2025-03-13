/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  AddBufferOperationMetadata.cpp
 * @brief LLPC source file: contains implementation of class lgc::AddBufferOperationMetadata.
 ***********************************************************************************************************************
 */
#include "lgc/lowering/AddBufferOperationMetadata.h"
#include "lgc/Builder.h"
#include "lgc/LgcDialect.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineState.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-add-metadata-for-buffer-operations"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] function : Function that we will patch.
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses AddBufferOperationMetadata::run(llvm::Function &function,
                                                  llvm::FunctionAnalysisManager &analysisManager) {

  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  m_pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();

  LLVM_DEBUG(dbgs() << "Run the pass Add-Buffer-Operation-Metadata\n");

  auto stage = getShaderStage(&function);
  if (!stage)
    return PreservedAnalyses::all();

  m_context = &function.getContext();
  m_stageMDNode = MDNode::get(
      *m_context, {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(function.getContext()), stage.value()))});

  static const auto visitor = llvm_dialects::VisitorBuilder<AddBufferOperationMetadata>()
                                  .add(&AddBufferOperationMetadata::visitLoadInst)
                                  .add(&AddBufferOperationMetadata::visitStoreInst)
                                  .add(&AddBufferOperationMetadata::visitMemCpyInst)
                                  .add(&AddBufferOperationMetadata::visitMemMoveInst)
                                  .add(&AddBufferOperationMetadata::visitMemSetInst)
                                  .add(&AddBufferOperationMetadata::visitLoadBufferDesc)
                                  .add(&AddBufferOperationMetadata::visitLoadStridedBufferDesc)
                                  .build();
  visitor.visit(*this, function);

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Visits "load" instruction.
//
// @param loadInst : The instruction
void AddBufferOperationMetadata::visitLoadInst(llvm::LoadInst &loadInst) {
  if (isAnyBufferPointer(loadInst.getPointerOperand()))
    loadInst.setMetadata(MetaNameBufferOpStage, m_stageMDNode);
}

// =====================================================================================================================
// Visits "store" instruction.
//
// @param storeInst : The instruction
void AddBufferOperationMetadata::visitStoreInst(llvm::StoreInst &storeInst) {
  if (isAnyBufferPointer(storeInst.getPointerOperand()))
    storeInst.setMetadata(MetaNameBufferOpStage, m_stageMDNode);
}

// =====================================================================================================================
// Post-process visits "memcpy" instruction.
//
// @param memCpyInst : The memcpy instruction
void AddBufferOperationMetadata::visitMemCpyInst(llvm::MemCpyInst &memCpyInst) {
  Value *const dest = memCpyInst.getArgOperand(0);
  Value *const src = memCpyInst.getArgOperand(1);
  if (isAnyBufferPointer(src) || isAnyBufferPointer(dest))
    memCpyInst.setMetadata(MetaNameBufferOpStage, m_stageMDNode);
}

// =====================================================================================================================
// Visits "memmove" instruction.
//
// @param memMoveInst : The memmove instruction
void AddBufferOperationMetadata::visitMemMoveInst(llvm::MemMoveInst &memMoveInst) {
  Value *const dest = memMoveInst.getArgOperand(0);
  Value *const src = memMoveInst.getArgOperand(1);
  if (isAnyBufferPointer(src) || isAnyBufferPointer(dest))
    memMoveInst.setMetadata(MetaNameBufferOpStage, m_stageMDNode);
}

// =====================================================================================================================
// Visits "memset" instruction.
//
// @param memSetInst : The memset instruction
void AddBufferOperationMetadata::visitMemSetInst(llvm::MemSetInst &memSetInst) {
  Value *const dest = memSetInst.getArgOperand(0);
  if (isAnyBufferPointer(dest))
    memSetInst.setMetadata(MetaNameBufferOpStage, m_stageMDNode);
}

// =====================================================================================================================
// Determine if a value is a buffer pointer. A buffer pointer is either a BUFFER_FAT_POINTER or
// a BUFFER_STRIDED_POINTER
//
// @param value : The value to check
bool AddBufferOperationMetadata::isAnyBufferPointer(const Value *const value) {
  return value->getType() == PointerType::get(*m_context, ADDR_SPACE_BUFFER_FAT_POINTER) ||
         value->getType() == PointerType::get(*m_context, ADDR_SPACE_BUFFER_STRIDED_POINTER);
}

// =====================================================================================================================
// Visits a load.buffer.desc operation
//
// @param op : the operation
void AddBufferOperationMetadata::visitLoadBufferDesc(LoadBufferDescOp &op) {
  if (op.getFlags() & Builder::BufferFlagLLcNoAlloc)
    addLlcMetadata(op.getDescSet(), op.getBinding(), &op);
}

// =====================================================================================================================
// Visits a load.strided.buffer.desc operation
//
// @param op : the operation
void AddBufferOperationMetadata::visitLoadStridedBufferDesc(LoadStridedBufferDescOp &op) {
  if (op.getFlags() & Builder::BufferFlagLLcNoAlloc)
    addLlcMetadata(op.getDescSet(), op.getBinding(), &op);
}

// =====================================================================================================================
// Add LLC metadata
//
// @param inst : The instruction
void AddBufferOperationMetadata::addLlcMetadata(unsigned set, unsigned binding, llvm::Value *inst) {
  SmallVector<Value *> users(inst->users());
  while (!users.empty()) {
    auto user = users.pop_back_val();
    if (auto gep = dyn_cast<GetElementPtrInst>(user)) {
      users.push_back(gep);
    } else if (auto load = dyn_cast<LoadInst>(user)) {
      load->setMetadata(MetaNameBufferOpLlc, MDNode::get(*m_context, {}));
    } else if (auto store = dyn_cast<StoreInst>(user)) {
      store->setMetadata(MetaNameBufferOpLlc, MDNode::get(*m_context, {}));
    }
  }
}

} // namespace lgc
