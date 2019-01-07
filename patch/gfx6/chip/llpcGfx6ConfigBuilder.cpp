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
 * @file  llpcGfx6ConfigBuilder.cpp
 * @brief LLPC header file: contains implementation of class Llpc::Gfx6::ConfigBuilder.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-gfx6-config-builder"

#include "SPIRVInternal.h"
#include "llpcAbiMetadata.h"
#include "llpcContext.h"
#include "llpcCodeGenManager.h"
#include "llpcGfx6ConfigBuilder.h"

namespace llvm
{
namespace cl
{

extern opt<bool> InRegEsGsLdsSize;

} // cl

} // llvm

namespace Llpc
{

namespace Gfx6
{

#include "si_ci_vi_merged_enum.h"
#include "si_ci_vi_merged_offset.h"

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

        if ((hasTs == false) && (m_hasGs == false))
        {
            // VS-FS pipeline
            result = BuildPipelineVsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
        }
        else if (hasTs && (m_hasGs == false))
        {
            // VS-TS-FS pipeline
            result = BuildPipelineVsTsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
        }
        else if ((hasTs == false) && m_hasGs)
        {
            // VS-GS-FS pipeline
            result = BuildPipelineVsGsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
        }
        else
        {
            // VS-TS-GS-FS pipeline
            result = BuildPipelineVsTsGsFsRegConfig(m_pContext, &m_pConfig, &m_configSize);
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

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint64_t hash64 = 0;

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsFsRegConfig)];
    PipelineVsFsRegConfig* pConfig = reinterpret_cast<PipelineVsFsRegConfig*>(pAllocBuf);
    pConfig->Init();

    BuildApiHwShaderMapping(Util::Abi::HwShaderVs,
                            0,
                            0,
                            0,
                            Util::Abi::HwShaderPs,
                            0,
                            pConfig);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        result = BuildVsRegConfig<PipelineVsFsRegConfig>(pContext, ShaderStageVertex, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_REAL);

        hash64 = pContext->GetShaderHashCode(ShaderStageVertex);
        SET_REG(pConfig, API_VS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_VS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
        if (pIntfData->vbTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, INDIRECT_TABLE_ENTRY, pIntfData->vbTable.resNodeIdx);
        }

        if (pIntfData->streamOutTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, STREAM_OUT_TABLE_ENTRY, pIntfData->streamOutTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        hash64 = pContext->GetShaderHashCode(ShaderStageFragment);
        SET_REG(pConfig, API_PS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_PS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const uint32_t primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

    hash64 = pContext->GetPiplineHashCode();
    SET_REG(pConfig, PIPELINE_HASH_LO, static_cast<uint32_t>(hash64));
    SET_REG(pConfig, PIPELINE_HASH_HI, static_cast<uint32_t>(hash64 >> 32));

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
    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint64_t hash64 = 0;

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsTsFsRegConfig)];
    PipelineVsTsFsRegConfig* pConfig = reinterpret_cast<PipelineVsTsFsRegConfig*>(pAllocBuf);
    pConfig->Init();

