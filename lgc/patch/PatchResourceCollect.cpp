/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchResourceCollect.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchResourceCollect.
 ***********************************************************************************************************************
 */
#include "PatchResourceCollect.h"
#include "Gfx6Chip.h"
#include "Gfx9Chip.h"
#include "NggLdsManager.h"
#include "NggPrimShader.h"
#include "lgc/Builder.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <set>

#define DEBUG_TYPE "lgc-patch-resource-collect"

using namespace llvm;
using namespace lgc;

// -disable-gs-onchip: disable geometry shader on-chip mode
cl::opt<bool> DisableGsOnChip("disable-gs-onchip", cl::desc("Disable geometry shader on-chip mode"), cl::init(false));

namespace lgc {

// =====================================================================================================================
// Initializes static members.
char PatchResourceCollect::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for resource collecting
ModulePass *createPatchResourceCollect() {
  return new PatchResourceCollect();
}

// =====================================================================================================================
PatchResourceCollect::PatchResourceCollect()
    : Patch(ID), m_hasDynIndexedInput(false), m_hasDynIndexedOutput(false), m_resUsage(nullptr) {
  m_locationMapManager = std::make_unique<InOutLocationMapManager>();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchResourceCollect::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Resource-Collect\n");

  Patch::init(&module);
  m_pipelineShaders = &getAnalysis<PipelineShaders>();
  m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);

  // If packing {VS, TES} outputs and {TCS, FS} inputs, scalarize those outputs and inputs now.
  if (m_pipelineState->canPackInOut())
    scalarizeForInOutPacking(&module);

  // Process each shader stage, in reverse order.
  for (int shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage) {
    m_entryPoint = m_pipelineShaders->getEntryPoint(static_cast<ShaderStage>(shaderStage));
    if (m_entryPoint) {
      m_shaderStage = static_cast<ShaderStage>(shaderStage);
      processShader();
    }
  }

  if (m_pipelineState->isGraphics()) {
    // Set NGG control settings
    setNggControl(&module);

    // Determine whether or not GS on-chip mode is valid for this pipeline
    bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
    bool checkGsOnChip = hasGs || m_pipelineState->getNggControl()->enableNgg;

    if (checkGsOnChip) {
      bool gsOnChip = checkGsOnChipValidity();
      m_pipelineState->setGsOnChip(gsOnChip);
    }
  }

  return true;
}

// =====================================================================================================================
// Sets NGG control settings
//
// @param [in/out] module : Module
void PatchResourceCollect::setNggControl(Module *module) {
  assert(m_pipelineState->isGraphics());

  // For GFX10+, initialize NGG control settings
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 10)
    return;

  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

  // Check the use of cull distance for NGG primitive shader
  bool useCullDistance = false;
  if (hasGs) {
    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    useCullDistance = resUsage->builtInUsage.gs.cullDistance > 0;
  } else if (hasTs) {
    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval);
    useCullDistance = resUsage->builtInUsage.tes.cullDistance > 0;
  } else {
    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageVertex);
    useCullDistance = resUsage->builtInUsage.vs.cullDistance > 0;
  }

  const auto &options = m_pipelineState->getOptions();
  NggControl &nggControl = *m_pipelineState->getNggControl();

  nggControl.enableNgg = canUseNgg(module);
  nggControl.enableGsUse = (options.nggFlags & NggFlagEnableGsUse);
  nggControl.alwaysUsePrimShaderTable = (options.nggFlags & NggFlagDontAlwaysUsePrimShaderTable) == 0;
  nggControl.compactMode = (options.nggFlags & NggFlagCompactDisable) ? NggCompactDisable : NggCompactVertices;

  nggControl.enableFastLaunch = false; // Currently, always false
  nggControl.enableVertexReuse = (options.nggFlags & NggFlagEnableVertexReuse);
  nggControl.enableBackfaceCulling = (options.nggFlags & NggFlagEnableBackfaceCulling);
  nggControl.enableFrustumCulling = (options.nggFlags & NggFlagEnableFrustumCulling);
  nggControl.enableBoxFilterCulling = (options.nggFlags & NggFlagEnableBoxFilterCulling);
  nggControl.enableSphereCulling = (options.nggFlags & NggFlagEnableSphereCulling);
  nggControl.enableSmallPrimFilter = (options.nggFlags & NggFlagEnableSmallPrimFilter);
  nggControl.enableCullDistanceCulling = ((options.nggFlags & NggFlagEnableCullDistanceCulling) && useCullDistance);

  nggControl.backfaceExponent = options.nggBackfaceExponent;
  nggControl.subgroupSizing = options.nggSubgroupSizing;
  nggControl.primsPerSubgroup = std::min(options.nggPrimsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup);
  nggControl.vertsPerSubgroup = std::min(options.nggVertsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup);

  if (nggControl.enableNgg) {
    if (options.nggFlags & NggFlagForceCullingMode)
      nggControl.passthroughMode = false;
    else {
      nggControl.passthroughMode = !nggControl.enableVertexReuse && !nggControl.enableBackfaceCulling &&
                                   !nggControl.enableFrustumCulling && !nggControl.enableBoxFilterCulling &&
                                   !nggControl.enableSphereCulling && !nggControl.enableSmallPrimFilter &&
                                   !nggControl.enableCullDistanceCulling;
    }

    // NOTE: Further check if we have to turn on pass-through mode forcibly.
    if (!nggControl.passthroughMode)
      nggControl.passthroughMode = !canUseNggCulling(module);

    // Build NGG culling-control registers
    buildNggCullingControlRegister(nggControl);

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC NGG control settings results\n\n");

    // Control option
    LLPC_OUTS("EnableNgg                    = " << nggControl.enableNgg << "\n");
    LLPC_OUTS("EnableGsUse                  = " << nggControl.enableGsUse << "\n");
    LLPC_OUTS("AlwaysUsePrimShaderTable     = " << nggControl.alwaysUsePrimShaderTable << "\n");
    LLPC_OUTS("PassthroughMode              = " << nggControl.passthroughMode << "\n");
    LLPC_OUTS("CompactMode                  = ");
    switch (nggControl.compactMode) {
    case NggCompactDisable:
      LLPC_OUTS("Disable\n");
      break;
    case NggCompactVertices:
      LLPC_OUTS("Vertices\n");
      break;
    default:
      break;
    }
    LLPC_OUTS("EnableFastLaunch             = " << nggControl.enableFastLaunch << "\n");
    LLPC_OUTS("EnableVertexReuse            = " << nggControl.enableVertexReuse << "\n");
    LLPC_OUTS("EnableBackfaceCulling        = " << nggControl.enableBackfaceCulling << "\n");
    LLPC_OUTS("EnableFrustumCulling         = " << nggControl.enableFrustumCulling << "\n");
    LLPC_OUTS("EnableBoxFilterCulling       = " << nggControl.enableBoxFilterCulling << "\n");
    LLPC_OUTS("EnableSphereCulling          = " << nggControl.enableSphereCulling << "\n");
    LLPC_OUTS("EnableSmallPrimFilter        = " << nggControl.enableSmallPrimFilter << "\n");
    LLPC_OUTS("EnableCullDistanceCulling    = " << nggControl.enableCullDistanceCulling << "\n");
    LLPC_OUTS("BackfaceExponent             = " << nggControl.backfaceExponent << "\n");
    LLPC_OUTS("SubgroupSizing               = ");
    switch (nggControl.subgroupSizing) {
    case NggSubgroupSizing::Auto:
      LLPC_OUTS("Auto\n");
      break;
    case NggSubgroupSizing::MaximumSize:
      LLPC_OUTS("MaximumSize\n");
      break;
    case NggSubgroupSizing::HalfSize:
      LLPC_OUTS("HalfSize\n");
      break;
    case NggSubgroupSizing::OptimizeForVerts:
      LLPC_OUTS("OptimizeForVerts\n");
      break;
    case NggSubgroupSizing::OptimizeForPrims:
      LLPC_OUTS("OptimizeForPrims\n");
      break;
    case NggSubgroupSizing::Explicit:
      LLPC_OUTS("Explicit\n");
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }
    LLPC_OUTS("PrimsPerSubgroup             = " << nggControl.primsPerSubgroup << "\n");
    LLPC_OUTS("VertsPerSubgroup             = " << nggControl.vertsPerSubgroup << "\n");
    LLPC_OUTS("\n");
  }
}

// =====================================================================================================================
// Checks whether NGG could be enabled.
//
// @param [in/out] module : Module
bool PatchResourceCollect::canUseNgg(Module *module) {
  assert(m_pipelineState->isGraphics());
  assert(m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 10);

  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

  // If the workaround flag requests us to disable NGG, respect it. Hardware must have some limitations.
  if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggDisabled)
    return false;

  // NGG used on GS is disabled by default.
  const auto &options = m_pipelineState->getOptions();
  if (hasGs && (options.nggFlags & NggFlagEnableGsUse) == 0)
    return false;

  // TODO: If transform feedback is enabled, currently disable NGG.
  const auto resUsage = m_pipelineState->getShaderResourceUsage(
      hasGs ? ShaderStageGeometry : (hasTs ? ShaderStageTessEval : ShaderStageVertex));
  if (resUsage->inOutUsage.enableXfb)
    return false;

  if (hasTs && hasGs) {
    auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();

    // NOTE: On GFX10, when tessllation and geometry shader are both enabled, the lowest number of GS primitives per
    // NGG subgroup is implicitly 3 (specified by HW). Thus, the maximum primitive amplification factor is therefore
    // 256/3 = 85.
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waLimitedMaxOutputVertexCount) {
      static const unsigned MaxOutputVertices = Gfx9::NggMaxThreadsPerSubgroup / 3;
      if (geometryMode.outputVertices > MaxOutputVertices)
        return false;
    }

    // NOTE: On GFX10, the bit VGT_GS_INSTANCE_CNT.EN_MAX_VERT_OUT_PER_GS_INSTANCE provided by HW allows each GS
    // instance to emit maximum vertices (256). But this mode is not supported when tessellation is enabled.
    if (m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waGeNggMaxVertOutWithGsInstancing) {
      if (geometryMode.invocations * geometryMode.outputVertices > Gfx9::NggMaxThreadsPerSubgroup)
        return false;
    }
  }

  // We can safely enable NGG here if NGG flag allows us to do so
  return (options.nggFlags & NggFlagDisable) == 0;
}

// =====================================================================================================================
// Checks whether NGG culling could be enabled.
//
// @param [in/out] module : Module
bool PatchResourceCollect::canUseNggCulling(Module *module) {
  assert(m_pipelineState->isGraphics());
  assert(m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 10);

  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);

  // Check topology, disable NGG culling if primitive is not triangle-based
  if (hasGs) {
    // For GS, check output primitive type
    const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    if (geometryMode.outputPrimitive != OutputPrimitives::TriangleStrip) {
      return false;
    }
  } else {
    const auto topology = m_pipelineState->getInputAssemblyState().topology;
    if (hasTs) {
      // For tessellation, check primitive mode
      assert(topology == PrimitiveTopology::PatchList);
      const auto &tessMode = m_pipelineState->getShaderModes()->getTessellationMode();
      if (tessMode.pointMode || tessMode.primitiveMode == PrimitiveMode::Isolines)
        return false;
    } else {
      // Check topology specified in pipeline state
      if (topology == PrimitiveTopology::PointList || topology == PrimitiveTopology::LineList ||
          topology == PrimitiveTopology::LineStrip || topology == PrimitiveTopology::LineListWithAdjacency ||
          topology == PrimitiveTopology::LineStripWithAdjacency)
        return false;
    }
  }

  // Check polygon mode, disable NGG culling if not filled mode
  const auto polygonMode = m_pipelineState->getRasterizerState().polygonMode;
  if (polygonMode == PolygonModeLine || polygonMode == PolygonModePoint) {
    return false;
  }

  // Check resource usage, disable culling if there are resource write operations (including atomic operations) in
  // non-GS NGG cases. This is because such write operations have side effect in execution sequences. But in GS NGG
  // cases, we can still enable culling. Culling is performed after GS execution.
  if (!hasGs) {
    const auto resUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    if (resUsage->resourceWrite)
      return false;
  }

  // Check the presence of position export, disable NGG culling if position export is absent
  bool usePosition = false;
  if (hasGs)
    usePosition = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs.position;
  else if (hasTs)
    usePosition = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes.position;
  else
    usePosition = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.position;

  if (!usePosition)
    return false; // No position export

  // Find position export call
  std::string posCallName = lgcName::OutputExportBuiltIn;
  posCallName += PipelineState::getBuiltInName(BuiltInPosition);
  auto callStage = hasGs ? ShaderStageGeometry : (hasTs ? ShaderStageTessEval : ShaderStageVertex);

  CallInst *posCall = nullptr;
  for (Function &func : *module) {
    if (func.getName().startswith(posCallName)) {
      for (User *user : func.users()) {
        auto call = cast<CallInst>(user);
        if (m_pipelineShaders->getShaderStage(call->getFunction()) == callStage) {
          posCall = call;
          break;
        }
      }

      if (posCall) // Already find the call
        break;
    }
  }
  assert(posCall); // Position export must exist

  // Check position value, disable NGG culling if it is constant
  auto posValue = posCall->getArgOperand(posCall->getNumArgOperands() - 1); // Last argument is position value
  if (isa<Constant>(posValue))
    return false;

  // We can safely enable NGG culling here
  return true;
}

