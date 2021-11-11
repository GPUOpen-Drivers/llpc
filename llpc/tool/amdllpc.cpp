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
 * @file  amdllpc.cpp
 * @brief LLPC source file: contains implementation of LLPC standalone tool.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "llpcAutoLayout.h"
#include "llpcCompilationUtils.h"
#include "llpcInputUtils.h"
#include "lgc/LgcContext.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#else
#ifdef WIN_OS
#include <io.h>
#include <signal.h>
#endif
#endif

// NOTE: To enable VLD, please add option BUILD_WIN_VLD=1 in build option.To run amdllpc with VLD enabled,
// please copy vld.ini and all files in.\winVisualMemDetector\bin\Win64 to current directory of amdllpc.
#ifdef BUILD_WIN_VLD
#include "vld.h"
#endif

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcFile.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcUtil.h"
#include "spvgen.h"
#include "vfx.h"
#include "vkgcElfReader.h"
#include <cstdlib> // getenv, EXIT_FAILURE, EXIT_SUCCESS

#define DEBUG_TYPE "amd-llpc"

using namespace llvm;
using namespace Llpc;
using namespace Llpc::StandaloneCompiler;
using namespace Vkgc;

namespace {
// Represents options of the amdllpc standalone tool.

// -gfxip: graphics IP version
cl::opt<std::string> GfxIp("gfxip", cl::desc("Graphics IP version"), cl::value_desc("major.minor.step"),
                           cl::init("8.0.2"));

// The GFXIP version parsed out of the -gfxip option before normal option processing occurs.
GfxIpVersion ParsedGfxIp = {8, 0, 2};

// Input sources
cl::list<std::string> InFiles(cl::Positional, cl::OneOrMore, cl::ValueRequired,
                              cl::desc("<source>...\n"
                                       "Type of input file is determined by its filename extension:\n"
                                       "  .spv      SPIR-V binary\n"
                                       "  .spvasm   SPIR-V assembly text\n"
                                       "  .vert     GLSL vertex shader\n"
                                       "  .tesc     GLSL tessellation control shader\n"
                                       "  .tese     GLSL tessellation evaluation shader\n"
                                       "  .geom     GLSL geometry shader\n"
                                       "  .frag     GLSL fragment shader\n"
                                       "  .comp     GLSL compute shader\n"
                                       "  .pipe     Pipeline info file\n"
                                       "  .ll       LLVM IR assembly text"));

// -o: output
cl::opt<std::string> OutFile("o", cl::desc("Output file"), cl::value_desc("filename (\"-\" for stdout)"));

// -l: link pipeline
cl::opt<bool> ToLink("l", cl::desc("Link pipeline and generate ISA codes"), cl::init(true));

// -auto-layout-desc: automatically create descriptor layout based on resource usages
cl::opt<bool> AutoLayoutDesc("auto-layout-desc",
                             cl::desc("Automatically create descriptor layout based on resource usages"),
                             cl::init(false));

// -unlinked : build an "unlinked" shader/part-pipeline ELF that needs a further link step
cl::opt<bool> Unlinked("unlinked", cl::desc("Build \"unlinked\" shader/part-pipeline ELF"), cl::init(false));

// -validate-spirv: validate input SPIR-V binary or text
cl::opt<bool> ValidateSpirv("validate-spirv", cl::desc("Validate input SPIR-V binary or text (default: true)"),
                            cl::init(true));

// -entry-target: name string of entry target (for multiple entry-points)
cl::opt<std::string> EntryTarget("entry-target", cl::desc("Name string of entry target"), cl::value_desc("entryname"),
                                 cl::init(""));

// -ignore-color-attachment-formats: ignore color attachment formats
cl::opt<bool> IgnoreColorAttachmentFormats("ignore-color-attachment-formats",
                                           cl::desc("Ignore color attachment formats"), cl::init(false));

// -enable-ngg: enable NGG mode
cl::opt<bool> EnableNgg("enable-ngg", cl::desc("Enable implicit primitive shader (NGG) mode"), cl::init(true));

// -enable-gs-use: enable NGG use on geometry shader
cl::opt<bool> NggEnableGsUse("ngg-enable-gs-use", cl::desc("Enable NGG use on geometry shader"), cl::init(false));

// -ngg-force-culling-mode: force NGG to run in culling mode
cl::opt<bool> NggForceCullingMode("ngg-force-culling-mode", cl::desc("Force NGG to run in culling mode"),
                                  cl::init(false));

// -ngg-compact-mode: NGG compaction mode (NGG)
cl::opt<unsigned> NggCompactionMode("ngg-compaction-mode",
                                    cl::desc("Compaction mode after culling operations (NGG):\n"
                                             "0: Compaction is disabled\n"
                                             "1: Compaction is based on vertices"),
                                    cl::value_desc("mode"), cl::init(static_cast<unsigned>(NggCompactVertices)));

// -ngg-enable-vertex-reuse: enable optimization to cull duplicate vertices (NGG)
cl::opt<bool> NggEnableVertexReuse("ngg-enable-vertex-reuse",
                                   cl::desc("Enable optimization to cull duplicate vertices (NGG)"), cl::init(false));

// -ngg-enable-backface-culling: enable culling of primitives that don't meet facing criteria (NGG)
cl::opt<bool> NggEnableBackfaceCulling("ngg-enable-backface-culling",
                                       cl::desc("Enable culling of primitives that don't meet facing criteria (NGG)"),
                                       cl::init(false));

// -ngg-enable-frustum-culling: enable discarding of primitives outside of view frustum (NGG)
cl::opt<bool> NggEnableFrustumCulling("ngg-enable-frustum-culling",
                                      cl::desc("Enable discarding of primitives outside of view frustum (NGG)"),
                                      cl::init(false));

// -ngg-enable-box-filter-culling: enable simpler frustum culler that is less accurate (NGG)
cl::opt<bool> NggEnableBoxFilterCulling("ngg-enable-box-filter-culling",
                                        cl::desc("Enable simpler frustum culler that is less accurate (NGG)"),
                                        cl::init(false));

// -ngg-enable-sphere-culling: enable frustum culling based on a sphere (NGG)
cl::opt<bool> NggEnableSphereCulling("ngg-enable-sphere-culling",
                                     cl::desc("Enable frustum culling based on a sphere (NGG)"), cl::init(false));

// -ngg-enable-small-prim-filter: enable trivial sub-sample primitive culling (NGG)
cl::opt<bool> NggEnableSmallPrimFilter("ngg-enable-small-prim-filter",
                                       cl::desc("Enable trivial sub-sample primitive culling (NGG)"), cl::init(false));

// -ngg-cull-distance-culling: enable culling when "cull distance" exports are present (NGG)
cl::opt<bool> NggEnableCullDistanceCulling("ngg-enable-cull-distance-culling",
                                           cl::desc("Enable culling when \"cull distance\" exports are present (NGG)"),
                                           cl::init(false));

// -ngg-backface-exponent: control backface culling algorithm (NGG, 1 ~ UINT32_MAX, 0 disables it)
cl::opt<unsigned> NggBackfaceExponent("ngg-backface-exponent", cl::desc("Control backface culling algorithm (NGG)"),
                                      cl::value_desc("exp"), cl::init(0));

// -ngg-subgroup-sizing: NGG sub-group sizing type (NGG)
cl::opt<unsigned> NggSubgroupSizing(
    "ngg-subgroup-sizing",
    cl::desc("NGG sub-group sizing type (NGG):\n"
             "0: Sub-group size is allocated as optimally determined\n"
             "1: Sub-group size is allocated to the maximum allowable size\n"
             "2: Sub-group size is allocated as to allow half of the maximum allowable size\n"
             "3: Sub-group size is optimized for vertex thread utilization\n"
             "4: Sub-group size is optimized for primitive thread utilization\n"
             "5: Sub-group size is allocated based on explicitly-specified vertsPerSubgroup and primsPerSubgroup"),
    cl::value_desc("sizing"), cl::init(static_cast<unsigned>(NggSubgroupSizingType::Auto)));

// -ngg-prims-per-subgroup: preferred numberof GS primitives to pack into a primitive shader sub-group (NGG)
cl::opt<unsigned>
    NggPrimsPerSubgroup("ngg-prims-per-subgroup",
                        cl::desc("Preferred numberof GS primitives to pack into a primitive shader sub-group (NGG)"),
                        cl::value_desc("prims"), cl::init(256));

// -ngg-verts-per-subgroup: preferred number of vertices consumed by a primitive shader sub-group (NGG)
cl::opt<unsigned>
    NggVertsPerSubgroup("ngg-verts-per-subgroup",
                        cl::desc("Preferred number of vertices consumed by a primitive shader sub-group (NGG)"),
                        cl::value_desc("verts"), cl::init(256));

// -spvgen-dir: load SPVGEN from specified directory
cl::opt<std::string> SpvGenDir("spvgen-dir", cl::desc("Directory to load SPVGEN library from"));

cl::opt<bool> RobustBufferAccess("robust-buffer-access", cl::desc("Validate if the index is out of bounds"),
                                 cl::init(false));

cl::opt<bool> ScalarBlockLayout("scalar-block-layout", cl::desc("Allows scalar block layout of types"),
                                cl::init(false));

cl::opt<bool> EnableRelocatableShaderElf("enable-relocatable-shader-elf",
                                         cl::desc("Compile pipelines using relocatable shader elf"), cl::init(false));

// -enable-scratch-bounds-checks: insert scratch access bounds checks on loads and stores
cl::opt<bool> EnableScratchAccessBoundsChecks("enable-scratch-bounds-checks",
                                              cl::desc("Insert scratch access bounds checks on loads and stores"),
                                              cl::init(false));

// -filter-pipeline-dump-by-type: filter which kinds of pipeline should be disabled.
cl::opt<unsigned> FilterPipelineDumpByType("filter-pipeline-dump-by-type",
                                           cl::desc("Filter which types of pipeline dump are disabled\n"
                                                    "0x00 - Always enable pipeline logging\n"
                                                    "0x01 - Disable logging for CS pipelines\n"
                                                    "0x02 - Disable logging for NGG pipelines\n"
                                                    "0x04 - Disable logging for GS pipelines\n"
                                                    "0x08 - Disable logging for TS pipelines\n"
                                                    "0x10 - Disable logging for VS-PS pipelines"),
                                           cl::init(0));

// -filter-pipeline-dump-by-hash: only dump the pipeline whose computed hash is equal to the specified (if non-zero).
cl::opt<uint64_t> FilterPipelineDumpByHash(
    "filter-pipeline-dump-by-hash",
    cl::desc("Only dump the pipeline whose computed hash is equal to the specified (if non-zero)"), cl::init(0));

//-dump-duplicate-pipelines: dump duplicated pipeline, attaching a numeric suffix
cl::opt<bool> DumpDuplicatePipelines(
    "dump-duplicate-pipelines",
    cl::desc("If TRUE, duplicate pipelines will be dumped to a file with a numeric suffix attached"), cl::init(false));

#ifdef WIN_OS
// -assert-to-msgbox: pop message box when an assert is hit, only valid in Windows
cl::opt<bool> AssertToMsgBox("assert-to-msgbox", cl::desc("Pop message box when assert is hit"));
#endif

} // namespace

