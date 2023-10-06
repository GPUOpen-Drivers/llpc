/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/patch/PatchResourceCollect.h"
#include "Gfx6Chip.h"
#include "Gfx9Chip.h"
#include "MeshTaskShader.h"
#include "NggPrimShader.h"
#include "lgc/Builder.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "lgc/util/Debug.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
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
PatchResourceCollect::PatchResourceCollect() : m_resUsage(nullptr) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchResourceCollect::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  runImpl(module, pipelineShaders, pipelineState);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineShaders : Pipeline shaders analysis result
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchResourceCollect::runImpl(Module &module, PipelineShadersResult &pipelineShaders,
                                   PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Resource-Collect\n");

  Patch::init(&module);
  m_pipelineShaders = &pipelineShaders;
  m_pipelineState = pipelineState;

  // This pass processes a missing fragment shader using FS input packing information passed into LGC
  // from the separate compile of the FS.
  m_processMissingFs = pipelineState->isPartPipeline();

  m_tcsInputHasDynamicIndexing = false;

  bool needPack = false;
  for (int shaderStage = 0; shaderStage < ShaderStageGfxCount; ++shaderStage) {
    ShaderStage stage = static_cast<ShaderStage>(shaderStage);
    if (pipelineState->hasShaderStage(stage) &&
        (pipelineState->canPackInput(stage) || pipelineState->canPackOutput(stage))) {
      needPack = true;
      break;
    }
  }
  if (needPack) {
    m_locationInfoMapManager = std::make_unique<InOutLocationInfoMapManager>();
    // If packing {VS, TES, GS} outputs and {TCS, GS, FS} inputs, scalarize those outputs and inputs now.
    scalarizeForInOutPacking(&module);
  }

  // Process each shader stage, in reverse order. We process FS even if it does not exist (part-pipeline compile).
  for (int shaderStage = ShaderStageCountInternal - 1; shaderStage >= 0; --shaderStage) {
    m_entryPoint = pipelineShaders.getEntryPoint(static_cast<ShaderStage>(shaderStage));
    m_shaderStage = static_cast<ShaderStage>(shaderStage);
    if (m_entryPoint)
      processShader();
    else if (m_shaderStage == ShaderStageFragment)
      processMissingFs();
  }

  // Process non-entry-point shaders
  for (Function &func : module) {
    if (func.isDeclaration())
      continue;
    m_shaderStage = getShaderStage(&func);
    if (m_shaderStage == ShaderStage::ShaderStageInvalid || &func == pipelineShaders.getEntryPoint(m_shaderStage))
      continue;
    m_entryPoint = &func;
    processShader();
  }

  // Check ray query LDS stack usage
  checkRayQueryLdsStackUsage(&module);

  if (pipelineState->isGraphics()) {
    // Set NGG control settings
    setNggControl(&module);

    // Determine whether or not GS on-chip mode is valid for this pipeline
    bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
    const bool meshPipeline =
        m_pipelineState->hasShaderStage(ShaderStageTask) || m_pipelineState->hasShaderStage(ShaderStageMesh);
    bool checkGsOnChip = hasGs || meshPipeline || pipelineState->getNggControl()->enableNgg;

    if (checkGsOnChip) {
      bool gsOnChip = checkGsOnChipValidity();
      pipelineState->setGsOnChip(gsOnChip);
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

  // If mesh pipeline, skip NGG control settings
  const bool meshPipeline =
      m_pipelineState->hasShaderStage(ShaderStageTask) || m_pipelineState->hasShaderStage(ShaderStageMesh);
  if (meshPipeline)
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
  nggControl.enableGsUse =
      (options.nggFlags & NggFlagEnableGsUse) ||
      (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11); // Always enable NGG on GS for GFX11+
  nggControl.compactVertex = (options.nggFlags & NggFlagCompactVertex);

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
      nggControl.passthroughMode = !nggControl.enableBackfaceCulling && !nggControl.enableFrustumCulling &&
                                   !nggControl.enableBoxFilterCulling && !nggControl.enableSphereCulling &&
                                   !nggControl.enableSmallPrimFilter && !nggControl.enableCullDistanceCulling;
    }

    // NOTE: Further check if we have to turn on pass-through mode forcibly.
    if (!nggControl.passthroughMode)
      nggControl.passthroughMode = !canUseNggCulling(module);

    LLPC_OUTS("===============================================================================\n");
    LLPC_OUTS("// LLPC NGG control settings results\n\n");

    // Control option
    LLPC_OUTS("EnableNgg                    = " << nggControl.enableNgg << "\n");
    LLPC_OUTS("EnableGsUse                  = " << nggControl.enableGsUse << "\n");
    LLPC_OUTS("PassthroughMode              = " << nggControl.passthroughMode << "\n");
    LLPC_OUTS("CompactVertex                = " << nggControl.compactVertex << "\n");
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

  // Always enable NGG for GFX11+
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 11)
    return true;

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
  if (m_pipelineState->enableXfb() || m_pipelineState->enablePrimStats())
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
    const auto primType = m_pipelineState->getInputAssemblyState().primitiveType;
    if (hasTs) {
      // For tessellation, check primitive mode
      assert(primType == PrimitiveType::Patch);
      const auto &tessMode = m_pipelineState->getShaderModes()->getTessellationMode();
      if (tessMode.pointMode || tessMode.primitiveMode == PrimitiveMode::Isolines)
        return false;
    } else {
      // Check primitive type specified in pipeline state
      if (primType < PrimitiveType::TriangleList)
        return false;
    }
  }

  // Check resource usage, disable culling if there are resource write operations (including atomic operations) in
  // NGG cases when API GS is not present. This is because such write operations have side effect in execution
  // sequences. But when GS is present, we can still enable culling. Culling is performed after GS execution.
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
  auto posValue = posCall->getArgOperand(posCall->arg_size() - 1); // Last argument is position value
  if (isa<Constant>(posValue))
    return false;

  // Heuristic detecting very simple calculated position for geometry that will
  // never be culled, disable NGG culling if there is no position fetch.
  auto hasPositionFetch = [posCall] {
    std::list<Instruction *> workList;
    workList.push_back(posCall);
    std::unordered_set<Instruction *> visited;
    while (!workList.empty()) {
      Instruction *inst = workList.front();
      workList.pop_front();
      visited.insert(inst);
      for (Value *op : inst->operands()) {
        LoadInst *opLoad = dyn_cast<LoadInst>(op);
        if (opLoad && opLoad->getPointerAddressSpace() != ADDR_SPACE_CONST)
          return true;
        Instruction *opInst = dyn_cast<Instruction>(op);
        if (opInst && visited.find(opInst) == visited.end())
          workList.push_back(opInst);
      }
    }
    return false;
  };
  bool hasVertexFetch = m_pipelineState->getPalMetadata()->getVertexFetchCount() != 0;
  if (!hasGs && !hasVertexFetch && !hasPositionFetch())
    return false;

  // We can safely enable NGG culling here
  return true;
}

