/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ShaderMerger.cpp
 * @brief LLPC source file: contains implementation of class lgc::ShaderMerger.
 ***********************************************************************************************************************
 */
#include "ShaderMerger.h"
#include "NggPrimShader.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "lgc-shader-merger"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
// @param pipelineShaders : API shaders in the pipeline
ShaderMerger::ShaderMerger(PipelineState *pipelineState, PipelineShadersResult *pipelineShaders)
    : m_pipelineState(pipelineState), m_context(&pipelineState->getContext()),
      m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()) {
  assert(m_gfxIp.major >= 9);
  assert(m_pipelineState->isGraphics());

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
}

// =====================================================================================================================
// Get the index of the specified special SGPR input according to graphics IP version (LS-HS merged shader).
//
// @param gfxIp : Graphics IP version
// @param sgprInput : Special SGPR input
// @returns : Index of the specified special SGPR input
unsigned ShaderMerger::getSpecialSgprInputIndex(GfxIpVersion gfxIp, LsHs::SpecialSgprInput sgprInput) {
  // Index map of special SGPR inputs of LS-HS merged shader
  static const std::unordered_map<LsHs::SpecialSgprInput, unsigned> LsHsSpecialSgprInputMapGfx9 = {
      {LsHs::UserDataAddrLow, 0},     // s0
      {LsHs::UserDataAddrHigh, 1},    // s1
      {LsHs::OffChipLdsBase, 2},      // s2
      {LsHs::MergedWaveInfo, 3},      // s3
      {LsHs::TfBufferBase, 4},        // s4
      {LsHs::SharedScratchOffset, 5}, // s5
      {LsHs::HsShaderAddrLow, 6},     // s6
      {LsHs::HsShaderAddrHigh, 7},    // s7
  };

  assert(gfxIp.major >= 9); // Must be GFX9+

  assert(LsHsSpecialSgprInputMapGfx9.count(sgprInput) > 0);
  return LsHsSpecialSgprInputMapGfx9.at(sgprInput);
}

// =====================================================================================================================
// Get the index of the specified special SGPR input according to graphics IP version (ES-GS merged shader).
//
// @param gfxIp : Graphics IP version
// @param sgprInput : Special SGPR input
// @returns : Index of the specified special SGPR input
unsigned ShaderMerger::getSpecialSgprInputIndex(GfxIpVersion gfxIp, EsGs::SpecialSgprInput sgprInput, bool useNgg) {
  // Index map of special SGPR inputs of ES-GS merged shader
  static const std::unordered_map<EsGs::SpecialSgprInput, unsigned> EsGsSpecialSgprInputMapGfx9 = {
      {EsGs::UserDataAddrLow, 0},     // s0
      {EsGs::UserDataAddrHigh, 1},    // s1
      {EsGs::GsVsOffset, 2},          // s2
      {EsGs::MergedWaveInfo, 3},      // s3
      {EsGs::OffChipLdsBase, 4},      // s4
      {EsGs::SharedScratchOffset, 5}, // s5
      {EsGs::GsShaderAddrLow, 6},     // s6
      {EsGs::GsShaderAddrHigh, 7},    // s7
  };

  static const std::unordered_map<EsGs::SpecialSgprInput, unsigned> EsGsSpecialSgprInputMapGfx10 = {
      {EsGs::UserDataAddrLow, 0},     // s0
      {EsGs::UserDataAddrHigh, 1},    // s1
      {EsGs::MergedGroupInfo, 2},     // s2
      {EsGs::MergedWaveInfo, 3},      // s3
      {EsGs::OffChipLdsBase, 4},      // s4
      {EsGs::SharedScratchOffset, 5}, // s5
      {EsGs::GsShaderAddrLow, 6},     // s6
      {EsGs::GsShaderAddrHigh, 7},    // s7
  };

  assert(gfxIp.major >= 9); // Must be GFX9+

  if (gfxIp.major >= 10) {
    if (useNgg) {
      assert(EsGsSpecialSgprInputMapGfx10.count(sgprInput) > 0);
      return EsGsSpecialSgprInputMapGfx10.at(sgprInput);
    }
  }

  assert(EsGsSpecialSgprInputMapGfx9.count(sgprInput) > 0);
  return EsGsSpecialSgprInputMapGfx9.at(sgprInput);
}

