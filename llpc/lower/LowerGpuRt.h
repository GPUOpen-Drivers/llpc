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
class GpurtGetStackSizeOp;
class GpurtGetStackBaseOp;
class GpurtGetStackStrideOp;
class GpurtStackWriteOp;
class GpurtStackReadOp;
class GpurtLdsStackInitOp;
class GpurtLdsStackStoreOp;
class GpurtGetBoxSortHeuristicModeOp;
class GpurtGetStaticFlagsOp;
class GpurtGetTriangleCompressionModeOp;
class GpurtGetFlattenedGroupThreadIdOp;
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
  void createRayStaticIdValue();
  void visitGetStackSize(lgc::GpurtGetStackSizeOp &inst);
  void visitGetStackBase(lgc::GpurtGetStackBaseOp &inst);
  void visitGetStackStride(lgc::GpurtGetStackStrideOp &inst);
  void visitStackWrite(lgc::GpurtStackWriteOp &inst);
  void visitStackRead(lgc::GpurtStackReadOp &inst);
  void visitLdsStackInit(lgc::GpurtLdsStackInitOp &inst);
  void visitLdsStackStore(lgc::GpurtLdsStackStoreOp &inst);
  void visitGetBoxSortHeuristicMode(lgc::GpurtGetBoxSortHeuristicModeOp &inst);
  void visitGetStaticFlags(lgc::GpurtGetStaticFlagsOp &inst);
  void visitGetTriangleCompressionMode(lgc::GpurtGetTriangleCompressionModeOp &inst);
  void visitGetFlattenedGroupThreadId(lgc::GpurtGetFlattenedGroupThreadIdOp &inst);
  llvm::Value *m_stack;                                  // Stack array to hold stack value
  llvm::Type *m_stackTy;                                 // Stack type
  bool m_lowerStack;                                     // If it is lowerStack
  llvm::SmallVector<llvm::Instruction *> m_callsToLower; // Call instruction to lower
  llvm::SmallSet<llvm::Function *, 4> m_funcsToLower;    // Functions to lower
};
} // namespace Llpc
