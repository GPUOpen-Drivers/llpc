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
 * @file  TargetInfo.cpp
 * @brief LLPC source file: code to set up TargetInfo
 ***********************************************************************************************************************
 */
#include "lgc/state/TargetInfo.h"
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
//
// @param [in/out] targetInfo : Target info
static void setGfx6BaseInfo(TargetInfo *targetInfo) {
  // Initial settings (could be adjusted later according to graphics IP version info)
  targetInfo->getGpuProperty().waveSize = 64;

  targetInfo->getGpuProperty().numShaderEngines = 4;
  targetInfo->getGpuProperty().maxSgprsAvailable = 104;
  targetInfo->getGpuProperty().maxVgprsAvailable = 256;

  // TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
  targetInfo->getGpuProperty().gsPrimBufferDepth = 0x100;

  targetInfo->getGpuProperty().maxUserDataCount = 16; // GFX6-8 value

  targetInfo->getGpuProperty().gsOnChipMaxLdsSize = 16384;

  targetInfo->getGpuProperty().tessOffChipLdsBufferSize = 32768;

  // TODO: Accept gsOnChipDefaultPrimsPerSubgroup from panel option
  targetInfo->getGpuProperty().gsOnChipDefaultPrimsPerSubgroup = 64;

  targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 4096;

  // TODO: Accept gsOnChipDefaultLdsSizePerSubgroup from panel option
  targetInfo->getGpuProperty().gsOnChipDefaultLdsSizePerSubgroup = 8192;
}

// gfx6
//
// @param [in/out] targetInfo : Target info
static void setGfx6Info(TargetInfo *targetInfo) {
  setGfx6BaseInfo(targetInfo);
  targetInfo->getGpuProperty().ldsSizePerThreadGroup = 8192;
  targetInfo->getGpuProperty().ldsSizeDwordGranularityShift = 6;

  // Hardware workarounds for GFX6 based GPU's:
  targetInfo->getGpuWorkarounds().gfx6.cbNoLt16BitIntClamp = 1;
  targetInfo->getGpuWorkarounds().gfx6.miscLoadBalancePerWatt = 1;
  targetInfo->getGpuWorkarounds().gfx6.shader8b16bLocalWriteCorruption = 1;

  targetInfo->getGpuWorkarounds().gfx6.shaderReadlaneSmrd = 1;

  targetInfo->getGpuWorkarounds().gfx6.shaderSpiCsRegAllocFragmentation = 1;

  targetInfo->getGpuWorkarounds().gfx6.shaderVcczScalarReadBranchFailure = 1;

  targetInfo->getGpuWorkarounds().gfx6.shaderMinMaxFlushDenorm = 1;
}

// gfx600
//
// @param [in/out] targetInfo : Target info
static void setGfx600Info(TargetInfo *targetInfo) {
  setGfx6Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 2;
  targetInfo->getGpuWorkarounds().gfx6.shaderZExport = 1;
}

// gfx601
//
// @param [in/out] targetInfo : Target info
static void setGfx601Info(TargetInfo *targetInfo) {
  setGfx6Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuWorkarounds().gfx6.shaderZExport = 1;
}

// gfx602
//
// @param [in/out] targetInfo : Target info
static void setGfx602Info(TargetInfo *targetInfo) {
  setGfx6Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 1;
}

// gfx7+
//
// @param [in/out] targetInfo : Target info
static void setGfx7BaseInfo(TargetInfo *targetInfo) {
  setGfx6BaseInfo(targetInfo);
  targetInfo->getGpuProperty().ldsSizePerThreadGroup = 16384;
  targetInfo->getGpuProperty().ldsSizeDwordGranularityShift = 7;
}

