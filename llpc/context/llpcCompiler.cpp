/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llpcFile.h"
#include "llpcGraphicsContext.h"
#include "llpcShaderModuleHelper.h"
#include "llpcSpirvLower.h"
#include "llpcSpirvLowerResourceCollect.h"
#include "llpcSpirvLowerTranslator.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcTimerProfiler.h"
#include "llpcUtil.h"
#include "spirvExt.h"
#include "vkgcElfReader.h"
#include "vkgcPipelineDumper.h"
#include "lgc/Builder.h"
#include "lgc/ElfLinker.h"
#include "lgc/EnumIterator.h"
#include "lgc/PassManager.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
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

// -executable-name: executable file name
static opt<std::string> ExecutableName("executable-name", desc("Executable file name"), value_desc("filename"),
                                       init("amdllpc"));

// -enable-shader-module-opt: Enable translate & lower phase in shader module build.
opt<bool> EnableShaderModuleOpt("enable-shader-module-opt",
                                cl::desc("Enable translate & lower phase in shader module build."), init(false));

// -trim-debug-info: Trim debug information in SPIR-V binary
opt<bool> TrimDebugInfo("trim-debug-info", cl::desc("Trim debug information in SPIR-V binary"), init(true));

// -enable-per-stage-cache: Enable shader cache per shader stage
opt<bool> EnablePerStageCache("enable-per-stage-cache", cl::desc("Enable shader cache per shader stage"), init(true));

// -context-reuse-limit: The maximum number of times a compiler context can be reused.
opt<int> ContextReuseLimit("context-reuse-limit",
                           cl::desc("The maximum number of times a compiler context can be reused"), init(100));

// -fatal-llvm-errors: Make all LLVM errors fatal
opt<bool> FatalLlvmErrors("fatal-llvm-errors", cl::desc("Make all LLVM errors fatal"), init(false));

// -new-pass-manager: Use LLVM's new pass manager (experimental)
opt<bool> NewPassManager("new-pass-manager", cl::desc("Use LLVM's new pass manager (experimental)"), init(false));

extern opt<bool> EnableOuts;

extern opt<bool> EnableErrs;

extern opt<std::string> LogFileDbgs;

extern opt<std::string> LogFileOuts;

} // namespace cl

} // namespace llvm

// -use-builder-recorder
static cl::opt<bool> UseBuilderRecorder("use-builder-recorder",
                                        cl::desc("Do lowering via recording and replaying LLPC builder"),
                                        cl::init(true));

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