    BuildApiHwShaderMapping(Util::Abi::HwShaderLs,
                            Util::Abi::HwShaderHs,
                            Util::Abi::HwShaderVs,
                            0,
                            Util::Abi::HwShaderPs,
                            0,
                            pConfig);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        result = BuildLsRegConfig<PipelineVsTsFsRegConfig>(pContext, ShaderStageVertex, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

        hash64 = pContext->GetShaderHashCode(ShaderStageVertex);
        SET_REG(pConfig, API_VS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_VS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
        if (pIntfData->vbTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, INDIRECT_TABLE_ENTRY, pIntfData->vbTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageTessControl)))
    {
        result = BuildHsRegConfig<PipelineVsTsFsRegConfig>(pContext, ShaderStageTessControl, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);

        hash64 = pContext->GetShaderHashCode(ShaderStageTessControl);
        SET_REG(pConfig, API_HS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_HS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageTessEval)))
    {
        result = BuildVsRegConfig<PipelineVsTsFsRegConfig>(pContext, ShaderStageTessEval, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_DS);

        hash64 = pContext->GetShaderHashCode(ShaderStageTessEval);
        SET_REG(pConfig, API_DS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_DS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageTessEval);
        if (pIntfData->streamOutTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, STREAM_OUT_TABLE_ENTRY, pIntfData->streamOutTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsTsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        hash64 = pContext->GetShaderHashCode(ShaderStageFragment);
        SET_REG(pConfig, API_PS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_PS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    if (pContext->IsTessOffChip())
    {
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
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

    SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

    // Set up VGT_TF_PARAM
    SetupVgtTfParam<PipelineVsTsFsRegConfig>(pContext, pConfig);

    hash64 = pContext->GetPiplineHashCode();
    SET_REG(pConfig, PIPELINE_HASH_LO, static_cast<uint32_t>(hash64));
    SET_REG(pConfig, PIPELINE_HASH_HI, static_cast<uint32_t>(hash64 >> 32));

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

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint64_t hash64 = 0;

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsGsFsRegConfig)];
    PipelineVsGsFsRegConfig* pConfig = reinterpret_cast<PipelineVsGsFsRegConfig*>(pAllocBuf);
    pConfig->Init();

    BuildApiHwShaderMapping(Util::Abi::HwShaderEs,
                            0,
                            0,
                            Util::Abi::HwShaderGs | Util::Abi::HwShaderVs,
                            Util::Abi::HwShaderPs,
                            0,
                            pConfig);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        result = BuildEsRegConfig<PipelineVsGsFsRegConfig>(pContext, ShaderStageVertex, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_REAL);

        hash64 = pContext->GetShaderHashCode(ShaderStageVertex);
        SET_REG(pConfig, API_VS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_VS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
        if (pIntfData->vbTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, INDIRECT_TABLE_ENTRY, pIntfData->vbTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageGeometry)))
    {
        result = BuildGsRegConfig<PipelineVsGsFsRegConfig>(pContext, ShaderStageGeometry, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

        hash64 = pContext->GetShaderHashCode(ShaderStageGeometry);
        SET_REG(pConfig, API_GS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_GS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageGeometry);
        if (pIntfData->streamOutTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, STREAM_OUT_TABLE_ENTRY, pIntfData->streamOutTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsGsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        hash64 = pContext->GetShaderHashCode(ShaderStageFragment);
        SET_REG(pConfig, API_PS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_PS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageCopyShader)))
    {
        result = BuildVsRegConfig<PipelineVsGsFsRegConfig>(pContext, ShaderStageCopyShader, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const uint32_t primGroupSize = 128;
    iaMultiVgtParam.bits.PRIMGROUP_SIZE = primGroupSize - 1;

    SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

    hash64 = pContext->GetPiplineHashCode();
    SET_REG(pConfig, PIPELINE_HASH_LO, static_cast<uint32_t>(hash64));
    SET_REG(pConfig, PIPELINE_HASH_HI, static_cast<uint32_t>(hash64 >> 32));

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

    const uint32_t stageMask = pContext->GetShaderStageMask();

    uint64_t hash64 = 0;

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineVsTsGsFsRegConfig)];
    PipelineVsTsGsFsRegConfig* pConfig = reinterpret_cast<PipelineVsTsGsFsRegConfig*>(pAllocBuf);
    pConfig->Init();

    BuildApiHwShaderMapping(Util::Abi::HwShaderLs,
                            Util::Abi::HwShaderHs,
                            Util::Abi::HwShaderEs,
                            Util::Abi::HwShaderGs | Util::Abi::HwShaderVs,
                            Util::Abi::HwShaderPs,
                            0,
                            pConfig);

    if (stageMask & ShaderStageToMask(ShaderStageVertex))
    {
        result = BuildLsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageVertex, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, LS_EN, LS_STAGE_ON);

        hash64 = pContext->GetShaderHashCode(ShaderStageVertex);
        SET_REG(pConfig, API_VS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_VS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageVertex);
        if (pIntfData->vbTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, INDIRECT_TABLE_ENTRY, pIntfData->vbTable.resNodeIdx);
        }

        if (pIntfData->streamOutTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, STREAM_OUT_TABLE_ENTRY, pIntfData->streamOutTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageTessControl)))
    {
        result = BuildHsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageTessControl, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, HS_EN, HS_STAGE_ON);

        hash64 = pContext->GetShaderHashCode(ShaderStageTessControl);
        SET_REG(pConfig, API_HS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_HS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageTessEval)))
    {
        result = BuildEsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageTessEval, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, ES_EN, ES_STAGE_DS);

        hash64 = pContext->GetShaderHashCode(ShaderStageTessEval);
        SET_REG(pConfig, API_DS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_DS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageGeometry)))
    {
        result = BuildGsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageGeometry, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, GS_EN, GS_STAGE_ON);

        hash64 = pContext->GetShaderHashCode(ShaderStageGeometry);
        SET_REG(pConfig, API_GS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_GS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
        const auto pIntfData = pContext->GetShaderInterfaceData(ShaderStageGeometry);
        if (pIntfData->streamOutTable.resNodeIdx != InvalidValue)
        {
            SET_REG(pConfig, STREAM_OUT_TABLE_ENTRY, pIntfData->streamOutTable.resNodeIdx);
        }
#endif
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageFragment)))
    {
        result = BuildPsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageFragment, pConfig);

        hash64 = pContext->GetShaderHashCode(ShaderStageFragment);
        SET_REG(pConfig, API_PS_HASH_DWORD0, static_cast<uint32_t>(hash64));
        SET_REG(pConfig, API_PS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));
    }

    if ((result == Result::Success) && (stageMask & ShaderStageToMask(ShaderStageCopyShader)))
    {
        result = BuildVsRegConfig<PipelineVsTsGsFsRegConfig>(pContext, ShaderStageCopyShader, pConfig);

        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, VS_EN, VS_STAGE_COPY_SHADER);
    }

    if (pContext->IsTessOffChip())
    {
        SET_REG_FIELD(pConfig, VGT_SHADER_STAGES_EN, DYNAMIC_HS, true);
    }

    // Set up IA_MULTI_VGT_PARAM
    regIA_MULTI_VGT_PARAM iaMultiVgtParam = {};

    const auto& tcsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs;
    const auto& tesBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
    const auto& gsBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

    if (tcsBuiltInUsage.primitiveId || tesBuiltInUsage.primitiveId || gsBuiltInUsage.primitiveId)
    {
        iaMultiVgtParam.bits.PARTIAL_ES_WAVE_ON = true;
        iaMultiVgtParam.bits.SWITCH_ON_EOI = true;
    }

    SET_REG(pConfig, IA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All);

    // Set up VGT_TF_PARAM
    SetupVgtTfParam<PipelineVsTsGsFsRegConfig>(pContext, pConfig);

    hash64 = pContext->GetPiplineHashCode();
    SET_REG(pConfig, PIPELINE_HASH_LO, static_cast<uint32_t>(hash64));
    SET_REG(pConfig, PIPELINE_HASH_HI, static_cast<uint32_t>(hash64 >> 32));

    LLPC_ASSERT((ppConfig != nullptr) && (pConfigSize != nullptr));
    *ppConfig = pAllocBuf;
    *pConfigSize = pConfig->GetRegCount() * sizeof(Util::Abi::PalMetadataNoteEntry);

    return result;
}