// =====================================================================================================================
// Builds LLVM function for hardware primitive shader (NGG).
//
// @param esEntryPoint : Entry-point of hardware export shader (ES) (could be null)
// @param gsEntryPoint : Entry-point of hardware geometry shader (GS) (could be null)
// @param copyShaderEntryPoint : Entry-point of hardware vertex shader (VS, copy shader) (could be null)
Function *ShaderMerger::buildPrimShader(Function *esEntryPoint, Function *gsEntryPoint,
                                        Function *copyShaderEntryPoint) {
#if VKI_RAY_TRACING
  processRayQueryLdsStack(esEntryPoint, gsEntryPoint);
#endif

  NggPrimShader primShader(m_pipelineState);
  return primShader.generate(esEntryPoint, gsEntryPoint, copyShaderEntryPoint);
}

// =====================================================================================================================
// Generates the type for the new entry-point of LS-HS merged shader.
//
// @param [out] inRegMask : "Inreg" bit mask for the arguments
FunctionType *ShaderMerger::generateLsHsEntryPointType(uint64_t *inRegMask) const {
  assert(m_hasVs || m_hasTcs);

  std::vector<Type *> argTys;

  // First 8 system values (SGPRs)
  for (unsigned i = 0; i < NumSpecialSgprInputs; ++i) {
    argTys.push_back(Type::getInt32Ty(*m_context));
    *inRegMask |= (1ull << i);
  }

  // User data (SGPRs)
  unsigned userDataCount = 0;
  if (m_hasVs) {
    const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
    userDataCount = std::max(intfData->userDataCount, userDataCount);
  }

  if (m_hasTcs) {
    const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl);
    userDataCount = std::max(intfData->userDataCount, userDataCount);
  }

  if (m_hasTcs && m_hasVs) {
    auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
    auto tcsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl);

    if (vsIntfData->spillTable.sizeInDwords == 0 && tcsIntfData->spillTable.sizeInDwords > 0) {
      vsIntfData->userDataUsage.spillTable = userDataCount;
      ++userDataCount;
      assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
    }
  }

  assert(userDataCount > 0);
  argTys.push_back(FixedVectorType::get(Type::getInt32Ty(*m_context), userDataCount));
  *inRegMask |= (1ull << NumSpecialSgprInputs);

  // Other system values (VGPRs)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID (control point ID included)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Vertex ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // Relative vertex ID (auto index)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Step rate
  argTys.push_back(Type::getInt32Ty(*m_context)); // Instance ID

  appendVertexFetchTypes(argTys);
  return FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for LS-HS merged shader.
