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
 * @file  ShaderMerger.cpp
 * @brief LLPC source file: contains implementation of class lgc::ShaderMerger.
 ***********************************************************************************************************************
 */
#include "ShaderMerger.h"
#include "NggPrimShader.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/lowering/PreparePipelineAbi.h"
#include "lgc/lowering/SystemValues.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
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
  assert(m_pipelineState->isGraphics());

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStage::Vertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStage::TessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStage::TessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStage::Geometry);
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

  static const std::unordered_map<LsHs::SpecialSgprInput, unsigned> LsHsSpecialSgprInputMapGfx11 = {
      {LsHs::HsShaderAddrLow, 0},  // s0
      {LsHs::HsShaderAddrHigh, 1}, // s1
      {LsHs::OffChipLdsBase, 2},   // s2
      {LsHs::MergedWaveInfo, 3},   // s3
      {LsHs::TfBufferBase, 4},     // s4
      {LsHs::waveIdInGroup, 5},    // s5
  };

  if (gfxIp.major >= 11) {
    assert(LsHsSpecialSgprInputMapGfx11.count(sgprInput) > 0);
    return LsHsSpecialSgprInputMapGfx11.at(sgprInput);
  }

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

  static const std::unordered_map<EsGs::SpecialSgprInput, unsigned> EsGsSpecialSgprInputMapGfx11 = {
      {EsGs::GsShaderAddrLow, 0},  // s0
      {EsGs::GsShaderAddrHigh, 1}, // s1
      {EsGs::MergedGroupInfo, 2},  // s2
      {EsGs::MergedWaveInfo, 3},   // s3
      {EsGs::OffChipLdsBase, 4},   // s4
      {EsGs::AttribRingBase, 5},   // s5
      {EsGs::FlatScratchLow, 6},   // s6
      {EsGs::FlatScratchHigh, 7},  // s7
  };

  if (gfxIp.major >= 11) {
    assert(EsGsSpecialSgprInputMapGfx11.count(sgprInput) > 0);
    return EsGsSpecialSgprInputMapGfx11.at(sgprInput);
  }

  if (useNgg) {
    assert(EsGsSpecialSgprInputMapGfx10.count(sgprInput) > 0);
    return EsGsSpecialSgprInputMapGfx10.at(sgprInput);
  }

  assert(EsGsSpecialSgprInputMapGfx9.count(sgprInput) > 0);
  return EsGsSpecialSgprInputMapGfx9.at(sgprInput);
}

// =====================================================================================================================
// Gather tuning attributes from an new entry-point function.
//
// @param tuningAttrs : Attribute builder holding gathered tuning options
// @param srcEntryPoint : Entry-point providing tuning options (can be null)
void ShaderMerger::gatherTuningAttributes(AttrBuilder &tuningAttrs, const Function *srcEntryPoint) const {
  if (!srcEntryPoint)
    return;

  const AttributeSet &fnAttrs = srcEntryPoint->getAttributes().getFnAttrs();
  for (const Attribute &srcAttr : fnAttrs) {
    if (!srcAttr.isStringAttribute())
      continue;

    auto attrKind = srcAttr.getKindAsString();
    if (!(attrKind.starts_with("amdgpu") || attrKind.starts_with("disable")))
      continue;

    // Note: this doesn't mean attribute values match
    if (!tuningAttrs.contains(attrKind)) {
      tuningAttrs.addAttribute(srcAttr);
    } else if (tuningAttrs.getAttribute(attrKind) != srcAttr) {
      LLVM_DEBUG(dbgs() << "[gatherTuningAttributes] Incompatible values for " << attrKind << "\n");
    }
  }
}

// =====================================================================================================================
// Apply tuning attributes to new entry-point function.
//
// @param dstEntryPoint : Entry-point receiving tuning options
// @param tuningAttrs : Attribute builder holding gathered tuning options
void ShaderMerger::applyTuningAttributes(Function *dstEntryPoint, const AttrBuilder &tuningAttrs) const {
  AttrBuilder attrs(*m_context);
  attrs.merge(tuningAttrs);

  // Remove any attributes already defined in the destination
  const AttributeSet &existingAttrs = dstEntryPoint->getAttributes().getFnAttrs();
  for (const Attribute &dstAttr : existingAttrs)
    attrs.removeAttribute(dstAttr);

  // Apply attributes
  dstEntryPoint->addFnAttrs(attrs);
}

// =====================================================================================================================
// Builds LLVM function for hardware primitive shader (NGG).
//
// @param esEntryPoint : Entry-point of hardware export shader (ES) (could be null)
// @param gsEntryPoint : Entry-point of hardware geometry shader (GS) (could be null)
// @param copyShaderEntryPoint : Entry-point of hardware vertex shader (VS, copy shader) (could be null)
Function *ShaderMerger::buildPrimShader(Function *esEntryPoint, Function *gsEntryPoint,
                                        Function *copyShaderEntryPoint) {
  processRayQueryLdsStack(esEntryPoint, gsEntryPoint);

  AttrBuilder tuningAttrs(*m_context);
  gatherTuningAttributes(tuningAttrs, esEntryPoint);
  gatherTuningAttributes(tuningAttrs, gsEntryPoint);
  gatherTuningAttributes(tuningAttrs, copyShaderEntryPoint);

  NggPrimShader primShader(m_pipelineState);
  auto primShaderEntryPoint = primShader.generate(esEntryPoint, gsEntryPoint, copyShaderEntryPoint);
  applyTuningAttributes(primShaderEntryPoint, tuningAttrs);
  return primShaderEntryPoint;
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
    const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);
    userDataCount = std::max(intfData->userDataCount, userDataCount);
  }

  if (m_hasTcs) {
    const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessControl);
    userDataCount = std::max(intfData->userDataCount, userDataCount);
  }

  if (m_hasTcs && m_hasVs) {
    auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);
    auto tcsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessControl);

    if (vsIntfData->spillTable.sizeInDwords == 0 && tcsIntfData->spillTable.sizeInDwords > 0) {
      vsIntfData->userDataUsage.spillTable = userDataCount;
      ++userDataCount;
      assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
    }
  }

  assert(userDataCount > 0);
  argTys.push_back(FixedVectorType::get(Type::getInt32Ty(*m_context), userDataCount));
  *inRegMask |= (1ull << NumSpecialSgprInputs);

  // HS VGPRs
  argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID (control point ID included)

  // LS VGPRs
  argTys.push_back(Type::getInt32Ty(*m_context)); // Vertex ID
  if (m_gfxIp.major <= 11) {
    // NOTE: GFX12 removes those two LS VGPRs.
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative vertex ID (auto index)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Unused
  }
  argTys.push_back(Type::getInt32Ty(*m_context)); // Instance ID

  return FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for LS-HS merged shader.
