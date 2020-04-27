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
 * @file  Gfx9ConfigBuilder.cpp
 * @brief LLPC header file: contains implementation of class lgc::Gfx9::ConfigBuilder.
 ***********************************************************************************************************************
 */
#include "Gfx9ConfigBuilder.h"
#include "lgc/BuiltIns.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-gfx9-config-builder"

namespace llvm {

namespace cl {

extern opt<bool> InRegEsGsLdsSize;

} // namespace cl

} // namespace llvm

using namespace llvm;

namespace lgc {

namespace Gfx9 {

#include "chip/gfx9/gfx9_plus_merged_enum.h"
#include "chip/gfx9/gfx9_plus_merged_offset.h"

using namespace Pal::Gfx9::Chip;

// =====================================================================================================================
// Builds PAL metadata for pipeline.
void ConfigBuilder::buildPalMetadata() {
  if (!m_pipelineState->isGraphics())
    buildPipelineCsRegConfig();
  else {
    const bool hasTs = (m_hasTcs || m_hasTes);
    const bool enableNgg = m_pipelineState->getNggControl()->enableNgg;

    if (!hasTs && !m_hasGs) {
      // VS-FS pipeline
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
// Builds register configuration for graphics pipeline (VS-FS).
void ConfigBuilder::buildPipelineVsFsRegConfig() // [out] Size of register configuration
{
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineVsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::VsPs);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  if (stageMask & shaderStageToMask(ShaderStageVertex)) {
    buildVsRegConfig<PipelineVsFsRegConfig>(ShaderStageVertex, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageVertex);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
    }

    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    SET_REG(&config, VGT_GS_ONCHIP_CNTL, 0);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
    }
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
  }

  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
  // even if there are more than 2 shader engines on the GPU.
  unsigned primGroupSize = 128;
  unsigned numShaderEngines = m_pipelineState->getTargetInfo().getGpuProperty().numShaderEngines;
  if (numShaderEngines > 2)
    primGroupSize = alignTo(primGroupSize, 2);

  iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

  if (gfxIp.major >= 10) {
    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
  } else {
    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-FS).
void ConfigBuilder::buildPipelineVsTsFsRegConfig() {
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineVsTsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Tess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);
  //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
  //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
  SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);

  if (stageMask & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageTessControl))) {
    const bool hasVs = ((stageMask & shaderStageToMask(ShaderStageVertex)) != 0);
    const bool hasTcs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0);

    buildLsHsRegConfig<PipelineVsTsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
    }

    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
  }

  if (stageMask & shaderStageToMask(ShaderStageTessEval)) {
    buildVsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageTessEval, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_DS);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessEval);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);

    unsigned checksum = setShaderHash(ShaderStageTessEval);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
    }
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const auto &tcsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
  const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

  if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId) {
    iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
    iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
  }

  if (gfxIp.major >= 10) {
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
void ConfigBuilder::buildPipelineVsGsFsRegConfig() // [out] Size of register configuration
{
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineVsGsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Gs);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  if (stageMask & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageGeometry))) {
    const bool hasVs = ((stageMask & shaderStageToMask(ShaderStageVertex)) != 0);
    const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

    buildEsGsRegConfig<PipelineVsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
  }

  if (stageMask & shaderStageToMask(ShaderStageCopyShader)) {
    buildVsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageCopyShader, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageCopyShader);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);
  }

  // Set up IA_MULTI_VGT_PARAM
  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  const unsigned primGroupSize = 128;
  iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

  if (gfxIp.major >= 10) {
    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
  } else {
    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-GS-FS).
void ConfigBuilder::buildPipelineVsTsGsFsRegConfig() {
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineVsTsGsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::GsTess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  if (stageMask & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageTessControl))) {
    const bool hasVs = ((stageMask & shaderStageToMask(ShaderStageVertex)) != 0);
    const bool hasTcs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0);

    buildLsHsRegConfig<PipelineVsTsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                  hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);

    //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
    //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
    SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
  }

  if (stageMask & (shaderStageToMask(ShaderStageTessEval) | shaderStageToMask(ShaderStageGeometry))) {
    const bool hasTes = ((stageMask & shaderStageToMask(ShaderStageTessEval)) != 0);
    const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

    buildEsGsRegConfig<PipelineVsTsGsFsRegConfig>(hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                  hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageTessEval);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
    }

    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
  }

  if (stageMask & shaderStageToMask(ShaderStageCopyShader)) {
    buildVsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageCopyShader, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageCopyShader);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);
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

  if (gfxIp.major >= 10) {
    SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
  } else {
    SET_REG(&config, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
  }

  // Set up VGT_TF_PARAM
  setupVgtTfParam(&config.lsHsRegs);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-FS).
void ConfigBuilder::buildPipelineNggVsFsRegConfig() {
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  assert(gfxIp.major >= 10);

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineNggVsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Ngg);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, nggControl->passthroughMode);

  if (stageMask & shaderStageToMask(ShaderStageVertex)) {
    buildPrimShaderRegConfig<PipelineNggVsFsRegConfig>(ShaderStageVertex, ShaderStageInvalid, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageVertex);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);

    unsigned checksum = setShaderHash(ShaderStageVertex);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
  }

  regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

  // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
  // even if there are more than 2 shader engines on the GPU.
  unsigned primGroupSize = 128;
  unsigned numShaderEngines = m_pipelineState->getTargetInfo().getGpuProperty().numShaderEngines;
  if (numShaderEngines > 2)
    primGroupSize = alignTo(primGroupSize, 2);

  iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

  SET_REG(&config, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-FS).
