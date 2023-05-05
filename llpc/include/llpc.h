/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpc.h
 * @brief LLPC header file: contains LLPC basic definitions (including interfaces and data types).
 ***********************************************************************************************************************
 */
#pragma once

#include "vkgcDefs.h"

namespace Llpc {

using Vkgc::BasicType;
using Vkgc::BinaryData;
using Vkgc::BinaryType;
using Vkgc::ColorTarget;
using Vkgc::ComputePipelineBuildInfo;
using Vkgc::DenormalMode;
using Vkgc::FsOutInfo;
using Vkgc::GfxIpVersion;
using Vkgc::GraphicsPipelineBuildInfo;
using Vkgc::MaxColorTargets;
using Vkgc::NggSubgroupSizingType;
using Vkgc::OutputAllocFunc;
using Vkgc::PipelineOptions;
using Vkgc::PipelineShaderInfo;
using Vkgc::PipelineShaderOptions;
using Vkgc::ResourceMappingData;
using Vkgc::ResourceMappingRootNode;
using Vkgc::StaticDescriptorValue;
#if VKI_RAY_TRACING
using Vkgc::RayTracingPipelineBuildInfo;
#endif
using Vkgc::ResourceMappingNode;
using Vkgc::ResourceMappingNodeType;
using Vkgc::ResourceNodeData;
using Vkgc::Result;
using Vkgc::ShaderHash;
using Vkgc::ShaderModuleData;
using Vkgc::ShaderModuleEntryData;
using Vkgc::ShaderModuleUsage;
using Vkgc::ShaderStage;
using Vkgc::ShaderStageCompute;
using Vkgc::ShaderStageCopyShader;
using Vkgc::ShaderStageCount;
using Vkgc::ShaderStageCountInternal;
using Vkgc::ShaderStageFragment;
using Vkgc::ShaderStageGeometry;
using Vkgc::ShaderStageGfxCount;
using Vkgc::ShaderStageInvalid;
using Vkgc::ShaderStageMesh;
using Vkgc::ShaderStageNativeStageCount;
using Vkgc::ShaderStageTask;
#if VKI_RAY_TRACING
using Vkgc::ShaderStageRayTracingAnyHit;
using Vkgc::ShaderStageRayTracingCallable;
using Vkgc::ShaderStageRayTracingClosestHit;
using Vkgc::ShaderStageRayTracingIntersect;
using Vkgc::ShaderStageRayTracingMiss;
using Vkgc::ShaderStageRayTracingRayGen;
#endif
using Vkgc::ShaderStageBit;
using Vkgc::ShaderStageComputeBit;
using Vkgc::ShaderStageFragmentBit;
using Vkgc::ShaderStageGeometryBit;
using Vkgc::ShaderStageTask;
using Vkgc::ShaderStageTaskBit;
using Vkgc::ShaderStageTessControl;
using Vkgc::ShaderStageTessEval;
using Vkgc::ShaderStageVertex;
#if VKI_RAY_TRACING
using Vkgc::ShaderStageRayTracingAnyHitBit;
using Vkgc::ShaderStageRayTracingCallableBit;
using Vkgc::ShaderStageRayTracingClosestHitBit;
using Vkgc::ShaderStageRayTracingIntersectBit;
using Vkgc::ShaderStageRayTracingMissBit;
using Vkgc::ShaderStageRayTracingRayGenBit;
#endif
using Vkgc::ShaderStageTessControlBit;
using Vkgc::ShaderStageTessEvalBit;
using Vkgc::ShaderStageVertexBit;
using Vkgc::WaveBreakSize;

static const unsigned MaxViewports = 16;
static const char VkIcdName[] = "amdvlk";

/// Represents per shader module options.
struct ShaderModuleOptions {
  PipelineOptions pipelineOptions; ///< Pipeline options related with this shader module
};

/// Represents info to build a shader module.
struct ShaderModuleBuildInfo {
  void *pInstance;                ///< Vulkan instance object
  void *pUserData;                ///< User data
  OutputAllocFunc pfnOutputAlloc; ///< Output buffer allocator
  BinaryData shaderBin;           ///< Shader binary data (SPIR-V binary)
  ShaderModuleOptions options;    ///< Per shader module options
};

/// Represents output of building a shader module.
struct ShaderModuleBuildOut {
  ShaderModuleData *pModuleData; ///< Output shader module data (opaque)
};

enum CacheAccessInfo : uint8_t {
  CacheNotChecked = 0, ///< Cache is not checked.
  CacheMiss,           ///< Cache miss.
  CacheHit,            ///< Cache hit using VkPipelineCache.
  InternalCacheHit,    ///< cache hit using internal cache.
};

/// Represents output of building a graphics pipeline.
struct GraphicsPipelineBuildOut {
  BinaryData pipelineBin;              ///< Output pipeline binary data
  CacheAccessInfo pipelineCacheAccess; ///< Pipeline cache access status i.e., hit, miss, or not checked
  CacheAccessInfo stageCacheAccesses[ShaderStageCount]; ///< Shader cache access status i.e., hit, miss, or not checked
};

/// Represents output of building a compute pipeline.
struct ComputePipelineBuildOut {
  BinaryData pipelineBin;              ///< Output pipeline binary data
  CacheAccessInfo pipelineCacheAccess; ///< Pipeline cache access status i.e., hit, miss, or not checked
  CacheAccessInfo stageCacheAccess;    ///< Shader cache access status i.e., hit, miss, or not checked
};

#if VKI_RAY_TRACING

/// Represents output of building a ray tracing pipeline.
struct RayTracingPipelineBuildOut {
  unsigned pipelineBinCount;                           ///< Output pipeline binary data count
  BinaryData *pipelineBins;                            ///< Output pipeline binary datas
  Vkgc::RayTracingShaderGroupHandle shaderGroupHandle; ///< Output data for shader group handle
  Vkgc::RayTracingShaderPropertySet shaderPropSet;     ///< Output property of a set of shader
  bool hasTraceRay;                                    ///< Output whether have traceray module
};
#endif

/// Defines callback function used to lookup shader cache info in an external cache
typedef Result (*ShaderCacheGetValue)(const void *pClientData, uint64_t hash, void *pValue, size_t *pValueLen);

/// Defines callback function used to store shader cache info in an external cache
typedef Result (*ShaderCacheStoreValue)(const void *pClientData, uint64_t hash, const void *pValue, size_t valueLen);

/// Specifies all information necessary to create a shader cache object.
struct ShaderCacheCreateInfo {
  const void *pInitialData; ///< Pointer to a data buffer whose contents should be used to seed the shader
                            ///  cache. This may be null if no initial data is present.
  size_t initialDataSize;   ///< Size of the initial data buffer, in bytes.

