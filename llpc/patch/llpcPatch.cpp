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
 * @file  llpcPatch.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Patch.
 ***********************************************************************************************************************
 */
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils.h"
#include "llpcBuilder.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "llpcPatchCheckShaderCache.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-patch"

using namespace llvm;

namespace llvm
{

namespace cl
{

// -disable-patch-opt: disable optimization for LLVM patching
opt<bool> DisablePatchOpt("disable-patch-opt", desc("Disable optimization for LLVM patching"));

// -use-llvm-opt: Use LLVM's standard optimization set instead of the curated optimization set
opt<bool> UseLlvmOpt("use-llvm-opt",
                     desc("Use LLVM's standard optimization set instead of the curated optimization set"),
                     init(false));

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Add whole-pipeline patch passes to pass manager
void Patch::AddPasses(
    PipelineState*        pPipelineState, // [in] Pipeline state
    legacy::PassManager&  passMgr,       // [in/out] Pass manager to add passes to
    ModulePass*           pReplayerPass, // [in] BuilderReplayer pass, or nullptr if not needed
    llvm::Timer*          pPatchTimer,   // [in] Timer to time patch passes with, nullptr if not timing
    llvm::Timer*          pOptTimer,     // [in] Timer to time LLVM optimization passes with, nullptr if not timing
    Pipeline::CheckShaderCacheFunc  checkShaderCacheFunc)
                                         // Callback function to check shader cache
{
    // Start timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, true));
    }

    // If using BuilderRecorder rather than BuilderImpl, replay the Builder calls now
    if (pReplayerPass != nullptr)
    {
        passMgr.add(pReplayerPass);
    }

    if (EnableOuts())
    {
        passMgr.add(createPrintModulePass(outs(),
                    "===============================================================================\n"
                    "// LLPC pipeline before-patching results\n"));
    }

    // Build null fragment shader if necessary
    passMgr.add(CreatePatchNullFragShader());

    // Patch resource collecting, remove inactive resources (should be the first preliminary pass)
    passMgr.add(CreatePatchResourceCollect());

    // Generate copy shader if necessary.
    passMgr.add(CreatePatchCopyShader());

    // Patch entry-point mutation (should be done before external library link)
    passMgr.add(CreatePatchEntryPointMutate());

    // Patch push constant loading (should be done before external library link)
    passMgr.add(CreatePatchPushConstOp());

    // Function inlining and remove dead functions after it
    passMgr.add(createAlwaysInlinerLegacyPass());
    passMgr.add(CreatePassDeadFuncRemove());

    // Patch input import and output export operations
    passMgr.add(CreatePatchInOutImportExport());

    // Patch descriptor load operations
    passMgr.add(CreatePatchDescriptorLoad());

    // Prior to general optimization, do function inlining and dead function removal once again
    passMgr.add(createAlwaysInlinerLegacyPass());
    passMgr.add(CreatePassDeadFuncRemove());

    // Check shader cache
    auto pCheckShaderCachePass = CreatePatchCheckShaderCache();
    passMgr.add(pCheckShaderCachePass);
    pCheckShaderCachePass->SetCallbackFunction(checkShaderCacheFunc);

    // Stop timer for patching passes and start timer for optimization passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, false));
        passMgr.add(CreateStartStopTimer(pOptTimer, true));
    }

    // Prepare pipeline ABI but only set the calling conventions to AMDGPU ones for now.
    passMgr.add(CreatePatchPreparePipelineAbi(/* onlySetCallingConvs = */true));

    // Add some optimization passes

    // Need to run a first promote mem 2 reg to remove alloca's whose only args are lifetimes
    passMgr.add(createPromoteMemoryToRegisterPass());

    if (cl::DisablePatchOpt == false)
    {
        AddOptimizationPasses(passMgr);
    }

    // Stop timer for optimization passes and restart timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pOptTimer, false));
        passMgr.add(CreateStartStopTimer(pPatchTimer, true));
    }

    // Patch buffer operations (must be after optimizations)
    passMgr.add(CreatePatchBufferOp());
    passMgr.add(createInstructionCombiningPass(false, 2));

    // Fully prepare the pipeline ABI (must be after optimizations)
    passMgr.add(CreatePatchPreparePipelineAbi(/* onlySetCallingConvs = */ false));

