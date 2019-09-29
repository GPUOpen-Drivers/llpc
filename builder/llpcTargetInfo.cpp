/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcTargetInfo.cpp
 * @brief LLPC source file: code to set up TargetInfo
 ***********************************************************************************************************************
 */
#include "llpcTargetInfo.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"

using namespace Llpc;
using namespace llvm;

#if LLPC_BUILD_GFX10
namespace Llpc
{

// -native-wave-size: an option to override hardware native wave size, it will allow compiler to choose
// final wave size base on it. Used in pre-silicon verification.
cl::opt<int> NativeWaveSize("native-wave-size", cl::desc("Overrides hardware native wave size"), cl::init(0));

} // Llpc
#endif

// =====================================================================================================================
// Functions to set up TargetInfo for the various targets

// gfx6+
static void SetGfx6BaseInfo(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    // Initial settings (could be adjusted later according to graphics IP version info)
    pTargetInfo->gpuProperty.waveSize = 64;

    pTargetInfo->gpuProperty.ldsSizePerThreadGroup = 32 * 1024;
    pTargetInfo->gpuProperty.numShaderEngines = 4;
    pTargetInfo->gpuProperty.maxSgprsAvailable = 104;
    pTargetInfo->gpuProperty.maxVgprsAvailable = 256;

    //TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
    pTargetInfo->gpuProperty.gsPrimBufferDepth = 0x100;

    pTargetInfo->gpuProperty.maxUserDataCount = 16; // GFX6-8 value

    pTargetInfo->gpuProperty.gsOnChipMaxLdsSize = 16384;

    pTargetInfo->gpuProperty.tessOffChipLdsBufferSize = 32768;

    // TODO: Accept gsOnChipDefaultPrimsPerSubgroup from panel option
    pTargetInfo->gpuProperty.gsOnChipDefaultPrimsPerSubgroup   = 64;

    pTargetInfo->gpuProperty.tessFactorBufferSizePerSe = 4096;

    // TODO: Accept gsOnChipDefaultLdsSizePerSubgroup from panel option
    pTargetInfo->gpuProperty.gsOnChipDefaultLdsSizePerSubgroup = 8192; // GFX6-8 value
}

// gfx6
static void SetGfx6Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx6BaseInfo(pTargetInfo);
    pTargetInfo->gpuProperty.ldsSizePerCu = 32768;
    pTargetInfo->gpuProperty.ldsSizeDwordGranularityShift = 6;

    // Hardware workarounds for GFX6 based GPU's:
    pTargetInfo->gpuWorkarounds.gfx6.cbNoLt16BitIntClamp = 1;
    pTargetInfo->gpuWorkarounds.gfx6.miscLoadBalancePerWatt = 1;
    pTargetInfo->gpuWorkarounds.gfx6.shader8b16bLocalWriteCorruption = 1;

    pTargetInfo->gpuWorkarounds.gfx6.shaderReadlaneSmrd = 1;

    pTargetInfo->gpuWorkarounds.gfx6.shaderSpiCsRegAllocFragmentation = 1;

    pTargetInfo->gpuWorkarounds.gfx6.shaderVcczScalarReadBranchFailure = 1;

    pTargetInfo->gpuWorkarounds.gfx6.shaderMinMaxFlushDenorm = 1;

    // NOTE: We only need workaround it in Tahiti, Pitcairn, Capeverde, to simplify the design, we set this
    // flag for all gfxIp.major == 6
    pTargetInfo->gpuWorkarounds.gfx6.shaderZExport = 1;
}

// gfx600
static void SetGfx600Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx6Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 2;
}

// gfx601
static void SetGfx601Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx6Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 1;
}

// gfx7+
static void SetGfx7BaseInfo(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx6BaseInfo(pTargetInfo);
    pTargetInfo->gpuProperty.ldsSizePerCu = 65536;
    pTargetInfo->gpuProperty.ldsSizeDwordGranularityShift = 7;
}

