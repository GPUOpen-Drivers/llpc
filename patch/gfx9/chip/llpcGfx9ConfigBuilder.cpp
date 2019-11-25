/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#define DEBUG_TYPE "llpc-gfx9-config-builder"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcCodeGenManager.h"
#include "llpcElfReader.h"
#include "llpcGfx9ConfigBuilder.h"

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
    Result result = Result::Success;

    if (m_pContext->IsGraphics() == false)
    {
        result = BuildPipelineCsRegConfig(m_pContext, &m_pConfig, &m_configSize);
    }
    else
    {
        const bool hasTs = (m_hasTcs || m_hasTes);
#if LLPC_BUILD_GFX10
        const bool enableNgg = m_pContext->GetNggControl()->enableNgg;
#endif

        if ((hasTs == false) && (m_hasGs == false))
        {
            // VS-FS pipeline
#if LLPC_BUILD_GFX10
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                result = BuildPipelineNggVsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
            else
#endif
            {
                result = BuildPipelineVsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
        }
        else if (hasTs && (m_hasGs == false))
        {
            // VS-TS-FS pipeline
#if LLPC_BUILD_GFX10
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                result = BuildPipelineNggVsTsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
            else
#endif
            {
                result = BuildPipelineVsTsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
        }
        else if ((hasTs == false) && m_hasGs)
        {
            // VS-GS-FS pipeline
#if LLPC_BUILD_GFX10
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                result = BuildPipelineNggVsGsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
            else
#endif
            {
                result = BuildPipelineVsGsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
        }
        else
        {
            // VS-TS-GS-FS pipeline
#if LLPC_BUILD_GFX10
            if ((m_gfxIp.major >= 10) && enableNgg)
            {
                result = Gfx9::ConfigBuilder::BuildPipelineNggVsTsGsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
            else
#endif
            {
                result = Gfx9::ConfigBuilder::BuildPipelineVsTsGsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
            }
        }
    }

    LLPC_ASSERT(result == Result::Success);
    LLPC_UNUSED(result);

    WritePalMetadata();
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-FS).
Result ConfigBuilder::BuildPipelineVsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for VS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsFsRegConfig)];
    PipelineVsFsRegConfig* pConfig = reinterpret_cast<PipelineVsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::VsPs);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        result = BuildVsRegConfig<PipelineVsFsRegConfig>(pContext, ShaderStageVertex, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageVertex);
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
#endif

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, hash);
        SET_REG(pConfig, VGT_GS_ONCHIP_CNTL, 0);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
#endif
    }

    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
    // even if there are more than 2 shader engines on the GPU.
    uint32_t primGroupSize = 128;
    uint32_t numShaderEngines = pContext->GetGpuProperty()->numShaderEngines;
    if (numShaderEngines > 2)
    {
        primGroupSize = Pow2Align(primGroupSize, 2);
    }

    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

#if LLPC_BUILD_GFX10
    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    }
    else
#endif
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-FS).
Result ConfigBuilder::BuildPipelineVsTsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for VS-TS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsTsFsRegConfig)];
    PipelineVsTsFsRegConfig* pConfig = reinterpret_cast<PipelineVsTsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessControl, Util::Abi::HwShaderHs);
    AddApiHwShaderMapping(ShaderStageTessEval, Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Tess);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);
#if LLPC_BUILD_GFX10
    //  In GEN_TWO the only supported mode is fully distributed tessellation. The programming model is expected
    //  to set VGT_SHADER_STAGES_EN.DYNAMIC_HS=1 and VGT_TF_PARAM.NUM_DS_WAVES_PER_SIMD=0
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
#endif

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageTessControl)))
    {
        const bool hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);

        result = BuildLsHsRegConfig<PipelineVsTsFsRegConfig>(pContext,
                                                             hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                             hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                             pConfig);

        ShaderHash vsHash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, vsHash);

        ShaderHash tcsHash = pContext->GetShaderHashCode(ShaderStageTessControl);
        SetShaderHash(ShaderStageTessControl, tcsHash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = vsHash.upper ^ tcsHash.upper;
            hash.lower = vsHash.lower ^ tcsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(vsHash ^ tcsHash);
#endif
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }
#endif

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageTessControl);
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
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageTessEval)))
    {
        result = BuildVsRegConfig<PipelineVsTsFsRegConfig>(pContext, ShaderStageTessEval, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_DS);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageTessEval);
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
#endif

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageTessEval);
        SetShaderHash(ShaderStageTessEval, hash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_CHKSUM_VS, CHECKSUM, checksum);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsTsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
#endif
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& tesBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

    if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

#if LLPC_BUILD_GFX10
    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

        SET_REG_FIELD(pConfig, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, EsVertsOffchipGsOrTess);
        SET_REG_FIELD(pConfig, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, GsPrimsOffchipGsOrTess);
        SET_REG_FIELD(pConfig, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, GsPrimsOffchipGsOrTess);
    }
    else
#endif
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-GS-FS).
Result ConfigBuilder::BuildPipelineVsGsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for VS-GS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsGsFsRegConfig)];
    PipelineVsGsFsRegConfig* pConfig = reinterpret_cast<PipelineVsGsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageGeometry, Util::Abi::HwShaderGs | Util::Abi::HwShaderVs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Gs);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    if (stageMask & (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasVs = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
        const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        result = BuildEsGsRegConfig<PipelineVsGsFsRegConfig>(pContext,
                                                             hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                             hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                             pConfig);

        ShaderHash vsHash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, vsHash);

        ShaderHash gsHash = pContext->GetShaderHashCode(ShaderStageGeometry);
        SetShaderHash(ShaderStageGeometry, gsHash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = vsHash.upper ^ gsHash.upper;
            hash.lower = vsHash.lower ^ gsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(vsHash ^ gsHash);
#endif
            SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }
#endif

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageGeometry);
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
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsGsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageCopyShader)))
    {
        result = BuildVsRegConfig<PipelineVsGsFsRegConfig>(pContext, ShaderStageCopyShader, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageCopyShader);
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
#endif
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const uint32_t primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

#if LLPC_BUILD_GFX10
    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    }
    else
#endif
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (VS-TS-GS-FS).
Result ConfigBuilder::BuildPipelineVsTsGsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for VS-TS-GS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsTsGsFsRegConfig)];
    PipelineVsTsGsFsRegConfig* pConfig = reinterpret_cast<PipelineVsTsGsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

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

        result = BuildLsHsRegConfig<PipelineVsTsGsFsRegConfig>(pContext,
                                                               hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                               hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                               pConfig);

        ShaderHash vsHash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, vsHash);

        ShaderHash tcsHash = pContext->GetShaderHashCode(ShaderStageTessControl);
        SetShaderHash(ShaderStageTessControl, tcsHash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = vsHash.upper ^ tcsHash.upper;
            hash.lower = vsHash.lower ^ tcsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(vsHash ^ tcsHash);
#endif
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }
#endif

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageTessControl);
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
#endif

    }

    if (stageMask & (ShaderStageToMask(ShaderStageTessEval) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasTes = ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0);
        const bool hasGs  = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        result = BuildEsGsRegConfig<PipelineVsTsGsFsRegConfig>(pContext,
                                                               hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                               hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                               pConfig);

        ShaderHash tesHash = pContext->GetShaderHashCode(ShaderStageTessEval);
        SetShaderHash(ShaderStageTessEval, tesHash);

        ShaderHash gsHash = pContext->GetShaderHashCode(ShaderStageGeometry);
        SetShaderHash(ShaderStageGeometry, gsHash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = tesHash.upper ^ gsHash.upper;
            hash.lower = tesHash.lower ^ gsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(tesHash ^ gsHash);
#endif
            SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }
#endif

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageGeometry);
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

#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

#if LLPC_BUILD_GFX10
        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageCopyShader)))
    {
        result = BuildVsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageCopyShader, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageCopyShader);
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
#endif
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& tesBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
    const auto& gsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

    if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

#if LLPC_BUILD_GFX10
    if (gfxIp.major >= 10)
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);
    }
    else