//
// @param lsEntryPoint : Entry-point of hardware local shader (LS) (could be null)
// @param hsEntryPoint : Entry-point of hardware hull shader (HS)
Function *ShaderMerger::generateLsHsEntryPoint(Function *lsEntryPoint, Function *hsEntryPoint) {
  bool createDbgInfo = false;
  if (lsEntryPoint) {
    lsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    lsEntryPoint->addFnAttr(Attribute::AlwaysInline);
    createDbgInfo |= lsEntryPoint->getSubprogram() != nullptr;
  }

  assert(hsEntryPoint);
  hsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
  hsEntryPoint->addFnAttr(Attribute::AlwaysInline);
  createDbgInfo |= hsEntryPoint->getSubprogram() != nullptr;

  processRayQueryLdsStack(lsEntryPoint, hsEntryPoint);

  uint64_t inRegMask = 0;
  auto entryPointTy = generateLsHsEntryPointType(&inRegMask);

  // Create the entrypoint for the merged shader, and insert it at the start.  This has to be done for unlinked shaders
  // because the vertex fetch shader will be prepended to this module and expect the fall through into the merged
  // shader.
  Function *entryPoint = createFunctionHelper(entryPointTy, GlobalValue::ExternalLinkage, hsEntryPoint->getParent(),
                                              createDbgInfo, lgcName::LsHsEntryPoint);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(entryPoint, ShaderStage::TessControl);

  auto module = hsEntryPoint->getParent();
  module->getFunctionList().push_front(entryPoint);

  AttrBuilder tuningAttrs(*m_context);
  gatherTuningAttributes(tuningAttrs, lsEntryPoint);
  gatherTuningAttributes(tuningAttrs, hsEntryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::TessControl);
  entryPoint->addFnAttr("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  applyTuningAttributes(entryPoint, tuningAttrs);

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
    arg.addAttr(Attribute::NoUndef);
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
  SmallVector<Argument *, 32> args;
  for (auto &arg : entryPoint->args())
    args.push_back(&arg);

  Value *offChipLdsBase = args[getSpecialSgprInputIndex(m_gfxIp, LsHs::OffChipLdsBase)];
  offChipLdsBase->setName("offChipLdsBase");

  Value *mergeWaveInfo = args[getSpecialSgprInputIndex(m_gfxIp, LsHs::MergedWaveInfo)];
  mergeWaveInfo->setName("mergeWaveInfo");

  Value *tfBufferBase = args[getSpecialSgprInputIndex(m_gfxIp, LsHs::TfBufferBase)];
  tfBufferBase->setName("tfBufferBase");

  Value *userData = args[NumSpecialSgprInputs];

  // Define basic blocks
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", entryPoint);
  auto beginLsBlock = BasicBlock::Create(*m_context, ".beginLs", entryPoint);
  auto endLsBlock = BasicBlock::Create(*m_context, ".endLs", entryPoint);
  auto beginHsBlock = BasicBlock::Create(*m_context, ".beginHs", entryPoint);
  auto endHsBlock = BasicBlock::Create(*m_context, ".endHs", entryPoint);

  // Construct ".entry" block
  BuilderBase builder(entryBlock);

  builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, {builder.getInt64(-1)});

  auto threadIdInWave =
      builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), threadIdInWave});
  }
  threadIdInWave->setName("threadIdInWave");

  auto lsVertCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergeWaveInfo, builder.getInt32(0), builder.getInt32(8)});
  lsVertCount->setName("lsVertCount");

  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  // HS VGPRs
  Value *patchId = vgprArgs[0];
  Value *relPatchId = vgprArgs[1];

  // LS VGPRs
  Value *vertexId = vgprArgs[2];
  Value *relVertexId = nullptr;
  Value *stepRate = PoisonValue::get(builder.getInt32Ty()); // Unused
  Value *instanceId = nullptr;

  if (m_gfxIp.major <= 11) {
    relVertexId = vgprArgs[3];
    instanceId = vgprArgs[5];
  } else {
    Value *waveIdInGroup = getFunctionArgument(entryPoint, getSpecialSgprInputIndex(m_gfxIp, LsHs::waveIdInGroup));
    waveIdInGroup = builder.CreateAnd(waveIdInGroup, 0x1F, "waveIdInGroup"); // waveIdInGroup = [4:0]

    relVertexId = builder.CreateMul(builder.getInt32(waveSize), waveIdInGroup);
    relVertexId = builder.CreateAdd(relVertexId, threadIdInWave);
    instanceId = vgprArgs[3];
  }

  // Vertex fetch VGPRs
  ArrayRef<Argument *> vertexFetches = vgprArgs.drop_front(m_gfxIp.major <= 11 ? 6 : 4);

  auto hsVertCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergeWaveInfo, builder.getInt32(8), builder.getInt32(8)});
  hsVertCount->setName("hsVertCount");

  if (m_pipelineState->canOptimizeTessFactor()) {
    // Clear hsPatchCount to zero
    const auto hsPatchCountStart = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)
                                       ->inOutUsage.tcs.hwConfig.onChip.hsPatchCountStart;
    writeValueToLds(builder.getInt32(0), builder.getInt32(hsPatchCountStart), builder);
  }

  auto validLsVert = builder.CreateICmpULT(threadIdInWave, lsVertCount, "validLsVert");
  builder.CreateCondBr(validLsVert, beginLsBlock, endLsBlock);

  // Construct ".beginLs" block
  builder.SetInsertPoint(beginLsBlock);

  if (m_hasVs) {
    // Call LS main function
    SmallVector<Value *> lsArgs;
    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);

    const auto lsArgCount = lsEntryPoint->arg_size();

    appendUserData(builder, lsArgs, lsEntryPoint, 0, userData, intfData->userDataCount);

    // Set up system value VGPRs (LS does not have system value SGPRs)
    if (lsArgs.size() < lsArgCount)
      lsArgs.push_back(vertexId);

    if (lsArgs.size() < lsArgCount)
      lsArgs.push_back(relVertexId);

    if (lsArgs.size() < lsArgCount)
      lsArgs.push_back(stepRate);

    if (lsArgs.size() < lsArgCount)
      lsArgs.push_back(instanceId);

    appendArguments(lsArgs, vertexFetches);

    CallInst *call = callFunctionHelper(lsEntryPoint, lsArgs, builder.GetInsertBlock());
    call->setCallingConv(CallingConv::AMDGPU_LS);
  }

  builder.CreateBr(endLsBlock);

  // Construct ".endLs" block
  builder.SetInsertPoint(endLsBlock);

  SyncScope::ID syncScope = m_context->getOrInsertSyncScopeID("workgroup");
  builder.CreateFence(AtomicOrdering::Release, syncScope);
  createBarrier(builder);
  builder.CreateFence(AtomicOrdering::Acquire, syncScope);

  if (m_pipelineState->canOptimizeTessFactor()) {
    auto accumulateHsPatchCountBlock = BasicBlock::Create(*m_context, ".accumulateHsPatchCount", entryPoint);
    accumulateHsPatchCountBlock->moveAfter(endLsBlock);
    auto endAccumulateHsPatchCountBlock = BasicBlock::Create(*m_context, ".endAccumulateHsPatchCount", entryPoint);
    endAccumulateHsPatchCountBlock->moveAfter(accumulateHsPatchCountBlock);

    Value *hsPatchCount = builder.CreateLShr(mergeWaveInfo, 16); // hsWaveCount = mergedWaveInfo[24:16]
    hsPatchCount = builder.CreateAnd(hsPatchCount, 0xFF);

    // If hsPatchCount is not zero for this wave, accumulate it
    auto hsPatchCountNotZero = builder.CreateICmpNE(hsPatchCount, builder.getInt32(0), "hsPatchCountNotZero");
    auto firstThreadInWave = builder.CreateICmpEQ(threadIdInWave, builder.getInt32(0), "firstThreadInWave");

    builder.CreateCondBr(builder.CreateAnd(hsPatchCountNotZero, firstThreadInWave), accumulateHsPatchCountBlock,
                         endAccumulateHsPatchCountBlock);

    // Construct ".accumulateHsPatchCount" block
    builder.SetInsertPoint(accumulateHsPatchCountBlock);

    const auto hsPatchCountStart = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)
                                       ->inOutUsage.tcs.hwConfig.onChip.hsPatchCountStart;
    atomicAdd(hsPatchCount, builder.getInt32(hsPatchCountStart), builder); // Accumulate hsPatchCount for each wave
    builder.CreateBr(endAccumulateHsPatchCountBlock);

    // Construct ".endAccumulateHsPatchCount" block
    builder.SetInsertPoint(endAccumulateHsPatchCountBlock);
  }

  auto validHsVert = builder.CreateICmpULT(threadIdInWave, hsVertCount, "validHsVert");
  builder.CreateCondBr(validHsVert, beginHsBlock, endHsBlock);

  // Construct ".beginHs" block
  builder.SetInsertPoint(beginHsBlock);

  if (m_hasTcs) {
    // Call HS main function
    SmallVector<Value *> hsArgs;

    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessControl);

    SmallVector<std::pair<unsigned, unsigned>> substitutions;
    if (intfData->spillTable.sizeInDwords > 0 && m_hasVs) {
      auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);
      assert(vsIntfData->userDataUsage.spillTable > 0);
      substitutions.emplace_back(intfData->userDataUsage.spillTable, vsIntfData->userDataUsage.spillTable);
    }
    appendUserData(builder, hsArgs, hsEntryPoint, 0, userData, intfData->userDataCount, substitutions);

    // Set up system value SGPRs
    hsArgs.push_back(offChipLdsBase);
    hsArgs.push_back(tfBufferBase);

    // Set up system value VGPRs
    hsArgs.push_back(patchId);
    hsArgs.push_back(relPatchId);

    CallInst *call = callFunctionHelper(hsEntryPoint, hsArgs, builder.GetInsertBlock());
    call->setCallingConv(CallingConv::AMDGPU_HS);

    // Store TF and HS outputs
    if (m_pipelineState->canOptimizeTessFactor()) {
      auto relativePatchId = builder.CreateAnd(relPatchId, builder.getInt32(0xFF));
      auto vertexIdx = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                               {relPatchId, builder.getInt32(8), builder.getInt32(5)});
      storeTessFactorsAndHsOutputsWithOpt(threadIdInWave, relativePatchId, vertexIdx, builder);
    }
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
      const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessEval);
      userDataCount = std::max(intfData->userDataCount, userDataCount);
    }
  } else {
    if (m_hasVs) {
      const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);
      userDataCount = std::max(intfData->userDataCount, userDataCount);
    }
  }

  const auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry);
  userDataCount = std::max(intfData->userDataCount, userDataCount);

  if (hasTs) {
    if (m_hasTes) {
      const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessEval);
      if (intfData->spillTable.sizeInDwords > 0 && tesIntfData->spillTable.sizeInDwords == 0) {
        tesIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    }
  } else {
    if (m_hasVs) {
      const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);
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

  // GS VGPRs
  argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offsets (vertex 0 and 1)
  argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offsets (vertex 2 and 3)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive ID (GS)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Invocation ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // ES to GS offsets (vertex 4 and 5)

  if (hasTs) {
    // ES VGPRs
    argTys.push_back(Type::getFloatTy(*m_context)); // X of TessCoord (U)
    argTys.push_back(Type::getFloatTy(*m_context)); // Y of TessCoord (V)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID
    argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID
  } else {
    // ES VGPRs
    argTys.push_back(Type::getInt32Ty(*m_context)); // Vertex ID
    argTys.push_back(Type::getInt32Ty(*m_context)); // Relative vertex ID (auto index)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Primitive ID (VS)
    argTys.push_back(Type::getInt32Ty(*m_context)); // Instance ID
  }

  return FunctionType::get(Type::getVoidTy(*m_context), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for ES-GS merged shader.
//
// @param esEntryPoint : Entry-point of hardware export shader (ES) (could be null)
// @param gsEntryPoint : Entry-point of hardware geometry shader (GS)
Function *ShaderMerger::generateEsGsEntryPoint(Function *esEntryPoint, Function *gsEntryPoint) {
  bool createDbgInfo = false;
  if (esEntryPoint) {
    esEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    esEntryPoint->addFnAttr(Attribute::AlwaysInline);
    createDbgInfo = esEntryPoint->getSubprogram() != nullptr;
  }

  assert(gsEntryPoint);
  gsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
  gsEntryPoint->addFnAttr(Attribute::AlwaysInline);
  createDbgInfo |= gsEntryPoint->getSubprogram() != nullptr;

  processRayQueryLdsStack(esEntryPoint, gsEntryPoint);

  auto module = gsEntryPoint->getParent();
  const bool hasTs = (m_hasTcs || m_hasTes);

  uint64_t inRegMask = 0;
  auto entryPointTy = generateEsGsEntryPointType(&inRegMask);

  // Create the entrypoint for the merged shader, and insert it at the start.  This has to be done for unlinked shaders
  // because the vertex fetch shader will be prepended to this module and expect the fall through into the merged
  // shader.
  Function *entryPoint =
      createFunctionHelper(entryPointTy, GlobalValue::ExternalLinkage, module, createDbgInfo, lgcName::EsGsEntryPoint);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  module->getFunctionList().push_front(entryPoint);

  AttrBuilder tuningAttrs(*m_context);
  gatherTuningAttributes(tuningAttrs, esEntryPoint);
  gatherTuningAttributes(tuningAttrs, gsEntryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)
  // NOTE: Legacy (non-NGG) HW path for GS doesn't support wave32 mode.
  assert(m_pipelineState->getShaderWaveSize(ShaderStage::Geometry) == 64);
  entryPoint->addFnAttr("target-features", ",+wavefrontsize64");
  applyTuningAttributes(entryPoint, tuningAttrs);

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
    arg.addAttr(Attribute::NoUndef);
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
  const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;

  SmallVector<Argument *, 32> args;
  for (auto &arg : entryPoint->args())
    args.push_back(&arg);

  Value *gsVsOffset = args[getSpecialSgprInputIndex(m_gfxIp, EsGs::GsVsOffset, false)];
  gsVsOffset->setName("gsVsOffset");

  Value *mergedWaveInfo = args[getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo, false)];
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *offChipLdsBase = args[getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase, false)];
  offChipLdsBase->setName("offChipLdsBase");

  Value *userData = args[NumSpecialSgprInputs];

  // Define basic blocks
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", entryPoint);
  auto beginEsBlock = BasicBlock::Create(*m_context, ".beginEs", entryPoint);
  auto endEsBlock = BasicBlock::Create(*m_context, ".endEs", entryPoint);
  auto beginGsBlock = BasicBlock::Create(*m_context, ".beginGs", entryPoint);
  auto endGsBlock = BasicBlock::Create(*m_context, ".endGs", entryPoint);

  // Construct ".entry" block
  BuilderBase builder(entryBlock);
  builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, {builder.getInt64(-1)});

  auto threadIdInWave =
      builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {builder.getInt32(-1), builder.getInt32(0)});
  threadIdInWave = builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {builder.getInt32(-1), threadIdInWave});
  threadIdInWave->setName("threadIdInWave");

  auto esVertCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergedWaveInfo, builder.getInt32(0), builder.getInt32(8)});
  esVertCount->setName("esVertCount");
  auto gsPrimCount = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                             {mergedWaveInfo, builder.getInt32(8), builder.getInt32(8)});
  gsPrimCount->setName("gsPrimCount");
  auto gsWaveId = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                          {mergedWaveInfo, builder.getInt32(16), builder.getInt32(8)});
  gsWaveId->setName("gsWaveId");
  auto waveInSubgroup = builder.CreateIntrinsic(Intrinsic::amdgcn_ubfe, {builder.getInt32Ty()},
                                                {mergedWaveInfo, builder.getInt32(24), builder.getInt32(4)});
  waveInSubgroup->setName("waveInSubgroup");

  auto esGsOffset = builder.CreateMul(waveInSubgroup, builder.getInt32(64 * hwConfig.esGsRingItemSize));

  auto validEsVert = builder.CreateICmpULT(threadIdInWave, esVertCount, "validEsVert");
  builder.CreateCondBr(validEsVert, beginEsBlock, endEsBlock);

  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  // GS VGPRs
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

  Value *esGsOffsets01 = vgprArgs[0];

  Value *esGsOffsets23 = PoisonValue::get(builder.getInt32Ty());
  if (hwConfig.inputVertices > 2 && geometryMode.inputPrimitive != InputPrimitives::Patch) {
    // NOTE: ES to GS offset (vertex 2 and 3) is valid once the primitive type has more than 2 vertices.
    esGsOffsets23 = vgprArgs[1];
  }

  Value *gsPrimitiveId = vgprArgs[2];
  Value *invocationId = vgprArgs[3];

  Value *esGsOffsets45 = PoisonValue::get(builder.getInt32Ty());
  if (hwConfig.inputVertices > 4 && geometryMode.inputPrimitive != InputPrimitives::Patch) {
    // NOTE: ES to GS offset (vertex 4 and 5) is valid once the primitive type has more than 4 vertices.
    esGsOffsets45 = vgprArgs[4];
  }

  // ES VGPRs
  Value *tessCoordX = vgprArgs[5];
  Value *tessCoordY = vgprArgs[6];
  Value *relPatchId = vgprArgs[7];
  Value *patchId = vgprArgs[8];

  Value *vertexId = vgprArgs[5];
  Value *relVertexId = vgprArgs[6];
  Value *vsPrimitiveId = vgprArgs[7];
  Value *instanceId = vgprArgs[8];

  // Vertex fetch VGPRs
  ArrayRef<Argument *> vertexFetches = vgprArgs.drop_front(9);

  // Construct ".beginEs" block
  unsigned spillTableIdx = 0;
  builder.SetInsertPoint(beginEsBlock);

  if ((hasTs && m_hasTes) || (!hasTs && m_hasVs)) {
    // Call ES main function
    SmallVector<Value *> esArgs;
    auto intfData = m_pipelineState->getShaderInterfaceData(hasTs ? ShaderStage::TessEval : ShaderStage::Vertex);
    spillTableIdx = intfData->userDataUsage.spillTable;

    const unsigned esArgCount = esEntryPoint->arg_size();

    appendUserData(builder, esArgs, esEntryPoint, 0, userData, intfData->userDataCount);

    if (hasTs) {
      // Set up system value SGPRs
      esArgs.push_back(offChipLdsBase);
      esArgs.push_back(esGsOffset);

      // Set up system value VGPRs
      esArgs.push_back(tessCoordX);
      esArgs.push_back(tessCoordY);
      esArgs.push_back(relPatchId);
      esArgs.push_back(patchId);
    } else {
      // Set up system value SGPRs
      esArgs.push_back(esGsOffset);

      // Set up system value VGPRs
      if (esArgs.size() < esArgCount)
        esArgs.push_back(vertexId);

      if (esArgs.size() < esArgCount)
        esArgs.push_back(relVertexId);

      if (esArgs.size() < esArgCount)
        esArgs.push_back(vsPrimitiveId);

      if (esArgs.size() < esArgCount)
        esArgs.push_back(instanceId);

      appendArguments(esArgs, vertexFetches);
    }

    CallInst *call = callFunctionHelper(esEntryPoint, esArgs, builder.GetInsertBlock());
    call->setCallingConv(CallingConv::AMDGPU_ES);
  }
  builder.CreateBr(endEsBlock);

  // Construct ".endEs" block
  builder.SetInsertPoint(endEsBlock);

  SyncScope::ID syncScope = m_context->getOrInsertSyncScopeID("workgroup");
  builder.CreateFence(AtomicOrdering::Release, syncScope);
  createBarrier(builder);
  builder.CreateFence(AtomicOrdering::Acquire, syncScope);

  auto validGsPrim = builder.CreateICmpULT(threadIdInWave, gsPrimCount, "validGsPrim");
  builder.CreateCondBr(validGsPrim, beginGsBlock, endGsBlock);

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
    SmallVector<llvm::Value *> gsArgs;
    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry);

    SmallVector<std::pair<unsigned, unsigned>> substitutions;
    if (intfData->spillTable.sizeInDwords > 0 && spillTableIdx > 0)
      substitutions.emplace_back(intfData->userDataUsage.spillTable, spillTableIdx);
    appendUserData(builder, gsArgs, gsEntryPoint, 0, userData, intfData->userDataCount, substitutions);

    // Set up system value SGPRs
    gsArgs.push_back(gsVsOffset);
    gsArgs.push_back(gsWaveId);

    // Set up system value VGPRs
    gsArgs.push_back(esGsOffset0);
    gsArgs.push_back(esGsOffset1);
    gsArgs.push_back(gsPrimitiveId);
    gsArgs.push_back(esGsOffset2);
    gsArgs.push_back(esGsOffset3);
    gsArgs.push_back(esGsOffset4);
    gsArgs.push_back(esGsOffset5);
    gsArgs.push_back(invocationId);

    CallInst *call = callFunctionHelper(gsEntryPoint, gsArgs, builder.GetInsertBlock());
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
// at @p argIdx. User data values are taken from the @p userData vector.
//
// @param builder : The builder that will be used to create code to prepare the arguments
// @param [in/out] args : The argument vector that will be appended to
// @param target : The function we are preparing to call
// @param argIdx : Index into the target function's arguments
// @param userData : The <N x i32> vector of user data values
// @param userDataCount : The number of element of @p userData that should be processed
// @param substitutions : A mapping of "target function user data index to merged function user data index" that is
//                        applied to i32 arguments of the target function.
void ShaderMerger::appendUserData(BuilderBase &builder, SmallVectorImpl<Value *> &args, Function *target,
                                  unsigned argIdx, Value *userData, unsigned userDataCount,
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

      auto newUserData = builder.CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(newUserData);
    } else {
      assert(argTy->isIntegerTy());

      unsigned actualUserDataIdx = userDataIdx;
      for (const auto &substitution : substitutions) {
        if (userDataIdx == substitution.first) {
          actualUserDataIdx = substitution.second;
          break;
        }
      }

      auto newUserData = builder.CreateExtractElement(userData, actualUserDataIdx);
      args.push_back(newUserData);
      ++userDataIdx;
    }

    ++argIdx;
  }
}

