/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ProcessGpuRtLibrary.cpp
 * @brief LLPC source file: contains implementation of class Llpc::ProcessGpuRtLibrary.
 ***********************************************************************************************************************
 */
#include "ProcessGpuRtLibrary.h"
#include "LowerInternalLibraryIntrinsic.h"
#include "LoweringUtil.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "compilerutils/ArgPromotion.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypesMetadata.h"
#include "llvmraytracing/Continuations.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/ValueSymbolTable.h"

#define DEBUG_TYPE "lower-gpurt-library"
using namespace lgc;
using namespace llvm;
using namespace lgc::rt;

namespace Llpc {
ProcessGpuRtLibrary::ProcessGpuRtLibrary(const GpurtKey &key) : m_gpurtKey(key) {
}

// =====================================================================================================================
// Executes this FE lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses ProcessGpuRtLibrary::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-gpurt-library\n");
  Lowering::init(&module);

  // Imbue the module with settings from the GPURT key.
  ContHelper::setStackAddrspace(module, m_gpurtKey.rtPipeline.cpsFlags & Vkgc::CpsFlag::CpsFlagStackInGlobalMem
                                            ? ContStackAddrspace::GlobalLLPC
                                            : ContStackAddrspace::ScratchLLPC);

  // The version is encoded as <major><minor> in decimal digits, so 11 is rtip 1.1, 20 is rtip 2.0
  ContHelper::setRtip(module, m_gpurtKey.rtipVersion.major * 10 + m_gpurtKey.rtipVersion.minor);

  SmallVector<ContSetting> contSettings;
  for (auto &option : m_gpurtKey.rtPipeline.options) {
    ContSetting setting;
    setting.NameHash = option.nameHash;
    setting.Value = option.value;
    contSettings.push_back(setting);
  }
  ContHelper::setGpurtSettings(module, contSettings);

  // Process each function.
  SmallVector<std::pair<Function *, SmallBitVector>> argPromotionsFuncs;
  SmallVector<Function *> maybeRtFuncs;
  for (Function &func : module) {
    if (func.isDeclaration() || !func.hasName())
      continue;
    // We have a function definition that was not left anonymous by being overridden by an earlier
    // RTIP-suffixed version of the same function.

    // For rayQuery functions, we detect which ones we want to keep, and we select the correct RTIP variant.
    // TODO: Use the same scheme for ray-tracing functions so we no longer need the GPURT-provided function
    // name table that the driver passes in to the compiler.
    // Detect a rayQuery function. If it needs pointer args promoting, set a bit vector for that.
    StringRef funcName = func.getName();
    SmallBitVector argPromotions(/*size=*/8);
    bool isRqFunc = false;
    if (funcName.starts_with("_RayQuery_TraceRayInline"))
      argPromotions.set(1, 8);
    else if (funcName.starts_with("_RayQuery_Proceed"))
      argPromotions.set(1, 3);
    else if (funcName.starts_with("_RayQuery_FetchTrianglePosition"))
      argPromotions.set(1);
    else {
      StringRef rqFuncName = funcName;
      isRqFunc = rqFuncName.consume_front("_RayQuery_");
      if (isRqFunc && rqFuncName.starts_with("CommitProceduralPrimitiveHit"))
        argPromotions.set(1);
    }
    isRqFunc |= argPromotions.any();

    if (!isRqFunc) {
      // This is not a rayQuery function. Add to the list for processing after this loop.
      maybeRtFuncs.push_back(&func);
      continue;
    }
    if (argPromotions.any()) {
      // Add this function to the list that need arg promotion.
      // We don't do the arg promotion here as it invalidates the module iterator.
      // Also, we might end up not needing to do it for a non-RTIP-suffixed function that gets overridden
      // by an RTIP-suffixed function later in the loop.
      argPromotionsFuncs.push_back({&func, argPromotions});
    }
  }

  // Promote args on functions as required. Skip overridden non-RTIP-suffixed ones that have gone back to
  // being internal linkage.
  for (const std::pair<Function *, SmallBitVector> &argPromotionsFunc : argPromotionsFuncs) {
    Function *func = argPromotionsFunc.first;
    if (func->getLinkage() == GlobalValue::InternalLinkage)
      continue;
    compilerutils::promotePointerArguments(func, argPromotionsFunc.second);
  }

  // Process ray-tracing (i.e. non-rayQuery) functions in a separate loop; processLibraryFunction() may do
  // arg promotion, so we cannot do it in the same loop.
  // Skip the processed functions so the leftover can be argument-promoted by earlyGpurtTransform.
  SmallVector<Function *> promotableFunctions;
  for (Function *func : maybeRtFuncs) {
    if (!processLibraryFunction(func))
      promotableFunctions.push_back(func);
  }

