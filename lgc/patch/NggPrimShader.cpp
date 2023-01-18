/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/state/PalMetadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "lgc-ngg-prim-shader"

using namespace llvm;

// -ngg-small-subgroup-threshold: threshold of vertex count to determine a small subgroup (NGG)
static cl::opt<unsigned> NggSmallSubgroupThreshold(
    "ngg-small-subgroup-threshold",
    cl::desc(
        "Threshold of vertex count to determine a small subgroup and such small subgroup won't perform NGG culling"),
    cl::value_desc("threshold"), cl::init(16));

namespace lgc {

// Represents GDS GRBM register for SW-emulated stream-out
enum {
  // For 4 stream-out buffers
  GDS_STRMOUT_DWORDS_WRITTEN_0 = 0,
  GDS_STRMOUT_DWORDS_WRITTEN_1 = 1,
  GDS_STRMOUT_DWORDS_WRITTEN_2 = 2,
  GDS_STRMOUT_DWORDS_WRITTEN_3 = 3,
  // For 4 stream-out streams
  GDS_STRMOUT_PRIMS_NEEDED_0 = 8,
  GDS_STRMOUT_PRIMS_WRITTEN_0 = 9,
  GDS_STRMOUT_PRIMS_NEEDED_1 = 10,
  GDS_STRMOUT_PRIMS_WRITTEN_1 = 11,
  GDS_STRMOUT_PRIMS_NEEDED_2 = 12,
  GDS_STRMOUT_PRIMS_WRITTEN_2 = 13,
  GDS_STRMOUT_PRIMS_NEEDED_3 = 14,
  GDS_STRMOUT_PRIMS_WRITTEN_3 = 15,
};

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
NggPrimShader::NggPrimShader(PipelineState *pipelineState)
    : m_pipelineState(pipelineState), m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()),
      m_nggControl(m_pipelineState->getNggControl()), m_hasVs(pipelineState->hasShaderStage(ShaderStageVertex)),
      m_hasTes(pipelineState->hasShaderStage(ShaderStageTessEval)),
      m_hasGs(pipelineState->hasShaderStage(ShaderStageGeometry)), m_builder(pipelineState->getContext()) {
  assert(m_nggControl->enableNgg);

  // Always allow approximation, to change fdiv(1.0, x) to rcp(x)
  FastMathFlags fastMathFlags;
  fastMathFlags.setApproxFunc();
  m_builder.setFastMathFlags(fastMathFlags);

  assert(m_pipelineState->isGraphics());

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
  }

  buildPrimShaderCbLayoutLookupTable();
  calcVertexCullInfoSizeAndOffsets(m_pipelineState, m_vertCullInfoOffsets);
}

// =====================================================================================================================
NggPrimShader::~NggPrimShader() {
  if (m_ldsManager)
    delete m_ldsManager;
}

// =====================================================================================================================
// Calculates the dword size of ES-GS ring item.
//
// @param pipelineState : Pipeline state
unsigned NggPrimShader::calcEsGsRingItemSize(PipelineState *pipelineState) {
  assert(pipelineState->getNggControl()->enableNgg);

  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  if (hasGs) {
    auto resUsage = pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
    return (4 * std::max(1u, resUsage->inOutUsage.inputMapLocCount)) | 1;
  }

  if (pipelineState->getNggControl()->passthroughMode) {
    unsigned esGsRingItemSize = 1;

    if (pipelineState->enableSwXfb()) {
      const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
      auto resUsage = pipelineState->getShaderResourceUsage(hasTes ? ShaderStageTessEval : ShaderStageVertex);

      // NOTE: For GFX11+, transform feedback outputs (each output is <4 x dword>) are stored as a ES-GS ring item.
      assert(resUsage->inOutUsage.xfbOutputExpCount > 0);
      esGsRingItemSize = resUsage->inOutUsage.xfbOutputExpCount * 4;
    }

    // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
    return esGsRingItemSize | 1;
  }

  VertexCullInfoOffsets vertCullInfoOffsets = {}; // Dummy offsets (don't care)
  // For non-GS NGG, in the culling mode, the ES-GS ring item is vertex cull info.
  unsigned esGsRingItemSize = calcVertexCullInfoSizeAndOffsets(pipelineState, vertCullInfoOffsets);

  // Change it back to dword size
  assert(esGsRingItemSize % SizeOfDword == 0);
  esGsRingItemSize /= SizeOfDword;

  // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
  return esGsRingItemSize | 1;
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
    esEntryPoint->setCallingConv(CallingConv::AMDGPU_ES);
    esEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    esEntryPoint->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    esEntryPoint->addFnAttr(Attribute::AlwaysInline);
  }

  if (gsEntryPoint) {
    module = gsEntryPoint->getParent();
    gsEntryPoint->setName(lgcName::NggGsEntryPoint);
    gsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
    gsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    gsEntryPoint->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    gsEntryPoint->addFnAttr(Attribute::AlwaysInline);

    assert(copyShaderEntryPoint); // Copy shader must be present
    copyShaderEntryPoint->setName(lgcName::NggCopyShaderEntryPoint);
    copyShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_VS);
    copyShaderEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    copyShaderEntryPoint->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    copyShaderEntryPoint->addFnAttr(Attribute::AlwaysInline);
  }

  // Create NGG LDS manager
  assert(module);
  assert(!m_ldsManager);
  m_ldsManager = new NggLdsManager(module, m_pipelineState, &m_builder);

  return generatePrimShaderEntryPoint(module);
}

// =====================================================================================================================
// Calculates and returns the byte size of vertex cull info. Meanwhile, builds the collection of LDS offsets within an
// item of vertex cull info region.
//
// @param pipelineState : Pipeline state
// @param [out] vertCullInfoOffsets : The collection of LDS offsets to build
unsigned NggPrimShader::calcVertexCullInfoSizeAndOffsets(PipelineState *pipelineState,
                                                         VertexCullInfoOffsets &vertCullInfoOffsets) {
  auto nggControl = pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  vertCullInfoOffsets = {};

  // Only for non-GS NGG with culling mode enabled
  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  if (hasGs || nggControl->passthroughMode)
    return 0;

  unsigned cullInfoSize = 0;
  unsigned cullInfoOffset = 0;

  if (pipelineState->enableSwXfb()) {
    const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
    auto resUsage = pipelineState->getShaderResourceUsage(hasTes ? ShaderStageTessEval : ShaderStageVertex);

    // NOTE: Each transform feedback output is <4 x dword>.
    const unsigned xfbOutputCount = resUsage->inOutUsage.xfbOutputExpCount;
    cullInfoSize += sizeof(VertexCullInfo::xfbOutputs) * xfbOutputCount;
    vertCullInfoOffsets.xfbOutputs = cullInfoOffset;
    cullInfoOffset += sizeof(VertexCullInfo::xfbOutputs) * xfbOutputCount;
  }

  if (nggControl->enableCullDistanceCulling) {
    cullInfoSize += sizeof(VertexCullInfo::cullDistanceSignMask);
    vertCullInfoOffsets.cullDistanceSignMask = cullInfoOffset;
    cullInfoOffset += sizeof(VertexCullInfo::cullDistanceSignMask);
  }

  cullInfoSize += sizeof(VertexCullInfo::drawFlag);
  vertCullInfoOffsets.drawFlag = cullInfoOffset;
  cullInfoOffset += sizeof(VertexCullInfo::drawFlag);

  if (nggControl->compactVertex) {
    cullInfoSize += sizeof(VertexCullInfo::compactedVertexIndex);
    vertCullInfoOffsets.compactedVertexIndex = cullInfoOffset;
    cullInfoOffset += sizeof(VertexCullInfo::compactedVertexIndex);

    const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
    if (hasTes) {
      auto builtInUsage = pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
      if (builtInUsage.tessCoord) {
        cullInfoSize += sizeof(VertexCullInfo::tes.tessCoordX);
        vertCullInfoOffsets.tessCoordX = cullInfoOffset;
        cullInfoOffset += sizeof(VertexCullInfo::tes.tessCoordX);

        cullInfoSize += sizeof(VertexCullInfo::tes.tessCoordY);
        vertCullInfoOffsets.tessCoordY = cullInfoOffset;
        cullInfoOffset += sizeof(VertexCullInfo::tes.tessCoordY);
      }

      cullInfoSize += sizeof(VertexCullInfo::tes.relPatchId);
      vertCullInfoOffsets.relPatchId = cullInfoOffset;
      cullInfoOffset += sizeof(VertexCullInfo::tes.relPatchId);

      if (builtInUsage.primitiveId) {
        cullInfoSize += sizeof(VertexCullInfo::tes.patchId);
        vertCullInfoOffsets.patchId = cullInfoOffset;
        cullInfoOffset += sizeof(VertexCullInfo::tes.patchId);
      }
    } else {
      auto builtInUsage = pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
      if (builtInUsage.vertexIndex) {
        cullInfoSize += sizeof(VertexCullInfo::vs.vertexId);
        vertCullInfoOffsets.vertexId = cullInfoOffset;
        cullInfoOffset += sizeof(VertexCullInfo::vs.vertexId);
      }

      if (builtInUsage.instanceIndex) {
        cullInfoSize += sizeof(VertexCullInfo::vs.instanceId);
        vertCullInfoOffsets.instanceId = cullInfoOffset;
        cullInfoOffset += sizeof(VertexCullInfo::vs.instanceId);
      }

      if (builtInUsage.primitiveId) {
        cullInfoSize += sizeof(VertexCullInfo::vs.primitiveId);
        vertCullInfoOffsets.primitiveId = cullInfoOffset;
        cullInfoOffset += sizeof(VertexCullInfo::vs.primitiveId);
      }
    }
  }

  return cullInfoSize;
}

// =====================================================================================================================
// Generates the type for the new entry-point of NGG primitive shader.
//
// @param module : IR module (for getting ES function if needed to get vertex fetch types)
// @param [out] inRegMask : "Inreg" bit mask for the arguments
FunctionType *NggPrimShader::generatePrimShaderEntryPointType(Module *module, uint64_t *inRegMask) {
  std::vector<Type *> argTys;

  // First 8 system values (SGPRs)
  for (unsigned i = 0; i < NumSpecialSgprInputs; ++i) {
    argTys.push_back(m_builder.getInt32Ty());
    *inRegMask |= (1ull << i);
  }

  // User data (SGPRs)
  unsigned userDataCount = 0;

  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);

  if (m_hasGs) {
    // GS is present in primitive shader (ES-GS merged shader)
    userDataCount = gsIntfData->userDataCount;

    if (m_hasTes) {
      userDataCount = std::max(tesIntfData->userDataCount, userDataCount);

      if (gsIntfData->spillTable.sizeInDwords > 0 && tesIntfData->spillTable.sizeInDwords == 0) {
        tesIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    } else {
      userDataCount = std::max(vsIntfData->userDataCount, userDataCount);

      if (gsIntfData->spillTable.sizeInDwords > 0 && vsIntfData->spillTable.sizeInDwords == 0) {
        vsIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
      }
    }
  } else {
    // No GS in primitive shader (ES only)
    userDataCount = m_hasTes ? tesIntfData->userDataCount : vsIntfData->userDataCount;
  }

  assert(userDataCount > 0);
  argTys.push_back(FixedVectorType::get(m_builder.getInt32Ty(), userDataCount));
  *inRegMask |= (1ull << NumSpecialSgprInputs);

  // Other system values (VGPRs)
  argTys.push_back(m_builder.getInt32Ty()); // ES to GS offsets (vertex 0 and 1)
  argTys.push_back(m_builder.getInt32Ty()); // ES to GS offsets (vertex 2 and 3)
  argTys.push_back(m_builder.getInt32Ty()); // Primitive ID (GS)
  argTys.push_back(m_builder.getInt32Ty()); // Invocation ID
  argTys.push_back(m_builder.getInt32Ty()); // ES to GS offsets (vertex 4 and 5)

  if (m_hasTes) {
    argTys.push_back(m_builder.getFloatTy()); // X of TessCoord (U)
    argTys.push_back(m_builder.getFloatTy()); // Y of TessCoord (V)
    argTys.push_back(m_builder.getInt32Ty()); // Relative patch ID
    argTys.push_back(m_builder.getInt32Ty()); // Patch ID
  } else {
    argTys.push_back(m_builder.getInt32Ty()); // Vertex ID
    argTys.push_back(m_builder.getInt32Ty()); // Relative vertex ID (auto index)
    argTys.push_back(m_builder.getInt32Ty()); // Primitive ID (VS)
    argTys.push_back(m_builder.getInt32Ty()); // Instance ID
  }

  // If the ES is the API VS, and it is a fetchless VS, then we need to add args for the vertex fetches.
  if (!m_hasTes) {
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

  return FunctionType::get(m_builder.getVoidTy(), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for NGG primitive shader.
//
// @param module : LLVM module
Function *NggPrimShader::generatePrimShaderEntryPoint(Module *module) {
  uint64_t inRegMask = 0;
  auto entryPointTy = generatePrimShaderEntryPointType(module, &inRegMask);

  Function *entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::NggPrimShaderEntryPoint);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);

  module->getFunctionList().push_front(entryPoint);

  entryPoint->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  for (auto &arg : entryPoint->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
  }

  auto arg = entryPoint->arg_begin();
  arg += NumSpecialSgprInputs;

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
  Value *instanceId = (arg + 8);

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
    instanceId->setName("instanceId");
  }

  if (m_hasGs) {
    // API GS is present
    buildPrimShaderWithGs(entryPoint);
  } else if (m_nggControl->passthroughMode) {
    // NGG passthrough mode is enabled
    buildPassthroughPrimShader(entryPoint);
  } else {
    // NGG passthrough mode is disabled
    buildPrimShader(entryPoint);
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
// Build the body of passthrough primitive shader.
//
// @param entryPoint : Entry-point of primitive shader to build
void NggPrimShader::buildPassthroughPrimShader(Function *entryPoint) {
  assert(m_nggControl->passthroughMode); // Make sure NGG passthrough mode is enabled
  assert(!m_hasGs);                      // Make sure API GS is not present

  auto arg = entryPoint->arg_begin();

  Value *mergedGroupInfo = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedGroupInfo));
  mergedGroupInfo->setName("mergedGroupInfo");

  Value *mergedWaveInfo = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo));
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *attribRingBase = nullptr;
  if (m_gfxIp.major >= 11) {
    attribRingBase = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
    attribRingBase->setName("attribRingBase");
  }

  Value *userData = arg + NumSpecialSgprInputs;
  arg += (NumSpecialSgprInputs + 1);

  Value *esGsOffsets01 = arg;
  Value *gsPrimitiveId = (arg + 2);

  //
  // NOTE: If primitive ID is used in VS, we have to insert several basic blocks to distribute the value across
  // LDS because the primitive ID is provided as per-primitive instead of per-vertex. The algorithm is something
  // like this:
  //
  // if (distributePrimitiveId) {
  //     if (threadIdInWave < primCountInWave)
  //       Distribute primitive ID to provoking vertex (vertex0)
  //     Barrier
  //
  //     if (threadIdInWave < vertCountInWave)
  //       Get primitive ID
  //     Barrier
  //   }
  //
  const bool distributePrimitiveId =
      !m_hasTes ? m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.primitiveId : false;

  //
  // For pass-through mode, the processing is something like this:
  //
  // NGG_PASSTHROUGH() {
  //   Initialize thread/wave info
  //
  //   if (distributePrimitiveId) {
  //     if (threadIdInWave < primCountInWave)
  //       Distribute primitive ID to provoking vertex (vertex0)
  //     Barrier
  //
  //     if (threadIdInWave < vertCountInWave)
  //       Get primitive ID
  //   }
  //   Barrier
  //
  //   if (waveId == 0)
  //     GS allocation request (GS_ALLOC_REQ)
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Do primitive connectivity data export
  //
  //   if (enableSwXfb)
  //     Process XFB output export (Run ES)
  //   else {
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Run ES
  //   }
  // }
  //

  // Define basic blocks
  auto entryBlock = createBlock(entryPoint, ".entry");

  // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
  BasicBlock *writePrimIdBlock = nullptr;
  BasicBlock *endWritePrimIdBlock = nullptr;
  BasicBlock *readPrimIdBlock = nullptr;
  BasicBlock *endReadPrimIdBlock = nullptr;

  if (distributePrimitiveId) {
    writePrimIdBlock = createBlock(entryPoint, ".writePrimId");
    endWritePrimIdBlock = createBlock(entryPoint, ".endWritePrimId");

    readPrimIdBlock = createBlock(entryPoint, ".readPrimId");
    endReadPrimIdBlock = createBlock(entryPoint, ".endReadPrimId");
  }

  // NOTE: For GFX11+, we use NO_MSG mode for NGG pass-through mode if SW-emulated stream-out is not requested. The
  // message GS_ALLOC_REQ is no longer necessary.
  bool passthroughNoMsg = m_gfxIp.major >= 11 && !m_pipelineState->enableSwXfb();

  BasicBlock *allocReqBlock = nullptr;
  BasicBlock *endAllocReqBlock = nullptr;
  if (!passthroughNoMsg) {
    allocReqBlock = createBlock(entryPoint, ".allocReq");
    endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");
  }

  auto expPrimBlock = createBlock(entryPoint, ".expPrim");
  auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

  BasicBlock *expVertBlock = nullptr;
  BasicBlock *endExpVertBlock = nullptr;
  if (!m_pipelineState->enableSwXfb()) {
    expVertBlock = createBlock(entryPoint, ".expVert");
    endExpVertBlock = createBlock(entryPoint, ".endExpVert");
  }

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      // Record attribute ring base ([14:0])
      m_nggInputs.attribRingBase = createUBfe(attribRingBase, 0, 15);

      if (m_pipelineState->enableSwXfb())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive connectivity data
    m_nggInputs.primData = esGsOffsets01;

    if (distributePrimitiveId) {
      auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.primCountInWave);
      m_builder.CreateCondBr(primValid, writePrimIdBlock, endWritePrimIdBlock);
    } else {
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

      if (!passthroughNoMsg) {
        auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
        m_builder.CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
      } else {
        auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
        m_builder.CreateCondBr(primValid, expPrimBlock, endExpPrimBlock);
      }
    }
  }

  if (distributePrimitiveId) {
    // Construct ".writePrimId" block
    {
      m_builder.SetInsertPoint(writePrimIdBlock);

      // Primitive data layout
      //   [31]    = null primitive flag
      //   [28:20] = vertexIndex2
      //   [18:10] = vertexIndex1
      //   [8:0]   = vertexIndex0

      // Distribute primitive ID
      Value *vertexIndex = nullptr;
      if (m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst)
        vertexIndex = createUBfe(m_nggInputs.primData, 0, 9);
      else
        vertexIndex = createUBfe(m_nggInputs.primData, 20, 9);
      writePerThreadDataToLds(gsPrimitiveId, vertexIndex, LdsRegionDistribPrimId);

      BranchInst::Create(endWritePrimIdBlock, writePrimIdBlock);
    }

    // Construct ".endWritePrimId" block
    {
      m_builder.SetInsertPoint(endWritePrimIdBlock);

      createFenceAndBarrier();

      auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
      m_builder.CreateCondBr(vertValid, readPrimIdBlock, endReadPrimIdBlock);
    }

    // Construct ".readPrimId" block
    Value *primitiveId = nullptr;
    {
      m_builder.SetInsertPoint(readPrimIdBlock);

      primitiveId =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionDistribPrimId);

      m_builder.CreateBr(endReadPrimIdBlock);
    }

    // Construct ".endReadPrimId" block
    {
      m_builder.SetInsertPoint(endReadPrimIdBlock);

      m_distributedPrimitiveId = createPhi(
          {{primitiveId, readPrimIdBlock}, {m_builder.getInt32(0), endWritePrimIdBlock}}, "distributedPrimitiveId");

      createFenceAndBarrier();

      if (!passthroughNoMsg) {
        auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
        m_builder.CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
      } else {
        auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
        m_builder.CreateCondBr(primValid, expPrimBlock, endExpPrimBlock);
      }
    }
  }

  if (!passthroughNoMsg) {
    // Construct ".allocReq" block
    {
      m_builder.SetInsertPoint(allocReqBlock);

      doParamCacheAllocRequest();
      m_builder.CreateBr(endAllocReqBlock);
    }

    // Construct ".endAllocReq" block
    {
      m_builder.SetInsertPoint(endAllocReqBlock);

      auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
      m_builder.CreateCondBr(primValid, expPrimBlock, endExpPrimBlock);
    }
  }

  // Construct ".expPrim" block
  {
    m_builder.SetInsertPoint(expPrimBlock);

    doPrimitiveExportWithoutGs();
    m_builder.CreateBr(endExpPrimBlock);
  }

  // Construct ".endExpPrim" block
  {
    m_builder.SetInsertPoint(endExpPrimBlock);

    // NOTE: For NGG passthrough mode, if SW-emulated stream-out is enabled, running ES is included in processing
    // transform feedback output exporting. There won't be separated ES running (ES is not split any more). This is
    // because we could encounter special cases in which there are memory atomics producing output values both for
    // transform feedback exporting and for vertex exporting like following codes. The atomics shouldn't be separated
    // and be run multiple times.
    //
    //   void ES() {
    //     ...
    //     value = atomicXXX()
    //     xfbExport = value
    //     vertexExport = value
    //  }
    //
    if (m_pipelineState->enableSwXfb()) {
      processSwXfb(entryPoint->getParent(), entryPoint->arg_begin());
      m_builder.CreateRetVoid();
    } else {
      auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(vertValid, expVertBlock, endExpVertBlock);
    }
  }

  if (!m_pipelineState->enableSwXfb()) {
    // Construct ".expVert" block
    {
      m_builder.SetInsertPoint(expVertBlock);

      runEs(entryPoint->getParent(), entryPoint->arg_begin());

      m_builder.CreateBr(endExpVertBlock);
    }

    // Construct ".endExpVert" block
    {
      m_builder.SetInsertPoint(endExpVertBlock);

      m_builder.CreateRetVoid();
    }
  }
}