// =====================================================================================================================
// Determines whether GS on-chip mode is valid for this pipeline, also computes ES-GS/GS-VS ring item size.
bool PatchResourceCollect::checkGsOnChipValidity() {
  bool gsOnChip = true;

  const bool hasTs =
      m_pipelineState->hasShaderStage(ShaderStageTessControl) || m_pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
  const bool meshPipeline =
      m_pipelineState->hasShaderStage(ShaderStageTask) || m_pipelineState->hasShaderStage(ShaderStageMesh);

  const auto &geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
  auto gsResUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

  const GfxIpVersion gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();

  unsigned inVertsPerPrim = 0;
  bool useAdjacency = false;

  if (hasGs) {
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
      llvm_unreachable("Unexpected input primitive type!");
      break;
    }

    gsResUsage->inOutUsage.gs.calcFactor.inputVertices = inVertsPerPrim;
  } else if (hasTs) {
    inVertsPerPrim = m_pipelineState->getNumPatchControlPoints();
  } else {
    const auto primType = m_pipelineState->getInputAssemblyState().primitiveType;
    switch (primType) {
    case PrimitiveType::Point:
      inVertsPerPrim = 1;
      break;
    case PrimitiveType::LineList:
    case PrimitiveType::LineStrip:
      inVertsPerPrim = 2;
      break;
    case PrimitiveType::TriangleList:
    case PrimitiveType::TriangleStrip:
    case PrimitiveType::TriangleFan:
    case PrimitiveType::TriangleListAdjacency:
    case PrimitiveType::TriangleStripAdjacency:
      inVertsPerPrim = 3;
      break;
    default:
      llvm_unreachable("Unexpected primitive type!");
      break;
    }
  }

  if (gfxIp.major <= 8) {
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

    // For normal primitives, the VGT only checks if they are past the ES verts per subgroup after allocating a full
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
    // need to clear unused builtin output before determining onchip/offchip GS mode.
    constexpr unsigned gsOffChipDefaultThreshold = 32;

    bool disableGsOnChip = DisableGsOnChip;
    if (hasTs || m_pipelineState->getTargetInfo().getGfxIpVersion().major == 6) {
      // GS on-chip is not supported with tessellation, and is not supported on GFX6
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

    if (meshPipeline) {
      assert(gfxIp >= GfxIpVersion({10, 3})); // Must be GFX10.3+
      const auto &meshMode = m_pipelineState->getShaderModes()->getMeshShaderMode();

      // Make sure we have enough threads to execute mesh shader.
      const unsigned numMeshThreads = meshMode.workgroupSizeX * meshMode.workgroupSizeY * meshMode.workgroupSizeZ;
      unsigned primAmpFactor = std::max(numMeshThreads, std::max(meshMode.outputVertices, meshMode.outputPrimitives));

      const unsigned ldsSizeDwordGranularity =
          1u << m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
      auto ldsSizeDwords =
          MeshTaskShader::layoutMeshShaderLds(m_pipelineState, m_pipelineShaders->getEntryPoint(ShaderStageMesh));
      ldsSizeDwords = alignTo(ldsSizeDwords, ldsSizeDwordGranularity);

      // Make sure we don't allocate more than what can legally be allocated by a single subgroup on the hardware.
      unsigned maxHwGsLdsSizeDwords = m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipMaxLdsSize;
      assert(ldsSizeDwords <= maxHwGsLdsSizeDwords);
      (void(maxHwGsLdsSizeDwords)); // Unused

      gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = 1;
      gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = 1;

      gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = ldsSizeDwords;

      gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize = 0;
      gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize = 0;

      gsResUsage->inOutUsage.gs.calcFactor.primAmpFactor = primAmpFactor;

      gsOnChip = true; // For mesh shader, GS is always on-chip
    } else if (nggControl->enableNgg) {
      unsigned esGsRingItemSize = NggPrimShader::calcEsGsRingItemSize(m_pipelineState); // In dwords

      const unsigned gsVsRingItemSize =
          hasGs ? std::max(1u, 4 * gsResUsage->inOutUsage.outputMapLocCount * geometryMode.outputVertices) : 0;

      const auto &ldsGeneralUsage = NggPrimShader::layoutPrimShaderLds(m_pipelineState);
      const bool needsLds = ldsGeneralUsage.needsLds;
      const unsigned esExtraLdsSize = ldsGeneralUsage.esExtraLdsSize; // In dwords
      const unsigned gsExtraLdsSize = ldsGeneralUsage.gsExtraLdsSize; // In dwords

      // NOTE: Primitive amplification factor must be at least 1. And for NGG with API GS, we force number of output
      // primitives to be equal to that of output vertices regardless of the output primitive type by emitting
      // invalid primitives. This is to simplify the algorithmic design and improve its efficiency.
      unsigned primAmpFactor = std::max(1u, geometryMode.outputVertices);

      unsigned esVertsPerSubgroup = 0;
      unsigned gsPrimsPerSubgroup = 0;

      // The numbers below come from hardware guidance and most likely require further tuning.
      switch (nggControl->subgroupSizing) {
      case NggSubgroupSizing::HalfSize:
        esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
        gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
        break;
      case NggSubgroupSizing::OptimizeForVerts:
        esVertsPerSubgroup = hasTs ? Gfx9::NggMaxThreadsPerSubgroup / 2 : (Gfx9::NggMaxThreadsPerSubgroup / 2 - 2);
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
      case NggSubgroupSizing::Auto:
        if (m_pipelineState->getTargetInfo().getGfxIpVersion().isGfx(10, 1)) {
          esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2 - 2;
          gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
        } else {
          // Newer hardware performs the decrement on esVertsPerSubgroup for us already.
          esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
          gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup / 2;
        }
        break;
      case NggSubgroupSizing::MaximumSize:
      default:
        esVertsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
        gsPrimsPerSubgroup = Gfx9::NggMaxThreadsPerSubgroup;
        break;
      }

      static const unsigned OptimalVerticesPerPrimitiveForTess = 2;
      unsigned gsInstanceCount = std::max(1u, geometryMode.invocations);
      bool enableMaxVertOut = false;

      if (hasGs) {
        unsigned maxVertOut = geometryMode.outputVertices;
        assert(maxVertOut >= primAmpFactor);
        assert(gsInstanceCount >= 1);

        // Each input GS primitive can generate at most maxVertOut vertices. Each output vertex will be emitted
        // from a different thread. Note that maxVertOut does not account for additional amplification due
        // to GS instancing.
        gsPrimsPerSubgroup =
            std::max(1u, std::min(gsPrimsPerSubgroup, Gfx9::NggMaxThreadsPerSubgroup / (maxVertOut * gsInstanceCount)));

        // NOTE: If one input GS primitive generates too many vertices (consider GS instancing) and they couldn't be
        // within a NGG subgroup, we enable maximum vertex output per GS instance. This will set the register field
        // EN_MAX_VERT_OUT_PER_GS_INSTANCE and turn off vertex reuse, restricting 1 input GS input
        // primitive per subgroup and create 1 subgroup per GS instance.
        if ((maxVertOut * gsInstanceCount) > Gfx9::NggMaxThreadsPerSubgroup) {
          enableMaxVertOut = true;
          gsInstanceCount = 1;
          gsPrimsPerSubgroup = 1;
        }

        esVertsPerSubgroup = std::min(gsPrimsPerSubgroup * inVertsPerPrim, Gfx9::NggMaxThreadsPerSubgroup);

        if (hasTs)
          esVertsPerSubgroup = std::min(esVertsPerSubgroup, OptimalVerticesPerPrimitiveForTess * gsPrimsPerSubgroup);

        // Low values of esVertsPerSubgroup are illegal. These numbers below come from HW restrictions.
        if (gfxIp.isGfx(10, 3))
          esVertsPerSubgroup = std::max(29u, esVertsPerSubgroup);
        else if (gfxIp.isGfx(10, 1))
          esVertsPerSubgroup = std::max(24u, esVertsPerSubgroup);
      } else {
        // If GS is not present, instance count must be 1
        assert(gsInstanceCount == 1);
      }

      // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
      // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
      // restriction by providing the capability of querying thread ID in the group rather than in wave.
      unsigned rayQueryLdsStackSize = 0;
      if (gsResUsage->useRayQueryLdsStack) {
        gsPrimsPerSubgroup = std::min(MaxRayQueryThreadsPerGroup, gsPrimsPerSubgroup);
        rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;
      }

      auto esResUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);
      if (esResUsage->useRayQueryLdsStack) {
        esVertsPerSubgroup = std::min(MaxRayQueryThreadsPerGroup, esVertsPerSubgroup);
        rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;
      }

      // Make sure that we have at least one primitive.
      gsPrimsPerSubgroup = std::max(1u, gsPrimsPerSubgroup);

      unsigned expectedEsLdsSize = esVertsPerSubgroup * esGsRingItemSize + esExtraLdsSize;
      unsigned expectedGsLdsSize = gsPrimsPerSubgroup * gsInstanceCount * gsVsRingItemSize + gsExtraLdsSize;

      const unsigned ldsSizeDwordGranularity =
          1u << m_pipelineState->getTargetInfo().getGpuProperty().ldsSizeDwordGranularityShift;
      unsigned ldsSizeDwords = alignTo(expectedEsLdsSize + expectedGsLdsSize, ldsSizeDwordGranularity);

      unsigned maxHwGsLdsSizeDwords = m_pipelineState->getTargetInfo().getGpuProperty().gsOnChipMaxLdsSize;
      maxHwGsLdsSizeDwords -= rayQueryLdsStackSize; // Exclude LDS space used as ray query stack

      // In exceedingly rare circumstances, a NGG subgroup might calculate its LDS space requirements and overallocate.
      // In those cases we need to scale down our esVertsPerSubgroup/gsPrimsPerSubgroup so that they'll fit in LDS.
      // In the following NGG calculation, we'll attempt to set esVertsPerSubgroup/gsPrimsPerSubgroup first and
      // then scale down if we overallocate LDS.
      if (ldsSizeDwords > maxHwGsLdsSizeDwords) {
        // For esVertsPerSubgroup, we can instead substitute (esVertToGsPrimRatio * gsPrimsPerSubgroup) for
        // esVertsPerSubgroup. Then we can rearrange the equation and solve for gsPrimsPerSubgroup.
        float esVertToGsPrimRatio = esVertsPerSubgroup / (gsPrimsPerSubgroup * 1.0f);

        if (hasTs)
          esVertToGsPrimRatio = std::min(esVertToGsPrimRatio, OptimalVerticesPerPrimitiveForTess * 1.0f);

        // The equation for required LDS is:
        // LDS allocation = (esGsRingItemSize * esVertsPerSubgroup) +
        //                  (gsVsRingItemSize * gsInstanceCount * gsPrimsPerSubgroup) +
        //                  extraLdsSize
        gsPrimsPerSubgroup =
            std::min(gsPrimsPerSubgroup, (maxHwGsLdsSizeDwords - esExtraLdsSize - gsExtraLdsSize) /
                                             (static_cast<unsigned>(esGsRingItemSize * esVertToGsPrimRatio) +
                                              gsVsRingItemSize * gsInstanceCount));

        // NOTE: If one input GS primitive generates too many vertices (consider GS instancing) and they couldn't be
        // within a NGG subgroup, we enable maximum vertex output per GS instance. This will set the register field
        // EN_MAX_VERT_OUT_PER_GS_INSTANCE and turn off vertex reuse, restricting 1 input GS input
        // primitive per subgroup and create 1 subgroup per GS instance.
        if (gsPrimsPerSubgroup < gsInstanceCount) {
          enableMaxVertOut = true;
          gsInstanceCount = 1;
          gsPrimsPerSubgroup = 1;
        }

        // Make sure that we have at least one primitive.
        gsPrimsPerSubgroup = std::max(1u, gsPrimsPerSubgroup);

        // inVertsPerPrim is the minimum number of vertices we must have per subgroup.
        esVertsPerSubgroup =
            std::max(inVertsPerPrim, std::min(static_cast<unsigned>(gsPrimsPerSubgroup * esVertToGsPrimRatio),
                                              Gfx9::NggMaxThreadsPerSubgroup));

        // Low values of esVertsPerSubgroup are illegal. These numbers below come from HW restrictions.
        if (gfxIp.isGfx(10, 3))
          esVertsPerSubgroup = std::max(29u, esVertsPerSubgroup);
        else if (gfxIp.isGfx(10, 1))
          esVertsPerSubgroup = std::max(24u, esVertsPerSubgroup);

        // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
        // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
        // restriction by providing the capability of querying thread ID in the group rather than in wave.
        if (gsResUsage->useRayQueryLdsStack)
          gsPrimsPerSubgroup = std::min(MaxRayQueryThreadsPerGroup, gsPrimsPerSubgroup);
        if (esResUsage->useRayQueryLdsStack)
          esVertsPerSubgroup = std::min(MaxRayQueryThreadsPerGroup, esVertsPerSubgroup);

        // And then recalculate our LDS usage.
        expectedEsLdsSize = (esVertsPerSubgroup * esGsRingItemSize) + esExtraLdsSize;
        expectedGsLdsSize = (gsPrimsPerSubgroup * gsInstanceCount * gsVsRingItemSize) + gsExtraLdsSize;
        ldsSizeDwords = alignTo(expectedEsLdsSize + expectedGsLdsSize, ldsSizeDwordGranularity);
      }

      // Make sure we don't allocate more than what can legally be allocated by a single subgroup on the hardware.
      assert(ldsSizeDwords <= maxHwGsLdsSizeDwords);

      // Make sure that we have at least one primitive
      assert(gsPrimsPerSubgroup >= 1);

      // HW has restriction on NGG + Tessellation where gsPrimsPerSubgroup must be >= 3 unless we enable
      // EN_MAX_VERT_OUT_PER_GS_INSTANCE.
      assert(!hasTs || enableMaxVertOut || gsPrimsPerSubgroup >= 3);

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
      gsResUsage->inOutUsage.gs.calcFactor.rayQueryLdsStackSize = rayQueryLdsStackSize;

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

      // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
      // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
      // restriction by providing the capability of querying thread ID in the group rather than in wave.
      auto esResUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

      unsigned rayQueryLdsStackSize = 0;
      if (esResUsage->useRayQueryLdsStack || gsResUsage->useRayQueryLdsStack)
        rayQueryLdsStackSize = MaxRayQueryLdsStackEntries * MaxRayQueryThreadsPerGroup;

      // Use the client-specified amount of LDS space per subgroup. If they specified zero, they want us to
      // choose a reasonable default. The final amount must be 128-dword aligned.
      // TODO: Accept DefaultLdsSizePerSubgroup from panel setting
      unsigned maxLdsSize = Gfx9::DefaultLdsSizePerSubgroup;
      maxLdsSize -= rayQueryLdsStackSize; // Exclude LDS space used as ray query stack

      // If total LDS usage is too big, refactor partitions based on ratio of ES-GS item sizes.
      if (gsOnChipLdsSize > maxLdsSize) {
        // Our target GS primitives per subgroup was too large

        // Calculate the maximum number of GS primitives per subgroup that will fit into LDS, capped
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

      // Vertices for adjacency primitives are not always reused (e.g. in the case of shadow volumes). According
      // to hardware engineers, we must restore esMinVertsPerSubgroup for ES_VERTS_PER_SUBGRP.
      if (useAdjacency)
        esMinVertsPerSubgroup = inVertsPerPrim;

      // For normal primitives, the VGT only checks if they are past the ES verts per sub group after allocating
      // a full GS primitive and if they are, kick off a new sub group.  But if those additional ES verts are
      // unique (e.g. not reused) we need to make sure there is enough LDS space to account for those ES verts
      // beyond ES_VERTS_PER_SUBGRP.
      esVertsPerSubgroup -= (esMinVertsPerSubgroup - 1);

      // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
      // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
      // restriction by providing the capability of querying thread ID in the group rather than in wave.
      if (esResUsage->useRayQueryLdsStack)
        esVertsPerSubgroup = std::min(esVertsPerSubgroup, MaxRayQueryThreadsPerGroup);
      if (gsResUsage->useRayQueryLdsStack)
        gsPrimsPerSubgroup = std::min(gsPrimsPerSubgroup, MaxRayQueryThreadsPerGroup);

      gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsPerSubgroup;
      gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize = esGsLdsSize;
      gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize = gsOnChipLdsSize;
      gsResUsage->inOutUsage.gs.calcFactor.rayQueryLdsStackSize = rayQueryLdsStackSize;

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

        // NOTE: If ray query uses LDS stack, the expected max thread count in the group is 64. And we force wave size
        // to be 64 in order to keep all threads in the same wave. In the future, we could consider to get rid of this
        // restriction by providing the capability of querying thread ID in the group rather than in wave.
        if (esResUsage->useRayQueryLdsStack)
          esVertsNum = std::min(esVertsNum, MaxRayQueryThreadsPerGroup);
        if (gsResUsage->useRayQueryLdsStack)
          gsPrimsNum = std::min(gsPrimsNum, MaxRayQueryThreadsPerGroup);

        gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup = esVertsNum;
        gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup = gsPrimsNum;
      }
    }
  }

  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC geometry calculation factor results\n\n");
  LLPC_OUTS("ES vertices per subgroup: " << gsResUsage->inOutUsage.gs.calcFactor.esVertsPerSubgroup << "\n");
  LLPC_OUTS("GS primitives per subgroup: " << gsResUsage->inOutUsage.gs.calcFactor.gsPrimsPerSubgroup << "\n");
  LLPC_OUTS("\n");
  LLPC_OUTS("ES-GS LDS size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.esGsLdsSize << "\n");
  LLPC_OUTS("On-chip GS LDS size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize << "\n");
  LLPC_OUTS("\n");
  LLPC_OUTS("ES-GS ring item size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.esGsRingItemSize << "\n");
  LLPC_OUTS("GS-VS ring item size (in dwords): " << gsResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize << "\n");
  LLPC_OUTS("\n");

  if (hasGs) {
    LLPC_OUTS("GS stream item sizes (in dwords):\n");
    for (unsigned i = 0; i < MaxGsStreams; ++i) {
      unsigned streamItemSize = gsResUsage->inOutUsage.gs.outLocCount[i] * geometryMode.outputVertices * 4;
      LLPC_OUTS("    stream[" << i << "] = " << streamItemSize);

      if (m_pipelineState->enableXfb()) {
        const auto &streamXfbBuffers = m_pipelineState->getStreamXfbBuffers();
        LLPC_OUTS(", XFB buffers = { ");
        if (streamXfbBuffers[i] != 0) {
          for (unsigned j = 0; j < MaxTransformFeedbackBuffers; ++j) {
            if ((streamXfbBuffers[i] & (1 << j)) != 0)
              LLPC_OUTS(j << " ");
          }
        }
        LLPC_OUTS("}");
      }

      LLPC_OUTS("\n");
    }
    LLPC_OUTS("\n");
  }

  if (gsOnChip || m_pipelineState->getTargetInfo().getGfxIpVersion().major >= 9) {
    if (gsResUsage->inOutUsage.gs.calcFactor.rayQueryLdsStackSize > 0) {
      LLPC_OUTS("Ray query LDS stack size (in dwords): "
                << gsResUsage->inOutUsage.gs.calcFactor.rayQueryLdsStackSize
                << " (start = " << gsResUsage->inOutUsage.gs.calcFactor.gsOnChipLdsSize << ")\n\n");
    }

    if (meshPipeline) {
      LLPC_OUTS("GS primitive amplification factor: " << gsResUsage->inOutUsage.gs.calcFactor.primAmpFactor << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("GS is on-chip (Mesh)\n");
    } else if (m_pipelineState->getNggControl()->enableNgg) {
      LLPC_OUTS("GS primitive amplification factor: " << gsResUsage->inOutUsage.gs.calcFactor.primAmpFactor << "\n");
      LLPC_OUTS("GS enable max output vertices per instance: "
                << (gsResUsage->inOutUsage.gs.calcFactor.enableMaxVertOut ? "true" : "false") << "\n");
      LLPC_OUTS("\n");
      LLPC_OUTS("GS is on-chip (NGG)\n");
    } else {
      LLPC_OUTS("GS is " << (gsOnChip ? "on-chip" : "off-chip") << "\n");
    }
  } else
    LLPC_OUTS("GS is off-chip\n");
  LLPC_OUTS("\n");

  return gsOnChip;
}

// =====================================================================================================================
// Process a single shader.
void PatchResourceCollect::processShader() {
  m_resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  // Invoke handling of "call" instruction
  visit(m_entryPoint);

  clearInactiveBuiltInInput();
  clearInactiveBuiltInOutput();
  clearUndefinedOutput();

  if (m_pipelineState->isGraphics()) {
    matchGenericInOut();
    mapBuiltInToGenericInOut();
  }

  if (m_shaderStage == ShaderStageFragment) {
    if (m_pipelineState->getRasterizerState().perSampleShading) {
      if (m_resUsage->builtInUsage.fs.fragCoord || m_resUsage->builtInUsage.fs.pointCoord ||
          m_resUsage->builtInUsage.fs.sampleMaskIn || m_resUsage->resourceWrite)
        m_resUsage->builtInUsage.fs.runAtSampleRate = true;
    }

    // If we're compiling a fragment shader only, then serialize inputLocInfoMap and builtInInputLocMap
    // into PAL metadata, for the other half of the pipeline to be compiled against later.
    if (m_pipelineState->getShaderStageMask() == 1U << ShaderStageFragment) {
      FsInputMappings fsInputMappings = {};
      for (const auto &it : m_resUsage->inOutUsage.inputLocInfoMap)
        fsInputMappings.locationInfo.push_back({it.first.getData(), it.second.getData()});
      for (auto it : m_resUsage->inOutUsage.builtInInputLocMap)
        fsInputMappings.builtInLocationInfo.push_back({it.first, it.second});
      fsInputMappings.clipDistanceCount = m_resUsage->builtInUsage.fs.clipDistance;
      fsInputMappings.cullDistanceCount = m_resUsage->builtInUsage.fs.cullDistance;
      m_pipelineState->getPalMetadata()->addFragmentInputInfo(fsInputMappings);
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
// Process missing fragment shader. This happens in a part-pipeline compile; we deserialize the FS's input mappings
// from PAL metadata that came from the separate FS compilation.
void PatchResourceCollect::processMissingFs() {
  assert(m_shaderStage == ShaderStageFragment);
  if (!m_processMissingFs)
    return;
  m_resUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage);

  FsInputMappings fsInputMappings = {};
  m_pipelineState->getPalMetadata()->retrieveFragmentInputInfo(fsInputMappings);
  // Deserialize generic inputs. We deserialize into both m_locationInfoMapManager and inputLocInfoMap as
  // later code seems to use both places to look up a mapping.
  m_locationInfoMapManager->deserializeMap(fsInputMappings.locationInfo);
  for (std::pair<unsigned, unsigned> oneLocInfo : fsInputMappings.locationInfo)
    m_resUsage->inOutUsage.inputLocInfoMap[oneLocInfo.first] = oneLocInfo.second;
  // Deserialize built-ins as generic inputs. We also need to set FS usage flags that are used by the last
  // pre-rasterization stage.
  for (std::pair<unsigned, unsigned> oneLocInfo : fsInputMappings.builtInLocationInfo) {
    m_resUsage->inOutUsage.builtInInputLocMap[oneLocInfo.first] = oneLocInfo.second;
    switch (oneLocInfo.first) {
    case BuiltInPrimitiveId:
      m_resUsage->builtInUsage.fs.primitiveId = true;
      break;
    case BuiltInLayer:
      m_resUsage->builtInUsage.fs.layer = true;
      break;
    case BuiltInViewportIndex:
      m_resUsage->builtInUsage.fs.viewportIndex = true;
      break;
    default:
      break;
    }
  }
  m_resUsage->builtInUsage.fs.clipDistance = fsInputMappings.clipDistanceCount;
  m_resUsage->builtInUsage.fs.cullDistance = fsInputMappings.cullDistanceCount;
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
// Check if ray query LDS stack usage.
//
// @param module : LLVM module
void PatchResourceCollect::checkRayQueryLdsStackUsage(Module *module) {
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major < 10)
    return; // Must be GFX10+

  auto ldsStack = module->getNamedGlobal(RayQueryLdsStackName);
  if (ldsStack) {
    for (auto user : ldsStack->users()) {
      auto inst = cast<Instruction>(user);
      assert(inst);

      auto shaderStage = lgc::getShaderStage(inst->getFunction());
      if (shaderStage != ShaderStageInvalid)
        m_pipelineState->getShaderResourceUsage(shaderStage)->useRayQueryLdsStack = true;
    }
  }
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

  if (isa<InputImportGenericOp>(callInst) || isa<InputImportInterpolatedOp>(callInst)) {
    if (isDeadCall)
      m_deadCalls.push_back(&callInst);
    else
      m_inputCalls.push_back(cast<GenericLocationOp>(&callInst));
  } else if (mangledName.startswith(lgcName::InputImportBuiltIn)) {
    // Built-in input import
    if (isDeadCall)
      m_deadCalls.push_back(&callInst);
    else {
      unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
      m_activeInputBuiltIns.insert(builtInId);
    }
  } else if (auto *outputImport = dyn_cast<OutputImportGenericOp>(&callInst)) {
    // Generic output import
    assert(m_shaderStage == ShaderStageTessControl);
    auto outputTy = outputImport->getType();
    assert(outputTy->isSingleValueType());
    (void)(outputTy);
    m_importedOutputCalls.push_back(outputImport);
  } else if (mangledName.startswith(lgcName::OutputImportBuiltIn)) {
    // Built-in output import
    assert(m_shaderStage == ShaderStageTessControl);
    unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
    m_importedOutputBuiltIns.insert(builtInId);
  } else if (mangledName.startswith(lgcName::OutputExportGeneric)) {
    m_outputCalls.push_back(&callInst);
  } else if (mangledName.startswith(lgcName::OutputExportBuiltIn)) {
    // NOTE: If an output value is unspecified, we can safely drop it and remove the output export call.
    // Currently, do this for geometry shader.
    if (m_shaderStage == ShaderStageGeometry) {
      auto outputValue = callInst.getArgOperand(callInst.arg_size() - 1);
      if (isa<UndefValue>(outputValue) || isa<PoisonValue>(outputValue))
        m_deadCalls.push_back(&callInst);
      else {
        unsigned builtInId = cast<ConstantInt>(callInst.getOperand(0))->getZExtValue();
        m_activeOutputBuiltIns.insert(builtInId);
      }
    }
  } else if (mangledName.startswith(lgcName::OutputExportXfb)) {
    auto outputValue = callInst.getArgOperand(callInst.arg_size() - 1);
    if (isa<UndefValue>(outputValue) || isa<PoisonValue>(outputValue)) {
      // NOTE: If an output value is unspecified, we can safely drop it and remove the transform feedback export call.
      m_deadCalls.push_back(&callInst);
    } else if (m_pipelineState->enableSwXfb()) {
      // Collect transform feedback export calls, used in SW-emulated stream-out. For GS, the collecting will
      // be done when we generate copy shader since GS is primitive-based.
      if (m_shaderStage != ShaderStageGeometry) {
        auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
        // A transform feedback export call is expected to be <4 x dword> at most
        inOutUsage.xfbExpCount += outputValue->getType()->getPrimitiveSizeInBits() > 128 ? 2 : 1;
      }
    }
  }
}

// =====================================================================================================================
// Clears inactive (those actually unused) inputs.
void PatchResourceCollect::clearInactiveBuiltInInput() {
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

    if (builtInUsage.tcs.viewIndex && m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end())
      builtInUsage.tcs.viewIndex = false;
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

    if (builtInUsage.tes.viewIndex && m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end())
      builtInUsage.tes.viewIndex = false;
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

    if (builtInUsage.gs.viewIndex && m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end())
      builtInUsage.gs.viewIndex = false;
  } else if (m_shaderStage == ShaderStageMesh) {
    if (builtInUsage.mesh.drawIndex && m_activeInputBuiltIns.find(BuiltInDrawIndex) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.drawIndex = false;

    if (builtInUsage.mesh.viewIndex && m_activeInputBuiltIns.find(BuiltInViewIndex) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.viewIndex = false;

    if (builtInUsage.mesh.numWorkgroups &&
        m_activeInputBuiltIns.find(BuiltInNumWorkgroups) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.numWorkgroups = false;

    if (builtInUsage.mesh.workgroupId && m_activeInputBuiltIns.find(BuiltInWorkgroupId) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.workgroupId = false;

    if (builtInUsage.mesh.localInvocationId &&
        m_activeInputBuiltIns.find(BuiltInLocalInvocationId) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.localInvocationId = false;

    if (builtInUsage.mesh.globalInvocationId &&
        m_activeInputBuiltIns.find(BuiltInGlobalInvocationId) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.globalInvocationId = false;

    if (builtInUsage.mesh.localInvocationIndex &&
        m_activeInputBuiltIns.find(BuiltInLocalInvocationIndex) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.localInvocationIndex = false;

    if (builtInUsage.mesh.subgroupId && m_activeInputBuiltIns.find(BuiltInSubgroupId) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.subgroupId = false;

    if (builtInUsage.mesh.numSubgroups &&
        m_activeInputBuiltIns.find(BuiltInNumSubgroups) == m_activeInputBuiltIns.end())
      builtInUsage.mesh.numSubgroups = false;
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

    if (builtInUsage.fs.baryCoord && m_activeInputBuiltIns.find(BuiltInBaryCoord) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoord = false;

    if (builtInUsage.fs.baryCoordNoPerspKHR &&
        m_activeInputBuiltIns.find(BuiltInBaryCoordNoPerspKHR) == m_activeInputBuiltIns.end())
      builtInUsage.fs.baryCoordNoPerspKHR = false;

    // BaryCoord depends on PrimitiveID
    if (builtInUsage.fs.primitiveId && !(builtInUsage.fs.baryCoordNoPerspKHR || builtInUsage.fs.baryCoord) &&
        m_activeInputBuiltIns.find(BuiltInPrimitiveId) == m_activeInputBuiltIns.end())
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

    if (builtInUsage.fs.shadingRate && m_activeInputBuiltIns.find(BuiltInShadingRate) == m_activeInputBuiltIns.end())
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
void PatchResourceCollect::clearInactiveBuiltInOutput() {
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

    if (builtInUsage.primitiveShadingRate &&
        m_activeOutputBuiltIns.find(BuiltInPrimitiveShadingRate) == m_activeOutputBuiltIns.end())
      builtInUsage.primitiveShadingRate = false;
  }
}

// =====================================================================================================================
// Does generic input/output matching and does location mapping afterwards.
//
// NOTE: This function should be called after the cleanup work of inactive inputs is done.
void PatchResourceCollect::matchGenericInOut() {
  assert(m_pipelineState->isGraphics());

  // Do input matching and location remapping
  bool packInput = m_pipelineState->canPackInput(m_shaderStage);
  if (m_shaderStage == ShaderStageTessControl && m_tcsInputHasDynamicIndexing) {
    packInput = false;
    // Disable to pack VS-TCS
    m_pipelineState->setPackInput(m_shaderStage, false);
    m_pipelineState->setPackOutput(ShaderStageVertex, false);
  }
  if (packInput)
    updateInputLocInfoMapWithPack();
  else
    updateInputLocInfoMapWithUnpack();

  // Do output matching and location remapping
  bool packOutput = m_pipelineState->canPackOutput(m_shaderStage);
  if (m_shaderStage == ShaderStageVertex && m_tcsInputHasDynamicIndexing)
    assert(!packOutput);
  if (packOutput) {
    // OutputLocInfoMap is used for computing the shader hash and looking remapped location
    updateOutputLocInfoMapWithPack();
    // Re-create output export calls to pack exp instruction for the last vertex processing stage
    if (m_shaderStage == m_pipelineState->getLastVertexProcessingStage() && m_shaderStage != ShaderStageGeometry)
      reassembleOutputExportCalls();
    m_outputCalls.clear();
  } else {
    updateOutputLocInfoMapWithUnpack();
  }

  // Update location count of input/output
  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC location input/output mapping results (" << getShaderStageAbbreviation(m_shaderStage)
                                                              << " shader)\n\n");
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &inLocInfoMap = inOutUsage.inputLocInfoMap;
  auto &outLocInfoMap = inOutUsage.outputLocInfoMap;
  auto &perPatchInLocMap = inOutUsage.perPatchInputLocMap;
  auto &perPatchOutLocMap = inOutUsage.perPatchOutputLocMap;
  auto &perPrimitiveInLocMap = inOutUsage.perPrimitiveInputLocMap;
  auto &perPrimitiveOutLocMap = inOutUsage.perPrimitiveOutputLocMap;

  if (!inLocInfoMap.empty()) {
    assert(inOutUsage.inputMapLocCount == 0);
    for (const auto &locInfoPair : inLocInfoMap) {
      const unsigned origLoc = locInfoPair.first.getLocation();
      const unsigned origComp = locInfoPair.first.getComponent();
      const unsigned newLoc = locInfoPair.second.getLocation();
      const unsigned newComp = locInfoPair.second.getComponent();
      assert(newLoc != InvalidValue);
      inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, newLoc + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  loc = " << origLoc
                    << ", comp = " << origComp << " =>  Mapped = " << newLoc << ", " << newComp << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!outLocInfoMap.empty()) {
    auto &outOrigLocs = inOutUsage.fs.outputOrigLocs;
    if (m_shaderStage == ShaderStageFragment)
      memset(&outOrigLocs, InvalidValue, sizeof(inOutUsage.fs.outputOrigLocs));

    assert(inOutUsage.outputMapLocCount == 0);

    // Update the value of outLocMap for non pack case
    // Note: The value of outLocMap should be filled in packing code path
    for (const auto &locInfoPair : outLocInfoMap) {
      const unsigned origLoc = locInfoPair.first.getLocation();
      const unsigned origComp = locInfoPair.first.getComponent();
      const unsigned newLoc = locInfoPair.second.getLocation();
      const unsigned newComp = locInfoPair.second.getComponent();

      if (m_shaderStage == ShaderStageGeometry) {
        const unsigned streamId = locInfoPair.first.getStreamId();
        unsigned assignedLocCount = inOutUsage.gs.outLocCount[0] + inOutUsage.gs.outLocCount[1] +
                                    inOutUsage.gs.outLocCount[2] + inOutUsage.gs.outLocCount[3];

        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, assignedLocCount);
        LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: stream = " << streamId << ", "
                      << " loc = " << origLoc << ", comp = " << origComp << "  =>  Mapped = " << newLoc << ", "
                      << newComp << "\n");
      } else {
        inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, newLoc + 1);
        LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: loc = " << origLoc
                      << ", comp = " << origComp << "  =>  Mapped = " << newLoc << ", " << newComp << "\n");

        if (m_shaderStage == ShaderStageFragment)
          outOrigLocs[newLoc] = origLoc;
      }
    }
    LLPC_OUTS("\n");
  }

  if (!perPatchInLocMap.empty()) {
    assert(inOutUsage.perPatchInputMapLocCount == 0);
    for (auto locMap : perPatchInLocMap) {
      assert(m_shaderStage == ShaderStageTessEval && locMap.second != InvalidValue);
      inOutUsage.perPatchInputMapLocCount = std::max(inOutUsage.perPatchInputMapLocCount, locMap.second + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input (per-patch):  loc = " << locMap.first
                    << "  =>  Mapped = " << locMap.second << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!perPatchOutLocMap.empty()) {
    assert(inOutUsage.perPatchOutputMapLocCount == 0);
    for (auto locMap : perPatchOutLocMap) {
      assert(m_shaderStage == ShaderStageTessControl && locMap.second != InvalidValue);
      inOutUsage.perPatchOutputMapLocCount = std::max(inOutUsage.perPatchOutputMapLocCount, locMap.second + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output (per-patch): loc = " << locMap.first
                    << "  =>  Mapped = " << locMap.second << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!perPrimitiveInLocMap.empty()) {
    assert(inOutUsage.perPrimitiveInputMapLocCount == 0);
    for (auto locMap : perPrimitiveInLocMap) {
      assert(m_shaderStage == ShaderStageFragment && locMap.second != InvalidValue);
      inOutUsage.perPrimitiveInputMapLocCount = std::max(inOutUsage.perPrimitiveInputMapLocCount, locMap.second + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input (per-primitive):  loc = " << locMap.first
                    << "  =>  Mapped = " << locMap.second << "\n");
    }
    LLPC_OUTS("\n");
  }

  if (!perPrimitiveOutLocMap.empty()) {
    assert(inOutUsage.perPrimitiveOutputMapLocCount == 0);
    for (auto locMap : perPrimitiveOutLocMap) {
      assert(m_shaderStage == ShaderStageMesh && locMap.second != InvalidValue);
      inOutUsage.perPrimitiveOutputMapLocCount = std::max(inOutUsage.perPrimitiveOutputMapLocCount, locMap.second + 1);
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output (per-primitive): loc = " << locMap.first
                    << "  =>  Mapped = " << locMap.second << "\n");
    }
    LLPC_OUTS("\n");
  }

  LLPC_OUTS("// LLPC location count results (after input/output matching) \n\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  loc count = " << inOutUsage.inputMapLocCount
                << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: loc count = " << inOutUsage.outputMapLocCount
                << "\n");
  if (m_shaderStage == ShaderStageTessEval) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Input (per-patch):  loc count = " << inOutUsage.perPatchInputMapLocCount << "\n");
  }
  if (m_shaderStage == ShaderStageTessControl) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Output (per-patch): loc count = " << inOutUsage.perPatchOutputMapLocCount << "\n");
  }
  if (m_shaderStage == ShaderStageFragment) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Input (per-primitive):  loc count = " << inOutUsage.perPrimitiveInputMapLocCount << "\n");
  }
  if (m_shaderStage == ShaderStageMesh) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Output (per-primitive): loc count = " << inOutUsage.perPrimitiveOutputMapLocCount << "\n");
  }
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
      } else {
        builtInUsage.vs.position = false;
      }

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.vs.pointSize = false;
      }

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else {
        builtInUsage.vs.clipDistance = 0;
      }

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else {
        builtInUsage.vs.cullDistance = 0;
      }

      if (nextBuiltInUsage.layerIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.vs.layer = false;
      }

      if (nextBuiltInUsage.viewportIndexIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.vs.viewportIndex = false;
      }

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
      } else {
        builtInUsage.vs.position = false;
      }

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.vs.pointSize = false;
      }

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else {
        builtInUsage.vs.clipDistance = 0;
      }

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else {
        builtInUsage.vs.cullDistance = 0;
      }

      if (nextBuiltInUsage.layerIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.vs.layer = 0;
      }

      if (nextBuiltInUsage.viewportIndexIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.vs.viewportIndex = 0;
      }

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

    if (builtInUsage.tcs.layerIn)
      inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;

    if (builtInUsage.tcs.viewportIndexIn)
      inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;

    // Map built-in outputs to generic ones
    if (nextStage == ShaderStageTessEval) {
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.tes;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      // NOTE: For tessellation control shader, those built-in outputs that involve in output import have to
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

      if (nextBuiltInUsage.layerIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      }

      if (nextBuiltInUsage.viewportIndexIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      }

      // NOTE: We shouldn't clear the usage of tessellation levels if the next stage doesn't read them back because they
      // are always required to be written to TF buffer.
      if (nextBuiltInUsage.tessLevelOuter) {
        assert(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelOuter) !=
               nextInOutUsage.perPatchBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelOuter];
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelOuter] = mapLoc;
        availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
      }

      if (nextBuiltInUsage.tessLevelInner) {
        assert(nextInOutUsage.perPatchBuiltInInputLocMap.find(BuiltInTessLevelInner) !=
               nextInOutUsage.perPatchBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPatchBuiltInInputLocMap[BuiltInTessLevelInner];
        inOutUsage.perPatchBuiltInOutputLocMap[BuiltInTessLevelInner] = mapLoc;
        availPerPatchOutMapLoc = std::max(availPerPatchOutMapLoc, mapLoc + 1);
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

      if (builtInUsage.tcs.layerIn)
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = availOutMapLoc++;

      if (builtInUsage.tcs.viewportIndexIn)
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = availOutMapLoc++;

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

    if (builtInUsage.tes.layerIn)
      inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;

    if (builtInUsage.tes.viewportIndexIn)
      inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;

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
    } else if (nextStage == ShaderStageGeometry) {
      // TES  ==>  GS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.gs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.positionIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPosition) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPosition];
        inOutUsage.builtInOutputLocMap[BuiltInPosition] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.tes.position = false;
      }

      if (nextBuiltInUsage.pointSizeIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInPointSize) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInPointSize];
        inOutUsage.builtInOutputLocMap[BuiltInPointSize] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.tes.pointSize = false;
      }

      if (nextBuiltInUsage.clipDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.clipDistanceIn > 4 ? 2u : 1u));
      } else {
        builtInUsage.tes.clipDistance = 0;
      }

      if (nextBuiltInUsage.cullDistanceIn > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + (nextBuiltInUsage.cullDistanceIn > 4 ? 2u : 1u));
      } else {
        builtInUsage.tes.cullDistance = 0;
      }

      if (nextBuiltInUsage.layerIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInLayer) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInLayer];
        inOutUsage.builtInOutputLocMap[BuiltInLayer] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.tes.layer = 0;
      }

      if (nextBuiltInUsage.viewportIndexIn) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInViewportIndex) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInViewportIndex];
        inOutUsage.builtInOutputLocMap[BuiltInViewportIndex] = mapLoc;
        availOutMapLoc = std::max(availOutMapLoc, mapLoc + 1);
      } else {
        builtInUsage.tes.viewportIndex = 0;
      }
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

    if (builtInUsage.gs.layerIn)
      inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;

    if (builtInUsage.gs.viewportIndexIn)
      inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;

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

    if (builtInUsage.gs.viewportIndex)
      mapGsBuiltInOutput(BuiltInViewportIndex, 1);

    if (m_pipelineState->getInputAssemblyState().enableMultiView)
      mapGsBuiltInOutput(BuiltInViewIndex, 1);

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
    }

    inOutUsage.inputMapLocCount = std::max(inOutUsage.inputMapLocCount, availInMapLoc);
  } else if (m_shaderStage == ShaderStageMesh) {
    // Mesh shader -> XXX
    unsigned availOutMapLoc = inOutUsage.outputMapLocCount;
    unsigned availPerPrimitiveOutMapLoc = inOutUsage.perPrimitiveOutputMapLocCount;

    // Map per-vertex built-in outputs to generic ones
    if (builtInUsage.mesh.position)
      inOutUsage.builtInOutputLocMap[BuiltInPosition] = availOutMapLoc++;

    if (builtInUsage.mesh.pointSize)
      inOutUsage.builtInOutputLocMap[BuiltInPointSize] = availOutMapLoc++;

    if (builtInUsage.mesh.clipDistance > 0) {
      inOutUsage.builtInOutputLocMap[BuiltInClipDistance] = availOutMapLoc++;
      if (builtInUsage.mesh.clipDistance > 4)
        ++availOutMapLoc;
    }

    if (builtInUsage.mesh.cullDistance > 0) {
      inOutUsage.builtInOutputLocMap[BuiltInCullDistance] = availOutMapLoc++;
      if (builtInUsage.mesh.cullDistance > 4)
        ++availOutMapLoc;
    }

    // Map per-primitive built-in outputs to generic ones
    if (builtInUsage.mesh.primitiveId)
      inOutUsage.perPrimitiveBuiltInOutputLocMap[BuiltInPrimitiveId] = availPerPrimitiveOutMapLoc++;

    if (builtInUsage.mesh.viewportIndex)
      inOutUsage.perPrimitiveBuiltInOutputLocMap[BuiltInViewportIndex] = availPerPrimitiveOutMapLoc++;

    if (builtInUsage.mesh.layer)
      inOutUsage.perPrimitiveBuiltInOutputLocMap[BuiltInLayer] = availPerPrimitiveOutMapLoc++;

    if (builtInUsage.mesh.primitiveShadingRate)
      inOutUsage.perPrimitiveBuiltInOutputLocMap[BuiltInPrimitiveShadingRate] = availPerPrimitiveOutMapLoc++;

    // Map per-vertex built-in outputs to exported locations
    if (nextStage == ShaderStageFragment) {
      // Mesh shader  ==>  FS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.fs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.clipDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInClipDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInClipDistance];
        inOutUsage.mesh.builtInExportLocs[BuiltInClipDistance] = mapLoc;
      }

      if (nextBuiltInUsage.cullDistance > 0) {
        assert(nextInOutUsage.builtInInputLocMap.find(BuiltInCullDistance) != nextInOutUsage.builtInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.builtInInputLocMap[BuiltInCullDistance];
        inOutUsage.mesh.builtInExportLocs[BuiltInCullDistance] = mapLoc;
      }
    } else if (nextStage == ShaderStageInvalid) {
      // Mesh shader only
      unsigned availExportLoc = inOutUsage.outputMapLocCount;

      if (builtInUsage.mesh.clipDistance > 0 || builtInUsage.mesh.cullDistance > 0) {
        unsigned exportLoc = availExportLoc++;
        if (builtInUsage.mesh.clipDistance + builtInUsage.mesh.cullDistance > 4) {
          assert(builtInUsage.mesh.clipDistance + builtInUsage.mesh.cullDistance <= MaxClipCullDistanceCount);
          ++availExportLoc; // Occupy two locations
        }

        if (builtInUsage.mesh.clipDistance > 0)
          inOutUsage.mesh.builtInExportLocs[BuiltInClipDistance] = exportLoc;

        if (builtInUsage.mesh.cullDistance > 0) {
          if (builtInUsage.mesh.clipDistance >= 4)
            ++exportLoc;
          inOutUsage.mesh.builtInExportLocs[BuiltInCullDistance] = exportLoc;
        }
      }
    }

    // Map per-primitive built-in outputs to exported locations
    if (nextStage == ShaderStageFragment) {
      // Mesh shader  ==>  FS
      const auto &nextBuiltInUsage = nextResUsage->builtInUsage.fs;
      auto &nextInOutUsage = nextResUsage->inOutUsage;

      if (nextBuiltInUsage.primitiveId) {
        assert(nextInOutUsage.perPrimitiveBuiltInInputLocMap.find(BuiltInPrimitiveId) !=
               nextInOutUsage.perPrimitiveBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInPrimitiveId];
        inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInPrimitiveId] = mapLoc;
      }

      if (nextBuiltInUsage.layer) {
        assert(nextInOutUsage.perPrimitiveBuiltInInputLocMap.find(BuiltInLayer) !=
               nextInOutUsage.perPrimitiveBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInLayer];
        inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInLayer] = mapLoc;
      }

      if (nextBuiltInUsage.viewportIndex) {
        assert(nextInOutUsage.perPrimitiveBuiltInInputLocMap.find(BuiltInViewportIndex) !=
               nextInOutUsage.perPrimitiveBuiltInInputLocMap.end());
        const unsigned mapLoc = nextInOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInViewportIndex];
        inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInViewportIndex] = mapLoc;
      }
    } else if (nextStage == ShaderStageInvalid) {
      // Mesh shader only
      unsigned availPerPrimitiveExportLoc = inOutUsage.perPrimitiveOutputMapLocCount;

      if (builtInUsage.mesh.primitiveId)
        inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInPrimitiveId] = availPerPrimitiveExportLoc++;

      if (builtInUsage.mesh.layer)
        inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInLayer] = availPerPrimitiveExportLoc++;

      if (builtInUsage.mesh.viewportIndex)
        inOutUsage.mesh.perPrimitiveBuiltInExportLocs[BuiltInViewportIndex] = availPerPrimitiveExportLoc++;
    }

    inOutUsage.mesh.genericOutputMapLocCount = inOutUsage.outputMapLocCount;
    inOutUsage.mesh.perPrimitiveGenericOutputMapLocCount = inOutUsage.perPrimitiveOutputMapLocCount;

    inOutUsage.outputMapLocCount = std::max(inOutUsage.outputMapLocCount, availOutMapLoc);
    inOutUsage.perPrimitiveOutputMapLocCount =
        std::max(inOutUsage.perPrimitiveOutputMapLocCount, availPerPrimitiveOutMapLoc);
  } else if (m_shaderStage == ShaderStageFragment) {
    // FS
    const auto prevStage = m_pipelineState->getPrevShaderStage(m_shaderStage);
    unsigned availInMapLoc = inOutUsage.inputMapLocCount;
    unsigned availPerPrimitiveInMapLoc = inOutUsage.perPrimitiveInputMapLocCount;

    if (builtInUsage.fs.pointCoord)
      inOutUsage.builtInInputLocMap[BuiltInPointCoord] = availInMapLoc++;

    if (builtInUsage.fs.primitiveId) {
      if (prevStage == ShaderStageMesh)
        inOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInPrimitiveId] = availPerPrimitiveInMapLoc++;
      else
        inOutUsage.builtInInputLocMap[BuiltInPrimitiveId] = availInMapLoc++;
    }

    if (builtInUsage.fs.layer) {
      if (prevStage == ShaderStageMesh)
        inOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInLayer] = availPerPrimitiveInMapLoc++;
      else
        inOutUsage.builtInInputLocMap[BuiltInLayer] = availInMapLoc++;
    }

    if (builtInUsage.fs.viewportIndex) {
      if (prevStage == ShaderStageMesh)
        inOutUsage.perPrimitiveBuiltInInputLocMap[BuiltInViewportIndex] = availPerPrimitiveInMapLoc++;
      else
        inOutUsage.builtInInputLocMap[BuiltInViewportIndex] = availInMapLoc++;
    }

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
    inOutUsage.perPrimitiveInputMapLocCount =
        std::max(inOutUsage.perPrimitiveInputMapLocCount, availPerPrimitiveInMapLoc);
  }

  // Do builtin-to-generic mapping
  LLPC_OUTS("===============================================================================\n");
  LLPC_OUTS("// LLPC builtin-to-generic mapping results (" << getShaderStageAbbreviation(m_shaderStage)
                                                           << " shader)\n\n");
  for (const auto &builtInMap : inOutUsage.builtInInputLocMap) {
    const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
    const unsigned loc = builtInMap.second;
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  builtin = "
                  << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
  }
  if (!inOutUsage.builtInInputLocMap.empty())
    LLPC_OUTS("\n");

  for (const auto &builtInMap : inOutUsage.builtInOutputLocMap) {
    const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
    const unsigned loc = builtInMap.second;

    if (m_shaderStage == ShaderStageGeometry) {
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                    << ") Output: stream = " << m_pipelineState->getRasterizerState().rasterStream << " , "
                    << "builtin = " << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
    } else {
      LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: builtin = "
                    << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
    }
  }
  if (!inOutUsage.builtInOutputLocMap.empty())
    LLPC_OUTS("\n");

  for (const auto &builtInMap : inOutUsage.perPatchBuiltInInputLocMap) {
    const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
    const unsigned loc = builtInMap.second;
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input (per-patch):  builtin = "
                  << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
  }
  if (!inOutUsage.perPatchBuiltInInputLocMap.empty())
    LLPC_OUTS("\n");

  for (const auto &builtInMap : inOutUsage.perPatchBuiltInOutputLocMap) {
    const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
    const unsigned loc = builtInMap.second;
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output (per-patch): builtin = "
                  << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
  }
  if (!inOutUsage.perPatchBuiltInOutputLocMap.empty())
    LLPC_OUTS("\n");

  for (const auto &builtInMap : inOutUsage.perPrimitiveBuiltInInputLocMap) {
    const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
    const unsigned loc = builtInMap.second;
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input (per-primitive):  builtin = "
                  << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
  }
  if (!inOutUsage.perPrimitiveBuiltInInputLocMap.empty())
    LLPC_OUTS("\n");

  for (const auto &builtInMap : inOutUsage.perPrimitiveBuiltInOutputLocMap) {
    const BuiltInKind builtInId = static_cast<BuiltInKind>(builtInMap.first);
    const unsigned loc = builtInMap.second;
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output (per-primitive): builtin = "
                  << PipelineState::getBuiltInName(builtInId) << "  =>  Mapped = " << loc << "\n");
  }
  if (!inOutUsage.perPrimitiveBuiltInOutputLocMap.empty())
    LLPC_OUTS("\n");

  LLPC_OUTS("// LLPC location count results (after builtin-to-generic mapping)\n\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Input:  loc count = " << inOutUsage.inputMapLocCount
                << "\n");
  LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage) << ") Output: loc count = " << inOutUsage.outputMapLocCount
                << "\n");
  if (m_shaderStage == ShaderStageTessEval) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Input (per-patch):  loc count = " << inOutUsage.perPatchInputMapLocCount << "\n");
  }
  if (m_shaderStage == ShaderStageTessControl) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Output (per-patch): loc count = " << inOutUsage.perPatchOutputMapLocCount << "\n");
  }
  if (m_shaderStage == ShaderStageFragment) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Input (per-primitive):  loc count = " << inOutUsage.perPrimitiveInputMapLocCount << "\n");
  }
  if (m_shaderStage == ShaderStageMesh) {
    LLPC_OUTS("(" << getShaderStageAbbreviation(m_shaderStage)
                  << ") Output (per-primitive): loc count = " << inOutUsage.perPrimitiveOutputMapLocCount << "\n");
  }
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
  unsigned streamId = m_pipelineState->getRasterizerState().rasterStream;

  resUsage->inOutUsage.builtInOutputLocMap[builtInId] = inOutUsage.outLocCount[streamId]++;

  if (elemCount > 4)
    inOutUsage.outLocCount[streamId]++;

  unsigned assignedLocCount =
      inOutUsage.outLocCount[0] + inOutUsage.outLocCount[1] + inOutUsage.outLocCount[2] + inOutUsage.outLocCount[3];

  resUsage->inOutUsage.outputMapLocCount = std::max(resUsage->inOutUsage.outputMapLocCount, assignedLocCount);
}

