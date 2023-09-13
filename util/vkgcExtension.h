/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  vkgcExtension.h
 * @brief VKGC header file: contains the list of BIL supported extensions.
 ***********************************************************************************************************************
 */

#pragma once

#include "vk_platform.h"

namespace Vkgc {
// List of name strings of supported BIL extensions
enum Extension : unsigned {
  KHR_SHADER_BALLOT,
  KHR_SUBGROUP_VOTE,
  KHR_DEVICE_GROUP,
  KHR_MULTIVIEW,
  KHR_SHADER_DRAW_PARAMETERS,
  KHR_16BIT_STORAGE,
  KHR_STORAGE_BUFFER_STORAGE_CLASS,
  KHR_8BIT_STORAGE,
  KHR_VARIABLE_POINTERS,
  KHR_FLOAT_CONTROLS,
  KHR_SHADER_CLOCK,
  KHR_VULKAN_MEMORY_MODEL,
  KHR_POST_DEPTH_COVERAGE,
  KHR_NON_SEMANTIC_INFO,
  KHR_PHYSICAL_STORAGE_BUFFER,
  KHR_TERMINATE_INVOCATION,
  KHR_FRAGMENT_SHADING_RATE,
  KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT,
  KHR_FRAGMENT_SHADER_BARYCENTRIC,
  EXT_NONUNIFORM_QUALIFIER,
  EXT_SHADER_STENCIL_EXPORT,
  EXT_SHADER_VIEWPORT_INDEX_LAYER,
  EXT_DEMOTE_TO_HELPER_INVOCATION,
  EXT_SHADER_IMAGE_ATOMIC_INT64,
  EXT_MESH_SHADER,
  AMD_SHADER_BALLOT,
  AMD_SHADER_TRINARY_MINMAX,
  AMD_SHADER_EXPLICIT_VERTEX_PARAMETER,
  AMD_GCN_SHADER,
  AMD_GPU_SHADER_HALF_FLOAT,
  AMD_TEXTURE_GATHER_BIAS_LOD,
  AMD_GPU_SHADER_INT16,
  AMD_SHADER_FRAGMENT_MASK,
  AMD_SHADER_IMAGE_LOAD_STORE_LOD,
  AMD_GPU_SHADER_HALF_FLOAT_FETCH,
  AMD_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS,
  ARB_SHADER_BALLOT,
  GOOGLE_DECORATE_STRING,
  GOOGLE_HLSL_FUNCTIONALITY1,
  GOOGLE_USER_TYPE,
  KHR_RAY_TRACING_POSITION_FETCH,
  KHR_RAY_TRACING,
  KHR_RAY_QUERY,
  NV_SHADER_ATOMIC_FLOAT,
  ExtensionCount,
};

static const unsigned MaxExtensionStringSize = 256;

// Gets extension string name from extension ID.
const char *GetExtensionName(Extension extId, char *pExtNameBuf, size_t extNameBufSize);

} // namespace Vkgc
