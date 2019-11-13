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
 * @file  llpcCompiler.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Compiler.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-compiler"

#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"

#include "LLVMSPIRVLib.h"
#include "spirvExt.h"
#include "SPIRVInternal.h"

#include "llpcBuilder.h"
#include "llpcCodeGenManager.h"
#include "llpcCompiler.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "llpcGfx6Chip.h"
#include "llpcGfx9Chip.h"
#include "llpcGraphicsContext.h"
#include "llpcBinaryStream.h"
#include "llpcShaderModuleHelper.h"
#include "llpcElfReader.h"
#include "llpcElfWriter.h"
#include "llpcFile.h"
#include "llpcFragColorExport.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "llpcPipelineDumper.h"
#include "llpcSpirvLower.h"
#include "llpcTimerProfiler.h"
#include "llpcVertexFetch.h"
#include <mutex>
#include <set>
#include <unordered_set>

#ifdef LLPC_ENABLE_SPIRV_OPT
    #define SPVGEN_STATIC_LIB 1
    #include "spvgen.h"
#endif

using namespace llvm;
using namespace MetroHash;
using namespace SPIRV;
using namespace spv;
using namespace Util;

namespace llvm
{

namespace cl
{

// -pipeline-dump-dir: directory where pipeline info are dumped
opt<std::string> PipelineDumpDir("pipeline-dump-dir",
                                 desc("Directory where pipeline shader info are dumped"),
                                 value_desc("dir"),
                                 init("."));

// -enable-pipeline-dump: enable pipeline info dump
opt<bool> EnablePipelineDump("enable-pipeline-dump", desc("Enable pipeline info dump"), init(false));

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

// -enable-spirv-opt: enable optimization for SPIR-V binary
opt<bool> EnableSpirvOpt("enable-spirv-opt", desc("Enable optimization for SPIR-V binary"), init(false));

// -enable-shadow-desc: enable shadow desriptor table
opt<bool> EnableShadowDescriptorTable("enable-shadow-desc", desc("Enable shadow descriptor table"), init(true));

// -shadow-desc-table-ptr-high: high part of VA for shadow descriptor table pointer
opt<uint32_t> ShadowDescTablePtrHigh("shadow-desc-table-ptr-high",
                                     desc("High part of VA for shadow descriptor table pointer"),
                                     init(2));

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 37
// -enable-dynamic-loop-unroll: Enable dynamic loop unroll. (Deprecated)
opt<bool> EnableDynamicLoopUnroll("enable-dynamic-loop-unroll", desc("Enable dynamic loop unroll (deprecated)"), init(false));
#endif

// -force-loop-unroll-count: Force to set the loop unroll count.
opt<int> ForceLoopUnrollCount("force-loop-unroll-count", cl::desc("Force loop unroll count"), init(0));

// -enable-shader-module-opt: Enable translate & lower phase in shader module build.
opt<bool> EnableShaderModuleOpt("enable-shader-module-opt",
                                cl::desc("Enable translate & lower phase in shader module build."),
                                init(false));

// -disable-licm: annotate loops with metadata to disable the LLVM LICM pass
opt<bool> DisableLicm("disable-licm", desc("Disable LLVM LICM pass"), init(false));

#if LLPC_BUILD_GFX10
// -native-wave-size: an option to override hardware native wave size, it will allow compiler to choose
// final wave size base on it. Used in pre-silicon verification.
opt<int> NativeWaveSize("native-wave-size", cl::desc("Overrides hardware native wave size"), init(0));

// -subgroup-size: sub-group size exposed via Vulkan API.
opt<int> SubgroupSize("subgroup-size", cl::desc("Sub-group size exposed via Vulkan API"), init(64));
#endif

// -trim-debug-info: Trim debug information in SPIR-V binary
opt<bool> TrimDebugInfo("trim-debug-info", cl::desc("Trim debug information in SPIR-V binary"), init(true));

// -enable-per-stage-cache: Enable shader cache per shader stage
opt<bool> EnablePerStageCache("enable-per-stage-cache", cl::desc("Enable shader cache per shader stage"), init(true));

extern opt<bool> EnableOuts;

extern opt<bool> EnableErrs;

extern opt<std::string> LogFileDbgs;

extern opt<std::string> LogFileOuts;

} // cl

} // llvm

namespace Llpc
{

llvm::sys::Mutex       Compiler::m_contextPoolMutex;
std::vector<Context*>* Compiler::m_pContextPool = nullptr;

// Enumerates modes used in shader replacement
enum ShaderReplaceMode
{
    ShaderReplaceDisable            = 0, // Disabled
    ShaderReplaceShaderHash         = 1, // Replacement based on shader hash
    ShaderReplaceShaderPipelineHash = 2, // Replacement based on both shader and pipeline hash
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
// Handler for diagnosis in pass run, derived from the standard one.
class LlpcDiagnosticHandler : public llvm::DiagnosticHandler
{
    bool handleDiagnostics(const DiagnosticInfo& diagInfo) override
    {
        if (EnableOuts() || EnableErrs())
        {
            if ((diagInfo.getSeverity() == DS_Error) || (diagInfo.getSeverity() == DS_Warning))
            {
                DiagnosticPrinterRawOStream printStream(outs());
                printStream << "ERROR: LLVM DIAGNOSIS INFO: ";
                diagInfo.print(printStream);
                printStream << "\n";
                outs().flush();
            }
            else if (EnableOuts())
            {
                DiagnosticPrinterRawOStream printStream(outs());
                printStream << "\n\n=====  LLVM DIAGNOSIS START  =====\n\n";
                diagInfo.print(printStream);
                printStream << "\n\n=====  LLVM DIAGNOSIS END  =====\n\n";
                outs().flush();
            }
        }
        LLPC_ASSERT(diagInfo.getSeverity() != DS_Error);
        return true;
    }
};

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

    std::lock_guard<sys::Mutex> lock(*s_compilerMutex);
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
        auto& passRegistry = *PassRegistry::getPassRegistry();

        // Initialize LLVM target: AMDGPU
        LLVMInitializeAMDGPUTargetInfo();
        LLVMInitializeAMDGPUTarget();
        LLVMInitializeAMDGPUTargetMC();
        LLVMInitializeAMDGPUAsmPrinter();
        LLVMInitializeAMDGPUAsmParser();
        LLVMInitializeAMDGPUDisassembler();

        // Initialize special passes which are checked in PassManager
        initializeJumpThreadingPass(passRegistry);
        initializePrintModulePassWrapperPass(passRegistry);

        // Initialize passes so they can be referenced by -llpc-stop-before etc.
        InitializeUtilPasses(passRegistry);
        InitializeLowerPasses(passRegistry);
        InitializeBuilderPasses(passRegistry);
        InitializePatchPasses(passRegistry);

        // LLVM fatal error handler only can be installed once.
        install_fatal_error_handler(FatalErrorHandler);

        // Initiailze m_pContextPool.
        {
            std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);