// =====================================================================================================================
// Builds NGG culling-control registers (fill part of compile-time primitive shader table).
//
// @param [in/out] nggControl : NggControl struct
void PatchResourceCollect::buildNggCullingControlRegister(NggControl &nggControl) {
  const auto &vpState = m_pipelineState->getViewportState();
  const auto &rsState = m_pipelineState->getRasterizerState();

  auto &pipelineState = nggControl.primShaderTable.pipelineStateCb;

  //
  // Program register PA_SU_SC_MODE_CNTL
  //
  PaSuScModeCntl paSuScModeCntl;
  paSuScModeCntl.u32All = 0;

  paSuScModeCntl.bits.polyOffsetFrontEnable = rsState.depthBiasEnable;
  paSuScModeCntl.bits.polyOffsetBackEnable = rsState.depthBiasEnable;
  paSuScModeCntl.bits.multiPrimIbEna = true;

  paSuScModeCntl.bits.polyMode = rsState.polygonMode != PolygonModeFill;

  if (rsState.polygonMode == PolygonModeFill) {
    paSuScModeCntl.bits.polymodeBackPtype = POLY_MODE_TRIANGLES;
    paSuScModeCntl.bits.polymodeFrontPtype = POLY_MODE_TRIANGLES;
  } else if (rsState.polygonMode == PolygonModeLine) {
    paSuScModeCntl.bits.polymodeBackPtype = POLY_MODE_LINES;
    paSuScModeCntl.bits.polymodeFrontPtype = POLY_MODE_LINES;
  } else if (rsState.polygonMode == PolygonModePoint) {
    paSuScModeCntl.bits.polymodeBackPtype = POLY_MODE_POINTS;
    paSuScModeCntl.bits.polymodeFrontPtype = POLY_MODE_POINTS;
  } else
    llvm_unreachable("Should never be called!");

  paSuScModeCntl.bits.cullFront = (rsState.cullMode & CullModeFront) != 0;
  paSuScModeCntl.bits.cullBack = (rsState.cullMode & CullModeBack) != 0;

  paSuScModeCntl.bits.face = rsState.frontFaceClockwise;

  pipelineState.paSuScModeCntl = paSuScModeCntl.u32All;

  //
  // Program register PA_CL_CLIP_CNTL
  //
  PaClClipCntl paClClipCntl;
  assert((rsState.usrClipPlaneMask & ~0x3F) == 0);
  paClClipCntl.u32All = rsState.usrClipPlaneMask;

  paClClipCntl.bits.dxClipSpaceDef = true;
  paClClipCntl.bits.dxLinearAttrClipEna = true;

  if (!static_cast<bool>(vpState.depthClipEnable)) {
    paClClipCntl.bits.zclipNearDisable = true;
    paClClipCntl.bits.zclipFarDisable = true;
  }

  if (rsState.rasterizerDiscardEnable)
    paClClipCntl.bits.dxRasterizationKill = true;

  pipelineState.paClClipCntl = paClClipCntl.u32All;

  //
  // Program register PA_CL_VTE_CNTL
  //
  PaClVteCntl paClVteCntl;
  paClVteCntl.u32All = 0;

  paClVteCntl.bits.vportXScaleEna = true;
  paClVteCntl.bits.vportXOffsetEna = true;
  paClVteCntl.bits.vportYScaleEna = true;
  paClVteCntl.bits.vportYOffsetEna = true;
  paClVteCntl.bits.vportZScaleEna = true;
  paClVteCntl.bits.vportZOffsetEna = true;
  paClVteCntl.bits.vtxW0Fmt = true;

  pipelineState.paClVteCntl = paClVteCntl.u32All;
}

