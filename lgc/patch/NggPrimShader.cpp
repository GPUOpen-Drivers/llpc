/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  NggPrimShader.cpp
 * @brief LLPC source file: contains implementation of class lgc::NggPrimShader.
 ***********************************************************************************************************************
 */
#include "NggPrimShader.h"
#include "Gfx9Chip.h"
#include "NggLdsManager.h"
#include "ShaderMerger.h"
#include "lgc/PassManager.h"
#include "lgc/state/PalMetadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "lgc-ngg-prim-shader"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
NggPrimShader::NggPrimShader(PipelineState *pipelineState)
    : m_pipelineState(pipelineState), m_context(&pipelineState->getContext()),
      m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()), m_nggControl(m_pipelineState->getNggControl()),
      m_ldsManager(nullptr), m_builder(new IRBuilder<>(*m_context)) {
  // Always allow reciprocal, to change fdiv(1/x) to rcp(x)
  FastMathFlags fastMathFlags;
  fastMathFlags.setAllowReciprocal();
  m_builder->setFastMathFlags(fastMathFlags);

  assert(m_pipelineState->isGraphics());

  buildPrimShaderCbLayoutLookupTable();

  memset(&m_nggFactor, 0, sizeof(m_nggFactor));

  m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
  m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
  m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
  m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

  // NOTE: For NGG GS mode, we change data layout of output vertices. They are grouped by vertex streams now.
  // Vertices belonging to different vertex streams are in different regions of GS-VS ring. Here, we calculate
  // the base offset of each vertex streams and record them. See NggPrimShader::exportGsOutput for detail.
  if (m_hasGs) {
    unsigned vertexItemSizes[MaxGsStreams] = {};
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      vertexItemSizes[i] = 4 * resUsage->inOutUsage.gs.outLocCount[i];

    unsigned gsVsRingItemSizes[MaxGsStreams] = {};
    const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      gsVsRingItemSizes[i] = vertexItemSizes[i] * geometryMode.outputVertices;

    const unsigned gsPrimsPerSubgroup =
        resUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup * geometryMode.invocations;
    unsigned gsStreamBase = 0;
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      m_gsStreamBases[i] = gsStreamBase;
      gsStreamBase += gsVsRingItemSizes[i] * gsPrimsPerSubgroup;
    }
  } else
    memset(m_gsStreamBases, 0, sizeof(m_gsStreamBases));
}

// =====================================================================================================================
NggPrimShader::~NggPrimShader() {
  if (m_ldsManager)
    delete m_ldsManager;
}

// =====================================================================================================================
// Generates NGG primitive shader entry-point.
//
// @param esEntryPoint : Entry-point of hardware export shader (ES) (could be null)
// @param gsEntryPoint : Entry-point of hardware geometry shader (GS) (could be null)
// @param copyShaderEntryPoint : Entry-point of hardware vertex shader (VS, copy shader) (could be null)
Function *NggPrimShader::generate(Function *esEntryPoint, Function *gsEntryPoint, Function *copyShaderEntryPoint) {
  assert(m_gfxIp.major >= 10);

  // ES and GS could not be null at the same time
  assert((!esEntryPoint && !gsEntryPoint) == false);

  Module *module = nullptr;
  if (esEntryPoint) {
    module = esEntryPoint->getParent();
    esEntryPoint->setName(lgcName::NggEsEntryPoint);
    esEntryPoint->setCallingConv(CallingConv::C);
    esEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    esEntryPoint->addFnAttr(Attribute::AlwaysInline);
  }

  if (gsEntryPoint) {
    module = gsEntryPoint->getParent();
    gsEntryPoint->setName(lgcName::NggGsEntryPoint);
    gsEntryPoint->setCallingConv(CallingConv::C);
    gsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    gsEntryPoint->addFnAttr(Attribute::AlwaysInline);

    assert(copyShaderEntryPoint); // Copy shader must be present
    copyShaderEntryPoint->setName(lgcName::NggCopyShaderEntryPoint);
    copyShaderEntryPoint->setCallingConv(CallingConv::C);
    copyShaderEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    copyShaderEntryPoint->addFnAttr(Attribute::AlwaysInline);
  }

  // Create NGG LDS manager
  assert(module);
  assert(!m_ldsManager);
  m_ldsManager = new NggLdsManager(module, m_pipelineState, m_builder.get());

  return generatePrimShaderEntryPoint(module);
}

// =====================================================================================================================
// Generates the type for the new entry-point of NGG primitive shader.
//
// @param module : IR module (for getting ES function if needed to get vertex fetch types)
// @param [out] inRegMask : "Inreg" bit mask for the arguments
FunctionType *NggPrimShader::generatePrimShaderEntryPointType(Module *module, uint64_t *inRegMask) const {
  std::vector<Type *> argTys;

  // First 8 system values (SGPRs)
  for (unsigned i = 0; i < EsGsSpecialSysValueCount; ++i) {
    argTys.push_back(m_builder->getInt32Ty());
    *inRegMask |= (1ull << i);
  }

  // User data (SGPRs)
  unsigned userDataCount = 0;

  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);

  bool hasTs = (m_hasTcs || m_hasTes);
  if (m_hasGs) {
    // GS is present in primitive shader (ES-GS merged shader)
    userDataCount = gsIntfData->userDataCount;

    if (hasTs) {
      if (m_hasTes) {
        userDataCount = std::max(tesIntfData->userDataCount, userDataCount);

        if (gsIntfData->spillTable.sizeInDwords > 0 && tesIntfData->spillTable.sizeInDwords == 0) {
          tesIntfData->userDataUsage.spillTable = userDataCount;
          ++userDataCount;
          assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
        }
      }
    } else {
      if (m_hasVs) {
        userDataCount = std::max(vsIntfData->userDataCount, userDataCount);

        if (gsIntfData->spillTable.sizeInDwords > 0 && vsIntfData->spillTable.sizeInDwords == 0) {
          vsIntfData->userDataUsage.spillTable = userDataCount;
          ++userDataCount;
        }
      }
    }
  } else {
    // No GS in primitive shader (ES only)
    if (hasTs) {
      if (m_hasTes)
        userDataCount = tesIntfData->userDataCount;
    } else {
      if (m_hasVs)
        userDataCount = vsIntfData->userDataCount;
    }
  }

  assert(userDataCount > 0);
  argTys.push_back(VectorType::get(m_builder->getInt32Ty(), userDataCount));
  *inRegMask |= (1ull << EsGsSpecialSysValueCount);

  // Other system values (VGPRs)
  argTys.push_back(m_builder->getInt32Ty()); // ES to GS offsets (vertex 0 and 1)
  argTys.push_back(m_builder->getInt32Ty()); // ES to GS offsets (vertex 2 and 3)
  argTys.push_back(m_builder->getInt32Ty()); // Primitive ID (GS)
  argTys.push_back(m_builder->getInt32Ty()); // Invocation ID
  argTys.push_back(m_builder->getInt32Ty()); // ES to GS offsets (vertex 4 and 5)

  if (hasTs) {
    argTys.push_back(m_builder->getFloatTy()); // X of TessCoord (U)
    argTys.push_back(m_builder->getFloatTy()); // Y of TessCoord (V)
    argTys.push_back(m_builder->getInt32Ty()); // Relative patch ID
    argTys.push_back(m_builder->getInt32Ty()); // Patch ID
  } else {
    argTys.push_back(m_builder->getInt32Ty()); // Vertex ID
    argTys.push_back(m_builder->getInt32Ty()); // Relative vertex ID (auto index)
    argTys.push_back(m_builder->getInt32Ty()); // Primitive ID (VS)
    argTys.push_back(m_builder->getInt32Ty()); // Instance ID
  }

  // If the ES is the API VS, and it is a fetchless VS, then we need to add args for the vertex fetches.
  if (!hasTs) {
    unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
    if (vertexFetchCount != 0) {
      // TODO: This will not work with non-GS culling.
      if (!m_hasGs && !m_nggControl->passthroughMode)
        m_pipelineState->setError("Fetchless VS with non-GS NGG culling not supported");
      // The final vertexFetchCount args of the ES (API VS) are the vertex fetches.
      Function *esEntry = module->getFunction(lgcName::NggEsEntryPoint);
      unsigned esArgSize = esEntry->arg_size();
      for (unsigned idx = esArgSize - vertexFetchCount; idx != esArgSize; ++idx)
        argTys.push_back(esEntry->getArg(idx)->getType());
    }
  }

  return FunctionType::get(m_builder->getVoidTy(), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for NGG primitive shader.
//
// @param module : LLVM module
Function *NggPrimShader::generatePrimShaderEntryPoint(Module *module) {
  uint64_t inRegMask = 0;
  auto entryPointTy = generatePrimShaderEntryPointType(module, &inRegMask);

  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::NggPrimShaderEntryPoint);

  module->getFunctionList().push_front(entryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
  }

  auto arg = entryPoint->arg_begin();

  Value *userDataAddrLow = (arg + EsGsSysValueUserDataAddrLow);
  Value *userDataAddrHigh = (arg + EsGsSysValueUserDataAddrHigh);
  Value *mergedGroupInfo = (arg + EsGsSysValueMergedGroupInfo);
  Value *mergedWaveInfo = (arg + EsGsSysValueMergedWaveInfo);
  Value *offChipLdsBase = (arg + EsGsSysValueOffChipLdsBase);
  Value *sharedScratchOffset = (arg + EsGsSysValueSharedScratchOffset);
  Value *primShaderTableAddrLow = (arg + EsGsSysValuePrimShaderTableAddrLow);
  Value *primShaderTableAddrHigh = (arg + EsGsSysValuePrimShaderTableAddrHigh);

  arg += EsGsSpecialSysValueCount;

  Value *userData = arg++;

  Value *esGsOffsets01 = arg;
  Value *esGsOffsets23 = (arg + 1);
  Value *gsPrimitiveId = (arg + 2);
  Value *invocationId = (arg + 3);
  Value *esGsOffsets45 = (arg + 4);

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *relVertexId = (arg + 6);
  Value *vsPrimitiveId = (arg + 7);
  Value *instanceId = (arg + 8);

  userDataAddrLow->setName("userDataAddrLow");
  userDataAddrHigh->setName("userDataAddrHigh");
  mergedGroupInfo->setName("mergedGroupInfo");
  mergedWaveInfo->setName("mergedWaveInfo");
  offChipLdsBase->setName("offChipLdsBase");
  sharedScratchOffset->setName("sharedScratchOffset");
  primShaderTableAddrLow->setName("primShaderTableAddrLow");
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  userData->setName("userData");
  esGsOffsets01->setName("esGsOffsets01");
  esGsOffsets23->setName("esGsOffsets23");
  gsPrimitiveId->setName("gsPrimitiveId");
  invocationId->setName("invocationId");
  esGsOffsets45->setName("esGsOffsets45");

  if (m_hasTes) {
    tessCoordX->setName("tessCoordX");
    tessCoordY->setName("tessCoordY");
    relPatchId->setName("relPatchId");
    patchId->setName("patchId");
  } else {
    vertexId->setName("vertexId");
    relVertexId->setName("relVertexId");
    vsPrimitiveId->setName("vsPrimitiveId");
    instanceId->setName("instanceId");
  }

  if (m_hasGs) {
    // GS is present in primitive shader (ES-GS merged shader)
    constructPrimShaderWithGs(module);
  } else {
    // GS is not present in primitive shader (ES-only shader)
    constructPrimShaderWithoutGs(module);
  }

  return entryPoint;
}

// =====================================================================================================================
// Builds layout lookup table of NGG primitive shader constant buffer, setting up a collection of buffer offsets
// according to the definition of this constant buffer in ABI.
void NggPrimShader::buildPrimShaderCbLayoutLookupTable() {
  m_cbLayoutTable = {}; // Initialized to all-zeros

  const unsigned pipelineStateOffset = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
  m_cbLayoutTable.gsAddressLo = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, gsAddressLo);
  m_cbLayoutTable.gsAddressHi = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, gsAddressHi);
  m_cbLayoutTable.paClVteCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClVteCntl);
  m_cbLayoutTable.paSuVtxCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paSuVtxCntl);
  m_cbLayoutTable.paClClipCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
  m_cbLayoutTable.paSuScModeCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paSuScModeCntl);
  m_cbLayoutTable.paClGbHorzClipAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzClipAdj);
  m_cbLayoutTable.paClGbVertClipAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertClipAdj);
  m_cbLayoutTable.paClGbHorzDiscAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
  m_cbLayoutTable.paClGbVertDiscAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
  m_cbLayoutTable.vgtPrimitiveType = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, vgtPrimitiveType);

  const unsigned renderStateOffset = offsetof(Util::Abi::PrimShaderCbLayout, renderStateCb);
  m_cbLayoutTable.primitiveRestartEnable =
      renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, primitiveRestartEnable);
  m_cbLayoutTable.primitiveRestartIndex =
      renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, primitiveRestartIndex);
  m_cbLayoutTable.matchAllBits = renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, matchAllBits);
  m_cbLayoutTable.enableConservativeRasterization =
      renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, enableConservativeRasterization);

  const unsigned vportStateOffset = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
  const unsigned vportControlSize = sizeof(Util::Abi::PrimShaderVportCb) / Util::Abi::MaxViewports;
  for (unsigned i = 0; i < Util::Abi::MaxViewports; ++i) {
    m_cbLayoutTable.vportControls[i].paClVportXscale =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    m_cbLayoutTable.vportControls[i].paClVportXoffset =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXoffset);
    m_cbLayoutTable.vportControls[i].paClVportYscale =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    m_cbLayoutTable.vportControls[i].paClVportYoffset =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYoffset);
  }
}