            m_pContextPool = new std::vector<Context*>();
        }
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
        std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);

        // Keep the max allowed count of contexts that reside in the pool so that we can speed up the creatoin of
        // compiler next time.
        for (auto it = m_pContextPool->begin(); it != m_pContextPool->end();)
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

            if ((pContext->IsInUse() == false) && (m_pContextPool->size() > maxResidentContexts))
            {
                it = m_pContextPool->erase(it);
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
        std::lock_guard<sys::Mutex> lock(*s_compilerMutex);
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
        std::lock_guard<sys::Mutex> lock(*s_compilerMutex);
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
        delete m_pContextPool;
        m_pContextPool = nullptr;
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
    void* pAllocBuf = nullptr;
    const void* pCacheData = nullptr;
    size_t allocSize = 0;
    ShaderModuleDataEx moduleDataEx = {};

    ElfPackage moduleBinary;
    raw_svector_ostream moduleBinaryStream(moduleBinary);
    std::vector<ShaderEntryName> entryNames;
    SmallVector<ShaderModuleEntryData, 4> moduleEntryDatas;
    SmallVector<ShaderModuleEntry, 4> moduleEntries;

    ShaderEntryState cacheEntryState = ShaderEntryState::New;
    CacheEntryHandle hEntry = nullptr;

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 32
    const PipelineOptions* pPipelineOptions = &pShaderInfo->options.pipelineOptions;
#else
    PipelineOptions dummyPipelineOptions = {};
    const PipelineOptions* pPipelineOptions = &dummyPipelineOptions;
#endif
    // Calculate the hash code of input data
    MetroHash::Hash hash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pShaderInfo->shaderBin.pCode),
        pShaderInfo->shaderBin.codeSize,
        hash.bytes);

    memcpy(moduleDataEx.common.hash, &hash, sizeof(hash));

    TimerProfiler timerProfiler(MetroHash::Compact64(&hash),
                                "LLPC ShaderModule",
                                TimerProfiler::ShaderModuleTimerEnableMask);

    // Check the type of input shader binary
    if (ShaderModuleHelper::IsSpirvBinary(&pShaderInfo->shaderBin))
    {
        uint32_t debugInfoSize = 0;

        moduleDataEx.common.binType = BinaryType::Spirv;
        if (ShaderModuleHelper::VerifySpirvBinary(&pShaderInfo->shaderBin) != Result::Success)
        {
            LLPC_ERRS("Unsupported SPIR-V instructions are found!\n");
            result = Result::Unsupported;
        }
        if (result == Result::Success)
        {
            ShaderModuleHelper::CollectInfoFromSpirvBinary(&pShaderInfo->shaderBin, &moduleDataEx.common.usage,
                                                           entryNames, &debugInfoSize);
        }
        moduleDataEx.common.binCode.codeSize = pShaderInfo->shaderBin.codeSize;
        if (cl::TrimDebugInfo)
        {
            moduleDataEx.common.binCode.codeSize -= debugInfoSize;
        }
    }
    else if (ShaderModuleHelper::IsLlvmBitcode(&pShaderInfo->shaderBin))
    {
        moduleDataEx.common.binType = BinaryType::LlvmBc;
        moduleDataEx.common.binCode = pShaderInfo->shaderBin;
    }
    else
    {
        result = Result::ErrorInvalidShader;
    }

    if (moduleDataEx.common.binType == BinaryType::Spirv)
    {
        // Dump SPIRV binary
        if (cl::EnablePipelineDump)
        {
            PipelineDumper::DumpSpirvBinary(cl::PipelineDumpDir.c_str(),
                &pShaderInfo->shaderBin,
                &hash);
        }

        // Trim debug info
        if (cl::TrimDebugInfo)
        {
            uint8_t* pTrimmedCode = new uint8_t[moduleDataEx.common.binCode.codeSize];
            ShaderModuleHelper::TrimSpirvDebugInfo(&pShaderInfo->shaderBin, moduleDataEx.common.binCode.codeSize, pTrimmedCode);
            moduleDataEx.common.binCode.pCode = pTrimmedCode;
        }
        else
        {
            moduleDataEx.common.binCode.pCode = pShaderInfo->shaderBin.pCode;
        }

        // Calculate SPIR-V cache hash
        MetroHash::Hash cacheHash = {};
        MetroHash64::Hash(reinterpret_cast<const uint8_t*>(moduleDataEx.common.binCode.pCode),
            moduleDataEx.common.binCode.codeSize,
            cacheHash.bytes);
        static_assert(sizeof(moduleDataEx.common.cacheHash) == sizeof(cacheHash), "Unexpected value!");
        memcpy(moduleDataEx.common.cacheHash, cacheHash.dwords, sizeof(cacheHash));

        // Do SPIR-V translate & lower if possible
        bool enableOpt = cl::EnableShaderModuleOpt;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 32
        enableOpt = enableOpt || pShaderInfo->options.enableOpt;
#endif
        enableOpt = moduleDataEx.common.usage.useSpecConstant ? false : enableOpt;

        if (enableOpt)
        {
            // Check internal cache for shader module build result
            // NOTE: We should not cache non-opt result, we may compile shader module multiple
            // times in async-compile mode.
            cacheEntryState = m_shaderCache->FindShader(cacheHash, true, &hEntry);
            if (cacheEntryState == ShaderEntryState::Ready)
            {
                result = m_shaderCache->RetrieveShader(hEntry, &pCacheData, &allocSize);
            }
            if (cacheEntryState != ShaderEntryState::Ready)
            {
                Context* pContext = AcquireContext();

                pContext->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>());
                pContext->SetBuilder(Builder::Create(*pContext));
                CodeGenManager::CreateTargetMachine(pContext, pPipelineOptions);

                for (uint32_t i = 0; i < entryNames.size(); ++i)
                {
                    ShaderModuleEntry moduleEntry = {};
                    ShaderModuleEntryData moduleEntryData = {};
                    ResourceUsage resUsage;
                    PipelineContext::InitShaderResourceUsage(entryNames[i].stage, &resUsage);

                    moduleEntryData.pShaderEntry = &moduleEntry;
                    moduleEntryData.stage = entryNames[i].stage;
                    moduleEntry.entryOffset = moduleBinary.size();
                    MetroHash::Hash entryNamehash = {};
                    MetroHash64::Hash(reinterpret_cast<const uint8_t*>(entryNames[i].pName),
                        strlen(entryNames[i].pName),
                        entryNamehash.bytes);
                    memcpy(moduleEntry.entryNameHash, entryNamehash.dwords, sizeof(entryNamehash));

                    // Create empty modules and set target machine in each.
                    Module* pModule = new Module(
                        (Twine("llpc") + GetShaderStageName(static_cast<ShaderStage>(entryNames[i].stage))).str(),
                        *pContext);

                    pContext->SetModuleTargetMachine(pModule);
                    pContext->SetResUsage(&resUsage);

                    uint32_t passIndex = 0;
                    PassManager lowerPassMgr(&passIndex);

                    // Set the shader stage in the Builder.
                    pContext->GetBuilder()->SetShaderStage(static_cast<ShaderStage>(entryNames[i].stage));

                    // Start timer for translate.
                    timerProfiler.AddTimerStartStopPass(&lowerPassMgr, TimerTranslate, true);

                    // SPIR-V translation, then dump the result.
                    PipelineShaderInfo shaderInfo = {};
                    shaderInfo.pModuleData = &moduleDataEx.common;
                    shaderInfo.entryStage = entryNames[i].stage;
                    shaderInfo.pEntryTarget = entryNames[i].pName;
                    lowerPassMgr.add(CreateSpirvLowerTranslator(static_cast<ShaderStage>(entryNames[i].stage),
                                                                &shaderInfo));
                    lowerPassMgr.add(CreateSpirvLowerResourceCollect());
                    if (EnableOuts())
                    {
                        lowerPassMgr.add(createPrintModulePass(outs(), "\n"
                            "===============================================================================\n"
                            "// LLPC SPIRV-to-LLVM translation results\n"));
                    }

                    // Stop timer for translate.
                    timerProfiler.AddTimerStartStopPass(&lowerPassMgr, TimerTranslate, false);

                    // Per-shader SPIR-V lowering passes.
                    SpirvLower::AddPasses(pContext,
                                          static_cast<ShaderStage>(entryNames[i].stage),
                                          lowerPassMgr,
                                          timerProfiler.GetTimer(TimerLower),
                                          cl::ForceLoopUnrollCount);

                    lowerPassMgr.add(createBitcodeWriterPass(moduleBinaryStream));

                    // Run the passes.
                    bool success = RunPasses(&lowerPassMgr, pModule);
                    if (success == false)
                    {
                        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
                        result = Result::ErrorInvalidShader;
                        delete pModule;
                        break;
                    }

                    moduleEntry.entrySize = moduleBinary.size() - moduleEntry.entryOffset;

                    // Serialize resource usage
                    moduleBinaryStream << *pContext->GetShaderResourceUsage(entryNames[i].stage);

                    moduleEntry.resUsageSize = moduleBinary.size() - moduleEntry.entryOffset - moduleEntry.entrySize;
                    moduleEntry.passIndex = passIndex;
                    moduleEntries.push_back(moduleEntry);
                    moduleEntryDatas.push_back(moduleEntryData);
                    delete pModule;
                }

                if (result == Result::Success)
                {
                    moduleDataEx.common.binType = BinaryType::MultiLlvmBc;
                    moduleDataEx.common.binCode.pCode = moduleBinary.data();
                    moduleDataEx.common.binCode.codeSize = moduleBinary.size();
                }

                pContext->setDiagnosticHandlerCallBack(nullptr);
            }
            moduleDataEx.extra.entryCount = entryNames.size();
        }
    }

    // Allocate memory and copy output data
    if (result == Result::Success)
    {
        if (pShaderInfo->pfnOutputAlloc != nullptr)
        {
            if (cacheEntryState != ShaderEntryState::Ready)
            {
                allocSize = sizeof(ShaderModuleDataEx) +
                    moduleDataEx.common.binCode.codeSize +
                    (moduleDataEx.extra.entryCount * (sizeof(ShaderModuleEntryData) + sizeof(ShaderModuleEntry)));
            }

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
        // Memory layout of pAllocBuf: ShaderModuleDataEx | ShaderModuleEntryData | ShaderModuleEntry | binCode.
        ShaderModuleDataEx* pModuleDataEx = reinterpret_cast<ShaderModuleDataEx*>(pAllocBuf);

        ShaderModuleEntryData* pEntryData = &pModuleDataEx->extra.entryDatas[0];
        size_t entryOffset = sizeof(ShaderModuleDataEx) +
                                 moduleDataEx.extra.entryCount * sizeof(ShaderModuleEntryData);
        ShaderModuleEntry* pEntry = reinterpret_cast<ShaderModuleEntry*>(VoidPtrInc(pAllocBuf, entryOffset));
        size_t codeOffset = entryOffset + moduleDataEx.extra.entryCount * sizeof(ShaderModuleEntry);

        if (cacheEntryState != ShaderEntryState::Ready)
        {
            // Copy module data
            memcpy(pModuleDataEx, &moduleDataEx, sizeof(moduleDataEx));
            pModuleDataEx->common.binCode.pCode = nullptr;

            // Copy entry info
            for (uint32_t i = 0; i < moduleDataEx.extra.entryCount; ++i)
            {
                pEntryData[i] = moduleEntryDatas[i];
                // Set module entry pointer
                pEntryData[i].pShaderEntry = &pEntry[i];
                // Copy module entry
                memcpy(pEntryData[i].pShaderEntry, &moduleEntries[i], sizeof(ShaderModuleEntry));
            }

            // Copy binary code
            void* pCode = VoidPtrInc(pAllocBuf, codeOffset);
            memcpy(pCode, moduleDataEx.common.binCode.pCode, moduleDataEx.common.binCode.codeSize);
            if (cacheEntryState == ShaderEntryState::Compiling)
            {
                if (hEntry != nullptr)
                {
                    m_shaderCache->InsertShader(hEntry, pModuleDataEx, allocSize);
                }
            }
        }
        else
        {
            memcpy(pModuleDataEx, pCacheData, allocSize);
        }

        // Update the pointers
        for (uint32_t i = 0; i < moduleDataEx.extra.entryCount; ++i)
        {
            pEntryData[i].pShaderEntry = &pEntry[i];
        }
        pModuleDataEx->common.binCode.pCode = VoidPtrInc(pAllocBuf, codeOffset);
        pShaderOut->pModuleData = &pModuleDataEx->common;
    }
    else
    {
        if (hEntry != nullptr)
        {
            m_shaderCache->ResetShader(hEntry);
        }
    }

    return result;
}

