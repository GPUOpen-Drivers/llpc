/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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
#include "LowerAdvancedBlend.h"
#include "PrepareContinuations.h"
#include "SPIRVEntry.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVInternal.h"
#include "llpcCacheAccessor.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcElfWriter.h"
#include "llpcError.h"
#include "llpcFile.h"
#include "llpcGraphicsContext.h"
#include "llpcRayTracingContext.h"
#include "llpcShaderModuleHelper.h"
#include "llpcSpirvLower.h"
#include "llpcSpirvLowerCfgMerges.h"
#include "llpcSpirvLowerRayTracing.h"
#include "llpcSpirvLowerTranslator.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcThreading.h"
#include "llpcTimerProfiler.h"
#include "llpcUtil.h"
#include "spirvExt.h"
#include "vkgcDefs.h"
#include "vkgcElfReader.h"
#include "vkgcPipelineDumper.h"
#include "lgc/Builder.h"
#include "lgc/ElfLinker.h"
#include "lgc/EnumIterator.h"
#include "lgc/GpurtDialect.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "lgc/PassManager.h"
#include "lgc/RuntimeContext.h"
#include "llvm-dialects/Dialect/Dialect.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Support/ErrorHandling.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/GpurtContext.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <set>
#include <unordered_set>

#ifdef LLPC_ENABLE_SPIRV_OPT
#include "spvgen.h"
#endif

namespace RtName {
extern const char *TraceRayKHR;
} // namespace RtName

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

// -cache-full-pipelines: Add full pipelines to the caches that are provided.
opt<bool> CacheFullPipelines("cache-full-pipelines", desc("Add full pipelines to the caches that are provided."),
                             init(true));

// -enable-per-stage-cache: Enable shader cache per shader stage
opt<bool> EnablePerStageCache("enable-per-stage-cache", cl::desc("Enable shader cache per shader stage"), init(true));

// -context-reuse-limit: The maximum number of times a compiler context can be reused.
opt<int> ContextReuseLimit("context-reuse-limit",
                           cl::desc("The maximum number of times a compiler context can be reused"), init(100));

// -fatal-llvm-errors: Make all LLVM errors fatal
opt<bool> FatalLlvmErrors("fatal-llvm-errors", cl::desc("Make all LLVM errors fatal"), init(false));

// -enable-part-pipeline: Use part pipeline compilation scheme (experimental)
opt<bool> EnablePartPipeline("enable-part-pipeline", cl::desc("Enable part pipeline compilation scheme"), init(false));

// -add-rt-helpers: Spawn additional helper threads to run RT pipeline compilations
opt<int> AddRtHelpers("add-rt-helpers", cl::desc("Add this number of helper threads for each RT pipeline compile"),
                      init(0));

#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 66
// -shader-cache-file-dir: root directory to store shader cache
opt<std::string> ShaderCacheFileDir("shader-cache-file-dir", desc("Root directory to store shader cache"),
                                    value_desc("dir"), init("."));

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

// -executable-name: executable file name
opt<std::string> ExecutableName("executable-name", desc("Executable file name"), value_desc("filename"),
                                init("amdllpc"));
#endif

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

// Represents the payload used by helper thread to build ray tracing Elf
struct HelperThreadBuildRayTracingPipelineElfPayload {
  ArrayRef<Module *> modules;                         // Modules to generate ELF packages
  std::vector<ElfPackage> &pipelineElfs;              // Output ELF packages
  std::vector<RayTracingShaderProperty> &shaderProps; // Output RayTracingShaderProperty
  std::vector<bool> &moduleCallsTraceRay;             // Whether each module calls OpTraceRay
  std::vector<Result> &results;                       // Build result of each module
  RayTracingContext *rayTracingContext;               // The ray tracing context across the pipeline
  Compiler *compiler;                                 // The compiler instance
  std::atomic<bool> helperThreadJoined;               // Whether helper thread has joined
  std::atomic<bool> mainThreadSwitchedContext;        // Whether main thread has finished switching context
};

