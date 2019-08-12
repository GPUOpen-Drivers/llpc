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
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/MutexGuard.h"
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
#include "llpcElfReader.h"
#include "llpcElfWriter.h"
#include "llpcFile.h"
#include "llpcPassLoopInfoCollect.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "llpcPipelineDumper.h"
#include "llpcSpirvLower.h"
#include "llpcVertexFetch.h"
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

// -enable-dynamic-loop-unroll: Enable dynamic loop unroll.
opt<bool> EnableDynamicLoopUnroll("enable-dynamic-loop-unroll", desc("Enable dynamic loop unroll"), init(false));

// -force-loop-unroll-count: Force to set the loop unroll count; this option will ignore dynamic loop unroll.
opt<int> ForceLoopUnrollCount("force-loop-unroll-count", cl::desc("Force loop unroll count"), init(0));

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

llvm::sys::Mutex      Compiler::m_contextPoolMutex;
std::vector<Context*> Compiler::m_contextPool;

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
    void* pAllocBuf = nullptr;
    BinaryType binType = BinaryType::Spirv;
    ShaderModuleInfo moduleInfo = {};

    // Check the type of input shader binary
    if (IsSpirvBinary(&pShaderInfo->shaderBin))
    {
        binType = BinaryType::Spirv;
        if (VerifySpirvBinary(&pShaderInfo->shaderBin) != Result::Success)
        {
            LLPC_ERRS("Unsupported SPIR-V instructions are found!\n");
            result = Result::Unsupported;
        }
        if (result == Result::Success)
        {
            CollectInfoFromSpirvBinary(&pShaderInfo->shaderBin, &moduleInfo);
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

    auto codeSize = pShaderInfo->shaderBin.codeSize;
    if (cl::TrimDebugInfo)
    {
        codeSize -= moduleInfo.debugInfoSize;
    }

    if (result == Result::Success)
    {
        if (pShaderInfo->pfnOutputAlloc != nullptr)
        {
            size_t allocSize = sizeof(ShaderModuleData) + codeSize;
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
        pModuleData->binCode.codeSize = codeSize;
        MetroHash::Hash hash = {};
        MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pShaderInfo->shaderBin.pCode),
                          pShaderInfo->shaderBin.codeSize,
                          hash.bytes);
        static_assert(sizeof(pModuleData->hash) == sizeof(hash), "Unexpected value!");
        memcpy(pModuleData->hash, hash.dwords, sizeof(hash));
        pModuleData->moduleInfo = moduleInfo;

        void* pCode = VoidPtrInc(pAllocBuf, sizeof(ShaderModuleData));

        if ((binType == BinaryType::Spirv) && cl::EnablePipelineDump)
        {
            PipelineDumper::DumpSpirvBinary(cl::PipelineDumpDir.c_str(),
                &pShaderInfo->shaderBin,
                &hash);
        }

        // Copy SPIR-V binary to output buffer
        if ((binType == BinaryType::Spirv) && cl::TrimDebugInfo)
        {
            TrimSpirvDebugInfo(&pShaderInfo->shaderBin, codeSize, pCode);
        }
        else
        {
            memcpy(pCode, pShaderInfo->shaderBin.pCode, pShaderInfo->shaderBin.codeSize);

        }
        pModuleData->binCode.pCode = pCode;

        // Calculate SPIR-V cache hash
        MetroHash::Hash cacheHash = {};
        MetroHash64::Hash(reinterpret_cast<const uint8_t*>(pModuleData->binCode.pCode),
            pModuleData->binCode.codeSize,
            cacheHash.bytes);
        static_assert(sizeof(pModuleData->moduleInfo.cacheHash) == sizeof(cacheHash), "Unexpected value!");
        memcpy(pModuleData->moduleInfo.cacheHash, cacheHash.dwords, sizeof(cacheHash));

        pShaderOut->pModuleData = pModuleData;
    }

    return result;
}

