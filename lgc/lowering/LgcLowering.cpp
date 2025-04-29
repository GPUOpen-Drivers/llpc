/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LgcLowering.cpp
 * @brief LLPC source file: contains implementation of class lgc::LgcLowering.
 ***********************************************************************************************************************
 */
#include "lgc/lowering/LgcLowering.h"
#include "GenerateNullFragmentShader.h"
#include "LowerPopsInterlock.h"
#include "LowerRayQueryWrapper.h"
#include "llvmraytracing/Continuations.h"
#include "lgc/Debug.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/Pipeline.h"
#include "lgc/builder/BuilderReplayer.h"
#include "lgc/lowering/AddBufferOperationMetadata.h"
#include "lgc/lowering/AddLoopMetadata.h"
#include "lgc/lowering/ApplyWorkarounds.h"
#include "lgc/lowering/CheckShaderCache.h"
#include "lgc/lowering/CollectImageOperations.h"
#include "lgc/lowering/CollectResourceUsage.h"
#include "lgc/lowering/CombineCooperativeMatrix.h"
#include "lgc/lowering/Continufy.h"
#include "lgc/lowering/EmitShaderHashToken.h"
#include "lgc/lowering/FragmentColorExport.h"
#include "lgc/lowering/GenerateCopyShader.h"
#include "lgc/lowering/IncludeLlvmIr.h"
#include "lgc/lowering/InitializeUndefInputs.h"
#include "lgc/lowering/InitializeWorkgroupMemory.h"
#include "lgc/lowering/LowerBufferOperations.h"
#include "lgc/lowering/LowerCooperativeMatrix.h"
#include "lgc/lowering/LowerDebugPrintf.h"
#include "lgc/lowering/LowerDesc.h"
#include "lgc/lowering/LowerGpuRt.h"
#include "lgc/lowering/LowerImageDerivatives.h"
#include "lgc/lowering/LowerInOut.h"
#include "lgc/lowering/LowerInvariantLoads.h"
#include "lgc/lowering/LowerMulDx9Zero.h"
#include "lgc/lowering/LowerReadFirstLane.h"
#include "lgc/lowering/LowerSubgroupOps.h"
#include "lgc/lowering/MutateEntryPoint.h"
#include "lgc/lowering/PassthroughHullShader.h"
#include "lgc/lowering/PeepholeOptimization.h"
#include "lgc/lowering/PreparePipelineAbi.h"
#include "lgc/lowering/ScalarizeLoads.h"
#include "lgc/lowering/SetupTargetFeatures.h"
#include "lgc/lowering/StructurizeBuffers.h"
#include "lgc/lowering/VertexFetch.h"
#include "lgc/lowering/WorkaroundDsSubdwordWrite.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Module.h"
#include "llvm/IRPrinter/IRPrintingPasses.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/ConstantMerge.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/BDCE.h"
#include "llvm/Transforms/Scalar/CorrelatedValuePropagation.h"
#include "llvm/Transforms/Scalar/DivRemPairs.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/InferAlignment.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Scalar/LoopIdiomRecognize.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/NewGVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SpeculativeExecution.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#define DEBUG_TYPE "lgc-lowering"

using namespace llvm;

static const char LdsGsName[] = "Lds.GS";
static const char LdsHsName[] = "Lds.HS";

