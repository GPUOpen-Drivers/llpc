/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerInternalLibraryIntrinsicUtil.cpp
 * @brief LLPC source file: utilities for lowering common internal library intrinsics.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "lgc/Builder.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace llvm;
using namespace lgc;

namespace Llpc {

// =====================================================================================================================
// Create function to get lane index (subgroup local invocation ID)
//
// @param func : The function to process
// @param builder : The IR builder
static void createLaneIndex(Function *func, Builder *builder) {
  builder->CreateRet(builder->CreateReadBuiltInInput(lgc::BuiltInSubgroupLocalInvocationId));
}

// =====================================================================================================================
// Create function to get lane count (wave size)
//
// @param func : The function to process
// @param builder : The IR builder
static void createLaneCount(Function *func, Builder *builder) {
  builder->CreateRet(builder->CreateGetWaveSize());
}

// =====================================================================================================================
// Create function to generate s_sethalt intrinsic
//
// @param func : The function to process
// @param builder : The IR builder
static void createHalt(Function *func, Builder *builder) {
  builder->CreateIntrinsic(Intrinsic::amdgcn_s_sethalt, {}, {builder->getInt32(1)});
  builder->CreateRetVoid();
}

// =====================================================================================================================
// Create function to compute the number of waves in the workgroup
//
// @param func : The function to process
// @param builder : The IR builder
static void createNumWavesCompute(Function *func, Builder *builder) {
  Value *workgroupSize = builder->CreateReadBuiltInInput(lgc::BuiltInWorkgroupSize);
  Value *workgroupSizeX = builder->CreateExtractElement(workgroupSize, uint64_t(0));
  Value *workgroupSizeY = builder->CreateExtractElement(workgroupSize, 1);
  Value *workgroupSizeZ = builder->CreateExtractElement(workgroupSize, 2);
  Value *numWaves = builder->CreateMul(workgroupSizeX, workgroupSizeY);
  numWaves = builder->CreateMul(numWaves, workgroupSizeZ);
  numWaves = builder->CreateSub(numWaves, builder->getInt32(1));
  Value *waveSize = builder->CreateGetWaveSize();
  numWaves = builder->CreateAdd(numWaves, waveSize);
  numWaves = builder->CreateUDiv(numWaves, waveSize);
  builder->CreateRet(numWaves);
}

// =====================================================================================================================
// Create function to compute the wave index in the workgroup
//
// @param func : The function to process
// @param builder : The IR builder
static void createWaveIndexCompute(Function *func, Builder *builder) {
  Value *waveId = builder->CreateReadBuiltInInput(lgc::BuiltInSubgroupId, {}, nullptr, nullptr, "");
  builder->CreateRet(waveId);
}

// =====================================================================================================================
// Create function to get gl_WorkGroupID
//
// @param func : The function to process
// @param builder : The IR builder
static void createGroupIdCompute(Function *func, Builder *builder) {
  Value *workGroupId = builder->CreateReadBuiltInInput(lgc::BuiltInWorkgroupId);
  builder->CreateRet(workGroupId);
}

// =====================================================================================================================
// Create function to get gl_WorkGroupSize
//
// @param func : The function to process
// @param builder : The IR builder
static void createGroupDimCompute(Function *func, Builder *builder) {
  Value *workGroupSize = builder->CreateReadBuiltInInput(lgc::BuiltInWorkgroupSize);
  builder->CreateRet(workGroupSize);
}

// =====================================================================================================================
// Create gl_LocalInvocationID
//
// @param func : The function to process
// @param builder : The IR builder
static void createThreadIdInGroupCompute(Function *func, Builder *builder) {
  Value *threadId = builder->CreateReadBuiltInInput(lgc::BuiltInLocalInvocationId);
  builder->CreateRet(threadId);
}

// =====================================================================================================================
// Create gl_LocalInvocationIndex, return uint
//
// @param func : The function to process
// @param builder : The IR builder
static void createFlattenedThreadIdInGroupCompute(Function *func, Builder *builder) {
  Value *threadId = builder->CreateReadBuiltInInput(lgc::BuiltInLocalInvocationIndex);
  builder->CreateRet(threadId);
}

// =====================================================================================================================
// Create subgroup mbcnt function
//
// @param func : The function to process
// @param builder : The IR builder
static void createMbcnt(Function *func, Builder *builder) {
  Value *ptr = func->getArg(0);
  Value *mask = builder->CreateLoad(FixedVectorType::get(builder->getInt32Ty(), 2), ptr);
  mask = builder->CreateBitCast(mask, builder->getInt64Ty());
  builder->CreateRet(builder->CreateSubgroupMbcnt(mask));
}

// =====================================================================================================================
// Create atomic function
//
// @param func : The function to process
// @param builder : The IR builder
// @param is64 : Whether we're creating a 64-bit atomic
// @param isCmpXchg : Whether we're creating a cmpxchg atomic
// @param binOp : If we're not creating a cmpxchg atomic, then this is the RMW atomic op
static void createAtomic(Function *func, Builder *builder, bool is64, bool isCmpXchg,
                         AtomicRMWInst::BinOp binOp = AtomicRMWInst::BAD_BINOP) {
  auto argIt = func->arg_begin();
  Value *arg = argIt++;
  Type *gpuVaTy = builder->getInt64Ty();
  Value *gpuAddr = builder->CreateLoad(gpuVaTy, arg);
  arg = argIt++;
  Type *offsetTy = builder->getInt32Ty();
  Value *offset = builder->CreateLoad(offsetTy, arg);
  Type *valueTy = is64 ? builder->getInt64Ty() : builder->getInt32Ty();
  Value *compare = isCmpXchg ? builder->CreateLoad(valueTy, argIt++) : nullptr;
  arg = argIt;
  Value *value = builder->CreateLoad(valueTy, arg);
  Type *gpuAddrAsPtrTy = PointerType::get(builder->getContext(), SPIRAS_Global);
  auto gpuAddrAsPtr = builder->CreateIntToPtr(gpuAddr, gpuAddrAsPtrTy);
  // Create GEP to get the byte address with byte offset
  gpuAddrAsPtr = builder->CreateGEP(builder->getInt8Ty(), gpuAddrAsPtr, offset);
  Value *atomicValue = nullptr;
  SyncScope::ID scope = func->getContext().getOrInsertSyncScopeID("agent");
  if (!isCmpXchg) {
    assert(binOp != AtomicRMWInst::BAD_BINOP);
    atomicValue = builder->CreateAtomicRMW(binOp, gpuAddrAsPtr, value, MaybeAlign(), AtomicOrdering::Monotonic, scope);
  } else {
    atomicValue = builder->CreateAtomicCmpXchg(gpuAddrAsPtr, compare, value, MaybeAlign(), AtomicOrdering::Monotonic,
                                               AtomicOrdering::Monotonic, scope);
    atomicValue = builder->CreateExtractValue(atomicValue, 0);
  }
  builder->CreateRet(atomicValue);
}

// =====================================================================================================================
// Create 32-bit atomic add at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomicAddAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ false, /*isCmpXchg*/ false, AtomicRMWInst::Add);
}

