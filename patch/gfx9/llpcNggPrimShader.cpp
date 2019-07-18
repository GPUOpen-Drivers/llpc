/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcNggPrimShader.cpp
 * @brief LLPC source file: contains implementation of class Llpc::NggPrimShader.
 ***********************************************************************************************************************
 */
#include "llvm/Linker/Linker.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llpcContext.h"
#include "llpcGfx9Chip.h"
#include "llpcNggLdsManager.h"
#include "llpcNggPrimShader.h"
#include "llpcPassDeadFuncRemove.h"
#include "llpcPassExternalLibLink.h"
#include "llpcPassManager.h"
#include "llpcShaderMerger.h"

#define DEBUG_TYPE "llpc-ngg-prim-shader"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
NggPrimShader::NggPrimShader(
    Context* pContext)  // [in] LLPC context
    :
    m_pContext(pContext),
    m_gfxIp(m_pContext->GetGfxIpVersion()),
    m_pNggControl(m_pContext->GetNggControl()),
    m_pLdsManager(nullptr)
{
    LLPC_ASSERT(m_pContext->IsGraphics());

    memset(&m_nggFactor, 0, sizeof(m_nggFactor));

    const uint32_t stageMask = m_pContext->GetShaderStageMask();
    m_hasVs  = ((stageMask & ShaderStageToMask(ShaderStageVertex)) != 0);
    m_hasTcs = ((stageMask & ShaderStageToMask(ShaderStageTessControl)) != 0);
    m_hasTes = ((stageMask & ShaderStageToMask(ShaderStageTessEval)) != 0);
    m_hasGs  = ((stageMask & ShaderStageToMask(ShaderStageGeometry)) != 0);
}

// =====================================================================================================================
NggPrimShader::~NggPrimShader()
{
    if (m_pLdsManager != nullptr)
    {
        delete m_pLdsManager;
    }
}

