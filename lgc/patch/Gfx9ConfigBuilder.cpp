/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Gfx9ConfigBuilder.cpp
 * @brief LLPC header file: contains implementation of class lgc::Gfx9::ConfigBuilder.
 ***********************************************************************************************************************
 */
#include "Gfx9ConfigBuilder.h"
#include "lgc/BuiltIns.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

#define DEBUG_TYPE "lgc-gfx9-config-builder"

using namespace llvm;

namespace lgc {

namespace Gfx9 {

#include "chip/gfx9/gfx9_plus_merged_enum.h"
#include "chip/gfx9/gfx9_plus_merged_offset.h"

using namespace Pal::Gfx9::Chip;

// =====================================================================================================================
// Builds PAL metadata for pipeline.
void ConfigBuilder::buildPalMetadata() {
  if (!m_pipelineState->isGraphics()) {
    buildPipelineCsRegConfig();
  } else {
    const bool hasTs = (m_hasTcs || m_hasTes);
    const bool enableNgg = m_pipelineState->getNggControl()->enableNgg;

    if (m_hasTask) {
      // Task-Mesh-FS pipeline
      buildPipelineTaskMeshFsConfig();
    } else if (m_hasMesh) {
      // Mesh-FS pipeline
      buildPipelineMeshFsConfig();
    } else if (!hasTs && !m_hasGs) {
      // VS-FS pipeline or FS-only shader (part-pipeline compilation)
      if (m_gfxIp.major >= 10 && enableNgg)
        buildPipelineNggVsFsRegConfig();
      else
        buildPipelineVsFsRegConfig();
    } else if (hasTs && !m_hasGs) {
      // VS-TS-FS pipeline
      if (m_gfxIp.major >= 10 && enableNgg)
        buildPipelineNggVsTsFsRegConfig();
      else
        buildPipelineVsTsFsRegConfig();
    } else if (!hasTs && m_hasGs) {
      // VS-GS-FS pipeline
      if (m_gfxIp.major >= 10 && enableNgg)
        buildPipelineNggVsGsFsRegConfig();
      else
        buildPipelineVsGsFsRegConfig();
    } else {
      // VS-TS-GS-FS pipeline
      if (m_gfxIp.major >= 10 && enableNgg)
        buildPipelineNggVsTsGsFsRegConfig();
      else
        buildPipelineVsTsGsFsRegConfig();
    }
  }

  writePalMetadata();
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-FS) or FS-only shader compilation
void ConfigBuilder::buildPipelineVsFsRegConfig() {
  assert(m_gfxIp.major <= 10); // Must be GFX10 or below

  PipelineVsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex)) {
    setPipelineType(Util::Abi::PipelineType::VsPs);
    addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderVs);
    buildVsRegConfig<PipelineVsFsRegConfig>(ShaderStageVertex, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageVertex);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveSize);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    SET_REG(&config, VGT_GS_ONCHIP_CNTL, 0);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_MOST_FIELD(&config.vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
    }

    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
    // even if there are more than 2 shader engines on the GPU.
    unsigned primGroupSize = 128;
    unsigned numShaderEngines = m_pipelineState->getTargetInfo().getGpuProperty().numShaderEngines;
    if (numShaderEngines > 2)
      primGroupSize = alignTo(primGroupSize, 2);

    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    if (m_gfxIp.major == 10) {
      SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    } else {
      SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsFsRegConfig>(ShaderStageFragment, &config);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-FS).
void ConfigBuilder::buildPipelineVsTsFsRegConfig() {
  assert(m_gfxIp.major <= 10); // Must be GFX10 or below

  PipelineVsTsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Tess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);
  //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
  //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
  SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex) || m_pipelineState->hasShaderStage(ShaderStageTessControl)) {
    const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);

    buildLsHsRegConfig<PipelineVsTsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessEval)) {
    buildVsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageTessEval, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_DS);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessEval);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveSize);

    unsigned checksum = setShaderHash(ShaderStageTessEval);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG(&config.vsRegs, SPI_SHADER_PGM_CHKSUM_VS, checksum);
    }
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageFragment, &config);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
  const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

  if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId) {
    iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
  }

  if (m_gfxIp.major == 10) {
    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    SET_REG_FIELD(&config, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, EsVertsOffchipGsOrTess);
    SET_REG_FIELD(&config, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, GsPrimsOffchipGsOrTess);
    SET_REG_FIELD(&config, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, GsPrimsOffchipGsOrTess);
  } else {
    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-GS-FS).
void ConfigBuilder::buildPipelineVsGsFsRegConfig() {
  assert(m_gfxIp.major <= 10); // Must be GFX10 or below

  PipelineVsGsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Gs);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex) || m_pipelineState->hasShaderStage(ShaderStageGeometry)) {
    const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

    buildEsGsRegConfig<PipelineVsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageFragment, &config);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageCopyShader)) {
    buildVsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageCopyShader, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageCopyShader);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveSize);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const unsigned primGroupSize = 128;
  iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

  if (m_gfxIp.major == 10) {
    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
  } else {
    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-GS-FS).
void ConfigBuilder::buildPipelineVsTsGsFsRegConfig() {
  assert(m_gfxIp.major <= 10); // Must be GFX10 or below

  PipelineVsTsGsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::GsTess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex) || m_pipelineState->hasShaderStage(ShaderStageTessControl)) {
    const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);

    buildLsHsRegConfig<PipelineVsTsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                  hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveSize);

    //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
    //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessEval) || m_pipelineState->hasShaderStage(ShaderStageGeometry)) {
    const bool hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
    const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

    buildEsGsRegConfig<PipelineVsTsGsFsRegConfig>(hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                  hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageTessEval);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageFragment, &config);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageCopyShader)) {
    buildVsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageCopyShader, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageCopyShader);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, (waveSize == 32));

    if (m_gfxIp.major == 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveSize);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
  const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
  const auto &gsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

  // With tessellation, SWITCH_ON_EOI and PARTIAL_ES_WAVE_ON must be set if primitive ID is used by either the TCS, TES,
  // or GS.
  if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveIdIn)
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;

  if (m_gfxIp.major == 10) {
    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
  } else {
    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  // Set up VGT_TF_PARAM
  setupVgtTfParam(&config.lsHsRegs);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-FS) or FS-only shader compilation
void ConfigBuilder::buildPipelineNggVsFsRegConfig() {
  assert(m_gfxIp.major >= 10); // Must be GFX10 or above

  PipelineNggVsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  if (m_pipelineState->hasShaderStage(ShaderStageVertex)) {
    const auto nggControl = m_pipelineState->getNggControl();
    assert(nggControl->enableNgg);

    addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
    setPipelineType(Util::Abi::PipelineType::Ngg);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
    if (m_gfxIp.major <= 11) {
      SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, nggControl->passthroughMode);
    }
    if (m_gfxIp.major >= 11) {
      SET_REG_GFX10_4_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_NO_MSG,
                                 nggControl->passthroughMode && !m_pipelineState->enableSwXfb());
      SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, NGG_WAVE_ID_EN, m_pipelineState->enableSwXfb());
    }

    buildPrimShaderRegConfig<PipelineNggVsFsRegConfig>(ShaderStageVertex, ShaderStageInvalid, &config);

    if (m_gfxIp.major <= 11) {
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    }

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageVertex);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, (waveSize == 32));

    setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);

    unsigned checksum = setShaderHash(ShaderStageVertex);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4,
    // and must be even if there are more than 2 shader engines on the GPU.
    unsigned primGroupSize = 128;
    unsigned numShaderEngines = m_pipelineState->getTargetInfo().getGpuProperty().numShaderEngines;
    if (numShaderEngines > 2)
      primGroupSize = alignTo(primGroupSize, 2);

    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsFsRegConfig>(ShaderStageFragment, &config);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-FS).
