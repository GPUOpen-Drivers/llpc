/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcCompiler.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Compiler.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-compiler"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

#include <sstream>
#include "LLVMSPIRVLib.h"
#include "spirvExt.h"
#include "SPIRVInternal.h"
#include "llpcCodeGenManager.h"
#include "llpcCompiler.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "llpcCopyShader.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"
#include "llpcGraphicsContext.h"
#include "llpcElf.h"
#include "llpcFile.h"
#include "llpcPassLoopInfoCollect.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "llpcShaderMerger.h"
#include "llpcPipelineDumper.h"
#include "llpcSpirvLower.h"
#include "llpcVertexFetch.h"
#include <unordered_set>

#ifdef LLPC_ENABLE_SPIRV_OPT
    #define SPVGEN_STATIC_LIB 1
    #include "spvgen.h"
#endif

using namespace llvm;
using namespace SPIRV;

namespace llvm
{

namespace cl
{

// -pipeline-dump-dir: directory where pipeline info are dumped
opt<std::string> PipelineDumpDir("pipeline-dump-dir",
                                 desc("Directory where pipeline shader info are dumped"),
                                 value_desc("dir"),
                                 init("."));

static opt<uint32_t> FilterPipelineDumpByType("filter-pipeline-dump-by-type",
                                              desc("Filter which types of pipeline dump are disabled\n"
                                                   "0x00 - Do not disable logging based on pipeline type\n"
                                                   "0x01 - Disable logging for CS pipelines\n"
                                                   "0x02 - Disable logging for NGG pipelines\n"
                                                   "0x04 - Disable logging for GS pipelines\n"
                                                   "0x08 - Disable logging for TS pipelines\n"
                                                   "0x10 - Disable logging for VS-PS pipelines"),
                                              init(0));

static opt<uint64_t> FilterPipelineDumpByHash("filter-pipeline-dump-by-hash",
                                              desc("Only dump the pipeline with this compiler hash if non-zero"),
                                              init(0));

static opt<bool> DumpDuplicatePipelines("dump-duplicate-pipelines",
    desc("If TRUE, duplicate pipelines will be dumped to a file with a numeric suffix attached"),
    init(false));

// -enable-pipeline-dump: enable pipeline info dump
opt<bool> EnablePipelineDump("enable-pipeline-dump", desc("Enable pipeline info dump"), init(false));

// -enable-time-profiler: enable time profiler for various compilation phases
static opt<bool> EnableTimeProfiler("enable-time-profiler",
                                    desc("Enable time profiler for various compilation phases"),
                                    init(false));

// -shader-cache-file-dir: root directory to store shader cache
opt<std::string> ShaderCacheFileDir("shader-cache-file-dir",
                                    desc("Root directory to store shader cache"),
                                    value_desc("dir"),
                                    init("."));

// -shader-cache-mode: shader cache mode:
// 0 - Disable
// 1 - Runtime cache
// 2 - Cache to disk
static opt<uint32_t> ShaderCacheMode("shader-cache-mode",
                                     desc("Shader cache mode, 0 - disable, 1 - runtime cache, 2 - cache to disk "),
                                     init(0));

// -executable-name: executable file name
static opt<std::string> ExecutableName("executable-name",
                                       desc("Executable file name"),
                                       value_desc("filename"),
                                       init("amdllpc"));

// -shader-replace-mode: shader replacement mode
// 0 - Disable
// 1 - Replacement based on shader hash
// 2 - Replacement based on both shader hash and pipeline hash
static opt<uint32_t> ShaderReplaceMode("shader-replace-mode",
                                       desc("Shader replacement mode, "
                                            "0 - disable, "
                                            "1 - replacement based on shader hash, "
                                            "2 - replacement based on both shader hash and pipeline hash"),
                                       init(0));

// -shader-replace-dir: directory to store the files used in shader replacement
static opt<std::string> ShaderReplaceDir("shader-replace-dir",
                                         desc("Directory to store the files used in shader replacement"),
                                         value_desc("dir"),
                                         init("."));

// -shader-replace-pipeline-hashes: a collection of pipeline hashes, specifying shader replacement is operated on which pipelines
static opt<std::string> ShaderReplacePipelineHashes("shader-replace-pipeline-hashes",
                                                    desc("A collection of pipeline hashes, specifying shader "
                                                         "replacement is operated on which pipelines"),
                                                    value_desc("hashes with comma as separator"),
                                                    init(""));

// -enable-spirv-opt: enable optimization for SPIR-V binary
opt<bool> EnableSpirvOpt("enable-spirv-opt", desc("Enable optimization for SPIR-V binary"), init(false));

// -enable-shadow-desc: enable shadow desriptor table
opt<bool> EnableShadowDescriptorTable("enable-shadow-desc", desc("Enable shadow descriptor table"), init(false));

// -shadow-desc-table-ptr-high: high part of VA for shadow descriptor table pointer
opt<uint32_t> ShadowDescTablePtrHigh("shadow-desc-table-ptr-high",
                                     desc("High part of VA for shadow descriptor table pointer"),
                                     init(2));

// -enable-dynamic-loop-unroll: Enable dynamic loop unroll.
opt<bool> EnableDynamicLoopUnroll("enable-dynamic-loop-unroll", desc("Enable dynamic loop unroll"), init(false));

// -force-loop-unroll-count: Force to set the loop unroll count; this option will ignore dynamic loop unroll.
opt<int> ForceLoopUnrollCount("force-loop-unroll-count", cl::desc("Force loop unroll count"), init(0));

extern opt<bool> EnableOuts;

extern opt<bool> EnableErrs;

extern opt<std::string> LogFileDbgs;

extern opt<std::string> LogFileOuts;

} // cl

} // llvm

namespace Llpc
{

llvm::sys::Mutex      Compiler::m_contextPoolMutex;
std::vector<Context*> Compiler::m_contextPool;

// Time profiling result
TimeProfileResult g_timeProfileResult = {};

// Enumerates modes used in shader replacement
enum ShaderReplaceMode
{
    ShaderReplaceDisable            = 0, // Disabled
    ShaderReplaceShaderHash         = 1, // Replacement based on shader hash
    ShaderReplaceShaderPipelineHash = 2, // Replacement based on both shader and pipeline hash
};

static const uint8_t GlslNullFsEmuLib[]=
{
    #include "generate/g_llpcGlslNullFsEmuLib.h"
};

static ManagedStatic<sys::Mutex> s_compilerMutex;
static MetroHash::Hash s_optionHash = {};

uint32_t Compiler::m_instanceCount = 0;
uint32_t Compiler::m_outRedirectCount = 0;

// =====================================================================================================================
// Handler for LLVM fatal error.
static void FatalErrorHandler(
    void*               userData,       // [in] An argument which will be passed to the installed error handler
    const std::string&  reason,         // Error reason
    bool                gen_crash_diag) // Whether diagnostic should be generated
{
    LLPC_ERRS("LLVM FATAL ERROR:" << reason << "\n");
#if LLPC_ENABLE_EXCEPTION
    throw("LLVM fatal error");
#endif
}

// =====================================================================================================================
// Creates LLPC compiler from the specified info.
Result VKAPI_CALL ICompiler::Create(
    GfxIpVersion      gfxIp,        // Graphics IP version
    uint32_t          optionCount,  // Count of compilation-option strings
    const char*const* options,      // [in] An array of compilation-option strings
    ICompiler**       ppCompiler)   // [out] Pointer to the created LLPC compiler object
{
    Result result = Result::Success;

    const char* pClient = options[0];
    bool ignoreErrors = (strcmp(pClient, VkIcdName) == 0);

    raw_null_ostream nullStream;

    MutexGuard lock(*s_compilerMutex);
    MetroHash::Hash optionHash = Compiler::GenerateHashForCompileOptions(optionCount, options);

    bool parseCmdOption = true;
    if (Compiler::GetInstanceCount() > 0)
    {
        bool isSameOption = memcmp(&optionHash, &s_optionHash, sizeof(optionHash)) == 0;

        parseCmdOption = false;
        if (isSameOption == false)
        {
            if (Compiler::GetOutRedirectCount() == 0)
            {
                // All compiler instances are destroyed, we can reset LLVM options in safe
                auto& options = cl::getRegisteredOptions();
                for (auto it = options.begin(); it != options.end(); ++it)
                {
                    it->second->reset();
                }
                parseCmdOption = true;
            }
            else
            {
                LLPC_ERRS("Incompatible compiler options cross compiler instances!");
                result = Result::ErrorInvalidValue;
                LLPC_NEVER_CALLED();
            }
        }
    }

    if (parseCmdOption)
    {
        // LLVM command options can't be parsed multiple times
        if (cl::ParseCommandLineOptions(optionCount,
                                        options,
                                        "AMD LLPC compiler",
                                        ignoreErrors ? &nullStream : nullptr) == false)
        {
            result = Result::ErrorInvalidValue;
        }
    }

    if (result == Result::Success)
    {
        s_optionHash = optionHash;
        *ppCompiler = new Compiler(gfxIp, optionCount, options, s_optionHash);
        LLPC_ASSERT(*ppCompiler != nullptr);
    }
    else
    {
       *ppCompiler = nullptr;
       result = Result::ErrorInvalidValue;
    }
    return result;
}

// =====================================================================================================================
// Checks whether a vertex attribute format is supported by fetch shader.
bool VKAPI_CALL ICompiler::IsVertexFormatSupported(
    VkFormat format)   // Vertex attribute format
{
    auto pInfo = VertexFetch::GetVertexFormatInfo(format);
    return ((pInfo->dfmt == BUF_DATA_FORMAT_INVALID) && (pInfo->numChannels == 0)) ? false : true;
}

// =====================================================================================================================
Compiler::Compiler(
    GfxIpVersion      gfxIp,        // Graphics IP version info
    uint32_t          optionCount,  // Count of compilation-option strings
    const char*const* pOptions,     // [in] An array of compilation-option strings
    MetroHash::Hash   optionHash)   // Hash code of compilation options
    :
    m_optionHash(optionHash),
    m_gfxIp(gfxIp)
{
    for (uint32_t i = 0; i < optionCount; ++i)
    {
        m_options.push_back(pOptions[i]);
    }

    if (m_outRedirectCount == 0)
    {
        RedirectLogOutput(false, optionCount, pOptions);
    }

    if (m_instanceCount == 0)
    {
        // Initialize LLVM target: AMDGPU
        LLVMInitializeAMDGPUTargetInfo();
        LLVMInitializeAMDGPUTarget();
        LLVMInitializeAMDGPUTargetMC();
        LLVMInitializeAMDGPUAsmPrinter();
        LLVMInitializeAMDGPUAsmParser();
        LLVMInitializeAMDGPUDisassembler();

        // LLVM fatal error handler only can be installed once.
        install_fatal_error_handler(FatalErrorHandler);

#ifdef LLPC_ENABLE_SPIRV_OPT
        InitSpvGen();
#endif
    }

    // Initialize shader cache
    ShaderCacheCreateInfo    createInfo = {};
    ShaderCacheAuxCreateInfo auxCreateInfo = {};
    uint32_t shaderCacheMode = cl::ShaderCacheMode;
    auxCreateInfo.shaderCacheMode = static_cast<ShaderCacheMode>(shaderCacheMode);
    auxCreateInfo.gfxIp           = m_gfxIp;
    auxCreateInfo.hash            = m_optionHash;
    auxCreateInfo.pExecutableName = cl::ExecutableName.c_str();
    auxCreateInfo.pCacheFilePath  = cl::ShaderCacheFileDir.c_str();
    if (cl::ShaderCacheFileDir.empty())
    {
#ifdef WIN_OS
        auxCreateInfo.pCacheFilePath  = getenv("LOCALAPPDATA");
#else
        LLPC_NEVER_CALLED();
#endif
    }

    m_shaderCache = ShaderCacheManager::GetShaderCacheManager()->GetShaderCacheObject(&createInfo, &auxCreateInfo);

    InitGpuProperty();
    InitGpuWorkaround();

    ++m_instanceCount;
    ++m_outRedirectCount;
}

// =====================================================================================================================
Compiler::~Compiler()
{
    bool shutdown = false;
    {
        // Free context pool
        MutexGuard lock(m_contextPoolMutex);

        // Keep the max allowed count of contexts that reside in the pool so that we can speed up the creatoin of
        // compiler next time.
        for (auto it = m_contextPool.begin(); it != m_contextPool.end();)
        {
            auto   pContext             = *it;
            size_t maxResidentContexts  = 0;

            // This is just a W/A for Teamcity. Setting AMD_RESIDENT_CONTEXTS could reduce more than 40 minutes of
            // CTS running time.
            char*  pMaxResidentContexts = getenv("AMD_RESIDENT_CONTEXTS");

            if (pMaxResidentContexts != nullptr)
            {
                maxResidentContexts = strtoul(pMaxResidentContexts, nullptr, 0);
            }

            if ((pContext->IsInUse() == false) && (m_contextPool.size() > maxResidentContexts))
            {
                it = m_contextPool.erase(it);
                delete pContext;
            }
            else
            {
                ++it;
            }
        }
    }

    // Restore default output
    {
        MutexGuard lock(*s_compilerMutex);
        -- m_outRedirectCount;
        if (m_outRedirectCount == 0)
        {
            RedirectLogOutput(true, 0, nullptr);
        }

        ShaderCacheManager::GetShaderCacheManager()->ReleaseShaderCacheObject(m_shaderCache);
    }

    if (m_options[0] == VkIcdName)
    {
        // NOTE: Skip subsequent cleanup work for Vulkan ICD. The work will be done by system itself
        return;
    }

    {
        // s_compilerMutex is managed by ManagedStatic, it can't be accessed after llvm_shutdown
        MutexGuard lock(*s_compilerMutex);
        -- m_instanceCount;
        if (m_instanceCount == 0)
        {
            shutdown = true;
        }
    }

    if (shutdown)
    {
        ShaderCacheManager::Shutdown();
        llvm_shutdown();
    }
}

// =====================================================================================================================
// Destroys the pipeline compiler.
void Compiler::Destroy()
{
    delete this;
}

// =====================================================================================================================
// Builds shader module from the specified info.
Result Compiler::BuildShaderModule(
    const ShaderModuleBuildInfo* pShaderInfo,   // [in] Info to build this shader module
    ShaderModuleBuildOut*        pShaderOut     // [out] Output of building this shader module
    ) const
{
    Result result = Result::Success;

    // Currently, copy SPIR-V binary as output shader module data
    size_t allocSize = sizeof(ShaderModuleData) + pShaderInfo->shaderBin.codeSize;
    void* pAllocBuf = nullptr;
    BinaryType binType = BinaryType::Spirv;

    // Check the type of input shader binary
    if (IsSpirvBinary(&pShaderInfo->shaderBin))
    {
        binType = BinaryType::Spirv;
        if (VerifySpirvBinary(&pShaderInfo->shaderBin) != Result::Success)
        {
            LLPC_ERRS("Unsupported SPIR-V instructions are found!\n");
            result = Result::Unsupported;
        }
    }
    else if (IsLlvmBitcode(&pShaderInfo->shaderBin))
    {
        binType = BinaryType::LlvmBc;
    }
    else
    {
        result = Result::ErrorInvalidShader;
    }

    if (result == Result::Success)
    {
        if (pShaderInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pShaderInfo->pfnOutputAlloc(pShaderInfo->pInstance,
                                                    pShaderInfo->pUserData,
                                                    allocSize);
            result = (pAllocBuf != nullptr) ? Result::Success : Result::ErrorOutOfMemory;
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }
    }

    if (result == Result::Success)
    {
        ShaderModuleData* pModuleData = reinterpret_cast<ShaderModuleData*>(pAllocBuf);
        memset(pModuleData, 0, sizeof(*pModuleData));

        pModuleData->binType = binType;
        pModuleData->binCode.codeSize = pShaderInfo->shaderBin.codeSize;
        MetroHash::Hash hash = {};
        MetroHash::MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pShaderInfo->shaderBin.pCode),
                          pShaderInfo->shaderBin.codeSize,
                          hash.bytes);
        static_assert(sizeof(pModuleData->hash) == sizeof(hash), "Unexpected value!");

