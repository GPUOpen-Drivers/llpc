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
 * @file  llpcSpirvProcessGpuRtLibrary.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvProcessGpuRtLibrary
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/ADT/FloatingPointMode.h"
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
  void createLoadDwordAtAddr(llvm::Function *func);
  void createLoadDwordAtAddrx2(llvm::Function *func);
  void createLoadDwordAtAddrx4(llvm::Function *func);
  void createLoadDwordAtAddrWithType(llvm::Function *func, llvm::Type *loadTy);
  void createConvertF32toF16NegInf(llvm::Function *func);
  void createConvertF32toF16PosInf(llvm::Function *func);
  void createConvertF32toF16WithRoundingMode(llvm::Function *func, llvm::RoundingMode rm);
  void createIntersectBvh(llvm::Function *func);
  void createSampleGpuTimer(llvm::Function *func);
  void createGetFlattenedGroupThreadId(llvm::Function *func);
  void createGetHitAttributes(llvm::Function *func);
  void createSetHitAttributes(llvm::Function *func);
  void createSetTraceParams(llvm::Function *func);
  void createCallClosestHitShader(llvm::Function *func);
  void createCallMissShader(llvm::Function *func);
  void createCallTriangleAnyHitShader(llvm::Function *func);
  void createCallIntersectionShader(llvm::Function *func);
  void createSetTriangleIntersectionAttributes(llvm::Function *func);
  void createSetHitTriangleNodePointer(llvm::Function *func);
  void createGetParentId(llvm::Function *func);
  void createSetParentId(llvm::Function *func);
  void createDispatchRayIndex(llvm::Function *func);
  void createGetStaticId(llvm::Function *func);
  void createGetKnownSetRayFlags(llvm::Function *func);
  void createGetKnownUnsetRayFlags(llvm::Function *func);
  void createContStackAlloc(llvm::Function *func);
  void createContStackFree(llvm::Function *func);
  void createContStackGetPtr(llvm::Function *func);
  void createContStackSetPtr(llvm::Function *func);
  void createContStackLoad(llvm::Function *func);
  void createContStackStore(llvm::Function *func);
  llvm::Value *createGetBvhSrd(llvm::Value *expansion, llvm::Value *boxSortMode);
};
} // namespace Llpc
