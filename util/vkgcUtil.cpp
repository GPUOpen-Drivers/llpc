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
 * @file  vkgcUtil.cpp
 * @brief VKGC source file: contains implementation of VKGC internal types and utility functions
 ***********************************************************************************************************************
 */
#include "vkgcUtil.h"
#include "spirv.hpp"
#include "vkgcElfReader.h"
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define DEBUG_TYPE "vkgc-util"

using namespace spv;

namespace Vkgc {

// =====================================================================================================================
// Gets the entry-point name from the SPIR-V binary.
//
// @param [in] spvBin : SPIR-V binary
const char *VKAPI_CALL IUtil::GetEntryPointNameFromSpirvBinary(const BinaryData *spvBin) {
  return getEntryPointNameFromSpirvBinary(spvBin);
}

// =====================================================================================================================
// Gets name string of the abbreviation for the specified shader stage
//
// @param shaderStage : Shader stage
// @param upper : Whether to use uppercase for the abbreviation (default is lowercase)
const char *getShaderStageAbbreviation(ShaderStage shaderStage, bool upper) {
  const char *abbr = nullptr;

  if (shaderStage == ShaderStageCopyShader)
    abbr = upper ? "COPY" : "Copy";
  else if (shaderStage < ShaderStageCount) {
    if (upper) {
      static const char *ShaderStageAbbrs[] = {
        "TASK",
        "VS",
        "TCS",
        "TES",
        "GS",
        "MESH",
        "FS",
        "CS",
#if VKI_RAY_TRACING
        "RGEN",
        "SECT",
        "AHIT",
        "CHIT",
        "MISS",
        "CALL"
#endif
      };

      abbr = ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
    } else {
      static const char *ShaderStageAbbrs[] = {
        "Task",
        "Vs",
        "Tcs",
        "Tes",
        "Gs",
        "Mesh",
        "Fs",
        "Cs",
#if VKI_RAY_TRACING
        "rgen",
        "sect",
        "ahit",
        "chit",
        "miss",
        "call"
#endif
      };

      abbr = ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
    }
  } else
    abbr = "Bad";

  return abbr;
}

// =====================================================================================================================
// Create directory.
//
// @param dir : The path of directory
bool createDirectory(const char *dir) {
#if defined(_WIN32)
  int result = _mkdir(dir);
  return (result == 0);
#else
  int result = mkdir(dir, S_IRWXU);
  return result == 0;
#endif
}

// =====================================================================================================================
// Helper macro
#define CASE_CLASSENUM_TO_STRING(TYPE, ENUM)                                                                           \
  case TYPE::ENUM:                                                                                                     \
    string = #ENUM;                                                                                                    \
    break;

// =====================================================================================================================
// Translate enum "ResourceMappingNodeType" to string
//
// @param type : Resource map node type
const char *getResourceMappingNodeTypeName(ResourceMappingNodeType type) {
  const char *string = nullptr;
  switch (type) {
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, Unknown)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorResource)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorSampler)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorYCbCrSampler)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorCombinedTexture)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorTexelBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorFmask)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorConstBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorConstBufferCompact)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorImage)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorConstTexelBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorTableVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, IndirectUserDataVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, PushConst)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorBufferCompact)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, StreamOutTableVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, InlineBuffer)
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorMutable)
#endif
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return string;
}

// =====================================================================================================================
// Checks whether input binary data is SPIR-V binary
//
// @param shaderBin : Shader binary codes
bool isSpirvBinary(const BinaryData *shaderBin) {
  bool isSpvBinary = false;
  if (shaderBin->codeSize > sizeof(SpirvHeader)) {
    const SpirvHeader *header = reinterpret_cast<const SpirvHeader *>(shaderBin->pCode);
    if (header->magicNumber == MagicNumber && header->spvVersion <= spv::Version && header->reserved == 0)
      isSpvBinary = true;
  }

  return isSpvBinary;
}

// =====================================================================================================================
// Gets the entry-point name from the SPIR-V binary
//
// NOTE: This function is for single entry-point. If the SPIR-V binary contains multiple entry-points, we get the name
// of the first entry-point and ignore others.
//
// @param spvBin : SPIR-V binary
const char *getEntryPointNameFromSpirvBinary(const BinaryData *spvBin) {
  const char *entryName = nullptr;

  const unsigned *code = reinterpret_cast<const unsigned *>(spvBin->pCode);
  const unsigned *end = code + spvBin->codeSize / sizeof(unsigned);

  if (isSpirvBinary(spvBin)) {
    // Skip SPIR-V header
    const unsigned *codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

    while (codePos < end) {
      unsigned opCode = (codePos[0] & OpCodeMask);
      unsigned wordCount = (codePos[0] >> WordCountShift);

      if (wordCount == 0 || codePos + wordCount > end) {
        assert("Invalid SPIR-V binary\n");
        break;
      }

      if (opCode == OpEntryPoint) {
        assert(wordCount >= 4);

        // The fourth word is start of the name string of the entry-point
        entryName = reinterpret_cast<const char *>(&codePos[3]);
        break;
      }

      // All "OpEntryPoint" are before "OpFunction"
      if (opCode == OpFunction)
        break;

      codePos += wordCount;
    }

    if (!entryName) {
      assert("Entry-point not found\n");
      entryName = "";
    }
  } else {
    assert("Invalid SPIR-V binary\n");
    entryName = "";
  }

  return entryName;
}

} // namespace Vkgc