        memcpy(pModuleData->hash, hash.dwords, sizeof(hash));
        if (cl::EnablePipelineDump)
        {
            PipelineDumper::DumpSpirvBinary(cl::PipelineDumpDir.c_str(),
                                          &pShaderInfo->shaderBin,
                                          &hash);
        }

        void* pCode = VoidPtrInc(pAllocBuf, sizeof(ShaderModuleData));
        memcpy(pCode, pShaderInfo->shaderBin.pCode, pShaderInfo->shaderBin.codeSize);
        pModuleData->binCode.pCode = pCode;
        if (pModuleData->binType == BinaryType::Spirv)
        {
            CollectInfoFromSpirvBinary(pModuleData);
        }
        pShaderOut->pModuleData = pModuleData;
    }

    return result;
}

// =====================================================================================================================
// Build graphics pipeline internally
Result Compiler::BuildGraphicsPipelineInternal(
    GraphicsContext*          pGraphicsContext,                     // [in] Graphics context this graphics pipeline
    const PipelineShaderInfo* shaderInfo[ShaderStageGfxCount],      // [in] Shader info of this graphics pipeline
    uint32_t                  forceLoopUnrollCount,                 // [in] Force loop unroll count (0 means disable)
    ElfPackage*               pPipelineElf,                         // [out] Output Elf package
    bool*                     pDynamicLoopUnroll)                   // [out] Need dynamic loop unroll or not
{
    Result          result    = Result::Success;
    bool            skipLower = false;
    bool            skipPatch = false;
    BinaryType      binType   = BinaryType::Unknown;

    Module* modules[ShaderStageCountInternal] = {};

    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pGraphicsContext);

    // Create the AMDGPU TargetMachine.
    result = CodeGenManager::CreateTargetMachine(pContext);

    // Translate SPIR-V binary to machine-independent LLVM module
    for (uint32_t stage = 0; (stage < ShaderStageGfxCount) && (result == Result::Success); ++stage)
    {
        const PipelineShaderInfo* pShaderInfo = shaderInfo[stage];
        if (pShaderInfo->pModuleData == nullptr)
        {
            continue;
        }

        Module* pModule = nullptr;

        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
        // Binary type must same for all shader stages
        LLPC_ASSERT((binType == BinaryType::Unknown) || (pModuleData->binType == binType));
        binType = pModuleData->binType;
        if (binType == BinaryType::Spirv)
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.translateTime);
            result = TranslateSpirvToLlvm(&pModuleData->binCode,
                                          static_cast<ShaderStage>(stage),
                                          pShaderInfo->pEntryTarget,
                                          pShaderInfo->pSpecializationInfo,
                                          pContext,
                                          forceLoopUnrollCount,
                                          &pModule);

            // Check if this module needs dynamic loop unroll. Only do this check when forceLoopUnrollCount is 0. Dynamic loop unroll is only enabled for fragment shader and compute shader.
            if (cl::EnableDynamicLoopUnroll && (result == Result::Success) &&
                (stage == ShaderStageFragment) && (forceLoopUnrollCount == 0))
            {
                *pDynamicLoopUnroll = NeedDynamicLoopUnroll(pModule);
            }
        }
        else if (binType == BinaryType::LlvmBc)
        {
            // Skip lower and patch phase if input is LLVM IR
            skipLower = true;
            skipPatch = true;
            pModule = pContext->LoadLibary(&pModuleData->binCode).release();
        }
        else
        {
            LLPC_NEVER_CALLED();
        }

        // Verify this LLVM module
        if (result == Result::Success)
        {
            LLPC_OUTS("===============================================================================\n");
            LLPC_OUTS("// LLPC SPIRV-to-LLVM translation results (" <<
                      GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
            LLPC_OUTS(*pModule);
            LLPC_OUTS("\n");
            std::string errMsg;
            raw_string_ostream errStream(errMsg);
            if (verifyModule(*pModule, &errStream))
            {
                LLPC_ERRS("Fails to verify module after translation (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader): " <<
                          errStream.str() << "\n");
                result = Result::ErrorInvalidShader;
            }
        }

        // Do SPIR-V lowering operations for this LLVM module
        if ((result == Result::Success) && (skipLower == false))
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.lowerTime);
            result = SpirvLower::Run(pModule, forceLoopUnrollCount);
            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to do SPIR-V lowering operations (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
            }
            else
            {
                LLPC_OUTS("===============================================================================\n");
                LLPC_OUTS("// LLPC SPIRV-lowering results (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
                LLPC_OUTS(*pModule);
                LLPC_OUTS("\n");
            }
        }

        modules[stage] = pModule;
    }

    // Build null fragment shader if necessary
    if ((result == Result::Success) && (pContext->NeedAutoLayoutDesc() == false) &&
          (modules[ShaderStageFragment] == nullptr))
    {
        TimeProfiler timeProfiler(&g_timeProfileResult.lowerTime);
        std::unique_ptr<Module> pNullFsModule;
        result = BuildNullFs(pContext, pNullFsModule);
        if (result == Result::Success)
        {
            modules[ShaderStageFragment] = pNullFsModule.release();
        }
        else
        {
            LLPC_ERRS("Fails to build a LLVM module for null fragment shader\n");
        }
    }

    // Do LLVM module pacthing (preliminary patch work)
    for (int32_t stage = ShaderStageGfxCount - 1; (stage >= 0) && (result == Result::Success); --stage)
    {
        Module* pModule = modules[stage];
        if ((pModule == nullptr) || skipPatch)
        {
            continue;
        }

        TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
        result = Patch::PreRun(pModule);
        if (result != Result::Success)
        {
            LLPC_ERRS("Fails to do preliminary patch work for LLVM module (" <<
                      GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
        }
    }

    // Determine whether or not GS on-chip mode is valid for this pipeline
    if ((result == Result::Success) && (modules[ShaderStageGeometry] != nullptr))
    {
        // NOTE: Always call CheckGsOnChipValidity() even when GS on-chip mode is disabled, because that method
        // also computes esGsRingItemSize and gsVsRingItemSize.
        bool gsOnChip = pContext->CheckGsOnChipValidity();
        pContext->SetGsOnChip(gsOnChip);
    }

    // Do user data node merge for merged shader
    if ((result == Result::Success) && (m_gfxIp.major >= 9))
    {
        pContext->DoUserDataNodeMerge();
    }

    // Do LLVM module patching (main patch work)
    for (int32_t stage = ShaderStageGfxCount - 1; (stage >= 0) && (result == Result::Success); --stage)
    {
        Module* pModule = modules[stage];
        if ((pModule == nullptr) || skipPatch)
        {
            continue;
        }

        TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
        result = Patch::Run(pModule);
        if (result != Result::Success)
        {
            LLPC_ERRS("Fails to patch LLVM module and link it with external library (" <<
                      GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
        }
        else
        {
            LLPC_OUTS("===============================================================================\n");
            LLPC_OUTS("// LLPC patching results (" <<
                      GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
            LLPC_OUTS(*pModule);
            LLPC_OUTS("\n");
        }
    }

    // Do shader merge operations
    if ((result == Result::Success) && (m_gfxIp.major >= 9))
    {
        const bool hasVs = (modules[ShaderStageVertex] != nullptr);
        const bool hasTcs = (modules[ShaderStageTessControl] != nullptr);

        const bool hasTs = ((modules[ShaderStageTessControl] != nullptr) ||
            (modules[ShaderStageTessEval] != nullptr));
        const bool hasGs = (modules[ShaderStageGeometry] != nullptr);

        ShaderMerger shaderMerger(pContext);

        if (hasTs && (hasVs || hasTcs))
        {
            // Ls-HS merged shader should be present
            Module* pLsModule = modules[ShaderStageVertex];
            Module* pHsModule = modules[ShaderStageTessControl];

            Module* pLsHsModule = nullptr;

            TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
            result = shaderMerger.BuildLsHsMergedShader(pLsModule, pHsModule, &pLsHsModule);

            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to build LS-HS merged shader\n");
            }
            else
            {
                LLPC_OUTS("===============================================================================\n");
                LLPC_OUTS("// LLPC shader merge results (LS-HS)\n");
                LLPC_OUTS(*pLsHsModule);
                LLPC_OUTS("\n");
            }

            // NOTE: After LS and HS are merged, LS and HS are destroy. And new LS-HS merged shader is treated
            // as tessellation control shader.
            modules[ShaderStageVertex] = nullptr;
            modules[ShaderStageTessControl] = pLsHsModule;
        }

        if (hasGs)
        {
            // ES-GS merged shader should be present
            Module* pEsModule = modules[hasTs ? ShaderStageTessEval : ShaderStageVertex];
            Module* pGsModule = modules[ShaderStageGeometry];

            {
                Module* pEsGsModule = nullptr;

                TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
                result = shaderMerger.BuildEsGsMergedShader(pEsModule, pGsModule, &pEsGsModule);

                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to build ES-GS merged shader\n");
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC shader merge results (ES-GS)\n");
                    LLPC_OUTS(*pEsGsModule);
                    LLPC_OUTS("\n");
                }

                // NOTE: After ES and GS are merged, ES and GS are destroy. And new ES-GS merged shader is treated
                // as geometry shader.
                modules[hasTs ? ShaderStageTessEval : ShaderStageVertex] = nullptr;
                modules[ShaderStageGeometry] = pEsGsModule;
            }
        }
    }

    // Build copy shader if necessary (has geometry shader)
    if ((result == Result::Success) && (modules[ShaderStageGeometry] != nullptr))
    {
        TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);
        result = BuildCopyShader(pContext, &modules[ShaderStageCopyShader]);
        if (result != Result::Success)
        {
            LLPC_ERRS("Fails to build a LLVM module for copy shader\n");
        }
    }

    // Create an empty module then link each shader module into it.
    auto pPipelineModule = new Module("llpcPipeline", *pContext);
    {
        Linker linker(*pPipelineModule);
        for (int32_t stage = 0; (stage < ShaderStageCountInternal) && (result == Result::Success); ++stage)
        {
            Module* pShaderModule = modules[stage];
            if (pShaderModule == nullptr)
            {
                continue;
            }
            // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
            // linked into pipeline module.
            if (linker.linkInModule(std::unique_ptr<Module>(pShaderModule)))
            {
                LLPC_ERRS("Fails to link shader module into pipeline module (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
                result = Result::ErrorInvalidShader;
            }
        }

        if (result == Result::Success)
        {
            LLPC_OUTS("===============================================================================\n");
            LLPC_OUTS("// LLPC linking results\n");
            LLPC_OUTS(*pPipelineModule);
            LLPC_OUTS("\n");
        }
    }

    if (result == Result::Success)
    {
        CodeGenManager::SetupTargetFeatures(pPipelineModule);

        // Generate GPU ISA binary. If "filetype=asm" is specified, generate ISA assembly text instead.
        // If "-emit-llvm" is specified, generate LLVM bitcode. These options are used through LLPC
        // standalone compiler tool "amdllpc".
        raw_svector_ostream elfStream(*pPipelineElf);
        std::string errMsg;

        TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);

        result = CodeGenManager::GenerateCode(pPipelineModule, elfStream, errMsg);
        if (result != Result::Success)
        {
            LLPC_ERRS("Fails to generate GPU ISA codes :" << errMsg << "\n");
        }
    }
    delete pPipelineModule;
    pPipelineModule = nullptr;

    ReleaseContext(pContext);

    return result;
}