// =====================================================================================================================
// Generates NGG primitive shader entry-point.
Function* NggPrimShader::Generate(
    Function*  pEsEntryPoint,     // [in] Entry-point of hardware export shader (ES)
    Function*  pGsEntryPoint)     // [in] Entry-point of hardware geometry shader (GS) (could be null)
{
    LLPC_ASSERT(m_gfxIp.major >= 10);

    LLPC_ASSERT(pEsEntryPoint != nullptr);
    auto pModule = pEsEntryPoint->getParent();

    pEsEntryPoint->setName(LlpcName::NggEsEntryPoint);
    pEsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
    pEsEntryPoint->addFnAttr(Attribute::AlwaysInline);

    if (pGsEntryPoint != nullptr)
    {
        pGsEntryPoint->setName(LlpcName::NggGsEntryPoint);
        pGsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        pGsEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    // Create NGG LDS manager
    LLPC_ASSERT(m_pLdsManager == nullptr);
    m_pLdsManager = new NggLdsManager(pModule, m_pContext);

    return GeneratePrimShaderEntryPoint(pModule);
}

// =====================================================================================================================
// Generates the type for the new entry-point of NGG primitive shader.
FunctionType* NggPrimShader::GeneratePrimShaderEntryPointType(
    uint64_t* pInRegMask // [out] "Inreg" bit mask for the arguments
    ) const
{
    std::vector<Type*> argTys;

    // First 8 system values (SGPRs)
    for (uint32_t i = 0; i < EsGsSpecialSysValueCount; ++i)
    {
        argTys.push_back(m_pContext->Int32Ty());
        *pInRegMask |= (1ull << i);
    }

    // User data (SGPRs)
    uint32_t userDataCount = 0;

    const auto pGsIntfData = m_pContext->GetShaderInterfaceData(ShaderStageGeometry);
    const auto pTesIntfData = m_pContext->GetShaderInterfaceData(ShaderStageTessEval);
    const auto pVsIntfData = m_pContext->GetShaderInterfaceData(ShaderStageVertex);

    bool hasTs = (m_hasTcs || m_hasTes);
    if (m_hasGs)
    {
        // GS is present in primitive shader (ES-GS merged shader)
        userDataCount = pGsIntfData->userDataCount;

        if (hasTs)
        {
            if (m_hasTes)
            {
                userDataCount = std::max(pTesIntfData->userDataCount, userDataCount);

                LLPC_ASSERT(pTesIntfData->userDataUsage.tes.viewIndex == pGsIntfData->userDataUsage.gs.viewIndex);
                if ((pGsIntfData->spillTable.sizeInDwords > 0) &&
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
                userDataCount = std::max(pVsIntfData->userDataCount, userDataCount);

                LLPC_ASSERT(pVsIntfData->userDataUsage.vs.viewIndex == pGsIntfData->userDataUsage.gs.viewIndex);
                if ((pGsIntfData->spillTable.sizeInDwords > 0) &&
                    (pVsIntfData->spillTable.sizeInDwords == 0))
                {
                    pVsIntfData->userDataUsage.spillTable = userDataCount;
                    ++userDataCount;
                }
            }
        }
    }
    else
    {
        // No GS in primitive shader (ES only)
        if (hasTs)
        {
            if (m_hasTes)
            {
                userDataCount = pTesIntfData->userDataCount;
            }
        }
        else
        {
            if (m_hasVs)
            {
                userDataCount = pVsIntfData->userDataCount;
            }
        }
    }

    if (userDataCount > 0)
    {
        argTys.push_back(VectorType::get(m_pContext->Int32Ty(), userDataCount));
        *pInRegMask |= (1ull << EsGsSpecialSysValueCount);
    }

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
// Generates the new entry-point for NGG primitive shader.
Function* NggPrimShader::GeneratePrimShaderEntryPoint(
    Module* pModule)  // [in] LLVM module
{
    const bool hasTs = (m_hasTcs || m_hasTes);

    const uint32_t waveSize = m_pContext->GetShaderWaveSize(ShaderStageGeometry);
    LLPC_ASSERT((waveSize == 32) || (waveSize == 64));

    const uint32_t waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

    uint64_t inRegMask = 0;
    auto pEntryPointTy = GeneratePrimShaderEntryPointType(&inRegMask);

    Function* pEntryPoint = Function::Create(pEntryPointTy,
                                             GlobalValue::ExternalLinkage,
                                             LlpcName::NggPrimShaderEntryPoint);

    pModule->getFunctionList().push_front(pEntryPoint);

    pEntryPoint->addFnAttr("amdgpu-flat-work-group-size", "128,128"); // Force s_barrier to be present (ignore optimization)

    for (auto& arg : pEntryPoint->args())
    {
        auto argIdx = arg.getArgNo();
        if (inRegMask & (1ull << argIdx))
        {
            arg.addAttr(Attribute::InReg);
        }
    }

    std::vector<Value*> args;
    std::vector<Attribute::AttrKind> attribs;

    auto pArg = pEntryPoint->arg_begin();

    Value* pMergedGroupInfo         = (pArg + EsGsSysValueMergedGroupInfo);
    Value* pMergedWaveInfo          = (pArg + EsGsSysValueMergedWaveInfo);
    Value* pOffChipLdsBase          = (pArg + EsGsSysValueOffChipLdsBase);
    Value* pPrimShaderTableAddrLow  = (pArg + EsGsSysValuePrimShaderTableAddrLow);
    Value* pPrimShaderTableAddrHigh = (pArg + EsGsSysValuePrimShaderTableAddrHigh);

    pArg += EsGsSpecialSysValueCount;

    Value* pUserData = pArg++;

    Value* pEsGsOffsets01 = pArg;
    Value* pEsGsOffsets23 = (pArg + 1);
    Value* pGsPrimitiveId = (pArg + 2);
    Value* pInvocationId  = (pArg + 3);
    Value* pEsGsOffsets45 = (pArg + 4);

    Value* pTessCoordX    = (pArg + 5);
    Value* pTessCoordY    = (pArg + 6);
    Value* pRelPatchId    = (pArg + 7);
    Value* pPatchId       = (pArg + 8);

    Value* pVertexId      = (pArg + 5);
    Value* pRelVertexId   = (pArg + 6);
    Value* pVsPrimitiveId = (pArg + 7);
    Value* pInstanceId    = (pArg + 8);

    if (m_hasGs)
    {
        // GS is present in primitive shader (ES-GS merged shader)
        const auto& calcFactor = m_pContext->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;

        // TODO: Remove unused variables once GS support in NGG is completed.
        LLPC_UNUSED(pGsPrimitiveId);
        LLPC_UNUSED(pInvocationId);
        LLPC_UNUSED(pPatchId);
        LLPC_UNUSED(pRelVertexId);
        LLPC_UNUSED(calcFactor);
        LLPC_UNUSED(pVertexId);
        LLPC_UNUSED(pUserData);
        LLPC_UNUSED(pOffChipLdsBase);
        LLPC_UNUSED(pVsPrimitiveId);
        LLPC_UNUSED(pTessCoordX);
        LLPC_UNUSED(pRelPatchId);
        LLPC_UNUSED(pTessCoordY);
        LLPC_UNUSED(pInstanceId);

        LLPC_NOT_IMPLEMENTED();
    }
    else
    {
        const auto pResUsage = m_pContext->GetShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

        // NOTE: If primitive ID is used in VS, we have to insert several basic blocks to distribute the value across
        // LDS because the primitive ID is provided as per-primitive instead of per-vertex. The algorithm is something
        // like this:
        //
        //   if (threadIdInWave < primCountInWave)
        //   {
        //      ldsOffset = vindex0 * 4
        //      ds_write ldsOffset, primId
        //   }
        //
        //   s_barrier
        //
        //   if (threadIdInWave < vertCountInWave)
        //   {
        //      ldsOffset = threadIdInSubgroup * 4
        //      ds_read primId, ldsOffset
        //   }
        //
        //   s_barrier
        //
        const bool distributePrimId = hasTs ? false : pResUsage->builtInUsage.vs.primitiveId;

        // No GS in primitive shader (ES only)
        if (m_pNggControl->passthroughMode)
        {
            // Pass-through mode

            // define dllexport amdgpu_gs @_amdgpu_gs_main(
            //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8])
            // {
            // .entry:
            //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
            //     call void @llvm.amdgcn.init.exec(i64 -1)
            //
            //     ; Get thread ID in a wave:
            //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
            //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
            //     ;   threadIdInWave = bitCount
            //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
            //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadIdInWave)
            //
            //     %waveIdInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 24, i32 4)
            //     %threadIdInSubgroup = mul i32 %waveIdInSubgroup, %waveSize
            //     %threadIdInSubgroup = add i32 %threadIdInSubgroup, %threadIdInWave
            //
            //     %primCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 22, i32 9)
            //     %vertCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 12, i32 9)
            //
            //     %primCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
            //     %vertCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
            //
            //     %primValid = icmp ult i32 %threadIdInWave , %primCountInWave
            //     br i1 %primValid, label %.writePrimId, label %.endWritePrimId
            // [
            // .writePrimId:
            //     ; Write LDS region (primitive ID)
            //     br label %.endWritePrimId
            //
            // .endWritePrimId:
            //     call void @llvm.amdgcn.s.barrier()
            //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
            //     br i1 %vertValid, label %.readPrimId, label %.endReadPrimId
            //
            // .readPrimId:
            //     ; Read LDS region (primitive ID)
            //     br label %.endReadPrimId
            //
            // .endReadPrimId:
            // ]
            //     call void @llvm.amdgcn.s.barrier()
            //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
            //     br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
            //
            // .allocReq:
            //     ; Do parameter cache (PC) alloc request: s_sendmsg(GS_ALLOC_REQ, ...)
            //     br label %.endAllocReq
            //
            // .endAllocReq:
            //     %primExp = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
            //     br i1 %primExp, label %.expPrim, label %.endExpPrim
            //
            // .expPrim:
            //     ; Do primitive export: exp prim, ...
            //     br label %.endExpPrim
            //
            // .endExpPrim:
            //     %vertExp = icmp ult i32 %threadIdInSubgroup, %vertCountInSubgroup
            //     br i1 %vertExp, label %.expVert, label %.endExpVert
            //
            // .expVert:
            //     call void @llpc.ngg.ES.main(%sgpr..., %userData..., %vgpr...)
            //     br label %.endExpVert
            //
            // .endExpVert:
            //     ret void
            // }

            // Define basic blocks
            auto pEndExpVertBlock  = BasicBlock::Create(*m_pContext, ".endExpVert", pEntryPoint);
            auto pExpVertBlock     = BasicBlock::Create(*m_pContext, ".expVert", pEntryPoint, pEndExpVertBlock);
            auto pEndExpPrimBlock  = BasicBlock::Create(*m_pContext, ".endExpPrim", pEntryPoint, pExpVertBlock);
            auto pExpPrimBlock     = BasicBlock::Create(*m_pContext, ".expPrim", pEntryPoint, pEndExpPrimBlock);
            auto pEndAllocReqBlock = BasicBlock::Create(*m_pContext, ".endAllocReq", pEntryPoint, pExpPrimBlock);
            auto pAllocReqBlock    = BasicBlock::Create(*m_pContext, ".allocReq", pEntryPoint, pEndAllocReqBlock);
            auto pEntryBlock       = BasicBlock::Create(*m_pContext, ".entry", pEntryPoint, pAllocReqBlock);

            // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
            BasicBlock* pWritePrimIdBlock       = nullptr;
            BasicBlock* pEndWritePrimIdBlock    = nullptr;
            BasicBlock* pReadPrimIdBlock        = nullptr;
            BasicBlock* pEndReadPrimIdBlock     = nullptr;

            if (distributePrimId)
            {
                pEndReadPrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".endReadPrimId", pEntryPoint, pAllocReqBlock);
                pReadPrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".readPrimId", pEntryPoint, pEndReadPrimIdBlock);
                pEndWritePrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".endWritePrimId", pEntryPoint, pReadPrimIdBlock);
                pWritePrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".writePrimId", pEntryPoint, pEndWritePrimIdBlock);
            }

            // Construct ".entry" block
            {
                args.clear();
                args.push_back(ConstantInt::get(m_pContext->Int64Ty(), -1));

                attribs.clear();
                attribs.push_back(Attribute::NoRecurse);

                EmitCall(pModule, "llvm.amdgcn.init.exec", m_pContext->VoidTy(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

                attribs.clear();
                attribs.push_back(Attribute::NoRecurse);

                auto pThreadIdInWave =
                    EmitCall(pModule, "llvm.amdgcn.mbcnt.lo", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                if (waveSize == 64)
                {
                    args.clear();
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
                    args.push_back(pThreadIdInWave);

                    pThreadIdInWave = EmitCall(pModule,
                                               "llvm.amdgcn.mbcnt.hi",
                                               m_pContext->Int32Ty(),
                                               args,
                                               attribs,
                                               pEntryBlock);
                }

                attribs.clear();
                attribs.push_back(Attribute::ReadNone);

                args.clear();
                args.push_back(pMergedGroupInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 22));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 9));

                auto pPrimCountInSubgroup =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedGroupInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 12));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 9));

                auto pVertCountInSubgroup =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedWaveInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

                auto pVertCountInWave =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedWaveInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

                auto pPrimCountInWave =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedWaveInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 24));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 4));

                auto pWaveIdInSubgroup =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                auto pThreadIdInSubgroup =
                    BinaryOperator::CreateMul(pWaveIdInSubgroup,
                                              ConstantInt::get(m_pContext->Int32Ty(), waveSize),
                                              "",
                                              pEntryBlock);
                pThreadIdInSubgroup = BinaryOperator::CreateAdd(pThreadIdInSubgroup, pThreadIdInWave, "", pEntryBlock);

                // Record NGG factors for future calculation
                m_nggFactor.pPrimCountInSubgroup    = pPrimCountInSubgroup;
                m_nggFactor.pVertCountInSubgroup    = pVertCountInSubgroup;
                m_nggFactor.pPrimCountInWave        = pPrimCountInWave;
                m_nggFactor.pVertCountInWave        = pVertCountInWave;
                m_nggFactor.pThreadIdInWave         = pThreadIdInWave;
                m_nggFactor.pThreadIdInSubgroup     = pThreadIdInSubgroup;
                m_nggFactor.pWaveIdInSubgroup       = pWaveIdInSubgroup;

                m_nggFactor.pEsGsOffsets01          = pEsGsOffsets01;

                if (distributePrimId)
                {
                    auto pPrimValid = new ICmpInst(*pEntryBlock,
                                                   ICmpInst::ICMP_ULT,
                                                   pThreadIdInWave,
                                                   pPrimCountInWave,
                                                   "");
                    BranchInst::Create(pWritePrimIdBlock, pEndWritePrimIdBlock, pPrimValid, pEntryBlock);
                }
                else
                {
                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEntryBlock);

                    auto pFirstWaveInSubgroup = new ICmpInst(*pEntryBlock,
                                                             ICmpInst::ICMP_EQ,
                                                             pWaveIdInSubgroup,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                             "");
                    BranchInst::Create(pAllocReqBlock, pEndAllocReqBlock, pFirstWaveInSubgroup, pEntryBlock);
                }
            }

            if (distributePrimId)
            {
                // Construct ".writePrimId" block
                {
                    // Primitive data layout
                    //   ES_GS_OFFSET01[31]    = null primitive flag
                    //   ES_GS_OFFSET01[28:20] = vertexId2 (in bytes)
                    //   ES_GS_OFFSET01[18:10] = vertexId1 (in bytes)
                    //   ES_GS_OFFSET01[8:0]   = vertexId0 (in bytes)
                    attribs.clear();
                    attribs.push_back(Attribute::ReadNone);

                    args.clear();
                    args.push_back(m_nggFactor.pEsGsOffsets01);
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 9));

                    // Distribute primitive ID
                    auto pVertexId0 = EmitCall(pModule,
                                               "llvm.amdgcn.ubfe.i32",
                                               m_pContext->Int32Ty(),
                                               args,
                                               attribs,
                                               pWritePrimIdBlock);

                    args.clear();
                    args.push_back(m_nggFactor.pEsGsOffsets01);
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 10));
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 9));

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);
                    auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                    auto pLdsOffset = BinaryOperator::CreateShl(pVertexId0,
                                                                ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                                "",
                                                                pWritePrimIdBlock);
                    pLdsOffset = BinaryOperator::CreateAdd(pRegionStart, pLdsOffset, "", pWritePrimIdBlock);

                    auto pPrimIdWriteValue = pGsPrimitiveId;
                    m_pLdsManager->WriteValueToLds(pPrimIdWriteValue, pLdsOffset, pWritePrimIdBlock);

                    BranchInst::Create(pEndWritePrimIdBlock, pWritePrimIdBlock);
                }

                // Construct ".endWritePrimId" block
                {
                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEndWritePrimIdBlock);

                    auto pVertValid = new ICmpInst(*pEndWritePrimIdBlock,
                                                   ICmpInst::ICMP_ULT,
                                                   m_nggFactor.pThreadIdInWave,
                                                   m_nggFactor.pVertCountInWave,
                                                   "");
                    BranchInst::Create(pReadPrimIdBlock, pEndReadPrimIdBlock, pVertValid, pEndWritePrimIdBlock);
                }

                // Construct ".readPrimId" block
                Value* pPrimIdReadValue = nullptr;
                {
                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                    auto pLdsOffset = BinaryOperator::CreateShl(m_nggFactor.pThreadIdInSubgroup,
                                                                ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                                "",
                                                                pReadPrimIdBlock);
                    pLdsOffset = BinaryOperator::CreateAdd(ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                                           pLdsOffset,
                                                           "",
                                                           pReadPrimIdBlock);
                    pPrimIdReadValue =
                        m_pLdsManager->ReadValueFromLds(m_pContext->Int32Ty(), pLdsOffset, pReadPrimIdBlock);

                    BranchInst::Create(pEndReadPrimIdBlock, pReadPrimIdBlock);
                }

                // Construct ".endReadPrimId" block
                {
                    auto pPrimitiveId = PHINode::Create(m_pContext->Int32Ty(), 2, "", pEndReadPrimIdBlock);

                    pPrimitiveId->addIncoming(pPrimIdReadValue, pReadPrimIdBlock);
                    pPrimitiveId->addIncoming(ConstantInt::get(m_pContext->Int32Ty(), 0), pEndWritePrimIdBlock);

                    // Record primitive ID
                    m_nggFactor.pPrimitiveId = pPrimitiveId;

                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEndReadPrimIdBlock);

                    auto pFirstWaveInSubgroup = new ICmpInst(*pEndReadPrimIdBlock,
                                                             ICmpInst::ICMP_EQ,
                                                             m_nggFactor.pWaveIdInSubgroup,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                             "");
                    BranchInst::Create(pAllocReqBlock, pEndAllocReqBlock, pFirstWaveInSubgroup, pEndReadPrimIdBlock);
                }
            }

            // Construct ".allocReq" block
            {
                DoParamCacheAllocRequest(pModule, pAllocReqBlock);
                BranchInst::Create(pEndAllocReqBlock, pAllocReqBlock);
            }

            // Construct ".endAllocReq" block
            {
                auto pPrimExp = new ICmpInst(*pEndAllocReqBlock,
                                             ICmpInst::ICMP_ULT,
                                             m_nggFactor.pThreadIdInSubgroup,
                                             m_nggFactor.pPrimCountInSubgroup,
                                             "");
                BranchInst::Create(pExpPrimBlock, pEndExpPrimBlock, pPrimExp, pEndAllocReqBlock);
            }

            // Construct ".expPrim" block
            {
                DoPrimitiveExport(pModule, nullptr, pExpPrimBlock);
                BranchInst::Create(pEndExpPrimBlock, pExpPrimBlock);
            }

            // Construct ".endExpPrim" block
            {
                auto pVertExp = new ICmpInst(*pEndExpPrimBlock,
                                             ICmpInst::ICMP_ULT,
                                             m_nggFactor.pThreadIdInSubgroup,
                                             m_nggFactor.pVertCountInSubgroup,
                                             "");
                BranchInst::Create(pExpVertBlock, pEndExpVertBlock, pVertExp, pEndExpPrimBlock);
            }

            // Construct ".expVert" block
            {
                RunEsOrEsVariant(pModule,
                                 LlpcName::NggEsEntryPoint,
                                 pEntryPoint->arg_begin(),
                                 false,
                                 nullptr,
                                 pExpVertBlock);

                BranchInst::Create(pEndExpVertBlock, pExpVertBlock);
            }

            // Construct ".endExpVert" block
            {
                ReturnInst::Create(*m_pContext, pEndExpVertBlock);
            }
        }
        else
        {
            // Non pass-through mode

            // define dllexport amdgpu_gs @_amdgpu_gs_main(
            //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8])
            // {
            // .entry:
            //     ; Initialize EXEC mask: exec = 0xFFFFFFFF'FFFFFFFF
            //     call void @llvm.amdgcn.init.exec(i64 -1)
            //
            //     ; Get thread ID in a wave:
            //     ;   bitCount  = ((1 << threadPosition) - 1) & 0xFFFFFFFF
            //     ;   bitCount += (((1 << threadPosition) - 1) >> 32) & 0xFFFFFFFF
            //     ;   threadIdInWave = bitCount
            //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.lo(i32 -1, i32 0)
            //     %threadIdInWave = call i32 @llvm.amdgcn.mbcnt.hi(i32 -1, i32 %threadIdInWave)
            //
            //     %waveIdInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 24, i32 4)
            //
            //     %threadIdInSubgroup = mul i32 %waveIdInSubgroup, %waveSize
            //     %threadIdInSubgroup = add i32 %threadIdInSubgroup, %threadIdInWave
            //
            //     %primCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 22, i32 9)
            //     %vertCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 12, i32 9)
            //
            //     %primCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
            //     %vertCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
            //
            // <if (distributePrimId)>
            // [
            // .writePrimId:
            //     ; Write LDS region (primitive ID)
            //     br label %.endWritePrimId
            //
            // .endWritePrimId:
            //     call void @llvm.amdgcn.s.barrier()
            //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
            //     br i1 %vertValid, label %.readPrimId, label %.endReadPrimId
            //
            // .readPrimId:
            //     ; Read LDS region (primitive ID)
            //     br label %.endReadPrimId
            //
            // .endReadPrimId:
            //     call void @llvm.amdgcn.s.barrier()
            // ]
            //     %firstThreadInSubgroup = icmp eq i32 %threadIdInSubgroup, 0
            //     br i1 %firstThreadInSubgroup, label %.zeroPrimWaveCount, label %.endZeroPrimWaveCount
            //
            // .zeroThreadCount:
            //     ; Zero LDS region (primitive/vertex count in waves), do it for the first thread
            //     br label %.endZeroThreadCount
            //
            // .endZeroThreadCount:
            //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
            //     br i1 %firstWaveInSubgroup, label %.zeroDrawFlag, label %.endZeroDrawFlag
            //
            // .zeroDrawFlag:
            //     ; Zero LDS regision (draw flag), do it for the first wave
            //     br label %.endZeroDrawFlag
            //
            // .endZeroDrawFlag:
            //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
            //     br i1 %vertValid, label %.writePosData, label %.endWritePosData
            //
            // .writePosData:
            //     ; Write LDS region (position data)
            //     %expData = call [ POS0: <4 x float>, POS1: <4 x float>, ...,
            //                       PARAM0: <4 x float>, PARAM1: <4 xfloat>, ... ]
            //                     @llpc.ngg.ES.exp(%sgpr..., %userData..., %vgpr...)
            //     br label %.endWritePosData
            //
            // .endWritePosData:
            //     call void @llvm.amdgcn.s.barrier(...)
            //
            //     %primValidInWave = icmp ult i32 %threadIdInWave, %primCountInWave
            //     %primValidInSubgroup = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
            //     %primValid = and i1 %primValidInWave, %primValidInSubgroup
            //     br i1 %primValid, label %.culling, label %.endCulling
            //
            // .culling:
            //     ; Do culling
            //     %doCull = call i32 @llpc.ngg.culling.XXX(...)
            //     br label %.endCulling
            //
            // .endCulling:
            //     %cullFlag = phi i1 [ true, %.endWritePosData ], [ %doCull, %.culling ]
            //     %drawFlag = xor i1 1, %cullFlag
            //     br i1 %drawFlag, label %.writeDrawFlag, label %.endWriteDrawFlag
            //
            // .writeDrawFlag:
            //     ; Write LDS region (draw flag)
            //     br label %.endWriteDrawFlag
            //
            // .endWriteDrawFlag:
            //     %drawMask = call i64 @llpc.subgroup.ballot(i1 %drawFlag)
            //     %drawCount = call i64 @llvm.ctpop.i64(i64 %drawMask)
            //     %hasSurviveDraw = icmp ne i64 %drawCount, 0
            //
            //     %theadIdUpbound = sub i32 %waveCountInSubgroup, %waveIdInSubgroup
            //     %threadValid = icmp ult i32 %threadIdInWave, %theadIdUpbound
            //     %primCountAcc = and i1 %hasSurviveDraw, %threadValid
            //     br i1 %primCountAcc, label %.accThreadCount, label %.endAccThreadCount
            //
            // .accThreadCount:
            //     ; Write LDS region (primitive/vertex count in waves)
            //     br label %.endAccThreadCount
            //
            // .endAccThreadCount:
            //     call void @llvm.amdgcn.s.barrier(...)
            //
            // <if (vertexCompact)>
            // [
            //      br lable %.readThreadCount
            //
            // .readThreadCount:
            //      %vertCount = ... (read LDS region, vertex count in waves)
            //
            //      %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
            //      br i1 %vertValid, label %.writeCompactData, label %.endWriteCompactData
            //
            // .writeCompactData:
            //      ; Write LDS region (compaction data: compacted thread ID, vertex position data,
            //      ; vertex ID/tessCoordX, instance ID/tessCoordY, primitive ID/relative patch ID, patch ID)
            //      br label %.endWriteCompactData
            //
            // .endWriteCompactData:
            //      %hasSurviveVert = icmp ne i32 %vertCount, 0
            //      br i1 %hasSurviveVert, label %.endReadThreadCount, label %.dummyAllocReq
            //
            // .dummyAllocReq:
            //      ; Do dummy parameter cache (PC) alloc request: s_sendmsg(GS_ALLOC_REQ, ...)
            //      ; primCount = 1, vertCount = 1
            //      br label %.endDummyAllocReq
            //
            // .endDummyAllocReq:
            //      %firstThreadInSubgroup = icmp eq i32 %threadIdInSubgroup, 0
            //      br i1 %firstThreadInSubgroup, label %.dummyExpPrim, label %.EndDummyExpPrim
            //
            // .dummyExpPrim:
            //      ; Do vertex position export: exp pos, ... (off, off, off, off)
            //      ; Do primitive export: exp prim, ... (0, off, off, off)
            //      br label %.EndDummyExpPrim
            //
            // .EndDummyExpPrim:
            //      ret void
            //
            // .endReadThreadCount:
            //      %vertCountInSubgroup = %vertCount
            //
            //      %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
            //      br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
            // ]
            // <else>
            // [
            //     %firstThreadInWave = icmp eq i32 %threadIdInWave, 0
            //     br i1 %firstThreadInWave, label %.readThreadCount, label %.endReadThreadCount
            //
            // .readThreadCount:
            //     %primCount = ... (read LDS region, primitive count in waves)
            //     br label %.endReadThreadCount
            //
            // .endReadThreadCount:
            //     %primCount = phi i32 [ primCountInSubgroup, %.endAccPrimCount ], [ %primCount, %.readThreadCount ]
            //     %hasSurvivePrim = icmp ne i32 %primCount, 0
            //     %primCountInSubgroup = select i1 %hasSurvivePrim, i32 %primCountInSubgroup, i32 0
            //     %hasSurvivePrim = icmp ne i32 %primCountInSubgroup, 0
            //     %vertCountInSubgroup = select i1 %hasSurvivePrim, i32 %vertCountInSubgroup, i32 0
            //
            //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
            //     br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
            // ]
            // .allocReq:
            //     ; Do parameter cache (PC) alloc request: s_sendmsg(GS_ALLOC_REQ, ...)
            //     br label %.endAllocReq
            //
            // .endAllocReq:
            //     %primExp = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
            //     br i1 %primExp, label %.expPrim, label %.endExpPrim
            //
            // .expPrim:
            //     ; Do primitive export: exp prim, ...
            //     br label %.endExpPrim
            //
            // .endExpPrim:
            //     %vertExp = icmp ult i32 %threadIdInSubgroup, %vertCountInSubgroup
            //     br i1 %vertExp, label %.expVertPos, label %.endExpVertPos
            //
            // .expVertPos:
            //     ; Do vertex position export: exp pos, ...
            //     br label %.endExpVertPos
            //
            // .endExpVertPos:
            //     br i1 %vertExp, label %.expVertParam, label %.endExpVertParam
            //
            // .expVertParam:
            //     ; Do vertex parameter export: exp param, ...
            //     br label %.endExpVertParam
            //
            // .endExpVertParam:
            //     ret void
            // }

            const bool vertexCompact = (m_pNggControl->compactMode == NggCompactVertices);

            // Define basic blocks
            auto pEndExpVertParamBlock = BasicBlock::Create(*m_pContext, ".endExpVertParam", pEntryPoint);
            auto pExpVertParamBlock =
                BasicBlock::Create(*m_pContext, ".expVertParam", pEntryPoint, pEndExpVertParamBlock);

            auto pEndExpVertPosBlock =
                BasicBlock::Create(*m_pContext, ".endExpVertPos", pEntryPoint, pExpVertParamBlock);
            auto pExpVertPosBlock = BasicBlock::Create(*m_pContext, ".expVertPos", pEntryPoint, pEndExpVertPosBlock);

            auto pEndExpPrimBlock = BasicBlock::Create(*m_pContext, ".endExpPrim", pEntryPoint, pExpVertPosBlock);
            auto pExpPrimBlock = BasicBlock::Create(*m_pContext, ".expPrim", pEntryPoint, pEndExpPrimBlock);

            auto pEndAllocReqBlock = BasicBlock::Create(*m_pContext, ".endAllocReq", pEntryPoint, pExpPrimBlock);
            auto pAllocReqBlock = BasicBlock::Create(*m_pContext, ".allocReq", pEntryPoint, pEndAllocReqBlock);

            // NOTE: Those basic blocks are conditionally created on the basis of actual NGG compaction mode.
            BasicBlock* pEndWriteCompactDataBlock = nullptr;
            BasicBlock* pWriteCompactDataBlock = nullptr;
            BasicBlock* pEndReadThreadCountBlock = nullptr;
            BasicBlock* pReadThreadCountBlock = nullptr;

            if (vertexCompact)
            {
                pEndReadThreadCountBlock =
                    BasicBlock::Create(*m_pContext, ".endReadThreadCount", pEntryPoint, pAllocReqBlock);
                pEndWriteCompactDataBlock =
                    BasicBlock::Create(*m_pContext, ".endWriteCompactData", pEntryPoint, pEndReadThreadCountBlock);
                pWriteCompactDataBlock =
                    BasicBlock::Create(*m_pContext, ".writeCompactData", pEntryPoint, pEndWriteCompactDataBlock);
                pReadThreadCountBlock =
                    BasicBlock::Create(*m_pContext, ".readThreadCount", pEntryPoint, pWriteCompactDataBlock);
            }
            else
            {
                pEndReadThreadCountBlock =
                    BasicBlock::Create(*m_pContext, ".endReadThreadCount", pEntryPoint, pAllocReqBlock);
                pReadThreadCountBlock =
                    BasicBlock::Create(*m_pContext, ".readThreadCount", pEntryPoint, pEndReadThreadCountBlock);
            }

            auto pEndAccThreadCountBlock =
                BasicBlock::Create(*m_pContext, ".endAccThreadCount", pEntryPoint, pReadThreadCountBlock);
            auto pAccThreadCountBlock =
                BasicBlock::Create(*m_pContext, ".accThreadCount", pEntryPoint, pEndAccThreadCountBlock);

            auto pEndWriteDrawFlagBlock =
                BasicBlock::Create(*m_pContext, ".endWriteDrawFlag", pEntryPoint, pAccThreadCountBlock);
            auto pWriteDrawFlagBlock =
                BasicBlock::Create(*m_pContext, ".writeDrawFlag", pEntryPoint, pEndWriteDrawFlagBlock);

            auto pEndCullingBlock = BasicBlock::Create(*m_pContext, ".endCulling", pEntryPoint, pWriteDrawFlagBlock);
            auto pCullingBlock = BasicBlock::Create(*m_pContext, ".culling", pEntryPoint, pEndCullingBlock);

            auto pEndWritePosDataBlock =
                BasicBlock::Create(*m_pContext, ".endWritePosData", pEntryPoint, pCullingBlock);
            auto pWritePosDataBlock =
                BasicBlock::Create(*m_pContext, ".writePosData", pEntryPoint, pEndWritePosDataBlock);

            auto pEndZeroDrawFlagBlock =
                BasicBlock::Create(*m_pContext, ".endZeroDrawFlag", pEntryPoint, pWritePosDataBlock);
            auto pZeroDrawFlagBlock =
                BasicBlock::Create(*m_pContext, ".zeroDrawFlag", pEntryPoint, pEndZeroDrawFlagBlock);

            auto pEndZeroThreadCountBlock =
                BasicBlock::Create(*m_pContext, ".endZeroThreadCount", pEntryPoint, pZeroDrawFlagBlock);
            auto pZeroThreadCountBlock =
                BasicBlock::Create(*m_pContext, ".zeroThreadCount", pEntryPoint, pEndZeroThreadCountBlock);

            auto pEntryBlock = BasicBlock::Create(*m_pContext, ".entry", pEntryPoint, pZeroThreadCountBlock);

            // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
            BasicBlock* pWritePrimIdBlock       = nullptr;
            BasicBlock* pEndWritePrimIdBlock    = nullptr;
            BasicBlock* pReadPrimIdBlock        = nullptr;
            BasicBlock* pEndReadPrimIdBlock     = nullptr;

            if (distributePrimId)
            {
                pEndReadPrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".endReadPrimId", pEntryPoint, pZeroThreadCountBlock);
                pReadPrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".readPrimId", pEntryPoint, pEndReadPrimIdBlock);
                pEndWritePrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".endWritePrimId", pEntryPoint, pReadPrimIdBlock);
                pWritePrimIdBlock =
                    BasicBlock::Create(*m_pContext, ".writePrimId", pEntryPoint, pEndWritePrimIdBlock);
            }

            // Construct ".entry" block
            {
                args.clear();
                args.push_back(ConstantInt::get(m_pContext->Int64Ty(), -1));

                attribs.clear();
                attribs.push_back(Attribute::NoRecurse);

                EmitCall(pModule, "llvm.amdgcn.init.exec", m_pContext->VoidTy(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

                attribs.clear();
                attribs.push_back(Attribute::NoRecurse);

                auto pThreadIdInWave =
                    EmitCall(pModule, "llvm.amdgcn.mbcnt.lo", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                if (waveSize == 64)
                {
                    args.clear();
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), -1));
                    args.push_back(pThreadIdInWave);

                    pThreadIdInWave = EmitCall(pModule,
                                               "llvm.amdgcn.mbcnt.hi",
                                               m_pContext->Int32Ty(),
                                               args, attribs, pEntryBlock);
                }

                attribs.clear();
                attribs.push_back(Attribute::ReadNone);

                args.clear();
                args.push_back(pMergedGroupInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 22));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 9));

                auto pPrimCountInSubgroup =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedGroupInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 12));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 9));

                auto pVertCountInSubgroup =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedWaveInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

                auto pVertCountInWave =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedWaveInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 8));

                auto pPrimCountInWave =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                args.clear();
                args.push_back(pMergedWaveInfo);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 24));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 4));

                auto pWaveIdInSubgroup =
                    EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pEntryBlock);

                auto pThreadIdInSubgroup =
                    BinaryOperator::CreateMul(pWaveIdInSubgroup,
                                              ConstantInt::get(m_pContext->Int32Ty(), waveSize),
                                              "",
                                              pEntryBlock);
                pThreadIdInSubgroup = BinaryOperator::CreateAdd(pThreadIdInSubgroup, pThreadIdInWave, "", pEntryBlock);

                // Record NGG factors for future calculation
                m_nggFactor.pPrimCountInSubgroup    = pPrimCountInSubgroup;
                m_nggFactor.pVertCountInSubgroup    = pVertCountInSubgroup;
                m_nggFactor.pPrimCountInWave        = pPrimCountInWave;
                m_nggFactor.pVertCountInWave        = pVertCountInWave;
                m_nggFactor.pThreadIdInWave         = pThreadIdInWave;
                m_nggFactor.pThreadIdInSubgroup     = pThreadIdInSubgroup;
                m_nggFactor.pWaveIdInSubgroup       = pWaveIdInSubgroup;

                m_nggFactor.pPrimShaderTableAddrLow  = pPrimShaderTableAddrLow;
                m_nggFactor.pPrimShaderTableAddrHigh = pPrimShaderTableAddrHigh;

                m_nggFactor.pEsGsOffsets01          = pEsGsOffsets01;
                m_nggFactor.pEsGsOffsets23          = pEsGsOffsets23;
                m_nggFactor.pEsGsOffsets45          = pEsGsOffsets45;

                if (distributePrimId)
                {
                    auto pPrimValid = new ICmpInst(*pEntryBlock,
                                                   ICmpInst::ICMP_ULT,
                                                   pThreadIdInWave,
                                                   pPrimCountInWave,
                                                   "");
                    BranchInst::Create(pWritePrimIdBlock, pEndWritePrimIdBlock, pPrimValid, pEntryBlock);
                }
                else
                {
                    auto pFirstThreadInSubgroup = new ICmpInst(*pEntryBlock,
                                                               ICmpInst::ICMP_EQ,
                                                               pThreadIdInSubgroup,
                                                               ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                               "");
                    BranchInst::Create(pZeroThreadCountBlock,
                                       pEndZeroThreadCountBlock,
                                       pFirstThreadInSubgroup,
                                       pEntryBlock);
                }
            }

            if (distributePrimId)
            {
                // Construct ".writePrimId" block
                {
                    // Primitive data layout
                    //   ES_GS_OFFSET23[15:0]  = vertexId2 (in DWORDs)
                    //   ES_GS_OFFSET01[31:16] = vertexId1 (in DWORDs)
                    //   ES_GS_OFFSET01[15:0]  = vertexId0 (in DWORDs)
                    attribs.clear();
                    attribs.push_back(Attribute::ReadNone);

                    args.clear();
                    args.push_back(m_nggFactor.pEsGsOffsets01);
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

                    // Use vertex0 as provoking vertex to distribute primitive ID
                    auto pEsGsOffset0 = EmitCall(pModule,
                                                 "llvm.amdgcn.ubfe.i32",
                                                 m_pContext->Int32Ty(),
                                                 args,
                                                 attribs,
                                                 pWritePrimIdBlock);

                    auto pVertexId0 = BinaryOperator::CreateLShr(pEsGsOffset0,
                                                                ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                                "",
                                                                pWritePrimIdBlock);

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);
                    auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                    auto pLdsOffset = BinaryOperator::CreateShl(pVertexId0,
                                                                ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                                "",
                                                                pWritePrimIdBlock);
                    pLdsOffset = BinaryOperator::CreateAdd(pRegionStart, pLdsOffset, "", pWritePrimIdBlock);

                    auto pPrimIdWriteValue = pGsPrimitiveId;
                    m_pLdsManager->WriteValueToLds(pPrimIdWriteValue, pLdsOffset, pWritePrimIdBlock);

                    BranchInst::Create(pEndWritePrimIdBlock, pWritePrimIdBlock);
                }

                // Construct ".endWritePrimId" block
                {
                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEndWritePrimIdBlock);

                    auto pVertValid = new ICmpInst(*pEndWritePrimIdBlock,
                                                   ICmpInst::ICMP_ULT,
                                                   m_nggFactor.pThreadIdInWave,
                                                   m_nggFactor.pVertCountInWave,
                                                   "");
                    BranchInst::Create(pReadPrimIdBlock, pEndReadPrimIdBlock, pVertValid, pEndWritePrimIdBlock);
                }

                // Construct ".readPrimId" block
                Value* pPrimIdReadValue = nullptr;
                {
                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                    auto pLdsOffset = BinaryOperator::CreateShl(m_nggFactor.pThreadIdInSubgroup,
                                                                ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                                "",
                                                                pReadPrimIdBlock);
                    pLdsOffset = BinaryOperator::CreateAdd(ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                                           pLdsOffset,
                                                           "",
                                                           pReadPrimIdBlock);
                    pPrimIdReadValue =
                        m_pLdsManager->ReadValueFromLds(m_pContext->Int32Ty(), pLdsOffset, pReadPrimIdBlock);

                    BranchInst::Create(pEndReadPrimIdBlock, pReadPrimIdBlock);
                }

                // Construct ".endReadPrimId" block
                {
                    auto pPrimitiveId = PHINode::Create(m_pContext->Int32Ty(), 2, "", pEndReadPrimIdBlock);

                    pPrimitiveId->addIncoming(pPrimIdReadValue, pReadPrimIdBlock);
                    pPrimitiveId->addIncoming(ConstantInt::get(m_pContext->Int32Ty(), 0), pEndWritePrimIdBlock);

                    // Record primitive ID
                    m_nggFactor.pPrimitiveId = pPrimitiveId;

                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEndReadPrimIdBlock);

                    auto pFirstThreadInSubgroup = new ICmpInst(*pEndReadPrimIdBlock,
                                                               ICmpInst::ICMP_EQ,
                                                               m_nggFactor.pThreadIdInSubgroup,
                                                               ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                               "");
                    BranchInst::Create(pZeroThreadCountBlock,
                                       pEndZeroThreadCountBlock,
                                       pFirstThreadInSubgroup,
                                       pEndReadPrimIdBlock);
                }
            }

            // Construct ".zeroThreadCount" block
            {
                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(
                    vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);

                auto pZero = ConstantInt::get(m_pContext->Int32Ty(), 0);

                // Zero per-wave primitive/vertex count
                std::vector<Constant*> zeros;
                for (uint32_t i = 0; i < Gfx9::NggMaxWavesPerSubgroup; ++i)
                {
                    zeros.push_back(pZero);
                }
                auto pZeros = ConstantVector::get(zeros);

                auto pLdsOffset = ConstantInt::get(m_pContext->Int32Ty(), regionStart);
                m_pLdsManager->WriteValueToLds(pZeros, pLdsOffset, pZeroThreadCountBlock);

                // Zero sub-group primitive/vertex count
                pLdsOffset = ConstantInt::get(m_pContext->Int32Ty(),
                                                regionStart + SizeOfDword * Gfx9::NggMaxWavesPerSubgroup);
                m_pLdsManager->WriteValueToLds(pZero, pLdsOffset, pZeroThreadCountBlock);

                BranchInst::Create(pEndZeroThreadCountBlock, pZeroThreadCountBlock);
            }

            // Construct ".endZeroThreadCount" block
            {
                auto pFirstWaveInSubgroup = new ICmpInst(*pEndZeroThreadCountBlock,
                                                         ICmpInst::ICMP_EQ,
                                                         m_nggFactor.pWaveIdInSubgroup,
                                                         ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                         "");
                BranchInst::Create(
                    pZeroDrawFlagBlock, pEndZeroDrawFlagBlock, pFirstWaveInSubgroup, pEndZeroThreadCountBlock);
            }

            // Construct ".zeroDrawFlag" block
            {
                Value* pLdsOffset =
                    BinaryOperator::CreateMul(m_nggFactor.pThreadIdInWave,
                                              ConstantInt::get(m_pContext->Int32Ty(), SizeOfDword),
                                              "",
                                              pZeroDrawFlagBlock);

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);
                auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pRegionStart, "", pZeroDrawFlagBlock);

                auto pZero = ConstantInt::get(m_pContext->Int32Ty(), 0);
                m_pLdsManager->WriteValueToLds(pZero, pLdsOffset, pZeroDrawFlagBlock);

                if (waveCountInSubgroup == 8)
                {
                    LLPC_ASSERT(waveSize == 32);
                    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                           ConstantInt::get(m_pContext->Int32Ty(), 32 * SizeOfDword),
                                                           "",
                                                           pZeroDrawFlagBlock);
                    m_pLdsManager->WriteValueToLds(pZero, pLdsOffset, pZeroDrawFlagBlock);
                }

                BranchInst::Create(pEndZeroDrawFlagBlock, pZeroDrawFlagBlock);
            }

            // Construct ".endZeroDrawFlag" block
            {
                auto pVertValid = new ICmpInst(*pEndZeroDrawFlagBlock,
                                               ICmpInst::ICMP_ULT,
                                               m_nggFactor.pThreadIdInWave,
                                               m_nggFactor.pVertCountInWave,
                                               "");
                BranchInst::Create(
                    pWritePosDataBlock, pEndWritePosDataBlock, pVertValid, pEndZeroDrawFlagBlock);
            }

            // Construct ".writePosData" block
            std::vector<ExpData> expDataSet;
            bool separateExp = false;
            {
                separateExp = (pResUsage->resourceWrite == false); // No resource writing

                // NOTE: For vertex compaction, we have to run ES for twice (get vertex position data and
                // get other exported data).
                const auto entryName = (separateExp || vertexCompact) ? LlpcName::NggEsEntryVariantExpPos :
                                                                        LlpcName::NggEsEntryVariantExp;

                RunEsOrEsVariant(pModule,
                                 entryName,
                                 pEntryPoint->arg_begin(),
                                 false,
                                 &expDataSet,
                                 pWritePosDataBlock);

                // Write vertex position data to LDS
                for (const auto& expData : expDataSet)
                {
                    if (expData.target == EXP_TARGET_POS_0)
                    {
                        const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);

                        Value* pLdsOffset = BinaryOperator::CreateMul(m_nggFactor.pThreadIdInSubgroup,
                                                                      ConstantInt::get(m_pContext->Int32Ty(), SizeOfVec4),
                                                                      "",
                                                                      pWritePosDataBlock);
                        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                               ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                                               "",
                                                               pWritePosDataBlock);

                        m_pLdsManager->WriteValueToLds(expData.pExpValue, pLdsOffset, pWritePosDataBlock);

                        break;
                    }
                }

                // Write cull distance sign mask to LDS
                if (m_pNggControl->enableCullDistanceCulling)
                {
                    uint32_t clipCullPos = EXP_TARGET_POS_1;
                    std::vector<Value*> clipCullDistance;
                    std::vector<Value*> cullDistance;

                    bool usePointSize     = false;
                    bool useLayer         = false;
                    bool useViewportIndex = false;
                    uint32_t clipDistanceCount = 0;
                    uint32_t cullDistanceCount = 0;

                    if (hasTs)
                    {
                        const auto& builtInUsage = pResUsage->builtInUsage.tes;

                        usePointSize        = builtInUsage.pointSize;
                        useLayer            = builtInUsage.layer;
                        useViewportIndex    = builtInUsage.viewportIndex;
                        clipDistanceCount   = builtInUsage.clipDistance;
                        cullDistanceCount   = builtInUsage.cullDistance;
                    }
                    else
                    {
                        const auto& builtInUsage = pResUsage->builtInUsage.vs;

                        usePointSize        = builtInUsage.pointSize;
                        useLayer            = builtInUsage.layer;
                        useViewportIndex    = builtInUsage.viewportIndex;
                        clipDistanceCount   = builtInUsage.clipDistance;
                        cullDistanceCount   = builtInUsage.cullDistance;
                    }

                    // NOTE: When gl_PointSize, gl_Layer, or gl_ViewportIndex is used, gl_ClipDistance[] or
                    // gl_CullDistance[] should start from pos2.
                    clipCullPos = (usePointSize || useLayer || useViewportIndex) ?
                                      EXP_TARGET_POS_2 : EXP_TARGET_POS_1;

                    // Collect clip/cull distance from exported value
                    for (const auto& expData : expDataSet)
                    {
                        if ((expData.target == clipCullPos) || (expData.target == clipCullPos + 1))
                        {
                            for (uint32_t i = 0; i < 4; ++i)
                            {
                                auto pExpValue = ExtractElementInst::Create(expData.pExpValue,
                                                                            ConstantInt::get(m_pContext->Int32Ty(), i),
                                                                            "",
                                                                            pWritePosDataBlock);
                                clipCullDistance.push_back(pExpValue);
                            }
                        }
                    }
                    LLPC_ASSERT(clipCullDistance.size() < MaxClipCullDistanceCount);

                    for (uint32_t i = clipDistanceCount; i < clipDistanceCount + cullDistanceCount; ++i)
                    {
                        cullDistance.push_back(clipCullDistance[i]);
                    }

                    // Calculate the sign mask for cull distance
                    Value* pSignMask = ConstantInt::get(m_pContext->Int32Ty(), 0);
                    for (uint32_t i = 0; i < cullDistance.size(); ++i)
                    {
                        auto pCullDistance = new BitCastInst(cullDistance[i], m_pContext->Int32Ty(), "", pWritePosDataBlock);

                        attribs.clear();
                        attribs.push_back(Attribute::ReadNone);

                        args.clear();
                        args.push_back(pCullDistance);
                        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 31));
                        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 1));

                        Value* pSignBit = EmitCall(pModule,
                                                   "llvm.amdgcn.ubfe.i32",
                                                   m_pContext->Int32Ty(),
                                                   args,
                                                   attribs,
                                                   pWritePosDataBlock);

                        pSignBit = BinaryOperator::CreateShl(pSignBit,
                                                             ConstantInt::get(m_pContext->Int32Ty(), i),
                                                             "",
                                                             pWritePosDataBlock);

                        pSignMask = BinaryOperator::CreateOr(pSignMask, pSignBit, "", pWritePosDataBlock);

                    }

                    // Write the sign mask to LDS
                    const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionCullDistance);

                    Value* pLdsOffset = BinaryOperator::CreateMul(m_nggFactor.pThreadIdInSubgroup,
                                                                  ConstantInt::get(m_pContext->Int32Ty(), SizeOfDword),
                                                                  "",
                                                                  pWritePosDataBlock);
                    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                           ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                                           "",
                                                           pWritePosDataBlock);

                    m_pLdsManager->WriteValueToLds(pSignMask, pLdsOffset, pWritePosDataBlock);
                }

                BranchInst::Create(pEndWritePosDataBlock, pWritePosDataBlock);
            }

            // Construct ".endWritePosData" block
            {
                auto pUndef = UndefValue::get(m_pContext->Floatx4Ty());
                for (auto& expData : expDataSet)
                {
                    PHINode* pExpValue = PHINode::Create(m_pContext->Floatx4Ty(), 2, "", pEndWritePosDataBlock);
                    pExpValue->addIncoming(expData.pExpValue, pWritePosDataBlock);
                    pExpValue->addIncoming(pUndef, pEndZeroDrawFlagBlock);

                    expData.pExpValue = pExpValue; // Update the exportd data
                }

                attribs.clear();
                attribs.push_back(Attribute::NoRecurse);
                args.clear();

                EmitCall(pModule,
                         "llvm.amdgcn.s.barrier",
                         m_pContext->VoidTy(),
                         args,
                         attribs,
                         pEndWritePosDataBlock);

                auto pPrimValidInWave = new ICmpInst(*pEndWritePosDataBlock,
                                                     ICmpInst::ICMP_ULT,
                                                     m_nggFactor.pThreadIdInWave,
                                                     m_nggFactor.pPrimCountInWave,
                                                     "");
                auto pPrimValidInSubgroup = new ICmpInst(*pEndWritePosDataBlock,
                                                         ICmpInst::ICMP_ULT,
                                                         m_nggFactor.pThreadIdInSubgroup,
                                                         m_nggFactor.pPrimCountInSubgroup,
                                                         "");

                auto pPrimValid =
                    BinaryOperator::CreateAnd(pPrimValidInWave, pPrimValidInSubgroup, "", pEndWritePosDataBlock);
                BranchInst::Create(pCullingBlock, pEndCullingBlock, pPrimValid, pEndWritePosDataBlock);
            }

            // Construct ".culling" block
            Value* pDoCull = nullptr;
            {
                pDoCull = DoCulling(pModule, pCullingBlock);
                BranchInst::Create(pEndCullingBlock, pCullingBlock);
            }

            // Construct ".endCulling" block
            Value* pDrawFlag = nullptr;
            PHINode* pCullFlag = nullptr;
            {
                pCullFlag = PHINode::Create(m_pContext->BoolTy(), 2, "", pEndCullingBlock);
                pCullFlag->addIncoming(ConstantInt::get(m_pContext->BoolTy(), true), pEndWritePosDataBlock);
                pCullFlag->addIncoming(pDoCull, pCullingBlock);

                pDrawFlag = BinaryOperator::CreateNot(pCullFlag, "", pEndCullingBlock);
                BranchInst::Create(pWriteDrawFlagBlock, pEndWriteDrawFlagBlock, pDrawFlag, pEndCullingBlock);
            }

            // Construct ".writeDrawFlag" block
            {
                attribs.clear();
                attribs.push_back(Attribute::ReadNone);

                args.clear();
                args.push_back(pEsGsOffsets01);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

                auto pEsGsOffset0 = EmitCall(pModule,
                                             "llvm.amdgcn.ubfe.i32",
                                             m_pContext->Int32Ty(),
                                             args,
                                             attribs,
                                             pWriteDrawFlagBlock);
                auto pVertexId0 = BinaryOperator::CreateLShr(pEsGsOffset0,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                             "",
                                                             pWriteDrawFlagBlock);

                args.clear();
                args.push_back(pEsGsOffsets01);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

                auto pEsGsOffset1 = EmitCall(pModule,
                                             "llvm.amdgcn.ubfe.i32",
                                             m_pContext->Int32Ty(),
                                             args,
                                             attribs,
                                             pWriteDrawFlagBlock);
                auto pVertexId1 = BinaryOperator::CreateLShr(pEsGsOffset1,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                             "",
                                                             pWriteDrawFlagBlock);

                args.clear();
                args.push_back(pEsGsOffsets23);
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
                args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

                auto pEsGsOffset2 = EmitCall(pModule,
                                             "llvm.amdgcn.ubfe.i32",
                                             m_pContext->Int32Ty(),
                                             args,
                                             attribs,
                                             pWriteDrawFlagBlock);
                auto pVertexId2 = BinaryOperator::CreateLShr(pEsGsOffset2,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                             "",
                                                             pWriteDrawFlagBlock);

                Value* vertexId[3] = { pVertexId0, pVertexId1, pVertexId2 };

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);
                auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                auto pOne = ConstantInt::get(m_pContext->Int8Ty(), 1);

                for (uint32_t i = 0; i < 3; ++i)
                {
                    auto pLdsOffset = BinaryOperator::CreateAdd(pRegionStart, vertexId[i], "", pWriteDrawFlagBlock);
                    m_pLdsManager->WriteValueToLds(pOne, pLdsOffset, pWriteDrawFlagBlock);
                }

                BranchInst::Create(pEndWriteDrawFlagBlock, pWriteDrawFlagBlock);
            }

            // Construct ".endWriteDrawFlag" block
            Value* pDrawCount = nullptr;
            {
                if (vertexCompact)
                {
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);
                    args.clear();

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEndWriteDrawFlagBlock);

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);
                    auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                    auto pLdsOffset = BinaryOperator::CreateAdd(m_nggFactor.pThreadIdInSubgroup,
                                                                pRegionStart,
                                                                "",
                                                                pEndWriteDrawFlagBlock);

                    pDrawFlag =
                        m_pLdsManager->ReadValueFromLds(m_pContext->Int8Ty(), pLdsOffset, pEndWriteDrawFlagBlock);
                    pDrawFlag = new TruncInst(pDrawFlag, m_pContext->BoolTy(), "", pEndWriteDrawFlagBlock);
                }

                auto pDrawMask = DoSubgroupBallot(pModule, pDrawFlag, pEndWriteDrawFlagBlock);

                args.clear();
                args.push_back(pDrawMask);

                pDrawCount = EmitCall(pModule,
                                      "llvm.ctpop.i64",
                                      m_pContext->Int64Ty(),
                                      args,
                                      NoAttrib,
                                      pEndWriteDrawFlagBlock);

                pDrawCount = new TruncInst(pDrawCount, m_pContext->Int32Ty(), "", pEndWriteDrawFlagBlock);

                auto pWaveCountInSubgroup = ConstantInt::get(m_pContext->Int32Ty(), waveCountInSubgroup);

                auto pThreadIdUpbound = BinaryOperator::CreateSub(pWaveCountInSubgroup,
                                                                  m_nggFactor.pWaveIdInSubgroup,
                                                                  "",
                                                                  pEndWriteDrawFlagBlock);
                auto pThreadValid = new ICmpInst(*pEndWriteDrawFlagBlock,
                                                 ICmpInst::ICMP_ULT,
                                                 m_nggFactor.pThreadIdInWave,
                                                 pThreadIdUpbound,
                                                 "");

                Value* pPrimCountAcc = nullptr;
                if (vertexCompact)
                {
                    pPrimCountAcc = pThreadValid;
                }
                else
                {
                    auto pHasSurviveDraw = new ICmpInst(*pEndWriteDrawFlagBlock,
                                                        ICmpInst::ICMP_NE,
                                                        pDrawCount,
                                                        ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                        "");

                    pPrimCountAcc =
                        BinaryOperator::CreateAnd(pHasSurviveDraw, pThreadValid, "", pEndWriteDrawFlagBlock);
                }

                BranchInst::Create(pAccThreadCountBlock,
                                   pEndAccThreadCountBlock,
                                   pPrimCountAcc,
                                   pEndWriteDrawFlagBlock);
            }

            // Construct ".accThreadCount" block
            {
                auto pLdsOffset = BinaryOperator::CreateAdd(m_nggFactor.pWaveIdInSubgroup,
                                                            m_nggFactor.pThreadIdInWave,
                                                            "",
                                                            pAccThreadCountBlock);
                pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                       ConstantInt::get(m_pContext->Int32Ty(), 1),
                                                       "",
                                                       pAccThreadCountBlock);
                pLdsOffset = BinaryOperator::CreateShl(pLdsOffset,
                                                       ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                       "",
                                                       pAccThreadCountBlock);

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(
                    vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);
                auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pRegionStart, "", pAccThreadCountBlock);
                m_pLdsManager->AtomicOpWithLds(AtomicRMWInst::Add, pDrawCount, pLdsOffset, pAccThreadCountBlock);

                BranchInst::Create(pEndAccThreadCountBlock, pAccThreadCountBlock);
            }

            // Construct ".endAccThreadCount" block
            {
                args.clear();
                attribs.clear();
                attribs.push_back(Attribute::NoRecurse);

                EmitCall(pModule,
                         "llvm.amdgcn.s.barrier",
                         m_pContext->VoidTy(),
                         args,
                         attribs,
                         pEndAccThreadCountBlock);

                if (vertexCompact)
                {
                    BranchInst::Create(pReadThreadCountBlock, pEndAccThreadCountBlock);
                }
                else
                {
                    auto pFirstThreadInWave = new ICmpInst(*pEndAccThreadCountBlock,
                                                           ICmpInst::ICMP_EQ,
                                                           m_nggFactor.pThreadIdInWave,
                                                           ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                           "");

                    BranchInst::Create(pReadThreadCountBlock,
                                       pEndReadThreadCountBlock,
                                       pFirstThreadInWave,
                                       pEndAccThreadCountBlock);
                }
            }

            if (vertexCompact)
            {
                // Construct ".readThreadCount" block
                Value* pVertCountInWaves = nullptr;
                Value* pVertCountInPrevWaves = nullptr;
                {
                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionVertCountInWaves);
                    auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

                    // The DWORD following DWORDs for all waves stores the vertex count of the entire sub-group
                    Value* pLdsOffset = ConstantInt::get(m_pContext->Int32Ty(),
                                                         regionStart + waveCountInSubgroup * SizeOfDword);
                    pVertCountInWaves =
                        m_pLdsManager->ReadValueFromLds(m_pContext->Int32Ty(), pLdsOffset, pReadThreadCountBlock);

                    // NOTE: We promote vertex count in waves to SGPR since it is treated as an uniform value.
                    args.clear();
                    args.push_back(pVertCountInWaves);

                    pVertCountInWaves = EmitCall(pModule,
                                                 "llvm.amdgcn.readfirstlane",
                                                 m_pContext->Int32Ty(),
                                                 args,
                                                 attribs,
                                                 pReadThreadCountBlock);

                    // Get vertex count for all waves prior to this wave
                    pLdsOffset = BinaryOperator::CreateShl(m_nggFactor.pWaveIdInSubgroup,
                                                           ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                           "",
                                                           pReadThreadCountBlock);
                    pLdsOffset = BinaryOperator::CreateAdd(pRegionStart, pLdsOffset, "", pReadThreadCountBlock);

                    pVertCountInPrevWaves =
                        m_pLdsManager->ReadValueFromLds(m_pContext->Int32Ty(), pLdsOffset, pReadThreadCountBlock);

                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pReadThreadCountBlock);

                    auto pVertValid = new ICmpInst(*pReadThreadCountBlock,
                                                   ICmpInst::ICMP_ULT,
                                                   m_nggFactor.pThreadIdInWave,
                                                   m_nggFactor.pVertCountInWave,
                                                   "");

                    auto pCompactDataWrite =
                        BinaryOperator::CreateAnd(pDrawFlag, pVertValid, "", pReadThreadCountBlock);

                    BranchInst::Create(
                        pWriteCompactDataBlock, pEndWriteCompactDataBlock, pCompactDataWrite, pReadThreadCountBlock);
                }

                // Construct ".writeCompactData" block
                {
                    args.clear();
                    args.push_back(pDrawFlag);

                    Value* pDrawMask = DoSubgroupBallot(pModule, pDrawFlag, pWriteCompactDataBlock);
                    pDrawMask = new BitCastInst(pDrawMask, m_pContext->Int32x2Ty(), "", pWriteCompactDataBlock);

                    auto pDrawMaskLow  = ExtractElementInst::Create(pDrawMask,
                                                                    ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                                    "",
                                                                    pWriteCompactDataBlock);
                    args.clear();
                    args.push_back(pDrawMaskLow);
                    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));

                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    Value* pCompactThreadIdInSubrgoup = EmitCall(pModule,
                                                                 "llvm.amdgcn.mbcnt.lo",
                                                                 m_pContext->Int32Ty(),
                                                                 args,
                                                                 attribs,
                                                                 pWriteCompactDataBlock);

                    if (waveSize == 64)
                    {
                        auto pDrawMaskHigh = ExtractElementInst::Create(pDrawMask,
                                                                        ConstantInt::get(m_pContext->Int32Ty(), 1),
                                                                        "",
                                                                        pWriteCompactDataBlock);

                        args.clear();
                        args.push_back(pDrawMaskHigh);
                        args.push_back(pCompactThreadIdInSubrgoup);

                        pCompactThreadIdInSubrgoup = EmitCall(pModule,
                                                              "llvm.amdgcn.mbcnt.hi",
                                                              m_pContext->Int32Ty(),
                                                              args,
                                                              attribs,
                                                              pWriteCompactDataBlock);
                    }

                    pCompactThreadIdInSubrgoup = BinaryOperator::CreateAdd(pVertCountInPrevWaves,
                                                                           pCompactThreadIdInSubrgoup,
                                                                           "",
                                                                           pWriteCompactDataBlock);

                    // Write vertex position data to LDS
                    for (const auto& expData : expDataSet)
                    {
                        if (expData.target == EXP_TARGET_POS_0)
                        {
                            const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);

                            Value* pLdsOffset = BinaryOperator::CreateMul(pCompactThreadIdInSubrgoup,
                                                                          ConstantInt::get(m_pContext->Int32Ty(), SizeOfVec4),
                                                                          "",
                                                                          pWriteCompactDataBlock);
                            pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                                   ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                                                   "",
                                                                   pWriteCompactDataBlock);

                            m_pLdsManager->WriteValueToLds(expData.pExpValue, pLdsOffset, pWriteCompactDataBlock);

                            break;
                        }
                    }

                    // Write thread ID in sub-group to LDS
                    Value* pCompactThreadId = new TruncInst(pCompactThreadIdInSubrgoup,
                                                            m_pContext->Int8Ty(),
                                                            "",
                                                            pWriteCompactDataBlock);
                    WriteCompactDataToLds(pCompactThreadId,
                                          m_nggFactor.pThreadIdInSubgroup,
                                          LdsRegionCompactThreadIdInSubgroup,
                                          pWriteCompactDataBlock);

                    if (hasTs)
                    {
                        // Write X/Y of tessCoord (U/V) to LDS
                        if (pResUsage->builtInUsage.tes.tessCoord)
                        {
                            WriteCompactDataToLds(pTessCoordX,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactTessCoordX,
                                                  pWriteCompactDataBlock);

                            WriteCompactDataToLds(pTessCoordY,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactTessCoordY,
                                                  pWriteCompactDataBlock);
                        }

                        // Write relative patch ID to LDS
                        WriteCompactDataToLds(pRelPatchId,
                                              pCompactThreadIdInSubrgoup,
                                              LdsRegionCompactRelPatchId,
                                              pWriteCompactDataBlock);

                        // Write patch ID to LDS
                        if (pResUsage->builtInUsage.tes.primitiveId)
                        {
                            WriteCompactDataToLds(pPatchId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactPatchId,
                                                  pWriteCompactDataBlock);
                        }
                    }
                    else
                    {
                        // Write vertex ID to LDS
                        if (pResUsage->builtInUsage.vs.vertexIndex)
                        {
                            WriteCompactDataToLds(pVertexId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactVertexId,
                                                  pWriteCompactDataBlock);
                        }

                        // Write instance ID to LDS
                        if (pResUsage->builtInUsage.vs.instanceIndex)
                        {
                            WriteCompactDataToLds(pInstanceId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactInstanceId,
                                                  pWriteCompactDataBlock);
                        }

                        // Write primitive ID to LDS
                        if (pResUsage->builtInUsage.vs.primitiveId)
                        {
                            LLPC_ASSERT(m_nggFactor.pPrimitiveId != nullptr);
                            WriteCompactDataToLds(m_nggFactor.pPrimitiveId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactPrimId,
                                                  pWriteCompactDataBlock);
                        }
                    }

                    BranchInst::Create(pEndWriteCompactDataBlock, pWriteCompactDataBlock);
                }

                // Construct dummy export blocks
                BasicBlock* pDummyExportBlock = nullptr;
                {
                    pDummyExportBlock = ConstructDummyExport(pModule, pEntryPoint);
                }

                // Construct ".endWriteCompactData" block
                {
                    Value* pHasSurviveVert = new ICmpInst(*pEndWriteCompactDataBlock,
                                                          ICmpInst::ICMP_NE,
                                                          pVertCountInWaves,
                                                          ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                          "");

                    BranchInst::Create(pEndReadThreadCountBlock,
                                       pDummyExportBlock,
                                       pHasSurviveVert,
                                       pEndWriteCompactDataBlock);
                }

                // Construct ".endReadThreadCount" block
                {
                    m_nggFactor.pVertCountInSubgroup = pVertCountInWaves;

                    auto pFirstWaveInSubgroup = new ICmpInst(*pEndReadThreadCountBlock,
                                                             ICmpInst::ICMP_EQ,
                                                             m_nggFactor.pWaveIdInSubgroup,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                             "");

                    BranchInst::Create(pAllocReqBlock,
                                       pEndAllocReqBlock,
                                       pFirstWaveInSubgroup,
                                       pEndReadThreadCountBlock);
                }
            }
            else
            {
                // Construct ".readThreadCount" block
                Value* pPrimCountInWaves = nullptr;
                {
                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPrimCountInWaves);

                    // The DWORD following DWORDs for all waves stores the primitive count of the entire sub-group
                    auto pLdsOffset = ConstantInt::get(m_pContext->Int32Ty(),
                                                       regionStart + waveCountInSubgroup * SizeOfDword);
                    pPrimCountInWaves =
                        m_pLdsManager->ReadValueFromLds(m_pContext->Int32Ty(), pLdsOffset, pReadThreadCountBlock);

                    BranchInst::Create(pEndReadThreadCountBlock, pReadThreadCountBlock);
                }

                // Construct ".endReadThreadCount" block
                {
                    Value* pPrimCount = PHINode::Create(m_pContext->Int32Ty(), 2, "", pEndReadThreadCountBlock);
                    static_cast<PHINode*>(pPrimCount)->addIncoming(m_nggFactor.pPrimCountInSubgroup,
                                                                   pEndAccThreadCountBlock);
                    static_cast<PHINode*>(pPrimCount)->addIncoming(pPrimCountInWaves, pReadThreadCountBlock);

                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);
                    attribs.push_back(Attribute::ReadOnly);

                    // NOTE: We promote primitive count in waves to SGPR since it is treated as an uniform value.
                    args.clear();
                    args.push_back(pPrimCount);

                    pPrimCount = EmitCall(pModule,
                                          "llvm.amdgcn.readfirstlane",
                                          m_pContext->Int32Ty(),
                                          args,
                                          attribs,
                                          pEndReadThreadCountBlock);

                    Value* pHasSurvivePrim = new ICmpInst(*pEndReadThreadCountBlock,
                                                          ICmpInst::ICMP_NE,
                                                          pPrimCount,
                                                          ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                          "");

                    Value* pPrimCountInSubgroup = SelectInst::Create(pHasSurvivePrim,
                                                                     m_nggFactor.pPrimCountInSubgroup,
                                                                     ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                                     "",
                                                                     pEndReadThreadCountBlock);

                    // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
                    // as an uniform value later. This is similar to the provided primitive count in sub-group that is
                    // a system value.
                    args.clear();
                    args.push_back(pPrimCountInSubgroup);

                    pPrimCountInSubgroup = EmitCall(pModule,
                                                    "llvm.amdgcn.readfirstlane",
                                                    m_pContext->Int32Ty(),
                                                    args,
                                                    attribs,
                                                    pEndReadThreadCountBlock);

                    pHasSurvivePrim = new ICmpInst(*pEndReadThreadCountBlock,
                                                   ICmpInst::ICMP_NE,
                                                   pPrimCountInSubgroup,
                                                   ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                   "");

                    Value* pVertCountInSubgroup = SelectInst::Create(pHasSurvivePrim,
                                                                     m_nggFactor.pVertCountInSubgroup,
                                                                     ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                                     "",
                                                                     pEndReadThreadCountBlock);

                    // NOTE: Here, we have to promote revised vertex count in sub-group to SGPR since it is treated as
                    // an uniform value later, similar to what we have done for the revised primitive count in
                    // sub-group.
                    args.clear();
                    args.push_back(pVertCountInSubgroup);

                    pVertCountInSubgroup = EmitCall(pModule,
                                                    "llvm.amdgcn.readfirstlane",
                                                    m_pContext->Int32Ty(),
                                                    args,
                                                    attribs,
                                                    pEndReadThreadCountBlock);

                    m_nggFactor.pPrimCountInSubgroup = pPrimCountInSubgroup;
                    m_nggFactor.pVertCountInSubgroup = pVertCountInSubgroup;

                    auto pFirstWaveInSubgroup = new ICmpInst(*pEndReadThreadCountBlock,
                                                             ICmpInst::ICMP_EQ,
                                                             m_nggFactor.pWaveIdInSubgroup,
                                                             ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                             "");

                    BranchInst::Create(pAllocReqBlock,
                                       pEndAllocReqBlock,
                                       pFirstWaveInSubgroup,
                                       pEndReadThreadCountBlock);
                }
            }

            // Construct ".allocReq" block
            {
                DoParamCacheAllocRequest(pModule, pAllocReqBlock);
                BranchInst::Create(pEndAllocReqBlock, pAllocReqBlock);
            }

            // Construct ".endAllocReq" block
            {
                if (vertexCompact)
                {
                    args.clear();
                    attribs.clear();
                    attribs.push_back(Attribute::NoRecurse);

                    EmitCall(pModule,
                             "llvm.amdgcn.s.barrier",
                             m_pContext->VoidTy(),
                             args,
                             attribs,
                             pEndAllocReqBlock);
                }

                auto pPrimExp = new ICmpInst(*pEndAllocReqBlock,
                                             ICmpInst::ICMP_ULT,
                                             m_nggFactor.pThreadIdInSubgroup,
                                             m_nggFactor.pPrimCountInSubgroup,
                                             "");
                BranchInst::Create(pExpPrimBlock, pEndExpPrimBlock, pPrimExp, pEndAllocReqBlock);
            }

            // Construct ".expPrim" block
            {
                DoPrimitiveExport(pModule, (vertexCompact ? pCullFlag : nullptr), pExpPrimBlock);
                BranchInst::Create(pEndExpPrimBlock, pExpPrimBlock);
            }

            // Construct ".endExpPrim" block
            Value* pVertExp = nullptr;
            {
                pVertExp = new ICmpInst(*pEndExpPrimBlock,
                                        ICmpInst::ICMP_ULT,
                                        m_nggFactor.pThreadIdInSubgroup,
                                        m_nggFactor.pVertCountInSubgroup,
                                        "");
                BranchInst::Create(pExpVertPosBlock, pEndExpVertPosBlock, pVertExp, pEndExpPrimBlock);
            }

            // Construct ".expVertPos" block
            {
                // NOTE: For vertex compaction, we have to run ES to get exported data once again.
                if (vertexCompact)
                {
                    expDataSet.clear();

                    RunEsOrEsVariant(pModule,
                                     LlpcName::NggEsEntryVariantExp,
                                     pEntryPoint->arg_begin(),
                                     true,
                                     &expDataSet,
                                     pExpVertPosBlock);

                    // For vertex position, we get the exported data from LDS
                    for (auto& expData : expDataSet)
                    {
                        if (expData.target == EXP_TARGET_POS_0)
                        {
                            const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);

                            auto pLdsOffset =
                                BinaryOperator::CreateMul(m_nggFactor.pThreadIdInSubgroup,
                                                          ConstantInt::get(m_pContext->Int32Ty(), SizeOfVec4),
                                                          "",
                                                          pExpVertPosBlock);
                            pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                                                   ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                                                   "",
                                                                   pExpVertPosBlock);
                            auto pExpValue =
                                m_pLdsManager->ReadValueFromLds(m_pContext->Floatx4Ty(), pLdsOffset, pExpVertPosBlock);
                            expData.pExpValue = pExpValue;

                            break;
                        }
                    }
                }

                for (const auto& expData : expDataSet)
                {
                    if ((expData.target >= EXP_TARGET_POS_0) && (expData.target <= EXP_TARGET_POS_4))
                    {
                        args.clear();
                        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), expData.target));        // tgt
                        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), expData.channelMask));   // en

                        // src0 ~ src3
                        for (uint32_t i = 0; i < 4; ++i)
                        {
                            auto pExpValue = ExtractElementInst::Create(expData.pExpValue,
                                                                        ConstantInt::get(m_pContext->Int32Ty(), i),
                                                                        "",
                                                                        pExpVertPosBlock);
                            args.push_back(pExpValue);
                        }

                        args.push_back(ConstantInt::get(m_pContext->BoolTy(), expData.doneFlag));       // done
                        args.push_back(ConstantInt::get(m_pContext->BoolTy(), false));                  // vm

                        EmitCall(pModule,
                                 "llvm.amdgcn.exp.f32",
                                 m_pContext->VoidTy(),
                                 args,
                                 NoAttrib,
                                 pExpVertPosBlock);
                    }
                }

                BranchInst::Create(pEndExpVertPosBlock, pExpVertPosBlock);
            }

            // Construct ".endExpVertPos" block
            {
                if (vertexCompact)
                {
                    auto pUndef = UndefValue::get(m_pContext->Floatx4Ty());
                    for (auto& expData : expDataSet)
                    {
                        PHINode* pExpValue = PHINode::Create(m_pContext->Floatx4Ty(), 2, "", pEndExpVertPosBlock);
                        pExpValue->addIncoming(expData.pExpValue, pExpVertPosBlock);
                        pExpValue->addIncoming(pUndef, pEndExpPrimBlock);

                        expData.pExpValue = pExpValue; // Update the exportd data
                    }
                }

                BranchInst::Create(pExpVertParamBlock, pEndExpVertParamBlock, pVertExp, pEndExpVertPosBlock);
            }

            // Construct ".expVertParam" block
            {
                // NOTE: For vertex compaction, ES must have been run in ".expVertPos" block.
                if (vertexCompact == false)
                {
                    if (separateExp)
                    {
                        // Should run ES variant to get exported parameter data
                        expDataSet.clear();

                        RunEsOrEsVariant(pModule,
                                         LlpcName::NggEsEntryVariantExpParam,
                                         pEntryPoint->arg_begin(),
                                         false,
                                         &expDataSet,
                                         pExpVertParamBlock);
                    }
                }

                for (const auto& expData : expDataSet)
                {
                    if ((expData.target >= EXP_TARGET_PARAM_0) && (expData.target <= EXP_TARGET_PARAM_31))
                    {
                        args.clear();
                        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), expData.target));        // tgt
                        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), expData.channelMask));   // en

                        // src0 ~ src3
                        for (uint32_t i = 0; i < 4; ++i)
                        {
                            auto pExpValue = ExtractElementInst::Create(expData.pExpValue,
                                                                        ConstantInt::get(m_pContext->Int32Ty(), i),
                                                                        "",
                                                                        pExpVertParamBlock);
                            args.push_back(pExpValue);
                        }

                        args.push_back(ConstantInt::get(m_pContext->BoolTy(), expData.doneFlag));       // done
                        args.push_back(ConstantInt::get(m_pContext->BoolTy(), false));                  // vm

                        EmitCall(pModule,
                                 "llvm.amdgcn.exp.f32",
                                 m_pContext->VoidTy(),
                                 args,
                                 NoAttrib,
                                 pExpVertParamBlock);
                    }
                }

                BranchInst::Create(pEndExpVertParamBlock, pExpVertParamBlock);
            }

            // Construct ".endExpVertParam" block
            {
                ReturnInst::Create(*m_pContext, pEndExpVertParamBlock);
            }
        }
    }

    return pEntryPoint;
}