// =====================================================================================================================
// Build pipeline internally -- common code for graphics and compute
Result Compiler::BuildPipelineInternal(
    Context*                            pContext,                   // [in] Acquired context
    ArrayRef<const PipelineShaderInfo*> shaderInfo,                 // [in] Shader info of this pipeline
    uint32_t                            forceLoopUnrollCount,       // [in] Force loop unroll count (0 means disable)
    ElfPackage*                         pPipelineElf)               // [out] Output Elf package
{
    Result          result = Result::Success;

    uint32_t passIndex = 0;
    TimerProfiler timerProfiler(pContext->GetPiplineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);

    pContext->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>());

    // Create the AMDGPU TargetMachine.
    result = CodeGenManager::CreateTargetMachine(pContext, pContext->GetPipelineContext()->GetPipelineOptions());

    Module* pPipelineModule = nullptr;

    // NOTE: If input is LLVM IR, read it now. There is now only ever one IR module representing the
    // whole pipeline.
    bool IsLlvmBc = false;
    const PipelineShaderInfo* pShaderInfo = (shaderInfo[0] != nullptr) ? shaderInfo[0] : shaderInfo.back();
    if (pShaderInfo != nullptr)
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
        if ((pModuleData != nullptr) && (pModuleData->binType == BinaryType::LlvmBc))
        {
            IsLlvmBc = true;
            pPipelineModule = pContext->LoadLibary(&pModuleData->binCode).release();
        }
    }

    // Merge user data for shader stages into one.
    pContext->GetPipelineContext()->DoUserDataNodeMerge();

    // If not IR input, run the per-shader passes, including SPIR-V translation, and then link the modules
    // into a single pipeline module.
    if (pPipelineModule == nullptr)
    {
        // Create empty modules and set target machine in each.
        std::vector<Module*> modules(shaderInfo.size());
        uint32_t stageSkipMask = 0;
        for (uint32_t shaderIndex = 0; (shaderIndex < shaderInfo.size()) && (result == Result::Success); ++shaderIndex)
        {
            const PipelineShaderInfo* pShaderInfo = shaderInfo[shaderIndex];
            if ((pShaderInfo == nullptr) || (pShaderInfo->pModuleData == nullptr))
            {
                continue;
            }

            const ShaderModuleDataEx* pModuleDataEx =
                reinterpret_cast<const ShaderModuleDataEx*>(pShaderInfo->pModuleData);

            Module* pModule = nullptr;
            if (pModuleDataEx->common.binType == BinaryType::MultiLlvmBc)
            {
                timerProfiler.StartStopTimer(TimerLoadBc, true);

                MetroHash::Hash entryNameHash = {};

                LLPC_ASSERT(pShaderInfo->pEntryTarget != nullptr);
                MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pShaderInfo->pEntryTarget),
                                  strlen(pShaderInfo->pEntryTarget),
                                  entryNameHash.bytes);

                BinaryData binCode = {};
                for (uint32_t i = 0; i < pModuleDataEx->extra.entryCount; ++i)
                {
                    auto pEntryData = &pModuleDataEx->extra.entryDatas[i];
                    auto pShaderEntry = reinterpret_cast<ShaderModuleEntry*>(pEntryData->pShaderEntry);
                    if ((pEntryData->stage == pShaderInfo->entryStage) &&
                        (memcmp(pShaderEntry->entryNameHash, &entryNameHash, sizeof(MetroHash::Hash)) == 0))
                    {
                        // LLVM bitcode
                        binCode.codeSize = pShaderEntry->entrySize;
                        binCode.pCode = VoidPtrInc(pModuleDataEx->common.binCode.pCode, pShaderEntry->entryOffset);

                        // Resource usage
                        const char* pResUsagePtr = reinterpret_cast<const char*>(
                            VoidPtrInc(pModuleDataEx->common.binCode.pCode,
                                       pShaderEntry->entryOffset + pShaderEntry->entrySize));
                        std::string resUsageBuf(pResUsagePtr, pShaderEntry->resUsageSize);
                        std::istringstream resUsageStrem(resUsageBuf);
                        resUsageStrem >> *(pContext->GetShaderResourceUsage(static_cast<ShaderStage>(shaderIndex)));
                        break;
                    }
                }

                if (binCode.codeSize > 0)
                {
                    pModule = pContext->LoadLibary(&binCode).release();
                    stageSkipMask |= (1 << shaderIndex);
                }
                else
                {
                    result = Result::ErrorInvalidShader;
                }

                 timerProfiler.StartStopTimer(TimerLoadBc, false);
            }
            else
            {
                pModule = new Module((Twine("llpc") + GetShaderStageName(pShaderInfo->entryStage)).str() +
                                     std::to_string(GetModuleIdByIndex(shaderIndex)), *pContext);
            }

            modules[shaderIndex] = pModule;
            pContext->SetModuleTargetMachine(pModule);
        }

        // Give the pipeline state to the Builder. (If we know we are using BuilderRecorder, in a future change
        // we could choose to delay this until after linking into a pipeline module.)
        pContext->GetPipelineContext()->SetBuilderPipelineState(pContext->GetBuilder());

        for (uint32_t shaderIndex = 0; (shaderIndex < shaderInfo.size()) && (result == Result::Success); ++shaderIndex)
        {
            const PipelineShaderInfo* pShaderInfo = shaderInfo[shaderIndex];
            if ((pShaderInfo == nullptr) ||
                (pShaderInfo->pModuleData == nullptr) ||
                (stageSkipMask & ShaderStageToMask(pShaderInfo->entryStage)))
            {
                continue;
            }

            PassManager lowerPassMgr(&passIndex);

            // Set the shader stage in the Builder.
            pContext->GetBuilder()->SetShaderStage(pShaderInfo->entryStage);

            // Start timer for translate.
            timerProfiler.AddTimerStartStopPass(&lowerPassMgr, TimerTranslate, true);

            // SPIR-V translation, then dump the result.
            lowerPassMgr.add(CreateSpirvLowerTranslator(pShaderInfo->entryStage, pShaderInfo));
            if (EnableOuts())
            {
                lowerPassMgr.add(createPrintModulePass(outs(), "\n"
                            "===============================================================================\n"
                            "// LLPC SPIRV-to-LLVM translation results\n"));
            }
            {
                lowerPassMgr.add(CreateSpirvLowerResourceCollect());
            }

            // Stop timer for translate.
            timerProfiler.AddTimerStartStopPass(&lowerPassMgr, TimerTranslate, false);

            // Run the passes.
            bool success = RunPasses(&lowerPassMgr, modules[shaderIndex]);
            if (success == false)
            {
                LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
                result = Result::ErrorInvalidShader;
            }
        }
        for (uint32_t shaderIndex = 0; (shaderIndex < shaderInfo.size()) && (result == Result::Success); ++shaderIndex)
        {
            // Per-shader SPIR-V lowering passes.
            const PipelineShaderInfo* pShaderInfo = shaderInfo[shaderIndex];
            if ((pShaderInfo == nullptr) ||
                (pShaderInfo->pModuleData == nullptr) ||
                (stageSkipMask & ShaderStageToMask(pShaderInfo->entryStage)))
            {
                continue;
            }

            pContext->GetBuilder()->SetShaderStage(pShaderInfo->entryStage);
            PassManager lowerPassMgr(&passIndex);

            SpirvLower::AddPasses(pContext,
                                  pShaderInfo->entryStage,
                                  lowerPassMgr,
                                  timerProfiler.GetTimer(TimerLower),
                                  forceLoopUnrollCount);
            // Run the passes.
            bool success = RunPasses(&lowerPassMgr, modules[shaderIndex]);
            if (success == false)
            {
                LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
                result = Result::ErrorInvalidShader;
            }
        }

        // Link the shader modules into a single pipeline module.
        pPipelineModule = pContext->GetBuilder()->Link(modules, true);
        if (pPipelineModule == nullptr)
        {
            LLPC_ERRS("Failed to link shader modules into pipeline module\n");
            result = Result::ErrorInvalidShader;
        }
    }

    // Set up function to check shader cache.
    GraphicsShaderCacheChecker graphicsShaderCacheChecker(this, pContext);

    std::function<uint32_t(const Module*, uint32_t, ArrayRef<ArrayRef<uint8_t>>)> checkShaderCacheFunc =
            [&graphicsShaderCacheChecker](
        const Module*               pModule,      // [in] Module
        uint32_t                    stageMask,    // Shader stage mask
        ArrayRef<ArrayRef<uint8_t>> stageHashes)  // Per-stage hash of in/out usage
    {
        return graphicsShaderCacheChecker.Check(pModule, stageMask, stageHashes);
    };

    // Only enable per stage cache for full graphic pipeline
    bool checkPerStageCache = cl::EnablePerStageCache && pContext->IsGraphics() &&
                              (pContext->GetShaderStageMask() &
                               (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageFragment)));
    if (checkPerStageCache == false)
    {
        checkShaderCacheFunc = nullptr;
    }

    {
        // Set up "whole pipeline" passes, where we have a single module representing the whole pipeline.
        //
        // TODO: The "whole pipeline" passes are supposed to include code generation passes. However, there is a CTS issue.
        // In the case "dEQP-VK.spirv_assembly.instruction.graphics.16bit_storage.struct_mixed_types.uniform_geom", GS gets
        // unrolled to such a size that backend compilation takes too long. Thus, we put code generation in its own pass
        // manager.
        PassManager patchPassMgr(&passIndex);
        patchPassMgr.add(createTargetTransformInfoWrapperPass(pContext->GetTargetMachine()->getTargetIRAnalysis()));

        // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
        AddTargetLibInfo(pContext, &patchPassMgr);

        raw_svector_ostream elfStream(*pPipelineElf);

        if (result == Result::Success)
        {
            // Patching.
            Patch::AddPasses(pContext,
                             patchPassMgr,
                             timerProfiler.GetTimer(TimerPatch),
                             timerProfiler.GetTimer(TimerOpt),
                             checkShaderCacheFunc);
        }

        // At this point, we have finished with the Builder. No patch pass should be using Builder.
        delete pContext->GetBuilder();
        pContext->SetBuilder(nullptr);

        // Run the "whole pipeline" passes, excluding the target backend.
        if (result == Result::Success)
        {
            bool success = RunPasses(&patchPassMgr, pPipelineModule);
            if (success)
            {
#if LLPC_BUILD_GFX10
                // NOTE: Ideally, target feature setup should be added to the last pass in patching. But NGG is somewhat
                // different in that it must involve extra LLVM optimization passes after preparing pipeline ABI. Thus,
                // we do target feature setup here.
#endif
                CodeGenManager::SetupTargetFeatures(pPipelineModule);
            }
            else
            {
                LLPC_ERRS("Fails to run whole pipeline passes\n");
                result = Result::ErrorInvalidShader;
            }
        }

        // A separate "whole pipeline" pass manager for code generation.
        PassManager codeGenPassMgr(&passIndex);

        if (result == Result::Success)
        {
            // Code generation.
            result = CodeGenManager::AddTargetPasses(pContext,
                                                     codeGenPassMgr,
                                                     timerProfiler.GetTimer(TimerCodeGen),
                                                     elfStream);
        }

        // Run the target backend codegen passes.
        if (result == Result::Success)
        {
            bool success = RunPasses(&codeGenPassMgr, pPipelineModule);
            if (success == false)
            {
                LLPC_ERRS("Fails to generate GPU ISA codes\n");
                result = Result::ErrorInvalidShader;
            }
        }
    }

    if (checkPerStageCache)
    {
        // For graphics, update shader caches with results of compile, and merge ELF outputs if necessary.
        graphicsShaderCacheChecker.UpdateAndMerge(result, pPipelineElf);
    }

    pContext->setDiagnosticHandlerCallBack(nullptr);

    delete pPipelineModule;
    pPipelineModule = nullptr;

    return result;
}

