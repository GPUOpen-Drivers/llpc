/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  vkgcPipelineDumper.cpp
* @breif VKGC source file: contains implementation of VKGC pipline dump utility.
***********************************************************************************************************************
*/
#include "llvm/Support/Mutex.h"
#include "llvm/Support/raw_ostream.h"

#include "vkgcElfReader.h"
#include "vkgcPipelineDumper.h"
#include "vkgcUtil.h"
#include <fstream>
#include <sstream>
#include <stdarg.h>
#include <sys/stat.h>
#include <unordered_set>

#define DEBUG_TYPE "vkgc-pipeline-dumper"

using namespace llvm;
using namespace MetroHash;
using namespace Util;

#define FILE_STAT stat

namespace Vkgc {

// Forward declaration
std::ostream &operator<<(std::ostream &out, VkVertexInputRate inputRate);
std::ostream &operator<<(std::ostream &out, VkFormat format);
std::ostream &operator<<(std::ostream &out, VkPrimitiveTopology topology);
std::ostream &operator<<(std::ostream &out, VkPolygonMode polygonMode);
std::ostream &operator<<(std::ostream &out, VkCullModeFlagBits cullMode);
std::ostream &operator<<(std::ostream &out, VkFrontFace frontFace);
std::ostream &operator<<(std::ostream &out, ResourceMappingNodeType type);
std::ostream &operator<<(std::ostream &out, NggSubgroupSizingType subgroupSizing);
std::ostream &operator<<(std::ostream &out, NggCompactMode compactMode);
std::ostream &operator<<(std::ostream &out, WaveBreakSize waveBreakSize);
std::ostream &operator<<(std::ostream &out, ShadowDescriptorTableUsage shadowDescriptorTableUsage);

template std::ostream &operator<<(std::ostream &out, ElfReader<Elf64> &reader);
template raw_ostream &operator<<(raw_ostream &out, ElfReader<Elf64> &reader);
constexpr size_t ShaderModuleCacheHashOffset = offsetof(ShaderModuleData, cacheHash);

// =====================================================================================================================
// Represents LLVM based mutex.
class Mutex {
public:
  Mutex() {}

  void lock() { m_mutex.lock(); }

  void unlock() { m_mutex.unlock(); }

private:
  llvm::sys::Mutex m_mutex;
};

// Mutex for pipeline dump
static Mutex SDumpMutex;

// =====================================================================================================================
// Represents the file objects for pipeline dump
struct PipelineDumpFile {
  PipelineDumpFile(const char *dumpFileName, const char *binaryFileName)
      : dumpFile(dumpFileName), binaryIndex(0), binaryFileName(binaryFileName) {}

  std::ofstream dumpFile;     // File object for .pipe file
  std::ofstream binaryFile;   // File object for ELF binary
  unsigned binaryIndex;       // ELF Binary index
  std::string binaryFileName; // File name of binary file
};

// =====================================================================================================================
// Dumps SPIR-V shader binary to external file.
//
// @param dumpDir : Directory of pipeline dump
// @param spirvBin : SPIR-V binary
void VKAPI_CALL IPipelineDumper::DumpSpirvBinary(const char *dumpDir, const BinaryData *spirvBin) {
  MetroHash::Hash hash = {};
  MetroHash64::Hash(reinterpret_cast<const uint8_t *>(spirvBin->pCode), spirvBin->codeSize, hash.bytes);
  PipelineDumper::DumpSpirvBinary(dumpDir, spirvBin, &hash);
}

// =====================================================================================================================
// Begins to dump graphics/compute pipeline info.
//
// @param dumpOptions : Pipeline dump options
// @param pipelineInfo : Info of the pipeline to be built
void *VKAPI_CALL IPipelineDumper::BeginPipelineDump(const PipelineDumpOptions *dumpOptions,
                                                    PipelineBuildInfo pipelineInfo) {
  MetroHash::Hash hash = {};
  if (pipelineInfo.pComputeInfo)
    hash = PipelineDumper::generateHashForComputePipeline(pipelineInfo.pComputeInfo, false, false);
  else {
    assert(pipelineInfo.pGraphicsInfo);
    hash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo.pGraphicsInfo, false, false);
  }

  return PipelineDumper::BeginPipelineDump(dumpOptions, pipelineInfo, &hash);
}

// =====================================================================================================================
// Ends to dump graphics/compute pipeline info.
//
// @param dumpFile : The handle of pipeline dump file
void VKAPI_CALL IPipelineDumper::EndPipelineDump(void *dumpFile) {
  PipelineDumper::EndPipelineDump(reinterpret_cast<PipelineDumpFile *>(dumpFile));
}

// =====================================================================================================================
// Disassembles pipeline binary and dumps it to pipeline info file.
//
// @param dumpFile : The handle of pipeline dump file
// @param gfxIp : Graphics IP version info
// @param pipelineBin : Pipeline binary (ELF)
void VKAPI_CALL IPipelineDumper::DumpPipelineBinary(void *dumpFile, GfxIpVersion gfxIp, const BinaryData *pipelineBin) {
  PipelineDumper::DumpPipelineBinary(reinterpret_cast<PipelineDumpFile *>(dumpFile), gfxIp, pipelineBin);
}

// =====================================================================================================================
// Dump extra info to pipeline file.
//
// @param dumpFile : The handle of pipeline dump file
// @param str : Extra string info to dump
void VKAPI_CALL IPipelineDumper::DumpPipelineExtraInfo(void *dumpFile, const char *str) {
  std::string tmpStr(str);
  PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile *>(dumpFile), &tmpStr);
}

// =====================================================================================================================
// Gets shader module hash code.
//
// @param moduleData : Pointer to the shader module data
uint64_t VKAPI_CALL IPipelineDumper::GetShaderHash(const void *moduleData) {
  const ShaderModuleData *shaderModuleData = reinterpret_cast<const ShaderModuleData *>(moduleData);
  return MetroHash::compact64(reinterpret_cast<const MetroHash::Hash *>(&shaderModuleData->hash));
}

// =====================================================================================================================
// Calculates graphics pipeline hash code.
//
// @param pipelineInfo : Info to build this graphics pipeline
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(const GraphicsPipelineBuildInfo *pipelineInfo) {
  auto hash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false, false);
  return MetroHash::compact64(&hash);
}
// =====================================================================================================================
// Get graphics pipeline name.
//
// @param [In] graphicsPipelineInfo : Info to build this graphics pipeline
// @param [Out] pipeNameOut : The full name of this graphics pipeline
// @param nameBufSize : Size of the buffer to store pipeline name
void VKAPI_CALL IPipelineDumper::GetPipelineName(const GraphicsPipelineBuildInfo *graphicsPipelineInfo,
                                                 char *pipeNameOut, const size_t nameBufSize) {
  auto hash = PipelineDumper::generateHashForGraphicsPipeline(graphicsPipelineInfo, false, false);
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pGraphicsInfo = graphicsPipelineInfo;
  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, &hash);
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Get compute pipeline name.
//
// @param [In] computePipelineInfo : Info to build this compute pipeline
// @param [Out] pipeNameOut : The full name of this compute pipeline
// @param nameBufSize : Size of the buffer to store pipeline name
void VKAPI_CALL IPipelineDumper::GetPipelineName(const ComputePipelineBuildInfo *computePipelineInfo, char *pipeNameOut,
                                                 const size_t nameBufSize) {
  auto hash = PipelineDumper::generateHashForComputePipeline(computePipelineInfo, false, false);
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pComputeInfo = computePipelineInfo;

  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, &hash);
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Calculates compute pipeline hash code.
//
// @param pipelineInfo : Info to build this compute pipeline
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(const ComputePipelineBuildInfo *pipelineInfo) {
  auto hash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, false, false);
  return MetroHash::compact64(&hash);
}

// =====================================================================================================================
// Gets the file name of SPIR-V binary according the specified shader hash.
//
// @param hash : Shader hash code
std::string PipelineDumper::getSpirvBinaryFileName(const MetroHash::Hash *hash) {
  uint64_t hashCode64 = MetroHash::compact64(hash);
  char fileName[64] = {};
  auto length = snprintf(fileName, 64, "Shader_0x%016" PRIX64 ".spv", hashCode64);
  (void(length)); // unused
  return std::string(fileName);
}

// =====================================================================================================================
// Gets the file name of pipeline info file according to the specified pipeline build info and pipeline hash.
//
// @param pipelineInfo : Info of the pipeline to be built
// @param hash : Pipeline hash code
std::string PipelineDumper::getPipelineInfoFileName(PipelineBuildInfo pipelineInfo, const MetroHash::Hash *hash) {
  uint64_t hashCode64 = MetroHash::compact64(hash);
  char fileName[64] = {};
  if (pipelineInfo.pComputeInfo) {
    auto length = snprintf(fileName, 64, "PipelineCs_0x%016" PRIX64, hashCode64);
    (void(length)); // unused
  }
  else {
    assert(pipelineInfo.pGraphicsInfo);
    const char *fileNamePrefix = nullptr;
    if (pipelineInfo.pGraphicsInfo->tes.pModuleData && pipelineInfo.pGraphicsInfo->gs.pModuleData)
      fileNamePrefix = "PipelineGsTess";
    else if (pipelineInfo.pGraphicsInfo->gs.pModuleData)
      fileNamePrefix = "PipelineGs";
    else if (pipelineInfo.pGraphicsInfo->tes.pModuleData)
      fileNamePrefix = "PipelineTess";
    else
      fileNamePrefix = "PipelineVsFs";

    auto length = snprintf(fileName, 64, "%s_0x%016" PRIX64, fileNamePrefix, hashCode64);
    (void(length)); // unused
  }

  return std::string(fileName);
}