  // Implement builtins whose implementation is generic, i.e. not specific to LGC.
  // Do not use the return value of `earlyGpurtTransform` since "Changed" would be trivially true in this pass.
  earlyGpurtTransform(module, promotableFunctions, /*PreserveWaitMasks = */ false);

  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Initialize library function pointer table
ProcessGpuRtLibrary::LibraryFunctionTable::LibraryFunctionTable() {
  m_libFuncPtrs["AmdTraceRayGetStackSize"] = &ProcessGpuRtLibrary::createGetStackSize;
  m_libFuncPtrs["AmdTraceRayLdsRead"] = &ProcessGpuRtLibrary::createLdsRead;
  m_libFuncPtrs["AmdTraceRayLdsWrite"] = &ProcessGpuRtLibrary::createLdsWrite;
  m_libFuncPtrs["AmdTraceRayGetStackBase"] = &ProcessGpuRtLibrary::createGetStackBase;
  m_libFuncPtrs["AmdTraceRayGetStackStride"] = &ProcessGpuRtLibrary::createGetStackStride;
  m_libFuncPtrs["AmdTraceRayLdsStackInit"] = &ProcessGpuRtLibrary::createLdsStackInit;
  m_libFuncPtrs["AmdTraceRayLdsStackStore"] = &ProcessGpuRtLibrary::createLdsStackStore;
  m_libFuncPtrs["AmdTraceRayGetBoxSortHeuristicMode"] = &ProcessGpuRtLibrary::createGetBoxSortHeuristicMode;
  m_libFuncPtrs["AmdTraceRayGetStaticFlags"] = &ProcessGpuRtLibrary::createGetStaticFlags;
  m_libFuncPtrs["AmdTraceRayGetTriangleCompressionMode"] = &ProcessGpuRtLibrary::createGetTriangleCompressionMode;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddr"] = &ProcessGpuRtLibrary::createLoadDwordAtAddr;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx2"] = &ProcessGpuRtLibrary::createLoadDwordAtAddrx2;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx3"] = &ProcessGpuRtLibrary::createLoadDwordAtAddrx3;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx4"] = &ProcessGpuRtLibrary::createLoadDwordAtAddrx4;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConstantLoadDwordAtAddr"] =
      &ProcessGpuRtLibrary::createConstantLoadDwordAtAddr;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConstantLoadDwordAtAddrx2"] =
      &ProcessGpuRtLibrary::createConstantLoadDwordAtAddrx2;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConstantLoadDwordAtAddrx4"] =
      &ProcessGpuRtLibrary::createConstantLoadDwordAtAddrx4;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConvertF32toF16NegInf"] = &ProcessGpuRtLibrary::createConvertF32toF16NegInf;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConvertF32toF16PosInf"] = &ProcessGpuRtLibrary::createConvertF32toF16PosInf;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_IntersectInternal"] = &ProcessGpuRtLibrary::createIntersectBvh;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ShaderMarker"] = &ProcessGpuRtLibrary::createShaderMarker;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_WaveScan"] = &ProcessGpuRtLibrary::createWaveScan;
  m_libFuncPtrs["AmdTraceRayDualIntersectRay"] = &ProcessGpuRtLibrary::createDualIntersectRay;
  m_libFuncPtrs["AmdTraceRayIntersectRayBvh8"] = &ProcessGpuRtLibrary::createIntersectRayBvh8;
  m_libFuncPtrs["AmdTraceRayDsStackPush8Pop1"] = &ProcessGpuRtLibrary::createDsStackPush8Pop1;
  m_libFuncPtrs["AmdTraceRayDsStackPush8Pop2"] = &ProcessGpuRtLibrary::createDsStackPush8Pop2;
  m_libFuncPtrs["AmdTraceRayDsStackPush8Pop1PrimRangeEnabled"] =
      &ProcessGpuRtLibrary::createDsStackPush8Pop1PrimRangeEnabled;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_FloatOpWithRoundMode"] = &ProcessGpuRtLibrary::createFloatOpWithRoundMode;
  m_libFuncPtrs["AmdExtDispatchThreadIdFlat"] = &ProcessGpuRtLibrary::createDispatchThreadIdFlat;
  m_libFuncPtrs["AmdTraceRaySampleGpuTimer"] = &ProcessGpuRtLibrary::createSampleGpuTimer;
  m_libFuncPtrs["AmdTraceRayGetFlattenedGroupThreadId"] = &ProcessGpuRtLibrary::createGetFlattenedGroupThreadId;
  m_libFuncPtrs["AmdTraceRayGetHitAttributes"] = &ProcessGpuRtLibrary::createGetHitAttributes;
  m_libFuncPtrs["AmdTraceRaySetHitAttributes"] = &ProcessGpuRtLibrary::createSetHitAttributes;
  m_libFuncPtrs["AmdTraceRaySetTraceParams"] = &ProcessGpuRtLibrary::createSetTraceParams;
  m_libFuncPtrs["AmdTraceRayCallClosestHitShader"] = &ProcessGpuRtLibrary::createCallClosestHitShader;
  m_libFuncPtrs["AmdTraceRayCallMissShader"] = &ProcessGpuRtLibrary::createCallMissShader;
  m_libFuncPtrs["AmdTraceRayCallTriangleAnyHitShader"] = &ProcessGpuRtLibrary::createCallTriangleAnyHitShader;
  m_libFuncPtrs["AmdTraceRayCallIntersectionShader"] = &ProcessGpuRtLibrary::createCallIntersectionShader;
  m_libFuncPtrs["AmdTraceRaySetTriangleIntersectionAttributes"] =
      &ProcessGpuRtLibrary::createSetTriangleIntersectionAttributes;
  m_libFuncPtrs["AmdTraceRaySetHitTriangleNodePointer"] = &ProcessGpuRtLibrary::createSetHitTriangleNodePointer;
  m_libFuncPtrs["AmdTraceRayGetParentId"] = &ProcessGpuRtLibrary::createGetParentId;
  m_libFuncPtrs["AmdTraceRaySetParentId"] = &ProcessGpuRtLibrary::createSetParentId;
  m_libFuncPtrs["AmdTraceRayDispatchRaysIndex"] = &ProcessGpuRtLibrary::createDispatchRayIndex;
  m_libFuncPtrs["AmdTraceRayGetStaticId"] = &ProcessGpuRtLibrary::createGetStaticId;
  m_libFuncPtrs["AmdTraceRayInitStaticId"] = &ProcessGpuRtLibrary::createInitStaticId;
  m_libFuncPtrs["AmdTraceRayGetKnownSetRayFlags"] = &ProcessGpuRtLibrary::createGetKnownSetRayFlags;
  m_libFuncPtrs["AmdTraceRayMakePC"] = &ProcessGpuRtLibrary::createMakePc;
  m_libFuncPtrs["AmdTraceRayGetKnownUnsetRayFlags"] = &ProcessGpuRtLibrary::createGetKnownUnsetRayFlags;
  m_libFuncPtrs["_AmdContStackAlloc"] = &ProcessGpuRtLibrary::createContStackAlloc;
  m_libFuncPtrs["_AmdContStackFree"] = &ProcessGpuRtLibrary::createContStackFree;
  m_libFuncPtrs["_AmdContStackGetPtr"] = &ProcessGpuRtLibrary::createContStackGetPtr;
  m_libFuncPtrs["_AmdContStackSetPtr"] = &ProcessGpuRtLibrary::createContStackSetPtr;
  m_libFuncPtrs["_AmdIsLlpc"] = &ProcessGpuRtLibrary::createIsLlpc;
  m_libFuncPtrs["_AmdGetShaderRecordIndex"] = &ProcessGpuRtLibrary::createGetShaderRecordIndex;
}