#endif
    {
        SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);
    }

    // Set up VGT_TF_PARAM
    SetupVgtTfParam(pContext, &pConfig->m_lsHsRegs);

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-FS).
Result ConfigBuilder::BuildPipelineNggVsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for NGG VS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();
    LLPC_ASSERT(gfxIp.major >= 10);

    const auto pNggControl = pContext->GetNggControl();
    LLPC_ASSERT(pNggControl->enableNgg);

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineNggVsFsRegConfig)];
    PipelineNggVsFsRegConfig* pConfig = reinterpret_cast<PipelineNggVsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

    AddApiHwShaderMapping(ShaderStageVertex, Util::Abi::HwShaderGs);
    AddApiHwShaderMapping(ShaderStageFragment, Util::Abi::HwShaderPs);

    SetPipelineType(Util::Abi::PipelineType::Ngg);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, MAX_PRIMGRP_IN_WAVE, 2);

    SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_EN, true);
    SET_REG_GFX10_FIELD(pConfig, VGT_SHADER_STAGES_EN, PRIMGEN_PASSTHRU_EN, pNggControl->passthroughMode);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        result = BuildPrimShaderRegConfig<PipelineNggVsFsRegConfig>(pContext,
                                                                    ShaderStageVertex,
                                                                    ShaderStageInvalid,
                                                                    pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageVertex);
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
#endif

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, hash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineNggVsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    // When non-patch primitives are used without tessellation enabled, PRIMGROUP_SIZE must be at least 4, and must be
    // even if there are more than 2 shader engines on the GPU.
    uint32_t primGroupSize = 128;
    uint32_t numShaderEngines = pContext->GetGpuProperty()->numShaderEngines;
    if (numShaderEngines > 2)
    {
        primGroupSize = Pow2Align(primGroupSize, 2);
    }

    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-FS).
Result ConfigBuilder::BuildPipelineNggVsTsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for NGG VS-TS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();
    LLPC_ASSERT(gfxIp.major >= 10);

    const auto pNggControl = pContext->GetNggControl();
    LLPC_ASSERT(pNggControl->enableNgg);

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineNggVsTsFsRegConfig)];
    PipelineNggVsTsFsRegConfig* pConfig = reinterpret_cast<PipelineNggVsTsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

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

        result = BuildLsHsRegConfig<PipelineNggVsTsFsRegConfig>(pContext,
                                                                hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                                hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                                pConfig);

        ShaderHash vsHash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, vsHash);

        ShaderHash tcsHash = pContext->GetShaderHashCode(ShaderStageTessControl);
        SetShaderHash(ShaderStageTessControl, tcsHash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = vsHash.upper ^ tcsHash.upper;
            hash.lower = vsHash.lower ^ tcsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(vsHash ^ tcsHash);
#endif
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageTessControl);
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
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageTessEval)))
    {
        result = BuildPrimShaderRegConfig<PipelineNggVsTsFsRegConfig>(pContext,
                                                                      ShaderStageTessEval,
                                                                      ShaderStageInvalid,
                                                                      pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageTessEval);
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
#endif

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageTessEval);
        SetShaderHash(ShaderStageTessEval, hash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineNggVsTsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;

    if (tcsBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-GS-FS).
Result ConfigBuilder::BuildPipelineNggVsGsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for NGG VS-GS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();
    LLPC_ASSERT(gfxIp.major >= 10);

    LLPC_ASSERT(pContext->GetNggControl()->enableNgg);

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineNggVsGsFsRegConfig)];
    PipelineNggVsGsFsRegConfig* pConfig = reinterpret_cast<PipelineNggVsGsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

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

        result = BuildPrimShaderRegConfig<PipelineNggVsGsFsRegConfig>(pContext,
                                                                      hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                                      hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                                      pConfig);

        ShaderHash vsHash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, vsHash);

        ShaderHash gsHash = pContext->GetShaderHashCode(ShaderStageGeometry);
        SetShaderHash(ShaderStageGeometry, gsHash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = vsHash.upper ^ gsHash.upper;
            hash.lower = vsHash.lower ^ gsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(vsHash ^ gsHash);
#endif
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageGeometry);
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
#endif
        // TODO: Set Gfx10::mmGE_NGG_SUBGRP_CNTL and Gfx10::mmGE_MAX_OUTPUT_PER_SUBGROUP
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineNggVsGsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const uint32_t primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for graphics pipeline (NGG, VS-TS-GS-FS).
Result ConfigBuilder::BuildPipelineNggVsTsGsFsRegConfig(
    Context*            pContext,         // [in] LLPC context
    uint8_t**           ppConfig,         // [out] Register configuration for NGG VS-TS-GS-FS pipeline
    size_t*             pConfigSize)      // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();
    LLPC_ASSERT(gfxIp.major >= 10);

    LLPC_ASSERT(pContext->GetNggControl()->enableNgg);

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineNggVsTsGsFsRegConfig)];
    PipelineNggVsTsGsFsRegConfig* pConfig = reinterpret_cast<PipelineNggVsTsGsFsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

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

        result = BuildLsHsRegConfig<PipelineNggVsTsGsFsRegConfig>(pContext,
                                                                  hasVs ? ShaderStageVertex : ShaderStageInvalid,
                                                                  hasTcs ? ShaderStageTessControl : ShaderStageInvalid,
                                                                  pConfig);

        ShaderHash vsHash = pContext->GetShaderHashCode(ShaderStageVertex);
        SetShaderHash(ShaderStageVertex, vsHash);

        ShaderHash tcsHash = pContext->GetShaderHashCode(ShaderStageTessControl);
        SetShaderHash(ShaderStageTessControl, tcsHash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = vsHash.upper ^ tcsHash.upper;
            hash.lower = vsHash.lower ^ tcsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(vsHash ^ tcsHash);
#endif
            SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_CHKSUM_HS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);
#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageTessControl);
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
#endif
    }

    if (stageMask & (ShaderStageToMask(ShaderStageTessEval) | ShaderStageToMask(ShaderStageGeometry)))
    {
        const bool hasTes = ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0);
        const bool hasGs  = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

        result =
            BuildPrimShaderRegConfig<PipelineNggVsTsGsFsRegConfig>(pContext,
                                                                   hasTes ? ShaderStageTessEval : ShaderStageInvalid,
                                                                   hasGs ? ShaderStageGeometry : ShaderStageInvalid,
                                                                   pConfig);

        ShaderHash tesHash = pContext->GetShaderHashCode(ShaderStageTessEval);
        SetShaderHash(ShaderStageTessEval, tesHash);

        ShaderHash gsHash = pContext->GetShaderHashCode(ShaderStageGeometry);
        SetShaderHash(ShaderStageGeometry, gsHash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
            ShaderHash hash = {};
            hash.upper = tesHash.upper ^ gsHash.upper;
            hash.lower = tesHash.lower ^ gsHash.lower;
            uint32_t checksum = MetroHash::Compact32(hash);
#else
            uint32_t checksum = MetroHash::Compact32(tesHash ^ gsHash);
#endif
            SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_CHKSUM_GS, CHECKSUM, checksum);
        }

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);
#if LLPC_BUILD_GFX10
        auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageGeometry);
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
#endif

        // TODO: Set Gfx10::mmGE_NGG_SUBGRP_CNTL and Gfx10::mmGE_MAX_OUTPUT_PER_SUBGROUP
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineNggVsTsGsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        ShaderHash hash = pContext->GetShaderHashCode(ShaderStageFragment);
        SetShaderHash(ShaderStageFragment, hash);

        if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
        {
            uint32_t checksum = MetroHash::Compact32(hash);
            SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_CHKSUM_PS, CHECKSUM, checksum);
        }
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& gsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

    if (tcsBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    SET_REG(pConfig, IA_MULTI_VGT_PARAM_PIPED, iaMultiVgtParam.u32All);

    // Set up VGT_TF_PARAM
    SetupVgtTfParam(pContext, &pConfig->m_lsHsRegs);

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}
#endif