// =====================================================================================================================
// Constructs primitive shader for ES-only merged shader (GS is not present).
//
// @param module : LLVM module
void NggPrimShader::constructPrimShaderWithoutGs(Module *module) {
  assert(m_hasGs == false);

  const bool hasTs = (m_hasTcs || m_hasTes);

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

  auto entryPoint = module->getFunction(lgcName::NggPrimShaderEntryPoint);

  auto arg = entryPoint->arg_begin();

  Value *mergedGroupInfo = (arg + EsGsSysValueMergedGroupInfo);
  Value *mergedWaveInfo = (arg + EsGsSysValueMergedWaveInfo);
  Value *primShaderTableAddrLow = (arg + EsGsSysValuePrimShaderTableAddrLow);
  Value *primShaderTableAddrHigh = (arg + EsGsSysValuePrimShaderTableAddrHigh);

  arg += (EsGsSpecialSysValueCount + 1);

  Value *esGsOffsets01 = arg;
  Value *esGsOffsets23 = (arg + 1);
  Value *gsPrimitiveId = (arg + 2);

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *instanceId = (arg + 8);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

  // NOTE: If primitive ID is used in VS, we have to insert several basic blocks to distribute the value across
  // LDS because the primitive ID is provided as per-primitive instead of per-vertex. The algorithm is something
  // like this:
  //
  //   if (threadIdInWave < primCountInWave)
  //   {
  //      ldsOffset = vindex0 * 4
  //      ds_write ldsOffset, primId
  //   }
  //
  //   s_barrier
  //
  //   if (threadIdInWave < vertCountInWave)
  //   {
  //      ldsOffset = threadIdInSubgroup * 4
  //      ds_read primId, ldsOffset
  //   }
  //
  //   s_barrier
  //
  const bool distributePrimId = hasTs ? false : resUsage->builtInUsage.vs.primitiveId;

  // No GS in primitive shader (ES only)
  if (m_nggControl->passthroughMode) {
    // Pass-through mode

    // define dllexport amdgpu_gs @_amdgpu_gs_main(
    //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8)
    // {
    // .entry:
    //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
    //     call void @llvm.amdgcn.init.exec(i64 -1)
    //
    //     ; Get thread ID in a wave:
    //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
    //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
    //     ;   threadIdInWave = bitCount
    //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
    //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadIdInWave)
    //
    //     %waveIdInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 24, i32 4)
    //     %threadIdInSubgroup = mul i32 %waveIdInSubgroup, %waveSize
    //     %threadIdInSubgroup = add i32 %threadIdInSubgroup, %threadIdInWave
    //
    //     %primCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 22, i32 9)
    //     %vertCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 12, i32 9)
    //
    //     %primCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
    //     %vertCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
    //
    //     %primValid = icmp ult i32 %threadIdInWave , %primCountInWave
    //     br i1 %primValid, label %.writePrimId, label %.endWritePrimId
    // [
    // .writePrimId:
    //     ; Write LDS region (primitive ID)
    //     br label %.endWritePrimId
    //
    // .endWritePrimId:
    //     call void @llvm.amdgcn.s.barrier()
    //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
    //     br i1 %vertValid, label %.readPrimId, label %.endReadPrimId
    //
    // .readPrimId:
    //     ; Read LDS region (primitive ID)
    //     br label %.endReadPrimId
    //
    // .endReadPrimId:
    // ]
    //     call void @llvm.amdgcn.s.barrier()
    //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
    //     br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
    //
    // .allocReq:
    //     ; Do parameter cache (PC) alloc request: s_sendmsg(GS_ALLOC_REQ, ...)
    //     br label %.endAllocReq
    //
    // .endAllocReq:
    //     %primExp = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
    //     br i1 %primExp, label %.expPrim, label %.endExpPrim
    //
    // .expPrim:
    //     ; Do primitive export: exp prim, ...
    //     br label %.endExpPrim
    //
    // .endExpPrim:
    //     %vertExp = icmp ult i32 %threadIdInSubgroup, %vertCountInSubgroup
    //     br i1 %vertExp, label %.expVert, label %.endExpVert
    //
    // .expVert:
    //     call void @llpc.ngg.ES.main(%sgpr..., %userData..., %vgpr...)
    //     br label %.endExpVert
    //
    // .endExpVert:
    //     ret void
    // }

    // Define basic blocks
    auto entryBlock = createBlock(entryPoint, ".entry");

    // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
    BasicBlock *writePrimIdBlock = nullptr;
    BasicBlock *endWritePrimIdBlock = nullptr;
    BasicBlock *readPrimIdBlock = nullptr;
    BasicBlock *endReadPrimIdBlock = nullptr;

    if (distributePrimId) {
      writePrimIdBlock = createBlock(entryPoint, ".writePrimId");
      endWritePrimIdBlock = createBlock(entryPoint, ".endWritePrimId");

      readPrimIdBlock = createBlock(entryPoint, ".readPrimId");
      endReadPrimIdBlock = createBlock(entryPoint, ".endReadPrimId");
    }

    auto allocReqBlock = createBlock(entryPoint, ".allocReq");
    auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

    auto expPrimBlock = createBlock(entryPoint, ".expPrim");
    auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

    auto expVertBlock = createBlock(entryPoint, ".expVert");
    auto endExpVertBlock = createBlock(entryPoint, ".endExpVert");

    // Construct ".entry" block
    {
      m_builder->SetInsertPoint(entryBlock);

      initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

      // Record ES-GS vertex offsets info
      m_nggFactor.esGsOffsets01 = esGsOffsets01;

      if (distributePrimId) {
        auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
        m_builder->CreateCondBr(primValid, writePrimIdBlock, endWritePrimIdBlock);
      } else {
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
      }
    }

    if (distributePrimId) {
      // Construct ".writePrimId" block
      {
        m_builder->SetInsertPoint(writePrimIdBlock);

        // Primitive data layout
        //   ES_GS_OFFSET01[31]    = null primitive flag
        //   ES_GS_OFFSET01[28:20] = vertexId2 (in bytes)
        //   ES_GS_OFFSET01[18:10] = vertexId1 (in bytes)
        //   ES_GS_OFFSET01[8:0]   = vertexId0 (in bytes)

        // Distribute primitive ID
        auto vertexId0 =
            m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                       {m_nggFactor.esGsOffsets01, m_builder->getInt32(0), m_builder->getInt32(9)});

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

        auto ldsOffset = m_builder->CreateShl(vertexId0, 2);
        ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

        auto primIdWriteValue = gsPrimitiveId;
        m_ldsManager->writeValueToLds(primIdWriteValue, ldsOffset);

        BranchInst::Create(endWritePrimIdBlock, writePrimIdBlock);
      }

      // Construct ".endWritePrimId" block
      {
        m_builder->SetInsertPoint(endWritePrimIdBlock);

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
        m_builder->CreateCondBr(vertValid, readPrimIdBlock, endReadPrimIdBlock);
      }

      // Construct ".readPrimId" block
      Value *primIdReadValue = nullptr;
      {
        m_builder->SetInsertPoint(readPrimIdBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

        auto ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
        ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

        primIdReadValue = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

        m_builder->CreateBr(endReadPrimIdBlock);
      }

      // Construct ".endReadPrimId" block
      {
        m_builder->SetInsertPoint(endReadPrimIdBlock);

        auto primitiveId = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);

        primitiveId->addIncoming(primIdReadValue, readPrimIdBlock);
        primitiveId->addIncoming(m_builder->getInt32(0), endWritePrimIdBlock);

        // Record primitive ID
        m_nggFactor.primitiveId = primitiveId;

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
      }
    }

    // Construct ".allocReq" block
    {
      m_builder->SetInsertPoint(allocReqBlock);

      doParamCacheAllocRequest();
      m_builder->CreateBr(endAllocReqBlock);
    }

    // Construct ".endAllocReq" block
    {
      m_builder->SetInsertPoint(endAllocReqBlock);

      auto primExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
      m_builder->CreateCondBr(primExp, expPrimBlock, endExpPrimBlock);
    }

    // Construct ".expPrim" block
    {
      m_builder->SetInsertPoint(expPrimBlock);

      doPrimitiveExportWithoutGs();
      m_builder->CreateBr(endExpPrimBlock);
    }

    // Construct ".endExpPrim" block
    {
      m_builder->SetInsertPoint(endExpPrimBlock);

      auto vertExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
      m_builder->CreateCondBr(vertExp, expVertBlock, endExpVertBlock);
    }

    // Construct ".expVert" block
    {
      m_builder->SetInsertPoint(expVertBlock);

      runEsOrEsVariant(module, lgcName::NggEsEntryPoint, entryPoint->arg_begin(), false, nullptr, expVertBlock);

      m_builder->CreateBr(endExpVertBlock);
    }

    // Construct ".endExpVert" block
    {
      m_builder->SetInsertPoint(endExpVertBlock);

      m_builder->CreateRetVoid();
    }
  } else {
    // Non pass-through mode

    // define dllexport amdgpu_gs @_amdgpu_gs_main(
    //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8])
    // {
    // .entry:
    //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
    //     call void @llvm.amdgcn.init.exec(i64 -1)
    //
    //     ; Get thread ID in a wave:
    //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
    //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
    //     ;   threadIdInWave = bitCount
    //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
    //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadIdInWave)
    //
    //     %waveIdInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 24, i32 4)
    //
    //     %threadIdInSubgroup = mul i32 %waveIdInSubgroup, %waveSize
    //     %threadIdInSubgroup = add i32 %threadIdInSubgroup, %threadIdInWave
    //
    //     %primCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 22, i32 9)
    //     %vertCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 12, i32 9)
    //
    //     %primCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
    //     %vertCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
    //
    // <if (distributePrimId)>
    // [
    // .writePrimId:
    //     ; Write LDS region (primitive ID)
    //     br label %.endWritePrimId
    //
    // .endWritePrimId:
    //     call void @llvm.amdgcn.s.barrier()
    //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
    //     br i1 %vertValid, label %.readPrimId, label %.endReadPrimId
    //
    // .readPrimId:
    //     ; Read LDS region (primitive ID)
    //     br label %.endReadPrimId
    //
    // .endReadPrimId:
    //     call void @llvm.amdgcn.s.barrier()
    // ]
    //     %firstThreadInSubgroup = icmp eq i32 %threadIdInSubgroup, 0
    //     br i1 %firstThreadInSubgroup, label %.zeroPrimWaveCount, label %.endZeroPrimWaveCount
    //
    // .zeroThreadCount:
    //     ; Zero LDS region (primitive/vertex count in waves), do it for the first thread
    //     br label %.endZeroThreadCount
    //
    // .endZeroThreadCount:
    //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
    //     br i1 %firstWaveInSubgroup, label %.zeroDrawFlag, label %.endZeroDrawFlag
    //
    // .zeroDrawFlag:
    //     ; Zero LDS regision (draw flag), do it for the first wave
    //     br label %.endZeroDrawFlag
    //
    // .endZeroDrawFlag:
    //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
    //     br i1 %vertValid, label %.writePosData, label %.endWritePosData
    //
    // .writePosData:
    //     ; Write LDS region (position data)
    //     %expData = call [ POS0: <4 x float>, POS1: <4 x float>, ...,
    //                       PARAM0: <4 x float>, PARAM1: <4 xfloat>, ... ]
    //                     @llpc.ngg.ES.variant(%sgpr..., %userData..., %vgpr...)
    //     br label %.endWritePosData
    //
    // .endWritePosData:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %primValidInWave = icmp ult i32 %threadIdInWave, %primCountInWave
    //     %primValidInSubgroup = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
    //     %primValid = and i1 %primValidInWave, %primValidInSubgroup
    //     br i1 %primValid, label %.culling, label %.endCulling
    //
    // .culling:
    //     ; Do culling
    //     %doCull = call i32 @llpc.ngg.culling.XXX(...)
    //     br label %.endCulling
    //
    // .endCulling:
    //     %cullFlag = phi i1 [ true, %.endWritePosData ], [ %doCull, %.culling ]
    //     %drawFlag = xor i1 1, %cullFlag
    //     br i1 %drawFlag, label %.writeDrawFlag, label %.endWriteDrawFlag
    //
    // .writeDrawFlag:
    //     ; Write LDS region (draw flag)
    //     br label %.endWriteDrawFlag
    //
    // .endWriteDrawFlag:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %drawMask = call i64 @llpc.subgroup.ballot(i1 %drawFlag)
    //     %drawCount = call i64 @llvm.ctpop.i64(i64 %drawMask)
    //     %hasSurviveDraw = icmp ne i64 %drawCount, 0
    //
    //     %theadIdUpbound = sub i32 %waveCountInSubgroup, %waveIdInSubgroup
    //     %threadValid = icmp ult i32 %threadIdInWave, %theadIdUpbound
    //     %primCountAcc = and i1 %hasSurviveDraw, %threadValid
    //     br i1 %primCountAcc, label %.accThreadCount, label %.endAccThreadCount
    //
    // .accThreadCount:
    //     ; Write LDS region (primitive/vertex count in waves)
    //     br label %.endAccThreadCount
    //
    // .endAccThreadCount:
    //     call void @llvm.amdgcn.s.barrier()
    //     br lable %.readThreadCount
    //
    // .readThreadCount:
    //     %vertCountInWaves = ... (read LDS region, vertex count in waves)
    //     %threadCountInWaves = %vertCountInWaves
    //
    //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
    //     %compactDataWrite = and i1 %vertValid, %drawFlag
    //     br i1 %compactDataWrite, label %.writeCompactData, label %.endReadThreadCount
    //
    // .writeCompactData:
    //     ; Write LDS region (compaction data: compacted thread ID, vertex position data,
    //     ; vertex ID/tessCoordX, instance ID/tessCoordY, primitive ID/relative patch ID, patch ID)
    //     br label %.endReadThreadCount
    //
    // .endReadThreadCount:
    //     %hasSurviveVert = icmp ne i32 %vertCountInWaves, 0
    //     %primCountInSubgroup =
    //         select i1 %hasSurviveVert, i32 %primCountInSubgroup, i32 %fullyCulledThreadCount
    //     %vertCountInSubgroup =
    //         select i1 %hasSurviveVert, i32 %vertCountInWaves, i32 %fullyCulledThreadCount
    //
    //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
    //     br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
    //
    // .allocReq:
    //     ; Do parameter cache (PC) alloc request: s_sendmsg(GS_ALLOC_REQ, ...)
    //     br label %.endAllocReq
    //
    // .endAlloReq:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %noSurviveThread = icmp eq %threadCountInWaves, 0
    //     br i1 %noSurviveThread, label %.earlyExit, label %.noEarlyExit
    //
    // .earlyExit:
    //     %firstThreadInSubgroup = icmp eq i32 %threadIdInSubgroup, 0
    //     br i1 %firstThreadInSubgroup, label %.dummyExp, label %.endDummyExp
    //
    // .dummyExp:
    //     ; Do vertex position export: exp pos, ... (off, off, off, off)
    //     ; Do primitive export: exp prim, ... (0, off, off, off)
    //     br label %.endDummyExp
    //
    // .endDummyExp:
    //     ret void
    //
    // .noEarlyExit:
    //     %primExp = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
    //     br i1 %primExp, label %.expPrim, label %.endExpPrim
    //
    // .expPrim:
    //     ; Do primitive export: exp prim, ...
    //     br label %.endExpPrim
    //
    // .endExpPrim:
    //     %vertExp = icmp ult i32 %threadIdInSubgroup, %vertCountInSubgroup
    //     br i1 %vertExp, label %.expVertPos, label %.endExpVertPos
    //
    // .expVertPos:
    //     ; Do vertex position export: exp pos, ...
    //     br label %.endExpVertPos
    //
    // .endExpVertPos:
    //     br i1 %vertExp, label %.expVertParam, label %.endExpVertParam
    //
    // .expVertParam:
    //     ; Do vertex parameter export: exp param, ...
    //     br label %.endExpVertParam
    //
    // .endExpVertParam:
    //     ret void
    // }

    // Thread count when the entire sub-group is fully culled
    const unsigned fullyCulledThreadCount =
        m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups ? 1 : 0;

    // Define basic blocks
    auto entryBlock = createBlock(entryPoint, ".entry");

    // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
    BasicBlock *writePrimIdBlock = nullptr;
    BasicBlock *endWritePrimIdBlock = nullptr;
    BasicBlock *readPrimIdBlock = nullptr;
    BasicBlock *endReadPrimIdBlock = nullptr;

    if (distributePrimId) {
      writePrimIdBlock = createBlock(entryPoint, ".writePrimId");
      endWritePrimIdBlock = createBlock(entryPoint, ".endWritePrimId");

      readPrimIdBlock = createBlock(entryPoint, ".readPrimId");
      endReadPrimIdBlock = createBlock(entryPoint, ".endReadPrimId");
    }

    auto zeroThreadCountBlock = createBlock(entryPoint, ".zeroThreadCount");
    auto endZeroThreadCountBlock = createBlock(entryPoint, ".endZeroThreadCount");

    auto zeroDrawFlagBlock = createBlock(entryPoint, ".zeroDrawFlag");
    auto endZeroDrawFlagBlock = createBlock(entryPoint, ".endZeroDrawFlag");

    auto writePosDataBlock = createBlock(entryPoint, ".writePosData");
    auto endWritePosDataBlock = createBlock(entryPoint, ".endWritePosData");

    auto cullingBlock = createBlock(entryPoint, ".culling");
    auto endCullingBlock = createBlock(entryPoint, ".endCulling");

    auto writeDrawFlagBlock = createBlock(entryPoint, ".writeDrawFlag");
    auto endWriteDrawFlagBlock = createBlock(entryPoint, ".endWriteDrawFlag");

    auto accThreadCountBlock = createBlock(entryPoint, ".accThreadCount");
    auto endAccThreadCountBlock = createBlock(entryPoint, ".endAccThreadCount");

    auto readThreadCountBlock = createBlock(entryPoint, ".readThreadCount");
    auto writeCompactDataBlock = createBlock(entryPoint, ".writeCompactData");
    auto endReadThreadCountBlock = createBlock(entryPoint, ".endReadThreadCount");

    auto allocReqBlock = createBlock(entryPoint, ".allocReq");
    auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

    auto earlyExitBlock = createBlock(entryPoint, ".earlyExit");
    auto noEarlyExitBlock = createBlock(entryPoint, ".noEarlyExit");

    auto expPrimBlock = createBlock(entryPoint, ".expPrim");
    auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

    auto expVertPosBlock = createBlock(entryPoint, ".expVertPos");
    auto endExpVertPosBlock = createBlock(entryPoint, ".endExpVertPos");

    auto expVertParamBlock = createBlock(entryPoint, ".expVertParam");
    auto endExpVertParamBlock = createBlock(entryPoint, ".endExpVertParam");

    // Construct ".entry" block
    {
      m_builder->SetInsertPoint(entryBlock);

      initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

      // Record primitive shader table address info
      m_nggFactor.primShaderTableAddrLow = primShaderTableAddrLow;
      m_nggFactor.primShaderTableAddrHigh = primShaderTableAddrHigh;

      // Record ES-GS vertex offsets info
      m_nggFactor.esGsOffsets01 = esGsOffsets01;
      m_nggFactor.esGsOffsets23 = esGsOffsets23;

      if (distributePrimId) {
        auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
        m_builder->CreateCondBr(primValid, writePrimIdBlock, endWritePrimIdBlock);
      } else {
        auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstThreadInSubgroup, zeroThreadCountBlock, endZeroThreadCountBlock);
      }
    }

    if (distributePrimId) {
      // Construct ".writePrimId" block
      {
        m_builder->SetInsertPoint(writePrimIdBlock);

        // Primitive data layout
        //   ES_GS_OFFSET23[15:0]  = vertexId2 (in dwords)
        //   ES_GS_OFFSET01[31:16] = vertexId1 (in dwords)
        //   ES_GS_OFFSET01[15:0]  = vertexId0 (in dwords)

        // Use vertex0 as provoking vertex to distribute primitive ID
        auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                      {
                                                          m_nggFactor.esGsOffsets01,
                                                          m_builder->getInt32(0),
                                                          m_builder->getInt32(16),
                                                      });

        auto vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

        auto ldsOffset = m_builder->CreateShl(vertexId0, 2);
        ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

        auto primIdWriteValue = gsPrimitiveId;
        m_ldsManager->writeValueToLds(primIdWriteValue, ldsOffset);

        m_builder->CreateBr(endWritePrimIdBlock);
      }

      // Construct ".endWritePrimId" block
      {
        m_builder->SetInsertPoint(endWritePrimIdBlock);

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
        m_builder->CreateCondBr(vertValid, readPrimIdBlock, endReadPrimIdBlock);
      }

      // Construct ".readPrimId" block
      Value *primIdReadValue = nullptr;
      {
        m_builder->SetInsertPoint(readPrimIdBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

        auto ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
        ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

        primIdReadValue = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

        m_builder->CreateBr(endReadPrimIdBlock);
      }

      // Construct ".endReadPrimId" block
      {
        m_builder->SetInsertPoint(endReadPrimIdBlock);

        auto primitiveId = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);

        primitiveId->addIncoming(primIdReadValue, readPrimIdBlock);
        primitiveId->addIncoming(m_builder->getInt32(0), endWritePrimIdBlock);

        // Record primitive ID
        m_nggFactor.primitiveId = primitiveId;

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstThreadInSubgroup, zeroThreadCountBlock, endZeroThreadCountBlock);
      }
    }

    // Construct ".zeroThreadCount" block
    {
      m_builder->SetInsertPoint(zeroThreadCountBlock);

      unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCountInWaves);

      auto zero = m_builder->getInt32(0);

      // Zero per-wave primitive/vertex count
      auto zeros = ConstantVector::getSplat({Gfx9::NggMaxWavesPerSubgroup, false}, zero);

      auto ldsOffset = m_builder->getInt32(regionStart);
      m_ldsManager->writeValueToLds(zeros, ldsOffset);

      // Zero sub-group primitive/vertex count
      ldsOffset = m_builder->getInt32(regionStart + SizeOfDword * Gfx9::NggMaxWavesPerSubgroup);
      m_ldsManager->writeValueToLds(zero, ldsOffset);

      m_builder->CreateBr(endZeroThreadCountBlock);
    }

    // Construct ".endZeroThreadCount" block
    {
      m_builder->SetInsertPoint(endZeroThreadCountBlock);

      auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
      m_builder->CreateCondBr(firstWaveInSubgroup, zeroDrawFlagBlock, endZeroDrawFlagBlock);
    }

    // Construct ".zeroDrawFlag" block
    {
      m_builder->SetInsertPoint(zeroDrawFlagBlock);

      Value *ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInWave, 2);

      unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDrawFlag);

      ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

      auto zero = m_builder->getInt32(0);
      m_ldsManager->writeValueToLds(zero, ldsOffset);

      if (waveCountInSubgroup == 8) {
        assert(waveSize == 32);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(32 * SizeOfDword));
        m_ldsManager->writeValueToLds(zero, ldsOffset);
      }

      m_builder->CreateBr(endZeroDrawFlagBlock);
    }

    // Construct ".endZeroDrawFlag" block
    {
      m_builder->SetInsertPoint(endZeroDrawFlagBlock);

      auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
      m_builder->CreateCondBr(vertValid, writePosDataBlock, endWritePosDataBlock);
    }

    // Construct ".writePosData" block
    std::vector<ExpData> expDataSet;
    bool separateExp = false;
    {
      m_builder->SetInsertPoint(writePosDataBlock);

      separateExp = !resUsage->resourceWrite; // No resource writing

      // NOTE: For vertex compaction, we have to run ES for twice (get vertex position data and
      // get other exported data).
      const auto entryName = separateExp ? lgcName::NggEsEntryVariantPos : lgcName::NggEsEntryVariant;

      runEsOrEsVariant(module, entryName, entryPoint->arg_begin(), false, &expDataSet, writePosDataBlock);

      // Write vertex position data to LDS
      for (const auto &expData : expDataSet) {
        if (expData.target == EXP_TARGET_POS_0) {
          const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);
          assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation

          Value *ldsOffset = m_builder->CreateMul(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(SizeOfVec4));
          ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

          // Use 128-bit LDS store
          m_ldsManager->writeValueToLds(expData.expValue, ldsOffset, true);

          break;
        }
      }

      // Write cull distance sign mask to LDS
      if (m_nggControl->enableCullDistanceCulling) {
        unsigned clipCullPos = EXP_TARGET_POS_1;
        std::vector<Value *> clipCullDistance;
        std::vector<Value *> cullDistance;

        bool usePointSize = false;
        bool useLayer = false;
        bool useViewportIndex = false;
        unsigned clipDistanceCount = 0;
        unsigned cullDistanceCount = 0;

        if (hasTs) {
          const auto &builtInUsage = resUsage->builtInUsage.tes;

          usePointSize = builtInUsage.pointSize;
          useLayer = builtInUsage.layer;
          useViewportIndex = builtInUsage.viewportIndex;
          clipDistanceCount = builtInUsage.clipDistance;
          cullDistanceCount = builtInUsage.cullDistance;
        } else {
          const auto &builtInUsage = resUsage->builtInUsage.vs;

          usePointSize = builtInUsage.pointSize;
          useLayer = builtInUsage.layer;
          useViewportIndex = builtInUsage.viewportIndex;
          clipDistanceCount = builtInUsage.clipDistance;
          cullDistanceCount = builtInUsage.cullDistance;
        }

        // NOTE: When gl_PointSize, gl_Layer, or gl_ViewportIndex is used, gl_ClipDistance[] or
        // gl_CullDistance[] should start from pos2.
        clipCullPos = (usePointSize || useLayer || useViewportIndex) ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;

        // Collect clip/cull distance from exported value
        for (const auto &expData : expDataSet) {
          if (expData.target == clipCullPos || expData.target == clipCullPos + 1) {
            for (unsigned i = 0; i < 4; ++i) {
              auto expValue = m_builder->CreateExtractElement(expData.expValue, i);
              clipCullDistance.push_back(expValue);
            }
          }
        }
        assert(clipCullDistance.size() < MaxClipCullDistanceCount);

        for (unsigned i = clipDistanceCount; i < clipDistanceCount + cullDistanceCount; ++i)
          cullDistance.push_back(clipCullDistance[i]);

        // Calculate the sign mask for cull distance
        Value *signMask = m_builder->getInt32(0);
        for (unsigned i = 0; i < cullDistance.size(); ++i) {
          auto cullDistanceVal = m_builder->CreateBitCast(cullDistance[i], m_builder->getInt32Ty());

          Value *signBit =
              m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                         {cullDistanceVal, m_builder->getInt32(31), m_builder->getInt32(1)});
          signBit = m_builder->CreateShl(signBit, i);

          signMask = m_builder->CreateOr(signMask, signBit);
        }

        // Write the sign mask to LDS
        const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionCullDistance);

        Value *ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

        m_ldsManager->writeValueToLds(signMask, ldsOffset);
      }

      m_builder->CreateBr(endWritePosDataBlock);
    }

    // Construct ".endWritePosData" block
    {
      m_builder->SetInsertPoint(endWritePosDataBlock);

      auto undef = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 4));
      for (auto &expData : expDataSet) {
        PHINode *expValue = m_builder->CreatePHI(VectorType::get(Type::getFloatTy(*m_context), 4), 2);
        expValue->addIncoming(expData.expValue, writePosDataBlock);
        expValue->addIncoming(undef, endZeroDrawFlagBlock);

        expData.expValue = expValue; // Update the exportd data
      }

      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

      auto primValidInWave = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
      auto primValidInSubgroup =
          m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);

      auto primValid = m_builder->CreateAnd(primValidInWave, primValidInSubgroup);
      m_builder->CreateCondBr(primValid, cullingBlock, endCullingBlock);
    }

    // Construct ".culling" block
    Value *doCull = nullptr;
    {
      m_builder->SetInsertPoint(cullingBlock);

      Value *vertexId0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.esGsOffsets01,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16),
                                                    });
      vertexId0 = m_builder->CreateLShr(vertexId0, 2);

      Value *vertexId1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.esGsOffsets01,
                                                        m_builder->getInt32(16),
                                                        m_builder->getInt32(16),
                                                    });
      vertexId1 = m_builder->CreateLShr(vertexId1, 2);

      Value *vertexId2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.esGsOffsets23,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16),
                                                    });
      vertexId2 = m_builder->CreateLShr(vertexId2, 2);

      doCull = doCulling(module, vertexId0, vertexId1, vertexId2);
      m_builder->CreateBr(endCullingBlock);
    }

    // Construct ".endCulling" block
    Value *drawFlag = nullptr;
    PHINode *cullFlag = nullptr;
    {
      m_builder->SetInsertPoint(endCullingBlock);

      cullFlag = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);

      cullFlag->addIncoming(m_builder->getTrue(), endWritePosDataBlock);
      cullFlag->addIncoming(doCull, cullingBlock);

      drawFlag = m_builder->CreateNot(cullFlag);
      m_builder->CreateCondBr(drawFlag, writeDrawFlagBlock, endWriteDrawFlagBlock);
    }

    // Construct ".writeDrawFlag" block
    {
      m_builder->SetInsertPoint(writeDrawFlagBlock);

      auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {esGsOffsets01, m_builder->getInt32(0), m_builder->getInt32(16)});
      auto vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

      auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {esGsOffsets01, m_builder->getInt32(16), m_builder->getInt32(16)});
      auto vertexId1 = m_builder->CreateLShr(esGsOffset1, 2);

      auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {esGsOffsets23, m_builder->getInt32(0), m_builder->getInt32(16)});
      auto vertexId2 = m_builder->CreateLShr(esGsOffset2, 2);

      Value *vertexId[3] = {vertexId0, vertexId1, vertexId2};

      unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDrawFlag);
      auto regionStartVal = m_builder->getInt32(regionStart);

      auto one = m_builder->getInt8(1);

      for (unsigned i = 0; i < 3; ++i) {
        auto ldsOffset = m_builder->CreateAdd(regionStartVal, vertexId[i]);
        m_ldsManager->writeValueToLds(one, ldsOffset);
      }

      m_builder->CreateBr(endWriteDrawFlagBlock);
    }

    // Construct ".endWriteDrawFlag" block
    Value *drawCount = nullptr;
    {
      m_builder->SetInsertPoint(endWriteDrawFlagBlock);

      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

      unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDrawFlag);

      auto ldsOffset = m_builder->CreateAdd(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(regionStart));

      drawFlag = m_ldsManager->readValueFromLds(m_builder->getInt8Ty(), ldsOffset);
      drawFlag = m_builder->CreateTrunc(drawFlag, m_builder->getInt1Ty());

      auto drawMask = doSubgroupBallot(drawFlag);

      drawCount = m_builder->CreateIntrinsic(Intrinsic::ctpop, m_builder->getInt64Ty(), drawMask);
      drawCount = m_builder->CreateTrunc(drawCount, m_builder->getInt32Ty());

      auto threadIdUpbound =
          m_builder->CreateSub(m_builder->getInt32(waveCountInSubgroup), m_nggFactor.waveIdInSubgroup);
      auto threadValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, threadIdUpbound);

      m_builder->CreateCondBr(threadValid, accThreadCountBlock, endAccThreadCountBlock);
    }

    // Construct ".accThreadCount" block
    {
      m_builder->SetInsertPoint(accThreadCountBlock);

      auto ldsOffset = m_builder->CreateAdd(m_nggFactor.waveIdInSubgroup, m_nggFactor.threadIdInWave);
      ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(1));
      ldsOffset = m_builder->CreateShl(ldsOffset, 2);

      unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCountInWaves);

      ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
      m_ldsManager->atomicOpWithLds(AtomicRMWInst::Add, drawCount, ldsOffset);

      m_builder->CreateBr(endAccThreadCountBlock);
    }

    // Construct ".endAccThreadCount" block
    {
      m_builder->SetInsertPoint(endAccThreadCountBlock);

      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
      m_builder->CreateBr(readThreadCountBlock);
    }

    // Construct ".readThreadCount" block
    Value *threadCountInWaves = nullptr;
    Value *vertCountInWaves = nullptr;
    Value *vertCountInPrevWaves = nullptr;
    {
      m_builder->SetInsertPoint(readThreadCountBlock);

      unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCountInWaves);

      // The dword following dwords for all waves stores the vertex count of the entire sub-group
      Value *ldsOffset = m_builder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
      vertCountInWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

      // NOTE: We promote vertex count in waves to SGPR since it is treated as an uniform value.
      vertCountInWaves = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInWaves);
      threadCountInWaves = vertCountInWaves;

      // Get vertex count for all waves prior to this wave
      ldsOffset = m_builder->CreateShl(m_nggFactor.waveIdInSubgroup, 2);
      ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

      vertCountInPrevWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

      auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);

      auto compactDataWrite = m_builder->CreateAnd(drawFlag, vertValid);

      m_builder->CreateCondBr(compactDataWrite, writeCompactDataBlock, endReadThreadCountBlock);
    }

    // Construct ".writeCompactData" block
    {
      m_builder->SetInsertPoint(writeCompactDataBlock);

      Value *drawMask = doSubgroupBallot(drawFlag);
      drawMask = m_builder->CreateBitCast(drawMask, VectorType::get(Type::getInt32Ty(*m_context), 2));

      auto drawMaskLow = m_builder->CreateExtractElement(drawMask, static_cast<uint64_t>(0));

      Value *compactThreadIdInSubrgoup =
          m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder->getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder->CreateExtractElement(drawMask, 1);

        compactThreadIdInSubrgoup =
            m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactThreadIdInSubrgoup});
      }

      compactThreadIdInSubrgoup = m_builder->CreateAdd(vertCountInPrevWaves, compactThreadIdInSubrgoup);

      // Write vertex position data to LDS
      for (const auto &expData : expDataSet) {
        if (expData.target == EXP_TARGET_POS_0) {
          const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);

          Value *ldsOffset = m_builder->CreateMul(compactThreadIdInSubrgoup, m_builder->getInt32(SizeOfVec4));
          ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

          m_ldsManager->writeValueToLds(expData.expValue, ldsOffset);

          break;
        }
      }

      // Write thread ID in sub-group to LDS
      Value *compactThreadId = m_builder->CreateTrunc(compactThreadIdInSubrgoup, m_builder->getInt8Ty());
      writePerThreadDataToLds(compactThreadId, m_nggFactor.threadIdInSubgroup, LdsRegionVertThreadIdMap);

      if (hasTs) {
        // Write X/Y of tessCoord (U/V) to LDS
        if (resUsage->builtInUsage.tes.tessCoord) {
          writePerThreadDataToLds(tessCoordX, compactThreadIdInSubrgoup, LdsRegionCompactTessCoordX);
          writePerThreadDataToLds(tessCoordY, compactThreadIdInSubrgoup, LdsRegionCompactTessCoordY);
        }

        // Write relative patch ID to LDS
        writePerThreadDataToLds(relPatchId, compactThreadIdInSubrgoup, LdsRegionCompactRelPatchId);

        // Write patch ID to LDS
        if (resUsage->builtInUsage.tes.primitiveId)
          writePerThreadDataToLds(patchId, compactThreadIdInSubrgoup, LdsRegionCompactPatchId);
      } else {
        // Write vertex ID to LDS
        if (resUsage->builtInUsage.vs.vertexIndex)
          writePerThreadDataToLds(vertexId, compactThreadIdInSubrgoup, LdsRegionCompactVertexId);

        // Write instance ID to LDS
        if (resUsage->builtInUsage.vs.instanceIndex)
          writePerThreadDataToLds(instanceId, compactThreadIdInSubrgoup, LdsRegionCompactInstanceId);

        // Write primitive ID to LDS
        if (resUsage->builtInUsage.vs.primitiveId) {
          assert(m_nggFactor.primitiveId);
          writePerThreadDataToLds(m_nggFactor.primitiveId, compactThreadIdInSubrgoup, LdsRegionCompactPrimId);
        }
      }

      m_builder->CreateBr(endReadThreadCountBlock);
    }

    // Construct ".endReadThreadCount" block
    {
      m_builder->SetInsertPoint(endReadThreadCountBlock);

      Value *hasSurviveVert = m_builder->CreateICmpNE(vertCountInWaves, m_builder->getInt32(0));

      Value *primCountInSubgroup = m_builder->CreateSelect(hasSurviveVert, m_nggFactor.primCountInSubgroup,
                                                           m_builder->getInt32(fullyCulledThreadCount));

      // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
      // as an uniform value later. This is similar to the provided primitive count in sub-group that is
      // a system value.
      primCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primCountInSubgroup);

      Value *vertCountInSubgroup =
          m_builder->CreateSelect(hasSurviveVert, vertCountInWaves, m_builder->getInt32(fullyCulledThreadCount));

      // NOTE: Here, we have to promote revised vertex count in sub-group to SGPR since it is treated as
      // an uniform value later, similar to what we have done for the revised primitive count in
      // sub-group.
      vertCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInSubgroup);

      m_nggFactor.primCountInSubgroup = primCountInSubgroup;
      m_nggFactor.vertCountInSubgroup = vertCountInSubgroup;

      auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));

      m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
    }

    // Construct ".allocReq" block
    {
      m_builder->SetInsertPoint(allocReqBlock);

      doParamCacheAllocRequest();
      m_builder->CreateBr(endAllocReqBlock);
    }

    // Construct ".endAllocReq" block
    {
      m_builder->SetInsertPoint(endAllocReqBlock);

      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

      auto noSurviveThread = m_builder->CreateICmpEQ(threadCountInWaves, m_builder->getInt32(0));
      m_builder->CreateCondBr(noSurviveThread, earlyExitBlock, noEarlyExitBlock);
    }

    // Construct ".earlyExit" block
    {
      m_builder->SetInsertPoint(earlyExitBlock);

      unsigned expPosCount = 0;
      for (const auto &expData : expDataSet) {
        if (expData.target >= EXP_TARGET_POS_0 && expData.target <= EXP_TARGET_POS_4)
          ++expPosCount;
      }

      doEarlyExit(fullyCulledThreadCount, expPosCount);
    }

    // Construct ".noEarlyExit" block
    {
      m_builder->SetInsertPoint(noEarlyExitBlock);

      auto primExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
      m_builder->CreateCondBr(primExp, expPrimBlock, endExpPrimBlock);
    }

    // Construct ".expPrim" block
    {
      m_builder->SetInsertPoint(expPrimBlock);

      doPrimitiveExportWithoutGs(cullFlag);
      m_builder->CreateBr(endExpPrimBlock);
    }

    // Construct ".endExpPrim" block
    Value *vertExp = nullptr;
    {
      m_builder->SetInsertPoint(endExpPrimBlock);

      vertExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
      m_builder->CreateCondBr(vertExp, expVertPosBlock, endExpVertPosBlock);
    }

    // Construct ".expVertPos" block
    {
      m_builder->SetInsertPoint(expVertPosBlock);

      // NOTE: We have to run ES to get exported data once again.
      expDataSet.clear();

      runEsOrEsVariant(module, lgcName::NggEsEntryVariant, entryPoint->arg_begin(), true, &expDataSet, expVertPosBlock);

      // For vertex position, we get the exported data from LDS
      for (auto &expData : expDataSet) {
        if (expData.target == EXP_TARGET_POS_0) {
          const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);
          assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation

          auto ldsOffset = m_builder->CreateMul(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(SizeOfVec4));
          ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

          // Use 128-bit LDS load
          auto expValue =
              m_ldsManager->readValueFromLds(VectorType::get(Type::getFloatTy(*m_context), 4), ldsOffset, true);
          expData.expValue = expValue;

          break;
        }
      }

      for (const auto &expData : expDataSet) {
        if (expData.target >= EXP_TARGET_POS_0 && expData.target <= EXP_TARGET_POS_4) {
          std::vector<Value *> args;

          args.push_back(m_builder->getInt32(expData.target));      // tgt
          args.push_back(m_builder->getInt32(expData.channelMask)); // en

          // src0 ~ src3
          for (unsigned i = 0; i < 4; ++i) {
            auto expValue = m_builder->CreateExtractElement(expData.expValue, i);
            args.push_back(expValue);
          }

          args.push_back(m_builder->getInt1(expData.doneFlag)); // done
          args.push_back(m_builder->getFalse());                // vm

          m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getFloatTy(), args);
        }
      }

      m_builder->CreateBr(endExpVertPosBlock);
    }

    // Construct ".endExpVertPos" block
    {
      m_builder->SetInsertPoint(endExpVertPosBlock);

      auto undef = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 4));
      for (auto &expData : expDataSet) {
        PHINode *expValue = m_builder->CreatePHI(VectorType::get(Type::getFloatTy(*m_context), 4), 2);

        expValue->addIncoming(expData.expValue, expVertPosBlock);
        expValue->addIncoming(undef, endExpPrimBlock);

        expData.expValue = expValue; // Update the exportd data
      }

      m_builder->CreateCondBr(vertExp, expVertParamBlock, endExpVertParamBlock);
    }

    // Construct ".expVertParam" block
    {
      m_builder->SetInsertPoint(expVertParamBlock);

      for (const auto &expData : expDataSet) {
        if (expData.target >= EXP_TARGET_PARAM_0 && expData.target <= EXP_TARGET_PARAM_31) {
          std::vector<Value *> args;

          args.push_back(m_builder->getInt32(expData.target));      // tgt
          args.push_back(m_builder->getInt32(expData.channelMask)); // en

          // src0 ~ src3
          for (unsigned i = 0; i < 4; ++i) {
            auto expValue = m_builder->CreateExtractElement(expData.expValue, i);
            args.push_back(expValue);
          }

          args.push_back(m_builder->getInt1(expData.doneFlag)); // done
          args.push_back(m_builder->getFalse());                // vm

          m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getFloatTy(), args);
        }
      }

      m_builder->CreateBr(endExpVertParamBlock);
    }

    // Construct ".endExpVertParam" block
    {
      m_builder->SetInsertPoint(endExpVertParamBlock);

      m_builder->CreateRetVoid();
    }
  }
}

