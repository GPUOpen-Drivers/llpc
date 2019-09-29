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
#include "llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"

#include "llvm/CodeGen/CommandFlags.inc"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"

using namespace Llpc;
using namespace llvm;

// -emit-llvm: emit LLVM bitcode instead of ISA
static cl::opt<bool> EmitLlvm("emit-llvm",
                              cl::desc("Emit LLVM bitcode instead of AMD GPU ISA"),
                              cl::init(false));

// =====================================================================================================================
BuilderContext::BuilderContext(
    LLVMContext&  context,              // [in] LLVM context to give each Builder
    bool          useBuilderRecorder)   // True to use BuilderRecorder, false to build directly
    : m_context(context), m_useBuilderRecorder(useBuilderRecorder)
{
}

// =====================================================================================================================
// Set target machine. Returns false on failure (GPU name not found or not supported)
bool BuilderContext::SetTargetMachine(
    StringRef     gpuName)      // LLVM GPU name, e.g. "gfx900"
{
    if (SetTargetInfo(gpuName, &m_targetInfo) == false)
    {
        return false;
    }

    // Get the LLVM target and create the target machine. This should not fail, as we determined above
    // that we support the requested target.
    StringRef triple = "amdgcn--amdpal";
    std::string errMsg;
    const Target* pTarget = TargetRegistry::lookupTarget(triple, errMsg);
    // Allow no signed zeros - this enables omod modifiers (div:2, mul:2)
    TargetOptions targetOpts;
    targetOpts.NoSignedZerosFPMath = true;

    m_pTargetMachine.reset(pTarget->createTargetMachine(triple,
                                                        gpuName,
                                                        "",
                                                        targetOpts,
                                                        Optional<Reloc::Model>()));
    LLPC_ASSERT(m_pTargetMachine);
    return true;
}

// =====================================================================================================================
// Create a Builder object
Builder* BuilderContext::CreateBuilder()
{
    if (m_useBuilderRecorder == false)
    {
        // Generate LLVM IR directly without recording
        return new BuilderImpl(this);
    }
    // Record with BuilderRecorder
    return new BuilderRecorder(this);
}

// =====================================================================================================================
// Create a BuilderImpl object directly, passing in the PipelineState to use.
Builder* BuilderContext::CreateBuilderImpl(
    PipelineState*  pPipelineState)   // [in] PipelineState to use
{
    // Generate LLVM IR directly without recording
    BuilderImpl* pBuilderImpl = new BuilderImpl(this);
    pBuilderImpl->SetPipelineState(pPipelineState);
    return pBuilderImpl;
}

// =====================================================================================================================
// Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
void BuilderContext::AddTargetPasses(
    PassManager&          passMgr,        // [in/out] pass manager to add passes to
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