// =====================================================================================================================
// Builds register configuration for compute pipeline.
Result ConfigBuilder::BuildPipelineCsRegConfig(
    Context*            pContext,        // [in] LLPC context
    uint8_t**           ppConfig,        // [out] Register configuration for compute pipeline
    size_t*             pConfigSize)     // [out] Size of register configuration
{
    Result result = Result::Success;
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    LLPC_ASSERT(pContext->GetShaderStageMask() == ShaderStageToMask(ShaderStageCompute));

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 36
    ShaderHash hash = {};
#else
    ShaderHash hash = 0;
#endif

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineCsRegConfig)];
    PipelineCsRegConfig* pConfig = reinterpret_cast<PipelineCsRegConfig*>(pAllocBuf);
    pConfig->Init(gfxIp);

    AddApiHwShaderMapping(ShaderStageCompute, Util::Abi::HwShaderCs);

    SetPipelineType(Util::Abi::PipelineType::Cs);

    result = BuildCsRegConfig(pContext, ShaderStageCompute, pConfig);

    hash = pContext->GetShaderHashCode(ShaderStageCompute);
    SetShaderHash(ShaderStageCompute, hash);

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportShaderPowerProfiling)
    {
        uint32_t checksum = MetroHash::Compact32(hash);
        SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_SHADER_CHKSUM, CHECKSUM, checksum);
    }
#endif

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware vertex shader.
template <typename T>
Result ConfigBuilder::BuildVsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for vertex-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT((shaderStage == ShaderStageVertex)   ||
                (shaderStage == ShaderStageTessEval) ||
                (shaderStage == ShaderStageCopyShader));

    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const auto pIntfData = pContext->GetShaderInterfaceData(shaderStage);

    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage;

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP, true);  // Follow PAL setting

    const auto& xfbStrides = pResUsage->inOutUsage.xfbStrides;
    bool enableXfb = pResUsage->inOutUsage.enableXfb;
    if (shaderStage == ShaderStageCopyShader)
    {
        // NOTE: For copy shader, we use fixed number of user data registers.
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, Llpc::CopyShaderUserSgprCount);
        SetNumAvailSgprs(Util::Abi::HardwareStage::Vs, pContext->GetGpuProperty()->maxSgprsAvailable);
        SetNumAvailVgprs(Util::Abi::HardwareStage::Vs, pContext->GetGpuProperty()->maxVgprsAvailable);

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
        const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, DEBUG_MODE, pShaderInfo->options.debugMode);

        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, TRAP_PRESENT, pShaderInfo->options.trapPresent);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, pIntfData->userDataCount);
        const bool userSgprMsb = (pIntfData->userDataCount > 31);
#if LLPC_BUILD_GFX10
        if (gfxIp.major == 10)
        {
            SET_REG_GFX10_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB, userSgprMsb);
        }
        else
#endif
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

#if LLPC_BUILD_GFX10
    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC1_VS, MEM_ORDERED, true);
    }
#endif

    auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());

    uint8_t usrClipPlaneMask = pPipelineInfo->rsState.usrClipPlaneMask;
    bool depthClipDisable = (pPipelineInfo->vpState.depthClipEnable == false);
    bool rasterizerDiscardEnable = pPipelineInfo->rsState.rasterizerDiscardEnable;
    bool disableVertexReuse = pPipelineInfo->iaState.disableVertexReuse;

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

        if (pContext->IsTessOffChip())
        {
            SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN, true);
        }
    }
    else
    {
        LLPC_ASSERT(shaderStage == ShaderStageCopyShader);

        usePointSize      = builtInUsage.gs.pointSize;
        usePrimitiveId    = builtInUsage.gs.primitiveIdIn;
        useLayer          = builtInUsage.gs.layer;
        useViewportIndex  = builtInUsage.gs.viewportIndex;
        clipDistanceCount = builtInUsage.gs.clipDistance;
        cullDistanceCount = builtInUsage.gs.cullDistance;

        // NOTE: For ES-GS merged shader, the actual use of primitive ID should take both ES and GS into consideration.
        const bool hasTs = ((pContext->GetShaderStageMask() & (ShaderStageToMask(ShaderStageTessControl) |
                                                               ShaderStageToMask(ShaderStageTessEval))) != 0);
        if (hasTs)
        {
            const auto& tesBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
            usePrimitiveId = usePrimitiveId || tesBuiltInUsage.primitiveId;
        }
        else
        {
            const auto& vsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
            usePrimitiveId = usePrimitiveId || vsBuiltInUsage.primitiveId;
        }

        const auto pGsIntfData = pContext->GetShaderInterfaceData(ShaderStageGeometry);
        if (pContext->IsGsOnChip() && cl::InRegEsGsLdsSize)
        {
            LLPC_ASSERT(pGsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize != 0);

            SET_DYN_REG(pConfig,
                        mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }

        if (enableXfb)
        {
            LLPC_ASSERT(pGsIntfData->userDataUsage.gs.copyShaderStreamOutTable != 0);
            SET_DYN_REG(pConfig,
                        mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderStreamOutTable,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, usePrimitiveId);

#if LLPC_BUILD_GFX10
    if ((gfxIp.major >= 10) && (pResUsage->inOutUsage.expCount == 0))
    {
        SET_REG_GFX10_FIELD(&pConfig->m_vsRegs, SPI_VS_OUT_CONFIG, NO_PC_EXPORT, true);
    }
    else
#endif
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

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuWorkarounds()->gfx10.waTessIncorrectRelativeIndex)
    {
        disableVertexReuse = true;
    }
#endif
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

    useLayer = useLayer || pPipelineInfo->iaState.enableMultiView;

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
#if LLPC_BUILD_GFX10
        else if (gfxIp.major == 10)
        {
        }
#endif
        else
        {
            LLPC_NOT_IMPLEMENTED();
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

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_USER_ACCUM_VS_3, CONTRIBUTION, 1);
    }
#endif

    // Set shader user data maping
    if (result == Result::Success)
    {
        result = ConfigBuilder::BuildUserDataConfig<T>(pContext,
                                                       shaderStage,
                                                       ShaderStageInvalid,
                                                       mmSPI_SHADER_USER_DATA_VS_0,
                                                       pConfig);
    }

    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware local-hull merged shader.
template <typename T>
Result ConfigBuilder::BuildLsHsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage1,   // Current first shader stage (from API side)
    ShaderStage         shaderStage2,   // Current second shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for local-hull-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageInvalid));
    LLPC_ASSERT((shaderStage2 == ShaderStageTessControl) || (shaderStage2 == ShaderStageInvalid));

    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const auto pTcsResUsage = pContext->GetShaderResourceUsage(ShaderStageTessControl);
    const auto& vsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
    const auto& tcsBuiltInUsage = pTcsResUsage->builtInUsage.tcs;

    uint32_t floatMode =
        SetupFloatingPointMode(pContext, (shaderStage2 != ShaderStageInvalid) ? shaderStage2 : shaderStage1);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DX10_CLAMP, true); // Follow PAL setting

    uint32_t lsVgtCompCnt = 1;
    if (vsBuiltInUsage.instanceIndex)
    {
        lsVgtCompCnt += 2; // Enable instance ID
    }
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, LS_VGPR_COMP_CNT, lsVgtCompCnt);

    const auto& pVsIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
    const auto& pTcsIntfData = pContext->GetShaderInterfaceData(ShaderStageTessControl);
    uint32_t userDataCount = std::max(pVsIntfData->userDataCount, pTcsIntfData->userDataCount);

    const auto pTcsShaderInfo = pContext->GetPipelineShaderInfo(ShaderStageTessControl);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, DEBUG_MODE, pTcsShaderInfo->options.debugMode);

    const bool userSgprMsb = (userDataCount > 31);
#if LLPC_BUILD_GFX10
    if (gfxIp.major == 10)
    {
        bool wgpMode = (pContext->GetShaderWgpMode(ShaderStageVertex) ||
                        pContext->GetShaderWgpMode(ShaderStageTessControl));

        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, MEM_ORDERED, true);
        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC1_HS, WGP_MODE, wgpMode);
        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
    }
    else
