/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchBoundsCheckMemory.h
 * @brief LLPC header file: contains declaration of class lgc::PatchBoundsCheckMemory.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include <utility>
#include <vector>

namespace llvm {
class GetElementPtrInst;
class StoreInst;
} // namespace llvm

namespace lgc {

// =====================================================================================================================
// The structure for getelementptr instruction which need to be checked.
struct GetElemPtrInfo {
  llvm::GetElementPtrInst *getElemPtr;
  // Dynamic index value and maximum for each index.
  llvm::SmallVector<std::pair<llvm::Value *, uint64_t>, 1> dynIndices;
};

// =====================================================================================================================
// Represents the pass of bounds check memory operations.
class PatchBoundsCheckMemory final : public Patch, public llvm::InstVisitor<PatchBoundsCheckMemory> {
public:
  PatchBoundsCheckMemory();

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
  }

  bool runOnModule(llvm::Module &module) override;
  void visitGetElementPtrInst(llvm::GetElementPtrInst &getElementPtrInst);

  static char ID; // ID of this pass

private:
  PatchBoundsCheckMemory(const PatchBoundsCheckMemory &) = delete;
  PatchBoundsCheckMemory &operator=(const PatchBoundsCheckMemory &) = delete;

  bool addBoundsCheck(GetElemPtrInfo &info);

  std::vector<GetElemPtrInfo> m_getElementPtrInsts; // The memory accesses to patch.
  std::unique_ptr<llvm::IRBuilder<>> m_builder;
  llvm::LLVMContext *m_context;
  PipelineState *m_pipelineState;
};

} // namespace lgc