// =====================================================================================================================
// Build pipeline internally -- common code for graphics and compute
Result Compiler::BuildPipelineInternal(
    Context*                            pContext,                   // [in] Acquired context
    ArrayRef<const PipelineShaderInfo*> shaderInfo,                 // [in] Shader info of this pipeline
    uint32_t                            forceLoopUnrollCount,       // [in] Force loop unroll count (0 means disable)
    ElfPackage*                         pPipelineElf,               // [out] Output Elf package
    bool*                               pDynamicLoopUnroll)         // [out] Need dynamic loop unroll or not
{
    Result          result    = Result::Success;

    if ((forceLoopUnrollCount != 0) || (cl::EnableDynamicLoopUnroll == false))
    {
        pDynamicLoopUnroll = nullptr;
    }

    // Create the timer group and timers for -time-passes.
    std::string hash;
    raw_string_ostream ostream(hash);
    ostream << format("0x%016" PRIX64, pContext->GetPipelineContext()->GetPiplineHashCode());
    ostream.flush();
    uint32_t passIndex = 0;
    // NOTE: It is a workaround to get fixed layout in timer reports. Please remove it if we find a better solution.
    // LLVM timer skips the field if it is zero in all timers, it causes the layout of the report isn't stable when
    // compile multiple pipelines. so we add a dummy record to force all fields is shown.
    // But LLVM TimeRecord can't be initialized explicitly. We have to use HackedTimeRecord to force update the vaule
    // in TimeRecord.
    TimeRecord timeRecord;
    struct HackedTimeRecord
    {
        double t1;
        double t2;
        double t3;
        ssize_t m1;
    } hackedTimeRecord = { 1e-100, 1e-100, 1e-100, 0 };
    static_assert(sizeof(timeRecord) == sizeof(hackedTimeRecord), "Unexpected Size!");
    memcpy(&timeRecord, &hackedTimeRecord, sizeof(TimeRecord));

    StringMap<TimeRecord> dummyTimeRecords;
    if (TimePassesIsEnabled)
    {
        dummyTimeRecords["DUMMY"] = timeRecord;
    }

    TimerGroup timerGroupTotal("llpc", (Twine("LLPC ") + hash).str(), dummyTimeRecords);
    Timer wholeTimer("llpc-total", (Twine("LLPC Total ") + hash).str(), timerGroupTotal);

    TimerGroup timerGroupPhases("llpc", (Twine("LLPC Phases ") + hash).str(), dummyTimeRecords);
    Timer translateTimer("llpc-translate", (Twine("LLPC Translate ") + hash).str(), timerGroupPhases);
    Timer lowerTimer("llpc-lower", (Twine("LLPC Lower ") + hash).str(), timerGroupPhases);
    Timer patchTimer("llpc-patch", (Twine("LLPC Patch ") + hash).str(), timerGroupPhases);
    Timer optTimer("llpc-opt", (Twine("LLPC Optimization ") + hash).str(), timerGroupPhases);
    Timer codeGenTimer("llpc-codegen", (Twine("LLPC CodeGen ") + hash).str(), timerGroupPhases);

    if (TimePassesIsEnabled)
    {
        wholeTimer.startTimer();
    }

    pContext->setDiagnosticHandler(llvm::make_unique<LlpcDiagnosticHandler>());

    // Create the AMDGPU TargetMachine.
    result = CodeGenManager::CreateTargetMachine(pContext);

    Module* pPipelineModule = nullptr;

    // NOTE: If input is LLVM IR, read it now. There is now only ever one IR module representing the
    // whole pipeline.
    bool IsLlvmBc = false;
    const PipelineShaderInfo* pShaderInfo = shaderInfo[0];
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
        Module* modules[ShaderStageNativeStageCount] = {};
        for (uint32_t stage = 0; (stage < shaderInfo.size()) && (result == Result::Success); ++stage)
        {
            const PipelineShaderInfo* pShaderInfo = shaderInfo[stage];
            if ((pShaderInfo == nullptr) || (pShaderInfo->pModuleData == nullptr))
            {
                continue;
            }

            Module* pModule = new Module((Twine("llpc") + GetShaderStageName(static_cast<ShaderStage>(stage))).str(),
                                         *pContext);
            modules[stage] = pModule;
            pContext->SetModuleTargetMachine(pModule);
        }

        // Give the pipeline state to the Builder. (If we know we are using BuilderRecorder, in a future change
        // we could choose to delay this until after linking into a pipeline module.)
        pContext->GetPipelineContext()->SetBuilderPipelineState(pContext->GetBuilder());

        for (uint32_t stage = 0; (stage < shaderInfo.size()) && (result == Result::Success); ++stage)
        {
            const PipelineShaderInfo* pShaderInfo = shaderInfo[stage];
            if ((pShaderInfo == nullptr) || (pShaderInfo->pModuleData == nullptr))
            {
                continue;
            }

            PassManager lowerPassMgr(&passIndex);

            // Set the shader stage in the Builder.
            pContext->GetBuilder()->SetShaderStage(static_cast<ShaderStage>(stage));

            // Start timer for translate.
            if (TimePassesIsEnabled)
            {
                lowerPassMgr.add(CreateStartStopTimer(&translateTimer, true));
            }

            // SPIR-V translation, then dump the result.
            lowerPassMgr.add(CreateSpirvLowerTranslator(static_cast<ShaderStage>(stage), pShaderInfo));
            if (EnableOuts())
            {
                lowerPassMgr.add(createPrintModulePass(outs(), "\n"
                            "===============================================================================\n"
                            "// LLPC SPIRV-to-LLVM translation results\n"));
            }

            // Stop timer for translate.
            if (TimePassesIsEnabled)
            {
                lowerPassMgr.add(CreateStartStopTimer(&translateTimer, false));
            }

            // Per-shader SPIR-V lowering passes.
            SpirvLower::AddPasses(static_cast<ShaderStage>(stage),
                                  lowerPassMgr,
                                  TimePassesIsEnabled ? &lowerTimer : nullptr,
                                  forceLoopUnrollCount,
                                  pDynamicLoopUnroll);

            // Run the passes.
            bool success = RunPasses(&lowerPassMgr, modules[stage]);
            if (success == false)
            {
                LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
                result = Result::ErrorInvalidShader;
            }
        }

        // Link the shader modules into a single pipeline module.
        pPipelineModule = pContext->GetBuilder()->Link(modules);
        if (pPipelineModule == nullptr)
        {
            LLPC_ERRS("Failed to link shader modules into pipeline module\n");
            result = Result::ErrorInvalidShader;
        }
    }

    // Run necessary patch pass to prepare per shader stage cache
    PassManager prePatchPassMgr(&passIndex);
    if (result == Result::Success)
    {
        // Patching.
        Patch::AddPrePatchPasses(pContext,
                                 prePatchPassMgr,
                                 TimePassesIsEnabled ? &patchTimer : nullptr);

	bool success = RunPasses(&prePatchPassMgr, pPipelineModule);
        if (success == false)
        {
            result = Result::ErrorInvalidShader;
        }
    }

    constexpr uint32_t ShaderCacheCount = 2;
    ShaderEntryState fragmentCacheEntryState = ShaderEntryState::New;
    ShaderCache* pFragmentShaderCache[ShaderCacheCount] = { nullptr, nullptr };
    CacheEntryHandle hFragmentEntry[ShaderCacheCount] = { nullptr, nullptr };

    ShaderEntryState nonFragmentCacheEntryState = ShaderEntryState::New;
    ShaderCache* pNonFragmentShaderCache[ShaderCacheCount] = { nullptr, nullptr };
    CacheEntryHandle hNonFragmentEntry[ShaderCacheCount] = { nullptr, nullptr };

    BinaryData fragmentElf = {};
    BinaryData nonFragmentElf = {};

    uint32_t stageMask = pContext->GetShaderStageMask();

    // Only enable per stage cache for full graphic pipeline
    bool checkPerStageCache = cl::EnablePerStageCache &&
                              pContext->IsGraphics() &&
                              (stageMask & ShaderStageToMask(ShaderStageVertex)) &&
                              (stageMask & ShaderStageToMask(ShaderStageFragment));

    if (checkPerStageCache && (result == Result::Success))
    {
        // Check per stage shader cache
        MetroHash::Hash fragmentHash = {};
        MetroHash::Hash nonFragmentHash = {};
        BuildShaderCacheHash(pContext, &fragmentHash, &nonFragmentHash);

        // NOTE: Global constant are added to the end of pipeline binary. we can't merge ELF binaries if global constant
        // is used in non-fragment shader stages.
        for (ShaderStage stage = ShaderStageVertex; stage < ShaderStageFragment; stage = static_cast<ShaderStage>(stage + 1))
        {
            if (stageMask & ShaderStageToMask(stage))
            {
                auto pResUsage = pContext->GetShaderResourceUsage(stage);
                if (pResUsage->globalConstant)
                {
                    checkPerStageCache = false;
                    break;
                }
            }
        }

        if (checkPerStageCache)
        {
            auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
            if (stageMask & ShaderStageToMask(ShaderStageFragment))
            {
                fragmentCacheEntryState = LookUpShaderCaches(pPipelineInfo->pShaderCache,
                                                             &fragmentHash,
                                                             &fragmentElf,
                                                             pFragmentShaderCache,
                                                             hFragmentEntry);
            }

            if (stageMask & ~ShaderStageToMask(ShaderStageFragment))
            {
                nonFragmentCacheEntryState = LookUpShaderCaches(pPipelineInfo->pShaderCache,
                                                                &nonFragmentHash,
                                                                &nonFragmentElf,
                                                                pNonFragmentShaderCache,
                                                                hNonFragmentEntry);
            }
        }
    }

    if ((checkPerStageCache == false) ||
        (fragmentCacheEntryState == ShaderEntryState::Compiling) ||
        (nonFragmentCacheEntryState == ShaderEntryState::Compiling))
    {
        // Set up "whole pipeline" passes, where we have a single module representing the whole pipeline.
        //
        // TODO: The "whole pipeline" passes are supposed to include code generation passes. However, there is a CTS issue.
        // In the case "dEQP-VK.spirv_assembly.instruction.graphics.16bit_storage.struct_mixed_types.uniform_geom", GS gets
        // unrolled to such a size that backend compilation takes too long. Thus, we put code generation in its own pass
        // manager.
        PassManager patchPassMgr(&passIndex);
        patchPassMgr.add(createTargetTransformInfoWrapperPass(pContext->GetTargetMachine()->getTargetIRAnalysis()));

        ElfPackage partialPipelineElf;

        // Store result to partialPipelineElf for partial pipeline compile
        bool partialCompile = (fragmentCacheEntryState == ShaderEntryState::Ready) || (nonFragmentCacheEntryState == ShaderEntryState::Ready);
        raw_svector_ostream elfStream(partialCompile ? partialPipelineElf : *pPipelineElf);

        uint32_t skipStageMask = 0;
        if (fragmentCacheEntryState == ShaderEntryState::Ready)
        {
            skipStageMask = ShaderStageToMask(ShaderStageFragment);
        }

        if (nonFragmentCacheEntryState == ShaderEntryState::Ready)
        {
            skipStageMask = pContext->GetShaderStageMask() & ~ShaderStageToMask(ShaderStageFragment);
        }

        if (result == Result::Success)
        {
            // Patching.
            Patch::AddPasses(pContext,
                             patchPassMgr,
                             skipStageMask,
                             TimePassesIsEnabled ? &patchTimer : nullptr,
                             TimePassesIsEnabled ? &optTimer : nullptr);
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
            if (TimePassesIsEnabled)
            {
                codeGenPassMgr.add(CreateStartStopTimer(&codeGenTimer, true));
            }

            // Code generation.
            result = CodeGenManager::AddTargetPasses(pContext, codeGenPassMgr, elfStream);

            if (TimePassesIsEnabled)
            {
                codeGenPassMgr.add(CreateStartStopTimer(&codeGenTimer, false));
            }
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

        // Only non-fragment shaders are compiled
        if ((fragmentCacheEntryState == ShaderEntryState::Ready) &&
            (nonFragmentCacheEntryState == ShaderEntryState::Compiling))
        {
            BinaryData pipelineElf = {};
            if (result == Result::Success)
            {
                BinaryData nonFragmentPipelineElf = {};
                nonFragmentPipelineElf.pCode = partialPipelineElf.data();
                nonFragmentPipelineElf.codeSize = partialPipelineElf.size();

                MergeElfBinary(pContext, &fragmentElf, &nonFragmentPipelineElf, pPipelineElf);

                pipelineElf.codeSize = pPipelineElf->size();
                pipelineElf.pCode = pPipelineElf->data();
            }

            UpdateShaderCaches(result == Result::Success,
                               &pipelineElf,
                               pNonFragmentShaderCache,
                               hNonFragmentEntry,
                               ShaderCacheCount);
        }

        // Only fragment shader is compiled
        if ((nonFragmentCacheEntryState == ShaderEntryState::Ready) &&
            (fragmentCacheEntryState == ShaderEntryState::Compiling))
        {
            BinaryData pipelineElf = {};
            if (result == Result::Success)
            {
                BinaryData fragmentPipelineElf = {};
                fragmentPipelineElf.pCode = partialPipelineElf.data();
                fragmentPipelineElf.codeSize = partialPipelineElf.size();

                MergeElfBinary(pContext, &fragmentPipelineElf, &nonFragmentElf, pPipelineElf);

                pipelineElf.codeSize = pPipelineElf->size();
                pipelineElf.pCode = pPipelineElf->data();
            }

            UpdateShaderCaches(result == Result::Success,
                               &pipelineElf,
                               pFragmentShaderCache,
                               hFragmentEntry,
                               ShaderCacheCount);
        }

        // Whole pipeline is compiled
        if ((fragmentCacheEntryState == ShaderEntryState::Compiling) &&
            (nonFragmentCacheEntryState == ShaderEntryState::Compiling))
        {
            BinaryData pipelineElf = {};
            pipelineElf.codeSize = pPipelineElf->size();
            pipelineElf.pCode = pPipelineElf->data();
            UpdateShaderCaches((result == Result::Success),
                               &pipelineElf,
                               pFragmentShaderCache,
                               hFragmentEntry,
                               ShaderCacheCount);

            UpdateShaderCaches(result == Result::Success,
                               &pipelineElf,
                               pNonFragmentShaderCache,
                               hNonFragmentEntry,
                               ShaderCacheCount);
        }
    }
    else
    {
        MergeElfBinary(pContext, &fragmentElf, &nonFragmentElf, pPipelineElf);
    }

    pContext->setDiagnosticHandlerCallBack(nullptr);

    if (TimePassesIsEnabled)
    {
        wholeTimer.stopTimer();
    }

    delete pPipelineModule;
    pPipelineModule = nullptr;

    return result;
}

