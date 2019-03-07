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
 * @file  llpcCodeGenManager.cpp
 * @brief LLPC source file: contains implementation of class Llpc::CodeGenManager.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-code-gen-manager"

#include "llvm/CodeGen/CommandFlags.inc"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "spirvExt.h"
#include "llpcCodeGenManager.h"
#include "llpcContext.h"
#include "llpcElf.h"
#include "llpcFile.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"

namespace llvm
{

namespace cl
{

extern opt<bool> EnablePipelineDump;

extern opt<bool> EnableDynamicLoopUnroll;

// -enable-si-scheduler: enable target option si-scheduler
static opt<bool> EnableSiScheduler("enable-si-scheduler",
                                   desc("Enable target option si-scheduler"),
                                   init(false));

// -disable-fp32-denormals: disable target option fp32-denormals
static opt<bool> DisableFp32Denormals("disable-fp32-denormals",
                                      desc("Disable target option fp32-denormals"),
                                      init(false));

// -emit-llvm: emit LLVM bitcode instead of ISA
static opt<bool> EmitLlvm("emit-llvm",
                          desc("Emit LLVM bitcode instead of AMD GPU ISA"),
                          init(false));

} // cl

} // llvm

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Handler for diagnosis in code generation, derived from the standard one.
class LlpcDiagnosticHandler: public llvm::DiagnosticHandler
{
    bool handleDiagnostics(const DiagnosticInfo &diagInfo) override
    {
        if (EnableOuts() || EnableErrs())
        {
            if ((diagInfo.getSeverity() == DS_Error) || (diagInfo.getSeverity() == DS_Warning))
            {
                DiagnosticPrinterRawOStream printStream(outs());
                printStream << "ERROR: LLVM DIAGNOSIS INFO: ";
                diagInfo.print(printStream);
                printStream << "\n";
                outs().flush();
            }
            else if (EnableOuts())
            {
                DiagnosticPrinterRawOStream printStream(outs());
                printStream << "\n\n=====  LLVM DIAGNOSIS START  =====\n\n";
                diagInfo.print(printStream);
                printStream << "\n\n=====  LLVM DIAGNOSIS END  =====\n\n";
                outs().flush();
            }
        }
        LLPC_ASSERT(diagInfo.getSeverity() != DS_Error);
        return true;
    }
};

// =====================================================================================================================
// Creates the TargetMachine if not already created, and stores it in the context. It then persists as long as
// the context.
Result CodeGenManager::CreateTargetMachine(
    Context*           pContext)  // [in/out] Pipeline context
{
    auto pPipelineOptions = pContext->GetPipelineContext()->GetPipelineOptions();
    if ((pContext->GetTargetMachine() != nullptr) &&
        (pPipelineOptions->includeDisassembly == pContext->GetTargetMachinePipelineOptions()->includeDisassembly) &&
        (pPipelineOptions->autoLayoutDesc == pContext->GetTargetMachinePipelineOptions()->autoLayoutDesc) &&
        (pPipelineOptions->scalarBlockLayout == pContext->GetTargetMachinePipelineOptions()->scalarBlockLayout) &&
        (pPipelineOptions->includeIr == pContext->GetTargetMachinePipelineOptions()->includeIr))
    {
        return Result::Success;
    }

    Result result = Result::ErrorInvalidShader;

    std::string triple("amdgcn--amdpal");

    std::string errMsg;
    auto pTarget = TargetRegistry::lookupTarget(triple, errMsg);
    if (pTarget != nullptr)
    {
        // TODO: We should probably be using InitTargetOptionsFromCodeGenFlags() here.
        // Currently we are not, and it would give an "unused function" warning when compiled with
        // CLANG. So we avoid the warning by referencing it here.
        LLPC_UNUSED(&InitTargetOptionsFromCodeGenFlags);

        TargetOptions targetOpts;
        auto relocModel = Optional<Reloc::Model>();
        std::string features = "";

        // Allow no signed zeros - this enables omod modifiers (div:2, mul:2)
        targetOpts.NoSignedZerosFPMath = true;

        auto pTargetMachine = pTarget->createTargetMachine(triple,
                                                           pContext->GetGpuNameString(),
                                                           features,
                                                           targetOpts,
                                                           relocModel);
        if (pTargetMachine != nullptr)
        {
            pContext->SetTargetMachine(pTargetMachine, pPipelineOptions);
            result = Result::Success;
        }
    }
    if (result != Result::Success)
    {
        LLPC_ERRS("Fails to create AMDGPU target machine: " << errMsg << "\n");
    }

    return result;
}

