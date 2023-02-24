/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  RegisterMetadataBuilder.cpp
 * @brief LLPC header file: contains implementation of class lgc::RegisterMetadataBuilder.
 ***********************************************************************************************************************
 */
#include "RegisterMetadataBuilder.h"
#include "Gfx9Chip.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

#define DEBUG_TYPE "lgc-register-metadata-builder"

using namespace llvm;

namespace lgc {

namespace Gfx9 {

#include "chip/gfx9/gfx9_plus_merged_enum.h"

using namespace Pal::Gfx9::Chip;

// =====================================================================================================================
// Builds PAL metadata for pipeline.
void RegisterMetadataBuilder::buildPalMetadata() {
  if (m_pipelineState->isGraphics()) {
    const bool hasTs = (m_hasTcs || m_hasTes);
    m_isNggMode = false;
    if (m_gfxIp.major >= 11)
      m_isNggMode = true;
    else if (m_gfxIp.major == 10)
      m_isNggMode = m_pipelineState->getNggControl()->enableNgg;

    Util::Abi::PipelineType pipelineType = Util::Abi::PipelineType::VsPs;

    DenseMap<unsigned, unsigned> apiHwShaderMap;
    if (m_hasTask || m_hasMesh) {
      assert(m_pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3}));
      apiHwShaderMap[ShaderStageMesh] = Util::Abi::HwShaderGs;
      pipelineType = Util::Abi::PipelineType::Mesh;
      if (m_hasTask) {
        apiHwShaderMap[ShaderStageTask] = Util::Abi::HwShaderCs;
        pipelineType = Util::Abi::PipelineType::TaskMesh;
      }
    } else {
      if (m_hasGs) {
        auto preGsStage = m_pipelineState->getPrevShaderStage(ShaderStageGeometry);
        apiHwShaderMap[preGsStage] = Util::Abi::HwShaderGs;
      }
      if (hasTs) {
        apiHwShaderMap[ShaderStageVertex] = Util::Abi::HwShaderHs;
        apiHwShaderMap[ShaderStageTessControl] = Util::Abi::HwShaderHs;
      }
      auto lastVertexProcessingStage = m_pipelineState->getLastVertexProcessingStage();
      if (m_isNggMode) {
        apiHwShaderMap[lastVertexProcessingStage] = Util::Abi::HwShaderGs;
        pipelineType = hasTs ? Util::Abi::PipelineType::NggTess : Util::Abi::PipelineType::Ngg;
      } else {
        apiHwShaderMap[lastVertexProcessingStage] = Util::Abi::HwShaderVs;
        if (m_hasGs)
          apiHwShaderMap[lastVertexProcessingStage] |= Util::Abi::HwShaderGs;

        pipelineType = Util::Abi::PipelineType::GsTess;
        if (hasTs && !m_hasGs)
          pipelineType = Util::Abi::PipelineType::Tess;
        else if (!hasTs && m_hasGs)
          pipelineType = Util::Abi::PipelineType::Gs;
      }
    }
    apiHwShaderMap[ShaderStageFragment] = Util::Abi::HwShaderPs;

    // Set the mapping between api shader stage and hardware stage
    unsigned hwStageMask = 0;
    for (const auto &entry : apiHwShaderMap) {
      const auto apiStage = static_cast<ShaderStage>(entry.first);
      hwStageMask |= entry.second;
      addApiHwShaderMapping(apiStage, entry.second);
    }

    if (hwStageMask & Util::Abi::HwShaderHs) {
      buildLsHsRegisters();
      ShaderStage apiStage1 = m_hasVs ? ShaderStageVertex : ShaderStageInvalid;
      ShaderStage apiStage2 = m_hasTcs ? ShaderStageTessControl : ShaderStageInvalid;
      buildShaderExecutionRegisters(Util::Abi::HardwareStage::Hs, apiStage1, apiStage2);
    }
    if (hwStageMask & Util::Abi::HwShaderGs) {
      if (m_isNggMode)
        buildPrimShaderRegisters();
      else
        buildEsGsRegisters();

      ShaderStage apiStage1 = ShaderStageInvalid;
      ShaderStage apiStage2 = ShaderStageInvalid;
      if (m_hasMesh) {
        apiStage1 = ShaderStageMesh;
      } else if (m_hasGs) {
        apiStage2 = ShaderStageGeometry;
        if (m_hasTes)
          apiStage1 = ShaderStageTessEval;
        else if (m_hasVs)
          apiStage1 = ShaderStageVertex;
      } else if (m_hasTes) {
        apiStage1 = ShaderStageTessEval;
      } else {
        apiStage1 = ShaderStageVertex;
      }
      buildShaderExecutionRegisters(Util::Abi::HardwareStage::Gs, apiStage1, apiStage2);
    }
    if (hwStageMask & Util::Abi::HwShaderVs) {
      buildHwVsRegisters();
      ShaderStage apiStage1 = ShaderStageVertex;
      if (m_pipelineState->hasShaderStage(ShaderStageCopyShader))
        apiStage1 = ShaderStageCopyShader;
      else if (m_hasTes)
        apiStage1 = ShaderStageTessEval;
      buildShaderExecutionRegisters(Util::Abi::HardwareStage::Vs, apiStage1, ShaderStageInvalid);
    }
    if (hwStageMask & Util::Abi::HwShaderPs) {
      buildPsRegisters();
      buildShaderExecutionRegisters(Util::Abi::HardwareStage::Ps, ShaderStageFragment, ShaderStageInvalid);
    }
    if (hwStageMask & Util::Abi::HwShaderCs) {
      buildCsRegisters(ShaderStageTask);
      buildShaderExecutionRegisters(Util::Abi::HardwareStage::Cs, ShaderStageTask, ShaderStageInvalid);
    }

    buildPaSpecificRegisters();
    setVgtShaderStagesEn(hwStageMask);
    setIaMultVgtParam();
    setPipelineType(pipelineType);
  } else {
    addApiHwShaderMapping(ShaderStageCompute, Util::Abi::HwShaderCs);
    setPipelineType(Util::Abi::PipelineType::Cs);
    buildCsRegisters(ShaderStageCompute);
    buildShaderExecutionRegisters(Util::Abi::HardwareStage::Cs, ShaderStageCompute, ShaderStageInvalid);
  }
}

// =====================================================================================================================
// Builds register configuration for hardware local-hull merged shader.
void RegisterMetadataBuilder::buildLsHsRegisters() {
  assert(m_hasTcs);
  // VGT_HOS_MIN(MAX)_TESS_LEVEL
  // Minimum and maximum tessellation factors supported by the hardware.
  constexpr float minTessFactor = 1.0f;
  constexpr float maxTessFactor = 64.0f;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtHosMinTessLevel] = bit_cast<uint32_t>(minTessFactor);
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtHosMaxTessLevel] = bit_cast<uint32_t>(maxTessFactor);

  // VGT_LS_HS_CONFIG
  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  auto vgtLsHsConfig = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtLsHsConfig].getMap(true);
  vgtLsHsConfig[Util::Abi::VgtLsHsConfigMetadataKey::NumPatches] = calcFactor.patchCountPerThreadGroup;
  vgtLsHsConfig[Util::Abi::VgtLsHsConfigMetadataKey::HsNumInputCp] =
      m_pipelineState->getInputAssemblyState().patchControlPoints;
  vgtLsHsConfig[Util::Abi::VgtLsHsConfigMetadataKey::HsNumOutputCp] =
      m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;

  // VGT_TF_PARAM
  setVgtTfParam();
}

