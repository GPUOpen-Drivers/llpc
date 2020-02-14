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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/ToolOutputFile.h"

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
    #define SPVGEN_STATIC_LIB   1
#endif
#include "spvgen.h"
#include "vfx.h"

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcElfReader.h"
#include "llpcInternal.h"
#include "llpcShaderModuleHelper.h"

#define DEBUG_TYPE "amd-llpc"

using namespace llvm;
using namespace Llpc;

// Represents options of LLPC standalone tool.

// -gfxip: graphics IP version
static cl::opt<std::string> GfxIp("gfxip",
                                  cl::desc("Graphics IP version"),
                                  cl::value_desc("major.minor.step"),
                                  cl::init("8.0.2"));

// The GFXIP version parsed out of the -gfxip option before normal option processing occurs.
static GfxIpVersion ParsedGfxIp = {8, 0, 2};

// Input sources
static cl::list<std::string> InFiles(cl::Positional, cl::OneOrMore, cl::ValueRequired,
            cl::desc("<source>...\n"
              "Type of input file is determined by its filename extension:\n"
              "  .spv      SPIR-V binary\n"
              "  .spvas    SPIR-V assembly text\n"
              "  .vert     GLSL vertex shader\n"
              "  .tesc     GLSL tessellation control shader\n"
              "  .tese     GLSL tessellation evaluation shader\n"
              "  .geom     GLSL geometry shader\n"
              "  .frag     GLSL fragment shader\n"
              "  .comp     GLSL compute shader\n"
              "  .pipe     Pipeline info file\n"
              "  .ll       LLVM IR assembly text"
              ));

// -o: output
static cl::opt<std::string> OutFile("o", cl::desc("Output file"), cl::value_desc("filename (\"-\" for stdout)"));

// -l: link pipeline
static cl::opt<bool>        ToLink("l", cl::desc("Link pipeline and generate ISA codes"), cl::init(true));

// -val: validate input SPIR-V binary or text
static cl::opt<bool>        Validate("val", cl::desc("Validate input SPIR-V binary or text"), cl::init(true));

// -entry-target: name string of entry target (for multiple entry-points)
static cl::opt<std::string> EntryTarget("entry-target",
                                        cl::desc("Name string of entry target"),
                                        cl::value_desc("entryname"),
                                        cl::init(""));

// -ignore-color-attachment-formats: ignore color attachment formats
static cl::opt<bool> IgnoreColorAttachmentFormats("ignore-color-attachment-formats",
                                                  cl::desc("Ignore color attachment formats"), cl::init(false));

// -enable-ngg: enable NGG mode
static cl::opt<bool> EnableNgg(
    "enable-ngg",
    cl::desc("Enable implicit primitive shader (NGG) mode"),
    cl::init(true));

// -enable-gs-use: enable NGG use on geometry shader
static cl::opt<bool> NggEnableGsUse(
    "ngg-enable-gs-use",
    cl::desc("Enable NGG use on geometry shader"),
    cl::init(false));

// -ngg-force-non-passthrough: force NGG to run in non pass-through mode
static cl::opt<bool> NggForceNonPassThrough(
    "ngg-force-non-passthrough",
    cl::desc("Force NGG to run in non pass-through mode"),
    cl::init(false));

// -ngg-always-use-prim-shader-table: always use primitive shader table to fetch culling-control registers
static cl::opt<bool> NggAlwaysUsePrimShaderTable(
    "ngg-always-use-prim-shader-table",
    cl::desc("Always use primitive shader table to fetch culling-control registers (NGG)"),
    cl::init(true));

// -ngg-compact-mode: NGG compaction mode (NGG)
static cl::opt<uint32_t> NggCompactionMode(
    "ngg-compaction-mode",
    cl::desc("Compaction mode after culling operations (NGG):\n"
             "0: Compaction is based on the whole sub-group\n"
             "1: Compaction is based on vertices"),
    cl::value_desc("mode"),
    cl::init(static_cast<uint32_t>(NggCompactVertices)));

// -ngg-enable-fast-launch-rate: enable the hardware to launch subgroups of work at a faster rate (NGG)
static cl::opt<bool> NggEnableFastLaunchRate(
    "ngg-enable-fast-launch-rate",
    cl::desc("Enable the hardware to launch subgroups of work at a faster rate (NGG)"),
    cl::init(false));

// -ngg-enable-vertex-reuse: enable optimization to cull duplicate vertices (NGG)
static cl::opt<bool> NggEnableVertexReuse(
    "ngg-enable-vertex-reuse",
    cl::desc("Enable optimization to cull duplicate vertices (NGG)"),
    cl::init(false));

// -ngg-enable-backface-culling: enable culling of primitives that don't meet facing criteria (NGG)
static cl::opt<bool> NggEnableBackfaceCulling(
    "ngg-enable-backface-culling",
    cl::desc("Enable culling of primitives that don't meet facing criteria (NGG)"),
    cl::init(false));

// -ngg-enable-frustum-culling: enable discarding of primitives outside of view frustum (NGG)
static cl::opt<bool> NggEnableFrustumCulling(
    "ngg-enable-frustum-culling",
    cl::desc("Enable discarding of primitives outside of view frustum (NGG)"),
    cl::init(false));

// -ngg-enable-box-filter-culling: enable simpler frustum culler that is less accurate (NGG)
static cl::opt<bool> NggEnableBoxFilterCulling(
    "ngg-enable-box-filter-culling",
    cl::desc("Enable simpler frustum culler that is less accurate (NGG)"),
    cl::init(false));

// -ngg-enable-sphere-culling: enable frustum culling based on a sphere (NGG)
static cl::opt<bool> NggEnableSphereCulling(
    "ngg-enable-sphere-culling",
    cl::desc("Enable frustum culling based on a sphere (NGG)"),
    cl::init(false));

// -ngg-enable-small-prim-filter: enable trivial sub-sample primitive culling (NGG)
static cl::opt<bool> NggEnableSmallPrimFilter(
    "ngg-enable-small-prim-filter",
    cl::desc("Enable trivial sub-sample primitive culling (NGG)"),
    cl::init(false));

// -ngg-cull-distance-culling: enable culling when "cull distance" exports are present (NGG)
static cl::opt<bool> NggEnableCullDistanceCulling(
    "ngg-enable-cull-distance-culling",
    cl::desc("Enable culling when \"cull distance\" exports are present (NGG)"),
    cl::init(false));

// -ngg-backface-exponent: control backface culling algorithm (NGG, 1 ~ UINT32_MAX, 0 disables it)
static cl::opt<uint32_t> NggBackfaceExponent(
    "ngg-backface-exponent",
    cl::desc("Control backface culling algorithm (NGG)"),
    cl::value_desc("exp"),
    cl::init(0));

// -ngg-subgroup-sizing: NGG sub-group sizing type (NGG)
static cl::opt<uint32_t> NggSubgroupSizing(
    "ngg-subgroup-sizing",
    cl::desc("NGG sub-group sizing type (NGG):\n"
             "0: Sub-group size is allocated as optimally determined\n"
             "1: Sub-group size is allocated to the maximum allowable size\n"
             "2: Sub-group size is allocated as to allow half of the maximum allowable size\n"
             "3: Sub-group size is optimized for vertex thread utilization\n"
             "4: Sub-group size is optimized for primitive thread utilization\n"
             "5: Sub-group size is allocated based on explicitly-specified vertsPerSubgroup and primsPerSubgroup"),
    cl::value_desc("sizing"),
    cl::init(static_cast<uint32_t>(NggSubgroupSizingType::Auto)));

// -ngg-prims-per-subgroup: preferred numberof GS primitives to pack into a primitive shader sub-group (NGG)
static cl::opt<uint32_t> NggPrimsPerSubgroup(
    "ngg-prims-per-subgroup",
    cl::desc("Preferred numberof GS primitives to pack into a primitive shader sub-group (NGG)"),
    cl::value_desc("prims"),
    cl::init(256));

// -ngg-verts-per-subgroup: preferred number of vertices consumed by a primitive shader sub-group (NGG)
static cl::opt<uint32_t> NggVertsPerSubgroup(
    "ngg-verts-per-subgroup",
    cl::desc("Preferred number of vertices consumed by a primitive shader sub-group (NGG)"),
    cl::value_desc("verts"),
    cl::init(256));

// -spvgen-dir: load SPVGEN from specified directory
static cl::opt<std::string> SpvGenDir("spvgen-dir", cl::desc("Directory to load SPVGEN library from"));

static cl::opt<bool> RobustBufferAccess("robust-buffer-access",
                                        cl::desc("Validate if the index is out of bounds"), cl::init(false));

