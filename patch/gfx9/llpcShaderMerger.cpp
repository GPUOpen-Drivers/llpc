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
 * @file  llpcShaderMerger.cpp
 * @brief LLPC source file: contains implementation of class Llpc::ShaderMerger.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"

#include "llpcCodeGenManager.h"
#include "llpcContext.h"
#if LLPC_BUILD_GFX10
#include "llpcNggPrimShader.h"
#endif
#include "llpcPassDeadFuncRemove.h"
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"
#include "llpcShaderMerger.h"

#define DEBUG_TYPE "llpc-shader-merger"

using namespace llvm;
using namespace Llpc;

// =====================================================================================================================
ShaderMerger::ShaderMerger(
    PipelineState*    pPipelineState,   // [in] Pipeline state
    PipelineShaders*  pPipelineShaders) // [in] API shaders in the pipeline
    :
    m_pPipelineState(pPipelineState),
    m_pContext(static_cast<Context*>(&pPipelineState->GetContext())),
    m_gfxIp(pPipelineState->GetBuilderContext()->GetGfxIpVersion()),
    m_pPipelineShaders(pPipelineShaders)
#if LLPC_BUILD_GFX10
    , m_primShader(pPipelineState)
#endif
{
    LLPC_ASSERT(m_gfxIp.major >= 9);
    LLPC_ASSERT(m_pContext->IsGraphics());

    const uint32_t stageMask = m_pContext->GetShaderStageMask();
    m_hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
    m_hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);
    m_hasTes = ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0);
    m_hasGs  = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Builds LLVM function for hardware primitive shader (NGG).
Function* ShaderMerger::BuildPrimShader(
    Function*  pEsEntryPoint,           // [in] Entry-point of hardware export shader (ES) (could be null)
    Function*  pGsEntryPoint,           // [in] Entry-point of hardware geometry shader (GS) (could be null)
    Function*  pCopyShaderEntryPoint)   // [in] Entry-point of hardware vertex shader (VS, copy shader) (could be null)
{
    NggPrimShader primShader(m_pPipelineState);
    return primShader.Generate(pEsEntryPoint, pGsEntryPoint, pCopyShaderEntryPoint);
}
#endif

