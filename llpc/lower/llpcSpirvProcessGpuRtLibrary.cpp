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
 * @file  llpcSpirvProcessGpuRtLibrary.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvProcessGpuRtLibrary.
 ***********************************************************************************************************************
 */
#include "llpcSpirvProcessGpuRtLibrary.h"
#include "SPIRVInternal.h"
#include "compilerutils/ArgPromotion.h"
#include "compilerutils/CompilerUtils.h"
#include "compilerutils/TypesMetadata.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "llpcSpirvLowerUtil.h"
#include "llvmraytracing/Continuations.h"
#include "lgc/Builder.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/ValueSymbolTable.h"

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

  // Process each function.
  SmallVector<std::pair<Function *, SmallBitVector>> argPromotionsFuncs;
  auto rtipVersion = m_context->getPipelineContext()->getRayTracingState()->rtIpVersion;
  unsigned rtip = rtipVersion.major * 10 + rtipVersion.minor;
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
    if (funcName.starts_with("TraceRayInline"))
      argPromotions.set(1, 8);
    else if (funcName.starts_with("RayQueryProceed"))
      argPromotions.set(1, 3);
    else if (funcName.starts_with("FetchTrianglePositionFromRayQuery"))
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

    // This is a rayQuery function, and we have the args requiring promotion in the argPromotions bit vector.
    // Parse off the RTIP suffix if any, e.g. "2_0", into a two-digit decimal number, e.g. 20.
    // Ignore BVH8 funcs.
    if (funcName.ends_with("BVH8"))
      continue;
    StringRef funcSuffix = funcName.take_back(3);
    unsigned funcRtip = 0;
    if (funcSuffix.size() == 3 && isdigit(funcSuffix[0]) && funcSuffix[1] == '_' && isdigit(funcSuffix[2])) {
      funcRtip = (funcSuffix[0] - '0') * 10 + (funcSuffix[2] - '0');
      funcName = funcName.drop_back(funcSuffix.size());
    }
    // If this function has an RTIP suffix but it is wrong, ignore it (leaving it as internal linkage so it gets
    // removed later).
    if (funcRtip != 0 && funcRtip != rtip)
      continue;

    if (funcRtip != 0) {
      // We have a function with the correct RTIP suffix. We want to rename it without the RTIP suffix.
      // If there is another function of the same name without the RTIP suffix, take its name and make the
      // other function internal so it gets removed later. (This works whether we saw that function first or
      // this RTIP-suffixed one.)
      if (Function *otherFunc = module.getFunction(funcName)) {
        otherFunc->setLinkage(GlobalValue::InternalLinkage);
        func.takeName(otherFunc);
      } else {
        // No other function. Set name the normal way. Note use of str() to copy the unsuffixed name out
        // before setName() frees it.
        func.setName(funcName.str());
      }
    }
    // Set external linkage on this function.
    func.setLinkage(GlobalValue::WeakAnyLinkage);

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
    Function *promotedFunc = CompilerUtils::promotePointerArguments(func, argPromotionsFunc.second);
    promotedFunc->setLinkage(GlobalValue::WeakAnyLinkage);
  }

  // Process ray-tracing (i.e. non-rayQuery) functions in a separate loop; processLibraryFunction() may do
  // arg promotion, so we cannot do it in the same loop.
  for (Function *func : maybeRtFuncs)
    processLibraryFunction(func);

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
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConstantLoadDwordAtAddr"] =
      &SpirvProcessGpuRtLibrary::createConstantLoadDwordAtAddr;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConstantLoadDwordAtAddrx2"] =
      &SpirvProcessGpuRtLibrary::createConstantLoadDwordAtAddrx2;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConstantLoadDwordAtAddrx4"] =
      &SpirvProcessGpuRtLibrary::createConstantLoadDwordAtAddrx4;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConvertF32toF16NegInf"] =
      &SpirvProcessGpuRtLibrary::createConvertF32toF16NegInf;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ConvertF32toF16PosInf"] =
      &SpirvProcessGpuRtLibrary::createConvertF32toF16PosInf;