// =====================================================================================================================
// Begins to dump graphics/compute pipeline info.
//
// @param dumpOptions : Pipeline dump options
// @param pipelineInfo : Info of the pipeline to be built
// @param hash : Pipeline hash code
PipelineDumpFile *PipelineDumper::BeginPipelineDump(const PipelineDumpOptions *dumpOptions,
                                                    PipelineBuildInfo pipelineInfo, const MetroHash::Hash *hash) {
  bool disableLog = false;
  std::string dumpFileName;
  std::string dumpPathName;
  std::string dumpBinaryName;
  PipelineDumpFile *dumpFile = nullptr;

  // Filter pipeline hash
  if (dumpOptions->filterPipelineDumpByHash != 0) {
    uint64_t hash64 = MetroHash::compact64(hash);
    if (hash64 != dumpOptions->filterPipelineDumpByHash)
      disableLog = true;
  }

  if (!disableLog) {
    // Filter pipeline type
    dumpFileName = getPipelineInfoFileName(pipelineInfo, hash);
    if (dumpOptions->filterPipelineDumpByType & PipelineDumpFilterCs) {
      if (dumpFileName.find("Cs") != std::string::npos)
        disableLog = true;
    }
    if (dumpOptions->filterPipelineDumpByType & PipelineDumpFilterGs) {
      if (dumpFileName.find("Gs") != std::string::npos)
        disableLog = true;
    }
    if (dumpOptions->filterPipelineDumpByType & PipelineDumpFilterTess) {
      if (dumpFileName.find("Tess") != std::string::npos)
        disableLog = true;
    }
    if (dumpOptions->filterPipelineDumpByType & PipelineDumpFilterVsPs) {
      if (dumpFileName.find("VsFs") != std::string::npos)
        disableLog = true;
    }
  }

  if (!disableLog) {
    bool enableDump = true;
    SDumpMutex.lock();

    // Create the dump directory
    createDirectory(dumpOptions->pDumpDir);

    // Build dump file name
    if (dumpOptions->dumpDuplicatePipelines) {
      unsigned index = 0;
      int result = 0;
      while (result != -1) {
        dumpPathName = dumpOptions->pDumpDir;
        dumpPathName += "/";
        dumpPathName += dumpFileName;
        if (index > 0) {
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
    } else {
      static std::unordered_set<std::string> FileNames;

      if (FileNames.find(dumpFileName) == FileNames.end()) {
        dumpPathName = dumpOptions->pDumpDir;
        dumpPathName += "/";
        dumpPathName += dumpFileName;
        dumpBinaryName = dumpPathName + ".elf";
        dumpPathName += ".pipe";
        FileNames.insert(dumpFileName);
      } else
        enableDump = false;
    }

    // Open dump file
    if (enableDump) {
      dumpFile = new PipelineDumpFile(dumpPathName.c_str(), dumpBinaryName.c_str());
      if (dumpFile->dumpFile.bad()) {
        delete dumpFile;
        dumpFile = nullptr;
      }
    }

    SDumpMutex.unlock();

    // Dump pipeline input info
    if (dumpFile) {
      if (pipelineInfo.pComputeInfo)
        dumpComputePipelineInfo(&dumpFile->dumpFile, dumpOptions->pDumpDir, pipelineInfo.pComputeInfo);

      if (pipelineInfo.pGraphicsInfo)
        dumpGraphicsPipelineInfo(&dumpFile->dumpFile, dumpOptions->pDumpDir, pipelineInfo.pGraphicsInfo);

    }
  }

  return dumpFile;
}

// =====================================================================================================================
// Ends to dump graphics/compute pipeline info.
//
// @param dumpFile : Dump file
void PipelineDumper::EndPipelineDump(PipelineDumpFile *dumpFile) {
  delete dumpFile;
}

// =====================================================================================================================
// Dumps resource mapping node to dumpFile.
//
// @param userDataNode : User data nodes to be dumped
// @param prefix : Prefix string for each line
// @param [out] dumpFile : dump file
void PipelineDumper::dumpResourceMappingNode(const ResourceMappingNode *userDataNode, const char *prefix,
                                             std::ostream &dumpFile) {
  dumpFile << prefix << ".type = " << userDataNode->type << "\n";
  dumpFile << prefix << ".offsetInDwords = " << userDataNode->offsetInDwords << "\n";
  dumpFile << prefix << ".sizeInDwords = " << userDataNode->sizeInDwords << "\n";

  switch (userDataNode->type) {
  case ResourceMappingNodeType::DescriptorResource:
  case ResourceMappingNodeType::DescriptorSampler:
  case ResourceMappingNodeType::DescriptorYCbCrSampler:
  case ResourceMappingNodeType::DescriptorCombinedTexture:
  case ResourceMappingNodeType::DescriptorTexelBuffer:
  case ResourceMappingNodeType::DescriptorBuffer:
  case ResourceMappingNodeType::DescriptorFmask:
  case ResourceMappingNodeType::DescriptorBufferCompact:
  {
    dumpFile << prefix << ".set = " << userDataNode->srdRange.set << "\n";
    dumpFile << prefix << ".binding = " << userDataNode->srdRange.binding << "\n";
    break;
  }
  case ResourceMappingNodeType::DescriptorTableVaPtr: {
    char prefixBuf[256];
    int length = 0;
    for (unsigned i = 0; i < userDataNode->tablePtr.nodeCount; ++i) {
      length = snprintf(prefixBuf, 256, "%s.next[%u]", prefix, i);
      dumpResourceMappingNode(userDataNode->tablePtr.pNext + i, prefixBuf, dumpFile);
    }
    break;
  }
  case ResourceMappingNodeType::IndirectUserDataVaPtr: {
    dumpFile << prefix << ".indirectUserDataCount = " << userDataNode->userDataPtr.sizeInDwords << "\n";
    break;
  }
  case ResourceMappingNodeType::StreamOutTableVaPtr: {
    break;
  }
  case ResourceMappingNodeType::PushConst: {
    dumpFile << prefix << ".set = " << userDataNode->srdRange.set << "\n";
    dumpFile << prefix << ".binding = " << userDataNode->srdRange.binding << "\n";
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Dumps pipeline shader info to file.
//
// @param shaderInfo : Shader info of specified shader stage
// @param [out] dumpFile : dump file
void PipelineDumper::dumpPipelineShaderInfo(const PipelineShaderInfo *shaderInfo, std::ostream &dumpFile) {
  const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
  auto moduleHash = reinterpret_cast<const MetroHash::Hash *>(&moduleData->hash[0]);

  // Output shader binary file
  ShaderStage stage = shaderInfo->entryStage;

  dumpFile << "[" << getShaderStageAbbreviation(stage) << "SpvFile]\n";
  dumpFile << "fileName = " << getSpirvBinaryFileName(moduleHash) << "\n\n";

  dumpFile << "[" << getShaderStageAbbreviation(stage) << "Info]\n";
  // Output entry point
  if (shaderInfo->pEntryTarget)
    dumpFile << "entryPoint = " << shaderInfo->pEntryTarget << "\n";

  // Output specialize info
  if (shaderInfo->pSpecializationInfo) {
    auto specializationInfo = shaderInfo->pSpecializationInfo;
    for (unsigned i = 0; i < specializationInfo->mapEntryCount; ++i) {
      dumpFile << "specConst.mapEntry[" << i << "].constantID = " << specializationInfo->pMapEntries[i].constantID
               << "\n";
      dumpFile << "specConst.mapEntry[" << i << "].offset = " << specializationInfo->pMapEntries[i].offset << "\n";
      dumpFile << "specConst.mapEntry[" << i << "].size = " << specializationInfo->pMapEntries[i].size << "\n";
    }
    const unsigned *data = reinterpret_cast<const unsigned *>(specializationInfo->pData);
    for (unsigned i = 0; i < (specializationInfo->dataSize + sizeof(unsigned) - 1) / sizeof(unsigned); ++i) {
      if ((i % 8) == 0)
        dumpFile << "specConst.uintData = ";
      dumpFile << data[i];
      if ((i % 8) == 7)
        dumpFile << "\n";
      else
        dumpFile << ", ";
    }
    dumpFile << "\n";
  }

  // Output descriptor range value
  if (shaderInfo->descriptorRangeValueCount > 0) {
    for (unsigned i = 0; i < shaderInfo->descriptorRangeValueCount; ++i) {
      auto descriptorRangeValue = &shaderInfo->pDescriptorRangeValues[i];
      dumpFile << "descriptorRangeValue[" << i << "].type = " << descriptorRangeValue->type << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].set = " << descriptorRangeValue->set << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].binding = " << descriptorRangeValue->binding << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].arraySize = " << descriptorRangeValue->arraySize << "\n";
      for (unsigned j = 0; j < descriptorRangeValue->arraySize; ++j) {
        dumpFile << "descriptorRangeValue[" << i << "].uintData = ";
        const unsigned descriptorSizeInDw =
            descriptorRangeValue->type == ResourceMappingNodeType::DescriptorYCbCrSampler ? 8 : 4;

        for (unsigned k = 0; k < descriptorSizeInDw - 1; ++k)
          dumpFile << descriptorRangeValue->pValue[k] << ", ";
        dumpFile << descriptorRangeValue->pValue[descriptorSizeInDw - 1] << "\n";
      }
    }
    dumpFile << "\n";
  }

  // Output resource node mapping
  if (shaderInfo->userDataNodeCount > 0) {
    char prefixBuff[64];
    for (unsigned i = 0; i < shaderInfo->userDataNodeCount; ++i) {
      auto userDataNode = &shaderInfo->pUserDataNodes[i];
      auto length = snprintf(prefixBuff, 64, "userDataNode[%u]", i);
      (void(length)); // unused
      dumpResourceMappingNode(userDataNode, prefixBuff, dumpFile);
    }
    dumpFile << "\n";
  }

  // Output pipeline shader options
  dumpFile << "options.trapPresent = " << shaderInfo->options.trapPresent << "\n";
  dumpFile << "options.debugMode = " << shaderInfo->options.debugMode << "\n";
  dumpFile << "options.enablePerformanceData = " << shaderInfo->options.enablePerformanceData << "\n";
  dumpFile << "options.allowReZ = " << shaderInfo->options.allowReZ << "\n";
  dumpFile << "options.vgprLimit = " << shaderInfo->options.vgprLimit << "\n";
  dumpFile << "options.sgprLimit = " << shaderInfo->options.sgprLimit << "\n";
  dumpFile << "options.maxThreadGroupsPerComputeUnit = " << shaderInfo->options.maxThreadGroupsPerComputeUnit << "\n";
  dumpFile << "options.waveSize = " << shaderInfo->options.waveSize << "\n";
  dumpFile << "options.wgpMode = " << shaderInfo->options.wgpMode << "\n";
  dumpFile << "options.waveBreakSize = " << shaderInfo->options.waveBreakSize << "\n";
  dumpFile << "options.forceLoopUnrollCount = " << shaderInfo->options.forceLoopUnrollCount << "\n";
  dumpFile << "options.useSiScheduler = " << shaderInfo->options.useSiScheduler << "\n";
  dumpFile << "options.updateDescInElf = " << shaderInfo->options.updateDescInElf << "\n";
  dumpFile << "options.allowVaryWaveSize = " << shaderInfo->options.allowVaryWaveSize << "\n";
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
  dumpFile << "options.enableLoadScalarizer = " << shaderInfo->options.enableLoadScalarizer << "\n";
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 35
  dumpFile << "options.disableLicm = " << shaderInfo->options.disableLicm << "\n";
#endif
  dumpFile << "options.unrollThreshold = " << shaderInfo->options.unrollThreshold << "\n";
  dumpFile << "options.scalarThreshold = " << shaderInfo->options.scalarThreshold << "\n";

  dumpFile << "\n";
}

// =====================================================================================================================
// Dumps SPIR-V shader binary to external file
//
// @param dumpDir : Directory of pipeline dump
// @param spirvBin : SPIR-V binary
// @param hash : Pipeline hash code
void PipelineDumper::DumpSpirvBinary(const char *dumpDir, const BinaryData *spirvBin, MetroHash::Hash *hash) {
  std::string pathName = dumpDir;
  pathName += "/";
  pathName += getSpirvBinaryFileName(hash);

  // Open dumpfile
  std::ofstream dumpFile(pathName.c_str(), std::ios_base::binary | std::ios_base::out);
  if (!dumpFile.bad())
    dumpFile.write(reinterpret_cast<const char *>(spirvBin->pCode), spirvBin->codeSize);
}

// =====================================================================================================================
// Disassembles pipeline binary and dumps it to pipeline info file.
//
// @param dumpFile : Directory of pipeline dump
// @param gfxIp : Graphics IP version info
// @param pipelineBin : Pipeline binary (ELF)
void PipelineDumper::DumpPipelineBinary(PipelineDumpFile *dumpFile, GfxIpVersion gfxIp, const BinaryData *pipelineBin) {
  if (!dumpFile)
    return;

  if (!pipelineBin->pCode || pipelineBin->codeSize == 0)
    return;

  ElfReader<Elf64> reader(gfxIp);
  size_t codeSize = pipelineBin->codeSize;
  auto result = reader.ReadFromBuffer(pipelineBin->pCode, &codeSize);
  assert(result == Result::Success);
  (void(result)); // unused

  dumpFile->dumpFile << "\n[CompileLog]\n";
  dumpFile->dumpFile << reader;

  std::string binaryFileName = dumpFile->binaryFileName;
  if (dumpFile->binaryIndex > 0) {
    char suffixBuffer[32] = {};
    snprintf(suffixBuffer, sizeof(suffixBuffer), ".%u", dumpFile->binaryIndex);
    binaryFileName += suffixBuffer;
  }

  dumpFile->binaryIndex++;
  dumpFile->binaryFile.open(binaryFileName.c_str(), std::ostream::out | std::ostream::binary);
  if (!dumpFile->binaryFile.bad()) {
    dumpFile->binaryFile.write(reinterpret_cast<const char *>(pipelineBin->pCode), pipelineBin->codeSize);
    dumpFile->binaryFile.close();
  }
}

// =====================================================================================================================
// Dump extra info to pipeline file.
//
// @param dumpFile : Directory of pipeline dump
// @param str : Extra info string
void PipelineDumper::DumpPipelineExtraInfo(PipelineDumpFile *dumpFile, const std::string *str) {
  if (dumpFile)
    dumpFile->dumpFile << *str;
}

// =====================================================================================================================
// Dumps LLPC version info to file
//
// @param [out] dumpFile : dump file
void PipelineDumper::dumpVersionInfo(std::ostream &dumpFile) {
  dumpFile << "[Version]\n";
  dumpFile << "version = " << Version << "\n\n";
}

// =====================================================================================================================
// Dumps compute pipeline state info to file.
//
// @param pipelineInfo : Info of the graphics pipeline to be built
// @param dumpDir : Directory of pipeline dump
// @param [out] dumpFile : dump file
void PipelineDumper::dumpComputeStateInfo(const ComputePipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                          std::ostream &dumpFile) {
  dumpFile << "[ComputePipelineState]\n";

  // Output pipeline states
  dumpFile << "deviceIndex = " << pipelineInfo->deviceIndex << "\n";
  dumpPipelineOptions(&pipelineInfo->options, dumpFile);
}

// =====================================================================================================================
// Dumps pipeline options to file.
//
// @param options : Pipeline options
// @param [out] dumpFile : dump file
void PipelineDumper::dumpPipelineOptions(const PipelineOptions *options, std::ostream &dumpFile) {
  dumpFile << "options.includeDisassembly = " << options->includeDisassembly << "\n";
  dumpFile << "options.scalarBlockLayout = " << options->scalarBlockLayout << "\n";
  dumpFile << "options.includeIr = " << options->includeIr << "\n";
  dumpFile << "options.robustBufferAccess = " << options->robustBufferAccess << "\n";
  dumpFile << "options.reconfigWorkgroupLayout = " << options->reconfigWorkgroupLayout << "\n";
  dumpFile << "options.shadowDescriptorTableUsage = " << options->shadowDescriptorTableUsage << "\n";
  dumpFile << "options.shadowDescriptorTablePtrHigh = " << options->shadowDescriptorTablePtrHigh << "\n";
}

// =====================================================================================================================
// Dumps compute pipeline information to file.
//
// @param dumpFile : Pipeline dump file
// @param dumpDir : Directory of pipeline dump
// @param pipelineInfo : Info of the compute pipeline to be built
void PipelineDumper::dumpComputePipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                             const ComputePipelineBuildInfo *pipelineInfo) {
  dumpVersionInfo(*dumpFile);

  // Output shader info
  dumpPipelineShaderInfo(&pipelineInfo->cs, *dumpFile);
  dumpComputeStateInfo(pipelineInfo, dumpDir, *dumpFile);

  dumpFile->flush();
}

// =====================================================================================================================
// Dumps graphics pipeline state info to file.
//
// @param pipelineInfo : Info of the graphics pipeline to be built
// @param dumpDir : Directory of pipeline dump
// @param [out] dumpFile : dump file
void PipelineDumper::dumpGraphicsStateInfo(const GraphicsPipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                           std::ostream &dumpFile) {
  dumpFile << "[GraphicsPipelineState]\n";

  // Output pipeline states
  dumpFile << "topology = " << pipelineInfo->iaState.topology << "\n";
  dumpFile << "patchControlPoints = " << pipelineInfo->iaState.patchControlPoints << "\n";
  dumpFile << "deviceIndex = " << pipelineInfo->iaState.deviceIndex << "\n";
  dumpFile << "disableVertexReuse = " << pipelineInfo->iaState.disableVertexReuse << "\n";
  dumpFile << "switchWinding = " << pipelineInfo->iaState.switchWinding << "\n";
  dumpFile << "enableMultiView = " << pipelineInfo->iaState.enableMultiView << "\n";
  dumpFile << "depthClipEnable = " << pipelineInfo->vpState.depthClipEnable << "\n";

  dumpFile << "rasterizerDiscardEnable = " << pipelineInfo->rsState.rasterizerDiscardEnable << "\n";
  dumpFile << "perSampleShading = " << pipelineInfo->rsState.perSampleShading << "\n";
  dumpFile << "numSamples = " << pipelineInfo->rsState.numSamples << "\n";
  dumpFile << "samplePatternIdx = " << pipelineInfo->rsState.samplePatternIdx << "\n";
  dumpFile << "usrClipPlaneMask = " << static_cast<unsigned>(pipelineInfo->rsState.usrClipPlaneMask) << "\n";
  dumpFile << "polygonMode = " << pipelineInfo->rsState.polygonMode << "\n";
  dumpFile << "cullMode = " << static_cast<VkCullModeFlagBits>(pipelineInfo->rsState.cullMode) << "\n";
  dumpFile << "frontFace = " << pipelineInfo->rsState.frontFace << "\n";
  dumpFile << "depthBiasEnable = " << pipelineInfo->rsState.depthBiasEnable << "\n";
  dumpFile << "alphaToCoverageEnable = " << pipelineInfo->cbState.alphaToCoverageEnable << "\n";
  dumpFile << "dualSourceBlendEnable = " << pipelineInfo->cbState.dualSourceBlendEnable << "\n";

  for (unsigned i = 0; i < MaxColorTargets; ++i) {
    if (pipelineInfo->cbState.target[i].format != VK_FORMAT_UNDEFINED) {
      auto cbTarget = &pipelineInfo->cbState.target[i];
      dumpFile << "colorBuffer[" << i << "].format = " << cbTarget->format << "\n";
      dumpFile << "colorBuffer[" << i << "].channelWriteMask = " << static_cast<unsigned>(cbTarget->channelWriteMask)
               << "\n";
      dumpFile << "colorBuffer[" << i << "].blendEnable = " << cbTarget->blendEnable << "\n";
      dumpFile << "colorBuffer[" << i << "].blendSrcAlphaToColor = " << cbTarget->blendSrcAlphaToColor << "\n";
    }
  }

  dumpFile << "nggState.enableNgg = " << pipelineInfo->nggState.enableNgg << "\n";
  dumpFile << "nggState.enableGsUse = " << pipelineInfo->nggState.enableGsUse << "\n";
  dumpFile << "nggState.forceNonPassthrough = " << pipelineInfo->nggState.forceNonPassthrough << "\n";
  dumpFile << "nggState.alwaysUsePrimShaderTable = " << pipelineInfo->nggState.alwaysUsePrimShaderTable << "\n";
  dumpFile << "nggState.compactMode = " << pipelineInfo->nggState.compactMode << "\n";
  dumpFile << "nggState.enableFastLaunch = " << pipelineInfo->nggState.enableFastLaunch << "\n";
  dumpFile << "nggState.enableVertexReuse = " << pipelineInfo->nggState.enableVertexReuse << "\n";
  dumpFile << "nggState.enableBackfaceCulling = " << pipelineInfo->nggState.enableBackfaceCulling << "\n";
  dumpFile << "nggState.enableFrustumCulling = " << pipelineInfo->nggState.enableFrustumCulling << "\n";
  dumpFile << "nggState.enableBoxFilterCulling = " << pipelineInfo->nggState.enableBoxFilterCulling << "\n";
  dumpFile << "nggState.enableSphereCulling = " << pipelineInfo->nggState.enableSphereCulling << "\n";
  dumpFile << "nggState.enableSmallPrimFilter = " << pipelineInfo->nggState.enableSmallPrimFilter << "\n";
  dumpFile << "nggState.enableCullDistanceCulling = " << pipelineInfo->nggState.enableCullDistanceCulling << "\n";
  dumpFile << "nggState.backfaceExponent = " << pipelineInfo->nggState.backfaceExponent << "\n";
  dumpFile << "nggState.subgroupSizing = " << pipelineInfo->nggState.subgroupSizing << "\n";
  dumpFile << "nggState.primsPerSubgroup = " << pipelineInfo->nggState.primsPerSubgroup << "\n";
  dumpFile << "nggState.vertsPerSubgroup = " << pipelineInfo->nggState.vertsPerSubgroup << "\n";

  dumpPipelineOptions(&pipelineInfo->options, dumpFile);
  dumpFile << "\n\n";

  // Output vertex input state
  if (pipelineInfo->pVertexInput && pipelineInfo->pVertexInput->vertexBindingDescriptionCount > 0) {
    dumpFile << "[VertexInputState]\n";
    for (unsigned i = 0; i < pipelineInfo->pVertexInput->vertexBindingDescriptionCount; ++i) {
      auto binding = &pipelineInfo->pVertexInput->pVertexBindingDescriptions[i];
      dumpFile << "binding[" << i << "].binding = " << binding->binding << "\n";
      dumpFile << "binding[" << i << "].stride = " << binding->stride << "\n";
      dumpFile << "binding[" << i << "].inputRate = " << binding->inputRate << "\n";
    }

    for (unsigned i = 0; i < pipelineInfo->pVertexInput->vertexAttributeDescriptionCount; ++i) {
      auto attrib = &pipelineInfo->pVertexInput->pVertexAttributeDescriptions[i];
      dumpFile << "attribute[" << i << "].location = " << attrib->location << "\n";
      dumpFile << "attribute[" << i << "].binding = " << attrib->binding << "\n";
      dumpFile << "attribute[" << i << "].format = " << attrib->format << "\n";
      dumpFile << "attribute[" << i << "].offset = " << attrib->offset << "\n";
    }

    auto divisorState = findVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, pipelineInfo->pVertexInput->pNext);

    for (unsigned i = 0; divisorState && i < divisorState->vertexBindingDivisorCount; ++i) {
      auto divisor = &divisorState->pVertexBindingDivisors[i];
      dumpFile << "divisor[" << i << "].binding = " << divisor->binding << "\n";
      dumpFile << "divisor[" << i << "].divisor = " << divisor->divisor << "\n";
    }
  }

}

// =====================================================================================================================
// Dumps graphics pipeline build info to file.
//
// @param dumpFile : Pipeline dump file
// @param dumpDir : Directory of pipeline dump
// @param pipelineInfo : Info of the graphics pipeline to be built
void PipelineDumper::dumpGraphicsPipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                              const GraphicsPipelineBuildInfo *pipelineInfo) {
  dumpVersionInfo(*dumpFile);
  // Dump pipeline
  const PipelineShaderInfo *shaderInfos[ShaderStageGfxCount] = {
      &pipelineInfo->vs, &pipelineInfo->tcs, &pipelineInfo->tes, &pipelineInfo->gs, &pipelineInfo->fs,
  };

  for (unsigned stage = 0; stage < ShaderStageGfxCount; ++stage) {
    const PipelineShaderInfo *shaderInfo = shaderInfos[stage];
    if (!shaderInfo->pModuleData)
      continue;
    dumpPipelineShaderInfo(shaderInfo, *dumpFile);
  }

  dumpGraphicsStateInfo(pipelineInfo, dumpDir, *dumpFile);

  dumpFile->flush();
}

