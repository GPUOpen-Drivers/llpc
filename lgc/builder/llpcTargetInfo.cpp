/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

using namespace lgc;
using namespace llvm;

// -native-wave-size: an option to override hardware native wave size, it will allow compiler to choose
// final wave size base on it. Used in pre-silicon verification.
static cl::opt<int> NativeWaveSize("native-wave-size", cl::desc("Overrides hardware native wave size"), cl::init(0));

// =====================================================================================================================
// Functions to set up TargetInfo for the various targets

// gfx6+
static void setGfx6BaseInfo(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    // Initial settings (could be adjusted later according to graphics IP version info)
    targetInfo->getGpuProperty().waveSize = 64;

    targetInfo->getGpuProperty().ldsSizePerThreadGroup = 32 * 1024;
    targetInfo->getGpuProperty().numShaderEngines = 4;
    targetInfo->getGpuProperty().maxSgprsAvailable = 104;
    targetInfo->getGpuProperty().maxVgprsAvailable = 256;

    //TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
    targetInfo->getGpuProperty().gsPrimBufferDepth = 0x100;

    targetInfo->getGpuProperty().maxUserDataCount = 16; // GFX6-8 value

    targetInfo->getGpuProperty().gsOnChipMaxLdsSize = 16384;

    targetInfo->getGpuProperty().tessOffChipLdsBufferSize = 32768;

    // TODO: Accept gsOnChipDefaultPrimsPerSubgroup from panel option
    targetInfo->getGpuProperty().gsOnChipDefaultPrimsPerSubgroup   = 64;

    targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 4096;

    // TODO: Accept gsOnChipDefaultLdsSizePerSubgroup from panel option
    targetInfo->getGpuProperty().gsOnChipDefaultLdsSizePerSubgroup = 8192; // GFX6-8 value
}

// gfx6
static void setGfx6Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx6BaseInfo(targetInfo);
    targetInfo->getGpuProperty().ldsSizePerCu = 32768;
    targetInfo->getGpuProperty().ldsSizeDwordGranularityShift = 6;

    // Hardware workarounds for GFX6 based GPU's:
    targetInfo->getGpuWorkarounds().gfx6.cbNoLt16BitIntClamp = 1;
    targetInfo->getGpuWorkarounds().gfx6.miscLoadBalancePerWatt = 1;
    targetInfo->getGpuWorkarounds().gfx6.shader8b16bLocalWriteCorruption = 1;

    targetInfo->getGpuWorkarounds().gfx6.shaderReadlaneSmrd = 1;

    targetInfo->getGpuWorkarounds().gfx6.shaderSpiCsRegAllocFragmentation = 1;

    targetInfo->getGpuWorkarounds().gfx6.shaderVcczScalarReadBranchFailure = 1;

    targetInfo->getGpuWorkarounds().gfx6.shaderMinMaxFlushDenorm = 1;

    // NOTE: We only need workaround it in Tahiti, Pitcairn, Capeverde, to simplify the design, we set this
    // flag for all gfxIp.major == 6
    targetInfo->getGpuWorkarounds().gfx6.shaderZExport = 1;
}

// gfx600
static void setGfx600Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx6Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 2;
}

// gfx601
static void setGfx601Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx6Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 1;
}

// gfx7+
static void setGfx7BaseInfo(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx6BaseInfo(targetInfo);
    targetInfo->getGpuProperty().ldsSizePerCu = 65536;
    targetInfo->getGpuProperty().ldsSizeDwordGranularityShift = 7;
}

// gfx7
static void setGfx7Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx7BaseInfo(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 1; // GFX7.0.2+ value

    // Hardware workarounds for GFX7 based GPU's:
    targetInfo->getGpuWorkarounds().gfx6.shaderVcczScalarReadBranchFailure = 1;
    targetInfo->getGpuWorkarounds().gfx6.shaderMinMaxFlushDenorm = 1;
}

// gfx700
static void setGfx700Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx7Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 2;

    // Hardware workarounds for GFX7.0.0
    targetInfo->getGpuWorkarounds().gfx6.cbNoLt16BitIntClamp = 1;
    // NOTE: Buffer store + index mode are not used in vulkan, so we can skip this workaround in safe.
    targetInfo->getGpuWorkarounds().gfx6.shaderCoalesceStore = 1;
}

