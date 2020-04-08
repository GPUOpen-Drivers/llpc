/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcShaderCacheManager.h"
#include "llpcShaderModuleHelper.h"
#include "vkgcElfReader.h"
#include "vkgcMetroHash.h"
#include "lgc/CommonDefs.h"

namespace llvm {

class Module;

} // namespace llvm

namespace lgc {

class PassManager;

} // namespace lgc

namespace Llpc {

using Vkgc::ElfPackage;
using Vkgc::findVkStructInChain;

// Forward declaration
class Compiler;
class ComputeContext;
class Context;
class GraphicsContext;

// =====================================================================================================================
// Object to manage checking and updating shader cache for graphics pipeline.
class GraphicsShaderCacheChecker {
public:
  GraphicsShaderCacheChecker(Compiler *compiler, Context *context) : m_compiler(compiler), m_context(context) {}

  // Check shader caches, returning mask of which shader stages we want to keep in this compile.
  unsigned check(const llvm::Module *module, unsigned stageMask, llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes);

  // Get cache results.
  ShaderEntryState getNonFragmentCacheEntryState() { return m_nonFragmentCacheEntryState; }
  ShaderEntryState getFragmentCacheEntryState() { return m_fragmentCacheEntryState; }

  // Update shader caches with results of compile, and merge ELF outputs if necessary.
  void updateAndMerge(Result result, ElfPackage *pipelineElf);
  void updateRootUserDateOffset(ElfPackage *pipelineElf);

private:
  Compiler *m_compiler;
  Context *m_context;

  ShaderEntryState m_nonFragmentCacheEntryState = ShaderEntryState::New;
  ShaderCache *m_nonFragmentShaderCache = nullptr;
  CacheEntryHandle m_hNonFragmentEntry = {};
  BinaryData m_nonFragmentElf = {};

  ShaderEntryState m_fragmentCacheEntryState = ShaderEntryState::New;
  ShaderCache *m_fragmentShaderCache = nullptr;
  CacheEntryHandle m_hFragmentEntry = {};
  BinaryData m_fragmentElf = {};
};

// =====================================================================================================================
// Represents LLPC pipeline compiler.
class Compiler : public ICompiler {
public:
  Compiler(GfxIpVersion gfxIp, unsigned optionCount, const char *const *options, MetroHash::Hash optionHash);
  ~Compiler();

  virtual void VKAPI_CALL Destroy();

  virtual Result BuildShaderModule(const ShaderModuleBuildInfo *shaderInfo, ShaderModuleBuildOut *shaderOut) const;

  virtual unsigned ConvertColorBufferFormatToExportFormat(const ColorTarget *target,
                                                          const bool enableAlphaToCoverage) const;

  virtual Result BuildGraphicsPipeline(const GraphicsPipelineBuildInfo *pipelineInfo,
                                       GraphicsPipelineBuildOut *pipelineOut, void *pipelineDumpFile = nullptr);

  virtual Result BuildComputePipeline(const ComputePipelineBuildInfo *pipelineInfo,
                                      ComputePipelineBuildOut *pipelineOut, void *pipelineDumpFile = nullptr);
  Result buildGraphicsPipelineInternal(GraphicsContext *graphicsContext,
                                       llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                       unsigned forceLoopUnrollCount, bool buildingRelocatableElf,
                                       ElfPackage *pipelineElf);

  Result buildComputePipelineInternal(ComputeContext *computeContext, const ComputePipelineBuildInfo *pipelineInfo,
                                      unsigned forceLoopUnrollCount, bool buildingRelocatableElf,
                                      ElfPackage *pipelineElf);

  Result buildPipelineWithRelocatableElf(Context *context, llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                         unsigned forceLoopUnrollCount, ElfPackage *pipelineElf);

  Result buildPipelineInternal(Context *context, llvm::ArrayRef<const PipelineShaderInfo *> shaderInfo,
                               unsigned forceLoopUnrollCount, ElfPackage *pipelineElf);

  // Gets the count of compiler instance.
  static unsigned getInstanceCount() { return m_instanceCount; }

  // Gets the count of redirect output
  static unsigned getOutRedirectCount() { return m_outRedirectCount; }

  static MetroHash::Hash generateHashForCompileOptions(unsigned optionCount, const char *const *options);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38 || LLPC_USE_EXPERIMENTAL_SHADER_CACHE_PIPELINES
  virtual Result CreateShaderCache(const ShaderCacheCreateInfo *pCreateInfo, IShaderCache **ppShaderCache);
#endif

  ShaderEntryState lookUpShaderCaches(IShaderCache *appPipelineCache, MetroHash::Hash *cacheHash, BinaryData *elfBin,
                                      ShaderCache **ppShaderCache, CacheEntryHandle *phEntry);

  void updateShaderCache(bool insert, const BinaryData *elfBin, ShaderCache *shaderCache, CacheEntryHandle phEntry);

  static void buildShaderCacheHash(Context *context, unsigned stageMask,
                                   llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes, MetroHash::Hash *fragmentHash,
                                   MetroHash::Hash *nonFragmentHash);

private:
  Compiler() = delete;
  Compiler(const Compiler &) = delete;
  Compiler &operator=(const Compiler &) = delete;

  Result validatePipelineShaderInfo(const PipelineShaderInfo *shaderInfo) const;

  Context *acquireContext() const;
  void releaseContext(Context *context) const;

  bool runPasses(lgc::PassManager *passMgr, llvm::Module *module) const;
  void linkRelocatableShaderElf(ElfPackage *shaderElfs, ElfPackage *pipelineElf, Context *context);
  bool canUseRelocatableGraphicsShaderElf(const llvm::ArrayRef<const PipelineShaderInfo *> &shaderInfo) const;
  bool canUseRelocatableComputeShaderElf(const PipelineShaderInfo *shaderInfo) const;

  // -----------------------------------------------------------------------------------------------------------------

  std::vector<std::string> m_options;           // Compilation options
  MetroHash::Hash m_optionHash;                 // Hash code of compilation options
  GfxIpVersion m_gfxIp;                         // Graphics IP version info
  static unsigned m_instanceCount;              // The count of compiler instance
  static unsigned m_outRedirectCount;           // The count of output redirect
  ShaderCachePtr m_shaderCache;                 // Shader cache
  static llvm::sys::Mutex m_contextPoolMutex;   // Mutex for context pool access
  static std::vector<Context *> *m_contextPool; // Context pool
};

// Convert front-end LLPC shader stage to middle-end LGC shader stage
lgc::ShaderStage getLgcShaderStage(ShaderStage stage);

} // namespace Llpc