// =====================================================================================================================
// Constructs primitive shader for ES-GS merged shader (GS is present).
//
// @param module : LLVM module
void NggPrimShader::constructPrimShaderWithGs(Module *module) {
  assert(m_hasGs);

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;
  const bool cullingMode = !m_nggControl->passthroughMode;

  auto entryPoint = module->getFunction(lgcName::NggPrimShaderEntryPoint);

  auto arg = entryPoint->arg_begin();

  Value *mergedGroupInfo = (arg + EsGsSysValueMergedGroupInfo);
  Value *mergedWaveInfo = (arg + EsGsSysValueMergedWaveInfo);
  Value *primShaderTableAddrLow = (arg + EsGsSysValuePrimShaderTableAddrLow);
  Value *primShaderTableAddrHigh = (arg + EsGsSysValuePrimShaderTableAddrHigh);

  arg += (EsGsSpecialSysValueCount + 1);

  Value *esGsOffsets01 = arg;
  Value *esGsOffsets23 = (arg + 1);
  Value *esGsOffsets45 = (arg + 4);

  //
  // The processing is something like this:
  //
  // NOTE: We purposely set primitive amplification to be max_vertices (treat line_strip and triangle_strip as point).
  // This will make primCountInSubgroup equal to vertCountInSubgroup to simplify the algorithm.
  //
  // NGG_GS() {
  //   Initialize thread/wave info
  //
  //   if (threadIdInWave < vertCountInWave)
  //     Run ES
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Initialize primitive connectivity data (0x80000000)
  //   if (threadIdInSubgroup < waveCount + 1)
  //     Initialize per-wave and per-subgroup count of output vertices
  //   Barrier
  //
  //   if (threadIdInWave < primCountInWave)
  //     Run GS
  //   Barrier
  //
  //   if (culling && valid primitive & threadIdInSubgroup < primCountInSubgroup) {
  //     Do culling (run culling algorithms)
  //     if (primitive culled)
  //       Nullify primitive connectivity data
  //   }
  //   Barrier
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Check draw flags of output vertices and compute draw mask
  //
  //   if (threadIdInWave < waveCount - waveId)
  //     Accumulate per-wave and per-subgroup count of output vertices
  //   Barrier
  //   Update vertCountInSubgroup
  //
  //   if (vertex compacted && vertex drawed)
  //     Compact vertex thread ID (map: compacted -> uncompacted)
  //
  //   if (waveId == 0)
  //     GS allocation request (GS_ALLOC_REQ)
  //   Barrier
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Do primitive connectivity data export
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Run copy shader
  // }
  //

  // Define basic blocks
  auto entryBlock = createBlock(entryPoint, ".entry");

  auto beginEsBlock = createBlock(entryPoint, ".beginEs");
  auto endEsBlock = createBlock(entryPoint, ".endEs");

  auto initOutPrimDataBlock = createBlock(entryPoint, ".initOutPrimData");
  auto endInitOutPrimDataBlock = createBlock(entryPoint, ".endInitOutPrimData");

  auto initOutVertCountBlock = createBlock(entryPoint, ".initOutVertCount");
  auto endInitOutVertCountBlock = createBlock(entryPoint, ".endInitOutVertCount");

  auto beginGsBlock = createBlock(entryPoint, ".beginGs");
  auto endGsBlock = createBlock(entryPoint, ".endGs");

  // Create blocks of culling only if culling is requested
  BasicBlock *cullingBlock = nullptr;
  BasicBlock *nullifyOutPrimDataBlock = nullptr;
  BasicBlock *endCullingBlock = nullptr;
  if (cullingMode) {
    cullingBlock = createBlock(entryPoint, ".culling");
    nullifyOutPrimDataBlock = createBlock(entryPoint, ".nullifyOutPrimData");
    endCullingBlock = createBlock(entryPoint, ".endCulling");
  }

  auto checkOutVertDrawFlagBlock = createBlock(entryPoint, ".checkOutVertDrawFlag");
  auto endCheckOutVertDrawFlagBlock = createBlock(entryPoint, ".endCheckOutVertDrawFlag");

  auto accumOutVertCountBlock = createBlock(entryPoint, ".accumOutVertCount");
  auto endAccumOutVertCountBlock = createBlock(entryPoint, ".endAccumOutVertCount");

  auto compactOutVertIdBlock = createBlock(entryPoint, ".compactOutVertId");
  auto endCompactOutVertIdBlock = createBlock(entryPoint, ".endCompactOutVertId");

  auto allocReqBlock = createBlock(entryPoint, ".allocReq");
  auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

  auto expPrimBlock = createBlock(entryPoint, ".expPrim");
  auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

  auto expVertBlock = createBlock(entryPoint, ".expVert");
  auto endExpVertBlock = createBlock(entryPoint, ".endExpVert");

  // Construct ".entry" block
  {
    m_builder->SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    // Record primitive shader table address info
    m_nggFactor.primShaderTableAddrLow = primShaderTableAddrLow;
    m_nggFactor.primShaderTableAddrHigh = primShaderTableAddrHigh;

    // Record ES-GS vertex offsets info
    m_nggFactor.esGsOffsets01 = esGsOffsets01;
    m_nggFactor.esGsOffsets23 = esGsOffsets23;
    m_nggFactor.esGsOffsets45 = esGsOffsets45;

    auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
    m_builder->CreateCondBr(vertValid, beginEsBlock, endEsBlock);
  }

  // Construct ".beginEs" block
  {
    m_builder->SetInsertPoint(beginEsBlock);

    runEsOrEsVariant(module, lgcName::NggEsEntryPoint, entryPoint->arg_begin(), false, nullptr, beginEsBlock);

    m_builder->CreateBr(endEsBlock);
  }

  // Construct ".endEs" block
  {
    m_builder->SetInsertPoint(endEsBlock);

    auto outPrimValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
    m_builder->CreateCondBr(outPrimValid, initOutPrimDataBlock, endInitOutPrimDataBlock);
  }

  // Construct ".initOutPrimData" block
  {
    m_builder->SetInsertPoint(initOutPrimDataBlock);

    writePerThreadDataToLds(m_builder->getInt32(NullPrim), m_nggFactor.threadIdInSubgroup, LdsRegionOutPrimData);

    m_builder->CreateBr(endInitOutPrimDataBlock);
  }

  // Construct ".endInitOutPrimData" block
  {
    m_builder->SetInsertPoint(endInitOutPrimDataBlock);

    auto waveValid =
        m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(waveCountInSubgroup + 1));
    m_builder->CreateCondBr(waveValid, initOutVertCountBlock, endInitOutVertCountBlock);
  }

  // Construct ".initOutVertCount" block
  {
    m_builder->SetInsertPoint(initOutVertCountBlock);

    writePerThreadDataToLds(m_builder->getInt32(0), m_nggFactor.threadIdInSubgroup, LdsRegionOutVertCountInWaves);

    m_builder->CreateBr(endInitOutVertCountBlock);
  }

  // Construct ".endInitOutVertCount" block
  {
    m_builder->SetInsertPoint(endInitOutVertCountBlock);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

    auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
    m_builder->CreateCondBr(primValid, beginGsBlock, endGsBlock);
  }

  // Construct ".beginGs" block
  {
    m_builder->SetInsertPoint(beginGsBlock);

    runGs(module, entryPoint->arg_begin());

    m_builder->CreateBr(endGsBlock);
  }

  // Construct ".endGs" block
  Value *primData = nullptr;
  {
    m_builder->SetInsertPoint(endGsBlock);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

    if (cullingMode) {
      // Do culling
      primData =
          readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionOutPrimData);
      auto doCull = m_builder->CreateICmpNE(primData, m_builder->getInt32(NullPrim));
      auto outPrimValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
      doCull = m_builder->CreateAnd(doCull, outPrimValid);
      m_builder->CreateCondBr(doCull, cullingBlock, endCullingBlock);
    } else {
      // No culling
      auto outVertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
      m_builder->CreateCondBr(outVertValid, checkOutVertDrawFlagBlock, endCheckOutVertDrawFlagBlock);
    }
  }

  // Construct culling blocks
  if (cullingMode) {
    // Construct ".culling" block
    {
      m_builder->SetInsertPoint(cullingBlock);

      assert(m_pipelineState->getShaderModes()->getGeometryShaderMode().outputPrimitive ==
             OutputPrimitives::TriangleStrip);

      // NOTE: primData[N] corresponds to the forming vertices <N, N+1, N+2> or <N, N+2, N+1>.
      Value *winding = m_builder->CreateICmpNE(primData, m_builder->getInt32(0));

      auto vertexId0 = m_nggFactor.threadIdInSubgroup;
      auto vertexId1 =
          m_builder->CreateAdd(m_nggFactor.threadIdInSubgroup,
                               m_builder->CreateSelect(winding, m_builder->getInt32(2), m_builder->getInt32(1)));
      auto vertexId2 =
          m_builder->CreateAdd(m_nggFactor.threadIdInSubgroup,
                               m_builder->CreateSelect(winding, m_builder->getInt32(1), m_builder->getInt32(2)));

      auto cullFlag = doCulling(module, vertexId0, vertexId1, vertexId2);
      m_builder->CreateCondBr(cullFlag, nullifyOutPrimDataBlock, endCullingBlock);
    }

    // Construct ".nullifyOutPrimData" block
    {
      m_builder->SetInsertPoint(nullifyOutPrimDataBlock);

      writePerThreadDataToLds(m_builder->getInt32(NullPrim), m_nggFactor.threadIdInSubgroup, LdsRegionOutPrimData);

      m_builder->CreateBr(endCullingBlock);
    }

    // Construct ".endCulling" block
    {
      m_builder->SetInsertPoint(endCullingBlock);

      m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

      auto outVertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
      m_builder->CreateCondBr(outVertValid, checkOutVertDrawFlagBlock, endCheckOutVertDrawFlagBlock);
    }
  }

  // Construct ".checkOutVertDrawFlag"
  Value *drawFlag = nullptr;
  {
    m_builder->SetInsertPoint(checkOutVertDrawFlagBlock);

    const unsigned outVertsPerPrim = getOutputVerticesPerPrimitive();

    // drawFlag = primData[N] != NullPrim
    auto primData0 =
        readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionOutPrimData);
    auto drawFlag0 = m_builder->CreateICmpNE(primData0, m_builder->getInt32(NullPrim));
    drawFlag = drawFlag0;

    if (outVertsPerPrim > 1) {
      // drawFlag |= N >= 1 ? (primData[N-1] != NullPrim) : false
      auto primData1 = readPerThreadDataFromLds(
          m_builder->getInt32Ty(), m_builder->CreateSub(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(1)),
          LdsRegionOutPrimData);
      auto drawFlag1 = m_builder->CreateSelect(
          m_builder->CreateICmpUGE(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(1)),
          m_builder->CreateICmpNE(primData1, m_builder->getInt32(NullPrim)), m_builder->getFalse());
      drawFlag = m_builder->CreateOr(drawFlag, drawFlag1);
    }

    if (outVertsPerPrim > 2) {
      // drawFlag |= N >= 2 ? (primData[N-2] != NullPrim) : false
      auto primData2 = readPerThreadDataFromLds(
          m_builder->getInt32Ty(), m_builder->CreateSub(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(2)),
          LdsRegionOutPrimData);
      auto drawFlag2 = m_builder->CreateSelect(
          m_builder->CreateICmpUGE(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(2)),
          m_builder->CreateICmpNE(primData2, m_builder->getInt32(NullPrim)), m_builder->getFalse());
      drawFlag = m_builder->CreateOr(drawFlag, drawFlag2);
    }

    m_builder->CreateBr(endCheckOutVertDrawFlagBlock);
  }

  // Construct ".endCheckOutVertDrawFlag"
  Value *drawMask = nullptr;
  Value *outVertCountInWave = nullptr;
  {
    m_builder->SetInsertPoint(endCheckOutVertDrawFlagBlock);

    auto drawFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    drawFlagPhi->addIncoming(drawFlag, checkOutVertDrawFlagBlock);
    // NOTE: The predecessors are different if culling mode is enabled.
    drawFlagPhi->addIncoming(m_builder->getFalse(), cullingMode ? endCullingBlock : endGsBlock);

    drawMask = doSubgroupBallot(drawFlagPhi);

    outVertCountInWave = m_builder->CreateIntrinsic(Intrinsic::ctpop, m_builder->getInt64Ty(), drawMask);
    outVertCountInWave = m_builder->CreateTrunc(outVertCountInWave, m_builder->getInt32Ty());

    auto threadIdUpbound = m_builder->CreateSub(m_builder->getInt32(waveCountInSubgroup), m_nggFactor.waveIdInSubgroup);
    auto threadValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, threadIdUpbound);

    m_builder->CreateCondBr(threadValid, accumOutVertCountBlock, endAccumOutVertCountBlock);
  }

  // Construct ".accumOutVertCount" block
  {
    m_builder->SetInsertPoint(accumOutVertCountBlock);

    auto ldsOffset = m_builder->CreateAdd(m_nggFactor.waveIdInSubgroup, m_nggFactor.threadIdInWave);
    ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(1));
    ldsOffset = m_builder->CreateShl(ldsOffset, 2);

    unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

    ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
    m_ldsManager->atomicOpWithLds(AtomicRMWInst::Add, outVertCountInWave, ldsOffset);

    m_builder->CreateBr(endAccumOutVertCountBlock);
  }

  // Construct ".endAccumOutVertCount" block
  Value *vertCompacted = nullptr;
  Value *vertCountInPrevWaves = nullptr;
  {
    m_builder->SetInsertPoint(endAccumOutVertCountBlock);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

    auto outVertCountInWaves =
        readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInWave, LdsRegionOutVertCountInWaves);

    // The last dword following dwords for all waves (each wave has one dword) stores GS output vertex count of the
    // entire sub-group
    auto vertCountInSubgroup = m_builder->CreateIntrinsic(
        Intrinsic::amdgcn_readlane, {}, {outVertCountInWaves, m_builder->getInt32(waveCountInSubgroup)});

    // Get output vertex count for all waves prior to this wave
    vertCountInPrevWaves =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, {outVertCountInWaves, m_nggFactor.waveIdInSubgroup});

    vertCompacted = m_builder->CreateICmpULT(vertCountInSubgroup, m_nggFactor.vertCountInSubgroup);
    m_builder->CreateCondBr(m_builder->CreateAnd(drawFlag, vertCompacted), compactOutVertIdBlock,
                            endCompactOutVertIdBlock);

    m_nggFactor.vertCountInSubgroup = vertCountInSubgroup; // Update GS output vertex count in sub-group
  }

  // Construct ".compactOutVertId" block
  Value *compactVertexId = nullptr;
  {
    m_builder->SetInsertPoint(compactOutVertIdBlock);

    auto drawMaskVec = m_builder->CreateBitCast(drawMask, VectorType::get(Type::getInt32Ty(*m_context), 2));

    auto drawMaskLow = m_builder->CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
    compactVertexId = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder->getInt32(0)});

    if (waveSize == 64) {
      auto drawMaskHigh = m_builder->CreateExtractElement(drawMaskVec, 1);
      compactVertexId = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactVertexId});
    }

    compactVertexId = m_builder->CreateAdd(vertCountInPrevWaves, compactVertexId);
    writePerThreadDataToLds(m_builder->CreateTrunc(m_nggFactor.threadIdInSubgroup, m_builder->getInt32Ty()),
                            compactVertexId, LdsRegionOutVertThreadIdMap);

    m_builder->CreateBr(endCompactOutVertIdBlock);
  }

  // Construct ".endCompactOutVertId" block
  {
    m_builder->SetInsertPoint(endCompactOutVertIdBlock);

    auto compactVertexIdPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
    compactVertexIdPhi->addIncoming(compactVertexId, compactOutVertIdBlock);
    compactVertexIdPhi->addIncoming(m_nggFactor.threadIdInSubgroup, endAccumOutVertCountBlock);
    compactVertexId = compactVertexIdPhi;

    auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
    m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
  }

  // Construct ".allocReq" block
  {
    m_builder->SetInsertPoint(allocReqBlock);

    doParamCacheAllocRequest();
    m_builder->CreateBr(endAllocReqBlock);
  }

  // Construct ".endAllocReq" block
  {
    m_builder->SetInsertPoint(endAllocReqBlock);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

    auto primExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
    m_builder->CreateCondBr(primExp, expPrimBlock, endExpPrimBlock);
  }

  // Construct ".expPrim" block
  {
    m_builder->SetInsertPoint(expPrimBlock);

    doPrimitiveExportWithGs(compactVertexId);
    m_builder->CreateBr(endExpPrimBlock);
  }

  // Construct ".endExpPrim" block
  {
    m_builder->SetInsertPoint(endExpPrimBlock);

    auto pVertExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
    m_builder->CreateCondBr(pVertExp, expVertBlock, endExpVertBlock);
  }

  // Construct ".expVert" block
  {
    m_builder->SetInsertPoint(expVertBlock);

    runCopyShader(module, vertCompacted);
    m_builder->CreateBr(endExpVertBlock);
  }

  // Construct ".endExpVert" block
  {
    m_builder->SetInsertPoint(endExpVertBlock);

    m_builder->CreateRetVoid();
  }
}

// =====================================================================================================================
// Extracts merged group/wave info and initializes part of NGG calculation factors.
//
// NOTE: This function must be invoked by the entry block of NGG shader module.
//
// @param mergedGroupInfo : Merged group info
// @param mergedWaveInfo : Merged wave info
void NggPrimShader::initWaveThreadInfo(Value *mergedGroupInfo, Value *mergedWaveInfo) {
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  m_builder->CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder->getInt64(-1));

  auto threadIdInWave =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder->getInt32(-1), m_builder->getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder->getInt32(-1), threadIdInWave});
  }

  auto primCountInSubgroup =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                 {mergedGroupInfo, m_builder->getInt32(22), m_builder->getInt32(9)});

  auto vertCountInSubgroup =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                 {mergedGroupInfo, m_builder->getInt32(12), m_builder->getInt32(9)});

  auto vertCountInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {mergedWaveInfo, m_builder->getInt32(0), m_builder->getInt32(8)});

  auto primCountInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                    {mergedWaveInfo, m_builder->getInt32(8), m_builder->getInt32(8)});

  auto waveIdInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                     {mergedWaveInfo, m_builder->getInt32(24), m_builder->getInt32(4)});

  auto threadIdInSubgroup = m_builder->CreateMul(waveIdInSubgroup, m_builder->getInt32(waveSize));
  threadIdInSubgroup = m_builder->CreateAdd(threadIdInSubgroup, threadIdInWave);

  primCountInSubgroup->setName("primCountInSubgroup");
  vertCountInSubgroup->setName("vertCountInSubgroup");
  primCountInWave->setName("primCountInWave");
  vertCountInWave->setName("vertCountInWave");
  threadIdInWave->setName("threadIdInWave");
  threadIdInSubgroup->setName("threadIdInSubgroup");
  waveIdInSubgroup->setName("waveIdInSubgroup");

  // Record wave/thread info
  m_nggFactor.primCountInSubgroup = primCountInSubgroup;
  m_nggFactor.vertCountInSubgroup = vertCountInSubgroup;
  m_nggFactor.primCountInWave = primCountInWave;
  m_nggFactor.vertCountInWave = vertCountInWave;
  m_nggFactor.threadIdInWave = threadIdInWave;
  m_nggFactor.threadIdInSubgroup = threadIdInSubgroup;
  m_nggFactor.waveIdInSubgroup = waveIdInSubgroup;

  m_nggFactor.mergedGroupInfo = mergedGroupInfo;
}