  // NOTE: The following parameters are all optional, and are only used when the IShaderCache will be used in
  // tandem with an external cache which serves as a backing store for the cached shader data.

  // [optional] Private client-opaque data which will be passed to the pClientData parameters of the Get and
  // Store callback functions.
  const void *pClientData;
  ShaderCacheGetValue pfnGetValueFunc;     ///< [Optional] Function to lookup shader cache data in an external cache
  ShaderCacheStoreValue pfnStoreValueFunc; ///< [Optional] Function to store shader cache data in an external cache
};

// =====================================================================================================================
/// Represents the interface of a cache for compiled shaders. The shader cache is designed to be optionally passed in at
/// pipeline create time. The compiled binary for the shaders is stored in the cache object to avoid compiling the same
/// shader multiple times. The shader cache also provides a method to serialize its data to be stored to disk.
class IShaderCache {
public:
  /// Serializes the shader cache data or queries the size required for serialization.
  ///
  /// @param [in]      pBlob  System memory pointer where the serialized data should be placed. This parameter can
  ///                         be null when querying the size of the serialized data. When non-null (and the size is
  ///                         correct/sufficient) then the contents of the shader cache will be placed in this
  ///                         location. The data is an opaque blob which is not intended to be parsed by clients.
  /// @param [in,out]  pSize  Size of the memory pointed to by pBlob. If the value stored in pSize is zero then no
  ///                         data will be copied and instead the size required for serialization will be returned
  ///                         in pSize.
  ///
  /// @returns : Success if data was serialized successfully, Unknown if fail to do serialize.
  virtual Result Serialize(void *pBlob, size_t *pSize) = 0;

