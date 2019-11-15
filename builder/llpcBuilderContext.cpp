/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderContext.cpp
 * @brief LLPC source file: implementation of llpc::BuilderContext class for creating and using Llpc::Builder
 ***********************************************************************************************************************
 */
#include "llpcBuilder.h"
#include "llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcPatch.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/CommandFlags.inc"
#include "llvm/InitializePasses.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"

using namespace Llpc;
using namespace llvm;

namespace llvm
{

class PassRegistry;

} // llvm

#ifndef NDEBUG
static bool Initialized;
#endif

// -emit-llvm: emit LLVM bitcode instead of ISA
static cl::opt<bool> EmitLlvm("emit-llvm",
                              cl::desc("Emit LLVM bitcode instead of AMD GPU ISA"),
                              cl::init(false));

// =====================================================================================================================
// Initialize the middle-end. This must be called before the first BuilderContext::Create, although you are
// allowed to call it again after that. It must also be called before LLVM command-line processing, so
// that you can use a pass name in an option such as -print-after. If multiple concurrent compiles are
// possible, this should be called in a thread-safe way.
void BuilderContext::Initialize()
{
#ifndef NDEBUG
    Initialized = true;
#endif

    auto& passRegistry = *PassRegistry::getPassRegistry();

    // Initialize LLVM target: AMDGPU
    LLVMInitializeAMDGPUTargetInfo();
    LLVMInitializeAMDGPUTarget();
    LLVMInitializeAMDGPUTargetMC();
    LLVMInitializeAMDGPUAsmPrinter();
    LLVMInitializeAMDGPUAsmParser();
    LLVMInitializeAMDGPUDisassembler();

    // Initialize special passes which are checked in PassManager
    initializeJumpThreadingPass(passRegistry);
    initializePrintModulePassWrapperPass(passRegistry);

    // Initialize passes so they can be referenced by -llpc-stop-before etc.
    InitializeUtilPasses(passRegistry);
    initializeBuilderReplayerPass(passRegistry);
    InitializePatchPasses(passRegistry);
}

// =====================================================================================================================
// Create the BuilderContext. Returns nullptr on failure to recognize the AMDGPU target whose name is specified
BuilderContext* BuilderContext::Create(
    LLVMContext&  context,              // [in] LLVM context to give each Builder
    StringRef     gpuName)              // LLVM GPU name (e.g. "gfx900"); empty to use -mcpu option setting
{
    LLPC_ASSERT(Initialized && "Must call BuilderContext::Initialize before BuilderContext::Create");

    BuilderContext* pBuilderContext = new BuilderContext(context);

    if (gpuName == "")
    {
        gpuName = MCPU; // -mcpu setting from llvm/CodeGen/CommandFlags.inc
    }

    pBuilderContext->m_pTargetInfo = new TargetInfo;
    if (pBuilderContext->m_pTargetInfo->SetTargetInfo(gpuName) == false)
    {
        delete pBuilderContext;
        return nullptr;
    }

    // Get the LLVM target and create the target machine. This should not fail, as we determined above
    // that we support the requested target.
    StringRef triple = "amdgcn--amdpal";
    std::string errMsg;
    const Target* pTarget = TargetRegistry::lookupTarget(triple, errMsg);
    // Allow no signed zeros - this enables omod modifiers (div:2, mul:2)
    TargetOptions targetOpts;
    targetOpts.NoSignedZerosFPMath = true;

    pBuilderContext->m_pTargetMachine = pTarget->createTargetMachine(triple,
                                                                     gpuName,
                                                                     "",
                                                                     targetOpts,
                                                                     Optional<Reloc::Model>());
    LLPC_ASSERT(pBuilderContext->m_pTargetMachine);
    return pBuilderContext;
}

// =====================================================================================================================
BuilderContext::BuilderContext(
    LLVMContext&  context)              // [in] LLVM context to give each Builder
    :
    m_context(context)
{
}