// =====================================================================================================================
// Clear the block before lowering the function
//
// @param func : The function to process
// @ret: Returns whether the function has been processed.
bool ProcessGpuRtLibrary::processLibraryFunction(Function *&func) {
  auto funcName = func->getName();

  // Special handling for _AmdContStackStore* and _AmdContStackLoad* to accept arbitrary type
  if (funcName.starts_with("_AmdContStackStore")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createContStackStore(func);
    return true;
  }

  if (funcName.starts_with("_AmdContStackLoad")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createContStackLoad(func);
    return true;
  }

  if (funcName.starts_with("_AmdEnqueue") || funcName.starts_with("_AmdWaitEnqueue")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createEnqueue(func);
    return true;
  }

  if (funcName.starts_with("_AmdValueGetI32") || funcName.starts_with("_AmdValueSetI32")) {
    // The intrinsic handling require first argument to be a pointer, the rest to be values.
    SmallBitVector promotionMask(func->arg_size(), true);
    promotionMask.reset(0);
    compilerutils::promotePointerArguments(func, promotionMask);
    return true;
  }

  // Create implementation for intrinsic functions.
  auto &gpurtFuncTable = LibraryFunctionTable::get().m_libFuncPtrs;
  auto gpurtFuncIt = gpurtFuncTable.find(funcName);
  if (gpurtFuncIt != gpurtFuncTable.end()) {
    auto funcPtr = gpurtFuncIt->second;
    m_builder->SetInsertPoint(clearBlock(func));
    (this->*funcPtr)(func);
    return true;
  }

  auto &commonFuncTable = InternalLibraryIntrinsicUtil::LibraryFunctionTable::get().m_libFuncPtrs;
  auto commonFuncIt = commonFuncTable.find(funcName);
  if (commonFuncIt != commonFuncTable.end()) {
    auto funcPtr = commonFuncIt->second;
    m_builder->SetInsertPoint(clearBlock(func));
    (*funcPtr)(func, m_builder);
    return true;
  }

  // NOTE: GPURT now preserves all function names started with "_Amd", but some of them are not intrinsics, e.g.,
  // "_AmdSystemData.IsTraversal", which are methods of system data structs. Skip those to let them be inlined
  // automatically.
  const bool isAmdIntrinsic = funcName.starts_with("_Amd") && !funcName.contains(".");
  if (funcName.contains("_cont_") || isAmdIntrinsic) {
    if (!isAmdIntrinsic)
      func->setLinkage(GlobalValue::WeakAnyLinkage);

    return false;
  }

  return true;
}

// =====================================================================================================================
// Fill in function to get stack size
//
// @param func : The function to process
void ProcessGpuRtLibrary::createGetStackSize(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackSizeOp>());
}

// =====================================================================================================================
// Fill in function to get stack base
//
// @param func : The function to process
void ProcessGpuRtLibrary::createGetStackBase(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackBaseOp>());
}

// =====================================================================================================================
// Fill in function to write LDS stack
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLdsWrite(Function *func) {
  auto argIt = func->arg_begin();
  auto int32ty = m_builder->getInt32Ty();
  Value *stackOffset = m_builder->CreateLoad(int32ty, argIt++);
  Value *stackData = m_builder->CreateLoad(int32ty, argIt);
  m_builder->CreateRet(m_builder->create<GpurtStackWriteOp>(stackOffset, stackData, false));
}

// =====================================================================================================================
// Fill in function to read LDS stack
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLdsRead(Function *func) {
  Value *stackIndex = func->arg_begin();
  stackIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), stackIndex);
  m_builder->CreateRet(m_builder->create<GpurtStackReadOp>(stackIndex, false));
}

// =====================================================================================================================
// Fill in function to get stack stride
//
// @param func : The function to process
void ProcessGpuRtLibrary::createGetStackStride(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackStrideOp>());
}

// =====================================================================================================================
// Fill in function to init stack LDS
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLdsStackInit(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtLdsStackInitOp>(false));
}

// =====================================================================================================================
void ProcessGpuRtLibrary::createFloatOpWithRoundMode(llvm::Function *func) {
  auto argIt = func->arg_begin();
  auto retType = cast<FixedVectorType>(func->getReturnType());
  auto int32Ty = m_builder->getInt32Ty();
  Value *roundMode = m_builder->CreateLoad(int32Ty, argIt++);
  Value *operation = m_builder->CreateLoad(int32Ty, argIt++);
  Value *src0 = m_builder->CreateLoad(retType, argIt++);
  Value *src1 = m_builder->CreateLoad(retType, argIt);
  m_builder->CreateRet(m_builder->create<GpurtFloatWithRoundModeOp>(roundMode, operation, src0, src1));
}

// =====================================================================================================================
// Fill in function to store stack LDS
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLdsStackStore(Function *func) {
  auto argIt = func->arg_begin();
  Value *stackAddr = argIt++;
  Value *stackAddrPos = m_builder->CreateLoad(m_builder->getInt32Ty(), stackAddr);
  Value *lastVisited = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  Value *data = m_builder->CreateLoad(int32x4Ty, argIt);
  auto ret = m_builder->create<GpurtLdsStackStoreOp>(stackAddrPos, lastVisited, data);
  Value *newStackPos = m_builder->CreateExtractValue(ret, 1);
  m_builder->CreateStore(newStackPos, stackAddr);
  m_builder->CreateRet(m_builder->CreateExtractValue(ret, 0));
}

// =====================================================================================================================
// Fill in function to get box sort heuristic mode
//
// @param func : The function to process
void ProcessGpuRtLibrary::createGetBoxSortHeuristicMode(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetBoxSortHeuristicModeOp>());
}

// =====================================================================================================================
// Fill in function to get static flags
//
// @param func : The function to process
void ProcessGpuRtLibrary::createGetStaticFlags(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStaticFlagsOp>());
}

// =====================================================================================================================
// Fill in function to get triangle compression mode
//
// @param func : The function to process
void ProcessGpuRtLibrary::createGetTriangleCompressionMode(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetTriangleCompressionModeOp>());
}