// gfx7
//
// @param [in/out] targetInfo : Target info
static void setGfx7Info(TargetInfo *targetInfo) {
  setGfx7BaseInfo(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 1; // GFX7.0.2+ value

  // Hardware workarounds for GFX7 based GPU's:
  targetInfo->getGpuWorkarounds().gfx6.shaderVcczScalarReadBranchFailure = 1;
  targetInfo->getGpuWorkarounds().gfx6.shaderMinMaxFlushDenorm = 1;
}

// gfx700
//
// @param [in/out] targetInfo : Target info
static void setGfx700Info(TargetInfo *targetInfo) {
  setGfx7Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 2;

  // Hardware workarounds for GFX7.0.0
  targetInfo->getGpuWorkarounds().gfx6.cbNoLt16BitIntClamp = 1;
  // NOTE: Buffer store + index mode are not used in vulkan, so we can skip this workaround in safe.
  targetInfo->getGpuWorkarounds().gfx6.shaderCoalesceStore = 1;
}

// gfx701
//
// @param [in/out] targetInfo : Target info
static void setGfx701Info(TargetInfo *targetInfo) {
  setGfx7Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx703 and gfx704
//
// @param [in/out] targetInfo : Target info
static void setGfx703Info(TargetInfo *targetInfo) {
  setGfx7Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 4;

  // Hardware workarounds for GFX7.0.3 / GFX7.0.4
  targetInfo->getGpuWorkarounds().gfx6.cbNoLt16BitIntClamp = 1;
  targetInfo->getGpuWorkarounds().gfx6.shaderCoalesceStore = 1;
  targetInfo->getGpuWorkarounds().gfx6.shaderSpiBarrierMgmt = 1;
  targetInfo->getGpuWorkarounds().gfx6.shaderSpiCsRegAllocFragmentation = 1;
}

// gfx705
//
// @param [in/out] targetInfo : Target info
static void setGfx705Info(TargetInfo *targetInfo) {
  setGfx703Info(targetInfo);
  targetInfo->getGpuWorkarounds().gfx6.shaderSpiCsRegAllocFragmentation = 0;
}

// gfx8+
//
// @param [in/out] targetInfo : Target info
static void setGfx8BaseInfo(TargetInfo *targetInfo) {
  setGfx7BaseInfo(targetInfo);
  targetInfo->getGpuProperty().maxSgprsAvailable = 102;
  targetInfo->getGpuProperty().supportsDpp = true;
}

// gfx8
//
// @param [in/out] targetInfo : Target info
static void setGfx8Info(TargetInfo *targetInfo) {
  setGfx8BaseInfo(targetInfo);

  // Hardware workarounds for GFX8.x based GPU's:
  targetInfo->getGpuWorkarounds().gfx6.shaderMinMaxFlushDenorm = 1;

  targetInfo->getGpuWorkarounds().gfx6.shaderSmemBufferAddrClamp = 1;

  targetInfo->getGpuWorkarounds().gfx6.shaderEstimateRegisterUsage = 1;
}

// gfx800/gfx801
//
// @param [in/out] targetInfo : Target info
static void setGfx800Info(TargetInfo *targetInfo) {
  setGfx8Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().supportsXnack = 1;
}

// gfx802/gfx805
//
// @param [in/out] targetInfo : Target info
static void setGfx802Info(TargetInfo *targetInfo) {
  setGfx8Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 4;

  // Hardware workarounds
  targetInfo->getGpuWorkarounds().gfx6.miscSpiSgprsNum = 1;
}

// gfx803/gfx804
//
// @param [in/out] targetInfo : Target info
static void setGfx803Info(TargetInfo *targetInfo) {
  setGfx8Info(targetInfo);
  // TODO: polaris11 and polaris12 is 2, but we can't identify them by GFX IP now.
  targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx81
//
// @param [in/out] targetInfo : Target info
static void setGfx81Info(TargetInfo *targetInfo) {
  setGfx8Info(targetInfo);
  targetInfo->getGpuProperty().numShaderEngines = 1;
}

// gfx9+
//
// @param [in/out] targetInfo : Target info
static void setGfx9BaseInfo(TargetInfo *targetInfo) {
  setGfx8BaseInfo(targetInfo);
  targetInfo->getGpuProperty().maxUserDataCount = 32;
  targetInfo->getGpuProperty().gsOnChipDefaultLdsSizePerSubgroup = 0; // GFX9+ does not use this
  targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 8192;
  targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx9
//
// @param [in/out] targetInfo : Target info
static void setGfx9Info(TargetInfo *targetInfo) {
  setGfx9BaseInfo(targetInfo);

  targetInfo->getGpuProperty().supportsXnack = 1;

  // TODO: Clean up code for all 1d texture patch
  targetInfo->getGpuWorkarounds().gfx9.treat1dImagesAs2d = 1;

  targetInfo->getGpuWorkarounds().gfx9.shaderImageGatherInstFix = 1;

  targetInfo->getGpuWorkarounds().gfx9.fixCacheLineStraddling = 1;
}

// gfx900
//
// @param [in/out] targetInfo : Target info
static void setGfx900Info(TargetInfo *targetInfo) {
  setGfx9Info(targetInfo);
  targetInfo->getGpuWorkarounds().gfx9.fixLsVgprInput = 1;
}

// gfx906
//
// @param [in/out] targetInfo : Target info
static void setGfx906Info(TargetInfo *targetInfo) {
  setGfx9Info(targetInfo);

  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth16 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth8 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth4 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.sameSignedness = true;
}

// gfx10
//
// @param [in/out] targetInfo : Target info
static void setGfx10Info(TargetInfo *targetInfo) {
  setGfx9BaseInfo(targetInfo);
  targetInfo->getGpuProperty().maxSgprsAvailable = 106;

  targetInfo->getGpuProperty().supportsPermLane16 = true;
  targetInfo->getGpuProperty().supportsDppRowXmask = true;

  // Compiler is free to choose wave mode if forced wave size is not specified.
  if (NativeWaveSize != 0) {
    assert(NativeWaveSize == 32 || NativeWaveSize == 64);
    targetInfo->getGpuProperty().waveSize = NativeWaveSize;
  } else
    targetInfo->getGpuProperty().waveSize = 32;

  targetInfo->getGpuProperty().numShaderEngines = 2;
  targetInfo->getGpuProperty().supportShaderPowerProfiling = true;
  targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 8192;

  // Hardware workarounds for GFX10 based GPU's:
  targetInfo->getGpuWorkarounds().gfx10.waLimitedMaxOutputVertexCount = 1;
  targetInfo->getGpuWorkarounds().gfx10.waGeNggMaxVertOutWithGsInstancing = 1;
}

// gfx1010 (including gfx101E and gfx101F)
//
// @param [in/out] targetInfo : Target info
static void setGfx1010Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);

  targetInfo->getGpuProperty().supportsXnack = 1;

  targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetch0 = 1;
  targetInfo->getGpuWorkarounds().gfx10.waDidtThrottleVmem = 1;
  targetInfo->getGpuWorkarounds().gfx10.waLdsVmemNotWaitingVmVsrc = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNsaAndClauseCanHang = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNsaCannotFollowWritelane = 1;
  targetInfo->getGpuWorkarounds().gfx10.waTessIncorrectRelativeIndex = 1;
  targetInfo->getGpuWorkarounds().gfx10.waSmemFollowedByVopc = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups = 1;
  targetInfo->getGpuWorkarounds().gfx10.waFixBadImageDescriptor = 1;
}

// gfx1011
//
// @param [in/out] targetInfo : Target info
static void setGfx1011Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);

  targetInfo->getGpuProperty().supportsXnack = 1;

  targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetch0 = 1;
  targetInfo->getGpuWorkarounds().gfx10.waDidtThrottleVmem = 1;
  targetInfo->getGpuWorkarounds().gfx10.waLdsVmemNotWaitingVmVsrc = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNsaCannotFollowWritelane = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNsaAndClauseCanHang = 1;
  targetInfo->getGpuWorkarounds().gfx10.waSmemFollowedByVopc = 1;
  targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetchFwd64 = 1;
  targetInfo->getGpuWorkarounds().gfx10.waWarFpAtomicDenormHazard = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups = 1;
  targetInfo->getGpuWorkarounds().gfx10.waFixBadImageDescriptor = 1;

  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth16 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth8 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth4 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.sameSignedness = true;
}

