/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcDebug.h"
#include "llpcElfReader.h"
#include "llpcInternal.h"
#include "llpcMetroHash.h"
#include "llpcShaderCacheManager.h"

namespace Llpc
{

// Forward declaration
class Builder;
class ComputeContext;
class Context;
class GraphicsContext;
class PassManager;

// Enumerates types of shader binary.
enum class BinaryType : uint32_t
{
    Unknown = 0,  // Invalid type
    Spirv,        // SPIR-V binary
    LlvmBc,       // LLVM bitcode
    MultiLlvmBc,  // Multiple LLVM bitcode
    Elf,          // ELF
};

// Represents the information of one shader entry in ShaderModuleData
struct ShaderModuleEntry
{
    ShaderStage stage;              // Shader stage
    uint32_t    entryNameHash[4];   // Hash code of entry name
    uint32_t    entryOffset;        // Byte offset of the entry data in the binCode of ShaderModuleData
    uint32_t    entrySize;          // Byte size of the entry data
    uint32_t    resUsageSize;       // Byte size of the resource usage
                                    // NOTE: It should be removed after we move all necessary resUsage info to
                                    // LLVM module metadata
    uint32_t    passIndex;          // Indices of passes, It is only for internal debug.
};

// Represents the name map <stage, name> of shader entry-point
struct ShaderEntryName
{
    ShaderStage stage;             // Shader stage
    const char* pName;             // Entry name
};

// Represents the information of a shader module
struct ShaderModuleInfo
{
    uint32_t              cacheHash[4];            // hash code for calculate pipeline cache key
    uint32_t              debugInfoSize;           // Byte size of debug instructions
    bool                  enableVarPtrStorageBuf;  // Whether to enable "VariablePointerStorageBuffer" capability
    bool                  enableVarPtr;            // Whether to enable "VariablePointer" capability
    bool                  useSubgroupSize;         // Whether gl_SubgroupSize is used
    bool                  useHelpInvocation;       // Whether fragment shader has helper-invocation for subgroup
    bool                  useSpecConstant;         // Whether specializaton constant is used
    bool                  keepUnusedFunctions;     // Whether to keep unused function
    uint32_t              entryCount;              // Entry count in the module
    ShaderModuleEntry     entries[1];              // Array of all entries
};

// Represents output data of building a shader module.
struct ShaderModuleData : public ShaderModuleDataHeader
{
    BinaryType       binType;                 // Shader binary type
    BinaryData       binCode;                 // Shader binary data
    ShaderModuleInfo moduleInfo;              // Shader module info
};

// Represents the properties of GPU device.
struct GpuProperty
{
    uint32_t numShaderEngines;                  // Number of shader engines present
    uint32_t waveSize;                          // Wavefront size
    uint32_t ldsSizePerCu;                      // LDS size per compute unit
    uint32_t ldsSizePerThreadGroup;             // LDS size per thread group
    uint32_t gsOnChipDefaultPrimsPerSubgroup;   // Default target number of primitives per subgroup for GS on-chip mode.
    uint32_t gsOnChipDefaultLdsSizePerSubgroup; // Default value for the maximum LDS size per subgroup for
    uint32_t gsOnChipMaxLdsSize;                // Max LDS size used by GS on-chip mode (in DWORDs)
    uint32_t ldsSizeDwordGranularityShift;      // Amount of bits used to shift the LDS_SIZE register field

    //TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
    uint32_t gsPrimBufferDepth;     // Comes from the hardware GPU__GC__GSPRIM_BUFF_DEPTH configuration option

    uint32_t maxUserDataCount;                  // Max allowed count of user data SGPRs
    uint32_t tessOffChipLdsBufferSize;          // Off-chip Tess Buffer Size
    uint32_t maxSgprsAvailable;                 // Number of max available SGPRs
    uint32_t maxVgprsAvailable;                 // Number of max available VGPRs
    uint32_t tessFactorBufferSizePerSe;         // Size of the tessellation-factor buffer per SE, in DWORDs.
#if LLPC_BUILD_GFX10
    bool     supportShaderPowerProfiling;       // Hardware supports Shader Profiling for Power
    bool     supportSpiPrefPriority;            // Hardware supports SPI shader preference priority
#endif
};

// Contains flags for all of the hardware workarounds which affect pipeline compilation.
struct WorkaroundFlags
{
    union
    {
        struct
        {
            uint32_t  cbNoLt16BitIntClamp               :  1;
            uint32_t  miscLoadBalancePerWatt            :  1;
            uint32_t  miscSpiSgprsNum                   :  1;
            uint32_t  shader8b16bLocalWriteCorruption   :  1;
            uint32_t  shaderCoalesceStore               :  1;
            uint32_t  shaderEstimateRegisterUsage       :  1;
            uint32_t  shaderReadlaneSmrd                :  1;
            uint32_t  shaderSmemBufferAddrClamp         :  1;
            uint32_t  shaderSpiBarrierMgmt              :  1;
            //
            //
            //
            //
            //