// =====================================================================================================================
// Builds hash code from graphics pipline build info.  If stage is a specific stage of the graphics pipeline, then only
// the portions of the pipeline build info that affect that stage will be included in the hash.  Otherwise, stage must
// be ShaderStageInvalid, and all values in the build info will be included.
//
// @param pipeline : Info to build a graphics pipeline
// @param isCacheHash : TRUE if the hash is used by shader cache
// @param isRelocatableShader : TRUE if we are building relocatable shader
// @param stage : The stage for which we are building the hash. ShaderStageInvalid if building for the entire pipeline.
MetroHash::Hash PipelineDumper::generateHashForGraphicsPipeline(const GraphicsPipelineBuildInfo *pipeline,
                                                                bool isCacheHash, bool isRelocatableShader,
                                                                unsigned stage) {
  MetroHash64 hasher;

  switch (stage) {
  case ShaderStageVertex:
    updateHashForPipelineShaderInfo(ShaderStageVertex, &pipeline->vs, isCacheHash, &hasher, isRelocatableShader);
    break;
  case ShaderStageTessControl:
    updateHashForPipelineShaderInfo(ShaderStageTessControl, &pipeline->tcs, isCacheHash, &hasher, isRelocatableShader);
    break;
  case ShaderStageTessEval:
    updateHashForPipelineShaderInfo(ShaderStageTessEval, &pipeline->tes, isCacheHash, &hasher, isRelocatableShader);
    break;
  case ShaderStageGeometry:
    updateHashForPipelineShaderInfo(ShaderStageGeometry, &pipeline->gs, isCacheHash, &hasher, isRelocatableShader);
    break;
  case ShaderStageFragment:
    updateHashForPipelineShaderInfo(ShaderStageFragment, &pipeline->fs, isCacheHash, &hasher, isRelocatableShader);
    break;
  case ShaderStageInvalid:
    updateHashForPipelineShaderInfo(ShaderStageVertex, &pipeline->vs, isCacheHash, &hasher, isRelocatableShader);
    updateHashForPipelineShaderInfo(ShaderStageTessControl, &pipeline->tcs, isCacheHash, &hasher, isRelocatableShader);
    updateHashForPipelineShaderInfo(ShaderStageTessEval, &pipeline->tes, isCacheHash, &hasher, isRelocatableShader);
    updateHashForPipelineShaderInfo(ShaderStageGeometry, &pipeline->gs, isCacheHash, &hasher, isRelocatableShader);
    updateHashForPipelineShaderInfo(ShaderStageFragment, &pipeline->fs, isCacheHash, &hasher, isRelocatableShader);
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  hasher.Update(pipeline->iaState.deviceIndex);

  if (stage != ShaderStageFragment) {
    updateHashForVertexInputState(pipeline->pVertexInput, &hasher);
    updateHashForNonFragmentState(pipeline, isCacheHash, &hasher);
  }

  if (stage == ShaderStageFragment || stage == ShaderStageInvalid)
    updateHashForFragmentState(pipeline, &hasher);

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return hash;
}

// =====================================================================================================================
// Builds hash code from compute pipline build info.
//
// @param pipeline : Info to build a compute pipeline
// @param isCacheHash : TRUE if the hash is used by shader cache
MetroHash::Hash PipelineDumper::generateHashForComputePipeline(const ComputePipelineBuildInfo *pipeline,
                                                               bool isCacheHash, bool isRelocatableShader) {
  MetroHash64 hasher;

  updateHashForPipelineShaderInfo(ShaderStageCompute, &pipeline->cs, isCacheHash, &hasher, isRelocatableShader);
  hasher.Update(pipeline->deviceIndex);
  hasher.Update(pipeline->options.includeDisassembly);
  hasher.Update(pipeline->options.scalarBlockLayout);
  hasher.Update(pipeline->options.includeIr);
  hasher.Update(pipeline->options.robustBufferAccess);
  hasher.Update(pipeline->options.shadowDescriptorTableUsage);
  hasher.Update(pipeline->options.shadowDescriptorTablePtrHigh);

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return hash;
}

// =====================================================================================================================
// Updates hash code context for vertex input state
//
// @param vertexInput : Vertex input state
// @param [in,out] hasher : Haher to generate hash code
void PipelineDumper::updateHashForVertexInputState(const VkPipelineVertexInputStateCreateInfo *vertexInput,
                                                   MetroHash64 *hasher) {
  if (vertexInput && vertexInput->vertexBindingDescriptionCount > 0) {
    hasher->Update(vertexInput->vertexBindingDescriptionCount);
    hasher->Update(reinterpret_cast<const uint8_t *>(vertexInput->pVertexBindingDescriptions),
                   sizeof(VkVertexInputBindingDescription) * vertexInput->vertexBindingDescriptionCount);
    hasher->Update(vertexInput->vertexAttributeDescriptionCount);
    if (vertexInput->vertexAttributeDescriptionCount > 0) {
      hasher->Update(reinterpret_cast<const uint8_t *>(vertexInput->pVertexAttributeDescriptions),
                     sizeof(VkVertexInputAttributeDescription) * vertexInput->vertexAttributeDescriptionCount);
    }

    auto vertexDivisor = findVkStructInChain<VkPipelineVertexInputDivisorStateCreateInfoEXT>(
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, vertexInput->pNext);
    unsigned divisorCount = vertexDivisor ? vertexDivisor->vertexBindingDivisorCount : 0;
    hasher->Update(divisorCount);
    if (divisorCount > 0) {
      hasher->Update(reinterpret_cast<const uint8_t *>(vertexDivisor->pVertexBindingDivisors),
                     sizeof(VkVertexInputBindingDivisorDescriptionEXT) * divisorCount);
    }
  }
}

// =====================================================================================================================
// Update hash code from non-fragment pipeline state
//
// @param pipeline : Info to build a graphics pipeline
// @param isCacheHash : TRUE if the hash is used by shader cache
// @param [in,out] hasher : Hasher to generate hash code
void PipelineDumper::updateHashForNonFragmentState(const GraphicsPipelineBuildInfo *pipeline, bool isCacheHash,
                                                   MetroHash64 *hasher) {
  auto iaState = &pipeline->iaState;
  hasher->Update(iaState->topology);
  hasher->Update(iaState->patchControlPoints);
  hasher->Update(iaState->disableVertexReuse);
  hasher->Update(iaState->switchWinding);
  hasher->Update(iaState->enableMultiView);

  auto vpState = &pipeline->vpState;
  hasher->Update(vpState->depthClipEnable);

  auto rsState = &pipeline->rsState;
  hasher->Update(rsState->rasterizerDiscardEnable);

  auto nggState = &pipeline->nggState;
  bool enableNgg = nggState->enableNgg;
  bool passthroughMode = !nggState->enableVertexReuse && !nggState->enableBackfaceCulling &&
                         !nggState->enableFrustumCulling && !nggState->enableBoxFilterCulling &&
                         !nggState->enableSphereCulling && !nggState->enableSmallPrimFilter &&
                         !nggState->enableCullDistanceCulling;

  bool updateHashFromRs = (!isCacheHash);
  updateHashFromRs |= (enableNgg && !passthroughMode);

  if (updateHashFromRs) {
    hasher->Update(rsState->usrClipPlaneMask);
    hasher->Update(rsState->polygonMode);
    hasher->Update(rsState->cullMode);
    hasher->Update(rsState->frontFace);
    hasher->Update(rsState->depthBiasEnable);
  }

  if (isCacheHash) {
    hasher->Update(nggState->enableNgg);
    hasher->Update(nggState->enableGsUse);
    hasher->Update(nggState->forceNonPassthrough);
    hasher->Update(nggState->alwaysUsePrimShaderTable);
    hasher->Update(nggState->compactMode);
    hasher->Update(nggState->enableFastLaunch);
    hasher->Update(nggState->enableVertexReuse);
    hasher->Update(nggState->enableBackfaceCulling);
    hasher->Update(nggState->enableFrustumCulling);
    hasher->Update(nggState->enableBoxFilterCulling);
    hasher->Update(nggState->enableSphereCulling);
    hasher->Update(nggState->enableSmallPrimFilter);
    hasher->Update(nggState->enableCullDistanceCulling);
    hasher->Update(nggState->backfaceExponent);
    hasher->Update(nggState->subgroupSizing);
    hasher->Update(nggState->primsPerSubgroup);
    hasher->Update(nggState->vertsPerSubgroup);

    hasher->Update(pipeline->options.includeDisassembly);
    hasher->Update(pipeline->options.scalarBlockLayout);
    hasher->Update(pipeline->options.includeIr);
    hasher->Update(pipeline->options.robustBufferAccess);
    hasher->Update(pipeline->options.reconfigWorkgroupLayout);
    hasher->Update(pipeline->options.shadowDescriptorTableUsage);
    hasher->Update(pipeline->options.shadowDescriptorTablePtrHigh);
  }
}

// =====================================================================================================================
// Update hash code from fragment pipeline state
//
// @param pipeline : Info to build a graphics pipeline
// @param [in,out] hasher : Hasher to generate hash code
void PipelineDumper::updateHashForFragmentState(const GraphicsPipelineBuildInfo *pipeline, MetroHash64 *hasher) {
  auto rsState = &pipeline->rsState;
  hasher->Update(rsState->innerCoverage);
  hasher->Update(rsState->perSampleShading);
  hasher->Update(rsState->numSamples);
  hasher->Update(rsState->samplePatternIdx);

  auto cbState = &pipeline->cbState;
  hasher->Update(cbState->alphaToCoverageEnable);
  hasher->Update(cbState->dualSourceBlendEnable);
  for (unsigned i = 0; i < MaxColorTargets; ++i) {
    if (cbState->target[i].format != VK_FORMAT_UNDEFINED) {
      hasher->Update(cbState->target[i].channelWriteMask);
      hasher->Update(cbState->target[i].blendEnable);
      hasher->Update(cbState->target[i].blendSrcAlphaToColor);
      hasher->Update(cbState->target[i].format);
    }
  }
}

// =====================================================================================================================
// Updates hash code context for pipeline shader stage.
//
// @param stage : shader stage
// @param shaderInfo : Shader info in specified shader stage
// @param isCacheHash : TRUE if the hash is used by shader cache
// @param [in,out] hasher : Haher to generate hash code
void PipelineDumper::updateHashForPipelineShaderInfo(ShaderStage stage, const PipelineShaderInfo *shaderInfo,
                                                     bool isCacheHash, MetroHash64 *hasher, bool isRelocatableShader) {
  if (shaderInfo->pModuleData) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
    hasher->Update(stage);
    if (isCacheHash) {
      hasher->Update(static_cast<const uint8_t *>(voidPtrInc(moduleData, ShaderModuleCacheHashOffset)),
                     sizeof(moduleData->hash));
    } else
      hasher->Update(moduleData->hash);

    size_t entryNameLen = 0;
    if (shaderInfo->pEntryTarget) {
      entryNameLen = strlen(shaderInfo->pEntryTarget);
      hasher->Update(entryNameLen);
      hasher->Update(reinterpret_cast<const uint8_t *>(shaderInfo->pEntryTarget), entryNameLen);
    } else
      hasher->Update(entryNameLen);

    auto specializationInfo = shaderInfo->pSpecializationInfo;
    unsigned mapEntryCount = specializationInfo ? specializationInfo->mapEntryCount : 0;
    hasher->Update(mapEntryCount);
    if (mapEntryCount > 0) {
      hasher->Update(reinterpret_cast<const uint8_t *>(specializationInfo->pMapEntries),
                     sizeof(VkSpecializationMapEntry) * specializationInfo->mapEntryCount);
      hasher->Update(specializationInfo->dataSize);
      hasher->Update(reinterpret_cast<const uint8_t *>(specializationInfo->pData), specializationInfo->dataSize);
    }

    hasher->Update(shaderInfo->descriptorRangeValueCount);
    if (shaderInfo->descriptorRangeValueCount > 0) {
      for (unsigned i = 0; i < shaderInfo->descriptorRangeValueCount; ++i) {
        auto descriptorRangeValue = &shaderInfo->pDescriptorRangeValues[i];
        hasher->Update(descriptorRangeValue->type);
        hasher->Update(descriptorRangeValue->set);
        hasher->Update(descriptorRangeValue->binding);
        hasher->Update(descriptorRangeValue->arraySize);

        // TODO: We should query descriptor size from patch

        // The second part of DescriptorRangeValue is YCbCrMetaData, which is 4 DWORDS.
        // The hasher should be updated when the content changes, this is because YCbCrMetaData
        // is engaged in pipeline compiling.
        const unsigned descriptorSize =
            descriptorRangeValue->type != ResourceMappingNodeType::DescriptorYCbCrSampler ? 16 : 32;

        hasher->Update(reinterpret_cast<const uint8_t *>(descriptorRangeValue->pValue),
                       descriptorRangeValue->arraySize * descriptorSize);
      }
    }

    hasher->Update(shaderInfo->userDataNodeCount);
    if (shaderInfo->userDataNodeCount > 0) {
      for (unsigned i = 0; i < shaderInfo->userDataNodeCount; ++i) {
        auto userDataNode = &shaderInfo->pUserDataNodes[i];
        updateHashForResourceMappingNode(userDataNode, true, hasher, isRelocatableShader);
      }
    }

    if (isCacheHash) {
      auto &options = shaderInfo->options;
      hasher->Update(options.trapPresent);
      hasher->Update(options.debugMode);
      hasher->Update(options.enablePerformanceData);
      hasher->Update(options.allowReZ);
      hasher->Update(options.sgprLimit);
      hasher->Update(options.vgprLimit);
      hasher->Update(options.maxThreadGroupsPerComputeUnit);
      hasher->Update(options.waveSize);
      hasher->Update(options.wgpMode);
      hasher->Update(options.waveBreakSize);
      hasher->Update(options.forceLoopUnrollCount);
      hasher->Update(options.useSiScheduler);
      hasher->Update(options.updateDescInElf);
      hasher->Update(options.allowVaryWaveSize);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 33
      hasher->Update(options.enableLoadScalarizer);
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 35
      hasher->Update(options.disableLicm);
#endif
      hasher->Update(options.unrollThreshold);
      hasher->Update(options.scalarThreshold);
    }
  }
}