// -check-auto-layout-compatible: check if auto descriptor layout got from spv file is commpatible with real layout
static cl::opt<bool> CheckAutoLayoutCompatible(
    "check-auto-layout-compatible",
    cl::desc("check if auto descriptor layout got from spv file is commpatible with real layout"));

namespace llvm
{

namespace cl
{

extern opt<bool> EnablePipelineDump;
extern opt<std::string> PipelineDumpDir;
extern opt<bool> DisableNullFragShader;
extern opt<bool> EnableTimerProfile;

// -filter-pipeline-dump-by-type: filter which kinds of pipeline should be disabled.
static opt<uint32_t> FilterPipelineDumpByType("filter-pipeline-dump-by-type",
                                              desc("Filter which types of pipeline dump are disabled\n"
                                                   "0x00 - Always enable pipeline logging\n"
                                                   "0x01 - Disable logging for CS pipelines\n"
                                                   "0x02 - Disable logging for NGG pipelines\n"
                                                   "0x04 - Disable logging for GS pipelines\n"
                                                   "0x08 - Disable logging for TS pipelines\n"
                                                   "0x10 - Disable logging for VS-PS pipelines"),
                                              init(0));

//// -filter-pipeline-dump-by-hash: only dump the pipeline whose computed hash is equal to the specified (if non-zero).
static opt<uint64_t> FilterPipelineDumpByHash("filter-pipeline-dump-by-hash",
                                              desc("Only dump the pipeline whose computed hash is equal to the specified (if non-zero)"),
                                              init(0));

//-dump-duplicate-pipelines: dump duplicated pipeline, attaching a numeric suffix
static opt<bool> DumpDuplicatePipelines("dump-duplicate-pipelines",
    desc("If TRUE, duplicate pipelines will be dumped to a file with a numeric suffix attached"),
    init(false));

} // cl

} // llvm

#ifdef WIN_OS
// -assert-to-msgbox: pop message box when an assert is hit, only valid in Windows
static cl::opt<bool>        AssertToMsgBox("assert-to-msgbox", cl::desc("Pop message box when assert is hit"));
#endif

// Represents allowed extensions of LLPC source files.
namespace LlpcExt
{

const char SpirvBin[]       = ".spv";
const char SpirvText[]      = ".spvas";
const char PipelineInfo[]   = ".pipe";
const char LlvmIr[]         = ".ll";

} // LlpcExt

// Represents the module info for a shader module.
struct ShaderModuleData
{
    ShaderStage                 shaderStage;  // Shader stage
    BinaryData                  spirvBin;     // SPIR-V binary codes
    ShaderModuleBuildInfo       shaderInfo;   // Info to build shader modules
    ShaderModuleBuildOut        shaderOut;    // Output of building shader modules
    void*                       shaderBuf;    // Allocation buffer of building shader modules
};

// Represents global compilation info of LLPC standalone tool (as tool context).
struct CompileInfo
{
    GfxIpVersion                gfxIp;                          // Graphics IP version info
    VkFlags                     stageMask;                      // Shader stage mask
    std::vector<::ShaderModuleData> shaderModuleDatas;            // ShaderModule Data
    GraphicsPipelineBuildInfo   gfxPipelineInfo;                // Info to build graphics pipeline
    GraphicsPipelineBuildOut    gfxPipelineOut;                 // Output of building graphics pipeline
    ComputePipelineBuildInfo    compPipelineInfo;               // Info to build compute pipeline
    ComputePipelineBuildOut     compPipelineOut;                // Output of building compute pipeline
    void*                       pPipelineBuf;                   // Alllocation buffer of building pipeline
    void*                       pPipelineInfoFile;              // VFX-style file containing pipeline info
    const char*                 pFileNames;                     // Names of input shader source files
    bool                        doAutoLayout;                   // Whether to auto layout descriptors
    bool                        checkAutoLayoutCompatible;      // Whether to comapre if auto layout descriptors is
                                                                // same as specified pipeline layout
};

// =====================================================================================================================
// Translates GLSL source language to corresponding shader stage.
static ShaderStage SourceLangToShaderStage(
    SpvGenStage sourceLang) // GLSL source language
{
    static_assert(SpvGenStageVertex         == 0, "Unexpected value!");
    static_assert(SpvGenStageTessControl    == 1, "Unexpected value!");
    static_assert(SpvGenStageTessEvaluation == 2, "Unexpected value!");
    static_assert(SpvGenStageGeometry       == 3, "Unexpected value!");
    static_assert(SpvGenStageFragment       == 4, "Unexpected value!");
    static_assert(SpvGenStageCompute        == 5, "Unexpected value!");

    return static_cast<ShaderStage>(sourceLang);
}

// =====================================================================================================================
// Performs initialization work for LLPC standalone tool.
static Result Init(
    int32_t      argc,          // Count of arguments
    char*        argv[],        // [in] List of arguments
    ICompiler**  ppCompiler)    // [out] Created LLPC compiler object
{
    Result result = Result::Success;

    if (result == Result::Success)
    {
        // NOTE: For testing consistency, these options should be kept the same as those of Vulkan
        // ICD (Device::InitLlpcCompiler()). Here, we check the specified options from command line.
        // For each default option that is missing, we add it manually. This code to check whether
        // the same option has been specified is not completely foolproof because it does not know
        // which arguments are not option names.
        static const char* defaultOptions[] =
        {
            // Name                                Option
            "-gfxip",                              "-gfxip=8.0.0",
            "-unroll-max-percent-threshold-boost", "-unroll-max-percent-threshold-boost=1000",
            "-pragma-unroll-threshold",            "-pragma-unroll-threshold=1000",
            "-unroll-allow-partial",               "-unroll-allow-partial",
            "-simplifycfg-sink-common",            "-simplifycfg-sink-common=false",
            "-amdgpu-vgpr-index-mode",             "-amdgpu-vgpr-index-mode",         // Force VGPR indexing on GFX8
            "-amdgpu-atomic-optimizations",        "-amdgpu-atomic-optimizations",    // Enable atomic optimizations
            "-use-gpu-divergence-analysis",        "-use-gpu-divergence-analysis",    // Use new divergence analysis
            "-filetype",                           "-filetype=obj",   // Target = obj, ELF binary; target = asm, ISA assembly text
        };

        // Build new arguments, starting with those supplied in command line
        std::vector<const char*> newArgs;
        for (int32_t i = 0; i < argc; ++i)
        {
            newArgs.push_back(argv[i]);
        }

        static const size_t defaultOptionCount = sizeof(defaultOptions) / (2 * sizeof(defaultOptions[0]));
        for (uint32_t optionIdx = 0; optionIdx != defaultOptionCount; ++optionIdx)
        {
            const char* pName = defaultOptions[2 * optionIdx];
            const char* pOption = defaultOptions[2 * optionIdx + 1];
            size_t nameLen = strlen(pName);
            bool found = false;
            const char* pArg = nullptr;
            for (int32_t i = 1; i < argc; ++i)
            {
                pArg = argv[i];
                if ((strncmp(pArg, pName, nameLen) == 0) &&
                    ((pArg[nameLen] == '\0') || (pArg[nameLen] == '=') || (isdigit((int32_t)pArg[nameLen]))))
                {
                    found = true;
                    break;
                }
            }

            if (found == false)
            {
                newArgs.push_back(pOption);
            }
            else if (optionIdx == 0) // Find option -gfxip
            {
                size_t argLen = strlen(pArg);
                if ((argLen > nameLen) && pArg[nameLen] == '=')
                {
                    // Extract tokens of graphics IP version info (delimiter is ".")
                    const uint32_t len = argLen - nameLen - 1;
                    char* pGfxIp = new char[len + 1];
                    memcpy(pGfxIp, &pArg[nameLen + 1], len);
                    pGfxIp[len] = '\0';

                    char* tokens[3] = {}; // Format: major.minor.step
                    char* pToken = std::strtok(pGfxIp, ".");
                    for (uint32_t i = 0; (i < 3) && (pToken != nullptr); ++i)
                    {
                        tokens[i] = pToken;
                        pToken = std::strtok(nullptr, ".");
                    }

                    ParsedGfxIp.major    = (tokens[0] != nullptr) ? std::strtoul(tokens[0], nullptr, 10) : 0;
                    ParsedGfxIp.minor    = (tokens[1] != nullptr) ? std::strtoul(tokens[1], nullptr, 10) : 0;
                    ParsedGfxIp.stepping = (tokens[2] != nullptr) ? std::strtoul(tokens[2], nullptr, 10) : 0;

                    delete[] pGfxIp;
                }
            }
        }

        const char* pName = "-shader-cache-file-dir";
        size_t nameLen = strlen(pName);
        bool found = false;
        const char* pArg = nullptr;
        for (int32_t i = 1; i < argc; ++i)
        {
            pArg = argv[i];
            if ((strncmp(pArg, pName, nameLen) == 0) &&
                ((pArg[nameLen] == '\0') || (pArg[nameLen] == '=')))
            {
                found = true;
                break;
            }
        }

        if (found == false)
        {
            // Initialize the path for shader cache
            constexpr uint32_t MaxFilePathLen = 512;
            char               shaderCacheFileDirOption[MaxFilePathLen];

            // Initialize the root path of cache files
            // Steps:
            //   1. Find AMD_SHADER_DISK_CACHE_PATH to keep backward compatibility.
            const char* pEnvString = getenv("AMD_SHADER_DISK_CACHE_PATH");

#ifdef WIN_OS
            //   2. Find LOCALAPPDATA.
            if (pEnvString == nullptr)
            {
                pEnvString = getenv("LOCALAPPDATA");
            }
#else
            char shaderCacheFileRootDir[MaxFilePathLen];

            //   2. Find XDG_CACHE_HOME.
            //   3. If AMD_SHADER_DISK_CACHE_PATH and XDG_CACHE_HOME both not set,
            //      use "$HOME/.cache".
            if (pEnvString == nullptr)
            {
                pEnvString = getenv("XDG_CACHE_HOME");
            }

            if (pEnvString == nullptr)
            {
                pEnvString = getenv("HOME");
                if (pEnvString != nullptr)
                {
                    snprintf(shaderCacheFileRootDir, sizeof(shaderCacheFileRootDir),
                        "%s/.cache", pEnvString);
                    pEnvString = &shaderCacheFileRootDir[0];
                }
            }
#endif

            if (pEnvString != nullptr)
            {
                snprintf(shaderCacheFileDirOption, sizeof(shaderCacheFileDirOption),
                         "-shader-cache-file-dir=%s", pEnvString);
            }
            else
            {
                strncpy(shaderCacheFileDirOption, "-shader-cache-file-dir=.", sizeof(shaderCacheFileDirOption));
            }
            newArgs.push_back(shaderCacheFileDirOption);
        }

        // NOTE: We set the option -disable-null-frag-shader to TRUE for standalone compiler as the default.
        // Subsequent command option parse will correct its value if this option is specified externally.
        cl::DisableNullFragShader.setValue(true);

        result = ICompiler::Create(ParsedGfxIp, newArgs.size(), &newArgs[0], ppCompiler);
    }

    if ((result == Result::Success) && (SpvGenDir != ""))
    {
        // -spvgen-dir option: preload spvgen from the given directory
        if (InitSpvGen(SpvGenDir.c_str()) == false)
        {
            LLPC_ERRS("Failed to load SPVGEN from specified directory\n");
            result = Result::ErrorUnavailable;
        }
    }

    return result;
}

