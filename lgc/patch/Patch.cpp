/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Patch.cpp
 * @brief LLPC source file: contains implementation of class lgc::Patch.
 ***********************************************************************************************************************
 */
#include "lgc/patch/Patch.h"
#include "PatchNullFragShader.h"
#include "llvmraytracing/Continuations.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/Pipeline.h"
#include "lgc/builder/BuilderReplayer.h"
#include "lgc/patch/CombineCooperativeMatrix.h"
#include "lgc/patch/Continufy.h"
#include "lgc/patch/FragColorExport.h"
#include "lgc/patch/LowerCooperativeMatrix.h"
#include "lgc/patch/LowerDebugPrintf.h"
#include "lgc/patch/LowerDesc.h"
#include "lgc/patch/LowerGpuRt.h"
#include "lgc/patch/LowerSubgroupOps.h"
#include "lgc/patch/PatchBufferOp.h"
#include "lgc/patch/PatchCheckShaderCache.h"
#include "lgc/patch/PatchCopyShader.h"
#include "lgc/patch/PatchEntryPointMutate.h"
#include "lgc/patch/PatchImageDerivatives.h"
#include "lgc/patch/PatchImageOpCollect.h"
#include "lgc/patch/PatchInOutImportExport.h"
#include "lgc/patch/PatchInitializeWorkgroupMemory.h"
#include "lgc/patch/PatchInvariantLoads.h"
#include "lgc/patch/PatchLlvmIrInclusion.h"
#include "lgc/patch/PatchLoadScalarizer.h"
#include "lgc/patch/PatchLoopMetadata.h"
#include "lgc/patch/PatchPeepholeOpt.h"
#include "lgc/patch/PatchPreparePipelineAbi.h"
#include "lgc/patch/PatchReadFirstLane.h"
#include "lgc/patch/PatchResourceCollect.h"
#include "lgc/patch/PatchSetupTargetFeatures.h"
#include "lgc/patch/PatchWorkarounds.h"
#include "lgc/patch/TcsPassthroughShader.h"
#include "lgc/patch/VertexFetch.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Module.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 442438
// Old version of the code
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/IRPrinter/IRPrintingPasses.h"
#endif
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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 475156
// Old version of the code
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/Transforms/Scalar/InferAlignment.h"
#endif
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

#define DEBUG_TYPE "lgc-patch"

using namespace llvm;

static const char LdsGsName[] = "Lds.GS";
static const char LdsHsName[] = "Lds.HS";

namespace lgc {

// =====================================================================================================================
// Add whole-pipeline patch passes to pass manager
//
// @param pipelineState : Pipeline state
// @param [in/out] passMgr : Pass manager to add passes to
// @param patchTimer : Timer to time patch passes with, nullptr if not timing
// @param optTimer : Timer to time LLVM optimization passes with, nullptr if not timing
// @param checkShaderCacheFunc : Callback function to check shader cache
// @param optLevel : The optimization level uses to adjust the aggressiveness of
//                   passes and which passes to add.
void Patch::addPasses(PipelineState *pipelineState, lgc::PassManager &passMgr, Timer *patchTimer, Timer *optTimer,
                      Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, uint32_t optLevel) {
  // Start timer for patching passes.
  if (patchTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, true);

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

  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.addPass(PrintModulePass(*outs,
                                    "===============================================================================\n"
                                    "// LLPC pipeline before-patching results\n"));
  }

  passMgr.addPass(IPSCCPPass());

  passMgr.addPass(createModuleToFunctionPassAdaptor(CombineCooperativeMatrix()));
  // Lower the cooperative matrix
  passMgr.addPass(LowerCooperativeMatrix());

  if (pipelineState->hasShaderStage(ShaderStage::Vertex) && !pipelineState->hasShaderStage(ShaderStage::TessControl) &&
      pipelineState->hasShaderStage(ShaderStage::TessEval))
    passMgr.addPass(TcsPassthroughShader());

  passMgr.addPass(PatchNullFragShader());
  passMgr.addPass(PatchResourceCollect()); // also removes inactive/unused resources

  // PatchCheckShaderCache depends on PatchResourceCollect
  passMgr.addPass(PatchCheckShaderCache(std::move(checkShaderCacheFunc)));

  // First part of lowering to "AMDGCN-style"
  passMgr.addPass(PatchWorkarounds());
  passMgr.addPass(PatchCopyShader());
  passMgr.addPass(LowerVertexFetch());
  passMgr.addPass(LowerFragColorExport());
  passMgr.addPass(LowerDebugPrintf());
  passMgr.addPass(LowerDesc());
  passMgr.addPass(PatchEntryPointMutate());
  passMgr.addPass(PatchInitializeWorkgroupMemory());
  passMgr.addPass(PatchInOutImportExport());

  // Patch invariant load and loop metadata.
  passMgr.addPass(createModuleToFunctionPassAdaptor(PatchInvariantLoads()));
  passMgr.addPass(createModuleToFunctionPassAdaptor(createFunctionToLoopPassAdaptor(PatchLoopMetadata())));

