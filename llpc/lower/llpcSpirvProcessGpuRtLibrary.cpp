/*
************************************************************************************************************************
*
*  Copyright (C) 2017-2023 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvLowerExecutionGraph.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerExecutionGraph.
 ***********************************************************************************************************************
 */
#include "llpcSpirvProcessGpuRtLibrary.h"
#include "llpcContext.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcContext.h"

#define DEBUG_TYPE "llpc-spirv-lower-gpurt-library"
using namespace lgc;
using namespace llvm;

namespace RtName {
static const char *AmdLibraryNames[] = {"AmdTraceRayGetStackSize",
                                        "AmdTraceRayLdsRead",
                                        "AmdTraceRayLdsWrite",
                                        "AmdTraceRayGetStackBase",
                                        "AmdTraceRayGetStackStride",
                                        "AmdTraceRayLdsStackInit",
                                        "AmdTraceRayLdsStackStore",
                                        "AmdTraceRayGetBoxSortHeuristicMode",
                                        "AmdTraceRayGetStaticFlags",
                                        "AmdTraceRayGetTriangleCompressionMode"

};
} // namespace RtName

namespace AmdLibraryFunc {
enum : unsigned {
  GetStackSize = 0,           // Get stack size
  LdsRead,                    // Read from LDS
  LdsWrite,                   // Write to LDS
  GetStackBase,               // Get stack base
  GetStackStride,             // Get stack stride
  LdsStackInit,               // Lds stack init
  LdsStackStore,              // Lds stack store
  GetBoxSortHeuristicMode,    // Get box sort heuristic mode
  GetStaticFlags,             // Get static flags
  GetTriangleCompressionMode, // Get triangle compression mode
  Count
};
} // namespace AmdLibraryFunc

namespace Llpc {
SpirvProcessGpuRtLibrary::SpirvProcessGpuRtLibrary() {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvProcessGpuRtLibrary::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-gpurt\n");
  SpirvLower::init(&module);
  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    processLibraryFunction(func);
  }
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Initialize library function pointer table
SpirvProcessGpuRtLibrary::LibraryFunctionTable::LibraryFunctionTable() {
  LibraryFuncPtr amdLibraryFuncs[] = {&SpirvProcessGpuRtLibrary::createGetStackSize,
                                      &SpirvProcessGpuRtLibrary::createLdsRead,
                                      &SpirvProcessGpuRtLibrary::createLdsWrite,
                                      &SpirvProcessGpuRtLibrary::createGetStackBase,
                                      &SpirvProcessGpuRtLibrary::createGetStackStride,
                                      &SpirvProcessGpuRtLibrary::createLdsStackInit,
                                      &SpirvProcessGpuRtLibrary::createLdsStackStore,
                                      &SpirvProcessGpuRtLibrary::createGetBoxSortHeuristicMode,
                                      &SpirvProcessGpuRtLibrary::createGetStaticFlags,
                                      &SpirvProcessGpuRtLibrary::createGetTriangleCompressionMode

  };
  for (unsigned i = 0; i < AmdLibraryFunc::Count; ++i) {
    m_libFuncPtrs[RtName::AmdLibraryNames[i]] = amdLibraryFuncs[i];
  }
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::processLibraryFunction(Function *&func) {
  auto &funcTable = LibraryFunctionTable::get().m_libFuncPtrs;

  auto funcIt = funcTable.find(func->getName());
  if (funcIt != funcTable.end()) {
    auto funcPtr = funcIt->second;
    m_builder->SetInsertPoint(clearBlock(func));
    (this->*funcPtr)(func);
  }
}

// =====================================================================================================================
// Create function to get stack size
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStackSize(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackSizeOp>());
}

// =====================================================================================================================
// Create function to get stack base
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStackBase(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackBaseOp>());
}

// =====================================================================================================================
// Create function to write LDS stack
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsWrite(llvm::Function *func) {
  auto argIt = func->arg_begin();
  auto int32ty = m_builder->getInt32Ty();
  Value *stackOffset = m_builder->CreateLoad(int32ty, argIt++);
  Value *stackData = m_builder->CreateLoad(int32ty, argIt);
  m_builder->CreateRet(m_builder->create<GpurtStackWriteOp>(stackOffset, stackData));
}

// =====================================================================================================================
// Create function to read LDS stack
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsRead(llvm::Function *func) {
  Value *stackIndex = func->arg_begin();
  stackIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), stackIndex);
  m_builder->CreateRet(m_builder->create<GpurtStackReadOp>(stackIndex));
}

// =====================================================================================================================
// Create function to get stack stride
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStackStride(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackStrideOp>());
}

// =====================================================================================================================
// Create function to init stack LDS
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsStackInit(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtLdsStackInitOp>());
}

// =====================================================================================================================
// Create function to store stack LDS
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsStackStore(llvm::Function *func) {
  auto argIt = func->arg_begin();
  Value *stackAddr = argIt++;
  Value *lastVisited = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  Value *data = m_builder->CreateLoad(int32x4Ty, argIt);
  m_builder->CreateRet(m_builder->create<GpurtLdsStackStoreOp>(stackAddr, lastVisited, data));
}

// =====================================================================================================================
// Create function to get box sort heuristic mode
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetBoxSortHeuristicMode(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetBoxSortHeuristicModeOp>());
}

// =====================================================================================================================
// Create function to get static flags
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStaticFlags(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStaticFlagsOp>());
}

// =====================================================================================================================
// Create function to get triangle compression mode
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetTriangleCompressionMode(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetTriangleCompressionModeOp>());
}

} // namespace Llpc