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
  * @file  llpcShaderModuleHelper.h
  * @brief LLPC header file: contains the definition of LLPC utility class ShaderModuleHelper.
  ***********************************************************************************************************************
  */

#pragma once
#include <vector>
#include "llpc.h"

namespace Llpc
{

// Enumerates types of shader binary.
enum class BinaryType : uint32_t
{
    Unknown = 0,  // Invalid type
    Spirv,        // SPIR-V binary
    LlvmBc,       // LLVM bitcode
    MultiLlvmBc,  // Multiple LLVM bitcode
    Elf,          // ELF
};

// Represents the special header of SPIR-V token stream (the first DWORD).
struct SpirvHeader
{
    uint32_t    magicNumber;        // Magic number of SPIR-V module
    uint32_t    spvVersion;         // SPIR-V version number
    uint32_t    genMagicNumber;     // Generator's magic number
    uint32_t    idBound;            // Upbound (X) of all IDs used in SPIR-V (0 < ID < X)
    uint32_t    reserved;           // Reserved word
};

// Represents the information of one shader entry in ShaderModuleData
struct ShaderModuleEntry
{
    ShaderStage stage;              // Shader stage
    uint32_t    entryNameHash[4];   // Hash code of entry name
    uint32_t    entryOffset;        // Byte offset of the entry data in the binCode of ShaderModuleData
    uint32_t    entrySize;          // Byte size of the entry data
    uint32_t    resUsageSize;       // Byte size of the resource usage
                                    // NOTE: It should be removed after we move all necessary resUsage info to
                                    // LLVM module metadata
    uint32_t    passIndex;          // Indices of passes, It is only for internal debug.
};

// Represents the name map <stage, name> of shader entry-point
struct ShaderEntryName
{
    ShaderStage stage;             // Shader stage
    const char* pName;             // Entry name
};

// Represents the information of a shader module
struct ShaderModuleInfo
{
    uint32_t              cacheHash[4];            // hash code for calculate pipeline cache key
    uint32_t              debugInfoSize;           // Byte size of debug instructions
    bool                  enableVarPtrStorageBuf;  // Whether to enable "VariablePointerStorageBuffer" capability
    bool                  enableVarPtr;            // Whether to enable "VariablePointer" capability
    bool                  useSubgroupSize;         // Whether gl_SubgroupSize is used
    bool                  useHelpInvocation;       // Whether fragment shader has helper-invocation for subgroup
    bool                  useSpecConstant;         // Whether specializaton constant is used
    bool                  keepUnusedFunctions;     // Whether to keep unused function
    uint32_t              entryCount;              // Entry count in the module
    ShaderModuleEntry     entries[1];              // Array of all entries
};

// Represents output data of building a shader module.
struct ShaderModuleData : public ShaderModuleDataHeader
{
    BinaryType       binType;                 // Shader binary type
    BinaryData       binCode;                 // Shader binary data
    ShaderModuleInfo moduleInfo;              // Shader module info
};

// =====================================================================================================================
// Represents LLPC shader module helper class
class ShaderModuleHelper
{
public:
    static Result CollectInfoFromSpirvBinary(
        const BinaryData*             pSpvBinCode,
        ShaderModuleInfo*             pShaderModuleInfo,
        std::vector<ShaderEntryName>& shaderEntryNames);

    static void TrimSpirvDebugInfo(
        const BinaryData* pSpvBin,
        uint32_t          bufferSize,
        void*             pTrimSpvBin);

    static Result OptimizeSpirv(
        const BinaryData* pSpirvBinIn,
        BinaryData*       pSpirvBinOut);

    static void CleanOptimizedSpirv(BinaryData* pSpirvBin);

    static uint32_t GetStageMaskFromSpirvBinary(
        const BinaryData* pSpvBin,
        const char*       pEntryName);

    static const char* GetEntryPointNameFromSpirvBinary(
        const BinaryData* pSpvBin);

    static Result VerifySpirvBinary(const BinaryData* pSpvBin);

    static bool IsSpirvBinary(const BinaryData* pShaderBin);

    static bool IsLlvmBitcode(const BinaryData* pShaderBin);
};

} // Llpc
