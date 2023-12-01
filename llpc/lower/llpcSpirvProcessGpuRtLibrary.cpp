/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvProcessGpuRtLibrary.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvProcessGpuRtLibrary.
 ***********************************************************************************************************************
 */
#include "llpcSpirvProcessGpuRtLibrary.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcRtDialect.h"

#define DEBUG_TYPE "llpc-spirv-lower-gpurt-library"
using namespace lgc;
using namespace llvm;
using namespace lgc::rt;

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
  m_libFuncPtrs["AmdTraceRayGetStackSize"] = &SpirvProcessGpuRtLibrary::createGetStackSize;
  m_libFuncPtrs["AmdTraceRayLdsRead"] = &SpirvProcessGpuRtLibrary::createLdsRead;
  m_libFuncPtrs["AmdTraceRayLdsWrite"] = &SpirvProcessGpuRtLibrary::createLdsWrite;
  m_libFuncPtrs["AmdTraceRayGetStackBase"] = &SpirvProcessGpuRtLibrary::createGetStackBase;
  m_libFuncPtrs["AmdTraceRayGetStackStride"] = &SpirvProcessGpuRtLibrary::createGetStackStride;
  m_libFuncPtrs["AmdTraceRayLdsStackInit"] = &SpirvProcessGpuRtLibrary::createLdsStackInit;
  m_libFuncPtrs["AmdTraceRayLdsStackStore"] = &SpirvProcessGpuRtLibrary::createLdsStackStore;
  m_libFuncPtrs["AmdTraceRayGetBoxSortHeuristicMode"] = &SpirvProcessGpuRtLibrary::createGetBoxSortHeuristicMode;
  m_libFuncPtrs["AmdTraceRayGetStaticFlags"] = &SpirvProcessGpuRtLibrary::createGetStaticFlags;
  m_libFuncPtrs["AmdTraceRayGetTriangleCompressionMode"] = &SpirvProcessGpuRtLibrary::createGetTriangleCompressionMode;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddr"] = &SpirvProcessGpuRtLibrary::createLoadDwordAtAddr;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx2"] = &SpirvProcessGpuRtLibrary::createLoadDwordAtAddrx2;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx4"] = &SpirvProcessGpuRtLibrary::createLoadDwordAtAddrx4;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConvertF32toF16NegInf"] =
      &SpirvProcessGpuRtLibrary::createConvertF32toF16NegInf;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConvertF32toF16PosInf"] =
      &SpirvProcessGpuRtLibrary::createConvertF32toF16PosInf;
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 33
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_IntersectBvhNode"] = &SpirvProcessGpuRtLibrary::createIntersectBvh;
#else
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_IntersectInternal"] = &SpirvProcessGpuRtLibrary::createIntersectBvh;
#endif
  m_libFuncPtrs["AmdTraceRaySampleGpuTimer"] = &SpirvProcessGpuRtLibrary::createSampleGpuTimer;
  m_libFuncPtrs["AmdTraceRayGetFlattenedGroupThreadId"] = &SpirvProcessGpuRtLibrary::createGetFlattenedGroupThreadId;
  m_libFuncPtrs["AmdTraceRayGetHitAttributes"] = &SpirvProcessGpuRtLibrary::createGetHitAttributes;
  m_libFuncPtrs["AmdTraceRaySetHitAttributes"] = &SpirvProcessGpuRtLibrary::createSetHitAttributes;
  m_libFuncPtrs["AmdTraceRaySetTraceParams"] = &SpirvProcessGpuRtLibrary::createSetTraceParams;
  m_libFuncPtrs["AmdTraceRayCallClosestHitShader"] = &SpirvProcessGpuRtLibrary::createCallClosestHitShader;
  m_libFuncPtrs["AmdTraceRayCallMissShader"] = &SpirvProcessGpuRtLibrary::createCallMissShader;
  m_libFuncPtrs["AmdTraceRayCallTriangleAnyHitShader"] = &SpirvProcessGpuRtLibrary::createCallTriangleAnyHitShader;
  m_libFuncPtrs["AmdTraceRayCallIntersectionShader"] = &SpirvProcessGpuRtLibrary::createCallIntersectionShader;
  m_libFuncPtrs["AmdTraceRaySetTriangleIntersectionAttributes"] =
      &SpirvProcessGpuRtLibrary::createSetTriangleIntersectionAttributes;
  m_libFuncPtrs["AmdTraceRaySetHitTriangleNodePointer"] = &SpirvProcessGpuRtLibrary::createSetHitTriangleNodePointer;
  m_libFuncPtrs["AmdTraceRayGetParentId"] = &SpirvProcessGpuRtLibrary::createGetParentId;
  m_libFuncPtrs["AmdTraceRaySetParentId"] = &SpirvProcessGpuRtLibrary::createSetParentId;
  m_libFuncPtrs["AmdTraceRayDispatchRaysIndex"] = &SpirvProcessGpuRtLibrary::createDispatchRayIndex;
  m_libFuncPtrs["AmdTraceRayGetStaticId"] = &SpirvProcessGpuRtLibrary::createGetStaticId;
  m_libFuncPtrs["AmdTraceRayGetKnownSetRayFlags"] = &SpirvProcessGpuRtLibrary::createGetKnownSetRayFlags;
  m_libFuncPtrs["AmdTraceRayGetKnownUnsetRayFlags"] = &SpirvProcessGpuRtLibrary::createGetKnownUnsetRayFlags;
  m_libFuncPtrs["_AmdContStackAlloc"] = &SpirvProcessGpuRtLibrary::createContStackAlloc;
  m_libFuncPtrs["_AmdContStackFree"] = &SpirvProcessGpuRtLibrary::createContStackFree;
  m_libFuncPtrs["_AmdContStackGetPtr"] = &SpirvProcessGpuRtLibrary::createContStackGetPtr;
  m_libFuncPtrs["_AmdContStackSetPtr"] = &SpirvProcessGpuRtLibrary::createContStackSetPtr;
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::processLibraryFunction(Function *&func) {
  auto funcName = func->getName();

  StringRef traceRayFuncName = m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_TRACE_RAY);

  const StringRef rayQueryInitializeFuncName =
      m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_TRACE_RAY_INLINE);
  const StringRef rayQueryProceedFuncName =
      m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_RAY_QUERY_PROCEED);

  const StringRef fetchTrianglePositionFromNodePointerFuncName =
      m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_NODE_POINTER);
  const StringRef fetchTrianglePositionFromRayQueryFuncName =
      m_context->getPipelineContext()->getRayTracingFunctionName(Vkgc::RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_RAY_QUERY);

  assert(!traceRayFuncName.empty());
  assert(!rayQueryInitializeFuncName.empty());
  assert(!rayQueryProceedFuncName.empty());
  assert(!fetchTrianglePositionFromNodePointerFuncName.empty());
  assert(!fetchTrianglePositionFromRayQueryFuncName.empty());

  // Set external linkage for library entry functions
  if (funcName.startswith(traceRayFuncName) || funcName.startswith(rayQueryInitializeFuncName) ||
      funcName.startswith(rayQueryProceedFuncName) ||
      funcName.startswith(fetchTrianglePositionFromNodePointerFuncName) ||
      funcName.startswith(fetchTrianglePositionFromRayQueryFuncName)) {
    func->setLinkage(GlobalValue::ExternalLinkage);
    return;
  }

  // Drop dummy entry function.
  static const char *LibraryEntryFuncName = "libraryEntry";
  if (funcName.startswith(LibraryEntryFuncName)) {
    func->dropAllReferences();
    func->eraseFromParent();
    func = nullptr;
    return;
  }

  // Special handling for _AmdContStackStore* and _AmdContStackLoad* to accept arbitrary type
  if (funcName.startswith("_AmdContStackStore")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createContStackStore(func);
    return;
  } else if (funcName.startswith("_AmdContStackLoad")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createContStackLoad(func);
    return;
  }

  // Create implementation for intrinsic functions.
  auto &gpurtFuncTable = LibraryFunctionTable::get().m_libFuncPtrs;
  auto gpurtFuncIt = gpurtFuncTable.find(funcName);
  if (gpurtFuncIt != gpurtFuncTable.end()) {
    auto funcPtr = gpurtFuncIt->second;
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
  }
}