// =====================================================================================================================
// Check shader cache for graphics pipeline, returning mask of which shader stages we want to keep in this compile.
// This is called from the PatchCheckShaderCache pass (via a lambda in BuildPipelineInternal), to remove
// shader stages that we don't want because there was a shader cache hit.
uint32_t GraphicsShaderCacheChecker::Check(
    const Module*               pModule,      // [in] Module
    uint32_t                    stageMask,    // Shader stage mask
    ArrayRef<ArrayRef<uint8_t>> stageHashes)  // Per-stage hash of in/out usage
{
    // Check per stage shader cache
    MetroHash::Hash fragmentHash = {};
    MetroHash::Hash nonFragmentHash = {};
    Compiler::BuildShaderCacheHash(m_pContext, stageMask, stageHashes, &fragmentHash, &nonFragmentHash);

    // NOTE: Global constant are added to the end of pipeline binary. we can't merge ELF binaries if global constant
    // is used in non-fragment shader stages.
    for (auto& global : pModule->globals())
    {
        if (auto pGlobalVar = dyn_cast<GlobalVariable>(&global))
        {
            if (pGlobalVar->isConstant())
            {
                SmallVector<const Value*, 4> vals;
                vals.push_back(pGlobalVar);
                for (uint32_t i = 0; i != vals.size(); ++i)
                {
                    for (auto pUser : vals[i]->users())
                    {
                        if (isa<Constant>(pUser))
                        {
                            vals.push_back(pUser);
                            continue;
                        }
                        if (GetShaderStageFromFunction(cast<Instruction>(pUser)->getFunction()) != ShaderStageFragment)
                        {
                            return stageMask;
                        }
                    }
                }
            }
        }
    }

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo());
#endif
    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        m_fragmentCacheEntryState = m_pCompiler->LookUpShaderCaches(pPipelineInfo->pShaderCache,
                                                                    &fragmentHash,
                                                                    &m_fragmentElf,
                                                                    m_pFragmentShaderCache,
                                                                    m_hFragmentEntry);