// =====================================================================================================================
// Build graphics pipeline from the specified info.
Result Compiler::BuildGraphicsPipeline(
    const GraphicsPipelineBuildInfo* pPipelineInfo, // [in] Info to build this graphics pipeline
    GraphicsPipelineBuildOut*        pPipelineOut)  // [out] Output of building this graphics pipeline
{
    Result           result  = Result::Success;
    CacheEntryHandle hEntry  = nullptr;
    const void*      pElf    = nullptr;
    size_t           elfSize = 0;

    const PipelineShaderInfo* shaderInfo[ShaderStageGfxCount] =
    {
        &pPipelineInfo->vs,
        &pPipelineInfo->tcs,
        &pPipelineInfo->tes,
        &pPipelineInfo->gs,
        &pPipelineInfo->fs,
    };

    for (uint32_t i = 0; (i < ShaderStageGfxCount) && (result == Result::Success); ++i)
    {
        result = ValidatePipelineShaderInfo(static_cast<ShaderStage>(i), shaderInfo[i]);
    }

    MetroHash::Hash cacheHash = {};
    MetroHash::Hash pipelineHash = {};
    cacheHash = PipelineDumper::GenerateHashForGraphicsPipeline(pPipelineInfo, true);
    pipelineHash = PipelineDumper::GenerateHashForGraphicsPipeline(pPipelineInfo, false);

    // Do shader replacement if it's enabled
    bool ShaderReplaced = false;
    const ShaderModuleData* restoreModuleData[ShaderStageGfxCount] = {};
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        char pipelineHashString[64];
        int32_t length = snprintf(pipelineHashString, 64, "0x%016" PRIX64, MetroHash::Compact64(&pipelineHash));
        LLPC_UNUSED(length);

        bool hashMatch = true;
        if (cl::ShaderReplaceMode == ShaderReplaceShaderPipelineHash)
        {
            std::string pipelineReplacementHashes = cl::ShaderReplacePipelineHashes;
            hashMatch = (pipelineReplacementHashes.find(pipelineHashString) != std::string::npos);

            if (hashMatch)
            {
                LLPC_OUTS("// Shader replacement for graphics pipeline: " << pipelineHashString << "\n");
            }
        }

        if (hashMatch)
        {
            for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
            {
                const ShaderModuleData* pOrigModuleData =
                    reinterpret_cast<const ShaderModuleData*>(shaderInfo[stage]->pModuleData);
                if (pOrigModuleData != nullptr)
                {
                    ShaderModuleData* pModuleData = nullptr;
                    if (ReplaceShader(pOrigModuleData, &pModuleData) == Result::Success)
                    {

                        ShaderReplaced = true;
                        restoreModuleData[stage] = pOrigModuleData;
                        const_cast<PipelineShaderInfo*>(shaderInfo[stage])->pModuleData = pModuleData;

                        char shaderHash[64] = {};
                        auto pHash = reinterpret_cast<const MetroHash::Hash*>(&restoreModuleData[stage]->hash[0]);
                        int32_t length = snprintf(shaderHash,
                                                  64,
                                                  "0x%016" PRIX64,
                                                  MetroHash::Compact64(pHash));
                        LLPC_UNUSED(length);
                        LLPC_OUTS("// Shader replacement for shader: " << shaderHash
                                  << ", in pipeline: " << pipelineHashString << "\n");
                    }
                }
            }

            if (ShaderReplaced)
            {
                // Update pipeline hash after shader replacement
                cacheHash = PipelineDumper::GenerateHashForGraphicsPipeline(pPipelineInfo, true);
            }
        }
    }

    if ((result == Result::Success) && EnableOuts())
    {
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC calculated hash results (graphics pipline)\n");
        LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::Compact64(&pipelineHash)) << "\n");
        for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
        {
            const ShaderModuleData* pModuleData =
                reinterpret_cast<const ShaderModuleData*>(shaderInfo[stage]->pModuleData);
            auto pHash = reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash[0]);
            if (pModuleData != nullptr)
            {
                LLPC_OUTS(format("%-4s : ", GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true)) <<
                          format("0x%016" PRIX64, MetroHash::Compact64(pHash)) << "\n");
            }
        }
        LLPC_OUTS("\n");
    }

    PipelineDumpFile* pPipelineDumperFile = nullptr;

    if ((result == Result::Success) && cl::EnablePipelineDump)
    {
        PipelineDumpOptions dumpOptions = {};
        dumpOptions.pDumpDir                 = cl::PipelineDumpDir.c_str();
        dumpOptions.filterPipelineDumpByType = cl::FilterPipelineDumpByType;
        dumpOptions.filterPipelineDumpByHash = cl::FilterPipelineDumpByHash;
        dumpOptions.dumpDuplicatePipelines   = cl::DumpDuplicatePipelines;
        pPipelineDumperFile = PipelineDumper::BeginPipelineDump(&dumpOptions, nullptr, pPipelineInfo, &pipelineHash);
        std::stringstream strStream;
        strStream << ";Compiler Options: ";
        for (auto& option : m_options)
        {
            strStream << option << " ";
        }
        std::string extraInfo = strStream.str();
        PipelineDumper::DumpPipelineExtraInfo(pPipelineDumperFile, &extraInfo);
    }

    ShaderEntryState cacheEntryState = ShaderEntryState::New;
    ShaderCache* pShaderCache = (pPipelineInfo->pShaderCache != nullptr) ?
                                    static_cast<ShaderCache*>(pPipelineInfo->pShaderCache) :
                                    m_shaderCache.get();
    if (cl::ShaderCacheMode == ShaderCacheForceInternalCacheOnDisk)
    {
        pShaderCache = m_shaderCache.get();
    }

    if (result == Result::Success)
    {
        if (ShaderReplaced)
        {
            cacheEntryState = ShaderEntryState::Compiling;
        }
        else
        {
            cacheEntryState = pShaderCache->FindShader(cacheHash, true, &hEntry);
            if (cacheEntryState == ShaderEntryState::Ready)
            {
                result = pShaderCache->RetrieveShader(hEntry, &pElf, &elfSize);
                // Re-try if shader cache return error unknown
                if (result == Result::ErrorUnknown)
                {
                    result = Result::Success;
                    hEntry = nullptr;
                    cacheEntryState = ShaderEntryState::Compiling;
                }
            }
        }
    }

    constexpr uint32_t CandidateCount = 4;
    uint32_t           loopUnrollCountCandidates[CandidateCount] = { 32, 16, 4, 1 };
    bool               needRecompile   = false;
    uint32_t           candidateIdx    = 0;
    ElfPackage         candidateElfs[CandidateCount];

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        std::vector<LoopAnalysisInfo> loopInfo;
        PipelineStatistics            pipelineStats[CandidateCount];
        uint32_t                      forceLoopUnrollCount = cl::ForceLoopUnrollCount;

        GraphicsContext graphicsContext(m_gfxIp, &m_gpuProperty, &m_gpuWorkarounds, pPipelineInfo, &pipelineHash);
        result = BuildGraphicsPipelineInternal(&graphicsContext,
                                               shaderInfo,
                                               forceLoopUnrollCount,
                                               &candidateElfs[candidateIdx],
                                               &needRecompile);

        if ((result == Result::Success) && needRecompile)
        {
            GetPipelineStatistics(candidateElfs[candidateIdx].data(),
                                  candidateElfs[candidateIdx].size(),
                                  m_gfxIp,
                                  &pipelineStats[candidateIdx]);

            LLPC_OUTS("// Dynamic loop unroll analysis pass " << candidateIdx << ":\n");
            LLPC_OUTS("//    loopUnrollCount =  " << loopUnrollCountCandidates[candidateIdx] <<
                      "; numUsedVgrps = " << pipelineStats[candidateIdx].numUsedVgprs <<
                      "; sgprSpill = " << pipelineStats[candidateIdx].sgprSpill <<
                      "; useScratchBuffer = " << pipelineStats[candidateIdx].useScratchBuffer << " \n");

            for (candidateIdx = 1; candidateIdx < CandidateCount; candidateIdx++)
            {
                forceLoopUnrollCount = loopUnrollCountCandidates[candidateIdx];
                GraphicsContext graphicsContext(m_gfxIp, &m_gpuProperty, &m_gpuWorkarounds, pPipelineInfo, &pipelineHash);
                result = BuildGraphicsPipelineInternal(&graphicsContext,
                                                       shaderInfo,
                                                       forceLoopUnrollCount,
                                                       &candidateElfs[candidateIdx],
                                                       &needRecompile);

                if (result == Result::Success)
                {
                    GetPipelineStatistics(candidateElfs[candidateIdx].data(),
                                          candidateElfs[candidateIdx].size(),
                                          m_gfxIp,
                                          &pipelineStats[candidateIdx]);

                    LLPC_OUTS("// Dynamic loop unroll analysis pass " << candidateIdx << ":\n");
                    LLPC_OUTS("//    loopUnrollCount =  " << loopUnrollCountCandidates[candidateIdx] <<
                              "; numUsedVgrps = " << pipelineStats[candidateIdx].numUsedVgprs <<
                              "; sgprSpill = " << pipelineStats[candidateIdx].sgprSpill <<
                              "; useScratchBuffer = " << pipelineStats[candidateIdx].useScratchBuffer << " \n");
                }
            }
        }

        candidateIdx = 0;
        if ((result == Result::Success) && needRecompile)
        {
            candidateIdx = ChooseLoopUnrollCountCandidate(pipelineStats, CandidateCount);
            LLPC_OUTS("// Dynamic loop unroll: choose candidate = " << candidateIdx <<
                      "; loopUnrollCount = " << loopUnrollCountCandidates[candidateIdx] << " \n");
        }

        if (result == Result::Success)
        {
            elfSize = candidateElfs[candidateIdx].size();
            pElf = candidateElfs[candidateIdx].data();
        }

        if ((ShaderReplaced == false) && (hEntry != nullptr))
        {
            if (result == Result::Success)
            {
                LLPC_ASSERT(elfSize > 0);
                pShaderCache->InsertShader(hEntry, pElf, elfSize);
            }
            else
            {
                pShaderCache->ResetShader(hEntry);
            }
        }
    }

    if (result == Result::Success)
    {
        void* pAllocBuf = nullptr;
        if (pPipelineInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pPipelineInfo->pfnOutputAlloc(pPipelineInfo->pInstance, pPipelineInfo->pUserData, elfSize);
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }

        uint8_t* pCode = static_cast<uint8_t*>(pAllocBuf);
        memcpy(pCode, pElf, elfSize);

        pPipelineOut->pipelineBin.codeSize = elfSize;
        pPipelineOut->pipelineBin.pCode = pCode;
    }

    if (pPipelineDumperFile != nullptr)
    {
        if (result == Result::Success)
        {
            PipelineDumper::DumpPipelineBinary(pPipelineDumperFile,
                                               m_gfxIp,
                                               &pPipelineOut->pipelineBin);

            if (needRecompile)
            {
                std::stringstream strStream;
                strStream << "Dynamic loop unroll enabled: loopUnrollCount = " << loopUnrollCountCandidates[candidateIdx] << " \n";
                std::string extraInfo = strStream.str();
                PipelineDumper::DumpPipelineExtraInfo(pPipelineDumperFile, &extraInfo);
            }
        }

        PipelineDumper::EndPipelineDump(pPipelineDumperFile);
    }

    // Free shader replacement allocations and restore original shader module
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
        {
            if (restoreModuleData[stage] != nullptr)
            {
                delete reinterpret_cast<const char*>(shaderInfo[stage]->pModuleData);
                const_cast<PipelineShaderInfo*>(shaderInfo[stage])->pModuleData = restoreModuleData[stage];
            }
        }
    }

    if (cl::EnableTimeProfiler)
    {
        DumpTimeProfilingResult(&pipelineHash);
    }

    return result;
}

