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
 * @file  llpcCompiler.h
 * @brief LLPC header file: contains declaration of class Llpc::Compiler.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcCacheAccessor.h"
#include "llpcShaderModuleHelper.h"
#include "llpcUtil.h"
#include "vkgcElfReader.h"
#include "vkgcMetroHash.h"
#include "lgc/CommonDefs.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/Support/Mutex.h"
#include <condition_variable>
#include <optional>

namespace llvm {

class Module;

} // namespace llvm

namespace lgc {

class PassManager;
class Pipeline;
enum class PipelineLink : unsigned;

} // namespace lgc

namespace Llpc {

using Vkgc::ElfPackage;
using Vkgc::findVkStructInChain;

// Forward declaration
class Compiler;
class ComputeContext;
class Context;
class GraphicsContext;
class RayTracingContext;
class TimerProfiler;

// =====================================================================================================================
// Object to manage checking and updating shader cache for graphics pipeline.
class GraphicsShaderCacheChecker {
public:
  GraphicsShaderCacheChecker(Compiler *compiler, Context *context) : m_compiler(compiler), m_context(context) {}

  // Check shader caches, returning mask of which shader stages we want to keep in this compile.
  unsigned check(const llvm::Module *module, unsigned stageMask, llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes,
                 llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses);

  // Update shader caches with results of compile, and merge ELF outputs if necessary.
  void updateAndMerge(Result result, ElfPackage *pipelineElf);

private:
  Compiler *m_compiler;
  Context *m_context;
  std::optional<CacheAccessor> m_nonFragmentCacheAccessor;
  std::optional<CacheAccessor> m_fragmentCacheAccessor;

  // New ICache
  Vkgc::EntryHandle m_nonFragmentEntry;

  Vkgc::EntryHandle m_fragmentEntry;
};

// =====================================================================================================================
// Represents LLPC pipeline compiler.
class Compiler : public ICompiler {
public:
  Compiler(GfxIpVersion gfxIp, unsigned optionCount, const char *const *options, MetroHash::Hash optionHash,
           Vkgc::ICache *cache);
  ~Compiler();

  virtual void VKAPI_CALL Destroy();

  virtual Result BuildShaderModule(const ShaderModuleBuildInfo *shaderInfo, ShaderModuleBuildOut *shaderOut);

  virtual Result buildGraphicsShaderStage(const GraphicsPipelineBuildInfo *pipelineInfo,
                                          GraphicsPipelineBuildOut *pipelineOut, Vkgc::UnlinkedShaderStage stage,
                                          void *pipelineDumpFile = nullptr);

  virtual Result buildGraphicsPipelineWithElf(const GraphicsPipelineBuildInfo *pipelineInfo,
                                              GraphicsPipelineBuildOut *pipelineOut, const BinaryData *elfPackage);

  virtual Result BuildColorExportShader(const GraphicsPipelineBuildInfo *pipelineInfo, const void *fsOutputMetaData,
                                        GraphicsPipelineBuildOut *pipelineOut, void *pipelineDumpFile = nullptr);

  virtual unsigned ConvertColorBufferFormatToExportFormat(const ColorTarget *target,
                                                          const bool enableAlphaToCoverage) const;

  virtual Result BuildGraphicsPipeline(const GraphicsPipelineBuildInfo *pipelineInfo,
                                       GraphicsPipelineBuildOut *pipelineOut, void *pipelineDumpFile = nullptr);

  virtual Result BuildComputePipeline(const ComputePipelineBuildInfo *pipelineInfo,
                                      ComputePipelineBuildOut *pipelineOut, void *pipelineDumpFile = nullptr);
  virtual Result BuildRayTracingPipeline(const RayTracingPipelineBuildInfo *pipelineInfo,
                                         RayTracingPipelineBuildOut *pipelineOut, void *pipelineDumpFile = nullptr,
                                         IHelperThreadProvider *pHelperThreadProvider = nullptr);

  Result buildGraphicsPipelineInternal(GraphicsContext *graphicsContext,
                                       llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                       bool buildingRelocatableElf, ElfPackage *pipelineElf,
                                       llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses);

  Result buildGraphicsPipelineWithPartPipelines(Context *context, llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                                ElfPackage *pipelineElf,
                                                llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses);

  Result buildComputePipelineInternal(ComputeContext *computeContext, const ComputePipelineBuildInfo *pipelineInfo,
                                      bool buildingRelocatableElf, ElfPackage *pipelineElf,
                                      CacheAccessInfo *stageCacheAccess);

  Result buildPipelineWithRelocatableElf(Context *context, llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                         ElfPackage *pipelineElf,
                                         llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses);

  Result buildPipelineInternal(Context *context, llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                               lgc::PipelineLink pipelineLink, lgc::Pipeline *otherPartPipeline,
                               ElfPackage *pipelineElf, llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses);

  // Gets the count of compiler instance.
  static unsigned getInstanceCount() { return m_instanceCount; }

  // Gets the count of redirect output
  static unsigned getOutRedirectCount() { return m_outRedirectCount; }

