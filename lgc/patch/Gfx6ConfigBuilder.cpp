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
 * @file  Gfx6ConfigBuilder.cpp
 * @brief LLPC header file: contains implementation of class lgc::Gfx6::ConfigBuilder.
 ***********************************************************************************************************************
 */
#include "Gfx6ConfigBuilder.h"
#include "lgc/BuiltIns.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

#define DEBUG_TYPE "lgc-gfx6-config-builder"

using namespace llvm;

namespace lgc {

namespace Gfx6 {

#include "chip/gfx6/si_ci_vi_merged_enum.h"
#include "chip/gfx6/si_ci_vi_merged_offset.h"

using namespace Pal::Gfx6::Chip;

// =====================================================================================================================
// Builds PAL metadata for pipeline.
void ConfigBuilder::buildPalMetadata() {
  if (!m_pipelineState->isGraphics())
    buildPipelineCsRegConfig();
  else {
    const bool hasTs = (m_hasTcs || m_hasTes);

    if (!hasTs && !m_hasGs) {
      // VS-FS pipeline or FS-only shader compilation (part-pipeline compilation)
      buildPipelineVsFsRegConfig();
    } else if (hasTs && !m_hasGs) {
      // VS-TS-FS pipeline
      buildPipelineVsTsFsRegConfig();
    } else if (!hasTs && m_hasGs) {
      // VS-GS-FS pipeline
      buildPipelineVsGsFsRegConfig();
    } else {
      // VS-TS-GS-FS pipeline
      buildPipelineVsTsGsFsRegConfig();
    }
  }

  writePalMetadata();
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-FS), or FS-only shader compilation.
void ConfigBuilder::buildPipelineVsFsRegConfig() {
  PipelineVsFsRegConfig config;

  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex)) {
    setPipelineType(Util::Abi::PipelineType::VsPs);
    addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderVs);
    buildVsRegConfig<PipelineVsFsRegConfig>(ShaderStageVertex, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

    setShaderHash(ShaderStageVertex);

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const unsigned primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsFsRegConfig>(ShaderStageFragment, &config);

    setShaderHash(ShaderStageFragment);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-FS).
void ConfigBuilder::buildPipelineVsTsFsRegConfig() {
  PipelineVsTsFsRegConfig config;

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderLs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Tess);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex)) {
    buildLsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageVertex, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

    setShaderHash(ShaderStageVertex);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessControl)) {
    buildHsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageTessControl, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);

    setShaderHash(ShaderStageTessControl);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessEval)) {
    buildVsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageTessEval, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_DS);

    setShaderHash(ShaderStageTessEval);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageFragment, &config);

    setShaderHash(ShaderStageFragment);
  }

  if (m_pipelineState->isTessOffChip()) {
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
  const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

  if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId) {
    iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
  }

  SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

  // Set up VGT_TF_PARAM
  setupVgtTfParam<PipelineVsTsFsRegConfig>(&config);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-GS-FS).
void ConfigBuilder::buildPipelineVsGsFsRegConfig() {
  PipelineVsGsFsRegConfig config;

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderEs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Gs);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex)) {
    buildEsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageVertex, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);

    setShaderHash(ShaderStageVertex);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageGeometry)) {
    buildGsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageGeometry, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    setShaderHash(ShaderStageGeometry);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageFragment, &config);

    setShaderHash(ShaderStageFragment);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageCopyShader)) {
    buildVsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageCopyShader, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const unsigned primGroupSize = 128;
  iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

  SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-GS-FS).
void ConfigBuilder::buildPipelineVsTsGsFsRegConfig() {
  PipelineVsTsGsFsRegConfig config;

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderLs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderEs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::GsTess);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex)) {
    buildLsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageVertex, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

    setShaderHash(ShaderStageVertex);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessControl)) {
    buildHsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageTessControl, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);

    setShaderHash(ShaderStageTessControl);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessEval)) {
    buildEsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageTessEval, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);

    setShaderHash(ShaderStageTessEval);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageGeometry)) {
    buildGsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageGeometry, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    setShaderHash(ShaderStageGeometry);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageFragment, &config);

    setShaderHash(ShaderStageFragment);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageCopyShader)) {
    buildVsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageCopyShader, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);
  }

  if (m_pipelineState->isTessOffChip()) {
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
  const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
  const auto &gsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

  // With tessellation, SWITCH_ON_EOI and PARTIAL_ES_WAVE_ON must be set if primitive ID is used by either the TCS, TES,
  // or GS.
  if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveIdIn) {
    iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
  }

  SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

  // Set up VGT_TF_PARAM
  setupVgtTfParam<PipelineVsTsGsFsRegConfig>(&config);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for compute pipeline.
