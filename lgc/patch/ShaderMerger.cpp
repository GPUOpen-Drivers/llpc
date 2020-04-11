/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "Patch.h"
#include "PipelineShaders.h"
#include "PipelineState.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "llpc-shader-merger"

using namespace llvm;
using namespace lgc;

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
// @param pipelineShaders : API shaders in the pipeline
ShaderMerger::ShaderMerger(PipelineState *pipelineState, PipelineShaders *pipelineShaders)
    : m_pipelineState(pipelineState), m_context(&pipelineState->getContext()),
      m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()), m_primShader(pipelineState) {
  assert(m_gfxIp.major >= 9);
  assert(m_pipelineState->isGraphics());

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
}

// =====================================================================================================================
// Builds LLVM function for hardware primitive shader (NGG).
//
// @param esEntryPoint : Entry-point of hardware export shader (ES) (could be null)
// @param gsEntryPoint : Entry-point of hardware geometry shader (GS) (could be null)
// @param copyShaderEntryPoint : Entry-point of hardware vertex shader (VS, copy shader) (could be null)
Function *ShaderMerger::buildPrimShader(Function *esEntryPoint, Function *gsEntryPoint,
                                        Function *copyShaderEntryPoint) {
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
  for (unsigned i = 0; i < LsHsSpecialSysValueCount; ++i) {
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
  argTys.push_back(VectorType::get(Type::getInt32Ty(*m_context), userDataCount));
  *inRegMask |= (1ull << LsHsSpecialSysValueCount);

  // Other system values (VGPRs)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Patch ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // Relative patch ID (control point ID included)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Vertex ID
  argTys.push_back(Type::getInt32Ty(*m_context)); // Relative vertex ID (auto index)
  argTys.push_back(Type::getInt32Ty(*m_context)); // Step rate
  argTys.push_back(Type::getInt32Ty(*m_context)); // Instance ID

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

  uint64_t inRegMask = 0;
  auto entryPointTy = generateLsHsEntryPointType(&inRegMask);

  // Create the entrypoint for the merged shader, and insert it just before the old HS.
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::LsHsEntryPoint);
  auto module = hsEntryPoint->getParent();
  module->getFunctionList().insert(hsEntryPoint->getIterator(), entryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
  }

  // define dllexport amdgpu_hs @_amdgpu_hs_main(
  //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..5)
  // {
  // .entry
  //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
  //     call void @llvm.amdgcn.init.exec(i64 -1)
  //
  //     ; Get thread ID:
  //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
  //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
  //     ;   threadId = bitCount
  //     %threadId = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
  //     %threadId = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadId)
  //
  //     %lsVertCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
  //     %hsVertCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
  //
  //     %nullHs = icmp eq i32 %hsVertCount, 0
  //     %vgpr0 = select i1 %nullHs, i32 %vgpr0, i32 %vgpr2
  //     %vgpr1 = select i1 %nullHs, i32 %vgpr1, i32 %vgpr3
  //     %vgpr2 = select i1 %nullHs, i32 %vgpr2, i32 %vgpr4
  //     %vgpr3 = select i1 %nullHs, i32 %vgpr3, i32 %vgpr5
  //
  //     %lsEnable = icmp ult i32 %threadId, %lsVertCount
  //     br i1 %lsEnable, label %.beginls, label %.endls
  //
  // .beginls:
  //     call void @llpc.ls.main(%sgpr..., %userData..., %vgpr...)
  //     br label %.endls
  //
  // .endls:
  //     call void @llvm.amdgcn.s.barrier()
  //     %hsEnable = icmp ult i32 %threadId, %hsVertCount
  //     br i1 %hsEnable, label %.beginhs, label %.endhs
  //
  // .beginhs:
  //     call void @llpc.hs.main(%sgpr..., %userData..., %vgpr...)
  //     br label %.endhs
  //
  // .endhs:
  //     ret void
  // }

  std::vector<Value *> args;
  std::vector<Attribute::AttrKind> attribs;

  auto arg = entryPoint->arg_begin();

  Value *offChipLdsBase = (arg + LsHsSysValueOffChipLdsBase);
  Value *mergeWaveInfo = (arg + LsHsSysValueMergedWaveInfo);
  Value *tfBufferBase = (arg + LsHsSysValueTfBufferBase);

  arg += LsHsSpecialSysValueCount;

  Value *userData = arg++;

  // Define basic blocks
  auto endHsBlock = BasicBlock::Create(*m_context, ".endhs", entryPoint);
  auto beginHsBlock = BasicBlock::Create(*m_context, ".beginhs", entryPoint, endHsBlock);
  auto endLsBlock = BasicBlock::Create(*m_context, ".endls", entryPoint, beginHsBlock);
  auto beginLsBlock = BasicBlock::Create(*m_context, ".beginls", entryPoint, endLsBlock);
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", entryPoint, beginLsBlock);

  // Construct ".entry" block
  args.clear();
  args.push_back(ConstantInt::get(Type::getInt64Ty(*m_context), -1));

  attribs.clear();
  attribs.push_back(Attribute::NoRecurse);

  emitCall("llvm.amdgcn.init.exec", Type::getVoidTy(*m_context), args, attribs, entryBlock);

  args.clear();
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), -1));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));

  attribs.clear();
  attribs.push_back(Attribute::NoRecurse);

  auto threadId = emitCall("llvm.amdgcn.mbcnt.lo", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
  if (waveSize == 64) {
    args.clear();
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), -1));
    args.push_back(threadId);

    threadId = emitCall("llvm.amdgcn.mbcnt.hi", Type::getInt32Ty(*m_context), args, attribs, entryBlock);
  }

  args.clear();
  args.push_back(mergeWaveInfo);
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));

  attribs.clear();
  attribs.push_back(Attribute::ReadNone);

  auto lsVertCount = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  Value *patchId = arg;
  Value *relPatchId = (arg + 1);
  Value *vertexId = (arg + 2);
  Value *relVertexId = (arg + 3);
  Value *stepRate = (arg + 4);
  Value *instanceId = (arg + 5);

  args.clear();
  args.push_back(mergeWaveInfo);
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));

  auto hsVertCount = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, entryBlock);
  // NOTE: For GFX9, hardware has an issue of initializing LS VGPRs. When HS is null, v0~v3 are initialized as LS
  // VGPRs rather than expected v2~v4.
  auto gpuWorkarounds = &m_pipelineState->getTargetInfo().getGpuWorkarounds();
  if (gpuWorkarounds->gfx9.fixLsVgprInput) {
    auto nullHs = new ICmpInst(*entryBlock, ICmpInst::ICMP_EQ, hsVertCount,
                               ConstantInt::get(Type::getInt32Ty(*m_context), 0), "");

    vertexId = SelectInst::Create(nullHs, arg, (arg + 2), "", entryBlock);
    relVertexId = SelectInst::Create(nullHs, (arg + 1), (arg + 3), "", entryBlock);
    stepRate = SelectInst::Create(nullHs, (arg + 2), (arg + 4), "", entryBlock);
    instanceId = SelectInst::Create(nullHs, (arg + 3), (arg + 5), "", entryBlock);
  }

  auto lsEnable = new ICmpInst(*entryBlock, ICmpInst::ICMP_ULT, threadId, lsVertCount, "");
  BranchInst::Create(beginLsBlock, endLsBlock, lsEnable, entryBlock);

  // Construct ".beginls" block
  if (m_hasVs) {
    // Call LS main function
    args.clear();

    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
    const unsigned userDataCount = intfData->userDataCount;

    unsigned userDataIdx = 0;

    auto lsArgBegin = lsEntryPoint->arg_begin();
    const unsigned lsArgCount = lsEntryPoint->arg_size();

    unsigned lsArgIdx = 0;

    // Set up user data SGPRs
    while (userDataIdx < userDataCount) {
      assert(lsArgIdx < lsArgCount);

      auto lsArg = (lsArgBegin + lsArgIdx);
      assert(lsArg->hasAttribute(Attribute::InReg));

      auto lsArgTy = lsArg->getType();
      if (lsArgTy->isVectorTy()) {
        assert(lsArgTy->getVectorElementType()->isIntegerTy());

        const unsigned userDataSize = lsArgTy->getVectorNumElements();

        std::vector<Constant *> shuffleMask;
        for (unsigned i = 0; i < userDataSize; ++i)
          shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), userDataIdx + i));

        userDataIdx += userDataSize;

        auto lsUserData = new ShuffleVectorInst(userData, userData, ConstantVector::get(shuffleMask), "", beginLsBlock);
        args.push_back(lsUserData);
      } else {
        assert(lsArgTy->isIntegerTy());

        auto lsUserData = ExtractElementInst::Create(
            userData, ConstantInt::get(Type::getInt32Ty(*m_context), userDataIdx), "", beginLsBlock);
        args.push_back(lsUserData);
        ++userDataIdx;
      }

      ++lsArgIdx;
    }

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

    assert(lsArgIdx == lsArgCount); // Must have visit all arguments of LS entry point

    CallInst::Create(lsEntryPoint, args, "", beginLsBlock);
  }
  BranchInst::Create(endLsBlock, beginLsBlock);

  // Construct ".endls" block
  args.clear();
  attribs.clear();
  attribs.push_back(Attribute::NoRecurse);
  emitCall("llvm.amdgcn.s.barrier", Type::getVoidTy(*m_context), args, attribs, endLsBlock);

  auto hsEnable = new ICmpInst(*endLsBlock, ICmpInst::ICMP_ULT, threadId, hsVertCount, "");
  BranchInst::Create(beginHsBlock, endHsBlock, hsEnable, endLsBlock);

  // Construct ".beginhs" block
  if (m_hasTcs) {
    // Call HS main function
    args.clear();

    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl);
    const unsigned userDataCount = intfData->userDataCount;

    unsigned userDataIdx = 0;

    auto hsArgBegin = hsEntryPoint->arg_begin();

    unsigned hsArgIdx = 0;

    // Set up user data SGPRs
    while (userDataIdx < userDataCount) {
      assert(hsArgIdx < hsEntryPoint->arg_size());

      auto hsArg = (hsArgBegin + hsArgIdx);
      assert(hsArg->hasAttribute(Attribute::InReg));

      auto hsArgTy = hsArg->getType();
      if (hsArgTy->isVectorTy()) {
        assert(hsArgTy->getVectorElementType()->isIntegerTy());

        const unsigned userDataSize = hsArgTy->getVectorNumElements();

        std::vector<Constant *> shuffleMask;
        for (unsigned i = 0; i < userDataSize; ++i)
          shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), userDataIdx + i));

        userDataIdx += userDataSize;

        auto hsUserData = new ShuffleVectorInst(userData, userData, ConstantVector::get(shuffleMask), "", beginHsBlock);
        args.push_back(hsUserData);
      } else {
        assert(hsArgTy->isIntegerTy());
        unsigned actualUserDataIdx = userDataIdx;
        if (intfData->spillTable.sizeInDwords > 0) {
          if (intfData->userDataUsage.spillTable == userDataIdx) {
            if (m_hasVs) {
              auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
              assert(vsIntfData->userDataUsage.spillTable > 0);
              actualUserDataIdx = vsIntfData->userDataUsage.spillTable;
            }
          }
        }
        auto hsUserData = ExtractElementInst::Create(
            userData, ConstantInt::get(Type::getInt32Ty(*m_context), actualUserDataIdx), "", beginHsBlock);
        args.push_back(hsUserData);
        ++userDataIdx;
      }

      ++hsArgIdx;
    }

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

    CallInst::Create(hsEntryPoint, args, "", beginHsBlock);
  }
  BranchInst::Create(endHsBlock, beginHsBlock);

  // Construct ".endhs" block
  ReturnInst::Create(*m_context, endHsBlock);

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
  for (unsigned i = 0; i < EsGsSpecialSysValueCount; ++i) {
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
      assert(tesIntfData->userDataUsage.tes.viewIndex == intfData->userDataUsage.gs.viewIndex);
      if (intfData->spillTable.sizeInDwords > 0 && tesIntfData->spillTable.sizeInDwords == 0) {
        tesIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    }
  } else {
    if (m_hasVs) {
      const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
      assert(vsIntfData->userDataUsage.vs.viewIndex == intfData->userDataUsage.gs.viewIndex);
      if (intfData->spillTable.sizeInDwords > 0 && vsIntfData->spillTable.sizeInDwords == 0) {
        vsIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    }
  }

  assert(userDataCount > 0);
  argTys.push_back(VectorType::get(Type::getInt32Ty(*m_context), userDataCount));
  *inRegMask |= (1ull << EsGsSpecialSysValueCount);

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

  auto module = gsEntryPoint->getParent();
  const bool hasTs = (m_hasTcs || m_hasTes);

  uint64_t inRegMask = 0;
  auto entryPointTy = generateEsGsEntryPointType(&inRegMask);

  // Create the entrypoint for the merged shader, and insert it just before the old GS.
  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::EsGsEntryPoint);
  module->getFunctionList().insert(gsEntryPoint->getIterator(), entryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
  }

  // define dllexport amdgpu_gs @_amdgpu_gs_main(
  //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8)
  // {
  // .entry
  //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
  //     call void @llvm.amdgcn.init.exec(i64 -1)
  //
  //     ; Get thread ID:
  //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
  //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
  //     ;   threadId = bitCount
  //     %threadId = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
  //     %threadId = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadId)
  //
  //     %esVertCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
  //     %gsPrimCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
  //
  //     %esEnable = icmp ult i32 %threadId, %esVertCount
  //     br i1 %esEnable, label %.begines, label %.endes
  //
  // .begines:
  //     call void @llpc.es.main(%sgpr..., %userData..., %vgpr...)
  //     br label %.endes
  //
  // .endes:
  //     call void @llvm.amdgcn.s.barrier()
  //     %gsEnable = icmp ult i32 %threadId, %gsPrimCount
  //     br i1 %gsEnable, label %.begings, label %.endgs
  //
  // .begings:
  //     call void @llpc.gs.main(%sgpr..., %userData..., %vgpr...)
  //     br label %.endgs
  //
  // .endgs:
  //     ret void
  // }

  std::vector<Value *> args;
  std::vector<Attribute::AttrKind> attribs;

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

  auto arg = entryPoint->arg_begin();

  Value *gsVsOffset = (arg + EsGsSysValueGsVsOffset);
  Value *mergedWaveInfo = (arg + EsGsSysValueMergedWaveInfo);
  Value *offChipLdsBase = (arg + EsGsSysValueOffChipLdsBase);

  arg += EsGsSpecialSysValueCount;

  Value *userData = arg++;

  // Define basic blocks
  auto endGsBlock = BasicBlock::Create(*m_context, ".endgs", entryPoint);
  auto beginGsBlock = BasicBlock::Create(*m_context, ".begings", entryPoint, endGsBlock);
  auto endEsBlock = BasicBlock::Create(*m_context, ".endes", entryPoint, beginGsBlock);
  auto beginEsBlock = BasicBlock::Create(*m_context, ".begines", entryPoint, endEsBlock);
  auto entryBlock = BasicBlock::Create(*m_context, ".entry", entryPoint, beginEsBlock);

  // Construct ".entry" block
  args.clear();
  args.push_back(ConstantInt::get(Type::getInt64Ty(*m_context), -1));

  attribs.clear();
  attribs.push_back(Attribute::NoRecurse);

  emitCall("llvm.amdgcn.init.exec", Type::getVoidTy(*m_context), args, attribs, entryBlock);

  args.clear();
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), -1));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));

  attribs.clear();
  attribs.push_back(Attribute::NoRecurse);

  auto threadId = emitCall("llvm.amdgcn.mbcnt.lo", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  if (waveSize == 64) {
    args.clear();
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), -1));
    args.push_back(threadId);

    threadId = emitCall("llvm.amdgcn.mbcnt.hi", Type::getInt32Ty(*m_context), args, attribs, entryBlock);
  }

  args.clear();
  args.push_back(mergedWaveInfo);
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));

  attribs.clear();
  attribs.push_back(Attribute::ReadNone);

  auto esVertCount = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  args.clear();
  args.push_back(mergedWaveInfo);
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));

  auto gsPrimCount = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  args.clear();
  args.push_back(mergedWaveInfo);
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 8));

  auto gsWaveId = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  args.clear();
  args.push_back(mergedWaveInfo);
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 24));
  args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 4));

  auto waveInSubgroup = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, entryBlock);

  auto esGsOffset = BinaryOperator::CreateMul(
      waveInSubgroup, ConstantInt::get(Type::getInt32Ty(*m_context), 64 * 4 * calcFactor.esGsRingItemSize), "",
      entryBlock);

  auto esEnable = new ICmpInst(*entryBlock, ICmpInst::ICMP_ULT, threadId, esVertCount, "");
  BranchInst::Create(beginEsBlock, endEsBlock, esEnable, entryBlock);

  Value *esGsOffsets01 = arg;

  Value *esGsOffsets23 = UndefValue::get(Type::getInt32Ty(*m_context));
  if (calcFactor.inputVertices > 2) {
    // NOTE: ES to GS offset (vertex 2 and 3) is valid once the primitive type has more than 2 vertices.
    esGsOffsets23 = (arg + 1);
  }

  Value *gsPrimitiveId = (arg + 2);
  Value *invocationId = (arg + 3);

  Value *esGsOffsets45 = UndefValue::get(Type::getInt32Ty(*m_context));
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

  // Construct ".begines" block
  unsigned spillTableIdx = 0;
  if ((hasTs && m_hasTes) || (!hasTs && m_hasVs)) {
    // Call ES main function
    args.clear();

    auto intfData = m_pipelineState->getShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    const unsigned userDataCount = intfData->userDataCount;
    spillTableIdx = intfData->userDataUsage.spillTable;

    unsigned userDataIdx = 0;

    auto esArgBegin = esEntryPoint->arg_begin();
    const unsigned esArgCount = esEntryPoint->arg_size();

    unsigned esArgIdx = 0;

    // Set up user data SGPRs
    while (userDataIdx < userDataCount) {
      assert(esArgIdx < esArgCount);

      auto esArg = (esArgBegin + esArgIdx);
      assert(esArg->hasAttribute(Attribute::InReg));

      auto esArgTy = esArg->getType();
      if (esArgTy->isVectorTy()) {
        assert(esArgTy->getVectorElementType()->isIntegerTy());

        const unsigned userDataSize = esArgTy->getVectorNumElements();

        std::vector<Constant *> shuffleMask;
        for (unsigned i = 0; i < userDataSize; ++i)
          shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), userDataIdx + i));

        userDataIdx += userDataSize;

        auto esUserData = new ShuffleVectorInst(userData, userData, ConstantVector::get(shuffleMask), "", beginEsBlock);
        args.push_back(esUserData);
      } else {
        assert(esArgTy->isIntegerTy());

        auto esUserData = ExtractElementInst::Create(
            userData, ConstantInt::get(Type::getInt32Ty(*m_context), userDataIdx), "", beginEsBlock);
        args.push_back(esUserData);
        ++userDataIdx;
      }

      ++esArgIdx;
    }

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
    }

    assert(esArgIdx == esArgCount); // Must have visit all arguments of ES entry point

    CallInst::Create(esEntryPoint, args, "", beginEsBlock);
  }
  BranchInst::Create(endEsBlock, beginEsBlock);

  // Construct ".endes" block
  args.clear();
  attribs.clear();
  attribs.push_back(Attribute::NoRecurse);
  emitCall("llvm.amdgcn.s.barrier", Type::getVoidTy(*m_context), args, attribs, endEsBlock);

  auto gsEnable = new ICmpInst(*endEsBlock, ICmpInst::ICMP_ULT, threadId, gsPrimCount, "");
  BranchInst::Create(beginGsBlock, endGsBlock, gsEnable, endEsBlock);

  // Construct ".begings" block
  {
    attribs.clear();
    attribs.push_back(Attribute::ReadNone);

    args.clear();
    args.push_back(esGsOffsets01);
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));

    auto esGsOffset0 = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, beginGsBlock);

    args.clear();
    args.push_back(esGsOffsets01);
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));

    auto esGsOffset1 = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, beginGsBlock);

    args.clear();
    args.push_back(esGsOffsets23);
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));

    auto esGsOffset2 = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, beginGsBlock);

    args.clear();
    args.push_back(esGsOffsets23);
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));

    auto esGsOffset3 = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, beginGsBlock);

    args.clear();
    args.push_back(esGsOffsets45);
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 0));
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));

    auto esGsOffset4 = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, beginGsBlock);

    args.clear();
    args.push_back(esGsOffsets45);
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));
    args.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), 16));

    auto esGsOffset5 = emitCall("llvm.amdgcn.ubfe.i32", Type::getInt32Ty(*m_context), args, attribs, beginGsBlock);

    // Call GS main function
    args.clear();

    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
    const unsigned userDataCount = intfData->userDataCount;

    unsigned userDataIdx = 0;

    auto gsArgBegin = gsEntryPoint->arg_begin();

    unsigned gsArgIdx = 0;

    // Set up user data SGPRs
    while (userDataIdx < userDataCount) {
      assert(gsArgIdx < gsEntryPoint->arg_size());

      auto gsArg = (gsArgBegin + gsArgIdx);
      assert(gsArg->hasAttribute(Attribute::InReg));

      auto gsArgTy = gsArg->getType();
      if (gsArgTy->isVectorTy()) {
        assert(gsArgTy->getVectorElementType()->isIntegerTy());

        const unsigned userDataSize = gsArgTy->getVectorNumElements();

        std::vector<Constant *> shuffleMask;
        for (unsigned i = 0; i < userDataSize; ++i)
          shuffleMask.push_back(ConstantInt::get(Type::getInt32Ty(*m_context), userDataIdx + i));

        userDataIdx += userDataSize;

        auto gsUserData = new ShuffleVectorInst(userData, userData, ConstantVector::get(shuffleMask), "", beginGsBlock);
        args.push_back(gsUserData);
      } else {
        assert(gsArgTy->isIntegerTy());
        unsigned actualUserDataIdx = userDataIdx;
        if (intfData->spillTable.sizeInDwords > 0) {
          if (intfData->userDataUsage.spillTable == userDataIdx) {
            if (spillTableIdx > 0)
              actualUserDataIdx = spillTableIdx;
          }
        }
        auto gsUserData = ExtractElementInst::Create(
            userData, ConstantInt::get(Type::getInt32Ty(*m_context), actualUserDataIdx), "", beginGsBlock);
        args.push_back(gsUserData);
        ++userDataIdx;
      }

      ++gsArgIdx;
    }

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

    CallInst::Create(gsEntryPoint, args, "", beginGsBlock);
  }
  BranchInst::Create(endGsBlock, beginGsBlock);

  // Construct ".endgs" block
  ReturnInst::Create(*m_context, endGsBlock);

  return entryPoint;
}