namespace lgc {

// =====================================================================================================================
// Add whole-pipeline LGC lowering passes to pass manager
//
// @param pipelineState : Pipeline state
// @param [in/out] passMgr : Pass manager to add passes to
// @param loweringTimer : Timer to time LGC lowering passes with, nullptr if not timing
// @param optTimer : Timer to time LLVM optimization passes with, nullptr if not timing
// @param checkShaderCacheFunc : Callback function to check shader cache
// @param optLevel : The optimization level uses to adjust the aggressiveness of
//                   passes and which passes to add.
void LgcLowering::addPasses(PipelineState *pipelineState, lgc::PassManager &passMgr, Timer *loweringTimer,
                            Timer *optTimer, Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, uint32_t optLevel) {
  // Start timer for LGC lowering passes.
  if (loweringTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, loweringTimer, true);

  if (pipelineState->getOptions().useGpurt) {
    passMgr.addPass(LowerRayQueryWrapper());
  }
  const auto indirectMode = pipelineState->getOptions().rtIndirectMode;
  if (indirectMode == RayTracingIndirectMode::ContinuationsContinufy ||
      indirectMode == RayTracingIndirectMode::Continuations) {
    if (indirectMode == RayTracingIndirectMode::ContinuationsContinufy) {
      passMgr.addPass(Continufy());
      // NOTE: LowerGpuRt needs to be run before continuation transform for continufy mode because some GPURT dialects
      // that continuation transform does not support are used.
      passMgr.addPass(LowerGpuRt());
    } else {
      // NOTE: LowerRaytracingPipelinePass should be run before getting into LGC because we will need to collect
      // metadata added by the pass.
      // Optimize away the alloca's insert during lower-raytracing pipeline to avoid being put in continuation state.
      passMgr.addPass(createModuleToFunctionPassAdaptor(SROAPass(llvm::SROAOptions::ModifyCFG)));
    }

    addLgcContinuationTransform(passMgr);
  }

  if (pipelineState->getOptions().useGpurt) {
    // NOTE: Lower GPURT operations and run InstCombinePass before builder replayer, because some Op are going to be
    // turned into constant value, so that we can eliminate unused `@lgc.load.buffer.desc` before getting into
    // replayer. Otherwise, unnecessary `writes_uavs` and `uses_uav` may be set.
    // NOTE: Lower GPURT operations after continuations transform, because we will inline some functions from GPURT
    // library which may use gpurt dialect, and the library itself doesn't run any LGC passes.
    passMgr.addPass(LowerGpuRt());
    passMgr.addPass(createModuleToFunctionPassAdaptor(InstCombinePass()));
  }

  // NOTE: Replay after continuations transform, because we will inline some functions from GPURT library which may use
  // lgc record ops, and the library itself doesn't run any LGC passes.
  // We're using BuilderRecorder; replay the Builder calls now
  passMgr.addPass(BuilderReplayer());
  passMgr.addPass(LowerSubgroupOps());

  passMgr.addPass(createModuleToFunctionPassAdaptor(AddLoopMetadata()));

  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.addPass(PrintModulePass(*outs,
                                    "===============================================================================\n"
                                    "// LLPC LGC before-lowering results\n"));
  }

  passMgr.addPass(IPSCCPPass());
  passMgr.addPass(createModuleToFunctionPassAdaptor(CombineCooperativeMatrix()));
  // Lower the cooperative matrix
  passMgr.addPass(LowerCooperativeMatrix());

  if (pipelineState->hasShaderStage(ShaderStage::Vertex) && !pipelineState->hasShaderStage(ShaderStage::TessControl) &&
      pipelineState->hasShaderStage(ShaderStage::TessEval))
    passMgr.addPass(PassthroughHullShader());

  passMgr.addPass(GenerateNullFragmentShader());
  passMgr.addPass(InitializeUndefInputs());
  passMgr.addPass(CollectResourceUsage()); // also removes inactive/unused resources

  // CheckShaderCache depends on CollectResourceUsage
  passMgr.addPass(CheckShaderCache(std::move(checkShaderCacheFunc)));

