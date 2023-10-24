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
    auto lastVertexProcessingStage = m_pipelineState->getLastVertexProcessingStage();

    DenseMap<unsigned, unsigned> apiHwShaderMap;
    if (m_hasTask || m_hasMesh) {
      assert(m_pipelineState->getTargetInfo().getGfxIpVersion() >= GfxIpVersion({10, 3}));
      if (m_hasMesh) {
        apiHwShaderMap[ShaderStageMesh] = Util::Abi::HwShaderGs;
        pipelineType = Util::Abi::PipelineType::Mesh;
      }
      if (m_hasTask) {
        apiHwShaderMap[ShaderStageTask] = Util::Abi::HwShaderCs;
        pipelineType = Util::Abi::PipelineType::TaskMesh;
      }
    } else {
      if (m_hasGs) {
        auto preGsStage = m_pipelineState->getPrevShaderStage(ShaderStageGeometry);
        if (preGsStage != ShaderStageInvalid)
          apiHwShaderMap[preGsStage] = Util::Abi::HwShaderGs;
      }
      if (m_hasTcs) {
        apiHwShaderMap[ShaderStageTessControl] = Util::Abi::HwShaderHs;
        if (m_hasVs)
          apiHwShaderMap[ShaderStageVertex] = Util::Abi::HwShaderHs;
      }

      if (lastVertexProcessingStage != ShaderStageInvalid) {
        if (lastVertexProcessingStage == ShaderStageCopyShader)
          lastVertexProcessingStage = ShaderStageGeometry;
        if (m_isNggMode) {
          apiHwShaderMap[lastVertexProcessingStage] = Util::Abi::HwShaderGs;
          pipelineType = hasTs ? Util::Abi::PipelineType::NggTess : Util::Abi::PipelineType::Ngg;
        } else {
          apiHwShaderMap[lastVertexProcessingStage] = Util::Abi::HwShaderVs;
          if (m_hasGs)
            apiHwShaderMap[lastVertexProcessingStage] |= Util::Abi::HwShaderGs;

          if (hasTs && m_hasGs)
            pipelineType = Util::Abi::PipelineType::GsTess;
          else if (hasTs)
            pipelineType = Util::Abi::PipelineType::Tess;
          else if (m_hasGs)
            pipelineType = Util::Abi::PipelineType::Gs;
          else
            pipelineType = Util::Abi::PipelineType::VsPs;
        }
      }
    }
    if (m_pipelineState->hasShaderStage(ShaderStageFragment))
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
      if (m_isNggMode || m_hasMesh)
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
    if (!m_isNggMode && (hwStageMask & Util::Abi::HwShaderVs)) {
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

    // Set other registers if it is not a single PS or CS
    if (hwStageMask & (Util::Abi::HwShaderHs | Util::Abi::HwShaderGs | Util::Abi::HwShaderVs)) {
      setVgtShaderStagesEn(hwStageMask);
      setIaMultVgtParam();
      setPipelineType(pipelineType);
    }

    if (hwStageMask & (Util::Abi::HwShaderGs | Util::Abi::HwShaderVs))
      buildPaSpecificRegisters();

    if (lastVertexProcessingStage != ShaderStageInvalid && m_pipelineState->isUnlinked()) {
      // Fill ".preraster_output_semantic"
      auto resUsage = m_pipelineState->getShaderResourceUsage(lastVertexProcessingStage);
      auto &outputLocInfoMap = resUsage->inOutUsage.outputLocInfoMap;
      auto &builtInOutputLocMap = resUsage->inOutUsage.builtInOutputLocMap;
      // Collect semantic info for generic input and builtIns {gl_ClipDistance, gl_CulDistance, gl_Layer,
      // gl_ViewportIndex} that exports via generic output as well.
      if (!outputLocInfoMap.empty() || !builtInOutputLocMap.empty()) {
        auto preRasterOutputSemanticNode =
            getPipelineNode()[Util::Abi::PipelineMetadataKey::PrerasterOutputSemantic].getArray(true);
        unsigned elemIdx = 0;
        for (auto locInfoPair : outputLocInfoMap) {
          auto preRasterOutputSemanticElem = preRasterOutputSemanticNode[elemIdx].getMap(true);
          preRasterOutputSemanticElem[Util::Abi::PrerasterOutputSemanticMetadataKey::Semantic] =
              MaxBuiltInSemantic + locInfoPair.first.getLocation();
          preRasterOutputSemanticElem[Util::Abi::PrerasterOutputSemanticMetadataKey::Index] =
              locInfoPair.second.getLocation();
          ++elemIdx;
        }

        for (auto locPair : builtInOutputLocMap) {
          if (locPair.first == BuiltInClipDistance || locPair.first == BuiltInCullDistance ||
              locPair.first == BuiltInLayer || locPair.first == BuiltInViewportIndex) {
            assert(locPair.first < MaxBuiltInSemantic);
            auto preRasterOutputSemanticElem = preRasterOutputSemanticNode[elemIdx].getMap(true);
            preRasterOutputSemanticElem[Util::Abi::PrerasterOutputSemanticMetadataKey::Semantic] = locPair.first;
            preRasterOutputSemanticElem[Util::Abi::PrerasterOutputSemanticMetadataKey::Index] = locPair.second;
            ++elemIdx;
          }
        }
      }
    }

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
  constexpr unsigned minTessFactor = 1;
  constexpr unsigned maxTessFactor = 64;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtHosMinTessLevel] = minTessFactor;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtHosMaxTessLevel] = maxTessFactor;

  // VGT_LS_HS_CONFIG
  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  auto vgtLsHsConfig = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtLsHsConfig].getMap(true);
  vgtLsHsConfig[Util::Abi::VgtLsHsConfigMetadataKey::NumPatches] = calcFactor.patchCountPerThreadGroup;
  vgtLsHsConfig[Util::Abi::VgtLsHsConfigMetadataKey::HsNumInputCp] = m_pipelineState->getNumPatchControlPoints();
  vgtLsHsConfig[Util::Abi::VgtLsHsConfigMetadataKey::HsNumOutputCp] =
      m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;

  // VGT_TF_PARAM
  setVgtTfParam();

  // LS_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC1_HS
  const auto &vsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
  unsigned lsVgprCompCnt = 0;
  if (m_gfxIp.major <= 11) {
    if (vsBuiltInUsage.instanceIndex)
      lsVgprCompCnt = 3; // Enable all LS VGPRs (LS VGPR2 - VGPR5)
    else
      lsVgprCompCnt = 1; // Must enable relative vertex ID (LS VGPR2 and VGPR3)
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::LsVgprCompCnt] = lsVgprCompCnt;

  // Set LDS_SIZE of SPI_SHADER_PGM_RSRC2_HS
  assert(m_pipelineState->isTessOffChip()); // Must be off-chip on GFX9+
  unsigned ldsSizeInDwords = calcFactor.tessOnChipLdsSize;
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;

  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Hs);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = calcLdsSize(ldsSizeInDwords);

  if (m_gfxIp.major == 10 && !m_hasGs && !m_isNggMode) {
    auto vgtGsOnChipCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsOnchipCntl].getMap(true);
    vgtGsOnChipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::EsVertsPerSubgroup] = EsVertsOffchipGsOrTess;
    vgtGsOnChipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::GsPrimsPerSubgroup] = GsPrimsOffchipGsOrTess;
    vgtGsOnChipCntl[Util::Abi::VgtGsOnchipCntlMetadataKey::GsInstPrimsPerSubgrp] = GsPrimsOffchipGsOrTess;
  }
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
  const bool hasTs = m_hasTcs || m_hasTes;

  // ES_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC2_GS
  unsigned gsVgprCompCnt = 0;
  if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
    gsVgprCompCnt = 3; // Enable vtx4/vtx5 offset (GS VGPR3) or GS instance ID (GS VGPR4)
  else if (gsBuiltInUsage.primitiveIdIn)
    gsVgprCompCnt = 2; // Enable primitive ID (GS VGPR2)
  else if (calcFactor.inputVertices > 2)
    gsVgprCompCnt = 1; // Enable vtx2/vtx3 offset (GS VGPR1)
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::GsVgprCompCnt] = gsVgprCompCnt;

  // ES_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC2_GS
  unsigned esVgprCompCnt = 0;
  if (hasTs) {
    if (tesBuiltInUsage.primitiveId)
      esVgprCompCnt = 3; // Enable patch ID (ES VGPR8)
    else
      esVgprCompCnt = 2; // Must enable relative patch ID (ES VGPR7)
  } else {
    if (vsBuiltInUsage.instanceIndex)
      esVgprCompCnt = 3; // Enable instance ID (ES VGPR8)
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::EsVgprCompCnt] = esVgprCompCnt;

  getHwShaderNode(Util::Abi::HardwareStage::Gs)[Util::Abi::HardwareStageMetadataKey::OffchipLdsEn] = hasTs;

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
    itemSizeArrayNode[i] = itemSize;
    if (i < itemCount - 1) {
      gsVsRingOffset += itemSize * maxVertOut;
      ringOffsetArrayNode[i] = gsVsRingOffset;
    }
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
  StringRef primTyStr =
      m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::GsOutPrimType(gsOutputPrimitiveType));
  vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType] = primTyStr;

  // Set multi-stream output primitive type
  if (itemSizeArrayNode[1].getUInt() > 0 || itemSizeArrayNode[2].getUInt() > 0 || itemSizeArrayNode[3].getUInt() > 0) {
    StringRef invalidTyStr = m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::GsOutPrimType::Rect2d);
    vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType_1] =
        itemSizeArrayNode[1].getUInt() > 0 ? primTyStr : invalidTyStr;
    vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType_2] =
        itemSizeArrayNode[2].getUInt() > 0 ? primTyStr : invalidTyStr;
    vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType_3] =
        itemSizeArrayNode[3].getUInt() > 0 ? primTyStr : invalidTyStr;
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

  // Set LDS_SIZE of SPI_SHADER_PGM_RSRC2_GS
  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;

  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Gs);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = calcLdsSize(ldsSizeInDwords);
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
  if (m_gfxIp.major <= 11) {
    if (m_hasGs) {
      if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
        gsVgprCompCnt = 3; // Enable vtx4/vtx5 offset (GS VGPR3) or GS instance ID (GS VGPR4)
      else if (gsBuiltInUsage.primitiveIdIn)
        gsVgprCompCnt = 2; // Enable primitive ID (GS VGPR2)
      else if (calcFactor.inputVertices > 2)
        gsVgprCompCnt = 1; // Enable vtx2/vtx3 offset (GS VGPR1)
    } else if (m_hasVs) {
      // NOTE: When GS is absent, only those VGPRs are required: vtx0/vtx1 offset, vtx2/vtx3 offset,
      // primitive ID (only for VS).
      gsVgprCompCnt = 1;
      if (!hasTs && vsBuiltInUsage.primitiveId)
        gsVgprCompCnt = 2; // Enable primitive ID (GS VGPR2)
    }
  } else {
    llvm_unreachable("Not implemented!");
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::GsVgprCompCnt] = gsVgprCompCnt;

  // ES_VGPR_COMP_CNT in SPI_SHADER_PGM_RSRC2_GS
  unsigned esVgprCompCnt = 0;
  if (m_gfxIp.major <= 11) {
    if (hasTs) {
      if (tesBuiltInUsage.primitiveId)
        esVgprCompCnt = 3; // Enable patch ID (ES VGPR8)
      else
        esVgprCompCnt = 2; // Must enable relative patch ID (ES VGPR7)
    } else if (m_hasVs) {
      if (vsBuiltInUsage.instanceIndex)
        esVgprCompCnt = 3; // Enable instance ID (ES VGPR8)
    }
  } else {
    llvm_unreachable("Not implemented!");
  }
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::EsVgprCompCnt] = esVgprCompCnt;

  getHwShaderNode(Util::Abi::HardwareStage::Gs)[Util::Abi::HardwareStageMetadataKey::OffchipLdsEn] = hasTs;

  // VGT_GS_MAX_VERT_OUT
  const unsigned outputVertices = m_hasMesh ? m_pipelineState->getShaderModes()->getMeshShaderMode().outputVertices
                                            : m_pipelineState->getShaderModes()->getGeometryShaderMode().outputVertices;
  const unsigned maxVertOut = std::max(1u, outputVertices);
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
  } else if (m_hasGs) {
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
  vgtGsOutPrimType[Util::Abi::VgtGsOutPrimTypeMetadataKey::OutprimType] =
      m_pipelineState->getPalMetadata()->serializeEnum(Util::Abi::GsOutPrimType(gsOutputPrimitiveType));

  assert(calcFactor.primAmpFactor >= 1);
  unsigned maxVertsPerSubgroup = NggMaxThreadsPerSubgroup;
  unsigned threadsPerSubgroup = NggMaxThreadsPerSubgroup;
  unsigned spiShaderIdsFormat = SPI_SHADER_1COMP;
  if (m_hasMesh) {
    maxVertsPerSubgroup = std::min(meshMode.outputVertices, NggMaxThreadsPerSubgroup);
    threadsPerSubgroup = calcFactor.primAmpFactor;
    const bool enableMultiView = m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable;
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
      spiShaderGsMeshletDim[Util::Abi::SpiShaderGsMeshletDimMetadataKey::ThreadgroupSize] = threadGroupSize - 1;

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
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsVertItemsize] =
        4 * gsInOutUsage.outputMapLocCount;

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

    if (m_gfxIp.major <= 11) {
      // VGT_GSVS_RING_ITEMSIZE
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtGsvsRingItemsize] = calcFactor.gsVsRingItemSize;

      // VGT_ESGS_RING_ITEMSIZE
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtEsgsRingItemsize] =
          (m_hasGs ? calcFactor.esGsRingItemSize : 1);
    }

    const auto nggControl = m_pipelineState->getNggControl();
    assert(nggControl->enableNgg);
    if (!nggControl->passthroughMode) {
      // If the NGG culling data buffer is not already specified by a hardware stage's user_data_reg_map, then this
      // field specified the register offset that is expected to point to the low 32-bits of address to the buffer.
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::NggCullingDataReg] = mmSPI_SHADER_PGM_LO_GS;
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

  // Set LDS_SIZE of SPI_SHADER_PGM_RSRC2_GS
  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;

  auto hwShaderNode = getHwShaderNode(Util::Abi::HardwareStage::Gs);
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::LdsSize] = calcLdsSize(ldsSizeInDwords);
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
  const bool enablePrimStats = m_pipelineState->enablePrimStats();

  // VGT_STRMOUT_CONFIG
  auto vgtStrmoutConfig = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtStrmoutConfig].getMap(true);
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_0En] = enablePrimStats || streamXfbBuffers[0] > 0;
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_1En] = enablePrimStats || streamXfbBuffers[1] > 0;
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_2En] = enablePrimStats || streamXfbBuffers[2] > 0;
  vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::Streamout_3En] = enablePrimStats || streamXfbBuffers[3] > 0;
  if (shaderStage == ShaderStageCopyShader)
    vgtStrmoutConfig[Util::Abi::VgtStrmoutConfigMetadataKey::RastStream] =
        m_pipelineState->getRasterizerState().rasterStream;

  // Set some field of SPI_SHADER_PGM_RSRC2_VS
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsStreamoutEn] = enableXfb;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase0En] = xfbStrides[0] > 0;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase1En] = xfbStrides[1] > 0;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase2En] = xfbStrides[2] > 0;
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VsSoBase3En] = xfbStrides[3] > 0;

  // VGT_STRMOUT_VTX_STRIDE_*
  unsigned xfbStridesInDwords[MaxTransformFeedbackBuffers] = {};
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    // Must be multiple of dword (PAL doesn't support 16-bit transform feedback outputs)
    assert(xfbStrides[i] % sizeof(unsigned) == 0);
    xfbStridesInDwords[i] = xfbStrides[i] / sizeof(unsigned);
  }
  setStreamOutVertexStrides(xfbStridesInDwords);

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

    if (m_pipelineState->isTessOffChip())
      getHwShaderNode(Util::Abi::HardwareStage::Vs)[Util::Abi::HardwareStageMetadataKey::OffchipLdsEn] = true;
  }
}

