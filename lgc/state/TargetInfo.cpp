/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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

namespace llvm {
namespace cl {
// Define a category for Helper options.
OptionCategory AmdCategory{"Helper Options"};
} // namespace cl
} // namespace llvm

// -native-wave-size: an option to override hardware native wave size, it will allow compiler to choose
// final wave size base on it. Used in pre-silicon verification.
static cl::opt<int> NativeWaveSize("native-wave-size", cl::desc("Overrides hardware native wave size"), cl::init(0));

namespace {

class TargetInfoPrinter {
public:
  void print();

  void operator=(bool value) {
    if (!value)
      return;
    print();
    exit(0);
  }
};

TargetInfoPrinter TargetInfoPrinterInstance;

cl::opt<TargetInfoPrinter, true, cl::parser<bool>> TargetPrinter{
    "targetInfo", cl::desc("Display the supported device infos."), cl::location(TargetInfoPrinterInstance),
    cl::cat(cl::AmdCategory)};
} // namespace

// =====================================================================================================================
// Functions to set up TargetInfo for the various targets

// gfx10+
//
// @param [in/out] targetInfo : Target info
static void setGfx10BaseInfo(TargetInfo *targetInfo) {
  // Initial settings (could be adjusted later according to graphics IP version info)
  targetInfo->getGpuProperty().waveSize = 64;

  targetInfo->getGpuProperty().numShaderEngines = 4;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 10;
  targetInfo->getGpuProperty().maxSgprsAvailable = 104;
  targetInfo->getGpuProperty().maxVgprsAvailable = 256;

  // TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
  targetInfo->getGpuProperty().gsPrimBufferDepth = 0x100;

  targetInfo->getGpuProperty().gsOnChipMaxLdsSize = 16384;

  targetInfo->getGpuProperty().tessOffChipLdsBufferSize = 32768;

  // TODO: Accept gsOnChipDefaultPrimsPerSubgroup from panel option
  targetInfo->getGpuProperty().gsOnChipDefaultPrimsPerSubgroup = 64;

  targetInfo->getGpuProperty().ldsSizePerThreadGroup = 16384;

  targetInfo->getGpuProperty().maxSgprsAvailable = 102;
  targetInfo->getGpuProperty().supportsDpp = true;

  targetInfo->getGpuProperty().maxUserDataCount = 32;
  targetInfo->getGpuProperty().gsOnChipDefaultLdsSizePerSubgroup = 8192;
  targetInfo->getGpuProperty().tessFactorBufferSizePerSe = 8192;
  targetInfo->getGpuProperty().maxMsaaRasterizerSamples = 16;
}

// gfx10
//
// @param [in/out] targetInfo : Target info
static void setGfx10Info(TargetInfo *targetInfo) {
  setGfx10BaseInfo(targetInfo);
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
  targetInfo->getGpuWorkarounds().gfx10.waNggPassthroughMessageHazard = 1;
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
  targetInfo->getGpuWorkarounds().gfx10.waNggPassthroughMessageHazard = 1;
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

  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 14;
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
  targetInfo->getGpuWorkarounds().gfx10.waNggPassthroughMessageHazard = 1;
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
  targetInfo->getGpuProperty().supportsRbPlus = true;
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
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
  targetInfo->getGpuWorkarounds().gfx10.waClearWriteCompressBit = 1;
}

// gfx1034
//
// @param [in/out] targetInfo : Target info
static void setGfx1034Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
}

// gfx1035
//
// @param [in/out] targetInfo : Target info
static void setGfx1035Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 6;
  targetInfo->getGpuWorkarounds().gfx10.waClearWriteCompressBit = 1;
}

// gfx1036
//
// @param [in/out] targetInfo : Target info
static void setGfx1036Info(TargetInfo *targetInfo) {
  setGfx10Info(targetInfo);
  setGfx103Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 2;
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
  targetInfo->getGpuProperty().supportsRbPlus = true;
}

// gfx1100
//
// @param [in/out] targetInfo : Target info
static void setGfx1100Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waUserSgprInitBug = 1;
  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 6;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
}

// gfx1101
//
// @param [in/out] targetInfo : Target info
static void setGfx1101Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 3;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 10;
}