  // First part of lowering to "AMDGCN-style"
  passMgr.addPass(ApplyWorkarounds());
  passMgr.addPass(GenerateCopyShader());
  passMgr.addPass(LowerVertexFetch());
  passMgr.addPass(LowerFragmentColorExport());
  passMgr.addPass(LowerDebugPrintf());
  // Mark shader stage for load/store.
  if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 12)
    passMgr.addPass(createModuleToFunctionPassAdaptor(AddBufferOperationMetadata()));
  passMgr.addPass(LowerDesc());
  passMgr.addPass(MutateEntryPoint());
  passMgr.addPass(createModuleToFunctionPassAdaptor(LowerPopsInterlock()));
  passMgr.addPass(InitializeWorkgroupMemory());
  passMgr.addPass(LowerInOut());

  // Lower invariant load and loop metadata.
  passMgr.addPass(createModuleToFunctionPassAdaptor(LowerInvariantLoads()));

  passMgr.addPass(WorkaroundDsSubdwordWrite());

  if (loweringTimer) {
    LgcContext::createAndAddStartStopTimer(passMgr, loweringTimer, false);
    LgcContext::createAndAddStartStopTimer(passMgr, optTimer, true);
  }

  addOptimizationPasses(passMgr, optLevel);

  if (loweringTimer) {
    LgcContext::createAndAddStartStopTimer(passMgr, optTimer, false);
    LgcContext::createAndAddStartStopTimer(passMgr, loweringTimer, true);
  }

  // Collect image operations
  if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 11)
    passMgr.addPass(CollectImageOperations());

  // Second part of lowering to "AMDGCN-style"
  passMgr.addPass(PreparePipelineAbi());
  passMgr.addPass(EmitShaderHashToken());

  // Do inlining and global DCE to inline subfunctions that were introduced during preparing pipeline ABI.
  passMgr.addPass(AlwaysInlinerPass());
  passMgr.addPass(GlobalDCEPass());

  bool usesNgg = false;
  if (pipelineState->isGraphics()) {
    const auto gfxIp = pipelineState->getTargetInfo().getGfxIpVersion();
    if (gfxIp.major >= 11) {
      usesNgg = true; // Must enable NGG on GFX11+
    } else {
      assert(gfxIp.major == 10);
      usesNgg = (pipelineState->getOptions().nggFlags & NggFlagDisable) == 0; // Check NGG disable flag
    }
  }
  const bool hasMeshShader = pipelineState->hasShaderStage(ShaderStage::Mesh);

  if (usesNgg || hasMeshShader) {
    if (loweringTimer) {
      LgcContext::createAndAddStartStopTimer(passMgr, loweringTimer, false);
      LgcContext::createAndAddStartStopTimer(passMgr, optTimer, true);
    }

    // Extra optimizations after NGG primitive shader creation or mesh shader lowering
    FunctionPassManager fpm;
    fpm.addPass(PromotePass());
    fpm.addPass(ADCEPass());
    fpm.addPass(StructurizeBuffers());
    fpm.addPass(LowerBufferOperations());
    fpm.addPass(InstCombinePass());
    fpm.addPass(SimplifyCFGPass());
    passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));

    if (loweringTimer) {
      LgcContext::createAndAddStartStopTimer(passMgr, optTimer, false);
      LgcContext::createAndAddStartStopTimer(passMgr, loweringTimer, true);
    }
  } else {
    FunctionPassManager fpm;
    fpm.addPass(StructurizeBuffers());
    fpm.addPass(LowerBufferOperations());
    fpm.addPass(InstCombinePass());
    passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  }

  passMgr.addPass(LowerImageDerivatives());

  // Set up target features in shader entry-points.
  // NOTE: Needs to be done after post-NGG function inlining, because LLVM refuses to inline something
  // with conflicting attributes. Attributes could conflict on GFX10 because SetUpTargetFeatures
  // adds a target feature to determine wave32 or wave64.
  passMgr.addPass(SetUpTargetFeatures());

  // Include LLVM IR as a separate section in the ELF binary
  if (pipelineState->getOptions().includeIr)
    passMgr.addPass(IncludeLlvmIr());

  // Stop timer for LGC lowering passes.
  if (loweringTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, loweringTimer, false);

  // Dump the result
  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.addPass(PrintModulePass(*outs,
                                    "===============================================================================\n"
                                    "// LLPC LGC lowering results\n"));
  }
}

// =====================================================================================================================
// Register all the LGC lowering passes into the given pass manager
//
// @param [in/out] passMgr : Pass manager
void LgcLowering::registerPasses(lgc::PassManager &passMgr) {
#define LLPC_PASS(NAME, CLASS) passMgr.registerPass(NAME, CLASS::name());
#define LLPC_MODULE_ANALYSIS(NAME, CLASS) passMgr.registerPass(NAME, CLASS::name());
#include "PassRegistry.inc"
}

