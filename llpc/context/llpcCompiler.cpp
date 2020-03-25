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
 * @file  llpcCompiler.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Compiler.
 ***********************************************************************************************************************
 */
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
#include "llvm/Support/Timer.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"

#include "LLVMSPIRVLib.h"
#include "spirvExt.h"
#include "SPIRVInternal.h"

#include "lgc/llpcBuilder.h"
#include "llpcCompiler.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcGraphicsContext.h"
#include "llpcShaderModuleHelper.h"
#include "llpcElfReader.h"
#include "llpcElfWriter.h"
#include "llpcFile.h"
#include "lgc/llpcPassManager.h"
#include "llpcPipelineDumper.h"
#include "llpcSpirvLower.h"
#include "llpcSpirvLowerResourceCollect.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcTimerProfiler.h"
#include <mutex>
#include <set>
#include <unordered_set>

#ifdef LLPC_ENABLE_SPIRV_OPT
    #define SPVGEN_STATIC_LIB 1
    #include "spvgen.h"
#endif

#define DEBUG_TYPE "llpc-compiler"

using namespace lgc;
using namespace llvm;
using namespace MetroHash;
using namespace SPIRV;
using namespace spv;
using namespace Util;
using namespace Vkgc;

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

// -use-relocatable-shader-elf: Gets LLVM to generate more generic elf files for each shader individually, and LLPC will
// then link those ELF files to generate the compiled pipeline.
opt<bool> UseRelocatableShaderElf("use-relocatable-shader-elf",
                                    desc("The pipeline will be built by building relocatable shader ELF files when "
                                         "possible, and linking them together.  This is a work in progress and should "
                                         "be used with caution."),
                                    init(false));

// -relocatable-shader-elf-limit=<n>: Limits the number of pipelines that will be compiled using relocatable shader ELF.
// This is to be used for debugging by doing a binary search to identify a pipeline that is being miscompiled when using
// relocatable shader ELF modules.
opt<int> RelocatableShaderElfLimit("relocatable-shader-elf-limit", cl::desc("Max number of pipeline compiles that will use "
                                                                   "relocatable shader ELF.  -1 means unlimited."),
                                   init(-1));

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

// -use-builder-recorder
static cl::opt<bool> UseBuilderRecorder("use-builder-recorder",
                                        cl::desc("Do lowering via recording and replaying LLPC builder"),
                                        cl::init(true));

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
    LLPC_ERRS("LLVM FATAL ERROR: " << reason << "\n");
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
        assert(diagInfo.getSeverity() != DS_Error);
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

    // Initialize passes so they can be referenced by -print-after etc.
    InitializeLowerPasses(*PassRegistry::getPassRegistry());
    BuilderContext::Initialize();

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
                llvm_unreachable("Should never be called!");
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
        assert(*ppCompiler != nullptr);

        if (EnableOuts())
        {
            // LLPC_OUTS is enabled. Ensure it is enabled in LGC (the middle-end) too.
            BuilderContext::SetLlpcOuts(&outs());
        }
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
    BufDataFormat dfmt = PipelineContext::MapVkFormat(format, false).first;
    return (dfmt != BufDataFormatInvalid);
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
        llvm_unreachable("Should never be called!");