// =====================================================================================================================
// Builds register configuration for hardware export-geometry merged shader.
void RegisterMetadataBuilder::buildEsGsRegisters() {
  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  const auto &vsBuiltInUsage = vsResUsage->builtInUsage.vs;
  const auto gsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  const auto &gsBuiltInUsage = gsResUsage->builtInUsage.gs;
  const auto &gsInOutUsage = gsResUsage->inOutUsage;
  const auto &calcFactor = gsInOutUsage.gs.calcFactor;
  const auto tesResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  const auto &tesBuiltInUsage = tesResUsage->builtInUsage.tes;

  // ES_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC2_GS
  unsigned gsVgprCompCnt = 0;
  if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
    gsVgprCompCnt = 3;
  else if (gsBuiltInUsage.primitiveIdIn)
    gsVgprCompCnt = 2;
  else if (calcFactor.inputVertices > 2)
    gsVgprCompCnt = 1;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::GsVgprCompCnt] = gsVgprCompCnt;

  // ES_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC2_GS
  unsigned esVgprCompCnt = 0;
  if (m_hasTcs || m_hasTes) {
    // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
    if (tesBuiltInUsage.primitiveId)
      esVgprCompCnt = 3;
    else
      esVgprCompCnt = 2;

    if (m_pipelineState->isTessOffChip())
      getHwShaderNode(Util::Abi::HardwareStage::Gs)[Util::Abi::HardwareStageMetadataKey::OffchipLdsEn] = true;
  } else {
    if (vsBuiltInUsage.instanceIndex)
      esVgprCompCnt = 3; // Enable instance ID
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::EsVgprCompCnt] = esVgprCompCnt;

  // VGT_GS_MAX_VERT_OUT
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsMaxVertOut] = maxVertOut;

  // VGT_GS_MODE
  auto vgtGsMode = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsMode].getMap(true);
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::Mode] = GS_SCENARIO_G;
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::EsWriteOptimize] = false;
  if (m_pipelineState->isGsOnChip()) {
    vgtGsMode[Util::Abi::VgtGsModeMetadataKey::Onchip] = VGT_GS_MODE_ONCHIP_ON;
    vgtGsMode[Util::Abi::VgtGsModeMetadataKey::GsWriteOptimize] = false;
  } else {
    vgtGsMode[Util::Abi::VgtGsModeMetadataKey::Onchip] = VGT_GS_MODE_ONCHIP_OFF;
    vgtGsMode[Util::Abi::VgtGsModeMetadataKey::GsWriteOptimize] = true;
  }
  unsigned cutModeVal = GS_CUT_1024__HASHWVS;
  if (geometryMode.outputVertices <= 128)
    cutModeVal = GS_CUT_128__HASHWVS;
  else if (geometryMode.outputVertices <= 256)
    cutModeVal = GS_CUT_256__HASHWVS;
  else if (geometryMode.outputVertices <= 512)
    cutModeVal = GS_CUT_512__HASHWVS;
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::CutMode] = cutModeVal;

  // VGT_GS_ONCHIP_CNTL
  auto vgtGsOnChipCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsOnchipCntl].getMap(true);
  vgtGsOnChipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::EsVertsPerSubgroup] = calcFactor.esVertsPerSubgroup;
  vgtGsOnChipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::GsPrimsPerSubgroup] = calcFactor.gsPrimsPerSubgroup;
  // NOTE: The value of field "GS_INST_PRIMS_IN_SUBGRP" should be strictly equal to the product of
  // VGT_GS_ONCHIP_CNTL.GS_PRIMS_PER_SUBGRP * VGT_GS_INSTANCE_CNT.CNT.
  const unsigned gsInstPrimsInSubgrp =
      geometryMode.invocations > 1 ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations) : 0;
  vgtGsOnChipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::GsInstPrimsPerSubgrp] = gsInstPrimsInSubgrp;

  // VGT_GS_VERT_ITEMSIZE and VGT_GSVS_RING_OFFSET
  auto itemSizeArrayNode =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsVertItemsize].getArray(true);
  auto ringOffsetArrayNode =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsvsRingOffset].getArray(true);
  const unsigned itemCount = 4;
  unsigned gsVsRingOffset = 0;
  for (unsigned i = 0; i < itemCount; ++i) {
    unsigned itemSize = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[i];
    gsVsRingOffset += itemSize * maxVertOut;
    itemSizeArrayNode[i] = itemSize;
    ringOffsetArrayNode[i] = gsVsRingOffset;
  }

  // VGT_GS_INSTANCE_CNT
  if (geometryMode.invocations > 1 || gsBuiltInUsage.invocationId) {
    auto vgtGsInstanceCnt = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsInstanceCnt].getMap(true);
    vgtGsInstanceCnt[Util::Abi::VgtGsInstanceCntMetadataKey::Enable] = true;
    vgtGsInstanceCnt[Util::Abi::VgtGsInstanceCntMetadataKey::Count] = geometryMode.invocations;
  }

  // VGT_GS_PER_VS
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsPerVs] = GsThreadsPerVsThread;

  // VGT_GS_OUT_PRIM_TYPE
  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
  if (geometryMode.outputPrimitive == OutputPrimitives::Points)
    gsOutputPrimitiveType = POINTLIST;
  else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
    gsOutputPrimitiveType = LINESTRIP;
  auto vgtGsOutPrimType = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsOutPrimType].getMap(true);
  vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType] = gsOutputPrimitiveType;
  // Set multi-stream output primitive type
  if (itemSizeArrayNode[1].getInt() > 0 || itemSizeArrayNode[2].getInt() > 0 || itemSizeArrayNode[3].getInt() > 0) {
    const static auto GsOutPrimInvalid = 3u;
    vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType_1] =
        itemSizeArrayNode[1].getInt() > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid;
    vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType_2] =
        itemSizeArrayNode[2].getInt() > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid;
    vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType_3] =
        itemSizeArrayNode[3].getInt() > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid;
  }

  // VGT_GSVS_RING_ITEMSIZE
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsvsRingItemsize] = calcFactor.gsVsRingItemSize;

  // VGT_ESGS_RING_ITEMSIZE
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtEsgsRingItemsize] = calcFactor.esGsRingItemSize;

  // GE_MAX_OUTPUT_PER_SUBGROUP and VGT_GS_MAX_PRIMS_PER_SUBGROUP
  const unsigned maxPrimsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, MaxGsThreadsPerSubgroup);
  if (m_gfxIp.major == 9)
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::MaxPrimsPerSubgroup] = maxPrimsPerSubgroup;
  else
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::MaxVertsPerSubgroup] = maxPrimsPerSubgroup;

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
#if VKI_RAY_TRACING
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;
#endif
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Gs);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = ldsSizeInDwords * 4;
  setEsGsLdsSize(calcFactor.esGsLdsSize * 4);
}