// =====================================================================================================================
// Builds register configuration for compute pipeline.
Result ConfigBuilder::BuildPipelineCsRegConfig(
    Context*            pContext,        // [in] LLPC context
    uint8_t**           ppConfig,        // [out] Register configuration for compute pipeline
    size_t*             pConfigSize)     // [out] Size of register configuration
{
    Result result = Result::Success;

    LLPC_ASSERT(pContext->GetShaderStageMask() == ShaderStageToMask(ShaderStageCompute));

    uint64_t hash64 = 0;

    uint8_t* pAllocBuf = new uint8_t[sizeof(PipelineCsRegConfig)];
    PipelineCsRegConfig* pConfig = reinterpret_cast<PipelineCsRegConfig*>(pAllocBuf);
    pConfig->Init();

    BuildApiHwShaderMapping(0,
                            0,
                            0,
                            0,
                            0,
                            Util::Abi::HwShaderCs,
                            pConfig);

    result = BuildCsRegConfig(pContext, ShaderStageCompute, pConfig);

    hash64 = pContext->GetShaderHashCode(ShaderStageCompute);
    SET_REG(pConfig, API_CS_HASH_DWORD0, static_cast<uint32_t>(hash64));
    SET_REG(pConfig, API_CS_HASH_DWORD1, static_cast<uint32_t>(hash64 >> 32));

    hash64 = pContext->GetPiplineHashCode();
    SET_REG(pConfig, PIPELINE_HASH_LO, static_cast<uint32_t>(hash64));
    SET_REG(pConfig, PIPELINE_HASH_HI, static_cast<uint32_t>(hash64 >> 32));

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
        SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, USER_SGPR, Llpc::CopyShaderUserSgprCount);
        SET_REG(&pConfig->m_vsRegs, VS_NUM_AVAIL_SGPRS, pContext->GetGpuProperty()->maxSgprsAvailable);
        SET_REG(&pConfig->m_vsRegs, VS_NUM_AVAIL_VGPRS, pContext->GetGpuProperty()->maxVgprsAvailable);

        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN,
            (pResUsage->inOutUsage.gs.outLocCount[0] > 0) && enableXfb);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN,
            pResUsage->inOutUsage.gs.outLocCount[1] > 0);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG,
            STREAMOUT_2_EN, pResUsage->inOutUsage.gs.outLocCount[2] > 0);
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

        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_0_EN, enableXfb);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_1_EN, false);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_2_EN, false);
        SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_CONFIG, STREAMOUT_3_EN, false);

        SET_REG(&pConfig->m_vsRegs, VS_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
        SET_REG(&pConfig->m_vsRegs, VS_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_EN, enableXfb);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE0_EN, (xfbStrides[0] > 0));
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE1_EN, (xfbStrides[1] > 0));
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE2_EN, (xfbStrides[2] > 0));
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_SHADER_PGM_RSRC2_VS, SO_BASE3_EN, (xfbStrides[3] > 0));

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_0, STRIDE, xfbStrides[0] / sizeof(int));
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_1, STRIDE, xfbStrides[1] / sizeof(int));
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_2, STRIDE, xfbStrides[2] / sizeof(int));
    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_STRMOUT_VTX_STRIDE_3, STRIDE, xfbStrides[3] / sizeof(int));

    uint32_t streamBufferConfig = 0;
    for (auto i = 0; i < MaxGsStreams; ++i)
    {
        streamBufferConfig |= (pResUsage->inOutUsage.streamXfbBuffers[i] << (i * 4));
    }
    SET_REG(&pConfig->m_vsRegs, VGT_STRMOUT_BUFFER_CONFIG, streamBufferConfig);

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

        const auto pGsIntfData = pContext->GetShaderInterfaceData(ShaderStageGeometry);
        if (cl::InRegEsGsLdsSize && pContext->IsGsOnChip())
        {
            SET_DYN_REG(pConfig,
                        mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderEsGsLdsSize,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }

        if (enableXfb)
        {
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 473
            SET_DYN_REG(pConfig,
                mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderStreamOutTable,
                0);
#else
            SET_DYN_REG(pConfig,
                mmSPI_SHADER_USER_DATA_VS_0 + pGsIntfData->userDataUsage.gs.copyShaderStreamOutTable,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
#endif
        }
    }

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_PRIMITIVEID_EN, PRIMITIVEID_EN, usePrimitiveId);
    SET_REG_FIELD(&pConfig->m_vsRegs, SPI_VS_OUT_CONFIG, VS_EXPORT_COUNT, pResUsage->inOutUsage.expCount - 1);
    SET_REG(&pConfig->m_vsRegs, USES_VIEWPORT_ARRAY_INDEX, useViewportIndex);

    // According to the IA_VGT_Spec, it is only legal to enable vertex reuse when we're using viewport array
    // index if each GS, DS, or VS invocation emits the same viewport array index for each vertex and we set
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

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_REUSE_OFF, REUSE_OFF, disableVertexReuse);

    SET_REG_FIELD(&pConfig->m_vsRegs, VGT_VERTEX_REUSE_BLOCK_CNTL, VTX_REUSE_DEPTH, 14);

    useLayer = useLayer || pPipelineInfo->iaState.enableMultiView;

    if (usePointSize || useLayer || useViewportIndex)
    {
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_POINT_SIZE, usePointSize);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_RENDER_TARGET_INDX, useLayer);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, USE_VTX_VIEWPORT_INDX, useViewportIndex);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_VEC_ENA, true);
        SET_REG_FIELD(&pConfig->m_vsRegs, PA_CL_VS_OUT_CNTL, VS_OUT_MISC_SIDE_BUS_ENA, true);
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

    if (result == Result::Success)
    {
        // Set shader user data maping
        result = ConfigBuilder::BuildUserDataConfig<T>(pContext, shaderStage, mmSPI_SHADER_USER_DATA_VS_0, pConfig);
    }

    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware hull shader.