// =====================================================================================================================
// Appends the arguments in the range [begin,end) to the vector.
//
// @param [in/out] args : The vector to which the arguments will be appends.
// @param argsToAppend : The arguments to be appended
void ShaderMerger::appendArguments(SmallVectorImpl<Value *> &args, ArrayRef<Argument *> argsToAppend) const {
  for (auto arg : argsToAppend) {
    args.push_back(arg);
  }
}

// =====================================================================================================================
// Process ray query LDS stack lowering by incorporating it to the LDS of merged shader. For merged HS, we place the
// LDS stack after the use of tessellation on-chip LDS; for merged GS, we place it after the use of GS on-chip LDS.
//
// @param entryPoint1 : The first entry-point of the shader pair (could be LS or ES)
// @param entryPoint2 : The second entry-point of the shader pair (could be HS or GS)
void ShaderMerger::processRayQueryLdsStack(Function *entryPoint1, Function *entryPoint2) const {
  Function *entryPoint = entryPoint2 ? entryPoint2 : entryPoint1;
  assert(entryPoint);

  Module *module = entryPoint->getParent();
  auto ldsStack = module->getNamedGlobal(RayQueryLdsStackName);
  if (ldsStack) {
    std::optional<ShaderStageEnum> shaderStage = lgc::getShaderStage(entryPoint);
    bool hasLdsStack = false;

    if (shaderStage == ShaderStage::TessControl) {
      // Must be LS-HS merged shader
      const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage.tcs.hwConfig;
      hasLdsStack = hwConfig.rayQueryLdsStackSize > 0;
    } else {
      // Must be ES-GS merged shader or NGG primitive shader
      const auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;
      hasLdsStack = hwConfig.rayQueryLdsStackSize > 0;
    }

    if (hasLdsStack) {
      auto lds = LgcLowering::getLdsVariable(m_pipelineState, entryPoint, /*rtStack=*/true);
      auto newLdsStack = ConstantExpr::getBitCast(lds, ldsStack->getType());

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

// =====================================================================================================================
// Handle the store of tessellation factors with optimization (TF0/TF1 messaging) and the store of HS outputs to
// off-chip LDS buffer if the patch is valid (all of its outer TFs are greater than zero).
//
// @param threadIdInWave : Thread ID in wave
// @param relPatchId : Relative patch ID (output patch ID in group)
// @param vertexIdx : Vertex indexing (output control point ID)
// @param builder : IR builder to insert instructions
void ShaderMerger::storeTessFactorsAndHsOutputsWithOpt(Value *threadIdInWave, Value *relPatchId, Value *vertexIdx,
                                                       BuilderBase &builder) {
  assert(m_pipelineState->canOptimizeTessFactor());

  //
  // The processing is something like this:
  //
  // OPTIMIZED_TF_STORE_AND_HS_OUTPUTS_STORE() {
  //   Read hsPatchCount from LDS
  //
  //   if (threadIdInGroup < hsPatchCount) {
  //     Read TFs from LDS (each thread corresponds to a patch)
  //     Compute per-thread specielTf
  //     Compute per-wave specielTf
  //   }
  //
  //   hsPatchWaveCount = alignTo(hsPatchCount, waveSize) / waveSize
  //   if (hsPatchWaveCount > 1) {
  //     Write per-wave specielTf to LDS
  //     Barrier
  //
  //     if (threadIdInWave < hsPatchWaveCount) {
  //       Read per-wave specielTf from LDS
  //       Compute per-group specielTf
  //     }
  //   }
  //
  //   if (threadIdInWave < hsPatchCount) {
  //     if (specialTf)
  //       if (waveIdInGroup == 0)
  //         Send HsTessFactor message
  //     } else {
  //       Write TFs to buffer
  //     }
  //   }
  //
  //   Read TFs from LDS (each thread corresponds to an output vertex)
  //   if (outerTfs > 0.0)
  //     Write HS outputs to off-chip LDS buffer
  // }
  //

  const auto fastMathFlags = builder.getFastMathFlags();
  FastMathFlags newFastMathFlags(fastMathFlags);
  newFastMathFlags.setNoNaNs(); // Set NoNaNs flag to let LLVM optimize floating-point min/max/eq in this algorithm.
  builder.setFastMathFlags(newFastMathFlags);

  auto insertBlock = builder.GetInsertBlock();
  auto entryPoint = insertBlock->getParent();
  assert(entryPoint->getName() == lgcName::LsHsEntryPoint); // Must be LS-HS merged shader

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::TessControl)->inOutUsage;
  const auto &hwConfig = inOutUsage.tcs.hwConfig;
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::TessControl);
  assert(waveSize == 32 || waveSize == 64);

  // Helper to create a basic block
  auto createBlock = [&](const Twine &name) { return BasicBlock::Create(*m_context, name, entryPoint); };

  // Helper to create a PHI node with two incomings
  auto createPhi = [&](std::pair<Value *, BasicBlock *> incoming1, std::pair<Value *, BasicBlock *> incoming2) {
    assert(incoming1.first->getType() == incoming2.first->getType());
    auto phi = builder.CreatePHI(incoming1.first->getType(), 2);
    phi->addIncoming(incoming1.first, incoming1.second);
    phi->addIncoming(incoming2.first, incoming2.second);
    return phi;
  };

  // Helper to do a group ballot
  auto ballot = [&](Value *value) {
    assert(value->getType()->isIntegerTy(1)); // Should be i1

    Value *ballot = builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, builder.getIntNTy(waveSize), value);
    if (waveSize == 32)
      ballot = builder.CreateZExt(ballot, builder.getInt64Ty());

    return ballot;
  };

  // Define basic blocks
  auto checkSpecilTfInWaveBlock = createBlock(".checkSpecialTfInWave");
  checkSpecilTfInWaveBlock->moveAfter(insertBlock);
  auto endCheckSpecialTfInWaveBlock = createBlock(".endCheckSpecialTfInWave");
  endCheckSpecialTfInWaveBlock->moveAfter(checkSpecilTfInWaveBlock);

  auto handleMultiWaveBlock = createBlock(".handleMultiWave");
  handleMultiWaveBlock->moveAfter(endCheckSpecialTfInWaveBlock);
  auto checkSpecilTfInGroupBlock = createBlock(".checkSpecialTfInGroup");
  checkSpecilTfInGroupBlock->moveAfter(handleMultiWaveBlock);
  auto endCheckSpecialTfInGroupBlock = createBlock(".endCheckSpecialTfInGroup");
  endCheckSpecialTfInGroupBlock->moveAfter(checkSpecilTfInGroupBlock);
  auto endHandleMultiWaveBlock = createBlock(".endHandleMultiWave");
  endHandleMultiWaveBlock->moveAfter(endCheckSpecialTfInGroupBlock);

  auto tryStoreTfBlock = createBlock(".tryStoreTf");
  tryStoreTfBlock->moveAfter(endHandleMultiWaveBlock);
  auto checkSendTfMessageBlock = createBlock(".checkSendTfMessage");
  checkSendTfMessageBlock->moveAfter(tryStoreTfBlock);
  auto sendTfMessageBlock = createBlock(".sendTfMessage");
  sendTfMessageBlock->moveAfter(checkSendTfMessageBlock);
  auto storeTfBlock = createBlock(".storeTf");
  storeTfBlock->moveAfter(sendTfMessageBlock);
  auto endTryStoreTfBlock = createBlock(".endTryStoreTf");
  endTryStoreTfBlock->moveAfter(storeTfBlock);

  // Construct current insert block
  Type *bufferDescTy = FixedVectorType::get(builder.getInt32Ty(), 4);
  Value *globalTablePtr = nullptr;
  Value *waveIdInGroup = nullptr;
  Value *threadIdInGroup = nullptr;
  Value *hsPatchCount = nullptr;
  Value *validHsPatch = nullptr;
  {
    auto userData = getFunctionArgument(entryPoint, NumSpecialSgprInputs);
    auto globalTable = builder.CreateExtractElement(
        userData, static_cast<uint64_t>(0)); // The first element of user data argument is always internal global table

#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
    Value *pc = builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {});
#else
    Value *pc = builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
#endif
    pc = builder.CreateBitCast(pc, FixedVectorType::get(builder.getInt32Ty(), 2));

    globalTablePtr = builder.CreateInsertElement(pc, globalTable, static_cast<uint64_t>(0));
    globalTablePtr = builder.CreateBitCast(globalTablePtr, builder.getInt64Ty());
    globalTablePtr =
        builder.CreateIntToPtr(globalTablePtr, PointerType::get(bufferDescTy, ADDR_SPACE_CONST), "globalTablePtr");

    waveIdInGroup = getFunctionArgument(entryPoint, getSpecialSgprInputIndex(m_gfxIp, LsHs::waveIdInGroup));
    waveIdInGroup = builder.CreateAnd(waveIdInGroup, 0x1F, "waveIdInGroup"); // waveIdInGroup = [4:0]

    threadIdInGroup = builder.CreateMul(builder.getInt32(waveSize), waveIdInGroup);
    threadIdInGroup = builder.CreateAdd(threadIdInGroup, threadIdInWave, "threadIdInGroup");

    const auto hsPatchCountStart = hwConfig.onChip.hsPatchCountStart;
    hsPatchCount = readValueFromLds(builder.getInt32Ty(), builder.getInt32(hsPatchCountStart), builder);
    hsPatchCount = builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, hsPatchCount);
    hsPatchCount->setName("hsPatchCount");

    validHsPatch = builder.CreateICmpULT(threadIdInGroup, hsPatchCount, "validHsPatch");
    builder.CreateCondBr(validHsPatch, checkSpecilTfInWaveBlock, endCheckSpecialTfInWaveBlock);
  }

  // Construct ".checkSpecialTfInWave" block
  Value *outerTf = nullptr;
  Value *innerTf = nullptr;
  std::pair<Value *, Value *> specialTfInWave = {}; // Special TF in this wave
  {
    builder.SetInsertPoint(checkSpecilTfInWaveBlock);

    // Read back TFs from LDS
    auto tessFactors = PreparePipelineAbi::readTessFactors(m_pipelineState, threadIdInGroup, builder);
    outerTf = tessFactors.first;
    innerTf = tessFactors.second;

    // Check if the thread has all-ones/all-zeros TFs
    auto minTf = builder.CreateExtractElement(outerTf, static_cast<uint64_t>(0));
    auto maxTf = minTf;
    for (unsigned i = 1; i < cast<FixedVectorType>(outerTf->getType())->getNumElements(); ++i) {
      auto elemTf = builder.CreateExtractElement(outerTf, i);
      minTf = builder.CreateBinaryIntrinsic(Intrinsic::minimum, minTf, elemTf);
      maxTf = builder.CreateBinaryIntrinsic(Intrinsic::maximum, maxTf, elemTf);
    }

    if (innerTf) {
      // Isoline doesn't have inner tessellation factors
      for (unsigned i = 0; i < cast<FixedVectorType>(innerTf->getType())->getNumElements(); ++i) {
        auto elemTf = builder.CreateExtractElement(innerTf, i);
        minTf = builder.CreateBinaryIntrinsic(Intrinsic::minimum, minTf, elemTf);
        maxTf = builder.CreateBinaryIntrinsic(Intrinsic::maximum, maxTf, elemTf);
      }
    }

    auto minTfEqMaxTf = builder.CreateFCmpOEQ(minTf, maxTf);
    Value *isOne = builder.CreateFCmpOEQ(minTf, ConstantFP::get(builder.getFloatTy(), 1.0));
    Value *isZero = builder.CreateFCmpOEQ(minTf, ConstantFP::get(builder.getFloatTy(), 0.0));

    auto isAllOnesTf = builder.CreateAnd(minTfEqMaxTf, isOne);
    auto isAllZerosTf = builder.CreateAnd(minTfEqMaxTf, isZero);

    auto validMask = ballot(builder.getTrue());

    // Check if the wave has all-ones TFs uniformly
    Value *allOnesTfMask = ballot(isAllOnesTf);
    auto isAllOnesTfInWave = builder.CreateICmpEQ(allOnesTfMask, validMask);

    // Check if the wave has all-zeros TFs uniformly
    Value *allZerosTfMask = ballot(isAllZerosTf);
    auto isAllZerosTfInWave = builder.CreateICmpEQ(allZerosTfMask, validMask);

    specialTfInWave = std::make_pair(isAllOnesTfInWave, isAllZerosTfInWave);

    builder.CreateBr(endCheckSpecialTfInWaveBlock);
  }

  // Construct ".endCheckSpecialTfInWave" block
  Value *hsPatchWaveCount = nullptr;
  {
    builder.SetInsertPoint(endCheckSpecialTfInWaveBlock);

    outerTf = createPhi({PoisonValue::get(outerTf->getType()), insertBlock}, {outerTf, checkSpecilTfInWaveBlock});
    outerTf->setName("outerTf");
    if (innerTf) {
      // Isoline doesn't have inner tessellation factors
      innerTf = createPhi({PoisonValue::get(innerTf->getType()), insertBlock}, {innerTf, checkSpecilTfInWaveBlock});
      innerTf->setName("innerTf");
    }

    auto isAllOnesTfInWave =
        createPhi({builder.getTrue(), insertBlock}, {specialTfInWave.first, checkSpecilTfInWaveBlock});
    isAllOnesTfInWave->setName("isAllOnesTfInWave");
    auto isAllZerosTfInWave =
        createPhi({builder.getTrue(), insertBlock}, {specialTfInWave.second, checkSpecilTfInWaveBlock});
    isAllZerosTfInWave->setName("isAllZerosTfInWave");
    specialTfInWave = std::make_pair(isAllOnesTfInWave, isAllZerosTfInWave);

    // hsPatchWaveCount = alignTo(hsPatchCount, waveSize) / waveSize = (hsPatchCount + waveSize - 1) / waveSize
    hsPatchWaveCount = builder.CreateAdd(hsPatchCount, builder.getInt32(waveSize - 1));
    hsPatchWaveCount = builder.CreateLShr(hsPatchWaveCount, Log2_32(waveSize), "hsPatchWaveCount");

    auto multiWave = builder.CreateICmpUGT(hsPatchWaveCount, builder.getInt32(1), "multiWave");
    builder.CreateCondBr(multiWave, handleMultiWaveBlock, endHandleMultiWaveBlock);
  }

  // Construct ".handleMultiWave" block
  {
    builder.SetInsertPoint(handleMultiWaveBlock);

    const unsigned specialTfValueStart = hwConfig.onChip.specialTfValueStart;

    // ldsOffset = specialTfValueStart + 2 * waveIdInGroup
    auto ldsOffset = builder.CreateAdd(builder.getInt32(specialTfValueStart), builder.CreateShl(waveIdInGroup, 1));
    writeValueToLds(builder.CreateZExt(specialTfInWave.first, builder.getInt32Ty()), ldsOffset,
                    builder); // Write isAllOnesTfInWave to LDS

    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(1));
    writeValueToLds(builder.CreateZExt(specialTfInWave.second, builder.getInt32Ty()), ldsOffset,
                    builder); // Write isAllZerosTfInWave to LDS

    SyncScope::ID syncScope = m_context->getOrInsertSyncScopeID("workgroup");
    builder.CreateFence(AtomicOrdering::Release, syncScope);
    createBarrier(builder);
    builder.CreateFence(AtomicOrdering::Acquire, syncScope);

    auto validHsPatchWave = builder.CreateICmpULT(threadIdInWave, hsPatchWaveCount, "validHsPatchWave");
    builder.CreateCondBr(validHsPatchWave, checkSpecilTfInGroupBlock, endCheckSpecialTfInGroupBlock);
  }

  // Construct ".checkSpecialTfInGroup" block
  std::pair<Value *, Value *> specialTfInGroup = {}; // Special TF in this group
  {
    builder.SetInsertPoint(checkSpecilTfInGroupBlock);

    const unsigned specialTfValueStart = hwConfig.onChip.specialTfValueStart;

    // ldsOffset = specialTfValueStart + 2 * threadIdInWave
    auto ldsOffset = builder.CreateAdd(builder.getInt32(specialTfValueStart), builder.CreateShl(threadIdInWave, 1));
    Value *isAllOnesTf = readValueFromLds(builder.getInt32Ty(), ldsOffset, builder);
    isAllOnesTf = builder.CreateTrunc(isAllOnesTf, builder.getInt1Ty());

    ldsOffset = builder.CreateAdd(ldsOffset, builder.getInt32(1));
    Value *isAllZerosTf = readValueFromLds(builder.getInt32Ty(), ldsOffset, builder);
    isAllZerosTf = builder.CreateTrunc(isAllZerosTf, builder.getInt1Ty());

    auto validMask = ballot(builder.getTrue());

    // Check if the group has all-ones TFs uniformly
    Value *allOnesTfMask = ballot(isAllOnesTf);
    Value *isAllOnesTfInGroup = builder.CreateICmpEQ(allOnesTfMask, validMask);

    // Check if the group has all-zeros TFs uniformly
    Value *allZerosTfMask = ballot(isAllZerosTf);
    Value *isAllZerosTfInGroup = builder.CreateICmpEQ(allZerosTfMask, validMask);

    specialTfInGroup = std::make_pair(isAllOnesTfInGroup, isAllZerosTfInGroup);

    builder.CreateBr(endCheckSpecialTfInGroupBlock);
  }

  // Construct ".endCheckSpecialTfInGroup" block
  {
    builder.SetInsertPoint(endCheckSpecialTfInGroupBlock);

    auto isAllOnesTfInGroup =
        createPhi({builder.getTrue(), handleMultiWaveBlock}, {specialTfInGroup.first, checkSpecilTfInGroupBlock});
    isAllOnesTfInGroup->setName("isAllOnesTfInGroup");
    auto isAllZerosTfInGroup =
        createPhi({builder.getTrue(), handleMultiWaveBlock}, {specialTfInGroup.second, checkSpecilTfInGroupBlock});
    isAllZerosTfInGroup->setName("isAllZerosTfInGroup");
    specialTfInGroup = std::make_pair(isAllOnesTfInGroup, isAllZerosTfInGroup);

    builder.CreateBr(endHandleMultiWaveBlock);
  }

  // Construct ".endHandleMultiWave" block
  std::pair<Value *, Value *> specialTf = {}; // Finalized special TF
  {
    builder.SetInsertPoint(endHandleMultiWaveBlock);

    auto isAllOnesTf = createPhi({specialTfInWave.first, endCheckSpecialTfInWaveBlock},
                                 {specialTfInGroup.first, endCheckSpecialTfInGroupBlock});
    isAllOnesTf->setName("isAllOnesTf");
    auto isAllZerosTf = createPhi({specialTfInWave.second, endCheckSpecialTfInWaveBlock},
                                  {specialTfInGroup.second, endCheckSpecialTfInGroupBlock});
    isAllZerosTf->setName("isAllZerosTf");
    specialTf = std::make_pair(isAllOnesTf, isAllZerosTf);

    builder.CreateCondBr(validHsPatch, tryStoreTfBlock, endTryStoreTfBlock);
  }

  // Construct ".tryStoreTf" block
  {
    builder.SetInsertPoint(tryStoreTfBlock);

    auto isSpecialTf = builder.CreateOr(specialTf.first, specialTf.second, "isSpecialTf");
    builder.CreateCondBr(isSpecialTf, checkSendTfMessageBlock, storeTfBlock);
  }

  // Construct ".checkSendTfMessage" block
  {
    builder.SetInsertPoint(checkSendTfMessageBlock);

    auto firstWaveInGroup = builder.CreateICmpEQ(waveIdInGroup, builder.getInt32(0), "firstWaveInGroup");
    builder.CreateCondBr(firstWaveInGroup, sendTfMessageBlock, endTryStoreTfBlock);
  }

  // Construct ".sendTfMessage" block
  {
    builder.SetInsertPoint(sendTfMessageBlock);

    // M0[0] = 1 (allOnesTf), 0 (allZerosTf)
    auto m0 = builder.CreateZExt(specialTf.first, builder.getInt32Ty());
    builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {builder.getInt32(HsTessFactor), m0});
    builder.CreateBr(endTryStoreTfBlock);
  }

  // Construct ".storeTf" block
  {
    builder.SetInsertPoint(storeTfBlock);

    Value *tfBufferDescPtr =
        builder.CreateConstGEP1_32(builder.getInt8Ty(), globalTablePtr, SiDrvTableTfBufferOffs * 4, "tfBufferDescPtr");
    auto tfBufferDesc = builder.CreateLoad(bufferDescTy, tfBufferDescPtr, "tfBufferDesc");
    auto tfBufferBase = getFunctionArgument(entryPoint, getSpecialSgprInputIndex(m_gfxIp, LsHs::TfBufferBase));

    // Store TFs to TF buffer
    PreparePipelineAbi::writeTessFactors(m_pipelineState, tfBufferDesc, tfBufferBase, threadIdInGroup, outerTf, innerTf,
                                         builder);
    builder.CreateBr(endTryStoreTfBlock);
  }

  // Construct ".endTryStoreTf" block
  {
    builder.SetInsertPoint(endTryStoreTfBlock);

    Value *offChipLdsDescPtr = builder.CreateConstGEP1_32(builder.getInt8Ty(), globalTablePtr,
                                                          SiDrvTableHsBufferOffs * 4, "offChipLdsDescPtr");
    auto offChipLdsDesc = builder.CreateLoad(bufferDescTy, offChipLdsDescPtr, "offChipLdsDesc");
    auto offChipLdsBase = getFunctionArgument(entryPoint, getSpecialSgprInputIndex(m_gfxIp, LsHs::OffChipLdsBase));

    // Store HS outputs to off-chip LDS buffer
    const auto &[outerTf, innerTf] = PreparePipelineAbi::readTessFactors(m_pipelineState, relPatchId, builder);
    PreparePipelineAbi::writeHsOutputs(m_pipelineState, offChipLdsDesc, offChipLdsBase, relPatchId, vertexIdx, outerTf,
                                       builder);
  }

  builder.setFastMathFlags(fastMathFlags); // Restore fast math flags
}