#endif
    {
        SET_REG_GFX9_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR_MSB, userSgprMsb);
    }
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, TRAP_PRESENT, pTcsShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR, userDataCount);

    // NOTE: On GFX7+, granularity for the LDS_SIZE field is 128. The range is 0~128 which allocates 0 to 16K
    // DWORDs.
    const auto& calcFactor = pTcsResUsage->inOutUsage.tcs.calcFactor;
    uint32_t ldsSizeInDwords = calcFactor.onChip.patchConstStart +
                               calcFactor.patchConstSize * calcFactor.patchCountPerThreadGroup;
    if (pContext->IsTessOffChip())
    {
        ldsSizeInDwords = calcFactor.inPatchSize * calcFactor.patchCountPerThreadGroup;
    }

    const uint32_t ldsSizeDwordGranularity = 128u;
    const uint32_t ldsSizeDwordGranularityShift = 7u;
    uint32_t ldsSize = Pow2Align(ldsSizeInDwords, ldsSizeDwordGranularity) >> ldsSizeDwordGranularityShift;

    if (gfxIp.major == 9)
    {
        SET_REG_GFX9_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
    }
#if LLPC_BUILD_GFX10
    else if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_PGM_RSRC2_HS, LDS_SIZE, ldsSize);
    }
#endif
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }

    SetLdsSizeByteSize(Util::Abi::HardwareStage::Hs, ldsSizeInDwords * 4);

    // Minimum and maximum tessellation factors supported by the hardware.
    constexpr float MinTessFactor = 1.0f;
    constexpr float MaxTessFactor = 64.0f;
    SET_REG(&pConfig->m_lsHsRegs, VGT_HOS_MIN_TESS_LEVEL, FloatToBits(MinTessFactor));
    SET_REG(&pConfig->m_lsHsRegs, VGT_HOS_MAX_TESS_LEVEL, FloatToBits(MaxTessFactor));

    // Set VGT_LS_HS_CONFIG
    SET_REG_FIELD(&pConfig->m_lsHsRegs, VGT_LS_HS_CONFIG, NUM_PATCHES, calcFactor.patchCountPerThreadGroup);
    auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
    SET_REG_FIELD(&pConfig->m_lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_INPUT_CP, pPipelineInfo->iaState.patchControlPoints);

    auto hsNumOutputCp = tcsBuiltInUsage.outputVertices;
    SET_REG_FIELD(&pConfig->m_lsHsRegs, VGT_LS_HS_CONFIG, HS_NUM_OUTPUT_CP, hsNumOutputCp);

    SetNumAvailSgprs(Util::Abi::HardwareStage::Hs, pTcsResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Hs, pTcsResUsage->numVgprsAvailable);

    // Set up VGT_TF_PARAM
    SetupVgtTfParam(pContext, &pConfig->m_lsHsRegs);

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_lsHsRegs, SPI_SHADER_USER_ACCUM_LSHS_3, CONTRIBUTION, 1);
    }
#endif

    if (gfxIp.major == 9)
    {
        result = ConfigBuilder::BuildUserDataConfig<T>(
                     pContext,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx09::mmSPI_SHADER_USER_DATA_LS_0,
                     pConfig);
    }
#if LLPC_BUILD_GFX10
    else if (gfxIp.major == 10)
    {
        result = ConfigBuilder::BuildUserDataConfig<T>(
                     pContext,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx10::mmSPI_SHADER_USER_DATA_HS_0,
                     pConfig);
    }
#endif
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }

    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware export-geometry merged shader.
template <typename T>
Result ConfigBuilder::BuildEsGsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage1,   // Current first shader stage (from API side)
    ShaderStage         shaderStage2,   // Current second shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for export-geometry-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageTessEval) ||
                (shaderStage1 == ShaderStageInvalid));
    LLPC_ASSERT((shaderStage2 == ShaderStageGeometry) || (shaderStage2 == ShaderStageInvalid));

    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    const uint32_t stageMask = pContext->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);

    const auto pVsResUsage = pContext->GetShaderResourceUsage(ShaderStageVertex);
    const auto& vsBuiltInUsage = pVsResUsage->builtInUsage.vs;

    const auto pTesResUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval);
    const auto& tesBuiltInUsage = pTesResUsage->builtInUsage.tes;

    const auto pGsResUsage = pContext->GetShaderResourceUsage(ShaderStageGeometry);
    const auto& gsBuiltInUsage = pGsResUsage->builtInUsage.gs;
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
        SetupFloatingPointMode(pContext, (shaderStage2 != ShaderStageInvalid) ? shaderStage2 : shaderStage1);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

    const auto pVsIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
    const auto pTesIntfData = pContext->GetShaderInterfaceData(ShaderStageTessEval);
    const auto pGsIntfData = pContext->GetShaderInterfaceData(ShaderStageGeometry);
    uint32_t userDataCount = std::max((hasTs ? pTesIntfData->userDataCount : pVsIntfData->userDataCount),
                                      pGsIntfData->userDataCount);

    const auto pGsShaderInfo = pContext->GetPipelineShaderInfo(ShaderStageGeometry);
    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, pGsShaderInfo->options.debugMode);

    const bool userSgprMsb = (userDataCount > 31);
#if LLPC_BUILD_GFX10
    if (gfxIp.major == 10)
    {
        bool wgpMode = (pContext->GetShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex) ||
                        pContext->GetShaderWgpMode(ShaderStageGeometry));

        SET_REG_GFX10_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
        SET_REG_GFX10_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);
        SET_REG_GFX10_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }
    else
#endif
    {
        SET_REG_GFX9_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }

    SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, pGsShaderInfo->options.trapPresent);
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

        if (pContext->IsTessOffChip())
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

    const auto ldsSizeDwordGranularityShift = pContext->GetGpuProperty()->ldsSizeDwordGranularityShift;

    SET_REG_FIELD(&pConfig->m_esGsRegs,
                  SPI_SHADER_PGM_RSRC2_GS,
                  LDS_SIZE,
                  calcFactor.gsOnChipLdsSize >> ldsSizeDwordGranularityShift);
    SetLdsSizeByteSize(Util::Abi::HardwareStage::Gs, calcFactor.gsOnChipLdsSize * 4);
    SetEsGsLdsSize(calcFactor.esGsLdsSize * 4);

    uint32_t maxVertOut = std::max(1u, static_cast<uint32_t>(gsBuiltInUsage.outputVertices));
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

    // TODO: Currently only support offchip GS
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);

    if (pContext->IsGsOnChip())
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

    if (gsBuiltInUsage.outputVertices <= 128)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128);
    }
    else if (gsBuiltInUsage.outputVertices <= 256)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256);
    }
    else if (gsBuiltInUsage.outputVertices <= 512)
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
        (gsBuiltInUsage.invocations > 1) ? (calcFactor.gsPrimsPerSubgroup * gsBuiltInUsage.invocations) : 0;
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

    if ((gsBuiltInUsage.invocations > 1) || gsBuiltInUsage.invocationId)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
        SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_INSTANCE_CNT, CNT, gsBuiltInUsage.invocations);
    }
    SET_REG_FIELD(&pConfig->m_esGsRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

    VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
    if (gsInOutUsage.outputMapLocCount == 0)
    {
        gsOutputPrimitiveType = POINTLIST;
    }
    else if (gsBuiltInUsage.outputPrimitive == OutputPoints)
    {
        gsOutputPrimitiveType = POINTLIST;
    }
    else if (gsBuiltInUsage.outputPrimitive == LINESTRIP)
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
#if LLPC_BUILD_GFX10
    else if (gfxIp.major == 10)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs,
                      GE_MAX_OUTPUT_PER_SUBGROUP,
                      MAX_VERTS_PER_SUBGROUP,
                      maxPrimsPerSubgroup);
    }
#endif
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }

    SetNumAvailSgprs(Util::Abi::HardwareStage::Gs, pGsResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Gs, pGsResUsage->numVgprsAvailable);

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_esGsRegs, SPI_SHADER_USER_ACCUM_ESGS_3, CONTRIBUTION, 1);
    }
