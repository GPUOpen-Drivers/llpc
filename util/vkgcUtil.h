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
 * @file  vkgcUtil.h
 * @brief VKGC header file: contains the definition of VKGC internal types and utility functions
 ***********************************************************************************************************************
 */
#pragma once

#include "vkgcDefs.h"

namespace Vkgc {

// Represents the special header of SPIR-V token stream (the first dword).
struct SpirvHeader {
  unsigned magicNumber;    // Magic number of SPIR-V module
  unsigned spvVersion;     // SPIR-V version number
  unsigned genMagicNumber; // Generator's magic number
  unsigned idBound;        // Upbound (X) of all IDs used in SPIR-V (0 < ID < X)
  unsigned reserved;       // Reserved word
};

// Invalid value
static const unsigned InvalidValue = ~0u;

// Gets name string of the abbreviation for the specified shader stage.
const char *getShaderStageAbbreviation(ShaderStage shaderStage, bool upper = false);

// Create directory.
bool createDirectory(const char *dir);

// Translate enum "ResourceMappingNodeType" to string
const char *getResourceMappingNodeTypeName(ResourceMappingNodeType type);

// Checks whether input binary data is SPIR-V binary
bool isSpirvBinary(const BinaryData *shaderBin);

// Gets the entry-point name from the SPIR-V binary
const char *getEntryPointNameFromSpirvBinary(const BinaryData *spvBin);

// =====================================================================================================================
// Increments a pointer by nBytes by first casting it to a uint8_t*.
//
// Returns incremented pointer.
//
// @param p : Pointer to be incremented.
// @param numBytes : Number of bytes to increment the pointer by
inline void *voidPtrInc(const void *p, size_t numBytes) {
  void *ptr = const_cast<void *>(p);
  return (static_cast<uint8_t *>(ptr) + numBytes);
}

// ===================================================================================
// Finds the expected structure in Vulkan structure chain with the specified info.
template <class T>
//
// @param type : Vulkan structure type
// @param next : Base pointer of Vulkan structure
inline const T *findVkStructInChain(VkStructureType type, const void *next) {
  struct VkStructHeader {
    VkStructureType type;
    VkStructHeader *next;
  };

  const VkStructHeader *structHeader = reinterpret_cast<const VkStructHeader *>(next);
  while (structHeader) {
    if (structHeader->type == type)
      break;
    else
      structHeader = structHeader->next;
  }

  return reinterpret_cast<const T *>(structHeader);
}

// Translates shader stage to corresponding stage mask.
inline unsigned shaderStageToMask(ShaderStage stage) {
  assert(stage < ShaderStageCount || stage == ShaderStageCopyShader);
  return 1U << static_cast<unsigned>(stage);
}

} // namespace Vkgc