// =====================================================================================================================
// Build compute pipeline internally
Result Compiler::BuildComputePipelineInternal(
    ComputeContext*                 pComputeContext,                // [in] Compute context this compute pipeline
    const ComputePipelineBuildInfo* pPipelineInfo,                  // [in] Pipeline info of this compute pipeline
    uint32_t                        forceLoopUnrollCount,           // [in] Force loop unroll count (0 means disable)
    ElfPackage*                     pPipelineElf,                   // [out] Output Elf package
    bool*                           pDynamicLoopUnroll)             // [out] Need dynamic loop unroll or not
{
    Result result   = Result::Success;
    bool skipPatch  = false;
    Module* pModule = nullptr;
    std::unique_ptr<Module> bitcode;

    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pComputeContext);

    // Create the AMDGPU target machine.
    result = CodeGenManager::CreateTargetMachine(pContext);

    // Translate SPIR-V binary to machine-independent LLVM module
    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
    if (pModuleData != nullptr)
    {
        if (pModuleData->binType == BinaryType::Spirv)
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.translateTime);

            result = TranslateSpirvToLlvm(&pModuleData->binCode,
                                          ShaderStageCompute,
                                          pPipelineInfo->cs.pEntryTarget,
                                          pPipelineInfo->cs.pSpecializationInfo,
                                          pContext,
                                          forceLoopUnrollCount,
                                          &pModule);

            // Check if this module needs dynamic loop unroll. Only do this check when forceLoopUnrollCount is 0. Dynamic loop unroll is only enabled for fragment shader and compute shader.
            if (cl::EnableDynamicLoopUnroll && (result == Result::Success) &&
                (forceLoopUnrollCount == 0))
            {
                *pDynamicLoopUnroll = NeedDynamicLoopUnroll(pModule);
            }

            // Verify this LLVM module
            if (result == Result::Success)
            {
                LLPC_OUTS("===============================================================================\n");
                LLPC_OUTS("// LLPC SPIRV-to-LLVM translation results (" <<
                          GetShaderStageName(ShaderStageCompute) << " shader)\n");
                LLPC_OUTS(*pModule);
                LLPC_OUTS("\n");
                std::string errMsg;
                raw_string_ostream errStream(errMsg);
                if (verifyModule(*pModule, &errStream))
                {
                    LLPC_ERRS("Fails to verify module after translation: (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader) :" << errStream.str() << "\n");
                    result = Result::ErrorInvalidShader;
                }
            }

            // Do SPIR-V lowering operations for this LLVM module
            if (result == Result::Success)
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.lowerTime);
                result = SpirvLower::Run(pModule, forceLoopUnrollCount);
                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to do SPIR-V lowering operations (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader)\n");
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC SPIRV-lowering results (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader)\n");
                    LLPC_OUTS(*pModule);
                    LLPC_OUTS("\n");
                }
            }
        }
        else
        {
            // TODO: Handle other binary types.
            LLPC_NOT_IMPLEMENTED();
        }
    }
    else if (pModuleData->binType == BinaryType::LlvmBc)
    {
        // Skip lower and patch phase if input is LLVM IR
        skipPatch = true;
        bitcode = pContext->LoadLibary(&pModuleData->binCode);
        pModule = bitcode.get();
    }
    else
    {
        LLPC_NEVER_CALLED();
    }

    // Do LLVM module pacthing and generate GPU ISA codes
    if (result == Result::Success)
    {
        LLPC_ASSERT(pModule != nullptr);

        // Preliminary patch work
        if (skipPatch == false)
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
            result = Patch::PreRun(pModule);
        }

        if (result != Result::Success)
        {
            LLPC_ERRS("Fails to do preliminary patch work for LLVM module (" <<
                      GetShaderStageName(ShaderStageCompute) << " shader)\n");
        }

        // Main patch work
        if (result == Result::Success)
        {
            if (skipPatch == false)
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
                result = Patch::Run(pModule);
            }

            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to patch LLVM module and link it with external library (" <<
                          GetShaderStageName(ShaderStageCompute) << " shader)\n");
            }
            else
            {
                LLPC_OUTS("===============================================================================\n");
                LLPC_OUTS("// LLPC patching result (" <<
                          GetShaderStageName(ShaderStageCompute) << " shader)\n");
                LLPC_OUTS(*pModule);
                LLPC_OUTS("\n");
            }
        }

        if (result == Result::Success)
        {
            CodeGenManager::SetupTargetFeatures(pModule);

            // Generate GPU ISA binary. If "filetype=asm" is specified, generate ISA assembly text
            // instead.  If "-emit-llvm" is specified, generate LLVM bitcode. These options are used
            // through LLPC standalone compiler tool "amdllpc".
            raw_svector_ostream elfStream(*pPipelineElf);
            std::string errMsg;

            TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);

            result = CodeGenManager::GenerateCode(pModule, elfStream, errMsg);
            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to generate GPU ISA codes (" <<
                          GetShaderStageName(ShaderStageCompute) << " shader) : " << errMsg << "\n");
            }
        }
    }

    if (bitcode != nullptr)
    {
        LLPC_ASSERT(bitcode.get() == pModule);
        bitcode = nullptr;
        pModule = nullptr;
    }
    else
    {
        delete pModule;
        pModule = nullptr;
    }

    ReleaseContext(pContext);
    return result;
}