namespace llvm {
namespace cl {

extern opt<bool> EnablePipelineDump;
extern opt<std::string> PipelineDumpDir;
extern opt<bool> EnableTimerProfile;
extern opt<bool> BuildShaderCache;

} // namespace cl
} // namespace llvm

// =====================================================================================================================
// Performs initialization work for LLPC standalone tool.
//
// @param argc : Count of arguments
// @param argv : List of arguments
// @param [out] compiler : Created LLPC compiler object
// @returns : Result::Success on success, other status codes on failure
static Result init(int argc, char *argv[], ICompiler *&compiler) {
  // Before we get to LLVM command-line option parsing, we need to find the -gfxip option value.
  for (int i = 1; i != argc; ++i) {
    StringRef arg = argv[i];
    if (arg.startswith("--gfxip"))
      arg = arg.drop_front(1);
    if (!arg.startswith("-gfxip"))
      continue;
    StringRef gfxipStr;
    arg = arg.slice(strlen("-gfxip"), StringRef::npos);
    if (arg.empty() && i + 1 != argc)
      gfxipStr = argv[i + 1];
    else if (arg[0] == '=')
      gfxipStr = arg.slice(1, StringRef::npos);
    else
      continue;
    if (!gfxipStr.consumeInteger(10, ParsedGfxIp.major)) {
      ParsedGfxIp.minor = 0;
      ParsedGfxIp.stepping = 0;
      if (gfxipStr.startswith(".")) {
        gfxipStr = gfxipStr.slice(1, StringRef::npos);
        if (!gfxipStr.consumeInteger(10, ParsedGfxIp.minor) && gfxipStr.startswith(".")) {
          gfxipStr = gfxipStr.slice(1, StringRef::npos);
          gfxipStr.consumeInteger(10, ParsedGfxIp.stepping);
        }
      }
    }
    break;
  }

  // Change defaults of NGG options according to GFX IP
  if (ParsedGfxIp >= GfxIpVersion{10, 3}) {
    // For GFX10.3+, we always prefer to enable NGG. Backface culling and small primitive filter are enabled as
    // well. Also, the compaction mode is set to compactionless.
    EnableNgg.setValue(true);
    NggCompactionMode.setValue(static_cast<unsigned>(NggCompactDisable));
    NggEnableBackfaceCulling.setValue(true);
    NggEnableSmallPrimFilter.setValue(true);
  }

  // Provide a default for -shader-cache-file-dir, as long as the environment variables below are
  // not set.
  // TODO: Was this code intended to set the default of -shader-cache-file-dir in the case that it
  // does find an environment variable setting? It did not, so I have preserved that behavior.
  // Steps:
  //   1. Find AMD_SHADER_DISK_CACHE_PATH to keep backward compatibility.
  const char *envString = getenv("AMD_SHADER_DISK_CACHE_PATH");

#ifdef WIN_OS
  //   2. Find LOCALAPPDATA.
  if (!envString)
    envString = getenv("LOCALAPPDATA");
#else
  char shaderCacheFileRootDir[PathBufferLen];

  //   2. Find XDG_CACHE_HOME.
  //   3. If AMD_SHADER_DISK_CACHE_PATH and XDG_CACHE_HOME both not set,
  //      use "$HOME/.cache".
  if (!envString)
    envString = getenv("XDG_CACHE_HOME");

  if (!envString) {
    envString = getenv("HOME");
    if (envString) {
      snprintf(shaderCacheFileRootDir, sizeof(shaderCacheFileRootDir), "%s/.cache", envString);
      envString = &shaderCacheFileRootDir[0];
    }
  }
#endif
  if (!envString) {
    auto optIterator = cl::getRegisteredOptions().find("shader-cache-file-dir");
    assert(optIterator != cl::getRegisteredOptions().end());
    cl::Option *opt = optIterator->second;
    *static_cast<cl::opt<std::string> *>(opt) = ".";
  }

  // Check to see that the ParsedGfxIp is valid
  std::string gfxIpName = lgc::LgcContext::getGpuNameString(ParsedGfxIp.major, ParsedGfxIp.minor, ParsedGfxIp.stepping);
  if (!lgc::LgcContext::isGpuNameValid(gfxIpName)) {
    LLPC_ERRS("Invalid gfxip: " << gfxIpName << "\n");
    return Result::Unsupported;
  }

  Result result = ICompiler::Create(ParsedGfxIp, argc, argv, &compiler);
  if (result != Result::Success)
    return result;

  if (SpvGenDir != "" && !InitSpvGen(SpvGenDir.c_str())) {
    // -spvgen-dir option: preload spvgen from the given directory
    LLPC_ERRS("Failed to load SPVGEN from specified directory\n");
    return Result::ErrorUnavailable;
  }

  return Result::Success;
}

