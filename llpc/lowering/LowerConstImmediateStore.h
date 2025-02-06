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
 * @file  LowerConstImmediateStore.h
 * @brief LLPC header file: contains declaration of class Llpc::LowerConstImmediateStore.
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class AllocaInst;
class StoreInst;
class Value;
} // namespace llvm

namespace Llpc {

// =====================================================================================================================
// Represents the pass of FE lowering operations for constant immediate store
class LowerConstImmediateStore : public SpirvLower, public llvm::PassInfoMixin<LowerConstImmediateStore> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower constant immediate store"; }

private:
  bool processAllocaInsts(llvm::Function *func);
  bool tryProcessAlloca(llvm::AllocaInst *allocaInst);

  llvm::DenseMap<llvm::Value *, llvm::GlobalVariable *> m_allocToGlobals;
};

} // namespace Llpc