template <typename T>
Result ConfigBuilder::BuildHsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for hull-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT(shaderStage == ShaderStageTessControl);

    const auto& pIntfData = pContext->GetShaderInterfaceData(shaderStage);
    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& calcFactor = pResUsage->inOutUsage.tcs.calcFactor;
    const auto& builtInUsage = pResUsage->builtInUsage.tcs;

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_hsRegs, SPI_SHADER_PGM_RSRC1_HS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_hsRegs, SPI_SHADER_PGM_RSRC1_HS, DX10_CLAMP, true);  // Follow PAL setting

    const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
    SET_REG_FIELD(&pConfig->m_hsRegs, SPI_SHADER_PGM_RSRC1_HS, DEBUG_MODE, pShaderInfo->options.debugMode);
    SET_REG_FIELD(&pConfig->m_hsRegs, SPI_SHADER_PGM_RSRC2_HS, TRAP_PRESENT, pShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_hsRegs, SPI_SHADER_PGM_RSRC2_HS, USER_SGPR, pIntfData->userDataCount);

    if (pContext->IsTessOffChip())
    {
        SET_REG_FIELD(&pConfig->m_hsRegs, SPI_SHADER_PGM_RSRC2_HS, OC_LDS_EN, true);
    }

    // Minimum and maximum tessellation factors supported by the hardware.
    constexpr float MinTessFactor = 1.0f;
    constexpr float MaxTessFactor = 64.0f;
    SET_REG(&pConfig->m_hsRegs, VGT_HOS_MIN_TESS_LEVEL, FloatToBits(MinTessFactor));
    SET_REG(&pConfig->m_hsRegs, VGT_HOS_MAX_TESS_LEVEL, FloatToBits(MaxTessFactor));

    // Set VGT_LS_HS_CONFIG
    SET_REG_FIELD(&pConfig->m_hsRegs, VGT_LS_HS_CONFIG, NUM_PATCHES, calcFactor.patchCountPerThreadGroup);
    auto pPipelineInfo = static_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
    SET_REG_FIELD(&pConfig->m_hsRegs, VGT_LS_HS_CONFIG, HS_NUM_INPUT_CP, pPipelineInfo->iaState.patchControlPoints);

    auto hsNumOutputCp = builtInUsage.outputVertices;
    SET_REG_FIELD(&pConfig->m_hsRegs, VGT_LS_HS_CONFIG, HS_NUM_OUTPUT_CP, hsNumOutputCp);

    SET_REG(&pConfig->m_hsRegs, HS_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
    SET_REG(&pConfig->m_hsRegs, HS_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);
    result = ConfigBuilder::BuildUserDataConfig<T>(pContext, shaderStage, mmSPI_SHADER_USER_DATA_HS_0, pConfig);

    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware export shader.
template <typename T>
Result ConfigBuilder::BuildEsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for export-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT((shaderStage == ShaderStageVertex) || (shaderStage == ShaderStageTessEval));

    const auto pIntfData = pContext->GetShaderInterfaceData(shaderStage);

    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage;

    LLPC_ASSERT((pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageGeometry)) != 0);
    const auto& calcFactor = pContext->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC1_ES, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC1_ES, DX10_CLAMP, true); // Follow PAL setting

    const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
    SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC1_ES, DEBUG_MODE, pShaderInfo->options.debugMode);
    SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC2_ES, TRAP_PRESENT, pShaderInfo->options.trapPresent);
    if (pContext->IsGsOnChip())
    {
        LLPC_ASSERT(calcFactor.gsOnChipLdsSize <= pContext->GetGpuProperty()->gsOnChipMaxLdsSize);
        LLPC_ASSERT((calcFactor.gsOnChipLdsSize %
                     (1 << pContext->GetGpuProperty()->ldsSizeDwordGranularityShift)) == 0);
        SET_REG_FIELD(&pConfig->m_esRegs,
                      SPI_SHADER_PGM_RSRC2_ES,
                      LDS_SIZE__CI__VI,
                      (calcFactor.gsOnChipLdsSize >>
                       pContext->GetGpuProperty()->ldsSizeDwordGranularityShift));
    }

    uint32_t vgprCompCnt = 0;
    if (shaderStage == ShaderStageVertex)
    {
        if (builtInUsage.vs.instanceIndex)
        {
            vgprCompCnt = 3; // Enable instance ID
        }
    }
    else
    {
        LLPC_ASSERT(shaderStage == ShaderStageTessEval);

        // NOTE: when primitive ID is used, set vgtCompCnt to 3 directly because primitive ID is the last VGPR.
        if (builtInUsage.tes.primitiveId)
        {
            vgprCompCnt = 3;
        }
        else
        {
            vgprCompCnt = 2;
        }

        if (pContext->IsTessOffChip())
        {
            SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC2_ES, OC_LDS_EN, true);
        }
    }

    SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC1_ES, VGPR_COMP_CNT, vgprCompCnt);

    SET_REG_FIELD(&pConfig->m_esRegs, SPI_SHADER_PGM_RSRC2_ES, USER_SGPR, pIntfData->userDataCount);

    SET_REG_FIELD(&pConfig->m_esRegs, VGT_ESGS_RING_ITEMSIZE, ITEMSIZE, calcFactor.esGsRingItemSize);

    SET_REG(&pConfig->m_esRegs, ES_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
    SET_REG(&pConfig->m_esRegs, ES_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);

    // Set shader user data maping
    result = ConfigBuilder::BuildUserDataConfig<T>(pContext, shaderStage, mmSPI_SHADER_USER_DATA_ES_0, pConfig);

    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware local shader.
template <typename T>
Result ConfigBuilder::BuildLsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for local-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT(shaderStage == ShaderStageVertex);

    const auto& pIntfData = pContext->GetShaderInterfaceData(shaderStage);
    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage.vs;

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC1_LS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC1_LS, DX10_CLAMP, true);  // Follow PAL setting
    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC1_LS, DEBUG_MODE, pShaderInfo->options.debugMode);
    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC2_LS, TRAP_PRESENT, pShaderInfo->options.trapPresent);

    uint32_t vgtCompCnt = 1;
    if (builtInUsage.instanceIndex)
    {
        vgtCompCnt += 2; // Enable instance ID
    }
    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC1_LS, VGPR_COMP_CNT, vgtCompCnt);

    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC2_LS, USER_SGPR, pIntfData->userDataCount);

    const auto& calcFactor = pContext->GetShaderResourceUsage(ShaderStageTessControl)->inOutUsage.tcs.calcFactor;

    uint32_t ldsSizeInDwords = calcFactor.onChip.patchConstStart +
                               calcFactor.patchConstSize * calcFactor.patchCountPerThreadGroup;
    if (pContext->IsTessOffChip())
    {
        ldsSizeInDwords = calcFactor.inPatchSize * calcFactor.patchCountPerThreadGroup;
    }

    auto pGpuWorkarounds = pContext->GetGpuWorkarounds();

    // Override the LDS size based on hardware workarounds.
    if (pGpuWorkarounds->gfx6.shaderSpiBarrierMgmt != 0)
    {
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
        const uint32_t outputVertices =
            pContext->GetShaderResourceUsage(ShaderStageTessControl)->builtInUsage.tcs.outputVertices;

        const uint32_t threadGroupSize = calcFactor.patchCountPerThreadGroup * outputVertices;
        const uint32_t waveSize = pContext->GetGpuProperty()->waveSize;
        const uint32_t wavesPerThreadGroup = (threadGroupSize + waveSize - 1) / waveSize;

        if (wavesPerThreadGroup > 1)
        {
            constexpr uint32_t MinLdsSizeWa = 1024; // 4KB in DWORDs.
            ldsSizeInDwords = std::max(ldsSizeInDwords, MinLdsSizeWa);
        }
    }

    uint32_t ldsSize = 0;

    // NOTE: On GFX6, granularity for the LDS_SIZE field is 64. The range is 0~128 which allocates 0 to 8K DWORDs.
    // On GFX7+, granularity for the LDS_SIZE field is 128. The range is 0~128 which allocates 0 to 16K DWORDs.
    const uint32_t ldsSizeDwordGranularityShift = pContext->GetGpuProperty()->ldsSizeDwordGranularityShift;
    const uint32_t ldsSizeDwordGranularity = 1u << ldsSizeDwordGranularityShift;
    ldsSize = Pow2Align(ldsSizeInDwords, ldsSizeDwordGranularity) >> ldsSizeDwordGranularityShift;

    SET_REG_FIELD(&pConfig->m_lsRegs, SPI_SHADER_PGM_RSRC2_LS, LDS_SIZE, ldsSize);

    SET_REG(&pConfig->m_lsRegs, LS_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
    SET_REG(&pConfig->m_lsRegs, LS_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);

    // Set shader user data maping
    result = ConfigBuilder::BuildUserDataConfig<T>(pContext, shaderStage, mmSPI_SHADER_USER_DATA_LS_0, pConfig);
    return result;
}