// =====================================================================================================================
// Performs per-pipeline initialization work for LLPC standalone tool.
//
// @param [out] compileInfo : Compilation info of LLPC standalone tool
// @returns : Result::Success on success, other status codes on failure
static Result initCompileInfo(CompileInfo *compileInfo) {
  compileInfo->gfxIp = ParsedGfxIp;
  compileInfo->entryTarget = EntryTarget;
  compileInfo->relocatableShaderElf = EnableRelocatableShaderElf;
  compileInfo->autoLayoutDesc = AutoLayoutDesc;
  compileInfo->robustBufferAccess = RobustBufferAccess;
  compileInfo->scalarBlockLayout = ScalarBlockLayout;
  compileInfo->scratchAccessBoundsChecks = EnableScratchAccessBoundsChecks;

  // Set NGG control settings
  if (ParsedGfxIp.major >= 10) {
    auto &nggState = compileInfo->gfxPipelineInfo.nggState;

    nggState.enableNgg = EnableNgg;
    nggState.enableGsUse = NggEnableGsUse;
    nggState.forceCullingMode = NggForceCullingMode;
    nggState.compactMode = static_cast<NggCompactMode>(NggCompactionMode.getValue());
    nggState.enableVertexReuse = NggEnableVertexReuse;
    nggState.enableBackfaceCulling = NggEnableBackfaceCulling;
    nggState.enableFrustumCulling = NggEnableFrustumCulling;
    nggState.enableBoxFilterCulling = NggEnableBoxFilterCulling;
    nggState.enableSphereCulling = NggEnableSphereCulling;
    nggState.enableSmallPrimFilter = NggEnableSmallPrimFilter;
    nggState.enableCullDistanceCulling = NggEnableCullDistanceCulling;

    nggState.backfaceExponent = NggBackfaceExponent;
    nggState.subgroupSizing = static_cast<NggSubgroupSizingType>(NggSubgroupSizing.getValue());
    nggState.primsPerSubgroup = NggPrimsPerSubgroup;
    nggState.vertsPerSubgroup = NggVertsPerSubgroup;
  }

  return Result::Success;
}

