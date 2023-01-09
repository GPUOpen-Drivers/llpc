/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcCompiler.h"
#include "LLVMSPIRVLib.h"
#include "SPIRVInternal.h"
#include "llpcCacheAccessor.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcElfWriter.h"
#include "llpcError.h"
#include "llpcFile.h"
#include "llpcGraphicsContext.h"
#if VKI_RAY_TRACING
#include "llpcRayTracingContext.h"
#endif
#include "llpcShaderModuleHelper.h"
#include "llpcSpirvLower.h"
#include "llpcSpirvLowerCfgMerges.h"
#if VKI_RAY_TRACING
#include "llpcSpirvLowerRayTracing.h"
#endif
#include "llpcSpirvLowerTranslator.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcTimerProfiler.h"
#include "llpcUtil.h"
#include "spirvExt.h"
#include "vkgcDefs.h"
#include "vkgcElfReader.h"
#include "vkgcPipelineDumper.h"
#include "lgc/Builder.h"
#include "lgc/ElfLinker.h"
#include "lgc/EnumIterator.h"
#include "lgc/PassManager.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#if VKI_RAY_TRACING
#include "llvm/Transforms/Utils/Cloning.h"
#endif
#include <cassert>
#include <mutex>
#include <set>
#include <unordered_set>

#ifdef LLPC_ENABLE_SPIRV_OPT
#include "spvgen.h"
#endif

#if VKI_RAY_TRACING
namespace RtName {
extern const char *TraceRayKHR;
} // namespace RtName
#endif

#define DEBUG_TYPE "llpc-compiler"

using namespace lgc;
using namespace llvm;
using namespace MetroHash;
using namespace SPIRV;
using namespace spv;
using namespace Util;
using namespace Vkgc;

namespace llvm {

namespace {

// llvm_shutdown_obj is used to ensure LLVM is only shutdown once the static
// destructors have run. This allows Compiler to re-initialize correctly -
// otherwise LLVM's static globals are not re-initialized.
llvm_shutdown_obj ShutdownObj;

} // namespace

namespace cl {

// -pipeline-dump-dir: directory where pipeline info are dumped
opt<std::string> PipelineDumpDir("pipeline-dump-dir", desc("Directory where pipeline shader info are dumped"),
                                 value_desc("dir"), init("."));

// -enable-pipeline-dump: enable pipeline info dump
opt<bool> EnablePipelineDump("enable-pipeline-dump", desc("Enable pipeline info dump"), init(false));

// -shader-cache-file-dir: root directory to store shader cache
opt<std::string> ShaderCacheFileDir("shader-cache-file-dir", desc("Root directory to store shader cache"),
                                    value_desc("dir"), init("."));

// DEPRECATED: This option should be removed once XGL sets the corresponding pipeline option.
// -use-relocatable-shader-elf: Gets LLVM to generate more generic elf files for each shader individually, and LLPC will
// then link those ELF files to generate the compiled pipeline.
opt<bool> UseRelocatableShaderElf("use-relocatable-shader-elf",
                                  desc("DEPRECATED: To be replaced by a pipeline option.  The pipeline will be built "
                                       "by building relocatable shader ELF files when "
                                       "possible, and linking them together.  This is a work in progress and should "
                                       "be used with caution."),
                                  init(false));

// -relocatable-shader-elf-limit=<n>: Limits the number of pipelines that will be compiled using relocatable shader ELF.
// This is to be used for debugging by doing a binary search to identify a pipeline that is being miscompiled when using
// relocatable shader ELF modules.
opt<int> RelocatableShaderElfLimit("relocatable-shader-elf-limit",
                                   cl::desc("Max number of pipeline compiles that will use "
                                            "relocatable shader ELF.  -1 means unlimited."),
                                   init(-1));

// -shader-cache-mode: shader cache mode:
// 0 - Disable
// 1 - Runtime cache
// 2 - Cache to disk
// 3 - Use internal on-disk cache in read/write mode.
// 4 - Use internal on-disk cache in read-only mode.
opt<unsigned> ShaderCacheMode("shader-cache-mode",
                              desc("Shader cache mode, 0 - disable, 1 - runtime cache, 2 - cache to disk, 3 - "
                                   "load on-disk cache for read/write, 4 - load on-disk cache for read only"),
                              init(0));

// -cache-full-pipelines: Add full pipelines to the caches that are provided.
opt<bool> CacheFullPipelines("cache-full-pipelines", desc("Add full pipelines to the caches that are provided."),
                             init(true));

// -executable-name: executable file name
static opt<std::string> ExecutableName("executable-name", desc("Executable file name"), value_desc("filename"),
                                       init("amdllpc"));

// -enable-per-stage-cache: Enable shader cache per shader stage
opt<bool> EnablePerStageCache("enable-per-stage-cache", cl::desc("Enable shader cache per shader stage"), init(true));

// -context-reuse-limit: The maximum number of times a compiler context can be reused.
opt<int> ContextReuseLimit("context-reuse-limit",
                           cl::desc("The maximum number of times a compiler context can be reused"), init(100));

// -fatal-llvm-errors: Make all LLVM errors fatal
opt<bool> FatalLlvmErrors("fatal-llvm-errors", cl::desc("Make all LLVM errors fatal"), init(false));

// -enable-part-pipeline: Use part pipeline compilation scheme (experimental)
opt<bool> EnablePartPipeline("enable-part-pipeline", cl::desc("Enable part pipeline compilation scheme"), init(false));

extern opt<bool> EnableOuts;

extern opt<bool> EnableErrs;

extern opt<std::string> LogFileDbgs;

extern opt<std::string> LogFileOuts;

} // namespace cl

} // namespace llvm

namespace Llpc {

sys::Mutex Compiler::m_contextPoolMutex;
std::vector<Context *> *Compiler::m_contextPool = nullptr;

// Enumerates modes used in shader replacement
enum ShaderReplaceMode {
  ShaderReplaceDisable = 0,            // Disabled
  ShaderReplaceShaderHash = 1,         // Replacement based on shader hash
  ShaderReplaceShaderPipelineHash = 2, // Replacement based on both shader and pipeline hash
};

static ManagedStatic<sys::Mutex> SCompilerMutex;
static bool HaveParsedOptions = false;
static MetroHash::Hash SOptionHash = {};

unsigned Compiler::m_instanceCount = 0;
unsigned Compiler::m_outRedirectCount = 0;

#if VKI_RAY_TRACING
// Represents the payload used by helper thread to build ray tracing Elf
struct HelperThreadBuildRayTracingPipelineElfPayload {
  std::vector<Module *> &modules;                     // Modules to generate ELF packages
  std::vector<ElfPackage> &pipelineElfs;              // Output ELF packages
  std::vector<RayTracingShaderProperty> &shaderProps; // Output RayTracingShaderProperty
  std::vector<bool> &moduleCallsTraceRay;             // Whether each module calls OpTraceRay
  std::vector<Result> &results;                       // Build result of each module
  RayTracingContext *rayTracingContext;               // The ray tracing context across the pipeline
  Compiler *compiler;                                 // The compiler instance
  volatile bool helperThreadJoined;                   // Whether helper thread has joined
  volatile bool mainThreadSwitchedContext;            // Whether main thread has finished switching context
};

sys::Mutex Compiler::m_helperThreadMutex;
std::condition_variable_any Compiler::m_helperThreadConditionVariable;
#endif

// =====================================================================================================================
// Handler for LLVM fatal error.
//
// @param userData : An argument which will be passed to the installed error handler
// @param reason : Error reason
// @param genCrashDiag : Whether diagnostic should be generated
static void fatalErrorHandler(void *userData, const char *reason, bool genCrashDiag) {
  LLPC_ERRS("LLVM FATAL ERROR: " << reason << "\n");
#if LLPC_ENABLE_EXCEPTION
  throw("LLVM fatal error");
#endif
}

// =====================================================================================================================
// Returns the cache accessor object resulting from checking the caches for the glue shader for the given identifier.
//
// @param glueShaderIdentifier : The linker object for which the glue shaders are needed.
// @param context : The context that contains the application caches.
// @param compiler : The compiler object that contains the internal caches.
static CacheAccessor checkCacheForGlueShader(StringRef glueShaderIdentifier, Context *context, Compiler *compiler) {
  Hash glueShaderCacheHash =
      PipelineDumper::generateHashForGlueShader({glueShaderIdentifier.size(), glueShaderIdentifier.data()});
  return CacheAccessor(context, glueShaderCacheHash, compiler->getInternalCaches());
}

// =====================================================================================================================
// Set the glue shader at glueIndex in the ELF linking with the data in the cache.  The data must be in the cache.
//
// @param elfLinker : The linker object for which the glue shaders are needed.
// @param glueIndex : The index of the glue shader to be set.
// @param cacheAccessor : The cache accessor that resulted from checking the caches for the glue shader.
static void setGlueBinaryBlobFromCacheData(ElfLinker *elfLinker, unsigned glueIndex,
                                           const CacheAccessor &cacheAccessor) {
  assert(cacheAccessor.isInCache());
  BinaryData elf = cacheAccessor.getElfFromCache();
  elfLinker->addGlue(glueIndex, StringRef(reinterpret_cast<const char *>(elf.pCode), elf.codeSize));
}

// =====================================================================================================================
// Set the data in the cache to the given data.
//
// @param cacheAccessor : The cache accessor for the entry to be updated.
// @param elfData : The data to use to update the cache.
static void updateCache(CacheAccessor &cacheAccessor, StringRef elfData) {
  BinaryData elfBin = {elfData.size(), elfData.data()};
  cacheAccessor.setElfInCache(elfBin);
}

// =====================================================================================================================
// Sets all of the glue shaders in elfLinker by getting the binary from the cache or compiling it.
//
// @param elfLinker : The linker object for which the glue shaders are needed.
// @param context : The context that contains the application caches.
// @param compiler : The compiler object that contains the internal caches.
static void setGlueBinaryBlobsInLinker(ElfLinker *elfLinker, Context *context, Compiler *compiler) {
  ArrayRef<StringRef> glueShaderIdentifiers = elfLinker->getGlueInfo();
  for (unsigned i = 0; i < glueShaderIdentifiers.size(); ++i) {
    LLPC_OUTS("ID for glue shader" << i << ": " << llvm::toHex(glueShaderIdentifiers[i]) << "\n");
    CacheAccessor cacheAccessor = checkCacheForGlueShader(glueShaderIdentifiers[i], context, compiler);

    if (cacheAccessor.isInCache()) {
      LLPC_OUTS("Cache hit for glue shader " << i << "\n");
      setGlueBinaryBlobFromCacheData(elfLinker, i, cacheAccessor);
    } else {
      LLPC_OUTS("Cache miss for glue shader " << i << "\n");
      StringRef elfData = elfLinker->compileGlue(i);
      LLPC_OUTS("Updating the cache for glue shader " << i << "\n");
      updateCache(cacheAccessor, elfData);
    }
  }
}

// =====================================================================================================================
// Handler for diagnosis in pass run, derived from the standard one.
class LlpcDiagnosticHandler : public DiagnosticHandler {
public:
  LlpcDiagnosticHandler(bool *hasError) : m_hasError(hasError) {}
  bool handleDiagnostics(const DiagnosticInfo &diagInfo) override {
    if (diagInfo.getSeverity() == DS_Error)
      *m_hasError = true;
    if (cl::FatalLlvmErrors && diagInfo.getSeverity() == DS_Error) {
      DiagnosticPrinterRawOStream printStream(errs());
      printStream << "LLVM FATAL ERROR: ";
      diagInfo.print(printStream);
      printStream << "\n";
      errs().flush();
#if LLPC_ENABLE_EXCEPTION
      throw("LLVM fatal error");
#endif
      abort();
    }

    if (EnableOuts() || EnableErrs()) {
      if (diagInfo.getSeverity() == DS_Error || diagInfo.getSeverity() == DS_Warning) {
        auto &outputStream = EnableOuts() ? outs() : errs();
        DiagnosticPrinterRawOStream printStream(outputStream);
        printStream << "ERROR: LLVM DIAGNOSIS INFO: ";
        diagInfo.print(printStream);
        printStream << "\n";
        outputStream.flush();
      } else if (EnableOuts()) {
        DiagnosticPrinterRawOStream printStream(outs());
        printStream << "\n\n=====  LLVM DIAGNOSIS START  =====\n\n";
        diagInfo.print(printStream);
        printStream << "\n\n=====  LLVM DIAGNOSIS END  =====\n\n";
        outs().flush();
      }
    }

    return true;
  }

private:
  bool *m_hasError;
};

// =====================================================================================================================
// Creates LLPC compiler from the specified info.
//
// @param gfxIp : Graphics IP version
// @param optionCount : Count of compilation-option strings
// @param options : An array of compilation-option strings
// @param [out] ppCompiler : Pointer to the created LLPC compiler object
// @param cache : Pointer to ICache implemented in client
Result VKAPI_CALL ICompiler::Create(GfxIpVersion gfxIp, unsigned optionCount, const char *const *options,
                                    ICompiler **ppCompiler, ICache *cache) {
  Result result = Result::Success;

  const char *client = options[0];
  bool ignoreErrors = (strcmp(client, VkIcdName) == 0);

  raw_null_ostream nullStream;

  std::lock_guard<sys::Mutex> lock(*SCompilerMutex);
  MetroHash::Hash optionHash = Compiler::generateHashForCompileOptions(optionCount, options);

  LgcContext::initialize();

  bool parseCmdOption = true;
  if (HaveParsedOptions) {
    bool isSameOption = memcmp(&optionHash, &SOptionHash, sizeof(optionHash)) == 0;

    parseCmdOption = false;
    if (!isSameOption) {
      if (Compiler::getOutRedirectCount() == 0) {
        // All compiler instances are destroyed, we can reset LLVM options
        cl::ResetAllOptionOccurrences();
        parseCmdOption = true;
      } else {
        LLPC_ERRS("Incompatible compiler options cross compiler instances!");
        result = Result::ErrorInvalidValue;
        llvm_unreachable("Should never be called!");
      }
    }
  }

  if (parseCmdOption) {
    // LLVM command options can't be parsed multiple times
    if (cl::ParseCommandLineOptions(optionCount, options, "AMD LLPC compiler", ignoreErrors ? &nullStream : nullptr)) {
      HaveParsedOptions = true;
    } else {
      result = Result::ErrorInvalidValue;
    }
  }

  if (result == Result::Success) {
    SOptionHash = optionHash;
    *ppCompiler = new Compiler(gfxIp, optionCount, options, SOptionHash, cache);
    assert(*ppCompiler);

    if (EnableOuts()) {
      // LLPC_OUTS is enabled. Ensure it is enabled in LGC (the middle-end) too.
      LgcContext::setLlpcOuts(&outs());
    }
  } else {
    *ppCompiler = nullptr;
    result = Result::ErrorInvalidValue;
  }
  return result;
}

// =====================================================================================================================
// Checks whether a vertex attribute format is supported by fetch shader.
//
// @param format : Vertex attribute format
bool VKAPI_CALL ICompiler::IsVertexFormatSupported(VkFormat format) {
  BufDataFormat dfmt = PipelineContext::mapVkFormat(format, false).first;
  return dfmt != BufDataFormatInvalid;
}

// =====================================================================================================================
//
// @param gfxIp : Graphics IP version info
// @param optionCount : Count of compilation-option strings
// @param options : An array of compilation-option strings
// @param optionHash : Hash code of compilation options
// @param cache : Pointer to ICache implemented in client
Compiler::Compiler(GfxIpVersion gfxIp, unsigned optionCount, const char *const *options, MetroHash::Hash optionHash,
                   ICache *cache)
    : m_optionHash(optionHash), m_gfxIp(gfxIp), m_cache(cache), m_relocatablePipelineCompilations(0) {
  for (unsigned i = 0; i < optionCount; ++i)
    m_options.push_back(options[i]);

  if (m_outRedirectCount == 0)
    redirectLogOutput(false, optionCount, options);

  if (m_instanceCount == 0) {
    // LLVM fatal error handler only can be installed once.
    install_fatal_error_handler(fatalErrorHandler);

    // Initialize m_pContextPool.
    {
      std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);

      m_contextPool = new std::vector<Context *>();
    }
  }