// =====================================================================================================================
// Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
bool PatchResourceCollect::checkGsOnChipValidity() {
  bool gsOnChip = true;

  unsigned stageMask = m_pipelineState->getShaderStageMask();
  const bool hasTs =
      ((stageMask & (shaderStageToMask(ShaderStageTessControl) | shaderStageToMask(ShaderStageTessEval))) != 0);
  const bool hasGs = ((stageMask & shaderStageToMask(ShaderStageGeometry)) != 0);

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  auto gsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

  unsigned inVertsPerPrim = 0;
  bool useAdjacency = false;
  switch (geometryMode.inputPrimitive) {
  case InputPrimitives::Points:
    inVertsPerPrim = 1;
    break;
  case InputPrimitives::Lines:
    inVertsPerPrim = 2;
    break;
  case InputPrimitives::LinesAdjacency:
    useAdjacency = true;
    inVertsPerPrim = 4;
    break;
  case InputPrimitives::Triangles:
    inVertsPerPrim = 3;
    break;
  case InputPrimitives::TrianglesAdjacency:
    useAdjacency = true;
    inVertsPerPrim = 6;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  gsResUsage->inOutUsage.gs.calcFactor.inputVertices = inVertsPerPrim;

  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 8) {
    unsigned gsPrimsPerSubgroup = m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipDefaultPrimsPerSubgroup;

    const unsigned esGsRingItemSize = 4 * std::max(1u, gsResUsage->inOutUsage.inputMapLocCount);
    const unsigned gsInstanceCount = geometryMode.invocations;
    const unsigned gsVsRingItemSize =
        4 * std::max(1u, (gsResUsage->inOutUsage.outputMapLocCount * geometryMode.outputVertices));

    unsigned esGsRingItemSizeOnChip = esGsRingItemSize;
    unsigned gsVsRingItemSizeOnChip = gsVsRingItemSize;

    // Optimize ES -> GS ring and GS -> VS ring layout for bank conflicts
    esGsRingItemSizeOnChip |= 1;
    gsVsRingItemSizeOnChip |= 1;

    unsigned gsVsRingItemSizeOnChipInstanced = gsVsRingItemSizeOnChip * gsInstanceCount;

    unsigned esMinVertsPerSubgroup = inVertsPerPrim;

    // If the primitive has adjacency half the number of vertices will be reused in multiple primitives.
    if (useAdjacency)
      esMinVertsPerSubgroup >>= 1;

    // There is a hardware requirement for gsPrimsPerSubgroup * gsInstanceCount to be capped by
    // GsOnChipMaxPrimsPerSubgroup for adjacency primitive or when GS instanceing is used.
    if (useAdjacency || gsInstanceCount > 1)
      gsPrimsPerSubgroup = std::min(gsPrimsPerSubgroup, (Gfx6::GsOnChipMaxPrimsPerSubgroup / gsInstanceCount));

    // Compute GS-VS LDS size based on target GS primitives per subgroup
    unsigned gsVsLdsSize = (gsVsRingItemSizeOnChipInstanced * gsPrimsPerSubgroup);

    // Compute ES-GS LDS size based on the worst case number of ES vertices needed to create the target number of
    // GS primitives per subgroup.
    const unsigned reuseOffMultiplier = isVertexReuseDisabled() ? gsInstanceCount : 1;
    unsigned worstCaseEsVertsPerSubgroup = esMinVertsPerSubgroup * gsPrimsPerSubgroup * reuseOffMultiplier;
    unsigned esGsLdsSize = esGsRingItemSizeOnChip * worstCaseEsVertsPerSubgroup;

    // Total LDS use per subgroup aligned to the register granularity
    unsigned gsOnChipLdsSize = alignTo(
        (esGsLdsSize + gsVsLdsSize),
        static_cast<unsigned>((1 << m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift)));

    // Use the client-specified amount of LDS space per subgroup. If they specified zero, they want us to choose a
    // reasonable default. The final amount must be 128-dword aligned.

    unsigned maxLdsSize = m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipDefaultLdsSizePerSubgroup;

    // TODO: For BONAIRE A0, GODAVARI and KALINDI, set maxLdsSize to 1024 due to SPI barrier management bug

    // If total LDS usage is too big, refactor partitions based on ratio of ES-GS and GS-VS item sizes.
    if (gsOnChipLdsSize > maxLdsSize) {
      const unsigned esGsItemSizePerPrim = esGsRingItemSizeOnChip * esMinVertsPerSubgroup * reuseOffMultiplier;
      const unsigned itemSizeTotal = esGsItemSizePerPrim + gsVsRingItemSizeOnChipInstanced;

      esGsLdsSize = alignTo((esGsItemSizePerPrim * maxLdsSize) / itemSizeTotal, esGsItemSizePerPrim);
      gsVsLdsSize = alignDown(maxLdsSize - esGsLdsSize, gsVsRingItemSizeOnChipInstanced);

      gsOnChipLdsSize = maxLdsSize;
    }

    // Based on the LDS space, calculate how many GS prims per subgroup and ES vertices per subgroup can be dispatched.
    gsPrimsPerSubgroup = (gsVsLdsSize / gsVsRingItemSizeOnChipInstanced);
    unsigned esVertsPerSubgroup = (esGsLdsSize / (esGsRingItemSizeOnChip * reuseOffMultiplier));

    assert(esVertsPerSubgroup >= esMinVertsPerSubgroup);

    // Vertices for adjacency primitives are not always reused. According to
    // hardware engineers, we must restore esMinVertsPerSubgroup for ES_VERTS_PER_SUBGRP.
    if (useAdjacency)
      esMinVertsPerSubgroup = inVertsPerPrim;

    // For normal primitives, the VGT only checks if they are past the ES verts per sub-group after allocating a full
    // GS primitive and if they are, kick off a new sub group. But if those additional ES vertices are unique
    // (e.g. not reused) we need to make sure there is enough LDS space to account for those ES verts beyond
    // ES_VERTS_PER_SUBGRP.
    esVertsPerSubgroup -= (esMinVertsPerSubgroup - 1);

    // TODO: Accept GsOffChipDefaultThreshold from panel option
    // TODO: Value of GsOffChipDefaultThreshold should be 64, due to an issue it's changed to 32 in order to test
    // on-chip GS code generation before fixing that issue.
    // The issue is because we only remove unused builtin output till final GS output store generation, when
    // determining onchip/offchip mode, unused builtin output like PointSize and Clip/CullDistance is factored in
    // LDS usage and deactivates onchip GS when GsOffChipDefaultThreshold  is 64. To fix this we will probably
    // need to clear unused builtin ouput before determining onchip/offchip GS mode.
    constexpr unsigned gsOffChipDefaultThreshold = 32;

    bool disableGsOnChip = DisableGsOnChip;
    if (hasTs || m_pipelineState->getTargetInfo().getGfxIpVersion().major == 6) {
      // GS on-chip is not supportd with tessellation, and is not supportd on GFX6
      disableGsOnChip = true;
    }

    if (disableGsOnChip || (gsPrimsPerSubgroup * gsInstanceCount) < gsOffChipDefaultThreshold ||
        esVertsPerSubgroup == 0) {
      gsOnChip = false;
      gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = 0;
      gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = 0;
      gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize = 0;
      gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = 0;

      gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize = esGsRingItemSize;
      gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize = gsVsRingItemSize;
    } else {
      gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize = esGsLdsSize;
      gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = gsOnChipLdsSize;

      gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize = esGsRingItemSizeOnChip;
      gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize = gsVsRingItemSizeOnChip;
    }
  } else {
    const auto nggControl = m_pipelineState->getNggControl();

    if (nggControl->enableNgg) {
      unsigned esGsRingItemSize = NggPrimShader::calcEsGsRingItemSize(m_pipelineState); // In dwords

      const unsigned gsVsRingItemSize =
          hasGs ? std::max(1u, 4 * gsResUsage->inOutUsage.outputMapLocCount * geometryMode.outputVertices) : 0;

      const unsigned esExtraLdsSize = NggLdsManager::calcEsExtraLdsSize(m_pipelineState) / 4; // In dwords
      const unsigned gsExtraLdsSize = NggLdsManager::calcGsExtraLdsSize(m_pipelineState) / 4; // In dwords

      // NOTE: Primitive amplification factor must be at least 1. And for NGG GS mode, we force number of output
      // primitives to be equal to that of output vertices regardless of the output primitive type by emitting
      // invalid primitives. This is to simplify the algorithmic design of NGG GS and improve its efficiency.
      unsigned primAmpFactor = std::max(1u, geometryMode.outputVertices);

      const unsigned vertsPerPrimitive = getVerticesPerPrimitive();

      const bool needsLds = (hasGs || !nggControl->passthroughMode || esExtraLdsSize > 0 || gsExtraLdsSize > 0);

      unsigned esVertsPerSubgroup = 0;
      unsigned gsPrimsPerSubgroup = 0;

      // It is expected that regular launch NGG will be the most prevalent, so handle its logic first.
      if (!nggControl->enableFastLaunch) {
        // The numbers below come from hardware guidance and most likely require further tuning.
        switch (nggControl->subgroupSizing) {
        case NggSubgroupSizing::HalfSize:
          esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
          gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
          break;
        case NggSubgroupSizing::OptimizeForVerts:
          esVertsPerSubgroup = hasTs ? 128 : 126;
          gsPrimsPerSubgroup = hasTs || needsLds ? 192 : Gfx9::NggMaxThreadsPerSubgroup;
          break;
        case NggSubgroupSizing::OptimizeForPrims:
          esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
          gsPrimsPerSubgroup = 128;
          break;
        case NggSubgroupSizing::Explicit:
          esVertsPerSubgroup = nggControl->vertsPerSubgroup;
          gsPrimsPerSubgroup = nggControl->primsPerSubgroup;
          break;
        default:
        case NggSubgroupSizing::Auto:
          esVertsPerSubgroup = 126;
          gsPrimsPerSubgroup = 128;
          break;
        case NggSubgroupSizing::MaximumSize:
          esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
          gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
          break;
        }
      } else {
        // Fast launch NGG launches like a compute shader and bypasses most of the fixed function hardware.
        // As such, the values of esVerts and gsPrims have to be accurate for the primitive type
        // (and vertsPerPrimitive) to avoid hanging.
        switch (nggControl->subgroupSizing) {
        case NggSubgroupSizing::HalfSize:
          esVertsPerSubgroup = alignDown((Gfx9::NggMaxThreadsPerSubgroup / 2u), vertsPerPrimitive);
          gsPrimsPerSubgroup = esVertsPerSubgroup / vertsPerPrimitive;
          break;
        case NggSubgroupSizing::OptimizeForVerts:
          // Currently the programming of OptimizeForVerts is an inverse of MaximumSize. OptimizeForVerts is
          // not expected to be a performant choice for fast launch, and as such MaximumSize, HalfSize, or
          // Explicit should be chosen, with Explicit being optimal for non-point topologies.
          gsPrimsPerSubgroup = alignDown(Gfx9::NggMaxThreadsPerSubgroup, vertsPerPrimitive);
          esVertsPerSubgroup = gsPrimsPerSubgroup / vertsPerPrimitive;
          break;
        case NggSubgroupSizing::Explicit:
          esVertsPerSubgroup = nggControl->vertsPerSubgroup;
          gsPrimsPerSubgroup = nggControl->primsPerSubgroup;
          break;
        case NggSubgroupSizing::OptimizeForPrims:
          // Currently the programming of OptimizeForPrims is the same as MaximumSize, it is possible that
          // this might change in the future. OptimizeForPrims is not expected to be a performant choice for
          // fast launch, and as such MaximumSize, HalfSize, or Explicit should be chosen, with Explicit
          // being optimal for non-point topologies.
          // Fallthrough intentional.
        case NggSubgroupSizing::Auto:
        case NggSubgroupSizing::MaximumSize:
        default:
          esVertsPerSubgroup = alignDown(Gfx9::NggMaxThreadsPerSubgroup, vertsPerPrimitive);
          gsPrimsPerSubgroup = esVertsPerSubgroup / vertsPerPrimitive;
          break;
        }
      }

      unsigned gsInstanceCount = std::max(1u, geometryMode.invocations);
      bool enableMaxVertOut = false;

      if (hasGs) {
        // NOTE: If primitive amplification is active and the currently calculated gsPrimsPerSubgroup multipled
        // by the amplification factor is larger than the supported number of primitives within a subgroup, we
        // need to shrimp the number of gsPrimsPerSubgroup down to a reasonable level to prevent
        // over-allocating LDS.
        unsigned maxVertOut = hasGs ? geometryMode.outputVertices : 1;

        assert(maxVertOut >= primAmpFactor);

        if ((gsPrimsPerSubgroup * maxVertOut) > Gfx9::NggMaxThreadsPerSubgroup)
          gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / maxVertOut;

        // Let's take into consideration instancing:
        assert(gsInstanceCount >= 1);
        if (gsPrimsPerSubgroup < gsInstanceCount) {
          // NOTE: If supported number of GS primitives within a subgroup is too small to allow GS
          // instancing, we enable maximum vertex output per GS instance. This will set the register field
          // EN_MAX_VERT_OUT_PER_GS_INSTANCE and turn off vertex reuse, restricting 1 input GS input
          // primitive per subgroup and create 1 subgroup per GS instance.
          enableMaxVertOut = true;
          gsInstanceCount = 1;
          gsPrimsPerSubgroup = 1;
        } else
          gsPrimsPerSubgroup /= gsInstanceCount;
        esVertsPerSubgroup = gsPrimsPerSubgroup * maxVertOut;
      } else {
        // If GS is not present, instance count must be 1
        assert(gsInstanceCount == 1);
      }

      // Make sure that we have at least one primitive
      assert(gsPrimsPerSubgroup >= 1);

      unsigned expectedEsLdsSize = esVertsPerSubgroup * esGsRingItemSize + esExtraLdsSize;
      const unsigned expectedGsLdsSize = gsPrimsPerSubgroup * gsInstanceCount * gsVsRingItemSize + gsExtraLdsSize;

      if (expectedGsLdsSize == 0) {
        assert(hasGs == false);

        expectedEsLdsSize = (Gfx9::NggMaxThreadsPerSubgroup * esGsRingItemSize) + esExtraLdsSize;
      }

      const unsigned ldsSizeDwords = alignTo(
          expectedEsLdsSize + expectedGsLdsSize,
          static_cast<unsigned>(1 << m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift));

      // Make sure we don't allocate more than what can legally be allocated by a single subgroup on the hardware.
      assert(ldsSizeDwords <= 16384);

      gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsPerSubgroup;

      // EsGsLdsSize is passed in a user data SGPR to the merged shader so that the API GS knows where to start
      // reading out of LDS. EsGsLdsSize is unnecessary when there is no API GS.
      gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize = hasGs ? expectedEsLdsSize : 0;
      gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = needsLds ? ldsSizeDwords : 0;

      gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize = esGsRingItemSize;
      gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize = gsVsRingItemSize;

      gsResUsage->inOutUsage.gs.calcFactor.primAmpFactor = primAmpFactor;
      gsResUsage->inOutUsage.gs.calcFactor.enableMaxVertOut = enableMaxVertOut;

      gsOnChip = true; // In NGG mode, GS is always on-chip since copy shader is not present.
    } else {
      unsigned ldsSizeDwordGranularity =
          static_cast<unsigned>(1 << m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift);

      // gsPrimsPerSubgroup shouldn't be bigger than wave size.
      unsigned gsPrimsPerSubgroup =
          std::min(m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipDefaultPrimsPerSubgroup,
                   m_pipelineState->getShaderWaveSize(ShaderStageGeometry));

      // NOTE: Make esGsRingItemSize odd by "| 1", to optimize ES -> GS ring layout for LDS bank conflicts.
      const unsigned esGsRingItemSize = (4 * std::max(1u, gsResUsage->inOutUsage.inputMapLocCount)) | 1;

      const unsigned gsVsRingItemSize =
          4 * std::max(1u, (gsResUsage->inOutUsage.outputMapLocCount * geometryMode.outputVertices));

      // NOTE: Make gsVsRingItemSize odd by "| 1", to optimize GS -> VS ring layout for LDS bank conflicts.
      const unsigned gsVsRingItemSizeOnChip = gsVsRingItemSize | 1;

      const unsigned gsInstanceCount = geometryMode.invocations;

      // TODO: Confirm no ES-GS extra LDS space used.
      const unsigned esGsExtraLdsDwords = 0;
      const unsigned maxEsVertsPerSubgroup = Gfx9::OnChipGsMaxEsVertsPerSubgroup;

      unsigned esMinVertsPerSubgroup = inVertsPerPrim;

      // If the primitive has adjacency half the number of vertices will be reused in multiple primitives.
      if (useAdjacency)
        esMinVertsPerSubgroup >>= 1;

      unsigned maxGsPrimsPerSubgroup = Gfx9::OnChipGsMaxPrimPerSubgroup;

      // There is a hardware requirement for gsPrimsPerSubgroup * gsInstanceCount to be capped by
      // OnChipGsMaxPrimPerSubgroup for adjacency primitive or when GS instanceing is used.
      if (useAdjacency || gsInstanceCount > 1)
        maxGsPrimsPerSubgroup = (Gfx9::OnChipGsMaxPrimPerSubgroupAdj / gsInstanceCount);

      gsPrimsPerSubgroup = std::min(gsPrimsPerSubgroup, maxGsPrimsPerSubgroup);

      const unsigned reuseOffMultiplier = isVertexReuseDisabled() ? gsInstanceCount : 1;
      unsigned worstCaseEsVertsPerSubgroup =
          std::min(esMinVertsPerSubgroup * gsPrimsPerSubgroup * reuseOffMultiplier, maxEsVertsPerSubgroup);

      unsigned esGsLdsSize = (esGsRingItemSize * worstCaseEsVertsPerSubgroup);

      // Total LDS use per subgroup aligned to the register granularity.
      unsigned gsOnChipLdsSize = alignTo(esGsLdsSize + esGsExtraLdsDwords, ldsSizeDwordGranularity);

      // Use the client-specified amount of LDS space per sub-group. If they specified zero, they want us to
      // choose a reasonable default. The final amount must be 128-dword aligned.
      // TODO: Accept DefaultLdsSizePerSubgroup from panel setting
      unsigned maxLdsSize = Gfx9::DefaultLdsSizePerSubgroup;

      // If total LDS usage is too big, refactor partitions based on ratio of ES-GS item sizes.
      if (gsOnChipLdsSize > maxLdsSize) {
        // Our target GS primitives per sub-group was too large

        // Calculate the maximum number of GS primitives per sub-group that will fit into LDS, capped
        // by the maximum that the hardware can support.
        unsigned availableLdsSize = maxLdsSize - esGsExtraLdsDwords;
        gsPrimsPerSubgroup =
            std::min((availableLdsSize / (esGsRingItemSize * esMinVertsPerSubgroup)), maxGsPrimsPerSubgroup);
        worstCaseEsVertsPerSubgroup =
            std::min(esMinVertsPerSubgroup * gsPrimsPerSubgroup * reuseOffMultiplier, maxEsVertsPerSubgroup);

        assert(gsPrimsPerSubgroup > 0);

        esGsLdsSize = (esGsRingItemSize * worstCaseEsVertsPerSubgroup);
        gsOnChipLdsSize = alignTo(esGsLdsSize + esGsExtraLdsDwords, ldsSizeDwordGranularity);

        assert(gsOnChipLdsSize <= maxLdsSize);
      }

      if (hasTs || DisableGsOnChip)
        gsOnChip = false;
      else {
        // Now let's calculate the onchip GSVS info and determine if it should be on or off chip.
        unsigned gsVsItemSize = gsVsRingItemSizeOnChip * gsInstanceCount;

        // Compute GSVS LDS size based on target GS prims per subgroup.
        unsigned gsVsLdsSize = gsVsItemSize * gsPrimsPerSubgroup;

        // Start out with the assumption that our GS prims per subgroup won't change.
        unsigned onchipGsPrimsPerSubgroup = gsPrimsPerSubgroup;

        // Total LDS use per subgroup aligned to the register granularity to keep ESGS and GSVS data on chip.
        unsigned onchipEsGsVsLdsSize = alignTo(esGsLdsSize + gsVsLdsSize, ldsSizeDwordGranularity);
        unsigned onchipEsGsLdsSizeOnchipGsVs = esGsLdsSize;

        if (onchipEsGsVsLdsSize > maxLdsSize) {
          // TODO: This code only allocates the minimum required LDS to hit the on chip GS prims per subgroup
          //       threshold. This leaves some LDS space unused. The extra space could potentially be used to
          //       increase the GS Prims per subgroup.

          // Set the threshold at the minimum to keep things on chip.
          onchipGsPrimsPerSubgroup = maxGsPrimsPerSubgroup;

          if (onchipGsPrimsPerSubgroup > 0) {
            worstCaseEsVertsPerSubgroup =
                std::min(esMinVertsPerSubgroup * onchipGsPrimsPerSubgroup * reuseOffMultiplier, maxEsVertsPerSubgroup);

            // Calculate the LDS sizes required to hit this threshold.
            onchipEsGsLdsSizeOnchipGsVs =
                alignTo(esGsRingItemSize * worstCaseEsVertsPerSubgroup, ldsSizeDwordGranularity);
            gsVsLdsSize = gsVsItemSize * onchipGsPrimsPerSubgroup;
            onchipEsGsVsLdsSize = onchipEsGsLdsSizeOnchipGsVs + gsVsLdsSize;

            if (onchipEsGsVsLdsSize > maxLdsSize) {
              // LDS isn't big enough to hit the target GS prim per subgroup count for on chip GSVS.
              gsOnChip = false;
            }
          } else {
            // With high GS instance counts, it is possible that the number of on chip GS prims
            // calculated is zero. If this is the case, we can't expect to use on chip GS.
            gsOnChip = false;
          }
        }

        // If on chip GSVS is optimal, update the ESGS parameters with any changes that allowed for GSVS data.
        if (gsOnChip) {
          gsOnChipLdsSize = onchipEsGsVsLdsSize;
          esGsLdsSize = onchipEsGsLdsSizeOnchipGsVs;
          gsPrimsPerSubgroup = onchipGsPrimsPerSubgroup;
        }
      }

      unsigned esVertsPerSubgroup =
          std::min(esGsLdsSize / (esGsRingItemSize * reuseOffMultiplier), maxEsVertsPerSubgroup);

      assert(esVertsPerSubgroup >= esMinVertsPerSubgroup);

      // Vertices for adjacency primitives are not always reused (e.g. in the case of shadow volumes). Acording
      // to hardware engineers, we must restore esMinVertsPerSubgroup for ES_VERTS_PER_SUBGRP.
      if (useAdjacency)
        esMinVertsPerSubgroup = inVertsPerPrim;

      // For normal primitives, the VGT only checks if they are past the ES verts per sub group after allocating
      // a full GS primitive and if they are, kick off a new sub group.  But if those additional ES verts are
      // unique (e.g. not reused) we need to make sure there is enough LDS space to account for those ES verts
      // beyond ES_VERTS_PER_SUBGRP.
      esVertsPerSubgroup -= (esMinVertsPerSubgroup - 1);

      gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize = esGsLdsSize;
      gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = gsOnChipLdsSize;

      gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize = esGsRingItemSize;
      gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize = gsOnChip ? gsVsRingItemSizeOnChip : gsVsRingItemSize;

      if (m_pipelineState->getTargetInfo().getGfxIpVersion().major == 10 && hasTs && !gsOnChip) {
        unsigned esVertsNum = Gfx9::EsVertsOffchipGsOrTess;
        unsigned onChipGsLdsMagicSize =
            alignTo((esVertsNum * esGsRingItemSize) + esGsExtraLdsDwords,
                    static_cast<unsigned>(
                        (1 << m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift)));

        // If the new size is greater than the size we previously set
        // then we need to either increase the size or decrease the verts
        if (onChipGsLdsMagicSize > gsOnChipLdsSize) {
          if (onChipGsLdsMagicSize > maxLdsSize) {
            // Decrease the verts
            esVertsNum = (maxLdsSize - esGsExtraLdsDwords) / esGsRingItemSize;
            gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = maxLdsSize;
          } else {
            // Increase the size
            gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = onChipGsLdsMagicSize;
          }
        }
        // Support multiple GS instances
        unsigned gsPrimsNum = Gfx9::GsPrimsOffchipGsOrTess / gsInstanceCount;

        gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsNum;
        gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsNum;
      }
    }
  }

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC geometry calculation factor results\n\n");
  LLPC_OUTS("ES vertices per sub-group: " << gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup << "\n");
  LLPC_OUTS("GS primitives per sub-group: " << gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup << "\n");
  LLPC_OUTS("\n");
  LLPC_OUTS("ES-GS LDS size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize << "\n");
  LLPC_OUTS("On-chip GS LDS size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize << "\n");
  LLPC_OUTS("\n");
  LLPC_OUTS("ES-GS ring item size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize << "\n");
  LLPC_OUTS("GS-VS ring item size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize << "\n");
  LLPC_OUTS("\n");

  LLPC_OUTS("GS stream item size:\n");
  for (unsigned i = 0; i < MaxGsStreams; ++i) {
    unsigned streamItemSize = gsResUsage->inOutUsage.gs.outLocCount[i] * geometryMode.outputVertices * 4;
    LLPC_OUTS("    stream " << i << " = " << streamItemSize);

    if (gsResUsage->inOutUsage.enableXfb) {
      LLPC_OUTS(", XFB buffer = ");
      for (unsigned j = 0; j < MaxTransformFeedbackBuffers; ++j) {
        if ((gsResUsage->inOutUsage.streamXfbBuffers[i] & (1 << j)) != 0) {
          LLPC_OUTS(j);
          if (j != MaxTransformFeedbackBuffers - 1)
            LLPC_OUTS(", ");
        }
      }
    }

    LLPC_OUTS("\n");
  }
  LLPC_OUTS("\n");

  if (gsOnChip || m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
    if (m_pipelineState->getNggControl()->enableNgg) {
      LLPC_OUTS("GS primitive amplification factor: " << gsResUsage->inOutUsage.gs.calcFactor.primAmpFactor << "\n");
      LLPC_OUTS("GS enable max output vertices per instance: "
                << (gsResUsage->inOutUsage.gs.calcFactor.enableMaxVertOut ? "true" : "false") << "\n");
      LLPC_OUTS("\n");

      LLPC_OUTS("GS is on-chip (NGG)\n");
    } else
      LLPC_OUTS("GS is " << (gsOnChip ? "on-chip" : "off-chip") << "\n");
  } else
    LLPC_OUTS("GS is off-chip\n");
  LLPC_OUTS("\n");

  return gsOnChip;
}

// =====================================================================================================================
// Gets the count of vertices per primitive
unsigned PatchResourceCollect::getVerticesPerPrimitive() const {
  unsigned vertsPerPrim = 1;

  switch (m_pipelineState->getInputAssemblyState().topology) {
  case PrimitiveTopology::PointList:
    vertsPerPrim = 1;
    break;
  case PrimitiveTopology::LineList:
    vertsPerPrim = 2;
    break;
  case PrimitiveTopology::LineStrip:
    vertsPerPrim = 2;
    break;
  case PrimitiveTopology::TriangleList:
    vertsPerPrim = 3;
    break;
  case PrimitiveTopology::TriangleStrip:
    vertsPerPrim = 3;
    break;
  case PrimitiveTopology::TriangleFan:
    vertsPerPrim = 3;
    break;
  case PrimitiveTopology::LineListWithAdjacency:
    vertsPerPrim = 4;
    break;
  case PrimitiveTopology::LineStripWithAdjacency:
    vertsPerPrim = 4;
    break;
  case PrimitiveTopology::TriangleListWithAdjacency:
    vertsPerPrim = 6;
    break;
  case PrimitiveTopology::TriangleStripWithAdjacency:
    vertsPerPrim = 6;
    break;
  case PrimitiveTopology::PatchList:
    vertsPerPrim = m_pipelineState->getInputAssemblyState().patchControlPoints;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  return vertsPerPrim;
}

// =====================================================================================================================
// Process a single shader
void PatchResourceCollect::processShader() {
  m_hasDynIndexedInput = false;
  m_hasDynIndexedOutput = false;
  m_resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  // Invoke handling of "call" instruction
  visit(m_entryPoint);

  clearInactiveInput();
  clearInactiveOutput();

  if (m_pipelineState->isGraphics()) {
    matchGenericInOut();
    mapBuiltInToGenericInOut();
  }

  if (m_shaderStage == ShaderStageFragment) {
    if (m_resUsage->builtInUsage.fs.fragCoord || m_resUsage->builtInUsage.fs.pointCoord ||
        m_resUsage->builtInUsage.fs.sampleMaskIn) {
      if (m_pipelineState->getRasterizerState().perSampleShading)
        m_resUsage->builtInUsage.fs.runAtSampleRate = true;
    }
  } else if (m_shaderStage == ShaderStageVertex) {
    // Collect resource usages from vertex input create info
    // TODO: In the future, we might check if the corresponding vertex attribute is active in vertex shader
    // and set the usage based on this info.
    for (const auto &vertexInput : m_pipelineState->getVertexInputDescriptions()) {
      if (vertexInput.inputRate == VertexInputRateVertex) {
        m_resUsage->builtInUsage.vs.vertexIndex = true;
        m_resUsage->builtInUsage.vs.baseVertex = true;
      } else {
        // TODO: We probably don't need instanceIndex for VertexInputRateNone.
        m_resUsage->builtInUsage.vs.instanceIndex = true;
        m_resUsage->builtInUsage.vs.baseInstance = true;
      }
    }
  }

  // Remove dead calls
  for (auto call : m_deadCalls) {
    assert(call->user_empty());
    call->dropAllReferences();
    call->eraseFromParent();
  }
  m_deadCalls.clear();
}

// =====================================================================================================================
// Check whether vertex reuse should be disabled.
bool PatchResourceCollect::isVertexReuseDisabled() {
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
  const bool hasTs =
      (m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval));
  const bool hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);

  bool disableVertexReuse = m_pipelineState->getInputAssemblyState().disableVertexReuse;

  bool useViewportIndex = false;
  if (hasGs)
    useViewportIndex = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->builtInUsage.gs.viewportIndex;
  else if (hasTs) {
    useViewportIndex = m_pipelineState->getShaderResourceUsage(ShaderStageTessEval)->builtInUsage.tes.viewportIndex;
  } else if (hasVs)
    useViewportIndex = m_pipelineState->getShaderResourceUsage(ShaderStageVertex)->builtInUsage.vs.viewportIndex;

  disableVertexReuse |= useViewportIndex;

  return disableVertexReuse;
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void PatchResourceCollect::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  bool isDeadCall = callInst.user_empty();

  auto mangledName = callee->getName();

  if (mangledName.startswith(lgcName::InputImportGeneric)) {
    // Generic input import
    if (isDeadCall)
      m_deadCalls.push_back(&callInst);
    else {
      auto inputTy = callInst.getType();
      assert(inputTy->isSingleValueType());

      auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();

      if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval) {
        auto locOffset = callInst.getOperand(1);
        auto compIdx = callInst.getOperand(2);

        if (isa<ConstantInt>(locOffset)) {
          // Location offset is constant
          loc += cast<ConstantInt>(locOffset)->getZExtValue();

          auto bitWidth = inputTy->getScalarSizeInBits();
          if (bitWidth == 64) {
            if (isa<ConstantInt>(compIdx)) {

              m_activeInputLocs.insert(loc);
              if (cast<ConstantInt>(compIdx)->getZExtValue() >= 2) {
                // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
                // occupied locations are two.
                m_activeInputLocs.insert(loc + 1);
              }
            } else {
              // NOTE: If vector component index is not constant, we treat this as dynamic indexing.
              m_hasDynIndexedInput = true;
            }
          } else {
            // NOTE: For non 64-bit vector/scalar, one location is sufficient regardless of vector component
            // addressing.
            assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);
            m_activeInputLocs.insert(loc);
          }
        } else {
          // NOTE: If location offset is not constant, we treat this as dynamic indexing.
          m_hasDynIndexedInput = true;
        }
      } else {
        m_activeInputLocs.insert(loc);
        if (inputTy->getPrimitiveSizeInBits() > (8 * SizeOfVec4)) {
          assert(inputTy->getPrimitiveSizeInBits() <= (8 * 2 * SizeOfVec4));
          m_activeInputLocs.insert(loc + 1);
        }
      }
    }
  } else if (mangledName.startswith(lgcName::InputImportInterpolant)) {
    // Interpolant input import
    assert(m_shaderStage == ShaderStageFragment);

    if (isDeadCall)
      m_deadCalls.push_back(&callInst);
    else {
      assert(callInst.getType()->isSingleValueType());

      auto locOffset = callInst.getOperand(1);
      if (isa<ConstantInt>(locOffset)) {
        // Location offset is constant
        auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        loc += cast<ConstantInt>(locOffset)->getZExtValue();

        assert(callInst.getType()->getPrimitiveSizeInBits() <= (8 * SizeOfVec4));
        m_activeInputLocs.insert(loc);
      } else {
        // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
        m_hasDynIndexedInput = true;
      }
    }
  } else if (mangledName.startswith(lgcName::InputImportBuiltIn)) {
    // Built-in input import
    if (isDeadCall)
      m_deadCalls.push_back(&callInst);
    else {
      unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
      m_activeInputBuiltIns.insert(builtInId);
    }
  } else if (mangledName.startswith(lgcName::OutputImportGeneric)) {
    // Generic output import
    assert(m_shaderStage == ShaderStageTessControl);

    auto outputTy = callInst.getType();
    assert(outputTy->isSingleValueType());

    auto loc = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
    auto locOffset = callInst.getOperand(1);
    auto compIdx = callInst.getOperand(2);

    if (isa<ConstantInt>(locOffset)) {
      // Location offset is constant
      loc += cast<ConstantInt>(locOffset)->getZExtValue();

      auto bitWidth = outputTy->getScalarSizeInBits();
      if (bitWidth == 64) {
        if (isa<ConstantInt>(compIdx)) {
          m_importedOutputLocs.insert(loc);
          if (cast<ConstantInt>(compIdx)->getZExtValue() >= 2) {
            // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
            // occupied locations are two.
            m_importedOutputLocs.insert(loc + 1);
          }
        } else {
          // NOTE: If vector component index is not constant, we treat this as dynamic indexing.
          m_hasDynIndexedOutput = true;
        }
      } else {
        // NOTE: For non 64-bit vector/scalar, one location is sufficient regardless of vector component
        // addressing.
        assert(bitWidth == 8 || bitWidth == 16 || bitWidth == 32);
        m_importedOutputLocs.insert(loc);
      }
    } else {
      // NOTE: If location offset is not constant, we treat this as dynamic indexing.
      m_hasDynIndexedOutput = true;
    }
  } else if (mangledName.startswith(lgcName::OutputImportBuiltIn)) {
    // Built-in output import
    assert(m_shaderStage == ShaderStageTessControl);

    unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
    m_importedOutputBuiltIns.insert(builtInId);
  } else if (mangledName.startswith(lgcName::OutputExportGeneric)) {
    // Generic output export
    if (m_shaderStage == ShaderStageTessControl) {
      auto output = callInst.getOperand(callInst.getNumArgOperands() - 1);
      auto outputTy = output->getType();
      assert(outputTy->isSingleValueType());

      auto locOffset = callInst.getOperand(1);
      auto compIdx = callInst.getOperand(2);

      if (isa<ConstantInt>(locOffset)) {
        // Location offset is constant
        auto bitWidth = outputTy->getScalarSizeInBits();
        if (bitWidth == 64 && !isa<ConstantInt>(compIdx)) {
          // NOTE: If vector component index is not constant and it is vector component addressing for
          // 64-bit vector, we treat this as dynamic indexing.
          m_hasDynIndexedOutput = true;
        }
      } else {
        // NOTE: If location offset is not constant, we consider dynamic indexing occurs.
        m_hasDynIndexedOutput = true;
      }
    }
  } else if (mangledName.startswith(lgcName::OutputExportBuiltIn)) {
    // NOTE: If output value is undefined one, we can safely drop it and remove the output export call.
    // Currently, do this for geometry shader.
    if (m_shaderStage == ShaderStageGeometry) {
      auto *outputValue = callInst.getArgOperand(callInst.getNumArgOperands() - 1);
      if (isa<UndefValue>(outputValue))
        m_deadCalls.push_back(&callInst);
      else {
        unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        m_activeOutputBuiltIns.insert(builtInId);
      }
    }
  }

  if (m_pipelineState->canPackInOut()) {
    // Process input import calls with constant location offset in FS (VS-FS, TES-FS) or TCS (VS-TCS)
    // Collect output export calls to re-assemble in VS (VS-FS) or TES (TES-FS)
    const bool isPackIn = m_shaderStage == ShaderStageFragment || m_shaderStage == ShaderStageTessControl;
    const bool isPackOut = m_pipelineState->getNextShaderStage(m_shaderStage) == ShaderStageFragment &&
                           (m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval);

    if (isPackIn && !m_hasDynIndexedInput && !isDeadCall &&
        (mangledName.startswith(lgcName::InputImportGeneric) ||
         mangledName.startswith(lgcName::InputImportInterpolant))) {
      // Collect LocationSpans according to each TCS or FS input call
      m_locationMapManager->addSpan(&callInst, m_shaderStage);
      m_inOutCalls.push_back(&callInst);
    } else if (isPackOut && mangledName.startswith(lgcName::OutputExportGeneric)) {
      // Collect outputs of VS or TES
      m_inOutCalls.push_back(&callInst);
      m_deadCalls.push_back(&callInst);
    }
  }
}

// =====================================================================================================================
// Clears inactive (those actually unused) inputs.
void PatchResourceCollect::clearInactiveInput() {
  // Clear those inactive generic inputs, remove them from location mappings
  if (m_pipelineState->isGraphics() && !m_hasDynIndexedInput && m_shaderStage != ShaderStageTessEval &&
      !m_pipelineState->isUnlinked()) {
    // TODO: Here, we keep all generic inputs of tessellation evaluation shader. This is because corresponding
    // generic outputs of tessellation control shader might involve in output import and dynamic indexing, which
    // is easy to cause incorrectness of location mapping.

    // Clear normal inputs
    std::set<InOutLocationInfo> unusedLocInfos;
    for (const auto &locInfoPair : m_resUsage->inOutUsage.inputLocInfoMap) {
      unsigned loc = locInfoPair.first.getLocation();
      if (m_activeInputLocs.find(loc) == m_activeInputLocs.end())
        unusedLocInfos.insert(locInfoPair.first);
    }

    for (auto locInfo : unusedLocInfos)
      m_resUsage->inOutUsage.inputLocInfoMap.erase(locInfo);

    // Clear per-patch inputs
    if (m_shaderStage == ShaderStageTessEval) {
      std::unordered_set<unsigned> unusedLocs;
      for (auto locMap : m_resUsage->inOutUsage.perPatchInputLocMap) {
        unsigned loc = locMap.first;
        if (m_activeInputLocs.find(loc) == m_activeInputLocs.end())
          unusedLocs.insert(loc);
      }

      for (auto loc : unusedLocs)
        m_resUsage->inOutUsage.perPatchInputLocMap.erase(loc);
    } else {
      // For other stages, must be empty
      assert(m_resUsage->inOutUsage.perPatchInputLocMap.empty());
    }
  }

  // Clear those inactive built-in inputs (some are not checked, whose usage flags do not rely on their
  // actual uses)
  auto &builtInUsage = m_resUsage->builtInUsage;

  // Check per-stage built-in usage
  if (m_shaderStage == ShaderStageTessControl) {
    if (builtInUsage.tcs.pointSizeIn && m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.pointSizeIn = false;

    if (builtInUsage.tcs.positionIn && m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.positionIn = false;

    if (builtInUsage.tcs.clipDistanceIn > 0 &&
        m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.clipDistanceIn = 0;

    if (builtInUsage.tcs.cullDistanceIn > 0 &&
        m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.cullDistanceIn = 0;

    if (builtInUsage.tcs.patchVertices &&
        m_activeInputBuiltIns.find(BuiltInPatchVertices) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.patchVertices = false;

    if (builtInUsage.tcs.primitiveId && m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.primitiveId = false;

    if (builtInUsage.tcs.invocationId && m_activeInputBuiltIns.find(BuiltInInvocationId) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.invocationId = false;
  } else if (m_shaderStage == ShaderStageTessEval) {
    if (builtInUsage.tes.pointSizeIn && m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end())
      builtInUsage.tes.pointSizeIn = false;

    if (builtInUsage.tes.positionIn && m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end())
      builtInUsage.tes.positionIn = false;

    if (builtInUsage.tes.clipDistanceIn > 0 &&
        m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end())
      builtInUsage.tes.clipDistanceIn = 0;

    if (builtInUsage.tes.cullDistanceIn > 0 &&
        m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end())
      builtInUsage.tes.cullDistanceIn = 0;

    if (builtInUsage.tes.patchVertices &&
        m_activeInputBuiltIns.find(BuiltInPatchVertices) == m_activeInputBuiltIns.end())
      builtInUsage.tes.patchVertices = false;

    if (builtInUsage.tes.primitiveId && m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end())
      builtInUsage.tes.primitiveId = false;

    if (builtInUsage.tes.tessCoord && m_activeInputBuiltIns.find(BuiltInTessCoord) == m_activeInputBuiltIns.end())
      builtInUsage.tes.tessCoord = false;

    if (builtInUsage.tes.tessLevelOuter &&
        m_activeInputBuiltIns.find(BuiltInTessLevelOuter) == m_activeInputBuiltIns.end())
      builtInUsage.tes.tessLevelOuter = false;

    if (builtInUsage.tes.tessLevelInner &&
        m_activeInputBuiltIns.find(BuiltInTessLevelInner) == m_activeInputBuiltIns.end())
      builtInUsage.tes.tessLevelInner = false;
  } else if (m_shaderStage == ShaderStageGeometry) {
    if (builtInUsage.gs.pointSizeIn && m_activeInputBuiltIns.find(BuiltInPointSize) == m_activeInputBuiltIns.end())
      builtInUsage.gs.pointSizeIn = false;

    if (builtInUsage.gs.positionIn && m_activeInputBuiltIns.find(BuiltInPosition) == m_activeInputBuiltIns.end())
      builtInUsage.gs.positionIn = false;

    if (builtInUsage.gs.clipDistanceIn > 0 &&
        m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end())
      builtInUsage.gs.clipDistanceIn = 0;

    if (builtInUsage.gs.cullDistanceIn > 0 &&
        m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end())
      builtInUsage.gs.cullDistanceIn = 0;

    if (builtInUsage.gs.primitiveIdIn && m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end())
      builtInUsage.gs.primitiveIdIn = false;

    if (builtInUsage.gs.invocationId && m_activeInputBuiltIns.find(BuiltInInvocationId) == m_activeInputBuiltIns.end())
      builtInUsage.gs.invocationId = false;
  } else if (m_shaderStage == ShaderStageFragment) {
    if (builtInUsage.fs.fragCoord && m_activeInputBuiltIns.find(BuiltInFragCoord) == m_activeInputBuiltIns.end())
      builtInUsage.fs.fragCoord = false;

    if (builtInUsage.fs.frontFacing && m_activeInputBuiltIns.find(BuiltInFrontFacing) == m_activeInputBuiltIns.end())
      builtInUsage.fs.frontFacing = false;

    if (builtInUsage.fs.fragCoord && m_activeInputBuiltIns.find(BuiltInFragCoord) == m_activeInputBuiltIns.end())
      builtInUsage.fs.fragCoord = false;

    if (builtInUsage.fs.clipDistance > 0 &&
        m_activeInputBuiltIns.find(BuiltInClipDistance) == m_activeInputBuiltIns.end())
      builtInUsage.fs.clipDistance = 0;

    if (builtInUsage.fs.cullDistance > 0 &&
        m_activeInputBuiltIns.find(BuiltInCullDistance) == m_activeInputBuiltIns.end())
      builtInUsage.fs.cullDistance = 0;

    if (builtInUsage.fs.pointCoord && m_activeInputBuiltIns.find(BuiltInPointCoord) == m_activeInputBuiltIns.end())
      builtInUsage.fs.pointCoord = false;

    if (builtInUsage.fs.primitiveId && m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end())
      builtInUsage.fs.primitiveId = false;

    if (builtInUsage.fs.sampleId && m_activeInputBuiltIns.find(BuiltInSampleId) == m_activeInputBuiltIns.end())
      builtInUsage.fs.sampleId = false;

    if (builtInUsage.fs.samplePosition &&
        m_activeInputBuiltIns.find(BuiltInSamplePosition) == m_activeInputBuiltIns.end())
      builtInUsage.fs.samplePosition = false;

    if (builtInUsage.fs.sampleMaskIn && m_activeInputBuiltIns.find(BuiltInSampleMask) == m_activeInputBuiltIns.end())
      builtInUsage.fs.sampleMaskIn = false;

    if (builtInUsage.fs.layer && m_activeInputBuiltIns.find(BuiltInLayer) == m_activeInputBuiltIns.end())
      builtInUsage.fs.layer = false;

    if (builtInUsage.fs.viewIndex && m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end())
      builtInUsage.fs.viewIndex = false;

    if (builtInUsage.fs.viewportIndex &&
        m_activeInputBuiltIns.find(BuiltInViewportIndex) == m_activeInputBuiltIns.end())
      builtInUsage.fs.viewportIndex = false;

    if (builtInUsage.fs.helperInvocation &&
        m_activeInputBuiltIns.find(BuiltInHelperInvocation) == m_activeInputBuiltIns.end())
      builtInUsage.fs.helperInvocation = false;

    if (builtInUsage.fs.shadingRate &&
        m_activeInputBuiltIns.find(BuiltInShadingRate) == m_activeInputBuiltIns.end())
      builtInUsage.fs.shadingRate = false;

    if (builtInUsage.fs.baryCoordNoPersp &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordNoPersp) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordNoPersp = false;

    if (builtInUsage.fs.baryCoordNoPerspCentroid &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspCentroid) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordNoPerspCentroid = false;

    if (builtInUsage.fs.baryCoordNoPerspSample &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspSample) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordNoPerspSample = false;

    if (builtInUsage.fs.baryCoordSmooth &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordSmooth) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordSmooth = false;

    if (builtInUsage.fs.baryCoordSmoothCentroid &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothCentroid) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordSmoothCentroid = false;

    if (builtInUsage.fs.baryCoordSmoothSample &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordSmoothSample) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordNoPerspSample = false;

    if (builtInUsage.fs.baryCoordPullModel &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordPullModel) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordPullModel = false;
  }
}

// =====================================================================================================================
// Clears inactive (those actually unused) outputs.
void PatchResourceCollect::clearInactiveOutput() {
  // Clear inactive output builtins
  if (m_shaderStage == ShaderStageGeometry) {
    auto &builtInUsage = m_resUsage->builtInUsage.gs;

    if (builtInUsage.position && m_activeOutputBuiltIns.find(BuiltInPosition) == m_activeOutputBuiltIns.end())
      builtInUsage.position = false;

    if (builtInUsage.pointSize && m_activeOutputBuiltIns.find(BuiltInPointSize) == m_activeOutputBuiltIns.end())
      builtInUsage.pointSize = false;

    if (builtInUsage.clipDistance && m_activeOutputBuiltIns.find(BuiltInClipDistance) == m_activeOutputBuiltIns.end())
      builtInUsage.clipDistance = false;

    if (builtInUsage.cullDistance && m_activeOutputBuiltIns.find(BuiltInCullDistance) == m_activeOutputBuiltIns.end())
      builtInUsage.cullDistance = false;

    if (builtInUsage.primitiveId && m_activeOutputBuiltIns.find(BuiltInPrimitiveId) == m_activeOutputBuiltIns.end())
      builtInUsage.primitiveId = false;

    if (builtInUsage.layer && m_activeOutputBuiltIns.find(BuiltInLayer) == m_activeOutputBuiltIns.end())
      builtInUsage.layer = false;

    if (builtInUsage.viewportIndex && m_activeOutputBuiltIns.find(BuiltInViewportIndex) == m_activeOutputBuiltIns.end())
      builtInUsage.viewportIndex = false;

    if (builtInUsage.primitiveShadingRate && m_activeOutputBuiltIns.find(BuiltInPrimitiveShadingRate) == m_activeOutputBuiltIns.end())
      builtInUsage.primitiveShadingRate = false;
  }
}

// =====================================================================================================================
// Does generic input/output matching and does location mapping afterwards.
//
// NOTE: This function should be called after the cleanup work of inactive inputs is done.
void PatchResourceCollect::matchGenericInOut() {
  assert(m_pipelineState->isGraphics());
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;

  auto &inLocInfoMap = inOutUsage.inputLocInfoMap;
  auto &outLocInfoMap = inOutUsage.outputLocInfoMap;

  auto &perPatchInLocMap = inOutUsage.perPatchInputLocMap;
  auto &perPatchOutLocMap = inOutUsage.perPatchOutputLocMap;

  // Do input/output matching
  if (!m_pipelineState->isUnlinked() && m_shaderStage != ShaderStageFragment) {
    const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);

    // Do normal input/output matching
    if (nextStage != ShaderStageInvalid) {
      const auto nextResUsage = m_pipelineState->getShaderResourceUsage(nextStage);
      const auto &nextInLocInfoMap = nextResUsage->inOutUsage.inputLocInfoMap;

      unsigned availInMapLoc = nextResUsage->inOutUsage.inputMapLocCount;

      // Collect locations of those outputs that are not used by next shader stage
      std::vector<InOutLocationInfo> unusedLocInfos;
      for (auto &locInfoPair : outLocInfoMap) {
        unsigned loc = locInfoPair.first.getLocation();
        bool outputXfb = false;
        if (m_shaderStage == ShaderStageGeometry)
          outputXfb = inOutUsage.gs.xfbOutsInfo.find(locInfoPair.first) != inOutUsage.gs.xfbOutsInfo.end();

        if (nextInLocInfoMap.find(locInfoPair.first) == nextInLocInfoMap.end() && !outputXfb) {
          if (m_hasDynIndexedOutput || m_importedOutputLocs.find(loc) != m_importedOutputLocs.end()) {
            // NOTE: If either dynamic indexing of generic outputs exists or the generic output involve in
            // output import, we have to mark it as active. The assigned location must not overlap with
            // those used by inputs of next shader stage.
            assert(m_shaderStage == ShaderStageTessControl);
            auto &newLocationInfo = locInfoPair.second;
            newLocationInfo.setData(0);
            newLocationInfo.setLocation(availInMapLoc++);
          } else
            unusedLocInfos.push_back(locInfoPair.first);
        }
      }

      // Remove those collected locations
      for (auto locInfo : unusedLocInfos)
        outLocInfoMap.erase(locInfo);
    }

    // Do per-patch input/output matching
    if (m_shaderStage == ShaderStageTessControl) {
      if (nextStage != ShaderStageInvalid) {
        const auto nextResUsage = m_pipelineState->getShaderResourceUsage(nextStage);
        const auto &nextPerPatchInLocMap = nextResUsage->inOutUsage.perPatchInputLocMap;

        unsigned availPerPatchInMapLoc = nextResUsage->inOutUsage.perPatchInputMapLocCount;

        // Collect locations of those outputs that are not used by next shader stage
        std::vector<unsigned> unusedLocs;
        for (auto &locMap : perPatchOutLocMap) {
          const unsigned loc = locMap.first;
          if (nextPerPatchInLocMap.find(loc) == nextPerPatchInLocMap.end()) {
            // NOTE: If either dynamic indexing of generic outputs exists or the generic output involve in
            // output import, we have to mark it as active. The assigned location must not overlap with
            // those used by inputs of next shader stage.
            if (m_hasDynIndexedOutput || m_importedOutputLocs.find(loc) != m_importedOutputLocs.end()) {
              assert(m_shaderStage == ShaderStageTessControl);
              locMap.second = availPerPatchInMapLoc++;
            } else
              unusedLocs.push_back(loc);
          }
        }

        // Remove those collected locations
        for (auto loc : unusedLocs)
          perPatchOutLocMap.erase(loc);
      }
    } else {
      // For other stages, must be empty
      assert(perPatchOutLocMap.empty());
    }
  }

  if (m_pipelineState->canPackInOut()) {
    // Do packing input/output
    packInOutLocation();
  }

  // Do location mapping
  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC location input/output mapping results (" << getShaderStageAbbreviation(m_shaderStage)
                                                              << " shader)\n\n");
  unsigned nextMapLoc = 0;
  if (!inLocInfoMap.empty()) {
    assert(inOutUsage.inputMapLocCount == 0);
    for (auto &locInfoPair : inLocInfoMap) {

      auto &newLocationInfo = locInfoPair.second;
      if (m_shaderStage == ShaderStageVertex) {
        // NOTE: For vertex shader, use the orignal location as the remapped location
        newLocationInfo = locInfoPair.first;
      } else if (newLocationInfo.isInvalid() || m_pipelineState->isUnlinked()) {
        // For other shaders, map the location to continous locations if they are not mapped or in un-linked mode
        newLocationInfo.setData(0);
        newLocationInfo.setLocation(nextMapLoc++);
      }
      const unsigned newLocation = locInfoPair.second.getLocation();

      inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, newLocation + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  loc = "
                    << locInfoPair.first.getLocation() << "  =>  Mapped = " << newLocation << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!outLocInfoMap.empty()) {
    auto &outOrigLocs = inOutUsage.fs.outputOrigLocs;
    if (m_shaderStage == ShaderStageFragment)
      memset(&outOrigLocs, InvalidValue, sizeof(inOutUsage.fs.outputOrigLocs));

    nextMapLoc = 0;
    assert(inOutUsage.outputMapLocCount == 0);
    bool generatingColorExportShader = m_shaderStage == ShaderStageFragment;
    generatingColorExportShader &= m_pipelineState->isUnlinked() && !m_pipelineState->hasColorExportFormats();

    for (auto locInfoMapIt = outLocInfoMap.begin(); locInfoMapIt != outLocInfoMap.end();) {
      const unsigned origLocation = locInfoMapIt->first.getLocation();
      auto &newLocationInfo = locInfoMapIt->second;

      if (m_shaderStage == ShaderStageFragment) {
        if (!generatingColorExportShader &&
            m_pipelineState->getColorExportFormat(origLocation).dfmt == BufDataFormatInvalid) {
          locInfoMapIt = outLocInfoMap.erase(locInfoMapIt);
          continue;
        }
      }

      if (m_shaderStage == ShaderStageGeometry) {
        if (newLocationInfo.isInvalid()) {
          // TODO: pack GS outputs
          const unsigned streamId = locInfoMapIt->first.getStreamId();
          newLocationInfo.setData(0);
          newLocationInfo.setLocation(inOutUsage.gs.outLocCount[streamId]++);
          newLocationInfo.setStreamId(streamId);

          unsigned assignedLocCount = inOutUsage.gs.outLocCount[0] + inOutUsage.gs.outLocCount[1] +
                                      inOutUsage.gs.outLocCount[2] + inOutUsage.gs.outLocCount[3];

          inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, assignedLocCount);
          LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: stream = " << streamId << ", "
                        << " loc = " << origLocation << "  =>  Mapped = " << newLocationInfo.getLocation() << "\n");
        }
      } else {
        if (newLocationInfo.isInvalid()) {
          newLocationInfo.setData(0);
          newLocationInfo.setLocation(nextMapLoc++);
        }
        const unsigned newLocation = newLocationInfo.getLocation();

        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, newLocation + 1);
        LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: loc = " << origLocation
                      << "  =>  Mapped = " << newLocation << "\n");

        if (m_shaderStage == ShaderStageFragment)
          outOrigLocs[newLocation] = origLocation;
      }

      ++locInfoMapIt;
    }
    LLPC_OUTS("\n");
  }

  if (!perPatchInLocMap.empty()) {
    nextMapLoc = 0;
    assert(inOutUsage.perPatchInputMapLocCount == 0);
    for (auto &locMap : perPatchInLocMap) {
      assert(locMap.second == InvalidValue);
      locMap.second = nextMapLoc++;
      inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, locMap.second + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input (per-patch):  loc = " << locMap.first
                    << "  =>  Mapped = " << locMap.second << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!perPatchOutLocMap.empty()) {
    nextMapLoc = 0;
    assert(inOutUsage.perPatchOutputMapLocCount == 0);
    for (auto &locMap : perPatchOutLocMap) {
      if (locMap.second == InvalidValue) {
        // Only do location mapping if the per-patch output has not been mapped
        locMap.second = nextMapLoc++;
      } else
        assert(m_shaderStage == ShaderStageTessControl);
      inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, locMap.second + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output (per-patch): loc = " << locMap.first
                    << "  =>  Mapped = " << locMap.second << "\n");
    }
    LLPC_OUTS("\n");
  }

  LLPC_OUTS("// LLPC location count results (after input/output matching) \n\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  loc count = " << inOutUsage.inputMapLocCount
                << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: loc count = " << inOutUsage.outputMapLocCount
                << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                << ") Input (per-patch):  loc count = " << inOutUsage.perPatchInputMapLocCount << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                << ") Output (per-patch): loc count = " << inOutUsage.perPatchOutputMapLocCount << "\n");
  LLPC_OUTS("\n");
}

// =====================================================================================================================
// Maps special built-in input/output to generic ones.
//
// NOTE: This function should be called after generic input/output matching is done.
void PatchResourceCollect::mapBuiltInToGenericInOut() {
  assert(m_pipelineState->isGraphics());

  const auto resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  auto &builtInUsage = resUsage->builtInUsage;
  auto &inOutUsage = resUsage->inOutUsage;

  const auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);
  auto nextResUsage = nextStage != ShaderStageInvalid ? m_pipelineState->getShaderResourceUsage(nextStage) : nullptr;

  assert(inOutUsage.builtInInputLocMap.empty()); // Should be empty
  assert(inOutUsage.builtInOutputLocMap.empty());

  // NOTE: The rules of mapping built-ins to generic inputs/outputs are as follow:
  //       (1) For built-in outputs, if next shader stager is valid and has corresponding built-in input used,
  //           get the mapped location from next shader stage inout usage and use it. If next shader stage
  //           is absent or it does not have such input used, we allocate the mapped location.
  //       (2) For built-on inputs, we always allocate the mapped location based its actual usage.
  if (m_shaderStage == ShaderStageVertex) {
    // VS  ==>  XXX
    unsigned availOutMapLoc = inOutUsage.outputMapLocCount;

    // Map built-in outputs to generic ones
    if (nextStage == ShaderStageFragment) {
      // VS  ==>  FS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.fs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.clipDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
      }

      if (nextBuiltInUsage.cullDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
      }

      if (nextBuiltInUsage.primitiveId) {
        // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
        builtInUsage.vs.primitiveId = true;

        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
        inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
      }

      if (nextBuiltInUsage.layer) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
      }

      if (nextBuiltInUsage.viewIndex) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
      }

      if (nextBuiltInUsage.viewportIndex) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
      }
    } else if (nextStage == ShaderStageTessControl) {
      // VS  ==>  TCS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.tcs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.positionIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else
        builtInUsage.vs.position = false;

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else
        builtInUsage.vs.pointSize = false;

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else
        builtInUsage.vs.clipDistance = 0;

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else
        builtInUsage.vs.cullDistance = 0;

      builtInUsage.vs.layer = false;
      builtInUsage.vs.viewportIndex = false;
      builtInUsage.vs.primitiveShadingRate = false;
    } else if (nextStage == ShaderStageGeometry) {
      // VS  ==>  GS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.gs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.positionIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else
        builtInUsage.vs.position = false;

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else
        builtInUsage.vs.pointSize = false;

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else
        builtInUsage.vs.clipDistance = 0;

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else
        builtInUsage.vs.cullDistance = 0;

      builtInUsage.vs.layer = false;
      builtInUsage.vs.viewportIndex = false;
      builtInUsage.vs.primitiveShadingRate = false;
    } else if (nextStage == ShaderStageInvalid) {
      // VS only
      if (builtInUsage.vs.clipDistance > 0 || builtInUsage.vs.cullDistance > 0) {
        unsigned mapLoc = availOutMapLoc++;
        if (builtInUsage.vs.clipDistance + builtInUsage.vs.cullDistance > 4) {
          assert(builtInUsage.vs.clipDistance + builtInUsage.vs.cullDistance <= MaxClipCullDistanceCount);
          ++availOutMapLoc; // Occupy two locations
        }

        if (builtInUsage.vs.clipDistance > 0)
          inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;

        if (builtInUsage.vs.cullDistance > 0) {
          if (builtInUsage.vs.clipDistance >= 4)
            ++mapLoc;
          inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        }
      }

      if (builtInUsage.vs.viewportIndex)
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;

      if (builtInUsage.vs.layer)
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;

      if (builtInUsage.vs.viewIndex)
        inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
    }

    inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
  } else if (m_shaderStage == ShaderStageTessControl) {
    // TCS  ==>  XXX
    unsigned availInMapLoc = inOutUsage.inputMapLocCount;
    unsigned availOutMapLoc = inOutUsage.outputMapLocCount;

    unsigned availPerPatchOutMapLoc = inOutUsage.perPatchOutputMapLocCount;

    // Map built-in inputs to generic ones
    if (builtInUsage.tcs.positionIn)
      inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;

    if (builtInUsage.tcs.pointSizeIn)
      inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;

    if (builtInUsage.tcs.clipDistanceIn > 0) {
      inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
      if (builtInUsage.tcs.clipDistanceIn > 4)
        ++availInMapLoc;
    }

    if (builtInUsage.tcs.cullDistanceIn > 0) {
      inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
      if (builtInUsage.tcs.cullDistanceIn > 4)
        ++availInMapLoc;
    }

    // Map built-in outputs to generic ones
    if (nextStage == ShaderStageTessEval) {
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.tes;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      // NOTE: For tessellation control shadder, those built-in outputs that involve in output import have to
      // be mapped to generic ones even if they do not have corresponding built-in inputs used in next shader
      // stage.
      if (nextBuiltInUsage.positionIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        if (m_importedOutputBuiltIns.find(BuiltInPosition) != m_importedOutputBuiltIns.end())
          inOutUsage.builtInOutputLocMap[BuiltInPosition] = InvalidValue;
        else
          builtInUsage.tcs.position = false;
      }

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        if (m_importedOutputBuiltIns.find(BuiltInPointSize) != m_importedOutputBuiltIns.end())
          inOutUsage.builtInOutputLocMap[BuiltInPointSize] = InvalidValue;
        else
          builtInUsage.tcs.pointSize = false;
      }

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else {
        if (m_importedOutputBuiltIns.find(BuiltInClipDistance) != m_importedOutputBuiltIns.end())
          inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = InvalidValue;
        else
          builtInUsage.tcs.clipDistance = 0;
      }

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else {
        if (m_importedOutputBuiltIns.find(BuiltInCullDistance) != m_importedOutputBuiltIns.end())
          inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = InvalidValue;
        else
          builtInUsage.tcs.cullDistance = 0;
      }

      if (nextBuiltInUsage.tessLevelOuter) {
        assert(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelOuter) !=
               nextInOutUsage.perPatchBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter];
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = mapLoc;
        availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
      } else {
        // NOTE: We have to map gl_TessLevelOuter to generic per-patch output as long as it is used.
        if (builtInUsage.tcs.tessLevelOuter)
          inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = InvalidValue;
      }

      if (nextBuiltInUsage.tessLevelInner) {
        assert(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelInner) !=
               nextInOutUsage.perPatchBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner];
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = mapLoc;
        availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
      } else {
        // NOTE: We have to map gl_TessLevelInner to generic per-patch output as long as it is used.
        if (builtInUsage.tcs.tessLevelInner)
          inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = InvalidValue;
      }

      // Revisit built-in outputs and map those unmapped to generic ones
      if (inOutUsage.builtInOutputLocMap.find(BuiltInPosition) != inOutUsage.builtInOutputLocMap.end() &&
          inOutUsage.builtInOutputLocMap[BuiltInPosition] == InvalidValue)
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;

      if (inOutUsage.builtInOutputLocMap.find(BuiltInPointSize) != inOutUsage.builtInOutputLocMap.end() &&
          inOutUsage.builtInOutputLocMap[BuiltInPointSize] == InvalidValue)
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;

      if (inOutUsage.builtInOutputLocMap.find(BuiltInClipDistance) != inOutUsage.builtInOutputLocMap.end() &&
          inOutUsage.builtInOutputLocMap[BuiltInClipDistance] == InvalidValue)
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;

      if (inOutUsage.builtInOutputLocMap.find(BuiltInCullDistance) != inOutUsage.builtInOutputLocMap.end() &&
          inOutUsage.builtInOutputLocMap[BuiltInCullDistance] == InvalidValue)
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;

      if (inOutUsage.perPatchBuiltInOutputLocMap.find(BuiltInTessLevelOuter) !=
              inOutUsage.perPatchBuiltInOutputLocMap.end() &&
          inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] == InvalidValue)
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = availPerPatchOutMapLoc++;

      if (inOutUsage.perPatchBuiltInOutputLocMap.find(BuiltInTessLevelInner) !=
              inOutUsage.perPatchBuiltInOutputLocMap.end() &&
          inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] == InvalidValue)
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = availPerPatchOutMapLoc++;
    } else if (nextStage == ShaderStageInvalid) {
      // TCS only
      if (builtInUsage.tcs.position)
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;

      if (builtInUsage.tcs.pointSize)
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;

      if (builtInUsage.tcs.clipDistance > 0) {
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;
        if (builtInUsage.tcs.clipDistance > 4)
          ++availOutMapLoc;
      }

      if (builtInUsage.tcs.cullDistance > 0) {
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;
        if (builtInUsage.tcs.cullDistance > 4)
          ++availOutMapLoc;
      }

      if (builtInUsage.tcs.tessLevelOuter)
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = availPerPatchOutMapLoc++;

      if (builtInUsage.tcs.tessLevelInner)
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = availPerPatchOutMapLoc++;
    }

    inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
    inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
    inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, availPerPatchOutMapLoc);
  } else if (m_shaderStage == ShaderStageTessEval) {
    // TES  ==>  XXX
    unsigned availInMapLoc = inOutUsage.inputMapLocCount;
    unsigned availOutMapLoc = inOutUsage.outputMapLocCount;

    unsigned availPerPatchInMapLoc = inOutUsage.perPatchInputMapLocCount;

    // Map built-in inputs to generic ones
    if (builtInUsage.tes.positionIn)
      inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;

    if (builtInUsage.tes.pointSizeIn)
      inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;

    if (builtInUsage.tes.clipDistanceIn > 0) {
      unsigned clipDistanceCount = builtInUsage.tes.clipDistanceIn;

      // NOTE: If gl_in[].gl_ClipDistance is used, we have to check the usage of gl_out[].gl_ClipDistance in
      // tessellation control shader. The clip distance is the maximum of the two. We do this to avoid
      // incorrectness of location assignment during builtin-to-generic mapping.
      const auto prevStage = m_pipelineState->getPrevShaderStage(m_shaderStage);
      if (prevStage == ShaderStageTessControl) {
        const auto &prevBuiltInUsage = m_pipelineState->getShaderResourceUsage(prevStage)->builtInUsage.tcs;
        clipDistanceCount = std::max(clipDistanceCount, prevBuiltInUsage.clipDistance);
      }

      inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
      if (clipDistanceCount > 4)
        ++availInMapLoc;
    }

    if (builtInUsage.tes.cullDistanceIn > 0) {
      unsigned cullDistanceCount = builtInUsage.tes.cullDistanceIn;

      const auto prevStage = m_pipelineState->getPrevShaderStage(m_shaderStage);
      if (prevStage == ShaderStageTessControl) {
        const auto &prevBuiltInUsage = m_pipelineState->getShaderResourceUsage(prevStage)->builtInUsage.tcs;
        cullDistanceCount = std::max(cullDistanceCount, prevBuiltInUsage.clipDistance);
      }

      inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
      if (cullDistanceCount > 4)
        ++availInMapLoc;
    }

    if (builtInUsage.tes.tessLevelOuter)
      inOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter] = availPerPatchInMapLoc++;

    if (builtInUsage.tes.tessLevelInner)
      inOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner] = availPerPatchInMapLoc++;

    // Map built-in outputs to generic ones
    if (nextStage == ShaderStageFragment) {
      // TES  ==>  FS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.fs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.clipDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
      }

      if (nextBuiltInUsage.cullDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
      }

      if (nextBuiltInUsage.primitiveId) {
        // NOTE: The usage flag of gl_PrimitiveID must be set if fragment shader uses it.
        builtInUsage.tes.primitiveId = true;

        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
        inOutUsage.builtInOutputLocMap[BuiltInPrimitiveId] = mapLoc;
      }

      if (nextBuiltInUsage.layer) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
      }

      if (nextBuiltInUsage.viewIndex) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = mapLoc;
      }

      if (nextBuiltInUsage.viewportIndex) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
      }
    } else if (nextStage == ShaderStageGeometry) {
      // TES  ==>  GS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.gs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.positionIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else
        builtInUsage.tes.position = false;

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else
        builtInUsage.tes.pointSize = false;

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else
        builtInUsage.tes.clipDistance = 0;

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else
        builtInUsage.tes.cullDistance = 0;

      builtInUsage.tes.layer = false;
      builtInUsage.tes.viewportIndex = false;
    } else if (nextStage == ShaderStageInvalid) {
      // TES only
      if (builtInUsage.tes.clipDistance > 0 || builtInUsage.tes.cullDistance > 0) {
        unsigned mapLoc = availOutMapLoc++;
        if (builtInUsage.tes.clipDistance + builtInUsage.tes.cullDistance > 4) {
          assert(builtInUsage.tes.clipDistance + builtInUsage.tes.cullDistance <= MaxClipCullDistanceCount);
          ++availOutMapLoc; // Occupy two locations
        }

        if (builtInUsage.tes.clipDistance > 0)
          inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;

        if (builtInUsage.tes.cullDistance > 0) {
          if (builtInUsage.tes.clipDistance >= 4)
            ++mapLoc;
          inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        }
      }

      if (builtInUsage.tes.viewportIndex)
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;

      if (builtInUsage.tes.layer)
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;

      if (builtInUsage.tes.viewIndex)
        inOutUsage.builtInOutputLocMap[BuiltInViewIndex] = availOutMapLoc++;
    }

    inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
    inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);

    inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, availPerPatchInMapLoc);
  } else if (m_shaderStage == ShaderStageGeometry) {
    // GS  ==>  XXX
    unsigned availInMapLoc = inOutUsage.inputMapLocCount;

    // Map built-in inputs to generic ones
    if (builtInUsage.gs.positionIn)
      inOutUsage.builtInInputLocMap[BuiltInPosition] = availInMapLoc++;

    if (builtInUsage.gs.pointSizeIn)
      inOutUsage.builtInInputLocMap[BuiltInPointSize] = availInMapLoc++;

    if (builtInUsage.gs.clipDistanceIn > 0) {
      inOutUsage.builtInInputLocMap[BuiltInClipDistance] = availInMapLoc++;
      if (builtInUsage.gs.clipDistanceIn > 4)
        ++availInMapLoc;
    }

    if (builtInUsage.gs.cullDistanceIn > 0) {
      inOutUsage.builtInInputLocMap[BuiltInCullDistance] = availInMapLoc++;
      if (builtInUsage.gs.cullDistanceIn > 4)
        ++availInMapLoc;
    }

    // Map built-in outputs to generic ones (for GS)
    if (builtInUsage.gs.position)
      mapGsBuiltInOutput(BuiltInPosition, 1);

    if (builtInUsage.gs.pointSize)
      mapGsBuiltInOutput(BuiltInPointSize, 1);

    if (builtInUsage.gs.clipDistance > 0)
      mapGsBuiltInOutput(BuiltInClipDistance, builtInUsage.gs.clipDistance);

    if (builtInUsage.gs.cullDistance > 0)
      mapGsBuiltInOutput(BuiltInCullDistance, builtInUsage.gs.cullDistance);

    if (builtInUsage.gs.primitiveId)
      mapGsBuiltInOutput(BuiltInPrimitiveId, 1);

    if (builtInUsage.gs.layer)
      mapGsBuiltInOutput(BuiltInLayer, 1);

    if (builtInUsage.gs.viewIndex)
      mapGsBuiltInOutput(BuiltInViewIndex, 1);

    if (builtInUsage.gs.viewportIndex)
      mapGsBuiltInOutput(BuiltInViewportIndex, 1);

    if (builtInUsage.gs.primitiveShadingRate)
      mapGsBuiltInOutput(BuiltInPrimitiveShadingRate, 1);

    // Map built-in outputs to generic ones (for copy shader)
    auto &builtInOutLocs = inOutUsage.gs.builtInOutLocs;

    if (nextStage == ShaderStageFragment) {
      // GS  ==>  FS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.fs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.clipDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        builtInOutLocs[BuiltInClipDistance] = mapLoc;
      }

      if (nextBuiltInUsage.cullDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        builtInOutLocs[BuiltInCullDistance] = mapLoc;
      }

      if (nextBuiltInUsage.primitiveId) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPrimitiveId) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPrimitiveId];
        builtInOutLocs[BuiltInPrimitiveId] = mapLoc;
      }

      if (nextBuiltInUsage.layer) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        builtInOutLocs[BuiltInLayer] = mapLoc;
      }

      if (nextBuiltInUsage.viewIndex) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewIndex];
        builtInOutLocs[BuiltInViewIndex] = mapLoc;
      }

      if (nextBuiltInUsage.viewportIndex) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        builtInOutLocs[BuiltInViewportIndex] = mapLoc;
      }
    } else if (nextStage == ShaderStageInvalid) {
      // GS only
      unsigned availOutMapLoc = inOutUsage.outputLocInfoMap.size(); // Reset available location

      if (builtInUsage.gs.clipDistance > 0 || builtInUsage.gs.cullDistance > 0) {
        unsigned mapLoc = availOutMapLoc++;
        if (builtInUsage.gs.clipDistance + builtInUsage.gs.cullDistance > 4) {
          assert(builtInUsage.gs.clipDistance + builtInUsage.gs.cullDistance <= MaxClipCullDistanceCount);
          ++availOutMapLoc; // Occupy two locations
        }

        if (builtInUsage.gs.clipDistance > 0)
          builtInOutLocs[BuiltInClipDistance] = mapLoc;

        if (builtInUsage.gs.cullDistance > 0) {
          if (builtInUsage.gs.clipDistance >= 4)
            ++mapLoc;
          builtInOutLocs[BuiltInCullDistance] = mapLoc;
        }
      }

      if (builtInUsage.gs.primitiveId)
        builtInOutLocs[BuiltInPrimitiveId] = availOutMapLoc++;

      if (builtInUsage.gs.viewportIndex)
        builtInOutLocs[BuiltInViewportIndex] = availOutMapLoc++;

      if (builtInUsage.gs.layer)
        builtInOutLocs[BuiltInLayer] = availOutMapLoc++;

      if (builtInUsage.gs.viewIndex)
        builtInOutLocs[BuiltInViewIndex] = availOutMapLoc++;
    }

    inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
  } else if (m_shaderStage == ShaderStageFragment) {
    // FS
    unsigned availInMapLoc = inOutUsage.inputMapLocCount;

    if (builtInUsage.fs.pointCoord)
      inOutUsage.builtInInputLocMap[BuiltInPointCoord] = availInMapLoc++;

    if (builtInUsage.fs.primitiveId)
      inOutUsage.builtInInputLocMap[BuiltInPrimitiveId] = availInMapLoc++;

    if (builtInUsage.fs.layer)
      inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;

    if (builtInUsage.fs.viewIndex)
      inOutUsage.builtInInputLocMap[BuiltInViewIndex] = availInMapLoc++;

    if (builtInUsage.fs.viewportIndex)
      inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;

    if (builtInUsage.fs.clipDistance > 0 || builtInUsage.fs.cullDistance > 0) {
      unsigned mapLoc = availInMapLoc++;
      if (builtInUsage.fs.clipDistance + builtInUsage.fs.cullDistance > 4) {
        assert(builtInUsage.fs.clipDistance + builtInUsage.fs.cullDistance <= MaxClipCullDistanceCount);
        ++availInMapLoc; // Occupy two locations
      }

      if (builtInUsage.fs.clipDistance > 0)
        inOutUsage.builtInInputLocMap[BuiltInClipDistance] = mapLoc;

      if (builtInUsage.fs.cullDistance > 0) {
        if (builtInUsage.fs.clipDistance >= 4)
          ++mapLoc;
        inOutUsage.builtInInputLocMap[BuiltInCullDistance] = mapLoc;
      }
    }

    inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
  }

  // Do builtin-to-generic mapping
  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC builtin-to-generic mapping results (" << getShaderStageAbbreviation(m_shaderStage)
                                                           << " shader)\n\n");
  if (!inOutUsage.builtInInputLocMap.empty()) {
    for (const auto &builtInMap : inOutUsage.builtInInputLocMap) {
      const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
      const unsigned loc = builtInMap.second;
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  builtin = "
                    << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!inOutUsage.builtInOutputLocMap.empty()) {
    for (const auto &builtInMap : inOutUsage.builtInOutputLocMap) {
      const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
      const unsigned loc = builtInMap.second;

      if (m_shaderStage == ShaderStageGeometry) {
        LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                      << ") Output: stream = " << inOutUsage.gs.rasterStream << " , "
                      << "builtin = " << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
      } else {
        LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: builtin = "
                      << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
      }
    }
    LLPC_OUTS("\n");
  }

  if (!inOutUsage.perPatchBuiltInInputLocMap.empty()) {
    for (const auto &builtInMap : inOutUsage.perPatchBuiltInInputLocMap) {
      const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
      const unsigned loc = builtInMap.second;
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input (per-patch):  builtin = "
                    << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!inOutUsage.perPatchBuiltInOutputLocMap.empty()) {
    for (const auto &builtInMap : inOutUsage.perPatchBuiltInOutputLocMap) {
      const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
      const unsigned loc = builtInMap.second;
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output (per-patch): builtin = "
                    << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
    }
    LLPC_OUTS("\n");
  }

  LLPC_OUTS("// LLPC location count results (after builtin-to-generic mapping)\n\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  loc count = " << inOutUsage.inputMapLocCount
                << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: loc count = " << inOutUsage.outputMapLocCount
                << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                << ") Input (per-patch):  loc count = " << inOutUsage.perPatchInputMapLocCount << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                << ") Output (per-patch): loc count = " << inOutUsage.perPatchOutputMapLocCount << "\n");
  LLPC_OUTS("\n");
}

// =====================================================================================================================
// Map built-in outputs of geometry shader to tightly packed locations.
//
// @param builtInId : Built-in ID
// @param elemCount : Element count of this built-in
void PatchResourceCollect::mapGsBuiltInOutput(unsigned builtInId, unsigned elemCount) {
  assert(m_shaderStage == ShaderStageGeometry);
  auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
  auto &inOutUsage = resUsage->inOutUsage.gs;
  unsigned streamId = inOutUsage.rasterStream;

  resUsage->inOutUsage.builtInOutputLocMap[builtInId] = inOutUsage.outLocCount[streamId]++;

  if (elemCount > 4)
    inOutUsage.outLocCount[streamId]++;

  unsigned assignedLocCount =
      inOutUsage.outLocCount[0] + inOutUsage.outLocCount[1] + inOutUsage.outLocCount[2] + inOutUsage.outLocCount[3];

  resUsage->inOutUsage.outputMapLocCount = std::max(resUsage->inOutUsage.outputMapLocCount, assignedLocCount);
}

// =====================================================================================================================
// The process of packing input/output
void PatchResourceCollect::packInOutLocation() {
  if (m_shaderStage == ShaderStageFragment || m_shaderStage == ShaderStageTessControl) {
    // Build location map based on FS (VS-FS, TES-FS) and TCS spans
    m_locationMapManager->buildLocationMap(m_shaderStage == ShaderStageFragment);
    fillInOutLocInfoMap();
  } else {
    reassembleOutputExportCalls();

    // Copy the InOutLocMap of the next stage to that of the current stage for computing the shader hash and looking
    // remapped location
    auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);
    if (nextStage != ShaderStageInvalid) {
      m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocInfoMap =
          m_pipelineState->getShaderResourceUsage(nextStage)->inOutUsage.inputLocInfoMap;
    }
  }
  // Clear it to hold previous calls
  m_inOutCalls.clear();
}