// =====================================================================================================================
// Fill in function to global load 1 dword at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLoadDwordAtAddr(Function *func) {
  createLoadDwordAtAddrWithType(func, m_builder->getInt32Ty(), SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to global load 2 dwords at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLoadDwordAtAddrx2(Function *func) {
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  createLoadDwordAtAddrWithType(func, int32x2Ty, SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to global load 3 dwords at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLoadDwordAtAddrx3(Function *func) {
  auto int32x3Ty = FixedVectorType::get(m_builder->getInt32Ty(), 3);
  createLoadDwordAtAddrWithType(func, int32x3Ty, SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to global load 4 dwords at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createLoadDwordAtAddrx4(Function *func) {
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  createLoadDwordAtAddrWithType(func, int32x4Ty, SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to constant load 1 dword at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createConstantLoadDwordAtAddr(Function *func) {
  createLoadDwordAtAddrWithType(func, m_builder->getInt32Ty(), SPIRAS_Constant);
}

// =====================================================================================================================
// Fill in function to constant load 2 dwords at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createConstantLoadDwordAtAddrx2(Function *func) {
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  createLoadDwordAtAddrWithType(func, int32x2Ty, SPIRAS_Constant);
}

// =====================================================================================================================
// Fill in function to constant load 4 dwords at given address
//
// @param func : The function to process
void ProcessGpuRtLibrary::createConstantLoadDwordAtAddrx4(Function *func) {
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  createLoadDwordAtAddrWithType(func, int32x4Ty, SPIRAS_Constant);
}

// =====================================================================================================================
// Fill in function to load dwords at given address based on given type
//
// @param func : The function to process
// @param loadTy : Load type
void ProcessGpuRtLibrary::createLoadDwordAtAddrWithType(Function *func, Type *loadTy, SPIRAddressSpace addressSpace) {
  auto argIt = func->arg_begin();

  Value *gpuLowAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *gpuHighAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *offset = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);

  // Use (gpuLowAddr, gpuHighAddr) to calculate i64 gpuAddr
  gpuLowAddr = m_builder->CreateZExt(gpuLowAddr, m_builder->getInt64Ty());
  gpuHighAddr = m_builder->CreateZExt(gpuHighAddr, m_builder->getInt64Ty());
  gpuHighAddr = m_builder->CreateShl(gpuHighAddr, m_builder->getInt64(32));
  Value *gpuAddr = m_builder->CreateOr(gpuLowAddr, gpuHighAddr);

  Type *gpuAddrAsPtrTy = PointerType::get(m_builder->getContext(), addressSpace);
  auto gpuAddrAsPtr = m_builder->CreateIntToPtr(gpuAddr, gpuAddrAsPtrTy);

  // Create GEP to get the byte address with byte offset
  Value *loadPtr = m_builder->CreateGEP(m_builder->getInt8Ty(), gpuAddrAsPtr, offset);

  Value *loadValue = m_builder->CreateLoad(loadTy, loadPtr);
  m_builder->CreateRet(loadValue);
}

// =====================================================================================================================
// Fill in function to convert f32 to f16 with rounding toward negative
//
// @param func : The function to process
void ProcessGpuRtLibrary::createConvertF32toF16NegInf(Function *func) {
  createConvertF32toF16WithRoundingMode(func, RoundingMode::TowardNegative);
}

// =====================================================================================================================
// Fill in function to convert f32 to f16 with rounding toward positive
//
// @param func : The function to process
void ProcessGpuRtLibrary::createConvertF32toF16PosInf(Function *func) {
  createConvertF32toF16WithRoundingMode(func, RoundingMode::TowardPositive);
}

// =====================================================================================================================
// Fill in function to convert f32 to f16 with given rounding mode
//
// @param func : The function to process
// @param rm : Rounding mode
void ProcessGpuRtLibrary::createConvertF32toF16WithRoundingMode(Function *func, RoundingMode rm) {
  auto argIt = func->arg_begin();

  Type *convertInputType = FixedVectorType::get(m_builder->getFloatTy(), 3);
  Value *inVec = m_builder->CreateLoad(convertInputType, argIt);

  Value *result = m_builder->CreateFpTruncWithRounding(inVec, FixedVectorType::get(m_builder->getHalfTy(), 3), rm);

  result = m_builder->CreateBitCast(result, FixedVectorType::get(m_builder->getInt16Ty(), 3));
  result = m_builder->CreateZExt(result, FixedVectorType::get(m_builder->getInt32Ty(), 3));

  m_builder->CreateRet(result);
}

// =====================================================================================================================
// Fill in function to return bvh node intersection result
//
// @param func : The function to create
void ProcessGpuRtLibrary::createIntersectBvh(Function *func) {
  if (m_gpurtKey.bvhResDesc.size() < 4) {
    m_builder->CreateRet(PoisonValue::get(func->getReturnType()));
    return;
  }

  // Ray tracing utility function: AmdExtD3DShaderIntrinsics_IntersectInternal
  // uint4 AmdExtD3DShaderIntrinsics_IntersectInternal(
  //     in uint2  address,
  //     in float  ray_extent,
  //     in float3 ray_origin,
  //     in float3 ray_dir,
  //     in float3 ray_inv_dir,
  //     in uint   flags,
  //     in uint   expansion)
  // {
  //     bvhSrd = SET_DESCRIPTOR_BUF(pOption->bvhSrd.descriptorData)
  //     return IMAGE_BVH64_INTERSECT_RAY(address, ray_extent, ray_origin, ray_dir, ray_inv_dir, bvhSrd)
  // }

  auto argIt = func->arg_begin();
  Value *address = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), argIt);
  argIt++;

  // Address int64 type
  address = m_builder->CreateBitCast(address, m_builder->getInt64Ty());

  // Ray extent float Type
  Value *extent = m_builder->CreateLoad(m_builder->getFloatTy(), argIt);
  argIt++;

  // Ray origin vec3 Type
  Value *origin = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 3), argIt);
  argIt++;

  // Ray dir vec3 type
  Value *dir = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 3), argIt);
  argIt++;

  // Ray inv_dir vec3 type
  Value *invDir = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 3), argIt);
  argIt++;

  // uint flag
  Value *flags = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt);
  argIt++;

  // uint expansion
  Value *expansion = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt);

  Value *imageDesc = createGetBvhSrd(expansion, flags);

  m_builder->CreateRet(m_builder->CreateImageBvhIntersectRay(address, extent, origin, dir, invDir, imageDesc));
}

