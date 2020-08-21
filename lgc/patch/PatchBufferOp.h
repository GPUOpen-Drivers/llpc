/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchBufferOp.h
 * @brief LLPC header file: contains declaration of class lgc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"

namespace llvm {
class LegacyDivergenceAnalysis;
} // namespace llvm

namespace lgc {

class PipelineState;

// =====================================================================================================================
// Represents the pass of LLVM patching operations for buffer operations
class PatchBufferOp final : public llvm::FunctionPass, public llvm::InstVisitor<PatchBufferOp> {
public:
  PatchBufferOp();

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override;
  bool runOnFunction(llvm::Function &function) override;

  // Visitors
  void visitAtomicCmpXchgInst(llvm::AtomicCmpXchgInst &atomicCmpXchgInst);
  void visitAtomicRMWInst(llvm::AtomicRMWInst &atomicRmwInst);
  void visitBitCastInst(llvm::BitCastInst &bitCastInst);
  void visitCallInst(llvm::CallInst &callInst);
  void visitExtractElementInst(llvm::ExtractElementInst &extractElementInst);
  void visitGetElementPtrInst(llvm::GetElementPtrInst &getElemPtrInst);
  void visitInsertElementInst(llvm::InsertElementInst &insertElementInst);
  void visitLoadInst(llvm::LoadInst &loadInst);
  void visitMemCpyInst(llvm::MemCpyInst &memCpyInst);
  void visitMemMoveInst(llvm::MemMoveInst &memMoveInst);
  void visitMemSetInst(llvm::MemSetInst &memSetInst);
  void visitPHINode(llvm::PHINode &phiNode);
  void visitSelectInst(llvm::SelectInst &selectInst);
  void visitStoreInst(llvm::StoreInst &storeInst);
  void visitICmpInst(llvm::ICmpInst &icmpInst);
  void visitPtrToIntInst(llvm::PtrToIntInst &ptrToIntInst);

  static char ID; // ID of this pass

private:
  PatchBufferOp(const PatchBufferOp &) = delete;
  PatchBufferOp &operator=(const PatchBufferOp &) = delete;

  llvm::Value *getPointerOperandAsInst(llvm::Value *const value);
  llvm::Value *getBaseAddressFromBufferDesc(llvm::Value *const bufferDesc) const;
  void copyMetadata(llvm::Value *const dest, const llvm::Value *const src) const;
  llvm::PointerType *getRemappedType(llvm::Type *const type) const;
  bool removeUsersForInvariantStarts(llvm::Value *const value);
  llvm::Value *replaceLoadStore(llvm::Instruction &loadInst);
  llvm::Value *replaceICmp(llvm::ICmpInst *const iCmpInst);
  llvm::Instruction *makeLoop(llvm::Value *const loopStart, llvm::Value *const loopEnd, llvm::Value *const loopStride,
                              llvm::Instruction *const insertPos);
  void postVisitMemCpyInst(llvm::MemCpyInst &memCpyInst);
  void postVisitMemSetInst(llvm::MemSetInst &memSetInst);
  void fixIncompletePhis();

  using Replacement = std::pair<llvm::Value *, llvm::Value *>;
  using PhiIncoming = std::pair<llvm::PHINode *, llvm::BasicBlock *>;
  llvm::DenseMap<llvm::Value *, Replacement> m_replacementMap; // The replacement map.
  llvm::DenseMap<PhiIncoming, llvm::Value *> m_incompletePhis; // The incomplete phi map.
  llvm::DenseSet<llvm::Value *> m_invariantSet;                // The invariant set.
  llvm::DenseSet<llvm::Value *> m_divergenceSet;               // The divergence set.
  llvm::LegacyDivergenceAnalysis *m_divergenceAnalysis;        // The divergence analysis.
  llvm::SmallVector<llvm::Instruction *, 16> m_postVisitInsts; // The post process instruction set.
  std::unique_ptr<llvm::IRBuilder<>> m_builder;                // The IRBuilder.
  llvm::LLVMContext *m_context;                                // The LLVM context.
  PipelineState *m_pipelineState;                              // The pipeline state

  static constexpr unsigned MinMemOpLoopBytes = 256;
};

} // namespace lgc
