/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcComputeContext.h
 * @brief LLPC header file: contains declaration of class Llpc::ComputeContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPipelineContext.h"

namespace Llpc
{

// =====================================================================================================================
// Represents LLPC context for compute pipeline compilation. Derived from the base class Llpc::Context.
class ComputeContext: public PipelineContext
{
public:
    ComputeContext(GfxIpVersion                    gfxIp,
                   const ComputePipelineBuildInfo* pPipelineInfo,
                   MetroHash::Hash*                pPipelineHash,
                   MetroHash::Hash*                pCacheHash);
    virtual ~ComputeContext() {}

    virtual const PipelineShaderInfo* GetPipelineShaderInfo(ShaderStage shaderStage) const;

    // Checks whether the pipeline is graphics or compute
    virtual bool IsGraphics() const { return false; }

    // Gets pipeline build info
    virtual const void* GetPipelineBuildInfo() const { return m_pPipelineInfo; }

    // Gets the mask of active shader stages bound to this pipeline
    virtual uint32_t GetShaderStageMask() const { return ShaderStageToMask(ShaderStageCompute); }

    // Sets the mask of active shader stages bound to this pipeline
    void SetShaderStageMask(uint32_t mask) { assert(mask == GetShaderStageMask()); }

    // Gets the count of active shader stages
    virtual uint32_t GetActiveShaderStageCount() const { return 1; }

    // Does user data node merging for all shader stages
    virtual void DoUserDataNodeMerge() { }

    // Gets per pipeline options
    virtual const PipelineOptions* GetPipelineOptions() const { return &m_pPipelineInfo->options; }

private:
    LLPC_DISALLOW_DEFAULT_CTOR(ComputeContext);
    LLPC_DISALLOW_COPY_AND_ASSIGN(ComputeContext);

    const ComputePipelineBuildInfo*     m_pPipelineInfo; // Info to build a compute pipeline
};

} // Llpc
