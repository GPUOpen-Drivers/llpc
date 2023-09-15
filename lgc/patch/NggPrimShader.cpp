/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  NggPrimShader.cpp
 * @brief LLPC source file: contains implementation of class lgc::NggPrimShader.
 ***********************************************************************************************************************
 */
#include "NggPrimShader.h"
#include "Gfx9Chip.h"
#include "ShaderMerger.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/Constants.h"
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
static const char NggEsCullDataFetcher[] = "lgc.ngg.ES.cull.data.fetcher";
static const char NggEsVertexExporter[] = "lgc.ngg.ES.vertex.exporter";

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

static const char NggXfbFetcher[] = "lgc.ngg.xfb.fetcher";

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
      m_nggControl(m_pipelineState->getNggControl()), m_hasVs(pipelineState->hasShaderStage(ShaderStageVertex)),
      m_hasTes(pipelineState->hasShaderStage(ShaderStageTessEval)),
      m_hasGs(pipelineState->hasShaderStage(ShaderStageGeometry)), m_builder(pipelineState->getContext()) {
  assert(m_nggControl->enableNgg);

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
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      vertexItemSizes[i] = 4 * resUsage->inOutUsage.gs.outLocCount[i];

    unsigned gsVsRingItemSizes[MaxGsStreams] = {};
    const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      gsVsRingItemSizes[i] = vertexItemSizes[i] * geometryMode.outputVertices;

    const unsigned gsPrimsPerSubgroup =
        resUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup * geometryMode.invocations;
    unsigned gsStreamBase = 0;
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      m_gsStreamBases[i] = gsStreamBase;
      gsStreamBase += gsVsRingItemSizes[i] * gsPrimsPerSubgroup;
    }
  }

  buildPrimShaderCbLayoutLookupTable();
  calcVertexCullInfoSizeAndOffsets(m_pipelineState, m_vertCullInfoOffsets);
}

