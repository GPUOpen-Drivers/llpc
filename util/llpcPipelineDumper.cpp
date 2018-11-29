/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPipelineDumper.cpp
 * @breif LLPC source file: contains implementation of LLPC pipline dump utility.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-pipeline-dumper"

#include "llvm/Support/Mutex.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <stdarg.h>
#include <sys/stat.h>

#include <metrohash.h>

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcElf.h"
#include "llpcPipelineDumper.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"
#include "llpcUtil.h"

using namespace llvm;

#if defined(_WIN32)
    #define FILE_STAT _stat
#else
    #define FILE_STAT stat
#endif

namespace Llpc
{

// Forward declaration
std::ostream& operator<<(std::ostream& out, VkVertexInputRate  inputRate);
std::ostream& operator<<(std::ostream& out, VkFormat           format);
std::ostream& operator<<(std::ostream& out, VkPrimitiveTopology topology);
std::ostream& operator<<(std::ostream& out, ResourceMappingNodeType type);

template std::ostream& operator<<(std::ostream& out, ElfReader<Elf64>& reader);
template raw_ostream& operator<<(raw_ostream& out, ElfReader<Elf64>& reader);

// =====================================================================================================================
// Represents LLVM based mutex.
class Mutex
{
public:
    Mutex()
    {
    }

    void Lock()
    {
        m_mutex.lock();
    }

    void Unlock()
    {
        m_mutex.unlock();
    }
private:
    llvm::sys::Mutex m_mutex;
};

// Mutex for pipeline dump
static Mutex s_dumpMutex;

// =====================================================================================================================
// Represents the file objects for pipeline dump
struct PipelineDumpFile
{
    PipelineDumpFile(
        const char* pDumpFileName,
        const char* pBinaryFileName)
        :
        dumpFile(pDumpFileName),
        binaryFile(pBinaryFileName, std::ios_base::binary | std::ios_base::out)
    {
    }

    std::ofstream dumpFile;     // File object for .pipe file
    std::ofstream binaryFile;   // File object for ELF binary
};

// =====================================================================================================================
// Dumps SPIR-V shader binary to extenal file.
void VKAPI_CALL IPipelineDumper::DumpSpirvBinary(
    const char*                     pDumpDir,   // [in] Directory of pipeline dump
    const BinaryData*               pSpirvBin)  // [in] SPIR-V binary
{
    MetroHash::Hash hash = {};
    MetroHash::MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pSpirvBin->pCode),
                      pSpirvBin->codeSize,
                      hash.bytes);
    PipelineDumper::DumpSpirvBinary(pDumpDir, pSpirvBin, &hash);
}

// =====================================================================================================================
// Begins to dump graphics/compute pipeline info.
void* VKAPI_CALL IPipelineDumper::BeginPipelineDump(
    const PipelineDumpOptions*       pDumpOptions,          // [in] Pipeline dump options
    const ComputePipelineBuildInfo*  pComputePipelineInfo,  // [in] Info of the compute pipeline to be built
    const GraphicsPipelineBuildInfo* pGraphicsPipelineInfo) // [in] Info of the graphics pipeline to be built
{
    MetroHash::Hash hash = {};
    if (pComputePipelineInfo != nullptr)
    {
        hash = PipelineDumper::GenerateHashForComputePipeline(pComputePipelineInfo, false);
    }
    else
    {
        LLPC_ASSERT(pGraphicsPipelineInfo != nullptr);
        hash = PipelineDumper::GenerateHashForGraphicsPipeline(pGraphicsPipelineInfo, false);
    }
    return PipelineDumper::BeginPipelineDump(pDumpOptions, pComputePipelineInfo, pGraphicsPipelineInfo, &hash);
}

// =====================================================================================================================
// Ends to dump graphics/compute pipeline info.
void VKAPI_CALL IPipelineDumper::EndPipelineDump(
    void* pDumpFile)  // [in] The handle of pipeline dump file
{
    PipelineDumper::EndPipelineDump(reinterpret_cast<PipelineDumpFile*>(pDumpFile));
}

// =====================================================================================================================
// Disassembles pipeline binary and dumps it to pipeline info file.
void VKAPI_CALL IPipelineDumper::DumpPipelineBinary(
    void*                    pDumpFile,    // [in] The handle of pipeline dump file
    GfxIpVersion             gfxIp,        // Graphics IP version info
    const BinaryData*        pPipelineBin) // [in] Pipeline binary (ELF)
{
    PipelineDumper::DumpPipelineBinary(reinterpret_cast<PipelineDumpFile*>(pDumpFile), gfxIp, pPipelineBin);
}

// =====================================================================================================================
// Gets shader module hash code.
uint64_t VKAPI_CALL IPipelineDumper::GetShaderHash(
    const void* pModuleData)   // [in] Pointer to the shader module data
{
    const ShaderModuleDataHeader* pModule =
            reinterpret_cast<const ShaderModuleDataHeader*>(pModuleData);
    return MetroHash::Compact64(reinterpret_cast<const MetroHash::Hash*>(&pModule->hash));
}