// =====================================================================================================================
// Setup LLVM target features, target features are set per entry point function.
void CodeGenManager::SetupTargetFeatures(
    Module* pModule)  // [in, out] LLVM module
{
    Context* pContext = static_cast<Context*>(&pModule->getContext());
    auto pPipelineOptions = pContext->GetPipelineContext()->GetPipelineOptions();

    std::string globalFeatures = "";

    if (cl::EnablePipelineDump ||
        EnableOuts() ||
        cl::EnableDynamicLoopUnroll ||
        pPipelineOptions->includeDisassembly)
    {
        globalFeatures += ",+DumpCode";
    }

    if (cl::EnableSiScheduler)
    {
        globalFeatures += ",+si-scheduler";
    }

    if (cl::DisableFp32Denormals)
    {
        globalFeatures += ",-fp32-denormals";
    }

    for (auto pFunc = pModule->begin(), pEnd = pModule->end(); pFunc != pEnd; ++pFunc)
    {
        if ((pFunc->empty() == false) && (pFunc->getLinkage() == GlobalValue::ExternalLinkage))
        {
             std::string targetFeatures(globalFeatures);

             ShaderStage shaderStage = GetShaderStageFromCallingConv(pContext->GetShaderStageMask(),
                                                                     pFunc->getCallingConv());

            auto fp16Control = pContext->GetShaderFloatControl(shaderStage, 16);
            auto fp32Control = pContext->GetShaderFloatControl(shaderStage, 32);
            auto fp64Control = pContext->GetShaderFloatControl(shaderStage, 64);

            if (fp16Control.denormPerserve || fp64Control.denormPerserve)
            {
                targetFeatures += ",+fp64-fp16-denormals";
            }
            else if (fp16Control.denormFlushToZero || fp64Control.denormFlushToZero)
            {
                targetFeatures += ",-fp64-fp16-denormals";
            }

            if (fp32Control.denormPerserve)
            {
                targetFeatures += ",+fp32-denormals";
            }
            else if (fp32Control.denormFlushToZero)
            {
                targetFeatures += ",-fp32-denormals";
            }

            AttrBuilder builder;
            builder.addAttribute("target-features", targetFeatures);
            AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
            pFunc->addAttributes(attribIdx, builder);
        }
    }
}

// =====================================================================================================================
// Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
Result CodeGenManager::AddTargetPasses(
    Context*              pContext,   // [in] LLPC context
    PassManager&          passMgr,    // [in/out] pass manager to add passes to
    raw_pwrite_stream&    outStream)  // [out] Output stream
{
    Result result = Result::Success;

    // Dump the module just before codegen.
    if (EnableOuts())
    {
        passMgr.add(createPrintModulePass(outs(),
                    "===============================================================================\n"
                    "// LLPC final pipeline module info\n"));
    }

    if (cl::EmitLlvm)
    {
        // For -emit-llvm, add a pass to output the LLVM IR, then tell the pass manager to stop adding
        // passes. We do it this way to ensure that we still get the immutable passes from
        // TargetMachine::addPassesToEmitFile, as they can affect LLVM middle-end optimizations.
        passMgr.add(createPrintModulePass(outStream));
        passMgr.stop();
    }

    auto pTargetMachine = pContext->GetTargetMachine();

    pContext->setDiagnosticHandler(llvm::make_unique<LlpcDiagnosticHandler>());

#if LLPC_ENABLE_EXCEPTION
    try
#endif
    {
        if (pTargetMachine->addPassesToEmitFile(passMgr, outStream, nullptr, FileType))
        {
            LLPC_ERRS("Target machine cannot emit a file of this type\n");
            result = Result::ErrorInvalidValue;
        }
    }
#if LLPC_ENABLE_EXCEPTION
    catch (const char*)
    {
        result = Result::ErrorInvalidValue;
    }
#endif

    pContext->setDiagnosticHandlerCallBack(nullptr);
    return result;
}

// =====================================================================================================================
// Runs passes on the module, with the diagnostic handler installed
Result CodeGenManager::Run(
    Module*               pModule,  // [in] LLVM module
    legacy::PassManager&  passMgr)  // [in] Pass manager to run
{
    Result result = Result::Success;

    Context* pContext = static_cast<Context*>(&pModule->getContext());

    pContext->setDiagnosticHandler(llvm::make_unique<LlpcDiagnosticHandler>());

    if (result == Result::Success)
    {
        LLVM_DEBUG(dbgs() << "Start code generation: \n"<< *pModule);

        bool success = false;
#if LLPC_ENABLE_EXCEPTION
        try
#endif
        {
            passMgr.run(*pModule);
            success = true;
        }
#if LLPC_ENABLE_EXCEPTION
        catch (const char*)
        {
            success = false;
        }
#endif

        if (success == false)
        {
            result = Result::ErrorInvalidShader;
        }
    }

    pContext->setDiagnosticHandlerCallBack(nullptr);
    return result;
}

} // Llpc