void ConfigBuilder::buildPipelineNggVsTsFsRegConfig() {
  assert(m_gfxIp.major >= 10); // Must be GFX10 or above

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  PipelineNggVsTsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::NggTess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  if (m_gfxIp.major <= 11) {
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, nggControl->passthroughMode);
  }
  if (m_gfxIp.major >= 11) {
    SET_REG_GFX10_4_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_NO_MSG,
                               nggControl->passthroughMode && !m_pipelineState->enableSwXfb());
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, NGG_WAVE_ID_EN, m_pipelineState->enableSwXfb());
  }

  if (m_pipelineState->hasShaderStage(ShaderStageVertex) || m_pipelineState->hasShaderStage(ShaderStageTessControl)) {
    const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);

    buildLsHsRegConfig<PipelineNggVsTsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                   hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    if (m_gfxIp.major <= 11)
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, (waveSize == 32));

    setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessEval)) {
    buildPrimShaderRegConfig<PipelineNggVsTsFsRegConfig>(ShaderStageTessEval, ShaderStageInvalid, &config);

    if (m_gfxIp.major <= 11) {
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    }

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessEval);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, (waveSize == 32));

    setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);

    unsigned checksum = setShaderHash(ShaderStageTessEval);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsTsFsRegConfig>(ShaderStageFragment, &config);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;

  if (tcsBuiltInUsage.primitiveId)
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;

  SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-GS-FS).
void ConfigBuilder::buildPipelineNggVsGsFsRegConfig() {
  assert(m_gfxIp.major >= 10); // Must be GFX10 or above

  assert(m_pipelineState->getNggControl()->enableNgg);

  PipelineNggVsGsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Ngg);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  if (m_gfxIp.major <= 11) {
    // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
    // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
    // hardware pass-through).
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, false);
  }
  if (m_gfxIp.major >= 11)
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, NGG_WAVE_ID_EN, m_pipelineState->enableSwXfb());

  if (m_pipelineState->hasShaderStage(ShaderStageVertex) || m_pipelineState->hasShaderStage(ShaderStageGeometry)) {
    const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

    buildPrimShaderRegConfig<PipelineNggVsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                         hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    if (m_gfxIp.major <= 11) {
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    }
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, (waveSize == 32));

    setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsGsFsRegConfig>(ShaderStageFragment, &config);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const unsigned primGroupSize = 128;
  iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

  SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-GS-FS).
void ConfigBuilder::buildPipelineNggVsTsGsFsRegConfig() {
  assert(m_gfxIp.major >= 10); // Must be GFX10 or above

  assert(m_pipelineState->getNggControl()->enableNgg);

  PipelineNggVsTsGsFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::NggTess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  if (m_gfxIp.major <= 11) {
    // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
    // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
    // hardware pass-through).
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, false);
  }
  if (m_gfxIp.major >= 11)
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, NGG_WAVE_ID_EN, m_pipelineState->enableSwXfb());

  if (m_pipelineState->hasShaderStage(ShaderStageVertex) || m_pipelineState->hasShaderStage(ShaderStageTessControl)) {
    const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    const bool hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);

    buildLsHsRegConfig<PipelineNggVsTsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                     hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    if (m_gfxIp.major <= 11)
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, (waveSize == 32));

    setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageTessEval) || m_pipelineState->hasShaderStage(ShaderStageGeometry)) {
    const bool hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
    const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

    buildPrimShaderRegConfig<PipelineNggVsTsGsFsRegConfig>(hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                           hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageTessEval);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    if (m_gfxIp.major <= 11) {
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
      SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    }
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    const auto waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    SET_REG_GFX10_PLUS_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, (waveSize == 32));

    setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);
  }

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsTsGsFsRegConfig>(ShaderStageFragment, &config);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
  const auto &gsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

  if (tcsBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveIdIn)
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;

  SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

  // Set up VGT_TF_PARAM
  setupVgtTfParam(&config.lsHsRegs);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (Mesh-FS).
void ConfigBuilder::buildPipelineMeshFsConfig() {
  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  PipelineMeshFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageMesh, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Mesh);

  // Must contain mesh shader
  assert(m_pipelineState->hasShaderStage(ShaderStageMesh));
  buildMeshRegConfig<PipelineMeshFsRegConfig>(ShaderStageMesh, &config);

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineMeshFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);
    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling)
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (Task-Mesh-FS).
void ConfigBuilder::buildPipelineTaskMeshFsConfig() {
  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  PipelineTaskMeshFsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageTask, Util::Abi::HwShaderCs);
  addApiHwShaderMapping(ShaderStageMesh, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::TaskMesh);

  // Must contain task shader
  assert(m_pipelineState->hasShaderStage(ShaderStageTask));
  buildCsRegConfig(ShaderStageTask, &config.taskRegs);

  if (m_pipelineState->hasShaderStage(ShaderStageMesh))
    buildMeshRegConfig<PipelineTaskMeshFsRegConfig>(ShaderStageMesh, &config);

  if (m_pipelineState->hasShaderStage(ShaderStageFragment)) {
    buildPsRegConfig<PipelineTaskMeshFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);
    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling)
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for compute pipeline.
void ConfigBuilder::buildPipelineCsRegConfig() {
  assert(m_pipelineState->hasShaderStage(ShaderStageCompute));

  CsRegConfig config(m_gfxIp);

  addApiHwShaderMapping(ShaderStageCompute, Util::Abi::HwShaderCs);

  setPipelineType(Util::Abi::PipelineType::Cs);

  buildCsRegConfig(ShaderStageCompute, &config);

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

  assert(m_gfxIp.major <= 10); // Must be GFX10 or below

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage;

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP, true); // Follow PAL setting

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();
  const bool enableXfb = m_pipelineState->enableXfb();
  if (shaderStage == ShaderStageCopyShader) {
    // NOTE: For copy shader, usually we use fixed number of user data registers.
    // But in some cases, we may change user data registers, we use variable to keep user sgpr count here
    auto copyShaderUserSgprCount = lgc::CopyShaderUserSgprCount;
    SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, copyShaderUserSgprCount);
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
    const bool userSgprMsb = (intfData->userDataCount > 31);

    if (m_gfxIp.major == 10) {
      SET_REG_GFX10_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
    } else {
      SET_REG_GFX9_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
    }

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

  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_0, STRIDE, xfbStrides[0] / sizeof(unsigned));
  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_1, STRIDE, xfbStrides[1] / sizeof(unsigned));
  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_2, STRIDE, xfbStrides[2] / sizeof(unsigned));
  SET_REG_FIELD(&config->vsRegs, VGT_STRMOUT_VTX_STRIDE_3, STRIDE, xfbStrides[3] / sizeof(unsigned));

  unsigned streamBufferConfig = 0;
  for (auto i = 0; i < MaxGsStreams; ++i)
    streamBufferConfig |= (streamXfbBuffers[i] << (i * 4));
  SET_REG(&config->vsRegs, VGT_STRMOUT_BUFFER_CONFIG, streamBufferConfig);

  if (m_gfxIp.major == 10) {
    SET_REG_GFX10_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, MEM_ORDERED, true);
  }

  if (shaderStage == ShaderStageVertex) {
    if (builtInUsage.vs.instanceIndex) {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable instance ID
    } else if (builtInUsage.vs.primitiveId) {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
    }
  } else if (shaderStage == ShaderStageTessEval) {
    if (builtInUsage.tes.primitiveId) {
      // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable primitive ID
    } else {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
    }

    if (m_pipelineState->isTessOffChip()) {
      SET_REG_FIELD(&config->vsRegs, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN, true);
    }
  }

  setupPaSpecificRegisters(&config->vsRegs);
}