// =====================================================================================================================
// Calculates graphics pipeline hash code.
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(
    const GraphicsPipelineBuildInfo* pPipelineInfo) // [in] Info to build this graphics pipeline
{
    auto hash = PipelineDumper::GenerateHashForGraphicsPipeline(pPipelineInfo, false);
    return MetroHash::Compact64(&hash);
}
// =====================================================================================================================
// Get graphics pipeline name.
void VKAPI_CALL IPipelineDumper::GetPipelineName(
    const  GraphicsPipelineBuildInfo* pPipelineInfo, // [In]  Info to build this graphics pipeline
    char*                             pPipeName,     // [Out] The full name of this graphics pipeline
    const size_t                      nameBufSize)   // Size of the buffer to store pipeline name
{
    auto hash = PipelineDumper::GenerateHashForGraphicsPipeline(pPipelineInfo, false);
    std::string pipeName = PipelineDumper::GetPipelineInfoFileName(nullptr, pPipelineInfo, &hash);
    snprintf(pPipeName, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Get compute pipeline name.
void VKAPI_CALL IPipelineDumper::GetPipelineName(
    const ComputePipelineBuildInfo* pPipelineInfo, // [In]  Info to build this compute pipeline
    char*                           pPipeName,     // [Out] The full name of this compute pipeline
    const size_t                    nameBufSize)   // Size of the buffer to store pipeline name
{
    auto hash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, false);
    std::string pipeName = PipelineDumper::GetPipelineInfoFileName(pPipelineInfo, nullptr, &hash);
    snprintf(pPipeName, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Calculates compute pipeline hash code.
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(
    const ComputePipelineBuildInfo* pPipelineInfo) // [in] Info to build this compute pipeline
{
    auto hash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, false);
    return MetroHash::Compact64(&hash);
}

// =====================================================================================================================
// Gets the file name of SPIR-V binary according the specified shader hash.
std::string PipelineDumper::GetSpirvBinaryFileName(
     const MetroHash::Hash* pHash)       // [in] Shader hash code
{
    uint64_t hashCode64 = MetroHash::Compact64(pHash);
    char     fileName[64] = {};
    auto     length = snprintf(fileName, 64, "Shader_0x%016" PRIX64 ".spv", hashCode64);
    LLPC_UNUSED(length);
    return std::string(fileName);
}

// =====================================================================================================================
// Gets the file name of pipeline info file according to the specified pipeline build info and pipeline hash.
std::string PipelineDumper::GetPipelineInfoFileName(
    const ComputePipelineBuildInfo*  pComputePipelineInfo,   // [in] Info of the compute pipeline to be built
    const GraphicsPipelineBuildInfo* pGraphicsPipelineInfo,  // [in] Info of the graphics pipeline to be built
    const MetroHash::Hash*           pHash)                  // [in] Pipeline hash code
{
    uint64_t        hashCode64 = MetroHash::Compact64(pHash);
    char            fileName[64] = {};
    if (pComputePipelineInfo != nullptr)
    {
        auto length = snprintf(fileName, 64, "PipelineCs_0x%016" PRIX64, hashCode64);
        LLPC_UNUSED(length);
    }
    else
    {
        LLPC_ASSERT(pGraphicsPipelineInfo != nullptr);
        const char* pFileNamePrefix = nullptr;
        if (pGraphicsPipelineInfo->tes.pModuleData != nullptr && pGraphicsPipelineInfo->gs.pModuleData != nullptr)
        {
             pFileNamePrefix = "PipelineGsTess";
        }
        else if (pGraphicsPipelineInfo->gs.pModuleData != nullptr)
        {
             pFileNamePrefix = "PipelineGs";
        }
        else if (pGraphicsPipelineInfo->tes.pModuleData != nullptr)
        {
             pFileNamePrefix = "PipelineTess";
        }
        else
        {
            pFileNamePrefix = "PipelineVsFs";
        }

        auto length = snprintf(fileName, 64, "%s_0x%016" PRIX64, pFileNamePrefix, hashCode64);
        LLPC_UNUSED(length);
    }

    return std::string(fileName);
}

// =====================================================================================================================
// Begins to dump graphics/compute pipeline info.
PipelineDumpFile* PipelineDumper::BeginPipelineDump(
    const PipelineDumpOptions*       pDumpOptions,           // [in] Pipeline dump options
    const ComputePipelineBuildInfo*  pComputePipelineInfo,   // [in] Info of the compute pipeline to be built
    const GraphicsPipelineBuildInfo* pGraphicsPipelineInfo,  // [in] Info of the graphics pipeline to be built
    const MetroHash::Hash*           pHash)                  // [in] Pipeline hash code
{
    bool disableLog = false;
    std::string dumpFileName;
    std::string dumpPathName;
    std::string dumpBinaryName;
    PipelineDumpFile* pDumpFile = nullptr;

    // Filter pipeline hash
    if (pDumpOptions->filterPipelineDumpByHash != 0)
    {
        uint64_t hash64 = MetroHash::Compact64(pHash);
        if (hash64 != pDumpOptions->filterPipelineDumpByHash)
        {
            disableLog = true;
        }
    }

    if (disableLog == false)
    {
        // Filter pipeline type
        dumpFileName = GetPipelineInfoFileName(pComputePipelineInfo, pGraphicsPipelineInfo, pHash);
        if (pDumpOptions->filterPipelineDumpByType & PipelineDumpFilterCs)
        {
            if (dumpFileName.find("Cs") != std::string::npos)
            {
                disableLog = true;
            }
        }
        if (pDumpOptions->filterPipelineDumpByType & PipelineDumpFilterGs)
        {
            if (dumpFileName.find("Gs") != std::string::npos)
            {
                disableLog = true;
            }
        }
        if (pDumpOptions->filterPipelineDumpByType & PipelineDumpFilterTess)
        {
            if (dumpFileName.find("Tess") != std::string::npos)
            {
                disableLog = true;
            }
        }
        if (pDumpOptions->filterPipelineDumpByType & PipelineDumpFilterVsPs)
        {
            if (dumpFileName.find("VsFs") != std::string::npos)
            {
                disableLog = true;
            }
        }
    }

    if (disableLog == false)
    {
        s_dumpMutex.Lock();
         // Build dump file name
        if (pDumpOptions->dumpDuplicatePipelines)
        {
            uint32_t index = 0;
            int32_t result = 0;
            while (result != -1)
            {
                dumpPathName = pDumpOptions->pDumpDir;
                dumpPathName += "/";
                dumpPathName += dumpFileName;
                if (index > 0)
                {
                    dumpPathName += "-[";
                    dumpPathName += std::to_string(index);
                    dumpPathName += "]";
                }
                dumpBinaryName = dumpPathName + ".elf";
                dumpPathName += ".pipe";
                struct FILE_STAT fileStatus = {};
                result = FILE_STAT(dumpPathName.c_str(), &fileStatus);
                ++index;
            };
        }
        else
        {
            dumpPathName = pDumpOptions->pDumpDir;
            dumpPathName += "/";
            dumpPathName += dumpFileName;
            dumpBinaryName = dumpPathName + ".elf";
            dumpPathName += ".pipe";
        }

        // Open dump file
        pDumpFile = new PipelineDumpFile(dumpPathName.c_str(), dumpBinaryName.c_str());
        if (pDumpFile->dumpFile.bad() || pDumpFile->binaryFile.bad())
        {
            delete pDumpFile;
            pDumpFile = nullptr;
            s_dumpMutex.Unlock();
        }

        // Create the dump directory
        CreateDirectory(pDumpOptions->pDumpDir);

        // Dump pipeline input info
        if (pComputePipelineInfo)
        {
            DumpComputePipelineInfo(&pDumpFile->dumpFile, pComputePipelineInfo);
        }

        if (pGraphicsPipelineInfo)
        {
            DumpGraphicsPipelineInfo(&pDumpFile->dumpFile, pGraphicsPipelineInfo);
        }
    }
    return pDumpFile;
}

// =====================================================================================================================
// Ends to dump graphics/compute pipeline info.
void PipelineDumper::EndPipelineDump(
    PipelineDumpFile* pDumpFile) // [in] Dump file
{
    delete pDumpFile;
    s_dumpMutex.Unlock();
}

// =====================================================================================================================
// Dumps resource mapping node to dumpFile.
void PipelineDumper::DumpResourceMappingNode(
    const ResourceMappingNode* pUserDataNode,    // [in] User data nodes to be dumped
    const char*                pPrefix,          // [in] Prefix string for each line
    std::ostream&              dumpFile)         // [out] dump file
{
    dumpFile << pPrefix << ".type = " << pUserDataNode->type << "\n";
    dumpFile << pPrefix << ".offsetInDwords = " << pUserDataNode->offsetInDwords << "\n";
    dumpFile << pPrefix << ".sizeInDwords = " << pUserDataNode->sizeInDwords << "\n";

    switch (pUserDataNode->type)
    {
    case ResourceMappingNodeType::DescriptorResource:
    case ResourceMappingNodeType::DescriptorSampler:
    case ResourceMappingNodeType::DescriptorCombinedTexture:
    case ResourceMappingNodeType::DescriptorTexelBuffer:
    case ResourceMappingNodeType::DescriptorBuffer:
    case ResourceMappingNodeType::DescriptorFmask:
    case ResourceMappingNodeType::DescriptorBufferCompact:
        {
            dumpFile << pPrefix << ".set = " << pUserDataNode->srdRange.set << "\n";
            dumpFile << pPrefix << ".binding = " << pUserDataNode->srdRange.binding << "\n";
            break;
        }
    case ResourceMappingNodeType::DescriptorTableVaPtr:
        {
            char prefixBuf[256];
            int32_t length = 0;
            for (uint32_t i = 0; i < pUserDataNode->tablePtr.nodeCount; ++i)
            {
                length = snprintf(prefixBuf, 256, "%s.next[%u]", pPrefix, i);
                DumpResourceMappingNode(pUserDataNode->tablePtr.pNext + i, prefixBuf, dumpFile);
            }
            break;
        }
    case ResourceMappingNodeType::IndirectUserDataVaPtr:
        {
            dumpFile << pPrefix << ".indirectUserDataCount = " << pUserDataNode->userDataPtr.sizeInDwords << "\n";
            break;
        }
    case ResourceMappingNodeType::StreamOutTableVaPtr:
        {
            break;
        }
    case ResourceMappingNodeType::PushConst:
        {
            dumpFile << pPrefix << ".set = " << pUserDataNode->srdRange.set << "\n";
            dumpFile << pPrefix << ".binding = " << pUserDataNode->srdRange.binding << "\n";
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Dumps pipeline shader info to file.
void PipelineDumper::DumpPipelineShaderInfo(
    ShaderStage               stage,       // Shader stage
    const PipelineShaderInfo* pShaderInfo, // [in] Shader info of specified shader stage
    std::ostream&             dumpFile)    // [out] dump file
{
    const ShaderModuleDataHeader* pModuleData = reinterpret_cast<const ShaderModuleDataHeader*>(pShaderInfo->pModuleData);
    auto pModuleHash = reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash[0]);

    // Output shader binary file
    dumpFile << "[" << GetShaderStageAbbreviation(stage) << "SpvFile]\n";
    dumpFile << "fileName = " << GetSpirvBinaryFileName(pModuleHash) << "\n\n";

    dumpFile << "[" << GetShaderStageAbbreviation(stage) << "Info]\n";
    // Output entry point
    if (pShaderInfo->pEntryTarget != nullptr)
    {
         dumpFile << "entryPoint = " << pShaderInfo->pEntryTarget << "\n";
    }

    // Output specialize info
    if (pShaderInfo->pSpecializationInfo)
    {
        auto pSpecializationInfo = pShaderInfo->pSpecializationInfo;
        for (uint32_t i = 0; i < pSpecializationInfo->mapEntryCount; ++i)
        {
            dumpFile << "specConst.mapEntry[" << i << "].constantID = " << pSpecializationInfo->pMapEntries[i].constantID << "\n";
            dumpFile << "specConst.mapEntry[" << i << "].offset = " << pSpecializationInfo->pMapEntries[i].offset << "\n";
            dumpFile << "specConst.mapEntry[" << i << "].size = " << pSpecializationInfo->pMapEntries[i].size << "\n";
        }
        const uint32_t* pData = reinterpret_cast<const uint32_t*>(pSpecializationInfo->pData);
        for (uint32_t i = 0; i < (pSpecializationInfo->dataSize + sizeof(uint32_t) - 1) / sizeof(uint32_t); ++i)
        {
            if ((i % 8) == 0)
            {
                dumpFile << "specConst.uintData = ";
            }
            dumpFile << pData[i];
            if ((i % 8) == 7)
            {
                dumpFile << "\n";
            }
            else
            {
                dumpFile << ", ";
            }
        }
        dumpFile << "\n";
    }

    // Output descriptor range value
    if (pShaderInfo->descriptorRangeValueCount > 0)
    {
        for (uint32_t i = 0; i < pShaderInfo->descriptorRangeValueCount; ++i)
        {
            auto pDescriptorRangeValue = &pShaderInfo->pDescriptorRangeValues[i];
            dumpFile << "descriptorRangeValue[" << i << "].type = " << pDescriptorRangeValue->type << "\n";
            dumpFile << "descriptorRangeValue[" << i << "].set = " << pDescriptorRangeValue->set << "\n";
            dumpFile << "descriptorRangeValue[" << i << "].binding = " << pDescriptorRangeValue->binding << "\n";
            dumpFile << "descriptorRangeValue[" << i << "].arraySize = " << pDescriptorRangeValue->arraySize << "\n";
            for (uint32_t j = 0; j < pDescriptorRangeValue->arraySize; ++j)
            {
                dumpFile << "descriptorRangeValue[" << i << "].uintData = ";
                const uint32_t DescriptorSizeInDw = 4;

                for (uint32_t k = 0; k < DescriptorSizeInDw -1; ++k)
                {
                     dumpFile << pDescriptorRangeValue->pValue[i] << ", ";
                }
                dumpFile << pDescriptorRangeValue->pValue[DescriptorSizeInDw - 1] << "\n";
            }
        }
        dumpFile << "\n";
    }

    // Output resource node mapping
    if (pShaderInfo->userDataNodeCount > 0)
    {
        char prefixBuff[64];
        for (uint32_t i = 0; i < pShaderInfo->userDataNodeCount; ++i)
        {
            auto pUserDataNode = &pShaderInfo->pUserDataNodes[i];
            auto length = snprintf(prefixBuff, 64, "userDataNode[%u]", i);
            LLPC_UNUSED(length);
            DumpResourceMappingNode(pUserDataNode, prefixBuff, dumpFile);
        }
        dumpFile << "\n";
    }

    // Output pipeline shader options
    dumpFile << "trapPresent = " << pShaderInfo->options.trapPresent << "\n";
    dumpFile << "debugMode = " << pShaderInfo->options.debugMode << "\n";
    dumpFile << "enablePerformanceData = " << pShaderInfo->options.enablePerformanceData << "\n";
    dumpFile << "allowReZ = " << pShaderInfo->options.allowReZ << "\n";
    dumpFile << "vgprLimit = " << pShaderInfo->options.vgprLimit << "\n";
    dumpFile << "sgprLimit = " << pShaderInfo->options.sgprLimit << "\n";
    dumpFile << "maxThreadGroupsPerComputeUnit = " << pShaderInfo->options.maxThreadGroupsPerComputeUnit << "\n";
    dumpFile << "\n";
}

// =====================================================================================================================
// Dumps SPIR-V shader binary to extenal file
void PipelineDumper::DumpSpirvBinary(
    const char*                     pDumpDir,     // [in] Directory of pipeline dump
    const BinaryData*               pSpirvBin,    // [in] SPIR-V binary
    MetroHash::Hash*                pHash)        // [in] Pipeline hash code
{
    std::string pathName = pDumpDir;
    pathName += "/";
    pathName += GetSpirvBinaryFileName(pHash);

    // Open dumpfile
    std::ofstream dumpFile(pathName.c_str(), std::ios_base::binary | std::ios_base::out);
    if (dumpFile.bad() == false)
    {
        dumpFile.write(reinterpret_cast<const char*>(pSpirvBin->pCode), pSpirvBin->codeSize);
    }
}

// =====================================================================================================================
// Disassembles pipeline binary and dumps it to pipeline info file.
void PipelineDumper::DumpPipelineBinary(
    PipelineDumpFile*                pDumpFile,              // [in] Directory of pipeline dump
    GfxIpVersion                     gfxIp,                  // Graphics IP version info
    const BinaryData*                pPipelineBin)           // [in] Pipeline binary (ELF)
{
    ElfReader<Elf64> reader(gfxIp);
    size_t codeSize = pPipelineBin->codeSize;
    auto result = reader.ReadFromBuffer(pPipelineBin->pCode, &codeSize);
    LLPC_ASSERT(result == Result::Success);
    LLPC_UNUSED(result);

    pDumpFile->dumpFile << "\n[CompileLog]\n";
    pDumpFile->dumpFile << reader;

    pDumpFile->binaryFile.write(reinterpret_cast<const char*>(pPipelineBin->pCode), pPipelineBin->codeSize);
}

// =====================================================================================================================
// Dump extra info to pipeline file.
void PipelineDumper::DumpPipelineExtraInfo(
    PipelineDumpFile*             pDumpFile,               // [in] Directory of pipeline dump
    const std::string*            pStr)                     // [in] Extra info string
{
    pDumpFile->dumpFile << *pStr;
}

// =====================================================================================================================
// Dumps LLPC version info to file
void PipelineDumper::DumpVersionInfo(
    std::ostream&                  dumpFile)      // [out] dump file
{
    dumpFile << "[Version]\n";
    dumpFile << "version = " << Version << "\n\n";
}

// =====================================================================================================================
// Dumps compute pipeline state info to file.
void PipelineDumper::DumpComputeStateInfo(
    const ComputePipelineBuildInfo* pPipelineInfo,  // [in] Info of the graphics pipeline to be built
    std::ostream&                   dumpFile)      // [out] dump file
{
    dumpFile << "[ComputePipelineState]\n";

    // Output pipeline states
    dumpFile << "deviceIndex = " << pPipelineInfo->deviceIndex << "\n";
    dumpFile << "includeDisassembly = " << pPipelineInfo->options.includeDisassembly << "\n";
    dumpFile << "autoLayoutDesc = " << pPipelineInfo->options.autoLayoutDesc << "\n";
}

// =====================================================================================================================
// Dumps compute pipeline information to file.
void PipelineDumper::DumpComputePipelineInfo(
    std::ostream*                   pDumpFile,         // [in] Pipeline dump file
    const ComputePipelineBuildInfo* pPipelineInfo)     // [in] Info of the compute pipeline to be built
{
    DumpVersionInfo(*pDumpFile);

    // Output shader info
    DumpPipelineShaderInfo(ShaderStageCompute, &pPipelineInfo->cs, *pDumpFile);
    DumpComputeStateInfo(pPipelineInfo, *pDumpFile);

    pDumpFile->flush();
}

// =====================================================================================================================
// Dumps graphics pipeline state info to file.
void PipelineDumper::DumpGraphicsStateInfo(
    const GraphicsPipelineBuildInfo* pPipelineInfo, // [in] Info of the graphics pipeline to be built
    std::ostream&                    dumpFile)      // [out] dump file
{
    dumpFile << "[GraphicsPipelineState]\n";

    // Output pipeline states
    dumpFile << "topology = " << pPipelineInfo->iaState.topology << "\n";
    dumpFile << "patchControlPoints = " << pPipelineInfo->iaState.patchControlPoints << "\n";
    dumpFile << "deviceIndex = " << pPipelineInfo->iaState.deviceIndex << "\n";
    dumpFile << "disableVertexReuse = " << pPipelineInfo->iaState.disableVertexReuse << "\n";
    dumpFile << "switchWinding = " << pPipelineInfo->iaState.switchWinding << "\n";
    dumpFile << "enableMultiView = " << pPipelineInfo->iaState.enableMultiView << "\n";
    dumpFile << "depthClipEnable = " << pPipelineInfo->vpState.depthClipEnable << "\n";

    dumpFile << "rasterizerDiscardEnable = " << pPipelineInfo->rsState.rasterizerDiscardEnable << "\n";
    dumpFile << "perSampleShading = " << pPipelineInfo->rsState.perSampleShading << "\n";
    dumpFile << "numSamples = " << pPipelineInfo->rsState.numSamples << "\n";
    dumpFile << "samplePatternIdx = " << pPipelineInfo->rsState.samplePatternIdx << "\n";
    dumpFile << "usrClipPlaneMask = " << static_cast<uint32_t>(pPipelineInfo->rsState.usrClipPlaneMask) << "\n";
    dumpFile << "polygonMode = " << pPipelineInfo->rsState.polygonMode << "\n";
    dumpFile << "cullMode = " << pPipelineInfo->rsState.cullMode << "\n";
    dumpFile << "frontFace = " << pPipelineInfo->rsState.frontFace << "\n";
    dumpFile << "depthBiasEnable = " << pPipelineInfo->rsState.depthBiasEnable << "\n";

    dumpFile << "includeDisassembly = " << pPipelineInfo->options.includeDisassembly << "\n";
    dumpFile << "autoLayoutDesc = " << pPipelineInfo->options.autoLayoutDesc << "\n";

    dumpFile << "alphaToCoverageEnable = " << pPipelineInfo->cbState.alphaToCoverageEnable << "\n";
    dumpFile << "dualSourceBlendEnable = " << pPipelineInfo->cbState.dualSourceBlendEnable << "\n";

    for (uint32_t i = 0; i < MaxColorTargets; ++i)
    {
        if (pPipelineInfo->cbState.target[i].format != VK_FORMAT_UNDEFINED)
        {
            auto pCbTarget = &pPipelineInfo->cbState.target[i];
            dumpFile << "colorBuffer[" << i << "].format = " << pCbTarget->format << "\n";
            dumpFile << "colorBuffer[" << i << "].channelWriteMask = " << static_cast<uint32_t>(pCbTarget->channelWriteMask) << "\n";
            dumpFile << "colorBuffer[" << i << "].blendEnable = " << pCbTarget->blendEnable << "\n";
            dumpFile << "colorBuffer[" << i << "].blendSrcAlphaToColor = " << pCbTarget->blendSrcAlphaToColor << "\n";
        }
    }
    dumpFile << "\n\n";

    // Output vertex input state
    if (pPipelineInfo->pVertexInput &&
        (pPipelineInfo->pVertexInput->vertexBindingDescriptionCount > 0))
    {
        dumpFile << "[VertexInputState]\n";
        for (uint32_t i = 0; i < pPipelineInfo->pVertexInput->vertexBindingDescriptionCount; ++i)
        {
            auto pBinding = &pPipelineInfo->pVertexInput->pVertexBindingDescriptions[i];
            dumpFile << "binding[" << i << "].binding = " << pBinding->binding << "\n";
            dumpFile << "binding[" << i << "].stride = " << pBinding->stride << "\n";
            dumpFile << "binding[" << i << "].inputRate = " << pBinding->inputRate << "\n";
        }

        for (uint32_t i = 0; i < pPipelineInfo->pVertexInput->vertexAttributeDescriptionCount; ++i)
        {
            auto pAttrib = &pPipelineInfo->pVertexInput->pVertexAttributeDescriptions[i];
            dumpFile << "attribute[" << i << "].location = " << pAttrib->location << "\n";
            dumpFile << "attribute[" << i << "].binding = " << pAttrib->binding << "\n";
            dumpFile << "attribute[" << i << "].format = " << pAttrib->format << "\n";
            dumpFile << "attribute[" << i << "].offset = " << pAttrib->offset << "\n";
        }

        auto pDivisorState= FindVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
            pPipelineInfo->pVertexInput->pNext);

        for (uint32_t i = 0; pDivisorState != nullptr && i < pDivisorState->vertexBindingDivisorCount; ++i)
        {
            auto pDivisor = &pDivisorState->pVertexBindingDivisors[i];
            dumpFile << "divisor[" << i << "].binding = " << pDivisor->binding << "\n";
            dumpFile << "divisor[" << i << "].divisor = " << pDivisor->divisor << "\n";
        }
    }
}

// =====================================================================================================================
// Dumps graphics pipeline build info to file.
void PipelineDumper::DumpGraphicsPipelineInfo(
    std::ostream*                    pDumpFile,       // [in] Pipeline dump file
    const GraphicsPipelineBuildInfo* pPipelineInfo)   // [in] Info of the graphics pipeline to be built
{
    DumpVersionInfo(*pDumpFile);
    // Dump pipeline
    const PipelineShaderInfo* shaderInfo[ShaderStageGfxCount] =
    {
        &pPipelineInfo->vs,
        &pPipelineInfo->tcs,
        &pPipelineInfo->tes,
        &pPipelineInfo->gs,
        &pPipelineInfo->fs,
    };

    for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
    {
        const PipelineShaderInfo* pShaderInfo = shaderInfo[stage];
        if (pShaderInfo->pModuleData == nullptr)
        {
            continue;
        }
        DumpPipelineShaderInfo(static_cast<ShaderStage>(stage), pShaderInfo, *pDumpFile);
    }

    DumpGraphicsStateInfo(pPipelineInfo, *pDumpFile);

    pDumpFile->flush();
}

// =====================================================================================================================
// Builds hash code from graphics pipline build info.
MetroHash::Hash PipelineDumper::GenerateHashForGraphicsPipeline(
    const GraphicsPipelineBuildInfo* pPipeline,   // [in] Info to build a graphics pipeline
    bool                            isCacheHash   // TRUE if the hash is used by shader cache
    )
{
    MetroHash::MetroHash64 hasher;

    UpdateHashForPipelineShaderInfo(ShaderStageVertex, &pPipeline->vs, isCacheHash, &hasher);
    UpdateHashForPipelineShaderInfo(ShaderStageTessControl, &pPipeline->tcs, isCacheHash, &hasher);
    UpdateHashForPipelineShaderInfo(ShaderStageTessEval, &pPipeline->tes, isCacheHash, &hasher);
    UpdateHashForPipelineShaderInfo(ShaderStageGeometry, &pPipeline->gs, isCacheHash, &hasher);
    UpdateHashForPipelineShaderInfo(ShaderStageFragment, &pPipeline->fs, isCacheHash, &hasher);

    if ((pPipeline->pVertexInput != nullptr) && (pPipeline->pVertexInput->vertexBindingDescriptionCount > 0))
    {
        auto pVertexInput = pPipeline->pVertexInput;
        hasher.Update(pVertexInput->vertexBindingDescriptionCount);
        hasher.Update(reinterpret_cast<const uint8_t*>(pVertexInput->pVertexBindingDescriptions),
                      sizeof(VkVertexInputBindingDescription) * pVertexInput->vertexBindingDescriptionCount);
        hasher.Update(pVertexInput->vertexAttributeDescriptionCount);
        if (pVertexInput->vertexAttributeDescriptionCount > 0)
        {
            hasher.Update(reinterpret_cast<const uint8_t*>(pVertexInput->pVertexAttributeDescriptions),
                      sizeof(VkVertexInputAttributeDescription) * pVertexInput->vertexAttributeDescriptionCount);
        }

        auto pVertexDivisor = FindVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
            pVertexInput->pNext);
        uint32_t divisorCount = (pVertexDivisor != nullptr) ? pVertexDivisor->vertexBindingDivisorCount : 0;
        hasher.Update(divisorCount);
        if (divisorCount > 0)
        {
            hasher.Update(reinterpret_cast<const uint8_t*>(pVertexDivisor->pVertexBindingDivisors),
                sizeof(VkVertexInputBindingDivisorDescriptionEXT) * divisorCount);
        }
    }

    auto pIaState = &pPipeline->iaState;
    hasher.Update(pIaState->topology);
    hasher.Update(pIaState->patchControlPoints);
    hasher.Update(pIaState->deviceIndex);
    hasher.Update(pIaState->disableVertexReuse);
    hasher.Update(pIaState->switchWinding);
    hasher.Update(pIaState->enableMultiView);

    auto pVpState = &pPipeline->vpState;
    hasher.Update(pVpState->depthClipEnable);

    auto pRsState = &pPipeline->rsState;
    hasher.Update(pRsState->rasterizerDiscardEnable);
    hasher.Update(pRsState->innerCoverage);
    hasher.Update(pRsState->perSampleShading);
    hasher.Update(pRsState->numSamples);
    hasher.Update(pRsState->samplePatternIdx);
    hasher.Update(pRsState->usrClipPlaneMask);
    hasher.Update(pRsState->polygonMode);
    hasher.Update(pRsState->cullMode);
    hasher.Update(pRsState->frontFace);
    hasher.Update(pRsState->depthBiasEnable);

    auto pCbState = &pPipeline->cbState;
    hasher.Update(pCbState->alphaToCoverageEnable);
    hasher.Update(pCbState->dualSourceBlendEnable);
    for (uint32_t i = 0; i < MaxColorTargets; ++i)
    {
        if (pCbState->target[i].format != VK_FORMAT_UNDEFINED)
        {
            hasher.Update(pCbState->target[i].channelWriteMask);
            hasher.Update(pCbState->target[i].blendEnable);
            hasher.Update(pCbState->target[i].blendSrcAlphaToColor);
            hasher.Update(pCbState->target[i].format);
        }
    }

    hasher.Update(pPipeline->options.includeDisassembly);
    hasher.Update(pPipeline->options.autoLayoutDesc);

    MetroHash::Hash hash = {};
    hasher.Finalize(hash.bytes);

    return hash;
}

// =====================================================================================================================
// Builds hash code from compute pipline build info.
MetroHash::Hash PipelineDumper::GenerateHashForComputePipeline(
    const ComputePipelineBuildInfo* pPipeline,   // [in] Info to build a compute pipeline
    bool                            isCacheHash  // TRUE if the hash is used by shader cache
    )
{
    MetroHash::MetroHash64 hasher;

    UpdateHashForPipelineShaderInfo(ShaderStageCompute, &pPipeline->cs, isCacheHash, &hasher);
    hasher.Update(pPipeline->deviceIndex);
    hasher.Update(pPipeline->options.includeDisassembly);
    hasher.Update(pPipeline->options.autoLayoutDesc);

    MetroHash::Hash hash = {};
    hasher.Finalize(hash.bytes);

    return hash;
}

// =====================================================================================================================
// Updates hash code context for pipeline shader stage.
void PipelineDumper::UpdateHashForPipelineShaderInfo(
    ShaderStage               stage,           // shader stage
    const PipelineShaderInfo* pShaderInfo,     // [in] Shader info in specified shader stage
    bool                      isCacheHash,     // TRUE if the hash is used by shader cache
    MetroHash::MetroHash64*   pHasher          // [in,out] Haher to generate hash code
    )
{
    if (pShaderInfo->pModuleData)
    {
        const ShaderModuleDataHeader* pModuleData =
            reinterpret_cast<const ShaderModuleDataHeader*>(pShaderInfo->pModuleData);
        pHasher->Update(stage);
        pHasher->Update(pModuleData->hash);

        size_t entryNameLen = 0;
        if (pShaderInfo->pEntryTarget)
        {
            entryNameLen = strlen(pShaderInfo->pEntryTarget);
            pHasher->Update(entryNameLen);
            pHasher->Update(reinterpret_cast<const uint8_t*>(pShaderInfo->pEntryTarget), entryNameLen);
        }
        else
        {
            pHasher->Update(entryNameLen);
        }

        auto pSpecializationInfo = pShaderInfo->pSpecializationInfo;
        uint32_t mapEntryCount = pSpecializationInfo ? pSpecializationInfo->mapEntryCount : 0;
        pHasher->Update(mapEntryCount);
        if (mapEntryCount > 0)
        {
            pHasher->Update(reinterpret_cast<const uint8_t*>(pSpecializationInfo->pMapEntries),
                            sizeof(VkSpecializationMapEntry) * pSpecializationInfo->mapEntryCount);
            pHasher->Update(pSpecializationInfo->dataSize);
            pHasher->Update(reinterpret_cast<const uint8_t*>(pSpecializationInfo->pData),
                            pSpecializationInfo->dataSize);
        }

        pHasher->Update(pShaderInfo->descriptorRangeValueCount);
        if (pShaderInfo->descriptorRangeValueCount > 0)
        {
            for (uint32_t i = 0; i < pShaderInfo->descriptorRangeValueCount; ++i)
            {
                auto pDescriptorRangeValue = &pShaderInfo->pDescriptorRangeValues[i];
                pHasher->Update(pDescriptorRangeValue->type);
                pHasher->Update(pDescriptorRangeValue->set);
                pHasher->Update(pDescriptorRangeValue->binding);
                pHasher->Update(pDescriptorRangeValue->arraySize);

                // TODO: We should query descriptor size from patch
                const uint32_t DescriptorSize = 16;
                LLPC_ASSERT(pDescriptorRangeValue->type == ResourceMappingNodeType::DescriptorSampler);
                pHasher->Update(reinterpret_cast<const uint8_t*>(pDescriptorRangeValue->pValue),
                                pDescriptorRangeValue->arraySize * DescriptorSize);
            }
        }

        pHasher->Update(pShaderInfo->userDataNodeCount);
        if (pShaderInfo->userDataNodeCount > 0)
        {
            for (uint32_t i = 0; i < pShaderInfo->userDataNodeCount; ++i)
            {
                auto pUserDataNode = &pShaderInfo->pUserDataNodes[i];
                UpdateHashForResourceMappingNode(pUserDataNode, true, pHasher);
            }
        }

        if (isCacheHash)
        {
            auto& options = pShaderInfo->options;
            pHasher->Update(options.trapPresent);
            pHasher->Update(options.debugMode);
            pHasher->Update(options.enablePerformanceData);
            pHasher->Update(options.allowReZ);
            pHasher->Update(options.sgprLimit);
            pHasher->Update(options.vgprLimit);
            pHasher->Update(options.maxThreadGroupsPerComputeUnit);
        }
    }
}

// =====================================================================================================================
// Updates hash code context for resource mapping node.
//
// NOTE: This function will be called recusively if node's type is "DescriptorTableVaPtr"
void PipelineDumper::UpdateHashForResourceMappingNode(
    const ResourceMappingNode* pUserDataNode,    // [in] Resource mapping node
    bool                       isRootNode,       // TRUE if the node is in root level
    MetroHash::MetroHash64*    pHasher           // [in,out] Haher to generate hash code
    )
{
    pHasher->Update(pUserDataNode->type);
    pHasher->Update(pUserDataNode->sizeInDwords);
    pHasher->Update(pUserDataNode->offsetInDwords);

    switch (pUserDataNode->type)
    {
    case ResourceMappingNodeType::DescriptorResource:
    case ResourceMappingNodeType::DescriptorSampler:
    case ResourceMappingNodeType::DescriptorCombinedTexture:
    case ResourceMappingNodeType::DescriptorTexelBuffer:
    case ResourceMappingNodeType::DescriptorBuffer:
    case ResourceMappingNodeType::DescriptorFmask:
    case ResourceMappingNodeType::DescriptorBufferCompact:
        {
            pHasher->Update(pUserDataNode->srdRange);
            break;
        }
    case ResourceMappingNodeType::DescriptorTableVaPtr:
        {
            for (uint32_t i = 0; i < pUserDataNode->tablePtr.nodeCount; ++i)
            {
                UpdateHashForResourceMappingNode(&pUserDataNode->tablePtr.pNext[i], false, pHasher);
            }
            break;
        }
    case ResourceMappingNodeType::IndirectUserDataVaPtr:
        {
            pHasher->Update(pUserDataNode->userDataPtr);
            break;
        }
    case ResourceMappingNodeType::StreamOutTableVaPtr:
        {
            // Do nothing for the stream-out table
            break;
        }
    case ResourceMappingNodeType::PushConst:
        {
            if (isRootNode == false)
            {
                pHasher->Update(pUserDataNode->srdRange);
            }
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Outputs text with specified range to output stream.
template <class OStream>
void OutputText(
    const uint8_t* pData,    // [in] Text data
    uint32_t       startPos, // Starting position
    uint32_t       endPos,   // End position
    OStream&       out)      // [out] Output stream
{
    if (endPos > startPos)
    {
        // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
        // text print will be incorrect.
        uint8_t lastChar = pData[endPos - 1];
        const_cast<uint8_t*>(pData)[endPos - 1] = '\0';

        // Output text
        const char* pText = reinterpret_cast<const char*>(pData + startPos);
        out << pText << lastChar;

        // Restore last character
        const_cast<uint8_t*>(pData)[endPos - 1] = lastChar;
    }
}

// =====================================================================================================================
// Outputs binary data with specified range to output stream.
template<class OStream>
void OutputBinary(
    const uint8_t* pData,     // [in] Binary data
    uint32_t       startPos,  // Starting position
    uint32_t       endPos,    // End position
    OStream&       out)       // [out] Output stream
{
    const uint32_t* pStartPos = reinterpret_cast<const uint32_t*>(pData + startPos);
    int32_t dwordCount = (endPos - startPos) / sizeof(uint32_t);
    char formatBuf[256];
    for (int32_t i = 0; i < dwordCount; ++i)
    {
        if (i % 8 == 0)
        {
            out << "        ";
        }
        auto length = snprintf(formatBuf, sizeof(formatBuf), "%08X", pStartPos[i]);
        LLPC_UNUSED(length);
        out << formatBuf;

        if (i % 8 == 7)
        {
            out << "\n";
        }
        else
        {
            out << " ";
        }
    }

    if ((endPos > startPos) && (endPos - startPos) % sizeof(uint32_t))
    {
        int32_t padPos = dwordCount * sizeof(uint32_t);
        for (int32_t i = padPos; i < endPos; ++i)
        {
            auto length = snprintf(formatBuf, sizeof(formatBuf), "%02X", pData[i]);
            LLPC_UNUSED(length);
            out << formatBuf;
        }
    }

    if ((dwordCount % 8) != 0)
    {
        out << "\n";
    }
}

// =====================================================================================================================
//  Dumps ELF package to out stream
template<class OStream, class Elf>
OStream& operator<<(
    OStream&          out,      // [out] Output stream
    ElfReader<Elf>&   reader)   // [in] ELF object
{
    GfxIpVersion gfxIp = reader.GetGfxIpVersion();

    uint32_t sectionCount = reader.GetSectionCount();
    char formatBuf[256];

    for (uint32_t sortIdx = 0; sortIdx < sectionCount; ++sortIdx)
    {
        typename ElfReader<Elf>::ElfSectionBuffer* pSection = nullptr;
        uint32_t secIdx = 0;
        Result result = reader.GetSectionDataBySortingIndex(sortIdx, &secIdx, &pSection);
        LLPC_ASSERT(result == Result::Success);
        LLPC_UNUSED(result);
        if ((strcmp(pSection->pName, ShStrTabName) == 0) ||
            (strcmp(pSection->pName, StrTabName) == 0) ||
            (strcmp(pSection->pName, SymTabName) == 0))
        {
            // Output system section
            out << pSection->pName << " (size = " << pSection->secHead.sh_size << " bytes)\n";
        }
        else if (strcmp(pSection->pName, NoteName) == 0)
        {
            // Output .note section
            out << pSection->pName << " (size = " << pSection->secHead.sh_size << " bytes)\n";
            uint32_t offset = 0;
            const uint32_t noteHeaderSize = sizeof(NoteHeader);
            while (offset < pSection->secHead.sh_size)
            {
                const NoteHeader* pNode = reinterpret_cast<const NoteHeader*>(pSection->pData + offset);
                switch (pNode->type)
                {
                case Util::Abi::PipelineAbiNoteType::HsaIsa:
                    {
                        out << "    HsaIsa                       (name = "
                            << pNode->name << "  size = "<< pNode->descSize << ")\n";

                        auto pGpu = reinterpret_cast<const Util::Abi::AbiAmdGpuVersionNote*>(
                            pSection->pData + offset + noteHeaderSize);

                        out << "        vendorName  = " << pGpu->vendorName << "\n";
                        out << "        archName    = " << pGpu->archName << "\n";
                        out << "        gfxIp       = " << pGpu->gfxipMajorVer << "."
                                                        << pGpu->gfxipMinorVer << "."
                                                        << pGpu->gfxipStepping << "\n";
                        break;
                    }
                case Util::Abi::PipelineAbiNoteType::AbiMinorVersion:
                    {
                        out << "    AbiMinorVersion              (name = "
                            << pNode->name << "  size = " << pNode->descSize << ")\n";

                        auto pCodeVersion = reinterpret_cast<const Util::Abi::AbiMinorVersionNote *>(
                            pSection->pData + offset + noteHeaderSize);
                        out << "        minor = " << pCodeVersion->minorVersion << "\n";
                        break;
                    }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 432
                case Util::Abi::PipelineAbiNoteType::LegacyMetadata:
#endif
                case Util::Abi::PipelineAbiNoteType::PalMetadata:
                    {
                        if (pNode->type == LegacyMetadata)
                        {
                            out << "    PalMetadata                  (name = "
                                << pNode->name << "  size = " << pNode->descSize << ")\n";

                            const uint32_t configCount = pNode->descSize / sizeof(Util::Abi::PalMetadataNoteEntry);
                            auto pConfig = reinterpret_cast<const Util::Abi::PalMetadataNoteEntry*>(
                                pSection->pData + offset + noteHeaderSize);

                            std::map<uint32_t, uint32_t> sortedConfigs;
                            for (uint32_t i = 0; i < configCount; ++i)
                            {
                                sortedConfigs[pConfig[i].key] = pConfig[i].value;
                            }

                            for (auto config : sortedConfigs)
                            {
                                const char* pRegName = nullptr;
                                if (gfxIp.major <= 8)
                                {
                                    pRegName = Gfx6::GetRegisterNameString(gfxIp, config.first * 4);
                                }
                                else
                                {
                                    pRegName = Gfx9::GetRegisterNameString(gfxIp, config.first * 4);
                                }
                                auto length = snprintf(formatBuf,
                                    sizeof(formatBuf),
                                    "        %-45s = 0x%08X\n",
                                    pRegName,
                                    config.second);
                                LLPC_UNUSED(length);
                                out << formatBuf;
                            }
                        }
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 432
                        else
                        {
                            out << "    PalMetadata                  (name = "
                                << pNode->name << "  size = " << pNode->descSize << ")\n";

                            auto pBuffer = pSection->pData + offset + noteHeaderSize;
                            reader.InitMsgPack(pBuffer, pNode->descSize);

                            while (reader.GetNextMsgItem())
                            {
                                auto msgIterStatus = reader.GetMsgIteratorStatus();
                                auto pItem = reader.GetMsgItem();
                                if (msgIterStatus == MsgPackIteratorMapKey)
                                {
                                    out << "\n";
                                    for (uint32_t i = 0; i < reader.GetMsgMapLevel(); ++i)
                                    {
                                        out << "    ";
                                    }
                                }

                                switch (pItem->type)
                                {
                                case CWP_ITEM_MAP:
                                    {
                                        out << "{";
                                        break;
                                    }
                                case CWP_ITEM_STR:
                                    {
                                        OutputText(reinterpret_cast<const uint8_t*>(pItem->as.str.start),
                                                   0,
                                                   pItem->as.str.length,
                                                   out);
                                        if (msgIterStatus == MsgPackIteratorMapKey)
                                        {
                                            out << ": ";
                                        }
                                        break;
                                    }
                                case CWP_ITEM_ARRAY:
                                    {
                                        out << "[ ";
                                        break;
                                    }
                                case CWP_ITEM_BIN:
                                    {
                                        OutputBinary(reinterpret_cast<const uint8_t*>(pItem->as.bin.start),
                                                     0,
                                                     pItem->as.bin.length,
                                                     out);
                                        break;
                                    }
                                case CWP_ITEM_BOOLEAN:
                                    {
                                        out << pItem->as.boolean << " ";
                                        break;
                                    }
                                case CWP_ITEM_POSITIVE_INTEGER:
                                case CWP_ITEM_NEGATIVE_INTEGER:
                                    {
                                        if (msgIterStatus == MsgPackIteratorMapKey)
                                        {
                                            LLPC_ASSERT(pItem->as.u64 < UINT32_MAX);
                                            const char* pRegName = nullptr;
                                            uint32_t regId = static_cast<uint32_t>(pItem->as.u64 * 4);
                                            if (gfxIp.major <= 8)
                                            {
                                                pRegName = Gfx6::GetRegisterNameString(gfxIp, regId);
                                            }
                                            else
                                            {
                                                pRegName = Gfx9::GetRegisterNameString(gfxIp, regId);
                                            }
                                            auto length = snprintf(formatBuf,
                                                                   sizeof(formatBuf),
                                                                   "%-45s ",
                                                                   pRegName);
                                            LLPC_UNUSED(length);
                                            out << formatBuf;
                                        }
                                        else
                                        {
                                            auto length = snprintf(formatBuf,
                                                                   sizeof(formatBuf),
                                                                   "0x%016" PRIX64 " ",
                                                                   pItem->as.u64);
                                            LLPC_UNUSED(length);
                                            out << formatBuf;
                                        }
                                        break;
                                    }
                                case CWP_ITEM_FLOAT:
                                    {
                                        out << pItem->as.real << " ";
                                        break;
                                    }
                                case CWP_ITEM_DOUBLE:
                                    {
                                        out << pItem->as.long_real << " ";
                                        break;
                                    }
                                default:
                                    {
                                        LLPC_NEVER_CALLED();
                                        break;
                                    }
                                }

                                reader.UpdateMsgPackStatus(
                                    [&](MsgPackIteratorStatus status)
                                    {
                                        if (status == MsgPackIteratorMapValue)
                                            out << "}";
                                        else
                                            out << "]";
                                    }
                                );
                            }
                        }
#endif
                        break;
                    }
                default:
                    {
                        if (static_cast<uint32_t>(pNode->type) == NT_AMD_AMDGPU_ISA)
                        {
                            out << "    IsaVersion                   (name = "
                            << pNode->name << "  size = " << pNode->descSize << ")\n";
                            auto pDesc = pSection->pData + offset + noteHeaderSize;
                            OutputText(pDesc, 0, pNode->descSize, out);
                            out << "\n";
                        }
                        else
                        {
                            out << "    Unknown(" << (uint32_t)pNode->type << ")                (name = "
                                << pNode->name << "  size = " << pNode->descSize << ")\n";
                            auto pDesc = pSection->pData + offset + noteHeaderSize;
                            OutputBinary(pDesc, 0, pNode->descSize, out);
                        }
                        break;
                    }
                }
                offset += noteHeaderSize + Pow2Align(pNode->descSize, sizeof(uint32_t));
                LLPC_ASSERT(offset <= pSection->secHead.sh_size);
            }
        }
        else if (strcmp(pSection->pName, RelocName) == 0)
        {
            // Output .reloc section
            out << pSection->pName << " (size = " << pSection->secHead.sh_size << " bytes)\n";
            const uint32_t relocCount = reader.GetRelocationCount();
            for (uint32_t i = 0; i < relocCount; ++i)
            {
                ElfReloc reloc = {};
                reader.GetRelocation(i, &reloc);
                ElfSymbol elfSym = {};
                reader.GetSymbol(reloc.symIdx, &elfSym);
                auto length = snprintf(formatBuf, sizeof(formatBuf), "    %-35s", elfSym.pSymName);
                LLPC_UNUSED(length);
                out << "#" << i << "    " << formatBuf
                    << "    offset = " << reloc.offset << "\n";
            }
        }
        else if (strncmp(pSection->pName, AmdGpuConfigName, sizeof(AmdGpuConfigName) - 1) == 0)
        {
            // Output .AMDGPU.config section
            const uint32_t configCount = static_cast<uint32_t>(pSection->secHead.sh_size / sizeof(uint32_t) / 2);
            const uint32_t* pConfig = reinterpret_cast<const uint32_t*>(pSection->pData);
            out << pSection->pName << " (" << configCount << " registers)\n";

            for (uint32_t i = 0; i < configCount; ++i)
            {
                const char* pRegName = nullptr;
                if (gfxIp.major <= 8)
                {
                    pRegName = Gfx6::GetRegisterNameString(gfxIp, pConfig[2 * i]);
                }
                else
                {
                    pRegName = Gfx9::GetRegisterNameString(gfxIp, pConfig[2 * i]);
                }
                auto length = snprintf(formatBuf, sizeof(formatBuf), "        %-45s = 0x%08X\n", pRegName, pConfig[2 * i + 1]);
                LLPC_UNUSED(length);
                out << formatBuf;
            }
        }
        else if ((strncmp(pSection->pName, AmdGpuDisasmName, sizeof(AmdGpuDisasmName) - 1) == 0) ||
                 (strncmp(pSection->pName, AmdGpuCsdataName, sizeof(AmdGpuCsdataName) - 1) == 0) ||
                 (strncmp(pSection->pName, CommentName, sizeof(CommentName) - 1) == 0))
        {
            // Output text based sections
            out << pSection->pName << " (size = " << pSection->secHead.sh_size << " bytes)\n";

            std::vector<ElfSymbol> symbols;
            reader.GetSymbolsBySectionIndex(secIdx, symbols);
            uint32_t symIdx = 0;
            uint32_t startPos = 0;
            uint32_t endPos = 0;
            while (startPos < pSection->secHead.sh_size)
            {
                if (symIdx < symbols.size())
                {
                    endPos = static_cast<uint32_t>(symbols[symIdx].value);
                }
                else
                {
                    endPos = static_cast<uint32_t>(pSection->secHead.sh_size);
                }

                OutputText(pSection->pData, startPos, endPos, out);
                out << "\n";

                if (symIdx < symbols.size())
                {
                    out << "    " << symbols[symIdx].pSymName
                        << " (offset = " << symbols[symIdx].value << "  size = " << symbols[symIdx].size << ")\n";
                }
                ++symIdx;
                startPos = endPos;
            }
        }
        else
        {
            // Output binary based sections
            out << (pSection->pName[0] == 0 ? "(null)" : pSection->pName)
                << " (size = " << pSection->secHead.sh_size << " bytes)\n";

            std::vector<ElfSymbol> symbols;
            reader.GetSymbolsBySectionIndex(secIdx, symbols);
            uint32_t symIdx = 0;
            uint32_t startPos = 0;
            uint32_t endPos = 0;
            while (startPos < pSection->secHead.sh_size)
            {
                if (symIdx < symbols.size())
                {
                    endPos = static_cast<uint32_t>(symbols[symIdx].value);
                }
                else
                {
                    endPos = static_cast<uint32_t>(pSection->secHead.sh_size);
                }

                OutputBinary(pSection->pData, startPos, endPos, out);

                if (symIdx < symbols.size())
                {
                    out << "    " << symbols[symIdx].pSymName
                        << " (offset = " << symbols[symIdx].value << "  size = " << symbols[symIdx].size << ")\n";
                }
                ++symIdx;
                startPos = endPos;
            }
        }
        out << "\n";
    }

    return out;
}

// =====================================================================================================================
// Assistant macros for pipeline dump
#define CASE_CLASSENUM_TO_STRING(TYPE, ENUM) \
    case TYPE::ENUM: pString = #ENUM; break;
#define CASE_ENUM_TO_STRING(ENUM) \
    case ENUM: pString = #ENUM; break;

// =====================================================================================================================
// Translates enum "VkVertexInputRate" to string and output to ostream.
std::ostream& operator<<(
    std::ostream&       out,        // [out] Output stream
    VkVertexInputRate  inputRate)   // Vertex input rate
{
    const char* pString = nullptr;
    switch (inputRate)
    {
    CASE_ENUM_TO_STRING(VK_VERTEX_INPUT_RATE_VERTEX)
    CASE_ENUM_TO_STRING(VK_VERTEX_INPUT_RATE_INSTANCE)
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }
    return out << pString;
}

// =====================================================================================================================
// Translates enum "ResourceMappingNodeType" to string and output to ostream.
std::ostream& operator<<(
    std::ostream&           out,   // [out] Output stream
    ResourceMappingNodeType type)  // Resource map node type
{
    const char* pString = nullptr;
    switch (type)
    {
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorResource)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorSampler)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorCombinedTexture)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorTexelBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorFmask)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorBuffer)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorTableVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, IndirectUserDataVaPtr)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, PushConst)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, DescriptorBufferCompact)
    CASE_CLASSENUM_TO_STRING(ResourceMappingNodeType, StreamOutTableVaPtr)
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }
    return out << pString;
}