// =====================================================================================================================
// Updates hash code context for resource mapping node.
//
// NOTE: This function will be called recusively if node's type is "DescriptorTableVaPtr"
//
// @param userDataNode : Resource mapping node
// @param isRootNode : TRUE if the node is in root level
// @param [in,out] hasher : Haher to generate hash code
void PipelineDumper::updateHashForResourceMappingNode(const ResourceMappingNode *userDataNode, bool isRootNode,
                                                      MetroHash64 *hasher, bool isRelocatableShader) {
  hasher->Update(userDataNode->type);
  if (!isRelocatableShader) {
    hasher->Update(userDataNode->sizeInDwords);
    hasher->Update(userDataNode->offsetInDwords);
  }
  switch (userDataNode->type) {
  case ResourceMappingNodeType::DescriptorResource:
  case ResourceMappingNodeType::DescriptorSampler:
  case ResourceMappingNodeType::DescriptorYCbCrSampler:
  case ResourceMappingNodeType::DescriptorCombinedTexture:
  case ResourceMappingNodeType::DescriptorTexelBuffer:
  case ResourceMappingNodeType::DescriptorBuffer:
  case ResourceMappingNodeType::DescriptorFmask:
  case ResourceMappingNodeType::DescriptorBufferCompact: {
    hasher->Update(userDataNode->srdRange);
    break;
  }
  case ResourceMappingNodeType::DescriptorTableVaPtr: {
    for (unsigned i = 0; i < userDataNode->tablePtr.nodeCount; ++i)
      updateHashForResourceMappingNode(&userDataNode->tablePtr.pNext[i], false, hasher, isRelocatableShader);
    break;
  }
  case ResourceMappingNodeType::IndirectUserDataVaPtr: {
    hasher->Update(userDataNode->userDataPtr);
    break;
  }
  case ResourceMappingNodeType::StreamOutTableVaPtr: {
    // Do nothing for the stream-out table
    break;
  }
  case ResourceMappingNodeType::PushConst: {
    if (!isRootNode)
      hasher->Update(userDataNode->srdRange);
    break;
  }
  default: {
    llvm_unreachable("Should never be called!");
    break;
  }
  }
}