sys::Mutex Compiler::m_helperThreadMutex;
std::condition_variable_any Compiler::m_helperThreadConditionVariable;

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
static CacheAccessor checkCacheForGlueShader(StringRef glueShaderIdentifier, Compiler *compiler) {
  Hash glueShaderCacheHash =
      PipelineDumper::generateHashForGlueShader({glueShaderIdentifier.size(), glueShaderIdentifier.data()});
  return CacheAccessor(glueShaderCacheHash, compiler->getInternalCaches());
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
    CacheAccessor cacheAccessor = checkCacheForGlueShader(glueShaderIdentifiers[i], compiler);

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
  assert(gfxIp.major >= 10); // Only accept GFx10+
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
    : m_gfxIp(gfxIp), m_cache(cache), m_relocatablePipelineCompilations(0) {
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
  }

  {
    // s_compilerMutex is managed by ManagedStatic, it can't be accessed after llvm_shutdown
    std::lock_guard<sys::Mutex> lock(*SCompilerMutex);
    --m_instanceCount;
    if (m_instanceCount == 0)
      shutdown = true;
  }

  if (shutdown) {
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
// Merge location and binding value, and replace the binding decoration in spirv binary.
//
// @param codeBuffer : Spirv binary
// @param imageSymbolInfo : Image symbol infos
static void mergeSpirvLocationAndBinding(MutableArrayRef<unsigned> codeBuffer,
                                         std::vector<ResourceNodeData> &imageSymbolInfo) {
  constexpr unsigned wordSize = sizeof(unsigned);

  unsigned *code = codeBuffer.data();
  unsigned *end = code + codeBuffer.size();
  unsigned *codePos = code + sizeof(SpirvHeader) / wordSize;

  while (codePos < end) {
    unsigned opCode = (codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);

    switch (opCode) {
    case OpDecorate: {
      auto decoration = static_cast<Decoration>(codePos[2]);

      if (decoration == DecorationBinding) {
        uint32_t varId = codePos[1];
        uint32_t binding = codePos[3];
        uint32_t location = 0;
        for (auto it = imageSymbolInfo.begin(); it != imageSymbolInfo.end(); ++it) {
          if (it->spvId == varId && it->binding == binding) {
            location = it->location;
            it->mergedLocationBinding = true;
          }
        }
        uint32_t locationBinding = location << 16 | binding;
        codePos[3] = locationBinding;
      }
    } break;
    default:
      break;
    }

    codePos += wordCount;
  }
}

// =====================================================================================================================
// Recalculate resource binding for separate shader object
//
// @param codeBuffer : Spirv binary
// @param resourceBindingOffset : resource binding offset
// @param symbolInfo : resource symbol infos
static void recalcResourceBinding(MutableArrayRef<unsigned> codeBuffer, unsigned resourceBindingOffset,
                                  std::vector<ResourceNodeData> &uniformBufferInfo,
                                  std::vector<ResourceNodeData> &storageBufferInfo,
                                  std::vector<ResourceNodeData> &textureSymbolInfo,
                                  std::vector<ResourceNodeData> &imageSymbolInfo,
                                  std::vector<ResourceNodeData> &atomicCounterSymbolInfo) {
  constexpr unsigned wordSize = sizeof(unsigned);

  unsigned *code = codeBuffer.data();
  unsigned *end = code + codeBuffer.size();
  unsigned *codePos = code + sizeof(SpirvHeader) / wordSize;

  auto updateResourceBinding = [resourceBindingOffset](uint32_t varId, uint32_t binding,
                                                       std::vector<ResourceNodeData> &symbolInfo) {
    for (auto it = symbolInfo.begin(); it != symbolInfo.end(); ++it) {
      if (it->spvId == varId && it->binding == binding) {
        it->binding = resourceBindingOffset << 16 | binding;
      }
    }
  };

  while (codePos < end) {
    unsigned opCode = (codePos[0] & OpCodeMask);
    unsigned wordCount = (codePos[0] >> WordCountShift);

    switch (opCode) {
    case OpDecorate: {
      auto decoration = static_cast<Decoration>(codePos[2]);

      if (decoration == DecorationBinding) {
        uint32_t varId = codePos[1];
        uint32_t binding = codePos[3];

        updateResourceBinding(varId, binding, uniformBufferInfo);
        updateResourceBinding(varId, binding, storageBufferInfo);
        updateResourceBinding(varId, binding, textureSymbolInfo);
        updateResourceBinding(varId, binding, imageSymbolInfo);
        updateResourceBinding(varId, binding, atomicCounterSymbolInfo);
        codePos[3] = resourceBindingOffset << 16 | binding;
      }
    } break;
    default:
      break;
    }

    codePos += wordCount;
  }
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

  auto codeSizeOrErr = ShaderModuleHelper::getCodeSize(shaderInfo);
  if (Error err = codeSizeOrErr.takeError())
    return errorToResult(std::move(err));

  const unsigned codeSize = *codeSizeOrErr;
  size_t allocSize = sizeof(ShaderModuleData) + codeSize;

  ShaderModuleData moduleData = {};
  std::vector<unsigned> codeBufferVector(codeSize / sizeof(unsigned));
  MutableArrayRef<unsigned> codeBuffer(codeBufferVector);
  memcpy(moduleData.hash, &hash, sizeof(hash));
  Result result = ShaderModuleHelper::getModuleData(shaderInfo, codeBuffer, moduleData);

  ResourcesNodes resourceNodes = {};
  std::vector<ResourceNodeData> inputSymbolInfo;
  std::vector<ResourceNodeData> outputSymbolInfo;
  std::vector<ResourceNodeData> uniformBufferInfo;
  std::vector<ResourceNodeData> storageBufferInfo;
  std::vector<ResourceNodeData> textureSymbolInfo;
  std::vector<ResourceNodeData> imageSymbolInfo;
  std::vector<ResourceNodeData> atomicCounterSymbolInfo;
  std::vector<ResourceNodeData> defaultUniformSymbolInfo;
  if (shaderInfo->options.pipelineOptions.getGlState().buildResourcesDataForShaderModule &&
      moduleData.binType == BinaryType::Spirv) {
    buildShaderModuleResourceUsage(shaderInfo, resourceNodes, inputSymbolInfo, outputSymbolInfo, uniformBufferInfo,
                                   storageBufferInfo, textureSymbolInfo, imageSymbolInfo, atomicCounterSymbolInfo,
                                   defaultUniformSymbolInfo, moduleData.usage);

    allocSize += sizeof(ResourcesNodes);
    allocSize += inputSymbolInfo.size() * sizeof(ResourceNodeData);
    allocSize += outputSymbolInfo.size() * sizeof(ResourceNodeData);
    allocSize += uniformBufferInfo.size() * sizeof(ResourceNodeData);
    allocSize += storageBufferInfo.size() * sizeof(ResourceNodeData);
    allocSize += textureSymbolInfo.size() * sizeof(ResourceNodeData);
    allocSize += imageSymbolInfo.size() * sizeof(ResourceNodeData);
    allocSize += atomicCounterSymbolInfo.size() * sizeof(ResourceNodeData);
    allocSize += defaultUniformSymbolInfo.size() * sizeof(ResourceNodeData);

    // Solve binding conflictions for separate shader object
    if (shaderInfo->options.resourceBindingOffset > 0) {
      recalcResourceBinding(codeBuffer, shaderInfo->options.resourceBindingOffset, uniformBufferInfo, storageBufferInfo,
                            textureSymbolInfo, imageSymbolInfo, atomicCounterSymbolInfo);
    } else if (imageSymbolInfo.size() && shaderInfo->options.mergeLocationAndBinding)
      // Merge location and binding if image binding doesn't exist, the issue only exist on spirv binary cases,
      // separate shader object doesn't support spirv binary, so it doesn't have such issue
      mergeSpirvLocationAndBinding(codeBuffer, imageSymbolInfo);
  }

  uint8_t *allocBuf =
      static_cast<uint8_t *>(shaderInfo->pfnOutputAlloc(shaderInfo->pInstance, shaderInfo->pUserData, allocSize));
  if (!allocBuf)
    return Result::ErrorOutOfMemory;

  uint8_t *bufferWritePtr = allocBuf;
  ShaderModuleData *pShaderModuleData = nullptr;
  ResourcesNodes *pResourcesNodes = nullptr;

  memcpy(bufferWritePtr, &moduleData, sizeof(moduleData));
  pShaderModuleData = reinterpret_cast<ShaderModuleData *>(bufferWritePtr);
  bufferWritePtr += sizeof(ShaderModuleData);

  memcpy(bufferWritePtr, codeBuffer.data(), codeBuffer.size() * sizeof(unsigned));
  pShaderModuleData->binCode.pCode = bufferWritePtr;
  bufferWritePtr += codeBuffer.size() * sizeof(unsigned);

  if (shaderInfo->options.pipelineOptions.getGlState().buildResourcesDataForShaderModule &&
      moduleData.binType == BinaryType::Spirv) {
    memcpy(bufferWritePtr, &resourceNodes, sizeof(ResourcesNodes));
    pResourcesNodes = reinterpret_cast<ResourcesNodes *>(bufferWritePtr);
    pShaderModuleData->usage.pResources = pResourcesNodes;
    bufferWritePtr += sizeof(ResourcesNodes);

    memcpy(bufferWritePtr, inputSymbolInfo.data(), inputSymbolInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pInputInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += inputSymbolInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, outputSymbolInfo.data(), outputSymbolInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pOutputInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += outputSymbolInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, uniformBufferInfo.data(), uniformBufferInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pUniformBufferInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += uniformBufferInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, storageBufferInfo.data(), storageBufferInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pShaderStorageInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += storageBufferInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, textureSymbolInfo.data(), textureSymbolInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pTexturesInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += textureSymbolInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, imageSymbolInfo.data(), imageSymbolInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pImagesInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += imageSymbolInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, atomicCounterSymbolInfo.data(), atomicCounterSymbolInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pAtomicCounterInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += atomicCounterSymbolInfo.size() * sizeof(ResourceNodeData);

    memcpy(bufferWritePtr, defaultUniformSymbolInfo.data(), defaultUniformSymbolInfo.size() * sizeof(ResourceNodeData));
    pResourcesNodes->pDefaultUniformInfo = reinterpret_cast<ResourceNodeData *>(bufferWritePtr);
    bufferWritePtr += defaultUniformSymbolInfo.size() * sizeof(ResourceNodeData);
  }

  shaderOut->pModuleData = pShaderModuleData;

  if (moduleData.binType == BinaryType::Spirv && cl::EnablePipelineDump) {
    // Dump the original input binary, since the offline tool will re-run BuildShaderModule
    PipelineDumper::DumpSpirvBinary(cl::PipelineDumpDir.c_str(), &shaderInfo->shaderBin, &hash);
  }

  return result;
}

// =====================================================================================================================
// Get resource node data info from spriv variable
//
// @param spvVar : Spriv variable
// @param [out] symbolInfo : Resource node data info
// @return: Whether symbol is builtIn
static bool getSymbolInfoFromSpvVariable(const SPIRVVariable *spvVar, ResourceNodeData *symbolInfo) {
  uint32_t arraySize = 1;
  SPIRVWord location = 0;
  SPIRVWord binding = 0;
  SPIRVWord varId = 0;
  BasicType basicType = BasicType::Unknown;
  symbolInfo->columnCount = 1;
  symbolInfo->componentCount = 1;

  SPIRVWord builtIn = false;
  bool isBuiltIn = spvVar->hasDecorate(DecorationBuiltIn, 0, &builtIn);
  spvVar->hasDecorate(DecorationLocation, 0, &location);
  spvVar->hasDecorate(DecorationBinding, 0, &binding);
  varId = spvVar->getId();

  SPIRVType *varElemTy = spvVar->getType()->getPointerElementType();
  while (varElemTy->isTypeArray()) {
    arraySize *= varElemTy->getArrayLength();
    varElemTy = varElemTy->getArrayElementType();
  }
  if (varElemTy->getOpCode() == OpTypeStruct) {
    for (uint32_t i = 0; i < arraySize; i++) {
      if (isBuiltIn)
        break;
      isBuiltIn = varElemTy->hasMemberDecorate(i, DecorationBuiltIn, 0, &builtIn);
    }
  }
  if (varElemTy->getOpCode() == OpTypeMatrix) {
    symbolInfo->columnCount = varElemTy->getMatrixColumnCount();
    varElemTy = varElemTy->getMatrixColumnType();
  }
  if (varElemTy->getOpCode() == OpTypeVector) {
    symbolInfo->componentCount = varElemTy->getVectorComponentCount();
    varElemTy = varElemTy->getVectorComponentType();
  }

  switch (varElemTy->getOpCode()) {
  case OpTypeInt: {
    bool isSigned = reinterpret_cast<SPIRVTypeInt *>(varElemTy)->isSigned();
    switch (varElemTy->getIntegerBitWidth()) {
    case 8:
      basicType = isSigned ? BasicType::Int8 : BasicType::Uint8;
      break;
    case 16:
      basicType = isSigned ? BasicType::Int16 : BasicType::Uint16;
      break;
    case 32:
      basicType = isSigned ? BasicType::Int : BasicType::Uint;
      break;
    case 64:
      basicType = isSigned ? BasicType::Int64 : BasicType::Uint64;
      break;
    }
    break;
  }
  case OpTypeFloat: {
    switch (varElemTy->getFloatBitWidth()) {
    case 16:
      basicType = BasicType::Float16;
      break;
    case 32:
      basicType = BasicType::Float;
      break;
    case 64:
      basicType = BasicType::Double;
      break;
    }
    break;
  }
  default: {
    break;
  }
  }

  symbolInfo->arraySize = arraySize;
  symbolInfo->location = location;
  symbolInfo->binding = binding;
  symbolInfo->basicType = basicType;
  symbolInfo->spvId = varId;

  return isBuiltIn;
}

// =====================================================================================================================
// Get sampler array size in default uniform struct
//
// @param spvStruct : Spriv default uniform struct type
// @return: Sampler array size in this struct
static unsigned getSamplerArraySizeInSpvStruct(const SPIRVType *spvStruct) {
  assert(spvStruct->isTypeStruct());

  unsigned memberCount = spvStruct->getStructMemberCount();
  unsigned samplerArraySize = 0;

  for (unsigned memberIdx = 0; memberIdx < memberCount; memberIdx++) {
    SPIRVType *memberTy = spvStruct->getStructMemberType(memberIdx);

    if (memberTy->isTypeSampledImage()) {
      samplerArraySize += 1;
    } else if (memberTy->isTypeArray()) {
      unsigned arraySize = 1;
      while (memberTy->isTypeArray()) {
        arraySize = memberTy->getArrayLength();
        memberTy = memberTy->getArrayElementType();
      }
      if (memberTy->isTypeSampledImage()) {
        samplerArraySize += arraySize;
      } else if (memberTy->isTypeStruct()) {
        samplerArraySize += (arraySize * getSamplerArraySizeInSpvStruct(memberTy));
      }
    } else if (memberTy->isTypeStruct()) {
      samplerArraySize += getSamplerArraySizeInSpvStruct(memberTy);
    }
  }

  return samplerArraySize;
}

// =====================================================================================================================
// Get shader module usage information by analyzing instructions
//
// @param module : Spriv module binary
// @param func : Spriv function
// @param [out] texelFetchImageIds: image Ids which are called by texelFetch
// @param [out] shaderModuleUsage: usage info of a shader module
static void getShaderModuleUsageFromInst(SPIRVModule *module, SPIRVFunction *func,
                                         std::set<unsigned> &texelFetchImageIds, ShaderModuleUsage &shaderModuleUsage) {
  std::map<unsigned, unsigned> instLoadVars;  // <dstOperand, srcOperand> pair of OpLoad instruction
  std::map<unsigned, unsigned> instImageVars; // <dstOperand, srcOperand> pair of OpImage instruction
  // Store variable ids for each OpAccessChain, avoid to handle these variables in OpStore again
  std::set<unsigned> aggregateVarIds;

  // Lambda to set the state of legacy built-in
  auto setLegacyBuiltInsUsageInfo = [&](unsigned location) {
    if (location == static_cast<SPIRVWord>(ClipVertex))
      shaderModuleUsage.useClipVertex = true;
    if (location == static_cast<SPIRVWord>(FrontColor))
      shaderModuleUsage.useFrontColor = true;
    if (location == static_cast<SPIRVWord>(BackColor))
      shaderModuleUsage.useBackColor = true;
    if (location == static_cast<SPIRVWord>(FrontSecondaryColor))
      shaderModuleUsage.useFrontSecondaryColor = true;
    if (location == static_cast<SPIRVWord>(BackSecondaryColor))
      shaderModuleUsage.useBackSecondaryColor = true;
  };

  for (auto j = 0; j < func->getNumBasicBlock(); ++j) {
    auto *bb = func->getBasicBlock(j);
    size_t instNum = bb->getNumInst();
    for (auto k = 0; k < instNum; ++k) {
      auto *inst = bb->getInst(k);
      spv::Op opCode = inst->getOpCode();
      if (opCode != spv::OpStore && opCode != spv::OpAccessChain && opCode != spv::OpLoad && opCode != spv::OpImage &&
          opCode != spv::OpImageFetch)
        continue;

      std::vector<SPIRVValue *> ops;
      std::vector<SPIRVId> ids;
      if (opCode != spv::OpLoad) {
        ops = inst->getOperands();
        ids = inst->getIds(ops);
      }

      if (opCode == spv::OpStore && aggregateVarIds.find(ids[1]) == aggregateVarIds.end()) {
        SPIRVType *varType = ops[1]->getValueType(ids[1]);
        auto var = static_cast<SPIRVVariable *>(module->getValue(ids[1]));
        // The builtin outputs are global variables
        if (varType->getPointerStorageClass() == StorageClassOutput) {
          SPIRVWord location = SPIRVID_INVALID;
          if (var->hasDecorate(DecorationLocation, 0, &location)) {
            setLegacyBuiltInsUsageInfo(location);
          }
        }
      } else if (opCode == spv::OpAccessChain) {
        aggregateVarIds.insert(inst->getId());

        SPIRVType *varType = ops[0]->getValueType(ids[0]);
        SPIRVType *elemTy = varType->getPointerElementType();
        uint64_t index = static_cast<SPIRVConstant *>(ops[1])->getZExtIntValue();
        if (varType->getPointerStorageClass() == StorageClassOutput) {
          // The builtin outputs are structure members (glsl version 150 and above)
          if (elemTy->isTypeStruct()) {
            SPIRVWord builtIn = false;
            SPIRVWord location = SPIRVID_INVALID;
            auto *memberType = elemTy->getStructMemberType(index);
            if (elemTy->hasMemberDecorate(index, DecorationBuiltIn, 0, &builtIn)) {
              if (builtIn == spv::BuiltInClipDistance)
                shaderModuleUsage.clipDistanceArraySize = memberType->getArrayLength();
            } else if (elemTy->hasMemberDecorate(index, DecorationLocation, 0, &location)) {
              setLegacyBuiltInsUsageInfo(location);
            }
          } else if (elemTy->isTypeArray()) {
            // gl_ClipDistance[] are global variables (glsl version 130 and 140)
            auto var = static_cast<SPIRVVariable *>(module->getValue(ids[0]));
            SPIRVWord builtIn = 0;
            if (var->hasDecorate(DecorationBuiltIn, 0, &builtIn) && builtIn == spv::BuiltInClipDistance)
              shaderModuleUsage.clipDistanceArraySize = elemTy->getArrayLength();
          }
        }
      } else if (opCode == spv::OpLoad) {
        SPIRVLoad *load = static_cast<SPIRVLoad *>(inst);
        instLoadVars[load->getId()] = load->getSrc()->getId();
      } else if (opCode == spv::OpImage) {
        auto iter = instLoadVars.find(ids[0]);
        if (iter != instLoadVars.end())
          instImageVars[inst->getId()] = iter->second;
      } else if (opCode == spv::OpImageFetch) {
        auto iter = instImageVars.find(ids[0]);
        if (iter != instImageVars.end())
          texelFetchImageIds.insert(iter->second);
      }
    }
  }
}

// =====================================================================================================================
// Parse the spirv binary to build the resource node data for buffers and opaque types, the resource node data will be
// returned to client driver together with other info of ShaderModuleUsage
//
// @param shaderInfo : Input shader info, including spirv binary
// @param [out] resourcesNodes : Output of resource usage
// @param [out] inputSymbolInfos : Output of input symbol infos
// @param [out] outputSymbolInfo : Output of output symbol infos
// @param [out] uniformBufferInfo : Output of uniform buffer infos
// @param [out] storageBufferInfo : Output of shader storage buffer infos
// @param [out] textureSymbolInfo : Output of texture symbol infos
// @param [out] imageSymbolInfo : Output of image symbol infos
// @param [out] atomicCounterSymbolInfo : Output of atomic counter symbol infos
// @param [out] defaultUniformSymbolInfo : Output of default uniform symbol infos
void Compiler::buildShaderModuleResourceUsage(
    const ShaderModuleBuildInfo *shaderInfo, Vkgc::ResourcesNodes &resourcesNodes,
    std::vector<ResourceNodeData> &inputSymbolInfo, std::vector<ResourceNodeData> &outputSymbolInfo,
    std::vector<ResourceNodeData> &uniformBufferInfo, std::vector<ResourceNodeData> &storageBufferInfo,
    std::vector<ResourceNodeData> &textureSymbolInfo, std::vector<ResourceNodeData> &imageSymbolInfo,
    std::vector<ResourceNodeData> &atomicCounterSymbolInfo, std::vector<ResourceNodeData> &defaultUniformSymbolInfo,
    ShaderModuleUsage &shaderModuleUsage) {
  // Parse the SPIR-V stream.
  std::string spirvCode(static_cast<const char *>(shaderInfo->shaderBin.pCode), shaderInfo->shaderBin.codeSize);
  std::istringstream spirvStream(spirvCode);
  std::unique_ptr<SPIRVModule> module(SPIRVModule::createSPIRVModule());
  spirvStream >> *module;

  ShaderStage shaderStage = shaderInfo->entryStage;
  std::set<unsigned> texelFetchImageIds;

  // Find the entry target.
  SPIRVEntryPoint *entryPoint = nullptr;
  SPIRVFunction *func = nullptr;
  for (unsigned i = 0, funcCount = module->getNumFunctions(); i < funcCount; ++i) {
    func = module->getFunction(i);
    getShaderModuleUsageFromInst(module.get(), func, texelFetchImageIds, shaderModuleUsage);
    entryPoint = module->getEntryPoint(func->getId());
    if (entryPoint && entryPoint->getExecModel() == convertToExecModel(shaderStage) &&
        entryPoint->getName() == shaderInfo->pEntryTarget)
      break;
    func = nullptr;
  }
  if (!entryPoint)
    return;

  if (func) {
    if (auto em = func->getExecutionMode(ExecutionModeLocalSize)) {
      shaderModuleUsage.localSizeX = em->getLiterals()[0];
      shaderModuleUsage.localSizeY = em->getLiterals()[1];
      shaderModuleUsage.localSizeZ = em->getLiterals()[2];
    }
  }

  // Process resources
  auto inOuts = entryPoint->getInOuts();
  std::vector<ResourceNodeData> inputSymbolWithArrayInfo;
  for (auto varId : ArrayRef<SPIRVWord>(inOuts.first, inOuts.second)) {
    auto var = static_cast<SPIRVVariable *>(module->getValue(varId));

    if (var->getStorageClass() == StorageClassInput) {
      if (shaderStage == ShaderStageVertex) {
        ResourceNodeData inputSymbol = {};
        if (!getSymbolInfoFromSpvVariable(var, &inputSymbol))
          inputSymbolWithArrayInfo.push_back(inputSymbol);
      }
    } else if (var->getStorageClass() == StorageClassOutput) {
      ResourceNodeData outputSymbol = {};
      if (!getSymbolInfoFromSpvVariable(var, &outputSymbol))
        outputSymbolInfo.push_back(outputSymbol);
    }
  }

  // Spirv Reader will expand matrix to vector arrays.
  // Add more rsrc node here to avoid poison value in vtxFetch.
  if (shaderInfo->entryStage == ShaderStage::ShaderStageVertex) {
    size_t inputSymbolSize = inputSymbolWithArrayInfo.size();
    for (size_t i = 0; i < inputSymbolSize; i++) {
      auto symbol = inputSymbolWithArrayInfo[i];
      int baseLocation = symbol.location;

      for (uint32_t ite = 0; ite < symbol.arraySize * symbol.columnCount; ite++) {
        ResourceNodeData elemSymbolInfo = symbol;
        inputSymbolInfo.push_back(elemSymbolInfo);
        inputSymbolInfo.back().location = baseLocation;
        baseLocation++;
      }
    }
  }

  for (unsigned i = 0, varCount = module->getNumVariables(); i < varCount; ++i) {
    SPIRVVariable *var = module->getVariable(i);
    SPIRVType *varElemTy = var->getType()->getPointerElementType();
    while (varElemTy->isTypeArray())
      varElemTy = varElemTy->getArrayElementType();

    switch (var->getStorageClass()) {
    case StorageClassUniform: {
      if (varElemTy->hasDecorate(DecorationBlock)) {
        ResourceNodeData uniformBufferSymbol = {};
        if (!getSymbolInfoFromSpvVariable(var, &uniformBufferSymbol))
          uniformBufferInfo.push_back(uniformBufferSymbol);
      } else {
        ResourceNodeData shaderStorageSymbol = {};
        if (!getSymbolInfoFromSpvVariable(var, &shaderStorageSymbol))
          storageBufferInfo.push_back(shaderStorageSymbol);
      }
    } break;
    case StorageClassStorageBuffer: {
      ResourceNodeData shaderStorageSymbol = {};
      if (!getSymbolInfoFromSpvVariable(var, &shaderStorageSymbol))
        storageBufferInfo.push_back(shaderStorageSymbol);
    } break;
    case StorageClassUniformConstant: {
      if (varElemTy->isTypeImage()) {
        ResourceNodeData imageSymbol = {};
        if (!getSymbolInfoFromSpvVariable(var, &imageSymbol)) {
          SPIRVTypeImage *imageType = static_cast<SPIRVTypeImage *>(varElemTy);
          imageSymbol.isTexelBuffer = imageType->getDescriptor().Dim == spv::DimBuffer;
          imageSymbolInfo.push_back(imageSymbol);
        }
      } else if (varElemTy->isTypeSampledImage()) {
        ResourceNodeData textureSymbol = {};
        if (!getSymbolInfoFromSpvVariable(var, &textureSymbol)) {
          SPIRVTypeSampledImage *sampledImageType = static_cast<SPIRVTypeSampledImage *>(varElemTy);
          SPIRVTypeImage *imageType = sampledImageType->getImageType();
          textureSymbol.isTexelBuffer = imageType->getDescriptor().Dim == spv::DimBuffer;
          textureSymbol.isTexelFetchUsed = (texelFetchImageIds.find(var->getId()) != texelFetchImageIds.end());
          textureSymbolInfo.push_back(textureSymbol);
        }
      } else {
        ResourceNodeData defaultUniformSymbol = {};
        if (!getSymbolInfoFromSpvVariable(var, &defaultUniformSymbol))
          defaultUniformSymbolInfo.push_back(defaultUniformSymbol);
        // Process image sampler in default uniform
        if (varElemTy->isTypeStruct()) {
          ResourceNodeData textureSymbol = {};
          textureSymbol.binding = defaultUniformSymbol.binding;
          textureSymbol.location = defaultUniformSymbol.location;
          textureSymbol.spvId = defaultUniformSymbol.spvId;
          textureSymbol.arraySize = getSamplerArraySizeInSpvStruct(varElemTy) * defaultUniformSymbol.arraySize;
          textureSymbol.isDefaultUniformSampler = true;
          if (textureSymbol.arraySize > 0)
            textureSymbolInfo.push_back(textureSymbol);
        }
      }
    } break;
    case StorageClassAtomicCounter: {
      ResourceNodeData atomicCounterSymbol = {};
      if (!getSymbolInfoFromSpvVariable(var, &atomicCounterSymbol))
        atomicCounterSymbolInfo.push_back(atomicCounterSymbol);
    } break;
    default:
      break;
    }
  }

  resourcesNodes.inputInfoCount = inputSymbolInfo.size();
  resourcesNodes.outputInfoCount = outputSymbolInfo.size();
  resourcesNodes.uniformBufferInfoCount = uniformBufferInfo.size();
  resourcesNodes.shaderStorageInfoCount = storageBufferInfo.size();
  resourcesNodes.textureInfoCount = textureSymbolInfo.size();
  resourcesNodes.imageInfoCount = imageSymbolInfo.size();
  resourcesNodes.atomicCounterInfoCount = atomicCounterSymbolInfo.size();
  resourcesNodes.defaultUniformInfoCount = defaultUniformSymbolInfo.size();
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
// Helper function for dumping fragment outputs
//
// @param pipelineDumpFile : Handle of pipeline dump file
// @param data : fragment output buffer
// @param size : buffer size
void Compiler::dumpFragmentOutputs(void *pipelineDumpFile, const uint8_t *data, unsigned size) {
  if (!pipelineDumpFile)
    return;
  PipelineDumper::DumpFragmentOutputs(reinterpret_cast<PipelineDumpFile *>(pipelineDumpFile), data, size);
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
  cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, stage);
  pipelineHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false, stage);

  // Compile
  GraphicsContext graphicsContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
  Context *context = acquireContext();
  context->attachPipelineContext(&graphicsContext);
  auto onExit = make_scope_exit([&] { releaseContext(context); });

  ElfPackage candidateElf;
  Result result =
      buildUnlinkedShaderInternal(context, shaderInfo, stage, candidateElf, pipelineOut->stageCacheAccesses);

  if (result != Result::Success)
    return result;

  unsigned metaDataSize = 0;
  SmallVector<FsOutInfo, 8> fsOuts;
  bool discardState = false;
  if (stage == UnlinkedStageFragment && pipelineInfo->enableColorExportShader) {
    // Parse ELF to get outputs
    ElfWriter<Elf64> writer(m_gfxIp);
    if (writer.ReadFromBuffer(candidateElf.data(), candidateElf.size()) == Result::Success) {
      ElfNote metaNote = writer.getNote(Abi::MetadataNoteType); // NT_AMDGPU_METADATA
      msgpack::Document document;
      document.readFromBlob(StringRef(reinterpret_cast<const char *>(metaNote.data), metaNote.hdr.descSize), false);
      auto pipeNode =
          document.getRoot().getMap(true)[PalAbi::CodeObjectMetadataKey::Pipelines].getArray(true)[0].getMap(true);

      // Color export infos
      auto it = pipeNode.find(Vkgc::ColorExports);
      if (it != pipeNode.end()) {
        assert(it->second.isArray());
        auto colorExports = it->second.getArray();
        for (unsigned i = 0; i < colorExports.size(); ++i) {
          msgpack::ArrayDocNode fetchNode = colorExports[i].getArray();
          FsOutInfo output;
          output.hwColorTarget = fetchNode[0].getUInt();
          output.location = fetchNode[1].getUInt();
          output.isSigned = fetchNode[2].getBool();
          StringRef typeName = fetchNode[3].getString();
          memset(output.typeName, 0, sizeof(output.typeName));
          memcpy(output.typeName, typeName.data(), typeName.size());
          fsOuts.push_back(output);
        }
        metaDataSize = sizeof(FragmentOutputs) + sizeof(FsOutInfo) * fsOuts.size();
      }
      // Discard state
      it = pipeNode.find(Vkgc::DiscardState);
      if (it != pipeNode.end())
        discardState = it->second.getBool();
    }
  }

  if (pipelineInfo->enableColorExportShader) {
    // Output elf for each stage if enableColorExportShader
    if (Llpc::EnableOuts()) {
      ElfReader<Elf64> reader(m_gfxIp);
      size_t readSize = 0;
      if (reader.ReadFromBuffer(candidateElf.data(), &readSize) == Result::Success) {
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC final " << getUnlinkedShaderStageName(stage) << " ELF\n");
        LLPC_OUTS(reader);
      }
    }
  }

  void *allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData,
                                                candidateElf.size() + metaDataSize);
  uint8_t *code = static_cast<uint8_t *>(allocBuf);
  memcpy(code, candidateElf.data(), candidateElf.size());
  pipelineOut->pipelineBin.codeSize = candidateElf.size();
  pipelineOut->pipelineBin.pCode = code;

  if (metaDataSize > 0) {
    pipelineOut->fsOutputMetaData = code + candidateElf.size();
    pipelineOut->fsOutputMetaDataSize = metaDataSize;
    FragmentOutputs *outputs = static_cast<FragmentOutputs *>(pipelineOut->fsOutputMetaData);
    outputs->fsOutInfoCount = fsOuts.size();
    outputs->discard = discardState;
    void *offsetData = static_cast<uint8_t *>(pipelineOut->fsOutputMetaData) + sizeof(FragmentOutputs);
    memcpy(offsetData, fsOuts.data(), sizeof(FsOutInfo) * fsOuts.size());
  }
  return result;
}

// =====================================================================================================================
// Explicitly build the color export shader.
//
// @param [in]  pipelineInfo : Info to build this shader module
// @param [in]  fsOutputMetaData : Info to fragment outputs
// @param [out] pipelineOut  : Output of building this shader module
// @param [out] pipelineDumpFile : Handle of pipeline dump file
//
// @returns : Result::Success if successful. Other return codes indicate failure.
Result Compiler::BuildColorExportShader(const GraphicsPipelineBuildInfo *pipelineInfo, const void *fsOutputMetaData,
                                        GraphicsPipelineBuildOut *pipelineOut, void *pipelineDumpFile) {

  if (!pipelineInfo->pfnOutputAlloc)
    return Result::ErrorInvalidPointer;

  if (pipelineInfo->iaState.enableMultiView) {
    LLPC_OUTS("Relocatable shader doesn't support \"MultiView\"");
    return Result::RequireFullPipeline;
  }

  if (!fsOutputMetaData)
    return Result::Success;

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  const FragmentOutputs *fsOuts = static_cast<const FragmentOutputs *>(fsOutputMetaData);
  const uint8 *metaPtr = static_cast<const uint8 *>(fsOutputMetaData);
  unsigned metaDatatSize = sizeof(FragmentOutputs) + fsOuts->fsOutInfoCount * sizeof(FsOutInfo);
  MetroHash64 hasher;
  hasher.Update(metaPtr, metaDatatSize);
  hasher.Update(pipelineInfo->options);
  hasher.Update(pipelineInfo->cbState);
  hasher.Finalize(pipelineHash.bytes);

  GraphicsContext graphicsContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
  Context *context = acquireContext();
  context->attachPipelineContext(&graphicsContext);
  LgcContext *builderContext = context->getLgcContext();
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  context->getPipelineContext()->setPipelineState(&*pipeline, /*hasher=*/nullptr, /*unlinked=*/false);
  auto onExit = make_scope_exit([&] { releaseContext(context); });

  SmallVector<ColorExportInfo, 8> exports;
  metaPtr = metaPtr + sizeof(FragmentOutputs);
  const FsOutInfo *outInfos = reinterpret_cast<const FsOutInfo *>(metaPtr);

  for (unsigned idx = 0; idx < fsOuts->fsOutInfoCount; idx++) {
    auto outInfo = outInfos[idx];
    ColorExportInfo colorExportInfo;
    colorExportInfo.hwColorTarget = outInfo.hwColorTarget;
    colorExportInfo.location = outInfo.location;
    colorExportInfo.isSigned = outInfo.isSigned;
    StringRef tyName = outInfo.typeName;
    Type *ty = nullptr;
    unsigned vecLength = 0;
    if (tyName[0] == 'v') {
      tyName = tyName.drop_front();
      tyName.consumeInteger(10, vecLength);
    }
    if (tyName == "i8")
      ty = Type::getInt8Ty(*context);
    else if (tyName == "i16")
      ty = Type::getInt16Ty(*context);
    else if (tyName == "i32")
      ty = Type::getInt32Ty(*context);
    else if (tyName == "f16")
      ty = Type::getHalfTy(*context);
    else if (tyName == "f32")
      ty = Type::getFloatTy(*context);
    if (vecLength != 0 && ty != nullptr)
      ty = FixedVectorType::get(ty, vecLength);
    colorExportInfo.ty = ty;
    exports.push_back(colorExportInfo);
  }

  // If no outputs, return;
  if (exports.empty())
    return Result::Success;

  dumpFragmentOutputs(pipelineDumpFile, static_cast<const uint8 *>(fsOutputMetaData), metaDatatSize);
  dumpCompilerOptions(pipelineDumpFile);
  bool hasError = false;
  context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));
  std::unique_ptr<ElfLinker> elfLinker(pipeline->createElfLinker({}));
  StringRef hashStr = elfLinker->createColorExportShader(exports, fsOuts->discard);

  BinaryData elf = {};
  MetroHash::Hash colorExportCache = {};
  MetroHash64 cacheHasher;
  cacheHasher.Update(reinterpret_cast<const uint8_t *>(hashStr.data()), hashStr.size());
  cacheHasher.Finalize(colorExportCache.bytes);
  CacheAccessor cacheAccessor(colorExportCache, getInternalCaches());
  if (cacheAccessor.isInCache()) {
    LLPC_OUTS("Cache hit for color export shader.\n");
    elf = cacheAccessor.getElfFromCache();
  } else {
    LLPC_OUTS("Cache miss for color export shader.\n");
    StringRef elfStr = elfLinker->compileGlue(0);
    elf.pCode = elfStr.data();
    elf.codeSize = elfStr.size();
    cacheAccessor.setElfInCache(elf);
  }

  context->setDiagnosticHandler(nullptr);

  if (hasError)
    return Result::ErrorInvalidShader;
  if (Llpc::EnableOuts()) {
    ElfReader<Elf64> reader(m_gfxIp);
    size_t readSize = 0;
    if (reader.ReadFromBuffer(elf.pCode, &readSize) == Result::Success) {
      LLPC_OUTS("===============================================================================\n");
      LLPC_OUTS("// LLPC final color export shader ELF\n");
      LLPC_OUTS(reader);
    }
  }

  void *allocBuf = pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, elf.codeSize);
  uint8_t *code = static_cast<uint8_t *>(allocBuf);
  memcpy(code, elf.pCode, elf.codeSize);
  pipelineOut->pipelineBin.codeSize = elf.codeSize;
  pipelineOut->pipelineBin.pCode = code;

  return Result::Success;
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
    return Result::RequireFullPipeline;
  }

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true);
  pipelineHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false);

  std::optional<CacheAccessor> cacheAccessor;
  if (cl::CacheFullPipelines) {
    cacheAccessor.emplace(cacheHash, getInternalCaches());
  }

  Result result = Result::Success;
  BinaryData elfBin = {};
  ElfPackage elf[UnlinkedStageCount] = {};
  ElfPackage pipelineElf;
  if (cacheAccessor && cacheAccessor->isInCache()) {
    LLPC_OUTS("Cache hit for graphics pipeline.\n");
    elfBin = cacheAccessor->getElfFromCache();
    pipelineOut->pipelineCacheAccess = CacheAccessInfo::InternalCacheHit;
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
        if (idx == UnlinkedStageFragment) {
          pipelineOut->stageCacheAccesses[ShaderStageFragment] = PartialPipelineHit;
        } else {
          for (unsigned stage = 0; stage < ShaderStageFragment; stage++) {
            if (doesShaderStageExist(shaderInfo, static_cast<ShaderStage>(stage))) {
              pipelineOut->stageCacheAccesses[stage] = PartialPipelineHit;
            }
          }
        }
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
    assert(!hasError);

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

  if (stage == UnlinkedStageFragment) {
    assert(shaderInfo[ShaderStageFragment]->pModuleData);
    // If fragment use builtIn inputs, return RequireFullPipeline.
    const ShaderModuleData *moduleData =
        static_cast<const ShaderModuleData *>(shaderInfo[ShaderStageFragment]->pModuleData);
    if (moduleData->usage.useGenericBuiltIn) {
      // TODO: We have added semantic to support generic builtIn, however, there seems to be some errors. We need to
      // add more info to sync inputs and outputs.
      return Result::RequireFullPipeline;
    }
  }

  unsigned originalShaderStageMask = context->getPipelineContext()->getShaderStageMask();
  const MetroHash::Hash originalCacheHash = context->getPipelineContext()->getCacheHashCodeWithoutCompact();
  unsigned shaderStageMask = getShaderStageMaskForType(stage) & originalShaderStageMask;
  context->getPipelineContext()->setShaderStageMask(shaderStageMask);
  const auto shaderStages = maskToShaderStages(shaderStageMask);
  assert(all_of(shaderStages, isNativeStage) && "Unexpected stage kind");

  // Check the cache for the relocatable shader for this stage.
  MetroHash::Hash cacheHash = {};
  auto caches = getInternalCaches();
  if (context->getPipelineType() == PipelineType::Graphics) {
    auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
    cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, stage);
  } else {
    auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
    cacheHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, true);
    // we have pipeline cache for compute pipeline, Per stage cache is not needed for compute pipeline.
    caches = {};
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
  CacheAccessor cacheAccessor(cacheHash, caches);
  if (cacheAccessor.isInCache()) {
    BinaryData elfBin = cacheAccessor.getElfFromCache();
    auto data = reinterpret_cast<const char *>(elfBin.pCode);
    elfPackage.assign(data, data + elfBin.codeSize);
    LLPC_OUTS("Cache hit for shader stage " << getUnlinkedShaderStageName(stage) << "\n");
    for (ShaderStage gfxStage : shaderStages)
      stageCacheAccesses[gfxStage] = CacheAccessInfo::InternalCacheHit;
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
// Returns true if a graphics pipeline can be built out of the given shader infos.
//
// @param shaderInfos : Shader infos for the pipeline to be built
// @param pipelineInfo : Pipeline info for the pipeline to be built
bool Compiler::canUseRelocatableGraphicsShaderElf(const ArrayRef<const PipelineShaderInfo *> &shaderInfos,
                                                  const GraphicsPipelineBuildInfo *pipelineInfo) {
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
  TimerProfiler timerProfiler(context->getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);
  bool buildingRelocatableElf = context->getPipelineContext()->isUnlinked();

  bool hasError = false;
  context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

  // Set up middle-end objects.
  LgcContext *builderContext = context->getLgcContext();
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  context->getPipelineContext()->setPipelineState(&*pipeline, /*hasher=*/nullptr,
                                                  pipelineLink == PipelineLink::Unlinked);
  context->setBuilder(builderContext->createBuilder(&*pipeline));

  std::unique_ptr<Module> pipelineModule;
  bool needLowerGpurt = false;

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
    SmallVector<std::unique_ptr<Module>> modules(shaderInfo.size());
    unsigned stageSkipMask = 0;

    bool enableAdvancedBlend = false;
    const GraphicsPipelineBuildInfo *pipelineInfo = nullptr;
    const PipelineShaderInfo *shaderInfoEntry = nullptr;
    if (pipelineLink == PipelineLink::PartPipeline) {
      if (shaderInfo.size() == 1)
        shaderInfoEntry = shaderInfo[0];
    } else {
      shaderInfoEntry = shaderInfo[ShaderStageFragment];
    }
    if (shaderInfoEntry && shaderInfoEntry->pModuleData) {
      pipelineInfo =
          static_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineContext()->getPipelineBuildInfo());
      enableAdvancedBlend = pipelineInfo->advancedBlendInfo.enableAdvancedBlend;
    }
    if (enableAdvancedBlend)
      context->ensureGfxRuntimeLibrary();

    for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
      const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
      if (!shaderInfoEntry || !shaderInfoEntry->pModuleData)
        continue;

      ShaderStage entryStage = shaderInfoEntry ? shaderInfoEntry->entryStage : ShaderStageInvalid;
      if (stageSkipMask & shaderStageToMask(entryStage))
        continue;

      const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);

      if (moduleData->binType == BinaryType::MultiLlvmBc) {
        result = Result::ErrorInvalidShader;
        continue;
      }

      modules[shaderIndex].reset(
          new Module((Twine("llpc") + "_" + getShaderStageName(shaderInfoEntry->entryStage)).str() + "_" +
                         std::to_string(getModuleIdByIndex(shaderIndex)),
                     *context));

      context->setModuleTargetMachine(modules[shaderIndex].get());

      // If input shader module is llvm bc, skip spirv to llvm translation
      if (moduleData->binType == BinaryType::LlvmBc) {
        llvm::SMDiagnostic errDiag;
        llvm::StringRef bcStringRef(static_cast<const char *>(moduleData->binCode.pCode), moduleData->binCode.codeSize);
        llvm::MemoryBufferRef bcBufferRef(bcStringRef, "");

        Expected<std::unique_ptr<Module>> MOrErr = llvm::parseBitcodeFile(bcBufferRef, *context);
        if (!MOrErr) {
          report_fatal_error("Failed to read bitcode");
          continue;
        }
        modules[shaderIndex] = std::move(*MOrErr);
      }

      std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
      lowerPassMgr->setPassIndex(&passIndex);
      SpirvLower::registerTranslationPasses(*lowerPassMgr);

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

      if (moduleData->usage.enableRayQuery) {
        assert(!moduleData->usage.rayQueryLibrary);
        context->ensureGpurtLibrary();
      }

      if (shaderIndex == ShaderStageFragment && enableAdvancedBlend) {
        lowerPassMgr->addPass(LowerAdvancedBlend(pipelineInfo->advancedBlendInfo.binding));
        if (EnableOuts()) {
          lowerPassMgr->addPass(PrintModulePass(
              outs(), "\n"
                      "===============================================================================\n"
                      "// LLPC Advanced Blend Pass results\n"));
        }
      }

      if (moduleData->usage.isInternalRtShader)
        setUseGpurt(&*pipeline);

      assert(!moduleData->usage.isInternalRtShader || entryStage == ShaderStageCompute);

      if (moduleData->usage.isInternalRtShader || moduleData->usage.enableRayQuery)
        needLowerGpurt |= true;

      // Stop timer for translate.
      timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, false);

      bool success = runPasses(&*lowerPassMgr, modules[shaderIndex].get());
      if (!success) {
        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
        result = Result::ErrorInvalidShader;
      }

      // If this is TCS, set inputVertices from patchControlPoints in the pipeline state.
      if (entryStage == ShaderStageTessControl ||
          (entryStage == ShaderStageTessEval && shaderInfo[ShaderStageTessControl]->pModuleData == nullptr))
        context->getPipelineContext()->setTcsInputVertices(modules[shaderIndex].get());
    }

    if (needLowerGpurt)
      setUseGpurt(&*pipeline);

    SmallVector<std::unique_ptr<Module>, ShaderStageGfxCount> modulesToLink;
    for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
      // Per-shader SPIR-V lowering passes.
      const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
      ShaderStage entryStage = shaderInfoEntry ? shaderInfoEntry->entryStage : ShaderStageInvalid;
      if (!shaderInfoEntry || !shaderInfoEntry->pModuleData)
        continue;
      if (stageSkipMask & shaderStageToMask(entryStage)) {
        // Do not run SPIR-V translator and lowering passes on this shader; we were given it as IR ready
        // to link into pipeline module.
        modulesToLink.push_back(std::move(modules[shaderIndex]));
        continue;
      }

      std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(context->getLgcContext()));
      lowerPassMgr->setPassIndex(&passIndex);
      SpirvLower::registerLoweringPasses(*lowerPassMgr);

      const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);
      LowerFlag flag = {};
      flag.isRayTracing = false;
      flag.isRayQuery = moduleData->usage.enableRayQuery;
      flag.isInternalRtShader = moduleData->usage.isInternalRtShader;
      flag.usesAdvancedBlend = enableAdvancedBlend;
      SpirvLower::addPasses(context, entryStage, *lowerPassMgr, timerProfiler.getTimer(TimerLower), flag);
      // Run the passes.
      bool success = runPasses(&*lowerPassMgr, modules[shaderIndex].get());
      if (!success) {
        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
        result = Result::ErrorInvalidShader;
      }

      // Add the shader module to the list for the pipeline.
      modulesToLink.push_back(std::move(modules[shaderIndex]));
    }

    // If this is a part-pipeline compile of the pre-rasterization stages, give the "other" pipeline object
    // containing the FS input mappings to our pipeline object.
    if (otherPartPipeline)
      pipeline->setOtherPartPipeline(*otherPartPipeline);

    // Link the shader modules into a single pipeline module.
    pipelineModule = pipeline->irLink(
        modulesToLink, context->getPipelineContext()->isUnlinked() ? PipelineLink::Unlinked : pipelineLink);
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
      [&graphicsShaderCacheChecker, stageCacheAccesses](const Module *module, ShaderStageMask stageMask,
                                                        ArrayRef<ArrayRef<uint8_t>> stageHashes) {
        ShaderStageMask result;
        result.m_value = graphicsShaderCacheChecker.check(module, stageMask.m_value, stageHashes, stageCacheAccesses);
        return result;
      };

  // Only enable per stage cache for full graphics pipeline (traditional pipeline or mesh pipeline)
  bool checkPerStageCache =
      cl::EnablePerStageCache && !cl::EnablePartPipeline && context->getPipelineType() == PipelineType::Graphics &&
      !buildingRelocatableElf &&
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
    m_fragmentCacheAccessor.emplace(fragmentHash, m_compiler->getInternalCaches());
    if (m_fragmentCacheAccessor->isInCache()) {
      // Remove fragment shader stages.
      stagesLeftToCompile &= ~getLgcShaderStageMask(ShaderStageFragment);
      stageCacheAccesses[ShaderStageFragment] = CacheAccessInfo::InternalCacheHit;
    } else {
      stageCacheAccesses[ShaderStageFragment] = CacheAccessInfo::CacheMiss;
    }
  }

  if (stageMask & ~getLgcShaderStageMask(ShaderStageFragment)) {
    auto accessInfo = CacheAccessInfo::CacheNotChecked;
    m_nonFragmentCacheAccessor.emplace(nonFragmentHash, m_compiler->getInternalCaches());
    if (m_nonFragmentCacheAccessor->isInCache()) {
      // Remove non-fragment shader stages.
      stagesLeftToCompile &= getLgcShaderStageMask(ShaderStageFragment);
      accessInfo = CacheAccessInfo::InternalCacheHit;
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

  Type *outputTy = FixedVectorType::get(Type::getFloatTy(*context), llvm::popcount(target->channelWriteMask));
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
    if (buildPartPipeline) {
      for (const auto *oneShaderInfo : shaderInfo) {
        if (!oneShaderInfo)
          continue;
        auto *moduleData = reinterpret_cast<const ShaderModuleData *>(oneShaderInfo->pModuleData);
        if (moduleData && moduleData->usage.enableRayQuery) {
          buildPartPipeline = false;
          break;
        }
      }
    }

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
          PipelineDumper::updateHashForPipelineShaderInfo(stage, shaderInfoEntry, /*isCacheHash=*/true, &hasher);
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
    CacheAccessor cacheAccessor(partPipelineHash, getInternalCaches());
    if (cacheAccessor.isInCache()) {
      LLPC_OUTS("Cache hit for stage " << getPartPipelineStageName(partPipelineStage) << ".\n");

      // Mark the applicable entries in stageCacheAccesses.
      for (ShaderStage shaderStage : maskToShaderStages(partStageMask)) {
        stageCacheAccesses[shaderStage] = CacheAccessInfo::InternalCacheHit;
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
      if (partPipelineElf.size() < 4 || !partPipelineElf.starts_with("\177ELF")) {
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

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true);
  pipelineHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, false);

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

  dumpCompilerOptions(pipelineDumpFile);

  std::optional<CacheAccessor> cacheAccessor;
  if (cl::CacheFullPipelines) {
    cacheAccessor.emplace(cacheHash, getInternalCaches());
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
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::InternalCacheHit;
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
  if (result != Result::Success)
    return result;

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, true);
  pipelineHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, false);

  if (EnableOuts()) {
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

  std::optional<CacheAccessor> cacheAccessor;
  if (cl::CacheFullPipelines) {
    cacheAccessor.emplace(cacheHash, getInternalCaches());
  }

  ElfPackage candidateElf;
  if (!cacheAccessor || !cacheAccessor->isInCache()) {
    LLPC_OUTS("Cache miss for compute pipeline.\n");
    ComputeContext computeContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
    result = buildComputePipelineInternal(&computeContext, pipelineInfo, buildUsingRelocatableElf, &candidateElf,
                                          &pipelineOut->stageCacheAccess);

    if (cacheAccessor && pipelineOut->pipelineCacheAccess == CacheAccessInfo::CacheNotChecked)
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::CacheMiss;

    if (result != Result::Success) {
      return result;
    }
    elfBin.codeSize = candidateElf.size();
    elfBin.pCode = candidateElf.data();
  } else {
    LLPC_OUTS("Cache hit for compute pipeline.\n");
    elfBin = cacheAccessor->getElfFromCache();
    pipelineOut->pipelineCacheAccess = CacheAccessInfo::InternalCacheHit;
  }

  if (!pipelineInfo->pfnOutputAlloc) // Allocator is not specified
    return Result::ErrorInvalidPointer;

  void *const allocBuf =
      pipelineInfo->pfnOutputAlloc(pipelineInfo->pInstance, pipelineInfo->pUserData, elfBin.codeSize);
  if (!allocBuf)
    return Result::ErrorOutOfMemory;

  uint8_t *code = static_cast<uint8_t *>(allocBuf);
  memcpy(code, elfBin.pCode, elfBin.codeSize);

  pipelineOut->pipelineBin.codeSize = elfBin.codeSize;
  pipelineOut->pipelineBin.pCode = code;

  if (cacheAccessor && !cacheAccessor->isInCache()) {
    cacheAccessor->setElfInCache(elfBin);
  }

  return Result::Success;
}

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

  std::vector<ElfPackage> elfBinarys;
  std::vector<RayTracingShaderProperty> shaderProps;

  const PipelineShaderInfo *representativeShaderInfo = nullptr;
  if (pipelineInfo->shaderCount > 0)
    representativeShaderInfo = &pipelineInfo->pShaders[0];

  RayTracingContext rayTracingContext(m_gfxIp, pipelineInfo, representativeShaderInfo, &pipelineHash, &cacheHash,
                                      pipelineInfo->indirectStageMask);
  auto &summary = rayTracingContext.getRayTracingLibrarySummary();
  summary.knownSetRayFlags = ~0;
  summary.knownUnsetRayFlags = ~0;

  // Note: These values are provided by the application via the ABI.
  summary.maxRayPayloadSize = pipelineInfo->payloadSizeMaxInLib;
  summary.maxHitAttributeSize = pipelineInfo->attributeSizeMaxInLib;

  for (unsigned i = 0; i < pipelineInfo->libraryCount; ++i) {
    const BinaryData &data = pipelineInfo->pLibrarySummaries[i];
    auto rls = cantFail(
        lgc::RayTracingLibrarySummary::decodeMsgpack(StringRef(static_cast<const char *>(data.pCode), data.codeSize)));
    summary.merge(rls);
  }

  pipelineOut->hasTraceRay = false;
  for (unsigned i = 0; i < pipelineInfo->shaderCount; ++i) {
    const auto &shaderInfo = pipelineInfo->pShaders[i];
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo.pModuleData);
    if (moduleData->usage.hasTraceRay) {
      summary.usesTraceRay = true;
      break;
    }
  }

  std::vector<const PipelineShaderInfo *> rayTracingShaderInfo;
  rayTracingShaderInfo.reserve(pipelineInfo->shaderCount + 1);
  for (unsigned i = 0; i < pipelineInfo->shaderCount; ++i) {
    rayTracingShaderInfo.push_back(&pipelineInfo->pShaders[i]);
    auto &shaderInfo = rayTracingShaderInfo[i];
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);

    if (shaderInfo->entryStage == ShaderStageRayTracingAnyHit ||
        shaderInfo->entryStage == ShaderStageRayTracingIntersect) {
      if (moduleData->usage.enableRayQuery) {
        rayTracingContext.setIndirectPipeline();
      }
    }
  }

  if (summary.knownSetRayFlags != ~0u || summary.knownUnsetRayFlags != ~0u) {
    auto knownBits = KnownBits(32);
    knownBits.One = APInt(32, summary.knownSetRayFlags);
    knownBits.Zero = APInt(32, summary.knownUnsetRayFlags);
    rayTracingContext.updateRayFlagsKnownBits(knownBits);
  }

  // Add entry module
  PipelineShaderInfo raygenMainShaderInfo = pipelineInfo->pShaders[0];
  raygenMainShaderInfo.entryStage = ShaderStageRayTracingRayGen;
  raygenMainShaderInfo.pModuleData = nullptr;
  rayTracingShaderInfo.push_back(&raygenMainShaderInfo);

  result = buildRayTracingPipelineInternal(rayTracingContext, rayTracingShaderInfo, false, elfBinarys, shaderProps,
                                           helperThreadProvider);

  if (result == Result::Success) {
    auto knownFlags = rayTracingContext.getRayFlagsKnownBits();
    summary.knownSetRayFlags &= knownFlags.One.getZExtValue();
    summary.knownUnsetRayFlags &= knownFlags.Zero.getZExtValue();

    pipelineOut->hasTraceRay = summary.hasTraceRayModule;

    std::string summaryMsgpack = summary.encodeMsgpack();
    void *allocBuf = nullptr;
    size_t shaderGroupHandleSize = pipelineInfo->shaderGroupCount * sizeof(RayTracingShaderIdentifier);
    size_t binaryDataSize = sizeof(BinaryData) * elfBinarys.size();
    size_t elfSize = 0;

    for (auto &elf : elfBinarys) {
      if (elf.size() % 8 != 0) {
        elf.resize(alignTo(elf.size(), alignof(BinaryData)));
      }
      elfSize += elf.size();
    }

    size_t allocSize = elfSize;
    allocSize += binaryDataSize;

    size_t shaderPropsSize = sizeof(RayTracingShaderProperty) * shaderProps.size();
    allocSize += shaderPropsSize;

    allocSize += shaderGroupHandleSize;

    allocSize += alignTo(summaryMsgpack.size(), 8);

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
    allocBuf = voidPtrInc(allocBuf, shaderGroupHandleSize);
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

    void *summaryOut = allocBuf;
    allocBuf = voidPtrInc(allocBuf, alignTo(summaryMsgpack.size(), 8));
    pipelineOut->librarySummary.pCode = summaryOut;
    pipelineOut->librarySummary.codeSize = summaryMsgpack.size();
    memcpy(summaryOut, summaryMsgpack.data(), summaryMsgpack.size());

    pipelineOut->isCps = rayTracingContext.isContinuationsMode();
    pipelineOut->isCps |= rayTracingContext.getRaytracingMode() == Vkgc::LlpcRaytracingMode::Continufy;
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
Result Compiler::buildRayTracingPipelineElf(Context *context, std::unique_ptr<Module> module, ElfPackage &pipelineElf,
                                            std::vector<RayTracingShaderProperty> &shaderProps,
                                            std::vector<bool> &moduleCallsTraceRay, unsigned moduleIndex,
                                            std::unique_ptr<Pipeline> &pipeline, TimerProfiler &timerProfiler) {
  auto rtContext = static_cast<RayTracingContext *>(context->getPipelineContext());
  if (moduleIndex > 0) {
    auto &shaderProp = shaderProps[moduleIndex - 1];
    const StringRef &funcName = module->getName();
    assert(funcName.size() <= RayTracingMaxShaderNameLength);
    strcpy(&shaderProp.name[0], funcName.data());
    shaderProp.shaderId = moduleIndex;
    shaderProp.hasTraceRay = moduleCallsTraceRay[moduleIndex - 1];
    shaderProp.onlyGpuVaLo = false;

    uint64_t shaderIdExtraBits = 0;
    if (rtContext->isContinuationsMode()) {
      if (auto stage = tryGetLgcRtShaderStageFromName(funcName)) {
        auto cpsLevel = cps::getCpsLevelForShaderStage(stage.value());
        shaderIdExtraBits |= static_cast<uint64_t>(cpsLevel);
      }
    }
    shaderProp.shaderIdExtraBits = shaderIdExtraBits;
  }

  auto options = pipeline->getOptions();
  MetroHash64 hasher;
  MetroHash::Hash hash = {};
  hasher.Update(options.hash[1]);
  hasher.Update(moduleIndex);
  hasher.Finalize(hash.bytes);
  options.hash[1] = MetroHash::compact64(&hash);

  if (rtContext->getIndirectStageMask() == 0) {
    options.rtIndirectMode = lgc::RayTracingIndirectMode::NotIndirect;
  } else if (rtContext->isContinuationsMode() && !LgcContext::getEmitLgc()) {
    // Assure indirect mode setting here, indirect stage mask may change after SPIRVReader.
    options.rtIndirectMode = lgc::RayTracingIndirectMode::Continuations;

    // For continuations mode, we need to run LowerRaytracingPipelinePass here first separately because we need to
    // collect metadata added by the pass
    std::unique_ptr<lgc::PassManager> passMgr(lgc::PassManager::Create(context->getLgcContext()));
    passMgr->registerModuleAnalysis([&] { return DialectContextAnalysis(false); });
    passMgr->addPass(LowerRaytracingPipelinePass());
    bool success = runPasses(&*passMgr, module.get());
    assert(success);
    (void(success)); // unused

    auto maxUsedPayloadRegisterCount = ContHelper::tryGetMaxUsedPayloadRegisterCount(*module).value_or(0);
    {
      // Library summary in rtContext could be shared between threads, need to ensure it is only modified by one thread
      // at a time.
      std::lock_guard<sys::Mutex> lock(getHelperThreadMutex());
      rtContext->getRayTracingLibrarySummary().maxUsedPayloadRegisterCount =
          std::max(rtContext->getRayTracingLibrarySummary().maxUsedPayloadRegisterCount, maxUsedPayloadRegisterCount);
    }
  }

  pipeline->setOptions(options);

  generatePipeline(context, moduleIndex, std::move(module), pipelineElf, pipeline.get(), timerProfiler);

  if (moduleIndex > 0)
    adjustRayTracingElf(&pipelineElf, rtContext, shaderProps[moduleIndex - 1]);

  return Result::Success;
}

// =====================================================================================================================
// Run lgc passes
// @param context : Acquired context
// @param moduleIndex : Module index
// @param module : The module used to build Elf package
// @param [out] pipelineElf : Output Elf package
// @param pipeline : The pipeline object
// @param timerProfiler : Timer profiler
Result Compiler::generatePipeline(Context *context, unsigned moduleIndex, std::unique_ptr<Module> module,
                                  ElfPackage &pipelineElf, Pipeline *pipeline, TimerProfiler &timerProfiler) {
  // Generate pipeline.
  std::unique_ptr<Module> pipelineModule;

  pipelineModule = pipeline->irLink(module, context->getPipelineContext()->isUnlinked() ? PipelineLink::Unlinked
                                                                                        : PipelineLink::WholePipeline);
  if (!pipelineModule) {
    LLPC_ERRS("Failed to link shader modules into pipeline module\n");
    return Result::ErrorInvalidShader;
  }

  raw_svector_ostream elfStream(pipelineElf);

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

  return Result::Success;
}

// =====================================================================================================================
// Set this pipeline use GPURT library
//
// @param pipeline : The pipeline object
void Compiler::setUseGpurt(lgc::Pipeline *pipeline) {
  auto options = pipeline->getOptions();
  options.useGpurt = true;
  pipeline->setOptions(options);
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

  context->ensureGpurtLibrary();
  helperThreadPayload->compiler->setUseGpurt(&*pipeline);

  TimerProfiler timerProfiler(context->getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);

  {
    // Block the helper thread until main thread has switched context, see comment in
    // Compiler::buildRayTracingPipelineInternal for why we need this.
    std::unique_lock<sys::Mutex> lock(helperThreadPayload->compiler->getHelperThreadMutex());
    helperThreadPayload->compiler->getHelperThreadConditionVariable().wait(
        lock, [helperThreadPayload]() { return helperThreadPayload->mainThreadSwitchedContext.load(); });
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
    auto result = helperThreadPayload->compiler->buildRayTracingPipelineElf(
        context, std::move(module), helperThreadPayload->pipelineElfs[moduleIndex], helperThreadPayload->shaderProps,
        helperThreadPayload->moduleCallsTraceRay, moduleIndex, pipeline, timerProfiler);

    helperThreadPayload->results[moduleIndex] = hasError ? Result::ErrorInvalidShader : result;

    helperThreadProvider->TaskCompleted();
  } while (helperThreadProvider->GetNextTask(&moduleIndex));

  context->setDiagnosticHandler(nullptr);
  helperThreadPayload->compiler->releaseContext(context);
}

// =====================================================================================================================
// Limited implementation of Llpc::IHelperThreadProvider to support -add-rt-helpers.
//
// If no deferred work helper thread providers is available when additional threads are requested via -add-rt-helpers
// then use an instances of this class to coordinate helper threads.
class InternalHelperThreadProvider : public Llpc::IHelperThreadProvider {
public:
  virtual void SetTasks(ThreadFunction *pFunction, uint32_t numTasks, void *pPayload) override {
    assert(!m_totalInstances && "InternalHelperThreadProvider is single use");
    m_totalInstances = numTasks;
  }

  virtual bool GetNextTask(uint32_t *pTaskIndex) override {
    assert(pTaskIndex != nullptr);
    *pTaskIndex = m_nextInstance.fetch_add(1);
    return (*pTaskIndex < m_totalInstances);
  }

  virtual void TaskCompleted() override {
    uint32_t completedInstances = m_completedInstances.fetch_add(1) + 1;
    if (completedInstances == m_totalInstances)
      m_event.notify_all();
  }

  virtual void WaitForTasks() override {
    std::unique_lock<std::mutex> lock(m_lock);
    while (m_completedInstances < m_totalInstances)
      m_event.wait(lock);
  }

private:
  uint32_t m_totalInstances = 0;
  std::atomic<uint32_t> m_nextInstance = 0;
  std::atomic<uint32_t> m_completedInstances = 0;
  std::condition_variable m_event;
  std::mutex m_lock;
};

// =====================================================================================================================
// Build raytracing pipeline internally
//
// @param rtContext : Ray tracing context
// @param shaderInfo : Shader info of this pipeline
// @param unlinked : Do not provide some state to LGC, so offsets are generated as relocs
// @param [out] pipelineElfs : Output multiple Elf packages
// @param [out] shaderProps : Output multiple RayTracingShaderProperty
Result Compiler::buildRayTracingPipelineInternal(RayTracingContext &rtContext,
                                                 ArrayRef<const PipelineShaderInfo *> shaderInfo, bool unlinked,
                                                 std::vector<ElfPackage> &pipelineElfs,
                                                 std::vector<RayTracingShaderProperty> &shaderProps,
                                                 IHelperThreadProvider *helperThreadProvider) {
  unsigned passIndex = 0;
  TimerProfiler timerProfiler(rtContext.getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);
  auto pipelineInfo = reinterpret_cast<const RayTracingPipelineBuildInfo *>(rtContext.getPipelineBuildInfo());

  bool hasError = false;
  Context *mainContext = acquireContext();
  mainContext->attachPipelineContext(&rtContext);
  mainContext->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

  auto scopedReleaseContext = llvm::make_scope_exit([&]() {
    mainContext->setDiagnosticHandlerCallBack(nullptr);
    releaseContext(mainContext);
  });

  // Step 1: Set up middle-end objects and read shaders.
  LgcContext *builderContext = mainContext->getLgcContext();
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  rtContext.setPipelineState(&*pipeline, /*hasher=*/nullptr, unlinked);

  std::vector<std::unique_ptr<Module>> modules(shaderInfo.size());
  mainContext->setBuilder(builderContext->createBuilder(&*pipeline));

  mainContext->ensureGpurtLibrary();

  // Create empty modules and set target machine in each.
  for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size(); ++shaderIndex) {
    const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
    std::string moduleName;
    if (shaderInfoEntry->pModuleData) {
      moduleName = (Twine("_") + getShaderStageAbbreviation(shaderInfoEntry->entryStage) + "_" +
                    Twine(getModuleIdByIndex(shaderIndex)))
                       .str();
      moduleName[1] = std::tolower(moduleName[1]);
    } else {
      moduleName = "main";
    }
    modules[shaderIndex] = std::make_unique<Module>(moduleName, *mainContext);
    mainContext->setModuleTargetMachine(modules[shaderIndex].get());

    if (!shaderInfoEntry->pModuleData)
      continue;

    std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create(builderContext));
    lowerPassMgr->setPassIndex(&passIndex);
    SpirvLower::registerTranslationPasses(*lowerPassMgr);

    // SPIR-V translation, then dump the result.
    lowerPassMgr->addPass(SpirvLowerTranslator(shaderInfoEntry->entryStage, shaderInfoEntry));
    lowerPassMgr->addPass(SpirvLowerCfgMerges());
    lowerPassMgr->addPass(AlwaysInlinerPass());

    // Run the passes.
    bool success = runPasses(&*lowerPassMgr, modules[shaderIndex].get());
    if (!success) {
      LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
      return Result::ErrorInvalidShader;
    }
  }

  // Step 2: Set up traversal module and kernel entry
  std::vector<std::unique_ptr<Module>> newModules;
  // Record which module calls TraceRay(), except the first one (For indirect mode, it is the entry function which will
  // never call TraceRay(). For inlined mode, we don't need to care).
  std::vector<bool> moduleCallsTraceRay;
  setUseGpurt(&*pipeline);
  GpurtContext &gpurtContext = GpurtContext::get(*mainContext);

  // Can currently only support all-or-nothing indirect for various reasons, the most important one being that the
  // Vulkan driver's shader group handle construction logic assume that if any shader identifier uses a VA mapping, then
  // all of them do.
  auto indirectStageMask = rtContext.getIndirectStageMask() & ShaderStageAllRayTracingBit;
  assert(indirectStageMask == 0 || indirectStageMask == ShaderStageAllRayTracingBit);

  const bool isContinuationsMode = rtContext.isContinuationsMode();

  // TODO: Do not build launch kernel for library.
  std::unique_ptr<Module> entry = std::move(modules.back());
  modules.pop_back();
  shaderInfo = shaderInfo.drop_back();

  newModules.push_back(std::move(entry));

  for (unsigned shaderIndex = 0; shaderIndex < pipelineInfo->shaderCount; ++shaderIndex) {
    const auto *shaderInfoEntry = shaderInfo[shaderIndex];
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);
    auto shaderModule = std::move(modules[shaderIndex]);

    newModules.push_back(std::move(shaderModule));
    moduleCallsTraceRay.push_back(moduleData->usage.hasTraceRay);
  }

  bool needTraversal = rtContext.getRayTracingLibrarySummary().usesTraceRay;

  // When compiling library, we do not build traversal module, as we will always use the one from complete pipeline.
  if (rtContext.getRayTracingPipelineBuildInfo()->libraryMode == LibraryMode::Library)
    needTraversal = false;

  if (needTraversal) {
    auto fetchRayTracingFuncName = [&](Vkgc::RAYTRACING_ENTRY_FUNC attribute) -> StringRef {
      return mainContext->getPipelineContext()->getRayTracingFunctionName(attribute);
    };
    StringRef traceRayFuncName = fetchRayTracingFuncName(Vkgc::RT_ENTRY_TRACE_RAY);
    // For continuations, the entry is _cont_Traversal.
    constexpr char ContTraceRayFuncName[] = "_cont_Traversal";
    if (isContinuationsMode)
      traceRayFuncName = ContTraceRayFuncName;

    std::unique_ptr<Module> traversal = CloneModule(*gpurtContext.theModule);

    // Prepare traversal module to be compiled separately
    for (auto funcIt = traversal->begin(), funcEnd = traversal->end(); funcIt != funcEnd;) {
      Function *func = &*funcIt++;
      if (func->getName().starts_with(traceRayFuncName)) {
        // We assigned GpuRt functions weak linkage prior to linking into app modules to not confuse the entry
        // point determination mechanism. Undo that on TraceRay to make it the entry of the module.
        func->setLinkage(GlobalValue::ExternalLinkage);
        lgc::rt::setLgcRtShaderStage(func, lgc::rt::RayTracingShaderStage::Traversal);
      } else if (func->getLinkage() == GlobalValue::WeakAnyLinkage && !func->empty()) {
        // Remove functions other than TraceRay entry, for traversal module we only need that.
        func->dropAllReferences();
        func->eraseFromParent();
      }
    }

    newModules.push_back(std::move(traversal));
    moduleCallsTraceRay.push_back(false);
  }

  assert(moduleCallsTraceRay.size() == (newModules.size() - 1));

  // Step 3: Run lower passes on all modules
  for (unsigned i = 0; i < newModules.size(); i++) {
    auto module = (newModules[i].get());
    std::unique_ptr<lgc::PassManager> passMgr(lgc::PassManager::Create(builderContext));
    SpirvLower::registerLoweringPasses(*passMgr);
    LowerFlag flag = {};
    flag.isRayTracing = true;
    flag.isInternalRtShader = false;
    SpirvLower::addPasses(mainContext, ShaderStageCompute, *passMgr, timerProfiler.getTimer(TimerLower), flag);
    if (isContinuationsMode) {
      passMgr->addPass(PrepareContinuations());
    }
    bool success = runPasses(&*passMgr, module);
    if (!success) {
      LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
      return Result::ErrorInvalidShader;
    }
  }

  // Step 4: Link module if necessary
  if (indirectStageMask == 0) {
    auto &mainModule = newModules[0];
    Linker linker(*mainModule);
    for (unsigned i = 1; i < newModules.size(); ++i) {
      linker.linkInModule(std::move(newModules[i]));
    }
    std::unique_ptr<lgc::PassManager> passMgr(lgc::PassManager::Create(builderContext));
    passMgr->addPass(AlwaysInlinerPass());
    bool success = runPasses(&*passMgr, mainModule.get());
    if (!success) {
      LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
      return Result::ErrorInvalidShader;
    }
    clearNonEntryFunctions(mainModule.get(), "main");
    newModules.erase(newModules.begin() + 1, newModules.end());
  }

  rtContext.setLinked(true);
  pipelineElfs.resize(newModules.size());
  shaderProps.resize(newModules.size() - 1);

  // Take entry module, it will be handled at last.
  entry = std::move(newModules[0]);

  std::unique_ptr<Module> traversalModule;
  if (indirectStageMask != 0 && needTraversal) {
    traversalModule = std::move(newModules.back());
    newModules.pop_back();
    rtContext.getRayTracingLibrarySummary().hasTraceRayModule = true;
  }

  InternalHelperThreadProvider ourHelperThreadProvider;
  if (cl::AddRtHelpers && !helperThreadProvider)
    helperThreadProvider = &ourHelperThreadProvider;

  // Step 5: Generate ELFs
  if (helperThreadProvider) {
    std::vector<Result> results(newModules.size(), Result::Success);
    std::vector<Module *> modulePointers;
    for (const auto &module : newModules)
      modulePointers.push_back(module.get());
    HelperThreadBuildRayTracingPipelineElfPayload helperThreadPayload = {
        modulePointers, pipelineElfs, shaderProps, moduleCallsTraceRay, results, &rtContext, this, false, false};
    helperThreadProvider->SetTasks(&helperThreadBuildRayTracingPipelineElf, newModules.size(),
                                   static_cast<void *>(&helperThreadPayload));

    unsigned moduleIndex = 0;
    // Initial increase to skip entry module, it will be handled later.
    helperThreadProvider->GetNextTask(&moduleIndex);
    helperThreadProvider->TaskCompleted();

    std::vector<std::thread> workers(cl::AddRtHelpers);
    for (std::thread &worker : workers) {
      worker = std::thread([&helperThreadProvider, &helperThreadPayload] {
        helperThreadBuildRayTracingPipelineElf(helperThreadProvider, &helperThreadPayload);
      });
    }

    while (!helperThreadPayload.helperThreadJoined && helperThreadProvider->GetNextTask(&moduleIndex)) {
      // NOTE: When a helper thread joins, it will move modules from the original context into a new one. However,
      // main thread may be processing on the original context at the same time, results in out of sync situation.
      // Here we keep main thread working on the original context until helper thread joins, to reduce the cost of
      // initializing new context and copying modules. Once helper thread has joined, main thread must switch to a new
      // context.
      results[moduleIndex] =
          buildRayTracingPipelineElf(mainContext, std::move(newModules[moduleIndex]), pipelineElfs[moduleIndex],
                                     shaderProps, moduleCallsTraceRay, moduleIndex, pipeline, timerProfiler);
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

    for (std::thread &worker : workers)
      worker.join();

    for (auto res : results) {
      if (res != Result::Success)
        return Result::ErrorInvalidShader;
    }

  } else {
    for (auto [moduleIndex, module] : llvm::enumerate(newModules)) {
      // Skip entry module here, it will be handled later.
      if (moduleIndex == 0)
        continue;

      Result result = buildRayTracingPipelineElf(mainContext, std::move(module), pipelineElfs[moduleIndex], shaderProps,
                                                 moduleCallsTraceRay, moduleIndex, pipeline, timerProfiler);
      if (result != Result::Success)
        return result;
    }
  }

  // Build traversal at last after we gather all needed information.
  if (traversalModule) {
    if (isContinuationsMode)
      ContHelper::setPreservedPayloadRegisterCount(*traversalModule,
                                                   rtContext.getRayTracingLibrarySummary().maxUsedPayloadRegisterCount);

    auto rayFlagsKnownBits = rtContext.getRayFlagsKnownBits();
    lgc::gpurt::setKnownSetRayFlags(*traversalModule, rayFlagsKnownBits.One.getZExtValue());
    lgc::gpurt::setKnownUnsetRayFlags(*traversalModule, rayFlagsKnownBits.Zero.getZExtValue());

    Result result =
        buildRayTracingPipelineElf(mainContext, std::move(traversalModule), pipelineElfs[newModules.size()],
                                   shaderProps, moduleCallsTraceRay, newModules.size(), pipeline, timerProfiler);
    if (result != Result::Success)
      return result;
  }

  // Build entry module at very last.
  Result result = buildRayTracingPipelineElf(mainContext, std::move(entry), pipelineElfs[0], shaderProps,
                                             moduleCallsTraceRay, 0, pipeline, timerProfiler);
  if (result != Result::Success)
    return result;

  return hasError ? Result::ErrorInvalidShader : Result::Success;
}

