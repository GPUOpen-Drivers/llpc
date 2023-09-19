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
* @file  vkgcPipelineDumper.cpp
* @brief VKGC source file: contains implementation of VKGC pipeline dump utility.
***********************************************************************************************************************
*/
#include "vkgcPipelineDumper.h"
#include "vkgcElfReader.h"
#include "vkgcUtil.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unordered_set>

#define DEBUG_TYPE "vkgc-pipeline-dumper"

using namespace llvm;
using namespace MetroHash;
using namespace Util;

#if defined(_WIN32)
#define FILE_STAT _stat
#else
#define FILE_STAT stat
#endif

namespace Vkgc {

// Forward declaration
std::ostream &operator<<(std::ostream &out, VkVertexInputRate inputRate);
std::ostream &operator<<(std::ostream &out, VkFormat format);
std::ostream &operator<<(std::ostream &out, VkPrimitiveTopology topology);
std::ostream &operator<<(std::ostream &out, VkRayTracingShaderGroupTypeKHR type);
std::ostream &operator<<(std::ostream &out, ResourceMappingNodeType type);
std::ostream &operator<<(std::ostream &out, NggSubgroupSizingType subgroupSizing);
std::ostream &operator<<(std::ostream &out, DenormalMode denormalMode);
std::ostream &operator<<(std::ostream &out, WaveBreakSize waveBreakSize);
std::ostream &operator<<(std::ostream &out, ShadowDescriptorTableUsage shadowDescriptorTableUsage);
std::ostream &operator<<(std::ostream &out, VkProvokingVertexModeEXT provokingVertexMode);
std::ostream &operator<<(std::ostream &out, ResourceLayoutScheme layout);
std::ostream &operator<<(std::ostream &out, ThreadGroupSwizzleMode threadGroupSwizzleMode);
std::ostream &operator<<(std::ostream &out, InvariantLoads invariants);

template std::ostream &operator<<(std::ostream &out, ElfReader<Elf64> &reader);
template raw_ostream &operator<<(raw_ostream &out, ElfReader<Elf64> &reader);
template raw_fd_ostream &operator<<(raw_fd_ostream &out, ElfReader<Elf64> &reader);
constexpr size_t ShaderModuleCacheHashOffset = offsetof(ShaderModuleData, cacheHash);

// =====================================================================================================================
// Represents LLVM based mutex.
class Mutex {
public:
  Mutex() {}

  void lock() { m_mutex.lock(); }

  void unlock() { m_mutex.unlock(); }

private:
  sys::Mutex m_mutex;
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
    hash = PipelineDumper::generateHashForComputePipeline(pipelineInfo.pComputeInfo, false);
  else if (pipelineInfo.pRayTracingInfo)
    hash = PipelineDumper::generateHashForRayTracingPipeline(pipelineInfo.pRayTracingInfo, false);
  else {
    assert(pipelineInfo.pGraphicsInfo);
    UnlinkedShaderStage unlinkedStage = UnlinkedStageCount;
    if (pipelineInfo.pGraphicsInfo->unlinked) {
      if (pipelineInfo.pGraphicsInfo->fs.pModuleData)
        unlinkedStage = UnlinkedStageFragment;
      else
        unlinkedStage = UnlinkedStageVertexProcess;
    }
    hash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo.pGraphicsInfo, false, unlinkedStage);
  }

  return PipelineDumper::BeginPipelineDump(dumpOptions, pipelineInfo, MetroHash::compact64(&hash));
}

// =====================================================================================================================
// Begins to dump graphics/compute pipeline info.
//
// @param dumpOptions : Pipeline dump options
// @param pipelineInfo : Info of the pipeline to be built
// @param hash64 : hash uint64_t code
void *VKAPI_CALL IPipelineDumper::BeginPipelineDump(const PipelineDumpOptions *dumpOptions,
                                                    PipelineBuildInfo pipelineInfo, uint64_t hash64) {

  return PipelineDumper::BeginPipelineDump(dumpOptions, pipelineInfo, hash64);
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
// Calculates graphics shader binary hash code.
//
// @param pipelineInfo : Info to build this partial graphics pipeline
// @param stage : The shader stage for which the code is calculated
uint64_t VKAPI_CALL IPipelineDumper::GetGraphicsShaderBinaryHash(const GraphicsPipelineBuildInfo *pipelineInfo,
                                                                 ShaderStage stage) {
  UnlinkedShaderStage unlinkedStage;
  if (stage < ShaderStageFragment)
    unlinkedStage = UnlinkedStageVertexProcess;
  else
    unlinkedStage = UnlinkedStageFragment;
  auto hash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false, unlinkedStage);
  return MetroHash::compact64(&hash);
}

// =====================================================================================================================
// Calculates graphics pipeline hash code.
//
// @param pipelineInfo : Info to build this graphics pipeline
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(const GraphicsPipelineBuildInfo *pipelineInfo) {
  auto hash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false);
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
  auto hash = PipelineDumper::generateHashForGraphicsPipeline(graphicsPipelineInfo, false);
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pGraphicsInfo = graphicsPipelineInfo;
  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, MetroHash::compact64(&hash));
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
  auto hash = PipelineDumper::generateHashForComputePipeline(computePipelineInfo, false);
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pComputeInfo = computePipelineInfo;

  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, MetroHash::compact64(&hash));
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Get graphics pipeline name.
//
// @param [In] graphicsPipelineInfo : Info to build this graphics pipeline
// @param [Out] pipeNameOut : The full name of this graphics pipeline
// @param nameBufSize : Size of the buffer to store pipeline name
// @param hashCode64 : Precalculated Hash code of pipeline
void VKAPI_CALL IPipelineDumper::GetPipelineName(const GraphicsPipelineBuildInfo *graphicsPipelineInfo,
                                                 char *pipeNameOut, const size_t nameBufSize, uint64_t hashCode64) {
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pGraphicsInfo = graphicsPipelineInfo;
  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, hashCode64);
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Get compute pipeline name.
//
// @param [In] computePipelineInfo : Info to build this compute pipeline
// @param [Out] pipeNameOut : The full name of this compute pipeline
// @param nameBufSize : Size of the buffer to store pipeline name
// @param hashCode64 : Precalculated Hash code of pipeline
void VKAPI_CALL IPipelineDumper::GetPipelineName(const ComputePipelineBuildInfo *computePipelineInfo, char *pipeNameOut,
                                                 const size_t nameBufSize, uint64_t hashCode64) {
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pComputeInfo = computePipelineInfo;

  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, hashCode64);
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Calculates compute pipeline hash code.
//
// @param pipelineInfo : Info to build this compute pipeline
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(const ComputePipelineBuildInfo *pipelineInfo) {
  auto hash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, false);
  return MetroHash::compact64(&hash);
}

// =====================================================================================================================
// Calculates ray tracing pipeline hash code.
//
// @param pipelineInfo : Info to build this ray tracing pipeline
uint64_t VKAPI_CALL IPipelineDumper::GetPipelineHash(const RayTracingPipelineBuildInfo *pipelineInfo) {
  auto hash = PipelineDumper::generateHashForRayTracingPipeline(pipelineInfo, false);
  return MetroHash::compact64(&hash);
}

// =====================================================================================================================
// Gets ray tracing pipeline name.
//
// @param rtPipelineInfo : Info to build this ray tracing pipeline
// @param [Out] pipeNameOut : The full name of this ray tracing pipeline
// @param nameBufSize : Size of the buffer to store pipeline name
void VKAPI_CALL IPipelineDumper::GetPipelineName(const RayTracingPipelineBuildInfo *rtPipelineInfo, char *pipeNameOut,
                                                 const size_t nameBufSize) {
  auto hash = PipelineDumper::generateHashForRayTracingPipeline(rtPipelineInfo, false);
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pRayTracingInfo = rtPipelineInfo;
  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, MetroHash::compact64(&hash));
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
// Gets ray tracing pipeline name.
//
// @param rtPipelineInfo : Info to build this ray tracing pipeline
// @param [Out] pipeNameOut : The full name of this ray tracing pipeline
// @param nameBufSize : Size of the buffer to store pipeline name
// @param hashCode64 : Precalculated Hash code of pipeline
void VKAPI_CALL IPipelineDumper::GetPipelineName(const RayTracingPipelineBuildInfo *rtPipelineInfo, char *pipeNameOut,
                                                 const size_t nameBufSize, uint64_t hashCode64) {
  PipelineBuildInfo pipelineInfo = {};
  pipelineInfo.pRayTracingInfo = rtPipelineInfo;
  std::string pipeName = PipelineDumper::getPipelineInfoFileName(pipelineInfo, hashCode64);
  snprintf(pipeNameOut, nameBufSize, "%s", pipeName.c_str());
}

// =====================================================================================================================
/// Dumps ray tracing pipeline metadata.
///
/// @param [in]  dumpFile       The handle of pipeline dump file
/// @param [in]  pipelineMeta   Ray tracing pipeline metadata binary
void VKAPI_CALL IPipelineDumper::DumpRayTracingPipelineMetadata(void *dumpFile, BinaryData *pipelineMeta) {
  PipelineDumper::dumpRayTracingPipelineMetadata(reinterpret_cast<PipelineDumpFile *>(dumpFile), pipelineMeta);
}

// =====================================================================================================================
// Gets the file name of SPIR-V binary according the specified shader hash.
//
// @param hash : Shader hash code
std::string PipelineDumper::getSpirvBinaryFileName(const MetroHash::Hash *hash) {
  uint64_t hashCode64 = MetroHash::compact64(hash);
  char fileName[64] = {};
  snprintf(fileName, 64, "Shader_0x%016" PRIX64 ".spv", hashCode64);
  return std::string(fileName);
}