// =====================================================================================================================
// Builds register configuration for hardware local-hull merged shader.
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param [out] config : Register configuration for local-hull-shader-specific pipeline
template <typename T>
void ConfigBuilder::buildLsHsRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *config) {
  assert(shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageInvalid);
  assert(shaderStage2 == ShaderStageTessControl || shaderStage2 == ShaderStageInvalid);

  const auto tcsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  const auto &vsBuiltInUsage = vsResUsage->builtInUsage.vs;

  unsigned floatMode = setupFloatingPointMode(shaderStage2 != ShaderStageInvalid ? shaderStage2 : shaderStage1);
  SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DX10_CLAMP, true); // Follow PAL setting

  unsigned lsVgprCompCnt = 0;
  if (m_gfxIp.major <= 11) {
    if (vsBuiltInUsage.instanceIndex)
      lsVgprCompCnt = 3; // Enable all LS VGPRs (LS VGPR2 - VGPR5)
    else
      lsVgprCompCnt = 1; // Must enable relative vertex ID (LS VGPR2 and VGPR3)
  } else {
    llvm_unreachable("Not implemented!");
  }
  SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, LS_VGPR_COMP_CNT, lsVgprCompCnt);

  const auto &vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
  const auto &tcsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl);
  unsigned userDataCount = std::max(vsIntfData->userDataCount, tcsIntfData->userDataCount);

  const auto &tcsShaderOptions = m_pipelineState->getShaderOptions(ShaderStageTessControl);
  SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DEBUG_MODE, tcsShaderOptions.debugMode);

  const bool userSgprMsb = (userDataCount > 31);
  if (m_gfxIp.major >= 10) {
    bool wgpMode = (m_pipelineState->getShaderWgpMode(ShaderStageVertex) ||
                    m_pipelineState->getShaderWgpMode(ShaderStageTessControl));

    SET_REG_GFX10_PLUS_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, MEM_ORDERED, true);
    SET_REG_GFX10_PLUS_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, WGP_MODE, wgpMode);
    SET_REG_GFX10_PLUS_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);

    // The shared scratch offset is reused by HW to provide HS wave ID in group
    SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, SCRATCH_EN, m_pipelineState->canOptimizeTessFactor());
  } else {
    SET_REG_GFX9_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
  }
  SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, TRAP_PRESENT, tcsShaderOptions.trapPresent);
  SET_REG_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR, userDataCount);

  const auto &calcFactor = tcsResUsage->inOutUsage.tcs.calcFactor;
  assert(m_pipelineState->isTessOffChip()); // Must be off-chip on GFX9+

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  unsigned ldsSizeInDwords = calcFactor.tessOnChipLdsSize;
#if VKI_RAY_TRACING
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;
#endif
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  const unsigned ldsSize = ldsSizeInDwords >> ldsSizeDwordGranularityShift;
  if (m_gfxIp.major == 9) {
    SET_REG_GFX9_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
  } else {
    SET_REG_GFX10_PLUS_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
  }

  if (m_gfxIp.major >= 11) {
    // Pixel wait sync+
    const bool useImageOp = vsResUsage->useImageOp || tcsResUsage->useImageOp;
    SET_REG_GFX11_FIELD(&config->lsHsRegs, SPI_SHADER_PGM_RSRC4_HS, IMAGE_OP, useImageOp);
  }

  setLdsSizeByteSize(Util::Abi::HardwareStage::Hs, ldsSizeInDwords * 4);

  // Minimum and maximum tessellation factors supported by the hardware.
  constexpr float minTessFactor = 1.0f;
  constexpr float maxTessFactor = 64.0f;
  SET_REG(&config->lsHsRegs, VGT_HOS_MIN_TESS_LEVEL, bit_cast<uint32_t>(minTessFactor));
  SET_REG(&config->lsHsRegs, VGT_HOS_MAX_TESS_LEVEL, bit_cast<uint32_t>(maxTessFactor));

  // Set VGT_LS_HS_CONFIG
  SET_REG_FIELD(&config->lsHsRegs, VGT_LS_HS_CONFIG, NUM_PATCHES, calcFactor.patchCountPerThreadGroup);
  SET_REG_FIELD(&config->lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_INPUT_CP, m_pipelineState->getNumPatchControlPoints());

  auto hsNumOutputCp = m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;
  SET_REG_FIELD(&config->lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_OUTPUT_CP, hsNumOutputCp);

  setNumAvailSgprs(Util::Abi::HardwareStage::Hs, tcsResUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Hs, tcsResUsage->numVgprsAvailable);

  // Set up VGT_TF_PARAM
  setupVgtTfParam(&config->lsHsRegs);
}