// =====================================================================================================================
// Create instructions to get BVH SRD given the expansion and box sort mode at the current insert point
//
// @param expansion : Box expansion
// @param boxSortMode : Box sort mode
Value *ProcessGpuRtLibrary::createGetBvhSrd(llvm::Value *expansion, llvm::Value *boxSortMode) {
  assert(m_gpurtKey.bvhResDesc.size() == 4);

  // Construct image descriptor from rtstate.
  Value *bvhSrd = PoisonValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 4));
  bvhSrd = m_builder->CreateInsertElement(bvhSrd, m_builder->getInt32(m_gpurtKey.bvhResDesc[0]), uint64_t(0));
  bvhSrd = m_builder->CreateInsertElement(bvhSrd, m_builder->getInt32(m_gpurtKey.bvhResDesc[2]), 2u);
  bvhSrd = m_builder->CreateInsertElement(bvhSrd, m_builder->getInt32(m_gpurtKey.bvhResDesc[3]), 3u);

  Value *bvhSrdDw1 = m_builder->getInt32(m_gpurtKey.bvhResDesc[1]);

  if (expansion) {
    const unsigned BvhSrdBoxExpansionShift = 23;
    const unsigned BvhSrdBoxExpansionBitCount = 8;
    // Update the box expansion ULPs field.
    bvhSrdDw1 = m_builder->CreateInsertBitField(bvhSrdDw1, expansion, m_builder->getInt32(BvhSrdBoxExpansionShift),
                                                m_builder->getInt32(BvhSrdBoxExpansionBitCount));
  }

  if (boxSortMode) {
    const unsigned BvhSrdBoxSortDisableValue = 3;
    const unsigned BvhSrdBoxSortModeShift = 21;
    const unsigned BvhSrdBoxSortModeBitCount = 2;
    const unsigned BvhSrdBoxSortEnabledFlag = 1u << 31u;
    // Update the box sort mode field.
    Value *newBvhSrdDw1 =
        m_builder->CreateInsertBitField(bvhSrdDw1, boxSortMode, m_builder->getInt32(BvhSrdBoxSortModeShift),
                                        m_builder->getInt32(BvhSrdBoxSortModeBitCount));
    // Box sort enabled, need to OR in the box sort flag at bit 31 in DWORD 1.
    newBvhSrdDw1 = m_builder->CreateOr(newBvhSrdDw1, m_builder->getInt32(BvhSrdBoxSortEnabledFlag));

    Value *boxSortEnabled = m_builder->CreateICmpNE(boxSortMode, m_builder->getInt32(BvhSrdBoxSortDisableValue));
    bvhSrdDw1 = m_builder->CreateSelect(boxSortEnabled, newBvhSrdDw1, bvhSrdDw1);
  }

  // Fill in modified DW1 to the BVH SRD.
  bvhSrd = m_builder->CreateInsertElement(bvhSrd, bvhSrdDw1, 1u);

  return bvhSrd;
}

// =====================================================================================================================
// Fill in function to sample gpu timer
//
// @param func : The function to create
void ProcessGpuRtLibrary::createSampleGpuTimer(llvm::Function *func) {
  if (func->arg_size() == 2) {
    Value *timerHiPtr = func->getArg(0);
    Value *timerLoPtr = func->getArg(1);

    Value *const readClock = m_builder->CreateReadClock(true);
    Value *clocksLo = m_builder->CreateAnd(readClock, m_builder->getInt64(UINT32_MAX));
    clocksLo = m_builder->CreateTrunc(clocksLo, m_builder->getInt32Ty());
    Value *clocksHi = m_builder->CreateLShr(readClock, m_builder->getInt64(32));
    clocksHi = m_builder->CreateTrunc(clocksHi, m_builder->getInt32Ty());

    m_builder->CreateStore(clocksLo, timerLoPtr);
    m_builder->CreateStore(clocksHi, timerHiPtr);

    m_builder->CreateRetVoid();
  } else {
    assert(func->arg_empty());
    Value *const readClock = m_builder->CreateReadClock(true);
    m_builder->CreateRet(readClock);
  }
}

// =====================================================================================================================
// Fill in function to get flattened group thread ID
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetFlattenedGroupThreadId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetFlattenedGroupThreadIdOp>());
}

// =====================================================================================================================
// Fill in function to get hit attributes
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetHitAttributes(llvm::Function *func) {
  Value *tCurrentPtr = func->getArg(0);
  Value *kindPtr = func->getArg(1);
  Value *statusPtr = func->getArg(2);
  m_builder->create<GpurtGetHitAttributesOp>(tCurrentPtr, kindPtr, statusPtr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to set hit attributes
//
// @param func : The function to create
void ProcessGpuRtLibrary::createSetHitAttributes(llvm::Function *func) {
  Value *tCurrent = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(0));
  Value *kind = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  Value *status = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(2));
  Value *instNodeAddrLo = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(3));
  Value *instNodeAddrHi = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(4));
  Value *primitiveIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(5));
  Value *anyHitCallType = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(6));
  Value *geometryIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(7));
  m_builder->create<GpurtSetHitAttributesOp>(tCurrent, kind, status, instNodeAddrLo, instNodeAddrHi, primitiveIndex,
                                             anyHitCallType, geometryIndex);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to set trace parameters
//
// @param func : The function to create
void ProcessGpuRtLibrary::createSetTraceParams(llvm::Function *func) {
  Value *rayFlags = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  Value *instanceInclusionMask = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  Value *originX = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(2));
  Value *originY = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(3));
  Value *originZ = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(4));
  Value *tMin = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(5));
  Value *dirX = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(6));
  Value *dirY = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(7));
  Value *dirZ = m_builder->CreateLoad(m_builder->getFloatTy(), func->getArg(8));
  m_builder->create<GpurtSetTraceParamsOp>(rayFlags, instanceInclusionMask, originX, originY, originZ, tMin, dirX, dirY,
                                           dirZ);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to call closest-hit shader
//
// @param func : The function to create
void ProcessGpuRtLibrary::createCallClosestHitShader(llvm::Function *func) {
  Value *shaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(0));
  Value *tableIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  m_builder->CreateRet(m_builder->create<GpurtCallClosestHitShaderOp>(shaderId, tableIndex));
}

// =====================================================================================================================
// Fill in function to call miss shader
//
// @param func : The function to create
void ProcessGpuRtLibrary::createCallMissShader(llvm::Function *func) {
  Value *shaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(0));
  Value *tableIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  m_builder->CreateRet(m_builder->create<GpurtCallMissShaderOp>(shaderId, tableIndex));
}