// gfx701
static void setGfx701Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx7Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx703 and gfx704
static void setGfx703Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx7Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 4;

    // Hardware workarounds for GFX7.0.3 / GFX7.0.4
    targetInfo->getGpuWorkarounds().gfx6.cbNoLt16BitIntClamp = 1;
    targetInfo->getGpuWorkarounds().gfx6.shaderCoalesceStore = 1;
    targetInfo->getGpuWorkarounds().gfx6.shaderSpiBarrierMgmt = 1;
    targetInfo->getGpuWorkarounds().gfx6.shaderSpiCsRegAllocFragmentation = 1;
}

// gfx8+
static void setGfx8BaseInfo(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx7BaseInfo(targetInfo);
}

// gfx8
static void setGfx8Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx8BaseInfo(targetInfo);

    // Hardware workarounds for GFX8.x based GPU's:
    targetInfo->getGpuWorkarounds().gfx6.shaderMinMaxFlushDenorm = 1;

    targetInfo->getGpuWorkarounds().gfx6.shaderSmemBufferAddrClamp = 1;

    targetInfo->getGpuWorkarounds().gfx6.shaderEstimateRegisterUsage = 1;
}

// gfx800/gfx801
static void setGfx800Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx8Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 1;
}

// gfx802
static void setGfx802Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx8Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 4;

    // Hardware workarounds
    targetInfo->getGpuWorkarounds().gfx6.miscSpiSgprsNum = 1;
}

// gfx803+
static void setGfx803Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx8Info(targetInfo);
    // TODO: polaris11 and polaris12 is 2, but we can't identify them by GFX IP now.
    targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx81
static void setGfx81Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx8Info(targetInfo);
    targetInfo->getGpuProperty().numShaderEngines = 1;
}

// gfx9+
static void setGfx9BaseInfo(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx8BaseInfo(targetInfo);
    targetInfo->getGpuProperty().maxUserDataCount = 32;
    targetInfo->getGpuProperty().gsOnChipDefaultLdsSizePerSubgroup = 0; // GFX9+ does not use this
    targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 8192;
    targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx9
static void setGfx9Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx9BaseInfo(targetInfo);

    // TODO: Clean up code for all 1d texture patch
    targetInfo->getGpuWorkarounds().gfx9.treat1dImagesAs2d = 1;

    targetInfo->getGpuWorkarounds().gfx9.shaderImageGatherInstFix = 1;

    targetInfo->getGpuWorkarounds().gfx9.fixCacheLineStraddling = 1;
}

// gfx900
static void setGfx900Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx9Info(targetInfo);
    targetInfo->getGpuWorkarounds().gfx9.fixLsVgprInput = 1;
}

// gfx10
static void setGfx10Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx9BaseInfo(targetInfo);

    // Compiler is free to choose wave mode if forced wave size is not specified.
    if (NativeWaveSize != 0)
    {
        assert(NativeWaveSize == 32 || NativeWaveSize == 64);
        targetInfo->getGpuProperty().waveSize = NativeWaveSize;
    }
    else
        targetInfo->getGpuProperty().waveSize = 32;

    targetInfo->getGpuProperty().numShaderEngines = 2;
    targetInfo->getGpuProperty().supportShaderPowerProfiling = true;
    targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 8192;
    targetInfo->getGpuProperty().supportSpiPrefPriority = true;

    // Hardware workarounds for GFX10 based GPU's:
    targetInfo->getGpuWorkarounds().gfx10.disableI32ModToI16Mod = 1;
}

// gfx1010 (including gfx101E and gfx101F)
static void setGfx1010Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx10Info(targetInfo);

    targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetch0 = 1;
    targetInfo->getGpuWorkarounds().gfx10.waDidtThrottleVmem = 1;
    targetInfo->getGpuWorkarounds().gfx10.waLdsVmemNotWaitingVmVsrc = 1;
    targetInfo->getGpuWorkarounds().gfx10.waNsaAndClauseCanHang = 1;
    targetInfo->getGpuWorkarounds().gfx10.waNsaCannotFollowWritelane = 1;
    targetInfo->getGpuWorkarounds().gfx10.waTessIncorrectRelativeIndex = 1;
    targetInfo->getGpuWorkarounds().gfx10.waSmemFollowedByVopc = 1;
}

