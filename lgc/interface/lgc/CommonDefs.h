/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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
#include "llvm/ADT/DenseMap.h"
#include <array>
#include <cstdint>

namespace lgc {

// Type used to hold a 128-bit hash value in LGC and LLPC.
using Hash128 = std::array<uint64_t, 2>;

/// Enumerates LGC shader stages.
namespace ShaderStage {
enum ShaderStage : unsigned {
  Task = 0,                       ///< Task shader
  Vertex,                         ///< Vertex shader
  TessControl,                    ///< Tessellation control shader
  TessEval,                       ///< Tessellation evaluation shader
  Geometry,                       ///< Geometry shader
  Mesh,                           ///< Mesh shader
  Fragment,                       ///< Fragment shader
  Compute,                        ///< Compute shader
  Count,                          ///< Count of shader stages
  Invalid = ~0u,                  ///< Invalid shader stage
  NativeStageCount = Compute + 1, ///< Native supported shader stage count
  GfxCount = Fragment + 1,        ///< Count of shader stages for graphics pipeline

  CopyShader = Count, ///< Copy shader (internal-use)
  CountInternal,      ///< Count of shader stages (internal-use)
};
} // namespace ShaderStage

// TODO Temporary definition until ShaderStage is converted to a class enum.
using ShaderStageEnum = ShaderStage::ShaderStage;

/// All shader stages
[[maybe_unused]] constexpr const std::array ShaderStages = {
    ShaderStage::Compute,  ShaderStage::Fragment,    ShaderStage::Vertex,
    ShaderStage::Geometry, ShaderStage::TessControl, ShaderStage::TessEval,
    ShaderStage::Task,     ShaderStage::Mesh,        ShaderStage::CopyShader,
};

/// All graphics shader stages.
/// These are in execution order.
[[maybe_unused]] constexpr const std::array ShaderStagesGraphics = {
    ShaderStage::Task,     ShaderStage::Vertex, ShaderStage::TessControl, ShaderStage::TessEval,
    ShaderStage::Geometry, ShaderStage::Mesh,   ShaderStage::Fragment,
};

/// Graphics and compute shader stages.
/// The graphics stages are in execution order.
[[maybe_unused]] constexpr const std::array ShaderStagesNative = {
    ShaderStage::Task,     ShaderStage::Vertex, ShaderStage::TessControl, ShaderStage::TessEval,
    ShaderStage::Geometry, ShaderStage::Mesh,   ShaderStage::Fragment,    ShaderStage::Compute,
};

/// Graphics and compute shader stages and copy shader.
/// The graphics stages are in execution order.
[[maybe_unused]] constexpr const std::array ShaderStagesNativeCopy = {
    ShaderStage::Task, ShaderStage::Vertex,   ShaderStage::TessControl, ShaderStage::TessEval,   ShaderStage::Geometry,
    ShaderStage::Mesh, ShaderStage::Fragment, ShaderStage::Compute,     ShaderStage::CopyShader,
};

class ShaderStageMask {
public:
  constexpr ShaderStageMask() {}

  constexpr explicit ShaderStageMask(ShaderStageEnum stage) {
    assert(static_cast<uint32_t>(stage) < 32 && "ShaderStage mask overflowed");
    m_value = 1U << static_cast<uint32_t>(stage);
  }

  constexpr explicit ShaderStageMask(std::initializer_list<ShaderStageEnum> stages) {
    for (auto stage : stages)
      *this |= ShaderStageMask(stage);
  }

  template <size_t N> constexpr explicit ShaderStageMask(const std::array<ShaderStageEnum, N> &stages) {
    for (auto stage : stages)
      *this |= ShaderStageMask(stage);
  }

  constexpr static ShaderStageMask fromRaw(uint32_t mask) {
    ShaderStageMask result;
    result.m_value = mask;
    return result;
  }
  constexpr uint32_t toRaw() const { return m_value; }

  constexpr bool operator==(const ShaderStageMask &other) const { return m_value == other.m_value; }

  constexpr bool operator!=(const ShaderStageMask &other) const { return !(*this == other); }

