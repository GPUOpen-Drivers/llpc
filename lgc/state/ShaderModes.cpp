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
#include "llvm/ADT/StringExtras.h"

#define DEBUG_TYPE "lgc-shader-modes"

using namespace lgc;
using namespace llvm;

// Names for named metadata nodes when storing and reading back pipeline state
static const char CommonShaderModeMetadataPrefix[] = "llpc.shader.mode.";
static const char TcsModeMetadataName[] = "llpc.tcs.mode";
static const char TesModeMetadataName[] = "llpc.tes.mode";
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
// @param module : Module to record in
// @param stage : Shader stage
// @param commonShaderMode : Common shader mode
void ShaderModes::setCommonShaderMode(Module &module, ShaderStage stage, const CommonShaderMode &commonShaderMode) {
  SmallString<64> metadataName(CommonShaderModeMetadataPrefix);
  metadataName += getShaderStageAbbreviation(static_cast<ShaderStage>(stage));
  // Or the mode into any existing recorded mode, in case the front-end has already called setSubgroupSizeUsage.
  PipelineState::orNamedMetadataToArrayOfInt32(&module, commonShaderMode, metadataName);
}

// =====================================================================================================================
// Get the common shader modes for the given shader stage: static edition that reads directly from IR.
CommonShaderMode ShaderModes::getCommonShaderMode(Module &module, ShaderStage stage) {
  SmallString<64> metadataName(CommonShaderModeMetadataPrefix);
  metadataName += getShaderStageAbbreviation(static_cast<ShaderStage>(stage));
  CommonShaderMode mode = {};
  PipelineState::readNamedMetadataArrayOfInt32(&module, metadataName, mode);
  return mode;
}

// =====================================================================================================================
// Get the common shader mode (FP mode) for the given shader stage
//
// @param stage : Shader stage
const CommonShaderMode &ShaderModes::getCommonShaderMode(ShaderStage stage) const {
  return ArrayRef<CommonShaderMode>(m_commonShaderModes)[stage];
}

// =====================================================================================================================
// Check if any shader stage has useSubgroupSize set
bool ShaderModes::getAnyUseSubgroupSize() const {
  for (const auto &commonShaderMode : m_commonShaderModes) {
    if (commonShaderMode.useSubgroupSize)
      return true;
  }
  return false;
}

// =====================================================================================================================
// Set the tessellation mode for the given shader stage (TCS or TES). The tessellation mode read back by a middle-end
// pass is a merge of the TCS and TES tessellation modes, to account for the fact that SPIR-V allows you to specify
// some things on either shader.
//
// @param module : Module to record in
// @param stage : Shader stage
// @param inMode : Tessellation mode
void ShaderModes::setTessellationMode(Module &module, ShaderStage stage, const TessellationMode &inMode) {
  assert(stage == ShaderStageTessControl || stage == ShaderStageTessEval);
  PipelineState::setNamedMetadataToArrayOfInt32(
      &module, inMode, stage == ShaderStageTessControl ? TcsModeMetadataName : TesModeMetadataName);
}

// =====================================================================================================================
// Get the tessellation mode for the given shader stage (TCS or TES): static edition that reads directly from IR.
TessellationMode ShaderModes::getTessellationMode(Module &module, ShaderStage stage) {
  assert(stage == ShaderStageTessControl || stage == ShaderStageTessEval);
  TessellationMode mode = {};
  PipelineState::readNamedMetadataArrayOfInt32(
      &module, stage == ShaderStageTessControl ? TcsModeMetadataName : TesModeMetadataName, mode);
  return mode;
}

// =====================================================================================================================
// Get the tessellation state.
const TessellationMode &ShaderModes::getTessellationMode() const {
  return m_tessellationMode;
}

// =====================================================================================================================
// Set the geometry shader mode
//
// @param module : Module to record in
// @param inMode : Geometry mode
void ShaderModes::setGeometryShaderMode(Module &module, const GeometryShaderMode &inMode) {
  PipelineState::setNamedMetadataToArrayOfInt32(&module, inMode, GeometryShaderModeMetadataName);
}

// =====================================================================================================================
// Get the geometry shader mode
const GeometryShaderMode &ShaderModes::getGeometryShaderMode() const {
  return m_geometryShaderMode;
}

// =====================================================================================================================
// Set the mesh shader mode
//
// @param module : Module to record in
// @param inMode : Mesh mode
void ShaderModes::setMeshShaderMode(Module &module, const MeshShaderMode &inMode) {
  PipelineState::setNamedMetadataToArrayOfInt32(&module, inMode, MeshShaderModeMetadataName);
}

// =====================================================================================================================
// Get the mesh shader mode
const MeshShaderMode &ShaderModes::getMeshShaderMode() const {
  return m_meshShaderMode;
}

// =====================================================================================================================
// Set the fragment shader mode
//
// @param module : Module to record in
// @param inMode : Fragment mode
void ShaderModes::setFragmentShaderMode(Module &module, const FragmentShaderMode &inMode) {
  PipelineState::setNamedMetadataToArrayOfInt32(&module, inMode, FragmentShaderModeMetadataName);
}