// =====================================================================================================================
// Fill in function to get stack size
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStackSize(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackSizeOp>());
}

// =====================================================================================================================
// Fill in function to get stack base
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStackBase(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackBaseOp>());
}

// =====================================================================================================================
// Fill in function to write LDS stack
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsWrite(Function *func) {
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
void SpirvProcessGpuRtLibrary::createLdsRead(Function *func) {
  Value *stackIndex = func->arg_begin();
  stackIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), stackIndex);
  m_builder->CreateRet(m_builder->create<GpurtStackReadOp>(stackIndex, false));
}

// =====================================================================================================================
// Fill in function to get stack stride
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStackStride(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStackStrideOp>());
}

// =====================================================================================================================
// Fill in function to init stack LDS
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsStackInit(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtLdsStackInitOp>());
}

// =====================================================================================================================
// Fill in function to store stack LDS
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLdsStackStore(Function *func) {
  auto argIt = func->arg_begin();
  Value *stackAddr = argIt++;
  Value *lastVisited = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  Value *data = m_builder->CreateLoad(int32x4Ty, argIt);
  m_builder->CreateRet(m_builder->create<GpurtLdsStackStoreOp>(stackAddr, lastVisited, data));
}

// =====================================================================================================================
// Fill in function to get box sort heuristic mode
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetBoxSortHeuristicMode(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetBoxSortHeuristicModeOp>());
}

