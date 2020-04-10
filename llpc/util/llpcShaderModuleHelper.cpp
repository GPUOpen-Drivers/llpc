/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include <unordered_set>
#include <set>

#include "llvm/Support/raw_ostream.h"
using namespace llvm;

using namespace spv;

namespace Llpc
{
// =====================================================================================================================
// Collect information from SPIR-V binary
Result ShaderModuleHelper::collectInfoFromSpirvBinary(
    const BinaryData*                spvBinCode,           // [in] SPIR-V binary data
    ShaderModuleUsage*               shaderModuleUsage,    // [out] Shader module usage info
    std::vector<ShaderEntryName>&    shaderEntryNames,      // [out] Entry names for this shader module
    unsigned*                        debugInfoSize)        // Debug info size
{
    Result result = Result::Success;

    const unsigned* code = reinterpret_cast<const unsigned*>(spvBinCode->pCode);
    const unsigned* end = code + spvBinCode->codeSize / sizeof(unsigned);

    const unsigned* codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

    // Parse SPIR-V instructions
    std::unordered_set<unsigned> capabilities;

    while (codePos < end)
    {
        unsigned opCode = (codePos[0] & OpCodeMask);
        unsigned wordCount = (codePos[0] >> WordCountShift);

        if ((wordCount == 0) || (codePos + wordCount > end))
        {
            LLPC_ERRS("Invalid SPIR-V binary\n");
            result = Result::ErrorInvalidShader;
            break;
        }

        // Parse each instruction and find those we are interested in
        switch (opCode)
        {
        case OpCapability:
            {
                assert(wordCount == 2);
                auto capability = static_cast<Capability>(codePos[1]);
                capabilities.insert(capability);
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
        case OpImageSparseSampleProjImplicitLod:
            {
                shaderModuleUsage->useHelpInvocation = true;
                break;
            }
        case OpString:
        case OpSource:
        case OpSourceContinued:
        case OpSourceExtension:
        case OpName:
        case OpMemberName:
        case OpLine:
        case OpNop:
        case OpNoLine:
        case OpModuleProcessed:
            {
                *debugInfoSize += wordCount * sizeof(unsigned);
                break;
            }
        case OpSpecConstantTrue:
        case OpSpecConstantFalse:
        case OpSpecConstant:
        case OpSpecConstantComposite:
        case OpSpecConstantOp:
            {
                shaderModuleUsage->useSpecConstant = true;
                break;
            }
        case OpEntryPoint:
            {
                ShaderEntryName entry = {};
                // The fourth word is start of the name string of the entry-point
                entry.name = reinterpret_cast<const char*>(&codePos[3]);
                entry.stage = convertToStageShage(codePos[1]);
                shaderEntryNames.push_back(entry);
                break;
            }
        default:
            {
                break;
            }
        }
        codePos += wordCount;
    }

    if (capabilities.find(CapabilityVariablePointersStorageBuffer) != capabilities.end())
        shaderModuleUsage->enableVarPtrStorageBuf = true;

    if (capabilities.find(CapabilityVariablePointers) != capabilities.end())
        shaderModuleUsage->enableVarPtr = true;

    return result;
}

// =====================================================================================================================
// Removes all debug instructions for SPIR-V binary.
void ShaderModuleHelper::trimSpirvDebugInfo(
    const BinaryData* spvBin,       // [in] SPIR-V binay code
    unsigned          bufferSize,    // Output buffer size in bytes
    void*             trimSpvBin)   // [out] Trimmed SPIR-V binary code
{
    assert(bufferSize > sizeof(SpirvHeader));

    const unsigned* code = reinterpret_cast<const unsigned*>(spvBin->pCode);
    const unsigned* end = code + spvBin->codeSize / sizeof(unsigned);
    const unsigned* codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

    unsigned* trimEnd = reinterpret_cast<unsigned*>(voidPtrInc(trimSpvBin, bufferSize));
    (void(trimEnd)); // unused
    unsigned* trimCodePos = reinterpret_cast<unsigned*>(voidPtrInc(trimSpvBin, sizeof(SpirvHeader)));

    // Copy SPIR-V header
    memcpy(trimSpvBin, code, sizeof(SpirvHeader));

    // Copy SPIR-V instructions
    while (codePos < end)
    {
        unsigned opCode = (codePos[0] & OpCodeMask);
        unsigned wordCount = (codePos[0] >> WordCountShift);
        switch (opCode)
        {
        case OpString:
        case OpSource:
        case OpSourceContinued:
        case OpSourceExtension:
        case OpName:
        case OpMemberName:
        case OpLine:
        case OpNop:
        case OpNoLine:
        case OpModuleProcessed:
            {
                // Skip debug instructions
                break;
            }
        default:
            {
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

    assert(trimCodePos == trimEnd);
}

// =====================================================================================================================
// Optimizes SPIR-V binary
Result ShaderModuleHelper::optimizeSpirv(
    const BinaryData* spirvBinIn,     // [in] Input SPIR-V binary
    BinaryData*       spirvBinOut)    // [out] Optimized SPIR-V binary
{
    bool success = false;
    unsigned optBinSize = 0;
    void* optBin = nullptr;

#ifdef LLPC_ENABLE_SPIRV_OPT
    if (cl::EnableSpirvOpt)
    {
        char logBuf[4096] = {};
        success = spvOptimizeSpirv(pSpirvBinIn->codeSize,
                                   pSpirvBinIn->pCode,
                                   0,
                                   nullptr,
                                   &optBinSize,
                                   &pOptBin,
                                   4096,
                                   logBuf);
        if (success == false)
        {
            LLPC_ERROR("Failed to optimize SPIR-V (" <<
                       GetShaderStageName(static_cast<ShaderStage>(shaderStage) << " shader): " << logBuf);
        }
    }
#endif

    if (success)
    {
        spirvBinOut->codeSize = optBinSize;
        spirvBinOut->pCode = optBin;
    }
    else
    {
        spirvBinOut->codeSize = 0;
        spirvBinOut->pCode = nullptr;
    }

    return success ? Result::Success : Result::ErrorInvalidShader;
}

// =====================================================================================================================
// Cleanup work for SPIR-V binary, freeing the allocated buffer by OptimizeSpirv()
void ShaderModuleHelper::cleanOptimizedSpirv(
    BinaryData* spirvBin)  // [in] Optimized SPIR-V binary
{
#ifdef LLPC_ENABLE_SPIRV_OPT
    if (pSpirvBin->pCode)
    {
        spvFreeBuffer(const_cast<void*>(pSpirvBin->pCode));
    }
#endif
}

// =====================================================================================================================
// Gets the shader stage mask from the SPIR-V binary according to the specified entry-point.
//
// Returns 0 on error, or the stage mask of the specified entry-point on success.
unsigned ShaderModuleHelper::getStageMaskFromSpirvBinary(
    const BinaryData* spvBin,      // [in] SPIR-V binary
    const char*       entryName)   // [in] Name of entry-point
{
    unsigned stageMask = 0;

    const unsigned* code = reinterpret_cast<const unsigned*>(spvBin->pCode);
    const unsigned* end = code + spvBin->codeSize / sizeof(unsigned);

    if (isSpirvBinary(spvBin))
    {
        // Skip SPIR-V header
        const unsigned* codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

        while (codePos < end)
        {
            unsigned opCode = (codePos[0] & OpCodeMask);
            unsigned wordCount = (codePos[0] >> WordCountShift);

            if ((wordCount == 0) || (codePos + wordCount > end))
            {
                LLPC_ERRS("Invalid SPIR-V binary\n");
                stageMask = 0;
                break;
            }

            if (opCode == OpEntryPoint)
            {
                assert(wordCount >= 4);

                // The fourth word is start of the name string of the entry-point
                const char* name = reinterpret_cast<const char*>(&codePos[3]);
                if (strcmp(entryName, name) == 0)
                {
                    // An matching entry-point is found
                    stageMask |= shaderStageToMask(convertToStageShage(codePos[1]));
                }
            }

            // All "OpEntryPoint" are before "OpFunction"
            if (opCode == OpFunction)
                break;

            codePos += wordCount;
        }
    }
    else
    {
        LLPC_ERRS("Invalid SPIR-V binary\n");
    }

    return stageMask;
}

// =====================================================================================================================
// Gets the entry-point name from the SPIR-V binary
//
// NOTE: This function is for single entry-point. If the SPIR-V binary contains multiple entry-points, we get the name
// of the first entry-point and ignore others.
const char* ShaderModuleHelper::getEntryPointNameFromSpirvBinary(
    const BinaryData* spvBin) // [in] SPIR-V binary
{
    const char* entryName = nullptr;

    const unsigned* code = reinterpret_cast<const unsigned*>(spvBin->pCode);
    const unsigned* end = code + spvBin->codeSize / sizeof(unsigned);

    if (isSpirvBinary(spvBin))
    {
        // Skip SPIR-V header
        const unsigned* codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

        while (codePos < end)
        {
            unsigned opCode = (codePos[0] & OpCodeMask);
            unsigned wordCount = (codePos[0] >> WordCountShift);

            if ((wordCount == 0) || (codePos + wordCount > end))
            {
                LLPC_ERRS("Invalid SPIR-V binary\n");
                break;
            }

            if (opCode == OpEntryPoint)
            {
                assert(wordCount >= 4);

                // The fourth word is start of the name string of the entry-point
                entryName = reinterpret_cast<const char*>(&codePos[3]);
                break;
            }

            // All "OpEntryPoint" are before "OpFunction"
            if (opCode == OpFunction)
                break;

            codePos += wordCount;
        }

        if (!entryName )
        {
            LLPC_ERRS("Entry-point not found\n");
            entryName = "";
        }
    }
    else
    {
        LLPC_ERRS("Invalid SPIR-V binary\n");
        entryName = "";
    }

    return entryName;
}

// =====================================================================================================================
// Verifies if the SPIR-V binary is valid and is supported
Result ShaderModuleHelper::verifySpirvBinary(
    const BinaryData* spvBin)  // [in] SPIR-V binary
{
    Result result = Result::Success;

#define _SPIRV_OP(x,...) Op##x,
    static const std::set<Op> OpSet{
       {
        #include "SPIRVOpCodeEnum.h"
       }
    };
#undef _SPIRV_OP

    const unsigned* code = reinterpret_cast<const unsigned*>(spvBin->pCode);
    const unsigned* end = code + spvBin->codeSize / sizeof(unsigned);

    // Skip SPIR-V header
    const unsigned* codePos = code + sizeof(SpirvHeader) / sizeof(unsigned);

    while (codePos < end)
    {
        Op opCode = static_cast<Op>(codePos[0] & OpCodeMask);
        unsigned wordCount = (codePos[0] >> WordCountShift);

        if ((wordCount == 0) || (codePos + wordCount > end))
        {
            result = Result::ErrorInvalidShader;
            break;
        }

        if (OpSet.find(opCode) == OpSet.end())
        {
            result = Result::ErrorInvalidShader;
            break;
        }

        codePos += wordCount;
    }

    return result;
}

// =====================================================================================================================
// Checks whether input binary data is SPIR-V binary
bool ShaderModuleHelper::isSpirvBinary(
    const BinaryData* shaderBin)  // [in] Shader binary codes
{
    bool isSpvBinary = false;
    if (shaderBin->codeSize > sizeof(SpirvHeader))
    {
        const SpirvHeader* header = reinterpret_cast<const SpirvHeader*>(shaderBin->pCode);
        if ((header->magicNumber == MagicNumber) && (header->spvVersion <= spv::Version) && (header->reserved == 0))
            isSpvBinary = true;
    }

    return isSpvBinary;
}

// =====================================================================================================================
// Checks whether input binary data is LLVM bitcode.
bool ShaderModuleHelper::isLlvmBitcode(
    const BinaryData* shaderBin)  // [in] Shader binary codes
{
    static unsigned BitcodeMagicNumber = 0xDEC04342; // 0x42, 0x43, 0xC0, 0xDE
    bool isLlvmBitcode = false;
    if ((shaderBin->codeSize > 4) &&
        (*reinterpret_cast<const unsigned*>(shaderBin->pCode) == BitcodeMagicNumber))
        isLlvmBitcode = true;

    return isLlvmBitcode;
}

} // Llpc