// =====================================================================================================================
// Update the inputLocInfoutputoMap, perPatchInputLocMap and perPrimitiveInputLocMap
void PatchResourceCollect::updateInputLocInfoMapWithUnpack() {
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &inputLocInfoMap = inOutUsage.inputLocInfoMap;
  // Remove unused locationInfo
  bool eraseUnusedLocInfo = !m_pipelineState->isUnlinked(); // Should be whole pipeline compilation
  if (m_shaderStage == ShaderStageTessEval) {
    // TODO: Here, we keep all generic inputs of tessellation evaluation shader. This is because corresponding
    // generic outputs of tessellation control shader might involve in output import and dynamic indexing, which
    // is easy to cause incorrectness of location mapping.
    // m_inputCalls holds the calls that have users
    eraseUnusedLocInfo = false;
  } else if (m_shaderStage == ShaderStageFragment) {
    // NOTE: If the previous stage of fragment shader is mesh shader, we skip this because the input/output packing
    // is disable between mesh shader and fragment shader.
    auto prevStage = m_pipelineState->getPrevShaderStage(ShaderStageFragment);
    if (prevStage == ShaderStageMesh) {
      eraseUnusedLocInfo = false;
    }
  } else if (m_shaderStage == ShaderStageTessControl) {
    // NOTE: If location offset or element index (64-bit element type) is dynamic, we keep all generic inputs of TCS.
    for (auto call : m_inputCalls) {
      auto locOffset = call->getLocOffset();
      if (!isa<ConstantInt>(locOffset)) {
        eraseUnusedLocInfo = false;
        break;
      }
      auto bitWidth = call->getType()->getScalarSizeInBits();
      if (bitWidth == 64) {
        auto elemIdx = call->getElemIdx();
        if (!isa<ConstantInt>(elemIdx)) {
          eraseUnusedLocInfo = false;
          break;
        }
      }
    }
  }

  if (eraseUnusedLocInfo) {
    // Collect active locations
    DenseSet<unsigned> activeLocs;
    for (auto call : m_inputCalls) {
      const unsigned loc = call->getLocation();
      activeLocs.insert(loc);
      auto bitWidth = call->getType()->getPrimitiveSizeInBits();
      if (bitWidth > (8 * SizeOfVec4)) {
        assert(bitWidth <= (8 * 2 * SizeOfVec4));
        activeLocs.insert(loc + 1);
      }
    }
    // Clear per-vertex generic inputs
    auto &locInfoMap = m_resUsage->inOutUsage.inputLocInfoMap;
    for (auto iter = locInfoMap.begin(); iter != locInfoMap.end();) {
      auto curIter = iter++;
      if (activeLocs.count(curIter->first.getLocation()) == 0)
        locInfoMap.erase(curIter);
    }

    // clear per-patch inputs
    auto &perPatchLocMap = m_resUsage->inOutUsage.perPatchInputLocMap;
    for (auto iter = perPatchLocMap.begin(); iter != perPatchLocMap.end();) {
      auto curIter = iter++;
      if (activeLocs.count(curIter->first) == 0)
        perPatchLocMap.erase(curIter);
    }

    // Clear per-primitive inputs
    auto &perPrimitiveLocMap = m_resUsage->inOutUsage.perPrimitiveInputLocMap;
    for (auto iter = perPrimitiveLocMap.begin(); iter != perPrimitiveLocMap.end();) {
      auto curIter = iter++;
      if (activeLocs.count(curIter->first) == 0)
        perPrimitiveLocMap.erase(curIter);
    }
  }

  // Special processing for TES/Mesh inputLocInfoMap and TES prePatchInputLocMap as their output location offset can be
  // dynamic. The dynamic location offset is marked with non-invalid value in the output map. We should keep the
  // corresponding input location in the next stage. For example, if TCS output has dynamic location indexing from
  // [0,2], we need add the corresponding location info to TES input map. Otherwise, it will cause mismatch when the
  // dynamic indexing is in a loop and TES only uses location 1.
  auto preStage = m_pipelineState->getPrevShaderStage(m_shaderStage);
  if (preStage == ShaderStageTessControl || preStage == ShaderStageMesh) {
    if (!inputLocInfoMap.empty()) {
      auto &outputLocInfoMap = m_pipelineState->getShaderResourceUsage(preStage)->inOutUsage.outputLocInfoMap;
      for (auto &infoPair : outputLocInfoMap) {
        if (infoPair.second != InvalidValue) {
          inputLocInfoMap[infoPair.first] = InvalidValue;
          infoPair.second = InvalidValue;
        }
      }
    }
    auto &perPatchInLocMap = inOutUsage.perPatchInputLocMap;
    if (!perPatchInLocMap.empty()) {
      auto &perPatchOutLocMap = m_pipelineState->getShaderResourceUsage(preStage)->inOutUsage.perPatchOutputLocMap;
      for (auto &locPair : perPatchOutLocMap) {
        if (locPair.second != InvalidValue) {
          perPatchInLocMap[locPair.first] = InvalidValue;
          locPair.second = InvalidValue;
        }
      }
    }
  }

  // Update the value of inputLocInfoMap
  if (!inputLocInfoMap.empty()) {
    unsigned nextMapLoc = 0;
    DenseMap<unsigned, unsigned> alreadyMappedLocs; // Map from original location to new location
    for (auto &locInfoPair : inputLocInfoMap) {
      auto &newLocationInfo = locInfoPair.second;
      if (m_shaderStage == ShaderStageVertex) {
        // NOTE: For vertex shader, use the original location as the remapped location
        newLocationInfo.setData(locInfoPair.first.getData());
      } else {
        const unsigned origLoc = locInfoPair.first.getLocation();
        unsigned mappedLoc = InvalidValue;
        // For other shaders, map the location to continuous locations
        auto locMapIt = alreadyMappedLocs.find(origLoc);
        if (locMapIt != alreadyMappedLocs.end()) {
          mappedLoc = locMapIt->second;
        } else {
          mappedLoc = nextMapLoc++;
          // NOTE: Record the map because we are handling multiple pairs of <location, component>. Some pairs have the
          // same location while the components are different.
          alreadyMappedLocs.insert({origLoc, mappedLoc});
        }

        newLocationInfo.setData(0);
        newLocationInfo.setLocation(mappedLoc);
        newLocationInfo.setComponent(locInfoPair.first.getComponent());
      }
    }
  }

  // Update the value of perPatchInputLocMap
  auto &perPatchInLocMap = inOutUsage.perPatchInputLocMap;
  if (!perPatchInLocMap.empty()) {
    unsigned nextMapLoc = 0;
    for (auto &locPair : perPatchInLocMap)
      locPair.second = nextMapLoc++;
  }

  // Update the value of perPrimitiveInputLocMap
  auto &perPrimitiveInLocMap = inOutUsage.perPrimitiveInputLocMap;
  if (!perPrimitiveInLocMap.empty()) {
    unsigned nextMapLoc = 0;
    for (auto &locPair : perPrimitiveInLocMap) {
      assert(locPair.second == InvalidValue);
      locPair.second = nextMapLoc++;
    }
  }

  m_inputCalls.clear();
}

