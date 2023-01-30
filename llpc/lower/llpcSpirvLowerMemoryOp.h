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
 * @file  llpcSpirvLowerMemoryOp.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerMemoryOp.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"
#include <unordered_set>

namespace llvm {
class GetElementPtrInst;
class StoreInst;
} // namespace llvm

namespace Llpc {

// =====================================================================================================================
// The structure for store instruction which needs to be expanded.
struct StoreExpandInfo {
  llvm::StoreInst *storeInst;                                  ///< "Store" instruction
  llvm::SmallVector<llvm::GetElementPtrInst *, 1> getElemPtrs; ///< A group of "getelementptr" with constant indices
  llvm::Value *dynIndex;                                       ///< Dynamic index of destination.
};

// =====================================================================================================================
// Represents the pass of SPIR-V lowering memory operations.
class SpirvLowerMemoryOp : public SpirvLower,
                           public llvm::InstVisitor<SpirvLowerMemoryOp>,
                           public llvm::PassInfoMixin<SpirvLowerMemoryOp> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  bool runImpl(llvm::Module &module);

  static llvm::StringRef name() { return "Lower SPIR-V memory operations"; }

  virtual void visitGetElementPtrInst(llvm::GetElementPtrInst &getElementPtrInst);
  virtual void visitExtractElementInst(llvm::ExtractElementInst &extractElementInst);

private:
  bool needExpandDynamicIndex(llvm::GetElementPtrInst *getElemPtr, unsigned *operandIndex,
                              unsigned *dynIndexBound) const;
  void expandLoadInst(llvm::LoadInst *loadInst, llvm::ArrayRef<llvm::GetElementPtrInst *> getElemPtrs,
                      llvm::Value *dynIndex);
  void recordStoreExpandInfo(llvm::StoreInst *storeInst, llvm::ArrayRef<llvm::GetElementPtrInst *> getElemPtrs,
                             llvm::Value *dynIndex);
  void expandStoreInst(llvm::StoreInst *storeInst, llvm::ArrayRef<llvm::GetElementPtrInst *> getElemPtrs,
                       llvm::Value *dynIndex);

  std::unordered_set<llvm::Instruction *> m_removeInsts;
  std::unordered_set<llvm::Instruction *> m_preRemoveInsts;
  llvm::SmallVector<StoreExpandInfo, 1> m_storeExpandInfo;
};

} // namespace Llpc