void ConfigBuilder::buildPipelineNggVsTsFsRegConfig() {
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  assert(gfxIp.major >= 10);

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineNggVsTsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::NggTess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, nggControl->passthroughMode);

  if (stageMask & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageTessControl))) {
    const bool hasVs = ((stageMask & shaderStageToMask(ShaderStageVertex)) != 0);
    const bool hasTcs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0);

    buildLsHsRegConfig<PipelineNggVsTsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                   hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
  }

  if (stageMask & shaderStageToMask(ShaderStageTessEval)) {
    buildPrimShaderRegConfig<PipelineNggVsTsFsRegConfig>(ShaderStageTessEval, ShaderStageInvalid, &config);

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessEval);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);

    unsigned checksum = setShaderHash(ShaderStageTessEval);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsTsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
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
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  assert(gfxIp.major >= 10);

  assert(m_pipelineState->getNggControl()->enableNgg);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineNggVsGsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::Ngg);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
  // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
  // hardware pass-through).
  SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, false);

  if (stageMask & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageGeometry))) {
    const bool hasVs = ((stageMask & shaderStageToMask(ShaderStageVertex)) != 0);
    const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

    buildPrimShaderRegConfig<PipelineNggVsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                         hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsGsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
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
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  assert(gfxIp.major >= 10);

  assert(m_pipelineState->getNggControl()->enableNgg);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();

  PipelineNggVsTsGsFsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
  addApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs);
  addApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

  setPipelineType(Util::Abi::PipelineType::NggTess);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

  SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
  // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
  // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
  // hardware pass-through).
  SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, false);

  if (stageMask & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageTessControl))) {
    const bool hasVs = ((stageMask & shaderStageToMask(ShaderStageVertex)) != 0);
    const bool hasTcs = ((stageMask & shaderStageToMask(ShaderStageTessControl)) != 0);

    buildLsHsRegConfig<PipelineNggVsTsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                     hasTcs ? ShaderStageTessControl : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageVertex);
    checksum = checksum ^ setShaderHash(ShaderStageTessControl);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageTessControl);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
  }

  if (stageMask & (shaderStageToMask(ShaderStageTessEval) | shaderStageToMask(ShaderStageGeometry))) {
    const bool hasTes = ((stageMask & shaderStageToMask(ShaderStageTessEval)) != 0);
    const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

    buildPrimShaderRegConfig<PipelineNggVsTsGsFsRegConfig>(hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                           hasGs ? ShaderStageGeometry : ShaderStageInvalid, &config);

    unsigned checksum = setShaderHash(ShaderStageTessEval);
    checksum = checksum ^ setShaderHash(ShaderStageGeometry);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
    }

    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
    SET_REG_FIELD(&config, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

    auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    if (waveFrontSize == 32) {
      SET_REG_GFX10_FIELD(&config, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
    }
    if (gfxIp.major >= 10)
      setWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
  }

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    buildPsRegConfig<PipelineNggVsTsGsFsRegConfig>(ShaderStageFragment, &config);

    unsigned checksum = setShaderHash(ShaderStageFragment);

    if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
      SET_REG_FIELD(&config.psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
    }
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
// Builds register configuration for compute pipeline.
void ConfigBuilder::buildPipelineCsRegConfig() {
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  assert(m_pipelineState->getShaderStageMask() == shaderStageToMask(ShaderStageCompute));

  CsRegConfig config(gfxIp);

  addApiHwShaderMapping(ShaderStageCompute, Util::Abi::HwShaderCs);

  setPipelineType(Util::Abi::PipelineType::Cs);

  buildCsRegConfig(ShaderStageCompute, &config);

  unsigned checksum = setShaderHash(ShaderStageCompute);

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportShaderPowerProfiling) {
    SET_REG_FIELD(&config, COMPUTE_SHADER_CHKSUM, CHECKSUM, checksum);
  }

  appendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for hardware vertex shader.
template <typename T>
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] pConfig : Register configuration for vertex-shader-specific pipeline
void ConfigBuilder::buildVsRegConfig(ShaderStage shaderStage, T *pConfig) {
  assert(shaderStage == ShaderStageVertex || shaderStage == ShaderStageTessEval ||
         shaderStage == ShaderStageCopyShader);

  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);

  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage;

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP, true); // Follow PAL setting

  const auto &xfbStrides = resUsage->inOutUsage.xfbStrides;
  bool enableXfb = resUsage->inOutUsage.enableXfb;
  if (shaderStage == ShaderStageCopyShader) {
    // NOTE: For copy shader, we use fixed number of user data registers.
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, lgc::CopyShaderUserSgprCount);
    setNumAvailSgprs(Util::Abi::HardwareStage::Vs, m_pipelineState->getTargetInfo().getGpuProperty().maxSgprsAvailable);
    setNumAvailVgprs(Util::Abi::HardwareStage::Vs, m_pipelineState->getTargetInfo().getGpuProperty().maxVgprsAvailable);

    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN,
                  resUsage->inOutUsage.gs.outLocCount[0] > 0 && enableXfb);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN, resUsage->inOutUsage.gs.outLocCount[1] > 0);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN, resUsage->inOutUsage.gs.outLocCount[2] > 0);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN, resUsage->inOutUsage.gs.outLocCount[3] > 0);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, RAST_STREAM, resUsage->inOutUsage.gs.rasterStream);
  } else {
    const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, DEBUG_MODE, shaderOptions.debugMode);

    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, TRAP_PRESENT, shaderOptions.trapPresent);
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, intfData->userDataCount);
    const bool userSgprMsb = (intfData->userDataCount > 31);

    if (gfxIp.major == 10) {
      SET_REG_GFX10_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
    } else {
      SET_REG_GFX9_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
    }

    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN, enableXfb);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN, false);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN, false);
    SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN, false);

    setNumAvailSgprs(Util::Abi::HardwareStage::Vs, resUsage->numSgprsAvailable);
    setNumAvailVgprs(Util::Abi::HardwareStage::Vs, resUsage->numVgprsAvailable);
  }

  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_EN, enableXfb);
  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE0_EN, (xfbStrides[0] > 0));
  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE1_EN, (xfbStrides[1] > 0));
  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE2_EN, (xfbStrides[2] > 0));
  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE3_EN, (xfbStrides[3] > 0));

  SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_VTX_STRIDE_0, STRIDE, xfbStrides[0] / sizeof(unsigned));
  SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_VTX_STRIDE_1, STRIDE, xfbStrides[1] / sizeof(unsigned));
  SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_VTX_STRIDE_2, STRIDE, xfbStrides[2] / sizeof(unsigned));
  SET_REG_FIELD(&pConfig->vsRegs, VGT_STRMOUT_VTX_STRIDE_3, STRIDE, xfbStrides[3] / sizeof(unsigned));

  unsigned streamBufferConfig = 0;
  for (auto i = 0; i < MaxGsStreams; ++i)
    streamBufferConfig |= (resUsage->inOutUsage.streamXfbBuffers[i] << (i * 4));
  SET_REG(&pConfig->vsRegs, VGT_STRMOUT_BUFFER_CONFIG, streamBufferConfig);

  if (gfxIp.major == 10) {
    SET_REG_GFX10_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, MEM_ORDERED, true);
  }

  uint8_t usrClipPlaneMask = m_pipelineState->getRasterizerState().usrClipPlaneMask;
  bool depthClipDisable = (!static_cast<bool>(m_pipelineState->getViewportState().depthClipEnable));
  bool rasterizerDiscardEnable = m_pipelineState->getRasterizerState().rasterizerDiscardEnable;
  bool disableVertexReuse = m_pipelineState->getInputAssemblyState().disableVertexReuse;

  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_0, (usrClipPlaneMask >> 0) & 0x1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_1, (usrClipPlaneMask >> 1) & 0x1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_2, (usrClipPlaneMask >> 2) & 0x1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_3, (usrClipPlaneMask >> 3) & 0x1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_4, (usrClipPlaneMask >> 4) & 0x1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_5, (usrClipPlaneMask >> 5) & 0x1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF, true); // DepthRange::ZeroToOne
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE, depthClipDisable);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE, depthClipDisable);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, DX_RASTERIZATION_KILL, rasterizerDiscardEnable);

  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VPORT_X_SCALE_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VPORT_X_OFFSET_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VPORT_Y_SCALE_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VPORT_Y_OFFSET_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VPORT_Z_SCALE_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VPORT_Z_OFFSET_ENA, true);
  SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VTE_CNTL, VTX_W0_FMT, true);

  SET_REG_FIELD(&pConfig->vsRegs, PA_SU_VTX_CNTL, PIX_CENTER, 1);
  SET_REG_FIELD(&pConfig->vsRegs, PA_SU_VTX_CNTL, ROUND_MODE, 2); // Round to even
  SET_REG_FIELD(&pConfig->vsRegs, PA_SU_VTX_CNTL, QUANT_MODE, 5); // Use 8-bit fractions

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
      SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable instance ID
    } else if (builtInUsage.vs.primitiveId) {
      SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
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
      SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable primitive ID
    } else {
      SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
    }

    if (m_pipelineState->isTessOffChip()) {
      SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN, true);
    }
  } else {
    assert(shaderStage == ShaderStageCopyShader);

    usePointSize = builtInUsage.gs.pointSize;
    usePrimitiveId = builtInUsage.gs.primitiveIdIn;
    useLayer = builtInUsage.gs.layer;
    useViewportIndex = builtInUsage.gs.viewportIndex;
    clipDistanceCount = builtInUsage.gs.clipDistance;
    cullDistanceCount = builtInUsage.gs.cullDistance;

    // NOTE: For ES-GS merged shader, the actual use of primitive ID should take both ES and GS into consideration.
    const bool hasTs = ((m_pipelineState->getShaderStageMask() &
                         (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);
    if (hasTs) {
      const auto &tesBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
      usePrimitiveId = usePrimitiveId || tesBuiltInUsage.primitiveId;
    } else {
      const auto &vsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
      usePrimitiveId = usePrimitiveId || vsBuiltInUsage.primitiveId;
    }

    const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
    if (m_pipelineState->isGsOnChip() && cl::InRegEsGsLdsSize) {
      assert(gsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize != 0);

      appendConfig(mmSPI_SHADER_USER_DATA_VS_0 + gsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::EsGsLdsSize));
    }

    if (enableXfb) {
      assert(gsIntfData->userDataUsage.gs.copyShaderStreamOutTable != 0);
      appendConfig(mmSPI_SHADER_USER_DATA_VS_0 + gsIntfData->userDataUsage.gs.copyShaderStreamOutTable,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::StreamOutTable));
    }
  }

  SET_REG_FIELD(&pConfig->vsRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, usePrimitiveId);

  if (gfxIp.major >= 10 && resUsage->inOutUsage.expCount == 0) {
    SET_REG_GFX10_FIELD(&pConfig->vsRegs, SPI_VS_OUT_CONFIG, NO_PC_EXPORT, true);
  } else {
    SET_REG_FIELD(&pConfig->vsRegs, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, resUsage->inOutUsage.expCount - 1);
  }

  setUsesViewportArrayIndex(useViewportIndex);

  // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
  // index if each GS, TES, or VS invocation emits the same viewport array index for each vertex and we set
  // VTE_VPORT_PROVOKE_DISABLE.
  if (useViewportIndex) {
    // TODO: In the future, we can only disable vertex reuse only if viewport array index is emitted divergently
    // for each vertex.
    disableVertexReuse = true;
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, true);
  } else {
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, false);
  }

  SET_REG_FIELD(&pConfig->vsRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

  useLayer = useLayer || m_pipelineState->getInputAssemblyState().enableMultiView;

  if (usePointSize || useLayer || useViewportIndex)
  {
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);

    if (gfxIp.major == 9) {
    } else if (gfxIp.major == 10) {
    } else
      llvm_unreachable("Not implemented!");
  }

  if (clipDistanceCount > 0 || cullDistanceCount > 0) {
    SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST0_VEC_ENA, true);
    if (clipDistanceCount + cullDistanceCount > 4) {
      SET_REG_FIELD(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST1_VEC_ENA, true);
    }

    unsigned clipDistanceMask = (1 << clipDistanceCount) - 1;
    unsigned cullDistanceMask = (1 << cullDistanceCount) - 1;

    // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
    unsigned paClVsOutCntl = GET_REG(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL);
    paClVsOutCntl |= clipDistanceMask;
    paClVsOutCntl |= (cullDistanceMask << 8);
    SET_REG(&pConfig->vsRegs, PA_CL_VS_OUT_CNTL, paClVsOutCntl);
  }

  unsigned posCount = 1; // gl_Position is always exported
  if (usePointSize || useLayer || useViewportIndex)
    ++posCount;

  if (clipDistanceCount + cullDistanceCount > 0) {
    ++posCount;
    if (clipDistanceCount + cullDistanceCount > 4)
      ++posCount;
  }

  SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_POS_FORMAT, POS0_EXPORT_FORMAT, SPI_SHADER_4COMP);
  if (posCount > 1) {
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_POS_FORMAT, POS1_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 2) {
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_POS_FORMAT, POS2_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 3) {
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_POS_FORMAT, POS3_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportSpiPrefPriority) {
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_USER_ACCUM_VS_0, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_USER_ACCUM_VS_1, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_USER_ACCUM_VS_2, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->vsRegs, SPI_SHADER_USER_ACCUM_VS_3, CONTRIBUTION, 1);
  }

  // Set shader user data maping
  buildUserDataConfig(shaderStage, ShaderStageInvalid, mmSPI_SHADER_USER_DATA_VS_0);
}

