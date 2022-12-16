/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerConstImmediateStore.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerConstImmediateStore.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class AllocaInst;
class StoreInst;
} // namespace llvm

namespace Llpc {

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations for constant immediate store
class SpirvLowerConstImmediateStore : public SpirvLower, public llvm::PassInfoMixin<SpirvLowerConstImmediateStore> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  bool runImpl(llvm::Module &module);

  static llvm::StringRef name() { return "Lower SPIR-V constant immediate store"; }

private:
  void processAllocaInsts(llvm::Function *func);
  llvm::StoreInst *findSingleStore(llvm::AllocaInst *allocaInst);
  void convertAllocaToReadOnlyGlobal(llvm::StoreInst *storeInst);
};

} // namespace Llpc
