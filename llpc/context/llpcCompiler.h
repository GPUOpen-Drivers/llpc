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
#include "vkgcElfReader.h"
#include "vkgcMetroHash.h"
#include "llpcShaderCacheManager.h"
#include "llpcShaderModuleHelper.h"
#include "lgc/llpcBuilderCommon.h"

namespace llvm
{

class Module;

} // llvm

namespace lgc
{

class PassManager;

} // lgc

namespace Llpc
{

using Vkgc::ElfPackage;
using Vkgc::FindVkStructInChain;

// Forward declaration
class Compiler;
class ComputeContext;
class Context;
class GraphicsContext;

// =====================================================================================================================
// Object to manage checking and updating shader cache for graphics pipeline.
class GraphicsShaderCacheChecker
{
public:
    GraphicsShaderCacheChecker(Compiler* pCompiler, Context* pContext) :
        m_pCompiler(pCompiler), m_pContext(pContext)
    {}

    // Check shader caches, returning mask of which shader stages we want to keep in this compile.
    unsigned Check(const llvm::Module*                     pModule,
                   unsigned                                stageMask,
                   llvm::ArrayRef<llvm::ArrayRef<uint8_t>> stageHashes);

    // Get cache results.
    ShaderEntryState GetNonFragmentCacheEntryState() { return m_nonFragmentCacheEntryState; }
    ShaderEntryState GetFragmentCacheEntryState() { return m_fragmentCacheEntryState; }

    // Update shader caches with results of compile, and merge ELF outputs if necessary.
    void UpdateAndMerge(Result result, ElfPackage* pPipelineElf);
    void UpdateRootUserDateOffset(ElfPackage* pPipelineElf);

private:
    Compiler* m_pCompiler;
    Context*  m_pContext;

    ShaderEntryState m_nonFragmentCacheEntryState = ShaderEntryState::New;
    ShaderCache* m_pNonFragmentShaderCache = nullptr;
    CacheEntryHandle m_hNonFragmentEntry = {};
    BinaryData m_nonFragmentElf = {};

    ShaderEntryState m_fragmentCacheEntryState = ShaderEntryState::New;
    ShaderCache* m_pFragmentShaderCache = nullptr;
    CacheEntryHandle m_hFragmentEntry = {};
    BinaryData m_fragmentElf = {};
};

// =====================================================================================================================
// Represents LLPC pipeline compiler.
class Compiler: public ICompiler
{
public:
    Compiler(GfxIpVersion gfxIp, unsigned optionCount, const char*const* pOptions, MetroHash::Hash optionHash);
    ~Compiler();

    virtual void VKAPI_CALL Destroy();

    virtual Result BuildShaderModule(const ShaderModuleBuildInfo* pShaderInfo,
                                     ShaderModuleBuildOut*        pShaderOut) const;

    virtual unsigned ConvertColorBufferFormatToExportFormat(const ColorTarget*  pTarget,
                                                            const bool          enableAlphaToCoverage) const;

    virtual Result BuildGraphicsPipeline(const GraphicsPipelineBuildInfo* pPipelineInfo,
                                         GraphicsPipelineBuildOut*        pPipelineOut,
                                         void*                            pPipelineDumpFile = nullptr);

    virtual Result BuildComputePipeline(const ComputePipelineBuildInfo* pPipelineInfo,
                                        ComputePipelineBuildOut*        pPipelineOut,
                                        void*                           pPipelineDumpFile = nullptr);
    Result BuildGraphicsPipelineInternal(GraphicsContext*                          pGraphicsContext,
                                         llvm::ArrayRef<const PipelineShaderInfo*> shaderInfo,
                                         unsigned                                  forceLoopUnrollCount,
                                         bool                                      buildingRelocatableElf,
                                         ElfPackage*                               pPipelineElf);

    Result BuildComputePipelineInternal(ComputeContext*                 pComputeContext,
                                        const ComputePipelineBuildInfo* pPipelineInfo,
                                        unsigned                        forceLoopUnrollCount,
                                        bool                            buildingRelocatableElf,
                                        ElfPackage*                     pPipelineElf);

    Result BuildPipelineWithRelocatableElf(Context*                                   pContext,
                                           llvm::ArrayRef<const PipelineShaderInfo*>  shaderInfo,
                                           unsigned                                   forceLoopUnrollCount,
                                           ElfPackage*                                pPipelineElf);

    Result BuildPipelineInternal(Context*                                   pContext,
                                 llvm::ArrayRef<const PipelineShaderInfo*>  shaderInfo,
                                 unsigned                                   forceLoopUnrollCount,
                                 ElfPackage*                                pPipelineElf);

    // Gets the count of compiler instance.
    static unsigned GetInstanceCount() { return m_instanceCount; }

    // Gets the count of redirect output
    static unsigned GetOutRedirectCount() { return m_outRedirectCount; }

    static MetroHash::Hash GenerateHashForCompileOptions(unsigned          optionCount,
                                                         const char*const* pOptions);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    virtual Result CreateShaderCache(const ShaderCacheCreateInfo* pCreateInfo, IShaderCache** ppShaderCache);
#endif

    ShaderEntryState LookUpShaderCaches(IShaderCache*       pAppPipelineCache,
                                        MetroHash::Hash*    pCacheHash,
                                        BinaryData*         pElfBin,
                                        ShaderCache**       ppShaderCache,
                                        CacheEntryHandle*   phEntry);

    void UpdateShaderCache(bool                insert,
                           const BinaryData*   pElfBin,
                           ShaderCache*        pShaderCache,
                           CacheEntryHandle    phEntry);

    static void BuildShaderCacheHash(Context*                                 pContext,
                                     unsigned                                 stageMask,
                                     llvm::ArrayRef<llvm::ArrayRef<uint8_t>>  stageHashes,
                                     MetroHash::Hash*                         pFragmentHash,
                                     MetroHash::Hash*                         pNonFragmentHash);

private:
    Compiler() = delete;
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    Result ValidatePipelineShaderInfo(const PipelineShaderInfo* pShaderInfo) const;

    Context* AcquireContext() const;
    void ReleaseContext(Context* pContext) const;

    bool RunPasses(lgc::PassManager* pPassMgr, llvm::Module* pModule) const;
    void LinkRelocatableShaderElf(ElfPackage *pShaderElfs, ElfPackage* pPipelineElf, Context* pContext);
    bool CanUseRelocatableGraphicsShaderElf(const llvm::ArrayRef<const PipelineShaderInfo*>& shaderInfo) const;
    bool CanUseRelocatableComputeShaderElf(const PipelineShaderInfo* pShaderInfo) const;

    // -----------------------------------------------------------------------------------------------------------------

    std::vector<std::string>      m_options;          // Compilation options
    MetroHash::Hash               m_optionHash;       // Hash code of compilation options
    GfxIpVersion                  m_gfxIp;            // Graphics IP version info
    static unsigned               m_instanceCount;    // The count of compiler instance
    static unsigned               m_outRedirectCount; // The count of output redirect
    ShaderCachePtr                m_shaderCache;      // Shader cache
    static llvm::sys::Mutex       m_contextPoolMutex; // Mutex for context pool access
    static std::vector<Context*>* m_pContextPool;      // Context pool
};

// Convert front-end LLPC shader stage to middle-end LGC shader stage
lgc::ShaderStage GetLgcShaderStage(ShaderStage stage);

} // Llpc