// =====================================================================================================================
// Builds register configuration for hardware primitive shader.
void RegisterMetadataBuilder::buildPrimShaderRegisters() {
  assert(m_gfxIp.major >= 10 || (m_hasMesh && m_gfxIp >= GfxIpVersion({10, 3})));
  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  const auto &vsBuiltInUsage = vsResUsage->builtInUsage.vs;
  const auto tesResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  const auto &tesBuiltInUsage = tesResUsage->builtInUsage.tes;
  const auto gsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  const auto &gsBuiltInUsage = gsResUsage->builtInUsage.gs;
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const auto &gsInOutUsage = gsResUsage->inOutUsage;
  const auto &calcFactor = gsInOutUsage.gs.calcFactor;
  const auto meshResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
  const auto &meshBuiltInUsage = meshResUsage->builtInUsage.mesh;
  const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const bool hasTs = m_hasTcs || m_hasTes;

  // RSRC1, RSRC2 and RSRC3 are handled by the HardwareStage metadata, with the exception of specifal bits are handled
  // by GraphicsRegisters metadata GS_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC1_GS
  unsigned gsVgprCompCnt = 0;
  if (m_hasGs) {
    if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
      gsVgprCompCnt = 3;
    else if (gsBuiltInUsage.primitiveIdIn)
      gsVgprCompCnt = 2;
    else if (calcFactor.inputVertices > 2)
      gsVgprCompCnt = 1;
  } else if (!m_hasMesh) {
    // NOTE: When GS is absent, only those VGPRs are required: vtx0/vtx1 offset, vtx2/vtx3 offset,
    // primitive ID (only for VS).
    gsVgprCompCnt = hasTs ? 1 : (vsBuiltInUsage.primitiveId ? 2 : 1);
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::GsVgprCompCnt] = gsVgprCompCnt;

  // ES_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC2_GS
  unsigned esVgprCompCnt = 0;
  if (hasTs) {
    // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
    if (tesBuiltInUsage.primitiveId)
      esVgprCompCnt = 3;
    else
      esVgprCompCnt = 2;
  } else if (!m_hasMesh) {
    if (vsBuiltInUsage.instanceIndex)
      esVgprCompCnt = 3; // Enable instance ID
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::EsVgprCompCnt] = esVgprCompCnt;

  // VGT_GS_MAX_VERT_OUT
  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsMaxVertOut] = maxVertOut;

  // VGT_GS_MODE
  auto vgtGsMode = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsMode].getMap(true);
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::Mode] = GS_SCENARIO_G;
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::Onchip] = VGT_GS_MODE_ONCHIP_OFF;
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::EsWriteOptimize] = false;
  vgtGsMode[Util::Abi::VgtGsModeMetadataKey::GsWriteOptimize] = true;

  // VGT_GS_ONCHIP_CNTL
  auto vgtGsOnchipCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsOnchipCntl].getMap(true);
  vgtGsOnchipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::EsVertsPerSubgroup] = calcFactor.esVertsPerSubgroup;
  vgtGsOnchipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::GsPrimsPerSubgroup] = calcFactor.gsPrimsPerSubgroup;

  unsigned gsInstPrimsInSubgrp = 1;
  if (!m_hasMesh) {
    gsInstPrimsInSubgrp = geometryMode.invocations > 1 ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations)
                                                       : calcFactor.gsPrimsPerSubgroup;
  }
  vgtGsOnchipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::GsInstPrimsPerSubgrp] = gsInstPrimsInSubgrp;

  // VGT_GS_PER_VS
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsPerVs] = GsThreadsPerVsThread;

  // VGT_GS_OUT_PRIM_TYPE
  // TODO: Multiple output streams are not supported.
  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = POINTLIST;
  if (m_hasMesh) {
    if (meshMode.outputPrimitive == OutputPrimitives::Points)
      gsOutputPrimitiveType = POINTLIST;
    else if (meshMode.outputPrimitive == OutputPrimitives::Lines)
      gsOutputPrimitiveType = LINESTRIP;
    else if (meshMode.outputPrimitive == OutputPrimitives::Triangles)
      gsOutputPrimitiveType = TRISTRIP;
    else
      llvm_unreachable("Should never be called!");
  }
  if (m_hasGs) {
    // GS present
    if (gsInOutUsage.outputMapLocCount == 0)
      gsOutputPrimitiveType = POINTLIST;
    else if (geometryMode.outputPrimitive == OutputPrimitives::Points)
      gsOutputPrimitiveType = POINTLIST;
    else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
      gsOutputPrimitiveType = LINESTRIP;
    else if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
      gsOutputPrimitiveType = TRISTRIP;
    else
      llvm_unreachable("Should never be called!");
  } else if (hasTs) {
    // With tessellation
    const auto &tessMode = m_pipelineState->getShaderModes()->getTessellationMode();
    if (tessMode.pointMode)
      gsOutputPrimitiveType = POINTLIST;
    else if (tessMode.primitiveMode == PrimitiveMode::Isolines)
      gsOutputPrimitiveType = LINESTRIP;
    else if (tessMode.primitiveMode == PrimitiveMode::Triangles || tessMode.primitiveMode == PrimitiveMode::Quads)
      gsOutputPrimitiveType = TRISTRIP;
    else
      llvm_unreachable("Should never be called!");
  } else {
    // Without tessellation
    const auto primType = m_pipelineState->getInputAssemblyState().primitiveType;
    switch (primType) {
    case PrimitiveType::Point:
      gsOutputPrimitiveType = POINTLIST;
      break;
    case PrimitiveType::LineList:
    case PrimitiveType::LineStrip:
      gsOutputPrimitiveType = LINESTRIP;
      break;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TriangleFan:
    case PrimitiveType::TriangleListAdjacency:
    case PrimitiveType::TriangleStripAdjacency:
      gsOutputPrimitiveType = TRISTRIP;
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }
  }
  auto vgtGsOutPrimType = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsOutPrimType].getMap(true);
  vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType] = gsOutputPrimitiveType;

  assert(calcFactor.primAmpFactor >= 1);
  unsigned maxVertsPerSubgroup = NggMaxThreadsPerSubgroup;
  unsigned threadsPerSubgroup = NggMaxThreadsPerSubgroup;
  unsigned spiShaderIdsFormat = SPI_SHADER_1COMP;
  if (m_hasMesh) {
    maxVertsPerSubgroup = std::min(meshMode.outputVertices, NggMaxThreadsPerSubgroup);
    threadsPerSubgroup = calcFactor.primAmpFactor;
    const bool enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
    bool hasPrimitivePayload = meshBuiltInUsage.layer || meshBuiltInUsage.viewportIndex ||
                               meshBuiltInUsage.primitiveShadingRate || enableMultiView;
    if (m_gfxIp.major < 11)
      hasPrimitivePayload |= meshBuiltInUsage.primitiveId;
    spiShaderIdsFormat = hasPrimitivePayload ? SPI_SHADER_2COMP : SPI_SHADER_1COMP;

    // VGT_DRAW_PAYLOAD_CNTL
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtDrawPrimPayloadEn] = hasPrimitivePayload;

    // Pipeline metadata: mesh_linear_dispatch_from_task
    bool meshLinearDispatchFromTask = false;
    if (m_hasTask) {
      meshLinearDispatchFromTask =
          m_pipelineState->getShaderResourceUsage(ShaderStageTask)->builtInUsage.task.meshLinearDispatch;
    }
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::MeshLinearDispatchFromTask] =
        meshLinearDispatchFromTask;

    if (m_gfxIp.major >= 11) {
      // SPI_SHADER_GS_MESHLET_DIM
      auto spiShaderGsMeshletDim =
          getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderGsMeshletDim].getMap(true);
      spiShaderGsMeshletDim[Util::Abi::SpiShaderGsMeshletDimMetadataKey::NumThreadX] = meshMode.workgroupSizeX - 1;
      spiShaderGsMeshletDim[Util::Abi::SpiShaderGsMeshletDimMetadataKey::NumThreadY] = meshMode.workgroupSizeY - 1;
      spiShaderGsMeshletDim[Util::Abi::SpiShaderGsMeshletDimMetadataKey::NumThreadZ] = meshMode.workgroupSizeZ - 1;
      // NOTE: If row export for mesh shader is enabled, the thread group size is set according to dimensions of work
      // group. Otherwise, it is set according to actual primitive amplification factor.
      const unsigned threadGroupSize = m_pipelineState->enableMeshRowExport()
                                           ? meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ
                                           : calcFactor.primAmpFactor;
      spiShaderGsMeshletDim[Util::Abi::SpiShaderGsMeshletDimMetadataKey::ThreadgroupSize] = threadGroupSize;

      // SPI_SHADER_GS_MESHLET_EXP_ALLOC
      auto spiShaderGsMeshletExpAlloc =
          getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderGsMeshletExpAlloc].getMap(true);
      spiShaderGsMeshletExpAlloc[Util::Abi::SpiShaderGsMeshletExpAllocMetadataKey::MaxExpVerts] =
          meshMode.outputVertices;
      spiShaderGsMeshletExpAlloc[Util::Abi::SpiShaderGsMeshletExpAllocMetadataKey::MaxExpPrims] =
          meshMode.outputPrimitives;
    }
  } else {
    maxVertsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, NggMaxThreadsPerSubgroup);
    // VGT_GS_VERT_ITEMSIZE
    auto itemSizeArrayNode =
        getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsVertItemsize].getArray(true);
    itemSizeArrayNode[0] = 4 * gsInOutUsage.outputMapLocCount;
    itemSizeArrayNode[1] = itemSizeArrayNode[2] = itemSizeArrayNode[3] = 0;

    // VGT_GS_INSTANCE_CNT
    if (geometryMode.invocations > 1 || gsBuiltInUsage.invocationId) {
      auto vgtGsInstanceCnt =
          getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsInstanceCnt].getMap(true);
      vgtGsInstanceCnt[Util::Abi::VgtGsInstanceCntMetadataKey::Enable] = true;
      vgtGsInstanceCnt[Util::Abi::VgtGsInstanceCntMetadataKey::Count] = geometryMode.invocations;
      if (m_gfxIp >= GfxIpVersion{10, 1})
        vgtGsInstanceCnt[Util::Abi::VgtGsInstanceCntMetadataKey::EnMaxVertOutPerGsInstance] =
            calcFactor.enableMaxVertOut;
    }

    // VGT_GSVS_RING_ITEMSIZE
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsvsRingItemsize] = calcFactor.gsVsRingItemSize;

    // VGT_ESGS_RING_ITEMSIZE
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtEsgsRingItemsize] =
        (m_hasGs ? calcFactor.esGsRingItemSize : 1);

    const auto nggControl = m_pipelineState->getNggControl();
    assert(nggControl->enableNgg);
    if (!nggControl->passthroughMode) {
      // NOTE: For NGG culling mode, the primitive shader table that contains culling data might be accessed by
      // shader. PAL expects 64-bit address of that table and will program it into SPI_SHADER_PGM_LO_GS and
      // SPI_SHADER_PGM_HI_GS if we do not provide one. By setting SPI_SHADER_PGM_LO_GS to NggCullingData, we tell
      // PAL that we will not provide it and it is fine to use SPI_SHADER_PGM_LO_GS and SPI_SHADER_PGM_HI_GS as
      // the address of that table.
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::NggCullingDataReg] =
          static_cast<unsigned>(UserDataMapping::NggCullingData);
    }
  }

  // GE_MAX_OUTPUT_PER_SUBGROUP/VGT_GS_MAX_PRIMS_PER_SUBGROUP
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::MaxVertsPerSubgroup] = maxVertsPerSubgroup;

  // GE_NGG_SUBGRP_CNTL
  auto geNggSubgrpCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::GeNggSubgrpCntl].getMap(true);
  geNggSubgrpCntl[Util::Abi::GeNggSubgrpCntlMetadataKey::PrimAmpFactor] = calcFactor.primAmpFactor;
  geNggSubgrpCntl[Util::Abi::GeNggSubgrpCntlMetadataKey::ThreadsPerSubgroup] = threadsPerSubgroup;

  // TODO: Support PIPELINE_PRIM_ID.
  // SPI_SHADER_IDX_FORMAT
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderIdxFormat] = spiShaderIdsFormat;

  // Pipeline metadata
  setNggSubgroupSize(m_hasMesh ? 1 : std::max(calcFactor.esVertsPerSubgroup, calcFactor.gsPrimsPerSubgroup));

  //
  // Build SW stream-out configuration (GFX11+)
  //
  if (m_pipelineState->enableSwXfb()) {
    const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
    std::array<unsigned, MaxTransformFeedbackBuffers> xfbStridesInDwords;
    for (unsigned i = 0; i < xfbStridesInDwords.size(); ++i) {
      // Must be multiple of dword (PAL doesn't support 16-bit transform feedback outputs)
      assert(xfbStrides[i] % sizeof(unsigned) == 0);
      xfbStridesInDwords[i] = xfbStrides[i] / sizeof(unsigned);
    }
    setStreamOutVertexStrides(xfbStridesInDwords); // Set SW stream-out vertex strides
  }

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
#if VKI_RAY_TRACING
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;
#endif
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Gs);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = ldsSizeInDwords * 4;
  if (!m_hasMesh)
    setEsGsLdsSize(calcFactor.esGsLdsSize * 4);
}