// =====================================================================================================================
// Does various culling for NGG primitive shader.
Value* NggPrimShader::DoCulling(
    Module*       pModule,      // [in] LLVM module
    BasicBlock*   pInsertAtEnd) // [in] Where to insert instructions
{
    Value* pCullFlag = ConstantInt::get(m_pContext->BoolTy(), false);

    // Skip culling if it is not requested
    if (EnableCulling() == false)
    {
        return pCullFlag;
    }

    std::vector<Value*> args;
    std::vector<Attribute::AttrKind> attribs;

    attribs.push_back(Attribute::ReadNone);

    args.clear();
    args.push_back(m_nggFactor.pEsGsOffsets01);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

    auto pEsGsOffset0 =
        EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pInsertAtEnd);
    auto pVertexId0 = BinaryOperator::CreateLShr(pEsGsOffset0,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                 "",
                                                 pInsertAtEnd);

    args.clear();
    args.push_back(m_nggFactor.pEsGsOffsets01);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

    auto pEsGsOffset1 =
        EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pInsertAtEnd);
    auto pVertexId1 = BinaryOperator::CreateLShr(pEsGsOffset1,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                 "",
                                                 pInsertAtEnd);

    args.clear();
    args.push_back(m_nggFactor.pEsGsOffsets23);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

    auto pEsGsOffset2 =
        EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pInsertAtEnd);
    auto pVertexId2 = BinaryOperator::CreateLShr(pEsGsOffset2,
                                                 ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                 "",
                                                 pInsertAtEnd);

    Value* vertexId[3] = { pVertexId0, pVertexId1, pVertexId2 };
    Value* vertex[3] = { nullptr };

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);
    auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

    for (uint32_t i = 0; i < 3; ++i)
    {
        Value* pLdsOffset = BinaryOperator::CreateMul(vertexId[i],
                                                      ConstantInt::get(m_pContext->Int32Ty(), SizeOfVec4),
                                                      "",
                                                      pInsertAtEnd);
        pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pRegionStart, "", pInsertAtEnd);

        vertex[i] = m_pLdsManager->ReadValueFromLds(m_pContext->Floatx4Ty(), pLdsOffset, pInsertAtEnd);
    }

    // Handle backface culling
    if (m_pNggControl->enableBackfaceCulling)
    {
        pCullFlag = DoBackfaceCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2], pInsertAtEnd);
    }

    // Handle frustum culling
    if (m_pNggControl->enableFrustumCulling)
    {
        pCullFlag = DoFrustumCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2], pInsertAtEnd);
    }

    // Handle box filter culling
    if (m_pNggControl->enableBoxFilterCulling)
    {
        pCullFlag = DoBoxFilterCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2], pInsertAtEnd);
    }

    // Handle sphere culling
    if (m_pNggControl->enableSphereCulling)
    {
        pCullFlag = DoSphereCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2], pInsertAtEnd);
    }

    // Handle small primitive filter culling
    if (m_pNggControl->enableSmallPrimFilter)
    {
        pCullFlag = DoSmallPrimFilterCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2], pInsertAtEnd);
    }

    // Handle cull distance culling
    if (m_pNggControl->enableCullDistanceCulling)
    {
        Value* signMask[3] = { nullptr };

        const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionCullDistance);
        auto pRegionStart = ConstantInt::get(m_pContext->Int32Ty(), regionStart);

        for (uint32_t i = 0; i < 3; ++i)
        {
            Value* pLdsOffset = BinaryOperator::CreateMul(vertex[i],
                                                          ConstantInt::get(m_pContext->Int32Ty(), SizeOfDword),
                                                          "",
                                                          pInsertAtEnd);
            pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset, pRegionStart, "", pInsertAtEnd);

            signMask[i] = m_pLdsManager->ReadValueFromLds(m_pContext->Int32Ty(), pLdsOffset, pInsertAtEnd);
        }

        pCullFlag = DoCullDistanceCulling(pModule, pCullFlag, signMask[0], signMask[1], signMask[2], pInsertAtEnd);
    }

    return pCullFlag;
}

