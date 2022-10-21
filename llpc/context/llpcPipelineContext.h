/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPipelineContext.h
 * @brief LLPC header file: contains declaration of class Llpc::PipelineContext.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcCompiler.h"
#include "spirvExt.h"
#include "vkgcMetroHash.h"
#include "lgc/Pipeline.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include <unordered_map>
#include <unordered_set>

namespace lgc {

class Pipeline;

} // namespace lgc

namespace Util {

class MetroHash64;

} // namespace Util

namespace Llpc {

// Enumerates types of descriptor.
enum class DescriptorType : unsigned {
  UniformBlock = 0,   // Uniform block
  ShaderStorageBlock, // Shader storage block
  Texture,            // Combined texture
  TextureResource,    // Separated texture resource
  TextureSampler,     // Separated texture sampler
  TexelBuffer,        // Texture buffer and image buffer
  Image,              // Image
  SubpassInput,       // Subpass input
};

// Represents floating-point control setting.
struct FloatControl {
  bool denormPreserve;           // Preserve denormals
  bool denormFlushToZero;        // Flush denormals to zero
  bool signedZeroInfNanPreserve; // Preserve signed zero/INF/NaN
  bool roundingModeRTE;          // Rounding mode: to nearest even
  bool roundingModeRTZ;          // Rounding mode: to zero
};

// Represents the info of a descriptor binding
struct DescriptorBinding {
  DescriptorType descType; // Type of the descriptor
  unsigned arraySize;      // Element count of arrayed binding (flattened)
  bool isMultisampled;     // Whether multisampled texture is used
};

typedef std::vector<DescriptorBinding> DescriptorSet;

// Shader FP mode for use by front-end
struct ShaderFpMode {
  unsigned denormPreserve : 4;           // Bitmask of denormPreserve flags
  unsigned denormFlushToZero : 4;        // Bitmask of denormFlushToZero flags
  unsigned signedZeroInfNanPreserve : 4; // Bitmask of signedZeroInfNanPreserve flags
  unsigned roundingModeRTE : 4;          // Bitmask of roundingModeRTE flags
  unsigned roundingModeRTZ : 4;          // Bitmask of roundingModeRTZ flags
};

// =====================================================================================================================
// Represents pipeline-specific context for pipeline compilation, it is a part of LLPC context
class PipelineContext {
public:
  PipelineContext(GfxIpVersion gfxIp, MetroHash::Hash *pipelineHash, MetroHash::Hash *cacheHash
#if VKI_RAY_TRACING
                  ,
                  const Vkgc::RtState *rtState

#endif
  );
  virtual ~PipelineContext();

  // Checks whether the pipeline is graphics or compute
  virtual bool isGraphics() const { return false; }

  // Gets pipeline shader info of the specified shader stage
  virtual const PipelineShaderInfo *getPipelineShaderInfo(ShaderStage shaderStage) const = 0;

  // Gets pipeline build info
  virtual const void *getPipelineBuildInfo() const = 0;

  // Gets the mask of active shader stages bound to this pipeline
  virtual unsigned getShaderStageMask() const = 0;

  // Sets the mask of active shader stages bound to this pipeline
  virtual void setShaderStageMask(unsigned mask) = 0;

  // Sets whether pre-rasterization part has a geometry shader.
  // NOTE: Only applicable in the part pipeline compilation mode.
  virtual void setPreRasterHasGs(bool preRasterHasGs) { llvm_unreachable("Should never be called!"); }

  // Gets whether pre-rasterization part has a geometry shader.
  // NOTE: Only applicable in the part pipeline compilation mode.
  virtual bool getPreRasterHasGs() const { return false; }

  // Gets the count of active shader stages
  virtual unsigned getActiveShaderStageCount() const = 0;

  // Gets per pipeline options
  virtual const PipelineOptions *getPipelineOptions() const = 0;

  // Gets subgroup size usage denoting which stage uses features relevant to subgroup size.
  // @returns : Bitmask per stage, in the same order as defined in `Vkgc::ShaderStage`.
#if VKI_RAY_TRACING
  // NOTE: For raytracing, returns (-1) if the pipeline uses features relevant to subgroup size.
#endif
  virtual unsigned getSubgroupSizeUsage() const = 0;

#if VKI_RAY_TRACING
  // Checks whether the pipeline is ray tracing
  virtual bool isRayTracing() const { return false; }

  virtual bool hasRayQuery() const { return false; }

  virtual void setIndirectStage(ShaderStage stage) {}

  virtual void collectPayloadSize(llvm::Type *type, const llvm::DataLayout &dataLayout) {}
  virtual void collectCallableDataSize(llvm::Type *type, const llvm::DataLayout &dataLayout) {}
  virtual void collectAttributeDataSize(llvm::Type *type, const llvm::DataLayout &dataLayout) {}
  virtual void collectBuiltIn(unsigned builtIn) {}
#endif

  static const char *getGpuNameAbbreviation(GfxIpVersion gfxIp);

  // Gets graphics IP version info
  GfxIpVersion getGfxIpVersion() const { return m_gfxIp; }

  // Gets pipeline hash code compacted to 64-bits.
  uint64_t getPipelineHashCode() const { return MetroHash::compact64(&m_pipelineHash); }

