/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Patch.cpp
 * @brief LLPC source file: contains implementation of class lgc::Patch.
 ***********************************************************************************************************************
 */
#include "lgc/patch/Patch.h"
#include "PatchNullFragShader.h"
#include "lgc/LgcContext.h"
#include "lgc/PassManager.h"
#include "lgc/builder/BuilderReplayer.h"
#include "lgc/patch/FragColorExport.h"
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
#include "lgc/patch/PatchWaveSizeAdjust.h"
#include "lgc/patch/PatchWorkarounds.h"
#include "lgc/patch/VertexFetch.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/ConstantMerge.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
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
                      Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, CodeGenOpt::Level optLevel) {
  // Start timer for patching passes.
  if (patchTimer)
    LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, true);

  // We're using BuilderRecorder; replay the Builder calls now
  passMgr.addPass(BuilderReplayer(pipelineState));

  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.addPass(PrintModulePass(*outs,
                                    "===============================================================================\n"
                                    "// LLPC pipeline before-patching results\n"));
  }

  passMgr.addPass(IPSCCPPass());

  passMgr.addPass(PatchNullFragShader());
  passMgr.addPass(PatchResourceCollect()); // also removes inactive/unused resources

  // PatchCheckShaderCache depends on PatchResourceCollect
  passMgr.addPass(PatchCheckShaderCache(std::move(checkShaderCacheFunc)));

  // First part of lowering to "AMDGCN-style"
  passMgr.addPass(PatchWaveSizeAdjust());
  passMgr.addPass(PatchWorkarounds());
  passMgr.addPass(PatchCopyShader());
  passMgr.addPass(LowerVertexFetch());
  passMgr.addPass(LowerFragColorExport());
  passMgr.addPass(PatchEntryPointMutate());
  passMgr.addPass(PatchInitializeWorkgroupMemory());
  passMgr.addPass(PatchInOutImportExport());

  // Prior to general optimization, do function inlining and dead function removal to remove helper functions that
  // were introduced during lowering (e.g. streamout stores).
  passMgr.addPass(AlwaysInlinerPass());
  passMgr.addPass(GlobalDCEPass());

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

  // Second part of lowering to "AMDGCN-style"
  passMgr.addPass(PatchPreparePipelineAbi());

  const bool canUseNgg = pipelineState->isGraphics() && pipelineState->getTargetInfo().getGfxIpVersion().major == 10 &&
                         (pipelineState->getOptions().nggFlags & NggFlagDisable) == 0;
  if (canUseNgg) {
    if (patchTimer) {
      LgcContext::createAndAddStartStopTimer(passMgr, patchTimer, false);
      LgcContext::createAndAddStartStopTimer(passMgr, optTimer, true);
    }

    // Extra optimizations after NGG primitive shader creation
    passMgr.addPass(AlwaysInlinerPass());
    passMgr.addPass(GlobalDCEPass());
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
    fpm.addPass(InstCombinePass(2));
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
#include "PassRegistry.inc"
}

// =====================================================================================================================
// Add whole-pipeline patch passes to pass manager
//
// @param pipelineState : Pipeline state
// @param [in/out] passMgr : Pass manager to add passes to
// @param replayerPass : BuilderReplayer pass, or nullptr if not needed
// @param patchTimer : Timer to time patch passes with, nullptr if not timing
// @param optTimer : Timer to time LLVM optimization passes with, nullptr if not timing
// @param checkShaderCacheFunc : Callback function to check shader cache
// @param optLevel : The optimization level uses to adjust the aggressiveness of
//                   passes and which passes to add.
void LegacyPatch::addPasses(PipelineState *pipelineState, legacy::PassManager &passMgr, ModulePass *replayerPass,
                            Timer *patchTimer, Timer *optTimer, Pipeline::CheckShaderCacheFunc checkShaderCacheFunc,
                            CodeGenOpt::Level optLevel)
