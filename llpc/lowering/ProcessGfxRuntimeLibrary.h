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
 * @file  ProcessGfxRuntimeLibrary.h
 * @brief LLPC header file: contains declaration of Llpc::ProcessGfxRuntimeLibrary
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {
class ProcessGfxRuntimeLibrary : public Lowering, public llvm::PassInfoMixin<ProcessGfxRuntimeLibrary> {
public:
  ProcessGfxRuntimeLibrary();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

private:
  typedef void (ProcessGfxRuntimeLibrary::*LibraryFuncPtr)(llvm::Function *);
  struct LibraryFunctionTable {
    llvm::DenseMap<llvm::StringRef, LibraryFuncPtr> m_libFuncPtrs;
    LibraryFunctionTable();
    static const LibraryFunctionTable &get() {
      static LibraryFunctionTable instance;
      return instance;
    }
  };
  void processLibraryFunction(llvm::Function *&func);
  void createTexelLoad(llvm::Function *func);
  void createTexelLoadMsaa(llvm::Function *func);
  void createCoherentTexelLoad(llvm::Function *func);
  void createCoherentTexelStore(llvm::Function *func);
  void createCoherentTexelLoadMsaa(llvm::Function *func);
  void createCoherentTexelStoreMsaa(llvm::Function *func);

  void loadTexel(llvm::Function *func, bool isMsaa, bool enableRov);
  void storeTexel(llvm::Function *func, bool isMsaa, bool enableRov);
};
} // namespace Llpc