// =====================================================================================================================
// Gets the file name of pipeline info file according to the specified pipeline build info and pipeline hash.
//
// @param pipelineInfo : Info of the pipeline to be built
// @param hash : Pipeline hash code
std::string PipelineDumper::getPipelineInfoFileName(PipelineBuildInfo pipelineInfo, const uint64_t hashCode64) {
  char fileName[64] = {};
  if (pipelineInfo.pComputeInfo) {
    snprintf(fileName, 64, "PipelineCs_0x%016" PRIX64, hashCode64);
  } else if (pipelineInfo.pRayTracingInfo) {
    auto length = snprintf(fileName, 64, "PipelineRays_0x%016" PRIX64, hashCode64);
    (void(length)); // unused
  } else {
    assert(pipelineInfo.pGraphicsInfo);
    const char *fileNamePrefix = nullptr;
    if (pipelineInfo.pGraphicsInfo->unlinked) {
      if (pipelineInfo.pGraphicsInfo->task.pModuleData)
        fileNamePrefix = "PipelineLibTask";
      else if (pipelineInfo.pGraphicsInfo->vs.pModuleData)
        fileNamePrefix = "PipelineLibVs";
      else if (pipelineInfo.pGraphicsInfo->tcs.pModuleData)
        fileNamePrefix = "PipelineLibTcs";
      else if (pipelineInfo.pGraphicsInfo->tes.pModuleData)
        fileNamePrefix = "PipelineLibTes";
      else if (pipelineInfo.pGraphicsInfo->gs.pModuleData)
        fileNamePrefix = "PipelineLibGs";
      else if (pipelineInfo.pGraphicsInfo->mesh.pModuleData)
        fileNamePrefix = "PipelineLibMesh";
      else
        fileNamePrefix = "PipelineLibFs";
    } else if (pipelineInfo.pGraphicsInfo->tes.pModuleData && pipelineInfo.pGraphicsInfo->gs.pModuleData)
      fileNamePrefix = "PipelineGsTess";
    else if (pipelineInfo.pGraphicsInfo->gs.pModuleData)
      fileNamePrefix = "PipelineGs";
    else if (pipelineInfo.pGraphicsInfo->tes.pModuleData)
      fileNamePrefix = "PipelineTess";
    else if (pipelineInfo.pGraphicsInfo->task.pModuleData && pipelineInfo.pGraphicsInfo->mesh.pModuleData)
      fileNamePrefix = "PipelineTaskMesh";
    else if (pipelineInfo.pGraphicsInfo->mesh.pModuleData)
      fileNamePrefix = "PipelineMesh";
    else
      fileNamePrefix = "PipelineVsFs";

    snprintf(fileName, 64, "%s_0x%016" PRIX64, fileNamePrefix, hashCode64);
  }

  return std::string(fileName);
}