// gfx1102
//
// @param [in/out] targetInfo : Target info
static void setGfx1102Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waUserSgprInitBug = 1;
  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 2;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
}

// gfx1103
//
// @param [in/out] targetInfo : Target info
static void setGfx1103Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 6;
}

#if LLPC_BUILD_STRIX1
// gfx1150
//
// @param [in/out] targetInfo : Target info
static void setGfx1150Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
}

// gfx115F
//
// @param [in/out] targetInfo : Target info
static void setGfx115FInfo(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
}
#endif

#if LLPC_BUILD_STRIX_HALO
// gfx1151
//
// @param [in/out] targetInfo : Target info
static void setGfx1151Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);

  targetInfo->getGpuWorkarounds().gfx11.waAtmPrecedesPos = 1;

  targetInfo->getGpuProperty().numShaderEngines = 1;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 10;
}
#endif

// gfx12
//
// @param [in/out] targetInfo : Target info
static void setGfx12Info(TargetInfo *targetInfo) {
  setGfx11Info(targetInfo);
}

#if LLPC_BUILD_NAVI48
// gfx1201
//
// @param [in/out] targetInfo : Target info
static void setGfx1201Info(TargetInfo *targetInfo) {
  setGfx12Info(targetInfo);

  targetInfo->getGpuProperty().numShaderEngines = 4;
  targetInfo->getGpuProperty().numComputeUnitsPerShaderEngine = 8;
  targetInfo->getGpuWorkarounds().gfx12.waNoReZSupport = 1;
}

#endif

// Represents device infos.
struct GpuNameStringMap {
  const char *gpuName;
  const char *deviceName;
  void (*setTargetInfoFunc)(TargetInfo *targetInfo);
};

// The supported device list
static const GpuNameStringMap GpuNameMap[] = {
    {"gfx1010", "Navi10", &setGfx1010Info},    // gfx1010
    {"gfx1011", "Navi12", &setGfx1011Info},    // gfx1011
    {"gfx1012", "Navi14", &setGfx1012Info},    // gfx1012
    {"gfx1030", "Navi21", &setGfx1030Info},    // gfx1030
    {"gfx1031", "Navi22", &setGfx1031Info},    // gfx1031
    {"gfx1032", "Navi23", &setGfx1032Info},    // gfx1032
    {"gfx1034", "Navi24", &setGfx1034Info},    // gfx1034
    {"gfx1035", "Rembrandt", &setGfx1035Info}, // gfx1035
    {"gfx1036", "Raphael", &setGfx1036Info},   // gfx1036
    {"gfx1100", "Navi31", &setGfx1100Info},    // gfx1100
    {"gfx1101", "Navi32", &setGfx1101Info},    // gfx1101
    {"gfx1102", "Navi33", &setGfx1102Info},    // gfx1102
    {"gfx1103", "Phoenix1", &setGfx1103Info},  // gfx1103
#if LLPC_BUILD_STRIX1
    {"gfx1150", "Strix1", &setGfx1150Info},    // gfx1150
    {"gfx115F", "Strix1 A0", &setGfx115FInfo}, // gfx115F
#endif
#if LLPC_BUILD_STRIX_HALO
    {"gfx1151", "Strix_halo", &setGfx1151Info}, // gfx1151
#endif
#if LLPC_BUILD_NAVI48
    {"gfx1201", "Navi48", &setGfx1201Info}, // gfx1201
#endif
};

// =====================================================================================================================
// Set TargetInfo. Returns false if the GPU name is not found or not supported.
//
// @param gpuName : LLVM GPU name, e.g. "gfx900"
bool TargetInfo::setTargetInfo(StringRef gpuName) {
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

// =====================================================================================================================
// Print the target infos
void TargetInfoPrinter::print() {
  unsigned count = sizeof(GpuNameMap) / sizeof(GpuNameMap[0]);
  for (unsigned i = 0; i < count; ++i) {
    // Remove substring "gfx"
    outs() << StringRef(GpuNameMap[i].gpuName).drop_front(3) << "  " << GpuNameMap[i].deviceName << '\n';
  }
}