// =====================================================================================================================
// Builds register configuration for hardware vertex shader.
void RegisterMetadataBuilder::buildHwVsRegisters() {
  assert(m_hasVs || m_hasTes || m_pipelineState->hasShaderStage(ShaderStageCopyShader));
  assert(m_gfxIp.major <= 10);
  ShaderStage shaderStage = ShaderStageVertex;
  if (m_pipelineState->hasShaderStage(ShaderStageCopyShader))
    shaderStage = ShaderStageCopyShader;
  else if (m_hasTes)
    shaderStage = ShaderStageTessEval;

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage;

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();
  const bool enableXfb = m_pipelineState->enableXfb();

  // VGT_STRMOUT_CONFIG
  auto vgtStrmoutConfig = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutConfig].getMap(true);
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_0En] = streamXfbBuffers[0] > 0;
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_1En] = streamXfbBuffers[1] > 0;
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_2En] = streamXfbBuffers[2] > 0;
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_3En] = streamXfbBuffers[3] > 0;
  if (shaderStage == ShaderStageCopyShader)
    vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::RastStream] = resUsage->inOutUsage.gs.rasterStream;

  // Set some field of SPI_SHADER_PGM_RSRC2_VS
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoEn] = enableXfb;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase0En] = xfbStrides[0] > 0;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase1En] = xfbStrides[1] > 0;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase2En] = xfbStrides[2] > 0;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase3En] = xfbStrides[3] > 0;

  // VGT_STRMOUT_VTX_STRIDE_*
  const unsigned sizeInByte = static_cast<unsigned>(sizeof(unsigned));
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutVtxStride0] = xfbStrides[0] / sizeInByte;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutVtxStride1] = xfbStrides[1] / sizeInByte;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutVtxStride2] = xfbStrides[2] / sizeInByte;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutVtxStride3] = xfbStrides[3] / sizeInByte;

  // VGT_STRMOUT_BUFFER_CONFIG
  auto vgtStrmoutBufferConfig =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutBufferConfig].getMap(true);
  vgtStrmoutBufferConfig[Util::Abi::VgtStrmoutBufferConfigMetadataKey::Stream_0BufferEn] = streamXfbBuffers[0];
  vgtStrmoutBufferConfig[Util::Abi::VgtStrmoutBufferConfigMetadataKey::Stream_1BufferEn] = streamXfbBuffers[1];
  vgtStrmoutBufferConfig[Util::Abi::VgtStrmoutBufferConfigMetadataKey::Stream_2BufferEn] = streamXfbBuffers[2];
  vgtStrmoutBufferConfig[Util::Abi::VgtStrmoutBufferConfigMetadataKey::Stream_3BufferEn] = streamXfbBuffers[3];

  // VGPR_COMP_CNT
  if (shaderStage == ShaderStageVertex) {
    if (builtInUsage.vs.instanceIndex)
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsVgprCompCnt] = 3; // 3: Enable instance ID
    else if (builtInUsage.vs.primitiveId)
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsVgprCompCnt] = 2;
  } else if (shaderStage == ShaderStageTessEval) {
    if (builtInUsage.tes.primitiveId)
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsVgprCompCnt] = 3; // 3: Enable primitive ID
    else
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsVgprCompCnt] = 2;
  }
}