// =====================================================================================================================
// Fill inputLocInfoMap based on FS or TCS input import calls
void PatchResourceCollect::fillInOutLocInfoMap() {
  if (m_inOutCalls.empty())
    return;

  assert(m_shaderStage == ShaderStageFragment || m_shaderStage == ShaderStageTessControl);

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &inputLocInfoMap = inOutUsage.inputLocInfoMap;
  inputLocInfoMap.clear();

  // TCS: @llpc.input.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
  // FS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
  //      @llpc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx,
  //                                            i32 interpMode, <2 x float> | i32 auxInterpValue)
  const bool isTcs = m_shaderStage == ShaderStageTessControl;
  for (auto call : m_inOutCalls) {
    const bool isInterpolant = !isTcs && call->getNumArgOperands() != 4;
    unsigned locOffset = 0;
    unsigned compIdxArgIdx = 1;
    if (isInterpolant || isTcs) {
      assert(isa<ConstantInt>(call->getOperand(1)));
      locOffset = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
      compIdxArgIdx = 2;
    }

    // Construct original InOutLocationInfo from the location and elemIdx operands of the FS' or TCS' input import call
    InOutLocationInfo origLocInfo(0);
    origLocInfo.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue() + locOffset);
    origLocInfo.setComponent(cast<ConstantInt>(call->getOperand(compIdxArgIdx))->getZExtValue());

    // Get the packed InOutLocationInfo from locationMap
    InOutLocationInfoMap::const_iterator mapIter;
    assert(m_locationMapManager->findMap(origLocInfo, mapIter));
    m_locationMapManager->findMap(origLocInfo, mapIter);
    inputLocInfoMap.insert({origLocInfo, mapIter->second});
  }
}