// =====================================================================================================================
// Calculates the dword size of ES-GS ring item.
//
// @param pipelineState : Pipeline state
// @returns : ES-GS ring item size in dwords
unsigned NggPrimShader::calcEsGsRingItemSize(PipelineState *pipelineState) {
  assert(pipelineState->getNggControl()->enableNgg); // Must enable NGG

  // API GS is present
  if (pipelineState->hasShaderStage(ShaderStageGeometry)) {
    auto resUsage = pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
    return (4 * std::max(1u, resUsage->inOutUsage.inputMapLocCount)) | 1;
  }

  // Passthrough mode is enabled (API GS is not present)
  if (pipelineState->getNggControl()->passthroughMode) {
    unsigned esGsRingItemSize = 1;

    if (pipelineState->enableSwXfb()) {
      const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
      auto resUsage = pipelineState->getShaderResourceUsage(hasTes ? ShaderStageTessEval : ShaderStageVertex);

      // NOTE: For GFX11+, transform feedback outputs (each output is <4 x dword>) are stored as a ES-GS ring item.
      assert(resUsage->inOutUsage.xfbExpCount > 0);
      esGsRingItemSize = resUsage->inOutUsage.xfbExpCount * 4;
    }

    // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
    return esGsRingItemSize | 1;
  }

  // Culling mode is enabled (API GS is not present)
  VertexCullInfoOffsets vertCullInfoOffsets = {}; // Dummy offsets (don't care)
  // In the culling mode, the ES-GS ring item is vertex cull info.
  unsigned esGsRingItemSize = calcVertexCullInfoSizeAndOffsets(pipelineState, vertCullInfoOffsets);

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

  const auto &calcFactor = pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

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
  if (pipelineState->hasShaderStage(ShaderStageGeometry)) {
    PrimShaderLdsUsageInfo ldsUsageInfo = {};
    ldsUsageInfo.needsLds = true;

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
      // NOTE: We round ES-GS LDS size to 4-dword alignment. This is for later LDS read/write operations of mutilple
      // dwords (such as DS128).
      ldsRegionSize = alignTo(calcFactor.esGsLdsSize, 4U);

      printLdsRegionInfo("ES-GS Ring", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::EsGsRing] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }

    // Primitive data
    ldsRegionSize = Gfx9::NggMaxThreadsPerSubgroup * MaxGsStreams; // 1 dword per primitive thread, 4 GS streams
    if (ldsLayout) {
      printLdsRegionInfo("Primitive Connectivity Data", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::PrimitiveData] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }
    ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;

    // Primitive counts
    if (pipelineState->enableSwXfb()) {
      ldsRegionSize =
          (Gfx9::NggMaxWavesPerSubgroup + 1) * MaxGsStreams; // 1 dword per wave and 1 dword per subgroup, 4 GS streams
      if (ldsLayout) {
        printLdsRegionInfo("Primitive Counts", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::PrimitiveCounts] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
    }

    // Primitive index map (compacted -> uncompacted)
    if (pipelineState->enableSwXfb()) {
      ldsRegionSize = Gfx9::NggMaxThreadsPerSubgroup * MaxGsStreams; // 1 dword per primitive thread, 4 GS streams
      if (ldsLayout) {
        printLdsRegionInfo("Primitive Index Map (To Uncompacted)", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::PrimitiveIndexMap] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
      ldsUsageInfo.gsExtraLdsSize += ldsRegionSize;
    }

    // Vertex counts
    if (pipelineState->enableSwXfb()) {
      if (ldsLayout) {
        // NOTE: If SW emulated stream-out is enabled, this region is overlapped with PrimitiveCounts
        (*ldsLayout)[PrimShaderLdsRegion::VertexCounts] = (*ldsLayout)[PrimShaderLdsRegion::PrimitiveCounts];
        printLdsRegionInfo("Vertex Counts", (*ldsLayout)[PrimShaderLdsRegion::VertexCounts].first,
                           (*ldsLayout)[PrimShaderLdsRegion::VertexCounts].second);
      }
    } else {
      ldsRegionSize =
          (Gfx9::NggMaxWavesPerSubgroup + 1) * MaxGsStreams; // 1 dword per wave and 1 dword per subgroup, 4 GS streams
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
          // NOTE: If SW emulated stream-out is enabled, this region is overlapped with PrimitiveIndexMap
          (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap] = (*ldsLayout)[PrimShaderLdsRegion::PrimitiveIndexMap];
          printLdsRegionInfo("Vertex Index Map (To Uncompacted)",
                             (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap].first,
                             (*ldsLayout)[PrimShaderLdsRegion::VertexIndexMap].second);
        }
      } else {
        ldsRegionSize = Gfx9::NggMaxThreadsPerSubgroup * MaxGsStreams; // 1 dword per vertex thread, 4 GS streams
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
      ldsRegionSize = calcFactor.gsOnChipLdsSize - esGsRingLdsSize - ldsUsageInfo.gsExtraLdsSize;

      printLdsRegionInfo("GS-VS Ring", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::GsVsRing] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }

    if (ldsLayout) {
      printLdsRegionInfo("Total LDS", 0, ldsOffset);
      LLPC_OUTS("\n");
      LLPC_OUTS("Needs LDS = " << (ldsUsageInfo.needsLds ? "true" : "false") << "\n");
      LLPC_OUTS("ES Extra LDS Size (in Dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.esExtraLdsSize) << "\n");
      LLPC_OUTS("GS Extra LDS Size (in Dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.gsExtraLdsSize) << "\n");
      LLPC_OUTS("\n");
    }

    return ldsUsageInfo;
  }

  const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool distributePrimitiveId =
      !hasTes && pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.primitiveId;

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
        ldsRegionSize = calcFactor.esVertsPerSubgroup; // 1 dword per vertex thread

        printLdsRegionInfo("Distributed Primitive ID", ldsOffset, ldsRegionSize);
        (*ldsLayout)[PrimShaderLdsRegion::DistributedPrimitiveId] = std::make_pair(ldsOffset, ldsRegionSize);
        ldsOffset += ldsRegionSize;
      }
    }

    ldsOffset = 0; // DistributedPrimitiveId is always the first region and is overlapped with XfbOutput

    // XFB outputs
    if (pipelineState->enableSwXfb()) {
      if (ldsLayout) {
        ldsRegionSize = calcFactor.esVertsPerSubgroup *
                        calcFactor.esGsRingItemSize; // Transform feedback outputs are stored as a ES-GS ring item

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
      LLPC_OUTS("ES Extra LDS Size (in Dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.esExtraLdsSize) << "\n");
      LLPC_OUTS("GS Extra LDS Size (in Dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.gsExtraLdsSize) << "\n");
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
      ldsRegionSize = calcFactor.esVertsPerSubgroup; // 1 dword per vertex thread

      printLdsRegionInfo("Distributed Primitive ID", ldsOffset, ldsRegionSize);
      (*ldsLayout)[PrimShaderLdsRegion::DistributedPrimitiveId] = std::make_pair(ldsOffset, ldsRegionSize);
      ldsOffset += ldsRegionSize;
    }
  }

  ldsOffset = 0; // DistributedPrimitiveId is always the first region and is overlapped with VertexPosition

  // Vertex position
  ldsRegionSize = 4 * Gfx9::NggMaxThreadsPerSubgroup; // 4 dwords per vertex thread
  if (ldsLayout) {
    printLdsRegionInfo("Vertex Position", ldsOffset, ldsRegionSize);
    (*ldsLayout)[PrimShaderLdsRegion::VertexPosition] = std::make_pair(ldsOffset, ldsRegionSize);
    ldsOffset += ldsRegionSize;
  }
  ldsUsageInfo.esExtraLdsSize += ldsRegionSize;

  // Vertex cull info
  if (ldsLayout) {
    ldsRegionSize =
        calcFactor.esGsRingItemSize * calcFactor.esVertsPerSubgroup; // Vertex cull info is stored as a ES-GS ring item

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
  ldsRegionSize = Gfx9::NggMaxWavesPerSubgroup + 1; // 1 dword per wave and 1 dword per subgroup
  if (ldsLayout) {
    printLdsRegionInfo("Vertex Counts", ldsOffset, ldsRegionSize);
    (*ldsLayout)[PrimShaderLdsRegion::VertexCounts] = std::make_pair(ldsOffset, ldsRegionSize);
    ldsOffset += ldsRegionSize;
  }
  ldsUsageInfo.esExtraLdsSize += ldsRegionSize;

  // Vertex index map
  if (pipelineState->getNggControl()->compactVertex) {
    ldsRegionSize = Gfx9::NggMaxThreadsPerSubgroup; // 1 dword per wave and 1 dword per subgroup
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
    LLPC_OUTS("ES Extra LDS Size (in Dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.esExtraLdsSize) << "\n");
    LLPC_OUTS("GS Extra LDS Size (in Dwords) = " << format("0x%04" PRIX32, ldsUsageInfo.gsExtraLdsSize) << "\n");
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
  assert(m_gfxIp.major >= 10);

  // ES and GS could not be null at the same time
  assert((!esMain && !gsMain) == false);

  // TODO: support counting generated primitives in software emulated stream-out
  assert(!m_pipelineState->enablePrimStats());

  // Assign names to ES, GS and copy shader main functions
  Module *module = nullptr;
  if (esMain) {
    module = esMain->getParent();

    esMain->setName(NggEsMain);
    esMain->setCallingConv(CallingConv::AMDGPU_ES);
    esMain->setLinkage(GlobalValue::InternalLinkage);
    esMain->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    esMain->addFnAttr(Attribute::AlwaysInline);
    m_esHandlers.main = esMain;
  }

  if (gsMain) {
    module = gsMain->getParent();

    gsMain->setName(NggGsMain);
    gsMain->setCallingConv(CallingConv::AMDGPU_GS);
    gsMain->setLinkage(GlobalValue::InternalLinkage);
    gsMain->setDLLStorageClass(GlobalValue::DefaultStorageClass);
    gsMain->addFnAttr(Attribute::AlwaysInline);
    m_gsHandlers.main = gsMain;

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

  Function *primShader = Function::Create(primShaderTy, GlobalValue::ExternalLinkage, lgcName::NggPrimShaderEntryPoint);
  primShader->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  primShader->addFnAttr("target-features", ",+wavefrontsize" + std::to_string(waveSize)); // Set wavefront size
  primShader->addFnAttr("amdgpu-flat-work-group-size",
                        "128,128"); // Force s_barrier to be present (ignore optimization)

  module->getFunctionList().push_front(primShader);

  SmallVector<Argument *, 32> args;
  for (auto &arg : primShader->args()) {
    auto argIdx = arg.getArgNo();
    if (inRegMask & (1ull << argIdx))
      arg.addAttr(Attribute::InReg);
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
    llvm_unreachable("Not implemented!");
  }

  // Setup LDS layout
  m_lds = Patch::getLdsVariable(m_pipelineState, module);
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
// @param [out] vertCullInfoOffsets : The collection of LDS offsets to build
// @returns : Dword size of vertex cull info
unsigned NggPrimShader::calcVertexCullInfoSizeAndOffsets(PipelineState *pipelineState,
                                                         VertexCullInfoOffsets &vertCullInfoOffsets) {
  auto nggControl = pipelineState->getNggControl();
  assert(nggControl->enableNgg);

  vertCullInfoOffsets = {};

  // Only for NGG culling mode without API GS
  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  if (hasGs || nggControl->passthroughMode)
    return 0;

  unsigned cullInfoSize = 0;
  unsigned cullInfoOffset = 0;
  unsigned itemSize = 0;

  if (pipelineState->enableSwXfb()) {
    const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
    auto resUsage = pipelineState->getShaderResourceUsage(hasTes ? ShaderStageTessEval : ShaderStageVertex);

    // NOTE: Each transform feedback output is <4 x dword>.
    const unsigned xfbOutputCount = resUsage->inOutUsage.xfbExpCount;
    itemSize = sizeof(VertexCullInfo::xfbOutputs) * xfbOutputCount / sizeof(unsigned);
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

    const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
    if (hasTes) {
      auto builtInUsage = pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes;
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
      auto builtInUsage = pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs;
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

  const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
  const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
  const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);

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
    llvm_unreachable("Not implemented!");
  }

  // If the ES is the API VS, and it is a fetchless VS, then we need to add args for the vertex fetches.
  if (!m_hasTes) {
    unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
    if (vertexFetchCount != 0) {
      // TODO: This will not work with non-GS culling.
      if (!m_hasGs && !m_nggControl->passthroughMode)
        m_pipelineState->setError("Fetchless VS in NGG culling mode (without API GS) not supported");
      // The final vertexFetchCount args of the ES (API VS) are the vertex fetches.
      unsigned esArgSize = m_esHandlers.main->arg_size();
      for (unsigned idx = esArgSize - vertexFetchCount; idx != esArgSize; ++idx)
        argTys.push_back(m_esHandlers.main->getArg(idx)->getType());
    }
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
// Calculate the dword offset of each item in the stream-out control buffer
void NggPrimShader::calcStreamOutControlCbOffsets() {
  assert(m_pipelineState->enableSwXfb());

  m_streamOutControlCbOffsets = {};

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    m_streamOutControlCbOffsets.bufOffsets[i] = (offsetof(Util::Abi::StreamOutControlCb, bufOffsets[0]) +
                                                 sizeof(Util::Abi::StreamOutControlCb::bufOffsets[0]) * i) /
                                                4;
  }
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
    llvm_unreachable("Not implemented!");

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
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Export primitive
  //
  //   if (Enable SW XFB)
  //     Process SW XFB (Run ES)
  //   else {
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Run ES (export vertex)
  //   }
  // }
  //

  // Define basic blocks
  auto entryBlock = createBlock(primShader, ".entry");

  auto sendGsAllocReqBlock = createBlock(primShader, ".sendGsAllocReq");
  auto endSendGsAllocReqBlock = createBlock(primShader, ".endSendGsAllocReq");

  auto exportPrimitiveBlock = createBlock(primShader, ".exportPrimitive");
  auto endExportPrimitiveBlock = createBlock(primShader, ".endExportPrimitive");

  auto exportVertexBlock = createBlock(primShader, ".exportVertex");
  auto endExportVertexBlock = createBlock(primShader, ".endExportVertex");

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      // Record attribute ring base ([14:0])
      m_nggInputs.attribRingBase = createUBfe(attribRingBase, 0, 15);

      if (m_pipelineState->enableSwXfb())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive connectivity data
    m_nggInputs.primData = primData;

    // Primitive connectivity data have such layout:
    //
    //   +----------------+---------------+---------------+---------------+
    //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
    //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
    //   +----------------+---------------+---------------+---------------+

    // Record relative vertex indices
    if (m_gfxIp.major <= 11) {
      m_nggInputs.vertexIndex0 = createUBfe(primData, 0, 9);
      m_nggInputs.vertexIndex1 = createUBfe(primData, 10, 9);
      m_nggInputs.vertexIndex2 = createUBfe(primData, 20, 9);
    } else {
      llvm_unreachable("Not implemented!");
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

    if (m_pipelineState->enableSwXfb())
      processSwXfb(args);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, exportVertexBlock, endExportVertexBlock);
  }

  // Construct ".exportVertex" block
  {
    m_builder.SetInsertPoint(exportVertexBlock);

    // NOTE: For NGG passthrough mode, if SW-emulated stream-out is enabled, running ES is included in processing
    // transform feedback exporting. There won't be separated ES running (ES is not split any more). This is
    // because we could encounter special cases in which there are memory atomics producing output values both for
    // transform feedback exporting and for vertex exporting like following codes. The atomics shouldn't be separated
    // and be run multiple times.
    //
    //   void ES() {
    //     ...
    //     value = atomicXXX()
    //     xfbExport = value
    //     vertexExport = value
    //  }
    if (!m_pipelineState->enableSwXfb())
      runEs(args);

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

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

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
    llvm_unreachable("Not implemented!");
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
  //   if (Enable SW XFB)
  //     Process SW XFB
  //
  //   if (threadIdInWave < vertCountInWave)
  //     Run part ES to fetch vertex cull data
  //
  //   if (Not runtime passthrough) {
  //     if (threadIdInSubgroup < vertCountInSubgroup)
  //       Initialize vertex draw flag
  //     if (threadIdInSubgroup < waveCount + 1)
  //       Initialize per-wave and per-subgroup count of output vertices
  //
  //     if (threadIdInWave < vertCountInWave)
  //       Write vertex cull data
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
  //     if (threadIdInWave < waveCount - waveId)
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
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Export primitive
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     if (Needn't compact vertex && empty wave)
  //       Dummy vertex export
  //     else
  //       Run part ES to do deferred vertex export
  //   }
  // }
  //

  // Export count when the entire subgroup is fully culled
  const bool waNggCullingNoEmptySubgroups =
      m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups;
  const unsigned dummyExportCount = waNggCullingNoEmptySubgroups ? 1 : 0;

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;

  // NOTE: Make sure vertex position data is 4-dword alignment because we will use 128-bit LDS read/write for it.
  assert(getLdsRegionStart(PrimShaderLdsRegion::VertexPosition) % 4U == 0);

  if (!m_nggControl->compactVertex)
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  // Define basic blocks
  auto entryBlock = createBlock(primShader, ".entry");

  auto checkFetchVertexCullDataBlock = createBlock(primShader, ".checkFetchVertexCullData");
  auto fetchVertexCullDataBlock = createBlock(primShader, ".fetchVertexCullData");
  auto endFetchVertexCullDataBlock = createBlock(primShader, ".endFetchVertexCullData");

  auto checkInitVertexDrawFlagBlock = createBlock(primShader, ".checkInitVertexDrawFlag");
  auto initVertexDrawFlagBlock = createBlock(primShader, ".initVertexDrawFlag");
  auto endInitVertexDrawFlagBlock = createBlock(primShader, ".endInitVertexDrawFlag");

  auto initVertexCountsBlock = createBlock(primShader, ".initVertexCounts");
  auto endInitVertexCountsBlock = createBlock(primShader, ".endInitVertexCounts");

  auto writeVertexCullDataBlock = createBlock(primShader, ".writeVertexCullData");
  auto endWriteVertexCullDataBlock = createBlock(primShader, ".endWriteVertexCullData");

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
  auto checkExportPrimitiveBlock = createBlock(primShader, ".checkExportPrimitive");

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
      // Record attribute ring base ([14:0])
      m_nggInputs.attribRingBase = createUBfe(attribRingBase, 0, 15);

      if (m_pipelineState->enableSwXfb())
        loadStreamOutBufferInfo(userData);
    }

    // Record primitive shader table address info
    m_nggInputs.primShaderTableAddr = std::make_pair(primShaderTableAddrLow, primShaderTableAddrHigh);

    // Record vertex indices
    if (m_gfxIp.major <= 11) {
      m_nggInputs.vertexIndex0 = createUBfe(vgprArgs[0], 0, 16);
      m_nggInputs.vertexIndex1 = createUBfe(vgprArgs[0], 16, 16);
      m_nggInputs.vertexIndex2 = createUBfe(vgprArgs[1], 0, 16);
    } else {
      llvm_unreachable("Not implemented!");
    }

    vertexItemOffset = m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(esGsRingItemSize));

    // Distribute primitive ID if needed
    distributePrimitiveId(primitiveId);

    // Process SW XFB
    if (m_pipelineState->enableSwXfb())
      processSwXfb(args);

    m_builder.CreateBr(checkFetchVertexCullDataBlock);
  }

  // Construct ".checkFetchVertexCullData" block
  {
    m_builder.SetInsertPoint(checkFetchVertexCullDataBlock);

    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_nggInputs.vertCountInWave);
    m_builder.CreateCondBr(validVertex, fetchVertexCullDataBlock, endFetchVertexCullDataBlock);
  }

  // Construct ".fetchVertexCullData" block
  Value *cullData = nullptr;
  Value *position = nullptr;
  {
    m_builder.SetInsertPoint(fetchVertexCullDataBlock);

    // Split ES to two parts: fetch cull data before NGG culling; do deferred vertex export after NGG culling
    splitEs();

    // Run part ES to fetch cull data
    auto cullData = runPartEs(args);
    position = m_nggControl->enableCullDistanceCulling ? m_builder.CreateExtractValue(cullData, 0) : cullData;

    m_builder.CreateBr(endFetchVertexCullDataBlock);
  }

  // Construct ".endFetchVertexCullData" block
  {
    m_builder.SetInsertPoint(endFetchVertexCullDataBlock);

    position = createPhi(
        {{position, fetchVertexCullDataBlock}, {PoisonValue::get(position->getType()), checkFetchVertexCullDataBlock}},
        "position"); // Update vertex position data

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
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(waveCountInSubgroup + 1));
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
    m_builder.CreateCondBr(validVertex, writeVertexCullDataBlock, endWriteVertexCullDataBlock);
  }

  // Construct ".writeVertexCullData" block
  {
    m_builder.SetInsertPoint(writeVertexCullDataBlock);

    // Write vertex position data
    writePerThreadDataToLds(position, m_nggInputs.threadIdInSubgroup, PrimShaderLdsRegion::VertexPosition, 0, true);

    // Write cull distance sign mask
    if (m_nggControl->enableCullDistanceCulling) {
      auto cullDistance = m_builder.CreateExtractValue(cullData, 1);

      // Calculate the sign mask for cull distance
      Value *signMask = m_builder.getInt32(0);
      for (unsigned i = 0; i < cullDistance->getType()->getArrayNumElements(); ++i) {
        auto cullDistanceVal = m_builder.CreateExtractValue(cullDistance, i);
        cullDistanceVal = m_builder.CreateBitCast(cullDistanceVal, m_builder.getInt32Ty());

        Value *signBit = createUBfe(cullDistanceVal, 31, 1);
        signBit = m_builder.CreateShl(signBit, i);

        signMask = m_builder.CreateOr(signMask, signBit);
      }

      writeVertexCullInfoToLds(signMask, vertexItemOffset, m_vertCullInfoOffsets.cullDistanceSignMask);
    }

    m_builder.CreateBr(endWriteVertexCullDataBlock);
  }

  // Construct ".endWriteVertexCullData" block
  {
    m_builder.SetInsertPoint(endWriteVertexCullDataBlock);

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
                                 {m_builder.getTrue(), endWriteVertexCullDataBlock}});

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

    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(waveCountInSubgroup), m_nggInputs.waveIdInSubgroup);
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
    atomicAdd(vertCountInWave, ldsOffset);

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
                                                    {vertCountInWaves, m_builder.getInt32(waveCountInSubgroup)});

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

      const auto resUsage = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
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
          createPhi({{hasCulledVertices, endCompactVertexBlock}, {m_builder.getFalse(), endFetchVertexCullDataBlock}},
                    "compactVertex");
    } else {
      assert(!m_compactVertex); // Must be null
    }

    // Update primitive culled flag
    primitiveCulled =
        createPhi({{primitiveCulled, endCompactVertexBlock}, {m_builder.getFalse(), endFetchVertexCullDataBlock}},
                  "primitiveCulled");

    // Update fully-culled flag
    fullyCulled = createPhi({{fullyCulled, endCompactVertexBlock}, {m_builder.getFalse(), endFetchVertexCullDataBlock}},
                            "fullyCulled");

    // Update primitive count in subgroup
    m_nggInputs.primCountInSubgroup = createPhi(
        {{primCountInSubgroup, endCompactVertexBlock}, {m_nggInputs.primCountInSubgroup, endFetchVertexCullDataBlock}},
        "primCountInSubgroup");

    // Update vertex count in subgroup
    m_nggInputs.vertCountInSubgroup = createPhi(
        {{vertCountInSubgroup, endCompactVertexBlock}, {m_nggInputs.vertCountInSubgroup, endFetchVertexCullDataBlock}},
        "vertCountInSubgroup");

    if (!m_nggControl->compactVertex) {
      // Update draw flag
      drawFlag = createPhi({{drawFlag, endCompactVertexBlock}, {m_builder.getTrue(), endFetchVertexCullDataBlock}},
                           "drawFlag");

      // Update vertex count in wave
      vertCountInWave = createPhi(
          {{vertCountInWave, endCompactVertexBlock}, {m_nggInputs.vertCountInWave, endFetchVertexCullDataBlock}},
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
      m_builder.CreateCondBr(fullyCulled, earlyExitBlock, checkExportPrimitiveBlock);
    else
      m_builder.CreateBr(checkExportPrimitiveBlock);
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

  // Construct ".checkExportPrimitive" block
  {
    m_builder.SetInsertPoint(checkExportPrimitiveBlock);

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

    auto validVertex = m_nggControl->compactVertex
                           ? m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup)
                           : drawFlag;
    m_builder.CreateCondBr(validVertex, exportVertexBlock, endExportVertexBlock);
  }

  // Construct ".exportVertex" block
  {
    m_builder.SetInsertPoint(exportVertexBlock);

    // Run part ES to do deferred vertex export
    runPartEs(args, position);

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

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  if (!m_nggControl->compactVertex)
    assert(m_gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+

  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;
  const bool cullingMode = !m_nggControl->passthroughMode;

  const auto rasterStream = m_pipelineState->getRasterizerState().rasterStream;

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
  //   if (Enable SW XFB)
  //     Process SW XFB
  //
  //  if (threadIdInSubgroup < waveCount + 1)
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
  //   if (threadIdInWave < waveCount - waveId)
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
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Export primitive
  //
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     if (Needn't compact vertex && empty wave)
  //       Dummy vertex export
  //     else
  //       Run copy shader (export vertex)
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

  auto initVertexCountsBlock = createBlock(primShader, ".initVertexCounts");
  auto endInitVertexCountsBlock = createBlock(primShader, ".endInitVertexCounts");

  auto cullPrimitiveBlock = createBlock(primShader, ".cullPrimitive");
  auto nullifyPrimitiveDataBlock = createBlock(primShader, ".nullifyPrimitiveData");
  auto endCullPrimitiveBlock = createBlock(primShader, ".endCullPrimitive");

  auto checkVertexDrawFlagBlock = createBlock(primShader, ".checkVertexDrawFlag");
  auto endCheckVertexDrawFlagBlock = createBlock(primShader, ".endCheckVertexDrawFlag");

  auto accumVertexCountsBlock = createBlock(primShader, ".accumVertexCounts");
  auto endAccumVertexCountsBlock = createBlock(primShader, ".endAccumVertexCounts");

  auto compactVertexIndexBlock = createBlock(primShader, ".compactVertexIndex");
  auto endCompactVertexIndexBlock = createBlock(primShader, ".endCompactVertexIndex");

  auto sendGsAllocReqBlock = createBlock(primShader, ".sendGsAllocReq");
  auto endSendGsAllocReqBlock = createBlock(primShader, ".endSendGsAllocReq");

  auto exportPrimitiveBlock = createBlock(primShader, ".exportPrimitive");
  auto endExportPrimitiveBlock = createBlock(primShader, ".endExportPrimitive");

  auto checkEmptyWaveBlock = createBlock(primShader, ".checkEmptyWave");
  auto dummyVertexExportBlock = createBlock(primShader, ".dummyVertexExport");
  auto checkExportVertexBlock = createBlock(primShader, ".checkExportVertex");

  auto exportVertexBlock = createBlock(primShader, ".exportVertex");
  auto endExportVertexBlock = createBlock(primShader, ".endExportVertex");

  // Construct ".entry" block
  {
    m_builder.SetInsertPoint(entryBlock);

    initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

    if (m_gfxIp.major >= 11) {
      // Record attribute ring base ([14:0])
      m_nggInputs.attribRingBase = createUBfe(attribRingBase, 0, 15);

      if (m_pipelineState->enableSwXfb())
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
                                PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * i);
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

    if (m_pipelineState->enableSwXfb())
      processSwXfbWithGs(args);

    auto validWave =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(waveCountInSubgroup + 1));
    m_builder.CreateCondBr(validWave, initVertexCountsBlock, endInitVertexCountsBlock);
  }

  // Construct ".initVertexCounts" block
  {
    m_builder.SetInsertPoint(initVertexCountsBlock);

    writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup, PrimShaderLdsRegion::VertexCounts,
                            (Gfx9::NggMaxWavesPerSubgroup + 1) * rasterStream);

    m_builder.CreateBr(endInitVertexCountsBlock);
  }

  // Construct ".endInitVertexCounts" block
  Value *primData = nullptr;
  {
    m_builder.SetInsertPoint(endInitVertexCountsBlock);

    createFenceAndBarrier();

    if (cullingMode) {
      primData =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                   PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
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
                              PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * rasterStream);

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
                                 PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
    auto drawFlag0 = m_builder.CreateICmpNE(primData0, m_builder.getInt32(NullPrim));
    drawFlag = drawFlag0;

    if (outVertsPerPrim > 1) {
      // drawFlag |= N >= 1 ? (primData[N-1] != NullPrim) : false
      auto primData1 = readPerThreadDataFromLds(
          m_builder.getInt32Ty(), m_builder.CreateSub(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(1)),
          PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
      auto drawFlag1 =
          m_builder.CreateSelect(m_builder.CreateICmpUGE(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(1)),
                                 m_builder.CreateICmpNE(primData1, m_builder.getInt32(NullPrim)), m_builder.getFalse());
      drawFlag = m_builder.CreateOr(drawFlag, drawFlag1);
    }

    if (outVertsPerPrim > 2) {
      // drawFlag |= N >= 2 ? (primData[N-2] != NullPrim) : false
      auto primData2 = readPerThreadDataFromLds(
          m_builder.getInt32Ty(), m_builder.CreateSub(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(2)),
          PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
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

    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(waveCountInSubgroup), m_nggInputs.waveIdInSubgroup);
    auto validThread = m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, threadIdUpbound);

    m_builder.CreateCondBr(validThread, accumVertexCountsBlock, endAccumVertexCountsBlock);
  }

  // Construct ".accumVertexCounts" block
  {
    m_builder.SetInsertPoint(accumVertexCountsBlock);

    auto ldsOffset = m_builder.CreateAdd(m_nggInputs.waveIdInSubgroup, m_nggInputs.threadIdInWave);
    ldsOffset = m_builder.CreateAdd(ldsOffset, m_builder.getInt32(1));

    unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCounts);

    ldsOffset = m_builder.CreateAdd(
        ldsOffset, m_builder.getInt32(regionStart + (Gfx9::NggMaxWavesPerSubgroup + 1) * rasterStream));
    atomicAdd(vertCountInWave, ldsOffset);

    m_builder.CreateBr(endAccumVertexCountsBlock);
  }

  // Construct ".endAccumVertexCounts" block
  Value *vertCountInPrevWaves = nullptr;
  {
    m_builder.SetInsertPoint(endAccumVertexCountsBlock);

    createFenceAndBarrier();

    if (m_nggControl->compactVertex) {
      auto vertCountInWaves = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave,
                                                       PrimShaderLdsRegion::VertexCounts,
                                                       (Gfx9::NggMaxWavesPerSubgroup + 1) * rasterStream);

      // The last dword following dwords for all waves (each wave has one dword) stores GS output vertex count of the
      // entire subgroup
      auto vertCountInSubgroup = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                           {vertCountInWaves, m_builder.getInt32(waveCountInSubgroup)});

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

    auto validVertex = m_nggControl->compactVertex
                           ? m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup)
                           : drawFlag;
    m_builder.CreateCondBr(validVertex, exportVertexBlock, endExportVertexBlock);
  }

  // Construct ".exportVertex" block
  {
    m_builder.SetInsertPoint(exportVertexBlock);

    runCopyShader(args);

    m_builder.CreateBr(endExportVertexBlock);
  }

  // Construct ".endExportVertex" block
  {
    m_builder.SetInsertPoint(endExportVertexBlock);

    m_builder.CreateRetVoid();
  }
}