// gfx7
static void SetGfx7Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx7BaseInfo(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 1; // GFX7.0.2+ value

    // Hardware workarounds for GFX7 based GPU's:
    pTargetInfo->gpuWorkarounds.gfx6.shaderVcczScalarReadBranchFailure = 1;
    pTargetInfo->gpuWorkarounds.gfx6.shaderMinMaxFlushDenorm = 1;
}

// gfx700
static void SetGfx700Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx7Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 2;

    // Hardware workarounds for GFX7.0.0
    pTargetInfo->gpuWorkarounds.gfx6.cbNoLt16BitIntClamp = 1;
    // NOTE: Buffer store + index mode are not used in vulkan, so we can skip this workaround in safe.
    pTargetInfo->gpuWorkarounds.gfx6.shaderCoalesceStore = 1;
}

// gfx701
static void SetGfx701Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx7Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 4;
}

// gfx703 and gfx704
static void SetGfx703Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx7Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 4;

    // Hardware workarounds for GFX7.0.3 / GFX7.0.4
    pTargetInfo->gpuWorkarounds.gfx6.cbNoLt16BitIntClamp = 1;
    pTargetInfo->gpuWorkarounds.gfx6.shaderCoalesceStore = 1;
    pTargetInfo->gpuWorkarounds.gfx6.shaderSpiBarrierMgmt = 1;
    pTargetInfo->gpuWorkarounds.gfx6.shaderSpiCsRegAllocFragmentation = 1;
}

// gfx8+
static void SetGfx8BaseInfo(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx7BaseInfo(pTargetInfo);
}

// gfx8
static void SetGfx8Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx8BaseInfo(pTargetInfo);

    // Hardware workarounds for GFX8.x based GPU's:
    pTargetInfo->gpuWorkarounds.gfx6.shaderMinMaxFlushDenorm = 1;

    pTargetInfo->gpuWorkarounds.gfx6.shaderSmemBufferAddrClamp = 1;

    pTargetInfo->gpuWorkarounds.gfx6.shaderEstimateRegisterUsage = 1;
}

// gfx800/gfx801
static void SetGfx800Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx8Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 1;
}

// gfx802
static void SetGfx802Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx8Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 4;

    // Hardware workarounds
    pTargetInfo->gpuWorkarounds.gfx6.miscSpiSgprsNum = 1;
}

// gfx803+
static void SetGfx803Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx8Info(pTargetInfo);
    // TODO: polaris11 and polaris12 is 2, but we can't identify them by GFX IP now.
    pTargetInfo->gpuProperty.numShaderEngines = 4;
}

// gfx81
static void SetGfx81Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx8Info(pTargetInfo);
    pTargetInfo->gpuProperty.numShaderEngines = 1;
}

// gfx9+
static void SetGfx9BaseInfo(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx8BaseInfo(pTargetInfo);
    pTargetInfo->gpuProperty.maxUserDataCount = 32;
    pTargetInfo->gpuProperty.gsOnChipDefaultLdsSizePerSubgroup = 0; // GFX9+ does not use this
    pTargetInfo->gpuProperty.tessFactorBufferSizePerSe = 8192;
    pTargetInfo->gpuProperty.numShaderEngines = 4;
}

// gfx9
static void SetGfx9Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx9BaseInfo(pTargetInfo);

    // TODO: Clean up code for all 1d texture patch
    pTargetInfo->gpuWorkarounds.gfx9.treat1dImagesAs2d = 1;

    pTargetInfo->gpuWorkarounds.gfx9.shaderImageGatherInstFix = 1;

    pTargetInfo->gpuWorkarounds.gfx9.fixCacheLineStraddling = 1;
}

// gfx900
static void SetGfx900Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx9Info(pTargetInfo);
    pTargetInfo->gpuWorkarounds.gfx9.fixLsVgprInput = 1;
}