// =====================================================================================================================
// Re-assemble output export functions based on the locationMap
void PatchResourceCollect::reassembleOutputExportCalls() {
  if (m_inOutCalls.empty())
    return;

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(m_inOutCalls.back());

  // ElementsInfo represents the info of composing a vector in a location
  struct ElementsInfo {
    // Elements to be packed in one location, where 32-bit element is placed at the even index
    Value *elements[8];
    // The corresponding call of each element
    CallInst *outCalls[8];
    // Element number of 32-bit
    unsigned elemCountOf32bit;
    // Element number of 16-bit
    unsigned elemCountOf16bit;
  };

  // Collect ElementsInfo in each packed location
  ElementsInfo elemsInfo = {{nullptr}, {nullptr}, 0, 0};
  std::vector<ElementsInfo> elementsInfoArray(m_inOutCalls.size(), elemsInfo);
  for (auto call : m_inOutCalls) {
    InOutLocationInfo origLocInfo(0);
    origLocInfo.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue());
    origLocInfo.setComponent(cast<ConstantInt>(call->getOperand(1))->getZExtValue());

    InOutLocationInfoMap::const_iterator mapIter;
    if (!m_locationMapManager->findMap(origLocInfo, mapIter)) {
      // An unused export call
      continue;
    }

    const unsigned newLoc = mapIter->second.getLocation();
    auto &elementsInfo = elementsInfoArray[newLoc];
    unsigned elemIdx = mapIter->second.getComponent() * 2 + mapIter->second.isHighHalf();
    elementsInfo.outCalls[elemIdx] = call;

    // Bit cast i8/i16/f16 to i32 for packing in a 32-bit component
    Value *element = call->getOperand(2);
    Type *elementTy = element->getType();
    const unsigned bitWidth = elementTy->getScalarSizeInBits();
    if (bitWidth == 8) {
      element = builder.CreateZExt(element, builder.getInt32Ty());
    } else if (bitWidth == 16) {
      if (elementTy->isHalfTy())
        element = builder.CreateBitCast(element, builder.getInt16Ty());
      element = builder.CreateZExt(element, builder.getInt32Ty());
    } else if (elementTy->isFloatTy()) {
      // float -> i32
      element = builder.CreateBitCast(element, builder.getInt32Ty());
    }
    elementsInfo.elements[elemIdx] = element;
    if (bitWidth < 32)
      ++elementsInfo.elemCountOf16bit;
    else
      ++elementsInfo.elemCountOf32bit;
  }

  // Re-assamble XX' output export calls for each packed location
  Value *args[3] = {};
  for (auto &elementsInfo : elementsInfoArray) {
    if (elementsInfo.elemCountOf16bit + elementsInfo.elemCountOf32bit == 0) {
      // It's the end of the packed location
      break;
    }

    // Construct the output value - a scalar or a vector
    const unsigned compCount = (elementsInfo.elemCountOf16bit + 1) / 2 + elementsInfo.elemCountOf32bit;
    assert(compCount <= 4);
    Value *outValue = nullptr;
    if (compCount == 1) {
      // Output a scalar
      outValue = elementsInfo.elements[0];
      if (elementsInfo.elemCountOf16bit == 2) {
        // Two 16-bit elements packed as a 32-bit scalar
        Value *highElem = builder.CreateShl(elementsInfo.elements[1], 16);
        outValue = builder.CreateOr(outValue, highElem);
      }
      outValue = builder.CreateBitCast(outValue, builder.getFloatTy());
    } else {
      // Output a vector
      outValue = UndefValue::get(FixedVectorType::get(builder.getFloatTy(), compCount));
      for (unsigned compIdx = 0; compIdx < compCount; ++compIdx) {
        const unsigned elemIdx = compIdx * 2;
        Value *elems[2] = {elementsInfo.elements[elemIdx], elementsInfo.elements[elemIdx + 1]};
        Value *component = elems[0];
        if (elems[1]) {
          // Two 16 - bit elements packed as a 32 - bit scalar
          elems[1] = builder.CreateShl(elems[1], 16);
          component = builder.CreateOr(component, elems[1]);
        }
        component = builder.CreateBitCast(component, builder.getFloatTy());
        outValue = builder.CreateInsertElement(outValue, component, compIdx);
      }
    }

    // Create an output export call with the original call aurgement
    args[0] = elementsInfo.outCalls[0]->getOperand(0);
    args[1] = elementsInfo.outCalls[0]->getOperand(1);
    args[2] = outValue;

    std::string callName(lgcName::OutputExportGeneric);
    addTypeMangling(nullptr, args, callName);
    builder.CreateNamedCall(callName, builder.getVoidTy(), args, {});
  }
}