#else
        m_fragmentCacheEntryState = m_pCompiler->LookUpShaderCache(&fragmentHash,
                                                                   &m_fragmentElf,
                                                                   &m_hFragmentEntry);
#endif
    }

    if (stageMask & ~ShaderStageToMask(ShaderStageFragment))
    {
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        m_nonFragmentCacheEntryState = m_pCompiler->LookUpShaderCaches(pPipelineInfo->pShaderCache,
                                                                       &nonFragmentHash,
                                                                       &m_nonFragmentElf,
                                                                       m_pNonFragmentShaderCache,
                                                                       m_hNonFragmentEntry);
#else
        m_nonFragmentCacheEntryState = m_pCompiler->LookUpShaderCache(&nonFragmentHash,
                                                                      &m_nonFragmentElf,
                                                                      &m_hNonFragmentEntry);
#endif
    }

    if (m_nonFragmentCacheEntryState != ShaderEntryState::Compiling)
    {
        // Remove non-fragment shader stages.
        stageMask &= ShaderStageToMask(ShaderStageFragment);
    }
    if (m_fragmentCacheEntryState != ShaderEntryState::Compiling)
    {
        // Remove fragment shader stages.
        stageMask &= ~ShaderStageToMask(ShaderStageFragment);
    }

    return stageMask;
}

// =====================================================================================================================
// Update shader caches for graphics pipeline from compile result, and merge ELF outputs if necessary.
void GraphicsShaderCacheChecker::UpdateAndMerge(
    Result            result,         // Result of compile
    ElfPackage*       pPipelineElf)   // ELF output of compile, updated to merge ELF from shader cache
{
    // Only non-fragment shaders were compiled
    if ((m_fragmentCacheEntryState == ShaderEntryState::Ready) &&
        (m_nonFragmentCacheEntryState == ShaderEntryState::Compiling))
    {
        ElfPackage partialPipelineElf = *pPipelineElf;
        pPipelineElf->clear();
        BinaryData pipelineElf = {};
        if (result == Result::Success)
        {
            BinaryData nonFragmentPipelineElf = {};
            nonFragmentPipelineElf.pCode = partialPipelineElf.data();
            nonFragmentPipelineElf.codeSize = partialPipelineElf.size();

            ElfWriter<Elf64> writer(m_pContext->GetGfxIpVersion());
            // Load ELF binary
            auto result = writer.ReadFromBuffer(nonFragmentPipelineElf.pCode, nonFragmentPipelineElf.codeSize);
            LLPC_ASSERT(result == Result::Success);
            LLPC_UNUSED(result);
            writer.MergeElfBinary(m_pContext, &m_fragmentElf, pPipelineElf);

            pipelineElf.codeSize = pPipelineElf->size();
            pipelineElf.pCode = pPipelineElf->data();
        }

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        Compiler::UpdateShaderCaches(result == Result::Success,
                                     &pipelineElf,
                                     m_pNonFragmentShaderCache,
                                     m_hNonFragmentEntry,
                                     ShaderCacheCount);
#else
        m_pCompiler->UpdateShaderCache(result == Result::Success, &pipelineElf, m_hNonFragmentEntry);
#endif
    }

    // Only fragment shader is compiled
    else if ((m_nonFragmentCacheEntryState == ShaderEntryState::Ready) &&
             (m_fragmentCacheEntryState == ShaderEntryState::Compiling))
    {
        ElfPackage partialPipelineElf = *pPipelineElf;
        pPipelineElf->clear();
        BinaryData pipelineElf = {};
        if (result == Result::Success)
        {
            BinaryData fragmentPipelineElf = {};
            fragmentPipelineElf.pCode = partialPipelineElf.data();
            fragmentPipelineElf.codeSize = partialPipelineElf.size();

            ElfWriter<Elf64> writer(m_pContext->GetGfxIpVersion());
            // Load ELF binary
            auto result = writer.ReadFromBuffer(m_nonFragmentElf.pCode, m_nonFragmentElf.codeSize);
            LLPC_ASSERT(result == Result::Success);
            LLPC_UNUSED(result);

            writer.MergeElfBinary(m_pContext, &fragmentPipelineElf, pPipelineElf);

            pipelineElf.codeSize = pPipelineElf->size();
            pipelineElf.pCode = pPipelineElf->data();
        }

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        Compiler::UpdateShaderCaches(result == Result::Success,
                                     &pipelineElf,
                                     m_pFragmentShaderCache,
                                     m_hFragmentEntry,
                                     ShaderCacheCount);
#else
        m_pCompiler->UpdateShaderCache(result == Result::Success, &pipelineElf, m_hFragmentEntry);
#endif
    }

    // Both shaders hit the shader cache.
    else if ((m_fragmentCacheEntryState == ShaderEntryState::Ready) &&
        (m_nonFragmentCacheEntryState == ShaderEntryState::Ready))
    {
        ElfWriter<Elf64> writer(m_pContext->GetGfxIpVersion());
        // Load ELF binary
        auto result = writer.ReadFromBuffer(m_nonFragmentElf.pCode, m_nonFragmentElf.codeSize);
        LLPC_ASSERT(result == Result::Success);
        LLPC_UNUSED(result);
        writer.MergeElfBinary(m_pContext, &m_fragmentElf, pPipelineElf);
    }

    // Whole pipeline is compiled
    else
    {
        BinaryData pipelineElf = {};
        pipelineElf.codeSize = pPipelineElf->size();
        pipelineElf.pCode = pPipelineElf->data();
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        Compiler::UpdateShaderCaches((result == Result::Success),
                                     &pipelineElf,
                                     m_pFragmentShaderCache,
                                     m_hFragmentEntry,
                                     ShaderCacheCount);
        Compiler::UpdateShaderCaches(result == Result::Success,
                                     &pipelineElf,
                                     m_pNonFragmentShaderCache,
                                     m_hNonFragmentEntry,
                                     ShaderCacheCount);
#else
        m_pCompiler->UpdateShaderCache(result == Result::Success, &pipelineElf, m_hFragmentEntry);
        m_pCompiler->UpdateShaderCache(result == Result::Success, &pipelineElf, m_hNonFragmentEntry);
#endif
    }
}

// =====================================================================================================================
// convert color buffer format to fragment shader export format
uint32_t Compiler::ConvertColorBufferFormatToExportFormat(
    const ColorTarget*          pTarget,                // [in] GraphicsPipelineBuildInfo
    const bool                  enableAlphaToCoverage   // whether enalbe AlphaToCoverage
    ) const
{
    ExportFormat exportFormat = FragColorExport::ConvertColorBufferFormatToExportFormat(
            pTarget,
            m_gfxIp,
            &m_gpuWorkarounds,
            pTarget->channelWriteMask,
            enableAlphaToCoverage);

    return static_cast<uint32_t>(exportFormat);
}