#if LLPC_BUILD_GFX10
    if (pPipelineState->IsGraphics() && (pPipelineState->GetTargetInfo().GetGfxIpVersion().major >= 10) &&
        ((pPipelineState->GetOptions().nggFlags & NggFlagDisable) == 0))
    {
        // Stop timer for patching passes and restart timer for optimization passes.
        if (pPatchTimer != nullptr)
        {
            passMgr.add(CreateStartStopTimer(pPatchTimer, false));
            passMgr.add(CreateStartStopTimer(pOptTimer, true));
        }

        // Extra optimizations after NGG primitive shader creation
        passMgr.add(createAlwaysInlinerLegacyPass());
        passMgr.add(CreatePassDeadFuncRemove());
        passMgr.add(createGlobalDCEPass());
        passMgr.add(createPromoteMemoryToRegisterPass());
        passMgr.add(createAggressiveDCEPass());
        passMgr.add(createInstructionCombiningPass(false));
        passMgr.add(createCFGSimplificationPass());

        // Stop timer for optimization passes and restart timer for patching passes.
        if (pPatchTimer != nullptr)
        {
            passMgr.add(CreateStartStopTimer(pOptTimer, false));
            passMgr.add(CreateStartStopTimer(pPatchTimer, true));
        }
    }
#endif

    // Set up target features in shader entry-points.
#if LLPC_BUILD_GFX10
    // NOTE: Needs to be done after post-NGG function inlining, because LLVM refuses to inline something
    // with conflicting attributes. Attributes could conflict on GFX10 because PatchSetupTargetFeatures
    // adds a target feature to determine wave32 or wave64.
#endif
    passMgr.add(CreatePatchSetupTargetFeatures());

    // Include LLVM IR as a separate section in the ELF binary
    if (pPipelineState->GetOptions().includeIr)
    {
        passMgr.add(CreatePatchLlvmIrInclusion());
    }

    // Stop timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, false));
    }

    // Dump the result
    if (EnableOuts())
    {
        passMgr.add(createPrintModulePass(outs(),
                    "===============================================================================\n"
                    "// LLPC pipeline patching results\n"));
    }
}