#if LLPC_BUILD_GFX10
// gfx10
static void SetGfx10Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx9BaseInfo(pTargetInfo);

    // Compiler is free to choose wave mode if forced wave size is not specified.
    if (NativeWaveSize != 0)
    {
        LLPC_ASSERT((NativeWaveSize == 32) || (NativeWaveSize == 64));
        pTargetInfo->gpuProperty.waveSize = NativeWaveSize;
    }
    else
    {
        pTargetInfo->gpuProperty.waveSize = 32;
    }

    pTargetInfo->gpuProperty.numShaderEngines = 2;
    pTargetInfo->gpuProperty.supportShaderPowerProfiling = true;
    pTargetInfo->gpuProperty.tessFactorBufferSizePerSe = 8192;
    pTargetInfo->gpuProperty.supportSpiPrefPriority = true;

    // Hardware workarounds for GFX10 based GPU's:
    pTargetInfo->gpuWorkarounds.gfx10.disableI32ModToI16Mod = 1;
}

// gfx1010 (including gfx101E and gfx101F)
static void SetGfx1010Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx10Info(pTargetInfo);

    pTargetInfo->gpuWorkarounds.gfx10.waShaderInstPrefetch0 = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waDidtThrottleVmem = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waLdsVmemNotWaitingVmVsrc = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNsaAndClauseCanHang = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNsaCannotFollowWritelane = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waTessIncorrectRelativeIndex = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waSmemFollowedByVopc = 1;
}

// gfx101F
static void SetGfx101FInfo(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx1010Info(pTargetInfo);
    pTargetInfo->gpuProperty.tessFactorBufferSizePerSe = 0x80;

    pTargetInfo->gpuWorkarounds.gfx10.waTessFactorBufferSizeLimitGeUtcl1Underflow = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waShaderInstPrefetch123   = 1;
    pTargetInfo->gpuWorkarounds.gfx10.nggTessDegeneratePrims    = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waThrottleInMultiDwordNsa = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNggCullingNoEmptySubgroups = 1;
}

#if LLPC_BUILD_NAVI12
// gfx1011
static void SetGfx1011Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx10Info(pTargetInfo);

    pTargetInfo->gpuWorkarounds.gfx10.waShaderInstPrefetch0      = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waDidtThrottleVmem         = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waLdsVmemNotWaitingVmVsrc  = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNsaCannotFollowWritelane = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNsaAndClauseCanHang      = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waSmemFollowedByVopc       = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waShaderInstPrefetchFwd64  = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waWarFpAtomicDenormHazard  = 1;
}

#endif
#if LLPC_BUILD_NAVI14
// gfx1012
static void SetGfx1012Info(
    TargetInfo* pTargetInfo)    // [in/out] Target info
{
    SetGfx10Info(pTargetInfo);

    pTargetInfo->gpuWorkarounds.gfx10.waShaderInstPrefetch0      = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waDidtThrottleVmem         = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waLdsVmemNotWaitingVmVsrc  = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNsaCannotFollowWritelane = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNsaAndClauseCanHang      = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waThrottleInMultiDwordNsa  = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waSmemFollowedByVopc       = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNggCullingNoEmptySubgroups = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waShaderInstPrefetchFwd64  = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waWarFpAtomicDenormHazard  = 1;
    pTargetInfo->gpuWorkarounds.gfx10.waNggDisabled              = 1;
}

