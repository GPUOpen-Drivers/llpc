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
 * @file  vkgcBase.h
 * @brief Minimal subset of the vkgc interface that avoids including vulkan.h
 ***********************************************************************************************************************
 */

#pragma once

#include <cstddef>
#include <tuple>

namespace Vkgc {

/// Represents graphics IP version info. See https://llvm.org/docs/AMDGPUUsage.html#processors for more
/// details.
struct GfxIpVersion {
  unsigned major;    ///< Major version
  unsigned minor;    ///< Minor version
  unsigned stepping; ///< Stepping info

  // GFX IP checkers
  bool operator==(const GfxIpVersion &rhs) const {
    return std::tie(major, minor, stepping) == std::tie(rhs.major, rhs.minor, rhs.stepping);
  }
  bool operator>=(const GfxIpVersion &rhs) const {
    return std::tie(major, minor, stepping) >= std::tie(rhs.major, rhs.minor, rhs.stepping);
  }
  bool isGfx(unsigned rhsMajor, unsigned rhsMinor) const {
    return std::tie(major, minor) == std::tie(rhsMajor, rhsMinor);
  }
};

/// Represents RT IP version
struct RtIpVersion {
  unsigned major; ///< Major version
  unsigned minor; ///< Minor version

  // RT IP checkers
  bool operator==(const RtIpVersion &rhs) const { return std::tie(major, minor) == std::tie(rhs.major, rhs.minor); }
  bool operator>=(const RtIpVersion &rhs) const { return std::tie(major, minor) >= std::tie(rhs.major, rhs.minor); }
  bool isRtIp(unsigned rhsMajor, unsigned rhsMinor) const {
    return std::tie(major, minor) == std::tie(rhsMajor, rhsMinor);
  }
};

// =====================================================================================================================
// Raytracing entry function indices
enum RAYTRACING_ENTRY_FUNC : unsigned {
  RT_ENTRY_TRACE_RAY,
  RT_ENTRY_TRACE_RAY_INLINE,
  RT_ENTRY_TRACE_RAY_HIT_TOKEN,
  RT_ENTRY_RAY_QUERY_PROCEED,
  RT_ENTRY_INSTANCE_INDEX,
  RT_ENTRY_INSTANCE_ID,
  RT_ENTRY_OBJECT_TO_WORLD_TRANSFORM,
  RT_ENTRY_WORLD_TO_OBJECT_TRANSFORM,
  RT_ENTRY_GET_INSTANCE_NODE,
  RT_ENTRY_RESERVE1,
  RT_ENTRY_RESERVE2,
  RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_NODE_POINTER,
  RT_ENTRY_FETCH_HIT_TRIANGLE_FROM_RAY_QUERY,
  RT_ENTRY_FUNC_COUNT,
};

/// Represents GPURT function table
struct GpurtFuncTable {
  static constexpr size_t MaxFunctionNameLength = 255;

  char pFunc[RT_ENTRY_FUNC_COUNT][MaxFunctionNameLength + 1]; ///< Function names
};

} // namespace Vkgc
