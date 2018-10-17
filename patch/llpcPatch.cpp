/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llpcContext.h"
#include "llpcInternal.h"
#include "llpcPassDeadFuncRemove.h"
#include "llpcPassExternalLibLink.h"
#include "llpcPatch.h"
#include "llpcPatchAddrSpaceMutate.h"
#include "llpcPatchBufferOp.h"
#include "llpcPatchDescriptorLoad.h"
#include "llpcPatchEntryPointMutate.h"
#include "llpcPatchGroupOp.h"
#include "llpcPatchImageOp.h"
#include "llpcPatchInOutImportExport.h"
#include "llpcPatchPushConstOp.h"
#include "llpcPatchResourceCollect.h"

#define DEBUG_TYPE "llpc-patch"

using namespace llvm;

namespace llvm
{

namespace cl
{

// -auto-layout-desc: automatically create descriptor layout based on resource usages
opt<bool> AutoLayoutDesc("auto-layout-desc",
                         desc("Automatically create descriptor layout based on resource usages"));

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Executes LLVM preliminary patching opertions for LLVM module.
Result Patch::PreRun(
    Module* pModule)    // [in,out] LLVM module to be run on
{
    Result result = Result::Success;

    auto pContext = static_cast<Context*>(&pModule->getContext());
    ShaderStage shaderStage = GetShaderStageFromModule(pModule);

    if (cl::AutoLayoutDesc)
    {
        // Automatically layout descriptor
        pContext->AutoLayoutDescriptor(shaderStage);
    }

    // Do patching opertions
    if (result == Result::Success)
    {
        legacy::PassManager passMgr;

        // Patch resource collecting, remove inactive resources (should be the first preliminary pass)
        passMgr.add(PatchResourceCollect::Create());

        if (passMgr.run(*pModule) == false)
        {
            result = Result::ErrorInvalidShader;
        }
    }

    return result;
}

// =====================================================================================================================
// Executes LLVM patching opertions for LLVM module and links it with external LLVM libraries.
Result Patch::Run(
    Module* pModule)    // [in,out] LLVM module to be run on
{
    Result result = Result::Success;
    Context* pContext = static_cast<Context*>(&pModule->getContext());
    // Do patching opertions
    legacy::PassManager passMgr;

    // Lower SPIRAS address spaces to AMDGPU address spaces.
    passMgr.add(PatchAddrSpaceMutate::Create());

    // Patch entry-point mutation (should be done before external library link)
    passMgr.add(PatchEntryPointMutate::Create());

    // Patch image operations (should be done before external library link)
    passMgr.add(PatchImageOp::Create());

    // Patch push constant loading (should be done before external library link)
    passMgr.add(PatchPushConstOp::Create());

    // Patch buffer operations (should be done before external library link)
    passMgr.add(PatchBufferOp::Create());

    // Patch group operations(should be done before external library link)
    passMgr.add(PatchGroupOp::Create());

    // Link external libraries and remove dead functions after it
    passMgr.add(PassExternalLibLink::Create(false)); // Not native only
    passMgr.add(PassDeadFuncRemove::Create());

    // Function inlining and remove dead functions after it
    passMgr.add(createFunctionInliningPass(InlineThreshold));
    passMgr.add(PassDeadFuncRemove::Create());

    // Patch input import and output export operations
    passMgr.add(PatchInOutImportExport::Create());

    // Patch descriptor load opertions
    passMgr.add(PatchDescriptorLoad::Create());

    // Prior to general optimization, do funcion inlining and dead function removal once again
    passMgr.add(createFunctionInliningPass(InlineThreshold));
    passMgr.add(PassDeadFuncRemove::Create());

    // Add some optimization passes
    passMgr.add(createPromoteMemoryToRegisterPass());
    passMgr.add(createSROAPass());
    passMgr.add(createLICMPass());
    passMgr.add(createAggressiveDCEPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createInstructionCombiningPass());

    if (passMgr.run(*pModule) == false)
    {
        result = Result::ErrorInvalidShader;
    }

    if (result == Result::Success)
    {
        std::string errMsg;
        raw_string_ostream errStream(errMsg);
        if (verifyModule(*pModule, &errStream))
        {
            LLPC_ERRS("Fails to verify module (" DEBUG_TYPE "): " << errStream.str() << "\n");
            result = Result::ErrorInvalidShader;
        }
    }

    return result;
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
    m_shaderStage = GetShaderStageFromModule(m_pModule);
    m_pEntryPoint = GetEntryPoint(m_pModule);
}

// =====================================================================================================================
// Expandd non-uniform index with waterfall intrinsics.
//
// Example:
//    %callValue = call <4 xi8> @llpc.buffer.load.uniform.v4i8(i32 %descSet, i32 %binding, i32 %blockOffset,
//        i32 %memberOffset, i1 %readonly, i1 %glc, i1 %slc, i1 1)
//    ==>
//    %waterfallBegin = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 %blockOffset)
//    %uniformIndex = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %waterfallBegin, i32 %blockOffset)
//    %callValue = call <4 xi8> @llpc.buffer.load.uniform.v4i8(i32 %descSet, i32 %binding, i32 %uniformIndex,
//        i32 %memberOffset, i1 %readonly, i1 %glc, i1 %slc, i1 1)
//    %callValue.i32 = bitcast <4 x i8> %callValue to i32
//    %waterfallEnd = call i32 @llvm.amdgcn.waterfall.end.i32(i32 %waterfallBegin, i32 %callValue.i32)
//    %result = bitcast i32 %waterfallEnd to <4 x i8>
//
void Patch::AddWaterFallInst(
    int32_t   nonUniformIndex1,     // [in] Operand index of first non-uniform index in "pCallInst"
    int32_t   nonUniformIndex2,     // [in] Operand index of second non-uniform index in "pCallInst"
    CallInst* pCallInst)            // [in] Pointer to call instruction which need expand with waterfall intrinsics
{
    LLPC_ASSERT(nonUniformIndex1 > 0);

    Value* pNonUniformIndex1 = pCallInst->getOperand(nonUniformIndex1);
    Value* pNonUniformIndex2 = nullptr;
    Value* pNonUniformIndex = pNonUniformIndex1;
    bool isVector = false;

    if (nonUniformIndex2 >=  0)
    {
        pNonUniformIndex2 = pCallInst->getOperand(nonUniformIndex2);
        if (pNonUniformIndex2 != pNonUniformIndex1)
        {
            // Merge the secondary non-uniform index with the first one.
            pNonUniformIndex = InsertElementInst::Create(UndefValue::get(m_pContext->Int32x2Ty()),
                                                         pNonUniformIndex1,
                                                         getInt32(m_pModule, 0),
                                                         "",
                                                         pCallInst);
            pNonUniformIndex = InsertElementInst::Create(pNonUniformIndex,
                                                         pNonUniformIndex2,
                                                         getInt32(m_pModule, 1),
                                                         "",
                                                         pCallInst);
            isVector = true;
        }
    }

    // %waterfallBegin = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 %nonUniformIndex)
    auto pWaterfallBegin = EmitCall(m_pModule,
                                    isVector ? "llvm.amdgcn.waterfall.begin.v2i32" : "llvm.amdgcn.waterfall.begin.i32",
                                    m_pContext->Int32Ty(),
                                    { pNonUniformIndex },
                                    NoAttrib,
                                    pCallInst);

    // %uniformIndex = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %waterfallBegin, i32 %nonUniformIndex)
    auto pUniformIndex = EmitCall(m_pModule,
                                  isVector ? "llvm.amdgcn.waterfall.readfirstlane.v2i32.v2i32" :
                                             "llvm.amdgcn.waterfall.readfirstlane.i32.i32",
                                  pNonUniformIndex->getType(),
                                  { pWaterfallBegin, pNonUniformIndex },
                                  NoAttrib,
                                  pCallInst);

    // Replace non-uniform index in pCallInst with uniform index
    if (isVector)
    {
        auto pUniformIndex1 = ExtractElementInst::Create(pUniformIndex, getInt32(m_pModule, 0), "", pCallInst);
        auto pUniformIndex2 = ExtractElementInst::Create(pUniformIndex, getInt32(m_pModule, 1), "", pCallInst);
        pCallInst->setOperand(nonUniformIndex1, pUniformIndex1);
        pCallInst->setOperand(nonUniformIndex2, pUniformIndex2);
    }
    else
    {
        pCallInst->setOperand(nonUniformIndex1, pUniformIndex);
        if (nonUniformIndex2 >= 0)
        {
            pCallInst->setOperand(nonUniformIndex2, pUniformIndex);
        }
    }

    // Insert waterfall.end after pCallInst
    auto pNextInst = pCallInst->getNextNode();
    auto pResultTy = pCallInst->getType();
    Type* pWaterfallEndTy = nullptr;

    if (pResultTy->isVoidTy() == false)
    {
        auto resultBitSize = pResultTy->getPrimitiveSizeInBits();
        LLPC_ASSERT(pNextInst != nullptr);

        // waterfall.end only support vector up to 8, we need check the type and cast it if necessary
        if ((resultBitSize % 32) == 0)
        {
            pWaterfallEndTy = (resultBitSize == 32) ?
                              m_pContext->Int32Ty() :
                              VectorType::get(m_pContext->Int32Ty(), resultBitSize / 32);
            LLPC_ASSERT((resultBitSize / 32) <= 8);
        }
        else
        {
            LLPC_ASSERT((resultBitSize % 16) == 0);
            pWaterfallEndTy = (resultBitSize == 16) ?
                               m_pContext->Int16Ty() :
                               VectorType::get(m_pContext->Int16Ty(), resultBitSize / 16);
            LLPC_ASSERT((resultBitSize / 32) <= 8);
        }

        // %waterfallEnd = call i32 @llvm.amdgcn.waterfall.end.i32(i32 %waterfallBegin, i32 %callValue)
        std::string waterfallEnd = "llvm.amdgcn.waterfall.end.";
        waterfallEnd += GetTypeNameForScalarOrVector(pWaterfallEndTy);
        Value* pResult = nullptr;

        // Insert waterfall.end before the next LLVM instructions
        if (pWaterfallEndTy != pResultTy)
        {
            // Do type cast
            Value* pCallValue = nullptr;

            pCallValue = new BitCastInst(pCallInst, pWaterfallEndTy, "", pNextInst);
            // Add waterfall.end
            auto pWaterfallEnd = EmitCall(m_pModule,
                                      waterfallEnd,
                                      pWaterfallEndTy,
                                      { pWaterfallBegin, pCallValue },
                                      NoAttrib,
                                      pNextInst);
            // Restore type
            pResult = new BitCastInst(pWaterfallEnd, pResultTy, "", pNextInst);

            // Replace all users of call inst with the result of waterfall.end,
            // except the user (pCallValue) which before waterfall.end
            pCallInst->replaceAllUsesWith(pResult);
            cast<Instruction>(pCallValue)->setOperand(0, pCallInst);
        }
        else
        {
            // Add waterfall.end
            pResult = EmitCall(m_pModule, waterfallEnd, pResultTy, { pWaterfallBegin, pCallInst }, NoAttrib, pNextInst);

            // Replace all users of call inst with the result of waterfall.end except waterfall.end itself.
            pCallInst->replaceAllUsesWith(pResult);
            cast<CallInst>(pResult)->setOperand(1, pCallInst);
        }
    }
}

} // Llpc