// =====================================================================================================================
// Build graphics pipeline internally
Result Compiler::BuildGraphicsPipelineInternal(
    GraphicsContext*                    pGraphicsContext,           // [in] Graphics context this graphics pipeline
    ArrayRef<const PipelineShaderInfo*> shaderInfo,                 // Shader info of this graphics pipeline
    uint32_t                            forceLoopUnrollCount,       // [in] Force loop unroll count (0 means disable)
    ElfPackage*                         pPipelineElf,               // [out] Output Elf package
    bool*                               pDynamicLoopUnroll)         // [out] Need dynamic loop unroll or not
{
    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pGraphicsContext);
    pContext->SetBuilder(Builder::Create(*pContext));

    Result result = BuildPipelineInternal(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf, pDynamicLoopUnroll);

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

    constexpr uint32_t ShaderCacheCount = 2;
    ShaderEntryState cacheEntryState  = ShaderEntryState::New;
    ShaderCache*     pShaderCache[ShaderCacheCount]  = { nullptr, nullptr };
    CacheEntryHandle hEntry[ShaderCacheCount]        = { nullptr, nullptr };

    cacheEntryState = LookUpShaderCaches(pPipelineInfo->pShaderCache, &cacheHash, &elfBin, pShaderCache, hEntry);

    constexpr uint32_t CandidateCount = 4;
    uint32_t           loopUnrollCountCandidates[CandidateCount] = { 32, 16, 4, 1 };
    bool               needRecompile   = false;
    uint32_t           candidateIdx    = 0;
    ElfPackage         candidateElfs[CandidateCount];

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        PipelineStatistics            pipelineStats[CandidateCount];
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
                GraphicsContext graphicsContext(m_gfxIp,
                                                &m_gpuProperty,
                                                &m_gpuWorkarounds,
                                                pPipelineInfo,
                                                &pipelineHash,
                                                &cacheHash);
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
            elfBin.codeSize = candidateElfs[candidateIdx].size();
            elfBin.pCode = candidateElfs[candidateIdx].data();
        }

        UpdateShaderCaches((result == Result::Success), &elfBin, pShaderCache, hEntry, ShaderCacheCount);
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

    if (pPipelineDumpFile != nullptr)
    {
        if (result == Result::Success)
        {
            if (needRecompile)
            {
                std::stringstream strStream;
                strStream << "Dynamic loop unroll enabled: loopUnrollCount = " << loopUnrollCountCandidates[candidateIdx] << " \n";
                std::string extraInfo = strStream.str();
                PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile*>(pPipelineDumpFile), &extraInfo);
            }
        }
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

    Result result = BuildPipelineInternal(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf, pDynamicLoopUnroll);

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

    constexpr uint32_t ShaderCacheCount = 2;
    ShaderEntryState cacheEntryState  = ShaderEntryState::New;
    ShaderCache*     pShaderCache[ShaderCacheCount]  = { nullptr, nullptr };
    CacheEntryHandle hEntry[ShaderCacheCount]        = { nullptr, nullptr };

    cacheEntryState = LookUpShaderCaches(pPipelineInfo->pShaderCache, &cacheHash, &elfBin, pShaderCache, hEntry);

    constexpr uint32_t CandidateCount = 4;
    uint32_t           loopUnrollCountCandidates[CandidateCount] = { 32, 16, 4, 1 };
    bool               needRecompile   = false;
    uint32_t           candidateIdx    = 0;
    ElfPackage         candidateElfs[CandidateCount];

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        PipelineStatistics            pipelineStats[CandidateCount];
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
                ComputeContext computeContext(m_gfxIp,
                                              &m_gpuProperty,
                                              &m_gpuWorkarounds,
                                              pPipelineInfo,
                                              &pipelineHash,
                                              &cacheHash);

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
            elfBin.codeSize = candidateElfs[candidateIdx].size();
            elfBin.pCode = candidateElfs[candidateIdx].data();
        }

        UpdateShaderCaches((result == Result::Success), &elfBin, pShaderCache, hEntry, ShaderCacheCount);
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

    if (pPipelineDumpFile != nullptr)
    {
        if (result == Result::Success)
        {
            if (needRecompile)
            {
                std::stringstream strStream;
                strStream << "Dynamic loop unroll enabled: loopUnrollCount = " << loopUnrollCountCandidates[candidateIdx] << " \n";
                std::string extraInfo = strStream.str();
                PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile*>(pPipelineDumpFile), &extraInfo);
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Translates SPIR-V binary to machine-independent LLVM module.
void Compiler::TranslateSpirvToLlvm(
    const BinaryData*           pSpirvBin,           // [in] SPIR-V binary
    ShaderStage                 shaderStage,         // Shader stage
    const char*                 pEntryTarget,        // [in] SPIR-V entry point
    const VkSpecializationInfo* pSpecializationInfo, // [in] Specialization info
    Module*                     pModule)             // [in/out] Module to translate into, initially empty
{
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

    Context* pContext = static_cast<Context*>(&pModule->getContext());
    pContext->SetModuleTargetMachine(pModule);

    const PipelineShaderInfo* pShaderInfo = pContext->GetPipelineShaderInfo(shaderStage);

    if (readSpirv(pContext->GetBuilder(),
                  pShaderInfo->pModuleData,
                  spirvStream,
                  static_cast<spv::ExecutionModel>(shaderStage),
                  pEntryTarget,
                  specConstMap,
                  pModule,
                  errMsg) == false)
    {
        report_fatal_error(Twine("Failed to translate SPIR-V to LLVM (") +
                            GetShaderStageName(static_cast<ShaderStage>(shaderStage)) + " shader): " + errMsg,
                           false);
    }

    CleanOptimizedSpirv(&optSpirvBin);

    // NOTE: Our shader entrypoint is marked in the SPIR-V reader as dllexport. Here we mark it as follows:
    //   * remove the dllexport;
    //   * ensure it is public.
    // Also mark all other functions internal and always_inline.
    //
    // TODO: We should rationalize this code as follows:
    //   1. Add code to the spir-v reader to add the entrypoint name as metadata;
    //   2. change this code here to detect that, instead of DLLExport;
    //   3. remove the code we added to the spir-v reader to detect the required entrypoint and mark it as DLLExport;
    //   4. remove the required entrypoint name and execution model args that we added to the spir-v reader API, to
    //      make it closer to the upstream Khronos copy of that code.
    for (auto& func : *pModule)
    {
        if (func.empty())
        {
            continue;
        }
        if (func.getDLLStorageClass() == GlobalValue::DLLExportStorageClass)
        {
            func.setDLLStorageClass(GlobalValue::DefaultStorageClass);
            func.setLinkage(GlobalValue::ExternalLinkage);
        }
        else
        {
            func.setLinkage(GlobalValue::InternalLinkage);
            func.addFnAttr(Attribute::AlwaysInline);
        }
    }
}

// =====================================================================================================================
// Optimizes SPIR-V binary
Result Compiler::OptimizeSpirv(
    const BinaryData* pSpirvBinIn,     // [in] Input SPIR-V binary
    BinaryData*       pSpirvBinOut)    // [out] Optimized SPIR-V binary
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
            report_fatal_error(Twine("Failed to optimize SPIR-V (") +
                                GetShaderStageName(static_cast<ShaderStage>(shaderStage) + " shader): " + logBuf,
                               false);
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
    BinaryData* pSpirvBin)  // [in] Optimized SPIR-V binary
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
        else if ((m_gfxIp.major == 10) && (m_gfxIp.minor == 0))
        {
            // Due to hardware issue, wave32 is not enabled in LLVM backend for gfx1000
            m_gpuProperty.waveSize = 64;
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

        if ((m_gfxIp.minor == 0) ||
            ((m_gfxIp.minor == 1) && (m_gfxIp.stepping == 0xFFFF)))
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

        if ((m_gfxIp.minor == 0) ||
            ((m_gfxIp.minor == 1) && (m_gfxIp.stepping == 0xFFFF)))
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
// Run a pass manager's passes on a module, catching any LLVM fatal error and returning a success indication
bool Compiler::RunPasses(
    PassManager*  pPassMgr, // [in] Pass manager
    Module*       pModule)  // [in/out] Module
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
    Context* pContext)    // [in] LLPC context
{
    MutexGuard lock(m_contextPoolMutex);
    pContext->SetInUse(false);
}

// =====================================================================================================================
// Collect information from SPIR-V binary
Result Compiler::CollectInfoFromSpirvBinary(
    const BinaryData*  pSpvBinCode,                 // [in] SPIR-V binary data
    ShaderModuleInfo*  pShaderModuleInfo            // [out] Shader module information
    )
{
    Result result = Result::Success;

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBinCode->pCode);
    const uint32_t* pEnd = pCode + pSpvBinCode->codeSize / sizeof(uint32_t);

    const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

    // Parse SPIR-V instructions
    std::unordered_set<uint32_t> capabilities;

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
        switch (opCode)
        {
        case spv::OpCapability:
            {
                LLPC_ASSERT(wordCount == 2);
                auto capability = static_cast<spv::Capability>(pCodePos[1]);
                capabilities.insert(capability);
                break;
            }
        case spv::OpExtension:
            {
                if ((strncmp(reinterpret_cast<const char*>(&pCodePos[1]), "SPV_AMD_shader_ballot",
                    strlen("SPV_AMD_shader_ballot")) == 0) && (pShaderModuleInfo->useSubgroupSize == false))
                {
                    pShaderModuleInfo->useSubgroupSize = true;
                }
                break;
            }
        case spv::OpDPdx:
        case spv::OpDPdy:
        case spv::OpDPdxCoarse:
        case spv::OpDPdyCoarse:
        case spv::OpDPdxFine:
        case spv::OpDPdyFine:
        case spv::OpImageSampleImplicitLod:
        case spv::OpImageSampleDrefImplicitLod:
        case spv::OpImageSampleProjImplicitLod:
        case spv::OpImageSampleProjDrefImplicitLod:
        case spv::OpImageSparseSampleImplicitLod:
        case spv::OpImageSparseSampleProjDrefImplicitLod:
        case spv::OpImageSparseSampleProjImplicitLod:
            {
                pShaderModuleInfo->useHelpInvocation = true;
                break;
            }
        case spv::OpString:
        case spv::OpSource:
        case spv::OpSourceContinued:
        case spv::OpSourceExtension:
        case spv::OpName:
        case spv::OpMemberName:
        case spv::OpLine:
        case spv::OpNop:
        case spv::OpNoLine:
        case spv::OpModuleProcessed:
            {
                pShaderModuleInfo->debugInfoSize += wordCount * sizeof(uint32_t);
                break;
            }
        default:
            {
                break;
            }
        }
        pCodePos += wordCount;
    }

    if (capabilities.find(spv::CapabilityVariablePointersStorageBuffer) != capabilities.end())
    {
        pShaderModuleInfo->enableVarPtrStorageBuf = true;
    }

    if (capabilities.find(spv::CapabilityVariablePointers) != capabilities.end())
    {
        pShaderModuleInfo->enableVarPtr = true;
    }

    if ((pShaderModuleInfo->useSubgroupSize == false) &&
        ((capabilities.find(spv::CapabilityGroupNonUniform) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformVote) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformArithmetic) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformBallot) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformShuffle) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformShuffleRelative) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformClustered) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroupNonUniformQuad) != capabilities.end()) ||
        (capabilities.find(spv::CapabilitySubgroupBallotKHR) != capabilities.end()) ||
        (capabilities.find(spv::CapabilitySubgroupVoteKHR) != capabilities.end()) ||
        (capabilities.find(spv::CapabilityGroups) != capabilities.end())))
    {
        pShaderModuleInfo->useSubgroupSize = true;
    }

    return result;
}

