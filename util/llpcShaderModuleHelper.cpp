/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include <unordered_set>
#include <set>

#include "llvm/Support/raw_ostream.h"
using namespace llvm;

using namespace spv;

namespace Llpc
{
// =====================================================================================================================
// Collect information from SPIR-V binary
Result ShaderModuleHelper::CollectInfoFromSpirvBinary(
    const BinaryData*                pSpvBinCode,           // [in] SPIR-V binary data
    ShaderModuleUsage*               pShaderModuleUsage,    // [out] Shader module usage info
    std::vector<ShaderEntryName>&    shaderEntryNames,      // [out] Entry names for this shader module
    uint32_t*                        pDebugInfoSize)        // Debug info size
{
    Result result = Result::Success;

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBinCode->pCode);
    const uint32_t* pEnd = pCode + pSpvBinCode->codeSize / sizeof(uint32_t);

    const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

    // Parse SPIR-V instructions
    std::unordered_set<uint32_t> capabilities;

    while (pCodePos < pEnd)
    {
        uint32_t opCode = (pCodePos[0] & OpCodeMask);
        uint32_t wordCount = (pCodePos[0] >> WordCountShift);

        if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
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
                LLPC_ASSERT(wordCount == 2);
                auto capability = static_cast<Capability>(pCodePos[1]);
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
                pShaderModuleUsage->useHelpInvocation = true;
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
                *pDebugInfoSize += wordCount * sizeof(uint32_t);
                break;
            }
        case OpSpecConstantTrue:
        case OpSpecConstantFalse:
        case OpSpecConstant:
        case OpSpecConstantComposite:
        case OpSpecConstantOp:
            {
                pShaderModuleUsage->useSpecConstant = true;
                break;
            }
        case OpEntryPoint:
            {
                ShaderEntryName entry = {};
                // The fourth word is start of the name string of the entry-point
                entry.pName = reinterpret_cast<const char*>(&pCodePos[3]);
                entry.stage = ConvertToStageShage(pCodePos[1]);
                shaderEntryNames.push_back(entry);
                break;
            }
        default:
            {
                break;
            }
        }
        pCodePos += wordCount;
    }

    if (capabilities.find(CapabilityVariablePointersStorageBuffer) != capabilities.end())
    {
        pShaderModuleUsage->enableVarPtrStorageBuf = true;
    }

    if (capabilities.find(CapabilityVariablePointers) != capabilities.end())
    {
        pShaderModuleUsage->enableVarPtr = true;
    }

    return result;
}

// =====================================================================================================================
// Removes all debug instructions for SPIR-V binary.
void ShaderModuleHelper::TrimSpirvDebugInfo(
    const BinaryData* pSpvBin,       // [in] SPIR-V binay code
    uint32_t          bufferSize,    // Output buffer size in bytes
    void*             pTrimSpvBin)   // [out] Trimmed SPIR-V binary code
{
    LLPC_ASSERT(bufferSize > sizeof(SpirvHeader));

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd = pCode + pSpvBin->codeSize / sizeof(uint32_t);
    const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

    uint32_t* pTrimEnd = reinterpret_cast<uint32_t*>(VoidPtrInc(pTrimSpvBin, bufferSize));
    LLPC_UNUSED(pTrimEnd);
    uint32_t* pTrimCodePos = reinterpret_cast<uint32_t*>(VoidPtrInc(pTrimSpvBin, sizeof(SpirvHeader)));

    // Copy SPIR-V header
    memcpy(pTrimSpvBin, pCode, sizeof(SpirvHeader));

    // Copy SPIR-V instructions
    while (pCodePos < pEnd)
    {
        uint32_t opCode = (pCodePos[0] & OpCodeMask);
        uint32_t wordCount = (pCodePos[0] >> WordCountShift);
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
                LLPC_ASSERT(pCodePos + wordCount <= pEnd);
                LLPC_ASSERT(pTrimCodePos + wordCount <= pTrimEnd);
                memcpy(pTrimCodePos, pCodePos, wordCount * sizeof(uint32_t));
                pTrimCodePos += wordCount;
                break;
            }
        }

        pCodePos += wordCount;
    }

    LLPC_ASSERT(pTrimCodePos == pTrimEnd);
}

// =====================================================================================================================
// Optimizes SPIR-V binary
Result ShaderModuleHelper::OptimizeSpirv(
    const BinaryData* pSpirvBinIn,     // [in] Input SPIR-V binary
    BinaryData*       pSpirvBinOut)    // [out] Optimized SPIR-V binary
{
    bool success = false;
    uint32_t optBinSize = 0;
    void* pOptBin = nullptr;

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
        pSpirvBinOut->codeSize = optBinSize;
        pSpirvBinOut->pCode = pOptBin;
    }
    else
    {
        pSpirvBinOut->codeSize = 0;
        pSpirvBinOut->pCode = nullptr;
    }

    return success ? Result::Success : Result::ErrorInvalidShader;
}

