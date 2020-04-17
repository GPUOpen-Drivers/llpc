/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  CommonDefs.h
 * @brief LLPC header file: contains common interface types in the LGC interface
 ***********************************************************************************************************************
 */
#pragma once

namespace lgc {

/// Enumerates LGC shader stages.
enum ShaderStage : unsigned {
  ShaderStageVertex = 0,                                ///< Vertex shader
  ShaderStageTessControl,                               ///< Tessellation control shader
  ShaderStageTessEval,                                  ///< Tessellation evaluation shader
  ShaderStageGeometry,                                  ///< Geometry shader
  ShaderStageFragment,                                  ///< Fragment shader
  ShaderStageCompute,                                   ///< Compute shader
  ShaderStageFetch,                                     ///< Fetch shader for vertex inputs
  ShaderStageCount,                                     ///< Count of shader stages
  ShaderStageInvalid = ~0u,                             ///< Invalid shader stage
  ShaderStageNativeStageCount = ShaderStageCompute + 1, ///< Native supported shader stage count
  ShaderStageGfxCount = ShaderStageFragment + 1,        ///< Count of shader stages for graphics pipeline

  ShaderStageCopyShader = ShaderStageCount, ///< Copy shader (internal-use)
  ShaderStageCountInternal,                 ///< Count of shader stages (internal-use)
};

} // namespace lgc