// =====================================================================================================================
// Builds register configuration for hardware export-geometry merged shader.
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param [out] config : Register configuration for export-geometry-shader-specific pipeline
template <typename T>
void ConfigBuilder::buildEsGsRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *config) {
  assert(shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageTessEval ||
         shaderStage1 == ShaderStageInvalid);
  assert(shaderStage2 == ShaderStageGeometry || shaderStage2 == ShaderStageInvalid);

  assert(m_gfxIp.major <= 10); // Must be GFX10 or below

  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);

  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  const auto &vsBuiltInUsage = vsResUsage->builtInUsage.vs;

  const auto tesResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  const auto &tesBuiltInUsage = tesResUsage->builtInUsage.tes;

  const auto gsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  const auto &gsBuiltInUsage = gsResUsage->builtInUsage.gs;
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const auto &gsInOutUsage = gsResUsage->inOutUsage;
  const auto &calcFactor = gsInOutUsage.gs.calcFactor;

  unsigned gsVgprCompCnt = 0;
  if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
    gsVgprCompCnt = 3; // Enable vtx4/vtx5 offset (GS VGPR3) or GS instance ID (GS VGPR4)
  else if (gsBuiltInUsage.primitiveIdIn)
    gsVgprCompCnt = 2; // Enable primitive ID (GS VGPR2)
  else if (calcFactor.inputVertices > 2)
    gsVgprCompCnt = 1; // Enable vtx2/vtx3 offset (GS VGPR1)

  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT, gsVgprCompCnt);

  unsigned floatMode = setupFloatingPointMode(shaderStage2 != ShaderStageInvalid ? shaderStage2 : shaderStage1);
  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  unsigned userDataCount =
      std::max((hasTs ? tesIntfData->userDataCount : vsIntfData->userDataCount), gsIntfData->userDataCount);

  const auto &gsShaderOptions = m_pipelineState->getShaderOptions(ShaderStageGeometry);
  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, gsShaderOptions.debugMode);

  const bool userSgprMsb = (userDataCount > 31);
  if (m_gfxIp.major == 10) {
    bool wgpMode = m_pipelineState->getShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex) ||
                   m_pipelineState->getShaderWgpMode(ShaderStageGeometry);

    SET_REG_GFX10_PLUS_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
    SET_REG_GFX10_PLUS_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);
    SET_REG_GFX10_PLUS_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
  } else {
    SET_REG_GFX9_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
  }

  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, gsShaderOptions.trapPresent);
  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

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
  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT, esVgprCompCnt);

  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN, hasTs);

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
#if VKI_RAY_TRACING
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;
#endif
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  const unsigned ldsSize = ldsSizeInDwords >> ldsSizeDwordGranularityShift;
  SET_REG_FIELD(&config->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE, ldsSize);

  setLdsSizeByteSize(Util::Abi::HardwareStage::Gs, ldsSizeInDwords * 4);
  setEsGsLdsSize(calcFactor.esGsLdsSize * 4);

  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

  // TODO: Currently only support offchip GS
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);

  if (m_pipelineState->isGsOnChip()) {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_ON);
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, false);

    setEsGsLdsByteSize(calcFactor.esGsLdsSize * 4);
  } else {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);
  }

  if (geometryMode.outputVertices <= 128) {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128__HASHWVS);
  } else if (geometryMode.outputVertices <= 256) {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256__HASHWVS);
  } else if (geometryMode.outputVertices <= 512) {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_512__HASHWVS);
  } else {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_1024__HASHWVS);
  }

  SET_REG_FIELD(&config->esGsRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);

  // NOTE: The value of field "GS_INST_PRIMS_IN_SUBGRP" should be strictly equal to the product of
  // VGT_GS_ONCHIP_CNTL.GS_PRIMS_PER_SUBGRP * VGT_GS_INSTANCE_CNT.CNT.
  const unsigned gsInstPrimsInSubgrp =
      geometryMode.invocations > 1 ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations) : 0;
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

  unsigned gsVertItemSize0 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[0];
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize0);

  unsigned gsVertItemSize1 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[1];
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_VERT_ITEMSIZE_1, ITEMSIZE, gsVertItemSize1);

  unsigned gsVertItemSize2 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[2];
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_VERT_ITEMSIZE_2, ITEMSIZE, gsVertItemSize2);

  unsigned gsVertItemSize3 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[3];
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_VERT_ITEMSIZE_3, ITEMSIZE, gsVertItemSize3);

  unsigned gsVsRingOffset = gsVertItemSize0 * maxVertOut;
  SET_REG_FIELD(&config->esGsRegs, VGT_GSVS_RING_OFFSET_1, OFFSET, gsVsRingOffset);

  gsVsRingOffset += gsVertItemSize1 * maxVertOut;
  SET_REG_FIELD(&config->esGsRegs, VGT_GSVS_RING_OFFSET_2, OFFSET, gsVsRingOffset);

  gsVsRingOffset += gsVertItemSize2 * maxVertOut;
  SET_REG_FIELD(&config->esGsRegs, VGT_GSVS_RING_OFFSET_3, OFFSET, gsVsRingOffset);

  if (geometryMode.invocations > 1 || gsBuiltInUsage.invocationId) {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
  }
  SET_REG_FIELD(&config->esGsRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
  if (geometryMode.outputPrimitive == OutputPrimitives::Points)
    gsOutputPrimitiveType = POINTLIST;
  else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
    gsOutputPrimitiveType = LINESTRIP;

  SET_REG_FIELD(&config->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);

  // Set multi-stream output primitive type
  if (gsVertItemSize1 > 0 || gsVertItemSize2 > 0 || gsVertItemSize3 > 0) {
    const static auto GsOutPrimInvalid = 3u;
    SET_REG_GFX9_10_FIELD(&config->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_1,
                          gsVertItemSize1 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);

    SET_REG_GFX9_10_FIELD(&config->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_2,
                          gsVertItemSize2 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);

    SET_REG_GFX9_10_FIELD(&config->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_3,
                          gsVertItemSize3 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);
  }

  SET_REG_FIELD(&config->esGsRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, calcFactor.gsVsRingItemSize);
  SET_REG_FIELD(&config->esGsRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

  const unsigned maxPrimsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, MaxGsThreadsPerSubgroup);

  if (m_gfxIp.major == 9) {
    SET_REG_FIELD(&config->esGsRegs, VGT_GS_MAX_PRIMS_PER_SUBGROUP, MAX_PRIMS_PER_SUBGROUP, maxPrimsPerSubgroup);
  } else {
    SET_REG_FIELD(&config->esGsRegs, GE_MAX_OUTPUT_PER_SUBGROUP, MAX_VERTS_PER_SUBGROUP, maxPrimsPerSubgroup);
  }

  setNumAvailSgprs(Util::Abi::HardwareStage::Gs, gsResUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Gs, gsResUsage->numVgprsAvailable);
}

// =====================================================================================================================
// Builds register configuration for hardware primitive shader.
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param [out] config : Register configuration for primitive-shader-specific pipeline
template <typename T>
void ConfigBuilder::buildPrimShaderRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *config) {
  assert(shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageTessEval ||
         shaderStage1 == ShaderStageInvalid);
  assert(shaderStage2 == ShaderStageGeometry || shaderStage2 == ShaderStageInvalid);

  assert(m_gfxIp.major >= 10); // Must be GFX10 or above

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

  const auto vsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
  const auto &vsBuiltInUsage = vsResUsage->builtInUsage.vs;

  const auto tesResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
  const auto &tesBuiltInUsage = tesResUsage->builtInUsage.tes;

  const auto gsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  const auto &gsBuiltInUsage = gsResUsage->builtInUsage.gs;
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const auto &gsInOutUsage = gsResUsage->inOutUsage;
  const auto &calcFactor = gsInOutUsage.gs.calcFactor;

  //
  // Build ES-GS specific configuration
  //
  unsigned gsVgprCompCnt = 0;
  if (m_gfxIp.major <= 11) {
    if (hasGs) {
      if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
        gsVgprCompCnt = 3; // Enable vtx4/vtx5 offset (GS VGPR3) or GS instance ID (GS VGPR4)
      else if (gsBuiltInUsage.primitiveIdIn)
        gsVgprCompCnt = 2; // Enable primitive ID (GS VGPR2)
      else if (calcFactor.inputVertices > 2)
        gsVgprCompCnt = 1; // Enable vtx2/vtx3 offset (GS VGPR1)
    } else {
      // NOTE: When GS is absent, only those VGPRs are required: vtx0/vtx1 offset, vtx2/vtx3 offset,
      // primitive ID (only for VS).
      gsVgprCompCnt = 1;
      if (!hasTs && vsBuiltInUsage.primitiveId)
        gsVgprCompCnt = 2; // Enable primitive ID (GS VGPR2)
    }
  } else {
    llvm_unreachable("Not implemented!");
  }
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT, gsVgprCompCnt);

  unsigned floatMode = setupFloatingPointMode(shaderStage2 != ShaderStageInvalid ? shaderStage2 : shaderStage1);
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  unsigned userDataCount =
      std::max((hasTs ? tesIntfData->userDataCount : vsIntfData->userDataCount), gsIntfData->userDataCount);

  const auto &gsShaderOptions = m_pipelineState->getShaderOptions(ShaderStageGeometry);
  bool wgpMode = m_pipelineState->getShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex);
  if (hasGs)
    wgpMode = (wgpMode || m_pipelineState->getShaderWgpMode(ShaderStageGeometry));

  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, gsShaderOptions.debugMode);
  SET_REG_GFX10_PLUS_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
  SET_REG_GFX10_PLUS_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);

  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, gsShaderOptions.trapPresent);
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

  const bool userSgprMsb = (userDataCount > 31);

  SET_REG_GFX10_PLUS_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);

  unsigned esVgprCompCnt = 0;
  if (m_gfxIp.major <= 11) {
    if (hasTs) {
      if (tesBuiltInUsage.primitiveId)
        esVgprCompCnt = 3; // Enable patch ID (ES VGPR8)
      else
        esVgprCompCnt = 2; // Must enable relative patch ID (ES VGPR7)
    } else {
      if (vsBuiltInUsage.instanceIndex)
        esVgprCompCnt = 3; // Enable instance ID (ES VGPR8)
    }
  } else {

    llvm_unreachable("Not implemented!");
  }
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT, esVgprCompCnt);

  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN, hasTs);

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
#if VKI_RAY_TRACING
  ldsSizeInDwords += calcFactor.rayQueryLdsStackSize;
