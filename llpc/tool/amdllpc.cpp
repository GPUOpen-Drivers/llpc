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
 * @file  amdllpc.cpp
 * @brief LLPC source file: contains implementation of LLPC standalone tool.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#endif

#include "amdllpc.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/ToolOutputFile.h"
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

#include <sstream>
#include <stdlib.h> // getenv

// NOTE: To enable VLD, please add option BUILD_WIN_VLD=1 in build option.To run amdllpc with VLD enabled,
// please copy vld.ini and all files in.\winVisualMemDetector\bin\Win64 to current directory of amdllpc.
#ifdef BUILD_WIN_VLD
#include "vld.h"
#endif

#ifndef LLPC_ENABLE_SPIRV_OPT
#define SPVGEN_STATIC_LIB 1
#endif
#include "llpc.h"
#include "llpcDebug.h"
#include "llpcShaderModuleHelper.h"
#include "llpcSpirvLowerUtil.h"
#include "llpcUtil.h"
#include "spvgen.h"
#include "vfx.h"
#include "vkgcElfReader.h"

#define DEBUG_TYPE "amd-llpc"

using namespace llvm;
using namespace Llpc;
using namespace Vkgc;

// Represents options of LLPC standalone tool.

// -gfxip: graphics IP version
static cl::opt<std::string> GfxIp("gfxip", cl::desc("Graphics IP version"), cl::value_desc("major.minor.step"),
                                  cl::init("8.0.2"));

// The GFXIP version parsed out of the -gfxip option before normal option processing occurs.
static GfxIpVersion ParsedGfxIp = {8, 0, 2};

