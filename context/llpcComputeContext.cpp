/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcComputeContext.cpp
 * @brief LLPC source file: contains implementation of class Llpc::ComputeContext.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-compute-context"

#include "llpcComputeContext.h"
#include "SPIRVInternal.h"

using namespace llvm;

namespace llvm
{

namespace cl
{

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
ComputeContext::ComputeContext(
    GfxIpVersion                    gfxIp,            // Graphics Ip version info
    const GpuProperty*              pGpuProp,         // [in] GPU Property
    const WorkaroundFlags*          pGpuWorkarounds,  // [in] GPU workarounds
    const ComputePipelineBuildInfo* pPipelineInfo,    // [in] Compute pipeline build info
    MetroHash::Hash*                pPipelineHash,    // [in] Pipeline hash code
    MetroHash::Hash*                pCacheHash)       // [in] Cache hash code
    :
    PipelineContext(gfxIp, pGpuProp, pGpuWorkarounds, pPipelineHash, pCacheHash),
    m_pPipelineInfo(pPipelineInfo)
{
    InitShaderResourceUsage(ShaderStageCompute);
    InitShaderInterfaceData(ShaderStageCompute);
}

// =====================================================================================================================
// Gets resource usage of the specified shader stage.
ResourceUsage* ComputeContext::GetShaderResourceUsage(
    ShaderStage shaderStage) // Shader stage
{
    LLPC_ASSERT(shaderStage == ShaderStageCompute);
    return &m_resUsage;
}

// =====================================================================================================================
// Gets interface data of the specified shader stage.
InterfaceData* ComputeContext::GetShaderInterfaceData(
    ShaderStage shaderStage)  // Shader stage
{
    LLPC_ASSERT(shaderStage == ShaderStageCompute);
    return &m_intfData;
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
const PipelineShaderInfo* ComputeContext::GetPipelineShaderInfo(
    ShaderStage shaderStage // Shader stage
    ) const
{
    LLPC_ASSERT(shaderStage == ShaderStageCompute);
    return &m_pPipelineInfo->cs;
}

// =====================================================================================================================
// Gets wave size for the specified shader stage
//
// NOTE: Need to be called after PatchResourceCollect pass, so usage of subgroupSize is confirmed.
uint32_t ComputeContext::GetShaderWaveSize(
    ShaderStage stage)  // Shader stage
{
    uint32_t waveSize = m_pGpuProperty->waveSize;
    return waveSize;

}

// =====================================================================================================================
// Gets float control settings of the specified shader stage for the provide floating-point type.
FloatControl ComputeContext::GetShaderFloatControl(
    ShaderStage shaderStage,    // Shader stage
    uint32_t    bitWidth        // Bit width of the floating-point type
    ) const
{
    LLPC_ASSERT(shaderStage == ShaderStageCompute);

    FloatControl floatControl = {};
    const auto& commonUsage = m_resUsage.builtInUsage.common;

    switch (bitWidth)
    {
    case 16:
        floatControl.denormPerserve             = ((commonUsage.denormPerserve & SPIRVTW_16Bit) != 0);
        floatControl.denormFlushToZero          = ((commonUsage.denormFlushToZero & SPIRVTW_16Bit) != 0);
        floatControl.signedZeroInfNanPreserve   = ((commonUsage.signedZeroInfNanPreserve & SPIRVTW_16Bit) != 0);
        floatControl.roundingModeRTE            = ((commonUsage.roundingModeRTE & SPIRVTW_16Bit) != 0);
        floatControl.roundingModeRTZ            = ((commonUsage.roundingModeRTZ & SPIRVTW_16Bit) != 0);
        break;
    case 32:
        floatControl.denormPerserve             = ((commonUsage.denormPerserve & SPIRVTW_32Bit) != 0);
        floatControl.denormFlushToZero          = ((commonUsage.denormFlushToZero & SPIRVTW_32Bit) != 0);
        floatControl.signedZeroInfNanPreserve   = ((commonUsage.signedZeroInfNanPreserve & SPIRVTW_32Bit) != 0);
        floatControl.roundingModeRTE            = ((commonUsage.roundingModeRTE & SPIRVTW_32Bit) != 0);
        floatControl.roundingModeRTZ            = ((commonUsage.roundingModeRTZ & SPIRVTW_32Bit) != 0);
        break;
    case 64:
        floatControl.denormPerserve             = ((commonUsage.denormPerserve & SPIRVTW_64Bit) != 0);
        floatControl.denormFlushToZero          = ((commonUsage.denormFlushToZero & SPIRVTW_64Bit) != 0);
        floatControl.signedZeroInfNanPreserve   = ((commonUsage.signedZeroInfNanPreserve & SPIRVTW_64Bit) != 0);
        floatControl.roundingModeRTE            = ((commonUsage.roundingModeRTE & SPIRVTW_64Bit) != 0);
        floatControl.roundingModeRTZ            = ((commonUsage.roundingModeRTZ & SPIRVTW_64Bit) != 0);
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return floatControl;
}

} // Llpc