// =====================================================================================================================
// Begins to dump graphics/compute pipeline info.
//
// @param dumpOptions : Pipeline dump options
// @param pipelineInfo : Info of the pipeline to be built
// @param hash64 : Pipeline hash code
PipelineDumpFile *PipelineDumper::BeginPipelineDump(const PipelineDumpOptions *dumpOptions,
                                                    PipelineBuildInfo pipelineInfo, const uint64_t hash64) {
  bool disableLog = false;
  std::string dumpFileName;
  std::string dumpPathName;
  std::string dumpBinaryName;
  PipelineDumpFile *dumpFile = nullptr;

  // Filter pipeline hash
  if (dumpOptions->filterPipelineDumpByHash != 0) {
    if (hash64 != dumpOptions->filterPipelineDumpByHash)
      disableLog = true;
  }

  if (!disableLog) {
    // Filter pipeline type
    dumpFileName = getPipelineInfoFileName(pipelineInfo, hash64);
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

      if (pipelineInfo.pRayTracingInfo)
        dumpRayTracingPipelineInfo(&dumpFile->dumpFile, dumpOptions->pDumpDir, pipelineInfo.pRayTracingInfo);
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
// @param [out] dumpFile : Dump file
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
  case ResourceMappingNodeType::PushConst:
  case ResourceMappingNodeType::InlineBuffer:
  case ResourceMappingNodeType::DescriptorConstBuffer:
  case ResourceMappingNodeType::DescriptorConstBufferCompact:
  case ResourceMappingNodeType::DescriptorImage:
  case ResourceMappingNodeType::DescriptorConstTexelBuffer:
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 63
  case ResourceMappingNodeType::DescriptorAtomicCounter:
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
  case ResourceMappingNodeType::DescriptorMutable:
#endif
  {
    char setHexvalue[64] = {};
    snprintf(setHexvalue, 64, "0x%08" PRIX32, userDataNode->srdRange.set);
    dumpFile << prefix << ".set = " << setHexvalue << "\n";
    dumpFile << prefix << ".binding = " << userDataNode->srdRange.binding << "\n";
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
    dumpFile << prefix << ".strideInDwords = " << userDataNode->srdRange.strideInDwords << "\n";
#endif
    break;
  }
  case ResourceMappingNodeType::DescriptorTableVaPtr: {
    char prefixBuf[256];
    for (unsigned i = 0; i < userDataNode->tablePtr.nodeCount; ++i) {
      snprintf(prefixBuf, 256, "%s.next[%u]", prefix, i);
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
// @param [out] dumpFile : Dump file
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

  // Output pipeline shader options
  // clang-format off
  dumpFile << "options.trapPresent = " << shaderInfo->options.trapPresent << "\n";
  dumpFile << "options.debugMode = " << shaderInfo->options.debugMode << "\n";
  dumpFile << "options.enablePerformanceData = " << shaderInfo->options.enablePerformanceData << "\n";
  dumpFile << "options.allowReZ = " << shaderInfo->options.allowReZ << "\n";
  dumpFile << "options.forceLateZ = " << shaderInfo->options.forceLateZ << "\n";
  dumpFile << "options.vgprLimit = " << shaderInfo->options.vgprLimit << "\n";
  dumpFile << "options.sgprLimit = " << shaderInfo->options.sgprLimit << "\n";
  dumpFile << "options.maxThreadGroupsPerComputeUnit = " << shaderInfo->options.maxThreadGroupsPerComputeUnit << "\n";
  dumpFile << "options.waveSize = " << shaderInfo->options.waveSize << "\n";
  dumpFile << "options.subgroupSize = " << shaderInfo->options.subgroupSize << "\n";
  dumpFile << "options.wgpMode = " << shaderInfo->options.wgpMode << "\n";
  dumpFile << "options.waveBreakSize = " << shaderInfo->options.waveBreakSize << "\n";
  dumpFile << "options.forceLoopUnrollCount = " << shaderInfo->options.forceLoopUnrollCount << "\n";
  dumpFile << "options.useSiScheduler = " << shaderInfo->options.useSiScheduler << "\n";
  dumpFile << "options.disableCodeSinking = " << shaderInfo->options.disableCodeSinking << "\n";
  dumpFile << "options.favorLatencyHiding = " << shaderInfo->options.favorLatencyHiding << "\n";
  dumpFile << "options.allowVaryWaveSize = " << shaderInfo->options.allowVaryWaveSize << "\n";
  dumpFile << "options.enableLoadScalarizer = " << shaderInfo->options.enableLoadScalarizer << "\n";
  dumpFile << "options.disableLicm = " << shaderInfo->options.disableLicm << "\n";
  dumpFile << "options.unrollThreshold = " << shaderInfo->options.unrollThreshold << "\n";
  dumpFile << "options.scalarThreshold = " << shaderInfo->options.scalarThreshold << "\n";
  dumpFile << "options.disableLoopUnroll = " << shaderInfo->options.disableLoopUnroll << "\n";
  dumpFile << "options.fp32DenormalMode = " << shaderInfo->options.fp32DenormalMode << "\n";
  dumpFile << "options.adjustDepthImportVrs = " << shaderInfo->options.adjustDepthImportVrs << "\n";
  dumpFile << "options.disableLicmThreshold = " << shaderInfo->options.disableLicmThreshold << "\n";
  dumpFile << "options.unrollHintThreshold = " << shaderInfo->options.unrollHintThreshold << "\n";
  dumpFile << "options.dontUnrollHintThreshold = " << shaderInfo->options.dontUnrollHintThreshold << "\n";
  dumpFile << "options.fastMathFlags = " << shaderInfo->options.fastMathFlags << "\n";
  dumpFile << "options.disableFastMathFlags = " << shaderInfo->options.disableFastMathFlags << "\n";
  dumpFile << "options.ldsSpillLimitDwords = " << shaderInfo->options.ldsSpillLimitDwords << "\n";
  dumpFile << "options.scalarizeWaterfallLoads = " << shaderInfo->options.scalarizeWaterfallLoads << "\n";
  dumpFile << "options.overrideShaderThreadGroupSizeX = " << shaderInfo->options.overrideShaderThreadGroupSizeX << "\n";
  dumpFile << "options.overrideShaderThreadGroupSizeY = " << shaderInfo->options.overrideShaderThreadGroupSizeY << "\n";
  dumpFile << "options.overrideShaderThreadGroupSizeZ = " << shaderInfo->options.overrideShaderThreadGroupSizeZ << "\n";
  dumpFile << "options.nsaThreshold = " << shaderInfo->options.nsaThreshold << "\n";
  dumpFile << "options.aggressiveInvariantLoads = " << shaderInfo->options.aggressiveInvariantLoads << "\n";
  dumpFile << "options.workaroundStorageImageFormats = " << shaderInfo->options.workaroundStorageImageFormats << "\n";
  dumpFile << "options.workaroundInitializeOutputsToZero = " << shaderInfo->options.workaroundInitializeOutputsToZero << "\n";
  dumpFile << "options.disableFMA = " << shaderInfo->options.disableFMA << "\n";
  dumpFile << "options.backwardPropagateNoContract = " << shaderInfo->options.backwardPropagateNoContract << "\n";
  dumpFile << "options.forwardPropagateNoContract = " << shaderInfo->options.forwardPropagateNoContract << "\n";
  dumpFile << "\n";
  // clang-format on
}

// =====================================================================================================================
// Dumps resource node and static descriptor value data to file.
//
// @param resourceMapping : Pipeline resource mapping data
// @param [out] dumpFile : Dump file
void PipelineDumper::dumpResourceMappingInfo(const ResourceMappingData *resourceMapping, std::ostream &dumpFile) {
  dumpFile << "[ResourceMapping]\n";

  // Output descriptor range value
  if (resourceMapping->staticDescriptorValueCount > 0) {
    for (unsigned i = 0; i < resourceMapping->staticDescriptorValueCount; ++i) {
      auto staticDescriptorValue = &resourceMapping->pStaticDescriptorValues[i];
      dumpFile << "descriptorRangeValue[" << i << "].visibility = " << staticDescriptorValue->visibility << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].type = " << staticDescriptorValue->type << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].set = " << staticDescriptorValue->set << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].binding = " << staticDescriptorValue->binding << "\n";
      dumpFile << "descriptorRangeValue[" << i << "].arraySize = " << staticDescriptorValue->arraySize << "\n";
      for (unsigned j = 0; j < staticDescriptorValue->arraySize; ++j) {
        dumpFile << "descriptorRangeValue[" << i << "].uintData = ";
        const unsigned descriptorSizeInDw =
            4 + (staticDescriptorValue->type == ResourceMappingNodeType::DescriptorYCbCrSampler
                     ? (sizeof(SamplerYCbCrConversionMetaData) / 4)
                     : 0);

        for (unsigned k = 0; k < descriptorSizeInDw - 1; ++k)
          dumpFile << staticDescriptorValue->pValue[k] << ", ";
        dumpFile << staticDescriptorValue->pValue[descriptorSizeInDw - 1] << "\n";
      }
    }
    dumpFile << "\n";
  }

  // Output resource node mapping
  if (resourceMapping->userDataNodeCount > 0) {
    char prefixBuff[64] = {};
    for (unsigned i = 0; i < resourceMapping->userDataNodeCount; ++i) {
      auto userDataNode = &resourceMapping->pUserDataNodes[i];
      snprintf(prefixBuff, 64, "userDataNode[%u]", i);
      dumpFile << prefixBuff << ".visibility = " << userDataNode->visibility << "\n";
      dumpResourceMappingNode(&userDataNode->node, prefixBuff, dumpFile);
    }
    dumpFile << "\n";
  }
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

  // Make sure directory exists
  createDirectory(dumpDir);

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
// @param [out] dumpFile : Dump file
void PipelineDumper::dumpVersionInfo(std::ostream &dumpFile) {
  dumpFile << "[Version]\n";
  dumpFile << "version = " << Version << "\n\n";
}

// =====================================================================================================================
// Dumps compute pipeline state info to file.
//
// @param pipelineInfo : Info of the graphics pipeline to be built
// @param dumpDir : Directory of pipeline dump
// @param [out] dumpFile : Dump file
void PipelineDumper::dumpComputeStateInfo(const ComputePipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                          std::ostream &dumpFile) {
  dumpFile << "[ComputePipelineState]\n";

  // Output pipeline states
  dumpFile << "deviceIndex = " << pipelineInfo->deviceIndex << "\n";
  dumpPipelineOptions(&pipelineInfo->options, dumpFile);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  // Output shader library binary
  if (pipelineInfo->shaderLibrary.codeSize > 0) {
    MetroHash::Hash hash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(pipelineInfo->shaderLibrary.pCode),
                      pipelineInfo->shaderLibrary.codeSize, hash.bytes);
    DumpSpirvBinary(dumpDir, &pipelineInfo->shaderLibrary, &hash);

    std::string shaderLibraryName = getSpirvBinaryFileName(&hash);
    dumpFile << "shaderLibrary = " << shaderLibraryName << "\n";
  }
#endif

  dumpRayTracingRtState(&pipelineInfo->rtState, dumpDir, dumpFile);

  if (pipelineInfo->pUniformMap) {
    dumpFile << "\n[UniformConstant]\n";
    dumpFile << "uniformConstantMaps[0].visibility = " << pipelineInfo->pUniformMap->visibility << "\n";
    UniformConstantMapEntry *locationOffsetMap = pipelineInfo->pUniformMap->pUniforms;
    for (unsigned i = 0; i < pipelineInfo->pUniformMap->numUniformConstants; i++) {
      dumpFile << "uniformConstantMaps[0].uniformConstants[" << i << "].location = " << locationOffsetMap[i].location
               << "\n";
      dumpFile << "uniformConstantMaps[0].uniformConstants[" << i << "].offset = " << locationOffsetMap[i].offset
               << "\n";
    }
  }
}

// =====================================================================================================================
// Dumps pipeline options to file.
//
// @param options : Pipeline options
// @param [out] dumpFile : Dump file
void PipelineDumper::dumpPipelineOptions(const PipelineOptions *options, std::ostream &dumpFile) {
  dumpFile << "options.includeDisassembly = " << options->includeDisassembly << "\n";
  dumpFile << "options.scalarBlockLayout = " << options->scalarBlockLayout << "\n";
  dumpFile << "options.resourceLayoutScheme = " << options->resourceLayoutScheme << "\n";
  dumpFile << "options.includeIr = " << options->includeIr << "\n";
  dumpFile << "options.robustBufferAccess = " << options->robustBufferAccess << "\n";
  dumpFile << "options.reconfigWorkgroupLayout = " << options->reconfigWorkgroupLayout << "\n";
  dumpFile << "options.forceCsThreadIdSwizzling = " << options->forceCsThreadIdSwizzling << "\n";
  dumpFile << "options.overrideThreadGroupSizeX = " << options->overrideThreadGroupSizeX << "\n";
  dumpFile << "options.overrideThreadGroupSizeY = " << options->overrideThreadGroupSizeY << "\n";
  dumpFile << "options.overrideThreadGroupSizeZ = " << options->overrideThreadGroupSizeZ << "\n";
  dumpFile << "options.shadowDescriptorTableUsage = " << options->shadowDescriptorTableUsage << "\n";
  dumpFile << "options.shadowDescriptorTablePtrHigh = " << options->shadowDescriptorTablePtrHigh << "\n";
  dumpFile << "options.extendedRobustness.robustBufferAccess = " << options->extendedRobustness.robustBufferAccess
           << "\n";
  dumpFile << "options.extendedRobustness.robustImageAccess = " << options->extendedRobustness.robustImageAccess
           << "\n";
  dumpFile << "options.extendedRobustness.nullDescriptor = " << options->extendedRobustness.nullDescriptor << "\n";
  dumpFile << "options.optimizeTessFactor = " << options->optimizeTessFactor << "\n";
  dumpFile << "options.optimizationLevel = " << options->optimizationLevel << "\n";
  dumpFile << "options.threadGroupSwizzleMode = " << options->threadGroupSwizzleMode << "\n";
  dumpFile << "options.reverseThreadGroup = " << options->reverseThreadGroup << "\n";
  dumpFile << "options.enableImplicitInvariantExports = " << options->enableImplicitInvariantExports << "\n";
  dumpFile << "options.internalRtShaders = " << options->internalRtShaders << "\n";
  dumpFile << "options.forceNonUniformResourceIndexStageMask = " << options->forceNonUniformResourceIndexStageMask
           << "\n";
  dumpFile << "options.replaceSetWithResourceType = " << options->replaceSetWithResourceType << "\n";
  dumpFile << "options.disableSampleMask = " << options->disableSampleMask << "\n";
  dumpFile << "options.buildResourcesDataForShaderModule = " << options->buildResourcesDataForShaderModule << "\n";
  dumpFile << "options.disableTruncCoordForGather = " << options->disableTruncCoordForGather << "\n";
  dumpFile << "options.vertex64BitsAttribSingleLoc = " << options->vertex64BitsAttribSingleLoc << "\n";
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

  dumpResourceMappingInfo(&pipelineInfo->resourceMapping, *dumpFile);

  dumpComputeStateInfo(pipelineInfo, dumpDir, *dumpFile);

  dumpFile->flush();
}

// =====================================================================================================================
// Dumps graphics pipeline state info to file.
//
// @param pipelineInfo : Info of the graphics pipeline to be built
// @param dumpDir : Directory of pipeline dump
// @param [out] dumpFile : Dump file
void PipelineDumper::dumpGraphicsStateInfo(const GraphicsPipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                           std::ostream &dumpFile) {
  dumpFile << "[GraphicsPipelineState]\n";
  // Output pipeline states
  dumpFile << "topology = " << pipelineInfo->iaState.topology << "\n";
  dumpFile << "provokingVertexMode = " << pipelineInfo->rsState.provokingVertexMode << "\n";
  dumpFile << "patchControlPoints = " << pipelineInfo->iaState.patchControlPoints << "\n";
  dumpFile << "deviceIndex = " << pipelineInfo->iaState.deviceIndex << "\n";
  dumpFile << "disableVertexReuse = " << pipelineInfo->iaState.disableVertexReuse << "\n";
  dumpFile << "switchWinding = " << pipelineInfo->iaState.switchWinding << "\n";
  dumpFile << "enableMultiView = " << pipelineInfo->iaState.enableMultiView << "\n";
  if (pipelineInfo->iaState.tessLevel) {
    dumpFile << "tessLevelInner[0] = " << pipelineInfo->iaState.tessLevel->inner[0] << "\n";
    dumpFile << "tessLevelInner[1] = " << pipelineInfo->iaState.tessLevel->inner[1] << "\n";
    dumpFile << "tessLevelOuter[0] = " << pipelineInfo->iaState.tessLevel->outer[0] << "\n";
    dumpFile << "tessLevelOuter[1] = " << pipelineInfo->iaState.tessLevel->outer[1] << "\n";
    dumpFile << "tessLevelOuter[2] = " << pipelineInfo->iaState.tessLevel->outer[2] << "\n";
    dumpFile << "tessLevelOuter[3] = " << pipelineInfo->iaState.tessLevel->outer[3] << "\n";
  }
  dumpFile << "depthClipEnable = " << pipelineInfo->vpState.depthClipEnable << "\n";

  dumpFile << "rasterizerDiscardEnable = " << pipelineInfo->rsState.rasterizerDiscardEnable << "\n";
  dumpFile << "perSampleShading = " << pipelineInfo->rsState.perSampleShading << "\n";
  dumpFile << "numSamples = " << pipelineInfo->rsState.numSamples << "\n";
  dumpFile << "pixelShaderSamples = " << pipelineInfo->rsState.pixelShaderSamples << "\n";
  dumpFile << "samplePatternIdx = " << pipelineInfo->rsState.samplePatternIdx << "\n";
  dumpFile << "dynamicSampleInfo = " << pipelineInfo->rsState.dynamicSampleInfo << "\n";
  dumpFile << "rasterStream = " << pipelineInfo->rsState.rasterStream << "\n";
  dumpFile << "usrClipPlaneMask = " << static_cast<unsigned>(pipelineInfo->rsState.usrClipPlaneMask) << "\n";
  dumpFile << "alphaToCoverageEnable = " << pipelineInfo->cbState.alphaToCoverageEnable << "\n";
  dumpFile << "dualSourceBlendEnable = " << pipelineInfo->cbState.dualSourceBlendEnable << "\n";
  dumpFile << "dualSourceBlendDynamic = " << pipelineInfo->cbState.dualSourceBlendDynamic << "\n";

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
  dumpFile << "nggState.forceCullingMode = " << pipelineInfo->nggState.forceCullingMode << "\n";
  dumpFile << "nggState.compactVertex = " << pipelineInfo->nggState.compactVertex << "\n";
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
  dumpFile << "dynamicVertexStride = " << pipelineInfo->dynamicVertexStride << "\n";
  dumpFile << "enableUberFetchShader = " << pipelineInfo->enableUberFetchShader << "\n";
  dumpFile << "enableEarlyCompile = " << pipelineInfo->enableEarlyCompile << "\n";
  dumpFile << "enableColorExportShader = " << pipelineInfo->enableColorExportShader << "\n";
  dumpPipelineOptions(&pipelineInfo->options, dumpFile);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  // Output shader library binary
  if (pipelineInfo->shaderLibrary.codeSize > 0) {
    MetroHash::Hash hash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(pipelineInfo->shaderLibrary.pCode),
                      pipelineInfo->shaderLibrary.codeSize, hash.bytes);
    DumpSpirvBinary(dumpDir, &pipelineInfo->shaderLibrary, &hash);

    std::string shaderLibraryName = getSpirvBinaryFileName(&hash);
    dumpFile << "shaderLibrary = " << shaderLibraryName << "\n";
  }
#endif

  dumpRayTracingRtState(&pipelineInfo->rtState, dumpDir, dumpFile);
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

  if (pipelineInfo->numUniformConstantMaps != 0) {
    dumpFile << "\n[UniformConstant]\n";
    for (unsigned s = 0; s < pipelineInfo->numUniformConstantMaps; s++) {
      if (!pipelineInfo->ppUniformMaps[s])
        continue;
      dumpFile << "uniformConstantMaps[" << s << "].visibility = " << pipelineInfo->ppUniformMaps[s]->visibility
               << "\n";
      UniformConstantMapEntry *locationOffsetMap = pipelineInfo->ppUniformMaps[s]->pUniforms;
      for (unsigned i = 0; i < pipelineInfo->ppUniformMaps[s]->numUniformConstants; i++) {
        dumpFile << "uniformConstantMaps[" << s << "].uniformConstants[" << i
                 << "].location = " << locationOffsetMap[i].location << "\n";
        dumpFile << "uniformConstantMaps[" << s << "].uniformConstants[" << i
                 << "].offset = " << locationOffsetMap[i].offset << "\n";
      }
    }
  }

  dumpFile << "\n[ApiXfbOutInfo]\n";
  dumpFile << "forceDisableStreamOut = " << pipelineInfo->apiXfbOutData.forceDisableStreamOut << "\n";
  dumpFile << "forceEnablePrimStats = " << pipelineInfo->apiXfbOutData.forceEnablePrimStats << "\n";
  const auto pXfbOutInfos = pipelineInfo->apiXfbOutData.pXfbOutInfos;
  for (unsigned idx = 0; idx < pipelineInfo->apiXfbOutData.numXfbOutInfo; ++idx) {
    dumpFile << "xfbOutInfo[" << idx << "].isBuiltIn = " << pXfbOutInfos[idx].isBuiltIn << "\n";
    dumpFile << "xfbOutInfo[" << idx << "].location = " << pXfbOutInfos[idx].location << "\n";
    dumpFile << "xfbOutInfo[" << idx << "].component = " << pXfbOutInfos[idx].component << "\n";
    dumpFile << "xfbOutInfo[" << idx << "].xfbBuffer = " << pXfbOutInfos[idx].xfbBuffer << "\n";
    dumpFile << "xfbOutInfo[" << idx << "].xfbOffset = " << pXfbOutInfos[idx].xfbOffset << "\n";
    dumpFile << "xfbOutInfo[" << idx << "].xfbStride = " << pXfbOutInfos[idx].xfbStride << "\n";
    dumpFile << "xfbOutInfo[" << idx << "].streamId = " << pXfbOutInfos[idx].streamId << "\n";
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
  // clang-format off
  const PipelineShaderInfo *shaderInfos[ShaderStageGfxCount] = {
    &pipelineInfo->task,
    &pipelineInfo->vs,
    &pipelineInfo->tcs,
    &pipelineInfo->tes,
    &pipelineInfo->gs,
    &pipelineInfo->mesh,
    &pipelineInfo->fs,
  };
  // clang-format on
  for (unsigned stage = 0; stage < ShaderStageGfxCount; ++stage) {
    const PipelineShaderInfo *shaderInfo = shaderInfos[stage];
    if (!shaderInfo->pModuleData)
      continue;
    dumpPipelineShaderInfo(shaderInfo, *dumpFile);
  }

  dumpResourceMappingInfo(&pipelineInfo->resourceMapping, *dumpFile);

  dumpGraphicsStateInfo(pipelineInfo, dumpDir, *dumpFile);

  dumpFile->flush();
}

// =====================================================================================================================
// Dumps ray tracing pipeline build info to file.
//
// @param dumpFile : Pipeline dump file
// @param dumpDir : Directory of pipeline dump
// @param pipelineInfo : Info of the ray tracing pipeline to be built
void PipelineDumper::dumpRayTracingPipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                                const RayTracingPipelineBuildInfo *pipelineInfo) {
  dumpVersionInfo(*dumpFile);

  // Dump pipeline
  for (unsigned i = 0; i < pipelineInfo->shaderCount; ++i) {
    const PipelineShaderInfo *shaderInfo = &pipelineInfo->pShaders[i];

    dumpPipelineShaderInfo(shaderInfo, *dumpFile);
  }

  dumpResourceMappingInfo(&pipelineInfo->resourceMapping, *dumpFile);

  dumpRayTracingStateInfo(pipelineInfo, dumpDir, *dumpFile);

  dumpFile->flush();
}