// =====================================================================================================================
// Performs per-pipeline initialization work for LLPC standalone tool.
static Result InitCompileInfo(
    CompileInfo* pCompileInfo)  // [out] Compilation info of LLPC standalone tool
{
    pCompileInfo->gfxIp = ParsedGfxIp;

    // Set NGG control settings
    if (ParsedGfxIp.major >= 10)
    {
        auto& nggState = pCompileInfo->gfxPipelineInfo.nggState;

        nggState.enableNgg                  = EnableNgg;
        nggState.enableGsUse                = NggEnableGsUse;
        nggState.forceNonPassthrough        = NggForceNonPassThrough;
        nggState.alwaysUsePrimShaderTable   = NggAlwaysUsePrimShaderTable;
        nggState.compactMode                = static_cast<NggCompactMode>(NggCompactionMode.getValue());
        nggState.enableFastLaunch           = NggEnableFastLaunchRate;
        nggState.enableVertexReuse          = NggEnableVertexReuse;
        nggState.enableBackfaceCulling      = NggEnableBackfaceCulling;
        nggState.enableFrustumCulling       = NggEnableFrustumCulling;
        nggState.enableBoxFilterCulling     = NggEnableBoxFilterCulling;
        nggState.enableSphereCulling        = NggEnableSphereCulling;
        nggState.enableSmallPrimFilter      = NggEnableSmallPrimFilter;
        nggState.enableCullDistanceCulling  = NggEnableCullDistanceCulling;

        nggState.backfaceExponent           = NggBackfaceExponent;
        nggState.subgroupSizing             = static_cast<NggSubgroupSizingType>(NggSubgroupSizing.getValue());
        nggState.primsPerSubgroup           = NggPrimsPerSubgroup;
        nggState.vertsPerSubgroup           = NggVertsPerSubgroup;
    }

    return Result::Success;
}

// =====================================================================================================================
// Performs cleanup work for LLPC standalone tool.
static void CleanupCompileInfo(
    CompileInfo* pCompileInfo)  // [in,out] Compilation info of LLPC standalone tool
{
    for (uint32_t i = 0; i < pCompileInfo->shaderModuleDatas.size(); ++i)
    {
        // NOTE: We do not have to free SPIR-V binary for pipeline info file.
        // It will be freed when we close the VFX doc.
        if (pCompileInfo->pPipelineInfoFile == nullptr)
        {
            delete[] reinterpret_cast<const char*>(pCompileInfo->shaderModuleDatas[i].spirvBin.pCode);
        }

        free(pCompileInfo->shaderModuleDatas[i].shaderBuf);
    }

    free(pCompileInfo->pPipelineBuf);

    if (pCompileInfo->pPipelineInfoFile)
    {
        Vfx::vfxCloseDoc(pCompileInfo->pPipelineInfoFile);
    }

    memset(pCompileInfo, 0, sizeof(*pCompileInfo));
}

