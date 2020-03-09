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
 * @file  llpcGfx9ConfigBuilder.cpp
 * @brief LLPC header file: contains implementation of class Llpc::Gfx9::ConfigBuilder.
 ***********************************************************************************************************************
 */
#include "llpcBuilderBuiltIns.h"
#include "llpcCodeGenManager.h"
#include "llpcGfx9ConfigBuilder.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"
#include "llpcUtil.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-gfx9-config-builder"

namespace llvm
{

namespace cl
{

extern opt<bool> InRegEsGsLdsSize;

} // cl

} // llvm

namespace Llpc
{

namespace Gfx9
{

#include "gfx9_plus_merged_enum.h"
#include "gfx9_plus_merged_offset.h"

// =====================================================================================================================
// Builds PAL metadata for pipeline.
void ConfigBuilder::BuildPalMetadata()
{
    if (m_pPipelineState->IsGraphics() == false)
    {
        BuildPipelineCsRegConfig();
    }
    else
    {
        const bool hasTs = (m_hasTcs || m_hasTes);
        const bool enableNgg = m_pPipelineState->GetNggControl()->enableNgg;

        if ((hasTs == false) && (m_hasGs == false))
        {
            // VS-FS pipeline
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                BuildPipelineNggVsFsRegConfig();
            }
            else
            {
                BuildPipelineVsFsRegConfig();
            }
        }
        else if (hasTs && (m_hasGs == false))
        {
            // VS-TS-FS pipeline
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                BuildPipelineNggVsTsFsRegConfig();
            }
            else
            {
                BuildPipelineVsTsFsRegConfig();
            }
        }
        else if ((hasTs == false) && m_hasGs)
        {
            // VS-GS-FS pipeline
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                BuildPipelineNggVsGsFsRegConfig();
            }
            else
            {
                BuildPipelineVsGsFsRegConfig();
            }
        }
        else
        {
            // VS-TS-GS-FS pipeline
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                BuildPipelineNggVsTsGsFsRegConfig();
            }
            else
            {
                BuildPipelineVsTsGsFsRegConfig();
            }
        }
    }

    WritePalMetadata();
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-FS).
void ConfigBuilder::BuildPipelineVsFsRegConfig()      // [out] Size of register configuration
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineVsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::VsPs);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        BuildVsRegConfig<PipelineVsFsRegConfig>(ShaderStageVertex, &config);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageVertex);
        if ( waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
        }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);
        }
#endif

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        SET_REG(pConfig, VGT_GS_ONCHIP_CNTL, 0);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
        }
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineVsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
    // even if there are more than 2 shader engines on the GPU.
    uint32_t primGroupSize = 128;
    uint32_t numShaderEngines = m_pPipelineState->GetTargetInfo().GetGpuProperty().numShaderEngines;
    if (numShaderEngines > 2)
    {
        primGroupSize = alignTo(primGroupSize, 2);
    }

    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    }
    else
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-FS).
void ConfigBuilder::BuildPipelineVsTsFsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineVsTsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Tess);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);
    //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
    //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageTessControl)))
    {
        const bool hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);

        BuildLsHsRegConfig<PipelineVsTsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                             hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                             pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        checksum = checksum ^ SetShaderHash(ShaderStageTessControl);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageTessControl);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
        }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
        }
#endif
    }

    if (stageMask & ShaderStageToMask(ShaderStageTessEval))
    {
        BuildVsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageTessEval, &config);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_DS);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageTessEval);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);
        }
#endif

        uint32_t checksum = SetShaderHash(ShaderStageTessEval);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
        }
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineVsTsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& tesBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

    if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

        SET_REG_FIELD(pConfig, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, EsVertsOffchipGsOrTess);
        SET_REG_FIELD(pConfig, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, GsPrimsOffchipGsOrTess);
        SET_REG_FIELD(pConfig, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, GsPrimsOffchipGsOrTess);
    }
    else
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-GS-FS).
void ConfigBuilder::BuildPipelineVsGsFsRegConfig()      // [out] Size of register configuration
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineVsGsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Gs);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasVs = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        BuildEsGsRegConfig<PipelineVsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                             hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                             pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        checksum = checksum ^ SetShaderHash(ShaderStageGeometry);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
        }
#endif
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    if (stageMask & ShaderStageToMask(ShaderStageCopyShader))
    {
        BuildVsRegConfig<PipelineVsGsFsRegConfig>(ShaderStageCopyShader, &config);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageCopyShader);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);
        }
#endif
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const uint32_t primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    }
    else
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-GS-FS).
void ConfigBuilder::BuildPipelineVsTsGsFsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineVsTsGsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::GsTess);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageTessControl)))
    {
        const bool hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);

        BuildLsHsRegConfig<PipelineVsTsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                               hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                               pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        checksum = checksum ^ SetShaderHash(ShaderStageTessControl);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageTessControl);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
        }
#endif

        //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
        //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
        SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
    }

    if (stageMask & (ShaderStageToMask(ShaderStageTessEval) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasTes = ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0);
        const bool hasGs  = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        BuildEsGsRegConfig<PipelineVsTsGsFsRegConfig>(hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                               hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                               pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageTessEval);
        checksum = checksum ^ SetShaderHash(ShaderStageGeometry);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
        }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
        }
#endif
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    if (stageMask & ShaderStageToMask(ShaderStageCopyShader))
    {
        BuildVsRegConfig<PipelineVsTsGsFsRegConfig>(ShaderStageCopyShader, &config);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageCopyShader);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Vs, waveFrontSize);
        }
#endif
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& tesBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
    const auto& gsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

    // With tessellation, SWITCH_ON_EOI and PARTIAL_ES_WAVE_ON must be set if primitive ID is used by either the TCS, TES, or GS.
    if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveIdIn)
    {
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    }
    else
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    // Set up VGT_TF_PARAM
    SetupVgtTfParam(&pConfig->m_lsHsRegs);

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-FS).
void ConfigBuilder::BuildPipelineNggVsFsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    assert(gfxIp.major >= 10);

    const auto pNggControl = m_pPipelineState->GetNggControl();
    assert(pNggControl->enableNgg);

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineNggVsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Ngg);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, pNggControl->passthroughMode);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        BuildPrimShaderRegConfig<PipelineNggVsFsRegConfig>(ShaderStageVertex,
                                                                    ShaderStageInvalid,
                                                                    pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageVertex);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
        }
#endif

        uint32_t checksum = SetShaderHash(ShaderStageVertex);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineNggVsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
    // even if there are more than 2 shader engines on the GPU.
    uint32_t primGroupSize = 128;
    uint32_t numShaderEngines = m_pPipelineState->GetTargetInfo().GetGpuProperty().numShaderEngines;
    if (numShaderEngines > 2)
    {
        primGroupSize = alignTo(primGroupSize, 2);
    }

    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-FS).
void ConfigBuilder::BuildPipelineNggVsTsFsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    assert(gfxIp.major >= 10);

    const auto pNggControl = m_pPipelineState->GetNggControl();
    assert(pNggControl->enableNgg);

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineNggVsTsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::NggTess);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, pNggControl->passthroughMode);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageTessControl)))
    {
        const bool hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);

        BuildLsHsRegConfig<PipelineNggVsTsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                                hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                                pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        checksum = checksum ^ SetShaderHash(ShaderStageTessControl);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageTessControl);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
        }
#endif
    }

    if (stageMask & ShaderStageToMask(ShaderStageTessEval))
    {
        BuildPrimShaderRegConfig<PipelineNggVsTsFsRegConfig>(ShaderStageTessEval,
                                                                      ShaderStageInvalid,
                                                                      pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageTessEval);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
        }
#endif

        uint32_t checksum = SetShaderHash(ShaderStageTessEval);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineNggVsTsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;

    if (tcsBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-GS-FS).
void ConfigBuilder::BuildPipelineNggVsGsFsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    assert(gfxIp.major >= 10);

    assert(m_pPipelineState->GetNggControl()->enableNgg);

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineNggVsGsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Ngg);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
    // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
    // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
    // hardware pass-through).
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, false);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasVs = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        BuildPrimShaderRegConfig<PipelineNggVsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                                      hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                                      pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        checksum = checksum ^ SetShaderHash(ShaderStageGeometry);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
        }
#endif
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineNggVsGsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const uint32_t primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-GS-FS).
void ConfigBuilder::BuildPipelineNggVsTsGsFsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    assert(gfxIp.major >= 10);

    assert(m_pPipelineState->GetNggControl()->enableNgg);

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();

    PipelineNggVsTsGsFsRegConfig config(gfxIp);
    auto* pConfig = &config; // TODO: remove; this was added in refactoring to reduce the size of a diff

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::NggTess);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
    // NOTE: When GS is present, NGG pass-through mode is always turned off regardless of the pass-through flag of
    // NGG control settings. In such case, the pass-through flag means whether there is culling (different from
    // hardware pass-through).
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, false);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageTessControl)))
    {
        const bool hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);

        BuildLsHsRegConfig<PipelineNggVsTsGsFsRegConfig>(hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                                  hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                                  pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageVertex);
        checksum = checksum ^ SetShaderHash(ShaderStageTessControl);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageTessControl);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Hs, waveFrontSize);
        }