// =====================================================================================================================
// Builds register configuration for hardware pixel shader.
void RegisterMetadataBuilder::buildPsRegisters() {
  ShaderStage shaderStage = ShaderStageFragment;
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto &fragmentMode = m_pipelineState->getShaderModes()->getFragmentShaderMode();
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.fs;

  // SPI_BARYC_CNTL
  auto spiBarycCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiBarycCntl].getMap(true);
  spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::FrontFaceAllBits] = true;
  if (fragmentMode.pixelCenterInteger) {
    // TRUE - Force floating point position to upper left corner of pixel (X.0, Y.0)
    spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::PosFloatUlc] = true;
  } else if (builtInUsage.runAtSampleRate) {
    // 2 - Calculate per-pixel floating point position at iterated sample number
    spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::PosFloatLocation] = 2;
  } else {
    // 0 - Calculate per-pixel floating point position at pixel center
    spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::PosFloatLocation] = 0;
  }

  // PA_SC_MODE_CNTL_1
  getGraphicsRegNode()[Util::Abi::PaScModeCntl1MetadataKey::PsIterSample] =
      m_pipelineState->getShaderResourceUsage(shaderStage)->builtInUsage.fs.runAtSampleRate > 0;

  // DB_SHADER_CONTROL
  ZOrder zOrder = LATE_Z;
  bool execOnHeirFail = false;
  if (shaderOptions.forceLateZ)
    zOrder = LATE_Z;
  else if (fragmentMode.earlyFragmentTests)
    zOrder = EARLY_Z_THEN_LATE_Z;
  else if (resUsage->resourceWrite) {
    zOrder = LATE_Z;
    execOnHeirFail = true;
  } else if (shaderOptions.allowReZ)
    zOrder = EARLY_Z_THEN_RE_Z;
  else
    zOrder = EARLY_Z_THEN_LATE_Z;

  ConservativeZExport conservativeZExport = EXPORT_ANY_Z;
  if (fragmentMode.conservativeDepth == ConservativeDepth::LessEqual)
    conservativeZExport = EXPORT_LESS_THAN_Z;
  else if (fragmentMode.conservativeDepth == ConservativeDepth::GreaterEqual)
    conservativeZExport = EXPORT_GREATER_THAN_Z;

  auto dbShaderControl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::DbShaderControl].getMap(true);
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ZOrder] = zOrder;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::KillEnable] = builtInUsage.discard;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ZExportEnable] = builtInUsage.fragDepth;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::StencilTestValExportEnable] = builtInUsage.fragStencilRef;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::MaskExportEnable] = builtInUsage.sampleMask;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::AlphaToMaskDisable] = 1; // Set during pipeline finalization.
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::DepthBeforeShader] = fragmentMode.earlyFragmentTests;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ExecOnNoop] =
      fragmentMode.earlyFragmentTests && resUsage->resourceWrite;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ExecOnHierFail] = execOnHeirFail;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ConservativeZExport] = conservativeZExport;
  if (m_gfxIp.major >= 10)
    dbShaderControl[Util::Abi::DbShaderControlMetadataKey::PreShaderDepthCoverageEnable] =
        fragmentMode.postDepthCoverage;

  // SPI_SHADER_Z_FORMAT
  unsigned depthExpFmt = EXP_FORMAT_ZERO;
  if (builtInUsage.sampleMask)
    depthExpFmt = EXP_FORMAT_32_ABGR;
  else if (builtInUsage.fragStencilRef)
    depthExpFmt = EXP_FORMAT_32_GR;
  else if (builtInUsage.fragDepth)
    depthExpFmt = EXP_FORMAT_32_R;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderZFormat] = depthExpFmt;

  // SPI_PS_INPUT_CNTL_0..31
  // NOTE: PAL expects at least one mmSPI_PS_INPUT_CNTL_0 register set, so we always patch it at least one if none
  // were identified in the shader.
  struct SpiPsInputCntlInfo {
    unsigned offset;
    unsigned attr0Valid;
    unsigned attr1Valid;
    bool flatShade;
    bool primAttr;
    bool fp16InterMode;
    bool ptSpriteTex;
  };
  constexpr unsigned UseDefaultVal = (1 << 5);
  constexpr unsigned PassThroughMode = (1 << 5);

  unsigned pointCoordLoc = InvalidValue;
  auto builtInInputLocMapIt = resUsage->inOutUsage.builtInInputLocMap.find(BuiltInPointCoord);
  if (builtInInputLocMapIt != resUsage->inOutUsage.builtInInputLocMap.end()) {
    // Get generic input corresponding to gl_PointCoord (to set the field PT_SPRITE_TEX)
    pointCoordLoc = builtInInputLocMapIt->second;
  }

  msgpack::ArrayDocNode spiPsInputCnt =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiPsInputCntl].getArray(true);
  const std::vector<FsInterpInfo> dummyInterpInfo{{0, false, false, false, false, false, false}};
  const auto &fsInterpInfo = resUsage->inOutUsage.fs.interpInfo;
  const auto *interpInfo = fsInterpInfo.size() == 0 ? &dummyInterpInfo : &fsInterpInfo;

  unsigned numPrimInterp = 0;
  for (unsigned i = 0; i < interpInfo->size(); ++i) {
    auto spiPsInputCntElem = spiPsInputCnt[i].getMap(true);
    auto interpInfoElem = (*interpInfo)[i];

    if (interpInfoElem.isPerPrimitive)
      ++numPrimInterp;

    if ((interpInfoElem.loc == InvalidFsInterpInfo.loc && interpInfoElem.flat == InvalidFsInterpInfo.flat &&
         interpInfoElem.custom == InvalidFsInterpInfo.custom && interpInfoElem.is16bit == InvalidFsInterpInfo.is16bit))
      interpInfoElem.loc = i;

    SpiPsInputCntlInfo spiPsInputCntlInfo = {};
    spiPsInputCntlInfo.offset = interpInfoElem.loc;
    spiPsInputCntlInfo.flatShade = interpInfoElem.flat && !interpInfoElem.isPerPrimitive;

    if (m_gfxIp.major >= 11 && interpInfoElem.isPerPrimitive) {
      const auto preStage = m_pipelineState->getPrevShaderStage(ShaderStageFragment);
      if (preStage == ShaderStageMesh) {
        // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and
        // PRIM_EXPORT_COUNT. When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even
        // though this is not what we want. Hence, we should reserve param0 as a dummy vertex attribute and all
        // primitive attributes are moved after it.
        bool hasNoVertexAttrib = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage.expCount == 0;
        if (hasNoVertexAttrib)
          ++spiPsInputCntlInfo.offset;
      }
      spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::PrimAttr] = true;
    }

    if (interpInfoElem.custom) {
      // NOTE: Force parameter cache data to be read in passthrough mode.
      spiPsInputCntlInfo.flatShade = true;
      spiPsInputCntlInfo.offset |= PassThroughMode;
    } else if (!interpInfoElem.flat && interpInfoElem.is16bit) {
      spiPsInputCntlInfo.fp16InterMode = true;
      spiPsInputCntlInfo.attr0Valid = interpInfoElem.attr0Valid;
      spiPsInputCntlInfo.attr1Valid = interpInfoElem.attr1Valid;
    }

    if (pointCoordLoc == i) {
      spiPsInputCntlInfo.ptSpriteTex = true;

      // NOTE: Set the offset value to force hardware to select input defaults (no VS match).
      spiPsInputCntlInfo.offset = UseDefaultVal;
    }

    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::FlatShade] = spiPsInputCntlInfo.flatShade;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Offset] = spiPsInputCntlInfo.offset;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Fp16InterpMode] = spiPsInputCntlInfo.fp16InterMode;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::PtSpriteTex] = spiPsInputCntlInfo.ptSpriteTex;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Attr0Valid] = spiPsInputCntlInfo.attr0Valid;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Attr1Valid] = spiPsInputCntlInfo.attr1Valid;
  }

  // SPI_PS_IN_CONTROL
  unsigned numInterp = resUsage->inOutUsage.fs.interpInfo.size() - numPrimInterp;
  if (m_gfxIp.major >= 11) {
    // NOTE: For GFX11+, vertex attributes and primitive attributes are counted together. The field
    // SPI_PS_INPUT_CNTL.PRIM_ATTR is used to differentiate them.
    numInterp = resUsage->inOutUsage.fs.interpInfo.size();
  }

  auto spiPsInControl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiPsInControl].getMap(true);
  spiPsInControl[Util::Abi::SpiPsInControlMetadataKey::NumInterps] = numInterp;
  if (m_gfxIp.isGfx(10, 3))
    spiPsInControl[Util::Abi::SpiPsInControlMetadataKey::NumPrimInterp] = numPrimInterp;
  auto waveFrontSize = m_pipelineState->getShaderWaveSize(shaderStage);
  if (waveFrontSize == 32)
    spiPsInControl[Util::Abi::SpiPsInControlMetadataKey::PsW32En] = true;

  // SPI_INTERP_CONTROL_0
  if (pointCoordLoc != InvalidValue) {
    auto spiInterpControl0 =
        getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiInterpControl].getMap(true);
    spiInterpControl0[Util::Abi::SpiInterpControlMetadataKey::PointSpriteEna] = true;
    spiInterpControl0[Util::Abi::SpiInterpControlMetadataKey::PointSpriteOverrideX] =
        m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::PointSpriteSelect(SPI_PNT_SPRITE_SEL_S));
    spiInterpControl0[Util::Abi::SpiInterpControlMetadataKey::PointSpriteOverrideY] =
        m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::PointSpriteSelect(SPI_PNT_SPRITE_SEL_T));
    spiInterpControl0[Util::Abi::SpiInterpControlMetadataKey::PointSpriteOverrideZ] =
        m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::PointSpriteSelect(SPI_PNT_SPRITE_SEL_0));
    spiInterpControl0[Util::Abi::SpiInterpControlMetadataKey::PointSpriteOverrideW] =
        m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::PointSpriteSelect(SPI_PNT_SPRITE_SEL_1));
  }

  setPsSampleMask(builtInUsage.sampleMaskIn | builtInUsage.sampleMask);
  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Ps);
  if (m_pipelineState->getPalAbiVersion() >= 456) {
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::UsesUavs] = resUsage->resourceRead || resUsage->resourceWrite;
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::WritesUavs] = resUsage->resourceWrite;
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::WritesDepth] = builtInUsage.fragDepth;
  } else {
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::UsesUavs] = resUsage->resourceWrite;
  }
}