// Input sources
static cl::list<std::string> InFiles(cl::Positional, cl::OneOrMore, cl::ValueRequired,
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
static cl::opt<std::string> OutFile("o", cl::desc("Output file"), cl::value_desc("filename (\"-\" for stdout)"));

// -l: link pipeline
static cl::opt<bool> ToLink("l", cl::desc("Link pipeline and generate ISA codes"), cl::init(true));

// -val: validate input SPIR-V binary or text
static cl::opt<bool> Validate("val", cl::desc("Validate input SPIR-V binary or text"), cl::init(true));

// -entry-target: name string of entry target (for multiple entry-points)
static cl::opt<std::string> EntryTarget("entry-target", cl::desc("Name string of entry target"),
                                        cl::value_desc("entryname"), cl::init(""));

// -ignore-color-attachment-formats: ignore color attachment formats
static cl::opt<bool> IgnoreColorAttachmentFormats("ignore-color-attachment-formats",
                                                  cl::desc("Ignore color attachment formats"), cl::init(false));

// -enable-ngg: enable NGG mode
static cl::opt<bool> EnableNgg("enable-ngg", cl::desc("Enable implicit primitive shader (NGG) mode"), cl::init(true));

// -enable-gs-use: enable NGG use on geometry shader
static cl::opt<bool> NggEnableGsUse("ngg-enable-gs-use", cl::desc("Enable NGG use on geometry shader"),
                                    cl::init(false));

// -ngg-force-non-passthrough: force NGG to run in non pass-through mode
static cl::opt<bool> NggForceNonPassThrough("ngg-force-non-passthrough",
                                            cl::desc("Force NGG to run in non pass-through mode"), cl::init(false));

// -ngg-always-use-prim-shader-table: always use primitive shader table to fetch culling-control registers
static cl::opt<bool>
    NggAlwaysUsePrimShaderTable("ngg-always-use-prim-shader-table",
                                cl::desc("Always use primitive shader table to fetch culling-control registers (NGG)"),
                                cl::init(true));

// -ngg-compact-mode: NGG compaction mode (NGG)
static cl::opt<unsigned> NggCompactionMode("ngg-compaction-mode",
                                           cl::desc("Compaction mode after culling operations (NGG):\n"
                                                    "0: Compaction is based on the whole sub-group\n"
                                                    "1: Compaction is based on vertices"),
                                           cl::value_desc("mode"), cl::init(static_cast<unsigned>(NggCompactVertices)));

// -ngg-enable-fast-launch-rate: enable the hardware to launch subgroups of work at a faster rate (NGG)
static cl::opt<bool>
    NggEnableFastLaunchRate("ngg-enable-fast-launch-rate",
                            cl::desc("Enable the hardware to launch subgroups of work at a faster rate (NGG)"),
                            cl::init(false));

// -ngg-enable-vertex-reuse: enable optimization to cull duplicate vertices (NGG)
static cl::opt<bool> NggEnableVertexReuse("ngg-enable-vertex-reuse",
                                          cl::desc("Enable optimization to cull duplicate vertices (NGG)"),
                                          cl::init(false));

// -ngg-enable-backface-culling: enable culling of primitives that don't meet facing criteria (NGG)
static cl::opt<bool>
    NggEnableBackfaceCulling("ngg-enable-backface-culling",
                             cl::desc("Enable culling of primitives that don't meet facing criteria (NGG)"),
                             cl::init(false));

// -ngg-enable-frustum-culling: enable discarding of primitives outside of view frustum (NGG)
static cl::opt<bool> NggEnableFrustumCulling("ngg-enable-frustum-culling",
                                             cl::desc("Enable discarding of primitives outside of view frustum (NGG)"),
                                             cl::init(false));

// -ngg-enable-box-filter-culling: enable simpler frustum culler that is less accurate (NGG)
static cl::opt<bool> NggEnableBoxFilterCulling("ngg-enable-box-filter-culling",
                                               cl::desc("Enable simpler frustum culler that is less accurate (NGG)"),
                                               cl::init(false));

// -ngg-enable-sphere-culling: enable frustum culling based on a sphere (NGG)
static cl::opt<bool> NggEnableSphereCulling("ngg-enable-sphere-culling",
                                            cl::desc("Enable frustum culling based on a sphere (NGG)"),
                                            cl::init(false));

// -ngg-enable-small-prim-filter: enable trivial sub-sample primitive culling (NGG)
static cl::opt<bool> NggEnableSmallPrimFilter("ngg-enable-small-prim-filter",
                                              cl::desc("Enable trivial sub-sample primitive culling (NGG)"),
                                              cl::init(false));

// -ngg-cull-distance-culling: enable culling when "cull distance" exports are present (NGG)
static cl::opt<bool>
    NggEnableCullDistanceCulling("ngg-enable-cull-distance-culling",
                                 cl::desc("Enable culling when \"cull distance\" exports are present (NGG)"),
                                 cl::init(false));

// -ngg-backface-exponent: control backface culling algorithm (NGG, 1 ~ UINT32_MAX, 0 disables it)
static cl::opt<unsigned> NggBackfaceExponent("ngg-backface-exponent",
                                             cl::desc("Control backface culling algorithm (NGG)"),
                                             cl::value_desc("exp"), cl::init(0));

// -ngg-subgroup-sizing: NGG sub-group sizing type (NGG)
static cl::opt<unsigned> NggSubgroupSizing(
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
static cl::opt<unsigned>
    NggPrimsPerSubgroup("ngg-prims-per-subgroup",
                        cl::desc("Preferred numberof GS primitives to pack into a primitive shader sub-group (NGG)"),
                        cl::value_desc("prims"), cl::init(256));

// -ngg-verts-per-subgroup: preferred number of vertices consumed by a primitive shader sub-group (NGG)
static cl::opt<unsigned>
    NggVertsPerSubgroup("ngg-verts-per-subgroup",
                        cl::desc("Preferred number of vertices consumed by a primitive shader sub-group (NGG)"),
                        cl::value_desc("verts"), cl::init(256));

// -spvgen-dir: load SPVGEN from specified directory
static cl::opt<std::string> SpvGenDir("spvgen-dir", cl::desc("Directory to load SPVGEN library from"));

static cl::opt<bool> RobustBufferAccess("robust-buffer-access", cl::desc("Validate if the index is out of bounds"),
                                        cl::init(false));

// -check-auto-layout-compatible: check if auto descriptor layout got from spv file is commpatible with real layout
static cl::opt<bool> CheckAutoLayoutCompatible(
    "check-auto-layout-compatible",
    cl::desc("check if auto descriptor layout got from spv file is commpatible with real layout"));

namespace llvm {

namespace cl {

extern opt<bool> EnablePipelineDump;
extern opt<std::string> PipelineDumpDir;
extern opt<bool> DisableNullFragShader;
extern opt<bool> EnableTimerProfile;

// -filter-pipeline-dump-by-type: filter which kinds of pipeline should be disabled.
static opt<unsigned> FilterPipelineDumpByType("filter-pipeline-dump-by-type",
                                              desc("Filter which types of pipeline dump are disabled\n"
                                                   "0x00 - Always enable pipeline logging\n"
                                                   "0x01 - Disable logging for CS pipelines\n"
                                                   "0x02 - Disable logging for NGG pipelines\n"
                                                   "0x04 - Disable logging for GS pipelines\n"
                                                   "0x08 - Disable logging for TS pipelines\n"
                                                   "0x10 - Disable logging for VS-PS pipelines"),
                                              init(0));

//// -filter-pipeline-dump-by-hash: only dump the pipeline whose computed hash is equal to the specified (if non-zero).
static opt<uint64_t>
    FilterPipelineDumpByHash("filter-pipeline-dump-by-hash",
                             desc("Only dump the pipeline whose computed hash is equal to the specified (if non-zero)"),
                             init(0));

//-dump-duplicate-pipelines: dump duplicated pipeline, attaching a numeric suffix
static opt<bool>
    DumpDuplicatePipelines("dump-duplicate-pipelines",
                           desc("If TRUE, duplicate pipelines will be dumped to a file with a numeric suffix attached"),
                           init(false));

} // namespace cl

} // namespace llvm

#ifdef WIN_OS
// -assert-to-msgbox: pop message box when an assert is hit, only valid in Windows
static cl::opt<bool> AssertToMsgBox("assert-to-msgbox", cl::desc("Pop message box when assert is hit"));
#endif

// Represents allowed extensions of LLPC source files.
namespace LlpcExt {

const char SpirvBin[] = ".spv";
const char SpirvText[] = ".spvasm";
const char PipelineInfo[] = ".pipe";
const char LlvmIr[] = ".ll";

} // namespace LlpcExt

// Represents the module info for a shader module.
struct ShaderModuleData {
  ShaderStage shaderStage;          // Shader stage
  BinaryData spirvBin;              // SPIR-V binary codes
  ShaderModuleBuildInfo shaderInfo; // Info to build shader modules
  ShaderModuleBuildOut shaderOut;   // Output of building shader modules
  void *shaderBuf;                  // Allocation buffer of building shader modules
};

// Represents global compilation info of LLPC standalone tool (as tool context).
struct CompileInfo {
  GfxIpVersion gfxIp;                                // Graphics IP version info
  VkFlags stageMask;                                 // Shader stage mask
  std::vector<::ShaderModuleData> shaderModuleDatas; // ShaderModule Data
  GraphicsPipelineBuildInfo gfxPipelineInfo;         // Info to build graphics pipeline
  GraphicsPipelineBuildOut gfxPipelineOut;           // Output of building graphics pipeline
  ComputePipelineBuildInfo compPipelineInfo;         // Info to build compute pipeline
  ComputePipelineBuildOut compPipelineOut;           // Output of building compute pipeline
  void *pipelineBuf;              // Alllocation buffer of building pipeline
  void *pipelineInfoFile;         // VFX-style file containing pipeline info
  const char *fileNames;          // Names of input shader source files
  bool doAutoLayout;              // Whether to auto layout descriptors
  bool checkAutoLayoutCompatible; // Whether to comapre if auto layout descriptors is
                                  // same as specified pipeline layout
};

// =====================================================================================================================
// Checks whether the input data is actually a ELF binary
//
// @param data : Input data to check
// @param dataSize : Size of the input data
static bool isElfBinary(const void *data, size_t dataSize) {
  bool isElfBin = false;
  if (dataSize >= sizeof(Elf64::FormatHeader)) {
    auto header = reinterpret_cast<const Elf64::FormatHeader *>(data);
    isElfBin = header->e_ident32[EI_MAG0] == ElfMagic;
  }
  return isElfBin;
}

// =====================================================================================================================
// Checks whether the input data is actually LLVM bitcode
//
// @param data : Input data to check
// @param dataSize : Size of the input data
static bool isLlvmBitcode(const void *data, size_t dataSize) {
  const unsigned char magic[] = {'B', 'C', 0xC0, 0xDE};
  return dataSize >= sizeof magic && memcmp(data, magic, sizeof magic) == 0;
}

// =====================================================================================================================
// Checks whether the output data is actually ISA assembler text
//
// @param data : Input data to check
// @param dataSize : Size of the input data
static bool isIsaText(const void *data, size_t dataSize) {
  // This is called by amdllpc to help distinguish between its three output types of ELF binary, LLVM IR assembler
  // and ISA assembler. Here we use the fact that ISA assembler is the only one that starts with a tab character.
  return dataSize != 0 && (reinterpret_cast<const char *>(data))[0] == '\t';
}

// =====================================================================================================================
// Translates GLSL source language to corresponding shader stage.
//
// @param sourceLang : GLSL source language
static ShaderStage sourceLangToShaderStage(SpvGenStage sourceLang) {
  static_assert(SpvGenStageVertex == 0, "Unexpected value!");
  static_assert(SpvGenStageTessControl == 1, "Unexpected value!");
  static_assert(SpvGenStageTessEvaluation == 2, "Unexpected value!");
  static_assert(SpvGenStageGeometry == 3, "Unexpected value!");
  static_assert(SpvGenStageFragment == 4, "Unexpected value!");
  static_assert(SpvGenStageCompute == 5, "Unexpected value!");

  return static_cast<ShaderStage>(sourceLang);
}

// =====================================================================================================================
// Performs initialization work for LLPC standalone tool.
//
// @param argc : Count of arguments
// @param argv : List of arguments
// @param [out] ppCompiler : Created LLPC compiler object
static Result init(int argc, char *argv[], ICompiler **ppCompiler) {
  Result result = Result::Success;

  if (result == Result::Success) {
    // NOTE: For testing consistency, these options should be kept the same as those of Vulkan
    // ICD (Device::InitLlpcCompiler()). Here, we check the specified options from command line.
    // For each default option that is missing, we add it manually. This code to check whether
    // the same option has been specified is not completely foolproof because it does not know
    // which arguments are not option names.
    static const char *DefaultOptions[] = {
        // Name                                Option
        "-gfxip",
        "-gfxip=8.0.0",
        "-unroll-max-percent-threshold-boost",
        "-unroll-max-percent-threshold-boost=1000",
        "-pragma-unroll-threshold",
        "-pragma-unroll-threshold=1000",
        "-unroll-allow-partial",
        "-unroll-allow-partial",
        "-simplifycfg-sink-common",
        "-simplifycfg-sink-common=false",
        "-amdgpu-vgpr-index-mode",
        "-amdgpu-vgpr-index-mode", // Force VGPR indexing on GFX8
        "-amdgpu-atomic-optimizations",
        "-amdgpu-atomic-optimizations", // Enable atomic optimizations
        "-use-gpu-divergence-analysis",
        "-use-gpu-divergence-analysis", // Use new divergence analysis
        "-filetype",
        "-filetype=obj", // Target = obj, ELF binary; target = asm, ISA assembly text
    };

    // Build new arguments, starting with those supplied in command line
    std::vector<const char *> newArgs;
    for (int i = 0; i < argc; ++i)
      newArgs.push_back(argv[i]);

    static const size_t DefaultOptionCount = sizeof(DefaultOptions) / (2 * sizeof(DefaultOptions[0]));
    for (unsigned optionIdx = 0; optionIdx != DefaultOptionCount; ++optionIdx) {
      const char *name = DefaultOptions[2 * optionIdx];
      const char *option = DefaultOptions[2 * optionIdx + 1];
      size_t nameLen = strlen(name);
      bool found = false;
      const char *arg = nullptr;
      for (int i = 1; i < argc; ++i) {
        arg = argv[i];
        if (strncmp(arg, name, nameLen) == 0 &&
            (arg[nameLen] == '\0' || arg[nameLen] == '=' || isdigit((int)arg[nameLen]))) {
          found = true;
          break;
        }
      }

      if (!found)
        newArgs.push_back(option);
      else if (optionIdx == 0) // Find option -gfxip
      {
        size_t argLen = strlen(arg);
        if (argLen > nameLen && arg[nameLen] == '=') {
          // Extract tokens of graphics IP version info (delimiter is ".")
          const unsigned len = argLen - nameLen - 1;
          char *gfxIp = new char[len + 1];
          memcpy(gfxIp, &arg[nameLen + 1], len);
          gfxIp[len] = '\0';

          char *tokens[3] = {}; // Format: major.minor.step
          char *token = std::strtok(gfxIp, ".");
          for (unsigned i = 0; i < 3 && token; ++i) {
            tokens[i] = token;
            token = std::strtok(nullptr, ".");
          }

          ParsedGfxIp.major = tokens[0] ? std::strtoul(tokens[0], nullptr, 10) : 0;
          ParsedGfxIp.minor = tokens[1] ? std::strtoul(tokens[1], nullptr, 10) : 0;
          ParsedGfxIp.stepping = tokens[2] ? std::strtoul(tokens[2], nullptr, 10) : 0;

          delete[] gfxIp;
        }
      }
    }

    const char *name = "-shader-cache-file-dir";
    size_t nameLen = strlen(name);
    bool found = false;
    const char *arg = nullptr;
    for (int i = 1; i < argc; ++i) {
      arg = argv[i];
      if (strncmp(arg, name, nameLen) == 0 && (arg[nameLen] == '\0' || arg[nameLen] == '=')) {
        found = true;
        break;
      }
    }

    if (!found) {
      // Initialize the path for shader cache
      constexpr unsigned maxFilePathLen = 512;
      char shaderCacheFileDirOption[maxFilePathLen];

      // Initialize the root path of cache files
      // Steps:
      //   1. Find AMD_SHADER_DISK_CACHE_PATH to keep backward compatibility.
      const char *envString = getenv("AMD_SHADER_DISK_CACHE_PATH");

#ifdef WIN_OS
      //   2. Find LOCALAPPDATA.
      if (envString == nullptr) {
        envString = getenv("LOCALAPPDATA");
      }
#else
      char shaderCacheFileRootDir[maxFilePathLen];

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

      if (envString) {
        snprintf(shaderCacheFileDirOption, sizeof(shaderCacheFileDirOption), "-shader-cache-file-dir=%s", envString);
      } else
        strncpy(shaderCacheFileDirOption, "-shader-cache-file-dir=.", sizeof(shaderCacheFileDirOption));
      newArgs.push_back(shaderCacheFileDirOption);
    }

    // NOTE: We set the option -disable-null-frag-shader to TRUE for standalone compiler as the default.
    // Subsequent command option parse will correct its value if this option is specified externally.
    cl::DisableNullFragShader.setValue(true);

    result = ICompiler::Create(ParsedGfxIp, newArgs.size(), &newArgs[0], ppCompiler);
  }

  if (result == Result::Success && SpvGenDir != "") {
    // -spvgen-dir option: preload spvgen from the given directory
    if (!InitSpvGen(SpvGenDir.c_str())) {
      LLPC_ERRS("Failed to load SPVGEN from specified directory\n");
      result = Result::ErrorUnavailable;
    }
  }

  return result;
}

// =====================================================================================================================
// Performs per-pipeline initialization work for LLPC standalone tool.
//
// @param [out] compileInfo : Compilation info of LLPC standalone tool
static Result initCompileInfo(CompileInfo *compileInfo) {
  compileInfo->gfxIp = ParsedGfxIp;

  // Set NGG control settings
  if (ParsedGfxIp.major >= 10) {
    auto &nggState = compileInfo->gfxPipelineInfo.nggState;

    nggState.enableNgg = EnableNgg;
    nggState.enableGsUse = NggEnableGsUse;
    nggState.forceNonPassthrough = NggForceNonPassThrough;
    nggState.alwaysUsePrimShaderTable = NggAlwaysUsePrimShaderTable;
    nggState.compactMode = static_cast<NggCompactMode>(NggCompactionMode.getValue());
    nggState.enableFastLaunch = NggEnableFastLaunchRate;
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
// Performs cleanup work for LLPC standalone tool.
//
// @param [in,out] compileInfo : Compilation info of LLPC standalone tool
static void cleanupCompileInfo(CompileInfo *compileInfo) {
  for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {
    // NOTE: We do not have to free SPIR-V binary for pipeline info file.
    // It will be freed when we close the VFX doc.
    if (!compileInfo->pipelineInfoFile)
      delete[] reinterpret_cast<const char *>(compileInfo->shaderModuleDatas[i].spirvBin.pCode);

    free(compileInfo->shaderModuleDatas[i].shaderBuf);
  }

  free(compileInfo->pipelineBuf);

  if (compileInfo->pipelineInfoFile)
    Vfx::vfxCloseDoc(compileInfo->pipelineInfoFile);

  memset(compileInfo, 0, sizeof(*compileInfo));
}

// =====================================================================================================================
// Callback function to allocate buffer for building shader module and building pipeline.
//
// @param instance : Dummy instance object, unused
// @param userData : User data
// @param size : Requested allocation size
void *VKAPI_CALL allocateBuffer(void *instance, void *userData, size_t size) {
  void *allocBuf = malloc(size);
  memset(allocBuf, 0, size);

  void **ppOutBuf = reinterpret_cast<void **>(userData);
  *ppOutBuf = allocBuf;
  return allocBuf;
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPIR-V assembly text file (.spvasm).
static bool isSpirvTextFile(const std::string &fileName) {
  bool isSpirvText = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == LlpcExt::SpirvText)
    isSpirvText = true;

  return isSpirvText;
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPIR-V binary file (.spv).
//
// @param fileName : File name to check
static bool isSpirvBinaryFile(const std::string &fileName) {
  bool isSpirvBin = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == LlpcExt::SpirvBin)
    isSpirvBin = true;

  return isSpirvBin;
}

// =====================================================================================================================
// Checks whether the specified file name represents a LLPC pipeline info file (.pipe).
//
// @param fileName : File name to check
static bool isPipelineInfoFile(const std::string &fileName) {
  bool isPipelineInfo = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == LlpcExt::PipelineInfo)
    isPipelineInfo = true;

  return isPipelineInfo;
}

// =====================================================================================================================
// Checks whether the specified file name represents a LLVM IR file (.ll).
//
// @param fileName : File name to check
static bool isLlvmIrFile(const std::string &fileName) {
  bool isLlvmIr = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == LlpcExt::LlvmIr)
    isLlvmIr = true;

  return isLlvmIr;
}

// =====================================================================================================================
// Gets SPIR-V binary codes from the specified binary file.
//
// @param spvBinFile : SPIR-V binary file
// @param [out] spvBin : SPIR-V binary codes
static Result getSpirvBinaryFromFile(const std::string &spvBinFile, BinaryData *spvBin) {
  Result result = Result::Success;

  FILE *binFile = fopen(spvBinFile.c_str(), "rb");
  if (!binFile) {
    LLPC_ERRS("Fails to open SPIR-V binary file: " << spvBinFile << "\n");
    result = Result::ErrorUnavailable;
  }

  if (result == Result::Success) {
    fseek(binFile, 0, SEEK_END);
    size_t binSize = ftell(binFile);
    fseek(binFile, 0, SEEK_SET);

    char *bin = new char[binSize];
    assert(bin);
    memset(bin, 0, binSize);
    binSize = fread(bin, 1, binSize, binFile);

    spvBin->codeSize = binSize;
    spvBin->pCode = bin;

    fclose(binFile);
  }

  return result;
}

// =====================================================================================================================
// GLSL compiler, compiles GLSL source text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, GLSL source text
// @param [out] stage : Shader stage
// @param [out] outFilename : Output filename, SPIR-V binary
static Result compileGlsl(const std::string &inFilename, ShaderStage *stage, std::string &outFilename) {
  if (!InitSpvGen()) {
    LLPC_ERRS("Failed to load SPVGEN -- cannot compile GLSL\n");
    return Result::ErrorUnavailable;
  }

  Result result = Result::Success;
  bool isHlsl = false;

  SpvGenStage lang = spvGetStageTypeFromName(inFilename.c_str(), &isHlsl);
  if (lang == SpvGenStageInvalid) {
    LLPC_ERRS("File " << inFilename << ": Bad file extension; try -help\n");
    return Result::ErrorInvalidShader;
  }
  *stage = sourceLangToShaderStage(lang);

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile) {
    LLPC_ERRS("Fails to open input file: " << inFilename << "\n");
    result = Result::ErrorUnavailable;
  }

  FILE *outFile = nullptr;
  if (result == Result::Success) {
    outFilename = sys::path::filename(inFilename).str() + LlpcExt::SpirvBin;

    outFile = fopen(outFilename.c_str(), "wb");
    if (!outFile) {
      LLPC_ERRS("Fails to open output file: " << outFilename << "\n");
      result = Result::ErrorUnavailable;
    }
  }

  if (result == Result::Success) {
    fseek(inFile, 0, SEEK_END);
    size_t textSize = ftell(inFile);
    fseek(inFile, 0, SEEK_SET);

    char *glslText = new char[textSize + 1];
    assert(glslText);
    memset(glslText, 0, textSize + 1);
    auto readSize = fread(glslText, 1, textSize, inFile);
    glslText[readSize] = 0;

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// GLSL sources: " << inFilename << "\n\n");
    LLPC_OUTS(glslText);
    LLPC_OUTS("\n\n");

    int sourceStringCount = 1;
    const char *const *sourceList[1] = {};
    const char *fileName = inFilename.c_str();
    const char *const *fileList[1] = {&fileName};
    sourceList[0] = &glslText;

    void *program = nullptr;
    const char *log = nullptr;
    int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules | SpvGenOptionDebug;
    compileOption |= isHlsl ? SpvGenOptionReadHlsl : 0;
    const char *entryPoints[] = {EntryTarget.c_str()};
    bool compileResult = spvCompileAndLinkProgramEx(1, &lang, &sourceStringCount, sourceList, fileList,
                                                    isHlsl ? entryPoints : nullptr, &program, &log, compileOption);

    LLPC_OUTS("// GLSL program compile/link log\n");

    if (compileResult) {
      const unsigned *spvBin = nullptr;
      unsigned binSize = spvGetSpirvBinaryFromProgram(program, 0, &spvBin);
      fwrite(spvBin, 1, binSize, outFile);

      textSize = binSize * 10 + 1024;
      char *spvText = new char[textSize];
      assert(spvText);
      memset(spvText, 0, textSize);
      LLPC_OUTS("\nSPIR-V disassembly: " << outFilename << "\n");
      spvDisassembleSpirv(binSize, spvBin, textSize, spvText);
      LLPC_OUTS(spvText << "\n");
      delete[] spvText;
    } else {
      LLPC_ERRS("Fail to compile GLSL sources\n\n" << log << "\n");
      result = Result::ErrorInvalidShader;
    }

    delete[] glslText;

    fclose(inFile);
    fclose(outFile);
  }

  return result;
}

// =====================================================================================================================
// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V binary file (output).
//
// @param inFilename : Input filename, SPIR-V assembly text
// @param [out] outFilename : Output filename, SPIR-V binary
static Result assembleSpirv(const std::string &inFilename, std::string &outFilename) {
  if (!InitSpvGen()) {
    LLPC_ERRS("Failed to load SPVGEN -- cannot assemble SPIR-V assembler source\n");
    return Result::ErrorUnavailable;
  }

  Result result = Result::Success;

  FILE *inFile = fopen(inFilename.c_str(), "r");
  if (!inFile) {
    LLPC_ERRS("Fails to open input file: " << inFilename << "\n");
    result = Result::ErrorUnavailable;
  }

  FILE *outFile = nullptr;
  if (result == Result::Success) {
    outFilename = sys::path::stem(sys::path::filename(inFilename)).str() + LlpcExt::SpirvBin;

    outFile = fopen(outFilename.c_str(), "wb");
    if (!outFile) {
      LLPC_ERRS("Fails to open output file: " << outFilename << "\n");
      result = Result::ErrorUnavailable;
    }
  }

  if (result == Result::Success) {
    fseek(inFile, 0, SEEK_END);
    size_t textSize = ftell(inFile);
    fseek(inFile, 0, SEEK_SET);

    char *spvText = new char[textSize + 1];
    assert(spvText);
    memset(spvText, 0, textSize + 1);

    size_t realSize = fread(spvText, 1, textSize, inFile);
    spvText[realSize] = '\0';

    int binSize = realSize * 4 + 1024; // Estimated SPIR-V binary size
    unsigned *spvBin = new unsigned[binSize / sizeof(unsigned)];
    assert(spvBin);

    const char *log = nullptr;
    binSize = spvAssembleSpirv(spvText, binSize, spvBin, &log);
    if (binSize < 0) {
      LLPC_ERRS("Fails to assemble SPIR-V: \n" << log << "\n");
      result = Result::ErrorInvalidShader;
    } else {
      fwrite(spvBin, 1, binSize, outFile);

      LLPC_OUTS("===============================================================================\n");
      LLPC_OUTS("// SPIR-V disassembly: " << inFilename << "\n");
      LLPC_OUTS(spvText);
      LLPC_OUTS("\n\n");
    }

    fclose(inFile);
    fclose(outFile);

    delete[] spvText;
    delete[] spvBin;
  }

  return result;
}

// =====================================================================================================================
// Decodes the binary after building a pipeline and outputs the decoded info.
//
// @param pipelineBin : Pipeline binary
// @param [in,out] compileInfo : Compilation info of LLPC standalone tool
// @param isGraphics : Whether it is graphics pipeline
static Result decodePipelineBinary(const BinaryData *pipelineBin, CompileInfo *compileInfo, bool isGraphics) {
  // Ignore failure from ElfReader. It fails if pPipelineBin is not ELF, as happens with
  // -filetype=asm.
  ElfReader<Elf64> reader(compileInfo->gfxIp);
  size_t readSize = 0;
  if (reader.readFromBuffer(pipelineBin->pCode, &readSize) == Result::Success) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC final ELF info\n");
    LLPC_OUTS(reader);
  }

  return Result::Success;
}