// =====================================================================================================================
// Build the body of primitive shader when API GS is not present.
//
// @param entryPoint : Entry-point of primitive shader to build
void NggPrimShader::buildPrimShader(Function *entryPoint) {
  assert(!m_nggControl->passthroughMode); // Make sure NGG passthrough mode is not enabled
  assert(!m_hasGs);                       // Make sure API GS is not present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

  auto arg = entryPoint->arg_begin();

  Value *mergedGroupInfo = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedGroupInfo));
  mergedGroupInfo->setName("mergedGroupInfo");

  Value *mergedWaveInfo = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo));
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *attribRingBase = nullptr;
  if (m_gfxIp.major >= 11) {
    attribRingBase = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
    attribRingBase->setName("attribRingBase");
  }

  // GS shader address is reused as primitive shader table address for NGG culling
  Value *primShaderTableAddrLow = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrLow));
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrHigh));
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  Value *userData = arg + NumSpecialSgprInputs;
  arg += (NumSpecialSgprInputs + 1);

  Value *esGsOffsets01 = arg;
  Value *esGsOffsets23 = (arg + 1);
  Value *gsPrimitiveId = (arg + 2);

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *instanceId = (arg + 8);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);

  //
  // NOTE: If primitive ID is used in VS, we have to insert several basic blocks to distribute the value across
  // LDS because the primitive ID is provided as per-primitive instead of per-vertex. The algorithm is something
  // like this:
  //
  // if (distributePrimitiveId) {
  //     if (threadIdInWave < primCountInWave)
  //       Distribute primitive ID to provoking vertex (vertex0)
  //     Barrier
  //
  //     if (threadIdInWave < vertCountInWave)
  //       Get primitive ID
  //     Barrier
  //   }
  //
  const bool distributePrimitiveId = !m_hasTes ? resUsage->builtInUsage.vs.primitiveId : false;

  //
  // The processing is something like this:
  //
  // NGG() {
  //   Initialize thread/wave info
  //
  //   if (distributePrimitiveId) {
  //     if (threadIdInWave < primCountInWave)
  //       Distribute primitive ID to provoking vertex (vertex0)
  //     Barrier
  //
  //     if (threadIdInWave < vertCountInWave)
  //       Get primitive ID
  //     Barrier
  //   }
  //
  //   if (enableSwXfb)
  //     Process XFB output export
  //
  //   if (threadIdInWave < vertCountInWave)
  //     Run ES-partial to fetch vertex cull data
  //
  //   if (!runtimePassthrough) {
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Initialize vertex draw flag
  //     if (threadIdInSubgroup < waveCount + 1)
  //       Initialize per-wave and per-subgroup count of output vertices
  //
  //     if (threadIdInWave < vertCountInWave)
  //       Write vertex cull data
  //     Barrier
  //
  //     if (threadIdInSubgroup < primCountInSubgroup) {
  //       Do culling (run culling algorithms)
  //       if (primitive not culled)
  //         Write draw flags of forming vertices
  //     }
  //     Barrier
  //
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Check draw flags of vertices and compute draw mask
  //
  //     if (threadIdInWave < waveCount - waveId)
  //       Accumulate per-wave and per-subgroup count of output vertices
  //     Barrier
  //
  //     if (vertex compacted && vertex drawn) {
  //       Compact vertex thread ID (map: compacted -> uncompacted)
  //       Write vertex compaction info
  //     }
  //     Update vertCountInSubgroup and primCountInSubgroup
  //   }
  //
  //   if (waveId == 0)
  //     GS allocation request (GS_ALLOC_REQ)
  //   Barrier
  //
  //   if (fullyCulled) {
  //     Do dummy export
  //     return (early exit)
  //   }
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Do primitive connectivity data export
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     if (vertex compactionless && empty wave)
  //       Do dummy vertex export
  //     else
  //       Run ES-partial to do deferred vertex export
  //   }
  // }
  //

  // Export count when the entire subgroup is fully culled
  const bool waNggCullingNoEmptySubgroups =
      m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups;
  const unsigned fullyCulledExportCount = waNggCullingNoEmptySubgroups ? 1 : 0;

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;

  // NOTE: Make sure vertex position data is 16-byte alignment because we will use 128-bit LDS read/write for it.
  assert(m_ldsManager->getLdsRegionStart(LdsRegionVertPosData) % SizeOfVec4 == 0);

  if (!m_nggControl->compactVertex)
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  // Define basic blocks
  auto entryBlock = createBlock(entryPoint, ".entry");

  // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
  BasicBlock *writePrimIdBlock = nullptr;
  BasicBlock *endWritePrimIdBlock = nullptr;
  BasicBlock *readPrimIdBlock = nullptr;
  BasicBlock *endReadPrimIdBlock = nullptr;

  if (distributePrimitiveId) {
    writePrimIdBlock = createBlock(entryPoint, ".writePrimId");
    endWritePrimIdBlock = createBlock(entryPoint, ".endWritePrimId");

    readPrimIdBlock = createBlock(entryPoint, ".readPrimId");
    endReadPrimIdBlock = createBlock(entryPoint, ".endReadPrimId");
  }

  auto fetchVertCullDataBlock = createBlock(entryPoint, ".fetchVertCullData");
  auto endFetchVertCullDataBlock = createBlock(entryPoint, ".endFetchVertCullData");

  auto runtimePassthroughBlock = createBlock(entryPoint, ".runtimePassthrough");
  auto noRuntimePassthroughBlock = createBlock(entryPoint, ".noRuntimePassthrough");

  auto initVertDrawFlagBlock = createBlock(entryPoint, ".initVertDrawFlag");
  auto endInitVertDrawFlagBlock = createBlock(entryPoint, ".endInitVertDrawFlag");

  auto initVertCountBlock = createBlock(entryPoint, ".initVertCount");
  auto endInitVertCountBlock = createBlock(entryPoint, ".endInitVertCount");

  auto writeVertCullDataBlock = createBlock(entryPoint, ".writeVertCullData");
  auto endWriteVertCullDataBlock = createBlock(entryPoint, ".endWriteVertCullData");

  auto cullingBlock = createBlock(entryPoint, ".culling");
  auto writeVertDrawFlagBlock = createBlock(entryPoint, ".writeVertDrawFlag");
  auto endCullingBlock = createBlock(entryPoint, ".endCulling");

  auto checkVertDrawFlagBlock = createBlock(entryPoint, ".checkVertDrawFlag");
  auto endCheckVertDrawFlagBlock = createBlock(entryPoint, ".endCheckVertDrawFlag");

  auto accumVertCountBlock = createBlock(entryPoint, ".accumVertCount");
  auto endAccumVertCountBlock = createBlock(entryPoint, ".endAccumVertCount");

  auto compactVertBlock =
      m_nggControl->compactVertex ? createBlock(entryPoint, ".compactVert") : nullptr; // Conditionally created
  auto endCompactVertBlock = createBlock(entryPoint, ".endCompactVert");

  auto checkAllocReqBlock = createBlock(entryPoint, ".checkAllocReq");
  auto allocReqBlock = createBlock(entryPoint, ".allocReq");
  auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

  // NOTE: Those basic blocks are conditionally created according to the enablement of a workaround flag that handles
  // empty subgroup.
  BasicBlock *earlyExitBlock = nullptr;
  BasicBlock *noEarlyExitBlock = nullptr;
  if (waNggCullingNoEmptySubgroups) {
    earlyExitBlock = createBlock(entryPoint, ".earlyExit");
    noEarlyExitBlock = createBlock(entryPoint, ".noEarlyExit");
  }

  auto expPrimBlock = createBlock(entryPoint, ".expPrim");
  auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

  // NOTE: Those basic blocks are conditionally created according to the enablement of vertex compactionless mode.
  BasicBlock *checkEmptyWaveBlock = nullptr;
  BasicBlock *emptyWaveExpBlock = nullptr;
  BasicBlock *noEmptyWaveExpBlock = nullptr;
  if (!m_nggControl->compactVertex) {
    checkEmptyWaveBlock = createBlock(entryPoint, ".checkEmptyWave");
    emptyWaveExpBlock = createBlock(entryPoint, ".emptyWaveExp");
    noEmptyWaveExpBlock = createBlock(entryPoint, ".noEmptyWaveExp");
  }

  auto expVertBlock = createBlock(entryPoint, ".expVert");
  auto endExpVertBlock = createBlock(entryPoint, ".endExpVert");

  // Construct ".entry" block
  Value *vertexItemOffset = nullptr;
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      // Record attribute ring base ([14:0])
      m_nggInputs.attribRingBase = createUBfe(attribRingBase, 0, 15);

      if (m_pipelineState->enableSwXfb())
        loadStreamOutBufferInfo(userData);
    }

    m_nggInputs.primShaderTableAddrLow = primShaderTableAddrLow;
    m_nggInputs.primShaderTableAddrHigh = primShaderTableAddrHigh;

    // Record ES-GS vertex offsets info
    m_nggInputs.esGsOffset0 = createUBfe(esGsOffsets01, 0, 16);
    m_nggInputs.esGsOffset1 = createUBfe(esGsOffsets01, 16, 16);
    m_nggInputs.esGsOffset2 = createUBfe(esGsOffsets23, 0, 16);

    vertexItemOffset =
        m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(esGsRingItemSize * SizeOfDword));

    if (distributePrimitiveId) {
      auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.primCountInWave);
      m_builder.CreateCondBr(primValid, writePrimIdBlock, endWritePrimIdBlock);
    } else {
      if (m_pipelineState->enableSwXfb())
        processSwXfb(entryPoint->getParent(), entryPoint->arg_begin());

      auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
      m_builder.CreateCondBr(vertValid, fetchVertCullDataBlock, endFetchVertCullDataBlock);
    }
  }

  if (distributePrimitiveId) {
    // Construct ".writePrimId" block
    {
      m_builder.SetInsertPoint(writePrimIdBlock);

      Value *vertexIndex = nullptr;
      if (m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst)
        vertexIndex = m_nggInputs.esGsOffset0;
      else
        vertexIndex = m_nggInputs.esGsOffset2;
      writePerThreadDataToLds(gsPrimitiveId, vertexIndex, LdsRegionDistribPrimId);

      m_builder.CreateBr(endWritePrimIdBlock);
    }

    // Construct ".endWritePrimId" block
    {
      m_builder.SetInsertPoint(endWritePrimIdBlock);

      createFenceAndBarrier();

      auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
      m_builder.CreateCondBr(vertValid, readPrimIdBlock, endReadPrimIdBlock);
    }

    // Construct ".readPrimId" block
    Value *primitiveId = nullptr;
    {
      m_builder.SetInsertPoint(readPrimIdBlock);

      primitiveId =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionDistribPrimId);

      m_builder.CreateBr(endReadPrimIdBlock);
    }

    // Construct ".endReadPrimId" block
    {
      m_builder.SetInsertPoint(endReadPrimIdBlock);

      m_distributedPrimitiveId = createPhi(
          {{primitiveId, readPrimIdBlock}, {m_builder.getInt32(0), endWritePrimIdBlock}}, "distributedPrimitiveId");

      createFenceAndBarrier();

      if (m_pipelineState->enableSwXfb())
        processSwXfb(entryPoint->getParent(), entryPoint->arg_begin());

      auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
      m_builder.CreateCondBr(vertValid, fetchVertCullDataBlock, endFetchVertCullDataBlock);
    }
  }

  // Construct ".fetchVertCullData" block
  Value *cullData = nullptr;
  Value *position = nullptr;
  {
    m_builder.SetInsertPoint(fetchVertCullDataBlock);

    // Split ES to two parts: fetch cull data before NGG culling; do deferred vertex export after NGG culling
    splitEs(entryPoint->getParent());

    // Run ES-partial to fetch cull data
    auto cullData = runEsPartial(entryPoint->getParent(), entryPoint->arg_begin());
    position = m_nggControl->enableCullDistanceCulling ? m_builder.CreateExtractValue(cullData, 0) : cullData;

    m_builder.CreateBr(endFetchVertCullDataBlock);
  }

  // Construct ".endFetchVertCullData" block
  {
    m_builder.SetInsertPoint(endFetchVertCullDataBlock);

    position =
        createPhi({{position, fetchVertCullDataBlock},
                   {PoisonValue::get(position->getType()), distributePrimitiveId ? endReadPrimIdBlock : entryBlock}},
                  "position"); // Update vertex position data

    // NOTE: If the Z channel of vertex position data is constant, we can go into runtime passthrough mode. Otherwise,
    // we will further check if this is a small subgroup and enable runtime passthrough mode accordingly.
    auto runtimePassthrough = m_constPositionZ ? m_builder.getTrue()
                                               : m_builder.CreateICmpULT(m_nggInputs.vertCountInSubgroup,
                                                                         m_builder.getInt32(NggSmallSubgroupThreshold));
    m_builder.CreateCondBr(runtimePassthrough, runtimePassthroughBlock, noRuntimePassthroughBlock);
  }

  // Construct ".runtimePassthrough" block
  {
    m_builder.SetInsertPoint(runtimePassthroughBlock);

    if (!distributePrimitiveId) {
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
    }

    m_builder.CreateBr(checkAllocReqBlock);
  }

  // Construct ".noRuntimePassthrough" block
  {
    m_builder.SetInsertPoint(noRuntimePassthroughBlock);

    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(vertValid, initVertDrawFlagBlock, endInitVertDrawFlagBlock);
  }

  // Construct ".initVertDrawFlag" block
  {
    m_builder.SetInsertPoint(initVertDrawFlagBlock);

    writeVertexCullInfoToLds(m_builder.getInt32(0), vertexItemOffset, m_vertCullInfoOffsets.drawFlag);

    m_builder.CreateBr(endInitVertDrawFlagBlock);
  }

  // Construct ".endInitVertDrawFlag" block
  {
    m_builder.SetInsertPoint(endInitVertDrawFlagBlock);

    auto waveValid =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(waveCountInSubgroup + 1));
    m_builder.CreateCondBr(waveValid, initVertCountBlock, endInitVertCountBlock);
  }

  // Construct ".initVertCount" block
  {
    m_builder.SetInsertPoint(initVertCountBlock);

    writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup, LdsRegionVertCountInWaves);

    m_builder.CreateBr(endInitVertCountBlock);
  }

  // Construct ".endInitVertCount" block
  {
    m_builder.SetInsertPoint(endInitVertCountBlock);

    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
    m_builder.CreateCondBr(vertValid, writeVertCullDataBlock, endWriteVertCullDataBlock);
  }

  // Construct ".writeVertexCullData" block
  {
    m_builder.SetInsertPoint(writeVertCullDataBlock);

    // Write vertex position data
    writePerThreadDataToLds(position, m_nggInputs.threadIdInSubgroup, LdsRegionVertPosData, 0, true);

    // Write cull distance sign mask
    if (m_nggControl->enableCullDistanceCulling) {
      auto cullDistance = m_builder.CreateExtractValue(cullData, 1);

      // Calculate the sign mask for cull distance
      Value *signMask = m_builder.getInt32(0);
      for (unsigned i = 0; i < cullDistance->getType()->getArrayNumElements(); ++i) {
        auto cullDistanceVal = m_builder.CreateExtractValue(cullDistance, i);
        cullDistanceVal = m_builder.CreateBitCast(cullDistanceVal, m_builder.getInt32Ty());

        Value *signBit = createUBfe(cullDistanceVal, 31, 1);
        signBit = m_builder.CreateShl(signBit, i);

        signMask = m_builder.CreateOr(signMask, signBit);
      }

      writeVertexCullInfoToLds(signMask, vertexItemOffset, m_vertCullInfoOffsets.cullDistanceSignMask);
    }

    m_builder.CreateBr(endWriteVertCullDataBlock);
  }

  // Construct ".endWriteVertCullData" block
  {
    m_builder.SetInsertPoint(endWriteVertCullDataBlock);

    createFenceAndBarrier();

    auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(primValid, cullingBlock, endCullingBlock);
  }

  // Construct ".culling" block
  Value *cullFlag = nullptr;
  {
    m_builder.SetInsertPoint(cullingBlock);

    auto vertexIndex0 = m_nggInputs.esGsOffset0;
    auto vertexIndex1 = m_nggInputs.esGsOffset1;
    auto vertexIndex2 = m_nggInputs.esGsOffset2;

    cullFlag = doCulling(entryPoint->getParent(), vertexIndex0, vertexIndex1, vertexIndex2);
    m_builder.CreateCondBr(cullFlag, endCullingBlock, writeVertDrawFlagBlock);
  }

  // Construct ".writeVertDrawFlag" block
  {
    m_builder.SetInsertPoint(writeVertDrawFlagBlock);

    auto vertexItemOffset0 =
        m_builder.CreateMul(m_nggInputs.esGsOffset0, m_builder.getInt32(esGsRingItemSize * SizeOfDword));
    auto vertexItemOffset1 =
        m_builder.CreateMul(m_nggInputs.esGsOffset1, m_builder.getInt32(esGsRingItemSize * SizeOfDword));
    auto vertexItemOffset2 =
        m_builder.CreateMul(m_nggInputs.esGsOffset2, m_builder.getInt32(esGsRingItemSize * SizeOfDword));

    writeVertexCullInfoToLds(m_builder.getInt32(1), vertexItemOffset0, m_vertCullInfoOffsets.drawFlag);
    writeVertexCullInfoToLds(m_builder.getInt32(1), vertexItemOffset1, m_vertCullInfoOffsets.drawFlag);
    writeVertexCullInfoToLds(m_builder.getInt32(1), vertexItemOffset2, m_vertCullInfoOffsets.drawFlag);

    m_builder.CreateBr(endCullingBlock);
  }

  // Construct ".endCulling" block
  {
    m_builder.SetInsertPoint(endCullingBlock);

    cullFlag = createPhi({{m_builder.getTrue(), cullingBlock},
                          {m_builder.getFalse(), writeVertDrawFlagBlock},
                          {m_builder.getTrue(), endWriteVertCullDataBlock}});

    createFenceAndBarrier();

    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(vertValid, checkVertDrawFlagBlock, endCheckVertDrawFlagBlock);
  }

  // Construct ".checkVertDrawFlag"
  Value *drawFlag = nullptr;
  {
    m_builder.SetInsertPoint(checkVertDrawFlagBlock);

    drawFlag = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.drawFlag);
    drawFlag = m_builder.CreateICmpNE(drawFlag, m_builder.getInt32(0));

    m_builder.CreateBr(endCheckVertDrawFlagBlock);
  }

  // Construct ".endCheckVertDrawFlag"
  Value *drawMask = nullptr;
  Value *vertCountInWave = nullptr;
  {
    m_builder.SetInsertPoint(endCheckVertDrawFlagBlock);

    drawFlag = createPhi(
        {{drawFlag, checkVertDrawFlagBlock}, {m_builder.getFalse(), endCullingBlock}}); // Update vertex draw flag
    drawMask = doSubgroupBallot(drawFlag);

    vertCountInWave = m_builder.CreateIntrinsic(Intrinsic::ctpop, m_builder.getInt64Ty(), drawMask);
    vertCountInWave = m_builder.CreateTrunc(vertCountInWave, m_builder.getInt32Ty());

    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(waveCountInSubgroup), m_nggInputs.waveIdInSubgroup);
    auto threadValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);

    m_builder.CreateCondBr(threadValid, accumVertCountBlock, endAccumVertCountBlock);
  }

  // Construct ".accumVertCount" block
  {
    m_builder.SetInsertPoint(accumVertCountBlock);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));
    ldsOffset = m_builder.CreateShl(ldsOffset, 2);

    unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCountInWaves);

    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart));
    m_ldsManager->atomicOpWithLds(AtomicRMWInst::Add, vertCountInWave, ldsOffset);

    m_builder.CreateBr(endAccumVertCountBlock);
  }

  // Construct ".endAccumVertCount" block
  Value *vertCountInPrevWaves = nullptr;
  Value *vertCountInSubgroup = nullptr;
  Value *vertCompacted = nullptr;
  {
    m_builder.SetInsertPoint(endAccumVertCountBlock);

    createFenceAndBarrier();

    auto vertCountInWaves =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, LdsRegionVertCountInWaves);

    // The last dword following dwords for all waves (each wave has one dword) stores vertex count of the
    // entire subgroup
    vertCountInSubgroup = m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {},
                                                    {vertCountInWaves, m_builder.getInt32(waveCountInSubgroup)});

    if (!m_nggControl->compactVertex) {
      m_builder.CreateBr(endCompactVertBlock);
    } else {
      // Get vertex count for all waves prior to this wave
      vertCountInPrevWaves =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, {vertCountInWaves, m_nggInputs.waveIdInSubgroup});

      vertCompacted = m_builder.CreateICmpULT(vertCountInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(m_builder.CreateAnd(drawFlag, vertCompacted), compactVertBlock, endCompactVertBlock);
    }
  }

  if (m_nggControl->compactVertex) {
    // Construct ".compactVert" block
    {
      m_builder.SetInsertPoint(compactVertBlock);

      auto drawMaskVec = m_builder.CreateBitCast(drawMask, FixedVectorType::get(m_builder.getInt32Ty(), 2));

      auto drawMaskLow = m_builder.CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
      Value *compactedVertexIndex =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder.getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder.CreateExtractElement(drawMaskVec, 1);
        compactedVertexIndex =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactedVertexIndex});
      }

      // Setup the map: compacted -> uncompacted
      compactedVertexIndex = m_builder.CreateAdd(vertCountInPrevWaves, compactedVertexIndex);
      writePerThreadDataToLds(m_nggInputs.threadIdInSubgroup, compactedVertexIndex, LdsRegionVertThreadIdMap);

      // Write compacted vertex index
      writeVertexCullInfoToLds(compactedVertexIndex, vertexItemOffset, m_vertCullInfoOffsets.compactedVertexIndex);

      if (m_hasTes) {
        // Write X/Y of tessCoord (U/V)
        if (resUsage->builtInUsage.tes.tessCoord) {
          writeVertexCullInfoToLds(tessCoordX, vertexItemOffset, m_vertCullInfoOffsets.tessCoordX);
          writeVertexCullInfoToLds(tessCoordY, vertexItemOffset, m_vertCullInfoOffsets.tessCoordY);
        }

        // Write relative patch ID
        writeVertexCullInfoToLds(relPatchId, vertexItemOffset, m_vertCullInfoOffsets.relPatchId);

        // Write patch ID
        if (resUsage->builtInUsage.tes.primitiveId)
          writeVertexCullInfoToLds(patchId, vertexItemOffset, m_vertCullInfoOffsets.patchId);
      } else {
        // Write vertex ID
        if (resUsage->builtInUsage.vs.vertexIndex)
          writeVertexCullInfoToLds(vertexId, vertexItemOffset, m_vertCullInfoOffsets.vertexId);

        // Write instance ID
        if (resUsage->builtInUsage.vs.instanceIndex)
          writeVertexCullInfoToLds(instanceId, vertexItemOffset, m_vertCullInfoOffsets.instanceId);

        // Write primitive ID
        if (resUsage->builtInUsage.vs.primitiveId) {
          assert(m_distributedPrimitiveId);
          writeVertexCullInfoToLds(m_distributedPrimitiveId, vertexItemOffset, m_vertCullInfoOffsets.primitiveId);
        }
      }

      m_builder.CreateBr(endCompactVertBlock);
    }
  }

  // Construct ".endCompactVert" block
  Value *fullyCulled = nullptr;
  Value *primCountInSubgroup = nullptr;
  {
    m_builder.SetInsertPoint(endCompactVertBlock);

    fullyCulled = m_builder.CreateICmpEQ(vertCountInSubgroup, m_builder.getInt32(0));

    primCountInSubgroup = m_builder.CreateSelect(fullyCulled, m_builder.getInt32(fullyCulledExportCount),
                                                 m_nggInputs.primCountInSubgroup);

    // NOTE: Here, we have to promote revised primitive count in subgroup to SGPR since it is treated
    // as an uniform value later. This is similar to the provided primitive count in subgroup that is
    // a system value.
    primCountInSubgroup = m_builder.CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primCountInSubgroup);

    vertCountInSubgroup =
        m_builder.CreateSelect(fullyCulled, m_builder.getInt32(fullyCulledExportCount),
                               m_nggControl->compactVertex ? vertCountInSubgroup : m_nggInputs.vertCountInSubgroup);

    // NOTE: Here, we have to promote revised vertex count in subgroup to SGPR since it is treated as
    // an uniform value later, similar to what we have done for the revised primitive count in
    // subgroup.
    vertCountInSubgroup = m_builder.CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInSubgroup);

    m_builder.CreateBr(checkAllocReqBlock);
  }

  // Construct ".checkAllocReq" block
  {
    m_builder.SetInsertPoint(checkAllocReqBlock);

    // NOTE: Here, we make several phi nodes to update some values that are different in runtime passthrough path
    // and no runtime passthrough path (normal culling path).

    if (m_nggControl->compactVertex) {
      // Update vertex compaction flag
      m_nggInputs.vertCompacted = createPhi(
          {{vertCompacted, endCompactVertBlock}, {m_builder.getFalse(), runtimePassthroughBlock}}, "vertCompacted");
    } else {
      assert(!m_nggInputs.vertCompacted); // Must be null
    }

    // Update cull flag
    cullFlag =
        createPhi({{cullFlag, endCompactVertBlock}, {m_builder.getFalse(), runtimePassthroughBlock}}, "cullFlag");

    // Update fully-culled flag
    fullyCulled =
        createPhi({{fullyCulled, endCompactVertBlock}, {m_builder.getFalse(), runtimePassthroughBlock}}, "fullyCulled");

    // Update primitive count in subgroup
    m_nggInputs.primCountInSubgroup = createPhi(
        {{primCountInSubgroup, endCompactVertBlock}, {m_nggInputs.primCountInSubgroup, runtimePassthroughBlock}},
        "primCountInSubgroup");

    // Update vertex count in subgroup
    m_nggInputs.vertCountInSubgroup = createPhi(
        {{vertCountInSubgroup, endCompactVertBlock}, {m_nggInputs.vertCountInSubgroup, runtimePassthroughBlock}},
        "vertCountInSubgroup");

    if (!m_nggControl->compactVertex) {
      // Update draw flag
      drawFlag =
          createPhi({{drawFlag, endCompactVertBlock}, {m_builder.getTrue(), runtimePassthroughBlock}}, "drawFlag");

      // Update vertex count in wave
      vertCountInWave =
          createPhi({{vertCountInWave, endCompactVertBlock}, {m_nggInputs.vertCountInWave, runtimePassthroughBlock}},
                    "vertCountInWave");
    }

    auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
  }

  // Construct ".allocReq" block
  {
    m_builder.SetInsertPoint(allocReqBlock);

    doParamCacheAllocRequest();
    m_builder.CreateBr(endAllocReqBlock);
  }

  // Construct ".endAllocReq" block
  {
    m_builder.SetInsertPoint(endAllocReqBlock);

    createFenceAndBarrier();

    if (waNggCullingNoEmptySubgroups)
      m_builder.CreateCondBr(fullyCulled, earlyExitBlock, noEarlyExitBlock);
    else {
      auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
      m_builder.CreateCondBr(primValid, expPrimBlock, endExpPrimBlock);
    }
  }

  if (waNggCullingNoEmptySubgroups) {
    // Construct ".earlyExit" block
    {
      m_builder.SetInsertPoint(earlyExitBlock);

      doEarlyExit(fullyCulledExportCount);
    }

    // Construct ".noEarlyExit" block
    {
      m_builder.SetInsertPoint(noEarlyExitBlock);

      auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
      m_builder.CreateCondBr(primValid, expPrimBlock, endExpPrimBlock);
    }
  }

  // Construct ".expPrim" block
  {
    m_builder.SetInsertPoint(expPrimBlock);

    doPrimitiveExportWithoutGs(cullFlag);

    m_builder.CreateBr(endExpPrimBlock);
  }

  // Construct ".endExpPrim" block
  {
    m_builder.SetInsertPoint(endExpPrimBlock);

    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    if (m_nggControl->compactVertex)
      m_builder.CreateCondBr(vertValid, expVertBlock, endExpVertBlock);
    else
      m_builder.CreateCondBr(vertValid, checkEmptyWaveBlock, endExpVertBlock);
  }

  if (!m_nggControl->compactVertex) {
    // Construct ".checkEmptyWave" block
    {
      m_builder.SetInsertPoint(checkEmptyWaveBlock);

      auto emptyWave = m_builder.CreateICmpEQ(vertCountInWave, m_builder.getInt32(0));
      m_builder.CreateCondBr(emptyWave, emptyWaveExpBlock, noEmptyWaveExpBlock);
    }

    // Construct ".emptyWaveExp" block
    {
      m_builder.SetInsertPoint(emptyWaveExpBlock);

      auto undef = UndefValue::get(m_builder.getFloatTy());
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                {
                                    m_builder.getInt32(EXP_TARGET_POS_0), // tgt
                                    m_builder.getInt32(0x0),              // en
                                    // src0 ~ src3
                                    undef, undef, undef, undef,
                                    m_builder.getTrue(), // done
                                    m_builder.getFalse() // vm
                                });

      m_builder.CreateRetVoid();
    }

    // Construct ".noEmptyWaveExp" block
    {
      m_builder.SetInsertPoint(noEmptyWaveExpBlock);

      m_builder.CreateCondBr(drawFlag, expVertBlock, endExpVertBlock);
    }
  }

  // Construct ".expVert" block
  {
    m_builder.SetInsertPoint(expVertBlock);

    // Run ES-partial to do deferred vertex export
    runEsPartial(entryPoint->getParent(), entryPoint->arg_begin(), position);

    m_builder.CreateBr(endExpVertBlock);
  }

  // Construct ".endExpVert" block
  {
    m_builder.SetInsertPoint(endExpVertBlock);

    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Build the body of primitive shader when API GS is present.
//
// @param entryPoint : Entry-point of primitive shader to build
void NggPrimShader::buildPrimShaderWithGs(Function *entryPoint) {
  assert(m_hasGs); // Make sure API GS is present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  if (!m_nggControl->compactVertex)
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;
  const bool cullingMode = !m_nggControl->passthroughMode;

  const auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs;
  const auto rasterStream = inOutUsage.rasterStream;

  auto arg = entryPoint->arg_begin();

  Value *mergedGroupInfo = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedGroupInfo));
  mergedGroupInfo->setName("mergedGroupInfo");

  Value *mergedWaveInfo = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo));
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *attribRingBase = nullptr;
  if (m_gfxIp.major >= 11) {
    attribRingBase = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
    attribRingBase->setName("attribRingBase");
  }

  // GS shader address is reused as primitive shader table address for NGG culling
  Value *primShaderTableAddrLow = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrLow));
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrHigh));
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  Value *userData = arg + NumSpecialSgprInputs;
  arg += (NumSpecialSgprInputs + 1);

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
  //   Barrier
  //
  //   if (threadIdInWave < primCountInWave)
  //     Run GS
  //
  //   if (enableSwXfb)
  //     Process XFB output export
  //
  //  if (threadIdInSubgroup < waveCount + 1)
  //     Initialize per-wave and per-subgroup count of output vertices
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
  //   if (vertex compacted && vertex drawn)
  //     Compact vertex thread ID (map: compacted -> uncompacted)
  //
  //   if (waveId == 0)
  //     GS allocation request (GS_ALLOC_REQ)
  //   Barrier
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Do primitive connectivity data export
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     if (vertex compactionless && empty wave)
  //       Do dummy vertex export
  //     else
  //       Run copy shader
  //   }
  // }
  //

  // Define basic blocks
  auto entryBlock = createBlock(entryPoint, ".entry");

  auto beginEsBlock = createBlock(entryPoint, ".beginEs");
  auto endEsBlock = createBlock(entryPoint, ".endEs");

  auto initOutPrimDataBlock = createBlock(entryPoint, ".initOutPrimData");
  auto endInitOutPrimDataBlock = createBlock(entryPoint, ".endInitOutPrimData");

  auto beginGsBlock = createBlock(entryPoint, ".beginGs");
  auto endGsBlock = createBlock(entryPoint, ".endGs");

  auto initOutVertCountBlock = createBlock(entryPoint, ".initOutVertCount");
  auto endInitOutVertCountBlock = createBlock(entryPoint, ".endInitOutVertCount");

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

  // NOTE: Those basic blocks are conditionally created according to the disablement of vertex compactionless mode.
  BasicBlock *compactOutVertIdBlock = nullptr;
  BasicBlock *endCompactOutVertIdBlock = nullptr;
  if (m_nggControl->compactVertex) {
    compactOutVertIdBlock = createBlock(entryPoint, ".compactOutVertId");
    endCompactOutVertIdBlock = createBlock(entryPoint, ".endCompactOutVertId");
  }

  auto allocReqBlock = createBlock(entryPoint, ".allocReq");
  auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

  auto expPrimBlock = createBlock(entryPoint, ".expPrim");
  auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

  // NOTE: Those basic blocks are conditionally created according to the enablement of vertex compactionless mode.
  BasicBlock *checkEmptyWaveBlock = nullptr;
  BasicBlock *emptyWaveExpBlock = nullptr;
  BasicBlock *noEmptyWaveExpBlock = nullptr;
  if (!m_nggControl->compactVertex) {
    checkEmptyWaveBlock = createBlock(entryPoint, ".checkEmptyWave");
    emptyWaveExpBlock = createBlock(entryPoint, ".emptyWaveExp");
    noEmptyWaveExpBlock = createBlock(entryPoint, ".noEmptyWaveExp");
  }

  auto expVertBlock = createBlock(entryPoint, ".expVert");
  auto endExpVertBlock = createBlock(entryPoint, ".endExpVert");

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      // Record attribute ring base ([14:0])
      m_nggInputs.attribRingBase = createUBfe(attribRingBase, 0, 15);

      if (m_pipelineState->enableSwXfb())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive shader table address info
    m_nggInputs.primShaderTableAddrLow = primShaderTableAddrLow;
    m_nggInputs.primShaderTableAddrHigh = primShaderTableAddrHigh;

    // Record ES-GS vertex offsets info
    m_nggInputs.esGsOffset0 = createUBfe(esGsOffsets01, 0, 16);
    m_nggInputs.esGsOffset1 = createUBfe(esGsOffsets01, 16, 16);
    m_nggInputs.esGsOffset2 = createUBfe(esGsOffsets23, 0, 16);
    m_nggInputs.esGsOffset3 = createUBfe(esGsOffsets23, 16, 16);
    m_nggInputs.esGsOffset4 = createUBfe(esGsOffsets45, 0, 16);
    m_nggInputs.esGsOffset5 = createUBfe(esGsOffsets45, 16, 16);

    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
    m_builder.CreateCondBr(vertValid, beginEsBlock, endEsBlock);
  }

  // Construct ".beginEs" block
  {
    m_builder.SetInsertPoint(beginEsBlock);

    runEs(entryPoint->getParent(), entryPoint->arg_begin());

    m_builder.CreateBr(endEsBlock);
  }

  // Construct ".endEs" block
  {
    m_builder.SetInsertPoint(endEsBlock);

    auto outPrimValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(outPrimValid, initOutPrimDataBlock, endInitOutPrimDataBlock);
  }

  // Construct ".initOutPrimData" block
  {
    m_builder.SetInsertPoint(initOutPrimDataBlock);

    if (m_pipelineState->enableSwXfb()) {
      const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();
      for (unsigned i = 0; i < MaxGsStreams; ++i) {
        bool streamActive = streamXfbBuffers[i] != 0;
        if (streamActive) { // Initialize primitive connectivity data if the stream is active
          writePerThreadDataToLds(m_builder.getInt32(NullPrim), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                                  SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * i);
        }
      }
    } else {
      writePerThreadDataToLds(m_builder.getInt32(NullPrim), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                              SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
    }

    m_builder.CreateBr(endInitOutPrimDataBlock);
  }

  // Construct ".endInitOutPrimData" block
  {
    m_builder.SetInsertPoint(endInitOutPrimDataBlock);

    createFenceAndBarrier();

    auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.primCountInWave);
    m_builder.CreateCondBr(primValid, beginGsBlock, endGsBlock);
  }

  // Construct ".beginGs" block
  {
    m_builder.SetInsertPoint(beginGsBlock);

    runGs(entryPoint->getParent(), entryPoint->arg_begin());

    m_builder.CreateBr(endGsBlock);
  }

  // Construct ".endGs" block
  {
    m_builder.SetInsertPoint(endGsBlock);

    if (m_pipelineState->enableSwXfb())
      processSwXfbWithGs(entryPoint->getParent(), entryPoint->arg_begin());

    auto waveValid =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(waveCountInSubgroup + 1));
    m_builder.CreateCondBr(waveValid, initOutVertCountBlock, endInitOutVertCountBlock);
  }

  // Construct ".initOutVertCount" block
  {
    m_builder.SetInsertPoint(initOutVertCountBlock);

    writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup, LdsRegionOutVertCountInWaves,
                            (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * rasterStream);

    m_builder.CreateBr(endInitOutVertCountBlock);
  }

  // Construct ".endInitOutVertCount" block
  Value *primData = nullptr;
  {
    m_builder.SetInsertPoint(endInitOutVertCountBlock);

    createFenceAndBarrier();

    if (cullingMode) {
      // Do culling
      primData = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                                          SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
      auto doCull = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));
      auto outPrimValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
      doCull = m_builder.CreateAnd(doCull, outPrimValid);
      m_builder.CreateCondBr(doCull, cullingBlock, endCullingBlock);
    } else {
      // No culling
      auto outVertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(outVertValid, checkOutVertDrawFlagBlock, endCheckOutVertDrawFlagBlock);
    }
  }

  // Construct culling blocks
  if (cullingMode) {
    // Construct ".culling" block
    {
      m_builder.SetInsertPoint(cullingBlock);

      assert(m_pipelineState->getShaderModes()->getGeometryShaderMode().outputPrimitive ==
             OutputPrimitives::TriangleStrip);

      // NOTE: primData[N] corresponds to the forming vertices <N, N+1, N+2> or <N, N+2, N+1>.
      Value *winding = m_builder.CreateICmpNE(primData, m_builder.getInt32(0));

      auto vertexIndex0 = m_nggInputs.threadIdInSubgroup;
      auto vertexIndex1 =
          m_builder.CreateAdd(m_nggInputs.threadIdInSubgroup,
                              m_builder.CreateSelect(winding, m_builder.getInt32(2), m_builder.getInt32(1)));
      auto vertexIndex2 =
          m_builder.CreateAdd(m_nggInputs.threadIdInSubgroup,
                              m_builder.CreateSelect(winding, m_builder.getInt32(1), m_builder.getInt32(2)));

      auto cullFlag = doCulling(entryPoint->getParent(), vertexIndex0, vertexIndex1, vertexIndex2);
      m_builder.CreateCondBr(cullFlag, nullifyOutPrimDataBlock, endCullingBlock);
    }

    // Construct ".nullifyOutPrimData" block
    {
      m_builder.SetInsertPoint(nullifyOutPrimDataBlock);

      writePerThreadDataToLds(m_builder.getInt32(NullPrim), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                              SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);

      m_builder.CreateBr(endCullingBlock);
    }

    // Construct ".endCulling" block
    {
      m_builder.SetInsertPoint(endCullingBlock);

      createFenceAndBarrier();

      auto outVertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(outVertValid, checkOutVertDrawFlagBlock, endCheckOutVertDrawFlagBlock);
    }
  }

  // Construct ".checkOutVertDrawFlag"
  Value *drawFlag = nullptr;
  {
    m_builder.SetInsertPoint(checkOutVertDrawFlagBlock);

    const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();

    // drawFlag = primData[N] != NullPrim
    auto primData0 =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                                 SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
    auto drawFlag0 = m_builder.CreateICmpNE(primData0, m_builder.getInt32(NullPrim));
    drawFlag = drawFlag0;

    if (outVertsPerPrim > 1) {
      // drawFlag |= N >= 1 ? (primData[N-1] != NullPrim) : false
      auto primData1 = readPerThreadDataFromLds(
          m_builder.getInt32Ty(), m_builder.CreateSub(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(1)),
          LdsRegionOutPrimData, SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
      auto drawFlag1 =
          m_builder.CreateSelect(m_builder.CreateICmpUGE(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(1)),
                                 m_builder.CreateICmpNE(primData1, m_builder.getInt32(NullPrim)), m_builder.getFalse());
      drawFlag = m_builder.CreateOr(drawFlag, drawFlag1);
    }

    if (outVertsPerPrim > 2) {
      // drawFlag |= N >= 2 ? (primData[N-2] != NullPrim) : false
      auto primData2 = readPerThreadDataFromLds(
          m_builder.getInt32Ty(), m_builder.CreateSub(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(2)),
          LdsRegionOutPrimData, SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
      auto drawFlag2 =
          m_builder.CreateSelect(m_builder.CreateICmpUGE(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(2)),
                                 m_builder.CreateICmpNE(primData2, m_builder.getInt32(NullPrim)), m_builder.getFalse());
      drawFlag = m_builder.CreateOr(drawFlag, drawFlag2);
    }

    m_builder.CreateBr(endCheckOutVertDrawFlagBlock);
  }

  // Construct ".endCheckOutVertDrawFlag"
  Value *drawMask = nullptr;
  Value *outVertCountInWave = nullptr;
  {
    m_builder.SetInsertPoint(endCheckOutVertDrawFlagBlock);

    // NOTE: The predecessors are different if culling mode is enabled.
    drawFlag = createPhi({{drawFlag, checkOutVertDrawFlagBlock},
                          {m_builder.getFalse(), cullingMode ? endCullingBlock : endInitOutVertCountBlock}},
                         "drawFlag");
    drawMask = doSubgroupBallot(drawFlag);

    outVertCountInWave = m_builder.CreateIntrinsic(Intrinsic::ctpop, m_builder.getInt64Ty(), drawMask);
    outVertCountInWave = m_builder.CreateTrunc(outVertCountInWave, m_builder.getInt32Ty());

    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(waveCountInSubgroup), m_nggInputs.waveIdInSubgroup);
    auto threadValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);

    m_builder.CreateCondBr(threadValid, accumOutVertCountBlock, endAccumOutVertCountBlock);
  }

  // Construct ".accumOutVertCount" block
  {
    m_builder.SetInsertPoint(accumOutVertCountBlock);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));
    ldsOffset = m_builder.CreateShl(ldsOffset, 2);

    unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

    ldsOffset = m_builder.CreateAdd(
        ldsOffset,
        m_builder.getInt32(regionStart + (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * rasterStream));
    m_ldsManager->atomicOpWithLds(AtomicRMWInst::Add, outVertCountInWave, ldsOffset);

    m_builder.CreateBr(endAccumOutVertCountBlock);
  }

  // Construct ".endAccumOutVertCount" block
  Value *vertCountInPrevWaves = nullptr;
  {
    m_builder.SetInsertPoint(endAccumOutVertCountBlock);

    createFenceAndBarrier();

    if (!m_nggControl->compactVertex) {
      auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
    } else {
      auto outVertCountInWaves =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, LdsRegionOutVertCountInWaves,
                                   (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * rasterStream);

      // The last dword following dwords for all waves (each wave has one dword) stores GS output vertex count of the
      // entire subgroup
      auto vertCountInSubgroup = m_builder.CreateIntrinsic(
          Intrinsic::amdgcn_readlane, {}, {outVertCountInWaves, m_builder.getInt32(waveCountInSubgroup)});

      // Get output vertex count for all waves prior to this wave
      vertCountInPrevWaves = m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {},
                                                       {outVertCountInWaves, m_nggInputs.waveIdInSubgroup});

      auto vertCompacted = m_builder.CreateICmpULT(vertCountInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(m_builder.CreateAnd(drawFlag, vertCompacted), compactOutVertIdBlock,
                             endCompactOutVertIdBlock);

      m_nggInputs.vertCountInSubgroup = vertCountInSubgroup; // Update GS output vertex count in subgroup
      m_nggInputs.vertCompacted = vertCompacted;             // Record vertex compaction flag
    }
  }

  Value *compactedVertexIndex = nullptr;
  if (m_nggControl->compactVertex) {
    // Construct ".compactOutVertId" block
    {
      m_builder.SetInsertPoint(compactOutVertIdBlock);

      auto drawMaskVec = m_builder.CreateBitCast(drawMask, FixedVectorType::get(m_builder.getInt32Ty(), 2));

      auto drawMaskLow = m_builder.CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
      compactedVertexIndex =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder.getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder.CreateExtractElement(drawMaskVec, 1);
        compactedVertexIndex =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactedVertexIndex});
      }

      compactedVertexIndex = m_builder.CreateAdd(vertCountInPrevWaves, compactedVertexIndex);
      writePerThreadDataToLds(m_nggInputs.threadIdInSubgroup, compactedVertexIndex, LdsRegionOutVertThreadIdMap);

      m_builder.CreateBr(endCompactOutVertIdBlock);
    }

    // Construct ".endCompactOutVertId" block
    {
      m_builder.SetInsertPoint(endCompactOutVertIdBlock);

      compactedVertexIndex = createPhi(
          {{compactedVertexIndex, compactOutVertIdBlock}, {m_nggInputs.threadIdInSubgroup, endAccumOutVertCountBlock}});

      auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
    }
  }

  // Construct ".allocReq" block
  {
    m_builder.SetInsertPoint(allocReqBlock);

    doParamCacheAllocRequest();
    m_builder.CreateBr(endAllocReqBlock);
  }

  // Construct ".endAllocReq" block
  {
    m_builder.SetInsertPoint(endAllocReqBlock);

    // NOTE: This barrier is not necessary if we disable vertex compaction.
    if (m_nggControl->compactVertex)
      createFenceAndBarrier();

    auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(primValid, expPrimBlock, endExpPrimBlock);
  }

  // Construct ".expPrim" block
  {
    m_builder.SetInsertPoint(expPrimBlock);

    doPrimitiveExportWithGs(m_nggControl->compactVertex ? compactedVertexIndex : m_nggInputs.threadIdInSubgroup);
    m_builder.CreateBr(endExpPrimBlock);
  }

  // Construct ".endExpPrim" block
  {
    m_builder.SetInsertPoint(endExpPrimBlock);

    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    if (m_nggControl->compactVertex)
      m_builder.CreateCondBr(vertValid, expVertBlock, endExpVertBlock);
    else
      m_builder.CreateCondBr(vertValid, checkEmptyWaveBlock, endExpVertBlock);
  }

  if (!m_nggControl->compactVertex) {
    // Construct ".checkEmptyWave" block
    {
      m_builder.SetInsertPoint(checkEmptyWaveBlock);

      auto emptyWave = m_builder.CreateICmpEQ(outVertCountInWave, m_builder.getInt32(0));
      m_builder.CreateCondBr(emptyWave, emptyWaveExpBlock, noEmptyWaveExpBlock);
    }

    // Construct ".emptyWaveExp" block
    {
      m_builder.SetInsertPoint(emptyWaveExpBlock);

      auto undef = UndefValue::get(m_builder.getFloatTy());
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                {
                                    m_builder.getInt32(EXP_TARGET_POS_0), // tgt
                                    m_builder.getInt32(0x0),              // en
                                    // src0 ~ src3
                                    undef, undef, undef, undef,
                                    m_builder.getTrue(), // done
                                    m_builder.getFalse() // vm
                                });

      m_builder.CreateRetVoid();
    }

    // Construct ".noEmptyWaveExp" block
    {
      m_builder.SetInsertPoint(noEmptyWaveExpBlock);

      m_builder.CreateCondBr(drawFlag, expVertBlock, endExpVertBlock);
    }
  }

  // Construct ".expVert" block
  {
    m_builder.SetInsertPoint(expVertBlock);

    runCopyShader(entryPoint->getParent(), entryPoint->arg_begin());
    m_builder.CreateBr(endExpVertBlock);
  }

  // Construct ".endExpVert" block
  {
    m_builder.SetInsertPoint(endExpVertBlock);

    m_builder.CreateRetVoid();
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

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder.getInt64(-1));

  auto threadIdInWave =
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), threadIdInWave});
  }

  auto primCountInSubgroup = createUBfe(mergedGroupInfo, 22, 9);
  auto vertCountInSubgroup = createUBfe(mergedGroupInfo, 12, 9);
  auto vertCountInWave = createUBfe(mergedWaveInfo, 0, 8);
  auto primCountInWave = createUBfe(mergedWaveInfo, 8, 8);
  auto waveIdInSubgroup = createUBfe(mergedWaveInfo, 24, 4);
  auto orderedWaveId = createUBfe(mergedGroupInfo, 0, 12);

  auto threadIdInSubgroup = m_builder.CreateMul(waveIdInSubgroup, m_builder.getInt32(waveSize));
  threadIdInSubgroup = m_builder.CreateAdd(threadIdInSubgroup, threadIdInWave);

  primCountInSubgroup->setName("primCountInSubgroup");
  vertCountInSubgroup->setName("vertCountInSubgroup");
  primCountInWave->setName("primCountInWave");
  vertCountInWave->setName("vertCountInWave");
  threadIdInWave->setName("threadIdInWave");
  threadIdInSubgroup->setName("threadIdInSubgroup");
  waveIdInSubgroup->setName("waveIdInSubgroup");
  orderedWaveId->setName("orderedWaveId");

  // Record wave/thread info
  m_nggInputs.primCountInSubgroup = primCountInSubgroup;
  m_nggInputs.vertCountInSubgroup = vertCountInSubgroup;
  m_nggInputs.primCountInWave = primCountInWave;
  m_nggInputs.vertCountInWave = vertCountInWave;
  m_nggInputs.threadIdInWave = threadIdInWave;
  m_nggInputs.threadIdInSubgroup = threadIdInSubgroup;
  m_nggInputs.waveIdInSubgroup = waveIdInSubgroup;
  m_nggInputs.orderedWaveId = orderedWaveId;
}

