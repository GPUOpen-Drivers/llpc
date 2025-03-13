/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  NggPrimShader.cpp
 * @brief LLPC source file: contains implementation of class lgc::NggPrimShader.
 ***********************************************************************************************************************
 */
#include "NggPrimShader.h"
#include "ShaderMerger.h"
#include "lgc/Debug.h"
#include "lgc/LgcDialect.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PalMetadata.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "lgc-ngg-prim-shader"

using namespace llvm;

// -ngg-small-subgroup-threshold: threshold of vertex count to determine a small subgroup (NGG)
static cl::opt<unsigned> NggSmallSubgroupThreshold(
    "ngg-small-subgroup-threshold",
    cl::desc(
        "Threshold of vertex count to determine a small subgroup and such small subgroup won't perform NGG culling"),
    cl::value_desc("threshold"), cl::init(16));

namespace lgc {

// List of names of handler functions
static const char NggEsMain[] = "lgc.ngg.ES.main";
static const char NggEsFirstPart[] = "lgc.ngg.ES.first.part";
static const char NggEsSecondPart[] = "lgc.ngg.ES.second.part";

static const char NggGsMain[] = "lgc.ngg.GS.main";
static const char NggCopyShader[] = "lgc.ngg.COPY.main";
static const char NggGsEmit[] = "lgc.ngg.GS.emit";
static const char NggGsCut[] = "lgc.ngg.GS.cut";

static const char NggCullerBackface[] = "lgc.ngg.culler.backface";
static const char NggCullerFrustum[] = "lgc.ngg.culler.frustum";
static const char NggCullerBoxFilter[] = "lgc.ngg.culler.box.filter";
static const char NggCullerSphere[] = "lgc.ngg.culler.sphere";
static const char NggCullerSmallPrimFilter[] = "lgc.ngg.culler.small.prim.filter";
static const char NggCullerCullDistance[] = "lgc.ngg.culler.cull.distance";
static const char NggCullerRegFetcher[] = "lgc.ngg.culler.reg.fetcher";

static const char NggExportCollector[] = "lgc.ngg.export.collector";

// Represents GDS GRBM register for SW-emulated stream-out
enum {
  // For 4 stream-out buffers
  GDS_STRMOUT_DWORDS_WRITTEN_0 = 0,
  GDS_STRMOUT_DWORDS_WRITTEN_1 = 1,
  GDS_STRMOUT_DWORDS_WRITTEN_2 = 2,
  GDS_STRMOUT_DWORDS_WRITTEN_3 = 3,
  // For 4 stream-out streams
  GDS_STRMOUT_PRIMS_NEEDED_0 = 8,
  GDS_STRMOUT_PRIMS_WRITTEN_0 = 9,
  GDS_STRMOUT_PRIMS_NEEDED_1 = 10,
  GDS_STRMOUT_PRIMS_WRITTEN_1 = 11,
  GDS_STRMOUT_PRIMS_NEEDED_2 = 12,
  GDS_STRMOUT_PRIMS_WRITTEN_2 = 13,
  GDS_STRMOUT_PRIMS_NEEDED_3 = 14,
  GDS_STRMOUT_PRIMS_WRITTEN_3 = 15,
};

// =====================================================================================================================
//
// @param pipelineState : Pipeline state
NggPrimShader::NggPrimShader(PipelineState *pipelineState)
    : m_pipelineState(pipelineState), m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()),
      m_nggControl(m_pipelineState->getNggControl()), m_hasVs(pipelineState->hasShaderStage(ShaderStage::Vertex)),
      m_hasTes(pipelineState->hasShaderStage(ShaderStage::TessEval)),
      m_hasGs(pipelineState->hasShaderStage(ShaderStage::Geometry)), m_builder(pipelineState->getContext()) {
  assert(m_nggControl->enableNgg);

  m_maxThreadsPerSubgroup = NggMaxThreadsPerSubgroup;
  if (m_hasGs) {
    // NOTE: Normally, the maximum value of GS output vertices is restricted to 256 by HW rasterization. However, we
    // encounter a special DX case where it emits >256 vertices and just do stream-out operations without
    // rasterization. Stream-out on GFX11+ is pure SW emulation and we can support such case. In experiments, we
    // find our HW can support GE_NGG_SUBGRP_CNTL.PRIM_AMP_FACTOR > 256 though it is not documented. There are 9 bits
    // that program the register field to launch 511 threads at most. With sufficient threads, this case could be
    // handled by our current design.
    const auto &geometryMode = pipelineState->getShaderModes()->getGeometryShaderMode();
    m_maxThreadsPerSubgroup = std::max(NggMaxThreadsPerSubgroup, geometryMode.outputVertices);
    assert(m_maxThreadsPerSubgroup <= NggMaxPrimitiveAmplifier);
  }
  const unsigned waveSize = pipelineState->getShaderWaveSize(
      m_hasGs ? ShaderStage::Geometry : (m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex));
  m_maxWavesPerSubgroup = alignTo(m_maxThreadsPerSubgroup, waveSize) / waveSize;

  // Always allow approximation, to change fdiv(1.0, x) to rcp(x)
  FastMathFlags fastMathFlags;
  fastMathFlags.setApproxFunc();
  m_builder.setFastMathFlags(fastMathFlags);

  assert(m_pipelineState->isGraphics());

  // NOTE: For NGG with API GS, we change data layout of output vertices. They are grouped by vertex streams now.
  // Vertices belonging to different vertex streams are in different regions of GS-VS ring. Here, we calculate
  // the base offset of each vertex streams and record them. See 'writeGsOutput' for detail.
  if (m_hasGs) {
    unsigned vertexItemSizes[MaxGsStreams] = {};
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry);
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      vertexItemSizes[i] = resUsage->inOutUsage.gs.hwConfig.gsVsVertexItemSize[i];

    unsigned gsVsRingItemSizes[MaxGsStreams] = {};
    const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      gsVsRingItemSizes[i] = vertexItemSizes[i] * geometryMode.outputVertices;

    const unsigned gsPrimsPerSubgroup = resUsage->inOutUsage.gs.hwConfig.gsPrimsPerSubgroup * geometryMode.invocations;
    unsigned gsStreamBase = 0;
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      m_gsStreamBases[i] = gsStreamBase;
      gsStreamBase += gsVsRingItemSizes[i] * gsPrimsPerSubgroup;
    }
  }

  buildPrimShaderCbLayoutLookupTable();
}

// =====================================================================================================================
// Calculates the dword size of ES-GS ring item.
//
// @param pipelineState : Pipeline state
// @param esMain : ES main function
// @returns : ES-GS ring item size in dwords
unsigned NggPrimShader::calcEsGsRingItemSize(PipelineState *pipelineState, Function *esMain) {
  assert(pipelineState->getNggControl()->enableNgg); // Must enable NGG

  // API GS is present
  if (pipelineState->hasShaderStage(ShaderStage::Geometry)) {
    auto resUsage = pipelineState->getShaderResourceUsage(ShaderStage::Geometry);
    // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
    return (4 * std::max(1u, resUsage->inOutUsage.inputMapLocCount)) | 1;
  }

  // Passthrough mode is enabled (API GS is not present)
  if (pipelineState->getNggControl()->passthroughMode) {
    unsigned esGsRingItemSize = 1;

    if (pipelineState->enableSwXfb())
      esGsRingItemSize = calcEsXfbOutputsSize(esMain);

    // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
    return esGsRingItemSize | 1;
  }

  // Culling mode is enabled (API GS is not present)
  VertexCullInfoOffsets vertCullInfoOffsets = {}; // Dummy offsets (don't care)
  // In the culling mode, the ES-GS ring item is vertex cull info.
  unsigned esGsRingItemSize = calcVertexCullInfoSizeAndOffsets(pipelineState, esMain, vertCullInfoOffsets);

  // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
  return esGsRingItemSize | 1;
}

// =====================================================================================================================
// Layout primitive shader LDS if 'ldsLayout' is specified and calculate the required total LDS size (in dwords).
//
// @param pipelineState : Pipeline state
// @param ldsLayout : Primitive shader LDS layout (could be null)
PrimShaderLdsUsageInfo NggPrimShader::layoutPrimShaderLds(PipelineState *pipelineState,
                                                          PrimShaderLdsLayout *ldsLayout) {
  assert(pipelineState->getNggControl()->enableNgg); // Must enable NGG

  const auto &hwConfig = pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;

  unsigned ldsOffset = 0;     // In dwords
  unsigned ldsRegionSize = 0; // In dwords

  auto printLdsRegionInfo = [=](const char *regionName, unsigned regionOffset, unsigned regionSize) {
    LLPC_OUTS(format("%-40s : offset = 0x%04" PRIX32 ", size = 0x%04" PRIX32, regionName, regionOffset, regionSize));
    if (regionSize == 0)
      LLPC_OUTS(" (empty)");
    LLPC_OUTS("\n");
  };

  if (ldsLayout) {
    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC primitive shader LDS region info (in dwords) and general usage info\n\n");
  }

  //
  // API GS is present
  //
  if (pipelineState->hasShaderStage(ShaderStage::Geometry)) {
    PrimShaderLdsUsageInfo ldsUsageInfo = {};
    ldsUsageInfo.needsLds = true;

    // NOTE: Normally, the maximum value of GS output vertices is restricted to 256 by HW rasterization. However, we
    // encounter a special DX case where it emits >256 vertices and just do stream-out operations without
    // rasterization. Stream-out on GFX11+ is pure SW emulation and we can support such case. In experiments, we
    // find our HW can support GE_NGG_SUBGRP_CNTL.PRIM_AMP_FACTOR > 256 though it is not documented. There are 9 bits
    // that program the register field to launch 511 threads at most. With sufficient threads, this case could be
    // handled by our current design.
    const auto &geometryMode = pipelineState->getShaderModes()->getGeometryShaderMode();
    unsigned maxThreadsPerSubgroup = std::max(NggMaxThreadsPerSubgroup, geometryMode.outputVertices);
    assert(maxThreadsPerSubgroup <= NggMaxPrimitiveAmplifier);

    const unsigned waveSize = pipelineState->getShaderWaveSize(ShaderStage::Geometry);
    assert(waveSize == 32 || waveSize == 64);
    unsigned maxWavesPerSubgroup = alignTo(maxThreadsPerSubgroup, waveSize) / waveSize;

    //
    // The LDS layout is something like this:
    //
    // +------------+----------------+------------------+---------------------+----------------+------------+
    // | ES-GS Ring | Primitive Data | Vertex Counts    | Vertex Index Map    | XFB statistics | GS-VS ring |
    // +------------+----------------+------------------+---------------------+----------------+------------+
    //                               | Primitive Counts | Primitive Index Map |
    //                               +------------------+---------------------+
    //

    // ES-GS ring
    if (ldsLayout) {
      ldsRegionSize = hwConfig.esGsLdsSize;

      printLdsRegionInfo("ES-GS Ring", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::EsGsRing] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }

    // Primitive data
    ldsRegionSize = maxThreadsPerSubgroup * MaxGsStreams; // 1 dword per primitive thread, 4 GS streams
    if (ldsLayout) {
      printLdsRegionInfo("Primitive Connectivity Data", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::PrimitiveData] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }
    ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;

    // Primitive counts
    if (pipelineState->enableSwXfb() || pipelineState->enablePrimStats()) {
      ldsRegionSize =
          (maxWavesPerSubgroup + 1) * MaxGsStreams; // 1 dword per wave and 1 dword per subgroup, 4 GS streams
      if (ldsLayout) {
        printLdsRegionInfo("Primitive Counts", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::PrimitiveCounts] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
    }

    // Primitive index map (compacted -> uncompacted)
    if (pipelineState->enableSwXfb()) {
      ldsRegionSize = maxThreadsPerSubgroup * MaxGsStreams; // 1 dword per primitive thread, 4 GS streams
      if (ldsLayout) {
        printLdsRegionInfo("Primitive Index Map (To Uncompacted)", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::PrimitiveIndexMap] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
    }

    // Vertex counts
    if (pipelineState->enableSwXfb() || pipelineState->enablePrimStats()) {
      if (ldsLayout) {
        // NOTE: If SW emulated stream-out or primitive statistics counting is enabled, this region is overlapped with
        // PrimitiveCounts.
        (*ldsLayout)[PrimShaderLdsRegion::VertexCounts] = (*ldsLayout)[PrimShaderLdsRegion::PrimitiveCounts];
        printLdsRegionInfo("Vertex Counts", (*ldsLayout)[PrimShaderLdsRegion::VertexCounts].first,
                           (*ldsLayout)[PrimShaderLdsRegion::VertexCounts].second);
      }
    } else {
      ldsRegionSize =
          (maxWavesPerSubgroup + 1) * MaxGsStreams; // 1 dword per wave and 1 dword per subgroup, 4 GS streams
      if (ldsLayout) {
        printLdsRegionInfo("Vertex Counts", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::VertexCounts] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
    }

    // Vertex index map (compacted -> uncompacted)
    if (pipelineState->getNggControl()->compactVertex) {
      if (pipelineState->enableSwXfb()) {
        if (ldsLayout) {
          // NOTE: If SW emulated stream-out is enabled, this region is overlapped with PrimitiveIndexMap.
          (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap] = (*ldsLayout)[PrimShaderLdsRegion::PrimitiveIndexMap];
          printLdsRegionInfo("Vertex Index Map (To Uncompacted)",
                             (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap].first,
                             (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap].second);
        }
      } else {
        ldsRegionSize = maxThreadsPerSubgroup * MaxGsStreams; // 1 dword per vertex thread, 4 GS streams
        if (ldsLayout) {
          printLdsRegionInfo("Vertex Index Map (To Uncompacted)", ldsOffset, ldsRegionSize);
          (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap] = std::make_pair(ldsOffset, ldsRegionSize);
          ldsOffset += ldsRegionSize;
        }
        ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
      }
    }

    // XFB statistics
    if (pipelineState->enableSwXfb()) {
      ldsRegionSize =
          MaxTransformFeedbackBuffers +
          MaxGsStreams; // 1 dword per XFB buffer : dword written, 1 dword per GS stream : primitives to write
      if (ldsLayout) {
        printLdsRegionInfo("XFB Statistics", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::XfbStats] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
    }

    // GS-VS ring
    if (ldsLayout) {
      const unsigned esGsRingLdsSize = (*ldsLayout)[PrimShaderLdsRegion::EsGsRing].second;
      ldsRegionSize = hwConfig.gsOnChipLdsSize - esGsRingLdsSize - ldsUsageInfo.gsExtraLdsSize;

      printLdsRegionInfo("GS-VS Ring", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::GsVsRing] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }

    if (ldsLayout) {
      printLdsRegionInfo("Total LDS", 0, ldsOffset);
      LLPC_OUTS("\n");
      LLPC_OUTS("Needs LDS = " << (ldsUsageInfo.needsLds ? "true" : "false") << "\n");
      LLPC_OUTS("ES Extra LDS Size (in dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.esExtraLdsSize) << "\n");
      LLPC_OUTS("GS Extra LDS Size (in dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.gsExtraLdsSize) << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Max Launched Threads = " << maxThreadsPerSubgroup << "\n");
      LLPC_OUTS("Max Launched Waves (Wave" << std::to_string(waveSize) << ") = " << maxWavesPerSubgroup << "\n");
      LLPC_OUTS("\n");
    }

    return ldsUsageInfo;
  }

  const bool hasTes = pipelineState->hasShaderStage(ShaderStage::TessEval);
  const bool distributePrimitiveId =
      !hasTes && pipelineState->getShaderResourceUsage(ShaderStage::Vertex)->builtInUsage.vs.primitiveId;

  const unsigned waveSize = pipelineState->getShaderWaveSize(hasTes ? ShaderStage::TessEval : ShaderStage::Vertex);
  assert(waveSize == 32 || waveSize == 64);
  unsigned maxThreadsPerSubgroup = NggMaxThreadsPerSubgroup;
  unsigned maxWavesPerSubgroup = NggMaxThreadsPerSubgroup / waveSize;

  //
  // Passthrough mode is enabled (API GS is not present)
  //
  if (pipelineState->getNggControl()->passthroughMode) {
    PrimShaderLdsUsageInfo ldsUsageInfo = {};
    ldsUsageInfo.needsLds = distributePrimitiveId || pipelineState->enableSwXfb();

    //
    // The LDS layout is something like this:
    //
    // +--------------------------+
    // | Distributed Primitive ID |
    // +--------------------------+----------------+
    // | XFB Outputs (4 x dword)  | XFB Statistics |
    // +--------------------------+----------------+
    //

    // Distributed primitive ID
    if (distributePrimitiveId) {
      if (ldsLayout) {
        ldsRegionSize = hwConfig.esVertsPerSubgroup; // 1 dword per vertex thread

        printLdsRegionInfo("Distributed Primitive ID", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::DistributedPrimitiveId] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
    }

    ldsOffset = 0; // DistributedPrimitiveId is always the first region and is overlapped with XfbOutput

    // XFB outputs
    if (pipelineState->enableSwXfb()) {
      if (ldsLayout) {
        ldsRegionSize =
            hwConfig.esVertsPerSubgroup * hwConfig.esGsRingItemSize; // XFB outputs are stored as a ES-GS ring item

        printLdsRegionInfo("XFB Outputs", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::XfbOutput] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
    }

    // XFB statistics
    if (pipelineState->enableSwXfb()) {
      ldsRegionSize =
          MaxTransformFeedbackBuffers + 1; // 1 dword per XFB buffer: dword written, 1 dword: primitives to write
      if (ldsLayout) {
        printLdsRegionInfo("XFB Statistics", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::XfbStats] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.esExtraLdsSize += ldsRegionSize;
    }

    if (ldsLayout) {
      printLdsRegionInfo("Total LDS", 0, ldsOffset);
      LLPC_OUTS("\n");
      LLPC_OUTS("Needs LDS = " << (ldsUsageInfo.needsLds ? "true" : "false") << "\n");
      LLPC_OUTS("ES Extra LDS Size (in dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.esExtraLdsSize) << "\n");
      LLPC_OUTS("GS Extra LDS Size (in dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.gsExtraLdsSize) << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("Max Launched Threads = " << maxThreadsPerSubgroup << "\n");
      LLPC_OUTS("Max Launched Waves (Wave" << std::to_string(waveSize) << ") = " << maxWavesPerSubgroup << "\n");
      LLPC_OUTS("\n");
    }

    return ldsUsageInfo;
  }

  //
  // Culling mode is enabled (API GS is not present)
  //
  PrimShaderLdsUsageInfo ldsUsageInfo = {};
  ldsUsageInfo.needsLds = true;

  //
  // The LDS layout is something like this:
  //
  // +--------------------------+
  // | Distributed Primitive ID |
  // +--------------------------+------------------+----------------+---------------+------------------+
  // | Vertex Position          | Vertex Cull Info | XFB Statistics | Vertex Counts | Vertex Index Map |
  // +--------------------------+------------------+----------------+----------------------------------+
  //

  // Distributed primitive ID
  if (distributePrimitiveId) {
    if (ldsLayout) {
      ldsRegionSize = hwConfig.esVertsPerSubgroup; // 1 dword per vertex thread

      printLdsRegionInfo("Distributed Primitive ID", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::DistributedPrimitiveId] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }
  }

  ldsOffset = 0; // DistributedPrimitiveId is always the first region and is overlapped with VertexPosition

  // Vertex position
  ldsRegionSize = 4 * maxThreadsPerSubgroup; // 4 dwords per vertex thread
  if (ldsLayout) {
    printLdsRegionInfo("Vertex Position", ldsOffset, ldsRegionSize);
    (*ldsLayout)[PrimShaderLdsRegion::VertexPosition] = std::make_pair(ldsOffset, ldsRegionSize);
    ldsOffset += ldsRegionSize;
  }
  ldsUsageInfo.esExtraLdsSize += ldsRegionSize;

  // Vertex cull info
  if (ldsLayout) {
    ldsRegionSize =
        hwConfig.esGsRingItemSize * hwConfig.esVertsPerSubgroup; // Vertex cull info is stored as a ES-GS ring item

    printLdsRegionInfo("Vertex Cull Info", ldsOffset, ldsRegionSize);
    (*ldsLayout)[PrimShaderLdsRegion::VertexCullInfo] = std::make_pair(ldsOffset, ldsRegionSize);
    ldsOffset += ldsRegionSize;
  }

  // XFB statistics
  if (pipelineState->enableSwXfb()) {
    ldsRegionSize =
        MaxTransformFeedbackBuffers + 1; // 1 dword per XFB buffer: dword written, 1 dword: primitives to write
    if (ldsLayout) {
      printLdsRegionInfo("XFB Statistics", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::XfbStats] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }
    ldsUsageInfo.esExtraLdsSize += ldsRegionSize;
  }

  // Vertex counts
  ldsRegionSize = maxWavesPerSubgroup + 1; // 1 dword per wave and 1 dword per subgroup
  if (ldsLayout) {
    printLdsRegionInfo("Vertex Counts", ldsOffset, ldsRegionSize);
    (*ldsLayout)[PrimShaderLdsRegion::VertexCounts] = std::make_pair(ldsOffset, ldsRegionSize);
    ldsOffset += ldsRegionSize;
  }
  ldsUsageInfo.esExtraLdsSize += ldsRegionSize;

  // Vertex index map
  if (pipelineState->getNggControl()->compactVertex) {
    ldsRegionSize = maxThreadsPerSubgroup; // 1 dword per wave and 1 dword per subgroup
    if (ldsLayout) {
      printLdsRegionInfo("Vertex Index Map (To Uncompacted)", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }
    ldsUsageInfo.esExtraLdsSize += ldsRegionSize;
  }

  if (ldsLayout) {
    printLdsRegionInfo("Total LDS", 0, ldsOffset);
    LLPC_OUTS("\n");
    LLPC_OUTS("Needs LDS = " << (ldsUsageInfo.needsLds ? "true" : "false") << "\n");
    LLPC_OUTS("ES Extra LDS Size (in dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.esExtraLdsSize) << "\n");
    LLPC_OUTS("GS Extra LDS Size (in dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.gsExtraLdsSize) << "\n");
    LLPC_OUTS("\n");
    LLPC_OUTS("Max Launched Threads = " << maxThreadsPerSubgroup << "\n");
    LLPC_OUTS("Max Launched Waves (Wave" << std::to_string(waveSize) << ") = " << maxWavesPerSubgroup << "\n");
    LLPC_OUTS("\n");
  }

  return ldsUsageInfo;
}

// =====================================================================================================================
// Generates the entry-point of primitive shader.
//
// @param esMain : ES main function (could be null)
// @param gsMain : GS main function (could be null)
// @param copyShader : Copy shader main function (could be null)
Function *NggPrimShader::generate(Function *esMain, Function *gsMain, Function *copyShader) {
  // ES and GS could not be null at the same time
  assert((!esMain && !gsMain) == false);

  // Assign names to ES, GS and copy shader main functions
  Module *module = nullptr;
  bool createDbgInfo = false;
  if (esMain) {
    module = esMain->getParent();

    esMain->setName(NggEsMain);
    esMain->setCallingConv(CallingConv::AMDGPU_ES);
    esMain->setLinkage(GlobalValue::InternalLinkage);
    esMain->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    esMain->addFnAttr(Attribute::AlwaysInline);
    m_esHandlers.main = esMain;
    createDbgInfo |= esMain->getSubprogram() != nullptr;
  }

  if (gsMain) {
    module = gsMain->getParent();

    gsMain->setName(NggGsMain);
    gsMain->setCallingConv(CallingConv::AMDGPU_GS);
    gsMain->setLinkage(GlobalValue::InternalLinkage);
    gsMain->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    gsMain->addFnAttr(Attribute::AlwaysInline);
    m_gsHandlers.main = gsMain;
    createDbgInfo |= gsMain->getSubprogram() != nullptr;

    assert(copyShader); // Copy shader must be present
    copyShader->setName(NggCopyShader);
    copyShader->setCallingConv(CallingConv::AMDGPU_VS);
    copyShader->setLinkage(GlobalValue::InternalLinkage);
    copyShader->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    copyShader->addFnAttr(Attribute::AlwaysInline);
    m_gsHandlers.copyShader = copyShader;
  }

  // Create primitive shader entry-point
  uint64_t inRegMask = 0;
  auto primShaderTy = getPrimShaderType(inRegMask);

  Function *primShader = createFunctionHelper(primShaderTy, GlobalValue::ExternalLinkage, module, createDbgInfo,
                                              lgcName::NggPrimShaderEntryPoint);
  primShader->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  primShader->addFnAttr("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  primShader->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  module->getFunctionList().push_front(primShader);

  SmallVector<Argument *, 32> args;
  for (auto &arg : primShader->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
    arg.addAttr(Attribute::NoUndef);
    args.push_back(&arg);
  }

  // Assign names to part of primitive shader arguments
  Value *userData = args[NumSpecialSgprInputs];
  userData->setName("userData");

  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());
  if (m_gfxIp.major <= 11) {
    // GS VGPRs
    vgprArgs[0]->setName("esGsOffsets01");
    vgprArgs[1]->setName("esGsOffsets23");
    vgprArgs[2]->setName("primitiveId");
    vgprArgs[3]->setName("invocationId");
    vgprArgs[4]->setName("esGsOffsets45");

    // ES VGPRs
    if (m_hasTes) {
      vgprArgs[5]->setName("tessCoordX");
      vgprArgs[6]->setName("tessCoordY");
      vgprArgs[7]->setName("relPatchId");
      vgprArgs[8]->setName("patchId");
    } else {
      vgprArgs[5]->setName("vertexId");
      // VGPR6 and VGPR7 are unused
      vgprArgs[8]->setName("instanceId");
    }
  } else {
#if LLPC_BUILD_GFX12
    // GS VGPRs
    vgprArgs[0]->setName("primData");
    vgprArgs[1]->setName("primitiveId");
    vgprArgs[2]->setName("primDataAdjacency");

    // ES VGPRs
    if (m_hasTes) {
      vgprArgs[3]->setName("tessCoordX");
      vgprArgs[4]->setName("tessCoordY");
      vgprArgs[5]->setName("relPatchId");
      vgprArgs[6]->setName("patchId");
    } else {
      vgprArgs[3]->setName("vertexId");
      vgprArgs[4]->setName("instanceId");
    }
#else
    llvm_unreachable("Not implemented!");
#endif
  }

  // Setup LDS layout
  m_lds = Patch::getLdsVariable(m_pipelineState, gsMain ? gsMain : esMain);
  calcVertexCullInfoSizeAndOffsets(m_pipelineState, esMain, m_vertCullInfoOffsets);
  layoutPrimShaderLds(m_pipelineState, &m_ldsLayout);

  // Build primitive shader body
  if (m_hasGs) {
    // API GS is present
    buildPrimShaderWithGs(primShader);
  } else if (m_nggControl->passthroughMode) {
    // NGG passthrough mode is enabled
    buildPassthroughPrimShader(primShader);
  } else {
    // NGG passthrough mode is disabled
    buildPrimShader(primShader);
  }

  return primShader;
}

// =====================================================================================================================
// Calculates and returns the dword size of vertex cull info. Meanwhile, builds the collection of LDS offsets within an
// item of vertex cull info region.
//
// @param pipelineState : Pipeline state
// @param esMain : ES main function
// @param [out] vertCullInfoOffsets : The collection of LDS offsets to build
// @returns : Dword size of vertex cull info
unsigned NggPrimShader::calcVertexCullInfoSizeAndOffsets(PipelineState *pipelineState, Function *esMain,
                                                         VertexCullInfoOffsets &vertCullInfoOffsets) {
  auto nggControl = pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  vertCullInfoOffsets = {};

  // Only for NGG culling mode without API GS
  const bool hasGs = pipelineState->hasShaderStage(ShaderStage::Geometry);
  if (hasGs || nggControl->passthroughMode)
    return 0;

  unsigned cullInfoSize = 0;
  unsigned cullInfoOffset = 0;
  unsigned itemSize = 0;

  if (pipelineState->enableSwXfb()) {
    itemSize = calcEsXfbOutputsSize(esMain);
    cullInfoSize += itemSize;
    vertCullInfoOffsets.xfbOutputs = cullInfoOffset;
    cullInfoOffset += itemSize;
  }

  if (nggControl->enableCullDistanceCulling) {
    itemSize = sizeof(VertexCullInfo::cullDistanceSignMask) / sizeof(unsigned);
    cullInfoSize += itemSize;
    vertCullInfoOffsets.cullDistanceSignMask = cullInfoOffset;
    cullInfoOffset += itemSize;
  }

  itemSize = sizeof(VertexCullInfo::drawFlag) / sizeof(unsigned);
  cullInfoSize += itemSize;
  vertCullInfoOffsets.drawFlag = cullInfoOffset;
  cullInfoOffset += itemSize;

  if (nggControl->compactVertex) {
    itemSize = sizeof(VertexCullInfo::compactedVertexIndex) / sizeof(unsigned);
    cullInfoSize += itemSize;
    vertCullInfoOffsets.compactedVertexIndex = cullInfoOffset;
    cullInfoOffset += itemSize;

    const bool hasTes = pipelineState->hasShaderStage(ShaderStage::TessEval);
    if (hasTes) {
      auto builtInUsage = pipelineState->getShaderResourceUsage(ShaderStage::TessEval)->builtInUsage.tes;
      if (builtInUsage.tessCoord) {
        itemSize = sizeof(VertexCullInfo::tes.tessCoordX) / sizeof(unsigned);
        cullInfoSize += itemSize;
        vertCullInfoOffsets.tessCoordX = cullInfoOffset;
        cullInfoOffset += itemSize;

        itemSize = sizeof(VertexCullInfo::tes.tessCoordY) / sizeof(unsigned);
        cullInfoSize += itemSize;
        vertCullInfoOffsets.tessCoordY = cullInfoOffset;
        cullInfoOffset += itemSize;
      }

      itemSize = sizeof(VertexCullInfo::tes.relPatchId) / sizeof(unsigned);
      cullInfoSize += itemSize;
      vertCullInfoOffsets.relPatchId = cullInfoOffset;
      cullInfoOffset += itemSize;

      if (builtInUsage.primitiveId) {
        itemSize = sizeof(VertexCullInfo::tes.patchId) / sizeof(unsigned);
        cullInfoSize += itemSize;
        vertCullInfoOffsets.patchId = cullInfoOffset;
        cullInfoOffset += itemSize;
      }
    } else {
      auto builtInUsage = pipelineState->getShaderResourceUsage(ShaderStage::Vertex)->builtInUsage.vs;
      if (builtInUsage.vertexIndex) {
        itemSize = sizeof(VertexCullInfo::vs.vertexId) / sizeof(unsigned);
        cullInfoSize += itemSize;
        vertCullInfoOffsets.vertexId = cullInfoOffset;
        cullInfoOffset += itemSize;
      }

      if (builtInUsage.instanceIndex) {
        itemSize = sizeof(VertexCullInfo::vs.instanceId) / sizeof(unsigned);
        cullInfoSize += itemSize;
        vertCullInfoOffsets.instanceId = cullInfoOffset;
        cullInfoOffset += itemSize;
      }

      if (builtInUsage.primitiveId) {
        itemSize = sizeof(VertexCullInfo::vs.primitiveId) / sizeof(unsigned);
        cullInfoSize += itemSize;
        vertCullInfoOffsets.primitiveId = cullInfoOffset;
        cullInfoOffset += itemSize;
      }
    }
  }

  return cullInfoSize;
}

// =====================================================================================================================
// Calculate and return the dword size of total XFB outputs to write for the ES stage.
//
// NOTE: For non 64-bit output, the value is its element count (8-bit/16-bit scalars are padded to 32-bit); for 64-bit
// output, the value is doubled since each 64-bit scalar is split to two dwords to write. This info is used by ES (VS
// or TES in non-GS pipeline) to write the outputs to NGG LDS space on GFX11+ to do SW emulated stream-out.
//
// @param esMain : ES main function
// @returns : Dword size of total XFB outputs to write
unsigned NggPrimShader::calcEsXfbOutputsSize(Function *esMain) {
  unsigned xfbOutputsSize = 0;

  struct Payload {
    unsigned &xfbOutputsSize;
  };
  Payload payload = {xfbOutputsSize};

  static const auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                                  .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                  .add<WriteXfbOutputOp>([](Payload &payload, WriteXfbOutputOp &writeXfbOutputOp) {
                                    Type *xfbOutputTy = writeXfbOutputOp.getOutputValue()->getType();
                                    unsigned xfbOutputSize = xfbOutputTy->isVectorTy()
                                                                 ? cast<FixedVectorType>(xfbOutputTy)->getNumElements()
                                                                 : 1;
                                    if (xfbOutputTy->getScalarSizeInBits() == 64)
                                      xfbOutputSize *= 2; // Double it
                                    payload.xfbOutputsSize += xfbOutputSize;
                                  })
                                  .build();
  visitor.visit(payload, *esMain);

  return xfbOutputsSize;
}

// =====================================================================================================================
// Get primitive shader entry-point type.
//
// @param [out] inRegMask : "Inreg" bit mask for the arguments
FunctionType *NggPrimShader::getPrimShaderType(uint64_t &inRegMask) {
  SmallVector<Type *, 32> argTys;

  // First 8 system values (SGPRs)
  for (unsigned i = 0; i < NumSpecialSgprInputs; ++i) {
    argTys.push_back(m_builder.getInt32Ty());
    inRegMask |= (1ull << i);
  }

  // User data (SGPRs)
  unsigned userDataCount = 0;

  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessEval);
  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex);

  if (m_hasGs) {
    // GS is present in primitive shader (ES-GS merged shader)
    userDataCount = gsIntfData->userDataCount;

    if (m_hasTes) {
      userDataCount = std::max(tesIntfData->userDataCount, userDataCount);

      if (gsIntfData->spillTable.sizeInDwords > 0 && tesIntfData->spillTable.sizeInDwords == 0) {
        tesIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
        assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
      }
    } else {
      userDataCount = std::max(vsIntfData->userDataCount, userDataCount);

      if (gsIntfData->spillTable.sizeInDwords > 0 && vsIntfData->spillTable.sizeInDwords == 0) {
        vsIntfData->userDataUsage.spillTable = userDataCount;
        ++userDataCount;
      }
    }
  } else {
    // No GS in primitive shader (ES only)
    userDataCount = m_hasTes ? tesIntfData->userDataCount : vsIntfData->userDataCount;
  }

  assert(userDataCount > 0);
  argTys.push_back(FixedVectorType::get(m_builder.getInt32Ty(), userDataCount));
  inRegMask |= (1ull << NumSpecialSgprInputs);

  if (m_gfxIp.major <= 11) {
    // GS VGPRs
    argTys.push_back(m_builder.getInt32Ty()); // ES to GS offsets (vertex 0 and 1)
    argTys.push_back(m_builder.getInt32Ty()); // ES to GS offsets (vertex 2 and 3)
    argTys.push_back(m_builder.getInt32Ty()); // Primitive ID (primitive based)
    argTys.push_back(m_builder.getInt32Ty()); // Invocation ID
    argTys.push_back(m_builder.getInt32Ty()); // ES to GS offsets (vertex 4 and 5)

    // ES VGPRs
    if (m_hasTes) {
      argTys.push_back(m_builder.getFloatTy()); // X of TessCoord (U)
      argTys.push_back(m_builder.getFloatTy()); // Y of TessCoord (V)
      argTys.push_back(m_builder.getInt32Ty()); // Relative patch ID
      argTys.push_back(m_builder.getInt32Ty()); // Patch ID
    } else {
      argTys.push_back(m_builder.getInt32Ty()); // Vertex ID
      argTys.push_back(m_builder.getInt32Ty()); // Unused
      argTys.push_back(m_builder.getInt32Ty()); // Unused
      argTys.push_back(m_builder.getInt32Ty()); // Instance ID
    }
  } else {
#if LLPC_BUILD_GFX12
    // GS VGPRs
    argTys.push_back(m_builder.getInt32Ty()); // Primitive connectivity data
    argTys.push_back(m_builder.getInt32Ty()); // Primitive ID (primitive based)
    argTys.push_back(m_builder.getInt32Ty()); // Primitive connectivity data (adjacency)

    // ES VGPRs
    if (m_hasTes) {
      argTys.push_back(m_builder.getFloatTy()); // X of TessCoord (U)
      argTys.push_back(m_builder.getFloatTy()); // Y of TessCoord (V)
      argTys.push_back(m_builder.getInt32Ty()); // Relative patch ID
      argTys.push_back(m_builder.getInt32Ty()); // Patch ID
    } else {
      argTys.push_back(m_builder.getInt32Ty()); // Vertex ID
      argTys.push_back(m_builder.getInt32Ty()); // Instance ID
    }
#else
    llvm_unreachable("Not implemented!");
#endif
  }

  return FunctionType::get(m_builder.getVoidTy(), argTys, false);
}

// =====================================================================================================================
// Builds layout lookup table of primitive shader constant buffer, setting up a collection of buffer offsets
// according to the definition of this constant buffer in ABI.
void NggPrimShader::buildPrimShaderCbLayoutLookupTable() {
  m_cbLayoutTable = {}; // Initialized to all-zeros

  const unsigned pipelineStateOffset = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
  m_cbLayoutTable.gsAddressLo = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, gsAddressLo);
  m_cbLayoutTable.gsAddressHi = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, gsAddressHi);
  m_cbLayoutTable.paClVteCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClVteCntl);
  m_cbLayoutTable.paSuVtxCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paSuVtxCntl);
  m_cbLayoutTable.paClClipCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
  m_cbLayoutTable.paSuScModeCntl = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paSuScModeCntl);
  m_cbLayoutTable.paClGbHorzClipAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzClipAdj);
  m_cbLayoutTable.paClGbVertClipAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertClipAdj);
  m_cbLayoutTable.paClGbHorzDiscAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
  m_cbLayoutTable.paClGbVertDiscAdj = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
  m_cbLayoutTable.vgtPrimitiveType = pipelineStateOffset + offsetof(Util::Abi::PrimShaderPsoCb, vgtPrimitiveType);

  const unsigned renderStateOffset = offsetof(Util::Abi::PrimShaderCbLayout, renderStateCb);
  m_cbLayoutTable.primitiveRestartEnable =
      renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, primitiveRestartEnable);
  m_cbLayoutTable.primitiveRestartIndex =
      renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, primitiveRestartIndex);
  m_cbLayoutTable.matchAllBits = renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, matchAllBits);
  m_cbLayoutTable.enableConservativeRasterization =
      renderStateOffset + offsetof(Util::Abi::PrimShaderRenderCb, enableConservativeRasterization);

  const unsigned vportStateOffset = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
  const unsigned vportControlSize = sizeof(Util::Abi::PrimShaderVportCb) / Util::Abi::MaxViewports;
  for (unsigned i = 0; i < Util::Abi::MaxViewports; ++i) {
    m_cbLayoutTable.vportControls[i].paClVportXscale =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    m_cbLayoutTable.vportControls[i].paClVportXoffset =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXoffset);
    m_cbLayoutTable.vportControls[i].paClVportYscale =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    m_cbLayoutTable.vportControls[i].paClVportYoffset =
        vportStateOffset + vportControlSize * i +
        offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYoffset);
  }
}

// =====================================================================================================================
// Calculate the dword offset of each item in the stream-out control buffer.
void NggPrimShader::calcStreamOutControlCbOffsets() {
  assert(m_pipelineState->enableSwXfb() || m_pipelineState->enablePrimStats());

  m_streamOutControlCbOffsets = {};

  if (m_pipelineState->enableSwXfb()) {
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      m_streamOutControlCbOffsets.bufOffsets[i] = (offsetof(Util::Abi::StreamOutControlCb, bufOffsets[0]) +
                                                   sizeof(Util::Abi::StreamOutControlCb::bufOffsets[0]) * i) /
                                                  4;
    }
  }

#if LLPC_BUILD_GFX12
  // Following calculations are only available on GFX12+ (caused by GDS removal)
  if (m_gfxIp.major >= 12) {
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      m_streamOutControlCbOffsets.primsNeeded[i] = (offsetof(Util::Abi::StreamOutControlCb, primsNeeded[0]) +
                                                    sizeof(Util::Abi::StreamOutControlCb::primsNeeded[0]) * i) /
                                                   4;
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      m_streamOutControlCbOffsets.primsWritten[i] = (offsetof(Util::Abi::StreamOutControlCb, primsWritten[0]) +
                                                     sizeof(Util::Abi::StreamOutControlCb::primsWritten[0]) * i) /
                                                    4;
    }

    if (m_pipelineState->enableSwXfb()) {
      const unsigned orderedIdPairOffset = offsetof(Util::Abi::StreamOutControlCb, orderedIdPair[0]);
      const unsigned orderedIdPairSize = sizeof(Util::Abi::OrderedIdPair);
      for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
        m_streamOutControlCbOffsets.orderedIdPair[i].orderedWaveId =
            (orderedIdPairOffset + orderedIdPairSize * i + offsetof(Util::Abi::OrderedIdPair, orderedWaveId)) / 4;
        m_streamOutControlCbOffsets.orderedIdPair[i].dwordsWritten =
            (orderedIdPairOffset + orderedIdPairSize * i + offsetof(Util::Abi::OrderedIdPair, dwordsWritten)) / 4;
      }
    }
  }
#endif
}

// =====================================================================================================================
// Build the body of passthrough primitive shader.
//
// @param primShader : Entry-point of primitive shader to build
void NggPrimShader::buildPassthroughPrimShader(Function *primShader) {
  assert(m_nggControl->passthroughMode); // Make sure NGG passthrough mode is enabled
  assert(!m_hasGs);                      // Make sure API GS is not present

  SmallVector<Argument *, 32> args;
  for (auto &arg : primShader->args())
    args.push_back(&arg);

  // System SGPRs
  Value *mergedGroupInfo = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedGroupInfo)];
  mergedGroupInfo->setName("mergedGroupInfo");

  Value *mergedWaveInfo = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo)];
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *attribRingBase = nullptr;
  if (m_gfxIp.major >= 11) {
    attribRingBase = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase)];
    attribRingBase->setName("attribRingBase");
  }

  // System user data
  Value *userData = args[NumSpecialSgprInputs];

  // System VGPRs
  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  Value *primData = vgprArgs[0];
  Value *primitiveId = nullptr;
  if (m_gfxIp.major <= 11)
    primitiveId = vgprArgs[2];
  else
#if LLPC_BUILD_GFX12
    primitiveId = vgprArgs[1];
#else
    llvm_unreachable("Not implemented!");
#endif

  //
  // For pass-through mode, the processing is something like this:
  //
  // NGG_PASSTHROUGH() {
  //   Initialize thread/wave info
  //
  //   if (Distribute primitive ID) {
  //     if (threadIdInSubgroup < primCountInSubgroup)
  //       Distribute primitive ID to provoking vertex (vertex0 or vertex2)
  //     Barrier
  //
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Get primitive ID
  //     Barrier
  //   }
  //
  //   if (waveId == 0)
  //     Send GS_ALLOC_REQ message
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Collect exports
  //
  //   if (Enable SW XFB)
  //     Process SW XFB
  //   else if (Enable primitive statistics counting)
  //     Collect primitive statistics
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Export primitive
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Export vertex
  // }
  //

  // Define basic blocks
  auto entryBlock = createBlock(primShader, ".entry");

  auto sendGsAllocReqBlock = createBlock(primShader, ".sendGsAllocReq");
  auto endSendGsAllocReqBlock = createBlock(primShader, ".endSendGsAllocReq");

  auto collectExportBlock = createBlock(primShader, ".collectExport");
  auto endCollectExportBlock = createBlock(primShader, ".endCollectExport");

  auto exportPrimitiveBlock = createBlock(primShader, ".exportPrimitive");
  auto endExportPrimitiveBlock = createBlock(primShader, ".endExportPrimitive");

  auto exportVertexBlock = createBlock(primShader, ".exportVertex");
  auto endExportVertexBlock = createBlock(primShader, ".endExportVertex");

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      if (!m_pipelineState->exportAttributeByExportInstruction())
        prepareAttribRingAccess(userData);

      if (m_pipelineState->enableSwXfb() || m_pipelineState->enablePrimStats())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive connectivity data
    m_nggInputs.primData = primData;

    // Primitive connectivity data have such layout:
    //
#if LLPC_BUILD_GFX12
    // Pre-GFX12:
#endif
    //   +----------------+---------------+---------------+---------------+
    //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
    //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
    //   +----------------+---------------+---------------+---------------+
#if LLPC_BUILD_GFX12
    //
    // GFX12 (from GE):
    //   +----------------+------------+---------------+------------+---------------+------------+---------------+
    //   | GS Instance ID | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
    //   | [31:27]        | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
    //   +----------------+------------+---------------+------------+---------------+------------+---------------+
    //
    // GFX12 (to PA):
    //   +----------------+------------+---------------+------------+---------------+------------+---------------+
    //   | Null Primitive | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
    //   | [31]           | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
    //   +----------------+------------+---------------+------------+---------------+------------+---------------+
#endif

    // Record relative vertex indices
    if (m_gfxIp.major <= 11) {
      m_nggInputs.vertexIndex0 = createUBfe(primData, 0, 9);
      m_nggInputs.vertexIndex1 = createUBfe(primData, 10, 9);
      m_nggInputs.vertexIndex2 = createUBfe(primData, 20, 9);
    } else {
#if LLPC_BUILD_GFX12
      m_nggInputs.vertexIndex0 = createUBfe(primData, 0, 8);
      m_nggInputs.vertexIndex1 = createUBfe(primData, 9, 8);
      m_nggInputs.vertexIndex2 = createUBfe(primData, 18, 8);
#else
      llvm_unreachable("Not implemented!");
#endif
    }

    // Distribute primitive ID if needed
    distributePrimitiveId(primitiveId);

    // Apply workaround to fix HW VMID reset bug (add an additional s_barrier before sending GS_ALLOC_REQ message)
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggPassthroughMessageHazard) {
      // If we distribute primitive ID, there must be at least a s_barrier inserted. Thus, following codes are not
      // needed.
      if (!m_distributedPrimitiveId)
        createBarrier();
    }

    auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstWaveInSubgroup, sendGsAllocReqBlock, endSendGsAllocReqBlock);
  }

  // Construct ".sendGsAllocReq" block
  {
    m_builder.SetInsertPoint(sendGsAllocReqBlock);

    // NOTE: For GFX11+, we use NO_MSG mode for NGG pass-through mode if SW-emulated stream-out is not requested. The
    // message GS_ALLOC_REQ is no longer necessary.
    const bool passthroughNoMsg = m_gfxIp.major >= 11 && !m_pipelineState->enableSwXfb();
    if (!passthroughNoMsg)
      sendGsAllocReqMessage();

    m_builder.CreateBr(endSendGsAllocReqBlock);
  }

  // Construct ".endSendGsAllocReq" block
  {
    m_builder.SetInsertPoint(endSendGsAllocReqBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, collectExportBlock, endCollectExportBlock);
  }

  // Construct ".collectExport" block
  SmallVector<VertexExport, 4> positionExports;
  SmallVector<VertexExport, 32> attributeExports;
  SmallVector<XfbExport, 32> xfbExports;
  {
    m_builder.SetInsertPoint(collectExportBlock);

    collectExports(args, m_esHandlers.main, false, &positionExports, &attributeExports, &xfbExports);

    m_builder.CreateBr(endCollectExportBlock);
  }

  // Construct ".endCollectExport" block
  {
    m_builder.SetInsertPoint(endCollectExportBlock);

    createPhiForExports(&positionExports, &attributeExports, &xfbExports);

    if (m_pipelineState->enableSwXfb())
      processSwXfb(args, xfbExports);
    else if (m_pipelineState->enablePrimStats())
      collectPrimitiveStats();

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, exportPrimitiveBlock, endExportPrimitiveBlock);
  }

  // Construct ".exportPrimitive" block
  {
    m_builder.SetInsertPoint(exportPrimitiveBlock);

    exportPassthroughPrimitive();
    m_builder.CreateBr(endExportPrimitiveBlock);
  }

  // Construct ".endExportPrimitive" block
  {
    m_builder.SetInsertPoint(endExportPrimitiveBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, exportVertexBlock, endExportVertexBlock);
  }

  // Construct ".exportVertex" block
  {
    m_builder.SetInsertPoint(exportVertexBlock);

    // NOTE: If the workaround of attributes-through-memory preceding vertex position data is required, we have to
    // place vertex exports after all attribute exports (ATM operations).
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos) {
      exportAttributes(attributeExports);
      if (!attributeExports.empty())
        m_builder.CreateFence(AtomicOrdering::Release, m_builder.getContext().getOrInsertSyncScopeID("agent"));
      exportPositions(positionExports);
    } else {
      exportPositions(positionExports);
      exportAttributes(attributeExports);
    }

    m_builder.CreateBr(endExportVertexBlock);
  }

  // Construct ".endExportVertex" block
  {
    m_builder.SetInsertPoint(endExportVertexBlock);

    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Build the body of primitive shader when API GS is not present.
//
// @param primShader : Entry-point of primitive shader to build
void NggPrimShader::buildPrimShader(Function *primShader) {
  assert(!m_nggControl->passthroughMode); // Make sure NGG passthrough mode is not enabled
  assert(!m_hasGs);                       // Make sure API GS is not present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  assert(waveSize == 32 || waveSize == 64);

  SmallVector<Argument *, 32> args;
  for (auto &arg : primShader->args())
    args.push_back(&arg);

  // System SGPRs
  Value *mergedGroupInfo = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedGroupInfo)];
  mergedGroupInfo->setName("mergedGroupInfo");

  Value *mergedWaveInfo = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo)];
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *attribRingBase = nullptr;
  if (m_gfxIp.major >= 11) {
    attribRingBase = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase)];
    attribRingBase->setName("attribRingBase");
  }

  // GS shader address is reused as primitive shader table address for NGG culling
  Value *primShaderTableAddrLow = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrLow)];
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrHigh)];
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  // System user data
  Value *userData = args[NumSpecialSgprInputs];

  // System VGPRs
  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  Value *primitiveId = nullptr;

  Value *tessCoordX = nullptr;
  Value *tessCoordY = nullptr;
  Value *relPatchId = nullptr;
  Value *patchId = nullptr;

  Value *vertexId = nullptr;
  Value *instanceId = nullptr;

  if (m_gfxIp.major <= 11) {
    primitiveId = vgprArgs[2];

    if (m_hasTes) {
      tessCoordX = vgprArgs[5];
      tessCoordY = vgprArgs[6];
      relPatchId = vgprArgs[7];
      patchId = vgprArgs[8];
    } else {
      vertexId = vgprArgs[5];
      instanceId = vgprArgs[8];
    }
  } else {
#if LLPC_BUILD_GFX12
    primitiveId = vgprArgs[1];

    if (m_hasTes) {
      tessCoordX = vgprArgs[3];
      tessCoordY = vgprArgs[4];
      relPatchId = vgprArgs[5];
      patchId = vgprArgs[6];
    } else {
      vertexId = vgprArgs[3];
      instanceId = vgprArgs[4];
    }
#else
    llvm_unreachable("Not implemented!");
#endif
  }

  //
  // The processing is something like this:
  //
  // NGG() {
  //   Initialize thread/wave info
  //
  //   if (Distribute primitive ID) {
  //     if (threadIdInSubgroup < primCountInSubgroup)
  //       Distribute primitive ID to provoking vertex (vertex0 or vertex2)
  //     Barrier
  //
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Get primitive ID
  //     Barrier
  //   }
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Collect XFB exports
  //
  //   if (Enable SW XFB)
  //     Process SW XFB
  //   else if (Enable primitive statistics counting)
  //     Collect primitive statistics
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Collect cull data
  //
  //   if (Not runtime passthrough) {
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Initialize vertex draw flag
  //     if (threadIdInSubgroup < maxWaves + 1)
  //       Initialize per-wave and per-subgroup count of output vertices
  //
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Write cull data
  //     Barrier
  //
  //     if (threadIdInSubgroup < primCountInSubgroup) {
  //       Cull primitive (run culling algorithms)
  //       if (primitive not culled)
  //         Write draw flags of forming vertices
  //     }
  //     Barrier
  //
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Check draw flags of vertices and compute draw mask
  //
  //     if (threadIdInWave < maxWaves - waveId)
  //       Accumulate per-wave and per-subgroup count of output vertices
  //     Barrier
  //
  //     if (Need compact vertex && vertex drawn) {
  //       Compact vertex (compacted -> uncompacted)
  //       Write vertex compaction info
  //     }
  //     Update vertCountInSubgroup and primCountInSubgroup
  //   }
  //
  //   if (waveId == 0)
  //     Send GS_ALLOC_REQ message
  //   Barrier
  //
  //   if (fullyCulled) {
  //     Dummy export
  //     return (early exit)
  //   }
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Collect vertex exports
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Export primitive
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     if (Needn't compact vertex && empty wave)
  //       Dummy vertex export
  //     else if (drawFlag)
  //       Export vertex
  //   }
  // }
  //

  // Export count when the entire subgroup is fully culled
  const bool waNggCullingNoEmptySubgroups =
      m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups;
  const unsigned dummyExportCount = waNggCullingNoEmptySubgroups ? 1 : 0;

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize;

  // NOTE: Make sure vertex position data is 4-dword alignment because we will use 128-bit LDS read/write for it.
  assert(getLdsRegionStart(PrimShaderLdsRegion::VertexPosition) % 4U == 0);

  if (!m_nggControl->compactVertex)
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  // Define basic blocks
  auto entryBlock = createBlock(primShader, ".entry");

  auto collectXfbExportBlock = createBlock(primShader, ".collectXfbExport");
  auto endCollectXfbExportBlock = createBlock(primShader, ".endCollectXfbExport");

  auto collectCullDataBlock = createBlock(primShader, ".collectCullData");
  auto endCollectCullDataBlock = createBlock(primShader, ".endCollectCullData");

  auto checkInitVertexDrawFlagBlock = createBlock(primShader, ".checkInitVertexDrawFlag");
  auto initVertexDrawFlagBlock = createBlock(primShader, ".initVertexDrawFlag");
  auto endInitVertexDrawFlagBlock = createBlock(primShader, ".endInitVertexDrawFlag");

  auto initVertexCountsBlock = createBlock(primShader, ".initVertexCounts");
  auto endInitVertexCountsBlock = createBlock(primShader, ".endInitVertexCounts");

  auto writeCullDataBlock = createBlock(primShader, ".writeCullData");
  auto endWriteCullDataBlock = createBlock(primShader, ".endWriteCullData");

  auto cullPrimitiveBlock = createBlock(primShader, ".cullPrimitive");
  auto writeVertexDrawFlagBlock = createBlock(primShader, ".writeVertexDrawFlag");
  auto endCullPrimitiveBlock = createBlock(primShader, ".endCullPrimitive");

  auto checkVertexDrawFlagBlock = createBlock(primShader, ".checkVertexDrawFlag");
  auto endCheckVertexDrawFlagBlock = createBlock(primShader, ".endCheckVertexDrawFlag");

  auto accumVertexCountsBlock = createBlock(primShader, ".accumVertexCounts");
  auto endAccumVertexCountsBlock = createBlock(primShader, ".endAccumVertexCounts");

  auto compactVertexBlock = createBlock(primShader, ".compactVertex");
  auto endCompactVertexBlock = createBlock(primShader, ".endCompactVertex");

  auto checkSendGsAllocReqBlock = createBlock(primShader, ".checkSendGsAllocReq");
  auto sendGsAllocReqBlock = createBlock(primShader, ".sendGsAllocReq");
  auto endSendGsAllocReqBlock = createBlock(primShader, ".endSendGsAllocReq");

  auto earlyExitBlock = createBlock(primShader, ".earlyExit");
  auto checkCollectVertexExportBlock = createBlock(primShader, ".checkCollectVertexExport");

  auto collectVertexExportBlock = createBlock(primShader, ".collectVertexExport");
  auto endCollectVertexExportBlock = createBlock(primShader, ".endCollectVertexExport");

  auto exportPrimitiveBlock = createBlock(primShader, ".exportPrimitive");
  auto endExportPrimitiveBlock = createBlock(primShader, ".endExportPrimitive");

  auto checkEmptyWaveBlock = createBlock(primShader, ".checkEmptyWave");
  auto dummyVertexExportBlock = createBlock(primShader, ".dummyVertexExport");

  auto checkExportVertexBlock = createBlock(primShader, ".checkExportVertex");
  auto exportVertexBlock = createBlock(primShader, ".exportVertex");
  auto endExportVertexBlock = createBlock(primShader, ".endExportVertex");

  // Construct ".entry" block
  Value *vertexItemOffset = nullptr;
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      if (!m_pipelineState->exportAttributeByExportInstruction())
        prepareAttribRingAccess(userData);

      if (m_pipelineState->enableSwXfb() || m_pipelineState->enablePrimStats())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive shader table address info
    m_nggInputs.primShaderTableAddr = std::make_pair(primShaderTableAddrLow, primShaderTableAddrHigh);

#if LLPC_BUILD_GFX12
    if (m_gfxIp.major >= 12) {
      // NOTE: From GFX12+, GE will always send the primitive connectivity data to us (the highest 5 bits are GS
      // instance ID, which is not valid when API GS is absent). We can record this data and use it
      // when exporting primitive to PA without reconstructing it like what we have done on pre-GFX12.
      m_nggInputs.primData = createUBfe(vgprArgs[0], 0, 27);
    }
#endif

    // Record vertex indices
    if (m_gfxIp.major <= 11) {
      m_nggInputs.vertexIndex0 = createUBfe(vgprArgs[0], 0, 16);
      m_nggInputs.vertexIndex1 = createUBfe(vgprArgs[0], 16, 16);
      m_nggInputs.vertexIndex2 = createUBfe(vgprArgs[1], 0, 16);
    } else {
#if LLPC_BUILD_GFX12
      m_nggInputs.vertexIndex0 = createUBfe(vgprArgs[0], 0, 8);
      m_nggInputs.vertexIndex1 = createUBfe(vgprArgs[0], 9, 8);
      m_nggInputs.vertexIndex2 = createUBfe(vgprArgs[0], 18, 8);
#else
      llvm_unreachable("Not implemented!");
#endif
    }

    vertexItemOffset = m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(esGsRingItemSize));

    // Distribute primitive ID if needed
    distributePrimitiveId(primitiveId);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, collectXfbExportBlock, endCollectXfbExportBlock);
  }

  // Construct ".collectXfbExport" block
  SmallVector<XfbExport, 32> xfbExports;
  {
    m_builder.SetInsertPoint(collectXfbExportBlock);

    if (m_pipelineState->enableSwXfb())
      collectExports(args, m_esHandlers.main, true, nullptr, nullptr, &xfbExports);

    m_builder.CreateBr(endCollectXfbExportBlock);
  }

  // Construct ".endCollectXfbExport" block
  {
    m_builder.SetInsertPoint(endCollectXfbExportBlock);

    if (m_pipelineState->enableSwXfb()) {
      createPhiForExports(nullptr, nullptr, &xfbExports);
      processSwXfb(args, xfbExports);
    } else if (m_pipelineState->enablePrimStats()) {
      collectPrimitiveStats();
    }

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, collectCullDataBlock, endCollectCullDataBlock);
  }

  // Construct ".collectCullData" block
  SmallVector<VertexExport, 4> positionExports;
  {
    m_builder.SetInsertPoint(collectCullDataBlock);

    // Split ES to two parts: cull data exports and other remaining exports
    splitEs();

    // Collect cull data exports
    collectExports(args, m_esHandlers.part.first, false, &positionExports, nullptr, nullptr);

    m_builder.CreateBr(endCollectCullDataBlock);
  }

  // Construct ".endCollectCullData" block
  Value *position0 = nullptr;
  SmallVector<Value *> cullDistance;
  {
    m_builder.SetInsertPoint(endCollectCullDataBlock);

    createPhiForExports(&positionExports, nullptr, nullptr);

    unsigned clipCullExportSlot = 1;
    unsigned clipDistanceCount = 0;
    unsigned cullDistanceCount = 0;
    if (m_nggControl->enableCullDistanceCulling) {
      const auto &resUsage =
          m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex);

      if (m_hasTes) {
        const auto &builtInUsage = resUsage->builtInUsage.tes;

        const bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
        clipCullExportSlot = miscExport ? 2 : 1;
        clipDistanceCount = builtInUsage.clipDistance;
        cullDistanceCount = builtInUsage.cullDistance;
      } else {
        const auto &builtInUsage = resUsage->builtInUsage.vs;

        const bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex ||
                                builtInUsage.primitiveShadingRate;
        clipCullExportSlot = miscExport ? 2 : 1;
        clipDistanceCount = builtInUsage.clipDistance;
        cullDistanceCount = builtInUsage.cullDistance;
      }

      assert(cullDistanceCount > 0); // Cull distance must exist if the culling is enabled
    }

    SmallVector<Value *, 8> clipCullDistance;
    for (auto &positionExport : positionExports) {
      if (positionExport.exportSlot == 0) {
        position0 = positionExport.exportValue;
        position0->setName("position0");
      } else {
        if (m_nggControl->enableCullDistanceCulling) {
          if (positionExport.exportSlot == clipCullExportSlot) {
            for (unsigned i = 0; i < 4; i++)
              clipCullDistance[i] = m_builder.CreateExtractElement(positionExport.exportValue, i);
          } else if (positionExport.exportSlot == clipCullExportSlot + 1 && clipDistanceCount + cullDistanceCount > 4) {
            for (unsigned i = 0; i < 4; i++)
              clipCullDistance[4 + i] = m_builder.CreateExtractElement(positionExport.exportValue, i);
          }
        }
      }
    }

    if (m_nggControl->enableCullDistanceCulling) {
      for (unsigned i = 0; i < cullDistanceCount; ++i) {
        cullDistance.push_back(clipCullDistance[clipDistanceCount + i]);
        cullDistance[i]->setName("cullDistance" + std::to_string(i));
      }
    }

    // NOTE: If the Z channel of vertex position data is constant, we can go into runtime passthrough mode. Otherwise,
    // we will further check if this is a small subgroup and enable runtime passthrough mode accordingly.
    auto runtimePassthrough = m_constPositionZ ? m_builder.getTrue()
                                               : m_builder.CreateICmpULT(m_nggInputs.vertCountInSubgroup,
                                                                         m_builder.getInt32(NggSmallSubgroupThreshold));
    m_builder.CreateCondBr(runtimePassthrough, checkSendGsAllocReqBlock, checkInitVertexDrawFlagBlock);
  }

  // Construct ".checkInitVertexDrawFlag" block
  {
    m_builder.SetInsertPoint(checkInitVertexDrawFlagBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, initVertexDrawFlagBlock, endInitVertexDrawFlagBlock);
  }

  // Construct ".initVertexDrawFlag" block
  {
    m_builder.SetInsertPoint(initVertexDrawFlagBlock);

    writeVertexCullInfoToLds(m_builder.getInt32(0), vertexItemOffset, m_vertCullInfoOffsets.drawFlag);

    m_builder.CreateBr(endInitVertexDrawFlagBlock);
  }

  // Construct ".endInitVertexDrawFlag" block
  {
    m_builder.SetInsertPoint(endInitVertexDrawFlagBlock);

    auto validWave =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(m_maxWavesPerSubgroup + 1));
    m_builder.CreateCondBr(validWave, initVertexCountsBlock, endInitVertexCountsBlock);
  }

  // Construct ".initVertexCounts" block
  {
    m_builder.SetInsertPoint(initVertexCountsBlock);

    writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup, PrimShaderLdsRegion::VertexCounts);

    m_builder.CreateBr(endInitVertexCountsBlock);
  }

  // Construct ".endInitVertexCounts" block
  {
    m_builder.SetInsertPoint(endInitVertexCountsBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
    m_builder.CreateCondBr(validVertex, writeCullDataBlock, endWriteCullDataBlock);
  }

  // Construct ".writeCullData" block
  {
    m_builder.SetInsertPoint(writeCullDataBlock);

    // Write vertex position data
    writePerThreadDataToLds(position0, m_nggInputs.threadIdInSubgroup, PrimShaderLdsRegion::VertexPosition, 0, true);

    // Write cull distance sign mask
    if (m_nggControl->enableCullDistanceCulling) {
      // Calculate the sign mask for cull distance
      Value *signMask = m_builder.getInt32(0);
      for (unsigned i = 0; i < cullDistance.size(); ++i) {
        Value *signBit = createUBfe(m_builder.CreateBitCast(cullDistance[i], m_builder.getInt32Ty()), 31, 1);
        signBit = m_builder.CreateShl(signBit, i);

        signMask = m_builder.CreateOr(signMask, signBit);
      }

      writeVertexCullInfoToLds(signMask, vertexItemOffset, m_vertCullInfoOffsets.cullDistanceSignMask);
    }

    m_builder.CreateBr(endWriteCullDataBlock);
  }

  // Construct ".endWriteCullData" block
  {
    m_builder.SetInsertPoint(endWriteCullDataBlock);

    createFenceAndBarrier();

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, cullPrimitiveBlock, endCullPrimitiveBlock);
  }

  // Construct ".cullPrimitive" block
  Value *primitiveCulled = nullptr;
  {
    m_builder.SetInsertPoint(cullPrimitiveBlock);

    primitiveCulled = cullPrimitive(m_nggInputs.vertexIndex0, m_nggInputs.vertexIndex1, m_nggInputs.vertexIndex2);
    m_builder.CreateCondBr(primitiveCulled, endCullPrimitiveBlock, writeVertexDrawFlagBlock);
  }

  // Construct ".writeVertexDrawFlag" block
  {
    m_builder.SetInsertPoint(writeVertexDrawFlagBlock);

    auto vertexItemOffset0 = m_builder.CreateMul(m_nggInputs.vertexIndex0, m_builder.getInt32(esGsRingItemSize));
    auto vertexItemOffset1 = m_builder.CreateMul(m_nggInputs.vertexIndex1, m_builder.getInt32(esGsRingItemSize));
    auto vertexItemOffset2 = m_builder.CreateMul(m_nggInputs.vertexIndex2, m_builder.getInt32(esGsRingItemSize));

    writeVertexCullInfoToLds(m_builder.getInt32(1), vertexItemOffset0, m_vertCullInfoOffsets.drawFlag);
    writeVertexCullInfoToLds(m_builder.getInt32(1), vertexItemOffset1, m_vertCullInfoOffsets.drawFlag);
    writeVertexCullInfoToLds(m_builder.getInt32(1), vertexItemOffset2, m_vertCullInfoOffsets.drawFlag);

    m_builder.CreateBr(endCullPrimitiveBlock);
  }

  // Construct ".endCullPrimitive" block
  {
    m_builder.SetInsertPoint(endCullPrimitiveBlock);

    primitiveCulled = createPhi({{m_builder.getTrue(), cullPrimitiveBlock},
                                 {m_builder.getFalse(), writeVertexDrawFlagBlock},
                                 {m_builder.getTrue(), endWriteCullDataBlock}});

    createFenceAndBarrier();

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, checkVertexDrawFlagBlock, endCheckVertexDrawFlagBlock);
  }

  // Construct ".checkVertexDrawFlag"
  Value *drawFlag = nullptr;
  {
    m_builder.SetInsertPoint(checkVertexDrawFlagBlock);

    drawFlag = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.drawFlag);
    drawFlag = m_builder.CreateICmpNE(drawFlag, m_builder.getInt32(0));

    m_builder.CreateBr(endCheckVertexDrawFlagBlock);
  }

  // Construct ".endCheckVertexDrawFlag"
  Value *drawMask = nullptr;
  Value *vertCountInWave = nullptr;
  {
    m_builder.SetInsertPoint(endCheckVertexDrawFlagBlock);

    drawFlag = createPhi({{drawFlag, checkVertexDrawFlagBlock},
                          {m_builder.getFalse(), endCullPrimitiveBlock}}); // Update vertex draw flag
    drawMask = ballot(drawFlag);

    vertCountInWave = m_builder.CreateIntrinsic(Intrinsic::ctpop, m_builder.getInt64Ty(), drawMask);
    vertCountInWave = m_builder.CreateTrunc(vertCountInWave, m_builder.getInt32Ty());

    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(m_maxWavesPerSubgroup), m_nggInputs.waveIdInSubgroup);
    auto validThread = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);
    m_builder.CreateCondBr(validThread, accumVertexCountsBlock, endAccumVertexCountsBlock);
  }

  // Construct ".accumVertexCounts" block
  {
    m_builder.SetInsertPoint(accumVertexCountsBlock);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));

    unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCounts);

    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart));
    atomicOp(AtomicRMWInst::Add, vertCountInWave, ldsOffset);

    m_builder.CreateBr(endAccumVertexCountsBlock);
  }

  // Construct ".endAccumVertexCounts" block
  Value *vertCountInPrevWaves = nullptr;
  Value *vertCountInSubgroup = nullptr;
  Value *hasCulledVertices = nullptr;
  {
    m_builder.SetInsertPoint(endAccumVertexCountsBlock);

    createFenceAndBarrier();

    auto vertCountInWaves =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, PrimShaderLdsRegion::VertexCounts);

    // The last dword following dwords for all waves (each wave has one dword) stores vertex count of the
    // entire subgroup
    vertCountInSubgroup = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                    {vertCountInWaves, m_builder.getInt32(m_maxWavesPerSubgroup)});

    if (m_nggControl->compactVertex) {
      // Get vertex count for all waves prior to this wave
      vertCountInPrevWaves = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                       {vertCountInWaves, m_nggInputs.waveIdInSubgroup});

      hasCulledVertices = m_builder.CreateICmpULT(vertCountInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(m_builder.CreateAnd(drawFlag, hasCulledVertices), compactVertexBlock,
                             endCompactVertexBlock);
    } else {
      m_builder.CreateBr(endCompactVertexBlock);
    }
  }

  if (m_nggControl->compactVertex) {
    // Construct ".compactVertex" block
    {
      m_builder.SetInsertPoint(compactVertexBlock);

      auto drawMaskVec = m_builder.CreateBitCast(drawMask, FixedVectorType::get(m_builder.getInt32Ty(), 2));

      auto drawMaskLow = m_builder.CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
      Value *compactedVertexIndex =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder.getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder.CreateExtractElement(drawMaskVec, 1);
        compactedVertexIndex =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactedVertexIndex});
      }

      // Setup the map: compacted -> uncompacted
      compactedVertexIndex = m_builder.CreateAdd(vertCountInPrevWaves, compactedVertexIndex);
      writePerThreadDataToLds(m_nggInputs.threadIdInSubgroup, compactedVertexIndex,
                              PrimShaderLdsRegion::VertexIndexMap);

      // Write compacted vertex index
      writeVertexCullInfoToLds(compactedVertexIndex, vertexItemOffset, m_vertCullInfoOffsets.compactedVertexIndex);

      const auto resUsage =
          m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex);
      if (m_hasTes) {
        // Write X/Y of tessCoord (U/V)
        if (resUsage->builtInUsage.tes.tessCoord) {
          writeVertexCullInfoToLds(tessCoordX, vertexItemOffset, m_vertCullInfoOffsets.tessCoordX);
          writeVertexCullInfoToLds(tessCoordY, vertexItemOffset, m_vertCullInfoOffsets.tessCoordY);
        }

        // Write relative patch ID
        writeVertexCullInfoToLds(relPatchId, vertexItemOffset, m_vertCullInfoOffsets.relPatchId);

        // Write patch ID
        if (resUsage->builtInUsage.tes.primitiveId)
          writeVertexCullInfoToLds(patchId, vertexItemOffset, m_vertCullInfoOffsets.patchId);
      } else {
        // Write vertex ID
        if (resUsage->builtInUsage.vs.vertexIndex)
          writeVertexCullInfoToLds(vertexId, vertexItemOffset, m_vertCullInfoOffsets.vertexId);

        // Write instance ID
        if (resUsage->builtInUsage.vs.instanceIndex)
          writeVertexCullInfoToLds(instanceId, vertexItemOffset, m_vertCullInfoOffsets.instanceId);

        // Write primitive ID
        if (resUsage->builtInUsage.vs.primitiveId) {
          assert(m_distributedPrimitiveId);
          writeVertexCullInfoToLds(m_distributedPrimitiveId, vertexItemOffset, m_vertCullInfoOffsets.primitiveId);
        }
      }

      m_builder.CreateBr(endCompactVertexBlock);
    }
  } else {
    // Mark ".compactVertex" block as unused
    {
      m_builder.SetInsertPoint(compactVertexBlock);
      m_builder.CreateUnreachable();
    }
  }

  // Construct ".endCompactVertex" block
  Value *fullyCulled = nullptr;
  Value *primCountInSubgroup = nullptr;
  {
    m_builder.SetInsertPoint(endCompactVertexBlock);

    fullyCulled = m_builder.CreateICmpEQ(vertCountInSubgroup, m_builder.getInt32(0));

    primCountInSubgroup =
        m_builder.CreateSelect(fullyCulled, m_builder.getInt32(dummyExportCount), m_nggInputs.primCountInSubgroup);

    // NOTE: Here, we have to promote revised primitive count in subgroup to SGPR since it is treated
    // as an uniform value later. This is similar to the provided primitive count in subgroup that is
    // a system value.
    primCountInSubgroup =
        m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, primCountInSubgroup);

    vertCountInSubgroup =
        m_builder.CreateSelect(fullyCulled, m_builder.getInt32(dummyExportCount),
                               m_nggControl->compactVertex ? vertCountInSubgroup : m_nggInputs.vertCountInSubgroup);

    // NOTE: Here, we have to promote revised vertex count in subgroup to SGPR since it is treated as
    // an uniform value later, similar to what we have done for the revised primitive count in
    // subgroup.
    vertCountInSubgroup =
        m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, vertCountInSubgroup);

    m_builder.CreateBr(checkSendGsAllocReqBlock);
  }

  // Construct ".checkSendGsAllocReq" block
  {
    m_builder.SetInsertPoint(checkSendGsAllocReqBlock);

    // NOTE: Here, we make several phi nodes to update some values that are different in runtime passthrough path
    // and no runtime passthrough path (normal culling path).
    if (m_nggControl->compactVertex) {
      m_compactVertex =
          createPhi({{hasCulledVertices, endCompactVertexBlock}, {m_builder.getFalse(), endCollectCullDataBlock}},
                    "compactVertex");
    } else {
      assert(!m_compactVertex); // Must be null
    }

    // Update primitive culled flag
    primitiveCulled = createPhi(
        {{primitiveCulled, endCompactVertexBlock}, {m_builder.getFalse(), endCollectCullDataBlock}}, "primitiveCulled");

    // Update fully-culled flag
    fullyCulled = createPhi({{fullyCulled, endCompactVertexBlock}, {m_builder.getFalse(), endCollectCullDataBlock}},
                            "fullyCulled");

    // Update primitive count in subgroup
    m_nggInputs.primCountInSubgroup = createPhi(
        {{primCountInSubgroup, endCompactVertexBlock}, {m_nggInputs.primCountInSubgroup, endCollectCullDataBlock}},
        "primCountInSubgroup");

    // Update vertex count in subgroup
    m_nggInputs.vertCountInSubgroup = createPhi(
        {{vertCountInSubgroup, endCompactVertexBlock}, {m_nggInputs.vertCountInSubgroup, endCollectCullDataBlock}},
        "vertCountInSubgroup");

    if (!m_nggControl->compactVertex) {
      // Update draw flag
      drawFlag =
          createPhi({{drawFlag, endCompactVertexBlock}, {m_builder.getTrue(), endCollectCullDataBlock}}, "drawFlag");

      // Update vertex count in wave
      vertCountInWave =
          createPhi({{vertCountInWave, endCompactVertexBlock}, {m_nggInputs.vertCountInWave, endCollectCullDataBlock}},
                    "vertCountInWave");
    }

    auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstWaveInSubgroup, sendGsAllocReqBlock, endSendGsAllocReqBlock);
  }

  // Construct ".sendGsAllocReq" block
  {
    m_builder.SetInsertPoint(sendGsAllocReqBlock);

    sendGsAllocReqMessage();
    m_builder.CreateBr(endSendGsAllocReqBlock);
  }

  // Construct ".endSendGsAllocReq" block
  {
    m_builder.SetInsertPoint(endSendGsAllocReqBlock);

    createFenceAndBarrier();

    if (waNggCullingNoEmptySubgroups)
      m_builder.CreateCondBr(fullyCulled, earlyExitBlock, checkCollectVertexExportBlock);
    else
      m_builder.CreateBr(checkCollectVertexExportBlock);
  }

  if (waNggCullingNoEmptySubgroups) {
    // Construct ".earlyExit" block
    {
      m_builder.SetInsertPoint(earlyExitBlock);

      if (dummyExportCount > 0)
        earlyExitWithDummyExport();
      else
        m_builder.CreateRetVoid();
    }
  } else {
    // Mark ".earlyExit" block as unused
    {
      m_builder.SetInsertPoint(earlyExitBlock);
      m_builder.CreateUnreachable();
    }
  }

  // Construct ".checkCollectVertexExport" block
  {
    m_builder.SetInsertPoint(checkCollectVertexExportBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    if (!m_nggControl->compactVertex)
      validVertex = m_builder.CreateAnd(validVertex, drawFlag); // Culled vertex vertices will not be drawn
    m_builder.CreateCondBr(validVertex, collectVertexExportBlock, endCollectVertexExportBlock);
  }

  // Construct ".collectVertexExport" block
  positionExports.clear(); // Will be reused, clear it
  SmallVector<VertexExport, 32> attributeExports;
  {
    m_builder.SetInsertPoint(collectVertexExportBlock);

    collectExports(args, m_esHandlers.part.second, false, &positionExports, &attributeExports, nullptr);

    // NOTE: After ES splitting, position0 is not contained in the ES second part. We have to insert it back to the
    // collection of position exports.
    if (m_compactVertex) {
      auto uncompactedVertexIndex = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                             PrimShaderLdsRegion::VertexIndexMap);
      auto newPosition0 =
          readPerThreadDataFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), uncompactedVertexIndex,
                                   PrimShaderLdsRegion::VertexPosition, 0, true);
      position0 = m_builder.CreateSelect(m_compactVertex, newPosition0, position0);
    }

    m_builder.CreateBr(endCollectVertexExportBlock);
  }

  // Construct ".endCollectVertexExport" block
  {
    m_builder.SetInsertPoint(endCollectVertexExportBlock);

    createPhiForExports(&positionExports, &attributeExports, nullptr);

    if (m_compactVertex) {
      auto exportBlock = cast<Instruction>(position0)->getParent();
      auto position0Phi =
          m_builder.CreatePHI(position0->getType(), pred_size(endCollectVertexExportBlock), "position0");
      for (BasicBlock *predBlock : predecessors(endCollectVertexExportBlock)) {
        position0Phi->addIncoming(predBlock == exportBlock ? position0 : PoisonValue::get(position0->getType()),
                                  predBlock);
      }
      position0 = position0Phi;
    }
    positionExports.push_back({0, 0xF, position0});

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, exportPrimitiveBlock, endExportPrimitiveBlock);
  }

  // Construct ".exportPrimitive" block
  {
    m_builder.SetInsertPoint(exportPrimitiveBlock);

    exportPrimitive(primitiveCulled);

    m_builder.CreateBr(endExportPrimitiveBlock);
  }

  // Construct ".endExportPrimitive" block
  {
    m_builder.SetInsertPoint(endExportPrimitiveBlock);

    if (m_nggControl->compactVertex) {
      m_builder.CreateBr(checkExportVertexBlock);
    } else {
      auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(validVertex, checkEmptyWaveBlock, endExportVertexBlock);
    }
  }

  if (m_nggControl->compactVertex) {
    // Mark ".checkEmptyWave" block as unused
    {
      m_builder.SetInsertPoint(checkEmptyWaveBlock);
      m_builder.CreateUnreachable();
    }

    // Mark ".dummyVertexExport" block as unused
    {
      m_builder.SetInsertPoint(dummyVertexExportBlock);
      m_builder.CreateUnreachable();
    }
  } else {
    // Construct ".checkEmptyWave" block
    {
      m_builder.SetInsertPoint(checkEmptyWaveBlock);

      auto emptyWave = m_builder.CreateICmpEQ(vertCountInWave, m_builder.getInt32(0));
      m_builder.CreateCondBr(emptyWave, dummyVertexExportBlock, checkExportVertexBlock);
    }

    // Construct ".dummyVertexExport" block
    {
      m_builder.SetInsertPoint(dummyVertexExportBlock);

      auto poison = PoisonValue::get(m_builder.getFloatTy());
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                {
                                    m_builder.getInt32(EXP_TARGET_POS_0), // tgt
                                    m_builder.getInt32(0x0),              // en
                                    // src0 ~ src3
                                    poison, poison, poison, poison,
                                    m_builder.getTrue(), // done
                                    m_builder.getFalse() // vm
                                });

      m_builder.CreateRetVoid();
    }
  }

  // Construct ".checkExportVertexBlock" block
  {
    m_builder.SetInsertPoint(checkExportVertexBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    if (!m_nggControl->compactVertex)
      validVertex = m_builder.CreateAnd(validVertex, drawFlag); // Culled vertex vertices will not be drawn
    m_builder.CreateCondBr(validVertex, exportVertexBlock, endExportVertexBlock);
  }

  // Construct ".exportVertex" block
  {
    m_builder.SetInsertPoint(exportVertexBlock);

    // NOTE: If the workaround of attributes-through-memory preceding vertex position data is required, we have to
    // place vertex exports after all attribute exports (ATM operations).
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos) {
      exportAttributes(attributeExports);
      if (!attributeExports.empty())
        m_builder.CreateFence(AtomicOrdering::Release, m_builder.getContext().getOrInsertSyncScopeID("agent"));
      exportPositions(positionExports);
    } else {
      exportPositions(positionExports);
      exportAttributes(attributeExports);
    }

    m_builder.CreateBr(endExportVertexBlock);
  }

  // Construct ".endExportVertex" block
  {
    m_builder.SetInsertPoint(endExportVertexBlock);

    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Build the body of primitive shader when API GS is present.
//
// @param primShader : Entry-point of primitive shader to build
void NggPrimShader::buildPrimShaderWithGs(Function *primShader) {
  assert(m_hasGs); // Make sure API GS is present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  assert(waveSize == 32 || waveSize == 64);

  if (!m_nggControl->compactVertex)
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  const bool cullingMode = !m_nggControl->passthroughMode;

  const auto rasterStream = m_pipelineState->getRasterizerState().rasterStream;
  const bool noRasterization = rasterStream == InvalidValue;

  SmallVector<Argument *, 32> args;
  for (auto &arg : primShader->args())
    args.push_back(&arg);

  Value *mergedGroupInfo = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedGroupInfo)];
  mergedGroupInfo->setName("mergedGroupInfo");

  Value *mergedWaveInfo = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::MergedWaveInfo)];
  mergedWaveInfo->setName("mergedWaveInfo");

  Value *attribRingBase = nullptr;
  if (m_gfxIp.major >= 11) {
    attribRingBase = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase)];
    attribRingBase->setName("attribRingBase");
  }

  // GS shader address is reused as primitive shader table address for NGG culling
  Value *primShaderTableAddrLow = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrLow)];
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::GsShaderAddrHigh)];
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  Value *userData = args[NumSpecialSgprInputs];

  //
  // The processing is something like this:
  //
  // NOTE: We purposely set primitive amplification to be max_vertices (treat line_strip and triangle_strip as point).
  // This will make primCountInSubgroup equal to vertCountInSubgroup to simplify the algorithm.
  //
  // NGG_GS() {
  //   Initialize thread/wave info
  //
  //   if (threadIdInWave < vertCountInWave)
  //     Run ES
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Initialize primitive connectivity data (0x80000000)
  //   Barrier
  //
  //   if (threadIdInWave < primCountInWave)
  //     Run GS
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Collect XFB exports
  //
  //   if (Enable SW XFB)
  //     Process SW XFB
  //   else if (Enable primitive statistics counting)
  //     Collect primitive statistics
  //
  //  if (threadIdInSubgroup < maxWaves + 1)
  //     Initialize per-wave and per-subgroup count of output vertices
  //   Barrier
  //
  //   if (Culling mode && valid primitive & threadIdInSubgroup < primCountInSubgroup) {
  //     Cull primitive (run culling algorithms)
  //     if (primitive culled)
  //       Nullify primitive connectivity data
  //   }
  //   Barrier
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Check draw flags of output vertices and compute draw mask
  //
  //   if (threadIdInWave < maxWaves - waveId)
  //     Accumulate per-wave and per-subgroup count of output vertices
  //   Barrier
  //   Update vertCountInSubgroup
  //
  //   if (Need compact vertex && vertex drawn)
  //     Compact vertex index (compacted -> uncompacted)
  //
  //   if (waveId == 0)
  //     Send GS_ALLOC_REQ message
  //   Barrier
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Collect vertex exports
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Export primitive
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     if (Needn't compact vertex && empty wave)
  //       Dummy vertex export
  //     else if (drawFlag)
  //       Export vertex
  //   }
  // }
  //

  // Define basic blocks
  auto entryBlock = createBlock(primShader, ".entry");

  auto beginEsBlock = createBlock(primShader, ".beginEs");
  auto endEsBlock = createBlock(primShader, ".endEs");

  auto initPrimitiveDataBlock = createBlock(primShader, ".initPrimitiveData");
  auto endInitPrimitiveDataBlock = createBlock(primShader, ".endInitPrimitiveData");

  auto beginGsBlock = createBlock(primShader, ".beginGs");
  auto endGsBlock = createBlock(primShader, ".endGs");

  auto collectXfbExportBlock = createBlock(primShader, ".collectXfbExport");
  auto endCollectXfbExportBlock = createBlock(primShader, ".endCollectXfbExport");

  BasicBlock *initVertexCountsBlock = nullptr;
  BasicBlock *endInitVertexCountsBlock = nullptr;

  BasicBlock *cullPrimitiveBlock = nullptr;
  BasicBlock *nullifyPrimitiveDataBlock = nullptr;
  BasicBlock *endCullPrimitiveBlock = nullptr;

  BasicBlock *checkVertexDrawFlagBlock = nullptr;
  BasicBlock *endCheckVertexDrawFlagBlock = nullptr;

  BasicBlock *accumVertexCountsBlock = nullptr;
  BasicBlock *endAccumVertexCountsBlock = nullptr;

  BasicBlock *compactVertexIndexBlock = nullptr;
  BasicBlock *endCompactVertexIndexBlock = nullptr;

  BasicBlock *sendGsAllocReqBlock = nullptr;
  BasicBlock *endSendGsAllocReqBlock = nullptr;

  BasicBlock *collectVertexExportBlock = nullptr;
  BasicBlock *endCollectVertexExportBlock = nullptr;

  BasicBlock *exportPrimitiveBlock = nullptr;
  BasicBlock *endExportPrimitiveBlock = nullptr;

  BasicBlock *checkEmptyWaveBlock = nullptr;
  BasicBlock *dummyVertexExportBlock = nullptr;
  BasicBlock *checkExportVertexBlock = nullptr;

  BasicBlock *exportVertexBlock = nullptr;
  BasicBlock *endExportVertexBlock = nullptr;

  if (noRasterization) {
    // NOTE: For the case of no rasterization (DX-specific), primitive/vertex exports could be completely ignored.
    // We just send message GS_ALLOC_REQ to tell HW we don't have any primitive/vertex to export.
    sendGsAllocReqBlock = createBlock(primShader, ".sendGsAllocReq");
    endSendGsAllocReqBlock = createBlock(primShader, ".endSendGsAllocReq");
  } else {
    initVertexCountsBlock = createBlock(primShader, ".initVertexCounts");
    endInitVertexCountsBlock = createBlock(primShader, ".endInitVertexCounts");

    cullPrimitiveBlock = createBlock(primShader, ".cullPrimitive");
    nullifyPrimitiveDataBlock = createBlock(primShader, ".nullifyPrimitiveData");
    endCullPrimitiveBlock = createBlock(primShader, ".endCullPrimitive");

    checkVertexDrawFlagBlock = createBlock(primShader, ".checkVertexDrawFlag");
    endCheckVertexDrawFlagBlock = createBlock(primShader, ".endCheckVertexDrawFlag");

    accumVertexCountsBlock = createBlock(primShader, ".accumVertexCounts");
    endAccumVertexCountsBlock = createBlock(primShader, ".endAccumVertexCounts");

    compactVertexIndexBlock = createBlock(primShader, ".compactVertexIndex");
    endCompactVertexIndexBlock = createBlock(primShader, ".endCompactVertexIndex");

    sendGsAllocReqBlock = createBlock(primShader, ".sendGsAllocReq");
    endSendGsAllocReqBlock = createBlock(primShader, ".endSendGsAllocReq");

    collectVertexExportBlock = createBlock(primShader, ".collectVertexExport");
    endCollectVertexExportBlock = createBlock(primShader, ".endCollectVertexExport");

    exportPrimitiveBlock = createBlock(primShader, ".exportPrimitive");
    endExportPrimitiveBlock = createBlock(primShader, ".endExportPrimitive");

    checkEmptyWaveBlock = createBlock(primShader, ".checkEmptyWave");
    dummyVertexExportBlock = createBlock(primShader, ".dummyVertexExport");
    checkExportVertexBlock = createBlock(primShader, ".checkExportVertex");

    exportVertexBlock = createBlock(primShader, ".exportVertex");
    endExportVertexBlock = createBlock(primShader, ".endExportVertex");
  }

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      if (!m_pipelineState->exportAttributeByExportInstruction())
        prepareAttribRingAccess(userData);

      if (m_pipelineState->enableSwXfb() || m_pipelineState->enablePrimStats())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive shader table address info
    m_nggInputs.primShaderTableAddr = std::make_pair(primShaderTableAddrLow, primShaderTableAddrHigh);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
    m_builder.CreateCondBr(validVertex, beginEsBlock, endEsBlock);
  }

  // Construct ".beginEs" block
  {
    m_builder.SetInsertPoint(beginEsBlock);

    runEs(args);

    m_builder.CreateBr(endEsBlock);
  }

  // Construct ".endEs" block
  {
    m_builder.SetInsertPoint(endEsBlock);

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, initPrimitiveDataBlock, endInitPrimitiveDataBlock);
  }

  // Construct ".initPrimitiveData" block
  {
    m_builder.SetInsertPoint(initPrimitiveDataBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) { // Initialize primitive connectivity data if the stream is active
        writePerThreadDataToLds(m_builder.getInt32(NullPrim), m_nggInputs.threadIdInSubgroup,
                                PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * i);
      }
    }

    m_builder.CreateBr(endInitPrimitiveDataBlock);
  }

  // Construct ".endInitPrimitiveData" block
  {
    m_builder.SetInsertPoint(endInitPrimitiveDataBlock);

    createFenceAndBarrier();

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.primCountInWave);
    m_builder.CreateCondBr(validPrimitive, beginGsBlock, endGsBlock);
  }

  // Construct ".beginGs" block
  {
    m_builder.SetInsertPoint(beginGsBlock);

    runGs(args);

    m_builder.CreateBr(endGsBlock);
  }

  // Construct ".endGs" block
  {
    m_builder.SetInsertPoint(endGsBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, collectXfbExportBlock, endCollectXfbExportBlock);
  }

  // Construct ".collectXfbExport" block
  SmallVector<XfbExport, 32> xfbExports;
  {
    m_builder.SetInsertPoint(collectXfbExportBlock);

    if (m_pipelineState->enableSwXfb())
      collectExports(args, m_gsHandlers.copyShader, true, nullptr, nullptr, &xfbExports);

    m_builder.CreateBr(endCollectXfbExportBlock);
  }

  // Construct ".endCollectXfbExport" block
  {
    m_builder.SetInsertPoint(endCollectXfbExportBlock);

    if (m_pipelineState->enableSwXfb()) {
      createPhiForExports(nullptr, nullptr, &xfbExports);
      processSwXfbWithGs(args, xfbExports);
    } else if (m_pipelineState->enablePrimStats()) {
      collectPrimitiveStats();
    }

    if (noRasterization) {
      auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstWaveInSubgroup, sendGsAllocReqBlock, endSendGsAllocReqBlock);
    } else {
      auto validWave =
          m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(m_maxWavesPerSubgroup + 1));
      m_builder.CreateCondBr(validWave, initVertexCountsBlock, endInitVertexCountsBlock);
    }
  }

  // NOTE: Here, we handle the case of no rasterization (DX-specific). In such case, primitive/vertex exports could be
  // completely ignored.
  if (noRasterization) {
    // Construct ".sendGsAllocReq" block
    {
      m_builder.SetInsertPoint(sendGsAllocReqBlock);

      // Clear primitive/vertex count
      m_nggInputs.primCountInSubgroup = m_builder.getInt32(0);
      m_nggInputs.vertCountInSubgroup = m_builder.getInt32(0);

      sendGsAllocReqMessage();
      m_builder.CreateBr(endSendGsAllocReqBlock);
    }

    // Construct ".endSendGsAllocReq" block
    {
      m_builder.SetInsertPoint(endSendGsAllocReqBlock);

      m_builder.CreateRetVoid(); // Early return for no rasterization case
    }

    return;
  }

  // The rasterization stream must be specified now
  assert(rasterStream != InvalidValue);

  // Construct ".initVertexCounts" block
  {
    m_builder.SetInsertPoint(initVertexCountsBlock);

    writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup, PrimShaderLdsRegion::VertexCounts,
                            (m_maxWavesPerSubgroup + 1) * rasterStream);

    m_builder.CreateBr(endInitVertexCountsBlock);
  }

  // Construct ".endInitVertexCounts" block
  Value *primData = nullptr;
  {
    m_builder.SetInsertPoint(endInitVertexCountsBlock);

    createFenceAndBarrier();

    if (cullingMode) {
      primData = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                          PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * rasterStream);
      auto tryCullPrimitive = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));
      auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
      tryCullPrimitive = m_builder.CreateAnd(tryCullPrimitive, validPrimitive);
      m_builder.CreateCondBr(tryCullPrimitive, cullPrimitiveBlock, endCullPrimitiveBlock);
    } else {
      m_builder.CreateBr(endCullPrimitiveBlock);
    }
  }

  if (cullingMode) {
    // Construct ".cullPrimitive" block
    {
      m_builder.SetInsertPoint(cullPrimitiveBlock);

      assert(m_pipelineState->getShaderModes()->getGeometryShaderMode().outputPrimitive ==
             OutputPrimitives::TriangleStrip);

      // NOTE: primData[N] corresponds to the forming vertex
      // The vertice indices in the first triangle <N, N+1, N+2>
      // If provoking vertex is the first one, the vertice indices in the second triangle is <N, N+2, N+1>, otherwise it
      // is <N+1, N, N+2>.
      unsigned windingIndices[3] = {};
      if (m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst) {
        windingIndices[0] = 0;
        windingIndices[1] = 2;
        windingIndices[2] = 1;
      } else {
        windingIndices[0] = 1;
        windingIndices[1] = 0;
        windingIndices[2] = 2;
      }
      Value *winding = m_builder.CreateICmpNE(primData, m_builder.getInt32(0));
      auto vertexIndex0 = m_builder.CreateAdd(
          m_nggInputs.threadIdInSubgroup,
          m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[0]), m_builder.getInt32(0)));
      auto vertexIndex1 = m_builder.CreateAdd(
          m_nggInputs.threadIdInSubgroup,
          m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[1]), m_builder.getInt32(1)));
      auto vertexIndex2 = m_builder.CreateAdd(
          m_nggInputs.threadIdInSubgroup,
          m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[2]), m_builder.getInt32(2)));

      auto primitiveCulled = cullPrimitive(vertexIndex0, vertexIndex1, vertexIndex2);
      m_builder.CreateCondBr(primitiveCulled, nullifyPrimitiveDataBlock, endCullPrimitiveBlock);
    }

    // Construct ".nullifyPrimitiveData" block
    {
      m_builder.SetInsertPoint(nullifyPrimitiveDataBlock);

      writePerThreadDataToLds(m_builder.getInt32(NullPrim), m_nggInputs.threadIdInSubgroup,
                              PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * rasterStream);

      m_builder.CreateBr(endCullPrimitiveBlock);
    }
  } else {
    // Mark ".cullPrimitive" block as unused
    {
      m_builder.SetInsertPoint(cullPrimitiveBlock);
      m_builder.CreateUnreachable();
    }

    // Mark ".nullifyPrimitiveData" block as unused
    {
      m_builder.SetInsertPoint(nullifyPrimitiveDataBlock);
      m_builder.CreateUnreachable();
    }
  }

  // Construct ".endCullPrimitive" block
  {
    m_builder.SetInsertPoint(endCullPrimitiveBlock);

    if (cullingMode)
      createFenceAndBarrier(); // Make sure we have completed updating primitive connectivity data

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, checkVertexDrawFlagBlock, endCheckVertexDrawFlagBlock);
  }

  // Construct ".checkVertexDrawFlag"
  Value *drawFlag = nullptr;
  {
    m_builder.SetInsertPoint(checkVertexDrawFlagBlock);

    const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();

    // drawFlag = primData[N] != NullPrim
    auto primData0 =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                 PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * rasterStream);
    auto drawFlag0 = m_builder.CreateICmpNE(primData0, m_builder.getInt32(NullPrim));
    drawFlag = drawFlag0;

    if (outVertsPerPrim > 1) {
      // drawFlag |= N >= 1 ? (primData[N-1] != NullPrim) : false
      auto primData1 = readPerThreadDataFromLds(
          m_builder.getInt32Ty(), m_builder.CreateSub(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(1)),
          PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * rasterStream);
      auto drawFlag1 =
          m_builder.CreateSelect(m_builder.CreateICmpUGE(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(1)),
                                 m_builder.CreateICmpNE(primData1, m_builder.getInt32(NullPrim)), m_builder.getFalse());
      drawFlag = m_builder.CreateOr(drawFlag, drawFlag1);
    }

    if (outVertsPerPrim > 2) {
      // drawFlag |= N >= 2 ? (primData[N-2] != NullPrim) : false
      auto primData2 = readPerThreadDataFromLds(
          m_builder.getInt32Ty(), m_builder.CreateSub(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(2)),
          PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * rasterStream);
      auto drawFlag2 =
          m_builder.CreateSelect(m_builder.CreateICmpUGE(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(2)),
                                 m_builder.CreateICmpNE(primData2, m_builder.getInt32(NullPrim)), m_builder.getFalse());
      drawFlag = m_builder.CreateOr(drawFlag, drawFlag2);
    }

    m_builder.CreateBr(endCheckVertexDrawFlagBlock);
  }

  // Construct ".endCheckVertexDrawFlag"
  Value *drawMask = nullptr;
  Value *vertCountInWave = nullptr;
  {
    m_builder.SetInsertPoint(endCheckVertexDrawFlagBlock);

    // NOTE: The predecessors are different if culling mode is enabled.
    drawFlag =
        createPhi({{drawFlag, checkVertexDrawFlagBlock}, {m_builder.getFalse(), endCullPrimitiveBlock}}, "drawFlag");
    drawMask = ballot(drawFlag);

    vertCountInWave = m_builder.CreateIntrinsic(Intrinsic::ctpop, m_builder.getInt64Ty(), drawMask);
    vertCountInWave = m_builder.CreateTrunc(vertCountInWave, m_builder.getInt32Ty());

    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(m_maxWavesPerSubgroup), m_nggInputs.waveIdInSubgroup);
    auto validThread = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);

    m_builder.CreateCondBr(validThread, accumVertexCountsBlock, endAccumVertexCountsBlock);
  }

  // Construct ".accumVertexCounts" block
  {
    m_builder.SetInsertPoint(accumVertexCountsBlock);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));

    unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCounts);

    ldsOffset =
        m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + (m_maxWavesPerSubgroup + 1) * rasterStream));
    atomicOp(AtomicRMWInst::Add, vertCountInWave, ldsOffset);

    m_builder.CreateBr(endAccumVertexCountsBlock);
  }

  // Construct ".endAccumVertexCounts" block
  Value *vertCountInPrevWaves = nullptr;
  {
    m_builder.SetInsertPoint(endAccumVertexCountsBlock);

    createFenceAndBarrier();

    if (m_nggControl->compactVertex) {
      auto vertCountInWaves =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave,
                                   PrimShaderLdsRegion::VertexCounts, (m_maxWavesPerSubgroup + 1) * rasterStream);

      // The last dword following dwords for all waves (each wave has one dword) stores GS output vertex count of the
      // entire subgroup
      auto vertCountInSubgroup =
          m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                    {vertCountInWaves, m_builder.getInt32(m_maxWavesPerSubgroup)});

      // Get output vertex count for all waves prior to this wave
      vertCountInPrevWaves = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                       {vertCountInWaves, m_nggInputs.waveIdInSubgroup});

      auto hasCulledVertices = m_builder.CreateICmpULT(vertCountInSubgroup, m_nggInputs.vertCountInSubgroup);

      m_nggInputs.vertCountInSubgroup = vertCountInSubgroup; // Update GS output vertex count in subgroup
      m_compactVertex = hasCulledVertices;

      m_builder.CreateCondBr(m_builder.CreateAnd(drawFlag, hasCulledVertices), compactVertexIndexBlock,
                             endCompactVertexIndexBlock);
    } else {
      m_builder.CreateBr(endCompactVertexIndexBlock);
    }
  }

  Value *compactedVertexIndex = nullptr;
  if (m_nggControl->compactVertex) {
    // Construct ".compactVertexIndex" block
    {
      m_builder.SetInsertPoint(compactVertexIndexBlock);

      auto drawMaskVec = m_builder.CreateBitCast(drawMask, FixedVectorType::get(m_builder.getInt32Ty(), 2));

      auto drawMaskLow = m_builder.CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
      compactedVertexIndex =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder.getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder.CreateExtractElement(drawMaskVec, 1);
        compactedVertexIndex =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactedVertexIndex});
      }

      compactedVertexIndex = m_builder.CreateAdd(vertCountInPrevWaves, compactedVertexIndex);
      writePerThreadDataToLds(m_nggInputs.threadIdInSubgroup, compactedVertexIndex,
                              PrimShaderLdsRegion::VertexIndexMap);

      m_builder.CreateBr(endCompactVertexIndexBlock);
    }
  } else {
    // Mark ".compactVertexIndex" block as unused
    m_builder.SetInsertPoint(compactVertexIndexBlock);
    m_builder.CreateUnreachable();
  }

  // Construct ".endCompactVertexIndex" block
  {
    m_builder.SetInsertPoint(endCompactVertexIndexBlock);

    if (m_nggControl->compactVertex) {
      compactedVertexIndex = createPhi({{compactedVertexIndex, compactVertexIndexBlock},
                                        {m_nggInputs.threadIdInSubgroup, endAccumVertexCountsBlock}});

      createFenceAndBarrier(); // Make sure we have completed writing compacted vertex indices
    }

    auto firstWaveInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstWaveInSubgroup, sendGsAllocReqBlock, endSendGsAllocReqBlock);
  }

  // Construct ".sendGsAllocReq" block
  {
    m_builder.SetInsertPoint(sendGsAllocReqBlock);

    sendGsAllocReqMessage();
    m_builder.CreateBr(endSendGsAllocReqBlock);
  }

  // Construct ".endSendGsAllocReq" block
  {
    m_builder.SetInsertPoint(endSendGsAllocReqBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    if (!m_nggControl->compactVertex)
      validVertex = m_builder.CreateAnd(validVertex, drawFlag); // Culled vertex vertices will not be drawn
    m_builder.CreateCondBr(validVertex, collectVertexExportBlock, endCollectVertexExportBlock);
  }

  // Construct ".collectVertexExport" block
  SmallVector<VertexExport, 4> positionExports;
  SmallVector<VertexExport, 32> attributeExports;
  {
    m_builder.SetInsertPoint(collectVertexExportBlock);

    mutateCopyShader();
    collectExports(args, m_gsHandlers.copyShader, false, &positionExports, &attributeExports, nullptr);

    m_builder.CreateBr(endCollectVertexExportBlock);
  }

  // Construct ".endCollectVertexExport" block
  {
    m_builder.SetInsertPoint(endCollectVertexExportBlock);

    createPhiForExports(&positionExports, &attributeExports, nullptr);

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, exportPrimitiveBlock, endExportPrimitiveBlock);
  }

  // Construct ".exportPrimitive" block
  {
    m_builder.SetInsertPoint(exportPrimitiveBlock);

    auto startingVertexIndex = m_nggControl->compactVertex ? compactedVertexIndex : m_nggInputs.threadIdInSubgroup;
    exportPrimitiveWithGs(startingVertexIndex);
    m_builder.CreateBr(endExportPrimitiveBlock);
  }

  // Construct ".endExportPrimitive" block
  {
    m_builder.SetInsertPoint(endExportPrimitiveBlock);

    if (m_nggControl->compactVertex) {
      m_builder.CreateBr(checkExportVertexBlock);
    } else {
      auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
      m_builder.CreateCondBr(validVertex, checkEmptyWaveBlock, endExportVertexBlock);
    }
  }

  if (m_nggControl->compactVertex) {
    // Mark ".checkEmptyWave" block as unused
    {
      m_builder.SetInsertPoint(checkEmptyWaveBlock);
      m_builder.CreateUnreachable();
    }

    // Mark ".dummyVertexExport" block as unused
    {
      m_builder.SetInsertPoint(dummyVertexExportBlock);
      m_builder.CreateUnreachable();
    }
  } else {
    // Construct ".checkEmptyWave" block
    {
      m_builder.SetInsertPoint(checkEmptyWaveBlock);

      auto emptyWave = m_builder.CreateICmpEQ(vertCountInWave, m_builder.getInt32(0));
      m_builder.CreateCondBr(emptyWave, dummyVertexExportBlock, checkExportVertexBlock);
    }

    // Construct ".dummyVertexExport" block
    {
      m_builder.SetInsertPoint(dummyVertexExportBlock);

      auto poison = PoisonValue::get(m_builder.getFloatTy());
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                {
                                    m_builder.getInt32(EXP_TARGET_POS_0), // tgt
                                    m_builder.getInt32(0x0),              // en
                                    // src0 ~ src3
                                    poison, poison, poison, poison,
                                    m_builder.getTrue(), // done
                                    m_builder.getFalse() // vm
                                });

      m_builder.CreateRetVoid();
    }
  }

  // Construct ".checkExportVertex" block
  {
    m_builder.SetInsertPoint(checkExportVertexBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    if (!m_nggControl->compactVertex)
      validVertex = m_builder.CreateAnd(validVertex, drawFlag); // Culled vertex vertices will not be drawn
    m_builder.CreateCondBr(validVertex, exportVertexBlock, endExportVertexBlock);
  }

  // Construct ".exportVertex" block
  {
    m_builder.SetInsertPoint(exportVertexBlock);

    // NOTE: If the workaround of attributes-through-memory preceding vertex position data is required, we have to
    // place vertex exports after all attribute exports (ATM operations).
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos) {
      exportAttributes(attributeExports);
      if (!attributeExports.empty())
        m_builder.CreateFence(AtomicOrdering::Release, m_builder.getContext().getOrInsertSyncScopeID("agent"));
      exportPositions(positionExports);
    } else {
      exportPositions(positionExports);
      exportAttributes(attributeExports);
    }

    m_builder.CreateBr(endExportVertexBlock);
  }

  // Construct ".endExportVertex" block
  {
    m_builder.SetInsertPoint(endExportVertexBlock);

    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Extracts merged group/wave info and initializes part of NGG inputs.
//
// NOTE: This function must be invoked by the entry block of NGG shader module.
//
// @param mergedGroupInfo : Merged group info
// @param mergedWaveInfo : Merged wave info
void NggPrimShader::initWaveThreadInfo(Value *mergedGroupInfo, Value *mergedWaveInfo) {
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  assert(waveSize == 32 || waveSize == 64);

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder.getInt64(-1));

  auto threadIdInWave =
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), threadIdInWave});
  }

  auto primCountInSubgroup = createUBfe(mergedGroupInfo, 22, 9);
  auto vertCountInSubgroup = createUBfe(mergedGroupInfo, 12, 9);
  auto vertCountInWave = createUBfe(mergedWaveInfo, 0, 8);
  auto primCountInWave = createUBfe(mergedWaveInfo, 8, 8);
  auto waveIdInSubgroup = createUBfe(mergedWaveInfo, 24, 4);
  auto orderedWaveId = createUBfe(mergedGroupInfo, 0, 12);

  auto threadIdInSubgroup = m_builder.CreateMul(waveIdInSubgroup, m_builder.getInt32(waveSize));
  threadIdInSubgroup = m_builder.CreateAdd(threadIdInSubgroup, threadIdInWave);

  primCountInSubgroup->setName("primCountInSubgroup");
  vertCountInSubgroup->setName("vertCountInSubgroup");
  primCountInWave->setName("primCountInWave");
  vertCountInWave->setName("vertCountInWave");
  threadIdInWave->setName("threadIdInWave");
  threadIdInSubgroup->setName("threadIdInSubgroup");
  waveIdInSubgroup->setName("waveIdInSubgroup");
  orderedWaveId->setName("orderedWaveId");

  // Record wave/thread info
  m_nggInputs.primCountInSubgroup = primCountInSubgroup;
  m_nggInputs.vertCountInSubgroup = vertCountInSubgroup;
  m_nggInputs.primCountInWave = primCountInWave;
  m_nggInputs.vertCountInWave = vertCountInWave;
  m_nggInputs.threadIdInWave = threadIdInWave;
  m_nggInputs.threadIdInSubgroup = threadIdInSubgroup;
  m_nggInputs.waveIdInSubgroup = waveIdInSubgroup;
  m_nggInputs.orderedWaveId = orderedWaveId;
}

// =====================================================================================================================
// Prepare attribute ring access by collecting attribute count, modifying the STRIDE field of attribute ring buffer
// descriptor, and calculating subgroup's attribute ring base offset.
//
// @param userData : User data
void NggPrimShader::prepareAttribRingAccess(Value *userData) {
  assert(m_gfxIp.major >= 11);                                    // For GFX11+
  assert(!m_pipelineState->exportAttributeByExportInstruction()); // ATM is allowed

  ShaderStageEnum shaderStage =
      m_hasGs ? ShaderStage::Geometry : (m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex);
  const unsigned attribCount = m_pipelineState->getShaderResourceUsage(shaderStage)->inOutUsage.expCount;
  if (attribCount == 0)
    return; // No vertex attribute exports

  // attribRingBase[14:0]
  auto entryPoint = m_builder.GetInsertBlock()->getParent();
  Value *attribRingBase =
      getFunctionArgument(entryPoint, ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::AttribRingBase));
  attribRingBase = m_builder.CreateAnd(attribRingBase, 0x7FFF);

  static const unsigned AttribGranularity = 32 * SizeOfVec4; // 32 * 16 bytes
  m_attribRingBaseOffset =
      m_builder.CreateMul(attribRingBase, m_builder.getInt32(AttribGranularity), "attribRingBaseOffset");

  assert(userData->getType()->isVectorTy());
  auto globalTablePtrValue = m_builder.CreateExtractElement(userData, static_cast<uint64_t>(0));
  auto globalTablePtr = makePointer(globalTablePtrValue, PointerType::get(m_builder.getContext(), ADDR_SPACE_CONST));

  m_attribRingBufDesc = readValueFromCb(FixedVectorType::get(m_builder.getInt32Ty(), 4), globalTablePtr,
                                        m_builder.getInt32(SiDrvTableOffChipParamCache));

  // Modify the field STRIDE of attribute ring buffer descriptor
  if (attribCount >= 2) {
    // STRIDE = WORD1[30:16], STRIDE is initialized to 16 by the driver, which is the right value for attribCount == 1.
    // We override the value if there are more attributes.
    auto descWord1 = m_builder.CreateExtractElement(m_attribRingBufDesc, 1);
    auto stride = m_builder.getInt32(attribCount * SizeOfVec4);
    if ((attribCount & 1) == 0) {
      // Clear the bit that was set in STRIDE by the driver.
      descWord1 = m_builder.CreateAnd(descWord1, ~0x3FFF0000);
    }
    descWord1 = m_builder.CreateOr(descWord1, m_builder.CreateShl(stride, 16)); // Set new STRIDE
    m_attribRingBufDesc = m_builder.CreateInsertElement(m_attribRingBufDesc, descWord1, 1);
  }
  m_attribRingBufDesc->setName("attribRingBufDesc");
}

// =====================================================================================================================
// Load stream-out info including stream-out buffer descriptors and buffer offsets.
//
// @param userData : User data
void NggPrimShader::loadStreamOutBufferInfo(Value *userData) {
  // Must enable SW emulated stream-out or primitive statistics counting
  assert(m_pipelineState->enableSwXfb() || m_pipelineState->enablePrimStats());

  if (m_pipelineState->enablePrimStats() && !m_pipelineState->enableSwXfb() && m_gfxIp.major <= 11) {
#if LLPC_BUILD_GFX12
    // NOTE: For pre-GFX12, if we only want to do primitive statistics counting (no SW XFB), there is no need of load
    // stream-out buffer info. The primitive counters are in GDS. For GFX12+, GDS is removed and the counters are
    // defined in stream-out control buffer. We still have to load the info.
#endif
    return;
  }

  calcStreamOutControlCbOffsets();

  // Helper to convert argument index to user data index
  auto getUserDataIndex = [&](Function *func, unsigned argIndex) {
    // Traverse all arguments prior to the argument specified by argIndex. All of them should be user data.
    unsigned userDataIndex = 0;
    for (unsigned i = 0; i < argIndex; ++i) {
      auto argTy = func->getArg(i)->getType();
      if (argTy->isVectorTy()) {
        assert(cast<FixedVectorType>(argTy)->getElementType()->isIntegerTy());
        userDataIndex += cast<FixedVectorType>(argTy)->getNumElements();
      } else {
        assert(argTy->isIntegerTy());
        ++userDataIndex;
      }
    }
    return userDataIndex;
  };

  const auto gsOrEsMain = m_hasGs ? m_gsHandlers.main : m_esHandlers.main;
  StreamOutData streamOutData = {};
  if (m_hasGs)
    streamOutData = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs.streamOutData;
  else if (m_hasTes)
    streamOutData = m_pipelineState->getShaderInterfaceData(ShaderStage::TessEval)->entryArgIdxs.tes.streamOutData;
  else
    streamOutData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex)->entryArgIdxs.vs.streamOutData;

  unsigned compositeData = m_pipelineState->getShaderInterfaceData(ShaderStage::Vertex)->entryArgIdxs.vs.compositeData;
  if (compositeData != 0) {
    // Use dynamic topology
    m_verticesPerPrimitive =
        createUBfe(m_builder.CreateExtractElement(userData, getUserDataIndex(gsOrEsMain, compositeData)), 0, 2);
  } else {
    // Use static topology
    m_verticesPerPrimitive = m_builder.getInt32(m_pipelineState->getVerticesPerPrimitive());
  }

  assert(userData->getType()->isVectorTy());
  const auto constBufferPtrTy = PointerType::get(m_builder.getContext(), ADDR_SPACE_CONST);

  // Get stream-out control buffer pointer value
  auto streamOutControlBufPtrValue =
      m_builder.CreateExtractElement(userData, getUserDataIndex(gsOrEsMain, streamOutData.controlBufPtr));
  m_streamOutControlBufPtr = makePointer(streamOutControlBufPtrValue, constBufferPtrTy);

  if (m_pipelineState->enableSwXfb()) {
    // Get stream-out table pointer value
    auto streamOutTablePtrValue =
        m_builder.CreateExtractElement(userData, getUserDataIndex(gsOrEsMain, streamOutData.tablePtr));
    auto streamOutTablePtr = makePointer(streamOutTablePtrValue, constBufferPtrTy);

    const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      bool bufferActive = xfbStrides[i] > 0;
      if (!bufferActive)
        continue; // XFB buffer inactive

      // Get stream-out buffer descriptors and record them
      m_streamOutBufDescs[i] = readValueFromCb(FixedVectorType::get(m_builder.getInt32Ty(), 4), streamOutTablePtr,
                                               m_builder.getInt32(4 * i)); // <4 x i32>

      // NOTE: PAL decided not to invalidate the SQC and L1 for every stream-out update, mainly because that will hurt
      // overall performance worse than just forcing this one buffer to be read via L2. Since PAL would not have wider
      // context, PAL believed that they would have to perform that invalidation on every Set/Load unconditionally.
      // Thus, we force the load of stream-out control buffer to be volatile to let LLVM backend add GLC and DLC flags.
      const bool isVolatile = m_gfxIp.major == 11;
      // Get stream-out buffer offsets and record them
      m_streamOutBufOffsets[i] =
          readValueFromCb(m_builder.getInt32Ty(), m_streamOutControlBufPtr,
                          m_builder.getInt32(m_streamOutControlCbOffsets.bufOffsets[i]), isVolatile); // i32
    }
  }
}

// =====================================================================================================================
// Distribute primitive ID from primitive-based to vertex-based.
//
// @param primitiveId : Primitive ID to distribute (primitive-based, provided by GE)
void NggPrimShader::distributePrimitiveId(Value *primitiveId) {
  // NOTE: If primitive ID is used in VS-FS pipeline, we have to distribute the value across LDS because the primitive
  // ID is provided as primitive-based instead of vertex-based.
  if (m_hasGs || m_hasTes)
    return; // Not VS-PS pipeline

  if (!m_pipelineState->getShaderResourceUsage(ShaderStage::Vertex)->builtInUsage.vs.primitiveId)
    return; // Primitive ID not used in VS

  //
  // The processing is something like this:
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Distribute primitive ID to provoking vertex (vertex0 or vertex2)
  //   Barrier
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Read back distributed primitive ID
  //   Barrier
  //
  auto insertBlock = m_builder.GetInsertBlock();
  auto primShader = insertBlock->getParent();

  auto distribPrimitiveIdBlock = createBlock(primShader, ".distribPrimitiveId");
  distribPrimitiveIdBlock->moveAfter(insertBlock);
  auto endDistribPrimitiveIdBlock = createBlock(primShader, ".endDistribPrimitiveId");
  endDistribPrimitiveIdBlock->moveAfter(distribPrimitiveIdBlock);

  auto readPrimitiveIdBlock = createBlock(primShader, ".readPrimitiveId");
  readPrimitiveIdBlock->moveAfter(endDistribPrimitiveIdBlock);
  auto endReadPrimitiveIdBlock = createBlock(primShader, ".endReadPrimitiveId");
  endReadPrimitiveIdBlock->moveAfter(readPrimitiveIdBlock);

  // Continue to construct insert block
  {
    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, distribPrimitiveIdBlock, endDistribPrimitiveIdBlock);
  }

  // Construct ".distribPrimitiveId" block
  {
    m_builder.SetInsertPoint(distribPrimitiveIdBlock);

    auto primitiveType = m_pipelineState->getInputAssemblyState().primitiveType;
    Value *provokingVertexIndex = nullptr;
    if (primitiveType == PrimitiveType::Point) {
      provokingVertexIndex = m_nggInputs.vertexIndex0;
    } else if (primitiveType == PrimitiveType::LineList || primitiveType == PrimitiveType::LineStrip) {
      provokingVertexIndex = m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst
                                 ? m_nggInputs.vertexIndex0
                                 : m_nggInputs.vertexIndex1;
    } else {
      assert(primitiveType == PrimitiveType::TriangleList || primitiveType == PrimitiveType::TriangleStrip ||
             primitiveType == PrimitiveType::TriangleFan || primitiveType == PrimitiveType::TriangleListAdjacency ||
             primitiveType == PrimitiveType::TriangleStripAdjacency || primitiveType == PrimitiveType::Rect);
      provokingVertexIndex = m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst
                                 ? m_nggInputs.vertexIndex0
                                 : m_nggInputs.vertexIndex2;
    }

    writePerThreadDataToLds(primitiveId, provokingVertexIndex, PrimShaderLdsRegion::DistributedPrimitiveId);

    m_builder.CreateBr(endDistribPrimitiveIdBlock);
  }

  // Construct ".endDistribPrimitiveId" block
  {
    m_builder.SetInsertPoint(endDistribPrimitiveIdBlock);

    createFenceAndBarrier();

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, readPrimitiveIdBlock, endReadPrimitiveIdBlock);
  }

  // Construct ".readPrimitiveId" block
  Value *distributedPrimitiveId = nullptr;
  {
    m_builder.SetInsertPoint(readPrimitiveIdBlock);

    distributedPrimitiveId = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                      PrimShaderLdsRegion::DistributedPrimitiveId);

    m_builder.CreateBr(endReadPrimitiveIdBlock);
  }

  // Construct ".endReadPrimitiveId" block
  {
    m_builder.SetInsertPoint(endReadPrimitiveIdBlock);

    m_distributedPrimitiveId = createPhi({{distributedPrimitiveId, readPrimitiveIdBlock},
                                          {PoisonValue::get(m_builder.getInt32Ty()), endReadPrimitiveIdBlock}},
                                         "distributedPrimitiveId");

    createFenceAndBarrier();
  }
}

// =====================================================================================================================
// Try to cull primitive by running various cullers.
//
// @param vertexIndex0: Relative index of vertex0 (forming this primitive)
// @param vertexIndex1: Relative index of vertex1 (forming this primitive)
// @param vertexIndex2: Relative index of vertex2 (forming this primitive)
Value *NggPrimShader::cullPrimitive(Value *vertexIndex0, Value *vertexIndex1, Value *vertexIndex2) {
  // Skip following culling if it is not requested
  if (!enableCulling())
    return m_builder.getFalse();

  Value *primitiveCulled = m_builder.getFalse();

  Value *vertex0 = fetchVertexPositionData(vertexIndex0);
  Value *vertex1 = fetchVertexPositionData(vertexIndex1);
  Value *vertex2 = fetchVertexPositionData(vertexIndex2);

  // Run backface culler
  if (m_nggControl->enableBackfaceCulling)
    primitiveCulled = runBackfaceCuller(primitiveCulled, vertex0, vertex1, vertex2);

  // Run frustum culler
  if (m_nggControl->enableFrustumCulling)
    primitiveCulled = runFrustumCuller(primitiveCulled, vertex0, vertex1, vertex2);

  // Run box filter culler
  if (m_nggControl->enableBoxFilterCulling)
    primitiveCulled = runBoxFilterCuller(primitiveCulled, vertex0, vertex1, vertex2);

  // Run sphere culler
  if (m_nggControl->enableSphereCulling)
    primitiveCulled = runSphereCuller(primitiveCulled, vertex0, vertex1, vertex2);

  // Run small primitive filter culler
  if (m_nggControl->enableSmallPrimFilter)
    primitiveCulled = runSmallPrimFilterCuller(primitiveCulled, vertex0, vertex1, vertex2);

  // Run cull distance culler
  if (m_nggControl->enableCullDistanceCulling) {
    Value *signMask0 = fetchCullDistanceSignMask(vertexIndex0);
    Value *signMask1 = fetchCullDistanceSignMask(vertexIndex1);
    Value *signMask2 = fetchCullDistanceSignMask(vertexIndex2);
    primitiveCulled = runCullDistanceCuller(primitiveCulled, signMask0, signMask1, signMask2);
  }

  return primitiveCulled;
}

// =====================================================================================================================
// Send the message GS_ALLOC_REQ to SPI indicating how many primitives and vertices in this NGG subgroup.
void NggPrimShader::sendGsAllocReqMessage() {
  // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
  Value *m0 = m_builder.CreateShl(m_nggInputs.primCountInSubgroup, 12);
  m0 = m_builder.CreateOr(m0, m_nggInputs.vertCountInSubgroup);

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder.getInt32(GsAllocReq), m0});
}

// =====================================================================================================================
// Export primitive in passthrough mode.
void NggPrimShader::exportPassthroughPrimitive() {
  assert(m_nggControl->passthroughMode); // Make sure NGG passthrough mode is enabled
  assert(!m_hasGs);                      // Make sure API GS is not present

  auto poison = PoisonValue::get(m_builder.getInt32Ty());
  m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                            {
                                m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                m_builder.getInt32(0x1),             // en
                                // src0 ~ src3
                                m_nggInputs.primData, poison, poison, poison,
                                m_builder.getTrue(),  // done, must be set
                                m_builder.getFalse(), // vm
                            });
}

// =====================================================================================================================
// Export primitive in culling mode without API GS.
//
// @param primitiveCulled : Whether the primitive is culled
void NggPrimShader::exportPrimitive(Value *primitiveCulled) {
  assert(!m_nggControl->passthroughMode); // Make sure NGG passthrough mode is not enabled
  assert(!m_hasGs);                       // Make sure API GS is not present

  //
  // The processing is something like this:
  //
  //   vertexIndices = Relative vertex indices
  //   if (compactVertex)
  //     vertexIndices = Read compacted relative vertex indices from LDS
  //   Export primitive
  //
  Value *vertexIndex0 = m_nggInputs.vertexIndex0;
  Value *vertexIndex1 = m_nggInputs.vertexIndex1;
  Value *vertexIndex2 = m_nggInputs.vertexIndex2;

  auto exportPrimitiveBlock = m_builder.GetInsertBlock();

  if (m_compactVertex) {
    auto compactVertexIndexBlock = createBlock(exportPrimitiveBlock->getParent(), ".compactVertexIndex");
    compactVertexIndexBlock->moveAfter(exportPrimitiveBlock);

    auto endCompactVertexIndexBlock = createBlock(exportPrimitiveBlock->getParent(), ".endCompactVertexIndex");
    endCompactVertexIndexBlock->moveAfter(compactVertexIndexBlock);

    m_builder.CreateCondBr(m_compactVertex, compactVertexIndexBlock, endCompactVertexIndexBlock);

    // Construct ".compactVertexIndex" block
    Value *compactedVertexIndex0 = nullptr;
    Value *compactedVertexIndex1 = nullptr;
    Value *compactedVertexIndex2 = nullptr;
    {
      m_builder.SetInsertPoint(compactVertexIndexBlock);

      const unsigned esGsRingItemSize =
          m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize;

      auto vertexItemOffset0 = m_builder.CreateMul(m_nggInputs.vertexIndex0, m_builder.getInt32(esGsRingItemSize));
      auto vertexItemOffset1 = m_builder.CreateMul(m_nggInputs.vertexIndex1, m_builder.getInt32(esGsRingItemSize));
      auto vertexItemOffset2 = m_builder.CreateMul(m_nggInputs.vertexIndex2, m_builder.getInt32(esGsRingItemSize));

      compactedVertexIndex0 = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset0,
                                                        m_vertCullInfoOffsets.compactedVertexIndex);
      compactedVertexIndex1 = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset1,
                                                        m_vertCullInfoOffsets.compactedVertexIndex);
      compactedVertexIndex2 = readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset2,
                                                        m_vertCullInfoOffsets.compactedVertexIndex);

      m_builder.CreateBr(endCompactVertexIndexBlock);
    }

    // Construct ".endCompactVertexIndex" block
    {
      m_builder.SetInsertPoint(endCompactVertexIndexBlock);

      vertexIndex0 =
          createPhi({{compactedVertexIndex0, compactVertexIndexBlock}, {vertexIndex0, exportPrimitiveBlock}});
      vertexIndex1 =
          createPhi({{compactedVertexIndex1, compactVertexIndexBlock}, {vertexIndex1, exportPrimitiveBlock}});
      vertexIndex2 =
          createPhi({{compactedVertexIndex2, compactVertexIndexBlock}, {vertexIndex2, exportPrimitiveBlock}});
    }
  }

  // Primitive connectivity data have such layout:
  //
#if LLPC_BUILD_GFX12
  // pre-GFX12:
#endif
  //   +----------------+---------------+---------------+---------------+
  //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
  //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
  //   +----------------+---------------+---------------+---------------+
#if LLPC_BUILD_GFX12
  //
  // GFX12 (from GE):
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //   | GS Instance ID | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
  //   | [31:27]        | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //
  // GFX12 (to PA):
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //   | Null Primitive | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
  //   | [31]           | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
#endif
  Value *primData = nullptr;
  if (m_gfxIp.major <= 11) {
    primData = m_builder.CreateShl(vertexIndex2, 10);
    primData = m_builder.CreateOr(primData, vertexIndex1);

    primData = m_builder.CreateShl(primData, 10);
    primData = m_builder.CreateOr(primData, vertexIndex0);
  } else {
#if LLPC_BUILD_GFX12
    if (m_compactVertex) {
      primData = m_builder.CreateShl(vertexIndex2, 9);
      primData = m_builder.CreateOr(primData, vertexIndex1);

      primData = m_builder.CreateShl(primData, 9);
      primData = m_builder.CreateOr(primData, vertexIndex0);
    } else {
      // NOTE: If vertex compaction is disabled, we can use the recorded primitive connectivity data straightforwardly
      // (sent by GE) without reconstructing it from relative vertex indices.
      primData = m_nggInputs.primData;
    }
#else
    llvm_unreachable("Not implemented!");
#endif
  }

  if (primitiveCulled)
    primData = m_builder.CreateSelect(primitiveCulled, m_builder.getInt32(NullPrim), primData);

  auto poison = PoisonValue::get(m_builder.getInt32Ty());
  m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                            {
                                m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                m_builder.getInt32(0x1),             // en
                                // src0 ~ src3
                                primData, poison, poison, poison,
                                m_builder.getTrue(),  // done, must be set
                                m_builder.getFalse(), // vm
                            });
}

// =====================================================================================================================
// Export primitive when API GS is present.
//
// @param startingVertexIndex : The relative index of starting vertex (Indices of vertices forming a GS primitive must
//                              be consecutive)
void NggPrimShader::exportPrimitiveWithGs(Value *startingVertexIndex) {
  assert(m_hasGs); // Make sure API GS is present

  //
  // The processing is something like this:
  //
  //   primData = Read primitive data from LDS
  //   if (valid primitive) {
  //     if (points)
  //       primData = vertexIndex0
  //     else if (line_strip) {
  //       primData = <vertexIndex0, vertexIndex1>
  //     } else if (triangle_strip) {
  //       winding = primData != 0
  //       if (winding == 0)
  //         primData = <vertexIndex0, vertexIndex1, vertexIndex2>
  //       else {
  //         if (provokingVertexMode == ProvokingVerexFirst)
  //           primData = <vertexIndex0, vertexIndex2, vertexIndex1>
  //         else
  //           primData = <vertexIndex1, vertexIndex0, vertexIndex2>
  //       }
  //     }
  //   }
  //   Export primitive
  //
  const auto rasterStream = m_pipelineState->getRasterizerState().rasterStream;
  assert(rasterStream != InvalidValue);
  Value *primData =
      readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                               PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * rasterStream);
  auto validPrimitive = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));

  // Primitive connectivity data have such layout:
  //
#if LLPC_BUILD_GFX12
  // pre-GFX12:
#endif
  //   +----------------+---------------+---------------+---------------+
  //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
  //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
  //   +----------------+---------------+---------------+---------------+
#if LLPC_BUILD_GFX12
  //
  // GFX12 (from GE):
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //   | GS Instance ID | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
  //   | [31:27]        | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //
  // GFX12 (to PA):
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
  //   | Null Primitive | Edge Flag2 | Vertex Index2 | Edge Flag1 | Vertex Index1 | Edge Flag0 | Vertex Index0 |
  //   | [31]           | [26]       | [25:18]       | [17]       | [16:9]        | [8]        | [7:0]         |
  //   +----------------+------------+---------------+------------+---------------+------------+---------------+
#endif
  Value *newPrimData = nullptr;
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

  switch (geometryMode.outputPrimitive) {
  case OutputPrimitives::Points:
    newPrimData = startingVertexIndex;
    break;
  case OutputPrimitives::LineStrip: {
    Value *vertexIndex0 = startingVertexIndex;
    Value *vertexIndex1 = m_builder.CreateAdd(startingVertexIndex, m_builder.getInt32(1));
    if (m_gfxIp.major <= 11)
      newPrimData = m_builder.CreateOr(m_builder.CreateShl(vertexIndex1, 10), vertexIndex0);
    else
#if LLPC_BUILD_GFX12
      newPrimData = m_builder.CreateOr(m_builder.CreateShl(vertexIndex1, 9), vertexIndex0);
#else
      llvm_unreachable("Not implemented!");
#endif
    break;
  }
  case OutputPrimitives::TriangleStrip: {
    // NOTE: primData[N] corresponds to the forming vertex
    // The vertice indices in the first triangle <N, N+1, N+2>
    // If provoking vertex is the first one, the vertice indices in the second triangle is <N, N+2, N+1>, otherwise it
    // is <N+1, N, N+2>.
    unsigned windingIndices[3] = {};
    if (m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst) {
      windingIndices[0] = 0;
      windingIndices[1] = 2;
      windingIndices[2] = 1;
    } else {
      windingIndices[0] = 1;
      windingIndices[1] = 0;
      windingIndices[2] = 2;
    }
    Value *winding = m_builder.CreateICmpNE(primData, m_builder.getInt32(0));
    auto vertexIndex0 =
        m_builder.CreateAdd(startingVertexIndex, m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[0]),
                                                                        m_builder.getInt32(0)));
    auto vertexIndex1 =
        m_builder.CreateAdd(startingVertexIndex, m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[1]),
                                                                        m_builder.getInt32(1)));
    auto vertexIndex2 =
        m_builder.CreateAdd(startingVertexIndex, m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[2]),
                                                                        m_builder.getInt32(2)));

    if (m_gfxIp.major <= 11) {
      newPrimData = m_builder.CreateOr(
          m_builder.CreateShl(m_builder.CreateOr(m_builder.CreateShl(vertexIndex2, 10), vertexIndex1), 10),
          vertexIndex0);
    } else {
#if LLPC_BUILD_GFX12
      newPrimData = m_builder.CreateOr(
          m_builder.CreateShl(m_builder.CreateOr(m_builder.CreateShl(vertexIndex2, 9), vertexIndex1), 9), vertexIndex0);
#else
      llvm_unreachable("Not implemented!");
#endif
    }
    break;
  }
  default:
    llvm_unreachable("Unexpected output primitive type!");
    break;
  }

  primData = m_builder.CreateSelect(validPrimitive, newPrimData, primData);

  auto poison = PoisonValue::get(m_builder.getInt32Ty());
  m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                            {
                                m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                m_builder.getInt32(0x1),             // en
                                primData, poison, poison, poison,    // src0 ~ src3
                                m_builder.getTrue(),                 // done, must be set
                                m_builder.getFalse(),                // vm
                            });
}

// =====================================================================================================================
// Early exit primitive shader when we detect that the entire subgroup is fully culled, doing dummy
// primitive/vertex export if necessary.
void NggPrimShader::earlyExitWithDummyExport() {
  auto earlyExitBlock = m_builder.GetInsertBlock();

  auto dummyExportBlock = createBlock(earlyExitBlock->getParent(), ".dummyExport");
  dummyExportBlock->moveAfter(earlyExitBlock);

  auto endDummyExportBlock = createBlock(earlyExitBlock->getParent(), ".endDummyExport");
  endDummyExportBlock->moveAfter(dummyExportBlock);

  // Construct ".earlyExit" block
  {
    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, dummyExportBlock, endDummyExportBlock);
  }

  // Construct ".dummyExport" block
  {
    m_builder.SetInsertPoint(dummyExportBlock);

    auto poison = PoisonValue::get(m_builder.getInt32Ty());
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getInt32Ty(),
                              {
                                  m_builder.getInt32(EXP_TARGET_PRIM), // tgt
                                  m_builder.getInt32(0x1),             // en
                                  // src0 ~ src3
                                  m_builder.getInt32(0), poison, poison, poison,
                                  m_builder.getTrue(), // done
                                  m_builder.getFalse() // vm
                              });

    // Determine how many dummy position exports we need
    unsigned posExpCount = 1;
    if (m_hasGs) {
      const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->builtInUsage.gs;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      miscExport |= builtInUsage.primitiveShadingRate;
      if (miscExport)
        ++posExpCount;

      posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
    } else if (m_hasTes) {
      const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->builtInUsage.tes;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      if (miscExport)
        ++posExpCount;

      posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
    } else {
      const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->builtInUsage.vs;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      miscExport |= builtInUsage.primitiveShadingRate;
      if (miscExport)
        ++posExpCount;

      posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
    }

    poison = PoisonValue::get(m_builder.getFloatTy());
    for (unsigned i = 0; i < posExpCount; ++i) {
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                {
                                    m_builder.getInt32(EXP_TARGET_POS_0 + i), // tgt
                                    m_builder.getInt32(0x0),                  // en
                                    // src0 ~ src3
                                    poison, poison, poison, poison,
                                    m_builder.getInt1(i == posExpCount - 1), // done
                                    m_builder.getFalse()                     // vm
                                });
    }

    m_builder.CreateBr(endDummyExportBlock);
  }

  // Construct ".endDummyExport" block
  {
    m_builder.SetInsertPoint(endDummyExportBlock);
    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Runs ES.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::runEs(ArrayRef<Argument *> args) {
  if (!m_esHandlers.main)
    return; // No ES, don't run

  assert(m_hasGs); // GS must be present, ES is run as part of ES-GS merged shader

  auto &hwConfig = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig;
  unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  auto esGsOffset =
      m_builder.CreateMul(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(waveSize * hwConfig.esGsRingItemSize));

  Value *offChipLdsBase = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase)];
  offChipLdsBase->setName("offChipLdsBase");

  Value *userData = args[NumSpecialSgprInputs];

  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  Value *tessCoordX = nullptr;
  Value *tessCoordY = nullptr;
  Value *relPatchId = nullptr;
  Value *patchId = nullptr;

  Value *vertexId = nullptr;
  Value *relVertexId = PoisonValue::get(m_builder.getInt32Ty()); // Unused
  // NOTE: VS primitive ID for NGG is specially obtained from primitive ID distribution.
  Value *vsPrimitiveId = m_distributedPrimitiveId ? m_distributedPrimitiveId : PoisonValue::get(m_builder.getInt32Ty());
  Value *instanceId = nullptr;

  if (m_gfxIp.major <= 11) {
    if (m_hasTes) {
      tessCoordX = vgprArgs[5];
      tessCoordY = vgprArgs[6];
      relPatchId = vgprArgs[7];
      patchId = vgprArgs[8];
    } else {
      vertexId = vgprArgs[5];
      instanceId = vgprArgs[8];
    }
  } else {
#if LLPC_BUILD_GFX12
    if (m_hasTes) {
      tessCoordX = vgprArgs[3];
      tessCoordY = vgprArgs[4];
      relPatchId = vgprArgs[5];
      patchId = vgprArgs[6];
    } else {
      vertexId = vgprArgs[3];
      instanceId = vgprArgs[4];
    }
#else
    llvm_unreachable("Not implemented!");
#endif
  }

  SmallVector<Value *, 32> esArgs;

  // Set up user data SGPRs
  const unsigned userDataCount =
      m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex)->userDataCount;
  appendUserData(esArgs, m_esHandlers.main, userData, userDataCount);

  if (m_hasTes) {
    // Set up system value SGPRs
    esArgs.push_back(offChipLdsBase);
    esArgs.push_back(esGsOffset);

    // Set up system value VGPRs
    esArgs.push_back(tessCoordX);
    esArgs.push_back(tessCoordY);
    esArgs.push_back(relPatchId);
    esArgs.push_back(patchId);
  } else {
    // Set up system value SGPRs
    esArgs.push_back(esGsOffset);

    // Set up system value VGPRs
    esArgs.push_back(vertexId);
    esArgs.push_back(relVertexId);
    esArgs.push_back(vsPrimitiveId);
    esArgs.push_back(instanceId);
  }

  assert(esArgs.size() == m_esHandlers.main->arg_size()); // Must have visit all arguments of ES entry point

  CallInst *esCall = callFunctionHelper(m_esHandlers.main, esArgs, m_builder.GetInsertBlock());
  esCall->setCallingConv(CallingConv::AMDGPU_ES);
}

// =====================================================================================================================
// Split ES to two parts. The first part only contains cull data exports, such as position and cull distance (if cull
// distance culling is enabled). The second part contains other remaining exports that are not in the first part.
//
// NOTE: After this splitting, original ES is removed and couldn't be used any more.
void NggPrimShader::splitEs() {
  assert(m_hasGs == false); // GS must not be present

  SmallVector<CallInst *, 8> callsToRemove;

  //
  // Create ES first part (only contains cull data exports)
  //

  // Clone ES
  auto esFirstPartTy = FunctionType::get(m_builder.getVoidTy(), m_esHandlers.main->getFunctionType()->params(), false);
  auto esFirstPart =
      Function::Create(esFirstPartTy, GlobalVariable::InternalLinkage, "", m_esHandlers.main->getParent());

  ValueToValueMapTy valueMap;

  Argument *newArg = esFirstPart->arg_begin();
  for (Argument &arg : m_esHandlers.main->args())
    valueMap[&arg] = newArg++;

  SmallVector<ReturnInst *, 8> retInsts;
  CloneFunctionInto(esFirstPart, m_esHandlers.main, valueMap, CloneFunctionChangeType::LocalChangesOnly, retInsts);

  esFirstPart->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  esFirstPart->setCallingConv(CallingConv::C);
  esFirstPart->addFnAttr(Attribute::AlwaysInline);

  esFirstPart->setName(NggEsFirstPart);

  // Mutate ES first part by only keeping cull data exports
  unsigned clipCullExportSlot = 1;
  unsigned clipDistanceCount = 0;
  unsigned cullDistanceCount = 0;

  if (m_nggControl->enableCullDistanceCulling) {
    const auto &resUsage =
        m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex);

    if (m_hasTes) {
      const auto &builtInUsage = resUsage->builtInUsage.tes;

      const bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      clipCullExportSlot = miscExport ? 2 : 1;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    } else {
      const auto &builtInUsage = resUsage->builtInUsage.vs;

      const bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex ||
                              builtInUsage.primitiveShadingRate;
      clipCullExportSlot = miscExport ? 2 : 1;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    }

    assert(cullDistanceCount > 0); // Cull distance must exist if the culling is enabled
  }

  {
    struct Payload {
      NggPrimShader &self;
      const unsigned clipCullExportSlot;
      const unsigned clipDistanceCount;
      const unsigned cullDistanceCount;
      SmallVectorImpl<CallInst *> &callsToRemove;
    };
    Payload payload = {*this, clipCullExportSlot, clipDistanceCount, cullDistanceCount, callsToRemove};

    static const auto visitor =
        llvm_dialects::VisitorBuilder<Payload>()
            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
            .add<NggExportPositionOp>([](Payload &payload, NggExportPositionOp &exportPositionOp) {
              bool keepExport = false;
              auto exportSlot = exportPositionOp.getExportSlot();
              if (exportSlot == 0) {
                keepExport = true; // Position0
              } else {
                if (payload.self.m_nggControl->enableCullDistanceCulling) {
                  if (exportSlot == payload.clipCullExportSlot ||
                      (exportSlot == payload.clipCullExportSlot + 1 &&
                       payload.clipDistanceCount + payload.cullDistanceCount > 4)) {
                    keepExport = true; // CullDistance
                  }
                }
              }

              if (!keepExport)
                payload.callsToRemove.push_back(&exportPositionOp);
            })
            .add<NggExportAttributeOp>([](Payload &payload, NggExportAttributeOp &exportAttributeOp) {
              payload.callsToRemove.push_back(&exportAttributeOp);
            })
            .add<WriteXfbOutputOp>([](Payload &payload, WriteXfbOutputOp &writeXfbOutputOp) {
              payload.callsToRemove.push_back(&writeXfbOutputOp);
            })
            .build();
    visitor.visit(payload, *esFirstPart);
  }

  //
  // Create ES second part (contains other remaining exports)
  //
  auto esSecondPart = m_esHandlers.main;

  esSecondPart->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  esSecondPart->setLinkage(GlobalValue::InternalLinkage);
  esSecondPart->setCallingConv(CallingConv::C);
  esSecondPart->addFnAttr(Attribute::AlwaysInline);

  esSecondPart->setName(NggEsSecondPart);

  // Mutate ES second part by keep other remaining exports
  {
    struct Payload {
      SmallVectorImpl<CallInst *> &callsToRemove;
    };
    Payload payload = {callsToRemove};

    static const auto visitor =
        llvm_dialects::VisitorBuilder<Payload>()
            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
            .add<NggExportPositionOp>([](Payload &payload, NggExportPositionOp &exportPositionOp) {
              if (exportPositionOp.getExportSlot() == 0)
                payload.callsToRemove.push_back(&exportPositionOp); // Position0
            })
            .add<WriteXfbOutputOp>([](Payload &payload, WriteXfbOutputOp &writeXfbOutputOp) {
              payload.callsToRemove.push_back(&writeXfbOutputOp);
            })
            .build();
    visitor.visit(payload, *esSecondPart);
  }

  // Remove original ES main function
  m_esHandlers.main = nullptr;
  m_esHandlers.part = std::make_pair(esFirstPart, esSecondPart);

  for (auto call : callsToRemove) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Runs GS.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::runGs(ArrayRef<Argument *> args) {
  assert(m_gsHandlers.main);
  assert(m_hasGs); // GS must be present

  mutateGs();

  Value *gsVsOffset = PoisonValue::get(m_builder.getInt32Ty()); // Unused

  // NOTE: This argument is expected to be GS wave ID, not wave ID in subgroup, for normal ES-GS merged shader.
  // However, in NGG mode, GS wave ID, sent to GS_EMIT and GS_CUT messages, is no longer required because of NGG
  // handling of such messages. Instead, wave ID in subgroup is required as the substitute.
  auto waveId = m_nggInputs.waveIdInSubgroup;

  Value *userData = args[NumSpecialSgprInputs];

  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  Value *esGsOffset0 = nullptr;
  Value *esGsOffset1 = nullptr;
  Value *esGsOffset2 = nullptr;
  Value *esGsOffset3 = nullptr;
  Value *esGsOffset4 = nullptr;
  Value *esGsOffset5 = nullptr;

  Value *primitiveId = nullptr;
  Value *invocationId = nullptr;

  if (m_gfxIp.major <= 11) {
    esGsOffset0 = createUBfe(vgprArgs[0], 0, 16);
    esGsOffset1 = createUBfe(vgprArgs[0], 16, 16);
    esGsOffset2 = createUBfe(vgprArgs[1], 0, 16);
    esGsOffset3 = createUBfe(vgprArgs[1], 16, 16);
    esGsOffset4 = createUBfe(vgprArgs[4], 0, 16);
    esGsOffset5 = createUBfe(vgprArgs[4], 16, 16);

    primitiveId = vgprArgs[2];
    // NOTE: For NGG, GS invocation ID is stored in lowest 8 bits ([7:0]) and other higher bits are used for other
    // purposes according to GE-SPI interface.
    invocationId = m_builder.CreateAnd(vgprArgs[3], m_builder.getInt32(0xFF));
  } else {
#if LLPC_BUILD_GFX12
    const auto esGsRingItemSize = m_builder.getInt32(
        m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize);

    esGsOffset0 = m_builder.CreateMul(createUBfe(vgprArgs[0], 0, 8), esGsRingItemSize);
    esGsOffset1 = m_builder.CreateMul(createUBfe(vgprArgs[0], 9, 8), esGsRingItemSize);
    esGsOffset2 = m_builder.CreateMul(createUBfe(vgprArgs[0], 18, 8), esGsRingItemSize);
    esGsOffset3 = m_builder.CreateMul(createUBfe(vgprArgs[2], 0, 8), esGsRingItemSize);
    esGsOffset4 = m_builder.CreateMul(createUBfe(vgprArgs[2], 9, 8), esGsRingItemSize);
    esGsOffset5 = m_builder.CreateMul(createUBfe(vgprArgs[2], 18, 8), esGsRingItemSize);

    primitiveId = vgprArgs[1];
    // NOTE: For GFX12, GS invocation ID is stored in highest 5 bits ([31:27])
    invocationId = createUBfe(vgprArgs[0], 27, 5);
#else
    llvm_unreachable("Not implemented!");
#endif
  }

  SmallVector<Value *, 32> gsArgs;

  // Set up user data SGPRs
  const unsigned userDataCount = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->userDataCount;
  appendUserData(gsArgs, m_gsHandlers.main, userData, userDataCount);

  // Set up system value SGPRs
  gsArgs.push_back(gsVsOffset);
  gsArgs.push_back(waveId);

  // Set up system value VGPRs
  gsArgs.push_back(esGsOffset0);
  gsArgs.push_back(esGsOffset1);
  gsArgs.push_back(primitiveId);
  gsArgs.push_back(esGsOffset2);
  gsArgs.push_back(esGsOffset3);
  gsArgs.push_back(esGsOffset4);
  gsArgs.push_back(esGsOffset5);
  gsArgs.push_back(invocationId);

  assert(gsArgs.size() == m_gsHandlers.main->arg_size()); // Must have visit all arguments of ES entry point

  CallInst *gsCall = m_builder.CreateCall(m_gsHandlers.main, gsArgs);
  gsCall->setCallingConv(CallingConv::AMDGPU_GS);
}

// =====================================================================================================================
// Mutates GS to handle writing GS outputs to GS-VS ring, and the messages GS_EMIT/GS_CUT.
void NggPrimShader::mutateGs() {
  assert(m_hasGs); // GS must be present

  IRBuilder<>::InsertPointGuard guard(m_builder);

  SmallVector<CallInst *, 32> callsToRemove;

  m_builder.SetInsertPointPastAllocas(m_gsHandlers.main);

  // Initialize counters of GS emitted vertices and GS output vertices of current primitive
  Value *emitVertsPtrs[MaxGsStreams] = {};
  Value *outVertsPtrs[MaxGsStreams] = {};
  Value *totalEmitVertsPtr = nullptr;

  {
    IRBuilder<>::InsertPointGuard allocaGuard(m_builder);
    m_builder.SetInsertPointPastAllocas(m_gsHandlers.main);

    for (int i = 0; i < MaxGsStreams; ++i) {
      auto emitVertsPtr = m_builder.CreateAlloca(m_builder.getInt32Ty());
      m_builder.CreateStore(m_builder.getInt32(0), emitVertsPtr); // emitVerts = 0
      emitVertsPtrs[i] = emitVertsPtr;

      auto outVertsPtr = m_builder.CreateAlloca(m_builder.getInt32Ty());
      m_builder.CreateStore(m_builder.getInt32(0), outVertsPtr); // outVerts = 0
      outVertsPtrs[i] = outVertsPtr;
    }

    totalEmitVertsPtr = m_builder.CreateAlloca(m_builder.getInt32Ty());
    m_builder.CreateStore(m_builder.getInt32(0), totalEmitVertsPtr); // emitTotalVerts = 0
  }

  // Initialize thread ID in wave
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  assert(waveSize == 32 || waveSize == 64);

  auto threadIdInWave =
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), threadIdInWave});
  }

  // Initialize thread ID in subgroup
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Geometry)->entryArgIdxs.gs;
  auto waveId = getFunctionArgument(m_gsHandlers.main, entryArgIdxs.gsWaveId);

  auto threadIdInSubgroup = m_builder.CreateMul(waveId, m_builder.getInt32(waveSize));
  threadIdInSubgroup = m_builder.CreateAdd(threadIdInSubgroup, threadIdInWave);

  // Handle dialect op NggWriteGsOutputOp and GS message
  struct Payload {
    NggPrimShader &self;
    const ArrayRef<Value *> emitVertsPtrs;
    const ArrayRef<Value *> outVertsPtrs;
    Value *totalEmitVertsPtr;
    Value *threadIdInSubgroup;
    SmallVectorImpl<CallInst *> &callsToRemove;
  };
  Payload payload = {*this, emitVertsPtrs, outVertsPtrs, totalEmitVertsPtr, threadIdInSubgroup, callsToRemove};

  static const auto visitor =
      llvm_dialects::VisitorBuilder<Payload>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<NggWriteGsOutputOp>([](Payload &payload, NggWriteGsOutputOp &writeGsOutputOp) {
            auto &builder = payload.self.m_builder;
            builder.SetInsertPoint(&writeGsOutputOp);

            const unsigned streamId = writeGsOutputOp.getStreamId();
            assert(streamId < MaxGsStreams);

            auto emitVerts = builder.CreateLoad(builder.getInt32Ty(), payload.emitVertsPtrs[streamId]);
            auto totalEmitVerts = builder.CreateLoad(builder.getInt32Ty(), payload.totalEmitVertsPtr);
            payload.self.writeGsOutput(writeGsOutputOp.getOutputValue(), writeGsOutputOp.getLocation(),
                                       writeGsOutputOp.getComponent(), streamId, payload.threadIdInSubgroup, emitVerts,
                                       totalEmitVerts);

            payload.callsToRemove.push_back(&writeGsOutputOp);
          })
          .add<GsEmitStreamOp>([](Payload &payload, GsEmitStreamOp &gsEmitStreamOp) {
            auto &builder = payload.self.m_builder;
            builder.SetInsertPoint(&gsEmitStreamOp);

            const unsigned streamId = gsEmitStreamOp.getStreamId();
            assert(streamId < MaxGsStreams);

            payload.self.processGsEmit(streamId, payload.threadIdInSubgroup, payload.emitVertsPtrs[streamId],
                                       payload.outVertsPtrs[streamId], payload.totalEmitVertsPtr);

            payload.callsToRemove.push_back(&gsEmitStreamOp);
          })
          .add<GsCutStreamOp>([](Payload &payload, GsCutStreamOp &gsCutStreamOp) {
            auto &builder = payload.self.m_builder;
            builder.SetInsertPoint(&gsCutStreamOp);

            const unsigned streamId = gsCutStreamOp.getStreamId();
            assert(streamId < MaxGsStreams);

            payload.self.processGsCut(streamId, payload.outVertsPtrs[streamId]);

            payload.callsToRemove.push_back(&gsCutStreamOp);
          })
          .build();
  visitor.visit(payload, *m_gsHandlers.main);

  for (auto call : callsToRemove) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Mutates copy shader to handle the reading GS outputs from GS-VS ring and remove already-handled XFB exports.
void NggPrimShader::mutateCopyShader() {
  assert(m_hasGs); // GS must be present

  IRBuilder<>::InsertPointGuard guard(m_builder);

  assert(m_gsHandlers.copyShader->arg_size() == 1); // Only one argument
  auto vertexIndex = getFunctionArgument(m_gsHandlers.copyShader, 0);

  const unsigned rasterStream = m_pipelineState->getRasterizerState().rasterStream;
  assert(rasterStream != InvalidValue);

  SmallVector<CallInst *, 32> callsToRemove;

  struct Payload {
    NggPrimShader &self;
    Value *vertexIndex;
    const unsigned rasterStream;
    SmallVectorImpl<CallInst *> &callsToRemove;
  };
  Payload payload = {*this, vertexIndex, rasterStream, callsToRemove};

  static const auto visitor =
      llvm_dialects::VisitorBuilder<Payload>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<NggReadGsOutputOp>([](Payload &payload, NggReadGsOutputOp &readGsOutputOp) {
            auto &builder = payload.self.m_builder;
            builder.SetInsertPoint(&readGsOutputOp);

            const unsigned streamId = readGsOutputOp.getStreamId();
            assert(streamId < MaxGsStreams);

            // Only lower the dialect op if it belongs to the rasterization stream.
            if (streamId == payload.rasterStream) {
              auto vertexOffset = payload.self.calcVertexItemOffset(streamId, payload.vertexIndex);
              auto outputValue = payload.self.readGsOutput(readGsOutputOp.getType(), readGsOutputOp.getLocation(),
                                                           readGsOutputOp.getComponent(), streamId, vertexOffset);
              readGsOutputOp.replaceAllUsesWith(outputValue);
            }

            payload.callsToRemove.push_back(&readGsOutputOp);
          })
          .add<WriteXfbOutputOp>([](Payload &payload, WriteXfbOutputOp &writeXfbOutputOp) {
            payload.callsToRemove.push_back(&writeXfbOutputOp);
          })
          .build();
  visitor.visit(payload, *m_gsHandlers.copyShader);

  for (auto call : callsToRemove) {
    assert(call->user_empty());
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Append user data arguments to the argument list for the target caller. Those arguments will be consumed by the
// target callee later.
//
// @param [in/out] args : The arguments that will be appended to
// @param target : The function we are preparing to call later
// @param userData : The <N x i32> vector of user data values
// @param userDataCount : The number of elements of user data that should be processed
void NggPrimShader::appendUserData(SmallVectorImpl<Value *> &args, Function *target, Value *userData,
                                   unsigned userDataCount) {
  unsigned userDataIdx = 0;

  auto argBegin = target->arg_begin();
  const unsigned argCount = target->arg_size();
  (void(argCount)); // Unused

  // Set up user data SGPRs
  while (userDataIdx < userDataCount) {
    assert(args.size() < argCount);

    auto arg = (argBegin + args.size());
    assert(arg->hasAttribute(Attribute::InReg));

    auto argTy = arg->getType();
    if (argTy->isVectorTy()) {
      assert(cast<VectorType>(argTy)->getElementType()->isIntegerTy());

      const unsigned userDataSize = cast<FixedVectorType>(argTy)->getNumElements();

      std::vector<int> shuffleMask;
      for (unsigned i = 0; i < userDataSize; ++i)
        shuffleMask.push_back(userDataIdx + i);

      userDataIdx += userDataSize;

      auto newUserData = m_builder.CreateShuffleVector(userData, userData, shuffleMask);
      args.push_back(newUserData);
    } else {
      assert(argTy->isIntegerTy());

      auto newUserData = m_builder.CreateExtractElement(userData, userDataIdx);
      args.push_back(newUserData);
      ++userDataIdx;
    }
  }
}

// =====================================================================================================================
// Write GS outputs to GS-VS ring.
//
// NOTE: The GS-VS ring layout in NGG mode is very different from that of non-NGG. We purposely group output vertices
// according to their belonging vertex streams in that copy shader doesn't exist actually and we take full control of
// GS-VS ring. The ring does not have to conform to hardware design requirements any more. This layout is to facilitate
// vertex offset calculation when we do vertex exporting and could improve NGG throughput by avoiding
// input-primitive-based loop.
//
// The layout is something like this (shader takes over it):
//
//   +----------+----+----------+----+----------+----+----------+
//   | Vertex 0 | .. | Vertex N | .. | Vertex 0 | .. | Vertex N | (N = max_vertices)
//   +----------+----+----------+----+----------+----+----------+
//   |<------ Primitive 0 ----->| .. |<------ Primitive M ----->| (M = prims_per_subgroup)
//   |<----------------------- Stream i ----------------------->|
//
//   +----------+----------+----------+----------+
//   | Stream 0 | Stream 1 | Stream 2 | Stream 3 |
//   +----------+----------+----------+----------+
//   |<--------------- GS-VS ring -------------->|
//
// By contrast, GS-VS ring layout of non-NGG is something like this (conform to hardware design):
//
//   +----------+----+----------+----+----------+----+----------+
//   | Vertex 0 | .. | Vertex N | .. | Vertex 0 | .. | Vertex N | (N = max_vertices)
//   +----------+----+----------+----+----------+----+----------+
//   |<-------- Stream 0 ------>| .. |<-------- Stream 3 ------>|
//   |<---------------------- Primitive i --------------------->|
//
//   +-------------+----+-------------+
//   | Primitive 0 | .. | Primitive M | (M = prims_per_subgroup)
//   +-------------+----+-------------+
//   |<--------- GS-VS ring --------->|
//
// @param output : Output value
// @param location : Location of the output
// @param component : Component index used for vector element addressing
// @param streamId : ID of output vertex stream
// @param primitiveIndex : Relative primitive index in subgroup
// @param emitVerts : Counter of GS emitted vertices for this stream
// @param totalEmitVerts : Counter of GS emitted vertices for all streams
void NggPrimShader::writeGsOutput(Value *output, unsigned location, unsigned component, unsigned streamId,
                                  Value *primitiveIndex, Value *emitVerts, llvm::Value *totalEmitVerts) {
  assert(streamId < MaxGsStreams);
  if (!m_pipelineState->enableSwXfb() && m_pipelineState->getRasterizerState().rasterStream != streamId) {
    // NOTE: If SW-emulated stream-out is not enabled, only import those outputs that belong to the rasterization
    // stream.
    return;
  }

  // NOTE: We only handle LDS vector/scalar writing, so change [n x Ty] to <n x Ty> for array.
  auto outputTy = output->getType();
  if (outputTy->isArrayTy()) {
    auto outputElemTy = outputTy->getArrayElementType();
    assert(outputElemTy->isSingleValueType());

    // [n x Ty] -> <n x Ty>
    const unsigned elemCount = outputTy->getArrayNumElements();
    Value *outputVec = PoisonValue::get(FixedVectorType::get(outputElemTy, elemCount));
    for (unsigned i = 0; i < elemCount; ++i) {
      auto outputElem = m_builder.CreateExtractValue(output, i);
      outputVec = m_builder.CreateInsertElement(outputVec, outputElem, i);
    }

    outputTy = outputVec->getType();
    output = outputVec;
  }

  const unsigned bitWidth = output->getType()->getScalarSizeInBits();
  if (bitWidth == 8 || bitWidth == 16) {
    // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend byte/word
    // to dword. This is because copy shader does not know the actual data type. It only generates output
    // export calls based on number of dwords.
    if (outputTy->isFPOrFPVectorTy()) {
      assert(bitWidth == 16);
      Type *castTy = m_builder.getInt16Ty();
      if (outputTy->isVectorTy())
        castTy = FixedVectorType::get(m_builder.getInt16Ty(), cast<FixedVectorType>(outputTy)->getNumElements());
      output = m_builder.CreateBitCast(output, castTy);
    }

    Type *extTy = m_builder.getInt32Ty();
    if (outputTy->isVectorTy())
      extTy = FixedVectorType::get(m_builder.getInt32Ty(), cast<FixedVectorType>(outputTy)->getNumElements());
    output = m_builder.CreateZExt(output, extTy);
  } else
    assert(bitWidth == 32 || bitWidth == 64);

  // vertexIndex = primitiveIndex * outputVertices + emitVerts
  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  auto vertexIndex = m_builder.CreateMul(primitiveIndex, m_builder.getInt32(geometryMode.outputVertices));
  vertexIndex = m_builder.CreateAdd(vertexIndex, emitVerts);

  // ldsOffset = vertexOffset + location * 4 + component (in dwords)
  auto vertexOffset = calcVertexItemOffset(streamId, vertexIndex);
  const unsigned attribOffset = (location * 4) + component;
  auto ldsOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(attribOffset));

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Skip GS-VS ring write if the emit is invalid
  if (geometryMode.robustGsEmits) {
    // validEmit = totalEmitVerts < outputVertices
    auto validEmit = m_builder.CreateICmpULT(totalEmitVerts, m_builder.getInt32(geometryMode.outputVertices));
    m_builder.CreateIf(validEmit, false);
  }

  writeValueToLds(output, ldsOffset);
}

// =====================================================================================================================
// Read GS outputs from GS-VS ring.
//
// @param outputTy : Type of the output
// @param location : Location of the output
// @param component : Component index used for vector element addressing
// @param streamId : ID of output vertex stream
// @param vertexOffset : Start offset of vertex item in GS-VS ring (in dwords)
Value *NggPrimShader::readGsOutput(Type *outputTy, unsigned location, unsigned component, unsigned streamId,
                                   Value *vertexOffset) {
  assert(streamId < MaxGsStreams);
  if (!m_pipelineState->enableSwXfb() && m_pipelineState->getRasterizerState().rasterStream != streamId) {
    // NOTE: If SW-emulated stream-out is not enabled, only import those outputs that belong to the rasterization
    // stream.
    return PoisonValue::get(outputTy);
  }

  // NOTE: We only handle LDS vector/scalar reading, so change [n x Ty] to <n x Ty> for array.
  auto origOutputTy = outputTy;
  if (outputTy->isArrayTy()) {
    auto outputElemTy = outputTy->getArrayElementType();
    assert(outputElemTy->isSingleValueType());

    // [n x Ty] -> <n x Ty>
    const unsigned elemCount = outputTy->getArrayNumElements();
    outputTy = FixedVectorType::get(outputElemTy, elemCount);
  }

  // ldsOffset = vertexOffset + location * 4 + component (in dwords)
  const unsigned attribOffset = location * 4 + component;
  auto ldsOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(attribOffset));

  auto output = readValueFromLds(outputTy, ldsOffset);

  if (origOutputTy != outputTy) {
    assert(origOutputTy->isArrayTy() && outputTy->isVectorTy() &&
           origOutputTy->getArrayNumElements() == cast<FixedVectorType>(outputTy)->getNumElements());

    // <n x Ty> -> [n x Ty]
    const unsigned elemCount = origOutputTy->getArrayNumElements();
    Value *outputArray = PoisonValue::get(origOutputTy);
    for (unsigned i = 0; i < elemCount; ++i) {
      auto outputElem = m_builder.CreateExtractElement(output, i);
      outputArray = m_builder.CreateInsertValue(outputArray, outputElem, i);
    }

    output = outputArray;
  }

  return output;
}

// =====================================================================================================================
// Process the dialect op NggGsEmit.
//
// @param streamId : ID of output vertex stream
// @param primitiveIndex : Relative primitive index in subgroup
// @param [in/out] emitVertsPtr : Pointer to the counter of GS emitted vertices for this stream
// @param [in/out] outVertsPtr : Pointer to the counter of GS output vertices of current primitive for this stream
// @param [in/out] totalEmitVertsPtr : Pointer to the counter of GS emitted vertices for all stream
void NggPrimShader::processGsEmit(unsigned streamId, Value *primitiveIndex, Value *emitVertsPtr, Value *outVertsPtr,
                                  Value *totalEmitVertsPtr) {
  if (!m_pipelineState->isVertexStreamActive(streamId))
    return; // Skip if this vertex stream is marked as inactive

  if (!m_gsHandlers.emit)
    m_gsHandlers.emit = createGsEmitHandler();

  m_builder.CreateCall(m_gsHandlers.emit,
                       {primitiveIndex, m_builder.getInt32(streamId), emitVertsPtr, outVertsPtr, totalEmitVertsPtr});
}

// =====================================================================================================================
// Process the dialect op NggGsCut.
//
// @param streamId : ID of output vertex stream
// @param [in/out] outVertsPtr : Pointer to the counter of GS output vertices of current primitive for this stream
void NggPrimShader::processGsCut(unsigned streamId, Value *outVertsPtr) {
  if (!m_pipelineState->isVertexStreamActive(streamId))
    return; // Skip if this vertex stream is marked as inactive

  if (!m_gsHandlers.cut)
    m_gsHandlers.cut = createGsCutHandler();

  m_builder.CreateCall(m_gsHandlers.cut, outVertsPtr);
}

// =====================================================================================================================
// Create the function that processes the dialect op NggGsEmit.
Function *NggPrimShader::createGsEmitHandler() {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   emitVerts++
  //   outVerts++
  //   totalEmitVerts++
  //   outVerts = (totalEmitVerts >= outputVertices) ? 0 : outVerts
  //
  //   if (outVerts >= outVertsPerPrim) {
  //     winding = triangleStrip ? ((outVerts - outVertsPerPrim) & 0x1) : 0
  //     N (starting vertex index) = primitiveIndex * outputVertices + emitVerts - outVertsPerPrim
  //     primData[N] = winding
  //   }
  //
  const auto addrSpace = m_builder.GetInsertBlock()->getModule()->getDataLayout().getAllocaAddrSpace();
  auto funcTy = FunctionType::get(m_builder.getVoidTy(),
                                  {
                                      m_builder.getInt32Ty(),                              // %primitiveIndex
                                      m_builder.getInt32Ty(),                              // %streamId
                                      PointerType::get(m_builder.getInt32Ty(), addrSpace), // %emitVertsPtr
                                      PointerType::get(m_builder.getInt32Ty(), addrSpace), // %outVertsPtr
                                      PointerType::get(m_builder.getInt32Ty(), addrSpace), // %totalEmitVertsPtr
                                  },
                                  false);
  auto func =
      Function::Create(funcTy, GlobalValue::InternalLinkage, NggGsEmit, m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveIndex = argIt++;
  primitiveIndex->setName("primitiveIndex");

  Value *streamId = argIt++;
  streamId->setName("streamId");

  Value *emitVertsPtr = argIt++;
  emitVertsPtr->setName("emitVertsPtr");

  Value *outVertsPtr = argIt++;
  outVertsPtr->setName("outVertsPtr");

  Value *totalEmitVertsPtr = argIt++;
  totalEmitVertsPtr->setName("totalEmitVertsPtr");

  auto entryBlock = createBlock(func, ".entry");
  auto emitPrimBlock = createBlock(func, ".emitPrim");
  auto endEmitPrimBlock = createBlock(func, ".endEmitPrim");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();

  // Construct ".entry" block
  Value *emitVerts = nullptr;
  Value *outVerts = nullptr;
  Value *totalEmitVerts = nullptr;
  Value *primEmit = nullptr;
  {
    m_builder.SetInsertPoint(entryBlock);

    emitVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), emitVertsPtr);
    outVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), outVertsPtr);

    // emitVerts++
    emitVerts = m_builder.CreateAdd(emitVerts, m_builder.getInt32(1));

    // outVerts++
    outVerts = m_builder.CreateAdd(outVerts, m_builder.getInt32(1));

    if (geometryMode.robustGsEmits) {
      totalEmitVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), totalEmitVertsPtr);
      // totalEmitVerts++
      totalEmitVerts = m_builder.CreateAdd(totalEmitVerts, m_builder.getInt32(1));
      // outVerts = (totalEmitVerts > outputVertices) ? 0 : outVerts
      Value *outOfRange = m_builder.CreateICmpUGT(totalEmitVerts, m_builder.getInt32(geometryMode.outputVertices));
      outVerts = m_builder.CreateSelect(outOfRange, m_builder.getInt32(0), outVerts);
    }

    // primEmit = (outVerts >= outVertsPerPrim)
    primEmit = m_builder.CreateICmpUGE(outVerts, m_builder.getInt32(outVertsPerPrim));
    m_builder.CreateCondBr(primEmit, emitPrimBlock, endEmitPrimBlock);
  }

  // Construct ".emitPrim" block
  {
    m_builder.SetInsertPoint(emitPrimBlock);

    // vertexIndex = primitiveIndex * outputVertices + emitVerts - outVertsPerPrim
    auto vertexIndex = m_builder.CreateMul(primitiveIndex, m_builder.getInt32(geometryMode.outputVertices));
    vertexIndex = m_builder.CreateAdd(vertexIndex, emitVerts);
    vertexIndex = m_builder.CreateSub(vertexIndex, m_builder.getInt32(outVertsPerPrim));

    Value *winding = m_builder.getInt32(0);
    if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip) {
      winding = m_builder.CreateSub(outVerts, m_builder.getInt32(outVertsPerPrim));
      winding = m_builder.CreateAnd(winding, 0x1);
    }

    // Write primitive data (just winding)
    const unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::PrimitiveData);
    // ldsOffset = regionStart + vertexIndex + maxThreadsPerSubgroup * streamId
    auto ldsOffset = m_builder.CreateAdd(m_builder.getInt32(regionStart), vertexIndex);
    ldsOffset =
        m_builder.CreateAdd(ldsOffset, m_builder.CreateMul(m_builder.getInt32(m_maxThreadsPerSubgroup), streamId));
    writeValueToLds(winding, ldsOffset);

    m_builder.CreateBr(endEmitPrimBlock);
  }

  // Construct ".endEmitPrim" block
  {
    m_builder.SetInsertPoint(endEmitPrimBlock);

    m_builder.CreateStore(emitVerts, emitVertsPtr);
    m_builder.CreateStore(outVerts, outVertsPtr);

    if (geometryMode.robustGsEmits)
      m_builder.CreateStore(totalEmitVerts, totalEmitVertsPtr);

    m_builder.CreateRetVoid();
  }

  return func;
}

// =====================================================================================================================
// Create the function that processes the dialect op NggGsCut.
Function *NggPrimShader::createGsCutHandler() {
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  //   outVerts = 0
  //
  const auto addrSpace = m_builder.GetInsertBlock()->getModule()->getDataLayout().getAllocaAddrSpace();
  auto funcTy =
      FunctionType::get(m_builder.getVoidTy(), PointerType::get(m_builder.getInt32Ty(), addrSpace), // %outVertsPtr
                        false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, NggGsCut, m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *outVertsPtr = argIt++;
  outVertsPtr->setName("outVertsPtr");

  auto entryBlock = createBlock(func, ".entry");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);
    m_builder.CreateStore(m_builder.getInt32(0), outVertsPtr); // Reset outVerts
    m_builder.CreateRetVoid();
  }

  return func;
}

// =====================================================================================================================
// Reads per-thread data from the specified primitive shader region in LDS.
//
// @param readDataTy : Data read from LDS
// @param threadId : Thread ID in subgroup to calculate LDS offset
// @param region : Primitive shader LDS region
// @param offsetInRegion : Offset within this LDS region (in dwords), the default is 0 (from the region beginning)
// @param useDs128 : Whether to use 128-bit LDS read, 16-byte alignment is guaranteed by caller
Value *NggPrimShader::readPerThreadDataFromLds(Type *readDataTy, Value *threadId, PrimShaderLdsRegion region,
                                               unsigned offsetInRegion, bool useDs128) {
  assert(region !=
         PrimShaderLdsRegion::VertexCullInfo); // Vertex cull info region is an aggregate-typed one, not applicable
  assert(readDataTy->getPrimitiveSizeInBits() % 32 == 0); // Must be dwords
  auto sizeInDwords = readDataTy->getPrimitiveSizeInBits() / 32;

  const auto regionStart = getLdsRegionStart(region);

  Value *ldsOffset = nullptr;
  if (sizeInDwords > 1)
    ldsOffset = m_builder.CreateMul(threadId, m_builder.getInt32(sizeInDwords));
  else
    ldsOffset = threadId;
  ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + offsetInRegion));

  return readValueFromLds(readDataTy, ldsOffset, useDs128);
}

// =====================================================================================================================
// Writes the per-thread data to the specified primitive shader region in LDS.
//
// @param writeData : Data written to LDS
// @param threadId : Thread ID in subgroup to calculate LDS offset
// @param region : Primitive shader LDS region
// @param offsetInRegion : Offset within this LDS region (in dwords), the default is 0 (from the region beginning)
// @param useDs128 : Whether to use 128-bit LDS write, 16-byte alignment is guaranteed by caller
void NggPrimShader::writePerThreadDataToLds(Value *writeData, Value *threadId, PrimShaderLdsRegion region,
                                            unsigned offsetInRegion, bool useDs128) {
  assert(region !=
         PrimShaderLdsRegion::VertexCullInfo); // Vertex cull info region is an aggregate-typed one, not applicable
  auto writeDataTy = writeData->getType();
  assert(writeDataTy->getPrimitiveSizeInBits() % 32 == 0); // Must be dwords
  auto sizeInDwords = writeDataTy->getPrimitiveSizeInBits() / 32;

  const auto regionStart = getLdsRegionStart(region);

  Value *ldsOffset = nullptr;
  if (sizeInDwords > 1)
    ldsOffset = m_builder.CreateMul(threadId, m_builder.getInt32(sizeInDwords));
  else
    ldsOffset = threadId;
  ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + offsetInRegion));

  writeValueToLds(writeData, ldsOffset, useDs128);
}

// =====================================================================================================================
// Reads vertex cull info from LDS (the region of vertex cull info).
//
// @param readDataTy : Data read from LDS
// @param vertexItemOffset : Per-vertex item offset (in dwords) in subgroup of the entire vertex cull info
// @param dataOffset : Data offset (in dwords) within an item of vertex cull info
Value *NggPrimShader::readVertexCullInfoFromLds(Type *readDataTy, Value *vertexItemOffset, unsigned dataOffset) {
  // Only applied to NGG culling mode without API GS
  assert(!m_hasGs && !m_nggControl->passthroughMode);
  assert(dataOffset != InvalidValue);

  const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + dataOffset));
  return readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes vertex cull info to LDS (the region of vertex cull info).
//
// @param writeData : Data written to LDS
// @param vertexItemOffset : Per-vertex item offset (in dwords) in subgroup of the entire vertex cull info
// @param dataOffset : Data offset (in dwords) within an item of vertex cull info
void NggPrimShader::writeVertexCullInfoToLds(Value *writeData, Value *vertexItemOffset, unsigned dataOffset) {
  // Only applied to NGG culling mode without API GS
  assert(!m_hasGs && !m_nggControl->passthroughMode);
  assert(dataOffset != InvalidValue);

  const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + dataOffset));
  writeValueToLds(writeData, ldsOffset);
}

// =====================================================================================================================
// Run backface culler.
//
// @param primitiveAlreadyCulled : Whether this primitive has been already culled before running the culler
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::runBackfaceCuller(Value *primitiveAlreadyCulled, Value *vertex0, Value *vertex1, Value *vertex2) {
  assert(m_nggControl->enableBackfaceCulling);

  if (!m_cullers.backface)
    m_cullers.backface = createBackfaceCuller();

  // Get register PA_SU_SC_MODE_CNTL
  Value *paSuScModeCntl = fetchCullingControlRegister(m_cbLayoutTable.paSuScModeCntl);

  // Get register PA_CL_VPORT_XSCALE
  auto paClVportXscale = fetchCullingControlRegister(m_cbLayoutTable.vportControls[0].paClVportXscale);

  // Get register PA_CL_VPORT_YSCALE
  auto paClVportYscale = fetchCullingControlRegister(m_cbLayoutTable.vportControls[0].paClVportYscale);

  // Run backface culler
  return m_builder.CreateCall(m_cullers.backface, {primitiveAlreadyCulled, vertex0, vertex1, vertex2,
                                                   m_builder.getInt32(m_nggControl->backfaceExponent), paSuScModeCntl,
                                                   paClVportXscale, paClVportYscale});
}

// =====================================================================================================================
// Run frustum culler.
//
// @param primitiveAlreadyCulled : Whether this primitive has been already culled before running the culler
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::runFrustumCuller(Value *primitiveAlreadyCulled, Value *vertex0, Value *vertex1, Value *vertex2) {
  assert(m_nggControl->enableFrustumCulling);

  if (!m_cullers.frustum)
    m_cullers.frustum = createFrustumCuller();

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = fetchCullingControlRegister(m_cbLayoutTable.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(m_cbLayoutTable.paClGbVertDiscAdj);

  // Run frustum culler
  return m_builder.CreateCall(m_cullers.frustum, {primitiveAlreadyCulled, vertex0, vertex1, vertex2, paClClipCntl,
                                                  paClGbHorzDiscAdj, paClGbVertDiscAdj});
}

// =====================================================================================================================
// Run box filter culler.
//
// @param primitiveAlreadyCulled : Whether this primitive has been already culled before running the culler
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::runBoxFilterCuller(Value *primitiveAlreadyCulled, Value *vertex0, Value *vertex1,
                                         Value *vertex2) {
  assert(m_nggControl->enableBoxFilterCulling);

  if (!m_cullers.boxFilter)
    m_cullers.boxFilter = createBoxFilterCuller();

  // Get register PA_CL_VTE_CNTL
  Value *paClVteCntl = fetchCullingControlRegister(m_cbLayoutTable.paClVteCntl);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = fetchCullingControlRegister(m_cbLayoutTable.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(m_cbLayoutTable.paClGbVertDiscAdj);

  // Run box filter culler
  return m_builder.CreateCall(m_cullers.boxFilter, {primitiveAlreadyCulled, vertex0, vertex1, vertex2, paClVteCntl,
                                                    paClClipCntl, paClGbHorzDiscAdj, paClGbVertDiscAdj});
}

// =====================================================================================================================
// Run sphere culler.
//
// @param primitiveAlreadyCulled : Whether this primitive has been already culled before running the culler
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::runSphereCuller(Value *primitiveAlreadyCulled, Value *vertex0, Value *vertex1, Value *vertex2) {
  assert(m_nggControl->enableSphereCulling);

  if (!m_cullers.sphere)
    m_cullers.sphere = createSphereCuller();

  // Get register PA_CL_VTE_CNTL
  Value *paClVteCntl = fetchCullingControlRegister(m_cbLayoutTable.paClVteCntl);

  // Get register PA_CL_CLIP_CNTL
  Value *paClClipCntl = fetchCullingControlRegister(m_cbLayoutTable.paClClipCntl);

  // Get register PA_CL_GB_HORZ_DISC_ADJ
  auto paClGbHorzDiscAdj = fetchCullingControlRegister(m_cbLayoutTable.paClGbHorzDiscAdj);

  // Get register PA_CL_GB_VERT_DISC_ADJ
  auto paClGbVertDiscAdj = fetchCullingControlRegister(m_cbLayoutTable.paClGbVertDiscAdj);

  // Run small primitive filter culler
  return m_builder.CreateCall(m_cullers.sphere, {primitiveAlreadyCulled, vertex0, vertex1, vertex2, paClVteCntl,
                                                 paClClipCntl, paClGbHorzDiscAdj, paClGbVertDiscAdj});
}

// =====================================================================================================================
// Run small primitive filter culler.
//
// @param primitiveAlreadyCulled : Whether this primitive has been already culled before running the culler
// @param vertex0 : Position data of vertex0
// @param vertex1 : Position data of vertex1
// @param vertex2 : Position data of vertex2
Value *NggPrimShader::runSmallPrimFilterCuller(Value *primitiveAlreadyCulled, Value *vertex0, Value *vertex1,
                                               Value *vertex2) {
  assert(m_nggControl->enableSmallPrimFilter);

  if (!m_cullers.smallPrimFilter)
    m_cullers.smallPrimFilter = createSmallPrimFilterCuller();

  // Get register PA_CL_VTE_CNTL
  Value *paClVteCntl = fetchCullingControlRegister(m_cbLayoutTable.paClVteCntl);

  // Get register PA_CL_VPORT_XSCALE
  auto paClVportXscale = fetchCullingControlRegister(m_cbLayoutTable.vportControls[0].paClVportXscale);

  // Get register PA_CL_VPORT_XOFFSET
  auto paClVportXoffset = fetchCullingControlRegister(m_cbLayoutTable.vportControls[0].paClVportXoffset);

  // Get register PA_CL_VPORT_YSCALE
  auto paClVportYscale = fetchCullingControlRegister(m_cbLayoutTable.vportControls[0].paClVportYscale);

  // Get register PA_CL_VPORT_YOFFSET
  auto paClVportYoffset = fetchCullingControlRegister(m_cbLayoutTable.vportControls[0].paClVportYoffset);

  // Get run-time flag enableConservativeRasterization
  auto conservativeRaster = fetchCullingControlRegister(m_cbLayoutTable.enableConservativeRasterization);
  conservativeRaster = m_builder.CreateICmpEQ(conservativeRaster, m_builder.getInt32(1));

  // Run small primitive filter culler
  return m_builder.CreateCall(m_cullers.smallPrimFilter,
                              {primitiveAlreadyCulled, vertex0, vertex1, vertex2, paClVteCntl, paClVportXscale,
                               paClVportXoffset, paClVportYscale, paClVportYoffset, conservativeRaster});
}

// =====================================================================================================================
// Run cull distance culler.
//
// @param primitiveAlreadyCulled : Whether this primitive has been already culled before running the culler
// @param signMask0 : Sign mask of cull distance of vertex0
// @param signMask1 : Sign mask of cull distance of vertex1
// @param signMask2 : Sign mask of cull distance of vertex2
Value *NggPrimShader::runCullDistanceCuller(Value *primitiveAlreadyCulled, Value *signMask0, Value *signMask1,
                                            Value *signMask2) {
  assert(m_nggControl->enableCullDistanceCulling);

  if (!m_cullers.cullDistance)
    m_cullers.cullDistance = createCullDistanceCuller();

  // Run cull distance culler
  return m_builder.CreateCall(m_cullers.cullDistance, {primitiveAlreadyCulled, signMask0, signMask1, signMask2});
}

// =====================================================================================================================
// Fetches culling-control register from primitive shader table.
//
// @param regOffset : Register offset in the primitive shader table (in bytes)
Value *NggPrimShader::fetchCullingControlRegister(unsigned regOffset) {
  if (!m_cullers.regFetcher)
    m_cullers.regFetcher = createFetchCullingRegister();

  return m_builder.CreateCall(
      m_cullers.regFetcher,
      {m_nggInputs.primShaderTableAddr.first, m_nggInputs.primShaderTableAddr.second, m_builder.getInt32(regOffset)});
}

// =====================================================================================================================
// Creates the function that does backface culling.
Function *NggPrimShader::createBackfaceCuller() {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %primitiveAlreadyCulled
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %backfaceExponent
                                      m_builder.getInt32Ty(),                          // %paSuScModeCntl
                                      m_builder.getInt32Ty(),                          // %paClVportXscale
                                      m_builder.getInt32Ty()                           // %paClVportYscale
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerBackface,
                               m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveAlreadyCulled = argIt++;
  primitiveAlreadyCulled->setName("primitiveAlreadyCulled");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *backfaceExponent = argIt++;
  backfaceExponent->setName("backfaceExponent");

  Value *paSuScModeCntl = argIt++;
  paSuScModeCntl->setName("paSuScModeCntl");

  Value *paClVportXscale = argIt++;
  paClVportXscale->setName("paClVportXscale");

  Value *paClVportYscale = argIt++;
  paClVportYscale->setName("paClVportYscale");

  auto backfaceEntryBlock = createBlock(func, ".backfaceEntry");
  auto backfaceCullBlock = createBlock(func, ".backfaceCull");
  auto backfaceExponentBlock = createBlock(func, ".backfaceExponent");
  auto backfaceExitBlock = createBlock(func, ".backfaceExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".backfaceEntry" block
  {
    m_builder.SetInsertPoint(backfaceEntryBlock);
    // If the primitive has already been culled, early exit
    m_builder.CreateCondBr(primitiveAlreadyCulled, backfaceExitBlock, backfaceCullBlock);
  }

  // Construct ".backfaceCull" block
  Value *primitiveCulled1 = nullptr;
  Value *w0 = nullptr;
  Value *w1 = nullptr;
  Value *w2 = nullptr;
  Value *area = nullptr;
  {
    m_builder.SetInsertPoint(backfaceCullBlock);

    //
    // Backface culling algorithm is described as follow:
    //
    //   if ((area > 0 && face == CCW) || (area < 0 && face == CW))
    //     frontFace = true
    //
    //   backFace = !frontFace
    //
    //   if ((frontFace && cullFront) || (backFace && cullBack))
    //     primitiveCulled = true
    //

    //          | x0 y0 w0 |
    //          |          |
    //   area = | x1 y1 w1 | =  x0 * (y1 * w2 - y2 * w1) - x1 * (y0 * w2 - y2 * w0) + x2 * (y0 * w1 - y1 * w0)
    //          |          |
    //          | x2 y2 w2 |
    //
    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    w2 = m_builder.CreateExtractElement(vertex2, 3);

    auto y1W2 = m_builder.CreateFMul(y1, w2);
    auto y2W1 = m_builder.CreateFMul(y2, w1);
    auto det0 = m_builder.CreateFSub(y1W2, y2W1);
    det0 = m_builder.CreateFMul(x0, det0);

    auto y0W2 = m_builder.CreateFMul(y0, w2);
    auto y2W0 = m_builder.CreateFMul(y2, w0);
    auto det1 = m_builder.CreateFSub(y0W2, y2W0);
    det1 = m_builder.CreateFMul(x1, det1);

    auto y0W1 = m_builder.CreateFMul(y0, w1);
    auto y1W0 = m_builder.CreateFMul(y1, w0);
    auto det2 = m_builder.CreateFSub(y0W1, y1W0);
    det2 = m_builder.CreateFMul(x2, det2);

    area = m_builder.CreateFSub(det0, det1);
    area = m_builder.CreateFAdd(area, det2);

    auto areaLtZero = m_builder.CreateFCmpOLT(area, ConstantFP::get(m_builder.getFloatTy(), 0.0));
    auto areaGtZero = m_builder.CreateFCmpOGT(area, ConstantFP::get(m_builder.getFloatTy(), 0.0));

    // xScale ^ yScale
    auto frontFace = m_builder.CreateXor(paClVportXscale, paClVportYscale);

    // signbit(xScale ^ yScale)
    frontFace = createUBfe(frontFace, 31, 1);

    // face = (FACE, PA_SU_SC_MODE_CNTL[2], 0 = CCW, 1 = CW)
    auto face = createUBfe(paSuScModeCntl, 2, 1);

    // frontFace = face ^ signbit(xScale ^ yScale)
    frontFace = m_builder.CreateXor(face, frontFace);

    // frontFace = (frontFace == 0)
    frontFace = m_builder.CreateICmpEQ(frontFace, m_builder.getInt32(0));

    // frontFace = frontFace == 0 ? area < 0 : area > 0
    frontFace = m_builder.CreateSelect(frontFace, areaLtZero, areaGtZero);

    // backFace = !frontFace
    auto backFace = m_builder.CreateNot(frontFace);

    // cullFront = (CULL_FRONT, PA_SU_SC_MODE_CNTL[0], 0 = DONT CULL, 1 = CULL)
    auto cullFront = m_builder.CreateAnd(paSuScModeCntl, m_builder.getInt32(1));
    cullFront = m_builder.CreateTrunc(cullFront, m_builder.getInt1Ty());

    // cullBack = (CULL_BACK, PA_SU_SC_MODE_CNTL[1], 0 = DONT CULL, 1 = CULL)
    Value *cullBack = createUBfe(paSuScModeCntl, 1, 1);
    cullBack = m_builder.CreateTrunc(cullBack, m_builder.getInt1Ty());

    // cullFront = cullFront ? frontFace : false
    cullFront = m_builder.CreateSelect(cullFront, frontFace, m_builder.getFalse());

    // cullBack = cullBack ? backFace : false
    cullBack = m_builder.CreateSelect(cullBack, backFace, m_builder.getFalse());

    // primitiveCulled = cullFront || cullBack
    primitiveCulled1 = m_builder.CreateOr(cullFront, cullBack);

    auto nonZeroBackfaceExp = m_builder.CreateICmpNE(backfaceExponent, m_builder.getInt32(0));
    m_builder.CreateCondBr(nonZeroBackfaceExp, backfaceExponentBlock, backfaceExitBlock);
  }

  // Construct ".backfaceExponent" block
  Value *primitiveCulled2 = nullptr;
  {
    m_builder.SetInsertPoint(backfaceExponentBlock);

    //
    // Ignore area calculations that are less enough
    //   if (|area| < (10 ^ (-backfaceExponent)) / |w0 * w1 * w2| )
    //     primitiveCulled = false
    //

    // |w0 * w1 * w2|
    auto absW0W1W2 = m_builder.CreateFMul(w0, w1);
    absW0W1W2 = m_builder.CreateFMul(absW0W1W2, w2);
    absW0W1W2 = m_builder.CreateIntrinsic(Intrinsic::fabs, m_builder.getFloatTy(), absW0W1W2);

    // threshold = (10 ^ (-backfaceExponent)) / |w0 * w1 * w2|
    auto threshold = m_builder.CreateNeg(backfaceExponent);
    threshold = m_builder.CreateIntrinsic(Intrinsic::powi, {m_builder.getFloatTy(), threshold->getType()},
                                          {ConstantFP::get(m_builder.getFloatTy(), 10.0), threshold});

    auto rcpAbsW0W1W2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), absW0W1W2);
    threshold = m_builder.CreateFMul(threshold, rcpAbsW0W1W2);

    // |area|
    auto absArea = m_builder.CreateIntrinsic(Intrinsic::fabs, m_builder.getFloatTy(), area);

    // primitiveCulled = primitiveCulled && (abs(area) >= threshold)
    primitiveCulled2 = m_builder.CreateFCmpOGE(absArea, threshold);
    primitiveCulled2 = m_builder.CreateAnd(primitiveCulled1, primitiveCulled2);

    m_builder.CreateBr(backfaceExitBlock);
  }

  // Construct ".backfaceExit" block
  {
    m_builder.SetInsertPoint(backfaceExitBlock);

    Value *primitiveCulled = createPhi({{primitiveAlreadyCulled, backfaceEntryBlock},
                                        {primitiveCulled1, backfaceCullBlock},
                                        {primitiveCulled2, backfaceExponentBlock}});

    // polyMode = (POLY_MODE, PA_SU_SC_MODE_CNTL[4:3], 0 = DISABLE, 1 = DUAL)
    auto polyMode = createUBfe(paSuScModeCntl, 3, 2);

    // polyMode == 1
    auto wireFrameMode = m_builder.CreateICmpEQ(polyMode, m_builder.getInt32(1));

    // Disable backface culler if POLY_MODE is set to 1 (wireframe)
    // primitiveCulled = (polyMode == 1) ? false : primitiveCulled
    primitiveCulled = m_builder.CreateSelect(wireFrameMode, m_builder.getFalse(), primitiveCulled);

    m_builder.CreateRet(primitiveCulled);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
Function *NggPrimShader::createFrustumCuller() {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %primitiveAlreadyCulled
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClClipCntl
                                      m_builder.getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder.getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func =
      Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerFrustum, m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveAlreadyCulled = argIt++;
  primitiveAlreadyCulled->setName("primitiveAlreadyCulled");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClClipCntl = argIt++;
  paClClipCntl->setName("paClClipCntl");

  Value *paClGbHorzDiscAdj = argIt++;
  paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

  Value *paClGbVertDiscAdj = argIt++;
  paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

  auto frustumEntryBlock = createBlock(func, ".frustumEntry");
  auto frustumCullBlock = createBlock(func, ".frustumCull");
  auto frustumExitBlock = createBlock(func, ".frustumExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".frustumEntry" block
  {
    m_builder.SetInsertPoint(frustumEntryBlock);
    // If the primitive has already been culled, early exit
    m_builder.CreateCondBr(primitiveAlreadyCulled, frustumExitBlock, frustumCullBlock);
  }

  // Construct ".frustumCull" block
  Value *primitiveCulled = nullptr;
  {
    m_builder.SetInsertPoint(frustumCullBlock);

    //
    // Frustum culling algorithm is described as follow:
    //
    //   if (x[i] > xDiscAdj * w[i] && y[i] > yDiscAdj * w[i] && z[i] > zFar * w[i])
    //     primitiveCulled = true
    //
    //   if (x[i] < -xDiscAdj * w[i] && y[i] < -yDiscAdj * w[i] && z[i] < zNear * w[i])
    //     primitiveCulled &= true
    //
    //   i = [0..2]
    //

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = createUBfe(paClClipCntl, 19, 1);
    clipSpaceDef = m_builder.CreateTrunc(clipSpaceDef, m_builder.getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    auto zNear = m_builder.CreateSelect(clipSpaceDef, ConstantFP::get(m_builder.getFloatTy(), -1.0),
                                        ConstantFP::get(m_builder.getFloatTy(), 0.0));

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder.CreateBitCast(paClGbHorzDiscAdj, m_builder.getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder.CreateBitCast(paClGbVertDiscAdj, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto z0 = m_builder.CreateExtractElement(vertex0, 2);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto z1 = m_builder.CreateExtractElement(vertex1, 2);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto z2 = m_builder.CreateExtractElement(vertex2, 2);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // -xDiscAdj
    auto negXDiscAdj = m_builder.CreateFNeg(xDiscAdj);

    // -yDiscAdj
    auto negYDiscAdj = m_builder.CreateFNeg(yDiscAdj);

    Value *clipMask[6] = {};

    //
    // Get clip mask for vertex0
    //

    // (x0 < -xDiscAdj * w0) ? 0x1 : 0
    clipMask[0] = m_builder.CreateFMul(negXDiscAdj, w0);
    clipMask[0] = m_builder.CreateFCmpOLT(x0, clipMask[0]);
    clipMask[0] = m_builder.CreateSelect(clipMask[0], m_builder.getInt32(0x1), m_builder.getInt32(0));

    // (x0 > xDiscAdj * w0) ? 0x2 : 0
    clipMask[1] = m_builder.CreateFMul(xDiscAdj, w0);
    clipMask[1] = m_builder.CreateFCmpOGT(x0, clipMask[1]);
    clipMask[1] = m_builder.CreateSelect(clipMask[1], m_builder.getInt32(0x2), m_builder.getInt32(0));

    // (y0 < -yDiscAdj * w0) ? 0x4 : 0
    clipMask[2] = m_builder.CreateFMul(negYDiscAdj, w0);
    clipMask[2] = m_builder.CreateFCmpOLT(y0, clipMask[2]);
    clipMask[2] = m_builder.CreateSelect(clipMask[2], m_builder.getInt32(0x4), m_builder.getInt32(0));

    // (y0 > yDiscAdj * w0) ? 0x8 : 0
    clipMask[3] = m_builder.CreateFMul(yDiscAdj, w0);
    clipMask[3] = m_builder.CreateFCmpOGT(y0, clipMask[3]);
    clipMask[3] = m_builder.CreateSelect(clipMask[3], m_builder.getInt32(0x8), m_builder.getInt32(0));

    // (z0 < zNear * w0) ? 0x10 : 0
    clipMask[4] = m_builder.CreateFMul(zNear, w0);
    clipMask[4] = m_builder.CreateFCmpOLT(z0, clipMask[4]);
    clipMask[4] = m_builder.CreateSelect(clipMask[4], m_builder.getInt32(0x10), m_builder.getInt32(0));

    // (z0 > w0) ? 0x20 : 0
    clipMask[5] = m_builder.CreateFCmpOGT(z0, w0);
    clipMask[5] = m_builder.CreateSelect(clipMask[5], m_builder.getInt32(0x20), m_builder.getInt32(0));

    // clipMask0
    auto clipMaskX0 = m_builder.CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY0 = m_builder.CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ0 = m_builder.CreateOr(clipMask[4], clipMask[5]);
    auto clipMask0 = m_builder.CreateOr(clipMaskX0, clipMaskY0);
    clipMask0 = m_builder.CreateOr(clipMask0, clipMaskZ0);

    //
    // Get clip mask for vertex1
    //

    // (x1 < -xDiscAdj * w1) ? 0x1 : 0
    clipMask[0] = m_builder.CreateFMul(negXDiscAdj, w1);
    clipMask[0] = m_builder.CreateFCmpOLT(x1, clipMask[0]);
    clipMask[0] = m_builder.CreateSelect(clipMask[0], m_builder.getInt32(0x1), m_builder.getInt32(0));

    // (x1 > xDiscAdj * w1) ? 0x2 : 0
    clipMask[1] = m_builder.CreateFMul(xDiscAdj, w1);
    clipMask[1] = m_builder.CreateFCmpOGT(x1, clipMask[1]);
    clipMask[1] = m_builder.CreateSelect(clipMask[1], m_builder.getInt32(0x2), m_builder.getInt32(0));

    // (y1 < -yDiscAdj * w1) ? 0x4 : 0
    clipMask[2] = m_builder.CreateFMul(negYDiscAdj, w1);
    clipMask[2] = m_builder.CreateFCmpOLT(y1, clipMask[2]);
    clipMask[2] = m_builder.CreateSelect(clipMask[2], m_builder.getInt32(0x4), m_builder.getInt32(0));

    // (y1 > yDiscAdj * w1) ? 0x8 : 0
    clipMask[3] = m_builder.CreateFMul(yDiscAdj, w1);
    clipMask[3] = m_builder.CreateFCmpOGT(y1, clipMask[3]);
    clipMask[3] = m_builder.CreateSelect(clipMask[3], m_builder.getInt32(0x8), m_builder.getInt32(0));

    // (z1 < zNear * w1) ? 0x10 : 0
    clipMask[4] = m_builder.CreateFMul(zNear, w1);
    clipMask[4] = m_builder.CreateFCmpOLT(z1, clipMask[4]);
    clipMask[4] = m_builder.CreateSelect(clipMask[4], m_builder.getInt32(0x10), m_builder.getInt32(0));

    // (z1 > w1) ? 0x20 : 0
    clipMask[5] = m_builder.CreateFCmpOGT(z1, w1);
    clipMask[5] = m_builder.CreateSelect(clipMask[5], m_builder.getInt32(0x20), m_builder.getInt32(0));

    // clipMask1
    auto clipMaskX1 = m_builder.CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY1 = m_builder.CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ1 = m_builder.CreateOr(clipMask[4], clipMask[5]);
    auto clipMask1 = m_builder.CreateOr(clipMaskX1, clipMaskY1);
    clipMask1 = m_builder.CreateOr(clipMask1, clipMaskZ1);

    //
    // Get clip mask for vertex2
    //

    // (x2 < -xDiscAdj * w2) ? 0x1 : 0
    clipMask[0] = m_builder.CreateFMul(negXDiscAdj, w2);
    clipMask[0] = m_builder.CreateFCmpOLT(x2, clipMask[0]);
    clipMask[0] = m_builder.CreateSelect(clipMask[0], m_builder.getInt32(0x1), m_builder.getInt32(0));

    // (x2 > xDiscAdj * w2) ? 0x2 : 0
    clipMask[1] = m_builder.CreateFMul(xDiscAdj, w2);
    clipMask[1] = m_builder.CreateFCmpOGT(x2, clipMask[1]);
    clipMask[1] = m_builder.CreateSelect(clipMask[1], m_builder.getInt32(0x2), m_builder.getInt32(0));

    // (y2 < -yDiscAdj * w2) ? 0x4 : 0
    clipMask[2] = m_builder.CreateFMul(negYDiscAdj, w2);
    clipMask[2] = m_builder.CreateFCmpOLT(y2, clipMask[2]);
    clipMask[2] = m_builder.CreateSelect(clipMask[2], m_builder.getInt32(0x4), m_builder.getInt32(0));

    // (y2 > yDiscAdj * w2) ? 0x8 : 0
    clipMask[3] = m_builder.CreateFMul(yDiscAdj, w2);
    clipMask[3] = m_builder.CreateFCmpOGT(y2, clipMask[3]);
    clipMask[3] = m_builder.CreateSelect(clipMask[3], m_builder.getInt32(0x8), m_builder.getInt32(0));

    // (z2 < zNear * w2) ? 0x10 : 0
    clipMask[4] = m_builder.CreateFMul(zNear, w2);
    clipMask[4] = m_builder.CreateFCmpOLT(z2, clipMask[4]);
    clipMask[4] = m_builder.CreateSelect(clipMask[4], m_builder.getInt32(0x10), m_builder.getInt32(0));

    // (z2 > zFar * w2) ? 0x20 : 0
    clipMask[5] = m_builder.CreateFCmpOGT(z2, w2);
    clipMask[5] = m_builder.CreateSelect(clipMask[5], m_builder.getInt32(0x20), m_builder.getInt32(0));

    // clipMask2
    auto clipMaskX2 = m_builder.CreateOr(clipMask[0], clipMask[1]);
    auto clipMaskY2 = m_builder.CreateOr(clipMask[2], clipMask[3]);
    auto clipMaskZ2 = m_builder.CreateOr(clipMask[4], clipMask[5]);
    auto clipMask2 = m_builder.CreateOr(clipMaskX2, clipMaskY2);
    clipMask2 = m_builder.CreateOr(clipMask2, clipMaskZ2);

    // clip = clipMask0 & clipMask1 & clipMask2
    auto clip = m_builder.CreateAnd(clipMask0, clipMask1);
    clip = m_builder.CreateAnd(clip, clipMask2);

    // primitiveCulled = (clip != 0)
    primitiveCulled = m_builder.CreateICmpNE(clip, m_builder.getInt32(0));

    m_builder.CreateBr(frustumExitBlock);
  }

  // Construct ".frustumExit" block
  {
    m_builder.SetInsertPoint(frustumExitBlock);

    primitiveCulled = createPhi({{primitiveAlreadyCulled, frustumEntryBlock}, {primitiveCulled, frustumCullBlock}});

    m_builder.CreateRet(primitiveCulled);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does box filter culling.
Function *NggPrimShader::createBoxFilterCuller() {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %primitiveAlreadyCulled
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClVteCntl
                                      m_builder.getInt32Ty(),                          // %paClClipCntl
                                      m_builder.getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder.getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerBoxFilter,
                               m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveAlreadyCulled = argIt++;
  primitiveAlreadyCulled->setName("primitiveAlreadyCulled");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClVteCntl = argIt++;
  paClVteCntl->setName("paClVteCntl");

  Value *paClClipCntl = argIt++;
  paClVteCntl->setName("paClClipCntl");

  Value *paClGbHorzDiscAdj = argIt++;
  paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

  Value *paClGbVertDiscAdj = argIt++;
  paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

  auto boxFilterEntryBlock = createBlock(func, ".boxfilterEntry");
  auto boxFilterCullBlock = createBlock(func, ".boxfilterCull");
  auto boxFilterExitBlock = createBlock(func, ".boxfilterExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".boxfilterEntry" block
  {
    m_builder.SetInsertPoint(boxFilterEntryBlock);
    // If the primitive has already been culled, early exit
    m_builder.CreateCondBr(primitiveAlreadyCulled, boxFilterExitBlock, boxFilterCullBlock);
  }

  // Construct ".boxfilterCull" block
  Value *primitiveCulled = nullptr;
  {
    m_builder.SetInsertPoint(boxFilterCullBlock);

    //
    // Box filter culling algorithm is described as follow:
    //
    //   if (min(x0/w0, x1/w1, x2/w2) > xDiscAdj || max(x0/w0, x1/w1, x2/w2) < -xDiscAdj ||
    //       min(y0/w0, y1/w1, y2/w2) > yDiscAdj || max(y0/w0, y1/w1, y2/w2) < -yDiscAdj ||
    //       min(z0/w0, z1/w1, z2/w2) > zFar     || min(z0/w0, z1/w1, z2/w2) < zNear)
    //     primitiveCulled = true
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = createUBfe(paClVteCntl, 8, 1);
    vtxXyFmt = m_builder.CreateTrunc(vtxXyFmt, m_builder.getInt1Ty());

    // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    Value *vtxZFmt = createUBfe(paClVteCntl, 9, 1);
    vtxZFmt = m_builder.CreateTrunc(vtxZFmt, m_builder.getInt1Ty());

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = createUBfe(paClClipCntl, 19, 1);
    clipSpaceDef = m_builder.CreateTrunc(clipSpaceDef, m_builder.getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
    auto zNear = m_builder.CreateSelect(clipSpaceDef, ConstantFP::get(m_builder.getFloatTy(), -1.0),
                                        ConstantFP::get(m_builder.getFloatTy(), 0.0));
    auto zFar = ConstantFP::get(m_builder.getFloatTy(), 1.0);

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder.CreateBitCast(paClGbHorzDiscAdj, m_builder.getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder.CreateBitCast(paClGbVertDiscAdj, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto z0 = m_builder.CreateExtractElement(vertex0, 2);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto z1 = m_builder.CreateExtractElement(vertex1, 2);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto z2 = m_builder.CreateExtractElement(vertex2, 2);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    auto rcpW0ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    auto rcpW1ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    auto rcpW2ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // VTX_Z_FMT ? 1.0 : 1 / w0
    auto rcpW0ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_Z_FMT ? 1.0 : 1 / w1
    auto rcpW1ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_Z_FMT ? 1.0 : 1 / w2
    auto rcpW2ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder.CreateFMul(x0, rcpW0ForXy);
    // y0' = y0/w0
    y0 = m_builder.CreateFMul(y0, rcpW0ForXy);
    // z0' = z0/w0
    z0 = m_builder.CreateFMul(z0, rcpW0ForZ);
    // x1' = x1/w1
    x1 = m_builder.CreateFMul(x1, rcpW1ForXy);
    // y1' = y1/w1
    y1 = m_builder.CreateFMul(y1, rcpW1ForXy);
    // z1' = z1/w1
    z1 = m_builder.CreateFMul(z1, rcpW1ForZ);
    // x2' = x2/w2
    x2 = m_builder.CreateFMul(x2, rcpW2ForXy);
    // y2' = y2/w2
    y2 = m_builder.CreateFMul(y2, rcpW2ForXy);
    // z2' = z2/w2
    z2 = m_builder.CreateFMul(z2, rcpW2ForZ);

    // -xDiscAdj
    auto negXDiscAdj = m_builder.CreateFNeg(xDiscAdj);

    // -yDiscAdj
    auto negYDiscAdj = m_builder.CreateFNeg(yDiscAdj);

    // minX = min(x0', x1', x2')
    auto minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {x0, x1});
    minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minX, x2});

    // minX > xDiscAdj
    auto minXGtXDiscAdj = m_builder.CreateFCmpOGT(minX, xDiscAdj);

    // maxX = max(x0', x1', x2')
    auto maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {x0, x1});
    maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxX, x2});

    // maxX < -xDiscAdj
    auto maxXLtNegXDiscAdj = m_builder.CreateFCmpOLT(maxX, negXDiscAdj);

    // minY = min(y0', y1', y2')
    auto minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {y0, y1});
    minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minY, y2});

    // minY > yDiscAdj
    auto minYGtYDiscAdj = m_builder.CreateFCmpOGT(minY, yDiscAdj);

    // maxY = max(y0', y1', y2')
    auto maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {y0, y1});
    maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxY, y2});

    // maxY < -yDiscAdj
    auto maxYLtNegYDiscAdj = m_builder.CreateFCmpOLT(maxY, negYDiscAdj);

    // minZ = min(z0', z1', z2')
    auto minZ = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {z0, z1});
    minZ = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minZ, z2});

    // minZ > zFar (1.0)
    auto minZGtZFar = m_builder.CreateFCmpOGT(minZ, zFar);

    // maxZ = min(z0', z1', z2')
    auto maxZ = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {z0, z1});
    maxZ = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxZ, z2});

    // maxZ < zNear
    auto maxZLtZNear = m_builder.CreateFCmpOLT(maxZ, zNear);

    // Get cull flag
    auto cullX = m_builder.CreateOr(minXGtXDiscAdj, maxXLtNegXDiscAdj);
    auto cullY = m_builder.CreateOr(minYGtYDiscAdj, maxYLtNegYDiscAdj);
    auto cullZ = m_builder.CreateOr(minZGtZFar, maxZLtZNear);
    primitiveCulled = m_builder.CreateOr(cullX, cullY);
    primitiveCulled = m_builder.CreateOr(primitiveCulled, cullZ);

    m_builder.CreateBr(boxFilterExitBlock);
  }

  // Construct ".boxfilterExit" block
  {
    m_builder.SetInsertPoint(boxFilterExitBlock);

    primitiveCulled = createPhi({{primitiveAlreadyCulled, boxFilterEntryBlock}, {primitiveCulled, boxFilterCullBlock}});

    m_builder.CreateRet(primitiveCulled);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does sphere culling.
Function *NggPrimShader::createSphereCuller() {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %primitiveAlreadyCulled
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClVteCntl
                                      m_builder.getInt32Ty(),                          // %paClClipCntl
                                      m_builder.getInt32Ty(),                          // %paClGbHorzDiscAdj
                                      m_builder.getInt32Ty()                           // %paClGbVertDiscAdj
                                  },
                                  false);
  auto func =
      Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerSphere, m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveAlreadyCulled = argIt++;
  primitiveAlreadyCulled->setName("primitiveAlreadyCulled");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClVteCntl = argIt++;
  paClVteCntl->setName("paClVteCntl");

  Value *paClClipCntl = argIt++;
  paClVteCntl->setName("paClClipCntl");

  Value *paClGbHorzDiscAdj = argIt++;
  paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

  Value *paClGbVertDiscAdj = argIt++;
  paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

  auto sphereEntryBlock = createBlock(func, ".sphereEntry");
  auto sphereCullBlock = createBlock(func, ".sphereCull");
  auto sphereExitBlock = createBlock(func, ".sphereExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".sphereEntry" block
  {
    m_builder.SetInsertPoint(sphereEntryBlock);
    // If the primitive has already been culled, early exit
    m_builder.CreateCondBr(primitiveAlreadyCulled, sphereExitBlock, sphereCullBlock);
  }

  // Construct ".sphereCull" block
  Value *primitiveCulled = nullptr;
  {
    m_builder.SetInsertPoint(sphereCullBlock);

    //
    // Sphere culling algorithm is somewhat complex and is described as following steps:
    //   (1) Transform discard space to -1..1 space;
    //   (2) Project from 3D coordinates to barycentric coordinates;
    //   (3) Solve linear system and find barycentric coordinates of the point closest to the origin;
    //   (4) Do clamping for the closest point if necessary;
    //   (5) Backproject from barycentric coordinates to 3D coordinates;
    //   (6) Compute the distance squared from 3D coordinates of the closest point;
    //   (7) Compare the distance with 3.0 and determine the cull flag.
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = createUBfe(paClVteCntl, 8, 1);
    vtxXyFmt = m_builder.CreateTrunc(vtxXyFmt, m_builder.getInt1Ty());

    // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
    Value *vtxZFmt = createUBfe(paClVteCntl, 9, 1);
    vtxZFmt = m_builder.CreateTrunc(vtxZFmt, m_builder.getInt1Ty());

    // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
    Value *clipSpaceDef = createUBfe(paClClipCntl, 19, 1);
    clipSpaceDef = m_builder.CreateTrunc(clipSpaceDef, m_builder.getInt1Ty());

    // zNear = clipSpaceDef ? -1.0 : 0.0
    auto zNear = m_builder.CreateSelect(clipSpaceDef, ConstantFP::get(m_builder.getFloatTy(), -1.0),
                                        ConstantFP::get(m_builder.getFloatTy(), 0.0));

    // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
    auto xDiscAdj = m_builder.CreateBitCast(paClGbHorzDiscAdj, m_builder.getFloatTy());

    // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
    auto yDiscAdj = m_builder.CreateBitCast(paClGbVertDiscAdj, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto z0 = m_builder.CreateExtractElement(vertex0, 2);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto z1 = m_builder.CreateExtractElement(vertex1, 2);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto z2 = m_builder.CreateExtractElement(vertex2, 2);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    auto rcpW0ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    auto rcpW1ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    auto rcpW2ForXy = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // VTX_Z_FMT ? 1.0 : 1 / w0
    auto rcpW0ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_Z_FMT ? 1.0 : 1 / w1
    auto rcpW1ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_Z_FMT ? 1.0 : 1 / w2
    auto rcpW2ForZ = m_builder.CreateSelect(vtxZFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder.CreateFMul(x0, rcpW0ForXy);
    // y0' = y0/w0
    y0 = m_builder.CreateFMul(y0, rcpW0ForXy);
    // z0' = z0/w0
    z0 = m_builder.CreateFMul(z0, rcpW0ForZ);
    // x1' = x1/w1
    x1 = m_builder.CreateFMul(x1, rcpW1ForXy);
    // y1' = y1/w1
    y1 = m_builder.CreateFMul(y1, rcpW1ForXy);
    // z1' = z1/w1
    z1 = m_builder.CreateFMul(z1, rcpW1ForZ);
    // x2' = x2/w2
    x2 = m_builder.CreateFMul(x2, rcpW2ForXy);
    // y2' = y2/w2
    y2 = m_builder.CreateFMul(y2, rcpW2ForXy);
    // z2' = z2/w2
    z2 = m_builder.CreateFMul(z2, rcpW2ForZ);

    //
    // === Step 1 ===: Discard space to -1..1 space.
    //

    // x" = x'/xDiscAdj
    // y" = y'/yDiscAdj
    // z" = (zNear + 2.0)z' + (-1.0 - zNear)
    auto rcpXDiscAdj = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), xDiscAdj);
    auto rcpYDiscAdj = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), yDiscAdj);
    auto rcpXyDiscAdj = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {rcpXDiscAdj, rcpYDiscAdj});

    Value *x0Y0 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x0, y0});
    Value *x1Y1 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x1, y1});
    Value *x2Y2 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {x2, y2});

    x0Y0 = m_builder.CreateFMul(x0Y0, rcpXyDiscAdj);
    x1Y1 = m_builder.CreateFMul(x1Y1, rcpXyDiscAdj);
    x2Y2 = m_builder.CreateFMul(x2Y2, rcpXyDiscAdj);

    // zNear + 2.0
    auto zNearPlusTwo = m_builder.CreateFAdd(zNear, ConstantFP::get(m_builder.getFloatTy(), 2.0));
    zNearPlusTwo = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {zNearPlusTwo, zNearPlusTwo});

    // -1.0 - zNear
    auto negOneMinusZNear = m_builder.CreateFSub(ConstantFP::get(m_builder.getFloatTy(), -1.0), zNear);
    negOneMinusZNear = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {negOneMinusZNear, negOneMinusZNear});

    Value *z0Z0 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {z0, z0});
    Value *z2Z1 = m_builder.CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, {z2, z1});

    z0Z0 = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                     {zNearPlusTwo, z0Z0, negOneMinusZNear});
    z2Z1 = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                     {zNearPlusTwo, z2Z1, negOneMinusZNear});

    //
    // === Step 2 ===: 3D coordinates to barycentric coordinates.
    //

    // <x20, y20> = <x2", y2"> - <x0", y0">
    auto x20Y20 = m_builder.CreateFSub(x2Y2, x0Y0);

    // <x10, y10> = <x1", y1"> - <x0", y0">
    auto x10Y10 = m_builder.CreateFSub(x1Y1, x0Y0);

    // <z20, z10> = <z2", z1"> - <z0", z0">
    auto z20Z10 = m_builder.CreateFSub(z2Z1, z0Z0);

    //
    // === Step 3 ===: Solve linear system and find the point closest to the origin.
    //

    // a00 = x10 + z10
    auto x10 = m_builder.CreateExtractElement(x10Y10, static_cast<uint64_t>(0));
    auto z10 = m_builder.CreateExtractElement(z20Z10, 1);
    auto a00 = m_builder.CreateFAdd(x10, z10);

    // a01 = x20 + z20
    auto x20 = m_builder.CreateExtractElement(x20Y20, static_cast<uint64_t>(0));
    auto z20 = m_builder.CreateExtractElement(z20Z10, static_cast<uint64_t>(0));
    auto a01 = m_builder.CreateFAdd(x20, z20);

    // a10 = y10 + y10
    auto y10 = m_builder.CreateExtractElement(x10Y10, 1);
    auto a10 = m_builder.CreateFAdd(y10, y10);

    // a11 = y20 + z20
    auto y20 = m_builder.CreateExtractElement(x20Y20, 1);
    auto a11 = m_builder.CreateFAdd(y20, z20);

    // b0 = -x0" - x2"
    x0 = m_builder.CreateExtractElement(x0Y0, static_cast<uint64_t>(0));
    auto negX0 = m_builder.CreateFNeg(x0);
    x2 = m_builder.CreateExtractElement(x2Y2, static_cast<uint64_t>(0));
    auto b0 = m_builder.CreateFSub(negX0, x2);

    // b1 = -x1" - x2"
    x1 = m_builder.CreateExtractElement(x1Y1, static_cast<uint64_t>(0));
    auto negX1 = m_builder.CreateFNeg(x1);
    auto b1 = m_builder.CreateFSub(negX1, x2);

    //     [ a00 a01 ]      [ b0 ]       [ s ]
    // A = [         ], B = [    ], ST = [   ], A * ST = B (crame rules)
    //     [ a10 a11 ]      [ b1 ]       [ t ]

    //           | a00 a01 |
    // det(A) =  |         | = a00 * a11 - a01 * a10
    //           | a10 a11 |
    auto detA = m_builder.CreateFMul(a00, a11);
    auto negA01 = m_builder.CreateFNeg(a01);
    detA = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {negA01, a10, detA});

    //            | b0 a01 |
    // det(Ab0) = |        | = b0 * a11 - a01 * b1
    //            | b1 a11 |
    auto detAB0 = m_builder.CreateFMul(b0, a11);
    detAB0 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {negA01, b1, detAB0});

    //            | a00 b0 |
    // det(Ab1) = |        | = a00 * b1 - b0 * a10
    //            | a10 b1 |
    auto detAB1 = m_builder.CreateFMul(a00, b1);
    auto negB0 = m_builder.CreateFNeg(b0);
    detAB1 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {negB0, a10, detAB1});

    // s = det(Ab0) / det(A)
    auto rcpDetA = m_builder.CreateFDiv(ConstantFP::get(m_builder.getHalfTy(), 1.0), detA);
    auto s = m_builder.CreateFMul(detAB0, rcpDetA);

    // t = det(Ab1) / det(A)
    auto t = m_builder.CreateFMul(detAB1, rcpDetA);

    //
    // === Step 4 ===: Do clamping for the closest point.
    //

    // <s, t>
    auto st = m_builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(m_builder.getHalfTy(), 2)), s,
                                            static_cast<uint64_t>(0));
    st = m_builder.CreateInsertElement(st, t, 1);

    // <s', t'> = <0.5 - 0.5(t - s), 0.5 + 0.5(t - s)>
    auto tMinusS = m_builder.CreateFSub(t, s);
    auto sT1 = m_builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(m_builder.getHalfTy(), 2)), tMinusS,
                                             static_cast<uint64_t>(0));
    sT1 = m_builder.CreateInsertElement(sT1, tMinusS, 1);

    sT1 = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                    {ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), -0.5),
                                                          ConstantFP::get(m_builder.getHalfTy(), 0.5)}),
                                     sT1,
                                     ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), 0.5),
                                                          ConstantFP::get(m_builder.getHalfTy(), 0.5)})});

    // <s", t"> = clamp(<s, t>)
    auto sT2 = m_builder.CreateIntrinsic(Intrinsic::maxnum, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                         {st, ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), 0.0),
                                                                   ConstantFP::get(m_builder.getHalfTy(), 0.0)})});
    sT2 = m_builder.CreateIntrinsic(Intrinsic::minnum, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                    {sT2, ConstantVector::get({ConstantFP::get(m_builder.getHalfTy(), 1.0),
                                                               ConstantFP::get(m_builder.getHalfTy(), 1.0)})});

    // <s, t> = (s + t) > 1.0 ? <s', t'> : <s", t">
    auto sPlusT = m_builder.CreateFAdd(s, t);
    auto sPlusTGtOne = m_builder.CreateFCmpOGT(sPlusT, ConstantFP::get(m_builder.getHalfTy(), 1.0));
    st = m_builder.CreateSelect(sPlusTGtOne, sT1, sT2);

    //
    // === Step 5 ===: Barycentric coordinates to 3D coordinates.
    //

    // x = x0" + s * x10 + t * x20
    // y = y0" + s * y10 + t * y20
    // z = z0" + s * z10 + t * z20
    s = m_builder.CreateExtractElement(st, static_cast<uint64_t>(0));
    t = m_builder.CreateExtractElement(st, 1);
    auto ss = m_builder.CreateInsertElement(st, s, 1);
    auto tt = m_builder.CreateInsertElement(st, t, static_cast<uint64_t>(0));

    // s * <x10, y10> + <x0", y0">
    auto xy =
        m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2), {ss, x10Y10, x0Y0});

    // <x, y> = t * <x20, y20> + (s * <x10, y10> + <x0", y0">)
    xy = m_builder.CreateIntrinsic(Intrinsic::fma, FixedVectorType::get(m_builder.getHalfTy(), 2), {tt, x20Y20, xy});

    // s * z10 + z0"
    z0 = m_builder.CreateExtractElement(z0Z0, static_cast<uint64_t>(0));
    auto z = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {s, z10, z0});

    // z = t * z20 + (s * z10 + z0")
    z = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {t, z20, z});

    auto x = m_builder.CreateExtractElement(xy, static_cast<uint64_t>(0));
    auto y = m_builder.CreateExtractElement(xy, 1);

    //
    // === Step 6 ===: Compute the distance squared of the closest point.
    //

    // r^2 = x^2 + y^2 + z^2
    auto squareR = m_builder.CreateFMul(x, x);
    squareR = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {y, y, squareR});
    squareR = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getHalfTy(), {z, z, squareR});

    //
    // == = Step 7 == = : Determine the cull flag
    //

    // primitiveCulled = (r ^ 2 > 3.0)
    primitiveCulled = m_builder.CreateFCmpOGT(squareR, ConstantFP::get(m_builder.getHalfTy(), 3.0));

    m_builder.CreateBr(sphereExitBlock);
  }

  // Construct ".sphereExit" block
  {
    m_builder.SetInsertPoint(sphereExitBlock);

    primitiveCulled = createPhi({{primitiveAlreadyCulled, sphereEntryBlock}, {primitiveCulled, sphereCullBlock}});

    m_builder.CreateRet(primitiveCulled);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does small primitive filter culling.
Function *NggPrimShader::createSmallPrimFilterCuller() {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),                           // %primitiveAlreadyCulled
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex0
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex1
                                      FixedVectorType::get(m_builder.getFloatTy(), 4), // %vertex2
                                      m_builder.getInt32Ty(),                          // %paClVteCntl
                                      m_builder.getInt32Ty(),                          // %paClVportXscale
                                      m_builder.getInt32Ty(),                          // %paClVportXoffset
                                      m_builder.getInt32Ty(),                          // %paClVportYscale
                                      m_builder.getInt32Ty(),                          // %paClVportYoffset
                                      m_builder.getInt1Ty()                            // %conservativeRaster
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerSmallPrimFilter,
                               m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveAlreadyCulled = argIt++;
  primitiveAlreadyCulled->setName("primitiveAlreadyCulled");

  Value *vertex0 = argIt++;
  vertex0->setName("vertex0");

  Value *vertex1 = argIt++;
  vertex1->setName("vertex1");

  Value *vertex2 = argIt++;
  vertex2->setName("vertex2");

  Value *paClVteCntl = argIt++;
  paClVteCntl->setName("paClVteCntl");

  Value *paClVportXscale = argIt++;
  paClVportXscale->setName("paClVportXscale");

  Value *paClVportXoffset = argIt++;
  paClVportXscale->setName("paClVportXoffset");

  Value *paClVportYscale = argIt++;
  paClVportYscale->setName("paClVportYscale");

  Value *paClVportYoffset = argIt++;
  paClVportYscale->setName("paClVportYoffset");

  Value *conservativeRaster = argIt++;
  conservativeRaster->setName("conservativeRaster");

  auto smallPrimFilterEntryBlock = createBlock(func, ".smallprimfilterEntry");
  auto smallPrimFilterCullBlock = createBlock(func, ".smallprimfilterCull");
  auto smallPrimFilterExitBlock = createBlock(func, ".smallprimfilterExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".smallprimfilterEntry" block
  {
    m_builder.SetInsertPoint(smallPrimFilterEntryBlock);

    // If the primitive has already been culled or if conservative rasterization, early exit
    m_builder.CreateCondBr(m_builder.CreateOr(primitiveAlreadyCulled, conservativeRaster), smallPrimFilterExitBlock,
                           smallPrimFilterCullBlock);
  }

  // Construct ".smallprimfilterCull" block
  Value *primitiveCulled = nullptr;
  {
    m_builder.SetInsertPoint(smallPrimFilterCullBlock);

    //
    // Small primitive filter culling algorithm is described as follow:
    //
    //   if (!conservativeRaster) {
    //     if (roundEven(min(screen(x0/w0), screen(x1/w1), screen(x2/w2)) ==
    //         roundEven(max(screen(x0/w0), screen(x1/w1), screen(x2/w2))) ||
    //         roundEven(min(screen(y0/w0), screen(y1/w1), screen(y2/w2)) ==
    //         roundEven(max(screen(y0/w0), screen(y1/w1), screen(y2/w2))))
    //       primitiveCulled = true
    //
    //     allowCull = (w0 < 0 && w1 < 0 && w2 < 0) || (w0 > 0 && w1 > 0 && w2 > 0))
    //     primitiveCulled = allowCull && primitiveCulled
    //   } else
    //     primitiveCulled = false
    //

    // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
    Value *vtxXyFmt = createUBfe(paClVteCntl, 8, 1);
    vtxXyFmt = m_builder.CreateTrunc(vtxXyFmt, m_builder.getInt1Ty());

    // xScale = (VPORT_XSCALE, PA_CL_VPORT_XSCALE[31:0])
    // NOTE: This register value has already been scaled by MSAA number of samples in driver.
    auto xScale = m_builder.CreateBitCast(paClVportXscale, m_builder.getFloatTy());

    // xOffset = (VPORT_XOFFSET, PA_CL_VPORT_XOFFSET[31:0])
    auto xOffset = m_builder.CreateBitCast(paClVportXoffset, m_builder.getFloatTy());

    // yScale = (VPORT_YSCALE, PA_CL_VPORT_YSCALE[31:0])
    // NOTE: This register value has already been scaled by MSAA number of samples in driver.
    auto yScale = m_builder.CreateBitCast(paClVportYscale, m_builder.getFloatTy());

    // yOffset = (VPORT_YOFFSET, PA_CL_VPORT_YOFFSET[31:0])
    auto yOffset = m_builder.CreateBitCast(paClVportYoffset, m_builder.getFloatTy());

    auto x0 = m_builder.CreateExtractElement(vertex0, static_cast<uint64_t>(0));
    auto y0 = m_builder.CreateExtractElement(vertex0, 1);
    auto w0 = m_builder.CreateExtractElement(vertex0, 3);

    auto x1 = m_builder.CreateExtractElement(vertex1, static_cast<uint64_t>(0));
    auto y1 = m_builder.CreateExtractElement(vertex1, 1);
    auto w1 = m_builder.CreateExtractElement(vertex1, 3);

    auto x2 = m_builder.CreateExtractElement(vertex2, static_cast<uint64_t>(0));
    auto y2 = m_builder.CreateExtractElement(vertex2, 1);
    auto w2 = m_builder.CreateExtractElement(vertex2, 3);

    // Convert xyz coordinate to normalized device coordinate (NDC)
    auto rcpW0 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w0);
    auto rcpW1 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w1);
    auto rcpW2 = m_builder.CreateFDiv(ConstantFP::get(m_builder.getFloatTy(), 1.0), w2);

    // VTX_XY_FMT ? 1.0 : 1 / w0
    rcpW0 = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW0);
    // VTX_XY_FMT ? 1.0 : 1 / w1
    rcpW1 = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW1);
    // VTX_XY_FMT ? 1.0 : 1 / w2
    rcpW2 = m_builder.CreateSelect(vtxXyFmt, ConstantFP::get(m_builder.getFloatTy(), 1.0), rcpW2);

    // x0' = x0/w0
    x0 = m_builder.CreateFMul(x0, rcpW0);
    // y0' = y0/w0
    y0 = m_builder.CreateFMul(y0, rcpW0);
    // x1' = x1/w1
    x1 = m_builder.CreateFMul(x1, rcpW1);
    // y1' = y1/w1
    y1 = m_builder.CreateFMul(y1, rcpW1);
    // x2' = x2/w2
    x2 = m_builder.CreateFMul(x2, rcpW2);
    // y2' = y2/w2
    y2 = m_builder.CreateFMul(y2, rcpW2);

    // NOTE: We apply a "fast" frustum culling based on screen space. VTE will convert coordinates from clip space to
    // screen space, so we can clamp the coordinate to (viewport min, viewport max) very quickly and save all of the
    // left/right/top/bottom plane checking, which is provided by traditional frustum culling.
    Value *screenMinX = nullptr;
    Value *screenMaxX = nullptr;
    Value *screenMinY = nullptr;
    Value *screenMaxY = nullptr;
    if (!m_nggControl->enableFrustumCulling) {
      // The scale can be negative, which causes the orientation of the viewport to be flipped.
      // Handle both orientations by checking the sign of the viewport scale and negating the offset
      // if negative.
      Value *xScalelessThanZero = m_builder.CreateFCmpOLT(xScale, ConstantFP::get(m_builder.getFloatTy(), 0.0));
      Value *yScalelessThanZero = m_builder.CreateFCmpOLT(yScale, ConstantFP::get(m_builder.getFloatTy(), 0.0));
      Value *xBoundaryOffset =
          m_builder.CreateSelect(xScalelessThanZero, ConstantFP::get(m_builder.getFloatTy(), -0.75),
                                 ConstantFP::get(m_builder.getFloatTy(), 0.75));
      Value *yBoundaryOffset =
          m_builder.CreateSelect(yScalelessThanZero, ConstantFP::get(m_builder.getFloatTy(), -0.75),
                                 ConstantFP::get(m_builder.getFloatTy(), 0.75));
      // screenMinX = -xScale + xOffset - 0.75
      screenMinX = m_builder.CreateFAdd(m_builder.CreateFNeg(xScale), xOffset);
      screenMinX = m_builder.CreateFAdd(screenMinX, m_builder.CreateFNeg(xBoundaryOffset));

      // screenMaxX = xScale + xOffset + 0.75
      screenMaxX = m_builder.CreateFAdd(xScale, xOffset);
      screenMaxX = m_builder.CreateFAdd(screenMaxX, xBoundaryOffset);

      // screenMinY = -yScale + yOffset - 0.75
      screenMinY = m_builder.CreateFAdd(m_builder.CreateFNeg(yScale), yOffset);
      screenMinY = m_builder.CreateFAdd(screenMinY, m_builder.CreateFNeg(yBoundaryOffset));

      // screenMaxY = yScale + yOffset + 0.75
      screenMaxY = m_builder.CreateFAdd(yScale, yOffset);
      screenMaxY = m_builder.CreateFAdd(screenMaxY, yBoundaryOffset);
    }

    // screenX0' = x0' * xScale + xOffset
    auto screenX0 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {x0, xScale, xOffset});

    // screenX1' = x1' * xScale + xOffset
    auto screenX1 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {x1, xScale, xOffset});

    // screenX2' = x2' * xScale + xOffset
    auto screenX2 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {x2, xScale, xOffset});

    // minX = clamp(min(screenX0', screenX1', screenX2'), screenMinX, screenMaxX) - 1/256.0
    Value *minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {screenX0, screenX1});
    minX = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minX, screenX2});
    if (!m_nggControl->enableFrustumCulling) {
      minX = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinX, minX, screenMaxX});
    }
    minX = m_builder.CreateFAdd(minX, ConstantFP::get(m_builder.getFloatTy(), -1 / 256.0));

    // minX = roundEven(minX)
    minX = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), minX);

    // maxX = clamp(max(screenX0', screenX1', screenX2'), screenMinX, screenMaxX) + 1/256.0
    Value *maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {screenX0, screenX1});
    maxX = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxX, screenX2});
    if (!m_nggControl->enableFrustumCulling) {
      maxX = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinX, maxX, screenMaxX});
    }
    maxX = m_builder.CreateFAdd(maxX, ConstantFP::get(m_builder.getFloatTy(), 1 / 256.0));

    // maxX = roundEven(maxX)
    maxX = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), maxX);

    // screenY0' = y0' * yScale + yOffset
    auto screenY0 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {y0, yScale, yOffset});

    // screenY1' = y1' * yScale + yOffset
    auto screenY1 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {y1, yScale, yOffset});

    // screenY2' = y2' * yScale + yOffset
    auto screenY2 = m_builder.CreateIntrinsic(Intrinsic::fma, m_builder.getFloatTy(), {y2, yScale, yOffset});

    // minY = clamp(min(screenY0', screenY1', screenY2'), screenMinY, screenMaxY) - 1/256.0
    Value *minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {screenY0, screenY1});
    minY = m_builder.CreateIntrinsic(Intrinsic::minnum, m_builder.getFloatTy(), {minY, screenY2});
    if (!m_nggControl->enableFrustumCulling) {
      minY = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinY, minY, screenMaxY});
    }
    minY = m_builder.CreateFAdd(minY, ConstantFP::get(m_builder.getFloatTy(), -1 / 256.0));

    // minY = roundEven(minY)
    minY = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), minY);

    // maxY = clamp(max(screenX0', screenY1', screenY2'), screenMinY, screenMaxY) + 1/256.0
    Value *maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {screenY0, screenY1});
    maxY = m_builder.CreateIntrinsic(Intrinsic::maxnum, m_builder.getFloatTy(), {maxY, screenY2});
    if (!m_nggControl->enableFrustumCulling) {
      maxY = m_builder.CreateIntrinsic(Intrinsic::amdgcn_fmed3, m_builder.getFloatTy(), {screenMinY, maxY, screenMaxY});
    }
    maxY = m_builder.CreateFAdd(maxY, ConstantFP::get(m_builder.getFloatTy(), 1 / 256.0));

    // maxY = roundEven(maxY)
    maxY = m_builder.CreateIntrinsic(Intrinsic::rint, m_builder.getFloatTy(), maxY);

    // minX == maxX
    auto minXEqMaxX = m_builder.CreateFCmpOEQ(minX, maxX);

    // minY == maxY
    auto minYEqMaxY = m_builder.CreateFCmpOEQ(minY, maxY);

    // Get primitive culled flag
    primitiveCulled = m_builder.CreateOr(minXEqMaxX, minYEqMaxY);

    // Check if W allows culling
    auto w0AsInt = m_builder.CreateBitCast(w0, m_builder.getInt32Ty());
    auto w1AsInt = m_builder.CreateBitCast(w1, m_builder.getInt32Ty());
    auto w2AsInt = m_builder.CreateBitCast(w2, m_builder.getInt32Ty());

    // w0 < 0 && w1 < 0 && w2 < 0
    auto isAllWNeg = m_builder.CreateAnd(w0AsInt, w1AsInt);
    isAllWNeg = m_builder.CreateAnd(isAllWNeg, w2AsInt);
    isAllWNeg = m_builder.CreateICmpSLT(isAllWNeg, m_builder.getInt32(0));

    // w0 > 0 && w1 > 0 && w2 > 0
    auto isAllWPos = m_builder.CreateOr(w0AsInt, w1AsInt);
    isAllWPos = m_builder.CreateOr(isAllWPos, w2AsInt);
    isAllWPos = m_builder.CreateICmpSGT(isAllWPos, m_builder.getInt32(0));

    auto allowCull = m_builder.CreateOr(isAllWNeg, isAllWPos);
    primitiveCulled = m_builder.CreateAnd(allowCull, primitiveCulled);

    m_builder.CreateBr(smallPrimFilterExitBlock);
  }

  // Construct ".smallprimfilterExit" block
  {
    m_builder.SetInsertPoint(smallPrimFilterExitBlock);

    primitiveCulled =
        createPhi({{primitiveAlreadyCulled, smallPrimFilterEntryBlock}, {primitiveCulled, smallPrimFilterCullBlock}});

    m_builder.CreateRet(primitiveCulled);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
Function *NggPrimShader::createCullDistanceCuller() {
  auto funcTy = FunctionType::get(m_builder.getInt1Ty(),
                                  {
                                      m_builder.getInt1Ty(),  // %primitiveAlreadyCulled
                                      m_builder.getInt32Ty(), // %signMask0
                                      m_builder.getInt32Ty(), // %signMask1
                                      m_builder.getInt32Ty()  // %signMask2
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerCullDistance,
                               m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setDoesNotAccessMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primitiveAlreadyCulled = argIt++;
  primitiveAlreadyCulled->setName("primitiveAlreadyCulled");

  Value *signMask0 = argIt++;
  signMask0->setName("signMask0");

  Value *signMask1 = argIt++;
  signMask1->setName("signMask1");

  Value *signMask2 = argIt++;
  signMask2->setName("signMask2");

  auto cullDistanceEntryBlock = createBlock(func, ".culldistanceEntry");
  auto cullDistanceCullBlock = createBlock(func, ".culldistanceCull");
  auto cullDistanceExitBlock = createBlock(func, ".culldistanceExit");

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct ".culldistanceEntry" block
  {
    m_builder.SetInsertPoint(cullDistanceEntryBlock);
    // If the primitive has already been culled, early exit
    m_builder.CreateCondBr(primitiveAlreadyCulled, cullDistanceExitBlock, cullDistanceCullBlock);
  }

  // Construct ".culldistanceCull" block
  Value *primitiveCulled = nullptr;
  {
    m_builder.SetInsertPoint(cullDistanceCullBlock);

    //
    // Cull distance culling algorithm is described as follow:
    //
    //   vertexSignMask[7:0] = [sign(ClipDistance[0])..sign(ClipDistance[7])]
    //   primSignMask = vertexSignMask0 & vertexSignMask1 & vertexSignMask2
    //   primitiveCulled = (primSignMask != 0)
    //
    auto signMask = m_builder.CreateAnd(signMask0, signMask1);
    signMask = m_builder.CreateAnd(signMask, signMask2);

    primitiveCulled = m_builder.CreateICmpNE(signMask, m_builder.getInt32(0));

    m_builder.CreateBr(cullDistanceExitBlock);
  }

  // Construct ".culldistanceExit" block
  {
    m_builder.SetInsertPoint(cullDistanceExitBlock);

    primitiveCulled =
        createPhi({{primitiveAlreadyCulled, cullDistanceEntryBlock}, {primitiveCulled, cullDistanceCullBlock}});

    m_builder.CreateRet(primitiveCulled);
  }

  return func;
}

// =====================================================================================================================
// Creates the function that fetches culling control registers.
Function *NggPrimShader::createFetchCullingRegister() {
  auto funcTy = FunctionType::get(m_builder.getInt32Ty(),
                                  {
                                      m_builder.getInt32Ty(), // %primShaderTableAddrLow
                                      m_builder.getInt32Ty(), // %primShaderTableAddrHigh
                                      m_builder.getInt32Ty()  // %regOffset
                                  },
                                  false);
  auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, NggCullerRegFetcher,
                               m_builder.GetInsertBlock()->getModule());

  func->setCallingConv(CallingConv::C);
  func->setOnlyReadsMemory();
  func->addFnAttr(Attribute::AlwaysInline);

  auto argIt = func->arg_begin();
  Value *primShaderTableAddrLow = argIt++;
  primShaderTableAddrLow->setName("primShaderTableAddrLow");

  Value *primShaderTableAddrHigh = argIt++;
  primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

  Value *regOffset = argIt++;
  regOffset->setName("regOffset");

  BasicBlock *entryBlock = createBlock(func); // Create entry block

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Construct entry block
  {
    m_builder.SetInsertPoint(entryBlock);

    Value *primShaderTableAddr =
        m_builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 2)),
                                      primShaderTableAddrLow, static_cast<uint64_t>(0));

    primShaderTableAddr = m_builder.CreateInsertElement(primShaderTableAddr, primShaderTableAddrHigh, 1);

    primShaderTableAddr = m_builder.CreateBitCast(primShaderTableAddr, m_builder.getInt64Ty());

    auto primShaderTableEltTy = ArrayType::get(m_builder.getInt32Ty(), 256);
    auto primShaderTablePtrTy = PointerType::get(primShaderTableEltTy, ADDR_SPACE_CONST); // [256 x i32]
    auto primShaderTablePtr = m_builder.CreateIntToPtr(primShaderTableAddr, primShaderTablePtrTy);

    // regOffset = regOffset >> 2
    regOffset = m_builder.CreateLShr(regOffset, 2); // To dword offset

    auto loadPtr = m_builder.CreateGEP(primShaderTableEltTy, primShaderTablePtr, {m_builder.getInt32(0), regOffset});
    cast<Instruction>(loadPtr)->setMetadata(MetaNameUniform, MDNode::get(m_builder.getContext(), {}));

    auto regValue = m_builder.CreateAlignedLoad(m_builder.getInt32Ty(), loadPtr, Align(4));
    regValue->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));

    m_builder.CreateRet(regValue);
  }

  return func;
}

// =====================================================================================================================
// Output a wave-base ballot (always return i64 mask)
//
// @param value : The value to do the ballot on.
Value *NggPrimShader::ballot(Value *value) {
  assert(value->getType()->isIntegerTy(1)); // Should be i1

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  assert(waveSize == 32 || waveSize == 64);

  Value *result = m_builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, m_builder.getIntNTy(waveSize), value);
  if (waveSize == 32)
    result = m_builder.CreateZExt(result, m_builder.getInt64Ty());

  return result;
}

// =====================================================================================================================
// Make an export collector from the specified function by returning the exports of position/attribute/XFB. The input
// collections are to decide which kinds of exports will be collected.
//
// @param [in/out] fromFunc : Function from which to make an export collector
// @param makeClone : Whether to make a clone of the specified function
// @param [out] positionExports : Collection of position exports
// @param [out] attributeExports : Collection of attribute exports
// @param [out] xfbExports : Collection of XFB exports
// @returns : Export collector after mutation
Function *NggPrimShader::makeExportCollector(Function *&fromFunc, bool makeClone,
                                             SmallVectorImpl<VertexExport> *positionExports,
                                             SmallVectorImpl<VertexExport> *attributeExports,
                                             SmallVectorImpl<XfbExport> *xfbExports) {
  const bool collectPositionExports = positionExports != nullptr;
  const bool collectAttributeExports = attributeExports != nullptr;
  const bool collectXfbExports = m_pipelineState->enableSwXfb() && xfbExports != nullptr;
  assert(collectPositionExports || collectAttributeExports || collectXfbExports);

  //
  // Count vertex position/attribute/XFB exports.
  //
  unsigned numPositionExports = 0;
  unsigned numAttributeExports = 0;
  unsigned numXfbExports = 0;

  {
    struct Payload {
      const bool collectPositionExports;
      const bool collectAttributeExports;
      const bool collectXfbExports;
      unsigned &numPositionExports;
      unsigned &numAttributeExports;
      unsigned &numXfbExports;
    };
    Payload payload = {collectPositionExports, collectAttributeExports, collectXfbExports,
                       numPositionExports,     numAttributeExports,     numXfbExports};

    static const auto visitor =
        llvm_dialects::VisitorBuilder<Payload>()
            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
            .add<NggExportPositionOp>([](Payload &payload, NggExportPositionOp &exportPositionOp) {
              if (payload.collectPositionExports)
                ++payload.numPositionExports;
            })
            .add<NggExportAttributeOp>([](Payload &payload, NggExportAttributeOp &exportAttributeOp) {
              if (payload.collectAttributeExports)
                ++payload.numAttributeExports;
            })
            .add<WriteXfbOutputOp>([](Payload &payload, WriteXfbOutputOp &writeXfbOutputOp) {
              if (payload.collectXfbExports)
                ++payload.numXfbExports;
            })
            .build();
    visitor.visit(payload, *fromFunc);
  }

  if (numPositionExports == 0 && numAttributeExports == 0 && numXfbExports == 0)
    return nullptr; // No exports to collect

  auto exportTy = FixedVectorType::get(m_builder.getFloatTy(), 4);

  ArrayType *positionExportsTy = nullptr;
  if (numPositionExports > 0) {
    positionExportsTy = ArrayType::get(exportTy, numPositionExports);
    positionExports->resize(numPositionExports);
  }

  ArrayType *attributeExportsTy = nullptr;
  if (numAttributeExports > 0) {
    attributeExportsTy = ArrayType::get(exportTy, numAttributeExports);
    attributeExports->resize(numAttributeExports);
  }

  ArrayType *xfbExportsTy = nullptr;
  if (numXfbExports > 0) {
    xfbExportsTy = ArrayType::get(exportTy, numXfbExports);
    xfbExports->resize(numXfbExports);
  }

  SmallVector<Type *, 3> exportsTy;
  if (positionExportsTy)
    exportsTy.push_back(positionExportsTy);
  if (attributeExportsTy)
    exportsTy.push_back(attributeExportsTy);
  if (xfbExportsTy)
    exportsTy.push_back(xfbExportsTy);
  auto returnTy = StructType::get(m_builder.getContext(), exportsTy);

  Function *exportCollector = nullptr;
  if (makeClone) {
    auto exportCollectorTy = FunctionType::get(returnTy, fromFunc->getFunctionType()->params(), false);
    exportCollector = Function::Create(exportCollectorTy, GlobalVariable::InternalLinkage, "", fromFunc->getParent());

    ValueToValueMapTy valueMap;

    Argument *newArg = exportCollector->arg_begin();
    for (Argument &arg : fromFunc->args())
      valueMap[&arg] = newArg++;

    SmallVector<ReturnInst *, 8> retInsts;
    CloneFunctionInto(exportCollector, fromFunc, valueMap, CloneFunctionChangeType::LocalChangesOnly, retInsts);
  } else {
    exportCollector = addFunctionArgs(fromFunc, returnTy, {}, {});

    // Original function is no longer needed
    assert(fromFunc->use_empty());
    fromFunc->eraseFromParent();
    fromFunc = nullptr;
  }

  exportCollector->setDLLStorageClass(GlobalValue::DefaultStorageClass);
  exportCollector->setLinkage(GlobalValue::InternalLinkage);
  exportCollector->setCallingConv(CallingConv::C);
  exportCollector->addFnAttr(Attribute::AlwaysInline);

  std::string postfix = "";
  if (numPositionExports > 0 && numAttributeExports > 0 && numXfbExports > 0)
    postfix = ""; // Collect all
  else if (numPositionExports > 0 && numAttributeExports > 0)
    postfix = ".vertex"; // Collect vertex (position and attribute)
  else if (numPositionExports > 0)
    postfix = ".position"; // Collect position
  else if (numAttributeExports > 0)
    postfix = ".attribute"; // Collect attribute
  else if (numXfbExports > 0)
    postfix = ".xfb"; // Collect XFB
  else
    llvm_unreachable("Unexpected collecting kind");

  exportCollector->setName(NggExportCollector + postfix);

  //
  // Collect vertex position/attribute/XFB exports.
  //
  SmallVector<NggExportPositionOp *, 4> exportPositionOps;
  SmallVector<NggExportAttributeOp *, 4> exportAttributeOps;
  SmallVector<WriteXfbOutputOp *, 4> writeXfbOutputOps;

  {
    struct Payload {
      SmallVectorImpl<NggExportPositionOp *> &exportPositionOps;
      SmallVectorImpl<NggExportAttributeOp *> &exportAttributeOps;
      SmallVectorImpl<WriteXfbOutputOp *> &writeXfbOutputOps;
    };
    Payload payload = {exportPositionOps, exportAttributeOps, writeXfbOutputOps};

    static const auto visitor =
        llvm_dialects::VisitorBuilder<Payload>()
            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
            .add<NggExportPositionOp>([](Payload &payload, NggExportPositionOp &exportPositionOp) {
              payload.exportPositionOps.push_back(&exportPositionOp);
            })
            .add<NggExportAttributeOp>([](Payload &payload, NggExportAttributeOp &exportAttributeOp) {
              payload.exportAttributeOps.push_back(&exportAttributeOp);
            })
            .add<WriteXfbOutputOp>([](Payload &payload, WriteXfbOutputOp &writeXfbOutputOp) {
              payload.writeXfbOutputOps.push_back(&writeXfbOutputOp);
            })
            .build();
    visitor.visit(payload, *exportCollector);
  }

  //
  // Construct the return value of export reader
  //
  IRBuilder<>::InsertPointGuard guard(m_builder);

  ReturnInst *retInst = nullptr;
  if (!exportPositionOps.empty())
    retInst = dyn_cast<ReturnInst>(exportPositionOps[0]->getParent()->getTerminator());
  else if (!exportAttributeOps.empty())
    retInst = dyn_cast<ReturnInst>(exportAttributeOps[0]->getParent()->getTerminator());
  else if (!writeXfbOutputOps.empty())
    retInst = dyn_cast<ReturnInst>(writeXfbOutputOps[0]->getParent()->getTerminator());
  assert(retInst);
  m_builder.SetInsertPoint(retInst);

  Value *returnValue = PoisonValue::get(returnTy);
  unsigned index = 0;
  if (numPositionExports > 0) {
    Value *positionExportValues = PoisonValue::get(positionExportsTy);
    unsigned i = 0;

    for (auto exportPositionOp : exportPositionOps) {
      std::array<Value *, 4> exportValues = {exportPositionOp->getExportValue0(), exportPositionOp->getExportValue1(),
                                             exportPositionOp->getExportValue2(), exportPositionOp->getExportValue3()};
      unsigned channelMask = 0;
      Value *positionExportValue = PoisonValue::get(exportTy);
      for (unsigned j = 0; j < 4; ++j) {
        if (!isa<UndefValue>(exportValues[j]) && !isa<PoisonValue>(exportValues[j]))
          channelMask |= (1u << j); // Update channel mask if the value is valid (not unspecified)
        positionExportValue = m_builder.CreateInsertElement(positionExportValue, exportValues[j], j);
      }

      (*positionExports)[i] = {};
      (*positionExports)[i].exportSlot = exportPositionOp->getExportSlot();
      (*positionExports)[i].channelMask = channelMask;

      positionExportValues = m_builder.CreateInsertValue(positionExportValues, positionExportValue, i);
      ++i;
    }

    returnValue = m_builder.CreateInsertValue(returnValue, positionExportValues, index++);
  }

  if (numAttributeExports > 0) {
    Value *attributeExportValues = PoisonValue::get(attributeExportsTy);
    unsigned i = 0;

    for (auto exportAttributeOp : exportAttributeOps) {
      std::array<Value *, 4> exportValues = {exportAttributeOp->getExportValue0(), exportAttributeOp->getExportValue1(),
                                             exportAttributeOp->getExportValue2(),
                                             exportAttributeOp->getExportValue3()};
      unsigned channelMask = 0;
      Value *attributeExportValue = PoisonValue::get(exportTy);
      for (unsigned j = 0; j < 4; ++j) {
        if (!isa<UndefValue>(exportValues[j]) && !isa<PoisonValue>(exportValues[j]))
          channelMask |= (1u << j); // Update channel mask if the value is valid (not unspecified)
        attributeExportValue = m_builder.CreateInsertElement(attributeExportValue, exportValues[j], j);
      }
      (*attributeExports)[i] = {};
      (*attributeExports)[i].exportSlot = exportAttributeOp->getExportSlot();
      (*attributeExports)[i].channelMask = channelMask;

      attributeExportValues = m_builder.CreateInsertValue(attributeExportValues, attributeExportValue, i);
      ++i;
    }

    returnValue = m_builder.CreateInsertValue(returnValue, attributeExportValues, index++);
  }

  if (numXfbExports > 0) {
    Value *xfbExportValues = PoisonValue::get(xfbExportsTy);
    unsigned i = 0;
    unsigned offsetInVertex = 0;

    for (auto writeXfbOutputOp : writeXfbOutputOps) {
      auto outputValue = writeXfbOutputOp->getOutputValue();
      auto outputTy = outputValue->getType();
      assert(outputTy->getScalarSizeInBits() == 32);
      unsigned numElements = outputTy->isVectorTy() ? cast<FixedVectorType>(outputTy)->getNumElements() : 1;
      assert(numElements <= 4);

      (*xfbExports)[i] = {};
      (*xfbExports)[i].xfbBuffer = writeXfbOutputOp->getXfbBuffer();
      (*xfbExports)[i].xfbOffset = writeXfbOutputOp->getXfbOffset();
      (*xfbExports)[i].numElements = numElements;

      if (m_hasGs) {
        // NOTE: For GS, the output value must be loaded by NggReadGsOutputOp. This is generated by copy shader.
        NggReadGsOutputOp *readGsOutputOp = dyn_cast<NggReadGsOutputOp>(outputValue);
        assert(readGsOutputOp->getStreamId() == writeXfbOutputOp->getStreamId()); // Stream IDs must match

        (*xfbExports)[i].locInfo.streamId = writeXfbOutputOp->getStreamId();
        (*xfbExports)[i].locInfo.location = readGsOutputOp->getLocation();
        (*xfbExports)[i].locInfo.component = readGsOutputOp->getComponent();

        ++i;
        continue;
      }

      if (outputTy->isIntOrIntVectorTy()) {
        if (numElements == 1) {
          outputValue = m_builder.CreateBitCast(outputValue, m_builder.getFloatTy());
        } else {
          outputValue = m_builder.CreateBitCast(outputValue, FixedVectorType::get(m_builder.getFloatTy(), numElements));
        }
      }

      // Always pad the write value to <4 x float>
      Value *xfbExportValue = outputValue;
      if (numElements == 1) {
        xfbExportValue =
            m_builder.CreateInsertElement(PoisonValue::get(exportTy), outputValue, static_cast<uint64_t>(0));
      } else if (numElements < 4) {
        xfbExportValue = m_builder.CreateShuffleVector(outputValue, PoisonValue::get(outputValue->getType()),
                                                       ArrayRef<int>({0U, 1U, 2U, 3U}));
      }

      (*xfbExports)[i].offsetInVertex = offsetInVertex;
      offsetInVertex += numElements; // Increment the offset

      xfbExportValues = m_builder.CreateInsertValue(xfbExportValues, xfbExportValue, i);
      ++i;
    }

    returnValue = m_builder.CreateInsertValue(returnValue, xfbExportValues, index++);
  }
  m_builder.CreateRet(returnValue);

  // Remove original return instruction
  retInst->eraseFromParent();
  retInst = nullptr;

  //
  // Clean-up
  //
  for (auto exportPositionOp : exportPositionOps) {
    exportPositionOp->dropAllReferences();
    exportPositionOp->eraseFromParent();
  }

  for (auto exportAttributeOp : exportAttributeOps) {
    exportAttributeOp->dropAllReferences();
    exportAttributeOp->eraseFromParent();
  }

  for (auto writeXfbOutputOp : writeXfbOutputOps) {
    writeXfbOutputOp->dropAllReferences();
    writeXfbOutputOp->eraseFromParent();
  }

  return exportCollector;
}

// =====================================================================================================================
// Collect exports from the specified function by making an export collector (either clone it or mutate it), calling it,
// and analyzing its return value.
//
// @param args : Arguments of primitive shader entry-point
// @param [in/out] fromFunc : Function from which to collect exports
// @param makeClone : Whether to make a clone of the specified function
// @param [out] positionExports : Collection of position exports
// @param [out] attributeExports : Collection of attribute exports
// @param [out] xfbExports : Collection of XFB exports
void NggPrimShader::collectExports(ArrayRef<Argument *> args, Function *&fromFunc, bool makeClone,
                                   SmallVectorImpl<VertexExport> *positionExports,
                                   SmallVectorImpl<VertexExport> *attributeExports,
                                   SmallVectorImpl<XfbExport> *xfbExports) {
  const bool collectPositionExports = positionExports != nullptr;
  const bool collectAttributeExports = attributeExports != nullptr;
  const bool collectXfbExports = m_pipelineState->enableSwXfb() && xfbExports != nullptr;
  assert(collectPositionExports || collectAttributeExports || collectXfbExports);

  //
  // Mutate the specified function to an export collector
  //
  auto exportCollector = makeExportCollector(fromFunc, makeClone, positionExports, attributeExports, xfbExports);
  if (!exportCollector)
    return; // No export to collect

  //
  // Run the export collector to collect exports
  //
  SmallVector<Value *, 32> exportCollectorArgs;

  if (m_hasGs) {
    // The export collector is derived from copy shader
    Value *vertexIndex = m_nggInputs.threadIdInSubgroup;
    if (m_compactVertex) {
      auto collectExportBlock = m_builder.GetInsertBlock();

      auto uncompactVertexIndexBlock = createBlock(collectExportBlock->getParent(), ".uncompactVertexIndex");
      uncompactVertexIndexBlock->moveAfter(collectExportBlock);

      auto endUncompactVertexIndexBlock = createBlock(collectExportBlock->getParent(), ".endUncompactVertexIndex");
      endUncompactVertexIndexBlock->moveAfter(uncompactVertexIndexBlock);

      m_builder.CreateCondBr(m_compactVertex, uncompactVertexIndexBlock, endUncompactVertexIndexBlock);

      // Construct ".uncompactVertexIndex" block
      Value *uncompactedVertexIndex = nullptr;
      {
        m_builder.SetInsertPoint(uncompactVertexIndexBlock);

        uncompactedVertexIndex = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                          PrimShaderLdsRegion::VertexIndexMap);

        m_builder.CreateBr(endUncompactVertexIndexBlock);
      }

      // Construct ".endUncompactVertexIndex" block
      {
        m_builder.SetInsertPoint(endUncompactVertexIndexBlock);

        vertexIndex = createPhi({{uncompactedVertexIndex, uncompactVertexIndexBlock},
                                 {vertexIndex, uncompactVertexIndexBlock->getSinglePredecessor()}});
      }
    }

    exportCollectorArgs.push_back(vertexIndex); // Only one argument
  } else {
    // The export collector is derived from ES
    Value *offChipLdsBase = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase)];
    offChipLdsBase->setName("offChipLdsBase");

    Value *userData = args[NumSpecialSgprInputs];

    ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

    Value *tessCoordX = nullptr;
    Value *tessCoordY = nullptr;
    Value *relPatchId = nullptr;
    Value *patchId = nullptr;

    Value *vertexId = nullptr;
    Value *relVertexId = PoisonValue::get(m_builder.getInt32Ty()); // Unused
    // NOTE: VS primitive ID for NGG is specially obtained from primitive ID distribution.
    Value *vsPrimitiveId =
        m_distributedPrimitiveId ? m_distributedPrimitiveId : PoisonValue::get(m_builder.getInt32Ty());
    Value *instanceId = nullptr;

    if (m_gfxIp.major <= 11) {
      if (m_hasTes) {
        tessCoordX = vgprArgs[5];
        tessCoordY = vgprArgs[6];
        relPatchId = vgprArgs[7];
        patchId = vgprArgs[8];
      } else {
        vertexId = vgprArgs[5];
        instanceId = vgprArgs[8];
      }
    } else {
#if LLPC_BUILD_GFX12
      if (m_hasTes) {
        tessCoordX = vgprArgs[3];
        tessCoordY = vgprArgs[4];
        relPatchId = vgprArgs[5];
        patchId = vgprArgs[6];
      } else {
        vertexId = vgprArgs[3];
        instanceId = vgprArgs[4];
      }
#else
      llvm_unreachable("Not implemented!");
#endif
    }

    if (m_compactVertex) {
      auto collectExportBlock = m_builder.GetInsertBlock();

      auto uncompactVertexBlock = createBlock(collectExportBlock->getParent(), ".uncompactVertex");
      uncompactVertexBlock->moveAfter(collectExportBlock);

      auto endUncompactVertexBlock = createBlock(collectExportBlock->getParent(), ".endUncompactVertex");
      endUncompactVertexBlock->moveAfter(uncompactVertexBlock);

      m_builder.CreateCondBr(m_compactVertex, uncompactVertexBlock, endUncompactVertexBlock);

      // Construct ".uncompactVertex" block
      Value *newTessCoordX = nullptr;
      Value *newTessCoordY = nullptr;
      Value *newRelPatchId = nullptr;
      Value *newPatchId = nullptr;
      Value *newVertexId = nullptr;
      Value *newVsPrimitiveId = nullptr;
      Value *newInstanceId = nullptr;
      {
        m_builder.SetInsertPoint(uncompactVertexBlock);

        const unsigned esGsRingItemSize =
            m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize;

        auto uncompactedVertexIndex = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                               PrimShaderLdsRegion::VertexIndexMap);
        auto vertexItemOffset = m_builder.CreateMul(uncompactedVertexIndex, m_builder.getInt32(esGsRingItemSize));

        // NOTE: If vertex compaction, some system values could be from vertex compaction info rather than from VGPRs
        // (caused by NGG culling and vertex compaction)
        const auto resUsage =
            m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex);
        if (m_hasTes) {
          if (resUsage->builtInUsage.tes.tessCoord) {
            newTessCoordX =
                readVertexCullInfoFromLds(m_builder.getFloatTy(), vertexItemOffset, m_vertCullInfoOffsets.tessCoordX);
            newTessCoordY =
                readVertexCullInfoFromLds(m_builder.getFloatTy(), vertexItemOffset, m_vertCullInfoOffsets.tessCoordY);
          }

          newRelPatchId =
              readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.relPatchId);

          if (resUsage->builtInUsage.tes.primitiveId) {
            newPatchId =
                readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.patchId);
          }
        } else {
          if (resUsage->builtInUsage.vs.vertexIndex) {
            newVertexId =
                readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.vertexId);
          }

          // NOTE: Relative vertex index provided by HW is not used when VS is merged to GS.

          if (resUsage->builtInUsage.vs.primitiveId) {
            newVsPrimitiveId =
                readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.primitiveId);
          }

          if (resUsage->builtInUsage.vs.instanceIndex) {
            newInstanceId =
                readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset, m_vertCullInfoOffsets.instanceId);
          }
        }
        m_builder.CreateBr(endUncompactVertexBlock);
      }

      // Construct ".endUncompactVertex" block
      {
        m_builder.SetInsertPoint(endUncompactVertexBlock);

        if (m_hasTes) {
          if (newTessCoordX)
            tessCoordX = createPhi(
                {{newTessCoordX, uncompactVertexBlock}, {tessCoordX, uncompactVertexBlock->getSinglePredecessor()}});

          if (newTessCoordY)
            tessCoordY = createPhi(
                {{newTessCoordY, uncompactVertexBlock}, {tessCoordY, uncompactVertexBlock->getSinglePredecessor()}});

          assert(newRelPatchId);
          relPatchId = createPhi(
              {{newRelPatchId, uncompactVertexBlock}, {relPatchId, uncompactVertexBlock->getSinglePredecessor()}});

          if (newPatchId)
            patchId = createPhi(
                {{newPatchId, uncompactVertexBlock}, {patchId, uncompactVertexBlock->getSinglePredecessor()}});
        } else {
          if (newVertexId)
            vertexId = createPhi(
                {{newVertexId, uncompactVertexBlock}, {vertexId, uncompactVertexBlock->getSinglePredecessor()}});

          if (newVsPrimitiveId)
            vsPrimitiveId = createPhi({{newVsPrimitiveId, uncompactVertexBlock},
                                       {vsPrimitiveId, uncompactVertexBlock->getSinglePredecessor()}});

          if (newInstanceId)
            instanceId = createPhi(
                {{newInstanceId, uncompactVertexBlock}, {instanceId, uncompactVertexBlock->getSinglePredecessor()}});
        }
      }
    }

    // Set up user data SGPRs
    const unsigned userDataCount =
        m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex)->userDataCount;
    appendUserData(exportCollectorArgs, exportCollector, userData, userDataCount);

    if (m_hasTes) {
      // Set up system value SGPRs
      exportCollectorArgs.push_back(offChipLdsBase);

      // Set up system value VGPRs
      exportCollectorArgs.push_back(tessCoordX);
      exportCollectorArgs.push_back(tessCoordY);
      exportCollectorArgs.push_back(relPatchId);
      exportCollectorArgs.push_back(patchId);
    } else {
      // Set up system value VGPRs
      exportCollectorArgs.push_back(vertexId);
      exportCollectorArgs.push_back(relVertexId);
      exportCollectorArgs.push_back(vsPrimitiveId);
      exportCollectorArgs.push_back(instanceId);
    }
  }

  assert(exportCollectorArgs.size() == exportCollector->arg_size()); // Must have visit all arguments of export reader
  auto returnValue = callFunctionHelper(exportCollector, exportCollectorArgs, m_builder.GetInsertBlock());

  //
  // Analyze the return value to extract export values
  //
  unsigned index = 0;

  const unsigned numPositionExports = collectPositionExports ? positionExports->size() : 0;
  const unsigned numAttributeExports = collectAttributeExports ? attributeExports->size() : 0;
  const unsigned numXfbExports = collectXfbExports ? xfbExports->size() : 0;

  if (numPositionExports > 0) {
    auto positionExportValues = m_builder.CreateExtractValue(returnValue, index++);
    assert(positionExportValues->getType()->isArrayTy());
    assert(numPositionExports == positionExportValues->getType()->getArrayNumElements()); // Sizes must match

    for (unsigned i = 0; i < numPositionExports; ++i)
      (*positionExports)[i].exportValue = m_builder.CreateExtractValue(positionExportValues, i);
  }

  if (numAttributeExports > 0) {
    auto attributeExportValues = m_builder.CreateExtractValue(returnValue, index++);
    assert(attributeExportValues->getType()->isArrayTy());
    assert(numAttributeExports == attributeExportValues->getType()->getArrayNumElements()); // Sizes must match

    for (unsigned i = 0; i < numAttributeExports; ++i)
      (*attributeExports)[i].exportValue = m_builder.CreateExtractValue(attributeExportValues, i);
  }

  if (numXfbExports > 0) {
    auto xfbExportValues = m_builder.CreateExtractValue(returnValue, index++);
    assert(xfbExportValues->getType()->isArrayTy());
    assert(numXfbExports == xfbExportValues->getType()->getArrayNumElements()); // Sizes must match

    for (unsigned i = 0; i < numXfbExports; ++i) {
      auto xfbExportValue = m_builder.CreateExtractValue(xfbExportValues, i);
      if ((*xfbExports)[i].numElements == 1) {
        (*xfbExports)[i].exportValue = m_builder.CreateExtractElement(xfbExportValue, static_cast<uint64_t>(0));
      } else {
        SmallVector<int, 8> shuffleMask;
        for (unsigned j = 0; j < (*xfbExports)[i].numElements; ++j)
          shuffleMask.push_back(j);
        (*xfbExports)[i].exportValue = m_builder.CreateShuffleVector(xfbExportValue, xfbExportValue, shuffleMask);
      }
    }
  }
}

// =====================================================================================================================
// Create PHI node for the export values of the export collections.
//
// @param [out] positionExports : Collection of position exports
// @param [out] attributeExports : Collection of attribute exports
// @param [out] xfbExports : Collection of XFB exports
void NggPrimShader::createPhiForExports(SmallVectorImpl<VertexExport> *positionExports,
                                        SmallVectorImpl<VertexExport> *attributeExports,
                                        SmallVectorImpl<XfbExport> *xfbExports) {
  auto currentBlock = m_builder.GetInsertBlock();

  if (positionExports) {
    for (auto &positionExport : *positionExports) {
      auto &exportValue = positionExport.exportValue;
      auto exportBlock = cast<Instruction>(exportValue)->getParent();
      std::string valueName = "position" + std::to_string(positionExport.exportSlot);

      auto exportValuePhi = m_builder.CreatePHI(exportValue->getType(), pred_size(currentBlock), valueName);
      for (BasicBlock *predBlock : predecessors(currentBlock)) {
        exportValuePhi->addIncoming(predBlock == exportBlock ? exportValue : PoisonValue::get(exportValue->getType()),
                                    predBlock);
      }
      exportValue = exportValuePhi;
    }
  }

  if (attributeExports) {
    for (auto &attributeExport : *attributeExports) {
      auto &exportValue = attributeExport.exportValue;
      auto exportBlock = cast<Instruction>(exportValue)->getParent();
      std::string valueName = "attribute" + std::to_string(attributeExport.exportSlot);

      auto exportValuePhi = m_builder.CreatePHI(exportValue->getType(), pred_size(currentBlock), valueName);
      for (BasicBlock *predBlock : predecessors(currentBlock)) {
        exportValuePhi->addIncoming(predBlock == exportBlock ? exportValue : PoisonValue::get(exportValue->getType()),
                                    predBlock);
      }
      exportValue = exportValuePhi;
    }
  }

  if (xfbExports && m_pipelineState->enableSwXfb()) {
    for (auto &xfbExport : *xfbExports) {
      auto &exportValue = xfbExport.exportValue;
      auto exportBlock = cast<Instruction>(exportValue)->getParent();
      std::string valueName =
          "xfb.buffer" + std::to_string(xfbExport.xfbBuffer) + ".offset" + std::to_string(xfbExport.xfbOffset);
      switch (xfbExport.numElements) {
      case 1:
        valueName += ".x";
        break;
      case 2:
        valueName += ".xy";
        break;
      case 3:
        valueName += ".xyz";
        break;
      case 4:
        valueName += ".xyzw";
        break;
      default:
        llvm_unreachable("Unexpected number of elements");
        break;
      }
      auto exportValuePhi = m_builder.CreatePHI(exportValue->getType(), pred_size(currentBlock), valueName);
      for (BasicBlock *predBlock : predecessors(currentBlock)) {
        exportValuePhi->addIncoming(predBlock == exportBlock ? exportValue : PoisonValue::get(exportValue->getType()),
                                    predBlock);
      }
      exportValue = exportValuePhi;
    }
  }
}

// =====================================================================================================================
// Export positions.
//
// @param positionExports : Input collection of position exports
void NggPrimShader::exportPositions(const SmallVectorImpl<VertexExport> &positionExports) {
  CallInst *lastExport = nullptr;
  for (auto &positionExport : positionExports) {
    std::array<Value *, 4> exportValues;
    for (unsigned i = 0; i < 4; ++i)
      exportValues[i] = m_builder.CreateExtractElement(positionExport.exportValue, i);

    lastExport = m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                           {m_builder.getInt32(EXP_TARGET_POS_0 + positionExport.exportSlot), // tgt
                                            m_builder.getInt32(positionExport.channelMask),                   // en
                                            exportValues[0],                                                  // src0
                                            exportValues[1],                                                  // src1
                                            exportValues[2],                                                  // src2
                                            exportValues[3],                                                  // src3
                                            m_builder.getFalse(),                                             // done
                                            m_builder.getFalse()});                                           // vm
  }

  if (lastExport)
    lastExport->setArgOperand(6, m_builder.getTrue()); // Set Done flag
}

// =====================================================================================================================
// Export attributes.
//
// @param attributeExports : Input collection of attribute exports
void NggPrimShader::exportAttributes(const SmallVectorImpl<VertexExport> &attributeExports) {
  for (auto &attributeExport : attributeExports) {
    if (m_pipelineState->exportAttributeByExportInstruction()) {
      std::array<Value *, 4> exportValues;
      for (unsigned i = 0; i < 4; ++i)
        exportValues[i] = m_builder.CreateExtractElement(attributeExport.exportValue, i);

      m_builder.CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder.getFloatTy(),
                                {m_builder.getInt32(EXP_TARGET_PARAM_0 + attributeExport.exportSlot), // tgt
                                 m_builder.getInt32(attributeExport.channelMask),                     // en
                                 exportValues[0],                                                     // src0
                                 exportValues[1],                                                     // src1
                                 exportValues[2],                                                     // src2
                                 exportValues[3],                                                     // src3
                                 m_builder.getFalse(),                                                // done
                                 m_builder.getFalse()});                                              // vm
    } else {
      auto attributeOffset = m_builder.getInt32(attributeExport.exportSlot * SizeOfVec4);

      CoherentFlag coherent = {};
      if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
        coherent.bits.glc = true;
#if LLPC_BUILD_GFX12
      } else {
        coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
        coherent.gfx12.th = TH::TH_NT_WB;

        unsigned cachePolicy = m_pipelineState->getOptions().cacheScopePolicyControl;
        if (cachePolicy & CacheScopePolicyType::AtmWriteUseSystemScope) {
          coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_SYS;
          coherent.gfx12.th = TH::TH_WB;
        }
        coherent.gfx12.th =
            m_pipelineState->getTemporalHint(coherent.gfx12.th, TemporalHintOpType::TemporalHintAtmWrite);
#endif
      }

      m_builder.CreateIntrinsic(m_builder.getVoidTy(), Intrinsic::amdgcn_struct_buffer_store,
                                {attributeExport.exportValue, m_attribRingBufDesc, m_nggInputs.threadIdInSubgroup,
                                 attributeOffset, m_attribRingBaseOffset, m_builder.getInt32(coherent.u32All)});
    }
  }
}

// =====================================================================================================================
// Processes SW emulated XFB when API GS is not present.
//
// @param args : Arguments of primitive shader entry-point
// @param xfbExports : Input collection of XFB exports
void NggPrimShader::processSwXfb(ArrayRef<Argument *> args, const SmallVectorImpl<XfbExport> &xfbExports) {
  assert(m_pipelineState->enableSwXfb());
  assert(!m_hasGs); // API GS is not present

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();

  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i)
    bufferActive[i] = xfbStrides[i] > 0;

  //
  // The processing is something like this:
  //
  // NGG_XFB() {
  //   if (threadIdInSubgroup < vertCountInSubgroup)
  //     Write XFB to LDS
  //
  //   Prepare XFB to update its relevant counters
  //   Barrier
  //
  //   Read XFB statistics info from LDS
  //   Read primsToWrite and dwordsWritten from XFB statistics info
  //
  //   if (threadIdInSubgroup < primsToWrite)
  //     Export XFB to buffer for each vertice of this primitive
  // }
  //
  BasicBlock *xfbEntryBlock = m_builder.GetInsertBlock();

  BasicBlock *writeXfbBlock = createBlock(xfbEntryBlock->getParent(), ".writeXfb");
  writeXfbBlock->moveAfter(xfbEntryBlock);
  BasicBlock *endWriteXfbBlock = createBlock(xfbEntryBlock->getParent(), ".endWriteXfb");
  endWriteXfbBlock->moveAfter(writeXfbBlock);

  unsigned possibleVertsPerPrim = 3;
  if (isa<ConstantInt>(m_verticesPerPrimitive))
    possibleVertsPerPrim = cast<ConstantInt>(m_verticesPerPrimitive)->getZExtValue();

  BasicBlock *exportXfbBlock[3] = {};
  auto insertPos = endWriteXfbBlock;
  for (unsigned i = 0; i < possibleVertsPerPrim; ++i) {
    exportXfbBlock[i] = createBlock(xfbEntryBlock->getParent(), ".exportXfbInVertex" + std::to_string(i));
    exportXfbBlock[i]->moveAfter(insertPos);
    insertPos = exportXfbBlock[i];
  }

  BasicBlock *endExportXfbBlock = createBlock(xfbEntryBlock->getParent(), ".endExportXfb");
  endExportXfbBlock->moveAfter(insertPos);

  // Insert branching in current block to process XFB
  {
    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, writeXfbBlock, endWriteXfbBlock);
  }

  // Construct ".writeXfb" block
  {
    m_builder.SetInsertPoint(writeXfbBlock);

    for (unsigned i = 0; i < xfbExports.size(); ++i)
      writeXfbOutputToLds(xfbExports[i].exportValue, m_nggInputs.threadIdInSubgroup,
                          xfbExports[i].offsetInVertex); // Write XFB to LDS

    m_builder.CreateBr(endWriteXfbBlock);
  }

  // Construct ".endWriteXfb" block
  Value *streamOutOffsets[MaxTransformFeedbackBuffers] = {}; // Stream-out offset to write XFB outputs
  {
    m_builder.SetInsertPoint(endWriteXfbBlock);

    prepareSwXfb({m_nggInputs.primCountInSubgroup});

    // We are going to read XFB statistics info and outputs from LDS and export them to transform
    // feedback buffers. Make all values have been written before this.
    createFenceAndBarrier();

    auto xfbStatInfo =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, PrimShaderLdsRegion::XfbStats);
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (bufferActive[i]) {
        streamOutOffsets[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                        {xfbStatInfo, m_builder.getInt32(i)});
        streamOutOffsets[i] = m_builder.CreateAdd(m_streamOutBufOffsets[i], streamOutOffsets[i]);
        streamOutOffsets[i] = m_builder.CreateShl(streamOutOffsets[i], 2);
      }
    }
    auto numPrimsToWrite = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                     {xfbStatInfo, m_builder.getInt32(MaxTransformFeedbackBuffers)});

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite);
    m_builder.CreateCondBr(validPrimitive, exportXfbBlock[0], endExportXfbBlock);
  }

  Value *vertexIndices[3] = {};
  vertexIndices[0] = m_nggInputs.vertexIndex0;
  vertexIndices[1] = m_nggInputs.vertexIndex1;
  vertexIndices[2] = m_nggInputs.vertexIndex2;

  for (unsigned i = 0; i < possibleVertsPerPrim; ++i) {
    // Construct ".exportXfbInVertex[N]" block
    m_builder.SetInsertPoint(exportXfbBlock[i]);

    for (unsigned j = 0; j < xfbExports.size(); ++j) {
      const auto &xfbExport = xfbExports[j];
      auto exportValue = readXfbOutputFromLds(xfbExport.numElements > 1
                                                  ? FixedVectorType::get(m_builder.getFloatTy(), xfbExport.numElements)
                                                  : m_builder.getFloatTy(),
                                              vertexIndices[i], xfbExport.offsetInVertex);

      unsigned format = 0;
      switch (xfbExport.numElements) {
      case 1:
        format = BUF_FORMAT_32_FLOAT;
        break;
      case 2:
        format = BUF_FORMAT_32_32_FLOAT_GFX11;
        break;
      case 3:
        format = BUF_FORMAT_32_32_32_FLOAT_GFX11;
        break;
      case 4:
        format = BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
        break;
      default:
        llvm_unreachable("Unexpected element number!");
        break;
      }

      CoherentFlag coherent = {};
      if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
        coherent.bits.glc = true;
        coherent.bits.slc = true;
#if LLPC_BUILD_GFX12
      } else {
        coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
        coherent.gfx12.th = TH::TH_HT;
#endif
      }

      // vertexOffset = (threadIdInSubgroup * vertsPerPrim + vertexIndex) * xfbStride
      Value *vertexOffset = m_builder.CreateAdd(
          m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_verticesPerPrimitive), m_builder.getInt32(i));
      vertexOffset = m_builder.CreateMul(vertexOffset, m_builder.getInt32(xfbStrides[xfbExport.xfbBuffer]));
      // xfbOutputOffset = vertexOffset + xfbOffset
      Value *xfbOutputOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(xfbExport.xfbOffset));

      m_builder.CreateIntrinsic(m_builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store,
                                {exportValue,                              // vdata
                                 m_streamOutBufDescs[xfbExport.xfbBuffer], // rsrc
                                 xfbOutputOffset,                          // offset
                                 streamOutOffsets[xfbExport.xfbBuffer],    // soffset
                                 m_builder.getInt32(format),               // format
                                 m_builder.getInt32(coherent.u32All)});    // auxiliary data
    }

    if (i == possibleVertsPerPrim - 1) {
      // Last vertex
      m_builder.CreateBr(endExportXfbBlock);
    } else {
      // Not last vertex, check if we need to export outputs of next vertex
      auto exportNextVertex = m_builder.CreateICmpUGT(m_verticesPerPrimitive, m_builder.getInt32(i + 1));
      m_builder.CreateCondBr(exportNextVertex, exportXfbBlock[i + 1], endExportXfbBlock);
    }
  }

  // Construct ".endExportXfb" block
  { m_builder.SetInsertPoint(endExportXfbBlock); }
}

// =====================================================================================================================
// Process SW emulated XFB when API GS is present.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::processSwXfbWithGs(ArrayRef<Argument *> args, const SmallVectorImpl<XfbExport> &xfbExports) {
  assert(m_pipelineState->enableSwXfb());
  assert(m_hasGs); // GS is present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStage::Geometry);
  assert(waveSize == 32 || waveSize == 64);

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();

  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i)
    bufferActive[i] = xfbStrides[i] > 0;

  unsigned firstActiveStream = InvalidValue;
  unsigned lastActiveStream = InvalidValue;

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!m_pipelineState->isVertexStreamActive(i))
      continue; // Stream is inactive

    if (firstActiveStream == InvalidValue)
      firstActiveStream = i;
    lastActiveStream = i;
  }

  //
  // The processing is something like this:
  //
  // NGG_GS_XFB() {
  //   if (threadIdInSubgroup < maxWaves + 1)
  //     Initialize per-wave and per-subgroup count of output primitives
  //   Barrier
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Check the draw flag of output primitives and compute draw mask
  //
  //   if (threadIdInWave < maxWaves - waveId)
  //     Accumulate per-wave and per-subgroup count of output primitives
  //   Barrier
  //
  //   for (each vertex stream) {
  //     if (primitive drawn)
  //       Compact primitive index (compacted -> uncompacted)
  //   }
  //
  //   Prepare XFB and update its relevant counters
  //   Barrier
  //
  //   Read XFB statistics info from LDS
  //   Read primsToWrite and dwordsWritten from XFB statistics info
  //
  //   for each vertex stream {
  //     if (threadIdInSubgroup < primsToWrite)
  //       Export XFB to buffer for each vertice of this primitive
  //   }
  // }
  //
  BasicBlock *xfbEntryBlock = m_builder.GetInsertBlock();

  BasicBlock *initPrimitiveCountsBlock = createBlock(xfbEntryBlock->getParent(), ".initPrimitiveCounts");
  initPrimitiveCountsBlock->moveAfter(xfbEntryBlock);
  BasicBlock *endInitPrimitiveCountsBlock = createBlock(xfbEntryBlock->getParent(), ".endInitPrimitiveCounts");
  endInitPrimitiveCountsBlock->moveAfter(initPrimitiveCountsBlock);

  BasicBlock *checkPrimitiveDrawFlagBlock = createBlock(xfbEntryBlock->getParent(), ".checkPrimitiveDrawFlag");
  checkPrimitiveDrawFlagBlock->moveAfter(endInitPrimitiveCountsBlock);
  BasicBlock *endCheckPrimitiveDrawFlagBlock = createBlock(xfbEntryBlock->getParent(), ".endCheckPrimitiveDrawFlag");
  endCheckPrimitiveDrawFlagBlock->moveAfter(checkPrimitiveDrawFlagBlock);

  BasicBlock *accumPrimitiveCountsBlock = createBlock(xfbEntryBlock->getParent(), ".accumPrimitiveCounts");
  accumPrimitiveCountsBlock->moveAfter(endCheckPrimitiveDrawFlagBlock);
  BasicBlock *endAccumPrimitiveCountsBlock = createBlock(xfbEntryBlock->getParent(), ".endAccumPrimitiveCounts");
  endAccumPrimitiveCountsBlock->moveAfter(accumPrimitiveCountsBlock);

  BasicBlock *compactPrimitiveIndexBlock[MaxGsStreams] = {};
  BasicBlock *endCompactPrimitiveIndexBlock[MaxGsStreams] = {};
  BasicBlock *insertPos = endAccumPrimitiveCountsBlock;
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (m_pipelineState->isVertexStreamActive(i)) {
      compactPrimitiveIndexBlock[i] =
          createBlock(xfbEntryBlock->getParent(), ".compactPrimitiveIndexInStream" + std::to_string(i));
      compactPrimitiveIndexBlock[i]->moveAfter(insertPos);
      insertPos = compactPrimitiveIndexBlock[i];

      endCompactPrimitiveIndexBlock[i] =
          createBlock(xfbEntryBlock->getParent(), ".endCompactPrimitiveIndexInStream" + std::to_string(i));
      endCompactPrimitiveIndexBlock[i]->moveAfter(insertPos);
      insertPos = endCompactPrimitiveIndexBlock[i];
    }
  }

  BasicBlock *exportXfbBlock[MaxGsStreams] = {};
  BasicBlock *endExportXfbBlock[MaxGsStreams] = {};
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (m_pipelineState->isVertexStreamActive(i)) {
      exportXfbBlock[i] = createBlock(xfbEntryBlock->getParent(), ".exportXfbInStream" + std::to_string(i));
      exportXfbBlock[i]->moveAfter(insertPos);
      insertPos = exportXfbBlock[i];

      endExportXfbBlock[i] = createBlock(xfbEntryBlock->getParent(), ".endExportXfbInStream" + std::to_string(i));
      endExportXfbBlock[i]->moveAfter(insertPos);
      insertPos = endExportXfbBlock[i];
    }
  }

  // Insert branching in current block to process XFB
  {
    auto validWave =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(m_maxWavesPerSubgroup + 1));
    m_builder.CreateCondBr(validWave, initPrimitiveCountsBlock, endInitPrimitiveCountsBlock);
  }

  // Construct ".initPrimitiveCounts" block
  {
    m_builder.SetInsertPoint(initPrimitiveCountsBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup,
                                PrimShaderLdsRegion::PrimitiveCounts, (m_maxWavesPerSubgroup + 1) * i);
      }
    }

    m_builder.CreateBr(endInitPrimitiveCountsBlock);
  }

  // Construct ".endInitPrimitiveCounts" block
  {
    m_builder.SetInsertPoint(endInitPrimitiveCountsBlock);

    createFenceAndBarrier();

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, checkPrimitiveDrawFlagBlock, endCheckPrimitiveDrawFlagBlock);
  }

  // Construct ".checkPrimitiveDrawFlag" block
  Value *drawFlag[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(checkPrimitiveDrawFlagBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        // drawFlag = primData[N] != NullPrim
        auto primData = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                 PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * i);
        drawFlag[i] = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));
      }
    }

    m_builder.CreateBr(endCheckPrimitiveDrawFlagBlock);
  }

  // Construct ".endCheckPrimitiveDrawFlag" block
  Value *drawMask[MaxGsStreams] = {};
  Value *primCountInWave[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endCheckPrimitiveDrawFlagBlock);

    // Update draw flags
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        drawFlag[i] = createPhi(
            {{drawFlag[i], checkPrimitiveDrawFlagBlock}, {m_builder.getFalse(), endInitPrimitiveCountsBlock}});
      }
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        drawMask[i] = ballot(drawFlag[i]);

        primCountInWave[i] = m_builder.CreateIntrinsic(Intrinsic::ctpop, m_builder.getInt64Ty(), drawMask[i]);
        primCountInWave[i] = m_builder.CreateTrunc(primCountInWave[i], m_builder.getInt32Ty());
      }
    }
    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(m_maxWavesPerSubgroup), m_nggInputs.waveIdInSubgroup);
    auto validThread = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);

    m_builder.CreateCondBr(validThread, accumPrimitiveCountsBlock, endAccumPrimitiveCountsBlock);
  }

  // Construct ".accumPrimitiveCounts" block
  {
    m_builder.SetInsertPoint(accumPrimitiveCountsBlock);

    unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::PrimitiveCounts);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        atomicOp(AtomicRMWInst::Add, primCountInWave[i],
                 m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + (m_maxWavesPerSubgroup + 1) * i)));
      }
    }

    m_builder.CreateBr(endAccumPrimitiveCountsBlock);
  }

  // Construct ".endAccumPrimitiveCounts" block
  Value *primCountInPrevWaves[MaxGsStreams] = {};
  Value *primCountInSubgroup[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endAccumPrimitiveCountsBlock);

    createFenceAndBarrier();

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (!m_pipelineState->isVertexStreamActive(i))
        continue;

      auto primCountInWaves =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave,
                                   PrimShaderLdsRegion::PrimitiveCounts, (m_maxWavesPerSubgroup + 1) * i);

      // The last dword following dwords for all waves (each wave has one dword) stores GS output primitive count of
      // the entire subgroup
      primCountInSubgroup[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                         {primCountInWaves, m_builder.getInt32(m_maxWavesPerSubgroup)});

      // Get output primitive count for all waves prior to this wave
      primCountInPrevWaves[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                          {primCountInWaves, m_nggInputs.waveIdInSubgroup});
    }

    m_builder.CreateCondBr(drawFlag[firstActiveStream], compactPrimitiveIndexBlock[firstActiveStream],
                           endCompactPrimitiveIndexBlock[firstActiveStream]);
  }

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!m_pipelineState->isVertexStreamActive(i))
      continue;

    // Construct ".compactPrimitiveIndexInStream[N]" block
    {
      m_builder.SetInsertPoint(compactPrimitiveIndexBlock[i]);

      auto drawMaskVec = m_builder.CreateBitCast(drawMask[i], FixedVectorType::get(m_builder.getInt32Ty(), 2));

      auto drawMaskLow = m_builder.CreateExtractElement(drawMaskVec, static_cast<uint64_t>(0));
      Value *compactedPrimitiveIndex =
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {drawMaskLow, m_builder.getInt32(0)});

      if (waveSize == 64) {
        auto drawMaskHigh = m_builder.CreateExtractElement(drawMaskVec, 1);
        compactedPrimitiveIndex =
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {drawMaskHigh, compactedPrimitiveIndex});
      }

      compactedPrimitiveIndex = m_builder.CreateAdd(primCountInPrevWaves[i], compactedPrimitiveIndex);
      writePerThreadDataToLds(m_nggInputs.threadIdInSubgroup, compactedPrimitiveIndex,
                              PrimShaderLdsRegion::PrimitiveIndexMap, m_maxThreadsPerSubgroup * i);

      m_builder.CreateBr(endCompactPrimitiveIndexBlock[i]);
    }

    // Construct ".endCompactPrimitiveIndexInStream[N]" block
    {
      m_builder.SetInsertPoint(endCompactPrimitiveIndexBlock[i]);

      if (i != lastActiveStream) {
        // Start to prepare XFB after we finish compacting primitive index of the last vertex stream.
        unsigned nextActiveStream = i + 1;
        while (!m_pipelineState->isVertexStreamActive(nextActiveStream)) {
          ++nextActiveStream;
        }

        assert(nextActiveStream <= lastActiveStream);
        m_builder.CreateCondBr(drawFlag[nextActiveStream], compactPrimitiveIndexBlock[nextActiveStream],
                               endCompactPrimitiveIndexBlock[nextActiveStream]);
      }
    }
  }

  Value *streamOutOffsets[MaxTransformFeedbackBuffers] = {}; // Stream-out offset to write XFB outputs
  Value *numPrimsToWrite[MaxGsStreams] = {};
  {
    prepareSwXfb(primCountInSubgroup);

    // We are going to read XFB statistics info from LDS. Make sure the info has been written before
    // this.
    createFenceAndBarrier();

    auto xfbStatInfo =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, PrimShaderLdsRegion::XfbStats);
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (bufferActive[i]) {
        streamOutOffsets[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                        {xfbStatInfo, m_builder.getInt32(i)});
        streamOutOffsets[i] = m_builder.CreateAdd(m_streamOutBufOffsets[i], streamOutOffsets[i]);
        streamOutOffsets[i] = m_builder.CreateShl(streamOutOffsets[i], 2);
      }
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        numPrimsToWrite[i] =
            m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                      {xfbStatInfo, m_builder.getInt32(MaxTransformFeedbackBuffers + i)});
      }
    }

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite[firstActiveStream]);
    m_builder.CreateCondBr(validPrimitive, exportXfbBlock[firstActiveStream], endExportXfbBlock[firstActiveStream]);
  }

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!m_pipelineState->isVertexStreamActive(i))
      continue;

    // Construct ".exportXfbInStream[N]" block
    {
      m_builder.SetInsertPoint(exportXfbBlock[i]);

      Value *vertexIndices[3] = {};

      Value *uncompactedPrimitiveIndex =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                   PrimShaderLdsRegion::PrimitiveIndexMap, m_maxThreadsPerSubgroup * i);
      Value *vertexIndex = uncompactedPrimitiveIndex;

      const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
      vertexIndices[0] = vertexIndex;

      if (outVertsPerPrim > 1)
        vertexIndices[1] = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(1));
      if (outVertsPerPrim > 2) {
        vertexIndices[2] = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(2));

        Value *primData = readPerThreadDataFromLds(m_builder.getInt32Ty(), uncompactedPrimitiveIndex,
                                                   PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * i);
        // NOTE: primData[N] corresponds to the forming vertex
        // The vertice indices in the first triangle <N, N+1, N+2>
        // If provoking vertex is the first one, the vertice indices in the second triangle is <N, N+2, N+1>, otherwise
        // it is <N+1, N, N+2>.
        unsigned windingIndices[3] = {};
        if (m_pipelineState->getRasterizerState().provokingVertexMode == ProvokingVertexFirst) {
          windingIndices[0] = 0;
          windingIndices[1] = 2;
          windingIndices[2] = 1;
        } else {
          windingIndices[0] = 1;
          windingIndices[1] = 0;
          windingIndices[2] = 2;
        }
        Value *winding = m_builder.CreateICmpNE(primData, m_builder.getInt32(0));
        vertexIndices[0] = m_builder.CreateAdd(
            vertexIndex, m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[0]), m_builder.getInt32(0)));
        vertexIndices[1] = m_builder.CreateAdd(
            vertexIndex, m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[1]), m_builder.getInt32(1)));
        vertexIndices[2] = m_builder.CreateAdd(
            vertexIndex, m_builder.CreateSelect(winding, m_builder.getInt32(windingIndices[2]), m_builder.getInt32(2)));
      }

      for (unsigned j = 0; j < outVertsPerPrim; ++j) {
        for (unsigned k = 0; k < xfbExports.size(); ++k) {
          const auto &xfbExport = xfbExports[k];
          if (xfbExport.locInfo.streamId != i)
            continue; // Output not belong to this stream

          auto exportValue = readGsOutput(
              xfbExport.numElements > 1 ? FixedVectorType::get(m_builder.getFloatTy(), xfbExport.numElements)
                                        : m_builder.getFloatTy(),
              xfbExport.locInfo.location, xfbExport.locInfo.component, i, calcVertexItemOffset(i, vertexIndices[j]));

          unsigned format = 0;
          switch (xfbExport.numElements) {
          case 1:
            format = BUF_FORMAT_32_FLOAT;
            break;
          case 2:
            format = BUF_FORMAT_32_32_FLOAT_GFX11;
            break;
          case 3:
            format = BUF_FORMAT_32_32_32_FLOAT_GFX11;
            break;
          case 4:
            format = BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
            break;
          default:
            llvm_unreachable("Unexpected element number!");
            break;
          }

          CoherentFlag coherent = {};
          if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
            coherent.bits.glc = true;
            coherent.bits.slc = true;
#if LLPC_BUILD_GFX12
          } else {
            coherent.gfx12.scope = MemoryScope::MEMORY_SCOPE_DEV;
            coherent.gfx12.th = TH::TH_HT;
#endif
          }

          // vertexOffset = (threadIdInSubgroup * outVertsPerPrim + vertexIndex) * xfbStride
          Value *vertexOffset = m_builder.CreateAdd(
              m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(outVertsPerPrim)),
              m_builder.getInt32(j));
          vertexOffset = m_builder.CreateMul(vertexOffset, m_builder.getInt32(xfbStrides[xfbExport.xfbBuffer]));
          // xfbOutputOffset = vertexOffset + xfbOffset
          Value *xfbOutputOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(xfbExport.xfbOffset));

          m_builder.CreateIntrinsic(m_builder.getVoidTy(), Intrinsic::amdgcn_raw_tbuffer_store,
                                    {exportValue,                              // vdata
                                     m_streamOutBufDescs[xfbExport.xfbBuffer], // rsrc
                                     xfbOutputOffset,                          // offset
                                     streamOutOffsets[xfbExport.xfbBuffer],    // soffset
                                     m_builder.getInt32(format),               // format
                                     m_builder.getInt32(coherent.u32All)});    // auxiliary data
        }
      }

      m_builder.CreateBr(endExportXfbBlock[i]);
    }

    // Construct ".endExportXfbInStream[N]" block
    {
      m_builder.SetInsertPoint(endExportXfbBlock[i]);

      if (i != lastActiveStream) {
        unsigned nextActiveStream = i + 1;
        while (!m_pipelineState->isVertexStreamActive(nextActiveStream)) {
          ++nextActiveStream;
        }

        assert(nextActiveStream <= lastActiveStream);
        auto validPrimitive =
            m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite[nextActiveStream]);
        m_builder.CreateCondBr(validPrimitive, exportXfbBlock[nextActiveStream], endExportXfbBlock[nextActiveStream]);
      }
    }
  }
}

// =====================================================================================================================
// Prepare SW emulated XFB. Update various counter relevant to XFB, such as dwordsWritten, primsNeed, and primsWritten.
//
// @param primCountInSubgroup : Number of primitives in subgroup for each vertex stream
void NggPrimShader::prepareSwXfb(ArrayRef<Value *> primCountInSubgroup) {
  assert(m_gfxIp.major >= 11); // Must be GFX11++

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i)
    bufferActive[i] = xfbStrides[i] > 0;

  const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();
  unsigned xfbBufferToStream[MaxTransformFeedbackBuffers] = {};
  if (m_hasGs) {
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      for (unsigned j = 0; j < MaxGsStreams; ++j) {
        if ((streamXfbBuffers[j] & (1 << i)) != 0) {
          // NOTE: According to GLSL spec, all outputs assigned to a given XFB buffer are required to
          // come from a single vertex stream.
          xfbBufferToStream[i] = j;
          break;
        }
      }
    }
  }

  // GFX11 SW emulated stream-out with GDS support
  if (m_gfxIp.major == 11) {
    //
    // The processing is something like this:
    //
    // PREPARE_XFB() {
    //   if (threadIdInSubgroup == 0) {
    //     Acquire the control of GDS_STRMOUT_DWORDS_WRITTEN_X
    //     Calculate primsToWrite and dwordsToWrite
    //     Increment GDS_STRMOUT_DWORDS_WRITTEN_X and release the control
    //     Store XFB statistics info to LDS
    //     Increment GDS_STRMOUT_PRIMS_NEEDED_X and GDS_STRMOUT_PRIMS_WRITTEN_X
    //   }
    //
    auto insertBlock = m_builder.GetInsertBlock();
    auto primShader = insertBlock->getParent();

    auto prepareXfbBlock = createBlock(primShader, ".prepareXfb");
    prepareXfbBlock->moveAfter(insertBlock);

    auto endPrepareXfbBlock = createBlock(primShader, ".endPrepareXfb");
    endPrepareXfbBlock->moveAfter(prepareXfbBlock);

    // Continue to construct insert block
    {
      auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, prepareXfbBlock, endPrepareXfbBlock);
    }

    // Construct ".prepareXfb" block
    {
      m_builder.SetInsertPoint(prepareXfbBlock);

      unsigned firstActiveBuffer = InvalidValue;
      unsigned lastActiveBuffer = InvalidValue;

      for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
        if (!bufferActive[i])
          continue; // XFB buffer is inactive

        if (firstActiveBuffer == InvalidValue)
          firstActiveBuffer = i;
        lastActiveBuffer = i;
      }

      Value *numPrimsToWrite[MaxGsStreams] = {};
      for (unsigned i = 0; i < MaxGsStreams; ++i) {
        if (m_pipelineState->isVertexStreamActive(i))
          numPrimsToWrite[i] = primCountInSubgroup[i];
      }

      Value *dwordsWritten[MaxTransformFeedbackBuffers] = {};

      // NOTE: HW requires us to insert s_waitcnt lgkmcnt(0) following each GDS ordered count instruction.
      // This is to avoid outstanding GDS instructions, which cause problems in GDS synchronization.
      SyncScope::ID workgroupScope = m_builder.getContext().getOrInsertSyncScopeID("workgroup");

      // Calculate numPrimsToWrite
      for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
        if (!bufferActive[i])
          continue;

        if (i == firstActiveBuffer) {
          // ds_ordered_count
          dwordsWritten[i] = m_builder.CreateIntrinsic(
              Intrinsic::amdgcn_ds_ordered_add, {},
              {
                  m_builder.CreateIntToPtr(m_nggInputs.orderedWaveId,
                                           PointerType::get(m_builder.getInt32Ty(), ADDR_SPACE_REGION)), // m0
                  m_builder.getInt32(0),                                                                 // value to add
                  m_builder.getInt32(0),                                                                 // ordering
                  m_builder.getInt32(0),                                                                 // scope
                  m_builder.getFalse(),                                                                  // isVolatile
                  m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) |
                                     (1 << 24)), // ordered count index, [27:24] is dword count
                  m_builder.getFalse(),          // wave release
                  m_builder.getFalse(),          // wave done
              });
          m_builder.CreateFence(AtomicOrdering::Release, workgroupScope);
        } else {
          // ds_add_gs_reg
          dwordsWritten[i] =
              m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                        {m_builder.getInt32(0),                                         // value to add
                                         m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
          m_builder.CreateFence(AtomicOrdering::Release, workgroupScope);
        }

        // NUM_RECORDS = SQ_BUF_RSRC_WORD2
        Value *numRecords = m_builder.CreateExtractElement(m_streamOutBufDescs[i], 2);
        // bufferSizeInDwords = numRecords >> 2 (NOTE: NUM_RECORDS is set to the byte size of stream-out buffer)
        Value *bufferSizeInDwords = m_builder.CreateLShr(numRecords, 2);
        // dwordsRemaining = max(0, bufferSizeInDwords - (bufferOffset + dwordsWritten))
        Value *dwordsRemaining =
            m_builder.CreateSub(bufferSizeInDwords, m_builder.CreateAdd(m_streamOutBufOffsets[i], dwordsWritten[i]));
        dwordsRemaining = m_builder.CreateIntrinsic(Intrinsic::smax, dwordsRemaining->getType(),
                                                    {dwordsRemaining, m_builder.getInt32(0)});
        // numPrimsToWrite = min(dwordsRemaining / dwordsPerPrim, numPrimsToWrite)
        Value *dwordsPerPrim =
            m_builder.CreateMul(m_verticesPerPrimitive, m_builder.getInt32(xfbStrides[i] / sizeof(unsigned)));
        Value *primsCanWrite = m_builder.CreateUDiv(dwordsRemaining, dwordsPerPrim);
        numPrimsToWrite[xfbBufferToStream[i]] =
            m_builder.CreateIntrinsic(Intrinsic::umin, numPrimsToWrite[xfbBufferToStream[i]]->getType(),
                                      {numPrimsToWrite[xfbBufferToStream[i]], primsCanWrite});

        Value *dwordsToWrite = m_builder.CreateMul(numPrimsToWrite[xfbBufferToStream[i]], dwordsPerPrim);

        if (i == lastActiveBuffer) {
          // ds_ordered_count, wave done
          dwordsWritten[i] = m_builder.CreateIntrinsic(
              Intrinsic::amdgcn_ds_ordered_add, {},
              {
                  m_builder.CreateIntToPtr(m_nggInputs.orderedWaveId,
                                           PointerType::get(m_builder.getInt32Ty(), ADDR_SPACE_REGION)), // m0
                  dwordsToWrite,                                                                         // value to add
                  m_builder.getInt32(0),                                                                 // ordering
                  m_builder.getInt32(0),                                                                 // scope
                  m_builder.getFalse(),                                                                  // isVolatile
                  m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) |
                                     (1 << 24)), // ordered count index, [27:24] is dword count
                  m_builder.getTrue(),           // wave release
                  m_builder.getTrue(),           // wave done
              });
          m_builder.CreateFence(AtomicOrdering::Release, workgroupScope);
        } else {
          // ds_add_gs_reg
          dwordsWritten[i] =
              m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, dwordsToWrite->getType(),
                                        {dwordsToWrite,                                                 // value to add
                                         m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
          m_builder.CreateFence(AtomicOrdering::Release, workgroupScope);
        }
      }

      // Update GDS primitive statistics counters
      const unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbStats);
      for (unsigned i = 0; i < MaxGsStreams; ++i) {
        if (!m_pipelineState->isVertexStreamActive(i))
          continue;

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, primCountInSubgroup[i]->getType(),
                                  {primCountInSubgroup[i],                                          // value to add
                                   m_builder.getInt32((GDS_STRMOUT_PRIMS_NEEDED_0 + 2 * i) << 2)}); // count index
        m_builder.CreateFence(AtomicOrdering::Release, workgroupScope);

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, numPrimsToWrite[i]->getType(),
                                  {numPrimsToWrite[i],                                               // value to add
                                   m_builder.getInt32((GDS_STRMOUT_PRIMS_WRITTEN_0 + 2 * i) << 2)}); // count index
        m_builder.CreateFence(AtomicOrdering::Release, workgroupScope);
      }

      // Store XFB statistics info to LDS
      for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
        if (!bufferActive[i])
          continue;
        writeValueToLds(dwordsWritten[i], m_builder.getInt32(regionStart + i));
      }

      for (unsigned i = 0; i < MaxGsStreams; ++i) {
        if (!m_pipelineState->isVertexStreamActive(i))
          continue;
        writeValueToLds(numPrimsToWrite[i], m_builder.getInt32(regionStart + MaxTransformFeedbackBuffers + i));
      }

      m_builder.CreateBr(endPrepareXfbBlock);
    }

    // Construct ".endPrepareXfb" block
    {
      m_builder.SetInsertPoint(endPrepareXfbBlock);
      // Nothing to do
    }

    return;
  }

#if LLPC_BUILD_GFX12
  // GFX12+ SW emulated stream-out with global ordered atomic add support
  assert(m_gfxIp.major >= 12);

  //
  // The processing is something like this:
  //
  // PREPARE_XFB() {
  //   if (threadIdInSubgroup < MaxGsStreams && streamActive)
  //     numPrimsToWrite[X] = primCountInSubgroup[X]
  //
  //   if (threadIdInSubgroup < MaxTransformFeedbackBuffers && bufferActive) {
  //     Load ordered ID pair from stream-out control buffer and try to increment dwordsWritten[X]
  //     while (orderedWaveId != readyOrderedWaveId) {
  //       Sleep for a while
  //       Reload ordered ID pair from stream-out control buffer and try to increment dwordsWritten[X]
  //     }
  //
  //     Calculate primsToWrite and dwordsToWrite
  //     Revise dwordsWritten[X]
  //     Store XFB statistics info to LDS
  //   }
  //
  //   if (threadIdInSubgroup < MaxGsStreams && streamActive)
  //     Increment primsNeeded[X] and primsWritten[X]
  //
  unsigned numActiveBuffers = 0;
  unsigned activeBufferMask = 0;
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    if (!bufferActive[i])
      continue; // XFB buffer is inactive

    ++numActiveBuffers;
    activeBufferMask |= (1U << i);
  }

  unsigned numActiveStreams = 0;
  unsigned activeStreamMask = 0;
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!m_pipelineState->isVertexStreamActive(i))
      continue; // Vertex stream is inactive

    ++numActiveStreams;
    activeStreamMask |= (1U << i);
  }

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(
      m_hasGs ? ShaderStage::Geometry : (m_hasTes ? ShaderStage::TessEval : ShaderStage::Vertex));

  const unsigned xfbStatsRegionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbStats);

  auto insertBlock = m_builder.GetInsertBlock();
  auto primShader = insertBlock->getParent();

  auto initPrimitivesToWriteBlock = createBlock(primShader, ".initPrimitivesToWrite");
  initPrimitivesToWriteBlock->moveAfter(insertBlock);

  auto endInitPrimitivesToWriteBlock = createBlock(primShader, ".endInitPrimitivesToWrite");
  endInitPrimitivesToWriteBlock->moveAfter(initPrimitivesToWriteBlock);

  auto waveOrderingHeaderBlock = createBlock(primShader, ".waveOrderingHeader");
  waveOrderingHeaderBlock->moveAfter(endInitPrimitivesToWriteBlock);

  auto waveOrderingBodyBlock = createBlock(primShader, ".waveOrderingBody");
  waveOrderingBodyBlock->moveAfter(waveOrderingHeaderBlock);

  auto endWaveOrderingBlock = createBlock(primShader, ".endWaveOrdering");
  endWaveOrderingBlock->moveAfter(waveOrderingBodyBlock);

  auto checkUpdatePrimitiveCounterBlock = createBlock(primShader, ".checkUpdatePrimitiveCounter");
  checkUpdatePrimitiveCounterBlock->moveAfter(endWaveOrderingBlock);

  auto updatePrimitiveCounterBlock = createBlock(primShader, ".updatePrimitiveCounter");
  updatePrimitiveCounterBlock->moveAfter(checkUpdatePrimitiveCounterBlock);

  auto endUpdatePrimitiveCounterBlock = createBlock(primShader, ".endUpdatePrimitiveCounter");
  endUpdatePrimitiveCounterBlock->moveAfter(updatePrimitiveCounterBlock);

  // Continue to construct insert block
  Value *validStream = nullptr;
  Value *numPrimsInStream = nullptr;
  {
    numPrimsInStream = PoisonValue::get(m_builder.getInt32Ty());

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (!m_pipelineState->isVertexStreamActive(i))
        continue;

      if (numActiveStreams > 1) {
        // Multiple active vertex streams, promote the values to VGPRs
        numPrimsInStream = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
                                                     {primCountInSubgroup[i], m_builder.getInt32(i), numPrimsInStream});
      } else {
        // Single active vertex stream, keep the values in SGPR
        assert(numActiveStreams == 1);
        numPrimsInStream = primCountInSubgroup[i];
        break;
      }
    }

    validStream = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(MaxGsStreams));
    assert(activeStreamMask != 0);
    Value *mask = m_builder.getIntN(waveSize, activeStreamMask);
    auto activeStream = m_builder.CreateIntrinsic(m_builder.getInt1Ty(), Intrinsic::amdgcn_inverse_ballot, mask);

    validStream = m_builder.CreateAnd(validStream, activeStream);
    m_builder.CreateCondBr(validStream, initPrimitivesToWriteBlock, endInitPrimitivesToWriteBlock);
  }

  // Construct ".initPrimitivesToWrite" block
  {
    m_builder.SetInsertPoint(initPrimitivesToWriteBlock);

    auto ldsOffset = m_builder.CreateAdd(m_builder.getInt32(xfbStatsRegionStart + MaxTransformFeedbackBuffers),
                                         m_nggInputs.threadIdInSubgroup);
    writeValueToLds(numPrimsInStream, ldsOffset);

    m_builder.CreateBr(endInitPrimitivesToWriteBlock);
  }

  // Construct ".endInitPrimitivesToWrite" block
  Value *dwordsPerPrim = nullptr;
  Value *bufferSizeInDwords = nullptr;
  Value *streamOutBufOffset = nullptr;
  Value *dwordsNeeded = nullptr;
  Value *bufferToStream = nullptr;
  {
    m_builder.SetInsertPoint(endInitPrimitivesToWriteBlock);

    dwordsPerPrim = PoisonValue::get(m_builder.getInt32Ty());
    bufferSizeInDwords = PoisonValue::get(m_builder.getInt32Ty());
    streamOutBufOffset = PoisonValue::get(m_builder.getInt32Ty());
    Value *primitiveCount = primCountInSubgroup[0];
    bufferToStream = m_builder.getInt32(0);

    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      Value *primitiveSize =
          m_builder.CreateMul(m_verticesPerPrimitive, m_builder.getInt32(xfbStrides[i] / sizeof(unsigned)));

      // NUM_RECORDS = SQ_BUF_RSRC_WORD2
      Value *numRecords = m_builder.CreateExtractElement(m_streamOutBufDescs[i], 2);
      // bufferSizeInDwords = numRecords >> 2 (NOTE: NUM_RECORDS is set to the byte size of stream-out buffer)
      Value *bufferSize = m_builder.CreateLShr(numRecords, 2);

      if (numActiveBuffers > 1) {
        // Multiple active XFB buffers, promote the values to VGPRs for later handling
        dwordsPerPrim = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
                                                  {primitiveSize, m_builder.getInt32(i), dwordsPerPrim});

        bufferSizeInDwords = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
                                                       {bufferSize, m_builder.getInt32(i), bufferSizeInDwords});

        streamOutBufOffset =
            m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
                                      {m_streamOutBufOffsets[i], m_builder.getInt32(i), streamOutBufOffset});

        if (m_hasGs) {
          primitiveCount = m_builder.CreateIntrinsic(
              m_builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
              {primCountInSubgroup[xfbBufferToStream[i]], m_builder.getInt32(i), primitiveCount});

          bufferToStream = m_builder.CreateIntrinsic(
              m_builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
              {m_builder.getInt32(xfbBufferToStream[i]), m_builder.getInt32(i), bufferToStream});
        }
      } else {
        // Single active XFB buffer, keep the values in SGPR
        assert(numActiveBuffers == 1);
        dwordsPerPrim = primitiveSize;
        bufferSizeInDwords = bufferSize;
        streamOutBufOffset = m_streamOutBufOffsets[i];
        if (m_hasGs) {
          primitiveCount = primCountInSubgroup[xfbBufferToStream[i]];
          bufferToStream = m_builder.getInt32(xfbBufferToStream[i]);
        }

        break; // We can exit the loop since we just handle one active XFB buffer
      }
    }

    dwordsNeeded = m_builder.CreateMul(dwordsPerPrim, primitiveCount);

    auto validBuffer =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(MaxTransformFeedbackBuffers));
    assert(activeBufferMask != 0);
    Value *mask = m_builder.getIntN(waveSize, activeBufferMask);
    auto activeBuffer = m_builder.CreateIntrinsic(m_builder.getInt1Ty(), Intrinsic::amdgcn_inverse_ballot, mask);
    validBuffer = m_builder.CreateAnd(validBuffer, activeBuffer);
    m_builder.CreateCondBr(validBuffer, waveOrderingHeaderBlock, checkUpdatePrimitiveCounterBlock);
  }

  // Construct ".waveOrderingHeader" block
  Value *readyOrderedWaveId = nullptr;
  Value *dwordsWritten = nullptr;
  {
    m_builder.SetInsertPoint(waveOrderingHeaderBlock);

    const unsigned orderedIdPairStride = m_streamOutControlCbOffsets.orderedIdPair[1].orderedWaveId -
                                         m_streamOutControlCbOffsets.orderedIdPair[0].orderedWaveId;
    auto orderedIdPairOffset =
        m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(orderedIdPairStride));
    orderedIdPairOffset = m_builder.CreateAdd(
        m_builder.getInt32(m_streamOutControlCbOffsets.orderedIdPair[0].orderedWaveId), orderedIdPairOffset);

    auto oldOrderedIdPair = globalAtomicOrderedAdd(std::make_pair(m_nggInputs.orderedWaveId, dwordsNeeded),
                                                   m_streamOutControlBufPtr, orderedIdPairOffset);
    readyOrderedWaveId = oldOrderedIdPair.first;
    dwordsWritten = oldOrderedIdPair.second;

    auto needToWait = m_builder.CreateICmpNE(m_nggInputs.orderedWaveId, readyOrderedWaveId);
    m_builder.CreateCondBr(needToWait, waveOrderingBodyBlock, endWaveOrderingBlock);
  }

  // Construct ".waveOrderingBody" block
  {
    m_builder.SetInsertPoint(waveOrderingBodyBlock);

    // NOTE: We use such rules to derive a variable amount of wait time based on the difference between orderedWaveId
    // and readyOrderedWaveId:
    //
    //   - If the difference value is only 1, we at most ~1/2 L2 latency time (~128 clocks).
    //   - For every additional unit of the difference value, we add ~2 round trip times (~512 clocks) to the sleep
    //     duration.
    //
    // The formula is therefore as follow (the unit of the sleep duration of s_sleep is 64 clocks):
    //   waitTime = 8 * (orderedWaveId - readyOrderedWaveId - 1) + 2
    auto waitTime = m_builder.CreateSub(m_nggInputs.orderedWaveId, readyOrderedWaveId);
    waitTime = m_builder.CreateSub(waitTime, m_builder.getInt32(1));
    waitTime = m_builder.CreateMul(waitTime, m_builder.getInt32(8));
    waitTime = m_builder.CreateAdd(waitTime, m_builder.getInt32(2));
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_sleep_var, {}, waitTime);

    m_builder.CreateBr(waveOrderingHeaderBlock);
  }

  // Construct ".endWaveOrdering" block
  {
    m_builder.SetInsertPoint(endWaveOrderingBlock);

    // dwordsRemaining = max(0, bufferSizeInDwords - (bufferOffset + dwordsWritten))
    Value *dwordsRemaining =
        m_builder.CreateSub(bufferSizeInDwords, m_builder.CreateAdd(streamOutBufOffset, dwordsWritten));
    dwordsRemaining = m_builder.CreateIntrinsic(Intrinsic::smax, dwordsRemaining->getType(),
                                                {dwordsRemaining, m_builder.getInt32(0)});
    // primsCanWrite = dwordsRemaining / dwordsPerPrim
    Value *primsCanWrite = m_builder.CreateUDiv(dwordsRemaining, dwordsPerPrim);
    // numPrimsToWrite = ds_min(primsCanWrite, numPrimsToWrite)
    auto ldsOffset =
        m_builder.CreateAdd(m_builder.getInt32(xfbStatsRegionStart + MaxTransformFeedbackBuffers), bufferToStream);
    atomicOp(AtomicRMWInst::UMin, primsCanWrite, ldsOffset);
    auto numPrimsToWrite =
        readValueFromLds(m_builder.getInt32Ty(), ldsOffset); // Read back the final result of numPrimsToWrite

    ldsOffset = m_builder.CreateAdd(m_builder.getInt32(xfbStatsRegionStart), m_nggInputs.threadIdInSubgroup);
    writeValueToLds(dwordsWritten, ldsOffset);

    // dwordsToWrite = numPrimsToWrite * dwordsPerPrim
    auto dwordsToWrite = m_builder.CreateMul(numPrimsToWrite, dwordsPerPrim);
    // dwordsWrittenDelta = dwordsNeeded - dwordsToWrite
    auto dwordsWrittenDelta = m_builder.CreateSub(dwordsNeeded, dwordsToWrite);

    // Revise dwordsWritten[X]
    const unsigned dwordsWrittenStride = m_streamOutControlCbOffsets.orderedIdPair[1].dwordsWritten -
                                         m_streamOutControlCbOffsets.orderedIdPair[0].dwordsWritten;
    auto dwordsWrittenOffset =
        m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(dwordsWrittenStride));
    dwordsWrittenOffset = m_builder.CreateAdd(
        m_builder.getInt32(m_streamOutControlCbOffsets.orderedIdPair[0].dwordsWritten), dwordsWrittenOffset);

    globalAtomicOp(AtomicRMWInst::BinOp::Sub, dwordsWrittenDelta, m_streamOutControlBufPtr, dwordsWrittenOffset);

    m_builder.CreateBr(checkUpdatePrimitiveCounterBlock);
  }

  // Construct ".checkUpdatePrimitiveCounter" block
  {
    m_builder.SetInsertPoint(checkUpdatePrimitiveCounterBlock);

    m_builder.CreateCondBr(validStream, updatePrimitiveCounterBlock, endUpdatePrimitiveCounterBlock);
  }

  // Construct ".updatePrimitiveCounter" block
  {
    m_builder.SetInsertPoint(updatePrimitiveCounterBlock);

    // Update the counters primsNeed[X] and primsWritten[X]
    const unsigned primsNeededStride =
        m_streamOutControlCbOffsets.primsNeeded[1] - m_streamOutControlCbOffsets.primsNeeded[0];
    auto primsNeededOffset = m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(primsNeededStride));
    primsNeededOffset =
        m_builder.CreateAdd(m_builder.getInt32(m_streamOutControlCbOffsets.primsNeeded[0]), primsNeededOffset);

    globalAtomicOp(AtomicRMWInst::Add, m_builder.CreateZExt(numPrimsInStream, m_builder.getInt64Ty()),
                   m_streamOutControlBufPtr, primsNeededOffset);

    auto ldsOffset = m_builder.CreateAdd(m_builder.getInt32(xfbStatsRegionStart + MaxTransformFeedbackBuffers),
                                         m_nggInputs.threadIdInSubgroup);
    Value *numPrimsToWrite = readValueFromLds(m_builder.getInt32Ty(), ldsOffset);

    const unsigned primsWrittenStride =
        m_streamOutControlCbOffsets.primsWritten[1] - m_streamOutControlCbOffsets.primsWritten[0];
    auto primsWrittenOffset =
        m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(primsWrittenStride));
    primsWrittenOffset =
        m_builder.CreateAdd(m_builder.getInt32(m_streamOutControlCbOffsets.primsWritten[0]), primsWrittenOffset);

    globalAtomicOp(AtomicRMWInst::Add, m_builder.CreateZExt(numPrimsToWrite, m_builder.getInt64Ty()),
                   m_streamOutControlBufPtr, primsWrittenOffset);

    m_builder.CreateBr(endUpdatePrimitiveCounterBlock);
  }

  // Construct ".endUpdatePrimitiveCounter" block
  {
    m_builder.SetInsertPoint(endUpdatePrimitiveCounterBlock);
    // Nothing to do
  }
#else
  llvm_unreachable("Not implemented!");
#endif
}

// =====================================================================================================================
// Collect primitive statistics (primitive statistics counting) and update the values in HW counters.
void NggPrimShader::collectPrimitiveStats() {
  // NOTE: For SW emulated stream-out, the processing will update HW counters at the same time unconditionally. We don't
  // have to particularly call this function.
  assert(!m_pipelineState->enableSwXfb());
  assert(m_pipelineState->enablePrimStats()); // Make sure we do need to count generated primitives

  if (!m_hasGs) {
    // GS is not present

    //
    // The processing is something like this:
    //
    // NGG_PRIM_STATS() {
    //   if (threadIdInSubgroup == 0)
    //     Increment GDS_STRMOUT_PRIMS_NEEDED_0 and GDS_STRMOUT_PRIMS_WRITTEN_0
    // }
    //
    BasicBlock *insertBlock = m_builder.GetInsertBlock();

    BasicBlock *collectPrimitiveStatsBlock = createBlock(insertBlock->getParent(), ".collectPrimitiveStats");
    collectPrimitiveStatsBlock->moveAfter(insertBlock);
    BasicBlock *endCollectPrimitiveStatsBlock = createBlock(insertBlock->getParent(), ".endCollectPrimitiveStats");
    endCollectPrimitiveStatsBlock->moveAfter(collectPrimitiveStatsBlock);

    // Insert branching in current block to collect primitive statistics
    {
      auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
      m_builder.CreateCondBr(firstThreadInSubgroup, collectPrimitiveStatsBlock, endCollectPrimitiveStatsBlock);
    }

    // Construct ".collectPrimitiveStats" block
    {
      m_builder.SetInsertPoint(collectPrimitiveStatsBlock);

      if (m_gfxIp.major <= 11) {
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_nggInputs.primCountInSubgroup->getType(),
                                  {m_nggInputs.primCountInSubgroup,                       // value to add
                                   m_builder.getInt32(GDS_STRMOUT_PRIMS_NEEDED_0 << 2)}); // count index

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                  {m_builder.getInt32(0),                                  // value to add
                                   m_builder.getInt32(GDS_STRMOUT_PRIMS_WRITTEN_0 << 2)}); // count index
      } else {
#if LLPC_BUILD_GFX12
        globalAtomicOp(AtomicRMWInst::Add,
                       m_builder.CreateZExt(m_nggInputs.primCountInSubgroup, m_builder.getInt64Ty()),
                       m_streamOutControlBufPtr, m_builder.getInt32(m_streamOutControlCbOffsets.primsNeeded[0]));

        globalAtomicOp(AtomicRMWInst::Add, m_builder.getInt64(0), m_streamOutControlBufPtr,
                       m_builder.getInt32(m_streamOutControlCbOffsets.primsWritten[0]));
#else
        llvm_unreachable("Not implemented!");
#endif
      }

      m_builder.CreateBr(endCollectPrimitiveStatsBlock);
    }

    // Construct ".endCollectPrimitiveStats" block
    { m_builder.SetInsertPoint(endCollectPrimitiveStatsBlock); }

    return;
  }

  // GS is present
  assert(m_hasGs);

  //
  // The processing is something like this:
  //
  // NGG_GS_PRIM_STATS() {
  //   if (threadIdInSubgroup < MaxGsStreams)
  //     Initialize output primitive count for each vertex stream
  //   Barrier
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Check the draw flag of output primitives and compute draw mask
  //
  //   if (threadIdInWave == 0)
  //     Accumulate output primitive count
  //   Barrier
  //
  //   if (threadIdInSubgroup == 0) {
  //     for each vertex stream
  //       Increment GDS_STRMOUT_PRIMS_NEEDED_X and GDS_STRMOUT_PRIMS_WRITTEN_X
  //   }
  // }
  //
  BasicBlock *insertBlock = m_builder.GetInsertBlock();

  BasicBlock *initPrimitiveCountsBlock = createBlock(insertBlock->getParent(), ".initPrimitiveCounts");
  initPrimitiveCountsBlock->moveAfter(insertBlock);
  BasicBlock *endInitPrimitiveCountsBlock = createBlock(insertBlock->getParent(), ".endInitPrimitiveCounts");
  endInitPrimitiveCountsBlock->moveAfter(initPrimitiveCountsBlock);

  BasicBlock *checkPrimitiveDrawFlagBlock = createBlock(insertBlock->getParent(), ".checkPrimitiveDrawFlag");
  checkPrimitiveDrawFlagBlock->moveAfter(endInitPrimitiveCountsBlock);
  BasicBlock *endCheckPrimitiveDrawFlagBlock = createBlock(insertBlock->getParent(), ".endCheckPrimitiveDrawFlag");
  endCheckPrimitiveDrawFlagBlock->moveAfter(checkPrimitiveDrawFlagBlock);

  BasicBlock *countPrimitivesBlock = createBlock(insertBlock->getParent(), ".countPrimitives");
  countPrimitivesBlock->moveAfter(endCheckPrimitiveDrawFlagBlock);
  BasicBlock *endCountPrimitivesBlock = createBlock(insertBlock->getParent(), ".endCountPrimitives");
  endCountPrimitivesBlock->moveAfter(countPrimitivesBlock);

  BasicBlock *collectPrimitiveStatsBlock = createBlock(insertBlock->getParent(), ".collectPrimitiveStats");
  collectPrimitiveStatsBlock->moveAfter(endCountPrimitivesBlock);
  BasicBlock *endCollectPrimitiveStatsBlock = createBlock(insertBlock->getParent(), ".endCollectPrimitiveStats");
  endCollectPrimitiveStatsBlock->moveAfter(collectPrimitiveStatsBlock);

  // Insert branching in current block to collect primitive statistics
  {
    auto validStream = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(MaxGsStreams));
    m_builder.CreateCondBr(validStream, initPrimitiveCountsBlock, endInitPrimitiveCountsBlock);
  }

  // Construct ".initPrimitiveCounts" block
  {
    m_builder.SetInsertPoint(initPrimitiveCountsBlock);

    writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup,
                            PrimShaderLdsRegion::PrimitiveCounts);

    m_builder.CreateBr(endInitPrimitiveCountsBlock);
  }

  // Construct ".endInitPrimitiveCounts" block
  {
    m_builder.SetInsertPoint(endInitPrimitiveCountsBlock);

    createFenceAndBarrier();

    auto validPrimitive = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.primCountInSubgroup);
    m_builder.CreateCondBr(validPrimitive, checkPrimitiveDrawFlagBlock, endCheckPrimitiveDrawFlagBlock);
  }

  // Construct ".checkPrimitiveDrawFlag" block
  Value *drawFlag[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(checkPrimitiveDrawFlagBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        // drawFlag = primData[N] != NullPrim
        auto primData = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                 PrimShaderLdsRegion::PrimitiveData, m_maxThreadsPerSubgroup * i);
        drawFlag[i] = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));
      }
    }

    m_builder.CreateBr(endCheckPrimitiveDrawFlagBlock);
  }

  // Construct ".endCheckPrimitiveDrawFlag" block
  Value *drawMask[MaxGsStreams] = {};
  Value *primCountInWave[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endCheckPrimitiveDrawFlagBlock);

    // Update draw flags
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        drawFlag[i] = createPhi(
            {{drawFlag[i], checkPrimitiveDrawFlagBlock}, {m_builder.getFalse(), endInitPrimitiveCountsBlock}});
      }
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        drawMask[i] = ballot(drawFlag[i]);

        primCountInWave[i] = m_builder.CreateUnaryIntrinsic(Intrinsic::ctpop, drawMask[i]);
        primCountInWave[i] = m_builder.CreateTrunc(primCountInWave[i], m_builder.getInt32Ty());
      }
    }

    auto firstThreadInWave = m_builder.CreateICmpEQ(m_nggInputs.threadIdInWave, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInWave, countPrimitivesBlock, endCountPrimitivesBlock);
  }

  // Construct ".countPrimitives" block
  {
    m_builder.SetInsertPoint(countPrimitivesBlock);

    unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::PrimitiveCounts);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i))
        atomicOp(AtomicRMWInst::Add, primCountInWave[i], m_builder.getInt32(regionStart + i));
    }

    m_builder.CreateBr(endCountPrimitivesBlock);
  }

  // Construct ".endCountPrimitives" block
  Value *primCountInSubgroup[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endCountPrimitivesBlock);

    createFenceAndBarrier();

    auto primCountInStreams = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave,
                                                       PrimShaderLdsRegion::PrimitiveCounts);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (!m_pipelineState->isVertexStreamActive(i))
        continue;

      primCountInSubgroup[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                         {primCountInStreams, m_builder.getInt32(i)});
    }

    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, collectPrimitiveStatsBlock, endCollectPrimitiveStatsBlock);
  }

  // Construct ".collectPrimitiveStats" block
  {
    m_builder.SetInsertPoint(collectPrimitiveStatsBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (!m_pipelineState->isVertexStreamActive(i))
        continue;

      if (m_gfxIp.major <= 11) {
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, primCountInSubgroup[i]->getType(),
                                  {primCountInSubgroup[i],                                          // value to add
                                   m_builder.getInt32((GDS_STRMOUT_PRIMS_NEEDED_0 + 2 * i) << 2)}); // count index

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                  {m_builder.getInt32(0),                                            // value to add
                                   m_builder.getInt32((GDS_STRMOUT_PRIMS_WRITTEN_0 + 2 * i) << 2)}); // count index
      } else {
#if LLPC_BUILD_GFX12
        globalAtomicOp(AtomicRMWInst::Add, m_builder.CreateZExt(primCountInSubgroup[i], m_builder.getInt64Ty()),
                       m_streamOutControlBufPtr, m_builder.getInt32(m_streamOutControlCbOffsets.primsNeeded[i]));

        globalAtomicOp(AtomicRMWInst::Add, m_builder.getInt64(0), m_streamOutControlBufPtr,
                       m_builder.getInt32(m_streamOutControlCbOffsets.primsWritten[i]));
#else
        llvm_unreachable("Not implemented!");
#endif
      }
    }

    m_builder.CreateBr(endCollectPrimitiveStatsBlock);
  }

  // Construct ".endCollectPrimitiveStats" block
  { m_builder.SetInsertPoint(endCollectPrimitiveStatsBlock); }
}

// =====================================================================================================================
// Reads XFB output from LDS
//
// @param readDataTy : Data read from LDS
// @param vertexIndex: Relative vertex index in NGG subgroup
// @param offsetInVertex : Output offset within all XFB outputs of a vertex (in dwords)
Value *NggPrimShader::readXfbOutputFromLds(Type *readDataTy, Value *vertexIndex, unsigned offsetInVertex) {
  assert(m_pipelineState->enableSwXfb()); // SW-emulated stream-out must be enabled
  assert(!m_hasGs);

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize;
  auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize));

  if (m_nggControl->passthroughMode) {
    const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbOutput);
    Value *ldsOffset = m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + offsetInVertex));
    return readValueFromLds(readDataTy, ldsOffset);
  }

  // NOTE: For NGG culling mode, XFB outputs are part of vertex cull info.
  const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(
      vertexItemOffset, m_builder.getInt32(regionStart + m_vertCullInfoOffsets.xfbOutputs + offsetInVertex));
  return readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes XFB output from LDS
//
// @param writeData : Data written to LDS
// @param vertexIndex: Relative vertex index in NGG subgroup
// @param offsetInVertex : Output offset within all XFB outputs of a vertex (in dwords)
void NggPrimShader::writeXfbOutputToLds(Value *writeData, Value *vertexIndex, unsigned offsetInVertex) {
  assert(m_pipelineState->enableSwXfb()); // SW-emulated stream-out must be enabled
  assert(!m_hasGs);

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize;
  auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize));

  if (m_nggControl->passthroughMode) {
    const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbOutput);
    Value *ldsOffset = m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + offsetInVertex));
    writeValueToLds(writeData, ldsOffset);
    return;
  }

  // NOTE: For NGG culling mode, XFB outputs are part of vertex cull info.
  const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCullInfo);
  Value *ldsOffset = m_builder.CreateAdd(
      vertexItemOffset, m_builder.getInt32(regionStart + m_vertCullInfoOffsets.xfbOutputs + offsetInVertex));
  writeValueToLds(writeData, ldsOffset);
}

// =====================================================================================================================
// Fetches the position data for the specified relative vertex index.
//
// @param vertexIndex : Relative vertex index in NGG subgroup.
Value *NggPrimShader::fetchVertexPositionData(Value *vertexIndex) {
  if (!m_hasGs) {
    // ES-only
    return readPerThreadDataFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), vertexIndex,
                                    PrimShaderLdsRegion::VertexPosition, 0, true);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInPosition];
  const unsigned rasterStream = m_pipelineState->getRasterizerState().rasterStream;
  assert(rasterStream != InvalidValue);
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexIndex);

  return readGsOutput(FixedVectorType::get(m_builder.getFloatTy(), 4), loc, 0, rasterStream, vertexOffset);
}

// =====================================================================================================================
// Fetches the aggregated sign mask of cull distances for the specified relative vertex index.
//
// @param vertexIndex : Relative vertex index in NGG subgroup.
Value *NggPrimShader::fetchCullDistanceSignMask(Value *vertexIndex) {
  assert(m_nggControl->enableCullDistanceCulling);

  if (!m_hasGs) {
    // ES-only
    const unsigned esGsRingItemSize =
        m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage.gs.hwConfig.esGsRingItemSize;
    auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize));
    return readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset,
                                     m_vertCullInfoOffsets.cullDistanceSignMask);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
  const unsigned rasterStream = m_pipelineState->getRasterizerState().rasterStream;
  assert(rasterStream != InvalidValue);
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexIndex);

  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->builtInUsage.gs;
  auto cullDistances = readGsOutput(ArrayType::get(m_builder.getFloatTy(), builtInUsage.cullDistance), loc, 0,
                                    rasterStream, vertexOffset);

  // Calculate the sign mask for all cull distances
  Value *signMask = m_builder.getInt32(0);
  for (unsigned i = 0; i < builtInUsage.cullDistance; ++i) {
    auto cullDistance = m_builder.CreateExtractValue(cullDistances, i);
    cullDistance = m_builder.CreateBitCast(cullDistance, m_builder.getInt32Ty());

    Value *signBit = createUBfe(cullDistance, 31, 1);
    signBit = m_builder.CreateShl(signBit, i);
    signMask = m_builder.CreateOr(signMask, signBit);
  }

  return signMask;
}

// =====================================================================================================================
// Calculates the starting LDS offset (in dwords) of vertex item data in GS-VS ring.
//
// @param streamId : ID of output vertex stream.
// @param vertexIndex : Relative vertex index in NGG subgroup.
Value *NggPrimShader::calcVertexItemOffset(unsigned streamId, Value *vertexIndex) {
  assert(m_hasGs); // GS must be present
  assert(streamId < MaxGsStreams);

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStage::Geometry)->inOutUsage;

  // vertexOffset = gsVsRingStart + streamBases[stream] + vertexIndex * vertexItemSize (in dwords)
  const unsigned vertexItemSize = inOutUsage.gs.hwConfig.gsVsVertexItemSize[streamId];

  auto vertexOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(vertexItemSize));
  vertexOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(m_gsStreamBases[streamId]));

  const unsigned gsVsRingStart = getLdsRegionStart(PrimShaderLdsRegion::GsVsRing);
  vertexOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(gsVsRingStart));

  return vertexOffset;
}

// =====================================================================================================================
// Creates a new basic block. Always insert it at the end of the parent function.
//
// @param parent : Parent function to which the new block belongs
// @param blockName : Name of the new block
BasicBlock *NggPrimShader::createBlock(Function *parent, const Twine &blockName) {
  return BasicBlock::Create(m_builder.getContext(), blockName, parent);
}

// =====================================================================================================================
// Extracts bitfield [offset, offset + count - 1] from the source value (int32). This is a substitute of the intrinsic
// amdgcn_ubfe when the offset and count are both constants.
//
// @param value : Source value to extract
// @param offset : Bit number of least-significant end of bitfield
// @param count : Count of bits in bitfield
// @returns : The extracted bitfield
Value *NggPrimShader::createUBfe(Value *value, unsigned offset, unsigned count) {
  assert(value->getType()->isIntegerTy(32));
  assert(offset <= 31 && count >= 1 && offset + count - 1 <= 31);

  if (count == 32)
    return value; // Return the whole

  if (offset == 0)
    return m_builder.CreateAnd(value, (1U << count) - 1); // Just need mask

  return m_builder.CreateAnd(m_builder.CreateLShr(value, offset), (1U << count) - 1);
}

// =====================================================================================================================
// Make 64-bit pointer of specified type from 32-bit integer value, extending it with PC.
//
// @param ptrValue : 32-bit integer value to extend
// @param ptrTy : Type that result pointer needs to be
Value *NggPrimShader::makePointer(Value *ptrValue, Type *ptrTy) {
  assert(ptrValue->getType()->isIntegerTy(32)); // Must be i32

  Value *pc = m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
  pc = m_builder.CreateBitCast(pc, FixedVectorType::get(m_builder.getInt32Ty(), 2));

  Value *ptr = m_builder.CreateInsertElement(pc, ptrValue, static_cast<uint64_t>(0));
  ptr = m_builder.CreateBitCast(ptr, m_builder.getInt64Ty());
  ptr = m_builder.CreateIntToPtr(ptr, ptrTy);

  return ptr;
}

// =====================================================================================================================
// Create a PHI node with the specified incomings.
//
// @param incomings : A set of incomings to create this PHI node
// @param name : Name of this PHI node
// @returns : The created PHI node
PHINode *NggPrimShader::createPhi(ArrayRef<std::pair<Value *, BasicBlock *>> incomings, const Twine &name) {
  assert(incomings.size() >= 2); // Must at least have two incomings

  auto phiType = incomings[0].first->getType();
  auto phi = m_builder.CreatePHI(phiType, incomings.size(), name);

  for (auto &incoming : incomings) {
    assert(incoming.first->getType() == phiType);
    phi->addIncoming(incoming.first, incoming.second);
  }

  return phi;
}

// =====================================================================================================================
// Create both LDS fence and barrier to guarantee the synchronization of LDS operations.
void NggPrimShader::createFenceAndBarrier() {
  SyncScope::ID syncScope = m_builder.getContext().getOrInsertSyncScopeID("workgroup");
  m_builder.CreateFence(AtomicOrdering::Release, syncScope);
  createBarrier();
  m_builder.CreateFence(AtomicOrdering::Acquire, syncScope);
}

// =====================================================================================================================
// Create LDS barrier to guarantee the synchronization of LDS operations.
void NggPrimShader::createBarrier() {
#if LLPC_BUILD_GFX12
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 12) {
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_signal, {}, m_builder.getInt32(WorkgroupNormalBarrierId));
    m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier_wait, {},
                              m_builder.getInt16(static_cast<uint16_t>(WorkgroupNormalBarrierId)));
    return;
  }
#endif

  m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
}

// =====================================================================================================================
// Read value from LDS.
//
// @param readTy : Type of value read from LDS
// @param ldsOffset : Start offset to do LDS read operations (in dwords)
// @param useDs128 : Whether to use 128-bit LDS load, 16-byte alignment is guaranteed by caller
// @returns : Value read from LDS
Value *NggPrimShader::readValueFromLds(Type *readTy, Value *ldsOffset, bool useDs128) {
  assert(readTy->isIntOrIntVectorTy() || readTy->isFPOrFPVectorTy());

  unsigned alignment = readTy->getScalarSizeInBits() / 8;
  if (useDs128) {
    assert(readTy->getPrimitiveSizeInBits() == 128);
    alignment = 16;
  }

  assert(m_lds);
  Value *readPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), m_lds, ldsOffset);
  readPtr = m_builder.CreateBitCast(readPtr, PointerType::get(readTy, ADDR_SPACE_LOCAL));

  return m_builder.CreateAlignedLoad(readTy, readPtr, Align(alignment));
}

// =====================================================================================================================
// Write value to LDS.
//
// @param writeValue : Value written to LDS
// @param ldsOffset : Start offset to do LDS write operations (in dwords)
// @param useDs128 : Whether to use 128-bit LDS store, 16-byte alignment is guaranteed by caller
void NggPrimShader::writeValueToLds(Value *writeValue, Value *ldsOffset, bool useDs128) {
  auto writeTy = writeValue->getType();
  assert(writeTy->isIntOrIntVectorTy() || writeTy->isFPOrFPVectorTy());

  unsigned alignment = writeTy->getScalarSizeInBits() / 8;
  if (useDs128) {
    assert(writeTy->getPrimitiveSizeInBits() == 128);
    alignment = 16;
  }

  assert(m_lds != nullptr);
  Value *writePtr = m_builder.CreateGEP(m_builder.getInt32Ty(), m_lds, ldsOffset);
  writePtr = m_builder.CreateBitCast(writePtr, PointerType::get(writeTy, ADDR_SPACE_LOCAL));

  m_builder.CreateAlignedStore(writeValue, writePtr, Align(alignment));
}

// =====================================================================================================================
// Do atomic operation with the value stored in LDS.
//
// @param atomicOp : Atomic operation
// @param value : Value to do atomic operation
// @param ldsOffset : Start offset to do LDS atomic operations (in dwords)
void NggPrimShader::atomicOp(AtomicRMWInst::BinOp atomicOp, Value *value, Value *ldsOffset) {
  assert(value->getType()->isIntegerTy(32));

  Value *atomicPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), m_lds, ldsOffset);

  SyncScope::ID syncScope = m_builder.getContext().getOrInsertSyncScopeID("workgroup");
  m_builder.CreateAtomicRMW(atomicOp, atomicPtr, value, MaybeAlign(), AtomicOrdering::SequentiallyConsistent,
                            syncScope);
}

// =====================================================================================================================
// Read value from the constant buffer.
//
// @param readTy : Type of the value read from constant buffer
// @param bufPtr : Buffer pointer
// @param offset : Dword offset from the provided buffer pointer
// @param isVolatile : Whether this is a volatile load
// @returns : Value read from the constant buffer
llvm::Value *NggPrimShader::readValueFromCb(Type *readyTy, Value *bufPtr, Value *offset, bool isVolatile) {
  assert(bufPtr->getType()->isPointerTy() && bufPtr->getType()->getPointerAddressSpace() == ADDR_SPACE_CONST);

  auto loadPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), bufPtr, offset);
  loadPtr = m_builder.CreateBitCast(loadPtr, PointerType::get(readyTy, ADDR_SPACE_CONST));
  cast<Instruction>(loadPtr)->setMetadata(MetaNameUniform, MDNode::get(m_builder.getContext(), {}));

  auto loadValue = m_builder.CreateAlignedLoad(readyTy, loadPtr, Align(4));
  if (isVolatile)
    loadValue->setVolatile(true);
  else
    loadValue->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder.getContext(), {}));

  return loadValue;
}

#if LLPC_BUILD_GFX12
// =====================================================================================================================
// Do global atomic operation with the value stored in the specified pointer
//
// @param atomicOp : Atomic operation
// @param value : Value to do global atomic operation
// @param basePtr : Base pointer
// @param offset : Dword offset from the base pointer
// @returns : Result value after doing global atomic operation
Value *NggPrimShader::globalAtomicOp(AtomicRMWInst::BinOp atomicOp, Value *value, Value *basePtr, Value *offset) {
  assert(basePtr->getType()->isPointerTy());
  assert(value->getType() == m_builder.getInt32Ty() ||
         value->getType() == m_builder.getInt64Ty()); // Must be i32 or i64

  // Cast the address space to global
  if (basePtr->getType()->getPointerAddressSpace() != ADDR_SPACE_GLOBAL)
    basePtr = m_builder.CreateAddrSpaceCast(basePtr, PointerType::get(m_builder.getContext(), ADDR_SPACE_GLOBAL));

  auto entryPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), basePtr, offset);
  if (value->getType() == m_builder.getInt64Ty())
    entryPtr = m_builder.CreateBitCast(entryPtr, PointerType::get(m_builder.getInt64Ty(), ADDR_SPACE_GLOBAL));

  return m_builder.CreateAtomicRMW(atomicOp, entryPtr, value, MaybeAlign(), AtomicOrdering::Monotonic,
                                   m_builder.getContext().getOrInsertSyncScopeID("agent"));
}

// =====================================================================================================================
// Do global ordered atomic add with the value stored in the specified pointer
//
// @param orderedIdPair : Ordered ID pair to do global ordered atomic add
// @param basePtr : Base pointer
// @param offset : Dword offset from the base pointer
// @returns : Old ordered ID pair before doing global ordered atomic add
std::pair<Value *, Value *> NggPrimShader::globalAtomicOrderedAdd(std::pair<Value *, Value *> orderedIdPair,
                                                                  Value *basePtr, Value *offset) {
  assert(basePtr->getType()->isPointerTy());
  assert(orderedIdPair.first->getType() == m_builder.getInt32Ty());
  assert(orderedIdPair.second->getType() == m_builder.getInt32Ty());

  auto int32x2Ty = FixedVectorType::get(m_builder.getInt32Ty(), 2);
  Value *value = PoisonValue::get(int32x2Ty);
  value = m_builder.CreateInsertElement(value, orderedIdPair.first, static_cast<uint64_t>(0));
  value = m_builder.CreateInsertElement(value, orderedIdPair.second, 1);
  value = m_builder.CreateBitCast(value, m_builder.getInt64Ty());

  // Cast the address space to global
  if (basePtr->getType()->getPointerAddressSpace() != ADDR_SPACE_GLOBAL)
    basePtr = m_builder.CreateAddrSpaceCast(basePtr, PointerType::get(m_builder.getContext(), ADDR_SPACE_GLOBAL));

  auto entryPtr = m_builder.CreateGEP(m_builder.getInt32Ty(), basePtr, offset);
  entryPtr = m_builder.CreateBitCast(entryPtr, PointerType::get(m_builder.getInt64Ty(), ADDR_SPACE_GLOBAL));
  Value *oldOrderedIdPair = m_builder.CreateIntrinsic(
      m_builder.getInt64Ty(), Intrinsic::amdgcn_global_atomic_ordered_add_b64, {entryPtr, value});
  oldOrderedIdPair = m_builder.CreateBitCast(oldOrderedIdPair, int32x2Ty);

  return std::make_pair(m_builder.CreateExtractElement(oldOrderedIdPair, static_cast<uint64_t>(0)),
                        m_builder.CreateExtractElement(oldOrderedIdPair, 1));
}
#endif

} // namespace lgc