// =====================================================================================================================
// Builds register configuration for hardware local-hull merged shader.
template <typename T>
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param [out] pConfig : Register configuration for local-hull-shader-specific pipeline
void ConfigBuilder::buildLsHsRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *pConfig) {
  assert(shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageInvalid);
  assert(shaderStage2 == ShaderStageTessControl || shaderStage2 == ShaderStageInvalid);

  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const auto tcsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessControl);
  const auto &vsBuiltInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;

  unsigned floatMode = setupFloatingPointMode(shaderStage2 != ShaderStageInvalid ? shaderStage2 : shaderStage1);
  SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DX10_CLAMP, true); // Follow PAL setting

  unsigned lsVgtCompCnt = 1;
  if (vsBuiltInUsage.instanceIndex)
    lsVgtCompCnt += 2; // Enable instance ID
  SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, LS_VGPR_COMP_CNT, lsVgtCompCnt);

  const auto &vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
  const auto &tcsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessControl);
  unsigned userDataCount = std::max(vsIntfData->userDataCount, tcsIntfData->userDataCount);

  const auto &tcsShaderOptions = m_pipelineState->getShaderOptions(ShaderStageTessControl);
  SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DEBUG_MODE, tcsShaderOptions.debugMode);

  const bool userSgprMsb = (userDataCount > 31);
  if (gfxIp.major == 10) {
    bool wgpMode = (getShaderWgpMode(ShaderStageVertex) || getShaderWgpMode(ShaderStageTessControl));

    SET_REG_GFX10_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, MEM_ORDERED, true);
    SET_REG_GFX10_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, WGP_MODE, wgpMode);
    SET_REG_GFX10_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
  } else {
    SET_REG_GFX9_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
  }
  SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, TRAP_PRESENT, tcsShaderOptions.trapPresent);
  SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR, userDataCount);

  // NOTE: On GFX7+, granularity for the LDS_SIZE field is 128. The range is 0~128 which allocates 0 to 16K
  // DWORDs.
  const auto &calcFactor = tcsResUsage->inOutUsage.tcs.calcFactor;
  unsigned ldsSizeInDwords =
      calcFactor.onChip.patchConstStart + calcFactor.patchConstSize * calcFactor.patchCountPerThreadGroup;
  if (m_pipelineState->isTessOffChip())
    ldsSizeInDwords = calcFactor.inPatchSize * calcFactor.patchCountPerThreadGroup;

  const unsigned ldsSizeDwordGranularity = 128u;
  const unsigned ldsSizeDwordGranularityShift = 7u;
  unsigned ldsSize = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity) >> ldsSizeDwordGranularityShift;

  if (gfxIp.major == 9) {
    SET_REG_GFX9_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
  } else if (gfxIp.major == 10) {
    SET_REG_GFX10_FIELD(&pConfig->lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
  } else
    llvm_unreachable("Not implemented!");

  setLdsSizeByteSize(Util::Abi::HardwareStage::Hs, ldsSizeInDwords * 4);

  // Minimum and maximum tessellation factors supported by the hardware.
  constexpr float minTessFactor = 1.0f;
  constexpr float maxTessFactor = 64.0f;
  SET_REG(&pConfig->lsHsRegs, VGT_HOS_MIN_TESS_LEVEL, FloatToBits(minTessFactor));
  SET_REG(&pConfig->lsHsRegs, VGT_HOS_MAX_TESS_LEVEL, FloatToBits(maxTessFactor));

  // Set VGT_LS_HS_CONFIG
  SET_REG_FIELD(&pConfig->lsHsRegs, VGT_LS_HS_CONFIG, NUM_PATCHES, calcFactor.patchCountPerThreadGroup);
  SET_REG_FIELD(&pConfig->lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_INPUT_CP,
                m_pipelineState->getInputAssemblyState().patchControlPoints);

  auto hsNumOutputCp = m_pipelineState->getShaderModes()->getTessellationMode().outputVertices;
  SET_REG_FIELD(&pConfig->lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_OUTPUT_CP, hsNumOutputCp);

  setNumAvailSgprs(Util::Abi::HardwareStage::Hs, tcsResUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Hs, tcsResUsage->numVgprsAvailable);

  // Set up VGT_TF_PARAM
  setupVgtTfParam(&pConfig->lsHsRegs);

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportSpiPrefPriority) {
    SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_0, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_1, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_2, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_3, CONTRIBUTION, 1);
  }

  if (gfxIp.major == 9) {
    buildUserDataConfig(shaderStage1 != ShaderStageInvalid ? shaderStage1 : shaderStage2,
                        shaderStage1 != ShaderStageInvalid ? shaderStage2 : ShaderStageInvalid,
                        Gfx09::mmSPI_SHADER_USER_DATA_LS_0);
  } else if (gfxIp.major == 10) {
    buildUserDataConfig(shaderStage1 != ShaderStageInvalid ? shaderStage1 : shaderStage2,
                        shaderStage1 != ShaderStageInvalid ? shaderStage2 : ShaderStageInvalid,
                        Pal::Gfx9::Chip::Gfx10::mmSPI_SHADER_USER_DATA_HS_0);
  } else
    llvm_unreachable("Not implemented!");
}