#endif
    }

    if (stageMask & (ShaderStageToMask(ShaderStageTessEval) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasTes = ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0);
        const bool hasGs  = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        BuildPrimShaderRegConfig<PipelineNggVsTsGsFsRegConfig>(hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                               hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                               pConfig);

        uint32_t checksum = SetShaderHash(ShaderStageTessEval);
        checksum = checksum ^ SetShaderHash(ShaderStageGeometry);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

        auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
        if (waveFrontSize == 32)
        {
            SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_W32_EN, true);
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
        if (gfxIp.major >= 10)
        {
            SetWaveFrontSize(Util::Abi::HardwareStage::Gs, waveFrontSize);
        }
#endif
    }

    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        BuildPsRegConfig<PipelineNggVsTsGsFsRegConfig>(ShaderStageFragment, &config);

        uint32_t checksum = SetShaderHash(ShaderStageFragment);

        if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
        {
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& gsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

    if (tcsBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveIdIn)
    {
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    // Set up VGT_TF_PARAM
    SetupVgtTfParam(&pConfig->m_lsHsRegs);

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for compute pipeline.
void ConfigBuilder::BuildPipelineCsRegConfig()
{
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    assert(m_pPipelineState->GetShaderStageMask() == ShaderStageToMask(ShaderStageCompute));

    CsRegConfig config(gfxIp);

    AddApiHwShaderMapping(ShaderStageCompute, Util::Abi::HwShaderCs);

    SetPipelineType(Util::Abi::PipelineType::Cs);

    BuildCsRegConfig(ShaderStageCompute, &config);

    uint32_t checksum = SetShaderHash(ShaderStageCompute);

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportShaderPowerProfiling)
    {
        SET_REG_FIELD(&config, COMPUTE_SHADER_CHKSUM, CHECKSUM, checksum);
    }

    AppendConfig(config);
}

// =====================================================================================================================
// Builds register configuration for hardware vertex shader.
template <typename T>
void ConfigBuilder::BuildVsRegConfig(
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for vertex-shader-specific pipeline
{
    assert((shaderStage == ShaderStageVertex)   ||
                (shaderStage == ShaderStageTessEval) ||
                (shaderStage == ShaderStageCopyShader));

    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const auto pIntfData = m_pPipelineState->GetShaderInterfaceData(shaderStage);

    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage;

    uint32_t floatMode = SetupFloatingPointMode(shaderStage);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP, true);  // Follow PAL setting

    const auto& xfbStrides = pResUsage->inOutUsage.xfbStrides;
    bool enableXfb = pResUsage->inOutUsage.enableXfb;
    if (shaderStage == ShaderStageCopyShader)
    {
        // NOTE: For copy shader, we use fixed number of user data registers.
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, Llpc::CopyShaderUserSgprCount);
        SetNumAvailSgprs(Util::Abi::HardwareStage::Vs, m_pPipelineState->GetTargetInfo().GetGpuProperty().maxSgprsAvailable);
        SetNumAvailVgprs(Util::Abi::HardwareStage::Vs, m_pPipelineState->GetTargetInfo().GetGpuProperty().maxVgprsAvailable);

        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN,
            (pResUsage->inOutUsage.gs.outLocCount[0] > 0) && enableXfb);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN,
            pResUsage->inOutUsage.gs.outLocCount[1] > 0);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN,
            pResUsage->inOutUsage.gs.outLocCount[2] > 0);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN,
            pResUsage->inOutUsage.gs.outLocCount[3] > 0);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, RAST_STREAM,
            pResUsage->inOutUsage.gs.rasterStream);
    }
    else
    {
        const auto& shaderOptions = m_pPipelineState->GetShaderOptions(shaderStage);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, DEBUG_MODE, shaderOptions.debugMode);

        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, TRAP_PRESENT, shaderOptions.trapPresent);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, pIntfData->userDataCount);
        const bool userSgprMsb = (pIntfData->userDataCount > 31);

        if (gfxIp.major == 10)
        {
            SET_REG_GFX10_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
        }
        else
        {
            SET_REG_GFX9_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
        }

        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN, enableXfb);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN, false);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN, false);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN, false);

        SetNumAvailSgprs(Util::Abi::HardwareStage::Vs, pResUsage->numSgprsAvailable);
        SetNumAvailVgprs(Util::Abi::HardwareStage::Vs, pResUsage->numVgprsAvailable);
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_EN, enableXfb);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE0_EN, (xfbStrides[0] > 0));
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE1_EN, (xfbStrides[1] > 0));
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE2_EN, (xfbStrides[2] > 0));
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE3_EN, (xfbStrides[3] > 0));

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_0, STRIDE, xfbStrides[0] / sizeof(uint32_t));
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_1, STRIDE, xfbStrides[1] / sizeof(uint32_t));
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_2, STRIDE, xfbStrides[2] / sizeof(uint32_t));
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_3, STRIDE, xfbStrides[3] / sizeof(uint32_t));

    uint32_t streamBufferConfig = 0;
    for (auto i = 0; i < MaxGsStreams; ++i)
    {
        streamBufferConfig |= (pResUsage->inOutUsage.streamXfbBuffers[i] << (i * 4));
    }
    SET_REG(&pConfig->m_vsRegs, VGT_STRMOUT_BUFFER_CONFIG, streamBufferConfig);

    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, MEM_ORDERED, true);
    }

    uint8_t usrClipPlaneMask = m_pPipelineState->GetRasterizerState().usrClipPlaneMask;
    bool depthClipDisable = (m_pPipelineState->GetViewportState().depthClipEnable == false);
    bool rasterizerDiscardEnable = m_pPipelineState->GetRasterizerState().rasterizerDiscardEnable;
    bool disableVertexReuse = m_pPipelineState->GetInputAssemblyState().disableVertexReuse;

    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_0, (usrClipPlaneMask >> 0) & 0x1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_1, (usrClipPlaneMask >> 1) & 0x1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_2, (usrClipPlaneMask >> 2) & 0x1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_3, (usrClipPlaneMask >> 3) & 0x1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_4, (usrClipPlaneMask >> 4) & 0x1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, UCP_ENA_5, (usrClipPlaneMask >> 5) & 0x1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA,true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF, true); // DepthRange::ZeroToOne
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE,depthClipDisable);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE, depthClipDisable);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, DX_RASTERIZATION_KILL,rasterizerDiscardEnable);

    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VPORT_X_SCALE_ENA, true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VPORT_X_OFFSET_ENA, true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VPORT_Y_SCALE_ENA, true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VPORT_Y_OFFSET_ENA, true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VPORT_Z_SCALE_ENA, true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VPORT_Z_OFFSET_ENA, true);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VTE_CNTL, VTX_W0_FMT, true);

    SET_REG_FIELD(&pConfig->m_vsRegs, PA_SU_VTX_CNTL, PIX_CENTER, 1);
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_SU_VTX_CNTL, ROUND_MODE, 2); // Round to even
    SET_REG_FIELD(&pConfig->m_vsRegs, PA_SU_VTX_CNTL, QUANT_MODE, 5); // Use 8-bit fractions

    // Stage-specific processing
    bool usePointSize = false;
    bool usePrimitiveId = false;
    bool useLayer = false;
    bool useViewportIndex = false;
    uint32_t clipDistanceCount = 0;
    uint32_t cullDistanceCount = 0;

    if (shaderStage == ShaderStageVertex)
    {
        usePointSize      = builtInUsage.vs.pointSize;
        usePrimitiveId    = builtInUsage.vs.primitiveId;
        useLayer          = builtInUsage.vs.layer;
        useViewportIndex  = builtInUsage.vs.viewportIndex;
        clipDistanceCount = builtInUsage.vs.clipDistance;
        cullDistanceCount = builtInUsage.vs.cullDistance;

        if (builtInUsage.vs.instanceIndex)
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable instance ID
        }
        else if (builtInUsage.vs.primitiveId)
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
        }
    }
    else if (shaderStage == ShaderStageTessEval)
    {
        usePointSize      = builtInUsage.tes.pointSize;
        usePrimitiveId    = builtInUsage.tes.primitiveId;
        useLayer          = builtInUsage.tes.layer;
        useViewportIndex  = builtInUsage.tes.viewportIndex;
        clipDistanceCount = builtInUsage.tes.clipDistance;
        cullDistanceCount = builtInUsage.tes.cullDistance;

        if (builtInUsage.tes.primitiveId)
        {
            // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 3); // 3: Enable primitive ID
        }
        else
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT, 2);
        }

        if (m_pPipelineState->IsTessOffChip())
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN, true);
        }
    }
    else
    {
        assert(shaderStage == ShaderStageCopyShader);

        usePointSize      = builtInUsage.gs.pointSize;
        usePrimitiveId    = builtInUsage.gs.primitiveIdIn;
        useLayer          = builtInUsage.gs.layer;
        useViewportIndex  = builtInUsage.gs.viewportIndex;
        clipDistanceCount = builtInUsage.gs.clipDistance;
        cullDistanceCount = builtInUsage.gs.cullDistance;

        // NOTE: For ES-GS merged shader, the actual use of primitive ID should take both ES and GS into consideration.
        const bool hasTs = ((m_pPipelineState->GetShaderStageMask() & (ShaderStageToMask(ShaderStageTessControl) |
                                                               ShaderStageToMask(ShaderStageTessEval))) != 0);
        if (hasTs)
        {
            const auto& tesBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
            usePrimitiveId = usePrimitiveId || tesBuiltInUsage.primitiveId;
        }
        else
        {
            const auto& vsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
            usePrimitiveId = usePrimitiveId || vsBuiltInUsage.primitiveId;
        }

        const auto pGsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry);
        if (m_pPipelineState->IsGsOnChip() && cl::InRegEsGsLdsSize)
        {
            assert(pGsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize != 0);

            AppendConfig(mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }

        if (enableXfb)
        {
            assert(pGsIntfData->userDataUsage.gs.copyShaderStreamOutTable != 0);
            AppendConfig(mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderStreamOutTable,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, usePrimitiveId);

    if ((gfxIp.major >= 10) && (pResUsage->inOutUsage.expCount == 0))
    {
        SET_REG_GFX10_FIELD(&pConfig->m_vsRegs, SPI_VS_OUT_CONFIG, NO_PC_EXPORT, true);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, pResUsage->inOutUsage.expCount - 1);
    }

    SetUsesViewportArrayIndex(useViewportIndex);

    // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
    // index if each GS, TES, or VS invocation emits the same viewport array index for each vertex and we set
    // VTE_VPORT_PROVOKE_DISABLE.
    if (useViewportIndex)
    {
        // TODO: In the future, we can only disable vertex reuse only if viewport array index is emitted divergently
        // for each vertex.
        disableVertexReuse = true;
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, true);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, false);
    }

    if (m_pPipelineState->GetTargetInfo().GetGpuWorkarounds().gfx10.waTessIncorrectRelativeIndex)
    {
        disableVertexReuse = true;
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

    useLayer = useLayer || m_pPipelineState->GetInputAssemblyState().enableMultiView;

    if (usePointSize || useLayer || useViewportIndex)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);

        if (gfxIp.major == 9)
        {
        }
        else if (gfxIp.major == 10)
        {
        }
        else
        {
            llvm_unreachable("Not implemented!");
        }
    }

    if ((clipDistanceCount > 0) || (cullDistanceCount > 0))
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST0_VEC_ENA, true);
        if (clipDistanceCount + cullDistanceCount > 4)
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST1_VEC_ENA, true);
        }

        uint32_t clipDistanceMask = (1 << clipDistanceCount) - 1;
        uint32_t cullDistanceMask = (1 << cullDistanceCount) - 1;

        // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
        uint32_t paClVsOutCntl = GET_REG(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL);
        paClVsOutCntl |= clipDistanceMask;
        paClVsOutCntl |= (cullDistanceMask << 8);
        SET_REG(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, paClVsOutCntl);
    }

    uint32_t posCount = 1; // gl_Position is always exported
    if (usePointSize || useLayer || useViewportIndex)
    {
        ++posCount;
    }

    if (clipDistanceCount + cullDistanceCount > 0)
    {
        ++posCount;
        if (clipDistanceCount + cullDistanceCount > 4)
        {
            ++posCount;
        }
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_POS_FORMAT, POS0_EXPORT_FORMAT, SPI_SHADER_4COMP);
    if (posCount > 1)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_POS_FORMAT, POS1_EXPORT_FORMAT, SPI_SHADER_4COMP);
    }
    if (posCount > 2)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_POS_FORMAT, POS2_EXPORT_FORMAT, SPI_SHADER_4COMP);
    }
    if (posCount > 3)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_POS_FORMAT, POS3_EXPORT_FORMAT, SPI_SHADER_4COMP);
    }

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_3, CONTRIBUTION, 1);
    }

    // Set shader user data maping
    BuildUserDataConfig(shaderStage, ShaderStageInvalid, mmSPI_SHADER_USER_DATA_VS_0);
}

