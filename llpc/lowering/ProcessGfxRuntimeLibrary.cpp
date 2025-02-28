/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "LowerInternalLibraryIntrinsic.h"
#include "LoweringUtil.h"
#include "compilerutils/ArgPromotion.h"
#include "compilerutils/TypesMetadata.h"
#include "lgc/Builder.h"
#include "lgc/LgcDialect.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "process-gfxruntime-library"
using namespace lgc;
using namespace llvm;

namespace Llpc {
ProcessGfxRuntimeLibrary::ProcessGfxRuntimeLibrary() {
}

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
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
  m_libFuncPtrs["AmdAdvancedBlendTexelLoadMsaa"] = &ProcessGfxRuntimeLibrary::createTexelLoadMsaa;
  m_libFuncPtrs["AmdAdvancedBlendCoherentTexelLoad"] = &ProcessGfxRuntimeLibrary::createCoherentTexelLoad;
  m_libFuncPtrs["AmdAdvancedBlendCoherentTexelStore"] = &ProcessGfxRuntimeLibrary::createCoherentTexelStore;
  m_libFuncPtrs["AmdAdvancedBlendCoherentTexelLoadMsaa"] = &ProcessGfxRuntimeLibrary::createCoherentTexelLoadMsaa;
  m_libFuncPtrs["AmdAdvancedBlendCoherentTexelStoreMsaa"] = &ProcessGfxRuntimeLibrary::createCoherentTexelStoreMsaa;
}

// =====================================================================================================================
// Clear the block before lowering the function
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
    func = compilerutils::promotePointerArguments(func, promotionMask);
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
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::createTexelLoad(Function *func) {
  loadTexel(func, false, false);
}

// =====================================================================================================================
// Create texel load with fmask
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::createTexelLoadMsaa(Function *func) {
  // Argument: imageDescMs, fmaskDesc, icoord, sampleNum
  constexpr unsigned argCount = 4;
  Type *coordTy = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {m_builder->getInt64Ty(), m_builder->getInt64Ty(), coordTy, m_builder->getInt32Ty()};
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
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::createCoherentTexelLoad(Function *func) {
  m_builder->create<PopsBeginInterlockOp>();
  loadTexel(func, false, true);
}

// =====================================================================================================================
// Create coherent texel store
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::createCoherentTexelStore(Function *func) {
  storeTexel(func, false, true);
  m_builder->create<PopsEndInterlockOp>();
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Create coherent texel Load with multi-sampling
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::createCoherentTexelLoadMsaa(Function *func) {
  m_builder->create<PopsBeginInterlockOp>();
  loadTexel(func, true, true);
}

// =====================================================================================================================
// Create coherent texel store with multi-sampling
//
// @param func : The function to process
void ProcessGfxRuntimeLibrary::createCoherentTexelStoreMsaa(Function *func) {
  storeTexel(func, true, true);
  m_builder->create<PopsEndInterlockOp>();
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Perform texel load with or without ROV supported
//
// @param func : The function to process
// @param isMsaa : Whether it is multi-sampling
// @param enableRov : Whether ROV is enabled
void ProcessGfxRuntimeLibrary::loadTexel(Function *func, bool isMsaa, bool enableRov) {
  // Argument: desc, icoord, sampleId
  constexpr unsigned argCount = 3;
  unsigned coordCount = enableRov ? 3 : 2;
  Type *coordTy = FixedVectorType::get(m_builder->getInt32Ty(), coordCount);
  Value *coord = PoisonValue::get(coordTy);

  Type *int2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {m_builder->getInt64Ty(), int2Ty, m_builder->getInt32Ty()};
  std::array<Value *, argCount> loadArgs;
  for (unsigned i = 0; i < argCount; ++i)
    loadArgs[i] = m_builder->CreateLoad(argTypes[i], func->getArg(i));

  unsigned dim = isMsaa ? Builder::Dim2DMsaa : Builder::Dim2D;
  unsigned imageFlag = Builder::ImageFlagInvariant | Builder::ImageFlagNotAliased | Builder::ImageFlagCoherent;
  loadArgs[0] = m_builder->CreateIntToPtr(loadArgs[0], PointerType::get(m_builder->getContext(), ADDR_SPACE_CONST));
  if (enableRov) {
    // (icood.x, icoord.y, icoord.z) = (loadArgs[1].x, loadArgs[1].y, sampleId)
    coord = m_builder->CreateInsertElement(coord, m_builder->CreateExtractElement(loadArgs[1], m_builder->getInt32(0)),
                                           static_cast<uint64_t>(0));
    coord =
        m_builder->CreateInsertElement(coord, m_builder->CreateExtractElement(loadArgs[1], m_builder->getInt32(1)), 1);
    coord = m_builder->CreateInsertElement(coord, loadArgs[2], 2);
  } else {
    coord = loadArgs[1];
  }

  auto imageLoad = m_builder->CreateImageLoad(func->getReturnType(), dim, imageFlag, loadArgs[0], coord, nullptr);
  m_builder->CreateRet(imageLoad);
}

// =====================================================================================================================
// Perform texel store with or without ROV supported
//
// @param func : The function to process
// @param isMsaa : Whether it is multi-sampling
// @param enableRov : Whether ROV is enabled
void ProcessGfxRuntimeLibrary::storeTexel(Function *func, bool isMsaa, bool enableRov) {
  // Argument: texel, desc, icoord, sampleId
  constexpr unsigned argCount = 4;
  unsigned coordCount = enableRov ? 3 : 2;
  Type *coordTy = FixedVectorType::get(m_builder->getInt32Ty(), coordCount);
  Value *coord = PoisonValue::get(coordTy);
  Type *texelTy = FixedVectorType::get(m_builder->getFloatTy(), 4);

  Type *int2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  Type *argTypes[] = {texelTy, m_builder->getInt64Ty(), int2Ty, m_builder->getInt32Ty()};
  std::array<Value *, argCount> loadArgs;
  for (unsigned i = 0; i < argCount; ++i)
    loadArgs[i] = m_builder->CreateLoad(argTypes[i], func->getArg(i));

  unsigned dim = isMsaa ? Builder::Dim2DMsaa : Builder::Dim2D;
  unsigned imageFlag = Builder::ImageFlagInvariant | Builder::ImageFlagNotAliased | Builder::ImageFlagCoherent;
  loadArgs[1] = m_builder->CreateIntToPtr(loadArgs[1], PointerType::get(m_builder->getContext(), ADDR_SPACE_CONST));
  if (enableRov) {
    // (icood.x, icoord.y, icoord.z) = (loadArgs[2].x, loadArgs[2].y, sampleId)
    coord = m_builder->CreateInsertElement(coord, m_builder->CreateExtractElement(loadArgs[2], m_builder->getInt32(0)),
                                           static_cast<uint64_t>(0));
    coord =
        m_builder->CreateInsertElement(coord, m_builder->CreateExtractElement(loadArgs[2], m_builder->getInt32(1)), 1);
    coord = m_builder->CreateInsertElement(coord, loadArgs[3], 2);
  } else {
    coord = loadArgs[2];
  }
  m_builder->CreateImageStore(loadArgs[0], dim, imageFlag, loadArgs[1], coord, nullptr);
}

} // namespace Llpc