// =====================================================================================================================
// Does various culling for NGG primitive shader.
//
// @param module : LLVM module
// @param vertexId0: ID of vertex0 (forming this triangle)
// @param vertexId1: ID of vertex1 (forming this triangle)
// @param vertexId2: ID of vertex2 (forming this triangle)
Value *NggPrimShader::doCulling(Module *module, Value *vertexId0, Value *vertexId1, Value *vertexId2) {
  // Skip following culling if it is not requested
  if (!enableCulling())
    return m_builder->getFalse();

  Value *cullFlag = m_builder->getFalse();

  Value *vertex0 = fetchVertexPositionData(vertexId0);
  Value *vertex1 = fetchVertexPositionData(vertexId1);
  Value *vertex2 = fetchVertexPositionData(vertexId2);

  // Handle backface culling
  if (m_nggControl->enableBackfaceCulling)
    cullFlag = doBackfaceCulling(module, cullFlag, vertex0, vertex1, vertex2);

  // Handle frustum culling
  if (m_nggControl->enableFrustumCulling)
    cullFlag = doFrustumCulling(module, cullFlag, vertex0, vertex1, vertex2);

  // Handle box filter culling
  if (m_nggControl->enableBoxFilterCulling)
    cullFlag = doBoxFilterCulling(module, cullFlag, vertex0, vertex1, vertex2);

  // Handle sphere culling
  if (m_nggControl->enableSphereCulling)
    cullFlag = doSphereCulling(module, cullFlag, vertex0, vertex1, vertex2);

  // Handle small primitive filter culling
  if (m_nggControl->enableSmallPrimFilter)
    cullFlag = doSmallPrimFilterCulling(module, cullFlag, vertex0, vertex1, vertex2);

  // Handle cull distance culling
  if (m_nggControl->enableCullDistanceCulling) {
    Value *signMask0 = fetchCullDistanceSignMask(vertexId0);
    Value *signMask1 = fetchCullDistanceSignMask(vertexId1);
    Value *signMask2 = fetchCullDistanceSignMask(vertexId2);
    cullFlag = doCullDistanceCulling(module, cullFlag, signMask0, signMask1, signMask2);
  }

  return cullFlag;
}

// =====================================================================================================================
// Requests that parameter cache space be allocated (send the message GS_ALLOC_REQ).
void NggPrimShader::doParamCacheAllocRequest() {
  // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
  Value *m0 = m_builder->CreateShl(m_nggFactor.primCountInSubgroup, 12);
  m0 = m_builder->CreateOr(m0, m_nggFactor.vertCountInSubgroup);

  m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder->getInt32(GsAllocReq), m0});
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader (GS is not present).
//
// @param cullFlag : Cull flag indicating whether this primitive has been culled (could be null)
void NggPrimShader::doPrimitiveExportWithoutGs(Value *cullFlag) {
  Value *primData = nullptr;

  // Primitive data layout [31:0]
  //   [31]    = null primitive flag
  //   [28:20] = vertexId2 (in bytes)
  //   [18:10] = vertexId1 (in bytes)
  //   [8:0]   = vertexId0 (in bytes)

  if (m_nggControl->passthroughMode) {
    // Pass-through mode (primitive data has been constructed)
    primData = m_nggFactor.esGsOffsets01;
  } else {
    // Non pass-through mode (primitive data has to be constructed)
    auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                  {
                                                      m_nggFactor.esGsOffsets01,
                                                      m_builder->getInt32(0),
                                                      m_builder->getInt32(16),
                                                  });
    Value *vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

    auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                  {
                                                      m_nggFactor.esGsOffsets01,
                                                      m_builder->getInt32(16),
                                                      m_builder->getInt32(16),
                                                  });
    Value *vertexId1 = m_builder->CreateLShr(esGsOffset1, 2);

    auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                  {
                                                      m_nggFactor.esGsOffsets23,
                                                      m_builder->getInt32(0),
                                                      m_builder->getInt32(16),
                                                  });
    Value *vertexId2 = m_builder->CreateLShr(esGsOffset2, 2);

    // NOTE: If the current vertex count in sub-group is less than the original value, then there must be
    // vertex culling. When vertex culling occurs, the vertex IDs should be fetched from LDS (compacted).
    auto vertCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                          {
                                                              m_nggFactor.mergedGroupInfo,
                                                              m_builder->getInt32(12),
                                                              m_builder->getInt32(9),
                                                          });
    auto vertCulled = m_builder->CreateICmpULT(m_nggFactor.vertCountInSubgroup, vertCountInSubgroup);

    auto expPrimBlock = m_builder->GetInsertBlock();

    auto readCompactIdBlock = createBlock(expPrimBlock->getParent(), "readCompactId");
    readCompactIdBlock->moveAfter(expPrimBlock);

    auto expPrimContBlock = createBlock(expPrimBlock->getParent(), "expPrimCont");
    expPrimContBlock->moveAfter(readCompactIdBlock);

    m_builder->CreateCondBr(vertCulled, readCompactIdBlock, expPrimContBlock);

    // Construct ".readCompactId" block
    Value *compactVertexId0 = nullptr;
    Value *compactVertexId1 = nullptr;
    Value *compactVertexId2 = nullptr;
    {
      m_builder->SetInsertPoint(readCompactIdBlock);

      compactVertexId0 = readPerThreadDataFromLds(m_builder->getInt8Ty(), vertexId0, LdsRegionVertThreadIdMap);
      compactVertexId0 = m_builder->CreateZExt(compactVertexId0, m_builder->getInt32Ty());

      compactVertexId1 = readPerThreadDataFromLds(m_builder->getInt8Ty(), vertexId1, LdsRegionVertThreadIdMap);
      compactVertexId1 = m_builder->CreateZExt(compactVertexId1, m_builder->getInt32Ty());

      compactVertexId2 = readPerThreadDataFromLds(m_builder->getInt8Ty(), vertexId2, LdsRegionVertThreadIdMap);
      compactVertexId2 = m_builder->CreateZExt(compactVertexId2, m_builder->getInt32Ty());

      m_builder->CreateBr(expPrimContBlock);
    }

    // Construct part of ".expPrimCont" block (phi nodes)
    {
      m_builder->SetInsertPoint(expPrimContBlock);

      auto vertexId0Phi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
      vertexId0Phi->addIncoming(compactVertexId0, readCompactIdBlock);
      vertexId0Phi->addIncoming(vertexId0, expPrimBlock);

      auto vertexId1Phi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
      vertexId1Phi->addIncoming(compactVertexId1, readCompactIdBlock);
      vertexId1Phi->addIncoming(vertexId1, expPrimBlock);

      auto vertexId2Phi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
      vertexId2Phi->addIncoming(compactVertexId2, readCompactIdBlock);
      vertexId2Phi->addIncoming(vertexId2, expPrimBlock);

      vertexId0 = vertexId0Phi;
      vertexId1 = vertexId1Phi;
      vertexId2 = vertexId2Phi;
    }

    primData = m_builder->CreateShl(vertexId2, 10);
    primData = m_builder->CreateOr(primData, vertexId1);

    primData = m_builder->CreateShl(primData, 10);
    primData = m_builder->CreateOr(primData, vertexId0);

    assert(cullFlag); // Must not be null
    const auto nullPrimVal = m_builder->getInt32(NullPrim);
    primData = m_builder->CreateSelect(cullFlag, nullPrimVal, primData);
  }

  auto undef = UndefValue::get(m_builder->getInt32Ty());

  m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getInt32Ty(),
                             {
                                 m_builder->getInt32(EXP_TARGET_PRIM), // tgt
                                 m_builder->getInt32(0x1),             // en
                                 // src0 ~ src3
                                 primData, undef, undef, undef,
                                 m_builder->getTrue(),  // done, must be set
                                 m_builder->getFalse(), // vm
                             });
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader (GS is present).
//
// @param vertexId : The ID of starting vertex (IDs of vertices forming a GS primitive must be consecutive)
void NggPrimShader::doPrimitiveExportWithGs(Value *vertexId) {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   primData = Read primitive data from LDS
  //   if (valid primitive) {
  //     if (points)
  //       primData = vertexId
  //     else if (line_strip) {
  //       primData = ((vertexId + 1) << 10) | vertexId
  //     } else if (triangle_strip) {
  //       winding = primData != 0
  //       primData = winding ? (((vertexId + 1) << 20) | ((vertexId + 2) << 10) | vertexId)
  //                          : (((vertexId + 2) << 20) | ((vertexId + 1) << 10) | vertexId)
  //     }
  //   }
  //   Export primitive data
  //
  //
  // Primitive data layout [31:0]
  //   [31]    = null primitive flag
  //   [28:20] = vertexId2 (in bytes)
  //   [18:10] = vertexId1 (in bytes)
  //   [8:0]   = vertexId0 (in bytes)
  //
  Value *primData =
      readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionOutPrimData);

  auto primValid = m_builder->CreateICmpNE(primData, m_builder->getInt32(NullPrim));

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

  Value *newPrimData = nullptr;
  switch (geometryMode.outputPrimitive) {
  case OutputPrimitives::Points:
    newPrimData = vertexId;
    break;
  case OutputPrimitives::LineStrip: {
    Value *vertexId0 = vertexId;
    Value *vertexId1 = m_builder->CreateAdd(vertexId, m_builder->getInt32(1));
    newPrimData = m_builder->CreateOr(m_builder->CreateShl(vertexId1, 10), vertexId0);
    break;
  }
  case OutputPrimitives::TriangleStrip: {
    Value *winding = m_builder->CreateICmpNE(primData, m_builder->getInt32(0));
    Value *vertexId0 = vertexId;
    Value *vertexId1 = m_builder->CreateAdd(vertexId, m_builder->getInt32(1));
    Value *vertexId2 = m_builder->CreateAdd(vertexId, m_builder->getInt32(2));
    auto newPrimDataNoWinding = m_builder->CreateOr(
        m_builder->CreateShl(m_builder->CreateOr(m_builder->CreateShl(vertexId2, 10), vertexId1), 10), vertexId0);
    auto newPrimDataWinding = m_builder->CreateOr(
        m_builder->CreateShl(m_builder->CreateOr(m_builder->CreateShl(vertexId1, 10), vertexId2), 10), vertexId0);
    newPrimData = m_builder->CreateSelect(winding, newPrimDataWinding, newPrimDataNoWinding);
    break;
  }
  default:
    llvm_unreachable("Unexpected output primitive type!");
    break;
  }

  primData = m_builder->CreateSelect(primValid, newPrimData, primData);

  auto pUndef = UndefValue::get(m_builder->getInt32Ty());

  m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getInt32Ty(),
                             {
                                 m_builder->getInt32(EXP_TARGET_PRIM), // tgt
                                 m_builder->getInt32(0x1),             // en
                                 primData, pUndef, pUndef, pUndef,     // src0 ~ src3
                                 m_builder->getTrue(),                 // done, must be set
                                 m_builder->getFalse(),                // vm
                             });
}

// =====================================================================================================================
// Early exit NGG primitive shader when we detect that the entire sub-group is fully culled, doing dummy
// primitive/vertex export if necessary.
//
// @param fullyCulledThreadCount : Thread count left when the entire sub-group is fully culled
// @param expPosCount : Position export count
void NggPrimShader::doEarlyExit(unsigned fullyCulledThreadCount, unsigned expPosCount) {
  if (fullyCulledThreadCount > 0) {
    assert(fullyCulledThreadCount == 1); // Currently, if workarounded, this is set to 1

    auto earlyExitBlock = m_builder->GetInsertBlock();

    auto dummyExpBlock = createBlock(earlyExitBlock->getParent(), ".dummyExp");
    dummyExpBlock->moveAfter(earlyExitBlock);

    auto endDummyExpBlock = createBlock(earlyExitBlock->getParent(), ".endDummyExp");
    endDummyExpBlock->moveAfter(dummyExpBlock);

    // Continue to construct ".earlyExit" block
    {
      auto firstThreadInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
      m_builder->CreateCondBr(firstThreadInSubgroup, dummyExpBlock, endDummyExpBlock);
    }

    // Construct ".dummyExp" block
    {
      m_builder->SetInsertPoint(dummyExpBlock);

      auto undef = UndefValue::get(m_builder->getInt32Ty());

      m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getInt32Ty(),
                                 {
                                     m_builder->getInt32(EXP_TARGET_PRIM), // tgt
                                     m_builder->getInt32(0x1),             // en
                                     // src0 ~ src3
                                     m_builder->getInt32(0), undef, undef, undef,
                                     m_builder->getTrue(), // done
                                     m_builder->getFalse() // vm
                                 });

      undef = UndefValue::get(m_builder->getFloatTy());

      for (unsigned i = 0; i < expPosCount; ++i) {
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getFloatTy(),
                                   {
                                       m_builder->getInt32(EXP_TARGET_POS_0 + i), // tgt
                                       m_builder->getInt32(0x0),                  // en
                                       // src0 ~ src3
                                       undef, undef, undef, undef,
                                       m_builder->getInt1(i == expPosCount - 1), // done
                                       m_builder->getFalse()                     // vm
                                   });
      }

      m_builder->CreateBr(endDummyExpBlock);
    }

    // Construct ".endDummyExp" block
    {
      m_builder->SetInsertPoint(endDummyExpBlock);
      m_builder->CreateRetVoid();
    }
  } else
    m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Runs ES or ES variant (to get exported data).
//
// NOTE: The ES variant is derived from original ES main function with some additional special handling added to the
// function body and also mutates its return type.
//
// @param module : LLVM module
// @param entryName : ES entry name
// @param sysValueStart : Start of system value
// @param sysValueFromLds : Whether some system values are loaded from LDS (for vertex compaction)
// @param [out] expDataSet : Set of exported data (could be null)
// @param insertAtEnd : Where to insert instructions
void NggPrimShader::runEsOrEsVariant(Module *module, StringRef entryName, Argument *sysValueStart, bool sysValueFromLds,
                                     std::vector<ExpData> *expDataSet, BasicBlock *insertAtEnd) {
  const bool hasTs = (m_hasTcs || m_hasTes);
  if (((hasTs && m_hasTes) || (!hasTs && m_hasVs)) == false) {
    // No TES (tessellation is enabled) or VS (tessellation is disabled), don't have to run
    return;
  }

  const bool runEsVariant = (entryName != lgcName::NggEsEntryPoint);

  Function *esEntry = nullptr;
  if (runEsVariant) {
    assert(expDataSet);
    esEntry = mutateEsToVariant(module, entryName, *expDataSet); // Mutate ES to variant

    if (!esEntry) {
      // ES variant is NULL, don't have to run
      return;
    }
  } else {
    esEntry = module->getFunction(lgcName::NggEsEntryPoint);
    assert(esEntry);
  }

  // Call ES entry
  Argument *arg = sysValueStart;

  Value *esGsOffset = nullptr;
  if (m_hasGs) {
    auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
    unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    unsigned esGsBytesPerWave = waveSize * 4 * calcFactor.esGsRingItemSize;
    esGsOffset = m_builder->CreateMul(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(esGsBytesPerWave));
  }

  Value *offChipLdsBase = (arg + EsGsSysValueOffChipLdsBase);
  Value *isOffChip = UndefValue::get(m_builder->getInt32Ty()); // NOTE: This flag is unused.

  arg += EsGsSpecialSysValueCount;

  Value *userData = arg++;

  // Initialize those system values to undefined ones
  Value *tessCoordX = UndefValue::get(m_builder->getFloatTy());
  Value *tessCoordY = UndefValue::get(m_builder->getFloatTy());
  Value *relPatchId = UndefValue::get(m_builder->getInt32Ty());
  Value *patchId = UndefValue::get(m_builder->getInt32Ty());

  Value *vertexId = UndefValue::get(m_builder->getInt32Ty());
  Value *relVertexId = UndefValue::get(m_builder->getInt32Ty());
  Value *vsPrimitiveId = UndefValue::get(m_builder->getInt32Ty());
  Value *instanceId = UndefValue::get(m_builder->getInt32Ty());

  if (sysValueFromLds) {
    // NOTE: For vertex compaction, system values are from LDS compaction data region rather than from VGPRs.
    assert(m_nggControl->compactMode == NggCompactVertices);

    const auto resUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

    if (hasTs) {
      if (resUsage->builtInUsage.tes.tessCoord) {
        tessCoordX = readPerThreadDataFromLds(m_builder->getFloatTy(), m_nggFactor.threadIdInSubgroup,
                                              LdsRegionCompactTessCoordX);

        tessCoordY = readPerThreadDataFromLds(m_builder->getFloatTy(), m_nggFactor.threadIdInSubgroup,
                                              LdsRegionCompactTessCoordY);
      }

      relPatchId =
          readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionCompactRelPatchId);

      if (resUsage->builtInUsage.tes.primitiveId) {
        patchId =
            readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionCompactPatchId);
      }
    } else {
      if (resUsage->builtInUsage.vs.vertexIndex) {
        vertexId =
            readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionCompactVertexId);
      }

      // NOTE: Relative vertex ID Will not be used when VS is merged to GS.

      if (resUsage->builtInUsage.vs.primitiveId) {
        vsPrimitiveId =
            readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionCompactPrimId);
      }

      if (resUsage->builtInUsage.vs.instanceIndex) {
        instanceId = readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup,
                                              LdsRegionCompactInstanceId);
      }
    }
  } else {
    tessCoordX = (arg + 5);
    tessCoordY = (arg + 6);
    relPatchId = (arg + 7);
    patchId = (arg + 8);

    vertexId = (arg + 5);
    relVertexId = (arg + 6);
    // NOTE: VS primitive ID for NGG is specially obtained, not simply from system VGPR.
    if (m_nggFactor.primitiveId)
      vsPrimitiveId = m_nggFactor.primitiveId;
    instanceId = (arg + 8);
  }

  std::vector<Value *> args;

  auto intfData = m_pipelineState->getShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
  const unsigned userDataCount = intfData->userDataCount;

  unsigned userDataIdx = 0;

  auto esArgBegin = esEntry->arg_begin();
  const unsigned esArgCount = esEntry->arg_size();
  (void(esArgCount)); // unused

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(args.size() < esArgCount);

    auto esArg = (esArgBegin + args.size());
    assert(esArg->hasAttribute(Attribute::InReg));

    auto esArgTy = esArg->getType();
    if (esArgTy->isVectorTy()) {
      assert(cast<VectorType>(esArgTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<VectorType>(esArgTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto esUserData = m_builder->CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(esUserData);
    } else {
      assert(esArgTy->isIntegerTy());

      auto esUserData = m_builder->CreateExtractElement(userData, userDataIdx);
      args.push_back(esUserData);
      ++userDataIdx;
    }
  }

  if (hasTs) {
    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      args.push_back(m_hasGs ? offChipLdsBase : isOffChip);
      args.push_back(m_hasGs ? isOffChip : offChipLdsBase);
    }

    if (m_hasGs)
      args.push_back(esGsOffset);

    // Set up system value VGPRs
    args.push_back(tessCoordX);
    args.push_back(tessCoordY);
    args.push_back(relPatchId);
    args.push_back(patchId);
  } else {
    // Set up system value SGPRs
    if (m_hasGs)
      args.push_back(esGsOffset);

    // Set up system value VGPRs
    args.push_back(vertexId);
    args.push_back(relVertexId);
    args.push_back(vsPrimitiveId);
    args.push_back(instanceId);
  }

  // If the ES is the API VS, and it is a fetchless VS, then we need to add args for the vertex fetches.
  // Also set the name of each vertex fetch prim shader arg while we're here.
  if (!hasTs) {
    unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
    if (vertexFetchCount != 0) {
      // The final vertexFetchCount args of the prim shader and of ES (API VS) are the vertex fetches.
      Function *primShader = insertAtEnd->getParent();
      unsigned primArgSize = primShader->arg_size();
      Function *esEntry = module->getFunction(lgcName::NggEsEntryPoint);
      unsigned esArgSize = esEntry->arg_size();
      for (unsigned idx = 0; idx != vertexFetchCount; ++idx) {
        Argument *arg = primShader->getArg(primArgSize - vertexFetchCount + idx);
        arg->setName(esEntry->getArg(esArgSize - vertexFetchCount + idx)->getName());
        args.push_back(arg);
      }
    }
  }

  assert(args.size() == esArgCount); // Must have visit all arguments of ES entry point

  if (runEsVariant) {
    auto expData = emitCall(entryName, esEntry->getReturnType(), args, {}, insertAtEnd);

    // Re-construct exported data from the return value
    auto expDataTy = expData->getType();
    assert(expDataTy->isArrayTy());

    const unsigned expCount = expDataTy->getArrayNumElements();
    for (unsigned i = 0; i < expCount; ++i) {
      Value *expValue = m_builder->CreateExtractValue(expData, i);
      (*expDataSet)[i].expValue = expValue;
    }
  } else {
    emitCall(entryName, esEntry->getReturnType(), args, {}, insertAtEnd);
  }
}

