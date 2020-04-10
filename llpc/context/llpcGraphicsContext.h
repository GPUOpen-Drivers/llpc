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
 * @file  llpcGraphicsContext.h
 * @brief LLPC header file: contains declaration of class Llpc::GraphicsContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPipelineContext.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace Llpc
{

// =====================================================================================================================
// Represents LLPC context for graphics pipeline compilation. Derived from the base class Llpc::Context.
class GraphicsContext: public PipelineContext
{
public:
    GraphicsContext(GfxIpVersion                     gfxIp,
                    const GraphicsPipelineBuildInfo* pPipelineInfo,
                    MetroHash::Hash*                 pPipelineHash,
                    MetroHash::Hash*                 pCacheHash);
    virtual ~GraphicsContext();

    virtual const PipelineShaderInfo* GetPipelineShaderInfo(ShaderStage shaderStage) const;

    // Checks whether the pipeline is graphics or compute
    virtual bool IsGraphics() const { return true; }

    // Gets pipeline build info
    virtual const void* GetPipelineBuildInfo() const { return m_pPipelineInfo; }

    // Gets the mask of active shader stages bound to this pipeline
    virtual unsigned GetShaderStageMask() const { return m_stageMask; }

    // Gets the mask of active shader stages bound to this pipeline
    void SetShaderStageMask(unsigned mask) { m_stageMask = mask; }

    // Gets the count of active shader stages
    virtual unsigned GetActiveShaderStageCount() const { return m_activeStageCount; }

    virtual void DoUserDataNodeMerge();

    // Gets per pipeline options
    virtual const PipelineOptions* GetPipelineOptions() const { return &m_pPipelineInfo->options; }

private:
    GraphicsContext() = delete;
    GraphicsContext(const GraphicsContext&) = delete;
    GraphicsContext& operator=(const GraphicsContext&) = delete;

    llvm::ArrayRef<ResourceMappingNode> MergeUserDataNodeTable(llvm::SmallVectorImpl<ResourceMappingNode>& allNodes);

    void BuildNggCullingControlRegister();

    const GraphicsPipelineBuildInfo*    m_pPipelineInfo; // Info to build a graphics pipeline

    unsigned m_stageMask; // Mask of active shader stages bound to this graphics pipeline
    unsigned m_activeStageCount;    // Count of active shader stages

    bool            m_gsOnChip;    // Whether to enable GS on-chip mode

    llvm::SmallVector<std::unique_ptr<llvm::SmallVectorImpl<ResourceMappingNode>>, 4>
                    m_allocUserDataNodes;               // Allocated merged user data nodes
    std::unique_ptr<llvm::SmallVectorImpl<DescriptorRangeValue>>
                    m_allocDescriptorRangeValues;       // Allocated merged descriptor range values
};

} // Llpc