// =====================================================================================================================
// Builds register configuration for hardware geometry shader.
template <typename T>
Result ConfigBuilder::BuildGsRegConfig(
    Context*            pContext,       // [in] LLPC context
    ShaderStage         shaderStage,    // Current shader stage (from API side)
    T*                  pConfig)        // [out] Register configuration for geometry-shader-specific pipeline
{
    Result result = Result::Success;

    LLPC_ASSERT(shaderStage == ShaderStageGeometry);

    const auto pIntfData = pContext->GetShaderInterfaceData(shaderStage);

    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage.gs;
    const auto& inOutUsage   = pResUsage->inOutUsage;

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_gsRegs, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_gsRegs, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP, true);  // Follow PAL setting

    const auto pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);
    SET_REG_FIELD(&pConfig->m_gsRegs, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE, pShaderInfo->options.debugMode);
    SET_REG_FIELD(&pConfig->m_gsRegs, SPI_SHADER_PGM_RSRC2_GS, TRAP_PRESENT, pShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_gsRegs, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR, pIntfData->userDataCount);

    const bool primAdjacency = (builtInUsage.inputPrimitive == InputLinesAdjacency) ||
                               (builtInUsage.inputPrimitive == InputTrianglesAdjacency);

    // Maximum number of GS primitives per ES thread is capped by the hardware's GS-prim FIFO.
    auto pGpuProp = pContext->GetGpuProperty();
    uint32_t maxGsPerEs = (pGpuProp->gsPrimBufferDepth + pGpuProp->waveSize);

    // This limit is halved if the primitive topology is adjacency-typed
    if (primAdjacency)
    {
        maxGsPerEs >>= 1;
    }

    uint32_t maxVertOut = std::max(1u, static_cast<uint32_t>(builtInUsage.outputVertices));
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MAX_VERT_OUT, MAX_VERT_OUT, maxVertOut);

    // TODO: Currently only support offchip GS
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, MODE, GS_SCENARIO_G);
    if (pContext->IsGsOnChip())
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, ONCHIP__CI__VI, VGT_GS_MODE_ONCHIP_ON);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, false);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, false);

        uint32_t gsPrimsPerSubgrp = std::min(maxGsPerEs, inOutUsage.gs.calcFactor.gsPrimsPerSubgroup);

        SET_REG_FIELD(&pConfig->m_gsRegs,
                      VGT_GS_ONCHIP_CNTL__CI__VI,
                      ES_VERTS_PER_SUBGRP,
                      inOutUsage.gs.calcFactor.esVertsPerSubgroup);

        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_ONCHIP_CNTL__CI__VI, GS_PRIMS_PER_SUBGRP, gsPrimsPerSubgrp);

        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_ES_PER_GS, ES_PER_GS, inOutUsage.gs.calcFactor.esVertsPerSubgroup);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_PER_ES, GS_PER_ES, gsPrimsPerSubgrp);

        if (cl::InRegEsGsLdsSize)
        {
            SET_DYN_REG(pConfig,
                        mmSPI_SHADER_USER_DATA_GS_0 + pIntfData->userDataUsage.gs.esGsLdsSize,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::EsGsLdsSize));
        }
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, ONCHIP__CI__VI, VGT_GS_MODE_ONCHIP_OFF);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, ES_WRITE_OPTIMIZE, true);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, GS_WRITE_OPTIMIZE, true);
        SET_REG(&pConfig->m_gsRegs, VGT_GS_ONCHIP_CNTL__CI__VI, 0);

        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_ES_PER_GS, ES_PER_GS, EsThreadsPerGsThread);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_PER_ES, GS_PER_ES, std::min(maxGsPerEs, GsPrimsPerEsThread));
    }
    if (builtInUsage.outputVertices <= 128)
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_128);
    }
    else if (builtInUsage.outputVertices <= 256)
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_256);
    }
    else if (builtInUsage.outputVertices <= 512)
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_512);
    }
    else
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_MODE, CUT_MODE, GS_CUT_1024);
    }

    uint32_t gsVertItemSize0 = sizeof(uint32_t) * inOutUsage.gs.outLocCount[0];
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_VERT_ITEMSIZE, ITEMSIZE, gsVertItemSize0);

    uint32_t gsVertItemSize1 = sizeof(uint32_t) * inOutUsage.gs.outLocCount[1];
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_VERT_ITEMSIZE_1, ITEMSIZE, gsVertItemSize1);

    uint32_t gsVertItemSize2 = sizeof(uint32_t) * inOutUsage.gs.outLocCount[2];
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_VERT_ITEMSIZE_2, ITEMSIZE, gsVertItemSize2);

    uint32_t gsVertItemSize3 = sizeof(uint32_t) * inOutUsage.gs.outLocCount[3];
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_VERT_ITEMSIZE_3, ITEMSIZE, gsVertItemSize3);

    uint32_t gsVsRingOffset = gsVertItemSize0 * maxVertOut;
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GSVS_RING_OFFSET_1, OFFSET, gsVsRingOffset);

    gsVsRingOffset += gsVertItemSize1 * maxVertOut;
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GSVS_RING_OFFSET_2, OFFSET, gsVsRingOffset);

    gsVsRingOffset += gsVertItemSize2 * maxVertOut;
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GSVS_RING_OFFSET_3, OFFSET, gsVsRingOffset);

    if ((builtInUsage.invocations > 1) || builtInUsage.invocationId)
    {
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_INSTANCE_CNT, ENABLE, true);
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_INSTANCE_CNT, CNT, builtInUsage.invocations);
    }
    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_PER_VS, GS_PER_VS, GsThreadsPerVsThread);

    VGT_GS_OUTPRIM_TYPE gsOutputPrimitiveType = TRISTRIP;
    if (inOutUsage.outputMapLocCount == 0)
    {
        gsOutputPrimitiveType = POINTLIST;
    }
    else if (builtInUsage.outputPrimitive == OutputPoints)
    {
        gsOutputPrimitiveType = POINTLIST;
    }
    else if (builtInUsage.outputPrimitive == LINESTRIP)
    {
        gsOutputPrimitiveType = LINESTRIP;
    }

    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE, gsOutputPrimitiveType);

    // Set multi-stream output primitive type
    if ((gsVertItemSize1 > 0) || (gsVertItemSize2 > 0) || (gsVertItemSize3 > 0))
    {
        const static auto GS_OUT_PRIM_INVALID = 3u;
        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_1,
            (gsVertItemSize1 > 0) ? gsOutputPrimitiveType : GS_OUT_PRIM_INVALID);

        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_2,
            (gsVertItemSize2 > 0) ? gsOutputPrimitiveType : GS_OUT_PRIM_INVALID);

        SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GS_OUT_PRIM_TYPE, OUTPRIM_TYPE_3,
            (gsVertItemSize3 > 0) ? gsOutputPrimitiveType : GS_OUT_PRIM_INVALID);
    }

    SET_REG_FIELD(&pConfig->m_gsRegs, VGT_GSVS_RING_ITEMSIZE, ITEMSIZE, inOutUsage.gs.calcFactor.gsVsRingItemSize);

    SET_REG(&pConfig->m_gsRegs, GS_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
    SET_REG(&pConfig->m_gsRegs, GS_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);
    // Set shader user data maping
    result = ConfigBuilder::BuildUserDataConfig<T>(pContext, shaderStage, mmSPI_SHADER_USER_DATA_GS_0, pConfig);

    return result;
}

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
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, ALPHA_TO_MASK_DISABLE, builtInUsage.sampleMask);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, DEPTH_BEFORE_SHADER, builtInUsage.earlyFragmentTests);
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, EXEC_ON_NOOP,
                  (builtInUsage.earlyFragmentTests && pResUsage->resourceWrite));
    SET_REG_FIELD(&pConfig->m_psRegs, DB_SHADER_CONTROL, EXEC_ON_HIER_FAIL, execOnHeirFail);

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

    if ((spiShaderColFormat == 0) && (depthExpFmt == EXP_FORMAT_ZERO))
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
                spiPsInputCntl.bits.FP16_INTERP_MODE__VI = true;
                spiPsInputCntl.bits.ATTR0_VALID__VI = true;
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
    SET_REG(&pConfig->m_psRegs, PS_USES_UAVS, static_cast<uint32_t>((pResUsage->resourceWrite ||
                                                                     pResUsage->resourceRead)));
    SET_REG(&pConfig->m_psRegs, PS_WRITES_UAVS, static_cast<uint32_t>(pResUsage->resourceWrite));
    SET_REG(&pConfig->m_psRegs, PS_WRITES_DEPTH, static_cast<uint32_t>(builtInUsage.fragDepth));