// =====================================================================================================================
// Outputs text with specified range to output stream.
template <class OStream>
//
// @param data : Text data
// @param startPos : Starting position
// @param endPos : End position
// @param [out] out : Output stream
void outputText(const uint8_t *data, unsigned startPos, unsigned endPos, OStream &out) {
  if (endPos > startPos) {
    // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
    // text print will be incorrect.
    uint8_t lastChar = data[endPos - 1];
    const_cast<uint8_t *>(data)[endPos - 1] = '\0';
    const char *end = reinterpret_cast<const char *>(&(data)[endPos]);
    // Output text
    const char *text = reinterpret_cast<const char *>(data + startPos);
    while (text != end) {
      out << text;
      text += strlen(text);
      text++;
    }

    if (lastChar != 0)
      out << static_cast<char>(lastChar);
    // Restore last character
    const_cast<uint8_t *>(data)[endPos - 1] = lastChar;
  }
}

// =====================================================================================================================
// Outputs binary data with specified range to output stream.
template <class OStream>
//
// @param data : Binary data
// @param startPos : Starting position
// @param endPos : End position
// @param [out] out : Output stream
void outputBinary(const uint8_t *data, unsigned startPos, unsigned endPos, OStream &out) {
  const unsigned *startData = reinterpret_cast<const unsigned *>(data + startPos);
  int dwordCount = (endPos - startPos) / sizeof(unsigned);
  char formatBuf[256];
  for (int i = 0; i < dwordCount; ++i) {
    size_t length = 0;
    if (i % 8 == 0) {
      length = snprintf(formatBuf, sizeof(formatBuf), "    %7u:", startPos + i * 4u);
      out << formatBuf;
    }
    length = snprintf(formatBuf, sizeof(formatBuf), "%08X", startData[i]);
    (void(length)); // unused
    out << formatBuf;

    if (i % 8 == 7)
      out << "\n";
    else
      out << " ";
  }

  if (endPos > startPos && (endPos - startPos) % sizeof(unsigned)) {
    int padPos = dwordCount * sizeof(unsigned);
    for (int i = padPos; i < endPos; ++i) {
      auto length = snprintf(formatBuf, sizeof(formatBuf), "%02X", data[i]);
      (void(length)); // unused
      out << formatBuf;
    }
  }

  if ((dwordCount % 8) != 0)
    out << "\n";
}

