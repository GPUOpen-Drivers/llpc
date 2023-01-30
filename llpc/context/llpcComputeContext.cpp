/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcComputeContext.h"
#include "SPIRVInternal.h"
#include "lgc/Pipeline.h"

#define DEBUG_TYPE "llpc-compute-context"

using namespace llvm;

namespace Llpc {

// =====================================================================================================================
//
// @param gfxIp : Graphics Ip version info
// @param pipelineInfo : Compute pipeline build info
// @param pipelineHash : Pipeline hash code
// @param cacheHash : Cache hash code
ComputeContext::ComputeContext(GfxIpVersion gfxIp, const ComputePipelineBuildInfo *pipelineInfo,
                               MetroHash::Hash *pipelineHash, MetroHash::Hash *cacheHash)
    : PipelineContext(gfxIp, pipelineHash, cacheHash
#if VKI_RAY_TRACING
                      ,
                      &pipelineInfo->rtState
#endif
                      ),
      m_pipelineInfo(pipelineInfo) {
  setUnlinked(pipelineInfo->unlinked);
  m_resourceMapping = pipelineInfo->resourceMapping;
  m_pipelineLayoutApiHash = pipelineInfo->pipelineLayoutApiHash;
}

// =====================================================================================================================
// Gets pipeline shader info of the specified shader stage
//
// @param shaderStage : Shader stage
const PipelineShaderInfo *ComputeContext::getPipelineShaderInfo(ShaderStage shaderStage) const {
  assert(shaderStage == ShaderStageCompute);
  return &m_pipelineInfo->cs;
}

// =====================================================================================================================
// Gets subgroup size usage
//
// @returns : Bitmask per stage, in the same order as defined in `Vkgc::ShaderStage`.
unsigned ComputeContext::getSubgroupSizeUsage() const {
  const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(m_pipelineInfo->cs.pModuleData);
  return moduleData->usage.useSubgroupSize ? ShaderStageComputeBit : 0;
}

} // namespace Llpc