  // Initialize shader cache
  ShaderCacheCreateInfo createInfo = {};
  ShaderCacheAuxCreateInfo auxCreateInfo = {};
  unsigned shaderCacheMode = cl::ShaderCacheMode;
  auxCreateInfo.shaderCacheMode = static_cast<ShaderCacheMode>(shaderCacheMode);
  auxCreateInfo.gfxIp = m_gfxIp;
  auxCreateInfo.hash = m_optionHash;
  auxCreateInfo.executableName = cl::ExecutableName.c_str();

  const char *shaderCachePath = cl::ShaderCacheFileDir.c_str();
  if (cl::ShaderCacheFileDir.empty()) {
#ifdef WIN_OS
    shaderCachePath = getenv("LOCALAPPDATA");
    assert(shaderCachePath);
#else
    llvm_unreachable("Should never be called!");
#endif
  }

  if (strlen(shaderCachePath) >= Llpc::MaxPathLen) {
    LLPC_ERRS("The shader-cache-file-dir exceed the maximum length (" << Llpc::MaxPathLen << ")\n");
    llvm_unreachable("ShaderCacheFileDir is too long");
  }
  auxCreateInfo.cacheFilePath = shaderCachePath;

  m_shaderCache = ShaderCacheManager::getShaderCacheManager()->getShaderCacheObject(&createInfo, &auxCreateInfo);

  ++m_instanceCount;
  ++m_outRedirectCount;
}

// =====================================================================================================================
Compiler::~Compiler() {
  bool shutdown = false;
  {
    // Free context pool
    std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);

    // Keep the max allowed count of contexts that reside in the pool so that we can speed up the creation of the
    // compiler next time.
    for (auto it = m_contextPool->begin(); it != m_contextPool->end();) {
      auto context = *it;
      size_t maxResidentContexts = 0;

      // This is just a W/A for Teamcity. Setting AMD_RESIDENT_CONTEXTS could reduce more than 40 minutes of
      // CTS running time.
      char *maxResidentContextsEnv = getenv("AMD_RESIDENT_CONTEXTS");

      if (maxResidentContextsEnv)
        maxResidentContexts = strtoul(maxResidentContextsEnv, nullptr, 0);

      if (!context->isInUse() && m_contextPool->size() > maxResidentContexts) {
        it = m_contextPool->erase(it);
        delete context;
      } else
        ++it;
    }
  }

  // Restore default output
  {
    std::lock_guard<sys::Mutex> lock(*SCompilerMutex);
    --m_outRedirectCount;
    if (m_outRedirectCount == 0)
      redirectLogOutput(true, 0, nullptr);

    ShaderCacheManager::getShaderCacheManager()->releaseShaderCacheObject(m_shaderCache);
  }

  {
    // s_compilerMutex is managed by ManagedStatic, it can't be accessed after llvm_shutdown
    std::lock_guard<sys::Mutex> lock(*SCompilerMutex);
    --m_instanceCount;
    if (m_instanceCount == 0)
      shutdown = true;
  }

  if (shutdown) {
    ShaderCacheManager::shutdown();
    remove_fatal_error_handler();
    delete m_contextPool;
    m_contextPool = nullptr;
  }
}

// =====================================================================================================================
// Destroys the pipeline compiler.
void Compiler::Destroy() {
  delete this;
}

// =====================================================================================================================
// Builds shader module from the specified info.
//
// @param shaderInfo : Info to build this shader module
// @param [out] shaderOut : Output of building this shader module
Result Compiler::BuildShaderModule(const ShaderModuleBuildInfo *shaderInfo, ShaderModuleBuildOut *shaderOut) {
  MetroHash::Hash hash = {};
  MetroHash64::Hash(reinterpret_cast<const uint8_t *>(shaderInfo->shaderBin.pCode), shaderInfo->shaderBin.codeSize,
                    hash.bytes);
  TimerProfiler timerProfiler(MetroHash::compact64(&hash), "LLPC ShaderModule",
                              TimerProfiler::ShaderModuleTimerEnableMask);

  if (!shaderInfo->pfnOutputAlloc) {
    // Allocator is not specified
    return Result::ErrorInvalidPointer;
  }

  unsigned codeSize = ShaderModuleHelper::getCodeSize(shaderInfo);
  size_t allocSize = sizeof(ShaderModuleData) + codeSize;
  unsigned *allocBuf =
      static_cast<unsigned *>(shaderInfo->pfnOutputAlloc(shaderInfo->pInstance, shaderInfo->pUserData, allocSize));
  if (!allocBuf)
    return Result::ErrorOutOfMemory;

  ShaderModuleData *moduleData = reinterpret_cast<ShaderModuleData *>(allocBuf);
  *moduleData = {};
  MutableArrayRef<unsigned> codeBuffer(allocBuf + sizeof(ShaderModuleData) / sizeof(*allocBuf),
                                       codeSize / sizeof(*allocBuf));

  memcpy(moduleData->hash, &hash, sizeof(hash));
  ShaderModuleHelper::getModuleData(shaderInfo, codeBuffer, *moduleData);
  shaderOut->pModuleData = moduleData;

  if (moduleData->binType == BinaryType::Spirv && cl::EnablePipelineDump)
    PipelineDumper::DumpSpirvBinary(cl::PipelineDumpDir.c_str(), &shaderInfo->shaderBin, &hash);

  return Result::Success;
}

// =====================================================================================================================
// Helper function for formatting raw data into a space-separated string of lowercase hex bytes.
// This assumes Little Endian byte order, e.g., {45u} --> `2d 00 00 00`.
//
// @param data : Raw data to be formatted.
// @returns : Formatted bytes, e.g., `ab c4 ef 00`.
template <typename T> static FormattedBytes formatBytesLittleEndian(ArrayRef<T> data) {
  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(data.data()), data.size() * sizeof(T));
  return format_bytes(bytes, /* FirstByteOffset = */ {}, /* NumPerLine = */ 16, /* ByteGroupSize = */ 1);
}

// =====================================================================================================================
// Helper function for dumping compiler options
//
// @param pipelineDumpFile : Handle of pipeline dump file
void Compiler::dumpCompilerOptions(void *pipelineDumpFile) {
  if (!pipelineDumpFile)
    return;
  std::string extraInfo;
  raw_string_ostream os(extraInfo);
  os << ";Compiler Options: " << join(m_options, " ");
  os.flush();
  PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile *>(pipelineDumpFile), &extraInfo);
}

// =====================================================================================================================
// Build unlinked shader to ElfPackage with pipeline info.
//
// @param pipelineInfo : Info to build this shader module
// @param [out] pipelineOut : Output of building this shader module
// @param stage : Shader stage of needing to compile
// @param pipelineDumpFile : Handle of pipeline dump file; It is optional, the default value is nullptr
// @returns : Result::Success if successful. Other return codes indicate failure.
Result Compiler::buildGraphicsShaderStage(const GraphicsPipelineBuildInfo *pipelineInfo,
                                          GraphicsPipelineBuildOut *pipelineOut, UnlinkedShaderStage stage,
                                          void *pipelineDumpFile) {
  if (!pipelineInfo->pfnOutputAlloc)
    return Result::ErrorInvalidPointer;
  // clang-format off
  SmallVector<const PipelineShaderInfo *, ShaderStageGfxCount> shaderInfo = {
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };
  // clang-format on

  switch (stage) {
  case UnlinkedStageVertexProcess: {
    shaderInfo[ShaderStageTask] = &pipelineInfo->task;
    shaderInfo[ShaderStageVertex] = &pipelineInfo->vs;
    shaderInfo[ShaderStageTessControl] = &pipelineInfo->tcs;
    shaderInfo[ShaderStageTessEval] = &pipelineInfo->tes;
    shaderInfo[ShaderStageGeometry] = &pipelineInfo->gs;
    shaderInfo[ShaderStageMesh] = &pipelineInfo->mesh;
    break;
  }
  case UnlinkedStageFragment: {
    shaderInfo[ShaderStageFragment] = &pipelineInfo->fs;
    break;
  }
  default:
    llvm_unreachable("");
    break;
  }

  dumpCompilerOptions(pipelineDumpFile);

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, true, stage);
  pipelineHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false, true, stage);

  // Compile
  GraphicsContext graphicsContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
  Context *context = acquireContext();
  context->attachPipelineContext(&graphicsContext);

  ElfPackage candidateElf;
  Result result =
      buildUnlinkedShaderInternal(context, shaderInfo, stage, candidateElf, pipelineOut->stageCacheAccesses);

  releaseContext(context);

  if (result != Result::Success)
    return result;

  void *allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, candidateElf.size());
  uint8_t *code = static_cast<uint8_t *>(allocBuf);
  memcpy(code, candidateElf.data(), candidateElf.size());
  pipelineOut->pipelineBin.codeSize = candidateElf.size();
  pipelineOut->pipelineBin.pCode = code;
  return result;
}

// =====================================================================================================================
// Build the whole graphics pipeline. If missing elfPackage of a certain stage, we will build it first, and
// link them to the full pipeline last.
//
// @param pipelineInfo : Info to build this shader module
// @param [out] pipelineOut : Output of building this shader module
// @param elfPackage : Early compiled elfPackage; it is an array which size is UnlinkedStageCount.
// @returns : Result::Success if successful. Other return codes indicate failure.
Result Compiler::buildGraphicsPipelineWithElf(const GraphicsPipelineBuildInfo *pipelineInfo,
                                              GraphicsPipelineBuildOut *pipelineOut, const BinaryData *elfPackage) {
  if (!pipelineInfo->pfnOutputAlloc)
    return Result::ErrorInvalidPointer;

  // clang-format off
  SmallVector<const PipelineShaderInfo *, ShaderStageGfxCount> shaderInfo = {
    &pipelineInfo->task,
    &pipelineInfo->vs,
    &pipelineInfo->tcs,
    &pipelineInfo->tes,
    &pipelineInfo->gs,
    &pipelineInfo->mesh,
    &pipelineInfo->fs,
  };
  // clang-format on

  if (!canUseRelocatableGraphicsShaderElf(shaderInfo, pipelineInfo)) {
    LLPC_OUTS("Relocatable shader compilation requested but not possible.\n");
    return Result::ErrorInvalidValue;
  }

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, false);
  pipelineHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false, false);

  Optional<CacheAccessor> cacheAccessor;
  if (cl::CacheFullPipelines) {
    cacheAccessor.emplace(pipelineInfo, cacheHash, getInternalCaches());
  }

  Result result = Result::Success;
  BinaryData elfBin = {};
  ElfPackage elf[UnlinkedStageCount] = {};
  ElfPackage pipelineElf;
  if (cacheAccessor && cacheAccessor->isInCache()) {
    LLPC_OUTS("Cache hit for graphics pipeline.\n");
    elfBin = cacheAccessor->getElfFromCache();
    pipelineOut->pipelineCacheAccess =
        cacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
  } else {
    LLPC_OUTS("Cache miss for graphics pipeline.\n");
    if (cacheAccessor && pipelineOut->pipelineCacheAccess == CacheAccessInfo::CacheNotChecked)
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::CacheMiss;

    GraphicsContext graphicsContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
    Context *context = acquireContext();
    context->attachPipelineContext(&graphicsContext);

    // Release context.
    auto onExit = make_scope_exit([&] { releaseContext(context); });

    for (unsigned idx = 0; idx < UnlinkedStageCount; idx++) {
      if (elfPackage[idx].pCode) {
        auto data = reinterpret_cast<const char *>(elfPackage[idx].pCode);
        elf[idx].assign(data, data + elfPackage[idx].codeSize);
      } else {
        graphicsContext.setUnlinked(true);
        result = buildUnlinkedShaderInternal(context, shaderInfo, static_cast<UnlinkedShaderStage>(idx), elf[idx],
                                             pipelineOut->stageCacheAccesses);
        if (result != Result::Success)
          return result;
      }
    }

    // Link
    graphicsContext.setUnlinked(false);
    bool hasError = false;
    context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));
    hasError |= !linkRelocatableShaderElf(elf, &pipelineElf, context);

    context->setDiagnosticHandler(nullptr);
    if (hasError)
      return Result::ErrorInvalidShader;

    elfBin.codeSize = pipelineElf.size();
    elfBin.pCode = pipelineElf.data();

    if (cacheAccessor && !cacheAccessor->isInCache()) {
      LLPC_OUTS("Adding graphics pipeline to the cache.\n");
      cacheAccessor->setElfInCache(elfBin);
    }
  }

  void *allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, elfBin.codeSize);
  uint8_t *code = static_cast<uint8_t *>(allocBuf);
  memcpy(code, elfBin.pCode, elfBin.codeSize);
  pipelineOut->pipelineBin.codeSize = elfBin.codeSize;
  pipelineOut->pipelineBin.pCode = code;

  return result;
}