#endif

    if (gfxIp.major == 9)
    {
        result = ConfigBuilder::BuildUserDataConfig<T>(
                     pContext,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx09::mmSPI_SHADER_USER_DATA_ES_0,
                     pConfig);
    }
#if LLPC_BUILD_GFX10
    else if (gfxIp.major == 10)
    {
        result = ConfigBuilder::BuildUserDataConfig<T>(
                     pContext,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                     (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                     Gfx10::mmSPI_SHADER_USER_DATA_GS_0,
                     pConfig);
    }
#endif
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }

    return result;
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Builds register configuration for hardware primitive shader.
template <typename T>
Result ConfigBuilder::BuildPrimShaderRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage1,   // Current first shader stage (from API side)
    ShaderStage         shaderStage2,   // Current second shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for primitive-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageTessEval) ||
                (shaderStage1 == ShaderStageInvalid));
    LLPC_ASSERT((shaderStage2 == ShaderStageGeometry) || (shaderStage2 == ShaderStageInvalid));

    const auto gfxIp = pContext->GetGfxIpVersion();
    LLPC_ASSERT(gfxIp.major >= 10);

    const auto pNggControl = pContext->GetNggControl();
    LLPC_ASSERT(pNggControl->enableNgg);

    const uint32_t stageMask = pContext->GetShaderStageMask();
    const bool hasTs = ((stageMask & (ShaderStageToMask(ShaderStageTessControl) |
                                      ShaderStageToMask(ShaderStageTessEval))) != 0);
    const bool hasGs = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);

    const auto pVsResUsage = pContext->GetShaderResourceUsage(ShaderStageVertex);
    const auto& vsBuiltInUsage = pVsResUsage->builtInUsage.vs;

    const auto pTesResUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval);
    const auto& tesBuiltInUsage = pTesResUsage->builtInUsage.tes;

    const auto pGsResUsage = pContext->GetShaderResourceUsage(ShaderStageGeometry);
    const auto& gsBuiltInUsage = pGsResUsage->builtInUsage.gs;
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
        SetupFloatingPointMode(pContext, (shaderStage2 != ShaderStageInvalid) ? shaderStage2 : shaderStage1);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true); // Follow PAL setting

    const auto pVsIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
    const auto pTesIntfData = pContext->GetShaderInterfaceData(ShaderStageTessEval);
    const auto pGsIntfData = pContext->GetShaderInterfaceData(ShaderStageGeometry);
    uint32_t userDataCount = std::max((hasTs ? pTesIntfData->userDataCount : pVsIntfData->userDataCount),
                                      pGsIntfData->userDataCount);

    const auto pGsShaderInfo = pContext->GetPipelineShaderInfo(ShaderStageGeometry);
    bool wgpMode = pContext->GetShaderWgpMode(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    if (hasGs)
    {
        wgpMode = (wgpMode || pContext->GetShaderWgpMode(ShaderStageGeometry));
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, pGsShaderInfo->options.debugMode);
    SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, MEM_ORDERED, true);
    SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE, wgpMode);

    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, pGsShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, userDataCount);

    const bool userSgprMsb = (userDataCount > 31);
#if LLPC_BUILD_GFX10
    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB, userSgprMsb);
    }
    else
#endif
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

        if (pContext->IsTessOffChip())
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

    const auto ldsSizeDwordGranularityShift = pContext->GetGpuProperty()->ldsSizeDwordGranularityShift;

    SET_REG_FIELD(&pConfig->m_primShaderRegs,
                  SPI_SHADER_PGM_RSRC2_GS,
                  LDS_SIZE,
                  calcFactor.gsOnChipLdsSize >> ldsSizeDwordGranularityShift);
    SetLdsSizeByteSize(Util::Abi::HardwareStage::Gs, calcFactor.gsOnChipLdsSize * 4);
    SetEsGsLdsSize(calcFactor.esGsLdsSize * 4);

    uint32_t maxVertOut = std::max(1u, static_cast<uint32_t>(gsBuiltInUsage.outputVertices));
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

    // TODO: Currently only support offchip GS
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, ONCHIP, VGT_GS_MODE_ONCHIP_OFF);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);
    if (gsBuiltInUsage.outputVertices <= 128)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128);
    }
    else if (gsBuiltInUsage.outputVertices <= 256)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256);
    }
    else if (gsBuiltInUsage.outputVertices <= 512)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_512);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_1024);
    }

    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_ONCHIP_CNTL, ES_VERTS_PER_SUBGRP, calcFactor.esVertsPerSubgroup);
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_PRIMS_PER_SUBGRP, calcFactor.gsPrimsPerSubgroup);

    const uint32_t gsInstPrimsInSubgrp =
        (gsBuiltInUsage.invocations > 1) ?
            (calcFactor.gsPrimsPerSubgroup * gsBuiltInUsage.invocations) : calcFactor.gsPrimsPerSubgroup;
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_ONCHIP_CNTL, GS_INST_PRIMS_IN_SUBGRP, gsInstPrimsInSubgrp);

    uint32_t gsVertItemSize = 4 * gsInOutUsage.outputMapLocCount;
    SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize);

    if ((gsBuiltInUsage.invocations > 1) || gsBuiltInUsage.invocationId)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, VGT_GS_INSTANCE_CNT, CNT, gsBuiltInUsage.invocations);
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
        else if (gsBuiltInUsage.outputPrimitive == OutputPoints)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if (gsBuiltInUsage.outputPrimitive == OutputLineStrip)
        {
            gsOutputPrimitiveType = LINESTRIP;
        }
        else if (gsBuiltInUsage.outputPrimitive == OutputTriangleStrip)
        {
            gsOutputPrimitiveType = TRISTRIP;
        }
        else
        {
            LLPC_NEVER_CALLED();
        }
    }
    else if (hasTs)
    {
        // With tessellation
        if (tesBuiltInUsage.pointMode)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if (tesBuiltInUsage.primitiveMode == Isolines)
        {
            gsOutputPrimitiveType = LINESTRIP;
        }
        else if ((tesBuiltInUsage.primitiveMode == Triangles) ||
                 (tesBuiltInUsage.primitiveMode == Quads))
        {
            gsOutputPrimitiveType = TRISTRIP;
        }
        else
        {
            LLPC_NEVER_CALLED();
        }
    }
    else
    {
        // Without tessellation
        const auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
        const auto topology = pPipelineInfo->iaState.topology;
        if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        {
            gsOutputPrimitiveType = POINTLIST;
        }
        else if ((topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY))
        {
            gsOutputPrimitiveType = LINESTRIP;
        }
        else if ((topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY) ||
                 (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY))
        {
            gsOutputPrimitiveType = TRISTRIP;
        }
        else
        {
            LLPC_NEVER_CALLED();
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

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_primShaderRegs, SPI_SHADER_USER_ACCUM_ESGS_3, CONTRIBUTION, 1);
    }
#endif

    //
    // Build VS specific configuration
    //
    auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());

    uint8_t usrClipPlaneMask = pPipelineInfo->rsState.usrClipPlaneMask;
    bool depthClipDisable = (pPipelineInfo->vpState.depthClipEnable == false);
    bool rasterizerDiscardEnable = pPipelineInfo->rsState.rasterizerDiscardEnable;
    bool disableVertexReuse = pPipelineInfo->iaState.disableVertexReuse;

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

    useLayer = useLayer || pPipelineInfo->iaState.enableMultiView;

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
    SET_REG_FIELD(&pConfig->m_primShaderRegs,
                  GE_NGG_SUBGRP_CNTL,
                  PRIM_AMP_FACTOR,
                  std::max(calcFactor.primAmpFactor, 1u));
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
    result = ConfigBuilder::BuildUserDataConfig<T>(
                 pContext,
                 (shaderStage1 != ShaderStageInvalid) ? shaderStage1 : shaderStage2,
                 (shaderStage1 != ShaderStageInvalid) ? shaderStage2 : ShaderStageInvalid,
                 Gfx10::mmSPI_SHADER_USER_DATA_GS_0,
                 pConfig);

    return result;
}
#endif