// gfx1012
//
// @param [in/out] targetInfo : Target info
static void setGfx1012Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);

  targetInfo->getGpuProperty().supportsXnack = 1;

  targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetch0 = 1;
  targetInfo->getGpuWorkarounds().gfx10.waDidtThrottleVmem = 1;
  targetInfo->getGpuWorkarounds().gfx10.waLdsVmemNotWaitingVmVsrc = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNsaCannotFollowWritelane = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNsaAndClauseCanHang = 1;
  targetInfo->getGpuWorkarounds().gfx10.waThrottleInMultiDwordNsa = 1;
  targetInfo->getGpuWorkarounds().gfx10.waSmemFollowedByVopc = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups = 1;
  targetInfo->getGpuWorkarounds().gfx10.waShaderInstPrefetchFwd64 = 1;
  targetInfo->getGpuWorkarounds().gfx10.waWarFpAtomicDenormHazard = 1;
  targetInfo->getGpuWorkarounds().gfx10.waNggDisabled = 1;
  targetInfo->getGpuWorkarounds().gfx10.waFixBadImageDescriptor = 1;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth16 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth8 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth4 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.sameSignedness = true;
}

// gfx103
//
// @param [in/out] targetInfo : Target info
static void setGfx103Info(TargetInfo *targetInfo) {
  // Hardware workarounds for GFX10.3 based GPU's:
  targetInfo->getGpuWorkarounds().gfx10.waAdjustDepthImportVrs = 1;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth16 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth8 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth4 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.sameSignedness = true;
}

