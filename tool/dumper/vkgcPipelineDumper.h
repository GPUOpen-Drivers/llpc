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
 * @file  vkgcPipelineDumper.h
 * @brief VKGC header file: contains definitions of VKGC pipline dump utility
 ***********************************************************************************************************************
 */
#pragma once

#include "vkgcDefs.h"
#include "vkgcMetroHash.h"
#include <fstream>
#if !defined(SINGLE_EXTERNAL_METROHASH)
namespace MetroHash {
class MetroHash64;
struct Hash;
}; // namespace MetroHash
#endif

namespace Vkgc {

struct ComputePipelineBuildInfo;
struct GraphicsPipelineBuildInfo;
struct BinaryData;
struct PipelineDumpFile;

// Enumerates which types of pipeline dump are disable
enum PipelineDumpFilters : unsigned {
  PipelineDumpFilterNone = 0x00, // Do not disable any pipeline type
  PipelineDumpFilterCs = 0x01,   // Disable pipeline dump for Cs
  PipelineDumpFilterNgg = 0x02,  // Disable pipeline dump for NGG
  PipelineDumpFilterGs = 0x04,   // Disable pipeline dump for Gs
  PipelineDumpFilterTess = 0x08, // Disable pipeline dump for Tess
  PipelineDumpFilterVsPs = 0x10, // Disable pipeline dump for VsPs
};

class PipelineDumper {
public:
#if defined(SINGLE_EXTERNAL_METROHASH)
  typedef Util::MetroHash64 MetroHash64;
#else
  typedef MetroHash::MetroHash64 MetroHash64;
#endif

  static void DumpSpirvBinary(const char *dumpDir, const BinaryData *spirvBin, MetroHash::Hash *hash);

  static PipelineDumpFile *BeginPipelineDump(const PipelineDumpOptions *dumpOptions, PipelineBuildInfo pipelineInfo,
                                             const MetroHash::Hash *hash);

  static void EndPipelineDump(PipelineDumpFile *dumpFile);

  static void DumpPipelineBinary(PipelineDumpFile *binaryFile, GfxIpVersion gfxIp, const BinaryData *pipelineBin);

  static void DumpPipelineExtraInfo(PipelineDumpFile *binaryFile, const std::string *str);

  static MetroHash::Hash generateHashForGraphicsPipeline(const GraphicsPipelineBuildInfo *pipeline, bool isCacheHash,
                                                        bool isRelocatableShader,
                                                        unsigned stage = ShaderStageInvalid);

  static MetroHash::Hash generateHashForComputePipeline(const ComputePipelineBuildInfo *pipeline,
                                                        bool isRelocatableShader, bool isCacheHash);

  static std::string getPipelineInfoFileName(PipelineBuildInfo pipelineInfo, const MetroHash::Hash *hash);

  static void updateHashForPipelineShaderInfo(ShaderStage stage, const PipelineShaderInfo *shaderInfo, bool isCacheHash,
                                              MetroHash64 *hasher, bool isRelocatableShader);

  static void updateHashForVertexInputState(const VkPipelineVertexInputStateCreateInfo *vertexInput,
                                            MetroHash64 *hasher);

  // Update hash for map object
  template <class MapType> static void updateHashForMap(MapType &m, MetroHash64 *hasher) {
    hasher->Update(m.size());
    for (auto mapIt : m) {
      hasher->Update(mapIt.first);
      hasher->Update(mapIt.second);
    }
  }

  static void updateHashForNonFragmentState(const GraphicsPipelineBuildInfo *pipeline, bool isCacheHash,
                                            MetroHash64 *hasher);

  static void updateHashForFragmentState(const GraphicsPipelineBuildInfo *pipeline, MetroHash64 *hasher);

  // Get name of register, or "" if not known
  static const char *getRegisterNameString(unsigned regNumber);

private:
  static std::string getSpirvBinaryFileName(const MetroHash::Hash *hash);

  static void dumpComputePipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                      const ComputePipelineBuildInfo *pipelineInfo);
  static void dumpGraphicsPipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                       const GraphicsPipelineBuildInfo *pipelineInfo);

  static void dumpVersionInfo(std::ostream &dumpFile);
  static void dumpPipelineShaderInfo(const PipelineShaderInfo *shaderInfo, std::ostream &dumpFile);
  static void dumpResourceMappingNode(const ResourceMappingNode *userDataNode, const char *prefix,
                                      std::ostream &dumpFile);
  static void dumpComputeStateInfo(const ComputePipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                   std::ostream &dumpFile);
  static void dumpGraphicsStateInfo(const GraphicsPipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                    std::ostream &dumpFile);
  static void dumpPipelineOptions(const PipelineOptions *options, std::ostream &dumpFile);

  static void updateHashForResourceMappingNode(const ResourceMappingNode *userDataNode, bool isRootNode,
                                               MetroHash64 *hasher, bool isRelocatableShader);
};

} // namespace Vkgc