// =====================================================================================================================
// Builds shader module based on the specified SPIR-V binary.
//
// @param compiler : LLPC compiler object
// @param [in,out] compileInfo : Compilation info of LLPC standalone tool
static Result buildShaderModules(const ICompiler *compiler, CompileInfo *compileInfo) {
  Result result = Result::Success;

  for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {
    ShaderModuleBuildInfo *shaderInfo = &(compileInfo->shaderModuleDatas[i].shaderInfo);
    ShaderModuleBuildOut *shaderOut = &(compileInfo->shaderModuleDatas[i].shaderOut);

    shaderInfo->pInstance = nullptr; // Dummy, unused
    shaderInfo->pUserData = &(compileInfo->shaderModuleDatas[i].shaderBuf);
    shaderInfo->pfnOutputAlloc = allocateBuffer;
    shaderInfo->shaderBin = compileInfo->shaderModuleDatas[i].spirvBin;

    result = compiler->BuildShaderModule(shaderInfo, shaderOut);
    if (result != Result::Success && result != Result::Delayed) {
      LLPC_ERRS("Fails to build " << getShaderStageName(compileInfo->shaderModuleDatas[i].shaderStage)
                                  << " shader module:\n");
      break;
    }
  }

  return result;
}

// =====================================================================================================================
// Check autolayout compatible.
//
// @param compiler : LLPC compiler object
// @param [in,out] compileInfo : Compilation info of LLPC standalone tool
static Result checkAutoLayoutCompatibleFunc(const ICompiler *compiler, CompileInfo *compileInfo) {
  Result result = Result::Success;

  bool isGraphics = (compileInfo->stageMask & (shaderStageToMask(ShaderStageCompute) - 1)) != 0;
  if (isGraphics) {
    // Build graphics pipeline
    GraphicsPipelineBuildInfo *pipelineInfo = &compileInfo->gfxPipelineInfo;

    // Fill pipeline shader info
    PipelineShaderInfo *shaderInfos[ShaderStageGfxCount] = {
        &pipelineInfo->vs, &pipelineInfo->tcs, &pipelineInfo->tes, &pipelineInfo->gs, &pipelineInfo->fs,
    };

    unsigned userDataOffset = 0;
    for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {

      PipelineShaderInfo *shaderInfo = shaderInfos[compileInfo->shaderModuleDatas[i].shaderStage];
      bool checkAutoLayoutCompatible = compileInfo->checkAutoLayoutCompatible;

      if (compileInfo->shaderModuleDatas[i].shaderStage != Vkgc::ShaderStageFragment)
        checkAutoLayoutCompatible = false;
      const ShaderModuleBuildOut *shaderOut = &(compileInfo->shaderModuleDatas[i].shaderOut);

      if (!shaderInfo->pEntryTarget) {
        // If entry target is not specified, use the one from command line option
        shaderInfo->pEntryTarget = EntryTarget.c_str();
      }
      shaderInfo->pModuleData = shaderOut->pModuleData;
      shaderInfo->entryStage = compileInfo->shaderModuleDatas[i].shaderStage;
      if (checkAutoLayoutCompatible) {
        PipelineShaderInfo shaderInfoCopy = *shaderInfo;
        GraphicsPipelineBuildInfo pipelineInfoCopy = *pipelineInfo;
        doAutoLayoutDesc(compileInfo->shaderModuleDatas[i].shaderStage, compileInfo->shaderModuleDatas[i].spirvBin,
                         &pipelineInfoCopy, &shaderInfoCopy, userDataOffset, true);
        if (checkShaderInfoComptible(shaderInfo, shaderInfoCopy.userDataNodeCount, shaderInfoCopy.pUserDataNodes) &&
            checkPipelineStateCompatible(compiler, pipelineInfo, &pipelineInfoCopy, ParsedGfxIp))
          outs() << "Auto Layout fragment shader in " << compileInfo->fileNames << " hitted\n";
        else
          outs() << "Auto Layout fragment shader in " << compileInfo->fileNames << " failed to hit\n";
        outs().flush();
      }
    }
  } else if (compileInfo->stageMask == shaderStageToMask(ShaderStageCompute)) {
    ComputePipelineBuildInfo *pipelineInfo = &compileInfo->compPipelineInfo;

    PipelineShaderInfo *shaderInfo = &pipelineInfo->cs;
    const ShaderModuleBuildOut *shaderOut = &compileInfo->shaderModuleDatas[0].shaderOut;

    if (!shaderInfo->pEntryTarget) {
      // If entry target is not specified, use the one from command line option
      shaderInfo->pEntryTarget = EntryTarget.c_str();
    }
    shaderInfo->entryStage = ShaderStageCompute;
    shaderInfo->pModuleData = shaderOut->pModuleData;

    if (compileInfo->checkAutoLayoutCompatible) {
      unsigned userDataOffset = 0;
      PipelineShaderInfo shaderInfoCopy = *shaderInfo;
      doAutoLayoutDesc(ShaderStageCompute, compileInfo->shaderModuleDatas[0].spirvBin, nullptr, &shaderInfoCopy,
                       userDataOffset, true);
      if (checkShaderInfoComptible(shaderInfo, shaderInfoCopy.userDataNodeCount, shaderInfoCopy.pUserDataNodes))
        outs() << "Auto Layout compute shader in " << compileInfo->fileNames << " hitted\n";
      else
        outs() << "Auto Layout compute shader in " << compileInfo->fileNames << " failed to hit\n";
      outs().flush();
    }
  }

  return result;
}