#endif
    }

    m_shaderCache = ShaderCacheManager::GetShaderCacheManager()->GetShaderCacheObject(&createInfo, &auxCreateInfo);

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
    // For trimming debug info
    uint8_t* pTrimmedCode = nullptr;

    ElfPackage moduleBinary;
    raw_svector_ostream moduleBinaryStream(moduleBinary);
    std::vector<ShaderEntryName> entryNames;
    SmallVector<ShaderModuleEntryData, 4> moduleEntryDatas;
    SmallVector<ShaderModuleEntry, 4> moduleEntries;
    SmallVector<FsOutInfo, 4> fsOutInfos;
    std::map<uint32_t, std::vector<ResourceNodeData>> entryResourceNodeDatas; // Map entry ID and resourceNodeData

    ShaderEntryState cacheEntryState = ShaderEntryState::New;
    CacheEntryHandle hEntry = nullptr;

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
            pTrimmedCode = new uint8_t[moduleDataEx.common.binCode.codeSize];
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
        enableOpt = enableOpt || pShaderInfo->options.enableOpt;
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
                pContext->SetBuilder(pContext->GetBuilderContext()->CreateBuilder(nullptr, true));

                for (uint32_t i = 0; i < entryNames.size(); ++i)
                {
                    ShaderModuleEntry moduleEntry = {};
                    ShaderModuleEntryData moduleEntryData = {};

                    moduleEntryData.pShaderEntry = &moduleEntry;
                    moduleEntryData.stage = entryNames[i].stage;
                    moduleEntryData.pEntryName = entryNames[i].pName;
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

                    uint32_t passIndex = 0;
                    std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create());
                    lowerPassMgr->SetPassIndex(&passIndex);

                    // Set the shader stage in the Builder.
                    pContext->GetBuilder()->SetShaderStage(
                                              GetLgcShaderStage(static_cast<ShaderStage>(entryNames[i].stage)));

                    // Start timer for translate.
                    timerProfiler.AddTimerStartStopPass(&*lowerPassMgr, TimerTranslate, true);

                    // SPIR-V translation, then dump the result.
                    PipelineShaderInfo shaderInfo = {};
                    shaderInfo.pModuleData = &moduleDataEx.common;
                    shaderInfo.entryStage = entryNames[i].stage;
                    shaderInfo.pEntryTarget = entryNames[i].pName;
                    lowerPassMgr->add(CreateSpirvLowerTranslator(static_cast<ShaderStage>(entryNames[i].stage),
                                                                &shaderInfo));
                    bool collectDetailUsage = ((entryNames[i].stage == ShaderStageFragment) ||
                                               (entryNames[i].stage == ShaderStageCompute)) ? true : false;
                    auto pResCollectPass = static_cast<SpirvLowerResourceCollect*>(
                                           CreateSpirvLowerResourceCollect(collectDetailUsage));
                    lowerPassMgr->add(pResCollectPass);
                    if (EnableOuts())
                    {
                        lowerPassMgr->add(createPrintModulePass(outs(), "\n"
                            "===============================================================================\n"
                            "// LLPC SPIRV-to-LLVM translation results\n"));
                    }

                    // Stop timer for translate.
                    timerProfiler.AddTimerStartStopPass(&*lowerPassMgr, TimerTranslate, false);

                    // Per-shader SPIR-V lowering passes.
                    SpirvLower::AddPasses(pContext,
                                          static_cast<ShaderStage>(entryNames[i].stage),
                                          *lowerPassMgr,
                                          timerProfiler.GetTimer(TimerLower),
                                          cl::ForceLoopUnrollCount);

                    lowerPassMgr->add(createBitcodeWriterPass(moduleBinaryStream));

                    // Run the passes.
                    bool success = RunPasses(&*lowerPassMgr, pModule);
                    if (success == false)
                    {
                        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
                        result = Result::ErrorInvalidShader;
                        delete pModule;
                        break;
                    }

                    moduleEntry.entrySize = moduleBinary.size() - moduleEntry.entryOffset;

                    moduleEntry.passIndex = passIndex;
                    if (pResCollectPass->DetailUsageValid())
                    {
                        auto& resNodeDatas = pResCollectPass->GetResourceNodeDatas();
                        moduleEntryData.resNodeDataCount = resNodeDatas.size();
                        for (auto resNodeData : resNodeDatas)
                        {
                            ResourceNodeData data = {};
                            data.type = resNodeData.second;
                            data.set = resNodeData.first.value.set;
                            data.binding = resNodeData.first.value.binding;
                            data.arraySize = resNodeData.first.value.arraySize;
                            entryResourceNodeDatas[i].push_back(data);
                        }

                        moduleEntryData.pushConstSize = pResCollectPass->GetPushConstSize();
                        auto& fsOutInfosFromPass = pResCollectPass->GetFsOutInfos();
                        for (auto& fsOutInfo : fsOutInfosFromPass)
                        {
                            fsOutInfos.push_back(fsOutInfo);
                        }
                    }
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
    uint32_t totalNodeCount = 0;
    if (result == Result::Success)
    {
        if (pShaderInfo->pfnOutputAlloc != nullptr)
        {
            if (cacheEntryState != ShaderEntryState::Ready)
            {
                for (uint32_t i = 0; i < moduleDataEx.extra.entryCount; ++i)
                {
                    totalNodeCount += moduleEntryDatas[i].resNodeDataCount;
                }

                allocSize = sizeof(ShaderModuleDataEx) +
                    moduleDataEx.common.binCode.codeSize +
                    (moduleDataEx.extra.entryCount * (sizeof(ShaderModuleEntryData) + sizeof(ShaderModuleEntry))) +
                    totalNodeCount * sizeof(ResourceNodeData) +
                    fsOutInfos.size() * sizeof(FsOutInfo);
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
        // Memory layout of pAllocBuf: ShaderModuleDataEx | ShaderModuleEntryData | ShaderModuleEntry | binCode
        //                             | Resource nodes | FsOutInfo
        ShaderModuleDataEx* pModuleDataEx = reinterpret_cast<ShaderModuleDataEx*>(pAllocBuf);

        ShaderModuleEntryData* pEntryData = &pModuleDataEx->extra.entryDatas[0];
        if (cacheEntryState != ShaderEntryState::Ready)
        {
            // Copy module data
            memcpy(pModuleDataEx, &moduleDataEx, sizeof(moduleDataEx));
            pModuleDataEx->common.binCode.pCode = nullptr;

            size_t entryOffset = 0, codeOffset = 0, resNodeOffset = 0, fsOutInfoOffset = 0;

            entryOffset = sizeof(ShaderModuleDataEx) +
                                 moduleDataEx.extra.entryCount * sizeof(ShaderModuleEntryData);
            codeOffset = entryOffset + moduleDataEx.extra.entryCount * sizeof(ShaderModuleEntry);
            resNodeOffset = codeOffset + moduleDataEx.common.binCode.codeSize;
            fsOutInfoOffset = resNodeOffset + totalNodeCount * sizeof(ResourceNodeData);
            pModuleDataEx->codeOffset = codeOffset;
            pModuleDataEx->entryOffset = entryOffset;
            pModuleDataEx->resNodeOffset   = resNodeOffset;
            pModuleDataEx->fsOutInfoOffset   = fsOutInfoOffset;
        }
        else
        {
            memcpy(pModuleDataEx, pCacheData, allocSize);
        }

        ShaderModuleEntry* pEntry = reinterpret_cast<ShaderModuleEntry*>(VoidPtrInc(pAllocBuf,
                                                                         pModuleDataEx->entryOffset));
        ResourceNodeData* pResNodeData = reinterpret_cast<ResourceNodeData*>(VoidPtrInc(pAllocBuf,
                                                                             pModuleDataEx->resNodeOffset));
        FsOutInfo* pFsOutInfo = reinterpret_cast<FsOutInfo*>(VoidPtrInc(pAllocBuf,
                                                                        pModuleDataEx->fsOutInfoOffset));
        void* pCode = VoidPtrInc(pAllocBuf, pModuleDataEx->codeOffset);

        if (cacheEntryState != ShaderEntryState::Ready)
        {
            // Copy entry info
            for (uint32_t i = 0; i < moduleDataEx.extra.entryCount; ++i)
            {
                pEntryData[i] = moduleEntryDatas[i];
                // Set module entry pointer
                pEntryData[i].pShaderEntry = &pEntry[i];
                // Copy module entry
                memcpy(pEntryData[i].pShaderEntry, &moduleEntries[i], sizeof(ShaderModuleEntry));
                // Copy resourceNodeData and set resource node pointer
                memcpy(pResNodeData, &entryResourceNodeDatas[i][0],
                       moduleEntryDatas[i].resNodeDataCount* sizeof(ResourceNodeData));
                pEntryData[i].pResNodeDatas = pResNodeData;
                pEntryData[i].resNodeDataCount = moduleEntryDatas[i].resNodeDataCount;
                pResNodeData += moduleEntryDatas[i].resNodeDataCount;
            }

            // Copy binary code
            memcpy(pCode, moduleDataEx.common.binCode.pCode, moduleDataEx.common.binCode.codeSize);
            // Destory the temporary module code
            if(pTrimmedCode != nullptr)
            {
                delete[] pTrimmedCode;
                pTrimmedCode = nullptr;
                moduleDataEx.common.binCode.pCode = nullptr;
            }

            // Copy fragment shader output variables
            pModuleDataEx->extra.fsOutInfoCount = fsOutInfos.size();
            if (fsOutInfos.size() > 0)
            {
                memcpy(pFsOutInfo, &fsOutInfos[0], fsOutInfos.size() * sizeof(FsOutInfo));
            }
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
            // Update the pointers
            for (uint32_t i = 0; i < moduleDataEx.extra.entryCount; ++i)
            {
                pEntryData[i].pShaderEntry = &pEntry[i];
                pEntryData[i].pResNodeDatas = pResNodeData;
                pResNodeData += pEntryData[i].resNodeDataCount;
            }
        }
        pModuleDataEx->common.binCode.pCode = pCode;
        pModuleDataEx->extra.pFsOutInfos = pFsOutInfo;
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
// Builds a pipeline by building relocatable elf files and linking them together.  The relocatable elf files will be
// cached for future use.
Result Compiler::BuildPipelineWithRelocatableElf(
    Context*                            pContext,                   // [in] Acquired context
    ArrayRef<const PipelineShaderInfo*> shaderInfo,                 // Shader info of this pipeline
    uint32_t                            forceLoopUnrollCount,       // Force loop unroll count (0 means disable)
    ElfPackage*                         pPipelineElf)               // [out] Output Elf package
{
    Result result = Result::Success;

    // Merge the user data once for all stages.
    pContext->GetPipelineContext()->DoUserDataNodeMerge();
    uint32_t originalShaderStageMask = pContext->GetPipelineContext()->GetShaderStageMask();
    pContext->GetBuilderContext()->SetBuildRelocatableElf(true);

    ElfPackage elf[ShaderStageNativeStageCount];
    for (uint32_t stage = 0; (stage < shaderInfo.size()) && (result == Result::Success); ++stage)
    {
        if (shaderInfo[stage] == nullptr || shaderInfo[stage]->pModuleData == nullptr)
        {
            continue;
        }

        pContext->GetPipelineContext()->SetShaderStageMask(ShaderStageToMask(static_cast<ShaderStage>(stage)));

        // Check the cache for the relocatable shader for this stage.
        MetroHash::Hash cacheHash = {};
        IShaderCache* pUserShaderCache = nullptr;
        if (pContext->IsGraphics())
        {
            auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
            cacheHash = PipelineDumper::GenerateHashForGraphicsPipeline(pPipelineInfo, true, stage);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
            pUserShaderCache = pPipelineInfo->pShaderCache;
#endif
        }
        else
        {
            auto pPipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo*>(pContext->GetPipelineBuildInfo());
            cacheHash = PipelineDumper::GenerateHashForComputePipeline(pPipelineInfo, true);
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
            pUserShaderCache = pPipelineInfo->pShaderCache;
#endif
        }

        ShaderEntryState cacheEntryState  = ShaderEntryState::New;
        BinaryData elfBin = {};

        ShaderCache* pShaderCache;
        CacheEntryHandle hEntry;
        cacheEntryState = LookUpShaderCaches(pUserShaderCache, &cacheHash, &elfBin, &pShaderCache, &hEntry);

        if (cacheEntryState == ShaderEntryState::Ready) {
            auto pData = reinterpret_cast<const char*>(elfBin.pCode);
            elf[stage].assign(pData, pData + elfBin.codeSize);
            continue;
        }

        // There was a cache miss, so we need to build the relocatable shader for
        // this stage.
        const PipelineShaderInfo* singleStageShaderInfo[ShaderStageNativeStageCount] =
        {
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        singleStageShaderInfo[stage] = shaderInfo[stage];

        result = BuildPipelineInternal(pContext, singleStageShaderInfo, forceLoopUnrollCount, &elf[stage]);

        // Add the result to the cache.
        if (result == Result::Success)
        {
            elfBin.codeSize = elf[stage].size();
            elfBin.pCode = elf[stage].data();
        }
        UpdateShaderCache((result == Result::Success), &elfBin, pShaderCache, hEntry);
    }
    pContext->GetPipelineContext()->SetShaderStageMask(originalShaderStageMask);
    pContext->GetBuilderContext()->SetBuildRelocatableElf(false);

    // Link the relocatable shaders into a single pipeline elf file.
    LinkRelocatableShaderElf(elf, pPipelineElf, pContext);

    return result;
}

// =====================================================================================================================
// Returns true if a graphics pipeline can be built out of the given shader info.
bool Compiler::CanUseRelocatableGraphicsShaderElf(
    const ArrayRef<const PipelineShaderInfo*>& shaderInfo  // Shader info for the pipeline to be built
    ) const
{
    if (!cl::UseRelocatableShaderElf) {
        return false;
    }

    bool useRelocatableShaderElf = true;
    for (uint32_t stage = 0; stage < shaderInfo.size(); ++stage)
    {
        if (stage != ShaderStageVertex && stage != ShaderStageFragment)
        {
            if ((shaderInfo[stage] != nullptr) && (shaderInfo[stage]->pModuleData != nullptr))
            {
                useRelocatableShaderElf = false;
            }
        }
        else if (shaderInfo[stage] == nullptr || shaderInfo[stage]->pModuleData == nullptr)
        {
            // TODO: Generate pass-through shaders when the fragment or vertex shaders are missing.
            useRelocatableShaderElf = false;
        }
    }

    if (useRelocatableShaderElf && shaderInfo[0] != nullptr)
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(shaderInfo[0]->pModuleData);
        if ((pModuleData != nullptr) && (pModuleData->binType != BinaryType::Spirv))
        {
            useRelocatableShaderElf = false;
        }
    }

    if (useRelocatableShaderElf && cl::RelocatableShaderElfLimit != -1)
    {
        static uint32_t relocatableElfCounter = 0;
        if (relocatableElfCounter >= cl::RelocatableShaderElfLimit)
        {
            useRelocatableShaderElf = false;
        }
        else
        {
            ++relocatableElfCounter;
        }
    }
    return useRelocatableShaderElf;
}

// =====================================================================================================================
// Returns true if a compute pipeline can be built out of the given shader info.
bool Compiler::CanUseRelocatableComputeShaderElf(
    const PipelineShaderInfo* pShaderInfo   // Shader info for the pipeline to be built
    ) const
{
    if (!llvm::cl::UseRelocatableShaderElf)
    {
        return false;
    }

    bool useRelocatableShaderElf = true;
    if (useRelocatableShaderElf && pShaderInfo != nullptr)
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
        if ((pModuleData != nullptr) && (pModuleData->binType != BinaryType::Spirv))
        {
            useRelocatableShaderElf = false;
        }
    }

    if (useRelocatableShaderElf && cl::RelocatableShaderElfLimit != -1)
    {
        static uint32_t relocatableElfCounter = 0;
        if (relocatableElfCounter >= cl::RelocatableShaderElfLimit)
        {
            useRelocatableShaderElf = false;
        }
        else
        {
            ++relocatableElfCounter;
        }
    }
    return useRelocatableShaderElf;
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
    const PipelineShaderInfo* pFragmentShaderInfo = nullptr;
    TimerProfiler timerProfiler(pContext->GetPiplineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);
    bool buildingRelocatableElf = pContext->GetBuilderContext()->BuildingRelocatableElf();

    pContext->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>());

    // Set a couple of pipeline options for front-end use.
    // TODO: The front-end should not be using pipeline options.
    pContext->SetScalarBlockLayout(pContext->GetPipelineContext()->GetPipelineOptions()->scalarBlockLayout);
    pContext->SetRobustBufferAccess(pContext->GetPipelineContext()->GetPipelineOptions()->robustBufferAccess);

    if (!buildingRelocatableElf)
    {
        // Merge user data for shader stages into one.
        pContext->GetPipelineContext()->DoUserDataNodeMerge();
    }

    // Set up middle-end objects.
    BuilderContext* pBuilderContext = pContext->GetBuilderContext();
    std::unique_ptr<Pipeline> pipeline(pBuilderContext->CreatePipeline());
    pContext->GetPipelineContext()->SetPipelineState(&*pipeline);
    pContext->SetBuilder(pBuilderContext->CreateBuilder(&*pipeline, UseBuilderRecorder));

    std::unique_ptr<Module> pipelineModule;

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
            pipelineModule.reset(pContext->LoadLibary(&pModuleData->binCode).release());
        }
    }

    // If not IR input, run the per-shader passes, including SPIR-V translation, and then link the modules
    // into a single pipeline module.
    if (pipelineModule == nullptr)
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

                assert(pShaderInfo->pEntryTarget != nullptr);
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
                pModule = new Module((Twine("llpc") +
                                     GetShaderStageName(pShaderInfo->entryStage)).str() +
                                     std::to_string(GetModuleIdByIndex(shaderIndex)), *pContext);
            }

            modules[shaderIndex] = pModule;
            pContext->SetModuleTargetMachine(pModule);
        }

        for (uint32_t shaderIndex = 0; (shaderIndex < shaderInfo.size()) && (result == Result::Success); ++shaderIndex)
        {
            const PipelineShaderInfo* pShaderInfo = shaderInfo[shaderIndex];
            ShaderStage entryStage = (pShaderInfo != nullptr) ? pShaderInfo->entryStage : ShaderStageInvalid;

            if (entryStage == ShaderStageFragment)
            {
                pFragmentShaderInfo = pShaderInfo;
            }
            if ((pShaderInfo == nullptr) ||
                (pShaderInfo->pModuleData == nullptr) ||
                (stageSkipMask & ShaderStageToMask(entryStage)))
            {
                continue;
            }

            std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create());
            lowerPassMgr->SetPassIndex(&passIndex);

            // Set the shader stage in the Builder.
            pContext->GetBuilder()->SetShaderStage(GetLgcShaderStage(entryStage));

            // Start timer for translate.
            timerProfiler.AddTimerStartStopPass(&*lowerPassMgr, TimerTranslate, true);

            // SPIR-V translation, then dump the result.
            lowerPassMgr->add(CreateSpirvLowerTranslator(entryStage, pShaderInfo));
            if (EnableOuts())
            {
                lowerPassMgr->add(createPrintModulePass(outs(), "\n"
                            "===============================================================================\n"
                            "// LLPC SPIRV-to-LLVM translation results\n"));
            }
            // Stop timer for translate.
            timerProfiler.AddTimerStartStopPass(&*lowerPassMgr, TimerTranslate, false);

            // Run the passes.
            bool success = RunPasses(&*lowerPassMgr, modules[shaderIndex]);
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
            ShaderStage entryStage = (pShaderInfo != nullptr) ? pShaderInfo->entryStage : ShaderStageInvalid;
            if ((pShaderInfo == nullptr) ||
                (pShaderInfo->pModuleData == nullptr) ||
                (stageSkipMask & ShaderStageToMask(entryStage)))
            {
                continue;
            }

            pContext->GetBuilder()->SetShaderStage(GetLgcShaderStage(entryStage));
            std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create());
            lowerPassMgr->SetPassIndex(&passIndex);

            SpirvLower::AddPasses(pContext,
                                  entryStage,
                                  *lowerPassMgr,
                                  timerProfiler.GetTimer(TimerLower),
                                  forceLoopUnrollCount);
            // Run the passes.
            bool success = RunPasses(&*lowerPassMgr, modules[shaderIndex]);
            if (success == false)
            {
                LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
                result = Result::ErrorInvalidShader;
            }
        }

        // Link the shader modules into a single pipeline module.
        pipelineModule.reset(pipeline->Link(modules));
        if (pipelineModule == nullptr)
        {
            LLPC_ERRS("Failed to link shader modules into pipeline module\n");
            result = Result::ErrorInvalidShader;
        }
    }

    // Set up function to check shader cache.
    GraphicsShaderCacheChecker graphicsShaderCacheChecker(this, pContext);

    Pipeline::CheckShaderCacheFunc checkShaderCacheFunc =
            [&graphicsShaderCacheChecker](
        const Module*               pModule,      // [in] Module
        uint32_t                    stageMask,    // Shader stage mask
        ArrayRef<ArrayRef<uint8_t>> stageHashes)  // Per-stage hash of in/out usage
    {
        return graphicsShaderCacheChecker.Check(pModule, stageMask, stageHashes);
    };

    // Only enable per stage cache for full graphic pipeline
    bool checkPerStageCache = cl::EnablePerStageCache && pContext->IsGraphics() &&
                              !buildingRelocatableElf &&
                              (pContext->GetShaderStageMask() &
                               (ShaderStageToMask(ShaderStageVertex) | ShaderStageToMask(ShaderStageFragment)));
    if (checkPerStageCache == false)
    {
        checkShaderCacheFunc = nullptr;
    }

    // Generate pipeline.
    raw_svector_ostream elfStream(*pPipelineElf);

    if (result == Result::Success)
    {
        result = Result::ErrorInvalidShader;
#if LLPC_ENABLE_EXCEPTION
        try
#endif
        {
            Timer* timers[] = {
                timerProfiler.GetTimer(TimerPatch),
                timerProfiler.GetTimer(TimerOpt),
                timerProfiler.GetTimer(TimerCodeGen),
            };

            pipeline->Generate(std::move(pipelineModule), elfStream, checkShaderCacheFunc, timers);
            result = Result::Success;
        }
#if LLPC_ENABLE_EXCEPTION
        catch (const char*)
        {
        }
#endif
    }

    if (checkPerStageCache)
    {
        // For graphics, update shader caches with results of compile, and merge ELF outputs if necessary.
        graphicsShaderCacheChecker.UpdateAndMerge(result, pPipelineElf);
    }

    if ((result == Result::Success) &&
        pFragmentShaderInfo &&
        pFragmentShaderInfo->options.updateDescInElf &&
        (pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageFragment)))
    {
        graphicsShaderCacheChecker.UpdateRootUserDateOffset(pPipelineElf);
    }

    pContext->setDiagnosticHandlerCallBack(nullptr);

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

    IShaderCache* pAppCache = nullptr;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    auto pPipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo*>(m_pContext->GetPipelineBuildInfo());
    pAppCache = pPipelineInfo->pShaderCache;