void ConfigBuilder::buildPipelineCsRegConfig() {
  assert(m_pipelineState->hasShaderStage(ShaderStageCompute));

  CsRegConfig config;

  addApiHwShaderMapping(ShaderStageCompute, Util::Abi::HwShaderCs);

  setPipelineType(Util::Abi::PipelineType::Cs);

  buildCsRegConfig(ShaderStageCompute, &config);

  setShaderHash(ShaderStageCompute);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for hardware vertex shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for vertex-shader-specific pipeline
template <typename T> void ConfigBuilder::buildVsRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageVertex || shaderStage == ShaderStageTessEval ||
         shaderStage == ShaderStageCopyShader);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage;

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP, true); // Follow PAL setting

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();
  bool enableXfb = m_pipelineState->enableXfb();

  if (shaderStage == ShaderStageCopyShader) {
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, lgc::CopyShaderUserSgprCount);
    setNumAvailSgprs(Util::Abi::HardwareStage::Vs, m_pipelineState->getTargetInfo().getGpuProperty().maxSgprsAvailable);
    setNumAvailVgprs(Util::Abi::HardwareStage::Vs, m_pipelineState->getTargetInfo().getGpuProperty().maxVgprsAvailable);

    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN, streamXfbBuffers[0] > 0);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN, streamXfbBuffers[1] > 0);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN, streamXfbBuffers[2] > 0);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN, streamXfbBuffers[3] > 0);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, RAST_STREAM, resUsage->inOutUsage.gs.rasterStream);
  } else {
    const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, DEBUG_MODE, shaderOptions.debugMode);
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, TRAP_PRESENT, shaderOptions.trapPresent);

    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, intfData->userDataCount);

    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN, enableXfb);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN, false);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN, false);
    SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN, false);

    setNumAvailSgprs(Util::Abi::HardwareStage::Vs, resUsage->numSgprsAvailable);
    setNumAvailVgprs(Util::Abi::HardwareStage::Vs, resUsage->numVgprsAvailable);
  }

  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_EN, enableXfb);
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE0_EN, (xfbStrides[0] > 0));
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE1_EN, (xfbStrides[1] > 0));
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE2_EN, (xfbStrides[2] > 0));
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE3_EN, (xfbStrides[3] > 0));

  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_0, STRIDE, xfbStrides[0] / sizeof(int));
  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_1, STRIDE, xfbStrides[1] / sizeof(int));
  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_2, STRIDE, xfbStrides[2] / sizeof(int));
  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_3, STRIDE, xfbStrides[3] / sizeof(int));

  unsigned streamBufferConfig = 0;
  for (auto i = 0; i < MaxGsStreams; ++i)
    streamBufferConfig |= (streamXfbBuffers[i] << (i * 4));
  SET_REG(&config->vsRegs, VGT_STRMOUT_BUFFER_CONFIG, streamBufferConfig);

  bool disableVertexReuse = m_pipelineState->getInputAssemblyState().disableVertexReuse;

  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VPORT_X_SCALE_ENA, true);
  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VPORT_X_OFFSET_ENA, true);
  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VPORT_Y_SCALE_ENA, true);
  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VPORT_Y_OFFSET_ENA, true);
  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VPORT_Z_SCALE_ENA, true);
  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VPORT_Z_OFFSET_ENA, true);
  SET_REG_FIELD(&config->vsRegs, PA_CL_VTE_CNTL, VTX_W0_FMT, true);

  SET_REG_FIELD(&config->vsRegs, PA_SU_VTX_CNTL, PIX_CENTER, 1);
  SET_REG_FIELD(&config->vsRegs, PA_SU_VTX_CNTL, ROUND_MODE, 2); // Round to even
  SET_REG_FIELD(&config->vsRegs, PA_SU_VTX_CNTL, QUANT_MODE, 5); // Use 8-bit fractions

  // Stage-specific processing
  bool usePointSize = false;
  bool usePrimitiveId = false;
  bool useLayer = false;
  bool useViewportIndex = false;
  unsigned clipDistanceCount = 0;
  unsigned cullDistanceCount = 0;

  if (shaderStage == ShaderStageVertex) {
    usePointSize = builtInUsage.vs.pointSize;
    usePrimitiveId = builtInUsage.vs.primitiveId;
    useLayer = builtInUsage.vs.layer;
    useViewportIndex = builtInUsage.vs.viewportIndex;
    clipDistanceCount = builtInUsage.vs.clipDistance;
    cullDistanceCount = builtInUsage.vs.cullDistance;

    if (builtInUsage.vs.instanceIndex) {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable instance ID
    } else if (builtInUsage.vs.primitiveId) {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
    }
  } else if (shaderStage == ShaderStageTessEval) {
    usePointSize = builtInUsage.tes.pointSize;
    usePrimitiveId = builtInUsage.tes.primitiveId;
    useLayer = builtInUsage.tes.layer;
    useViewportIndex = builtInUsage.tes.viewportIndex;
    clipDistanceCount = builtInUsage.tes.clipDistance;
    cullDistanceCount = builtInUsage.tes.cullDistance;

    if (builtInUsage.tes.primitiveId) {
      // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable primitive ID
    } else {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
    }

    if (m_pipelineState->isTessOffChip()) {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN, true);
    }
  } else {
    assert(shaderStage == ShaderStageCopyShader);

    usePointSize = builtInUsage.gs.pointSize;
    usePrimitiveId = builtInUsage.gs.primitiveIdIn;
    useLayer = builtInUsage.gs.layer;
    useViewportIndex = builtInUsage.gs.viewportIndex;
    clipDistanceCount = builtInUsage.gs.clipDistance;
    cullDistanceCount = builtInUsage.gs.cullDistance;
  }

  SET_REG_FIELD(&config->vsRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, usePrimitiveId);
  SET_REG_FIELD(&config->vsRegs, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, resUsage->inOutUsage.expCount - 1);
  setUsesViewportArrayIndex(useViewportIndex);

  // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
  // index if each GS, DS, or VS invocation emits the same viewport array index for each vertex and we set
  // VTE_VPORT_PROVOKE_DISABLE.
  if (useViewportIndex) {
    // TODO: In the future, we can only disable vertex reuse only if viewport array index is emitted divergently
    // for each vertex.
    disableVertexReuse = true;
    SET_REG_FIELD(&config->vsRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, true);
  } else {
    SET_REG_FIELD(&config->vsRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, false);
  }

  SET_REG_FIELD(&config->vsRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

  SET_REG_FIELD(&config->vsRegs, VGT_VERTEX_REUSE_BLOCK_CNTL, VTX_REUSE_DEPTH, 14);

  useLayer = useLayer || m_pipelineState->getInputAssemblyState().enableMultiView;

  bool miscExport = usePointSize || useLayer || useViewportIndex;
  if (miscExport) {
    SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);
    SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
    SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);
    SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
    SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);
  }

  if (clipDistanceCount > 0 || cullDistanceCount > 0) {
    SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST0_VEC_ENA, true);
    if (clipDistanceCount + cullDistanceCount > 4) {
      SET_REG_FIELD(&config->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST1_VEC_ENA, true);
    }

    unsigned clipDistanceMask = (1 << clipDistanceCount) - 1;
    unsigned cullDistanceMask = ((1 << cullDistanceCount) - 1) << clipDistanceCount;

    // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
    unsigned paClVsOutCntl = GET_REG(&config->vsRegs, PA_CL_VS_OUT_CNTL);
    paClVsOutCntl |= clipDistanceMask;
    paClVsOutCntl |= (cullDistanceMask << 8);
    SET_REG(&config->vsRegs, PA_CL_VS_OUT_CNTL, paClVsOutCntl);
  }

  unsigned posCount = 1; // gl_Position is always exported
  if (miscExport)
    ++posCount;

  if (clipDistanceCount + cullDistanceCount > 0) {
    ++posCount;
    if (clipDistanceCount + cullDistanceCount > 4)
      ++posCount;
  }

  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_POS_FORMAT, POS0_EXPORT_FORMAT, SPI_SHADER_4COMP);
  if (posCount > 1) {
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_POS_FORMAT, POS1_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 2) {
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_POS_FORMAT, POS2_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 3) {
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_POS_FORMAT, POS3_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
}