  static MetroHash::Hash generateHashForCompileOptions(unsigned optionCount, const char *const *options);

  static void buildShaderCacheHash(Context *context, unsigned stageMask,
                                   llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes, MetroHash::Hash *fragmentHash,
                                   MetroHash::Hash *nonFragmentHash);

  Vkgc::ICache *getInternalCaches() { return m_cache; }

  Context *acquireContext() const;
  void releaseContext(Context *context) const;

  Result buildRayTracingPipelineElf(Context *context, std::unique_ptr<llvm::Module> module, ElfPackage &pipelineElf,
                                    std::vector<Vkgc::RayTracingShaderProperty> &shaderProps,
                                    std::vector<bool> &moduleCallsTraceRay, unsigned moduleIndex,
                                    std::unique_ptr<lgc::Pipeline> &pipeline, TimerProfiler &timerProfiler);
  llvm::sys::Mutex &getHelperThreadMutex() { return m_helperThreadMutex; }
  std::condition_variable_any &getHelperThreadConditionVariable() { return m_helperThreadConditionVariable; }

  void setUseGpurt(lgc::Pipeline *pipeline);

private:
  Compiler() = delete;
  Compiler(const Compiler &) = delete;
  Compiler &operator=(const Compiler &) = delete;

  Result validatePipelineShaderInfo(const PipelineShaderInfo *shaderInfo) const;

  bool runPasses(lgc::PassManager *passMgr, llvm::Module *module) const;
  bool linkRelocatableShaderElf(ElfPackage *shaderElfs, ElfPackage *pipelineElf, Context *context);
  bool canUseRelocatableGraphicsShaderElf(const llvm::ArrayRef<const PipelineShaderInfo *> &shaderInfo,
                                          const GraphicsPipelineBuildInfo *pipelineInfo);
  bool canUseRelocatableComputeShaderElf(const ComputePipelineBuildInfo *pipelineInfo);
  std::unique_ptr<llvm::Module> createGpurtShaderLibrary(Context *context);
  Result buildRayTracingPipelineInternal(RayTracingContext &rtContext,
                                         llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo, bool unlinked,
                                         std::vector<ElfPackage> &pipelineElfs,
                                         std::vector<Vkgc::RayTracingShaderProperty> &shaderProps,
                                         IHelperThreadProvider *helperThreadProvider);
  void addRayTracingIndirectPipelineMetadata(ElfPackage *pipelineElf);
  Result buildUnlinkedShaderInternal(Context *context, llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                     Vkgc::UnlinkedShaderStage stage, ElfPackage &elfPackage,
                                     llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses);
  void dumpCompilerOptions(void *pipelineDumpFile);
  Result generatePipeline(Context *context, unsigned moduleIndex, std::unique_ptr<llvm::Module> module,
                          ElfPackage &pipelineElf, lgc::Pipeline *pipeline, TimerProfiler &timerProfiler);

  std::vector<std::string> m_options;           // Compilation options
  MetroHash::Hash m_optionHash;                 // Hash code of compilation options
  GfxIpVersion m_gfxIp;                         // Graphics IP version info
  Vkgc::ICache *m_cache;                        // Point to ICache implemented in client
  static unsigned m_instanceCount;              // The count of compiler instance
  static unsigned m_outRedirectCount;           // The count of output redirect
  static llvm::sys::Mutex m_contextPoolMutex;   // Mutex for context pool access
  static std::vector<Context *> *m_contextPool; // Context pool
  unsigned m_relocatablePipelineCompilations;   // The number of pipelines compiled using relocatable shader elf
  static llvm::sys::Mutex m_helperThreadMutex;  // Mutex for helper thread
  static std::condition_variable_any m_helperThreadConditionVariable; // Condition variable used by helper thread to
                                                                      // wait for main thread switching context

  void buildShaderModuleResourceUsage(
      const ShaderModuleBuildInfo *shaderInfo, Vkgc::ResourcesNodes &resourcesNodes,
      std::vector<ResourceNodeData> &inputSymbolInfo, std::vector<ResourceNodeData> &outputSymbolInfo,
      std::vector<ResourceNodeData> &uniformBufferInfo, std::vector<ResourceNodeData> &storageBufferInfo,
      std::vector<ResourceNodeData> &textureSymbolInfo, std::vector<ResourceNodeData> &imageSymbolInfo,
      std::vector<ResourceNodeData> &atomicCounterSymbolInfo, std::vector<ResourceNodeData> &defaultUniformSymbolInfo,
      ShaderModuleUsage &shaderModuleUsage);
};

// Convert front-end LLPC shader stage to middle-end LGC shader stage
lgc::ShaderStage getLgcShaderStage(ShaderStage stage);

// Convert front-end LLPC shader stage to middle-end LGC rt shader stage.
// Returns std::nullopt if not a raytracing stage.
std::optional<lgc::rt::RayTracingShaderStage> getLgcRtShaderStage(ShaderStage stage);

// Convert front-end LLPC shader stage to middle-end LGC shader stage mask
unsigned getLgcShaderStageMask(ShaderStage stage);

} // namespace Llpc