  // Gets cache hash code compacted to 64-bits.
  uint64_t get64BitCacheHashCode() const { return MetroHash::compact64(&m_cacheHash); }

#if VKI_RAY_TRACING
  unsigned getRayTracingWaveSize() const;

  const char *getRayTracingFunctionName(unsigned funcType);

  // Gets ray tracing state info
  const Vkgc::RtState *getRayTracingState() { return m_rtState; }
#endif

  // Gets the finalized 128-bit cache hash code.
  lgc::Hash128 get128BitCacheHashCode() const {
    lgc::Hash128 finalizedCacheData = {};
    Util::MetroHash128 hash128 = {};
    hash128.Update(m_cacheHash);
    hash128.Finalize(reinterpret_cast<uint8_t *>(finalizedCacheData.data()));
    return finalizedCacheData;
  }

  // Get the current cache hash code without compacting it.
  MetroHash::Hash getCacheHashCodeWithoutCompact() const { return m_cacheHash; }

  // Sets pipeline hash code
  void setPipelineHashCode(const MetroHash::Hash &hash) { m_pipelineHash = hash; }

  // Sets the cache hash for the pipeline.  This is the hash that is used to do cache lookups.
  void setHashForCacheLookUp(MetroHash::Hash hash) { m_cacheHash = hash; }

  ShaderHash getShaderHashCode(ShaderStage stage) const;

  // Set pipeline state in lgc::Pipeline object for middle-end, and (optionally) hash the state.
  void setPipelineState(lgc::Pipeline *pipeline, Util::MetroHash64 *hasher, bool unlinked) const;

  // Get ShaderFpMode struct for the given shader stage
  ShaderFpMode &getShaderFpMode(ShaderStage stage) { return m_shaderFpModes[stage]; }

  // Map a VkFormat to a {BufDataFormat, BufNumFormat}. Returns BufDataFormatInvalid if the
  // VkFormat is not supported.
  static std::pair<lgc::BufDataFormat, lgc::BufNumFormat> mapVkFormat(VkFormat format, bool isColorExport);

  // Set whether we are building a relocatable (unlinked) ElF
  void setUnlinked(bool unlinked) { m_unlinked = unlinked; }

  // Get whether we are building a relocatable (unlinked) ElF
  bool isUnlinked() const { return m_unlinked; }

  // Gets pipeline resource mapping data
  const ResourceMappingData *getResourceMapping() const { return &m_resourceMapping; }

  // Gets pipeline layout api hash
  const uint64_t getPipelineLayoutApiHash() const { return m_pipelineLayoutApiHash; }

protected:
  // Gets dummy vertex input create info
  virtual VkPipelineVertexInputStateCreateInfo *getDummyVertexInputInfo() { return nullptr; }

  // Gets dummy vertex binding info
  virtual std::vector<VkVertexInputBindingDescription> *getDummyVertexBindings() { return nullptr; }

  // Gets dummy vertex attribute info
  virtual std::vector<VkVertexInputAttributeDescription> *getDummyVertexAttributes() { return nullptr; }

  GfxIpVersion m_gfxIp;                  // Graphics IP version info
  MetroHash::Hash m_pipelineHash;        // Pipeline hash code
  MetroHash::Hash m_cacheHash;           // Cache hash code
  ResourceMappingData m_resourceMapping; // Contains resource mapping nodes and static descriptor values
  uint64_t m_pipelineLayoutApiHash;      // Pipeline Layout Api Hash
#if VKI_RAY_TRACING
  const Vkgc::RtState *m_rtState; // Ray tracing state
#endif

private:
  PipelineContext() = delete;
  PipelineContext(const PipelineContext &) = delete;
  PipelineContext &operator=(const PipelineContext &) = delete;

  // Type of immutable nodes map used in SetUserDataNodesTable
  typedef std::map<std::pair<unsigned, unsigned>, const StaticDescriptorValue *> ImmutableNodesMap;

  // Give the pipeline options to the middle-end, and/or hash them.
  void setOptionsInPipeline(lgc::Pipeline *pipeline, Util::MetroHash64 *hasher) const;

  // Give the user data nodes and descriptor range values to the middle-end, and/or hash them.
  void setUserDataInPipeline(lgc::Pipeline *pipeline, Util::MetroHash64 *hasher, unsigned stageMask) const;
  void setUserDataNodesTable(lgc::Pipeline *pipeline, llvm::ArrayRef<ResourceMappingNode> nodes,
                             const ImmutableNodesMap &immutableNodesMap, bool isRoot, lgc::ResourceNode *destTable,
                             lgc::ResourceNode *&destInnerTable) const;

  // Give the graphics pipeline state to the middle-end, and/or hash it.
  void setGraphicsStateInPipeline(lgc::Pipeline *pipeline, Util::MetroHash64 *hasher, unsigned stageMask) const;

  // Set vertex input descriptions in middle-end Pipeline, and/or hash them.
  void setVertexInputDescriptions(lgc::Pipeline *pipeline, Util::MetroHash64 *hasher) const;

  // Give the color export state to the middle-end, and/or hash it.
  void setColorExportState(lgc::Pipeline *pipeline, Util::MetroHash64 *hasher) const;

  ShaderFpMode m_shaderFpModes[ShaderStageCountInternal] = {};
  bool m_unlinked = false; // Whether we are building an "unlinked" shader ELF
};

} // namespace Llpc