// =====================================================================================================================
// Build graphics pipeline internally
Result Compiler::BuildGraphicsPipelineInternal(
    GraphicsContext*                    pGraphicsContext,           // [in] Graphics context this graphics pipeline
    ArrayRef<const PipelineShaderInfo*> shaderInfo,                 // Shader info of this graphics pipeline
    uint32_t                            forceLoopUnrollCount,       // [in] Force loop unroll count (0 means disable)
    ElfPackage*                         pPipelineElf)               // [out] Output Elf package
{
    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pGraphicsContext);
    pContext->SetBuilder(Builder::Create(*pContext));

    Result result = BuildPipelineInternal(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf);

    delete pContext->GetBuilder();
    pContext->SetBuilder(nullptr);
    ReleaseContext(pContext);
    return result;
}

// =====================================================================================================================
// Build graphics pipeline from the specified info.
Result Compiler::BuildGraphicsPipeline(
    const GraphicsPipelineBuildInfo* pPipelineInfo,     // [in] Info to build this graphics pipeline
    GraphicsPipelineBuildOut*        pPipelineOut,      // [out] Output of building this graphics pipeline
    void*                            pPipelineDumpFile) // [in] Handle of pipeline dump file
{
    Result           result = Result::Success;
    BinaryData       elfBin = {};

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

    if ((result == Result::Success) && EnableOuts())
    {
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC calculated hash results (graphics pipline)\n\n");
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

    if ((result == Result::Success) && (pPipelineDumpFile != nullptr))
    {
        std::stringstream strStream;
        strStream << ";Compiler Options: ";
        for (auto& option : m_options)
        {
            strStream << option << " ";
        }
        std::string extraInfo = strStream.str();
        PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile*>(pPipelineDumpFile), &extraInfo);
    }

    ShaderEntryState cacheEntryState  = ShaderEntryState::New;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    constexpr uint32_t ShaderCacheCount = 2;
    ShaderCache*     pShaderCache[ShaderCacheCount]  = { nullptr, nullptr };
    CacheEntryHandle hEntry[ShaderCacheCount]        = { nullptr, nullptr };

    cacheEntryState = LookUpShaderCaches(pPipelineInfo->pShaderCache, &cacheHash, &elfBin, pShaderCache, hEntry);
#else
    CacheEntryHandle hEntry = nullptr;
    cacheEntryState = LookUpShaderCache(&cacheHash, &elfBin, &hEntry);
#endif

    ElfPackage candidateElf;

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        uint32_t                      forceLoopUnrollCount = cl::ForceLoopUnrollCount;

        GraphicsContext graphicsContext(m_gfxIp,
                                        &m_gpuProperty,
                                        &m_gpuWorkarounds,
                                        pPipelineInfo,
                                        &pipelineHash,
                                        &cacheHash);
        result = BuildGraphicsPipelineInternal(&graphicsContext,
                                               shaderInfo,
                                               forceLoopUnrollCount,
                                               &candidateElf);

        if (result == Result::Success)
        {
            elfBin.codeSize = candidateElf.size();
            elfBin.pCode = candidateElf.data();
        }
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        UpdateShaderCaches((result == Result::Success), &elfBin, pShaderCache, hEntry, ShaderCacheCount);
#else
        UpdateShaderCache((result == Result::Success), &elfBin, hEntry);
#endif
    }

    if (result == Result::Success)
    {
        void* pAllocBuf = nullptr;
        if (pPipelineInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pPipelineInfo->pfnOutputAlloc(pPipelineInfo->pInstance, pPipelineInfo->pUserData, elfBin.codeSize);
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }

        uint8_t* pCode = static_cast<uint8_t*>(pAllocBuf);
        memcpy(pCode, elfBin.pCode, elfBin.codeSize);

        pPipelineOut->pipelineBin.codeSize = elfBin.codeSize;
        pPipelineOut->pipelineBin.pCode = pCode;
    }

    return result;
}

// =====================================================================================================================
// Build compute pipeline internally
Result Compiler::BuildComputePipelineInternal(
    ComputeContext*                 pComputeContext,                // [in] Compute context this compute pipeline
    const ComputePipelineBuildInfo* pPipelineInfo,                  // [in] Pipeline info of this compute pipeline
    uint32_t                        forceLoopUnrollCount,           // [in] Force loop unroll count (0 means disable)
    ElfPackage*                     pPipelineElf)                   // [out] Output Elf package
{
    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pComputeContext);
    pContext->SetBuilder(Builder::Create(*pContext));

    const PipelineShaderInfo* shaderInfo[ShaderStageNativeStageCount] =
    {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &pPipelineInfo->cs,
    };

    Result result = BuildPipelineInternal(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf);

    delete pContext->GetBuilder();
    pContext->SetBuilder(nullptr);
    ReleaseContext(pContext);
    return result;
}

// =====================================================================================================================
// Build compute pipeline from the specified info.
Result Compiler::BuildComputePipeline(
    const ComputePipelineBuildInfo* pPipelineInfo,     // [in] Info to build this compute pipeline
    ComputePipelineBuildOut*        pPipelineOut,      // [out] Output of building this compute pipeline
    void*                           pPipelineDumpFile) // [in] Handle of pipeline dump file
{
    BinaryData elfBin = {};

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 32
    // NOTE: It is to workaround the bug in Device::CreateInternalComputePipeline,
    // we forgot to set the entryStage in it. To keep backward compatibility, set the entryStage within LLPC.
    const_cast<ComputePipelineBuildInfo*>(pPipelineInfo)->cs.entryStage = ShaderStageCompute;
#endif

    Result result = ValidatePipelineShaderInfo(ShaderStageCompute, &pPipelineInfo->cs);

    MetroHash::Hash cacheHash = {};
    MetroHash::Hash pipelineHash = {};
    cacheHash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, true);
    pipelineHash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, false);

    if ((result == Result::Success) && EnableOuts())
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
        auto pModuleHash = reinterpret_cast<const MetroHash::Hash*>(&pModuleData->hash[0]);
        LLPC_OUTS("\n===============================================================================\n");
        LLPC_OUTS("// LLPC calculated hash results (compute pipline)\n\n");
        LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::Compact64(&pipelineHash)) << "\n");
        LLPC_OUTS(format("%-4s : ", GetShaderStageAbbreviation(ShaderStageCompute, true)) <<
                  format("0x%016" PRIX64, MetroHash::Compact64(pModuleHash)) << "\n");
        LLPC_OUTS("\n");
    }

    if ((result == Result::Success) && (pPipelineDumpFile != nullptr))
    {
        std::stringstream strStream;
        strStream << ";Compiler Options: ";
        for (auto& option : m_options)
        {
            strStream << option << " ";
        }
        std::string extraInfo = strStream.str();
        PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile*>(pPipelineDumpFile), &extraInfo);
    }

    ShaderEntryState cacheEntryState  = ShaderEntryState::New;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    constexpr uint32_t ShaderCacheCount = 2;
    ShaderCache* pShaderCache[ShaderCacheCount] = { nullptr, nullptr };
    CacheEntryHandle hEntry[ShaderCacheCount] = { nullptr, nullptr };

    cacheEntryState = LookUpShaderCaches(pPipelineInfo->pShaderCache, &cacheHash, &elfBin, pShaderCache, hEntry);
#else
    CacheEntryHandle hEntry = nullptr;
    cacheEntryState = LookUpShaderCache(&cacheHash, &elfBin, &hEntry);
#endif

    ElfPackage candidateElf;

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        uint32_t                      forceLoopUnrollCount = cl::ForceLoopUnrollCount;

        ComputeContext computeContext(m_gfxIp,
                                      &m_gpuProperty,
                                      &m_gpuWorkarounds,
                                      pPipelineInfo,
                                      &pipelineHash,
                                      &cacheHash);

        result = BuildComputePipelineInternal(&computeContext,
                                              pPipelineInfo,
                                              forceLoopUnrollCount,
                                              &candidateElf);

        if (result == Result::Success)
        {
            elfBin.codeSize = candidateElf.size();
            elfBin.pCode = candidateElf.data();
        }
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
        UpdateShaderCaches((result == Result::Success), &elfBin, pShaderCache, hEntry, ShaderCacheCount);