// =====================================================================================================================
// Register all the LGC lowering passes into the given pass manager
//
// @param [in/out] passMgr : Pass manager
void LgcLowering::registerPasses(PassBuilder &passBuilder) {
#define HANDLE_PASS(NAME, CLASS)                                                                                       \
  if (innerPipeline.empty() && name == NAME) {                                                                         \
    passMgr.addPass(CLASS());                                                                                          \
    return true;                                                                                                       \
  }

#define HANDLE_ANALYSIS(NAME, CLASS, IRUNIT)                                                                           \
  if (innerPipeline.empty() && name == "require<" NAME ">") {                                                          \
    passMgr.addPass(RequireAnalysisPass<CLASS, IRUNIT>());                                                             \
    return true;                                                                                                       \
  }                                                                                                                    \
  if (innerPipeline.empty() && name == "invalidate<" NAME ">") {                                                       \
    passMgr.addPass(InvalidateAnalysisPass<CLASS>());                                                                  \
    return true;                                                                                                       \
  }

  auto checkNameWithParams = [](StringRef name, StringRef passName, StringRef &params) -> bool {
    params = name;
    if (!params.consume_front(passName))
      return false;
    if (params.empty())
      return true;
    if (!params.consume_front("<"))
      return false;
    if (!params.consume_back(">"))
      return false;
    return true;
  };
  (void)checkNameWithParams;

#define HANDLE_PASS_WITH_PARSER(NAME, CLASS)                                                                           \
  if (innerPipeline.empty() && checkNameWithParams(name, NAME, params))                                                \
    return CLASS::parsePass(params, passMgr);

  passBuilder.registerPipelineParsingCallback(
      [=](StringRef name, ModulePassManager &passMgr, ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef params;
        (void)params;
#define LLPC_PASS(NAME, CLASS) /* */
#define LLPC_MODULE_PASS HANDLE_PASS
#define LLPC_MODULE_PASS_WITH_PARSER HANDLE_PASS_WITH_PARSER
#define LLPC_MODULE_ANALYSIS(NAME, CLASS) HANDLE_ANALYSIS(NAME, CLASS, Module)
#include "PassRegistry.inc"

        return false;
      });

  passBuilder.registerPipelineParsingCallback(
      [=](StringRef name, FunctionPassManager &passMgr, ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef params;
        (void)params;
#define LLPC_PASS(NAME, CLASS) /* */
#define LLPC_FUNCTION_PASS HANDLE_PASS
#define LLPC_FUNCTION_PASS_WITH_PARSER HANDLE_PASS_WITH_PARSER
#include "PassRegistry.inc"

        return false;
      });

  passBuilder.registerPipelineParsingCallback(
      [=](StringRef name, LoopPassManager &passMgr, ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef params;
        (void)params;
#define LLPC_PASS(NAME, CLASS) /* */
#define LLPC_LOOP_PASS HANDLE_PASS
#define LLPC_LOOP_PASS_WITH_PARSER HANDLE_PASS_WITH_PARSER
#include "PassRegistry.inc"

        return false;
      });

#undef HANDLE_PASS
#undef HANDLE_PASS_WITH_PARSER
}

// =====================================================================================================================
// Add optimization passes to pass manager
//
// @param [in/out] passMgr : Pass manager to add passes to
// @param optLevel : The optimization level uses to adjust the aggressiveness of
//                   passes and which passes to add.
void LgcLowering::addOptimizationPasses(lgc::PassManager &passMgr, uint32_t optLevel) {
  LLPC_OUTS("PassManager optimization level = " << optLevel << "\n");

  passMgr.addPass(ForceFunctionAttrsPass());
  FunctionPassManager fpm;
  fpm.addPass(InstCombinePass());
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
  fpm.addPass(EarlyCSEPass(true));
  fpm.addPass(SpeculativeExecutionPass(/* OnlyIfDivergentTarget = */ true));
  fpm.addPass(CorrelatedValuePropagationPass());
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(AggressiveInstCombinePass());
  fpm.addPass(InstCombinePass());
  fpm.addPass(PeepholeOptimization());
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(ReassociatePass());
  LoopPassManager lpm;
  lpm.addPass(LoopRotatePass());
  lpm.addPass(LICMPass(LICMOptions()));
  fpm.addPass(createFunctionToLoopPassAdaptor(std::move(lpm), true));
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(InstCombinePass());
  LoopPassManager lpm2;
  lpm2.addPass(IndVarSimplifyPass());
  lpm2.addPass(LoopIdiomRecognizePass());
  lpm2.addPass(LoopDeletionPass());
  fpm.addPass(createFunctionToLoopPassAdaptor(std::move(lpm2), true));
  fpm.addPass(LoopUnrollPass(
      LoopUnrollOptions(optLevel).setPeeling(true).setRuntime(false).setUpperBound(false).setPartial(false)));
  fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
  ScalarizerPassOptions scalarizerOptions;
  scalarizerOptions.ScalarizeMinBits = 32;
  fpm.addPass(ScalarizerPass(scalarizerOptions));
  fpm.addPass(LowerMulDx9Zero());
  fpm.addPass(ScalarizeLoads());
  fpm.addPass(InstSimplifyPass());
  fpm.addPass(NewGVNPass());
  fpm.addPass(BDCEPass());
  fpm.addPass(InstCombinePass());
  fpm.addPass(CorrelatedValuePropagationPass());
  fpm.addPass(ADCEPass());
  fpm.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
  fpm.addPass(SimplifyCFGPass(SimplifyCFGOptions()
                                  .bonusInstThreshold(1)
                                  .forwardSwitchCondToPhi(true)
                                  .convertSwitchToLookupTable(true)
                                  .needCanonicalLoops(true)
                                  .hoistCommonInsts(true)
                                  .sinkCommonInsts(true)));
  fpm.addPass(LoopUnrollPass(LoopUnrollOptions(optLevel)));
  fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
  // uses UniformityAnalysis
  fpm.addPass(LowerReadFirstLane());
  fpm.addPass(InferAlignmentPass());
  fpm.addPass(InstCombinePass());
  passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  passMgr.addPass(ConstantMergePass());
  FunctionPassManager fpm2;
  fpm2.addPass(DivRemPairsPass());
  fpm2.addPass(SimplifyCFGPass());
  passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm2)));
}