#if GPURT_CLIENT_INTERFACE_MAJOR_VERSION < 33
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_IntersectBvhNode"] = &SpirvProcessGpuRtLibrary::createIntersectBvh;
#else
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_IntersectInternal"] = &SpirvProcessGpuRtLibrary::createIntersectBvh;
#endif
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_ShaderMarker"] = &SpirvProcessGpuRtLibrary::createShaderMarker;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_WaveScan"] = &SpirvProcessGpuRtLibrary::createWaveScan;
  m_libFuncPtrs["AmdExtD3DShaderIntrinsics_FloatOpWithRoundMode"] =
      &SpirvProcessGpuRtLibrary::createFloatOpWithRoundMode;
  m_libFuncPtrs["AmdExtDispatchThreadIdFlat"] = &SpirvProcessGpuRtLibrary::createDispatchThreadIdFlat;
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
  m_libFuncPtrs["_AmdContinuationStackIsGlobal"] = &SpirvProcessGpuRtLibrary::createContinuationStackIsGlobal;
  m_libFuncPtrs["_AmdGetRtip"] = &SpirvProcessGpuRtLibrary::createGetRtip;
}

// =====================================================================================================================
// Clear the block before patching the function
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::processLibraryFunction(Function *&func) {
  auto funcName = func->getName();

  // Special handling for _AmdContStackStore* and _AmdContStackLoad* to accept arbitrary type
  if (funcName.starts_with("_AmdContStackStore")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createContStackStore(func);
    return;
  } else if (funcName.starts_with("_AmdContStackLoad")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createContStackLoad(func);
    return;
  } else if (funcName.starts_with("_AmdEnqueue") || funcName.starts_with("_AmdWaitEnqueue")) {
    m_builder->SetInsertPoint(clearBlock(func));
    createEnqueue(func);
    return;
  } else if (funcName.starts_with("_AmdGetUninitialized")) {
    m_builder->SetInsertPoint(clearBlock(func));
    Value *FrozenPoison = m_builder->CreateFreeze(PoisonValue::get(func->getReturnType()));
    m_builder->CreateRet(FrozenPoison);
    return;
  } else if (funcName.starts_with("_AmdRestoreSystemData")) {
    // We don't need this, leave it as dummy function so that it does nothing.
    return;
  } else if (funcName.starts_with("_AmdGetSetting")) {
    auto rtContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());
    SmallVector<ContSetting> contSettings;
    for (unsigned i = 0; i < rtContext->getRayTracingPipelineBuildInfo()->gpurtOptionCount; i++) {
      ContSetting setting;
      setting.NameHash = rtContext->getRayTracingPipelineBuildInfo()->pGpurtOptions[i].nameHash;
      setting.Value = rtContext->getRayTracingPipelineBuildInfo()->pGpurtOptions[i].value;
      contSettings.push_back(setting);
    }
    ContHelper::handleGetSetting(*func, contSettings);
    return;
  } else if (funcName.starts_with("_AmdValueI32Count")) {
    ContHelper::handleValueI32Count(*func, *m_builder);
    return;
  } else if (funcName.starts_with("_AmdValueGetI32") || funcName.starts_with("_AmdValueSetI32")) {
    // The intrinsic handling require first argument to be a pointer, the rest to be values.
    SmallBitVector promotionMask(func->arg_size(), true);
    promotionMask.reset(0);
    auto newFunc = CompilerUtils::promotePointerArguments(func, promotionMask);
    if (funcName.starts_with("_AmdValueGetI32"))
      ContHelper::handleValueGetI32(*newFunc, *m_builder);
    else
      ContHelper::handleValueSetI32(*newFunc, *m_builder);
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
    return;
  }

  bool isAmdAwaitLike = funcName.starts_with("_AmdAwait") || funcName.starts_with("_AmdWaitAwait");
  // NOTE: GPURT now preserves all function names started with "_Amd", but some of them are not intrinsics, e.g.,
  // "_AmdSystemData.IsTraversal", which are methods of system data structs. Skip those to let them be inlined
  // automatically.
  bool isAmdIntrinsic = funcName.starts_with("_Amd") && !funcName.contains(".");
  if (funcName.starts_with("_cont_") || isAmdIntrinsic) {
    // This function is provided by GPURT to the compiler.
    if (!isAmdIntrinsic)
      func->setLinkage(GlobalValue::WeakAnyLinkage);

    // Skip _AmdAwaitTraversal function resulting from calls to _AmdWaitAwaitTraversal.
    if (!func->hasMetadata(TypedFuncTy::MDTypesName) && !func->arg_empty())
      return;

    SmallBitVector promotionMask(func->arg_size());
    for (unsigned argNo = 0; argNo < func->arg_size(); argNo++) {
      auto *arg = func->getArg(argNo);
      TypedArgTy argTy = TypedArgTy::get(arg);
      auto funcName = func->getName();

      if (!argTy.isPointerTy())
        continue;

      // Change the pointer type to its value type for non-struct types.
      // Amd*Await, use value types for all arguments.
      // For _cont_SetTriangleHitAttributes, we always use its value type for hitAttributes argument.
      if (!isa<StructType>(argTy.getPointerElementType()) || isAmdAwaitLike ||
          (funcName == ContDriverFunc::SetTriangleHitAttributesName && argNo == 1))
        promotionMask.set(argNo);
    }

    auto *newFunc = CompilerUtils::promotePointerArguments(func, promotionMask);

    // This function is provided by the compiler to GPURT. It will be substituted by LowerRaytracingPipeline.
    if (isAmdIntrinsic)
      newFunc->deleteBody();

    if (newFunc->getName().starts_with("_AmdWaitAwait")) {
      llvm::forEachCall(*newFunc, [&](CallInst &CInst) {
        SmallVector<Value *> args(CInst.args());
        // NOTE: Theoretically we should remove the wait mask so that the function signature matches
        // _AmdAwait*(addr, returnAddr, SystemData, ...). However, _AmdWaitAwaitTraversal's arguments are defined as
        // (addr, waitMask, SystemData, ...), thus we need to keep the waitMask as a dummy returnAddr so that
        // LowerRaytracingPipeline can handle it correctly.
        if (!newFunc->getName().starts_with("_AmdWaitAwaitTraversal"))
          args.erase(args.begin() + 1);

        m_builder->SetInsertPoint(&CInst);
        auto *newValue = m_builder->CreateNamedCall("_AmdAwait", CInst.getType(), args, {});
        CInst.replaceAllUsesWith(newValue);
        CInst.eraseFromParent();
      });
    }

    return;
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
  m_builder->CreateRet(m_builder->create<GpurtLdsStackInitOp>(false));
}