// =====================================================================================================================
// Builds pipeline and do linking.
//
// @param compiler : LLPC compiler object
// @param [in,out] compileInfo : Compilation info of LLPC standalone tool
static Result buildPipeline(ICompiler *compiler, CompileInfo *compileInfo) {
  Result result = Result::Success;

  bool isGraphics = (compileInfo->stageMask & (shaderStageToMask(ShaderStageCompute) - 1)) != 0;
  if (isGraphics) {
    // Build graphics pipeline
    GraphicsPipelineBuildInfo *pipelineInfo = &compileInfo->gfxPipelineInfo;
    GraphicsPipelineBuildOut *pipelineOut = &compileInfo->gfxPipelineOut;

    // Fill pipeline shader info
    PipelineShaderInfo *shaderInfos[ShaderStageGfxCount] = {
        &pipelineInfo->vs, &pipelineInfo->tcs, &pipelineInfo->tes, &pipelineInfo->gs, &pipelineInfo->fs,
    };

    unsigned userDataOffset = 0;
    for (unsigned i = 0; i < compileInfo->shaderModuleDatas.size(); ++i) {

      PipelineShaderInfo *shaderInfo = shaderInfos[compileInfo->shaderModuleDatas[i].shaderStage];
      const ShaderModuleBuildOut *shaderOut = &(compileInfo->shaderModuleDatas[i].shaderOut);

      if (!shaderInfo->pEntryTarget) {
        // If entry target is not specified, use the one from command line option
        shaderInfo->pEntryTarget = EntryTarget.c_str();
      }
      shaderInfo->pModuleData = shaderOut->pModuleData;
      shaderInfo->entryStage = compileInfo->shaderModuleDatas[i].shaderStage;

      // If not compiling from pipeline, lay out user data now.
      if (compileInfo->doAutoLayout) {
        doAutoLayoutDesc(compileInfo->shaderModuleDatas[i].shaderStage, compileInfo->shaderModuleDatas[i].spirvBin,
                         pipelineInfo, shaderInfo, userDataOffset, false);
      }
    }

    pipelineInfo->pInstance = nullptr; // Dummy, unused
    pipelineInfo->pUserData = &compileInfo->pipelineBuf;
    pipelineInfo->pfnOutputAlloc = allocateBuffer;

    // NOTE: If number of patch control points is not specified, we set it to 3.
    if (pipelineInfo->iaState.patchControlPoints == 0)
      pipelineInfo->iaState.patchControlPoints = 3;

    pipelineInfo->options.robustBufferAccess = RobustBufferAccess;

    void *pipelineDumpHandle = nullptr;
    if (llvm::cl::EnablePipelineDump) {
      PipelineDumpOptions dumpOptions = {};
      dumpOptions.pDumpDir = llvm::cl::PipelineDumpDir.c_str();
      dumpOptions.filterPipelineDumpByType = llvm::cl::FilterPipelineDumpByType;
      dumpOptions.filterPipelineDumpByHash = llvm::cl::FilterPipelineDumpByHash;
      dumpOptions.dumpDuplicatePipelines = llvm::cl::DumpDuplicatePipelines;

      PipelineBuildInfo localPipelineInfo = {};
      localPipelineInfo.pGraphicsInfo = pipelineInfo;
      pipelineDumpHandle = Vkgc::IPipelineDumper::BeginPipelineDump(&dumpOptions, localPipelineInfo);
    }

    if (TimePassesIsEnabled || cl::EnableTimerProfile) {
      auto hash = Vkgc::IPipelineDumper::GetPipelineHash(pipelineInfo);
      outs() << "LLPC PipelineHash: " << format("0x%016" PRIX64, hash) << " Files: " << compileInfo->fileNames << "\n";
      outs().flush();
    }

    result = compiler->BuildGraphicsPipeline(pipelineInfo, pipelineOut, pipelineDumpHandle);

    if (result == Result::Success) {
      if (llvm::cl::EnablePipelineDump) {
        Vkgc::BinaryData pipelineBinary = {};
        pipelineBinary.codeSize = pipelineOut->pipelineBin.codeSize;
        pipelineBinary.pCode = pipelineOut->pipelineBin.pCode;
        Vkgc::IPipelineDumper::DumpPipelineBinary(pipelineDumpHandle, ParsedGfxIp, &pipelineBinary);

        Vkgc::IPipelineDumper::EndPipelineDump(pipelineDumpHandle);
      }

      result = decodePipelineBinary(&pipelineOut->pipelineBin, compileInfo, true);
    }
  }
  else {
    // Build compute pipeline
    assert(compileInfo->shaderModuleDatas.size() == 1);
    assert(compileInfo->shaderModuleDatas[0].shaderStage == ShaderStageCompute);

    ComputePipelineBuildInfo *pipelineInfo = &compileInfo->compPipelineInfo;
    ComputePipelineBuildOut *pipelineOut = &compileInfo->compPipelineOut;

    PipelineShaderInfo *shaderInfo = &pipelineInfo->cs;
    const ShaderModuleBuildOut *shaderOut = &compileInfo->shaderModuleDatas[0].shaderOut;

    if (!shaderInfo->pEntryTarget) {
      // If entry target is not specified, use the one from command line option
      shaderInfo->pEntryTarget = EntryTarget.c_str();
    }

    shaderInfo->entryStage = ShaderStageCompute;
    shaderInfo->pModuleData = shaderOut->pModuleData;

    // If not compiling from pipeline, lay out user data now.
    if (compileInfo->doAutoLayout) {
      unsigned userDataOffset = 0;
      doAutoLayoutDesc(ShaderStageCompute, compileInfo->shaderModuleDatas[0].spirvBin, nullptr, shaderInfo,
                       userDataOffset, false);
    }

    pipelineInfo->pInstance = nullptr; // Dummy, unused
    pipelineInfo->pUserData = &compileInfo->pipelineBuf;
    pipelineInfo->pfnOutputAlloc = allocateBuffer;
    pipelineInfo->options.robustBufferAccess = RobustBufferAccess;

    void *pipelineDumpHandle = nullptr;
    if (llvm::cl::EnablePipelineDump) {
      PipelineDumpOptions dumpOptions = {};
      dumpOptions.pDumpDir = llvm::cl::PipelineDumpDir.c_str();
      dumpOptions.filterPipelineDumpByType = llvm::cl::FilterPipelineDumpByType;
      dumpOptions.filterPipelineDumpByHash = llvm::cl::FilterPipelineDumpByHash;
      dumpOptions.dumpDuplicatePipelines = llvm::cl::DumpDuplicatePipelines;
      PipelineBuildInfo localPipelineInfo = {};
      localPipelineInfo.pComputeInfo = pipelineInfo;
      pipelineDumpHandle = Vkgc::IPipelineDumper::BeginPipelineDump(&dumpOptions, localPipelineInfo);
    }

    if (TimePassesIsEnabled || cl::EnableTimerProfile) {
      auto hash = Vkgc::IPipelineDumper::GetPipelineHash(pipelineInfo);
      outs() << "LLPC PipelineHash: " << format("0x%016" PRIX64, hash) << " Files: " << compileInfo->fileNames << "\n";
      outs().flush();
    }

    result = compiler->BuildComputePipeline(pipelineInfo, pipelineOut, pipelineDumpHandle);

    if (result == Result::Success) {
      if (llvm::cl::EnablePipelineDump) {
        Vkgc::BinaryData pipelineBinary = {};
        pipelineBinary.codeSize = pipelineOut->pipelineBin.codeSize;
        pipelineBinary.pCode = pipelineOut->pipelineBin.pCode;
        Vkgc::IPipelineDumper::DumpPipelineBinary(pipelineDumpHandle, ParsedGfxIp, &pipelineBinary);

        Vkgc::IPipelineDumper::EndPipelineDump(pipelineDumpHandle);
      }

      result = decodePipelineBinary(&pipelineOut->pipelineBin, compileInfo, false);
    }
  }

  return result;
}