// =====================================================================================================================
// Fill in function to get static flags
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetStaticFlags(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetStaticFlagsOp>());
}

// =====================================================================================================================
// Fill in function to get triangle compression mode
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createGetTriangleCompressionMode(Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetTriangleCompressionModeOp>());
}

// =====================================================================================================================
// Fill in function to load 1 dword at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddr(Function *func) {
  createLoadDwordAtAddrWithType(func, m_builder->getInt32Ty());
}

// =====================================================================================================================
// Fill in function to load 2 dwords at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddrx2(Function *func) {
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  createLoadDwordAtAddrWithType(func, int32x2Ty);
}

// =====================================================================================================================
// Fill in function to load 4 dwords at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddrx4(Function *func) {
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  createLoadDwordAtAddrWithType(func, int32x4Ty);
}

// =====================================================================================================================
// Fill in function to load dwords at given address based on given type
//
// @param func : The function to process
// @param loadTy : Load type
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddrWithType(Function *func, Type *loadTy) {
  auto argIt = func->arg_begin();

  Value *gpuLowAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *gpuHighAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *offset = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);

  // Use (gpuLowAddr, gpuHighAddr) to calculate i64 gpuAddr
  gpuLowAddr = m_builder->CreateZExt(gpuLowAddr, m_builder->getInt64Ty());
  gpuHighAddr = m_builder->CreateZExt(gpuHighAddr, m_builder->getInt64Ty());
  gpuHighAddr = m_builder->CreateShl(gpuHighAddr, m_builder->getInt64(32));
  Value *gpuAddr = m_builder->CreateOr(gpuLowAddr, gpuHighAddr);

  Type *gpuAddrAsPtrTy = PointerType::get(m_builder->getContext(), SPIRAS_Global);
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
void SpirvProcessGpuRtLibrary::createConvertF32toF16NegInf(Function *func) {
  createConvertF32toF16WithRoundingMode(func, RoundingMode::TowardNegative);
}

// =====================================================================================================================
// Fill in function to convert f32 to f16 with rounding toward positive
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createConvertF32toF16PosInf(Function *func) {
  createConvertF32toF16WithRoundingMode(func, RoundingMode::TowardPositive);
}