// =====================================================================================================================
// Callback function to allocate buffer for building shader module and building pipeline.
void* VKAPI_CALL AllocateBuffer(
    void*  pInstance,   // [in] Dummy instance object, unused
    void*  pUserData,   // [in] User data
    size_t size)        // Requested allocation size
{
    void* pAllocBuf = malloc(size);
    memset(pAllocBuf, 0, size);

    void** ppOutBuf = reinterpret_cast<void**>(pUserData);
    *ppOutBuf = pAllocBuf;
    return pAllocBuf;
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPRI-V assembly text file (.spvas).
static bool IsSpirvTextFile(
    const std::string& fileName)
{
    bool isSpirvText = false;

    size_t extPos = fileName.find_last_of(".");
    std::string extName;
    if (extPos != std::string::npos)
    {
        extName = fileName.substr(extPos, fileName.size() - extPos);
    }

    if ((extName.empty() == false) && (extName == LlpcExt::SpirvText))
    {
        isSpirvText = true;
    }

    return isSpirvText;
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPRI-V binary file (.spv).
static bool IsSpirvBinaryFile(
    const std::string& fileName) // [in] File name to check
{
    bool isSpirvBin = false;

    size_t extPos = fileName.find_last_of(".");
    std::string extName;
    if (extPos != std::string::npos)
    {
        extName = fileName.substr(extPos, fileName.size() - extPos);
    }

    if ((extName.empty() == false) && (extName == LlpcExt::SpirvBin))
    {
        isSpirvBin = true;
    }

    return isSpirvBin;
}

// =====================================================================================================================
// Checks whether the specified file name represents a LLPC pipeline info file (.pipe).
static bool IsPipelineInfoFile(
    const std::string& fileName) // [in] File name to check
{
    bool isPipelineInfo = false;

    size_t extPos = fileName.find_last_of(".");
    std::string extName;
    if (extPos != std::string::npos)
    {
        extName = fileName.substr(extPos, fileName.size() - extPos);
    }

    if ((extName.empty() == false) && (extName == LlpcExt::PipelineInfo))
    {
        isPipelineInfo = true;
    }

    return isPipelineInfo;
}

// =====================================================================================================================
// Checks whether the specified file name represents a LLVM IR file (.ll).
static bool IsLlvmIrFile(
    const std::string& fileName) // [in] File name to check
{
    bool isLlvmIr = false;

    size_t extPos = fileName.find_last_of(".");
    std::string extName;
    if (extPos != std::string::npos)
    {
        extName = fileName.substr(extPos, fileName.size() - extPos);
    }

    if ((extName.empty() == false) && (extName == LlpcExt::LlvmIr))
    {
        isLlvmIr = true;
    }

    return isLlvmIr;
}

// =====================================================================================================================
// Gets SPIR-V binary codes from the specified binary file.
static Result GetSpirvBinaryFromFile(
    const std::string& spvBinFile,  // [in] SPIR-V binary file
    BinaryData*        pSpvBin)     // [out] SPIR-V binary codes
{
    Result result = Result::Success;

    FILE* pBinFile = fopen(spvBinFile.c_str(), "rb");
    if (pBinFile == nullptr)
    {
        LLPC_ERRS("Fails to open SPIR-V binary file: " << spvBinFile << "\n");
        result = Result::ErrorUnavailable;
    }

    if (result == Result::Success)
    {
        fseek(pBinFile, 0, SEEK_END);
        size_t binSize = ftell(pBinFile);
        fseek(pBinFile, 0, SEEK_SET);

        char* pBin= new char[binSize];
        LLPC_ASSERT(pBin != nullptr);
        memset(pBin, 0, binSize);
        binSize = fread(pBin, 1, binSize, pBinFile);

        pSpvBin->codeSize = binSize;
        pSpvBin->pCode    = pBin;

        fclose(pBinFile);
    }

    return result;
}

// =====================================================================================================================
// GLSL compiler, compiles GLSL source text file (input) to SPIR-V binary file (output).
static Result CompileGlsl(
    const std::string& inFile,      // [in] Input file, GLSL source text
    ShaderStage*       pStage,      // [out] Shader stage
    std::string&       outFile)     // [out] Output file, SPIR-V binary
{
    if (InitSpvGen() == false)
    {
        LLPC_ERRS("Failed to load SPVGEN -- cannot compile GLSL\n");
        return Result::ErrorUnavailable;
    }

    Result result = Result::Success;
    bool isHlsl = false;

    SpvGenStage lang = spvGetStageTypeFromName(inFile.c_str(), &isHlsl);
    if (lang == SpvGenStageInvalid)
    {
        LLPC_ERRS("File " << inFile << ": Bad file extension; try -help\n");
        return Result::ErrorInvalidShader;
    }
    *pStage = SourceLangToShaderStage(lang);

    FILE* pInFile = fopen(inFile.c_str(), "r");
    if (pInFile == nullptr)
    {
        LLPC_ERRS("Fails to open input file: " << inFile << "\n");
        result = Result::ErrorUnavailable;
    }

    FILE* pOutFile = nullptr;
    if (result == Result::Success)
    {
        outFile = sys::path::filename(inFile).str() + LlpcExt::SpirvBin;

        pOutFile = fopen(outFile.c_str(), "wb");
        if (pOutFile == nullptr)
        {
            LLPC_ERRS("Fails to open output file: " << outFile << "\n");
            result = Result::ErrorUnavailable;
        }
    }

    if (result == Result::Success)
    {
        fseek(pInFile, 0, SEEK_END);
        size_t textSize = ftell(pInFile);
        fseek(pInFile, 0, SEEK_SET);

        char* pGlslText = new char[textSize + 1];
        LLPC_ASSERT(pGlslText != nullptr);
        memset(pGlslText, 0, textSize + 1);
        auto readSize = fread(pGlslText, 1, textSize, pInFile);
        pGlslText[readSize] = 0;

        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// GLSL sources: " << inFile << "\n\n");
        LLPC_OUTS(pGlslText);
        LLPC_OUTS("\n\n");

        int32_t sourceStringCount = 1;
        const char*const* sourceList[1] = {};
        const char* pFileName = inFile.c_str();
        const char* const* fileList[1]  = { &pFileName };
        sourceList[0] = &pGlslText;

        void* pProgram = nullptr;
        const char* pLog = nullptr;
        int compileOption = SpvGenOptionDefaultDesktop | SpvGenOptionVulkanRules | SpvGenOptionDebug;
        compileOption |= isHlsl ? SpvGenOptionReadHlsl : 0;
        const char* entryPoints[] = { EntryTarget.c_str() };
        bool compileResult = spvCompileAndLinkProgramEx(1,
                                                        &lang,
                                                        &sourceStringCount,
                                                        sourceList,
                                                        fileList,
                                                        isHlsl ? entryPoints : nullptr,
                                                        &pProgram,
                                                        &pLog,
                                                        compileOption);

        LLPC_OUTS("// GLSL program compile/link log\n");

        if (compileResult)
        {
            const uint32_t* pSpvBin = nullptr;
            uint32_t binSize = spvGetSpirvBinaryFromProgram(pProgram, 0, &pSpvBin);
            fwrite(pSpvBin, 1, binSize, pOutFile);

            textSize = binSize * 10 + 1024;
            char* pSpvText = new char[textSize];
            LLPC_ASSERT(pSpvText != nullptr);
            memset(pSpvText, 0, textSize);
            LLPC_OUTS("\nSPIR-V disassembly: " << outFile << "\n");
            spvDisassembleSpirv(binSize, pSpvBin, textSize, pSpvText);
            LLPC_OUTS(pSpvText << "\n");
            delete[] pSpvText;
        }
        else
        {
            LLPC_ERRS("Fail to compile GLSL sources\n\n" << pLog << "\n");
            result = Result::ErrorInvalidShader;
        }

        delete[] pGlslText;

        fclose(pInFile);
        fclose(pOutFile);
    }

    return result;
}

// =====================================================================================================================
// SPIR-V assembler, converts SPIR-V assembly text file (input) to SPIR-V binary file (output).
static Result AssembleSpirv(
    const std::string& inFile,  // [in] Input file, SPIR-V assembly text
    std::string&       outFile) // [out] Output file, SPIR-V binary
{
    if (InitSpvGen() == false)
    {
        LLPC_ERRS("Failed to load SPVGEN -- cannot assemble SPIR-V assembler source\n");
        return Result::ErrorUnavailable;
    }

    Result result = Result::Success;

    FILE* pInFile = fopen(inFile.c_str(), "r");
    if (pInFile == nullptr)
    {
        LLPC_ERRS("Fails to open input file: " << inFile << "\n");
        result = Result::ErrorUnavailable;
    }

    FILE* pOutFile = nullptr;
    if (result == Result::Success)
    {
        outFile = sys::path::stem(sys::path::filename(inFile)).str() + LlpcExt::SpirvBin;

        pOutFile = fopen(outFile.c_str(), "wb");
        if (pOutFile == nullptr)
        {
            LLPC_ERRS("Fails to open output file: " << outFile << "\n");
            result = Result::ErrorUnavailable;
        }
    }

    if (result == Result::Success)
    {
        fseek(pInFile, 0, SEEK_END);
        size_t textSize = ftell(pInFile);
        fseek(pInFile, 0, SEEK_SET);

        char* pSpvText = new char[textSize + 1];
        LLPC_ASSERT(pSpvText != nullptr);
        memset(pSpvText, 0, textSize + 1);

        size_t realSize = fread(pSpvText, 1, textSize, pInFile);
        pSpvText[realSize] = '\0';

        int32_t binSize = realSize * 4 + 1024; // Estimated SPIR-V binary size
        uint32_t* pSpvBin = new uint32_t[binSize / sizeof(uint32_t)];
        LLPC_ASSERT(pSpvBin != nullptr);

        const char* pLog = nullptr;
        binSize = spvAssembleSpirv(pSpvText, binSize, pSpvBin, &pLog);
        if (binSize < 0)
        {
            LLPC_ERRS("Fails to assemble SPIR-V: \n" << pLog << "\n");
            result = Result::ErrorInvalidShader;
        }
        else
        {
            fwrite(pSpvBin, 1, binSize, pOutFile);

            LLPC_OUTS("===============================================================================\n");
            LLPC_OUTS("// SPIR-V disassembly: " << inFile << "\n");
            LLPC_OUTS(pSpvText);
            LLPC_OUTS("\n\n");
        }

        fclose(pInFile);
        fclose(pOutFile);

        delete[] pSpvText;
        delete[] pSpvBin;
    }

    return result;
}

// =====================================================================================================================
// Decodes the binary after building a pipeline and outputs the decoded info.
static Result DecodePipelineBinary(
    const BinaryData* pPipelineBin, // [in] Pipeline binary
    CompileInfo*      pCompileInfo, // [in,out] Compilation info of LLPC standalone tool
    bool              isGraphics)   // Whether it is graphics pipeline
{
    // Ignore failure from ElfReader. It fails if pPipelineBin is not ELF, as happens with
    // -filetype=asm.
    ElfReader<Elf64> reader(pCompileInfo->gfxIp);
    size_t readSize = 0;
    if (reader.ReadFromBuffer(pPipelineBin->pCode, &readSize) == Result::Success)
    {
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC final ELF info\n");
        LLPC_OUTS(reader);
    }

    return Result::Success;
}

// =====================================================================================================================
// Builds shader module based on the specified SPIR-V binary.
static Result BuildShaderModules(
    const ICompiler* pCompiler,     // [in] LLPC compiler object
    CompileInfo*     pCompileInfo)  // [in,out] Compilation info of LLPC standalone tool
{
    Result result = Result::Success;

    for (uint32_t i = 0; i < pCompileInfo->shaderModuleDatas.size(); ++i)
    {
        ShaderModuleBuildInfo* pShaderInfo = &(pCompileInfo->shaderModuleDatas[i].shaderInfo);
        ShaderModuleBuildOut* pShaderOut = &(pCompileInfo->shaderModuleDatas[i].shaderOut);

        pShaderInfo->pInstance = nullptr; // Dummy, unused
        pShaderInfo->pUserData = &(pCompileInfo->shaderModuleDatas[i].shaderBuf);
        pShaderInfo->pfnOutputAlloc = AllocateBuffer;
        pShaderInfo->shaderBin = pCompileInfo->shaderModuleDatas[i].spirvBin;

        result = pCompiler->BuildShaderModule(pShaderInfo, pShaderOut);
        if ((result != Result::Success) && (result != Result::Delayed))
        {
            LLPC_ERRS("Fails to build "
                << GetShaderStageName(pCompileInfo->shaderModuleDatas[i].shaderStage)
                << " shader module:\n");
            break;
        }
    }

    return result;
}

// =====================================================================================================================
// Check autolayout compatible.
static Result CheckAutoLayoutCompatibleFunc(
    const ICompiler*    pCompiler,          // [in] LLPC compiler object
    CompileInfo*        pCompileInfo)       // [in,out] Compilation info of LLPC standalone tool
{
    Result result = Result::Success;

    bool isGraphics = (pCompileInfo->stageMask & (ShaderStageToMask(ShaderStageCompute) -1)) ? true : false;
    if (isGraphics)
    {
        // Build graphics pipeline
        GraphicsPipelineBuildInfo* pPipelineInfo = &pCompileInfo->gfxPipelineInfo;

        // Fill pipeline shader info
        PipelineShaderInfo* shaderInfo[ShaderStageGfxCount] =
        {
            &pPipelineInfo->vs,
            &pPipelineInfo->tcs,
            &pPipelineInfo->tes,
            &pPipelineInfo->gs,
            &pPipelineInfo->fs,
        };

        uint32_t userDataOffset = 0;
        for (uint32_t i = 0; i < pCompileInfo->shaderModuleDatas.size(); ++i)
        {

            PipelineShaderInfo*         pShaderInfo = shaderInfo[pCompileInfo->shaderModuleDatas[i].shaderStage];
            bool                        checkAutoLayoutCompatible = pCompileInfo->checkAutoLayoutCompatible;

            if (pCompileInfo->shaderModuleDatas[i].shaderStage != Llpc::ShaderStageFragment)
            {
                checkAutoLayoutCompatible = false;
            }
            const ShaderModuleBuildOut* pShaderOut  = &(pCompileInfo->shaderModuleDatas[i].shaderOut);

            if (pShaderInfo->pEntryTarget == nullptr)
            {
                // If entry target is not specified, use the one from command line option
                pShaderInfo->pEntryTarget = EntryTarget.c_str();
            }
            pShaderInfo->pModuleData  = pShaderOut->pModuleData;
            pShaderInfo->entryStage = pCompileInfo->shaderModuleDatas[i].shaderStage;
            if (checkAutoLayoutCompatible)
            {
                PipelineShaderInfo shaderInfo = *pShaderInfo;
                GraphicsPipelineBuildInfo pipelineInfo = *pPipelineInfo;
                DoAutoLayoutDesc(pCompileInfo->shaderModuleDatas[i].shaderStage,
                                 pCompileInfo->shaderModuleDatas[i].spirvBin,
                                 &pipelineInfo,
                                 &shaderInfo,
                                 userDataOffset,
                                 true);
                if (CheckShaderInfoComptible(pShaderInfo, shaderInfo.userDataNodeCount, shaderInfo.pUserDataNodes) &&
                    CheckPipelineStateCompatible(pCompiler, pPipelineInfo, &pipelineInfo, ParsedGfxIp))
                {
                    outs() << "Auto Layout fragment shader in " << pCompileInfo->pFileNames << " hitted\n";
                }
                else
                {
                    outs() << "Auto Layout fragment shader in " << pCompileInfo->pFileNames << " failed to hit\n";
                }
                outs().flush();
            }
        }
    }
    else if (pCompileInfo->stageMask == ShaderStageToMask(ShaderStageCompute))
    {
        ComputePipelineBuildInfo* pPipelineInfo = &pCompileInfo->compPipelineInfo;

        PipelineShaderInfo*         pShaderInfo = &pPipelineInfo->cs;
        const ShaderModuleBuildOut* pShaderOut  = &pCompileInfo->shaderModuleDatas[0].shaderOut;

        if (pShaderInfo->pEntryTarget == nullptr)
        {
            // If entry target is not specified, use the one from command line option
            pShaderInfo->pEntryTarget = EntryTarget.c_str();
        }
        pShaderInfo->entryStage = ShaderStageCompute;
        pShaderInfo->pModuleData  = pShaderOut->pModuleData;

        if (pCompileInfo->checkAutoLayoutCompatible)
        {
            uint32_t userDataOffset = 0;
            PipelineShaderInfo shaderInfo = *pShaderInfo;
            DoAutoLayoutDesc(ShaderStageCompute,
                             pCompileInfo->shaderModuleDatas[0].spirvBin,
                             nullptr,
                             &shaderInfo,
                             userDataOffset,
                             true);
            if (CheckShaderInfoComptible(pShaderInfo, shaderInfo.userDataNodeCount, shaderInfo.pUserDataNodes))
            {
                outs() << "Auto Layout compute shader in " << pCompileInfo->pFileNames << " hitted\n";
            }
            else
            {
                outs() << "Auto Layout compute shader in " << pCompileInfo->pFileNames << " failed to hit\n";
            }
            outs().flush();
        }
    }

    return result;
}

// =====================================================================================================================
// Builds pipeline and do linking.
static Result BuildPipeline(
    ICompiler*    pCompiler,        // [in] LLPC compiler object
    CompileInfo*  pCompileInfo)     // [in,out] Compilation info of LLPC standalone tool
{
    Result result = Result::Success;

    bool isGraphics = (pCompileInfo->stageMask & (ShaderStageToMask(ShaderStageCompute) -1)) ? true : false;
    if (isGraphics)
    {
        // Build graphics pipeline
        GraphicsPipelineBuildInfo* pPipelineInfo = &pCompileInfo->gfxPipelineInfo;
        GraphicsPipelineBuildOut*  pPipelineOut  = &pCompileInfo->gfxPipelineOut;

        // Fill pipeline shader info
        PipelineShaderInfo* shaderInfo[ShaderStageGfxCount] =
        {
            &pPipelineInfo->vs,
            &pPipelineInfo->tcs,
            &pPipelineInfo->tes,
            &pPipelineInfo->gs,
            &pPipelineInfo->fs,
        };

        uint32_t userDataOffset = 0;
        for (uint32_t i = 0; i < pCompileInfo->shaderModuleDatas.size(); ++i)
        {

            PipelineShaderInfo*         pShaderInfo = shaderInfo[pCompileInfo->shaderModuleDatas[i].shaderStage];
            const ShaderModuleBuildOut* pShaderOut  = &(pCompileInfo->shaderModuleDatas[i].shaderOut);

            if (pShaderInfo->pEntryTarget == nullptr)
            {
                // If entry target is not specified, use the one from command line option
                pShaderInfo->pEntryTarget = EntryTarget.c_str();
            }
            pShaderInfo->pModuleData  = pShaderOut->pModuleData;
            pShaderInfo->entryStage = pCompileInfo->shaderModuleDatas[i].shaderStage;

            // If not compiling from pipeline, lay out user data now.
            if (pCompileInfo->doAutoLayout)
            {
                DoAutoLayoutDesc(pCompileInfo->shaderModuleDatas[i].shaderStage,
                                    pCompileInfo->shaderModuleDatas[i].spirvBin,
                                    pPipelineInfo,
                                    pShaderInfo,
                                    userDataOffset,
                                    false);
            }
        }

        pPipelineInfo->pInstance      = nullptr; // Dummy, unused
        pPipelineInfo->pUserData      = &pCompileInfo->pPipelineBuf;
        pPipelineInfo->pfnOutputAlloc = AllocateBuffer;

        // NOTE: If number of patch control points is not specified, we set it to 3.
        if (pPipelineInfo->iaState.patchControlPoints == 0)
        {
            pPipelineInfo->iaState.patchControlPoints = 3;
        }

        pPipelineInfo->options.robustBufferAccess = RobustBufferAccess;

        void* pPipelineDumpHandle = nullptr;
        if (llvm::cl::EnablePipelineDump)
        {
            PipelineDumpOptions dumpOptions = {};
            dumpOptions.pDumpDir                 = llvm::cl::PipelineDumpDir.c_str();
            dumpOptions.filterPipelineDumpByType = llvm::cl::FilterPipelineDumpByType;
            dumpOptions.filterPipelineDumpByHash = llvm::cl::FilterPipelineDumpByHash;
            dumpOptions.dumpDuplicatePipelines   = llvm::cl::DumpDuplicatePipelines;

            PipelineBuildInfo pipelineInfo = {};
            pipelineInfo.pGraphicsInfo = pPipelineInfo;
            pPipelineDumpHandle = Llpc::IPipelineDumper::BeginPipelineDump(&dumpOptions, pipelineInfo);
        }

        if (TimePassesIsEnabled || cl::EnableTimerProfile)
        {
            auto hash = Llpc::IPipelineDumper::GetPipelineHash(pPipelineInfo);
            outs() << "LLPC PipelineHash: " << format("0x%016" PRIX64, hash)
                   << " Files: " << pCompileInfo->pFileNames << "\n";
            outs().flush();
        }

        result = pCompiler->BuildGraphicsPipeline(pPipelineInfo, pPipelineOut, pPipelineDumpHandle);

        if (result == Result::Success)
        {
            if (llvm::cl::EnablePipelineDump)
            {
                Llpc::BinaryData pipelineBinary = {};
                pipelineBinary.codeSize = pPipelineOut->pipelineBin.codeSize;
                pipelineBinary.pCode = pPipelineOut->pipelineBin.pCode;
                Llpc::IPipelineDumper::DumpPipelineBinary(pPipelineDumpHandle, ParsedGfxIp, &pipelineBinary);

                Llpc::IPipelineDumper::EndPipelineDump(pPipelineDumpHandle);
            }

            result = DecodePipelineBinary(&pPipelineOut->pipelineBin, pCompileInfo, true);
        }
    }
    else
    {
        // Build compute pipeline
        LLPC_ASSERT(pCompileInfo->shaderModuleDatas.size() == 1);
        LLPC_ASSERT(pCompileInfo->shaderModuleDatas[0].shaderStage == ShaderStageCompute);

        ComputePipelineBuildInfo* pPipelineInfo = &pCompileInfo->compPipelineInfo;
        ComputePipelineBuildOut*  pPipelineOut  = &pCompileInfo->compPipelineOut;

        PipelineShaderInfo*         pShaderInfo = &pPipelineInfo->cs;
        const ShaderModuleBuildOut* pShaderOut  = &pCompileInfo->shaderModuleDatas[0].shaderOut;

        if (pShaderInfo->pEntryTarget == nullptr)
        {
            // If entry target is not specified, use the one from command line option
            pShaderInfo->pEntryTarget = EntryTarget.c_str();
        }

        pShaderInfo->entryStage = ShaderStageCompute;
        pShaderInfo->pModuleData  = pShaderOut->pModuleData;

        // If not compiling from pipeline, lay out user data now.
        if (pCompileInfo->doAutoLayout)
        {
            uint32_t userDataOffset = 0;
            DoAutoLayoutDesc(ShaderStageCompute,
                             pCompileInfo->shaderModuleDatas[0].spirvBin,
                             nullptr,
                             pShaderInfo,
                             userDataOffset,
                             false);
        }

        pPipelineInfo->pInstance      = nullptr; // Dummy, unused
        pPipelineInfo->pUserData      = &pCompileInfo->pPipelineBuf;
        pPipelineInfo->pfnOutputAlloc = AllocateBuffer;
        pPipelineInfo->options.robustBufferAccess = RobustBufferAccess;

        void* pPipelineDumpHandle = nullptr;
        if (llvm::cl::EnablePipelineDump)
        {
            PipelineDumpOptions dumpOptions = {};
            dumpOptions.pDumpDir                 = llvm::cl::PipelineDumpDir.c_str();
            dumpOptions.filterPipelineDumpByType = llvm::cl::FilterPipelineDumpByType;
            dumpOptions.filterPipelineDumpByHash = llvm::cl::FilterPipelineDumpByHash;
            dumpOptions.dumpDuplicatePipelines   = llvm::cl::DumpDuplicatePipelines;
            PipelineBuildInfo pipelineInfo = {};
            pipelineInfo.pComputeInfo = pPipelineInfo;
            pPipelineDumpHandle = Llpc::IPipelineDumper::BeginPipelineDump(&dumpOptions, pipelineInfo);
        }

        if (TimePassesIsEnabled || cl::EnableTimerProfile)
        {
            auto hash = Llpc::IPipelineDumper::GetPipelineHash(pPipelineInfo);
            outs() << "LLPC PipelineHash: " << format("0x%016" PRIX64, hash)
                   << " Files: " << pCompileInfo->pFileNames << "\n";
            outs().flush();
        }

        result = pCompiler->BuildComputePipeline(pPipelineInfo, pPipelineOut, pPipelineDumpHandle);

        if (result == Result::Success)
        {
            if (llvm::cl::EnablePipelineDump)
            {
                Llpc::BinaryData pipelineBinary = {};
                pipelineBinary.codeSize = pPipelineOut->pipelineBin.codeSize;
                pipelineBinary.pCode = pPipelineOut->pipelineBin.pCode;
                Llpc::IPipelineDumper::DumpPipelineBinary(pPipelineDumpHandle, ParsedGfxIp, &pipelineBinary);

                Llpc::IPipelineDumper::EndPipelineDump(pPipelineDumpHandle);
            }

            result = DecodePipelineBinary(&pPipelineOut->pipelineBin, pCompileInfo, false);
        }
    }

    return result;
}

// =====================================================================================================================
// Output LLPC resulting binary (ELF binary, ISA assembly text, or LLVM bitcode) to the specified target file.
static Result OutputElf(
    CompileInfo*       pCompileInfo,  // [in] Compilation info of LLPC standalone tool
    const std::string& outFile,       // [in] Name of the file to output ELF binary (specify "" to use base name of
                                      //     first input file with appropriate extension; specify "-" to use stdout)
    StringRef          firstInFile)   // [in] Name of first input file
{
    Result result = Result::Success;
    const BinaryData* pPipelineBin = (pCompileInfo->stageMask & ShaderStageToMask(ShaderStageCompute)) ?
                                         &pCompileInfo->compPipelineOut.pipelineBin :
                                         &pCompileInfo->gfxPipelineOut.pipelineBin;
    SmallString<64> outFileName(outFile);
    if (outFileName.empty())
    {
        // NOTE: The output file name was not specified, so we construct a default file name.  We detect the
        // output file type and determine the file extension according to it. We are unable to access the
        // values of the options "-filetype" and "-emit-llvm".
        const char* pExt = ".s";
        if (IsElfBinary(pPipelineBin->pCode, pPipelineBin->codeSize))
        {
            pExt = ".elf";
        }
        else if (IsLlvmBitcode(pPipelineBin->pCode, pPipelineBin->codeSize))
        {
            pExt = ".bc";
        }
        else if (IsIsaText(pPipelineBin->pCode, pPipelineBin->codeSize))
        {
            pExt = ".s";
        }
        else
        {
            pExt = ".ll";
        }
        outFileName = sys::path::filename(firstInFile);
        sys::path::replace_extension(outFileName, pExt);
    }

    FILE* pOutFile = stdout;
    if (outFileName != "-")
    {
        pOutFile = fopen(outFileName.c_str(), "wb");
    }

    if (pOutFile == nullptr)
    {
        LLPC_ERRS("Failed to open output file: " << outFileName << "\n");
        result = Result::ErrorUnavailable;
    }

    if (result == Result::Success)
    {
        if (fwrite(pPipelineBin->pCode, 1, pPipelineBin->codeSize, pOutFile) != pPipelineBin->codeSize)
        {
            result = Result::ErrorUnavailable;
        }

        if ((pOutFile != stdout) && (fclose(pOutFile) != 0))
        {
            result = Result::ErrorUnavailable;
        }

        if (result != Result::Success)
        {
            LLPC_ERRS("Failed to write output file: " << outFileName << "\n");
        }
    }
    return result;
}

#ifdef WIN_OS
// =====================================================================================================================
// Callback function for SIGABRT.
extern "C" void LlpcSignalAbortHandler(
    int signal)  // Signal type
{
    if (signal == SIGABRT)
    {
        RedirectLogOutput(true, 0, nullptr); // Restore redirecting to show crash in console window
        LLVM_BUILTIN_TRAP;
    }
}
#endif

#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
// =====================================================================================================================
// Enable VC run-time based memory leak detection.
static void EnableMemoryLeakDetection()
{
   // Retrieve the state of CRT debug reporting:
   int32_t dbgFlag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG );

   // Append custom flags to enable memory leak checks:
   dbgFlag |= _CRTDBG_LEAK_CHECK_DF;
   dbgFlag |= _CRTDBG_ALLOC_MEM_DF;

   // Update the run-time settings:
   _CrtSetDbgFlag (dbgFlag);
}
#endif

// =====================================================================================================================
// Process one pipeline.
static Result ProcessPipeline(
    ICompiler*            pCompiler,   // [in] LLPC context
    ArrayRef<std::string> inFiles,     // Input filename(s)
    uint32_t              startFile,   // Index of the starting file name being processed in the file name array
    uint32_t*             pNextFile)   // [out] Index of next file name being processed in the file name array
{
    Result result = Result::Success;
    CompileInfo compileInfo = {};
    std::string fileNames;
    compileInfo.doAutoLayout = true;
    compileInfo.checkAutoLayoutCompatible = CheckAutoLayoutCompatible;

    result = InitCompileInfo(&compileInfo);

    //
    // Translate sources to SPIR-V binary
    //
    for (uint32_t i = startFile; (i < inFiles.size()) && (result == Result::Success); ++i)
    {
        const std::string& inFile = inFiles[i];
        std::string spvBinFile;

        if (IsSpirvTextFile(inFile) || IsSpirvBinaryFile(inFile))
        {
            // SPIR-V assembly text or SPIR-V binary
            if (IsSpirvTextFile(inFile))
            {
                result = AssembleSpirv(inFile, spvBinFile);
            }
            else
            {
                spvBinFile = inFile;
            }

            BinaryData spvBin = {};

            if (result == Result::Success)
            {
                result = GetSpirvBinaryFromFile(spvBinFile, &spvBin);

                if (result == Result::Success)
                {
                    if (InitSpvGen() == false)
                    {
                        LLPC_OUTS("Failed to load SPVGEN -- no SPIR-V disassembler available\n");
                    }
                    else
                    {
                        // Disassemble SPIR-V code
                        uint32_t textSize = spvBin.codeSize * 10 + 1024;
                        char* pSpvText = new char[textSize];
                        LLPC_ASSERT(pSpvText != nullptr);
                        memset(pSpvText, 0, textSize);

                        LLPC_OUTS("\nSPIR-V disassembly for " << inFile << "\n");
                        spvDisassembleSpirv(spvBin.codeSize, spvBin.pCode, textSize, pSpvText);
                        LLPC_OUTS(pSpvText << "\n");

                        delete[] pSpvText;
                    }
                }
            }

            if ((result == Result::Success) && Validate)
            {
                char log[1024] = {};
                if (InitSpvGen() == false)
                {
                    errs() << "Warning: Failed to load SPVGEN -- cannot validate SPIR-V\n";
                }
                else
                {
                    if (spvValidateSpirv(spvBin.codeSize, spvBin.pCode, sizeof(log), log) == false)
                    {
                        LLPC_ERRS("Fails to validate SPIR-V: \n" << log << "\n");
                        result = Result::ErrorInvalidShader;
                    }
                }
            }

            if (result == Result::Success)
            {
                // NOTE: If the entry target is not specified, we set it to the one gotten from SPIR-V binary.
                if (EntryTarget.empty())
                {
                    EntryTarget.setValue(ShaderModuleHelper::GetEntryPointNameFromSpirvBinary(&spvBin));
                }

                uint32_t stageMask = ShaderModuleHelper::GetStageMaskFromSpirvBinary(&spvBin, EntryTarget.c_str());

                if ((stageMask & compileInfo.stageMask) != 0)
                {
                    break;
                }
                else if (stageMask != 0)
                {
                    for (uint32_t stage = ShaderStageVertex; stage < ShaderStageCount; ++stage)
                    {
                        if (stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage)))
                        {
                            ::ShaderModuleData shaderModuleData = {};
                            shaderModuleData.shaderStage = static_cast<ShaderStage>(stage);
                            shaderModuleData.spirvBin = spvBin;
                            compileInfo.shaderModuleDatas.push_back(shaderModuleData);
                            compileInfo.stageMask |= ShaderStageToMask(static_cast<ShaderStage>(stage));
                            break;
                        }
                    }
                }
                else
                {
                    LLPC_ERRS(format("Fails to identify shader stages by entry-point \"%s\"\n", EntryTarget.c_str()));
                    result = Result::ErrorUnavailable;
                }
            }

        }
        else if (IsPipelineInfoFile(inFile))
        {
            // NOTE: If the input file is pipeline file, we set the option -disable-null-frag-shader to FALSE
            // unconditionally.
            cl::DisableNullFragShader.setValue(false);

            const char* pLog = nullptr;
            bool vfxResult = Vfx::vfxParseFile(inFile.c_str(),
                                               0,
                                               nullptr,
                                               VfxDocTypePipeline,
                                               &compileInfo.pPipelineInfoFile,
                                               &pLog);
            if (vfxResult)
            {
                VfxPipelineStatePtr pPipelineState = nullptr;
                Vfx::vfxGetPipelineDoc(compileInfo.pPipelineInfoFile, &pPipelineState);

                if (pPipelineState->version != Llpc::Version)
                {
                    LLPC_ERRS("Version incompatible, SPVGEN::Version = " << pPipelineState->version <<
                              " AMDLLPC::Version = " << Llpc::Version << "\n");
                    result = Result::ErrorInvalidShader;
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// Pipeline file info for " << inFile << " \n\n");

                    if ((pLog != nullptr) && (strlen(pLog) > 0))
                    {
                        LLPC_OUTS("Pipeline file parse warning:\n" << pLog << "\n");
                    }

                    compileInfo.compPipelineInfo = pPipelineState->compPipelineInfo;
                    compileInfo.gfxPipelineInfo = pPipelineState->gfxPipelineInfo;
                    if (IgnoreColorAttachmentFormats)
                    {
                        // NOTE: When this option is enabled, we set color attachment format to
                        // R8G8B8A8_SRGB for color target 0. Also, for other color targets, if the
                        // formats are not UNDEFINED, we set them to R8G8B8A8_SRGB as well.
                        for (uint32_t target = 0; target < MaxColorTargets; ++target)
                        {
                            if ((target == 0) ||
                                (compileInfo.gfxPipelineInfo.cbState.target[target].format != VK_FORMAT_UNDEFINED))
                            {
                                compileInfo.gfxPipelineInfo.cbState.target[target].format = VK_FORMAT_R8G8B8A8_SRGB;
                            }
                        }
                    }

                    if (EnableOuts() && (InitSpvGen() == false))
                    {
                        LLPC_OUTS("Failed to load SPVGEN -- cannot disassemble and validate SPIR-V\n");
                    }

                    for (uint32_t stage = 0; stage < pPipelineState->numStages; ++stage)
                    {
                        if (pPipelineState->stages[stage].dataSize > 0)
                        {
                            ::ShaderModuleData shaderModuleData = {};
                            shaderModuleData.spirvBin.codeSize = pPipelineState->stages[stage].dataSize;
                            shaderModuleData.spirvBin.pCode = pPipelineState->stages[stage].pData;
                            shaderModuleData.shaderStage = pPipelineState->stages[stage].stage;

                            compileInfo.shaderModuleDatas.push_back(shaderModuleData);
                            compileInfo.stageMask |= ShaderStageToMask(pPipelineState->stages[stage].stage);

                            if (spvDisassembleSpirv != nullptr)
                            {
                                uint32_t binSize =  pPipelineState->stages[stage].dataSize;
                                uint32_t textSize = binSize * 10 + 1024;
                                char* pSpvText = new char[textSize];
                                LLPC_ASSERT(pSpvText != nullptr);
                                memset(pSpvText, 0, textSize);
                                LLPC_OUTS("\nSPIR-V disassembly for " <<
                                          GetShaderStageName(pPipelineState->stages[stage].stage) << " shader module:\n");
                                spvDisassembleSpirv(binSize, shaderModuleData.spirvBin.pCode, textSize, pSpvText);
                                LLPC_OUTS(pSpvText << "\n");
                                delete[] pSpvText;
                            }
                        }
                    }

                    bool isGraphics = (compileInfo.stageMask & ShaderStageToMask(ShaderStageCompute)) ? false : true;
                    for (uint32_t i = 0; i < compileInfo.shaderModuleDatas.size(); ++i)
                    {
                        compileInfo.shaderModuleDatas[i].shaderInfo.options.pipelineOptions = isGraphics ?
                                                                            compileInfo.gfxPipelineInfo.options :
                                                                            compileInfo.compPipelineInfo.options;
                    }

                    fileNames += inFile;
                    fileNames += " ";
                    *pNextFile = i + 1;
                    compileInfo.doAutoLayout = false;
                    break;
                }
            }
            else
            {
                 LLPC_ERRS("Failed to parse input file: " << inFile << "\n" << pLog << "\n");
                 result = Result::ErrorInvalidShader;
            }
        }
        else if (IsLlvmIrFile(inFile))
        {
            LLVMContext context;
            SMDiagnostic errDiag;

            // Load LLVM IR
            std::unique_ptr<Module> pModule =
                parseAssemblyFile(inFile, errDiag, context, nullptr, false);
            if (pModule.get() == nullptr)
            {
                std::string errMsg;
                raw_string_ostream errStream(errMsg);
                errDiag.print(inFile.c_str(), errStream);
                LLPC_ERRS(errMsg);
                result = Result::ErrorInvalidShader;
            }

            // Verify LLVM module
            std::string errMsg;
            raw_string_ostream errStream(errMsg);
            if ((result == Result::Success) && verifyModule(*pModule.get(), &errStream))
            {
                LLPC_ERRS("File " << inFile << " parsed, but fail to verify the module: " << errMsg << "\n");
                result = Result::ErrorInvalidShader;
            }

            // Check the shader stage of input module
            ShaderStage shaderStage = ShaderStageInvalid;
            if (result == Result::Success)
            {
                shaderStage = GetShaderStageFromModule(pModule.get());
                if (shaderStage == ShaderStageInvalid)
                {
                    LLPC_ERRS("File " << inFile << ": Fail to determine shader stage\n");
                    result = Result::ErrorInvalidShader;
                }

                if (compileInfo.stageMask & ShaderStageToMask(static_cast<ShaderStage>(shaderStage)))
                {
                    break;
                }
            }

            if (result == Result::Success)
            {
                // Translate LLVM module to LLVM bitcode
                llvm::SmallString<1024> bitcodeBuf;
                raw_svector_ostream bitcodeStream(bitcodeBuf);
                WriteBitcodeToFile(*pModule.get(), bitcodeStream);
                void* pCode = new uint8_t[bitcodeBuf.size()];
                memcpy(pCode, bitcodeBuf.data(), bitcodeBuf.size());

                ::ShaderModuleData shaderModuledata = {};
                shaderModuledata.spirvBin.codeSize = bitcodeBuf.size();
                shaderModuledata.spirvBin.pCode = pCode;
                shaderModuledata.shaderStage = shaderStage;
                compileInfo.shaderModuleDatas.push_back(shaderModuledata);
                compileInfo.stageMask |= ShaderStageToMask(static_cast<ShaderStage>(shaderStage));
                compileInfo.doAutoLayout = false;
            }
        }
        else
        {
            // GLSL source text

            // NOTE: If the entry target is not specified, we set it to GLSL default ("main").
            if (EntryTarget.empty())
            {
                EntryTarget.setValue("main");
            }

            ShaderStage stage = ShaderStageInvalid;
            result = CompileGlsl(inFile, &stage, spvBinFile);
            if (result == Result::Success)
            {
                if (compileInfo.stageMask & ShaderStageToMask(static_cast<ShaderStage>(stage)))
                {
                    break;
                }

                compileInfo.stageMask |= ShaderStageToMask(stage);
                ::ShaderModuleData shaderModuleData = {};
                result = GetSpirvBinaryFromFile(spvBinFile, &shaderModuleData.spirvBin);
                shaderModuleData.shaderStage = stage;
                compileInfo.shaderModuleDatas.push_back(shaderModuleData);
            }
        }

        fileNames += inFile;
        fileNames += " ";
        *pNextFile = i + 1;
    }

    if ((result == Result::Success) && (compileInfo.checkAutoLayoutCompatible == true))
    {
        compileInfo.pFileNames = fileNames.c_str();
        result = CheckAutoLayoutCompatibleFunc(pCompiler, &compileInfo);
    }
    else
    {
        //
        // Build shader modules
        //
        if ((result == Result::Success) && (compileInfo.stageMask != 0))
        {
            result = BuildShaderModules(pCompiler, &compileInfo);
        }

        //
        // Build pipeline
        //
        if ((result == Result::Success) && ToLink)
        {
            compileInfo.pFileNames = fileNames.c_str();
            result = BuildPipeline(pCompiler, &compileInfo);
            if (result == Result::Success)
            {
                result = OutputElf(&compileInfo, OutFile, inFiles[0]);
            }
        }
    }
    //
    // Clean up
    //
    CleanupCompileInfo(&compileInfo);

    return result;
}