// =====================================================================================================================
// Requests that parameter cache space be allocated (send the message GS_ALLOC_REQ).
void NggPrimShader::DoParamCacheAllocRequest(
    Module*      pModule,       // [in] LLVM module
    BasicBlock*  pInsertAtEnd)  // [in] Where to insert instructions
{
    std::vector<Value*> args;

    // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
    Value* pM0 = BinaryOperator::CreateShl(m_nggFactor.pPrimCountInSubgroup,
                                           ConstantInt::get(m_pContext->Int32Ty(), 12),
                                           "",
                                           pInsertAtEnd);

    pM0 = BinaryOperator::CreateOr(pM0, m_nggFactor.pVertCountInSubgroup, "", pInsertAtEnd);

    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), GS_ALLOC_REQ));
    args.push_back(pM0);

    EmitCall(pModule, "llvm.amdgcn.s.sendmsg", m_pContext->VoidTy(), args, NoAttrib, pInsertAtEnd);
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader.
void NggPrimShader::DoPrimitiveExport(
    Module*       pModule,          // [in] LLVM module
    Value*        pCullFlag,        // [in] Cull flag indicating whether this primitive has been culled (could be null)
    BasicBlock*   pInsertAtEnd)     // [in] Where to insert instructions
{
    const bool vertexCompact = (m_pNggControl->compactMode == NggCompactVertices);

    std::vector<Value*> args;
    std::vector<Attribute::AttrKind> attribs;

    Value* pPrimData = nullptr;

    // Primitive data layout [31:0]
    //   [31]    = null primitive flag
    //   [28:20] = vertexId2 (in bytes)
    //   [18:10] = vertexId1 (in bytes)
    //   [8:0]   = vertexId0 (in bytes)

    if (m_pNggControl->passthroughMode)
    {
        // Pass-through mode (primitive data has been constructed)
        pPrimData = m_nggFactor.pEsGsOffsets01;
    }
    else
    {
        // Non pass-through mode (primitive data has to be constructed)
        attribs.clear();
        attribs.push_back(Attribute::ReadNone);

        args.clear();
        args.push_back(m_nggFactor.pEsGsOffsets01);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset0 =
            EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pInsertAtEnd);
        Value* pVertexId0 = BinaryOperator::CreateLShr(pEsGsOffset0,
                                                       ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                       "",
                                                       pInsertAtEnd);

        args.clear();
        args.push_back(m_nggFactor.pEsGsOffsets01);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset1 =
            EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pInsertAtEnd);
        Value* pVertexId1 = BinaryOperator::CreateLShr(pEsGsOffset1,
                                                       ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                       "",
                                                       pInsertAtEnd);

        args.clear();
        args.push_back(m_nggFactor.pEsGsOffsets23);
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 16));

        auto pEsGsOffset2 =
            EmitCall(pModule, "llvm.amdgcn.ubfe.i32", m_pContext->Int32Ty(), args, attribs, pInsertAtEnd);
        Value* pVertexId2 = BinaryOperator::CreateLShr(pEsGsOffset2,
                                                       ConstantInt::get(m_pContext->Int32Ty(), 2),
                                                       "",
                                                       pInsertAtEnd);

        if (vertexCompact)
        {
            pVertexId0 = ReadCompactDataFromLds(m_pContext->Int8Ty(),
                                                pVertexId0,
                                                LdsRegionCompactThreadIdInSubgroup,
                                                pInsertAtEnd);
            pVertexId0 = new ZExtInst(pVertexId0, m_pContext->Int32Ty(), "", pInsertAtEnd);

            pVertexId1 = ReadCompactDataFromLds(m_pContext->Int8Ty(),
                                                pVertexId1,
                                                LdsRegionCompactThreadIdInSubgroup,
                                                pInsertAtEnd);
            pVertexId1 = new ZExtInst(pVertexId1, m_pContext->Int32Ty(), "", pInsertAtEnd);

            pVertexId2 = ReadCompactDataFromLds(m_pContext->Int8Ty(),
                                                pVertexId2,
                                                LdsRegionCompactThreadIdInSubgroup,
                                                pInsertAtEnd);
            pVertexId2 = new ZExtInst(pVertexId2, m_pContext->Int32Ty(), "", pInsertAtEnd);
        }

        pPrimData = BinaryOperator::CreateShl(pVertexId2,
                                              ConstantInt::get(m_pContext->Int32Ty(), 10),
                                              "",
                                              pInsertAtEnd);
        pPrimData = BinaryOperator::CreateOr(pPrimData, pVertexId1, "", pInsertAtEnd);

        pPrimData = BinaryOperator::CreateShl(pPrimData,
                                              ConstantInt::get(m_pContext->Int32Ty(), 10),
                                              "",
                                              pInsertAtEnd);
        pPrimData = BinaryOperator::CreateOr(pPrimData, pVertexId0, "", pInsertAtEnd);

        if (vertexCompact)
        {
            LLPC_ASSERT(pCullFlag != nullptr); // Must not be null
            const auto pNullPrim = ConstantInt::get(m_pContext->Int32Ty(), (1u << 31));
            pPrimData = SelectInst::Create(pCullFlag, pNullPrim, pPrimData, "", pInsertAtEnd);
        }
    }

    auto pUndef = UndefValue::get(m_pContext->Int32Ty());

    args.clear();
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), EXP_TARGET_PRIM));  // tgt
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0x1));              // en

    // src0 ~ src3
    args.push_back(pPrimData);
    args.push_back(pUndef);
    args.push_back(pUndef);
    args.push_back(pUndef);

    args.push_back(ConstantInt::get(m_pContext->BoolTy(), true));   // done, must be set
    args.push_back(ConstantInt::get(m_pContext->BoolTy(), false));  // vm

    EmitCall(pModule, "llvm.amdgcn.exp.i32", m_pContext->VoidTy(), args, NoAttrib, pInsertAtEnd);
}

