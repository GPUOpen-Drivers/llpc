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
    m_pLdsManager(nullptr),
    m_pBuilder(new IRBuilder<>(*m_pContext))
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
    m_pLdsManager = new NggLdsManager(pModule, m_pContext, m_pBuilder.get());

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
        argTys.push_back(m_pBuilder->getInt32Ty());
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
        argTys.push_back(VectorType::get(m_pBuilder->getInt32Ty(), userDataCount));
        *pInRegMask |= (1ull << EsGsSpecialSysValueCount);
    }

    // Other system values (VGPRs)
    argTys.push_back(m_pBuilder->getInt32Ty());         // ES to GS offsets (vertex 0 and 1)
    argTys.push_back(m_pBuilder->getInt32Ty());         // ES to GS offsets (vertex 2 and 3)
    argTys.push_back(m_pBuilder->getInt32Ty());         // Primitive ID (GS)
    argTys.push_back(m_pBuilder->getInt32Ty());         // Invocation ID
    argTys.push_back(m_pBuilder->getInt32Ty());         // ES to GS offsets (vertex 4 and 5)

    if (hasTs)
    {
        argTys.push_back(m_pBuilder->getFloatTy());    // X of TessCoord (U)
        argTys.push_back(m_pBuilder->getFloatTy());    // Y of TessCoord (V)
        argTys.push_back(m_pBuilder->getInt32Ty());    // Relative patch ID
        argTys.push_back(m_pBuilder->getInt32Ty());    // Patch ID
    }
    else
    {
        argTys.push_back(m_pBuilder->getInt32Ty());    // Vertex ID
        argTys.push_back(m_pBuilder->getInt32Ty());    // Relative vertex ID (auto index)
        argTys.push_back(m_pBuilder->getInt32Ty());    // Primitive ID (VS)
        argTys.push_back(m_pBuilder->getInt32Ty());    // Instance ID
    }

    return FunctionType::get(m_pBuilder->getVoidTy(), argTys, false);
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
                m_pBuilder->SetInsertPoint(pEntryBlock);

                m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_pBuilder->getInt64(-1));

                auto pThreadIdInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                                   {},
                                                                   {
                                                                       m_pBuilder->getInt32(-1),
                                                                       m_pBuilder->getInt32(0)
                                                                   });

                if (waveSize == 64)
                {
                    pThreadIdInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                                  {},
                                                                  {
                                                                      m_pBuilder->getInt32(-1),
                                                                      pThreadIdInWave
                                                                  });
                }

                auto pPrimCountInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                        m_pBuilder->getInt32Ty(),
                                                                        {
                                                                            pMergedGroupInfo,
                                                                            m_pBuilder->getInt32(22),
                                                                            m_pBuilder->getInt32(9)
                                                                        });

                auto pVertCountInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                        m_pBuilder->getInt32Ty(),
                                                                        {
                                                                            pMergedGroupInfo,
                                                                            m_pBuilder->getInt32(12),
                                                                            m_pBuilder->getInt32(9)
                                                                        });

                auto pVertCountInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_pBuilder->getInt32Ty(),
                                                                    {
                                                                        pMergedWaveInfo,
                                                                        m_pBuilder->getInt32(0),
                                                                        m_pBuilder->getInt32(8)
                                                                    });

                auto pPrimCountInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_pBuilder->getInt32Ty(),
                                                                    {
                                                                        pMergedWaveInfo,
                                                                        m_pBuilder->getInt32(8),
                                                                        m_pBuilder->getInt32(8)
                                                                    });

                auto pWaveIdInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                     m_pBuilder->getInt32Ty(),
                                                                     {
                                                                         pMergedWaveInfo,
                                                                         m_pBuilder->getInt32(24),
                                                                         m_pBuilder->getInt32(4)
                                                                     });

                auto pThreadIdInSubgroup = m_pBuilder->CreateMul(pWaveIdInSubgroup, m_pBuilder->getInt32(waveSize));
                pThreadIdInSubgroup = m_pBuilder->CreateAdd(pThreadIdInSubgroup, pThreadIdInWave);

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
                    auto pPrimValid = m_pBuilder->CreateICmpULT(pThreadIdInWave, pPrimCountInWave);
                    m_pBuilder->CreateCondBr(pPrimValid, pWritePrimIdBlock, pEndWritePrimIdBlock);
                }
                else
                {
                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    auto pFirstWaveInSubgroup = m_pBuilder->CreateICmpEQ(pWaveIdInSubgroup, m_pBuilder->getInt32(0));
                    m_pBuilder->CreateCondBr(pFirstWaveInSubgroup, pAllocReqBlock, pEndAllocReqBlock);
                }
            }

            if (distributePrimId)
            {
                // Construct ".writePrimId" block
                {
                    m_pBuilder->SetInsertPoint(pWritePrimIdBlock);

                    // Primitive data layout
                    //   ES_GS_OFFSET01[31]    = null primitive flag
                    //   ES_GS_OFFSET01[28:20] = vertexId2 (in bytes)
                    //   ES_GS_OFFSET01[18:10] = vertexId1 (in bytes)
                    //   ES_GS_OFFSET01[8:0]   = vertexId0 (in bytes)

                    // Distribute primitive ID
                    auto pVertexId0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                  m_pBuilder->getInt32Ty(),
                                                                  {
                                                                      m_nggFactor.pEsGsOffsets01,
                                                                      m_pBuilder->getInt32(0),
                                                                      m_pBuilder->getInt32(9)
                                                                  });

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                    auto pLdsOffset = m_pBuilder->CreateShl(pVertexId0, m_pBuilder->getInt32(2));
                    pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(regionStart), pLdsOffset);

                    auto pPrimIdWriteValue = pGsPrimitiveId;
                    m_pLdsManager->WriteValueToLds(pPrimIdWriteValue, pLdsOffset);

                    BranchInst::Create(pEndWritePrimIdBlock, pWritePrimIdBlock);
                }

                // Construct ".endWritePrimId" block
                {
                    m_pBuilder->SetInsertPoint(pEndWritePrimIdBlock);

                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    auto pVertValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave,
                                                                m_nggFactor.pVertCountInWave);
                    m_pBuilder->CreateCondBr(pVertValid, pReadPrimIdBlock, pEndReadPrimIdBlock);
                }

                // Construct ".readPrimId" block
                Value* pPrimIdReadValue = nullptr;
                {
                    m_pBuilder->SetInsertPoint(pReadPrimIdBlock);

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                    auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(2));
                    pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(regionStart), pLdsOffset);

                    pPrimIdReadValue = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

                    m_pBuilder->CreateBr(pEndReadPrimIdBlock);
                }

                // Construct ".endReadPrimId" block
                {
                    m_pBuilder->SetInsertPoint(pEndReadPrimIdBlock);

                    auto pPrimitiveId = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);

                    pPrimitiveId->addIncoming(pPrimIdReadValue, pReadPrimIdBlock);
                    pPrimitiveId->addIncoming(m_pBuilder->getInt32(0), pEndWritePrimIdBlock);

                    // Record primitive ID
                    m_nggFactor.pPrimitiveId = pPrimitiveId;

                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    auto pFirstWaveInSubgroup = m_pBuilder->CreateICmpEQ(m_nggFactor.pWaveIdInSubgroup,
                                                                         m_pBuilder->getInt32(0));
                    m_pBuilder->CreateCondBr(pFirstWaveInSubgroup, pAllocReqBlock, pEndAllocReqBlock);
                }
            }

            // Construct ".allocReq" block
            {
                m_pBuilder->SetInsertPoint(pAllocReqBlock);

                DoParamCacheAllocRequest();
                m_pBuilder->CreateBr(pEndAllocReqBlock);
            }

            // Construct ".endAllocReq" block
            {
                m_pBuilder->SetInsertPoint(pEndAllocReqBlock);

                auto pPrimExp =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pPrimCountInSubgroup);
                m_pBuilder->CreateCondBr(pPrimExp, pExpPrimBlock, pEndExpPrimBlock);
            }

            // Construct ".expPrim" block
            {
                m_pBuilder->SetInsertPoint(pExpPrimBlock);

                DoPrimitiveExport();
                m_pBuilder->CreateBr(pEndExpPrimBlock);
            }

            // Construct ".endExpPrim" block
            {
                m_pBuilder->SetInsertPoint(pEndExpPrimBlock);

                auto pVertExp =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pVertCountInSubgroup);
                m_pBuilder->CreateCondBr(pVertExp, pExpVertBlock, pEndExpVertBlock);
            }

            // Construct ".expVert" block
            {
                m_pBuilder->SetInsertPoint(pExpVertBlock);

                RunEsOrEsVariant(pModule,
                                 LlpcName::NggEsEntryPoint,
                                 pEntryPoint->arg_begin(),
                                 false,
                                 nullptr,
                                 pExpVertBlock);

                m_pBuilder->CreateBr(pEndExpVertBlock);
            }

            // Construct ".endExpVert" block
            {
                m_pBuilder->SetInsertPoint(pEndExpVertBlock);

                m_pBuilder->CreateRetVoid();
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
                m_pBuilder->SetInsertPoint(pEntryBlock);

                m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_pBuilder->getInt64(-1));

                auto pThreadIdInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                                   {},
                                                                   {
                                                                       m_pBuilder->getInt32(-1),
                                                                       m_pBuilder->getInt32(0)
                                                                   });

                if (waveSize == 64)
                {
                    pThreadIdInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                                   {},
                                                                   {
                                                                       m_pBuilder->getInt32(-1),
                                                                       pThreadIdInWave
                                                                   });
                }

                auto pPrimCountInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                        m_pBuilder->getInt32Ty(),
                                                                        {
                                                                            pMergedGroupInfo,
                                                                            m_pBuilder->getInt32(22),
                                                                            m_pBuilder->getInt32(9),
                                                                        });

                auto pVertCountInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                        m_pBuilder->getInt32Ty(),
                                                                        {
                                                                            pMergedGroupInfo,
                                                                            m_pBuilder->getInt32(12),
                                                                            m_pBuilder->getInt32(9),
                                                                        });

                auto pVertCountInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_pBuilder->getInt32Ty(),
                                                                    {
                                                                        pMergedWaveInfo,
                                                                        m_pBuilder->getInt32(0),
                                                                        m_pBuilder->getInt32(8),
                                                                    });

                auto pPrimCountInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_pBuilder->getInt32Ty(),
                                                                    {
                                                                        pMergedWaveInfo,
                                                                        m_pBuilder->getInt32(8),
                                                                        m_pBuilder->getInt32(8),
                                                                    });

                auto pWaveIdInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                     m_pBuilder->getInt32Ty(),
                                                                     {
                                                                         pMergedWaveInfo,
                                                                         m_pBuilder->getInt32(24),
                                                                         m_pBuilder->getInt32(4),
                                                                     });

                auto pThreadIdInSubgroup = m_pBuilder->CreateMul(pWaveIdInSubgroup, m_pBuilder->getInt32(waveSize));
                pThreadIdInSubgroup = m_pBuilder->CreateAdd(pThreadIdInSubgroup, pThreadIdInWave);

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
                    auto pPrimValid = m_pBuilder->CreateICmpULT(pThreadIdInWave, pPrimCountInWave);
                    m_pBuilder->CreateCondBr(pPrimValid, pWritePrimIdBlock, pEndWritePrimIdBlock);
                }
                else
                {
                    auto pFirstThreadInSubgroup =
                        m_pBuilder->CreateICmpEQ(pThreadIdInSubgroup, m_pBuilder->getInt32(0));
                    m_pBuilder->CreateCondBr(pFirstThreadInSubgroup, pZeroThreadCountBlock, pEndZeroThreadCountBlock);
                }
            }

            if (distributePrimId)
            {
                // Construct ".writePrimId" block
                {
                    m_pBuilder->SetInsertPoint(pWritePrimIdBlock);

                    // Primitive data layout
                    //   ES_GS_OFFSET23[15:0]  = vertexId2 (in DWORDs)
                    //   ES_GS_OFFSET01[31:16] = vertexId1 (in DWORDs)
                    //   ES_GS_OFFSET01[15:0]  = vertexId0 (in DWORDs)

                    // Use vertex0 as provoking vertex to distribute primitive ID
                    auto pEsGsOffset0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_pBuilder->getInt32Ty(),
                                                                    {
                                                                        m_nggFactor.pEsGsOffsets01,
                                                                        m_pBuilder->getInt32(0),
                                                                        m_pBuilder->getInt32(16),
                                                                    });

                    auto pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, m_pBuilder->getInt32(2));

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                    auto pLdsOffset = m_pBuilder->CreateShl(pVertexId0, m_pBuilder->getInt32(2));
                    pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(regionStart), pLdsOffset);

                    auto pPrimIdWriteValue = pGsPrimitiveId;
                    m_pLdsManager->WriteValueToLds(pPrimIdWriteValue, pLdsOffset);

                    m_pBuilder->CreateBr(pEndWritePrimIdBlock);
                }

                // Construct ".endWritePrimId" block
                {
                    m_pBuilder->SetInsertPoint(pEndWritePrimIdBlock);

                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    auto pVertValid =
                        m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pVertCountInWave);
                    m_pBuilder->CreateCondBr(pVertValid, pReadPrimIdBlock, pEndReadPrimIdBlock);
                }

                // Construct ".readPrimId" block
                Value* pPrimIdReadValue = nullptr;
                {
                    m_pBuilder->SetInsertPoint(pReadPrimIdBlock);

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                    auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(2));
                    pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(regionStart), pLdsOffset);

                    pPrimIdReadValue =
                        m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

                    m_pBuilder->CreateBr(pEndReadPrimIdBlock);
                }

                // Construct ".endReadPrimId" block
                {
                    m_pBuilder->SetInsertPoint(pEndReadPrimIdBlock);

                    auto pPrimitiveId = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);

                    pPrimitiveId->addIncoming(pPrimIdReadValue, pReadPrimIdBlock);
                    pPrimitiveId->addIncoming(m_pBuilder->getInt32(0), pEndWritePrimIdBlock);

                    // Record primitive ID
                    m_nggFactor.pPrimitiveId = pPrimitiveId;

                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    auto pFirstThreadInSubgroup =
                        m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(0));
                    m_pBuilder->CreateCondBr(pFirstThreadInSubgroup,
                                             pZeroThreadCountBlock,
                                             pEndZeroThreadCountBlock);
                }
            }

            // Construct ".zeroThreadCount" block
            {
                m_pBuilder->SetInsertPoint(pZeroThreadCountBlock);

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(
                    vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);

                auto pZero = m_pBuilder->getInt32(0);

                // Zero per-wave primitive/vertex count
                std::vector<Constant*> zeros;
                for (uint32_t i = 0; i < Gfx9::NggMaxWavesPerSubgroup; ++i)
                {
                    zeros.push_back(pZero);
                }
                auto pZeros = ConstantVector::get(zeros);

                auto pLdsOffset = m_pBuilder->getInt32(regionStart);
                m_pLdsManager->WriteValueToLds(pZeros, pLdsOffset);

                // Zero sub-group primitive/vertex count
                pLdsOffset = m_pBuilder->getInt32(regionStart + SizeOfDword * Gfx9::NggMaxWavesPerSubgroup);
                m_pLdsManager->WriteValueToLds(pZero, pLdsOffset);

                m_pBuilder->CreateBr(pEndZeroThreadCountBlock);
            }

            // Construct ".endZeroThreadCount" block
            {
                m_pBuilder->SetInsertPoint(pEndZeroThreadCountBlock);

                auto pFirstWaveInSubgroup =
                    m_pBuilder->CreateICmpEQ(m_nggFactor.pWaveIdInSubgroup, m_pBuilder->getInt32(0));
                m_pBuilder->CreateCondBr(pFirstWaveInSubgroup, pZeroDrawFlagBlock, pEndZeroDrawFlagBlock);
            }

            // Construct ".zeroDrawFlag" block
            {
                m_pBuilder->SetInsertPoint(pZeroDrawFlagBlock);

                Value* pLdsOffset =
                    m_pBuilder->CreateMul(m_nggFactor.pThreadIdInWave, m_pBuilder->getInt32(SizeOfDword));

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);

                pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                auto pZero = m_pBuilder->getInt32(0);
                m_pLdsManager->WriteValueToLds(pZero, pLdsOffset);

                if (waveCountInSubgroup == 8)
                {
                    LLPC_ASSERT(waveSize == 32);
                    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(32 * SizeOfDword));
                    m_pLdsManager->WriteValueToLds(pZero, pLdsOffset);
                }

                m_pBuilder->CreateBr(pEndZeroDrawFlagBlock);
            }

            // Construct ".endZeroDrawFlag" block
            {
                m_pBuilder->SetInsertPoint(pEndZeroDrawFlagBlock);

                auto pVertValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pVertCountInWave);
                m_pBuilder->CreateCondBr(pVertValid, pWritePosDataBlock, pEndWritePosDataBlock);
            }

            // Construct ".writePosData" block
            std::vector<ExpData> expDataSet;
            bool separateExp = false;
            {
                m_pBuilder->SetInsertPoint(pWritePosDataBlock);

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

                        Value* pLdsOffset =
                            m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(SizeOfVec4));
                        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                        m_pLdsManager->WriteValueToLds(expData.pExpValue, pLdsOffset);

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
                                auto pExpValue = m_pBuilder->CreateExtractElement(expData.pExpValue, i);
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
                    Value* pSignMask = m_pBuilder->getInt32(0);
                    for (uint32_t i = 0; i < cullDistance.size(); ++i)
                    {
                        auto pCullDistance = m_pBuilder->CreateBitCast(cullDistance[i], m_pBuilder->getInt32Ty());

                        Value* pSignBit = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                      m_pBuilder->getInt32Ty(),
                                                                      {
                                                                          pCullDistance,
                                                                          m_pBuilder->getInt32(31),
                                                                          m_pBuilder->getInt32(1)
                                                                      });
                        pSignBit = m_pBuilder->CreateShl(pSignBit, m_pBuilder->getInt32(i));

                        pSignMask = m_pBuilder->CreateOr(pSignMask, pSignBit);
                    }

                    // Write the sign mask to LDS
                    const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionCullDistance);

                    Value* pLdsOffset =
                        m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(SizeOfDword));
                    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                    m_pLdsManager->WriteValueToLds(pSignMask, pLdsOffset);
                }

                m_pBuilder->CreateBr(pEndWritePosDataBlock);
            }

            // Construct ".endWritePosData" block
            {
                m_pBuilder->SetInsertPoint(pEndWritePosDataBlock);

                auto pUndef = UndefValue::get(m_pContext->Floatx4Ty());
                for (auto& expData : expDataSet)
                {
                    PHINode* pExpValue = m_pBuilder->CreatePHI(m_pContext->Floatx4Ty(), 2);
                    pExpValue->addIncoming(expData.pExpValue, pWritePosDataBlock);
                    pExpValue->addIncoming(pUndef, pEndZeroDrawFlagBlock);

                    expData.pExpValue = pExpValue; // Update the exportd data
                }

                m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto pPrimValidInWave =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
                auto pPrimValidInSubgroup =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pPrimCountInSubgroup);

                auto pPrimValid = m_pBuilder->CreateAnd(pPrimValidInWave, pPrimValidInSubgroup);
                m_pBuilder->CreateCondBr(pPrimValid, pCullingBlock, pEndCullingBlock);
            }

            // Construct ".culling" block
            Value* pDoCull = nullptr;
            {
                m_pBuilder->SetInsertPoint(pCullingBlock);

                pDoCull = DoCulling(pModule, pCullingBlock);
                m_pBuilder->CreateBr(pEndCullingBlock);
            }

            // Construct ".endCulling" block
            Value* pDrawFlag = nullptr;
            PHINode* pCullFlag = nullptr;
            {
                m_pBuilder->SetInsertPoint(pEndCullingBlock);

                pCullFlag = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);

                pCullFlag->addIncoming(m_pBuilder->getTrue(), pEndWritePosDataBlock);
                pCullFlag->addIncoming(pDoCull, pCullingBlock);

                pDrawFlag = m_pBuilder->CreateNot(pCullFlag);
                m_pBuilder->CreateCondBr(pDrawFlag, pWriteDrawFlagBlock, pEndWriteDrawFlagBlock);
            }

            // Construct ".writeDrawFlag" block
            {
                m_pBuilder->SetInsertPoint(pWriteDrawFlagBlock);

                auto pEsGsOffset0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                m_pBuilder->getInt32Ty(),
                                                                {
                                                                    pEsGsOffsets01,
                                                                    m_pBuilder->getInt32(0),
                                                                    m_pBuilder->getInt32(16)
                                                                });
                auto pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, m_pBuilder->getInt32(2));

                auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                m_pBuilder->getInt32Ty(),
                                                                {
                                                                    pEsGsOffsets01,
                                                                    m_pBuilder->getInt32(16),
                                                                    m_pBuilder->getInt32(16)
                                                                });
                auto pVertexId1 = m_pBuilder->CreateLShr(pEsGsOffset1, m_pBuilder->getInt32(2));

                auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                m_pBuilder->getInt32Ty(),
                                                                {
                                                                    pEsGsOffsets23,
                                                                    m_pBuilder->getInt32(0),
                                                                    m_pBuilder->getInt32(16)
                                                                });
                auto pVertexId2 = m_pBuilder->CreateLShr(pEsGsOffset2, m_pBuilder->getInt32(2));

                Value* vertexId[3] = { pVertexId0, pVertexId1, pVertexId2 };

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);
                auto pRegionStart = m_pBuilder->getInt32(regionStart);

                auto pOne = m_pBuilder->getInt8(1);

                for (uint32_t i = 0; i < 3; ++i)
                {
                    auto pLdsOffset = m_pBuilder->CreateAdd(pRegionStart, vertexId[i]);
                    m_pLdsManager->WriteValueToLds(pOne, pLdsOffset);
                }

                m_pBuilder->CreateBr(pEndWriteDrawFlagBlock);
            }

            // Construct ".endWriteDrawFlag" block
            Value* pDrawCount = nullptr;
            {
                m_pBuilder->SetInsertPoint(pEndWriteDrawFlagBlock);

                if (vertexCompact)
                {
                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);

                    auto pLdsOffset =
                        m_pBuilder->CreateAdd(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(regionStart));

                    pDrawFlag = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt8Ty(), pLdsOffset);
                    pDrawFlag = m_pBuilder->CreateTrunc(pDrawFlag, m_pBuilder->getInt1Ty());
                }

                auto pDrawMask = DoSubgroupBallot(pDrawFlag);

                pDrawCount = m_pBuilder->CreateIntrinsic(Intrinsic::ctpop,
                                                         m_pBuilder->getInt64Ty(),
                                                         pDrawMask);

                pDrawCount = m_pBuilder->CreateTrunc(pDrawCount, m_pBuilder->getInt32Ty());

                auto pThreadIdUpbound = m_pBuilder->CreateSub(m_pBuilder->getInt32(waveCountInSubgroup),
                                                              m_nggFactor.pWaveIdInSubgroup);
                auto pThreadValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, pThreadIdUpbound);

                Value* pPrimCountAcc = nullptr;
                if (vertexCompact)
                {
                    pPrimCountAcc = pThreadValid;
                }
                else
                {
                    auto pHasSurviveDraw = m_pBuilder->CreateICmpNE(pDrawCount, m_pBuilder->getInt32(0));

                    pPrimCountAcc = m_pBuilder->CreateAnd(pHasSurviveDraw, pThreadValid);
                }

                m_pBuilder->CreateCondBr(pPrimCountAcc, pAccThreadCountBlock,pEndAccThreadCountBlock);
            }

            // Construct ".accThreadCount" block
            {
                m_pBuilder->SetInsertPoint(pAccThreadCountBlock);

                auto pLdsOffset = m_pBuilder->CreateAdd(m_nggFactor.pWaveIdInSubgroup, m_nggFactor.pThreadIdInWave);
                pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(1));
                pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, m_pBuilder->getInt32(2));

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(
                    vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);

                pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));
                m_pLdsManager->AtomicOpWithLds(AtomicRMWInst::Add, pDrawCount, pLdsOffset);

                m_pBuilder->CreateBr(pEndAccThreadCountBlock);
            }

            // Construct ".endAccThreadCount" block
            {
                m_pBuilder->SetInsertPoint(pEndAccThreadCountBlock);

                m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                if (vertexCompact)
                {
                    m_pBuilder->CreateBr(pReadThreadCountBlock);
                }
                else
                {
                    auto pFirstThreadInWave =
                        m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInWave, m_pBuilder->getInt32(0));

                    m_pBuilder->CreateCondBr(pFirstThreadInWave,
                                             pReadThreadCountBlock,
                                             pEndReadThreadCountBlock);
                }
            }

            if (vertexCompact)
            {
                // Construct ".readThreadCount" block
                Value* pVertCountInWaves = nullptr;
                Value* pVertCountInPrevWaves = nullptr;
                {
                    m_pBuilder->SetInsertPoint(pReadThreadCountBlock);

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionVertCountInWaves);

                    // The DWORD following DWORDs for all waves stores the vertex count of the entire sub-group
                    Value* pLdsOffset = m_pBuilder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
                    pVertCountInWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

                    // NOTE: We promote vertex count in waves to SGPR since it is treated as an uniform value.
                    pVertCountInWaves =
                        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pVertCountInWaves);

                    // Get vertex count for all waves prior to this wave
                    pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pWaveIdInSubgroup, m_pBuilder->getInt32(2));
                    pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(regionStart), pLdsOffset);

                    pVertCountInPrevWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                    auto pVertValid =
                        m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pVertCountInWave);

                    auto pCompactDataWrite = m_pBuilder->CreateAnd(pDrawFlag, pVertValid);

                    m_pBuilder->CreateCondBr(pCompactDataWrite,
                                             pWriteCompactDataBlock,
                                             pEndWriteCompactDataBlock);
                }

                // Construct ".writeCompactData" block
                {
                    m_pBuilder->SetInsertPoint(pWriteCompactDataBlock);

                    Value* pDrawMask = DoSubgroupBallot(pDrawFlag);
                    pDrawMask = m_pBuilder->CreateBitCast(pDrawMask, m_pContext->Int32x2Ty());

                    auto pDrawMaskLow = m_pBuilder->CreateExtractElement(pDrawMask, static_cast<uint64_t>(0));

                    Value* pCompactThreadIdInSubrgoup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                                                    {},
                                                                                    {
                                                                                        pDrawMaskLow,
                                                                                        m_pBuilder->getInt32(0)
                                                                                    });

                    if (waveSize == 64)
                    {
                        auto pDrawMaskHigh = m_pBuilder->CreateExtractElement(pDrawMask, 1);

                        pCompactThreadIdInSubrgoup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                                                {},
                                                                                {
                                                                                    pDrawMaskHigh,
                                                                                    pCompactThreadIdInSubrgoup
                                                                                });
                    }

                    pCompactThreadIdInSubrgoup =
                        m_pBuilder->CreateAdd(pVertCountInPrevWaves, pCompactThreadIdInSubrgoup);

                    // Write vertex position data to LDS
                    for (const auto& expData : expDataSet)
                    {
                        if (expData.target == EXP_TARGET_POS_0)
                        {
                            const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);

                            Value* pLdsOffset =
                                m_pBuilder->CreateMul(pCompactThreadIdInSubrgoup, m_pBuilder->getInt32(SizeOfVec4));
                            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                            m_pLdsManager->WriteValueToLds(expData.pExpValue, pLdsOffset);

                            break;
                        }
                    }

                    // Write thread ID in sub-group to LDS
                    Value* pCompactThreadId =
                        m_pBuilder->CreateTrunc(pCompactThreadIdInSubrgoup, m_pBuilder->getInt8Ty());
                    WriteCompactDataToLds(pCompactThreadId,
                                          m_nggFactor.pThreadIdInSubgroup,
                                          LdsRegionCompactThreadIdInSubgroup);

                    if (hasTs)
                    {
                        // Write X/Y of tessCoord (U/V) to LDS
                        if (pResUsage->builtInUsage.tes.tessCoord)
                        {
                            WriteCompactDataToLds(pTessCoordX,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactTessCoordX);

                            WriteCompactDataToLds(pTessCoordY,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactTessCoordY);
                        }

                        // Write relative patch ID to LDS
                        WriteCompactDataToLds(pRelPatchId,
                                              pCompactThreadIdInSubrgoup,
                                              LdsRegionCompactRelPatchId);

                        // Write patch ID to LDS
                        if (pResUsage->builtInUsage.tes.primitiveId)
                        {
                            WriteCompactDataToLds(pPatchId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactPatchId);
                        }
                    }
                    else
                    {
                        // Write vertex ID to LDS
                        if (pResUsage->builtInUsage.vs.vertexIndex)
                        {
                            WriteCompactDataToLds(pVertexId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactVertexId);
                        }

                        // Write instance ID to LDS
                        if (pResUsage->builtInUsage.vs.instanceIndex)
                        {
                            WriteCompactDataToLds(pInstanceId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactInstanceId);
                        }

                        // Write primitive ID to LDS
                        if (pResUsage->builtInUsage.vs.primitiveId)
                        {
                            LLPC_ASSERT(m_nggFactor.pPrimitiveId != nullptr);
                            WriteCompactDataToLds(m_nggFactor.pPrimitiveId,
                                                  pCompactThreadIdInSubrgoup,
                                                  LdsRegionCompactPrimId);
                        }
                    }

                    m_pBuilder->CreateBr(pEndWriteCompactDataBlock);
                }

                // Construct dummy export blocks
                BasicBlock* pDummyExportBlock = nullptr;
                {
                    pDummyExportBlock = ConstructDummyExport(pModule, pEntryPoint);
                }

                // Construct ".endWriteCompactData" block
                {
                    m_pBuilder->SetInsertPoint(pEndWriteCompactDataBlock);

                    Value* pHasSurviveVert = m_pBuilder->CreateICmpNE(pVertCountInWaves, m_pBuilder->getInt32(0));

                    m_pBuilder->CreateCondBr(pHasSurviveVert, pEndReadThreadCountBlock, pDummyExportBlock);
                }

                // Construct ".endReadThreadCount" block
                {
                    m_pBuilder->SetInsertPoint(pEndReadThreadCountBlock);

                    m_nggFactor.pVertCountInSubgroup = pVertCountInWaves;

                    auto pFirstWaveInSubgroup =
                        m_pBuilder->CreateICmpEQ(m_nggFactor.pWaveIdInSubgroup, m_pBuilder->getInt32(0));

                    m_pBuilder->CreateCondBr(pFirstWaveInSubgroup, pAllocReqBlock, pEndAllocReqBlock);
                }
            }
            else
            {
                // Construct ".readThreadCount" block
                Value* pPrimCountInWaves = nullptr;
                {
                    m_pBuilder->SetInsertPoint(pReadThreadCountBlock);

                    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPrimCountInWaves);

                    // The DWORD following DWORDs for all waves stores the primitive count of the entire sub-group
                    auto pLdsOffset = m_pBuilder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
                    pPrimCountInWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

                    m_pBuilder->CreateBr(pEndReadThreadCountBlock);
                }

                // Construct ".endReadThreadCount" block
                {
                    m_pBuilder->SetInsertPoint(pEndReadThreadCountBlock);

                    Value* pPrimCount = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);

                    static_cast<PHINode*>(pPrimCount)->addIncoming(m_nggFactor.pPrimCountInSubgroup,
                                                                   pEndAccThreadCountBlock);
                    static_cast<PHINode*>(pPrimCount)->addIncoming(pPrimCountInWaves, pReadThreadCountBlock);

                    // NOTE: We promote primitive count in waves to SGPR since it is treated as an uniform value.
                    pPrimCount = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pPrimCount);

                    Value* pHasSurvivePrim = m_pBuilder->CreateICmpNE(pPrimCount, m_pBuilder->getInt32(0));

                    Value* pPrimCountInSubgroup = m_pBuilder->CreateSelect(pHasSurvivePrim,
                                                                           m_nggFactor.pPrimCountInSubgroup,
                                                                           m_pBuilder->getInt32(0));

                    // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
                    // as an uniform value later. This is similar to the provided primitive count in sub-group that is
                    // a system value.
                    pPrimCountInSubgroup =
                        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pPrimCountInSubgroup);

                    pHasSurvivePrim = m_pBuilder->CreateICmpNE(pPrimCountInSubgroup, m_pBuilder->getInt32(0));

                    Value* pVertCountInSubgroup = m_pBuilder->CreateSelect(pHasSurvivePrim,
                                                                           m_nggFactor.pVertCountInSubgroup,
                                                                           m_pBuilder->getInt32(0));

                    // NOTE: Here, we have to promote revised vertex count in sub-group to SGPR since it is treated as
                    // an uniform value later, similar to what we have done for the revised primitive count in
                    // sub-group.
                    pVertCountInSubgroup =
                        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pVertCountInSubgroup);

                    m_nggFactor.pPrimCountInSubgroup = pPrimCountInSubgroup;
                    m_nggFactor.pVertCountInSubgroup = pVertCountInSubgroup;

                    auto pFirstWaveInSubgroup =
                        m_pBuilder->CreateICmpEQ(m_nggFactor.pWaveIdInSubgroup, m_pBuilder->getInt32(0));

                    m_pBuilder->CreateCondBr(pFirstWaveInSubgroup, pAllocReqBlock, pEndAllocReqBlock);
                }
            }

            // Construct ".allocReq" block
            {
                m_pBuilder->SetInsertPoint(pAllocReqBlock);

                DoParamCacheAllocRequest();
                m_pBuilder->CreateBr(pEndAllocReqBlock);
            }

            // Construct ".endAllocReq" block
            {
                m_pBuilder->SetInsertPoint(pEndAllocReqBlock);

                if (vertexCompact)
                {
                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});
                }

                auto pPrimExp =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pPrimCountInSubgroup);
                m_pBuilder->CreateCondBr(pPrimExp, pExpPrimBlock, pEndExpPrimBlock);
            }

            // Construct ".expPrim" block
            {
                m_pBuilder->SetInsertPoint(pExpPrimBlock);

                DoPrimitiveExport(vertexCompact ? pCullFlag : nullptr);
                m_pBuilder->CreateBr(pEndExpPrimBlock);
            }

            // Construct ".endExpPrim" block
            Value* pVertExp = nullptr;
            {
                m_pBuilder->SetInsertPoint(pEndExpPrimBlock);

                pVertExp =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pVertCountInSubgroup);
                m_pBuilder->CreateCondBr(pVertExp, pExpVertPosBlock, pEndExpVertPosBlock);
            }

            // Construct ".expVertPos" block
            {
                m_pBuilder->SetInsertPoint(pExpVertPosBlock);

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
                                m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(SizeOfVec4));
                            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                            auto pExpValue = m_pLdsManager->ReadValueFromLds(m_pContext->Floatx4Ty(), pLdsOffset);
                            expData.pExpValue = pExpValue;

                            break;
                        }
                    }
                }

                for (const auto& expData : expDataSet)
                {
                    if ((expData.target >= EXP_TARGET_POS_0) && (expData.target <= EXP_TARGET_POS_4))
                    {
                        std::vector<Value*> args;

                        args.push_back(m_pBuilder->getInt32(expData.target));        // tgt
                        args.push_back(m_pBuilder->getInt32(expData.channelMask));   // en

                        // src0 ~ src3
                        for (uint32_t i = 0; i < 4; ++i)
                        {
                            auto pExpValue = m_pBuilder->CreateExtractElement(expData.pExpValue, i);
                            args.push_back(pExpValue);
                        }

                        args.push_back(m_pBuilder->getInt1(expData.doneFlag));       // done
                        args.push_back(m_pBuilder->getFalse());                      // vm

                        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_pBuilder->getFloatTy(), args);
                    }
                }

                m_pBuilder->CreateBr(pEndExpVertPosBlock);
            }

            // Construct ".endExpVertPos" block
            {
                m_pBuilder->SetInsertPoint(pEndExpVertPosBlock);

                if (vertexCompact)
                {
                    auto pUndef = UndefValue::get(m_pContext->Floatx4Ty());
                    for (auto& expData : expDataSet)
                    {
                        PHINode* pExpValue = m_pBuilder->CreatePHI(m_pContext->Floatx4Ty(), 2);

                        pExpValue->addIncoming(expData.pExpValue, pExpVertPosBlock);
                        pExpValue->addIncoming(pUndef, pEndExpPrimBlock);

                        expData.pExpValue = pExpValue; // Update the exportd data
                    }
                }

                m_pBuilder->CreateCondBr(pVertExp, pExpVertParamBlock, pEndExpVertParamBlock);
            }

            // Construct ".expVertParam" block
            {
                m_pBuilder->SetInsertPoint(pExpVertParamBlock);

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
                        std::vector<Value*> args;

                        args.push_back(m_pBuilder->getInt32(expData.target));        // tgt
                        args.push_back(m_pBuilder->getInt32(expData.channelMask));   // en

                                                                                     // src0 ~ src3
                        for (uint32_t i = 0; i < 4; ++i)
                        {
                            auto pExpValue = m_pBuilder->CreateExtractElement(expData.pExpValue, i);
                            args.push_back(pExpValue);
                        }

                        args.push_back(m_pBuilder->getInt1(expData.doneFlag));       // done
                        args.push_back(m_pBuilder->getFalse());                      // vm

                        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_pBuilder->getFloatTy(), args);
                    }
                }

                m_pBuilder->CreateBr(pEndExpVertParamBlock);
            }

            // Construct ".endExpVertParam" block
            {
                m_pBuilder->SetInsertPoint(pEndExpVertParamBlock);

                m_pBuilder->CreateRetVoid();
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
    Value* pCullFlag = m_pBuilder->getFalse();

    // Skip culling if it is not requested
    if (EnableCulling() == false)
    {
        return pCullFlag;
    }

    auto pEsGsOffset0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.pEsGsOffsets01,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(16),
                                                    });
    auto pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, m_pBuilder->getInt32(2));

    auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.pEsGsOffsets01,
                                                        m_pBuilder->getInt32(16),
                                                        m_pBuilder->getInt32(16),
                                                    });
    auto pVertexId1 = m_pBuilder->CreateLShr(pEsGsOffset1, m_pBuilder->getInt32(2));

    auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.pEsGsOffsets23,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(16),
                                                    });
    auto pVertexId2 = m_pBuilder->CreateLShr(pEsGsOffset2, m_pBuilder->getInt32(2));

    Value* vertexId[3] = { pVertexId0, pVertexId1, pVertexId2 };
    Value* vertex[3] = { nullptr };

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);
    auto pRegionStart = m_pBuilder->getInt32(regionStart);

    for (uint32_t i = 0; i < 3; ++i)
    {
        Value* pLdsOffset = m_pBuilder->CreateMul(vertexId[i], m_pBuilder->getInt32(SizeOfVec4));
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pRegionStart);

        vertex[i] = m_pLdsManager->ReadValueFromLds(m_pContext->Floatx4Ty(), pLdsOffset);
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
        auto pRegionStart = m_pBuilder->getInt32(regionStart);

        for (uint32_t i = 0; i < 3; ++i)
        {
            Value* pLdsOffset = m_pBuilder->CreateMul(vertex[i], m_pBuilder->getInt32(SizeOfDword));
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pRegionStart);

            signMask[i] = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);
        }

        pCullFlag = DoCullDistanceCulling(pModule, pCullFlag, signMask[0], signMask[1], signMask[2], pInsertAtEnd);
    }

    return pCullFlag;
}