            uint32_t  shaderSpiCsRegAllocFragmentation  :  1;
            uint32_t  shaderVcczScalarReadBranchFailure :  1;
            uint32_t  shaderZExport                     :  1;
            // Pre-GFX9 hardware doesn't support min/max denorm flush, we insert extra fmul with 1.0 to flush the denorm value
            uint32_t  shaderMinMaxFlushDenorm           :  1;
            uint32_t  reserved                          : 19;
        };
        uint32_t  u32All;
    } gfx6;

    union
    {
        struct
        {
            uint32_t  fixCacheLineStraddling       :  1;
            uint32_t  fixLsVgprInput               :  1;
            uint32_t  shaderImageGatherInstFix     :  1;
            uint32_t  treat1dImagesAs2d            :  1;
            uint32_t  reserved                     : 28;
        };
        uint32_t  u32All;
    } gfx9;

#if LLPC_BUILD_GFX10
    union
    {
        struct
        {
            uint32_t  disableI32ModToI16Mod        :  1;

            uint32_t  waTessFactorBufferSizeLimitGeUtcl1Underflow : 1;
            uint32_t  waTessIncorrectRelativeIndex : 1;
            uint32_t  waShaderInstPrefetch123 : 1;
            uint32_t  waShaderInstPrefetch0 : 1;
            uint32_t  nggTessDegeneratePrims : 1;
            uint32_t  waDidtThrottleVmem : 1;
            uint32_t  waLdsVmemNotWaitingVmVsrc : 1;
            uint32_t  waNsaCannotBeLastInClause : 1;
            uint32_t  waNsaAndClauseCanHang : 1;
            uint32_t  waNsaCannotFollowWritelane : 1;
            uint32_t  waThrottleInMultiDwordNsa : 1;
            uint32_t  waSmemFollowedByVopc : 1;
            uint32_t  waNggCullingNoEmptySubgroups : 1;
            uint32_t  waShaderInstPrefetchFwd64 : 1;
            uint32_t  waWarFpAtomicDenormHazard : 1;
            uint32_t  waNggDisabled : 1;
            uint32_t  reserved : 15;
        };
        uint32_t u32All;
    } gfx10;
#endif
};

// Represents statistics info for pipeline module
struct PipelineStatistics
{
    uint32_t    numUsedVgprs;       // Number of used VGPRs
    uint32_t    numAvailVgprs;      // Number of available VGPRs
    bool        sgprSpill;          // Has SGPR spill
    bool        useScratchBuffer;   // Whether scratch buffer is used
};

// =====================================================================================================================
// Represents LLPC pipeline compiler.
class Compiler: public ICompiler
{
public:
    Compiler(GfxIpVersion gfxIp, uint32_t optionCount, const char*const* pOptions, MetroHash::Hash optionHash);
    ~Compiler();

    virtual void VKAPI_CALL Destroy();

    virtual Result BuildShaderModule(const ShaderModuleBuildInfo* pShaderInfo,
                                     ShaderModuleBuildOut*        pShaderOut) const;

    virtual Result BuildGraphicsPipeline(const GraphicsPipelineBuildInfo* pPipelineInfo,
                                         GraphicsPipelineBuildOut*        pPipelineOut,
                                         void*                            pPipelineDumpFile = nullptr);

    virtual Result BuildComputePipeline(const ComputePipelineBuildInfo* pPipelineInfo,
                                        ComputePipelineBuildOut*        pPipelineOut,
                                        void*                           pPipelineDumpFile = nullptr);
    Result BuildGraphicsPipelineInternal(GraphicsContext*                           pGraphicsContext,
                                         llvm::ArrayRef<const PipelineShaderInfo*>  shaderInfo,
                                         uint32_t                                   forceLoopUnrollCount,
                                         ElfPackage*                                pPipelineElf);