// gfx1030
//
// @param [in/out] targetInfo : Target info
static void setGfx1030Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 4;
}

// gfx1031
//
// @param [in/out] targetInfo : Target info
static void setGfx1031Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 2;
}

// gfx1032
//
// @param [in/out] targetInfo : Target info
static void setGfx1032Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 2;
  targetInfo->getGpuWorkarounds().gfx10.waClearWriteCompressBit = 1;
}

// gfx1034
//
// @param [in/out] targetInfo : Target info
static void setGfx1034Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 1;
}

// gfx11
//
// @param [in/out] targetInfo : Target info
static void setGfx11Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);

  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth16 = false;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth8 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.compBitwidth4 = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.sameSignedness = true;
  targetInfo->getGpuProperty().supportIntegerDotFlag.diffSignedness = true;
}

// gfx1100
//
// @param [in/out] targetInfo : Target info
static void setGfx1100Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waUserSgprInitBug = 1;
  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 6;
}

// =====================================================================================================================
// Set TargetInfo. Returns false if the GPU name is not found or not supported.
//
// @param gpuName : LLVM GPU name, e.g. "gfx900"
bool TargetInfo::setTargetInfo(StringRef gpuName) {
  struct GpuNameStringMap {
    const char *gpuName;
    void (*setTargetInfoFunc)(TargetInfo *targetInfo);
  };

  static const GpuNameStringMap GpuNameMap[] = {
      {"gfx600", &setGfx600Info},   // gfx600, tahiti
      {"gfx601", &setGfx601Info},   // gfx601, pitcairn, verde
      {"gfx602", &setGfx602Info},   // gfx601, oland, hainan
      {"gfx700", &setGfx700Info},   // gfx700, kaveri
      {"gfx701", &setGfx701Info},   // gfx701, hawaii
      {"gfx702", &setGfx7Info},     // gfx702
      {"gfx703", &setGfx703Info},   // gfx703, kabini, mullins
      {"gfx704", &setGfx703Info},   // gfx704, bonaire
      {"gfx705", &setGfx705Info},   // gfx705
      {"gfx800", &setGfx800Info},   // gfx800, iceland
      {"gfx801", &setGfx800Info},   // gfx801, carrizo
      {"gfx802", &setGfx802Info},   // gfx802, tonga
      {"gfx803", &setGfx803Info},   // gfx803, fiji, polaris10, polaris11
      {"gfx804", &setGfx803Info},   // gfx804
      {"gfx805", &setGfx802Info},   // gfx805, tongapro
      {"gfx810", &setGfx81Info},    // gfx810, stoney
      {"gfx900", &setGfx900Info},   // gfx900
      {"gfx901", &setGfx9Info},     // gfx901
      {"gfx902", &setGfx900Info},   // gfx902
      {"gfx903", &setGfx9Info},     // gfx903
      {"gfx904", &setGfx9Info},     // gfx904, vega12
      {"gfx906", &setGfx906Info},   // gfx906, vega20
      {"gfx909", &setGfx9Info},     // gfx909, raven2
      {"gfx90c", &setGfx9Info},     // gfx90c
      {"gfx1010", &setGfx1010Info}, // gfx1010
      {"gfx1011", &setGfx1011Info}, // gfx1011, navi12
      {"gfx1012", &setGfx1012Info}, // gfx1012, navi14
      {"gfx1030", &setGfx1030Info}, // gfx1030, navi21
      {"gfx1031", &setGfx1031Info}, // gfx1031, navi22
      {"gfx1032", &setGfx1032Info}, // gfx1032, navi23
      {"gfx1034", &setGfx1034Info}, // gfx1034, navi24
      {"gfx1100", &setGfx1100Info}, // gfx1100, navi31
  };

  void (*setTargetInfoFunc)(TargetInfo * targetInfo) = nullptr;
  for (const GpuNameStringMap &mapEntry : ArrayRef<GpuNameStringMap>(GpuNameMap)) {
    if (gpuName == mapEntry.gpuName) {
      setTargetInfoFunc = mapEntry.setTargetInfoFunc;
      break;
    }
  }
  if (!setTargetInfoFunc)
    return false; // Target not supported

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