// =====================================================================================================================
// Handler for LLVM fatal error.
//
// @param userData : An argument which will be passed to the installed error handler
// @param reason : Error reason
// @param genCrashDiag : Whether diagnostic should be generated
static void fatalErrorHandler(void *userData, const std::string &reason, bool genCrashDiag) {
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

  // Initialize passes so they can be referenced by -print-after etc.
  initializeLowerPasses(*PassRegistry::getPassRegistry());
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
    : m_optionHash(optionHash), m_gfxIp(gfxIp), m_cache(cache), m_relocatablePipelineCompilations(0)
{
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
  Result result = Result::Success;
  void *allocBuf = nullptr;
  const void *cacheData = nullptr;
  uint8_t *allocData = nullptr;
  size_t allocSize = 0;
  ShaderModuleDataEx moduleDataEx = {};
  // For trimming debug info
  uint8_t *trimmedCode = nullptr;

  ElfPackage moduleBinary;
  raw_svector_ostream moduleBinaryStream(moduleBinary);
  std::vector<ShaderEntryName> entryNames;
  SmallVector<ShaderModuleEntryData, 4> moduleEntryDatas;
  SmallVector<ShaderModuleEntry, 4> moduleEntries;
  SmallVector<FsOutInfo, 4> fsOutInfos;
  std::map<unsigned, std::vector<ResourceNodeData>> entryResourceNodeDatas; // Map entry ID and resourceNodeData

  ShaderEntryState cacheEntryState = ShaderEntryState::New;
  CacheEntryHandle hEntry = nullptr;
  Result cacheResult = Result::Unsupported;
  EntryHandle cacheEntry;
  bool allocateOnMiss = true;

  // Calculate the hash code of input data
  MetroHash::Hash hash = {};
  MetroHash64::Hash(reinterpret_cast<const uint8_t *>(shaderInfo->shaderBin.pCode), shaderInfo->shaderBin.codeSize,
                    hash.bytes);

  memcpy(moduleDataEx.common.hash, &hash, sizeof(hash));

  TimerProfiler timerProfiler(MetroHash::compact64(&hash), "LLPC ShaderModule",
                              TimerProfiler::ShaderModuleTimerEnableMask);

  bool trimDebugInfo = cl::TrimDebugInfo
      ;

  // Check the type of input shader binary
  if (Vkgc::isSpirvBinary(&shaderInfo->shaderBin)) {
    unsigned debugInfoSize = 0;

    moduleDataEx.common.binType = BinaryType::Spirv;
    if (ShaderModuleHelper::verifySpirvBinary(&shaderInfo->shaderBin) != Result::Success) {
      LLPC_ERRS("Unsupported SPIR-V instructions are found!\n");
      result = Result::Unsupported;
    }
    if (result == Result::Success) {
      ShaderModuleHelper::collectInfoFromSpirvBinary(&shaderInfo->shaderBin, &moduleDataEx.common.usage, entryNames,
                                                     &debugInfoSize);
    }
    moduleDataEx.common.binCode.codeSize = shaderInfo->shaderBin.codeSize;
    if (trimDebugInfo)
      moduleDataEx.common.binCode.codeSize -= debugInfoSize;
  } else if (ShaderModuleHelper::isLlvmBitcode(&shaderInfo->shaderBin)) {
    moduleDataEx.common.binType = BinaryType::LlvmBc;
    moduleDataEx.common.binCode = shaderInfo->shaderBin;
  } else
    result = Result::ErrorInvalidShader;

  if (moduleDataEx.common.binType == BinaryType::Spirv) {
    // Dump SPIRV binary
    if (cl::EnablePipelineDump) {
      PipelineDumper::DumpSpirvBinary(cl::PipelineDumpDir.c_str(), &shaderInfo->shaderBin, &hash);
    }

    // Trim debug info
    if (trimDebugInfo) {
      trimmedCode = new uint8_t[moduleDataEx.common.binCode.codeSize];
      ShaderModuleHelper::trimSpirvDebugInfo(&shaderInfo->shaderBin, moduleDataEx.common.binCode.codeSize, trimmedCode);
      moduleDataEx.common.binCode.pCode = trimmedCode;
    } else
      moduleDataEx.common.binCode.pCode = shaderInfo->shaderBin.pCode;

    // Calculate SPIR-V cache hash
    MetroHash::Hash cacheHash = {};
    MetroHash64::Hash(reinterpret_cast<const uint8_t *>(moduleDataEx.common.binCode.pCode),
                      moduleDataEx.common.binCode.codeSize, cacheHash.bytes);
    static_assert(sizeof(moduleDataEx.common.cacheHash) == sizeof(cacheHash), "Unexpected value!");
    memcpy(moduleDataEx.common.cacheHash, cacheHash.dwords, sizeof(cacheHash));
    HashId cacheHashId = {};
    static_assert(sizeof(HashId) == sizeof(cacheHash), "Hash size is different!");
    memcpy(cacheHashId.dwords, cacheHash.dwords, sizeof(cacheHash));

    // Do SPIR-V translate & lower if possible
    bool enableOpt = cl::EnableShaderModuleOpt;
    enableOpt = enableOpt || shaderInfo->options.enableOpt;
    enableOpt = moduleDataEx.common.usage.useSpecConstant ? false : enableOpt;

    if (enableOpt) {
      // Check internal cache for shader module build result
      // NOTE: We should not cache non-opt result, we may compile shader module multiple
      // times in async-compile mode.
      if (m_cache) {
        cacheResult = m_cache->GetEntry(cacheHashId, allocateOnMiss, &cacheEntry);
        if (cacheResult == Result::NotReady)
          cacheResult = cacheEntry.WaitForEntry();
        if (cacheResult == Result::Success) {
          cacheResult = cacheEntry.GetValueZeroCopy(&cacheData, &allocSize);
          if (cacheResult == Result::Unsupported && !cacheData) {
            cacheResult = cacheEntry.GetValue(nullptr, &allocSize);
            if (cacheResult == Result::Success && allocSize > 0) {
              cacheData = allocData = new uint8_t[allocSize];
              cacheResult = cacheEntry.GetValue(const_cast<void *>(cacheData), &allocSize);
            }
          }
        }
      } else {
        cacheEntryState = m_shaderCache->findShader(cacheHash, allocateOnMiss, &hEntry);
        if (cacheEntryState == ShaderEntryState::Ready)
          result = m_shaderCache->retrieveShader(hEntry, &cacheData, &allocSize);
      }
      if (cacheResult != Result::Success && cacheEntryState != ShaderEntryState::Ready) {
        bool hasError = false;
        Context *context = acquireContext();

        context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));
        context->setBuilder(context->getLgcContext()->createBuilder(nullptr, true));

        for (unsigned i = 0; i < entryNames.size(); ++i) {
          ShaderModuleEntry moduleEntry = {};
          ShaderModuleEntryData moduleEntryData = {};

          moduleEntryData.pShaderEntry = &moduleEntry;
          moduleEntryData.stage = entryNames[i].stage;
          moduleEntryData.pEntryName = entryNames[i].name;
          moduleEntry.entryOffset = moduleBinary.size();
          MetroHash::Hash entryNamehash = {};
          MetroHash64::Hash(reinterpret_cast<const uint8_t *>(entryNames[i].name), strlen(entryNames[i].name),
                            entryNamehash.bytes);
          memcpy(moduleEntry.entryNameHash, entryNamehash.dwords, sizeof(entryNamehash));

          // Create empty modules and set target machine in each.
          Module *module = new Module(
              (Twine("llpc") + getShaderStageName(static_cast<ShaderStage>(entryNames[i].stage))).str(), *context);

          context->setModuleTargetMachine(module);

          unsigned passIndex = 0;
          std::unique_ptr<lgc::LegacyPassManager> lowerPassMgr(lgc::LegacyPassManager::Create());
          lowerPassMgr->setPassIndex(&passIndex);

          // Set the shader stage in the Builder.
          context->getBuilder()->setShaderStage(getLgcShaderStage(static_cast<ShaderStage>(entryNames[i].stage)));

          // Start timer for translate.
          timerProfiler.addTimerStartStopPass(&*lowerPassMgr, TimerTranslate, true);

          // SPIR-V translation, then dump the result.
          PipelineShaderInfo shaderInfo = {};
          shaderInfo.pModuleData = &moduleDataEx.common;
          shaderInfo.entryStage = entryNames[i].stage;
          shaderInfo.pEntryTarget = entryNames[i].name;
          lowerPassMgr->add(createSpirvLowerTranslator(static_cast<ShaderStage>(entryNames[i].stage), &shaderInfo));
          bool collectDetailUsage =
              entryNames[i].stage == ShaderStageFragment || entryNames[i].stage == ShaderStageCompute;
          auto resCollectPass =
              static_cast<SpirvLowerResourceCollect *>(createSpirvLowerResourceCollect(collectDetailUsage));
          lowerPassMgr->add(resCollectPass);
          if (EnableOuts()) {
            lowerPassMgr->add(createPrintModulePass(
                outs(), "\n"
                        "===============================================================================\n"
                        "// LLPC SPIRV-to-LLVM translation results\n"));
          }

          // Stop timer for translate.
          timerProfiler.addTimerStartStopPass(&*lowerPassMgr, TimerTranslate, false);

          // Per-shader SPIR-V lowering passes.
          LegacySpirvLower::addPasses(context, static_cast<ShaderStage>(entryNames[i].stage), *lowerPassMgr,
                                timerProfiler.getTimer(TimerLower)
          );

          lowerPassMgr->add(createBitcodeWriterPass(moduleBinaryStream));

          // Run the passes.
          bool success = runPasses(&*lowerPassMgr, module);
          if (!success) {
            LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
            result = Result::ErrorInvalidShader;
            delete module;
            break;
          }

          moduleEntry.entrySize = moduleBinary.size() - moduleEntry.entryOffset;

          moduleEntry.passIndex = passIndex;
          if (resCollectPass->detailUsageValid()) {
            auto &resNodeDatas = resCollectPass->getResourceNodeDatas();
            moduleEntryData.resNodeDataCount = resNodeDatas.size();
            for (auto resNodeData : resNodeDatas) {
              ResourceNodeData data = {};
              data.type = resNodeData.second;
              data.set = resNodeData.first.value.set;
              data.binding = resNodeData.first.value.binding;
              data.arraySize = resNodeData.first.value.arraySize;
              entryResourceNodeDatas[i].push_back(data);
            }

            moduleEntryData.pushConstSize = resCollectPass->getPushConstSize();
            auto &fsOutInfosFromPass = resCollectPass->getFsOutInfos();
            for (auto &fsOutInfo : fsOutInfosFromPass)
              fsOutInfos.push_back(fsOutInfo);
          }
          moduleEntries.push_back(moduleEntry);
          moduleEntryDatas.push_back(moduleEntryData);
          delete module;
        }

        if (result == Result::Success) {
          if (!hasError) {
            moduleDataEx.common.binType = BinaryType::MultiLlvmBc;
            moduleDataEx.common.binCode.pCode = moduleBinary.data();
            moduleDataEx.common.binCode.codeSize = moduleBinary.size();
          } else {
            result = Result::ErrorInvalidShader;
          }
        }

        context->setDiagnosticHandler(nullptr);
      }
      moduleDataEx.extra.entryCount = entryNames.size();
    }
  }

  // Allocate memory and copy output data
  unsigned totalNodeCount = 0;
  if (result == Result::Success) {
    if (shaderInfo->pfnOutputAlloc) {
      if (cacheResult != Result::Success && cacheEntryState != ShaderEntryState::Ready) {
        for (unsigned i = 0; i < moduleDataEx.extra.entryCount; ++i)
          totalNodeCount += moduleEntryDatas[i].resNodeDataCount;

        allocSize = sizeof(ShaderModuleDataEx) + moduleDataEx.common.binCode.codeSize +
                    (moduleDataEx.extra.entryCount * (sizeof(ShaderModuleEntryData) + sizeof(ShaderModuleEntry))) +
                    totalNodeCount * sizeof(ResourceNodeData) + fsOutInfos.size() * sizeof(FsOutInfo);
      }

      allocBuf = shaderInfo->pfnOutputAlloc(shaderInfo->pInstance, shaderInfo->pUserData, allocSize);

      result = allocBuf ? Result::Success : Result::ErrorOutOfMemory;
    } else {
      // Allocator is not specified
      result = Result::ErrorInvalidPointer;
    }
  }

  if (result == Result::Success) {
    // Memory layout of pAllocBuf: ShaderModuleDataEx | ShaderModuleEntryData | ShaderModuleEntry | binCode
    //                             | Resource nodes | FsOutInfo
    ShaderModuleDataEx *moduleDataExCopy = reinterpret_cast<ShaderModuleDataEx *>(allocBuf);

    ShaderModuleEntryData *entryData = &moduleDataExCopy->extra.entryDatas[0];
    if (cacheResult != Result::Success && cacheEntryState != ShaderEntryState::Ready) {
      // Copy module data
      memcpy(moduleDataExCopy, &moduleDataEx, sizeof(moduleDataEx));
      moduleDataExCopy->common.binCode.pCode = nullptr;

      size_t entryOffset = 0, codeOffset = 0, resNodeOffset = 0, fsOutInfoOffset = 0;

      entryOffset = sizeof(ShaderModuleDataEx) + moduleDataEx.extra.entryCount * sizeof(ShaderModuleEntryData);
      codeOffset = entryOffset + moduleDataEx.extra.entryCount * sizeof(ShaderModuleEntry);
      resNodeOffset = codeOffset + moduleDataEx.common.binCode.codeSize;
      fsOutInfoOffset = resNodeOffset + totalNodeCount * sizeof(ResourceNodeData);
      moduleDataExCopy->codeOffset = codeOffset;
      moduleDataExCopy->entryOffset = entryOffset;
      moduleDataExCopy->resNodeOffset = resNodeOffset;
      moduleDataExCopy->fsOutInfoOffset = fsOutInfoOffset;
    } else
      memcpy(moduleDataExCopy, cacheData, allocSize);

    ShaderModuleEntry *entry =
        reinterpret_cast<ShaderModuleEntry *>(voidPtrInc(allocBuf, moduleDataExCopy->entryOffset));
    ResourceNodeData *resNodeData =
        reinterpret_cast<ResourceNodeData *>(voidPtrInc(allocBuf, moduleDataExCopy->resNodeOffset));
    FsOutInfo *fsOutInfo = reinterpret_cast<FsOutInfo *>(voidPtrInc(allocBuf, moduleDataExCopy->fsOutInfoOffset));
    void *code = voidPtrInc(allocBuf, moduleDataExCopy->codeOffset);

    if (cacheResult != Result::Success && cacheEntryState != ShaderEntryState::Ready) {
      // Copy entry info
      for (unsigned i = 0; i < moduleDataEx.extra.entryCount; ++i) {
        entryData[i] = moduleEntryDatas[i];
        // Set module entry pointer
        entryData[i].pShaderEntry = &entry[i];
        // Copy module entry
        memcpy(entryData[i].pShaderEntry, &moduleEntries[i], sizeof(ShaderModuleEntry));
        // Copy resourceNodeData and set resource node pointer
        memcpy(resNodeData, &entryResourceNodeDatas[i][0],
               moduleEntryDatas[i].resNodeDataCount * sizeof(ResourceNodeData));
        entryData[i].pResNodeDatas = resNodeData;
        entryData[i].resNodeDataCount = moduleEntryDatas[i].resNodeDataCount;
        resNodeData += moduleEntryDatas[i].resNodeDataCount;
      }

      // Copy binary code
      memcpy(code, moduleDataEx.common.binCode.pCode, moduleDataEx.common.binCode.codeSize);
      // Destory the temporary module code
      if (trimmedCode) {
        delete[] trimmedCode;
        trimmedCode = nullptr;
        moduleDataEx.common.binCode.pCode = nullptr;
      }

      // Copy fragment shader output variables
      moduleDataExCopy->extra.fsOutInfoCount = fsOutInfos.size();
      if (fsOutInfos.size() > 0)
        memcpy(fsOutInfo, &fsOutInfos[0], fsOutInfos.size() * sizeof(FsOutInfo));
      if (m_cache && allocateOnMiss && cacheResult == Result::NotFound)
        cacheEntry.SetValue(true, moduleDataExCopy, allocSize);
      if (cacheEntryState == ShaderEntryState::Compiling) {
        if (hEntry)
          m_shaderCache->insertShader(hEntry, moduleDataExCopy, allocSize);
      }
    } else {
      // Update the pointers
      for (unsigned i = 0; i < moduleDataEx.extra.entryCount; ++i) {
        entryData[i].pShaderEntry = &entry[i];
        entryData[i].pResNodeDatas = resNodeData;
        resNodeData += entryData[i].resNodeDataCount;
      }
    }
    moduleDataExCopy->common.binCode.pCode = code;
    moduleDataExCopy->extra.pFsOutInfos = fsOutInfo;
    shaderOut->pModuleData = &moduleDataExCopy->common;
  } else {
    if (hEntry)
      m_shaderCache->resetShader(hEntry);
  }
  delete[] allocData;

  return result;
}