// =====================================================================================================================
// Build unlinked shader from the specified info.
//
// @param context : Acquired context
// @param shaderInfo : Shader info of this pipeline
// @param stage :  Shader stage of needing to compile
// @param [out] elfPackage : Output Elf package
// @param [out] stageCacheAccesses : Stage cache access result. All elements
//                                   must be initialized in the caller as
//                                   CacheAccessInfo::CacheNotChecked
// @returns : Result::Success if successful. Other return codes indicate failure.
Result Compiler::buildUnlinkedShaderInternal(Context *context, ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                             UnlinkedShaderStage stage, ElfPackage &elfPackage,
                                             MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  if (!hasDataForUnlinkedShaderType(stage, shaderInfo))
    return Result::Success;

  unsigned originalShaderStageMask = context->getPipelineContext()->getShaderStageMask();
  const MetroHash::Hash originalCacheHash = context->getPipelineContext()->getCacheHashCodeWithoutCompact();
  unsigned shaderStageMask = getShaderStageMaskForType(stage) & originalShaderStageMask;
  context->getPipelineContext()->setShaderStageMask(shaderStageMask);
  const auto shaderStages = maskToShaderStages(shaderStageMask);
  assert(all_of(shaderStages, isNativeStage) && "Unexpected stage kind");

  // Check the cache for the relocatable shader for this stage.
  MetroHash::Hash cacheHash = {};
  if (context->isGraphics()) {
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, true, stage);
  } else {
    auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    cacheHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, true, true);
  }
  // Note that this code updates m_pipelineHash of the pipeline context. It must be restored.
  context->getPipelineContext()->setHashForCacheLookUp(cacheHash);
  LLPC_OUTS("Finalized hash for " << getUnlinkedShaderStageName(stage) << " stage cache lookup: "
                                  << format_hex(context->getPipelineContext()->get128BitCacheHashCode()[0], 18) << ' '
                                  << format_hex(context->getPipelineContext()->get128BitCacheHashCode()[1], 18)
                                  << '\n');

  auto onExit = make_scope_exit([&] {
    context->getPipelineContext()->setShaderStageMask(originalShaderStageMask);
    context->getPipelineContext()->setHashForCacheLookUp(originalCacheHash);
  });

  Result result = Result::Success;
  CacheAccessor cacheAccessor(context, cacheHash, getInternalCaches());
  if (cacheAccessor.isInCache()) {
    BinaryData elfBin = cacheAccessor.getElfFromCache();
    auto data = reinterpret_cast<const char *>(elfBin.pCode);
    elfPackage.assign(data, data + elfBin.codeSize);
    LLPC_OUTS("Cache hit for shader stage " << getUnlinkedShaderStageName(stage) << "\n");
    for (ShaderStage gfxStage : shaderStages)
      stageCacheAccesses[gfxStage] =
          cacheAccessor.hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
  } else {
    LLPC_OUTS("Cache miss for shader stage " << getUnlinkedShaderStageName(stage) << "\n");
    for (ShaderStage gfxStage : shaderStages)
      stageCacheAccesses[gfxStage] = CacheAccessInfo::CacheMiss;

    // There was a cache miss, so we need to build the relocatable shader for
    // this stage.
    const PipelineShaderInfo *singleStageShaderInfo[ShaderStageNativeStageCount] = {nullptr, nullptr, nullptr,
                                                                                    nullptr, nullptr, nullptr};

    for (ShaderStage nativeStage : shaderStages)
      singleStageShaderInfo[nativeStage] = shaderInfo[nativeStage];

    result = buildPipelineInternal(context, singleStageShaderInfo, PipelineLink::Unlinked, nullptr, &elfPackage,
                                   stageCacheAccesses);
    if (result == Result::Success) {
      // Add the result to the cache.
      BinaryData elfBin = {elfPackage.size(), elfPackage.data()};
      cacheAccessor.setElfInCache(elfBin);
      LLPC_OUTS("Updating the cache for unlinked shader stage " << getUnlinkedShaderStageName(stage) << "\n");
    }
  }

  return result;
}

// =====================================================================================================================
// Builds a pipeline by building relocatable elf files and linking them together.  The relocatable elf files will be
// cached for future use.
//
// @param context : Acquired context
// @param shaderInfo : Shader info of this pipeline
// @param [out] pipelineElf : Output Elf package
// @param [out] stageCacheAccesses : Stage cache access result. All elements
//                                   must be initialized in the caller as
//                                   CacheAccessInfo::CacheNotChecked
Result Compiler::buildPipelineWithRelocatableElf(Context *context, ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                                 ElfPackage *pipelineElf,
                                                 MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  LLPC_OUTS("Building pipeline with relocatable shader elf.\n");
  Result result = Result::Success;

  bool isUnlinkedPipeline = context->getPipelineContext()->isUnlinked();
  context->getPipelineContext()->setUnlinked(true);

  ElfPackage elf[enumCount<UnlinkedShaderStage>()];
  assert(stageCacheAccesses.size() >= shaderInfo.size());

  const MetroHash::Hash originalCacheHash = context->getPipelineContext()->getCacheHashCodeWithoutCompact();
  // Print log in the format matching llvm-readelf to simplify testing.
  LLPC_OUTS("LLPC version: " << VersionTuple(LLPC_INTERFACE_MAJOR_VERSION, LLPC_INTERFACE_MINOR_VERSION) << "\n");
  LLPC_OUTS("Hash for pipeline cache lookup: " << formatBytesLittleEndian<uint8_t>(originalCacheHash.bytes) << "\n");

  for (UnlinkedShaderStage stage : lgc::enumRange<UnlinkedShaderStage>()) {
    if (!hasDataForUnlinkedShaderType(stage, shaderInfo))
      continue;
    if (buildUnlinkedShaderInternal(context, shaderInfo, stage, elf[stage], stageCacheAccesses) != Result::Success)
      break;
  }
  context->getPipelineContext()->setUnlinked(false);

  if (result == Result::Success) {
    if (!isUnlinkedPipeline) {
      // Link the relocatable shaders into a single pipeline elf file.
      bool hasError = false;
      context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

      hasError |= !linkRelocatableShaderElf(elf, pipelineElf, context);
      context->setDiagnosticHandler(nullptr);

      if (hasError)
        result = Result::ErrorInvalidShader;
    } else {
      // Return the first relocatable shader, since we can only return one anyway.
      for (auto unlinkedStage : enumRange<UnlinkedShaderStage>()) {
        auto &unlinkedStageElf = elf[unlinkedStage];
        if (unlinkedStageElf.empty())
          continue;
        *pipelineElf = unlinkedStageElf;
        break;
      }
    }
  }
  return result;
}

// =====================================================================================================================
// Returns true if node is of a descriptor type that is unsupported by relocatable shader compilation.
//
// @param [in] node : User data node
static bool isUnrelocatableResourceMappingRootNode(const ResourceMappingNode *node) {
  switch (node->type) {
  case ResourceMappingNodeType::DescriptorTableVaPtr: {
    const ResourceMappingNode *startInnerNode = node->tablePtr.pNext;
    const ResourceMappingNode *endInnerNode = startInnerNode + node->tablePtr.nodeCount;
    for (const ResourceMappingNode *innerNode = startInnerNode; innerNode != endInnerNode; ++innerNode) {
      if (innerNode->type == ResourceMappingNodeType::InlineBuffer) {
        // The code to handle an inline buffer cannot be easily patched, so relocatable shaders
        // assume there are no inline buffers.
        return true;
      }
    }
    break;
  }
  case ResourceMappingNodeType::DescriptorSampler:
  case ResourceMappingNodeType::DescriptorCombinedTexture:
  case ResourceMappingNodeType::DescriptorTexelBuffer:
    // Generic descriptors in the top level are not handled by the linker.
    return true;
  case ResourceMappingNodeType::InlineBuffer:
    // Loading from an inline buffer requires building a descriptor that is not handled by the linker.
    return true;
  default:
    break;
  }
  return false;
}

// =====================================================================================================================
// Returns true if resourceMapping contains a user data node entry with a descriptor type that is unsupported by
// relocatable shader compilation.
//
// @param [in] resourceMapping : resource mapping data, containing user data nodes
static bool hasUnrelocatableDescriptorNode(const ResourceMappingData *resourceMapping) {
  auto descriptorRangeValues = ArrayRef<StaticDescriptorValue>(resourceMapping->pStaticDescriptorValues,
                                                               resourceMapping->staticDescriptorValueCount);
  for (const auto &range : descriptorRangeValues) {
    if (range.type == ResourceMappingNodeType::DescriptorYCbCrSampler) {
      return true;
    }
  }

  for (unsigned i = 0; i < resourceMapping->userDataNodeCount; ++i) {
    if (isUnrelocatableResourceMappingRootNode(&resourceMapping->pUserDataNodes[i].node))
      return true;
  }

  // If there is no 1-to-1 mapping between descriptor sets and descriptor tables, then relocatable shaders will fail.
  SmallSet<unsigned, 8> descriptorSetsSeen;
  for (unsigned i = 0; i < resourceMapping->userDataNodeCount; ++i) {
    const ResourceMappingNode *node = &resourceMapping->pUserDataNodes[i].node;
    if (node->type != ResourceMappingNodeType::DescriptorTableVaPtr)
      continue;
    const ResourceMappingNode *innerNode = node->tablePtr.pNext;
    if (innerNode && !descriptorSetsSeen.insert(innerNode->srdRange.set).second)
      return true;
  }

  return false;
}

// =====================================================================================================================
// Returns true if a graphics pipeline can be built out of the given shader infos.
//
// @param shaderInfos : Shader infos for the pipeline to be built
// @param pipelineInfo : Pipeline info for the pipeline to be built
bool Compiler::canUseRelocatableGraphicsShaderElf(const ArrayRef<const PipelineShaderInfo *> &shaderInfos,
                                                  const GraphicsPipelineBuildInfo *pipelineInfo) {
  // Check user data nodes for unsupported Descriptor types.
  if (hasUnrelocatableDescriptorNode(&pipelineInfo->resourceMapping))
    return false;

  if (pipelineInfo->iaState.enableMultiView)
    return false;

  if (shaderInfos[0]) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfos[0]->pModuleData);
    if (moduleData && moduleData->binType != BinaryType::Spirv)
      return false;
  }

  if (cl::RelocatableShaderElfLimit != -1) {
    if (m_relocatablePipelineCompilations >= cl::RelocatableShaderElfLimit)
      return false;

    ++m_relocatablePipelineCompilations;
  }
  return true;
}

// =====================================================================================================================
// Returns true if a compute pipeline can be built out of the given shader info.
//
// @param shaderInfo : Shader info for the pipeline to be built
bool Compiler::canUseRelocatableComputeShaderElf(const ComputePipelineBuildInfo *pipelineInfo) {
  const PipelineShaderInfo *shaderInfo = &pipelineInfo->cs;
  if (shaderInfo) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
    if (moduleData && moduleData->binType != BinaryType::Spirv)
      return false;
  }

  // Check UserDataNode for unsupported Descriptor types.
  if (hasUnrelocatableDescriptorNode(&pipelineInfo->resourceMapping))
    return false;

  if (cl::RelocatableShaderElfLimit != -1) {
    if (m_relocatablePipelineCompilations >= cl::RelocatableShaderElfLimit)
      return false;

    ++m_relocatablePipelineCompilations;
  }
  return true;
}

// =====================================================================================================================
// Build pipeline internally -- common code for graphics and compute
//
// @param context : Acquired context
// @param shaderInfo : Shader info of this pipeline
// @param pipelineLink : WholePipeline = whole pipeline compile
//                       Unlinked = shader or part-pipeline compiled without pipeline state such as vertex fetch
// @param otherPartPipeline : Nullptr or the other Pipeline object containing FS input mappings for a part-pipeline
//                            compilation of the pre-rasterization stages
// @param [out] pipelineElf : Output Elf package
// @param [out] pipelineElf : Stage cache access info.
Result Compiler::buildPipelineInternal(Context *context, ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                       PipelineLink pipelineLink, Pipeline *otherPartPipeline, ElfPackage *pipelineElf,
                                       llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  Result result = Result::Success;
  unsigned passIndex = 0;
  const PipelineShaderInfo *fragmentShaderInfo = nullptr;
  TimerProfiler timerProfiler(context->getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);
  bool buildingRelocatableElf = context->getPipelineContext()->isUnlinked();

  bool hasError = false;
  context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

  // Set up middle-end objects.
  LgcContext *builderContext = context->getLgcContext();
  auto dialectGuard = llvm_dialects::withDialects(builderContext->getDialectContext());
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  context->getPipelineContext()->setPipelineState(&*pipeline, /*hasher=*/nullptr,
                                                  pipelineLink == PipelineLink::Unlinked);
  context->setBuilder(builderContext->createBuilder(&*pipeline));

  std::unique_ptr<Module> pipelineModule;

  // NOTE: If input is LLVM IR, read it now. There is now only ever one IR module representing the
  // whole pipeline.
  const PipelineShaderInfo *shaderInfoEntry = shaderInfo[0] ? shaderInfo[0] : shaderInfo.back();
  if (shaderInfoEntry) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);
    if (moduleData && moduleData->binType == BinaryType::LlvmBc)
      pipelineModule = context->loadLibrary(&moduleData->binCode);
  }

  // If not IR input, run the per-shader passes, including SPIR-V translation, and then link the modules
  // into a single pipeline module.
  if (!pipelineModule) {
    // Create empty modules and set target machine in each.
    std::vector<Module *> modules(shaderInfo.size());
    unsigned stageSkipMask = 0;
    for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
      const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
      if (!shaderInfoEntry || !shaderInfoEntry->pModuleData)
        continue;

      const ShaderModuleData *moduleDataEx = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);

      Module *module = nullptr;
      if (moduleDataEx->binType == BinaryType::MultiLlvmBc) {
        result = Result::ErrorInvalidShader;
      } else {
        module = new Module((Twine("llpc") + "_" + getShaderStageName(shaderInfoEntry->entryStage)).str() + "_" +
                                std::to_string(getModuleIdByIndex(shaderIndex)),
                            *context);
      }

      modules[shaderIndex] = module;
      context->setModuleTargetMachine(module);
    }

#if VKI_RAY_TRACING
    unsigned rayQueryLibraryIndex = InvalidValue;
    bool isInternalRtShader = false;
#endif
    for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
      const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
      ShaderStage entryStage = shaderInfoEntry ? shaderInfoEntry->entryStage : ShaderStageInvalid;

      if (entryStage == ShaderStageFragment)
        fragmentShaderInfo = shaderInfoEntry;
      if (!shaderInfoEntry || !shaderInfoEntry->pModuleData || (stageSkipMask & shaderStageToMask(entryStage)))
        continue;

      std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
      lowerPassMgr->setPassIndex(&passIndex);
      SpirvLower::registerPasses(*lowerPassMgr);

      // Start timer for translate.
      timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, true);

      // SPIR-V translation, then dump the result.
      lowerPassMgr->addPass(SpirvLowerTranslator(entryStage, shaderInfoEntry));
      if (EnableOuts()) {
        lowerPassMgr->addPass(
            PrintModulePass(outs(), "\n"
                                    "===============================================================================\n"
                                    "// LLPC SPIRV-to-LLVM translation results\n"));
      }

#if VKI_RAY_TRACING
      const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);
      if (moduleData->usage.enableRayQuery) {
        lowerPassMgr->addPass(SpirvLowerRayQuery(moduleData->usage.rayQueryLibrary));
        rayQueryLibraryIndex = shaderIndex;
      }

      isInternalRtShader = moduleData->usage.isInternalRtShader;

      if (isInternalRtShader)
        assert(entryStage == ShaderStageCompute);
#endif

      // Stop timer for translate.
      timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, false);

      bool success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
      if (!success) {
        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
        result = Result::ErrorInvalidShader;
      }
    }
#if VKI_RAY_TRACING
    std::vector<const PipelineShaderInfo *> newShaderInfos;
    bool rayQuery = (rayQueryLibraryIndex != InvalidValue);
    if (rayQuery) {
      LLPC_OUTS("// LLPC link ray query modules");
      assert(rayQueryLibraryIndex != InvalidValue);
      newShaderInfos.clear();
      auto shaderLibraryModule = modules[rayQueryLibraryIndex];
      modules.erase(modules.begin() + rayQueryLibraryIndex);
      for (unsigned shaderIndex = 0; shaderIndex < modules.size(); ++shaderIndex) {
        Module *module = modules[shaderIndex];
        newShaderInfos.push_back(shaderInfo[shaderIndex]);

        if (!module)
          continue;

        Linker linker(*module);
        // Module would be consumed after linked, need to clone module first in case there more than one graphics
        // shaders
        if (linker.linkInModule(CloneModule(*shaderLibraryModule))) {
          result = Result::ErrorInvalidShader;
        }
      }
      delete shaderLibraryModule;
      shaderInfo = newShaderInfos;
    }
