/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  vkgcUtil.cpp
 * @brief VKGC source file: contains implementation of VKGC internal types and utility functions
 ***********************************************************************************************************************
 */
#include "vkgcUtil.h"
#include "spirv.hpp"
#include "vkgcElfReader.h"
#include "lgc/Disassembler.h"
#include <filesystem>
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
// Translate enum "ResourceMappingNodeType" to string
//
// @param type : Resource map node type
const char *VKAPI_CALL IUtil::GetResourceMappingNodeTypeName(ResourceMappingNodeType type) {
  return getResourceMappingNodeTypeName(type);
}

// =====================================================================================================================
// Disassembles a symbol from an ELF object
// If pOutDisassembly is null, only the size of the disassembly is written to pDisassemblySize
// Returns Result::Success if the operation completed successfully, all other results mean the operation was
// semantically a no-op.
//
// @param [in] pElfObj : ELF object data
// @param [in] objSize : size of ELF object data
// @param [in] pSymbolName : symbol to disassemble
// @param [out] pDisassemblySize : size of disassembled code
// @param [out] pOutDisassembly : disassembled code
// @returns : Success code, possible values:
//   * Success: operation completed successfully
//   * ErrorInvalidPointer: pElfObj is nullptr
//   * ErrorInvalidShader: pElfObj could not be decoded
//   * NotFound: pSymbolName not found in pElfObj
//   * ErrorUnknown: other error occurred during disassembly
Result VKAPI_CALL IUtil::GetSymbolDisassemblyFromElf(const void *pElfObj, const size_t objSize, const char *pSymbolName,
                                                     size_t *pDisassemblySize, void *pOutDisassembly) {
  if (pElfObj != nullptr) {
    llvm::MemoryBufferRef memBufRef =
        llvm::MemoryBufferRef(llvm::StringRef(static_cast<const char *>(pElfObj), objSize), "ElfObj");
    std::string disassembly;
    llvm::raw_string_ostream ostream(disassembly);

    llvm::Error err = lgc::disassembleSingleSymbol(memBufRef, ostream, pSymbolName);

    if (!err) {
      if (pDisassemblySize != nullptr) {
        *pDisassemblySize = disassembly.length() + 1;
      }

      if (pOutDisassembly != nullptr) {
        memcpy(pOutDisassembly, disassembly.c_str(), disassembly.length() + 1);
      }

      return Result::Success;
    } else {
      std::string errMsg;
      llvm::raw_string_ostream errStream(errMsg);
      llvm::logAllUnhandledErrors(std::move(err), errStream);

      if (errMsg.find("ELF object file") != std::string::npos) {
        // Could not decode pElfObj
        return Result::ErrorInvalidShader;
      }

      if (errMsg.find("Symbol not found") != std::string::npos) {
        // pSymbolName not found in pElfObj
        return Result::NotFound;
      }

      return Result::ErrorUnknown;
    }
  }

  return Result::ErrorInvalidPointer;
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
      static const char *ShaderStageAbbrs[] = {"TASK", "VS",   "TCS",  "TES",  "GS",   "MESH", "FS",
                                               "CS",   "RGEN", "SECT", "AHIT", "CHIT", "MISS", "CALL"};

      abbr = ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
    } else {
      static const char *ShaderStageAbbrs[] = {"Task", "Vs",   "Tcs",  "Tes",  "Gs",   "Mesh", "Fs",
                                               "Cs",   "rgen", "sect", "ahit", "chit", "miss", "call"};

      abbr = ShaderStageAbbrs[static_cast<unsigned>(shaderStage)];
    }
  } else
    abbr = "Bad";

  return abbr;
}

// =====================================================================================================================
// Wrapper around thread-safe versions of strtok
static char *safeStrtok(char *s, const char *delim, char **savePtr) {
  // strtok* and friends are a mess:
  // * strtok is not thread-safe in standard C
  // * POSIX defined thread-safe strtok_r long ago
  // * Microsoft added strtok_s with the same signature and semantics but different name
  // * C11 added strtok_s with a different signature from Microsoft's strtok_s
#if defined(_WIN32)
  return strtok_s(s, delim, savePtr);
#else
  return strtok_r(s, delim, savePtr);
#endif
}

// =====================================================================================================================
// Create directory recursively.
//
// @param dir : The path of directory
bool createDirectory(const char *dir) {
  namespace fs = std::filesystem;
  char *dirdup = strdup(dir);
  char *saveptr = nullptr;
  char *token = safeStrtok(dirdup, "/", &saveptr);

  bool result = false;

#if defined(_WIN32)
  // Windows path starts without "/"
  std::string tmp;
#else
  std::string tmp("/");
#endif

  while (token != NULL) {
    tmp += token;

    if (!fs::exists(tmp)) {
      result = fs::create_directories(tmp);

      if (!result) {
        break;
      }
    }

    token = safeStrtok(NULL, "/", &saveptr);
    tmp += "/";
  }

  free(dirdup);
  return result;
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
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorAtomicCounter)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorMutable)
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

  const bool isSpvBinary = isSpirvBinary(spvBin);

  if (isSpvBinary) {
    // Skip SPIR-V header
    const unsigned *codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

    while (codePos < end) {
      unsigned opCode = (codePos[0] & OpCodeMask);
      unsigned wordCount = (codePos[0] >> WordCountShift);

      if (wordCount == 0 || codePos + wordCount > end) {
        assert(wordCount != 0 && codePos + wordCount <= end && "Invalid SPIR-V binary");
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
      assert(entryName && "Entry-point not found");
      entryName = "";
    }
  } else {
    assert(isSpvBinary && "Invalid SPIR-V binary");
    entryName = "";
  }

  return entryName;
}

// =====================================================================================================================
// Calculate 64-bit CRC for the given block of data
//
// Returns 64-bit CRC compatible with CRC-64/XZ
//
// @param data : Pointer to the block of data
// @param size : Size of the data in bytes
// @param refin : Whether to reflect input
// @param refout : Whether to reflect result
uint64_t calculateCrc64(const void *data, size_t size, bool refin, bool refout) {
  static constexpr uint64_t Poly = 0x42F0E1EBA9EA3693;
  static constexpr uint64_t InitV = 0xFFFFFFFFFFFFFFFF;
  static constexpr uint64_t XorOut = 0xFFFFFFFFFFFFFFFF;

  auto reflectByte = [](uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
  };

  auto reflect64 = [](uint64_t value) {
    uint64_t result = 0;
    for (int i = 0; i < 64; ++i) {
      if (value & (1ULL << i)) {
        result |= 1ULL << (63 - i);
      }
    }
    return result;
  };

  uint64_t crc = InitV;
  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(data);
  const uint8_t *end = ptr + size;
  while (ptr < end) {
    uint8_t byte = *ptr;
    if (refin) {
      byte = reflectByte(byte);
    }
    crc ^= static_cast<uint64_t>(byte) << 56;
    for (int i = 0; i < 8; ++i) {
      if (crc & 0x8000000000000000) {
        crc = (crc << 1) ^ Poly;
      } else {
        crc <<= 1;
      }
    }
    ptr++;
  }

  if (refout) {
    crc = reflect64(crc);
  }
  return crc ^ XorOut;
}

} // namespace Vkgc