// =====================================================================================================================
void SpirvProcessGpuRtLibrary::createFloatOpWithRoundMode(llvm::Function *func) {
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
void SpirvProcessGpuRtLibrary::createLdsStackStore(Function *func) {
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
// Fill in function to global load 1 dword at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddr(Function *func) {
  createLoadDwordAtAddrWithType(func, m_builder->getInt32Ty(), SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to global load 2 dwords at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddrx2(Function *func) {
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  createLoadDwordAtAddrWithType(func, int32x2Ty, SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to global load 4 dwords at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddrx4(Function *func) {
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  createLoadDwordAtAddrWithType(func, int32x4Ty, SPIRAS_Global);
}

// =====================================================================================================================
// Fill in function to constant load 1 dword at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createConstantLoadDwordAtAddr(Function *func) {
  createLoadDwordAtAddrWithType(func, m_builder->getInt32Ty(), SPIRAS_Constant);
}

// =====================================================================================================================
// Fill in function to constant load 2 dwords at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createConstantLoadDwordAtAddrx2(Function *func) {
  auto int32x2Ty = FixedVectorType::get(m_builder->getInt32Ty(), 2);
  createLoadDwordAtAddrWithType(func, int32x2Ty, SPIRAS_Constant);
}

// =====================================================================================================================
// Fill in function to constant load 4 dwords at given address
//
// @param func : The function to process
void SpirvProcessGpuRtLibrary::createConstantLoadDwordAtAddrx4(Function *func) {
  auto int32x4Ty = FixedVectorType::get(m_builder->getInt32Ty(), 4);
  createLoadDwordAtAddrWithType(func, int32x4Ty, SPIRAS_Constant);
}

// =====================================================================================================================
// Fill in function to load dwords at given address based on given type
//
// @param func : The function to process
// @param loadTy : Load type
void SpirvProcessGpuRtLibrary::createLoadDwordAtAddrWithType(Function *func, Type *loadTy,
                                                             SPIRAddressSpace addressSpace) {
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
  m_builder->CreateRet(m_builder->create<GpurtGetKnownSetRayFlagsOp>());
}

// =====================================================================================================================
// Fill in function to get known unset ray flags
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetKnownUnsetRayFlags(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtGetKnownUnsetRayFlagsOp>());
}

// =====================================================================================================================
// Fill in function of AmdExtDispatchThreadIdFlat
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createDispatchThreadIdFlat(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtDispatchThreadIdFlatOp>());
}
// =====================================================================================================================
// Fill in function to allocate continuation stack pointer
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContStackAlloc(llvm::Function *func) {
  assert(func->arg_size() == 1);
  Value *byteSize = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
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
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createEnqueue(Function *func) {
  auto funcName = func->getName();

  Value *addr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));

  SmallVector<Value *> tailArgs;
  bool hasWaitMaskArg = funcName.contains("Wait");
  // Skip waitMask
  unsigned retAddrArgIdx = hasWaitMaskArg ? 2 : 1;
  tailArgs.push_back(m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(retAddrArgIdx)));
  // Get shader-index from system-data.
  unsigned systemDataArgIdx = retAddrArgIdx + 1;
  tailArgs.push_back(m_builder->CreateNamedCall("_cont_GetLocalRootIndex", m_builder->getInt32Ty(),
                                                {func->getArg(systemDataArgIdx)}, {}));
  // Process system-data and arguments after.
  unsigned argIdx = systemDataArgIdx;
  while (argIdx < func->arg_size()) {
    tailArgs.push_back(m_builder->CreateLoad(getFuncArgPtrElementType(func, argIdx), func->getArg(argIdx)));
    argIdx++;
  }

  // TODO: pass the levelMask correctly.
  m_builder->create<cps::JumpOp>(addr, -1, PoisonValue::get(StructType::get(*m_context, {})), tailArgs);
  m_builder->CreateUnreachable();
}