#else
        UpdateShaderCache((result == Result::Success), &elfBin, hEntry);
#endif
    }

    if (result == Result::Success)
    {
        void* pAllocBuf = nullptr;
        if (pPipelineInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pPipelineInfo->pfnOutputAlloc(pPipelineInfo->pInstance, pPipelineInfo->pUserData, elfBin.codeSize);
            if (pAllocBuf != nullptr)
            {
                uint8_t* pCode = static_cast<uint8_t*>(pAllocBuf);
                memcpy(pCode, elfBin.pCode, elfBin.codeSize);

                pPipelineOut->pipelineBin.codeSize = elfBin.codeSize;
                pPipelineOut->pipelineBin.pCode = pCode;
            }
            else
            {
                result = Result::ErrorOutOfMemory;
            }
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }
    }

    return result;
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
        cl::ShaderCacheFileDir.ArgStr,
        cl::ShaderCacheMode.ArgStr,
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

    MetroHash64 hasher;

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
                uint32_t stageMask =
                    ShaderModuleHelper::GetStageMaskFromSpirvBinary(pSpirvBin, pShaderInfo->pEntryTarget);

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
        else if ((pModuleData->binType == BinaryType::LlvmBc) || (pModuleData->binType == BinaryType::MultiLlvmBc))
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

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
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

    if ((result == Result::Success) &&
        ((cl::ShaderCacheMode == ShaderCacheEnableRuntime) ||
         (cl::ShaderCacheMode == ShaderCacheEnableOnDisk)) &&
        (pCreateInfo->initialDataSize > 0))
    {
        m_shaderCache->Merge(1, const_cast<const IShaderCache**>(ppShaderCache));
    }

    return result;
}
#endif

// =====================================================================================================================
// Initialize GPU property.
void Compiler::InitGpuProperty()
{
    // Initial settings (could be adjusted later according to graphics IP version info)
    memset(&m_gpuProperty, 0, sizeof(m_gpuProperty));
    m_gpuProperty.waveSize = 64;

#if LLPC_BUILD_GFX10
    if (m_gfxIp.major == 10)
    {
        // Compiler is free to choose wave mode if forced wave size is not specified.
        if (cl::NativeWaveSize != 0)
        {
            LLPC_ASSERT((cl::NativeWaveSize == 32) || (cl::NativeWaveSize == 64));
            m_gpuProperty.waveSize = cl::NativeWaveSize;
        }
        else
        {
            m_gpuProperty.waveSize = 32;
        }
    }
    else if (m_gfxIp.major > 10)
    {
        LLPC_NOT_IMPLEMENTED();
    }
#endif

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

    m_gpuProperty.tessFactorBufferSizePerSe = 4096;

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
        m_gpuProperty.tessFactorBufferSizePerSe = 8192;
        if (m_gfxIp.stepping == 0)
        {
            m_gpuProperty.numShaderEngines = 4;
        }
    }
#if LLPC_BUILD_GFX10
    else if (m_gfxIp.major == 10)
    {
        m_gpuProperty.numShaderEngines = 2;
        m_gpuProperty.supportShaderPowerProfiling = true;
        m_gpuProperty.tessFactorBufferSizePerSe = 8192;

        if (m_gfxIp.minor != 0)
        {
            m_gpuProperty.supportSpiPrefPriority = true; // For GFX10.1+
        }

        if ((m_gfxIp.minor == 1) && (m_gfxIp.stepping == 0xFFFF))
        {
            m_gpuProperty.tessFactorBufferSizePerSe = 0x80;
        }
    }
#endif
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

        m_gpuWorkarounds.gfx6.shaderReadlaneSmrd = 1;

        m_gpuWorkarounds.gfx6.shaderSpiCsRegAllocFragmentation = 1;

        m_gpuWorkarounds.gfx6.shaderVcczScalarReadBranchFailure = 1;

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
        if ((m_gfxIp.stepping == 3) || (m_gfxIp.stepping == 4))
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

        m_gpuWorkarounds.gfx6.shaderSmemBufferAddrClamp = 1;

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

        m_gpuWorkarounds.gfx9.shaderImageGatherInstFix = 1;

        m_gpuWorkarounds.gfx9.fixCacheLineStraddling = 1;

        if (m_gfxIp.stepping == 0 || m_gfxIp.stepping == 2)
        {
            m_gpuWorkarounds.gfx9.fixLsVgprInput = 1;
        }
    }
#if LLPC_BUILD_GFX10
    else if (m_gfxIp.major == 10)
    {
        // Hardware workarounds for GFX10 based GPU's:
        m_gpuWorkarounds.gfx10.disableI32ModToI16Mod = 1;

        if ((m_gfxIp.minor == 1) && (m_gfxIp.stepping == 0xFFFF))
        {
            m_gpuWorkarounds.gfx10.waTessFactorBufferSizeLimitGeUtcl1Underflow = 1;
        }

        if (m_gfxIp.minor == 1)
        {
            switch (m_gfxIp.stepping)
            {
            case 0:
            case 0xFFFE:
            case 0xFFFF:
                m_gpuWorkarounds.gfx10.waShaderInstPrefetch0 = 1;
                m_gpuWorkarounds.gfx10.waDidtThrottleVmem = 1;
                m_gpuWorkarounds.gfx10.waLdsVmemNotWaitingVmVsrc = 1;
                m_gpuWorkarounds.gfx10.waNsaAndClauseCanHang = 1;
                m_gpuWorkarounds.gfx10.waNsaCannotFollowWritelane = 1;
                m_gpuWorkarounds.gfx10.waTessIncorrectRelativeIndex = 1;
                m_gpuWorkarounds.gfx10.waSmemFollowedByVopc = 1;

                if (m_gfxIp.stepping == 0xFFFF)
                {
                    m_gpuWorkarounds.gfx10.waShaderInstPrefetch123   = 1;
                    m_gpuWorkarounds.gfx10.nggTessDegeneratePrims    = 1;
                    m_gpuWorkarounds.gfx10.waThrottleInMultiDwordNsa = 1;
                    m_gpuWorkarounds.gfx10.waNggCullingNoEmptySubgroups = 1;
                }
                break;
            default:
                LLPC_NEVER_CALLED();
                break;
            }
        }
    }
#endif

}
// =====================================================================================================================
// Acquires a free context from context pool.
Context* Compiler::AcquireContext() const
{
    Context* pFreeContext = nullptr;

    std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);

    // Try to find a free context from pool first
    for (auto pContext : *m_pContextPool)
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
        m_pContextPool->push_back(pFreeContext);
    }

    LLPC_ASSERT(pFreeContext != nullptr);
    return pFreeContext;
}

// =====================================================================================================================
// Run a pass manager's passes on a module, catching any LLVM fatal error and returning a success indication
bool Compiler::RunPasses(
    PassManager*  pPassMgr, // [in] Pass manager
    Module*       pModule   // [in/out] Module
    ) const
{
    bool success = false;
#if LLPC_ENABLE_EXCEPTION
    try
#endif
    {
        pPassMgr->run(*pModule);
        success = true;
    }
#if LLPC_ENABLE_EXCEPTION
    catch (const char*)
    {
        success = false;
    }
#endif
    return success;
}

// =====================================================================================================================
// Releases LLPC context.
void Compiler::ReleaseContext(
    Context* pContext    // [in] LLPC context
    ) const
{
    std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);
    pContext->Reset();
    pContext->SetInUse(false);
}