#endif
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  const unsigned ldsSize = ldsSizeInDwords >> ldsSizeDwordGranularityShift;
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE, ldsSize);
  setLdsSizeByteSize(Util::Abi::HardwareStage::Gs, ldsSizeInDwords * 4);
  setEsGsLdsSize(calcFactor.esGsLdsSize * 4);

  if (m_gfxIp.major >= 11) {
    // Pixel wait sync+
    bool useImageOp = hasGs ? gsResUsage->useImageOp : false;
    if (hasTs)
      useImageOp |= tesResUsage->useImageOp;
    else
      useImageOp |= vsResUsage->useImageOp;
    SET_REG_GFX11_FIELD(&config->primShaderRegs, SPI_SHADER_PGM_RSRC4_GS, IMAGE_OP, useImageOp);
  }

  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);

  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);
  setNggSubgroupSize(std::max(calcFactor.esVertsPerSubgroup, calcFactor.gsPrimsPerSubgroup));

  const unsigned gsInstPrimsInSubgrp = geometryMode.invocations > 1
                                           ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations)
                                           : calcFactor.gsPrimsPerSubgroup;
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

  unsigned gsVertItemSize = 4 * gsInOutUsage.outputMapLocCount;
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize);

  if (geometryMode.invocations > 1 || gsBuiltInUsage.invocationId) {
    SET_REG_FIELD(&config->primShaderRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
    SET_REG_FIELD(&config->primShaderRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
    if (m_gfxIp >= GfxIpVersion{10, 1}) {
      SET_REG_GFX10_PLUS_FIELD(&config->primShaderRegs, VGT_GS_INSTANCE_CNT, EN_MAX_VERT_OUT_PER_GS_INSTANCE,
                               calcFactor.enableMaxVertOut);
    }
  }
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = POINTLIST;
  if (hasGs) {
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

  // TODO: Multiple output streams are not supported.
  SET_REG_FIELD(&config->primShaderRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);
  if (m_gfxIp.major <= 11) {
    SET_REG_FIELD(&config->primShaderRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, calcFactor.gsVsRingItemSize);
    // NOTE: When GS is absent, always set ES-GS ring item size to 1. Thus, we can easily get vertex ID in subgroup
    // without any additional calculations.
    SET_REG_FIELD(&config->primShaderRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, hasGs ? calcFactor.esGsRingItemSize : 1);
  }

  const unsigned maxVertsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, NggMaxThreadsPerSubgroup);
  SET_REG_FIELD(&config->primShaderRegs, GE_MAX_OUTPUT_PER_SUBGROUP, MAX_VERTS_PER_SUBGROUP, maxVertsPerSubgroup);

  if (hasGs) {
    setNumAvailSgprs(Util::Abi::HardwareStage::Gs, gsResUsage->numSgprsAvailable);
    setNumAvailVgprs(Util::Abi::HardwareStage::Gs, gsResUsage->numVgprsAvailable);
  } else {
    if (hasTs) {
      setNumAvailSgprs(Util::Abi::HardwareStage::Gs, tesResUsage->numSgprsAvailable);
      setNumAvailVgprs(Util::Abi::HardwareStage::Gs, tesResUsage->numVgprsAvailable);
    } else {
      setNumAvailSgprs(Util::Abi::HardwareStage::Gs, vsResUsage->numSgprsAvailable);
      setNumAvailVgprs(Util::Abi::HardwareStage::Gs, vsResUsage->numVgprsAvailable);
    }
  }

  //
  // Build VS specific configuration
  //
  setupPaSpecificRegisters(&config->primShaderRegs);

  //
  // Build NGG configuration
  //
  assert(calcFactor.primAmpFactor >= 1);
  SET_REG_FIELD(&config->primShaderRegs, GE_NGG_SUBGRP_CNTL, PRIM_AMP_FACTOR, calcFactor.primAmpFactor);
  SET_REG_FIELD(&config->primShaderRegs, GE_NGG_SUBGRP_CNTL, THDS_PER_SUBGRP, NggMaxThreadsPerSubgroup);

  // TODO: Support PIPELINE_PRIM_ID.
  SET_REG_FIELD(&config->primShaderRegs, SPI_SHADER_IDX_FORMAT, IDX0_EXPORT_FORMAT, SPI_SHADER_1COMP);

  if (nggControl->passthroughMode) {
    INVALIDATE_REG(&config->primShaderRegs, SPI_SHADER_PGM_LO_GS);
  } else {
    // NOTE: For NGG culling mode, the primitive shader table that contains culling data might be accessed by
    // shader. PAL expects 64-bit address of that table and will program it into SPI_SHADER_PGM_LO_GS and
    // SPI_SHADER_PGM_HI_GS if we do not provide one. By setting SPI_SHADER_PGM_LO_GS to NggCullingData, we tell
    // PAL that we will not provide it and it is fine to use SPI_SHADER_PGM_LO_GS and SPI_SHADER_PGM_HI_GS as
    // the address of that table.
    SET_REG(&config->primShaderRegs, SPI_SHADER_PGM_LO_GS, static_cast<unsigned>(UserDataMapping::NggCullingData));
  }

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

  const bool userSgprMsb = (intfData->userDataCount > 31);

  if (m_gfxIp.major >= 10) {
    SET_REG_GFX10_PLUS_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC1_PS, MEM_ORDERED, true);
    SET_REG_MOST_FIELD(&config->psRegs, PA_STEREO_CNTL, STEREO_MODE, STATE_STEREO_X);
    SET_REG_GFX10_PLUS_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
  } else {
    SET_REG_GFX9_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
  }

  if (m_gfxIp.major >= 11) {
    // Pixel wait sync+
    SET_REG_GFX11_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC4_PS, IMAGE_OP, resUsage->useImageOp);
  }

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

  ConservativeZExport conservativeZExport = EXPORT_ANY_Z;
  if (fragmentMode.conservativeDepth == ConservativeDepth::LessEqual)
    conservativeZExport = EXPORT_LESS_THAN_Z;
  else if (fragmentMode.conservativeDepth == ConservativeDepth::GreaterEqual)
    conservativeZExport = EXPORT_GREATER_THAN_Z;

  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, Z_ORDER, zOrder);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, KILL_ENABLE, builtInUsage.discard);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, Z_EXPORT_ENABLE, builtInUsage.fragDepth);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, STENCIL_TEST_VAL_EXPORT_ENABLE, builtInUsage.fragStencilRef);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, MASK_EXPORT_ENABLE, builtInUsage.sampleMask);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, ALPHA_TO_MASK_DISABLE, 1); // Set during pipeline finalization.
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, DEPTH_BEFORE_SHADER, fragmentMode.earlyFragmentTests);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, EXEC_ON_NOOP,
                (fragmentMode.earlyFragmentTests && resUsage->resourceWrite));
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, EXEC_ON_HIER_FAIL, execOnHeirFail);
  SET_REG_FIELD(&config->psRegs, DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT, conservativeZExport);

  if (m_gfxIp.major >= 10) {
    SET_REG_GFX10_PLUS_FIELD(&config->psRegs, DB_SHADER_CONTROL, PRE_SHADER_DEPTH_COVERAGE_ENABLE,
                             fragmentMode.postDepthCoverage);
  }

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

  const auto waveSize = m_pipelineState->getShaderWaveSize(shaderStage);
  SET_REG_GFX10_PLUS_FIELD(&config->psRegs, SPI_PS_IN_CONTROL, PS_W32_EN, (waveSize == 32));

  if (m_gfxIp.major >= 10)
    setWaveFrontSize(Util::Abi::HardwareStage::Ps, waveSize);

  unsigned pointCoordLoc = InvalidValue;

  auto builtInInputLocMapIt = resUsage->inOutUsage.builtInInputLocMap.find(BuiltInPointCoord);
  if (builtInInputLocMapIt != resUsage->inOutUsage.builtInInputLocMap.end()) {
    // Get generic input corresponding to gl_PointCoord (to set the field PT_SPRITE_TEX)
    pointCoordLoc = builtInInputLocMapIt->second;
  }

  // NOTE: PAL expects at least one mmSPI_PS_INPUT_CNTL_0 register set, so we always patch it at least one if none
  // were identified in the shader.
  const std::vector<FsInterpInfo> dummyInterpInfo{{0, false, false, false, false, false, false}};
  const auto &fsInterpInfo = resUsage->inOutUsage.fs.interpInfo;
  const auto *interpInfo = fsInterpInfo.size() == 0 ? &dummyInterpInfo : &fsInterpInfo;

  unsigned numPrimInterp = 0;
  for (unsigned i = 0; i < interpInfo->size(); ++i) {
    auto interpInfoElem = (*interpInfo)[i];

    if (interpInfoElem.isPerPrimitive)
      ++numPrimInterp;

    if (!m_pipelineState->isWholePipeline() && interpInfoElem.loc == InvalidFsInterpInfo.loc) {
      appendConfig(mmSPI_PS_INPUT_CNTL_0 + i, i);
      continue;
    }
    if ((interpInfoElem.loc == InvalidFsInterpInfo.loc && interpInfoElem.flat == InvalidFsInterpInfo.flat &&
         interpInfoElem.custom == InvalidFsInterpInfo.custom && interpInfoElem.is16bit == InvalidFsInterpInfo.is16bit))
      interpInfoElem.loc = i;

    regSPI_PS_INPUT_CNTL_0 spiPsInputCntl = {};
    // NOTE: Flat shading flag is only set for per-vertex parameter.
    spiPsInputCntl.bits.FLAT_SHADE = interpInfoElem.flat && !interpInfoElem.isPerPrimitive;
    spiPsInputCntl.bits.OFFSET = interpInfoElem.loc;
    if (m_gfxIp.major >= 11 && interpInfoElem.isPerPrimitive) {
      // NOTE: HW allocates and manages attribute ring based on the register fields: VS_EXPORT_COUNT and
      // PRIM_EXPORT_COUNT. When VS_EXPORT_COUNT = 0, HW assumes there is still a vertex attribute exported even
      // though this is not what we want. Hence, we should reserve param0 as a dummy vertex attribute and all
      // primitive attributes are moved after it.
      const bool hasNoVertexAttrib = resUsage->inOutUsage.inputMapLocCount == 0; // No vertex attribute
      if (hasNoVertexAttrib)
        ++spiPsInputCntl.bits.OFFSET;

      spiPsInputCntl.gfx11.PRIM_ATTR = true;
    }

    if (interpInfoElem.custom) {
      // NOTE: Force parameter cache data to be read in passthrough mode.
      static const unsigned PassThroughMode = (1 << 5);
      spiPsInputCntl.bits.FLAT_SHADE = true;
      spiPsInputCntl.bits.OFFSET |= PassThroughMode;
    } else if (!interpInfoElem.flat && interpInfoElem.is16bit) {
      spiPsInputCntl.bits.FP16_INTERP_MODE = true;
      spiPsInputCntl.bits.ATTR0_VALID = interpInfoElem.attr0Valid;
      spiPsInputCntl.bits.ATTR1_VALID = interpInfoElem.attr1Valid;
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

  unsigned numInterp = resUsage->inOutUsage.fs.interpInfo.size() - numPrimInterp;
  if (m_gfxIp.major >= 11) {
    // NOTE: For GFX11+, vertex attributes and primitive attributes are counted together. The field
    // SPI_PS_INPUT_CNTL.PRIM_ATTR is used to differentiate them.
    numInterp = resUsage->inOutUsage.fs.interpInfo.size();
  }
  SET_REG_FIELD(&config->psRegs, SPI_PS_IN_CONTROL, NUM_INTERP, numInterp);
  if (m_gfxIp.isGfx(10, 3))
    SET_REG_GFX10_3_PLUS_EXCLUSIVE_FIELD(&config->psRegs, SPI_PS_IN_CONTROL, NUM_PRIM_INTERP, numPrimInterp);

  if (pointCoordLoc != InvalidValue) {
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_ENA, true);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_X, SPI_PNT_SPRITE_SEL_S);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Y, SPI_PNT_SPRITE_SEL_T);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Z, SPI_PNT_SPRITE_SEL_0);
    SET_REG_FIELD(&config->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_W, SPI_PNT_SPRITE_SEL_1);
  }

  if (m_pipelineState->getPalAbiVersion() >= 456) {
    setPsUsesUavs(resUsage->resourceWrite || resUsage->resourceRead);
    setPsWritesUavs(resUsage->resourceWrite);
    setPsWritesDepth(builtInUsage.fragDepth);
  } else
    setPsUsesUavs(static_cast<unsigned>(resUsage->resourceWrite));

  setPsSampleMask(builtInUsage.sampleMaskIn | builtInUsage.sampleMask);

  const unsigned loadCollisionWaveId = GET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC2_PS, LOAD_COLLISION_WAVEID);
  const unsigned loadIntrawaveCollision =
      GET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);

  SET_REG_CORE_FIELD(&config->psRegs, PA_SC_SHADER_CONTROL, LOAD_COLLISION_WAVEID, loadCollisionWaveId);
  SET_REG_CORE_FIELD(&config->psRegs, PA_SC_SHADER_CONTROL, LOAD_INTRAWAVE_COLLISION, loadIntrawaveCollision);

  setNumAvailSgprs(Util::Abi::HardwareStage::Ps, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Ps, resUsage->numVgprsAvailable);

  const unsigned checksum = setShaderHash(shaderStage);
  if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling)
    SET_REG_FIELD(&config->psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
}

