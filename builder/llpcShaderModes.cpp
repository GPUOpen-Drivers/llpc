/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcShaderModes.cpp
 * @brief LLPC source file: contains implementation of class Llpc::ShaderModes
 ***********************************************************************************************************************
 */

#include "llpc.h"
#include "llpcIntrinsDefs.h"
#include "llpcPipelineState.h"
#include "llpcShaderModes.h"
#include "llpcUtil.h"

#define DEBUG_TYPE "llpc-shader-modes"

using namespace Llpc;
using namespace llvm;

// Names for named metadata nodes when storing and reading back pipeline state
static const char CommonShaderModeMetadataPrefix[] = "llpc.shader.mode.";
static const char TessellationModeMetadataName[] = "llpc.tessellation.mode";
static const char GeometryShaderModeMetadataName[] = "llpc.geometry.mode";
static const char FragmentShaderModeMetadataName[] = "llpc.fragment.mode";
static const char ComputeShaderModeMetadataName[] = "llpc.compute.mode";

// =====================================================================================================================
// Clear shader modes
void ShaderModes::Clear()
{
    memset(m_commonShaderModes, 0, sizeof(m_commonShaderModes));
}

// =====================================================================================================================
// Set the common shader mode (FP modes) for the given shader stage
void ShaderModes::SetCommonShaderMode(
    ShaderStage             stage,             // Shader stage
    const CommonShaderMode& commonShaderMode)  // [in] Common shader mode
{
    auto modes = MutableArrayRef<CommonShaderMode>(m_commonShaderModes);
    modes[stage] = commonShaderMode;
    m_anySet = true;
}

// =====================================================================================================================
// Get the common shader mode (FP mode) for the given shader stage
const CommonShaderMode& ShaderModes::GetCommonShaderMode(
    ShaderStage                       stage)              // Shader stage
{
    return ArrayRef<CommonShaderMode>(m_commonShaderModes)[stage];
}

// =====================================================================================================================
// Set the tessellation mode. This in fact merges the supplied values with any previously supplied values,
// to allow the client to call this twice, once for TCS and once for TES.
void ShaderModes::SetTessellationMode(
    const TessellationMode& inMode)   // [in] Tessellation mode
{
    LLPC_ASSERT(inMode.outputVertices <= MaxTessPatchVertices);

    m_tessellationMode.vertexSpacing = (inMode.vertexSpacing != static_cast<VertexSpacing>(0)) ?
                                        inMode.vertexSpacing : m_tessellationMode.vertexSpacing;
    m_tessellationMode.vertexOrder = (inMode.vertexOrder != static_cast<VertexOrder>(0)) ?
                                      inMode.vertexOrder : m_tessellationMode.vertexOrder;
    m_tessellationMode.primitiveMode = (inMode.primitiveMode != static_cast<PrimitiveMode>(0)) ?
                                        inMode.primitiveMode : m_tessellationMode.primitiveMode;
    m_tessellationMode.pointMode |= inMode.pointMode;
    m_tessellationMode.outputVertices = (inMode.outputVertices != 0) ?
                                            inMode.outputVertices : m_tessellationMode.outputVertices;
}

// =====================================================================================================================
// Get the tessellation state.
const TessellationMode& ShaderModes::GetTessellationMode()
{
    // Ensure defaults are correctly set the first time the middle-end uses TessellationMode.
    if (m_tessellationMode.outputVertices == 0)
    {
        m_tessellationMode.outputVertices = MaxTessPatchVertices;
    }
    if (m_tessellationMode.vertexSpacing == VertexSpacing::Unknown)
    {
        m_tessellationMode.vertexSpacing = VertexSpacing::Equal;
    }
    if (m_tessellationMode.vertexOrder == VertexOrder::Unknown)
    {
        m_tessellationMode.vertexOrder = VertexOrder::Ccw;
    }
    if (m_tessellationMode.primitiveMode == PrimitiveMode::Unknown)
    {
        m_tessellationMode.primitiveMode = PrimitiveMode::Triangles;
    }
    return m_tessellationMode;
}

// =====================================================================================================================
// Set the geometry shader mode
void ShaderModes::SetGeometryShaderMode(const GeometryShaderMode& inMode)
{
    m_geometryShaderMode = inMode;
}

// =====================================================================================================================
// Get the geometry shader mode
const GeometryShaderMode& ShaderModes::GetGeometryShaderMode()
{
    return m_geometryShaderMode;
}

// =====================================================================================================================
// Set the fragment shader mode
void ShaderModes::SetFragmentShaderMode(const FragmentShaderMode& inMode)
{
    m_fragmentShaderMode = inMode;
}

// =====================================================================================================================
// Get the fragment shader mode
const FragmentShaderMode& ShaderModes::GetFragmentShaderMode()
{
    return m_fragmentShaderMode;
}