// =====================================================================================================================
// Extracts merged group/wave info and initializes part of NGG calculation factors.
//
// NOTE: This function must be invoked by the entry block of NGG shader module.
//
// @param mergedGroupInfo : Merged group info
// @param mergedWaveInfo : Merged wave info
void NggPrimShader::initWaveThreadInfo(Value *mergedGroupInfo, Value *mergedWaveInfo) {
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
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
// Load stream-out info including stream-out buffer descriptors and buffer offsets.
//
// @param userData : User data
void NggPrimShader::loadStreamOutBufferInfo(Value *userData) {
  assert(m_pipelineState->enableSwXfb()); // Must enable SW emulated stream-out

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

  // Get stream-out table pointer value and stream-out control buffer pointer value
  const auto gsOrEsMain = m_hasGs ? m_gsHandlers.main : m_esHandlers.main;
  StreamOutData streamOutData = {};
  if (m_hasGs)
    streamOutData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs.streamOutData;
  else if (m_hasTes)
    streamOutData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval)->entryArgIdxs.tes.streamOutData;
  else
    streamOutData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex)->entryArgIdxs.vs.streamOutData;

  assert(userData->getType()->isVectorTy());
  auto streamOutTablePtrValue =
      m_builder.CreateExtractElement(userData, getUserDataIndex(gsOrEsMain, streamOutData.tablePtr));
  auto streamOutControlBufPtrValue =
      m_builder.CreateExtractElement(userData, getUserDataIndex(gsOrEsMain, streamOutData.controlBufPtr));

  // Helper to make a pointer from its integer address value and the type
  auto makePointer = [&](Value *ptrValue, Type *ptrTy) {
    Value *pc = m_builder.CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
    pc = m_builder.CreateBitCast(pc, FixedVectorType::get(m_builder.getInt32Ty(), 2));

    Value *ptr = m_builder.CreateInsertElement(pc, ptrValue, static_cast<uint64_t>(0));
    ptr = m_builder.CreateBitCast(ptr, m_builder.getInt64Ty());
    ptr = m_builder.CreateIntToPtr(ptr, ptrTy);

    return ptr;
  };

  const auto constBufferPtrTy = PointerType::get(m_builder.getContext(), ADDR_SPACE_CONST);
  auto streamOutTablePtr = makePointer(streamOutTablePtrValue, constBufferPtrTy);
  m_streamOutControlBufPtr = makePointer(streamOutControlBufPtrValue, constBufferPtrTy);

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    bool bufferActive = xfbStrides[i] > 0;
    if (!bufferActive)
      continue; // Transform feedback buffer inactive

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

// =====================================================================================================================
// Distribute primitive ID from primitive-based to vertex-based.
//
// @param primitiveId : Primitive ID to distribute (primitive-based, provided by GE)
void NggPrimShader::distributePrimitiveId(Value *primitiveId) {
  // NOTE: If primitive ID is used in VS-FS pipeline, we have to distribute the value across LDS because the primitive
  // ID is provided as primitive-based instead of vertex-based.
  if (m_hasGs || m_hasTes)
    return; // Not VS-PS pipeline

  if (!m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.primitiveId)
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
             primitiveType == PrimitiveType::TriangleStripAdjacency);
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
          m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;

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
  //   +----------------+---------------+---------------+---------------+
  //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
  //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
  //   +----------------+---------------+---------------+---------------+
  Value *primData = nullptr;
  if (m_gfxIp.major <= 11) {
    primData = m_builder.CreateShl(vertexIndex2, 10);
    primData = m_builder.CreateOr(primData, vertexIndex1);

    primData = m_builder.CreateShl(primData, 10);
    primData = m_builder.CreateOr(primData, vertexIndex0);
  } else {
    llvm_unreachable("Not implemented!");
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
  Value *primData =
      readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                               PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * rasterStream);
  auto validPrimitive = m_builder.CreateICmpNE(primData, m_builder.getInt32(NullPrim));

  // Primitive connectivity data have such layout:
  //
  //   +----------------+---------------+---------------+---------------+
  //   | Null Primitive | Vertex Index2 | Vertex Index1 | Vertex Index0 |
  //   | [31]           | [28:20]       | [18:10]       | [8:0]         |
  //   +----------------+---------------+---------------+---------------+
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
      llvm_unreachable("Not implemented!");
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
      llvm_unreachable("Not implemented!");
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
      const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      miscExport |= builtInUsage.primitiveShadingRate;
      if (miscExport)
        ++posExpCount;

      posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
    } else if (m_hasTes) {
      const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.tes;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      if (miscExport)
        ++posExpCount;

      posExpCount += (builtInUsage.clipDistance + builtInUsage.cullDistance) / 4;
    } else {
      const auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.vs;

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
  if (!m_hasTes && !m_hasVs) {
    // No TES or VS, don't have to run
    return;
  }

  if (m_gfxIp.major >= 11 && !m_hasGs) // For GS, vertex attribute exports are in copy shader
    processVertexAttribExport(m_esHandlers.main);

  Value *esGsOffset = nullptr;
  if (m_hasGs) {
    auto &calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
    unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    unsigned esGsBytesPerWave = waveSize * sizeof(unsigned) * calcFactor.esGsRingItemSize;
    esGsOffset = m_builder.CreateMul(m_nggInputs.waveIdInSubgroup, m_builder.getInt32(esGsBytesPerWave));
  }

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
    llvm_unreachable("Not implemented!");
  }

  SmallVector<Value *, 32> esArgs;

  // Setup attribute ring base and relative vertex index in subgroup as two additional arguments to export vertex
  // attributes through memory
  if (m_gfxIp.major >= 11 && !m_hasGs) { // For GS, vertex attribute exports are in copy shader
    const auto attribCount = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)
                                 ->inOutUsage.expCount;
    if (attribCount > 0) {
      esArgs.push_back(m_nggInputs.attribRingBase);
      esArgs.push_back(m_nggInputs.threadIdInSubgroup);
    }
  }

  // Set up user data SGPRs
  const unsigned userDataCount =
      m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)->userDataCount;
  appendUserData(esArgs, m_esHandlers.main, userData, userDataCount);

  if (m_hasTes) {
    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      Value *isOffChip = PoisonValue::get(m_builder.getInt32Ty()); // Unused
      esArgs.push_back(m_hasGs ? offChipLdsBase : isOffChip);
      esArgs.push_back(m_hasGs ? isOffChip : offChipLdsBase);
    }

    if (m_hasGs)
      esArgs.push_back(esGsOffset);

    // Set up system value VGPRs
    esArgs.push_back(tessCoordX);
    esArgs.push_back(tessCoordY);
    esArgs.push_back(relPatchId);
    esArgs.push_back(patchId);
  } else {
    // Set up system value SGPRs
    if (m_hasGs)
      esArgs.push_back(esGsOffset);

    // Set up system value VGPRs
    esArgs.push_back(vertexId);
    esArgs.push_back(relVertexId);
    esArgs.push_back(vsPrimitiveId);
    esArgs.push_back(instanceId);

    // When tessellation is not enabled, the ES is actually a fetchless VS. Then, we need to add arguments for the
    // vertex fetches. Also set the name of each vertex fetch primitive shader argument while we're here.
    unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
    if (vertexFetchCount > 0) {
      ArrayRef<Argument *> vertexFetches = vgprArgs.drop_front(m_gfxIp.major <= 11 ? 9 : 5);
      assert(vertexFetches.size() == vertexFetchCount);

      for (unsigned i = 0; i < vertexFetchCount; ++i) {
        vertexFetches[i]->setName(m_esHandlers.main->getArg(m_esHandlers.main->arg_size() - vertexFetchCount + i)
                                      ->getName()); // Copy argument name
        esArgs.push_back(vertexFetches[i]);
      }
    }
  }

  assert(esArgs.size() == m_esHandlers.main->arg_size()); // Must have visit all arguments of ES entry point

  CallInst *esCall = m_builder.CreateCall(m_esHandlers.main, esArgs);
  esCall->setCallingConv(CallingConv::AMDGPU_ES);
}