// =====================================================================================================================
// Scalarize last vertex processing stage outputs and {TCS,FS} inputs ready for packing.
//
// @param [in/out] module : Module
void PatchResourceCollect::scalarizeForInOutPacking(Module *module) {
  // First gather the input/output calls that need scalarizing.
  SmallVector<CallInst *, 4> outputCalls;
  SmallVector<CallInst *, 4> inputCalls;
  for (Function &func : *module) {
    if (!m_pipelineState->canPackInOut())
      break;
    const bool isInterpolant = func.getName().startswith(lgcName::InputImportInterpolant);
    if (func.getName().startswith(lgcName::InputImportGeneric) || isInterpolant) {
      // This is a generic (possibly interpolated) input. Find its uses in FS (VS-FS, TES-FS) or TCS.
      for (User *user : func.users()) {
        auto call = cast<CallInst>(user);
        ShaderStage shaderStage = m_pipelineShaders->getShaderStage(call->getFunction());
        const bool isFs = shaderStage == ShaderStageFragment;
        const bool isTcs = shaderStage == ShaderStageTessControl;
        if (isFs || isTcs) {
          // This is a workaround to disable pack for the pipeline if there exists dynamic index in TCS
          // TODO: Do partial packing except calls with dynamic index in future change
          // NOTE: Dynamic index (location offset or component) in FS is processed to be constant in lower pass.
          assert(!isInterpolant ||
                 (isInterpolant && isa<ConstantInt>(call->getOperand(1)) && isa<ConstantInt>(call->getOperand(2))));
          const bool hasDynIdx =
              isTcs && (!isa<ConstantInt>(call->getOperand(1)) || !isa<ConstantInt>(call->getOperand(2)));
          if (hasDynIdx) {
            m_pipelineState->setPackInOut(false);
            break;
          }
          // We have a use in FS (VS-FS, TES-FS) or TCS. See if it needs scalarizing.
          if (isa<VectorType>(call->getType()) || call->getType()->getPrimitiveSizeInBits() == 64)
            inputCalls.push_back(call);
        }
      }
    } else if (func.getName().startswith(lgcName::OutputExportGeneric)) {
      // This is a generic output. Find its uses in VS or TES (TES-FS).
      for (User *user : func.users()) {
        auto call = cast<CallInst>(user);
        ShaderStage shaderStage = m_pipelineShaders->getShaderStage(call->getFunction());
        if (shaderStage == ShaderStageTessEval || shaderStage == ShaderStageVertex) {
          // We have a use in the last vertex processing stage. See if it needs scalarizing. The output value is
          // always the final argument.
          assert(isa<ConstantInt>(call->getOperand(1)));
          Type *valueTy = call->getArgOperand(call->getNumArgOperands() - 1)->getType();
          if (isa<VectorType>(valueTy) || valueTy->getPrimitiveSizeInBits() == 64)
            outputCalls.push_back(call);
        }
      }
    }
  }
  if (m_pipelineState->canPackInOut()) {
    // Scalarize the gathered inputs and outputs.
    for (CallInst *call : inputCalls)
      scalarizeGenericInput(call);
    for (CallInst *call : outputCalls)
      scalarizeGenericOutput(call);
  }
}