//
// @param lsEntryPoint : Entry-point of hardware local shader (LS) (could be null)
// @param hsEntryPoint : Entry-point of hardware hull shader (HS)
Function *ShaderMerger::generateLsHsEntryPoint(Function *lsEntryPoint, Function *hsEntryPoint) {
  if (lsEntryPoint) {
    lsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    lsEntryPoint->addFnAttr(Attribute::AlwaysInline);
  }

  assert(hsEntryPoint);
  hsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
  hsEntryPoint->addFnAttr(Attribute::AlwaysInline);

#if VKI_RAY_TRACING
  processRayQueryLdsStack(lsEntryPoint, hsEntryPoint);
#endif

  uint64_t inRegMask = 0;
  auto entryPointTy = generateLsHsEntryPointType(&inRegMask);

  // Create the entrypoint for the merged shader, and insert it at the start.  This has to be done for unlinked shaders
  // because the vertex fetch shader will be prepended to this module and expect the fall through into the merged
  // shader.
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::LsHsEntryPoint);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  auto module = hsEntryPoint->getParent();
  module->getFunctionList().push_front(entryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
  }

  //
  // The processing is something like this:
  //
  // LS_HS() {
  //   Initialize exec mask to all ones
  //
  //   if (threadIdInWave < lsVertCount)
  //     Run LS
  //
  //   Fence + Barrier
  //
  //   if (threadIdInWave < hsVertCount)
  //     Run HS
  // }
  //

  auto arg = entryPoint->arg_begin();

  Value *offChipLdsBase = (arg + getSpecialSgprInputIndex(m_gfxIp, LsHs::OffChipLdsBase));
  offChipLdsBase->setName("offChipLdsBase");

  Value *mergeWaveInfo = (arg + getSpecialSgprInputIndex(m_gfxIp, LsHs::MergedWaveInfo));
  mergeWaveInfo->setName("mergeWaveInfo");

  Value *tfBufferBase = (arg + getSpecialSgprInputIndex(m_gfxIp, LsHs::TfBufferBase));
  tfBufferBase->setName("tfBufferBase");

  arg += NumSpecialSgprInputs;

  Value *userData = arg++;

  // Define basic blocks
  auto endHsBlock = BasicBlock::Create(*m_context, ".endHs", entryPoint);
  auto beginHsBlock = BasicBlock::Create(*m_context, ".beginHs", entryPoint, endHsBlock);
  auto endLsBlock = BasicBlock::Create(*m_context, ".endLs", entryPoint, beginHsBlock);
  auto beginLsBlock = BasicBlock::Create(*m_context, ".beginLs", entryPoint, endLsBlock);
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", entryPoint, beginLsBlock);

  // Construct ".entry" block
  BuilderBase builder(entryBlock);

  builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, {builder.getInt64(-1)});

  auto threadId = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});

  unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
  if (waveSize == 64) {
    threadId = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), threadId});
  }

  auto lsVertCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergeWaveInfo, builder.getInt32(0), builder.getInt32(8)});

  Value *patchId = arg;
  Value *relPatchId = (arg + 1);
  Value *vertexId = (arg + 2);
  Value *relVertexId = (arg + 3);
  Value *stepRate = (arg + 4);
  Value *instanceId = (arg + 5);
  auto vertexFetchesStart = (arg + 6);
  auto vertexFetchesEnd = entryPoint->arg_end();

  auto hsVertCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergeWaveInfo, builder.getInt32(8), builder.getInt32(8)});

  // NOTE: For GFX9, hardware has an issue of initializing LS VGPRs. When HS is null, v0~v3 are initialized as LS
  // VGPRs rather than expected v2~v4.
  auto gpuWorkarounds = &m_pipelineState->getTargetInfo().getGpuWorkarounds();
  if (gpuWorkarounds->gfx9.fixLsVgprInput) {
    auto nullHs = builder.CreateICmpEQ(hsVertCount, builder.getInt32(0));

    vertexId = builder.CreateSelect(nullHs, arg, (arg + 2));
    relVertexId = builder.CreateSelect(nullHs, (arg + 1), (arg + 3));
    stepRate = builder.CreateSelect(nullHs, (arg + 2), (arg + 4));
    instanceId = builder.CreateSelect(nullHs, (arg + 3), (arg + 5));
  }

  auto lsEnable = builder.CreateICmpULT(threadId, lsVertCount);
  builder.CreateCondBr(lsEnable, beginLsBlock, endLsBlock);

  // Construct ".beginLs" block
  builder.SetInsertPoint(beginLsBlock);

  if (m_hasVs) {
    // Call LS main function
    SmallVector<Value *> args;
    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);

    unsigned lsArgIdx = 0;
    const unsigned lsArgCount = lsEntryPoint->arg_size();

    appendUserData(builder, args, lsEntryPoint, lsArgIdx, userData, intfData->userDataCount);

    // Set up system value VGPRs (LS does not have system value SGPRs)
    if (lsArgIdx < lsArgCount) {
      args.push_back(vertexId);
      ++lsArgIdx;
    }

    if (lsArgIdx < lsArgCount) {
      args.push_back(relVertexId);
      ++lsArgIdx;
    }

    if (lsArgIdx < lsArgCount) {
      args.push_back(stepRate);
      ++lsArgIdx;
    }

    if (lsArgIdx < lsArgCount) {
      args.push_back(instanceId);
      ++lsArgIdx;
    }

    appendArguments(args, vertexFetchesStart, vertexFetchesEnd);
    lsArgIdx += (vertexFetchesEnd - vertexFetchesStart);

    CallInst *call = builder.CreateCall(lsEntryPoint, args);
    call->setCallingConv(CallingConv::AMDGPU_LS);
  }

  builder.CreateBr(endLsBlock);

  // Construct ".endLs" block
  builder.SetInsertPoint(endLsBlock);

  SyncScope::ID workgroupScope = m_context->getOrInsertSyncScopeID("workgroup");
  builder.CreateFence(AtomicOrdering::Release, workgroupScope);
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
  builder.CreateFence(AtomicOrdering::Acquire, workgroupScope);

  auto hsEnable = builder.CreateICmpULT(threadId, hsVertCount);
  builder.CreateCondBr(hsEnable, beginHsBlock, endHsBlock);

  // Construct ".beginHs" block
  builder.SetInsertPoint(beginHsBlock);

  if (m_hasTcs) {
    // Call HS main function
    SmallVector<Value *> args;

    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl);

    unsigned hsArgIdx = 0;

    SmallVector<std::pair<unsigned, unsigned>> substitutions;
    if (intfData->spillTable.sizeInDwords > 0 && m_hasVs) {
      auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      assert(vsIntfData->userDataUsage.spillTable > 0);
      substitutions.emplace_back(intfData->userDataUsage.spillTable, vsIntfData->userDataUsage.spillTable);
    }
    appendUserData(builder, args, hsEntryPoint, hsArgIdx, userData, intfData->userDataCount, substitutions);

    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      args.push_back(offChipLdsBase);
      ++hsArgIdx;
    }

    args.push_back(tfBufferBase);
    ++hsArgIdx;

    // Set up system value VGPRs
    args.push_back(patchId);
    ++hsArgIdx;

    args.push_back(relPatchId);
    ++hsArgIdx;

    assert(hsArgIdx == hsEntryPoint->arg_size()); // Must have visit all arguments of HS entry point

    CallInst *call = builder.CreateCall(hsEntryPoint, args);
    call->setCallingConv(CallingConv::AMDGPU_HS);
  }
  builder.CreateBr(endHsBlock);

  // Construct ".endHs" block
  builder.SetInsertPoint(endHsBlock);
  builder.CreateRetVoid();

  return entryPoint;
}