// =====================================================================================================================
// Generates the type for the new entry-point of LS-HS merged shader.
FunctionType* ShaderMerger::GenerateLsHsEntryPointType(
    uint64_t* pInRegMask // [out] "Inreg" bit mask for the arguments
    ) const
{
    LLPC_ASSERT(m_hasVs || m_hasTcs);

    std::vector<Type*> argTys;

    // First 8 system values (SGPRs)
    for (uint32_t i = 0; i < LsHsSpecialSysValueCount; ++i)
    {
        argTys.push_back(m_pContext->Int32Ty());
        *pInRegMask |= (1ull << i);
    }

    // User data (SGPRs)
    uint32_t userDataCount = 0;
    if (m_hasVs)
    {
        const auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
        userDataCount = std::max(pIntfData->userDataCount, userDataCount);
    }

    if (m_hasTcs)
    {
        const auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageTessControl);
        userDataCount = std::max(pIntfData->userDataCount, userDataCount);
    }

    if (m_hasTcs && m_hasVs)
    {
        auto pVsIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
        auto pTcsIntfData = m_pContext->GetShaderInterfaceData(ShaderStageTessControl);

        if ((pVsIntfData->spillTable.sizeInDwords == 0) &&
            (pTcsIntfData->spillTable.sizeInDwords > 0))
        {
            pVsIntfData->userDataUsage.spillTable = userDataCount;
            ++userDataCount;
            LLPC_ASSERT(userDataCount <= m_pContext->GetGpuProperty()->maxUserDataCount);
        }
    }

    LLPC_ASSERT(userDataCount > 0);
    argTys.push_back(VectorType::get(m_pContext->Int32Ty(), userDataCount));
    *pInRegMask |= (1ull << LsHsSpecialSysValueCount);

    // Other system values (VGPRs)
    argTys.push_back(m_pContext->Int32Ty()); // Patch ID
    argTys.push_back(m_pContext->Int32Ty()); // Relative patch ID (control point ID included)
    argTys.push_back(m_pContext->Int32Ty()); // Vertex ID
    argTys.push_back(m_pContext->Int32Ty()); // Relative vertex ID (auto index)
    argTys.push_back(m_pContext->Int32Ty()); // Step rate
    argTys.push_back(m_pContext->Int32Ty()); // Instance ID

    return FunctionType::get(m_pContext->VoidTy(), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for LS-HS merged shader.
Function* ShaderMerger::GenerateLsHsEntryPoint(
    Function* pLsEntryPoint,  // [in] Entry-point of hardware local shader (LS) (could be null)
    Function* pHsEntryPoint)  // [in] Entry-point of hardware hull shader (HS)
{
    if (pLsEntryPoint != nullptr)
    {
        pLsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        pLsEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    LLPC_ASSERT(pHsEntryPoint != nullptr);
    pHsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    pHsEntryPoint->addFnAttr(Attribute::AlwaysInline);

    uint64_t inRegMask = 0;
    auto pEntryPointTy = GenerateLsHsEntryPointType(&inRegMask);

    // Create the entrypoint for the merged shader, and insert it just before the old HS.
    Function* pEntryPoint = Function::Create(pEntryPointTy,
                                             GlobalValue::ExternalLinkage,
                                             LlpcName::LsHsEntryPoint);
    auto pModule = pHsEntryPoint->getParent();
    pModule->getFunctionList().insert(pHsEntryPoint->getIterator(), pEntryPoint);

    pEntryPoint->addFnAttr("amdgpu-flat-work-group-size", "128,128"); // Force s_barrier to be present (ignore optimization)

    for (auto& arg : pEntryPoint->args())
    {
        auto argIdx = arg.getArgNo();
        if (inRegMask & (1ull << argIdx))
        {
            arg.addAttr(Attribute::InReg);
        }
    }

    // define dllexport amdgpu_hs @_amdgpu_hs_main(
    //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..5)
    // {
    // .entry
    //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
    //     call void @llvm.amdgcn.init.exec(i64 -1)
    //
    //     ; Get thread ID:
    //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
    //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
    //     ;   threadId = bitCount
    //     %threadId = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
    //     %threadId = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadId)
    //
    //     %lsVertCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
    //     %hsVertCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
    //
    //     %nullHs = icmp eq i32 %hsVertCount, 0
    //     %vgpr0 = select i1 %nullHs, i32 %vgpr0, i32 %vgpr2
    //     %vgpr1 = select i1 %nullHs, i32 %vgpr1, i32 %vgpr3
    //     %vgpr2 = select i1 %nullHs, i32 %vgpr2, i32 %vgpr4
    //     %vgpr3 = select i1 %nullHs, i32 %vgpr3, i32 %vgpr5
    //
    //     %lsEnable = icmp ult i32 %threadId, %lsVertCount
    //     br i1 %lsEnable, label %.beginls, label %.endls
    //
    // .beginls:
    //     call void @llpc.ls.main(%sgpr..., %userData..., %vgpr...)
    //     br label %.endls
    //
    // .endls:
    //     call void @llvm.amdgcn.s.barrier()
    //     %hsEnable = icmp ult i32 %threadId, %hsVertCount
    //     br i1 %hsEnable, label %.beginhs, label %.endhs
    //
    // .beginhs:
    //     call void @llpc.hs.main(%sgpr..., %userData..., %vgpr...)
    //     br label %.endhs
    //
    // .endhs:
    //     ret void
    // }

    std::vector<Value*> args;
    std::vector<Attribute::AttrKind> attribs;

    auto pArg = pEntryPoint->arg_begin();

    Value* pOffChipLdsBase  = (pArg + LsHsSysValueOffChipLdsBase);
    Value* pMergeWaveInfo   = (pArg + LsHsSysValueMergedWaveInfo);
    Value* pTfBufferBase    = (pArg + LsHsSysValueTfBufferBase);

    pArg += LsHsSpecialSysValueCount;

    Value* pUserData = pArg++;

    // Define basic blocks
    auto pEndHsBlock    = BasicBlock::Create(*m_pContext, ".endhs", pEntryPoint);
    auto pBeginHsBlock  = BasicBlock::Create(*m_pContext, ".beginhs", pEntryPoint, pEndHsBlock);
    auto pEndLsBlock    = BasicBlock::Create(*m_pContext, ".endls", pEntryPoint, pBeginHsBlock);
    auto pBeginLsBlock  = BasicBlock::Create(*m_pContext, ".beginls", pEntryPoint, pEndLsBlock);
    auto pEntryBlock    = BasicBlock::Create(*m_pContext, ".entry", pEntryPoint, pBeginLsBlock);

    // Construct ".entry" block
    args.clear();
    args.push_back(ConstantInt::get(m_pContext->Int64Ty(), -1));

    attribs.clear();
    attribs.push_back(Attribute::NoRecurse);

    EmitCall("llvm.amdgcn.init.exec", m_pContext->VoidTy(), args, attribs, pEntryBlock);

    args.clear();
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

    attribs.clear();
    attribs.push_back(Attribute::NoRecurse);

    auto pThreadId = EmitCall("llvm.amdgcn.mbcnt.lo", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

#if LLPC_BUILD_GFX10
    uint32_t waveSize = m_pContext->GetShaderWaveSize(ShaderStageTessControl);
    if (waveSize == 64)
#endif
    {
        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
        args.push_back(pThreadId);

        pThreadId = EmitCall("llvm.amdgcn.mbcnt.hi", m_pContext->Int32Ty(), args, attribs, pEntryBlock);
    }

    args.clear();
    args.push_back(pMergeWaveInfo);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

    attribs.clear();
    attribs.push_back(Attribute::ReadNone);

    auto pLsVertCount = EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

    Value* pPatchId     = pArg;
    Value* pRelPatchId  = (pArg + 1);
    Value* pVertexId    = (pArg + 2);
    Value* pRelVertexId = (pArg + 3);
    Value* pStepRate    = (pArg + 4);
    Value* pInstanceId  = (pArg + 5);

    args.clear();
    args.push_back(pMergeWaveInfo);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

    auto pHsVertCount = EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);
    // NOTE: For GFX9, hardware has an issue of initializing LS VGPRs. When HS is null, v0~v3 are initialized as LS
    // VGPRs rather than expected v2~v4.
    auto pGpuWorkarounds = m_pContext->GetGpuWorkarounds();
    if (pGpuWorkarounds->gfx9.fixLsVgprInput)
    {
        auto pNullHs = new ICmpInst(*pEntryBlock,
                                    ICmpInst::ICMP_EQ,
                                    pHsVertCount,
                                    ConstantInt::get(m_pContext->Int32Ty(), 0),
                                    "");

        pVertexId    = SelectInst::Create(pNullHs, pArg,       (pArg + 2), "", pEntryBlock);
        pRelVertexId = SelectInst::Create(pNullHs, (pArg + 1), (pArg + 3), "", pEntryBlock);
        pStepRate    = SelectInst::Create(pNullHs, (pArg + 2), (pArg + 4), "", pEntryBlock);
        pInstanceId  = SelectInst::Create(pNullHs, (pArg + 3), (pArg + 5), "", pEntryBlock);
    }

    auto pLsEnable = new ICmpInst(*pEntryBlock, ICmpInst::ICMP_ULT, pThreadId, pLsVertCount, "");
    BranchInst::Create(pBeginLsBlock, pEndLsBlock, pLsEnable, pEntryBlock);

    // Construct ".beginls" block
    if (m_hasVs)
    {
        // Call LS main function
        args.clear();

        auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
        const uint32_t userDataCount = pIntfData->userDataCount;

        uint32_t userDataIdx = 0;

        auto pLsArgBegin = pLsEntryPoint->arg_begin();
        const uint32_t lsArgCount = pLsEntryPoint->arg_size();

        uint32_t lsArgIdx = 0;

        // Set up user data SGPRs
        while (userDataIdx < userDataCount)
        {
            LLPC_ASSERT(lsArgIdx < lsArgCount);

            auto pLsArg = (pLsArgBegin + lsArgIdx);
            LLPC_ASSERT(pLsArg->hasAttribute(Attribute::InReg));

            auto pLsArgTy = pLsArg->getType();
            if (pLsArgTy->isVectorTy())
            {
                LLPC_ASSERT(pLsArgTy->getVectorElementType()->isIntegerTy());

                const uint32_t userDataSize = pLsArgTy->getVectorNumElements();

                std::vector<Constant*> shuffleMask;
                for (uint32_t i = 0; i < userDataSize; ++i)
                {
                    shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), userDataIdx + i));
                }

                userDataIdx += userDataSize;

                auto pLsUserData =
                    new ShuffleVectorInst(pUserData, pUserData, ConstantVector::get(shuffleMask), "", pBeginLsBlock);
                args.push_back(pLsUserData);
            }
            else
            {
                LLPC_ASSERT(pLsArgTy->isIntegerTy());

                auto pLsUserData = ExtractElementInst::Create(pUserData,
                                                              ConstantInt::get(m_pContext->Int32Ty(), userDataIdx),
                                                              "",
                                                              pBeginLsBlock);
                args.push_back(pLsUserData);
                ++userDataIdx;
            }

            ++lsArgIdx;
        }

        // Set up system value VGPRs (LS does not have system value SGPRs)
        if (lsArgIdx < lsArgCount)
        {
            args.push_back(pVertexId);
            ++lsArgIdx;
        }

        if (lsArgIdx < lsArgCount)
        {
            args.push_back(pRelVertexId);
            ++lsArgIdx;
        }

        if (lsArgIdx < lsArgCount)
        {
            args.push_back(pStepRate);
            ++lsArgIdx;
        }

        if (lsArgIdx < lsArgCount)
        {
            args.push_back(pInstanceId);
            ++lsArgIdx;
        }

        LLPC_ASSERT(lsArgIdx == lsArgCount); // Must have visit all arguments of LS entry point

        CallInst::Create(pLsEntryPoint, args, "", pBeginLsBlock);
    }
    BranchInst::Create(pEndLsBlock, pBeginLsBlock);

    // Construct ".endls" block
    args.clear();
    attribs.clear();
    attribs.push_back(Attribute::NoRecurse);
    EmitCall("llvm.amdgcn.s.barrier", m_pContext->VoidTy(), args, attribs, pEndLsBlock);

    auto pHsEnable = new ICmpInst(*pEndLsBlock, ICmpInst::ICMP_ULT, pThreadId, pHsVertCount, "");
    BranchInst::Create(pBeginHsBlock, pEndHsBlock, pHsEnable, pEndLsBlock);

    // Construct ".beginhs" block
    if (m_hasTcs)
    {
        // Call HS main function
        args.clear();

        auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageTessControl);
        const uint32_t userDataCount = pIntfData->userDataCount;

        uint32_t userDataIdx = 0;

        auto pHsArgBegin = pHsEntryPoint->arg_begin();

        uint32_t hsArgIdx = 0;

        // Set up user data SGPRs
        while (userDataIdx < userDataCount)
        {
            LLPC_ASSERT(hsArgIdx < pHsEntryPoint->arg_size());

            auto pHsArg = (pHsArgBegin + hsArgIdx);
            LLPC_ASSERT(pHsArg->hasAttribute(Attribute::InReg));

            auto pHsArgTy = pHsArg->getType();
            if (pHsArgTy->isVectorTy())
            {
                LLPC_ASSERT(pHsArgTy->getVectorElementType()->isIntegerTy());

                const uint32_t userDataSize = pHsArgTy->getVectorNumElements();

                std::vector<Constant*> shuffleMask;
                for (uint32_t i = 0; i < userDataSize; ++i)
                {
                    shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), userDataIdx + i));
                }

                userDataIdx += userDataSize;

                auto pHsUserData =
                    new ShuffleVectorInst(pUserData, pUserData, ConstantVector::get(shuffleMask), "", pBeginHsBlock);
                args.push_back(pHsUserData);
            }
            else
            {
                LLPC_ASSERT(pHsArgTy->isIntegerTy());
                uint32_t actualUserDataIdx = userDataIdx;
                if (pIntfData->spillTable.sizeInDwords > 0)
                {
                    if (pIntfData->userDataUsage.spillTable == userDataIdx)
                    {
                        if (m_hasVs)
                        {
                            auto pVsIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
                            LLPC_ASSERT(pVsIntfData->userDataUsage.spillTable > 0);
                            actualUserDataIdx = pVsIntfData->userDataUsage.spillTable;
                        }
                    }
                }
                auto pHsUserData = ExtractElementInst::Create(pUserData,
                                                              ConstantInt::get(m_pContext->Int32Ty(), actualUserDataIdx),
                                                              "",
                                                              pBeginHsBlock);
                args.push_back(pHsUserData);
                ++userDataIdx;
            }

            ++hsArgIdx;
        }

        // Set up system value SGPRs
        if (m_pPipelineState->IsTessOffChip())
        {
            args.push_back(pOffChipLdsBase);
            ++hsArgIdx;
        }

        args.push_back(pTfBufferBase);
        ++hsArgIdx;

        // Set up system value VGPRs
        args.push_back(pPatchId);
        ++hsArgIdx;

        args.push_back(pRelPatchId);
        ++hsArgIdx;

        LLPC_ASSERT(hsArgIdx == pHsEntryPoint->arg_size()); // Must have visit all arguments of HS entry point

        CallInst::Create(pHsEntryPoint, args, "", pBeginHsBlock);
    }
    BranchInst::Create(pEndHsBlock, pBeginHsBlock);

    // Construct ".endhs" block
    ReturnInst::Create(*m_pContext, pEndHsBlock);

    return pEntryPoint;
}

