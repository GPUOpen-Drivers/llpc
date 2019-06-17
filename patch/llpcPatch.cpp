/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/Bitcode/BitstreamReader.h"
#include "llvm/Bitcode/BitstreamWriter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include <llvm/IR/LegacyPassManager.h>
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h>
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include <llvm/Transforms/IPO/ForceFunctionAttrs.h>
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include <llvm/Transforms/Scalar/GVN.h>
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils.h"
#include "llpcBuilder.h"
#include "llpcContext.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "SPIRVInternal.h"

#define DEBUG_TYPE "llpc-patch"

using namespace llvm;

namespace llvm
{

namespace cl
{

// -disable-patch-opt: disable optimization for LLVM patching
opt<bool> DisablePatchOpt("disable-patch-opt", desc("Disable optimization for LLVM patching"));

// -include-llvm-ir: include LLVM IR as a separate section in the ELF binary
opt<bool> IncludeLlvmIr("include-llvm-ir",
                        desc("Include LLVM IR as a separate section in the ELF binary"),
                        init(false));

// -use-llvm-opt: Use LLVM's standard optimization set instead of the curated optimization set
opt<bool> UseLlvmOpt("use-llvm-opt",
                     desc("Use LLVM's standard optimization set instead of the curated optimization set"),
                     init(false));

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Add patch passes to pass manager for per stage cache
void Patch::AddPrePatchPasses(
    Context*             pContext,     // [in] LLPC context
    legacy::PassManager& passMgr,      // [in/out] Pass manager to add passes to
    llvm::Timer*         pPatchTimer)  // [in] Timer to time patch passes with, nullptr if not timing
{
    // Start timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, true));
    }

    // If using BuilderRecorder rather than BuilderImpl, replay the Builder calls now
    auto pBuilderReplayer = pContext->GetBuilder()->CreateBuilderReplayer();
    if (pBuilderReplayer != nullptr)
    {
        passMgr.add(pBuilderReplayer);
    }

    // Build null fragment shader if necessary
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 30
    if (pContext->GetTargetMachinePipelineOptions()->disableNullFragShader == false)
    {
        passMgr.add(CreatePatchNullFragShader());
    }
#else
    passMgr.add(CreatePatchNullFragShader());
#endif


    // Patch resource collecting, remove inactive resources (should be the first preliminary pass)
    passMgr.add(CreatePatchResourceCollect());

    // Stop timer for optimization passes and restart timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, false));
    }
}