// =====================================================================================================================
// Create 32-bit atomic max at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomicMaxAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ false, /*isCmpXchg*/ false, AtomicRMWInst::Max);
}

// =====================================================================================================================
// Create 32-bit atomic and at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomicAndAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ false, /*isCmpXchg*/ false, AtomicRMWInst::And);
}

// =====================================================================================================================
// Create 32-bit atomic or at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomicOrAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ false, /*isCmpXchg*/ false, AtomicRMWInst::Or);
}

// =====================================================================================================================
// Create 64-bit atomic add at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomic64AddAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ true, /*isCmpXchg*/ false, AtomicRMWInst::Add);
}

// =====================================================================================================================
// Create 64-bit atomic max at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomic64MaxAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ true, /*isCmpXchg*/ false, AtomicRMWInst::Max);
}

// =====================================================================================================================
// Create 64-bit atomic and at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomic64AndAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ true, /*isCmpXchg*/ false, AtomicRMWInst::And);
}

// =====================================================================================================================
// Create 64-bit atomic or at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomic64OrAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ true, /*isCmpXchg*/ false, AtomicRMWInst::Or);
}

// =====================================================================================================================
// Create 64-bit atomic exchange at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomic64XchgAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ true, /*isCmpXchg*/ false, AtomicRMWInst::Xchg);
}

// =====================================================================================================================
// Create 64-bit atomic compare and exchange at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createAtomic64CmpXchgAtAddr(Function *func, Builder *builder) {
  createAtomic(func, builder, /*is64*/ true, /*isCmpXchg*/ true);
}

// =====================================================================================================================
// Create load store function cached/unchached
//
// @param func : The function to process
// @param builder : The IR builder
static void createLoadStore(Function *func, Builder *builder, bool isLoad, bool isUncached) {
  auto argIt = func->arg_begin();
  Value *arg = argIt++;
  Type *gpuVaTy = builder->getInt64Ty();
  Value *gpuAddr = builder->CreateLoad(gpuVaTy, arg);
  arg = argIt;
  Type *offsetTy = builder->getInt32Ty();
  Value *offset = builder->CreateLoad(offsetTy, arg);
  Type *gpuAddrAsPtrTy = PointerType::get(builder->getContext(), SPIRAS_Global);
  Value *gpuAddrAsPtr = builder->CreateIntToPtr(gpuAddr, gpuAddrAsPtrTy);
  gpuAddrAsPtr = builder->CreateGEP(builder->getInt8Ty(), gpuAddrAsPtr, offset);

  // Cast to the return type pointer
  Type *gpuAddrAsTy = builder->getInt32Ty();
  gpuAddrAsPtrTy = gpuAddrAsTy->getPointerTo(SPIRAS_Global);
  gpuAddrAsPtr = builder->CreateBitCast(gpuAddrAsPtr, gpuAddrAsPtrTy);

  // Load value
  if (isLoad) {
    Value *loadValue = builder->CreateLoad(gpuAddrAsTy, gpuAddrAsPtr, isUncached);
    builder->CreateRet(loadValue);
  } else {
    arg = ++argIt;
    Type *valueTy = builder->getInt32Ty();
    Value *data = builder->CreateLoad(valueTy, arg);
    builder->CreateStore(data, gpuAddrAsPtr, isUncached);
    builder->CreateRetVoid();
  }
}