// =====================================================================================================================
// Builds register configuration for compute/task shader.
void RegisterMetadataBuilder::buildCsRegisters(ShaderStage shaderStage) {
  assert(shaderStage == ShaderStageCompute || shaderStage == ShaderStageTask);

  getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidXEn] = true;
  getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidYEn] = true;
  getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidZEn] = true;
  getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgSizeEn] = true;

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &computeMode = m_pipelineState->getShaderModes()->getComputeShaderMode();

  unsigned workgroupSizes[3] = {};
  if (shaderStage == ShaderStageCompute) {
    const auto &builtInUsage = resUsage->builtInUsage.cs;
    if (builtInUsage.foldWorkgroupXY) {
      workgroupSizes[0] = computeMode.workgroupSizeX * computeMode.workgroupSizeY;
      workgroupSizes[1] = computeMode.workgroupSizeZ;
      workgroupSizes[2] = 1;
    } else {
      workgroupSizes[0] = computeMode.workgroupSizeX;
      workgroupSizes[1] = computeMode.workgroupSizeY;
      workgroupSizes[2] = computeMode.workgroupSizeZ;
    }
  } else {
    assert(shaderStage == ShaderStageTask);
    workgroupSizes[0] = computeMode.workgroupSizeX;
    workgroupSizes[1] = computeMode.workgroupSizeY;
    workgroupSizes[2] = computeMode.workgroupSizeZ;
  }

  // 0 = X, 1 = XY, 2 = XYZ
  unsigned tidigCompCnt = 0;
  if (workgroupSizes[2] > 1)
    tidigCompCnt = 2;
  else if (workgroupSizes[1] > 1)
    tidigCompCnt = 1;
  getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TidigCompCnt] = tidigCompCnt;

  setThreadgroupDimensions(workgroupSizes);
}

// =====================================================================================================================
// Build registers fields related to shader execution.
//
// @param hwStageId: The hardware shader stage
// @param apiStage1: The first api shader stage
// @param apiStage2: The second api shader stage
void RegisterMetadataBuilder::buildShaderExecutionRegisters(Util::Abi::HardwareStage hwStageId, ShaderStage apiStage1,
                                                            ShaderStage apiStage2) {
  // Set hardware stage metadata
  auto hwShaderNode = getHwShaderNode(hwStageId);
  ShaderStage apiStage = apiStage2 != ShaderStageInvalid ? apiStage2 : apiStage1;

  if (m_isNggMode || m_gfxIp.major >= 10) {
    unsigned waveFrontSize = m_pipelineState->getShaderWaveSize(apiStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::WavefrontSize] = waveFrontSize;
  }

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
    unsigned checksum = setShaderHash(apiStage1);
    if (apiStage2 != ShaderStageInvalid)
      checksum ^= setShaderHash(apiStage2);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::ChecksumValue] = checksum;
  }

  hwShaderNode[Util::Abi::HardwareStageMetadataKey::FloatMode] = setupFloatingPointMode(apiStage);

  unsigned userDataCount = 0;
  unsigned sgprLimits = 0;
  unsigned vgprLimits = 0;
  if (apiStage1 == ShaderStageCopyShader) {
    // NOTE: For copy shader, usually we use fixed number of user data registers.
    // But in some cases, we may change user data registers, we use variable to keep user sgpr count here
    userDataCount = lgc::CopyShaderUserSgprCount;
#if VKI_BUILD_SHADER_DBG
    if (m_pipelineState->getOptions().shaderTraceMask != 0) {
      std::vector<unsigned> userDataMap(32, static_cast<unsigned>(UserDataMapping::Invalid));
      userDataMap[userDataCount] = UserDataMapping::ShaderDbgAddr;
      m_pipelineState->setUserDataMap(m_shaderStage, userDataMap);

      // 2 SGPRs holding the GPU VA address of the buffer for shader debugging
      userDataCount += 2;
    }
#endif
    sgprLimits = m_pipelineState->getTargetInfo().getGpuProperty().maxSgprsAvailable;
    vgprLimits = m_pipelineState->getTargetInfo().getGpuProperty().maxVgprsAvailable;
  } else {
    userDataCount = m_pipelineState->getShaderInterfaceData(apiStage1)->userDataCount;
    if (apiStage2 != ShaderStageInvalid) {
      userDataCount = std::max(userDataCount, m_pipelineState->getShaderInterfaceData(apiStage2)->userDataCount);
    }

    sgprLimits = m_pipelineState->getShaderResourceUsage(apiStage)->numSgprsAvailable;
    vgprLimits = m_pipelineState->getShaderResourceUsage(apiStage)->numVgprsAvailable;

    const auto &shaderOptions = m_pipelineState->getShaderOptions(apiStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::DebugMode] = shaderOptions.debugMode;
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::TrapPresent] = shaderOptions.trapPresent;
  }
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::UserSgprs] = userDataCount;

  if (m_gfxIp.major >= 10) {
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::MemOrdered] = true;
    if (hwStageId == Util::Abi::HardwareStage::Hs || hwStageId == Util::Abi::HardwareStage::Gs) {
      bool wgpMode = m_pipelineState->getShaderWgpMode(apiStage1);
      if (apiStage2 != ShaderStageInvalid)
        wgpMode = wgpMode || m_pipelineState->getShaderWgpMode(apiStage2);
      hwShaderNode[Util::Abi::HardwareStageMetadataKey::WgpMode] = wgpMode;
    }
  }

  if (apiStage1 == ShaderStageTessEval && m_pipelineState->isTessOffChip())
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::OffchipLdsEn] = true;

  hwShaderNode[Util::Abi::HardwareStageMetadataKey::SgprLimit] = sgprLimits;
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::VgprLimit] = vgprLimits;

  if (m_gfxIp.major >= 11 && hwStageId != Util::Abi::HardwareStage::Vs) {
    bool useImageOp = m_pipelineState->getShaderResourceUsage(apiStage1)->useImageOp;
    if (apiStage2 != ShaderStageInvalid)
      useImageOp |= m_pipelineState->getShaderResourceUsage(apiStage2)->useImageOp;
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::ImageOp] = useImageOp;
  }

  auto userDataNode = hwShaderNode[Util::Abi::HardwareStageMetadataKey::UserDataRegMap].getArray(true);
  unsigned idx = 0;
  for (auto value : m_pipelineState->getUserDataMap(apiStage))
    userDataNode[idx++] = value;
}