// =====================================================================================================================
// Dumps ray tracing pipeline state info to file.
//
// @param pipelineInfo : Info of the ray tracing pipeline to be built
// @param dumpDir : Directory of pipeline dump
// @param dumpFile : Pipeline dump file
void PipelineDumper::dumpRayTracingStateInfo(const RayTracingPipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                             std::ostream &dumpFile) {
  dumpFile << "[RayTracingPipelineState]\n";

  // Output pipeline states
  dumpFile << "deviceIndex = " << pipelineInfo->deviceIndex << "\n";
  dumpPipelineOptions(&pipelineInfo->options, dumpFile);

  // Output shader groups
  for (unsigned i = 0; i < pipelineInfo->shaderGroupCount; ++i) {
    auto shaderGroup = &pipelineInfo->pShaderGroups[i];
    dumpFile << "groups[" << i << "].type = " << shaderGroup->type << "\n";
    dumpFile << "groups[" << i << "].generalShader = " << static_cast<int>(shaderGroup->generalShader) << "\n";
    dumpFile << "groups[" << i << "].closestHitShader = " << static_cast<int>(shaderGroup->closestHitShader) << "\n";
    dumpFile << "groups[" << i << "].anyHitShader = " << static_cast<int>(shaderGroup->anyHitShader) << "\n";
    dumpFile << "groups[" << i << "].intersectionShader = " << static_cast<int>(shaderGroup->intersectionShader)
             << "\n";
  }

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  // Output trace ray shader binary
  MetroHash::Hash hash = {};
  MetroHash64::Hash(reinterpret_cast<const uint8_t *>(pipelineInfo->shaderTraceRay.pCode),
                    pipelineInfo->shaderTraceRay.codeSize, hash.bytes);
  DumpSpirvBinary(dumpDir, &pipelineInfo->shaderTraceRay, &hash);

  std::string traceRayName = getSpirvBinaryFileName(&hash);
  dumpFile << "shaderTraceRay = " << traceRayName << "\n";
#endif

  dumpFile << "maxRecursionDepth = " << pipelineInfo->maxRecursionDepth << "\n";
  dumpFile << "indirectStageMask = " << pipelineInfo->indirectStageMask << "\n";
  dumpFile << "mode = " << static_cast<unsigned>(pipelineInfo->mode) << "\n";
  dumpRayTracingRtState(&pipelineInfo->rtState, dumpDir, dumpFile);
  dumpFile << "payloadSizeMaxInLib = " << pipelineInfo->payloadSizeMaxInLib << "\n";
  dumpFile << "attributeSizeMaxInLib = " << pipelineInfo->attributeSizeMaxInLib << "\n";
  dumpFile << "hasPipelineLibrary = " << pipelineInfo->hasPipelineLibrary << "\n";
  dumpFile << "pipelineLibStageMask = " << pipelineInfo->pipelineLibStageMask << "\n";
}

