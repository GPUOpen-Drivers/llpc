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
 * @file  llpcPatchImageOp.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchImageOp.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-patch-image-op"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcPatchImageOp.h"
#include "llpcPipelineShaders.h"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char PatchImageOp::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching opertions for image operations
ModulePass* CreatePatchImageOp()
{
    return new PatchImageOp();
}

// =====================================================================================================================
PatchImageOp::PatchImageOp()
    :
    Patch(ID)
{
    initializePipelineShadersPass(*PassRegistry::getPassRegistry());
    initializePatchImageOpPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
bool PatchImageOp::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Image-Op\n");

    Patch::Init(&module);

    // Invoke handling of "call" instruction
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (uint32_t shaderStage = 0; shaderStage < ShaderStageCountInternal; ++shaderStage)
    {
        m_pEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStage(shaderStage));
        if (m_pEntryPoint != nullptr)
        {
            m_shaderStage = ShaderStage(shaderStage);
            visit(*m_pEntryPoint);
        }
    }

    for (auto pCallInst: m_imageCalls)
    {
        pCallInst->dropAllReferences();
        pCallInst->eraseFromParent();
    }
    m_imageCalls.clear();

    return true;
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchImageOp::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

    auto mangledName = pCallee->getName();
    if (mangledName.startswith(LlpcName::ImageCallPrefix))
    {
        ShaderImageCallMetadata imageCallMeta = {};
        LLPC_ASSERT(callInst.getNumArgOperands() >= 2);
        uint32_t metaOperandIndex = callInst.getNumArgOperands() - 1; // Image call metadata is last argument
        imageCallMeta.U32All =  cast<ConstantInt>(callInst.getArgOperand(metaOperandIndex))->getZExtValue();

        std::string callName = mangledName.str();
        std::vector<Value*> args;

        if ((imageCallMeta.OpKind == ImageOpQueryNonLod) && (imageCallMeta.Dim == DimBuffer))
        {
            // NOTE: For image buffer, the implementation of query size is different (between GFX6/7 and GFX8).
            const GfxIpVersion gfxIp = m_pContext->GetGfxIpVersion();
            if (gfxIp.major <= 8)
            {
                for (uint32_t i = 0; i < callInst.getNumArgOperands(); ++i)
                {
                    Value* pArg = callInst.getArgOperand(i);
                    args.push_back(pArg);
                }

                callName += (gfxIp.major == 8) ? ".gfx8" : ".gfx6";
                CallInst* pImageCall = cast<CallInst>(EmitCall(m_pModule,
                                                               callName,
                                                               callInst.getType(),
                                                               args,
                                                               NoAttrib,
                                                               &callInst));

                callInst.replaceAllUsesWith(pImageCall);

                m_imageCalls.insert(&callInst);
            }
        }

    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for image operations.
INITIALIZE_PASS(PatchImageOp, DEBUG_TYPE,
                "Patch LLVM for image operations (F-mask support)", false, false)