// =====================================================================================================================
// Builds register configuration for hardware local-hull merged shader.
template <typename T>
void ConfigBuilder::BuildLsHsRegConfig(
    ShaderStage         shaderStage1,   // Current first shader stage (from API side)
    ShaderStage         shaderStage2,   // Current second shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for local-hull-shader-specific pipeline
{
    assert((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageInvalid));
    assert((shaderStage2 == ShaderStageTessControl) || (shaderStage2 == ShaderStageInvalid));

    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const auto pTcsResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessControl);
    const auto& vsBuiltInUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;

    uint32_t floatMode =
        SetupFloatingPointMode((shaderStage2 != ShaderStageInvalid) ? shaderStage2 : shaderStage1);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DX10_CLAMP, true); // Follow PAL setting

    uint32_t lsVgtCompCnt = 1;
    if (vsBuiltInUsage.instanceIndex)
    {
        lsVgtCompCnt += 2; // Enable instance ID
    }
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, LS_VGPR_COMP_CNT, lsVgtCompCnt);

    const auto& pVsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex);
    const auto& pTcsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessControl);
    uint32_t userDataCount = std::max(pVsIntfData->userDataCount, pTcsIntfData->userDataCount);

    const auto& tcsShaderOptions = m_pPipelineState->GetShaderOptions(ShaderStageTessControl);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DEBUG_MODE, tcsShaderOptions.debugMode);

    const bool userSgprMsb = (userDataCount > 31);
    if (gfxIp.major == 10)
    {
        bool wgpMode = (GetShaderWgpMode(ShaderStageVertex) ||
                        GetShaderWgpMode(ShaderStageTessControl));

        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, MEM_ORDERED, true);
        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, WGP_MODE, wgpMode);
        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
    }
    else
    {
        SET_REG_GFX9_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
    }
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, TRAP_PRESENT, tcsShaderOptions.trapPresent);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR, userDataCount);

    // NOTE: On GFX7+, granularity for the LDS_SIZE field is 128. The range is 0~128 which allocates 0 to 16K
    // DWORDs.
    const auto& calcFactor = pTcsResUsage->inOutUsage.tcs.calcFactor;
    uint32_t ldsSizeInDwords = calcFactor.onChip.patchConstStart +
                               calcFactor.patchConstSize * calcFactor.patchCountPerThreadGroup;
    if (m_pPipelineState->IsTessOffChip())
    {
        ldsSizeInDwords = calcFactor.inPatchSize * calcFactor.patchCountPerThreadGroup;
    }

    const uint32_t ldsSizeDwordGranularity = 128u;
    const uint32_t ldsSizeDwordGranularityShift = 7u;
    uint32_t ldsSize = alignTo(ldsSizeInDwords, ldsSizeDwordGranularity) >> ldsSizeDwordGranularityShift;

    if (gfxIp.major == 9)
    {
        SET_REG_GFX9_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
    }
    else if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
    }
    else
    {
        llvm_unreachable("Not implemented!");
    }

    SetLdsSizeByteSize(Util::Abi::HardwareStage::Hs, ldsSizeInDwords * 4);

    // Minimum and maximum tessellation factors supported by the hardware.
    constexpr float MinTessFactor = 1.0f;
    constexpr float MaxTessFactor = 64.0f;
    SET_REG(&pConfig->m_lsHsRegs, VGT_HOS_MIN_TESS_LEVEL, FloatToBits(MinTessFactor));
    SET_REG(&pConfig->m_lsHsRegs, VGT_HOS_MAX_TESS_LEVEL, FloatToBits(MaxTessFactor));

    // Set VGT_LS_HS_CONFIG
    SET_REG_FIELD(&pConfig->m_lsHsRegs, VGT_LS_HS_CONFIG, NUM_PATCHES, calcFactor.patchCountPerThreadGroup);
    SET_REG_FIELD(&pConfig->m_lsHsRegs,
                  VGT_LS_HS_CONFIG,
                  HS_NUM_INPUT_CP,
                  m_pPipelineState->GetInputAssemblyState().patchControlPoints);

    auto hsNumOutputCp = m_pPipelineState->GetShaderModes()->GetTessellationMode().outputVertices;
    SET_REG_FIELD(&pConfig->m_lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_OUTPUT_CP, hsNumOutputCp);

    SetNumAvailSgprs(Util::Abi::HardwareStage::Hs, pTcsResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Hs, pTcsResUsage->numVgprsAvailable);

    // Set up VGT_TF_PARAM
    SetupVgtTfParam(&pConfig->m_lsHsRegs);

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_3, CONTRIBUTION, 1);
    }

    if (gfxIp.major == 9)
    {
        BuildUserDataConfig(
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx09::mmSPI_SHADER_USER_DATA_LS_0);
    }
    else if (gfxIp.major == 10)
    {
        BuildUserDataConfig(
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx10::mmSPI_SHADER_USER_DATA_HS_0);
    }
    else
    {
        llvm_unreachable("Not implemented!");
    }
}

