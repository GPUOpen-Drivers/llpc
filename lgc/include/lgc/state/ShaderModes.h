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
// Shader modes from input language. This class is used as follows:
//
// * Front-end passes call the set*Mode methods in Builder, which forward to static methods here. Those methods write
//   directly into IR metadata.
//
// * There are also a few occurrences of front-end passes needing to get that data back again. They call get*Mode
//   methods in Builder, which forward to static methods here. Those methods read directly out of IR metadata.
//
// * Middle-end passes have a ShaderModes object owned by PipelineState, which is created at the start of the
//   LGC pass flow. That ShaderModes object reads the IR metadata once, and provides non-static get*Mode methods
//   to return mode data.
class ShaderModes {
public:
  // Set the common shader mode (FP modes) for the given shader stage
  static void setCommonShaderMode(llvm::Module &module, ShaderStage stage, const CommonShaderMode &commonShaderMode);

  // Get the common shader modes for the given shader stage: static edition that reads directly from IR.
  static CommonShaderMode getCommonShaderMode(llvm::Module &module, ShaderStage stage);

  // Get the common shader modes for the given shader stage
  const CommonShaderMode &getCommonShaderMode(ShaderStage stage) const;

  // Check if any shader stage has useSubgroupSize set
  bool getAnyUseSubgroupSize() const;

  // Set the tessellation mode for the given shader stage (TCS or TES).
  static void setTessellationMode(llvm::Module &module, ShaderStage stage, const TessellationMode &inMode);

  // Get the tessellation mode for the given shader stage (TCS or TES): static edition that reads directly from IR.
  static TessellationMode getTessellationMode(llvm::Module &module, ShaderStage shaderStage);

  // Get the tessellation state.
  const TessellationMode &getTessellationMode() const;

  // Set the geometry shader mode
  static void setGeometryShaderMode(llvm::Module &module, const GeometryShaderMode &inMode);

  // Get the geometry shader mode
  const GeometryShaderMode &getGeometryShaderMode() const;

  // Set the mesh shader mode
  static void setMeshShaderMode(llvm::Module &module, const MeshShaderMode &inMode);

  // Get the mesh shader mode
  const MeshShaderMode &getMeshShaderMode() const;

  // Set the fragment shader mode
  static void setFragmentShaderMode(llvm::Module &module, const FragmentShaderMode &inMode);

  // Get the fragment shader mode
  const FragmentShaderMode &getFragmentShaderMode() const;

  // Set the compute shader mode (workgroup size)
  static void setComputeShaderMode(llvm::Module &module, const ComputeShaderMode &inMode);

  // Get the compute shader mode (workgroup size): static edition that reads directly from IR.
  static ComputeShaderMode getComputeShaderMode(llvm::Module &module);

  // Get the compute shader mode (workgroup size)
  const ComputeShaderMode &getComputeShaderMode() const;

  // Set subgroup size usage.
  static void setSubgroupSizeUsage(llvm::Module &module, ShaderStage stage, bool usage);

  // Clear all modes
  void clear();

  // Read shader modes from IR metadata in a pipeline
  void readModesFromPipeline(llvm::Module *module);

private:
  CommonShaderMode m_commonShaderModes[ShaderStageCompute + 1] = {}; // Per-shader FP modes
  TessellationMode m_tessellationMode = {};                          // Tessellation mode
  GeometryShaderMode m_geometryShaderMode = {};                      // Geometry shader mode
  MeshShaderMode m_meshShaderMode = {};                              // Mesh shader mode
  FragmentShaderMode m_fragmentShaderMode = {};                      // Fragment shader mode
  ComputeShaderMode m_computeShaderMode = {};                        // Compute shader mode (workgroup size)
};

} // namespace lgc
