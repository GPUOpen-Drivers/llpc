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
 * @file  llpcPipelineContext.h
 * @brief LLPC header file: contains declaration of class Llpc::PipelineContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_map>
#include <unordered_set>
#include "spirvExt.h"

#include "llpc.h"
#include "llpcCompiler.h"
#include "vkgcMetroHash.h"
#include "lgc/llpcPipeline.h"

namespace lgc
{

class Pipeline;

} // lgc

namespace Llpc
{

// Enumerates types of descriptor.
enum class DescriptorType : uint32_t
{
    UniformBlock = 0,     // Uniform block
    ShaderStorageBlock,   // Shader storage block
    Texture,              // Combined texture
    TextureResource,      // Separated texture resource
    TextureSampler,       // Separated texture sampler
    TexelBuffer,          // Texture buffer and image buffer
    Image,                // Image
    SubpassInput,         // Subpass input
};

// Represents floating-point control setting.
struct FloatControl
{
    bool    denormPerserve;             // Preserve denormals
    bool    denormFlushToZero;          // Flush denormals to zero
    bool    signedZeroInfNanPreserve;   // Preserve signed zero/INF/NaN
    bool    roundingModeRTE;            // Rounding mode: to nearest even
    bool    roundingModeRTZ;            // Rounding mode: to zero
};

// Represents the info of a descriptor binding
struct DescriptorBinding
{
    DescriptorType descType;        // Type of the descriptor
    uint32_t       arraySize;       // Element count of arrayed binding (flattened)
    bool           isMultisampled;  // Whether multisampled texture is used
};

typedef std::vector<DescriptorBinding> DescriptorSet;

// Shader FP mode for use by front-end
struct ShaderFpMode
{
    uint32_t denormPerserve            : 4;  // Bitmask of denormPerserve flags
    uint32_t denormFlushToZero         : 4;  // Bitmask of denormFlushToZero flags
    uint32_t signedZeroInfNanPreserve  : 4;  // Bitmask of signedZeroInfNanPreserve flags
    uint32_t roundingModeRTE           : 4;  // Bitmask of roundingModeRTE flags
    uint32_t roundingModeRTZ           : 4;  // Bitmask of roundingModeRTZ flags
};

// =====================================================================================================================
// Represents pipeline-specific context for pipeline compilation, it is a part of LLPC context
class PipelineContext
{
public:
    PipelineContext(GfxIpVersion           gfxIp,
                    MetroHash::Hash*       pPipelineHash,
                    MetroHash::Hash*       pCacheHash);
    virtual ~PipelineContext();

    // Checks whether the pipeline is graphics or compute
    virtual bool IsGraphics() const = 0;

    // Gets pipeline shader info of the specified shader stage
    virtual const PipelineShaderInfo* GetPipelineShaderInfo(ShaderStage shaderStage) const = 0;

    // Gets pipeline build info
    virtual const void* GetPipelineBuildInfo() const = 0;

    // Gets the mask of active shader stages bound to this pipeline
    virtual uint32_t GetShaderStageMask() const = 0;

    // Sets the mask of active shader stages bound to this pipeline
    virtual void SetShaderStageMask(uint32_t mask) = 0;

    // Gets the count of active shader stages
    virtual uint32_t GetActiveShaderStageCount() const = 0;

    // Does user data node merge for merged shader
    virtual void DoUserDataNodeMerge() = 0;

    static void GetGpuNameString(GfxIpVersion gfxIp, std::string& gpuName);
    static const char* GetGpuNameAbbreviation(GfxIpVersion gfxIp);

    // Gets graphics IP version info
    GfxIpVersion GetGfxIpVersion() const { return m_gfxIp; }

    // Gets pipeline hash code
    uint64_t GetPiplineHashCode() const { return MetroHash::Compact64(&m_pipelineHash); }
    uint64_t GetCacheHashCode() const { return MetroHash::Compact64(&m_cacheHash); }

    virtual ShaderHash GetShaderHashCode(ShaderStage stage) const;

    // Gets per pipeline options
    virtual const PipelineOptions* GetPipelineOptions() const = 0;

    // Set pipeline state in lgc::Pipeline object for middle-end
    void SetPipelineState(lgc::Pipeline* pPipeline) const;

    // Get ShaderFpMode struct for the given shader stage
    ShaderFpMode& GetShaderFpMode(ShaderStage stage) { return m_shaderFpModes[stage]; }

    // Map a VkFormat to a {BufDataFormat, BufNumFormat}. Returns BufDataFormatInvalid if the
    // VkFormat is not supported.
    static std::pair<lgc::BufDataFormat, lgc::BufNumFormat> MapVkFormat(VkFormat format, bool isColorExport);

protected:
    // Gets dummy vertex input create info
    virtual VkPipelineVertexInputStateCreateInfo* GetDummyVertexInputInfo() { return nullptr; }

    // Gets dummy vertex binding info
    virtual std::vector<VkVertexInputBindingDescription>* GetDummyVertexBindings() { return nullptr; }

    // Gets dummy vertex attribute info
    virtual std::vector<VkVertexInputAttributeDescription>* GetDummyVertexAttributes() { return nullptr; }

    // -----------------------------------------------------------------------------------------------------------------

    GfxIpVersion           m_gfxIp;         // Graphics IP version info
    MetroHash::Hash        m_pipelineHash;  // Pipeline hash code
    MetroHash::Hash        m_cacheHash;     // Cache hash code

private:
    PipelineContext() = delete;
    PipelineContext(const PipelineContext&) = delete;
    PipelineContext& operator=(const PipelineContext&) = delete;

    // Type of immutable nodes map used in SetUserDataNodesTable
    typedef std::map<std::pair<uint32_t, uint32_t>, const DescriptorRangeValue*> ImmutableNodesMap;

    // Give the pipeline options to the middle-end.
    void SetOptionsInPipeline(lgc::Pipeline* pPipeline) const;

    // Give the user data nodes and descriptor range values to the middle-end.
    void SetUserDataInPipeline(lgc::Pipeline* pPipeline) const;
    void SetUserDataNodesTable(llvm::LLVMContext&                   context,
                               llvm::ArrayRef<ResourceMappingNode>  nodes,
                               const ImmutableNodesMap&             immutableNodesMap,
                               lgc::ResourceNode*                   pDestTable,
                               lgc::ResourceNode*&                  pDestInnerTable) const;

    // Give the graphics pipeline state to the middle-end.
    void SetGraphicsStateInPipeline(lgc::Pipeline* pPipeline) const;

    // Set vertex input descriptions in middle-end Pipeline
    void SetVertexInputDescriptions(lgc::Pipeline* pPipeline) const;

    // Give the color export state to the middle-end.
    void SetColorExportState(lgc::Pipeline* pPipeline) const;

    // -----------------------------------------------------------------------------------------------------------------

    ShaderFpMode           m_shaderFpModes[ShaderStageCountInternal] = {};
};

} // Llpc