// =====================================================================================================================
// Builds register configuration for hardware export-geometry merged shader.
template <typename T>
void ConfigBuilder::BuildEsGsRegConfig(
    ShaderStage         shaderStage1,   // Current first shader stage (from API side)
    ShaderStage         shaderStage2,   // Current second shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for export-geometry-shader-specific pipeline
{
    assert((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageTessEval) ||
                (shaderStage1 == ShaderStageInvalid));
    assert((shaderStage2 == ShaderStageGeometry) || (shaderStage2 == ShaderStageInvalid));

    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);

    const auto pVsResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex);
    const auto& vsBuiltInUsage = pVsResUsage->builtInUsage.vs;

    const auto pTesResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval);
    const auto& tesBuiltInUsage = pTesResUsage->builtInUsage.tes;

    const auto pGsResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    const auto& gsBuiltInUsage = pGsResUsage->builtInUsage.gs;
    const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
    const auto& gsInOutUsage   = pGsResUsage->inOutUsage;
    const auto& calcFactor     = gsInOutUsage.gs.calcFactor;

    uint32_t gsVgprCompCnt = 0;
    if ((calcFactor.inputVertices > 4) || gsBuiltInUsage.invocationId)
    {
        gsVgprCompCnt = 3;
    }
    else if (gsBuiltInUsage.primitiveIdIn)
    {
        gsVgprCompCnt = 2;
    }
    else if (calcFactor.inputVertices > 2)
    {
        gsVgprCompCnt = 1;
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT, gsVgprCompCnt);

    uint32_t floatMode =
        SetupFloatingPointMode((shaderStage2 != ShaderStageInvalid) ? shaderStage2 : shaderStage1);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

    const auto pVsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex);
    const auto pTesIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessEval);
    const auto pGsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry);
    uint32_t userDataCount = std::max((hasTs ? pTesIntfData->userDataCount : pVsIntfData->userDataCount),
                                      pGsIntfData->userDataCount);

    const auto& gsShaderOptions = m_pPipelineState->GetShaderOptions(ShaderStageGeometry);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, gsShaderOptions.debugMode);

    const bool userSgprMsb = (userDataCount > 31);
    if (gfxIp.major == 10)
    {
        bool wgpMode = (GetShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex) ||
                        GetShaderWgpMode(ShaderStageGeometry));

        SET_REG_GFX10_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
        SET_REG_GFX10_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);
        SET_REG_GFX10_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }
    else
    {
        SET_REG_GFX9_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, gsShaderOptions.trapPresent);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

    uint32_t esVgprCompCnt = 0;
    if (hasTs)
    {
        // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
        if (tesBuiltInUsage.primitiveId)
        {
            esVgprCompCnt = 3;
        }
        else
        {
            esVgprCompCnt = 2;
        }

        if (m_pPipelineState->IsTessOffChip())
        {
            SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN, true);
        }
    }
    else
    {
        if (vsBuiltInUsage.instanceIndex)
        {
            esVgprCompCnt = 3; // Enable instance ID
        }
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT, esVgprCompCnt);

    const auto ldsSizeDwordGranularityShift = m_pPipelineState->GetTargetInfo().GetGpuProperty().ldsSizeDwordGranularityShift;

    SET_REG_FIELD(&pConfig->m_esGsRegs,
                  SPI_SHADER_PGM_RSRC2_GS,
                  LDS_SIZE,
                  calcFactor.gsOnChipLdsSize >> ldsSizeDwordGranularityShift);
    SetLdsSizeByteSize(Util::Abi::HardwareStage::Gs, calcFactor.gsOnChipLdsSize * 4);
    SetEsGsLdsSize(calcFactor.esGsLdsSize * 4);

    uint32_t maxVertOut = std::max(1u, static_cast<uint32_t>(geometryMode.outputVertices));
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

    // TODO: Currently only support offchip GS
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);

    if (m_pPipelineState->IsGsOnChip())
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_ON);
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, false);

        SetEsGsLdsByteSize(calcFactor.esGsLdsSize * 4);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);
    }

    if (geometryMode.outputVertices <= 128)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128);
    }
    else if (geometryMode.outputVertices <= 256)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256);
    }
    else if (geometryMode.outputVertices <= 512)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_512);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_1024);
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);

    // NOTE: The value of field "GS_INST_PRIMS_IN_SUBGRP" should be strictly equal to the product of
    // VGT_GS_ONCHIP_CNTL.GS_PRIMS_PER_SUBGRP * VGT_GS_INSTANCE_CNT.CNT.
    const uint32_t gsInstPrimsInSubgrp =
        (geometryMode.invocations > 1) ? (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations) : 0;
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

    uint32_t gsVertItemSize0 = sizeof(uint32_t) * gsInOutUsage.gs.outLocCount[0];
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize0);

    uint32_t gsVertItemSize1 = sizeof(uint32_t) * gsInOutUsage.gs.outLocCount[1];
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_VERT_ITEMSIZE_1, ITEMSIZE, gsVertItemSize1);

    uint32_t gsVertItemSize2 = sizeof(uint32_t) * gsInOutUsage.gs.outLocCount[2];
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_VERT_ITEMSIZE_2, ITEMSIZE, gsVertItemSize2);

    uint32_t gsVertItemSize3 = sizeof(uint32_t) * gsInOutUsage.gs.outLocCount[3];
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_VERT_ITEMSIZE_3, ITEMSIZE, gsVertItemSize3);

    uint32_t gsVsRingOffset = gsVertItemSize0 * maxVertOut;
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GSVS_RING_OFFSET_1, OFFSET, gsVsRingOffset);

    gsVsRingOffset += gsVertItemSize1 * maxVertOut;
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GSVS_RING_OFFSET_2, OFFSET, gsVsRingOffset);

    gsVsRingOffset += gsVertItemSize2 * maxVertOut;
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GSVS_RING_OFFSET_3, OFFSET, gsVsRingOffset);

    if ((geometryMode.invocations > 1) || gsBuiltInUsage.invocationId)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
    }
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

    VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
    if (gsInOutUsage.outputMapLocCount == 0)
    {
        gsOutputPrimitiveType = POINTLIST;
    }
    else if (geometryMode.outputPrimitive == OutputPrimitives::Points)
    {
        gsOutputPrimitiveType = POINTLIST;
    }
    else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
    {
        gsOutputPrimitiveType = LINESTRIP;
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);

    // Set multi-stream output primitive type
    if ((gsVertItemSize1 > 0) || (gsVertItemSize2 > 0) || (gsVertItemSize3 > 0))
    {
        const static auto GS_OUT_PRIM_INVALID = 3u;
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_1,
            (gsVertItemSize1 > 0)? gsOutputPrimitiveType: GS_OUT_PRIM_INVALID);

        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_2,
            (gsVertItemSize2 > 0) ? gsOutputPrimitiveType : GS_OUT_PRIM_INVALID);

        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_3,
            (gsVertItemSize3 > 0) ? gsOutputPrimitiveType : GS_OUT_PRIM_INVALID);
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, calcFactor.gsVsRingItemSize);
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

    const uint32_t maxPrimsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, MaxGsThreadsPerSubgroup);

    if (gfxIp.major == 9)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs,
                      VGT_GS_MAX_PRIMS_PER_SUBGROUP,
                      MAX_PRIMS_PER_SUBGROUP,
                      maxPrimsPerSubgroup);
    }
    else if (gfxIp.major == 10)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs,
                      GE_MAX_OUTPUT_PER_SUBGROUP,
                      MAX_VERTS_PER_SUBGROUP,
                      maxPrimsPerSubgroup);
    }
    else
    {
        llvm_unreachable("Not implemented!");
    }

    SetNumAvailSgprs(Util::Abi::HardwareStage::Gs, pGsResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Gs, pGsResUsage->numVgprsAvailable);

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_3, CONTRIBUTION, 1);
    }

    if (gfxIp.major == 9)
    {
        BuildUserDataConfig(
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx09::mmSPI_SHADER_USER_DATA_ES_0);
    }
    else if (gfxIp.major == 10)
    {
        BuildUserDataConfig(
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx10::mmSPI_SHADER_USER_DATA_GS_0);
    }
    else
    {
        llvm_unreachable("Not implemented!");
    }
}