// =====================================================================================================================
// Clear unused output from outputLocInfoMap, perPatchOutputLocMap, and perPrimitiveOutputLocMap
void PatchResourceCollect::clearUnusedOutput() {
  ShaderStage nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &outputLocInfoMap = inOutUsage.outputLocInfoMap;
  if (nextStage != ShaderStageInvalid) {
    // Collect the locations of TCS's imported outputs
    DenseSet<unsigned> importOutputLocs;
    if (m_shaderStage == ShaderStageTessControl) {
      // Imported output calls
      for (auto &outputImport : m_importedOutputCalls) {
        unsigned loc = outputImport->getLocation();
        Value *const locOffset = outputImport->getLocOffset();
        Value *const compIdx = outputImport->getElemIdx();
        importOutputLocs.insert(loc);
        // Location offset and component index are both constant
        if (isa<ConstantInt>(locOffset) && isa<ConstantInt>(compIdx)) {
          loc += cast<ConstantInt>(locOffset)->getZExtValue();
          auto bitWidth = outputImport->getType()->getScalarSizeInBits();
          if (bitWidth == 64 && cast<ConstantInt>(compIdx)->getZExtValue() >= 2) {
            // NOTE: For the addressing of .z/.w component of 64-bit vector/scalar, the count of
            // occupied locations are two.
            importOutputLocs.insert(loc + 1);
          }
        }
      }
    }

    // Do normal input/output matching
    SmallVector<InOutLocationInfo, 4> unusedLocInfos;
    auto nextResUsage = m_pipelineState->getShaderResourceUsage(nextStage);
    const auto &nextInLocInfoMap = nextResUsage->inOutUsage.inputLocInfoMap;
    unsigned availInMapLoc = nextResUsage->inOutUsage.inputMapLocCount;

    for (auto &locInfoPair : outputLocInfoMap) {
      const unsigned origLoc = locInfoPair.first.getLocation();
      if (m_shaderStage == ShaderStageFragment) {
        // Collect locations with invalid data format
        const bool generatingColorExportShader =
            m_pipelineState->isUnlinked() && !m_pipelineState->hasColorExportFormats();
        if (!generatingColorExportShader && m_pipelineState->getColorExportFormat(origLoc).dfmt == BufDataFormatInvalid)
          unusedLocInfos.push_back(locInfoPair.first);
      } else {
        // Collect locations of those outputs that are not used
        bool isOutputXfb = false;
        bool foundInNextStage = false;

        if (m_shaderStage == ShaderStageGeometry) {
          isOutputXfb = inOutUsage.locInfoXfbOutInfoMap.count(locInfoPair.first) > 0;

          auto locInfo = locInfoPair.first;
          if (m_pipelineState->getRasterizerState().rasterStream == locInfo.getStreamId()) {
            // StreamId only valid in GS stage.
            locInfo.setStreamId(0);
            foundInNextStage = (nextInLocInfoMap.find(locInfo) != nextInLocInfoMap.end());
          }
        } else
          foundInNextStage = (nextInLocInfoMap.find(locInfoPair.first) != nextInLocInfoMap.end());

        if (!isOutputXfb && !foundInNextStage) {
          bool isActiveLoc = false;
          if (m_shaderStage == ShaderStageTessControl) {
            // NOTE: if the output is used as imported in TCS, it is marked as active.
            isActiveLoc = importOutputLocs.find(origLoc) != importOutputLocs.end();
          }
          if (isActiveLoc) {
            // The assigned location must not overlap with those used by inputs of next shader stage.
            auto &newLocationInfo = locInfoPair.second;
            newLocationInfo.setData(0);
            newLocationInfo.setLocation(availInMapLoc++);
          } else {
            unusedLocInfos.push_back(locInfoPair.first);
          }
        }
      }
    }
    // Remove those collected InOutLocationInfos
    for (auto &locInfo : unusedLocInfos)
      outputLocInfoMap.erase(locInfo);

    // Do per-patch input/output matching
    if (m_shaderStage == ShaderStageTessControl) {
      auto &perPatchOutputLocMap = inOutUsage.perPatchOutputLocMap;
      const auto &nextPerPatchInLocMap = nextResUsage->inOutUsage.perPatchInputLocMap;
      unsigned availPerPatchInMapLoc = nextResUsage->inOutUsage.perPatchInputMapLocCount;

      // Collect locations of those outputs that are not used by next shader stage or read by TCS
      SmallVector<unsigned, 4> unusedLocs;
      for (auto &locPair : perPatchOutputLocMap) {
        const unsigned loc = locPair.first;
        if (nextPerPatchInLocMap.find(loc) == nextPerPatchInLocMap.end()) {
          if (importOutputLocs.find(loc) != importOutputLocs.end())
            locPair.second = availPerPatchInMapLoc++;
          else
            unusedLocs.push_back(loc);
        }
      }
      // Remove those collected locations
      for (auto loc : unusedLocs)
        perPatchOutputLocMap.erase(loc);
    }

    // Do per-primitive input/output matching
    if (m_shaderStage == ShaderStageMesh) {
      auto &perPrimitiveOutputLocMap = inOutUsage.perPrimitiveOutputLocMap;
      const auto &nextPerPrimitiveInLocMap = nextResUsage->inOutUsage.perPrimitiveInputLocMap;
      unsigned availPerPrimitiveInMapLoc = nextResUsage->inOutUsage.perPrimitiveInputMapLocCount;

      // Collect locations of those outputs that are not used by next shader stage
      SmallVector<unsigned, 4> unusedLocs;
      for (auto &locPair : perPrimitiveOutputLocMap) {
        const unsigned loc = locPair.first;
        if (nextPerPrimitiveInLocMap.find(loc) == nextPerPrimitiveInLocMap.end()) {
          if (importOutputLocs.find(loc) != importOutputLocs.end())
            locPair.second = availPerPrimitiveInMapLoc++;
          else
            unusedLocs.push_back(loc);
        }
      }
      // Remove those collected locations
      for (auto loc : unusedLocs)
        perPrimitiveOutputLocMap.erase(loc);
    }
  }

  // Remove output of FS with invalid data format
  if (m_shaderStage == ShaderStageFragment) {
    const bool generatingColorExportShader = m_pipelineState->isUnlinked() && !m_pipelineState->hasColorExportFormats();
    for (auto locInfoMapIt = outputLocInfoMap.begin(); locInfoMapIt != outputLocInfoMap.end();) {
      const unsigned origLoc = locInfoMapIt->first.getLocation();
      if (!generatingColorExportShader && m_pipelineState->getColorExportFormat(origLoc).dfmt == BufDataFormatInvalid)
        locInfoMapIt = outputLocInfoMap.erase(locInfoMapIt);
      else
        ++locInfoMapIt;
    }
  }
}

