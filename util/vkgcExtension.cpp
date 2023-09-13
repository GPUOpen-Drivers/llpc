/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  vkgcExtension.cpp
 * @brief VKGC header file: contains the list of BIL supported extensions.
 ***********************************************************************************************************************
 */

#include "vkgcExtension.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

namespace Vkgc {
namespace Strings {
namespace Ext {
#include "g_extensions_decl.h"
#include "g_extensions_impl.h"
} // namespace Ext

} // namespace Strings

// Represents the name pair from extension ID to extension name
struct ExtensionNamePair {
  Extension extId;
  const char *pExtName;
};

#define DeclExtensionName(ExtId)                                                                                       \
  { ExtId, Strings::Ext::SPV_##ExtId##_name }

// Extension name list
const ExtensionNamePair ExtensionNameTable[ExtensionCount] = {
    DeclExtensionName(KHR_SHADER_BALLOT),
    DeclExtensionName(KHR_SUBGROUP_VOTE),
    DeclExtensionName(KHR_DEVICE_GROUP),
    DeclExtensionName(KHR_MULTIVIEW),
    DeclExtensionName(KHR_SHADER_DRAW_PARAMETERS),
    DeclExtensionName(KHR_16BIT_STORAGE),
    DeclExtensionName(KHR_STORAGE_BUFFER_STORAGE_CLASS),
    DeclExtensionName(KHR_8BIT_STORAGE),
    DeclExtensionName(KHR_VARIABLE_POINTERS),
    DeclExtensionName(KHR_FLOAT_CONTROLS),
    DeclExtensionName(KHR_SHADER_CLOCK),
    DeclExtensionName(KHR_VULKAN_MEMORY_MODEL),
    DeclExtensionName(KHR_POST_DEPTH_COVERAGE),
    DeclExtensionName(KHR_NON_SEMANTIC_INFO),
    DeclExtensionName(KHR_PHYSICAL_STORAGE_BUFFER),
    DeclExtensionName(KHR_TERMINATE_INVOCATION),
    DeclExtensionName(KHR_FRAGMENT_SHADING_RATE),
    DeclExtensionName(KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT),
    DeclExtensionName(KHR_FRAGMENT_SHADER_BARYCENTRIC),
    DeclExtensionName(EXT_NONUNIFORM_QUALIFIER),
    DeclExtensionName(EXT_SHADER_STENCIL_EXPORT),
    DeclExtensionName(EXT_SHADER_VIEWPORT_INDEX_LAYER),
    DeclExtensionName(EXT_DEMOTE_TO_HELPER_INVOCATION),
    DeclExtensionName(EXT_SHADER_IMAGE_ATOMIC_INT64),
    DeclExtensionName(EXT_MESH_SHADER),
    DeclExtensionName(AMD_SHADER_BALLOT),
    DeclExtensionName(AMD_SHADER_TRINARY_MINMAX),
    DeclExtensionName(AMD_SHADER_EXPLICIT_VERTEX_PARAMETER),
    DeclExtensionName(AMD_GCN_SHADER),
    DeclExtensionName(AMD_GPU_SHADER_HALF_FLOAT),
    DeclExtensionName(AMD_TEXTURE_GATHER_BIAS_LOD),
    DeclExtensionName(AMD_GPU_SHADER_INT16),
    DeclExtensionName(AMD_SHADER_FRAGMENT_MASK),
    DeclExtensionName(AMD_SHADER_IMAGE_LOAD_STORE_LOD),
    DeclExtensionName(AMD_GPU_SHADER_HALF_FLOAT_FETCH),
    DeclExtensionName(AMD_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS),
    DeclExtensionName(ARB_SHADER_BALLOT),
    DeclExtensionName(GOOGLE_DECORATE_STRING),
    DeclExtensionName(GOOGLE_HLSL_FUNCTIONALITY1),
    DeclExtensionName(GOOGLE_USER_TYPE),
    DeclExtensionName(KHR_RAY_TRACING_POSITION_FETCH),
    DeclExtensionName(KHR_RAY_TRACING),
    DeclExtensionName(KHR_RAY_QUERY),
    DeclExtensionName(NV_SHADER_ATOMIC_FLOAT),
};

// =====================================================================================================================
// Gets extension string name from extension ID.
const char *GetExtensionName(Extension extId,       // Extension ID
                             char *pExtNameBuf,     // [out] Name buffer of the extension for string decoding
                             size_t extNameBufSize) // Size of name buffer
{
  assert(ExtensionNameTable[extId].extId == extId);
  size_t length = strlen(ExtensionNameTable[extId].pExtName);
  memcpy(pExtNameBuf, ExtensionNameTable[extId].pExtName, length);
  return pExtNameBuf;
}

} // namespace Vkgc