// =====================================================================================================================
//  Dumps ELF package to out stream
template <class OStream, class Elf>
//
// @param [out] out : Output stream
// @param reader : ELF object
OStream &operator<<(OStream &out, ElfReader<Elf> &reader) {
  unsigned sectionCount = reader.getSectionCount();
  char formatBuf[256];

  for (unsigned sortIdx = 0; sortIdx < sectionCount; ++sortIdx) {
    typename ElfReader<Elf>::SectionBuffer *section = nullptr;
    unsigned secIdx = 0;
    Result result = reader.getSectionDataBySortingIndex(sortIdx, &secIdx, &section);
    assert(result == Result::Success);
    (void(result)); // unused
    if (strcmp(section->name, ShStrTabName) == 0 || strcmp(section->name, StrTabName) == 0 ||
        strcmp(section->name, SymTabName) == 0) {
      // Output system section
      out << section->name << " (size = " << section->secHead.sh_size << " bytes)\n";
    } else if (strcmp(section->name, NoteName) == 0) {
      // Output .note section
      out << section->name << " (size = " << section->secHead.sh_size << " bytes)\n";
      unsigned offset = 0;
      const unsigned noteHeaderSize = sizeof(NoteHeader) - 8;
      while (offset < section->secHead.sh_size) {
        const NoteHeader *node = reinterpret_cast<const NoteHeader *>(section->data + offset);
        const unsigned noteNameSize = alignTo(node->nameSize, 4);
        switch (static_cast<unsigned>(node->type)) {
        case static_cast<unsigned>(Util::Abi::PipelineAbiNoteType::HsaIsa): {
          out << "    HsaIsa                       (name = " << node->name << "  size = " << node->descSize << ")\n";

          auto gpu = reinterpret_cast<const Util::Abi::AbiAmdGpuVersionNote *>(section->data + offset + noteHeaderSize +
                                                                               noteNameSize);

          out << "        vendorName  = " << gpu->vendorName << "\n";
          out << "        archName    = " << gpu->archName << "\n";
          out << "        gfxIp       = " << gpu->gfxipMajorVer << "." << gpu->gfxipMinorVer << "."
              << gpu->gfxipStepping << "\n";
          break;
        }
        case static_cast<unsigned>(Util::Abi::PipelineAbiNoteType::AbiMinorVersion): {
          out << "    AbiMinorVersion              (name = " << node->name << "  size = " << node->descSize << ")\n";

          auto codeVersion = reinterpret_cast<const Util::Abi::AbiMinorVersionNote *>(section->data + offset +
                                                                                      noteHeaderSize + noteNameSize);
          out << "        minor = " << codeVersion->minorVersion << "\n";
          break;
        }
        case static_cast<unsigned>(Util::Abi::PipelineAbiNoteType::PalMetadata): {
          out << "    PalMetadata                  (name = " << node->name << "  size = " << node->descSize << ")\n";

          auto buffer = section->data + offset + noteHeaderSize + noteNameSize;
          auto descSize = node->descSize;
          reader.initMsgPackDocument(buffer, descSize);

          do {
            auto node = reader.getMsgNode();
            auto msgIterStatus = reader.getMsgIteratorStatus();
            switch (node->getKind()) {
            case msgpack::Type::Int:
            case msgpack::Type::UInt: {
              if (msgIterStatus == MsgPackIteratorMapKey) {
                unsigned regId = static_cast<unsigned>(node->getUInt());
                const char *regName = PipelineDumper::getRegisterNameString(regId);

                auto length = snprintf(formatBuf, sizeof(formatBuf), "%-45s ", regName);
                (void(length)); // unused
                out << formatBuf;
              } else {
                auto length = snprintf(formatBuf, sizeof(formatBuf), "0x%016" PRIX64 " ", node->getUInt());
                (void(length)); // unused
                out << formatBuf;
              }
              break;
            }
            case msgpack::Type::String:
            case msgpack::Type::Binary: {
              outputText((const uint8_t *)(node->getString().data()), 0, node->getString().size(), out);
              if (msgIterStatus == MsgPackIteratorMapKey)
                out << ": ";
              break;
            }
            case msgpack::Type::Array: {
              if (msgIterStatus == MsgPackIteratorArray)
                out << "[ ";
              else
                out << "]";
              break;
            }
            case msgpack::Type::Map: {
              if (msgIterStatus == MsgPackIteratorMapPair) {
                out << "\n";
                for (unsigned i = 0; i < reader.getMsgMapLevel(); ++i)
                  out << "    ";
              } else if (msgIterStatus == MsgPackIteratorMapBegin)
                out << "{";
              else
                out << "}";
              break;
            }
            case msgpack::Type::Float: {
              out << node->getFloat() << " ";
              break;
            }
            case msgpack::Type::Nil: {
              break;
            }
            case msgpack::Type::Boolean: {
              out << node->getBool() << " ";
              break;
            }
            default: {
              llvm_unreachable("Should never be called!");
              break;
            }
            }

          } while (reader.getNextMsgNode());
          out << "\n";
          break;
        }
        default: {
          if (static_cast<unsigned>(node->type) == NT_AMD_AMDGPU_ISA) {
            out << "    IsaVersion                   (name = " << node->name << "  size = " << node->descSize << ")\n";
            auto desc = section->data + offset + noteHeaderSize + noteNameSize;
            outputText(desc, 0, node->descSize, out);
            out << "\n";
          } else {
            out << "    Unknown(" << (unsigned)node->type << ")                (name = " << node->name
                << "  size = " << node->descSize << ")\n";
            auto desc = section->data + offset + noteHeaderSize + noteNameSize;
            outputBinary(desc, 0, node->descSize, out);
          }
          break;
        }
        }
        offset += noteHeaderSize + noteNameSize + alignTo(node->descSize, sizeof(unsigned));
        assert(offset <= section->secHead.sh_size);
      }
    } else if (strcmp(section->name, RelocName) == 0) {
      // Output .reloc section
      out << section->name << " (size = " << section->secHead.sh_size << " bytes)\n";
      const unsigned relocCount = reader.getRelocationCount();
      for (unsigned i = 0; i < relocCount; ++i) {
        ElfReloc reloc = {};
        reader.getRelocation(i, &reloc);
        ElfSymbol elfSym = {};
        reader.getSymbol(reloc.symIdx, &elfSym);
        auto length = snprintf(formatBuf, sizeof(formatBuf), "    %-35s", elfSym.pSymName);
        (void(length)); // unused
        out << "#" << i << "    " << formatBuf << "    offset = " << reloc.offset << "\n";
      }
    } else if (strncmp(section->name, AmdGpuConfigName, sizeof(AmdGpuConfigName) - 1) == 0) {
      // Output .AMDGPU.config section
      const unsigned configCount = static_cast<unsigned>(section->secHead.sh_size / sizeof(unsigned) / 2);
      const unsigned *config = reinterpret_cast<const unsigned *>(section->data);
      out << section->name << " (" << configCount << " registers)\n";

      for (unsigned i = 0; i < configCount; ++i) {
        const char *regName = PipelineDumper::getRegisterNameString(config[2 * i] / 4);
        auto length = snprintf(formatBuf, sizeof(formatBuf), "        %-45s = 0x%08X\n", regName, config[2 * i + 1]);
        (void(length)); // unused
        out << formatBuf;
      }
    } else if (strncmp(section->name, AmdGpuDisasmName, sizeof(AmdGpuDisasmName) - 1) == 0 ||
               strncmp(section->name, AmdGpuCsdataName, sizeof(AmdGpuCsdataName) - 1) == 0 ||
               strncmp(section->name, CommentName, sizeof(CommentName) - 1) == 0) {
      // Output text based sections
      out << section->name << " (size = " << section->secHead.sh_size << " bytes)\n";

      std::vector<ElfSymbol> symbols;
      reader.GetSymbolsBySectionIndex(secIdx, symbols);
      unsigned symIdx = 0;
      unsigned startPos = 0;
      unsigned endPos = 0;
      while (startPos < section->secHead.sh_size) {
        if (symIdx < symbols.size())
          endPos = static_cast<unsigned>(symbols[symIdx].value);
        else
          endPos = static_cast<unsigned>(section->secHead.sh_size);

        outputText(section->data, startPos, endPos, out);
        out << "\n";

        if (symIdx < symbols.size()) {
          out << "    " << symbols[symIdx].pSymName << " (offset = " << symbols[symIdx].value
              << "  size = " << symbols[symIdx].size;
          MetroHash::Hash hash = {};
          MetroHash64::Hash(
              reinterpret_cast<const uint8_t *>(voidPtrInc(section->data, static_cast<size_t>(symbols[symIdx].value))),
              symbols[symIdx].size, hash.bytes);
          uint64_t hashCode64 = MetroHash::compact64(&hash);
          snprintf(formatBuf, sizeof(formatBuf), " hash = 0x%016" PRIX64 ")\n", hashCode64);
          out << formatBuf;
        }
        ++symIdx;
        startPos = endPos;
      }
    } else if (strncmp(section->name, Util::Abi::AmdGpuCommentName, sizeof(Util::Abi::AmdGpuCommentName) - 1) == 0) {
      auto name = section->name;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 475
      if (strncmp(name, Util::Abi::AmdGpuCommentAmdIlName, sizeof(Util::Abi::AmdGpuCommentAmdIlName) - 1) == 0)
#else
      if (strncmp(name, ".AMDGPU.comment.amdil", sizeof(".AMDGPU.comment.amdil") - 1) == 0)
#endif
      {
        // Output text based sections
        out << section->name << " (size = " << section->secHead.sh_size << " bytes)\n";

        std::vector<ElfSymbol> symbols;
        reader.GetSymbolsBySectionIndex(secIdx, symbols);
        unsigned symIdx = 0;
        unsigned startPos = 0;
        unsigned endPos = 0;
        while (startPos < section->secHead.sh_size) {
          if (symIdx < symbols.size())
            endPos = static_cast<unsigned>(symbols[symIdx].value);
          else
            endPos = static_cast<unsigned>(section->secHead.sh_size);

          outputText(section->data, startPos, endPos, out);
          out << "\n";

          if (symIdx < symbols.size()) {
            out << "    " << symbols[symIdx].pSymName << " (offset = " << symbols[symIdx].value
                << "  size = " << symbols[symIdx].size;
            MetroHash::Hash hash = {};
            MetroHash64::Hash(reinterpret_cast<const uint8_t *>(
                                  voidPtrInc(section->data, static_cast<size_t>(symbols[symIdx].value))),
                              symbols[symIdx].size, hash.bytes);
            uint64_t hashCode64 = MetroHash::compact64(&hash);
            snprintf(formatBuf, sizeof(formatBuf), " hash = 0x%016" PRIX64 ")\n", hashCode64);
            out << formatBuf;
          }
          ++symIdx;
          startPos = endPos;
        }
      } else {
        // Output text based sections
        out << section->name << " (size = " << section->secHead.sh_size << " bytes)\n";

        outputText(section->data, 0, static_cast<unsigned>(section->secHead.sh_size), out);
      }
    } else {
      // Output binary based sections
      out << (section->name[0] == 0 ? "(null)" : section->name) << " (size = " << section->secHead.sh_size
          << " bytes)\n";

      std::vector<ElfSymbol> symbols;
      reader.GetSymbolsBySectionIndex(secIdx, symbols);

      unsigned symIdx = 0;
      unsigned startPos = 0;
      unsigned endPos = 0;

      while (startPos < section->secHead.sh_size) {
        if (symIdx < symbols.size())
          endPos = static_cast<unsigned>(symbols[symIdx].value);
        else
          endPos = static_cast<unsigned>(section->secHead.sh_size);

        outputBinary(section->data, startPos, endPos, out);

        if (symIdx < symbols.size()) {
          out << "    " << symbols[symIdx].pSymName << " (offset = " << symbols[symIdx].value
              << "  size = " << symbols[symIdx].size;

          MetroHash::Hash hash = {};
          MetroHash64::Hash(
              reinterpret_cast<const uint8_t *>(voidPtrInc(section->data, static_cast<size_t>(symbols[symIdx].value))),
              symbols[symIdx].size, hash.bytes);
          uint64_t hashCode64 = MetroHash::compact64(&hash);
          snprintf(formatBuf, sizeof(formatBuf), " hash = 0x%016" PRIX64 ")\n", hashCode64);
          out << formatBuf;
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
#define CASE_CLASSENUM_TO_STRING(TYPE, ENUM)                                                                           \
  case TYPE::ENUM:                                                                                                     \
    string = #ENUM;                                                                                                    \
    break;
#define CASE_ENUM_TO_STRING(ENUM)                                                                                      \
  case ENUM:                                                                                                           \
    string = #ENUM;                                                                                                    \
    break;

// =====================================================================================================================
// Translates enum "VkVertexInputRate" to string and output to ostream.
//
// @param [out] out : Output stream
// @param inputRate : Vertex input rate
std::ostream &operator<<(std::ostream &out, VkVertexInputRate inputRate) {
  const char *string = nullptr;
  switch (inputRate) {
    CASE_ENUM_TO_STRING(VK_VERTEX_INPUT_RATE_VERTEX)
    CASE_ENUM_TO_STRING(VK_VERTEX_INPUT_RATE_INSTANCE)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return out << string;
}

// =====================================================================================================================
// Translates enum "ResourceMappingNodeType" to string and output to ostream.
//
// @param [out] out : Output stream
// @param type : Resource map node type
std::ostream &operator<<(std::ostream &out, ResourceMappingNodeType type) {
  return out << getResourceMappingNodeTypeName(type);
}

// =====================================================================================================================
// Translates enum "NggSubgroupSizingType" to string and output to ostream.
//
// @param [out] out : Output stream
// @param subgroupSizing : NGG sub-group sizing type
std::ostream &operator<<(std::ostream &out, NggSubgroupSizingType subgroupSizing) {
  const char *string = nullptr;
  switch (subgroupSizing) {
    CASE_CLASSENUM_TO_STRING(NggSubgroupSizingType, Auto)
    CASE_CLASSENUM_TO_STRING(NggSubgroupSizingType, MaximumSize)
    CASE_CLASSENUM_TO_STRING(NggSubgroupSizingType, HalfSize)
    CASE_CLASSENUM_TO_STRING(NggSubgroupSizingType, OptimizeForVerts)
    CASE_CLASSENUM_TO_STRING(NggSubgroupSizingType, OptimizeForPrims)
    CASE_CLASSENUM_TO_STRING(NggSubgroupSizingType, Explicit)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "NggCompactMode" to string and output to ostream.
//
// @param [out] out : Output stream
// @param compactMode : NGG compaction mode
std::ostream &operator<<(std::ostream &out, NggCompactMode compactMode) {
  const char *string = nullptr;
  switch (compactMode) {
    CASE_ENUM_TO_STRING(NggCompactSubgroup)
    CASE_ENUM_TO_STRING(NggCompactVertices)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "WaveBreakSize" to string and output to ostream.
//
// @param [out] out : Output stream
// @param waveBreakSize : Wave break size
std::ostream &operator<<(std::ostream &out, WaveBreakSize waveBreakSize) {
  const char *string = nullptr;
  switch (waveBreakSize) {
    CASE_CLASSENUM_TO_STRING(WaveBreakSize, None)
    CASE_CLASSENUM_TO_STRING(WaveBreakSize, _8x8)
    CASE_CLASSENUM_TO_STRING(WaveBreakSize, _16x16)
    CASE_CLASSENUM_TO_STRING(WaveBreakSize, _32x32)
    CASE_CLASSENUM_TO_STRING(WaveBreakSize, DrawTime)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "ShadowDescriptorTableUsage" to string and output to ostream.
//
// @param [out] out : Output stream
// @param shadowDescriptorTableUsage : Shadow descriptor table setting
std::ostream &operator<<(std::ostream &out, ShadowDescriptorTableUsage shadowDescriptorTableUsage) {
  const char *string = nullptr;
  switch (shadowDescriptorTableUsage) {
    CASE_CLASSENUM_TO_STRING(ShadowDescriptorTableUsage, Auto)
    CASE_CLASSENUM_TO_STRING(ShadowDescriptorTableUsage, Enable)
    CASE_CLASSENUM_TO_STRING(ShadowDescriptorTableUsage, Disable)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "VkPrimitiveTopology" to string and output to ostream.
//
// @param [out] out : Output stream
// @param topology : Primitive topology
std::ostream &operator<<(std::ostream &out, VkPrimitiveTopology topology) {
  const char *string = nullptr;
  switch (topology) {
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
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "VkPolygonMode" to string and output to ostream.
//
// @param [out] out : Output stream
// @param polygonMode : Rendering mode
std::ostream &operator<<(std::ostream &out, VkPolygonMode polygonMode) {
  const char *string = nullptr;
  switch (polygonMode) {
    CASE_ENUM_TO_STRING(VK_POLYGON_MODE_FILL)
    CASE_ENUM_TO_STRING(VK_POLYGON_MODE_LINE)
    CASE_ENUM_TO_STRING(VK_POLYGON_MODE_POINT)
    CASE_ENUM_TO_STRING(VK_POLYGON_MODE_FILL_RECTANGLE_NV)
    CASE_ENUM_TO_STRING(VK_POLYGON_MODE_MAX_ENUM)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "VkCullModeFlagBits" to string and output to ostream.
//
// @param [out] out : Output stream
// @param cullMode : Culling mode
std::ostream &operator<<(std::ostream &out, VkCullModeFlagBits cullMode) {
  const char *string = nullptr;
  switch (cullMode) {
    CASE_ENUM_TO_STRING(VK_CULL_MODE_NONE)
    CASE_ENUM_TO_STRING(VK_CULL_MODE_FRONT_BIT)
    CASE_ENUM_TO_STRING(VK_CULL_MODE_BACK_BIT)
    CASE_ENUM_TO_STRING(VK_CULL_MODE_FRONT_AND_BACK)
    CASE_ENUM_TO_STRING(VK_CULL_MODE_FLAG_BITS_MAX_ENUM)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "VkFrontFace" to string and output to ostream.
//
// @param [out] out : Output stream
// @param frontFace : Front facing orientation
std::ostream &operator<<(std::ostream &out, VkFrontFace frontFace) {
  const char *string = nullptr;
  switch (frontFace) {
    CASE_ENUM_TO_STRING(VK_FRONT_FACE_COUNTER_CLOCKWISE)
    CASE_ENUM_TO_STRING(VK_FRONT_FACE_CLOCKWISE)
    CASE_ENUM_TO_STRING(VK_FRONT_FACE_MAX_ENUM)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "VkFormat" to string and output to ostream.
//
// @param [out] out : Output stream
// @param format : Resource format
std::ostream &operator<<(std::ostream &out, VkFormat format) {
  const char *string = nullptr;
  switch (format) {
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
    llvm_unreachable("Should never be called!");
    break;
  }
  return out << string;
}

} // namespace Vkgc