#endif
    if (stageMask & ShaderStageToMask(ShaderStageFragment))
    {
        m_fragmentCacheEntryState = m_pCompiler->LookUpShaderCaches(pAppCache,
                                                                    &fragmentHash,
                                                                    &m_fragmentElf,
                                                                    &m_pFragmentShaderCache,
                                                                    &m_hFragmentEntry);
    }

    if (stageMask & ~ShaderStageToMask(ShaderStageFragment))
    {
        m_nonFragmentCacheEntryState = m_pCompiler->LookUpShaderCaches(pAppCache,
                                                                       &nonFragmentHash,
                                                                       &m_nonFragmentElf,
                                                                       &m_pNonFragmentShaderCache,
                                                                       &m_hNonFragmentEntry);
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
// Update root level descriptor offset for graphics pipeline.
void GraphicsShaderCacheChecker::UpdateRootUserDateOffset(
    ElfPackage*       pPipelineElf)   // [In, Out] ELF that could be from compile or merged
{
    ElfWriter<Elf64> writer(m_pContext->GetGfxIpVersion());
    // Load ELF binary
    auto result = writer.ReadFromBuffer(pPipelineElf->data(), pPipelineElf->size());
    assert(result == Result::Success);
    (void(result)); // unused
    writer.UpdateElfBinary(m_pContext, pPipelineElf);
}

// =====================================================================================================================
// Update shader caches for graphics pipeline from compile result, and merge ELF outputs if necessary.
void GraphicsShaderCacheChecker::UpdateAndMerge(
    Result            result,         // Result of compile
    ElfPackage*       pPipelineElf)   // ELF output of compile, updated to merge ELF from shader cache
{
    // Update the shader cache if required, with the compiled pipeline or with a failure state.
    if (m_fragmentCacheEntryState == ShaderEntryState::Compiling ||
        m_nonFragmentCacheEntryState == ShaderEntryState::Compiling)
    {
        BinaryData pipelineElf = {};
        pipelineElf.codeSize = pPipelineElf->size();
        pipelineElf.pCode = pPipelineElf->data();

        if (m_fragmentCacheEntryState == ShaderEntryState::Compiling)
        {
            m_pCompiler->UpdateShaderCache(result == Result::Success, &pipelineElf, m_pFragmentShaderCache,
                                           m_hFragmentEntry);
        }

        if (m_nonFragmentCacheEntryState == ShaderEntryState::Compiling)
        {
            m_pCompiler->UpdateShaderCache(result == Result::Success, &pipelineElf, m_pNonFragmentShaderCache,
                                           m_hNonFragmentEntry);
        }
    }

    // Now merge ELFs if one or both parts are from the cache. Nothing needs to be merged if we just compiled the full
    // pipeline, as everything is already contained in the single incoming ELF in this case.
    if (result == Result::Success &&
        (m_fragmentCacheEntryState == ShaderEntryState::Ready ||
         m_nonFragmentCacheEntryState == ShaderEntryState::Ready))
    {
        // Move the compiled ELF out of the way.
        ElfPackage compiledPipelineElf = std::move(*pPipelineElf);
        pPipelineElf->clear();

        // Determine where the fragment / non-fragment parts come from (cache or just-compiled).
        BinaryData fragmentElf = {};
        if (m_fragmentCacheEntryState == ShaderEntryState::Ready)
        {
            fragmentElf = m_fragmentElf;
        }
        else
        {
            fragmentElf.pCode = compiledPipelineElf.data();
            fragmentElf.codeSize = compiledPipelineElf.size();
        }

        BinaryData nonFragmentElf = {};
        if (m_nonFragmentCacheEntryState == ShaderEntryState::Ready)
        {
            nonFragmentElf = m_nonFragmentElf;
        }
        else
        {
            nonFragmentElf.pCode = compiledPipelineElf.data();
            nonFragmentElf.codeSize = compiledPipelineElf.size();
        }

        // Merge and store the result in pPipelineElf
        ElfWriter<Elf64> writer(m_pContext->GetGfxIpVersion());
        auto result = writer.ReadFromBuffer(nonFragmentElf.pCode, nonFragmentElf.codeSize);
        assert(result == Result::Success);
        (void(result)); // unused
        writer.MergeElfBinary(m_pContext, &fragmentElf, pPipelineElf);
    }
}

// =====================================================================================================================
// Convert color buffer format to fragment shader export format
// This is not used in a normal compile; it is only used by amdllpc's -check-auto-layout-compatible option.
uint32_t Compiler::ConvertColorBufferFormatToExportFormat(
    const ColorTarget*          pTarget,                // [in] GraphicsPipelineBuildInfo
    const bool                  enableAlphaToCoverage   // whether enalbe AlphaToCoverage
    ) const
{
    Context* pContext = AcquireContext();
    std::unique_ptr<Pipeline> pipeline(pContext->GetBuilderContext()->CreatePipeline());
    ColorExportFormat format = {};
    ColorExportState state = {};
    std::tie(format.dfmt, format.nfmt) = PipelineContext::MapVkFormat(pTarget->format, true);
    format.blendEnable = pTarget->blendEnable;
    format.blendSrcAlphaToColor = pTarget->blendSrcAlphaToColor;
    state.alphaToCoverageEnable = enableAlphaToCoverage;
    pipeline->SetColorExportState(format, state);

    Type* pOutputTy = VectorType::get(Type::getFloatTy(*pContext), countPopulation(pTarget->channelWriteMask));
    uint32_t exportFormat = pipeline->ComputeExportFormat(pOutputTy, 0);

    pipeline.reset(nullptr);
    ReleaseContext(pContext);

    return exportFormat;
}

// =====================================================================================================================
// Build graphics pipeline internally
Result Compiler::BuildGraphicsPipelineInternal(
    GraphicsContext*                    pGraphicsContext,         // [in] Graphics context this graphics pipeline
    ArrayRef<const PipelineShaderInfo*> shaderInfo,               // Shader info of this graphics pipeline
    uint32_t                            forceLoopUnrollCount,     // Force loop unroll count (0 means disable)
    bool                                buildingRelocatableElf,   // Build the pipeline by linking relocatable elf
    ElfPackage*                         pPipelineElf)             // [out] Output Elf package
{
    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pGraphicsContext);

    Result result = Result::Success;
    if (buildingRelocatableElf)
    {
        result = BuildPipelineWithRelocatableElf(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf);
    }
    else
    {
        result = BuildPipelineInternal(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf);
    }
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
        result = ValidatePipelineShaderInfo(shaderInfo[i]);
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
    bool buildingRelocatableElf = CanUseRelocatableGraphicsShaderElf(shaderInfo);
    IShaderCache* pAppCache = nullptr;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    pAppCache = pPipelineInfo->pShaderCache;
#endif
    ShaderCache* pShaderCache = nullptr;
    CacheEntryHandle hEntry = nullptr;

    if (!buildingRelocatableElf)
    {
        cacheEntryState = LookUpShaderCaches(pAppCache, &cacheHash, &elfBin, &pShaderCache, &hEntry);
    }
    else
    {
        cacheEntryState = ShaderEntryState::Compiling;
    }

    ElfPackage candidateElf;

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        uint32_t                      forceLoopUnrollCount = cl::ForceLoopUnrollCount;

        GraphicsContext graphicsContext(m_gfxIp,
                                        pPipelineInfo,
                                        &pipelineHash,
                                        &cacheHash);
        result = BuildGraphicsPipelineInternal(&graphicsContext,
                                               shaderInfo,
                                               forceLoopUnrollCount,
                                               buildingRelocatableElf,
                                               &candidateElf);

        if (result == Result::Success)
        {
            elfBin.codeSize = candidateElf.size();
            elfBin.pCode = candidateElf.data();
        }

        if (!buildingRelocatableElf)
            UpdateShaderCache((result == Result::Success), &elfBin, pShaderCache, hEntry);
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
    uint32_t                        forceLoopUnrollCount,           // Force loop unroll count (0 means disable)
    bool                            buildingRelocatableElf,         // Build the pipeline by linking relocatable elf
    ElfPackage*                     pPipelineElf)                   // [out] Output Elf package
{
    Context* pContext = AcquireContext();
    pContext->AttachPipelineContext(pComputeContext);

    const PipelineShaderInfo* shaderInfo[ShaderStageNativeStageCount] =
    {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &pPipelineInfo->cs,
    };

    Result result;
    if (buildingRelocatableElf)
    {
        result = BuildPipelineWithRelocatableElf(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf);
    }
    else
    {
        result = BuildPipelineInternal(pContext, shaderInfo, forceLoopUnrollCount, pPipelineElf);
    }
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

    bool buildingRelocatableElf = CanUseRelocatableComputeShaderElf(&pPipelineInfo->cs);

    Result result = ValidatePipelineShaderInfo(&pPipelineInfo->cs);

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
    IShaderCache* pAppCache = nullptr;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 38
    pAppCache = pPipelineInfo->pShaderCache;
#endif
    ShaderCache* pShaderCache = nullptr;
    CacheEntryHandle hEntry = nullptr;

    if (!buildingRelocatableElf)
    {
        cacheEntryState = LookUpShaderCaches(pAppCache, &cacheHash, &elfBin, &pShaderCache, &hEntry);
    }
    else
    {
        cacheEntryState = ShaderEntryState::Compiling;
    }

    ElfPackage candidateElf;

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        uint32_t                      forceLoopUnrollCount = cl::ForceLoopUnrollCount;

        ComputeContext computeContext(m_gfxIp,
                                      pPipelineInfo,
                                      &pipelineHash,
                                      &cacheHash);

        result = BuildComputePipelineInternal(&computeContext,
                                              pPipelineInfo,
                                              forceLoopUnrollCount,
                                              buildingRelocatableElf,
                                              &candidateElf);

        if (result == Result::Success)
        {
            elfBin.codeSize = candidateElf.size();
            elfBin.pCode = candidateElf.data();
        }
        if (!buildingRelocatableElf)
        {
            UpdateShaderCache((result == Result::Success), &elfBin, pShaderCache, hEntry);
        }
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
        cl::ExecutableName.ArgStr
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
    const PipelineShaderInfo* pShaderInfo     // [in] Pipeline shader info
    ) const
{
    Result result = Result::Success;
    ShaderStage shaderStage = (pShaderInfo != nullptr) ? pShaderInfo->entryStage : ShaderStageInvalid;

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
        pFreeContext = new Context(m_gfxIp);
        pFreeContext->SetInUse(true);
        m_pContextPool->push_back(pFreeContext);
    }

    assert(pFreeContext != nullptr);
    return pFreeContext;
}