// =====================================================================================================================
// Helper function for formatting raw data into a space-separated string of lowercase hex bytes.
// This assumes Little Endian byte order, e.g., {45u} --> `2d 00 00 00`.
//
// @param data : Raw data to be formatted.
// @returns : Formatted bytes, e.g., `ab c4 ef 00`.
template <typename T> static FormattedBytes formatBytesLittleEndian(ArrayRef<T> data) {
  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(data.data()), data.size() * sizeof(T));
  return format_bytes(bytes, /* FirstByteOffset = */ None, /* NumPerLine = */ 16, /* ByteGroupSize = */ 1);
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

  unsigned originalShaderStageMask = context->getPipelineContext()->getShaderStageMask();
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

    unsigned shaderStageMask = getShaderStageMaskForType(stage) & originalShaderStageMask;
    context->getPipelineContext()->setShaderStageMask(shaderStageMask);

    // Check the cache for the relocatable shader for this stage .
    MetroHash::Hash cacheHash = {};
    if (context->isGraphics()) {
      auto pipelineInfo = reinterpret_cast<const GraphicsPipelineBuildInfo *>(context->getPipelineBuildInfo());
      cacheHash = PipelineDumper::generateHashForGraphicsPipeline(pipelineInfo, true, true, stage);
    } else {
      auto pipelineInfo = reinterpret_cast<const ComputePipelineBuildInfo *>(context->getPipelineBuildInfo());
      cacheHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, true, true);
    }
    // Note that this code updates m_pipelineHash of the pipeline context. It
    // must be restored before we link the pipeline ELF at the end of this for-loop.
    context->getPipelineContext()->setHashForCacheLookUp(cacheHash);
    LLPC_OUTS("Finalized hash for " << getUnlinkedShaderStageName(stage) << " stage cache lookup: "
                                    << format_hex(context->getPipelineContext()->get128BitCacheHashCode()[0], 18) << ' '
                                    << format_hex(context->getPipelineContext()->get128BitCacheHashCode()[1], 18)
                                    << '\n');

    CacheAccessor cacheAccessor(context, cacheHash, getInternalCaches());
    if (cacheAccessor.isInCache()) {
      BinaryData elfBin = cacheAccessor.getElfFromCache();
      auto data = reinterpret_cast<const char *>(elfBin.pCode);
      elf[stage].assign(data, data + elfBin.codeSize);
      LLPC_OUTS("Cache hit for shader stage " << getUnlinkedShaderStageName(stage) << "\n");
      for (ShaderStage gfxStage : gfxShaderStages()) {
        if (shaderStageMask & shaderStageToMask(gfxStage))
          stageCacheAccesses[gfxStage] =
              cacheAccessor.hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
      }
      continue;
    }
    LLPC_OUTS("Cache miss for shader stage " << getUnlinkedShaderStageName(stage) << "\n");
    for (ShaderStage gfxStage : gfxShaderStages()) {
      if (shaderStageMask & shaderStageToMask(gfxStage))
        stageCacheAccesses[gfxStage] = CacheAccessInfo::CacheMiss;
    }

    // There was a cache miss, so we need to build the relocatable shader for
    // this stage.
    const PipelineShaderInfo *singleStageShaderInfo[ShaderStageNativeStageCount] = {nullptr, nullptr, nullptr,
                                                                                    nullptr, nullptr, nullptr};

    for (ShaderStage nativeStage : nativeShaderStages()) {
      if (shaderStageMask & shaderStageToMask(nativeStage))
        singleStageShaderInfo[nativeStage] = shaderInfo[nativeStage];
    }

    Vkgc::ElfPackage &stageElf = elf[stage];
    result = buildPipelineInternal(context, singleStageShaderInfo, /*unlinked=*/true, &stageElf, stageCacheAccesses);
    if (result != Result::Success)
      break;

    // Add the result to the cache.
    BinaryData elfBin = {stageElf.size(), stageElf.data()};
    cacheAccessor.setElfInCache(elfBin);
    LLPC_OUTS("Updating the cache for unlinked shader stage " << getUnlinkedShaderStageName(stage) << "\n");
  }
  context->getPipelineContext()->setHashForCacheLookUp(originalCacheHash);
  context->getPipelineContext()->setShaderStageMask(originalShaderStageMask);
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
      if (innerNode->type == ResourceMappingNodeType::DescriptorBufferCompact)
        // The code to handle a compact descriptor cannot be easily patched, so relocatable shaders assume there are
        // no compact descriptors.
        return true;
    }
    break;
  }
  case ResourceMappingNodeType::DescriptorResource:
  case ResourceMappingNodeType::DescriptorSampler:
  case ResourceMappingNodeType::DescriptorCombinedTexture:
  case ResourceMappingNodeType::DescriptorTexelBuffer:
  case ResourceMappingNodeType::DescriptorBufferCompact:
    // Generic descriptors in the top level are not handled by the linker.
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
  // The code to handle an immutable sampler cannot be easily patched.
  if (resourceMapping->staticDescriptorValueCount != 0)
    return true;

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
  if (!pipelineInfo->unlinked) {
    for (unsigned stage = 0; stage < shaderInfos.size(); ++stage) {
      if (stage != ShaderStageVertex && stage != ShaderStageFragment && stage != ShaderStageGeometry) {
        if (shaderInfos[stage] && shaderInfos[stage]->pModuleData)
          return false;
      } else if (stage != ShaderStageGeometry && (!shaderInfos[stage] || !shaderInfos[stage]->pModuleData)) {
        // TODO: Generate pass-through shaders when the fragment or vertex shaders are missing.
        return false;
      }
    }
  }

  // Check user data nodes for unsupported Descriptor types.
  if (hasUnrelocatableDescriptorNode(&pipelineInfo->resourceMapping))
    return false;

  if (shaderInfos[0]) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfos[0]->pModuleData);
    if (moduleData && moduleData->binType != BinaryType::Spirv)
      return false;
  }

  if (cl::RelocatableShaderElfLimit != -1) {
    if (m_relocatablePipelineCompilations >= cl::RelocatableShaderElfLimit)
      return false;
    else
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
    else
      ++m_relocatablePipelineCompilations;
  }
  return true;
}