// =====================================================================================================================
// Load stream-out info including stream-out buffer descriptors and buffer offsets.
//
// @param userData : User data
void NggPrimShader::loadStreamOutBufferInfo(Value *userData) {
  assert(m_gfxIp.major >= 11 && m_pipelineState->enableSwXfb()); // Must be GFX11+ and enable SW emulated stream-out

  // Get stream-out table pointer and stream-out control buffer pointer
  unsigned indexOfStreamOutTablePtr = InvalidValue;
  unsigned indexOfStreamOutControlBufPtr = InvalidValue;
  if (m_hasGs) {
    const auto &entryArgIdx = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
    indexOfStreamOutTablePtr = entryArgIdx.streamOutData.tablePtr;
    indexOfStreamOutControlBufPtr = entryArgIdx.streamOutData.controlBufPtr;
  } else if (m_hasTes) {
    const auto &entryArgIdx = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes;
    indexOfStreamOutTablePtr = entryArgIdx.streamOutData.tablePtr;
    indexOfStreamOutControlBufPtr = entryArgIdx.streamOutData.controlBufPtr;
  } else {
    const auto &entryArgIdx = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs;
    indexOfStreamOutTablePtr = entryArgIdx.streamOutData.tablePtr;
    indexOfStreamOutControlBufPtr = entryArgIdx.streamOutData.controlBufPtr;
  }

  // Helper to make a pointer from its integer address value and the type
  auto makePointer = [&](Value *ptrValue, Type *ptrTy) {
    Value *pc = m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
    pc = m_builder.CreateBitCast(pc, FixedVectorType::get(m_builder.getInt32Ty(), 2));

    Value *ptr = m_builder.CreateInsertElement(pc, ptrValue, static_cast<uint64_t>(0));
    ptr = m_builder.CreateBitCast(ptr, m_builder.getInt64Ty());
    ptr = m_builder.CreateIntToPtr(ptr, ptrTy, "globalTablePtr");

    return ptr;
  };

  auto streamOutBufDescTy = FixedVectorType::get(m_builder.getInt32Ty(), 4);
  auto streamOutTableTy = ArrayType::get(streamOutBufDescTy, MaxTransformFeedbackBuffers);
  auto streamOutTablePtrTy = PointerType::get(streamOutTableTy, ADDR_SPACE_CONST);

  assert(userData->getType()->isVectorTy());
  auto streamOutTablePtr =
      makePointer(m_builder.CreateExtractElement(userData, indexOfStreamOutTablePtr), streamOutTablePtrTy);

  // NOTE: The stream-out control buffer has the following memory layout:
  //   OFFSET[X]: OFFSET0, OFFSET1, ..., OFFSETN
  //   FILLED_SIZE[X]: FILLED_SIZE0, FILLED_SIZE1, ..., FILLED_SIZEN
  auto streamOutControlBufTy =
      ArrayType::get(FixedVectorType::get(m_builder.getInt32Ty(), MaxTransformFeedbackBuffers), 2);
  auto streamOutControlBufPtrTy = PointerType::get(streamOutControlBufTy, ADDR_SPACE_CONST);

  auto streamOutControlBufPtr =
      makePointer(m_builder.CreateExtractElement(userData, indexOfStreamOutControlBufPtr), streamOutControlBufPtrTy);

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    bool bufferActive = xfbStrides[i] > 0;
    if (!bufferActive)
      continue; // Transform feedback buffer inactive

    // Get stream-out buffer descriptors and record them
    auto streamOutBufDescPtr =
        m_builder.CreateGEP(streamOutTableTy, streamOutTablePtr, {m_builder.getInt32(0), m_builder.getInt32(i)});
    cast<GetElementPtrInst>(streamOutBufDescPtr)->setMetadata(MetaNameUniform, MDNode::get(m_builder.getContext(), {}));

    auto streamOutBufDesc = m_builder.CreateAlignedLoad(streamOutBufDescTy, streamOutBufDescPtr, Align(16));
    streamOutBufDesc->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));
    m_streamOutBufDescs[i] = streamOutBufDesc;

    // Get stream-out buffer offsets and record them
    auto streamOutBufOffsetPtr = m_builder.CreateGEP(streamOutControlBufTy, streamOutControlBufPtr,
                                                     {m_builder.getInt32(0),
                                                      m_builder.getInt32(0), // 0: OFFSET[X], 1: FILLED_SIZE[X]
                                                      m_builder.getInt32(i)});
    cast<GetElementPtrInst>(streamOutBufOffsetPtr)
        ->setMetadata(MetaNameUniform, MDNode::get(m_builder.getContext(), {}));

    auto streamOutBufOffset = m_builder.CreateAlignedLoad(m_builder.getInt32Ty(), streamOutBufOffsetPtr, Align(4));
    // NOTE: PAL decided not to invalidate the SQC and L1 for every stream-out update, mainly because that will hurt
    // overall performance worse than just forcing this one buffer to be read via L2. Since PAL would not have wider
    // context, PAL believed that they would have to perform that invalidation on every Set/Load unconditionally.
    // Thus, we force the load of stream-out control buffer to be volatile to let LLVM backend add GLC and DLC flags.
    streamOutBufOffset->setVolatile(true);

    m_streamOutBufOffsets[i] = streamOutBufOffset;
  }
}

// =====================================================================================================================
// Does various culling for NGG primitive shader.
//
// @param module : LLVM module
// @param vertexIndex0: Relative index of vertex0 (forming this primitive)
// @param vertexIndex1: Relative index of vertex1 (forming this primitive)
// @param vertexIndex2: Relative index of vertex2 (forming this primitive)
Value *NggPrimShader::doCulling(Module *module, Value *vertexIndex0, Value *vertexIndex1, Value *vertexIndex2) {
  // Skip following culling if it is not requested
  if (!enableCulling())
    return m_builder.getFalse();

  Value *cullFlag = m_builder.getFalse();

  Value *vertex0 = fetchVertexPositionData(vertexIndex0);
  Value *vertex1 = fetchVertexPositionData(vertexIndex1);
  Value *vertex2 = fetchVertexPositionData(vertexIndex2);

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
    Value *signMask0 = fetchCullDistanceSignMask(vertexIndex0);
    Value *signMask1 = fetchCullDistanceSignMask(vertexIndex1);
    Value *signMask2 = fetchCullDistanceSignMask(vertexIndex2);
    cullFlag = doCullDistanceCulling(module, cullFlag, signMask0, signMask1, signMask2);
  }

  return cullFlag;
}

// =====================================================================================================================
// Requests that parameter cache space be allocated (send the message GS_ALLOC_REQ).
void NggPrimShader::doParamCacheAllocRequest() {
  // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
  Value *m0 = m_builder.CreateShl(m_nggInputs.primCountInSubgroup, 12);
  m0 = m_builder.CreateOr(m0, m_nggInputs.vertCountInSubgroup);

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder.getInt32(GsAllocReq), m0});
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader (GS is not present).
//
// @param cullFlag : Cull flag indicating whether this primitive has been culled (could be null)
void NggPrimShader::doPrimitiveExportWithoutGs(Value *cullFlag) {
  Value *primData = nullptr;

  // Primitive data layout [31:0]
  //   [31]    = null primitive flag
  //   [28:20] = vertexIndex2
  //   [18:10] = vertexIndex1
  //   [8:0]   = vertexIndex0

  if (m_nggControl->passthroughMode) {
    // Pass-through mode (primitive data has been constructed)
    primData = m_nggInputs.primData;
  } else {
    // Culling mode (primitive data has to be constructed)
    Value *vertexIndex0 = m_nggInputs.esGsOffset0;
    Value *vertexIndex1 = m_nggInputs.esGsOffset1;
    Value *vertexIndex2 = m_nggInputs.esGsOffset2;

    //
    // The processing is something like this:
    //
    //   vertexIndices = Relative vertex indices
    //   if (compactVertex)
    //     vertexIndices = Read compacted relative vertex indices from LDS
    //   Export primitive
    //

    auto expPrimBlock = m_builder.GetInsertBlock();

    if (m_nggInputs.vertCompacted) {
      auto compactVertIdBlock = createBlock(expPrimBlock->getParent(), ".compactVertId");
      compactVertIdBlock->moveAfter(expPrimBlock);

      auto endCompactVertIdBlock = createBlock(expPrimBlock->getParent(), ".endCompactVertId");
      endCompactVertIdBlock->moveAfter(compactVertIdBlock);

      m_builder.CreateCondBr(m_nggInputs.vertCompacted, compactVertIdBlock, endCompactVertIdBlock);

      // Construct ".compactVertId" block
      Value *compactedVertexIndex0 = nullptr;
      Value *compactedVertexIndex1 = nullptr;
      Value *compactedVertexIndex2 = nullptr;
      {
        m_builder.SetInsertPoint(compactVertIdBlock);

        const unsigned esGsRingItemSize =
            m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;

        auto vertexItemOffset0 =
            m_builder.CreateMul(m_nggInputs.esGsOffset0, m_builder.getInt32(esGsRingItemSize * SizeOfDword));
        auto vertexItemOffset1 =
            m_builder.CreateMul(m_nggInputs.esGsOffset1, m_builder.getInt32(esGsRingItemSize * SizeOfDword));
        auto vertexItemOffset2 =
            m_builder.CreateMul(m_nggInputs.esGsOffset2, m_builder.getInt32(esGsRingItemSize * SizeOfDword));

        compactedVertexIndex0 = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset0,
                                                          m_vertCullInfoOffsets.compactedVertexIndex);
        compactedVertexIndex1 = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset1,
                                                          m_vertCullInfoOffsets.compactedVertexIndex);
        compactedVertexIndex2 = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset2,
                                                          m_vertCullInfoOffsets.compactedVertexIndex);

        m_builder.CreateBr(endCompactVertIdBlock);
      }

      // Construct ".endCompactVertId" block
      {
        m_builder.SetInsertPoint(endCompactVertIdBlock);

        vertexIndex0 = createPhi({{compactedVertexIndex0, compactVertIdBlock}, {vertexIndex0, expPrimBlock}});
        vertexIndex1 = createPhi({{compactedVertexIndex1, compactVertIdBlock}, {vertexIndex1, expPrimBlock}});
        vertexIndex2 = createPhi({{compactedVertexIndex2, compactVertIdBlock}, {vertexIndex2, expPrimBlock}});
      }
    }

    primData = m_builder.CreateShl(vertexIndex2, 10);
    primData = m_builder.CreateOr(primData, vertexIndex1);

    primData = m_builder.CreateShl(primData, 10);
    primData = m_builder.CreateOr(primData, vertexIndex0);

    // Check cull flag to determine whether this primitive is culled if the cull flag is specified.
    if (cullFlag)
      primData = m_builder.CreateSelect(cullFlag, m_builder.getInt32(NullPrim), primData);
  }

  auto undef = UndefValue::get(m_builder.getInt32Ty());

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                            {
                                m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                m_builder.getInt32(0x1),             // en
                                // src0 ~ src3
                                primData, undef, undef, undef,
                                m_builder.getTrue(),  // done, must be set
                                m_builder.getFalse(), // vm
                            });
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader (GS is present).
//
// @param vertexIndex : The relative index of starting vertex (Indices of vertices forming a GS primitive must be
//                      consecutive)
void NggPrimShader::doPrimitiveExportWithGs(Value *vertexIndex) {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   primData = Read primitive data from LDS
  //   if (valid primitive) {
  //     if (points)
  //       primData = vertexIndex0
  //     else if (line_strip) {
  //       primData = <vertexIndex0, vertexIndex1>
  //     } else if (triangle_strip) {
  //       winding = primData != 0
  //       primData = winding ? <vertexIndex0, vertexIndex2, vertexIndex1>
  //                          : <vertexIndex0, vertexIndex1, vertexIndex2>
  //     }
  //   }
  //   Export primitive data
  //
  //
  // Primitive data layout [31:0]
  //   [31]    = null primitive flag
  //   [28:20] = vertexIndex2
  //   [18:10] = vertexIndex1
  //   [8:0]   = vertexIndex0
  //
  const auto rasterStream = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.rasterStream;
  Value *primData =
      readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                               SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * rasterStream);

  auto primValid = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

  Value *newPrimData = nullptr;
  switch (geometryMode.outputPrimitive) {
  case OutputPrimitives::Points:
    newPrimData = vertexIndex;
    break;
  case OutputPrimitives::LineStrip: {
    Value *vertexIndex0 = vertexIndex;
    Value *vertexIndex1 = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(1));
    newPrimData = m_builder.CreateOr(m_builder.CreateShl(vertexIndex1, 10), vertexIndex0);
    break;
  }
  case OutputPrimitives::TriangleStrip: {
    Value *winding = m_builder.CreateICmpNE(primData, m_builder.getInt32(0));
    Value *vertexIndex0 = vertexIndex;
    Value *vertexIndex1 = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(1));
    Value *vertexIndex2 = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(2));
    auto newPrimDataNoWinding = m_builder.CreateOr(
        m_builder.CreateShl(m_builder.CreateOr(m_builder.CreateShl(vertexIndex2, 10), vertexIndex1), 10), vertexIndex0);
    auto newPrimDataWinding = m_builder.CreateOr(
        m_builder.CreateShl(m_builder.CreateOr(m_builder.CreateShl(vertexIndex1, 10), vertexIndex2), 10), vertexIndex0);
    newPrimData = m_builder.CreateSelect(winding, newPrimDataWinding, newPrimDataNoWinding);
    break;
  }
  default:
    llvm_unreachable("Unexpected output primitive type!");
    break;
  }

  primData = m_builder.CreateSelect(primValid, newPrimData, primData);

  auto undef = UndefValue::get(m_builder.getInt32Ty());

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                            {
                                m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                m_builder.getInt32(0x1),             // en
                                primData, undef, undef, undef,       // src0 ~ src3
                                m_builder.getTrue(),                 // done, must be set
                                m_builder.getFalse(),                // vm
                            });
}

// =====================================================================================================================
// Early exit NGG primitive shader when we detect that the entire subgroup is fully culled, doing dummy
// primitive/vertex export if necessary.
//
// @param fullyCulledExportCount : Primitive/vertex count for dummy export when the entire subgroup is fully culled
void NggPrimShader::doEarlyExit(unsigned fullyCulledExportCount) {
  if (fullyCulledExportCount > 0) {
    assert(fullyCulledExportCount == 1); // Currently, if workarounded, this is set to 1

    auto earlyExitBlock = m_builder.GetInsertBlock();

    auto dummyExpBlock = createBlock(earlyExitBlock->getParent(), ".dummyExp");
    dummyExpBlock->moveAfter(earlyExitBlock);

    auto endDummyExpBlock = createBlock(earlyExitBlock->getParent(), ".endDummyExp");
    endDummyExpBlock->moveAfter(dummyExpBlock);

    // Construct ".earlyExit" block
    {
      auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, dummyExpBlock, endDummyExpBlock);
    }

    // Construct ".dummyExp" block
    {
      m_builder.SetInsertPoint(dummyExpBlock);

      auto undef = UndefValue::get(m_builder.getInt32Ty());

      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                                {
                                    m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                    m_builder.getInt32(0x1),             // en
                                    // src0 ~ src3
                                    m_builder.getInt32(0), undef, undef, undef,
                                    m_builder.getTrue(), // done
                                    m_builder.getFalse() // vm
                                });

      // Determine how many dummy position exports we need
      unsigned posExpCount = 1;
      if (m_hasGs) {
        const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

        bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
        miscExport |= builtInUsage.primitiveShadingRate;
        if (miscExport)
          ++posExpCount;

        posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
      } else if (m_hasTes) {
        const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.tes;

        bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
        if (miscExport)
          ++posExpCount;

        posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
      } else {
        const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.vs;

        bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
        miscExport |= builtInUsage.primitiveShadingRate;
        if (miscExport)
          ++posExpCount;

        posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
      }

      undef = UndefValue::get(m_builder.getFloatTy());

      for (unsigned i = 0; i < posExpCount; ++i) {
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                  {
                                      m_builder.getInt32(EXP_TARGET_POS_0 + i), // tgt
                                      m_builder.getInt32(0x0),                  // en
                                      // src0 ~ src3
                                      undef, undef, undef, undef,
                                      m_builder.getInt1(i == posExpCount - 1), // done
                                      m_builder.getFalse()                     // vm
                                  });
      }

      m_builder.CreateBr(endDummyExpBlock);
    }

    // Construct ".endDummyExp" block
    {
      m_builder.SetInsertPoint(endDummyExpBlock);
      m_builder.CreateRetVoid();
    }
  } else
    m_builder.CreateRetVoid();
}

