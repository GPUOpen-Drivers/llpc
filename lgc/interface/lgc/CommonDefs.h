/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "EnumIterator.h"
#include <array>
#include <cstdint>

namespace lgc {

// Type used to hold a 128-bit hash value in LGC and LLPC.
using Hash128 = std::array<uint64_t, 2>;

/// Enumerates LGC shader stages.
enum ShaderStage : unsigned {
  ShaderStageTask = 0,                                  ///< Task shader
  ShaderStageVertex,                                    ///< Vertex shader
  ShaderStageTessControl,                               ///< Tessellation control shader
  ShaderStageTessEval,                                  ///< Tessellation evaluation shader
  ShaderStageGeometry,                                  ///< Geometry shader
  ShaderStageMesh,                                      ///< Mesh shader
  ShaderStageFragment,                                  ///< Fragment shader
  ShaderStageCompute,                                   ///< Compute shader
  ShaderStageCount,                                     ///< Count of shader stages
  ShaderStageInvalid = ~0u,                             ///< Invalid shader stage
  ShaderStageNativeStageCount = ShaderStageCompute + 1, ///< Native supported shader stage count
  ShaderStageGfxCount = ShaderStageFragment + 1,        ///< Count of shader stages for graphics pipeline

  ShaderStageCopyShader = ShaderStageCount, ///< Copy shader (internal-use)
  ShaderStageCountInternal,                 ///< Count of shader stages (internal-use)
};

} // namespace lgc
namespace llvm {
// Enable iteration over shader stages with `lgc::enumRange<lgc::ShaderStage>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(lgc::ShaderStage, lgc::ShaderStage::ShaderStageCountInternal);
} // namespace llvm

namespace lgc {
// Enumerates the function of a particular node in a shader's resource mapping graph. Also used as descriptor
// type in Builder descriptor functions.
enum class ResourceNodeType : unsigned {
  Unknown = 0,               ///< Invalid type
  DescriptorResource,        ///< Generic descriptor: resource, including texture resource, image, input
                             ///  attachment
  DescriptorSampler,         ///< Generic descriptor: sampler
  DescriptorCombinedTexture, ///< Generic descriptor: combined texture, combining resource descriptor with
                             ///  sampler descriptor of the same texture, starting with resource descriptor
  DescriptorTexelBuffer,     ///< Generic descriptor: texel buffer, including texture buffer and image buffer
  DescriptorFmask,           ///< Generic descriptor: F-mask
  DescriptorBuffer,          ///< Generic descriptor: buffer, including uniform buffer and shader storage buffer
  DescriptorTableVaPtr,      ///< Descriptor table VA pointer
  IndirectUserDataVaPtr,     ///< Indirect user data VA pointer
  PushConst,                 ///< Push constant; only a single PushConst in the root table is allowed
  DescriptorBufferCompact,   ///< Compact buffer descriptor, only contains the buffer address
  StreamOutTableVaPtr,       ///< Stream-out buffer table VA pointer
  DescriptorReserved12,
  DescriptorReserved13,
  InlineBuffer,                 ///< Inline buffer, with descriptor set and binding
  DescriptorConstBuffer,        ///< Generic descriptor: constant buffer
  DescriptorConstBufferCompact, ///< Compact buffer descriptor, only contains the buffer address
  DescriptorMutable,            ///< Mutable descriptor type
  Count,                        ///< Count of resource mapping node types.
};

// Represents mapping layout of the resources used in shaders
enum class ResourceLayoutScheme : unsigned {
  Compact = 0, ///< Compact scheme make full use of all the user data registers.
  Indirect     ///< Fixed layout, push constant will be the sub node of DescriptorTableVaPtr
};
} // namespace lgc

namespace llvm {
// Enable iteration over resource node type with `lgc::enumRange<ResourceNodeType>()`.
LGC_DEFINE_DEFAULT_ITERABLE_ENUM(lgc::ResourceNodeType);

} // namespace llvm