// Callback function to check shader cache
{
  // Start timer for patching passes.
  if (patchTimer)
    passMgr.add(LgcContext::createStartStopTimer(patchTimer, true));

  // If using BuilderRecorder rather than BuilderImpl, replay the Builder calls now
  if (replayerPass)
    passMgr.add(replayerPass);

  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.add(
        createPrintModulePass(*outs, "===============================================================================\n"
                                     "// LLPC pipeline before-patching results\n"));
  }

  // Build null fragment shader if necessary
  passMgr.add(createLegacyPatchNullFragShader());

  // Patch resource collecting, remove inactive resources (should be the first preliminary pass)
  passMgr.add(createLegacyPatchResourceCollect());

  // Patch wave size adjusting heuristic
  passMgr.add(createLegacyPatchWaveSizeAdjust());

  // Patch workarounds
  passMgr.add(createLegacyPatchWorkarounds());

  // Generate copy shader if necessary.
  passMgr.add(createLegacyPatchCopyShader());

  // Lower vertex fetch operations.
  passMgr.add(createLegacyLowerVertexFetch());

  // Lower fragment export operations.
  passMgr.add(createLegacyLowerFragColorExport());

  // Run IPSCCP before EntryPointMutate to avoid adding unnecessary arguments to an entry point.
  passMgr.add(createIPSCCPPass());

  // Patch entry-point mutation (should be done before external library link)
  passMgr.add(createLegacyPatchEntryPointMutate());

  // Patch workgroup memory initialization.
  passMgr.add(createLegacyPatchInitializeWorkgroupMemory());

  // Patch input import and output export operations
  passMgr.add(createLegacyPatchInOutImportExport());

  // Prior to general optimization, do function inlining and dead function removal
  passMgr.add(createAlwaysInlinerLegacyPass());
  passMgr.add(createGlobalDCEPass());

  // Patch invariant load metadata before optimizations.
  passMgr.add(createLegacyPatchInvariantLoads());

  // Patch loop metadata
  passMgr.add(createLegacyPatchLoopMetadata());

  // Check shader cache
  auto checkShaderCachePass = createLegacyPatchCheckShaderCache();
  passMgr.add(checkShaderCachePass);
  checkShaderCachePass->setCallbackFunction(std::move(checkShaderCacheFunc));

  // Stop timer for patching passes and start timer for optimization passes.
  if (patchTimer) {
    passMgr.add(LgcContext::createStartStopTimer(patchTimer, false));
    passMgr.add(LgcContext::createStartStopTimer(optTimer, true));
  }

  // Add some optimization passes
  addOptimizationPasses(passMgr, optLevel);

  // Stop timer for optimization passes and restart timer for patching passes.
  if (patchTimer) {
    passMgr.add(LgcContext::createStartStopTimer(optTimer, false));
    passMgr.add(LgcContext::createStartStopTimer(patchTimer, true));
  }

  // Patch buffer operations (must be after optimizations)
  passMgr.add(createLegacyPatchBufferOp());
  passMgr.add(createInstructionCombiningPass(2));

  // Fully prepare the pipeline ABI (must be after optimizations)
  passMgr.add(createLegacyPatchPreparePipelineAbi());

  const bool canUseNgg = pipelineState->isGraphics() && pipelineState->getTargetInfo().getGfxIpVersion().major == 10 &&
                         (pipelineState->getOptions().nggFlags & NggFlagDisable) == 0;
  if (canUseNgg) {
    // Stop timer for patching passes and restart timer for optimization passes.
    if (patchTimer) {
      passMgr.add(LgcContext::createStartStopTimer(patchTimer, false));
      passMgr.add(LgcContext::createStartStopTimer(optTimer, true));
    }

    // Extra optimizations after NGG primitive shader creation
    passMgr.add(createAlwaysInlinerLegacyPass());
    passMgr.add(createGlobalDCEPass());
    passMgr.add(createPromoteMemoryToRegisterPass());
    passMgr.add(createAggressiveDCEPass());
    passMgr.add(createInstructionCombiningPass());
    passMgr.add(createCFGSimplificationPass());

    // Stop timer for optimization passes and restart timer for patching passes.
    if (patchTimer) {
      passMgr.add(LgcContext::createStartStopTimer(optTimer, false));
      passMgr.add(LgcContext::createStartStopTimer(patchTimer, true));
    }
  }

  passMgr.add(createLegacyPatchImageDerivatives());

  // Set up target features in shader entry-points.
  // NOTE: Needs to be done after post-NGG function inlining, because LLVM refuses to inline something
  // with conflicting attributes. Attributes could conflict on GFX10 because PatchSetupTargetFeatures
  // adds a target feature to determine wave32 or wave64.
  passMgr.add(createLegacyPatchSetupTargetFeatures());

  // Include LLVM IR as a separate section in the ELF binary
  if (pipelineState->getOptions().includeIr)
    passMgr.add(createLegacyPatchLlvmIrInclusion());

  // Stop timer for patching passes.
  if (patchTimer)
    passMgr.add(LgcContext::createStartStopTimer(patchTimer, false));

  // Dump the result
  if (raw_ostream *outs = getLgcOuts()) {
    passMgr.add(
        createPrintModulePass(*outs, "===============================================================================\n"
                                     "// LLPC pipeline patching results\n"));
  }
}

