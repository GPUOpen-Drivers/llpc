/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2015-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief SPVGEN header file: contains the definition and the wrap implementation of SPIR-V generator entry-points
 ***********************************************************************************************************************
 */
#pragma once

#define SPVGEN_VERSION  0x10000
#define SPVGEN_REVISION 4

#ifndef SH_IMPORT_EXPORT
    #ifdef _WIN32
        #define SPVAPI __cdecl
        #ifdef SH_EXPORTING
            #define SH_IMPORT_EXPORT __declspec(dllexport)
        #else
            #define SH_IMPORT_EXPORT __declspec(dllimport)
        #endif
    #else
        #define SH_IMPORT_EXPORT
        #define SPVAPI
    #endif
#endif

#include "vfx.h"

enum SpvGenVersion
{
    SpvGenVersionGlslang,
    SpvGenVersionSpirv,
    SpvGenVersionStd450,
    SpvGenVersionExtAmd,
    SpvGenVersionCount,
};

// Command-line options
enum TOptions {
    EOptionNone = 0,
    EOptionVulkanRules = (1 << 0),
    EOptionDefaultDesktop = (1 << 1),
    EOptionReadHlsl = (1 << 2),
    EOptionHlslOffsets = (1 << 3),
    EOptionHlslIoMapping = (1 << 4),
    EOptionDebug = (1 << 5),
    EOptionAutoMapBindings = (1 << 6),
    EOptionFlattenUniformArrays = (1 << 7),
    EOptionAutoMapLocations = (1 << 8),
    EOptionOptimizeDisable = (1 << 9),
    EOptionOptimizeSize = (1 << 10),
    EOptionInvertY = (1 << 11),
};

#define VkStageCount 6

#ifdef SH_EXPORTING

#ifdef __cplusplus
extern "C"{
#endif
bool SH_IMPORT_EXPORT spvCompileAndLinkProgramFromFile(
    int             fileNum,
    const char*     fileList[],
    void**          pProgram,
    const char**    ppLog);

bool SH_IMPORT_EXPORT spvCompileAndLinkProgram(
    int                sourceStringCount[VkStageCount],
    const char* const* sourceList[VkStageCount],
    void**             pProgram,
    const char**       ppLog);

bool SH_IMPORT_EXPORT spvCompileAndLinkProgramWithOptions(
    int                sourceStringCount[VkStageCount],
    const char* const* sourceList[VkStageCount],
    void**             pProgram,
    const char**       ppLog,
    int                options);

void SH_IMPORT_EXPORT spvDestroyProgram(
    void* hProgram);

int SH_IMPORT_EXPORT spvGetSpirvBinaryFromProgram(
    void*                hProgram,
    int                  stage,
    const unsigned int** ppData);

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

static inline bool InitSpvGen()
{
    return true;
}

#else

typedef enum {
    EShLangVertex,
    EShLangTessControl,
    EShLangTessEvaluation,
    EShLangGeometry,
    EShLangFragment,
    EShLangCompute,
    EShLangCount,
} EShLanguage;

// =====================================================================================================================
// SPIR-V generator entrypoints declaration
typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvCompileAndLinkProgramFromFile)(
    int             fileNum,
    const char*     fileList[],
    void**          pProgram,
    const char**    ppLog);

typedef bool SH_IMPORT_EXPORT (SPVAPI* PFN_spvCompileAndLinkProgram)(
    int                sourceStringCount[VkStageCount],
    const char* const* sourceList[VkStageCount],
    void**             pProgram,
    const char**       ppLog);

typedef bool SH_IMPORT_EXPORT(SPVAPI* PFN_spvCompileAndLinkProgramWithOptions)(
    int                sourceStringCount[VkStageCount],
    const char* const* sourceList[VkStageCount],
    void**             pProgram,
    const char**       ppLog,
    int                options);

typedef void SH_IMPORT_EXPORT (SPVAPI* PFN_spvDestroyProgram)(void* hProgram);

