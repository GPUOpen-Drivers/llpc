/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  TargetInfo.h
 * @brief LLPC header file: declaration of TargetInfo struct
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"

namespace lgc {

// Represents graphics IP version info. See https://llvm.org/docs/AMDGPUUsage.html#processors for more
// details.
struct GfxIpVersion {
  unsigned major;    // Major version
  unsigned minor;    // Minor version
  unsigned stepping; // Stepping info

  // GFX IP checkers
  bool operator==(const GfxIpVersion &rhs) const {
    return std::tie(major, minor, stepping) == std::tie(rhs.major, rhs.minor, rhs.stepping);
  }
  bool operator>=(const GfxIpVersion &rhs) const {
    return std::tie(major, minor, stepping) >= std::tie(rhs.major, rhs.minor, rhs.stepping);
  }
};

// Represents the properties of GPU device.
struct GpuProperty {
  unsigned numShaderEngines;                  // Number of shader engines present
  unsigned waveSize;                          // Wavefront size
  unsigned ldsSizePerThreadGroup;             // LDS size per thread group in dwords
  unsigned gsOnChipDefaultPrimsPerSubgroup;   // Default target number of primitives per subgroup for GS on-chip mode.
  unsigned gsOnChipDefaultLdsSizePerSubgroup; // Default value for the maximum LDS size per subgroup for
  unsigned gsOnChipMaxLdsSize;                // Max LDS size used by GS on-chip mode (in dwords)
  unsigned ldsSizeDwordGranularityShift;      // Amount of bits used to shift the LDS_SIZE register field

  // TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
  unsigned gsPrimBufferDepth; // Comes from the hardware GPU__GC__GSPRIM_BUFF_DEPTH configuration option

  unsigned maxUserDataCount;          // Max allowed count of user data SGPRs
  unsigned tessOffChipLdsBufferSize;  // Off-chip Tess Buffer Size
  unsigned maxSgprsAvailable;         // Number of max available SGPRs
  unsigned maxVgprsAvailable;         // Number of max available VGPRs
  unsigned tessFactorBufferSizePerSe; // Size of the tessellation-factor buffer per SE, in dwords.
  bool supportShaderPowerProfiling;   // Hardware supports Shader Profiling for Power
  struct {
    unsigned compBitwidth16 : 1; // Whether the vector is 16-bit component
    unsigned compBitwidth8 : 1;  // Whether the vector is 8-bit component
    unsigned compBitwidth4 : 1;  // Whether the vector is 4-bit component
    unsigned sameSignedness : 1; // Whether the components of two vectors have the same signedness
    unsigned diffSignedness : 1; // Whether the components of two vectors have the diff signedness
  } supportIntegerDotFlag;       // The flag indicates the HW supports integer dot product
  unsigned supportsXnack;        // GPU supports XNACK
  bool supportsDpp;              // GPU supports DPP
  bool supportsDppRowXmask;      // GPU supports DPP ROW_XMASK
  bool supportsPermLane16;       // GPU supports perm lane 16
};

// Contains flags for all of the hardware workarounds which affect pipeline compilation.
struct WorkaroundFlags {
  union {
    struct {
      unsigned cbNoLt16BitIntClamp : 1;
      unsigned miscLoadBalancePerWatt : 1;
      unsigned miscSpiSgprsNum : 1;
      unsigned shader8b16bLocalWriteCorruption : 1;
      unsigned shaderCoalesceStore : 1;
      unsigned shaderEstimateRegisterUsage : 1;
      unsigned shaderReadlaneSmrd : 1;
      unsigned shaderSmemBufferAddrClamp : 1;
      unsigned shaderSpiBarrierMgmt : 1;

      unsigned shaderSpiCsRegAllocFragmentation : 1;
      unsigned shaderVcczScalarReadBranchFailure : 1;
      unsigned shaderZExport : 1;
      // Pre-GFX9 hardware doesn't support min/max denorm flush, we insert extra fmul with 1.0 to flush the denorm value
      unsigned shaderMinMaxFlushDenorm : 1;
      unsigned reserved : 19;
    };
    unsigned u32All;
  } gfx6;

  union {
    struct {
      unsigned fixCacheLineStraddling : 1;
      unsigned fixLsVgprInput : 1;
      unsigned shaderImageGatherInstFix : 1;
      unsigned treat1dImagesAs2d : 1;
      unsigned reserved : 28;
    };
    unsigned u32All;
  } gfx9;

  union {
    struct {
      unsigned waTessFactorBufferSizeLimitGeUtcl1Underflow : 1;
      unsigned waTessIncorrectRelativeIndex : 1;
      unsigned waShaderInstPrefetch123 : 1;
      unsigned waShaderInstPrefetch0 : 1;
      unsigned nggTessDegeneratePrims : 1;
      unsigned waDidtThrottleVmem : 1;
      unsigned waLdsVmemNotWaitingVmVsrc : 1;
      unsigned waNsaCannotBeLastInClause : 1;
      unsigned waNsaAndClauseCanHang : 1;
      unsigned waNsaCannotFollowWritelane : 1;
      unsigned waThrottleInMultiDwordNsa : 1;
      unsigned waSmemFollowedByVopc : 1;
      unsigned waNggCullingNoEmptySubgroups : 1;
      unsigned waShaderInstPrefetchFwd64 : 1;
      unsigned waWarFpAtomicDenormHazard : 1;
      unsigned waNggDisabled : 1;
      unsigned waFixBadImageDescriptor : 1;
      unsigned waLimitedMaxOutputVertexCount : 1;
      unsigned waGeNggMaxVertOutWithGsInstancing : 1;
      unsigned waAdjustDepthImportVrs : 1;
      // Clear write compress bit in an image descriptor being used for a read operation.
      unsigned waClearWriteCompressBit : 1;
      unsigned reserved : 11;
    };
    unsigned u32All;
  } gfx10;
};

// =====================================================================================================================
// TargetInfo class, representing features and workarounds for the particular selected target
class TargetInfo {
public:
  // Set TargetInfo. Returns false if the GPU name is not found or not supported.
  bool setTargetInfo(llvm::StringRef gpuName);

  // Accessors.
  GfxIpVersion getGfxIpVersion() const { return m_gfxIp; }
  GpuProperty &getGpuProperty() { return m_gpuProperty; }
  const GpuProperty &getGpuProperty() const { return m_gpuProperty; }
  WorkaroundFlags &getGpuWorkarounds() { return m_gpuWorkarounds; }
  const WorkaroundFlags &getGpuWorkarounds() const { return m_gpuWorkarounds; }

private:
  GfxIpVersion m_gfxIp = {};             // major.minor.stepping
  GpuProperty m_gpuProperty = {};        // GPU properties
  WorkaroundFlags m_gpuWorkarounds = {}; // GPU workarounds
};

} // namespace lgc
