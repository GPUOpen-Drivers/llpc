/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <unordered_set>

using namespace llvm;
using namespace MetroHash;
using namespace spv;
using namespace Util;

using Vkgc::SpirvHeader;

namespace llvm {
namespace cl {

// -trim-debug-info: Trim debug information in SPIR-V binary
opt<bool> TrimDebugInfo("trim-debug-info", cl::desc("Trim debug information in SPIR-V binary"), init(true));

} // namespace cl
} // namespace llvm

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
    case OpExtInst: {
      auto extInst = static_cast<GLSLstd450>(codePos[4]);
      switch (extInst) {
      case GLSLstd450InterpolateAtSample:
        shaderModuleUsage.useSampleInfo = true;
        break;
      case GLSLstd450NMin:
      case GLSLstd450NMax:
        shaderModuleUsage.useIsNan = true;
        break;
      default:
        break;
      }
      break;
    }
    case OpExtension: {
      StringRef extName = reinterpret_cast<const char *>(&codePos[1]);
      if (extName == "SPV_AMD_shader_ballot") {
        shaderModuleUsage.useSubgroupSize = true;
      }
      break;
    }
    case OpExecutionMode: {
      auto execMode = static_cast<ExecutionMode>(codePos[2]);
      switch (execMode) {
      case ExecutionModeOriginUpperLeft:
        shaderModuleUsage.originUpperLeft = true;
        break;
      case ExecutionModePixelCenterInteger:
        shaderModuleUsage.pixelCenterInteger = true;
        break;
      default: {
        break;
      }
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
      if (decoration == DecorationBuiltIn) {
        auto builtIn = (opCode == OpDecorate) ? static_cast<BuiltIn>(codePos[3]) : static_cast<BuiltIn>(codePos[4]);
        switch (builtIn) {
        case BuiltInPointSize: {
          shaderModuleUsage.usePointSize = true;
          break;
        }
        case BuiltInPrimitiveShadingRateKHR:
        case BuiltInShadingRateKHR: {
          shaderModuleUsage.useShadingRate = true;
          break;
        }
        case BuiltInSamplePosition: {
          shaderModuleUsage.useSampleInfo = true;
          break;
        }
        case BuiltInFragCoord: {
          shaderModuleUsage.useFragCoord = true;
          break;
        }
        case BuiltInPointCoord:
        case BuiltInPrimitiveId:
        case BuiltInLayer:
        case BuiltInClipDistance:
        case BuiltInCullDistance: {
          shaderModuleUsage.useGenericBuiltIn = true;
          break;
        }
        default: {
          break;
        }
        }
      }
      if (decoration == DecorationLocation) {
        auto location = (opCode == OpDecorate) ? codePos[3] : codePos[4];
        if (location == static_cast<unsigned>(Vkgc::GlCompatibilityInOutLocation::ClipVertex))
          shaderModuleUsage.useClipVertex = true;
      }
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
    case OpTraceNV:
    case OpTraceRayKHR: {
      shaderModuleUsage.hasTraceRay = true;
      break;
    }
    case OpExecuteCallableNV:
    case OpExecuteCallableKHR:
      shaderModuleUsage.hasExecuteCallable = true;
      break;
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

  if (capabilities.find(CapabilityRayQueryKHR) != capabilities.end())
    shaderModuleUsage.enableRayQuery = true;

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
// Returns the number of bytes in the spir-v binary if the debug instructions are removed.  If codeBuffer is not empty,
// then the spir-v binary without the debug instructions will be written to it.  The size of codeBuffer must be large
// enough to contain the binary.
//
// @param spvBin : SPIR-V binary code
// @param codeBuffer : The buffer in which to copy the shader code.
// @returns : The number of bytes written to trimSpvBin
unsigned ShaderModuleHelper::trimSpirvDebugInfo(const BinaryData *spvBin, llvm::MutableArrayRef<unsigned> codeBuffer) {
  bool writeCode = !codeBuffer.empty();
  assert(codeBuffer.empty() || codeBuffer.size() > sizeof(SpirvHeader));

  constexpr unsigned wordSize = sizeof(unsigned);
  const unsigned *code = reinterpret_cast<const unsigned *>(spvBin->pCode);
  const unsigned *end = code + spvBin->codeSize / wordSize;
  const unsigned *codePos = code + sizeof(SpirvHeader) / wordSize;

  unsigned totalSizeInWords = sizeof(Vkgc::SpirvHeader) / wordSize;
  static_assert(sizeof(Vkgc::SpirvHeader) % wordSize == 0,
                "The size of the spir-v header must be a multiple of the word size, otherwise later calculations will "
                "be incorrect.");

  // Copy SPIR-V header
  if (writeCode) {
    memcpy(codeBuffer.data(), code, sizeof(SpirvHeader));
    codeBuffer = codeBuffer.drop_front(sizeof(Vkgc::SpirvHeader) / wordSize);
  }

  // Copy SPIR-V instructions
  while (codePos < end) {
    unsigned opCode = (codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);
    switch (opCode) {
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
      if (writeCode) {
        assert(codePos + wordCount <= end);
        assert(wordCount <= codeBuffer.size());
        memcpy(codeBuffer.data(), codePos, wordCount * wordSize);
        codeBuffer = codeBuffer.drop_front(wordCount);
      }
      totalSizeInWords += wordCount;
      break;
    }
    }

    codePos += wordCount;
  }
  return totalSizeInWords * wordSize;
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
        spvOptimizeSpirv(spirvBinIn->codeSize, spirvBinIn->pCode, 0, nullptr, &optBinSize, &pOptBin, 4096, logBuf);
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
// @param shaderBinary : Shader binary code
// @param[out] binaryType : Overwritten with the detected binary type, or BinaryType::Unknown if it could not be
//                          determined
// @return : Success is returned if the binary type was detected and any sanity checks have passed
Result ShaderModuleHelper::getShaderBinaryType(BinaryData shaderBinary, BinaryType &binaryType) {
  binaryType = BinaryType::Unknown;
  if (ShaderModuleHelper::isLlvmBitcode(&shaderBinary)) {
    binaryType = BinaryType::LlvmBc;
    return Result::Success;
  }
  if (Vkgc::isSpirvBinary(&shaderBinary)) {
    binaryType = BinaryType::Spirv;
    if (verifySpirvBinary(&shaderBinary) != Result::Success) {
      LLPC_ERRS("Unsupported SPIR-V instructions found in the SPIR-V binary!\n");
      return Result::ErrorInvalidShader;
    }
    return Result::Success;
  }
  return Result::ErrorInvalidShader;
}

// =====================================================================================================================
// Returns the extended module data for the given binary data.  If the code needs to be trimmed then the module data
// will point to the data in trimmed code.  It should not be resized or deallocated while moduleData is still needed.
//
// @param shaderInfo : Shader module build info
// @param codeBuffer [out] : A buffer to hold the trimmed code if it is needed.
// @param moduleData [out] : If successful, the module data for the module.  Undefined if unsuccessful.
// @return : Success if the data was read.  The appropriate error otherwise.
Result ShaderModuleHelper::getModuleData(const ShaderModuleBuildInfo *shaderInfo,
                                         llvm::MutableArrayRef<unsigned> codeBuffer,
                                         Vkgc::ShaderModuleData &moduleData) {
  const BinaryData &shaderBinary = shaderInfo->shaderBin;
  Result result = ShaderModuleHelper::getShaderBinaryType(shaderBinary, moduleData.binType);
  if (result != Result::Success)
    return result;

  if (moduleData.binType == BinaryType::Spirv) {
    moduleData.usage = ShaderModuleHelper::getShaderModuleUsageInfo(&shaderBinary);
    moduleData.binCode = getShaderCode(shaderInfo, codeBuffer);
    moduleData.usage.isInternalRtShader = shaderInfo->options.pipelineOptions.internalRtShaders;

    // Calculate SPIR-V cache hash
    Hash cacheHash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(moduleData.binCode.pCode), moduleData.binCode.codeSize,
                      cacheHash.bytes);
    static_assert(sizeof(moduleData.cacheHash) == sizeof(cacheHash),
                  "Expecting the cacheHash entry in the module data to be the same size as the MetroHash hash!");
    memcpy(moduleData.cacheHash, cacheHash.dwords, sizeof(cacheHash));
  } else {
    moduleData.binCode = shaderBinary;
  }

  return Result::Success;
}

// =====================================================================================================================
// Copies the shader code that should go into the module data into the codeBuffer and returns the BinaryData for it.
// The debug info will be removed if cl::TrimDebugInfo is set.
//
// @param shaderInfo : Shader module build info
// @param codeBuffer [out] : A buffer to hold the shader code.
// @return : The BinaryData for the shaderCode written to codeBuffer.
BinaryData ShaderModuleHelper::getShaderCode(const ShaderModuleBuildInfo *shaderInfo,
                                             MutableArrayRef<unsigned int> &codeBuffer) {
  BinaryData code;
  const BinaryData &shaderBinary = shaderInfo->shaderBin;
  bool trimDebugInfo = cl::TrimDebugInfo && !(shaderInfo->options.pipelineOptions.internalRtShaders);
  if (trimDebugInfo) {
    code.codeSize = trimSpirvDebugInfo(&shaderBinary, codeBuffer);
  } else {
    assert(shaderBinary.codeSize <= codeBuffer.size() * sizeof(codeBuffer.front()));
    memcpy(codeBuffer.data(), shaderBinary.pCode, shaderBinary.codeSize);
    code.codeSize = shaderBinary.codeSize;
  }
  code.pCode = codeBuffer.data();
  return code;
}

// =====================================================================================================================
// @param shaderInfo : Shader module build info
// @return : The number of bytes need to hold the code for this shader module.
unsigned ShaderModuleHelper::getCodeSize(const ShaderModuleBuildInfo *shaderInfo) {
  const BinaryData &shaderBinary = shaderInfo->shaderBin;
  bool trimDebugInfo = cl::TrimDebugInfo && !(shaderInfo->options.pipelineOptions.internalRtShaders);
  if (!trimDebugInfo)
    return shaderBinary.codeSize;
  return ShaderModuleHelper::trimSpirvDebugInfo(&shaderBinary, {});
}

} // namespace Llpc
