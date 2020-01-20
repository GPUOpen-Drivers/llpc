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
 * @file  llpcTargetInfo.h
 * @brief LLPC header file: declaration of TargetInfo struct
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcDebug.h"
#include "llvm/ADT/StringRef.h"

namespace Llpc
{

using namespace llvm;

// Represents the properties of GPU device.
struct GpuProperty
{
    uint32_t numShaderEngines;                  // Number of shader engines present
    uint32_t waveSize;                          // Wavefront size
    uint32_t ldsSizePerCu;                      // LDS size per compute unit
    uint32_t ldsSizePerThreadGroup;             // LDS size per thread group
    uint32_t gsOnChipDefaultPrimsPerSubgroup;   // Default target number of primitives per subgroup for GS on-chip mode.
    uint32_t gsOnChipDefaultLdsSizePerSubgroup; // Default value for the maximum LDS size per subgroup for
    uint32_t gsOnChipMaxLdsSize;                // Max LDS size used by GS on-chip mode (in DWORDs)
    uint32_t ldsSizeDwordGranularityShift;      // Amount of bits used to shift the LDS_SIZE register field

    //TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
    uint32_t gsPrimBufferDepth;     // Comes from the hardware GPU__GC__GSPRIM_BUFF_DEPTH configuration option

    uint32_t maxUserDataCount;                  // Max allowed count of user data SGPRs
    uint32_t tessOffChipLdsBufferSize;          // Off-chip Tess Buffer Size
    uint32_t maxSgprsAvailable;                 // Number of max available SGPRs
    uint32_t maxVgprsAvailable;                 // Number of max available VGPRs
    uint32_t tessFactorBufferSizePerSe;         // Size of the tessellation-factor buffer per SE, in DWORDs.
#if LLPC_BUILD_GFX10
    bool     supportShaderPowerProfiling;       // Hardware supports Shader Profiling for Power
    bool     supportSpiPrefPriority;            // Hardware supports SPI shader preference priority
#endif
};

// Contains flags for all of the hardware workarounds which affect pipeline compilation.
struct WorkaroundFlags
{
    union
    {
        struct
        {
            uint32_t  cbNoLt16BitIntClamp               :  1;
            uint32_t  miscLoadBalancePerWatt            :  1;
            uint32_t  miscSpiSgprsNum                   :  1;
            uint32_t  shader8b16bLocalWriteCorruption   :  1;
            uint32_t  shaderCoalesceStore               :  1;
            uint32_t  shaderEstimateRegisterUsage       :  1;
            uint32_t  shaderReadlaneSmrd                :  1;
            uint32_t  shaderSmemBufferAddrClamp         :  1;
            uint32_t  shaderSpiBarrierMgmt              :  1;

            uint32_t  shaderSpiCsRegAllocFragmentation  :  1;
            uint32_t  shaderVcczScalarReadBranchFailure :  1;
            uint32_t  shaderZExport                     :  1;
            // Pre-GFX9 hardware doesn't support min/max denorm flush, we insert extra fmul with 1.0 to flush the denorm value
            uint32_t  shaderMinMaxFlushDenorm           :  1;
            uint32_t  reserved                          : 19;
        };
        uint32_t  u32All;
    } gfx6;

    union
    {
        struct
        {
            uint32_t  fixCacheLineStraddling       :  1;
            uint32_t  fixLsVgprInput               :  1;
            uint32_t  shaderImageGatherInstFix     :  1;
            uint32_t  treat1dImagesAs2d            :  1;
            uint32_t  reserved                     : 28;
        };
        uint32_t  u32All;
    } gfx9;

#if LLPC_BUILD_GFX10
    union
    {
        struct
        {
            uint32_t  disableI32ModToI16Mod        :  1;

            uint32_t  waTessFactorBufferSizeLimitGeUtcl1Underflow :  1;
            uint32_t  waTessIncorrectRelativeIndex :  1;
            uint32_t  waShaderInstPrefetch123      :  1;
            uint32_t  waShaderInstPrefetch0        :  1;
            uint32_t  nggTessDegeneratePrims       :  1;
            uint32_t  waDidtThrottleVmem           :  1;
            uint32_t  waLdsVmemNotWaitingVmVsrc    :  1;
            uint32_t  waNsaCannotBeLastInClause    :  1;
            uint32_t  waNsaAndClauseCanHang        :  1;
            uint32_t  waNsaCannotFollowWritelane   :  1;
            uint32_t  waThrottleInMultiDwordNsa    :  1;
            uint32_t  waSmemFollowedByVopc         :  1;
            uint32_t  waNggCullingNoEmptySubgroups :  1;
            uint32_t  waShaderInstPrefetchFwd64    :  1;
            uint32_t  waWarFpAtomicDenormHazard    :  1;
            uint32_t  waNggDisabled                :  1;
            uint32_t  reserved                     : 15;
        };
        uint32_t u32All;
    } gfx10;
#endif
};

// =====================================================================================================================
// TargetInfo class, representing features and workarounds for the particular selected target
class TargetInfo
{
public:
    // Set TargetInfo. Returns false if the GPU name is not found or not supported.
    bool SetTargetInfo(StringRef gpuName);

    // Accessors.
    GfxIpVersion GetGfxIpVersion() const { return m_gfxIp; }
    GpuProperty& GetGpuProperty() { return m_gpuProperty; }
    const GpuProperty& GetGpuProperty() const { return m_gpuProperty; }
    WorkaroundFlags& GetGpuWorkarounds() { return m_gpuWorkarounds; }
    const WorkaroundFlags& GetGpuWorkarounds() const { return m_gpuWorkarounds; }

private:
    GfxIpVersion    m_gfxIp = {};          // major.minor.stepping
    GpuProperty     m_gpuProperty = {};    // GPU properties
    WorkaroundFlags m_gpuWorkarounds = {}; // GPU workarounds
};

} // Llpc