#ifdef WIN_OS
// =====================================================================================================================
// Finds all filenames which can match input file name
void FindAllMatchFiles(
    const std::string&        inFile,         // [in] Input file name, include wildcard
    std::vector<std::string>* pOutFiles)      // [out] Output file names which can match input file name
{
    WIN32_FIND_DATAA data = {};

    // Separate folder name
    std::string folderName;
    auto separatorPos = inFile.find_last_of("/\\");
    if (separatorPos != std::string::npos)
    {
        folderName = inFile.substr(0, separatorPos + 1);
    }

    // Search first file
    HANDLE pSearchHandle = FindFirstFileA(inFile.c_str(), &data);
    if (pSearchHandle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    // Copy first file's name
    pOutFiles->push_back(folderName + data.cFileName);

    // Copy other file names
    while (FindNextFileA(pSearchHandle, &data))
    {
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
int32_t main(
    int32_t argc,       // Count of arguments
    char*   argv[])     // [in] List of arguments
{
    Result result = Result::Success;

    ICompiler*  pCompiler   = nullptr;

    //
    // Initialization
    //

    // TODO: CRT based Memory leak detection is conflict with stack trace now, we only can enable one of them.
#if defined(LLPC_MEM_TRACK_LEAK) && defined(_DEBUG)
    EnableMemoryLeakDetection();
#else
    EnablePrettyStackTrace();
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram X(argc, argv);

#ifdef WIN_OS
    signal(SIGABRT, LlpcSignalAbortHandler);
#endif
#endif

    result = Init(argc, argv, &pCompiler);

#ifdef WIN_OS
    if (AssertToMsgBox)
    {
        _set_error_mode(_OUT_TO_MSGBOX);
    }
#endif

    if (IsPipelineInfoFile(InFiles[0]) || IsLlvmIrFile(InFiles[0]))
    {
        uint32_t nextFile = 0;

        // The first input file is a pipeline file or LLVM IR file. Assume they all are, and compile each one
        // separately but in the same context.
        for (uint32_t i = 0; (i < InFiles.size()) && (result == Result::Success); ++i)
        {
#ifdef WIN_OS
            if (InFiles[i].find_last_of("*?") != std::string::npos)
            {
                std::vector<std::string> matchFiles;
                FindAllMatchFiles(InFiles[i], &matchFiles);
                if (matchFiles.size() == 0)
                {
                    LLPC_ERRS("\nFailed to read file " << InFiles[i] << "\n");
                    result = Result::ErrorInvalidValue;
                }
                else
                {
                    for (uint32_t j = 0; (j < matchFiles.size()) && (result == Result::Success); ++j)
                    {
                        result = ProcessPipeline(pCompiler, matchFiles[j], 0, &nextFile);
                    }
                }
            }
            else
#endif
            {
                result = ProcessPipeline(pCompiler, InFiles[i], 0, &nextFile);
            }
        }
    }
    else if (result == Result::Success)
    {
        // Otherwise, join all input files into the same pipeline.
#ifdef WIN_OS
        if ((InFiles.size() == 1) &&
            (InFiles[0].find_last_of("*?") != std::string::npos))
        {
            std::vector<std::string> matchFiles;
            FindAllMatchFiles(InFiles[0], &matchFiles);
            if (matchFiles.size() == 0)
            {
                LLPC_ERRS("\nFailed to read file " << InFiles[0] << "\n");
                result = Result::ErrorInvalidValue;
            }
            else
            {
                uint32_t nextFile = 0;
                for (uint32_t i = 0; (i < matchFiles.size()) && (result == Result::Success); ++i)
                {
                    result = ProcessPipeline(pCompiler, matchFiles[i], 0, &nextFile);
                }
            }
        }
        else
#endif
        {
            SmallVector<std::string, 6> inFiles;
            for (const auto& inFile : InFiles)
            {
#ifdef WIN_OS
                if (InFiles[0].find_last_of("*?") != std::string::npos)
                {
                    LLPC_ERRS("\nCan't use wilecard if multiple filename is set in command\n");
                    result = Result::ErrorInvalidValue;
                    break;
                }
#endif
                inFiles.push_back(inFile);
            }

            if (result == Result::Success)
            {
                uint32_t nextFile = 0;
                for (; result == Result::Success && nextFile < inFiles.size();)
                {
                    result = ProcessPipeline(pCompiler, inFiles, nextFile, &nextFile);
                }
            }
        }
    }

    pCompiler->Destroy();

    if (result == Result::Success)
    {
        LLPC_OUTS("\n=====  AMDLLPC SUCCESS  =====\n");
    }
    else
    {
        LLPC_ERRS("\n=====  AMDLLPC FAILED  =====\n");
    }

    return (result == Result::Success) ? 0 : 1;
}