typedef int SH_IMPORT_EXPORT (SPVAPI* PFN_spvGetSpirvBinaryFromProgram)(
    void*                hProgram,
    int                  stage,
    const unsigned int** ppData);

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
DECL_EXPORT_FUNC(spvCompileAndLinkProgram);
DECL_EXPORT_FUNC(spvCompileAndLinkProgramWithOptions);
DECL_EXPORT_FUNC(spvDestroyProgram);
DECL_EXPORT_FUNC(spvGetSpirvBinaryFromProgram);
DECL_EXPORT_FUNC(spvAssembleSpirv);
DECL_EXPORT_FUNC(spvDisassembleSpirv);
DECL_EXPORT_FUNC(spvValidateSpirv);
DECL_EXPORT_FUNC(spvOptimizeSpirv);
DECL_EXPORT_FUNC(spvFreeBuffer);
DECL_EXPORT_FUNC(spvGetVersion);
DECL_EXPORT_FUNC(vfxParseFile);
DECL_EXPORT_FUNC(vfxCloseDoc);
DECL_EXPORT_FUNC(vfxGetRenderDoc);
DECL_EXPORT_FUNC(vfxGetPipelineDoc);
DECL_EXPORT_FUNC(vfxPrintDoc);

bool InitSpvGen();

#endif

#ifdef SPVGEN_STATIC_LIB

#define DEFI_EXPORT_FUNC(func) \
  PFN_##func g_pfn##func = nullptr

DEFI_EXPORT_FUNC(spvCompileAndLinkProgramFromFile);
DEFI_EXPORT_FUNC(spvCompileAndLinkProgram);
DEFI_EXPORT_FUNC(spvCompileAndLinkProgramWithOptions);
DEFI_EXPORT_FUNC(spvDestroyProgram);
DEFI_EXPORT_FUNC(spvGetSpirvBinaryFromProgram);
DEFI_EXPORT_FUNC(spvAssembleSpirv);
DEFI_EXPORT_FUNC(spvDisassembleSpirv);
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
#ifdef UNICODE
static const wchar_t* SpvGeneratorName = L"spvgen.dll";
#else
static const char* SpvGeneratorName = "spvgen.dll";
#endif

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

#endif // _WIN32

#define DEINITFUNC(func) g_pfn##func = nullptr;

// =====================================================================================================================
// Initialize SPIR-V generator entry-points
// This can be called multiple times in the same application.
bool InitSpvGen()
{
    if (g_pfnspvCompileAndLinkProgramFromFile != nullptr)
    {
        // Already loaded.
        return true;
    }

    bool success = true;
#ifdef _WIN32
    HMODULE hModule = LoadLibrary(SpvGeneratorName);
#else
    void* hModule = dlopen(SpvGeneratorName, RTLD_GLOBAL | RTLD_NOW);
#endif

    if (hModule != NULL)
    {
        INITFUNC(spvCompileAndLinkProgramFromFile);
        INITFUNC(spvCompileAndLinkProgram);
        INITFUNC(spvCompileAndLinkProgramWithOptions);
        INITFUNC(spvDestroyProgram);
        INITFUNC(spvGetSpirvBinaryFromProgram);
        INITFUNC(spvAssembleSpirv);
        INITFUNC(spvDisassembleSpirv);
        INITFUNC(spvValidateSpirv);
        INITFUNC(spvOptimizeSpirv);
        INITFUNC(spvFreeBuffer);
        INIT_OPT_FUNC(spvGetVersion);
        INITFUNC(vfxParseFile);
        INITFUNC(vfxCloseDoc);
        INITFUNC(vfxGetRenderDoc);
        INITFUNC(vfxGetPipelineDoc);
        INIT_OPT_FUNC(vfxPrintDoc);
    }
    else
    {
        success = false;
    }
    if (success == false)
    {
        DEINITFUNC(spvCompileAndLinkProgramFromFile);
        DEINITFUNC(spvCompileAndLinkProgram);
        DEINITFUNC(spvCompileAndLinkProgramWithOptions);
        DEINITFUNC(spvDestroyProgram);
        DEINITFUNC(spvGetSpirvBinaryFromProgram);
        DEINITFUNC(spvAssembleSpirv);
        DEINITFUNC(spvDisassembleSpirv);
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
#define spvCompileAndLinkProgram            g_pfnspvCompileAndLinkProgram
#define spvCompileAndLinkProgramWithOptions g_pfnspvCompileAndLinkProgramWithOptions
#define spvDestroyProgram                   g_pfnspvDestroyProgram
#define spvGetSpirvBinaryFromProgram        g_pfnspvGetSpirvBinaryFromProgram
#define spvAssembleSpirv                    g_pfnspvAssembleSpirv
#define spvDisassembleSpirv                 g_pfnspvDisassembleSpirv
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

