/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerGraphLibrary.h
 * @brief LLPC header file: contains declaration of Llpc::LowerGraphLibrary
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace Llpc {

// =====================================================================================================================
// Represents the pass of SPIR-V lowering graph library
class LowerGraphLibrary : public SpirvLower, public llvm::PassInfoMixin<LowerGraphLibrary> {
public:
  LowerGraphLibrary();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "Lower SPIR-V library shader"; }

private:
  typedef void (LowerGraphLibrary::*LibraryFuncPtr)(llvm::Function *, unsigned);
  void processLibraryFunction(llvm::Function *&func);
  llvm::BasicBlock *clearBlock(llvm::Function *func);
  void createBackingStore(llvm::Function *func, unsigned);
  void createShaderDirectory(llvm::Function *func, unsigned);
  void createNodeDispatchInfo1(llvm::Function *func, unsigned);
  void createNodeDispatchInfo2(llvm::Function *func, unsigned);
  void createTraceBuffer(llvm::Function *func, unsigned);
  void createLdsLoadDword(llvm::Function *func, unsigned);
  void createLdsStoreDword(llvm::Function *func, unsigned);
  void createLdsAtomicAddDword(llvm::Function *func, unsigned);
  void createOutputCount(llvm::Function *func, unsigned);
  llvm::DenseSet<llvm::StringRef> m_workgraphNames;         // External linked workgraph functions
  llvm::DenseMap<llvm::StringRef, unsigned> m_extFuncNames; // Library functions to patch
};
}; // namespace Llpc