  if (patchTimer) {
    LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, false);
    LgcContext::createAndAddStartStopTimer(passMgr, optTimer, true);
  }

  addOptimizationPasses(passMgr, optLevel);

  if (patchTimer) {
    LgcContext::createAndAddStartStopTimer(passMgr, optTimer, false);
    LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, true);
  }

  // Collect image operations
  if (pipelineState->getTargetInfo().getGfxIpVersion().major >= 11)
    passMgr.addPass(PatchImageOpCollect());

  // Second part of lowering to "AMDGCN-style"
  passMgr.addPass(PatchPreparePipelineAbi());

  // Do inlining and global DCE to inline subfunctions that were introduced during preparing pipeline ABI.
  passMgr.addPass(AlwaysInlinerPass());
  passMgr.addPass(GlobalDCEPass());

  const bool canUseNgg = pipelineState->isGraphics() &&
                         ((pipelineState->getTargetInfo().getGfxIpVersion().major == 10 &&
                           (pipelineState->getOptions().nggFlags & NggFlagDisable) == 0) ||
                          pipelineState->getTargetInfo().getGfxIpVersion().major >= 11); // Must enable NGG on GFX11+
  if (canUseNgg) {
    if (patchTimer) {
      LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, false);
      LgcContext::createAndAddStartStopTimer(passMgr, optTimer, true);
    }

    // Extra optimizations after NGG primitive shader creation
    FunctionPassManager fpm;
    fpm.addPass(PromotePass());
    fpm.addPass(ADCEPass());
    fpm.addPass(PatchBufferOp());
    fpm.addPass(InstCombinePass());
    fpm.addPass(SimplifyCFGPass());
    passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));

    if (patchTimer) {
      LgcContext::createAndAddStartStopTimer(passMgr, optTimer, false);
      LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, true);
    }
  } else {
    FunctionPassManager fpm;
    fpm.addPass(PatchBufferOp());
    fpm.addPass(InstCombinePass());
    passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  }

  passMgr.addPass(PatchImageDerivatives());

  // Set up target features in shader entry-points.
  // NOTE: Needs to be done after post-NGG function inlining, because LLVM refuses to inline something
  // with conflicting attributes. Attributes could conflict on GFX10 because PatchSetupTargetFeatures
  // adds a target feature to determine wave32 or wave64.
  passMgr.addPass(PatchSetupTargetFeatures());

  // Include LLVM IR as a separate section in the ELF binary
  if (pipelineState->getOptions().includeIr)
    passMgr.addPass(PatchLlvmIrInclusion());

  // Stop timer for patching passes.
  if (patchTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, false);

  // Dump the result
  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.addPass(PrintModulePass(*outs,
                                    "===============================================================================\n"
                                    "// LLPC pipeline patching results\n"));
  }
}

// =====================================================================================================================
// Register all the patching passes into the given pass manager
//
// @param [in/out] passMgr : Pass manager
void Patch::registerPasses(lgc::PassManager &passMgr) {
#define LLPC_PASS(NAME, CLASS) passMgr.registerPass(NAME, CLASS::name());
#define LLPC_MODULE_ANALYSIS(NAME, CLASS) passMgr.registerPass(NAME, CLASS::name());
#include "PassRegistry.inc"
}

// =====================================================================================================================
// Register all the patching passes into the given pass manager
//
// @param [in/out] passMgr : Pass manager
void Patch::registerPasses(PassBuilder &passBuilder) {
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
void Patch::addOptimizationPasses(lgc::PassManager &passMgr, uint32_t optLevel) {
  LLPC_OUTS("PassManager optimization level = " << optLevel << "\n");

  passMgr.addPass(ForceFunctionAttrsPass());
  FunctionPassManager fpm;
  fpm.addPass(InstCombinePass());
  fpm.addPass(SimplifyCFGPass());
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 444780
  // Old version of the code
  fpm.addPass(SROAPass());
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  fpm.addPass(SROAPass(SROAOptions::ModifyCFG));
#endif
  fpm.addPass(EarlyCSEPass(true));
  fpm.addPass(SpeculativeExecutionPass(/* OnlyIfDivergentTarget = */ true));
  fpm.addPass(CorrelatedValuePropagationPass());
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(AggressiveInstCombinePass());
  fpm.addPass(InstCombinePass());
  fpm.addPass(PatchPeepholeOpt());
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
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 464212
  // Old version of the code
  fpm.addPass(ScalarizerPass());
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  ScalarizerPassOptions scalarizerOptions;
  scalarizerOptions.ScalarizeMinBits = 32;
  fpm.addPass(ScalarizerPass(scalarizerOptions));
#endif
  fpm.addPass(PatchLoadScalarizer());
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
  fpm.addPass(PatchReadFirstLane());
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 475156
  // Old version of the code
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  fpm.addPass(InferAlignmentPass());
#endif
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
void Patch::init(Module *module) {
  m_module = module;
  m_context = &m_module->getContext();
  m_shaderStage = ShaderStage::Invalid;
  m_entryPoint = nullptr;
}

// =====================================================================================================================
// Get or create global variable for LDS.
//
// @param pipelineState : Pipeline state
// @param [in/out] module : Module to get or create LDS in
Constant *Patch::getLdsVariable(PipelineState *pipelineState, Function *func, bool rtStack) {
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
  auto oldLds = func->getParent()->getNamedValue(ldsName);
  if (oldLds)
    return cast<GlobalVariable>(oldLds);

  // Else create LDS variable for this function.
  // LDS type: [ldsSize * i32], address space 3
  const auto i32Ty = Type::getInt32Ty(*context);
  const auto ldsTy = ArrayType::get(i32Ty, ldsSize);
  auto lds = new GlobalVariable(*module, ldsTy, false, GlobalValue::ExternalLinkage, nullptr, Twine(ldsName), nullptr,
                                GlobalValue::NotThreadLocal, ADDR_SPACE_LOCAL);
  lds->setAlignment(MaybeAlign(sizeof(unsigned)));

  if (rtStack) {
    auto *offset = Constant::getIntegerValue(i32Ty, APInt(32, staticLdsSize));
    return ConstantExpr::getGetElementPtr(i32Ty, lds, offset);
  }

  return lds;
}

} // namespace lgc