// =====================================================================================================================
// Builds register configuration for mesh shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for mesh-shader-specific pipeline
template <typename T> void ConfigBuilder::buildMeshRegConfig(ShaderStage shaderStage, T *config) {
  assert(shaderStage == ShaderStageMesh);

  assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);

  const auto &builtInUsage = resUsage->builtInUsage.mesh;

  const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();
  const auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

  SET_REG_FIELD(&config->meshRegs, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config->meshRegs, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(shaderStage);
  if (waveSize == 32)
    SET_REG_GFX10_PLUS_FIELD(&config->meshRegs, VGT_SHADER_STAGES_EN, GS_W32_EN, true);

  if (m_gfxIp.major <= 11)
    SET_REG_FIELD(&config->meshRegs, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
  SET_REG_FIELD(&config->meshRegs, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
  const unsigned gsFastLaunch = m_gfxIp.major == 11 ? 0x2 : 0x1; // GFX11 sets GS fast launch mode to 0x2
  SET_REG_GFX09_1X_PLUS_FIELD(&config->meshRegs, VGT_SHADER_STAGES_EN, GS_FAST_LAUNCH, gsFastLaunch);

  //
  // Build ES-GS specific configuration
  //
  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

  unsigned userDataCount = intfData->userDataCount;

  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  bool wgpMode = m_pipelineState->getShaderWgpMode(shaderStage);

  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, shaderOptions.debugMode);
  SET_REG_GFX10_PLUS_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
  SET_REG_GFX10_PLUS_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);

  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, shaderOptions.trapPresent);
  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

  const bool userSgprMsb = (userDataCount > 31);
  SET_REG_GFX10_PLUS_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);

  const unsigned ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
  const unsigned ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;

  unsigned ldsSizeInDwords = calcFactor.gsOnChipLdsSize;
  ldsSizeInDwords = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity);

  const unsigned ldsSize = ldsSizeInDwords >> ldsSizeDwordGranularityShift;
  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE, ldsSize);
  setLdsSizeByteSize(Util::Abi::HardwareStage::Gs, ldsSizeInDwords * 4);

  if (m_gfxIp.major >= 11) {
    // Pixel wait sync+
    SET_REG_GFX11_FIELD(&config->meshRegs, SPI_SHADER_PGM_RSRC4_GS, IMAGE_OP, resUsage->useImageOp);
  }

  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(meshMode.outputVertices));
  SET_REG_FIELD(&config->meshRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

  SET_REG_FIELD(&config->meshRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
  SET_REG_FIELD(&config->meshRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
  SET_REG_FIELD(&config->meshRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
  SET_REG_FIELD(&config->meshRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);

  assert(calcFactor.esVertsPerSubgroup == 1 && calcFactor.gsPrimsPerSubgroup == 1);
  SET_REG_FIELD(&config->meshRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, 1);
  SET_REG_FIELD(&config->meshRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, 1);
  SET_REG_FIELD(&config->meshRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, 1);
  setNggSubgroupSize(1);

  SET_REG_FIELD(&config->meshRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = POINTLIST;
  switch (meshMode.outputPrimitive) {
  case OutputPrimitives::Points:
    gsOutputPrimitiveType = POINTLIST;
    break;
  case OutputPrimitives::Lines:
    gsOutputPrimitiveType = LINESTRIP;
    break;
  case OutputPrimitives::Triangles:
    gsOutputPrimitiveType = TRISTRIP;
    break;
  default:
    llvm_unreachable("Unknown primitive type!");
    break;
  }
  SET_REG_FIELD(&config->meshRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);

  const unsigned maxVertsPerSubgroup = std::min(meshMode.outputVertices, NggMaxThreadsPerSubgroup);
  SET_REG_FIELD(&config->meshRegs, GE_MAX_OUTPUT_PER_SUBGROUP, MAX_VERTS_PER_SUBGROUP, maxVertsPerSubgroup);

  //
  // Build VS specific configuration
  //
  setupPaSpecificRegisters(&config->meshRegs);

  //
  // Build NGG specific configuration
  //
  assert(calcFactor.primAmpFactor >= 1);
  SET_REG_FIELD(&config->meshRegs, GE_NGG_SUBGRP_CNTL, PRIM_AMP_FACTOR, calcFactor.primAmpFactor);
  SET_REG_FIELD(&config->meshRegs, GE_NGG_SUBGRP_CNTL, THDS_PER_SUBGRP, calcFactor.primAmpFactor);

  const bool enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;
  bool hasPrimitivePayload =
      builtInUsage.layer || builtInUsage.viewportIndex || builtInUsage.primitiveShadingRate || enableMultiView;
  if (m_gfxIp.major < 11)
    hasPrimitivePayload |= builtInUsage.primitiveId;
  SET_REG_FIELD(&config->meshRegs, SPI_SHADER_IDX_FORMAT, IDX0_EXPORT_FORMAT,
                hasPrimitivePayload ? SPI_SHADER_2COMP : SPI_SHADER_1COMP);
  SET_REG_GFX10_PLUS_FIELD(&config->meshRegs, VGT_DRAW_PAYLOAD_CNTL, EN_PRIM_PAYLOAD, hasPrimitivePayload);

  if (m_gfxIp.major >= 11) {
    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_GS_MESHLET_DIM, MESHLET_NUM_THREAD_X, meshMode.workgroupSizeX - 1);
    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_GS_MESHLET_DIM, MESHLET_NUM_THREAD_Y, meshMode.workgroupSizeY - 1);
    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_GS_MESHLET_DIM, MESHLET_NUM_THREAD_Z, meshMode.workgroupSizeZ - 1);
    // NOTE: If row export for mesh shader is enabled, the thread group size is set according to dimensions of work
    // group. Otherwise, it is set according to actual primitive amplification factor.
    const unsigned threadGroupSize = m_pipelineState->enableMeshRowExport()
                                         ? meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ
                                         : calcFactor.primAmpFactor;
    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_GS_MESHLET_DIM, MESHLET_THREADGROUP_SIZE, threadGroupSize - 1);

    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_GS_MESHLET_EXP_ALLOC, MAX_EXP_VERTS, meshMode.outputVertices);
    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_GS_MESHLET_EXP_ALLOC, MAX_EXP_PRIMS, meshMode.outputPrimitives);
  }

  setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveSize);

  setNumAvailSgprs(Util::Abi::HardwareStage::Gs, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Gs, resUsage->numVgprsAvailable);

  const unsigned checksum = setShaderHash(ShaderStageMesh);
  if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling)
    SET_REG_FIELD(&config->meshRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);

  //
  // Set up IA_MULTI_VGT_PARAM
  //
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  static constexpr unsigned PrimGroupSize = 128;
  iaMultiVgtParam.bits.PRIMGROUP_SIZE = PrimGroupSize - 1;

  SET_REG(&config->meshRegs, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
}