// =====================================================================================================================
// Builds register configuration for hardware export-geometry merged shader.
template <typename T>
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param [out] pConfig : Register configuration for export-geometry-shader-specific pipeline
void ConfigBuilder::buildEsGsRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *pConfig) {
  assert(shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageTessEval ||
         shaderStage1 == ShaderStageInvalid);
  assert(shaderStage2 == ShaderStageGeometry || shaderStage2 == ShaderStageInvalid);

  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  const bool hasTs =
      ((stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);

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
    gsVgprCompCnt = 3;
  else if (gsBuiltInUsage.primitiveIdIn)
    gsVgprCompCnt = 2;
  else if (calcFactor.inputVertices > 2)
    gsVgprCompCnt = 1;

  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT, gsVgprCompCnt);

  unsigned floatMode = setupFloatingPointMode(shaderStage2 != ShaderStageInvalid ? shaderStage2 : shaderStage1);
  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  unsigned userDataCount =
      std::max((hasTs ? tesIntfData->userDataCount : vsIntfData->userDataCount), gsIntfData->userDataCount);

  const auto &gsShaderOptions = m_pipelineState->getShaderOptions(ShaderStageGeometry);
  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, gsShaderOptions.debugMode);

  const bool userSgprMsb = (userDataCount > 31);
  if (gfxIp.major == 10) {
    bool wgpMode =
        (getShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex) || getShaderWgpMode(ShaderStageGeometry));

    SET_REG_GFX10_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
    SET_REG_GFX10_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);
    SET_REG_GFX10_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
  } else {
    SET_REG_GFX9_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
  }

  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, gsShaderOptions.trapPresent);
  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

  unsigned esVgprCompCnt = 0;
  if (hasTs) {
    // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
    if (tesBuiltInUsage.primitiveId)
      esVgprCompCnt = 3;
    else
      esVgprCompCnt = 2;

    if (m_pipelineState->isTessOffChip()) {
      SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN, true);
    }
  } else {
    if (vsBuiltInUsage.instanceIndex)
      esVgprCompCnt = 3; // Enable instance ID
  }

  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT, esVgprCompCnt);

  const auto ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;

  SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE,
                calcFactor.gsOnChipLdsSize >> ldsSizeDwordGranularityShift);
  setLdsSizeByteSize(Util::Abi::HardwareStage::Gs, calcFactor.gsOnChipLdsSize * 4);
  setEsGsLdsSize(calcFactor.esGsLdsSize * 4);

  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

  // TODO: Currently only support offchip GS
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);

  if (m_pipelineState->isGsOnChip()) {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_ON);
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, false);

    setEsGsLdsByteSize(calcFactor.esGsLdsSize * 4);
  } else {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);
  }

  if (geometryMode.outputVertices <= 128) {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128);
  } else if (geometryMode.outputVertices <= 256) {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256);
  } else if (geometryMode.outputVertices <= 512) {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_512);
  } else {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_1024);
  }

  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);

  // NOTE: The value of field "GS_INST_PRIMS_IN_SUBGRP" should be strictly equal to the product of
  // VGT_GS_ONCHIP_CNTL.GS_PRIMS_PER_SUBGRP * VGT_GS_INSTANCE_CNT.CNT.
  const unsigned gsInstPrimsInSubgrp =
      geometryMode.invocations > 1 ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations) : 0;
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

  unsigned gsVertItemSize0 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[0];
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize0);

  unsigned gsVertItemSize1 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[1];
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_VERT_ITEMSIZE_1, ITEMSIZE, gsVertItemSize1);

  unsigned gsVertItemSize2 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[2];
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_VERT_ITEMSIZE_2, ITEMSIZE, gsVertItemSize2);

  unsigned gsVertItemSize3 = sizeof(unsigned) * gsInOutUsage.gs.outLocCount[3];
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_VERT_ITEMSIZE_3, ITEMSIZE, gsVertItemSize3);

  unsigned gsVsRingOffset = gsVertItemSize0 * maxVertOut;
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GSVS_RING_OFFSET_1, OFFSET, gsVsRingOffset);

  gsVsRingOffset += gsVertItemSize1 * maxVertOut;
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GSVS_RING_OFFSET_2, OFFSET, gsVsRingOffset);

  gsVsRingOffset += gsVertItemSize2 * maxVertOut;
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GSVS_RING_OFFSET_3, OFFSET, gsVsRingOffset);

  if (geometryMode.invocations > 1 || gsBuiltInUsage.invocationId) {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
  }
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

  VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
  if (gsInOutUsage.outputMapLocCount == 0)
    gsOutputPrimitiveType = POINTLIST;
  else if (geometryMode.outputPrimitive == OutputPrimitives::Points)
    gsOutputPrimitiveType = POINTLIST;
  else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
    gsOutputPrimitiveType = LINESTRIP;

  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);

  // Set multi-stream output primitive type
  if (gsVertItemSize1 > 0 || gsVertItemSize2 > 0 || gsVertItemSize3 > 0) {
    const static auto GsOutPrimInvalid = 3u;
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_1,
                  gsVertItemSize1 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);

    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_2,
                  gsVertItemSize2 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);

    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_3,
                  gsVertItemSize3 > 0 ? gsOutputPrimitiveType : GsOutPrimInvalid);
  }

  SET_REG_FIELD(&pConfig->esGsRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, calcFactor.gsVsRingItemSize);
  SET_REG_FIELD(&pConfig->esGsRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

  const unsigned maxPrimsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, MaxGsThreadsPerSubgroup);

  if (gfxIp.major == 9) {
    SET_REG_FIELD(&pConfig->esGsRegs, VGT_GS_MAX_PRIMS_PER_SUBGROUP, MAX_PRIMS_PER_SUBGROUP, maxPrimsPerSubgroup);
  } else if (gfxIp.major == 10) {
    SET_REG_FIELD(&pConfig->esGsRegs, GE_MAX_OUTPUT_PER_SUBGROUP, MAX_VERTS_PER_SUBGROUP, maxPrimsPerSubgroup);
  } else
    llvm_unreachable("Not implemented!");

  setNumAvailSgprs(Util::Abi::HardwareStage::Gs, gsResUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Gs, gsResUsage->numVgprsAvailable);

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportSpiPrefPriority) {
    SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_0, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_1, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_2, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_3, CONTRIBUTION, 1);
  }

  if (gfxIp.major == 9) {
    buildUserDataConfig(shaderStage1 != ShaderStageInvalid ? shaderStage1 : shaderStage2,
                        shaderStage1 != ShaderStageInvalid ? shaderStage2 : ShaderStageInvalid,
                        Gfx09::mmSPI_SHADER_USER_DATA_ES_0);
  } else if (gfxIp.major == 10) {
    buildUserDataConfig(shaderStage1 != ShaderStageInvalid ? shaderStage1 : shaderStage2,
                        shaderStage1 != ShaderStageInvalid ? shaderStage2 : ShaderStageInvalid,
                        Pal::Gfx9::Chip::Gfx10::mmSPI_SHADER_USER_DATA_GS_0);
  } else
    llvm_unreachable("Not implemented!");
}