// =====================================================================================================================
// Builds register configuration for hardware pixel shader.
template <typename T>
Result ConfigBuilder::BuildPsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for pixel-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT(shaderStage == ShaderStageFragment);

    const GraphicsPipelineBuildInfo* pPipelineInfo =
        static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());

    const auto pIntfData = pContext->GetShaderInterfaceData(shaderStage);
    const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage.fs;

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP, true);  // Follow PAL setting
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE, pShaderInfo->options.debugMode);

    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, TRAP_PRESENT, pShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR, pIntfData->userDataCount);

    const bool userSgprMsb = (pIntfData->userDataCount > 31);
#if LLPC_BUILD_GFX10
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC1_PS, MEM_ORDERED, true);

        if (pShaderInfo->options.waveBreakSize == Llpc::WaveBreakSize::DrawTime)
        {
            SetCalcWaveBreakSizeAtDrawTime(true);
        }
        else
        {
            SET_REG_GFX10_FIELD(&pConfig->m_psRegs, PA_SC_SHADER_CONTROL, WAVE_BREAK_REGION_SIZE,
                                static_cast<uint32_t>(pShaderInfo->options.waveBreakSize));
        }

        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, PA_STEREO_CNTL, STEREO_MODE, STATE_STEREO_X);
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
    }
    else
#endif
    {
        SET_REG_GFX9_FIELD(&pConfig->m_psRegs, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB, userSgprMsb);
    }

    SET_REG_FIELD(&pConfig->m_psRegs, SPI_BARYC_CNTL, FRONT_FACE_ALL_BITS, true);
    if (builtInUsage.pixelCenterInteger)
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
    if (builtInUsage.earlyFragmentTests)
    {
        zOrder = EARLY_Z_THEN_LATE_Z;
    }
    else if (pResUsage->resourceWrite)
    {
        zOrder = LATE_Z;
        execOnHeirFail = true;
    }
    else if (pShaderInfo->options.allowReZ)
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
                  (builtInUsage.sampleMask || (pPipelineInfo->cbState.alphaToCoverageEnable == false)));
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, DEPTH_BEFORE_SHADER, builtInUsage.earlyFragmentTests);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, EXEC_ON_NOOP,
                  (builtInUsage.earlyFragmentTests && pResUsage->resourceWrite));
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, EXEC_ON_HIER_FAIL, execOnHeirFail);
#if LLPC_BUILD_GFX10
    if (gfxIp.major == 10)
    {
        SET_REG_GFX10_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, PRE_SHADER_DEPTH_COVERAGE_ENABLE,
                            builtInUsage.postDepthCoverage);
    }
#endif

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
    uint32_t cbShaderMask = (pShaderInfo->pModuleData == nullptr) ? 0 : pResUsage->inOutUsage.fs.cbShaderMask;
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
#if LLPC_BUILD_GFX10
    auto waveFrontSize = pContext->GetShaderWaveSize(ShaderStageFragment);
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
#endif

    uint32_t pointCoordLoc = InvalidValue;
    if (pResUsage->inOutUsage.builtInInputLocMap.find(spv::BuiltInPointCoord) !=
        pResUsage->inOutUsage.builtInInputLocMap.end())
    {
        // Get generic input corresponding to gl_PointCoord (to set the field PT_SPRITE_TEX)
        pointCoordLoc = pResUsage->inOutUsage.builtInInputLocMap[spv::BuiltInPointCoord];
    }

    // NOTE: PAL expects at least one mmSPI_PS_INPUT_CNTL_0 register set, so we always patch it at least one if none
    // were identified in the shader.
    const std::vector<FsInterpInfo> dummyInterpInfo {{ 0, false, false, false }};
    const auto& fsInterpInfo = pResUsage->inOutUsage.fs.interpInfo;
    const auto* pInterpInfo = (fsInterpInfo.size() == 0) ? &dummyInterpInfo : &fsInterpInfo;

    for (uint32_t i = 0; i < pInterpInfo->size(); ++i)
    {
        const auto& interpInfoElem = (*pInterpInfo)[i];
        LLPC_ASSERT(((interpInfoElem.loc     == InvalidFsInterpInfo.loc) &&
                     (interpInfoElem.flat    == InvalidFsInterpInfo.flat) &&
                     (interpInfoElem.custom  == InvalidFsInterpInfo.custom) &&
                     (interpInfoElem.is16bit == InvalidFsInterpInfo.is16bit)) == false);

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

        SET_DYN_REG(pConfig, mmSPI_PS_INPUT_CNTL_0 + i, spiPsInputCntl.u32All);
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

    if (pPipelineInfo->rsState.innerCoverage)
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

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_psRegs, SPI_SHADER_USER_ACCUM_PS_3, CONTRIBUTION, 1);
    }
#endif

    // Set shader user data mapping
    if (result == Result::Success)
    {
        result = ConfigBuilder::BuildUserDataConfig<T>(pContext,
                                                       shaderStage,
                                                       ShaderStageInvalid,
                                                       mmSPI_SHADER_USER_DATA_PS_0,
                                                       pConfig);
    }

    return result;
}

// =====================================================================================================================
// Builds register configuration for compute shader.
Result ConfigBuilder::BuildCsRegConfig(
    Context*             pContext,      // [in] LLPC context
    ShaderStage          shaderStage,   // Current shader stage (from API side)
    PipelineCsRegConfig* pConfig)       // [out] Register configuration for compute pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT(shaderStage == ShaderStageCompute);

    const auto pIntfData = pContext->GetShaderInterfaceData(shaderStage);
    const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage.cs;
    uint32_t workgroupSizes[3];

    switch (static_cast<WorkgroupLayout>(builtInUsage.workgroupLayout))
    {
    case WorkgroupLayout::Unknown:
    case WorkgroupLayout::Linear:
        workgroupSizes[0] = builtInUsage.workgroupSizeX;
        workgroupSizes[1] = builtInUsage.workgroupSizeY;
        workgroupSizes[2] = builtInUsage.workgroupSizeZ;
        break;
    case WorkgroupLayout::Quads:
    case WorkgroupLayout::SexagintiQuads:
        workgroupSizes[0] = builtInUsage.workgroupSizeX * builtInUsage.workgroupSizeY;
        workgroupSizes[1] = builtInUsage.workgroupSizeZ;
        workgroupSizes[2] = 1;
        break;
    }
    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, DX10_CLAMP, true);  // Follow PAL setting
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, DEBUG_MODE, pShaderInfo->options.debugMode);

#if LLPC_BUILD_GFX10
    GfxIpVersion gfxIp = pContext->GetGfxIpVersion();

    if (gfxIp.major == 10)
    {
        bool wgpMode = pContext->GetShaderWgpMode(ShaderStageCompute);

        SET_REG_GFX10_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, MEM_ORDERED, true);
        SET_REG_GFX10_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, WGP_MODE, wgpMode);
        uint32_t waveSize = pContext->GetShaderWaveSize(ShaderStageCompute);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 495
        if (waveSize == 32)
        {
            // For GFX10 pipeline, PAL expects to get CS_W32_EN from pipeline metadata,
            // other fields of this register are set by PAL.
            SET_REG_GFX10_FIELD(&pConfig->m_csRegs, COMPUTE_DISPATCH_INITIATOR, CS_W32_EN, true);
        }
#else
        LLPC_ASSERT((waveSize == 32) || (waveSize == 64));
        SetWaveFrontSize(Util::Abi::HardwareStage::Cs, waveSize);
#endif
    }
#endif

    // Set registers based on shader interface data
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TRAP_PRESENT, pShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, USER_SGPR, pIntfData->userDataCount);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TGID_X_EN, true);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TGID_Y_EN, true);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TGID_Z_EN, true);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TG_SIZE_EN, true);

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
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TIDIG_COMP_CNT, tidigCompCnt);

    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_NUM_THREAD_X, NUM_THREAD_FULL, workgroupSizes[0]);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_NUM_THREAD_Y, NUM_THREAD_FULL, workgroupSizes[1]);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_NUM_THREAD_Z, NUM_THREAD_FULL, workgroupSizes[2]);

    SetNumAvailSgprs(Util::Abi::HardwareStage::Cs, pResUsage->numSgprsAvailable);
    SetNumAvailVgprs(Util::Abi::HardwareStage::Cs, pResUsage->numVgprsAvailable);