// =====================================================================================================================
// Builds register configuration for compute/task shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for compute
void ConfigBuilder::buildCsRegConfig(ShaderStage shaderStage, CsRegConfig *config) {
  assert(shaderStage == ShaderStageCompute || shaderStage == ShaderStageTask);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
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

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC1, FLOAT_MODE, floatMode);
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC1, DX10_CLAMP, true); // Follow PAL setting
  SET_REG_FIELD(config, COMPUTE_PGM_RSRC1, DEBUG_MODE, shaderOptions.debugMode);

  if (m_gfxIp.major >= 10) {
    bool wgpMode = m_pipelineState->getShaderWgpMode(shaderStage);

    SET_REG_GFX10_PLUS_FIELD(config, COMPUTE_PGM_RSRC1, MEM_ORDERED, true);
    SET_REG_GFX10_PLUS_FIELD(config, COMPUTE_PGM_RSRC1, WGP_MODE, wgpMode);
    unsigned waveSize = m_pipelineState->getShaderWaveSize(shaderStage);
    assert(waveSize == 32 || waveSize == 64);
    setWaveFrontSize(Util::Abi::HardwareStage::Cs, waveSize);
  }

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

  if (m_gfxIp.major >= 11) {
    // Pixel wait sync+
    SET_REG_GFX11_FIELD(config, COMPUTE_PGM_RSRC3, IMAGE_OP, resUsage->useImageOp);
  }

  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_X, NUM_THREAD_FULL, workgroupSizes[0]);
  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_Y, NUM_THREAD_FULL, workgroupSizes[1]);
  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_Z, NUM_THREAD_FULL, workgroupSizes[2]);

  setThreadgroupDimensions(workgroupSizes);

  setNumAvailSgprs(Util::Abi::HardwareStage::Cs, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Cs, resUsage->numVgprsAvailable);

  const unsigned checksum = setShaderHash(shaderStage);
  if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling)
    SET_REG_FIELD(config, COMPUTE_SHADER_CHKSUM, CHECKSUM, checksum);
}