// =====================================================================================================================
// Builds register configuration for hardware pixel shader.
void RegisterMetadataBuilder::buildPsRegisters() {
  ShaderStage shaderStage = ShaderStageFragment;
  const auto &options = m_pipelineState->getOptions();
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto &fragmentMode = m_pipelineState->getShaderModes()->getFragmentShaderMode();
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.fs;

  const bool useFloatLocationAtIteratedSampleNumber =
      options.fragCoordUsesInterpLoc ? builtInUsage.fragCoordIsSample : builtInUsage.runAtSampleRate;

  // SPI_BARYC_CNTL
  auto spiBarycCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::SpiBarycCntl].getMap(true);
  spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::FrontFaceAllBits] = true;
  if (fragmentMode.pixelCenterInteger) {
    // TRUE - Force floating point position to upper left corner of pixel (X.0, Y.0)
    spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::PosFloatUlc] = true;
  } else if (useFloatLocationAtIteratedSampleNumber) {
    // 2 - Calculate per-pixel floating point position at iterated sample number
    spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::PosFloatLocation] = 2;
  } else {
    // 0 - Calculate per-pixel floating point position at pixel center
    spiBarycCntl[Util::Abi::SpiBarycCntlMetadataKey::PosFloatLocation] = 0;
  }

  // Provoking vtx
  if (m_pipelineState->getShaderInterfaceData(shaderStage)->entryArgIdxs.fs.provokingVtxInfo != 0) {
    assert(m_gfxIp >= GfxIpVersion({10, 3}));
    getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PsLoadProvokingVtx] = true;
  }

  // PA_SC_MODE_CNTL_1
  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PsIterSample] =
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
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::KillEnable] = builtInUsage.discard == 1;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ZExportEnable] = builtInUsage.fragDepth;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::StencilTestValExportEnable] = builtInUsage.fragStencilRef;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::MaskExportEnable] = builtInUsage.sampleMask == 1;
  // Set during pipeline finalization.
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::AlphaToMaskDisable] = true;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::DepthBeforeShader] = fragmentMode.earlyFragmentTests;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ExecOnNoop] =
      fragmentMode.earlyFragmentTests && resUsage->resourceWrite;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ExecOnHierFail] = execOnHeirFail;
  dbShaderControl[Util::Abi::DbShaderControlMetadataKey::ConservativeZExport] = conservativeZExport;
  if (m_gfxIp.major >= 10)
    dbShaderControl[Util::Abi::DbShaderControlMetadataKey::PreShaderDepthCoverageEnable] =
        fragmentMode.postDepthCoverage;

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
      // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and
      // PRIM_EXPORT_COUNT. When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even
      // though this is not what we want. Hence, we should reserve param0 as a dummy vertex attribute and all
      // primitive attributes are moved after it.
      bool hasNoVertexAttrib = m_pipelineState->getShaderResourceUsage(ShaderStageMesh)->inOutUsage.expCount == 0;
      if (hasNoVertexAttrib)
        ++spiPsInputCntlInfo.offset;
      spiPsInputCntlInfo.primAttr = true;
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

    // NOTE: Set SPI_PS_INPUT_CNTL_* here, but the register can still be changed later,
    // when it becomes known that gl_ViewportIndex is not used and fields OFFSET and FLAT_SHADE
    // can be amended.
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::FlatShade] = spiPsInputCntlInfo.flatShade;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Offset] = spiPsInputCntlInfo.offset;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Fp16InterpMode] = spiPsInputCntlInfo.fp16InterMode;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::PtSpriteTex] = spiPsInputCntlInfo.ptSpriteTex;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Attr0Valid] = spiPsInputCntlInfo.attr0Valid;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::Attr1Valid] = spiPsInputCntlInfo.attr1Valid;
    spiPsInputCntElem[Util::Abi::SpiPsInputCntlMetadataKey::PrimAttr] = spiPsInputCntlInfo.primAttr;
  }
  // Set .num_interpolants in amdpal.pipelines
  getPipelineNode()[Util::Abi::PipelineMetadataKey::NumInterpolants] = unsigned(interpInfo->size());

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
  const auto waveSize = m_pipelineState->getShaderWaveSize(shaderStage);
  spiPsInControl[Util::Abi::SpiPsInControlMetadataKey::PsW32En] = (waveSize == 32);

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

  // Fill .ps_input_semantic for partial pipeline
  if (m_pipelineState->isUnlinked()) {
    // Collect semantic info for generic input and builtIns {gl_ClipDistance, gl_CulDistance, gl_Layer,
    // gl_ViewportIndex} that exports via generic output as well.
    auto &inputLocInfoMap = resUsage->inOutUsage.inputLocInfoMap;
    auto &builtInInputLocMap = resUsage->inOutUsage.builtInInputLocMap;
    if (!inputLocInfoMap.empty() || !builtInInputLocMap.empty()) {
      auto psInputSemanticNode = getPipelineNode()[Util::Abi::PipelineMetadataKey::PsInputSemantic].getArray(true);
      unsigned elemIdx = 0;
      for (auto locInfoPair : inputLocInfoMap) {
        auto psInputSemanticElem = psInputSemanticNode[elemIdx].getMap(true);
        psInputSemanticElem[Util::Abi::PsInputSemanticMetadataKey::Semantic] =
            MaxBuiltInSemantic + locInfoPair.first.getLocation();
        ++elemIdx;
      }

      for (auto locPair : builtInInputLocMap) {
        if (locPair.first == BuiltInClipDistance || locPair.first == BuiltInCullDistance ||
            locPair.first == BuiltInLayer || locPair.first == BuiltInViewportIndex) {
          assert(locPair.first < MaxBuiltInSemantic);
          auto psInputSemanticElem = psInputSemanticNode[elemIdx].getMap(true);
          psInputSemanticElem[Util::Abi::PsInputSemanticMetadataKey::Semantic] = locPair.first;
          ++elemIdx;
        }
      }
    }
  }
}