// =====================================================================================================================
// Dumps ray tracing pipeline state info to file.
//
// @param rtState : Pipeline ray tracing state
// @param dumpDir : Directory of pipeline dump
// @param dumpFile : Pipeline dump file
void PipelineDumper::dumpRayTracingRtState(const RtState *rtState, const char *dumpDir, std::ostream &dumpStream) {
  dumpStream << "rtState.bvhResDescSize = " << rtState->bvhResDesc.dataSizeInDwords << "\n";
  for (unsigned i = 0; i < rtState->bvhResDesc.dataSizeInDwords; ++i)
    dumpStream << "rtState.bvhResDesc[" << i << "] = " << rtState->bvhResDesc.descriptorData[i] << "\n";

  dumpStream << "rtState.nodeStrideShift = " << rtState->nodeStrideShift << "\n";
  dumpStream << "rtState.staticPipelineFlags = " << rtState->staticPipelineFlags << "\n";
  dumpStream << "rtState.triCompressMode = " << rtState->triCompressMode << "\n";
  dumpStream << "rtState.pipelineFlags = " << rtState->pipelineFlags << "\n";
  dumpStream << "rtState.threadGroupSizeX = " << rtState->threadGroupSizeX << "\n";
  dumpStream << "rtState.threadGroupSizeY = " << rtState->threadGroupSizeY << "\n";
  dumpStream << "rtState.threadGroupSizeZ = " << rtState->threadGroupSizeZ << "\n";
  dumpStream << "rtState.boxSortHeuristicMode = " << rtState->boxSortHeuristicMode << "\n";
  dumpStream << "rtState.counterMode = " << rtState->counterMode << "\n";
  dumpStream << "rtState.counterMask = " << rtState->counterMask << "\n";
  dumpStream << "rtState.rayQueryCsSwizzle = " << rtState->rayQueryCsSwizzle << "\n";
  dumpStream << "rtState.ldsStackSize = " << rtState->ldsStackSize << "\n";
  dumpStream << "rtState.dispatchRaysThreadGroupSize = " << rtState->dispatchRaysThreadGroupSize << "\n";
  dumpStream << "rtState.ldsSizePerThreadGroup = " << rtState->ldsSizePerThreadGroup << "\n";
  dumpStream << "rtState.outerTileSize = " << rtState->outerTileSize << "\n";
  dumpStream << "rtState.dispatchDimSwizzleMode = " << rtState->dispatchDimSwizzleMode << "\n";
  dumpStream << "rtState.exportConfig.indirectCallingConvention = " << rtState->exportConfig.indirectCallingConvention
             << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.raygen = "
             << rtState->exportConfig.indirectCalleeSavedRegs.raygen << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.miss = "
             << rtState->exportConfig.indirectCalleeSavedRegs.miss << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.closestHit = "
             << rtState->exportConfig.indirectCalleeSavedRegs.closestHit << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.anyHit = "
             << rtState->exportConfig.indirectCalleeSavedRegs.anyHit << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.intersection = "
             << rtState->exportConfig.indirectCalleeSavedRegs.intersection << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.callable = "
             << rtState->exportConfig.indirectCalleeSavedRegs.callable << "\n";
  dumpStream << "rtState.exportConfig.indirectCalleeSavedRegs.traceRays = "
             << rtState->exportConfig.indirectCalleeSavedRegs.traceRays << "\n";
  dumpStream << "rtState.exportConfig.enableUniformNoReturn = " << rtState->exportConfig.enableUniformNoReturn << "\n";
  dumpStream << "rtState.exportConfig.enableTraceRayArgsInLds = " << rtState->exportConfig.enableTraceRayArgsInLds
             << "\n";
  dumpStream << "rtState.exportConfig.readsDispatchRaysIndex = " << rtState->exportConfig.readsDispatchRaysIndex
             << "\n";
  dumpStream << "rtState.exportConfig.enableDynamicLaunch = " << rtState->exportConfig.enableDynamicLaunch << "\n";
  dumpStream << "rtState.exportConfig.emitRaytracingShaderDataToken = "
             << rtState->exportConfig.emitRaytracingShaderDataToken << "\n";
  dumpStream << "rtState.enableRayQueryCsSwizzle = " << rtState->enableRayQueryCsSwizzle << "\n";
  dumpStream << "rtState.enableDispatchRaysInnerSwizzle = " << rtState->enableDispatchRaysInnerSwizzle << "\n";
  dumpStream << "rtState.enableDispatchRaysOuterSwizzle = " << rtState->enableDispatchRaysOuterSwizzle << "\n";
  dumpStream << "rtState.forceInvalidAccelStruct = " << rtState->forceInvalidAccelStruct << "\n";
  dumpStream << "rtState.enableRayTracingCounters = " << rtState->enableRayTracingCounters << "\n";
  dumpStream << "rtState.enableRayTracingHwTraversalStack = " << rtState->enableRayTracingHwTraversalStack << "\n";
  dumpStream << "rtState.enableOptimalLdsStackSizeForIndirect = " << rtState->enableOptimalLdsStackSizeForIndirect
             << "\n";
  dumpStream << "rtState.enableOptimalLdsStackSizeForUnified = " << rtState->enableOptimalLdsStackSizeForUnified
             << "\n";
  dumpStream << "rtState.maxRayLength = " << rtState->maxRayLength << "\n";
  dumpStream << "rtState.gpurtFeatureFlags = " << rtState->gpurtFeatureFlags << "\n";

  if (rtState->gpurtShaderLibrary.codeSize > 0) {
    // Output GPURT shader library binary
    MetroHash::Hash hash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(rtState->gpurtShaderLibrary.pCode),
                      rtState->gpurtShaderLibrary.codeSize, hash.bytes);
    DumpSpirvBinary(dumpDir, &rtState->gpurtShaderLibrary, &hash);

    std::string libraryName = getSpirvBinaryFileName(&hash);
    dumpStream << "rtState.gpurtShaderLibrary = " << libraryName << "\n";
  }

  for (unsigned i = 0; i < RT_ENTRY_FUNC_COUNT; ++i) {
    dumpStream << "rtState.gpurtFuncTable.pFunc[" << i << "] = " << rtState->gpurtFuncTable.pFunc[i] << "\n";
  }
  dumpStream << "rtState.rtIpVersion = " << rtState->rtIpVersion.major << "." << rtState->rtIpVersion.minor << "\n";
  dumpStream << "rtState.gpurtOverride = " << rtState->gpurtOverride << "\n";
  dumpStream << "rtState.rtIpOverride = " << rtState->rtIpOverride << "\n";
}

// =====================================================================================================================
/// Dumps ray tracing pipeline metadata.
///
/// @param [in]  dumpFile       The pointer of pipeline dump file
/// @param [in]  pipelineMeta   Ray tracing pipeline metadata binary
void PipelineDumper::dumpRayTracingPipelineMetadata(PipelineDumpFile *dumpFile, const BinaryData *pipelineMeta) {
  if (!dumpFile)
    return;

  if (!pipelineMeta->pCode || pipelineMeta->codeSize == 0)
    return;

  auto extPos = dumpFile->binaryFileName.rfind('.');
  assert(extPos != std::string::npos);
  std::string metaFileName = dumpFile->binaryFileName.substr(0, extPos) + ".meta";

  dumpFile->binaryFile.open(metaFileName.c_str(), std::ostream::out | std::ostream::binary);
  if (!dumpFile->binaryFile.bad()) {
    dumpFile->binaryFile.write(reinterpret_cast<const char *>(pipelineMeta->pCode), pipelineMeta->codeSize);
    dumpFile->binaryFile.close();
  }
}

// =====================================================================================================================
// Update hash code for the pipeline rtstate
//
// @param rtState : Pipeline rtstate
// @param [in,out] hasher : Haher to generate hash code
// @param isCacheHash : TRUE if hash is used by the shader cache
void PipelineDumper::updateHashForRtState(const RtState *rtState, MetroHash64 *hasher, bool isCacheHash) {
  hasher->Update(rtState->nodeStrideShift);
  hasher->Update(rtState->staticPipelineFlags);
  hasher->Update(rtState->triCompressMode);
  hasher->Update(rtState->threadGroupSizeX);
  hasher->Update(rtState->threadGroupSizeY);
  hasher->Update(rtState->threadGroupSizeZ);
  for (unsigned i = 0; i < rtState->bvhResDesc.dataSizeInDwords; ++i)
    hasher->Update(rtState->bvhResDesc.descriptorData[i]);

  hasher->Update(rtState->counterMask);
  hasher->Update(rtState->rayQueryCsSwizzle);
  hasher->Update(rtState->ldsStackSize);
  hasher->Update(rtState->dispatchRaysThreadGroupSize);
  hasher->Update(rtState->ldsSizePerThreadGroup);
  hasher->Update(rtState->outerTileSize);
  hasher->Update(rtState->dispatchDimSwizzleMode);
  hasher->Update(rtState->exportConfig.indirectCallingConvention);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.raygen);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.miss);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.closestHit);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.anyHit);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.intersection);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.callable);
  hasher->Update(rtState->exportConfig.indirectCalleeSavedRegs.traceRays);
  hasher->Update(rtState->exportConfig.enableUniformNoReturn);
  hasher->Update(rtState->exportConfig.enableTraceRayArgsInLds);
  hasher->Update(rtState->exportConfig.readsDispatchRaysIndex);
  hasher->Update(rtState->exportConfig.enableDynamicLaunch);
  hasher->Update(rtState->exportConfig.emitRaytracingShaderDataToken);
  hasher->Update(rtState->enableRayQueryCsSwizzle);
  hasher->Update(rtState->enableDispatchRaysInnerSwizzle);
  hasher->Update(rtState->enableDispatchRaysOuterSwizzle);
  hasher->Update(rtState->forceInvalidAccelStruct);
  hasher->Update(rtState->enableRayTracingCounters);
  hasher->Update(rtState->enableRayTracingHwTraversalStack);
  hasher->Update(rtState->enableOptimalLdsStackSizeForIndirect);
  hasher->Update(rtState->enableOptimalLdsStackSizeForUnified);
  hasher->Update(rtState->maxRayLength);

  if (isCacheHash) {
    hasher->Update(rtState->gpurtFeatureFlags);

    hasher->Update(rtState->gpurtShaderLibrary.codeSize);
    if (rtState->gpurtShaderLibrary.codeSize > 0) {
      hasher->Update(static_cast<const uint8_t *>(rtState->gpurtShaderLibrary.pCode),
                     rtState->gpurtShaderLibrary.codeSize);
    }

    for (unsigned i = 0; i < RT_ENTRY_FUNC_COUNT; ++i) {
      size_t funcNameLen = 0;
      if (rtState->gpurtFuncTable.pFunc[i]) {
        funcNameLen = strlen(rtState->gpurtFuncTable.pFunc[i]);
        hasher->Update(funcNameLen);
        hasher->Update(reinterpret_cast<const uint8_t *>(rtState->gpurtFuncTable.pFunc[i]), funcNameLen);
      } else {
        hasher->Update(funcNameLen);
      }
    }

    hasher->Update(rtState->rtIpVersion);
    hasher->Update(rtState->gpurtOverride);
    hasher->Update(rtState->rtIpOverride);
  }
}