// =====================================================================================================================
// Create load dword at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createLoadDwordAtAddr(Function *func, Builder *builder) {
  createLoadStore(func, builder, true, false);
}

// =====================================================================================================================
// Create load uncached dword at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createLoadDwordAtAddrUncached(Function *func, Builder *builder) {
  createLoadStore(func, builder, true, true);
}

// =====================================================================================================================
// Create store dword at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createStoreDwordAtAddr(Function *func, Builder *builder) {
  createLoadStore(func, builder, false, false);
}

// =====================================================================================================================
// Create store uncached dword at address
//
// @param func : The function to process
// @param builder : The IR builder
static void createStoreDwordAtAddrUncached(Function *func, Builder *builder) {
  createLoadStore(func, builder, false, true);
}

// =====================================================================================================================
// Create coordinates of the current fragment
//
// @param func : The function to process
// @param builder : The IR builder
static void createFragCoord(Function *func, Builder *builder) {
  builder->CreateRet(builder->CreateReadBuiltInInput(lgc::BuiltInFragCoord, {}, nullptr, nullptr, ""));
}

// =====================================================================================================================
// Create sample ID of the current fragment
//
// @param func : The function to process
// @param builder : The IR builder
static void createSampleId(Function *func, Builder *builder) {
  builder->CreateRet(builder->CreateReadBuiltInInput(lgc::BuiltInSampleId, {}, nullptr, nullptr, ""));
}

// =====================================================================================================================
// Initialize library function pointer table
InternalLibraryIntrinsicUtil::LibraryFunctionTable::LibraryFunctionTable() {
  m_libFuncPtrs["AmdExtLaneIndex"] = &createLaneIndex;
  m_libFuncPtrs["AmdExtLaneCount"] = &createLaneCount;
  m_libFuncPtrs["AmdExtHalt"] = &createHalt;
  m_libFuncPtrs["AmdExtNumWavesCompute"] = &createNumWavesCompute;
  m_libFuncPtrs["AmdExtWaveIndexCompute"] = &createWaveIndexCompute;
  m_libFuncPtrs["AmdExtGroupIdCompute"] = &createGroupIdCompute;
  m_libFuncPtrs["AmdExtGroupDimCompute"] = &createGroupDimCompute;
  m_libFuncPtrs["AmdExtThreadIdInGroupCompute"] = &createThreadIdInGroupCompute;
  m_libFuncPtrs["AmdExtFlattenedThreadIdInGroupCompute"] = &createFlattenedThreadIdInGroupCompute;
  m_libFuncPtrs["AmdExtMbcnt"] = &createMbcnt;
  m_libFuncPtrs["AmdExtAtomicAddAtAddr"] = &createAtomicAddAtAddr;
  m_libFuncPtrs["AmdExtAtomicMaxAtAddr"] = &createAtomicMaxAtAddr;
  m_libFuncPtrs["AmdExtAtomicAndAtAddr"] = &createAtomicAndAtAddr;
  m_libFuncPtrs["AmdExtAtomicOrAtAddr"] = &createAtomicOrAtAddr;
  m_libFuncPtrs["AmdExtAtomic64AddAtAddr"] = &createAtomic64AddAtAddr;
  m_libFuncPtrs["AmdExtAtomic64MaxAtAddr"] = &createAtomic64MaxAtAddr;
  m_libFuncPtrs["AmdExtAtomic64AndAtAddr"] = &createAtomic64AndAtAddr;
  m_libFuncPtrs["AmdExtAtomic64OrAtAddr"] = &createAtomic64OrAtAddr;
  m_libFuncPtrs["AmdExtAtomic64XchgAtAddr"] = &createAtomic64XchgAtAddr;
  m_libFuncPtrs["AmdExtAtomic64CmpXchgAtAddr"] = &createAtomic64CmpXchgAtAddr;
  m_libFuncPtrs["AmdExtLoadDwordAtAddr"] = &createLoadDwordAtAddr;
  m_libFuncPtrs["AmdExtLoadDwordAtAddrUncached"] = &createLoadDwordAtAddrUncached;
  m_libFuncPtrs["AmdExtStoreDwordAtAddr"] = &createStoreDwordAtAddr;
  m_libFuncPtrs["AmdExtStoreDwordAtAddrUncached"] = &createStoreDwordAtAddrUncached;
  m_libFuncPtrs["AmdExtFragCoord"] = &createFragCoord;
  m_libFuncPtrs["AmdExtSampleId"] = &createSampleId;
}

} // namespace Llpc