// =====================================================================================================================
// Adjust raytracing pipeline ELF package
//
// @param [in/out] pipelineElf : The pipeline ELF
// @param [in] rtContext : The ray tracing context
// @param [in/out] shaderProp : The shader property
void Compiler::adjustRayTracingElf(ElfPackage *pipelineElf, RayTracingContext *rtContext,
                                   RayTracingShaderProperty &shaderProp) {
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
  auto &pipeline = document.getRoot().getMap(true)[PalAbi::CodeObjectMetadataKey::Pipelines].getArray(true)[0];
  auto &shaderFunctionSection = pipeline.getMap(true)[PalAbi::PipelineMetadataKey::ShaderFunctions].getMap(true);

  // Get the shader function
  for (auto &funcSection : shaderFunctionSection) {
    auto shaderFunctionName = funcSection.first.getString();
    auto &shaderFunction = funcSection.second.getMap(true);

    // 1. Add raytracing pipeline indirect pipeline metadata
    // The metadata is needed for RGP to correctly show different subtype of shaders.
    // Determine the shader subtype by name
    auto subtype = lgc::rt::getShaderSubtypeForRtShaderStage(
        tryGetLgcRtShaderStageFromName(shaderFunctionName).value_or(lgc::rt::RayTracingShaderStage::Count));
    shaderFunction[".shader_subtype"] = subtype;

    // 2. Apply the .internal_pipeline_hash to .api_shader_hash in .shader_functions section
    // NOTE: this is needed for RGP to recognize different shader subtype
    auto pipelineHash = pipeline.getMap(true)[PalAbi::PipelineMetadataKey::InternalPipelineHash].getArray(true);
    shaderFunction[PalAbi::ShaderMetadataKey::ApiShaderHash].getArray(true)[0] = pipelineHash[0];
    shaderFunction[PalAbi::ShaderMetadataKey::ApiShaderHash].getArray(true)[1] = pipelineHash[1];
  }

  // Write modified metadata to the pipeline ELF
  ElfNote newMetaNote = metaNote;
  std::string destBlob;
  document.writeToBlob(destBlob);
  auto newData = new uint8_t[destBlob.size()];
  memcpy(newData, destBlob.data(), destBlob.size());
  newMetaNote.hdr.descSize = destBlob.size();
  newMetaNote.data = newData;
  writer.setNote(&newMetaNote);
  writer.writeToBuffer(pipelineElf);
}