#if LLPC_BUILD_GFX10
    if (pContext->GetGpuProperty()->supportSpiPrefPriority)
    {
        SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_USER_ACCUM_0, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_USER_ACCUM_1, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_USER_ACCUM_2, CONTRIBUTION, 1);
        SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_USER_ACCUM_3, CONTRIBUTION, 1);
    }
#endif

    // Set shader user data mapping
    if (result == Result::Success)
    {
        result = ConfigBuilder::BuildUserDataConfig<PipelineCsRegConfig>(pContext,
                                                                         shaderStage,
                                                                         ShaderStageInvalid,
                                                                         mmCOMPUTE_USER_DATA_0,
                                                                         pConfig);
    }

    return result;
}

// =====================================================================================================================
// Builds user data configuration for the specified shader stage.
template <typename T>
Result ConfigBuilder::BuildUserDataConfig(
    Context*    pContext,       // [in] LLPC context
    ShaderStage shaderStage1,   // Current first shader stage (from API side)
    ShaderStage shaderStage2,   // Current second shader stage (from API side)
    uint32_t    startUserData,  // Starting user data
    T*          pConfig)        // [out] Register configuration for the associated pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT(shaderStage1 != ShaderStageInvalid); // The first shader stage must be a valid one

    // NOTE: For merged shader, the second shader stage should be tessellation control shader (LS-HS) or geometry
    // shader (ES-GS).
    LLPC_ASSERT((shaderStage2 == ShaderStageTessControl) || (shaderStage2 == ShaderStageGeometry) ||
                (shaderStage2 == ShaderStageInvalid));

    bool enableMultiView = false;
    if (pContext->IsGraphics())
    {
        enableMultiView = static_cast<const GraphicsPipelineBuildInfo*>(
            pContext->GetPipelineBuildInfo())->iaState.enableMultiView;
    }

    bool enableXfb = false;
    if (pContext->IsGraphics())
    {
        if (((shaderStage1 == ShaderStageVertex) || (shaderStage1 == ShaderStageTessEval)) &&
            (shaderStage2 == ShaderStageInvalid))
        {
            enableXfb = pContext->GetShaderResourceUsage(shaderStage1)->inOutUsage.enableXfb;
        }
    }

#if LLPC_BUILD_GFX10
    const bool enableNgg = pContext->IsGraphics() ? pContext->GetNggControl()->enableNgg : false;
    LLPC_UNUSED(enableNgg);