// =====================================================================================================================
// Run a pass manager's passes on a module, catching any LLVM fatal error and returning a success indication
bool Compiler::RunPasses(
    lgc::PassManager* pPassMgr, // [in] Pass manager
    Module*           pModule   // [in/out] Module
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
// Upon hit, Ready is returned and pElfBin is filled in. Upon miss, Compiling is returned and ppShaderCache and
// phEntry are filled in.
ShaderEntryState Compiler::LookUpShaderCaches(
    IShaderCache*                    pAppPipelineCache, // [in] App's pipeline cache
    MetroHash::Hash*                 pCacheHash,        // [in] Hash code of the shader
    BinaryData*                      pElfBin,           // [out] Pointer to shader data
    ShaderCache**                    ppShaderCache,     // [out] Shader cache to use
    CacheEntryHandle*                phEntry            // [out] Handle to use
    )
{
    ShaderCache* pShaderCache[2];
    uint32_t shaderCacheCount = 0;

    pShaderCache[shaderCacheCount++] = m_shaderCache.get();

    if (pAppPipelineCache != nullptr && cl::ShaderCacheMode != ShaderCacheForceInternalCacheOnDisk)
    {
        // Put the application's cache last so that we prefer adding entries there (only relevant with old
        // client version).
        pShaderCache[shaderCacheCount++] = static_cast<ShaderCache*>(pAppPipelineCache);
    }

    for (uint32_t i = 0; i < shaderCacheCount; i++)
    {
        // Lookup the shader. Allocate on miss when we've reached the last cache.
        bool allocateOnMiss = (i + 1) == shaderCacheCount;
        CacheEntryHandle hCurrentEntry;
        ShaderEntryState cacheEntryState = pShaderCache[i]->FindShader(*pCacheHash, allocateOnMiss, &hCurrentEntry);
        if (cacheEntryState == ShaderEntryState::Ready)
        {
            Result result = pShaderCache[i]->RetrieveShader(hCurrentEntry, &pElfBin->pCode, &pElfBin->codeSize);
            if (result == Result::Success)
                return ShaderEntryState::Ready;
        }
        else if (cacheEntryState == ShaderEntryState::Compiling)
        {
            *ppShaderCache = pShaderCache[i];
            *phEntry = hCurrentEntry;
            return ShaderEntryState::Compiling;
        }
    }

    // Unable to allocate an entry in a cache, but we can compile anyway.
    *ppShaderCache = nullptr;
    *phEntry = nullptr;

    return ShaderEntryState::Compiling;
}

// =====================================================================================================================
// Update the shader caches with the given entry handle, based on the "insert" flag.
void Compiler::UpdateShaderCache(
    bool                             insert,           // To insert data or reset the shader cache
    const BinaryData*                pElfBin,          // [in] Pointer to shader data
    ShaderCache*                     pShaderCache,     // [in] Shader cache to update (may be nullptr for default)
    CacheEntryHandle                 hEntry)           // [in] Handle to update
{
    if (!hEntry)
        return;

    if (!pShaderCache)
        pShaderCache = m_shaderCache.get();

    if (insert)
    {
        assert(pElfBin->codeSize > 0);
        pShaderCache->InsertShader(hEntry, pElfBin->pCode, pElfBin->codeSize);
    }
    else
    {
        pShaderCache->ResetShader(hEntry);
    }
}

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
        fragmentHasher.Update(pPipelineOptions->scalarBlockLayout);
        fragmentHasher.Update(pPipelineOptions->reconfigWorkgroupLayout);
        fragmentHasher.Update(pPipelineOptions->includeIr);
        fragmentHasher.Update(pPipelineOptions->robustBufferAccess);
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
// Link relocatable shader elf file into a pipeline elf file and apply relocations.
void Compiler::LinkRelocatableShaderElf(
    ElfPackage* pShaderElfs,   // [in]  An array of pipeline elf packages, indexed by stage, containing relocatable elf
    ElfPackage* pPipelineElf,  // [out] Elf package containing the pipeline elf
    Context* pContext)         // [in]  Acquired context
{
    assert(pShaderElfs[ShaderStageTessControl].empty() && "Cannot link tessellation shaders yet.");
    assert(pShaderElfs[ShaderStageTessEval].empty() && "Cannot link tessellation shaders yet.");
    assert(pShaderElfs[ShaderStageGeometry].empty() && "Cannot link geometry shaders yet.");

    Result result = Result::Success;
    ElfWriter<Elf64> writer(m_gfxIp);

    if (pShaderElfs[ShaderStageCompute].empty())
    {
        ElfReader<Elf64> vsReader(m_gfxIp);
        ElfReader<Elf64> fsReader(m_gfxIp);
        if (!pShaderElfs[ShaderStageVertex].empty())
        {
            size_t codeSize = pShaderElfs[ShaderStageVertex].size_in_bytes();
            result = vsReader.ReadFromBuffer(pShaderElfs[ShaderStageVertex].data(), &codeSize);
            if (result != Result::Success)
            {
                return;
            }
        }

        if (!pShaderElfs[ShaderStageFragment].empty())
        {
            size_t codeSize = pShaderElfs[ShaderStageFragment].size_in_bytes();
            result = fsReader.ReadFromBuffer(pShaderElfs[ShaderStageFragment].data(), &codeSize);
            if (result != Result::Success)
            {
                return;
            }
        }

        result = writer.LinkGraphicsRelocatableElf({&vsReader, &fsReader}, pContext);
    }
    else
    {
        ElfReader<Elf64> csReader(m_gfxIp);
        size_t codeSize = pShaderElfs[ShaderStageCompute].size_in_bytes();
        result = csReader.ReadFromBuffer(pShaderElfs[ShaderStageCompute].data(), &codeSize);
        if (result != Result::Success)
        {
            return;
        }
        result = writer.LinkComputeRelocatableElf(csReader, pContext);
    }

    if (result != Result::Success)
    {
        return;
    }
    writer.WriteToBuffer(pPipelineElf);
}

// =====================================================================================================================
// Convert front-end LLPC shader stage to middle-end LGC shader type
lgc::ShaderStage GetLgcShaderStage(Llpc::ShaderStage stage)
{
    switch (stage)
    {
    case ShaderStageCompute:
        return lgc::ShaderStageCompute;
    case ShaderStageVertex:
        return lgc::ShaderStageVertex;
    case ShaderStageTessControl:
        return lgc::ShaderStageTessControl;
    case ShaderStageTessEval:
        return lgc::ShaderStageTessEval;
    case ShaderStageGeometry:
        return lgc::ShaderStageGeometry;
    case ShaderStageFragment:
        return lgc::ShaderStageFragment;
    default:
        llvm_unreachable("");
        return lgc::ShaderStageInvalid;
    }
}

} // Llpc