// =====================================================================================================================
// Get the fragment shader mode
const FragmentShaderMode &ShaderModes::getFragmentShaderMode() const {
  return m_fragmentShaderMode;
}

// =====================================================================================================================
// Set the compute shader mode (workgroup size)
//
// @param module : Module to record in
// @param inMode : Compute shader state
void ShaderModes::setComputeShaderMode(Module &module, const ComputeShaderMode &inMode) {
  ComputeShaderMode mode = inMode;
  // 0 is taken to be 1 in workgroup size.
  mode.workgroupSizeX = std::max(1U, mode.workgroupSizeX);
  mode.workgroupSizeY = std::max(1U, mode.workgroupSizeY);
  mode.workgroupSizeZ = std::max(1U, mode.workgroupSizeZ);
  assert(mode.workgroupSizeX <= MaxComputeWorkgroupSize && mode.workgroupSizeY <= MaxComputeWorkgroupSize &&
         mode.workgroupSizeZ <= MaxComputeWorkgroupSize);
  PipelineState::setNamedMetadataToArrayOfInt32(&module, inMode, ComputeShaderModeMetadataName);
}

// =====================================================================================================================
// Get the compute shader mode (workgroup size): static edition that reads directly from IR.
ComputeShaderMode ShaderModes::getComputeShaderMode(Module &module) {
  ComputeShaderMode mode = {};
  PipelineState::readNamedMetadataArrayOfInt32(&module, ComputeShaderModeMetadataName, mode);
  return mode;
}

// =====================================================================================================================
// Get the compute shader mode (workgroup size)
const ComputeShaderMode &ShaderModes::getComputeShaderMode() const {
  return m_computeShaderMode;
}

// =====================================================================================================================
// Set subgroup size usage. This relies on being called after setCommonShaderMode for this shader stage; calling
// setCommonShaderMode after this will lose the useSubgroupSize set here.
//
// @param module : Module to record in
// @param stage : Shader stage
// @param usage : Subgroup size usage
void ShaderModes::setSubgroupSizeUsage(Module &module, ShaderStage stage, bool usage) {
  CommonShaderMode mode = {};
  mode.useSubgroupSize = usage;
  SmallString<64> metadataName(CommonShaderModeMetadataPrefix);
  metadataName += getShaderStageAbbreviation(static_cast<ShaderStage>(stage));
  // Or the mode into any existing recorded mode, in case the front-end has already called setCommonShaderMode.
  PipelineState::orNamedMetadataToArrayOfInt32(&module, mode, metadataName);
}

// =====================================================================================================================
// Read shader modes (common and specific) from the pipeline IR module.
//
// @param module : LLVM module
void ShaderModes::readModesFromPipeline(Module *module) {
  // First the common state.
  for (unsigned stage = 0; stage < ArrayRef<CommonShaderMode>(m_commonShaderModes).size(); ++stage)
    m_commonShaderModes[stage] = getCommonShaderMode(*module, ShaderStage(stage));

  // Then the specific shader modes except tessellation.
  PipelineState::readNamedMetadataArrayOfInt32(module, GeometryShaderModeMetadataName, m_geometryShaderMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, MeshShaderModeMetadataName, m_meshShaderMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, FragmentShaderModeMetadataName, m_fragmentShaderMode);
  m_computeShaderMode = getComputeShaderMode(*module);

  // Finally the tessellation mode. We might have two of those, one from TCS and one from TES, and we
  // want to merge them and ensure defaults are correctly set.
  TessellationMode tcsMode = {};
  TessellationMode tesMode = {};
  PipelineState::readNamedMetadataArrayOfInt32(module, TcsModeMetadataName, tcsMode);
  PipelineState::readNamedMetadataArrayOfInt32(module, TesModeMetadataName, tesMode);
  m_tessellationMode.vertexSpacing = tcsMode.vertexSpacing != VertexSpacing::Unknown   ? tcsMode.vertexSpacing
                                     : tesMode.vertexSpacing != VertexSpacing::Unknown ? tesMode.vertexSpacing
                                                                                       : VertexSpacing::Equal;
  m_tessellationMode.vertexOrder = tcsMode.vertexOrder != VertexOrder::Unknown   ? tcsMode.vertexOrder
                                   : tesMode.vertexOrder != VertexOrder::Unknown ? tesMode.vertexOrder
                                                                                 : VertexOrder::Ccw;
  m_tessellationMode.primitiveMode = tcsMode.primitiveMode != PrimitiveMode::Unknown   ? tcsMode.primitiveMode
                                     : tesMode.primitiveMode != PrimitiveMode::Unknown ? tesMode.primitiveMode
                                                                                       : PrimitiveMode::Triangles;
  m_tessellationMode.pointMode = tcsMode.pointMode | tesMode.pointMode;
  m_tessellationMode.outputVertices = tcsMode.outputVertices != 0 ? tcsMode.outputVertices : tesMode.outputVertices;
  m_tessellationMode.inputVertices = tcsMode.inputVertices != 0 ? tcsMode.inputVertices : tesMode.inputVertices;
}