// =====================================================================================================================
// Add optimization passes to pass manager
void Patch::AddOptimizationPasses(
    legacy::PassManager&  passMgr)  // [in/out] Pass manager to add passes to
{
    // Set up standard optimization passes.
    if (cl::UseLlvmOpt == false)
    {
        uint32_t optLevel = 3;
        bool expensiveCombines = false;
        bool disableGvnLoadPre = true;

        passMgr.add(createForceFunctionAttrsLegacyPass());
        passMgr.add(createIPSCCPPass());
        passMgr.add(createCalledValuePropagationPass());
        passMgr.add(createGlobalOptimizerPass());
        passMgr.add(createPromoteMemoryToRegisterPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines, 5));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createSROAPass());
        passMgr.add(createEarlyCSEPass(true));
        passMgr.add(createSpeculativeExecutionIfHasBranchDivergencePass());
        passMgr.add(createCorrelatedValuePropagationPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createAggressiveInstCombinerPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines, 3));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createReassociatePass());
        passMgr.add(createLoopRotatePass());
        passMgr.add(createLICMPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines, 2));
        passMgr.add(createIndVarSimplifyPass());
        passMgr.add(createLoopIdiomPass());
        passMgr.add(createLoopDeletionPass());
        passMgr.add(createSimpleLoopUnrollPass(optLevel));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createScalarizerPass());
        passMgr.add(CreatePatchLoadScalarizer());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createMergedLoadStoreMotionPass());
        passMgr.add(createGVNPass(disableGvnLoadPre));
        passMgr.add(createSCCPPass());
        passMgr.add(createBitTrackingDCEPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines, 2));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createCorrelatedValuePropagationPass());
        passMgr.add(createAggressiveDCEPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createFloat2IntPass());
        passMgr.add(createLoopRotatePass());
        passMgr.add(createCFGSimplificationPass(1, true, true, true, true));
        passMgr.add(CreatePatchPeepholeOpt(true));
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createLoopUnrollPass(optLevel));
        passMgr.add(createInstructionCombiningPass(expensiveCombines, 2));
        passMgr.add(createLICMPass());
        passMgr.add(createStripDeadPrototypesPass());
        passMgr.add(createGlobalDCEPass());
        passMgr.add(createConstantMergePass());
        passMgr.add(createLoopSinkPass());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createDivRemPairsPass());
        passMgr.add(createCFGSimplificationPass());
    }
    else
    {
        PassManagerBuilder passBuilder;
        passBuilder.OptLevel = 3; // -O3
        passBuilder.DisableGVNLoadPRE = true;
        passBuilder.DivergentTarget = true;

        passBuilder.addExtension(PassManagerBuilder::EP_Peephole,
                                 [](const PassManagerBuilder&, legacy::PassManagerBase& passMgr)
                                 {
                                     passMgr.add(CreatePatchPeepholeOpt());
                                     passMgr.add(createInstSimplifyLegacyPass());
                                 });
        passBuilder.addExtension(PassManagerBuilder::EP_LoopOptimizerEnd,
            [](const PassManagerBuilder&, legacy::PassManagerBase& passMgr)
            {
                // We run our peephole pass just before the scalarizer to ensure that our simplification optimizations
                // are performed before the scalarizer. One important case this helps with is when you have bit casts
                // whose source is a PHI - we want to make sure that the PHI does not have an i8 type before the
                // scalarizer is called, otherwise a different kind of PHI mess is generated.
                passMgr.add(CreatePatchPeepholeOpt(true));

                // Run the scalarizer as it helps our register pressure in the backend significantly. The scalarizer
                // allows us to much more easily identify dead parts of vectors that we do not need to do any
                // computation for.
                passMgr.add(createScalarizerPass());

                // We add an extra inst simplify here to make sure that dead PHI nodes that are easily identified post
                // running the scalarizer can be folded away before instruction combining tries to re-create them.
                passMgr.add(createInstSimplifyLegacyPass());
            });

        passBuilder.populateModulePassManager(passMgr);
    }
}

// =====================================================================================================================
// Initializes the pass according to the specified module.
//
// NOTE: This function should be called at the beginning of "runOnModule()".
void Patch::Init(
    Module* pModule) // [in] LLVM module
{
    m_pModule  = pModule;
    m_pContext = &m_pModule->getContext();
    m_shaderStage = ShaderStageInvalid;
    m_pEntryPoint = nullptr;
}

// =====================================================================================================================
// Get or create global variable for LDS.
GlobalVariable* Patch::GetLdsVariable(
    PipelineState*  pPipelineState, // [in] Pipeline state
    Module*         pModule)        // [in/out] Module to get or create LDS in
{
    auto pContext = &pModule->getContext();

    // See if this module already has LDS.
    auto pOldLds = pModule->getNamedValue("lds");
    if (pOldLds != nullptr)
    {
        // We already have LDS.
        return cast<GlobalVariable>(pOldLds);
    }
    // Now we can create LDS.
    // Construct LDS type: [ldsSize * i32], address space 3
    auto ldsSize = pPipelineState->GetTargetInfo().GetGpuProperty().ldsSizePerCu;
    auto pLdsTy = ArrayType::get(Type::getInt32Ty(*pContext), ldsSize / sizeof(uint32_t));

    auto pLds = new GlobalVariable(*pModule,
                                   pLdsTy,
                                   false,
                                   GlobalValue::ExternalLinkage,
                                   nullptr,
                                   "lds",
                                   nullptr,
                                   GlobalValue::NotThreadLocal,
                                   ADDR_SPACE_LOCAL);
    pLds->setAlignment(MaybeAlign(sizeof(uint32_t)));
    return pLds;
}

} // Llpc