  /// Merges the provided source shader caches' content into this shader cache.
  ///
  /// @param [in]  srcCacheCount  Count of source shader caches to be merged.
  /// @param [in]  ppSrcCaches    Pointer to an array of pointers to shader cache objects.
  ///
  /// @returns : Success if data of source shader caches was merged successfully, OutOfMemory if the internal allocator
  ///          memory cannot be allocated.
  virtual Result Merge(unsigned srcCacheCount, const IShaderCache **ppSrcCaches) = 0;

  /// Frees all resources associated with this object.
  virtual void Destroy() = 0;

protected:
  /// @internal Constructor. Prevent use of new operator on this interface.
  IShaderCache() {}

  /// @internal Destructor. Prevent use of delete operator on this interface.
  virtual ~IShaderCache() {}
};

#if VKI_RAY_TRACING
// Users of LLPC may implement this interface to allow the compiler to request additional threads.
//
// Lifetime of this object:
//  - User of LLPC prepares an object with this interface and passes it to an ICompiler method
//  - LLPC calls SetTasks on the main thread
//  - User calls the thread function on any number of helper threads (may be 0 threads).
//    User should check that there are remaining tasks *before* calling the thread function, but there
//    is no guarantee that GetNextTask will succeed since races with other helper threads are possible.
//  - LLPC calls GetNextTask and TaskCompleted from main and helper threads.
//  - LLPC calls WaitForTasks on the main thread.
class IHelperThreadProvider {
public:
  using ThreadFunction = void(IHelperThreadProvider *, void *);

  // Set the number of tasks and thread function. The given payload is opaque data which is
  // provided to the thread function.
  virtual void SetTasks(ThreadFunction *pFunction, uint32_t numTasks, void *payload) = 0;

  // Obtain the next task index. Returns true on success, or false if all tasks have completed.
  // Called from main and helper threads.
  virtual bool GetNextTask(uint32_t *pTaskIndex) = 0;

  // Notify that work has completed on one task.
  // Called from main and helper threads, exactly once per successful GetNextTask
  // (even if an error occurred during the processing of the task).
  virtual void TaskCompleted() = 0;

  // Wait for all tasks to complete. Called from main thread.
  virtual void WaitForTasks() = 0;
};
#endif

// =====================================================================================================================
/// Represents the interfaces of a pipeline compiler.
class ICompiler {
public:
  /// Creates pipeline compiler from the specified info.
  ///
  /// @param [in]  optionCount    Count of compilation-option strings
  /// @param [in]  options        An array of compilation-option strings
  /// @param [out] ppCompiler : Pointer to the created pipeline compiler object
  /// @param [in]  pCache         Pointer to the ICache object
  ///
  /// @returns : Result::Success if successful. Other return codes indicate failure.
  static Result VKAPI_CALL Create(GfxIpVersion gfxIp, unsigned optionCount, const char *const *options,
                                  ICompiler **ppCompiler, Vkgc::ICache *pCache = nullptr);

  /// Checks whether a vertex attribute format is supported by fetch shader.
  ///
  /// @parame [in] format  Vertex attribute format
  ///
  /// @returns : True if the specified format is supported by fetch shader. Otherwise, FALSE is returned.
  static bool VKAPI_CALL IsVertexFormatSupported(VkFormat format);

  /// Destroys the pipeline compiler.
  virtual void VKAPI_CALL Destroy() = 0;

  /// Convert ColorBufferFormat to fragment shader export format
  ///
  /// param [in] pTarget                  Color target including color buffer format
  /// param [in] enableAlphaToCoverage    Whether enable AlphaToCoverage
  ///
  /// @returns : Unsigned type casted from fragment shader export format.
  virtual unsigned ConvertColorBufferFormatToExportFormat(const ColorTarget *pTarget,
                                                          const bool enableAlphaToCoverage) const = 0;

