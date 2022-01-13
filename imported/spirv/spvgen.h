/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2015-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  spvgen.h
 * @brief SPVGEN header file: contains the definition and the wrap implementation of SPIR-V generator entry-points.
 ***********************************************************************************************************************
 */
// clang-format off
#pragma once

#define SPVGEN_VERSION  0x20000
#define SPVGEN_REVISION 5

#define SPVGEN_MAJOR_VERSION(version)  (version >> 16)
#define SPVGEN_MINOR_VERSION(version)  (version & 0xFFFF)

#ifdef _WIN32
    #define SPVAPI __cdecl
#else
    #define SPVAPI
#endif

#ifndef SH_IMPORT_EXPORT
    #ifdef _WIN32
        #ifdef SH_EXPORTING
            #define SH_IMPORT_EXPORT __declspec(dllexport)
        #else
            #define SH_IMPORT_EXPORT __declspec(dllimport)
        #endif
    #else
        #define SH_IMPORT_EXPORT
    #endif
#endif

#include "vfx.h"
enum SpvGenVersion : uint32_t
{
    SpvGenVersionGlslang,
    SpvGenVersionSpirv,
    SpvGenVersionStd450,
    SpvGenVersionExtAmd,
    SpvGenVersionSpvGen,
    SpvGenVersionVfx,
    SpvGenVersionCount,
};

// Command-line options
enum SpvGenOptions : uint32_t
{
    SpvGenOptionNone                 = 0,
    SpvGenOptionVulkanRules          = (1 << 0),
    SpvGenOptionDefaultDesktop       = (1 << 1),
    SpvGenOptionReadHlsl             = (1 << 2),
    SpvGenOptionHlslOffsets          = (1 << 3),
    SpvGenOptionHlslIoMapping        = (1 << 4),
    SpvGenOptionDebug                = (1 << 5),
    SpvGenOptionAutoMapBindings      = (1 << 6),
    SpvGenOptionFlattenUniformArrays = (1 << 7),
    SpvGenOptionAutoMapLocations     = (1 << 8),
    SpvGenOptionOptimizeDisable      = (1 << 9),
    SpvGenOptionOptimizeSize         = (1 << 10),
    SpvGenOptionInvertY              = (1 << 11),
    SpvGenOptionSuppressInfolog      = (1 << 12),
    SpvGenOptionHlslDX9compatible    = (1 << 13),
    SpvGenOptionHlslEnable16BitTypes = (1 << 14)
};

enum SpvSourceLanguage : uint32_t
{
    SpvSourceLanguageGLSL,      // OpenGL-style
    SpvSourceLanguageVulkan,    // Vulkan GLSL
    SpvSourceLanguageMSL,       // Metal Shading Language
    SpvSourceLanguageHLSL,      // HLSL-style
    SpvSourceLanguageESSL,      // ESSL
};
enum SpvGenStage : uint32_t
{
    SpvGenStageTask,
    SpvGenStageVertex,
    SpvGenStageTessControl,
    SpvGenStageTessEvaluation,
    SpvGenStageGeometry,
    SpvGenStageMesh,
    SpvGenStageFragment,
    SpvGenStageCompute,
    SpvGenStageRayTracingRayGen,
    SpvGenStageRayTracingIntersect,
    SpvGenStageRayTracingAnyHit,
    SpvGenStageRayTracingClosestHit,
    SpvGenStageRayTracingMiss,
    SpvGenStageRayTracingCallable,
    SpvGenStageCount,
    SpvGenStageInvalid = ~0u,
    SpvGenNativeStageCount = SpvGenStageCompute + 1,
};

#ifdef SH_EXPORTING

