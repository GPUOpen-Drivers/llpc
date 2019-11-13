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

#if LLPC_BUILD_GFX10
extern opt<int> SubgroupSize;
#endif

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
    InitShaderResourceUsage(ShaderStageCompute, GetShaderResourceUsage(ShaderStageCompute));
    InitShaderInterfaceData(GetShaderInterfaceData(ShaderStageCompute));
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
#if LLPC_BUILD_GFX10
    LLPC_ASSERT(stage == ShaderStageCompute);

    if (m_gfxIp.major == 10)
    {
        // NOTE: GPU property wave size is used in shader, unless:
        //  1) If specified by tuning option, use the specified wave size.
        //  2) If gl_SubgroupSize is used in shader, use the specified subgroup size when required.

        if (m_pPipelineInfo->cs.options.waveSize != 0)
        {
            waveSize = m_pPipelineInfo->cs.options.waveSize;
        }

        // Check is subgroup size used in shader. If it's used, use the specified subgroup size as wave size.
        const PipelineShaderInfo* pShaderInfo = GetPipelineShaderInfo(ShaderStageCompute);
        const ShaderModuleData* pModuleData =
            reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);

        if ((pModuleData != nullptr) && pModuleData->usage.useSubgroupSize
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 31
         && (m_pPipelineInfo->cs.options.allowVaryWaveSize == false)
#endif
           )
        {
            waveSize = cl::SubgroupSize;
        }

        LLPC_ASSERT((waveSize == 32) || (waveSize == 64));
    }
    else if (m_gfxIp.major > 10)
    {
        LLPC_NOT_IMPLEMENTED();
    }
#endif
    return waveSize;

}

} // Llpc
