/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MiscBuilder.cpp
 * @brief LLPC source file: implementation of miscellaneous Builder methods
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-builder-impl-misc"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
// the current output primitive in the specified output-primitive stream number.
//
// @param streamId : Stream number, 0 if only one stream is present
Instruction *BuilderImpl::CreateEmitVertex(unsigned streamId) {
  assert(m_shaderStage == ShaderStage::Geometry);

  // Mark this vertex stream as active if transform feedback is enabled, or primitive statistics counting is enabled,
  // or this is the rasterization stream.
  if (m_pipelineState->enableXfb() || m_pipelineState->enablePrimStats() ||
      m_pipelineState->getRasterizerState().rasterStream == streamId)
    m_pipelineState->setVertexStreamActive(streamId);

  // Get GsWaveId
  std::string callName = lgcName::InputImportBuiltIn;
  callName += "GsWaveId.i32.i32";
  Value *gsWaveId = CreateNamedCall(callName, getInt32Ty(), getInt32(BuiltInGsWaveId), {});

  // Do the sendmsg.
  // [9:8] = stream, [5:4] = 2 (emit), [3:0] = 2 (GS)
  unsigned msg = (streamId << GsEmitCutStreamIdShift) | GsEmit;
  return CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {getInt32(msg), gsWaveId}, nullptr);
}

// =====================================================================================================================
// In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
//
// @param streamId : Stream number, 0 if only one stream is present
Instruction *BuilderImpl::CreateEndPrimitive(unsigned streamId) {
  assert(m_shaderStage == ShaderStage::Geometry);

  // Mark this vertex stream as active if transform feedback is enabled, or primitive statistics counting is enabled,
  // or this is the rasterization stream.
  if (m_pipelineState->enableXfb() || m_pipelineState->enablePrimStats() ||
      m_pipelineState->getRasterizerState().rasterStream == streamId)
    m_pipelineState->setVertexStreamActive(streamId);

  // Get GsWaveId
  std::string callName = lgcName::InputImportBuiltIn;
  callName += "GsWaveId.i32.i32";
  Value *gsWaveId = CreateNamedCall(callName, getInt32Ty(), getInt32(BuiltInGsWaveId), {});

  // Do the sendmsg.
  // [9:8] = stream, [5:4] = 1 (cut), [3:0] = 2 (GS)
  unsigned msg = (streamId << GsEmitCutStreamIdShift) | GsCut;
  return CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {getInt32(msg), gsWaveId}, nullptr);
}

// =====================================================================================================================
// Create a workgroup control barrier.
Instruction *BuilderImpl::CreateBarrier() {

  return CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
}

// =====================================================================================================================
// Create a "kill". Only allowed in a fragment shader.
//
// @param instName : Name to give instruction(s)
Instruction *BuilderImpl::CreateKill(const Twine &instName) {
  // This tells the config builder to set KILL_ENABLE in DB_SHADER_CONTROL.
  // Doing it here is suboptimal, as it does not allow for subsequent middle-end optimizations removing the
  // section of code containing the kill.
  auto resUsage = getPipelineState()->getShaderResourceUsage(ShaderStage::Fragment);
  resUsage->builtInUsage.fs.discard = true;

  return CreateIntrinsic(Intrinsic::amdgcn_kill, {}, getFalse(), nullptr, instName);
}

// =====================================================================================================================
// Create a "system halt"
//
// @param instName : Name to give instruction(s)
Instruction *BuilderCommon::CreateDebugBreak(const Twine &instName) {
  return CreateIntrinsic(Intrinsic::amdgcn_s_sethalt, {}, getInt32(1), nullptr, instName);
}

// =====================================================================================================================
// Create a demote to helper invocation operation. Only allowed in a fragment shader.
//
// @param instName : Name to give instruction(s)
Instruction *BuilderImpl::CreateDemoteToHelperInvocation(const Twine &instName) {
  // Treat a demote as a kill for the purposes of disabling middle-end optimizations.
  auto resUsage = getPipelineState()->getShaderResourceUsage(ShaderStage::Fragment);
  resUsage->builtInUsage.fs.discard = true;

  return CreateIntrinsic(Intrinsic::amdgcn_wqm_demote, {}, getFalse(), nullptr, instName);
}