// =====================================================================================================================
// Constructs basic blocks to do dummy primitive/vertex export in NGG primitive shader when we detect that all vertices
// in the sub-group are culled.
//
// Returns the entry block doing dummy export.
BasicBlock* NggPrimShader::ConstructDummyExport(
    Module*   pModule,      // [in] LLVM module
    Function* pEntryPoint)  // [in] Shader entry-point
{
    LLPC_ASSERT(m_pNggControl->compactMode == NggCompactVertices);

    auto pEndDummyExpPrimBlock = BasicBlock::Create(*m_pContext, ".endDummyExpPrim", pEntryPoint);
    auto pDummyExpPrimBlock = BasicBlock::Create(*m_pContext, ".dummyExpPrim", pEntryPoint, pEndDummyExpPrimBlock);
    auto pEndDummyAllocReqBlock = BasicBlock::Create(*m_pContext, ".endDummyAllocReq", pEntryPoint, pDummyExpPrimBlock);
    auto pDummyAllocReqBlock = BasicBlock::Create(*m_pContext, ".dummyAllocReq", pEntryPoint, pEndDummyAllocReqBlock);

    std::vector<Value*> args;

    // Construct ".dummyAllocReq" block
    {
        // M0[10:0] = vertCntInSubgroup = 1, M0[22:12] = primCntInSubgroup = 1
        union PrimData
        {
            struct
            {
                uint32_t vertCount : 11;
                uint32_t           : 1;
                uint32_t primCount : 11;
                uint32_t           : 9;
            } bits;

            uint32_t u32All;

        } primData;

        primData.bits.vertCount = 1;
        primData.bits.primCount = 1;

        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), GS_ALLOC_REQ));
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), primData.u32All));

        EmitCall(pModule, "llvm.amdgcn.s.sendmsg", m_pContext->VoidTy(), args, NoAttrib, pDummyAllocReqBlock);

        BranchInst::Create(pEndDummyAllocReqBlock, pDummyAllocReqBlock);
    }

    // Construct ".endDummyAllocReq" block
    {
        auto pFirstThreadInSubgroup = new ICmpInst(*pEndDummyAllocReqBlock,
                                                   ICmpInst::ICMP_EQ,
                                                   m_nggFactor.pThreadIdInSubgroup,
                                                   ConstantInt::get(m_pContext->Int32Ty(), 0),
                                                   "");
        BranchInst::Create(pDummyExpPrimBlock, pEndDummyExpPrimBlock, pFirstThreadInSubgroup, pEndDummyAllocReqBlock);
    }

    // Construct ".dummyExpPrim" block
    {
        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), EXP_TARGET_POS_0));      // tgt
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0x0));                   // en

        // src0 ~ src3
        auto pUndef = UndefValue::get(m_pContext->FloatTy());
        args.push_back(pUndef);
        args.push_back(pUndef);
        args.push_back(pUndef);
        args.push_back(pUndef);

        args.push_back(ConstantInt::get(m_pContext->BoolTy(), true));                   // done
        args.push_back(ConstantInt::get(m_pContext->BoolTy(), false));                  // vm

        EmitCall(pModule, "llvm.amdgcn.exp.f32", m_pContext->VoidTy(), args, NoAttrib, pDummyExpPrimBlock);

        args.clear();
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), EXP_TARGET_PRIM));       // tgt
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0x1));                   // en

        // src0 ~ src3
        pUndef = UndefValue::get(m_pContext->Int32Ty());
        args.push_back(ConstantInt::get(m_pContext->Int32Ty(), 0));
        args.push_back(pUndef);
        args.push_back(pUndef);
        args.push_back(pUndef);

        args.push_back(ConstantInt::get(m_pContext->BoolTy(), true));                   // done
        args.push_back(ConstantInt::get(m_pContext->BoolTy(), false));                  // vm

        EmitCall(pModule, "llvm.amdgcn.exp.i32", m_pContext->VoidTy(), args, NoAttrib, pDummyExpPrimBlock);

        BranchInst::Create(pEndDummyExpPrimBlock, pDummyExpPrimBlock);
    }

    // Construct ".endDummyExpPrim" block
    {
        ReturnInst::Create(*m_pContext, pEndDummyExpPrimBlock);
    }

    return pDummyAllocReqBlock;
}