// =====================================================================================================================
// Update the outputLocInfoMap, perPatchOutputLocMap, and perPrimitiveOutputLocMap
void PatchResourceCollect::updateOutputLocInfoMapWithUnpack() {
  clearUnusedOutput();

  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &outputLocInfoMap = inOutUsage.outputLocInfoMap;
  auto &perPatchOutputLocMap = inOutUsage.perPatchOutputLocMap;
  auto &perPrimitiveOutputLocMap = inOutUsage.perPrimitiveOutputLocMap;

  // Update the value of outputLocInfoMap
  if (!outputLocInfoMap.empty()) {
    unsigned nextMapLoc[MaxGsStreams] = {};
    DenseMap<unsigned, unsigned> alreadyMappedLocs[MaxGsStreams]; // Map from original location to new location

    for (auto &locInfoPair : outputLocInfoMap) {
      const auto &locationInfo = locInfoPair.first;
      auto &newLocationInfo = locInfoPair.second;

      if (!newLocationInfo.isInvalid())
        continue; // Skip any location that is mapped

      newLocationInfo.setData(0);
      newLocationInfo.setComponent(locationInfo.getComponent());
      const unsigned streamId = m_shaderStage == ShaderStageGeometry ? locationInfo.getStreamId() : 0;
      newLocationInfo.setStreamId(streamId);

      const unsigned locToBeMapped = locationInfo.getLocation();
      unsigned mappedLoc = InvalidValue;
      const bool keepLocation = m_shaderStage == ShaderStageGeometry && !canChangeOutputLocationsForGs();
      if (keepLocation) {
        // Keep location unchanged
        mappedLoc = locToBeMapped;
      } else {
        // Map to new location
        if (alreadyMappedLocs[streamId].count(locToBeMapped) > 0) {
          mappedLoc = alreadyMappedLocs[streamId][locToBeMapped];
        } else {
          mappedLoc = nextMapLoc[streamId]++;
          // NOTE: Record the map because we are handling multiple pairs of <location, component>. Some pairs have the
          // same location while the components are different.
          alreadyMappedLocs[streamId][locToBeMapped] = mappedLoc;
        }
      }

      assert(mappedLoc != InvalidValue);
      newLocationInfo.setLocation(mappedLoc);

      if (m_shaderStage == ShaderStageGeometry)
        inOutUsage.gs.outLocCount[streamId] = std::max(inOutUsage.gs.outLocCount[streamId], mappedLoc + 1);
    }
  }

  // Update the value of perPatchOutputLocMap
  if (!perPatchOutputLocMap.empty()) {
    unsigned nextMapLoc = 0;
    for (auto &locPair : perPatchOutputLocMap) {
      if (locPair.second == InvalidValue) {
        // Only do location mapping if the per-patch output has not been mapped
        locPair.second = nextMapLoc++;
      } else
        assert(m_shaderStage == ShaderStageTessControl);
    }
  }

  // Update the value of perPrimitiveOutputLocMap
  if (!perPrimitiveOutputLocMap.empty()) {
    unsigned nextMapLoc = 0;
    for (auto &locPair : perPrimitiveOutputLocMap) {
      if (locPair.second == InvalidValue) {
        // Only do location mapping if the per-primitive output has not been mapped
        locPair.second = nextMapLoc++;
      } else
        assert(m_shaderStage == ShaderStageMesh);
    }
  }

  m_outputCalls.clear();
  m_importedOutputCalls.clear();
}