// =====================================================================================================================
// Builds hash code from compilation-options
//
// @param optionCount : Count of compilation-option strings
// @param options : An array of compilation-option strings
MetroHash::Hash Compiler::generateHashForCompileOptions(unsigned optionCount, const char *const *options) {
  // Options which needn't affect compilation results
  static StringRef IgnoredOptions[] = {cl::PipelineDumpDir.ArgStr,
                                       cl::EnablePipelineDump.ArgStr,
                                       cl::EnableOuts.ArgStr,
                                       cl::EnableErrs.ArgStr,
                                       cl::LogFileDbgs.ArgStr,
                                       cl::LogFileOuts.ArgStr,
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
      if (option.starts_with(IgnoredOptions[j])) {
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
  assert(context->getPipelineType() == PipelineType::Graphics);
  MetroHash64 fragmentHasher;
  MetroHash64 nonFragmentHasher;
  auto pipelineContext = static_cast<const GraphicsContext *>(context->getPipelineContext());
  auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
  auto pipelineOptions = pipelineContext->getPipelineOptions();

  ShaderStage preStage = ShaderStageInvalid;
  // Build hash per shader stage
  for (ShaderStage stage : gfxShaderStages()) {
    if ((stageMask & getLgcShaderStageMask(stage)) == 0)
      continue;

    auto shaderInfo = pipelineContext->getPipelineShaderInfo(stage);
    MetroHash64 hasher;

    // Update common shader info
    PipelineDumper::updateHashForPipelineShaderInfo(stage, shaderInfo, true, &hasher);
    hasher.Update(pipelineInfo->iaState.deviceIndex);

    PipelineDumper::updateHashForResourceMappingInfo(context->getResourceMapping(), context->getPipelineLayoutApiHash(),
                                                     &hasher, stage);

    // Update input/output usage (provided by middle-end caller of this callback).
    hasher.Update(stageHashes[getLgcShaderStage(stage)].data(), stageHashes[getLgcShaderStage(stage)].size());

    // Update vertex input state
    if (stage == ShaderStageVertex) {
      PipelineDumper::updateHashForVertexInputState(pipelineInfo->pVertexInput, pipelineInfo->dynamicVertexStride,
                                                    &hasher);
    }

    MetroHash::Hash hash = {};
    hasher.Finalize(hash.bytes);

    // Add per stage hash code to fragmentHasher or nonFragmentHasher per shader stage
    auto shaderHashCode = MetroHash::compact64(&hash);
    if (stage == ShaderStageFragment) {
      fragmentHasher.Update(shaderHashCode);
      const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfo->pModuleData);
      if (moduleData && moduleData->usage.useBarycentric) {
        // If fragment uses barycentrics, we still need to care about the previous stage, because the primitive type
        // might be specified there.
        if ((preStage != ShaderStageInvalid) && (preStage != ShaderStageVertex)) {
          auto preShaderInfo = pipelineContext->getPipelineShaderInfo(preStage);
          moduleData = reinterpret_cast<const ShaderModuleData *>(preShaderInfo->pModuleData);
          fragmentHasher.Update(moduleData->cacheHash);
        }
      }
    } else
      nonFragmentHasher.Update(shaderHashCode);
    preStage = stage;
  }

  // Add additional pipeline state to final hasher
  if (stageMask & getLgcShaderStageMask(ShaderStageFragment)) {
    PipelineDumper::updateHashForPipelineOptions(pipelineOptions, &fragmentHasher, true, UnlinkedStageFragment);
    PipelineDumper::updateHashForFragmentState(pipelineInfo, &fragmentHasher);
    fragmentHasher.Finalize(fragmentHash->bytes);
  }

  if (stageMask & ~getLgcShaderStageMask(ShaderStageFragment)) {
    PipelineDumper::updateHashForPipelineOptions(pipelineOptions, &nonFragmentHasher, true, UnlinkedStageVertexProcess);
    PipelineDumper::updateHashForNonFragmentState(pipelineInfo, true, &nonFragmentHasher);
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
// Convert front-end LLPC shader stage to LGC ray tracing shader stage
// Returns nullopt if not a raytracing stage.
//
// @param stage : Front-end LLPC shader stage
std::optional<lgc::rt::RayTracingShaderStage> getLgcRtShaderStage(Llpc::ShaderStage stage) {
  switch (stage) {
  case ShaderStageRayTracingRayGen:
    return lgc::rt::RayTracingShaderStage::RayGeneration;
  case ShaderStageRayTracingIntersect:
    return lgc::rt::RayTracingShaderStage::Intersection;
  case ShaderStageRayTracingAnyHit:
    return lgc::rt::RayTracingShaderStage::AnyHit;
  case ShaderStageRayTracingClosestHit:
    return lgc::rt::RayTracingShaderStage::ClosestHit;
  case ShaderStageRayTracingMiss:
    return lgc::rt::RayTracingShaderStage::Miss;
  case ShaderStageRayTracingCallable:
    return lgc::rt::RayTracingShaderStage::Callable;
  default:
    return std::nullopt;
  }
}

// =====================================================================================================================
// Convert front-end LLPC shader stage to middle-end LGC shader type
//
// @param stage : Front-end LLPC shader stage
lgc::ShaderStageEnum getLgcShaderStage(Llpc::ShaderStage stage) {
  switch (stage) {
  case ShaderStageTask:
    return lgc::ShaderStage::Task;
  case ShaderStageCompute:
    return lgc::ShaderStage::Compute;
  case ShaderStageVertex:
    return lgc::ShaderStage::Vertex;
  case ShaderStageTessControl:
    return lgc::ShaderStage::TessControl;
  case ShaderStageTessEval:
    return lgc::ShaderStage::TessEval;
  case ShaderStageGeometry:
    return lgc::ShaderStage::Geometry;
  case ShaderStageMesh:
    return lgc::ShaderStage::Mesh;
  case ShaderStageFragment:
    return lgc::ShaderStage::Fragment;
  case ShaderStageCopyShader:
    return lgc::ShaderStage::CopyShader;
  case ShaderStageRayTracingRayGen:
  case ShaderStageRayTracingIntersect:
  case ShaderStageRayTracingAnyHit:
  case ShaderStageRayTracingClosestHit:
  case ShaderStageRayTracingMiss:
  case ShaderStageRayTracingCallable:
    return lgc::ShaderStage::Compute;
  default:
    llvm_unreachable("");
    return lgc::ShaderStage::Invalid;
  }
}

// =====================================================================================================================
// Convert front-end LLPC shader stage to middle-end LGC shader stage mask
//
// @param stage : Front-end LLPC shader stage
unsigned getLgcShaderStageMask(ShaderStage stage) {
  return (1 << getLgcShaderStage(stage));
}

// =====================================================================================================================
// Convert a name to middle-end LGC RT shader stage
// Returns std::nullopt if cannot determine
//
// @param name : The name to check
std::optional<lgc::rt::RayTracingShaderStage> tryGetLgcRtShaderStageFromName(llvm::StringRef name) {
  // TODO: We should eventually get rid of using name to determine shader stage
  if (name.contains("rgen"))
    return lgc::rt::RayTracingShaderStage::RayGeneration;
  else if (name.contains("miss"))
    return lgc::rt::RayTracingShaderStage::Miss;
  else if (name.contains("ahit"))
    return lgc::rt::RayTracingShaderStage::AnyHit;
  else if (name.contains("chit"))
    return lgc::rt::RayTracingShaderStage::ClosestHit;
  else if (name.contains("sect"))
    return lgc::rt::RayTracingShaderStage::Intersection;
  else if (name.contains("call"))
    return lgc::rt::RayTracingShaderStage::Callable;
  else if (name.contains("cs"))
    return lgc::rt::RayTracingShaderStage::Traversal;

  return std::nullopt;
}

} // namespace Llpc