// =====================================================================================================================
// Generates the type for the new entry-point of ES-GS merged shader.
FunctionType* ShaderMerger::GenerateEsGsEntryPointType(
    uint64_t* pInRegMask // [out] "Inreg" bit mask for the arguments
    ) const
{
    LLPC_ASSERT(m_hasGs);

    std::vector<Type*> argTys;

    // First 8 system values (SGPRs)
    for (uint32_t i = 0; i < EsGsSpecialSysValueCount; ++i)
    {
        argTys.push_back(m_pContext->Int32Ty());
        *pInRegMask |= (1ull << i);
    }

    // User data (SGPRs)
    uint32_t userDataCount = 0;
    bool hasTs = (m_hasTcs || m_hasTes);
    if (hasTs)
    {
        if (m_hasTes)
        {
            const auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageTessEval);
            userDataCount = std::max(pIntfData->userDataCount, userDataCount);
        }
    }
    else
    {
        if (m_hasVs)
        {
            const auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
            userDataCount = std::max(pIntfData->userDataCount, userDataCount);
        }
    }

    const auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageGeometry);
    userDataCount = std::max(pIntfData->userDataCount, userDataCount);

    if (hasTs)
    {
        if (m_hasTes)
        {
            const auto pTesIntfData = m_pContext->GetShaderInterfaceData(ShaderStageTessEval);
            LLPC_ASSERT(pTesIntfData->userDataUsage.tes.viewIndex == pIntfData->userDataUsage.gs.viewIndex);
            if ((pIntfData->spillTable.sizeInDwords > 0) &&
                (pTesIntfData->spillTable.sizeInDwords == 0))
            {
                pTesIntfData->userDataUsage.spillTable = userDataCount;
                ++userDataCount;
                LLPC_ASSERT(userDataCount <= m_pContext->GetGpuProperty()->maxUserDataCount);
            }
        }
    }
    else
    {
        if (m_hasVs)
        {
            const auto pVsIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);
            LLPC_ASSERT(pVsIntfData->userDataUsage.vs.viewIndex == pIntfData->userDataUsage.gs.viewIndex);
            if ((pIntfData->spillTable.sizeInDwords > 0) &&
                (pVsIntfData->spillTable.sizeInDwords == 0))
            {
                pVsIntfData->userDataUsage.spillTable = userDataCount;
                ++userDataCount;
                LLPC_ASSERT(userDataCount <= m_pContext->GetGpuProperty()->maxUserDataCount);
            }
        }
    }

    LLPC_ASSERT(userDataCount > 0);
    argTys.push_back(VectorType::get(m_pContext->Int32Ty(), userDataCount));
    *pInRegMask |= (1ull << EsGsSpecialSysValueCount);

    // Other system values (VGPRs)
    argTys.push_back(m_pContext->Int32Ty());        // ES to GS offsets (vertex 0 and 1)
    argTys.push_back(m_pContext->Int32Ty());        // ES to GS offsets (vertex 2 and 3)
    argTys.push_back(m_pContext->Int32Ty());        // Primitive ID (GS)
    argTys.push_back(m_pContext->Int32Ty());        // Invocation ID
    argTys.push_back(m_pContext->Int32Ty());        // ES to GS offsets (vertex 4 and 5)

    if (hasTs)
    {
        argTys.push_back(m_pContext->FloatTy());    // X of TessCoord (U)
        argTys.push_back(m_pContext->FloatTy());    // Y of TessCoord (V)
        argTys.push_back(m_pContext->Int32Ty());    // Relative patch ID
        argTys.push_back(m_pContext->Int32Ty());    // Patch ID
    }
    else
    {
        argTys.push_back(m_pContext->Int32Ty());    // Vertex ID
        argTys.push_back(m_pContext->Int32Ty());    // Relative vertex ID (auto index)
        argTys.push_back(m_pContext->Int32Ty());    // Primitive ID (VS)
        argTys.push_back(m_pContext->Int32Ty());    // Instance ID
    }

    return FunctionType::get(m_pContext->VoidTy(), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for ES-GS merged shader.
Function* ShaderMerger::GenerateEsGsEntryPoint(
    Function* pEsEntryPoint,  // [in] Entry-point of hardware export shader (ES) (could be null)
    Function* pGsEntryPoint)  // [in] Entry-point of hardware geometry shader (GS)
{
    if (pEsEntryPoint != nullptr)
    {
        pEsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        pEsEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    LLPC_ASSERT(pGsEntryPoint != nullptr);
    pGsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    pGsEntryPoint->addFnAttr(Attribute::AlwaysInline);

    auto pModule = pGsEntryPoint->getParent();
    const bool hasTs = (m_hasTcs || m_hasTes);

    uint64_t inRegMask = 0;
    auto pEntryPointTy = GenerateEsGsEntryPointType(&inRegMask);

    // Create the entrypoint for the merged shader, and insert it just before the old GS.
    Function* pEntryPoint = Function::Create(pEntryPointTy,
                                             GlobalValue::ExternalLinkage,
                                             LlpcName::EsGsEntryPoint);
    pModule->getFunctionList().insert(pGsEntryPoint->getIterator(), pEntryPoint);

    pEntryPoint->addFnAttr("amdgpu-flat-work-group-size", "128,128"); // Force s_barrier to be present (ignore optimization)

    for (auto& arg : pEntryPoint->args())
    {
        auto argIdx = arg.getArgNo();
        if (inRegMask & (1ull << argIdx))
        {
            arg.addAttr(Attribute::InReg);
        }
    }

    // define dllexport amdgpu_gs @_amdgpu_gs_main(
    //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8)
    // {
    // .entry
    //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
    //     call void @llvm.amdgcn.init.exec(i64 -1)
    //
    //     ; Get thread ID:
    //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
    //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
    //     ;   threadId = bitCount
    //     %threadId = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
    //     %threadId = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadId)
    //
    //     %esVertCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
    //     %gsPrimCount = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
    //
    //     %esEnable = icmp ult i32 %threadId, %esVertCount
    //     br i1 %esEnable, label %.begines, label %.endes
    //
    // .begines:
    //     call void @llpc.es.main(%sgpr..., %userData..., %vgpr...)
    //     br label %.endes
    //
    // .endes:
    //     call void @llvm.amdgcn.s.barrier()
    //     %gsEnable = icmp ult i32 %threadId, %gsPrimCount
    //     br i1 %gsEnable, label %.begings, label %.endgs
    //
    // .begings:
    //     call void @llpc.gs.main(%sgpr..., %userData..., %vgpr...)
    //     br label %.endgs
    //
    // .endgs:
    //     ret void
    // }

    std::vector<Value*> args;
    std::vector<Attribute::AttrKind> attribs;

    const auto& calcFactor = m_pContext->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

    auto pArg = pEntryPoint->arg_begin();

    Value* pGsVsOffset      = (pArg + EsGsSysValueGsVsOffset);
    Value* pMergedWaveInfo  = (pArg + EsGsSysValueMergedWaveInfo);
    Value* pOffChipLdsBase  = (pArg + EsGsSysValueOffChipLdsBase);

    pArg += EsGsSpecialSysValueCount;

    Value* pUserData = pArg++;

    // Define basic blocks
    auto pEndGsBlock    = BasicBlock::Create(*m_pContext, ".endgs", pEntryPoint);
    auto pBeginGsBlock  = BasicBlock::Create(*m_pContext, ".begings", pEntryPoint, pEndGsBlock);
    auto pEndEsBlock    = BasicBlock::Create(*m_pContext, ".endes", pEntryPoint, pBeginGsBlock);
    auto pBeginEsBlock  = BasicBlock::Create(*m_pContext, ".begines", pEntryPoint, pEndEsBlock);
    auto pEntryBlock    = BasicBlock::Create(*m_pContext, ".entry", pEntryPoint, pBeginEsBlock);

    // Construct ".entry" block
    args.clear();
    args.push_back(ConstantInt::get(m_pContext->Int64Ty(), -1));

    attribs.clear();
    attribs.push_back(Attribute::NoRecurse);

    EmitCall("llvm.amdgcn.init.exec", m_pContext->VoidTy(), args, attribs, pEntryBlock);

    args.clear();
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

    attribs.clear();
    attribs.push_back(Attribute::NoRecurse);

    auto pThreadId = EmitCall("llvm.amdgcn.mbcnt.lo", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

#if LLPC_BUILD_GFX10
    uint32_t waveSize = m_pContext->GetShaderWaveSize(ShaderStageGeometry);
    if (waveSize == 64)
#endif
    {
        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
        args.push_back(pThreadId);

        pThreadId = EmitCall("llvm.amdgcn.mbcnt.hi", m_pContext->Int32Ty(), args, attribs, pEntryBlock);
    }

    args.clear();
    args.push_back(pMergedWaveInfo);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

    attribs.clear();
    attribs.push_back(Attribute::ReadNone);

    auto pEsVertCount = EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

    args.clear();
    args.push_back(pMergedWaveInfo);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

    auto pGsPrimCount = EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

    args.clear();
    args.push_back(pMergedWaveInfo);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

    auto pGsWaveId = EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

    args.clear();
    args.push_back(pMergedWaveInfo);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 24));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 4));

    auto pWaveInSubgroup =
        EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

    auto pEsGsOffset =
        BinaryOperator::CreateMul(pWaveInSubgroup,
                                  ConstantInt::get(m_pContext->Int32Ty(), 64 * 4 * calcFactor.esGsRingItemSize),
                                  "",
                                  pEntryBlock);

    auto pEsEnable = new ICmpInst(*pEntryBlock, ICmpInst::ICMP_ULT, pThreadId, pEsVertCount, "");
    BranchInst::Create(pBeginEsBlock, pEndEsBlock, pEsEnable, pEntryBlock);

    Value* pEsGsOffsets01 = pArg;

    Value* pEsGsOffsets23 = UndefValue::get(m_pContext->Int32Ty());
    if (calcFactor.inputVertices > 2)
    {
        // NOTE: ES to GS offset (vertex 2 and 3) is valid once the primitive type has more than 2 vertices.
        pEsGsOffsets23 = (pArg + 1);
    }

    Value* pGsPrimitiveId = (pArg + 2);
    Value* pInvocationId  = (pArg + 3);

    Value* pEsGsOffsets45 = UndefValue::get(m_pContext->Int32Ty());
    if (calcFactor.inputVertices > 4)
    {
        // NOTE: ES to GS offset (vertex 4 and 5) is valid once the primitive type has more than 4 vertices.
        pEsGsOffsets45 = (pArg + 4);
    }

    Value* pTessCoordX    = (pArg + 5);
    Value* pTessCoordY    = (pArg + 6);
    Value* pRelPatchId    = (pArg + 7);
    Value* pPatchId       = (pArg + 8);

    Value* pVertexId      = (pArg + 5);
    Value* pRelVertexId   = (pArg + 6);
    Value* pVsPrimitiveId = (pArg + 7);
    Value* pInstanceId    = (pArg + 8);

    // Construct ".begines" block
    uint32_t spillTableIdx = 0;
    if ((hasTs && m_hasTes) || ((hasTs == false) && m_hasVs))
    {
        // Call ES main function
        args.clear();

        auto pIntfData = m_pContext->GetShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
        const uint32_t userDataCount = pIntfData->userDataCount;
        spillTableIdx = pIntfData->userDataUsage.spillTable;

        uint32_t userDataIdx = 0;

        auto pEsArgBegin = pEsEntryPoint->arg_begin();
        const uint32_t esArgCount = pEsEntryPoint->arg_size();

        uint32_t esArgIdx = 0;

        // Set up user data SGPRs
        while (userDataIdx < userDataCount)
        {
            LLPC_ASSERT(esArgIdx < esArgCount);

            auto pEsArg = (pEsArgBegin + esArgIdx);
            LLPC_ASSERT(pEsArg->hasAttribute(Attribute::InReg));

            auto pEsArgTy = pEsArg->getType();
            if (pEsArgTy->isVectorTy())
            {
                LLPC_ASSERT(pEsArgTy->getVectorElementType()->isIntegerTy());

                const uint32_t userDataSize = pEsArgTy->getVectorNumElements();

                std::vector<Constant*> shuffleMask;
                for (uint32_t i = 0; i < userDataSize; ++i)
                {
                    shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), userDataIdx + i));
                }

                userDataIdx += userDataSize;

                auto pEsUserData =
                    new ShuffleVectorInst(pUserData, pUserData, ConstantVector::get(shuffleMask), "", pBeginEsBlock);
                args.push_back(pEsUserData);
            }
            else
            {
                LLPC_ASSERT(pEsArgTy->isIntegerTy());

                auto pEsUserData = ExtractElementInst::Create(pUserData,
                                                              ConstantInt::get(m_pContext->Int32Ty(), userDataIdx),
                                                              "",
                                                              pBeginEsBlock);
                args.push_back(pEsUserData);
                ++userDataIdx;
            }

            ++esArgIdx;
        }

        if (hasTs)
        {
            // Set up system value SGPRs
            if (m_pPipelineState->IsTessOffChip())
            {
                args.push_back(pOffChipLdsBase);
                ++esArgIdx;

                args.push_back(pOffChipLdsBase);
                ++esArgIdx;
            }

            args.push_back(pEsGsOffset);
            ++esArgIdx;

            // Set up system value VGPRs
            args.push_back(pTessCoordX);
            ++esArgIdx;

            args.push_back(pTessCoordY);
            ++esArgIdx;

            args.push_back(pRelPatchId);
            ++esArgIdx;

            args.push_back(pPatchId);
            ++esArgIdx;
        }
        else
        {
            // Set up system value SGPRs
            args.push_back(pEsGsOffset);
            ++esArgIdx;

            // Set up system value VGPRs
            if (esArgIdx < esArgCount)
            {
                args.push_back(pVertexId);
                ++esArgIdx;
            }

            if (esArgIdx < esArgCount)
            {
                args.push_back(pRelVertexId);
                ++esArgIdx;
            }

            if (esArgIdx < esArgCount)
            {
                args.push_back(pVsPrimitiveId);
                ++esArgIdx;
            }

            if (esArgIdx < esArgCount)
            {
                args.push_back(pInstanceId);
                ++esArgIdx;
            }
        }

        LLPC_ASSERT(esArgIdx == esArgCount); // Must have visit all arguments of ES entry point

        CallInst::Create(pEsEntryPoint, args, "", pBeginEsBlock);
    }
    BranchInst::Create(pEndEsBlock, pBeginEsBlock);

    // Construct ".endes" block
    args.clear();
    attribs.clear();
    attribs.push_back(Attribute::NoRecurse);
    EmitCall("llvm.amdgcn.s.barrier", m_pContext->VoidTy(), args, attribs, pEndEsBlock);

    auto pGsEnable = new ICmpInst(*pEndEsBlock, ICmpInst::ICMP_ULT, pThreadId, pGsPrimCount, "");
    BranchInst::Create(pBeginGsBlock, pEndGsBlock, pGsEnable, pEndEsBlock);

    // Construct ".begings" block
    {
        attribs.clear();
        attribs.push_back(Attribute::ReadNone);

        args.clear();
        args.push_back(pEsGsOffsets01);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset0 =
            EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pBeginGsBlock);

        args.clear();
        args.push_back(pEsGsOffsets01);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset1 =
            EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pBeginGsBlock);

        args.clear();
        args.push_back(pEsGsOffsets23);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset2 =
            EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pBeginGsBlock);

        args.clear();
        args.push_back(pEsGsOffsets23);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset3 =
            EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pBeginGsBlock);

        args.clear();
        args.push_back(pEsGsOffsets45);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset4 =
            EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pBeginGsBlock);

        args.clear();
        args.push_back(pEsGsOffsets45);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset5 =
            EmitCall("llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pBeginGsBlock);

        // Call GS main function
        args.clear();

        auto pIntfData = m_pContext->GetShaderInterfaceData(ShaderStageGeometry);
        const uint32_t userDataCount = pIntfData->userDataCount;

        uint32_t userDataIdx = 0;

        auto pGsArgBegin = pGsEntryPoint->arg_begin();

        uint32_t gsArgIdx = 0;

        // Set up user data SGPRs
        while (userDataIdx < userDataCount)
        {
            LLPC_ASSERT(gsArgIdx < pGsEntryPoint->arg_size());

            auto pGsArg = (pGsArgBegin + gsArgIdx);
            LLPC_ASSERT(pGsArg->hasAttribute(Attribute::InReg));

            auto pGsArgTy = pGsArg->getType();
            if (pGsArgTy->isVectorTy())
            {
                LLPC_ASSERT(pGsArgTy->getVectorElementType()->isIntegerTy());

                const uint32_t userDataSize = pGsArgTy->getVectorNumElements();

                std::vector<Constant*> shuffleMask;
                for (uint32_t i = 0; i < userDataSize; ++i)
                {
                    shuffleMask.push_back(ConstantInt::get(m_pContext->Int32Ty(), userDataIdx + i));
                }

                userDataIdx += userDataSize;

                auto pGsUserData =
                    new ShuffleVectorInst(pUserData, pUserData, ConstantVector::get(shuffleMask), "", pBeginGsBlock);
                args.push_back(pGsUserData);
            }
            else
            {
                LLPC_ASSERT(pGsArgTy->isIntegerTy());
                uint32_t actualUserDataIdx = userDataIdx;
                if (pIntfData->spillTable.sizeInDwords > 0)
                {
                    if (pIntfData->userDataUsage.spillTable == userDataIdx)
                    {
                        if (spillTableIdx > 0)
                        {
                            actualUserDataIdx = spillTableIdx;
                        }
                    }
                }
                auto pGsUserData = ExtractElementInst::Create(pUserData,
                                                              ConstantInt::get(m_pContext->Int32Ty(), actualUserDataIdx),
                                                              "",
                                                              pBeginGsBlock);
                args.push_back(pGsUserData);
                ++userDataIdx;
            }

            ++gsArgIdx;
        }

        // Set up system value SGPRs
        args.push_back(pGsVsOffset);
        ++gsArgIdx;

        args.push_back(pGsWaveId);
        ++gsArgIdx;

        // Set up system value VGPRs
        args.push_back(pEsGsOffset0);
        ++gsArgIdx;

        args.push_back(pEsGsOffset1);
        ++gsArgIdx;

        args.push_back(pGsPrimitiveId);
        ++gsArgIdx;

        args.push_back(pEsGsOffset2);
        ++gsArgIdx;

        args.push_back(pEsGsOffset3);
        ++gsArgIdx;

        args.push_back(pEsGsOffset4);
        ++gsArgIdx;

        args.push_back(pEsGsOffset5);
        ++gsArgIdx;

        args.push_back(pInvocationId);
        ++gsArgIdx;

        LLPC_ASSERT(gsArgIdx == pGsEntryPoint->arg_size()); // Must have visit all arguments of GS entry point

        CallInst::Create(pGsEntryPoint, args, "", pBeginGsBlock);
    }
    BranchInst::Create(pEndGsBlock, pBeginGsBlock);

    // Construct ".endgs" block
    ReturnInst::Create(*m_pContext, pEndGsBlock);

    return pEntryPoint;
}