// =====================================================================================================================
// Runs ES or ES variant (to get exported data).
void NggPrimShader::RunEsOrEsVariant(
    Module*               pModule,          // [in] LLVM module
    StringRef             entryName,        // ES entry name
    Argument*             pSysValueStart,   // Start of system value
    bool                  sysValueFromLds,  // Whether some system values are loaded from LDS (for vertex compaction)
    std::vector<ExpData>* pExpDataSet,      // [out] Set of exported data (could be null)
    BasicBlock*           pInsertAtEnd)     // [in] Where to insert instructions
{
    LLPC_ASSERT(m_hasGs == false); // GS must not be present

    const bool hasTs = (m_hasTcs || m_hasTes);
    if (((hasTs && m_hasTes) || ((hasTs == false) && m_hasVs)) == false)
    {
        // No TES (tessellation is enabled) or VS (tessellation is disabled), don't have to run
        return;
    }

    const bool runEsVariant = (entryName != LlpcName::NggEsEntryPoint);

    Function* pEsEntry = nullptr;
    if (runEsVariant)
    {
        LLPC_ASSERT(pExpDataSet != nullptr);
        pEsEntry = MutateEsToVariant(pModule, entryName, *pExpDataSet); // Mutate ES to variant

        if (pEsEntry == nullptr)
        {
            // ES variant is NULL, don't have to run
            return;
        }
    }
    else
    {
        pEsEntry = pModule->getFunction(LlpcName::NggEsEntryPoint);
        LLPC_ASSERT(pEsEntry != nullptr);
    }

    // Call ES entry
    Argument* pArg = pSysValueStart;

    Value* pOffChipLdsBase = (pArg + EsGsSysValueOffChipLdsBase);
    pArg += EsGsSpecialSysValueCount;

    Value* pUserData = pArg++;

    // Initialize those system values to undefined ones
    Value* pTessCoordX    = UndefValue::get(m_pContext->FloatTy());
    Value* pTessCoordY    = UndefValue::get(m_pContext->FloatTy());
    Value* pRelPatchId    = UndefValue::get(m_pContext->Int32Ty());
    Value* pPatchId       = UndefValue::get(m_pContext->Int32Ty());

    Value* pVertexId      = UndefValue::get(m_pContext->Int32Ty());
    Value* pRelVertexId   = UndefValue::get(m_pContext->Int32Ty());
    Value* pVsPrimitiveId = UndefValue::get(m_pContext->Int32Ty());
    Value* pInstanceId    = UndefValue::get(m_pContext->Int32Ty());

    if (sysValueFromLds)
    {
        // NOTE: For vertex compaction, system values are from LDS compaction data region rather than from VGPRs.
        LLPC_ASSERT(m_pNggControl->compactMode == NggCompactVertices);

        const auto pResUsage = m_pContext->GetShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

        if (hasTs)
        {
            if (pResUsage->builtInUsage.tes.tessCoord)
            {
                pTessCoordX = ReadCompactDataFromLds(m_pContext->FloatTy(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactTessCoordX,
                                                     pInsertAtEnd);

                pTessCoordY = ReadCompactDataFromLds(m_pContext->FloatTy(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactTessCoordY,
                                                     pInsertAtEnd);
            }

            pRelPatchId = ReadCompactDataFromLds(m_pContext->Int32Ty(),
                                                 m_nggFactor.pThreadIdInSubgroup,
                                                 LdsRegionCompactRelPatchId,
                                                 pInsertAtEnd);

            if (pResUsage->builtInUsage.tes.primitiveId)
            {
                pPatchId = ReadCompactDataFromLds(m_pContext->Int32Ty(),
                                                  m_nggFactor.pThreadIdInSubgroup,
                                                  LdsRegionCompactPatchId,
                                                  pInsertAtEnd);
            }
        }
        else
        {
            if (pResUsage->builtInUsage.vs.vertexIndex)
            {
                pVertexId = ReadCompactDataFromLds(m_pContext->Int32Ty(),
                                                   m_nggFactor.pThreadIdInSubgroup,
                                                   LdsRegionCompactVertexId,
                                                   pInsertAtEnd);
            }

            // NOTE: Relative vertex ID Will not be used when VS is merged to GS.

            if (pResUsage->builtInUsage.vs.primitiveId)
            {
                pVsPrimitiveId = ReadCompactDataFromLds(m_pContext->Int32Ty(),
                                                        m_nggFactor.pThreadIdInSubgroup,
                                                        LdsRegionCompactPrimId,
                                                        pInsertAtEnd);
            }

            if (pResUsage->builtInUsage.vs.instanceIndex)
            {
                pInstanceId = ReadCompactDataFromLds(m_pContext->Int32Ty(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactInstanceId,
                                                     pInsertAtEnd);
            }
        }
    }
    else
    {
        pTessCoordX    = (pArg + 5);
        pTessCoordY    = (pArg + 6);
        pRelPatchId    = (pArg + 7);
        pPatchId       = (pArg + 8);

        pVertexId      = (pArg + 5);
        pRelVertexId   = (pArg + 6);
        // NOTE: VS primitive ID for NGG is specially obtained, not simply from system VGPR.
        if (m_nggFactor.pPrimitiveId != nullptr)
        {
            pVsPrimitiveId = m_nggFactor.pPrimitiveId;
        }
        pInstanceId    = (pArg + 8);
    }

    std::vector<Value*> args;

    auto pIntfData =
        m_pContext->GetShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    const uint32_t userDataCount = pIntfData->userDataCount;

    uint32_t userDataIdx = 0;

    auto pEsArgBegin = pEsEntry->arg_begin();
    const uint32_t esArgCount = pEsEntry->arg_size();

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

            auto pEsUserData = new ShuffleVectorInst(pUserData,
                                                        pUserData,
                                                        ConstantVector::get(shuffleMask),
                                                        "",
                                                        pInsertAtEnd);
            args.push_back(pEsUserData);
        }
        else
        {
            LLPC_ASSERT(pEsArgTy->isIntegerTy());

            auto pEsUserData =
                ExtractElementInst::Create(pUserData,
                                            ConstantInt::get(m_pContext->Int32Ty(), userDataIdx),
                                            "",
                                            pInsertAtEnd);
            args.push_back(pEsUserData);
            ++userDataIdx;
        }

        ++esArgIdx;
    }

    if (hasTs)
    {
        // Set up system value SGPRs
        if (m_pContext->IsTessOffChip())
        {
            args.push_back(pOffChipLdsBase);
            ++esArgIdx;

            args.push_back(pOffChipLdsBase);
            ++esArgIdx;
        }

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

    if (runEsVariant)
    {
        auto pExpData = EmitCall(pModule,
                                 entryName,
                                 pEsEntry->getReturnType(),
                                 args,
                                 NoAttrib,
                                 pInsertAtEnd);

        // Re-construct exported data from the return value
        auto pExpDataTy = pExpData->getType();
        LLPC_ASSERT(pExpDataTy->isArrayTy());

        const uint32_t expCount = pExpDataTy->getArrayNumElements();
        for (uint32_t i = 0; i < expCount; ++i)
        {
            Value* pExpValue = ExtractValueInst::Create(pExpData, { i }, "", pInsertAtEnd);
            (*pExpDataSet)[i].pExpValue = pExpValue;
        }
    }
    else
    {
        EmitCall(pModule,
                 entryName,
                 pEsEntry->getReturnType(),
                 args,
                 NoAttrib,
                 pInsertAtEnd);
    }
}

// =====================================================================================================================
// Mutates the entry-point (".main") of ES to its variant (".exp").
//
// NOTE: Initially, the return type of ES entry-point is void. After this mutation, position and parameter exporting
// are both removed. Instead, the exported values are returned via either a new entry-point (combined) or two new
// entry-points (separate). Return types is something like this:
//   .exp:       [ POS0: <4 x float>, POS1: <4 x float>, ..., PARAM0: <4 x float>, PARAM1: <4 x float>, ... ]
//   .exp.pos:   [ POS0: <4 x float>, POS1: <4 x float>, ... ]
//   .exp.param: [ PARAM0: <4 x float>, PARAM1: <4 x float>, ... ]
Function* NggPrimShader::MutateEsToVariant(
    Module*               pModule,          // [in] LLVM module
    StringRef             entryName,        // ES entry name
    std::vector<ExpData>& expDataSet)       // [out] Set of exported data
{
    LLPC_ASSERT(m_hasGs == false); // GS must not be present
    LLPC_ASSERT(expDataSet.empty());

    const auto pEsEntryPoint = pModule->getFunction(LlpcName::NggEsEntryPoint);
    LLPC_ASSERT(pEsEntryPoint != nullptr);

    const bool doExp      = (entryName == LlpcName::NggEsEntryVariantExp);
    const bool doPosExp   = (entryName == LlpcName::NggEsEntryVariantExpPos);
    const bool doParamExp = (entryName == LlpcName::NggEsEntryVariantExpParam);

    // Calculate export count
    uint32_t expCount = 0;

    for (auto& inst : pEsEntryPoint->back())
    {
        if (isa<CallInst>(&inst))
        {
            auto pCall = cast<CallInst>(&inst);
            auto pCallee = pCall->getCalledFunction();
            auto calleeName = pCallee->getName();

            if (calleeName.startswith("llvm.amdgcn.exp."))
            {
                uint8_t expTarget = cast<ConstantInt>(pCall->getArgOperand(0))->getZExtValue();

                bool expPos = ((expTarget >= EXP_TARGET_POS_0) && (expTarget <= EXP_TARGET_POS_4));
                bool expParam = ((expTarget >= EXP_TARGET_PARAM_0) && (expTarget <= EXP_TARGET_PARAM_31));

                if ((doExp && (expPos || expParam)) ||
                    (doPosExp && expPos)            ||
                    (doParamExp && expParam))
                {
                    ++expCount;
                }
            }
        }
    }

    if (expCount == 0)
    {
        // If the export count is zero, return NULL
        return nullptr;
    }

    // Clone new entry-point
    auto pExpDataTy = ArrayType::get(m_pContext->Floatx4Ty(), expCount);
    Value* pExpData = UndefValue::get(pExpDataTy);

    auto pEsEntryVariantTy = FunctionType::get(pExpDataTy, pEsEntryPoint->getFunctionType()->params(), false);
    auto pEsEntryVariant = Function::Create(pEsEntryVariantTy,
                                            pEsEntryPoint->getLinkage(),
                                            entryName,
                                            pModule);
    pEsEntryVariant->copyAttributesFrom(pEsEntryPoint);

    ValueToValueMapTy valueMap;

    Argument* pVariantArg = pEsEntryVariant->arg_begin();
    for (Argument &arg : pEsEntryPoint->args())
    {
        valueMap[&arg] = pVariantArg++;
    }

    SmallVector<ReturnInst*, 8> retInsts;
    CloneFunctionInto(pEsEntryVariant, pEsEntryPoint, valueMap, false, retInsts);

    // Remove old "return" instruction
    BasicBlock* pRetBlock = &pEsEntryVariant->back();

    LLPC_ASSERT(isa<ReturnInst>(pEsEntryVariant->back().getTerminator()));
    ReturnInst* pRetInst = cast<ReturnInst>(pEsEntryVariant->back().getTerminator());

    pRetInst->dropAllReferences();
    pRetInst->eraseFromParent();

    // Get exported data
    std::vector<Instruction*> expCalls;

    for (auto& inst : *pRetBlock)
    {
        if (isa<CallInst>(&inst))
        {
            auto pCall = cast<CallInst>(&inst);
            auto pCallee = pCall->getCalledFunction();
            auto calleeName = pCallee->getName();

            if (calleeName.startswith("llvm.amdgcn.exp."))
            {
                uint8_t expTarget = cast<ConstantInt>(pCall->getArgOperand(0))->getZExtValue();

                bool expPos = ((expTarget >= EXP_TARGET_POS_0) && (expTarget <= EXP_TARGET_POS_4));
                bool expParam = ((expTarget >= EXP_TARGET_PARAM_0) && (expTarget <= EXP_TARGET_PARAM_31));

                if ((doExp && (expPos || expParam)) ||
                    (doPosExp && expPos)            ||
                    (doParamExp && expParam))
                {
                    uint8_t channelMask = cast<ConstantInt>(pCall->getArgOperand(1))->getZExtValue();

                    Value* expValue[4] = {};
                    expValue[0] = pCall->getArgOperand(2);
                    expValue[1] = pCall->getArgOperand(3);
                    expValue[2] = pCall->getArgOperand(4);
                    expValue[3] = pCall->getArgOperand(5);

                    if (calleeName.endswith(".i32"))
                    {
                        expValue[0] = new BitCastInst(expValue[0], m_pContext->FloatTy(), "", pRetBlock);
                        expValue[1] = new BitCastInst(expValue[1], m_pContext->FloatTy(), "", pRetBlock);
                        expValue[2] = new BitCastInst(expValue[2], m_pContext->FloatTy(), "", pRetBlock);
                        expValue[3] = new BitCastInst(expValue[3], m_pContext->FloatTy(), "", pRetBlock);
                    }

                    Value* pExpValue = UndefValue::get(m_pContext->Floatx4Ty());
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        pExpValue = InsertElementInst::Create(pExpValue,
                                                              expValue[i],
                                                              ConstantInt::get(m_pContext->Int32Ty(), i),
                                                              "",
                                                              pRetBlock);
                    }

                    bool doneFlag = (cast<ConstantInt>(pCall->getArgOperand(6))->getZExtValue() != 0);

                    ExpData expData = { expTarget, channelMask, doneFlag, pExpValue };
                    expDataSet.push_back(expData);
                }

                expCalls.push_back(pCall);
            }
        }
    }
    LLPC_ASSERT(expDataSet.size() == expCount);

    // Construct exported data
    uint32_t i = 0;
    for (auto& expData : expDataSet)
    {
        pExpData = InsertValueInst::Create(pExpData, expData.pExpValue, { i++ }, "", pRetBlock);
        expData.pExpValue = nullptr;
    }

    // Insert new "return" instruction
    ReturnInst::Create(*m_pContext, pExpData, pRetBlock);

    // Clear export calls
    for (auto pExpCall : expCalls)
    {
        pExpCall->dropAllReferences();
        pExpCall->eraseFromParent();
    }

    return pEsEntryVariant;
}

// =====================================================================================================================
// Reads the specified data from NGG compaction data region in LDS.
Value* NggPrimShader::ReadCompactDataFromLds(
    Type*             pReadDataTy,  // [in] Data written to LDS
    Value*            pThreadId,    // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType  region,       // NGG compaction data region
    BasicBlock*       pInsertAtEnd) // [in] Where to insert instructions
{
    auto sizeInBytes = pReadDataTy->getPrimitiveSizeInBits() / 8;

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(region);

    Value* pLdsOffset = nullptr;
    if (sizeInBytes > 1)
    {
        pLdsOffset = BinaryOperator::CreateMul(pThreadId,
                                               ConstantInt::get(m_pContext->Int32Ty(), sizeInBytes),
                                               "",
                                               pInsertAtEnd);
    }
    else
    {
        pLdsOffset = pThreadId;
    }
    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                           ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                           "",
                                           pInsertAtEnd);;

    return m_pLdsManager->ReadValueFromLds(pReadDataTy, pLdsOffset, pInsertAtEnd);
}