// =====================================================================================================================
// Removes all debug instructions for SPIR-V binary.
void Compiler::TrimSpirvDebugInfo(
    const BinaryData* pSpvBin,   // [in] SPIR-V binay code
    uint32_t          bufferSize,    // Output buffer size in bytes
    void*             pTrimSpvBin)     // [out] Trimmed SPIR-V binary code
{
    LLPC_ASSERT(bufferSize > sizeof(SpirvHeader));

    const uint32_t* pCode = reinterpret_cast<const uint32_t*>(pSpvBin->pCode);
    const uint32_t* pEnd = pCode + pSpvBin->codeSize / sizeof(uint32_t);
    const uint32_t* pCodePos = pCode + sizeof(SpirvHeader) / sizeof(uint32_t);

    uint32_t* pTrimEnd = reinterpret_cast<uint32_t*>(VoidPtrInc(pTrimSpvBin, bufferSize));
    LLPC_UNUSED(pTrimEnd);
    uint32_t* pTrimCodePos = reinterpret_cast<uint32_t*>(VoidPtrInc(pTrimSpvBin, sizeof(SpirvHeader)));

    // Copy SPIR-V header
    memcpy(pTrimSpvBin, pCode, sizeof(SpirvHeader));

    // Copy SPIR-V instructions
    while (pCodePos < pEnd)
    {
        uint32_t opCode = (pCodePos[0] & OpCodeMask);
        uint32_t wordCount = (pCodePos[0] >> WordCountShift);
        switch (opCode)
        {
        case spv::OpString:
        case spv::OpSource:
        case spv::OpSourceContinued:
        case spv::OpSourceExtension:
        case spv::OpName:
        case spv::OpMemberName:
        case spv::OpLine:
        case spv::OpNop:
        case spv::OpNoLine:
        case spv::OpModuleProcessed:
            {
                // Skip debug instructions
                break;
            }
        default:
            {
                // Copy other instructions
                LLPC_ASSERT(pCodePos + wordCount <= pEnd);
                LLPC_ASSERT(pTrimCodePos + wordCount <= pTrimEnd);
                memcpy(pTrimCodePos, pCodePos, wordCount * sizeof(uint32_t));
                pTrimCodePos += wordCount;
                break;
            }
        }

        pCodePos += wordCount;
    }

    LLPC_ASSERT(pTrimCodePos == pTrimEnd);
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
        typename ElfReader<Elf64>::SectionBuffer* pSection = nullptr;
        bool isCompute = false;
        Result result = reader.GetSectionDataBySectionIndex(secIdx, &pSection);
        LLPC_ASSERT(result == Result::Success);
        LLPC_UNUSED(result);

        if (strcmp(pSection->pName, NoteName) == 0)
        {
            uint32_t offset = 0;
            const uint32_t noteHeaderSize = sizeof(NoteHeader) - 8;
            while (offset < pSection->secHead.sh_size)
            {
                const NoteHeader* pNode = reinterpret_cast<const NoteHeader*>(pSection->pData + offset);
                if (strncmp(pNode->name, "AMD", pNode->nameSize) == 0 &&
                    (pNode->type == Util::Abi::PipelineAbiNoteType::LegacyMetadata))
                {
                    const uint32_t configCount = pNode->descSize / sizeof(Util::Abi::PalMetadataNoteEntry);
                    auto pConfig = reinterpret_cast<const Util::Abi::PalMetadataNoteEntry*>(
                                   pSection->pData + offset +
                                     noteHeaderSize + Pow2Align(pNode->nameSize, sizeof(uint32_t)));
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
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 432
                else if (pNode->type == Util::Abi::PipelineAbiNoteType::PalMetadata)
                {
                    // Msgpack metadata.
                    msgpack::Document document;
                    if (document.readFromBlob(StringRef(reinterpret_cast<const char*>(pSection->pData + offset +
                                                            noteHeaderSize + Pow2Align(pNode->nameSize,
                                                          sizeof(uint32_t))),
                                                        pNode->descSize),
                                              false))
                    {
                        auto hwStages = document.getRoot().getMap(true)[Util::Abi::PalCodeObjectMetadataKey::Pipelines]
                                                          .getArray(true)[0]
                                                          .getMap(true)[Util::Abi::PipelineMetadataKey::HardwareStages]
                                                          .getMap(true);
                        auto stageIt = hwStages.find(".ps");
                        if (stageIt == hwStages.end())
                        {
                            stageIt = hwStages.find(".cs");
                            isCompute = true;
                        }

                        if (stageIt != hwStages.end())
                        {
                            auto hwStage = stageIt->second.getMap(true);
                            auto node = hwStage[Util::Abi::HardwareStageMetadataKey::VgprCount];

                            if (node.getKind() == msgpack::Type::UInt)
                            {
                                pPipelineStats->numUsedVgprs = node.getUInt();
                            }

                            node = hwStage[Util::Abi::HardwareStageMetadataKey::VgprLimit];
                            if (node.getKind() == msgpack::Type::UInt)
                            {
                                pPipelineStats->numAvailVgprs = node.getUInt();
                            }

                            node = hwStage[Util::Abi::PipelineMetadataKey::ScratchMemorySize];
                            if (node.getKind() == msgpack::Type::UInt)
                            {
                                pPipelineStats->useScratchBuffer = (node.getUInt() > 0);
                            }
                        }
                    }
                }
#endif

                offset += noteHeaderSize + Pow2Align(pNode->nameSize, sizeof(uint32_t)) +
                          Pow2Align(pNode->descSize, sizeof(uint32_t));
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

// =====================================================================================================================
// Lookup in the shader caches with the given pipeline hash code.
// It will try App's pipelince cache first if that's available.
// Then try on the internal shader cache next if it misses.
//
// NOTE: Only two items in the array of shader caches; one for App's pipeline cache and one for internal cache
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

// =====================================================================================================================
// Update the shader caches with the given entry handle, based on the "bInsert" flag.
void Compiler::UpdateShaderCaches(
    bool                             bInsert,           // [in] To insert data or reset the shader caches
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
            if (bInsert)
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

// =====================================================================================================================
// Builds hash code from input context for per shader stage cache
void Compiler::BuildShaderCacheHash(
    Context*         pContext,           // [in] Acquired context
    MetroHash::Hash* pFragmentHash,      // [out] Hash code of fragment shader
    MetroHash::Hash* pNonFragmentHash)   // [out] Hash code of all non-fragment shader
{
    MetroHash64 fragmentHasher;
    MetroHash64 nonFragmentHasher;
    auto stageMask = pContext->GetShaderStageMask();
    auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());

    // Build hash per shader stage
    for (auto stage = ShaderStageVertex; stage < ShaderStageGfxCount; stage = static_cast<ShaderStage>(stage + 1))
    {
        if ((stageMask & ShaderStageToMask(stage)) == 0)
        {
            continue;
        }

        auto pShaderInfo = pContext->GetPipelineShaderInfo(stage);
        auto pResUsage = pContext->GetShaderResourceUsage(stage);
        MetroHash64 hasher;

        // Update common shader info
        PipelineDumper::UpdateHashForPipelineShaderInfo(stage, pShaderInfo, true, &hasher);
        hasher.Update(pPipelineInfo->iaState.deviceIndex);

        // Update input/output usage
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.inputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.outputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.perPatchInputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.perPatchOutputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.builtInInputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.builtInOutputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.perPatchBuiltInInputLocMap, &hasher);
        PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.perPatchBuiltInOutputLocMap, &hasher);

        if (stage == ShaderStageGeometry)
        {
            // NOTE: For geometry shader, copy shader will use this special map info (from built-in outputs to
            // locations of generic outputs). We have to add it to shader hash calculation.
            PipelineDumper::UpdateHashForMap(pResUsage->inOutUsage.gs.builtInOutLocs, &hasher);
        }

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
        PipelineDumper::UpdateHashForFragmentState(pPipelineInfo, &fragmentHasher);
        fragmentHasher.Finalize(pFragmentHash->bytes);
    }

    if (stageMask & ~ShaderStageToMask(ShaderStageFragment))
    {
        PipelineDumper::UpdateHashForNonFragmentState(pPipelineInfo, true, &nonFragmentHasher);
        nonFragmentHasher.Finalize(pNonFragmentHash->bytes);
    }
}

// =====================================================================================================================
// Merge ELF binary of fragment shader and ELF binary of non-fragment shaders into single ELF binary
void Compiler::MergeElfBinary(
    Context*          pContext,        // [in] Pipeline context
    const BinaryData* pFragmentElf,    // [in] ELF binary of fragment shader
    const BinaryData* pNonFragmentElf, // [in] ELF binary of non-fragment shaders
    ElfPackage*       pPipelineElf)    // [out] Final ELF binary
{
    auto FragmentIsaSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsMainEntry)];
    auto FragmentIntrlTblSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsShdrIntrlTblPtr)];
    auto FragmentDisassemblySymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsDisassembly)];
    auto FragmentIntrlDataSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsShdrIntrlData)];
    auto FragmentAmdIlSymbolName =
        Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(Util::Abi::PipelineSymbolType::PsAmdIl)];

    ElfWriter<Elf64> writer(m_gfxIp);
    ElfReader<Elf64> reader(m_gfxIp);

    // Load ELF binary
    auto result = writer.ReadFromBuffer(pNonFragmentElf->pCode, pNonFragmentElf->codeSize);
    LLPC_ASSERT(result == Result::Success);

    auto fragmentCodesize = pFragmentElf->codeSize;
    result = reader.ReadFromBuffer(pFragmentElf->pCode, &fragmentCodesize);
    LLPC_ASSERT(result == Result::Success);

    // Merge GPU ISA code
    const ElfSectionBuffer<Elf64::SectionHeader>* pNonFragmentTextSection = nullptr;
    ElfSectionBuffer<Elf64::SectionHeader>* pFragmentTextSection = nullptr;
    std::vector<ElfSymbol> fragmentSymbols;
    std::vector<ElfSymbol*> nonFragmentSymbols;

    auto fragmentTextSecIndex = reader.GetSectionIndex(TextName);
    auto nonFragmentSecIndex = writer.GetSectionIndex(TextName);
    reader.GetSectionDataBySectionIndex(fragmentTextSecIndex, &pFragmentTextSection);
    reader.GetSymbolsBySectionIndex(fragmentTextSecIndex, fragmentSymbols);

    writer.GetSectionDataBySectionIndex(nonFragmentSecIndex, &pNonFragmentTextSection);
    writer.GetSymbolsBySectionIndex(nonFragmentSecIndex, nonFragmentSymbols);
    ElfSymbol* pFragmentIsaSymbol = nullptr;
    ElfSymbol* pNonFragmentIsaSymbol = nullptr;
    std::string firstIsaSymbolName;

    for (auto pSymbol : nonFragmentSymbols)
    {
        if (firstIsaSymbolName.empty())
        {
            // NOTE: Entry name of the first shader stage is missed in disassembly section, we have to add it back
            // when merge disassembly sections.
            if (strncmp(pSymbol->pSymName, "_amdgpu_", strlen("_amdgpu_")) == 0)
            {
                firstIsaSymbolName = pSymbol->pSymName;
            }
        }

        if (strcmp(pSymbol->pSymName, FragmentIsaSymbolName) == 0)
        {
            pNonFragmentIsaSymbol = pSymbol;
        }

        if (pNonFragmentIsaSymbol == nullptr)
        {
            continue;
        }

        // Reset all symbols after _amdgpu_ps_main
        pSymbol->secIdx = InvalidValue;
    }

    size_t isaOffset = (pNonFragmentIsaSymbol == nullptr) ?
                       Pow2Align(pNonFragmentTextSection->secHead.sh_size, 0x100) :
                       pNonFragmentIsaSymbol->value;
    for (auto& fragmentSymbol : fragmentSymbols)
    {
        if (strcmp(fragmentSymbol.pSymName, FragmentIsaSymbolName) == 0)
        {
            // Modify ISA code
            pFragmentIsaSymbol = &fragmentSymbol;
            ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
            writer.MergeSection(pNonFragmentTextSection,
                                isaOffset,
                                nullptr,
                                pFragmentTextSection,
                                pFragmentIsaSymbol->value,
                                nullptr,
                                &newSection);
            writer.SetSection(nonFragmentSecIndex, &newSection);
        }

        if (pFragmentIsaSymbol == nullptr)
        {
            continue;
        }

        // Update fragment shader related symbols
        ElfSymbol* pSymbol = nullptr;
        pSymbol = writer.GetSymbol(fragmentSymbol.pSymName);
        pSymbol->secIdx = nonFragmentSecIndex;
        pSymbol->pSecName = nullptr;
        pSymbol->value = isaOffset + fragmentSymbol.value - pFragmentIsaSymbol->value;
        pSymbol->size = fragmentSymbol.size;
    }

    // LLPC doesn't use per pipeline internal table, and LLVM backend doesn't add symbols for disassembly info.
    LLPC_ASSERT((reader.IsValidSymbol(FragmentIntrlTblSymbolName) == false) &&
                (reader.IsValidSymbol(FragmentDisassemblySymbolName) == false) &&
                (reader.IsValidSymbol(FragmentIntrlDataSymbolName) == false) &&
                (reader.IsValidSymbol(FragmentAmdIlSymbolName) == false));
    LLPC_UNUSED(FragmentIntrlTblSymbolName);
    LLPC_UNUSED(FragmentDisassemblySymbolName);
    LLPC_UNUSED(FragmentIntrlDataSymbolName);
    LLPC_UNUSED(FragmentAmdIlSymbolName);

    // Merge ISA disassemble
    auto fragmentDisassemblySecIndex = reader.GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
    auto nonFragmentDisassemblySecIndex = writer.GetSectionIndex(Util::Abi::AmdGpuDisassemblyName);
    ElfSectionBuffer<Elf64::SectionHeader>* pFragmentDisassemblySection = nullptr;
    const ElfSectionBuffer<Elf64::SectionHeader>* pNonFragmentDisassemblySection = nullptr;
    reader.GetSectionDataBySectionIndex(fragmentDisassemblySecIndex, &pFragmentDisassemblySection);
    writer.GetSectionDataBySectionIndex(nonFragmentDisassemblySecIndex, &pNonFragmentDisassemblySection);
    if (pNonFragmentDisassemblySection != nullptr)
    {
        LLPC_ASSERT(pFragmentDisassemblySection != nullptr);
        // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
        // text search will be incorrect. It is only needed for ElfReader, ElfWriter always append a null terminator
        // for all section data.
        auto pFragmentDisassemblySectionEnd = pFragmentDisassemblySection->pData +
                                              pFragmentDisassemblySection->secHead.sh_size - 1;
        uint8_t lastChar = *pFragmentDisassemblySectionEnd;
        const_cast<uint8_t*>(pFragmentDisassemblySectionEnd)[0] = '\0';
        auto pFragmentDisassembly = strstr(reinterpret_cast<const char*>(pFragmentDisassemblySection->pData),
                                          FragmentIsaSymbolName);
        const_cast<uint8_t*>(pFragmentDisassemblySectionEnd)[0] = lastChar;

        auto fragmentDisassemblyOffset =
            (pFragmentDisassembly == nullptr) ?
            0 :
            (pFragmentDisassembly - reinterpret_cast<const char*>(pFragmentDisassemblySection->pData));

        auto pDisassemblyEnd = strstr(reinterpret_cast<const char*>(pNonFragmentDisassemblySection->pData),
                                     FragmentIsaSymbolName);
        auto disassemblySize = (pDisassemblyEnd == nullptr) ?
                              pNonFragmentDisassemblySection->secHead.sh_size :
                              pDisassemblyEnd - reinterpret_cast<const char*>(pNonFragmentDisassemblySection->pData);

        ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
        writer.MergeSection(pNonFragmentDisassemblySection,
                            disassemblySize,
                            firstIsaSymbolName.c_str(),
                            pFragmentDisassemblySection,
                            fragmentDisassemblyOffset,
                            FragmentIsaSymbolName,
                            &newSection);
        writer.SetSection(nonFragmentDisassemblySecIndex, &newSection);
    }

    // Merge LLVM IR disassemble
    const std::string LlvmIrSectionName = std::string(Util::Abi::AmdGpuCommentName) + ".llvmir";
    ElfSectionBuffer<Elf64::SectionHeader>* pFragmentLlvmIrSection = nullptr;
    const ElfSectionBuffer<Elf64::SectionHeader>* pNonFragmentLlvmIrSection = nullptr;

    auto fragmentLlvmIrSecIndex = reader.GetSectionIndex(LlvmIrSectionName.c_str());
    auto nonFragmentLlvmIrSecIndex = writer.GetSectionIndex(LlvmIrSectionName.c_str());
    reader.GetSectionDataBySectionIndex(fragmentLlvmIrSecIndex, &pFragmentLlvmIrSection);
    writer.GetSectionDataBySectionIndex(nonFragmentLlvmIrSecIndex, &pNonFragmentLlvmIrSection);

    if (pNonFragmentLlvmIrSection != nullptr)
    {
        LLPC_ASSERT(pFragmentLlvmIrSection != nullptr);

        // NOTE: We have to replace last character with null terminator and restore it afterwards. Otherwise, the
        // text search will be incorrect. It is only needed for ElfReader, ElfWriter always append a null terminator
        // for all section data.
        auto pFragmentLlvmIrSectionEnd = pFragmentLlvmIrSection->pData +
                                         pFragmentLlvmIrSection->secHead.sh_size - 1;
        uint8_t lastChar = *pFragmentLlvmIrSectionEnd;
        const_cast<uint8_t*>(pFragmentLlvmIrSectionEnd)[0] = '\0';
        auto pFragmentLlvmIrStart = strstr(reinterpret_cast<const char*>(pFragmentLlvmIrSection->pData),
                                           FragmentIsaSymbolName);
        const_cast<uint8_t*>(pFragmentLlvmIrSectionEnd)[0] = lastChar;

        auto fragmentLlvmIrOffset =
            (pFragmentLlvmIrStart == nullptr) ?
            0 :
            (pFragmentLlvmIrStart - reinterpret_cast<const char*>(pFragmentLlvmIrSection->pData));

        auto pLlvmIrEnd = strstr(reinterpret_cast<const char*>(pNonFragmentLlvmIrSection->pData),
                                 FragmentIsaSymbolName);
        auto llvmIrSize = (pLlvmIrEnd == nullptr) ?
                          pNonFragmentLlvmIrSection->secHead.sh_size :
                          pLlvmIrEnd - reinterpret_cast<const char*>(pNonFragmentLlvmIrSection->pData);

        ElfSectionBuffer<Elf64::SectionHeader> newSection = {};
        writer.MergeSection(pNonFragmentLlvmIrSection,
                            llvmIrSize,
                            firstIsaSymbolName.c_str(),
                            pFragmentLlvmIrSection,
                            fragmentLlvmIrOffset,
                            FragmentIsaSymbolName,
                            &newSection);
        writer.SetSection(nonFragmentLlvmIrSecIndex, &newSection);
    }

    // Merge PAL metadata
    ElfNote nonFragmentMetaNote = {};
    ElfNote nonFragmentLegacyMetaNote = {};
    nonFragmentMetaNote = writer.GetNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
    nonFragmentLegacyMetaNote = writer.GetNote(Util::Abi::PipelineAbiNoteType::LegacyMetadata);

    if (nonFragmentLegacyMetaNote.pData != nullptr)
    {
        LLPC_ASSERT(nonFragmentMetaNote.pData == nullptr);
        ElfNote fragmentLegacyMetaNote = {};
        ElfNote newLegacyMetaNote = {};
        fragmentLegacyMetaNote = reader.GetNote(Util::Abi::PipelineAbiNoteType::LegacyMetadata);
        writer.MergeLegacyMetaNote(pContext, &nonFragmentLegacyMetaNote, &fragmentLegacyMetaNote, &newLegacyMetaNote);
        writer.SetNote(&newLegacyMetaNote);
    }
    else
    {
        LLPC_ASSERT(nonFragmentMetaNote.pData != nullptr);
        ElfNote fragmentMetaNote = {};
        ElfNote newMetaNote = {};
        fragmentMetaNote = reader.GetNote(Util::Abi::PipelineAbiNoteType::PalMetadata);
        writer.MergeMetaNote(pContext, &nonFragmentMetaNote, &fragmentMetaNote, &newMetaNote);
        writer.SetNote(&newMetaNote);
    }

    writer.WriteToBuffer(pPipelineElf);
}

} // Llpc