#endif
#endif
// =====================================================================================================================
// Set TargetInfo. Returns false if the GPU name is not found or not supported.
bool Llpc::SetTargetInfo(
    StringRef     gpuName,      // LLVM GPU name, e.g. "gfx900"
    TargetInfo*   pTargetInfo)  // [out] TargetInfo struct to set up
{
    *pTargetInfo = {};

    struct GpuNameStringMap
    {
        const char* pGpuName;
        void(*      pSetTargetInfoFunc)(TargetInfo* pTargetInfo);
    };

    static const GpuNameStringMap GpuNameMap[] =
    {
        { "gfx600",   &SetGfx600Info },   // gfx600, tahiti
        { "gfx601",   &SetGfx601Info },   // gfx601, pitcairn, verde, oland, hainan
        { "gfx700",   &SetGfx700Info },   // gfx700, kaveri
        { "gfx701",   &SetGfx701Info },   // gfx701, hawaii
        { "gfx702",   &SetGfx7Info },     // gfx702
        { "gfx703",   &SetGfx703Info },   // gfx703, kabini, mullins
        { "gfx704",   &SetGfx703Info },   // gfx704, bonaire
        { "gfx800",   &SetGfx800Info },   // gfx800, iceland
        { "gfx801",   &SetGfx800Info },   // gfx801, carrizo
        { "gfx802",   &SetGfx802Info },   // gfx802, tonga
        { "gfx803",   &SetGfx803Info },   // gfx803, fiji, polaris10, polaris11
        { "gfx804",   &SetGfx803Info },   // gfx804
        { "gfx810",   &SetGfx81Info },    // gfx810, stoney
        { "gfx900",   &SetGfx900Info },   // gfx900
        { "gfx901",   &SetGfx9Info },     // gfx901
        { "gfx902",   &SetGfx900Info },   // gfx902
        { "gfx903",   &SetGfx9Info },     // gfx903
        { "gfx904",   &SetGfx9Info },     // gfx904, vega12
#if LLPC_BUILD_VEGA20
        { "gfx906",   &SetGfx9Info },     // gfx906, vega20
#endif
#if LLPC_BUILD_RAVEN2
        { "gfx909",   &SetGfx9Info },     // gfx909, raven2
#endif
#if LLPC_BUILD_GFX10
        { "gfx101F",  &SetGfx101FInfo },
        { "gfx101E",  &SetGfx1010Info },
        { "gfx1010",  &SetGfx1010Info },  // gfx1010
#if LLPC_BUILD_NAVI12
        { "gfx1011",  &SetGfx1011Info },  // gfx1011，navi12
#endif
#if LLPC_BUILD_NAVI14
        { "gfx101D",  &SetGfx1012Info },
        { "gfx1012",  &SetGfx1012Info },  // gfx1012, navi14
#endif
#if LLPC_BUILD_NAVI21
        { "gfx1031",  &SetGfx10Info },    // gfx1031, navi21
#endif
#endif
    };

    void(* pSetTargetInfoFunc)(TargetInfo* pTargetInfo) = nullptr;
    for (const GpuNameStringMap& mapEntry : ArrayRef<GpuNameStringMap>(GpuNameMap))
    {
        if (gpuName == mapEntry.pGpuName)
        {
            pSetTargetInfoFunc = mapEntry.pSetTargetInfoFunc;
            break;
        }
    }
    if (pSetTargetInfoFunc == nullptr)
    {
        return false;   // Target not supported
    }

    // Set up TargetInfo.gfxIp from the GPU name. This is the inverse of what happens to encode the
    // GPU name in PipelineContext::GetGpuNameString. But longer term we should remove all the uses of
    // TargetInfo.gfxIp in the middle-end and use specific feature bits instead.
    gpuName.slice(3, gpuName.size() - 2).consumeInteger(10, pTargetInfo->gfxIp.major);
    pTargetInfo->gfxIp.minor = gpuName[gpuName.size() - 2] - '0';
    pTargetInfo->gfxIp.stepping = gpuName[gpuName.size() - 1] - '0';
    if (pTargetInfo->gfxIp.stepping >= 10)
    {
        pTargetInfo->gfxIp.stepping = gpuName[gpuName.size() - 1] - 'A' + 0xFFFA;
    }

    // Set up the rest of TargetInfo.
    (*pSetTargetInfoFunc)(pTargetInfo);

    return true;
}