// =====================================================================================================================
// Mutates the entry-point (".main") of ES to its variant (".variant").
//
// NOTE: Initially, the return type of ES entry-point is void. After this mutation, position and parameter exporting
// are both removed. Instead, the exported values are returned via either a new entry-point (combined) or two new
// entry-points (separate). Return types is something like this:
//   .variant:       [ POS0: <4 x float>, POS1: <4 x float>, ..., PARAM0: <4 x float>, PARAM1: <4 x float>, ... ]
//   .variant.pos:   [ POS0: <4 x float>, POS1: <4 x float>, ... ]
//   .variant.param: [ PARAM0: <4 x float>, PARAM1: <4 x float>, ... ]
//
// @param module : LLVM module
// @param entryName : ES entry name
// @param [out] expDataSet : Set of exported data
Function *NggPrimShader::mutateEsToVariant(Module *module, StringRef entryName, std::vector<ExpData> &expDataSet) {
  assert(m_hasGs == false); // GS must not be present
  assert(expDataSet.empty());

  const auto esEntryPoint = module->getFunction(lgcName::NggEsEntryPoint);
  assert(esEntryPoint);

  const bool doExp = (entryName == lgcName::NggEsEntryVariant);
  const bool doPosExp = (entryName == lgcName::NggEsEntryVariantPos);
  const bool doParamExp = (entryName == lgcName::NggEsEntryVariantParam);

  // Calculate export count
  unsigned expCount = 0;

  for (auto &func : module->functions()) {
    if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp) {
      for (auto user : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(user);
        assert(call);

        if (call->getParent()->getParent() != esEntryPoint) {
          // Export call doesn't belong to ES, skip
          continue;
        }

        uint8_t expTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();

        bool expPos = (expTarget >= EXP_TARGET_POS_0 && expTarget <= EXP_TARGET_POS_4);
        bool expParam = (expTarget >= EXP_TARGET_PARAM_0 && expTarget <= EXP_TARGET_PARAM_31);

        if ((doExp && (expPos || expParam)) || (doPosExp && expPos) || (doParamExp && expParam))
          ++expCount;
      }
    }
  }

  if (expCount == 0) {
    // If the export count is zero, return NULL
    return nullptr;
  }

  // Clone new entry-point
  auto expDataTy = ArrayType::get(VectorType::get(Type::getFloatTy(*m_context), 4), expCount);
  Value *expData = UndefValue::get(expDataTy);

  auto esEntryVariantTy = FunctionType::get(expDataTy, esEntryPoint->getFunctionType()->params(), false);
  auto esEntryVariant = Function::Create(esEntryVariantTy, esEntryPoint->getLinkage(), "", module);
  esEntryVariant->copyAttributesFrom(esEntryPoint);

  ValueToValueMapTy valueMap;

  Argument *variantArg = esEntryVariant->arg_begin();
  for (Argument &arg : esEntryPoint->args())
    valueMap[&arg] = variantArg++;

  SmallVector<ReturnInst *, 8> retInsts;
  CloneFunctionInto(esEntryVariant, esEntryPoint, valueMap, false, retInsts);

  esEntryVariant->setName(entryName);

  auto savedInsertPos = m_builder->saveIP();

  // Find the return block and remove old return instruction
  BasicBlock *retBlock = nullptr;
  for (BasicBlock &block : *esEntryVariant) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      retInst->dropAllReferences();
      retInst->eraseFromParent();

      retBlock = &block;
      break;
    }
  }

  m_builder->SetInsertPoint(retBlock);

  // Get exported data
  std::vector<Instruction *> expCalls;

  unsigned lastExport = InvalidValue; // Record last position export that needs "done" flag
  for (auto &func : module->functions()) {
    if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp) {
      for (auto user : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(user);
        assert(call);

        if (call->getParent()->getParent() != esEntryVariant) {
          // Export call doesn't belong to ES variant, skip
          continue;
        }

        assert(call->getParent() == retBlock); // Must in return block

        uint8_t expTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();

        bool expPos = (expTarget >= EXP_TARGET_POS_0 && expTarget <= EXP_TARGET_POS_4);
        bool expParam = (expTarget >= EXP_TARGET_PARAM_0 && expTarget <= EXP_TARGET_PARAM_31);

        if ((doExp && (expPos || expParam)) || (doPosExp && expPos) || (doParamExp && expParam)) {
          uint8_t channelMask = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();

          Value *expValues[4] = {};
          expValues[0] = call->getArgOperand(2);
          expValues[1] = call->getArgOperand(3);
          expValues[2] = call->getArgOperand(4);
          expValues[3] = call->getArgOperand(5);

          if (func.getName().endswith(".i32")) {
            expValues[0] = m_builder->CreateBitCast(expValues[0], m_builder->getFloatTy());
            expValues[1] = m_builder->CreateBitCast(expValues[1], m_builder->getFloatTy());
            expValues[2] = m_builder->CreateBitCast(expValues[2], m_builder->getFloatTy());
            expValues[3] = m_builder->CreateBitCast(expValues[3], m_builder->getFloatTy());
          }

          Value *expValue = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 4));
          for (unsigned i = 0; i < 4; ++i)
            expValue = m_builder->CreateInsertElement(expValue, expValues[i], i);

          if (expPos) {
            // Last position export that needs "done" flag
            lastExport = expDataSet.size();
          }

          ExpData expData = {expTarget, channelMask, false, expValue};
          expDataSet.push_back(expData);
        }

        expCalls.push_back(call);
      }
    }
  }
  assert(expDataSet.size() == expCount);

  // Set "done" flag for last position export
  if (lastExport != InvalidValue)
    expDataSet[lastExport].doneFlag = true;

  // Construct exported data
  unsigned i = 0;
  for (auto &expDataElement : expDataSet) {
    expData = m_builder->CreateInsertValue(expData, expDataElement.expValue, i++);
    expDataElement.expValue = nullptr;
  }

  // Insert new "return" instruction
  m_builder->CreateRet(expData);

  // Clear export calls
  for (auto expCall : expCalls) {
    expCall->dropAllReferences();
    expCall->eraseFromParent();
  }

  m_builder->restoreIP(savedInsertPos);

  return esEntryVariant;
}

// =====================================================================================================================
// Runs GS.
//
// @param module : LLVM module
// @param sysValueStart : Start of system value
void NggPrimShader::runGs(Module *module, Argument *sysValueStart) {
  assert(m_hasGs); // GS must be present

  Function *gsEntry = mutateGs(module);

  // Call GS entry
  Argument *arg = sysValueStart;

  Value *gsVsOffset = UndefValue::get(m_builder->getInt32Ty()); // NOTE: For NGG, GS-VS offset is unused

  // NOTE: This argument is expected to be GS wave ID, not wave ID in sub-group, for normal ES-GS merged shader.
  // However, in NGG mode, GS wave ID, sent to GS_EMIT and GS_CUT messages, is no longer required because of NGG
  // handling of such messages. Instead, wave ID in sub-group is required as the substitue.
  auto waveId = m_nggFactor.waveIdInSubgroup;

  arg += EsGsSpecialSysValueCount;

  Value *userData = arg++;

  Value *esGsOffsets01 = arg;
  Value *esGsOffsets23 = (arg + 1);
  Value *gsPrimitiveId = (arg + 2);
  Value *invocationId = (arg + 3);
  Value *esGsOffsets45 = (arg + 4);

  // NOTE: For NGG, GS invocation ID is stored in lowest 8 bits ([7:0]) and other higher bits are used for other
  // purposes according to GE-SPI interface.
  invocationId = m_builder->CreateAnd(invocationId, m_builder->getInt32(0xFF));

  auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {esGsOffsets01, m_builder->getInt32(0), m_builder->getInt32(16)});

  auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {esGsOffsets01, m_builder->getInt32(16), m_builder->getInt32(16)});

  auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {esGsOffsets23, m_builder->getInt32(0), m_builder->getInt32(16)});

  auto esGsOffset3 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {esGsOffsets23, m_builder->getInt32(16), m_builder->getInt32(16)});

  auto esGsOffset4 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {esGsOffsets45, m_builder->getInt32(0), m_builder->getInt32(16)});

  auto esGsOffset5 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {esGsOffsets45, m_builder->getInt32(16), m_builder->getInt32(16)});

  std::vector<Value *> args;

  auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  const unsigned userDataCount = intfData->userDataCount;

  unsigned userDataIdx = 0;

  auto gsArgBegin = gsEntry->arg_begin();
  const unsigned gsArgCount = gsEntry->arg_size();
  (void(gsArgCount)); // unused

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(args.size() < gsArgCount);

    auto gsArg = (gsArgBegin + args.size());
    assert(gsArg->hasAttribute(Attribute::InReg));

    auto gsArgTy = gsArg->getType();
    if (gsArgTy->isVectorTy()) {
      assert(cast<VectorType>(gsArgTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<VectorType>(gsArgTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto gsUserData = m_builder->CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(gsUserData);
    } else {
      assert(gsArgTy->isIntegerTy());

      auto gsUserData = m_builder->CreateExtractElement(userData, userDataIdx);
      args.push_back(gsUserData);
      ++userDataIdx;
    }
  }

  // Set up system value SGPRs
  args.push_back(gsVsOffset);
  args.push_back(waveId);

  // Set up system value VGPRs
  args.push_back(esGsOffset0);
  args.push_back(esGsOffset1);
  args.push_back(gsPrimitiveId);
  args.push_back(esGsOffset2);
  args.push_back(esGsOffset3);
  args.push_back(esGsOffset4);
  args.push_back(esGsOffset5);
  args.push_back(invocationId);

  assert(args.size() == gsArgCount); // Must have visit all arguments of ES entry point

  m_builder->CreateCall(gsEntry, args);
}

// =====================================================================================================================
// Mutates GS to handle exporting GS outputs to GS-VS ring, and the messages GS_EMIT/GS_CUT.
//
// @param module : LLVM module
Function *NggPrimShader::mutateGs(Module *module) {
  assert(m_hasGs); // GS must be present

  auto gsEntryPoint = module->getFunction(lgcName::NggGsEntryPoint);
  assert(gsEntryPoint);

  auto savedInsertPos = m_builder->saveIP();

  std::vector<Instruction *> removeCalls;

  m_builder->SetInsertPoint(&*gsEntryPoint->front().getFirstInsertionPt());

  // Initialize counters of GS emitted vertices and GS output vertices of current primitive
  Value *emitVertsPtrs[MaxGsStreams] = {};
  Value *outVertsPtrs[MaxGsStreams] = {};

  for (int i = 0; i < MaxGsStreams; ++i) {
    auto emitVertsPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
    m_builder->CreateStore(m_builder->getInt32(0), emitVertsPtr); // emitVerts = 0
    emitVertsPtrs[i] = emitVertsPtr;

    auto outVertsPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
    m_builder->CreateStore(m_builder->getInt32(0), outVertsPtr); // outVerts = 0
    outVertsPtrs[i] = outVertsPtr;
  }

  // Initialize thread ID in wave
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  auto threadIdInWave =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder->getInt32(-1), m_builder->getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder->getInt32(-1), threadIdInWave});
  }

  // Initialzie thread ID in subgroup
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
  auto waveId = getFunctionArgument(gsEntryPoint, entryArgIdxs.waveId);

  auto threadIdInSubgroup = m_builder->CreateMul(waveId, m_builder->getInt32(waveSize));
  threadIdInSubgroup = m_builder->CreateAdd(threadIdInSubgroup, threadIdInWave);

  // Handle GS message and GS output export
  for (auto &func : module->functions()) {
    if (func.getName().startswith(lgcName::NggGsOutputExport)) {
      // Export GS outputs to GS-VS ring
      for (auto user : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(user);
        assert(call);
        m_builder->SetInsertPoint(call);

        assert(call->getNumArgOperands() == 4);
        const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
        const unsigned compIdx = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
        assert(streamId < MaxGsStreams);
        Value *output = call->getOperand(3);

        auto emitVerts = m_builder->CreateLoad(emitVertsPtrs[streamId]);
        exportGsOutput(output, location, compIdx, streamId, threadIdInSubgroup, emitVerts);

        removeCalls.push_back(call);
      }
    } else if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg) {
      // Handle GS message
      for (auto user : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(user);
        assert(call);
        m_builder->SetInsertPoint(call);

        uint64_t message = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        if (message == GsEmitStreaM0 || message == GsEmitStreaM1 || message == GsEmitStreaM2 ||
            message == GsEmitStreaM3) {
          // Handle GS_EMIT, MSG[9:8] = STREAM_ID
          unsigned streamId = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
          assert(streamId < MaxGsStreams);
          processGsEmit(module, streamId, threadIdInSubgroup, emitVertsPtrs[streamId], outVertsPtrs[streamId]);
        } else if (message == GsCutStreaM0 || message == GsCutStreaM1 || message == GsCutStreaM2 ||
                   message == GsCutStreaM3) {
          // Handle GS_CUT, MSG[9:8] = STREAM_ID
          unsigned streamId = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
          assert(streamId < MaxGsStreams);
          processGsCut(module, streamId, outVertsPtrs[streamId]);
        } else if (message == GsDone) {
          // Handle GS_DONE, do nothing (just remove this call)
        } else {
          // Unexpected GS message
          llvm_unreachable("Unexpected GS message!");
        }

        removeCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removeCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }

  m_builder->restoreIP(savedInsertPos);

  return gsEntryPoint;
}

// =====================================================================================================================
// Runs copy shader.
//
// @param module : LLVM module
// @param vertCompacted : Whether vertex compaction is performed
void NggPrimShader::runCopyShader(Module *module, Value *vertCompacted) {
  assert(m_hasGs); // GS must be present

  //
  // The processing is something like this:
  //
  //   uncompactVertexId = Thread ID in subgroup
  //   if (vertCompacted)
  //     uncompactVertexId = Read uncompacted vertex ID from LDS
  //   Calculate vertex offset and run copy shader
  //
  auto expVertBlock = m_builder->GetInsertBlock();

  auto uncompactOutVertIdBlock = createBlock(expVertBlock->getParent(), ".uncompactOutVertId");
  uncompactOutVertIdBlock->moveAfter(expVertBlock);

  auto endUncompactOutVertIdBlock = createBlock(expVertBlock->getParent(), ".endUncompactOutVertId");
  endUncompactOutVertIdBlock->moveAfter(uncompactOutVertIdBlock);

  m_builder->CreateCondBr(vertCompacted, uncompactOutVertIdBlock, endUncompactOutVertIdBlock);

  // Construct ".uncompactOutVertId" block
  Value *uncompactVertexId = nullptr;
  {
    m_builder->SetInsertPoint(uncompactOutVertIdBlock);

    uncompactVertexId =
        readPerThreadDataFromLds(m_builder->getInt32Ty(), m_nggFactor.threadIdInSubgroup, LdsRegionOutVertThreadIdMap);

    m_builder->CreateBr(endUncompactOutVertIdBlock);
  }

  // Construct ".endUncompactOutVertId" block
  {
    m_builder->SetInsertPoint(endUncompactOutVertIdBlock);

    auto uncompactVertexIdPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
    uncompactVertexIdPhi->addIncoming(uncompactVertexId, uncompactOutVertIdBlock);
    uncompactVertexIdPhi->addIncoming(m_nggFactor.threadIdInSubgroup, expVertBlock);

    const auto rasterStream = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.rasterStream;
    auto vertexOffset = calcVertexItemOffset(rasterStream, uncompactVertexIdPhi);

    auto copyShaderEntry = mutateCopyShader(module);

    // Run copy shader
    std::vector<Value *> args;

    static const unsigned CopyShaderSysValueCount = 11; // Fixed layout: 10 SGPRs, 1 VGPR
    for (unsigned i = 0; i < CopyShaderSysValueCount; ++i) {
      if (i == CopyShaderUserSgprIdxVertexOffset)
        args.push_back(vertexOffset);
      else {
        // All SGPRs are not used
        args.push_back(UndefValue::get(getFunctionArgument(copyShaderEntry, i)->getType()));
      }
    }

    m_builder->CreateCall(copyShaderEntry, args);
  }
}

// =====================================================================================================================
// Mutates copy shader to handle the importing GS outputs from GS-VS ring.
//
// @param module : LLVM module
Function *NggPrimShader::mutateCopyShader(Module *pModule) {
  auto copyShaderEntryPoint = pModule->getFunction(lgcName::NggCopyShaderEntryPoint);
  assert(copyShaderEntryPoint != nullptr);

  auto savedInsertPos = m_builder->saveIP();

  auto vertexOffset = getFunctionArgument(copyShaderEntryPoint, CopyShaderUserSgprIdxVertexOffset);

  std::vector<Instruction *> removeCalls;

  for (auto &func : pModule->functions()) {
    if (func.getName().startswith(lgcName::NggGsOutputImport)) {
      // Import GS outputs from GS-VS ring
      for (auto pUser : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(pUser);
        assert(call != nullptr);
        m_builder->SetInsertPoint(call);

        assert(call->getNumArgOperands() == 3);
        const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
        const unsigned compIdx = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
        assert(streamId < MaxGsStreams);

        auto output = importGsOutput(call->getType(), location, compIdx, streamId, vertexOffset);

        call->replaceAllUsesWith(output);
        removeCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removeCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }

  m_builder->restoreIP(savedInsertPos);

  return copyShaderEntryPoint;
}

// =====================================================================================================================
// Exports outputs of geometry shader to GS-VS ring.
//
// NOTE: The GS-VS ring layout in NGG mode is very different from that of non-NGG. We purposely group output vertices
// according to their belonging vertex streams in that copy shader doesn't exist actually and we take full control of
// GS-VS ring. The ring does not have to conform to hardware design requirements any more. This layout is to facilitate
// vertex offset calculation when we do vertex exporting and could improve NGG throughput by avoiding
// input-primitive-based loop.
//
// The layout is something like this (shader takes over it):
//
//   +----------+----+----------+----+----------+----+----------+
//   | Vertex 0 | .. | Vertex N | .. | Vertex 0 | .. | Vertex N | (N = max_vertices)
//   +----------+----+----------+----+----------+----+----------+
//   |<------ Primitive 0 ----->| .. |<------ Primitive M ----->| (M = prims_per_subgroup)
//   |<----------------------- Stream i ----------------------->|
//
//   +----------+----------+----------+----------+
//   | Stream 0 | Stream 1 | Stream 2 | Stream 3 |
//   +----------+----------+----------+----------+
//   |<--------------- GS-VS ring -------------->|
//
// By contrast, GS-VS ring layout of non-NGG is something like this (conform to hardware design):
//
//   +----------+----+----------+----+----------+----+----------+
//   | Vertex 0 | .. | Vertex N | .. | Vertex 0 | .. | Vertex N | (N = max_vertices)
//   +----------+----+----------+----+----------+----+----------+
//   |<-------- Stream 0 ------>| .. |<-------- Stream 3 ------>|
//   |<---------------------- Primitive i --------------------->|
//
//   +-------------+----+-------------+
//   | Primitive 0 | .. | Primitive M | (M = prims_per_subgroup)
//   +-------------+----+-------------+
//   |<--------- GS-VS ring --------->|
//
// @param output : Output value
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param streamId : ID of output vertex stream
// @param threadIdInSubgroup : Thread ID in sub-group
// @param emitVerts : Counter of GS emitted vertices for this stream
void NggPrimShader::exportGsOutput(Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                                   Value *threadIdInSubgroup, Value *emitVerts) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  if (resUsage->inOutUsage.gs.rasterStream != streamId) {
    // NOTE: Only export those outputs that belong to the rasterization stream.
    assert(resUsage->inOutUsage.enableXfb == false); // Transform feedback must be disabled
    return;
  }

  // NOTE: We only handle LDS vector/scalar writing, so change [n x Ty] to <n x Ty> for array.
  auto outputTy = output->getType();
  if (outputTy->isArrayTy()) {
    auto outputElemTy = outputTy->getArrayElementType();
    assert(outputElemTy->isSingleValueType());

    // [n x Ty] -> <n x Ty>
    const unsigned elemCount = outputTy->getArrayNumElements();
    Value *outputVec = UndefValue::get(VectorType::get(outputElemTy, elemCount));
    for (unsigned i = 0; i < elemCount; ++i) {
      auto outputElem = m_builder->CreateExtractValue(output, i);
      m_builder->CreateInsertElement(outputVec, outputElem, i);
    }

    outputTy = outputVec->getType();
    output = outputVec;
  }

  const unsigned bitWidth = output->getType()->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word
    // to dword. This is because copy shader does not know the actual data type. It only generates output
    // export calls based on number of dwords.
    if (outputTy->isFPOrFPVectorTy()) {
      assert(bitWidth == 16);
      Type *castTy = m_builder->getInt16Ty();
      if (outputTy->isVectorTy())
        castTy = VectorType::get(m_builder->getInt16Ty(), cast<VectorType>(outputTy)->getNumElements());
      output = m_builder->CreateBitCast(output, castTy);
    }

    Type *extTy = m_builder->getInt32Ty();
    if (outputTy->isVectorTy())
      extTy = VectorType::get(m_builder->getInt32Ty(), cast<VectorType>(outputTy)->getNumElements());
    output = m_builder->CreateZExt(output, extTy);
  } else
    assert(bitWidth == 32 || bitWidth == 64);

  // vertexId = threadIdInSubgroup * outputVertices + emitVerts
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  auto vertexId = m_builder->CreateMul(threadIdInSubgroup, m_builder->getInt32(geometryMode.outputVertices));
  vertexId = m_builder->CreateAdd(vertexId, emitVerts);

  // ldsOffset = vertexOffset + (location * 4 + compIdx) * 4 (in bytes)
  auto vertexOffset = calcVertexItemOffset(streamId, vertexId);
  const unsigned attribOffset = (location * 4) + compIdx;
  auto ldsOffset = m_builder->CreateAdd(vertexOffset, m_builder->getInt32(attribOffset * 4));

  m_ldsManager->writeValueToLds(output, ldsOffset);
}

// =====================================================================================================================
// Imports outputs of geometry shader from GS-VS ring.
//
// @param outputTy : Type of the output
// @param location : Location of the output
// @param compIdx : Index used for vector element indexing
// @param streamId : ID of output vertex stream
// @param vertexOffset : Start offset of vertex item in GS-VS ring (in bytes)
Value *NggPrimShader::importGsOutput(Type *outputTy, unsigned location, unsigned compIdx, unsigned streamId,
                                     Value *vertexOffset) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  if (resUsage->inOutUsage.gs.rasterStream != streamId) {
    // NOTE: Only import those outputs that belong to the rasterization stream.
    assert(resUsage->inOutUsage.enableXfb == false); // Transform feedback must be disabled
    return UndefValue::get(outputTy);
  }

  // NOTE: We only handle LDS vector/scalar reading, so change [n x Ty] to <n x Ty> for array.
  auto origOutputTy = outputTy;
  if (outputTy->isArrayTy()) {
    auto outputElemTy = outputTy->getArrayElementType();
    assert(outputElemTy->isSingleValueType());

    // [n x Ty] -> <n x Ty>
    const unsigned elemCount = outputTy->getArrayNumElements();
    outputTy = VectorType::get(outputElemTy, elemCount);
  }

  // ldsOffset = vertexOffset + (location * 4 + compIdx) * 4 (in bytes)
  const unsigned attribOffset = (location * 4) + compIdx;
  auto ldsOffset = m_builder->CreateAdd(vertexOffset, m_builder->getInt32(attribOffset * 4));

  auto output = m_ldsManager->readValueFromLds(outputTy, ldsOffset);

  if (origOutputTy != outputTy) {
    assert(origOutputTy->isArrayTy() && outputTy->isVectorTy() &&
           origOutputTy->getArrayNumElements() == cast<VectorType>(outputTy)->getNumElements());

    // <n x Ty> -> [n x Ty]
    const unsigned elemCount = origOutputTy->getArrayNumElements();
    Value *outputArray = UndefValue::get(origOutputTy);
    for (unsigned i = 0; i < elemCount; ++i) {
      auto outputElem = m_builder->CreateExtractElement(output, i);
      outputArray = m_builder->CreateInsertValue(outputArray, outputElem, i);
    }

    output = outputArray;
  }

  return output;
}

// =====================================================================================================================
// Processes the message GS_EMIT.
//
// @param module : LLVM module
// @param streamId : ID of output vertex stream
// @param threadIdInSubgroup : Thread ID in subgroup
// @param [in,out] emitVertsPtr : Pointer to the counter of GS emitted vertices for this stream
// @param [in,out] outVertsPtr : Pointer to the counter of GS output vertices of current primitive for this stream
void NggPrimShader::processGsEmit(Module *module, unsigned streamId, Value *threadIdInSubgroup, Value *emitVertsPtr,
                                  Value *outVertsPtr) {
  auto gsEmitHandler = module->getFunction(lgcName::NggGsEmit);
  if (!gsEmitHandler)
    gsEmitHandler = createGsEmitHandler(module, streamId);

  m_builder->CreateCall(gsEmitHandler, {threadIdInSubgroup, emitVertsPtr, outVertsPtr});
}

// =====================================================================================================================
// Processes the message GS_CUT.
//
// @param module : LLVM module
// @param streamId : ID of output vertex stream
// @param [in,out] outVertsPtr : Pointer to the counter of GS output vertices of current primitive for this stream
void NggPrimShader::processGsCut(Module *module, unsigned streamId, Value *outVertsPtr) {
  auto gsCutHandler = module->getFunction(lgcName::NggGsCut);
  if (!gsCutHandler)
    gsCutHandler = createGsCutHandler(module, streamId);

  m_builder->CreateCall(gsCutHandler, outVertsPtr);
}

