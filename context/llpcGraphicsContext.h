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
                    const GpuProperty*               pGpuProp,
                    const WorkaroundFlags*           pGpuWorkarounds,
                    const GraphicsPipelineBuildInfo* pPipelineInfo,
                    MetroHash::Hash*                 pPipelineHash,
                    MetroHash::Hash*                 pCacheHash);
    virtual ~GraphicsContext();

    virtual ResourceUsage* GetShaderResourceUsage(ShaderStage shaderStage);
    virtual InterfaceData* GetShaderInterfaceData(ShaderStage shaderStage);
    virtual const PipelineShaderInfo* GetPipelineShaderInfo(ShaderStage shaderStage) const;

    // Checks whether the pipeline is graphics or compute
    virtual bool IsGraphics() const { return true; }

    // Gets pipeline build info
    virtual const void* GetPipelineBuildInfo() const { return m_pPipelineInfo; }

    // Gets the mask of active shader stages bound to this pipeline
    virtual uint32_t GetShaderStageMask() const { return m_stageMask; }

    // Gets the count of active shader stages
    virtual uint32_t GetActiveShaderStageCount() const { return m_activeStageCount; }

    virtual ShaderStage GetPrevShaderStage(ShaderStage shaderStage) const;
    virtual ShaderStage GetNextShaderStage(ShaderStage shaderStage) const;

    virtual void DoUserDataNodeMerge();

#if LLPC_BUILD_GFX10
    // Gets WGP mode enablement for the specified shader stage
    virtual bool GetShaderWgpMode(ShaderStage shaderStage) const;
#endif

    // Gets the count of vertices per primitive
    virtual uint32_t GetVerticesPerPrimitive() const;

    // Gets wave size for the specified shader stage
    virtual uint32_t GetShaderWaveSize(ShaderStage stage);

    // Gets per pipeline options
    virtual const PipelineOptions* GetPipelineOptions() const { return &m_pPipelineInfo->options; }

    // Checks whether the requirements of packing input/output is satisfied
    virtual bool CanPackInOut(ShaderStage shaderStage, bool isOutput) const;

    // Checks whether pack input/output is enabled
    virtual bool IsPackInOutEnabled() const { return m_packInOut; }

    // Enable/disable pack input/output
    virtual void EnablePackInOut(bool packInOut) { m_packInOut = packInOut; }

    void InitShaderInfoForNullFs();

private:
    LLPC_DISALLOW_DEFAULT_CTOR(GraphicsContext);
    LLPC_DISALLOW_COPY_AND_ASSIGN(GraphicsContext);

    llvm::ArrayRef<ResourceMappingNode> MergeUserDataNodeTable(llvm::SmallVectorImpl<ResourceMappingNode>& allNodes);

#if LLPC_BUILD_GFX10
    void BuildNggCullingControlRegister();
#endif

    const GraphicsPipelineBuildInfo*    m_pPipelineInfo; // Info to build a graphics pipeline

    uint32_t m_stageMask; // Mask of active shader stages bound to this graphics pipeline
    uint32_t m_activeStageCount;    // Count of active shader stages

    ResourceUsage   m_resUsages[ShaderStageGfxCount];   // Resource usages of all graphics shader stages
    InterfaceData   m_intfData[ShaderStageGfxCount];    // Interface data of all graphics shader stages

    bool            m_gsOnChip;    // Whether to enable GS on-chip mode
    bool            m_packInOut;    // Whether to enable/disable pack in/out

    llvm::SmallVector<std::unique_ptr<llvm::SmallVectorImpl<ResourceMappingNode>>, 4>
                    m_allocUserDataNodes;               // Allocated merged user data nodes
    std::unique_ptr<llvm::SmallVectorImpl<DescriptorRangeValue>>
                    m_allocDescriptorRangeValues;       // Allocated merged descriptor range values
};

} // Llpc