// =====================================================================================================================
// Cleanup work for SPIR-V binary, freeing the allocated buffer by OptimizeSpirv()
void ShaderModuleHelper::CleanOptimizedSpirv(
    BinaryData* pSpirvBin)  // [in] Optimized SPIR-V binary
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
uint32_t ShaderModuleHelper::GetStageMaskFromSpirvBinary(
    const BinaryData* pSpvBin,      // [in] SPIR-V binary
    const char*       pEntryName)   // [in] Name of entry-point
{
    uint32_t stageMask = 0;

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd = pCode + pSpvBin->codeSize / sizeof(uint32_t);

    if (IsSpirvBinary(pSpvBin))
    {
        // Skip SPIR-V header
        const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

        while (pCodePos < pEnd)
        {
            uint32_t opCode = (pCodePos[0] & OpCodeMask);
            uint32_t wordCount = (pCodePos[0] >> WordCountShift);

            if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
            {
                LLPC_ERRS("Invalid SPIR-V binary\n");
                stageMask = 0;
                break;
            }

            if (opCode == OpEntryPoint)
            {
                LLPC_ASSERT(wordCount >= 4);

                // The fourth word is start of the name string of the entry-point
                const char* pName = reinterpret_cast<const char*>(&pCodePos[3]);
                if (strcmp(pEntryName, pName) == 0)
                {
                    // An matching entry-point is found
                    stageMask |= ShaderStageToMask(ConvertToStageShage(pCodePos[1]));
                }
            }

            // All "OpEntryPoint" are before "OpFunction"
            if (opCode == OpFunction)
            {
                break;
            }

            pCodePos += wordCount;
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
const char* ShaderModuleHelper::GetEntryPointNameFromSpirvBinary(
    const BinaryData* pSpvBin) // [in] SPIR-V binary
{
    const char* pEntryName = nullptr;

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd = pCode + pSpvBin->codeSize / sizeof(uint32_t);

    if (IsSpirvBinary(pSpvBin))
    {
        // Skip SPIR-V header
        const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

        while (pCodePos < pEnd)
        {
            uint32_t opCode = (pCodePos[0] & OpCodeMask);
            uint32_t wordCount = (pCodePos[0] >> WordCountShift);

            if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
            {
                LLPC_ERRS("Invalid SPIR-V binary\n");
                break;
            }

            if (opCode == OpEntryPoint)
            {
                LLPC_ASSERT(wordCount >= 4);

                // The fourth word is start of the name string of the entry-point
                pEntryName = reinterpret_cast<const char*>(&pCodePos[3]);
                break;
            }

            // All "OpEntryPoint" are before "OpFunction"
            if (opCode == OpFunction)
            {
                break;
            }

            pCodePos += wordCount;
        }

        if (pEntryName == nullptr)
        {
            LLPC_ERRS("Entry-point not found\n");
            pEntryName = "";
        }
    }
    else
    {
        LLPC_ERRS("Invalid SPIR-V binary\n");
        pEntryName = "";
    }

    return pEntryName;
}

// =====================================================================================================================
// Verifies if the SPIR-V binary is valid and is supported
Result ShaderModuleHelper::VerifySpirvBinary(
    const BinaryData* pSpvBin)  // [in] SPIR-V binary
{
    Result result = Result::Success;

#define _SPIRV_OP(x,...) Op##x,
    static const std::set<Op> OpSet{
       {
        #include "SPIRVOpCodeEnum.h"
       }
    };
#undef _SPIRV_OP

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd = pCode + pSpvBin->codeSize / sizeof(uint32_t);

    // Skip SPIR-V header
    const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

    while (pCodePos < pEnd)
    {
        Op opCode = static_cast<Op>(pCodePos[0] & OpCodeMask);
        uint32_t wordCount = (pCodePos[0] >> WordCountShift);

        if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
        {
            result = Result::ErrorInvalidShader;
            break;
        }

        if (OpSet.find(opCode) == OpSet.end())
        {
            result = Result::ErrorInvalidShader;
            break;
        }

        pCodePos += wordCount;
    }

    return result;
}

// =====================================================================================================================
// Checks whether input binary data is SPIR-V binary
bool ShaderModuleHelper::IsSpirvBinary(
    const BinaryData* pShaderBin)  // [in] Shader binary codes
{
    bool isSpvBinary = false;
    if (pShaderBin->codeSize > sizeof(SpirvHeader))
    {
        const SpirvHeader* pHeader = reinterpret_cast<const SpirvHeader*>(pShaderBin->pCode);
        if ((pHeader->magicNumber == MagicNumber) && (pHeader->spvVersion <= spv::Version) && (pHeader->reserved == 0))
        {
            isSpvBinary = true;
        }
    }

    return isSpvBinary;
}

// =====================================================================================================================
// Checks whether input binary data is LLVM bitcode.
bool ShaderModuleHelper::IsLlvmBitcode(
    const BinaryData* pShaderBin)  // [in] Shader binary codes
{
    static uint32_t BitcodeMagicNumber = 0xDEC04342; // 0x42, 0x43, 0xC0, 0xDE
    bool isLlvmBitcode = false;
    if ((pShaderBin->codeSize > 4) &&
        (*reinterpret_cast<const uint32_t*>(pShaderBin->pCode) == BitcodeMagicNumber))
    {
        isLlvmBitcode = true;
    }

    return isLlvmBitcode;
}

} // Llpc