// =====================================================================================================================
// Creates the function that processes GS_EMIT.
//
// @param module : LLVM module
// @param streamId : ID of output vertex stream
Function *NggPrimShader::createGsEmitHandler(Module *module, unsigned streamId) {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   emitVerts++
  //   outVerts++
  //
  //   if (outVerts >= outVertsPerPrim) {
  //     winding = triangleStrip ? ((outVerts - outVertsPerPrim) & 0x1) : 0
  //     N (starting vertex ID) = threadIdInSubgroup * outputVertices + emitVerts - outVertsPerPrim
  //     primData[N] = winding
  //   }
  //
  const auto addrSpace = module->getDataLayout().getAllocaAddrSpace();
  auto funcTy = FunctionType::get(m_builder->getVoidTy(),
                                  {
                                      m_builder->getInt32Ty(),                              // %threadIdInSubgroup
                                      PointerType::get(m_builder->getInt32Ty(), addrSpace), // %emitVertsPtr
                                      PointerType::get(m_builder->getInt32Ty(), addrSpace), // %outVertsPtr
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggGsEmit, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *threadIdInSubgroup = argIt++;
  threadIdInSubgroup->setName("threadIdInSubgroup");

  Value *emitVertsPtr = argIt++;
  emitVertsPtr->setName("emitVertsPtr");

  Value *outVertsPtr = argIt++;
  outVertsPtr->setName("outVertsPtr");

  auto entryBlock = createBlock(func, ".entry");
  auto emitPrimBlock = createBlock(func, ".emitPrim");
  auto endEmitPrimBlock = createBlock(func, ".endEmitPrim");

  auto savedInsertPoint = m_builder->saveIP();

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const auto &resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

  const unsigned outVertsPerPrim = getOutputVerticesPerPrimitive();

  // Construct ".entry" block
  Value *emitVerts = nullptr;
  Value *outVerts = nullptr;
  Value *primEmit = nullptr;
  {
    m_builder->SetInsertPoint(entryBlock);

    emitVerts = m_builder->CreateLoad(emitVertsPtr);
    outVerts = m_builder->CreateLoad(outVertsPtr);

    // emitVerts++
    emitVerts = m_builder->CreateAdd(emitVerts, m_builder->getInt32(1));

    // outVerts++
    outVerts = m_builder->CreateAdd(outVerts, m_builder->getInt32(1));

    // primEmit = (outVerts >= outVertsPerPrim)
    primEmit = m_builder->CreateICmpUGE(outVerts, m_builder->getInt32(outVertsPerPrim));
    m_builder->CreateCondBr(primEmit, emitPrimBlock, endEmitPrimBlock);
  }

  // Construct ".emitPrim" block
  {
    m_builder->SetInsertPoint(emitPrimBlock);

    // NOTE: Only calculate GS output primitive data and write it to LDS for rasterization stream.
    if (streamId == resUsage->inOutUsage.gs.rasterStream) {
      // vertexId = threadIdInSubgroup * outputVertices + emitVerts - outVertsPerPrim
      auto vertexId = m_builder->CreateMul(threadIdInSubgroup, m_builder->getInt32(geometryMode.outputVertices));
      vertexId = m_builder->CreateAdd(vertexId, emitVerts);
      vertexId = m_builder->CreateSub(vertexId, m_builder->getInt32(outVertsPerPrim));

      Value *winding = m_builder->getInt32(0);
      if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip) {
        winding = m_builder->CreateSub(outVerts, m_builder->getInt32(outVertsPerPrim));
        winding = m_builder->CreateAnd(winding, 0x1);
      }

      // Write primitive data (just winding)
      writePerThreadDataToLds(winding, vertexId, LdsRegionOutPrimData);
    }

    m_builder->CreateBr(endEmitPrimBlock);
  }

  // Construct ".endEmitPrim" block
  {
    m_builder->SetInsertPoint(endEmitPrimBlock);

    m_builder->CreateStore(emitVerts, emitVertsPtr);
    m_builder->CreateStore(outVerts, outVertsPtr);
    m_builder->CreateRetVoid();
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that processes GS_CUT.
//
// @param module : LLVM module
// @param streamId : ID of output vertex stream
Function *NggPrimShader::createGsCutHandler(Module *module, unsigned streamId) {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   outVerts = 0
  //
  const auto addrSpace = module->getDataLayout().getAllocaAddrSpace();
  auto funcTy =
      FunctionType::get(m_builder->getVoidTy(), PointerType::get(m_builder->getInt32Ty(), addrSpace), // %outVertsPtr
                        false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggGsCut, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *outVertsPtr = argIt++;
  outVertsPtr->setName("outVertsPtr");

  auto entryBlock = createBlock(func, ".entry");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".entry" block
  {
    m_builder->SetInsertPoint(entryBlock);
    m_builder->CreateStore(m_builder->getInt32(0), outVertsPtr); // Reset outVerts
    m_builder->CreateRetVoid();
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Reads per-thread data from the specified NGG region in LDS.
//
// @param readDataTy : Data written to LDS
// @param threadId : Thread ID in sub-group to calculate LDS offset
// @param region : NGG LDS region
Value *NggPrimShader::readPerThreadDataFromLds(Type *readDataTy, Value *threadId, NggLdsRegionType region) {
  auto sizeInBytes = readDataTy->getPrimitiveSizeInBits() / 8;

  const auto regionStart = m_ldsManager->getLdsRegionStart(region);

  Value *ldsOffset = nullptr;
  if (sizeInBytes > 1)
    ldsOffset = m_builder->CreateMul(threadId, m_builder->getInt32(sizeInBytes));
  else
    ldsOffset = threadId;
  ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

  return m_ldsManager->readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes the per-thread data to the specified NGG region in LDS.
//
// @param writeData : Data written to LDS
// @param threadId : Thread ID in sub-group to calculate LDS offset
// @param region : NGG LDS region
void NggPrimShader::writePerThreadDataToLds(Value *writeData, Value *threadId, NggLdsRegionType region) {
  auto writeDataTy = writeData->getType();
  auto sizeInBytes = writeDataTy->getPrimitiveSizeInBits() / 8;

  const auto regionStart = m_ldsManager->getLdsRegionStart(region);

  Value *ldsOffset = nullptr;
  if (sizeInBytes > 1)
    ldsOffset = m_builder->CreateMul(threadId, m_builder->getInt32(sizeInBytes));
  else
    ldsOffset = threadId;
  ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

  m_ldsManager->writeValueToLds(writeData, ldsOffset);
}

// =====================================================================================================================
// Backface culler.
//
// @param module : LLVM module
// @param cullFlag : Cull flag before doing this culling
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::doBackfaceCulling(Module *module, Value *cullFlag, Value *vertex0, Value *vertex1,
                                        Value *vertex2) {
  assert(m_nggControl->enableBackfaceCulling);

  auto backfaceCuller = module->getFunction(lgcName::NggCullingBackface);
  if (!backfaceCuller)
    backfaceCuller = createBackfaceCuller(module);

  // Get register PA_SU_SC_MODE_CNTL
  Value *paSuScModeCntl = nullptr;
  if (m_nggControl->alwaysUsePrimShaderTable)
    paSuScModeCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paSuScModeCntl);
  else
    paSuScModeCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paSuScModeCntl);

  // Get register PA_CL_VPORT_XSCALE
  auto paClVportXscale = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportXscale);

  // Get register PA_CL_VPORT_YSCALE
  auto paClVportYscale = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportYscale);

  // Do backface culling
  return m_builder->CreateCall(backfaceCuller, {cullFlag, vertex0, vertex1, vertex2,
                                                m_builder->getInt32(m_nggControl->backfaceExponent), paSuScModeCntl,
                                                paClVportXscale, paClVportYscale});
}

// =====================================================================================================================
// Frustum culler.
//
// @param module : LLVM module
// @param cullFlag : Cull flag before doing this culling
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::doFrustumCulling(Module *module, Value *cullFlag, Value *vertex0, Value *vertex1,
                                       Value *vertex2) {
  assert(m_nggControl->enableFrustumCulling);

  auto frustumCuller = module->getFunction(lgcName::NggCullingFrustum);
  if (!frustumCuller)
    frustumCuller = createFrustumCuller(module);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = nullptr;
  if (m_nggControl->alwaysUsePrimShaderTable)
    paClClipCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paClClipCntl);
  else
    paClClipCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbVertDiscAdj);

  // Do frustum culling
  return m_builder->CreateCall(
      frustumCuller, {cullFlag, vertex0, vertex1, vertex2, paClClipCntl, paClGbHorzDiscAdj, paClGbVertDiscAdj});
}

// =====================================================================================================================
// Box filter culler.
//
// @param module : LLVM module
// @param cullFlag : Cull flag before doing this culling
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::doBoxFilterCulling(Module *module, Value *cullFlag, Value *vertex0, Value *vertex1,
                                         Value *vertex2) {
  assert(m_nggControl->enableBoxFilterCulling);

  auto boxFilterCuller = module->getFunction(lgcName::NggCullingBoxFilter);
  if (!boxFilterCuller)
    boxFilterCuller = createBoxFilterCuller(module);

  // Get register PA_CL_VTE_CNTL
  Value *paClVteCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = nullptr;
  if (m_nggControl->alwaysUsePrimShaderTable)
    paClClipCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paClClipCntl);
  else
    paClClipCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbVertDiscAdj);

  // Do box filter culling
  return m_builder->CreateCall(boxFilterCuller, {cullFlag, vertex0, vertex1, vertex2, paClVteCntl, paClClipCntl,
                                                 paClGbHorzDiscAdj, paClGbVertDiscAdj});
}

// =====================================================================================================================
// Sphere culler.
//
// @param module : LLVM module
// @param cullFlag : Cull flag before doing this culling
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::doSphereCulling(Module *module, Value *cullFlag, Value *vertex0, Value *vertex1, Value *vertex2) {
  assert(m_nggControl->enableSphereCulling);

  auto sphereCuller = module->getFunction(lgcName::NggCullingSphere);
  if (!sphereCuller)
    sphereCuller = createSphereCuller(module);

  // Get register PA_CL_VTE_CNTL
  Value *paClVteCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = nullptr;
  if (m_nggControl->alwaysUsePrimShaderTable)
    paClClipCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paClClipCntl);
  else
    paClClipCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbVertDiscAdj);

  // Do small primitive filter culling
  return m_builder->CreateCall(sphereCuller, {cullFlag, vertex0, vertex1, vertex2, paClVteCntl, paClClipCntl,
                                              paClGbHorzDiscAdj, paClGbVertDiscAdj});
}

// =====================================================================================================================
// Small primitive filter culler.
//
// @param module : LLVM module
// @param cullFlag : Cull flag before doing this culling
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::doSmallPrimFilterCulling(Module *module, Value *cullFlag, Value *vertex0, Value *vertex1,
                                               Value *vertex2) {
  assert(m_nggControl->enableSmallPrimFilter);

  auto smallPrimFilterCuller = module->getFunction(lgcName::NggCullingSmallPrimFilter);
  if (!smallPrimFilterCuller)
    smallPrimFilterCuller = createSmallPrimFilterCuller(module);

  // Get register PA_CL_VTE_CNTL
  Value *paClVteCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

  // Get register PA_CL_VPORT_XSCALE
  auto paClVportXscale = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportXscale);

  // Get register PA_CL_VPORT_XOFFSET
  auto paClVportXoffset = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportXoffset);

  // Get register PA_CL_VPORT_YSCALE
  auto paClVportYscale = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportYscale);

  // Get register PA_CL_VPORT_YOFFSET
  auto paClVportYoffset = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportYoffset);

  // Get run-time flag enableConservativeRasterization
  auto conservativeRaster = fetchCullingControlRegister(module, m_cbLayoutTable.enableConservativeRasterization);
  conservativeRaster = m_builder->CreateICmpNE(conservativeRaster, m_builder->getInt32(0));

  // Do small primitive filter culling
  return m_builder->CreateCall(smallPrimFilterCuller,
                               {cullFlag, vertex0, vertex1, vertex2, paClVteCntl, paClVportXscale, paClVportXoffset,
                                paClVportYscale, paClVportYoffset, conservativeRaster});
}

// =====================================================================================================================
// Cull distance culler.
//
// @param module : LLVM module
// @param cullFlag : Cull flag before doing this culling
// @param signMask0 : Sign mask of cull distance of vertex0
// @param signMask1 : Sign mask of cull distance of vertex1
// @param signMask2 : Sign mask of cull distance of vertex2
Value *NggPrimShader::doCullDistanceCulling(Module *module, Value *cullFlag, Value *signMask0, Value *signMask1,
                                            Value *signMask2) {
  assert(m_nggControl->enableCullDistanceCulling);

  auto cullDistanceCuller = module->getFunction(lgcName::NggCullingCullDistance);
  if (!cullDistanceCuller)
    cullDistanceCuller = createCullDistanceCuller(module);

  // Do cull distance culling
  return m_builder->CreateCall(cullDistanceCuller, {cullFlag, signMask0, signMask1, signMask2});
}

// =====================================================================================================================
// Fetches culling-control register from primitive shader table.
//
// @param module : LLVM module
// @param regOffset : Register offset in the primitive shader table (in bytes)
Value *NggPrimShader::fetchCullingControlRegister(Module *module, unsigned regOffset) {
  auto fetchCullingRegister = module->getFunction(lgcName::NggCullingFetchReg);
  if (!fetchCullingRegister)
    fetchCullingRegister = createFetchCullingRegister(module);

  return m_builder->CreateCall(
      fetchCullingRegister,
      {m_nggFactor.primShaderTableAddrLow, m_nggFactor.primShaderTableAddrHigh, m_builder->getInt32(regOffset)});
}

// =====================================================================================================================
// Creates the function that does backface culling.
//
// @param module : LLVM module
Function *NggPrimShader::createBackfaceCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                  {
                                      m_builder->getInt1Ty(),                           // %cullFlag
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                      m_builder->getInt32Ty(),                          // %backfaceExponent
                                      m_builder->getInt32Ty(),                          // %paSuScModeCntl
                                      m_builder->getInt32Ty(),                          // %paClVportXscale
                                      m_builder->getInt32Ty()                           // %paClVportYscale
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingBackface, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadNone);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *cullFlag = argIt++;
  cullFlag->setName("cullFlag");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *backfaceExponent = argIt++;
  backfaceExponent->setName("backfaceExponent");

  Value *paSuScModeCntl = argIt++;
  paSuScModeCntl->setName("paSuScModeCntl");

  Value *paClVportXscale = argIt++;
  paClVportXscale->setName("paClVportXscale");

  Value *paClVportYscale = argIt++;
  paClVportYscale->setName("paClVportYscale");

  auto backfaceEntryBlock = createBlock(func, ".backfaceEntry");
  auto backfaceCullBlock = createBlock(func, ".backfaceCull");
  auto backfaceExponentBlock = createBlock(func, ".backfaceExponent");
  auto endBackfaceCullBlock = createBlock(func, ".endBackfaceCull");
  auto backfaceExitBlock = createBlock(func, ".backfaceExit");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".backfaceEntry" block
  {
    m_builder->SetInsertPoint(backfaceEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder->CreateCondBr(cullFlag, backfaceExitBlock, backfaceCullBlock);
  }

  // Construct ".backfaceCull" block
  Value *cullFlag1 = nullptr;
  Value *w0 = nullptr;
  Value *w1 = nullptr;
  Value *w2 = nullptr;
  Value *area = nullptr;
  {
    m_builder->SetInsertPoint(backfaceCullBlock);

    //
    // Backface culling algorithm is described as follow:
    //
    //   if ((area > 0 && face == CCW) || (area < 0 && face == CW))
    //     frontFace = true
    //
    //   if ((area < 0 && face == CCW) || (area > 0 && face == CW))
    //     backFace = true
    //
    //   if (area == 0 || (frontFace && cullFront) || (backFace && cullBack))
    //     cullFlag = true
    //

    //        | x0 y0 w0 |
    //        |          |
    // area = | x1 y1 w1 | =  x0 * (y1 * w2 - y2 * w1) - x1 * (y0 * w2 - y2 * w0) + x2 * (y0 * w1 - y1 * w0)
    //        |          |
    //        | x2 y2 w2 |
    //
    auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder->CreateExtractElement(vertex0, 1);
    w0 = m_builder->CreateExtractElement(vertex0, 3);

    auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder->CreateExtractElement(vertex1, 1);
    w1 = m_builder->CreateExtractElement(vertex1, 3);

    auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder->CreateExtractElement(vertex2, 1);
    w2 = m_builder->CreateExtractElement(vertex2, 3);

    auto y1W2 = m_builder->CreateFMul(y1, w2);
    auto y2W1 = m_builder->CreateFMul(y2, w1);
    auto det0 = m_builder->CreateFSub(y1W2, y2W1);
    det0 = m_builder->CreateFMul(x0, det0);

    auto y0W2 = m_builder->CreateFMul(y0, w2);
    auto y2W0 = m_builder->CreateFMul(y2, w0);
    auto det1 = m_builder->CreateFSub(y0W2, y2W0);
    det1 = m_builder->CreateFMul(x1, det1);

    auto y0W1 = m_builder->CreateFMul(y0, w1);
    auto y1W0 = m_builder->CreateFMul(y1, w0);
    auto det2 = m_builder->CreateFSub(y0W1, y1W0);
    det2 = m_builder->CreateFMul(x2, det2);

    area = m_builder->CreateFSub(det0, det1);
    area = m_builder->CreateFAdd(area, det2);

    auto areaLtZero = m_builder->CreateFCmpOLT(area, ConstantFP::get(m_builder->getFloatTy(), 0.0));
    auto areaGtZero = m_builder->CreateFCmpOGT(area, ConstantFP::get(m_builder->getFloatTy(), 0.0));

    // xScale ^ yScale
    auto frontFace = m_builder->CreateXor(paClVportXscale, paClVportYscale);

    // signbit(xScale ^ yScale)
    frontFace = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                           {frontFace, m_builder->getInt32(31), m_builder->getInt32(1)});

    // face = (FACE, PA_SU_SC_MODE_CNTRL[2], 0 = CCW, 1 = CW)
    auto face = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                           {paSuScModeCntl, m_builder->getInt32(2), m_builder->getInt32(1)});

    // face ^ signbit(xScale ^ yScale)
    frontFace = m_builder->CreateXor(face, frontFace);

    // (face ^ signbit(xScale ^ yScale)) == 0
    frontFace = m_builder->CreateICmpEQ(frontFace, m_builder->getInt32(0));

    // frontFace = ((face ^ signbit(xScale ^ yScale)) == 0) ? (area < 0) : (area > 0)
    frontFace = m_builder->CreateSelect(frontFace, areaLtZero, areaGtZero);

    // backFace = !frontFace
    auto backFace = m_builder->CreateNot(frontFace);

    // cullFront = (CULL_FRONT, PA_SU_SC_MODE_CNTRL[0], 0 = DONT CULL, 1 = CULL)
    auto cullFront = m_builder->CreateAnd(paSuScModeCntl, m_builder->getInt32(1));
    cullFront = m_builder->CreateTrunc(cullFront, m_builder->getInt1Ty());

    // cullBack = (CULL_BACK, PA_SU_SC_MODE_CNTRL[1], 0 = DONT CULL, 1 = CULL)
    Value *cullBack = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                 {paSuScModeCntl, m_builder->getInt32(1), m_builder->getInt32(1)});
    cullBack = m_builder->CreateTrunc(cullBack, m_builder->getInt1Ty());

    // cullFront = cullFront ? frontFace : false
    cullFront = m_builder->CreateSelect(cullFront, frontFace, m_builder->getFalse());

    // cullBack = cullBack ? backFace : false
    cullBack = m_builder->CreateSelect(cullBack, backFace, m_builder->getFalse());

    // cullFlag = cullFront || cullBack
    cullFlag1 = m_builder->CreateOr(cullFront, cullBack);

    auto nonZeroBackfaceExp = m_builder->CreateICmpNE(backfaceExponent, m_builder->getInt32(0));
    m_builder->CreateCondBr(nonZeroBackfaceExp, backfaceExponentBlock, endBackfaceCullBlock);
  }

  // Construct ".backfaceExponent" block
  Value *cullFlag2 = nullptr;
  {
    m_builder->SetInsertPoint(backfaceExponentBlock);

    //
    // Ignore area calculations that are less enough
    //   if (|area| < (10 ^ (-backfaceExponent)) / |w0 * w1 * w2| )
    //       cullFlag = false
    //

    // |w0 * w1 * w2|
    auto absW0W1W2 = m_builder->CreateFMul(w0, w1);
    absW0W1W2 = m_builder->CreateFMul(absW0W1W2, w2);
    absW0W1W2 = m_builder->CreateIntrinsic(Intrinsic::fabs, m_builder->getFloatTy(), absW0W1W2);

    // threeshold = (10 ^ (-backfaceExponent)) / |w0 * w1 * w2|
    auto threshold = m_builder->CreateNeg(backfaceExponent);
    threshold = m_builder->CreateIntrinsic(Intrinsic::powi, m_builder->getFloatTy(),
                                           {ConstantFP::get(m_builder->getFloatTy(), 10.0), threshold});

    auto rcpAbsW0W1W2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), absW0W1W2);
    threshold = m_builder->CreateFMul(threshold, rcpAbsW0W1W2);

    // |area|
    auto absArea = m_builder->CreateIntrinsic(Intrinsic::fabs, m_builder->getFloatTy(), area);

    // cullFlag = cullFlag && (abs(area) >= threshold)
    cullFlag2 = m_builder->CreateFCmpOGE(absArea, threshold);
    cullFlag2 = m_builder->CreateAnd(cullFlag1, cullFlag2);

    m_builder->CreateBr(endBackfaceCullBlock);
  }

  // Construct ".endBackfaceCull" block
  Value *cullFlag3 = nullptr;
  {
    m_builder->SetInsertPoint(endBackfaceCullBlock);

    // cullFlag = cullFlag || (area == 0)
    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag1, backfaceCullBlock);
    cullFlagPhi->addIncoming(cullFlag2, backfaceExponentBlock);

    auto areaEqZero = m_builder->CreateFCmpOEQ(area, ConstantFP::get(m_builder->getFloatTy(), 0.0));

    cullFlag3 = m_builder->CreateOr(cullFlagPhi, areaEqZero);

    m_builder->CreateBr(backfaceExitBlock);
  }

  // Construct ".backfaceExit" block
  {
    m_builder->SetInsertPoint(backfaceExitBlock);

    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag, backfaceEntryBlock);
    cullFlagPhi->addIncoming(cullFlag3, endBackfaceCullBlock);

    // polyMode = (POLY_MODE, PA_SU_SC_MODE_CNTRL[4:3], 0 = DISABLE, 1 = DUAL)
    auto polyMode = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                               {
                                                   paSuScModeCntl,
                                                   m_builder->getInt32(3),
                                                   m_builder->getInt32(2),
                                               });

    // polyMode == 1
    auto wireFrameMode = m_builder->CreateICmpEQ(polyMode, m_builder->getInt32(1));

    // Disable backface culler if POLY_MODE is set to 1 (wireframe)
    // cullFlag = (polyMode == 1) ? false : cullFlag
    cullFlag = m_builder->CreateSelect(wireFrameMode, m_builder->getFalse(), cullFlagPhi);

    m_builder->CreateRet(cullFlag);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