// =====================================================================================================================
// Generates the type for the new entry-point of ES-GS merged shader.
//
// @param [out] inRegMask : "Inreg" bit mask for the arguments
FunctionType *ShaderMerger::generateEsGsEntryPointType(uint64_t *inRegMask) const {
  assert(m_hasGs);

  std::vector<Type *> argTys;

  // First 8 system values (SGPRs)
  for (unsigned i = 0; i < NumSpecialSgprInputs; ++i) {
    argTys.push_back(Type::getInt32Ty(*m_context));
    *inRegMask |= (1ull << i);
  }

  // User data (SGPRs)
  unsigned userDataCount = 0;
  bool hasTs = (m_hasTcs || m_hasTes);
  if (hasTs) {
    if (m_hasTes) {
      const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
      userDataCount = std::max(intfData->userDataCount, userDataCount);
    }
  } else {
    if (m_hasVs) {
      const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      userDataCount = std::max(intfData->userDataCount, userDataCount);
    }
  }

  const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  userDataCount = std::max(intfData->userDataCount, userDataCount);

  if (hasTs) {
    if (m_hasTes) {
      const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
      if (intfData->spillTable.sizeInDwords > 0 && tesIntfData->spillTable.sizeInDwords == 0) {
        tesIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    }
  } else {
    if (m_hasVs) {
      const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      if (intfData->spillTable.sizeInDwords > 0 && vsIntfData->spillTable.sizeInDwords == 0) {
        vsIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    }
  }

  assert(userDataCount > 0);
  argTys.push_back(FixedVectorType::get(Type::getInt32Ty(*m_context), userDataCount));
  *inRegMask |= (1ull << NumSpecialSgprInputs);

  // Other system values (VGPRs)
  argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offsets (vertex 0 and 1)
  argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offsets (vertex 2 and 3)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive ID (GS)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Invocation ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offsets (vertex 4 and 5)

  if (hasTs) {
    argTys.push_back(Type::getFloatTy(*m_context)); // X of TessCoord (U)
    argTys.push_back(Type::getFloatTy(*m_context)); // Y of TessCoord (V)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID
    argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID
  } else {
    argTys.push_back(Type::getInt32Ty(*m_context)); // Vertex ID
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative vertex ID (auto index)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive ID (VS)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Instance ID
    appendVertexFetchTypes(argTys);
  }

  return FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for ES-GS merged shader.
//
// @param esEntryPoint : Entry-point of hardware export shader (ES) (could be null)
// @param gsEntryPoint : Entry-point of hardware geometry shader (GS)
Function *ShaderMerger::generateEsGsEntryPoint(Function *esEntryPoint, Function *gsEntryPoint) {
  if (esEntryPoint) {
    esEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    esEntryPoint->addFnAttr(Attribute::AlwaysInline);
  }

  assert(gsEntryPoint);
  gsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
  gsEntryPoint->addFnAttr(Attribute::AlwaysInline);

#if VKI_RAY_TRACING
  processRayQueryLdsStack(esEntryPoint, gsEntryPoint);
#endif

  auto module = gsEntryPoint->getParent();
  const bool hasTs = (m_hasTcs || m_hasTes);

  uint64_t inRegMask = 0;
  auto entryPointTy = generateEsGsEntryPointType(&inRegMask);

  // Create the entrypoint for the merged shader, and insert it at the start.  This has to be done for unlinked shaders
  // because the vertex fetch shader will be prepended to this module and expect the fall through into the merged
  // shader.
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::EsGsEntryPoint);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  module->getFunctionList().push_front(entryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
  }

  //
  // The processing is something like this:
  //
  // ES_GS() {
  //   Initialize exec mask to all ones
  //
  //   if (threadIdInWave < esVertCount)
  //     Run ES
  //
  //   Fence + Barrier
  //
  //   if (threadIdInWave < gsPrimCount)
  //     Run GS
  // }
  //

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

  auto arg = entryPoint->arg_begin();

  Value *gsVsOffset = (arg + getSpecialSgprInputIndex(m_gfxIp, EsGs::GsVsOffset, false));
  gsVsOffset->setName("gsVsOffset");

  Value *mergedWaveInfo = (arg + getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo, false));
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *offChipLdsBase = (arg + getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase, false));
  offChipLdsBase->setName("offChipLdsBase");

  arg += NumSpecialSgprInputs;

  Value *userData = arg++;

  // Define basic blocks
  auto endGsBlock = BasicBlock::Create(*m_context, ".endGs", entryPoint);
  auto beginGsBlock = BasicBlock::Create(*m_context, ".beginGs", entryPoint, endGsBlock);
  auto endEsBlock = BasicBlock::Create(*m_context, ".endEs", entryPoint, beginGsBlock);
  auto beginEsBlock = BasicBlock::Create(*m_context, ".beginEs", entryPoint, endEsBlock);
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", entryPoint, beginEsBlock);

  // Construct ".entry" block
  BuilderBase builder(entryBlock);
  builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, {builder.getInt64(-1)});

  auto threadId = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});

  unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  if (waveSize == 64) {
    threadId = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), threadId});
  }

  auto esVertCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergedWaveInfo, builder.getInt32(0), builder.getInt32(8)});
  auto gsPrimCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergedWaveInfo, builder.getInt32(8), builder.getInt32(8)});
  auto gsWaveId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                          {mergedWaveInfo, builder.getInt32(16), builder.getInt32(8)});
  auto waveInSubgroup = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                                {mergedWaveInfo, builder.getInt32(24), builder.getInt32(4)});

  unsigned esGsBytesPerWave = waveSize * 4 * calcFactor.esGsRingItemSize;
  auto esGsOffset = builder.CreateMul(waveInSubgroup, builder.getInt32(esGsBytesPerWave));

  auto esEnable = builder.CreateICmpULT(threadId, esVertCount);
  builder.CreateCondBr(esEnable, beginEsBlock, endEsBlock);

  Value *esGsOffsets01 = arg;

  Value *esGsOffsets23 = PoisonValue::get(Type::getInt32Ty(*m_context));
  if (calcFactor.inputVertices > 2) {
    // NOTE: ES to GS offset (vertex 2 and 3) is valid once the primitive type has more than 2 vertices.
    esGsOffsets23 = (arg + 1);
  }

  Value *gsPrimitiveId = (arg + 2);
  Value *invocationId = (arg + 3);

  Value *esGsOffsets45 = PoisonValue::get(Type::getInt32Ty(*m_context));
  if (calcFactor.inputVertices > 4) {
    // NOTE: ES to GS offset (vertex 4 and 5) is valid once the primitive type has more than 4 vertices.
    esGsOffsets45 = (arg + 4);
  }

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *relVertexId = (arg + 6);
  Value *vsPrimitiveId = (arg + 7);
  Value *instanceId = (arg + 8);
  auto vertexFetchesStart = (arg + 9);
  auto vertexFetchesEnd = entryPoint->arg_end();

  // Construct ".beginEs" block
  unsigned spillTableIdx = 0;
  builder.SetInsertPoint(beginEsBlock);

  if ((hasTs && m_hasTes) || (!hasTs && m_hasVs)) {
    // Call ES main function
    SmallVector<Value *> args;
    auto intfData = m_pipelineState->getShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    spillTableIdx = intfData->userDataUsage.spillTable;

    unsigned esArgIdx = 0;
    const unsigned esArgCount = esEntryPoint->arg_size();

    appendUserData(builder, args, esEntryPoint, esArgIdx, userData, intfData->userDataCount);

    if (hasTs) {
      // Set up system value SGPRs
      if (m_pipelineState->isTessOffChip()) {
        args.push_back(offChipLdsBase);
        ++esArgIdx;

        args.push_back(offChipLdsBase);
        ++esArgIdx;
      }

      args.push_back(esGsOffset);
      ++esArgIdx;

      // Set up system value VGPRs
      args.push_back(tessCoordX);
      ++esArgIdx;

      args.push_back(tessCoordY);
      ++esArgIdx;

      args.push_back(relPatchId);
      ++esArgIdx;

      args.push_back(patchId);
      ++esArgIdx;
    } else {
      // Set up system value SGPRs
      args.push_back(esGsOffset);
      ++esArgIdx;

      // Set up system value VGPRs
      if (esArgIdx < esArgCount) {
        args.push_back(vertexId);
        ++esArgIdx;
      }

      if (esArgIdx < esArgCount) {
        args.push_back(relVertexId);
        ++esArgIdx;
      }

      if (esArgIdx < esArgCount) {
        args.push_back(vsPrimitiveId);
        ++esArgIdx;
      }

      if (esArgIdx < esArgCount) {
        args.push_back(instanceId);
        ++esArgIdx;
      }

      appendArguments(args, vertexFetchesStart, vertexFetchesEnd);
      esArgIdx += (vertexFetchesEnd - vertexFetchesStart);
    }

    CallInst *call = builder.CreateCall(esEntryPoint, args);
    call->setCallingConv(CallingConv::AMDGPU_ES);
  }
  builder.CreateBr(endEsBlock);

  // Construct ".endEs" block
  builder.SetInsertPoint(endEsBlock);

  SyncScope::ID workgroupScope = m_context->getOrInsertSyncScopeID("workgroup");
  builder.CreateFence(AtomicOrdering::Release, workgroupScope);
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
  builder.CreateFence(AtomicOrdering::Acquire, workgroupScope);

  auto gsEnable = builder.CreateICmpULT(threadId, gsPrimCount);
  builder.CreateCondBr(gsEnable, beginGsBlock, endGsBlock);

  // Construct ".beginGs" block
  builder.SetInsertPoint(beginGsBlock);
  {
    auto esGsOffset0 = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {esGsOffsets01, builder.getInt32(0), builder.getInt32(16)});
    auto esGsOffset1 = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {esGsOffsets01, builder.getInt32(16), builder.getInt32(16)});
    auto esGsOffset2 = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {esGsOffsets23, builder.getInt32(0), builder.getInt32(16)});
    auto esGsOffset3 = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {esGsOffsets23, builder.getInt32(16), builder.getInt32(16)});
    auto esGsOffset4 = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {esGsOffsets45, builder.getInt32(0), builder.getInt32(16)});
    auto esGsOffset5 = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {esGsOffsets45, builder.getInt32(16), builder.getInt32(16)});

    // Call GS main function
    SmallVector<llvm::Value *> args;
    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
    unsigned gsArgIdx = 0;

    SmallVector<std::pair<unsigned, unsigned>> substitutions;
    if (intfData->spillTable.sizeInDwords > 0 && spillTableIdx > 0)
      substitutions.emplace_back(intfData->userDataUsage.spillTable, spillTableIdx);
    appendUserData(builder, args, gsEntryPoint, gsArgIdx, userData, intfData->userDataCount, substitutions);

    // Set up system value SGPRs
    args.push_back(gsVsOffset);
    ++gsArgIdx;

    args.push_back(gsWaveId);
    ++gsArgIdx;

    // Set up system value VGPRs
    args.push_back(esGsOffset0);
    ++gsArgIdx;

    args.push_back(esGsOffset1);
    ++gsArgIdx;

    args.push_back(gsPrimitiveId);
    ++gsArgIdx;

    args.push_back(esGsOffset2);
    ++gsArgIdx;

    args.push_back(esGsOffset3);
    ++gsArgIdx;

    args.push_back(esGsOffset4);
    ++gsArgIdx;

    args.push_back(esGsOffset5);
    ++gsArgIdx;

    args.push_back(invocationId);
    ++gsArgIdx;

    assert(gsArgIdx == gsEntryPoint->arg_size()); // Must have visit all arguments of GS entry point

    CallInst *call = builder.CreateCall(gsEntryPoint, args);
    call->setCallingConv(CallingConv::AMDGPU_GS);
  }
  builder.CreateBr(endGsBlock);

  // Construct ".endGs" block
  builder.SetInsertPoint(endGsBlock);
  builder.CreateRetVoid();

  return entryPoint;
}

