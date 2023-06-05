/*
************************************************************************************************************************
*
*  Copyright (C) 2023 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
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
  void visitGetStackSize(lgc::GpurtGetStackSizeOp &inst);
  void visitGetStackBase(lgc::GpurtGetStackBaseOp &inst);
  void visitGetStackStride(lgc::GpurtGetStackStrideOp &inst);
  void visitStackWrite(lgc::GpurtStackWriteOp &inst);
  void visitStackRead(lgc::GpurtStackReadOp &inst);
  void visitLdsStackInit(lgc::GpurtLdsStackInitOp &inst);
  void visitLdsStackStore(lgc::GpurtLdsStackStoreOp &inst);
  llvm::Value *m_stack;                                  // Stack array to hold stack value
  llvm::Type *m_stackTy;                                 // Stack type
  bool m_lowerStack;                                     // If it is lowerStack
  llvm::SmallVector<llvm::Instruction *> m_callsToLower; // Call instruction to lower
  llvm::SmallSet<llvm::Function *, 4> m_funcsToLower;    // Functions to lower
};
} // namespace Llpc