// =====================================================================================================================
// Runs part ES. Before doing this, ES must have been already split to two parts: one is to fetch cull data for
// NGG culling; the other is to do deferred vertex export.
//
// @param args : Arguments of primitive shader entry-point
// @param position : Vertex position data (if provided, the part ES is to do deferred vertex export)
Value *NggPrimShader::runPartEs(ArrayRef<Argument *> args, Value *position) {
  assert(m_hasGs == false);                       // GS must not be present
  assert(m_nggControl->passthroughMode == false); // NGG culling is enabled

  const bool deferredVertexExport = position != nullptr;

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
    llvm_unreachable("Not implemented!");
  }

  if (deferredVertexExport && m_compactVertex) {
    auto exportVertexBlock = m_builder.GetInsertBlock();

    auto uncompactVertexBlock = createBlock(exportVertexBlock->getParent(), ".uncompactVertex");
    uncompactVertexBlock->moveAfter(exportVertexBlock);

    auto endUncompactVertexBlock = createBlock(exportVertexBlock->getParent(), ".endUncompactVertex");
    endUncompactVertexBlock->moveAfter(uncompactVertexBlock);

    m_builder.CreateCondBr(m_compactVertex, uncompactVertexBlock, endUncompactVertexBlock);

    // Construct ".uncompactVertex" block
    Value *newPosition = nullptr;
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
          m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;

      auto uncompactedVertexIndex = readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                                             PrimShaderLdsRegion::VertexIndexMap);
      auto vertexItemOffset = m_builder.CreateMul(uncompactedVertexIndex, m_builder.getInt32(esGsRingItemSize));

      newPosition = readPerThreadDataFromLds(FixedVectorType::get(m_builder.getFloatTy(), 4), uncompactedVertexIndex,
                                             PrimShaderLdsRegion::VertexPosition, true);

      // NOTE: For deferred vertex export, some system values could be from vertex compaction info rather than from
      // VGPRs (caused by NGG culling and vertex compaction)
      const auto resUsage = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
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

      position = createPhi({{newPosition, uncompactVertexBlock}, {position, exportVertexBlock}});

      if (m_hasTes) {
        if (newTessCoordX)
          tessCoordX = createPhi({{newTessCoordX, uncompactVertexBlock}, {tessCoordX, exportVertexBlock}});

        if (newTessCoordY)
          tessCoordX = createPhi({{newTessCoordY, uncompactVertexBlock}, {tessCoordY, exportVertexBlock}});

        assert(newRelPatchId);
        relPatchId = createPhi({{newRelPatchId, uncompactVertexBlock}, {relPatchId, exportVertexBlock}});

        if (newPatchId)
          patchId = createPhi({{newPatchId, uncompactVertexBlock}, {patchId, exportVertexBlock}});
      } else {
        if (newVertexId)
          vertexId = createPhi({{newVertexId, uncompactVertexBlock}, {vertexId, exportVertexBlock}});

        if (newVsPrimitiveId)
          vsPrimitiveId = createPhi({{newVsPrimitiveId, uncompactVertexBlock}, {vsPrimitiveId, exportVertexBlock}});

        if (newInstanceId)
          instanceId = createPhi({{newInstanceId, uncompactVertexBlock}, {instanceId, exportVertexBlock}});
      }
    }
  }

  auto partEs = deferredVertexExport ? m_esHandlers.vertexExporter : m_esHandlers.cullDataFetcher;

  SmallVector<Value *, 32> partEsArgs;

  // Setup attribute ring base and relative vertex index in subgroup as two additional arguments to export vertex
  // attributes through memory
  if (m_gfxIp.major >= 11 && deferredVertexExport) {
    const auto attribCount = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)
                                 ->inOutUsage.expCount;
    if (attribCount > 0) {
      partEsArgs.push_back(m_nggInputs.attribRingBase);
      partEsArgs.push_back(m_nggInputs.threadIdInSubgroup);
    }
  }

  if (deferredVertexExport)
    partEsArgs.push_back(position); // Setup vertex position data as the additional argument

  // Set up user data SGPRs
  const unsigned userDataCount =
      m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)->userDataCount;
  appendUserData(partEsArgs, partEs, userData, userDataCount);

  if (m_hasTes) {
    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      Value *isOffChip = PoisonValue::get(m_builder.getInt32Ty()); // Unused
      partEsArgs.push_back(isOffChip);
      partEsArgs.push_back(offChipLdsBase);
    }

    // Set up system value VGPRs
    partEsArgs.push_back(tessCoordX);
    partEsArgs.push_back(tessCoordY);
    partEsArgs.push_back(relPatchId);
    partEsArgs.push_back(patchId);
  } else {
    // Set up system value VGPRs
    partEsArgs.push_back(vertexId);
    partEsArgs.push_back(relVertexId);
    partEsArgs.push_back(vsPrimitiveId);
    partEsArgs.push_back(instanceId);
  }

  assert(partEsArgs.size() == partEs->arg_size()); // Must have visit all arguments of the part ES

  CallInst *partEsCall = m_builder.CreateCall(partEs, partEsArgs);
  partEsCall->setCallingConv(CallingConv::AMDGPU_ES);
  return partEsCall;
}

// =====================================================================================================================
// Split ES to two parts. One is to fetch cull data for NGG culling, such as position and cull distance (if cull
// distance culling is enabled). The other is to do deferred vertex export like original ES.
//
// NOTE: After this splitting, original ES is removed and couldn't be used any more.
void NggPrimShader::splitEs() {
  assert(m_hasGs == false); // GS must not be present

  //
  // Collect all export calls for further analysis
  //
  SmallVector<Function *, 8> expFuncs;
  for (auto &func : m_esHandlers.main->getParent()->functions()) {
    if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp)
      expFuncs.push_back(&func);
    else if (m_gfxIp.major >= 11) {
      if (func.getName().startswith(lgcName::NggAttribExport) || func.getName().startswith(lgcName::NggXfbExport))
        expFuncs.push_back(&func);
    }
  }

  //
  // Preparation for fetching cull distances
  //
  unsigned clipCullPos = EXP_TARGET_POS_1;
  unsigned clipDistanceCount = 0;
  unsigned cullDistanceCount = 0;

  if (m_nggControl->enableCullDistanceCulling) {
    const auto &resUsage = m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex);

    if (m_hasTes) {
      const auto &builtInUsage = resUsage->builtInUsage.tes;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      clipCullPos = miscExport ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    } else {
      const auto &builtInUsage = resUsage->builtInUsage.vs;

      bool miscExport = builtInUsage.pointSize || builtInUsage.layer || builtInUsage.viewportIndex;
      miscExport |= builtInUsage.primitiveShadingRate;
      clipCullPos = miscExport ? EXP_TARGET_POS_2 : EXP_TARGET_POS_1;
      clipDistanceCount = builtInUsage.clipDistance;
      cullDistanceCount = builtInUsage.cullDistance;
    }

    assert(cullDistanceCount > 0); // Cull distance must exist if the culling is enabled
  }

  //
  // Create the part ES to fetch cull data for NGG culling
  //
  const auto positionTy = FixedVectorType::get(m_builder.getFloatTy(), 4);
  const auto cullDistanceTy = ArrayType::get(m_builder.getFloatTy(), cullDistanceCount);

  Type *cullDataTy = positionTy;
  if (m_nggControl->enableCullDistanceCulling)
    cullDataTy = StructType::get(m_builder.getContext(), {positionTy, cullDistanceTy});

  // Clone ES
  auto esCullDataFetcherTy = FunctionType::get(cullDataTy, m_esHandlers.main->getFunctionType()->params(), false);
  auto esCullDataFetcher =
      Function::Create(esCullDataFetcherTy, m_esHandlers.main->getLinkage(), "", m_esHandlers.main->getParent());

  ValueToValueMapTy valueMap;

  Argument *newArg = esCullDataFetcher->arg_begin();
  for (Argument &arg : m_esHandlers.main->args())
    valueMap[&arg] = newArg++;

  SmallVector<ReturnInst *, 8> retInsts;
  CloneFunctionInto(esCullDataFetcher, m_esHandlers.main, valueMap, CloneFunctionChangeType::LocalChangesOnly,
                    retInsts);
  esCullDataFetcher->setName(NggEsCullDataFetcher);

  // Find the return block, remove all exports, and mutate return type
  BasicBlock *retBlock = nullptr;
  for (BasicBlock &block : *esCullDataFetcher) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      retInst->dropAllReferences();
      retInst->eraseFromParent();

      retBlock = &block;
      break;
    }
  }
  assert(retBlock);

  IRBuilder<>::InsertPointGuard guard(m_builder);
  m_builder.SetInsertPoint(retBlock);

  SmallVector<CallInst *, 8> removedCalls;

  // Fetch position and cull distances
  Value *position = PoisonValue::get(positionTy);
  SmallVector<Value *, MaxClipCullDistanceCount> clipCullDistance(MaxClipCullDistanceCount);

  for (auto func : expFuncs) {
    for (auto user : func->users()) {
      CallInst *const call = cast<CallInst>(user);

      if (call->getParent()->getParent() != esCullDataFetcher)
        continue; // Export call doesn't belong to targeted function, skip

      assert(call->getParent() == retBlock); // Must in return block

      if (func->isIntrinsic() && func->getIntrinsicID() == Intrinsic::amdgcn_exp) {
        unsigned exportTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        if (exportTarget == EXP_TARGET_POS_0) {
          // Get position value
          m_constPositionZ = isa<Constant>(call->getArgOperand(4));
          for (unsigned i = 0; i < 4; ++i)
            position = m_builder.CreateInsertElement(position, call->getArgOperand(2 + i), i);
        } else if (exportTarget == clipCullPos) {
          // Get clip/cull distance value
          if (m_nggControl->enableCullDistanceCulling) {
            clipCullDistance[0] = call->getArgOperand(2);
            clipCullDistance[1] = call->getArgOperand(3);
            clipCullDistance[2] = call->getArgOperand(4);
            clipCullDistance[3] = call->getArgOperand(5);
          }
        } else if (exportTarget == clipCullPos + 1 && clipDistanceCount + cullDistanceCount > 4) {
          // Get clip/cull distance value
          if (m_nggControl->enableCullDistanceCulling) {
            clipCullDistance[4] = call->getArgOperand(2);
            clipCullDistance[5] = call->getArgOperand(3);
            clipCullDistance[6] = call->getArgOperand(4);
            clipCullDistance[7] = call->getArgOperand(5);
          }
        }
      }

      removedCalls.push_back(call); // Remove export
    }
  }

  Value *cullData = position;
  if (m_nggControl->enableCullDistanceCulling) {
    Value *cullDistance = PoisonValue::get(cullDistanceTy);

    for (unsigned i = 0; i < cullDistanceCount; ++i)
      cullDistance = m_builder.CreateInsertValue(cullDistance, clipCullDistance[clipDistanceCount + i], i);

    cullData = m_builder.CreateInsertValue(PoisonValue::get(cullDataTy), position, 0);
    cullData = m_builder.CreateInsertValue(cullData, cullDistance, 1);
  }

  m_builder.CreateRet(cullData);

  //
  // Create the part ES to do deferred vertex export after NGG culling
  //

  // NOTE: Here, we just mutate original ES to do deferred vertex export. We add vertex position data as an additional
  // argument. This could avoid re-fetching it since we already get the data before NGG culling.
  auto esVertexExporter = addFunctionArgs(m_esHandlers.main, nullptr, {positionTy}, {"position"});
  esVertexExporter->setName(NggEsVertexExporter);

  position = esVertexExporter->getArg(0); // The first argument is vertex position data
  assert(position->getType() == positionTy);

  for (auto func : expFuncs) {
    for (auto user : func->users()) {
      CallInst *const call = cast<CallInst>(user);

      if (call->getParent()->getParent() != esVertexExporter)
        continue; // Export call doesn't belong to targeted function, skip

      if (func->isIntrinsic() && func->getIntrinsicID() == Intrinsic::amdgcn_exp) {
        unsigned exportTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        if (exportTarget == EXP_TARGET_POS_0) {
          // Replace vertex position data
          m_builder.SetInsertPoint(call);
          call->setArgOperand(2, m_builder.CreateExtractElement(position, static_cast<uint64_t>(0)));
          call->setArgOperand(3, m_builder.CreateExtractElement(position, 1));
          call->setArgOperand(4, m_builder.CreateExtractElement(position, 2));
          call->setArgOperand(5, m_builder.CreateExtractElement(position, 3));
        }
      }
    }
  }

  if (m_gfxIp.major >= 11)
    processVertexAttribExport(esVertexExporter);

  // Remove original ES since it is no longer needed
  assert(m_esHandlers.main->use_empty());
  m_esHandlers.main->eraseFromParent();
  m_esHandlers.main = nullptr;

  // Record new part ES
  m_esHandlers.cullDataFetcher = esCullDataFetcher;
  m_esHandlers.vertexExporter = esVertexExporter;

  // Remove calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Runs GS.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::runGs(ArrayRef<Argument *> args) {
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
    llvm_unreachable("Not implemented!");
  }

  SmallVector<Value *, 32> gsArgs;

  // Set up user data SGPRs
  const unsigned userDataCount = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->userDataCount;
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

  SmallVector<Instruction *, 32> removedCalls;

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
  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  auto threadIdInWave =
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {m_builder.getInt32(-1), m_builder.getInt32(0)});

  if (waveSize == 64) {
    threadIdInWave =
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {m_builder.getInt32(-1), threadIdInWave});
  }

  // Initialize thread ID in subgroup
  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
  auto waveId = getFunctionArgument(m_gsHandlers.main, entryArgIdxs.gsWaveId);

  auto threadIdInSubgroup = m_builder.CreateMul(waveId, m_builder.getInt32(waveSize));
  threadIdInSubgroup = m_builder.CreateAdd(threadIdInSubgroup, threadIdInWave);

  // Handle GS message and GS output export
  for (auto &func : m_gsHandlers.main->getParent()->functions()) {
    if (func.getName().startswith(lgcName::NggWriteGsOutput)) {
      // Export GS outputs to GS-VS ring
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);
        m_builder.SetInsertPoint(call);

        assert(call->arg_size() == 4);
        const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
        const unsigned compIdx = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
        assert(streamId < MaxGsStreams);
        Value *output = call->getOperand(3);

        auto emitVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), emitVertsPtrs[streamId]);
        auto totalEmitVerts = m_builder.CreateLoad(m_builder.getInt32Ty(), totalEmitVertsPtr);
        writeGsOutput(output, location, compIdx, streamId, threadIdInSubgroup, emitVerts, totalEmitVerts);

        removedCalls.push_back(call);
      }
    } else if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg) {
      // Handle GS message
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);
        m_builder.SetInsertPoint(call);

        if (getShaderStage(call->getParent()->getParent()) != ShaderStageGeometry)
          continue; // Not belong to GS messages

        uint64_t message = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        if (message == GsEmitStreaM0 || message == GsEmitStreaM1 || message == GsEmitStreaM2 ||
            message == GsEmitStreaM3) {
          // Handle GS_EMIT, MSG[9:8] = STREAM_ID
          unsigned streamId = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
          assert(streamId < MaxGsStreams);
          processGsEmit(streamId, threadIdInSubgroup, emitVertsPtrs[streamId], outVertsPtrs[streamId],
                        totalEmitVertsPtr);
        } else if (message == GsCutStreaM0 || message == GsCutStreaM1 || message == GsCutStreaM2 ||
                   message == GsCutStreaM3) {
          // Handle GS_CUT, MSG[9:8] = STREAM_ID
          unsigned streamId = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
          assert(streamId < MaxGsStreams);
          processGsCut(streamId, outVertsPtrs[streamId]);
        } else if (message == GsDone) {
          // Handle GS_DONE, do nothing (just remove this call)
        } else {
          // Unexpected GS message
          llvm_unreachable("Unexpected GS message!");
        }

        removedCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Runs copy shader.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::runCopyShader(ArrayRef<Argument *> args) {
  assert(m_hasGs); // GS must be present

  //
  // The processing is something like this:
  //
  //   vertexIndices = Relative vertex indices
  //   if (compactVertex)
  //     vertexIndices = Read uncompacted relative vertex indices from LDS
  //   Calculate vertex offset and run copy shader
  //
  Value *vertexIndex = m_nggInputs.threadIdInSubgroup;
  if (m_compactVertex) {
    auto exportVertexBlock = m_builder.GetInsertBlock();

    auto uncompactVertexIndexBlock = createBlock(exportVertexBlock->getParent(), ".uncompactVertexIndex");
    uncompactVertexIndexBlock->moveAfter(exportVertexBlock);

    auto endUncompactVertexIndexBlock = createBlock(exportVertexBlock->getParent(), ".endUncompactVertexIndex");
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

      vertexIndex = createPhi({{uncompactedVertexIndex, uncompactVertexIndexBlock}, {vertexIndex, exportVertexBlock}});
    }
  }

  mutateCopyShader();

  // Run copy shader
  SmallVector<Value *, 32> copyShaderArgs;

  if (m_gfxIp.major >= 11) {
    // Setup attribute ring base and relative vertex index in subgroup as two additional arguments to export vertex
    // attributes through memory
    const auto attribCount = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.expCount;
    if (attribCount > 0) {
      copyShaderArgs.push_back(m_nggInputs.attribRingBase);
      copyShaderArgs.push_back(m_nggInputs.threadIdInSubgroup);
    }

    // Global table
    auto userData = args[NumSpecialSgprInputs];
    assert(userData->getType()->isVectorTy());
    auto globalTable = m_builder.CreateExtractElement(userData, static_cast<uint64_t>(0)); // The first user data SGPRs
    copyShaderArgs.push_back(globalTable);
  }

  // Relative vertex index in subgroup
  copyShaderArgs.push_back(vertexIndex);

  CallInst *copyShaderCall = m_builder.CreateCall(m_gsHandlers.copyShader, copyShaderArgs);
  copyShaderCall->setCallingConv(CallingConv::AMDGPU_VS);
}