#ifdef __cplusplus
extern "C"{
#endif
bool SH_IMPORT_EXPORT spvCompileAndLinkProgramFromFile(
    int             fileNum,
    const char*     fileList[],
    void**          pProgram,
    const char**    ppLog);

bool SH_IMPORT_EXPORT spvCompileAndLinkProgramFromFileEx(
    int             fileNum,
    const char*     fileList[],
    const char*     entryPoints[],
    void**          pProgram,
    const char**    ppLog,
    int             options);

bool SH_IMPORT_EXPORT spvCompileAndLinkProgram(
    int                sourceStringCount[SpvGenNativeStageCount],
    const char* const* sourceList[SpvGenNativeStageCount],
    void**             pProgram,
    const char**       ppLog);

bool SH_IMPORT_EXPORT spvCompileAndLinkProgramEx(
    int                stageCount,
    const SpvGenStage* stageList,
    const int*         sourceStringCount,
    const char* const* sourceList[],
    const char* const* fileList[],
    const char*        entryPoints[],
    void**             pProgram,
    const char**       ppLog,
    int                options);

void SH_IMPORT_EXPORT spvDestroyProgram(
    void* hProgram);

int SH_IMPORT_EXPORT spvGetSpirvBinaryFromProgram(
    void*                hProgram,
    int                  stage,
    const unsigned int** ppData);

SpvGenStage SH_IMPORT_EXPORT spvGetStageTypeFromName(
    const char* pName,
    bool*       pIsHlsl);

int SH_IMPORT_EXPORT spvAssembleSpirv(
    const char*   pSpvText,
    unsigned int  bufSize,
    unsigned int* pBuffer,
    const char**  ppLog);

bool SH_IMPORT_EXPORT spvDisassembleSpirv(
    unsigned int size,
    const void*  pSpvToken,
    unsigned int bufSize,
    char*        pBuffer);

bool SH_IMPORT_EXPORT spvCrossSpirv(
    SpvSourceLanguage   sourceLanguage,
    unsigned int        size,
    const void*         pSpvToken,
    char**              spvCrossSpirv);

bool SH_IMPORT_EXPORT spvCrossSpirvEx(
    SpvSourceLanguage   sourceLanguage,
    uint32_t            version,
    unsigned int        size,
    const void*         pSpvToken,
    char**              spvCrossSpirv);

bool SH_IMPORT_EXPORT spvValidateSpirv(
    unsigned int size,
    const void*  pSpvToken,
    unsigned int logSize,
    char*        pLog);

bool SH_IMPORT_EXPORT spvOptimizeSpirv(
    unsigned int   size,
    const void*    pSpvToken,
    int            optionCount,
    const char*    options[],
    unsigned int*  pBufSize,
    void**         ppOptBuf,
    unsigned int   logSize,
    char*          pLog);

void SH_IMPORT_EXPORT spvFreeBuffer(
    void* pBuffer);

bool SH_IMPORT_EXPORT spvGetVersion(
    SpvGenVersion version,
    unsigned int* pVersion,
    unsigned int* pReversion);

bool SH_IMPORT_EXPORT vfxParseFile(
    const char*  pFilename,
    unsigned int numMacro,
    const char*  pMacros[],
    VfxDocType   type,
    void**       ppDoc,
    const char** ppErrorMsg);

void SH_IMPORT_EXPORT vfxCloseDoc(
    void* pDoc);

void SH_IMPORT_EXPORT vfxGetRenderDoc(
    void*              pDoc,
    VfxRenderStatePtr* pRenderState);

void SH_IMPORT_EXPORT vfxGetPipelineDoc(
    void*                pDoc,
    VfxPipelineStatePtr* pPipelineState);

void SH_IMPORT_EXPORT vfxPrintDoc(
    void*                pDoc);

#ifdef __cplusplus
}
#endif

static inline bool InitSpvGen(
    const char* pSpvGenDir = nullptr)
{
    return true;
}

#else

// =====================================================================================================================
// SPIR-V generator entrypoints declaration
typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvCompileAndLinkProgramFromFile)(
    int             fileNum,
    const char*     fileList[],
    void**          pProgram,
    const char**    ppLog);

typedef bool SH_IMPORT_EXPORT(SPVAPI* PFN_spvCompileAndLinkProgramFromFileEx)(
    int             fileNum,
    const char*     fileList[],
    const char*     entryPoints[],
    void**          pProgram,
    const char**    ppLog,
    int             options);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvCompileAndLinkProgram)(
    int                sourceStringCount[SpvGenNativeStageCount],
    const char* const* sourceList[SpvGenNativeStageCount],
    void**             pProgram,
    const char**       ppLog);

typedef bool SH_IMPORT_EXPORT(SPVAPI* PFN_spvCompileAndLinkProgramEx)(
    int                stageCount,
    const SpvGenStage* stageList,
    const int*         sourceStringCount,
    const char* const* sourceList[],
    const char* const* fileList[],
    const char*        entryPoints[],
    void**             pProgram,
    const char**       ppLog,
    int                options);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_spvDestroyProgram)(void* hProgram);