// =====================================================================================================================
// Initializes the pass according to the specified module.
//
// NOTE: This function should be called at the beginning of "runOnModule()".
//
// @param module : LLVM module
void LgcLowering::init(Module *module) {
  m_module = module;
  m_context = &m_module->getContext();
  m_shaderStage = std::nullopt;
  m_entryPoint = nullptr;
}

// =====================================================================================================================
// Get or create global variable for LDS.
//
// @param pipelineState : Pipeline state
// @param [in/out] module : Module to get or create LDS in
Constant *LgcLowering::getLdsVariable(PipelineState *pipelineState, Function *func, bool rtStack) {
  auto module = func->getParent();
  auto context = &module->getContext();

  auto stage = getShaderStage(func);
  assert(stage && "unable to determine stage for LDS usage");

  unsigned hwStageMask = pipelineState->getShaderHwStageMask(*stage);

  ShaderStageEnum ldsStage;
  const char *ldsName;
  if (hwStageMask & Util::Abi::HwShaderGs) {
    ldsName = LdsGsName;
    ldsStage = ShaderStage::Geometry;
  } else if (hwStageMask & Util::Abi::HwShaderHs) {
    ldsName = LdsHsName;
    ldsStage = ShaderStage::TessControl;
  } else {
    assert(false && "requesting LDS variable for unknown shader type");
    return nullptr;
  }

  const unsigned staticLdsSize = pipelineState->getShaderStaticLdsUsage(ldsStage, /*rtStack=*/false);
  const unsigned rtLdsSize = pipelineState->getShaderStaticLdsUsage(ldsStage, /*rtStack=*/true);
  const unsigned ldsSize = staticLdsSize + rtLdsSize;

  // See if module already has LDS variable.
  GlobalVariable *lds = nullptr;
  auto oldLds = func->getParent()->getNamedValue(ldsName);
  const auto i32Ty = Type::getInt32Ty(*context);
  if (oldLds) {
    lds = cast<GlobalVariable>(oldLds);
  } else {
    // Else create LDS variable for this function.
    // LDS type: [ldsSize * i32], address space 3
    const auto ldsTy = ArrayType::get(i32Ty, ldsSize);
    lds = new GlobalVariable(*module, ldsTy, false, GlobalValue::ExternalLinkage, nullptr, Twine(ldsName), nullptr,
                             GlobalValue::NotThreadLocal, ADDR_SPACE_LOCAL);
    lds->setAlignment(MaybeAlign(sizeof(unsigned)));
  }

  if (rtStack) {
    auto *offset = Constant::getIntegerValue(i32Ty, APInt(32, staticLdsSize));
    return ConstantExpr::getGetElementPtr(i32Ty, lds, offset);
  }

  return lds;
}

} // namespace lgc