// =====================================================================================================================
// Builds register configuration for hardware primitive shader.
template <typename T>
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param [out] pConfig : Register configuration for primitive-shader-specific pipeline
void ConfigBuilder::buildPrimShaderRegConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, T *pConfig) {
  assert(shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageTessEval ||
         shaderStage1 == ShaderStageInvalid);
  assert(shaderStage2 == ShaderStageGeometry || shaderStage2 == ShaderStageInvalid);

  const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  assert(gfxIp.major >= 10);

  const auto nggControl = m_pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  const unsigned stageMask = m_pipelineState->getShaderStageMask();
  const bool hasTs =
      ((stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);
  const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

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
  if (hasGs) {
    if (calcFactor.inputVertices > 4 || gsBuiltInUsage.invocationId)
      gsVgprCompCnt = 3;
    else if (gsBuiltInUsage.primitiveIdIn)
      gsVgprCompCnt = 2;
    else if (calcFactor.inputVertices > 2)
      gsVgprCompCnt = 1;
  } else {
    // NOTE: When GS is absent, only those VGPRs are required: vtx0/vtx1 offset, vtx2/vtx3 offset,
    // primitive ID (only for VS).
    gsVgprCompCnt = hasTs ? 1 : (vsBuiltInUsage.primitiveId ? 2 : 1);
  }

  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT, gsVgprCompCnt);

  unsigned floatMode = setupFloatingPointMode(shaderStage2 != ShaderStageInvalid ? shaderStage2 : shaderStage1);
  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  unsigned userDataCount =
      std::max((hasTs ? tesIntfData->userDataCount : vsIntfData->userDataCount), gsIntfData->userDataCount);

  const auto &gsShaderOptions = m_pipelineState->getShaderOptions(ShaderStageGeometry);
  bool wgpMode = getShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex);
  if (hasGs)
    wgpMode = (wgpMode || getShaderWgpMode(ShaderStageGeometry));

  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, gsShaderOptions.debugMode);
  SET_REG_GFX10_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
  SET_REG_GFX10_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);

  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, gsShaderOptions.trapPresent);
  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

  const bool userSgprMsb = (userDataCount > 31);

  if (gfxIp.major == 10) {
    SET_REG_GFX10_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
  } else {
    SET_REG_GFX9_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
  }

  unsigned esVgprCompCnt = 0;
  if (hasTs) {
    // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
    if (tesBuiltInUsage.primitiveId)
      esVgprCompCnt = 3;
    else
      esVgprCompCnt = 2;

    if (m_pipelineState->isTessOffChip()) {
      SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN, true);
    }
  } else {
    if (vsBuiltInUsage.instanceIndex)
      esVgprCompCnt = 3; // Enable instance ID
  }

  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT, esVgprCompCnt);

  const auto ldsSizeDwordGranularityShift =
      m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;

  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE,
                calcFactor.gsOnChipLdsSize >> ldsSizeDwordGranularityShift);
  setLdsSizeByteSize(Util::Abi::HardwareStage::Gs, calcFactor.gsOnChipLdsSize * 4);
  setEsGsLdsSize(calcFactor.esGsLdsSize * 4);

  unsigned maxVertOut = std::max(1u, static_cast<unsigned>(geometryMode.outputVertices));
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);

  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);

  const unsigned gsInstPrimsInSubgrp = geometryMode.invocations > 1
                                           ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations)
                                           : calcFactor.gsPrimsPerSubgroup;
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

  unsigned gsVertItemSize = 4 * gsInOutUsage.outputMapLocCount;
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize);

  if (geometryMode.invocations > 1 || gsBuiltInUsage.invocationId) {
    SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
    SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
    if (gfxIp.major > 10 || (gfxIp.major == 10 && gfxIp.minor >= 1)) {
      SET_REG_GFX10_FIELD(&pConfig->primShaderRegs, VGT_GS_INSTANCE_CNT, EN_MAX_VERT_OUT_PER_GS_INSTANCE,
                                 calcFactor.enableMaxVertOut);
    }
  }
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

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
    const auto topology = m_pipelineState->getInputAssemblyState().topology;
    if (topology == PrimitiveTopology::PointList)
      gsOutputPrimitiveType = POINTLIST;
    else if (topology == PrimitiveTopology::LineList || topology == PrimitiveTopology::LineStrip ||
             topology == PrimitiveTopology::LineListWithAdjacency ||
             topology == PrimitiveTopology::LineStripWithAdjacency)
      gsOutputPrimitiveType = LINESTRIP;
    else if (topology == PrimitiveTopology::TriangleList || topology == PrimitiveTopology::TriangleStrip ||
             topology == PrimitiveTopology::TriangleFan || topology == PrimitiveTopology::TriangleListWithAdjacency ||
             topology == PrimitiveTopology::TriangleStripWithAdjacency)
      gsOutputPrimitiveType = TRISTRIP;
    else
      llvm_unreachable("Should never be called!");
  }

  // TODO: Multiple output streams are not supported.
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, calcFactor.gsVsRingItemSize);
  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

  const unsigned maxVertsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, NggMaxThreadsPerSubgroup);
  SET_REG_FIELD(&pConfig->primShaderRegs, GE_MAX_OUTPUT_PER_SUBGROUP, MAX_VERTS_PER_SUBGROUP, maxVertsPerSubgroup);

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

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportSpiPrefPriority) {
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_0, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_1, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_2, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_3, CONTRIBUTION, 1);
  }

  //
  // Build VS specific configuration
  //
  uint8_t usrClipPlaneMask = m_pipelineState->getRasterizerState().usrClipPlaneMask;
  bool depthClipDisable = (!static_cast<bool>(m_pipelineState->getViewportState().depthClipEnable));
  bool rasterizerDiscardEnable = m_pipelineState->getRasterizerState().rasterizerDiscardEnable;
  bool disableVertexReuse = m_pipelineState->getInputAssemblyState().disableVertexReuse;

  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_0, (usrClipPlaneMask >> 0) & 0x1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_1, (usrClipPlaneMask >> 1) & 0x1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_2, (usrClipPlaneMask >> 2) & 0x1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_3, (usrClipPlaneMask >> 3) & 0x1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_4, (usrClipPlaneMask >> 4) & 0x1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_5, (usrClipPlaneMask >> 5) & 0x1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF, true); // DepthRange::ZeroToOne
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE, depthClipDisable);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE, depthClipDisable);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, DX_RASTERIZATION_KILL, rasterizerDiscardEnable);

  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VPORT_X_SCALE_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VPORT_X_OFFSET_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VPORT_Y_SCALE_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VPORT_Y_OFFSET_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VPORT_Z_SCALE_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VPORT_Z_OFFSET_ENA, true);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VTE_CNTL, VTX_W0_FMT, true);

  SET_REG_FIELD(&pConfig->primShaderRegs, PA_SU_VTX_CNTL, PIX_CENTER, 1);
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_SU_VTX_CNTL, ROUND_MODE, 2); // Round to even
  SET_REG_FIELD(&pConfig->primShaderRegs, PA_SU_VTX_CNTL, QUANT_MODE, 5); // Use 8-bit fractions

  // Stage-specific processing
  bool usePointSize = false;
  bool usePrimitiveId = false;
  bool useLayer = false;
  bool useViewportIndex = false;

  unsigned clipDistanceCount = 0;
  unsigned cullDistanceCount = 0;

  unsigned expCount = 0;

  if (hasGs) {
    usePointSize = gsBuiltInUsage.pointSize;
    usePrimitiveId = gsBuiltInUsage.primitiveIdIn;
    useLayer = gsBuiltInUsage.layer;
    useViewportIndex = gsBuiltInUsage.viewportIndex;
    clipDistanceCount = gsBuiltInUsage.clipDistance;
    cullDistanceCount = gsBuiltInUsage.cullDistance;

    expCount = gsResUsage->inOutUsage.expCount;

    // NOTE: For ES-GS merged shader, the actual use of primitive ID should take both ES and GS into consideration.
    if (hasTs)
      usePrimitiveId = usePrimitiveId || tesBuiltInUsage.primitiveId;
    else
      usePrimitiveId = usePrimitiveId || vsBuiltInUsage.primitiveId;
  } else {
    if (hasTs) {
      usePointSize = tesBuiltInUsage.pointSize;
      useLayer = tesBuiltInUsage.layer;
      useViewportIndex = tesBuiltInUsage.viewportIndex;
      clipDistanceCount = tesBuiltInUsage.clipDistance;
      cullDistanceCount = tesBuiltInUsage.cullDistance;

      expCount = tesResUsage->inOutUsage.expCount;
    } else {
      usePointSize = vsBuiltInUsage.pointSize;
      usePrimitiveId = vsBuiltInUsage.primitiveId;
      useLayer = vsBuiltInUsage.layer;
      useViewportIndex = vsBuiltInUsage.viewportIndex;
      clipDistanceCount = vsBuiltInUsage.clipDistance;
      cullDistanceCount = vsBuiltInUsage.cullDistance;

      expCount = vsResUsage->inOutUsage.expCount;
    }
  }

  if (usePrimitiveId) {
    SET_REG_FIELD(&pConfig->primShaderRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, true);

    // NOTE: If primitive ID is used and there is no GS present, the field NGG_DISABLE_PROVOK_REUSE must be
    // set to ensure provoking vertex reuse is disabled in the GE.
    if (!m_hasGs) {
      SET_REG_FIELD(&pConfig->primShaderRegs, VGT_PRIMITIVEID_EN, NGG_DISABLE_PROVOK_REUSE, true);
    }
  }

  if (expCount == 0) {
    // No generic output is present
    SET_REG_GFX10_FIELD(&pConfig->primShaderRegs, SPI_VS_OUT_CONFIG, NO_PC_EXPORT, true);
  } else {
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, expCount - 1);
  }

  setUsesViewportArrayIndex(useViewportIndex);

  // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
  // index if each GS, TES, or VS invocation emits the same viewport array index for each vertex and we set
  // VTE_VPORT_PROVOKE_DISABLE.
  if (useViewportIndex) {
    // TODO: In the future, we can only disable vertex reuse only if viewport array index is emitted divergently
    // for each vertex.
    disableVertexReuse = true;
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, true);
  } else {
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, false);
  }

  SET_REG_FIELD(&pConfig->primShaderRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

  useLayer = useLayer || m_pipelineState->getInputAssemblyState().enableMultiView;

  if (usePointSize || useLayer || useViewportIndex)
  {
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);
  }

  if (clipDistanceCount > 0 || cullDistanceCount > 0) {
    SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST0_VEC_ENA, true);
    if (clipDistanceCount + cullDistanceCount > 4) {
      SET_REG_FIELD(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST1_VEC_ENA, true);
    }

    unsigned clipDistanceMask = (1 << clipDistanceCount) - 1;
    unsigned cullDistanceMask = (1 << cullDistanceCount) - 1;

    // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
    unsigned paClVsOutCntl = GET_REG(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL);
    paClVsOutCntl |= clipDistanceMask;
    paClVsOutCntl |= (cullDistanceMask << 8);
    SET_REG(&pConfig->primShaderRegs, PA_CL_VS_OUT_CNTL, paClVsOutCntl);
  }

  unsigned posCount = 1; // gl_Position is always exported
  if (usePointSize || useLayer || useViewportIndex)
    ++posCount;

  if (clipDistanceCount + cullDistanceCount > 0) {
    ++posCount;
    if (clipDistanceCount + cullDistanceCount > 4)
      ++posCount;
  }

  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_POS_FORMAT, POS0_EXPORT_FORMAT, SPI_SHADER_4COMP);
  if (posCount > 1) {
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_POS_FORMAT, POS1_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 2) {
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_POS_FORMAT, POS2_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }
  if (posCount > 3) {
    SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_POS_FORMAT, POS3_EXPORT_FORMAT, SPI_SHADER_4COMP);
  }

  //
  // Build NGG configuration
  //
  assert(calcFactor.primAmpFactor >= 1);
  SET_REG_FIELD(&pConfig->primShaderRegs, GE_NGG_SUBGRP_CNTL, PRIM_AMP_FACTOR, calcFactor.primAmpFactor);
  SET_REG_FIELD(&pConfig->primShaderRegs, GE_NGG_SUBGRP_CNTL, THDS_PER_SUBGRP, NggMaxThreadsPerSubgroup);

  // TODO: Support PIPELINE_PRIM_ID.
  SET_REG_FIELD(&pConfig->primShaderRegs, SPI_SHADER_IDX_FORMAT, IDX0_EXPORT_FORMAT, SPI_SHADER_1COMP);

  if (nggControl->passthroughMode) {
    INVALIDATE_REG(&pConfig->primShaderRegs, SPI_SHADER_PGM_LO_GS);
  } else {
    // NOTE: For NGG culling mode, the primitive shader table that contains culling data might be accessed by
    // shader. PAL expects 64-bit address of that table and will program it into SPI_SHADER_PGM_LO_GS and
    // SPI_SHADER_PGM_HI_GS if we do not provide one. By setting SPI_SHADER_PGM_LO_GS to NggCullingData, we tell
    // PAL that we will not provide it and it is fine to use SPI_SHADER_PGM_LO_GS and SPI_SHADER_PGM_HI_GS as
    // the address of that table.
    SET_REG(&pConfig->primShaderRegs, SPI_SHADER_PGM_LO_GS,
            static_cast<unsigned>(Util::Abi::UserDataMapping::NggCullingData));
  }

  //
  // Build use data configuration
  //
  buildUserDataConfig(shaderStage1 != ShaderStageInvalid ? shaderStage1 : shaderStage2,
                      shaderStage1 != ShaderStageInvalid ? shaderStage2 : ShaderStageInvalid,
                      Pal::Gfx9::Chip::Gfx10::mmSPI_SHADER_USER_DATA_GS_0);
}

