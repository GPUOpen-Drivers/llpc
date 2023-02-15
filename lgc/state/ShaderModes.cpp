/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ShaderModes.cpp
 * @brief LLPC source file: contains implementation of class lgc::ShaderModes
 ***********************************************************************************************************************
 */

#include "lgc/state/ShaderModes.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"

#define DEBUG_TYPE "lgc-shader-modes"

using namespace lgc;
using namespace llvm;

// Names for named metadata nodes when storing and reading back pipeline state
static const char CommonShaderModeMetadataPrefix[] = "llpc.shader.mode.";
static const char TessellationModeMetadataName[] = "llpc.tessellation.mode";
static const char GeometryShaderModeMetadataName[] = "llpc.geometry.mode";
static const char MeshShaderModeMetadataName[] = "llpc.mesh.mode";
static const char FragmentShaderModeMetadataName[] = "llpc.fragment.mode";
static const char ComputeShaderModeMetadataName[] = "llpc.compute.mode";

// =====================================================================================================================
// Clear shader modes
void ShaderModes::clear() {
  memset(m_commonShaderModes, 0, sizeof(m_commonShaderModes));
}

// =====================================================================================================================
// Set the common shader mode (FP modes) for the given shader stage
//
// @param stage : Shader stage
// @param commonShaderMode : Common shader mode
void ShaderModes::setCommonShaderMode(ShaderStage stage, const CommonShaderMode &commonShaderMode) {
  auto modes = MutableArrayRef<CommonShaderMode>(m_commonShaderModes);
  modes[stage] = commonShaderMode;
  m_anySet = true;
}

// =====================================================================================================================
// Get the common shader mode (FP mode) for the given shader stage
//
// @param stage : Shader stage
const CommonShaderMode &ShaderModes::getCommonShaderMode(ShaderStage stage) {
  return ArrayRef<CommonShaderMode>(m_commonShaderModes)[stage];
}

// =====================================================================================================================
// Check if any shader stage has useSubgroupSize set
bool ShaderModes::getAnyUseSubgroupSize() {
  for (const auto &commonShaderMode : m_commonShaderModes) {
    if (commonShaderMode.useSubgroupSize)
      return true;
  }
  return false;
}

// =====================================================================================================================
// Set the tessellation mode. This in fact merges the supplied values with any previously supplied values,
// to allow the client to call this twice, once for TCS and once for TES.
//
// @param inMode : Tessellation mode
void ShaderModes::setTessellationMode(const TessellationMode &inMode) {
  assert(inMode.outputVertices <= MaxTessPatchVertices);

  m_tessellationMode.vertexSpacing =
      inMode.vertexSpacing != static_cast<VertexSpacing>(0) ? inMode.vertexSpacing : m_tessellationMode.vertexSpacing;
  m_tessellationMode.vertexOrder =
      inMode.vertexOrder != static_cast<VertexOrder>(0) ? inMode.vertexOrder : m_tessellationMode.vertexOrder;
  m_tessellationMode.primitiveMode =
      inMode.primitiveMode != static_cast<PrimitiveMode>(0) ? inMode.primitiveMode : m_tessellationMode.primitiveMode;
  m_tessellationMode.pointMode |= inMode.pointMode;
  m_tessellationMode.outputVertices =
      inMode.outputVertices != 0 ? inMode.outputVertices : m_tessellationMode.outputVertices;
}

// =====================================================================================================================
// Get the tessellation state.
const TessellationMode &ShaderModes::getTessellationMode() {
  // Ensure defaults are correctly set the first time the middle-end uses TessellationMode.
  if (m_tessellationMode.outputVertices == 0)
    m_tessellationMode.outputVertices = MaxTessPatchVertices;
  if (m_tessellationMode.vertexSpacing == VertexSpacing::Unknown)
    m_tessellationMode.vertexSpacing = VertexSpacing::Equal;
  if (m_tessellationMode.vertexOrder == VertexOrder::Unknown)
    m_tessellationMode.vertexOrder = VertexOrder::Ccw;
  if (m_tessellationMode.primitiveMode == PrimitiveMode::Unknown)
    m_tessellationMode.primitiveMode = PrimitiveMode::Triangles;
  return m_tessellationMode;
}

// =====================================================================================================================
// Set the geometry shader mode
void ShaderModes::setGeometryShaderMode(const GeometryShaderMode &inMode) {
  m_geometryShaderMode = inMode;
}

// =====================================================================================================================
// Get the geometry shader mode
const GeometryShaderMode &ShaderModes::getGeometryShaderMode() {
  return m_geometryShaderMode;
}

// =====================================================================================================================
// Set the mesh shader mode
void ShaderModes::setMeshShaderMode(const MeshShaderMode &inMode) {
  m_meshShaderMode = inMode;
}

// =====================================================================================================================
// Get the mesh shader mode
const MeshShaderMode &ShaderModes::getMeshShaderMode() {
  return m_meshShaderMode;
}

// =====================================================================================================================
// Set the fragment shader mode
void ShaderModes::setFragmentShaderMode(const FragmentShaderMode &inMode) {
  m_fragmentShaderMode = inMode;
}

// =====================================================================================================================
// Get the fragment shader mode
const FragmentShaderMode &ShaderModes::getFragmentShaderMode() {
  return m_fragmentShaderMode;
}