// =====================================================================================================================
// Fill in function to call triangle any-hit shader
//
// @param func : The function to create
void ProcessGpuRtLibrary::createCallTriangleAnyHitShader(llvm::Function *func) {
  Value *shaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(0));
  Value *tableIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));

  Type *attrTy = StructType::get(*m_context, FixedVectorType::get(m_builder->getFloatTy(), 2), false);
  Value *attr = m_builder->CreateLoad(attrTy, func->getArg(2));
  attr = m_builder->CreateExtractValue(attr, 0);
  m_builder->create<GpurtCallTriangleAnyHitShaderOp>(shaderId, tableIndex, attr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to call intersection shader
//
// @param func : The function to create
void ProcessGpuRtLibrary::createCallIntersectionShader(llvm::Function *func) {
  Value *shaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(0));
  Value *anyHitShaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(1));
  Value *tableIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(2));
  m_builder->create<GpurtCallIntersectionShaderOp>(shaderId, anyHitShaderId, tableIndex);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to set triangle intersection attributes
//
// @param func : The function to create
void ProcessGpuRtLibrary::createSetTriangleIntersectionAttributes(llvm::Function *func) {
  Value *barycentrics = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 2), func->getArg(0));
  m_builder->create<GpurtSetTriangleIntersectionAttributesOp>(barycentrics);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to set hit triangle node pointer
//
// @param func : The function to create
void ProcessGpuRtLibrary::createSetHitTriangleNodePointer(llvm::Function *func) {
  Value *bvhAddress = m_builder->CreateLoad(m_builder->getInt64Ty(), func->getArg(0));
  Value *nodePointer = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  m_builder->create<GpurtSetHitTriangleNodePointerOp>(bvhAddress, nodePointer);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to get parent ID
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetParentId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetParentIdOp>());
}

// =====================================================================================================================
// Fill in function to get set parent ID
//
// @param func : The function to create
void ProcessGpuRtLibrary::createSetParentId(llvm::Function *func) {
  Value *rayId = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->create<GpurtSetParentIdOp>(rayId);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to get dispatch ray index
//
// @param func : The function to create
void ProcessGpuRtLibrary::createDispatchRayIndex(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<DispatchRaysIndexOp>());
}

// =====================================================================================================================
// Fill in function to get ray static ID
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetStaticId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetRayStaticIdOp>());
}

// =====================================================================================================================
// Fill in function to initialize ray static ID
//
// @param func : The function to create
void ProcessGpuRtLibrary::createInitStaticId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtInitStaticIdOp>());
}

// =====================================================================================================================
// Fill in function to get known set ray flags
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetKnownSetRayFlags(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetKnownSetRayFlagsOp>());
}

// =====================================================================================================================
// Fill in function to get known unset ray flags
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetKnownUnsetRayFlags(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetKnownUnsetRayFlagsOp>());
}

// =====================================================================================================================
// Fill in function to make a trace ray PC
//
// @param func : The function to create
void ProcessGpuRtLibrary::createMakePc(llvm::Function *func) {
  Value *addr32 = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->CreateRet(m_builder->create<GpurtMakePcOp>(func->getReturnType(), addr32));
}

// =====================================================================================================================
// Fill in function of AmdExtDispatchThreadIdFlat
//
// @param func : The function to create
void ProcessGpuRtLibrary::createDispatchThreadIdFlat(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtDispatchThreadIdFlatOp>());
}
// =====================================================================================================================
// Fill in function to allocate continuation stack pointer
//
// @param func : The function to create
void ProcessGpuRtLibrary::createContStackAlloc(llvm::Function *func) {
  assert(func->arg_size() == 1);
  Value *byteSize = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  auto stackPtr = m_builder->create<cps::AllocOp>(byteSize);
  m_builder->CreateRet(m_builder->CreatePtrToInt(stackPtr, m_builder->getInt32Ty()));
}

// =====================================================================================================================
// Fill in function to free continuation stack pointer
//
// @param func : The function to create
void ProcessGpuRtLibrary::createContStackFree(llvm::Function *func) {
  Value *byteSize = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->create<cps::FreeOp>(byteSize);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to get continuation stack pointer
//
// @param func : The function to create
void ProcessGpuRtLibrary::createContStackGetPtr(llvm::Function *func) {
  auto stackPtr = m_builder->create<cps::GetVspOp>();
  m_builder->CreateRet(m_builder->CreatePtrToInt(stackPtr, m_builder->getInt32Ty()));
}

// =====================================================================================================================
// Fill in function to set continuation stack pointer
//
// @param func : The function to create
void ProcessGpuRtLibrary::createContStackSetPtr(llvm::Function *func) {
  auto csp = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->create<cps::SetVspOp>(m_builder->CreateIntToPtr(csp, m_builder->getPtrTy(cps::stackAddrSpace)));
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to load from given continuation stack address
//
// @param func : The function to create
void ProcessGpuRtLibrary::createContStackLoad(llvm::Function *func) {
  auto loadTy = func->getReturnType();
  auto addr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  auto ptr = m_builder->CreateIntToPtr(addr, m_builder->getPtrTy(cps::stackAddrSpace));
  m_builder->CreateRet(m_builder->CreateLoad(loadTy, ptr));
}

// =====================================================================================================================
// Fill in function to store to given continuation stack address
//
// @param func : The function to create
void ProcessGpuRtLibrary::createContStackStore(llvm::Function *func) {
  unsigned dataArgIndex = func->arg_size() - 1;
  Type *dataType = getFuncArgPtrElementType(func, dataArgIndex);

  auto addr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  auto data = m_builder->CreateLoad(dataType, func->getArg(1));
  auto ptr = m_builder->CreateIntToPtr(addr, m_builder->getPtrTy(cps::stackAddrSpace));
  m_builder->CreateStore(data, ptr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to enqueue shader
//
// TODO: Once the handling of local root indices and continuation reference bit sizes has been unified, remove this
//       method in favor of letting earlyGpurtTransform do everything.
//
// @param func : The function to create
void ProcessGpuRtLibrary::createEnqueue(Function *func) {
  auto funcName = func->getName();

  Value *addr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));

  SmallVector<Value *> tailArgs;
  bool hasWaitMaskArg = funcName.contains("Wait");
  // Skip waitMask
  const unsigned shaderIdxArgIdx = hasWaitMaskArg ? 2 : 1;
  Value *shaderIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(shaderIdxArgIdx));
  const unsigned retAddrArgIdx = shaderIdxArgIdx + 1;

  Value *retAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(retAddrArgIdx));
  const unsigned systemDataArgIdx = retAddrArgIdx + 1;
  // Process system-data and arguments after.
  unsigned argIdx = systemDataArgIdx;
  while (argIdx < func->arg_size()) {
    tailArgs.push_back(m_builder->CreateLoad(getFuncArgPtrElementType(func, argIdx), func->getArg(argIdx)));
    argIdx++;
  }

  // TODO: pass the levelMask correctly.
  if (!funcName.contains("EnqueueAnyHit"))
    tailArgs.insert(tailArgs.begin() + 1, PoisonValue::get(StructType::get(m_builder->getContext())));
  m_builder->create<cps::JumpOp>(addr, -1, PoisonValue::get(m_builder->getInt32Ty()), shaderIndex, retAddr, tailArgs);
  m_builder->CreateUnreachable();

  // Clear the name so that earlyGpurtTransform doesn't try to handle the function.
  func->setName({});
}