// =====================================================================================================================
// Builds register configuration for hardware primitive shader.
template <typename T>
void ConfigBuilder::BuildPrimShaderRegConfig(
    ShaderStage         shaderStage1,   // Current first shader stage (from API side)
    ShaderStage         shaderStage2,   // Current second shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for primitive-shader-specific pipeline
{
    assert((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageTessEval) ||
                (shaderStage1 == ShaderStageInvalid));
    assert((shaderStage2 == ShaderStageGeometry) || (shaderStage2 == ShaderStageInvalid));

    const auto gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();
    assert(gfxIp.major >= 10);

    const auto pNggControl = m_pPipelineState->GetNggControl();
    assert(pNggControl->enableNgg);

    const uint32_t stageMask = m_pPipelineState->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    const auto pVsResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageVertex);
    const auto& vsBuiltInUsage = pVsResUsage->builtInUsage.vs;

    const auto pTesResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageTessEval);
    const auto& tesBuiltInUsage = pTesResUsage->builtInUsage.tes;

    const auto pGsResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    const auto& gsBuiltInUsage = pGsResUsage->builtInUsage.gs;
    const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
    const auto& gsInOutUsage   = pGsResUsage->inOutUsage;
    const auto& calcFactor     = gsInOutUsage.gs.calcFactor;

    //
    // Build ES-GS specific configuration
    //
    uint32_t gsVgprCompCnt = 0;
    if (hasGs)
    {
        if ((calcFactor.inputVertices > 4) || gsBuiltInUsage.invocationId)
        {
            gsVgprCompCnt = 3;
        }
        else if (gsBuiltInUsage.primitiveIdIn)
        {
            gsVgprCompCnt = 2;
        }
        else if (calcFactor.inputVertices > 2)
        {
            gsVgprCompCnt = 1;
        }
    }
    else
    {
        // NOTE: When GS is absent, only those VGPRs are required: vtx0/vtx1 offset, vtx2/vtx3 offset,
        // primitive ID (only for VS).
        gsVgprCompCnt = hasTs ? 1 : (vsBuiltInUsage.primitiveId ? 2 : 1);
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT, gsVgprCompCnt);

    uint32_t floatMode =
        SetupFloatingPointMode((shaderStage2 != ShaderStageInvalid) ? shaderStage2 : shaderStage1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

    const auto pVsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex);
    const auto pTesIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessEval);
    const auto pGsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry);
    uint32_t userDataCount = std::max((hasTs ? pTesIntfData->userDataCount : pVsIntfData->userDataCount),
                                      pGsIntfData->userDataCount);

    const auto& gsShaderOptions = m_pPipelineState->GetShaderOptions(ShaderStageGeometry);
    bool wgpMode = GetShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    if (hasGs)
    {
        wgpMode = (wgpMode || GetShaderWgpMode(ShaderStageGeometry));
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, gsShaderOptions.debugMode);
    SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
    SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, gsShaderOptions.trapPresent);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

    const bool userSgprMsb = (userDataCount > 31);

    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }
    else
    {
        SET_REG_GFX9_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }

    uint32_t esVgprCompCnt = 0;
    if (hasTs)
    {
        // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
        if (tesBuiltInUsage.primitiveId)
        {
            esVgprCompCnt = 3;
        }
        else
        {
            esVgprCompCnt = 2;
        }

        if (m_pPipelineState->IsTessOffChip())
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN, true);
        }
    }
    else
    {
        if (vsBuiltInUsage.instanceIndex)
        {
            esVgprCompCnt = 3; // Enable instance ID
        }
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT, esVgprCompCnt);

    const auto ldsSizeDwordGranularityShift =
        m_pPipelineState->GetTargetInfo().GetGpuProperty().ldsSizeDwordGranularityShift;

    SET_REG_FIELD(&pConfig->m_primShaderRegs,
                  SPI_SHADER_PGM_RSRC2_GS,
                  LDS_SIZE,
                  calcFactor.gsOnChipLdsSize >> ldsSizeDwordGranularityShift);
    SetLdsSizeByteSize(Util::Abi::HardwareStage::Gs, calcFactor.gsOnChipLdsSize * 4);
    SetEsGsLdsSize(calcFactor.esGsLdsSize * 4);

    uint32_t maxVertOut = std::max(1u, static_cast<uint32_t>(geometryMode.outputVertices));
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);

    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);

    const uint32_t gsInstPrimsInSubgrp =
        (geometryMode.invocations > 1) ?
            (calcFactor.gsPrimsPerSubgroup * geometryMode.invocations) : calcFactor.gsPrimsPerSubgroup;
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

    uint32_t gsVertItemSize = 4 * gsInOutUsage.outputMapLocCount;
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize);

    if ((geometryMode.invocations > 1) || gsBuiltInUsage.invocationId)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_INSTANCE_CNT, CNT, geometryMode.invocations);
        if ((gfxIp.major > 10) || ((gfxIp.major == 10) && (gfxIp.minor >= 1)))
        {
            SET_REG_GFX10_1_PLUS_FIELD(&pConfig->m_primShaderRegs,
                                       VGT_GS_INSTANCE_CNT,
                                       EN_MAX_VERT_OUT_PER_GS_INSTANCE,
                                       calcFactor.enableMaxVertOut);
        }
    }
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

    VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = POINTLIST;
    if (hasGs)
    {
        // GS present
        if (gsInOutUsage.outputMapLocCount == 0)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if (geometryMode.outputPrimitive == OutputPrimitives::Points)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if (geometryMode.outputPrimitive == OutputPrimitives::LineStrip)
        {
            gsOutputPrimitiveType = LINESTRIP;
        }
        else if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
        {
            gsOutputPrimitiveType = TRISTRIP;
        }
        else
        {
            llvm_unreachable("Should never be called!");
        }
    }
    else if (hasTs)
    {
        // With tessellation
        const auto& tessMode = m_pPipelineState->GetShaderModes()->GetTessellationMode();
        if (tessMode.pointMode)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if (tessMode.primitiveMode == PrimitiveMode::Isolines)
        {
            gsOutputPrimitiveType = LINESTRIP;
        }
        else if ((tessMode.primitiveMode == PrimitiveMode::Triangles) ||
                 (tessMode.primitiveMode == PrimitiveMode::Quads))
        {
            gsOutputPrimitiveType = TRISTRIP;
        }
        else
        {
            llvm_unreachable("Should never be called!");
        }
    }
    else
    {
        // Without tessellation
        const auto topology = m_pPipelineState->GetInputAssemblyState().topology;
        if (topology == PrimitiveTopology::PointList)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if ((topology == PrimitiveTopology::LineList) ||
                 (topology == PrimitiveTopology::LineStrip) ||
                 (topology == PrimitiveTopology::LineListWithAdjacency) ||
                 (topology == PrimitiveTopology::LineStripWithAdjacency))
        {
            gsOutputPrimitiveType = LINESTRIP;
        }
        else if ((topology == PrimitiveTopology::TriangleList) ||
                 (topology == PrimitiveTopology::TriangleStrip) ||
                 (topology == PrimitiveTopology::TriangleFan) ||
                 (topology == PrimitiveTopology::TriangleListWithAdjacency) ||
                 (topology == PrimitiveTopology::TriangleStripWithAdjacency))
        {
            gsOutputPrimitiveType = TRISTRIP;
        }
        else
        {
            llvm_unreachable("Should never be called!");
        }
    }

    // TODO: Multiple output streams are not supported.
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, calcFactor.gsVsRingItemSize);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

    const uint32_t maxVertsPerSubgroup = std::min(gsInstPrimsInSubgrp * maxVertOut, NggMaxThreadsPerSubgroup);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, GE_MAX_OUTPUT_PER_SUBGROUP, MAX_VERTS_PER_SUBGROUP, maxVertsPerSubgroup);

    if (hasGs)
    {
        SetNumAvailSgprs(Util::Abi::HardwareStage::Gs, pGsResUsage->numSgprsAvailable);
        SetNumAvailVgprs(Util::Abi::HardwareStage::Gs, pGsResUsage->numVgprsAvailable);
    }
    else
    {
        if (hasTs)
        {
            SetNumAvailSgprs(Util::Abi::HardwareStage::Gs, pTesResUsage->numSgprsAvailable);
            SetNumAvailVgprs(Util::Abi::HardwareStage::Gs, pTesResUsage->numVgprsAvailable);
        }
        else
        {
            SetNumAvailSgprs(Util::Abi::HardwareStage::Gs, pVsResUsage->numSgprsAvailable);
            SetNumAvailVgprs(Util::Abi::HardwareStage::Gs, pVsResUsage->numVgprsAvailable);
        }
    }

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_3, CONTRIBUTION, 1);
    }

    //
    // Build VS specific configuration
    //
    uint8_t usrClipPlaneMask = m_pPipelineState->GetRasterizerState().usrClipPlaneMask;
    bool depthClipDisable = (m_pPipelineState->GetViewportState().depthClipEnable == false);
    bool rasterizerDiscardEnable = m_pPipelineState->GetRasterizerState().rasterizerDiscardEnable;
    bool disableVertexReuse = m_pPipelineState->GetInputAssemblyState().disableVertexReuse;

    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_0, (usrClipPlaneMask >> 0) & 0x1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_1, (usrClipPlaneMask >> 1) & 0x1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_2, (usrClipPlaneMask >> 2) & 0x1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_3, (usrClipPlaneMask >> 3) & 0x1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_4, (usrClipPlaneMask >> 4) & 0x1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, UCP_ENA_5, (usrClipPlaneMask >> 5) & 0x1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA,true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF, true); // DepthRange::ZeroToOne
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE,depthClipDisable);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE, depthClipDisable);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, DX_RASTERIZATION_KILL,rasterizerDiscardEnable);

    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VPORT_X_SCALE_ENA, true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VPORT_X_OFFSET_ENA, true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VPORT_Y_SCALE_ENA, true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VPORT_Y_OFFSET_ENA, true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VPORT_Z_SCALE_ENA, true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VPORT_Z_OFFSET_ENA, true);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VTE_CNTL, VTX_W0_FMT, true);

    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_SU_VTX_CNTL, PIX_CENTER, 1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_SU_VTX_CNTL, ROUND_MODE, 2); // Round to even
    SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_SU_VTX_CNTL, QUANT_MODE, 5); // Use 8-bit fractions

    // Stage-specific processing
    bool usePointSize = false;
    bool usePrimitiveId = false;
    bool useLayer = false;
    bool useViewportIndex = false;

    uint32_t clipDistanceCount = 0;
    uint32_t cullDistanceCount = 0;

    uint32_t expCount = 0;

    if (hasGs)
    {
        usePointSize      = gsBuiltInUsage.pointSize;
        usePrimitiveId    = gsBuiltInUsage.primitiveIdIn;
        useLayer          = gsBuiltInUsage.layer;
        useViewportIndex  = gsBuiltInUsage.viewportIndex;
        clipDistanceCount = gsBuiltInUsage.clipDistance;
        cullDistanceCount = gsBuiltInUsage.cullDistance;

        expCount = pGsResUsage->inOutUsage.expCount;

        // NOTE: For ES-GS merged shader, the actual use of primitive ID should take both ES and GS into consideration.
        if (hasTs)
        {
            usePrimitiveId = usePrimitiveId || tesBuiltInUsage.primitiveId;
        }
        else
        {
            usePrimitiveId = usePrimitiveId || vsBuiltInUsage.primitiveId;
        }
    }
    else
    {
        if (hasTs)
        {
            usePointSize      = tesBuiltInUsage.pointSize;
            useLayer          = tesBuiltInUsage.layer;
            useViewportIndex  = tesBuiltInUsage.viewportIndex;
            clipDistanceCount = tesBuiltInUsage.clipDistance;
            cullDistanceCount = tesBuiltInUsage.cullDistance;

            expCount = pTesResUsage->inOutUsage.expCount;
        }
        else
        {
            usePointSize      = vsBuiltInUsage.pointSize;
            usePrimitiveId    = vsBuiltInUsage.primitiveId;
            useLayer          = vsBuiltInUsage.layer;
            useViewportIndex  = vsBuiltInUsage.viewportIndex;
            clipDistanceCount = vsBuiltInUsage.clipDistance;
            cullDistanceCount = vsBuiltInUsage.cullDistance;

            expCount = pVsResUsage->inOutUsage.expCount;
        }
    }

    if (usePrimitiveId)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, true);

        // NOTE: If primitive ID is used and there is no GS present, the field NGG_DISABLE_PROVOK_REUSE must be
        // set to ensure provoking vertex reuse is disabled in the GE.
        if (m_hasGs == false)
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_PRIMITIVEID_EN, NGG_DISABLE_PROVOK_REUSE, true);
        }
    }

    if (expCount == 0)
    {
        // No generic output is present
        SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_VS_OUT_CONFIG, NO_PC_EXPORT, true);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, expCount - 1);
    }

    SetUsesViewportArrayIndex(useViewportIndex);

    // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
    // index if each GS, TES, or VS invocation emits the same viewport array index for each vertex and we set
    // VTE_VPORT_PROVOKE_DISABLE.
    if (useViewportIndex)
    {
        // TODO: In the future, we can only disable vertex reuse only if viewport array index is emitted divergently
        // for each vertex.
        disableVertexReuse = true;
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, true);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE, false);
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

    useLayer = useLayer || m_pPipelineState->GetInputAssemblyState().enableMultiView;

    if (usePointSize || useLayer || useViewportIndex)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);
    }

    if ((clipDistanceCount > 0) || (cullDistanceCount > 0))
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST0_VEC_ENA, true);
        if (clipDistanceCount + cullDistanceCount > 4)
        {
            SET_REG_FIELD(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, VS_OUT_CCDIST1_VEC_ENA, true);
        }

        uint32_t clipDistanceMask = (1 << clipDistanceCount) - 1;
        uint32_t cullDistanceMask = (1 << cullDistanceCount) - 1;

        // Set fields CLIP_DIST_ENA_0 ~ CLIP_DIST_ENA_7 and CULL_DIST_ENA_0 ~ CULL_DIST_ENA_7
        uint32_t paClVsOutCntl = GET_REG(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL);
        paClVsOutCntl |= clipDistanceMask;
        paClVsOutCntl |= (cullDistanceMask << 8);
        SET_REG(&pConfig->m_primShaderRegs, PA_CL_VS_OUT_CNTL, paClVsOutCntl);
    }

    uint32_t posCount = 1; // gl_Position is always exported
    if (usePointSize || useLayer || useViewportIndex)
    {
        ++posCount;
    }

    if (clipDistanceCount + cullDistanceCount > 0)
    {
        ++posCount;
        if (clipDistanceCount + cullDistanceCount > 4)
        {
            ++posCount;
        }
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_POS_FORMAT, POS0_EXPORT_FORMAT, SPI_SHADER_4COMP);
    if (posCount > 1)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_POS_FORMAT, POS1_EXPORT_FORMAT, SPI_SHADER_4COMP);
    }
    if (posCount > 2)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_POS_FORMAT, POS2_EXPORT_FORMAT, SPI_SHADER_4COMP);
    }
    if (posCount > 3)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_POS_FORMAT, POS3_EXPORT_FORMAT, SPI_SHADER_4COMP);
    }

    //
    // Build NGG configuration
    //
    assert(calcFactor.primAmpFactor >= 1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, GE_NGG_SUBGRP_CNTL, PRIM_AMP_FACTOR, calcFactor.primAmpFactor);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, GE_NGG_SUBGRP_CNTL, THDS_PER_SUBGRP, NggMaxThreadsPerSubgroup);

    // TODO: Support PIPELINE_PRIM_ID.
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_IDX_FORMAT, IDX0_EXPORT_FORMAT, SPI_SHADER_1COMP);

    if (pNggControl->passthroughMode)
    {
        INVALIDATE_REG(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_LO_GS);
    }
    else
    {
        // NOTE: For NGG culling mode, the primitive shader table that contains culling data might be accessed by
        // shader. PAL expects 64-bit address of that table and will program it into SPI_SHADER_PGM_LO_GS and
        // SPI_SHADER_PGM_HI_GS if we do not provide one. By setting SPI_SHADER_PGM_LO_GS to NggCullingData, we tell
        // PAL that we will not provide it and it is fine to use SPI_SHADER_PGM_LO_GS and SPI_SHADER_PGM_HI_GS as
        // the address of that table.
        SET_REG(&pConfig->m_primShaderRegs,
                SPI_SHADER_PGM_LO_GS,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::NggCullingData));
    }

    //
    // Build use data configuration
    //
    BuildUserDataConfig(
                 (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                 (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                 Gfx10::mmSPI_SHADER_USER_DATA_GS_0);
}

// =====================================================================================================================
// Builds register configuration for hardware pixel shader.
template <typename T>
void ConfigBuilder::BuildPsRegConfig(
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for pixel-shader-specific pipeline
{
    assert(shaderStage == ShaderStageFragment);

    const auto pIntfData = m_pPipelineState->GetShaderInterfaceData(shaderStage);
    const auto& shaderOptions = m_pPipelineState->GetShaderOptions(shaderStage);
    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage.fs;
    const auto& fragmentMode = m_pPipelineState->GetShaderModes()->GetFragmentShaderMode();

    uint32_t floatMode = SetupFloatingPointMode(shaderStage);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP, true);  // Follow PAL setting
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE, shaderOptions.debugMode);

    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, TRAP_PRESENT, shaderOptions.trapPresent);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR, pIntfData->userDataCount);

    const bool userSgprMsb = (pIntfData->userDataCount > 31);
    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, MEM_ORDERED, true);

        if (shaderOptions.waveBreakSize == Llpc::WaveBreakSize::DrawTime)
        {
            SetCalcWaveBreakSizeAtDrawTime(true);
        }
        else
        {
            SET_REG_GFX10_FIELD(&pConfig->m_psRegs, PA_SC_SHADER_CONTROL, WAVE_BREAK_REGION_SIZE,
                                static_cast<uint32_t>(shaderOptions.waveBreakSize));
        }

        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, PA_STEREO_CNTL, STEREO_MODE, STATE_STEREO_X);
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
    }
    else
    {
        SET_REG_GFX9_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
    }

    SET_REG_FIELD(&pConfig->m_psRegs, SPI_BARYC_CNTL, FRONT_FACE_ALL_BITS, true);
    if (fragmentMode.pixelCenterInteger)
    {
        // TRUE - Force floating point position to upper left corner of pixel (X.0, Y.0)
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_BARYC_CNTL, POS_FLOAT_ULC, true);
    }
    else if (builtInUsage.runAtSampleRate)
    {
        // 2 - Calculate per-pixel floating point position at iterated sample number
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_BARYC_CNTL, POS_FLOAT_LOCATION, 2);
    }
    else
    {
        // 0 - Calculate per-pixel floating point position at pixel center
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_BARYC_CNTL, POS_FLOAT_LOCATION, 0);
    }

    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, WALK_ALIGN8_PRIM_FITS_ST, true);
    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, WALK_FENCE_ENABLE, true);
    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, TILE_WALK_ORDER_ENABLE, true);
    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, PS_ITER_SAMPLE, builtInUsage.runAtSampleRate);

    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, SUPERTILE_WALK_ORDER_ENABLE, true);
    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE, true);
    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, FORCE_EOV_CNTDWN_ENABLE, true);
    SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_MODE_CNTL_1, FORCE_EOV_REZ_ENABLE, true);

    ZOrder zOrder = LATE_Z;
    bool execOnHeirFail = false;
    if (fragmentMode.earlyFragmentTests)
    {
        zOrder = EARLY_Z_THEN_LATE_Z;
    }
    else if (pResUsage->resourceWrite)
    {
        zOrder = LATE_Z;
        execOnHeirFail = true;
    }
    else if (shaderOptions.allowReZ)
    {
        zOrder = EARLY_Z_THEN_RE_Z;
    }
    else
    {
        zOrder = EARLY_Z_THEN_LATE_Z;
    }

    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, Z_ORDER, zOrder);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, KILL_ENABLE, builtInUsage.discard);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, Z_EXPORT_ENABLE, builtInUsage.fragDepth);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, STENCIL_TEST_VAL_EXPORT_ENABLE, builtInUsage.fragStencilRef);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, MASK_EXPORT_ENABLE, builtInUsage.sampleMask);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, ALPHA_TO_MASK_DISABLE,
                  (builtInUsage.sampleMask ||
                   (m_pPipelineState->GetColorExportState().alphaToCoverageEnable == false)));
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, DEPTH_BEFORE_SHADER, fragmentMode.earlyFragmentTests);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, EXEC_ON_NOOP,
                  (fragmentMode.earlyFragmentTests && pResUsage->resourceWrite));
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, EXEC_ON_HIER_FAIL, execOnHeirFail);

    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, PRE_SHADER_DEPTH_COVERAGE_ENABLE,
                            fragmentMode.postDepthCoverage);
    }

    uint32_t depthExpFmt = EXP_FORMAT_ZERO;
    if (builtInUsage.sampleMask)
    {
        depthExpFmt = EXP_FORMAT_32_ABGR;
    }
    else if (builtInUsage.fragStencilRef)
    {
        depthExpFmt = EXP_FORMAT_32_GR;
    }
    else if (builtInUsage.fragDepth)
    {
        depthExpFmt = EXP_FORMAT_32_R;
    }
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_Z_FORMAT, Z_EXPORT_FORMAT, depthExpFmt);

    uint32_t spiShaderColFormat = 0;
    uint32_t cbShaderMask = pResUsage->inOutUsage.fs.cbShaderMask;
    cbShaderMask = pResUsage->inOutUsage.fs.isNullFs ? 0 : cbShaderMask;
    const auto& expFmts = pResUsage->inOutUsage.fs.expFmts;
    for (uint32_t i = 0; i < MaxColorTargets; ++i)
    {
        // Set fields COL0_EXPORT_FORMAT ~ COL7_EXPORT_FORMAT
        spiShaderColFormat |= (expFmts[i] << (4 * i));
    }

    if ((spiShaderColFormat == 0) && (depthExpFmt == EXP_FORMAT_ZERO) && pResUsage->inOutUsage.fs.dummyExport)
    {
        // NOTE: Hardware requires that fragment shader always exports "something" (color or depth) to the SX.
        // If both SPI_SHADER_Z_FORMAT and SPI_SHADER_COL_FORMAT are zero, we need to override
        // SPI_SHADER_COL_FORMAT to export one channel to MRT0. This dummy export format will be masked
        // off by CB_SHADER_MASK.
        spiShaderColFormat = SPI_SHADER_32_R;
    }

    SET_REG(&pConfig->m_psRegs, SPI_SHADER_COL_FORMAT, spiShaderColFormat);

    SET_REG(&pConfig->m_psRegs, CB_SHADER_MASK, cbShaderMask);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_PS_IN_CONTROL, NUM_INTERP, pResUsage->inOutUsage.fs.interpInfo.size());

    auto waveFrontSize = m_pPipelineState->GetShaderWaveSize(ShaderStageFragment);
    if (waveFrontSize == 32)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, SPI_PS_IN_CONTROL, PS_W32_EN, true);
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 495
    if (gfxIp.major >= 10)
    {
        SetWaveFrontSize(Util::Abi::HardwareStage::Ps, waveFrontSize);
    }