// =====================================================================================================================
// Read value from LDS.
//
// @param readTy : Type of value to read
// @param ldsOffset : LDS offset in dwords
// @param builder : IR builder to insert instructions
// @returns : The Value read from LDS
Value *ShaderMerger::readValueFromLds(Type *readTy, Value *ldsOffset, IRBuilder<> &builder) {
  assert(readTy->getScalarSizeInBits() == 32); // Only accept 32-bit data

  auto lds = LgcLowering::getLdsVariable(m_pipelineState, builder.GetInsertBlock()->getParent());
  Value *readPtr = builder.CreateGEP(builder.getInt32Ty(), lds, ldsOffset);
  readPtr = builder.CreateBitCast(readPtr, PointerType::get(readTy, readPtr->getType()->getPointerAddressSpace()));
  return builder.CreateAlignedLoad(readTy, readPtr, Align(4));
}

// =====================================================================================================================
// Write value to LDS.
//
// @param writeValue : Value to write
// @param ldsOffset : LDS offset in dwords
// @param builder : IR builder to insert instructions
void ShaderMerger::writeValueToLds(Value *writeValue, Value *ldsOffset, IRBuilder<> &builder) {
  auto writeTy = writeValue->getType();
  assert(writeTy->getScalarSizeInBits() == 32); // Only accept 32-bit data

  auto lds = LgcLowering::getLdsVariable(m_pipelineState, builder.GetInsertBlock()->getParent());
  Value *writePtr = builder.CreateGEP(builder.getInt32Ty(), lds, ldsOffset);
  writePtr = builder.CreateBitCast(writePtr, PointerType::get(writeTy, writePtr->getType()->getPointerAddressSpace()));
  builder.CreateAlignedStore(writeValue, writePtr, Align(4));
}