// =====================================================================================================================
// Builds register configuration for hardware pixel shader.
template <typename T>
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] pConfig : Register configuration for pixel-shader-specific pipeline
void ConfigBuilder::buildPsRegConfig(ShaderStage shaderStage, T *pConfig) {
  assert(shaderStage == ShaderStageFragment);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.fs;
  const auto &fragmentMode = m_pipelineState->getShaderModes()->getFragmentShaderMode();

  unsigned floatMode = setupFloatingPointMode(shaderStage);
  SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE, floatMode);
  SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP, true); // Follow PAL setting
  SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE, shaderOptions.debugMode);

  SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC2_PS, TRAP_PRESENT, shaderOptions.trapPresent);
  SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR, intfData->userDataCount);

  const bool userSgprMsb = (intfData->userDataCount > 31);
  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  if (gfxIp.major == 10) {
    SET_REG_GFX10_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC1_PS, MEM_ORDERED, true);

    if (shaderOptions.waveBreakSize == lgc::WaveBreak::DrawTime)
      setCalcWaveBreakSizeAtDrawTime(true);
    else {
      SET_REG_GFX10_FIELD(&pConfig->psRegs, PA_SC_SHADER_CONTROL, WAVE_BREAK_REGION_SIZE,
                          static_cast<unsigned>(shaderOptions.waveBreakSize));
    }

    SET_REG_GFX10_FIELD(&pConfig->psRegs, PA_STEREO_CNTL, STEREO_MODE, STATE_STEREO_X);
    SET_REG_GFX10_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
  } else {
    SET_REG_GFX9_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
  }

  SET_REG_FIELD(&pConfig->psRegs, SPI_BARYC_CNTL, FRONT_FACE_ALL_BITS, true);
  if (fragmentMode.pixelCenterInteger) {
    // TRUE - Force floating point position to upper left corner of pixel (X.0, Y.0)
    SET_REG_FIELD(&pConfig->psRegs, SPI_BARYC_CNTL, POS_FLOAT_ULC, true);
  } else if (builtInUsage.runAtSampleRate) {
    // 2 - Calculate per-pixel floating point position at iterated sample number
    SET_REG_FIELD(&pConfig->psRegs, SPI_BARYC_CNTL, POS_FLOAT_LOCATION, 2);
  } else {
    // 0 - Calculate per-pixel floating point position at pixel center
    SET_REG_FIELD(&pConfig->psRegs, SPI_BARYC_CNTL, POS_FLOAT_LOCATION, 0);
  }

  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, WALK_ALIGN8_PRIM_FITS_ST, true);
  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, WALK_FENCE_ENABLE, true);
  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, TILE_WALK_ORDER_ENABLE, true);
  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, PS_ITER_SAMPLE, builtInUsage.runAtSampleRate);

  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, SUPERTILE_WALK_ORDER_ENABLE, true);
  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE, true);
  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, FORCE_EOV_CNTDWN_ENABLE, true);
  SET_REG_FIELD(&pConfig->psRegs, PA_SC_MODE_CNTL_1, FORCE_EOV_REZ_ENABLE, true);

  ZOrder zOrder = LATE_Z;
  bool execOnHeirFail = false;
  if (fragmentMode.earlyFragmentTests)
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

  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, Z_ORDER, zOrder);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, KILL_ENABLE, builtInUsage.discard);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, Z_EXPORT_ENABLE, builtInUsage.fragDepth);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, STENCIL_TEST_VAL_EXPORT_ENABLE, builtInUsage.fragStencilRef);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, MASK_EXPORT_ENABLE, builtInUsage.sampleMask);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, ALPHA_TO_MASK_DISABLE,
                (builtInUsage.sampleMask || m_pipelineState->getColorExportState().alphaToCoverageEnable == false));
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, DEPTH_BEFORE_SHADER, fragmentMode.earlyFragmentTests);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, EXEC_ON_NOOP,
                (fragmentMode.earlyFragmentTests && resUsage->resourceWrite));
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, EXEC_ON_HIER_FAIL, execOnHeirFail);
  SET_REG_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT, conservativeZExport);

  if (gfxIp.major == 10) {
    SET_REG_GFX10_FIELD(&pConfig->psRegs, DB_SHADER_CONTROL, PRE_SHADER_DEPTH_COVERAGE_ENABLE,
                        fragmentMode.postDepthCoverage);
  }

  unsigned depthExpFmt = EXP_FORMAT_ZERO;
  if (builtInUsage.sampleMask)
    depthExpFmt = EXP_FORMAT_32_ABGR;
  else if (builtInUsage.fragStencilRef)
    depthExpFmt = EXP_FORMAT_32_GR;
  else if (builtInUsage.fragDepth)
    depthExpFmt = EXP_FORMAT_32_R;
  SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_Z_FORMAT, Z_EXPORT_FORMAT, depthExpFmt);

  unsigned spiShaderColFormat = 0;
  unsigned cbShaderMask = resUsage->inOutUsage.fs.cbShaderMask;
  cbShaderMask = resUsage->inOutUsage.fs.isNullFs ? 0 : cbShaderMask;
  const auto &expFmts = resUsage->inOutUsage.fs.expFmts;
  for (unsigned i = 0; i < MaxColorTargets; ++i) {
    // Set fields COL0_EXPORT_FORMAT ~ COL7_EXPORT_FORMAT
    spiShaderColFormat |= (expFmts[i] << (4 * i));
  }

  if (spiShaderColFormat == 0 && depthExpFmt == EXP_FORMAT_ZERO && resUsage->inOutUsage.fs.dummyExport) {
    // NOTE: Hardware requires that fragment shader always exports "something" (color or depth) to the SX.
    // If both SPI_SHADER_Z_FORMAT and SPI_SHADER_COL_FORMAT are zero, we need to override
    // SPI_SHADER_COL_FORMAT to export one channel to MRT0. This dummy export format will be masked
    // off by CB_SHADER_MASK.
    spiShaderColFormat = SPI_SHADER_32_R;
  }

  SET_REG(&pConfig->psRegs, SPI_SHADER_COL_FORMAT, spiShaderColFormat);

  SET_REG(&pConfig->psRegs, CB_SHADER_MASK, cbShaderMask);
  SET_REG_FIELD(&pConfig->psRegs, SPI_PS_IN_CONTROL, NUM_INTERP, resUsage->inOutUsage.fs.interpInfo.size());

  auto waveFrontSize = m_pipelineState->getShaderWaveSize(ShaderStageFragment);
  if (waveFrontSize == 32) {
    SET_REG_GFX10_FIELD(&pConfig->psRegs, SPI_PS_IN_CONTROL, PS_W32_EN, true);
  }

  if (gfxIp.major >= 10)
    setWaveFrontSize(Util::Abi::HardwareStage::Ps, waveFrontSize);

  unsigned pointCoordLoc = InvalidValue;
  if (resUsage->inOutUsage.builtInInputLocMap.find(BuiltInPointCoord) !=
      resUsage->inOutUsage.builtInInputLocMap.end()) {
    // Get generic input corresponding to gl_PointCoord (to set the field PT_SPRITE_TEX)
    pointCoordLoc = resUsage->inOutUsage.builtInInputLocMap[BuiltInPointCoord];
  }

  // NOTE: PAL expects at least one mmSPI_PS_INPUT_CNTL_0 register set, so we always patch it at least one if none
  // were identified in the shader.
  const std::vector<FsInterpInfo> dummyInterpInfo{{0, false, false, false}};
  const auto &fsInterpInfo = resUsage->inOutUsage.fs.interpInfo;
  const auto *interpInfo = fsInterpInfo.size() == 0 ? &dummyInterpInfo : &fsInterpInfo;

  for (unsigned i = 0; i < interpInfo->size(); ++i) {
    auto interpInfoElem = (*interpInfo)[i];
    if (m_pipelineState->isUnlinked() && interpInfoElem.loc == InvalidFsInterpInfo.loc)
      continue;
    if ((interpInfoElem.loc == InvalidFsInterpInfo.loc && interpInfoElem.flat == InvalidFsInterpInfo.flat &&
         interpInfoElem.custom == InvalidFsInterpInfo.custom && interpInfoElem.is16bit == InvalidFsInterpInfo.is16bit))
      interpInfoElem.loc = i;

    regSPI_PS_INPUT_CNTL_0 spiPsInputCntl = {};
    spiPsInputCntl.bits.FLAT_SHADE = interpInfoElem.flat;
    spiPsInputCntl.bits.OFFSET = interpInfoElem.loc;

    if (interpInfoElem.custom) {
      // NOTE: Force parameter cache data to be read in passthrough mode.
      static const unsigned PassThroughMode = (1 << 5);
      spiPsInputCntl.bits.FLAT_SHADE = true;
      spiPsInputCntl.bits.OFFSET |= PassThroughMode;
    } else {
      if (interpInfoElem.is16bit) {
        // NOTE: Enable 16-bit interpolation mode for non-passthrough mode. Attribute 0 is always valid.
        spiPsInputCntl.bits.FP16_INTERP_MODE = true;
        spiPsInputCntl.bits.ATTR0_VALID = true;
      }
    }

    if (pointCoordLoc == i) {
      spiPsInputCntl.bits.PT_SPRITE_TEX = true;

      // NOTE: Set the offset value to force hardware to select input defaults (no VS match).
      static const unsigned UseDefaultVal = (1 << 5);
      spiPsInputCntl.bits.OFFSET = UseDefaultVal;
    }

    appendConfig(mmSPI_PS_INPUT_CNTL_0 + i, spiPsInputCntl.u32All);
  }

  if (pointCoordLoc != InvalidValue) {
    SET_REG_FIELD(&pConfig->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_ENA, true);
    SET_REG_FIELD(&pConfig->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_X, SPI_PNT_SPRITE_SEL_S);
    SET_REG_FIELD(&pConfig->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Y, SPI_PNT_SPRITE_SEL_T);
    SET_REG_FIELD(&pConfig->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Z, SPI_PNT_SPRITE_SEL_0);
    SET_REG_FIELD(&pConfig->psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_W, SPI_PNT_SPRITE_SEL_1);
  }

  if (m_pipelineState->getPalAbiVersion() >= 456) {
    setPsUsesUavs(resUsage->resourceWrite || resUsage->resourceRead);
    setPsWritesUavs(resUsage->resourceWrite);
    setPsWritesDepth(builtInUsage.fragDepth);
  } else
    setPsUsesUavs(static_cast<unsigned>(resUsage->resourceWrite));

  if (m_pipelineState->getRasterizerState().innerCoverage) {
    SET_REG_FIELD(&pConfig->psRegs, PA_SC_AA_CONFIG, COVERAGE_TO_SHADER_SELECT, INPUT_INNER_COVERAGE);
  } else {
    SET_REG_FIELD(&pConfig->psRegs, PA_SC_AA_CONFIG, COVERAGE_TO_SHADER_SELECT, INPUT_COVERAGE);
  }

  const unsigned loadCollisionWaveId = GET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC2_PS, LOAD_COLLISION_WAVEID);
  const unsigned loadIntrawaveCollision =
      GET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);

  SET_REG_CORE_FIELD(&pConfig->psRegs, PA_SC_SHADER_CONTROL, LOAD_COLLISION_WAVEID, loadCollisionWaveId);
  SET_REG_CORE_FIELD(&pConfig->psRegs, PA_SC_SHADER_CONTROL, LOAD_INTRAWAVE_COLLISION, loadIntrawaveCollision);

  setNumAvailSgprs(Util::Abi::HardwareStage::Ps, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Ps, resUsage->numVgprsAvailable);

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportSpiPrefPriority) {
    SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_USER_ACCUM_PS_0, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_USER_ACCUM_PS_1, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_USER_ACCUM_PS_2, CONTRIBUTION, 1);
    SET_REG_FIELD(&pConfig->psRegs, SPI_SHADER_USER_ACCUM_PS_3, CONTRIBUTION, 1);
  }

  // Set shader user data mapping
  buildUserDataConfig(shaderStage, ShaderStageInvalid, mmSPI_SHADER_USER_DATA_PS_0);
}

