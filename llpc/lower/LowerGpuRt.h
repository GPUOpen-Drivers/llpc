/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerGpuRt.h
 * @brief LLPC header file: contains declaration of Llpc::LowerGpuRt
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/PassManager.h"

namespace lgc {
class GpurtGetStackSize;
class GpurtGetStackBase;
class GpurtGetStackStride;
class GpurtStackWrite;
class GpurtStackRead;
class GpurtLdsStackInit;
class GpurtLdsStackStore;
} // namespace lgc

namespace llvm {
class AllocaInst;
}

namespace Llpc {
class LowerGpuRt : public SpirvLower, public llvm::PassInfoMixin<LowerGpuRt> {
public:
  LowerGpuRt();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

private:
  typedef void (LowerGpuRt::*LibraryFuncPtr)(llvm::Function *, unsigned);
  const static unsigned MaxLdsStackEntries = 16;
  uint32_t getWorkgroupSize() const;
  llvm::Value *getThreadIdInGroup() const;
  void createGlobalStack();
  void getStackSize(lgc::GpurtGetStackSize &inst);
  void getStackBase(lgc::GpurtGetStackBase &inst);
  void getStackStride(lgc::GpurtGetStackStride &inst);
  void stackWrite(lgc::GpurtStackWrite &inst);
  void stackRead(lgc::GpurtStackRead &inst);
  void ldsStackInit(lgc::GpurtLdsStackInit &inst);
  void ldsStackStore(lgc::GpurtLdsStackStore &inst);
  llvm::Value *m_stack;                                  // Stack array to hold stack value
  llvm::Type *m_stackTy;                                 // Stack type
  bool m_lowerStack;                                     // If it is lowerStack
  llvm::SmallVector<llvm::Instruction *> m_callsToLower; // Call instruction to lower
  llvm::SmallSet<llvm::Function *, 4> m_funcsToLower;    // Functions to lower
};
} // namespace Llpc