// =====================================================================================================================
// Build PA-specific (primitive assembler) registers.
void RegisterMetadataBuilder::buildPaSpecificRegisters() {
  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool meshPipeline =
      m_pipelineState->hasShaderStage(ShaderStageTask) || m_pipelineState->hasShaderStage(ShaderStageMesh);

  // VGT_PRIMITIVEID_EN
  // Stage-specific processing
  bool usePointSize = false;
  bool useLayer = false;
  bool useViewportIndex = false;
  bool useShadingRate = false;
  unsigned clipDistanceCount = 0;
  unsigned cullDistanceCount = 0;

  unsigned expCount = 0;
  unsigned primExpCount = 0;

  if (meshPipeline) {
    // Mesh pipeline
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageMesh);
    const auto &builtInUsage = resUsage->builtInUsage.mesh;

    usePointSize = builtInUsage.pointSize;
    useLayer = builtInUsage.layer;
    useViewportIndex = builtInUsage.viewportIndex;
    useShadingRate = builtInUsage.primitiveShadingRate;
    clipDistanceCount = builtInUsage.clipDistance;
    cullDistanceCount = builtInUsage.cullDistance;

    expCount = resUsage->inOutUsage.expCount;
    primExpCount = resUsage->inOutUsage.primExpCount;
  } else {
    bool usePrimitiveId = false;

    if (m_hasGs) {
      const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
      const auto &builtInUsage = resUsage->builtInUsage.gs;

      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveIdIn;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;

      expCount = resUsage->inOutUsage.expCount;

      // NOTE: For ES-GS merged shader, the actual use of primitive ID should take both ES and GS into consideration.
      if (hasTs) {
        const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
        usePrimitiveId = usePrimitiveId || tesBuiltInUsage.primitiveId;
      } else {
        const auto &vsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
        usePrimitiveId = usePrimitiveId || vsBuiltInUsage.primitiveId;
      }
    } else if (hasTs) {
      const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
      const auto &builtInUsage = resUsage->builtInUsage.tes;

      usePointSize = builtInUsage.pointSize;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;

      expCount = resUsage->inOutUsage.expCount;
    } else {
      const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
      const auto &builtInUsage = resUsage->builtInUsage.vs;

      usePointSize = builtInUsage.pointSize;
      usePrimitiveId = builtInUsage.primitiveId;
      useLayer = builtInUsage.layer;
      useViewportIndex = builtInUsage.viewportIndex;
      useShadingRate = builtInUsage.primitiveShadingRate;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;

      expCount = resUsage->inOutUsage.expCount;
    }

    useLayer = useLayer || m_pipelineState->getInputAssemblyState().enableMultiView;

    if (usePrimitiveId) {
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtPrimitiveIdEn] = true;
      if (m_pipelineState->getNggControl()->enableNgg) {
        // NOTE: If primitive ID is used and there is no GS present, the field NGG_DISABLE_PROVOK_REUSE must be
        // set to ensure provoking vertex reuse is disabled in the GE.
        if (!m_hasGs)
          getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::NggDisableProvokReuse] = true;
      }
    }
  }

  // SPI_VS_OUT_CONFIG
  auto spiVsOutConfig = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiVsOutConfig].getMap(true);
  if (expCount == 0 && primExpCount == 0) {
    // No generic output is present
    spiVsOutConfig[Util::Abi::SpiVsOutConfigMetadataKey::NoPcExport] = true;
  } else {
    if (expCount > 0)
      spiVsOutConfig[Util::Abi::SpiVsOutConfigMetadataKey::VsExportCount] = expCount - 1;

    if (primExpCount > 0) {
      assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
      spiVsOutConfig[Util::Abi::SpiVsOutConfigMetadataKey::PrimExportCount] = primExpCount;
    }
  }

  // VGT_REUSE_OFF
  bool disableVertexReuse = m_pipelineState->getInputAssemblyState().disableVertexReuse;
  disableVertexReuse |= meshPipeline; // Mesh pipeline always disable vertex reuse
  if (useViewportIndex)
    disableVertexReuse = true;

  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtReuseOff] =
      disableVertexReuse || m_pipelineState->enableSwXfb();

  // PA_CL_CLIP_CNTL
  msgpack::MapDocNode paClClipCntl =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaClClipCntl].getMap(true);
  const unsigned usrClipPlaneMask = m_pipelineState->getRasterizerState().usrClipPlaneMask;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::UserClipPlane0Ena] = (usrClipPlaneMask & 0x1) > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::UserClipPlane1Ena] = ((usrClipPlaneMask >> 1) & 0x1) > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::UserClipPlane2Ena] = ((usrClipPlaneMask >> 2) & 0x1) > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::UserClipPlane3Ena] = ((usrClipPlaneMask >> 3) & 0x1) > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::UserClipPlane4Ena] = ((usrClipPlaneMask >> 4) & 0x1) > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::UserClipPlane5Ena] = ((usrClipPlaneMask >> 5) & 0x1) > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::DxLinearAttrClipEna] = true;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::RasterizationKill] =
      m_pipelineState->getRasterizerState().rasterizerDiscardEnable > 0;
  paClClipCntl[Util::Abi::PaClClipCntlMetadataKey::VteVportProvokeDisable] = useViewportIndex;

  // PA_CL_VS_OUT_CNTL
  bool miscExport = usePointSize;
  if (!meshPipeline) {
    // NOTE: Those built-ins are exported through primitive payload for mesh pipeline rather than vertex position data.
    miscExport |= useLayer || useViewportIndex || useShadingRate;
  }

  auto paClVsOutCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaClVsOutCntl].getMap(true);
  if (miscExport) {
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::UseVtxPointSize] = usePointSize;

    if (meshPipeline) {
      if (useShadingRate) {
        assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
        paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::BypassVtxRateCombiner] = true;
      }
    } else {
      // NOTE: Those built-ins are exported through primitive payload for mesh pipeline rather than vertex position
      // data.
      paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::UseVtxRenderTargetIndx] = useLayer;
      paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::UseVtxViewportIndx] = useViewportIndex;

      if (useShadingRate) {
        assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
        paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::UseVtxVrsRate] = true;
        paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::BypassPrimRateCombiner] = true;
      }
    }
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutMiscVecEna] = true;
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutMiscSideBusEna] = true;
  }

  if (clipDistanceCount > 0 || cullDistanceCount > 0) {
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutCcDist0VecEna] = true;

    if (clipDistanceCount + cullDistanceCount > 4)
      paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutCcDist1VecEna] = true;

    unsigned clipDistanceMask = (1 << clipDistanceCount) - 1;
    unsigned cullDistanceMask = (1 << cullDistanceCount) - 1;

    // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
    static const unsigned MaxDistCount = 8;
    bool clipDistEna[MaxDistCount] = {};
    bool cullDistEna[MaxDistCount] = {};
    for (unsigned i = 0; i < MaxDistCount; ++i) {
      clipDistEna[i] = (clipDistanceMask >> i) & 0x1;
      cullDistEna[i] = (cullDistanceMask >> i) & 0x1;
    }
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_0] = clipDistEna[0];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_1] = clipDistEna[1];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_2] = clipDistEna[2];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_3] = clipDistEna[3];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_4] = clipDistEna[4];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_5] = clipDistEna[5];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_6] = clipDistEna[6];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::ClipDistEna_7] = clipDistEna[7];

    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_0] = cullDistEna[0];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_1] = cullDistEna[1];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_2] = cullDistEna[2];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_3] = cullDistEna[3];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_4] = cullDistEna[4];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_5] = cullDistEna[5];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_6] = cullDistEna[6];
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::CullDistEna_7] = cullDistEna[7];

    // On 10.3+ all auxiliary position exports are optimized, not just the misc exports.
    if (m_gfxIp >= GfxIpVersion{10, 3})
      paClClipCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutMiscSideBusEna] = true;
  }

  // PA_CL_VTE_CNTL
  msgpack::MapDocNode paClVteCntl =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaClVteCntl].getMap(true);
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::XScaleEna] = true;
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::XOffsetEna] = true;
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::YScaleEna] = true;
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::YOffsetEna] = true;
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::ZScaleEna] = true;
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::ZOffsetEna] = true;
  paClVteCntl[Util::Abi::PaClVteCntlMetadataKey::VtxW0Fmt] = true;

  // PA_SU_VTX_CNTL
  msgpack::MapDocNode paSuVtxCntl =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaSuVtxCntl].getMap(true);
  paSuVtxCntl[Util::Abi::PaSuVtxCntlMetadataKey::PixCenter] = 1;
  paSuVtxCntl[Util::Abi::PaSuVtxCntlMetadataKey::RoundMode] = 2; // Round to even
  paSuVtxCntl[Util::Abi::PaSuVtxCntlMetadataKey::QuantMode] = 5; // Use 8-bit fractions

  // SPI_SHADER_POS_FORMAT
  unsigned availPosCount = 1; // gl_Position is always exported
  unsigned posCount = m_gfxIp.major >= 10 ? 5 : 4;
  if (miscExport)
    ++availPosCount;

  if (clipDistanceCount + cullDistanceCount > 0) {
    ++availPosCount;
    if (clipDistanceCount + cullDistanceCount > 4)
      ++availPosCount;
  }
  auto arrayNode = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiShaderPosFormat].getArray(true);
  for (unsigned i = 0; i < availPosCount; ++i)
    arrayNode[i] = SPI_SHADER_4COMP;
  for (unsigned i = availPosCount; i < posCount; ++i)
    arrayNode[i] = 0;

  // Pipeline metadata
  setUsesViewportArrayIndex(useViewportIndex);
}