// =====================================================================================================================
// Build compute pipeline from the specified info.
Result Compiler::BuildComputePipeline(
    const ComputePipelineBuildInfo* pPipelineInfo,  // [in] Info to build this compute pipeline
    ComputePipelineBuildOut*        pPipelineOut)   // [out] Output of building this compute pipeline
{
    CacheEntryHandle hEntry    = nullptr;
    const void*      pElf      = nullptr;
    size_t           elfSize   = 0;

    Result result = ValidatePipelineShaderInfo(ShaderStageCompute, &pPipelineInfo->cs);

    MetroHash::Hash cacheHash = {};
    MetroHash::Hash pipelineHash = {};
    cacheHash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, true);
    pipelineHash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, false);

    // Do shader replacement if it's enabled
    bool ShaderReplaced = false;
    const ShaderModuleData* pRestoreModuleData = nullptr;
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        char pipelineHashString[64];
        int32_t length = snprintf(pipelineHashString, 64, "0x%016" PRIX64, MetroHash::Compact64(&pipelineHash));
        LLPC_UNUSED(length);

        bool hashMatch = true;
        if (cl::ShaderReplaceMode == ShaderReplaceShaderPipelineHash)
        {
            std::string pipelineReplacementHashes = cl::ShaderReplacePipelineHashes;
            hashMatch = (pipelineReplacementHashes.find(pipelineHashString) != std::string::npos);

            if (hashMatch)
            {
                LLPC_OUTS("// Shader replacement for compute pipeline: " << pipelineHashString << "\n");
            }
        }

        if (hashMatch)
        {
            const ShaderModuleData* pOrigModuleData =
                reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
            if (pOrigModuleData != nullptr)
            {
                ShaderModuleData* pModuleData = nullptr;
                if (ReplaceShader(pOrigModuleData, &pModuleData) == Result::Success)
                {
                    ShaderReplaced = true;
                    pRestoreModuleData = pOrigModuleData;
                    const_cast<PipelineShaderInfo*>(&pPipelineInfo->cs)->pModuleData = pModuleData;

                    char shaderHash[64];
                    auto pHash = reinterpret_cast<const MetroHash::Hash*>(&pRestoreModuleData->hash[0]);
                    int32_t length = snprintf(shaderHash, 64, "0x%016" PRIX64, MetroHash::Compact64(pHash));
                    LLPC_UNUSED(length);
                    LLPC_OUTS("// Shader replacement for shader: " << shaderHash
                               << ", in pipeline: " << pipelineHashString << "\n");
                }
            }

            if (ShaderReplaced)
            {
                // Update pipeline hash after shader replacement
                cacheHash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, true);
            }
        }
    }

    if ((result == Result::Success) && EnableOuts())
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
        auto pModuleHash = reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash[0]);
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC calculated hash results (compute pipline)\n");
        LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::Compact64(&pipelineHash)) << "\n");
        LLPC_OUTS(format("%-4s : ", GetShaderStageAbbreviation(ShaderStageCompute, true)) <<
                  format("0x%016" PRIX64, MetroHash::Compact64(pModuleHash)) << "\n");
        LLPC_OUTS("\n");
    }

    PipelineDumpFile* pPipelineDumperFile = nullptr;
    if ((result == Result::Success) && cl::EnablePipelineDump)
    {
        PipelineDumpOptions dumpOptions = {};
        dumpOptions.pDumpDir                 = cl::PipelineDumpDir.c_str();
        dumpOptions.filterPipelineDumpByType = cl::FilterPipelineDumpByType;
        dumpOptions.filterPipelineDumpByHash = cl::FilterPipelineDumpByHash;
        dumpOptions.dumpDuplicatePipelines   = cl::DumpDuplicatePipelines;
        pPipelineDumperFile = PipelineDumper::BeginPipelineDump(&dumpOptions, pPipelineInfo, nullptr, &pipelineHash);
        std::stringstream strStream;
        strStream << ";Compiler Options: ";
        for (auto& option : m_options)
        {
            strStream << option << " ";
        }
        std::string extraInfo = strStream.str();
        PipelineDumper::DumpPipelineExtraInfo(pPipelineDumperFile, &extraInfo);
    }

    ShaderEntryState cacheEntryState = ShaderEntryState::New;
    ShaderCache* pShaderCache = (pPipelineInfo->pShaderCache != nullptr) ?
                                    static_cast<ShaderCache*>(pPipelineInfo->pShaderCache) :
                                    m_shaderCache.get();
    if (cl::ShaderCacheMode == ShaderCacheForceInternalCacheOnDisk)
    {
        pShaderCache = m_shaderCache.get();
    }

    if (result == Result::Success)
    {
        if (ShaderReplaced)
        {
            cacheEntryState = ShaderEntryState::Compiling;
        }
        else
        {
            cacheEntryState = pShaderCache->FindShader(cacheHash, true, &hEntry);
            if (cacheEntryState == ShaderEntryState::Ready)
            {
                result = pShaderCache->RetrieveShader(hEntry, &pElf, &elfSize);
                // Re-try if shader cache return error unknown
                if (result == Result::ErrorUnknown)
                {
                    result = Result::Success;
                    hEntry = nullptr;
                    cacheEntryState = ShaderEntryState::Compiling;
                }
            }
        }
    }

    constexpr uint32_t CandidateCount = 4;
    uint32_t           loopUnrollCountCandidates[CandidateCount] = { 32, 16, 4, 1 };
    bool               needRecompile   = false;
    uint32_t           candidateIdx    = 0;
    ElfPackage         candidateElfs[CandidateCount];

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        std::vector<LoopAnalysisInfo> loopInfo;
        PipelineStatistics            pipelineStats[CandidateCount];
        uint32_t                      forceLoopUnrollCount = cl::ForceLoopUnrollCount;

        ComputeContext computeContext(m_gfxIp, &m_gpuProperty, &m_gpuWorkarounds, pPipelineInfo, &pipelineHash);

        result = BuildComputePipelineInternal(&computeContext,
                                              pPipelineInfo,
                                              forceLoopUnrollCount,
                                              &candidateElfs[candidateIdx],
                                              &needRecompile);

        if ((result == Result::Success) && needRecompile)
        {
            GetPipelineStatistics(candidateElfs[candidateIdx].data(),
                                  candidateElfs[candidateIdx].size(),
                                  m_gfxIp,
                                  &pipelineStats[candidateIdx]);

            LLPC_OUTS("// Dynamic loop unroll analysis pass " << candidateIdx << ":\n");
            LLPC_OUTS("//    loopUnrollCount =  " << loopUnrollCountCandidates[candidateIdx] <<
                      "; numUsedVgrps = " << pipelineStats[candidateIdx].numUsedVgprs <<
                      "; sgprSpill = " << pipelineStats[candidateIdx].sgprSpill <<
                      "; useScratchBuffer = " << pipelineStats[candidateIdx].useScratchBuffer << " \n");

            for (candidateIdx = 1; candidateIdx < CandidateCount; candidateIdx++)
            {
                forceLoopUnrollCount = loopUnrollCountCandidates[candidateIdx];
                ComputeContext computeContext(m_gfxIp, &m_gpuProperty, &m_gpuWorkarounds, pPipelineInfo, &pipelineHash);

                result = BuildComputePipelineInternal(&computeContext,
                                                      pPipelineInfo,
                                                      forceLoopUnrollCount,
                                                      &candidateElfs[candidateIdx],
                                                      &needRecompile);

                if ((result == Result::Success) && needRecompile)
                {
                    GetPipelineStatistics(candidateElfs[candidateIdx].data(),
                                          candidateElfs[candidateIdx].size(),
                                          m_gfxIp,
                                          &pipelineStats[candidateIdx]);

                    LLPC_OUTS("// Dynamic loop unroll analysis pass " << candidateIdx << ":\n");
                    LLPC_OUTS("//    loopUnrollCount =  " << loopUnrollCountCandidates[candidateIdx] <<
                              "; numUsedVgrps = " << pipelineStats[candidateIdx].numUsedVgprs <<
                              "; sgprSpill = " << pipelineStats[candidateIdx].sgprSpill <<
                              "; useScratchBuffer = " << pipelineStats[candidateIdx].useScratchBuffer << " \n");

                }
            }
        }

        candidateIdx = 0;
        if ((result == Result::Success) && needRecompile)
        {
            candidateIdx = ChooseLoopUnrollCountCandidate(pipelineStats, CandidateCount);
            LLPC_OUTS("// Dynamic loop unroll: choose candidate = " << candidateIdx <<
                      "; loopUnrollCount = " << loopUnrollCountCandidates[candidateIdx] << " \n");
        }

        if (result == Result::Success)
        {
            elfSize = candidateElfs[candidateIdx].size();
            pElf = candidateElfs[candidateIdx].data();
        }

        if ((ShaderReplaced == false) && (hEntry != nullptr))
        {
            if (result == Result::Success)
            {
                LLPC_ASSERT(elfSize > 0);
                pShaderCache->InsertShader(hEntry, pElf, elfSize);
            }
            else
            {
                pShaderCache->ResetShader(hEntry);
            }
        }
    }

    if (result == Result::Success)
    {
        void* pAllocBuf = nullptr;
        if (pPipelineInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pPipelineInfo->pfnOutputAlloc(pPipelineInfo->pInstance, pPipelineInfo->pUserData, elfSize);
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }

        uint8_t* pCode = static_cast<uint8_t*>(pAllocBuf);
        memcpy(pCode, pElf, elfSize);

        pPipelineOut->pipelineBin.codeSize = elfSize;
        pPipelineOut->pipelineBin.pCode = pCode;
    }

    if (pPipelineDumperFile != nullptr)
    {
        if (result == Result::Success)
        {
            PipelineDumper::DumpPipelineBinary(pPipelineDumperFile,
                                               m_gfxIp,
                                               &pPipelineOut->pipelineBin);

            if (needRecompile)
            {
                std::stringstream strStream;
                strStream << "Dynamic loop unroll enabled: loopUnrollCount = " << loopUnrollCountCandidates[candidateIdx] << " \n";
                std::string extraInfo = strStream.str();
                PipelineDumper::DumpPipelineExtraInfo(pPipelineDumperFile, &extraInfo);
            }
        }

        PipelineDumper::EndPipelineDump(pPipelineDumperFile);
    }

    // Free shader replacement allocations and restore original shader module
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        if (pRestoreModuleData != nullptr)
        {
            delete reinterpret_cast<const char*>(pPipelineInfo->cs.pModuleData);
            const_cast<PipelineShaderInfo*>(&pPipelineInfo->cs)->pModuleData = pRestoreModuleData;
        }
    }

    if (cl::EnableTimeProfiler)
    {
        DumpTimeProfilingResult(&pipelineHash);
    }

    return result;
}