#endif
    SmallVector<Module *, ShaderStageGfxCount> modulesToLink;
    for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
      // Per-shader SPIR-V lowering passes.
      const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
      ShaderStage entryStage = shaderInfoEntry ? shaderInfoEntry->entryStage : ShaderStageInvalid;
      if (!shaderInfoEntry || !shaderInfoEntry->pModuleData)
        continue;
      if (stageSkipMask & shaderStageToMask(entryStage)) {
        // Do not run SPIR-V translator and lowering passes on this shader; we were given it as IR ready
        // to link into pipeline module.
        modulesToLink.push_back(modules[shaderIndex]);
        continue;
      }

      std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
      lowerPassMgr->setPassIndex(&passIndex);
      SpirvLower::registerPasses(*lowerPassMgr);

      SpirvLower::addPasses(context, entryStage, *lowerPassMgr, timerProfiler.getTimer(TimerLower)
#if VKI_RAY_TRACING
                                                                    ,
                            false, rayQuery, isInternalRtShader
#endif
      );
      // Run the passes.
      bool success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
      if (!success) {
        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
        result = Result::ErrorInvalidShader;
      }

      // Add the shader module to the list for the pipeline.
      modulesToLink.push_back(modules[shaderIndex]);
    }

    // If this is a part-pipeline compile of the pre-rasterization stages, give the "other" pipeline object
    // containing the FS input mappings to our pipeline object.
    if (otherPartPipeline)
      pipeline->setOtherPartPipeline(*otherPartPipeline);

    // Link the shader modules into a single pipeline module.
    pipelineModule.reset(pipeline->irLink(
        modulesToLink, context->getPipelineContext()->isUnlinked() ? PipelineLink::Unlinked : pipelineLink));
    if (!pipelineModule) {
      LLPC_ERRS("Failed to link shader modules into pipeline module\n");
      result = Result::ErrorInvalidShader;
    }
  }

  // Set up function to check shader cache.
  GraphicsShaderCacheChecker graphicsShaderCacheChecker(this, context);

  Pipeline::CheckShaderCacheFunc checkShaderCacheFunc =
      // @param module : Module
      // @param stageMask : Shader stage mask
      // @param stageHashes : Per-stage hash of in/out usage
      // @returns : Stage mask of stages not found in cache
      [&graphicsShaderCacheChecker, stageCacheAccesses](const Module *module, unsigned stageMask,
                                                        ArrayRef<ArrayRef<uint8_t>> stageHashes) {
        return graphicsShaderCacheChecker.check(module, stageMask, stageHashes, stageCacheAccesses);
      };

  // Only enable per stage cache for full graphics pipeline (traditional pipeline or mesh pipeline)
  bool checkPerStageCache =
      cl::EnablePerStageCache && !cl::EnablePartPipeline && context->isGraphics() && !buildingRelocatableElf &&
      (context->getShaderStageMask() & (ShaderStageVertexBit | ShaderStageMeshBit | ShaderStageFragmentBit));

  if (!checkPerStageCache)
    checkShaderCacheFunc = nullptr;

  // Generate pipeline.
  raw_svector_ostream elfStream(*pipelineElf);

  if (result == Result::Success) {
#if LLPC_ENABLE_EXCEPTION
    result = Result::ErrorInvalidShader;
    try
#endif
    {
      Timer *timers[] = {
          timerProfiler.getTimer(TimerPatch),
          timerProfiler.getTimer(TimerOpt),
          timerProfiler.getTimer(TimerCodeGen),
      };

      pipeline->generate(std::move(pipelineModule), elfStream, checkShaderCacheFunc, timers);
#if LLPC_ENABLE_EXCEPTION
      result = Result::Success;
#endif
    }
#if LLPC_ENABLE_EXCEPTION
    catch (const char *) {
    }
#endif
  }
  if (checkPerStageCache) {
    // For graphics, update shader caches with results of compile, and merge ELF outputs if necessary.
    graphicsShaderCacheChecker.updateAndMerge(result, pipelineElf);
  }

  if (result == Result::Success && fragmentShaderInfo && fragmentShaderInfo->options.updateDescInElf &&
      (context->getShaderStageMask() & ShaderStageFragmentBit))
    graphicsShaderCacheChecker.updateRootUserDateOffset(pipelineElf);

  context->setDiagnosticHandler(nullptr);

  if (result == Result::Success && hasError)
    result = Result::ErrorInvalidShader;

  return result;
}

// =====================================================================================================================
// Check shader cache for graphics pipeline, returning mask of which shader stages we want to keep in this compile.
// This is called from the PatchCheckShaderCache pass (via a lambda in BuildPipelineInternal), to remove
// shader stages that we don't want because there was a shader cache hit.
//
// @param module : Module
// @param stageMask : Shader stage mask (NOTE: This is a LGC shader stage mask passed by middle-end)
// @param stageHashes : Per-stage hash of in/out usage
// @param [out] stageCacheAccesses : Stage cache access info to fill out
// @returns : Stage mask of the stages left to compile.
unsigned GraphicsShaderCacheChecker::check(const Module *module, const unsigned stageMask,
                                           ArrayRef<ArrayRef<uint8_t>> stageHashes,
                                           llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  // Check per stage shader cache
  MetroHash::Hash fragmentHash = {};
  MetroHash::Hash nonFragmentHash = {};
  Compiler::buildShaderCacheHash(m_context, stageMask, stageHashes, &fragmentHash, &nonFragmentHash);
  unsigned stagesLeftToCompile = stageMask;

  if (stageMask & getLgcShaderStageMask(ShaderStageFragment)) {
    m_fragmentCacheAccessor.emplace(m_context, fragmentHash, m_compiler->getInternalCaches());
    if (m_fragmentCacheAccessor->isInCache()) {
      // Remove fragment shader stages.
      stagesLeftToCompile &= ~getLgcShaderStageMask(ShaderStageFragment);
      stageCacheAccesses[ShaderStageFragment] =
          m_fragmentCacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
    } else {
      stageCacheAccesses[ShaderStageFragment] = CacheAccessInfo::CacheMiss;
    }
  }

  if (stageMask & ~getLgcShaderStageMask(ShaderStageFragment)) {
    auto accessInfo = CacheAccessInfo::CacheNotChecked;
    m_nonFragmentCacheAccessor.emplace(m_context, nonFragmentHash, m_compiler->getInternalCaches());
    if (m_nonFragmentCacheAccessor->isInCache()) {
      // Remove non-fragment shader stages.
      stagesLeftToCompile &= getLgcShaderStageMask(ShaderStageFragment);
      accessInfo = m_nonFragmentCacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit
                                                                  : CacheAccessInfo::CacheHit;
    } else {
      accessInfo = CacheAccessInfo::CacheMiss;
    }

    for (ShaderStage stage : gfxShaderStages())
      if (stage != ShaderStageFragment && (getLgcShaderStageMask(stage) & stageMask))
        stageCacheAccesses[stage] = accessInfo;
  }
  return stagesLeftToCompile;
}

// =====================================================================================================================
// Update root level descriptor offset for graphics pipeline.
//
// @param [in/out] pipelineElf : ELF that could be from compile or merged
void GraphicsShaderCacheChecker::updateRootUserDateOffset(ElfPackage *pipelineElf) {
  ElfWriter<Elf64> writer(m_context->getGfxIpVersion());
  // Load ELF binary
  auto result = writer.ReadFromBuffer(pipelineElf->data(), pipelineElf->size());
  assert(result == Result::Success);
  (void(result)); // unused
  writer.updateElfBinary(m_context, pipelineElf);
}

// =====================================================================================================================
// Update shader caches for graphics pipeline from compile result, and merge ELF outputs if necessary.
//
// @param result : Result of compile
// @param outputPipelineElf : ELF output of compile, updated to merge ELF from shader cache
void GraphicsShaderCacheChecker::updateAndMerge(Result result, ElfPackage *outputPipelineElf) {
  // Update the shader cache if required, with the compiled pipeline or with a failure state.
  bool needToMergeElf = false;
  BinaryData pipelineElf = {};
  pipelineElf.codeSize = outputPipelineElf->size();
  pipelineElf.pCode = outputPipelineElf->data();
  if (m_nonFragmentCacheAccessor) {
    if (!m_nonFragmentCacheAccessor->isInCache()) {
      m_nonFragmentCacheAccessor->setElfInCache(pipelineElf);
      LLPC_OUTS("Non fragment shader cache miss.\n");
    } else {
      needToMergeElf = true;
      LLPC_OUTS("Non fragment shader cache hit.\n");
    }
  }

  if (m_fragmentCacheAccessor) {
    if (!m_fragmentCacheAccessor->isInCache()) {
      m_fragmentCacheAccessor->setElfInCache(pipelineElf);
      LLPC_OUTS("Fragment shader cache miss.\n");
    } else {
      needToMergeElf = true;
      LLPC_OUTS("Fragment shader cache hit.\n");
    }
  }

  // Now merge ELFs if one or both parts are from the cache. Nothing needs to be merged if we just compiled the full
  // pipeline, as everything is already contained in the single incoming ELF in this case.
  if (needToMergeElf) {
    // Move the compiled ELF out of the way.
    ElfPackage compiledPipelineElf = std::move(*outputPipelineElf);
    outputPipelineElf->clear();

    // Determine where the fragment / non-fragment parts come from (cache or just-compiled).
    BinaryData fragmentElf = {};
    if (m_fragmentCacheAccessor && m_fragmentCacheAccessor->isInCache())
      fragmentElf = m_fragmentCacheAccessor->getElfFromCache();
    else {
      fragmentElf.pCode = compiledPipelineElf.data();
      fragmentElf.codeSize = compiledPipelineElf.size();
    }

    BinaryData nonFragmentElf = {};
    if ((m_nonFragmentCacheAccessor && m_nonFragmentCacheAccessor->isInCache()))
      nonFragmentElf = m_nonFragmentCacheAccessor->getElfFromCache();
    else {
      nonFragmentElf.pCode = compiledPipelineElf.data();
      nonFragmentElf.codeSize = compiledPipelineElf.size();
    }

    // Merge and store the result in pipelineElf
    ElfWriter<Elf64> writer(m_context->getGfxIpVersion());
    auto result = writer.ReadFromBuffer(nonFragmentElf.pCode, nonFragmentElf.codeSize);
    assert(result == Result::Success);
    (void(result)); // unused
    writer.mergeElfBinary(m_context, &fragmentElf, outputPipelineElf);
  }
}

// =====================================================================================================================
// Convert color buffer format to fragment shader export format
// This is not used in a normal compile; it is only used by amdllpc's -check-auto-layout-compatible option.
//
// @param target : GraphicsPipelineBuildInfo
// @param enableAlphaToCoverage : Whether enable AlphaToCoverage
unsigned Compiler::ConvertColorBufferFormatToExportFormat(const ColorTarget *target,
                                                          const bool enableAlphaToCoverage) const {
  Context *context = acquireContext();
  std::unique_ptr<Pipeline> pipeline(context->getLgcContext()->createPipeline());
  ColorExportFormat format = {};
  ColorExportState state = {};
  std::tie(format.dfmt, format.nfmt) = PipelineContext::mapVkFormat(target->format, true);
  format.blendEnable = target->blendEnable;
  format.blendSrcAlphaToColor = target->blendSrcAlphaToColor;
  state.alphaToCoverageEnable = enableAlphaToCoverage;
  pipeline->setColorExportState(format, state);

  Type *outputTy = FixedVectorType::get(Type::getFloatTy(*context), countPopulation(target->channelWriteMask));
  unsigned exportFormat = pipeline->computeExportFormat(outputTy, 0);

  pipeline.reset(nullptr);
  releaseContext(context);

  return exportFormat;
}

// =====================================================================================================================
// Build graphics pipeline internally
//
// @param graphicsContext : Graphics context this graphics pipeline
// @param shaderInfo : Shader info of this graphics pipeline
// @param buildingRelocatableElf : Build the pipeline by linking relocatable elf
// @param [out] pipelineElf : Output Elf package
// @param [out] stageCacheAccesses : Stage cache access result. All elements
//                                   must be initialized in the caller as
//                                   CacheAccessInfo::CacheNotChecked
Result Compiler::buildGraphicsPipelineInternal(GraphicsContext *graphicsContext,
                                               ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                               bool buildingRelocatableElf, ElfPackage *pipelineElf,
                                               MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  Context *context = acquireContext();
  context->attachPipelineContext(graphicsContext);
  Result result = Result::ErrorUnavailable;
  if (buildingRelocatableElf) {
    result = buildPipelineWithRelocatableElf(context, shaderInfo, pipelineElf, stageCacheAccesses);
  }

  if (result != Result::Success) {
    // See if we want to use part-pipeline compilation, if there is an FS and at least one pre-rasterization
    // shader stage.
    unsigned stageMask = context->getShaderStageMask();
    bool buildPartPipeline = (cl::EnablePartPipeline && isShaderStageInMask(ShaderStageFragment, stageMask) &&
                              (stageMask & ~shaderStageToMask(ShaderStageFragment)));
#if VKI_RAY_TRACING
    buildPartPipeline &= !graphicsContext->hasRayQuery();
#endif
    if (buildPartPipeline)
      result = buildGraphicsPipelineWithPartPipelines(context, shaderInfo, pipelineElf, stageCacheAccesses);
    else
      result = buildPipelineInternal(context, shaderInfo, PipelineLink::WholePipeline, nullptr, pipelineElf,
                                     stageCacheAccesses);
  }
  releaseContext(context);
  return result;
}

