/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  llpcShaderModuleHelper.cpp
* @brief LLPC source file: Implementation of LLPC utility class ShaderModuleHelper
***********************************************************************************************************************
*/
#include "llpcShaderModuleHelper.h"
#include "SPIRVEntry.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVModule.h"
#include "llpcDebug.h"
#include "llpcError.h"
#include "llpcUtil.h"
#include "vkgcUtil.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <unordered_set>

using namespace llvm;
using namespace MetroHash;
using namespace spv;
using namespace SPIRV;
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
// Returns the shader module usage for the given SPIR-V module.
//
// @param module : SPIR-V module
// @returns : Shader module usage info
ShaderModuleUsage ShaderModuleHelper::getShaderModuleUsageInfo(SPIRVModule *module) {
  assert(module);
  ShaderModuleUsage shaderModuleUsage = {};

  // Helper to set corresponding usage based on the specified built-in
  auto processBuiltIn = [&](BuiltIn builtIn, bool structMember) {
    switch (builtIn) {
    case BuiltInPointSize:
      // NOTE: When any member of gl_PerVertex is used, its other members will be added to SPIR-V in the annotation
      // section. We are unable to determine their actual usage unless we parse the AccessChain instruction.
      if (!structMember)
        shaderModuleUsage.usePointSize = true;
      break;
    case BuiltInPrimitiveShadingRateKHR:
    case BuiltInShadingRateKHR:
      shaderModuleUsage.useShadingRate = true;
      break;
    case BuiltInSamplePosition:
      shaderModuleUsage.useSampleInfo = true;
      break;
    case BuiltInFragCoord:
      shaderModuleUsage.useFragCoord = true;
      break;
    case BuiltInViewportIndex:
    case BuiltInPointCoord:
    case BuiltInLayer:
      shaderModuleUsage.useGenericBuiltIn = true;
      break;
    case BuiltInClipDistance:
    case BuiltInCullDistance:
      // NOTE: When any member of gl_PerVertex is used, its other members will be added to SPIR-V in the annotation
      // section. We are unable to determine their actual usage unless we parse the AccessChain instruction.
      if (!structMember)
        shaderModuleUsage.useGenericBuiltIn = true;
      break;
    case BuiltInBaryCoordKHR:
    case BuiltInBaryCoordNoPerspKHR:
      shaderModuleUsage.useBarycentric = true;
      break;
    case BuiltInLaunchIdKHR:
      shaderModuleUsage.rtSystemValueUsage.ray.launchId = true;
      break;
    case BuiltInLaunchSizeKHR:
      shaderModuleUsage.rtSystemValueUsage.ray.launchSize = true;
      break;
    case BuiltInWorldRayOriginKHR:
      shaderModuleUsage.rtSystemValueUsage.ray.worldRayOrigin = true;
      break;
    case BuiltInWorldRayDirectionKHR:
      shaderModuleUsage.rtSystemValueUsage.ray.worldRayDirection = true;
      break;
    case BuiltInIncomingRayFlagsKHR:
      shaderModuleUsage.rtSystemValueUsage.ray.flags = true;
      break;
    case BuiltInRayTminKHR:
      shaderModuleUsage.rtSystemValueUsage.ray.tMin = true;
      break;
    case BuiltInHitTNV:
      shaderModuleUsage.rtSystemValueUsage.ray.tCurrent = true;
      break;
    case BuiltInObjectRayOriginKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.objectRayOrigin = true;
      break;
    case BuiltInObjectRayDirectionKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.objectRayDirection = true;
      break;
    case BuiltInPrimitiveId:
      shaderModuleUsage.useGenericBuiltIn = true;
      shaderModuleUsage.rtSystemValueUsage.primitive.primitiveIndex = true;
      break;
    case BuiltInInstanceId:
      shaderModuleUsage.rtSystemValueUsage.primitive.instanceID = true;
      break;
    case BuiltInInstanceCustomIndexKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.instanceIndex = true;
      break;
    case BuiltInObjectToWorldKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.objectToWorld = true;
      break;
    case BuiltInWorldToObjectKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.worldToObject = true;
      break;
    case BuiltInHitKindKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.hitKind = true;
      break;
    case BuiltInHitTriangleVertexPositionsKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.hitTrianglePosition = true;
      break;
    case BuiltInRayGeometryIndexKHR:
      shaderModuleUsage.rtSystemValueUsage.primitive.geometryIndex = true;
      break;
    default:
      break;
    }
  };

  // Set usage relevant to constants
  for (unsigned i = 0; i < module->getNumConstants(); ++i) {
    auto constant = module->getConstant(i);

    // Built-in decoration could be applied to constant
    SPIRVWord builtIn = SPIRVWORD_MAX;
    if (constant->hasDecorate(DecorationBuiltIn, 0, &builtIn))
      processBuiltIn(static_cast<BuiltIn>(builtIn), false);

    if (constant->getOpCode() == OpSpecConstantTrue || constant->getOpCode() == OpSpecConstantFalse ||
        constant->getOpCode() == OpSpecConstant || constant->getOpCode() == OpSpecConstantComposite ||
        constant->getOpCode() == OpSpecConstantOp)
      shaderModuleUsage.useSpecConstant = true;
  }

  // Set usage relevant to variables
  bool hasIndexDecorate = false;

  for (unsigned i = 0; i < module->getNumVariables(); ++i) {
    auto variable = module->getVariable(i);
    if (variable->hasDecorate(DecorationIndex))
      hasIndexDecorate = true;

    if (variable->hasDecorate(DecorationInvariant))
      shaderModuleUsage.useInvariant = true;

    // Built-in decoration applied to variable
    SPIRVWord builtIn = SPIRVWORD_MAX;
    if (variable->hasDecorate(DecorationBuiltIn, 0, &builtIn))
      processBuiltIn(static_cast<BuiltIn>(builtIn), false);

    auto variableType = variable->getType()->getPointerElementType(); // Dereference to variable value type
    if (variableType && variableType->isTypeStruct()) {
      // Struct type, built-in decoration could be applied to struct member
      for (unsigned j = 0; j < variableType->getStructMemberCount(); ++j) {
        if (variableType->hasMemberDecorate(j, DecorationInvariant))
          shaderModuleUsage.useInvariant = true;

        builtIn = SPIRVWORD_MAX;
        if (variableType->hasMemberDecorate(j, DecorationBuiltIn, 0, &builtIn))
          processBuiltIn(static_cast<BuiltIn>(builtIn), true);
      }
    }
  }

  if (!hasIndexDecorate)
    shaderModuleUsage.disableDualSource = true;

  // Set usage relevant to instructions
  for (unsigned i = 0; i < module->getNumFunctions(); ++i) {
    auto func = module->getFunction(i);
    for (unsigned j = 0; j < func->getNumBasicBlock(); ++j) {
      auto block = func->getBasicBlock(j);
      for (unsigned k = 0; k < block->getNumInst(); ++k) {
        auto inst = block->getInst(k);
        switch (inst->getOpCode()) {
        case OpExtInst: {
          auto extInst = static_cast<SPIRVExtInst *>(inst);
          if (extInst->getExtOp() == GLSLstd450InterpolateAtSample)
            shaderModuleUsage.useSampleInfo = true;
          else if (extInst->getExtOp() == GLSLstd450NMin || extInst->getExtOp() == GLSLstd450NMax)
            shaderModuleUsage.useIsNan = true;
          break;
        }
        case OpTraceNV:
        case OpTraceRayKHR:
          shaderModuleUsage.hasTraceRay = true;
          break;
        case OpExecuteCallableNV:
        case OpExecuteCallableKHR:
          shaderModuleUsage.hasExecuteCallable = true;
          break;
        case OpIsNan:
          shaderModuleUsage.useIsNan = true;
          break;
        case OpAccessChain: {
          auto accessChain = static_cast<SPIRVAccessChain *>(inst);
          auto base = accessChain->getBase();
          auto baseType = base->getType()->getPointerElementType(); // Dereference to base value type

          // NOTE: When any member of gl_PerVertex is used, its other members will be added to SPIR-V in the annotation
          // section. We are unable to determine their actual usage unless we parse the AccessChain instruction.
          // This has impacts on Position, PointSize, ClipDistance, and CullDistance.
          if (base->getType()->getPointerStorageClass() == StorageClassOutput && baseType && baseType->isTypeStruct()) {
            // We find an output struct variable, further check its member built-in decorations.
            const auto index = static_cast<SPIRVConstant *>(accessChain->getIndices()[0])->getZExtIntValue();
            SPIRVWord builtIn = SPIRVWORD_MAX;
            if (baseType->hasMemberDecorate(index, DecorationBuiltIn, 0, &builtIn)) {
              switch (builtIn) {
              case BuiltInPointSize:
                shaderModuleUsage.usePointSize = true;
                break;
              case BuiltInClipDistance:
              case BuiltInCullDistance:
                shaderModuleUsage.useGenericBuiltIn = true;
                break;
              default:
                break;
              }
            }
          }
          break;
        }
        default:
          break;
        }
      }
    }
  }

  // Set usage relevant to execution modes
  for (unsigned i = 0; i < module->getNumFunctions(); ++i) {
    auto func = module->getFunction(i);
    if (module->getEntryPoint(func->getId())) {
      if (func->getExecutionMode(ExecutionModeOriginUpperLeft))
        shaderModuleUsage.originUpperLeft = true;

      if (func->getExecutionMode(ExecutionModePixelCenterInteger))
        shaderModuleUsage.pixelCenterInteger = true;

      if (func->getExecutionMode(ExecutionModeXfb))
        shaderModuleUsage.enableXfb = true;
    }
  }

  // Set usage relevant to capabilities
  if (module->hasCapability(CapabilityVariablePointersStorageBuffer))
    shaderModuleUsage.enableVarPtrStorageBuf = true;

  if (module->hasCapability(CapabilityVariablePointers))
    shaderModuleUsage.enableVarPtr = true;

  if (module->hasCapability(CapabilityRayQueryKHR))
    shaderModuleUsage.enableRayQuery = true;

  if (module->getExtension().count("SPV_AMD_shader_ballot") > 0)
    shaderModuleUsage.useSubgroupSize = true;

  if (!shaderModuleUsage.useSubgroupSize &&
      (module->hasCapability(CapabilityGroupNonUniform) || module->hasCapability(CapabilityGroupNonUniformVote) ||
       module->hasCapability(CapabilityGroupNonUniformArithmetic) ||
       module->hasCapability(CapabilityGroupNonUniformBallot) ||
       module->hasCapability(CapabilityGroupNonUniformShuffle) ||
       module->hasCapability(CapabilityGroupNonUniformShuffleRelative) ||
       module->hasCapability(CapabilityGroupNonUniformClustered) ||
       module->hasCapability(CapabilityGroupNonUniformQuad) || module->hasCapability(CapabilitySubgroupBallotKHR) ||
       module->hasCapability(CapabilitySubgroupVoteKHR) || module->hasCapability(CapabilityGroups) ||
       module->hasCapability(CapabilityGroupNonUniformRotateKHR))) {
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
// @returns : The number of bytes written to trimSpvBin or an error if invalid data was encountered
Expected<unsigned> ShaderModuleHelper::trimSpirvDebugInfo(const BinaryData *spvBin,
                                                          llvm::MutableArrayRef<unsigned> codeBuffer) {
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

  unsigned nonSemanticShaderDebug = ~0;

  // Copy SPIR-V instructions
  while (codePos < end) {
    unsigned opCode = (codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);

    if (wordCount == 0 || codePos + wordCount > end) {
      LLPC_ERRS("Invalid SPIR-V binary\n");
      return createResultError(Result::ErrorInvalidShader);
    }

    bool skip = false;
    switch (opCode) {
    case OpSource:
    case OpSourceContinued:
    case OpSourceExtension:
    case OpMemberName:
    case OpLine:
    case OpNop:
    case OpNoLine:
    case OpModuleProcessed:
      skip = true;
      break;
    case OpExtInstImport: {
      unsigned id = codePos[1];
      const char *name = reinterpret_cast<const char *>(&codePos[2]);
      if (!strcmp(name, "NonSemantic.Shader.DebugInfo.100")) {
        nonSemanticShaderDebug = id;
        skip = true;
      }
      break;
    }
    case OpExtInstWithForwardRefsKHR:
    case OpExtInst: {
      unsigned set = codePos[3];
      if (set == nonSemanticShaderDebug)
        skip = true;
      break;
    }
    default:
      break;
    }

    if (!skip) {
      if (writeCode) {
        assert(codePos + wordCount <= end);
        assert(wordCount <= codeBuffer.size());
        memcpy(codeBuffer.data(), codePos, wordCount * wordSize);
        codeBuffer = codeBuffer.drop_front(wordCount);
      }
      totalSizeInWords += wordCount;
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
// @param module : SPIR-V module (valid when the binary type is SPIR-V)
// @param codeBuffer [out] : A buffer to hold the trimmed code if it is needed.
// @param moduleData [out] : If successful, the module data for the module.  Undefined if unsuccessful.
// @return : Success if the data was read.  The appropriate error otherwise.
Result ShaderModuleHelper::getModuleData(const ShaderModuleBuildInfo *shaderInfo, SPIRVModule *module,
                                         llvm::MutableArrayRef<unsigned> codeBuffer,
                                         Vkgc::ShaderModuleData &moduleData) {
  if (moduleData.binType == BinaryType::Spirv) {
    moduleData.usage = ShaderModuleHelper::getShaderModuleUsageInfo(module);
    moduleData.usage.isInternalRtShader = shaderInfo->options.pipelineOptions.internalRtShaders;
    auto codeOrErr = getShaderCode(shaderInfo, codeBuffer);
    if (Error err = codeOrErr.takeError())
      return errorToResult(std::move(err));

    moduleData.binCode = *codeOrErr;

    // Calculate SPIR-V cache hash
    Hash cacheHash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(moduleData.binCode.pCode), moduleData.binCode.codeSize,
                      cacheHash.bytes);
    static_assert(sizeof(moduleData.cacheHash) == sizeof(cacheHash),
                  "Expecting the cacheHash entry in the module data to be the same size as the MetroHash hash!");
    memcpy(moduleData.cacheHash, cacheHash.dwords, sizeof(cacheHash));
  } else {
    moduleData.binCode = shaderInfo->shaderBin;
    memcpy(codeBuffer.data(), shaderInfo->shaderBin.pCode, shaderInfo->shaderBin.codeSize);
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
Expected<BinaryData> ShaderModuleHelper::getShaderCode(const ShaderModuleBuildInfo *shaderInfo,
                                                       MutableArrayRef<unsigned int> &codeBuffer) {
  BinaryData code;
  const BinaryData &shaderBinary = shaderInfo->shaderBin;
  bool trimDebugInfo = cl::TrimDebugInfo && !(shaderInfo->options.pipelineOptions.internalRtShaders);
  if (trimDebugInfo) {
    auto sizeOrErr = trimSpirvDebugInfo(&shaderBinary, codeBuffer);
    if (Error err = sizeOrErr.takeError())
      return std::move(err);
    code.codeSize = *sizeOrErr;
  } else {
    assert(shaderBinary.codeSize <= codeBuffer.size() * sizeof(codeBuffer.front()));
    memcpy(codeBuffer.data(), shaderBinary.pCode, shaderBinary.codeSize);
    code.codeSize = shaderBinary.codeSize;
  }
  code.pCode = codeBuffer.data();
  return code;
}

// =====================================================================================================================
// Get shader code size. If SPIR-V binary is trimmed, get the new size.
//
// @param shaderInfo : Shader module build info
// @return : The number of bytes need to hold the code for this shader module.
Expected<unsigned> ShaderModuleHelper::getShaderCodeSize(const ShaderModuleBuildInfo *shaderInfo) {
  const BinaryData &shaderBinary = shaderInfo->shaderBin;
  BinaryType binaryType;
  Result result = ShaderModuleHelper::getShaderBinaryType(shaderBinary, binaryType);
  if (result != Result::Success)
    return createResultError(Result::ErrorInvalidShader);

  bool trimDebugInfo =
      binaryType != BinaryType::LlvmBc && cl::TrimDebugInfo && !(shaderInfo->options.pipelineOptions.internalRtShaders);
  if (!trimDebugInfo)
    return shaderBinary.codeSize;

  return ShaderModuleHelper::trimSpirvDebugInfo(&shaderBinary, {});
}

} // namespace Llpc