// =====================================================================================================================
// Does shader replacement if it is feasible (files used by replacement exist as expected).
Result Compiler::ReplaceShader(
    const ShaderModuleData*     pOrigModuleData,    // [in] Original shader module
    ShaderModuleData**          ppModuleData        // [out] Resuling shader module after shader replacement
    ) const
{
    auto pModuleHash = reinterpret_cast<const MetroHash::Hash*>(&pOrigModuleData->hash[0]);
    uint64_t shaderHash = MetroHash::Compact64(pModuleHash);

    char fileName[64];
    int32_t length = snprintf(fileName, 64, "Shader_0x%016" PRIX64 "_replace.spv", shaderHash);
    LLPC_UNUSED(length);
    std::string replaceFileName = cl::ShaderReplaceDir;
    replaceFileName += "/";
    replaceFileName += fileName;

    Result result = File::Exists(replaceFileName.c_str()) ? Result::Success : Result::ErrorUnavailable;
    if (result == Result::Success)
    {
        File shaderFile;
        result = shaderFile.Open(replaceFileName.c_str(), FileAccessRead | FileAccessBinary);
        if (result == Result::Success)
        {
            size_t binSize = File::GetFileSize(replaceFileName.c_str());

            void *pAllocBuf = new char[binSize + sizeof(ShaderModuleData)];
            ShaderModuleData *pModuleData = reinterpret_cast<ShaderModuleData*>(pAllocBuf);
            pAllocBuf = VoidPtrInc(pAllocBuf, sizeof(ShaderModuleData));

            void* pShaderBin = pAllocBuf;
            shaderFile.Read(pShaderBin, binSize, nullptr);

            pModuleData->binType = pOrigModuleData->binType;
            pModuleData->binCode.codeSize = binSize;
            pModuleData->binCode.pCode = pShaderBin;
            MetroHash::Hash hash = {};
            MetroHash::MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pShaderBin), binSize, hash.bytes);
            memcpy(&pModuleData->hash, &hash, sizeof(hash));

            *ppModuleData = pModuleData;

            shaderFile.Close();
        }
    }

    return result;
}

// =====================================================================================================================
// Translates SPIR-V binary to machine-independent LLVM module.
Result Compiler::TranslateSpirvToLlvm(
    const BinaryData*           pSpirvBin,           // [in] SPIR-V binary
    ShaderStage                 shaderStage,         // Shader stage
    const char*                 pEntryTarget,        // [in] SPIR-V entry point
    const VkSpecializationInfo* pSpecializationInfo, // [in] Specialization info
    LLVMContext*                pContext,            // [in] LLPC pipeline context
    uint32_t                    forceLoopUnrollCount,// [in] Force loop unroll count
    Module**                    ppModule             // [out] Created LLVM module after this translation
    ) const
{
    Result result = Result::Success;

    BinaryData  optSpirvBin = {};

    if (OptimizeSpirv(pSpirvBin, &optSpirvBin) == Result::Success)
    {
        pSpirvBin = &optSpirvBin;
    }

    std::string spirvCode(static_cast<const char*>(pSpirvBin->pCode), pSpirvBin->codeSize);
    std::istringstream spirvStream(spirvCode);
    std::string errMsg;
    SPIRVSpecConstMap specConstMap;

    // Build specialization constant map
    if (pSpecializationInfo != nullptr)
    {
        for (uint32_t i = 0; i < pSpecializationInfo->mapEntryCount; ++i)
        {
            SPIRVSpecConstEntry specConstEntry  = {};
            auto pMapEntry = &pSpecializationInfo->pMapEntries[i];
            specConstEntry.DataSize= pMapEntry->size;
            specConstEntry.Data = VoidPtrInc(pSpecializationInfo->pData, pMapEntry->offset);
            specConstMap[pMapEntry->constantID] = specConstEntry;
        }
    }

    if (readSpirv(*pContext,
                    spirvStream,
                    static_cast<spv::ExecutionModel>(shaderStage),
                    pEntryTarget,
                    specConstMap,
                    *ppModule,
                    errMsg) == false)
    {
        LLPC_ERRS("Fails to translate SPIR-V to LLVM (" <<
                    GetShaderStageName(static_cast<ShaderStage>(shaderStage)) << " shader): " << errMsg << "\n");
        result = Result::ErrorInvalidShader;
    }

    CleanOptimizedSpirv(&optSpirvBin);

    return result;
}

// =====================================================================================================================
// Optimizes SPIR-V binary
Result Compiler::OptimizeSpirv(
    const BinaryData* pSpirvBinIn,     // [in] Input SPIR-V binary
    BinaryData*       pSpirvBinOut     // [out] Optimized SPIR-V binary
    ) const
{
    bool success = false;
    uint32_t optBinSize = 0;
    void* pOptBin = nullptr;

#ifdef LLPC_ENABLE_SPIRV_OPT
    if (cl::EnableSpirvOpt)
    {
        char logBuf[4096] = {};
        success = spvOptimizeSpirv(pSpirvBinIn->codeSize,
                                   pSpirvBinIn->pCode,
                                   0,
                                   nullptr,
                                   &optBinSize,
                                   &pOptBin,
                                   4096,
                                   logBuf);
        if (success == false)
        {
            LLPC_ERRS(logBuf);
        }
    }
#endif

    if (success)
    {
        pSpirvBinOut->codeSize = optBinSize;
        pSpirvBinOut->pCode = pOptBin;
    }
    else
    {
        pSpirvBinOut->codeSize = 0;
        pSpirvBinOut->pCode = nullptr;
    }
    return success ? Result::Success : Result::ErrorInvalidShader;
}

// =====================================================================================================================
// Cleanup work for SPIR-V binary, freeing the allocated buffer by OptimizeSpirv()
void Compiler::CleanOptimizedSpirv(
    BinaryData* pSpirvBin   // [in] Optimized SPIR-V binary
    ) const
{
#ifdef LLPC_ENABLE_SPIRV_OPT
    if (pSpirvBin->pCode)
    {
        spvFreeBuffer(const_cast<void*>(pSpirvBin->pCode));
    }
#endif
}

// =====================================================================================================================
// Builds hash code from compilation-options
MetroHash::Hash Compiler::GenerateHashForCompileOptions(
    uint32_t          optionCount,    // Count of compilation-option strings
    const char*const* pOptions        // [in] An array of compilation-option strings
    )
{
    // Options which needn't affect compilation results
    static StringRef IgnoredOptions[] =
    {
        cl::PipelineDumpDir.ArgStr,
        cl::EnablePipelineDump.ArgStr,
        cl::EnableTimeProfiler.ArgStr,
        cl::ShaderCacheFileDir.ArgStr,
        cl::ShaderCacheMode.ArgStr,
        cl::ShaderReplaceMode.ArgStr,
        cl::ShaderReplaceDir.ArgStr,
        cl::ShaderReplacePipelineHashes.ArgStr,
        cl::EnableOuts.ArgStr,
        cl::EnableErrs.ArgStr,
        cl::LogFileDbgs.ArgStr,
        cl::LogFileOuts.ArgStr,
        cl::EnableShadowDescriptorTable.ArgStr,
        cl::ShadowDescTablePtrHigh.ArgStr,
    };

    std::set<StringRef> effectingOptions;
    // Build effecting options
    for (uint32_t i = 1; i < optionCount; ++i)
    {
        StringRef option = pOptions[i] + 1;  // Skip '-' in options
        bool ignore = false;
        for (uint32_t j = 0; j < sizeof(IgnoredOptions) / sizeof(IgnoredOptions[0]); ++j)
        {
            if (option.startswith(IgnoredOptions[j]))
            {
                ignore = true;
                break;
            }
        }

        if (ignore == false)
        {
            effectingOptions.insert(option);
        }
    }

    MetroHash::MetroHash64 hasher;

    // Build hash code from effecting options
    for (auto option : effectingOptions)
    {
        hasher.Update(reinterpret_cast<const uint8_t*>(option.data()), option.size());
    }

    MetroHash::Hash hash = {};
    hasher.Finalize(hash.bytes);

    return hash;
}

// =====================================================================================================================
// Checks whether fields in pipeline shader info are valid.
Result Compiler::ValidatePipelineShaderInfo(
    ShaderStage               shaderStage,    // Shader stage
    const PipelineShaderInfo* pShaderInfo     // [in] Pipeline shader info
    ) const
{
    Result result = Result::Success;
    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
    if (pModuleData != nullptr)
    {
        if (pModuleData->binType == BinaryType::Spirv)
        {
            auto pSpirvBin = &pModuleData->binCode;
            if (pShaderInfo->pEntryTarget != nullptr)
            {
                uint32_t stageMask = GetStageMaskFromSpirvBinary(pSpirvBin, pShaderInfo->pEntryTarget);

                if ((stageMask & ShaderStageToMask(shaderStage)) == 0)
                {
                    LLPC_ERRS("Fail to find entry-point " << pShaderInfo->pEntryTarget << " for " <<
                              GetShaderStageName(shaderStage) << " shader\n");
                    result = Result::ErrorInvalidShader;
                }
            }
            else
            {
                LLPC_ERRS("Missing entry-point name for " << GetShaderStageName(shaderStage) << " shader\n");
                result = Result::ErrorInvalidShader;
            }
        }
        else if (pModuleData->binType == BinaryType::LlvmBc)
        {
            // Do nothing if input is LLVM IR
        }
        else
        {
            LLPC_ERRS("Invalid shader binary type for " << GetShaderStageName(shaderStage) << " shader\n");
            result = Result::ErrorInvalidShader;
        }
    }

    return result;
}

// =====================================================================================================================
// Builds LLVM module for null fragment shader.
Result Compiler::BuildNullFs(
    Context*                 pContext,     // [in] LLPC context
    std::unique_ptr<Module>& pNullFsModule // [out] Null fragment shader module
    ) const
{
    Result result = Result::Success;

    auto pMemBuffer = MemoryBuffer::getMemBuffer(StringRef(reinterpret_cast<const char*>(&GlslNullFsEmuLib[0]),
                                                 sizeof(GlslNullFsEmuLib)),
                                                 "",
                                                 false);

    Expected<std::unique_ptr<Module>> moduleOrErr = getLazyBitcodeModule(pMemBuffer->getMemBufferRef(), *pContext);
    if (!moduleOrErr)
    {
        Error error = moduleOrErr.takeError();
        LLPC_ERRS("Fails to load LLVM bitcode (null fragment shader)\n");
        result = Result::ErrorInvalidShader;
    }
    else
    {
        if (llvm::Error errCode = (*moduleOrErr)->materializeAll())
        {
            LLPC_ERRS("Fails to materialize (null fragment shader)\n");
            result = Result::ErrorInvalidShader;
        }
    }

    if (result == Result::Success)
    {
        pNullFsModule = std::move(*moduleOrErr);
        pContext->SetModuleTargetMachine(pNullFsModule.get());

        GraphicsContext* pGraphicsContext = static_cast<GraphicsContext*>(pContext->GetPipelineContext());
        pGraphicsContext->InitShaderInfoForNullFs();
    }

    return result;
}

// =====================================================================================================================
// Builds LLVM module for copy shader and generates GPU ISA codes accordingly.
Result Compiler::BuildCopyShader(
    Context*         pContext,            // [in] LLPC context
    Module**         ppCopyShaderModule   // [out] LLVM module for copy shader
    ) const
{
    CopyShader copyShader(pContext);
    return copyShader.Run(ppCopyShaderModule);
}

// =====================================================================================================================
// Creates shader cache object with the requested properties.
Result Compiler::CreateShaderCache(
    const ShaderCacheCreateInfo* pCreateInfo,    // [in] Shader cache create info
    IShaderCache**               ppShaderCache)  // [out] Shader cache object
{
    Result result = Result::Success;

    ShaderCacheAuxCreateInfo auxCreateInfo = {};
    auxCreateInfo.shaderCacheMode = ShaderCacheMode::ShaderCacheEnableRuntime;
    auxCreateInfo.gfxIp           = m_gfxIp;
    auxCreateInfo.hash            = m_optionHash;

    ShaderCache* pShaderCache = new ShaderCache();

    if (pShaderCache != nullptr)
    {
        result = pShaderCache->Init(pCreateInfo, &auxCreateInfo);
        if (result != Result::Success)
        {
            pShaderCache->Destroy();
            pShaderCache = nullptr;
        }
    }
    else
    {
        result = Result::ErrorOutOfMemory;
    }

    *ppShaderCache = pShaderCache;
    return result;
}