// =====================================================================================================================
// Runs ES.
//
// @param module : LLVM module
// @param sysValueStart : Start of system value
void NggPrimShader::runEs(Module *module, Argument *sysValueStart) {
  if (!m_hasTes && !m_hasVs) {
    // No TES or VS, don't have to run
    return;
  }

  auto esEntry = module->getFunction(lgcName::NggEsEntryPoint);
  assert(esEntry);

  if (m_gfxIp.major >= 11 && !m_hasGs) // For GS, vertex attribute exports are in copy shader
    processVertexAttribExport(esEntry);

  // Call ES entry
  Argument *arg = sysValueStart;

  Value *esGsOffset = nullptr;
  if (m_hasGs) {
    auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
    unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    unsigned esGsBytesPerWave = waveSize * SizeOfDword * calcFactor.esGsRingItemSize;
    esGsOffset = m_builder.CreateMul(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(esGsBytesPerWave));
  }

  Value *offChipLdsBase = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase));
  offChipLdsBase->setName("offChipLdsBase");

  Value *isOffChip = UndefValue::get(m_builder.getInt32Ty()); // NOTE: This flag is unused.

  arg += NumSpecialSgprInputs;

  Value *userData = arg++;

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *relVertexId = (arg + 6);
  // NOTE: VS primitive ID for NGG is specially obtained from primitive ID distribution.
  Value *vsPrimitiveId = m_distributedPrimitiveId ? m_distributedPrimitiveId : PoisonValue::get(m_builder.getInt32Ty());
  Value *instanceId = (arg + 8);

  std::vector<Value *> args;

  // Setup attribute ring base and vertex thread ID in subgroup as two additional arguments to export vertex attributes
  // through memory
  if (m_gfxIp.major >= 11 && !m_hasGs) { // For GS, vertex attribute exports are in copy shader
    const auto attribCount = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)
                                 ->inOutUsage.expCount;
    if (attribCount > 0) {
      args.push_back(m_nggInputs.attribRingBase);
      args.push_back(m_nggInputs.threadIdInSubgroup);
    }
  }

  auto intfData = m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
  const unsigned userDataCount = intfData->userDataCount;

  unsigned userDataIdx = 0;

  auto esArgBegin = esEntry->arg_begin();
  const unsigned esArgCount = esEntry->arg_size();
  (void(esArgCount)); // Unused

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(args.size() < esArgCount);

    auto esArg = (esArgBegin + args.size());
    assert(esArg->hasAttribute(Attribute::InReg));

    auto esArgTy = esArg->getType();
    if (esArgTy->isVectorTy()) {
      assert(cast<VectorType>(esArgTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<FixedVectorType>(esArgTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto esUserData = m_builder.CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(esUserData);
    } else {
      assert(esArgTy->isIntegerTy());

      auto esUserData = m_builder.CreateExtractElement(userData, userDataIdx);
      args.push_back(esUserData);
      ++userDataIdx;
    }
  }

  if (m_hasTes) {
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

    // When tessellation is not enabled, the ES is actually a fetchless VS. Then, we need to add arguments for the
    // vertex fetches. Also set the name of each vertex fetch primitive shader argument while we're here.
    unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
    if (vertexFetchCount != 0) {
      // The last vertexFetchCount arguments of the primitive shader and ES are the vertex fetches
      Function *primShader = m_builder.GetInsertBlock()->getParent();
      unsigned primArgCount = primShader->arg_size();
      for (unsigned i = 0; i != vertexFetchCount; ++i) {
        Argument *vertexFetch = primShader->getArg(primArgCount - vertexFetchCount + i);
        vertexFetch->setName(esEntry->getArg(esArgCount - vertexFetchCount + i)->getName()); // Copy argument name
        args.push_back(vertexFetch);
      }
    }
  }

  assert(args.size() == esArgCount); // Must have visit all arguments of ES entry point

  CallInst *esCall = m_builder.CreateCall(esEntry, args);
  esCall->setCallingConv(CallingConv::AMDGPU_ES);
}

// =====================================================================================================================
// Runs ES-partial. Before doing this, ES must have been already split to two parts: one is to fetch cull data for
// NGG culling; the other is to do deferred vertex export.
//
// @param module : LLVM module
// @param sysValueStart : Start of system value
// @param position : Vertex position data (if provided, the ES-partial is to do deferred vertex export)
Value *NggPrimShader::runEsPartial(Module *module, Argument *sysValueStart, Value *position) {
  assert(m_hasGs == false);                       // GS must not be present
  assert(m_nggControl->passthroughMode == false); // NGG culling is enabled

  const bool deferredVertexExport = position != nullptr;
  Function *esPartialEntry =
      module->getFunction(deferredVertexExport ? lgcName::NggEsDeferredVertexExport : lgcName::NggEsCullDataFetch);
  assert(esPartialEntry);

  // Call ES-partial entry
  Argument *arg = sysValueStart;

  Value *offChipLdsBase = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase));
  offChipLdsBase->setName("offChipLdsBase");

  Value *isOffChip = UndefValue::get(m_builder.getInt32Ty()); // NOTE: This flag is unused.

  arg += NumSpecialSgprInputs;

  Value *userData = arg++;

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *relVertexId = (arg + 6);
  // NOTE: VS primitive ID for NGG is specially obtained from primitive ID distribution.
  Value *vsPrimitiveId = m_distributedPrimitiveId ? m_distributedPrimitiveId : PoisonValue::get(m_builder.getInt32Ty());
  Value *instanceId = (arg + 8);

  if (deferredVertexExport && m_nggInputs.vertCompacted) {
    auto expVertBlock = m_builder.GetInsertBlock();

    auto uncompactVertBlock = createBlock(expVertBlock->getParent(), ".uncompactVert");
    uncompactVertBlock->moveAfter(expVertBlock);

    auto endUncompactVertBlock = createBlock(expVertBlock->getParent(), ".endUncompactVert");
    endUncompactVertBlock->moveAfter(uncompactVertBlock);

    m_builder.CreateCondBr(m_nggInputs.vertCompacted, uncompactVertBlock, endUncompactVertBlock);

    // Construct ".uncompactVert" block
    Value *newPosition = nullptr;
    Value *newTessCoordX = nullptr;
    Value *newTessCoordY = nullptr;
    Value *newRelPatchId = nullptr;
    Value *newPatchId = nullptr;
    Value *newVertexId = nullptr;
    Value *newVsPrimitiveId = nullptr;
    Value *newInstanceId = nullptr;
    {
      m_builder.SetInsertPoint(uncompactVertBlock);

      const unsigned esGsRingItemSize =
          m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;

      auto uncompactedVertexIndex =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionVertThreadIdMap);
      auto vertexItemOffset =
          m_builder.CreateMul(uncompactedVertexIndex, m_builder.getInt32(esGsRingItemSize * SizeOfDword));

      newPosition = readPerThreadDataFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), uncompactedVertexIndex,
                                             LdsRegionVertPosData, true);

      // NOTE: For deferred vertex export, some system values could be from vertex compaction info rather than from
      // VGPRs (caused by NGG culling and vertex compaction)
      const auto resUsage = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
      if (m_hasTes) {
        if (resUsage->builtInUsage.tes.tessCoord) {
          newTessCoordX =
              readVertexCullInfoFromLds(m_builder.getFloatTy(), vertexItemOffset, m_vertCullInfoOffsets.tessCoordX);
          newTessCoordY =
              readVertexCullInfoFromLds(m_builder.getFloatTy(), vertexItemOffset, m_vertCullInfoOffsets.tessCoordY);
        }

        newRelPatchId =
            readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.relPatchId);

        if (resUsage->builtInUsage.tes.primitiveId) {
          newPatchId =
              readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.patchId);
        }
      } else {
        if (resUsage->builtInUsage.vs.vertexIndex) {
          newVertexId =
              readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.vertexId);
        }

        // NOTE: Relative vertex ID is not used when VS is merged to GS.

        if (resUsage->builtInUsage.vs.primitiveId) {
          newVsPrimitiveId =
              readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.primitiveId);
        }

        if (resUsage->builtInUsage.vs.instanceIndex) {
          newInstanceId =
              readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.instanceId);
        }
      }
      m_builder.CreateBr(endUncompactVertBlock);
    }

    // Construct ".endUncompactVert" block
    {
      m_builder.SetInsertPoint(endUncompactVertBlock);

      position = createPhi({{newPosition, uncompactVertBlock}, {position, expVertBlock}});

      if (m_hasTes) {
        if (newTessCoordX)
          tessCoordX = createPhi({{newTessCoordX, uncompactVertBlock}, {tessCoordX, expVertBlock}});

        if (newTessCoordY)
          tessCoordX = createPhi({{newTessCoordY, uncompactVertBlock}, {tessCoordY, expVertBlock}});

        assert(newRelPatchId);
        relPatchId = createPhi({{newRelPatchId, uncompactVertBlock}, {relPatchId, expVertBlock}});

        if (newPatchId)
          patchId = createPhi({{newPatchId, uncompactVertBlock}, {patchId, expVertBlock}});
      } else {
        if (newVertexId)
          vertexId = createPhi({{newVertexId, uncompactVertBlock}, {vertexId, expVertBlock}});

        if (newVsPrimitiveId)
          vsPrimitiveId = createPhi({{newVsPrimitiveId, uncompactVertBlock}, {vsPrimitiveId, expVertBlock}});

        if (newInstanceId)
          instanceId = createPhi({{newInstanceId, uncompactVertBlock}, {instanceId, expVertBlock}});
      }
    }
  }

  std::vector<Value *> args;

  // Setup attribute ring base and vertex thread ID in subgroup as two additional arguments to export vertex attributes
  // through memory
  if (m_gfxIp.major >= 11 && deferredVertexExport) {
    const auto attribCount = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)
                                 ->inOutUsage.expCount;
    if (attribCount > 0) {
      args.push_back(m_nggInputs.attribRingBase);
      args.push_back(m_nggInputs.threadIdInSubgroup);
    }
  }

  if (deferredVertexExport)
    args.push_back(position); // Setup vertex position data as the additional argument

  auto intfData = m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
  const unsigned userDataCount = intfData->userDataCount;

  unsigned userDataIdx = 0;

  auto esPartialArgBegin = esPartialEntry->arg_begin();
  const unsigned esPartialArgCount = esPartialEntry->arg_size();
  (void(esPartialArgCount)); // Unused

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(args.size() < esPartialArgCount);

    auto esPartialArg = (esPartialArgBegin + args.size());
    assert(esPartialArg->hasAttribute(Attribute::InReg));

    auto esPartialArgTy = esPartialArg->getType();
    if (esPartialArgTy->isVectorTy()) {
      assert(cast<VectorType>(esPartialArgTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<FixedVectorType>(esPartialArgTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto esUserData = m_builder.CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(esUserData);
    } else {
      assert(esPartialArgTy->isIntegerTy());

      auto esUserData = m_builder.CreateExtractElement(userData, userDataIdx);
      args.push_back(esUserData);
      ++userDataIdx;
    }
  }

  if (m_hasTes) {
    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      args.push_back(isOffChip);
      args.push_back(offChipLdsBase);
    }

    // Set up system value VGPRs
    args.push_back(tessCoordX);
    args.push_back(tessCoordY);
    args.push_back(relPatchId);
    args.push_back(patchId);
  } else {
    // Set up system value VGPRs
    args.push_back(vertexId);
    args.push_back(relVertexId);
    args.push_back(vsPrimitiveId);
    args.push_back(instanceId);
  }

  assert(args.size() == esPartialArgCount); // Must have visit all arguments of ES-partial entry point

  CallInst *esPartialCall = m_builder.CreateCall(esPartialEntry, args);
  esPartialCall->setCallingConv(CallingConv::AMDGPU_ES);
  return esPartialCall;
}

// =====================================================================================================================
// Split ES to two parts. One is to fetch cull data for NGG culling, such as position and cull distance (if cull
// distance culling is enabled). The other is to do deferred vertex export like original ES.
//
// @param module : LLVM module
// @param fetchData : Whether the mutation is to fetch data
void NggPrimShader::splitEs(Module *module) {
  assert(m_hasGs == false); // GS must not be present

  const auto esEntryPoint = module->getFunction(lgcName::NggEsEntryPoint);
  assert(esEntryPoint);

  //
  // Collect all export calls for further analysis
  //
  SmallVector<Function *, 8> expFuncs;
  for (auto &func : module->functions()) {
    if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp)
      expFuncs.push_back(&func);
    else if (m_gfxIp.major >= 11) {
      if (func.getName().startswith(lgcName::NggAttribExport) || func.getName().startswith(lgcName::NggXfbOutputExport))
        expFuncs.push_back(&func);
    }
  }

  //
  // Preparation for fetching cull distances
  //
  unsigned clipCullPos = EXP_TARGET_POS_1;
  unsigned clipDistanceCount = 0;
  unsigned cullDistanceCount = 0;

  if (m_nggControl->enableCullDistanceCulling) {
    const auto &resUsage = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);

    if (m_hasTes) {
      const auto &builtInUsage = resUsage->builtInUsage.tes;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      clipCullPos = miscExport ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    } else {
      const auto &builtInUsage = resUsage->builtInUsage.vs;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      miscExport |= builtInUsage.primitiveShadingRate;
      clipCullPos = miscExport ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    }

    assert(cullDistanceCount > 0); // Cull distance must exist if the culling is enabled
  }

  //
  // Create ES-partial to fetch cull data for NGG culling
  //
  const auto positionTy = FixedVectorType::get(m_builder.getFloatTy(), 4);
  const auto cullDistanceTy = ArrayType::get(m_builder.getFloatTy(), cullDistanceCount);

  Type *cullDataTy = positionTy;
  if (m_nggControl->enableCullDistanceCulling)
    cullDataTy = StructType::get(m_builder.getContext(), {positionTy, cullDistanceTy});

  // Clone ES
  auto esCullDataFetchFuncTy = FunctionType::get(cullDataTy, esEntryPoint->getFunctionType()->params(), false);
  auto esCullDataFetchFunc = Function::Create(esCullDataFetchFuncTy, esEntryPoint->getLinkage(), "", module);

  ValueToValueMapTy valueMap;

  Argument *newArg = esCullDataFetchFunc->arg_begin();
  for (Argument &arg : esEntryPoint->args())
    valueMap[&arg] = newArg++;

  SmallVector<ReturnInst *, 8> retInsts;
  CloneFunctionInto(esCullDataFetchFunc, esEntryPoint, valueMap, CloneFunctionChangeType::LocalChangesOnly, retInsts);
  esCullDataFetchFunc->setName(lgcName::NggEsCullDataFetch);

  // Find the return block, remove all exports, and mutate return type
  BasicBlock *retBlock = nullptr;
  for (BasicBlock &block : *esCullDataFetchFunc) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      retInst->dropAllReferences();
      retInst->eraseFromParent();

      retBlock = &block;
      break;
    }
  }
  assert(retBlock);

  IRBuilder<>::InsertPointGuard guard(m_builder);
  m_builder.SetInsertPoint(retBlock);

  SmallVector<CallInst *, 8> removeCalls;

  // Fetch position and cull distances
  Value *position = UndefValue::get(positionTy);
  SmallVector<Value *, MaxClipCullDistanceCount> clipCullDistance(MaxClipCullDistanceCount);

  for (auto func : expFuncs) {
    for (auto user : func->users()) {
      CallInst *const call = cast<CallInst>(user);

      if (call->getParent()->getParent() != esCullDataFetchFunc)
        continue; // Export call doesn't belong to targeted function, skip

      assert(call->getParent() == retBlock); // Must in return block

      if (func->isIntrinsic() && func->getIntrinsicID() == Intrinsic::amdgcn_exp) {
        unsigned exportTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        if (exportTarget == EXP_TARGET_POS_0) {
          // Get position value
          m_constPositionZ = isa<Constant>(call->getArgOperand(4));
          for (unsigned i = 0; i < 4; ++i)
            position = m_builder.CreateInsertElement(position, call->getArgOperand(2 + i), i);
        } else if (exportTarget == clipCullPos) {
          // Get clip/cull distance value
          if (m_nggControl->enableCullDistanceCulling) {
            clipCullDistance[0] = call->getArgOperand(2);
            clipCullDistance[1] = call->getArgOperand(3);
            clipCullDistance[2] = call->getArgOperand(4);
            clipCullDistance[3] = call->getArgOperand(5);
          }
        } else if (exportTarget == clipCullPos + 1 && clipDistanceCount + cullDistanceCount > 4) {
          // Get clip/cull distance value
          if (m_nggControl->enableCullDistanceCulling) {
            clipCullDistance[4] = call->getArgOperand(2);
            clipCullDistance[5] = call->getArgOperand(3);
            clipCullDistance[6] = call->getArgOperand(4);
            clipCullDistance[7] = call->getArgOperand(5);
          }
        }
      }

      removeCalls.push_back(call); // Remove export
    }
  }

  Value *cullData = position;
  if (m_nggControl->enableCullDistanceCulling) {
    Value *cullDistance = UndefValue::get(cullDistanceTy);

    for (unsigned i = 0; i < cullDistanceCount; ++i)
      cullDistance = m_builder.CreateInsertValue(cullDistance, clipCullDistance[clipDistanceCount + i], i);

    cullData = m_builder.CreateInsertValue(UndefValue::get(cullDataTy), position, 0);
    cullData = m_builder.CreateInsertValue(cullData, cullDistance, 1);
  }

  m_builder.CreateRet(cullData);

  //
  // Create ES-partial to do deferred vertex export after NGG culling
  //

  // NOTE: Here, we just mutate original ES to do deferred vertex export. We add vertex position data as an additional
  // argument. This could avoid re-fetching it since we already get the data before NGG culling.
  auto esDeferredVertexExportFunc = addFunctionArgs(esEntryPoint, nullptr, {positionTy}, {"position"});
  esDeferredVertexExportFunc->setName(lgcName::NggEsDeferredVertexExport);

  position = esDeferredVertexExportFunc->getArg(0); // The first argument is vertex position data
  assert(position->getType() == positionTy);

  for (auto func : expFuncs) {
    for (auto user : func->users()) {
      CallInst *const call = cast<CallInst>(user);

      if (call->getParent()->getParent() != esDeferredVertexExportFunc)
        continue; // Export call doesn't belong to targeted function, skip

      if (func->isIntrinsic() && func->getIntrinsicID() == Intrinsic::amdgcn_exp) {
        unsigned exportTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        if (exportTarget == EXP_TARGET_POS_0) {
          // Replace vertex position data
          m_builder.SetInsertPoint(call);
          call->setArgOperand(2, m_builder.CreateExtractElement(position, static_cast<uint64_t>(0)));
          call->setArgOperand(3, m_builder.CreateExtractElement(position, 1));
          call->setArgOperand(4, m_builder.CreateExtractElement(position, 2));
          call->setArgOperand(5, m_builder.CreateExtractElement(position, 3));
        }
      }
    }
  }

  if (m_gfxIp.major >= 11)
    processVertexAttribExport(esDeferredVertexExportFunc);

  // Original ES is no longer needed
  assert(esEntryPoint->use_empty());
  esEntryPoint->eraseFromParent();

  // Remove calls
  for (auto call : removeCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
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

  Value *gsVsOffset = UndefValue::get(m_builder.getInt32Ty()); // NOTE: For NGG, GS-VS offset is unused

  // NOTE: This argument is expected to be GS wave ID, not wave ID in subgroup, for normal ES-GS merged shader.
  // However, in NGG mode, GS wave ID, sent to GS_EMIT and GS_CUT messages, is no longer required because of NGG
  // handling of such messages. Instead, wave ID in subgroup is required as the substitute.
  auto waveId = m_nggInputs.waveIdInSubgroup;

  arg += NumSpecialSgprInputs;

  Value *userData = arg++;

  Value *gsPrimitiveId = (arg + 2);
  Value *invocationId = (arg + 3);

  // NOTE: For NGG, GS invocation ID is stored in lowest 8 bits ([7:0]) and other higher bits are used for other
  // purposes according to GE-SPI interface.
  invocationId = m_builder.CreateAnd(invocationId, m_builder.getInt32(0xFF));

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

      const unsigned userDataSize = cast<FixedVectorType>(gsArgTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto gsUserData = m_builder.CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(gsUserData);
    } else {
      assert(gsArgTy->isIntegerTy());

      auto gsUserData = m_builder.CreateExtractElement(userData, userDataIdx);
      args.push_back(gsUserData);
      ++userDataIdx;
    }
  }

  // Set up system value SGPRs
  args.push_back(gsVsOffset);
  args.push_back(waveId);

  // Set up system value VGPRs
  args.push_back(m_nggInputs.esGsOffset0);
  args.push_back(m_nggInputs.esGsOffset1);
  args.push_back(gsPrimitiveId);
  args.push_back(m_nggInputs.esGsOffset2);
  args.push_back(m_nggInputs.esGsOffset3);
  args.push_back(m_nggInputs.esGsOffset4);
  args.push_back(m_nggInputs.esGsOffset5);
  args.push_back(invocationId);

  assert(args.size() == gsArgCount); // Must have visit all arguments of ES entry point

  CallInst *gsCall = m_builder.CreateCall(gsEntry, args);
  gsCall->setCallingConv(CallingConv::AMDGPU_GS);
}

// =====================================================================================================================
// Mutates GS to handle exporting GS outputs to GS-VS ring, and the messages GS_EMIT/GS_CUT.
//
// @param module : LLVM module
Function *NggPrimShader::mutateGs(Module *module) {
  assert(m_hasGs); // GS must be present

  auto gsEntryPoint = module->getFunction(lgcName::NggGsEntryPoint);
  assert(gsEntryPoint);

  IRBuilder<>::InsertPointGuard guard(m_builder);

  std::vector<Instruction *> removeCalls;

  m_builder.SetInsertPointPastAllocas(gsEntryPoint);

  // Initialize counters of GS emitted vertices and GS output vertices of current primitive
  Value *emitVertsPtrs[MaxGsStreams] = {};
  Value *outVertsPtrs[MaxGsStreams] = {};

  for (int i = 0; i < MaxGsStreams; ++i) {
    auto emitVertsPtr = m_builder.CreateAlloca(m_builder.getInt32Ty());
    m_builder.CreateStore(m_builder.getInt32(0), emitVertsPtr); // emitVerts = 0
    emitVertsPtrs[i] = emitVertsPtr;

    auto outVertsPtr = m_builder.CreateAlloca(m_builder.getInt32Ty());
    m_builder.CreateStore(m_builder.getInt32(0), outVertsPtr); // outVerts = 0
    outVertsPtrs[i] = outVertsPtr;
  }

  // Initialize thread ID in wave
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  auto threadIdInWave =
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), threadIdInWave});
  }

  // Initialize thread ID in subgroup
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
  auto waveId = getFunctionArgument(gsEntryPoint, entryArgIdxs.gsWaveId);

  auto threadIdInSubgroup = m_builder.CreateMul(waveId, m_builder.getInt32(waveSize));
  threadIdInSubgroup = m_builder.CreateAdd(threadIdInSubgroup, threadIdInWave);

  // Handle GS message and GS output export
  for (auto &func : module->functions()) {
    if (func.getName().startswith(lgcName::NggGsOutputExport)) {
      // Export GS outputs to GS-VS ring
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);
        m_builder.SetInsertPoint(call);

        assert(call->arg_size() == 4);
        const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
        const unsigned compIdx = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
        assert(streamId < MaxGsStreams);
        Value *output = call->getOperand(3);

        auto emitVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), emitVertsPtrs[streamId]);
        exportGsOutput(output, location, compIdx, streamId, threadIdInSubgroup, emitVerts);

        removeCalls.push_back(call);
      }
    } else if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg) {
      // Handle GS message
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);
        m_builder.SetInsertPoint(call);

        if (getShaderStage(call->getParent()->getParent()) != ShaderStageGeometry)
          continue; // Not belong to GS messages

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

  return gsEntryPoint;
}

// =====================================================================================================================
// Runs copy shader.
//
// @param module : LLVM module
// @param sysValueStart : Start of system value
void NggPrimShader::runCopyShader(Module *module, Argument *sysValueStart) {
  assert(m_hasGs); // GS must be present

  //
  // The processing is something like this:
  //
  //   vertexIndices = Relative vertex indices
  //   if (compactVertex)
  //     vertexIndices = Read uncompacted relative vertex indices from LDS
  //   Calculate vertex offset and run copy shader
  //
  Value *vertexIndex = m_nggInputs.threadIdInSubgroup;
  if (m_nggInputs.vertCompacted) {
    auto expVertBlock = m_builder.GetInsertBlock();

    auto uncompactOutVertIdBlock = createBlock(expVertBlock->getParent(), ".uncompactOutVertId");
    uncompactOutVertIdBlock->moveAfter(expVertBlock);

    auto endUncompactOutVertIdBlock = createBlock(expVertBlock->getParent(), ".endUncompactOutVertId");
    endUncompactOutVertIdBlock->moveAfter(uncompactOutVertIdBlock);

    m_builder.CreateCondBr(m_nggInputs.vertCompacted, uncompactOutVertIdBlock, endUncompactOutVertIdBlock);

    // Construct ".uncompactOutVertId" block
    Value *uncompactedVertexIndex = nullptr;
    {
      m_builder.SetInsertPoint(uncompactOutVertIdBlock);

      uncompactedVertexIndex =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionOutVertThreadIdMap);

      m_builder.CreateBr(endUncompactOutVertIdBlock);
    }

    // Construct ".endUncompactOutVertId" block
    {
      m_builder.SetInsertPoint(endUncompactOutVertIdBlock);

      vertexIndex = createPhi({{uncompactedVertexIndex, uncompactOutVertIdBlock}, {vertexIndex, expVertBlock}});
    }
  }

  auto copyShaderEntry = mutateCopyShader(module);

  // Run copy shader
  std::vector<Value *> args;

  if (m_gfxIp.major >= 11) {
    // Setup attribute ring base and vertex thread ID in subgroup as two additional arguments to export vertex
    // attributes through memory
    const auto attribCount = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.expCount;
    if (attribCount > 0) {
      args.push_back(m_nggInputs.attribRingBase);
      args.push_back(m_nggInputs.threadIdInSubgroup);
    }

    // Global table
    auto userData = sysValueStart + NumSpecialSgprInputs;
    assert(userData->getType()->isVectorTy());
    auto globalTable = m_builder.CreateExtractElement(userData, static_cast<uint64_t>(0)); // The first user data SGPRs
    args.push_back(globalTable);
  }

  // Relative vertex index in subgroup
  args.push_back(vertexIndex);

  CallInst *copyShaderCall = m_builder.CreateCall(copyShaderEntry, args);
  copyShaderCall->setCallingConv(CallingConv::AMDGPU_VS);
}

// =====================================================================================================================
// Mutates copy shader to handle the importing GS outputs from GS-VS ring.
//
// @param module : LLVM module
Function *NggPrimShader::mutateCopyShader(Module *module) {
  auto copyShaderEntryPoint = module->getFunction(lgcName::NggCopyShaderEntryPoint);
  assert(copyShaderEntryPoint);

  if (m_gfxIp.major >= 11)
    processVertexAttribExport(copyShaderEntryPoint);

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Relative vertex index is always the last argument
  auto vertexIndex = getFunctionArgument(copyShaderEntryPoint, copyShaderEntryPoint->arg_size() - 1);
  const unsigned rasterStream =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.rasterStream;

  std::vector<Instruction *> removeCalls;

  for (auto &func : module->functions()) {
    if (func.getName().startswith(lgcName::NggGsOutputImport)) {
      // Import GS outputs from GS-VS ring
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);

        if (call->getFunction() != copyShaderEntryPoint)
          continue; // Not belong to copy shader

        m_builder.SetInsertPoint(call);

        assert(call->arg_size() == 2);
        const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
        assert(streamId < MaxGsStreams);

        // Only lower the GS output import calls if they belong to the rasterization stream.
        if (streamId == rasterStream) {
          auto vertexOffset = calcVertexItemOffset(streamId, vertexIndex);
          auto output = importGsOutput(call->getType(), location, streamId, vertexOffset);
          call->replaceAllUsesWith(output);
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
// @param threadIdInSubgroup : Thread ID in subgroup
// @param emitVerts : Counter of GS emitted vertices for this stream
void NggPrimShader::exportGsOutput(Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                                   Value *threadIdInSubgroup, Value *emitVerts) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  if (!m_pipelineState->enableSwXfb() && resUsage->inOutUsage.gs.rasterStream != streamId) {
    // NOTE: If SW-emulated stream-out is not enabled, only import those outputs that belong to the rasterization
    // stream.
    return;
  }

  // NOTE: We only handle LDS vector/scalar writing, so change [n x Ty] to <n x Ty> for array.
  auto outputTy = output->getType();
  if (outputTy->isArrayTy()) {
    auto outputElemTy = outputTy->getArrayElementType();
    assert(outputElemTy->isSingleValueType());

    // [n x Ty] -> <n x Ty>
    const unsigned elemCount = outputTy->getArrayNumElements();
    Value *outputVec = UndefValue::get(FixedVectorType::get(outputElemTy, elemCount));
    for (unsigned i = 0; i < elemCount; ++i) {
      auto outputElem = m_builder.CreateExtractValue(output, i);
      outputVec = m_builder.CreateInsertElement(outputVec, outputElem, i);
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
      Type *castTy = m_builder.getInt16Ty();
      if (outputTy->isVectorTy())
        castTy = FixedVectorType::get(m_builder.getInt16Ty(), cast<FixedVectorType>(outputTy)->getNumElements());
      output = m_builder.CreateBitCast(output, castTy);
    }

    Type *extTy = m_builder.getInt32Ty();
    if (outputTy->isVectorTy())
      extTy = FixedVectorType::get(m_builder.getInt32Ty(), cast<FixedVectorType>(outputTy)->getNumElements());
    output = m_builder.CreateZExt(output, extTy);
  } else
    assert(bitWidth == 32 || bitWidth == 64);

  // vertexIndex = threadIdInSubgroup * outputVertices + emitVerts
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  auto vertexIndex = m_builder.CreateMul(threadIdInSubgroup, m_builder.getInt32(geometryMode.outputVertices));
  vertexIndex = m_builder.CreateAdd(vertexIndex, emitVerts);

  // ldsOffset = vertexOffset + (location * 4 + compIdx) * 4 (in bytes)
  auto vertexOffset = calcVertexItemOffset(streamId, vertexIndex);
  const unsigned attribOffset = (location * 4) + compIdx;
  auto ldsOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(attribOffset * 4));

  m_ldsManager->writeValueToLds(output, ldsOffset);
}

// =====================================================================================================================
// Imports outputs of geometry shader from GS-VS ring.
//
// @param outputTy : Type of the output
// @param location : Location of the output
// @param streamId : ID of output vertex stream
// @param vertexOffset : Start offset of vertex item in GS-VS ring (in bytes)
Value *NggPrimShader::importGsOutput(Type *outputTy, unsigned location, unsigned streamId, Value *vertexOffset) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  if (!m_pipelineState->enableSwXfb() && resUsage->inOutUsage.gs.rasterStream != streamId) {
    // NOTE: If SW-emulated stream-out is not enabled, only import those outputs that belong to the rasterization
    // stream.
    return UndefValue::get(outputTy);
  }

  // NOTE: We only handle LDS vector/scalar reading, so change [n x Ty] to <n x Ty> for array.
  auto origOutputTy = outputTy;
  if (outputTy->isArrayTy()) {
    auto outputElemTy = outputTy->getArrayElementType();
    assert(outputElemTy->isSingleValueType());

    // [n x Ty] -> <n x Ty>
    const unsigned elemCount = outputTy->getArrayNumElements();
    outputTy = FixedVectorType::get(outputElemTy, elemCount);
  }

  // ldsOffset = vertexOffset + location * 4 * 4 (in bytes)
  const unsigned attribOffset = location * 4;
  auto ldsOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(attribOffset * 4));

  auto output = m_ldsManager->readValueFromLds(outputTy, ldsOffset);

  if (origOutputTy != outputTy) {
    assert(origOutputTy->isArrayTy() && outputTy->isVectorTy() &&
           origOutputTy->getArrayNumElements() == cast<FixedVectorType>(outputTy)->getNumElements());

    // <n x Ty> -> [n x Ty]
    const unsigned elemCount = origOutputTy->getArrayNumElements();
    Value *outputArray = UndefValue::get(origOutputTy);
    for (unsigned i = 0; i < elemCount; ++i) {
      auto outputElem = m_builder.CreateExtractElement(output, i);
      outputArray = m_builder.CreateInsertValue(outputArray, outputElem, i);
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
// @param [in/out] emitVertsPtr : Pointer to the counter of GS emitted vertices for this stream
// @param [in/out] outVertsPtr : Pointer to the counter of GS output vertices of current primitive for this stream
void NggPrimShader::processGsEmit(Module *module, unsigned streamId, Value *threadIdInSubgroup, Value *emitVertsPtr,
                                  Value *outVertsPtr) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  if (!m_pipelineState->enableSwXfb() && resUsage->inOutUsage.gs.rasterStream != streamId) {
    // NOTE: If SW-emulated stream-out is not enabled, only handle GS_EMIT message that belongs to the rasterization
    // stream.
    return;
  }

  auto gsEmitHandler = module->getFunction(lgcName::NggGsEmit);
  if (!gsEmitHandler)
    gsEmitHandler = createGsEmitHandler(module);

  m_builder.CreateCall(gsEmitHandler, {threadIdInSubgroup, m_builder.getInt32(streamId), emitVertsPtr, outVertsPtr});
}

// =====================================================================================================================
// Processes the message GS_CUT.
//
// @param module : LLVM module
// @param streamId : ID of output vertex stream
// @param [in/out] outVertsPtr : Pointer to the counter of GS output vertices of current primitive for this stream
void NggPrimShader::processGsCut(Module *module, unsigned streamId, Value *outVertsPtr) {
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  if (!m_pipelineState->enableSwXfb() && resUsage->inOutUsage.gs.rasterStream != streamId) {
    // NOTE: If SW-emulated stream-out is not enabled, only handle GS_CUT message that belongs to the rasterization
    // stream.
    return;
  }

  auto gsCutHandler = module->getFunction(lgcName::NggGsCut);
  if (!gsCutHandler)
    gsCutHandler = createGsCutHandler(module);

  m_builder.CreateCall(gsCutHandler, outVertsPtr);
}

// =====================================================================================================================
// Creates the function that processes GS_EMIT.
//
// @param module : LLVM module
Function *NggPrimShader::createGsEmitHandler(Module *module) {
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
  auto funcTy = FunctionType::get(m_builder.getVoidTy(),
                                  {
                                      m_builder.getInt32Ty(),                              // %threadIdInSubgroup
                                      m_builder.getInt32Ty(),                              // %streamId
                                      PointerType::get(m_builder.getInt32Ty(), addrSpace), // %emitVertsPtr
                                      PointerType::get(m_builder.getInt32Ty(), addrSpace), // %outVertsPtr
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggGsEmit, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *threadIdInSubgroup = argIt++;
  threadIdInSubgroup->setName("threadIdInSubgroup");

  Value *streamId = argIt++;
  threadIdInSubgroup->setName("streamId");

  Value *emitVertsPtr = argIt++;
  emitVertsPtr->setName("emitVertsPtr");

  Value *outVertsPtr = argIt++;
  outVertsPtr->setName("outVertsPtr");

  auto entryBlock = createBlock(func, ".entry");
  auto emitPrimBlock = createBlock(func, ".emitPrim");
  auto endEmitPrimBlock = createBlock(func, ".endEmitPrim");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();

  // Construct ".entry" block
  Value *emitVerts = nullptr;
  Value *outVerts = nullptr;
  Value *primEmit = nullptr;
  {
    m_builder.SetInsertPoint(entryBlock);

    emitVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), emitVertsPtr);
    outVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), outVertsPtr);

    // emitVerts++
    emitVerts = m_builder.CreateAdd(emitVerts, m_builder.getInt32(1));

    // outVerts++
    outVerts = m_builder.CreateAdd(outVerts, m_builder.getInt32(1));

    // primEmit = (outVerts >= outVertsPerPrim)
    primEmit = m_builder.CreateICmpUGE(outVerts, m_builder.getInt32(outVertsPerPrim));
    m_builder.CreateCondBr(primEmit, emitPrimBlock, endEmitPrimBlock);
  }

  // Construct ".emitPrim" block
  {
    m_builder.SetInsertPoint(emitPrimBlock);

    // vertexIndex = threadIdInSubgroup * outputVertices + emitVerts - outVertsPerPrim
    auto vertexIndex = m_builder.CreateMul(threadIdInSubgroup, m_builder.getInt32(geometryMode.outputVertices));
    vertexIndex = m_builder.CreateAdd(vertexIndex, emitVerts);
    vertexIndex = m_builder.CreateSub(vertexIndex, m_builder.getInt32(outVertsPerPrim));

    Value *winding = m_builder.getInt32(0);
    if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip) {
      winding = m_builder.CreateSub(outVerts, m_builder.getInt32(outVertsPerPrim));
      winding = m_builder.CreateAnd(winding, 0x1);
    }

    // Write primitive data (just winding)
    const unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimData);
    // ldsOffset = regionStart + vertexIndex * sizeof(DWORD) + sizeof(DWORD) * NggMaxThreadsPerSubgroup * streamId
    auto ldsOffset = m_builder.CreateAdd(m_builder.getInt32(regionStart),
                                         m_builder.CreateMul(vertexIndex, m_builder.getInt32(SizeOfDword)));
    ldsOffset = m_builder.CreateAdd(
        ldsOffset, m_builder.CreateMul(m_builder.getInt32(SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup), streamId));
    m_ldsManager->writeValueToLds(winding, ldsOffset);

    m_builder.CreateBr(endEmitPrimBlock);
  }

  // Construct ".endEmitPrim" block
  {
    m_builder.SetInsertPoint(endEmitPrimBlock);

    m_builder.CreateStore(emitVerts, emitVertsPtr);
    m_builder.CreateStore(outVerts, outVertsPtr);
    m_builder.CreateRetVoid();
  }

  return func;
}

