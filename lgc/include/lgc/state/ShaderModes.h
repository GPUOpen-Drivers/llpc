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
 * @file  PipelineState.h
 * @brief LLPC header file: contains declaration of class lgc::PipelineState
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"

namespace lgc {

// =====================================================================================================================
// Shader modes from input language. The front-end calls Set*Mode methods in Builder, which forward to here.
// The middle-end gets these modes by calling PipelineState::GetShaderModes then calling a Get*Mode method here.
// The structs for the modes are declared in Pipeline.h.
class ShaderModes {
public:
  // Set the common shader mode (FP modes) for the given shader stage
  void setCommonShaderMode(ShaderStage stage, const CommonShaderMode &commonShaderMode);

  // Get the common shader modes for the given shader stage
  const CommonShaderMode &getCommonShaderMode(ShaderStage stage);

  // Check if any shader stage has useSubgroupSize set
  bool getAnyUseSubgroupSize();

  // Set the tessellation mode. This in fact merges the supplied values with any previously supplied values,
  // to allow the client to call this twice, once for TCS and once for TES.
  void setTessellationMode(const TessellationMode &inMode);

  // Get the tessellation state.
  const TessellationMode &getTessellationMode();

  // Set the geometry shader mode
  void setGeometryShaderMode(const GeometryShaderMode &inMode);

  // Get the geometry shader mode
  const GeometryShaderMode &getGeometryShaderMode();

  // Set the mesh shader mode
  void setMeshShaderMode(const MeshShaderMode &inMode);

  // Get the mesh shader mode
  const MeshShaderMode &getMeshShaderMode();

  // Set the fragment shader mode
  void setFragmentShaderMode(const FragmentShaderMode &inMode);

  // Get the fragment shader mode
  const FragmentShaderMode &getFragmentShaderMode();

  // Set the compute shader mode (workgroup size)
  void setComputeShaderMode(const ComputeShaderMode &inMode);

  // Get the compute shader mode (workgroup size)
  const ComputeShaderMode &getComputeShaderMode();

  // Set subgroup size usage
  void setSubgroupSizeUsage(ShaderStage stage, bool usage);

  // Clear all modes
  void clear();

  // Record modes to IR metadata
  void record(llvm::Module *module);

  // Read shader modes (common and specific) from a shader IR module, but only if no modes have been set
  // in this ShaderModes. This is used to handle the case that the shader module comes from an earlier
  // shader compile, and it had its ShaderModes recorded into IR then.
  void readModesFromShader(llvm::Module *module, ShaderStage stage);

  // Read shader modes from IR metadata in a pipeline
  void readModesFromPipeline(llvm::Module *module);

private:
  bool m_anySet = false;                                             // Whether any Set*Mode method called
  CommonShaderMode m_commonShaderModes[ShaderStageCompute + 1] = {}; // Per-shader FP modes
  TessellationMode m_tessellationMode = {};                          // Tessellation mode
  GeometryShaderMode m_geometryShaderMode = {};                      // Geometry shader mode
  MeshShaderMode m_meshShaderMode = {};                              // Mesh shader mode
  FragmentShaderMode m_fragmentShaderMode = {};                      // Fragment shader mode
  ComputeShaderMode m_computeShaderMode = {};                        // Compute shader mode (workgroup size)
};

} // namespace lgc
