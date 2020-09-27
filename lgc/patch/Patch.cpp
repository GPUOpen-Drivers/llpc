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
 * @file  Patch.cpp
 * @brief LLPC source file: contains implementation of class lgc::Patch.
 ***********************************************************************************************************************
 */
#include "lgc/patch/Patch.h"
#include "PatchCheckShaderCache.h"
#include "lgc/LgcContext.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Debug.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/NewGVN.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils.h"

#define DEBUG_TYPE "lgc-patch"

using namespace llvm;

namespace llvm {

namespace cl {

// -disable-patch-opt: disable optimization for LLVM patching
opt<bool> DisablePatchOpt("disable-patch-opt", desc("Disable optimization for LLVM patching"));

// -use-llvm-opt: Use LLVM's standard optimization set instead of the curated optimization set
opt<bool> UseLlvmOpt("use-llvm-opt",
                     desc("Use LLVM's standard optimization set instead of the curated optimization set"), init(false));

// -opt: Set the optimization level
opt<CodeGenOpt::Level> OptLevel("opt", desc("Set the optimization level:"), init(CodeGenOpt::Default),
                                values(clEnumValN(CodeGenOpt::None, "none", "no optimizations"),
                                       clEnumValN(CodeGenOpt::Less, "quick", "quick compilation time"),
                                       clEnumValN(CodeGenOpt::Default, "default", "default optimizations"),
                                       clEnumValN(CodeGenOpt::Aggressive, "fast", "fast execution time")));

} // namespace cl

} // namespace llvm

namespace lgc {

// =====================================================================================================================
// Add whole-pipeline patch passes to pass manager
//
// @param pipelineState : Pipeline state
// @param [in/out] passMgr : Pass manager to add passes to
// @param replayerPass : BuilderReplayer pass, or nullptr if not needed
// @param patchTimer : Timer to time patch passes with, nullptr if not timing
// @param optTimer : Timer to time LLVM optimization passes with, nullptr if not timing
void Patch::addPasses(PipelineState *pipelineState, legacy::PassManager &passMgr, ModulePass *replayerPass,
                      Timer *patchTimer, Timer *optTimer, Pipeline::CheckShaderCacheFunc checkShaderCacheFunc)
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
  passMgr.add(createPatchNullFragShader());

  // Patch resource collecting, remove inactive resources (should be the first preliminary pass)
  passMgr.add(createPatchResourceCollect());

  // Patch workarounds
  passMgr.add(createPatchWorkarounds());

  // Generate copy shader if necessary.
  passMgr.add(createPatchCopyShader());

  // Lower vertex fetch operations.
  passMgr.add(createLowerVertexFetch());

  // Lower fragment export operations.
  passMgr.add(createLowerFragColorExport());

  // Patch entry-point mutation (should be done before external library link)
  passMgr.add(createPatchEntryPointMutate());

  // Function inlining and remove dead functions after it
  passMgr.add(createAlwaysInlinerLegacyPass());
  passMgr.add(createGlobalDCEPass());

  // Patch input import and output export operations
  passMgr.add(createPatchInOutImportExport());

  // Prior to general optimization, do function inlining and dead function removal once again
  passMgr.add(createAlwaysInlinerLegacyPass());
  passMgr.add(createGlobalDCEPass());

  // Patch loop metadata
  passMgr.add(createPatchLoopMetadata());

  // Check shader cache
  auto checkShaderCachePass = createPatchCheckShaderCache();
  passMgr.add(checkShaderCachePass);
  checkShaderCachePass->setCallbackFunction(checkShaderCacheFunc);

  // Stop timer for patching passes and start timer for optimization passes.
  if (patchTimer) {
    passMgr.add(LgcContext::createStartStopTimer(patchTimer, false));
    passMgr.add(LgcContext::createStartStopTimer(optTimer, true));
  }

  // Prepare pipeline ABI but only set the calling conventions to AMDGPU ones for now.
  passMgr.add(createPatchPreparePipelineAbi(/* onlySetCallingConvs = */ true));

  // Add some optimization passes

  // Need to run a first promote mem 2 reg to remove alloca's whose only args are lifetimes
  passMgr.add(createPromoteMemoryToRegisterPass());

  if (!cl::DisablePatchOpt)
    addOptimizationPasses(passMgr);

  // Stop timer for optimization passes and restart timer for patching passes.
  if (patchTimer) {
    passMgr.add(LgcContext::createStartStopTimer(optTimer, false));
    passMgr.add(LgcContext::createStartStopTimer(patchTimer, true));
  }

  // Patch buffer operations (must be after optimizations)
  passMgr.add(createPatchBufferOp());
  passMgr.add(createInstructionCombiningPass(2));

  // Fully prepare the pipeline ABI (must be after optimizations)
  passMgr.add(createPatchPreparePipelineAbi(/* onlySetCallingConvs = */ false));