// =====================================================================================================================
// Returns true if the locations for the GS output can be compressed.
bool PatchResourceCollect::canChangeOutputLocationsForGs() {
  // The GS outputs can only be changed if LGC has access to the fragment shader's inputs.
  if (!m_pipelineState->isUnlinked())
    return true;
  if (m_pipelineState->getPalMetadata()->haveFsInputMappings())
    return true;
  if (m_pipelineState->getNextShaderStage(ShaderStageGeometry) != ShaderStageInvalid)
    return true;
  return false;
}

// =====================================================================================================================
// Update inputLocInfoMap based on {TCS, GS, FS} input import calls
void PatchResourceCollect::updateInputLocInfoMapWithPack() {
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &inputLocInfoMap = inOutUsage.inputLocInfoMap;
  inputLocInfoMap.clear();

  if (m_inputCalls.empty())
    return;

  const bool isTcs = m_shaderStage == ShaderStageTessControl;
  const bool isFs = m_shaderStage == ShaderStageFragment;
  const bool isGs = m_shaderStage == ShaderStageGeometry;
  assert(isTcs || isFs || isGs);

  // The locations of TCS with dynamic indexing (locOffset/elemIdx) cannot be unpacked
  // NOTE: Dynamic indexing in FS is processed to be constant in the lower pass.

  // LDS load/store copes with dword. For 8-bit/16-bit data type, we will extend them to 32-bit
  bool partPipelineHasGs = m_pipelineState->isPartPipeline() && m_pipelineState->getPreRasterHasGs();
  bool isFsAndHasGs = (isFs && (m_pipelineState->hasShaderStage(ShaderStageGeometry) || partPipelineHasGs));
  bool requireDword = isTcs || isGs || isFsAndHasGs;
  // Create locationMap
  m_locationInfoMapManager->createMap(m_inputCalls, m_shaderStage, requireDword);

  // Fill inputLocInfoMap of {TCS, GS, FS} for the packable calls
  unsigned newLocIdx = 0;
  for (auto input : m_inputCalls) {
    unsigned locOffset = cast<ConstantInt>(input->getLocOffset())->getZExtValue();

    // Get the packed InOutLocationInfo from locationInfoMap
    InOutLocationInfo origLocInfo;
    origLocInfo.setLocation(input->getLocation() + locOffset);
    origLocInfo.setComponent(cast<ConstantInt>(input->getElemIdx())->getZExtValue());
    InOutLocationInfoMap::const_iterator mapIter;
    assert(m_locationInfoMapManager->findMap(origLocInfo, mapIter));
    m_locationInfoMapManager->findMap(origLocInfo, mapIter);
    inputLocInfoMap[origLocInfo] = mapIter->second;
    newLocIdx = std::max(newLocIdx, mapIter->second.getLocation() + 1);
  }
  m_inputCalls.clear();
}

// =====================================================================================================================
// Update outputLocInfoMap based on inputLocInfoMap of next stage or GS output export calls for copy shader
void PatchResourceCollect::updateOutputLocInfoMapWithPack() {
  auto &inOutUsage = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage;
  auto &outputLocInfoMap = inOutUsage.outputLocInfoMap;
  outputLocInfoMap.clear();

  if (m_outputCalls.empty())
    return;

  assert(m_shaderStage == ShaderStageVertex || m_shaderStage == ShaderStageTessEval ||
         m_shaderStage == ShaderStageGeometry);
  auto nextStage = m_pipelineState->getNextShaderStage(m_shaderStage);
  assert(nextStage != ShaderStageInvalid);
  auto &nextStageInputLocInfoMap = m_pipelineState->getShaderResourceUsage(nextStage)->inOutUsage.inputLocInfoMap;

  // Remove unused outputs and update the output map
  if (m_shaderStage != m_pipelineState->getLastVertexProcessingStage()) {
    // For VS-{TCS, GS}, the dead output has no matching input of the next stage
    for (auto call : m_outputCalls) {
      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue());
      origLocInfo.setComponent(cast<ConstantInt>(call->getOperand(1))->getZExtValue());
      if (nextStageInputLocInfoMap.find(origLocInfo) == nextStageInputLocInfoMap.end())
        m_deadCalls.push_back(call);
    }
    // The output map should be equal to the input map of the next stage
    outputLocInfoMap = nextStageInputLocInfoMap;
  } else {
    // For {VS, TES, GS}-FS, the dead output is neither a XFB output or a corresponding FS' input.
    assert(nextStage == ShaderStageFragment);

    // Collect XFB locations
    auto &xfbOutLocInfoMap = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.locInfoXfbOutInfoMap;
    std::set<unsigned> xfbOutputLocs[MaxGsStreams];
    for (const auto &locInfoPair : xfbOutLocInfoMap) {
      const auto &locInfo = locInfoPair.first;
      xfbOutputLocs[locInfo.getStreamId()].insert(locInfo.getLocation());
    }

    // Store the output calls that have no corresponding input in FS
    std::vector<CallInst *> noMappedCalls;
    for (auto call : m_outputCalls) {
      // NOTE: Don't set stream ID to the original output location info for GS. This is because the corresponding input
      // location info of FS doesn't have stream ID. This will cause in-out mismatch.
      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue());
      origLocInfo.setComponent(cast<ConstantInt>(call->getOperand(1))->getZExtValue());

      const unsigned origLocation = origLocInfo.getLocation();
      const bool hasNoMappedInput = (nextStageInputLocInfoMap.find(origLocInfo) == nextStageInputLocInfoMap.end());
      if (hasNoMappedInput) {
        const unsigned streamId =
            m_shaderStage == ShaderStageGeometry ? cast<ConstantInt>(call->getOperand(2))->getZExtValue() : 0;

        if (xfbOutputLocs[streamId].count(origLocation) == 0)
          m_deadCalls.push_back(call);
        else
          noMappedCalls.push_back(call);
      }
    }
    // The output map of current stage contains at most two parts: the first part is consistent with FS input map and
    // the second part is built from the no mapped calls.
    std::vector<InOutLocationInfo> outLocInfos;
    for (auto call : noMappedCalls) {
      InOutLocationInfo origLocInfo;
      origLocInfo.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue());
      origLocInfo.setComponent(cast<ConstantInt>(call->getOperand(1))->getZExtValue());
      if (m_shaderStage == ShaderStageGeometry)
        origLocInfo.setStreamId(cast<ConstantInt>(call->getOperand(2))->getZExtValue());
      outLocInfos.push_back(origLocInfo);
    }
    m_locationInfoMapManager->createMap(outLocInfos, m_shaderStage);
    const auto &calcOutLocInfoMap = m_locationInfoMapManager->getMap();

    if (m_shaderStage == ShaderStageGeometry) {
      // NOTE: The output location info from next shader stage (FS) doesn't contain raster stream ID. We have to
      // reconstruct it.
      const auto rasterStream = m_pipelineState->getRasterizerState().rasterStream;
      for (auto &entry : nextStageInputLocInfoMap) {
        InOutLocationInfo origLocInfo(entry.first);
        origLocInfo.setStreamId(rasterStream);
        InOutLocationInfo newLocInfo(entry.second);
        newLocInfo.setStreamId(rasterStream);
        outputLocInfoMap.insert({origLocInfo, newLocInfo});
      }
    } else {
      outputLocInfoMap = nextStageInputLocInfoMap;
    }

    unsigned newLocMax = 0;
    for (const auto &entry : outputLocInfoMap)
      newLocMax = std::max(newLocMax, entry.second.getLocation() + 1);
    // Update output map
    for (const auto &entry : calcOutLocInfoMap) {
      InOutLocationInfo origLocInfo;
      origLocInfo.setStreamId(entry.first.getStreamId());
      origLocInfo.setLocation(entry.first.getLocation());
      origLocInfo.setComponent(entry.first.getComponent());
      InOutLocationInfo newLocInfo(entry.second);
      newLocInfo.setLocation(newLocInfo.getLocation() + newLocMax);
      outputLocInfoMap.insert({origLocInfo, newLocInfo});
    }

    // update output count per stream for GS
    if (m_shaderStage == ShaderStageGeometry) {
      for (auto &locInfoPair : outputLocInfoMap) {
        auto &outLocCount = inOutUsage.gs.outLocCount[locInfoPair.first.getStreamId()];
        outLocCount = std::max(outLocCount, locInfoPair.second.getLocation() + 1);
      }
    }
  }
}

