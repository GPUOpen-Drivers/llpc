/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayTracingIntrinsics.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerRayTracingIntrinsics.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerRayTracingIntrinsics.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"

#define DEBUG_TYPE "llpc-spirv-lower-ray-tracing-intrinsics"

using namespace llvm;
using namespace Llpc;

namespace RtName {
const char *LoadDwordAtAddr = "AmdExtD3DShaderIntrinsics_LoadDwordAtAddr";
const char *LoadDwordAtAddrx2 = "AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx2";
const char *LoadDwordAtAddrx4 = "AmdExtD3DShaderIntrinsics_LoadDwordAtAddrx4";
const char *ConvertF32toF16NegInf = "AmdExtD3DShaderIntrinsics_ConvertF32toF16NegInf";
const char *ConvertF32toF16PosInf = "AmdExtD3DShaderIntrinsics_ConvertF32toF16PosInf";
} // namespace RtName

namespace Llpc {

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerRayTracingIntrinsics::run(Module &module, ModuleAnalysisManager &analysisManager) {
  if (runImpl(module))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerRayTracingIntrinsics::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Ray-Tracing-Intrinsics\n");

  SpirvLower::init(&module);

  bool changed = false;

  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    changed |= processIntrinsicsFunction(func);
  }

  return changed;
}

// =====================================================================================================================
// Process intrinsics function in the module
//
// @param func : The function to process
bool SpirvLowerRayTracingIntrinsics::processIntrinsicsFunction(Function *func) {
  bool changed = false;
  auto mangledName = func->getName();
  if (mangledName.equals(RtName::LoadDwordAtAddr)) {
    createLoadDwordAtAddr(func, m_builder->getInt32Ty());
    changed = true;
  } else if (mangledName.equals(RtName::LoadDwordAtAddrx2)) {
    auto int32x2Ty = FixedVectorType::get(Type::getInt32Ty(*m_context), 2);
    createLoadDwordAtAddr(func, int32x2Ty);
    changed = true;
  } else if (mangledName.equals(RtName::LoadDwordAtAddrx4)) {
    auto int32x4Ty = FixedVectorType::get(Type::getInt32Ty(*m_context), 4);
    createLoadDwordAtAddr(func, int32x4Ty);
    changed = true;
  } else if (mangledName.equals(RtName::ConvertF32toF16NegInf)) {
    // RM = fp::rmDownward;
    createConvertF32toF16(func, 2);
    changed = true;
  } else if (mangledName.equals(RtName::ConvertF32toF16PosInf)) {
    // RM = fp::rmUpward;
    createConvertF32toF16(func, 3);
    changed = true;
  }

  // TODO: Add support for other intrinsics function if needed.

  return changed;
}

// =====================================================================================================================
// Create AmdExtD3DShaderIntrinsics_LoadDwordAtAddr, LoadDwordAtAddrx2, LoadDwordAtAddrx4,
//
// @param func : Function to create
// @param loadTy : Base type of the load value
void SpirvLowerRayTracingIntrinsics::createLoadDwordAtAddr(Function *func, Type *loadTy) {
  assert(func->size() == 1);
  (*func->begin()).eraseFromParent();

  Type *loadPtrTy = loadTy->getPointerTo(SPIRAS_Global);

  BasicBlock *entryBlock = BasicBlock::Create(m_builder->getContext(), "", func);
  m_builder->SetInsertPoint(entryBlock);
  auto argIt = func->arg_begin();

  Value *gpuLowAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *gpuHighAddr = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);
  Value *offset = m_builder->CreateLoad(m_builder->getInt32Ty(), argIt++);

  // Use (gpuLowAddr, gpuHighAddr) to calculate i64 gpuAddr
  gpuLowAddr = m_builder->CreateZExt(gpuLowAddr, m_builder->getInt64Ty());
  gpuHighAddr = m_builder->CreateZExt(gpuHighAddr, m_builder->getInt64Ty());
  gpuHighAddr = m_builder->CreateShl(gpuHighAddr, m_builder->getInt64(32));
  Value *gpuAddr = m_builder->CreateOr(gpuLowAddr, gpuHighAddr);

  Type *gpuAddrAsPtrTy = Type::getInt8PtrTy(m_builder->getContext(), SPIRAS_Global);
  auto gpuAddrAsPtr = m_builder->CreateIntToPtr(gpuAddr, gpuAddrAsPtrTy);

  // Create GEP to get the byte address with byte offset
  Value *loadValue = m_builder->CreateGEP(m_builder->getInt8Ty(), gpuAddrAsPtr, offset);
  // Cast to the return type pointer
  loadValue = m_builder->CreateBitCast(loadValue, loadPtrTy);

  loadValue = m_builder->CreateLoad(loadTy, loadValue);
  m_builder->CreateRet(loadValue);
}

// =====================================================================================================================
// Create AmdExtD3DShaderIntrinsics_ConvertF32toF16NegInf, AmdExtD3DShaderIntrinsics_ConvertF32toF16PosInf
//
// @param func : Function to create
// @param roundingMode : Rounding mode for the conversion
void SpirvLowerRayTracingIntrinsics::createConvertF32toF16(Function *func, unsigned roundingMode) {
  // uint3 AmdExtD3DShaderIntrinsics_ConvertF32toF16NegInf/PosInf(in float3 inVec)
  // {
  //   return uint3(f32tof16NegInf/PosInf(inVec));
  // }

  assert(func->size() == 1);
  (*func->begin()).eraseFromParent();

  BasicBlock *entryBlock = BasicBlock::Create(m_builder->getContext(), "", func);
  m_builder->SetInsertPoint(entryBlock);
  auto argIt = func->arg_begin();

  Type *convertInputType = FixedVectorType::get(m_builder->getFloatTy(), 3);
  // TODO: Remove this when LLPC will switch fully to opaque pointers.
  assert(IS_OPAQUE_OR_POINTEE_TYPE_MATCHES(argIt->getType(), convertInputType));
  Value *inVec = m_builder->CreateLoad(convertInputType, argIt);
  // TODO: Backend currently does not support rounding mode correctly. LGC is also treating all rounding mode other than
  // RTE as RTZ. We need RTN and RTP here. LGC needs a change after backend confirm the support of rounding mode.
  Value *result = m_builder->CreateFpTruncWithRounding(inVec, FixedVectorType::get(m_builder->getHalfTy(), 3),
                                                       static_cast<RoundingMode>(roundingMode));

  result = m_builder->CreateBitCast(result, FixedVectorType::get(m_builder->getInt16Ty(), 3));
  result = m_builder->CreateZExt(result, FixedVectorType::get(m_builder->getInt32Ty(), 3));

  m_builder->CreateRet(result);
}

} // namespace Llpc