// =====================================================================================================================
// Creates the function that processes GS_CUT.
//
// @param module : LLVM module
Function *NggPrimShader::createGsCutHandler(Module *module) {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   outVerts = 0
  //
  const auto addrSpace = module->getDataLayout().getAllocaAddrSpace();
  auto funcTy =
      FunctionType::get(m_builder.getVoidTy(), PointerType::get(m_builder.getInt32Ty(), addrSpace), // %outVertsPtr
                        false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggGsCut, module);

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *outVertsPtr = argIt++;
  outVertsPtr->setName("outVertsPtr");

  auto entryBlock = createBlock(func, ".entry");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);
    m_builder.CreateStore(m_builder.getInt32(0), outVertsPtr); // Reset outVerts
    m_builder.CreateRetVoid();
  }

  return func;
}

// =====================================================================================================================
// Reads per-thread data from the specified NGG region in LDS.
//
// @param readDataTy : Data read from LDS
// @param threadId : Thread ID in subgroup to calculate LDS offset
// @param region : NGG LDS region
// @param offsetInRegion : Offset within this NGG LDS region (in bytes), the default is 0 (from the region beginning)
// @param useDs128 : Whether to use 128-bit LDS read, 16-byte alignment is guaranteed by caller
Value *NggPrimShader::readPerThreadDataFromLds(Type *readDataTy, Value *threadId, NggLdsRegionType region,
                                               unsigned offsetInRegion, bool useDs128) {
  assert(region != LdsRegionVertCullInfo); // Vertex cull info region is an aggregate-typed one, not applicable
  auto sizeInBytes = readDataTy->getPrimitiveSizeInBits() / 8;

  const auto regionStart = m_ldsManager->getLdsRegionStart(region);

  Value *ldsOffset = nullptr;
  if (sizeInBytes > 1)
    ldsOffset = m_builder.CreateMul(threadId, m_builder.getInt32(sizeInBytes));
  else
    ldsOffset = threadId;
  ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + offsetInRegion));

  return m_ldsManager->readValueFromLds(readDataTy, ldsOffset, useDs128);
}

// =====================================================================================================================
// Writes the per-thread data to the specified NGG region in LDS.
//
// @param writeData : Data written to LDS
// @param threadId : Thread ID in subgroup to calculate LDS offset
// @param region : NGG LDS region
// @param offsetInRegion : Offset within this NGG LDS region (in bytes), the default is 0 (from the region beginning)
// @param useDs128 : Whether to use 128-bit LDS write, 16-byte alignment is guaranteed by caller
void NggPrimShader::writePerThreadDataToLds(Value *writeData, Value *threadId, NggLdsRegionType region,
                                            unsigned offsetInRegion, bool useDs128) {
  assert(region != LdsRegionVertCullInfo); // Vertex cull info region is an aggregate-typed one, not applicable
  auto writeDataTy = writeData->getType();
  auto sizeInBytes = writeDataTy->getPrimitiveSizeInBits() / 8;

  const auto regionStart = m_ldsManager->getLdsRegionStart(region);

  Value *ldsOffset = nullptr;
  if (sizeInBytes > 1)
    ldsOffset = m_builder.CreateMul(threadId, m_builder.getInt32(sizeInBytes));
  else
    ldsOffset = threadId;
  ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + offsetInRegion));

  m_ldsManager->writeValueToLds(writeData, ldsOffset, useDs128);
}

// =====================================================================================================================
// Reads vertex cull info from LDS (the region of vertex cull info).
//
// @param readDataTy : Data read from LDS
// @param vertexItemOffset : Per-vertex item offset (in bytes) in subgroup of the entire vertex cull info
// @param dataOffset : Data offset (in bytes) within an item of vertex cull info
Value *NggPrimShader::readVertexCullInfoFromLds(Type *readDataTy, Value *vertexItemOffset, unsigned dataOffset) {
  // Only applied to culling mode of non-GS NGG
  assert(!m_hasGs && !m_nggControl->passthroughMode);
  assert(dataOffset != InvalidValue);

  const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + dataOffset));
  return m_ldsManager->readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes vertex cull info to LDS (the region of vertex cull info).