#else
    SET_REG(&pConfig->m_psRegs, PS_USES_UAVS, static_cast<uint32_t>(pResUsage->resourceWrite));
#endif

    SET_REG(&pConfig->m_psRegs, PS_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
    SET_REG(&pConfig->m_psRegs, PS_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);

    if (result == Result::Success)
    {
        // Set shader user data mapping
        result = ConfigBuilder::BuildUserDataConfig<T>(pContext, shaderStage, mmSPI_SHADER_USER_DATA_PS_0, pConfig);
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

    uint32_t floatMode = SetupFloatingPointMode(pContext, shaderStage);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, FLOAT_MODE, floatMode);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, DX10_CLAMP, true);  // Follow PAL setting
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC1, DEBUG_MODE, pShaderInfo->options.debugMode);

    // Set registers based on shader interface data
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TRAP_PRESENT, pShaderInfo->options.trapPresent);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, USER_SGPR, pIntfData->userDataCount);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TGID_X_EN, true);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TGID_Y_EN, true);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TGID_Z_EN, true);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TG_SIZE_EN, true);

    // 0 = X, 1 = XY, 2 = XYZ
    uint32_t tidigCompCnt = 0;
    if (builtInUsage.workgroupSizeZ > 1)
    {
        tidigCompCnt = 2;
    }
    else if (builtInUsage.workgroupSizeY > 1)
    {
        tidigCompCnt = 1;
    }
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_PGM_RSRC2, TIDIG_COMP_CNT, tidigCompCnt);

    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_NUM_THREAD_X, NUM_THREAD_FULL, builtInUsage.workgroupSizeX);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_NUM_THREAD_Y, NUM_THREAD_FULL, builtInUsage.workgroupSizeY);
    SET_REG_FIELD(&pConfig->m_csRegs, COMPUTE_NUM_THREAD_Z, NUM_THREAD_FULL, builtInUsage.workgroupSizeZ);

    SET_REG(&pConfig->m_csRegs, CS_NUM_AVAIL_SGPRS, pResUsage->numSgprsAvailable);
    SET_REG(&pConfig->m_csRegs, CS_NUM_AVAIL_VGPRS, pResUsage->numVgprsAvailable);

    // Set shader user data mapping
    if (result == Result::Success)
    {
        result = ConfigBuilder::BuildUserDataConfig<PipelineCsRegConfig>(pContext,
                                                                         shaderStage,
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
    ShaderStage shaderStage,    // Current shader stage (from API side)
    uint32_t    startUserData,  // Starting user data
    T*          pConfig)        // [out] Register configuration for the associated pipeline
{
    Result result = Result::Success;

    bool enableMultiView = false;
    if (pContext->IsGraphics())
    {
        enableMultiView = static_cast<const GraphicsPipelineBuildInfo*>(
            pContext->GetPipelineBuildInfo())->iaState.enableMultiView;
    }

    const auto pIntfData = pContext->GetShaderInterfaceData(shaderStage);
    const auto pResUsage = pContext->GetShaderResourceUsage(shaderStage);
    const auto& builtInUsage = pResUsage->builtInUsage;

    // Stage-specific processing
    if (shaderStage == ShaderStageVertex)
    {
        // TODO: PAL only check BaseVertex now, we need update code once PAL check them separately.
        if (builtInUsage.vs.baseVertex || builtInUsage.vs.baseInstance)
        {
            LLPC_ASSERT(pIntfData->entryArgIdxs.vs.baseVertex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.vs.baseVertex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::BaseVertex));

            LLPC_ASSERT(pIntfData->entryArgIdxs.vs.baseInstance > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.vs.baseInstance,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::BaseInstance));
        }

        if (builtInUsage.vs.drawIndex)
        {
            LLPC_ASSERT(pIntfData->entryArgIdxs.vs.drawIndex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.vs.drawIndex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::DrawIndex));
        }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 473
        if (pIntfData->userDataUsage.vs.vbTablePtr > 0)
        {
            LLPC_ASSERT(pIntfData->userDataMap[pIntfData->userDataUsage.vs.vbTablePtr] ==
                InterfaceData::UserDataUnmapped);

            SET_DYN_REG(pConfig,
                startUserData + pIntfData->userDataUsage.vs.vbTablePtr,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::VertexBufferTable));
        }

        if (pIntfData->userDataUsage.vs.streamOutTablePtr > 0)
        {
            LLPC_ASSERT(pIntfData->userDataMap[pIntfData->userDataUsage.vs.streamOutTablePtr] ==
                InterfaceData::UserDataUnmapped);

            SET_DYN_REG(pConfig,
                startUserData + pIntfData->userDataUsage.vs.streamOutTablePtr,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }
#endif

        if (enableMultiView)
        {
            LLPC_ASSERT(pIntfData->entryArgIdxs.vs.viewIndex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.vs.viewIndex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
        }
    }
    else if (shaderStage == ShaderStageTessEval)
    {
        if (enableMultiView)
        {
            LLPC_ASSERT(pIntfData->entryArgIdxs.tes.viewIndex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.tes.viewIndex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 473
        if (pIntfData->userDataUsage.tes.streamOutTablePtr > 0)
        {
            LLPC_ASSERT(pIntfData->userDataMap[pIntfData->userDataUsage.tes.streamOutTablePtr] ==
                InterfaceData::UserDataUnmapped);

            SET_DYN_REG(pConfig,
                startUserData + pIntfData->userDataUsage.tes.streamOutTablePtr,
                static_cast<uint32_t>(Util::Abi::UserDataMapping::StreamOutTable));
        }
#endif
    }
    else if (shaderStage == ShaderStageGeometry)
    {
        if (builtInUsage.gs.viewIndex)
        {
            LLPC_ASSERT(pIntfData->entryArgIdxs.gs.viewIndex > 0);
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.gs.viewIndex,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::ViewId));
        }
    }
    else if (shaderStage == ShaderStageCompute)
    {
        if (builtInUsage.cs.numWorkgroups > 0)
        {
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.cs.numWorkgroupsPtr,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::Workgroup));
        }
    }

    SET_DYN_REG(pConfig, startUserData, static_cast<uint32_t>(Util::Abi::UserDataMapping::GlobalTable));

    if (pResUsage->perShaderTable)
    {
        SET_DYN_REG(pConfig, startUserData + 1, static_cast<uint32_t>(Util::Abi::UserDataMapping::PerShaderTable));
    }

    uint32_t userDataLimit = 0;
    uint32_t spillThreshold = UINT32_MAX;
    if (shaderStage != ShaderStageCopyShader)
    {
        uint32_t maxUserDataCount = pContext->GetGpuProperty()->maxUserDataCount;
        for (uint32_t i = 0; i < maxUserDataCount; ++i)
        {
            if (pIntfData->userDataMap[i] != InterfaceData::UserDataUnmapped)
            {
                SET_DYN_REG(pConfig, startUserData + i, pIntfData->userDataMap[i]);
                userDataLimit = std::max(userDataLimit, pIntfData->userDataMap[i] + 1);
            }
        }

        if (pIntfData->userDataUsage.spillTable > 0)
        {
            SET_DYN_REG(pConfig,
                        startUserData + pIntfData->userDataUsage.spillTable,
                        static_cast<uint32_t>(Util::Abi::UserDataMapping::SpillTable));
            userDataLimit = std::max(userDataLimit,
                                     pIntfData->spillTable.offsetInDwords + pIntfData->spillTable.sizeInDwords);
            spillThreshold = pIntfData->spillTable.offsetInDwords;
        }
    }

    if (userDataLimit > GET_REG(pConfig, USER_DATA_LIMIT))
    {
        SET_REG(pConfig, USER_DATA_LIMIT, userDataLimit)
    }

    if (spillThreshold < GET_REG(pConfig, SPILL_THRESHOLD))
    {
        SET_REG(pConfig, SPILL_THRESHOLD, spillThreshold)
    }

    return result;
}