// =====================================================================================================================
// Set the compute shader mode (workgroup size)
void ShaderModes::SetComputeShaderMode(
    const ComputeShaderMode& inMode)   // [in] Compute shader state
{
    // 0 is taken to be 1 in workgroup size.
    m_computeShaderMode.workgroupSizeX = std::max(1U, inMode.workgroupSizeX);
    m_computeShaderMode.workgroupSizeY = std::max(1U, inMode.workgroupSizeY);
    m_computeShaderMode.workgroupSizeZ = std::max(1U, inMode.workgroupSizeZ);

    LLPC_ASSERT((m_computeShaderMode.workgroupSizeX <= MaxComputeWorkgroupSize) &&
                (m_computeShaderMode.workgroupSizeY <= MaxComputeWorkgroupSize) &&
                (m_computeShaderMode.workgroupSizeZ <= MaxComputeWorkgroupSize));
}

// =====================================================================================================================
// Get the compute shader mode (workgroup size)
const ComputeShaderMode& ShaderModes::GetComputeShaderMode()
{
    return m_computeShaderMode;
}

// =====================================================================================================================
// Record shader modes (common and specific) into IR metadata
void ShaderModes::Record(
    Module* pModule)    // [in/out] Module to record the IR metadata in
{
    // First the common state.
    for (uint32_t stage = 0; stage < ArrayRef<CommonShaderMode>(m_commonShaderModes).size(); ++stage)
    {
        std::string metadataName = std::string(CommonShaderModeMetadataPrefix) +
                                   GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true);
        PipelineState::SetNamedMetadataToArrayOfInt32(pModule, m_commonShaderModes[stage], metadataName);
    }

    // Then the specific shader modes.
    PipelineState::SetNamedMetadataToArrayOfInt32(pModule, m_tessellationMode, TessellationModeMetadataName);
    PipelineState::SetNamedMetadataToArrayOfInt32(pModule, m_geometryShaderMode, GeometryShaderModeMetadataName);
    PipelineState::SetNamedMetadataToArrayOfInt32(pModule, m_fragmentShaderMode, FragmentShaderModeMetadataName);
    PipelineState::SetNamedMetadataToArrayOfInt32(pModule, m_computeShaderMode, ComputeShaderModeMetadataName);
}

// =====================================================================================================================
// Read shader modes (common and specific) from a shader IR module, but only if no modes have been set
// in this ShaderModes. This is used to handle the case that the shader module comes from an earlier
// shader compile, and it had its ShaderModes recorded into IR then.
void ShaderModes::ReadModesFromShader(
    Module*     pModule,    // [in] LLVM module
    ShaderStage stage)      // Shader stage
{
    // Bail if any modes have been set, which would mean that this is a full pipeline compile.
    if (m_anySet)
    {
        return;
    }

    // First the common state.
    std::string metadataName = std::string(CommonShaderModeMetadataPrefix) +
                               GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true);
    PipelineState::ReadNamedMetadataArrayOfInt32(pModule, metadataName, m_commonShaderModes[stage]);

    // Then the specific shader modes.
    switch (stage)
    {
    case ShaderStageTessControl:
    case ShaderStageTessEval:
        PipelineState::ReadNamedMetadataArrayOfInt32(pModule, TessellationModeMetadataName, m_tessellationMode);
        break;
    case ShaderStageGeometry:
        PipelineState::ReadNamedMetadataArrayOfInt32(pModule, GeometryShaderModeMetadataName, m_geometryShaderMode);
        break;
    case ShaderStageFragment:
        PipelineState::ReadNamedMetadataArrayOfInt32(pModule, FragmentShaderModeMetadataName, m_fragmentShaderMode);
        break;
    case ShaderStageCompute:
        PipelineState::ReadNamedMetadataArrayOfInt32(pModule,
                                                     ComputeShaderModeMetadataName,
                                                     m_computeShaderMode);
        break;
    default:
        break;
    }
}

// =====================================================================================================================
// Read shader modes (common and specific) from the pipeline IR module.
void ShaderModes::ReadModesFromPipeline(
    Module* pModule)    // [in] LLVM module
{
    // First the common state.
    for (uint32_t stage = 0; stage < ArrayRef<CommonShaderMode>(m_commonShaderModes).size(); ++stage)
    {
        std::string metadataName = std::string(CommonShaderModeMetadataPrefix) +
                                   GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true);
        PipelineState::ReadNamedMetadataArrayOfInt32(pModule, metadataName, m_commonShaderModes[stage]);
    }

    // Then the specific shader modes.
    PipelineState::ReadNamedMetadataArrayOfInt32(pModule, TessellationModeMetadataName, m_tessellationMode);
    PipelineState::ReadNamedMetadataArrayOfInt32(pModule, GeometryShaderModeMetadataName, m_geometryShaderMode);
    PipelineState::ReadNamedMetadataArrayOfInt32(pModule, FragmentShaderModeMetadataName, m_fragmentShaderMode);
    PipelineState::ReadNamedMetadataArrayOfInt32(pModule, ComputeShaderModeMetadataName, m_computeShaderMode);
}