// =====================================================================================================================
// Set up the metadata for register VGT_SHADER_STAGES_EN.
//
// @param hwStageMask : Mask of the hardware shader stage
void RegisterMetadataBuilder::setVgtShaderStagesEn(unsigned hwStageMask) {
  if (m_hasTask)
    return;

  auto vgtShaderStagesEn = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtShaderStagesEn].getMap(true);
  vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::MaxPrimgroupInWave] = 2;

  auto nggControl = m_pipelineState->getNggControl();

  if (m_isNggMode || m_hasMesh) {
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenEn] = true;
    // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
    // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
    // hardware pass-through).
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenPassthruEn] =
        m_hasGs ? false : nggControl->passthroughMode;
    unsigned fastLaunch = 0x1;

    if (m_gfxIp.major >= 11) {
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::NggWaveIdEn] = m_pipelineState->enableSwXfb();
      if (!m_hasGs)
        vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenPassthruNoMsg] =
            nggControl->passthroughMode && !m_pipelineState->enableSwXfb();
      fastLaunch = 0x2;
    }

    if (m_hasMesh)
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::GsFastLaunch] = fastLaunch;
  } else if (m_hasTcs || m_hasTes) {
    //# NOTE: From:  //gfxip/gfx10/doc/blocks/ge/Combined_Geometry_Engine_MAS.docx
    //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
    //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::DynamicHs] = true;
  }

  if (hwStageMask & Util::Abi::HwShaderVs) {
    ShaderStage apiStage = ShaderStageVertex;
    unsigned vsStageEnVal = VS_STAGE_REAL;
    if (m_pipelineState->hasShaderStage(ShaderStageCopyShader)) {
      apiStage = ShaderStageCopyShader;
      vsStageEnVal = VS_STAGE_COPY_SHADER;
    } else if (m_hasTes) {
      apiStage = ShaderStageTessEval;
      vsStageEnVal = VS_STAGE_DS;
    }
    auto waveFrontSize = m_pipelineState->getShaderWaveSize(apiStage);
    if (waveFrontSize == 32)
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::VsW32En] = true;

    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::VsStageEn] = vsStageEnVal;
  }
  if (hwStageMask & Util::Abi::HwShaderGs) {
    unsigned esEnVal = ES_STAGE_REAL;
    ShaderStage apiStage = ShaderStageVertex;
    if (m_hasGs || m_hasMesh) {
      apiStage = m_hasGs ? ShaderStageGeometry : ShaderStageMesh;
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::GsStageEn] = GS_STAGE_ON;
    } else if (m_hasTes) {
      apiStage = ShaderStageTessEval;
      esEnVal = ES_STAGE_DS;
    }
    auto waveFrontSize = m_pipelineState->getShaderWaveSize(apiStage);
    if (waveFrontSize == 32)
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::GsW32En] = true;

    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::EsStageEn] = esEnVal;
    if (m_isNggMode && !m_hasMesh)
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::VsStageEn] = VS_STAGE_REAL;
  }
  if (hwStageMask & Util::Abi::HwShaderHs) {
    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    if (waveFrontSize == 32)
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::HsW32En] = true;
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::LsStageEn] = LS_STAGE_ON;
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::HsStageEn] = HS_STAGE_ON;
  }
}

// =====================================================================================================================
// Set up the metadata for register IA_MULT_VGT_PARAM
void RegisterMetadataBuilder::setIaMultVgtParam() {
  bool isIaMultVgtParamPiped = m_isNggMode || (m_gfxIp.major == 10 && !m_isNggMode);

  if (m_hasTcs || m_hasTes) {
    // With tessellation, SWITCH_ON_EOI and PARTIAL_ES_WAVE_ON must be set if primitive ID is used by either the TCS,
    // TES, or GS.
    const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    bool usePrimitiveId = tcsBuiltInUsage.primitiveId;
    bool needWaveOnField = false;
    if (m_hasTes && !m_isNggMode) {
      const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
      usePrimitiveId = tesBuiltInUsage.primitiveId;
      needWaveOnField = true;
    }
    if (m_hasGs) {
      const auto &gsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;
      usePrimitiveId = gsBuiltInUsage.primitiveId;
    }

    if (isIaMultVgtParamPiped) {
      auto iaMultVgtParamPiped =
          getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::IaMultiVgtParamPiped].getMap(true);
      iaMultVgtParamPiped[Util::Abi::IaMultiVgtParamPipedMetadataKey::SwitchOnEoi] = usePrimitiveId;
      if (needWaveOnField)
        iaMultVgtParamPiped[Util::Abi::IaMultiVgtParamPipedMetadataKey::PartialEsWaveOn] = usePrimitiveId;
    } else {
      auto iaMultVgtParam = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::IaMultiVgtParam].getMap(true);
      iaMultVgtParam[Util::Abi::IaMultiVgtParamMetadataKey::SwitchOnEoi] = usePrimitiveId;
      if (needWaveOnField)
        iaMultVgtParam[Util::Abi::IaMultiVgtParamMetadataKey::PrimgroupSize] = usePrimitiveId;
    }

  } else {
    unsigned primGroupSize = 128;
    if (!m_hasGs && !m_hasMesh) {
      // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
      // even if there are more than 2 shader engines on the GPU.
      unsigned numShaderEngines = m_pipelineState->getTargetInfo().getGpuProperty().numShaderEngines;
      if (numShaderEngines > 2)
        primGroupSize = alignTo(primGroupSize, 2);
    }

    if (isIaMultVgtParamPiped || m_hasMesh) {
      auto iaMultVgtParamPiped =
          getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::IaMultiVgtParamPiped].getMap(true);
      iaMultVgtParamPiped[Util::Abi::IaMultiVgtParamPipedMetadataKey::PrimgroupSize] = primGroupSize - 1;
    } else {
      auto iaMultVgtParam = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::IaMultiVgtParam].getMap(true);
      iaMultVgtParam[Util::Abi::IaMultiVgtParamMetadataKey::PrimgroupSize] = primGroupSize - 1;
    }
  }
}

// =====================================================================================================================
// Set up the metadata for register VGT_FT_PARAM
void RegisterMetadataBuilder::setVgtTfParam() {
  unsigned primType = InvalidValue;
  unsigned partition = InvalidValue;
  unsigned topology = InvalidValue;

  const auto &tessMode = m_pipelineState->getShaderModes()->getTessellationMode();

  assert(tessMode.primitiveMode != PrimitiveMode::Unknown);
  if (tessMode.primitiveMode == PrimitiveMode::Isolines)
    primType = TESS_ISOLINE;
  else if (tessMode.primitiveMode == PrimitiveMode::Triangles)
    primType = TESS_TRIANGLE;
  else if (tessMode.primitiveMode == PrimitiveMode::Quads)
    primType = TESS_QUAD;
  assert(primType != InvalidValue);

  assert(tessMode.vertexSpacing != VertexSpacing::Unknown);
  if (tessMode.vertexSpacing == VertexSpacing::Equal)
    partition = PART_INTEGER;
  else if (tessMode.vertexSpacing == VertexSpacing::FractionalOdd)
    partition = PART_FRAC_ODD;
  else if (tessMode.vertexSpacing == VertexSpacing::FractionalEven)
    partition = PART_FRAC_EVEN;
  assert(partition != InvalidValue);

  assert(tessMode.vertexOrder != VertexOrder::Unknown);
  if (tessMode.pointMode)
    topology = OUTPUT_POINT;
  else if (tessMode.primitiveMode == PrimitiveMode::Isolines)
    topology = OUTPUT_LINE;
  else if (tessMode.vertexOrder == VertexOrder::Cw)
    topology = OUTPUT_TRIANGLE_CW;
  else if (tessMode.vertexOrder == VertexOrder::Ccw)
    topology = OUTPUT_TRIANGLE_CCW;

  if (m_pipelineState->getInputAssemblyState().switchWinding) {
    if (topology == OUTPUT_TRIANGLE_CW)
      topology = OUTPUT_TRIANGLE_CCW;
    else if (topology == OUTPUT_TRIANGLE_CCW)
      topology = OUTPUT_TRIANGLE_CW;
  }

  assert(topology != InvalidValue);

  auto vgtTfParam = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtTfParam].getMap(true);
  vgtTfParam[Util::Abi::VgtTfParamMetadataKey::Type] = primType;
  vgtTfParam[Util::Abi::VgtTfParamMetadataKey::Partitioning] = partition;
  vgtTfParam[Util::Abi::VgtTfParamMetadataKey::Topology] = topology;
  if (m_pipelineState->isTessOffChip())
    vgtTfParam[Util::Abi::VgtTfParamMetadataKey::DistributionMode] = TRAPEZOIDS;
}

} // namespace Gfx9

} // namespace lgc