//
// @param writeData : Data written to LDS
// @param vertexItemOffset : Per-vertex item offset (in bytes) in subgroup of the entire vertex cull info
// @param dataOffset : Data offset (in bytes) within an item of vertex cull info
void NggPrimShader::writeVertexCullInfoToLds(Value *writeData, Value *vertexItemOffset, unsigned dataOffset) {
  // Only applied to culling mode of non-GS NGG
  assert(!m_hasGs && !m_nggControl->passthroughMode);
  assert(dataOffset != InvalidValue);

  const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + dataOffset));
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
  Value *paSuScModeCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paSuScModeCntl);

  // Get register PA_CL_VPORT_XSCALE
  auto paClVportXscale = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportXscale);

  // Get register PA_CL_VPORT_YSCALE
  auto paClVportYscale = fetchCullingControlRegister(module, m_cbLayoutTable.vportControls[0].paClVportYscale);

  // Do backface culling
  return m_builder.CreateCall(backfaceCuller,
                              {cullFlag, vertex0, vertex1, vertex2, m_builder.getInt32(m_nggControl->backfaceExponent),
                               paSuScModeCntl, paClVportXscale, paClVportYscale});
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
  Value *paClClipCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbVertDiscAdj);

  // Do frustum culling
  return m_builder.CreateCall(
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
  Value *paClVteCntl = m_builder.getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbVertDiscAdj);

  // Do box filter culling
  return m_builder.CreateCall(boxFilterCuller, {cullFlag, vertex0, vertex1, vertex2, paClVteCntl, paClClipCntl,
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
  Value *paClVteCntl = m_builder.getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = fetchCullingControlRegister(module, m_cbLayoutTable.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(module, m_cbLayoutTable.paClGbVertDiscAdj);

  // Do small primitive filter culling
  return m_builder.CreateCall(sphereCuller, {cullFlag, vertex0, vertex1, vertex2, paClVteCntl, paClClipCntl,
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
  Value *paClVteCntl = m_builder.getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

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
  conservativeRaster = m_builder.CreateICmpEQ(conservativeRaster, m_builder.getInt32(1));

  // Do small primitive filter culling
  return m_builder.CreateCall(smallPrimFilterCuller,
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
  return m_builder.CreateCall(cullDistanceCuller, {cullFlag, signMask0, signMask1, signMask2});
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

  return m_builder.CreateCall(
      fetchCullingRegister,
      {m_nggInputs.primShaderTableAddrLow, m_nggInputs.primShaderTableAddrHigh, m_builder.getInt32(regOffset)});
}

// =====================================================================================================================
// Creates the function that does backface culling.
//
// @param module : LLVM module
Function *NggPrimShader::createBackfaceCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %cullFlag
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %backfaceExponent
                                      m_builder.getInt32Ty(),                          // %paSuScModeCntl
                                      m_builder.getInt32Ty(),                          // %paClVportXscale
                                      m_builder.getInt32Ty()                           // %paClVportYscale
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingBackface, module);

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
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
  auto backfaceExitBlock = createBlock(func, ".backfaceExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".backfaceEntry" block
  {
    m_builder.SetInsertPoint(backfaceEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder.CreateCondBr(cullFlag, backfaceExitBlock, backfaceCullBlock);
  }

  // Construct ".backfaceCull" block
  Value *cullFlag1 = nullptr;
  Value *w0 = nullptr;
  Value *w1 = nullptr;
  Value *w2 = nullptr;
  Value *area = nullptr;
  {
    m_builder.SetInsertPoint(backfaceCullBlock);

    //
    // Backface culling algorithm is described as follow:
    //
    //   if ((area > 0 && face == CCW) || (area < 0 && face == CW))
    //     frontFace = true
    //
    //   backFace = !frontFace
    //
    //   if ((frontFace && cullFront) || (backFace && cullBack))
    //     cullFlag = true
    //

    //          | x0 y0 w0 |
    //          |          |
    //   area = | x1 y1 w1 | =  x0 * (y1 * w2 - y2 * w1) - x1 * (y0 * w2 - y2 * w0) + x2 * (y0 * w1 - y1 * w0)
    //          |          |
    //          | x2 y2 w2 |
    //
    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    w2 = m_builder.CreateExtractElement(vertex2, 3);

    auto y1W2 = m_builder.CreateFMul(y1, w2);
    auto y2W1 = m_builder.CreateFMul(y2, w1);
    auto det0 = m_builder.CreateFSub(y1W2, y2W1);
    det0 = m_builder.CreateFMul(x0, det0);

    auto y0W2 = m_builder.CreateFMul(y0, w2);
    auto y2W0 = m_builder.CreateFMul(y2, w0);
    auto det1 = m_builder.CreateFSub(y0W2, y2W0);
    det1 = m_builder.CreateFMul(x1, det1);

    auto y0W1 = m_builder.CreateFMul(y0, w1);
    auto y1W0 = m_builder.CreateFMul(y1, w0);
    auto det2 = m_builder.CreateFSub(y0W1, y1W0);
    det2 = m_builder.CreateFMul(x2, det2);

    area = m_builder.CreateFSub(det0, det1);
    area = m_builder.CreateFAdd(area, det2);

    auto areaLtZero = m_builder.CreateFCmpOLT(area, ConstantFP::get(m_builder.getFloatTy(), 0.0));
    auto areaGtZero = m_builder.CreateFCmpOGT(area, ConstantFP::get(m_builder.getFloatTy(), 0.0));

    // xScale ^ yScale
    auto frontFace = m_builder.CreateXor(paClVportXscale, paClVportYscale);

    // signbit(xScale ^ yScale)
    frontFace = createUBfe(frontFace, 31, 1);

    // face = (FACE, PA_SU_SC_MODE_CNTL[2], 0 = CCW, 1 = CW)
    auto face = createUBfe(paSuScModeCntl, 2, 1);

    // frontFace = face ^ signbit(xScale ^ yScale)
    frontFace = m_builder.CreateXor(face, frontFace);

    // frontFace = (frontFace == 0)
    frontFace = m_builder.CreateICmpEQ(frontFace, m_builder.getInt32(0));

    // frontFace = frontFace == 0 ? area < 0 : area > 0
    frontFace = m_builder.CreateSelect(frontFace, areaLtZero, areaGtZero);

    // backFace = !frontFace
    auto backFace = m_builder.CreateNot(frontFace);

    // cullFront = (CULL_FRONT, PA_SU_SC_MODE_CNTL[0], 0 = DONT CULL, 1 = CULL)
    auto cullFront = m_builder.CreateAnd(paSuScModeCntl, m_builder.getInt32(1));
    cullFront = m_builder.CreateTrunc(cullFront, m_builder.getInt1Ty());

    // cullBack = (CULL_BACK, PA_SU_SC_MODE_CNTL[1], 0 = DONT CULL, 1 = CULL)
    Value *cullBack = createUBfe(paSuScModeCntl, 1, 1);
    cullBack = m_builder.CreateTrunc(cullBack, m_builder.getInt1Ty());

    // cullFront = cullFront ? frontFace : false
    cullFront = m_builder.CreateSelect(cullFront, frontFace, m_builder.getFalse());

    // cullBack = cullBack ? backFace : false
    cullBack = m_builder.CreateSelect(cullBack, backFace, m_builder.getFalse());

    // cullFlag = cullFront || cullBack
    cullFlag1 = m_builder.CreateOr(cullFront, cullBack);

    auto nonZeroBackfaceExp = m_builder.CreateICmpNE(backfaceExponent, m_builder.getInt32(0));
    m_builder.CreateCondBr(nonZeroBackfaceExp, backfaceExponentBlock, backfaceExitBlock);
  }

  // Construct ".backfaceExponent" block
  Value *cullFlag2 = nullptr;
  {
    m_builder.SetInsertPoint(backfaceExponentBlock);

    //
    // Ignore area calculations that are less enough
    //   if (|area| < (10 ^ (-backfaceExponent)) / |w0 * w1 * w2| )
    //     cullFlag = false
    //

    // |w0 * w1 * w2|
    auto absW0W1W2 = m_builder.CreateFMul(w0, w1);
    absW0W1W2 = m_builder.CreateFMul(absW0W1W2, w2);
    absW0W1W2 = m_builder.CreateIntrinsic(Intrinsic::fabs, m_builder.getFloatTy(), absW0W1W2);

    // threshold = (10 ^ (-backfaceExponent)) / |w0 * w1 * w2|
    auto threshold = m_builder.CreateNeg(backfaceExponent);
    threshold = m_builder.CreateIntrinsic(Intrinsic::powi, {m_builder.getFloatTy(), threshold->getType()},
                                          {ConstantFP::get(m_builder.getFloatTy(), 10.0), threshold});

    auto rcpAbsW0W1W2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), absW0W1W2);
    threshold = m_builder.CreateFMul(threshold, rcpAbsW0W1W2);

    // |area|
    auto absArea = m_builder.CreateIntrinsic(Intrinsic::fabs, m_builder.getFloatTy(), area);

    // cullFlag = cullFlag && (abs(area) >= threshold)
    cullFlag2 = m_builder.CreateFCmpOGE(absArea, threshold);
    cullFlag2 = m_builder.CreateAnd(cullFlag1, cullFlag2);

    m_builder.CreateBr(backfaceExitBlock);
  }

  // Construct ".backfaceExit" block
  {
    m_builder.SetInsertPoint(backfaceExitBlock);

    auto cullFlagPhi =
        createPhi({{cullFlag, backfaceEntryBlock}, {cullFlag1, backfaceCullBlock}, {cullFlag2, backfaceExponentBlock}});

    // polyMode = (POLY_MODE, PA_SU_SC_MODE_CNTL[4:3], 0 = DISABLE, 1 = DUAL)
    auto polyMode = createUBfe(paSuScModeCntl, 3, 2);

    // polyMode == 1
    auto wireFrameMode = m_builder.CreateICmpEQ(polyMode, m_builder.getInt32(1));

    // Disable backface culler if POLY_MODE is set to 1 (wireframe)
    // cullFlag = (polyMode == 1) ? false : cullFlag
    cullFlag = m_builder.CreateSelect(wireFrameMode, m_builder.getFalse(), cullFlagPhi);

    m_builder.CreateRet(cullFlag);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
//
// @param module : LLVM module
Function *NggPrimShader::createFrustumCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %cullFlag
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClClipCntl
                                      m_builder.getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder.getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingFrustum, module);

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
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

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".frustumEntry" block
  {
    m_builder.SetInsertPoint(frustumEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder.CreateCondBr(cullFlag, frustumExitBlock, frustumCullBlock);
  }

  // Construct ".frustumCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder.SetInsertPoint(frustumCullBlock);

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
    Value *clipSpaceDef = createUBfe(paClClipCntl, 19, 1);
    clipSpaceDef = m_builder.CreateTrunc(clipSpaceDef, m_builder.getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    auto zNear = m_builder.CreateSelect(clipSpaceDef, ConstantFP::get(m_builder.getFloatTy(), -1.0),
                                        ConstantFP::get(m_builder.getFloatTy(), 0.0));

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder.CreateBitCast(paClGbHorzDiscAdj, m_builder.getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder.CreateBitCast(paClGbVertDiscAdj, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto z0 = m_builder.CreateExtractElement(vertex0, 2);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto z1 = m_builder.CreateExtractElement(vertex1, 2);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto z2 = m_builder.CreateExtractElement(vertex2, 2);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // -xDiscAdj
    auto negXDiscAdj = m_builder.CreateFNeg(xDiscAdj);

    // -yDiscAdj
    auto negYDiscAdj = m_builder.CreateFNeg(yDiscAdj);

    Value *clipMask[6] = {};

    //
    // Get clip mask for vertex0
    //

    // (x0 < -xDiscAdj * w0) ? 0x1 : 0
    clipMask[0] = m_builder.CreateFMul(negXDiscAdj, w0);
    clipMask[0] = m_builder.CreateFCmpOLT(x0, clipMask[0]);
    clipMask[0] = m_builder.CreateSelect(clipMask[0], m_builder.getInt32(0x1), m_builder.getInt32(0));

    // (x0 > xDiscAdj * w0) ? 0x2 : 0
    clipMask[1] = m_builder.CreateFMul(xDiscAdj, w0);
    clipMask[1] = m_builder.CreateFCmpOGT(x0, clipMask[1]);
    clipMask[1] = m_builder.CreateSelect(clipMask[1], m_builder.getInt32(0x2), m_builder.getInt32(0));

    // (y0 < -yDiscAdj * w0) ? 0x4 : 0
    clipMask[2] = m_builder.CreateFMul(negYDiscAdj, w0);
    clipMask[2] = m_builder.CreateFCmpOLT(y0, clipMask[2]);
    clipMask[2] = m_builder.CreateSelect(clipMask[2], m_builder.getInt32(0x4), m_builder.getInt32(0));

    // (y0 > yDiscAdj * w0) ? 0x8 : 0
    clipMask[3] = m_builder.CreateFMul(yDiscAdj, w0);
    clipMask[3] = m_builder.CreateFCmpOGT(y0, clipMask[3]);
    clipMask[3] = m_builder.CreateSelect(clipMask[3], m_builder.getInt32(0x8), m_builder.getInt32(0));

    // (z0 < zNear * w0) ? 0x10 : 0
    clipMask[4] = m_builder.CreateFMul(zNear, w0);
    clipMask[4] = m_builder.CreateFCmpOLT(z0, clipMask[4]);
    clipMask[4] = m_builder.CreateSelect(clipMask[4], m_builder.getInt32(0x10), m_builder.getInt32(0));

    // (z0 > w0) ? 0x20 : 0
    clipMask[5] = m_builder.CreateFCmpOGT(z0, w0);
    clipMask[5] = m_builder.CreateSelect(clipMask[5], m_builder.getInt32(0x20), m_builder.getInt32(0));

    // clipMask0
    auto clipMaskX0 = m_builder.CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY0 = m_builder.CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ0 = m_builder.CreateOr(clipMask[4], clipMask[5]);
    auto clipMask0 = m_builder.CreateOr(clipMaskX0, clipMaskY0);
    clipMask0 = m_builder.CreateOr(clipMask0, clipMaskZ0);

    //
    // Get clip mask for vertex1
    //

    // (x1 < -xDiscAdj * w1) ? 0x1 : 0
    clipMask[0] = m_builder.CreateFMul(negXDiscAdj, w1);
    clipMask[0] = m_builder.CreateFCmpOLT(x1, clipMask[0]);
    clipMask[0] = m_builder.CreateSelect(clipMask[0], m_builder.getInt32(0x1), m_builder.getInt32(0));

    // (x1 > xDiscAdj * w1) ? 0x2 : 0
    clipMask[1] = m_builder.CreateFMul(xDiscAdj, w1);
    clipMask[1] = m_builder.CreateFCmpOGT(x1, clipMask[1]);
    clipMask[1] = m_builder.CreateSelect(clipMask[1], m_builder.getInt32(0x2), m_builder.getInt32(0));

    // (y1 < -yDiscAdj * w1) ? 0x4 : 0
    clipMask[2] = m_builder.CreateFMul(negYDiscAdj, w1);
    clipMask[2] = m_builder.CreateFCmpOLT(y1, clipMask[2]);
    clipMask[2] = m_builder.CreateSelect(clipMask[2], m_builder.getInt32(0x4), m_builder.getInt32(0));

    // (y1 > yDiscAdj * w1) ? 0x8 : 0
    clipMask[3] = m_builder.CreateFMul(yDiscAdj, w1);
    clipMask[3] = m_builder.CreateFCmpOGT(y1, clipMask[3]);
    clipMask[3] = m_builder.CreateSelect(clipMask[3], m_builder.getInt32(0x8), m_builder.getInt32(0));

    // (z1 < zNear * w1) ? 0x10 : 0
    clipMask[4] = m_builder.CreateFMul(zNear, w1);
    clipMask[4] = m_builder.CreateFCmpOLT(z1, clipMask[4]);
    clipMask[4] = m_builder.CreateSelect(clipMask[4], m_builder.getInt32(0x10), m_builder.getInt32(0));

    // (z1 > w1) ? 0x20 : 0
    clipMask[5] = m_builder.CreateFCmpOGT(z1, w1);
    clipMask[5] = m_builder.CreateSelect(clipMask[5], m_builder.getInt32(0x20), m_builder.getInt32(0));

    // clipMask1
    auto clipMaskX1 = m_builder.CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY1 = m_builder.CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ1 = m_builder.CreateOr(clipMask[4], clipMask[5]);
    auto clipMask1 = m_builder.CreateOr(clipMaskX1, clipMaskY1);
    clipMask1 = m_builder.CreateOr(clipMask1, clipMaskZ1);

    //
    // Get clip mask for vertex2
    //

    // (x2 < -xDiscAdj * w2) ? 0x1 : 0
    clipMask[0] = m_builder.CreateFMul(negXDiscAdj, w2);
    clipMask[0] = m_builder.CreateFCmpOLT(x2, clipMask[0]);
    clipMask[0] = m_builder.CreateSelect(clipMask[0], m_builder.getInt32(0x1), m_builder.getInt32(0));

    // (x2 > xDiscAdj * w2) ? 0x2 : 0
    clipMask[1] = m_builder.CreateFMul(xDiscAdj, w2);
    clipMask[1] = m_builder.CreateFCmpOGT(x2, clipMask[1]);
    clipMask[1] = m_builder.CreateSelect(clipMask[1], m_builder.getInt32(0x2), m_builder.getInt32(0));

    // (y2 < -yDiscAdj * w2) ? 0x4 : 0
    clipMask[2] = m_builder.CreateFMul(negYDiscAdj, w2);
    clipMask[2] = m_builder.CreateFCmpOLT(y2, clipMask[2]);
    clipMask[2] = m_builder.CreateSelect(clipMask[2], m_builder.getInt32(0x4), m_builder.getInt32(0));

    // (y2 > yDiscAdj * w2) ? 0x8 : 0
    clipMask[3] = m_builder.CreateFMul(yDiscAdj, w2);
    clipMask[3] = m_builder.CreateFCmpOGT(y2, clipMask[3]);
    clipMask[3] = m_builder.CreateSelect(clipMask[3], m_builder.getInt32(0x8), m_builder.getInt32(0));

    // (z2 < zNear * w2) ? 0x10 : 0
    clipMask[4] = m_builder.CreateFMul(zNear, w2);
    clipMask[4] = m_builder.CreateFCmpOLT(z2, clipMask[4]);
    clipMask[4] = m_builder.CreateSelect(clipMask[4], m_builder.getInt32(0x10), m_builder.getInt32(0));

    // (z2 > zFar * w2) ? 0x20 : 0
    clipMask[5] = m_builder.CreateFCmpOGT(z2, w2);
    clipMask[5] = m_builder.CreateSelect(clipMask[5], m_builder.getInt32(0x20), m_builder.getInt32(0));

    // clipMask2
    auto clipMaskX2 = m_builder.CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY2 = m_builder.CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ2 = m_builder.CreateOr(clipMask[4], clipMask[5]);
    auto clipMask2 = m_builder.CreateOr(clipMaskX2, clipMaskY2);
    clipMask2 = m_builder.CreateOr(clipMask2, clipMaskZ2);

    // clip = clipMask0 & clipMask1 & clipMask2
    auto clip = m_builder.CreateAnd(clipMask0, clipMask1);
    clip = m_builder.CreateAnd(clip, clipMask2);

    // cullFlag = (clip != 0)
    newCullFlag = m_builder.CreateICmpNE(clip, m_builder.getInt32(0));

    m_builder.CreateBr(frustumExitBlock);
  }

  // Construct ".frustumExit" block
  {
    m_builder.SetInsertPoint(frustumExitBlock);

    auto cullFlagPhi = createPhi({{cullFlag, frustumEntryBlock}, {newCullFlag, frustumCullBlock}});

    m_builder.CreateRet(cullFlagPhi);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does box filter culling.
//
// @param module : LLVM module
Function *NggPrimShader::createBoxFilterCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %cullFlag
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClVteCntl
                                      m_builder.getInt32Ty(),                          // %paClClipCntl
                                      m_builder.getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder.getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingBoxFilter, module);

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
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

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".boxfilterEntry" block
  {
    m_builder.SetInsertPoint(boxFilterEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder.CreateCondBr(cullFlag, boxFilterExitBlock, boxFilterCullBlock);
  }

  // Construct ".boxfilterCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder.SetInsertPoint(boxFilterCullBlock);

    //
    // Box filter culling algorithm is described as follow:
    //
    //   if (min(x0/w0, x1/w1, x2/w2) > xDiscAdj || max(x0/w0, x1/w1, x2/w2) < -xDiscAdj ||
    //       min(y0/w0, y1/w1, y2/w2) > yDiscAdj || max(y0/w0, y1/w1, y2/w2) < -yDiscAdj ||
    //       min(z0/w0, z1/w1, z2/w2) > zFar     || min(z0/w0, z1/w1, z2/w2) < zNear)
    //     cullFlag = true
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = createUBfe(paClVteCntl, 8, 1);
    vtxXyFmt = m_builder.CreateTrunc(vtxXyFmt, m_builder.getInt1Ty());

    // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    Value *vtxZFmt = createUBfe(paClVteCntl, 9, 1);
    vtxZFmt = m_builder.CreateTrunc(vtxZFmt, m_builder.getInt1Ty());

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = createUBfe(paClClipCntl, 19, 1);
    clipSpaceDef = m_builder.CreateTrunc(clipSpaceDef, m_builder.getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    auto zNear = m_builder.CreateSelect(clipSpaceDef, ConstantFP::get(m_builder.getFloatTy(), -1.0),
                                        ConstantFP::get(m_builder.getFloatTy(), 0.0));
    auto zFar = ConstantFP::get(m_builder.getFloatTy(), 1.0);

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder.CreateBitCast(paClGbHorzDiscAdj, m_builder.getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder.CreateBitCast(paClGbVertDiscAdj, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto z0 = m_builder.CreateExtractElement(vertex0, 2);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto z1 = m_builder.CreateExtractElement(vertex1, 2);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto z2 = m_builder.CreateExtractElement(vertex2, 2);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    auto rcpW0ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    auto rcpW1ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    auto rcpW2ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // VTX_Z_FMT ? 1.0 : 1 / w0
    auto rcpW0ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_Z_FMT ? 1.0 : 1 / w1
    auto rcpW1ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_Z_FMT ? 1.0 : 1 / w2
    auto rcpW2ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder.CreateFMul(x0, rcpW0ForXy);
    // y0' = y0/w0
    y0 = m_builder.CreateFMul(y0, rcpW0ForXy);
    // z0' = z0/w0
    z0 = m_builder.CreateFMul(z0, rcpW0ForZ);
    // x1' = x1/w1
    x1 = m_builder.CreateFMul(x1, rcpW1ForXy);
    // y1' = y1/w1
    y1 = m_builder.CreateFMul(y1, rcpW1ForXy);
    // z1' = z1/w1
    z1 = m_builder.CreateFMul(z1, rcpW1ForZ);
    // x2' = x2/w2
    x2 = m_builder.CreateFMul(x2, rcpW2ForXy);
    // y2' = y2/w2
    y2 = m_builder.CreateFMul(y2, rcpW2ForXy);
    // z2' = z2/w2
    z2 = m_builder.CreateFMul(z2, rcpW2ForZ);

    // -xDiscAdj
    auto negXDiscAdj = m_builder.CreateFNeg(xDiscAdj);

    // -yDiscAdj
    auto negYDiscAdj = m_builder.CreateFNeg(yDiscAdj);

    // minX = min(x0', x1', x2')
    auto minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {x0, x1});
    minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minX, x2});

    // minX > xDiscAdj
    auto minXGtXDiscAdj = m_builder.CreateFCmpOGT(minX, xDiscAdj);

    // maxX = max(x0', x1', x2')
    auto maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {x0, x1});
    maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxX, x2});

    // maxX < -xDiscAdj
    auto maxXLtNegXDiscAdj = m_builder.CreateFCmpOLT(maxX, negXDiscAdj);

    // minY = min(y0', y1', y2')
    auto minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {y0, y1});
    minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minY, y2});

    // minY > yDiscAdj
    auto minYGtYDiscAdj = m_builder.CreateFCmpOGT(minY, yDiscAdj);

    // maxY = max(y0', y1', y2')
    auto maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {y0, y1});
    maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxY, y2});

    // maxY < -yDiscAdj
    auto maxYLtNegYDiscAdj = m_builder.CreateFCmpOLT(maxY, negYDiscAdj);

    // minZ = min(z0', z1', z2')
    auto minZ = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {z0, z1});
    minZ = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minZ, z2});

    // minZ > zFar (1.0)
    auto minZGtZFar = m_builder.CreateFCmpOGT(minZ, zFar);

    // maxZ = min(z0', z1', z2')
    auto maxZ = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {z0, z1});
    maxZ = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxZ, z2});

    // maxZ < zNear
    auto maxZLtZNear = m_builder.CreateFCmpOLT(maxZ, zNear);

    // Get cull flag
    auto cullX = m_builder.CreateOr(minXGtXDiscAdj, maxXLtNegXDiscAdj);
    auto cullY = m_builder.CreateOr(minYGtYDiscAdj, maxYLtNegYDiscAdj);
    auto cullZ = m_builder.CreateOr(minZGtZFar, maxZLtZNear);
    newCullFlag = m_builder.CreateOr(cullX, cullY);
    newCullFlag = m_builder.CreateOr(newCullFlag, cullZ);

    m_builder.CreateBr(boxFilterExitBlock);
  }

  // Construct ".boxfilterExit" block
  {
    m_builder.SetInsertPoint(boxFilterExitBlock);

    auto cullFlagPhi = createPhi({{cullFlag, boxFilterEntryBlock}, {newCullFlag, boxFilterCullBlock}});

    m_builder.CreateRet(cullFlagPhi);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does sphere culling.
//
// @param module : LLVM module
Function *NggPrimShader::createSphereCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %cullFlag
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClVteCntl
                                      m_builder.getInt32Ty(),                          // %paClClipCntl
                                      m_builder.getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder.getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingSphere, module);

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
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

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".sphereEntry" block
  {
    m_builder.SetInsertPoint(sphereEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder.CreateCondBr(cullFlag, sphereExitBlock, sphereCullBlock);
  }

  // Construct ".sphereCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder.SetInsertPoint(sphereCullBlock);

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
    Value *vtxXyFmt = createUBfe(paClVteCntl, 8, 1);
    vtxXyFmt = m_builder.CreateTrunc(vtxXyFmt, m_builder.getInt1Ty());

    // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    Value *vtxZFmt = createUBfe(paClVteCntl, 9, 1);
    vtxZFmt = m_builder.CreateTrunc(vtxZFmt, m_builder.getInt1Ty());

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = createUBfe(paClClipCntl, 19, 1);
    clipSpaceDef = m_builder.CreateTrunc(clipSpaceDef, m_builder.getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0
    auto zNear = m_builder.CreateSelect(clipSpaceDef, ConstantFP::get(m_builder.getFloatTy(), -1.0),
                                        ConstantFP::get(m_builder.getFloatTy(), 0.0));

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder.CreateBitCast(paClGbHorzDiscAdj, m_builder.getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder.CreateBitCast(paClGbVertDiscAdj, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto z0 = m_builder.CreateExtractElement(vertex0, 2);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto z1 = m_builder.CreateExtractElement(vertex1, 2);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto z2 = m_builder.CreateExtractElement(vertex2, 2);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    auto rcpW0ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    auto rcpW1ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    auto rcpW2ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // VTX_Z_FMT ? 1.0 : 1 / w0
    auto rcpW0ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_Z_FMT ? 1.0 : 1 / w1
    auto rcpW1ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_Z_FMT ? 1.0 : 1 / w2
    auto rcpW2ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder.CreateFMul(x0, rcpW0ForXy);
    // y0' = y0/w0
    y0 = m_builder.CreateFMul(y0, rcpW0ForXy);
    // z0' = z0/w0
    z0 = m_builder.CreateFMul(z0, rcpW0ForZ);
    // x1' = x1/w1
    x1 = m_builder.CreateFMul(x1, rcpW1ForXy);
    // y1' = y1/w1
    y1 = m_builder.CreateFMul(y1, rcpW1ForXy);
    // z1' = z1/w1
    z1 = m_builder.CreateFMul(z1, rcpW1ForZ);
    // x2' = x2/w2
    x2 = m_builder.CreateFMul(x2, rcpW2ForXy);
    // y2' = y2/w2
    y2 = m_builder.CreateFMul(y2, rcpW2ForXy);
    // z2' = z2/w2
    z2 = m_builder.CreateFMul(z2, rcpW2ForZ);

    //
    // === Step 1 ===: Discard space to -1..1 space.
    //

    // x" = x'/xDiscAdj
    // y" = y'/yDiscAdj
    // z" = (zNear + 2.0)z' + (-1.0 - zNear)
    auto rcpXDiscAdj = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), xDiscAdj);
    auto rcpYDiscAdj = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), yDiscAdj);
    auto rcpXyDiscAdj = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {rcpXDiscAdj, rcpYDiscAdj});

    Value *x0Y0 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x0, y0});
    Value *x1Y1 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x1, y1});
    Value *x2Y2 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x2, y2});

    x0Y0 = m_builder.CreateFMul(x0Y0, rcpXyDiscAdj);
    x1Y1 = m_builder.CreateFMul(x1Y1, rcpXyDiscAdj);
    x2Y2 = m_builder.CreateFMul(x2Y2, rcpXyDiscAdj);

    // zNear + 2.0
    auto zNearPlusTwo = m_builder.CreateFAdd(zNear, ConstantFP::get(m_builder.getFloatTy(), 2.0));
    zNearPlusTwo = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {zNearPlusTwo, zNearPlusTwo});

    // -1.0 - zNear
    auto negOneMinusZNear = m_builder.CreateFSub(ConstantFP::get(m_builder.getFloatTy(), -1.0), zNear);
    negOneMinusZNear = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {negOneMinusZNear, negOneMinusZNear});

    Value *z0Z0 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {z0, z0});
    Value *z2Z1 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {z2, z1});

    z0Z0 = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                     {zNearPlusTwo, z0Z0, negOneMinusZNear});
    z2Z1 = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                     {zNearPlusTwo, z2Z1, negOneMinusZNear});

    //
    // === Step 2 ===: 3D coordinates to barycentric coordinates.
    //

    // <x20, y20> = <x2", y2"> - <x0", y0">
    auto x20Y20 = m_builder.CreateFSub(x2Y2, x0Y0);

    // <x10, y10> = <x1", y1"> - <x0", y0">
    auto x10Y10 = m_builder.CreateFSub(x1Y1, x0Y0);

    // <z20, z10> = <z2", z1"> - <z0", z0">
    auto z20Z10 = m_builder.CreateFSub(z2Z1, z0Z0);

    //
    // === Step 3 ===: Solve linear system and find the point closest to the origin.
    //

    // a00 = x10 + z10
    auto x10 = m_builder.CreateExtractElement(x10Y10, static_cast<uint64_t>(0));
    auto z10 = m_builder.CreateExtractElement(z20Z10, 1);
    auto a00 = m_builder.CreateFAdd(x10, z10);

    // a01 = x20 + z20
    auto x20 = m_builder.CreateExtractElement(x20Y20, static_cast<uint64_t>(0));
    auto z20 = m_builder.CreateExtractElement(z20Z10, static_cast<uint64_t>(0));
    auto a01 = m_builder.CreateFAdd(x20, z20);

    // a10 = y10 + y10
    auto y10 = m_builder.CreateExtractElement(x10Y10, 1);
    auto a10 = m_builder.CreateFAdd(y10, y10);

    // a11 = y20 + z20
    auto y20 = m_builder.CreateExtractElement(x20Y20, 1);
    auto a11 = m_builder.CreateFAdd(y20, z20);

    // b0 = -x0" - x2"
    x0 = m_builder.CreateExtractElement(x0Y0, static_cast<uint64_t>(0));
    auto negX0 = m_builder.CreateFNeg(x0);
    x2 = m_builder.CreateExtractElement(x2Y2, static_cast<uint64_t>(0));
    auto b0 = m_builder.CreateFSub(negX0, x2);

    // b1 = -x1" - x2"
    x1 = m_builder.CreateExtractElement(x1Y1, static_cast<uint64_t>(0));
    auto negX1 = m_builder.CreateFNeg(x1);
    auto b1 = m_builder.CreateFSub(negX1, x2);

    //     [ a00 a01 ]      [ b0 ]       [ s ]
    // A = [         ], B = [    ], ST = [   ], A * ST = B (crame rules)
    //     [ a10 a11 ]      [ b1 ]       [ t ]

    //           | a00 a01 |
    // det(A) =  |         | = a00 * a11 - a01 * a10
    //           | a10 a11 |
    auto detA = m_builder.CreateFMul(a00, a11);
    auto negA01 = m_builder.CreateFNeg(a01);
    detA = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {negA01, a10, detA});

    //            | b0 a01 |
    // det(Ab0) = |        | = b0 * a11 - a01 * b1
    //            | b1 a11 |
    auto detAB0 = m_builder.CreateFMul(b0, a11);
    detAB0 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {negA01, b1, detAB0});

    //            | a00 b0 |
    // det(Ab1) = |        | = a00 * b1 - b0 * a10
    //            | a10 b1 |
    auto detAB1 = m_builder.CreateFMul(a00, b1);
    auto negB0 = m_builder.CreateFNeg(b0);
    detAB1 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {negB0, a10, detAB1});

    // s = det(Ab0) / det(A)
    auto rcpDetA = m_builder.CreateFDiv(ConstantFP::get(m_builder.getHalfTy(), 1.0), detA);
    auto s = m_builder.CreateFMul(detAB0, rcpDetA);

    // t = det(Ab1) / det(A)
    auto t = m_builder.CreateFMul(detAB1, rcpDetA);

    //
    // === Step 4 ===: Do clamping for the closest point.
    //

    // <s, t>
    auto st = m_builder.CreateInsertElement(UndefValue::get(FixedVectorType::get(m_builder.getHalfTy(), 2)), s,
                                            static_cast<uint64_t>(0));
    st = m_builder.CreateInsertElement(st, t, 1);

    // <s', t'> = <0.5 - 0.5(t - s), 0.5 + 0.5(t - s)>
    auto tMinusS = m_builder.CreateFSub(t, s);
    auto sT1 = m_builder.CreateInsertElement(UndefValue::get(FixedVectorType::get(m_builder.getHalfTy(), 2)), tMinusS,
                                             static_cast<uint64_t>(0));
    sT1 = m_builder.CreateInsertElement(sT1, tMinusS, 1);

    sT1 = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                    {ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), -0.5),
                                                          ConstantFP::get(m_builder.getHalfTy(), 0.5)}),
                                     sT1,
                                     ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), 0.5),
                                                          ConstantFP::get(m_builder.getHalfTy(), 0.5)})});

    // <s", t"> = clamp(<s, t>)
    auto sT2 = m_builder.CreateIntrinsic(Intrinsic::maxnum, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                         {st, ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), 0.0),
                                                                   ConstantFP::get(m_builder.getHalfTy(), 0.0)})});
    sT2 = m_builder.CreateIntrinsic(Intrinsic::minnum, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                    {sT2, ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), 1.0),
                                                               ConstantFP::get(m_builder.getHalfTy(), 1.0)})});

    // <s, t> = (s + t) > 1.0 ? <s', t'> : <s", t">
    auto sPlusT = m_builder.CreateFAdd(s, t);
    auto sPlusTGtOne = m_builder.CreateFCmpOGT(sPlusT, ConstantFP::get(m_builder.getHalfTy(), 1.0));
    st = m_builder.CreateSelect(sPlusTGtOne, sT1, sT2);

    //
    // === Step 5 ===: Barycentric coordinates to 3D coordinates.
    //

    // x = x0" + s * x10 + t * x20
    // y = y0" + s * y10 + t * y20
    // z = z0" + s * z10 + t * z20
    s = m_builder.CreateExtractElement(st, static_cast<uint64_t>(0));
    t = m_builder.CreateExtractElement(st, 1);
    auto ss = m_builder.CreateInsertElement(st, s, 1);
    auto tt = m_builder.CreateInsertElement(st, t, static_cast<uint64_t>(0));

    // s * <x10, y10> + <x0", y0">
    auto xy =
        m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2), {ss, x10Y10, x0Y0});

    // <x, y> = t * <x20, y20> + (s * <x10, y10> + <x0", y0">)
    xy = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2), {tt, x20Y20, xy});

    // s * z10 + z0"
    z0 = m_builder.CreateExtractElement(z0Z0, static_cast<uint64_t>(0));
    auto z = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {s, z10, z0});

    // z = t * z20 + (s * z10 + z0")
    z = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {t, z20, z});

    auto x = m_builder.CreateExtractElement(xy, static_cast<uint64_t>(0));
    auto y = m_builder.CreateExtractElement(xy, 1);

    //
    // === Step 6 ===: Compute the distance squared of the closest point.
    //

    // r^2 = x^2 + y^2 + z^2
    auto squareR = m_builder.CreateFMul(x, x);
    squareR = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {y, y, squareR});
    squareR = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {z, z, squareR});

    //
    // == = Step 7 == = : Determine the cull flag
    //

    // cullFlag = (r ^ 2 > 3.0)
    newCullFlag = m_builder.CreateFCmpOGT(squareR, ConstantFP::get(m_builder.getHalfTy(), 3.0));

    m_builder.CreateBr(sphereExitBlock);
  }

  // Construct ".sphereExit" block
  {
    m_builder.SetInsertPoint(sphereExitBlock);

    auto cullFlagPhi = createPhi({{cullFlag, sphereEntryBlock}, {newCullFlag, sphereCullBlock}});

    m_builder.CreateRet(cullFlagPhi);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does small primitive filter culling.
//
// @param module : LLVM module
Function *NggPrimShader::createSmallPrimFilterCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %cullFlag
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClVteCntl
                                      m_builder.getInt32Ty(),                          // %paClVportXscale
                                      m_builder.getInt32Ty(),                          // %paClVportXoffset
                                      m_builder.getInt32Ty(),                          // %paClVportYscale
                                      m_builder.getInt32Ty(),                          // %paClVportYoffset
                                      m_builder.getInt1Ty()                            // %conservativeRaster
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingSmallPrimFilter, module);

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
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

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".smallprimfilterEntry" block
  {
    m_builder.SetInsertPoint(smallPrimFilterEntryBlock);

    // If cull flag has already been TRUE or if conservative rasterization, early return
    m_builder.CreateCondBr(m_builder.CreateOr(cullFlag, conservativeRaster), smallPrimFilterExitBlock,
                           smallPrimFilterCullBlock);
  }

  // Construct ".smallprimfilterCull" block
  Value *newCullFlag = nullptr;
  {
    m_builder.SetInsertPoint(smallPrimFilterCullBlock);

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
    Value *vtxXyFmt = createUBfe(paClVteCntl, 8, 1);
    vtxXyFmt = m_builder.CreateTrunc(vtxXyFmt, m_builder.getInt1Ty());

    // xScale = (VPORT_XSCALE, PA_CL_VPORT_XSCALE[31:0])
    // NOTE: This register value has already been scaled by MSAA number of samples in driver.
    auto xScale = m_builder.CreateBitCast(paClVportXscale, m_builder.getFloatTy());

    // xOffset = (VPORT_XOFFSET, PA_CL_VPORT_XOFFSET[31:0])
    auto xOffset = m_builder.CreateBitCast(paClVportXoffset, m_builder.getFloatTy());

    // yScale = (VPORT_YSCALE, PA_CL_VPORT_YSCALE[31:0])
    // NOTE: This register value has already been scaled by MSAA number of samples in driver.
    auto yScale = m_builder.CreateBitCast(paClVportYscale, m_builder.getFloatTy());

    // yOffset = (VPORT_YOFFSET, PA_CL_VPORT_YOFFSET[31:0])
    auto yOffset = m_builder.CreateBitCast(paClVportYoffset, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    rcpW0 = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    rcpW1 = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    rcpW2 = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder.CreateFMul(x0, rcpW0);
    // y0' = y0/w0
    y0 = m_builder.CreateFMul(y0, rcpW0);
    // x1' = x1/w1
    x1 = m_builder.CreateFMul(x1, rcpW1);
    // y1' = y1/w1
    y1 = m_builder.CreateFMul(y1, rcpW1);
    // x2' = x2/w2
    x2 = m_builder.CreateFMul(x2, rcpW2);
    // y2' = y2/w2
    y2 = m_builder.CreateFMul(y2, rcpW2);

    // NOTE: We apply a "fast" frustum culling based on screen space. VTE will convert coordinates from clip space to
    // screen space, so we can clamp the coordinate to (viewport min, viewport max) very quickly and save all of the
    // left/right/top/bottom plane checking, which is provided by traditional frustum culling.
    Value *screenMinX = nullptr;
    Value *screenMaxX = nullptr;
    Value *screenMinY = nullptr;
    Value *screenMaxY = nullptr;
    if (!m_nggControl->enableFrustumCulling) {
      // screenMinX = -xScale + xOffset - 0.75
      screenMinX = m_builder.CreateFAdd(m_builder.CreateFNeg(xScale), xOffset);
      screenMinX = m_builder.CreateFAdd(screenMinX, ConstantFP::get(m_builder.getFloatTy(), -0.75));

      // screenMaxX = xScale + xOffset + 0.75
      screenMaxX = m_builder.CreateFAdd(xScale, xOffset);
      screenMaxX = m_builder.CreateFAdd(screenMaxX, ConstantFP::get(m_builder.getFloatTy(), 0.75));

      // screenMinY = -yScale + yOffset - 0.75
      screenMinY = m_builder.CreateFAdd(m_builder.CreateFNeg(yScale), yOffset);
      screenMinY = m_builder.CreateFAdd(screenMinY, ConstantFP::get(m_builder.getFloatTy(), -0.75));

      // screenMaxY = yScale + yOffset + 0.75
      screenMaxY = m_builder.CreateFAdd(yScale, yOffset);
      screenMaxY = m_builder.CreateFAdd(screenMaxY, ConstantFP::get(m_builder.getFloatTy(), 0.75));
    }

    // screenX0' = x0' * xScale + xOffset
    auto screenX0 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {x0, xScale, xOffset});

    // screenX1' = x1' * xScale + xOffset
    auto screenX1 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {x1, xScale, xOffset});

    // screenX2' = x2' * xScale + xOffset
    auto screenX2 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {x2, xScale, xOffset});

    // minX = clamp(min(screenX0', screenX1', screenX2'), screenMinX, screenMaxX) - 1/256.0
    Value *minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {screenX0, screenX1});
    minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minX, screenX2});
    if (!m_nggControl->enableFrustumCulling) {
      minX = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinX, minX, screenMaxX});
    }
    minX = m_builder.CreateFAdd(minX, ConstantFP::get(m_builder.getFloatTy(), -1 / 256.0));

    // minX = roundEven(minX)
    minX = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), minX);

    // maxX = clamp(max(screenX0', screenX1', screenX2'), screenMinX, screenMaxX) + 1/256.0
    Value *maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {screenX0, screenX1});
    maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxX, screenX2});
    if (!m_nggControl->enableFrustumCulling) {
      maxX = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinX, maxX, screenMaxX});
    }
    maxX = m_builder.CreateFAdd(maxX, ConstantFP::get(m_builder.getFloatTy(), 1 / 256.0));

    // maxX = roundEven(maxX)
    maxX = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), maxX);

    // screenY0' = y0' * yScale + yOffset
    auto screenY0 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {y0, yScale, yOffset});

    // screenY1' = y1' * yScale + yOffset
    auto screenY1 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {y1, yScale, yOffset});

    // screenY2' = y2' * yScale + yOffset
    auto screenY2 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {y2, yScale, yOffset});

    // minY = clamp(min(screenY0', screenY1', screenY2'), screenMinY, screenMaxY) - 1/256.0
    Value *minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {screenY0, screenY1});
    minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minY, screenY2});
    if (!m_nggControl->enableFrustumCulling) {
      minY = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinY, minY, screenMaxY});
    }
    minY = m_builder.CreateFAdd(minY, ConstantFP::get(m_builder.getFloatTy(), -1 / 256.0));

    // minY = roundEven(minY)
    minY = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), minY);

    // maxY = clamp(max(screenX0', screenY1', screenY2'), screenMinY, screenMaxY) + 1/256.0
    Value *maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {screenY0, screenY1});
    maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxY, screenY2});
    if (!m_nggControl->enableFrustumCulling) {
      maxY = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinY, maxY, screenMaxY});
    }
    maxY = m_builder.CreateFAdd(maxY, ConstantFP::get(m_builder.getFloatTy(), 1 / 256.0));

    // maxY = roundEven(maxY)
    maxY = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), maxY);

    // minX == maxX
    auto minXEqMaxX = m_builder.CreateFCmpOEQ(minX, maxX);

    // minY == maxY
    auto minYEqMaxY = m_builder.CreateFCmpOEQ(minY, maxY);

    // Get cull flag
    newCullFlag = m_builder.CreateOr(minXEqMaxX, minYEqMaxY);

    // Check if W allows culling
    auto w0AsInt = m_builder.CreateBitCast(w0, m_builder.getInt32Ty());
    auto w1AsInt = m_builder.CreateBitCast(w1, m_builder.getInt32Ty());
    auto w2AsInt = m_builder.CreateBitCast(w2, m_builder.getInt32Ty());

    // w0 < 0 && w1 < 0 && w2 < 0
    auto isAllWNeg = m_builder.CreateAnd(w0AsInt, w1AsInt);
    isAllWNeg = m_builder.CreateAnd(isAllWNeg, w2AsInt);
    isAllWNeg = m_builder.CreateICmpSLT(isAllWNeg, m_builder.getInt32(0));

    // w0 > 0 && w1 > 0 && w2 > 0
    auto isAllWPos = m_builder.CreateOr(w0AsInt, w1AsInt);
    isAllWPos = m_builder.CreateOr(isAllWPos, w2AsInt);
    isAllWPos = m_builder.CreateICmpSGT(isAllWPos, m_builder.getInt32(0));

    auto allowCull = m_builder.CreateOr(isAllWNeg, isAllWPos);
    newCullFlag = m_builder.CreateAnd(allowCull, newCullFlag);

    m_builder.CreateBr(smallPrimFilterExitBlock);
  }

  // Construct ".smallprimfilterExit" block
  {
    m_builder.SetInsertPoint(smallPrimFilterExitBlock);

    auto cullFlagPhi = createPhi({{cullFlag, smallPrimFilterEntryBlock}, {newCullFlag, smallPrimFilterCullBlock}});

    m_builder.CreateRet(cullFlagPhi);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
//
// @param module : LLVM module
Function *NggPrimShader::createCullDistanceCuller(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),  // %cullFlag
                                      m_builder.getInt32Ty(), // %signMask0
                                      m_builder.getInt32Ty(), // %signMask1
                                      m_builder.getInt32Ty()  // %signMask2
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingCullDistance, module);

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
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

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".culldistanceEntry" block
  {
    m_builder.SetInsertPoint(cullDistanceEntryBlock);
    // If cull flag has already been TRUE, early return
    m_builder.CreateCondBr(cullFlag, cullDistanceExitBlock, cullDistanceCullBlock);
  }

  // Construct ".culldistanceCull" block
  Value *cullFlag1 = nullptr;
  {
    m_builder.SetInsertPoint(cullDistanceCullBlock);

    //
    // Cull distance culling algorithm is described as follow:
    //
    //   vertexSignMask[7:0] = [sign(ClipDistance[0])..sign(ClipDistance[7])]
    //   primSignMask = vertexSignMask0 & vertexSignMask1 & vertexSignMask2
    //   cullFlag = (primSignMask != 0)
    //
    auto signMask = m_builder.CreateAnd(signMask0, signMask1);
    signMask = m_builder.CreateAnd(signMask, signMask2);

    cullFlag1 = m_builder.CreateICmpNE(signMask, m_builder.getInt32(0));

    m_builder.CreateBr(cullDistanceExitBlock);
  }

  // Construct ".culldistanceExit" block
  {
    m_builder.SetInsertPoint(cullDistanceExitBlock);

    auto cullFlagPhi = createPhi({{cullFlag, cullDistanceEntryBlock}, {cullFlag1, cullDistanceCullBlock}});

    m_builder.CreateRet(cullFlagPhi);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that fetches culling control registers.
//
// @param module : LLVM module
Function *NggPrimShader::createFetchCullingRegister(Module *module) {
  auto funcTy = FunctionType::get(m_builder.getInt32Ty(),
                                  {
                                      m_builder.getInt32Ty(), // %primShaderTableAddrLow
                                      m_builder.getInt32Ty(), // %primShaderTableAddrHigh
                                      m_builder.getInt32Ty()  // %regOffset
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingFetchReg, module);

  func->setCallingConv(CallingConv::C);
  func->setOnlyReadsMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primShaderTableAddrLow = argIt++;
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = argIt++;
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  Value *regOffset = argIt++;
  regOffset->setName("regOffset");

  BasicBlock *entryBlock = createBlock(func); // Create entry block

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct entry block
  {
    m_builder.SetInsertPoint(entryBlock);

    Value *primShaderTableAddr =
        m_builder.CreateInsertElement(UndefValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2)),
                                      primShaderTableAddrLow, static_cast<uint64_t>(0));

    primShaderTableAddr = m_builder.CreateInsertElement(primShaderTableAddr, primShaderTableAddrHigh, 1);

    primShaderTableAddr = m_builder.CreateBitCast(primShaderTableAddr, m_builder.getInt64Ty());

    auto primShaderTableEltTy = ArrayType::get(m_builder.getInt32Ty(), 256);
    auto primShaderTablePtrTy = PointerType::get(primShaderTableEltTy, ADDR_SPACE_CONST); // [256 x i32]
    auto primShaderTablePtr = m_builder.CreateIntToPtr(primShaderTableAddr, primShaderTablePtrTy);

    // regOffset = regOffset >> 2
    regOffset = m_builder.CreateLShr(regOffset, 2); // To dword offset

    auto loadPtr = m_builder.CreateGEP(primShaderTableEltTy, primShaderTablePtr, {m_builder.getInt32(0), regOffset});
    cast<Instruction>(loadPtr)->setMetadata(MetaNameUniform, MDNode::get(m_builder.getContext(), {}));

    auto regValue = m_builder.CreateAlignedLoad(m_builder.getInt32Ty(), loadPtr, Align(4));
    regValue->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));

    m_builder.CreateRet(regValue);
  }

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

  value = m_builder.CreateSelect(value, m_builder.getInt32(1), m_builder.getInt32(0));

  auto inlineAsmTy = FunctionType::get(m_builder.getInt32Ty(), m_builder.getInt32Ty(), false);
  auto inlineAsm = InlineAsm::get(inlineAsmTy, "; %1", "=v,0", true);
  value = m_builder.CreateCall(inlineAsm, value);

  static const unsigned PredicateNE = 33; // 33 = predicate NE
  Value *ballot = m_builder.CreateIntrinsic(Intrinsic::amdgcn_icmp,
                                            {
                                                m_builder.getIntNTy(waveSize), // Return type
                                                m_builder.getInt32Ty()         // Argument type
                                            },
                                            {value, m_builder.getInt32(0), m_builder.getInt32(PredicateNE)});

  if (waveSize == 32)
    ballot = m_builder.CreateZExt(ballot, m_builder.getInt64Ty());

  return ballot;
}

