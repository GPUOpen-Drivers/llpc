/*
************************************************************************************************************************
*
*  Copyright (C) 2023 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvProcessGpuRtLibrary.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvProcessGpuRtLibrary
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/IR/PassManager.h"
namespace Llpc {
class SpirvProcessGpuRtLibrary : public SpirvLower, public llvm::PassInfoMixin<SpirvProcessGpuRtLibrary> {
public:
  SpirvProcessGpuRtLibrary();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

private:
  typedef void (SpirvProcessGpuRtLibrary::*LibraryFuncPtr)(llvm::Function *);
  struct LibraryFunctionTable {
    llvm::DenseMap<llvm::StringRef, LibraryFuncPtr> m_libFuncPtrs;
    LibraryFunctionTable();
    static const LibraryFunctionTable &get() {
      static LibraryFunctionTable instance;
      return instance;
    }
  };
  void processLibraryFunction(llvm::Function *&func);
  void createGetStackSize(llvm::Function *func);
  void createGetStackBase(llvm::Function *func);
  void createLdsWrite(llvm::Function *func);
  void createLdsRead(llvm::Function *func);
  void createGetStackStride(llvm::Function *func);
  void createLdsStackInit(llvm::Function *func);
  void createLdsStackStore(llvm::Function *func);
  void createGetBoxSortHeuristicMode(llvm::Function *func);
  void createGetStaticFlags(llvm::Function *func);
  void createGetTriangleCompressionMode(llvm::Function *func);
};
} // namespace Llpc