// =====================================================================================================================
// Build pipeline internally -- common code for graphics and compute
//
// @param context : Acquired context
// @param shaderInfo : Shader info of this pipeline
// @param unlinked : Do not provide some state to LGC, so offsets are generated as relocs
// @param [out] pipelineElf : Output Elf package
// @param [out] pipelineElf : Stage cache acccess info.
Result Compiler::buildPipelineInternal(Context *context, ArrayRef<const PipelineShaderInfo *> shaderInfo, bool unlinked,
                                       ElfPackage *pipelineElf,
                                       llvm::MutableArrayRef<CacheAccessInfo> stageCacheAccesses) {
  Result result = Result::Success;
  unsigned passIndex = 0;
  const PipelineShaderInfo *fragmentShaderInfo = nullptr;
  TimerProfiler timerProfiler(context->getPipelineHashCode(), "LLPC", TimerProfiler::PipelineTimerEnableMask);
  bool buildingRelocatableElf = context->getPipelineContext()->isUnlinked();

  bool hasError = false;
  context->setDiagnosticHandler(std::make_unique<LlpcDiagnosticHandler>(&hasError));

  // Set a couple of pipeline options for front-end use.
  // TODO: The front-end should not be using pipeline options.
  context->setScalarBlockLayout(context->getPipelineContext()->getPipelineOptions()->scalarBlockLayout);
  context->setRobustBufferAccess(context->getPipelineContext()->getPipelineOptions()->robustBufferAccess);

  // Set up middle-end objects.
  LgcContext *builderContext = context->getLgcContext();
  std::unique_ptr<Pipeline> pipeline(builderContext->createPipeline());
  context->getPipelineContext()->setPipelineState(&*pipeline, unlinked);
  context->setBuilder(builderContext->createBuilder(&*pipeline, UseBuilderRecorder));

  std::unique_ptr<Module> pipelineModule;

  // NOTE: If input is LLVM IR, read it now. There is now only ever one IR module representing the
  // whole pipeline.
  const PipelineShaderInfo *shaderInfoEntry = shaderInfo[0] ? shaderInfo[0] : shaderInfo.back();
  if (shaderInfoEntry) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(shaderInfoEntry->pModuleData);
    if (moduleData && moduleData->binType == BinaryType::LlvmBc)
      pipelineModule.reset(context->loadLibary(&moduleData->binCode).release());
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

      const ShaderModuleDataEx *moduleDataEx =
          reinterpret_cast<const ShaderModuleDataEx *>(shaderInfoEntry->pModuleData);

      Module *module = nullptr;
      if (moduleDataEx->common.binType == BinaryType::MultiLlvmBc) {
        timerProfiler.startStopTimer(TimerLoadBc, true);

        MetroHash::Hash entryNameHash = {};

        assert(shaderInfoEntry->pEntryTarget);
        MetroHash64::Hash(reinterpret_cast<const uint8_t *>(shaderInfoEntry->pEntryTarget),
                          strlen(shaderInfoEntry->pEntryTarget), entryNameHash.bytes);

        BinaryData binCode = {};
        for (unsigned i = 0; i < moduleDataEx->extra.entryCount; ++i) {
          auto entryData = &moduleDataEx->extra.entryDatas[i];
          auto shaderEntry = reinterpret_cast<ShaderModuleEntry *>(entryData->pShaderEntry);
          if (entryData->stage == shaderInfoEntry->entryStage &&
              memcmp(shaderEntry->entryNameHash, &entryNameHash, sizeof(MetroHash::Hash)) == 0) {
            // LLVM bitcode
            binCode.codeSize = shaderEntry->entrySize;
            binCode.pCode = voidPtrInc(moduleDataEx->common.binCode.pCode, shaderEntry->entryOffset);
            break;
          }
        }

        if (binCode.codeSize > 0) {
          module = context->loadLibary(&binCode).release();
          stageSkipMask |= (1 << shaderIndex);
        } else
          result = Result::ErrorInvalidShader;

        timerProfiler.startStopTimer(TimerLoadBc, false);
      } else {
        module = new Module((Twine("llpc") + getShaderStageName(shaderInfoEntry->entryStage)).str() +
                                std::to_string(getModuleIdByIndex(shaderIndex)),
                            *context);
      }

      modules[shaderIndex] = module;
      context->setModuleTargetMachine(module);
    }

    for (unsigned shaderIndex = 0; shaderIndex < shaderInfo.size() && result == Result::Success; ++shaderIndex) {
      const PipelineShaderInfo *shaderInfoEntry = shaderInfo[shaderIndex];
      ShaderStage entryStage = shaderInfoEntry ? shaderInfoEntry->entryStage : ShaderStageInvalid;

      if (entryStage == ShaderStageFragment)
        fragmentShaderInfo = shaderInfoEntry;
      if (!shaderInfoEntry || !shaderInfoEntry->pModuleData || (stageSkipMask & shaderStageToMask(entryStage)))
        continue;

      // Set the shader stage in the Builder.
      context->getBuilder()->setShaderStage(getLgcShaderStage(entryStage));

      if (cl::NewPassManager) {
        std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create());
        lowerPassMgr->setPassIndex(&passIndex);

        // Start timer for translate.
        timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, true);

        // SPIR-V translation, then dump the result.
        lowerPassMgr->addPass(SpirvLowerTranslator(entryStage, shaderInfoEntry));
        if (EnableOuts()) {
          lowerPassMgr->addPass(PrintModulePass(
              outs(), "\n"
                      "===============================================================================\n"
                      "// LLPC SPIRV-to-LLVM translation results\n"));
        }

        // Stop timer for translate.
        timerProfiler.addTimerStartStopPass(*lowerPassMgr, TimerTranslate, false);

        bool success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
        if (!success) {
          LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
          result = Result::ErrorInvalidShader;
        }
      } else {
        std::unique_ptr<lgc::LegacyPassManager> lowerPassMgr(lgc::LegacyPassManager::Create());
        lowerPassMgr->setPassIndex(&passIndex);

        // Start timer for translate.
        timerProfiler.addTimerStartStopPass(&*lowerPassMgr, TimerTranslate, true);

        // SPIR-V translation, then dump the result.
        lowerPassMgr->add(createSpirvLowerTranslator(entryStage, shaderInfoEntry));
        if (EnableOuts()) {
          lowerPassMgr->add(createPrintModulePass(
              outs(), "\n"
                      "===============================================================================\n"
                      "// LLPC SPIRV-to-LLVM translation results\n"));
        }
        // Stop timer for translate.
        timerProfiler.addTimerStartStopPass(&*lowerPassMgr, TimerTranslate, false);

        // Run the passes.
        bool success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
        if (!success) {
          LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
          result = Result::ErrorInvalidShader;
        }
      }
    }
    SmallVector<Module *, 5> modulesToLink;
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

      context->getBuilder()->setShaderStage(getLgcShaderStage(entryStage));
      bool success;
      if (cl::NewPassManager) {
        std::unique_ptr<lgc::PassManager> lowerPassMgr(lgc::PassManager::Create());
        lowerPassMgr->setPassIndex(&passIndex);

        SpirvLower::addPasses(context, entryStage, *lowerPassMgr, timerProfiler.getTimer(TimerLower));
        // Run the passes.
        success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
      } else {
        std::unique_ptr<lgc::LegacyPassManager> lowerPassMgr(lgc::LegacyPassManager::Create());
        lowerPassMgr->setPassIndex(&passIndex);

        LegacySpirvLower::addPasses(context, entryStage, *lowerPassMgr, timerProfiler.getTimer(TimerLower)
      );
        // Run the passes.
        success = runPasses(&*lowerPassMgr, modules[shaderIndex]);
      }
      if (!success) {
        LLPC_ERRS("Failed to translate SPIR-V or run per-shader passes\n");
        result = Result::ErrorInvalidShader;
      }

      // Add the shader module to the list for the pipeline.
      modulesToLink.push_back(modules[shaderIndex]);
    }

    // Link the shader modules into a single pipeline module.
    pipelineModule.reset(pipeline->irLink(modulesToLink, context->getPipelineContext()->isUnlinked()
                                                             ? PipelineLink::Unlinked
                                                             : PipelineLink::WholePipeline));
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

  // Only enable per stage cache for full graphic pipeline
  bool checkPerStageCache =
      cl::EnablePerStageCache && context->isGraphics() && !buildingRelocatableElf &&
      (context->getShaderStageMask() & (shaderStageToMask(ShaderStageVertex) | shaderStageToMask(ShaderStageFragment)));
  if (!checkPerStageCache)
    checkShaderCacheFunc = nullptr;

  // Generate pipeline.
  raw_svector_ostream elfStream(*pipelineElf);

  if (result == Result::Success) {
    result = Result::ErrorInvalidShader;
#if LLPC_ENABLE_EXCEPTION
    try
#endif
    {
      Timer *timers[] = {
          timerProfiler.getTimer(TimerPatch),
          timerProfiler.getTimer(TimerOpt),
          timerProfiler.getTimer(TimerCodeGen),
      };

      pipeline->generate(std::move(pipelineModule), elfStream, checkShaderCacheFunc, timers);
      result = Result::Success;
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
      (context->getShaderStageMask() & shaderStageToMask(ShaderStageFragment)))
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
// @param stageMask : Shader stage mask
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

  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    m_fragmentCacheAccessor.emplace(m_context, fragmentHash, m_compiler->getInternalCaches());
    if (m_fragmentCacheAccessor->isInCache()) {
      // Remove fragment shader stages.
      stagesLeftToCompile &= ~shaderStageToMask(ShaderStageFragment);
      stageCacheAccesses[ShaderStageFragment] =
          m_fragmentCacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit : CacheAccessInfo::CacheHit;
    } else {
      stageCacheAccesses[ShaderStageFragment] = CacheAccessInfo::CacheMiss;
    }
  }

  if (stageMask & ~shaderStageToMask(ShaderStageFragment)) {
    auto accessInfo = CacheAccessInfo::CacheNotChecked;
    m_nonFragmentCacheAccessor.emplace(m_context, nonFragmentHash, m_compiler->getInternalCaches());
    if (m_nonFragmentCacheAccessor->isInCache()) {
      // Remove non-fragment shader stages.
      stagesLeftToCompile &= shaderStageToMask(ShaderStageFragment);
      accessInfo = m_nonFragmentCacheAccessor->hitInternalCache() ? CacheAccessInfo::InternalCacheHit
                                                                  : CacheAccessInfo::CacheHit;
    } else {
      accessInfo = CacheAccessInfo::CacheMiss;
    }

    for (ShaderStage stage : gfxShaderStages())
      if (stage != ShaderStageFragment && (shaderStageToMask(stage) & stageMask))
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

    // Merge and store the result in pPipelineElf
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
    result = buildPipelineInternal(context, shaderInfo, /*unlinked=*/false, pipelineElf, stageCacheAccesses);
  }
  releaseContext(context);
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

  SmallVector<const PipelineShaderInfo *, 6> shaderInfo = {
      &pipelineInfo->vs, &pipelineInfo->tcs, &pipelineInfo->tes, &pipelineInfo->gs, &pipelineInfo->fs,
  };

  const bool relocatableElfRequested = pipelineInfo->options.enableRelocatableShaderElf || cl::UseRelocatableShaderElf;
  const bool buildingRelocatableElf =
      relocatableElfRequested && canUseRelocatableGraphicsShaderElf(shaderInfo, pipelineInfo);

  for (ShaderStage stage : gfxShaderStages()) {
    result = validatePipelineShaderInfo(shaderInfo[stage]);
    if (result != Result::Success)
      break;
  }

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
    if (relocatableElfRequested && !buildingRelocatableElf) {
      LLPC_OUTS("\nWarning: Relocatable shader compilation requested but not possible. "
                << "Falling back to whole-pipeline compilation.\n");
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

  Optional<CacheAccessor> cacheAccessor;
  if (!buildingRelocatableElf) {
    cacheAccessor.emplace(pipelineInfo, cacheHash, getInternalCaches());
  }

  ElfPackage candidateElf;

  if (!cacheAccessor || !cacheAccessor->isInCache()) {

    GraphicsContext graphicsContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
    result = buildGraphicsPipelineInternal(&graphicsContext, shaderInfo, buildingRelocatableElf, &candidateElf,
                                           pipelineOut->stageCacheAccesses);

    if (result == Result::Success) {
      elfBin.codeSize = candidateElf.size();
      elfBin.pCode = candidateElf.data();
    }

    if (cacheAccessor && pipelineOut->pipelineCacheAccess == CacheAccessInfo::CacheNotChecked)
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::CacheMiss;
  } else {
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
      nullptr, nullptr, nullptr, nullptr, nullptr, &pipelineInfo->cs,
  };
  Result result = Result::ErrorUnavailable;
  CacheAccessInfo stageCacheAccesses[ShaderStageCount] = {};
  if (buildingRelocatableElf) {
    result = buildPipelineWithRelocatableElf(context, shadersInfo, pipelineElf, stageCacheAccesses);
    *stageCacheAccess = stageCacheAccesses[ShaderStageCompute];
  }

  if (result != Result::Success) {
    result = buildPipelineInternal(context, shadersInfo, /*unlinked=*/false, pipelineElf, stageCacheAccesses);
  }
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
  const bool buildingRelocatableElf = relocatableElfRequested && canUseRelocatableComputeShaderElf(pipelineInfo);

  Result result = validatePipelineShaderInfo(&pipelineInfo->cs);

  MetroHash::Hash cacheHash = {};
  MetroHash::Hash pipelineHash = {};
  cacheHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, true, buildingRelocatableElf);
  pipelineHash = PipelineDumper::generateHashForComputePipeline(pipelineInfo, false, buildingRelocatableElf);

  if (result == Result::Success && EnableOuts()) {
    const ShaderModuleData *moduleData = reinterpret_cast<const ShaderModuleData *>(pipelineInfo->cs.pModuleData);
    auto moduleHash = reinterpret_cast<const MetroHash::Hash *>(&moduleData->hash[0]);
    LLPC_OUTS("\n===============================================================================\n");
    LLPC_OUTS("// LLPC calculated hash results (compute pipeline)\n\n");
    LLPC_OUTS("PIPE : " << format("0x%016" PRIX64, MetroHash::compact64(&pipelineHash)) << "\n");
    LLPC_OUTS(format("%-4s : ", getShaderStageAbbreviation(ShaderStageCompute, true))
              << format("0x%016" PRIX64, MetroHash::compact64(moduleHash)) << "\n");
    if (relocatableElfRequested && !buildingRelocatableElf) {
      LLPC_OUTS("\nWarning: Relocatable shader compilation requested but not possible. "
                << "Falling back to whole-pipeline compilation.\n");
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

  std::unique_ptr<CacheAccessor> cacheAccessor;

  if (!buildingRelocatableElf) {
    cacheAccessor = std::make_unique<CacheAccessor>(pipelineInfo, cacheHash, getInternalCaches());
  }

  ElfPackage candidateElf;
  if (!cacheAccessor || !cacheAccessor->isInCache()) {
    ComputeContext computeContext(m_gfxIp, pipelineInfo, &pipelineHash, &cacheHash);
    result = buildComputePipelineInternal(&computeContext, pipelineInfo, buildingRelocatableElf, &candidateElf,
                                          &pipelineOut->stageCacheAccess);

    if (result == Result::Success) {
      elfBin.codeSize = candidateElf.size();
      elfBin.pCode = candidateElf.data();
    }
    if (cacheAccessor && pipelineOut->pipelineCacheAccess == CacheAccessInfo::CacheNotChecked)
      pipelineOut->pipelineCacheAccess = CacheAccessInfo::CacheMiss;
  } else {
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
// @returns : Result::Success if creation succeeedds, error status otherwise.
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
    m_shaderCache->Merge(1, const_cast<const IShaderCache **>(ppShaderCache));
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
// Run legacy pass manager's passes on a module, catching any LLVM fatal error and returning a success indication
//
// @param passMgr : Pass manager
// @param [in/out] module : Module
bool Compiler::runPasses(lgc::LegacyPassManager *passMgr, Module *module) const {
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
// @param stageMask : Shader stage mask
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
    if ((stageMask & shaderStageToMask(stage)) == 0)
      continue;

    auto shaderInfo = context->getPipelineShaderInfo(stage);
    MetroHash64 hasher;

    // Update common shader info
    PipelineDumper::updateHashForPipelineShaderInfo(stage, shaderInfo, true, &hasher, false);
    hasher.Update(pipelineInfo->iaState.deviceIndex);

    PipelineDumper::updateHashForResourceMappingInfo(context->getResourceMapping(), &hasher, false);

    // Update input/output usage (provided by middle-end caller of this callback).
    hasher.Update(stageHashes[stage].data(), stageHashes[stage].size());

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

  // Add addtional pipeline state to final hasher
  if (stageMask & shaderStageToMask(ShaderStageFragment)) {
    // Add pipeline options to fragment hash
    fragmentHasher.Update(pipelineOptions->includeDisassembly);
    fragmentHasher.Update(pipelineOptions->scalarBlockLayout);
    fragmentHasher.Update(pipelineOptions->reconfigWorkgroupLayout);
    fragmentHasher.Update(pipelineOptions->includeIr);
    fragmentHasher.Update(pipelineOptions->robustBufferAccess);
    fragmentHasher.Update(pipelineOptions->extendedRobustness.robustBufferAccess);
    fragmentHasher.Update(pipelineOptions->extendedRobustness.robustImageAccess);
    fragmentHasher.Update(pipelineOptions->extendedRobustness.nullDescriptor);
    PipelineDumper::updateHashForFragmentState(pipelineInfo, &fragmentHasher, false);
    fragmentHasher.Finalize(fragmentHash->bytes);
  }

  if (stageMask & ~shaderStageToMask(ShaderStageFragment)) {
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
  context->getPipelineContext()->setPipelineState(&*pipeline, /*unlinked=*/false);

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
lgc::ShaderStage getLgcShaderStage(Llpc::ShaderStage stage) {
  switch (stage) {
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

} // namespace Llpc