// =====================================================================================================================
// Output LLPC resulting binary (ELF binary, ISA assembly text, or LLVM bitcode) to the specified target file.
//
// @param compileInfo : Compilation info of LLPC standalone tool
// @param suppliedOutFile : Name of the file to output ELF binary (specify "" to use base name of first input file with
// appropriate extension; specify "-" to use stdout)
// @param firstInFile : Name of first input file
static Result outputElf(CompileInfo *compileInfo, const std::string &suppliedOutFile, StringRef firstInFile) {
  Result result = Result::Success;
  const BinaryData *pipelineBin = (compileInfo->stageMask & shaderStageToMask(ShaderStageCompute))
                                      ? &compileInfo->compPipelineOut.pipelineBin
                                      : &compileInfo->gfxPipelineOut.pipelineBin;
  SmallString<64> outFileName(suppliedOutFile);
  if (outFileName.empty()) {
    // NOTE: The output file name was not specified, so we construct a default file name.  We detect the
    // output file type and determine the file extension according to it. We are unable to access the
    // values of the options "-filetype" and "-emit-llvm".
    const char *ext = ".s";
    if (isElfBinary(pipelineBin->pCode, pipelineBin->codeSize))
      ext = ".elf";
    else if (isLlvmBitcode(pipelineBin->pCode, pipelineBin->codeSize))
      ext = ".bc";
    else if (isIsaText(pipelineBin->pCode, pipelineBin->codeSize))
      ext = ".s";
    else
      ext = ".ll";
    outFileName = sys::path::filename(firstInFile);
    sys::path::replace_extension(outFileName, ext);
  }

  FILE *outFile = stdout;
  if (outFileName != "-")
    outFile = fopen(outFileName.c_str(), "wb");

  if (!outFile) {
    LLPC_ERRS("Failed to open output file: " << outFileName << "\n");
    result = Result::ErrorUnavailable;
  }

  if (result == Result::Success) {
    if (fwrite(pipelineBin->pCode, 1, pipelineBin->codeSize, outFile) != pipelineBin->codeSize)
      result = Result::ErrorUnavailable;

    if (outFile != stdout && fclose(outFile) != 0)
      result = Result::ErrorUnavailable;

    if (result != Result::Success) {
      LLPC_ERRS("Failed to write output file: " << outFileName << "\n");
    }
  }
  return result;
}