// =====================================================================================================================
// Add whole-pipeline patch passes to pass manager
void Patch::AddPasses(
    Context*              pContext,      // [in] LLPC context
    legacy::PassManager&  passMgr,       // [in/out] Pass manager to add passes to
    uint32_t              skipStageMask, // Mask indicating which shader stages should be skipped in processing
    llvm::Timer*          pPatchTimer,   // [in] Timer to time patch passes with, nullptr if not timing
    llvm::Timer*          pOptTimer)     // [in] Timer to time LLVM optimization passes with, nullptr if not timing
{
    // Start timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, true));
    }

    // Generate copy shader if necessary.
    passMgr.add(CreatePatchCopyShader());

    // Patch entry-point mutation (should be done before external library link)
    passMgr.add(CreatePatchEntryPointMutate());

    // Patch image operations (should be done before external library link)
    passMgr.add(CreatePatchImageOp());

    // Patch push constant loading (should be done before external library link)
    passMgr.add(CreatePatchPushConstOp());

    // Patch group operations (should be done before external library link)
    passMgr.add(CreatePatchGroupOp());

    // Link external libraries and remove dead functions after it
    passMgr.add(CreatePassExternalLibLink(false)); // Not native only
    passMgr.add(CreatePassDeadFuncRemove());

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

    // Stop timer for patching passes and start timer for optimization passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pPatchTimer, false));
        passMgr.add(CreateStartStopTimer(pOptTimer, true));
    }

    // Prepare pipeline ABI but only set the calling conventions to AMDGPU ones for now.
    passMgr.add(CreatePatchPreparePipelineAbi(/* onlySetCallingConvs = */true, skipStageMask));

    // Add some optimization passes

    // Need to run a first promote mem 2 reg to remove alloca's whose only args are lifetimes
    passMgr.add(createPromoteMemoryToRegisterPass());

    if (cl::DisablePatchOpt == false)
    {
        AddOptimizationPasses(pContext, passMgr);

        // Link external libraries to solve LLVM functions after it
        passMgr.add(CreatePassExternalLibLink(false));
        passMgr.add(CreatePassDeadFuncRemove());
    }

    // Stop timer for optimization passes and restart timer for patching passes.
    if (pPatchTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pOptTimer, false));
        passMgr.add(CreateStartStopTimer(pPatchTimer, true));
    }

    // Patch buffer operations (must be after optimizations)
    passMgr.add(CreatePatchBufferOp());
    passMgr.add(createInstructionCombiningPass(false));

    // Fully prepare the pipeline ABI (must be after optimizations)
    passMgr.add(CreatePatchPreparePipelineAbi(/* onlySetCallingConvs = */ false, skipStageMask));

    // Set up target features in shader entry-points.
    passMgr.add(CreatePatchSetupTargetFeatures());

    // Include LLVM IR as a separate section in the ELF binary
    if (cl::IncludeLlvmIr || pContext->GetPipelineContext()->GetPipelineOptions()->includeIr)
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
    Context*              pContext, // [in] LLPC context
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
        passMgr.add(createInstructionCombiningPass(expensiveCombines));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createSROAPass());
        passMgr.add(createEarlyCSEPass(true));
        passMgr.add(createSpeculativeExecutionIfHasBranchDivergencePass());
        passMgr.add(createCorrelatedValuePropagationPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createAggressiveInstCombinerPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createReassociatePass());
        passMgr.add(createLoopRotatePass());
        passMgr.add(createLICMPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines));
        passMgr.add(createIndVarSimplifyPass());
        passMgr.add(createLoopIdiomPass());
        passMgr.add(CreatePatchLoopUnrollInfoRectify());
        passMgr.add(createLoopDeletionPass());
        passMgr.add(createSimpleLoopUnrollPass(optLevel));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createScalarizerPass());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createMergedLoadStoreMotionPass());
        passMgr.add(createGVNPass(disableGvnLoadPre));
        passMgr.add(createSCCPPass());
        passMgr.add(createBitTrackingDCEPass());
        passMgr.add(createInstructionCombiningPass(expensiveCombines));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createCorrelatedValuePropagationPass());
        passMgr.add(createAggressiveDCEPass());
        passMgr.add(createCFGSimplificationPass());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createFloat2IntPass());
        passMgr.add(createLoopRotatePass());
        passMgr.add(createCFGSimplificationPass(1, true, true, false, true));
        passMgr.add(CreatePatchPeepholeOpt());
        passMgr.add(createInstSimplifyLegacyPass());
        passMgr.add(createLoopUnrollPass(optLevel));
        passMgr.add(createInstructionCombiningPass(expensiveCombines));
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
                passMgr.add(CreatePatchPeepholeOpt());

                // Run the scalarizer as it helps our register pressure in the backend significantly. The scalarizer
                // allows us to much more easily identify dead parts of vectors that we do not need to do any
                // computation for.
                passMgr.add(createScalarizerPass());

                // We add an extra inst simplify here to make sure that dead PHI nodes that are easily identified post
                // running the scalarizer can be folded away before instruction combining tries to re-create them.
                passMgr.add(createInstSimplifyLegacyPass());
            });
        passBuilder.addExtension(PassManagerBuilder::EP_LateLoopOptimizations,
                                 [](const PassManagerBuilder&, legacy::PassManagerBase& passMgr)
                                 {
                                     passMgr.add(CreatePatchLoopUnrollInfoRectify());
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
    m_pContext = static_cast<Context*>(&m_pModule->getContext());
    m_shaderStage = ShaderStageInvalid;
    m_pEntryPoint = nullptr;
}

// =====================================================================================================================
// Get or create global variable for LDS.
GlobalVariable* Patch::GetLdsVariable(
    Module* pModule)  // [in/out] Module to get or create LDS in
{
    auto pContext = static_cast<Context*>(&pModule->getContext());

    // See if this module already has LDS.
    auto pOldLds = pModule->getNamedValue("lds");
    if (pOldLds != nullptr)
    {
        // We already have LDS.
        return cast<GlobalVariable>(pOldLds);
    }
    // Now we can create LDS.
    // Construct LDS type: [ldsSize * i32], address space 3
    auto ldsSize = pContext->GetGpuProperty()->ldsSizePerCu;
    auto pLdsTy = ArrayType::get(pContext->Int32Ty(), ldsSize / sizeof(uint32_t));

    auto pLds = new GlobalVariable(*pModule,
                                   pLdsTy,
                                   false,
                                   GlobalValue::ExternalLinkage,
                                   nullptr,
                                   "lds",
                                   nullptr,
                                   GlobalValue::NotThreadLocal,
                                   ADDR_SPACE_LOCAL);
    pLds->setAlignment(sizeof(uint32_t));
    return pLds;
}

} // Llpc