// Fill in function to check whether continuation stack is global
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createContinuationStackIsGlobal(llvm::Function *func) {
  m_builder->CreateRet(m_builder->create<GpurtContinuationStackIsGlobalOp>());
}

// =====================================================================================================================
// Fill in function to get RTIP
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createGetRtip(llvm::Function *func) {
  auto rtip = m_context->getPipelineContext()->getRayTracingState()->rtIpVersion;
  // The version is encoded as <major><minor> in decimal digits, so 11 is rtip 1.1, 20 is rtip 2.0
  m_builder->CreateRet(m_builder->getInt32(rtip.major * 10 + rtip.minor));
}

// =====================================================================================================================
// Fill in function to write shader marker
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createShaderMarker(llvm::Function *func) {
  Value *dataPtr = m_builder->CreateLoad(m_builder->getInt32Ty(), func->getArg(0));
  m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_ttracedata, {}, dataPtr);
  m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Fill in function to write wave scan
//
// @param func : The function to create
void SpirvProcessGpuRtLibrary::createWaveScan(llvm::Function *func) {
  auto argIt = func->arg_begin();
  auto retType = cast<FixedVectorType>(func->getReturnType());
  auto int32Ty = m_builder->getInt32Ty();
  Value *waveOp = m_builder->CreateLoad(int32Ty, argIt++);
  Value *flags = m_builder->CreateLoad(int32Ty, argIt++);
  Value *src0 = m_builder->CreateLoad(retType, argIt);
  m_builder->CreateRet(m_builder->create<GpurtWaveScanOp>(waveOp, flags, src0));
}

} // namespace Llpc