#ifdef WIN_OS
// =====================================================================================================================
// Callback function for SIGABRT.
extern "C" void LlpcSignalAbortHandler(int signal) // Signal type
{
  if (signal == SIGABRT) {
    redirectLogOutput(true, 0, nullptr); // Restore redirecting to show crash in console window
    LLVM_BUILTIN_TRAP;
  }
}
#endif

#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
// =====================================================================================================================
// Enable VC run-time based memory leak detection.
static void EnableMemoryLeakDetection() {
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
// Process one pipeline.
//
// @param compiler : LLPC context
// @param inFiles : Input filename(s)
// @param startFile : Index of the starting file name being processed in the file name array
// @param [out] nextFile : Index of next file name being processed in the file name array
static Result processPipeline(ICompiler *compiler, ArrayRef<std::string> inFiles, unsigned startFile,
                              unsigned *nextFile) {
  Result result = Result::Success;
  CompileInfo compileInfo = {};
  std::string fileNames;
  compileInfo.doAutoLayout = true;
  compileInfo.checkAutoLayoutCompatible = CheckAutoLayoutCompatible;

  result = initCompileInfo(&compileInfo);

  //
  // Translate sources to SPIR-V binary
  //
  for (unsigned i = startFile; i < inFiles.size() && result == Result::Success; ++i) {
    const std::string &inFile = inFiles[i];
    std::string spvBinFile;

    if (isSpirvTextFile(inFile) || isSpirvBinaryFile(inFile)) {
      // SPIR-V assembly text or SPIR-V binary
      if (isSpirvTextFile(inFile))
        result = assembleSpirv(inFile, spvBinFile);
      else
        spvBinFile = inFile;

      BinaryData spvBin = {};

      if (result == Result::Success) {
        result = getSpirvBinaryFromFile(spvBinFile, &spvBin);

        if (result == Result::Success) {
          if (!InitSpvGen()) {
            LLPC_OUTS("Failed to load SPVGEN -- no SPIR-V disassembler available\n");
          } else {
            // Disassemble SPIR-V code
            unsigned textSize = spvBin.codeSize * 10 + 1024;
            char *spvText = new char[textSize];
            assert(spvText);
            memset(spvText, 0, textSize);

            LLPC_OUTS("\nSPIR-V disassembly for " << inFile << "\n");
            spvDisassembleSpirv(spvBin.codeSize, spvBin.pCode, textSize, spvText);
            LLPC_OUTS(spvText << "\n");

            delete[] spvText;
          }
        }
      }

      if (result == Result::Success && Validate) {
        char log[1024] = {};
        if (!InitSpvGen())
          errs() << "Warning: Failed to load SPVGEN -- cannot validate SPIR-V\n";
        else {
          if (!spvValidateSpirv(spvBin.codeSize, spvBin.pCode, sizeof(log), log)) {
            LLPC_ERRS("Fails to validate SPIR-V: \n" << log << "\n");
            result = Result::ErrorInvalidShader;
          }
        }
      }

      if (result == Result::Success) {
        // NOTE: If the entry target is not specified, we set it to the one gotten from SPIR-V binary.
        if (EntryTarget.empty())
          EntryTarget.setValue(ShaderModuleHelper::getEntryPointNameFromSpirvBinary(&spvBin));

        unsigned stageMask = ShaderModuleHelper::getStageMaskFromSpirvBinary(&spvBin, EntryTarget.c_str());

        if ((stageMask & compileInfo.stageMask) != 0)
          break;
        else if (stageMask != 0) {
          for (unsigned stage = ShaderStageVertex; stage < ShaderStageCount; ++stage) {
            if (stageMask & shaderStageToMask(static_cast<ShaderStage>(stage))) {
              ::ShaderModuleData shaderModuleData = {};
              shaderModuleData.shaderStage = static_cast<ShaderStage>(stage);
              shaderModuleData.spirvBin = spvBin;
              compileInfo.shaderModuleDatas.push_back(shaderModuleData);
              compileInfo.stageMask |= shaderStageToMask(static_cast<ShaderStage>(stage));
              break;
            }
          }
        } else {
          LLPC_ERRS(format("Fails to identify shader stages by entry-point \"%s\"\n", EntryTarget.c_str()));
          result = Result::ErrorUnavailable;
        }
      }

    } else if (isPipelineInfoFile(inFile)) {
      // NOTE: If the input file is pipeline file, we set the option -disable-null-frag-shader to FALSE
      // unconditionally.
      cl::DisableNullFragShader.setValue(false);

      const char *log = nullptr;
      bool vfxResult =
          Vfx::vfxParseFile(inFile.c_str(), 0, nullptr, VfxDocTypePipeline, &compileInfo.pipelineInfoFile, &log);
      if (vfxResult) {
        VfxPipelineStatePtr pipelineState = nullptr;
        Vfx::vfxGetPipelineDoc(compileInfo.pipelineInfoFile, &pipelineState);

        if (pipelineState->version != Vkgc::Version) {
          LLPC_ERRS("Version incompatible, SPVGEN::Version = " << pipelineState->version
                                                               << " AMDLLPC::Version = " << Vkgc::Version << "\n");
          result = Result::ErrorInvalidShader;
        } else {
          LLPC_OUTS("===============================================================================\n");
          LLPC_OUTS("// Pipeline file info for " << inFile << " \n\n");

          if (log && strlen(log) > 0) {
            LLPC_OUTS("Pipeline file parse warning:\n" << log << "\n");
          }

          compileInfo.compPipelineInfo = pipelineState->compPipelineInfo;
          compileInfo.gfxPipelineInfo = pipelineState->gfxPipelineInfo;
          if (IgnoreColorAttachmentFormats) {
            // NOTE: When this option is enabled, we set color attachment format to
            // R8G8B8A8_SRGB for color target 0. Also, for other color targets, if the
            // formats are not UNDEFINED, we set them to R8G8B8A8_SRGB as well.
            for (unsigned target = 0; target < MaxColorTargets; ++target) {
              if (target == 0 || compileInfo.gfxPipelineInfo.cbState.target[target].format != VK_FORMAT_UNDEFINED)
                compileInfo.gfxPipelineInfo.cbState.target[target].format = VK_FORMAT_R8G8B8A8_SRGB;
            }
          }

          if (EnableOuts() && !InitSpvGen()) {
            LLPC_OUTS("Failed to load SPVGEN -- cannot disassemble and validate SPIR-V\n");
          }

          for (unsigned stage = 0; stage < pipelineState->numStages; ++stage) {
            if (pipelineState->stages[stage].dataSize > 0) {
              ::ShaderModuleData shaderModuleData = {};
              shaderModuleData.spirvBin.codeSize = pipelineState->stages[stage].dataSize;
              shaderModuleData.spirvBin.pCode = pipelineState->stages[stage].pData;
              shaderModuleData.shaderStage = pipelineState->stages[stage].stage;

              compileInfo.shaderModuleDatas.push_back(shaderModuleData);
              compileInfo.stageMask |= shaderStageToMask(pipelineState->stages[stage].stage);

              if (spvDisassembleSpirv) {
                unsigned binSize = pipelineState->stages[stage].dataSize;
                unsigned textSize = binSize * 10 + 1024;
                char *spvText = new char[textSize];
                assert(spvText);
                memset(spvText, 0, textSize);
                LLPC_OUTS("\nSPIR-V disassembly for " << getShaderStageName(pipelineState->stages[stage].stage)
                                                      << " shader module:\n");
                spvDisassembleSpirv(binSize, shaderModuleData.spirvBin.pCode, textSize, spvText);
                LLPC_OUTS(spvText << "\n");
                delete[] spvText;
              }
            }
          }

          bool isGraphics = (compileInfo.stageMask & shaderStageToMask(ShaderStageCompute)) == 0;
          for (unsigned i = 0; i < compileInfo.shaderModuleDatas.size(); ++i) {
            compileInfo.shaderModuleDatas[i].shaderInfo.options.pipelineOptions =
                isGraphics ? compileInfo.gfxPipelineInfo.options : compileInfo.compPipelineInfo.options;
          }

          fileNames += inFile;
          fileNames += " ";
          *nextFile = i + 1;
          compileInfo.doAutoLayout = false;
          break;
        }
      } else {
        LLPC_ERRS("Failed to parse input file: " << inFile << "\n" << log << "\n");
        result = Result::ErrorInvalidShader;
      }
    } else if (isLlvmIrFile(inFile)) {
      LLVMContext context;
      SMDiagnostic errDiag;

      // Load LLVM IR
      std::unique_ptr<Module> module = parseAssemblyFile(inFile, errDiag, context, nullptr, false);
      if (!module.get()) {
        std::string errMsg;
        raw_string_ostream errStream(errMsg);
        errDiag.print(inFile.c_str(), errStream);
        LLPC_ERRS(errMsg);
        result = Result::ErrorInvalidShader;
      }

      // Verify LLVM module
      std::string errMsg;
      raw_string_ostream errStream(errMsg);
      if (result == Result::Success && verifyModule(*module.get(), &errStream)) {
        LLPC_ERRS("File " << inFile << " parsed, but fail to verify the module: " << errMsg << "\n");
        result = Result::ErrorInvalidShader;
      }

      // Check the shader stage of input module
      ShaderStage shaderStage = ShaderStageInvalid;
      if (result == Result::Success) {
        shaderStage = getShaderStageFromModule(module.get());
        if (shaderStage == ShaderStageInvalid) {
          LLPC_ERRS("File " << inFile << ": Fail to determine shader stage\n");
          result = Result::ErrorInvalidShader;
        }

        if (compileInfo.stageMask & shaderStageToMask(static_cast<ShaderStage>(shaderStage)))
          break;
      }

      if (result == Result::Success) {
        // Translate LLVM module to LLVM bitcode
        llvm::SmallString<1024> bitcodeBuf;
        raw_svector_ostream bitcodeStream(bitcodeBuf);
        WriteBitcodeToFile(*module.get(), bitcodeStream);
        void *code = new uint8_t[bitcodeBuf.size()];
        memcpy(code, bitcodeBuf.data(), bitcodeBuf.size());

        ::ShaderModuleData shaderModuledata = {};
        shaderModuledata.spirvBin.codeSize = bitcodeBuf.size();
        shaderModuledata.spirvBin.pCode = code;
        shaderModuledata.shaderStage = shaderStage;
        compileInfo.shaderModuleDatas.push_back(shaderModuledata);
        compileInfo.stageMask |= shaderStageToMask(static_cast<ShaderStage>(shaderStage));
        compileInfo.doAutoLayout = false;
      }
    } else {
      // GLSL source text

      // NOTE: If the entry target is not specified, we set it to GLSL default ("main").
      if (EntryTarget.empty())
        EntryTarget.setValue("main");

      ShaderStage stage = ShaderStageInvalid;
      result = compileGlsl(inFile, &stage, spvBinFile);
      if (result == Result::Success) {
        if (compileInfo.stageMask & shaderStageToMask(static_cast<ShaderStage>(stage)))
          break;

        compileInfo.stageMask |= shaderStageToMask(stage);
        ::ShaderModuleData shaderModuleData = {};
        result = getSpirvBinaryFromFile(spvBinFile, &shaderModuleData.spirvBin);
        shaderModuleData.shaderStage = stage;
        compileInfo.shaderModuleDatas.push_back(shaderModuleData);
      }
    }

    fileNames += inFile;
    fileNames += " ";
    *nextFile = i + 1;
  }

  if (result == Result::Success && compileInfo.checkAutoLayoutCompatible) {
    compileInfo.fileNames = fileNames.c_str();
    result = checkAutoLayoutCompatibleFunc(compiler, &compileInfo);
  } else {
    //
    // Build shader modules
    //
    if (result == Result::Success && compileInfo.stageMask != 0)
      result = buildShaderModules(compiler, &compileInfo);

    //
    // Build pipeline
    //
    if (result == Result::Success && ToLink) {
      compileInfo.fileNames = fileNames.c_str();
      result = buildPipeline(compiler, &compileInfo);
      if (result == Result::Success)
        result = outputElf(&compileInfo, OutFile, inFiles[0]);
    }
  }
  //
  // Clean up
  //
  cleanupCompileInfo(&compileInfo);

  return result;
}

#ifdef WIN_OS
// =====================================================================================================================
// Finds all filenames which can match input file name
void findAllMatchFiles(const std::string &inFile,           // [in] Input file name, include wildcard
                       std::vector<std::string> *pOutFiles) // [out] Output file names which can match input file name
{
  WIN32_FIND_DATAA data = {};

  // Separate folder name
  std::string folderName;
  auto separatorPos = inFile.find_last_of("/\\");
  if (separatorPos != std::string::npos) {
    folderName = inFile.substr(0, separatorPos + 1);
  }

  // Search first file
  HANDLE pSearchHandle = FindFirstFileA(inFile.c_str(), &data);
  if (pSearchHandle == INVALID_HANDLE_VALUE) {
    return;
  }

  // Copy first file's name
  pOutFiles->push_back(folderName + data.cFileName);

  // Copy other file names
  while (FindNextFileA(pSearchHandle, &data)) {
    pOutFiles->push_back(folderName + data.cFileName);
  }

  FindClose(pSearchHandle);
  return;
}
#endif
// =====================================================================================================================
// Main function of LLPC standalone tool, entry-point.
//
// Returns 0 if successful. Other numeric values indicate failure.
//
// @param argc : Count of arguments
// @param argv : List of arguments
int main(int argc, char *argv[]) {
  Result result = Result::Success;

  ICompiler *compiler = nullptr;

  //
  // Initialization
  //

  // TODO: CRT based Memory leak detection is conflict with stack trace now, we only can enable one of them.
#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
  EnableMemoryLeakDetection();
#else
  EnablePrettyStackTrace();
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram x(argc, argv);

#ifdef WIN_OS
  signal(SIGABRT, LlpcSignalAbortHandler);
#endif
#endif

  result = init(argc, argv, &compiler);

#ifdef WIN_OS
  if (AssertToMsgBox) {
    _set_error_mode(_OUT_TO_MSGBOX);
  }
#endif

  if (isPipelineInfoFile(InFiles[0]) || isLlvmIrFile(InFiles[0])) {
    unsigned nextFile = 0;

    // The first input file is a pipeline file or LLVM IR file. Assume they all are, and compile each one
    // separately but in the same context.
    for (unsigned i = 0; i < InFiles.size() && result == Result::Success; ++i) {
#ifdef WIN_OS
      if (InFiles[i].find_last_of("*?") != std::string::npos) {
        std::vector<std::string> matchFiles;
        findAllMatchFiles(InFiles[i], &matchFiles);
        if (matchFiles.size() == 0) {
          LLPC_ERRS("\nFailed to read file " << InFiles[i] << "\n");
          result = Result::ErrorInvalidValue;
        } else {
          for (unsigned j = 0; (j < matchFiles.size()) && (result == Result::Success); ++j) {
            result = processPipeline(compiler, matchFiles[j], 0, &nextFile);
          }
        }
      } else
#endif
      {
        result = processPipeline(compiler, InFiles[i], 0, &nextFile);
      }
    }
  } else if (result == Result::Success) {
    // Otherwise, join all input files into the same pipeline.
#ifdef WIN_OS
    if ((InFiles.size() == 1) && (InFiles[0].find_last_of("*?") != std::string::npos)) {
      std::vector<std::string> matchFiles;
      findAllMatchFiles(InFiles[0], &matchFiles);
      if (matchFiles.size() == 0) {
        LLPC_ERRS("\nFailed to read file " << InFiles[0] << "\n");
        result = Result::ErrorInvalidValue;
      } else {
        unsigned nextFile = 0;
        for (unsigned i = 0; (i < matchFiles.size()) && (result == Result::Success); ++i) {
          result = processPipeline(compiler, matchFiles[i], 0, &nextFile);
        }
      }
    } else
#endif
    {
      SmallVector<std::string, 6> inFiles;
      for (const auto &inFile : InFiles) {
#ifdef WIN_OS
        if (InFiles[0].find_last_of("*?") != std::string::npos) {
          LLPC_ERRS("\nCan't use wilecard if multiple filename is set in command\n");
          result = Result::ErrorInvalidValue;
          break;
        }
#endif
        inFiles.push_back(inFile);
      }

      if (result == Result::Success) {
        unsigned nextFile = 0;
        for (; result == Result::Success && nextFile < inFiles.size();)
          result = processPipeline(compiler, inFiles, nextFile, &nextFile);
      }
    }
  }

  compiler->Destroy();

  if (result == Result::Success) {
    LLPC_OUTS("\n=====  AMDLLPC SUCCESS  =====\n");
  } else {
    LLPC_ERRS("\n=====  AMDLLPC FAILED  =====\n");
  }

  return result == Result::Success ? 0 : 1;
}