// =====================================================================================================================
// Mutates copy shader to handle the reading GS outputs from GS-VS ring.
void NggPrimShader::mutateCopyShader() {
  if (m_gfxIp.major >= 11)
    processVertexAttribExport(m_gsHandlers.copyShader);

  IRBuilder<>::InsertPointGuard guard(m_builder);

  // Relative vertex index is always the last argument
  auto vertexIndex = getFunctionArgument(m_gsHandlers.copyShader, m_gsHandlers.copyShader->arg_size() - 1);
  const unsigned rasterStream = m_pipelineState->getRasterizerState().rasterStream;

  SmallVector<Instruction *, 32> removedCalls;

  for (auto &func : m_gsHandlers.copyShader->getParent()->functions()) {
    if (func.getName().startswith(lgcName::NggReadGsOutput)) {
      // Import GS outputs from GS-VS ring
      for (auto user : func.users()) {
        CallInst *const call = cast<CallInst>(user);

        if (call->getFunction() != m_gsHandlers.copyShader)
          continue; // Not belong to copy shader

        m_builder.SetInsertPoint(call);

        assert(call->arg_size() == 3);
        const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
        const unsigned component = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
        const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
        assert(streamId < MaxGsStreams);

        // Only lower the GS output import calls if they belong to the rasterization stream.
        if (streamId == rasterStream) {
          auto vertexOffset = calcVertexItemOffset(streamId, vertexIndex);
          auto output = readGsOutput(call->getType(), location, component, streamId, vertexOffset);
          call->replaceAllUsesWith(output);
        }

        removedCalls.push_back(call);
      }
    }
  }

  // Clear removed calls
  for (auto call : removedCalls) {
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

  if (geometryMode.robustGsEmits) {
    // skip the lds write by writing to a dummy offset.
    // ldsOffset = (totalEmitVerts >= outputVertices) ? InvalidValue : ldsOffset
    auto dummyOffset = m_builder.getInt32(0x80000000);
    auto outOfRange = m_builder.CreateICmpUGE(totalEmitVerts, m_builder.getInt32(geometryMode.outputVertices));
    ldsOffset = m_builder.CreateSelect(outOfRange, dummyOffset, ldsOffset);
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
// Processes the message GS_EMIT.
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
// Processes the message GS_CUT.
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
// Creates the function that processes GS_EMIT.
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
      // outVerts = (totalEmitVerts >= outputVertices) ? 0 : outVerts
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
    // ldsOffset = regionStart + vertexIndex + NggMaxThreadsPerSubgroup * streamId
    auto ldsOffset = m_builder.CreateAdd(m_builder.getInt32(regionStart), vertexIndex);
    ldsOffset = m_builder.CreateAdd(ldsOffset,
                                    m_builder.CreateMul(m_builder.getInt32(Gfx9::NggMaxThreadsPerSubgroup), streamId));
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
// Creates the function that processes GS_CUT.
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
      // screenMinX = -xScale + xOffset - 0.75
      screenMinX = m_builder.CreateFAdd(m_builder.CreateFNeg(xScale), xOffset);
      screenMinX = m_builder.CreateFAdd(screenMinX, ConstantFP::get(m_builder.getFloatTy(), -0.75));

      // screenMaxX = xScale + xOffset + 0.75
      screenMaxX = m_builder.CreateFAdd(xScale, xOffset);
      screenMaxX = m_builder.CreateFAdd(screenMaxX, ConstantFP::get(m_builder.getFloatTy(), 0.75));

      // screenMinY = -yScale + yOffset - 0.75
      screenMinY = m_builder.CreateFAdd(m_builder.CreateFNeg(yScale), yOffset);
      screenMinY = m_builder.CreateFAdd(screenMinY, ConstantFP::get(m_builder.getFloatTy(), -0.75));

      // screenMaxY = yScale + yOffset + 0.75
      screenMaxY = m_builder.CreateFAdd(yScale, yOffset);
      screenMaxY = m_builder.CreateFAdd(screenMaxY, ConstantFP::get(m_builder.getFloatTy(), 0.75));
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

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);

  Value *result = m_builder.CreateIntrinsic(Intrinsic::amdgcn_ballot, m_builder.getIntNTy(waveSize), value);
  if (waveSize == 32)
    result = m_builder.CreateZExt(result, m_builder.getInt64Ty());

  return result;
}

// =====================================================================================================================
// Processes vertex attribute export calls in the target function. We mutate the argument list of the target function
// by adding two additional arguments (one is attribute ring base and the other is relative vertex index in subgroup).
// Also, we expand all export calls by replacing it with real instructions that do vertex attribute exporting through
// memory.
//
// @param [in/out] target : Target function to process vertex attribute export
void NggPrimShader::processVertexAttribExport(Function *&target) {
  assert(m_gfxIp.major >= 11); // For GFX11+

  ShaderStage shaderStage = m_hasGs ? ShaderStageGeometry : (m_hasTes ? ShaderStageTessEval : ShaderStageVertex);
  const unsigned attribCount = m_pipelineState->getShaderResourceUsage(shaderStage)->inOutUsage.expCount;
  if (attribCount == 0)
    return; // No vertex attribute exports

  IRBuilder<>::InsertPointGuard guard(m_builder);

  //
  // Mutate the argument list by adding two additional arguments
  //
  auto newTarget = addFunctionArgs(target, nullptr,
                                   {
                                       m_builder.getInt32Ty(), // Attribute ring base (SGPR)
                                       m_builder.getInt32Ty()  // Relative vertex index in subgroup (VGPR)
                                   },
                                   {"attribRingBase", "vertexIndex"}, 0x1);

  // Original function is no longer needed
  assert(target->use_empty());
  target->eraseFromParent();

  target = newTarget;

  //
  // Expand vertex attribute export calls by replacing them with real instructions
  //
  Value *attribRingBufDesc = nullptr;

  // Always the first two arguments, added by us
  auto attribRingBase = target->getArg(0);
  auto vertexIndex = target->getArg(1);

  m_builder.SetInsertPointPastAllocas(target);

  // ringOffset = attribRingBase * 32 * 16
  //            = attribRingBase * 512
  static const unsigned AttribGranularity = 32 * SizeOfVec4; // 32 * 16 bytes
  auto ringOffset = m_builder.CreateMul(attribRingBase, m_builder.getInt32(AttribGranularity));

  SmallVector<CallInst *, 8> removedCalls;

  for (auto &func : target->getParent()->functions()) {
    if (func.getName().startswith(lgcName::NggAttribExport)) {
      for (auto user : func.users()) {
        CallInst *const call = dyn_cast<CallInst>(user);
        assert(call);

        if (call->getParent()->getParent() != target)
          continue; // Export call doesn't belong to targeted function, skip

        // NOTE: We always set the insert point before the terminator of the basic block to which this call belongs.
        // This is because we might modify attribute ring buffer descriptor and this modified descriptor will be used
        // by subsequent ring buffer store instructions that do vertex attribute exporting.
        m_builder.SetInsertPoint(call->getParent()->getTerminator());

        if (!attribRingBufDesc) {
          attribRingBufDesc = call->getArgOperand(0); // Initialize it if necessary

          // Fixup the STRIDE field if necessary, STRIDE = WORD1[30:16].
          //
          // STRIDE is initialized to 16 by the driver, which is the right value for attribCount == 1.
          // We override the value if there are more attributes.
          if (attribCount > 1) {
            auto descWord1 = m_builder.CreateExtractElement(attribRingBufDesc, 1);
            auto stride = m_builder.getInt32(attribCount * SizeOfVec4);
            if ((attribCount & 1) == 0) {
              // Clear the bit that was set in STRIDE by the driver.
              descWord1 = m_builder.CreateAnd(descWord1, ~0x3FFF0000);
            }
            descWord1 = m_builder.CreateOr(descWord1, m_builder.CreateShl(stride, 16)); // Set new STRIDE
            attribRingBufDesc = m_builder.CreateInsertElement(attribRingBufDesc, descWord1, 1);
          }
        }

        const unsigned location = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
        auto attribValue = call->getArgOperand(2);

        // Export vertex attributes
        assert(attribValue->getType() == FixedVectorType::get(m_builder.getFloatTy(), 4)); // Must be <4 xfloat>

        auto locationOffset = m_builder.getInt32(location * SizeOfVec4);

        CoherentFlag coherent = {};
        if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
          coherent.bits.glc = true;
          coherent.bits.slc = true;
        }
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_store, attribValue->getType(),
                                  {attribValue, attribRingBufDesc, vertexIndex, locationOffset, ringOffset,
                                   m_builder.getInt32(coherent.u32All)});

        removedCalls.push_back(call);
      }

      break; // Vertex attribute export calls are handled, could exit the loop
    }
  }

  // NOTE: If the workaround of attributes-through-memory preceding vertex position data is required, we have to collect
  // all vertex position export calls and move them before the return instruction. This actually places them after the
  // writing operations of attributes-through-memory
  if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx11.waAtmPrecedesPos) {
    SmallVector<CallInst *, 4> exportCalls;

    // Colllect export calls of vertex position data
    for (auto &func : target->getParent()->functions()) {
      if (func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp) {
        for (auto user : func.users()) {
          CallInst *const call = dyn_cast<CallInst>(user);
          assert(call);

          if (call->getParent()->getParent() != target)
            continue; // Export call doesn't belong to targeted function, skip

          exportCalls.push_back(call);
        }
      }
    }

    // Move the export calls before the return instructions
    ReturnInst *retInst = nullptr;
    for (unsigned i = 0; i < exportCalls.size(); ++i) {
      auto exportCall = exportCalls[i];

      if (retInst) {
        // All export calls are expected to be in the same basic block
        assert(retInst == exportCall->getParent()->getTerminator());
      } else {
        retInst = dyn_cast<ReturnInst>(exportCall->getParent()->getTerminator());
        assert(retInst);
      }

      exportCall->setOperand(
          6, m_builder.getInt1(i == exportCalls.size() - 1)); // Make export done flag for the last export call
      exportCall->moveBefore(retInst);
    }

    // Before the first export call, add s_wait_vscnt 0 to make sure the completion of all attributes being written
    // to the attribute ring buffer
    m_builder.SetInsertPoint(exportCalls[0]);
    m_builder.CreateFence(AtomicOrdering::Release, SyncScope::System);
  }

  // Remove calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }
}