  /// Build shader module from the specified info.
  ///
  /// @param [in]  pShaderInfo    Info to build this shader module
  /// @param [out] pShaderOut : Output of building this shader module
  ///
  /// @returns : Result::Success if successful. Other return codes indicate failure.
  virtual Result BuildShaderModule(const ShaderModuleBuildInfo *pShaderInfo, ShaderModuleBuildOut *pShaderOut) = 0;

  /// Build unlinked shader to ElfPackage with part pipeline info.
  ///
  /// @param [in]  pipelineInfo     : Info to build this shader module
  /// @param [out] pipelineOut      : Output of building this shader module
  /// @param [in]  stage            : Shader stage of needing to compile
  /// @param [out] pipelineDumpFile : Handle of pipeline dump file
  ///
  /// @returns : Result::Success if successful. Other return codes indicate failure.
  virtual Result buildGraphicsShaderStage(const GraphicsPipelineBuildInfo *pipelineInfo,
                                          GraphicsPipelineBuildOut *pipelineOut, Vkgc::UnlinkedShaderStage stage,
                                          void *pipelineDumpFile = nullptr) = 0;

  /// Build the whole graphics pipeline. If missing elfPackage of a certain stage, we will build it first, and
  /// link them to the full pipeline last.
  ///
  /// @param [in]  pipelineInfo : Info to build this shader module
  /// @param [out] pipelineOut  : Output of building this shader module
  /// @param [in]  elfPackage   : Early compiled elfPackage; it is an array which size is UnlinkedStageCount.
  ///
  /// @returns : Result::Success if successful. Other return codes indicate failure.
  virtual Result buildGraphicsPipelineWithElf(const GraphicsPipelineBuildInfo *pipelineInfo,
                                              GraphicsPipelineBuildOut *pipelineOut, const BinaryData *elfPackage) = 0;

  /// Build graphics pipeline from the specified info.
  ///
  /// @param [in]  pPipelineInfo  Info to build this graphics pipeline
  /// @param [out] pPipelineOut : Output of building this graphics pipeline
  ///
  /// @returns : Result::Success if successful. Other return codes indicate failure.
  virtual Result BuildGraphicsPipeline(const GraphicsPipelineBuildInfo *pPipelineInfo,
                                       GraphicsPipelineBuildOut *pPipelineOut, void *pPipelineDumpFile = nullptr) = 0;

  /// Build compute pipeline from the specified info.
  ///
  /// @param [in]  pPipelineInfo  Info to build this compute pipeline
  /// @param [out] pPipelineOut : Output of building this compute pipeline
  ///
  /// @returns : Result::Success if successful. Other return codes indicate failure.
  virtual Result BuildComputePipeline(const ComputePipelineBuildInfo *pPipelineInfo,
                                      ComputePipelineBuildOut *pPipelineOut, void *pPipelineDumpFile = nullptr) = 0;
#if VKI_RAY_TRACING
  /// Build ray tracing pipeline from the specified info.
  ///
  /// @param [in]  pPipelineInfo  Info to build this ray tracing pipeline
  /// @param [out] pPipelineOut   Output of building this ray tracing pipeline
  ///
  /// @returns Result::Success if successful. Other return codes indicate failure.
  virtual Result BuildRayTracingPipeline(const RayTracingPipelineBuildInfo *pPipelineInfo,
                                         RayTracingPipelineBuildOut *pPipelineOut, void *pPipelineDumpFile = nullptr,
                                         IHelperThreadProvider *pHelperThreadProvider = nullptr) = 0;
#endif

#if LLPC_ENABLE_SHADER_CACHE
  /// Creates a shader cache object with the requested properties.
  ///
  /// @param [in]  pCreateInfo    Create info of the shader cache.
  /// @param [out] ppShaderCache : Constructed shader cache object.
  ///
  /// @returns : Success if the shader cache was successfully created. Otherwise, ErrorOutOfMemory is returned.
  virtual Result CreateShaderCache(const ShaderCacheCreateInfo *pCreateInfo, IShaderCache **ppShaderCache) = 0;
#endif

protected:
  ICompiler() {}
  /// Destructor
  virtual ~ICompiler() {}
};

} // namespace Llpc