#endif

    uint32_t pointCoordLoc = InvalidValue;
    if (pResUsage->inOutUsage.builtInInputLocMap.find(BuiltInPointCoord) !=
        pResUsage->inOutUsage.builtInInputLocMap.end())
    {
        // Get generic input corresponding to gl_PointCoord (to set the field PT_SPRITE_TEX)
        pointCoordLoc = pResUsage->inOutUsage.builtInInputLocMap[BuiltInPointCoord];
    }

    // NOTE: PAL expects at least one mmSPI_PS_INPUT_CNTL_0 register set, so we always patch it at least one if none
    // were identified in the shader.
    const std::vector<FsInterpInfo> dummyInterpInfo {{ 0, false, false, false }};
    const auto& fsInterpInfo = pResUsage->inOutUsage.fs.interpInfo;
    const auto* pInterpInfo = (fsInterpInfo.size() == 0) ? &dummyInterpInfo : &fsInterpInfo;

    for (uint32_t i = 0; i < pInterpInfo->size(); ++i)
    {
        auto interpInfoElem = (*pInterpInfo)[i];
        if (((interpInfoElem.loc     == InvalidFsInterpInfo.loc) &&
             (interpInfoElem.flat    == InvalidFsInterpInfo.flat) &&
             (interpInfoElem.custom  == InvalidFsInterpInfo.custom) &&
             (interpInfoElem.is16bit == InvalidFsInterpInfo.is16bit))) {
          interpInfoElem.loc = i;
        }

        regSPI_PS_INPUT_CNTL_0 spiPsInputCntl = {};
        spiPsInputCntl.bits.FLAT_SHADE = interpInfoElem.flat;
        spiPsInputCntl.bits.OFFSET = interpInfoElem.loc;

        if (interpInfoElem.custom)
        {
            // NOTE: Force parameter cache data to be read in passthrough mode.
            static const uint32_t PassThroughMode = (1 << 5);
            spiPsInputCntl.bits.FLAT_SHADE = true;
            spiPsInputCntl.bits.OFFSET |= PassThroughMode;
        }
        else
        {
            if (interpInfoElem.is16bit)
            {
                // NOTE: Enable 16-bit interpolation mode for non-passthrough mode. Attribute 0 is always valid.
                spiPsInputCntl.bits.FP16_INTERP_MODE = true;
                spiPsInputCntl.bits.ATTR0_VALID = true;
            }
        }

        if (pointCoordLoc == i)
        {
            spiPsInputCntl.bits.PT_SPRITE_TEX = true;

            // NOTE: Set the offset value to force hardware to select input defaults (no VS match).
            static const uint32_t UseDefaultVal = (1 << 5);
            spiPsInputCntl.bits.OFFSET = UseDefaultVal;
        }

        AppendConfig(mmSPI_PS_INPUT_CNTL_0 + i, spiPsInputCntl.u32All);
    }

    if (pointCoordLoc != InvalidValue)
    {
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_ENA, true);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_X, SPI_PNT_SPRITE_SEL_S);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Y, SPI_PNT_SPRITE_SEL_T);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_Z, SPI_PNT_SPRITE_SEL_0);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_INTERP_CONTROL_0, PNT_SPRITE_OVRD_W, SPI_PNT_SPRITE_SEL_1);
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 456
    SetPsUsesUavs(pResUsage->resourceWrite || pResUsage->resourceRead);
    SetPsWritesUavs(pResUsage->resourceWrite);
    SetPsWritesDepth(builtInUsage.fragDepth);