// =====================================================================================================================
BuilderContext::~BuilderContext()
{
    delete m_pTargetMachine;
    delete m_pTargetInfo;
}

// =====================================================================================================================
// Create a Pipeline object for a pipeline compile.
// This actually creates a PipelineState, but returns the Pipeline superclass that is visible to
// the front-end.
Pipeline* BuilderContext::CreatePipeline()
{
    return new PipelineState(this);
}

// =====================================================================================================================
// Create a Builder object. For a shader compile (pPipeline is nullptr), useBuilderRecorder is ignored
// because it always uses BuilderRecorder.
Builder* BuilderContext::CreateBuilder(
    Pipeline*   pPipeline,          // [in] Pipeline object for pipeline compile, nullptr for shader compile
    bool        useBuilderRecorder) // true to use BuilderRecorder, false to use BuilderImpl
{
    if ((pPipeline == nullptr) || useBuilderRecorder)
    {
        return new BuilderRecorder(this, pPipeline);
    }
    return new BuilderImpl(this, pPipeline);
}

// =====================================================================================================================
// Prepare a pass manager. This manually adds a target-aware TLI pass, so middle-end optimizations do not think that
// we have library functions.
void BuilderContext::PreparePassManager(
    legacy::PassManager*  pPassMgr)   // [in/out] Pass manager
{
    TargetLibraryInfoImpl targetLibInfo(GetTargetMachine()->getTargetTriple());

    // Adjust it to allow memcpy and memset.
    // TODO: Investigate why the latter is necessary. I found that
    // test/shaderdb/ObjStorageBlock_TestMemCpyInt32.comp
    // got unrolled far too much, and at too late a stage for the descriptor loads to be commoned up. It might
    // be an unfortunate interaction between LoopIdiomRecognize and fat pointer laundering.
    targetLibInfo.setAvailable(LibFunc_memcpy);
    targetLibInfo.setAvailable(LibFunc_memset);

    // Also disallow tan functions.
    // TODO: This can be removed once we have LLVM fix D67406.
    targetLibInfo.setUnavailable(LibFunc_tan);
    targetLibInfo.setUnavailable(LibFunc_tanf);
    targetLibInfo.setUnavailable(LibFunc_tanl);

    auto pTargetLibInfoPass = new TargetLibraryInfoWrapperPass(targetLibInfo);
    pPassMgr->add(pTargetLibInfoPass);
}

// =====================================================================================================================
// Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
void BuilderContext::AddTargetPasses(
    Llpc::PassManager&    passMgr,        // [in/out] pass manager to add passes to
    Timer*                pCodeGenTimer,  // [in] Timer to time target passes with, nullptr if not timing
    raw_pwrite_stream&    outStream)      // [out] Output stream
{
    // Start timer for codegen passes.
    if (pCodeGenTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pCodeGenTimer, true));
    }

    // Dump the module just before codegen.
    if (EnableOuts())
    {
        passMgr.add(createPrintModulePass(outs(),
                    "===============================================================================\n"
                    "// LLPC final pipeline module info\n"));
    }

    if (EmitLlvm)
    {
        // For -emit-llvm, add a pass to output the LLVM IR, then tell the pass manager to stop adding
        // passes. We do it this way to ensure that we still get the immutable passes from
        // TargetMachine::addPassesToEmitFile, as they can affect LLVM middle-end optimizations.
        passMgr.add(createPrintModulePass(outStream));
        passMgr.stop();
    }

    // TODO: We should probably be using InitTargetOptionsFromCodeGenFlags() here.
    // Currently we are not, and it would give an "unused function" warning when compiled with
    // CLANG. So we avoid the warning by referencing it here.
    LLPC_UNUSED(&InitTargetOptionsFromCodeGenFlags);

    if (GetTargetMachine()->addPassesToEmitFile(passMgr, outStream, nullptr, FileType))
    {
        report_fatal_error("Target machine cannot emit a file of this type");
    }

    // Stop timer for codegen passes.
    if (pCodeGenTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pCodeGenTimer, false));
    }
}