// =====================================================================================================================
// Builds register configuration for compute shader.
//
// @param shaderStage : Current shader stage (from API side)
// @param [out] config : Register configuration for compute
void ConfigBuilder::buildCsRegConfig(ShaderStage shaderStage, CsRegConfig *config) {
  assert(shaderStage == ShaderStageCompute);

  const auto intfData = m_pipelineState->getShaderInterfaceData(shaderStage);
  const auto &shaderOptions = m_pipelineState->getShaderOptions(shaderStage);
  const auto resUsage = m_pipelineState->getShaderResourceUsage(shaderStage);
  const auto &builtInUsage = resUsage->builtInUsage.cs;
  const auto &computeMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  unsigned workgroupSizes[3];

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

  GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  if (gfxIp.major == 10) {
    bool wgpMode = getShaderWgpMode(ShaderStageCompute);

    SET_REG_GFX10_FIELD(config, COMPUTE_PGM_RSRC1, MEM_ORDERED, true);
    SET_REG_GFX10_FIELD(config, COMPUTE_PGM_RSRC1, WGP_MODE, wgpMode);
    unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageCompute);
    assert(waveSize == 32 || waveSize == 64);
    if (m_pipelineState->getPalAbiVersion() < 495) {
      if (waveSize == 32) {
        // For GFX10 pipeline, PAL expects to get CS_W32_EN from pipeline metadata,
        // other fields of this register are set by PAL.
        SET_REG_GFX10_FIELD(config, COMPUTE_DISPATCH_INITIATOR, CS_W32_EN, true);
      }
    } else
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

  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_X, NUM_THREAD_FULL, workgroupSizes[0]);
  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_Y, NUM_THREAD_FULL, workgroupSizes[1]);
  SET_REG_FIELD(config, COMPUTE_NUM_THREAD_Z, NUM_THREAD_FULL, workgroupSizes[2]);

  setNumAvailSgprs(Util::Abi::HardwareStage::Cs, resUsage->numSgprsAvailable);
  setNumAvailVgprs(Util::Abi::HardwareStage::Cs, resUsage->numVgprsAvailable);

  if (m_pipelineState->getTargetInfo().getGpuProperty().supportSpiPrefPriority) {
    SET_REG_FIELD(config, COMPUTE_USER_ACCUM_0, CONTRIBUTION, 1);
    SET_REG_FIELD(config, COMPUTE_USER_ACCUM_1, CONTRIBUTION, 1);
    SET_REG_FIELD(config, COMPUTE_USER_ACCUM_2, CONTRIBUTION, 1);
    SET_REG_FIELD(config, COMPUTE_USER_ACCUM_3, CONTRIBUTION, 1);
  }

  // Set shader user data mapping
  buildUserDataConfig(shaderStage, ShaderStageInvalid, mmCOMPUTE_USER_DATA_0);
}