// =====================================================================================================================
// Processes SW emulated transform feedback when API GS is not present.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::processSwXfb(ArrayRef<Argument *> args) {
  assert(m_pipelineState->enableSwXfb());
  assert(!m_hasGs); // API GS is not present

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();

  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  unsigned firstActiveXfbBuffer = InvalidValue;
  unsigned lastActiveXfbBuffer = InvalidValue;

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    bufferActive[i] = xfbStrides[i] > 0;
    if (!bufferActive[i])
      continue; // Transform feedback buffer is inactive

    if (firstActiveXfbBuffer == InvalidValue)
      firstActiveXfbBuffer = i;
    lastActiveXfbBuffer = i;
  }

  //
  // The processing is something like this:
  //
  // NGG_XFB() {
  //   if (threadIdInSubgroup < vertCountInSubgroup) {
  //     Mutate/clone ES to fetch XFB outputs
  //     Write XFB outputs to LDS region
  //   }
  //
  //   if (threadIdInSubgroup == 0) {
  //     Acquire the control of GDS_STRMOUT_DWORDS_WRITTEN_X
  //     Calculate primsToWrite and dwordsToWrite
  //     Increment GDS_STRMOUT_DWORDS_WRITTEN_X and release the control
  //     Store XFB statistics info to LDS
  //     Increment GDS_STRMOUT_PRIMS_NEEDED_X and GDS_STRMOUT_PRIMS_WRITTEN_X
  //   }
  //   Barrier
  //
  //   if (threadIdInWave < MaxXfbBuffers + 1)
  //     Read XFB statistics info from LDS
  //
  //   Read primsToWrite and dwordsWritten from XFB statistics info
  //
  //   if (threadIdInSubgroup < primsToWrite)
  //     Export XFB outputs to buffer for each vertice of this primitive
  // }
  //
  BasicBlock *xfbEntryBlock = m_builder.GetInsertBlock();

  BasicBlock *fetchXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".fetchXfbOutput");
  fetchXfbOutputBlock->moveAfter(xfbEntryBlock);
  BasicBlock *endFetchXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".endFetchXfbOutput");
  endFetchXfbOutputBlock->moveAfter(fetchXfbOutputBlock);

  BasicBlock *prepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".prepareXfbExport");
  prepareXfbExportBlock->moveAfter(endFetchXfbOutputBlock);
  BasicBlock *endPrepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".endPrepareXfbExport");
  endPrepareXfbExportBlock->moveAfter(prepareXfbExportBlock);

  BasicBlock *readXfbStatInfoBlock = createBlock(xfbEntryBlock->getParent(), ".readXfbStatInfo");
  readXfbStatInfoBlock->moveAfter(endPrepareXfbExportBlock);
  BasicBlock *endReadXfbStatInfoBlock = createBlock(xfbEntryBlock->getParent(), ".endReadXfbStatInfo");
  endReadXfbStatInfoBlock->moveAfter(readXfbStatInfoBlock);

  BasicBlock *exportXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".exportXfbOutput");
  exportXfbOutputBlock->moveAfter(endReadXfbStatInfoBlock);
  BasicBlock *endExportXfbOutputBlock = createBlock(xfbEntryBlock->getParent(), ".endExportXfbOutput");
  endExportXfbOutputBlock->moveAfter(exportXfbOutputBlock);

  // Insert branching in current block to process transform feedback export
  {
    auto validVertex = m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_nggInputs.vertCountInSubgroup);
    m_builder.CreateCondBr(validVertex, fetchXfbOutputBlock, endFetchXfbOutputBlock);
  }

  // Construct ".fetchXfbOutput" block
  SmallVector<XfbOutputExport, 32> xfbOutputExports;
  {
    m_builder.SetInsertPoint(fetchXfbOutputBlock);

    auto xfbOutputs = fetchXfbOutput(m_esHandlers.main, args, xfbOutputExports);

    for (unsigned i = 0; i < xfbOutputExports.size(); ++i) {
      assert(xfbOutputs->getType()->isArrayTy()); // Must be arrayed
      auto outputValue = m_builder.CreateExtractValue(xfbOutputs, i);

      // Write transform feedback outputs to LDS region
      writeXfbOutputToLds(outputValue, m_nggInputs.threadIdInSubgroup, i);
    }

    m_builder.CreateBr(endFetchXfbOutputBlock);
  }

  // Construct ".endFetchXfbOutput" block
  {
    m_builder.SetInsertPoint(endFetchXfbOutputBlock);

    auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
    m_builder.CreateCondBr(firstThreadInSubgroup, prepareXfbExportBlock, endPrepareXfbExportBlock);
  }

  // Construct ".prepareXfbExport" block
  {
    m_builder.SetInsertPoint(prepareXfbExportBlock);

    const unsigned vertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
    Value *numPrimsToWrite = m_nggInputs.primCountInSubgroup;

    Value *dwordsWritten[MaxTransformFeedbackBuffers] = {};
    Value *dwordsPerPrim[MaxTransformFeedbackBuffers] = {};

    // Calculate numPrimsToWrite
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      if (m_gfxIp.major <= 11) {
        if (i == firstActiveXfbBuffer) {
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
        } else {
          // ds_add_gs_reg
          dwordsWritten[i] =
              m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                        {m_builder.getInt32(0),                                         // value to add
                                         m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
        }
      } else {
        llvm_unreachable("Not implemented!");
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
      dwordsPerPrim[i] = m_builder.getInt32(vertsPerPrim * xfbStrides[i] / sizeof(unsigned));
      Value *primsCanWrite = m_builder.CreateUDiv(dwordsRemaining, dwordsPerPrim[i]);
      numPrimsToWrite =
          m_builder.CreateIntrinsic(Intrinsic::umin, numPrimsToWrite->getType(), {numPrimsToWrite, primsCanWrite});
    }

    // Increment dwordsWritten
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      Value *dwordsToWrite = m_builder.CreateMul(numPrimsToWrite, dwordsPerPrim[i]);

      if (m_gfxIp.major <= 11) {
        if (i == lastActiveXfbBuffer) {
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
        } else {
          // ds_add_gs_reg
          dwordsWritten[i] =
              m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, dwordsToWrite->getType(),
                                        {dwordsToWrite,                                                 // value to add
                                         m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
        }
      } else {
        llvm_unreachable("Not implemented!");
      }
    }

    // Store transform feedback statistics info to LDS and GDS
    const unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbStats);
    writeValueToLds(numPrimsToWrite, m_builder.getInt32(regionStart + MaxTransformFeedbackBuffers));
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      writeValueToLds(dwordsWritten[i], m_builder.getInt32(regionStart + i));
    }

    if (m_gfxIp.major <= 11) {
      m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_nggInputs.primCountInSubgroup->getType(),
                                {m_nggInputs.primCountInSubgroup,                       // value to add
                                 m_builder.getInt32(GDS_STRMOUT_PRIMS_NEEDED_0 << 2)}); // count index

      m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, numPrimsToWrite->getType(),
                                {numPrimsToWrite,                                        // value to add
                                 m_builder.getInt32(GDS_STRMOUT_PRIMS_WRITTEN_0 << 2)}); // count index
    } else {
      llvm_unreachable("Not implemented!");
    }

    m_builder.CreateBr(endPrepareXfbExportBlock);
  }

  // Construct ".endPrepareXfbExport" block
  {
    m_builder.SetInsertPoint(endPrepareXfbExportBlock);

    // We are going to read transform feedback statistics info and outputs from LDS and export them to transform
    // feedback buffers. Make sure the output values have been all written before this.
    createFenceAndBarrier();

    auto validThread =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInWave, m_builder.getInt32(1 + MaxTransformFeedbackBuffers));
    m_builder.CreateCondBr(validThread, readXfbStatInfoBlock, endReadXfbStatInfoBlock);
  }

  // Construct ".readXfbStatInfo" block
  Value *xfbStatInfo = nullptr;
  {
    m_builder.SetInsertPoint(readXfbStatInfoBlock);

    xfbStatInfo =
        readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInWave, PrimShaderLdsRegion::XfbStats);
    m_builder.CreateBr(endReadXfbStatInfoBlock);
  }

  // Construct ".endReadXfbStatInfo" block
  Value *streamOutOffsets[MaxTransformFeedbackBuffers] = {}; // Stream-out offset to write transform feedback outputs
  {
    m_builder.SetInsertPoint(endReadXfbStatInfoBlock);

    xfbStatInfo = createPhi(
        {{xfbStatInfo, readXfbStatInfoBlock}, {PoisonValue::get(xfbStatInfo->getType()), endPrepareXfbExportBlock}});

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
    m_builder.CreateCondBr(validPrimitive, exportXfbOutputBlock, endExportXfbOutputBlock);
  }

  // Construct ".exportXfbOutput" block
  {
    m_builder.SetInsertPoint(exportXfbOutputBlock);

    const unsigned vertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
    Value *vertexIndices[3] = {};
    vertexIndices[0] = m_nggInputs.vertexIndex0;
    if (vertsPerPrim > 1)
      vertexIndices[1] = m_nggInputs.vertexIndex1;
    if (vertsPerPrim > 2)
      vertexIndices[2] = m_nggInputs.vertexIndex2;

    for (unsigned i = 0; i < vertsPerPrim; ++i) {
      for (unsigned j = 0; j < xfbOutputExports.size(); ++j) {
        const auto &xfbOutputExport = xfbOutputExports[j];
        auto outputValue = readXfbOutputFromLds(
            xfbOutputExport.numElements > 1 ? FixedVectorType::get(m_builder.getFloatTy(), xfbOutputExport.numElements)
                                            : m_builder.getFloatTy(),
            vertexIndices[i], j);

        if (xfbOutputExport.is16bit) {
          // NOTE: For 16-bit transform feedbakc outputs, they are stored as 32-bit without tightly packed in LDS.
          outputValue = m_builder.CreateBitCast(
              outputValue, FixedVectorType::get(m_builder.getInt32Ty(), xfbOutputExport.numElements));
          outputValue = m_builder.CreateTrunc(
              outputValue, FixedVectorType::get(m_builder.getInt16Ty(), xfbOutputExport.numElements));
          outputValue = m_builder.CreateBitCast(
              outputValue, FixedVectorType::get(m_builder.getHalfTy(), xfbOutputExport.numElements));
        }

        unsigned format = 0;
        switch (xfbOutputExport.numElements) {
        case 1:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_FLOAT : BUF_FORMAT_32_FLOAT;
          break;
        case 2:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_FLOAT_GFX11;
          break;
        case 3:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_32_FLOAT_GFX11;
          break;
        case 4:
          format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_16_16_FLOAT_GFX11 : BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
          break;
        default:
          llvm_unreachable("Unexpected element number!");
          break;
        }

        CoherentFlag coherent = {};
        if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
          coherent.bits.glc = true;
          coherent.bits.slc = true;
        }

        // vertexOffset = (threadIdInSubgroup * vertsPerPrim + vertexIndex) * xfbStride
        Value *vertexOffset =
            m_builder.CreateAdd(m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(vertsPerPrim)),
                                m_builder.getInt32(i));
        vertexOffset = m_builder.CreateMul(vertexOffset, m_builder.getInt32(xfbStrides[xfbOutputExport.xfbBuffer]));
        // xfbOutputOffset = vertexOffset + xfbOffset
        Value *xfbOutputOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(xfbOutputExport.xfbOffset));

        if (xfbOutputExport.is16bit && xfbOutputExport.numElements == 3) {
          // NOTE: For 16vec3, HW doesn't have a corresponding buffer store instruction. We have to split it to 16vec2
          // and 16scalar.
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, FixedVectorType::get(m_builder.getHalfTy(), 2),
                                    {m_builder.CreateShuffleVector(outputValue, ArrayRef<int>{0, 1}), // vdata
                                     m_streamOutBufDescs[xfbOutputExport.xfbBuffer],                  // rsrc
                                     xfbOutputOffset,                                                 // offset
                                     streamOutOffsets[xfbOutputExport.xfbBuffer],                     // soffset
                                     m_builder.getInt32(BUF_FORMAT_16_16_FLOAT),                      // format
                                     m_builder.getInt32(coherent.u32All)});                           // auxiliary data

          m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, m_builder.getHalfTy(),
                                    {m_builder.CreateExtractElement(outputValue, 2), // vdata
                                     m_streamOutBufDescs[xfbOutputExport.xfbBuffer], // rsrc
                                     m_builder.CreateAdd(xfbOutputOffset,
                                                         m_builder.getInt32(2 * sizeof(uint16_t))), // offset
                                     streamOutOffsets[xfbOutputExport.xfbBuffer],                   // soffset
                                     m_builder.getInt32(BUF_FORMAT_16_FLOAT),                       // format
                                     m_builder.getInt32(coherent.u32All)});                         // auxiliary data
        } else {
          m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outputValue->getType(),
                                    {outputValue,                                    // vdata
                                     m_streamOutBufDescs[xfbOutputExport.xfbBuffer], // rsrc
                                     xfbOutputOffset,                                // offset
                                     streamOutOffsets[xfbOutputExport.xfbBuffer],    // soffset
                                     m_builder.getInt32(format),                     // format
                                     m_builder.getInt32(coherent.u32All)});          // auxiliary data
        }
      }
    }

    m_builder.CreateBr(endExportXfbOutputBlock);
  }

  // Construct ".endExportXfbOutput" block
  { m_builder.SetInsertPoint(endExportXfbOutputBlock); }
}