// =====================================================================================================================
// Build graphics pipeline using part-pipeline compilation
//
// @param graphicsContext : Graphics context this graphics pipeline
// @param shaderInfo : Shader info of this graphics pipeline
// @param [out] pipelineElf : Output Elf package. If an error occurs, the content is undefined.
// @param [out] stageCacheAccesses : Stage cache access result. All elements must be initialized in the caller as
//                                   CacheAccessInfo::CacheNotChecked
Result Compiler::buildGraphicsPipelineWithPartPipelines(Context *context,
                                                        ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                                        ElfPackage *pipelineElf,
                                                        MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  unsigned wholeStageMask = context->getShaderStageMask();
  ElfPackage partPipelineBuffers[enumCount<PartPipelineStage>()];

  // Create the linker and its pipeline object.
  std::unique_ptr<Pipeline> elfLinkerPipeline(context->getLgcContext()->createPipeline());
  std::unique_ptr<ElfLinker> elfLinker(elfLinkerPipeline->createElfLinker({}));

  // Give selected pipeline state to the linker's pipeline object, required to complete the ELF link.
  // We pass unlinked=true as we do not need to pass user data layout into the ELF link.
  context->getPipelineContext()->setPipelineState(&*elfLinkerPipeline, /*hasher=*/nullptr, /*unlinked=*/true);

  bool textualOutput = false;
  // For each of the two part pipelines (fragment first, then pre-rasterization)...
  for (auto partPipelineStage : lgc::enumRange<PartPipelineStage>()) {
    // Compile the part pipelines, or find it in the cache.
    llvm::StringRef partPipelineElf;
    unsigned partStageMask = partPipelineStage == PartPipelineStageFragment
                                 ? shaderStageToMask(ShaderStageFragment)
                                 : wholeStageMask & ~shaderStageToMask(ShaderStageFragment);
    context->getPipelineContext()->setShaderStageMask(partStageMask);

    if (partPipelineStage == PartPipelineStageFragment && isShaderStageInMask(ShaderStageGeometry, wholeStageMask))
      context->getPipelineContext()->setPreRasterHasGs(true);

    // Hash the selected shaders and the pipeline state applicable to them.
    Util::MetroHash64 hasher;
    for (const PipelineShaderInfo *shaderInfoEntry : shaderInfo) {
      if (shaderInfoEntry) {
        ShaderStage stage = shaderInfoEntry->entryStage;
        if (shaderInfoEntry->pModuleData && isShaderStageInMask(stage, partStageMask)) {
          // This is a shader to include in this part pipeline. Add the shader code and options to the hash.
          PipelineDumper::updateHashForPipelineShaderInfo(stage, shaderInfoEntry, /*isCacheHash=*/true, &hasher,
                                                          /*isRelocatableShader=*/false);
        }
      }
    }
    // Add applicable pipeline state to the hash. (This uses getShaderStageMask() to decide which parts of the
    // state are applicable.)
    context->getPipelineContext()->setPipelineState(/*pipeline=*/nullptr, /*hasher=*/&hasher, /*unlinked=*/false);

    // If we are doing the pre-rasterization part pipeline, the linker's pipeline object contains the FS input mapping
    // state. We need to include that state in the hash.
    if (partPipelineStage == PartPipelineStagePreRasterization && elfLinker->haveFsInputMappings()) {
      assert(elfLinker);
      StringRef fsInputMappings = elfLinker->getFsInputMappings();
      hasher.Update(reinterpret_cast<const uint8_t *>(fsInputMappings.data()), fsInputMappings.size());
    }

    // Finalize the hash, and look it up in the cache.
    MetroHash::Hash partPipelineHash = {};
    hasher.Finalize(partPipelineHash.bytes);
    CacheAccessor cacheAccessor(context, partPipelineHash, getInternalCaches());
    if (cacheAccessor.isInCache()) {
      LLPC_OUTS("Cache hit for stage " << getPartPipelineStageName(partPipelineStage) << ".\n");

      // Mark the applicable entries in stageCacheAccesses.
      for (ShaderStage shaderStage : maskToShaderStages(partStageMask)) {
        stageCacheAccesses[shaderStage] =
            cacheAccessor.hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
      }
      // Get the ELF from the cache.
      partPipelineElf = llvm::StringRef(static_cast<const char *>(cacheAccessor.getElfFromCache().pCode),
                                        cacheAccessor.getElfFromCache().codeSize);
    } else {
      LLPC_OUTS("Cache miss for stage " << getPartPipelineStageName(partPipelineStage) << ".\n");

      // Compile the part pipeline.
      // Keep the PipelineShaderInfo structs for the shaders we are including.
      llvm::SmallVector<const PipelineShaderInfo *, 4> partPipelineShaderInfo;
      for (const PipelineShaderInfo *oneShaderInfo : shaderInfo) {
        if (oneShaderInfo && isShaderStageInMask(oneShaderInfo->entryStage, partStageMask))
          partPipelineShaderInfo.push_back(oneShaderInfo);
      }
      auto otherPartPipeline =
          (!textualOutput && partPipelineStage == PartPipelineStagePreRasterization) ? &*elfLinkerPipeline : nullptr;
      Result result =
          buildPipelineInternal(context, partPipelineShaderInfo, PipelineLink::PartPipeline, otherPartPipeline,
                                &partPipelineBuffers[partPipelineStage], stageCacheAccesses);
      if (result != Result::Success)
        return result;
      partPipelineElf = partPipelineBuffers[partPipelineStage];

      // If the "ELF" does not look like ELF, then it must be textual output from -emit-lgc, -emit-llvm, -filetype=asm.
      // We can't link that, so just concatenate it on to the output.
      if (partPipelineElf.size() < 4 || !partPipelineElf.startswith("\177ELF")) {
        const unsigned char magic[] = {'B', 'C', 0xC0, 0xDE};
        if (partPipelineElf.size() > 4 && memcmp(partPipelineElf.data(), magic, sizeof(magic)) == 0)
          report_fatal_error("Cannot emit llvm bitcode with part pipeline compilation.");
        if (textualOutput)
          *pipelineElf += "Part Pipeline:\n";
        *pipelineElf += partPipelineElf;
        textualOutput = true;
        continue;
      }

      // Store the part-pipeline ELF in the cache.
      cacheAccessor.setElfInCache(BinaryData({partPipelineElf.size(), partPipelineElf.data()}));
    }

    if (Llpc::EnableOuts()) {
      ElfReader<Elf64> reader(m_gfxIp);
      size_t readSize = 0;
      if (reader.ReadFromBuffer(partPipelineElf.data(), &readSize) == Result::Success) {
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC part-pipeline ELF (from cache or just compiled)\n");
        LLPC_OUTS(reader);
      }
    }

    // Add the ELF to the linker.
    elfLinker->addInputElf(llvm::MemoryBufferRef(partPipelineElf, ""));
  }

  Result result = Result::Success;
  // Link the two part-pipelines if the output is not textual.
  if (textualOutput)
    return result;
  raw_svector_ostream elfStream(*pipelineElf);
  bool ok = elfLinker->link(elfStream);
  if (!ok) {
    errs() << elfLinkerPipeline->getLastError() << "\n";
    result = Result::ErrorUnavailable;
    pipelineElf->clear();
  }

  return result;
}

// =====================================================================================================================
// Build graphics pipeline from the specified info.
//
// @param pipelineInfo : Info to build this graphics pipeline
// @param [out] pipelineOut : Output of building this graphics pipeline
// @param pipelineDumpFile : Handle of pipeline dump file
Result Compiler::BuildGraphicsPipeline(const GraphicsPipelineBuildInfo *pipelineInfo,
                                       GraphicsPipelineBuildOut *pipelineOut, void *pipelineDumpFile) {
  Result result = Result::Success;
  BinaryData elfBin = {};
  // clang-format off
  SmallVector<const PipelineShaderInfo *, ShaderStageGfxCount> shaderInfo = {
    &pipelineInfo->task,
    &pipelineInfo->vs,
    &pipelineInfo->tcs,
    &pipelineInfo->tes,
    &pipelineInfo->gs,
    &pipelineInfo->mesh,
    &pipelineInfo->fs,
  };
  // clang-format on
  const bool relocatableElfRequested = pipelineInfo->options.enableRelocatableShaderElf || cl::UseRelocatableShaderElf;
  const bool buildUsingRelocatableElf =
      relocatableElfRequested && canUseRelocatableGraphicsShaderElf(shaderInfo, pipelineInfo);

  for (ShaderStage stage : gfxShaderStages()) {
    result = validatePipelineShaderInfo(shaderInfo[stage]);
    if (result != Result::Success)
      break;
  }

#if VKI_RAY_TRACING
  // Setup the rayquery Graphics PipelineShaderinfo
  PipelineShaderInfo rayQueryShaderInfo = {};
  ShaderModuleData rayQueryModuleData = {};
  if (pipelineInfo->shaderLibrary.codeSize > 0) {
    rayQueryModuleData.binCode = pipelineInfo->shaderLibrary;
    rayQueryModuleData.binType = BinaryType::Spirv;
    rayQueryModuleData.usage.keepUnusedFunctions = true;
    rayQueryModuleData.usage.rayQueryLibrary = true;
    rayQueryModuleData.usage.enableRayQuery = true;

    rayQueryShaderInfo.entryStage = ShaderStageCompute;
    rayQueryShaderInfo.pEntryTarget = Vkgc::getEntryPointNameFromSpirvBinary(&pipelineInfo->shaderLibrary);
    rayQueryShaderInfo.pModuleData = &rayQueryModuleData;

    bool hwIntersectRay = pipelineInfo->rtState.bvhResDesc.dataSizeInDwords > 0;
    // Disable fast math Contract when there is no hardware intersectRay
    rayQueryShaderInfo.options.noContract = !hwIntersectRay;

    // Add the rayquery PipelineShaderinfo
    shaderInfo.push_back(&rayQueryShaderInfo);
  }
#endif

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, false);
  pipelineHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false, false);

  if (result == Result::Success && EnableOuts()) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC calculated hash results (graphics pipeline)\n\n");
    LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::compact64(&pipelineHash)) << "\n");
    for (ShaderStage stage : gfxShaderStages()) {
      auto moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo[stage]->pModuleData);
      if (moduleData) {
        auto hash = reinterpret_cast<const MetroHash::Hash *>(&moduleData->hash[0]);
        LLPC_OUTS(format("%-4s : ", getShaderStageAbbreviation(stage, true))
                  << format("0x%016" PRIX64, MetroHash::compact64(hash)) << "\n");
      }
    }
    if (relocatableElfRequested && !buildUsingRelocatableElf) {
      LLPC_OUTS("\nWarning: Relocatable shader compilation requested but not possible. "
                << "Falling back to whole-pipeline compilation.\n");
    }
    LLPC_OUTS("\n");
  }

  if (result == Result::Success)
    dumpCompilerOptions(pipelineDumpFile);

  Optional<CacheAccessor> cacheAccessor;
  if (cl::CacheFullPipelines) {
    cacheAccessor.emplace(pipelineInfo, cacheHash, getInternalCaches());
  }

  ElfPackage candidateElf;

  if (!cacheAccessor || !cacheAccessor->isInCache()) {
    LLPC_OUTS("Cache miss for graphics pipeline.\n");
    GraphicsContext graphicsContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
    result = buildGraphicsPipelineInternal(&graphicsContext, shaderInfo, buildUsingRelocatableElf, &candidateElf,
                                           pipelineOut->stageCacheAccesses);

    if (result == Result::Success) {
      elfBin.codeSize = candidateElf.size();
      elfBin.pCode = candidateElf.data();
    }

    if (cacheAccessor && pipelineOut->pipelineCacheAccess == CacheAccessInfo::CacheNotChecked)
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::CacheMiss;
  } else {
    LLPC_OUTS("Cache hit for graphics pipeline.\n");
    elfBin = cacheAccessor->getElfFromCache();
    if (cacheAccessor->isInCache()) {
      pipelineOut->pipelineCacheAccess =
          cacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
    }
  }

  if (result == Result::Success) {
    void *allocBuf = nullptr;
    if (pipelineInfo->pfnOutputAlloc) {
      allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, elfBin.codeSize);

      uint8_t *code = static_cast<uint8_t *>(allocBuf);
      memcpy(code, elfBin.pCode, elfBin.codeSize);

      pipelineOut->pipelineBin.codeSize = elfBin.codeSize;
      pipelineOut->pipelineBin.pCode = code;
    } else {
      // Allocator is not specified
      result = Result::ErrorInvalidPointer;
    }
  }

  if (cacheAccessor && !cacheAccessor->isInCache() && result == Result::Success) {
    LLPC_OUTS("Adding graphics pipeline to the cache.\n");
    cacheAccessor->setElfInCache(elfBin);
  }
  return result;
}

// =====================================================================================================================
// Build compute pipeline internally
//
// @param computeContext : Compute context this compute pipeline
// @param pipelineInfo : Pipeline info of this compute pipeline
// @param buildingRelocatableElf : Build the pipeline by linking relocatable elf
// @param [out] pipelineElf : Output Elf package
// @param [out] stageCacheAccess : Compute shader stage cache access result
Result Compiler::buildComputePipelineInternal(ComputeContext *computeContext,
                                              const ComputePipelineBuildInfo *pipelineInfo, bool buildingRelocatableElf,
                                              ElfPackage *pipelineElf, CacheAccessInfo *stageCacheAccess) {
  Context *context = acquireContext();
  context->attachPipelineContext(computeContext);

  std::vector<const PipelineShaderInfo *> shadersInfo = {
      nullptr,          ///< Task shader
      nullptr,          ///< Vertex shader
      nullptr,          ///< Tessellation control shader
      nullptr,          ///< Tessellation evaluation shader
      nullptr,          ///< Geometry shader
      nullptr,          ///< Mesh shader
      nullptr,          ///< Fragment shader
      &pipelineInfo->cs ///< Compute shader
  };

#if VKI_RAY_TRACING
  PipelineShaderInfo rayQueryShaderInfo = pipelineInfo->cs;
  ShaderModuleData rayQueryModuleData = {};
  if (pipelineInfo->shaderLibrary.codeSize > 0) {
    // Setup the rayquery PipelineShaderinfo
    rayQueryModuleData.binCode = pipelineInfo->shaderLibrary;
    rayQueryModuleData.binType = BinaryType::Spirv;
    rayQueryModuleData.usage.keepUnusedFunctions = true;
    rayQueryModuleData.usage.rayQueryLibrary = true;
    rayQueryModuleData.usage.enableRayQuery = true;

    rayQueryShaderInfo.entryStage = ShaderStageCompute;
    rayQueryShaderInfo.pEntryTarget = Vkgc::getEntryPointNameFromSpirvBinary(&pipelineInfo->shaderLibrary);
    rayQueryShaderInfo.pModuleData = &rayQueryModuleData;

    bool hwIntersectRay = pipelineInfo->rtState.bvhResDesc.dataSizeInDwords > 0;
    // Disable fast math Contract when there is no hardware intersectRay
    rayQueryShaderInfo.options.noContract = !hwIntersectRay;
    // Add the rayquery PipelineShaderinfo
    shadersInfo.push_back(&rayQueryShaderInfo);
  }
#endif
  Result result = Result::ErrorUnavailable;
  CacheAccessInfo stageCacheAccesses[ShaderStageCount] = {};
  if (buildingRelocatableElf) {
    result = buildPipelineWithRelocatableElf(context, shadersInfo, pipelineElf, stageCacheAccesses);
    *stageCacheAccess = stageCacheAccesses[ShaderStageCompute];
  }

  if (result != Result::Success)
    result = buildPipelineInternal(context, shadersInfo, PipelineLink::WholePipeline, nullptr, pipelineElf,
                                   stageCacheAccesses);
  releaseContext(context);
  return result;
}