// gfx1012
static void setGfx1012Info(
    TargetInfo* targetInfo)    // [in/out] Target info
{
    setGfx10Info(targetInfo);

    targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetch0      = 1;
    targetInfo->getGpuWorkarounds().gfx10.waDidtThrottleVmem         = 1;
    targetInfo->getGpuWorkarounds().gfx10.waLdsVmemNotWaitingVmVsrc  = 1;
    targetInfo->getGpuWorkarounds().gfx10.waNsaCannotFollowWritelane = 1;
    targetInfo->getGpuWorkarounds().gfx10.waNsaAndClauseCanHang      = 1;
    targetInfo->getGpuWorkarounds().gfx10.waThrottleInMultiDwordNsa  = 1;
    targetInfo->getGpuWorkarounds().gfx10.waSmemFollowedByVopc       = 1;
    targetInfo->getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups = 1;
    targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetchFwd64  = 1;
    targetInfo->getGpuWorkarounds().gfx10.waWarFpAtomicDenormHazard  = 1;
    targetInfo->getGpuWorkarounds().gfx10.waNggDisabled              = 1;
}

// =====================================================================================================================
// Set TargetInfo. Returns false if the GPU name is not found or not supported.
bool TargetInfo::setTargetInfo(
    StringRef     gpuName)      // LLVM GPU name, e.g. "gfx900"
{
    struct GpuNameStringMap
    {
        const char* gpuName;
        void(*      setTargetInfoFunc)(TargetInfo* targetInfo);
    };

    static const GpuNameStringMap GpuNameMap[] =
    {
        { "gfx600",   &setGfx600Info },   // gfx600, tahiti
        { "gfx601",   &setGfx601Info },   // gfx601, pitcairn, verde, oland, hainan
        { "gfx700",   &setGfx700Info },   // gfx700, kaveri
        { "gfx701",   &setGfx701Info },   // gfx701, hawaii
        { "gfx702",   &setGfx7Info },     // gfx702
        { "gfx703",   &setGfx703Info },   // gfx703, kabini, mullins
        { "gfx704",   &setGfx703Info },   // gfx704, bonaire
        { "gfx800",   &setGfx800Info },   // gfx800, iceland
        { "gfx801",   &setGfx800Info },   // gfx801, carrizo
        { "gfx802",   &setGfx802Info },   // gfx802, tonga
        { "gfx803",   &setGfx803Info },   // gfx803, fiji, polaris10, polaris11
        { "gfx804",   &setGfx803Info },   // gfx804
        { "gfx810",   &setGfx81Info },    // gfx810, stoney
        { "gfx900",   &setGfx900Info },   // gfx900
        { "gfx901",   &setGfx9Info },     // gfx901
        { "gfx902",   &setGfx900Info },   // gfx902
        { "gfx903",   &setGfx9Info },     // gfx903
        { "gfx904",   &setGfx9Info },     // gfx904, vega12
        { "gfx906",   &setGfx9Info },     // gfx906, vega20
        { "gfx909",   &setGfx9Info },     // gfx909, raven2
        { "gfx1010",  &setGfx1010Info },  // gfx1010
        { "gfx1012",  &setGfx1012Info },  // gfx1012, navi14
    };

    void(* setTargetInfoFunc)(TargetInfo* targetInfo) = nullptr;
    for (const GpuNameStringMap& mapEntry : ArrayRef<GpuNameStringMap>(GpuNameMap))
    {
        if (gpuName == mapEntry.gpuName)
        {
            setTargetInfoFunc = mapEntry.setTargetInfoFunc;
            break;
        }
    }
    if (!setTargetInfoFunc )
        return false;   // Target not supported

    // Set up TargetInfo.gfxIp from the GPU name. This is the inverse of what happens to encode the
    // GPU name in PipelineContext::GetGpuNameString. But longer term we should remove all the uses of
    // TargetInfo.gfxIp in the middle-end and use specific feature bits instead.
    gpuName.slice(3, gpuName.size() - 2).consumeInteger(10, m_gfxIp.major);
    m_gfxIp.minor = gpuName[gpuName.size() - 2] - '0';
    m_gfxIp.stepping = gpuName[gpuName.size() - 1] - '0';
    if (m_gfxIp.stepping >= 10)
        m_gfxIp.stepping = gpuName[gpuName.size() - 1] - 'A' + 0xFFFA;

    // Set up the rest of TargetInfo.
    (*setTargetInfoFunc)(this);

    return true;
}