// =====================================================================================================================
// Create a helper invocation query. Only allowed in a fragment shader.
//
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateIsHelperInvocation(const Twine &instName) {
  auto isLive = CreateIntrinsic(Intrinsic::amdgcn_live_mask, {}, {}, nullptr, instName);
  return CreateNot(isLive);
}

// =====================================================================================================================
// Create a "readclock".
//
// @param realtime : Whether to read real-time clock counter
// @param instName : Name to give instruction(s)
Instruction *BuilderImpl::CreateReadClock(bool realtime, const Twine &instName) {
  CallInst *readClock = nullptr;
  if (realtime) {
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 11)
      readClock =
          CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg_rtn, getInt64Ty(), getInt32(GetRealTime), nullptr, instName);
    else
      readClock = CreateIntrinsic(Intrinsic::amdgcn_s_memrealtime, {}, {}, nullptr, instName);
  } else
    readClock = CreateIntrinsic(Intrinsic::readcyclecounter, {}, {}, nullptr, instName);
  readClock->setOnlyReadsMemory();

  // NOTE: The inline ASM is to prevent optimization of backend compiler.
  InlineAsm *asmFunc = InlineAsm::get(FunctionType::get(getInt64Ty(), {getInt64Ty()}, false), "; %1", "=r,0", true);

  readClock = CreateCall(asmFunc, {readClock});

  return readClock;
}

// =====================================================================================================================
// Create derivative calculation on float or vector of float or half
//
// @param value : Input value
// @param isDirectionY : False for derivative in X direction, true for Y direction
// @param isFine : True for "fine" calculation, where the value in the current fragment is used. False for "coarse"
// calculation, where it might use fewer locations to calculate.
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateDerivative(Value *value, bool isDirectionY, bool isFine, const Twine &instName) {
  unsigned tableIdx = isDirectionY * 2 + isFine;
  Value *result = nullptr;

  // DPP (GFX9+) version.
  // For quad pixels, quad_perm:[pix0,pix1,pix2,pix3] = [0,1,2,3]
  // Table of first dpp_ctrl, in order coarseX, fineX, coarseY, fineY
  static const unsigned FirstDppCtrl[4] = {
      0x55, // CoarseX: [0,1,2,3] -> [1,1,1,1]
      0xF5, // FineX:   [0,1,2,3]->[1,1,3,3]
      0xAA, // CoarseY: [0,1,2,3] -> [2,2,2,2]
      0xEE, // FineY:   [0,1,2,3]->[2,3,2,3]
  };
  // Table of second dpp_ctrl, in order coarseX, fineX, coarseY, fineY
  static const unsigned SecondDppCtrl[4] = {
      0x00, // CoarseX: [0,1,2,3]->[0,0,0,0]
      0xA0, // FineX:   [0,1,2,3]->[0,0,2,2]
      0x00, // CoarseY: [0,1,2,3]->[0,0,0,0]
      0x44, // FineY:   [0,1,2,3]->[0,1,0,1]
  };
  unsigned perm1 = FirstDppCtrl[tableIdx];
  unsigned perm2 = SecondDppCtrl[tableIdx];
  result = scalarize(value, [this, perm1, perm2](Value *value) {
    Type *valTy = value->getType();
    value = CreateBitCast(value, getIntNTy(valTy->getPrimitiveSizeInBits()));
    value = CreateZExtOrTrunc(value, getInt32Ty());
    Value *firstVal = CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, getInt32Ty(),
                                      {value, getInt32(perm1), getInt32(15), getInt32(15), getTrue()});
    firstVal = CreateZExtOrTrunc(firstVal, getIntNTy(valTy->getPrimitiveSizeInBits()));
    firstVal = CreateBitCast(firstVal, valTy);
    Value *secondVal = CreateIntrinsic(Intrinsic::amdgcn_mov_dpp, getInt32Ty(),
                                       {value, getInt32(perm2), getInt32(15), getInt32(15), getTrue()});
    secondVal = CreateZExtOrTrunc(secondVal, getIntNTy(valTy->getPrimitiveSizeInBits()));
    secondVal = CreateBitCast(secondVal, valTy);
    Value *result = CreateFSub(firstVal, secondVal);
    return CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, result);
  });

  result->setName(instName);
  return result;
}