// =====================================================================================================================
// Append the user data arguments for calling @p target to @p args by referring to the arguments of @p target starting
// at @p argIdx (which will be updated). User data values are taken from the @p userData vector.
//
// @param builder : The builder that will be used to create code to prepare the arguments
// @param [in/out] args : The argument vector that will be appended to
// @param target : The function we are preparing to call
// @param [in/out] argIdx : Index into the target function's arguments
// @param userData : The <N x i32> vector of user data values
// @param userDataCount : The number of element of @p userData that should be processed
// @param substitutions : A mapping of "target function user data index to merged function user data index" that is
//                        applied to i32 arguments of the target function.
void ShaderMerger::appendUserData(BuilderBase &builder, SmallVectorImpl<Value *> &args, Function *target,
                                  unsigned &argIdx, Value *userData, unsigned userDataCount,
                                  ArrayRef<std::pair<unsigned, unsigned>> substitutions) {
  unsigned userDataIdx = 0;

  auto argBegin = target->arg_begin();

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(argIdx < target->arg_size());

    auto arg = (argBegin + argIdx);
    assert(arg->hasAttribute(Attribute::InReg));

    auto argTy = arg->getType();
    if (argTy->isVectorTy()) {
      assert(cast<VectorType>(argTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<FixedVectorType>(argTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto lsUserData = builder.CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(lsUserData);
    } else {
      assert(argTy->isIntegerTy());

      unsigned actualUserDataIdx = userDataIdx;
      for (const auto &substitution : substitutions) {
        if (userDataIdx == substitution.first) {
          actualUserDataIdx = substitution.second;
          break;
        }
      }

      auto lsUserData = builder.CreateExtractElement(userData, actualUserDataIdx);
      args.push_back(lsUserData);
      ++userDataIdx;
    }

    ++argIdx;
  }
}

// =====================================================================================================================
// Appends the type for each of the vertex fetches found in the PAL metadata.
//
// @param [in/out] argTys : The vector to which the type will be appended.
void ShaderMerger::appendVertexFetchTypes(std::vector<Type *> &argTys) const {
  if (m_pipelineState->getPalMetadata()->getVertexFetchCount() != 0) {
    SmallVector<VertexFetchInfo> fetches;
    m_pipelineState->getPalMetadata()->getVertexFetchInfo(fetches);
    m_pipelineState->getPalMetadata()->addVertexFetchInfo(fetches);
    for (const auto &fetchInfo : fetches) {
      argTys.push_back(getVgprTy(fetchInfo.ty));
    }
  }
}

// =====================================================================================================================
// Appends the arguments in the range [begin,end) to the vector.
//
// @param [in/out] args : The vector to which the arguments will be appends.
// @param begin : The start of the argument to add.
// @param end : One past the last argument to add.
void ShaderMerger::appendArguments(SmallVectorImpl<Value *> &args, Argument *begin, Argument *end) const {
  for (auto &fetch : make_range(begin, end)) {
    args.push_back(&fetch);
  }
}

#if VKI_RAY_TRACING
// =====================================================================================================================
// Process ray query LDS stack lowering by incorporating it to the LDS of merged shader. For merged HS, we place the
// LDS stack after the use of tessellation on-chip LDS; for merged GS, we place it after the use of GS on-chip LDS.
//
// @param entryPoint1 : The first entry-point of the shader pair (could be LS or ES)
// @param entryPoint2 : The second entry-point of the shader pair (could be HS or GS)
void ShaderMerger::processRayQueryLdsStack(Function *entryPoint1, Function *entryPoint2) const {
  if (m_gfxIp.major < 10)
    return; // Must be GFX10+

  Module *module = nullptr;
  if (entryPoint1)
    module = entryPoint1->getParent();
  else if (entryPoint2)
    module = entryPoint2->getParent();
  assert(module);

  auto ldsStack = module->getNamedGlobal(RayQueryLdsStackName);
  if (ldsStack) {
    unsigned ldsStackBase = 0;

    ShaderStage shaderStage2 = ShaderStageInvalid;
    if (entryPoint2)
      shaderStage2 = lgc::getShaderStage(entryPoint2);

    if (shaderStage2 == ShaderStageTessControl) {
      // Must be LS-HS merged shader
      const auto &calcFactor =
          m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
      if (calcFactor.rayQueryLdsStackSize > 0)
        ldsStackBase = calcFactor.tessOnChipLdsSize;
    } else {
      // Must be ES-GS merged shader or NGG primitive shader
      const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
      if (calcFactor.rayQueryLdsStackSize > 0)
        ldsStackBase = calcFactor.gsOnChipLdsSize;
    }

    if (ldsStackBase > 0) {
      auto lds = Patch::getLdsVariable(m_pipelineState, module);
      auto newLdsStack = ConstantExpr::getGetElementPtr(
          lds->getValueType(), lds,
          ArrayRef<Constant *>({ConstantInt::get(Type::getInt32Ty(*m_context), 0),
                                ConstantInt::get(Type::getInt32Ty(*m_context), ldsStackBase)}));
      newLdsStack = ConstantExpr::getBitCast(newLdsStack, ldsStack->getType());

      SmallVector<Instruction *, 4> ldsStackInsts;
      for (auto user : ldsStack->users()) {
        auto inst = cast<Instruction>(user);
        assert(inst);
        if (inst->getFunction() == entryPoint1 || inst->getFunction() == entryPoint2)
          ldsStackInsts.push_back(inst);
      }

      for (auto inst : ldsStackInsts) {
        inst->replaceUsesOfWith(ldsStack, newLdsStack);
      }
    }

    if (ldsStack->user_empty())
      ldsStack->eraseFromParent();
  }
}
#endif