// =====================================================================================================================
// Sets up the register value for VGT_TF_PARAM.
//
// @param [out] config : Register configuration for local-hull-shader-specific pipeline
void ConfigBuilder::setupVgtTfParam(LsHsRegConfig *config) {
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

  if (m_pipelineState->isTessOffChip()) {
    SET_REG_FIELD(config, VGT_TF_PARAM, DISTRIBUTION_MODE, TRAPEZOIDS);
  }
}

// =====================================================================================================================
// Set up PA-specific (primitive assembler) registers.
//
// @param [out] config : Register configuration
template <typename T> void ConfigBuilder::setupPaSpecificRegisters(T *config) {
  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
  const bool meshPipeline =
      m_pipelineState->hasShaderStage(ShaderStageTask) || m_pipelineState->hasShaderStage(ShaderStageMesh);

  uint8_t usrClipPlaneMask = m_pipelineState->getRasterizerState().usrClipPlaneMask;
  bool rasterizerDiscardEnable = m_pipelineState->getRasterizerState().rasterizerDiscardEnable;

  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, UCP_ENA_0, (usrClipPlaneMask >> 0) & 0x1);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, UCP_ENA_1, (usrClipPlaneMask >> 1) & 0x1);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, UCP_ENA_2, (usrClipPlaneMask >> 2) & 0x1);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, UCP_ENA_3, (usrClipPlaneMask >> 3) & 0x1);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, UCP_ENA_4, (usrClipPlaneMask >> 4) & 0x1);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, UCP_ENA_5, (usrClipPlaneMask >> 5) & 0x1);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA, true);
  SET_REG_FIELD(config, PA_CL_CLIP_CNTL, DX_RASTERIZATION_KILL, rasterizerDiscardEnable);

  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VPORT_X_SCALE_ENA, true);
  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VPORT_X_OFFSET_ENA, true);
  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VPORT_Y_SCALE_ENA, true);
  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VPORT_Y_OFFSET_ENA, true);
  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VPORT_Z_SCALE_ENA, true);
  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VPORT_Z_OFFSET_ENA, true);
  SET_REG_FIELD(config, PA_CL_VTE_CNTL, VTX_W0_FMT, true);

  SET_REG_FIELD(config, PA_SU_VTX_CNTL, PIX_CENTER, 1);
  SET_REG_FIELD(config, PA_SU_VTX_CNTL, ROUND_MODE, 2); // Round to even
  SET_REG_FIELD(config, PA_SU_VTX_CNTL, QUANT_MODE, 5); // Use 8-bit fractions

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

    if (hasGs) {
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
      SET_REG_FIELD(config, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, true);

      if (m_pipelineState->getNggControl()->enableNgg) {
        // NOTE: If primitive ID is used and there is no GS present, the field NGG_DISABLE_PROVOK_REUSE must be
        // set to ensure provoking vertex reuse is disabled in the GE.
        if (!m_hasGs) {
          SET_REG_FIELD(config, VGT_PRIMITIVEID_EN, NGG_DISABLE_PROVOK_REUSE, true);
        }
      }
    }
  }

  if (expCount == 0 && primExpCount == 0) {
    // No generic output is present
    SET_REG_GFX10_PLUS_FIELD(config, SPI_VS_OUT_CONFIG, NO_PC_EXPORT, true);
  } else {
    if (expCount > 0)
      SET_REG_FIELD(config, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, expCount - 1);

    if (primExpCount > 0) {
      assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
      SET_REG_GFX10_3_PLUS_EXCLUSIVE_FIELD(config, SPI_VS_OUT_CONFIG, PRIM_EXPORT_COUNT, primExpCount);
    }
  }

  setUsesViewportArrayIndex(useViewportIndex);

  bool disableVertexReuse = m_pipelineState->getInputAssemblyState().disableVertexReuse;
  disableVertexReuse |= meshPipeline; // Mesh pipeline always disable vertex reuse

  // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
  // index if each GS, TES, or VS invocation emits the same viewport array index for each vertex and we set
  // VTE_VPORT_PROVOKE_DISABLE.
  if (useViewportIndex) {
    // TODO: In the future, we can only disable vertex reuse only if viewport array index is emitted divergently
    // for each vertex.
    disableVertexReuse = true;
    SET_REG_FIELD(config, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, true);
  } else {
    SET_REG_FIELD(config, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, false);
  }

  SET_REG_FIELD(config, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse || m_pipelineState->enableSwXfb());

  bool miscExport = usePointSize;
  if (!meshPipeline) {
    // NOTE: Those built-ins are exported through primitive payload for mesh pipeline rather than vertex position data.
    miscExport |= useLayer || useViewportIndex || useShadingRate;
  }

  if (miscExport) {
    SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);

    if (meshPipeline) {
      if (useShadingRate) {
        assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
        SET_REG_GFX10_3_PLUS_FIELD(config, PA_CL_VS_OUT_CNTL, BYPASS_VTX_RATE_COMBINER, true);
      }
    } else {
      // NOTE: Those built-ins are exported through primitive payload for mesh pipeline rather than vertex position
      // data.
      SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
      SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);

      if (useShadingRate) {
        assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
        SET_REG_GFX10_3_PLUS_FIELD(config, PA_CL_VS_OUT_CNTL, USE_VTX_VRS_RATE, true);
        SET_REG_GFX10_3_PLUS_FIELD(config, PA_CL_VS_OUT_CNTL, BYPASS_PRIM_RATE_COMBINER, true);
      }
    }

    SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
    SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);
  }

  if (clipDistanceCount > 0 || cullDistanceCount > 0) {
    SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST0_VEC_ENA, true);
    if (clipDistanceCount + cullDistanceCount > 4) {
      SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST1_VEC_ENA, true);
    }

    unsigned clipDistanceMask = (1 << clipDistanceCount) - 1;
    unsigned cullDistanceMask = ((1 << cullDistanceCount) - 1) << clipDistanceCount;

    // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
    unsigned paClVsOutCntl = GET_REG(config, PA_CL_VS_OUT_CNTL);

    // Note: Point primitives are only affected by the cull mask, so enable culling also based on clip distances
    unsigned finalMask = clipDistanceMask | cullDistanceMask;
    paClVsOutCntl |= clipDistanceMask | (finalMask << 8);
    SET_REG(config, PA_CL_VS_OUT_CNTL, paClVsOutCntl);

    // On 10.3+ all auxiliary position exports are optimized, not just the misc exports.
    if (m_gfxIp >= GfxIpVersion{10, 3})
      SET_REG_FIELD(config, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);
  }

  unsigned posCount = 1; // gl_Position is always exported
  if (miscExport)
    ++posCount;

  if (clipDistanceCount + cullDistanceCount > 0) {
    ++posCount;
    if (clipDistanceCount + cullDistanceCount > 4)
      ++posCount;
  }

  SET_REG_FIELD(config, SPI_SHADER_POS_FORMAT, POS0_EXPORT_FORMAT, SPI_SHADER_4COMP);
  if (posCount > 1) {
    SET_REG_FIELD(config, SPI_SHADER_POS_FORMAT, POS1_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 2) {
    SET_REG_FIELD(config, SPI_SHADER_POS_FORMAT, POS2_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 3) {
    SET_REG_FIELD(config, SPI_SHADER_POS_FORMAT, POS3_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
}

} // namespace Gfx9

} // namespace lgc