// =====================================================================================================================
// Processes vertex attribute export calls in the target function. We mutate the argument list of the target function
// by adding two additional arguments (one is attribute ring base and the other is vertex thread ID in subgroup).
// Also, we expand all export calls by replacing it with real instructions that do vertex attribute exporting through
// memory.
//
// @param [in/out] target : Targeted function that has vertex attribute calls to process
void NggPrimShader::processVertexAttribExport(Function *&targetFunc) {
  assert(m_gfxIp.major >= 11); // For GFX11+

  const unsigned attribCount =
      m_pipelineState
          ->getShaderResourceUsage(m_hasGs ? ShaderStageGeometry : (m_hasTes ? ShaderStageTessEval : ShaderStageVertex))
          ->inOutUsage.expCount;
  if (attribCount == 0)
    return; // No vertex attribute exports

  IRBuilder<>::InsertPointGuard guard(m_builder);

  //
  // Mutate the argument list by adding two additional arguments
  //
  auto newTargetFunc = addFunctionArgs(targetFunc, nullptr,
                                       {
                                           m_builder.getInt32Ty(), // Attribute ring base (SGPR)
                                           m_builder.getInt32Ty()  // Vertex thread ID in subgroup (VGPR)
                                       },
                                       {"attribRingBase", "vertexIndex"}, 0x1);

  // Original function is no longer needed
  assert(targetFunc->use_empty());
  targetFunc->eraseFromParent();

  targetFunc = newTargetFunc;

  //
  // Expand vertex attribute export calls by replacing them with real instructions
  //
  // Modify the field STRIDE of attribute ring buffer descriptor if we have more than one vertex attribute to export
  bool modifyAttribRingBufDesc = attribCount > 1;
  Value *attribRingBufDesc = nullptr;

  // Always the first two arguments, added by us
  auto attribRingBase = targetFunc->getArg(0);
  auto vertexIndex = targetFunc->getArg(1);

  SmallVector<CallInst *, 8> removeCalls;

  for (auto &func : targetFunc->getParent()->functions()) {
    if (func.getName().startswith(lgcName::NggAttribExport)) {
      for (auto user : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(user);
        assert(call);

        if (call->getParent()->getParent() != targetFunc)
          continue; // Export call doesn't belong to targeted function, skip

        // NOTE: We always set the insert point before the terminator of the basic block to which this call belongs.
        // This is because we might modify attribute ring buffer descriptor and this modified descriptor will be used
        // by subsequent ring buffer store instructions that do vertex attribute exporting.
        m_builder.SetInsertPoint(call->getParent()->getTerminator());

        if (!attribRingBufDesc)
          attribRingBufDesc = call->getArgOperand(0); // Initialize it if necessary
        const unsigned location = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
        auto attribValue = call->getArgOperand(2);

        // Modify the field STRIDE of attribute ring buffer descriptor
        if (modifyAttribRingBufDesc) {
          // STRIDE = WORD1[30:16], STRIDE is multiplied by attribute count
          auto descWord1 = m_builder.CreateExtractElement(attribRingBufDesc, 1);
          auto stride = createUBfe(descWord1, 16, 14);
          stride = m_builder.CreateMul(stride, m_builder.getInt32(attribCount));

          descWord1 = m_builder.CreateAnd(descWord1, ~0x3FFF0000);                    // Clear STRIDE
          descWord1 = m_builder.CreateOr(descWord1, m_builder.CreateShl(stride, 16)); // Set new STRIDE
          attribRingBufDesc = m_builder.CreateInsertElement(attribRingBufDesc, descWord1, 1);

          modifyAttribRingBufDesc = false; // Clear the flag once finished
        }

        // Export vertex attributes
        assert(attribValue->getType() == FixedVectorType::get(m_builder.getFloatTy(), 4)); // Must be <4 xfloat>

        // ringOffset = attribRingBase * 32 * 16 + 32 * location * 16
        //            = attribRingBase * 512 + location * 512
        static const unsigned AttribGranularity = 32 * SizeOfVec4; // 32 * 16 bytes
        auto ringOffset = m_builder.CreateMul(attribRingBase, m_builder.getInt32(AttribGranularity));
        ringOffset = m_builder.CreateAdd(ringOffset, m_builder.getInt32(AttribGranularity * location));

        CoherentFlag coherent = {};
        coherent.bits.glc = true;
        coherent.bits.slc = true;

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_store, attribValue->getType(),
                                  {attribValue, attribRingBufDesc, vertexIndex, m_builder.getInt32(0), ringOffset,
                                   m_builder.getInt32(coherent.u32All)});

        removeCalls.push_back(call);
      }

      break; // Vertex attribute export calls are handled, could exit the loop
    }
  }

  // NOTE: If the workaround of attributes-through-memory preceding vertex position data is required, we have to collect
  // all vertex position export calls and move them before the return instruction. This actually places them after the
  // writing operations of attributes-through-memory
  if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos) {
    SmallVector<CallInst *, 4> exportCalls;

    // Colllect export calls of vertex position data
    for (auto &func : targetFunc->getParent()->functions()) {
      if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp) {
        for (auto user : func.users()) {
          CallInst *const call = dyn_cast<CallInst>(user);
          assert(call);

          if (call->getParent()->getParent() != targetFunc)
            continue; // Export call doesn't belong to targeted function, skip

          exportCalls.push_back(call);
        }
      }
    }

    // Move the export calls before the return instructions
    ReturnInst *retInst = nullptr;
    for (unsigned i = 0; i < exportCalls.size(); ++i) {
      auto exportCall = exportCalls[i];

      if (retInst) {
        // All export calls are expected to be in the same basic block
        assert(retInst == exportCall->getParent()->getTerminator());
      } else {
        retInst = dyn_cast<ReturnInst>(exportCall->getParent()->getTerminator());
        assert(retInst);
      }

      exportCall->setOperand(
          6, m_builder.getInt1(i == exportCalls.size() - 1)); // Make export done flag for the last export call
      exportCall->moveBefore(retInst);
    }

    // Before the first export call, add s_wait_vscnt 0 to make sure the completion of all attributes being written
    // to the attribute ring buffer
    m_builder.SetInsertPoint(exportCalls[0]);
    m_builder.CreateFence(AtomicOrdering::Release, SyncScope::System);
  }

  // Remove calls
  for (auto call : removeCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Processes SW emulated transform feedback when API GS is not present.
//
// @param module : LLVM module.
// @param sysValueStart : Start of system value
void NggPrimShader::processSwXfb(Module *module, Argument *sysValueStart) {
  assert(m_pipelineState->enableSwXfb());
  assert(!m_hasGs); // GS is not present

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();

  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  unsigned firstActiveXfbBuffer = InvalidValue;
  unsigned lastActiveXfbBuffer = InvalidValue;

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    bufferActive[i] = xfbStrides[i] > 0;
    if (!bufferActive[i])
      continue; // Transform feedback buffer is inactive

    if (firstActiveXfbBuffer == InvalidValue)
      firstActiveXfbBuffer = i;
    lastActiveXfbBuffer = i;
  }

  //
  // The processing is something like this:
  //
  // NGG_XFB() {
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     Mutate/clone ES to fetch XFB outputs
  //     Write XFB outputs to LDS region
  //   }
  //
  //   if (threadIdInSubgroup == 0) {
  //     Acquire the control of GDS_STRMOUT_DWORDS_WRITTEN_X
  //     Calculate primsToWrite and dwordsToWrite
  //     Increment GDS_STRMOUT_DWORDS_WRITTEN_X and release the control
  //     Store XFB statistics info to LDS
  //     Increment GDS_STRMOUT_PRIMS_NEEDED_X and GDS_STRMOUT_PRIMS_WRITTEN_X
  //   }
  //   Barrier
  //
  //   if (threadIdInWave < MaxXfbBuffers + 1)
  //     Read XFB statistics info from LDS
  //
  //   Read primsToWrite and dwordsWritten from XFB statistics info
  //
  //   if (threadIdInSubgroup < primsToWrite)
  //     Export XFB outputs to buffer for each vertice of this primitive
  // }
  //
  BasicBlock *xfbEntryBlock = m_builder.GetInsertBlock();

  BasicBlock *fetchXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".fetchXfbOutput");
  fetchXfbOutputBlock->moveAfter(xfbEntryBlock);
  BasicBlock *endFetchXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".endFetchXfbOutput");
  endFetchXfbOutputBlock->moveAfter(fetchXfbOutputBlock);

  BasicBlock *prepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".prepareXfbExport");
  prepareXfbExportBlock->moveAfter(endFetchXfbOutputBlock);
  BasicBlock *endPrepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".endPrepareXfbExport");
  endPrepareXfbExportBlock->moveAfter(prepareXfbExportBlock);

  BasicBlock *readXfbStatInfoBlock = createBlock(xfbEntryBlock->getParent(), ".readXfbStatInfo");
  readXfbStatInfoBlock->moveAfter(endPrepareXfbExportBlock);
  BasicBlock *endReadXfbStatInfoBlock = createBlock(xfbEntryBlock->getParent(), ".endReadXfbStatInfo");
  endReadXfbStatInfoBlock->moveAfter(readXfbStatInfoBlock);

  BasicBlock *exportXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".exportXfbOutput");
  exportXfbOutputBlock->moveAfter(endReadXfbStatInfoBlock);
  BasicBlock *endExportXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".endExportXfbOutput");
  endExportXfbOutputBlock->moveAfter(exportXfbOutputBlock);

  // Insert branching in current block to process transform feedback output export
  {
    auto vertValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(vertValid, fetchXfbOutputBlock, endFetchXfbOutputBlock);
  }

  // Construct ".fetchXfbOutput" block
  SmallVector<XfbOutputExport, 32> xfbOutputExports;
  {
    m_builder.SetInsertPoint(fetchXfbOutputBlock);

    auto xfbOutputs = fetchXfbOutput(module, sysValueStart, xfbOutputExports);

    for (unsigned i = 0; i < xfbOutputExports.size(); ++i) {
      assert(xfbOutputs->getType()->isArrayTy()); // Must be arrayed
      auto outputValue = m_builder.CreateExtractValue(xfbOutputs, i);

      // Write transform feedback outputs to LDS region
      writeXfbOutputToLds(outputValue, m_nggInputs.threadIdInSubgroup, i);
    }

    m_builder.CreateBr(endFetchXfbOutputBlock);
  }

  // Construct ".endFetchXfbOutput" block
  {
    m_builder.SetInsertPoint(endFetchXfbOutputBlock);

    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, prepareXfbExportBlock, endPrepareXfbExportBlock);
  }

  // Construct ".prepareXfbExport" block
  {
    m_builder.SetInsertPoint(prepareXfbExportBlock);

    const unsigned vertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
    Value *numPrimsToWrite = m_nggInputs.primCountInSubgroup;

    Value *dwordsWritten[MaxTransformFeedbackBuffers] = {};
    Value *dwordsPerPrim[MaxTransformFeedbackBuffers] = {};

    // Calculate numPrimsToWrite
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      if (i == firstActiveXfbBuffer) {
        // ds_ordered_count
        dwordsWritten[i] = m_builder.CreateIntrinsic(
            Intrinsic::amdgcn_ds_ordered_add, {},
            {
                m_builder.CreateIntToPtr(m_nggInputs.orderedWaveId,
                                         PointerType::get(m_builder.getInt32Ty(), ADDR_SPACE_REGION)), // m0
                m_builder.getInt32(0),                                                                 // value to add
                m_builder.getInt32(0),                                                                 // ordering
                m_builder.getInt32(0),                                                                 // scope
                m_builder.getFalse(),                                                                  // isVolatile
                m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) |
                                   (1 << 24)), // ordered count index, [27:24] is dword count
                m_builder.getFalse(),          // wave release
                m_builder.getFalse(),          // wave done
            });
      } else {
        // ds_add_gs_reg
        dwordsWritten[i] =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                      {m_builder.getInt32(0),                                         // value to add
                                       m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
      }

      // NUM_RECORDS = SQ_BUF_RSRC_WORD2
      Value *numRecords = m_builder.CreateExtractElement(m_streamOutBufDescs[i], 2);
      // bufferSizeInDwords = numRecords >> 2 (NOTE: NUM_RECORDS is set to the byte size of stream-out buffer)
      Value *bufferSizeInDwords = m_builder.CreateLShr(numRecords, 2);
      // dwordsRemaining = max(0, bufferSizeInDwords - (bufferOffset + dwordsWritten))
      Value *dwordsRemaining =
          m_builder.CreateSub(bufferSizeInDwords, m_builder.CreateAdd(m_streamOutBufOffsets[i], dwordsWritten[i]));
      dwordsRemaining = m_builder.CreateIntrinsic(Intrinsic::smax, dwordsRemaining->getType(),
                                                  {dwordsRemaining, m_builder.getInt32(0)});
      // numPrimsToWrite = min(dwordsRemaining / dwordsPerPrim, numPrimsToWrite)
      dwordsPerPrim[i] = m_builder.getInt32(vertsPerPrim * xfbStrides[i] / SizeOfDword);
      Value *primsCanWrite = m_builder.CreateUDiv(dwordsRemaining, dwordsPerPrim[i]);
      numPrimsToWrite =
          m_builder.CreateIntrinsic(Intrinsic::umin, numPrimsToWrite->getType(), {numPrimsToWrite, primsCanWrite});
    }

    // Increment dwordsWritten
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      Value *dwordsToWrite = m_builder.CreateMul(numPrimsToWrite, dwordsPerPrim[i]);

      if (i == lastActiveXfbBuffer) {
        // ds_ordered_count, wave done
        dwordsWritten[i] = m_builder.CreateIntrinsic(
            Intrinsic::amdgcn_ds_ordered_add, {},
            {
                m_builder.CreateIntToPtr(m_nggInputs.orderedWaveId,
                                         PointerType::get(m_builder.getInt32Ty(), ADDR_SPACE_REGION)), // m0
                dwordsToWrite,                                                                         // value to add
                m_builder.getInt32(0),                                                                 // ordering
                m_builder.getInt32(0),                                                                 // scope
                m_builder.getFalse(),                                                                  // isVolatile
                m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) |
                                   (1 << 24)), // ordered count index, [27:24] is dword count
                m_builder.getTrue(),           // wave release
                m_builder.getTrue(),           // wave done
            });
      } else {
        // ds_add_gs_reg
        dwordsWritten[i] =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, dwordsToWrite->getType(),
                                      {dwordsToWrite,                                                 // value to add
                                       m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
      }
    }

    // Store transform feedback statistics info to LDS and GDS
    const unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionXfbStatInfo);
    m_ldsManager->writeValueToLds(numPrimsToWrite,
                                  m_builder.getInt32(regionStart + MaxTransformFeedbackBuffers * SizeOfDword));
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      m_ldsManager->writeValueToLds(dwordsWritten[i], m_builder.getInt32(regionStart + i * SizeOfDword));
    }

    m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_nggInputs.primCountInSubgroup->getType(),
                              {m_nggInputs.primCountInSubgroup,                       // value to add
                               m_builder.getInt32(GDS_STRMOUT_PRIMS_NEEDED_0 << 2)}); // count index

    m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, numPrimsToWrite->getType(),
                              {numPrimsToWrite,                                        // value to add
                               m_builder.getInt32(GDS_STRMOUT_PRIMS_WRITTEN_0 << 2)}); // count index

    m_builder.CreateBr(endPrepareXfbExportBlock);
  }

  // Construct ".endPrepareXfbExport" block
  {
    m_builder.SetInsertPoint(endPrepareXfbExportBlock);

    // We are going to read transform feedback statistics info and outputs from LDS and export them to transform
    // feedback buffers. Make sure the output values have been all written before this.
    createFenceAndBarrier();

    auto threadValid =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_builder.getInt32(1 + MaxTransformFeedbackBuffers));
    m_builder.CreateCondBr(threadValid, readXfbStatInfoBlock, endReadXfbStatInfoBlock);
  }

  // Construct ".readXfbStatInfo" block
  Value *xfbStatInfo = nullptr;
  {
    m_builder.SetInsertPoint(readXfbStatInfoBlock);

    xfbStatInfo = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, LdsRegionXfbStatInfo);
    m_builder.CreateBr(endReadXfbStatInfoBlock);
  }

  // Construct ".endReadXfbStatInfo" block
  Value *streamOutOffsets[MaxTransformFeedbackBuffers] = {}; // Stream-out offset to write transform feedback outputs
  {
    m_builder.SetInsertPoint(endReadXfbStatInfoBlock);

    xfbStatInfo = createPhi(
        {{xfbStatInfo, readXfbStatInfoBlock}, {PoisonValue::get(xfbStatInfo->getType()), endPrepareXfbExportBlock}});

    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (bufferActive[i]) {
        streamOutOffsets[i] =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, {xfbStatInfo, m_builder.getInt32(i)});
        streamOutOffsets[i] = m_builder.CreateAdd(m_streamOutBufOffsets[i], streamOutOffsets[i]);
        streamOutOffsets[i] = m_builder.CreateShl(streamOutOffsets[i], 2);
      }
    }
    auto numPrimsToWrite = m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {},
                                                     {xfbStatInfo, m_builder.getInt32(MaxTransformFeedbackBuffers)});

    auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite);
    m_builder.CreateCondBr(primValid, exportXfbOutputBlock, endExportXfbOutputBlock);
  }

  // Construct ".exportXfbOutput" block
  {
    m_builder.SetInsertPoint(exportXfbOutputBlock);

    Value *vertexIndices[3] = {};

    const unsigned vertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
    if (m_nggControl->passthroughMode) {
      // [8:0]   = vertexIndex0
      vertexIndices[0] = createUBfe(m_nggInputs.primData, 0, 9);
      // [18:10] = vertexIndex1
      if (vertsPerPrim > 1)
        vertexIndices[1] = createUBfe(m_nggInputs.primData, 10, 9);
      // [28:20] = vertexIndex2
      if (vertsPerPrim > 2)
        vertexIndices[2] = createUBfe(m_nggInputs.primData, 20, 9);
    } else {
      // Must be triangle
      assert(vertsPerPrim == 3);
      vertexIndices[0] = m_nggInputs.esGsOffset0;
      vertexIndices[1] = m_nggInputs.esGsOffset1;
      vertexIndices[2] = m_nggInputs.esGsOffset2;
    }

    for (unsigned i = 0; i < vertsPerPrim; ++i) {
      for (unsigned j = 0; j < xfbOutputExports.size(); ++j) {
        const auto &xfbOutputExport = xfbOutputExports[j];
        auto outputValue = readXfbOutputFromLds(
            xfbOutputExport.numElements > 1 ? FixedVectorType::get(m_builder.getFloatTy(), xfbOutputExport.numElements)
                                            : m_builder.getFloatTy(),
            vertexIndices[i], j);

        if (xfbOutputExport.is16bit) {
          // NOTE: For 16-bit transform feedbakc outputs, they are stored as 32-bit without tightly packed in LDS.
          outputValue = m_builder.CreateBitCast(
              outputValue, FixedVectorType::get(m_builder.getInt32Ty(), xfbOutputExport.numElements));
          outputValue = m_builder.CreateTrunc(
              outputValue, FixedVectorType::get(m_builder.getInt16Ty(), xfbOutputExport.numElements));
          outputValue = m_builder.CreateBitCast(
              outputValue, FixedVectorType::get(m_builder.getHalfTy(), xfbOutputExport.numElements));
        }

        unsigned format = 0;
        switch (xfbOutputExport.numElements) {
        case 1:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_FLOAT : BUF_FORMAT_32_FLOAT;
          break;
        case 2:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_FLOAT_GFX11;
          break;
        case 3:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_32_FLOAT_GFX11;
          break;
        case 4:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_16_16_FLOAT_GFX11 : BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
          break;
        default:
          llvm_unreachable("Unexpected element number!");
          break;
        }

        CoherentFlag coherent = {};
        coherent.bits.glc = true;
        coherent.bits.slc = true;

        // vertexOffset = (threadIdInSubgroup * vertsPerPrim + vertexIndex) * xfbStride
        Value *vertexOffset =
            m_builder.CreateAdd(m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(vertsPerPrim)),
                                m_builder.getInt32(i));
        vertexOffset = m_builder.CreateMul(vertexOffset, m_builder.getInt32(xfbStrides[xfbOutputExport.xfbBuffer]));
        // xfbOutputOffset = vertexOffset + xfbOffset
        Value *xfbOutputOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(xfbOutputExport.xfbOffset));

        if (xfbOutputExport.is16bit && xfbOutputExport.numElements == 3) {
          // NOTE: For 16vec3, HW doesn't have a corresponding buffer store instruction. We have to split it to 16vec2
          // and 16scalar.
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                    {m_builder.CreateShuffleVector(outputValue, ArrayRef<int>{0, 1}), // vdata
                                     m_streamOutBufDescs[xfbOutputExport.xfbBuffer],                  // rsrc
                                     xfbOutputOffset,                                                 // offset
                                     streamOutOffsets[xfbOutputExport.xfbBuffer],                     // soffset
                                     m_builder.getInt32(BUF_FORMAT_16_16_FLOAT),                      // format
                                     m_builder.getInt32(coherent.u32All)});                           // auxiliary data

          m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, m_builder.getHalfTy(),
                                    {m_builder.CreateExtractElement(outputValue, 2), // vdata
                                     m_streamOutBufDescs[xfbOutputExport.xfbBuffer], // rsrc
                                     m_builder.CreateAdd(xfbOutputOffset,
                                                         m_builder.getInt32(2 * sizeof(uint16_t))), // offset
                                     streamOutOffsets[xfbOutputExport.xfbBuffer],                   // soffset
                                     m_builder.getInt32(BUF_FORMAT_16_FLOAT),                       // format
                                     m_builder.getInt32(coherent.u32All)});                         // auxiliary data
        } else {
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outputValue->getType(),
                                    {outputValue,                                    // vdata
                                     m_streamOutBufDescs[xfbOutputExport.xfbBuffer], // rsrc
                                     xfbOutputOffset,                                // offset
                                     streamOutOffsets[xfbOutputExport.xfbBuffer],    // soffset
                                     m_builder.getInt32(format),                     // format
                                     m_builder.getInt32(coherent.u32All)});          // auxiliary data
        }
      }
    }

    m_builder.CreateBr(endExportXfbOutputBlock);
  }

  // Construct ".endExportXfbOutput" block
  { m_builder.SetInsertPoint(endExportXfbOutputBlock); }
}

// =====================================================================================================================
// Process SW emulated transform feedback when API GS is present.
//
// @param module : LLVM module.
// @param sysValueStart : Start of system value
void NggPrimShader::processSwXfbWithGs(Module *module, Argument *sysValueStart) {
  assert(m_pipelineState->enableSwXfb());
  assert(m_hasGs); // GS is present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);
  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();

  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  unsigned firstActiveXfbBuffer = InvalidValue;
  unsigned lastActiveXfbBuffer = InvalidValue;

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    bufferActive[i] = xfbStrides[i] > 0;
    if (!bufferActive[i])
      continue; // Transform feedback buffer is inactive

    if (firstActiveXfbBuffer == InvalidValue)
      firstActiveXfbBuffer = i;
    lastActiveXfbBuffer = i;
  }

  bool streamActive[MaxGsStreams] = {};
  unsigned firstActiveStream = InvalidValue;
  unsigned lastActiveStream = InvalidValue;

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    streamActive[i] = streamXfbBuffers[i] != 0;
    if (!streamActive[i])
      continue; // Stream is inactive

    if (firstActiveStream == InvalidValue)
      firstActiveStream = i;
    lastActiveStream = i;
  }

  unsigned xfbBufferToStream[MaxTransformFeedbackBuffers] = {};

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    for (unsigned j = 0; j < MaxGsStreams; ++j) {
      if ((streamXfbBuffers[j] & (1 << i)) != 0) {
        // NOTE: According to GLSL spec, all outputs assigned to a given transform feedback buffer are required to
        // come from a single vertex stream.
        xfbBufferToStream[i] = j;
        break;
      }
    }
  }

  //
  // The processing is something like this:
  //
  // NGG_GS_XFB() {
  //   if (threadIdInSubgroup < waveCount + 1)
  //     Initialize per-wave and per-subgroup count of output primitives
  //   Barrier
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Check the draw flag of output primitives and compute draw mask
  //
  //   if (threadIdInWave < waveCount - waveId)
  //     Accumulate per-wave and per-subgroup count of output primitives
  //   Barrier
  //
  //   for each vertex stream) {
  //     if (primitive drawn)
  //       Compact primitive thread ID (map: compacted -> uncompacted)
  //   }
  //
  //   Mutate copy shader to fetch XFB outputs
  //
  //   if (threadIdInSubgroup == 0) {
  //     Acquire the control of GDS_STRMOUT_DWORDS_WRITTEN_X
  //     Calculate primsToWrite and dwordsToWrite
  //     Increment GDS_STRMOUT_DWORDS_WRITTEN_X and release the control
  //     Store GS XFB statistics info to LDS
  //     Increment GDS_STRMOUT_PRIMS_NEEDED_X and GDS_STRMOUT_PRIMS_WRITTEN_X
  //   }
  //   Barrier
  //
  //   Read XFB statistics info from LDS
  //   Read primsToWrite and dwordsWritten from XFB statistics info
  //
  //   for each vertex stream {
  //     if (threadIdInSubgroup < primsToWrite)
  //       Export XFB outputs to buffer for each vertice of this primitive
  //   }
  // }
  //
  BasicBlock *xfbEntryBlock = m_builder.GetInsertBlock();

  BasicBlock *initOutPrimCountBlock = createBlock(xfbEntryBlock->getParent(), ".initOutPrimCount");
  initOutPrimCountBlock->moveAfter(xfbEntryBlock);
  BasicBlock *endInitOutPrimCountBlock = createBlock(xfbEntryBlock->getParent(), ".endInitOutPrimCount");
  endInitOutPrimCountBlock->moveAfter(initOutPrimCountBlock);

  BasicBlock *checkOutPrimDrawFlagBlock = createBlock(xfbEntryBlock->getParent(), ".checkOutPrimDrawFlag");
  checkOutPrimDrawFlagBlock->moveAfter(endInitOutPrimCountBlock);
  BasicBlock *endCheckOutPrimDrawFlagBlock = createBlock(xfbEntryBlock->getParent(), ".endCheckOutPrimDrawFlag");
  endCheckOutPrimDrawFlagBlock->moveAfter(checkOutPrimDrawFlagBlock);

  BasicBlock *accumOutPrimCountBlock = createBlock(xfbEntryBlock->getParent(), ".accumOutPrimCount");
  accumOutPrimCountBlock->moveAfter(endCheckOutPrimDrawFlagBlock);
  BasicBlock *endAccumOutPrimCountBlock = createBlock(xfbEntryBlock->getParent(), ".endAccumOutPrimCount");
  endAccumOutPrimCountBlock->moveAfter(accumOutPrimCountBlock);

  BasicBlock *compactOutPrimIdBlock[MaxGsStreams] = {};
  BasicBlock *endCompactOutPrimIdBlock[MaxGsStreams] = {};
  BasicBlock *insertPos = endAccumOutPrimCountBlock;
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (streamActive[i]) {
      compactOutPrimIdBlock[i] =
          createBlock(xfbEntryBlock->getParent(), ".compactOutPrimIdInStream" + std::to_string(i));
      compactOutPrimIdBlock[i]->moveAfter(insertPos);
      insertPos = compactOutPrimIdBlock[i];

      endCompactOutPrimIdBlock[i] =
          createBlock(xfbEntryBlock->getParent(), ".endCompactOutPrimIdInStream" + std::to_string(i));
      endCompactOutPrimIdBlock[i]->moveAfter(insertPos);
      insertPos = endCompactOutPrimIdBlock[i];
    }
  }

  BasicBlock *prepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".prepareXfbExport");
  prepareXfbExportBlock->moveAfter(insertPos);
  BasicBlock *endPrepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".endPrepareXfbExport");
  endPrepareXfbExportBlock->moveAfter(prepareXfbExportBlock);

  BasicBlock *exportXfbOutputBlock[MaxGsStreams] = {};
  BasicBlock *endExportXfbOutputBlock[MaxGsStreams] = {};
  insertPos = endPrepareXfbExportBlock;
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (streamActive[i]) {
      exportXfbOutputBlock[i] = createBlock(xfbEntryBlock->getParent(), ".exportXfbOutputInStream" + std::to_string(i));
      exportXfbOutputBlock[i]->moveAfter(insertPos);
      insertPos = exportXfbOutputBlock[i];

      endExportXfbOutputBlock[i] =
          createBlock(xfbEntryBlock->getParent(), ".endExportXfbOutputInStream" + std::to_string(i));
      endExportXfbOutputBlock[i]->moveAfter(insertPos);
      insertPos = endExportXfbOutputBlock[i];
    }
  }

  // Insert branching in current block to process transform feedback output export
  {
    auto waveValid =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(waveCountInSubgroup + 1));
    m_builder.CreateCondBr(waveValid, initOutPrimCountBlock, endInitOutPrimCountBlock);
  }

  // Construct ".initOutPrimCount" block
  {
    m_builder.SetInsertPoint(initOutPrimCountBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimCountInWaves,
                                (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * i);
      }
    }

    m_builder.CreateBr(endInitOutPrimCountBlock);
  }

  // Construct ".endInitOutPrimCount" block
  {
    m_builder.SetInsertPoint(endInitOutPrimCountBlock);

    createFenceAndBarrier();

    auto outPrimValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(outPrimValid, checkOutPrimDrawFlagBlock, endCheckOutPrimDrawFlagBlock);
  }

  // Construct ".checkOutPrimDrawFlag" block
  Value *drawFlag[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(checkOutPrimDrawFlagBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        // drawFlag = primData[N] != NullPrim
        auto primData =
            readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimData,
                                     SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * i);
        drawFlag[i] = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));
      }
    }

    m_builder.CreateBr(endCheckOutPrimDrawFlagBlock);
  }

  // Construct ".endCheckOutPrimDrawFlag" block
  Value *drawMask[MaxGsStreams] = {};
  Value *outPrimCountInWave[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endCheckOutPrimDrawFlagBlock);

    // Update draw flags
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        drawFlag[i] =
            createPhi({{drawFlag[i], checkOutPrimDrawFlagBlock}, {m_builder.getFalse(), endInitOutPrimCountBlock}});
      }
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        drawMask[i] = doSubgroupBallot(drawFlag[i]);

        outPrimCountInWave[i] = m_builder.CreateIntrinsic(Intrinsic::ctpop, m_builder.getInt64Ty(), drawMask[i]);
        outPrimCountInWave[i] = m_builder.CreateTrunc(outPrimCountInWave[i], m_builder.getInt32Ty());
      }
    }
    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(waveCountInSubgroup), m_nggInputs.waveIdInSubgroup);
    auto threadValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);

    m_builder.CreateCondBr(threadValid, accumOutPrimCountBlock, endAccumOutPrimCountBlock);
  }

  // Construct ".accumOutPrimCount" block
  {
    m_builder.SetInsertPoint(accumOutPrimCountBlock);

    unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimCountInWaves);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));
    ldsOffset = m_builder.CreateShl(ldsOffset, 2);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        m_ldsManager->atomicOpWithLds(
            AtomicRMWInst::Add, outPrimCountInWave[i],
            m_builder.CreateAdd(
                ldsOffset,
                m_builder.getInt32(regionStart + (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * i)));
      }
    }

    m_builder.CreateBr(endAccumOutPrimCountBlock);
  }

  // Construct ".endAccumOutPrimCount" block
  Value *primCountInPrevWaves[MaxGsStreams] = {};
  Value *primCountInSubgroup[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endAccumOutPrimCountBlock);

    createFenceAndBarrier();

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        auto outPrimCountInWaves =
            readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, LdsRegionOutPrimCountInWaves,
                                     (SizeOfDword * Gfx9::NggMaxWavesPerSubgroup + SizeOfDword) * i);

        // The last dword following dwords for all waves (each wave has one dword) stores GS output primitive count of
        // the entire subgroup
        primCountInSubgroup[i] = m_builder.CreateIntrinsic(
            Intrinsic::amdgcn_readlane, {}, {outPrimCountInWaves, m_builder.getInt32(waveCountInSubgroup)});

        // Get output primitive count for all waves prior to this wave
        primCountInPrevWaves[i] = m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {},
                                                            {outPrimCountInWaves, m_nggInputs.waveIdInSubgroup});
      }
    }

    m_builder.CreateCondBr(drawFlag[firstActiveStream], compactOutPrimIdBlock[firstActiveStream],
                           endCompactOutPrimIdBlock[firstActiveStream]);
  }

  SmallVector<XfbOutputExport, 32> xfbOutputExports;

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!streamActive[i])
      continue;

    // Construct ".compactOutPrimIdInStream[N]" block
    {
      m_builder.SetInsertPoint(compactOutPrimIdBlock[i]);

      auto drawMaskVec = m_builder.CreateBitCast(drawMask[i], FixedVectorType::get(m_builder.getInt32Ty(), 2));

      auto drawMaskLow = m_builder.CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
      Value *compactPrimitiveId =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder.getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder.CreateExtractElement(drawMaskVec, 1);
        compactPrimitiveId =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactPrimitiveId});
      }

      compactPrimitiveId = m_builder.CreateAdd(primCountInPrevWaves[i], compactPrimitiveId);
      writePerThreadDataToLds(m_nggInputs.threadIdInSubgroup, compactPrimitiveId, LdsRegionOutPrimThreadIdMap,
                              SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * i);

      m_builder.CreateBr(endCompactOutPrimIdBlock[i]);
    }

    // Construct ".endCompactOutPrimIdInStream[N]" block
    {
      m_builder.SetInsertPoint(endCompactOutPrimIdBlock[i]);

      if (i == lastActiveStream) {
        // Start to fetch transform feedback outputs after we finish compacting primitive thread IDs of the last vertex
        // stream.
        fetchXfbOutput(module, sysValueStart, xfbOutputExports);

        auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
        m_builder.CreateCondBr(firstThreadInSubgroup, prepareXfbExportBlock, endPrepareXfbExportBlock);
      } else {
        unsigned nextActiveStream = i + 1;
        while (!streamActive[nextActiveStream]) {
          ++nextActiveStream;
        }

        assert(nextActiveStream <= lastActiveStream);
        m_builder.CreateCondBr(drawFlag[nextActiveStream], compactOutPrimIdBlock[nextActiveStream],
                               endCompactOutPrimIdBlock[nextActiveStream]);
      }
    }
  }

  // Construct ".prepareXfbExport" block
  {
    m_builder.SetInsertPoint(prepareXfbExportBlock);

    const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();

    Value *numPrimsToWrite[MaxGsStreams] = {};
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      numPrimsToWrite[i] = primCountInSubgroup[i];

    Value *dwordsWritten[MaxTransformFeedbackBuffers] = {};
    Value *dwordsPerPrim[MaxTransformFeedbackBuffers] = {};

    // Calculate numPrimsToWrite[N]
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      if (i == firstActiveXfbBuffer) {
        // ds_ordered_count
        dwordsWritten[i] = m_builder.CreateIntrinsic(
            Intrinsic::amdgcn_ds_ordered_add, {},
            {
                m_builder.CreateIntToPtr(m_nggInputs.orderedWaveId,
                                         PointerType::get(m_builder.getInt32Ty(), ADDR_SPACE_REGION)), // m0
                m_builder.getInt32(0),                                                                 // value to add
                m_builder.getInt32(0),                                                                 // ordering
                m_builder.getInt32(0),                                                                 // scope
                m_builder.getFalse(),                                                                  // isVolatile
                m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) |
                                   (1 << 24)), // ordered count index, [27:24] is dword count
                m_builder.getFalse(),          // wave release
                m_builder.getFalse(),          // wave done
            });
      } else {
        // ds_add_gs_reg
        dwordsWritten[i] =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                      {m_builder.getInt32(0),                                         // value to add
                                       m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
      }

      // NUM_RECORDS = SQ_BUF_RSRC_WORD2
      Value *numRecords = m_builder.CreateExtractElement(m_streamOutBufDescs[i], 2);
      // bufferSizeInDwords = numRecords >> 2 (NOTE: NUM_RECORDS is set to the byte size of stream-out buffer)
      Value *bufferSizeInDwords = m_builder.CreateLShr(numRecords, 2);
      // dwordsRemaining = max(0, bufferSizeInDwords - (bufferOffset + dwordsWritten))
      Value *dwordsRemaining =
          m_builder.CreateSub(bufferSizeInDwords, m_builder.CreateAdd(m_streamOutBufOffsets[i], dwordsWritten[i]));
      dwordsRemaining = m_builder.CreateIntrinsic(Intrinsic::smax, dwordsRemaining->getType(),
                                                  {dwordsRemaining, m_builder.getInt32(0)});
      // numPrimsToWrite = min(dwordsRemaining / dwordsPerPrim, numPrimsToWrite)
      dwordsPerPrim[i] = m_builder.getInt32(outVertsPerPrim * xfbStrides[i] / SizeOfDword);
      Value *primsCanWrite = m_builder.CreateUDiv(dwordsRemaining, dwordsPerPrim[i]);
      numPrimsToWrite[xfbBufferToStream[i]] =
          m_builder.CreateIntrinsic(Intrinsic::umin, numPrimsToWrite[xfbBufferToStream[i]]->getType(),
                                    {numPrimsToWrite[xfbBufferToStream[i]], primsCanWrite});
    }

    // Increment dwordsWritten
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      Value *dwordsToWrite = m_builder.CreateMul(numPrimsToWrite[xfbBufferToStream[i]], dwordsPerPrim[i]);

      if (i == lastActiveXfbBuffer) {
        // ds_ordered_count, wave done
        dwordsWritten[i] = m_builder.CreateIntrinsic(
            Intrinsic::amdgcn_ds_ordered_add, {},
            {
                m_builder.CreateIntToPtr(m_nggInputs.orderedWaveId,
                                         PointerType::get(m_builder.getInt32Ty(), ADDR_SPACE_REGION)), // m0
                dwordsToWrite,                                                                         // value to add
                m_builder.getInt32(0),                                                                 // ordering
                m_builder.getInt32(0),                                                                 // scope
                m_builder.getFalse(),                                                                  // isVolatile
                m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) |
                                   (1 << 24)), // ordered count index, [27:24] is dword count
                m_builder.getTrue(),           // wave release
                m_builder.getTrue(),           // wave done
            });
      } else {
        // ds_add_gs_reg
        dwordsWritten[i] =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, dwordsToWrite->getType(),
                                      {dwordsToWrite,                                                 // value to add
                                       m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
      }
    }

    // Store transform feedback statistics info to LDS and GDS
    const unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionGsXfbStatInfo);
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      m_ldsManager->writeValueToLds(dwordsWritten[i], m_builder.getInt32(regionStart + i * SizeOfDword));
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (!streamActive[i])
        continue;

      m_ldsManager->writeValueToLds(
          numPrimsToWrite[i],
          m_builder.getInt32(regionStart + MaxTransformFeedbackBuffers * SizeOfDword + i * SizeOfDword));

      m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, primCountInSubgroup[i]->getType(),
                                {primCountInSubgroup[i],                                          // value to add
                                 m_builder.getInt32((GDS_STRMOUT_PRIMS_NEEDED_0 + 2 * i) << 2)}); // count index

      m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, numPrimsToWrite[i]->getType(),
                                {numPrimsToWrite[i],                                               // value to add
                                 m_builder.getInt32((GDS_STRMOUT_PRIMS_WRITTEN_0 + 2 * i) << 2)}); // count index
    }

    m_builder.CreateBr(endPrepareXfbExportBlock);
  }

  // Construct ".endPrepareXfbExport" block
  Value *streamOutOffsets[MaxTransformFeedbackBuffers] = {}; // Stream-out offset to write transform feedback outputs
  Value *numPrimsToWrite[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endPrepareXfbExportBlock);

    // We are going to read transform feedback statistics info from LDS. Make sure the info has been written before
    // this.
    createFenceAndBarrier();

    auto xfbStatInfo =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, LdsRegionGsXfbStatInfo);
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (bufferActive[i]) {
        streamOutOffsets[i] =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, {xfbStatInfo, m_builder.getInt32(i)});
        streamOutOffsets[i] = m_builder.CreateAdd(m_streamOutBufOffsets[i], streamOutOffsets[i]);
        streamOutOffsets[i] = m_builder.CreateShl(streamOutOffsets[i], 2);
      }
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (streamActive[i]) {
        numPrimsToWrite[i] = m_builder.CreateIntrinsic(
            Intrinsic::amdgcn_readlane, {}, {xfbStatInfo, m_builder.getInt32(MaxTransformFeedbackBuffers + i)});
      }
    }

    auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite[firstActiveStream]);
    m_builder.CreateCondBr(primValid, exportXfbOutputBlock[firstActiveStream],
                           endExportXfbOutputBlock[firstActiveStream]);
  }

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!streamActive[i])
      continue;

    // Construct ".exportXfbOutputInStream[N]" block
    {
      m_builder.SetInsertPoint(exportXfbOutputBlock[i]);

      Value *vertexIndices[3] = {};

      Value *uncompactedPrimitiveId =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup, LdsRegionOutPrimThreadIdMap,
                                   SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * i);
      Value *vertexIndex = uncompactedPrimitiveId;

      const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
      vertexIndices[0] = vertexIndex;

      if (outVertsPerPrim > 1)
        vertexIndices[1] = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(1));
      if (outVertsPerPrim > 2) {
        vertexIndices[2] = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(2));

        Value *primData = readPerThreadDataFromLds(m_builder.getInt32Ty(), uncompactedPrimitiveId, LdsRegionOutPrimData,
                                                   SizeOfDword * Gfx9::NggMaxThreadsPerSubgroup * i);
        Value *winding = m_builder.CreateICmpNE(primData, m_builder.getInt32(0));
        Value *vertexIndex1 = m_builder.CreateSelect(winding, vertexIndices[2], vertexIndices[1]);
        Value *vertexIndex2 = m_builder.CreateSelect(winding, vertexIndices[1], vertexIndices[2]);
        vertexIndices[1] = vertexIndex1;
        vertexIndices[2] = vertexIndex2;
      }

      for (unsigned j = 0; j < outVertsPerPrim; ++j) {
        for (unsigned k = 0; k < xfbOutputExports.size(); ++k) {
          const auto &xfbOutputExport = xfbOutputExports[k];
          if (xfbOutputExport.locInfo.streamId != i)
            continue; // Output not belong to this stream

          auto outputValue =
              importGsOutput(xfbOutputExport.numElements > 1
                                 ? FixedVectorType::get(m_builder.getFloatTy(), xfbOutputExport.numElements)
                                 : m_builder.getFloatTy(),
                             xfbOutputExport.locInfo.loc, i, calcVertexItemOffset(i, vertexIndices[j]));

          if (xfbOutputExport.is16bit) {
            // NOTE: For 16-bit transform feedbakc outputs, they are stored as 32-bit without tightly packed in LDS.
            outputValue = m_builder.CreateBitCast(
                outputValue, FixedVectorType::get(m_builder.getInt32Ty(), xfbOutputExport.numElements));
            outputValue = m_builder.CreateTrunc(
                outputValue, FixedVectorType::get(m_builder.getInt16Ty(), xfbOutputExport.numElements));
            outputValue = m_builder.CreateBitCast(
                outputValue, FixedVectorType::get(m_builder.getHalfTy(), xfbOutputExport.numElements));
          }

          unsigned format = 0;
          switch (xfbOutputExport.numElements) {
          case 1:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_FLOAT : BUF_FORMAT_32_FLOAT;
            break;
          case 2:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_FLOAT_GFX11;
            break;
          case 3:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_32_FLOAT_GFX11;
            break;
          case 4:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_16_16_FLOAT_GFX11 : BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
            break;
          default:
            llvm_unreachable("Unexpected element number!");
            break;
          }

          CoherentFlag coherent = {};
          coherent.bits.glc = true;
          coherent.bits.slc = true;

          // vertexOffset = (threadIdInSubgroup * outVertsPerPrim + vertexIndex) * xfbStride
          Value *vertexOffset = m_builder.CreateAdd(
              m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(outVertsPerPrim)),
              m_builder.getInt32(j));
          vertexOffset = m_builder.CreateMul(vertexOffset, m_builder.getInt32(xfbStrides[xfbOutputExport.xfbBuffer]));
          // xfbOutputOffset = vertexOffset + xfbOffset
          Value *xfbOutputOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(xfbOutputExport.xfbOffset));

          if (xfbOutputExport.is16bit && xfbOutputExport.numElements == 3) {
            // NOTE: For 16vec3, HW doesn't have a corresponding buffer store instruction. We have to split it to 16vec2
            // and 16scalar.
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store,
                                      FixedVectorType::get(m_builder.getHalfTy(), 2),
                                      {m_builder.CreateShuffleVector(outputValue, ArrayRef<int>{0, 1}), // vdata
                                       m_streamOutBufDescs[xfbOutputExport.xfbBuffer],                  // rsrc
                                       xfbOutputOffset,                                                 // offset
                                       streamOutOffsets[xfbOutputExport.xfbBuffer],                     // soffset
                                       m_builder.getInt32(BUF_FORMAT_16_16_FLOAT),                      // format
                                       m_builder.getInt32(coherent.u32All)}); // auxiliary data

            m_builder.CreateIntrinsic(
                Intrinsic::amdgcn_raw_tbuffer_store, m_builder.getHalfTy(),
                {m_builder.CreateExtractElement(outputValue, 2),                                 // vdata
                 m_streamOutBufDescs[xfbOutputExport.xfbBuffer],                                 // rsrc
                 m_builder.CreateAdd(xfbOutputOffset, m_builder.getInt32(2 * sizeof(uint16_t))), // offset
                 streamOutOffsets[xfbOutputExport.xfbBuffer],                                    // soffset
                 m_builder.getInt32(BUF_FORMAT_16_FLOAT),                                        // format
                 m_builder.getInt32(coherent.u32All)});                                          // auxiliary data
          } else {
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outputValue->getType(),
                                      {outputValue,                                    // vdata
                                       m_streamOutBufDescs[xfbOutputExport.xfbBuffer], // rsrc
                                       xfbOutputOffset,                                // offset
                                       streamOutOffsets[xfbOutputExport.xfbBuffer],    // soffset
                                       m_builder.getInt32(format),                     // format
                                       m_builder.getInt32(coherent.u32All)});          // auxiliary data
          }
        }
      }

      m_builder.CreateBr(endExportXfbOutputBlock[i]);
    }

    // Construct ".endExportXfbOutputInStream[N]" block
    {
      m_builder.SetInsertPoint(endExportXfbOutputBlock[i]);

      if (i != lastActiveStream) {
        unsigned nextActiveStream = i + 1;
        while (!streamActive[nextActiveStream]) {
          ++nextActiveStream;
        }

        assert(nextActiveStream <= lastActiveStream);
        auto primValid = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite[nextActiveStream]);
        m_builder.CreateCondBr(primValid, exportXfbOutputBlock[nextActiveStream],
                               endExportXfbOutputBlock[nextActiveStream]);
      }
    }
  }
}