// =====================================================================================================================
// Fill in function to tell GPURT it is compiled from LLPC
//
// @param func : The function to create
void ProcessGpuRtLibrary::createIsLlpc(llvm::Function *func) {
  auto *trueConst = ConstantInt::getTrue(func->getContext());
  m_builder->CreateRet(trueConst);
}

// =====================================================================================================================
// Fill in function to get the current functions shader record index
//
// @param func : The function to create
void ProcessGpuRtLibrary::createGetShaderRecordIndex(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<lgc::rt::ShaderIndexOp>());
}

// =====================================================================================================================
// Fill in function to write shader marker
//
// @param func : The function to create
void ProcessGpuRtLibrary::createShaderMarker(llvm::Function *func) {
  Value *dataPtr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_ttracedata, {}, dataPtr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to write wave scan
//
// @param func : The function to create
void ProcessGpuRtLibrary::createWaveScan(llvm::Function *func) {
  auto argIt = func->arg_begin();
  auto retType = cast<FixedVectorType>(func->getReturnType());
  auto int32Ty = m_builder->getInt32Ty();
  Value *waveOp = m_builder->CreateLoad(int32Ty, argIt++);
  Value *flags = m_builder->CreateLoad(int32Ty, argIt++);
  Value *src0 = m_builder->CreateLoad(retType, argIt);
  m_builder->CreateRet(m_builder->create<GpurtWaveScanOp>(waveOp, flags, src0));
}

// =====================================================================================================================
void ProcessGpuRtLibrary::createDualIntersectRay(Function *func) {
  createIntersectRay(func, true);
}

void ProcessGpuRtLibrary::createIntersectRayBvh8(Function *func) {
  createIntersectRay(func, false);
}

// =====================================================================================================================
// Create function to return dual ray intersection result
//
// @param func : The function to create
void ProcessGpuRtLibrary::createIntersectRay(Function *func, bool isDualNode) {
  auto rtip = m_gpurtKey.rtipVersion;
  if (m_gpurtKey.bvhResDesc.size() < 4 || (rtip < 3 && rtip != Vkgc::RtIpVersion({1, 5}))) {
    // Don't generate code for non fitting RTIP.
    m_builder->CreateRet(PoisonValue::get(func->getReturnType()));
    return;
  }
  auto argIt = func->arg_begin();
  // 1.
  // struct DualIntersectResult
  // {
  //   uint4 first;
  //   uint4 second;
  //   uint2 geometryId;
  // };
  // DualIntersectResult AmdTraceRayDualIntersectRay(
  //   in uint2     baseNodePtr,
  //   inout float3 rayOrigin,
  //   inout float3 rayDir,
  //   in float     rayExtent,
  //   in uint      instanceMask,
  //   in uint      boxSortHeuristic,
  //   in uint      node0,
  //   in uint      node1)
  // {
  //   bvhSrd = generateBvhSrd()
  //   offsets.x = node0
  //   offsets.y = node1
  //   call {<10 x i32>, <3 x float>, <3 x float>} @llvm.amdgcn.image.bvh.dual.intersect.ray(i64 %node_ptr, float
  //   %ray_extent, i8 %instance_mask, <3 x float> %ray_origin, <3 x float> %ray_dir, <2 x i32> %offsets,
  //   <4 x i32> %tdescr)
  // }

  // 2.
  // struct Bvh8IntersectResult
  //  {
  //    uint4 slot0;
  //    uint4 slot1;
  //    uint2 ext;
  //  }

  // Bvh8IntersectResult AmdTraceRayIntersectRayBvh8(
  //     in uint2     baseNodePtr,
  //     inout float3 rayOrigin,
  //     inout float3 rayDir,
  //     in float     rayExtent,
  //     in uint      instanceMask,
  //     in uint      boxSortHeuristic,
  //     in uint      node)
  // {
  //   bvhSrd = generateBvhSrd()
  //   offsets = node
  //   call {<10 x i32>, <3 x float>, <3 x float>} @llvm.amdgcn.image.bvh8.intersect.ray(i64 %node_ptr, float
  //   %ray_extent, i8 %instance_mask, <3 x float> %ray_origin, <3 x float> %ray_dir, <i32> %offsets,
  //   <4 x i32> %tdescr)
  // }

  // uint2 baseNodePtr
  Value *baseNodePtr = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), argIt);
  baseNodePtr = m_builder->CreateBitCast(baseNodePtr, m_builder->getInt64Ty());
  argIt++;

  // float3 rayOrigin
  Value *rayOrigin = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 3), argIt);
  argIt++;

  // float3 rayDir
  Value *rayDir = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 3), argIt);
  argIt++;

  // float rayExtent
  Value *rayExtent = m_builder->CreateLoad(m_builder->getFloatTy(), argIt);
  argIt++;

  // uint instanceMask
  Value *instanceMask = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt);
  instanceMask = m_builder->CreateTrunc(instanceMask, m_builder->getInt8Ty());
  argIt++;

  // uint boxSortHeuristic
  Value *boxSortHeuristic = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt);
  argIt++;

  // uint node0
  Value *node0 = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt);
  Value *dualNodes = PoisonValue::get(FixedVectorType::get(Type::getInt32Ty(*m_context), 2));
  if (isDualNode) {
    argIt++;
    // uint node1
    Value *node1 = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt);
    // Pack two node together
    dualNodes = m_builder->CreateInsertElement(dualNodes, node0, uint64_t(0));
    dualNodes = m_builder->CreateInsertElement(dualNodes, node1, 1);
  }

  Value *imageDesc = createGetBvhSrd(nullptr, boxSortHeuristic);

  auto intx10Ty = llvm::FixedVectorType::get(m_builder->getInt32Ty(), 10);
  auto floatx3Ty = llvm::FixedVectorType::get(m_builder->getFloatTy(), 3);
  Type *returnTy = llvm::StructType::get(m_builder->getContext(), {intx10Ty, floatx3Ty, floatx3Ty});
  std::string callName =
      (isDualNode == 1) ? "llvm.amdgcn.image.bvh.dual.intersect.ray" : "llvm.amdgcn.image.bvh8.intersect.ray";

  Value *result = m_builder->CreateNamedCall(
      callName, returnTy,
      {baseNodePtr, rayExtent, instanceMask, rayOrigin, rayDir, isDualNode ? dualNodes : node0, imageDesc}, {});

  // @llvm.amdgcn.image.bvh.dual.intersect.ray and @llvm.amdgcn.image.bvh8.intersect.ray intrinsic
  // returns {<10 x i32>, <3 x float>, <3 x float>}, which are:
  // DualIntersectResult/Bvh8IntersectResult, ray_origin, ray_dir.
  Value *dualIntersectOrBvh8Result = m_builder->CreateExtractValue(result, 0);
  Value *resultFirst = m_builder->CreateShuffleVector(dualIntersectOrBvh8Result, ArrayRef<int>{0, 1, 2, 3});
  Value *resultSecond = m_builder->CreateShuffleVector(dualIntersectOrBvh8Result, ArrayRef<int>{4, 5, 6, 7});
  Value *resultGeometryId = m_builder->CreateShuffleVector(dualIntersectOrBvh8Result, ArrayRef<int>{8, 9});

  Value *resultRayOrigin = m_builder->CreateExtractValue(result, 1);
  Value *resultRayDir = m_builder->CreateExtractValue(result, 2);

  assert(func->getReturnType()->isStructTy() && (func->getReturnType()->getStructNumElements() == 3));
  Value *ret = PoisonValue::get(func->getReturnType());
  ret = m_builder->CreateInsertValue(ret, resultFirst, 0);
  ret = m_builder->CreateInsertValue(ret, resultSecond, 1);
  ret = m_builder->CreateInsertValue(ret, resultGeometryId, 2);

  // Store rayOrigin and rayDir back.
  m_builder->CreateStore(resultRayOrigin, func->getArg(1));
  m_builder->CreateStore(resultRayDir, func->getArg(2));

  m_builder->CreateRet(ret);
}