// =====================================================================================================================
// Build compute pipeline from the specified info.
//
// @param pipelineInfo : Info to build this compute pipeline
// @param [out] pipelineOut : Output of building this compute pipeline
// @param pipelineDumpFile : Handle of pipeline dump file
Result Compiler::BuildComputePipeline(const ComputePipelineBuildInfo *pipelineInfo,
                                      ComputePipelineBuildOut *pipelineOut, void *pipelineDumpFile) {
  BinaryData elfBin = {};

  const bool relocatableElfRequested = pipelineInfo->options.enableRelocatableShaderElf || cl::UseRelocatableShaderElf;
  const bool buildUsingRelocatableElf = relocatableElfRequested && canUseRelocatableComputeShaderElf(pipelineInfo);

  Result result = validatePipelineShaderInfo(&pipelineInfo->cs);

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, true, false);
  pipelineHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, false, false);

  if (result == Result::Success && EnableOuts()) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(pipelineInfo->cs.pModuleData);
    auto moduleHash = reinterpret_cast<const MetroHash::Hash *>(&moduleData->hash[0]);
    LLPC_OUTS("\n===============================================================================\n");
    LLPC_OUTS("// LLPC calculated hash results (compute pipeline)\n\n");
    LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::compact64(&pipelineHash)) << "\n");
    LLPC_OUTS(format("%-4s : ", getShaderStageAbbreviation(ShaderStageCompute, true))
              << format("0x%016" PRIX64, MetroHash::compact64(moduleHash)) << "\n");
    if (relocatableElfRequested && !buildUsingRelocatableElf) {
      LLPC_OUTS("\nWarning: Relocatable shader compilation requested but not possible. "
                << "Falling back to whole-pipeline compilation.\n");
    }
    LLPC_OUTS("\n");
  }

  if (result == Result::Success)
    dumpCompilerOptions(pipelineDumpFile);

  Optional<CacheAccessor> cacheAccessor;
  if (cl::CacheFullPipelines) {
    cacheAccessor.emplace(pipelineInfo, cacheHash, getInternalCaches());
  }

  ElfPackage candidateElf;
  if (!cacheAccessor || !cacheAccessor->isInCache()) {
    LLPC_OUTS("Cache miss for compute pipeline.\n");
    ComputeContext computeContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
    result = buildComputePipelineInternal(&computeContext, pipelineInfo, buildUsingRelocatableElf, &candidateElf,
                                          &pipelineOut->stageCacheAccess);

    if (result == Result::Success) {
      elfBin.codeSize = candidateElf.size();
      elfBin.pCode = candidateElf.data();
    }
    if (cacheAccessor && pipelineOut->pipelineCacheAccess == CacheAccessInfo::CacheNotChecked)
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::CacheMiss;
  } else {
    LLPC_OUTS("Cache hit for compute pipeline.\n");
    elfBin = cacheAccessor->getElfFromCache();
    pipelineOut->pipelineCacheAccess =
        cacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
  }

  if (result == Result::Success) {
    void *allocBuf = nullptr;
    if (pipelineInfo->pfnOutputAlloc) {
      allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, elfBin.codeSize);
      if (allocBuf) {
        uint8_t *code = static_cast<uint8_t *>(allocBuf);
        memcpy(code, elfBin.pCode, elfBin.codeSize);

        pipelineOut->pipelineBin.codeSize = elfBin.codeSize;
        pipelineOut->pipelineBin.pCode = code;
      } else
        result = Result::ErrorOutOfMemory;
    } else {
      // Allocator is not specified
      result = Result::ErrorInvalidPointer;
    }
  }

  if (cacheAccessor && !cacheAccessor->isInCache() && result == Result::Success) {
    cacheAccessor->setElfInCache(elfBin);
  }
  return result;
}

#if VKI_RAY_TRACING
// =====================================================================================================================
// Build ray tracing pipeline from the specified info.
//
// @param pipelineInfo : Info to build this ray tracing pipeline
// @param [out] pipelineOut : Output of building this ray tracing pipeline
// @param pipelineDumpFile : Handle of pipeline dump file
Result Compiler::BuildRayTracingPipeline(const RayTracingPipelineBuildInfo *pipelineInfo,
                                         RayTracingPipelineBuildOut *pipelineOut, void *pipelineDumpFile,
                                         IHelperThreadProvider *helperThreadProvider) {
  Result result = Result::Success;

  for (unsigned i = 0; i < pipelineInfo->shaderCount; ++i) {
    const auto shaderInfo = &pipelineInfo->pShaders[i];
    result = validatePipelineShaderInfo(shaderInfo);
  }

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForRayTracingPipeline(pipelineInfo, true);
  pipelineHash = PipelineDumper::generateHashForRayTracingPipeline(pipelineInfo, false);

  if (result == Result::Success && EnableOuts()) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC calculated hash results (ray tracing pipeline)\n\n");
    LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::compact64(&pipelineHash)) << "\n");
    for (unsigned i = 0; i < pipelineInfo->shaderCount; ++i) {
      const auto shaderInfo = &pipelineInfo->pShaders[i];
      ShaderStage entryStage = shaderInfo ? shaderInfo->entryStage : ShaderStageInvalid;
      const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
      auto hash = reinterpret_cast<const MetroHash::Hash *>(&moduleData->hash[0]);
      if (moduleData) {
        LLPC_OUTS(format("%-4s : ", getShaderStageAbbreviation(entryStage, true))
                  << format("0x%016" PRIX64, MetroHash::compact64(hash)) << "\n");
      }
    }
    LLPC_OUTS("\n");
  }

  if (result == Result::Success && pipelineDumpFile) {
    std::stringstream strStream;
    strStream << ";Compiler Options: ";
    for (auto &option : m_options)
      strStream << option << " ";
    std::string extraInfo = strStream.str();
    PipelineDumper::DumpPipelineExtraInfo(reinterpret_cast<PipelineDumpFile *>(pipelineDumpFile), &extraInfo);
  }

  ShaderEntryState cacheEntryState = ShaderEntryState::Compiling;

  std::vector<ElfPackage> elfBinarys;
  std::vector<RayTracingShaderProperty> shaderProps;

  if (cacheEntryState == ShaderEntryState::Compiling) {

    ShaderModuleData traceRayModuleData = {};
    traceRayModuleData.binCode = pipelineInfo->shaderTraceRay;
    traceRayModuleData.binType = BinaryType::Spirv;
    traceRayModuleData.usage.keepUnusedFunctions = true;

    // Using the raytracing pipeline first shader to initialize trace ray pipeline shader info
    PipelineShaderInfo traceRayShaderInfo = pipelineInfo->pShaders[0];
    traceRayShaderInfo.entryStage = ShaderStageCompute;
    traceRayShaderInfo.pModuleData = &traceRayModuleData;
    traceRayShaderInfo.pEntryTarget = Vkgc::getEntryPointNameFromSpirvBinary(&pipelineInfo->shaderTraceRay);

    bool hwIntersectRay = pipelineInfo->rtState.bvhResDesc.dataSizeInDwords > 0;
    // Disable fast math Contract when there is no hardware intersectRay
    traceRayShaderInfo.options.noContract = !hwIntersectRay;

    RayTracingContext rayTracingContext(m_gfxIp, pipelineInfo, &traceRayShaderInfo, &pipelineHash, &cacheHash,
                                        pipelineInfo->indirectStageMask);

    Context *context = acquireContext();
    context->attachPipelineContext(&rayTracingContext);

    // Raytracing modules: pipeline shader count + entryModule
    unsigned modulesCount = pipelineInfo->shaderCount + 1;

    // Set pipeline has trace ray module by default
    pipelineOut->hasTraceRay = true;
    unsigned i = 0;

    if (pipelineInfo->rtState.pipelineFlags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) {
      // Library  pipeline usually does not have trace ray module
      pipelineOut->hasTraceRay = false;
      for (i = 0; i < pipelineInfo->shaderCount; ++i) {
        const auto &shaderInfo = pipelineInfo->pShaders[i];
        // Check raygen spirv module have OpTraceRayKHR or not
        if (shaderInfo.entryStage == ShaderStageRayTracingRayGen) {
          const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo.pModuleData);
          if (moduleData->usage.hasTraceRay) {
            pipelineOut->hasTraceRay = true;
            break;
          }
        }
      }
    }

    // For traceray module, update the modules count
    modulesCount = pipelineOut->hasTraceRay ? modulesCount + 1 : modulesCount;

    std::vector<const PipelineShaderInfo *> rayTracingShaderInfo(modulesCount);
    for (i = 0; i < pipelineInfo->shaderCount; ++i) {
      rayTracingShaderInfo[i] = &pipelineInfo->pShaders[i];
    }

    // Add entry module
    PipelineShaderInfo raygenMainShaderInfo = pipelineInfo->pShaders[0];
    raygenMainShaderInfo.entryStage = ShaderStageRayTracingRayGen;
    raygenMainShaderInfo.pModuleData = nullptr;
    rayTracingShaderInfo[i++] = &raygenMainShaderInfo;

    if (i < modulesCount) {
      rayTracingShaderInfo[i] = &traceRayShaderInfo;
    }

    result = buildRayTracingPipelineInternal(context, rayTracingShaderInfo, false, elfBinarys, shaderProps,
                                             helperThreadProvider);
    releaseContext(context);
  }

  if (result == Result::Success) {
    void *allocBuf = nullptr;
    size_t shaderGroupHandleSize = pipelineInfo->shaderGroupCount * sizeof(RayTracingShaderIdentifier);
    size_t binaryDataSize = sizeof(BinaryData) * elfBinarys.size();
    size_t elfSize = 0;

    for (auto &elf : elfBinarys)
      elfSize += elf.size();

    size_t allocSize = elfSize;
    allocSize += binaryDataSize;

    size_t shaderPropsSize = sizeof(RayTracingShaderProperty) * shaderProps.size();
    allocSize += shaderPropsSize;

    allocSize += shaderGroupHandleSize;

    if (pipelineInfo->pfnOutputAlloc)
      allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, allocSize);
    else {
      // Allocator is not specified
      result = Result::ErrorInvalidPointer;
    }

    pipelineOut->pipelineBinCount = elfBinarys.size();
    BinaryData *pipelineBins = reinterpret_cast<BinaryData *>(voidPtrInc(allocBuf, elfSize));

    unsigned elfIndex = 0;
    for (auto &elf : elfBinarys) {
      uint8_t *code = static_cast<uint8_t *>(allocBuf);
      memcpy(code, elf.data(), elf.size());
      allocBuf = voidPtrInc(allocBuf, elf.size());
      pipelineBins[elfIndex].codeSize = elf.size();
      pipelineBins[elfIndex++].pCode = code;
    }
    pipelineOut->pipelineBins = pipelineBins;

    allocBuf = voidPtrInc(allocBuf, binaryDataSize);
    pipelineOut->shaderPropSet.shaderCount = shaderProps.size();
    pipelineOut->shaderPropSet.traceRayIndex = shaderProps.size() - 1;
    if (!shaderProps.empty()) {
      RayTracingShaderProperty *shaderProp = reinterpret_cast<RayTracingShaderProperty *>(allocBuf);
      memcpy(shaderProp, &shaderProps[0], shaderPropsSize);
      pipelineOut->shaderPropSet.shaderProps = shaderProp;
      allocBuf = voidPtrInc(allocBuf, shaderPropsSize);
    }

    // Get to the address of shaderGroupHandles pass elfCode size
    RayTracingShaderIdentifier *shaderHandles = reinterpret_cast<RayTracingShaderIdentifier *>(allocBuf);
    memset(shaderHandles, 0, shaderGroupHandleSize);
    pipelineOut->shaderGroupHandle.shaderHandles = shaderHandles;
    pipelineOut->shaderGroupHandle.shaderHandleCount = pipelineInfo->shaderGroupCount;

    for (unsigned i = 0; i < pipelineInfo->shaderGroupCount; ++i) {
      auto shaderGroup = &pipelineInfo->pShaderGroups[i];
      if (shaderGroup->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR) {
        if (shaderGroup->generalShader != VK_SHADER_UNUSED_KHR)
          shaderHandles[i].shaderId = getModuleIdByIndex(shaderGroup->generalShader);
      } else if (shaderGroup->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR ||
                 shaderGroup->type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR) {
        if (shaderGroup->closestHitShader != VK_SHADER_UNUSED_KHR)
          shaderHandles[i].shaderId = getModuleIdByIndex(shaderGroup->closestHitShader);

        if (shaderGroup->anyHitShader != VK_SHADER_UNUSED_KHR)
          shaderHandles[i].anyHitId = getModuleIdByIndex(shaderGroup->anyHitShader);

        if (shaderGroup->intersectionShader != VK_SHADER_UNUSED_KHR)
          shaderHandles[i].intersectionId = getModuleIdByIndex(shaderGroup->intersectionShader);
      }
    }
  }

  return result;
}

// =====================================================================================================================
// Build single ray tracing pipeline ELF package.
//
// @param context : Acquired context
// @param module : The module used to build Elf package
// @param [out] pipelineElf : Output Elf package
// @param [out] shaderProps : Output RayTracingShaderProperty
// @param moduleCallsTraceRay : Whether a module calls OpTraceRay
// @param moduleIndex : Current processing module index
// @param pipeline : The pipeline object
// @param timerProfiler : Timer profiler
Result Compiler::buildRayTracingPipelineElf(Context *context, Module *module, ElfPackage &pipelineElf,
                                            std::vector<RayTracingShaderProperty> &shaderProps,
                                            std::vector<bool> &moduleCallsTraceRay, unsigned moduleIndex,
                                            std::unique_ptr<Pipeline> &pipeline, TimerProfiler &timerProfiler) {
  // Per-shader SPIR-V lowering passes.
  unsigned passIndex = 0;
  std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
  lowerPassMgr->setPassIndex(&passIndex);
  SpirvLower::registerPasses(*lowerPassMgr);

  SpirvLower::addPasses(context, ShaderStageCompute, *lowerPassMgr, timerProfiler.getTimer(TimerLower), true, false,
                        false);
  // Run the passes.
  bool success = runPasses(&*lowerPassMgr, module);
  if (!success) {
    LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
    return Result::ErrorInvalidShader;
  }

  if (moduleIndex > 0) {
    const StringRef &funcName = module->getName();
    assert(funcName.size() <= RayTracingMaxShaderNameLength);
    auto pos = funcName.rfind('_');
    int index = 0;
    funcName.substr(pos + 1).consumeInteger(0, index);
    auto &shaderProp = shaderProps[moduleIndex - 1];
    shaderProp.shaderId = index;
    strcpy(&shaderProp.name[0], funcName.data());

    shaderProp.hasTraceRay = moduleCallsTraceRay[moduleIndex - 1];

    // NOTE: We need to distinguish each pipelines's cache hash from others, so that RGP can capture them correctly.
    auto options = pipeline->getOptions();
    uint64_t originalCacheHash = options.hash[1];

    // Re-hash the cache hash with module index.
    MetroHash64 hasher;
    MetroHash::Hash hash = {};
    hasher.Update(originalCacheHash);
    hasher.Update(moduleIndex);
    hasher.Finalize(hash.bytes);

    options.hash[1] = MetroHash::compact64(&hash);
    pipeline->setOptions(options);
  }

  // Generate pipeline.
  std::unique_ptr<Module> pipelineModule;

  pipelineModule.reset(pipeline->irLink(
      module, context->getPipelineContext()->isUnlinked() ? PipelineLink::Unlinked : PipelineLink::WholePipeline));
  if (!pipelineModule) {
    LLPC_ERRS("Failed to link shader modules into pipeline module\n");
    return Result::ErrorInvalidShader;
  }

  ElfPackage *elfPackage = &pipelineElf;
  raw_svector_ostream elfStream(*elfPackage);

#if LLPC_ENABLE_EXCEPTION
  try
#endif
  {
    Timer *timers[] = {
        timerProfiler.getTimer(TimerPatch),
        timerProfiler.getTimer(TimerOpt),
        timerProfiler.getTimer(TimerCodeGen),
    };

    pipeline->generate(std::move(pipelineModule), elfStream, nullptr, timers);
  }
#if LLPC_ENABLE_EXCEPTION
  catch (const char *) {
    return Result::ErrorInvalidShader;
  }
#endif

  if (moduleIndex > 0)
    addRayTracingIndirectPipelineMetadata(elfPackage);

  return Result::Success;
}