// =====================================================================================================================
// Builds hash code from graphics pipeline build info.  If stage is a specific stage of the graphics pipeline, then only
// the portions of the pipeline build info that affect that stage will be included in the hash.  Otherwise, stage must
// be ShaderStageInvalid, and all values in the build info will be included.
//
// @param pipeline : Info to build a graphics pipeline
// @param isCacheHash : TRUE if the hash is used by shader cache
// @param isRelocatableShader : TRUE if we are building relocatable shader
// @param stage : The stage for which we are building the hash. ShaderStageInvalid if building for the entire pipeline.
MetroHash::Hash PipelineDumper::generateHashForGraphicsPipeline(const GraphicsPipelineBuildInfo *pipeline,
                                                                bool isCacheHash,
                                                                UnlinkedShaderStage unlinkedShaderType) {
  MetroHash64 hasher;

  switch (unlinkedShaderType) {
  case UnlinkedStageVertexProcess:
    updateHashForPipelineShaderInfo(ShaderStageTask, &pipeline->task, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageVertex, &pipeline->vs, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageTessControl, &pipeline->tcs, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageTessEval, &pipeline->tes, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageGeometry, &pipeline->gs, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageMesh, &pipeline->mesh, isCacheHash, &hasher);
    break;
  case UnlinkedStageFragment:
    updateHashForPipelineShaderInfo(ShaderStageFragment, &pipeline->fs, isCacheHash, &hasher);
    break;
  case UnlinkedStageCount:
    updateHashForPipelineShaderInfo(ShaderStageTask, &pipeline->task, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageVertex, &pipeline->vs, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageTessControl, &pipeline->tcs, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageTessEval, &pipeline->tes, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageGeometry, &pipeline->gs, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageMesh, &pipeline->mesh, isCacheHash, &hasher);
    updateHashForPipelineShaderInfo(ShaderStageFragment, &pipeline->fs, isCacheHash, &hasher);
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  updateHashForResourceMappingInfo(&pipeline->resourceMapping, pipeline->unlinked ? 0 : pipeline->pipelineLayoutApiHash,
                                   &hasher);
  hasher.Update(pipeline->iaState.deviceIndex);

  // Relocatable shaders force an unlinked compilation.
  hasher.Update(pipeline->unlinked);
  hasher.Update(pipeline->enableEarlyCompile);
  if (unlinkedShaderType == UnlinkedStageFragment)
    hasher.Update(pipeline->enableColorExportShader);
  updateHashForPipelineOptions(&pipeline->options, &hasher, isCacheHash, unlinkedShaderType);

  if (unlinkedShaderType != UnlinkedStageFragment) {
    if (!pipeline->enableUberFetchShader)
      updateHashForVertexInputState(pipeline->pVertexInput, pipeline->dynamicVertexStride, &hasher);
    updateHashForNonFragmentState(pipeline, isCacheHash, &hasher);
  }

  if (unlinkedShaderType != UnlinkedStageVertexProcess)
    updateHashForFragmentState(pipeline, &hasher);

  updateHashForRtState(&pipeline->rtState, &hasher, isCacheHash);

  if (pipeline->iaState.tessLevel)
    hasher.Update(*pipeline->iaState.tessLevel);

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return hash;
}

// =====================================================================================================================
// Builds hash code from compute pipeline build info.
//
// @param pipeline : Info to build a compute pipeline
// @param isCacheHash : TRUE if the hash is used by shader cache
// @param isRelocatableShader : TRUE if we are building relocatable shader
MetroHash::Hash PipelineDumper::generateHashForComputePipeline(const ComputePipelineBuildInfo *pipeline,
                                                               bool isCacheHash) {
  MetroHash64 hasher;

  updateHashForPipelineShaderInfo(ShaderStageCompute, &pipeline->cs, isCacheHash, &hasher);

  updateHashForResourceMappingInfo(&pipeline->resourceMapping, pipeline->pipelineLayoutApiHash, &hasher);

  hasher.Update(pipeline->deviceIndex);

  updateHashForPipelineOptions(&pipeline->options, &hasher, isCacheHash, UnlinkedStageCompute);

  updateHashForRtState(&pipeline->rtState, &hasher, isCacheHash);

  // Relocatable shaders force an unlinked compilation.
  hasher.Update(pipeline->unlinked);

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return hash;
}

// =====================================================================================================================
// Builds hash code from ray tracing pipeline build info.
//
// @param pipeline : Info to build a ray tracing pipeline
// @param isCacheHash : TRUE if the hash is used by shader cache
MetroHash::Hash PipelineDumper::generateHashForRayTracingPipeline(const RayTracingPipelineBuildInfo *pipeline,
                                                                  bool isCacheHash) {
  MetroHash64 hasher;

  hasher.Update(pipeline->shaderCount);
  for (unsigned i = 0; i < pipeline->shaderCount; ++i) {
    updateHashForPipelineShaderInfo(pipeline->pShaders[i].entryStage, &pipeline->pShaders[i], isCacheHash, &hasher);
  }

  updateHashForResourceMappingInfo(&pipeline->resourceMapping, pipeline->pipelineLayoutApiHash, &hasher);

  hasher.Update(pipeline->deviceIndex);

  updateHashForPipelineOptions(&pipeline->options, &hasher, isCacheHash, UnlinkedStageRayTracing);

  hasher.Update(pipeline->shaderGroupCount);
  for (unsigned i = 0; i < pipeline->shaderGroupCount; ++i) {
    auto shaderGroup = &pipeline->pShaderGroups[i];
    hasher.Update(shaderGroup->type);
    hasher.Update(shaderGroup->generalShader);
    hasher.Update(shaderGroup->closestHitShader);
    hasher.Update(shaderGroup->anyHitShader);
    hasher.Update(shaderGroup->intersectionShader);
  }

  hasher.Update(pipeline->maxRecursionDepth);
  hasher.Update(pipeline->indirectStageMask);
  hasher.Update(pipeline->mode);
  updateHashForRtState(&pipeline->rtState, &hasher, isCacheHash);

  hasher.Update(pipeline->payloadSizeMaxInLib);
  hasher.Update(pipeline->attributeSizeMaxInLib);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 62
  if (isCacheHash) {
    hasher.Update(pipeline->shaderTraceRay.codeSize);
    if (pipeline->shaderTraceRay.codeSize > 0) {
      hasher.Update(static_cast<const uint8_t *>(pipeline->shaderTraceRay.pCode), pipeline->shaderTraceRay.codeSize);
    }
  }
#endif

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return hash;
}

// =====================================================================================================================
// Updates hash code context for vertex input state
//
// @param vertexInput : Vertex input state
// @param [in/out] hasher : Haher to generate hash code
void PipelineDumper::updateHashForVertexInputState(const VkPipelineVertexInputStateCreateInfo *vertexInput,
                                                   bool dynamicVertexStride, MetroHash64 *hasher) {
  if (vertexInput && vertexInput->vertexBindingDescriptionCount > 0) {
    hasher->Update(vertexInput->vertexBindingDescriptionCount);
    if (dynamicVertexStride) {
      for (uint32_t i = 0; i < vertexInput->vertexBindingDescriptionCount; i++) {
        auto attribBinding = vertexInput->pVertexBindingDescriptions[i];
        attribBinding.stride = 0;
        hasher->Update(attribBinding);
      }
    } else {
      hasher->Update(reinterpret_cast<const uint8_t *>(vertexInput->pVertexBindingDescriptions),
                     sizeof(VkVertexInputBindingDescription) * vertexInput->vertexBindingDescriptionCount);
    }
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
// @param [in/out] hasher : Hasher to generate hash code
// @param isRelocatableShader : TRUE if we are building relocatable shader
void PipelineDumper::updateHashForNonFragmentState(const GraphicsPipelineBuildInfo *pipeline, bool isCacheHash,
                                                   MetroHash64 *hasher) {
  auto nggState = &pipeline->nggState;
  bool enableNgg = nggState->enableNgg;

  auto iaState = &pipeline->iaState;
  if (enableNgg) {
    hasher->Update(iaState->topology);
    hasher->Update(pipeline->rsState.provokingVertexMode);
  }

  if (pipeline->gs.pModuleData || pipeline->tcs.pModuleData || pipeline->tes.pModuleData)
    hasher->Update(iaState->patchControlPoints);
  hasher->Update(iaState->disableVertexReuse);
  hasher->Update(iaState->switchWinding);
  hasher->Update(iaState->enableMultiView);

  auto vpState = &pipeline->vpState;
  hasher->Update(vpState->depthClipEnable);

  auto rsState = &pipeline->rsState;
  hasher->Update(rsState->rasterizerDiscardEnable);

  hasher->Update(pipeline->dynamicVertexStride);
  hasher->Update(pipeline->enableUberFetchShader);

  bool passthroughMode = !nggState->enableBackfaceCulling && !nggState->enableFrustumCulling &&
                         !nggState->enableBoxFilterCulling && !nggState->enableSphereCulling &&
                         !nggState->enableSmallPrimFilter && !nggState->enableCullDistanceCulling;

  bool updateHashFromRs = (!isCacheHash);
  updateHashFromRs |= (enableNgg && !passthroughMode);

  if (updateHashFromRs) {
    hasher->Update(rsState->usrClipPlaneMask);
    hasher->Update(rsState->rasterStream);
  }

  if (isCacheHash) {
    hasher->Update(nggState->enableNgg);
    if (nggState->enableNgg) {
      hasher->Update(nggState->enableGsUse);
      hasher->Update(nggState->forceCullingMode);
      hasher->Update(nggState->compactVertex);
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
    }
  }

  hasher->Update(pipeline->apiXfbOutData.forceDisableStreamOut);
  hasher->Update(pipeline->apiXfbOutData.forceEnablePrimStats);
}

// =====================================================================================================================
// Update hash code from fragment pipeline state
//
// @param pipeline : Info to build a graphics pipeline
// @param [in/out] hasher : Hasher to generate hash code
// @param isRelocatableShader : TRUE if we are building relocatable shader
void PipelineDumper::updateHashForFragmentState(const GraphicsPipelineBuildInfo *pipeline, MetroHash64 *hasher) {
  auto rsState = &pipeline->rsState;
  hasher->Update(rsState->perSampleShading);
  hasher->Update(rsState->provokingVertexMode);
  hasher->Update(rsState->pixelShaderSamples);

  // Topology is required when BaryCoord is used
  hasher->Update(pipeline->iaState.topology);

  // View index in fragment shader depends on the enablement of multi-view
  hasher->Update(pipeline->iaState.enableMultiView);

  hasher->Update(rsState->innerCoverage);
  hasher->Update(rsState->numSamples);
  hasher->Update(rsState->samplePatternIdx);
  hasher->Update(rsState->rasterStream);
  hasher->Update(rsState->dynamicSampleInfo);

  auto cbState = &pipeline->cbState;
  hasher->Update(cbState->alphaToCoverageEnable);
  hasher->Update(cbState->dualSourceBlendEnable);
  hasher->Update(cbState->dualSourceBlendDynamic);
  for (unsigned i = 0; i < MaxColorTargets; ++i) {
    hasher->Update(cbState->target[i].channelWriteMask);
    hasher->Update(cbState->target[i].blendEnable);
    hasher->Update(cbState->target[i].blendSrcAlphaToColor);
    hasher->Update(cbState->target[i].format);
  }
}

// =====================================================================================================================
// Update hash code from pipeline options
//
// @param options: Pipeline options
// @param [in/out] hasher : Hasher to generate hash code
// @param isCacheHash : True if the hash will be used as a key for a cache lookup.
// @param isRelocatableShader : TRUE if we are building a relocatable shader
// @param stage : The unlinked shader stage that should be included in the hash.
void PipelineDumper::updateHashForPipelineOptions(const PipelineOptions *options, MetroHash64 *hasher, bool isCacheHash,
                                                  UnlinkedShaderStage stage) {
  hasher->Update(options->includeDisassembly);
  hasher->Update(options->scalarBlockLayout);
  hasher->Update(options->includeIr);
  hasher->Update(options->robustBufferAccess);
  hasher->Update(options->reconfigWorkgroupLayout);
  hasher->Update(options->forceCsThreadIdSwizzling);
  hasher->Update(options->overrideThreadGroupSizeX);
  hasher->Update(options->overrideThreadGroupSizeY);
  hasher->Update(options->overrideThreadGroupSizeZ);
  hasher->Update(options->enableRelocatableShaderElf);
  hasher->Update(options->disableImageResourceCheck);
  hasher->Update(options->enableScratchAccessBoundsChecks);
  hasher->Update(options->enableImplicitInvariantExports);
  hasher->Update(options->resourceLayoutScheme);

  hasher->Update(options->shadowDescriptorTableUsage);
  hasher->Update(options->shadowDescriptorTablePtrHigh);

  hasher->Update(options->extendedRobustness.robustBufferAccess);
  hasher->Update(options->extendedRobustness.robustImageAccess);
  hasher->Update(options->extendedRobustness.nullDescriptor);
  if (stage != UnlinkedStageCompute) {
    hasher->Update(options->optimizeTessFactor);
  }

  if (stage == UnlinkedStageFragment || stage == UnlinkedStageCount) {
    hasher->Update(options->enableInterpModePatch);
    hasher->Update(options->disableSampleMask);
  }

  hasher->Update(options->pageMigrationEnabled);
  hasher->Update(options->optimizationLevel);
  hasher->Update(options->threadGroupSwizzleMode);
  hasher->Update(options->reverseThreadGroup);
  hasher->Update(options->internalRtShaders);
  hasher->Update(options->forceNonUniformResourceIndexStageMask);
  hasher->Update(options->replaceSetWithResourceType);
  hasher->Update(options->disableTruncCoordForGather);
}

// =====================================================================================================================
// Updates hash code context for pipeline shader stage.
//
// @param stage : Shader stage
// @param shaderInfo : Shader info in specified shader stage
// @param isCacheHash : TRUE if the hash is used by shader cache
// @param [in/out] hasher : Hasher to generate hash code
// @param isRelocatableShader : TRUE if we are building relocatable shader
void PipelineDumper::updateHashForPipelineShaderInfo(ShaderStage stage, const PipelineShaderInfo *shaderInfo,
                                                     bool isCacheHash, MetroHash64 *hasher) {
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

    if (isCacheHash) {
      auto &options = shaderInfo->options;
      hasher->Update(options.trapPresent);
      hasher->Update(options.debugMode);
      hasher->Update(options.enablePerformanceData);
      hasher->Update(options.allowReZ);
      hasher->Update(options.forceLateZ);
      hasher->Update(options.sgprLimit);
      hasher->Update(options.vgprLimit);
      hasher->Update(options.maxThreadGroupsPerComputeUnit);
      hasher->Update(options.waveSize);
      hasher->Update(options.subgroupSize);
      hasher->Update(options.wgpMode);
      hasher->Update(options.waveBreakSize);
      hasher->Update(options.forceLoopUnrollCount);
      hasher->Update(options.useSiScheduler);
      hasher->Update(options.disableCodeSinking);
      hasher->Update(options.favorLatencyHiding);
      hasher->Update(options.allowVaryWaveSize);
      hasher->Update(options.enableLoadScalarizer);
      hasher->Update(options.disableLicm);
      hasher->Update(options.unrollThreshold);
      hasher->Update(options.scalarThreshold);
      hasher->Update(options.disableLoopUnroll);
      hasher->Update(options.fp32DenormalMode);
      hasher->Update(options.adjustDepthImportVrs);
      hasher->Update(options.disableLicmThreshold);
      hasher->Update(options.unrollHintThreshold);
      hasher->Update(options.dontUnrollHintThreshold);
      hasher->Update(options.fastMathFlags);
      hasher->Update(options.disableFastMathFlags);
      hasher->Update(options.ldsSpillLimitDwords);
      hasher->Update(options.scalarizeWaterfallLoads);
      hasher->Update(options.overrideShaderThreadGroupSizeX);
      hasher->Update(options.overrideShaderThreadGroupSizeY);
      hasher->Update(options.overrideShaderThreadGroupSizeZ);
      hasher->Update(options.nsaThreshold);
      hasher->Update(options.aggressiveInvariantLoads);
      hasher->Update(options.workaroundStorageImageFormats);
      hasher->Update(options.workaroundInitializeOutputsToZero);
      hasher->Update(options.disableFMA);
      hasher->Update(options.backwardPropagateNoContract);
      hasher->Update(options.forwardPropagateNoContract);
    }
  }
}

// =====================================================================================================================
// Updates hash code context for resource node and static descriptor value data.
//
// @param resourceMapping : Pipeline resource mapping data.
// @param [in,out] hasher : Haher to generate hash code.
// @param stage : The stage for which we are building the hash. ShaderStageInvalid if building for the entire pipeline.
void PipelineDumper::updateHashForResourceMappingInfo(const ResourceMappingData *resourceMapping,
                                                      const uint64_t pipelineLayoutApiHash, MetroHash64 *hasher,
                                                      ShaderStage stage) {
  if ((pipelineLayoutApiHash > 0) && (stage == ShaderStageInvalid || stage == ShaderStageCompute)) {
    hasher->Update(reinterpret_cast<const uint8_t *>(&pipelineLayoutApiHash), sizeof(pipelineLayoutApiHash));
  } else {
    hasher->Update(resourceMapping->staticDescriptorValueCount);
    if (resourceMapping->staticDescriptorValueCount > 0) {
      for (unsigned i = 0; i < resourceMapping->staticDescriptorValueCount; ++i) {
        auto staticDescriptorValue = &resourceMapping->pStaticDescriptorValues[i];
        if (stage == ShaderStageInvalid || (staticDescriptorValue->visibility & shaderStageToMask(stage))) {
          if (stage == ShaderStageInvalid)
            hasher->Update(staticDescriptorValue->visibility);
          hasher->Update(staticDescriptorValue->type);
          hasher->Update(staticDescriptorValue->set);
          hasher->Update(staticDescriptorValue->binding);
          hasher->Update(staticDescriptorValue->arraySize);
        }

        // TODO: We should query descriptor size from patch

        // The second part of StaticDescriptorValue is YCbCrMetaData, which is 4 dwords.
        // The hasher should be updated when the content changes, this is because YCbCrMetaData
        // is engaged in pipeline compiling.
        const unsigned descriptorSize =
            16 + (staticDescriptorValue->type != ResourceMappingNodeType::DescriptorYCbCrSampler
                      ? 0
                      : sizeof(SamplerYCbCrConversionMetaData));

        hasher->Update(reinterpret_cast<const uint8_t *>(staticDescriptorValue->pValue),
                       staticDescriptorValue->arraySize * descriptorSize);
      }
    }

    hasher->Update(resourceMapping->userDataNodeCount);
    if (resourceMapping->userDataNodeCount > 0) {
      for (unsigned i = 0; i < resourceMapping->userDataNodeCount; ++i) {
        auto userDataNode = &resourceMapping->pUserDataNodes[i];
        if (stage == ShaderStageInvalid || (userDataNode->visibility & shaderStageToMask(stage))) {
          if (stage == ShaderStageInvalid)
            hasher->Update(userDataNode->visibility);
          updateHashForResourceMappingNode(&userDataNode->node, true, hasher);
        }
      }
    }
  }
}

// =====================================================================================================================
// Updates hash code context for resource mapping node.
//
// NOTE: This function will be called recursively if node's type is "DescriptorTableVaPtr"
//
// @param userDataNode : Resource mapping node
// @param isRootNode : TRUE if the node is in root level
// @param [in/out] hasher : Haher to generate hash code
void PipelineDumper::updateHashForResourceMappingNode(const ResourceMappingNode *userDataNode, bool isRootNode,
                                                      MetroHash64 *hasher) {
  hasher->Update(userDataNode->type);
  hasher->Update(userDataNode->sizeInDwords);
  hasher->Update(userDataNode->offsetInDwords);
  switch (userDataNode->type) {
  case ResourceMappingNodeType::DescriptorResource:
  case ResourceMappingNodeType::DescriptorSampler:
  case ResourceMappingNodeType::DescriptorYCbCrSampler:
  case ResourceMappingNodeType::DescriptorCombinedTexture:
  case ResourceMappingNodeType::DescriptorTexelBuffer:
  case ResourceMappingNodeType::DescriptorBuffer:
  case ResourceMappingNodeType::DescriptorFmask:
  case ResourceMappingNodeType::DescriptorBufferCompact:
  case ResourceMappingNodeType::DescriptorConstBuffer:
  case ResourceMappingNodeType::DescriptorConstBufferCompact:
  case ResourceMappingNodeType::DescriptorImage:
  case ResourceMappingNodeType::DescriptorConstTexelBuffer:
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 63
  case ResourceMappingNodeType::DescriptorAtomicCounter:
#endif
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 61
  case ResourceMappingNodeType::DescriptorMutable:
#endif
  {
    hasher->Update(userDataNode->srdRange);
    break;
  }
  case ResourceMappingNodeType::DescriptorTableVaPtr: {
    for (unsigned i = 0; i < userDataNode->tablePtr.nodeCount; ++i)
      updateHashForResourceMappingNode(&userDataNode->tablePtr.pNext[i], false, hasher);
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
  case ResourceMappingNodeType::InlineBuffer:
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

const Hash PipelineDumper::generateHashForGlueShader(BinaryData glueShaderString) {
  MetroHash64 hasher;
  hasher.Update(reinterpret_cast<const uint8_t *>(glueShaderString.pCode), glueShaderString.codeSize);
  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);
  return hash;
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
    if (i % 8 == 0) {
      snprintf(formatBuf, sizeof(formatBuf), "    %7u:", startPos + i * 4u);
      out << formatBuf;
    }
    // 'data' may not be aligned to sizeof(unsigned) so use a temporary to avoid
    // undefined behaviour.
    unsigned alignedData;
    memcpy(&alignedData, startData + i, sizeof(alignedData));
    snprintf(formatBuf, sizeof(formatBuf), "%08X", alignedData);
    out << formatBuf;

    if (i % 8 == 7)
      out << "\n";
    else
      out << " ";
  }

  if (endPos > startPos && (endPos - startPos) % sizeof(unsigned)) {
    int padPos = dwordCount * sizeof(unsigned);
    for (int i = padPos; i < endPos; ++i) {
      snprintf(formatBuf, sizeof(formatBuf), "%02X", data[i]);
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
        case static_cast<unsigned>(Util::Abi::MetadataNoteType): {
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

                snprintf(formatBuf, sizeof(formatBuf), "%-45s ", regName);
                out << formatBuf;
              } else {
                snprintf(formatBuf, sizeof(formatBuf), "0x%016" PRIX64 " ", node->getUInt());
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
        snprintf(formatBuf, sizeof(formatBuf), "    %-35s", elfSym.pSymName);
        out << "#" << i << "    " << formatBuf << "    offset = " << reloc.offset << "\n";
      }
    } else if (strncmp(section->name, AmdGpuConfigName, sizeof(AmdGpuConfigName) - 1) == 0) {
      // Output .AMDGPU.config section
      const unsigned configCount = static_cast<unsigned>(section->secHead.sh_size / sizeof(unsigned) / 2);
      const unsigned *config = reinterpret_cast<const unsigned *>(section->data);
      out << section->name << " (" << configCount << " registers)\n";

      for (unsigned i = 0; i < configCount; ++i) {
        const char *regName = PipelineDumper::getRegisterNameString(config[2 * i] / 4);
        snprintf(formatBuf, sizeof(formatBuf), "        %-45s = 0x%08X\n", regName, config[2 * i + 1]);
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

      if (strncmp(name, Util::Abi::AmdGpuCommentAmdIlName, sizeof(Util::Abi::AmdGpuCommentAmdIlName) - 1) == 0) {
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
// @param subgroupSizing : NGG subgroup sizing type
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
// Translates enum "DenormalMode" to string and output to ostream.
//
// @param [out] out : Output stream
// @param denormalMode : Denormal mode
std::ostream &operator<<(std::ostream &out, DenormalMode denormalMode) {
  const char *string = nullptr;
  switch (denormalMode) {
    CASE_CLASSENUM_TO_STRING(DenormalMode, Auto)
    CASE_CLASSENUM_TO_STRING(DenormalMode, FlushToZero)
    CASE_CLASSENUM_TO_STRING(DenormalMode, Preserve)
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
// Translates enum "VkRayTracingShaderGroupTypeKHR" to string and output to ostream.
//
// @param [out] out : Output stream
// @param type : Ray tracing shader group type
std::ostream &operator<<(std::ostream &out, VkRayTracingShaderGroupTypeKHR type) {
  const char *string = nullptr;
  switch (type) {
    CASE_ENUM_TO_STRING(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR)
    CASE_ENUM_TO_STRING(VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR)
    CASE_ENUM_TO_STRING(VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR)
    CASE_ENUM_TO_STRING(VK_RAY_TRACING_SHADER_GROUP_TYPE_MAX_ENUM_KHR)
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
    CASE_ENUM_TO_STRING(VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT)
    CASE_ENUM_TO_STRING(VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT)

    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return out << string;
}

// =====================================================================================================================
// Translates enum "VkProvokingVertexModeEXT" to string and output to ostream.
//
// @param [out] out : Output stream
// @param provokingVertexMode : Provoking vertex mode
std::ostream &operator<<(std::ostream &out, VkProvokingVertexModeEXT provokingVertexMode) {
  const char *string = nullptr;
  switch (provokingVertexMode) {
    CASE_ENUM_TO_STRING(VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT)
    CASE_ENUM_TO_STRING(VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return out << string;
}

// =====================================================================================================================
// Translates enum "ResourceLayoutScheme" to string and output to ostream.
//
// @param [out] out : Output stream
// @param ResourceLayoutScheme : Resource layout scheme
std::ostream &operator<<(std::ostream &out, ResourceLayoutScheme layout) {
  const char *string = nullptr;
  switch (layout) {
    CASE_CLASSENUM_TO_STRING(ResourceLayoutScheme, Compact)
    CASE_CLASSENUM_TO_STRING(ResourceLayoutScheme, Indirect)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return out << string;
}

// =====================================================================================================================
// Translates enum "ThreadGroupSwizzleMode" to string and output to ostream.
//
// @param [out] out : Output stream
// @param threadGroupSwizzleMode : Provoking vertex mode
std::ostream &operator<<(std::ostream &out, ThreadGroupSwizzleMode threadGroupSwizzleMode) {
  const char *string = nullptr;
  switch (threadGroupSwizzleMode) {
    CASE_CLASSENUM_TO_STRING(ThreadGroupSwizzleMode, Default)
    CASE_CLASSENUM_TO_STRING(ThreadGroupSwizzleMode, _4x4)
    CASE_CLASSENUM_TO_STRING(ThreadGroupSwizzleMode, _8x8)
    CASE_CLASSENUM_TO_STRING(ThreadGroupSwizzleMode, _16x16)
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return out << string;
}

// =====================================================================================================================
// Translates enum "InvariantLoads" to string and output to ostream.
//
// @param [out] out : Output stream
// @param option : Value to convert
std::ostream &operator<<(std::ostream &out, InvariantLoads option) {
  const char *string = nullptr;
  switch (option) {
    CASE_CLASSENUM_TO_STRING(InvariantLoads, Auto)
    CASE_CLASSENUM_TO_STRING(InvariantLoads, EnableOptimization)
    CASE_CLASSENUM_TO_STRING(InvariantLoads, DisableOptimization)
    CASE_CLASSENUM_TO_STRING(InvariantLoads, ClearInvariants)
  default:
    llvm_unreachable("Should never be called!");
  }

  return out << string;
}

} // namespace Vkgc