typedef int SH_IMPORT_EXPORT (SPVAPI* PFN_spvGetSpirvBinaryFromProgram)(
    void*                hProgram,
    int                  stage,
    const unsigned int** ppData);

typedef SpvGenStage SH_IMPORT_EXPORT(SPVAPI* PFN_spvGetStageTypeFromName)(
    const char* pName,
    bool*       pIsHlsl);

typedef int SH_IMPORT_EXPORT (SPVAPI* PFN_spvAssembleSpirv)(
    const char*     pSpvText,
    unsigned int    codeBufSize,
    unsigned int*   pSpvCodeBuf,
    const char**    ppLog);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvDisassembleSpirv)(
    unsigned int        size,
    const void*         pSpvCode,
    unsigned int        textBufSize,
    char*               pSpvTextBuf);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvCrossSpirv)(
    SpvSourceLanguage   sourceLanguage,
    unsigned int        size,
    const void*         pSpvToken,
    char**              ppSourceString);

typedef bool SH_IMPORT_EXPORT(SPVAPI* PFN_spvCrossSpirvEx)(
    SpvSourceLanguage   sourceLanguage,
    uint32_t            version,
    unsigned int        size,
    const void*         pSpvToken,
    char**              ppSourceString);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvValidateSpirv)(
    unsigned int        size,
    const void*         pSpvToken,
    unsigned int        bufSize,
    char*               pLog);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvOptimizeSpirv)(
    unsigned int   size,
    const void*    pSpvToken,
    int            optionCount,
    const char*    options[],
    unsigned int*  pBufSize,
    void**         ppOptBuf,
    unsigned int   logSize,
    char*          pLog);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_spvFreeBuffer)(
    void* pBuffer);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvGetVersion)(
    SpvGenVersion  version,
     unsigned int* pVersion,
     unsigned int* pReversion);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_vfxParseFile)(
    const char*  pFilename,
    unsigned int numMacro,
    const char*  pMacros[],
    VfxDocType   type,
    void**       ppDoc,
    const char** ppErrorMsg);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_vfxCloseDoc)(
    void* pDoc);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_vfxGetRenderDoc)(
    void*              pDoc,
    VfxRenderStatePtr* pRenderState);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_vfxGetPipelineDoc)(
    void*                pDoc,
    VfxPipelineStatePtr* pPipelineState);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_vfxPrintDoc)(
    void*                pDoc);

// =====================================================================================================================
// SPIR-V generator entry-points
#define DECL_EXPORT_FUNC(func) \
  extern PFN_##func g_pfn##func

DECL_EXPORT_FUNC(spvCompileAndLinkProgramFromFile);
DECL_EXPORT_FUNC(spvCompileAndLinkProgramFromFileEx);
DECL_EXPORT_FUNC(spvCompileAndLinkProgram);
DECL_EXPORT_FUNC(spvCompileAndLinkProgramEx);
DECL_EXPORT_FUNC(spvDestroyProgram);
DECL_EXPORT_FUNC(spvGetSpirvBinaryFromProgram);
DECL_EXPORT_FUNC(spvGetStageTypeFromName);
DECL_EXPORT_FUNC(spvAssembleSpirv);
DECL_EXPORT_FUNC(spvDisassembleSpirv);
DECL_EXPORT_FUNC(spvCrossSpirv);
DECL_EXPORT_FUNC(spvCrossSpirvEx);
DECL_EXPORT_FUNC(spvValidateSpirv);
DECL_EXPORT_FUNC(spvOptimizeSpirv);
DECL_EXPORT_FUNC(spvFreeBuffer);
DECL_EXPORT_FUNC(spvGetVersion);
DECL_EXPORT_FUNC(vfxParseFile);
DECL_EXPORT_FUNC(vfxCloseDoc);
DECL_EXPORT_FUNC(vfxGetRenderDoc);
DECL_EXPORT_FUNC(vfxGetPipelineDoc);
DECL_EXPORT_FUNC(vfxPrintDoc);

bool SPVAPI InitSpvGen(const char* pSpvGenDir = nullptr);

#endif

#ifdef SPVGEN_STATIC_LIB

#define DEFI_EXPORT_FUNC(func) \
  PFN_##func g_pfn##func = nullptr