#endif

    const auto pIntfData1 = pContext->GetShaderInterfaceData(shaderStage1);
    const auto& entryArgIdxs1 = pIntfData1->entryArgIdxs;
    LLPC_UNUSED(entryArgIdxs1);

    const auto pResUsage1 = pContext->GetShaderResourceUsage(shaderStage1);
    const auto& builtInUsage1 = pResUsage1->builtInUsage;

    const auto pIntfData2 = (shaderStage2 != ShaderStageInvalid) ?
                                pContext->GetShaderInterfaceData(shaderStage2) : nullptr;

    // Stage-specific processing
    if (shaderStage1 == ShaderStageVertex)
    {
        // TODO: PAL only check BaseVertex now, we need update code once PAL check them separately.
        if (builtInUsage1.vs.baseVertex || builtInUsage1.vs.baseInstance)
        {
            LLPC_ASSERT(entryArgIdxs1.vs.baseVertex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.vs.baseVertex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::BaseVertex));

            LLPC_ASSERT(entryArgIdxs1.vs.baseInstance > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.vs.baseInstance,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::BaseInstance));
        }

        if (builtInUsage1.vs.drawIndex)
        {
            LLPC_ASSERT(entryArgIdxs1.vs.drawIndex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.vs.drawIndex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::DrawIndex));
        }

        if (pIntfData1->userDataUsage.vs.vbTablePtr > 0)
        {
            LLPC_ASSERT(pIntfData1->userDataMap[pIntfData1->userDataUsage.vs.vbTablePtr] ==
                InterfaceData::UserDataUnmapped);

            SET_DYN_REG(pConfig,
                startUserData + pIntfData1->userDataUsage.vs.vbTablePtr,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::VertexBufferTable));
        }

        if (enableXfb && (pIntfData1->userDataUsage.vs.streamOutTablePtr > 0) && (shaderStage2 == ShaderStageInvalid))
        {
            LLPC_ASSERT(pIntfData1->userDataMap[pIntfData1->userDataUsage.vs.streamOutTablePtr] ==
                InterfaceData::UserDataUnmapped);

            SET_DYN_REG(pConfig,
                startUserData + pIntfData1->userDataUsage.vs.streamOutTablePtr,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }

        if (enableMultiView)
        {
            if ((shaderStage2 == ShaderStageInvalid) || (shaderStage2 == ShaderStageTessControl))
            {
                // Act as hardware VS or LS-HS merged shader
                LLPC_ASSERT(entryArgIdxs1.vs.viewIndex > 0);
                SET_DYN_REG(pConfig,
                            startUserData + pIntfData1->userDataUsage.vs.viewIndex,
                            static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
            else if (shaderStage2 == ShaderStageGeometry)
            {
                // Act as hardware ES-GS merged shader
                const auto& entryArgIdxs2 = pIntfData2->entryArgIdxs;

                LLPC_ASSERT((entryArgIdxs1.vs.viewIndex > 0) && (entryArgIdxs2.gs.viewIndex > 0));
                LLPC_UNUSED(entryArgIdxs2);
                LLPC_ASSERT(pIntfData1->userDataUsage.vs.viewIndex == pIntfData2->userDataUsage.gs.viewIndex);
                SET_DYN_REG(pConfig,
                            startUserData + pIntfData1->userDataUsage.vs.viewIndex,
                            static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
            else
            {
                LLPC_NEVER_CALLED();
            }
        }

        if (shaderStage2 == ShaderStageGeometry)
        {
            if (pIntfData2->userDataUsage.gs.esGsLdsSize > 0)
            {
                SET_DYN_REG(pConfig,
                            startUserData + pIntfData2->userDataUsage.gs.esGsLdsSize,
                            static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
            }
        }
#if LLPC_BUILD_GFX10
        else if (shaderStage2 == ShaderStageInvalid)
        {
            if (pIntfData1->userDataUsage.vs.esGsLdsSize > 0)
            {
                LLPC_ASSERT(enableNgg);
                SET_DYN_REG(pConfig,
                            startUserData + pIntfData1->userDataUsage.vs.esGsLdsSize,
                            static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
            }
        }
#endif
    }
    else if (shaderStage1 == ShaderStageTessEval)
    {
        if (enableXfb && (pIntfData1->userDataUsage.tes.streamOutTablePtr > 0) && (shaderStage2 == ShaderStageInvalid))
        {
            LLPC_ASSERT(pIntfData1->userDataMap[pIntfData1->userDataUsage.tes.streamOutTablePtr] ==
                InterfaceData::UserDataUnmapped);

            SET_DYN_REG(pConfig,
                startUserData + pIntfData1->userDataUsage.tes.streamOutTablePtr,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }

        if (enableMultiView)
        {
            if (shaderStage2 == ShaderStageInvalid)
            {
                // Act as hardware VS
                LLPC_ASSERT(entryArgIdxs1.tes.viewIndex > 0);
                SET_DYN_REG(pConfig,
                            startUserData + pIntfData1->userDataUsage.tes.viewIndex,
                            static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
            else if (shaderStage2 == ShaderStageGeometry)
            {
                // Act as hardware ES-GS merged shader
                const auto& entryArgIdxs2 = pIntfData2->entryArgIdxs;

                LLPC_ASSERT((entryArgIdxs1.tes.viewIndex > 0) && (entryArgIdxs2.gs.viewIndex > 0));
                LLPC_UNUSED(entryArgIdxs2);
                LLPC_ASSERT(pIntfData1->userDataUsage.tes.viewIndex == pIntfData2->userDataUsage.gs.viewIndex);
                SET_DYN_REG(pConfig,
                            startUserData + pIntfData1->userDataUsage.tes.viewIndex,
                            static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
            }
        }

#if LLPC_BUILD_GFX10
        if (pIntfData1->userDataUsage.tes.esGsLdsSize > 0)
        {
            LLPC_ASSERT(enableNgg);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.tes.esGsLdsSize,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }
#endif
    }
    else if (shaderStage1 == ShaderStageGeometry)
    {
        LLPC_ASSERT(shaderStage2 == ShaderStageInvalid);

        if (enableMultiView)
        {
            LLPC_ASSERT(entryArgIdxs1.gs.viewIndex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.gs.viewIndex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
        }

#if LLPC_BUILD_GFX10
        if (pIntfData1->userDataUsage.gs.esGsLdsSize > 0)
        {
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.gs.esGsLdsSize,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }
#endif
    }
    else if (shaderStage1 == ShaderStageCompute)
    {
        LLPC_ASSERT(shaderStage2 == ShaderStageInvalid);

        if (builtInUsage1.cs.numWorkgroups > 0)
        {
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.cs.numWorkgroupsPtr,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::Workgroup));
        }
    }

    // NOTE: After user data nodes are merged together, any stage of merged shader are ought to have the same
    // configuration for general user data (apart from those special). In this sense, we are safe to use the first
    // shader stage to build user data register settings here.
    SET_DYN_REG(pConfig, startUserData, static_cast<uint32_t>(Util::Abi::UserDataMapping::GlobalTable));

    if (pResUsage1->perShaderTable)
    {
        SET_DYN_REG(pConfig, startUserData + 1, static_cast<uint32_t>(Util::Abi::UserDataMapping::PerShaderTable));
    }

    // NOTE: For copy shader, we use fixed number of user data SGPRs. Thus, there is no need of building user data
    // registers here.
    if (shaderStage1 != ShaderStageCopyShader)
    {
        uint32_t userDataLimit = 0;
        uint32_t spillThreshold = UINT32_MAX;
        uint32_t maxUserDataCount = pContext->GetGpuProperty()->maxUserDataCount;
        for (uint32_t i = 0; i < maxUserDataCount; ++i)
        {
            if (pIntfData1->userDataMap[i] != InterfaceData::UserDataUnmapped)
            {
                SET_DYN_REG(pConfig, startUserData + i, pIntfData1->userDataMap[i]);
                userDataLimit = std::max(userDataLimit, pIntfData1->userDataMap[i] + 1);
            }
        }

        if (pIntfData1->userDataUsage.spillTable > 0)
        {
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData1->userDataUsage.spillTable,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::SpillTable));
            userDataLimit = std::max(userDataLimit,
                                     pIntfData1->spillTable.offsetInDwords + pIntfData1->spillTable.sizeInDwords);
            spillThreshold = pIntfData1->spillTable.offsetInDwords;
        }

        m_userDataLimit = std::max(m_userDataLimit, userDataLimit);
        m_spillThreshold = std::min(m_spillThreshold, spillThreshold);
    }

    return result;
}

// =====================================================================================================================
// Sets up the register value for VGT_TF_PARAM.
void ConfigBuilder::SetupVgtTfParam(
    Context*        pContext,  // [in] LLPC context
    LsHsRegConfig*  pConfig)   // [out] Register configuration for local-hull-shader-specific pipeline
{
    uint32_t primType  = InvalidValue;
    uint32_t partition = InvalidValue;
    uint32_t topology  = InvalidValue;

    const auto& builtInUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;

    LLPC_ASSERT(builtInUsage.primitiveMode != SPIRVPrimitiveModeKind::Unknown);
    if (builtInUsage.primitiveMode == Isolines)
    {
        primType = TESS_ISOLINE;
    }
    else if (builtInUsage.primitiveMode == Triangles)
    {
        primType = TESS_TRIANGLE;
    }
    else if (builtInUsage.primitiveMode == Quads)
    {
        primType = TESS_QUAD;
    }
    LLPC_ASSERT(primType != InvalidValue);

    LLPC_ASSERT(builtInUsage.vertexSpacing != SpacingUnknown);
    if (builtInUsage.vertexSpacing == SpacingEqual)
    {
        partition = PART_INTEGER;
    }
    else if (builtInUsage.vertexSpacing == SpacingFractionalOdd)
    {
        partition = PART_FRAC_ODD;
    }
    else if (builtInUsage.vertexSpacing == SpacingFractionalEven)
    {
        partition = PART_FRAC_EVEN;
    }
    LLPC_ASSERT(partition != InvalidValue);

    LLPC_ASSERT(builtInUsage.vertexOrder != VertexOrderUnknown);
    if (builtInUsage.pointMode)
    {
        topology = OUTPUT_POINT;
    }
    else if (builtInUsage.primitiveMode == Isolines)
    {
        topology = OUTPUT_LINE;
    }
    else if (builtInUsage.vertexOrder == VertexOrderCw)
    {
        topology = OUTPUT_TRIANGLE_CW;
    }
    else if (builtInUsage.vertexOrder == VertexOrderCcw)
    {
        topology = OUTPUT_TRIANGLE_CCW;
    }

    auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
    if (pPipelineInfo->iaState.switchWinding)
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

    LLPC_ASSERT(topology != InvalidValue);

    SET_REG_FIELD(pConfig, VGT_TF_PARAM, TYPE, primType);
    SET_REG_FIELD(pConfig, VGT_TF_PARAM, PARTITIONING, partition);
    SET_REG_FIELD(pConfig, VGT_TF_PARAM, TOPOLOGY, topology);

    if (pContext->IsTessOffChip())
    {
        SET_REG_FIELD(pConfig, VGT_TF_PARAM, DISTRIBUTION_MODE, TRAPEZOIDS);
    }
}

// =====================================================================================================================
// Sets up floating point mode from the specified floating point control flags.
uint32_t ConfigBuilder::SetupFloatingPointMode(
    Context*    pContext,       // [in] LLPC context
    ShaderStage shaderStage)    // Shader stage
{
    FloatMode floatMode = {};
    floatMode.bits.fp16fp64DenormMode = FP_DENORM_FLUSH_NONE;

    auto fp16Control = pContext->GetShaderFloatControl(shaderStage, 16);
    auto fp32Control = pContext->GetShaderFloatControl(shaderStage, 32);
    auto fp64Control = pContext->GetShaderFloatControl(shaderStage, 64);

    if (fp16Control.roundingModeRTE || fp64Control.roundingModeRTE)
    {
        floatMode.bits.fp16fp64RoundMode = FP_ROUND_TO_NEAREST_EVEN;
    }
    else if (fp16Control.roundingModeRTZ || fp64Control.roundingModeRTZ)
    {
        floatMode.bits.fp16fp64RoundMode = FP_ROUND_TO_ZERO;
    }

    if (fp32Control.roundingModeRTE)
    {
        floatMode.bits.fp32RoundMode = FP_ROUND_TO_NEAREST_EVEN;
    }
    else if (fp32Control.roundingModeRTZ)
    {
        floatMode.bits.fp32RoundMode = FP_ROUND_TO_ZERO;
    }

    if (fp16Control.denormPerserve || fp64Control.denormPerserve)
    {
        floatMode.bits.fp16fp64DenormMode = FP_DENORM_FLUSH_NONE;
    }
    else if (fp16Control.denormFlushToZero || fp64Control.denormFlushToZero)
    {
        floatMode.bits.fp16fp64DenormMode = FP_DENORM_FLUSH_IN_OUT;
    }

    if (fp32Control.denormPerserve)
    {
        floatMode.bits.fp32DenormMode = FP_DENORM_FLUSH_NONE;
    }
    else if (fp32Control.denormFlushToZero)
    {
        floatMode.bits.fp32DenormMode = FP_DENORM_FLUSH_IN_OUT;
    }

    return floatMode.u32All;
}

} // Gfx9

} // Llpc