// =====================================================================================================================
// Process SW emulated transform feedback when API GS is present.
//
// @param args : Arguments of primitive shader entry-point
void NggPrimShader::processSwXfbWithGs(ArrayRef<Argument *> args) {
  assert(m_pipelineState->enableSwXfb());
  assert(m_hasGs); // GS is present

  const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
  assert(waveSize == 32 || waveSize == 64);
  const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

  const auto &xfbStrides = m_pipelineState->getXfbBufferStrides();
  const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();

  bool bufferActive[MaxTransformFeedbackBuffers] = {};
  unsigned firstActiveXfbBuffer = InvalidValue;
  unsigned lastActiveXfbBuffer = InvalidValue;

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    bufferActive[i] = xfbStrides[i] > 0;
    if (!bufferActive[i])
      continue; // Transform feedback buffer is inactive

    if (firstActiveXfbBuffer == InvalidValue)
      firstActiveXfbBuffer = i;
    lastActiveXfbBuffer = i;
  }

  unsigned firstActiveStream = InvalidValue;
  unsigned lastActiveStream = InvalidValue;

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!m_pipelineState->isVertexStreamActive(i))
      continue; // Stream is inactive

    if (firstActiveStream == InvalidValue)
      firstActiveStream = i;
    lastActiveStream = i;
  }

  unsigned xfbBufferToStream[MaxTransformFeedbackBuffers] = {};

  for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
    for (unsigned j = 0; j < MaxGsStreams; ++j) {
      if ((streamXfbBuffers[j] & (1 << i)) != 0) {
        // NOTE: According to GLSL spec, all outputs assigned to a given transform feedback buffer are required to
        // come from a single vertex stream.
        xfbBufferToStream[i] = j;
        break;
      }
    }
  }

  //
  // The processing is something like this:
  //
  // NGG_GS_XFB() {
  //   if (threadIdInSubgroup < waveCount + 1)
  //     Initialize per-wave and per-subgroup count of output primitives
  //   Barrier
  //
  //   if (threadIdInSubgroup < primCountInSubgroup)
  //     Check the draw flag of output primitives and compute draw mask
  //
  //   if (threadIdInWave < waveCount - waveId)
  //     Accumulate per-wave and per-subgroup count of output primitives
  //   Barrier
  //
  //   for (each vertex stream) {
  //     if (primitive drawn)
  //       Compact primitive index (compacted -> uncompacted)
  //   }
  //
  //   Mutate copy shader to fetch XFB outputs
  //
  //   if (threadIdInSubgroup == 0) {
  //     Acquire the control of GDS_STRMOUT_DWORDS_WRITTEN_X
  //     Calculate primsToWrite and dwordsToWrite
  //     Increment GDS_STRMOUT_DWORDS_WRITTEN_X and release the control
  //     Store GS XFB statistics info to LDS
  //     Increment GDS_STRMOUT_PRIMS_NEEDED_X and GDS_STRMOUT_PRIMS_WRITTEN_X
  //   }
  //   Barrier
  //
  //   Read XFB statistics info from LDS
  //   Read primsToWrite and dwordsWritten from XFB statistics info
  //
  //   for each vertex stream {
  //     if (threadIdInSubgroup < primsToWrite)
  //       Export XFB outputs to buffer for each vertice of this primitive
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

  BasicBlock *prepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".prepareXfbExport");
  prepareXfbExportBlock->moveAfter(insertPos);
  BasicBlock *endPrepareXfbExportBlock = createBlock(xfbEntryBlock->getParent(), ".endPrepareXfbExport");
  endPrepareXfbExportBlock->moveAfter(prepareXfbExportBlock);

  BasicBlock *exportXfbOutputBlock[MaxGsStreams] = {};
  BasicBlock *endExportXfbOutputBlock[MaxGsStreams] = {};
  insertPos = endPrepareXfbExportBlock;
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (m_pipelineState->isVertexStreamActive(i)) {
      exportXfbOutputBlock[i] = createBlock(xfbEntryBlock->getParent(), ".exportXfbOutputInStream" + std::to_string(i));
      exportXfbOutputBlock[i]->moveAfter(insertPos);
      insertPos = exportXfbOutputBlock[i];

      endExportXfbOutputBlock[i] =
          createBlock(xfbEntryBlock->getParent(), ".endExportXfbOutputInStream" + std::to_string(i));
      endExportXfbOutputBlock[i]->moveAfter(insertPos);
      insertPos = endExportXfbOutputBlock[i];
    }
  }

  // Insert branching in current block to process transform feedback export
  {
    auto validWave =
        m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(waveCountInSubgroup + 1));
    m_builder.CreateCondBr(validWave, initPrimitiveCountsBlock, endInitPrimitiveCountsBlock);
  }

  // Construct ".initPrimitiveCounts" block
  {
    m_builder.SetInsertPoint(initPrimitiveCountsBlock);

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (m_pipelineState->isVertexStreamActive(i)) {
        writePerThreadDataToLds(m_builder.getInt32(0), m_nggInputs.threadIdInSubgroup,
                                PrimShaderLdsRegion::PrimitiveCounts, (Gfx9::NggMaxWavesPerSubgroup + 1) * i);
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
        auto primData =
            readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                     PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * i);
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
    auto threadIdUpbound = m_builder.CreateSub(m_builder.getInt32(waveCountInSubgroup), m_nggInputs.waveIdInSubgroup);
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
        atomicAdd(
            primCountInWave[i],
            m_builder.CreateAdd(ldsOffset, m_builder.getInt32(regionStart + (Gfx9::NggMaxWavesPerSubgroup + 1) * i)));
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
                                   PrimShaderLdsRegion::PrimitiveCounts, (Gfx9::NggMaxWavesPerSubgroup + 1) * i);

      // The last dword following dwords for all waves (each wave has one dword) stores GS output primitive count of
      // the entire subgroup
      primCountInSubgroup[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                         {primCountInWaves, m_builder.getInt32(waveCountInSubgroup)});

      // Get output primitive count for all waves prior to this wave
      primCountInPrevWaves[i] = m_builder.CreateIntrinsic(m_builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                                          {primCountInWaves, m_nggInputs.waveIdInSubgroup});
    }

    m_builder.CreateCondBr(drawFlag[firstActiveStream], compactPrimitiveIndexBlock[firstActiveStream],
                           endCompactPrimitiveIndexBlock[firstActiveStream]);
  }

  SmallVector<XfbOutputExport, 32> xfbOutputExports;

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
                              PrimShaderLdsRegion::PrimitiveIndexMap, Gfx9::NggMaxThreadsPerSubgroup * i);

      m_builder.CreateBr(endCompactPrimitiveIndexBlock[i]);
    }

    // Construct ".endCompactPrimitiveIndexInStream[N]" block
    {
      m_builder.SetInsertPoint(endCompactPrimitiveIndexBlock[i]);

      if (i == lastActiveStream) {
        // Start to fetch transform feedback outputs after we finish compacting primitive index of the last vertex
        // stream.
        fetchXfbOutput(m_gsHandlers.copyShader, args, xfbOutputExports);

        auto firstThreadInSubgroup = m_builder.CreateICmpEQ(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(0));
        m_builder.CreateCondBr(firstThreadInSubgroup, prepareXfbExportBlock, endPrepareXfbExportBlock);
      } else {
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

  // Construct ".prepareXfbExport" block
  {
    m_builder.SetInsertPoint(prepareXfbExportBlock);

    const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();

    Value *numPrimsToWrite[MaxGsStreams] = {};
    for (unsigned i = 0; i < MaxGsStreams; ++i)
      numPrimsToWrite[i] = primCountInSubgroup[i];

    Value *dwordsWritten[MaxTransformFeedbackBuffers] = {};
    Value *dwordsPerPrim[MaxTransformFeedbackBuffers] = {};

    // Calculate numPrimsToWrite[N]
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      if (m_gfxIp.major <= 11) {
        if (i == firstActiveXfbBuffer) {
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
        } else {
          // ds_add_gs_reg
          dwordsWritten[i] =
              m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, m_builder.getInt32Ty(),
                                        {m_builder.getInt32(0),                                         // value to add
                                         m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
        }
      } else {
        llvm_unreachable("Not implemented!");
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
      dwordsPerPrim[i] = m_builder.getInt32(outVertsPerPrim * xfbStrides[i] / sizeof(unsigned));
      Value *primsCanWrite = m_builder.CreateUDiv(dwordsRemaining, dwordsPerPrim[i]);
      numPrimsToWrite[xfbBufferToStream[i]] =
          m_builder.CreateIntrinsic(Intrinsic::umin, numPrimsToWrite[xfbBufferToStream[i]]->getType(),
                                    {numPrimsToWrite[xfbBufferToStream[i]], primsCanWrite});
    }

    // Increment dwordsWritten
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      Value *dwordsToWrite = m_builder.CreateMul(numPrimsToWrite[xfbBufferToStream[i]], dwordsPerPrim[i]);

      if (m_gfxIp.major <= 11) {
        if (i == lastActiveXfbBuffer) {
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
        } else {
          // ds_add_gs_reg
          dwordsWritten[i] =
              m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, dwordsToWrite->getType(),
                                        {dwordsToWrite,                                                 // value to add
                                         m_builder.getInt32((GDS_STRMOUT_DWORDS_WRITTEN_0 + i) << 2)}); // count index
        }
      } else {
        llvm_unreachable("Not implemented!");
      }
    }

    // Store transform feedback statistics info to LDS and GDS
    const unsigned regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbStats);
    for (unsigned i = 0; i < MaxTransformFeedbackBuffers; ++i) {
      if (!bufferActive[i])
        continue;

      writeValueToLds(dwordsWritten[i], m_builder.getInt32(regionStart + i));
    }

    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      if (!m_pipelineState->isVertexStreamActive(i))
        continue;

      writeValueToLds(numPrimsToWrite[i], m_builder.getInt32(regionStart + MaxTransformFeedbackBuffers + i));

      if (m_gfxIp.major <= 11) {
        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, primCountInSubgroup[i]->getType(),
                                  {primCountInSubgroup[i],                                          // value to add
                                   m_builder.getInt32((GDS_STRMOUT_PRIMS_NEEDED_0 + 2 * i) << 2)}); // count index

        m_builder.CreateIntrinsic(Intrinsic::amdgcn_ds_add_gs_reg_rtn, numPrimsToWrite[i]->getType(),
                                  {numPrimsToWrite[i],                                               // value to add
                                   m_builder.getInt32((GDS_STRMOUT_PRIMS_WRITTEN_0 + 2 * i) << 2)}); // count index
      } else {
        llvm_unreachable("Not implemented!");
      }
    }

    m_builder.CreateBr(endPrepareXfbExportBlock);
  }

  // Construct ".endPrepareXfbExport" block
  Value *streamOutOffsets[MaxTransformFeedbackBuffers] = {}; // Stream-out offset to write transform feedback outputs
  Value *numPrimsToWrite[MaxGsStreams] = {};
  {
    m_builder.SetInsertPoint(endPrepareXfbExportBlock);

    // We are going to read transform feedback statistics info from LDS. Make sure the info has been written before
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
    m_builder.CreateCondBr(validPrimitive, exportXfbOutputBlock[firstActiveStream],
                           endExportXfbOutputBlock[firstActiveStream]);
  }

  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    if (!m_pipelineState->isVertexStreamActive(i))
      continue;

    // Construct ".exportXfbOutputInStream[N]" block
    {
      m_builder.SetInsertPoint(exportXfbOutputBlock[i]);

      Value *vertexIndices[3] = {};

      Value *uncompactedPrimitiveIndex =
          readPerThreadDataFromLds(m_builder.getInt32Ty(), m_nggInputs.threadIdInSubgroup,
                                   PrimShaderLdsRegion::PrimitiveIndexMap, Gfx9::NggMaxThreadsPerSubgroup * i);
      Value *vertexIndex = uncompactedPrimitiveIndex;

      const unsigned outVertsPerPrim = m_pipelineState->getVerticesPerPrimitive();
      vertexIndices[0] = vertexIndex;

      if (outVertsPerPrim > 1)
        vertexIndices[1] = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(1));
      if (outVertsPerPrim > 2) {
        vertexIndices[2] = m_builder.CreateAdd(vertexIndex, m_builder.getInt32(2));

        Value *primData =
            readPerThreadDataFromLds(m_builder.getInt32Ty(), uncompactedPrimitiveIndex,
                                     PrimShaderLdsRegion::PrimitiveData, Gfx9::NggMaxThreadsPerSubgroup * i);
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
        for (unsigned k = 0; k < xfbOutputExports.size(); ++k) {
          const auto &xfbOutputExport = xfbOutputExports[k];
          if (xfbOutputExport.locInfo.streamId != i)
            continue; // Output not belong to this stream

          auto outputValue =
              readGsOutput(xfbOutputExport.numElements > 1
                               ? FixedVectorType::get(m_builder.getFloatTy(), xfbOutputExport.numElements)
                               : m_builder.getFloatTy(),
                           xfbOutputExport.locInfo.location, xfbOutputExport.locInfo.component, i,
                           calcVertexItemOffset(i, vertexIndices[j]));

          if (xfbOutputExport.is16bit) {
            // NOTE: For 16-bit transform feedbakc outputs, they are stored as 32-bit without tightly packed in LDS.
            outputValue = m_builder.CreateBitCast(
                outputValue, FixedVectorType::get(m_builder.getInt32Ty(), xfbOutputExport.numElements));
            outputValue = m_builder.CreateTrunc(
                outputValue, FixedVectorType::get(m_builder.getInt16Ty(), xfbOutputExport.numElements));
            outputValue = m_builder.CreateBitCast(
                outputValue, FixedVectorType::get(m_builder.getHalfTy(), xfbOutputExport.numElements));
          }

          unsigned format = 0;
          switch (xfbOutputExport.numElements) {
          case 1:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_FLOAT : BUF_FORMAT_32_FLOAT;
            break;
          case 2:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_FLOAT_GFX11;
            break;
          case 3:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_FLOAT : BUF_FORMAT_32_32_32_FLOAT_GFX11;
            break;
          case 4:
            format = xfbOutputExport.is16bit ? BUF_FORMAT_16_16_16_16_FLOAT_GFX11 : BUF_FORMAT_32_32_32_32_FLOAT_GFX11;
            break;
          default:
            llvm_unreachable("Unexpected element number!");
            break;
          }

          CoherentFlag coherent = {};
          if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
            coherent.bits.glc = true;
            coherent.bits.slc = true;
          }

          // vertexOffset = (threadIdInSubgroup * outVertsPerPrim + vertexIndex) * xfbStride
          Value *vertexOffset = m_builder.CreateAdd(
              m_builder.CreateMul(m_nggInputs.threadIdInSubgroup, m_builder.getInt32(outVertsPerPrim)),
              m_builder.getInt32(j));
          vertexOffset = m_builder.CreateMul(vertexOffset, m_builder.getInt32(xfbStrides[xfbOutputExport.xfbBuffer]));
          // xfbOutputOffset = vertexOffset + xfbOffset
          Value *xfbOutputOffset = m_builder.CreateAdd(vertexOffset, m_builder.getInt32(xfbOutputExport.xfbOffset));

          if (xfbOutputExport.is16bit && xfbOutputExport.numElements == 3) {
            // NOTE: For 16vec3, HW doesn't have a corresponding buffer store instruction. We have to split it to 16vec2
            // and 16scalar.
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store,
                                      FixedVectorType::get(m_builder.getHalfTy(), 2),
                                      {m_builder.CreateShuffleVector(outputValue, ArrayRef<int>{0, 1}), // vdata
                                       m_streamOutBufDescs[xfbOutputExport.xfbBuffer],                  // rsrc
                                       xfbOutputOffset,                                                 // offset
                                       streamOutOffsets[xfbOutputExport.xfbBuffer],                     // soffset
                                       m_builder.getInt32(BUF_FORMAT_16_16_FLOAT),                      // format
                                       m_builder.getInt32(coherent.u32All)}); // auxiliary data

            m_builder.CreateIntrinsic(
                Intrinsic::amdgcn_raw_tbuffer_store, m_builder.getHalfTy(),
                {m_builder.CreateExtractElement(outputValue, 2),                                 // vdata
                 m_streamOutBufDescs[xfbOutputExport.xfbBuffer],                                 // rsrc
                 m_builder.CreateAdd(xfbOutputOffset, m_builder.getInt32(2 * sizeof(uint16_t))), // offset
                 streamOutOffsets[xfbOutputExport.xfbBuffer],                                    // soffset
                 m_builder.getInt32(BUF_FORMAT_16_FLOAT),                                        // format
                 m_builder.getInt32(coherent.u32All)});                                          // auxiliary data
          } else {
            m_builder.CreateIntrinsic(Intrinsic::amdgcn_raw_tbuffer_store, outputValue->getType(),
                                      {outputValue,                                    // vdata
                                       m_streamOutBufDescs[xfbOutputExport.xfbBuffer], // rsrc
                                       xfbOutputOffset,                                // offset
                                       streamOutOffsets[xfbOutputExport.xfbBuffer],    // soffset
                                       m_builder.getInt32(format),                     // format
                                       m_builder.getInt32(coherent.u32All)});          // auxiliary data
          }
        }
      }

      m_builder.CreateBr(endExportXfbOutputBlock[i]);
    }

    // Construct ".endExportXfbOutputInStream[N]" block
    {
      m_builder.SetInsertPoint(endExportXfbOutputBlock[i]);

      if (i != lastActiveStream) {
        unsigned nextActiveStream = i + 1;
        while (!m_pipelineState->isVertexStreamActive(nextActiveStream)) {
          ++nextActiveStream;
        }

        assert(nextActiveStream <= lastActiveStream);
        auto validPrimitive =
            m_builder.CreateICmpULT(m_nggInputs.threadIdInSubgroup, numPrimsToWrite[nextActiveStream]);
        m_builder.CreateCondBr(validPrimitive, exportXfbOutputBlock[nextActiveStream],
                               endExportXfbOutputBlock[nextActiveStream]);
      }
    }
  }
}