// =====================================================================================================================
// Lookup in the shader caches with the given pipeline hash code.
// It will try App's pipelince cache first if that's available.
// Then try on the internal shader cache next if it misses.
//
// NOTE: Only two items in the array of shader caches; one for App's pipeline cache and one for internal cache
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
ShaderEntryState Compiler::LookUpShaderCaches(
    IShaderCache*                    pAppPipelineCache, // [in]    App's pipeline cache
    MetroHash::Hash*                 pCacheHash,        // [in]    Hash code of the shader
    BinaryData*                      pElfBin,           // [inout] Pointer to shader data
    ShaderCache**                    ppShaderCache,     // [in]    Array of shader caches.
    CacheEntryHandle*                phEntry            // [in]    Array of handles of the shader caches entry
    )
{
    ShaderEntryState cacheEntryState  = ShaderEntryState::New;
    uint32_t         shaderCacheCount = 1;
    Result           result           = Result::Success;

    if (pAppPipelineCache != nullptr)
    {
        ppShaderCache[0] = static_cast<ShaderCache*>(pAppPipelineCache);
        ppShaderCache[1] = m_shaderCache.get();
        shaderCacheCount = 2;
    }
    else
    {
        ppShaderCache[0] = m_shaderCache.get();
        ppShaderCache[1] = nullptr;
    }

    if (cl::ShaderCacheMode == ShaderCacheForceInternalCacheOnDisk)
    {
        ppShaderCache[0] = m_shaderCache.get();
        ppShaderCache[1] = nullptr;
        shaderCacheCount = 1;
    }

    for (uint32_t i = 0; i < shaderCacheCount; i++)
    {
        cacheEntryState = ppShaderCache[i]->FindShader(*pCacheHash, true, &phEntry[i]);
        if (cacheEntryState == ShaderEntryState::Ready)
        {
            result = ppShaderCache[i]->RetrieveShader(phEntry[i], &pElfBin->pCode, &pElfBin->codeSize);
            // Re-try if shader cache return error unknown
            if (result == Result::ErrorUnknown)
            {
                result = Result::Success;
                phEntry[i] = nullptr;
                cacheEntryState = ShaderEntryState::Compiling;
            }
            else
            {
                if (i == 1)
                {
                    // App's pipeline cache misses while internal cache hits
                    if (phEntry[0] != nullptr)
                    {
                        LLPC_ASSERT(pElfBin->codeSize > 0);
                        ppShaderCache[0]->InsertShader(phEntry[0], pElfBin->pCode, pElfBin->codeSize);
                    }
                }
                break;
            }
        }
    }

    return cacheEntryState;
}
#else
ShaderEntryState Compiler::LookUpShaderCache(
    MetroHash::Hash*                 pCacheHash,        // [in]    Hash code of the shader
    BinaryData*                      pElfBin,           // [out]   Pointer to shader data
    CacheEntryHandle*                phEntry            // [out]   Handle of the shader caches entry
    )
{
    ShaderEntryState cacheEntryState  = ShaderEntryState::New;
    Result           result           = Result::Success;

    cacheEntryState = m_shaderCache->FindShader(*pCacheHash, true, phEntry);
    if (cacheEntryState == ShaderEntryState::Ready)
    {
        result = m_shaderCache->RetrieveShader(*phEntry, &pElfBin->pCode, &pElfBin->codeSize);
        // Re-try if shader cache return error unknown
        if (result == Result::ErrorUnknown)
        {
            result = Result::Success;
            *phEntry = nullptr;
            cacheEntryState = ShaderEntryState::Compiling;
        }
    }

    return cacheEntryState;
}
#endif

// =====================================================================================================================
// Update the shader caches with the given entry handle, based on the "insert" flag.
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
void Compiler::UpdateShaderCaches(
    bool                             insert,           // [in] To insert data or reset the shader caches
    const BinaryData*                pElfBin,           // [in] Pointer to shader data
    ShaderCache**                    ppShaderCache,     // [in] Array of shader caches; one for App's pipeline cache and one for internal cache
    CacheEntryHandle*                phEntry,           // [in] Array of handles of the shader caches entry
    uint32_t                         shaderCacheCount   // [in] Shader caches count
)
{
    for (uint32_t i = 0; i < shaderCacheCount; i++)
    {
        if (phEntry[i] != nullptr)
        {
            if (insert)
            {
                LLPC_ASSERT(pElfBin->codeSize > 0);
                ppShaderCache[i]->InsertShader(phEntry[i], pElfBin->pCode, pElfBin->codeSize);
            }
            else
            {
                ppShaderCache[i]->ResetShader(phEntry[i]);
            }
        }
    }
}
#else
void Compiler::UpdateShaderCache(
    bool                             insert,           // [in] To insert data or reset the shader caches
    const BinaryData*                pElfBin,           // [in] Pointer to shader data
    CacheEntryHandle                 hEntry)            // [in] Handle of the shader caches entry
{
    if (hEntry != nullptr)
    {
        if (insert)
        {
            LLPC_ASSERT(pElfBin->codeSize > 0);
            m_shaderCache->InsertShader(hEntry, pElfBin->pCode, pElfBin->codeSize);
        }
        else
        {
            m_shaderCache->ResetShader(hEntry);
        }
    }
}
#endif

// =====================================================================================================================
// Builds hash code from input context for per shader stage cache
void Compiler::BuildShaderCacheHash(
    Context*                    pContext,           // [in] Acquired context
    uint32_t                    stageMask,          // Shader stage mask
    ArrayRef<ArrayRef<uint8_t>> stageHashes,        // Per-stage hash of in/out usage
    MetroHash::Hash*            pFragmentHash,      // [out] Hash code of fragment shader
    MetroHash::Hash*            pNonFragmentHash)   // [out] Hash code of all non-fragment shader
{
    MetroHash64 fragmentHasher;
    MetroHash64 nonFragmentHasher;
    auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
    auto pPipelineOptions = pContext->GetPipelineContext()->GetPipelineOptions();

    // Build hash per shader stage
    for (auto stage = ShaderStageVertex; stage < ShaderStageGfxCount; stage = static_cast<ShaderStage>(stage + 1))
    {
        if ((stageMask & ShaderStageToMask(stage)) == 0)
        {
            continue;
        }

        auto pShaderInfo = pContext->GetPipelineShaderInfo(stage);
        MetroHash64 hasher;

        // Update common shader info
        PipelineDumper::UpdateHashForPipelineShaderInfo(stage, pShaderInfo, true, &hasher);
        hasher.Update(pPipelineInfo->iaState.deviceIndex);

        // Update input/output usage (provided by middle-end caller of this callback).
        hasher.Update(stageHashes[stage].data(), stageHashes[stage].size());

        // Update vertex input state
        if (stage == ShaderStageVertex)
        {
            PipelineDumper::UpdateHashForVertexInputState(pPipelineInfo->pVertexInput, &hasher);
        }

        MetroHash::Hash  hash = {};
        hasher.Finalize(hash.bytes);

        // Add per stage hash code to fragmentHasher or nonFragmentHaser per shader stage
        auto shaderHashCode = MetroHash::Compact64(&hash);
        if (stage == ShaderStageFragment)
        {
            fragmentHasher.Update(shaderHashCode);
        }
        else
        {
            nonFragmentHasher.Update(shaderHashCode);
        }
    }

    // Add addtional pipeline state to final hasher
    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        // Add pipeline options to fragment hash
        fragmentHasher.Update(pPipelineOptions->includeDisassembly);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 30
        fragmentHasher.Update(pPipelineOptions->autoLayoutDesc);
#endif
        fragmentHasher.Update(pPipelineOptions->scalarBlockLayout);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
        fragmentHasher.Update(pPipelineOptions->reconfigWorkgroupLayout);
#endif
        fragmentHasher.Update(pPipelineOptions->includeIr);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 23
        fragmentHasher.Update(pPipelineOptions->robustBufferAccess);
#endif
#if (LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 25) && (LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 27)
        fragmentHasher.Update(pPipelineOptions->includeIrBinary);
#endif
        PipelineDumper::UpdateHashForFragmentState(pPipelineInfo, &fragmentHasher);
        fragmentHasher.Finalize(pFragmentHash->bytes);
    }

    if (stageMask & ~ShaderStageToMask(ShaderStageFragment))
    {
        PipelineDumper::UpdateHashForNonFragmentState(pPipelineInfo, true, &nonFragmentHasher);
        nonFragmentHasher.Finalize(pNonFragmentHash->bytes);
    }
}

} // Llpc