// =====================================================================================================================
// Do atomic add with the value stored in LDS.
//
// @param value : Value to do atomic add
// @param ldsOffset : LDS offset to do atomic add
// @param builder : IR builder to insert instructions
void ShaderMerger::atomicAdd(Value *value, Value *ldsOffset, IRBuilder<> &builder) {
  assert(value->getType()->isIntegerTy(32));

  auto lds = LgcLowering::getLdsVariable(m_pipelineState, builder.GetInsertBlock()->getParent());
  Value *atomicPtr = builder.CreateGEP(builder.getInt32Ty(), lds, ldsOffset);

  builder.CreateAtomicRMW(AtomicRMWInst::BinOp::Add, atomicPtr, value, MaybeAlign(),
                          AtomicOrdering::SequentiallyConsistent,
                          builder.getContext().getOrInsertSyncScopeID("workgroup"));
}

// =====================================================================================================================
// Create LDS barrier to guarantee the synchronization of LDS operations.
//
// @param builder : IR builder to insert instructions
void ShaderMerger::createBarrier(IRBuilder<> &builder) {
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 12) {
    builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_signal, {}, builder.getInt32(WorkgroupNormalBarrierId));
    builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_wait, {},
                            builder.getInt16(static_cast<uint16_t>(WorkgroupNormalBarrierId)));
    return;
  }

#if !LLVM_MAIN_REVISION || LLVM_MAIN_REVISION >= 532478
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {});
#else
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
#endif
}