// =====================================================================================================================
// Build single ray tracing pipeline ELF package.
//
// @param IHelperThreadProvider : The helper thread provider
// @param payload : Payload to build ray tracing pipeline Elf package
void helperThreadBuildRayTracingPipelineElf(IHelperThreadProvider *helperThreadProvider, void *payload) {
  HelperThreadBuildRayTracingPipelineElfPayload *helperThreadPayload =
      static_cast<HelperThreadBuildRayTracingPipelineElfPayload *>(payload);

  helperThreadPayload->helperThreadJoined = true;

  unsigned moduleIndex = 0;

  // No remaining tasks, do not proceed
  if (helperThreadProvider->GetNextTask(&moduleIndex) == false)
    return;

  // Set up context for each helper thread
  Context *context = helperThreadPayload->compiler->acquireContext();

  bool hasError = false;
  context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

  context->attachPipelineContext(helperThreadPayload->rayTracingContext);

  LgcContext *builderContext = context->getLgcContext();
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  helperThreadPayload->rayTracingContext->setPipelineState(&*pipeline, /*hasher=*/nullptr, false);
  context->setBuilder(builderContext->createBuilder(&*pipeline));

  TimerProfiler timerProfiler(context->getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);

  {
    // Block the helper thread until main thread has switched context, see comment in
    // Compiler::buildRayTracingPipelineInternal for why we need this.
    std::unique_lock<sys::Mutex> lock(helperThreadPayload->compiler->getHelperThreadMutex());
    helperThreadPayload->compiler->getHelperThreadConditionVariable().wait(
        lock, [helperThreadPayload]() { return helperThreadPayload->mainThreadSwitchedContext; });
  }

  do {
    // NOTE: All modules were in the same context, which is not thread safe. We need to 'clone' the module into separate
    // context here to ensure we can do the work simultaneously. We achieve this by outputting the module as bitcode and
    // read it back in another context.
    Module *originalModule = helperThreadPayload->modules[moduleIndex];

    // FIXME: There will be out of sync assertion when the main thread is doing something related to context (probably
    // in PipelineState::generate), and the helper thread is using bitcode writer, we need to find a decent solution for
    // such situation.
    SmallVector<char, 0> bcBuffer;
    BitcodeWriter bcWriter(bcBuffer);
    bcWriter.writeModule(*originalModule);
    bcWriter.writeSymtab();
    bcWriter.writeStrtab();

    SmallVectorMemoryBuffer bcMemBuf(std::move(bcBuffer), originalModule->getName());
    auto moduleOrErr = getLazyBitcodeModule(std::move(bcMemBuf), *context);
    std::unique_ptr<Module> module = nullptr;

    if (!moduleOrErr) {
      LLPC_ERRS("Failed to load bit code\n");
      helperThreadPayload->results[moduleIndex] = Result::ErrorInvalidShader;
      helperThreadProvider->TaskCompleted();
      continue;
    }

    module = std::move(*moduleOrErr);
    if (Error errCode = module->materializeAll()) {
      LLPC_ERRS("Failed to materialize module\n");
      module = nullptr;
      helperThreadPayload->results[moduleIndex] = Result::ErrorInvalidShader;
      helperThreadProvider->TaskCompleted();
      continue;
    }

    Module *newModule = module.release();
    auto result = helperThreadPayload->compiler->buildRayTracingPipelineElf(
        context, newModule, helperThreadPayload->pipelineElfs[moduleIndex], helperThreadPayload->shaderProps,
        helperThreadPayload->moduleCallsTraceRay, moduleIndex, pipeline, timerProfiler);

    helperThreadPayload->results[moduleIndex] = hasError ? Result::ErrorInvalidShader : result;

    helperThreadProvider->TaskCompleted();
  } while (helperThreadProvider->GetNextTask(&moduleIndex));

  context->setDiagnosticHandler(nullptr);
  helperThreadPayload->compiler->releaseContext(context);
}

// =====================================================================================================================
// Build raytracing pipeline internally
//
// @param context : Acquired context
// @param shaderInfo : Shader info of this pipeline
// @param unlinked : Do not provide some state to LGC, so offsets are generated as relocs
// @param [out] pipelineElfs : Output multiple Elf packages
// @param [out] shaderProps : Output multiple RayTracingShaderProperty
Result Compiler::buildRayTracingPipelineInternal(Context *context, ArrayRef<const PipelineShaderInfo *> shaderInfo,
                                                 bool unlinked, std::vector<ElfPackage> &pipelineElfs,
                                                 std::vector<RayTracingShaderProperty> &shaderProps,
                                                 IHelperThreadProvider *helperThreadProvider) {
  Result result = Result::Success;
  unsigned passIndex = 0;
  TimerProfiler timerProfiler(context->getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);
  RayTracingContext *rayTracingContext = static_cast<RayTracingContext *>(context->getPipelineContext());
  auto pipelineInfo = reinterpret_cast<const RayTracingPipelineBuildInfo *>(context->getPipelineBuildInfo());
  auto inputShaderCount = pipelineInfo->shaderCount;
  bool hasTraceRayModule = (shaderInfo.size() - inputShaderCount) > 1;

  bool hasError = false;
  context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

  // Set up middle-end objects.
  LgcContext *builderContext = context->getLgcContext();
  auto dialectGuard = llvm_dialects::withDialects(builderContext->getDialectContext());
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  rayTracingContext->setPipelineState(&*pipeline, /*hasher=*/nullptr, unlinked);
  context->setBuilder(builderContext->createBuilder(&*pipeline));

  // Create empty modules and set target machine in each.
  std::vector<Module *> modules(shaderInfo.size());
  for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
    const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
    auto moduleName = std::string("_") + getShaderStageAbbreviation(shaderInfoEntry->entryStage) + "_" +
                      std::to_string(getModuleIdByIndex(shaderIndex));
    moduleName[1] = std::tolower(moduleName[1]);
    modules[shaderIndex] = new Module(moduleName, *context);
    context->setModuleTargetMachine(modules[shaderIndex]);

    if (!shaderInfoEntry->pModuleData)
      continue;

    std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
    lowerPassMgr->setPassIndex(&passIndex);
    SpirvLower::registerPasses(*lowerPassMgr);

    // SPIR-V translation, then dump the result.
    lowerPassMgr->addPass(SpirvLowerTranslator(shaderInfoEntry->entryStage, shaderInfoEntry));

    // Run the passes.
    bool success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
    if (!success) {
      LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
      result = Result::ErrorInvalidShader;
    }
  }

  for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
    ShaderStage entryStage = shaderInfo[shaderIndex]->entryStage;

    std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
    lowerPassMgr->setPassIndex(&passIndex);
    SpirvLower::registerPasses(*lowerPassMgr);

    // Start timer for translate.
    timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, true);

    // OpTerminateRay/OpIgnoreIntersection of anyhit shader and OpReportIntersection of intersection shader could
    // terminate ray during inbetween of shader execution. So functions in these shaders need to be inlined.
    if (entryStage == ShaderStageRayTracingAnyHit || entryStage == ShaderStageRayTracingIntersect) {
      // Lower SPIR-V CFG merges before inlining
      lowerPassMgr->addPass(SpirvLowerCfgMerges());
      lowerPassMgr->addPass(AlwaysInlinerPass());
    }
    lowerPassMgr->addPass(SpirvLowerRayTracing(false));

    // Stop timer for translate.
    timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, false);

    // Run the passes.
    bool success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
    if (!success) {
      LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
      result = Result::ErrorInvalidShader;
    }
  }

  std::vector<Module *> newModules;
  // Record which module calls TraceRay(), except the first one (For indirect mode, it is the entry function which will
  // never call TraceRay(). For inlined mode, we don't need to care).
  std::vector<bool> moduleCallsTraceRay;
  auto indirectStageMask = rayTracingContext->getIndirectStageMask();
  unsigned intersectMask = ShaderStageRayTracingIntersectBit | ShaderStageRayTracingAnyHitBit |
                           ShaderStageRayTracingClosestHitBit | ShaderStageRayTracingMissBit;

  if (indirectStageMask == 0) {
    // Create an empty module then link each shader module into it.
    Module *rayTracingModule = new Module("llpcLinkedRayTrace", *context);
    context->setModuleTargetMachine(rayTracingModule);
    Linker linker(*rayTracingModule);

    for (unsigned shaderIndex = 0; shaderIndex < modules.size(); ++shaderIndex) {
      if (modules[shaderIndex]) {
        // NOTE: We use unique_ptr here. The shader module will be destroyed after it is
        // linked into pipeline module.
        if (linker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex]))) {
          result = Result::ErrorInvalidShader;
          break;
        }
      }
    }
    newModules.push_back(rayTracingModule);
    if (result != Result::Success) {
      delete rayTracingModule;
      rayTracingModule = nullptr;
    } else
      setShaderStageToModule(rayTracingModule, ShaderStageCompute);
  } else if (!hasTraceRayModule) {
    assert(shaderInfo.size() > inputShaderCount);
    modules[inputShaderCount]->setModuleIdentifier("main");
    newModules.push_back(modules[inputShaderCount]);
    unsigned shaderIndex = 0;
    for (shaderIndex = 0; shaderIndex < inputShaderCount; ++shaderIndex) {
      newModules.push_back(modules[shaderIndex]);
      moduleCallsTraceRay.push_back(false);
    }
  } else {
    // Begin indirect modules processing
    // First stage : link rayquery functions for ray tracing modules
    unsigned shaderIndex = 0;
    // Last module in the modules is trace ray module
    Module *shaderLibraryModule = modules[shaderInfo.size() - 1];
    for (; shaderIndex < inputShaderCount; ++shaderIndex) {
      const auto *shaderInfoEntry = shaderInfo[shaderIndex];
      const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);
      if (moduleData->usage.enableRayQuery) {
        Linker linker(*modules[shaderIndex]);
        if (linker.linkInModule(CloneModule(*shaderLibraryModule))) {
          result = Result::ErrorInvalidShader;
        }
      }
    }

    if (indirectStageMask & intersectMask) {
      // currently only support ClosestHit, Miss, AnyHit, Intersect shader inline/indirect
      Module *traceRayModule = new Module(modules[shaderInfo.size() - 1]->getName(), *context);
      Module *raygenModule = new Module("main", *context);
      context->setModuleTargetMachine(traceRayModule);
      context->setModuleTargetMachine(raygenModule);
      Linker traceRaylinker(*traceRayModule);
      Linker raygenlinker(*raygenModule);

      newModules.push_back(raygenModule);

      for (shaderIndex = 0; shaderIndex < inputShaderCount; ++shaderIndex) {
        unsigned entryStageMask = shaderStageToMask(shaderInfo[shaderIndex]->entryStage);
        const ShaderModuleData *moduleData =
            reinterpret_cast<const ShaderModuleData *>(shaderInfo[shaderIndex]->pModuleData);
        if (indirectStageMask & entryStageMask) {
          newModules.push_back(modules[shaderIndex]);
          moduleCallsTraceRay.push_back(moduleData->usage.hasTraceRay);
        } else if (intersectMask & entryStageMask) {
          // link library module
          if (traceRaylinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex]))) {
            result = Result::ErrorInvalidShader;
            break;
          }
        } else if (entryStageMask == ShaderStageRayTracingRayGenBit) {
          if (raygenlinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex]))) {
            result = Result::ErrorInvalidShader;
            break;
          }
        }
      }
      if (traceRaylinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex + 1])))
        result = Result::ErrorInvalidShader;

      if (raygenlinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex])))
        result = Result::ErrorInvalidShader;

      if (traceRayModule->empty()) {
        delete traceRayModule;
        traceRayModule = modules[shaderIndex + 1];
      }
      newModules.push_back(traceRayModule);
      moduleCallsTraceRay.push_back(false);

      if (raygenModule->empty()) {
        delete raygenModule;
        newModules[0] = modules[shaderIndex];
      }
    } else {
      Module *raygenModule = new Module("main", *context);
      context->setModuleTargetMachine(raygenModule);
      Linker raygenlinker(*raygenModule);

      newModules.push_back(raygenModule);

      for (shaderIndex = 0; shaderIndex < inputShaderCount; ++shaderIndex) {
        unsigned entryStageMask = shaderStageToMask(shaderInfo[shaderIndex]->entryStage);
        const ShaderModuleData *moduleData =
            reinterpret_cast<const ShaderModuleData *>(shaderInfo[shaderIndex]->pModuleData);
        if (indirectStageMask & entryStageMask) {
          newModules.push_back(modules[shaderIndex]);
          moduleCallsTraceRay.push_back(moduleData->usage.hasTraceRay);
        } else {
          if (raygenlinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex]))) {
            result = Result::ErrorInvalidShader;
            break;
          }
        }
      }

      // Link entry modules
      if (raygenlinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex++])))
        result = Result::ErrorInvalidShader;

      // Link trace ray modules
      if (raygenlinker.linkInModule(std::unique_ptr<Module>(modules[shaderIndex])))
        result = Result::ErrorInvalidShader;

      if (raygenModule->empty()) {
        delete raygenModule;
        newModules[0] = modules[shaderIndex];
      }
    }
  }

  assert(moduleCallsTraceRay.size() == (newModules.size() - 1));

  rayTracingContext->setLinked(true);
  pipelineElfs.resize(newModules.size());
  shaderProps.resize(newModules.size() - 1);

  unsigned moduleIndex = 0;
  if (helperThreadProvider) {
    std::vector<Result> results(newModules.size(), Result::Success);
    HelperThreadBuildRayTracingPipelineElfPayload helperThreadPayload = {
        newModules, pipelineElfs, shaderProps, moduleCallsTraceRay, results, rayTracingContext, this, false, false};
    helperThreadProvider->SetTasks(&helperThreadBuildRayTracingPipelineElf, newModules.size(),
                                   static_cast<void *>(&helperThreadPayload));
    while (!helperThreadPayload.helperThreadJoined && helperThreadProvider->GetNextTask(&moduleIndex)) {
      // NOTE: When a helper thread joins, it will move modules from the original context into a new one. However,
      // main thread may be processing on the original context at the same time, results in out of sync situation.
      // Here we keep main thread working on the original context until helper thread joins, to reduce the cost of
      // initializing new context and copying modules. Once helper thread has joined, main thread must switch to a new
      // context.
      results[moduleIndex] =
          buildRayTracingPipelineElf(context, newModules[moduleIndex], pipelineElfs[moduleIndex], shaderProps,
                                     moduleCallsTraceRay, moduleIndex, pipeline, timerProfiler);
      helperThreadProvider->TaskCompleted();
    }

    if (helperThreadPayload.helperThreadJoined) {
      // Tasks may not finished but helper thread joined, need to switch to new context and notify helper thread to
      // proceed.
      helperThreadPayload.mainThreadSwitchedContext = true;
      m_helperThreadConditionVariable.notify_all();
      helperThreadBuildRayTracingPipelineElf(helperThreadProvider, &helperThreadPayload);
    }
    helperThreadProvider->WaitForTasks();

    for (auto res : results) {
      if (res != Result::Success)
        result = Result::ErrorInvalidShader;
      break;
    }

  } else {
    for (auto itermodule = newModules.begin(); itermodule != newModules.end() && result == Result::Success;
         ++itermodule, ++moduleIndex) {
      result = buildRayTracingPipelineElf(context, *itermodule, pipelineElfs[moduleIndex], shaderProps,
                                          moduleCallsTraceRay, moduleIndex, pipeline, timerProfiler);
    }
  }
  context->setDiagnosticHandlerCallBack(nullptr);

  return result;
}