// =====================================================================================================================
// Scalarize a generic input.
// This is known to be an FS generic or interpolant input or TCS input that is either a vector or 64 bit.
//
// @param call : Call that represents importing the generic or interpolant input
void PatchResourceCollect::scalarizeGenericInput(CallInst *call) {
  BuilderBase builder(call->getContext());
  builder.SetInsertPoint(call);
  // TCS: @llpc.input.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
  // FS:  @llpc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
  //      @llpc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx,
  //                                            i32 interpMode, <2 x float> | i32 auxInterpValue)
  SmallVector<Value *, 5> args;
  for (unsigned i = 0, end = call->getNumArgOperands(); i != end; ++i)
    args.push_back(call->getArgOperand(i));

  const bool isFs = m_pipelineShaders->getShaderStage(call->getFunction()) == ShaderStageFragment;
  bool isInterpolant = isFs && args.size() == 5;
  unsigned elemIdxArgIdx = isFs && !isInterpolant ? 1 : 2;
  unsigned elemIdx = cast<ConstantInt>(args[elemIdxArgIdx])->getZExtValue();
  Type *resultTy = call->getType();

  if (!isa<VectorType>(resultTy)) {
    // Handle the case of splitting a 64 bit scalar in two.
    assert(resultTy->getPrimitiveSizeInBits() == 64);
    std::string callName = isInterpolant ? lgcName::InputImportInterpolant : lgcName::InputImportGeneric;
    addTypeMangling(builder.getInt32Ty(), args, callName);
    Value *result = UndefValue::get(FixedVectorType::get(builder.getInt32Ty(), 2));
    for (unsigned i = 0; i != 2; ++i) {
      args[elemIdxArgIdx] = builder.getInt32(elemIdx * 2 + i);
      result = builder.CreateInsertElement(
          result, builder.CreateNamedCall(callName, builder.getInt32Ty(), args, Attribute::ReadOnly), i);
    }
    result = builder.CreateBitCast(result, call->getType());
    call->replaceAllUsesWith(result);
    call->eraseFromParent();
    return;
  }

  // Now we know we're reading a vector.
  Type *elementTy = cast<VectorType>(resultTy)->getElementType();
  unsigned scalarizeBy = cast<FixedVectorType>(resultTy)->getNumElements();

  // Find trivially unused elements.
  // This is not quite as good as the previous version of this code that scalarized in the
  // front-end before running some LLVM optimizations that removed unused inputs. In the future,
  // we can fix this properly by doing the whole of generic input/output assignment later on in
  // the middle-end, somewhere in the LLVM middle-end optimization pass flow.
  static const unsigned MaxScalarizeBy = 4;
  assert(scalarizeBy <= MaxScalarizeBy);
  bool elementUsed[MaxScalarizeBy] = {};
  bool unknownElementsUsed = false;
  for (User *user : call->users()) {
    if (auto extract = dyn_cast<ExtractElementInst>(user)) {
      unsigned idx = cast<ConstantInt>(extract->getIndexOperand())->getZExtValue();
      assert(idx < scalarizeBy);
      elementUsed[idx] = true;
      continue;
    }
    if (auto shuffle = dyn_cast<ShuffleVectorInst>(user)) {
      SmallVector<int, 4> mask;
      shuffle->getShuffleMask(mask);
      for (int maskElement : mask) {
        if (maskElement >= 0) {
          if (maskElement < scalarizeBy) {
            if (shuffle->getOperand(0) == call)
              elementUsed[maskElement] = true;
          } else {
            assert(maskElement < 2 * scalarizeBy);
            if (shuffle->getOperand(1) == call)
              elementUsed[maskElement - scalarizeBy] = true;
          }
        }
      }
      continue;
    }
    unknownElementsUsed = true;
    break;
  }

  // Load the individual elements and insert into a vector.
  Value *result = UndefValue::get(resultTy);
  std::string callName = isInterpolant ? lgcName::InputImportInterpolant : lgcName::InputImportGeneric;
  addTypeMangling(elementTy, args, callName);
  const unsigned nextLocIdx = cast<ConstantInt>(args[0])->getZExtValue() + 1;
  const bool is64Bit = elementTy->getPrimitiveSizeInBits() == 64;
  for (unsigned i = 0; i != scalarizeBy; ++i) {
    if (!unknownElementsUsed && !elementUsed[i])
      continue; // Omit trivially unused element
    unsigned newElemIdx = elemIdx + i;
    if (is64Bit && i > 1) {
      args[0] = builder.getInt32(nextLocIdx);
      newElemIdx = newElemIdx - 2;
    }
    args[elemIdxArgIdx] = builder.getInt32(newElemIdx);

    CallInst *element = builder.CreateNamedCall(callName, elementTy, args, Attribute::ReadOnly);
    result = builder.CreateInsertElement(result, element, i);
    if (elementTy->getPrimitiveSizeInBits() == 64) {
      // If scalarizing with 64 bit elements, further split each element.
      scalarizeGenericInput(element);
    }
  }

  call->replaceAllUsesWith(result);
  call->eraseFromParent();
}

