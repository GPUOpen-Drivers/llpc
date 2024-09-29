/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerGraphLibrary.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerGraphLibrary.
 ***********************************************************************************************************************
 */

#include "LowerGraphLibrary.h"
#include "LowerInternalLibraryIntrinsic.h"
#include "SPIRVInternal.h"
#include "lgc/Builder.h"
#include "lgc/BuiltIns.h"
#include "lgc/LgcWgDialect.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lower-graph-library"

using namespace llvm;
using namespace Llpc;
using namespace lgc;
extern const char *WorkGraphNames[];
constexpr unsigned WorkGraphFuncCount = 16;

namespace AmdExtFunc {
enum : unsigned {
  BackingStore = 0,  // Backing store
  ShaderDirectory,   // Shader Directory
  NodeDispatchInfo1, // Node Dispatch Info1
  NodeDispatchInfo2, // Node Dispatch Info2
  TraceBuffer,       // Trace Buffer
  LdsLoadDword,      // Lds load dword
  LdsStoreDword,     // Lds store dword
  LdsAtomicAddDword, // Lds atomic add
  OutputCount,       // Lds output count
  Count
};
}

static const char *AmdExtNames[] = {
    "AmdWorkGraphsBackingStore",      "AmdWorkGraphsShaderDirectory",   "AmdWorkGraphsNodeDispatchInfo1",
    "AmdWorkGraphsNodeDispatchInfo2", "AmdWorkGraphsTraceBuffer",       "AmdWorkGraphsLdsLoadDword",
    "AmdWorkGraphsLdsStoreDword",     "AmdWorkGraphsLdsAtomicAddDword", "AmdWorkGraphsOutputCount"};

// =====================================================================================================================
LowerGraphLibrary::LowerGraphLibrary() {
  for (unsigned i = 0; i < AmdExtFunc::Count; ++i) {
    m_extFuncNames[AmdExtNames[i]] = i;
  }
  for (unsigned i = 0; i < WorkGraphFuncCount; ++i)
    m_workgraphNames.insert(WorkGraphNames[i]);
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerGraphLibrary::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-execution-graph\n");
  SpirvLower::init(&module);
  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    processLibraryFunction(func);
  }
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to clear
BasicBlock *LowerGraphLibrary::clearBlock(Function *func) {
  assert(func->size() == 1);
  BasicBlock &entryBlock = func->getEntryBlock();
  for (auto instIt = entryBlock.begin(); instIt != entryBlock.end();) {
    auto &inst = *instIt++;
    inst.eraseFromParent();
  }
  return &entryBlock;
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to process
void LowerGraphLibrary::processLibraryFunction(Function *&func) {
  LibraryFuncPtr amdLibraryFuncs[] = {
      &LowerGraphLibrary::createBackingStore,      &LowerGraphLibrary::createShaderDirectory,
      &LowerGraphLibrary::createNodeDispatchInfo1, &LowerGraphLibrary::createNodeDispatchInfo2,
      &LowerGraphLibrary::createTraceBuffer,       &LowerGraphLibrary::createLdsLoadDword,
      &LowerGraphLibrary::createLdsStoreDword,     &LowerGraphLibrary::createLdsAtomicAddDword,
      &LowerGraphLibrary::createOutputCount};

  if (m_workgraphNames.find(func->getName()) != m_workgraphNames.end()) {
    func->setLinkage(GlobalValue::WeakAnyLinkage);
    return;
  }
  auto funcIt = m_extFuncNames.find(func->getName());

  if (funcIt != m_extFuncNames.end()) {
    auto funcIdx = funcIt->second;
    (this->*amdLibraryFuncs[funcIdx])(func, funcIdx);
    return;
  }

  auto &commonFuncTable = InternalLibraryIntrinsicUtil::LibraryFunctionTable::get().m_libFuncPtrs;
  auto commonFuncIt = commonFuncTable.find(func->getName());
  if (commonFuncIt != commonFuncTable.end()) {
    auto funcPtr = commonFuncIt->second;
    m_builder->SetInsertPoint(clearBlock(func));
    (*funcPtr)(func, m_builder);
  }
}

// =====================================================================================================================
// Create Backing store
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createBackingStore(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::BackingStore);
  m_builder->SetInsertPoint(clearBlock(func));
  m_builder->CreateRet(m_builder->CreateReadBuiltInInput(lgc::BuiltInGraphControlStruct));
}