// =====================================================================================================================
// Add raytracing pipeline indirect pipeline metadata
// The metadata is needed for RGP to correctly show different subtype of shaders.
//
// @param [in/out] pipelineElfs : The pipeline ELF
void Compiler::addRayTracingIndirectPipelineMetadata(ElfPackage *pipelineElf) {
  // Read the ELF package
  ElfWriter<Elf64> writer(m_gfxIp);
  Result result = writer.ReadFromBuffer(pipelineElf->data(), pipelineElf->size());
  if (result != Result::Success) {
    // Not an ELF file, this is the case when -emit-llvm or -emit-lgc is passed
    return;
  }

  ElfNote metaNote = writer.getNote(Abi::MetadataNoteType);

  msgpack::Document document;

  auto success =
      document.readFromBlob(StringRef(reinterpret_cast<const char *>(metaNote.data), metaNote.hdr.descSize), false);
  assert(success);
  (void(success)); // unused

  // Get the shader_functions section
  auto pipeline = document.getRoot().getMap(true)[PalAbi::CodeObjectMetadataKey::Pipelines].getArray(true)[0];
  auto shaderFunctionSection = pipeline.getMap(true)[PalAbi::PipelineMetadataKey::ShaderFunctions].getMap(true);

  // Get the shader function
  auto shaderFunctionName = shaderFunctionSection.begin()->first.getString();
  auto shaderFunction = shaderFunctionSection.begin()->second.getMap(true);

  // Determine the shader subtype by name
  auto subtype = "Unknown";
  if (shaderFunctionName.contains("rgen"))
    subtype = "RayGeneration";
  else if (shaderFunctionName.contains("miss"))
    subtype = "Miss";
  else if (shaderFunctionName.contains("ahit"))
    subtype = "AnyHit";
  else if (shaderFunctionName.contains("chit"))
    subtype = "ClosestHit";
  else if (shaderFunctionName.contains("sect"))
    subtype = "Intersection";
  else if (shaderFunctionName.contains("call"))
    subtype = "Callable";
  else if (shaderFunctionName.contains("cs"))
    subtype = "Traversal";

  shaderFunction[".shader_subtype"] = subtype;

  // Apply the .internal_pipeline_hash to .api_shader_hash in .shader_functions section
  // NOTE: this is needed for RGP to recognize different shader subtype
  auto pipelineHash = pipeline.getMap(true)[PalAbi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
  shaderFunction[PalAbi::ShaderMetadataKey::ApiShaderHash].getArray(true)[0] = pipelineHash[0];
  shaderFunction[PalAbi::ShaderMetadataKey::ApiShaderHash].getArray(true)[1] = pipelineHash[1];

  // Write to the pipeline ELF
  ElfNote newMetaNote = metaNote;
  std::string destBlob;
  document.writeToBlob(destBlob);
  auto newData = new uint8_t[destBlob.size()]; // 4 is for additional alignment space
  memcpy(newData, destBlob.data(), destBlob.size());
  newMetaNote.hdr.descSize = destBlob.size();
  newMetaNote.data = newData;
  writer.setNote(&newMetaNote);
  writer.writeToBuffer(pipelineElf);
}
#endif

// =====================================================================================================================
// Builds hash code from compilation-options
//
// @param optionCount : Count of compilation-option strings
// @param options : An array of compilation-option strings
MetroHash::Hash Compiler::generateHashForCompileOptions(unsigned optionCount, const char *const *options) {
  // Options which needn't affect compilation results
  static StringRef IgnoredOptions[] = {cl::PipelineDumpDir.ArgStr,
                                       cl::EnablePipelineDump.ArgStr,
                                       cl::ShaderCacheFileDir.ArgStr,
                                       cl::ShaderCacheMode.ArgStr,
                                       cl::EnableOuts.ArgStr,
                                       cl::EnableErrs.ArgStr,
                                       cl::LogFileDbgs.ArgStr,
                                       cl::LogFileOuts.ArgStr,
                                       cl::ExecutableName.ArgStr,
                                       "unlinked",
                                       "o"};

  std::set<StringRef> effectingOptions;
  // Build effecting options
  for (unsigned i = 1; i < optionCount; ++i) {
    if (options[i][0] != '-') {
      // Ignore input file names.
      continue;
    }

    StringRef option = options[i] + 1; // Skip '-' in options
    bool ignore = false;
    for (unsigned j = 0; j < sizeof(IgnoredOptions) / sizeof(IgnoredOptions[0]); ++j) {
      if (option.startswith(IgnoredOptions[j])) {
        ignore = true;
        break;
      }
    }

    if (!ignore)
      effectingOptions.insert(option);
  }

  MetroHash64 hasher;

  // Build hash code from effecting options
  for (auto option : effectingOptions)
    hasher.Update(reinterpret_cast<const uint8_t *>(option.data()), option.size());

  MetroHash::Hash hash = {};
  hasher.Finalize(hash.bytes);

  return hash;
}

// =====================================================================================================================
// Checks whether fields in pipeline shader info are valid.
//
// @param shaderInfo : Pipeline shader info
Result Compiler::validatePipelineShaderInfo(const PipelineShaderInfo *shaderInfo) const {
  Result result = Result::Success;
  ShaderStage shaderStage = shaderInfo ? shaderInfo->entryStage : ShaderStageInvalid;

  const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
  if (moduleData) {
    if (moduleData->binType == BinaryType::Spirv) {
      auto spirvBin = &moduleData->binCode;
      if (shaderInfo->pEntryTarget) {
        unsigned stageMask = ShaderModuleHelper::getStageMaskFromSpirvBinary(spirvBin, shaderInfo->pEntryTarget);

        if ((stageMask & shaderStageToMask(shaderStage)) == 0) {
          LLPC_ERRS("Fail to find entry-point " << shaderInfo->pEntryTarget << " for "
                                                << getShaderStageName(shaderStage) << " shader\n");
          result = Result::ErrorInvalidShader;
        }
      } else {
        LLPC_ERRS("Missing entry-point name for " << getShaderStageName(shaderStage) << " shader\n");
        result = Result::ErrorInvalidShader;
      }
    } else if (moduleData->binType == BinaryType::LlvmBc || moduleData->binType == BinaryType::MultiLlvmBc) {
      // Do nothing if input is LLVM IR
    } else {
      LLPC_ERRS("Invalid shader binary type for " << getShaderStageName(shaderStage) << " shader\n");
      result = Result::ErrorInvalidShader;
    }
  }

  return result;
}

#if LLPC_ENABLE_SHADER_CACHE
// =====================================================================================================================
// Creates shader cache object with the requested properties.
// @param : Shader cache create info.
// @param [out] : Shader cache object
// @returns : Result::Success if creation succeeds, error status otherwise.
Result Compiler::CreateShaderCache(const ShaderCacheCreateInfo *pCreateInfo, IShaderCache **ppShaderCache) {
  Result result = Result::Success;

  ShaderCacheAuxCreateInfo auxCreateInfo = {};
  auxCreateInfo.shaderCacheMode = ShaderCacheMode::ShaderCacheEnableRuntime;
  auxCreateInfo.gfxIp = m_gfxIp;
  auxCreateInfo.hash = m_optionHash;

  ShaderCache *shaderCache = new ShaderCache();

  if (shaderCache) {
    result = shaderCache->init(pCreateInfo, &auxCreateInfo);
    if (result != Result::Success) {
      shaderCache->Destroy();
      delete shaderCache;
      shaderCache = nullptr;
    }
  } else {
    result = Result::ErrorOutOfMemory;
  }

  *ppShaderCache = shaderCache;

  if ((result == Result::Success) &&
      ((cl::ShaderCacheMode == ShaderCacheEnableRuntime) || (cl::ShaderCacheMode == ShaderCacheEnableOnDisk)) &&
      (pCreateInfo->initialDataSize > 0)) {
    result = m_shaderCache->Merge(1, const_cast<const IShaderCache **>(ppShaderCache));
  }

  return result;
}
#endif

// =====================================================================================================================
// Acquires a free context from context pool.
Context *Compiler::acquireContext() const {
  Context *freeContext = nullptr;

  std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);

  // Try to find a free context from pool first
  for (auto &context : *m_contextPool) {
    GfxIpVersion gfxIpVersion = context->getGfxIpVersion();

    if (!context->isInUse() && gfxIpVersion == m_gfxIp) {
      // Free up context if it is being used too many times to avoid consuming too much memory.
      int contextReuseLimit = cl::ContextReuseLimit.getValue();
      if (contextReuseLimit > 0 && context->getUseCount() > contextReuseLimit) {
        delete context;
        context = new Context(m_gfxIp);
      }
      freeContext = context;
      break;
    }
  }

  if (!freeContext) {
    // Create a new one if we fail to find an available one
    freeContext = new Context(m_gfxIp);
    m_contextPool->push_back(freeContext);
  }

  assert(freeContext);
  freeContext->setInUse(true);

  return freeContext;
}

// =====================================================================================================================
// Run pass manager's passes on a module, catching any LLVM fatal error and returning a success indication
//
// @param passMgr : Pass manager
// @param [in/out] module : Module
bool Compiler::runPasses(lgc::PassManager *passMgr, Module *module) const {
  bool success = false;
#if LLPC_ENABLE_EXCEPTION
  try
#endif
  {
    passMgr->run(*module);
    success = true;
  }
#if LLPC_ENABLE_EXCEPTION
  catch (const char *) {
    success = false;
  }
#endif
  return success;
}

// =====================================================================================================================
// Releases LLPC context.
//
// @param context : LLPC context
void Compiler::releaseContext(Context *context) const {
  std::lock_guard<sys::Mutex> lock(m_contextPoolMutex);
  context->reset();
  context->setInUse(false);
}

// =====================================================================================================================
// Builds hash code from input context for per shader stage cache
//
// @param context : Acquired context
// @param stageMask : Shader stage mask (NOTE: This is a LGC shader stage mask passed by middle-end)
// @param stageHashes : Per-stage hash of in/out usage
// @param [out] fragmentHash : Hash code of fragment shader
// @param [out] nonFragmentHash : Hash code of all non-fragment shader
void Compiler::buildShaderCacheHash(Context *context, unsigned stageMask, ArrayRef<ArrayRef<uint8_t>> stageHashes,
                                    MetroHash::Hash *fragmentHash, MetroHash::Hash *nonFragmentHash) {
  MetroHash64 fragmentHasher;
  MetroHash64 nonFragmentHasher;
  auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
  auto pipelineOptions = context->getPipelineContext()->getPipelineOptions();

  // Build hash per shader stage
  for (ShaderStage stage : gfxShaderStages()) {
    if ((stageMask & getLgcShaderStageMask(stage)) == 0)
      continue;

    auto shaderInfo = context->getPipelineShaderInfo(stage);
    MetroHash64 hasher;

    // Update common shader info
    PipelineDumper::updateHashForPipelineShaderInfo(stage, shaderInfo, true, &hasher, false);
    hasher.Update(pipelineInfo->iaState.deviceIndex);

    PipelineDumper::updateHashForResourceMappingInfo(context->getResourceMapping(), context->getPipelineLayoutApiHash(),
                                                     &hasher, stage);

    // Update input/output usage (provided by middle-end caller of this callback).
    hasher.Update(stageHashes[getLgcShaderStage(stage)].data(), stageHashes[getLgcShaderStage(stage)].size());

    // Update vertex input state
    if (stage == ShaderStageVertex)
      PipelineDumper::updateHashForVertexInputState(pipelineInfo->pVertexInput, pipelineInfo->dynamicVertexStride,
                                                    &hasher);

    MetroHash::Hash hash = {};
    hasher.Finalize(hash.bytes);

    // Add per stage hash code to fragmentHasher or nonFragmentHasher per shader stage
    auto shaderHashCode = MetroHash::compact64(&hash);
    if (stage == ShaderStageFragment)
      fragmentHasher.Update(shaderHashCode);
    else
      nonFragmentHasher.Update(shaderHashCode);
  }

  // Add additional pipeline state to final hasher
  if (stageMask & getLgcShaderStageMask(ShaderStageFragment)) {
    PipelineDumper::updateHashForPipelineOptions(pipelineOptions, &fragmentHasher, true, false, UnlinkedStageFragment);
    PipelineDumper::updateHashForFragmentState(pipelineInfo, &fragmentHasher, false);
    fragmentHasher.Finalize(fragmentHash->bytes);
  }

  if (stageMask & ~getLgcShaderStageMask(ShaderStageFragment)) {
    PipelineDumper::updateHashForPipelineOptions(pipelineOptions, &nonFragmentHasher, true, false,
                                                 UnlinkedStageVertexProcess);
    PipelineDumper::updateHashForNonFragmentState(pipelineInfo, true, &nonFragmentHasher, false);
    nonFragmentHasher.Finalize(nonFragmentHash->bytes);
  }
}

// =====================================================================================================================
// Link relocatable shader elf file into a pipeline elf file and apply relocations.  Returns true if successful.
//
// @param shaderElfs : An array of pipeline elf packages, indexed by stage, containing relocatable elf.
//                     TODO: This has an implicit length of ShaderStageNativeStageCount. Use ArrayRef instead.
// @param [out] pipelineElf : Elf package containing the pipeline elf
// @param context : Acquired context
bool Compiler::linkRelocatableShaderElf(ElfPackage *shaderElfs, ElfPackage *pipelineElf, Context *context) {
  assert(!context->getPipelineContext()->isUnlinked() && "Not supposed to link this pipeline.");

  // Set up middle-end objects, including setting up pipeline state.
  LgcContext *builderContext = context->getLgcContext();
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  context->getPipelineContext()->setPipelineState(&*pipeline, /*hasher=*/nullptr, /*unlinked=*/false);

  // Create linker, passing ELFs to it.
  SmallVector<MemoryBufferRef, 3> elfs;
  for (auto stage : enumRange<UnlinkedShaderStage>()) {
    if (!shaderElfs[stage].empty())
      elfs.push_back(MemoryBufferRef(shaderElfs[stage].str(), getUnlinkedShaderStageName(stage)));
  }
  std::unique_ptr<ElfLinker> elfLinker(pipeline->createElfLinker(elfs));

  if (elfLinker->fragmentShaderUsesMappedBuiltInInputs()) {
    LLPC_OUTS("Failed to link relocatable shaders because FS uses builtin inputs.");
    return false;
  }

  setGlueBinaryBlobsInLinker(elfLinker.get(), context, this);
  // Do the link.
  raw_svector_ostream outStream(*pipelineElf);
  if (!elfLinker->link(outStream)) {
    // Link failed in a recoverable way.
    // TODO: Action this failure by doing a full pipeline compile.
    report_fatal_error("Link failed; need full pipeline compile instead: " + pipeline->getLastError());
  }
  return true;
}

// =====================================================================================================================
// Convert front-end LLPC shader stage to middle-end LGC shader type
//
// @param stage : Front-end LLPC shader stage
lgc::ShaderStage getLgcShaderStage(Llpc::ShaderStage stage) {
  switch (stage) {
  case ShaderStageTask:
    return lgc::ShaderStageTask;
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
  case ShaderStageMesh:
    return lgc::ShaderStageMesh;
  case ShaderStageFragment:
    return lgc::ShaderStageFragment;
  case ShaderStageCopyShader:
    return lgc::ShaderStageCopyShader;
#if VKI_RAY_TRACING
  case ShaderStageRayTracingRayGen:
  case ShaderStageRayTracingIntersect:
  case ShaderStageRayTracingAnyHit:
  case ShaderStageRayTracingClosestHit:
  case ShaderStageRayTracingMiss:
  case ShaderStageRayTracingCallable:
    return lgc::ShaderStageCompute;
#endif
  default:
    llvm_unreachable("");
    return lgc::ShaderStageInvalid;
  }
}

// =====================================================================================================================
// Convert front-end LLPC shader stage to middle-end LGC shader stage mask
//
// @param stage : Front-end LLPC shader stage
unsigned getLgcShaderStageMask(ShaderStage stage) {
  return (1 << getLgcShaderStage(stage));
}

} // namespace Llpc