// =====================================================================================================================
// Initialize GPU property.
void Compiler::InitGpuProperty()
{
    // Initial settings (could be adjusted later according to graphics IP version info)
    memset(&m_gpuProperty, 0, sizeof(m_gpuProperty));
    m_gpuProperty.waveSize = 64;

    m_gpuProperty.ldsSizePerCu = (m_gfxIp.major > 6) ? 65536 : 32768;
    m_gpuProperty.ldsSizePerThreadGroup = 32 * 1024;
    m_gpuProperty.numShaderEngines = 4;
    m_gpuProperty.maxSgprsAvailable = 104;
    m_gpuProperty.maxVgprsAvailable = 256;

    //TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
    m_gpuProperty.gsPrimBufferDepth = 0x100;

    m_gpuProperty.maxUserDataCount = (m_gfxIp.major >= 9) ? 32 : 16;

    m_gpuProperty.gsOnChipMaxLdsSize = 16384;

    m_gpuProperty.tessOffChipLdsBufferSize = 32768;

    // TODO: Accept gsOnChipDefaultPrimsPerSubgroup from panel option
    m_gpuProperty.gsOnChipDefaultPrimsPerSubgroup   = 64;

    if (m_gfxIp.major <= 6)
    {
        m_gpuProperty.ldsSizeDwordGranularityShift = 6;
    }
    else
    {
        m_gpuProperty.ldsSizeDwordGranularityShift = 7;
    }

    if (m_gfxIp.major <= 8)
    {
        // TODO: Accept gsOnChipDefaultLdsSizePerSubgroup from panel option
        m_gpuProperty.gsOnChipDefaultLdsSizePerSubgroup = 8192;
    }

    if (m_gfxIp.major == 6)
    {
        m_gpuProperty.numShaderEngines = (m_gfxIp.stepping == 0) ? 2 : 1;
    }
    else if (m_gfxIp.major == 7)
    {
        if (m_gfxIp.stepping == 0)
        {
            m_gpuProperty.numShaderEngines = 2;
        }
        else if (m_gfxIp.stepping == 1)
        {
            m_gpuProperty.numShaderEngines = 4;
        }
        else
        {
            m_gpuProperty.numShaderEngines = 1;
        }
    }
    else if (m_gfxIp.major == 8)
    {
        // TODO: polaris11 and polaris12 is 2, but we can't identify them by GFX IP now.
        m_gpuProperty.numShaderEngines = ((m_gfxIp.minor == 1) || (m_gfxIp.stepping <= 1)) ? 1 : 4;
    }
    else if (m_gfxIp.major == 9)
    {
        if (m_gfxIp.stepping == 0)
        {
            m_gpuProperty.numShaderEngines = 4;
        }
    }
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }
}

// =====================================================================================================================
// Initialize GPU workarounds.
void Compiler::InitGpuWorkaround()
{
    memset(&m_gpuWorkarounds, 0, sizeof(m_gpuWorkarounds));
    if (m_gfxIp.major == 6)
    {
        // Hardware workarounds for GFX6 based GPU's:
        m_gpuWorkarounds.gfx6.cbNoLt16BitIntClamp = 1;
        m_gpuWorkarounds.gfx6.miscLoadBalancePerWatt = 1;
        m_gpuWorkarounds.gfx6.shader8b16bLocalWriteCorruption = 1;

        // TODO: Should be done in back-end, see SCOption_R1000_READLANE_SMRD_WORKAROUND_BUG343479 for detail
        m_gpuWorkarounds.gfx6.shaderReadlaneSmrd = 1;

        // TODO: Should be done in back-end, see SCOption_UBTS460287_FILL_SIMD_WITH_VGPRS for detail
        m_gpuWorkarounds.gfx6.shaderSpiCsRegAllocFragmentation = 1;

        // TODO: Should be done in back-end, see SCOption_R1000R1100_VCCZ_CLOBBER_WORKAROUND_BUG457939 for detail
        m_gpuWorkarounds.gfx6.shaderVcczScalarReadBranchFailure = 1;

        // TODO: Should be done in back-end, see SCOption_MIN_MAX_DENORM_WORKAROUND for detail
        m_gpuWorkarounds.gfx6.shaderMinMaxFlushDenorm = 1;

        // NOTE: We only need workaround it in Tahiti, Pitcairn, Capeverde, to simplify the design, we set this
        // flag for all gfxIp.major == 6
        m_gpuWorkarounds.gfx6.shaderZExport = 1;

    }
    else if (m_gfxIp.major == 7)
    {
        // Hardware workarounds for GFX7 based GPU's:
        m_gpuWorkarounds.gfx6.shaderVcczScalarReadBranchFailure = 1;
        m_gpuWorkarounds.gfx6.shaderMinMaxFlushDenorm = 1;

        if (m_gfxIp.stepping == 0)
        {
            m_gpuWorkarounds.gfx6.cbNoLt16BitIntClamp = 1;

            // NOTE: Buffer store + index mode are not used in vulkan, so we can skip this workaround in safe.
            m_gpuWorkarounds.gfx6.shaderCoalesceStore = 1;
        }
        if (m_gfxIp.stepping == 3 ||
            m_gfxIp.stepping == 4)
        {
            m_gpuWorkarounds.gfx6.cbNoLt16BitIntClamp = 1;
            m_gpuWorkarounds.gfx6.shaderCoalesceStore = 1;
            m_gpuWorkarounds.gfx6.shaderSpiBarrierMgmt = 1;
            m_gpuWorkarounds.gfx6.shaderSpiCsRegAllocFragmentation = 1;
        }
    }
    else if (m_gfxIp.major == 8)
    {
        // Hardware workarounds for GFX8.x based GPU's:
        m_gpuWorkarounds.gfx6.shaderMinMaxFlushDenorm = 1;

        // TODO: Should be done in back-end, see SCOption_R1200_UBTS523206_SMEM_RANGE_CHECK for detail
        m_gpuWorkarounds.gfx6.shaderSmemBufferAddrClamp = 1;

        // TODO: Should be done in back-end, see SCOption_GFX8_DEGGIGX80_444_IMAGE_GATHER4_WORKAROUND and
        // SCOption_GFX81_DEGGI___IMAGE_STORE_D16_WORKAROUND for detail
        m_gpuWorkarounds.gfx6.shaderEstimateRegisterUsage = 1;

        if (m_gfxIp.minor == 0 && m_gfxIp.stepping == 2)
        {
            m_gpuWorkarounds.gfx6.miscSpiSgprsNum = 1;
        }
    }
    else if (m_gfxIp.major == 9)
    {
        // Hardware workarounds for GFX9 based GPU's:

        // TODO: Clean up code for all 1d texture patch
        m_gpuWorkarounds.gfx9.treat1dImagesAs2d = 1;

        // TODO: Should be done in back-end, see SCOption_GFX9_DEGGIGX90_864_IMAGE_GATHER_H_PCK_WORKAROUND and
        // SCOption_GFX9_DEGGIGX90_1552_IMAGE_GATHER4_WORKAROUND for detail
        m_gpuWorkarounds.gfx9.shaderImageGatherInstFix = 1;

        // TODO: Should be done in back-end, see SCOption_GFX9_DEGGIGX90_1641_CACHE_LINE_STRADDLING_BUG for detail
        m_gpuWorkarounds.gfx9.fixCacheLineStraddling = 1;

        if (m_gfxIp.stepping == 0 || m_gfxIp.stepping == 2)
        {
            m_gpuWorkarounds.gfx9.fixLsVgprInput = 1;
        }
    }
}
// =====================================================================================================================
// Acquires a free context from context pool.
Context* Compiler::AcquireContext()
{
    Context* pFreeContext = nullptr;

    MutexGuard lock(m_contextPoolMutex);

    // Try to find a free context from pool first
    for (auto pContext : m_contextPool)
    {
        GfxIpVersion gfxIpVersion = pContext->GetGfxIpVersion();

        if ((pContext->IsInUse()   == false) &&
            (gfxIpVersion.major    == m_gfxIp.major) &&
            (gfxIpVersion.minor    == m_gfxIp.minor) &&
            (gfxIpVersion.stepping == m_gfxIp.stepping))
        {
            pFreeContext = pContext;
            pFreeContext->SetInUse(true);
            break;
        }
    }

    if (pFreeContext == nullptr)
    {
        // Create a new one if we fail to find an available one
        pFreeContext = new Context(m_gfxIp, &m_gpuWorkarounds);
        pFreeContext->SetInUse(true);
        m_contextPool.push_back(pFreeContext);
    }

    LLPC_ASSERT(pFreeContext != nullptr);
    return pFreeContext;
}

// =====================================================================================================================
// Releases LLPC context.
void Compiler::ReleaseContext(
    Context* pContext)    // [in] LLPC context
{
    MutexGuard lock(m_contextPoolMutex);
    pContext->SetInUse(false);
}

// =====================================================================================================================
// Dumps the result of time profile.
void Compiler::DumpTimeProfilingResult(
    const MetroHash::Hash* pHash)   // [in] Pipeline hash
{
    int64_t freq = {};
    freq = GetPerfFrequency();

    char shaderHash[64] = {};
    snprintf(shaderHash, 64, "0x%016" PRIX64, MetroHash::Compact64(pHash));

    // NOTE: To get correct profile result, we have to disable general info output, so we have to output time profile
    // result to LLPC_ERRS
    LLPC_ERRS("Time Profiling Results(General): "
              << "Hash = " << shaderHash << ", "
              << "Translate = " << float(g_timeProfileResult.translateTime) / freq << ", "
              << "SPIR-V Lower = " << float(g_timeProfileResult.lowerTime) / freq << ", "
              << "LLVM Patch = " << float(g_timeProfileResult.patchTime) / freq << ", "
              << "Code Generation = " << float(g_timeProfileResult.codeGenTime) / freq << "\n");

    LLPC_ERRS("Time Profiling Results(Special): "
              << "SPIR-V Lower (Optimization) = " << float(g_timeProfileResult.lowerOptTime) / freq << ", "
              << "LLVM Patch (Lib Link) = " << float(g_timeProfileResult.patchLinkTime) / freq << "\n");
}