// =====================================================================================================================
// Create Shader Directory
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createShaderDirectory(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::ShaderDirectory);
  m_builder->SetInsertPoint(clearBlock(func));
  m_builder->CreateRet(m_builder->CreateReadBuiltInInput(lgc::BuiltInShaderDirectory));
}

// =====================================================================================================================
// Create Node Dispatch Info1
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createNodeDispatchInfo1(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::NodeDispatchInfo1);
  m_builder->SetInsertPoint(clearBlock(func));
  m_builder->CreateRet(m_builder->CreateReadBuiltInInput(lgc::BuiltInNodeDispatchInfo1));
}

// =====================================================================================================================
// Create Node Dispatch Info2
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createNodeDispatchInfo2(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::NodeDispatchInfo2);
  m_builder->SetInsertPoint(clearBlock(func));
  m_builder->CreateRet(m_builder->CreateReadBuiltInInput(lgc::BuiltInNodeDispatchInfo2));
}

// =====================================================================================================================
// Create Trace Buffer
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createTraceBuffer(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::TraceBuffer);
  m_builder->SetInsertPoint(clearBlock(func));
  m_builder->CreateRet(m_builder->CreateReadBuiltInInput(lgc::BuiltInWorkGraphTraceBuf));
}

// =====================================================================================================================
// Create Load DWORD from lds
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createLdsLoadDword(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::LdsLoadDword);
  // AmdWorkGraphsLdsLoadDword(uint offset) in byte
  m_builder->SetInsertPoint(clearBlock(func));
  Value *offset = func->getArg(0);
  offset = m_builder->CreateLoad(m_builder->getInt32Ty(), offset);
  // convert offset from BYTE to DWORD
  offset = m_builder->CreateLShr(offset, 2);
  auto graphLds = m_builder->create<wg::GraphGetLdsOp>();
  auto ldsPtr = m_builder->CreateGEP(m_builder->getInt32Ty(), graphLds, {offset});
  // Load value from lds position
  Value *ldsValue = m_builder->CreateLoad(m_builder->getInt32Ty(), ldsPtr);
  m_builder->CreateRet(ldsValue);
}

// =====================================================================================================================
// Create store DWORD to lds
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createLdsStoreDword(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::LdsStoreDword);
  // void AmdWorkGraphsLdsStoreDword(uint offset, uint value)
  m_builder->SetInsertPoint(clearBlock(func));
  Value *offset = func->getArg(0);
  offset = m_builder->CreateLoad(m_builder->getInt32Ty(), offset);
  // convert offset from BYTE to DWORD
  offset = m_builder->CreateLShr(offset, 2);
  Value *value = func->getArg(1);
  value = m_builder->CreateLoad(m_builder->getInt32Ty(), value);
  auto graphLds = m_builder->create<wg::GraphGetLdsOp>();
  auto ldsPtr = m_builder->CreateGEP(m_builder->getInt32Ty(), graphLds, {offset});
  m_builder->CreateStore(value, ldsPtr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Create atomic add DWORD to lds
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createLdsAtomicAddDword(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::LdsAtomicAddDword);
  // AmdWorkGraphsLdsAtomicAddDword(uint offset, uint value)
  m_builder->SetInsertPoint(clearBlock(func));
  Value *offset = func->getArg(0);
  offset = m_builder->CreateLoad(m_builder->getInt32Ty(), offset);
  // convert offset from BYTE to DWORD
  offset = m_builder->CreateLShr(offset, 2);
  Value *value = func->getArg(1);
  value = m_builder->CreateLoad(m_builder->getInt32Ty(), value);
  auto graphLds = m_builder->create<wg::GraphGetLdsOp>();
  auto ldsPtr = m_builder->CreateGEP(m_builder->getInt32Ty(), graphLds, {offset});
  m_builder->CreateAtomicRMW(AtomicRMWInst::Add, ldsPtr, value, MaybeAlign(), AtomicOrdering::Monotonic,
                             SyncScope::System);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Create output count
//
// @param func : The function to process
// @param funcId : The function ID
void LowerGraphLibrary::createOutputCount(Function *func, unsigned funcId) {
  assert(funcId == AmdExtFunc::OutputCount);
  // uint AmdWorkgraphsOutputCount()
  m_builder->SetInsertPoint(clearBlock(func));
  auto outputCount = m_builder->create<wg::OutputCountOp>();
  m_builder->CreateRet(outputCount);
}