// =====================================================================================================================
// Writes the specified data to NGG compaction data region in LDS.
void NggPrimShader::WriteCompactDataToLds(
    Value*           pWriteData,        // [in] Data written to LDS
    Value*           pThreadId,         // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType region,            // NGG compaction data region
    BasicBlock*      pInsertAtEnd)      // [in] Where to insert instructions
{
    auto pWriteDataTy = pWriteData->getType();
    auto sizeInBytes = pWriteDataTy->getPrimitiveSizeInBits() / 8;

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(region);

    Value* pLdsOffset = nullptr;
    if (sizeInBytes > 1)
    {
        pLdsOffset = BinaryOperator::CreateMul(pThreadId,
                                               ConstantInt::get(m_pContext->Int32Ty(), sizeInBytes),
                                               "",
                                               pInsertAtEnd);
    }
    else
    {
        pLdsOffset = pThreadId;
    }
    pLdsOffset = BinaryOperator::CreateAdd(pLdsOffset,
                                           ConstantInt::get(m_pContext->Int32Ty(), regionStart),
                                           "",
                                           pInsertAtEnd);

    m_pLdsManager->WriteValueToLds(pWriteData, pLdsOffset, pInsertAtEnd);

}

// =====================================================================================================================
// Backface culler.
Value* NggPrimShader::DoBackfaceCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2,       // [in] Position data of vertex2
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    LLPC_ASSERT(m_pNggControl->enableBackfaceCulling);

    uint32_t regOffset = 0;

    // Get register PA_SU_SC_MODE_CNTL
    Value* pPaSuScModeCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paSuScModeCntl);
        pPaSuScModeCntl = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);
    }
    else
    {
        pPaSuScModeCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                           m_pNggControl->primShaderTable.pipelineStateCb.paSuScModeCntl);
    }

    // Get register PA_CL_VPORT_XSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    auto pPaClVportXscale = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Get register PA_CL_VPORT_YSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    auto pPaClVportYscale = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Do backface culling
    std::vector<Value*> args;
    args.push_back(pCullFlag);
    args.push_back(pVertex0);
    args.push_back(pVertex1);
    args.push_back(pVertex2);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), m_pNggControl->backfaceExponent));
    args.push_back(pPaSuScModeCntl);
    args.push_back(pPaClVportXscale);
    args.push_back(pPaClVportYscale);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingBackface,
                         m_pContext->BoolTy(),
                         args,
                         attribs,
                         pInsertAtEnd);

    return pCullFlag;
}