  if (pipelineState->isGraphics() && pipelineState->getTargetInfo().getGfxIpVersion().major >= 10 &&
      (pipelineState->getOptions().nggFlags & NggFlagDisable) == 0) {
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

  // Set up target features in shader entry-points.
  // NOTE: Needs to be done after post-NGG function inlining, because LLVM refuses to inline something
  // with conflicting attributes. Attributes could conflict on GFX10 because PatchSetupTargetFeatures
  // adds a target feature to determine wave32 or wave64.
  passMgr.add(createPatchSetupTargetFeatures());

  // Include LLVM IR as a separate section in the ELF binary
  if (pipelineState->getOptions().includeIr)
    passMgr.add(createPatchLlvmIrInclusion());

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
void Patch::addOptimizationPasses(legacy::PassManager &passMgr) {
  LLPC_OUTS("PassManager optimization level = " << cl::OptLevel << "\n");

  // Set up standard optimization passes.
  if (!cl::UseLlvmOpt) {
    passMgr.add(createForceFunctionAttrsLegacyPass());
    passMgr.add(createIPSCCPPass());
    passMgr.add(createCalledValuePropagationPass());
    passMgr.add(createGlobalOptimizerPass());
    passMgr.add(createPromoteMemoryToRegisterPass());
    passMgr.add(createInstructionCombiningPass(5));
    passMgr.add(createPatchPeepholeOpt());
    passMgr.add(createInstSimplifyLegacyPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createSROAPass());
    passMgr.add(createEarlyCSEPass(true));
    passMgr.add(createSpeculativeExecutionIfHasBranchDivergencePass());
    passMgr.add(createCorrelatedValuePropagationPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createAggressiveInstCombinerPass());
    passMgr.add(createInstructionCombiningPass(3));
    passMgr.add(createPatchPeepholeOpt());
    passMgr.add(createInstSimplifyLegacyPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createReassociatePass());
    passMgr.add(createLoopRotatePass());
    passMgr.add(createLICMPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createInstructionCombiningPass(2));
    passMgr.add(createIndVarSimplifyPass());
    passMgr.add(createLoopIdiomPass());
    passMgr.add(createLoopDeletionPass());
    passMgr.add(createSimpleLoopUnrollPass(cl::OptLevel));
    passMgr.add(createPatchPeepholeOpt());
    passMgr.add(createScalarizerPass());
    passMgr.add(createPatchLoadScalarizer());
    passMgr.add(createInstSimplifyLegacyPass());
    passMgr.add(createMergedLoadStoreMotionPass());
    passMgr.add(createNewGVNPass());
    passMgr.add(createSCCPPass());
    passMgr.add(createBitTrackingDCEPass());
    passMgr.add(createInstructionCombiningPass(2));
    passMgr.add(createPatchPeepholeOpt());
    passMgr.add(createCorrelatedValuePropagationPass());
    passMgr.add(createAggressiveDCEPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createInstSimplifyLegacyPass());
    passMgr.add(createFloat2IntPass());
    passMgr.add(createLoopRotatePass());
    passMgr.add(createCFGSimplificationPass(SimplifyCFGOptions()
                                                .bonusInstThreshold(1)
                                                .forwardSwitchCondToPhi(true)
                                                .convertSwitchToLookupTable(true)
                                                .needCanonicalLoops(true)
                                                .sinkCommonInsts(true)));
    passMgr.add(createPatchPeepholeOpt());
    passMgr.add(createInstSimplifyLegacyPass());
    passMgr.add(createLoopUnrollPass(cl::OptLevel));
    // uses DivergenceAnalysis
    passMgr.add(createPatchReadFirstLane());
    passMgr.add(createInstructionCombiningPass(2));
    passMgr.add(createLICMPass());
    passMgr.add(createStripDeadPrototypesPass());
    passMgr.add(createGlobalDCEPass());
    passMgr.add(createConstantMergePass());
    passMgr.add(createLoopSinkPass());
    passMgr.add(createInstSimplifyLegacyPass());
    passMgr.add(createDivRemPairsPass());
    passMgr.add(createCFGSimplificationPass());
  } else {
    PassManagerBuilder passBuilder;
    passBuilder.OptLevel = cl::OptLevel;
    passBuilder.DisableGVNLoadPRE = true;
    passBuilder.DivergentTarget = true;

    passBuilder.addExtension(PassManagerBuilder::EP_Peephole,
                             [](const PassManagerBuilder &, legacy::PassManagerBase &passMgr) {
                               passMgr.add(createPatchPeepholeOpt());
                               passMgr.add(createInstSimplifyLegacyPass());
                             });
    passBuilder.addExtension(PassManagerBuilder::EP_LoopOptimizerEnd,
                             [](const PassManagerBuilder &, legacy::PassManagerBase &passMgr) {
                               // We run our peephole pass just before the scalarizer to ensure that our simplification
                               // optimizations are performed before the scalarizer. One important case this helps with
                               // is when you have bit casts whose source is a PHI - we want to make sure that the PHI
                               // does not have an i8 type before the scalarizer is called, otherwise a different kind
                               // of PHI mess is generated.
                               passMgr.add(createPatchPeepholeOpt());

                               // Run the scalarizer as it helps our register pressure in the backend significantly. The
                               // scalarizer allows us to much more easily identify dead parts of vectors that we do not
                               // need to do any computation for.
                               passMgr.add(createScalarizerPass());

                               // We add an extra inst simplify here to make sure that dead PHI nodes that are easily
                               // identified post running the scalarizer can be folded away before instruction combining
                               // tries to re-create them.
                               passMgr.add(createInstSimplifyLegacyPass());
                             });

    passBuilder.populateModulePassManager(passMgr);
  }
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

  // See if this module already has LDS.
  auto oldLds = module->getNamedValue("lds");
  if (oldLds) {
    // We already have LDS.
    return cast<GlobalVariable>(oldLds);
  }
  // Now we can create LDS.
  // Construct LDS type: [ldsSize * i32], address space 3
  auto ldsSize = pipelineState->getTargetInfo().getGpuProperty().ldsSizePerCu;
  auto ldsTy = ArrayType::get(Type::getInt32Ty(*context), ldsSize / sizeof(unsigned));

  auto lds = new GlobalVariable(*module, ldsTy, false, GlobalValue::ExternalLinkage, nullptr, "lds", nullptr,
                                GlobalValue::NotThreadLocal, ADDR_SPACE_LOCAL);
  lds->setAlignment(MaybeAlign(sizeof(unsigned)));
  return lds;
}

} // namespace lgc