// =====================================================================================================================
// Fetches transform feedback outputs by creating a fetch function cloned from ES/copy shader entry-point or just
// mutating existing ES and running it after that. Meanwhile, we collect the transform feedback export info.
//
// @param module : LLVM module.
// @param sysValueStart : Start of system value
// @param [out] xfbOutputExports : Export info of transform feedback outputs
Value *NggPrimShader::fetchXfbOutput(Module *module, Argument *sysValueStart,
                                     SmallVector<XfbOutputExport, 32> &xfbOutputExports) {
  assert(m_pipelineState->enableSwXfb());

  const unsigned xfbOutputCount =
      m_pipelineState
          ->getShaderResourceUsage(m_hasGs ? ShaderStageGeometry : (m_hasTes ? ShaderStageTessEval : ShaderStageVertex))
          ->inOutUsage.xfbOutputExpCount;

  // Skip following handling if transform feedback output is empty
  if (xfbOutputCount == 0)
    return nullptr;

  //
  // Clone ES/copy shader entry-point or just mutate existing ES entry-point to fetch transform feedback outputs
  //
  auto entryPoint = module->getFunction(m_hasGs ? lgcName::NggCopyShaderEntryPoint : lgcName::NggEsEntryPoint);
  assert(entryPoint);

  // We don't clone the entry-point if we are in passthrough mode without GS
  bool dontClone = !m_hasGs && m_nggControl->passthroughMode;

  // Collect all export calls for further analysis
  SmallVector<Function *, 8> expFuncs;
  for (auto &func : module->functions()) {
    if (dontClone) {
      if (func.getName().startswith(lgcName::NggXfbOutputExport))
        expFuncs.push_back(&func);
    } else {
      if ((func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp) ||
          func.getName().startswith(lgcName::NggAttribExport) || func.getName().startswith(lgcName::NggXfbOutputExport))
        expFuncs.push_back(&func);
    }
  }

  // Clone or mutate entry-point
  xfbOutputExports.resize(xfbOutputCount);

  // NOTE: For non-GS transform feedback, the return type is represented as an array of transform feedback outputs; for
  // GS transform feedback, the return type is void. This is because output values must be loaded by GS output import
  // call. Thus, we don't have to return output values. Instead, we recode the location in transform feedback export
  // info and fetch them later.
  Type *xfbOutputsTy = ArrayType::get(FixedVectorType::get(m_builder.getInt32Ty(), 4), xfbOutputCount);
  Type *xfbReturnTy = m_hasGs ? m_builder.getVoidTy() : xfbOutputsTy;

  Function *xfbOutputFetchFunc = entryPoint;
  if (dontClone) {
    processVertexAttribExport(entryPoint);
    xfbOutputFetchFunc = addFunctionArgs(entryPoint, xfbReturnTy, {}, {}, 0);

    // Original function is no longer needed
    assert(entryPoint->use_empty());
    entryPoint->eraseFromParent();
  } else {
    auto xfbOutputFetchFuncTy = FunctionType::get(xfbReturnTy, entryPoint->getFunctionType()->params(), false);
    xfbOutputFetchFunc = Function::Create(xfbOutputFetchFuncTy, entryPoint->getLinkage(), "", module);

    ValueToValueMapTy valueMap;

    Argument *newArg = xfbOutputFetchFunc->arg_begin();
    for (Argument &arg : entryPoint->args())
      valueMap[&arg] = newArg++;

    SmallVector<ReturnInst *, 8> retInsts;
    CloneFunctionInto(xfbOutputFetchFunc, entryPoint, valueMap, CloneFunctionChangeType::LocalChangesOnly, retInsts);
    xfbOutputFetchFunc->setName(m_hasGs ? lgcName::NggGsXfbOutputFetch : lgcName::NggEsXfbOutputFetch);
  }

  // Find the return block
  BasicBlock *retBlock = nullptr;
  for (BasicBlock &block : *xfbOutputFetchFunc) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      retInst->dropAllReferences();
      retInst->eraseFromParent();

      retBlock = &block;
      break;
    }
  }
  assert(retBlock);

  auto savedInsertPos = m_builder.saveIP();
  m_builder.SetInsertPoint(retBlock);

  // Visit all export calls, removing those unnecessary and mutating the return type
  SmallVector<CallInst *, 8> removeCalls;

  Value *xfbOutputs = UndefValue::get(xfbOutputsTy);
  unsigned outputIndex = 0;

  for (auto func : expFuncs) {
    for (auto user : func->users()) {
      CallInst *const call = dyn_cast<CallInst>(user);
      assert(call);

      if (!dontClone) {
        // Remove transform feedback output export calls from original ES/copy shader. No need of doing this if we
        // don't clone the entry-point.
        if (call->getFunction() == entryPoint && func->getName().startswith(lgcName::NggXfbOutputExport)) {
          removeCalls.push_back(call);
          continue;
        }
      }

      if (call->getFunction() != xfbOutputFetchFunc)
        continue;

      assert(call->getParent() == retBlock); // Must in return block

      if (func->getName().startswith(lgcName::NggXfbOutputExport)) {
        // Lower transform feedback export calls
        auto xfbBuffer = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        auto xfbOffset = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
        auto outputValue = call->getArgOperand(3);

        const unsigned numElements =
            outputValue->getType()->isVectorTy() ? cast<FixedVectorType>(outputValue->getType())->getNumElements() : 1;
        const bool is16bit = outputValue->getType()->getScalarSizeInBits() == 16;

        // Those values are just for GS
        auto streamId = InvalidValue;
        unsigned loc = InvalidValue;

        if (m_hasGs) {
          // NOTE: For GS, the output value must be loaded by GS output import call. This is generated by copy shader.
          CallInst *importCall = dyn_cast<CallInst>(outputValue);
          assert(importCall && importCall->getCalledFunction()->getName().startswith(lgcName::NggGsOutputImport));
          streamId = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
          assert(streamId == cast<ConstantInt>(importCall->getArgOperand(1))->getZExtValue()); // Stream ID must match
          loc = cast<ConstantInt>(importCall->getArgOperand(0))->getZExtValue();
        } else {
          // If the output value is floating point, cast it to integer type
          if (outputValue->getType()->isFPOrFPVectorTy()) {
            if (numElements == 1) {
              outputValue =
                  m_builder.CreateBitCast(outputValue, is16bit ? m_builder.getInt16Ty() : m_builder.getInt32Ty());
            } else {
              outputValue = m_builder.CreateBitCast(
                  outputValue,
                  FixedVectorType::get(is16bit ? m_builder.getInt16Ty() : m_builder.getInt32Ty(), numElements));
            }
          }

          // If the output value is 16-bit, zero-extend it to 32-bit
          if (is16bit)
            outputValue = m_builder.CreateZExt(outputValue, FixedVectorType::get(m_builder.getInt32Ty(), numElements));

          // Always pad the output value to <4 x i32>
          if (numElements == 1) {
            outputValue =
                m_builder.CreateInsertElement(UndefValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 4)),
                                              outputValue, static_cast<uint64_t>(0));
          } else if (numElements < 4) {
            outputValue = m_builder.CreateShuffleVector(outputValue, UndefValue::get(outputValue->getType()),
                                                        ArrayRef<int>({0U, 1U, 2U, 3U}));
          }
        }

        // For VS/TES, return the output value
        if (!m_hasGs)
          xfbOutputs = m_builder.CreateInsertValue(xfbOutputs, outputValue, outputIndex);

        // Collect export info
        xfbOutputExports[outputIndex].xfbBuffer = xfbBuffer;
        xfbOutputExports[outputIndex].xfbOffset = xfbOffset;
        xfbOutputExports[outputIndex].numElements = numElements;
        xfbOutputExports[outputIndex].is16bit = is16bit;
        // Those values are just for GS
        xfbOutputExports[outputIndex].locInfo.streamId = streamId;
        xfbOutputExports[outputIndex].locInfo.loc = loc;

        ++outputIndex;
      }

      removeCalls.push_back(call); // Remove export
    }
  }

  assert(outputIndex == xfbOutputCount); // Visit all transform feedback output export calls
  m_builder.CreateRet(xfbOutputs);

  // Remove calls
  for (auto call : removeCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }

  m_builder.restoreIP(savedInsertPos);

  //
  // Run transform feedback output fetch function
  //
  if (m_hasGs) {
    // Copy shader has fixed argument layout
    Value *userData = sysValueStart + NumSpecialSgprInputs;
    assert(userData->getType()->isVectorTy());

    auto globalTable = m_builder.CreateExtractElement(userData, static_cast<uint64_t>(0));
    return m_builder.CreateCall(xfbOutputFetchFunc,
                                {globalTable,                      // Global table
                                 m_nggInputs.threadIdInSubgroup}); // Vertex ID in sub-rgoup
  }

  Argument *arg = sysValueStart;

  Value *offChipLdsBase = (arg + ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase));
  offChipLdsBase->setName("offChipLdsBase");

  Value *isOffChip = UndefValue::get(m_builder.getInt32Ty()); // NOTE: This flag is unused.

  arg += NumSpecialSgprInputs;

  Value *userData = arg++;

  Value *tessCoordX = (arg + 5);
  Value *tessCoordY = (arg + 6);
  Value *relPatchId = (arg + 7);
  Value *patchId = (arg + 8);

  Value *vertexId = (arg + 5);
  Value *relVertexId = (arg + 6);
  // NOTE: VS primitive ID for NGG is specially obtained from primitive ID distribution.
  Value *vsPrimitiveId = m_distributedPrimitiveId ? m_distributedPrimitiveId : PoisonValue::get(m_builder.getInt32Ty());
  Value *instanceId = (arg + 8);

  std::vector<Value *> args;

  // If we don't clone the entry-point, we are going to run the whole ES and handle vertex attribute through memory
  // here.
  if (dontClone) {
    // Setup attribute ring base and vertex thread ID in subgroup as two additional arguments to export vertex
    // attributes through memory
    if (m_gfxIp.major >= 11 && !m_hasGs) { // For GS, vertex attribute exports are in copy shader
      const auto attribCount =
          m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)
              ->inOutUsage.expCount;
      if (attribCount > 0) {
        args.push_back(m_nggInputs.attribRingBase);
        args.push_back(m_nggInputs.threadIdInSubgroup);
      }
    }
  }

  auto intfData = m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
  const unsigned userDataCount = intfData->userDataCount;

  unsigned userDataIdx = 0;

  auto argBegin = xfbOutputFetchFunc->arg_begin();
  const unsigned argCount = xfbOutputFetchFunc->arg_size();
  (void(argCount)); // Unused

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(args.size() < argCount);

    auto arg = (argBegin + args.size());
    assert(arg->hasAttribute(Attribute::InReg));

    auto argTy = arg->getType();
    if (argTy->isVectorTy()) {
      assert(cast<VectorType>(argTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<FixedVectorType>(argTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      args.push_back(m_builder.CreateShuffleVector(userData, userData, shuffleMask));
      userDataIdx += userDataSize;
    } else {
      assert(argTy->isIntegerTy());
      args.push_back(m_builder.CreateExtractElement(userData, userDataIdx));
      ++userDataIdx;
    }
  }

  if (m_hasTes) {
    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      args.push_back(isOffChip);
      args.push_back(offChipLdsBase);
    }

    // Set up system value VGPRs
    args.push_back(tessCoordX);
    args.push_back(tessCoordY);
    args.push_back(relPatchId);
    args.push_back(patchId);
  } else {
    // Set up system value VGPRs
    args.push_back(vertexId);
    args.push_back(relVertexId);
    args.push_back(vsPrimitiveId);
    args.push_back(instanceId);

    if (m_nggControl->passthroughMode) {
      // When tessellation is not enabled, the ES is actually a fetchless VS. Then, we need to add arguments for the
      // vertex fetches. Also set the name of each vertex fetch primitive shader argument while we're here.
      unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
      if (vertexFetchCount != 0) {
        // The last vertexFetchCount arguments of the primitive shader and ES are the vertex fetches
        Function *primShader = m_builder.GetInsertBlock()->getParent();
        unsigned primArgCount = primShader->arg_size();
        for (unsigned i = 0; i != vertexFetchCount; ++i) {
          Argument *vertexFetch = primShader->getArg(primArgCount - vertexFetchCount + i);
          vertexFetch->setName(
              xfbOutputFetchFunc->getArg(argCount - vertexFetchCount + i)->getName()); // Copy argument name
          args.push_back(vertexFetch);
        }
      }
    }
  }

  assert(args.size() == argCount); // Must have visit all arguments

  return m_builder.CreateCall(xfbOutputFetchFunc, args);
}

// =====================================================================================================================
// Reads transform feedback output from LDS
//
// @param readDataTy : Data read from LDS
// @param vertexIndex: Relative vertex index in NGG subgroup
// @param outputIndex : Index of this transform feedback output
Value *NggPrimShader::readXfbOutputFromLds(llvm::Type *readDataTy, llvm::Value *vertexIndex, unsigned outputIndex) {
  assert(m_pipelineState->enableSwXfb()); // SW-emulated stream-out must be enabled
  assert(!m_hasGs);

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;
  auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize * SizeOfDword));

  if (m_nggControl->passthroughMode) {
    const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionXfbOutput);
    Value *ldsOffset =
        m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + SizeOfVec4 * outputIndex));
    return m_ldsManager->readValueFromLds(readDataTy, ldsOffset);
  }

  // NOTE: For NGG culling mode, transform feedback outputs are part of vertex cull info.
  const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(
      vertexItemOffset, m_builder.getInt32(regionStart + m_vertCullInfoOffsets.xfbOutputs + SizeOfVec4 * outputIndex));
  return m_ldsManager->readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes transform feedback output from LDS
//
// @param writeData : Data written to LDS
// @param vertexIndex: Relative vertex index in NGG subgroup
// @param outputIndex : Index of this transform feedback output
void NggPrimShader::writeXfbOutputToLds(Value *writeData, Value *vertexIndex, unsigned outputIndex) {
  assert(m_pipelineState->enableSwXfb()); // SW-emulated stream-out must be enabled
  assert(!m_hasGs);

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;
  auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize * SizeOfDword));

  if (m_nggControl->passthroughMode) {
    const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionXfbOutput);
    Value *ldsOffset =
        m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + SizeOfVec4 * outputIndex));
    m_ldsManager->writeValueToLds(writeData, ldsOffset);
    return;
  }

  // NOTE: For NGG culling mode, transform feedback outputs are part of vertex cull info.
  const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(
      vertexItemOffset, m_builder.getInt32(regionStart + m_vertCullInfoOffsets.xfbOutputs + SizeOfVec4 * outputIndex));
  m_ldsManager->writeValueToLds(writeData, ldsOffset);
}

// =====================================================================================================================
// Fetches the position data for the specified relative vertex index.
//
// @param vertexIndex : Relative vertex index in NGG subgroup.
Value *NggPrimShader::fetchVertexPositionData(Value *vertexIndex) {
  if (!m_hasGs) {
    // ES-only
    return readPerThreadDataFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), vertexIndex, LdsRegionVertPosData,
                                    0, true);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInPosition];
  const unsigned rasterStream = inOutUsage.gs.rasterStream;
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexIndex);

  return importGsOutput(FixedVectorType::get(m_builder.getFloatTy(), 4), loc, rasterStream, vertexOffset);
}

// =====================================================================================================================
// Fetches the aggregated sign mask of cull distances for the specified relative vertex index.
//
// @param vertexIndex : Relative vertex index in NGG subgroup.
Value *NggPrimShader::fetchCullDistanceSignMask(Value *vertexIndex) {
  assert(m_nggControl->enableCullDistanceCulling);

  if (!m_hasGs) {
    // ES-only
    const unsigned esGsRingItemSize =
        m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;
    auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize * SizeOfDword));
    return readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset,
                                     m_vertCullInfoOffsets.cullDistanceSignMask);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
  const unsigned rasterStream = inOutUsage.gs.rasterStream;
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexIndex);

  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;
  auto cullDistances = importGsOutput(ArrayType::get(m_builder.getFloatTy(), builtInUsage.cullDistance), loc,
                                      rasterStream, vertexOffset);

  // Calculate the sign mask for all cull distances
  Value *signMask = m_builder.getInt32(0);
  for (unsigned i = 0; i < builtInUsage.cullDistance; ++i) {
    auto cullDistance = m_builder.CreateExtractValue(cullDistances, i);
    cullDistance = m_builder.CreateBitCast(cullDistance, m_builder.getInt32Ty());

    Value *signBit = createUBfe(cullDistance, 31, 1);
    signBit = m_builder.CreateShl(signBit, i);
    signMask = m_builder.CreateOr(signMask, signBit);
  }

  return signMask;
}

// =====================================================================================================================
// Calculates the starting LDS offset (in bytes) of vertex item data in GS-VS ring.
//
// @param streamId : ID of output vertex stream.
// @param vertexIndex : Relative vertex index in NGG subgroup.
Value *NggPrimShader::calcVertexItemOffset(unsigned streamId, Value *vertexIndex) {
  assert(m_hasGs); // GS must be present

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

  // vertexOffset = gsVsRingStart + (streamBases[stream] + vertexIndex * vertexItemSize) * 4 (in bytes)
  const unsigned vertexItemSize = 4 * inOutUsage.gs.outLocCount[streamId];
  auto vertexOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(vertexItemSize));
  vertexOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(m_gsStreamBases[streamId]));
  vertexOffset = m_builder.CreateShl(vertexOffset, 2);

  const unsigned gsVsRingStart = m_ldsManager->getLdsRegionStart(LdsRegionGsVsRing);
  vertexOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(gsVsRingStart));

  return vertexOffset;
}

// =====================================================================================================================
// Creates a new basic block. Always insert it at the end of the parent function.
//
// @param parent : Parent function to which the new block belongs
// @param blockName : Name of the new block
BasicBlock *NggPrimShader::createBlock(Function *parent, const Twine &blockName) {
  return BasicBlock::Create(m_builder.getContext(), blockName, parent);
}

// =====================================================================================================================
// Extracts bitfield [offset, offset + count - 1] from the source value (int32). This is a substitute of the intrinsic
// amdgcn_ubfe when the offset and count are both constants.
//
// @param value : Source value to extract
// @param offset : Bit number of least-significant end of bitfield
// @param count : Count of bits in bitfield
// @returns : The extracted bitfield
Value *NggPrimShader::createUBfe(Value *value, unsigned offset, unsigned count) {
  assert(value->getType()->isIntegerTy(32));
  assert(offset <= 31 && count >= 1 && offset + count - 1 <= 31);

  if (count == 32)
    return value; // Return the whole

  if (offset == 0)
    return m_builder.CreateAnd(value, (1U << count) - 1); // Just need mask

  return m_builder.CreateAnd(m_builder.CreateLShr(value, offset), (1U << count) - 1);
}

// =====================================================================================================================
// Create a PHI node with the specified incomings.
//
// @param incomings : A set of incomings to create this PHI node
// @param name : Name of this PHI node
// @returns : The created PHI node
PHINode *NggPrimShader::createPhi(ArrayRef<std::pair<Value *, BasicBlock *>> incomings, const Twine &name) {
  assert(incomings.size() >= 2); // Must at least have two incomings

  auto phiType = incomings[0].first->getType();
  auto phi = m_builder.CreatePHI(phiType, incomings.size(), name);

  for (auto &incoming : incomings) {
    assert(incoming.first->getType() == phiType);
    phi->addIncoming(incoming.first, incoming.second);
  }

  return phi;
}

// =====================================================================================================================
// Create LDS fence and barrier to guarantee the synchronization of LDS operations.
void NggPrimShader::createFenceAndBarrier() {
  SyncScope::ID syncScope = m_builder.getContext().getOrInsertSyncScopeID("workgroup");
  m_builder.CreateFence(AtomicOrdering::Release, syncScope);
  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
  m_builder.CreateFence(AtomicOrdering::Acquire, syncScope);
}

} // namespace lgc