// =====================================================================================================================
// Process one pipeline. This can either be a single .pipe file or a set of shader stages.
//
// @param compiler : LLPC compiler
// @param inFiles : Input filename(s)
// @returns : Result::Success on success, other status codes on failure
static Result processInputs(ICompiler *compiler, ArrayRef<std::string> inFiles) {
  CompileInfo compileInfo = {};
  compileInfo.unlinked = true;
  compileInfo.doAutoLayout = true;

  // Clean code that gets run automatically before returning.
  auto onExit = make_scope_exit([&compileInfo] { cleanupCompileInfo(&compileInfo); });
  Result result = initCompileInfo(&compileInfo);
  if (result != Result::Success)
    return result;

  std::string fileNames;
  if (inFiles.size() == 1 && isPipelineInfoFile(inFiles[0])) {
    fileNames = inFiles[0] + " ";
    result = processInputPipeline(compiler, compileInfo, inFiles[0], Unlinked, IgnoreColorAttachmentFormats);
    if (result != Result::Success)
      return result;
  } else {
    result = processInputStages(compiler, compileInfo, inFiles, ValidateSpirv, fileNames);
    if (result != Result::Success)
      return result;
  }

  //
  // Build shader modules
  //
  if (compileInfo.stageMask != 0) {
    result = buildShaderModules(compiler, &compileInfo);
    if (result != Result::Success)
      return result;
  }

  if (!ToLink)
    return Result::Success;

  //
  // Build pipeline
  //
  Optional<PipelineDumpOptions> dumpOptions = None;
  if (cl::EnablePipelineDump) {
    dumpOptions.emplace();
    dumpOptions->pDumpDir = cl::PipelineDumpDir.c_str();
    dumpOptions->filterPipelineDumpByType = FilterPipelineDumpByType;
    dumpOptions->filterPipelineDumpByHash = FilterPipelineDumpByHash;
    dumpOptions->dumpDuplicatePipelines = DumpDuplicatePipelines;
  }

  compileInfo.fileNames = fileNames.c_str();
  result = buildPipeline(compiler, &compileInfo, dumpOptions, TimePassesIsEnabled || cl::EnableTimerProfile);
  if (result != Result::Success)
    return result;

  return outputElf(&compileInfo, OutFile, inFiles[0]);
}