#else
    SetPsUsesUavs(static_cast<uint32_t>(pResUsage->resourceWrite));
#endif

    if (m_pPipelineState->GetRasterizerState().innerCoverage)
    {
        SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_AA_CONFIG, COVERAGE_TO_SHADER_SELECT, INPUT_INNER_COVERAGE);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_psRegs, PA_SC_AA_CONFIG, COVERAGE_TO_SHADER_SELECT, INPUT_COVERAGE);
    }

    const uint32_t loadCollisionWaveId =
        GET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, LOAD_COLLISION_WAVEID);
    const uint32_t  loadIntrawaveCollision =
        GET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);

    SET_REG_CORE_FIELD(&pConfig->m_psRegs, PA_SC_SHADER_CONTROL, LOAD_COLLISION_WAVEID, loadCollisionWaveId);
    SET_REG_CORE_FIELD(&pConfig->m_psRegs, PA_SC_SHADER_CONTROL, LOAD_INTRAWAVE_COLLISION, loadIntrawaveCollision);

    SetNumAvailSgprs(Util::Abi::HardwareStage::Ps, pResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Ps, pResUsage->numVgprsAvailable);

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_3, CONTRIBUTION, 1);
    }

    // Set shader user data mapping
    BuildUserDataConfig(shaderStage, ShaderStageInvalid, mmSPI_SHADER_USER_DATA_PS_0);
}

// =====================================================================================================================
// Builds register configuration for compute shader.
void ConfigBuilder::BuildCsRegConfig(
    ShaderStage          shaderStage,   // Current shader stage (from API side)
    CsRegConfig*         pConfig)       // [out] Register configuration for compute
{
    assert(shaderStage == ShaderStageCompute);

    const auto pIntfData = m_pPipelineState->GetShaderInterfaceData(shaderStage);
    const auto& shaderOptions = m_pPipelineState->GetShaderOptions(shaderStage);
    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage.cs;
    const auto& computeMode = m_pPipelineState->GetShaderModes()->GetComputeShaderMode();
    uint32_t workgroupSizes[3];

    switch (static_cast<WorkgroupLayout>(builtInUsage.workgroupLayout))
    {
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
    uint32_t floatMode = SetupFloatingPointMode(shaderStage);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC1, FLOAT_MODE, floatMode);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC1, DX10_CLAMP, true);  // Follow PAL setting
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC1, DEBUG_MODE, shaderOptions.debugMode);

    GfxIpVersion gfxIp = m_pPipelineState->GetTargetInfo().GetGfxIpVersion();

    if (gfxIp.major == 10)
    {
        bool wgpMode = GetShaderWgpMode(ShaderStageCompute);

        SET_REG_GFX10_FIELD(pConfig, COMPUTE_PGM_RSRC1, MEM_ORDERED, true);
        SET_REG_GFX10_FIELD(pConfig, COMPUTE_PGM_RSRC1, WGP_MODE, wgpMode);
        uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageCompute);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 495
        if (waveSize == 32)
        {
            // For GFX10 pipeline, PAL expects to get CS_W32_EN from pipeline metadata,
            // other fields of this register are set by PAL.
            SET_REG_GFX10_FIELD(pConfig, COMPUTE_DISPATCH_INITIATOR, CS_W32_EN, true);
        }
#else
        assert((waveSize == 32) || (waveSize == 64));
        SetWaveFrontSize(Util::Abi::HardwareStage::Cs, waveSize);
#endif
    }

    // Set registers based on shader interface data
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, TRAP_PRESENT, shaderOptions.trapPresent);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, USER_SGPR, pIntfData->userDataCount);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, TGID_X_EN, true);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, TGID_Y_EN, true);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, TGID_Z_EN, true);
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, TG_SIZE_EN, true);

    // 0 = X, 1 = XY, 2 = XYZ
    uint32_t tidigCompCnt = 0;
    if (workgroupSizes[2] > 1)
    {
        tidigCompCnt = 2;
    }
    else if (workgroupSizes[1] > 1)
    {
        tidigCompCnt = 1;
    }
    SET_REG_FIELD(pConfig, COMPUTE_PGM_RSRC2, TIDIG_COMP_CNT, tidigCompCnt);

    SET_REG_FIELD(pConfig, COMPUTE_NUM_THREAD_X, NUM_THREAD_FULL, workgroupSizes[0]);
    SET_REG_FIELD(pConfig, COMPUTE_NUM_THREAD_Y, NUM_THREAD_FULL, workgroupSizes[1]);
    SET_REG_FIELD(pConfig, COMPUTE_NUM_THREAD_Z, NUM_THREAD_FULL, workgroupSizes[2]);

    SetNumAvailSgprs(Util::Abi::HardwareStage::Cs, pResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Cs, pResUsage->numVgprsAvailable);

    if (m_pPipelineState->GetTargetInfo().GetGpuProperty().supportSpiPrefPriority)
    {
         SET_REG_FIELD(pConfig, COMPUTE_USER_ACCUM_0, CONTRIBUTION, 1);
        SET_REG_FIELD(pConfig, COMPUTE_USER_ACCUM_1, CONTRIBUTION, 1);
        SET_REG_FIELD(pConfig, COMPUTE_USER_ACCUM_2, CONTRIBUTION, 1);
        SET_REG_FIELD(pConfig, COMPUTE_USER_ACCUM_3, CONTRIBUTION, 1);
    }

    // Set shader user data mapping
    BuildUserDataConfig(shaderStage, ShaderStageInvalid, mmCOMPUTE_USER_DATA_0);
}

