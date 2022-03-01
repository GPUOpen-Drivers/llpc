/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  llpcShaderModuleHelper.cpp
* @brief LLPC source file: Implementation of LLPC utility class ShaderModuleHelper
***********************************************************************************************************************
*/
#include "llpcShaderModuleHelper.h"
#include "llpcDebug.h"
#include "llpcUtil.h"
#include "spirvExt.h"
#include "vkgcUtil.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <unordered_set>

using namespace llvm;
using namespace MetroHash;
using namespace spv;
using namespace Util;

using Vkgc::SpirvHeader;

namespace Llpc {
// =====================================================================================================================
// Returns the shader module usage for the given Spir-V binary.
//
// @param spvBinCode : SPIR-V binary data
// @returns : Shader module usage info
ShaderModuleUsage ShaderModuleHelper::getShaderModuleUsageInfo(const BinaryData *spvBinCode) {
  const unsigned *code = reinterpret_cast<const unsigned *>(spvBinCode->pCode);
  const unsigned *end = code + spvBinCode->codeSize / sizeof(unsigned);
  const unsigned *codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

  ShaderModuleUsage shaderModuleUsage = {};
  // Parse SPIR-V instructions
  std::unordered_set<unsigned> capabilities;

  while (codePos < end) {
    unsigned opCode = (codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);
    assert(wordCount > 0 && codePos + wordCount <= end && "Invalid SPIR-V binary\n");

    // Parse each instruction and find those we are interested in
    switch (opCode) {
    case OpCapability: {
      assert(wordCount == 2);
      auto capability = static_cast<Capability>(codePos[1]);
      capabilities.insert(capability);
      break;
    }
    case OpExtension: {
      StringRef extName = reinterpret_cast<const char *>(&codePos[1]);
      if (extName == "SPV_AMD_shader_ballot") {
        shaderModuleUsage.useSubgroupSize = true;
      }
      break;
    }
    case OpDecorate:
    case OpMemberDecorate: {
      auto decoration =
          (opCode == OpDecorate) ? static_cast<Decoration>(codePos[2]) : static_cast<Decoration>(codePos[3]);
      if (decoration == DecorationInvariant) {
        shaderModuleUsage.useInvariant = true;
      }
      break;
    }
    case OpDPdx:
    case OpDPdy:
    case OpDPdxCoarse:
    case OpDPdyCoarse:
    case OpDPdxFine:
    case OpDPdyFine:
    case OpImageSampleImplicitLod:
    case OpImageSampleDrefImplicitLod:
    case OpImageSampleProjImplicitLod:
    case OpImageSampleProjDrefImplicitLod:
    case OpImageSparseSampleImplicitLod:
    case OpImageSparseSampleProjDrefImplicitLod:
    case OpImageSparseSampleProjImplicitLod: {
      shaderModuleUsage.useHelpInvocation = true;
      break;
    }
    case OpSpecConstantTrue:
    case OpSpecConstantFalse:
    case OpSpecConstant:
    case OpSpecConstantComposite:
    case OpSpecConstantOp: {
      shaderModuleUsage.useSpecConstant = true;
      break;
    }
    case OpIsNan: {
      shaderModuleUsage.useIsNan = true;
      break;
    }
    default: {
      break;
    }
    }
    codePos += wordCount;
  }

  if (capabilities.find(CapabilityVariablePointersStorageBuffer) != capabilities.end())
    shaderModuleUsage.enableVarPtrStorageBuf = true;

  if (capabilities.find(CapabilityVariablePointers) != capabilities.end())
    shaderModuleUsage.enableVarPtr = true;

  if ((!shaderModuleUsage.useSubgroupSize) &&
      ((capabilities.count(CapabilityGroupNonUniform) > 0) || (capabilities.count(CapabilityGroupNonUniformVote) > 0) ||
       (capabilities.count(CapabilityGroupNonUniformArithmetic) > 0) ||
       (capabilities.count(CapabilityGroupNonUniformBallot) > 0) ||
       (capabilities.count(CapabilityGroupNonUniformShuffle) > 0) ||
       (capabilities.count(CapabilityGroupNonUniformShuffleRelative) > 0) ||
       (capabilities.count(CapabilityGroupNonUniformClustered) > 0) ||
       (capabilities.count(CapabilityGroupNonUniformQuad) > 0) ||
       (capabilities.count(CapabilitySubgroupBallotKHR) > 0) || (capabilities.count(CapabilitySubgroupVoteKHR) > 0) ||
       (capabilities.count(CapabilityGroups) > 0))) {
    shaderModuleUsage.useSubgroupSize = true;
  }

  return shaderModuleUsage;
}

// =====================================================================================================================
// Removes all debug instructions for SPIR-V binary.
//
// @param spvBin : SPIR-V binary code
// @param bufferSize : Output buffer size in bytes
// @param [out] trimSpvBin : Trimmed SPIR-V binary code
// @returns : The number of bytes written to trimSpvBin
unsigned ShaderModuleHelper::trimSpirvDebugInfo(const BinaryData *spvBin, unsigned bufferSize, void *trimSpvBin) {
  assert(bufferSize > sizeof(SpirvHeader));

  const unsigned *code = reinterpret_cast<const unsigned *>(spvBin->pCode);
  const unsigned *end = code + spvBin->codeSize / sizeof(unsigned);
  const unsigned *codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

  unsigned *trimStart = reinterpret_cast<unsigned *>(trimSpvBin);
  unsigned *trimEnd = reinterpret_cast<unsigned *>(voidPtrInc(trimSpvBin, bufferSize));
  (void(trimEnd)); // unused
  unsigned *trimCodePos = reinterpret_cast<unsigned *>(voidPtrInc(trimSpvBin, sizeof(SpirvHeader)));

  // Copy SPIR-V header
  memcpy(trimSpvBin, code, sizeof(SpirvHeader));

  // Copy SPIR-V instructions
  while (codePos < end) {
    unsigned opCode = (codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);
    switch (opCode) {
    case OpString:
    case OpSource:
    case OpSourceContinued:
    case OpSourceExtension:
    case OpMemberName:
    case OpLine:
    case OpNop:
    case OpNoLine:
    case OpModuleProcessed: {
      // Skip debug instructions
      break;
    }
    default: {
      // Copy other instructions
      assert(codePos + wordCount <= end);
      assert(trimCodePos + wordCount <= trimEnd);
      memcpy(trimCodePos, codePos, wordCount * sizeof(unsigned));
      trimCodePos += wordCount;
      break;
    }
    }

    codePos += wordCount;
  }

  assert(trimCodePos <= trimEnd);
  return (trimCodePos - trimStart) * sizeof(*trimStart);
}

// =====================================================================================================================
// Optimizes SPIR-V binary
//
// @param spirvBinIn : Input SPIR-V binary
// @param [out] spirvBinOut : Optimized SPIR-V binary
Result ShaderModuleHelper::optimizeSpirv(const BinaryData *spirvBinIn, BinaryData *spirvBinOut) {
  bool success = false;
  unsigned optBinSize = 0;
  void *optBin = nullptr;

#ifdef LLPC_ENABLE_SPIRV_OPT
  if (cl::EnableSpirvOpt) {
    char logBuf[4096] = {};
    success =
        spvOptimizeSpirv(pSpirvBinIn->codeSize, pSpirvBinIn->pCode, 0, nullptr, &optBinSize, &pOptBin, 4096, logBuf);
    if (success == false) {
      LLPC_ERROR("Failed to optimize SPIR-V ("
                 << GetShaderStageName(static_cast<ShaderStage>(shaderStage) << " shader): " << logBuf));
    }
  }
#endif

  if (success) {
    spirvBinOut->codeSize = optBinSize;
    spirvBinOut->pCode = optBin;
  } else {
    spirvBinOut->codeSize = 0;
    spirvBinOut->pCode = nullptr;
  }

  return success ? Result::Success : Result::ErrorInvalidShader;
}

// =====================================================================================================================
// Cleanup work for SPIR-V binary, freeing the allocated buffer by OptimizeSpirv()
//
// @param spirvBin : Optimized SPIR-V binary
void ShaderModuleHelper::cleanOptimizedSpirv(BinaryData *spirvBin) {
#ifdef LLPC_ENABLE_SPIRV_OPT
  if (pSpirvBin->pCode) {
    spvFreeBuffer(const_cast<void *>(pSpirvBin->pCode));
  }
#endif
}

// =====================================================================================================================
// Gets the shader stage mask from the SPIR-V binary according to the specified entry-point.
//
// Returns 0 on error, or the stage mask of the specified entry-point on success.
//
// @param spvBin : SPIR-V binary
// @param entryName : Name of entry-point
unsigned ShaderModuleHelper::getStageMaskFromSpirvBinary(const BinaryData *spvBin, const char *entryName) {
  unsigned stageMask = 0;

  const unsigned *code = reinterpret_cast<const unsigned *>(spvBin->pCode);
  const unsigned *end = code + spvBin->codeSize / sizeof(unsigned);

  if (isSpirvBinary(spvBin)) {
    // Skip SPIR-V header
    const unsigned *codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

    while (codePos < end) {
      unsigned opCode = (codePos[0] & OpCodeMask);
      unsigned wordCount = (codePos[0] >> WordCountShift);

      if (wordCount == 0 || codePos + wordCount > end) {
        LLPC_ERRS("Invalid SPIR-V binary\n");
        stageMask = 0;
        break;
      }

      if (opCode == OpEntryPoint) {
        assert(wordCount >= 4);

        // The fourth word is start of the name string of the entry-point
        const char *name = reinterpret_cast<const char *>(&codePos[3]);
        if (strcmp(entryName, name) == 0) {
          // An matching entry-point is found
          stageMask |= shaderStageToMask(convertToShaderStage(codePos[1]));
        }
      }

      // All "OpEntryPoint" are before "OpFunction"
      if (opCode == OpFunction)
        break;

      codePos += wordCount;
    }
  } else {
    LLPC_ERRS("Invalid SPIR-V binary\n");
  }

  return stageMask;
}

// =====================================================================================================================
// Verifies if the SPIR-V binary is valid and is supported
//
// @param spvBin : SPIR-V binary
Result ShaderModuleHelper::verifySpirvBinary(const BinaryData *spvBin) {
  Result result = Result::Success;

#define _SPIRV_OP(x, ...) Op##x,
  static const std::set<Op> OpSet{{
#include "SPIRVOpCodeEnum.h"
  }};
#undef _SPIRV_OP

  const unsigned *code = reinterpret_cast<const unsigned *>(spvBin->pCode);
  const unsigned *end = code + spvBin->codeSize / sizeof(unsigned);

  // Skip SPIR-V header
  const unsigned *codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

  while (codePos < end) {
    Op opCode = static_cast<Op>(codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);

    if (wordCount == 0 || codePos + wordCount > end) {
      result = Result::ErrorInvalidShader;
      break;
    }

    if (OpSet.find(opCode) == OpSet.end()) {
      result = Result::ErrorInvalidShader;
      break;
    }

    codePos += wordCount;
  }

  return result;
}

// =====================================================================================================================
// Checks whether input binary data is LLVM bitcode.
//
// @param shaderBin : Shader binary codes
bool ShaderModuleHelper::isLlvmBitcode(const BinaryData *shaderBin) {
  static unsigned BitcodeMagicNumber = 0xDEC04342; // 0x42, 0x43, 0xC0, 0xDE
  bool isLlvmBitcode = false;
  if (shaderBin->codeSize > 4 && *reinterpret_cast<const unsigned *>(shaderBin->pCode) == BitcodeMagicNumber)
    isLlvmBitcode = true;

  return isLlvmBitcode;
}

// =====================================================================================================================
// Returns the binary type for the given shader binary.
//
// @param shaderBin : Shader binary code
// @return : The binary type for the shader or BinaryType::Unknown if it could not be determined.
BinaryType ShaderModuleHelper::getShaderBinaryType(BinaryData shaderBinary) {
  if (ShaderModuleHelper::isLlvmBitcode(&shaderBinary))
    return BinaryType::LlvmBc;
  if (Vkgc::isSpirvBinary(&shaderBinary)) {
    if (verifySpirvBinary(&shaderBinary) != Result::Success) {
      LLPC_ERRS("Unsupported SPIR-V instructions found in the SPIR-V binary!\n");
      return BinaryType::Unknown;
    }
    return BinaryType::Spirv;
  }
  return BinaryType::Unknown;
}

// =====================================================================================================================
// Returns the extended module data for the given binary data.  If the code needs to be trimmed then the module data
// will point to the data in trimmed code.  It should not be resized or deallocated while moduleData is still needed.
//
// @param moduleBinary : Shader binary code
// @param trimDebugInfo : True if the debug info should be removed from the SPIR-V
// @param trimmedCode [out] : A buffer to hold the trimmed code if it is needed.
// @param moduleData [out] : If successful, the module data for the module.  Undefined if unsuccessful.
// @return : Success if the data was read.  The appropriate error otherwise.
Result ShaderModuleHelper::getExtendedModuleData(const BinaryData &moduleBinary, bool trimDebugInfo,
                                                 llvm::SmallVector<uint8_t> &trimmedCode,
                                                 Vkgc::ShaderModuleData &moduleData) {
  moduleData.binType = ShaderModuleHelper::getShaderBinaryType(moduleBinary);
  if (moduleData.binType == BinaryType::Unknown)
    return Result::ErrorInvalidShader;

  if (moduleData.binType == BinaryType::Spirv) {
    moduleData.usage = ShaderModuleHelper::getShaderModuleUsageInfo(&moduleBinary);

    if (trimDebugInfo) {
      trimmedCode.resize(moduleBinary.codeSize);
      moduleData.binCode.pCode = trimmedCode.data();
      moduleData.binCode.codeSize =
          ShaderModuleHelper::trimSpirvDebugInfo(&moduleBinary, trimmedCode.size(), trimmedCode.data());
    } else {
      moduleData.binCode = moduleBinary;
    }

    // Calculate SPIR-V cache hash
    Hash cacheHash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(moduleData.binCode.pCode), moduleData.binCode.codeSize,
                      cacheHash.bytes);
    static_assert(sizeof(moduleData.cacheHash) == sizeof(cacheHash), "Unexpected value!");
    memcpy(moduleData.cacheHash, cacheHash.dwords, sizeof(cacheHash));
  } else {
    moduleData.binCode = moduleBinary;
  }

  return Result::Success;
}

} // namespace Llpc