// =====================================================================================================================
// Set the compute shader mode (workgroup size)
//
// @param inMode : Compute shader state
void ShaderModes::setComputeShaderMode(const ComputeShaderMode &inMode) {
  // 0 is taken to be 1 in workgroup size.
  m_computeShaderMode.workgroupSizeX = std::max(1U, inMode.workgroupSizeX);
  m_computeShaderMode.workgroupSizeY = std::max(1U, inMode.workgroupSizeY);
  m_computeShaderMode.workgroupSizeZ = std::max(1U, inMode.workgroupSizeZ);
  m_computeShaderMode.derivatives = inMode.derivatives;

  assert(m_computeShaderMode.workgroupSizeX <= MaxComputeWorkgroupSize &&
         m_computeShaderMode.workgroupSizeY <= MaxComputeWorkgroupSize &&
         m_computeShaderMode.workgroupSizeZ <= MaxComputeWorkgroupSize);
}

// =====================================================================================================================
// Get the compute shader mode (workgroup size)
const ComputeShaderMode &ShaderModes::getComputeShaderMode() {
  return m_computeShaderMode;
}

// =====================================================================================================================
// Set subgroup size usage
//
// @param stage : Shader stage
// @param usage : Subgroup size usage
void ShaderModes::setSubgroupSizeUsage(ShaderStage stage, bool usage) {
  MutableArrayRef<CommonShaderMode> modes(m_commonShaderModes);
  modes[stage].useSubgroupSize = usage;
}

// =====================================================================================================================
// Record shader modes (common and specific) into IR metadata
//
// @param [in/out] module : Module to record the IR metadata in
void ShaderModes::record(Module *module) {
  // First the common state.
  for (unsigned stage = 0; stage < ArrayRef<CommonShaderMode>(m_commonShaderModes).size(); ++stage) {
    std::string metadataName =
        std::string(CommonShaderModeMetadataPrefix) + getShaderStageAbbreviation(static_cast<ShaderStage>(stage));
    PipelineState::setNamedMetadataToArrayOfInt32(module, m_commonShaderModes[stage], metadataName);
  }

  // Then the specific shader modes.
  PipelineState::setNamedMetadataToArrayOfInt32(module, m_tessellationMode, TessellationModeMetadataName);
  PipelineState::setNamedMetadataToArrayOfInt32(module, m_geometryShaderMode, GeometryShaderModeMetadataName);
  PipelineState::setNamedMetadataToArrayOfInt32(module, m_meshShaderMode, MeshShaderModeMetadataName);
  PipelineState::setNamedMetadataToArrayOfInt32(module, m_fragmentShaderMode, FragmentShaderModeMetadataName);
  PipelineState::setNamedMetadataToArrayOfInt32(module, m_computeShaderMode, ComputeShaderModeMetadataName);
}

// =====================================================================================================================
// Read shader modes (common and specific) from a shader IR module, but only if no modes have been set
// in this ShaderModes. This is used to handle the case that the shader module comes from an earlier
// shader compile, and it had its ShaderModes recorded into IR then.
//
// @param module : LLVM module
// @param stage : Shader stage
void ShaderModes::readModesFromShader(Module *module, ShaderStage stage) {
  // Bail if any modes have been set, which would mean that this is a full pipeline compile.
  if (m_anySet)
    return;

  // First the common state.
  std::string metadataName =
      std::string(CommonShaderModeMetadataPrefix) + getShaderStageAbbreviation(static_cast<ShaderStage>(stage));
  PipelineState::readNamedMetadataArrayOfInt32(module, metadataName, m_commonShaderModes[stage]);

  // Then the specific shader modes.
  switch (stage) {
  case ShaderStageTessControl:
  case ShaderStageTessEval:
    PipelineState::readNamedMetadataArrayOfInt32(module, TessellationModeMetadataName, m_tessellationMode);
    break;
  case ShaderStageGeometry:
    PipelineState::readNamedMetadataArrayOfInt32(module, GeometryShaderModeMetadataName, m_geometryShaderMode);
    break;
  case ShaderStageMesh:
    PipelineState::readNamedMetadataArrayOfInt32(module, MeshShaderModeMetadataName, m_meshShaderMode);
    break;
  case ShaderStageFragment:
    PipelineState::readNamedMetadataArrayOfInt32(module, FragmentShaderModeMetadataName, m_fragmentShaderMode);
    break;
  case ShaderStageCompute:
    PipelineState::readNamedMetadataArrayOfInt32(module, ComputeShaderModeMetadataName, m_computeShaderMode);
    break;
  default:
    break;
  }
}

// =====================================================================================================================
// Read shader modes (common and specific) from the pipeline IR module.
//
// @param module : LLVM module
void ShaderModes::readModesFromPipeline(Module *module) {
  // First the common state.
  for (unsigned stage = 0; stage < ArrayRef<CommonShaderMode>(m_commonShaderModes).size(); ++stage) {
    std::string metadataName =
        std::string(CommonShaderModeMetadataPrefix) + getShaderStageAbbreviation(static_cast<ShaderStage>(stage));
    PipelineState::readNamedMetadataArrayOfInt32(module, metadataName, m_commonShaderModes[stage]);
  }

  // Then the specific shader modes.
  PipelineState::readNamedMetadataArrayOfInt32(module, TessellationModeMetadataName, m_tessellationMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, GeometryShaderModeMetadataName, m_geometryShaderMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, MeshShaderModeMetadataName, m_meshShaderMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, FragmentShaderModeMetadataName, m_fragmentShaderMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, ComputeShaderModeMetadataName, m_computeShaderMode);
}
