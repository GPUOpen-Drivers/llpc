/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief VKGC header file: contains definitions of VKGC pipeline dump utility
 ***********************************************************************************************************************
 */
#pragma once

#include "vkgcDefs.h"
#include "vkgcMetroHash.h"
#include "vkgcRegisterDefs.h"
#include <fstream>

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
  typedef Util::MetroHash64 MetroHash64;

  static void DumpSpirvBinary(const char *dumpDir, const BinaryData *spirvBin, MetroHash::Hash *hash);

  static PipelineDumpFile *BeginPipelineDump(const PipelineDumpOptions *dumpOptions, PipelineBuildInfo pipelineInfo,
                                             const uint64_t hashCode64);

  static void EndPipelineDump(PipelineDumpFile *dumpFile);

  static void DumpPipelineBinary(PipelineDumpFile *binaryFile, GfxIpVersion gfxIp, const BinaryData *pipelineBin);

  static void DumpPipelineExtraInfo(PipelineDumpFile *binaryFile, const std::string *str);

  static MetroHash::Hash generateHashForGraphicsPipeline(const GraphicsPipelineBuildInfo *pipeline, bool isCacheHash,
                                                         bool isRelocatableShader,
                                                         UnlinkedShaderStage unlinkedShaderType = UnlinkedStageCount);

  static MetroHash::Hash generateHashForComputePipeline(const ComputePipelineBuildInfo *pipeline, bool isCacheHash,
                                                        bool isRelocatableShader);
#if VKI_RAY_TRACING
  static MetroHash::Hash generateHashForRayTracingPipeline(const RayTracingPipelineBuildInfo *pipeline,
                                                           bool isCacheHash);
  static void dumpRayTracingRtState(const RtState *rtState, std::ostream &dumpFile);
  static void dumpRayTracingPipelineMetadata(PipelineDumpFile *binaryFile, const BinaryData *pipelineBin);
#endif

  static std::string getPipelineInfoFileName(PipelineBuildInfo pipelineInfo, const uint64_t hashCode64);

  static void updateHashForPipelineShaderInfo(ShaderStage stage, const PipelineShaderInfo *shaderInfo, bool isCacheHash,
                                              MetroHash64 *hasher, bool isRelocatableShader);

  static void updateHashForResourceMappingInfo(const ResourceMappingData *resourceMapping,
                                               const uint64_t pipelineLayoutApiHash, MetroHash64 *hasher,
                                               ShaderStage stage = ShaderStageInvalid);

  static void updateHashForVertexInputState(const VkPipelineVertexInputStateCreateInfo *vertexInput,
                                            bool dynamicVertexStride, MetroHash64 *hasher);

  // Update hash for map object
  template <class MapType> static void updateHashForMap(MapType &m, MetroHash64 *hasher) {
    hasher->Update(m.size());
    for (auto mapIt : m) {
      hasher->Update(mapIt.first);
      hasher->Update(mapIt.second);
    }
  }

  static void updateHashForNonFragmentState(const GraphicsPipelineBuildInfo *pipeline, bool isCacheHash,
                                            MetroHash64 *hasher, bool isRelocatableShader);

  static void updateHashForFragmentState(const GraphicsPipelineBuildInfo *pipeline, MetroHash64 *hasher,
                                         bool isRelocatableShader);

  static void updateHashForPipelineOptions(const PipelineOptions *options, MetroHash64 *hasher, bool isCacheHash,
                                           bool isRelocatableShader, UnlinkedShaderStage stage);

  // Get name of register, or "" if not known
  static const char *getRegisterNameString(unsigned regNumber);

  // Returns the hash for the glue shader that corresponds to the given glue shader string.
  static const MetroHash::Hash generateHashForGlueShader(BinaryData glueShaderString);

private:
  static std::string getSpirvBinaryFileName(const MetroHash::Hash *hash);

  static void dumpComputePipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                      const ComputePipelineBuildInfo *pipelineInfo);
  static void dumpGraphicsPipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                       const GraphicsPipelineBuildInfo *pipelineInfo);
#if VKI_RAY_TRACING
  static void dumpRayTracingPipelineInfo(std::ostream *dumpFile, const char *dumpDir,
                                         const RayTracingPipelineBuildInfo *pipelineInfo);

  static void dumpRayTracingStateInfo(const RayTracingPipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                      std::ostream &dumpFile);
  static void updateHashForRtState(const RtState *rtState, MetroHash64 *hasher);
#endif

  static void dumpVersionInfo(std::ostream &dumpFile);
  static void dumpPipelineShaderInfo(const PipelineShaderInfo *shaderInfo, std::ostream &dumpFile);
  static void dumpResourceMappingInfo(const ResourceMappingData *resourceMapping, std::ostream &dumpFile);
  static void dumpResourceMappingNode(const ResourceMappingNode *userDataNode, const char *prefix,
                                      std::ostream &dumpFile);
  static void dumpComputeStateInfo(const ComputePipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                   std::ostream &dumpFile);
  static void dumpGraphicsStateInfo(const GraphicsPipelineBuildInfo *pipelineInfo, const char *dumpDir,
                                    std::ostream &dumpFile);
  static void dumpPipelineOptions(const PipelineOptions *options, std::ostream &dumpFile);

  static void updateHashForResourceMappingNode(const ResourceMappingNode *userDataNode, bool isRootNode,
                                               MetroHash64 *hasher);
};

} // namespace Vkgc