  constexpr ShaderStageMask &operator|=(const ShaderStageMask &other);
  constexpr ShaderStageMask &operator&=(const ShaderStageMask &other);
  constexpr ShaderStageMask operator~() const {
    ShaderStageMask result;
    result.m_value = ~m_value;
    return result;
  }

  constexpr bool contains(ShaderStageEnum stage) const;
  constexpr bool contains_any(std::initializer_list<ShaderStageEnum> stages) const;
  template <size_t N> constexpr bool contains_any(const std::array<ShaderStageEnum, N> &stages) const;
  constexpr bool empty() const { return m_value == 0; }

  uint32_t m_value = 0;
};

constexpr ShaderStageMask operator|(const ShaderStageMask &lhs, const ShaderStageMask &rhs) {
  ShaderStageMask result;
  result.m_value = lhs.m_value | rhs.m_value;
  return result;
}

constexpr ShaderStageMask operator&(const ShaderStageMask &lhs, const ShaderStageMask &rhs) {
  ShaderStageMask result;
  result.m_value = lhs.m_value & rhs.m_value;
  return result;
}

constexpr ShaderStageMask &ShaderStageMask::operator|=(const ShaderStageMask &other) {
  *this = *this | other;
  return *this;
}

constexpr ShaderStageMask &ShaderStageMask::operator&=(const ShaderStageMask &other) {
  *this = *this & other;
  return *this;
}

constexpr bool ShaderStageMask::contains(ShaderStageEnum stage) const {
  return (*this & ShaderStageMask(stage)).m_value != 0;
}

constexpr bool ShaderStageMask::contains_any(std::initializer_list<ShaderStageEnum> stages) const {
  return (*this & ShaderStageMask(stages)).m_value != 0;
}

template <size_t N> constexpr bool ShaderStageMask::contains_any(const std::array<ShaderStageEnum, N> &stages) const {
  return (*this & ShaderStageMask(stages)).m_value != 0;
}

enum AddrSpace {
  ADDR_SPACE_FLAT = 0,                   // Flat memory
  ADDR_SPACE_GLOBAL = 1,                 // Global memory
  ADDR_SPACE_REGION = 2,                 // GDS memory
  ADDR_SPACE_LOCAL = 3,                  // Local memory
  ADDR_SPACE_CONST = 4,                  // Constant memory
  ADDR_SPACE_PRIVATE = 5,                // Private memory
  ADDR_SPACE_CONST_32BIT = 6,            // Constant 32-bit memory
  ADDR_SPACE_BUFFER_FAT_POINTER = 7,     // Buffer fat-pointer memory
  ADDR_SPACE_BUFFER_STRIDED_POINTER = 9, // Strided Buffer pointer memory
  ADDR_SPACE_MAX = ADDR_SPACE_BUFFER_STRIDED_POINTER
};

// Max number of threads per subgroup in NGG mode.
constexpr unsigned NggMaxThreadsPerSubgroup = 256;

// Max number of GS primitive amplifier defined by GE_NGG_SUBGRP_CNTL.PRIM_AMP_FACTOR.
// NOTE: There are 9 bits that program the register field to launch 511 threads at most though it is not
// documented in HW spec. HW spec says the maximum value is 256 and this value might be limited by rasterization.
// In experiments, we find it is able to launch 511 threads.
constexpr unsigned NggMaxPrimitiveAmplifier = 511;

constexpr unsigned EsVertsOffchipGsOrTess = 250;
constexpr unsigned GsPrimsOffchipGsOrTess = 126;

} // namespace lgc
namespace llvm {
// Enable iteration over shader stages with `lgc::enumRange<lgc::ShaderStageEnum>()`.
LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(lgc::ShaderStageEnum, lgc::ShaderStage::CountInternal);
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

template <> struct DenseMapInfo<lgc::ShaderStageEnum> {
  using T = lgc::ShaderStageEnum;

  static T getEmptyKey() { return static_cast<T>(DenseMapInfo<uint32_t>::getEmptyKey()); }
  static T getTombstoneKey() { return static_cast<T>(DenseMapInfo<uint32_t>::getTombstoneKey()); }
  static unsigned getHashValue(const T &Val) { return static_cast<unsigned>(Val); }
  static bool isEqual(const T &LHS, const T &RHS) { return LHS == RHS; }
};

} // namespace llvm
