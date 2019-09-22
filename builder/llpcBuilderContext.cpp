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
#include "llpcPatch.h"
#include "llpcPipelineState.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/TargetSelect.h"

using namespace Llpc;
using namespace llvm;

namespace llvm
{

class PassRegistry;

} // llvm

#ifndef NDEBUG
static bool Initialized;
#endif

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
// Create the BuilderContext. Returns nullptr on failure.
BuilderContext* BuilderContext::Create(
    LLVMContext&  context)              // [in] LLVM context to give each Builder
{
    LLPC_ASSERT(Initialized && "Must call BuilderContext::Initialize before BuilderContext::Create");

    return new BuilderContext(context);
}

// =====================================================================================================================
BuilderContext::BuilderContext(
    LLVMContext&  context)              // [in] LLVM context to give each Builder
    :
    m_context(context)
{
}

// =====================================================================================================================
// Get the target machine. For now, this gets the TargetMachine from Llpc::Context.
TargetMachine* BuilderContext::GetTargetMachine()
{
    return reinterpret_cast<Context*>(&GetContext())->GetTargetMachine();
}

// =====================================================================================================================
// Get the GfxIpVersion. For now, this gets it from Llpc::Context.
GfxIpVersion BuilderContext::GetGfxIpVersion() const
{
    return reinterpret_cast<Context*>(&GetContext())->GetGfxIpVersion();
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

