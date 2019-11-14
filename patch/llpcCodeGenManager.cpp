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
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "llpcCodeGenManager.h"
#include "llpcContext.h"
#include "llpcElfReader.h"
#include "llpcFile.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcPipelineState.h"

namespace llvm
{

namespace cl
{

extern opt<bool> EnablePipelineDump;

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
// Creates the TargetMachine if not already created, and stores it in the context. It then persists as long as
// the context.
Result CodeGenManager::CreateTargetMachine(
    Context*               pContext,          // [in/out] Pipeline context
    const PipelineOptions* pPipelineOptions)  // [in] Pipeline options
{
    if ((pContext->GetTargetMachine() != nullptr) &&
        (pPipelineOptions->includeDisassembly == pContext->GetTargetMachinePipelineOptions()->includeDisassembly) &&
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION < 30
        (pPipelineOptions->autoLayoutDesc == pContext->GetTargetMachinePipelineOptions()->autoLayoutDesc) &&
#endif
        (pPipelineOptions->scalarBlockLayout == pContext->GetTargetMachinePipelineOptions()->scalarBlockLayout) &&
        (pPipelineOptions->includeIr == pContext->GetTargetMachinePipelineOptions()->includeIr)
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 23
        && (pPipelineOptions->robustBufferAccess == pContext->GetTargetMachinePipelineOptions()->robustBufferAccess)
#endif
        )
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
    PipelineState*      pPipelineState, // [in] Pipeline state
    Module*             pModule)        // [in, out] LLVM module
{
    Context* pContext = static_cast<Context*>(&pModule->getContext());
    auto pPipelineOptions = pContext->GetPipelineContext()->GetPipelineOptions();

    std::string globalFeatures = "";

    if (cl::EnablePipelineDump ||
        EnableOuts() ||
        pPipelineOptions->includeDisassembly)
    {
        globalFeatures += ",+DumpCode";
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
             AttrBuilder builder;

             ShaderStage shaderStage = GetShaderStageFromCallingConv(pContext->GetShaderStageMask(),
                                                                     pFunc->getCallingConv());

            bool useSiScheduler = cl::EnableSiScheduler;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 28
            if (shaderStage != ShaderStageCopyShader)
            {
                auto pShaderOptions = pContext->GetPipelineShaderInfo(shaderStage)->options;
                useSiScheduler |= pShaderOptions.useSiScheduler;
            }
#endif
            if (useSiScheduler)
            {
                // It was found that enabling both SIScheduler and SIFormClauses was bad on one particular
                // game. So we disable the latter here. That only affects XNACK targets.
                targetFeatures += ",+si-scheduler";
                builder.addAttribute("amdgpu-max-memory-clause", "1");
            }

#if LLPC_BUILD_GFX10
            if (pFunc->getCallingConv() == CallingConv::AMDGPU_GS)
            {
                // NOTE: For NGG primitive shader, enable 128-bit LDS load/store operations to optimize gvec4 data
                // read/write. This usage must enable the feature of using CI+ additional instructions.
                const auto pNggControl = pPipelineState->GetNggControl();
                if (pNggControl->enableNgg && (pNggControl->passthroughMode == false))
                {
                    targetFeatures += ",+ci-insts,+enable-ds128";
                }
            }
#endif
            if (pFunc->getCallingConv() == CallingConv::AMDGPU_HS)
            {
                // Force s_barrier to be present (ignore optimization)
                builder.addAttribute("amdgpu-flat-work-group-size", "128,128");
            }
            if (pFunc->getCallingConv() == CallingConv::AMDGPU_CS)
            {
                // Set the work group size
                const auto& csBuiltInUsage = pContext->GetShaderResourceUsage(ShaderStageCompute)->builtInUsage.cs;
                uint32_t flatWorkGroupSize =
                    csBuiltInUsage.workgroupSizeX * csBuiltInUsage.workgroupSizeY * csBuiltInUsage.workgroupSizeZ;
                auto flatWorkGroupSizeString = std::to_string(flatWorkGroupSize);
                builder.addAttribute("amdgpu-flat-work-group-size",
                                     flatWorkGroupSizeString + "," + flatWorkGroupSizeString);
            }

            auto gfxIp = pContext->GetGfxIpVersion();
            if (gfxIp.major >= 9)
            {
                targetFeatures += ",+enable-scratch-bounds-checks";
            }

#if LLPC_BUILD_GFX10
            if (gfxIp.major >= 10)
            {
                // Setup wavefront size per shader stage
                uint32_t waveSize = pContext->GetShaderWaveSize(shaderStage);

                targetFeatures += ",+wavefrontsize" + std::to_string(waveSize);

                // Allow driver setting for WGP by forcing backend to set 0
                // which is then OR'ed with the driver set value
                targetFeatures += ",+cumode";
            }
#endif

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

            builder.addAttribute("target-features", targetFeatures);
            AttributeList::AttrIndex attribIdx = AttributeList::AttrIndex(AttributeList::FunctionIndex);
            pFunc->addAttributes(attribIdx, builder);
        }
    }
}

// =====================================================================================================================
// Adds target passes to pass manager, depending on "-filetype" and "-emit-llvm" options
void CodeGenManager::AddTargetPasses(
    Context*              pContext,      // [in] LLPC context
    PassManager&          passMgr,       // [in/out] pass manager to add passes to
    Timer*                pCodeGenTimer, // [in] Timer to time target passes with, nullptr if not timing
    raw_pwrite_stream&    outStream)     // [out] Output stream
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

    if (cl::EmitLlvm)
    {
        // For -emit-llvm, add a pass to output the LLVM IR, then tell the pass manager to stop adding
        // passes. We do it this way to ensure that we still get the immutable passes from
        // TargetMachine::addPassesToEmitFile, as they can affect LLVM middle-end optimizations.
        passMgr.add(createPrintModulePass(outStream));
        if (pCodeGenTimer != nullptr)
        {
            passMgr.add(CreateStartStopTimer(pCodeGenTimer, false));
        }
        passMgr.stop();
    }

    auto pTargetMachine = pContext->GetTargetMachine();

    if (pTargetMachine->addPassesToEmitFile(passMgr, outStream, nullptr, FileType))
    {
        report_fatal_error("Target machine cannot emit a file of this type");
    }

    // Stop timer for codegen passes.
    if (pCodeGenTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pCodeGenTimer, false));
    }
}

} // Llpc