#ifdef WIN_OS
// =====================================================================================================================
// Callback function for SIGABRT.
//
// @param signal : Signal type
extern "C" void llpcSignalAbortHandler(int signal) {
  if (signal == SIGABRT) {
    redirectLogOutput(true, 0, nullptr); // Restore redirecting to show crash in console window
    LLVM_BUILTIN_TRAP;
  }
}
#endif

#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
// =====================================================================================================================
// Enable VC run-time based memory leak detection.
static void enableMemoryLeakDetection() {
  // Retrieve the state of CRT debug reporting:
  int dbgFlag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);

  // Append custom flags to enable memory leak checks:
  dbgFlag |= _CRTDBG_LEAK_CHECK_DF;
  dbgFlag |= _CRTDBG_ALLOC_MEM_DF;

  // Update the run-time settings:
  _CrtSetDbgFlag(dbgFlag);
}
#endif

// =====================================================================================================================
// Main function of LLPC standalone tool, entry-point.
//
// @param argc : Count of arguments
// @param argv : List of arguments
// @returns : 0 if successful, other numeric values on failure
int main(int argc, char *argv[]) {
  //
  // Initialization
  //

  // TODO: CRT based Memory leak detection is conflict with stack trace now, we only can enable one of them.
#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
  enableMemoryLeakDetection();
#else
  EnablePrettyStackTrace();
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram x(argc, argv);

#ifdef WIN_OS
  signal(SIGABRT, llpcSignalAbortHandler);
#endif
#endif

#ifdef WIN_OS
  if (AssertToMsgBox) {
    _set_error_mode(_OUT_TO_MSGBOX);
  }
#endif

  ICompiler *compiler = nullptr;
  Result result = init(argc, argv, compiler);

  // Cleanup code that gets run automatically before returning.
  auto onExit = make_scope_exit([compiler, &result] {
    if (compiler)
      compiler->Destroy();

    if (result == Result::Success)
      LLPC_OUTS("\n=====  AMDLLPC SUCCESS  =====\n");
    else
      LLPC_ERRS("\n=====  AMDLLPC FAILED  =====\n");
  });

  if (result != Result::Success)
    return EXIT_FAILURE;

  std::vector<std::string> expandedInputFiles;
  result = expandInputFilenames(InFiles, expandedInputFiles);
  if (result != Result::Success)
    return EXIT_FAILURE;

  auto inputGroupsOrErr = groupInputFiles(expandedInputFiles);
  if (Error err = inputGroupsOrErr.takeError()) {
    reportError(std::move(err));
    result = Result::ErrorInvalidValue;
    return EXIT_FAILURE;
  }
  for (InputFilesGroup &inputGroup : *inputGroupsOrErr) {
    result = processInputs(compiler, inputGroup);
    if (result != Result::Success)
      return EXIT_FAILURE;
  }

  assert(result == Result::Success);
  return EXIT_SUCCESS;
}