// =====================================================================================================================
// Builds register configuration for hardware hull shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for hull-shader-specific pipeline
template <typename T> void ConfigBuilder::buildHsRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageTessControl);

  const auto &intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &calcFactor = resUsage->inOutUsage.tcs.calcFactor;
  const auto &tessMode = m_pipelineState->getShaderModes()->getTessellationMode();

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->hsRegs, SPI_SHADER_PGM_RSRC1_HS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->hsRegs, SPI_SHADER_PGM_RSRC1_HS, DX10_CLAMP, true); // Follow PAL setting

  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  SET_REG_FIELD(&config->hsRegs, SPI_SHADER_PGM_RSRC1_HS, DEBUG_MODE, shaderOptions.debugMode);
  SET_REG_FIELD(&config->hsRegs, SPI_SHADER_PGM_RSRC2_HS, TRAP_PRESENT, shaderOptions.trapPresent);
  SET_REG_FIELD(&config->hsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR, intfData->userDataCount);

  if (m_pipelineState->isTessOffChip()) {
    SET_REG_FIELD(&config->hsRegs, SPI_SHADER_PGM_RSRC2_HS, OC_LDS_EN, true);
  }

  // Minimum and maximum tessellation factors supported by the hardware.
  constexpr float minTessFactor = 1.0f;
  constexpr float maxTessFactor = 64.0f;
  SET_REG(&config->hsRegs, VGT_HOS_MIN_TESS_LEVEL, FloatToBits(minTessFactor));
  SET_REG(&config->hsRegs, VGT_HOS_MAX_TESS_LEVEL, FloatToBits(maxTessFactor));

  // Set VGT_LS_HS_CONFIG
  SET_REG_FIELD(&config->hsRegs, VGT_LS_HS_CONFIG, NUM_PATCHES, calcFactor.patchCountPerThreadGroup);
  SET_REG_FIELD(&config->hsRegs, VGT_LS_HS_CONFIG, HS_NUM_INPUT_CP,
                m_pipelineState->getInputAssemblyState().patchControlPoints);

  auto hsNumOutputCp = tessMode.outputVertices;
  SET_REG_FIELD(&config->hsRegs, VGT_LS_HS_CONFIG, HS_NUM_OUTPUT_CP, hsNumOutputCp);

  setNumAvailSgprs(Util::Abi::HardwareStage::Hs, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Hs, resUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Builds register configuration for hardware export shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for export-shader-specific pipeline
template <typename T> void ConfigBuilder::buildEsRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageVertex || shaderStage == ShaderStageTessEval);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage;

  assert(m_pipelineState->hasShaderStage(ShaderStageGeometry));
  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC1_ES, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC1_ES, DX10_CLAMP, true); // Follow PAL setting

  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC1_ES, DEBUG_MODE, shaderOptions.debugMode);
  SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC2_ES, TRAP_PRESENT, shaderOptions.trapPresent);
  if (m_pipelineState->isGsOnChip()) {
    assert(calcFactor.gsOnChipLdsSize <= m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipMaxLdsSize);

    const unsigned ldsSizeDwordGranularityShift =
        m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
    const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

    const unsigned ldsSizeInDwords = alignTo(calcFactor.gsOnChipLdsSize, ldsSizeDwordGranularity);

    const unsigned ldsSize = ldsSizeInDwords >> ldsSizeDwordGranularityShift;
    SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC2_ES, LDS_SIZE__CI__VI, ldsSize);
    setEsGsLdsSize(calcFactor.esGsLdsSize * 4);
  }

  unsigned vgprCompCnt = 0;
  if (shaderStage == ShaderStageVertex) {
    if (builtInUsage.vs.instanceIndex)
      vgprCompCnt = 3; // Enable instance ID
  } else {
    assert(shaderStage == ShaderStageTessEval);

    // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
    if (builtInUsage.tes.primitiveId)
      vgprCompCnt = 3;
    else
      vgprCompCnt = 2;

    if (m_pipelineState->isTessOffChip()) {
      SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC2_ES, OC_LDS_EN, true);
    }
  }

  SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC1_ES, VGPR_COMP_CNT, vgprCompCnt);

  SET_REG_FIELD(&config->esRegs, SPI_SHADER_PGM_RSRC2_ES, USER_SGPR, intfData->userDataCount);

  SET_REG_FIELD(&config->esRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

  setNumAvailSgprs(Util::Abi::HardwareStage::Es, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Es, resUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Builds register configuration for hardware local shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for local-shader-specific pipeline
template <typename T> void ConfigBuilder::buildLsRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageVertex);

  const auto &intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.vs;

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC1_LS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC1_LS, DX10_CLAMP, true); // Follow PAL setting
  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC1_LS, DEBUG_MODE, shaderOptions.debugMode);
  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC2_LS, TRAP_PRESENT, shaderOptions.trapPresent);

  unsigned vgtCompCnt = 1;
  if (builtInUsage.instanceIndex)
    vgtCompCnt += 2; // Enable instance ID
  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC1_LS, VGPR_COMP_CNT, vgtCompCnt);

  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC2_LS, USER_SGPR, intfData->userDataCount);

  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;
  unsigned ldsSizeInDwords = calcFactor.tessOnChipLdsSize;

  auto gpuWorkarounds = &m_pipelineState->getTargetInfo().getGpuWorkarounds();

  // Override the LDS size based on hardware workarounds.
  if (gpuWorkarounds->gfx6.shaderSpiBarrierMgmt != 0) {
    // The SPI has a bug where the VS never checks for or waits on barrier resources, so if all barriers are in-use
    // on a CU which gets picked for VS work the SPI will overflow the resources and clobber the barrier tracking.
    // (There are 16 barriers available per CU, if resource reservations have not reduced this.)
    //
    // The workaround is to set a minimum LDS allocation size of 4KB for all dependent groups (tessellation, onchip
    // GS, and CS) threadgroups larger than one wavefront.  This means that any wave type which wants to use a
    // barrier must allocate >= 1/16th of the available LDS space per CU which will guarantee that the SPI will not
    // overflow the resource tracking (since LDS will be full).

    // If the HS threadgroup requires more than one wavefront, barriers will be allocated and we need to limit the
    // number of thread groups in flight.
    const unsigned outputVertices = m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;

    const unsigned threadGroupSize = calcFactor.patchCountPerThreadGroup * outputVertices;
    const unsigned waveSize = m_pipelineState->getTargetInfo().getGpuProperty().waveSize;
    const unsigned wavesPerThreadGroup = (threadGroupSize + waveSize - 1) / waveSize;

    if (wavesPerThreadGroup > 1) {
      constexpr unsigned minLdsSizeWa = 1024; // 4KB in dwords.
      ldsSizeInDwords = std::max(ldsSizeInDwords, minLdsSizeWa);
    }
  }

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  const unsigned ldsSize = ldsSizeInDwords >> ldsSizeDwordGranularityShift;
  SET_REG_FIELD(&config->lsRegs, SPI_SHADER_PGM_RSRC2_LS, LDS_SIZE, ldsSize);
  setLdsSizeByteSize(Util::Abi::HardwareStage::Ls, ldsSizeInDwords * 4);

  setNumAvailSgprs(Util::Abi::HardwareStage::Ls, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Ls, resUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Builds register configuration for hardware geometry shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for geometry-shader-specific pipeline
template <typename T> void ConfigBuilder::buildGsRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageGeometry);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.gs;
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const auto &inOutUsage = resUsage->inOutUsage;

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->gsRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->gsRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  SET_REG_FIELD(&config->gsRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, shaderOptions.debugMode);
  SET_REG_FIELD(&config->gsRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, shaderOptions.trapPresent);
  SET_REG_FIELD(&config->gsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, intfData->userDataCount);

  const bool primAdjacency = geometryMode.inputPrimitive == InputPrimitives::LinesAdjacency ||
                             geometryMode.inputPrimitive == InputPrimitives::TrianglesAdjacency;

  // Maximum number of GS primitives per ES thread is capped by the hardware's GS-prim FIFO.
  auto gpuProp = &m_pipelineState->getTargetInfo().getGpuProperty();
  unsigned maxGsPerEs = (gpuProp->gsPrimBufferDepth + gpuProp->waveSize);

  // This limit is halved if the primitive topology is adjacency-typed
  if (primAdjacency)
    maxGsPerEs >>= 1;

  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  SET_REG_FIELD(&config->gsRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

  // TODO: Currently only support offchip GS
  SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
  if (m_pipelineState->isGsOnChip()) {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, ONCHIP__CI__VI, VGT_GS_MODE_ONCHIP_ON);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, false);

    unsigned gsPrimsPerSubgrp = std::min(maxGsPerEs, inOutUsage.gs.calcFactor.gsPrimsPerSubgroup);

    SET_REG_FIELD(&config->gsRegs, VGT_GS_ONCHIP_CNTL__CI__VI, ES_VERTS_PER_SUBGRP,
                  inOutUsage.gs.calcFactor.esVertsPerSubgroup);

    SET_REG_FIELD(&config->gsRegs, VGT_GS_ONCHIP_CNTL__CI__VI, GS_PRIMS_PER_SUBGRP, gsPrimsPerSubgrp);

    SET_REG_FIELD(&config->gsRegs, VGT_ES_PER_GS, ES_PER_GS, inOutUsage.gs.calcFactor.esVertsPerSubgroup);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_PER_ES, GS_PER_ES, gsPrimsPerSubgrp);
  } else {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, ONCHIP__CI__VI, VGT_GS_MODE_ONCHIP_OFF);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, true);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);
    SET_REG(&config->gsRegs, VGT_GS_ONCHIP_CNTL__CI__VI, 0);

    SET_REG_FIELD(&config->gsRegs, VGT_ES_PER_GS, ES_PER_GS, EsThreadsPerGsThread);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_PER_ES, GS_PER_ES, std::min(maxGsPerEs, GsPrimsPerEsThread));
  }
  if (geometryMode.outputVertices <= 128) {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128);
  } else if (geometryMode.outputVertices <= 256) {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256);
  } else if (geometryMode.outputVertices <= 512) {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_512);
  } else {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_1024);
  }

  unsigned gsVertItemSize0 = sizeof(unsigned) * inOutUsage.gs.outLocCount[0];
  SET_REG_FIELD(&config->gsRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize0);

  unsigned gsVertItemSize1 = sizeof(unsigned) * inOutUsage.gs.outLocCount[1];
  SET_REG_FIELD(&config->gsRegs, VGT_GS_VERT_ITEMSIZE_1, ITEMSIZE, gsVertItemSize1);

  unsigned gsVertItemSize2 = sizeof(unsigned) * inOutUsage.gs.outLocCount[2];
  SET_REG_FIELD(&config->gsRegs, VGT_GS_VERT_ITEMSIZE_2, ITEMSIZE, gsVertItemSize2);

  unsigned gsVertItemSize3 = sizeof(unsigned) * inOutUsage.gs.outLocCount[3];
  SET_REG_FIELD(&config->gsRegs, VGT_GS_VERT_ITEMSIZE_3, ITEMSIZE, gsVertItemSize3);

  unsigned gsVsRingOffset = gsVertItemSize0 * maxVertOut;
  SET_REG_FIELD(&config->gsRegs, VGT_GSVS_RING_OFFSET_1, OFFSET, gsVsRingOffset);

  gsVsRingOffset += gsVertItemSize1 * maxVertOut;
  SET_REG_FIELD(&config->gsRegs, VGT_GSVS_RING_OFFSET_2, OFFSET, gsVsRingOffset);

  gsVsRingOffset += gsVertItemSize2 * maxVertOut;
  SET_REG_FIELD(&config->gsRegs, VGT_GSVS_RING_OFFSET_3, OFFSET, gsVsRingOffset);

  if (geometryMode.invocations > 1 || builtInUsage.invocationId) {
    SET_REG_FIELD(&config->gsRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
    SET_REG_FIELD(&config->gsRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
  }
  SET_REG_FIELD(&config->gsRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
  if (geometryMode.outputPrimitive == OutputPrimitives::Points)
    gsOutputPrimitiveType = POINTLIST;
  else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
    gsOutputPrimitiveType = LINESTRIP;

  SET_REG_FIELD(&config->gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);

  // Set multi-stream output primitive type
  if (gsVertItemSize1 > 0 || gsVertItemSize2 > 0 || gsVertItemSize3 > 0) {
    const static auto GsOutPrimInvalid = 3u;
    SET_REG_FIELD(&config->gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_1,
                  gsVertItemSize1 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);

    SET_REG_FIELD(&config->gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_2,
                  gsVertItemSize2 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);

    SET_REG_FIELD(&config->gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_3,
                  gsVertItemSize3 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);
  }

  SET_REG_FIELD(&config->gsRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, inOutUsage.gs.calcFactor.gsVsRingItemSize);

  setNumAvailSgprs(Util::Abi::HardwareStage::Gs, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Gs, resUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Builds register configuration for hardware pixel shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for pixel-shader-specific pipeline
template <typename T> void ConfigBuilder::buildPsRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageFragment);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.fs;
  const auto &fragmentMode = m_pipelineState->getShaderModes()->getFragmentShaderMode();

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP, true); // Follow PAL setting
  SET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE, shaderOptions.debugMode);

  SET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC2_PS, TRAP_PRESENT, shaderOptions.trapPresent);
  SET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR, intfData->userDataCount);

  SET_REG_FIELD(&config->psRegs, SPI_BARYC_CNTL, FRONT_FACE_ALL_BITS, true);
  if (fragmentMode.pixelCenterInteger) {
    // TRUE - Force floating point position to upper left corner of pixel (X.0, Y.0)
    SET_REG_FIELD(&config->psRegs, SPI_BARYC_CNTL, POS_FLOAT_ULC, true);
  } else if (builtInUsage.runAtSampleRate) {
    // 2 - Calculate per-pixel floating point position at iterated sample number
    SET_REG_FIELD(&config->psRegs, SPI_BARYC_CNTL, POS_FLOAT_LOCATION, 2);
  } else {
    // 0 - Calculate per-pixel floating point position at pixel center
    SET_REG_FIELD(&config->psRegs, SPI_BARYC_CNTL, POS_FLOAT_LOCATION, 0);
  }

  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, WALK_ALIGN8_PRIM_FITS_ST, true);
  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, WALK_FENCE_ENABLE, true);
  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, TILE_WALK_ORDER_ENABLE, true);
  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, PS_ITER_SAMPLE, builtInUsage.runAtSampleRate);

  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, SUPERTILE_WALK_ORDER_ENABLE, true);
  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE, true);
  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, FORCE_EOV_CNTDWN_ENABLE, true);
  SET_REG_FIELD(&config->psRegs, PA_SC_MODE_CNTL_1, FORCE_EOV_REZ_ENABLE, true);

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

  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, Z_ORDER, zOrder);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, KILL_ENABLE, builtInUsage.discard);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, Z_EXPORT_ENABLE, builtInUsage.fragDepth);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, STENCIL_TEST_VAL_EXPORT_ENABLE, builtInUsage.fragStencilRef);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, MASK_EXPORT_ENABLE, builtInUsage.sampleMask);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, ALPHA_TO_MASK_DISABLE, builtInUsage.sampleMask);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, DEPTH_BEFORE_SHADER, fragmentMode.earlyFragmentTests);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, EXEC_ON_NOOP,
                (fragmentMode.earlyFragmentTests && resUsage->resourceWrite));
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, EXEC_ON_HIER_FAIL, execOnHeirFail);

  unsigned depthExpFmt = EXP_FORMAT_ZERO;
  if (builtInUsage.sampleMask)
    depthExpFmt = EXP_FORMAT_32_ABGR;
  else if (builtInUsage.fragStencilRef)
    depthExpFmt = EXP_FORMAT_32_GR;
  else if (builtInUsage.fragDepth)
    depthExpFmt = EXP_FORMAT_32_R;
  SET_REG_FIELD(&config->psRegs, SPI_SHADER_Z_FORMAT, Z_EXPORT_FORMAT, depthExpFmt);

  unsigned cbShaderMask = resUsage->inOutUsage.fs.cbShaderMask;
  cbShaderMask = resUsage->inOutUsage.fs.isNullFs ? 0 : cbShaderMask;
  SET_REG(&config->psRegs, CB_SHADER_MASK, cbShaderMask);
  SET_REG_FIELD(&config->psRegs, SPI_PS_IN_CONTROL, NUM_INTERP, resUsage->inOutUsage.fs.interpInfo.size());

  unsigned pointCoordLoc = InvalidValue;

  auto builtInInputLocMapIt = resUsage->inOutUsage.builtInInputLocMap.find(BuiltInPointCoord);
  if (builtInInputLocMapIt != resUsage->inOutUsage.builtInInputLocMap.end()) {
    // Get generic input corresponding to gl_PointCoord (to set the field PT_SPRITE_TEX)
    pointCoordLoc = builtInInputLocMapIt->second;
  }

  // NOTE: PAL expects at least one mmSPI_PS_INPUT_CNTL_0 register set, so we always patch it at least one if none
  // were identified in the shader.
  const std::vector<FsInterpInfo> dummyInterpInfo{{0, false, false, false, false, false}};
  const auto &fsInterpInfo = resUsage->inOutUsage.fs.interpInfo;
  const auto *interpInfo = fsInterpInfo.size() == 0 ? &dummyInterpInfo : &fsInterpInfo;

  for (unsigned i = 0; i < interpInfo->size(); ++i) {
    const auto &interpInfoElem = (*interpInfo)[i];
    if (!m_pipelineState->isWholePipeline() && interpInfoElem.loc == InvalidFsInterpInfo.loc) {
      appendConfig(mmSPI_PS_INPUT_CNTL_0 + i, i);
      continue;
    }
    assert((interpInfoElem.loc == InvalidFsInterpInfo.loc && interpInfoElem.flat == InvalidFsInterpInfo.flat &&
            interpInfoElem.custom == InvalidFsInterpInfo.custom &&
            interpInfoElem.is16bit == InvalidFsInterpInfo.is16bit &&
            interpInfoElem.attr0Valid == InvalidFsInterpInfo.attr0Valid &&
            interpInfoElem.attr1Valid == InvalidFsInterpInfo.attr1Valid) == false);

    regSPI_PS_INPUT_CNTL_0 spiPsInputCntl = {};
    spiPsInputCntl.bits.FLAT_SHADE = interpInfoElem.flat;
    spiPsInputCntl.bits.OFFSET = interpInfoElem.loc;

    if (interpInfoElem.custom) {
      // NOTE: Force parameter cache data to be read in passthrough mode.
      static const unsigned PassThroughMode = (1 << 5);
      spiPsInputCntl.bits.FLAT_SHADE = true;
      spiPsInputCntl.bits.OFFSET |= PassThroughMode;
    } else if (!interpInfoElem.flat && interpInfoElem.is16bit) {
      spiPsInputCntl.bits.FP16_INTERP_MODE__VI = true;
      spiPsInputCntl.bits.ATTR0_VALID__VI = interpInfoElem.attr0Valid;
      spiPsInputCntl.bits.ATTR1_VALID__VI = interpInfoElem.attr1Valid;
    }

    constexpr unsigned UseDefaultVal = (1 << 5);
    if (pointCoordLoc == i) {
      spiPsInputCntl.bits.PT_SPRITE_TEX = true;

      // NOTE: Set the offset value to force hardware to select input defaults (no VS match).
      spiPsInputCntl.bits.OFFSET = UseDefaultVal;
    }

    // NOTE: Set SPI_PS_INPUT_CNTL_* here, but the register can still be changed later,
    // when it becomes known that gl_ViewportIndex is not used and fields OFFSET and FLAT_SHADE
    // can be amended.
    appendConfig(mmSPI_PS_INPUT_CNTL_0 + i, spiPsInputCntl.u32All);
  }

  if (pointCoordLoc != InvalidValue) {
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_ENA, true);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_X, SPI_PNT_SPRITE_SEL_S);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Y, SPI_PNT_SPRITE_SEL_T);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Z, SPI_PNT_SPRITE_SEL_0);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_W, SPI_PNT_SPRITE_SEL_1);
  }

  setPsUsesUavs(resUsage->resourceWrite || resUsage->resourceRead);
  setPsWritesUavs(resUsage->resourceWrite);
  setPsWritesDepth(builtInUsage.fragDepth);

  setNumAvailSgprs(Util::Abi::HardwareStage::Ps, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Ps, resUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Builds register configuration for compute shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for compute pipeline
void ConfigBuilder::buildCsRegConfig(ShaderStage shaderStage, CsRegConfig *config) {
  assert(shaderStage == ShaderStageCompute);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.cs;
  const auto &computeMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  unsigned workgroupSizes[3] = {};

  switch (static_cast<WorkgroupLayout>(builtInUsage.workgroupLayout)) {
  case WorkgroupLayout::Unknown:
  case WorkgroupLayout::Linear:
    workgroupSizes[0] = computeMode.workgroupSizeX;
    workgroupSizes[1] = computeMode.workgroupSizeY;
    workgroupSizes[2] = computeMode.workgroupSizeZ;
    break;
  case WorkgroupLayout::Quads:
  case WorkgroupLayout::SexagintiQuads:
    workgroupSizes[0] = computeMode.workgroupSizeX * computeMode.workgroupSizeY;
    workgroupSizes[1] = computeMode.workgroupSizeZ;
    workgroupSizes[2] = 1;
    break;
  }
  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC1, FLOAT_MODE, floatMode);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC1, DX10_CLAMP, true); // Follow PAL setting
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC1, DEBUG_MODE, shaderOptions.debugMode);

  // Set registers based on shader interface data
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, TRAP_PRESENT, shaderOptions.trapPresent);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, USER_SGPR, intfData->userDataCount);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, TGID_X_EN, true);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, TGID_Y_EN, true);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, TGID_Z_EN, true);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, TG_SIZE_EN, true);

  // 0 = X, 1 = XY, 2 = XYZ
  unsigned tidigCompCnt = 0;
  if (workgroupSizes[2] > 1)
    tidigCompCnt = 2;
  else if (workgroupSizes[1] > 1)
    tidigCompCnt = 1;
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC2, TIDIG_COMP_CNT, tidigCompCnt);

  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_X, NUM_THREAD_FULL, workgroupSizes[0]);
  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_Y, NUM_THREAD_FULL, workgroupSizes[1]);
  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_Z, NUM_THREAD_FULL, workgroupSizes[2]);

  setThreadgroupDimensions(workgroupSizes);

  setNumAvailSgprs(Util::Abi::HardwareStage::Cs, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Cs, resUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Sets up the register value for VGT_TF_PARAM.
//
// @param [out] config : Register configuration for the associated pipeline
template <typename T> void ConfigBuilder::setupVgtTfParam(T *config) {
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

  SET_REG_FIELD(config, VGT_TF_PARAM, TYPE, primType);
  SET_REG_FIELD(config, VGT_TF_PARAM, PARTITIONING, partition);
  SET_REG_FIELD(config, VGT_TF_PARAM, TOPOLOGY, topology);
}

} // namespace Gfx6

} // namespace lgc