// =====================================================================================================================
// Re-assemble output export functions based on the locationInfoMap
void PatchResourceCollect::reassembleOutputExportCalls() {
  if (m_outputCalls.empty())
    return;
  assert(m_pipelineState->canPackOutput(m_shaderStage));

  BuilderBase builder(*m_context);
  builder.SetInsertPoint(m_outputCalls.back());

  // The output vector can have at most 4 elements, but 2 16-bit values can be packed a single element
  static constexpr size_t MaxVectorComponents = 4;
  static constexpr size_t MaxNumElems = MaxVectorComponents * 2;
  // ElementsInfo represents the info of composing a vector in a location
  struct ElementsInfo {
    // Elements to be packed in one location, where 32-bit element is placed at the even index
    std::array<Value *, MaxNumElems> elements = {};
    // The corresponding call of each element
    std::array<CallInst *, MaxNumElems> outCalls = {};
    static_assert(std::tuple_size<decltype(elements)>::value == std::tuple_size<decltype(outCalls)>::value,
                  "The code assumes that 'elements' and 'outCalls' have the same number of elements");
    // Element number of 32-bit
    unsigned elemCountOf32bit = 0;
    // Element number of 16-bit
    unsigned elemCountOf16bit = 0;
    // First component index in the mapped vector.
    unsigned baseMappedComponentIdx = InvalidValue;
  };

  // Collect ElementsInfo in each packed location
  auto &outputLocInfoMap = m_pipelineState->getShaderResourceUsage(m_shaderStage)->inOutUsage.outputLocInfoMap;
  std::vector<ElementsInfo> elementsInfoArray(outputLocInfoMap.size());

  for (auto call : m_outputCalls) {
    InOutLocationInfo origLocInfo;
    origLocInfo.setLocation(cast<ConstantInt>(call->getOperand(0))->getZExtValue());
    origLocInfo.setComponent(cast<ConstantInt>(call->getOperand(1))->getZExtValue());

    auto mapIter = outputLocInfoMap.find(origLocInfo);
    // Unused scalarized calls have been added into dead call set
    if (mapIter == outputLocInfoMap.end())
      continue;
    // Add used scalarized calls to dead call set
    m_deadCalls.push_back(call);

    const unsigned newLoc = mapIter->second.getLocation();
    auto &elementsInfo = elementsInfoArray[newLoc];

    const unsigned origComponentIdx = mapIter->second.getComponent();
    // Update the first components used
    elementsInfo.baseMappedComponentIdx = std::min(elementsInfo.baseMappedComponentIdx, origComponentIdx);

    const unsigned elemIdx = origComponentIdx * 2 + (mapIter->second.isHighHalf() ? 1 : 0);
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

  // Re-assemble XX' output export calls for each packed location
  for (auto &elementsInfo : elementsInfoArray) {
    if (elementsInfo.elemCountOf16bit + elementsInfo.elemCountOf32bit == 0) {
      // Invalid elements
      continue;
    }

    // Construct the output value - a scalar or a vector
    const unsigned compCount = (elementsInfo.elemCountOf16bit + 1) / 2 + elementsInfo.elemCountOf32bit;
    assert(compCount <= MaxVectorComponents);
    assert(elementsInfo.baseMappedComponentIdx != InvalidValue);
    const unsigned baseComponentIdx = elementsInfo.baseMappedComponentIdx;
    const unsigned baseElementIdx = baseComponentIdx * 2;

    Value *outValue = nullptr;
    if (compCount == 1) {
      // Output a scalar
      outValue = elementsInfo.elements[baseElementIdx];
      assert(outValue);
      if (elementsInfo.elemCountOf16bit == 2) {
        // Two 16-bit elements packed as a 32-bit scalar
        Value *highElem = elementsInfo.elements[baseElementIdx + 1];
        assert(highElem);
        highElem = builder.CreateShl(highElem, 16);
        outValue = builder.CreateOr(outValue, highElem);
      }
      outValue = builder.CreateBitCast(outValue, builder.getFloatTy());
    } else {
      // Output a vector
      outValue = PoisonValue::get(FixedVectorType::get(builder.getFloatTy(), compCount));
      for (unsigned vectorComp = 0, elemIdx = baseElementIdx; vectorComp < compCount; vectorComp += 1, elemIdx += 2) {
        assert(elemIdx < MaxNumElems);
        Value *component = elementsInfo.elements[elemIdx];
        assert(component);
        if (Value *highElem = elementsInfo.elements[elemIdx + 1]) {
          // Two 16 - bit elements packed as a 32 - bit scalar
          highElem = builder.CreateShl(highElem, 16);
          component = builder.CreateOr(component, highElem);
        }
        component = builder.CreateBitCast(component, builder.getFloatTy());
        outValue = builder.CreateInsertElement(outValue, component, vectorComp);
      }
    }
    assert(outValue);

    // Create an output export call with the original call argument
    CallInst *baseOutCall = elementsInfo.outCalls[baseElementIdx];
    assert(baseOutCall);
    Value *args[3] = {baseOutCall->getOperand(0), baseOutCall->getOperand(1), outValue};

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
  SmallVector<GenericLocationOp *, 4> inputCalls;

  struct Payload {
    PatchResourceCollect *self;
    SmallVectorImpl<GenericLocationOp *> &inputCalls;
  };
  Payload payload = {this, inputCalls};

  static auto visitInput = [](Payload &payload, GenericLocationOp &input) {
    ShaderStage shaderStage = payload.self->m_pipelineShaders->getShaderStage(input.getFunction());
    if (payload.self->m_pipelineState->canPackInput(shaderStage)) {
      // Collect input calls without dynamic indexing that need scalarize
      const bool hasDynIndex = !isa<ConstantInt>(input.getLocOffset()) || !isa<ConstantInt>(input.getElemIdx());
      if (hasDynIndex) {
        // NOTE: Dynamic indexing (location offset or component) in FS is processed to be constant in lower pass.
        assert(shaderStage == ShaderStageTessControl);

        // Conservatively disable all packing of the VS-TCS interface if dynamic indexing is detected.
        payload.self->m_tcsInputHasDynamicIndexing = true;
      }
      if (!hasDynIndex && (isa<FixedVectorType>(input.getType()) || input.getType()->getPrimitiveSizeInBits() == 64))
        payload.inputCalls.push_back(&input);
    }
  };
  static auto visitor = llvm_dialects::VisitorBuilder<Payload>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add<InputImportGenericOp>([](auto &payload, auto &op) { visitInput(payload, op); })
                            .add<InputImportInterpolatedOp>([](auto &payload, auto &op) { visitInput(payload, op); })
                            .build();
  visitor.visit(payload, *module);

  for (Function &func : *module) {
    if (func.getName().startswith(lgcName::OutputExportGeneric)) {
      // This is a generic output. Find its uses in VS/TES/GS.
      for (User *user : func.users()) {
        auto call = cast<CallInst>(user);
        ShaderStage shaderStage = m_pipelineShaders->getShaderStage(call->getFunction());
        if (m_pipelineState->canPackOutput(shaderStage)) {
          // We have a use in VS/TES/GS. See if it needs scalarizing. The output value is always the final argument.
          Type *valueTy = call->getArgOperand(call->arg_size() - 1)->getType();
          if (isa<VectorType>(valueTy) || valueTy->getPrimitiveSizeInBits() == 64)
            outputCalls.push_back(call);
        }
      }
    }
  }
  // Scalarize the gathered inputs and outputs.
  for (GenericLocationOp *call : inputCalls) {
    // Don't scalarize TCS inputs if dynamic indexing is used.
    if (m_tcsInputHasDynamicIndexing) {
      ShaderStage shaderStage = m_pipelineShaders->getShaderStage(call->getFunction());
      if (shaderStage == ShaderStageTessControl)
        continue;
    }
    scalarizeGenericInput(call);
  }
  for (CallInst *call : outputCalls) {
    // Don't scalarize VS outputs if dynamic indexing is used in TCS inputs.
    if (m_tcsInputHasDynamicIndexing) {
      ShaderStage shaderStage = m_pipelineShaders->getShaderStage(call->getFunction());
      if (shaderStage == ShaderStageVertex)
        continue;
    }
    scalarizeGenericOutput(call);
  }
}

// =====================================================================================================================
// Scalarize a generic input.
// This is known to be an FS generic or interpolant input or TCS input that is either a vector or 64 bit.
//
// @param input : The input import op
void PatchResourceCollect::scalarizeGenericInput(GenericLocationOp *input) {
  BuilderBase builder(input);
  auto *interpolatedInput = dyn_cast<InputImportInterpolatedOp>(input);
  assert(interpolatedInput || isa<InputImportGenericOp>(input));

  unsigned elemIdx = cast<ConstantInt>(input->getElemIdx())->getZExtValue();
  Type *resultTy = input->getType();

  if (!isa<VectorType>(resultTy)) {
    // Handle the case of splitting a 64 bit scalar in two.
    assert(resultTy->getPrimitiveSizeInBits() == 64);
    Value *result = PoisonValue::get(FixedVectorType::get(builder.getInt32Ty(), 2));
    for (unsigned i = 0; i != 2; ++i) {
      Value *subElemIdx = builder.getInt32(elemIdx * 2 + i);
      Value *subElem;
      if (interpolatedInput) {
        assert(!interpolatedInput->getPerPrimitive());
        subElem = builder.create<InputImportInterpolatedOp>(
            builder.getInt32Ty(), false, interpolatedInput->getLocation(), interpolatedInput->getLocOffset(),
            subElemIdx, PoisonValue::get(builder.getInt32Ty()), interpolatedInput->getInterpMode(),
            interpolatedInput->getInterpValue());
      } else {
        subElem =
            builder.create<InputImportGenericOp>(builder.getInt32Ty(), input->getPerPrimitive(), input->getLocation(),
                                                 input->getLocOffset(), subElemIdx, input->getArrayIndex());
      }
      result = builder.CreateInsertElement(result, subElem, i);
    }
    result = builder.CreateBitCast(result, resultTy);
    input->replaceAllUsesWith(result);
    input->eraseFromParent();
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
  for (User *user : input->users()) {
    if (auto extract = dyn_cast<ExtractElementInst>(user)) {
      // NOTE: The extracted index is allowed to be constant or not. For non-constant, we just break the loop and mark
      // as unknownElementsUsed.
      if (auto extractIndex = dyn_cast<ConstantInt>(extract->getIndexOperand())) {
        unsigned idx = extractIndex->getZExtValue();
        assert(idx < scalarizeBy);
        elementUsed[idx] = true;
        continue;
      }
    } else if (auto shuffle = dyn_cast<ShuffleVectorInst>(user)) {
      SmallVector<int, 4> mask;
      shuffle->getShuffleMask(mask);
      for (int maskElement : mask) {
        if (maskElement >= 0) {
          if (maskElement < scalarizeBy) {
            if (shuffle->getOperand(0) == input)
              elementUsed[maskElement] = true;
          } else {
            assert(maskElement < 2 * scalarizeBy);
            if (shuffle->getOperand(1) == input)
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
  Value *result = PoisonValue::get(resultTy);
  const unsigned location = input->getLocation();
  const bool is64Bit = elementTy->getPrimitiveSizeInBits() == 64;
  for (unsigned i = 0; i != scalarizeBy; ++i) {
    if (!unknownElementsUsed && !elementUsed[i])
      continue; // Omit trivially unused element

    unsigned newElemIdx = elemIdx + i;
    unsigned newLocation = location;
    if (is64Bit && i >= 2) {
      newLocation++;
      newElemIdx -= 2;
    }

    Value *element;
    if (interpolatedInput) {
      assert(!interpolatedInput->getPerPrimitive());
      element = builder.create<InputImportInterpolatedOp>(
          elementTy, false, newLocation, interpolatedInput->getLocOffset(), builder.getInt32(newElemIdx),
          PoisonValue::get(builder.getInt32Ty()), interpolatedInput->getInterpMode(),
          interpolatedInput->getInterpValue());
    } else {
      element =
          builder.create<InputImportGenericOp>(elementTy, input->getPerPrimitive(), newLocation, input->getLocOffset(),
                                               builder.getInt32(newElemIdx), input->getArrayIndex());
    }
    result = builder.CreateInsertElement(result, element, i);
    if (elementTy->getPrimitiveSizeInBits() == 64) {
      // If scalarizing with 64 bit elements, further split each element.
      scalarizeGenericInput(cast<GenericLocationOp>(element));
    }
  }

  input->replaceAllUsesWith(result);
  input->eraseFromParent();
}

// =====================================================================================================================
// Scalarize a generic output.
// This is known to be a last vertex processing stage (VS/TES/GS) generic output that is either a vector or 64 bit.
//
// @param call : Call that represents exporting the generic output
void PatchResourceCollect::scalarizeGenericOutput(CallInst *call) {
  BuilderBase builder(call->getContext());
  builder.SetInsertPoint(call);

  // VS: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
  // TES: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
  // GS: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
  SmallVector<Value *, 5> args;
  for (unsigned i = 0, end = call->arg_size(); i != end; ++i)
    args.push_back(call->getArgOperand(i));

  static const unsigned ElemIdxArgIdx = 1;
  unsigned valArgIdx = call->arg_size() - 1;
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
// Clear non-specified output value in non-fragment shader stages
void PatchResourceCollect::clearUndefinedOutput() {
  if (m_shaderStage == ShaderStageFragment)
    return;
  // NOTE: If a vector or all used channels in a location are not specified, we can safely drop it and remove the output
  // export call
  struct CandidateInfo {
    unsigned undefMask = 0;
    unsigned usedMask = 0;
    SmallVector<CallInst *> candidateCalls;
  };
  // Collect candidate info with undefined value at a location.
  std::map<InOutLocationInfo, CandidateInfo> locCandidateInfoMap;

  for (auto call : m_outputCalls) {
    auto outputValue = call->getArgOperand(call->arg_size() - 1);
    bool isUndefVal = isa<UndefValue>(outputValue) || isa<PoisonValue>(outputValue);
    unsigned index = (m_shaderStage == ShaderStageMesh || m_shaderStage == ShaderStageTessControl) ? 2 : 1;
    bool isDynElemIndexing = !isa<ConstantInt>(call->getArgOperand(index));

    InOutLocationInfo locInfo;
    locInfo.setLocation(cast<ConstantInt>(call->getArgOperand(0))->getZExtValue());
    if (m_shaderStage == ShaderStageGeometry)
      locInfo.setStreamId(cast<ConstantInt>(call->getArgOperand(2))->getZExtValue());

    unsigned undefMask = 0;
    unsigned usedMask = 0;
    if (isDynElemIndexing)
      usedMask = 1; // keep the call
    else {
      const unsigned elemIdx = cast<ConstantInt>(call->getArgOperand(index))->getZExtValue();
      usedMask = 1 << elemIdx;
      if (isUndefVal)
        undefMask = 1 << elemIdx;
    }

    auto iter = locCandidateInfoMap.find(locInfo);
    if (iter == locCandidateInfoMap.end()) {
      CandidateInfo candidataInfo;
      candidataInfo.undefMask = undefMask;
      candidataInfo.usedMask = usedMask;
      candidataInfo.candidateCalls.push_back(call);
      locCandidateInfoMap[locInfo] = candidataInfo;
    } else {
      iter->second.undefMask |= undefMask;
      iter->second.usedMask |= usedMask;
      iter->second.candidateCalls.push_back(call);
    }
  }
  m_outputCalls.clear();
  // Check if all used channels are undefined in a location in a stream
  for (auto &locCandidate : locCandidateInfoMap) {
    auto candidateCalls = locCandidate.second.candidateCalls;
    if (locCandidate.second.usedMask != locCandidate.second.undefMask) {
      m_outputCalls.insert(m_outputCalls.end(), candidateCalls.begin(), candidateCalls.end());
      continue;
    }

    m_deadCalls.insert(m_deadCalls.end(), candidateCalls.begin(), candidateCalls.end());

    for (auto call : candidateCalls) {
      // For unlinked case, we should keep the location info map unchanged.
      if (m_pipelineState->getNextShaderStage(m_shaderStage) != ShaderStageInvalid) {
        // Remove the output location info if it exists
        unsigned index = m_shaderStage == ShaderStageMesh ? 2 : 1;
        unsigned component = cast<ConstantInt>(call->getArgOperand(index))->getZExtValue();
        auto outputValue = call->getArgOperand(call->arg_size() - 1);
        if (outputValue->getType()->getScalarSizeInBits() == 64)
          component *= 2; // Component in location info is dword-based

        InOutLocationInfo outLocInfo;
        const unsigned location = locCandidate.first.getLocation();
        outLocInfo.setLocation(location);
        outLocInfo.setComponent(component);
        if (m_shaderStage == ShaderStageGeometry)
          outLocInfo.setStreamId(locCandidate.first.getStreamId());

        auto &outLocInfoMap = m_resUsage->inOutUsage.outputLocInfoMap;
        if (outLocInfoMap.count(outLocInfo) > 0) {
          outLocInfoMap.erase(outLocInfo);
          if (outputValue->getType()->getPrimitiveSizeInBits() > 128) {
            // NOTE: For any data that is larger than <4 x dword>, there are two consecutive locations occupied.
            outLocInfo.setLocation(location + 1);
            outLocInfoMap.erase(outLocInfo);
          }
        }

        // Remove transform location info if it exists
        outLocInfo.setLocation(location);
        auto &locInfoXfbOutInfoMap = m_resUsage->inOutUsage.locInfoXfbOutInfoMap;
        if (locInfoXfbOutInfoMap.count(outLocInfo) > 0) {
          locInfoXfbOutInfoMap.erase(outLocInfo);
          if (outputValue->getType()->getPrimitiveSizeInBits() > 128) {
            // NOTE: For any data that is larger than <4 x dword>, there are two consecutive locations occupied.
            outLocInfo.setLocation(location + 1);
            locInfoXfbOutInfoMap.erase(outLocInfo);
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Create a locationInfo map for the given shader stage
//
// @param call : Call to process
// @param shaderStage : Shader stage
// @param requireDword : Whether need extend 8-bit/16-bit data to dword
void InOutLocationInfoMapManager::createMap(ArrayRef<GenericLocationOp *> calls, ShaderStage shaderStage,
                                            bool requireDword) {
  for (auto call : calls)
    addSpan(call, shaderStage, requireDword);
  // Build locationInfoMap according to the collected LocationSpans
  buildMap(shaderStage);
}

// =====================================================================================================================
// Create a locationInfo map for the given shader stage
//
// @param locInfos : location infos to process
// @param shaderStage : Shader stage
void InOutLocationInfoMapManager::createMap(const std::vector<InOutLocationInfo> &locInfos, ShaderStage shaderStage) {
  for (const auto &locInfo : locInfos) {
    LocationSpan span{};
    span.firstLocationInfo = locInfo;
    m_locationSpans.insert(span);
  }
  buildMap(shaderStage);
}

// =====================================================================================================================
// Create a locationInfo map by deserializing the serialized map. Used when compiling the vertex-processing
// part-pipeline given the packed input map from the separate FS compilation.
void InOutLocationInfoMapManager::deserializeMap(ArrayRef<std::pair<unsigned, unsigned>> serializedMap) {
  m_locationInfoMap.clear();
  for (std::pair<unsigned, unsigned> entry : serializedMap)
    m_locationInfoMap[entry.first] = entry.second;
}

// =====================================================================================================================
// Fill the locationSpan container by constructing a LocationSpan from each input import call or GS output export call
//
// @param call : Call to process
// @param shaderStage : Shader stage
// @param requireDword : Whether need extend to dword
void InOutLocationInfoMapManager::addSpan(CallInst *call, ShaderStage shaderStage, bool requireDword) {
  const bool isFs = shaderStage == ShaderStageFragment;
  unsigned interpMode = InOutInfo::InterpModeCustom;
  bool isInterpolated = false;
  unsigned location = InvalidValue;
  unsigned elemIdx = InvalidValue;
  std::optional<unsigned> streamId;
  unsigned bitWidth = call->getType()->getScalarSizeInBits();

  if (auto *genericLocationOp = dyn_cast<GenericLocationOp>(call)) {
    location = genericLocationOp->getLocation() + cast<ConstantInt>(genericLocationOp->getLocOffset())->getZExtValue();
    elemIdx = cast<ConstantInt>(genericLocationOp->getElemIdx())->getZExtValue();

    if (auto *interpolated = dyn_cast<InputImportInterpolatedOp>(genericLocationOp)) {
      isInterpolated = true;
      interpMode = interpolated->getInterpMode();
    }
  } else {
    location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();

    unsigned compIdxArgIdx = 1;
    if (shaderStage == ShaderStageTessControl) {
      location += cast<ConstantInt>(call->getOperand(1))->getZExtValue();
      compIdxArgIdx = 2;
    }

    elemIdx = cast<ConstantInt>(call->getOperand(compIdxArgIdx))->getZExtValue();

    if (shaderStage == ShaderStageGeometry &&
        call->getCalledFunction()->getName().startswith(lgcName::OutputExportGeneric)) {
      // Set streamId and output bitWidth of a GS output export for copy shader use
      streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
      bitWidth = call->getOperand(3)->getType()->getScalarSizeInBits();
    }
  }

  if (requireDword && bitWidth < 32)
    bitWidth = 32;
  else if (bitWidth == 8)
    bitWidth = 16;

  LocationSpan span = {};
  span.firstLocationInfo.setLocation(location);
  span.firstLocationInfo.setComponent(elemIdx);

  span.compatibilityInfo.halfComponentCount = bitWidth / 16;
  // For VS/TES-FS, 32-bit and 16-bit are packed separately; For VS-TCS, VS/TES-GS and GS-FS, they are packed together
  span.compatibilityInfo.is16Bit = bitWidth == 16;

  if (streamId.has_value())
    span.firstLocationInfo.setStreamId(*streamId);

  if (isFs && isInterpolated) {
    span.compatibilityInfo.isFlat = interpMode == InOutInfo::InterpModeFlat;
    span.compatibilityInfo.isCustom = interpMode == InOutInfo::InterpModeCustom;
  }
  m_locationSpans.insert(span);
}

// =====================================================================================================================
// Build the map between original InOutLocationInfo and packed InOutLocationInfo based on sorted location spans
//
// @param shaderStage : The shader stage to determine whether to check compatibility
void InOutLocationInfoMapManager::buildMap(ShaderStage shaderStage) {
  m_locationInfoMap.clear();
  if (m_locationSpans.empty())
    return;

  // Map original InOutLocationInfo to new InOutLocationInfo
  unsigned consecutiveLocation = 0;
  unsigned compIdx = 0;
  bool isHighHalf = false;
  const bool isGs = shaderStage == ShaderStageGeometry;

  for (auto spanIt = m_locationSpans.begin(); spanIt != m_locationSpans.end(); ++spanIt) {
    if (spanIt != m_locationSpans.begin()) {
      // Check the current span with previous span to determine whether it is put in the same location or the next
      // location.
      const auto &prevSpan = *(--spanIt);
      ++spanIt;

      bool compatible = isCompatible(prevSpan, *spanIt, shaderStage);

      // If the current locationSpan is compatible with previous one, increase component index with location unchanged
      // until the component index is up to 4 and increase location index and reset component index to 0. Otherwise,
      // reset the location index for GS or increase location index, and reset component index to 0.
      if (compatible) {
        if (compIdx > 3) {
          ++consecutiveLocation;
          compIdx = 0;
          isHighHalf = false;
        } else {
          isHighHalf = spanIt->compatibilityInfo.is16Bit ? !isHighHalf : false;
        }
      } else {
        ++consecutiveLocation;
        // NOTE: For GS, the indexing of remapped location is zero-based in each stream
        if (isGs && spanIt->firstLocationInfo.getStreamId() != prevSpan.firstLocationInfo.getStreamId())
          consecutiveLocation = 0;
        compIdx = 0;
        isHighHalf = false;
      }
    }

    // Add a location map item
    InOutLocationInfo newLocInfo;
    newLocInfo.setLocation(consecutiveLocation);
    newLocInfo.setComponent(compIdx);
    newLocInfo.setHighHalf(isHighHalf);
    newLocInfo.setStreamId(spanIt->firstLocationInfo.getStreamId());
    m_locationInfoMap.insert({spanIt->firstLocationInfo, newLocInfo});

    // Update component index
    if (isHighHalf || !spanIt->compatibilityInfo.is16Bit)
      ++compIdx;
    assert(compIdx <= 4);
  }

  // Exists temporarily for computing m_locationInfoMap
  m_locationSpans.clear();
}

// =====================================================================================================================
// Output a mapped InOutLocationInfo from a given InOutLocationInfo if the mapping exists
//
// @param origLocInfo : The original InOutLocationInfo
// @param [out] mapIt : Iterator to an element of m_locationInfoMap with key equivalent to the given InOutLocationInfo
bool InOutLocationInfoMapManager::findMap(const InOutLocationInfo &origLocInfo,
                                          InOutLocationInfoMap::const_iterator &mapIt) {
  mapIt = m_locationInfoMap.find(origLocInfo);
  return mapIt != m_locationInfoMap.end();
}

} // namespace lgc