    Result BuildComputePipelineInternal(ComputeContext*                 pComputeContext,
                                        const ComputePipelineBuildInfo* pPipelineInfo,
                                        uint32_t                        forceLoopUnrollCount,
                                        ElfPackage*                     pPipelineElf);

    Result BuildPipelineInternal(Context*                                   pContext,
                                 llvm::ArrayRef<const PipelineShaderInfo*>  shaderInfo,
                                 uint32_t                                   forceLoopUnrollCount,
                                 ElfPackage*                                pPipelineElf);

    // Gets the count of compiler instance.
    static uint32_t GetInstanceCount() { return m_instanceCount; }

    // Gets the count of redirect output
    static uint32_t GetOutRedirectCount() { return m_outRedirectCount; }

    static MetroHash::Hash GenerateHashForCompileOptions(uint32_t          optionCount,
                                                         const char*const* pOptions);

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    virtual Result CreateShaderCache(const ShaderCacheCreateInfo* pCreateInfo, IShaderCache** ppShaderCache);
#endif

    static void TranslateSpirvToLlvm(const PipelineShaderInfo*    pShaderInfo,
                                     llvm::Module*                pModule);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(Compiler);
    LLPC_DISALLOW_COPY_AND_ASSIGN(Compiler);

    Result ValidatePipelineShaderInfo(ShaderStage shaderStage, const PipelineShaderInfo* pShaderInfo) const;

    void InitGpuProperty();
    void InitGpuWorkaround();

    Context* AcquireContext() const;
    void ReleaseContext(Context* pContext) const;

    static Result OptimizeSpirv(const BinaryData* pSpirvBinIn, BinaryData* pSpirvBinOut);

    static void CleanOptimizedSpirv(BinaryData* pSpirvBin);

    static void TrimSpirvDebugInfo(const BinaryData* pSpvBinCode, uint32_t bufferSize, void* pTrimCode);

    static Result CollectInfoFromSpirvBinary(const BinaryData*                      pSpvBinCode,
                                             ShaderModuleInfo*                      pShaderModuleInfo,
                                             llvm::SmallVector<ShaderEntryName, 4>& shaderEntryNames);

    void GetPipelineStatistics(const void*             pCode,
                               size_t                  codeSize,
                               GfxIpVersion            gfxIp,
                               PipelineStatistics*     pPipelineStats) const;

    bool RunPasses(PassManager* pPassMgr, llvm::Module* pModule) const;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    ShaderEntryState LookUpShaderCaches(IShaderCache*       pAppPipelineCache,
                                        MetroHash::Hash*    pCacheHash,
                                        BinaryData*         pElfBin,
                                        ShaderCache**       ppShaderCache,
                                        CacheEntryHandle*   phEntry);

    void UpdateShaderCaches(bool                insert,
                            const BinaryData*   pElfBin,
                            ShaderCache**       ppShaderCache,
                            CacheEntryHandle*   phEntry,
                            uint32_t            shaderCacheCount);
#else
    ShaderEntryState LookUpShaderCache(MetroHash::Hash*    pCacheHash,
                                       BinaryData*         pElfBin,
                                       CacheEntryHandle*   phEntry);

    void UpdateShaderCache(bool                insert,
                           const BinaryData*   pElfBin,
                           CacheEntryHandle   phEntry);
#endif
    void BuildShaderCacheHash(Context* pContext, MetroHash::Hash* pFragmentHash, MetroHash::Hash* pNonFragmentHash);

    void MergeElfBinary(Context*          pContext,
                        const BinaryData* pFragmentElf,
                        const BinaryData* pNonFragmentElf,
                        ElfPackage*       pPipelineElf);
    // -----------------------------------------------------------------------------------------------------------------

    std::vector<std::string>      m_options;          // Compilation options
    MetroHash::Hash               m_optionHash;       // Hash code of compilation options
    GfxIpVersion                  m_gfxIp;            // Graphics IP version info
    static uint32_t               m_instanceCount;    // The count of compiler instance
    static uint32_t               m_outRedirectCount; // The count of output redirect
    ShaderCachePtr                m_shaderCache;      // Shader cache
    GpuProperty                   m_gpuProperty;      // GPU property
    WorkaroundFlags               m_gpuWorkarounds;   // GPU workarounds;
    static llvm::sys::Mutex       m_contextPoolMutex; // Mutex for context pool access
    static std::vector<Context*>* m_pContextPool;      // Context pool
};

} // Llpc