// =====================================================================================================================
// Collect information from SPIR-V binary
Result Compiler::CollectInfoFromSpirvBinary(
    ShaderModuleData* pModuleData   // [in] The shader module data
    ) const
{
    Result result = Result::Success;

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pModuleData->binCode.pCode);
    const uint32_t* pEnd = pCode + pModuleData->binCode.codeSize / sizeof(uint32_t);

    if (IsSpirvBinary(&pModuleData->binCode))
    {
        const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

        // Parse SPIR-V instructions
        std::unordered_set<uint32_t> capabilities;

        bool exit = false;
        while (pCodePos < pEnd)
        {
            uint32_t opCode = (pCodePos[0] & OpCodeMask);
            uint32_t wordCount = (pCodePos[0] >> WordCountShift);

            if ((wordCount == 0) || (pCodePos + wordCount > pEnd))
            {
                LLPC_ERRS("Invalid SPIR-V binary\n");
                result = Result::ErrorInvalidShader;
                break;
            }

            // Parse each instruction and find those we are interested in

            // NOTE: SPIR-V binary has fixed instruction layout. This is stated in the spec "2.4 Logical
            // Layout of a Module". We can simply skip those sections we do not interested in and exit
            // instruction scan early.
            switch (opCode)
            {
            case spv::OpCapability:
                {
                    LLPC_ASSERT(wordCount == 2);
                    auto capability = static_cast<spv::Capability>(pCodePos[1]);
                    capabilities.insert(capability);
                    break;
                }
            default:
                {
                    // Other instructions beyond info-collecting scope, exit
                    exit = true;
                    break;
                }
            }

            if (exit)
            {
                break;
            }
            else
            {
                pCodePos += wordCount;
            }
        }

        if (capabilities.find(spv::CapabilityVariablePointersStorageBuffer) != capabilities.end())
        {
            pModuleData->enableVarPtrStorageBuf = true;
        }

        if (capabilities.find(spv::CapabilityVariablePointers) != capabilities.end())
        {
            pModuleData->enableVarPtr = true;
        }

        if ((capabilities.find(spv::CapabilityGroupNonUniform) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformVote) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformArithmetic) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformBallot) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformShuffle) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformShuffleRelative) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformClustered) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroupNonUniformQuad) != capabilities.end()) ||
            (capabilities.find(spv::CapabilitySubgroupBallotKHR) != capabilities.end()) ||
            (capabilities.find(spv::CapabilitySubgroupVoteKHR) != capabilities.end()) ||
            (capabilities.find(spv::CapabilityGroups) != capabilities.end()))
        {
            pModuleData->useSubgroupSize = true;
        }
    }
    else
    {
        result = Result::ErrorInvalidShader;
        LLPC_ERRS("Invalid SPIR-V binary\n");
    }

    return result;
}

// =====================================================================================================================
// Checks if dynamic loop unroll is needed for this module.
bool Compiler::NeedDynamicLoopUnroll(
    Module* pModule  // [in]  LLVM module to check
    ) const
{
    std::vector<LoopAnalysisInfo>  loopInfo;
    bool needDynamicLoopUnroll = false;
    PassLoopInfoCollect* pLoopPass = new PassLoopInfoCollect(&loopInfo);

    PassManager passMgr;
    passMgr.add(pLoopPass);
    passMgr.run(*pModule);

    for (uint32_t i = 0; i < loopInfo.size(); i++)
    {
        // These conditions mean the loop is a complex loop.
        if ((loopInfo[i].numAluInsts > 20) ||
            (loopInfo[i].nestedLevel >= 3) ||
            (loopInfo[i].numBasicBlocks > 8))
        {
            needDynamicLoopUnroll = true;
            break;
        }
    }

    return needDynamicLoopUnroll;
}

// =====================================================================================================================
// Gets the statistics info for the specified pipeline binary.
void Compiler::GetPipelineStatistics(
    const void*             pCode,                  // [in]  Pipeline ISA code
    size_t                  codeSize,               // Pipeline ISA code size
    GfxIpVersion            gfxIp,                  // Graphics IP version info
    PipelineStatistics*     pPipelineStats          // [out] Output statistics info for the pipeline
    ) const
{
    ElfReader<Elf64> reader(gfxIp);
    auto result = reader.ReadFromBuffer(pCode, &codeSize);
    LLPC_ASSERT(result == Result::Success);
    LLPC_UNUSED(result);

    pPipelineStats->numAvailVgprs       = 0;
    pPipelineStats->numUsedVgprs        = 0;
    pPipelineStats->useScratchBuffer    = false;
    pPipelineStats->sgprSpill           = false;

    uint32_t sectionCount = reader.GetSectionCount();
    for (uint32_t secIdx = 0; secIdx < sectionCount; ++secIdx)
    {
        typename ElfReader<Elf64>::ElfSectionBuffer* pSection = nullptr;
        bool isCompute = false;
        Result result = reader.GetSectionDataBySectionIndex(secIdx, &pSection);
        LLPC_ASSERT(result == Result::Success);
        LLPC_UNUSED(result);

        if (strcmp(pSection->pName, NoteName) == 0)
        {
            uint32_t offset = 0;
            const uint32_t noteHeaderSize = sizeof(NoteHeader);
            while (offset < pSection->secHead.sh_size)
            {
                const NoteHeader* pNode = reinterpret_cast<const NoteHeader*>(pSection->pData + offset);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 432
                if (pNode->type == Util::Abi::PipelineAbiNoteType::LegacyMetadata)
#else
                if (pNode->type == LegacyMetadata)
#endif
                {
                    const uint32_t configCount = pNode->descSize / sizeof(Util::Abi::PalMetadataNoteEntry);
                    auto pConfig = reinterpret_cast<const Util::Abi::PalMetadataNoteEntry*>(
                                   pSection->pData + offset + noteHeaderSize);
                    for (uint32_t i = 0; i < configCount; ++i)
                    {
                        uint32_t regId = pConfig[i].key;
                        switch (regId)
                        {
                        case mmPS_NUM_USED_VGPRS:
                            pPipelineStats->numUsedVgprs= pConfig[i].value;
                            break;
                        case mmCS_NUM_USED_VGPRS:
                            isCompute = true;
                            pPipelineStats->numUsedVgprs = pConfig[i].value;
                            break;
                        case mmPS_SCRATCH_BYTE_SIZE:
                        case mmCS_SCRATCH_BYTE_SIZE:
                            pPipelineStats->useScratchBuffer = (pConfig[i].value > 0);
                            break;
                        case mmPS_NUM_AVAIL_VGPRS:
                        case mmCS_NUM_AVAIL_VGPRS:
                            pPipelineStats->numAvailVgprs = pConfig[i].value;
                        default:
                            break;
                        }
                    }
                }

                offset += noteHeaderSize + Pow2Align(pNode->descSize, sizeof(uint32_t));
                LLPC_ASSERT(offset <= pSection->secHead.sh_size);
            }
        }
        else if (strncmp(pSection->pName, AmdGpuDisasmName, sizeof(AmdGpuDisasmName) - 1) == 0)
        {
            uint32_t startPos = 0;
            uint32_t endPos = static_cast<uint32_t>(pSection->secHead.sh_size);

            uint8_t lastChar = pSection->pData[endPos - 1];
            const_cast<uint8_t*>(pSection->pData)[endPos - 1] = '\0';

            const char* pText = reinterpret_cast<const char*>(pSection->pData + startPos);

            // Search PS or CS segment first
            auto entryStage = Util::Abi::PipelineSymbolType::PsMainEntry;
            if (isCompute)
            {
                entryStage = Util::Abi::PipelineSymbolType::CsMainEntry;
            }

            const char* pEntryName = Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(entryStage)];

            pText = strstr(pText, pEntryName);

            if (pText != nullptr)
            {
                // Search program end marker
                const char* pEndPgm = strstr(pText, "s_endpgm");
                LLPC_ASSERT(pEndPgm);
                char savedChar = *pEndPgm;
                const_cast<char*>(pEndPgm)[0] = '\0';

                // Search writelane instructions, which mean SGPR spill.
                if (strstr(pText, "writelane") != nullptr)
                {
                    pPipelineStats->sgprSpill = true;
                }

                // Restore last character
                const_cast<char*>(pEndPgm)[0] = savedChar;
            }

            // Restore last character
            const_cast<uint8_t*>(pSection->pData)[endPos - 1] = lastChar;
        }
    }
}

// =====================================================================================================================
// Chooses the optimal candidate of loop unroll count with the specified pipeline statistics info
//
// NOTE: Candidates of loop unroll count is assumed to be provide with a descending order. The first candidate has max loop unroll count while the last has the min one.
uint32_t Compiler::ChooseLoopUnrollCountCandidate(
    PipelineStatistics* pPipelineStats,             // [in] Pipeline module info
    uint32_t            candidateCount              // [in] Candidate  Count
    ) const
{
    uint32_t candidateIdx        = 0;
    uint32_t optimalCandidateIdx = 0;
    uint32_t noSpillCandidateIdx = 0;
    uint32_t maxWaveCandidateIdx = 0;

    // Find the optimal candidate with no SGPR spill or scratch buffer.
    for (; candidateIdx < candidateCount; candidateIdx++)
    {
        if ((pPipelineStats[candidateIdx].sgprSpill == false) &&
            (pPipelineStats[candidateIdx].useScratchBuffer == false))
        {
            break;
        }
    }
    noSpillCandidateIdx = candidateIdx;

    // Check available wavefronts
    // Only need to check VGPR usage. There are a lot more SGPRs(800).
    // It will only limit waves to 8, which is not a problem as the designed wave ratio is 0.2
    uint32_t maxWave = 0;
    uint32_t totalVgprs = pPipelineStats[0].numAvailVgprs;
    LLPC_ASSERT(totalVgprs > 0);

    for (candidateIdx = 0; candidateIdx < candidateCount; candidateIdx++)
    {
        uint32_t wave = totalVgprs / pPipelineStats[candidateIdx].numUsedVgprs;
        if (maxWave < wave)
        {
            maxWave = wave;
            maxWaveCandidateIdx = candidateIdx;
        }
    }

    float waveRatio = 0.0f;

    if (noSpillCandidateIdx < candidateCount)
    {
        if (noSpillCandidateIdx < maxWaveCandidateIdx)
        {
            // Choose the optimal candidate with max wave
            optimalCandidateIdx = maxWaveCandidateIdx;
            uint32_t wave = totalVgprs / pPipelineStats[optimalCandidateIdx - 1].numUsedVgprs;

            waveRatio = static_cast<float>(maxWave - wave);
            waveRatio = waveRatio / maxWave;

            // Choose larger loop unroll count if wave difference is relatively small.
            if (waveRatio < 0.2)
            {
                optimalCandidateIdx--;
                int32_t nextIndex = static_cast<int32_t>(optimalCandidateIdx) - 1;
                while (nextIndex > 0)
                {
                    if (wave != (totalVgprs / pPipelineStats[nextIndex].numUsedVgprs))
                    {
                        break;
                    }
                    optimalCandidateIdx--;
                    nextIndex = optimalCandidateIdx - 1;
                }
            }
        }
        else if (noSpillCandidateIdx == maxWaveCandidateIdx)
        {
            optimalCandidateIdx = maxWaveCandidateIdx;
        }
        else
        {
            // Large loop unroll count has more wavefronts.
            // This is not a common case.
            waveRatio = static_cast<float>(maxWave - totalVgprs / pPipelineStats[noSpillCandidateIdx].numUsedVgprs);
            waveRatio = waveRatio / maxWave;
            if (waveRatio < 0.2)
            {
                optimalCandidateIdx = noSpillCandidateIdx;
            }
            else
            {
                optimalCandidateIdx = maxWaveCandidateIdx;
            }
        }
    }
    else
    {
        optimalCandidateIdx = 0;
    }

    return optimalCandidateIdx;
}

} // Llpc