//
// @param module : LLVM module
Function *NggPrimShader::createFrustumCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                  {
                                      m_builder->getInt1Ty(),                           // %cullFlag
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                      m_builder->getInt32Ty(),                          // %paClClipCntl
                                      m_builder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder->getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingFrustum, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadNone);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *cullFlag = argIt++;
  cullFlag->setName("cullFlag");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClClipCntl = argIt++;
  paClClipCntl->setName("paClClipCntl");

  Value *paClGbHorzDiscAdj = argIt++;
  paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

  Value *paClGbVertDiscAdj = argIt++;
  paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

  auto frustumEntryBlock = createBlock(func, ".frustumEntry");
  auto frustumCullBlock = createBlock(func, ".frustumCull");
  auto frustumExitBlock = createBlock(func, ".frustumExit");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".frustumEntry" block
  {
    m_builder->SetInsertPoint(frustumEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder->CreateCondBr(cullFlag, frustumExitBlock, frustumCullBlock);
  }

  // Construct ".frustumCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder->SetInsertPoint(frustumCullBlock);

    //
    // Frustum culling algorithm is described as follow:
    //
    //   if (x[i] > xDiscAdj * w[i] && y[i] > yDiscAdj * w[i] && z[i] > zFar * w[i])
    //     cullFlag = true
    //
    //   if (x[i] < -xDiscAdj * w[i] && y[i] < -yDiscAdj * w[i] && z[i] < zNear * w[i])
    //     cullFlag &= true
    //
    //   i = [0..2]
    //

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                     {paClClipCntl, m_builder->getInt32(19), m_builder->getInt32(1)});
    clipSpaceDef = m_builder->CreateTrunc(clipSpaceDef, m_builder->getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    auto zNear = m_builder->CreateSelect(clipSpaceDef, ConstantFP::get(m_builder->getFloatTy(), -1.0),
                                         ConstantFP::get(m_builder->getFloatTy(), 0.0));

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder->CreateBitCast(paClGbHorzDiscAdj, m_builder->getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder->CreateBitCast(paClGbVertDiscAdj, m_builder->getFloatTy());

    auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder->CreateExtractElement(vertex0, 1);
    auto z0 = m_builder->CreateExtractElement(vertex0, 2);
    auto w0 = m_builder->CreateExtractElement(vertex0, 3);

    auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder->CreateExtractElement(vertex1, 1);
    auto z1 = m_builder->CreateExtractElement(vertex1, 2);
    auto w1 = m_builder->CreateExtractElement(vertex1, 3);

    auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder->CreateExtractElement(vertex2, 1);
    auto z2 = m_builder->CreateExtractElement(vertex2, 2);
    auto w2 = m_builder->CreateExtractElement(vertex2, 3);

    // -xDiscAdj
    auto negXDiscAdj = m_builder->CreateFNeg(xDiscAdj);

    // -yDiscAdj
    auto negYDiscAdj = m_builder->CreateFNeg(yDiscAdj);

    Value *clipMask[6] = {};

    //
    // Get clip mask for vertex0
    //

    // (x0 < -xDiscAdj * w0) ? 0x1 : 0
    clipMask[0] = m_builder->CreateFMul(negXDiscAdj, w0);
    clipMask[0] = m_builder->CreateFCmpOLT(x0, clipMask[0]);
    clipMask[0] = m_builder->CreateSelect(clipMask[0], m_builder->getInt32(0x1), m_builder->getInt32(0));

    // (x0 > xDiscAdj * w0) ? 0x2 : 0
    clipMask[1] = m_builder->CreateFMul(xDiscAdj, w0);
    clipMask[1] = m_builder->CreateFCmpOGT(x0, clipMask[1]);
    clipMask[1] = m_builder->CreateSelect(clipMask[1], m_builder->getInt32(0x2), m_builder->getInt32(0));

    // (y0 < -yDiscAdj * w0) ? 0x4 : 0
    clipMask[2] = m_builder->CreateFMul(negYDiscAdj, w0);
    clipMask[2] = m_builder->CreateFCmpOLT(y0, clipMask[2]);
    clipMask[2] = m_builder->CreateSelect(clipMask[2], m_builder->getInt32(0x4), m_builder->getInt32(0));

    // (y0 > yDiscAdj * w0) ? 0x8 : 0
    clipMask[3] = m_builder->CreateFMul(yDiscAdj, w0);
    clipMask[3] = m_builder->CreateFCmpOGT(y0, clipMask[3]);
    clipMask[3] = m_builder->CreateSelect(clipMask[3], m_builder->getInt32(0x8), m_builder->getInt32(0));

    // (z0 < zNear * w0) ? 0x10 : 0
    clipMask[4] = m_builder->CreateFMul(zNear, w0);
    clipMask[4] = m_builder->CreateFCmpOLT(z0, clipMask[4]);
    clipMask[4] = m_builder->CreateSelect(clipMask[4], m_builder->getInt32(0x10), m_builder->getInt32(0));

    // (z0 > w0) ? 0x20 : 0
    clipMask[5] = m_builder->CreateFCmpOGT(z0, w0);
    clipMask[5] = m_builder->CreateSelect(clipMask[5], m_builder->getInt32(0x20), m_builder->getInt32(0));

    // clipMask0
    auto clipMaskX0 = m_builder->CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY0 = m_builder->CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ0 = m_builder->CreateOr(clipMask[4], clipMask[5]);
    auto clipMask0 = m_builder->CreateOr(clipMaskX0, clipMaskY0);
    clipMask0 = m_builder->CreateOr(clipMask0, clipMaskZ0);

    //
    // Get clip mask for vertex1
    //

    // (x1 < -xDiscAdj * w1) ? 0x1 : 0
    clipMask[0] = m_builder->CreateFMul(negXDiscAdj, w1);
    clipMask[0] = m_builder->CreateFCmpOLT(x1, clipMask[0]);
    clipMask[0] = m_builder->CreateSelect(clipMask[0], m_builder->getInt32(0x1), m_builder->getInt32(0));

    // (x1 > xDiscAdj * w1) ? 0x2 : 0
    clipMask[1] = m_builder->CreateFMul(xDiscAdj, w1);
    clipMask[1] = m_builder->CreateFCmpOGT(x1, clipMask[1]);
    clipMask[1] = m_builder->CreateSelect(clipMask[1], m_builder->getInt32(0x2), m_builder->getInt32(0));

    // (y1 < -yDiscAdj * w1) ? 0x4 : 0
    clipMask[2] = m_builder->CreateFMul(negYDiscAdj, w1);
    clipMask[2] = m_builder->CreateFCmpOLT(y1, clipMask[2]);
    clipMask[2] = m_builder->CreateSelect(clipMask[2], m_builder->getInt32(0x4), m_builder->getInt32(0));

    // (y1 > yDiscAdj * w1) ? 0x8 : 0
    clipMask[3] = m_builder->CreateFMul(yDiscAdj, w1);
    clipMask[3] = m_builder->CreateFCmpOGT(y1, clipMask[3]);
    clipMask[3] = m_builder->CreateSelect(clipMask[3], m_builder->getInt32(0x8), m_builder->getInt32(0));

    // (z1 < zNear * w1) ? 0x10 : 0
    clipMask[4] = m_builder->CreateFMul(zNear, w1);
    clipMask[4] = m_builder->CreateFCmpOLT(z1, clipMask[4]);
    clipMask[4] = m_builder->CreateSelect(clipMask[4], m_builder->getInt32(0x10), m_builder->getInt32(0));

    // (z1 > w1) ? 0x20 : 0
    clipMask[5] = m_builder->CreateFCmpOGT(z1, w1);
    clipMask[5] = m_builder->CreateSelect(clipMask[5], m_builder->getInt32(0x20), m_builder->getInt32(0));

    // clipMask1
    auto clipMaskX1 = m_builder->CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY1 = m_builder->CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ1 = m_builder->CreateOr(clipMask[4], clipMask[5]);
    auto clipMask1 = m_builder->CreateOr(clipMaskX1, clipMaskY1);
    clipMask1 = m_builder->CreateOr(clipMask1, clipMaskZ1);

    //
    // Get clip mask for vertex2
    //

    // (x2 < -xDiscAdj * w2) ? 0x1 : 0
    clipMask[0] = m_builder->CreateFMul(negXDiscAdj, w2);
    clipMask[0] = m_builder->CreateFCmpOLT(x2, clipMask[0]);
    clipMask[0] = m_builder->CreateSelect(clipMask[0], m_builder->getInt32(0x1), m_builder->getInt32(0));

    // (x2 > xDiscAdj * w2) ? 0x2 : 0
    clipMask[1] = m_builder->CreateFMul(xDiscAdj, w2);
    clipMask[1] = m_builder->CreateFCmpOGT(x2, clipMask[1]);
    clipMask[1] = m_builder->CreateSelect(clipMask[1], m_builder->getInt32(0x2), m_builder->getInt32(0));

    // (y2 < -yDiscAdj * w2) ? 0x4 : 0
    clipMask[2] = m_builder->CreateFMul(negYDiscAdj, w2);
    clipMask[2] = m_builder->CreateFCmpOLT(y2, clipMask[2]);
    clipMask[2] = m_builder->CreateSelect(clipMask[2], m_builder->getInt32(0x4), m_builder->getInt32(0));

    // (y2 > yDiscAdj * w2) ? 0x8 : 0
    clipMask[3] = m_builder->CreateFMul(yDiscAdj, w2);
    clipMask[3] = m_builder->CreateFCmpOGT(y2, clipMask[3]);
    clipMask[3] = m_builder->CreateSelect(clipMask[3], m_builder->getInt32(0x8), m_builder->getInt32(0));

    // (z2 < zNear * w2) ? 0x10 : 0
    clipMask[4] = m_builder->CreateFMul(zNear, w2);
    clipMask[4] = m_builder->CreateFCmpOLT(z2, clipMask[4]);
    clipMask[4] = m_builder->CreateSelect(clipMask[4], m_builder->getInt32(0x10), m_builder->getInt32(0));

    // (z2 > zFar * w2) ? 0x20 : 0
    clipMask[5] = m_builder->CreateFCmpOGT(z2, w2);
    clipMask[5] = m_builder->CreateSelect(clipMask[5], m_builder->getInt32(0x20), m_builder->getInt32(0));

    // clipMask2
    auto clipMaskX2 = m_builder->CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY2 = m_builder->CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ2 = m_builder->CreateOr(clipMask[4], clipMask[5]);
    auto clipMask2 = m_builder->CreateOr(clipMaskX2, clipMaskY2);
    clipMask2 = m_builder->CreateOr(clipMask2, clipMaskZ2);

    // clip = clipMask0 & clipMask1 & clipMask2
    auto clip = m_builder->CreateAnd(clipMask0, clipMask1);
    clip = m_builder->CreateAnd(clip, clipMask2);

    // cullFlag = (clip != 0)
    newCullFlag = m_builder->CreateICmpNE(clip, m_builder->getInt32(0));

    m_builder->CreateBr(frustumExitBlock);
  }

  // Construct ".frustumExit" block
  {
    m_builder->SetInsertPoint(frustumExitBlock);

    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag, frustumEntryBlock);
    cullFlagPhi->addIncoming(newCullFlag, frustumCullBlock);

    m_builder->CreateRet(cullFlagPhi);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that does box filter culling.
//
// @param module : LLVM module
Function *NggPrimShader::createBoxFilterCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                  {
                                      m_builder->getInt1Ty(),                           // %cullFlag
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                      m_builder->getInt32Ty(),                          // %paClVteCntl
                                      m_builder->getInt32Ty(),                          // %paClClipCntl
                                      m_builder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder->getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingBoxFilter, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadNone);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *cullFlag = argIt++;
  cullFlag->setName("cullFlag");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClVteCntl = argIt++;
  paClVteCntl->setName("paClVteCntl");

  Value *paClClipCntl = argIt++;
  paClVteCntl->setName("paClClipCntl");

  Value *paClGbHorzDiscAdj = argIt++;
  paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

  Value *paClGbVertDiscAdj = argIt++;
  paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

  auto boxFilterEntryBlock = createBlock(func, ".boxfilterEntry");
  auto boxFilterCullBlock = createBlock(func, ".boxfilterCull");
  auto boxFilterExitBlock = createBlock(func, ".boxfilterExit");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".boxfilterEntry" block
  {
    m_builder->SetInsertPoint(boxFilterEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder->CreateCondBr(cullFlag, boxFilterExitBlock, boxFilterCullBlock);
  }

  // Construct ".boxfilterCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder->SetInsertPoint(boxFilterCullBlock);

    //
    // Box filter culling algorithm is described as follow:
    //
    //   if (min(x0/w0, x1/w1, x2/w2) > xDiscAdj || max(x0/w0, x1/w1, x2/w2) < -xDiscAdj ||
    //       min(y0/w0, y1/w1, y2/w2) > yDiscAdj || max(y0/w0, y1/w1, y2/w2) < -yDiscAdj ||
    //       min(z0/w0, z1/w1, z2/w2) > zFar     || min(z0/w0, z1/w1, z2/w2) < zNear)
    //     cullFlag = true
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                 {paClVteCntl, m_builder->getInt32(8), m_builder->getInt32(1)});
    vtxXyFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

    // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    Value *vtxZFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {paClVteCntl, m_builder->getInt32(9), m_builder->getInt32(1)});
    vtxZFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                     {paClClipCntl, m_builder->getInt32(19), m_builder->getInt32(1)});
    clipSpaceDef = m_builder->CreateTrunc(clipSpaceDef, m_builder->getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    auto zNear = m_builder->CreateSelect(clipSpaceDef, ConstantFP::get(m_builder->getFloatTy(), -1.0),
                                         ConstantFP::get(m_builder->getFloatTy(), 0.0));
    auto zFar = ConstantFP::get(m_builder->getFloatTy(), 1.0);

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder->CreateBitCast(paClGbHorzDiscAdj, m_builder->getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder->CreateBitCast(paClGbVertDiscAdj, m_builder->getFloatTy());

    auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder->CreateExtractElement(vertex0, 1);
    auto z0 = m_builder->CreateExtractElement(vertex0, 2);
    auto w0 = m_builder->CreateExtractElement(vertex0, 3);

    auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder->CreateExtractElement(vertex1, 1);
    auto z1 = m_builder->CreateExtractElement(vertex1, 2);
    auto w1 = m_builder->CreateExtractElement(vertex1, 3);

    auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder->CreateExtractElement(vertex2, 1);
    auto z2 = m_builder->CreateExtractElement(vertex2, 2);
    auto w2 = m_builder->CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    auto rcpW0ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    auto rcpW1ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    auto rcpW2ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

    // VTX_Z_FMT ? 1.0 : 1 / w0
    auto rcpW0ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
    // VTX_Z_FMT ? 1.0 : 1 / w1
    auto rcpW1ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
    // VTX_Z_FMT ? 1.0 : 1 / w2
    auto rcpW2ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder->CreateFMul(x0, rcpW0ForXy);
    // y0' = y0/w0
    y0 = m_builder->CreateFMul(y0, rcpW0ForXy);
    // z0' = z0/w0
    z0 = m_builder->CreateFMul(z0, rcpW0ForZ);
    // x1' = x1/w1
    x1 = m_builder->CreateFMul(x1, rcpW1ForXy);
    // y1' = y1/w1
    y1 = m_builder->CreateFMul(y1, rcpW1ForXy);
    // z1' = z1/w1
    z1 = m_builder->CreateFMul(z1, rcpW1ForZ);
    // x2' = x2/w2
    x2 = m_builder->CreateFMul(x2, rcpW2ForXy);
    // y2' = y2/w2
    y2 = m_builder->CreateFMul(y2, rcpW2ForXy);
    // z2' = z2/w2
    z2 = m_builder->CreateFMul(z2, rcpW2ForZ);

    // -xDiscAdj
    auto negXDiscAdj = m_builder->CreateFNeg(xDiscAdj);

    // -yDiscAdj
    auto negYDiscAdj = m_builder->CreateFNeg(yDiscAdj);

    // minX = min(x0', x1', x2')
    auto minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {x0, x1});
    minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {minX, x2});

    // minX > xDiscAdj
    auto minXGtXDiscAdj = m_builder->CreateFCmpOGT(minX, xDiscAdj);

    // maxX = max(x0', x1', x2')
    auto maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {x0, x1});
    maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {maxX, x2});

    // maxX < -xDiscAdj
    auto maxXLtNegXDiscAdj = m_builder->CreateFCmpOLT(maxX, negXDiscAdj);

    // minY = min(y0', y1', y2')
    auto minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {y0, y1});
    minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {minY, y2});

    // minY > yDiscAdj
    auto minYGtYDiscAdj = m_builder->CreateFCmpOGT(minY, yDiscAdj);

    // maxY = max(y0', y1', y2')
    auto maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {y0, y1});
    maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {maxY, y2});

    // maxY < -yDiscAdj
    auto maxYLtNegYDiscAdj = m_builder->CreateFCmpOLT(maxY, negYDiscAdj);

    // minZ = min(z0', z1', z2')
    auto minZ = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {z0, z1});
    minZ = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {minZ, z2});

    // minZ > zFar (1.0)
    auto minZGtZFar = m_builder->CreateFCmpOGT(minZ, zFar);

    // maxZ = min(z0', z1', z2')
    auto maxZ = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {z0, z1});
    maxZ = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {maxZ, z2});

    // maxZ < zNear
    auto maxZLtZNear = m_builder->CreateFCmpOLT(maxZ, zNear);

    // Get cull flag
    auto cullX = m_builder->CreateOr(minXGtXDiscAdj, maxXLtNegXDiscAdj);
    auto cullY = m_builder->CreateOr(minYGtYDiscAdj, maxYLtNegYDiscAdj);
    auto cullZ = m_builder->CreateOr(minZGtZFar, maxZLtZNear);
    newCullFlag = m_builder->CreateOr(cullX, cullY);
    newCullFlag = m_builder->CreateOr(newCullFlag, cullZ);

    m_builder->CreateBr(boxFilterExitBlock);
  }

  // Construct ".boxfilterExit" block
  {
    m_builder->SetInsertPoint(boxFilterExitBlock);

    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag, boxFilterEntryBlock);
    cullFlagPhi->addIncoming(newCullFlag, boxFilterCullBlock);

    m_builder->CreateRet(cullFlagPhi);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that does sphere culling.
//
// @param module : LLVM module
Function *NggPrimShader::createSphereCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                  {
                                      m_builder->getInt1Ty(),                           // %cullFlag
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                      m_builder->getInt32Ty(),                          // %paClVteCntl
                                      m_builder->getInt32Ty(),                          // %paClClipCntl
                                      m_builder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder->getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingSphere, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadNone);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *cullFlag = argIt++;
  cullFlag->setName("cullFlag");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClVteCntl = argIt++;
  paClVteCntl->setName("paClVteCntl");

  Value *paClClipCntl = argIt++;
  paClVteCntl->setName("paClClipCntl");

  Value *paClGbHorzDiscAdj = argIt++;
  paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

  Value *paClGbVertDiscAdj = argIt++;
  paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

  auto sphereEntryBlock = createBlock(func, ".sphereEntry");
  auto sphereCullBlock = createBlock(func, ".sphereCull");
  auto sphereExitBlock = createBlock(func, ".sphereExit");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".sphereEntry" block
  {
    m_builder->SetInsertPoint(sphereEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder->CreateCondBr(cullFlag, sphereExitBlock, sphereCullBlock);
  }

  // Construct ".sphereCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder->SetInsertPoint(sphereCullBlock);

    //
    // Sphere culling algorithm is somewhat complex and is described as following steps:
    //   (1) Transform discard space to -1..1 space;
    //   (2) Project from 3D coordinates to barycentric coordinates;
    //   (3) Solve linear system and find barycentric coordinates of the point closest to the origin;
    //   (4) Do clamping for the closest point if necessary;
    //   (5) Backproject from barycentric coordinates to 3D coordinates;
    //   (6) Compute the distance squared from 3D coordinates of the closest point;
    //   (7) Compare the distance with 3.0 and determine the cull flag.
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                 {paClVteCntl, m_builder->getInt32(8), m_builder->getInt32(1)});
    vtxXyFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

    // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    Value *vtxZFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {paClVteCntl, m_builder->getInt32(9), m_builder->getInt32(1)});
    vtxZFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                     {paClClipCntl, m_builder->getInt32(19), m_builder->getInt32(1)});
    clipSpaceDef = m_builder->CreateTrunc(clipSpaceDef, m_builder->getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0
    auto zNear = m_builder->CreateSelect(clipSpaceDef, ConstantFP::get(m_builder->getFloatTy(), -1.0),
                                         ConstantFP::get(m_builder->getFloatTy(), 0.0));

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder->CreateBitCast(paClGbHorzDiscAdj, m_builder->getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder->CreateBitCast(paClGbVertDiscAdj, m_builder->getFloatTy());

    auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder->CreateExtractElement(vertex0, 1);
    auto z0 = m_builder->CreateExtractElement(vertex0, 2);
    auto w0 = m_builder->CreateExtractElement(vertex0, 3);

    auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder->CreateExtractElement(vertex1, 1);
    auto z1 = m_builder->CreateExtractElement(vertex1, 2);
    auto w1 = m_builder->CreateExtractElement(vertex1, 3);

    auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder->CreateExtractElement(vertex2, 1);
    auto z2 = m_builder->CreateExtractElement(vertex2, 2);
    auto w2 = m_builder->CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    auto rcpW0ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    auto rcpW1ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    auto rcpW2ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

    // VTX_Z_FMT ? 1.0 : 1 / w0
    auto rcpW0ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
    // VTX_Z_FMT ? 1.0 : 1 / w1
    auto rcpW1ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
    // VTX_Z_FMT ? 1.0 : 1 / w2
    auto rcpW2ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder->CreateFMul(x0, rcpW0ForXy);
    // y0' = y0/w0
    y0 = m_builder->CreateFMul(y0, rcpW0ForXy);
    // z0' = z0/w0
    z0 = m_builder->CreateFMul(z0, rcpW0ForZ);
    // x1' = x1/w1
    x1 = m_builder->CreateFMul(x1, rcpW1ForXy);
    // y1' = y1/w1
    y1 = m_builder->CreateFMul(y1, rcpW1ForXy);
    // z1' = z1/w1
    z1 = m_builder->CreateFMul(z1, rcpW1ForZ);
    // x2' = x2/w2
    x2 = m_builder->CreateFMul(x2, rcpW2ForXy);
    // y2' = y2/w2
    y2 = m_builder->CreateFMul(y2, rcpW2ForXy);
    // z2' = z2/w2
    z2 = m_builder->CreateFMul(z2, rcpW2ForZ);

    //
    // === Step 1 ===: Discard space to -1..1 space.
    //

    // x" = x'/xDiscAdj
    // y" = y'/yDiscAdj
    // z" = (zNear + 2.0)z' + (-1.0 - zNear)
    auto rcpXDiscAdj = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), xDiscAdj);
    auto rcpYDiscAdj = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), yDiscAdj);
    auto rcpXyDiscAdj = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {rcpXDiscAdj, rcpYDiscAdj});

    Value *x0Y0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x0, y0});
    Value *x1Y1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x1, y1});
    Value *x2Y2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x2, y2});

    x0Y0 = m_builder->CreateFMul(x0Y0, rcpXyDiscAdj);
    x1Y1 = m_builder->CreateFMul(x1Y1, rcpXyDiscAdj);
    x2Y2 = m_builder->CreateFMul(x2Y2, rcpXyDiscAdj);

    // zNear + 2.0
    auto zNearPlusTwo = m_builder->CreateFAdd(zNear, ConstantFP::get(m_builder->getFloatTy(), 2.0));
    zNearPlusTwo = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {zNearPlusTwo, zNearPlusTwo});

    // -1.0 - zNear
    auto negOneMinusZNear = m_builder->CreateFSub(ConstantFP::get(m_builder->getFloatTy(), -1.0), zNear);
    negOneMinusZNear =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {negOneMinusZNear, negOneMinusZNear});

    Value *z0Z0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {z0, z0});
    Value *z2Z1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {z2, z1});

    z0Z0 = m_builder->CreateIntrinsic(Intrinsic::fma, VectorType::get(Type::getHalfTy(*m_context), 2),
                                      {zNearPlusTwo, z0Z0, negOneMinusZNear});
    z2Z1 = m_builder->CreateIntrinsic(Intrinsic::fma, VectorType::get(Type::getHalfTy(*m_context), 2),
                                      {zNearPlusTwo, z2Z1, negOneMinusZNear});

    //
    // === Step 2 ===: 3D coordinates to barycentric coordinates.
    //

    // <x20, y20> = <x2", y2"> - <x0", y0">
    auto x20Y20 = m_builder->CreateFSub(x2Y2, x0Y0);

    // <x10, y10> = <x1", y1"> - <x0", y0">
    auto x10Y10 = m_builder->CreateFSub(x1Y1, x0Y0);

    // <z20, z10> = <z2", z1"> - <z0", z0">
    auto z20Z10 = m_builder->CreateFSub(z2Z1, z0Z0);

    //
    // === Step 3 ===: Solve linear system and find the point closest to the origin.
    //

    // a00 = x10 + z10
    auto x10 = m_builder->CreateExtractElement(x10Y10, static_cast<uint64_t>(0));
    auto z10 = m_builder->CreateExtractElement(z20Z10, 1);
    auto a00 = m_builder->CreateFAdd(x10, z10);

    // a01 = x20 + z20
    auto x20 = m_builder->CreateExtractElement(x20Y20, static_cast<uint64_t>(0));
    auto z20 = m_builder->CreateExtractElement(z20Z10, static_cast<uint64_t>(0));
    auto a01 = m_builder->CreateFAdd(x20, z20);

    // a10 = y10 + y10
    auto y10 = m_builder->CreateExtractElement(x10Y10, 1);
    auto a10 = m_builder->CreateFAdd(y10, y10);

    // a11 = y20 + z20
    auto y20 = m_builder->CreateExtractElement(x20Y20, 1);
    auto a11 = m_builder->CreateFAdd(y20, z20);

    // b0 = -x0" - x2"
    x0 = m_builder->CreateExtractElement(x0Y0, static_cast<uint64_t>(0));
    auto negX0 = m_builder->CreateFNeg(x0);
    x2 = m_builder->CreateExtractElement(x2Y2, static_cast<uint64_t>(0));
    auto b0 = m_builder->CreateFSub(negX0, x2);

    // b1 = -x1" - x2"
    x1 = m_builder->CreateExtractElement(x1Y1, static_cast<uint64_t>(0));
    auto negX1 = m_builder->CreateFNeg(x1);
    auto b1 = m_builder->CreateFSub(negX1, x2);

    //     [ a00 a01 ]      [ b0 ]       [ s ]
    // A = [         ], B = [    ], ST = [   ], A * ST = B (crame rules)
    //     [ a10 a11 ]      [ b1 ]       [ t ]

    //           | a00 a01 |
    // det(A) =  |         | = a00 * a11 - a01 * a10
    //           | a10 a11 |
    auto detA = m_builder->CreateFMul(a00, a11);
    auto negA01 = m_builder->CreateFNeg(a01);
    detA = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {negA01, a10, detA});

    //            | b0 a01 |
    // det(Ab0) = |        | = b0 * a11 - a01 * b1
    //            | b1 a11 |
    auto detAB0 = m_builder->CreateFMul(b0, a11);
    detAB0 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {negA01, b1, detAB0});

    //            | a00 b0 |
    // det(Ab1) = |        | = a00 * b1 - b0 * a10
    //            | a10 b1 |
    auto detAB1 = m_builder->CreateFMul(a00, b1);
    auto negB0 = m_builder->CreateFNeg(b0);
    detAB1 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {negB0, a10, detAB1});

    // s = det(Ab0) / det(A)
    auto rcpDetA = m_builder->CreateFDiv(ConstantFP::get(m_builder->getHalfTy(), 1.0), detA);
    auto s = m_builder->CreateFMul(detAB0, rcpDetA);

    // t = det(Ab1) / det(A)
    auto t = m_builder->CreateFMul(detAB1, rcpDetA);

    //
    // === Step 4 ===: Do clamping for the closest point.
    //

    // <s, t>
    auto st = m_builder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getHalfTy(*m_context), 2)), s,
                                             static_cast<uint64_t>(0));
    st = m_builder->CreateInsertElement(st, t, 1);

    // <s', t'> = <0.5 - 0.5(t - s), 0.5 + 0.5(t - s)>
    auto tMinusS = m_builder->CreateFSub(t, s);
    auto sT1 = m_builder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getHalfTy(*m_context), 2)), tMinusS,
                                              static_cast<uint64_t>(0));
    sT1 = m_builder->CreateInsertElement(sT1, tMinusS, 1);

    sT1 = m_builder->CreateIntrinsic(Intrinsic::fma, VectorType::get(Type::getHalfTy(*m_context), 2),
                                     {ConstantVector::get({ConstantFP::get(m_builder->getHalfTy(), -0.5),
                                                           ConstantFP::get(m_builder->getHalfTy(), 0.5)}),
                                      sT1,
                                      ConstantVector::get({ConstantFP::get(m_builder->getHalfTy(), 0.5),
                                                           ConstantFP::get(m_builder->getHalfTy(), 0.5)})});

    // <s", t"> = clamp(<s, t>)
    auto sT2 = m_builder->CreateIntrinsic(Intrinsic::maxnum, VectorType::get(Type::getHalfTy(*m_context), 2),
                                          {st, ConstantVector::get({ConstantFP::get(m_builder->getHalfTy(), 0.0),
                                                                    ConstantFP::get(m_builder->getHalfTy(), 0.0)})});
    sT2 = m_builder->CreateIntrinsic(Intrinsic::minnum, VectorType::get(Type::getHalfTy(*m_context), 2),
                                     {sT2, ConstantVector::get({ConstantFP::get(m_builder->getHalfTy(), 1.0),
                                                                ConstantFP::get(m_builder->getHalfTy(), 1.0)})});

    // <s, t> = (s + t) > 1.0 ? <s', t'> : <s", t">
    auto sPlusT = m_builder->CreateFAdd(s, t);
    auto sPlusTGtOne = m_builder->CreateFCmpOGT(sPlusT, ConstantFP::get(m_builder->getHalfTy(), 1.0));
    st = m_builder->CreateSelect(sPlusTGtOne, sT1, sT2);

    //
    // === Step 5 ===: Barycentric coordinates to 3D coordinates.
    //

    // x = x0" + s * x10 + t * x20
    // y = y0" + s * y10 + t * y20
    // z = z0" + s * z10 + t * z20
    s = m_builder->CreateExtractElement(st, static_cast<uint64_t>(0));
    t = m_builder->CreateExtractElement(st, 1);
    auto ss = m_builder->CreateInsertElement(st, s, 1);
    auto tt = m_builder->CreateInsertElement(st, t, static_cast<uint64_t>(0));

    // s * <x10, y10> + <x0", y0">
    auto xy =
        m_builder->CreateIntrinsic(Intrinsic::fma, VectorType::get(Type::getHalfTy(*m_context), 2), {ss, x10Y10, x0Y0});

    // <x, y> = t * <x20, y20> + (s * <x10, y10> + <x0", y0">)
    xy = m_builder->CreateIntrinsic(Intrinsic::fma, VectorType::get(Type::getHalfTy(*m_context), 2), {tt, x20Y20, xy});

    // s * z10 + z0"
    z0 = m_builder->CreateExtractElement(z0Z0, static_cast<uint64_t>(0));
    auto z = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {s, z10, z0});

    // z = t * z20 + (s * z10 + z0")
    z = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {t, z20, z});

    auto x = m_builder->CreateExtractElement(xy, static_cast<uint64_t>(0));
    auto y = m_builder->CreateExtractElement(xy, 1);

    //
    // === Step 6 ===: Compute the distance squared of the closest point.
    //

    // r^2 = x^2 + y^2 + z^2
    auto squareR = m_builder->CreateFMul(x, x);
    squareR = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {y, y, squareR});
    squareR = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), {z, z, squareR});

    //
    // == = Step 7 == = : Determine the cull flag
    //

    // cullFlag = (r ^ 2 > 3.0)
    newCullFlag = m_builder->CreateFCmpOGT(squareR, ConstantFP::get(m_builder->getHalfTy(), 3.0));

    m_builder->CreateBr(sphereExitBlock);
  }

  // Construct ".sphereExit" block
  {
    m_builder->SetInsertPoint(sphereExitBlock);

    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag, sphereEntryBlock);
    cullFlagPhi->addIncoming(newCullFlag, sphereCullBlock);

    m_builder->CreateRet(cullFlagPhi);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that does small primitive filter culling.