// =====================================================================================================================
// Push 8 nodes to LDS stack and Pop N nodes
//
// @param func : The function to create
// @param returnNodeCount : Number of returned node
// @param primRangeEnable : Whether to enable primitive range
void ProcessGpuRtLibrary::createDsStackPush8PopN(Function *func, unsigned returnNodeCount, bool primRangeEnable) {
  assert((returnNodeCount == 1) || (returnNodeCount == 2));
  assert(m_context->getGfxIpVersion().major >= 12);

  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  const static unsigned MaxLdsStackEntries = 16;

  auto argIt = func->arg_begin();
  Value *stackAddr = argIt++;
  Value *stackAddrVal = m_builder->CreateLoad(m_builder->getInt32Ty(), stackAddr);
  Value *lastNodePtr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *data0 = m_builder->CreateLoad(int32x4Ty, argIt++);
  Value *data1 = m_builder->CreateLoad(int32x4Ty, argIt);

  Value *data = m_builder->CreateShuffleVector(data0, data1, ArrayRef<int>{0, 1, 2, 3, 4, 5, 6, 7});

  // OFFSET = {OFFSET1, OFFSET0}
  // stack_size[4:0] = OFFSET0[4:0]
  assert(MaxLdsStackEntries == 16);
  unsigned offsetVal = MaxLdsStackEntries;
  if (primRangeEnable) {
    assert(returnNodeCount == 1);
    // NOTE: For the push8-pop1 variant, bit 1 of OFFSET1 indicates if primitive range is enabled. We set the bit
    // here by request.
    offsetVal |= 1 << 9;
  }

  Value *offset = m_builder->getInt32(offsetVal);

  Intrinsic::AMDGCNIntrinsics intrinsic = (returnNodeCount == 1) ? Intrinsic::amdgcn_ds_bvh_stack_push8_pop1_rtn
                                                                 : Intrinsic::amdgcn_ds_bvh_stack_push8_pop2_rtn;
  Value *result = m_builder->CreateIntrinsic(intrinsic, {}, {stackAddrVal, lastNodePtr, data, offset});

  m_builder->CreateStore(m_builder->CreateExtractValue(result, 1), stackAddr);

  Value *ret = m_builder->CreateExtractValue(result, 0);

  if (returnNodeCount == 1) {
    m_builder->CreateRet(ret);
  } else {
    // llvm.amdgcn.ds.bvh.stack.push8.pop2.rtn returns i64, cast it to uvec2.
    m_builder->CreateRet(m_builder->CreateBitCast(ret, FixedVectorType::get(m_builder->getInt32Ty(), 2)));
  }
}

// =====================================================================================================================
// Create function to do LDS stack push 8 pop 1
//
// @param func : The function to create
void ProcessGpuRtLibrary::createDsStackPush8Pop1(Function *func) {
  if (m_gpurtKey.rtipVersion >= 3)
    createDsStackPush8PopN(func, 1, false);
  else
    m_builder->CreateRet(PoisonValue::get(func->getReturnType()));
}

// =====================================================================================================================
// Create function to do LDS stack push 8 pop 2
//
// @param func : The function to create
void ProcessGpuRtLibrary::createDsStackPush8Pop2(Function *func) {
  if (m_gpurtKey.rtipVersion >= 3 || m_gpurtKey.rtipVersion == Vkgc::RtIpVersion({1, 5}))
    createDsStackPush8PopN(func, 2, false);
  else
    m_builder->CreateRet(PoisonValue::get(func->getReturnType()));
}

// =====================================================================================================================
// Create function to do LDS stack push 8 pop 1 with primitive range enabled
//
// @param func : The function to create
void ProcessGpuRtLibrary::createDsStackPush8Pop1PrimRangeEnabled(Function *func) {
  if (m_gpurtKey.rtipVersion >= 3)
    createDsStackPush8PopN(func, 1, true);
  else
    m_builder->CreateRet(PoisonValue::get(func->getReturnType()));
}

} // namespace Llpc