// =====================================================================================================================
// Builds user data configuration for the specified shader stage.
void ConfigBuilder::BuildUserDataConfig(
    ShaderStage shaderStage1,   // Current first shader stage (from API side)
    ShaderStage shaderStage2,   // Current second shader stage (from API side)
    uint32_t    startUserData)  // Starting user data
{
    assert(shaderStage1 != ShaderStageInvalid); // The first shader stage must be a valid one

    // NOTE: For merged shader, the second shader stage should be tessellation control shader (LS-HS) or geometry
    // shader (ES-GS).
    assert((shaderStage2 == ShaderStageTessControl) || (shaderStage2 == ShaderStageGeometry) ||
                (shaderStage2 == ShaderStageInvalid));

    bool enableMultiView = m_pPipelineState->GetInputAssemblyState().enableMultiView;

    bool enableXfb = false;
    if (m_pPipelineState->IsGraphics())
    {
        if (((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageTessEval)) &&
            (shaderStage2 == ShaderStageInvalid))
        {
            enableXfb = m_pPipelineState->GetShaderResourceUsage(shaderStage1)->inOutUsage.enableXfb;
        }
    }

    const bool enableNgg = m_pPipelineState->IsGraphics() ? m_pPipelineState->GetNggControl()->enableNgg : false;
    (void(enableNgg)); // unused

    const auto pIntfData1 = m_pPipelineState->GetShaderInterfaceData(shaderStage1);
    const auto& entryArgIdxs1 = pIntfData1->entryArgIdxs;
    (void(entryArgIdxs1)); // unused

    const auto pResUsage1 = m_pPipelineState->GetShaderResourceUsage(shaderStage1);
    const auto& builtInUsage1 = pResUsage1->builtInUsage;

    const auto pIntfData2 = (shaderStage2 != ShaderStageInvalid) ?
                                m_pPipelineState->GetShaderInterfaceData(shaderStage2) : nullptr;

    // Stage-specific processing
    if (shaderStage1 == ShaderStageVertex)
    {
        // TODO: PAL only check BaseVertex now, we need update code once PAL check them separately.
        if (builtInUsage1.vs.baseVertex || builtInUsage1.vs.baseInstance)
        {
            assert(entryArgIdxs1.vs.baseVertex > 0);
            AppendConfig(startUserData + pIntfData1->userDataUsage.vs.baseVertex,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::BaseVertex));

            assert(entryArgIdxs1.vs.baseInstance > 0);
            AppendConfig(startUserData + pIntfData1->userDataUsage.vs.baseInstance,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::BaseInstance));
        }

        if (builtInUsage1.vs.drawIndex)
        {
            assert(entryArgIdxs1.vs.drawIndex > 0);
            AppendConfig(startUserData + pIntfData1->userDataUsage.vs.drawIndex,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::DrawIndex));
        }

        if (pIntfData1->userDataUsage.vs.vbTablePtr > 0)
        {
            assert(pIntfData1->userDataMap[pIntfData1->userDataUsage.vs.vbTablePtr] ==
                InterfaceData::UserDataUnmapped);

            AppendConfig(startUserData + pIntfData1->userDataUsage.vs.vbTablePtr,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::VertexBufferTable));
        }

        if (enableXfb && (pIntfData1->userDataUsage.vs.streamOutTablePtr > 0) && (shaderStage2 == ShaderStageInvalid))
        {
            assert(pIntfData1->userDataMap[pIntfData1->userDataUsage.vs.streamOutTablePtr] ==
                InterfaceData::UserDataUnmapped);

            AppendConfig(startUserData + pIntfData1->userDataUsage.vs.streamOutTablePtr,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }

        if (enableMultiView)
        {
            if ((shaderStage2 == ShaderStageInvalid) || (shaderStage2 == ShaderStageTessControl))
            {
                // Act as hardware VS or LS-HS merged shader
                assert(entryArgIdxs1.vs.viewIndex > 0);
                AppendConfig(startUserData + pIntfData1->userDataUsage.vs.viewIndex,
                             static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
            else if (shaderStage2 == ShaderStageGeometry)
            {
                // Act as hardware ES-GS merged shader
                const auto& entryArgIdxs2 = pIntfData2->entryArgIdxs;

                assert((entryArgIdxs1.vs.viewIndex > 0) && (entryArgIdxs2.gs.viewIndex > 0));
                (void(entryArgIdxs2)); // unused
                assert(pIntfData1->userDataUsage.vs.viewIndex == pIntfData2->userDataUsage.gs.viewIndex);
                AppendConfig(startUserData + pIntfData1->userDataUsage.vs.viewIndex,
                             static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
            else
            {
                llvm_unreachable("Should never be called!");
            }
        }

        if (shaderStage2 == ShaderStageGeometry)
        {
            if (pIntfData2->userDataUsage.gs.esGsLdsSize > 0)
            {
                AppendConfig(startUserData + pIntfData2->userDataUsage.gs.esGsLdsSize,
                             static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
            }
        }
        else if (shaderStage2 == ShaderStageInvalid)
        {
            if (pIntfData1->userDataUsage.vs.esGsLdsSize > 0)
            {
                assert(enableNgg);
                AppendConfig(startUserData + pIntfData1->userDataUsage.vs.esGsLdsSize,
                             static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
            }
        }
    }
    else if (shaderStage1 == ShaderStageTessEval)
    {
        if (enableXfb && (pIntfData1->userDataUsage.tes.streamOutTablePtr > 0) && (shaderStage2 == ShaderStageInvalid))
        {
            assert(pIntfData1->userDataMap[pIntfData1->userDataUsage.tes.streamOutTablePtr] ==
                InterfaceData::UserDataUnmapped);

            AppendConfig(startUserData + pIntfData1->userDataUsage.tes.streamOutTablePtr,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }

        if (enableMultiView)
        {
            if (shaderStage2 == ShaderStageInvalid)
            {
                // Act as hardware VS
                assert(entryArgIdxs1.tes.viewIndex > 0);
                AppendConfig(startUserData + pIntfData1->userDataUsage.tes.viewIndex,
                             static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
            else if (shaderStage2 == ShaderStageGeometry)
            {
                // Act as hardware ES-GS merged shader
                const auto& entryArgIdxs2 = pIntfData2->entryArgIdxs;

                assert((entryArgIdxs1.tes.viewIndex > 0) && (entryArgIdxs2.gs.viewIndex > 0));
                (void(entryArgIdxs2)); // unused
                assert(pIntfData1->userDataUsage.tes.viewIndex == pIntfData2->userDataUsage.gs.viewIndex);
                AppendConfig(startUserData + pIntfData1->userDataUsage.tes.viewIndex,
                             static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
        }

        if (pIntfData1->userDataUsage.tes.esGsLdsSize > 0)
        {
            assert(enableNgg);
            AppendConfig(startUserData + pIntfData1->userDataUsage.tes.esGsLdsSize,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }
    }
    else if (shaderStage1 == ShaderStageGeometry)
    {
        assert(shaderStage2 == ShaderStageInvalid);

        if (enableMultiView)
        {
            assert(entryArgIdxs1.gs.viewIndex > 0);
            AppendConfig(startUserData + pIntfData1->userDataUsage.gs.viewIndex,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
        }

        if (pIntfData1->userDataUsage.gs.esGsLdsSize > 0)
        {
            AppendConfig(startUserData + pIntfData1->userDataUsage.gs.esGsLdsSize,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }
    }
    else if (shaderStage1 == ShaderStageCompute)
    {
        assert(shaderStage2 == ShaderStageInvalid);

        if (builtInUsage1.cs.numWorkgroups > 0)
        {
            AppendConfig(startUserData + pIntfData1->userDataUsage.cs.numWorkgroupsPtr,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::Workgroup));
        }
    }

    // NOTE: After user data nodes are merged together, any stage of merged shader are ought to have the same
    // configuration for general user data (apart from those special). In this sense, we are safe to use the first
    // shader stage to build user data register settings here.
    AppendConfig(startUserData, static_cast<uint32_t>(Util::Abi::UserDataMapping::GlobalTable));

    if (pResUsage1->perShaderTable)
    {
        AppendConfig(startUserData + 1, static_cast<uint32_t>(Util::Abi::UserDataMapping::PerShaderTable));
    }

    // NOTE: For copy shader, we use fixed number of user data SGPRs. Thus, there is no need of building user data
    // registers here.
    if (shaderStage1 != ShaderStageCopyShader)
    {
        uint32_t userDataLimit = 0;
        uint32_t spillThreshold = UINT32_MAX;
        uint32_t maxUserDataCount = m_pPipelineState->GetTargetInfo().GetGpuProperty().maxUserDataCount;
        for (uint32_t i = 0; i < maxUserDataCount; ++i)
        {
            if (pIntfData1->userDataMap[i] != InterfaceData::UserDataUnmapped)
            {
                AppendConfig(startUserData + i, pIntfData1->userDataMap[i]);
                if ((pIntfData1->userDataMap[i] & DescRelocMagicMask) != DescRelocMagic)
                {
                    userDataLimit = std::max(userDataLimit, pIntfData1->userDataMap[i] + 1);
                }

            }
        }

        if (pIntfData1->userDataUsage.spillTable > 0)
        {
            AppendConfig(startUserData + pIntfData1->userDataUsage.spillTable,
                         static_cast<uint32_t>(Util::Abi::UserDataMapping::SpillTable));
            userDataLimit = std::max(userDataLimit,
                                     pIntfData1->spillTable.offsetInDwords + pIntfData1->spillTable.sizeInDwords);
            spillThreshold = pIntfData1->spillTable.offsetInDwords;
        }

        m_userDataLimit = std::max(m_userDataLimit, userDataLimit);
        m_spillThreshold = std::min(m_spillThreshold, spillThreshold);
    }
}

// =====================================================================================================================
// Sets up the register value for VGT_TF_PARAM.
void ConfigBuilder::SetupVgtTfParam(
    LsHsRegConfig*  pConfig)   // [out] Register configuration for local-hull-shader-specific pipeline
{
    uint32_t primType  = InvalidValue;
    uint32_t partition = InvalidValue;
    uint32_t topology  = InvalidValue;

    const auto& tessMode = m_pPipelineState->GetShaderModes()->GetTessellationMode();

    assert(tessMode.primitiveMode != PrimitiveMode::Unknown);
    if (tessMode.primitiveMode == PrimitiveMode::Isolines)
    {
        primType = TESS_ISOLINE;
    }
    else if (tessMode.primitiveMode == PrimitiveMode::Triangles)
    {
        primType = TESS_TRIANGLE;
    }
    else if (tessMode.primitiveMode == PrimitiveMode::Quads)
    {
        primType = TESS_QUAD;
    }
    assert(primType != InvalidValue);

    assert(tessMode.vertexSpacing != VertexSpacing::Unknown);
    if (tessMode.vertexSpacing == VertexSpacing::Equal)
    {
        partition = PART_INTEGER;
    }
    else if (tessMode.vertexSpacing == VertexSpacing::FractionalOdd)
    {
        partition = PART_FRAC_ODD;
    }
    else if (tessMode.vertexSpacing == VertexSpacing::FractionalEven)
    {
        partition = PART_FRAC_EVEN;
    }
    assert(partition != InvalidValue);

    assert(tessMode.vertexOrder != VertexOrder::Unknown);
    if (tessMode.pointMode)
    {
        topology = OUTPUT_POINT;
    }
    else if (tessMode.primitiveMode == PrimitiveMode::Isolines)
    {
        topology = OUTPUT_LINE;
    }
    else if (tessMode.vertexOrder == VertexOrder::Cw)
    {
        topology = OUTPUT_TRIANGLE_CW;
    }
    else if (tessMode.vertexOrder == VertexOrder::Ccw)
    {
        topology = OUTPUT_TRIANGLE_CCW;
    }

    if (m_pPipelineState->GetInputAssemblyState().switchWinding)
    {
        if (topology == OUTPUT_TRIANGLE_CW)
        {
            topology = OUTPUT_TRIANGLE_CCW;
        }
        else if (topology == OUTPUT_TRIANGLE_CCW)
        {
            topology = OUTPUT_TRIANGLE_CW;
        }
    }

    assert(topology != InvalidValue);

    SET_REG_FIELD(pConfig, VGT_TF_PARAM, TYPE, primType);
    SET_REG_FIELD(pConfig, VGT_TF_PARAM, PARTITIONING, partition);
    SET_REG_FIELD(pConfig, VGT_TF_PARAM, TOPOLOGY, topology);

    if (m_pPipelineState->IsTessOffChip())
    {
        SET_REG_FIELD(pConfig, VGT_TF_PARAM, DISTRIBUTION_MODE, TRAPEZOIDS);
    }
}

// =====================================================================================================================
// Gets WGP mode enablement for the specified shader stage
bool ConfigBuilder::GetShaderWgpMode(
    ShaderStage shaderStage // Shader stage
    ) const
{
    if (shaderStage == ShaderStageCopyShader)
    {
        // Treat copy shader as part of geometry shader
        shaderStage = ShaderStageGeometry;
    }

    assert(shaderStage <= ShaderStageCompute);

    return m_pPipelineState->GetShaderOptions(shaderStage).wgpMode;
}

} // Gfx9

} // Llpc