DEFI_EXPORT_FUNC(spvCompileAndLinkProgramFromFile);
DEFI_EXPORT_FUNC(spvCompileAndLinkProgramFromFileEx);
DEFI_EXPORT_FUNC(spvCompileAndLinkProgram);
DEFI_EXPORT_FUNC(spvCompileAndLinkProgramEx);
DEFI_EXPORT_FUNC(spvDestroyProgram);
DEFI_EXPORT_FUNC(spvGetSpirvBinaryFromProgram);
DEFI_EXPORT_FUNC(spvGetStageTypeFromName);
DEFI_EXPORT_FUNC(spvAssembleSpirv);
DEFI_EXPORT_FUNC(spvDisassembleSpirv);
DEFI_EXPORT_FUNC(spvCrossSpirv);
DEFI_EXPORT_FUNC(spvCrossSpirvEx);
DEFI_EXPORT_FUNC(spvValidateSpirv);
DEFI_EXPORT_FUNC(spvOptimizeSpirv);
DEFI_EXPORT_FUNC(spvFreeBuffer);
DEFI_EXPORT_FUNC(spvGetVersion);
DEFI_EXPORT_FUNC(vfxParseFile);
DEFI_EXPORT_FUNC(vfxCloseDoc);
DEFI_EXPORT_FUNC(vfxGetRenderDoc);
DEFI_EXPORT_FUNC(vfxGetPipelineDoc);
DEFI_EXPORT_FUNC(vfxPrintDoc);

// SPIR-V generator Windows implementation
#ifdef _WIN32

#include <windows.h>
// SPIR-V generator Windows DLL name
static const char* SpvGeneratorName = "spvgen.dll";

