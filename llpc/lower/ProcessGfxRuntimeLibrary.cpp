/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ProcessGfxRuntimeLibrary.cpp
 * @brief LLPC source file: contains implementation of class Llpc::ProcessGfxRuntimeLibrary.
 ***********************************************************************************************************************
 */
#include "ProcessGfxRuntimeLibrary.h"
#include "compilerutils/ArgPromotion.h"
#include "compilerutils/TypesMetadata.h"
#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "process-gfxruntime-library"
using namespace lgc;
using namespace llvm;

namespace Llpc {
ProcessGfxRuntimeLibrary::ProcessGfxRuntimeLibrary() {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses ProcessGfxRuntimeLibrary::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-gfxruntime-library\n");
  SpirvLower::init(&module);
  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    processLibraryFunction(func);
  }

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Initialize library function pointer table
ProcessGfxRuntimeLibrary::LibraryFunctionTable::LibraryFunctionTable() {
  m_libFuncPtrs["AmdAdvancedBlendTexelLoad"] = &ProcessGfxRuntimeLibrary::createTexelLoad;
  m_libFuncPtrs["AmdAdvancedBlendTexelLoadFmask"] = &ProcessGfxRuntimeLibrary::createTexelLoadFmask;
  m_libFuncPtrs["AmdAdvancedBlendCoherentTexelLoad"] = &ProcessGfxRuntimeLibrary::createCoherentTexelLoad;
  m_libFuncPtrs["AmdAdvancedBlendCoherentTexelStore"] = &ProcessGfxRuntimeLibrary::createCoherentTexelStore;
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::processLibraryFunction(Function *&func) {
  auto funcName = func->getName();

  static const char *AdvancedBlendInternalName = "AmdAdvancedBlendInternal";
  if (funcName.starts_with(AdvancedBlendInternalName)) {
    func->setLinkage(GlobalValue::ExternalLinkage);
    SmallBitVector promotionMask(func->arg_size());
    for (unsigned argId = 0; argId < func->arg_size(); ++argId) {
      auto *arg = func->getArg(argId);
      TypedArgTy argTy = TypedArgTy::get(arg);
      if (!argTy.isPointerTy())
        continue;
      promotionMask.set(argId);
    }
    func = CompilerUtils::promotePointerArguments(func, promotionMask);
    return;
  }

  auto gfxruntimeFuncTable = LibraryFunctionTable::get().m_libFuncPtrs;
  auto gfxruntimeFuncIt = gfxruntimeFuncTable.find(funcName);
  if (gfxruntimeFuncIt != gfxruntimeFuncTable.end()) {
    auto funcPtr = gfxruntimeFuncIt->second;
    m_builder->SetInsertPoint(clearBlock(func));
    (this->*funcPtr)(func);
    return;
  }

  auto &commonFuncTable = InternalLibraryIntrinsicUtil::LibraryFunctionTable::get().m_libFuncPtrs;
  auto commonFuncIt = commonFuncTable.find(funcName);
  if (commonFuncIt != commonFuncTable.end()) {
    auto funcPtr = commonFuncIt->second;
    m_builder->SetInsertPoint(clearBlock(func));
    (*funcPtr)(func, m_builder);
    return;
  }
}

// =====================================================================================================================
// Create texel load
void ProcessGfxRuntimeLibrary::createTexelLoad(Function *func) {
  // Arguments: imageDesc, icoord, lod
  constexpr unsigned argCount = 3;
  Type *int2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {m_builder->getInt64Ty(), int2Ty, m_builder->getInt32Ty()};
  std::array<Value *, argCount> loadArgs;
  for (unsigned i = 0; i < argCount; ++i)
    loadArgs[i] = m_builder->CreateLoad(argTypes[i], func->getArg(i));
  unsigned imageFlag = Builder::ImageFlagInvariant | Builder::ImageFlagNotAliased;
  loadArgs[0] = m_builder->CreateIntToPtr(loadArgs[0], PointerType::get(m_builder->getContext(), ADDR_SPACE_CONST));
  auto imageLoad = m_builder->CreateImageLoad(func->getReturnType(), Builder::Dim2D, imageFlag, loadArgs[0],
                                              loadArgs[1], loadArgs[2]);
  m_builder->CreateRet(imageLoad);
}

// =====================================================================================================================
// Create texel load with fmask
void ProcessGfxRuntimeLibrary::createTexelLoadFmask(Function *func) {
  // Argument: imageDescMs, fmaskDesc, icoord, lod
  constexpr unsigned argCount = 4;
  Type *int2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {m_builder->getInt64Ty(), m_builder->getInt64Ty(), int2Ty, m_builder->getInt32Ty()};
  std::array<Value *, argCount> loadArgs;
  for (unsigned i = 0; i < argCount; ++i)
    loadArgs[i] = m_builder->CreateLoad(argTypes[i], func->getArg(i));
  unsigned imageFlag = Builder::ImageFlagInvariant | Builder::ImageFlagNotAliased;
  loadArgs[0] = m_builder->CreateIntToPtr(loadArgs[0], PointerType::get(m_builder->getContext(), ADDR_SPACE_CONST));
  loadArgs[1] = m_builder->CreateIntToPtr(loadArgs[1], PointerType::get(m_builder->getContext(), ADDR_SPACE_CONST));
  auto imageLoad = m_builder->CreateImageLoadWithFmask(func->getReturnType(), Builder::Dim2DMsaa, imageFlag,
                                                       loadArgs[0], loadArgs[1], loadArgs[2], loadArgs[3]);
  m_builder->CreateRet(imageLoad);
}

// =====================================================================================================================
// Create coherent texel Load
void ProcessGfxRuntimeLibrary::createCoherentTexelLoad(Function *func) {
  // Argument: inColor, icoord, sampleId
  constexpr unsigned argCount = 3;
  Type *Float4Ty = FixedVectorType::get(m_builder->getFloatTy(), 4);
  Type *int2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {Float4Ty, int2Ty, m_builder->getInt32Ty()};
  std::array<Value *, argCount> loadArgs;
  for (unsigned i = 0; i < argCount; ++i)
    loadArgs[i] = m_builder->CreateLoad(argTypes[i], func->getArg(i));
  // TODO: Implement load texel based on ROV
  m_builder->CreateRet(loadArgs[0]);
}

// =====================================================================================================================
// Create coherent texel store
void ProcessGfxRuntimeLibrary::createCoherentTexelStore(Function *func) {
  // Argument: inColor, icoord, sampleId
  constexpr unsigned argCount = 3;
  Type *Float4Ty = FixedVectorType::get(m_builder->getFloatTy(), 4);
  Type *int2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {Float4Ty, int2Ty, m_builder->getInt32Ty()};
  std::array<Value *, argCount> storeArgs;
  for (unsigned i = 0; i < argCount; ++i)
    storeArgs[i] = m_builder->CreateLoad(argTypes[i], func->getArg(i));
  // TODO: Implement store texel based on ROV
  m_builder->CreateRetVoid();
}

} // namespace Llpc