// =====================================================================================================================
// Frustum culler.
Value* NggPrimShader::DoFrustumCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2,       // [in] Position data of vertex2
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    LLPC_ASSERT(m_pNggControl->enableFrustumCulling);

    uint32_t regOffset = 0;

    // Get register PA_CL_CLIP_CNTL
    Value* pPaClClipCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        pPaClClipCntl = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);
    }
    else
    {
        pPaClClipCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                         m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
    }

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto pPaClGbHorzDiscAdj = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto pPaClGbVertDiscAdj = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Do frustum culling
    std::vector<Value*> args;
    args.push_back(pCullFlag);
    args.push_back(pVertex0);
    args.push_back(pVertex1);
    args.push_back(pVertex2);
    args.push_back(pPaClClipCntl);
    args.push_back(pPaClGbHorzDiscAdj);
    args.push_back(pPaClGbVertDiscAdj);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingFrustum,
                         m_pContext->BoolTy(),
                         args,
                         attribs,
                         pInsertAtEnd);

    return pCullFlag;
}

// =====================================================================================================================
// Box filter culler.
Value* NggPrimShader::DoBoxFilterCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2,       // [in] Position data of vertex2
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    LLPC_ASSERT(m_pNggControl->enableBoxFilterCulling);

    uint32_t regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* pPaClVteCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                           m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_CLIP_CNTL
    Value* pPaClClipCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        pPaClClipCntl = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);
    }
    else
    {
        pPaClClipCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                         m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
    }

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto pPaClGbHorzDiscAdj = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto pPaClGbVertDiscAdj = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Do box filter culling
    std::vector<Value*> args;
    args.push_back(pCullFlag);
    args.push_back(pVertex0);
    args.push_back(pVertex1);
    args.push_back(pVertex2);
    args.push_back(pPaClVteCntl);
    args.push_back(pPaClClipCntl);
    args.push_back(pPaClGbHorzDiscAdj);
    args.push_back(pPaClGbVertDiscAdj);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingBoxFilter,
                         m_pContext->BoolTy(),
                         args,
                         attribs,
                         pInsertAtEnd);

    return pCullFlag;
}

// =====================================================================================================================
// Sphere culler.
Value* NggPrimShader::DoSphereCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2,       // [in] Position data of vertex2
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    LLPC_ASSERT(m_pNggControl->enableSphereCulling);

    uint32_t regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* pPaClVteCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                           m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_CLIP_CNTL
    Value* pPaClClipCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        pPaClClipCntl = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);
    }
    else
    {
        pPaClClipCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                         m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
    }

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto pPaClGbHorzDiscAdj = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto pPaClGbVertDiscAdj = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Do small primitive filter culling
    std::vector<Value*> args;
    args.push_back(pCullFlag);
    args.push_back(pVertex0);
    args.push_back(pVertex1);
    args.push_back(pVertex2);
    args.push_back(pPaClVteCntl);
    args.push_back(pPaClClipCntl);
    args.push_back(pPaClGbHorzDiscAdj);
    args.push_back(pPaClGbVertDiscAdj);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingSphere,
                         m_pContext->BoolTy(),
                         args,
                         attribs,
                         pInsertAtEnd);

    return pCullFlag;
}

// =====================================================================================================================
// Small primitive filter culler.
Value* NggPrimShader::DoSmallPrimFilterCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2,       // [in] Position data of vertex2
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    LLPC_ASSERT(m_pNggControl->enableSmallPrimFilter);

    uint32_t regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* pPaClVteCntl = ConstantInt::get(m_pContext->Int32Ty(),
                                           m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_VPORT_XSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    auto pPaClVportXscale = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Get register PA_CL_VPORT_YSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    auto pPaClVportYscale = FetchCullingControlRegister(pModule, regOffset, pInsertAtEnd);

    // Do small primitive filter culling
    std::vector<Value*> args;
    args.push_back(pCullFlag);
    args.push_back(pVertex0);
    args.push_back(pVertex1);
    args.push_back(pVertex2);
    args.push_back(pPaClVteCntl);
    args.push_back(pPaClVportXscale);
    args.push_back(pPaClVportYscale);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingSmallPrimFilter,
                         m_pContext->BoolTy(),
                         args,
                         attribs,
                         pInsertAtEnd);

    return pCullFlag;
}

// =====================================================================================================================
// Cull distance culler.
Value* NggPrimShader::DoCullDistanceCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pSignMask0,     // [in] Sign mask of cull distance of vertex0
    Value*      pSignMask1,     // [in] Sign mask of cull distance of vertex1
    Value*      pSignMask2,     // [in] Sign mask of cull distance of vertex2
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    LLPC_ASSERT(m_pNggControl->enableCullDistanceCulling);

    // Do cull distance culling
    std::vector<Value*> args;
    args.push_back(pCullFlag);
    args.push_back(pSignMask0);
    args.push_back(pSignMask1);
    args.push_back(pSignMask2);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingCullDistance,
                         m_pContext->BoolTy(),
                         args,
                         attribs,
                         pInsertAtEnd);

    return pCullFlag;
}

// =====================================================================================================================
// Fetches culling-control register from primitive shader table.
Value* NggPrimShader::FetchCullingControlRegister(
    Module*     pModule,        // [in] LLVM module
    uint32_t    regOffset,      // Register offset in the primitive shader table (in BYTEs)
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    std::vector<Value*> args;

    args.push_back(m_nggFactor.pPrimShaderTableAddrLow);
    args.push_back(m_nggFactor.pPrimShaderTableAddrHigh);
    args.push_back(ConstantInt::get(m_pContext->Int32Ty(), regOffset));

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadOnly);

    auto pRegValue = EmitCall(pModule,
                              LlpcName::NggCullingFetchReg,
                              m_pContext->Int32Ty(),
                              args,
                              attribs,
                              pInsertAtEnd);

    return pRegValue;
}

// =====================================================================================================================
// Output a subgroup ballot
Value* NggPrimShader::DoSubgroupBallot(
    Module*     pModule,      // [in] LLVM module
    Value*      pValue,       // [in] The value to do the ballot on.
    BasicBlock* pInsertAtEnd) // [in] Where to insert instructions
{
    LLPC_ASSERT(pValue->getType()->isIntegerTy(1));

    const uint32_t waveSize = m_pContext->GetShaderWaveSize(ShaderStageGeometry);
    LLPC_ASSERT((waveSize == 32) || (waveSize == 64));

    pValue = new ZExtInst(pValue, m_pContext->Int32Ty(), "", pInsertAtEnd);

    Type* const pThreadMaskTy = (waveSize == 64) ? m_pContext->Int64Ty() : m_pContext->Int32Ty();

    Function* const pICmpFunc = Intrinsic::getDeclaration(pModule,
                                                          Intrinsic::amdgcn_icmp,
                                                          {
                                                              pThreadMaskTy,
                                                              m_pContext->Int32Ty()
                                                          });

    Value* pThreadMask = CallInst::Create(pICmpFunc,
                                          {
                                             pValue,
                                             ConstantInt::get(m_pContext->Int32Ty(), 0),
                                             ConstantInt::get(m_pContext->Int32Ty(), 33) // 33 = predicate NE
                                          },
                                          "",
                                          pInsertAtEnd);

    if (waveSize != 64)
    {
        pThreadMask = new ZExtInst(pThreadMask, m_pContext->Int64Ty(), "", pInsertAtEnd);
    }

    return pThreadMask;
}

} // Llpc