// =====================================================================================================================
// Add optimization passes to pass manager
//
// @param [in/out] passMgr : Pass manager to add passes to
// @param optLevel : The optimization level uses to adjust the aggressiveness of
//                   passes and which passes to add.
void Patch::addOptimizationPasses(lgc::PassManager &passMgr, CodeGenOpt::Level optLevel) {
  LLPC_OUTS("PassManager optimization level = " << optLevel << "\n");

  passMgr.addPass(ForceFunctionAttrsPass());
  FunctionPassManager fpm;
  fpm.addPass(InstCombinePass(1));
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(SROAPass());
  fpm.addPass(EarlyCSEPass(true));
  fpm.addPass(SpeculativeExecutionPass(/* OnlyIfDivergentTarget = */ true));
  fpm.addPass(CorrelatedValuePropagationPass());
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(AggressiveInstCombinePass());
  fpm.addPass(InstCombinePass(1));
  fpm.addPass(PatchPeepholeOpt());
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(ReassociatePass());
  LoopPassManager lpm;
  lpm.addPass(LoopRotatePass());
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 418547
  lpm.addPass(LICMPass());
#else
  lpm.addPass(LICMPass(LICMOptions()));
#endif
  fpm.addPass(createFunctionToLoopPassAdaptor(std::move(lpm), true));
  fpm.addPass(SimplifyCFGPass());
  fpm.addPass(InstCombinePass(1));
  LoopPassManager lpm2;
  lpm2.addPass(IndVarSimplifyPass());
  lpm2.addPass(LoopIdiomRecognizePass());
  lpm2.addPass(LoopDeletionPass());
  fpm.addPass(createFunctionToLoopPassAdaptor(std::move(lpm2), true));
  fpm.addPass(LoopUnrollPass(
      LoopUnrollOptions(optLevel).setPeeling(true).setRuntime(false).setUpperBound(false).setPartial(false)));
  fpm.addPass(ScalarizerPass());
  fpm.addPass(PatchLoadScalarizer());
  fpm.addPass(InstSimplifyPass());
  fpm.addPass(NewGVNPass());
  fpm.addPass(BDCEPass());
  fpm.addPass(InstCombinePass(1));
  fpm.addPass(CorrelatedValuePropagationPass());
  fpm.addPass(ADCEPass());
  fpm.addPass(createFunctionToLoopPassAdaptor(LoopRotatePass()));
  fpm.addPass(SimplifyCFGPass(SimplifyCFGOptions()
                                  .bonusInstThreshold(1)
                                  .forwardSwitchCondToPhi(true)
                                  .convertSwitchToLookupTable(true)
                                  .needCanonicalLoops(true)
                                  .sinkCommonInsts(true)));
  fpm.addPass(LoopUnrollPass(LoopUnrollOptions(optLevel)));
  // uses DivergenceAnalysis
  fpm.addPass(PatchReadFirstLane());
  fpm.addPass(InstCombinePass(1));
  passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  passMgr.addPass(ConstantMergePass());
  FunctionPassManager fpm2;
  fpm2.addPass(DivRemPairsPass());
  fpm2.addPass(SimplifyCFGPass());
  passMgr.addPass(createModuleToFunctionPassAdaptor(std::move(fpm2)));
}

