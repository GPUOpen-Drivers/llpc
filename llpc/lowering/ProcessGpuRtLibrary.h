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
 * @file  ProcessGpuRtLibrary.h
 * @brief LLPC header file: contains declaration of Llpc::ProcessGpuRtLibrary
 ***********************************************************************************************************************
 */
#pragma once
#include "Lowering.h"
#include "SPIRVInternal.h"
#include "llvm/ADT/FloatingPointMode.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {

// =====================================================================================================================
// Key that fully determines the cached GPURT library module.
//
// Code run during the one-time specialization of the GPURT library module must only depend on fields in this structure.
// In particular, it must not depend directly on any fields from the pipeline context -- such fields must be passed
// through the GpurtKey structure so that we can reliably test whether a cached GPURT module can be reused.
struct GpurtKey {
  Vkgc::RtIpVersion rtipVersion;
  unsigned gpurtFeatureFlags;
  llvm::SmallVector<uint32_t, 4> bvhResDesc;

  struct {
    bool valid;
    uint32_t cpsFlags;
    std::vector<Vkgc::GpurtOption> options; // sorted by nameHash
  } rtPipeline;

  // Returns true if this key is equal to or (strictly) _refines_ the other key. A key with RT pipeline settings
  // can refine a key without if all the general settings (outside of rtPipeline) are equal.
  bool refines(const GpurtKey &other) const {
    if (!rtPipeline.valid && other.rtPipeline.valid)
      return false;
    if (rtPipeline.valid && other.rtPipeline.valid) {
      if (rtPipeline.cpsFlags != other.rtPipeline.cpsFlags)
        return false;
      if (!llvm::equal(rtPipeline.options, other.rtPipeline.options,
                       [](const Vkgc::GpurtOption &lhs, const Vkgc::GpurtOption &rhs) {
                         return lhs.nameHash == rhs.nameHash && lhs.value == rhs.value;
                       }))
        return false;
    }
    return rtipVersion == other.rtipVersion && gpurtFeatureFlags == other.gpurtFeatureFlags &&
           llvm::equal(bvhResDesc, other.bvhResDesc);
  }
};

class ProcessGpuRtLibrary : public SpirvLower, public llvm::PassInfoMixin<ProcessGpuRtLibrary> {
public:
  ProcessGpuRtLibrary(const GpurtKey &key);
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

private:
  // The key holding all the information necessary for specializing the GPURT module. No other state may be used to
  // affect the specialization, in particular no state from the pipeline context.
  const GpurtKey m_gpurtKey;

  typedef void (ProcessGpuRtLibrary::*LibraryFuncPtr)(llvm::Function *);
  struct LibraryFunctionTable {
    llvm::DenseMap<llvm::StringRef, LibraryFuncPtr> m_libFuncPtrs;
    LibraryFunctionTable();
    static const LibraryFunctionTable &get() {
      static LibraryFunctionTable instance;
      return instance;
    }
  };
  bool processLibraryFunction(llvm::Function *&func);
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
  void createLoadDwordAtAddrx3(llvm::Function *func);
  void createLoadDwordAtAddrx4(llvm::Function *func);
  void createConstantLoadDwordAtAddr(llvm::Function *func);
  void createConstantLoadDwordAtAddrx2(llvm::Function *func);
  void createConstantLoadDwordAtAddrx4(llvm::Function *func);
  void createLoadDwordAtAddrWithType(llvm::Function *func, llvm::Type *loadTy, SPIRV::SPIRAddressSpace addressSpace);
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
  void createDispatchThreadIdFlat(llvm::Function *func);
  void createGetStaticId(llvm::Function *func);
  void createInitStaticId(llvm::Function *func);
  void createGetKnownSetRayFlags(llvm::Function *func);
  void createGetKnownUnsetRayFlags(llvm::Function *func);
  void createMakePc(llvm::Function *func);
  void createContStackAlloc(llvm::Function *func);
  void createContStackFree(llvm::Function *func);
  void createContStackGetPtr(llvm::Function *func);
  void createContStackSetPtr(llvm::Function *func);
  void createContStackLoad(llvm::Function *func);
  void createContStackStore(llvm::Function *func);
  void createFloatOpWithRoundMode(llvm::Function *func);
  void createEnqueue(llvm::Function *func);
  void createIsLlpc(llvm::Function *func);
  void createGetShaderRecordIndex(llvm::Function *func);
  void createShaderMarker(llvm::Function *func);
  void createWaveScan(llvm::Function *func);
#if LLPC_BUILD_GFX12
  void createDualIntersectRay(llvm::Function *func);
  void createIntersectRayBvh8(llvm::Function *func);
  void createDsStackPush8Pop1(llvm::Function *func);
  void createDsStackPush8Pop2(llvm::Function *func);
  void createIntersectRay(llvm::Function *func, bool isDualNode);
  void createDsStackPush8Pop1PrimRangeEnabled(llvm::Function *func);
  void createDsStackPush8PopN(llvm::Function *func, unsigned returnNodeCount, bool primRangeEnable);
#endif
  llvm::Value *createGetBvhSrd(llvm::Value *expansion, llvm::Value *boxSortMode);
};
} // namespace Llpc
