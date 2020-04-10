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
                    const GraphicsPipelineBuildInfo* pipelineInfo,
                    MetroHash::Hash*                 pipelineHash,
                    MetroHash::Hash*                 cacheHash);
    virtual ~GraphicsContext();

    virtual const PipelineShaderInfo* getPipelineShaderInfo(ShaderStage shaderStage) const;

    // Checks whether the pipeline is graphics or compute
    virtual bool isGraphics() const { return true; }

    // Gets pipeline build info
    virtual const void* getPipelineBuildInfo() const { return m_pipelineInfo; }

    // Gets the mask of active shader stages bound to this pipeline
    virtual unsigned getShaderStageMask() const { return m_stageMask; }

    // Gets the mask of active shader stages bound to this pipeline
    void setShaderStageMask(unsigned mask) { m_stageMask = mask; }

    // Gets the count of active shader stages
    virtual unsigned getActiveShaderStageCount() const { return m_activeStageCount; }

    virtual void doUserDataNodeMerge();

    // Gets per pipeline options
    virtual const PipelineOptions* getPipelineOptions() const { return &m_pipelineInfo->options; }

private:
    GraphicsContext() = delete;
    GraphicsContext(const GraphicsContext&) = delete;
    GraphicsContext& operator=(const GraphicsContext&) = delete;

    llvm::ArrayRef<ResourceMappingNode> mergeUserDataNodeTable(llvm::SmallVectorImpl<ResourceMappingNode>& allNodes);

    void buildNggCullingControlRegister();

    const GraphicsPipelineBuildInfo*    m_pipelineInfo; // Info to build a graphics pipeline

    unsigned m_stageMask; // Mask of active shader stages bound to this graphics pipeline
    unsigned m_activeStageCount;    // Count of active shader stages

    bool            m_gsOnChip;    // Whether to enable GS on-chip mode

    llvm::SmallVector<std::unique_ptr<llvm::SmallVectorImpl<ResourceMappingNode>>, 4>
                    m_allocUserDataNodes;               // Allocated merged user data nodes
    std::unique_ptr<llvm::SmallVectorImpl<DescriptorRangeValue>>
                    m_allocDescriptorRangeValues;       // Allocated merged descriptor range values
};

} // Llpc