//
// @param module : LLVM module
Function *NggPrimShader::createSmallPrimFilterCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                  {
                                      m_builder->getInt1Ty(),                           // %cullFlag
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                      VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                      m_builder->getInt32Ty(),                          // %paClVteCntl
                                      m_builder->getInt32Ty(),                          // %paClVportXscale
                                      m_builder->getInt32Ty(),                          // %paClVportXoffset
                                      m_builder->getInt32Ty(),                          // %paClVportYscale
                                      m_builder->getInt32Ty(),                          // %paClVportYoffset
                                      m_builder->getInt1Ty()                            // %conservativeRaster
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingSmallPrimFilter, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadNone);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *cullFlag = argIt++;
  cullFlag->setName("cullFlag");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClVteCntl = argIt++;
  paClVteCntl->setName("paClVteCntl");

  Value *paClVportXscale = argIt++;
  paClVportXscale->setName("paClVportXscale");

  Value *paClVportXoffset = argIt++;
  paClVportXscale->setName("paClVportXoffset");

  Value *paClVportYscale = argIt++;
  paClVportYscale->setName("paClVportYscale");

  Value *paClVportYoffset = argIt++;
  paClVportYscale->setName("paClVportYoffset");

  Value *conservativeRaster = argIt++;
  conservativeRaster->setName("conservativeRaster");

  auto smallPrimFilterEntryBlock = createBlock(func, ".smallprimfilterEntry");
  auto smallPrimFilterCullBlock = createBlock(func, ".smallprimfilterCull");
  auto smallPrimFilterExitBlock = createBlock(func, ".smallprimfilterExit");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".smallprimfilterEntry" block
  {
    m_builder->SetInsertPoint(smallPrimFilterEntryBlock);

    // If cull flag has already been TRUE or if conservative rasterization, early return
    m_builder->CreateCondBr(m_builder->CreateOr(cullFlag, conservativeRaster), smallPrimFilterExitBlock,
                            smallPrimFilterCullBlock);
  }

  // Construct ".smallprimfilterCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder->SetInsertPoint(smallPrimFilterCullBlock);

    //
    // Small primitive filter culling algorithm is described as follow:
    //
    //   if (!conservativeRaster) {
    //     if (roundEven(min(screen(x0/w0), screen(x1/w1), screen(x2/w2)) ==
    //         roundEven(max(screen(x0/w0), screen(x1/w1), screen(x2/w2))) ||
    //         roundEven(min(screen(y0/w0), screen(y1/w1), screen(y2/w2)) ==
    //         roundEven(max(screen(y0/w0), screen(y1/w1), screen(y2/w2))))
    //       cullFlag = true
    //
    //     allowCull = (w0 < 0 && w1 < 0 && w2 < 0) || (w0 > 0 && w1 > 0 && w2 > 0))
    //     cullFlag = allowCull && cullFlag
    //   } else
    //     cullFlag = false
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                 {paClVteCntl, m_builder->getInt32(8), m_builder->getInt32(1)});
    vtxXyFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

    // xScale = (VPORT_XSCALE, PA_CL_VPORT_XSCALE[31:0])
    auto xScale = m_builder->CreateBitCast(paClVportXscale, m_builder->getFloatTy());

    // xOffset = (VPORT_XOFFSET, PA_CL_VPORT_XOFFSET[31:0])
    auto xOffset = m_builder->CreateBitCast(paClVportXoffset, m_builder->getFloatTy());

    // yScale = (VPORT_YSCALE, PA_CL_VPORT_YSCALE[31:0])
    auto yScale = m_builder->CreateBitCast(paClVportYscale, m_builder->getFloatTy());

    // yOffset = (VPORT_YOFFSET, PA_CL_VPORT_YOFFSET[31:0])
    auto yOffset = m_builder->CreateBitCast(paClVportYoffset, m_builder->getFloatTy());

    auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder->CreateExtractElement(vertex0, 1);
    auto w0 = m_builder->CreateExtractElement(vertex0, 3);

    auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder->CreateExtractElement(vertex1, 1);
    auto w1 = m_builder->CreateExtractElement(vertex1, 3);

    auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder->CreateExtractElement(vertex2, 1);
    auto w2 = m_builder->CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    rcpW0 = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    rcpW1 = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    rcpW2 = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder->CreateFMul(x0, rcpW0);
    // y0' = y0/w0
    y0 = m_builder->CreateFMul(y0, rcpW0);
    // x1' = x1/w1
    x1 = m_builder->CreateFMul(x1, rcpW1);
    // y1' = y1/w1
    y1 = m_builder->CreateFMul(y1, rcpW1);
    // x2' = x2/w2
    x2 = m_builder->CreateFMul(x2, rcpW2);
    // y2' = y2/w2
    y2 = m_builder->CreateFMul(y2, rcpW2);

    // screenMinX = -xScale + xOffset - 0.5
    auto screenMinX = m_builder->CreateFAdd(m_builder->CreateFNeg(xScale), xOffset);
    screenMinX = m_builder->CreateFAdd(screenMinX, ConstantFP::get(m_builder->getFloatTy(), -0.5));

    // screenMaxX = xScale + xOffset + 0.5
    auto screenMaxX = m_builder->CreateFAdd(xScale, xOffset);
    screenMaxX = m_builder->CreateFAdd(screenMaxX, ConstantFP::get(m_builder->getFloatTy(), 0.5));

    // screenX0' = x0' * xScale + xOffset
    auto screenX0 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getFloatTy(), {x0, xScale, xOffset});

    // screenX1' = x1' * xScale + xOffset
    auto screenX1 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getFloatTy(), {x1, xScale, xOffset});

    // screenX2' = x2' * xScale + xOffset
    auto screenX2 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getFloatTy(), {x2, xScale, xOffset});

    // minX = clamp(min(screenX0', screenX1', screenX2'), screenMinX, screenMaxX) - 1/256.0
    Value *minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {screenX0, screenX1});
    minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {minX, screenX2});
    minX = m_builder->CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder->getFloatTy(), {screenMinX, minX, screenMaxX});
    minX = m_builder->CreateFAdd(minX, ConstantFP::get(m_builder->getFloatTy(), -1 / 256.0));

    // minX = roundEven(minX)
    minX = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), minX);

    // maxX = clamp(max(screenX0', screenX1', screenX2'), screenMinX, screenMaxX) + 1/256.0
    Value *maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {screenX0, screenX1});
    maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {maxX, screenX2});
    maxX = m_builder->CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder->getFloatTy(), {screenMinX, maxX, screenMaxX});
    maxX = m_builder->CreateFAdd(maxX, ConstantFP::get(m_builder->getFloatTy(), 1 / 256.0));

    // maxX = roundEven(maxX)
    maxX = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), maxX);

    // screenMinY = -yScale + yOffset - 0.5
    auto screenMinY = m_builder->CreateFAdd(m_builder->CreateFNeg(yScale), yOffset);
    screenMinY = m_builder->CreateFAdd(screenMinY, ConstantFP::get(m_builder->getFloatTy(), -0.5));

    // screenMaxY = yScale + yOffset + 0.5
    auto screenMaxY = m_builder->CreateFAdd(yScale, yOffset);
    screenMaxY = m_builder->CreateFAdd(screenMaxX, ConstantFP::get(m_builder->getFloatTy(), 0.5));

    // screenY0' = y0' * yScale + yOffset
    auto screenY0 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getFloatTy(), {y0, yScale, yOffset});

    // screenY1' = y1' * yScale + yOffset
    auto screenY1 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getFloatTy(), {y1, yScale, yOffset});

    // screenY2' = y2' * yScale + yOffset
    auto screenY2 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getFloatTy(), {y2, yScale, yOffset});

    // minY = clamp(min(screenY0', screenY1', screenY2'), screenMinY, screenMaxY) - 1/256.0
    Value *minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {screenY0, screenY1});
    minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), {minY, screenY2});
    minY = m_builder->CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder->getFloatTy(), {screenMinY, minY, screenMaxY});
    minY = m_builder->CreateFAdd(minY, ConstantFP::get(m_builder->getFloatTy(), -1 / 256.0));

    // minY = roundEven(minY)
    minY = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), minY);

    // maxY = clamp(max(screenX0', screenY1', screenY2'), screenMinY, screenMaxY) + 1/256.0
    Value *maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {screenY0, screenY1});
    maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), {maxY, screenY2});
    maxY = m_builder->CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder->getFloatTy(), {screenMinY, maxY, screenMaxY});
    maxY = m_builder->CreateFAdd(maxY, ConstantFP::get(m_builder->getFloatTy(), 1 / 256.0));

    // maxY = roundEven(maxY)
    maxY = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), maxY);

    // minX == maxX
    auto minXEqMaxX = m_builder->CreateFCmpOEQ(minX, maxX);

    // minY == maxY
    auto minYEqMaxY = m_builder->CreateFCmpOEQ(minY, maxY);

    // Get cull flag
    newCullFlag = m_builder->CreateOr(minXEqMaxX, minYEqMaxY);

    // Check if W allows culling
    auto w0AsInt = m_builder->CreateBitCast(w0, m_builder->getInt32Ty());
    auto w1AsInt = m_builder->CreateBitCast(w1, m_builder->getInt32Ty());
    auto w2AsInt = m_builder->CreateBitCast(w2, m_builder->getInt32Ty());

    // w0 < 0 && w1 < 0 && w2 < 0
    auto isAllWNeg = m_builder->CreateAnd(w0AsInt, w1AsInt);
    isAllWNeg = m_builder->CreateAnd(isAllWNeg, w2AsInt);
    isAllWNeg = m_builder->CreateICmpSLT(isAllWNeg, m_builder->getInt32(0));

    // w0 > 0 && w1 > 0 && w2 > 0
    auto isAllWPos = m_builder->CreateOr(w0AsInt, w1AsInt);
    isAllWPos = m_builder->CreateOr(isAllWPos, w2AsInt);
    isAllWPos = m_builder->CreateICmpSGT(isAllWPos, m_builder->getInt32(0));

    auto allowCull = m_builder->CreateOr(isAllWNeg, isAllWPos);
    newCullFlag = m_builder->CreateAnd(allowCull, newCullFlag);

    m_builder->CreateBr(smallPrimFilterExitBlock);
  }

  // Construct ".smallprimfilterExit" block
  {
    m_builder->SetInsertPoint(smallPrimFilterExitBlock);

    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag, smallPrimFilterEntryBlock);
    cullFlagPhi->addIncoming(newCullFlag, smallPrimFilterCullBlock);

    m_builder->CreateRet(cullFlagPhi);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
//
// @param module : LLVM module
Function *NggPrimShader::createCullDistanceCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                  {
                                      m_builder->getInt1Ty(),  // %cullFlag
                                      m_builder->getInt32Ty(), // %signMask0
                                      m_builder->getInt32Ty(), // %signMask1
                                      m_builder->getInt32Ty()  // %signMask2
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingCullDistance, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadNone);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *cullFlag = argIt++;
  cullFlag->setName("cullFlag");

  Value *signMask0 = argIt++;
  signMask0->setName("signMask0");

  Value *signMask1 = argIt++;
  signMask1->setName("signMask1");

  Value *signMask2 = argIt++;
  signMask2->setName("signMask2");

  auto cullDistanceEntryBlock = createBlock(func, ".culldistanceEntry");
  auto cullDistanceCullBlock = createBlock(func, ".culldistanceCull");
  auto cullDistanceExitBlock = createBlock(func, ".culldistanceExit");

  auto savedInsertPoint = m_builder->saveIP();

  // Construct ".culldistanceEntry" block
  {
    m_builder->SetInsertPoint(cullDistanceEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder->CreateCondBr(cullFlag, cullDistanceExitBlock, cullDistanceCullBlock);
  }

  // Construct ".culldistanceCull" block
  Value *cullFlag1 = nullptr;
  {
    m_builder->SetInsertPoint(cullDistanceCullBlock);

    //
    // Cull distance culling algorithm is described as follow:
    //
    //   vertexSignMask[7:0] = [sign(ClipDistance[0])..sign(ClipDistance[7])]
    //   primSignMask = vertexSignMask0 & vertexSignMask1 & vertexSignMask2
    //   cullFlag = (primSignMask != 0)
    //
    auto signMask = m_builder->CreateAnd(signMask0, signMask1);
    signMask = m_builder->CreateAnd(signMask, signMask2);

    cullFlag1 = m_builder->CreateICmpNE(signMask, m_builder->getInt32(0));

    m_builder->CreateBr(cullDistanceExitBlock);
  }

  // Construct ".culldistanceExit" block
  {
    m_builder->SetInsertPoint(cullDistanceExitBlock);

    auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
    cullFlagPhi->addIncoming(cullFlag, cullDistanceEntryBlock);
    cullFlagPhi->addIncoming(cullFlag1, cullDistanceCullBlock);

    m_builder->CreateRet(cullFlagPhi);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Creates the function that fetches culling control registers.
//
// @param module : LLVM module
Function *NggPrimShader::createFetchCullingRegister(Module *module) {
  auto funcTy = FunctionType::get(m_builder->getInt32Ty(),
                                  {
                                      m_builder->getInt32Ty(), // %primShaderTableAddrLow
                                      m_builder->getInt32Ty(), // %primShaderTableAddrHigh
                                      m_builder->getInt32Ty()  // %regOffset
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingFetchReg, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::ReadOnly);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primShaderTableAddrLow = argIt++;
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = argIt++;
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  Value *regOffset = argIt++;
  regOffset->setName("regOffset");

  BasicBlock *entryBlock = createBlock(func); // Create entry block

  auto savedInsertPoint = m_builder->saveIP();

  // Construct entry block
  {
    m_builder->SetInsertPoint(entryBlock);

    Value *primShaderTableAddr =
        m_builder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getInt32Ty(*m_context), 2)),
                                       primShaderTableAddrLow, static_cast<uint64_t>(0));

    primShaderTableAddr = m_builder->CreateInsertElement(primShaderTableAddr, primShaderTableAddrHigh, 1);

    primShaderTableAddr = m_builder->CreateBitCast(primShaderTableAddr, m_builder->getInt64Ty());

    auto primShaderTablePtrTy = PointerType::get(ArrayType::get(m_builder->getInt32Ty(), 256),
                                                 ADDR_SPACE_CONST); // [256 x i32]
    auto primShaderTablePtr = m_builder->CreateIntToPtr(primShaderTableAddr, primShaderTablePtrTy);

    // regOffset = regOffset >> 2
    regOffset = m_builder->CreateLShr(regOffset, 2); // To dword offset

    auto loadPtr = m_builder->CreateGEP(primShaderTablePtr, {m_builder->getInt32(0), regOffset});
    cast<Instruction>(loadPtr)->setMetadata(MetaNameUniform, MDNode::get(m_builder->getContext(), {}));

    auto regValue = m_builder->CreateAlignedLoad(loadPtr, Align(4), /*volatile=*/true);
    regValue->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder->getContext(), {}));

    m_builder->CreateRet(regValue);
  }

  m_builder->restoreIP(savedInsertPoint);

  return func;
}

// =====================================================================================================================
// Output a subgroup ballot (always return i64 mask)
//
// @param value : The value to do the ballot on.
Value *NggPrimShader::doSubgroupBallot(Value *value) {
  assert(value->getType()->isIntegerTy(1)); // Should be i1

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  value = m_builder->CreateSelect(value, m_builder->getInt32(1), m_builder->getInt32(0));

  auto inlineAsmTy = FunctionType::get(m_builder->getInt32Ty(), m_builder->getInt32Ty(), false);
  auto inlineAsm = InlineAsm::get(inlineAsmTy, "; %1", "=v,0", true);
  value = m_builder->CreateCall(inlineAsm, value);

  static const unsigned PredicateNE = 33; // 33 = predicate NE
  Value *ballot = m_builder->CreateIntrinsic(Intrinsic::amdgcn_icmp,
                                             {
                                                 m_builder->getIntNTy(waveSize), // Return type
                                                 m_builder->getInt32Ty()         // Argument type
                                             },
                                             {value, m_builder->getInt32(0), m_builder->getInt32(PredicateNE)});

  if (waveSize == 32)
    ballot = m_builder->CreateZExt(ballot, m_builder->getInt64Ty());

  return ballot;
}

// =====================================================================================================================
// Gets the count of GS output vertices per primitive
unsigned NggPrimShader::getOutputVerticesPerPrimitive() const {
  assert(m_hasGs);
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

  unsigned outVertsPerPrim = 0;
  switch (geometryMode.outputPrimitive) {
  case OutputPrimitives::Points:
    outVertsPerPrim = 1;
    break;
  case OutputPrimitives::LineStrip:
    outVertsPerPrim = 2;
    break;
  case OutputPrimitives::TriangleStrip:
    outVertsPerPrim = 3;
    break;
  default:
    llvm_unreachable("Unexpected output primitive type!");
    break;
  }

  return outVertsPerPrim;
}

// =====================================================================================================================
// Fetches the position data for the specified vertex ID.
//
// @param vertexId : Vertex thread ID in sub-group.
Value *NggPrimShader::fetchVertexPositionData(Value *vertexId) {
  if (!m_hasGs) {
    // ES-only
    return readPerThreadDataFromLds(VectorType::get(m_builder->getFloatTy(), 4), vertexId, LdsRegionPosData);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInPosition];
  const unsigned rasterStream = inOutUsage.gs.rasterStream;
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexId);

  return importGsOutput(VectorType::get(m_builder->getFloatTy(), 4), loc, 0, rasterStream, vertexOffset);
}

// =====================================================================================================================
// Fetches the aggregated sign mask of cull distances for the specified vertex ID.
//
// @param vertexId : Vertex thread ID in sub-group.
Value *NggPrimShader::fetchCullDistanceSignMask(Value *vertexId) {
  assert(m_nggControl->enableCullDistanceCulling);

  if (!m_hasGs) {
    // ES-only
    return readPerThreadDataFromLds(m_builder->getInt32Ty(), vertexId, LdsRegionCullDistance);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
  const unsigned rasterStream = inOutUsage.gs.rasterStream;
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexId);

  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;
  auto cullDistances = importGsOutput(ArrayType::get(m_builder->getFloatTy(), builtInUsage.cullDistance), loc, 0,
                                      rasterStream, vertexOffset);

  // Calculate the sign mask for all cull distances
  Value *signMask = m_builder->getInt32(0);
  for (unsigned i = 0; i < builtInUsage.cullDistance; ++i) {
    auto cullDistance = m_builder->CreateExtractValue(cullDistances, i);
    cullDistance = m_builder->CreateBitCast(cullDistance, m_builder->getInt32Ty());

    Value *signBit = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
                                                {cullDistance, m_builder->getInt32(31), m_builder->getInt32(1)});
    signBit = m_builder->CreateShl(signBit, i);
    signMask = m_builder->CreateOr(signMask, signBit);
  }

  return signMask;
}

// =====================================================================================================================
// Calculates the starting LDS offset (in bytes) of vertex item data in GS-VS ring.
//
// @param streamId : ID of output vertex stream.
// @param vertexId : Vertex thread ID in sub-group.
Value *NggPrimShader::calcVertexItemOffset(unsigned streamId, Value *vertexId) {
  assert(m_hasGs); // GS must be present

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

  // vertexOffset = gsVsRingStart + (streamBases[stream] + vertexId * vertexItemSize) * 4 (in bytes)
  const unsigned vertexItemSize = 4 * inOutUsage.gs.outLocCount[streamId];
  auto vertexOffset = m_builder->CreateMul(vertexId, m_builder->getInt32(vertexItemSize));
  vertexOffset = m_builder->CreateAdd(vertexOffset, m_builder->getInt32(m_gsStreamBases[streamId]));
  vertexOffset = m_builder->CreateShl(vertexOffset, 2);

  const unsigned gsVsRingStart = m_ldsManager->getLdsRegionStart(LdsRegionGsVsRing);
  vertexOffset = m_builder->CreateAdd(vertexOffset, m_builder->getInt32(gsVsRingStart));

  return vertexOffset;
}

// =====================================================================================================================
// Creates a new basic block. Always insert it at the end of the parent function.
//
// @param parent : Parent function to which the new block belongs
// @param blockName : Name of the new block
BasicBlock *NggPrimShader::createBlock(Function *parent, const Twine &blockName) {
  return BasicBlock::Create(*m_context, blockName, parent);
}

} // namespace lgc