// =====================================================================================================================
// Fill in function to convert f32 to f16 with given rounding mode
//
// @param func : The function to process
// @param rm : Rounding mode
void SpirvProcessGpuRtLibrary::createConvertF32toF16WithRoundingMode(Function *func, RoundingMode rm) {
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
void SpirvProcessGpuRtLibrary::createIntersectBvh(Function *func) {
  const auto *rtState = m_context->getPipelineContext()->getRayTracingState();
  assert(rtState->bvhResDesc.dataSizeInDwords != 0);
  if (rtState->bvhResDesc.dataSizeInDwords < 4)
    return;

#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 33
    // Ray tracing utility function: AmdExtD3DShaderIntrinsics_IntersectBvhNode
    // uint4 AmdExtD3DShaderIntrinsics_IntersectBvhNode(
#else
    // Ray tracing utility function: AmdExtD3DShaderIntrinsics_IntersectInternal
    // uint4 AmdExtD3DShaderIntrinsics_IntersectInternal(
#endif
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
Value *SpirvProcessGpuRtLibrary::createGetBvhSrd(llvm::Value *expansion, llvm::Value *boxSortMode) {
  const auto *rtState = m_context->getPipelineContext()->getRayTracingState();
  assert(rtState->bvhResDesc.dataSizeInDwords == 4);

  // Construct image descriptor from rtstate.
  Value *bvhSrd = PoisonValue::get(FixedVectorType::get(m_builder->getInt32Ty(), 4));
  bvhSrd =
      m_builder->CreateInsertElement(bvhSrd, m_builder->getInt32(rtState->bvhResDesc.descriptorData[0]), uint64_t(0));
  bvhSrd = m_builder->CreateInsertElement(bvhSrd, m_builder->getInt32(rtState->bvhResDesc.descriptorData[2]), 2u);
  bvhSrd = m_builder->CreateInsertElement(bvhSrd, m_builder->getInt32(rtState->bvhResDesc.descriptorData[3]), 3u);

  Value *bvhSrdDw1 = m_builder->getInt32(rtState->bvhResDesc.descriptorData[1]);

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
void SpirvProcessGpuRtLibrary::createSampleGpuTimer(llvm::Function *func) {
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
}

// =====================================================================================================================
// Fill in function to get flattened group thread ID
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetFlattenedGroupThreadId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetFlattenedGroupThreadIdOp>());
}

// =====================================================================================================================
// Fill in function to get hit attributes
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetHitAttributes(llvm::Function *func) {
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
void SpirvProcessGpuRtLibrary::createSetHitAttributes(llvm::Function *func) {
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
void SpirvProcessGpuRtLibrary::createSetTraceParams(llvm::Function *func) {
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
void SpirvProcessGpuRtLibrary::createCallClosestHitShader(llvm::Function *func) {
  Value *shaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(0));
  Value *tableIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  m_builder->CreateRet(m_builder->create<GpurtCallClosestHitShaderOp>(shaderId, tableIndex));
}

// =====================================================================================================================
// Fill in function to call miss shader
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createCallMissShader(llvm::Function *func) {
  Value *shaderId = m_builder->CreateLoad(FixedVectorType::get(m_builder->getInt32Ty(), 2), func->getArg(0));
  Value *tableIndex = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  m_builder->CreateRet(m_builder->create<GpurtCallMissShaderOp>(shaderId, tableIndex));
}

// =====================================================================================================================
// Fill in function to call triangle any-hit shader
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createCallTriangleAnyHitShader(llvm::Function *func) {
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
void SpirvProcessGpuRtLibrary::createCallIntersectionShader(llvm::Function *func) {
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
void SpirvProcessGpuRtLibrary::createSetTriangleIntersectionAttributes(llvm::Function *func) {
  Value *barycentrics = m_builder->CreateLoad(FixedVectorType::get(m_builder->getFloatTy(), 2), func->getArg(0));
  m_builder->create<GpurtSetTriangleIntersectionAttributesOp>(barycentrics);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to set hit triangle node pointer
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createSetHitTriangleNodePointer(llvm::Function *func) {
  Value *bvhAddress = m_builder->CreateLoad(m_builder->getInt64Ty(), func->getArg(0));
  Value *nodePointer = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  m_builder->create<GpurtSetHitTriangleNodePointerOp>(bvhAddress, nodePointer);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to get parent ID
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetParentId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetParentIdOp>());
}

// =====================================================================================================================
// Fill in function to get set parent ID
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createSetParentId(llvm::Function *func) {
  Value *rayId = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->create<GpurtSetParentIdOp>(rayId);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to get dispatch ray index
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createDispatchRayIndex(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<DispatchRaysIndexOp>());
}

// =====================================================================================================================
// Fill in function to get ray static ID
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetStaticId(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetRayStaticIdOp>());
}

// =====================================================================================================================
// Fill in function to get known set ray flags
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetKnownSetRayFlags(llvm::Function *func) {
  // TODO: currently return 0 to indicate that there is no known set
  // We will probably need to analyse the traceRay ray flags for actual value
  m_builder->CreateRet(m_builder->getInt32(0));
}

// =====================================================================================================================
// Fill in function to get known unset ray flags
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetKnownUnsetRayFlags(llvm::Function *func) {
  // TODO: return 0 to indicate there is no knownUnset bits
  // We will probably need to analyse the traceRay ray flags for actual value
  m_builder->CreateRet(m_builder->getInt32(0));
}

// =====================================================================================================================
// Fill in function to allocate continuation stack pointer
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackAlloc(llvm::Function *func) {
  Value *byteSize = nullptr;
  if (func->arg_size() == 2) {
    // TODO: Remove this when refactoring is done.
    // Ignore the first argument.
    byteSize = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(1));
  } else {
    assert(func->arg_size() == 1);
    byteSize = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  }
  auto stackPtr = m_builder->create<cps::AllocOp>(byteSize);
  m_builder->CreateRet(m_builder->CreatePtrToInt(stackPtr, m_builder->getInt32Ty()));
}

// =====================================================================================================================
// Fill in function to free continuation stack pointer
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackFree(llvm::Function *func) {
  Value *byteSize = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->create<cps::FreeOp>(byteSize);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to get continuation stack pointer
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackGetPtr(llvm::Function *func) {
  auto stackPtr = m_builder->create<cps::GetVspOp>();
  m_builder->CreateRet(m_builder->CreatePtrToInt(stackPtr, m_builder->getInt32Ty()));
}

// =====================================================================================================================
// Fill in function to set continuation stack pointer
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackSetPtr(llvm::Function *func) {
  auto csp = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->create<cps::SetVspOp>(m_builder->CreateIntToPtr(csp, m_builder->getPtrTy(cps::stackAddrSpace)));
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to load from given continuation stack address
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackLoad(llvm::Function *func) {
  auto loadTy = func->getReturnType();
  auto addr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  auto ptr = m_builder->CreateIntToPtr(addr, m_builder->getPtrTy(cps::stackAddrSpace));
  m_builder->CreateRet(m_builder->CreateLoad(loadTy, ptr));
}

// =====================================================================================================================
// Fill in function to store to given continuation stack address
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackStore(llvm::Function *func) {
  MDNode *storeTypeMeta = func->getMetadata(gSPIRVMD::ContStackStoreType);
  assert(storeTypeMeta);
  const auto constMD = cast<ConstantAsMetadata>(storeTypeMeta->getOperand(0));
  auto dataType = constMD->getType();

  auto addr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  auto data = m_builder->CreateLoad(dataType, func->getArg(1));
  auto ptr = m_builder->CreateIntToPtr(addr, m_builder->getPtrTy(cps::stackAddrSpace));
  m_builder->CreateStore(data, ptr);
  m_builder->CreateRetVoid();
}

} // namespace Llpc