// =====================================================================================================================
// Requests that parameter cache space be allocated (send the message GS_ALLOC_REQ).
void NggPrimShader::DoParamCacheAllocRequest()
{
    // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
    Value* pM0 = m_pBuilder->CreateShl(m_nggFactor.pPrimCountInSubgroup, m_pBuilder->getInt32(12));
    pM0 = m_pBuilder->CreateOr(pM0, m_nggFactor.pVertCountInSubgroup);

    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, { m_pBuilder->getInt32(GS_ALLOC_REQ), pM0 });
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader.
void NggPrimShader::DoPrimitiveExport(
    Value* pCullFlag)       // [in] Cull flag indicating whether this primitive has been culled (could be null)
{
    const bool vertexCompact = (m_pNggControl->compactMode == NggCompactVertices);

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
        auto pEsGsOffset0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_pBuilder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.pEsGsOffsets01,
                                                            m_pBuilder->getInt32(0),
                                                            m_pBuilder->getInt32(16),
                                                        });
        Value* pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, m_pBuilder->getInt32(2));

        auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_pBuilder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.pEsGsOffsets01,
                                                            m_pBuilder->getInt32(16),
                                                            m_pBuilder->getInt32(16),
                                                        });
        Value* pVertexId1 = m_pBuilder->CreateLShr(pEsGsOffset1, m_pBuilder->getInt32(2));

        auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_pBuilder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.pEsGsOffsets23,
                                                            m_pBuilder->getInt32(0),
                                                            m_pBuilder->getInt32(16),
                                                        });
        Value* pVertexId2 = m_pBuilder->CreateLShr(pEsGsOffset2, m_pBuilder->getInt32(2));

        if (vertexCompact)
        {
            pVertexId0 = ReadCompactDataFromLds(m_pBuilder->getInt8Ty(),
                                                pVertexId0,
                                                LdsRegionCompactThreadIdInSubgroup);
            pVertexId0 = m_pBuilder->CreateZExt(pVertexId0, m_pBuilder->getInt32Ty());

            pVertexId1 = ReadCompactDataFromLds(m_pBuilder->getInt8Ty(),
                                                pVertexId1,
                                                LdsRegionCompactThreadIdInSubgroup);
            pVertexId1 = m_pBuilder->CreateZExt(pVertexId1, m_pBuilder->getInt32Ty());

            pVertexId2 = ReadCompactDataFromLds(m_pBuilder->getInt8Ty(),
                                                pVertexId2,
                                                LdsRegionCompactThreadIdInSubgroup);
            pVertexId2 = m_pBuilder->CreateZExt(pVertexId2, m_pBuilder->getInt32Ty());
        }

        pPrimData = m_pBuilder->CreateShl(pVertexId2, m_pBuilder->getInt32(10));
        pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId1);

        pPrimData = m_pBuilder->CreateShl(pPrimData, m_pBuilder->getInt32(10));
        pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId0);

        if (vertexCompact)
        {
            LLPC_ASSERT(pCullFlag != nullptr); // Must not be null
            const auto pNullPrim = m_pBuilder->getInt32(1u << 31);
            pPrimData = m_pBuilder->CreateSelect(pCullFlag, pNullPrim, pPrimData);
        }
    }

    auto pUndef = UndefValue::get(m_pBuilder->getInt32Ty());

    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                m_pBuilder->getInt32Ty(),
                                {
                                    m_pBuilder->getInt32(EXP_TARGET_PRIM),      // tgt
                                    m_pBuilder->getInt32(0x1),                  // en
                                    // src0 ~ src3
                                    pPrimData,
                                    pUndef,
                                    pUndef,
                                    pUndef,
                                    m_pBuilder->getTrue(),                      // done, must be set
                                    m_pBuilder->getFalse(),                     // vm
                                });
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

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".dummyAllocReq" block
    {
        m_pBuilder->SetInsertPoint(pDummyAllocReqBlock);

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

        primData.u32All = 0;
        primData.bits.vertCount = 1;
        primData.bits.primCount = 1;

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg,
                                    {},
                                    {
                                        m_pBuilder->getInt32(GS_ALLOC_REQ),
                                        m_pBuilder->getInt32(primData.u32All)
                                    });

        m_pBuilder->CreateBr(pEndDummyAllocReqBlock);
    }

    // Construct ".endDummyAllocReq" block
    {
        m_pBuilder->SetInsertPoint(pEndDummyAllocReqBlock);

        auto pFirstThreadInSubgroup =
            m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(0));
        m_pBuilder->CreateCondBr(pFirstThreadInSubgroup, pDummyExpPrimBlock, pEndDummyExpPrimBlock);
    }

    // Construct ".dummyExpPrim" block
    {
        m_pBuilder->SetInsertPoint(pDummyExpPrimBlock);

        auto pUndef = UndefValue::get(m_pBuilder->getFloatTy());

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                    m_pBuilder->getFloatTy(),
                                    {
                                        m_pBuilder->getInt32(EXP_TARGET_POS_0),         // tgt
                                        m_pBuilder->getInt32(0x0),                      // en
                                        // src0 ~ src3
                                        pUndef,
                                        pUndef,
                                        pUndef,
                                        pUndef,
                                        m_pBuilder->getTrue(),                          // done
                                        m_pBuilder->getFalse()                          // vm
                                    });

        pUndef = UndefValue::get(m_pBuilder->getInt32Ty());

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                    m_pBuilder->getInt32Ty(),
                                    {
                                        m_pBuilder->getInt32(EXP_TARGET_PRIM),          // tgt
                                        m_pBuilder->getInt32(0x1),                      // en
                                        // src0 ~ src3
                                        m_pBuilder->getInt32(0),
                                        pUndef,
                                        pUndef,
                                        pUndef,
                                        m_pBuilder->getTrue(),                          // done
                                        m_pBuilder->getFalse()                          // vm
                                    });

        m_pBuilder->CreateBr(pEndDummyExpPrimBlock);
    }

    // Construct ".endDummyExpPrim" block
    {
        m_pBuilder->SetInsertPoint(pEndDummyExpPrimBlock);

        m_pBuilder->CreateRetVoid();
    }

    m_pBuilder->restoreIP(savedInsertPoint);

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
    Value* pTessCoordX    = UndefValue::get(m_pBuilder->getFloatTy());
    Value* pTessCoordY    = UndefValue::get(m_pBuilder->getFloatTy());
    Value* pRelPatchId    = UndefValue::get(m_pBuilder->getInt32Ty());
    Value* pPatchId       = UndefValue::get(m_pBuilder->getInt32Ty());

    Value* pVertexId      = UndefValue::get(m_pBuilder->getInt32Ty());
    Value* pRelVertexId   = UndefValue::get(m_pBuilder->getInt32Ty());
    Value* pVsPrimitiveId = UndefValue::get(m_pBuilder->getInt32Ty());
    Value* pInstanceId    = UndefValue::get(m_pBuilder->getInt32Ty());

    if (sysValueFromLds)
    {
        // NOTE: For vertex compaction, system values are from LDS compaction data region rather than from VGPRs.
        LLPC_ASSERT(m_pNggControl->compactMode == NggCompactVertices);

        const auto pResUsage = m_pContext->GetShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

        if (hasTs)
        {
            if (pResUsage->builtInUsage.tes.tessCoord)
            {
                pTessCoordX = ReadCompactDataFromLds(m_pBuilder->getFloatTy(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactTessCoordX);

                pTessCoordY = ReadCompactDataFromLds(m_pBuilder->getFloatTy(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactTessCoordY);
            }

            pRelPatchId = ReadCompactDataFromLds(m_pBuilder->getInt32Ty(),
                                                 m_nggFactor.pThreadIdInSubgroup,
                                                 LdsRegionCompactRelPatchId);

            if (pResUsage->builtInUsage.tes.primitiveId)
            {
                pPatchId = ReadCompactDataFromLds(m_pBuilder->getInt32Ty(),
                                                  m_nggFactor.pThreadIdInSubgroup,
                                                  LdsRegionCompactPatchId);
            }
        }
        else
        {
            if (pResUsage->builtInUsage.vs.vertexIndex)
            {
                pVertexId = ReadCompactDataFromLds(m_pBuilder->getInt32Ty(),
                                                   m_nggFactor.pThreadIdInSubgroup,
                                                   LdsRegionCompactVertexId);
            }

            // NOTE: Relative vertex ID Will not be used when VS is merged to GS.

            if (pResUsage->builtInUsage.vs.primitiveId)
            {
                pVsPrimitiveId = ReadCompactDataFromLds(m_pBuilder->getInt32Ty(),
                                                        m_nggFactor.pThreadIdInSubgroup,
                                                        LdsRegionCompactPrimId);
            }

            if (pResUsage->builtInUsage.vs.instanceIndex)
            {
                pInstanceId = ReadCompactDataFromLds(m_pBuilder->getInt32Ty(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactInstanceId);
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

            std::vector<uint32_t> shuffleMask;
            for (uint32_t i = 0; i < userDataSize; ++i)
            {
                shuffleMask.push_back(userDataIdx + i);
            }

            userDataIdx += userDataSize;

            auto pEsUserData = m_pBuilder->CreateShuffleVector(pUserData, pUserData, shuffleMask);
            args.push_back(pEsUserData);
        }
        else
        {
            LLPC_ASSERT(pEsArgTy->isIntegerTy());

            auto pEsUserData = m_pBuilder->CreateExtractElement(pUserData, userDataIdx);
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
            Value* pExpValue = m_pBuilder->CreateExtractValue(pExpData, i);
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

    auto savedInsertPos = m_pBuilder->saveIP();
    m_pBuilder->SetInsertPoint(pRetBlock);

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
                        expValue[0] = m_pBuilder->CreateBitCast(expValue[0], m_pBuilder->getFloatTy());
                        expValue[1] = m_pBuilder->CreateBitCast(expValue[1], m_pBuilder->getFloatTy());
                        expValue[2] = m_pBuilder->CreateBitCast(expValue[2], m_pBuilder->getFloatTy());
                        expValue[3] = m_pBuilder->CreateBitCast(expValue[3], m_pBuilder->getFloatTy());
                    }

                    Value* pExpValue = UndefValue::get(m_pContext->Floatx4Ty());
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        pExpValue = m_pBuilder->CreateInsertElement(pExpValue, expValue[i], i);
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
        pExpData = m_pBuilder->CreateInsertValue(pExpData, expData.pExpValue, i++);
        expData.pExpValue = nullptr;
    }

    // Insert new "return" instruction
    m_pBuilder->CreateRet(pExpData);

    // Clear export calls
    for (auto pExpCall : expCalls)
    {
        pExpCall->dropAllReferences();
        pExpCall->eraseFromParent();
    }

    m_pBuilder->restoreIP(savedInsertPos);

    return pEsEntryVariant;
}

// =====================================================================================================================
// Reads the specified data from NGG compaction data region in LDS.
Value* NggPrimShader::ReadCompactDataFromLds(
    Type*             pReadDataTy,  // [in] Data written to LDS
    Value*            pThreadId,    // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType  region)       // NGG compaction data region
{
    auto sizeInBytes = pReadDataTy->getPrimitiveSizeInBits() / 8;

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(region);

    Value* pLdsOffset = nullptr;
    if (sizeInBytes > 1)
    {
        pLdsOffset = m_pBuilder->CreateMul(pThreadId, m_pBuilder->getInt32(sizeInBytes));
    }
    else
    {
        pLdsOffset = pThreadId;
    }
    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

    return m_pLdsManager->ReadValueFromLds(pReadDataTy, pLdsOffset);
}

// =====================================================================================================================
// Writes the specified data to NGG compaction data region in LDS.
void NggPrimShader::WriteCompactDataToLds(
    Value*           pWriteData,        // [in] Data written to LDS
    Value*           pThreadId,         // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType region)            // NGG compaction data region
{
    auto pWriteDataTy = pWriteData->getType();
    auto sizeInBytes = pWriteDataTy->getPrimitiveSizeInBits() / 8;

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(region);

    Value* pLdsOffset = nullptr;
    if (sizeInBytes > 1)
    {
        pLdsOffset = m_pBuilder->CreateMul(pThreadId, m_pBuilder->getInt32(sizeInBytes));
    }
    else
    {
        pLdsOffset = pThreadId;
    }
    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

    m_pLdsManager->WriteValueToLds(pWriteData, pLdsOffset);
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
        pPaSuScModeCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paSuScModeCntl);
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
    args.push_back(m_pBuilder->getInt32(m_pNggControl->backfaceExponent));
    args.push_back(pPaSuScModeCntl);
    args.push_back(pPaClVportXscale);
    args.push_back(pPaClVportYscale);

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadNone);

    pCullFlag = EmitCall(pModule,
                         LlpcName::NggCullingBackface,
                         m_pBuilder->getInt1Ty(),
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
        pPaClClipCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
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
                         m_pBuilder->getInt1Ty(),
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
    Value* pPaClVteCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

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
        pPaClClipCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
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
                         m_pBuilder->getInt1Ty(),
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
    Value* pPaClVteCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

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
        pPaClClipCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
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
                         m_pBuilder->getInt1Ty(),
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
    Value* pPaClVteCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

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
                         m_pBuilder->getInt1Ty(),
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
                         m_pBuilder->getInt1Ty(),
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
    args.push_back(m_pBuilder->getInt32(regOffset));

    std::vector<Attribute::AttrKind> attribs;
    attribs.push_back(Attribute::ReadOnly);

    auto pRegValue = EmitCall(pModule,
                              LlpcName::NggCullingFetchReg,
                              m_pBuilder->getInt32Ty(),
                              args,
                              attribs,
                              pInsertAtEnd);

    return pRegValue;
}

// =====================================================================================================================
// Output a subgroup ballot (always return i64 mask)
Value* NggPrimShader::DoSubgroupBallot(
    Value* pValue) // [in] The value to do the ballot on.
{
    LLPC_ASSERT(pValue->getType()->isIntegerTy(1));

    const uint32_t waveSize = m_pContext->GetShaderWaveSize(ShaderStageGeometry);
    LLPC_ASSERT((waveSize == 32) || (waveSize == 64));

    pValue = m_pBuilder->CreateSelect(pValue, m_pBuilder->getInt32(1), m_pBuilder->getInt32(0));

    auto pInlineAsmTy = FunctionType::get(m_pBuilder->getInt32Ty(), m_pBuilder->getInt32Ty(), false);
    auto pInlineAsm = InlineAsm::get(pInlineAsmTy, "; %1", "=v,0", true);
    pValue = m_pBuilder->CreateCall(pInlineAsm, pValue);

    static const uint32_t PredicateNE = 33; // 33 = predicate NE
    Value* pBallot = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_icmp,
                                                 {
                                                     m_pBuilder->getIntNTy(waveSize),  // Return type
                                                     m_pBuilder->getInt32Ty()          // Argument type
                                                 },
                                                 {
                                                     pValue,
                                                     m_pBuilder->getInt32(0),
                                                     m_pBuilder->getInt32(PredicateNE)
                                                 });

    if (waveSize == 32)
    {
        pBallot = m_pBuilder->CreateZExt(pBallot, m_pBuilder->getInt64Ty());
    }

    return pBallot;
}

} // Llpc