// =====================================================================================================================
// Translates enum "VkPrimitiveTopology" to string and output to ostream.
std::ostream& operator<<(
    std::ostream&       out,       // [out] Output stream
    VkPrimitiveTopology topology)  // Primitive topology
{
    const char* pString = nullptr;
    switch (topology)
    {
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
    CASE_ENUM_TO_STRING(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }

    return out << pString;
}

// =====================================================================================================================
// Translates enum "VkFormat" to string and output to ostream.
std::ostream& operator<<(
    std::ostream&       out,     // [out] Output stream
    VkFormat           format)  // Resource format
{
    const char* pString = nullptr;
    switch (format)
    {
    CASE_ENUM_TO_STRING(VK_FORMAT_UNDEFINED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R4G4_UNORM_PACK8)
    CASE_ENUM_TO_STRING(VK_FORMAT_R4G4B4A4_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_B4G4R4A4_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_R5G6B5_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_B5G6R5_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_R5G5B5A1_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_B5G5R5A1_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_A1R5G5B5_UNORM_PACK16)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8_SRGB)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8_SRGB)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8_SRGB)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8_SRGB)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R8G8B8A8_SRGB)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_B8G8R8A8_SRGB)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_UNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_SNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_USCALED_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_SSCALED_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_UINT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_SINT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A8B8G8R8_SRGB_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2R10G10B10_UNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2R10G10B10_SNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2R10G10B10_USCALED_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2R10G10B10_SSCALED_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2R10G10B10_UINT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2R10G10B10_SINT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2B10G10R10_UNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2B10G10R10_SNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2B10G10R10_USCALED_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2B10G10R10_SSCALED_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2B10G10R10_UINT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_A2B10G10R10_SINT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_SNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_USCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_SSCALED)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R16G16B16A16_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32B32_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32B32_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32B32_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32B32A32_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32B32A32_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R32G32B32A32_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64B64_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64B64_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64B64_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64B64A64_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64B64A64_SINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_R64G64B64A64_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_B10G11R11_UFLOAT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_D16_UNORM)
    CASE_ENUM_TO_STRING(VK_FORMAT_X8_D24_UNORM_PACK32)
    CASE_ENUM_TO_STRING(VK_FORMAT_D32_SFLOAT)
    CASE_ENUM_TO_STRING(VK_FORMAT_S8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_D16_UNORM_S8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_D24_UNORM_S8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_D32_SFLOAT_S8_UINT)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC1_RGB_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC1_RGB_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC1_RGBA_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC2_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC2_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC3_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC3_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC4_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC4_SNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC5_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC5_SNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC6H_UFLOAT_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC6H_SFLOAT_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC7_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_BC7_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_EAC_R11_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_EAC_R11_SNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_EAC_R11G11_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_EAC_R11G11_SNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_4x4_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_4x4_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_5x4_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_5x4_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_5x5_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_5x5_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_6x5_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_6x5_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_6x6_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_6x6_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_8x5_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_8x5_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_8x6_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_8x6_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_8x8_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_8x8_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x5_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x5_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x6_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x6_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x8_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x8_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x10_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_10x10_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_12x10_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_12x10_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_12x12_UNORM_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC2_2BPP_UNORM_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG)
    CASE_ENUM_TO_STRING(VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG)
        break;
    default:
        LLPC_NEVER_CALLED();
        break;
    }
    return out << pString;
}

} // Llpc