// =====================================================================================================================
// Builds user data configuration for the specified shader stage.
//
// @param shaderStage1 : Current first shader stage (from API side)
// @param shaderStage2 : Current second shader stage (from API side)
// @param startUserData : Starting user data
void ConfigBuilder::buildUserDataConfig(ShaderStage shaderStage1, ShaderStage shaderStage2, unsigned startUserData) {
  assert(shaderStage1 != ShaderStageInvalid); // The first shader stage must be a valid one

  // NOTE: For merged shader, the second shader stage should be tessellation control shader (LS-HS) or geometry
  // shader (ES-GS).
  assert(shaderStage2 == ShaderStageTessControl || shaderStage2 == ShaderStageGeometry ||
         shaderStage2 == ShaderStageInvalid);

  bool enableMultiView = m_pipelineState->getInputAssemblyState().enableMultiView;

  bool enableXfb = false;
  if (m_pipelineState->isGraphics()) {
    if ((shaderStage1 == ShaderStageVertex || shaderStage1 == ShaderStageTessEval) &&
        shaderStage2 == ShaderStageInvalid)
      enableXfb = m_pipelineState->getShaderResourceUsage(shaderStage1)->inOutUsage.enableXfb;
  }

  const bool enableNgg = m_pipelineState->isGraphics() ? m_pipelineState->getNggControl()->enableNgg : false;
  (void(enableNgg)); // unused

  const auto intfData1 = m_pipelineState->getShaderInterfaceData(shaderStage1);
  const auto &entryArgIdxs1 = intfData1->entryArgIdxs;
  (void(entryArgIdxs1)); // unused

  const auto resUsage1 = m_pipelineState->getShaderResourceUsage(shaderStage1);
  const auto &builtInUsage1 = resUsage1->builtInUsage;

  const auto intfData2 =
      shaderStage2 != ShaderStageInvalid ? m_pipelineState->getShaderInterfaceData(shaderStage2) : nullptr;

  // Stage-specific processing
  if (shaderStage1 == ShaderStageVertex) {
    // TODO: PAL only check BaseVertex now, we need update code once PAL check them separately.
    if (builtInUsage1.vs.baseVertex || builtInUsage1.vs.baseInstance) {
      assert(entryArgIdxs1.vs.baseVertex > 0);
      appendConfig(startUserData + intfData1->userDataUsage.vs.baseVertex,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::BaseVertex));

      assert(entryArgIdxs1.vs.baseInstance > 0);
      appendConfig(startUserData + intfData1->userDataUsage.vs.baseInstance,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::BaseInstance));
    }

    if (builtInUsage1.vs.drawIndex) {
      assert(entryArgIdxs1.vs.drawIndex > 0);
      appendConfig(startUserData + intfData1->userDataUsage.vs.drawIndex,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::DrawIndex));
    }

    if (intfData1->userDataUsage.vs.vbTablePtr > 0) {
      assert(intfData1->userDataMap[intfData1->userDataUsage.vs.vbTablePtr] == InterfaceData::UserDataUnmapped);

      appendConfig(startUserData + intfData1->userDataUsage.vs.vbTablePtr,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::VertexBufferTable));
    }

    if (enableXfb && intfData1->userDataUsage.vs.streamOutTablePtr > 0 && shaderStage2 == ShaderStageInvalid) {
      assert(intfData1->userDataMap[intfData1->userDataUsage.vs.streamOutTablePtr] == InterfaceData::UserDataUnmapped);

      appendConfig(startUserData + intfData1->userDataUsage.vs.streamOutTablePtr,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::StreamOutTable));
    }

    if (enableMultiView) {
      if (shaderStage2 == ShaderStageInvalid || shaderStage2 == ShaderStageTessControl) {
        // Act as hardware VS or LS-HS merged shader
        assert(entryArgIdxs1.vs.viewIndex > 0);
        appendConfig(startUserData + intfData1->userDataUsage.vs.viewIndex,
                     static_cast<unsigned>(Util::Abi::UserDataMapping::ViewId));
      } else if (shaderStage2 == ShaderStageGeometry) {
        // Act as hardware ES-GS merged shader
        const auto &entryArgIdxs2 = intfData2->entryArgIdxs;

        assert(entryArgIdxs1.vs.viewIndex > 0 && entryArgIdxs2.gs.viewIndex > 0);
        (void(entryArgIdxs2)); // unused
        assert(intfData1->userDataUsage.vs.viewIndex == intfData2->userDataUsage.gs.viewIndex);
        appendConfig(startUserData + intfData1->userDataUsage.vs.viewIndex,
                     static_cast<unsigned>(Util::Abi::UserDataMapping::ViewId));
      } else
        llvm_unreachable("Should never be called!");
    }

    if (shaderStage2 == ShaderStageGeometry) {
      if (intfData2->userDataUsage.gs.esGsLdsSize > 0) {
        appendConfig(startUserData + intfData2->userDataUsage.gs.esGsLdsSize,
                     static_cast<unsigned>(Util::Abi::UserDataMapping::EsGsLdsSize));
      }
    } else if (shaderStage2 == ShaderStageInvalid) {
      if (intfData1->userDataUsage.vs.esGsLdsSize > 0) {
        assert(enableNgg);
        appendConfig(startUserData + intfData1->userDataUsage.vs.esGsLdsSize,
                     static_cast<unsigned>(Util::Abi::UserDataMapping::EsGsLdsSize));
      }
    }
  } else if (shaderStage1 == ShaderStageTessEval) {
    if (enableXfb && intfData1->userDataUsage.tes.streamOutTablePtr > 0 && shaderStage2 == ShaderStageInvalid) {
      assert(intfData1->userDataMap[intfData1->userDataUsage.tes.streamOutTablePtr] == InterfaceData::UserDataUnmapped);

      appendConfig(startUserData + intfData1->userDataUsage.tes.streamOutTablePtr,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::StreamOutTable));
    }

    if (enableMultiView) {
      if (shaderStage2 == ShaderStageInvalid) {
        // Act as hardware VS
        assert(entryArgIdxs1.tes.viewIndex > 0);
        appendConfig(startUserData + intfData1->userDataUsage.tes.viewIndex,
                     static_cast<unsigned>(Util::Abi::UserDataMapping::ViewId));
      } else if (shaderStage2 == ShaderStageGeometry) {
        // Act as hardware ES-GS merged shader
        const auto &entryArgIdxs2 = intfData2->entryArgIdxs;

        assert(entryArgIdxs1.tes.viewIndex > 0 && entryArgIdxs2.gs.viewIndex > 0);
        (void(entryArgIdxs2)); // unused
        assert(intfData1->userDataUsage.tes.viewIndex == intfData2->userDataUsage.gs.viewIndex);
        appendConfig(startUserData + intfData1->userDataUsage.tes.viewIndex,
                     static_cast<unsigned>(Util::Abi::UserDataMapping::ViewId));
      }
    }

    if (intfData1->userDataUsage.tes.esGsLdsSize > 0) {
      assert(enableNgg);
      appendConfig(startUserData + intfData1->userDataUsage.tes.esGsLdsSize,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::EsGsLdsSize));
    }
  } else if (shaderStage1 == ShaderStageGeometry) {
    assert(shaderStage2 == ShaderStageInvalid);

    if (enableMultiView) {
      assert(entryArgIdxs1.gs.viewIndex > 0);
      appendConfig(startUserData + intfData1->userDataUsage.gs.viewIndex,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::ViewId));
    }

    if (intfData1->userDataUsage.gs.esGsLdsSize > 0) {
      appendConfig(startUserData + intfData1->userDataUsage.gs.esGsLdsSize,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::EsGsLdsSize));
    }
  } else if (shaderStage1 == ShaderStageCompute) {
    assert(shaderStage2 == ShaderStageInvalid);

    if (builtInUsage1.cs.numWorkgroups > 0) {
      appendConfig(startUserData + intfData1->userDataUsage.cs.numWorkgroupsPtr,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::Workgroup));
    }
  }

  // NOTE: After user data nodes are merged together, any stage of merged shader are ought to have the same
  // configuration for general user data (apart from those special). In this sense, we are safe to use the first
  // shader stage to build user data register settings here.
  appendConfig(startUserData, static_cast<unsigned>(Util::Abi::UserDataMapping::GlobalTable));

  if (resUsage1->perShaderTable)
    appendConfig(startUserData + 1, static_cast<unsigned>(Util::Abi::UserDataMapping::PerShaderTable));

  // NOTE: For copy shader, we use fixed number of user data SGPRs. Thus, there is no need of building user data
  // registers here.
  if (shaderStage1 != ShaderStageCopyShader) {
    unsigned userDataLimit = 0;
    unsigned spillThreshold = UINT32_MAX;
    unsigned maxUserDataCount = m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount;
    for (unsigned i = 0; i < maxUserDataCount; ++i) {
      if (intfData1->userDataMap[i] != InterfaceData::UserDataUnmapped) {
        appendConfig(startUserData + i, intfData1->userDataMap[i]);
        if ((intfData1->userDataMap[i] & DescRelocMagicMask) != DescRelocMagic)
          userDataLimit = std::max(userDataLimit, intfData1->userDataMap[i] + 1);
      }
    }

    if (intfData1->userDataUsage.spillTable > 0) {
      appendConfig(startUserData + intfData1->userDataUsage.spillTable,
                   static_cast<unsigned>(Util::Abi::UserDataMapping::SpillTable));
      spillThreshold = intfData1->spillTable.offsetInDwords;
    }

    // If spill is in use, just say that all of the top-level resource node
    // table is in use except the vertex buffer table and streamout table.
    // Also do this if no user data nodes are used; PAL does not like userDataLimit being 0
    // if there are any user data nodes.
    if (intfData1->userDataUsage.spillTable > 0 || userDataLimit == 0) {
      for (auto &node : m_pipelineState->getUserDataNodes()) {
        if (node.type != ResourceNodeType::IndirectUserDataVaPtr && node.type != ResourceNodeType::StreamOutTableVaPtr)
          userDataLimit = std::max(userDataLimit, node.offsetInDwords + node.sizeInDwords);
      }
    }

    m_userDataLimit = std::max(m_userDataLimit, userDataLimit);
    m_spillThreshold = std::min(m_spillThreshold, spillThreshold);
  }
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
// Gets WGP mode enablement for the specified shader stage
//
// @param shaderStage : Shader stage
bool ConfigBuilder::getShaderWgpMode(ShaderStage shaderStage) const {
  if (shaderStage == ShaderStageCopyShader) {
    // Treat copy shader as part of geometry shader
    shaderStage = ShaderStageGeometry;
  }

  assert(shaderStage <= ShaderStageCompute);

  return m_pipelineState->getShaderOptions(shaderStage).wgpMode;
}

} // namespace Gfx9

} // namespace lgc