// =====================================================================================================================
// Builds register configuration for compute/task shader.
void RegisterMetadataBuilder::buildCsRegisters(ShaderStage shaderStage) {
  assert(shaderStage == ShaderStageCompute || shaderStage == ShaderStageTask);
  if (shaderStage == ShaderStageCompute) {
    Function *entryFunc = nullptr;
    for (Function &func : *m_module) {
      // Only entrypoint compute shader may have the function attribute for workgroup id optimization.
      if (isShaderEntryPoint(&func)) {
        entryFunc = &func;
        break;
      }
    }
    bool hasWorkgroupIdX = !entryFunc || !entryFunc->hasFnAttribute("amdgpu-no-workgroup-id-x");
    bool hasWorkgroupIdY = !entryFunc || !entryFunc->hasFnAttribute("amdgpu-no-workgroup-id-y");
    bool hasWorkgroupIdZ = !entryFunc || !entryFunc->hasFnAttribute("amdgpu-no-workgroup-id-z");
    getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidXEn] = hasWorkgroupIdX;
    getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidYEn] = hasWorkgroupIdY;
    getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidZEn] = hasWorkgroupIdZ;
  } else {
    getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidXEn] = true;
    getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidYEn] = true;
    getComputeRegNode()[Util::Abi::ComputeRegisterMetadataKey::TgidZEn] = true;
  }
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
// @param hwStage: The hardware shader stage
// @param apiStage1: The first api shader stage
// @param apiStage2: The second api shader stage
void RegisterMetadataBuilder::buildShaderExecutionRegisters(Util::Abi::HardwareStage hwStage, ShaderStage apiStage1,
                                                            ShaderStage apiStage2) {
  // Set hardware stage metadata
  auto hwShaderNode = getHwShaderNode(hwStage);
  ShaderStage apiStage = apiStage2 != ShaderStageInvalid ? apiStage2 : apiStage1;

  if (m_isNggMode || m_gfxIp.major >= 10) {
    const unsigned waveSize = m_pipelineState->getShaderWaveSize(apiStage);
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::WavefrontSize] = waveSize;
  }

  unsigned checksum = 0;
  if (apiStage1 != ShaderStageInvalid && apiStage1 != ShaderStageCopyShader)
    checksum = setShaderHash(apiStage1);
  if (apiStage2 != ShaderStageInvalid)
    checksum ^= setShaderHash(apiStage2);
  if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling)
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::ChecksumValue] = checksum;

  hwShaderNode[Util::Abi::HardwareStageMetadataKey::FloatMode] = setupFloatingPointMode(apiStage);

  unsigned userDataCount = 0;
  unsigned sgprLimits = 0;
  unsigned vgprLimits = 0;
  if (apiStage1 == ShaderStageCopyShader) {
    // NOTE: For copy shader, usually we use fixed number of user data registers.
    // But in some cases, we may change user data registers, we use variable to keep user sgpr count here
    userDataCount = lgc::CopyShaderUserSgprCount;
    sgprLimits = m_pipelineState->getTargetInfo().getGpuProperty().maxSgprsAvailable;
    vgprLimits = m_pipelineState->getTargetInfo().getGpuProperty().maxVgprsAvailable;
  } else {
    userDataCount = 0;
    if (apiStage1 != ShaderStageInvalid)
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
    if (hwStage == Util::Abi::HardwareStage::Hs || hwStage == Util::Abi::HardwareStage::Gs) {
      bool wgpMode = false;
      if (apiStage1 != ShaderStageInvalid)
        wgpMode = m_pipelineState->getShaderWgpMode(apiStage1);
      if (apiStage2 != ShaderStageInvalid)
        wgpMode = wgpMode || m_pipelineState->getShaderWgpMode(apiStage2);
      hwShaderNode[Util::Abi::HardwareStageMetadataKey::WgpMode] = wgpMode;
    }
  }

  hwShaderNode[Util::Abi::HardwareStageMetadataKey::SgprLimit] = sgprLimits;
  hwShaderNode[Util::Abi::HardwareStageMetadataKey::VgprLimit] = vgprLimits;

  if (m_gfxIp.major >= 11 && hwStage != Util::Abi::HardwareStage::Vs) {
    bool useImageOp = false;
    if (apiStage1 != ShaderStageInvalid)
      useImageOp = m_pipelineState->getShaderResourceUsage(apiStage1)->useImageOp;
    if (apiStage2 != ShaderStageInvalid)
      useImageOp |= m_pipelineState->getShaderResourceUsage(apiStage2)->useImageOp;
    hwShaderNode[Util::Abi::HardwareStageMetadataKey::ImageOp] = useImageOp;
  }

  // Fill ".user_data_reg_map" and update ".user_data_limit"
  auto userDataNode = hwShaderNode[Util::Abi::HardwareStageMetadataKey::UserDataRegMap].getArray(true);
  unsigned idx = 0;
  unsigned userDataLimit = 1;
  for (auto value : m_pipelineState->getUserDataMap(apiStage)) {
    userDataNode[idx++] = value;
    if (value < InterfaceData::MaxSpillTableSize && (value + 1) > userDataLimit)
      userDataLimit = value + 1;
  }
  m_pipelineState->getPalMetadata()->setUserDataLimit(userDataLimit);
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
  bool useViewportIndexImplicitly = false;
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

    useLayer = useLayer || m_pipelineState->getInputAssemblyState().multiView != MultiViewMode::Disable;
    // useViewportIndex must be set in this mode as API shader may not export viewport index.
    if (m_pipelineState->getInputAssemblyState().multiView == MultiViewMode::PerView) {
      useViewportIndexImplicitly = !useViewportIndex;
      useViewportIndex = true;
    }

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
  // If viewport index is implicitly set by multiview, then it must be uniform and reuse should be allowed.
  if (useViewportIndex && !useViewportIndexImplicitly)
    disableVertexReuse = true;

  getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtReuseOff] =
      disableVertexReuse || m_pipelineState->enableSwXfb();

  // PA_CL_CLIP_CNTL
  msgpack::MapDocNode paClClipCntl =
      getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaClClipCntl].getMap(true);
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

  if (miscExport) {
    auto paClVsOutCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaClVsOutCntl].getMap(true);
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
    auto paClVsOutCntl = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::PaClVsOutCntl].getMap(true);
    paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutCcDist0VecEna] = true;

    if (clipDistanceCount + cullDistanceCount > 4)
      paClVsOutCntl[Util::Abi::PaClVsOutCntlMetadataKey::VsOutCcDist1VecEna] = true;

    unsigned clipDistanceMask = (1 << clipDistanceCount) - 1;
    unsigned cullDistanceMask = ((1 << cullDistanceCount) - 1) << clipDistanceCount;

    // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
    static const unsigned MaxDistCount = 8;
    bool clipDistEna[MaxDistCount] = {};
    bool cullDistEna[MaxDistCount] = {};
    for (unsigned i = 0; i < MaxDistCount; ++i) {
      clipDistEna[i] = (clipDistanceMask >> i) & 0x1;
      // Note: Point primitives are only affected by the cull mask, so enable culling also based on clip distances
      cullDistEna[i] = ((clipDistanceMask | cullDistanceMask) >> i) & 0x1;
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
  auto vgtShaderStagesEn = getGraphicsRegNode()[Util::Abi::GraphicsRegisterMetadataKey::VgtShaderStagesEn].getMap(true);
  vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::MaxPrimgroupInWave] = 2;

  auto nggControl = m_pipelineState->getNggControl();

  if (m_isNggMode || m_hasMesh) {
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenEn] = true;
    if (!m_hasMesh) {
      if (m_gfxIp.major <= 11) {
        // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
        // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
        // hardware pass-through).
        vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenPassthruEn] =
            m_hasGs ? false : nggControl->passthroughMode;
      }

      if (m_gfxIp.major >= 11) {
        vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::NggWaveIdEn] = m_pipelineState->enableSwXfb();
        if (!m_hasGs)
          vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::PrimgenPassthruNoMsg] =
              nggControl->passthroughMode && !m_pipelineState->enableSwXfb();
      }
    } else {
      const unsigned gsFastLaunch = m_gfxIp.major == 11 ? 0x2 : 0x1; // GFX11 defines the new fast launch mode to 0x2
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::GsFastLaunch] = gsFastLaunch;
    }
  } else if (m_hasTcs || m_hasTes) {
    //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
    //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::DynamicHs] = true;
  }

  if (hwStageMask & Util::Abi::HwShaderVs) {
    assert(m_gfxIp.major < 11);
    ShaderStage apiStage = ShaderStageVertex;
    unsigned vsStageEn = VS_STAGE_REAL;
    if (m_pipelineState->hasShaderStage(ShaderStageCopyShader)) {
      apiStage = ShaderStageCopyShader;
      vsStageEn = VS_STAGE_COPY_SHADER;
    } else if (m_hasTes) {
      apiStage = ShaderStageTessEval;
      vsStageEn = VS_STAGE_DS;
    }
    const auto waveSize = m_pipelineState->getShaderWaveSize(apiStage);
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::VsW32En] = (waveSize == 32);

    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::VsStageEn] = vsStageEn;
  }

  if (hwStageMask & Util::Abi::HwShaderGs) {
    ShaderStage apiStage = ShaderStageVertex;
    if (m_hasGs || m_hasMesh) {
      apiStage = m_hasGs ? ShaderStageGeometry : ShaderStageMesh;
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::GsStageEn] = GS_STAGE_ON;
    } else if (m_hasTes) {
      apiStage = ShaderStageTessEval;
    }
    const auto waveSize = m_pipelineState->getShaderWaveSize(apiStage);
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::GsW32En] = (waveSize == 32);

    if (m_gfxIp.major <= 11) {
      vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::EsStageEn] = m_hasTes ? ES_STAGE_DS : ES_STAGE_REAL;
      if (m_isNggMode && !m_hasMesh)
        vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::VsStageEn] = VS_STAGE_REAL;
    }
  }

  if (hwStageMask & Util::Abi::HwShaderHs) {
    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    vgtShaderStagesEn[Util::Abi::VgtShaderStagesEnMetadataKey::HsW32En] = (waveSize == 32);

    if (m_gfxIp.major <= 11)
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

// =====================================================================================================================
// Calculate the LDS size in bytes.
//
// @param onChipLdsSize : The value of onChip LDS size
unsigned RegisterMetadataBuilder::calcLdsSize(unsigned ldsSizeInDwords) {
  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);
  return (ldsSizeInDwords * 4);
}

} // namespace Gfx9

} // namespace lgc