// =====================================================================================================================
// Fetches transform feedback outputs by creating a fetcher cloned from the target function or just mutating
// the target function and running it after that. Meanwhile, we collect the transform feedback export info.
//
// @param target : Target function to process SW emulated transform feedback
// @param args : Arguments of primitive shader entry-point
// @param [out] xfbOutputExports : Export info of transform feedback outputs
Value *NggPrimShader::fetchXfbOutput(Function *target, ArrayRef<Argument *> args,
                                     SmallVector<XfbOutputExport, 32> &xfbOutputExports) {
  assert(m_pipelineState->enableSwXfb());

  const unsigned xfbOutputCount =
      m_pipelineState
          ->getShaderResourceUsage(m_hasGs ? ShaderStageGeometry : (m_hasTes ? ShaderStageTessEval : ShaderStageVertex))
          ->inOutUsage.xfbExpCount;

  // Skip following handling if transform feedback output is empty
  if (xfbOutputCount == 0)
    return nullptr;

  //
  // Clone the target function or just mutate the target function to fetch transform feedback outputs
  //

  // We don't clone the target function if we are in passthrough mode without GS
  bool dontClone = !m_hasGs && m_nggControl->passthroughMode;

  // Collect all export calls for further analysis
  SmallVector<Function *, 8> expFuncs;
  for (auto &func : target->getParent()->functions()) {
    if (dontClone) {
      if (func.getName().startswith(lgcName::NggXfbExport))
        expFuncs.push_back(&func);
    } else {
      if ((func.isIntrinsic() && func.getIntrinsicID() == Intrinsic::amdgcn_exp) ||
          func.getName().startswith(lgcName::NggAttribExport) || func.getName().startswith(lgcName::NggXfbExport))
        expFuncs.push_back(&func);
    }
  }

  // Clone or mutate the target function
  xfbOutputExports.resize(xfbOutputCount);

  // NOTE: For non-GS transform feedback, the return type is represented as an array of transform feedback outputs; for
  // GS transform feedback, the return type is void. This is because output values must be loaded by GS read output
  // call. Thus, we don't have to return output values. Instead, we recode the location in transform feedback export
  // info and fetch them later.
  Type *xfbOutputsTy = ArrayType::get(FixedVectorType::get(m_builder.getInt32Ty(), 4), xfbOutputCount);
  Type *xfbReturnTy = m_hasGs ? m_builder.getVoidTy() : xfbOutputsTy;

  Function *xfbFetcher = target;
  if (dontClone) {
    processVertexAttribExport(target);
    xfbFetcher = addFunctionArgs(target, xfbReturnTy, {}, {}, 0);

    // Original target function is no longer needed
    assert(target->use_empty());
    target->eraseFromParent();
  } else {
    auto xfbFetcherTy = FunctionType::get(xfbReturnTy, target->getFunctionType()->params(), false);
    xfbFetcher = Function::Create(xfbFetcherTy, target->getLinkage(), "", target->getParent());

    ValueToValueMapTy valueMap;

    Argument *newArg = xfbFetcher->arg_begin();
    for (Argument &arg : target->args())
      valueMap[&arg] = newArg++;

    SmallVector<ReturnInst *, 8> retInsts;
    CloneFunctionInto(xfbFetcher, target, valueMap, CloneFunctionChangeType::LocalChangesOnly, retInsts);
    xfbFetcher->setName(NggXfbFetcher);
  }

  // Find the return block
  BasicBlock *retBlock = nullptr;
  for (BasicBlock &block : *xfbFetcher) {
    auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
    if (retInst) {
      retInst->dropAllReferences();
      retInst->eraseFromParent();

      retBlock = &block;
      break;
    }
  }
  assert(retBlock);

  auto savedInsertPos = m_builder.saveIP();
  m_builder.SetInsertPoint(retBlock);

  // Visit all export calls, removing those unnecessary and mutating the return type
  SmallVector<CallInst *, 8> removedCalls;

  Value *xfbOutputs = PoisonValue::get(xfbOutputsTy);
  unsigned outputIndex = 0;

  for (auto func : expFuncs) {
    for (auto user : func->users()) {
      CallInst *const call = dyn_cast<CallInst>(user);
      assert(call);

      if (!dontClone) {
        // Remove transform feedback export calls from the target function. No need of doing this if we
        // just mutate it without cloning.
        if (call->getFunction() == target && func->getName().startswith(lgcName::NggXfbExport)) {
          removedCalls.push_back(call);
          continue;
        }
      }

      if (call->getFunction() != xfbFetcher)
        continue;

      assert(call->getParent() == retBlock); // Must in return block

      if (func->getName().startswith(lgcName::NggXfbExport)) {
        // Lower transform feedback export calls
        auto xfbBuffer = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
        auto xfbOffset = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
        auto outputValue = call->getArgOperand(3);

        const unsigned numElements =
            outputValue->getType()->isVectorTy() ? cast<FixedVectorType>(outputValue->getType())->getNumElements() : 1;
        const bool is16bit = outputValue->getType()->getScalarSizeInBits() == 16;

        // Those values are just for GS
        auto streamId = InvalidValue;
        unsigned location = InvalidValue;
        unsigned component = InvalidValue;

        if (m_hasGs) {
          // NOTE: For GS, the output value must be loaded by GS read output call. This is generated by copy shader.
          CallInst *readCall = dyn_cast<CallInst>(outputValue);
          assert(readCall && readCall->getCalledFunction()->getName().startswith(lgcName::NggReadGsOutput));
          streamId = cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
          assert(streamId == cast<ConstantInt>(readCall->getArgOperand(2))->getZExtValue()); // Stream ID must match
          location = cast<ConstantInt>(readCall->getArgOperand(0))->getZExtValue();
          component = cast<ConstantInt>(readCall->getArgOperand(1))->getZExtValue();
        } else {
          // If the output value is floating point, cast it to integer type
          if (outputValue->getType()->isFPOrFPVectorTy()) {
            if (numElements == 1) {
              outputValue =
                  m_builder.CreateBitCast(outputValue, is16bit ? m_builder.getInt16Ty() : m_builder.getInt32Ty());
            } else {
              outputValue = m_builder.CreateBitCast(
                  outputValue,
                  FixedVectorType::get(is16bit ? m_builder.getInt16Ty() : m_builder.getInt32Ty(), numElements));
            }
          }

          // If the output value is 16-bit, zero-extend it to 32-bit
          if (is16bit)
            outputValue = m_builder.CreateZExt(outputValue, FixedVectorType::get(m_builder.getInt32Ty(), numElements));

          // Always pad the output value to <4 x i32>
          if (numElements == 1) {
            outputValue =
                m_builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(m_builder.getInt32Ty(), 4)),
                                              outputValue, static_cast<uint64_t>(0));
          } else if (numElements < 4) {
            outputValue = m_builder.CreateShuffleVector(outputValue, PoisonValue::get(outputValue->getType()),
                                                        ArrayRef<int>({0U, 1U, 2U, 3U}));
          }
        }

        // For VS/TES, return the output value
        if (!m_hasGs)
          xfbOutputs = m_builder.CreateInsertValue(xfbOutputs, outputValue, outputIndex);

        // Collect export info
        xfbOutputExports[outputIndex].xfbBuffer = xfbBuffer;
        xfbOutputExports[outputIndex].xfbOffset = xfbOffset;
        xfbOutputExports[outputIndex].numElements = numElements;
        xfbOutputExports[outputIndex].is16bit = is16bit;
        // Those values are just for GS
        xfbOutputExports[outputIndex].locInfo.streamId = streamId;
        xfbOutputExports[outputIndex].locInfo.location = location;
        xfbOutputExports[outputIndex].locInfo.component = component;

        ++outputIndex;
      }

      removedCalls.push_back(call); // Remove export
    }
  }

  assert(outputIndex == xfbOutputCount); // Visit all transform feedback export calls
  m_builder.CreateRet(xfbOutputs);

  // Remove calls
  for (auto call : removedCalls) {
    call->dropAllReferences();
    call->eraseFromParent();
  }

  m_builder.restoreIP(savedInsertPos);

  //
  // Run transform feedback fetch function
  //
  if (m_hasGs) {
    // Copy shader has fixed argument layout
    Value *userData = args[NumSpecialSgprInputs];
    assert(userData->getType()->isVectorTy());

    auto globalTable = m_builder.CreateExtractElement(userData, static_cast<uint64_t>(0));
    return m_builder.CreateCall(xfbFetcher,
                                {globalTable,                      // Global table
                                 m_nggInputs.threadIdInSubgroup}); // Relative vertex index in subgroup
  }

  Value *offChipLdsBase = args[ShaderMerger::getSpecialSgprInputIndex(m_gfxIp, EsGs::OffChipLdsBase)];
  offChipLdsBase->setName("offChipLdsBase");

  Value *userData = args[NumSpecialSgprInputs];

  ArrayRef<Argument *> vgprArgs(args.begin() + NumSpecialSgprInputs + 1, args.end());

  Value *tessCoordX = nullptr;
  Value *tessCoordY = nullptr;
  Value *relPatchId = nullptr;
  Value *patchId = nullptr;

  Value *vertexId = nullptr;
  Value *relVertexId = PoisonValue::get(m_builder.getInt32Ty());
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
    llvm_unreachable("Not implemented!");
  }

  SmallVector<Value *, 32> xfbFetcherArgs;

  // If we don't clone the target function, we are going to run it and handle vertex attribute through memory here.
  if (dontClone) {
    // Setup attribute ring base and relative vertx index in subgroup as two additional arguments to export vertex
    // attributes through memory
    if (m_gfxIp.major >= 11 && !m_hasGs) { // For GS, vertex attribute exports are in copy shader
      const auto attribCount =
          m_pipelineState->getShaderResourceUsage(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)
              ->inOutUsage.expCount;
      if (attribCount > 0) {
        xfbFetcherArgs.push_back(m_nggInputs.attribRingBase);
        xfbFetcherArgs.push_back(m_nggInputs.threadIdInSubgroup);
      }
    }
  }

  // Set up user data SGPRs
  const unsigned userDataCount =
      m_pipelineState->getShaderInterfaceData(m_hasTes ? ShaderStageTessEval : ShaderStageVertex)->userDataCount;
  appendUserData(xfbFetcherArgs, xfbFetcher, userData, userDataCount);

  if (m_hasTes) {
    // Set up system value SGPRs
    if (m_pipelineState->isTessOffChip()) {
      Value *isOffChip = PoisonValue::get(m_builder.getInt32Ty()); // Unused
      xfbFetcherArgs.push_back(isOffChip);
      xfbFetcherArgs.push_back(offChipLdsBase);
    }

    // Set up system value VGPRs
    xfbFetcherArgs.push_back(tessCoordX);
    xfbFetcherArgs.push_back(tessCoordY);
    xfbFetcherArgs.push_back(relPatchId);
    xfbFetcherArgs.push_back(patchId);
  } else {
    // Set up system value VGPRs
    xfbFetcherArgs.push_back(vertexId);
    xfbFetcherArgs.push_back(relVertexId);
    xfbFetcherArgs.push_back(vsPrimitiveId);
    xfbFetcherArgs.push_back(instanceId);

    if (m_nggControl->passthroughMode) {
      // When tessellation is not enabled, the transform feedback fetch function is actually a fetchless VS. Then, we
      // need to add arguments for the vertex fetches. Also set the name of each vertex fetch primitive shader argument
      // while we're here.
      unsigned vertexFetchCount = m_pipelineState->getPalMetadata()->getVertexFetchCount();
      if (vertexFetchCount > 0) {
        ArrayRef<Argument *> vertexFetches = vgprArgs.drop_front(m_gfxIp.major <= 11 ? 9 : 5);
        assert(vertexFetches.size() == vertexFetchCount);

        for (unsigned i = 0; i < vertexFetchCount; ++i) {
          vertexFetches[i]->setName(
              xfbFetcher->getArg(xfbFetcher->arg_size() - vertexFetchCount + i)->getName()); // Copy argument name
          xfbFetcherArgs.push_back(vertexFetches[i]);
        }
      }
    }
  }

  assert(xfbFetcherArgs.size() == xfbFetcher->arg_size()); // Must have visit all arguments

  return m_builder.CreateCall(xfbFetcher, xfbFetcherArgs);
}

// =====================================================================================================================
// Reads transform feedback output from LDS
//
// @param readDataTy : Data read from LDS
// @param vertexIndex: Relative vertex index in NGG subgroup
// @param outputIndex : Index of this transform feedback output
Value *NggPrimShader::readXfbOutputFromLds(Type *readDataTy, Value *vertexIndex, unsigned outputIndex) {
  assert(m_pipelineState->enableSwXfb()); // SW-emulated stream-out must be enabled
  assert(!m_hasGs);

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;
  auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize));

  if (m_nggControl->passthroughMode) {
    const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbOutput);
    Value *ldsOffset =
        m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + 4 * outputIndex)); // <4 x dword>
    return readValueFromLds(readDataTy, ldsOffset);
  }

  // NOTE: For NGG culling mode, transform feedback outputs are part of vertex cull info.
  const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCullInfo);
  Value *ldsOffset =
      m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + m_vertCullInfoOffsets.xfbOutputs +
                                                               4 * outputIndex)); // <4 x dword>
  return readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes transform feedback output from LDS
//
// @param writeData : Data written to LDS
// @param vertexIndex: Relative vertex index in NGG subgroup
// @param outputIndex : Index of this transform feedback output
void NggPrimShader::writeXfbOutputToLds(Value *writeData, Value *vertexIndex, unsigned outputIndex) {
  assert(m_pipelineState->enableSwXfb()); // SW-emulated stream-out must be enabled
  assert(!m_hasGs);

  const unsigned esGsRingItemSize =
      m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;
  auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize));

  if (m_nggControl->passthroughMode) {
    const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::XfbOutput);
    Value *ldsOffset =
        m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + 4 * outputIndex)); // <4 x dword>
    writeValueToLds(writeData, ldsOffset);
    return;
  }

  // NOTE: For NGG culling mode, transform feedback outputs are part of vertex cull info.
  const auto regionStart = getLdsRegionStart(PrimShaderLdsRegion::VertexCullInfo);
  Value *ldsOffset =
      m_builder.CreateAdd(vertexItemOffset, m_builder.getInt32(regionStart + m_vertCullInfoOffsets.xfbOutputs +
                                                               4 * outputIndex)); // <4 x dword>
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
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInPosition];
  const unsigned rasterStream = m_pipelineState->getRasterizerState().rasterStream;
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
        m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor.esGsRingItemSize;
    auto vertexItemOffset = m_builder.CreateMul(vertexIndex, m_builder.getInt32(esGsRingItemSize));
    return readVertexCullInfoFromLds(m_builder.getInt32Ty(), vertexItemOffset,
                                     m_vertCullInfoOffsets.cullDistanceSignMask);
  }

  // ES-GS
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;
  assert(inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end());
  const unsigned loc = inOutUsage.builtInOutputLocMap[BuiltInCullDistance];
  const unsigned rasterStream = m_pipelineState->getRasterizerState().rasterStream;
  auto vertexOffset = calcVertexItemOffset(rasterStream, vertexIndex);

  auto &builtInUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs;
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

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage;

  // vertexOffset = gsVsRingStart + streamBases[stream] + vertexIndex * vertexItemSize (in dwords)
  const unsigned vertexItemSize = 4 * inOutUsage.gs.outLocCount[streamId];
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
// Do atomic add operation with the value stored in LDS.
//
// @param valueToAdd : Value to do atomic add
// @param ldsOffset : Start offset to do LDS atomic operations (in dwords)
void NggPrimShader::atomicAdd(Value *ValueToAdd, Value *ldsOffset) {
  assert(ValueToAdd->getType()->isIntegerTy(32));

  Value *atomicPtr = m_builder.CreateGEP(m_lds->getValueType(), m_lds, {m_builder.getInt32(0), ldsOffset});

  SyncScope::ID syncScope = m_builder.getContext().getOrInsertSyncScopeID("workgroup");
  m_builder.CreateAtomicRMW(AtomicRMWInst::BinOp::Add, atomicPtr, ValueToAdd, MaybeAlign(),
                            AtomicOrdering::SequentiallyConsistent, syncScope);
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

} // namespace lgc