// =====================================================================================================================
// Scalarize a generic output.
// This is known to be a last vertex processing stage (VS/TES/GS) generic output that is either a vector or 64 bit.
//
// @param call : Call that represents exporting the generic output
void PatchResourceCollect::scalarizeGenericOutput(CallInst *call) {
  BuilderBase builder(call->getContext());
  builder.SetInsertPoint(call);

  // VS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
  // TES: @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
  // GS:  @llpc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
  SmallVector<Value *, 5> args;
  for (unsigned i = 0, end = call->getNumArgOperands(); i != end; ++i)
    args.push_back(call->getArgOperand(i));

  static const unsigned ElemIdxArgIdx = 1;
  unsigned valArgIdx = call->getNumArgOperands() - 1;
  unsigned elemIdx = cast<ConstantInt>(args[ElemIdxArgIdx])->getZExtValue();
  Value *outputVal = call->getArgOperand(valArgIdx);
  Type *elementTy = outputVal->getType();
  unsigned scalarizeBy = 1;
  if (auto vectorTy = dyn_cast<FixedVectorType>(elementTy)) {
    scalarizeBy = vectorTy->getNumElements();
    elementTy = vectorTy->getElementType();
  }

  // For a 64-bit element type, split each element in two. (We're assuming no interpolation for 64 bit.)
  if (elementTy->getPrimitiveSizeInBits() == 64) {
    scalarizeBy *= 2;
    elemIdx *= 2;
    elementTy = builder.getInt32Ty();
  }

  // Bitcast the original value to the vector type if necessary.
  outputVal = builder.CreateBitCast(outputVal, FixedVectorType::get(elementTy, scalarizeBy));

  // Extract and store the individual elements.
  std::string callName;
  const unsigned nextLocIdx = cast<ConstantInt>(args[0])->getZExtValue() + 1;
  for (unsigned i = 0; i != scalarizeBy; ++i) {
    unsigned newElemIdx = elemIdx + i;
    if (i >= 4) {
      args[0] = builder.getInt32(nextLocIdx);
      newElemIdx = newElemIdx - 4;
    }
    args[ElemIdxArgIdx] = builder.getInt32(newElemIdx);
    args[valArgIdx] = builder.CreateExtractElement(outputVal, i);
    if (i == 0) {
      callName = lgcName::OutputExportGeneric;
      addTypeMangling(nullptr, args, callName);
    }
    builder.CreateNamedCall(callName, builder.getVoidTy(), args, {});
  }

  call->eraseFromParent();
}

// =====================================================================================================================
// Fill the locationSpan container by constructing a LocationSpan from each input import call
//
// @param call : Call to process
// @param shaderStage : Shader stage
void InOutLocationMapManager::addSpan(CallInst *call, ShaderStage shaderStage) {
  const bool isTcs = shaderStage == ShaderStageTessControl;
  const bool isInterpolant = !isTcs && call->getNumArgOperands() != 4;
  unsigned locOffset = 0;
  unsigned compIdxArgIdx = 1;
  if (isInterpolant || isTcs) {
    assert(isa<ConstantInt>(call->getOperand(1)));
    locOffset = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
    compIdxArgIdx = 2;
  }

  LocationSpan span = {};
  span.firstLocation.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue() + locOffset);
  span.firstLocation.setComponent(cast<ConstantInt>(call->getOperand(compIdxArgIdx))->getZExtValue());

  unsigned bitWidth = call->getType()->getScalarSizeInBits();
  if (isTcs && bitWidth < 32)
    bitWidth = 32;
  else if (bitWidth == 8)
    bitWidth = 16;
  span.compatibilityInfo.halfComponentCount = bitWidth / 16;
  // For XX-FS, 32-bit and 16-bit are packed seperately; For VS-TCS, they are packed together
  span.compatibilityInfo.is16Bit = bitWidth == 16;

  if (!isTcs) {
    const unsigned interpMode = cast<ConstantInt>(call->getOperand(compIdxArgIdx + 1))->getZExtValue();
    span.compatibilityInfo.isFlat = interpMode == InOutInfo::InterpModeFlat;
    span.compatibilityInfo.isCustom = interpMode == InOutInfo::InterpModeCustom;

    assert(isInterpolant || (!isInterpolant && !is_contained(m_locationSpans, span)));
  }
  if (!is_contained(m_locationSpans, span)) {
    m_locationSpans.push_back(span);
  }
}

// =====================================================================================================================
// Build the map between orignal InOutLocationInfo and packed InOutLocationInfo based on sorted locaiton spans
//
// @param checkCompatibility : whether to check compatibilty between two spans to start a new location
void InOutLocationMapManager::buildLocationMap(bool checkCompatibility) {
  if (m_locationSpans.empty())
    return;
  // Sort m_locationSpans based on LocationSpan::GetCompatibilityKey() and InOutLocationInfo::AsIndex()
  std::sort(m_locationSpans.begin(), m_locationSpans.end());

  m_locationMap.clear();

  // Map original InOutLocationInfo to new InOutLocationInfo
  unsigned consectiveLocation = 0;
  unsigned compIdx = 0;
  bool isHighHalf = false;
  for (auto spanIt = m_locationSpans.begin(); spanIt != m_locationSpans.end(); ++spanIt) {
    if (spanIt != m_locationSpans.begin()) {
      // Check the current span with previous span to determine wether it is put in the same location or the next
      // location.
      const auto &prevSpan = *(--spanIt);
      ++spanIt;

      // Start a new location in two case:
      // 1. the component index is up to 4
      // 2. checkCompatibility is enabled (FS input) and the two adjacent spans are not compatible
      if (compIdx > 3 || (checkCompatibility && !isCompatible(prevSpan, *spanIt))) {
        ++consectiveLocation;
        compIdx = 0;
        isHighHalf = false;
      } else {
        isHighHalf = spanIt->compatibilityInfo.is16Bit ? !isHighHalf : false;
      }
    }

    // Add a location map item
    InOutLocationInfo newLocInfo(0);
    newLocInfo.setLocation(consectiveLocation);
    newLocInfo.setComponent(compIdx);
    newLocInfo.setHighHalf(isHighHalf);
    m_locationMap.insert({spanIt->firstLocation, newLocInfo});

    // Update component index
    if ((spanIt->compatibilityInfo.is16Bit && isHighHalf) || !spanIt->compatibilityInfo.is16Bit)
      ++compIdx;
    assert(compIdx <= 4);
  }

  // Exists temporarily for computing m_locationMap
  m_locationSpans.clear();
}

// =====================================================================================================================
// Output a mapped InOutLocationInfo from a given InOutLocationInfo if the mapping exists
//
// @param origLocInfo : The original InOutLocationInfo
// @param [out] mapIt : Iterator to an element of m_locationMap with key equivalent to the given InOutLocationInfo
bool InOutLocationMapManager::findMap(const InOutLocationInfo &origLocInfo,
                                      InOutLocationInfoMap::const_iterator &mapIt) {
  mapIt = m_locationMap.find(origLocInfo);
  return mapIt != m_locationMap.end();
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for resource collecting.
INITIALIZE_PASS(PatchResourceCollect, DEBUG_TYPE, "Patch LLVM for resource collecting", false, false)