#define INITFUNC(func) \
  g_pfn##func = reinterpret_cast<PFN_##func>(GetProcAddress(hModule, #func));\
  if (g_pfn##func == NULL)\
  {\
      success = false;\
  }

#define INIT_OPT_FUNC(func) \
  g_pfn##func = reinterpret_cast<PFN_##func>(GetProcAddress(hModule, #func));

#else

#include <dlfcn.h>
#include <stdio.h>
#if __APPLE__ && __MACH__
static const char* SpvGeneratorName = "spvgen.dylib";
#else
static const char* SpvGeneratorName = "spvgen.so";
#endif

#define INITFUNC(func) \
  g_pfn##func = reinterpret_cast<PFN_##func>(dlsym(hModule, #func));\
  if (g_pfn##func == NULL)\
  {\
      success = false;\
  }

#define INIT_OPT_FUNC(func) \
  g_pfn##func = reinterpret_cast<PFN_##func>(dlsym(hModule, #func));

#endif

#define DEINITFUNC(func) g_pfn##func = nullptr;

// =====================================================================================================================
// Initialize SPIR-V generator entry-points
// This can be called multiple times in the same application.
bool SPVAPI InitSpvGen(
    const char* pSpvGenDir)   // [in] Directory to load SPVGEN library from, or null to use OS's default search path
{
    if (g_pfnspvGetVersion != nullptr)
    {
        // Already loaded.
        return true;
    }

    bool success = true;
    const char* pLibName = SpvGeneratorName;
    std::string libNameBuffer;
    if (pSpvGenDir != nullptr)
    {
        libNameBuffer = pSpvGenDir;
        libNameBuffer += "/";
        libNameBuffer += pLibName;
        pLibName = libNameBuffer.c_str();
    }
#ifdef _WIN32
    HMODULE hModule = LoadLibraryA(pLibName);
#else
    void* hModule = dlopen(pLibName, RTLD_GLOBAL | RTLD_NOW);
#endif

    if (hModule != NULL)
    {
        INITFUNC(spvCompileAndLinkProgramFromFile);
        INITFUNC(spvCompileAndLinkProgramFromFileEx);
        INITFUNC(spvCompileAndLinkProgram);
        INITFUNC(spvCompileAndLinkProgramEx);
        INITFUNC(spvDestroyProgram);
        INITFUNC(spvGetSpirvBinaryFromProgram);
        INITFUNC(spvGetStageTypeFromName);
        INITFUNC(spvAssembleSpirv);
        INITFUNC(spvDisassembleSpirv);
        INITFUNC(spvCrossSpirv);
        INITFUNC(spvCrossSpirvEx);
        INITFUNC(spvValidateSpirv);
        INITFUNC(spvOptimizeSpirv);
        INITFUNC(spvFreeBuffer);
        INITFUNC(spvGetVersion);
        INITFUNC(vfxParseFile);
        INITFUNC(vfxCloseDoc);
        INIT_OPT_FUNC(vfxGetRenderDoc);
        INIT_OPT_FUNC(vfxGetPipelineDoc);
        INITFUNC(vfxPrintDoc);
    }
    else
    {
        success = false;
    }

    if (success)
    {
        unsigned int version = 0;
        unsigned int revsion = 0;
        success = g_pfnspvGetVersion(SpvGenVersionSpvGen, &version, &revsion);
        if (SPVGEN_MAJOR_VERSION(version) != SPVGEN_MAJOR_VERSION(SPVGEN_VERSION))
        {
            success = false;
        }
    }

    if (success == false)
    {
        DEINITFUNC(spvCompileAndLinkProgramFromFile);
        DEINITFUNC(spvCompileAndLinkProgramFromFileEx);
        DEINITFUNC(spvCompileAndLinkProgram);
        DEINITFUNC(spvCompileAndLinkProgramEx);
        DEINITFUNC(spvDestroyProgram);
        DEINITFUNC(spvGetSpirvBinaryFromProgram);
        DEINITFUNC(spvGetStageTypeFromName);
        DEINITFUNC(spvAssembleSpirv);
        DEINITFUNC(spvDisassembleSpirv);
        DEINITFUNC(spvCrossSpirv);
        DEINITFUNC(spvCrossSpirvEx);
        DEINITFUNC(spvValidateSpirv);
        DEINITFUNC(spvOptimizeSpirv);
        DEINITFUNC(spvFreeBuffer);
        DEINITFUNC(spvGetVersion);
        DEINITFUNC(vfxParseFile);
        DEINITFUNC(vfxCloseDoc);
        DEINITFUNC(vfxGetRenderDoc);
        DEINITFUNC(vfxGetPipelineDoc);
        DEINITFUNC(vfxPrintDoc);
    }
    return success;
}

#endif

#ifndef SH_EXPORTING

#define spvCompileAndLinkProgramFromFile    g_pfnspvCompileAndLinkProgramFromFile
#define spvCompileAndLinkProgramFromFileEx  g_pfnspvCompileAndLinkProgramFromFileEx
#define spvCompileAndLinkProgram            g_pfnspvCompileAndLinkProgram
#define spvCompileAndLinkProgramEx          g_pfnspvCompileAndLinkProgramEx
#define spvDestroyProgram                   g_pfnspvDestroyProgram
#define spvGetSpirvBinaryFromProgram        g_pfnspvGetSpirvBinaryFromProgram
#define spvGetStageTypeFromName             g_pfnspvGetStageTypeFromName
#define spvAssembleSpirv                    g_pfnspvAssembleSpirv
#define spvDisassembleSpirv                 g_pfnspvDisassembleSpirv
#define spvCrossSpirv                       g_pfnspvCrossSpirv
#define spvCrossSpirvEx                     g_pfnspvCrossSpirvEx
#define spvValidateSpirv                    g_pfnspvValidateSpirv
#define spvOptimizeSpirv                    g_pfnspvOptimizeSpirv
#define spvFreeBuffer                       g_pfnspvFreeBuffer
#define spvGetVersion                       g_pfnspvGetVersion

static inline bool vfxParseFile(
    const char*  pFilename,
    unsigned int numMacro,
    const char*  pMacros[],
    VfxDocType   type,
    void**       ppDoc,
    const char** ppErrorMsg)
{
    return (*g_pfnvfxParseFile)(pFilename, numMacro, pMacros, type, ppDoc, ppErrorMsg);
}

static inline void vfxCloseDoc(
    void* pDoc)
{
    (*g_pfnvfxCloseDoc)(pDoc);
}

static inline void vfxGetRenderDoc(
    void*              pDoc,
    VfxRenderStatePtr* pRenderState)
{
    (*g_pfnvfxGetRenderDoc)(pDoc, pRenderState);
}

static inline void vfxGetPipelineDoc(
    void*                pDoc,
    VfxPipelineStatePtr* pPipelineState)
{
    (*g_pfnvfxGetPipelineDoc)(pDoc, pPipelineState);
}

static inline void vfxPrintDoc(
    void*                pDoc)
{
    (*g_pfnvfxPrintDoc)(pDoc);
}

#endif
// clang-format on
