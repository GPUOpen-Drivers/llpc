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
 * @file  AddBufferOperationMetadata.h
 * @brief LLPC header file: contains declaration of class lgc::AddBufferOperationMetadata.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/state/PipelineState.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of LGC lowering operations for buffer operations
class AddBufferOperationMetadata : public llvm::PassInfoMixin<AddBufferOperationMetadata> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Add metadata for buffer operations"; }

private:
  void visitLoadInst(llvm::LoadInst &loadInst);
  void visitStoreInst(llvm::StoreInst &storeInst);
  void visitMemCpyInst(llvm::MemCpyInst &memCpyInst);
  void visitMemMoveInst(llvm::MemMoveInst &memMoveInst);
  void visitMemSetInst(llvm::MemSetInst &memSetInst);
  bool isAnyBufferPointer(const llvm::Value *const value);

  llvm::LLVMContext *m_context = nullptr; // Associated LLVM context of the LLVM module that passes run on
  llvm::MDNode *m_temporalHint = nullptr;
};

} // namespace lgc