// =====================================================================================================================
// Add optimization passes to pass manager
//
// @param [in/out] passMgr : Pass manager to add passes to
// @param optLevel : The optimization level uses to adjust the aggressiveness of
//                   passes and which passes to add.
void LegacyPatch::addOptimizationPasses(legacy::PassManager &passMgr, CodeGenOpt::Level optLevel) {
  LLPC_OUTS("PassManager optimization level = " << optLevel << "\n");

  passMgr.add(createForceFunctionAttrsLegacyPass());
  passMgr.add(createInstructionCombiningPass(1));
  passMgr.add(createCFGSimplificationPass());
  passMgr.add(createSROAPass());
  passMgr.add(createEarlyCSEPass(true));
  passMgr.add(createSpeculativeExecutionIfHasBranchDivergencePass());
  passMgr.add(createCorrelatedValuePropagationPass());
  passMgr.add(createCFGSimplificationPass());
  passMgr.add(createAggressiveInstCombinerPass());
  passMgr.add(createInstructionCombiningPass(1));
  passMgr.add(createLegacyPatchPeepholeOpt());
  passMgr.add(createCFGSimplificationPass());
  passMgr.add(createReassociatePass());
  passMgr.add(createLoopRotatePass());
  passMgr.add(createLICMPass());
  passMgr.add(createCFGSimplificationPass());
  passMgr.add(createInstructionCombiningPass(1));
  passMgr.add(createIndVarSimplifyPass());
  passMgr.add(createLoopIdiomPass());
  passMgr.add(createLoopDeletionPass());
  passMgr.add(createSimpleLoopUnrollPass(optLevel));
  passMgr.add(createScalarizerPass());
  passMgr.add(createLegacyPatchLoadScalarizer());
  passMgr.add(createInstSimplifyLegacyPass());
  passMgr.add(createNewGVNPass());
  passMgr.add(createBitTrackingDCEPass());
  passMgr.add(createInstructionCombiningPass(1));
  passMgr.add(createCorrelatedValuePropagationPass());
  passMgr.add(createAggressiveDCEPass());
  passMgr.add(createLoopRotatePass());
  passMgr.add(createCFGSimplificationPass(SimplifyCFGOptions()
                                              .bonusInstThreshold(1)
                                              .forwardSwitchCondToPhi(true)
                                              .convertSwitchToLookupTable(true)
                                              .needCanonicalLoops(true)
                                              .sinkCommonInsts(true)));
  passMgr.add(createLoopUnrollPass(optLevel));
  // uses DivergenceAnalysis
  passMgr.add(createLegacyPatchReadFirstLane());
  passMgr.add(createInstructionCombiningPass(1));
  passMgr.add(createConstantMergePass());
  passMgr.add(createDivRemPairsPass());
  passMgr.add(createCFGSimplificationPass());
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
  m_shaderStage = ShaderStageInvalid;
  m_entryPoint = nullptr;
}

// =====================================================================================================================
// Get or create global variable for LDS.
//
// @param pipelineState : Pipeline state
// @param [in/out] module : Module to get or create LDS in
GlobalVariable *Patch::getLdsVariable(PipelineState *pipelineState, Module *module) {
  auto context = &module->getContext();

  static const char *LdsName = "Lds"; // Name of LDS

  // See if this module already has LDS.
  auto oldLds = module->getNamedValue(LdsName);
  if (oldLds) {
    // We already have LDS.
    return cast<GlobalVariable>(oldLds);
  }
  // Now we can create LDS.
  // Construct LDS type: [ldsSize * i32], address space 3
  auto ldsSize = pipelineState->getTargetInfo().getGpuProperty().ldsSizePerThreadGroup;
  auto ldsTy = ArrayType::get(Type::getInt32Ty(*context), ldsSize);

  auto lds = new GlobalVariable(*module, ldsTy, false, GlobalValue::ExternalLinkage, nullptr, LdsName, nullptr,
                                GlobalValue::NotThreadLocal, ADDR_SPACE_LOCAL);
  lds->setAlignment(MaybeAlign(sizeof(unsigned)));
  return lds;
}

} // namespace lgc