// =====================================================================================================================
// Sets up the register value for VGT_TF_PARAM.
template <typename T>
void ConfigBuilder::SetupVgtTfParam(
    Context* pContext,  // [in] LLPC context
    T*       pConfig)   // [out] Register configuration for the associated pipeline
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
}

// =====================================================================================================================
// Builds metadata API_HW_SHADER_MAPPING_HI/LO.
void ConfigBuilder::BuildApiHwShaderMapping(
    uint32_t           vsHwShader,    // Hardware shader mapping for vertex shader
    uint32_t           tcsHwShader,   // Hardware shader mapping for tessellation control shader
    uint32_t           tesHwShader,   // Hardware shader mapping for tessellation evaluation shader
    uint32_t           gsHwShader,    // Hardware shader mapping for geometry shader
    uint32_t           fsHwShader,    // Hardware shader mapping for fragment shader
    uint32_t           csHwShader,    // Hardware shader mapping for compute shader
    PipelineRegConfig* pConfig)       // [out] Register configuration for the associated pipeline
{
    Util::Abi::ApiHwShaderMapping apiHwShaderMapping = {};

    apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Cs)] = csHwShader;
    apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Vs)] = vsHwShader;
    apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Hs)] = tcsHwShader;
    apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Ds)] = tesHwShader;
    apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Gs)] = gsHwShader;
    apiHwShaderMapping.apiShaders[static_cast<uint32_t>(Util::Abi::ApiShaderType::Ps)] = fsHwShader;

    SET_REG(pConfig, API_HW_SHADER_MAPPING_LO, apiHwShaderMapping.u32Lo);
    SET_REG(pConfig, API_HW_SHADER_MAPPING_HI, apiHwShaderMapping.u32Hi);
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

} // Gfx6

} // Llpc
