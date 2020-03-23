/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @brief LLPC source file: contains implementation of class lgc::NggPrimShader.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llpcGfx9Chip.h"
#include "llpcNggLdsManager.h"
#include "llpcNggPrimShader.h"
#include "llpcPassManager.h"
#include "llpcShaderMerger.h"

#define DEBUG_TYPE "llpc-ngg-prim-shader"

using namespace llvm;

namespace lgc
{

// =====================================================================================================================
NggPrimShader::NggPrimShader(
    PipelineState*  pPipelineState) // [in] Pipeline state
    :
    m_pPipelineState(pPipelineState),
    m_pContext(&pPipelineState->GetContext()),
    m_gfxIp(pPipelineState->GetTargetInfo().GetGfxIpVersion()),
    m_pNggControl(m_pPipelineState->GetNggControl()),
    m_pLdsManager(nullptr),
    m_pBuilder(new IRBuilder<>(*m_pContext))
{
    assert(m_pPipelineState->IsGraphics());

    memset(&m_nggFactor, 0, sizeof(m_nggFactor));

    m_hasVs = m_pPipelineState->HasShaderStage(ShaderStageVertex);
    m_hasTcs = m_pPipelineState->HasShaderStage(ShaderStageTessControl);
    m_hasTes = m_pPipelineState->HasShaderStage(ShaderStageTessEval);
    m_hasGs = m_pPipelineState->HasShaderStage(ShaderStageGeometry);
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
    Function*  pEsEntryPoint,           // [in] Entry-point of hardware export shader (ES) (could be null)
    Function*  pGsEntryPoint,           // [in] Entry-point of hardware geometry shader (GS) (could be null)
    Function*  pCopyShaderEntryPoint)   // [in] Entry-point of hardware vertex shader (VS, copy shader) (could be null)
{
    assert(m_gfxIp.major >= 10);

    // ES and GS could not be null at the same time
    assert(((pEsEntryPoint == nullptr) && (pGsEntryPoint == nullptr)) == false);

    Module* pModule = nullptr;
    if (pEsEntryPoint != nullptr)
    {
        pModule = pEsEntryPoint->getParent();
        pEsEntryPoint->setName(lgcName::NggEsEntryPoint);
        pEsEntryPoint->setCallingConv(CallingConv::C);
        pEsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        pEsEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    if (pGsEntryPoint != nullptr)
    {
        pModule = pGsEntryPoint->getParent();
        pGsEntryPoint->setName(lgcName::NggGsEntryPoint);
        pGsEntryPoint->setCallingConv(CallingConv::C);
        pGsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        pGsEntryPoint->addFnAttr(Attribute::AlwaysInline);

        assert(pCopyShaderEntryPoint != nullptr); // Copy shader must be present
        pCopyShaderEntryPoint->setName(lgcName::NggCopyShaderEntryPoint);
        pCopyShaderEntryPoint->setCallingConv(CallingConv::C);
        pCopyShaderEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        pCopyShaderEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    // Create NGG LDS manager
    assert(pModule != nullptr);
    assert(m_pLdsManager == nullptr);
    m_pLdsManager = new NggLdsManager(pModule, m_pPipelineState, m_pBuilder.get());

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

    const auto pGsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry);
    const auto pTesIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageTessEval);
    const auto pVsIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageVertex);

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

                assert(pTesIntfData->userDataUsage.tes.viewIndex == pGsIntfData->userDataUsage.gs.viewIndex);
                if ((pGsIntfData->spillTable.sizeInDwords > 0) &&
                    (pTesIntfData->spillTable.sizeInDwords == 0))
                {
                    pTesIntfData->userDataUsage.spillTable = userDataCount;
                    ++userDataCount;
                    assert(userDataCount <= m_pPipelineState->GetTargetInfo().GetGpuProperty().maxUserDataCount);
                }
            }
        }
        else
        {
            if (m_hasVs)
            {
                userDataCount = std::max(pVsIntfData->userDataCount, userDataCount);

                assert(pVsIntfData->userDataUsage.vs.viewIndex == pGsIntfData->userDataUsage.gs.viewIndex);
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

    assert(userDataCount > 0);
    argTys.push_back(VectorType::get(m_pBuilder->getInt32Ty(), userDataCount));
    *pInRegMask |= (1ull << EsGsSpecialSysValueCount);

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
    uint64_t inRegMask = 0;
    auto pEntryPointTy = GeneratePrimShaderEntryPointType(&inRegMask);

    Function* pEntryPoint = Function::Create(pEntryPointTy,
                                             GlobalValue::ExternalLinkage,
                                             lgcName::NggPrimShaderEntryPoint);

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

    Value* pUserDataAddrLow         = (pArg + EsGsSysValueUserDataAddrLow);
    Value* pUserDataAddrHigh        = (pArg + EsGsSysValueUserDataAddrHigh);
    Value* pMergedGroupInfo         = (pArg + EsGsSysValueMergedGroupInfo);
    Value* pMergedWaveInfo          = (pArg + EsGsSysValueMergedWaveInfo);
    Value* pOffChipLdsBase          = (pArg + EsGsSysValueOffChipLdsBase);
    Value* pSharedScratchOffset     = (pArg + EsGsSysValueSharedScratchOffset);
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

    pUserDataAddrLow->setName("userDataAddrLow");
    pUserDataAddrHigh->setName("userDataAddrHigh");
    pMergedGroupInfo->setName("mergedGroupInfo");
    pMergedWaveInfo->setName("mergedWaveInfo");
    pOffChipLdsBase->setName("offChipLdsBase");
    pSharedScratchOffset->setName("sharedScratchOffset");
    pPrimShaderTableAddrLow->setName("primShaderTableAddrLow");
    pPrimShaderTableAddrHigh->setName("primShaderTableAddrHigh");

    pUserData->setName("userData");
    pEsGsOffsets01->setName("esGsOffsets01");
    pEsGsOffsets23->setName("esGsOffsets23");
    pGsPrimitiveId->setName("gsPrimitiveId");
    pInvocationId->setName("invocationId");
    pEsGsOffsets45->setName("esGsOffsets45");

    if (m_hasTes)
    {
        pTessCoordX->setName("tessCoordX");
        pTessCoordY->setName("tessCoordY");
        pRelPatchId->setName("relPatchId");
        pPatchId->setName("patchId");
    }
    else
    {
        pVertexId->setName("vertexId");
        pRelVertexId->setName("relVertexId");
        pVsPrimitiveId->setName("vsPrimitiveId");
        pInstanceId->setName("instanceId");
    }

    if (m_hasGs)
    {
        // GS is present in primitive shader (ES-GS merged shader)
        ConstructPrimShaderWithGs(pModule);
    }
    else
    {
        // GS is not present in primitive shader (ES-only shader)
        ConstructPrimShaderWithoutGs(pModule);
    }

    return pEntryPoint;
}

// =====================================================================================================================
// Constructs primitive shader for ES-only merged shader (GS is not present).
void NggPrimShader::ConstructPrimShaderWithoutGs(
    Module* pModule) // [in] LLVM module
{
    assert(m_hasGs == false);

    const bool hasTs = (m_hasTcs || m_hasTes);

    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    const uint32_t waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

    auto pEntryPoint = pModule->getFunction(lgcName::NggPrimShaderEntryPoint);

    auto pArg = pEntryPoint->arg_begin();

    Value* pMergedGroupInfo = (pArg + EsGsSysValueMergedGroupInfo);
    Value* pMergedWaveInfo = (pArg + EsGsSysValueMergedWaveInfo);
    Value* pPrimShaderTableAddrLow = (pArg + EsGsSysValuePrimShaderTableAddrLow);
    Value* pPrimShaderTableAddrHigh = (pArg + EsGsSysValuePrimShaderTableAddrHigh);

    pArg += (EsGsSpecialSysValueCount + 1);

    Value* pEsGsOffsets01 = pArg;
    Value* pEsGsOffsets23 = (pArg + 1);
    Value* pGsPrimitiveId = (pArg + 2);

    Value* pTessCoordX = (pArg + 5);
    Value* pTessCoordY = (pArg + 6);
    Value* pRelPatchId = (pArg + 7);
    Value* pPatchId = (pArg + 8);

    Value* pVertexId = (pArg + 5);
    Value* pInstanceId = (pArg + 8);

    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

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
        //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8)
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
        auto pEntryBlock = CreateBlock(pEntryPoint, ".entry");

        // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
        BasicBlock* pWritePrimIdBlock = nullptr;
        BasicBlock* pEndWritePrimIdBlock = nullptr;
        BasicBlock* pReadPrimIdBlock = nullptr;
        BasicBlock* pEndReadPrimIdBlock = nullptr;

        if (distributePrimId)
        {
            pWritePrimIdBlock = CreateBlock(pEntryPoint, ".writePrimId");
            pEndWritePrimIdBlock = CreateBlock(pEntryPoint, ".endWritePrimId");

            pReadPrimIdBlock = CreateBlock(pEntryPoint, ".readPrimId");
            pEndReadPrimIdBlock = CreateBlock(pEntryPoint, ".endReadPrimId");
        }

        auto pAllocReqBlock = CreateBlock(pEntryPoint, ".allocReq");
        auto pEndAllocReqBlock = CreateBlock(pEntryPoint, ".endAllocReq");

        auto pExpPrimBlock = CreateBlock(pEntryPoint, ".expPrim");
        auto pEndExpPrimBlock = CreateBlock(pEntryPoint, ".endExpPrim");

        auto pExpVertBlock = CreateBlock(pEntryPoint, ".expVert");
        auto pEndExpVertBlock = CreateBlock(pEntryPoint, ".endExpVert");

        // Construct ".entry" block
        {
            m_pBuilder->SetInsertPoint(pEntryBlock);

            InitWaveThreadInfo(pMergedGroupInfo, pMergedWaveInfo);

            // Record ES-GS vertex offsets info
            m_nggFactor.pEsGsOffsets01 = pEsGsOffsets01;

            if (distributePrimId)
            {
                auto pPrimValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
                m_pBuilder->CreateCondBr(pPrimValid, pWritePrimIdBlock, pEndWritePrimIdBlock);
            }
            else
            {
                m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto pFirstWaveInSubgroup =
                    m_pBuilder->CreateICmpEQ(m_nggFactor.pWaveIdInSubgroup, m_pBuilder->getInt32(0));
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

                auto pLdsOffset = m_pBuilder->CreateShl(pVertexId0, 2);
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

                auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, 2);
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
                             lgcName::NggEsEntryPoint,
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
        //                     @llpc.ngg.ES.variant(%sgpr..., %userData..., %vgpr...)
        //     br label %.endWritePosData
        //
        // .endWritePosData:
        //     call void @llvm.amdgcn.s.barrier()
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
        // <if (vertexCompact)>
        // [
        //     call void @llvm.amdgcn.s.barrier()
        // ]
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
        //     call void @llvm.amdgcn.s.barrier()
        //
        // <if (vertexCompact)>
        // [
        //     br lable %.readThreadCount
        //
        // .readThreadCount:
        //     %vertCountInWaves = ... (read LDS region, vertex count in waves)
        //     %threadCountInWaves = %vertCountInWaves
        //
        //     %vertValid = icmp ult i32 %threadIdInWave , %vertCountInWave
        //     %compactDataWrite = and i1 %vertValid, %drawFlag
        //     br i1 %compactDataWrite, label %.writeCompactData, label %.endReadThreadCount
        //
        // .writeCompactData:
        //     ; Write LDS region (compaction data: compacted thread ID, vertex position data,
        //     ; vertex ID/tessCoordX, instance ID/tessCoordY, primitive ID/relative patch ID, patch ID)
        //     br label %.endReadThreadCount
        //
        // .endReadThreadCount:
        //     %hasSurviveVert = icmp ne i32 %vertCountInWaves, 0
        //     %primCountInSubgroup =
        //         select i1 %hasSurviveVert, i32 %primCountInSubgroup, i32 %fullyCulledThreadCount
        //     %vertCountInSubgroup =
        //         select i1 %hasSurviveVert, i32 %vertCountInWaves, i32 %fullyCulledThreadCount
        //
        //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
        //     br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
        // ]
        // <else>
        // [
        //     %firstThreadInWave = icmp eq i32 %threadIdInWave, 0
        //     br i1 %firstThreadInWave, label %.readThreadCount, label %.endReadThreadCount
        //
        // .readThreadCount:
        //     %primCount = ... (read LDS region, primitive count in waves)
        //     %threadCountInWaves = %primCount
        //
        //     br label %.endReadThreadCount
        //
        // .endReadThreadCount:
        //     %primCount = phi i32 [ primCountInSubgroup, %.endAccPrimCount ], [ %primCount, %.readThreadCount ]
        //     %hasSurvivePrim = icmp ne i32 %primCount, 0
        //     %primCountInSubgroup =
        //         select i1 %hasSurvivePrim, i32 %primCountInSubgroup, i32 %fullyCulledThreadCount
        //     %hasSurvivePrim = icmp ne i32 %primCountInSubgroup, 0
        //     %vertCountInSubgroup =
        //         select i1 %hasSurvivePrim, i32 %vertCountInSubgroup, i32 %fullyCulledThreadCount
        //
        //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
        //     br i1 %firstWaveInSubgroup, label %.allocreq, label %.endAllocReq
        // ]
        // .allocReq:
        //     ; Do parameter cache (PC) alloc request: s_sendmsg(GS_ALLOC_REQ, ...)
        //     br label %.endAllocReq
        //
        // .endAlloReq:
        // <if (vertexCompact)>
        // [
        //     call void @llvm.amdgcn.s.barrier()
        // ]
        //     %noSurviveThread = icmp eq %threadCountInWaves, 0
        //     br i1 %noSurviveThread, label %.earlyExit, label %.noEarlyExit
        //
        // .earlyExit:
        //     %firstThreadInSubgroup = icmp eq i32 %threadIdInSubgroup, 0
        //     br i1 %firstThreadInSubgroup, label %.dummyExp, label %.endDummyExp
        //
        // .dummyExp:
        //     ; Do vertex position export: exp pos, ... (off, off, off, off)
        //     ; Do primitive export: exp prim, ... (0, off, off, off)
        //     br label %.endDummyExp
        //
        // .endDummyExp:
        //     ret void
        //
        // .noEarlyExit:
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

        // Thread count when the entire sub-group is fully culled
        const uint32_t fullyCulledThreadCount =
            m_pPipelineState->GetTargetInfo().GetGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups ? 1 : 0;

        // Define basic blocks
        auto pEntryBlock = CreateBlock(pEntryPoint, ".entry");

        // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
        BasicBlock* pWritePrimIdBlock = nullptr;
        BasicBlock* pEndWritePrimIdBlock = nullptr;
        BasicBlock* pReadPrimIdBlock = nullptr;
        BasicBlock* pEndReadPrimIdBlock = nullptr;

        if (distributePrimId)
        {
            pWritePrimIdBlock = CreateBlock(pEntryPoint, ".writePrimId");
            pEndWritePrimIdBlock = CreateBlock(pEntryPoint, ".endWritePrimId");

            pReadPrimIdBlock = CreateBlock(pEntryPoint, ".readPrimId");
            pEndReadPrimIdBlock = CreateBlock(pEntryPoint, ".endReadPrimId");
        }

        auto pZeroThreadCountBlock = CreateBlock(pEntryPoint, ".zeroThreadCount");
        auto pEndZeroThreadCountBlock = CreateBlock(pEntryPoint, ".endZeroThreadCount");

        auto pZeroDrawFlagBlock = CreateBlock(pEntryPoint, ".zeroDrawFlag");
        auto pEndZeroDrawFlagBlock = CreateBlock(pEntryPoint, ".endZeroDrawFlag");

        auto pWritePosDataBlock = CreateBlock(pEntryPoint, ".writePosData");
        auto pEndWritePosDataBlock = CreateBlock(pEntryPoint, ".endWritePosData");

        auto pCullingBlock = CreateBlock(pEntryPoint, ".culling");
        auto pEndCullingBlock = CreateBlock(pEntryPoint, ".endCulling");

        auto pWriteDrawFlagBlock = CreateBlock(pEntryPoint, ".writeDrawFlag");
        auto pEndWriteDrawFlagBlock = CreateBlock(pEntryPoint, ".endWriteDrawFlag");

        auto pAccThreadCountBlock = CreateBlock(pEntryPoint, ".accThreadCount");
        auto pEndAccThreadCountBlock = CreateBlock(pEntryPoint, ".endAccThreadCount");

        // NOTE: Those basic blocks are conditionally created on the basis of actual NGG compaction mode.
        BasicBlock* pReadThreadCountBlock = nullptr;
        BasicBlock* pWriteCompactDataBlock = nullptr;
        BasicBlock* pEndReadThreadCountBlock = nullptr;

        if (vertexCompact)
        {
            pReadThreadCountBlock = CreateBlock(pEntryPoint, ".readThreadCount");
            pWriteCompactDataBlock = CreateBlock(pEntryPoint, ".writeCompactData");
            pEndReadThreadCountBlock = CreateBlock(pEntryPoint, ".endReadThreadCount");
        }
        else
        {
            pReadThreadCountBlock = CreateBlock(pEntryPoint, ".readThreadCount");
            pEndReadThreadCountBlock = CreateBlock(pEntryPoint, ".endReadThreadCount");
        }

        auto pAllocReqBlock = CreateBlock(pEntryPoint, ".allocReq");
        auto pEndAllocReqBlock = CreateBlock(pEntryPoint, ".endAllocReq");

        auto pEarlyExitBlock = CreateBlock(pEntryPoint, ".earlyExit");
        auto pNoEarlyExitBlock = CreateBlock(pEntryPoint, ".noEarlyExit");

        auto pExpPrimBlock = CreateBlock(pEntryPoint, ".expPrim");
        auto pEndExpPrimBlock = CreateBlock(pEntryPoint, ".endExpPrim");

        auto pExpVertPosBlock = CreateBlock(pEntryPoint, ".expVertPos");
        auto pEndExpVertPosBlock = CreateBlock(pEntryPoint, ".endExpVertPos");

        auto pExpVertParamBlock = CreateBlock(pEntryPoint, ".expVertParam");
        auto pEndExpVertParamBlock = CreateBlock(pEntryPoint, ".endExpVertParam");

        // Construct ".entry" block
        {
            m_pBuilder->SetInsertPoint(pEntryBlock);

            InitWaveThreadInfo(pMergedGroupInfo, pMergedWaveInfo);

            // Record primitive shader table address info
            m_nggFactor.pPrimShaderTableAddrLow  = pPrimShaderTableAddrLow;
            m_nggFactor.pPrimShaderTableAddrHigh = pPrimShaderTableAddrHigh;

            // Record ES-GS vertex offsets info
            m_nggFactor.pEsGsOffsets01  = pEsGsOffsets01;
            m_nggFactor.pEsGsOffsets23  = pEsGsOffsets23;

            if (distributePrimId)
            {
                auto pPrimValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
                m_pBuilder->CreateCondBr(pPrimValid, pWritePrimIdBlock, pEndWritePrimIdBlock);
            }
            else
            {
                auto pFirstThreadInSubgroup =
                    m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(0));
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

                auto pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, 2);

                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDistribPrimId);

                auto pLdsOffset = m_pBuilder->CreateShl(pVertexId0, 2);
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

                auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, 2);
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
                m_pBuilder->CreateCondBr(pFirstThreadInSubgroup, pZeroThreadCountBlock, pEndZeroThreadCountBlock);
            }
        }

        // Construct ".zeroThreadCount" block
        {
            m_pBuilder->SetInsertPoint(pZeroThreadCountBlock);

            uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(
                vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);

            auto pZero = m_pBuilder->getInt32(0);

            // Zero per-wave primitive/vertex count
            auto pZeros = ConstantVector::getSplat({Gfx9::NggMaxWavesPerSubgroup, false}, pZero);

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

            Value* pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInWave, 2);

            uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);

            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

            auto pZero = m_pBuilder->getInt32(0);
            m_pLdsManager->WriteValueToLds(pZero, pLdsOffset);

            if (waveCountInSubgroup == 8)
            {
                assert(waveSize == 32);
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
            const auto entryName = (separateExp || vertexCompact) ? lgcName::NggEsEntryVariantPos :
                                                                    lgcName::NggEsEntryVariant;

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
                    assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation

                    Value* pLdsOffset =
                        m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(SizeOfVec4));
                    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                    // Use 128-bit LDS store
                    m_pLdsManager->WriteValueToLds(expData.pExpValue, pLdsOffset, true);

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
                assert(clipCullDistance.size() < MaxClipCullDistanceCount);

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
                    pSignBit = m_pBuilder->CreateShl(pSignBit, i);

                    pSignMask = m_pBuilder->CreateOr(pSignMask, pSignBit);
                }

                // Write the sign mask to LDS
                const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionCullDistance);

                Value* pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, 2);
                pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                m_pLdsManager->WriteValueToLds(pSignMask, pLdsOffset);
            }

            m_pBuilder->CreateBr(pEndWritePosDataBlock);
        }

        // Construct ".endWritePosData" block
        {
            m_pBuilder->SetInsertPoint(pEndWritePosDataBlock);

            auto pUndef = UndefValue::get(VectorType::get(Type::getFloatTy(*m_pContext), 4));
            for (auto& expData : expDataSet)
            {
                PHINode* pExpValue = m_pBuilder->CreatePHI(VectorType::get(Type::getFloatTy(*m_pContext), 4), 2);
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

            pDoCull = DoCulling(pModule);
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
            auto pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, 2);

            auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_pBuilder->getInt32Ty(),
                                                            {
                                                                pEsGsOffsets01,
                                                                m_pBuilder->getInt32(16),
                                                                m_pBuilder->getInt32(16)
                                                            });
            auto pVertexId1 = m_pBuilder->CreateLShr(pEsGsOffset1, 2);

            auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_pBuilder->getInt32Ty(),
                                                            {
                                                                pEsGsOffsets23,
                                                                m_pBuilder->getInt32(0),
                                                                m_pBuilder->getInt32(16)
                                                            });
            auto pVertexId2 = m_pBuilder->CreateLShr(pEsGsOffset2, 2);

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

            m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

            if (vertexCompact)
            {
                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionDrawFlag);

                auto pLdsOffset =
                    m_pBuilder->CreateAdd(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(regionStart));

                pDrawFlag = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt8Ty(), pLdsOffset);
                pDrawFlag = m_pBuilder->CreateTrunc(pDrawFlag, m_pBuilder->getInt1Ty());
            }

            auto pDrawMask = DoSubgroupBallot(pDrawFlag);

            pDrawCount = m_pBuilder->CreateIntrinsic(Intrinsic::ctpop, m_pBuilder->getInt64Ty(), pDrawMask);
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

            m_pBuilder->CreateCondBr(pPrimCountAcc, pAccThreadCountBlock, pEndAccThreadCountBlock);
        }

        // Construct ".accThreadCount" block
        {
            m_pBuilder->SetInsertPoint(pAccThreadCountBlock);

            auto pLdsOffset = m_pBuilder->CreateAdd(m_nggFactor.pWaveIdInSubgroup, m_nggFactor.pThreadIdInWave);
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(1));
            pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, 2);

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

                m_pBuilder->CreateCondBr(pFirstThreadInWave, pReadThreadCountBlock, pEndReadThreadCountBlock);
            }
        }

        Value* pThreadCountInWaves = nullptr;
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
                pThreadCountInWaves = pVertCountInWaves;

                // Get vertex count for all waves prior to this wave
                pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pWaveIdInSubgroup, 2);
                pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(regionStart), pLdsOffset);

                pVertCountInPrevWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

                auto pVertValid =
                    m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pVertCountInWave);

                auto pCompactDataWrite = m_pBuilder->CreateAnd(pDrawFlag, pVertValid);

                m_pBuilder->CreateCondBr(pCompactDataWrite, pWriteCompactDataBlock, pEndReadThreadCountBlock);
            }

            // Construct ".writeCompactData" block
            {
                m_pBuilder->SetInsertPoint(pWriteCompactDataBlock);

                Value* pDrawMask = DoSubgroupBallot(pDrawFlag);
                pDrawMask = m_pBuilder->CreateBitCast(pDrawMask, VectorType::get(Type::getInt32Ty(*m_pContext), 2));

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
                WritePerThreadDataToLds(pCompactThreadId, m_nggFactor.pThreadIdInSubgroup, LdsRegionVertThreadIdMap);

                if (hasTs)
                {
                    // Write X/Y of tessCoord (U/V) to LDS
                    if (pResUsage->builtInUsage.tes.tessCoord)
                    {
                        WritePerThreadDataToLds(pTessCoordX, pCompactThreadIdInSubrgoup, LdsRegionCompactTessCoordX);
                        WritePerThreadDataToLds(pTessCoordY, pCompactThreadIdInSubrgoup, LdsRegionCompactTessCoordY);
                    }

                    // Write relative patch ID to LDS
                    WritePerThreadDataToLds(pRelPatchId, pCompactThreadIdInSubrgoup, LdsRegionCompactRelPatchId);

                    // Write patch ID to LDS
                    if (pResUsage->builtInUsage.tes.primitiveId)
                    {
                        WritePerThreadDataToLds(pPatchId, pCompactThreadIdInSubrgoup, LdsRegionCompactPatchId);
                    }
                }
                else
                {
                    // Write vertex ID to LDS
                    if (pResUsage->builtInUsage.vs.vertexIndex)
                    {
                        WritePerThreadDataToLds(pVertexId, pCompactThreadIdInSubrgoup, LdsRegionCompactVertexId);
                    }

                    // Write instance ID to LDS
                    if (pResUsage->builtInUsage.vs.instanceIndex)
                    {
                        WritePerThreadDataToLds(pInstanceId, pCompactThreadIdInSubrgoup, LdsRegionCompactInstanceId);
                    }

                    // Write primitive ID to LDS
                    if (pResUsage->builtInUsage.vs.primitiveId)
                    {
                        assert(m_nggFactor.pPrimitiveId != nullptr);
                        WritePerThreadDataToLds(m_nggFactor.pPrimitiveId,
                                                pCompactThreadIdInSubrgoup,
                                                LdsRegionCompactPrimId);
                    }
                }

                m_pBuilder->CreateBr(pEndReadThreadCountBlock);
            }

            // Construct ".endReadThreadCount" block
            {
                m_pBuilder->SetInsertPoint(pEndReadThreadCountBlock);

                Value* pHasSurviveVert = m_pBuilder->CreateICmpNE(pVertCountInWaves, m_pBuilder->getInt32(0));

                Value* pPrimCountInSubgroup =
                    m_pBuilder->CreateSelect(pHasSurviveVert,
                                             m_nggFactor.pPrimCountInSubgroup,
                                             m_pBuilder->getInt32(fullyCulledThreadCount));

                // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
                // as an uniform value later. This is similar to the provided primitive count in sub-group that is
                // a system value.
                pPrimCountInSubgroup =
                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pPrimCountInSubgroup);

                Value* pVertCountInSubgroup =
                    m_pBuilder->CreateSelect(pHasSurviveVert,
                                             pVertCountInWaves,
                                             m_pBuilder->getInt32(fullyCulledThreadCount));

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
                pThreadCountInWaves = pPrimCount;

                Value* pHasSurvivePrim = m_pBuilder->CreateICmpNE(pPrimCount, m_pBuilder->getInt32(0));

                Value* pPrimCountInSubgroup =
                    m_pBuilder->CreateSelect(pHasSurvivePrim,
                                             m_nggFactor.pPrimCountInSubgroup,
                                             m_pBuilder->getInt32(fullyCulledThreadCount));

                // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
                // as an uniform value later. This is similar to the provided primitive count in sub-group that is
                // a system value.
                pPrimCountInSubgroup =
                    m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pPrimCountInSubgroup);

                Value* pVertCountInSubgroup =
                    m_pBuilder->CreateSelect(pHasSurvivePrim,
                                             m_nggFactor.pVertCountInSubgroup,
                                             m_pBuilder->getInt32(fullyCulledThreadCount));

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

            m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

            auto pNoSurviveThread = m_pBuilder->CreateICmpEQ(pThreadCountInWaves, m_pBuilder->getInt32(0));
            m_pBuilder->CreateCondBr(pNoSurviveThread, pEarlyExitBlock, pNoEarlyExitBlock);
        }

        // Construct ".earlyExit" block
        {
            m_pBuilder->SetInsertPoint(pEarlyExitBlock);

            uint32_t expPosCount = 0;
            for (const auto& expData : expDataSet)
            {
                if ((expData.target >= EXP_TARGET_POS_0) && (expData.target <= EXP_TARGET_POS_4))
                {
                    ++expPosCount;
                }
            }

            DoEarlyExit(fullyCulledThreadCount, expPosCount);
        }

        // Construct ".noEarlyExit" block
        {
            m_pBuilder->SetInsertPoint(pNoEarlyExitBlock);

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
                                 lgcName::NggEsEntryVariant,
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
                        assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation

                        auto pLdsOffset =
                            m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(SizeOfVec4));
                        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

                        // Use 128-bit LDS load
                        auto pExpValue =
                            m_pLdsManager->ReadValueFromLds(VectorType::get(Type::getFloatTy(*m_pContext), 4),
                                                            pLdsOffset,
                                                            true);
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
                auto pUndef = UndefValue::get(VectorType::get(Type::getFloatTy(*m_pContext), 4));
                for (auto& expData : expDataSet)
                {
                    PHINode* pExpValue = m_pBuilder->CreatePHI(VectorType::get(Type::getFloatTy(*m_pContext), 4), 2);

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
                                     lgcName::NggEsEntryVariantParam,
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

// =====================================================================================================================
// Constructs primitive shader for ES-GS merged shader (GS is present).
void NggPrimShader::ConstructPrimShaderWithGs(
    Module* pModule) // [in] LLVM module
{
    assert(m_hasGs);

    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    const uint32_t waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    const uint32_t rasterStream = pResUsage->inOutUsage.gs.rasterStream;
    assert(rasterStream < MaxGsStreams);

    const auto& calcFactor = pResUsage->inOutUsage.gs.calcFactor;
    const uint32_t maxOutPrims = calcFactor.primAmpFactor;

    auto pEntryPoint = pModule->getFunction(lgcName::NggPrimShaderEntryPoint);

    auto pArg = pEntryPoint->arg_begin();

    Value* pMergedGroupInfo = (pArg + EsGsSysValueMergedGroupInfo);
    Value* pMergedWaveInfo = (pArg + EsGsSysValueMergedWaveInfo);

    pArg += (EsGsSpecialSysValueCount + 1);

    Value* pEsGsOffsets01 = pArg;
    Value* pEsGsOffsets23 = (pArg + 1);
    Value* pEsGsOffsets45 = (pArg + 4);

    // define dllexport amdgpu_gs @_amdgpu_gs_main(
    //     inreg i32 %sgpr0..7, inreg <n x i32> %userData, i32 %vgpr0..8)
    // {
    // .entry:
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
    //     %primCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 22, i32 9)
    //     %vertCountInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr2, i32 12, i32 9)
    //
    //     %primCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 8, i32 8)
    //     %vertCountInWave = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 0, i32 8)
    //
    //     %waveIdInSubgroup = call i32 @llvm.amdgcn.ubfe.i32(i32 %sgpr3, i32 24, i32 4)
    //     %threadIdInSubgroup = mul i32 %waveIdInSubgroup, %waveSize
    //     %threadIdInSubgroup = add i32 %threadIdInSubgroup, %threadIdInWave
    //
    //     %vertValid = icmp ult i32 %threadId, %vertCountInWave
    //     br i1 %vertValid, label %.begines, label %.endes
    //
    // .beginEs:
    //     call void @llpc.ngg.ES.main(%sgpr..., %userData..., %vgpr...)
    //     br label %.endes
    //
    // .endEs:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %primValid = icmp ult i32 %threadId, %primCountInWave
    //     br i1 %primValid, label %.initOutPrimData, label %.endInitOutPrimData
    //
    // .initOutPrimData:
    //     ; Initialize LDS region (GS output primitive data)
    //     br label %.endInitOutPrimData
    //
    // .endInitOutPrimData:
    //     %firstThreadInSubgroup = icmp eq i32 %threadIdInSubgroup, 0
    //     br i1 %firstThreadInSubgroup, label %.zeroOutVertCount, label %.endZeroOutVertCount
    //
    // .zeroOutVertCount:
    //     ; Zero LDS region (GS output vertex count in wave)
    //     br labe %endZeroOutVertCount
    //
    // .endZeroOutVertCount:
    //     %primValid = icmp ult i32 %threadId, %primCountInWave
    //     br i1 %primValid, label %.begings, label %.endgs
    //
    // .beginGs:
    //     %outPrimVertCountInfo = call { OUT_PRIM_COUNT: i32,
    //                                    OUT_VERT_COUNT: i32,
    //                                    INCLUSIVE_OUT_VERT_COUNT: i32,
    //                                    VERT_COUNT_IN_WAVE: i32 }
    //                           @llpc.ngg.GS.variant(%sgpr..., %userData..., %vgpr...)
    //     %outPrimCount          = extractvalue { i32, i32, i32 } %outPrimVertCountInfo, 0
    //     %outVertCount          = extractvalue { i32, i32, i32 } %outPrimVertCountInfo, 1
    //     %inclusiveOutVertCount = extractvalue { i32, i32, i32 } %outPrimVertCountInfo, 2
    //     %vertCountInWave       = extractvalue { i32, i32, i32 } %outPrimVertCountInfo, 3
    //
    //     br label %.endgs
    //
    // .endGs:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %hasSurviveVert = icmp ne i32 %vertCountInWave, 0
    //     %threadIdUpbound = sub i32 %waveCountInSubgroup, %waveIdInSubgroup
    //     %threadValid = icmp ult i32 %threadIdInWave, %threadIdUpbound
    //     %threadValid = and i1 %threadValid, %hasSurviveVert
    //     br i1 %threadValid, label %.accVertCount, label %..endAccVertCount
    //
    // .accVertCount:
    //     ; Write LDS region (GS output vertex count in waves)
    //     br label %.endAccVertCount
    //
    // .endAccVertCount:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %firstThreadInWave = icmp eq i32 %threadIdInWave, 0
    //     br i1 %firstThreadInWave, label %.readVertCount, label %.endReadVertCount
    //
    // .readVertCount:
    //     %vertCountInSubgroup = ... (read LDS region, GS output vertex count in waves)
    //     br label %.endReadVertCount
    //
    // .endReadVertCount:
    //     %firstWaveInSubgroup = icmp eq i32 %waveIdInSubgroup, 0
    //     br i1 %firstWaveInSubgroup, label %.allocReq, label %.endAllocReq
    //
    // .allocReq:
    //     ; Do parameter cache(PC) alloc request : s_sendmsg(GS_ALLOC_REQ, ...)
    //     br label %.endAllocReq
    //
    // .endAllocReq:
    //     %primValid = icmp ult i32 %threadIdInWave, %primCountInWave
    //     br i1 %primValid, label %.reviseOutPrimData, label %.reviseOutPrimDataLoop
    //
    // .reviseOutPrimData:
    //     %outVertCountInPrevWaves = ... (read LDS region, GS output vertex count in waves)
    //     %exclusiveOutVertCount = sub i32 %inclusiveOutVertCount, %outVertCount
    //     %vertexIdAdjust = %outVertCountInPrevWaves + %exclusiveOutVertCount
    //
    //     %adjustVertexId = icmp ne i32 %vertexIdAdjust, 0
    //     br i1 %adjustVertexId, label %.reviseOutPrimDataLoop, label %.endReviseOutPrimData
    //
    // .reviseOutPrimDataLoop:
    //     %outPrimId = phi i32 [ 0, %.reviseOutPrimData ],
    //                          [ %outPrimId, %.reviseOutPrimDataLoop ]
    //
    //     %primData = ... (read LDS region, GS output primitive data)
    //
    //     %vertexId0 = ... (primData[8:0])
    //     %vertexId0 = add i32 %vertexId0, %vertexIdAdjust
    //     %vertexId1 = ... (primData[18:10])
    //     %vertexId1 = add i32 %vertexId1, %vertexIdAdjust
    //     %vertexId2 = ... (primData[28:20])
    //     %vertexId2 = add i32 %vertexId2, %vertexIdAdjust
    //     %primData  = ... ((vertexId2 << 20) | (vertexId1 << 10) | vertexId0)
    //     ; Write LDS region (GS output primitive data)
    //
    //     %outPrimId = add i32 %outPrimId, 1
    //     %reviseContinue = icmp ult i32 %outPrimId, %outPrimCount
    //     br i1 %reviseContinue, label %.reviseOutPrimDataLoop, label %.endReviseOutPrimData
    //
    // .endReviseOutPrimData:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %primExp = icmp ult i32 %threadIdInSubgroup, %primCountInSubgroup
    //     br i1 %primExp, label %.expPrim, label %.endExpPrim
    //
    // .expPrim:
    //     ; Do primitive export: exp prim, ..
    //     br label %.endExpPrim
    //
    // .endExpPrim:
    //     %primValid = icmp ult i32 %threadIdInWave, %primCountInWave
    //     br i1 %primValid, label %.writeOutVertOffset, label %.endWriteOutVertOffset
    //
    // .writeOutVertOffset:
    //     %outVertCountInPrevWaves = ... (read LDS region, GS output vertex count in waves)
    //     %exclusiveOutVertCount = sub i32 %inclusiveOutVertCount, %outVertCount
    //     %outVertThreadId = %outVertCountInPrevWaves + %exclusiveOutVertCount
    //
    //     %writeOffset = ... (OutVertOffsetStart + outVertThreadId * 4)
    //     %writeValue = ... (GsVsRingStart + threadIdInSubgroup * gsVsRingItemSize)
    //
    //     br label %.writeOutVertOffsetLoop
    //
    // .writeOutVertOffsetLoop:
    //     %outVertIdInPrim = phi i32 [ 0, %.writeOutVertOffset ],
    //                                [ %outVertIdInPrim, %.writeOutVertOffsetLoop ]
    //
    //     %ldsOffset = ... (writeOffset + 4 * outVertIdInPrim)
    //     %vertexOffset = ... (writeValue + 4 * vertexSize * outVertIdInPrim)
    //     ; Write LDS region (GS output vertex offset)
    //
    //     %outVertIdInPrim = add i32 %outVertIdInPrim, 1
    //     %writeEnd = icmp ult %outVertIdInPrim, %outVertCount
    //     br i1 %writeContinue, label %.writeOutVertOffsetLoop, label %.writeOutVertOffset
    //
    // .endWriteOutVertOffset:
    //     call void @llvm.amdgcn.s.barrier()
    //
    //     %vertExp = icmp ult i32 %threadIdInSubgroup, %vertCountInSubgroup
    //     br i1 %vertExp, label %.expVert, label %.endExpVert
    //
    // .expVert:
    //     call void @llpc.ngg.COPY.main(%sgpr..., %vgpr)
    //     br label %.endExpvert
    //
    // .endExpVert:
    //     ret void
    // }

    // Define basic blocks
    auto pEntryBlock = CreateBlock(pEntryPoint, ".entry");

    auto pBeginEsBlock = CreateBlock(pEntryPoint, ".beginEs");
    auto pEndEsBlock = CreateBlock(pEntryPoint, ".endEs");

    auto pInitOutPrimDataBlock = CreateBlock(pEntryPoint, ".initOutPrimData");
    auto pEndInitOutPrimDataBlock = CreateBlock(pEntryPoint, ".endInitOutPrimData");

    auto pZeroOutVertCountBlock = CreateBlock(pEntryPoint, ".zeroOutVertCount");
    auto pEndZeroOutVertCountBlock = CreateBlock(pEntryPoint, ".endZeroOutVertCount");

    auto pBeginGsBlock = CreateBlock(pEntryPoint, ".beginGs");
    auto pEndGsBlock = CreateBlock(pEntryPoint, ".endGs");

    auto pAccVertCountBlock = CreateBlock(pEntryPoint, ".accVertCount");
    auto pEndAccVertCountBlock = CreateBlock(pEntryPoint, ".endAccVertCount");

    auto pReadVertCountBlock = CreateBlock(pEntryPoint, ".readVertCount");
    auto pEndReadVertCountBlock = CreateBlock(pEntryPoint, ".endReadVertCount");

    auto pAllocReqBlock = CreateBlock(pEntryPoint, ".allocReq");
    auto pEndAllocReqBlock = CreateBlock(pEntryPoint, ".endAllocReq");

    auto pReviseOutPrimDataBlock = CreateBlock(pEntryPoint, ".reviseOutPrimData");
    auto pReviseOutPrimDataLoopBlock = CreateBlock(pEntryPoint, ".reviseOutPrimDataLoop");
    auto pEndReviseOutPrimDataBlock = CreateBlock(pEntryPoint, ".endReviseOutPrimData");

    auto pExpPrimBlock = CreateBlock(pEntryPoint, ".expPrim");
    auto pEndExpPrimBlock = CreateBlock(pEntryPoint, ".endExpPrim");

    auto pWriteOutVertOffsetBlock = CreateBlock(pEntryPoint, ".writeOutVertOffset");
    auto pWriteOutVertOffsetLoopBlock = CreateBlock(pEntryPoint, ".writeOutVertOffsetLoop");
    auto pEndWriteOutVertOffsetBlock = CreateBlock(pEntryPoint, ".endWriteOutVertOffset");

    auto pExpVertBlock = CreateBlock(pEntryPoint, ".expVert");
    auto pEndExpVertBlock = CreateBlock(pEntryPoint, ".endExpVert");

    // Construct ".entry" block
    {
        m_pBuilder->SetInsertPoint(pEntryBlock);

        InitWaveThreadInfo(pMergedGroupInfo, pMergedWaveInfo);

        // Record ES-GS vertex offsets info
        m_nggFactor.pEsGsOffsets01 = pEsGsOffsets01;
        m_nggFactor.pEsGsOffsets23 = pEsGsOffsets23;
        m_nggFactor.pEsGsOffsets45 = pEsGsOffsets45;

        auto pVertValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pVertCountInWave);
        m_pBuilder->CreateCondBr(pVertValid, pBeginEsBlock, pEndEsBlock);
    }

    // Construct ".beginEs" block
    {
        m_pBuilder->SetInsertPoint(pBeginEsBlock);

        RunEsOrEsVariant(pModule,
                         lgcName::NggEsEntryPoint,
                         pEntryPoint->arg_begin(),
                         false,
                         nullptr,
                         pBeginEsBlock);

        m_pBuilder->CreateBr(pEndEsBlock);
    }

    // Construct ".endEs" block
    {
        m_pBuilder->SetInsertPoint(pEndEsBlock);

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto pPrimValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
        m_pBuilder->CreateCondBr(pPrimValid, pInitOutPrimDataBlock, pEndInitOutPrimDataBlock);
    }

    // Construct ".initOutPrimData" block
    {
        m_pBuilder->SetInsertPoint(pInitOutPrimDataBlock);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutPrimData);

        auto pLdsOffset = m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(maxOutPrims));
        pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, 2);
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

        auto pNullPrim = m_pBuilder->getInt32(NullPrim);
        Value* pNullPrims = UndefValue::get(VectorType::get(m_pBuilder->getInt32Ty(), maxOutPrims));
        for (uint32_t i = 0; i < maxOutPrims; ++i)
        {
            pNullPrims = m_pBuilder->CreateInsertElement(pNullPrims, pNullPrim, i);
        }

        m_pLdsManager->WriteValueToLds(pNullPrims, pLdsOffset);

        m_pBuilder->CreateBr(pEndInitOutPrimDataBlock);
    }

    // Construct ".endInitOutPrimData" block
    {
        m_pBuilder->SetInsertPoint(pEndInitOutPrimDataBlock);

        auto pFirstThreadInSubgroup =
            m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(0));
        m_pBuilder->CreateCondBr(pFirstThreadInSubgroup, pZeroOutVertCountBlock, pEndZeroOutVertCountBlock);
    }

    // Construct ".zeroOutVertCount" block
    {
        m_pBuilder->SetInsertPoint(pZeroOutVertCountBlock);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertCountInWaves);

        auto pZero = m_pBuilder->getInt32(0);

        for (uint32_t i = 0; i < MaxGsStreams; ++i)
        {
            // NOTE: Only do this for rasterization stream.
            if (i == rasterStream)
            {
                // Zero per-wave GS output vertex count
                auto pZeros = ConstantVector::getSplat({Gfx9::NggMaxWavesPerSubgroup, false}, pZero);

                auto pLdsOffset =
                    m_pBuilder->getInt32(regionStart + i * SizeOfDword * (Gfx9::NggMaxWavesPerSubgroup + 1));
                m_pLdsManager->WriteValueToLds(pZeros, pLdsOffset);

                // Zero sub-group GS output vertex count
                pLdsOffset = m_pBuilder->getInt32(regionStart + SizeOfDword * Gfx9::NggMaxWavesPerSubgroup);
                m_pLdsManager->WriteValueToLds(pZero, pLdsOffset);

                break;
            }
        }

        m_pBuilder->CreateBr(pEndZeroOutVertCountBlock);
    }

    // Construct ".endZeroOutVertCount" block
    {
        m_pBuilder->SetInsertPoint(pEndZeroOutVertCountBlock);

        auto pPrimValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
        m_pBuilder->CreateCondBr(pPrimValid, pBeginGsBlock, pEndGsBlock);
    }

    // Construct ".beginGs" block
    Value* pOutPrimCount = nullptr;
    Value* pOutVertCount = nullptr;
    Value* pInclusiveOutVertCount = nullptr;
    Value* pOutVertCountInWave = nullptr;
    {
        m_pBuilder->SetInsertPoint(pBeginGsBlock);

        Value* pOutPrimVertCountInfo = RunGsVariant(pModule, pEntryPoint->arg_begin(), pBeginGsBlock);

        // Extract output primitive/vertex count info from the return value
        assert(pOutPrimVertCountInfo->getType()->isStructTy());
        pOutPrimCount = m_pBuilder->CreateExtractValue(pOutPrimVertCountInfo, 0);
        pOutVertCount = m_pBuilder->CreateExtractValue(pOutPrimVertCountInfo, 1);
        pInclusiveOutVertCount = m_pBuilder->CreateExtractValue(pOutPrimVertCountInfo, 2);
        pOutVertCountInWave = m_pBuilder->CreateExtractValue(pOutPrimVertCountInfo, 3);

        m_pBuilder->CreateBr(pEndGsBlock);
    }

    // Construct ".endGs" block
    {
        m_pBuilder->SetInsertPoint(pEndGsBlock);

        auto pOutPrimCountPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        pOutPrimCountPhi->addIncoming(m_pBuilder->getInt32(0), pEndZeroOutVertCountBlock);
        pOutPrimCountPhi->addIncoming(pOutPrimCount, pBeginGsBlock);
        pOutPrimCount = pOutPrimCountPhi;
        pOutPrimCount->setName("outPrimCount");

        auto pOutVertCountPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        pOutVertCountPhi->addIncoming(m_pBuilder->getInt32(0), pEndZeroOutVertCountBlock);
        pOutVertCountPhi->addIncoming(pOutVertCount, pBeginGsBlock);
        pOutVertCount = pOutVertCountPhi;
        pOutVertCount->setName("outVertCount");

        auto pInclusiveOutVertCountPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        pInclusiveOutVertCountPhi->addIncoming(m_pBuilder->getInt32(0), pEndZeroOutVertCountBlock);
        pInclusiveOutVertCountPhi->addIncoming(pInclusiveOutVertCount, pBeginGsBlock);
        pInclusiveOutVertCount = pInclusiveOutVertCountPhi;
        pInclusiveOutVertCount->setName("inclusiveOutVertCount");

        auto pOutVertCountInWavePhi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        pOutVertCountInWavePhi->addIncoming(m_pBuilder->getInt32(0), pEndZeroOutVertCountBlock);
        pOutVertCountInWavePhi->addIncoming(pOutVertCountInWave, pBeginGsBlock);
        pOutVertCountInWave = pOutVertCountInWavePhi;
        // NOTE: We promote GS output vertex count in wave to SGPR since it is treated as an uniform value. Otherwise,
        // phi node resolving still treats it as VGPR, not as expected.
        pOutVertCountInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pOutVertCountInWave);
        pOutVertCountInWave->setName("outVertCountInWave");

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto pHasSurviveVert = m_pBuilder->CreateICmpNE(pOutVertCountInWave, m_pBuilder->getInt32(0));

        auto pThreadIdUpbound = m_pBuilder->CreateSub(m_pBuilder->getInt32(waveCountInSubgroup),
                                                      m_nggFactor.pWaveIdInSubgroup);
        auto pThreadValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, pThreadIdUpbound);

        auto pVertCountAcc = m_pBuilder->CreateAnd(pHasSurviveVert, pThreadValid);

        m_pBuilder->CreateCondBr(pVertCountAcc, pAccVertCountBlock, pEndAccVertCountBlock);
    }

    // Construct ".accVertCount" block
    {
        m_pBuilder->SetInsertPoint(pAccVertCountBlock);

        auto pLdsOffset = m_pBuilder->CreateAdd(m_nggFactor.pWaveIdInSubgroup, m_nggFactor.pThreadIdInWave);
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(1));
        pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, 2);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertCountInWaves);

        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));
        m_pLdsManager->AtomicOpWithLds(AtomicRMWInst::Add, pOutVertCountInWave, pLdsOffset);

        m_pBuilder->CreateBr(pEndAccVertCountBlock);
    }

    // Construct ".endAccVertCount" block
    {
        m_pBuilder->SetInsertPoint(pEndAccVertCountBlock);

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto pFirstThreadInWave = m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInWave, m_pBuilder->getInt32(0));
        m_pBuilder->CreateCondBr(pFirstThreadInWave, pReadVertCountBlock, pEndReadVertCountBlock);
    }

    // Construct ".readVertCount" block
    Value* pOutVertCountInWaves = nullptr;
    {
        m_pBuilder->SetInsertPoint(pReadVertCountBlock);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertCountInWaves);

        // The DWORD following DWORDs for all waves stores GS output vertex count of the entire sub-group
        auto pLdsOffset = m_pBuilder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
        pOutVertCountInWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

        m_pBuilder->CreateBr(pEndReadVertCountBlock);
    }

    // Construct ".endReadVertCount" block
    {
        m_pBuilder->SetInsertPoint(pEndReadVertCountBlock);

        Value* pVertCountInSubgroup = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        static_cast<PHINode*>(pVertCountInSubgroup)->addIncoming(m_pBuilder->getInt32(0), pEndAccVertCountBlock);
        static_cast<PHINode*>(pVertCountInSubgroup)->addIncoming(pOutVertCountInWaves, pReadVertCountBlock);

        // NOTE: We promote GS output vertex count in subgroup to SGPR since it is treated as an uniform value.
        pVertCountInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, pVertCountInSubgroup);

        m_nggFactor.pVertCountInSubgroup = pVertCountInSubgroup;

        auto pFirstWaveInSubgroup = m_pBuilder->CreateICmpEQ(m_nggFactor.pWaveIdInSubgroup, m_pBuilder->getInt32(0));
        m_pBuilder->CreateCondBr(pFirstWaveInSubgroup, pAllocReqBlock, pEndAllocReqBlock);
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

        auto pPrimValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
        m_pBuilder->CreateCondBr(pPrimValid, pReviseOutPrimDataBlock, pEndReviseOutPrimDataBlock);
    }

    // Construct ".reviseOutPrimData" block
    Value* pVertexIdAdjust = nullptr;
    {
        m_pBuilder->SetInsertPoint(pReviseOutPrimDataBlock);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertCountInWaves);

        auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pWaveIdInSubgroup, 2);
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));
        auto pOutVertCountInPreWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

        // vertexIdAdjust = outVertCountInPreWaves + exclusiveOutVertCount
        auto pExclusiveOutVertCount = m_pBuilder->CreateSub(pInclusiveOutVertCount, pOutVertCount);
        pVertexIdAdjust = m_pBuilder->CreateAdd(pOutVertCountInPreWaves, pExclusiveOutVertCount);

        auto pAdjustVertexId = m_pBuilder->CreateICmpNE(pVertexIdAdjust, m_pBuilder->getInt32(0));
        m_pBuilder->CreateCondBr(pAdjustVertexId, pReviseOutPrimDataLoopBlock, pEndReviseOutPrimDataBlock);
    }

    // Construct ".reviseOutPrimDataLoop" block
    {
        m_pBuilder->SetInsertPoint(pReviseOutPrimDataLoopBlock);

        //
        // The processing is something like this:
        //   for (outPrimId = 0; outPrimId < outPrimCount; outPrimId++)
        //   {
        //       ldsOffset = regionStart + 4 * (threadIdInSubgroup * maxOutPrims + outPrimId)
        //       Read GS output primitive data from LDS, revise them, and write back to LDS
        //   }
        //
        auto pOutPrimIdPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        pOutPrimIdPhi->addIncoming(m_pBuilder->getInt32(0), pReviseOutPrimDataBlock); // outPrimId = 0

        ReviseOutputPrimitiveData(pOutPrimIdPhi, pVertexIdAdjust);

        auto pOutPrimId = m_pBuilder->CreateAdd(pOutPrimIdPhi, m_pBuilder->getInt32(1)); // outPrimId++
        pOutPrimIdPhi->addIncoming(pOutPrimId, pReviseOutPrimDataLoopBlock);

        auto pReviseContinue = m_pBuilder->CreateICmpULT(pOutPrimId, pOutPrimCount);
        m_pBuilder->CreateCondBr(pReviseContinue, pReviseOutPrimDataLoopBlock, pEndReviseOutPrimDataBlock);
    }

    // Construct ".endReviseOutPrimData" block
    {
        m_pBuilder->SetInsertPoint(pEndReviseOutPrimDataBlock);

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto pPrimExp = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pPrimCountInSubgroup);
        m_pBuilder->CreateCondBr(pPrimExp, pExpPrimBlock, pEndExpPrimBlock);
    }

    // Construct ".expPrim" block
    {
        m_pBuilder->SetInsertPoint(pExpPrimBlock);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutPrimData);

        auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, 2);
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

        auto pPrimData = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

        auto pUndef = UndefValue::get(m_pBuilder->getInt32Ty());
        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                    m_pBuilder->getInt32Ty(),
                                    {
                                        m_pBuilder->getInt32(EXP_TARGET_PRIM),      // tgt
                                        m_pBuilder->getInt32(0x1),                  // en
                                        pPrimData,                                  // src0 ~ src3
                                        pUndef,
                                        pUndef,
                                        pUndef,
                                        m_pBuilder->getTrue(),                      // done, must be set
                                        m_pBuilder->getFalse(),                     // vm
                                    });

        m_pBuilder->CreateBr(pEndExpPrimBlock);
    }

    // Construct ".endExpPrim" block
    {
        m_pBuilder->SetInsertPoint(pEndExpPrimBlock);

        auto pPrimValid = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInWave, m_nggFactor.pPrimCountInWave);
        m_pBuilder->CreateCondBr(pPrimValid, pWriteOutVertOffsetBlock, pEndWriteOutVertOffsetBlock);
    }

    // Construct ".writeOutVertOffset" block
    Value* pWriteOffset = nullptr;
    Value* pWriteValue = nullptr;
    {
        m_pBuilder->SetInsertPoint(pWriteOutVertOffsetBlock);

        uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertCountInWaves);

        auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pWaveIdInSubgroup, 2);
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));
        auto pOutVertCountInPrevWaves = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

        // outVertThreadId = outVertCountInPrevWaves + exclusiveOutVertCount
        auto pExclusiveOutVertCount = m_pBuilder->CreateSub(pInclusiveOutVertCount, pOutVertCount);
        auto pOutVertThreadId = m_pBuilder->CreateAdd(pOutVertCountInPrevWaves, pExclusiveOutVertCount);

        // writeOffset = regionStart (OutVertOffset) + outVertThreadId * 4
        regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertOffset);
        pWriteOffset = m_pBuilder->CreateShl(pOutVertThreadId, 2);
        pWriteOffset = m_pBuilder->CreateAdd(pWriteOffset, m_pBuilder->getInt32(regionStart));

        // vertexItemOffset = threadIdInSubgroup * gsVsRingItemSize * 4 (in BYTE)
        auto pVertexItemOffset = m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup,
                                                       m_pBuilder->getInt32(calcFactor.gsVsRingItemSize * 4));

        // writeValue = regionStart (GsVsRing) + vertexItemOffset
        regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionGsVsRing);
        pWriteValue = m_pBuilder->CreateAdd(pVertexItemOffset, m_pBuilder->getInt32(regionStart));

        m_pBuilder->CreateBr(pWriteOutVertOffsetLoopBlock);
    }

    // Construct ".writeOutVertOffsetLoop" block
    {
        m_pBuilder->SetInsertPoint(pWriteOutVertOffsetLoopBlock);

        //
        // The processing is something like this:
        //   for (outVertIdInPrim = 0; outVertIdInPrim < outVertCount; outVertIdInPrim++)
        //   {
        //       ldsOffset = writeOffset + 4 * outVertIdInPrim
        //       vertexOffset = writeValue + 4 * vertexSize * outVertIdInPrim
        //       Write GS output vertex offset to LDS
        //   }
        //
        auto pOutVertIdInPrimPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
        pOutVertIdInPrimPhi->addIncoming(m_pBuilder->getInt32(0), pWriteOutVertOffsetBlock); // outVertIdInPrim = 0

        auto pLdsOffset = m_pBuilder->CreateShl(pOutVertIdInPrimPhi, 2);
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pWriteOffset);

        const uint32_t vertexSize = pResUsage->inOutUsage.gs.outLocCount[rasterStream] * 4;
        auto pVertexoffset = m_pBuilder->CreateMul(pOutVertIdInPrimPhi, m_pBuilder->getInt32(4 * vertexSize));
        pVertexoffset = m_pBuilder->CreateAdd(pVertexoffset, pWriteValue);

        m_pLdsManager->WriteValueToLds(pVertexoffset, pLdsOffset);

        auto pOutVertIdInPrim =
            m_pBuilder->CreateAdd(pOutVertIdInPrimPhi, m_pBuilder->getInt32(1)); // outVertIdInPrim++
        pOutVertIdInPrimPhi->addIncoming(pOutVertIdInPrim, pWriteOutVertOffsetLoopBlock);

        auto pWriteContinue = m_pBuilder->CreateICmpULT(pOutVertIdInPrim, pOutVertCount);
        m_pBuilder->CreateCondBr(pWriteContinue, pWriteOutVertOffsetLoopBlock, pEndWriteOutVertOffsetBlock);
    }

    // Construct ".endWriteOutVertOffset" block
    {
        m_pBuilder->SetInsertPoint(pEndWriteOutVertOffsetBlock);

        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto pVertExp = m_pBuilder->CreateICmpULT(m_nggFactor.pThreadIdInSubgroup, m_nggFactor.pVertCountInSubgroup);
        m_pBuilder->CreateCondBr(pVertExp, pExpVertBlock, pEndExpVertBlock);
    }

    // Construct ".expVert" block
    {
        m_pBuilder->SetInsertPoint(pExpVertBlock);

        RunCopyShader(pModule, pExpVertBlock);
        m_pBuilder->CreateBr(pEndExpVertBlock);
    }

    // Construct ".endExpVert" block
    {
        m_pBuilder->SetInsertPoint(pEndExpVertBlock);

        m_pBuilder->CreateRetVoid();
    }
}

// =====================================================================================================================
// Extracts merged group/wave info and initializes part of NGG calculation factors.
//
// NOTE: This function must be invoked by the entry block of NGG shader module.
void NggPrimShader::InitWaveThreadInfo(
    Value* pMergedGroupInfo,    // [in] Merged group info
    Value* pMergedWaveInfo)     // [in] Merged wave info
{
    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

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

    pPrimCountInSubgroup->setName("primCountInSubgroup");
    pVertCountInSubgroup->setName("vertCountInSubgroup");
    pPrimCountInWave->setName("primCountInWave");
    pVertCountInWave->setName("vertCountInWave");
    pThreadIdInWave->setName("threadIdInWave");
    pThreadIdInSubgroup->setName("threadIdInSubgroup");
    pWaveIdInSubgroup->setName("waveIdInSubgroup");

    // Record wave/thread info
    m_nggFactor.pPrimCountInSubgroup    = pPrimCountInSubgroup;
    m_nggFactor.pVertCountInSubgroup    = pVertCountInSubgroup;
    m_nggFactor.pPrimCountInWave        = pPrimCountInWave;
    m_nggFactor.pVertCountInWave        = pVertCountInWave;
    m_nggFactor.pThreadIdInWave         = pThreadIdInWave;
    m_nggFactor.pThreadIdInSubgroup     = pThreadIdInSubgroup;
    m_nggFactor.pWaveIdInSubgroup       = pWaveIdInSubgroup;

    m_nggFactor.pMergedGroupInfo        = pMergedGroupInfo;
}

// =====================================================================================================================
// Does various culling for NGG primitive shader.
Value* NggPrimShader::DoCulling(
    Module* pModule)    // [in] LLVM module
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
    auto pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, 2);

    auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.pEsGsOffsets01,
                                                        m_pBuilder->getInt32(16),
                                                        m_pBuilder->getInt32(16),
                                                    });
    auto pVertexId1 = m_pBuilder->CreateLShr(pEsGsOffset1, 2);

    auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.pEsGsOffsets23,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(16),
                                                    });
    auto pVertexId2 = m_pBuilder->CreateLShr(pEsGsOffset2, 2);

    Value* vertexId[3] = { pVertexId0, pVertexId1, pVertexId2 };
    Value* vertex[3] = {};

    const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionPosData);
    assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation
    auto pRegionStart = m_pBuilder->getInt32(regionStart);

    for (uint32_t i = 0; i < 3; ++i)
    {
        Value* pLdsOffset = m_pBuilder->CreateMul(vertexId[i], m_pBuilder->getInt32(SizeOfVec4));
        pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pRegionStart);

        // Use 128-bit LDS load
        vertex[i] = m_pLdsManager->ReadValueFromLds(
            VectorType::get(Type::getFloatTy(*m_pContext), 4), pLdsOffset, true);
    }

    // Handle backface culling
    if (m_pNggControl->enableBackfaceCulling)
    {
        pCullFlag = DoBackfaceCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2]);
    }

    // Handle frustum culling
    if (m_pNggControl->enableFrustumCulling)
    {
        pCullFlag = DoFrustumCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2]);
    }

    // Handle box filter culling
    if (m_pNggControl->enableBoxFilterCulling)
    {
        pCullFlag = DoBoxFilterCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2]);
    }

    // Handle sphere culling
    if (m_pNggControl->enableSphereCulling)
    {
        pCullFlag = DoSphereCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2]);
    }

    // Handle small primitive filter culling
    if (m_pNggControl->enableSmallPrimFilter)
    {
        pCullFlag = DoSmallPrimFilterCulling(pModule, pCullFlag, vertex[0], vertex[1], vertex[2]);
    }

    // Handle cull distance culling
    if (m_pNggControl->enableCullDistanceCulling)
    {
        Value* signMask[3] = {};

        const auto regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionCullDistance);
        auto pRegionStart = m_pBuilder->getInt32(regionStart);

        for (uint32_t i = 0; i < 3; ++i)
        {
            Value* pLdsOffset = m_pBuilder->CreateShl(vertexId[i], 2);
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pRegionStart);

            signMask[i] = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);
        }

        pCullFlag = DoCullDistanceCulling(pModule, pCullFlag, signMask[0], signMask[1], signMask[2]);
    }

    return pCullFlag;
}

// =====================================================================================================================
// Requests that parameter cache space be allocated (send the message GS_ALLOC_REQ).
void NggPrimShader::DoParamCacheAllocRequest()
{
    // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
    Value* pM0 = m_pBuilder->CreateShl(m_nggFactor.pPrimCountInSubgroup, 12);
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
        Value* pVertexId0 = m_pBuilder->CreateLShr(pEsGsOffset0, 2);

        auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_pBuilder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.pEsGsOffsets01,
                                                            m_pBuilder->getInt32(16),
                                                            m_pBuilder->getInt32(16),
                                                        });
        Value* pVertexId1 = m_pBuilder->CreateLShr(pEsGsOffset1, 2);

        auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_pBuilder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.pEsGsOffsets23,
                                                            m_pBuilder->getInt32(0),
                                                            m_pBuilder->getInt32(16),
                                                        });
        Value* pVertexId2 = m_pBuilder->CreateLShr(pEsGsOffset2, 2);

        if (vertexCompact)
        {
            // NOTE: If the current vertex count in sub-group is less than the original value, then there must be
            // vertex culling. When vertex culling occurs, the vertex IDs should be fetched from LDS (compacted).
            auto pVertCountInSubgroup = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_pBuilder->getInt32Ty(),
                                                                    {
                                                                        m_nggFactor.pMergedGroupInfo,
                                                                        m_pBuilder->getInt32(12),
                                                                        m_pBuilder->getInt32(9),
                                                                    });
            auto pVertCulled = m_pBuilder->CreateICmpULT(m_nggFactor.pVertCountInSubgroup, pVertCountInSubgroup);

            auto pExpPrimBlock = m_pBuilder->GetInsertBlock();

            auto pReadCompactIdBlock = CreateBlock(pExpPrimBlock->getParent(), "readCompactId");
            pReadCompactIdBlock->moveAfter(pExpPrimBlock);

            auto pExpPrimContBlock = CreateBlock(pExpPrimBlock->getParent(), "expPrimCont");
            pExpPrimContBlock->moveAfter(pReadCompactIdBlock);

            m_pBuilder->CreateCondBr(pVertCulled, pReadCompactIdBlock, pExpPrimContBlock);

            // Construct ".readCompactId" block
            Value* pCompactVertexId0 = nullptr;
            Value* pCompactVertexId1 = nullptr;
            Value* pCompactVertexId2 = nullptr;
            {
                m_pBuilder->SetInsertPoint(pReadCompactIdBlock);

                pCompactVertexId0 = ReadPerThreadDataFromLds(m_pBuilder->getInt8Ty(),
                                                             pVertexId0,
                                                             LdsRegionVertThreadIdMap);
                pCompactVertexId0 = m_pBuilder->CreateZExt(pCompactVertexId0, m_pBuilder->getInt32Ty());

                pCompactVertexId1 = ReadPerThreadDataFromLds(m_pBuilder->getInt8Ty(),
                                                             pVertexId1,
                                                             LdsRegionVertThreadIdMap);
                pCompactVertexId1 = m_pBuilder->CreateZExt(pCompactVertexId1, m_pBuilder->getInt32Ty());

                pCompactVertexId2 = ReadPerThreadDataFromLds(m_pBuilder->getInt8Ty(),
                                                             pVertexId2,
                                                             LdsRegionVertThreadIdMap);
                pCompactVertexId2 = m_pBuilder->CreateZExt(pCompactVertexId2, m_pBuilder->getInt32Ty());

                m_pBuilder->CreateBr(pExpPrimContBlock);
            }

            // Construct part of ".expPrimCont" block (phi nodes)
            {
                m_pBuilder->SetInsertPoint(pExpPrimContBlock);

                auto pVertexId0Phi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
                pVertexId0Phi->addIncoming(pCompactVertexId0, pReadCompactIdBlock);
                pVertexId0Phi->addIncoming(pVertexId0, pExpPrimBlock);

                auto pVertexId1Phi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
                pVertexId1Phi->addIncoming(pCompactVertexId1, pReadCompactIdBlock);
                pVertexId1Phi->addIncoming(pVertexId1, pExpPrimBlock);

                auto pVertexId2Phi = m_pBuilder->CreatePHI(m_pBuilder->getInt32Ty(), 2);
                pVertexId2Phi->addIncoming(pCompactVertexId2, pReadCompactIdBlock);
                pVertexId2Phi->addIncoming(pVertexId2, pExpPrimBlock);

                pVertexId0 = pVertexId0Phi;
                pVertexId1 = pVertexId1Phi;
                pVertexId2 = pVertexId2Phi;
            }
        }

        pPrimData = m_pBuilder->CreateShl(pVertexId2, 10);
        pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId1);

        pPrimData = m_pBuilder->CreateShl(pPrimData, 10);
        pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId0);

        if (vertexCompact)
        {
            assert(pCullFlag != nullptr); // Must not be null
            const auto pNullPrim = m_pBuilder->getInt32(NullPrim);
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
// Early exit NGG primitive shader when we detect that the entire sub-group is fully culled, doing dummy
// primitive/vertex export if necessary.
void NggPrimShader::DoEarlyExit(
    uint32_t  fullyCulledThreadCount,   // Thread count left when the entire sub-group is fully culled
    uint32_t  expPosCount)              // Position export count
{
    if (fullyCulledThreadCount > 0)
    {
        assert(fullyCulledThreadCount == 1); // Currently, if workarounded, this is set to 1

        auto pEarlyExitBlock = m_pBuilder->GetInsertBlock();

        auto pDummyExpBlock = CreateBlock(pEarlyExitBlock->getParent(), ".dummyExp");
        pDummyExpBlock->moveAfter(pEarlyExitBlock);

        auto pEndDummyExpBlock = CreateBlock(pEarlyExitBlock->getParent(), ".endDummyExp");
        pEndDummyExpBlock->moveAfter(pDummyExpBlock);

        // Continue to construct ".earlyExit" block
        {
            auto pFirstThreadInSubgroup =
                m_pBuilder->CreateICmpEQ(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(0));
            m_pBuilder->CreateCondBr(pFirstThreadInSubgroup, pDummyExpBlock, pEndDummyExpBlock);
        }

        // Construct ".dummyExp" block
        {
            m_pBuilder->SetInsertPoint(pDummyExpBlock);

            auto pUndef = UndefValue::get(m_pBuilder->getInt32Ty());

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

            pUndef = UndefValue::get(m_pBuilder->getFloatTy());

            for (uint32_t i = 0; i < expPosCount; ++i)
            {
                m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                            m_pBuilder->getFloatTy(),
                                            {
                                                m_pBuilder->getInt32(EXP_TARGET_POS_0 + i), // tgt
                                                m_pBuilder->getInt32(0x0),                  // en
                                                // src0 ~ src3
                                                pUndef,
                                                pUndef,
                                                pUndef,
                                                pUndef,
                                                m_pBuilder->getInt1(i == expPosCount - 1),  // done
                                                m_pBuilder->getFalse()                      // vm
                                            });
            }

            m_pBuilder->CreateBr(pEndDummyExpBlock);
        }

        // Construct ".endDummyExp" block
        {
            m_pBuilder->SetInsertPoint(pEndDummyExpBlock);
            m_pBuilder->CreateRetVoid();
        }
    }
    else
    {
        m_pBuilder->CreateRetVoid();
    }
}

// =====================================================================================================================
// Runs ES or ES variant (to get exported data).
//
// NOTE: The ES variant is derived from original ES main function with some additional special handling added to the
// function body and also mutates its return type.
void NggPrimShader::RunEsOrEsVariant(
    Module*               pModule,          // [in] LLVM module
    StringRef             entryName,        // ES entry name
    Argument*             pSysValueStart,   // Start of system value
    bool                  sysValueFromLds,  // Whether some system values are loaded from LDS (for vertex compaction)
    std::vector<ExpData>* pExpDataSet,      // [out] Set of exported data (could be null)
    BasicBlock*           pInsertAtEnd)     // [in] Where to insert instructions
{
    const bool hasTs = (m_hasTcs || m_hasTes);
    if (((hasTs && m_hasTes) || ((hasTs == false) && m_hasVs)) == false)
    {
        // No TES (tessellation is enabled) or VS (tessellation is disabled), don't have to run
        return;
    }

    const bool runEsVariant = (entryName != lgcName::NggEsEntryPoint);

    Function* pEsEntry = nullptr;
    if (runEsVariant)
    {
        assert(pExpDataSet != nullptr);
        pEsEntry = MutateEsToVariant(pModule, entryName, *pExpDataSet); // Mutate ES to variant

        if (pEsEntry == nullptr)
        {
            // ES variant is NULL, don't have to run
            return;
        }
    }
    else
    {
        pEsEntry = pModule->getFunction(lgcName::NggEsEntryPoint);
        assert(pEsEntry != nullptr);
    }

    // Call ES entry
    Argument* pArg = pSysValueStart;

    Value* pEsGsOffset = nullptr;
    if (m_hasGs)
    {
        auto& calcFactor = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
        pEsGsOffset = m_pBuilder->CreateMul(m_nggFactor.pWaveIdInSubgroup,
                                            m_pBuilder->getInt32(64 * 4 * calcFactor.esGsRingItemSize));
    }

    Value* pOffChipLdsBase = (pArg + EsGsSysValueOffChipLdsBase);
    Value* pIsOffChip = UndefValue::get(m_pBuilder->getInt32Ty()); // NOTE: This flag is unused.

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
        assert(m_pNggControl->compactMode == NggCompactVertices);

        const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

        if (hasTs)
        {
            if (pResUsage->builtInUsage.tes.tessCoord)
            {
                pTessCoordX = ReadPerThreadDataFromLds(m_pBuilder->getFloatTy(),
                                                       m_nggFactor.pThreadIdInSubgroup,
                                                       LdsRegionCompactTessCoordX);

                pTessCoordY = ReadPerThreadDataFromLds(m_pBuilder->getFloatTy(),
                                                       m_nggFactor.pThreadIdInSubgroup,
                                                       LdsRegionCompactTessCoordY);
            }

            pRelPatchId = ReadPerThreadDataFromLds(m_pBuilder->getInt32Ty(),
                                                   m_nggFactor.pThreadIdInSubgroup,
                                                   LdsRegionCompactRelPatchId);

            if (pResUsage->builtInUsage.tes.primitiveId)
            {
                pPatchId = ReadPerThreadDataFromLds(m_pBuilder->getInt32Ty(),
                                                    m_nggFactor.pThreadIdInSubgroup,
                                                    LdsRegionCompactPatchId);
            }
        }
        else
        {
            if (pResUsage->builtInUsage.vs.vertexIndex)
            {
                pVertexId = ReadPerThreadDataFromLds(m_pBuilder->getInt32Ty(),
                                                     m_nggFactor.pThreadIdInSubgroup,
                                                     LdsRegionCompactVertexId);
            }

            // NOTE: Relative vertex ID Will not be used when VS is merged to GS.

            if (pResUsage->builtInUsage.vs.primitiveId)
            {
                pVsPrimitiveId = ReadPerThreadDataFromLds(m_pBuilder->getInt32Ty(),
                                                          m_nggFactor.pThreadIdInSubgroup,
                                                          LdsRegionCompactPrimId);
            }

            if (pResUsage->builtInUsage.vs.instanceIndex)
            {
                pInstanceId = ReadPerThreadDataFromLds(m_pBuilder->getInt32Ty(),
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
        m_pPipelineState->GetShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    const uint32_t userDataCount = pIntfData->userDataCount;

    uint32_t userDataIdx = 0;

    auto pEsArgBegin = pEsEntry->arg_begin();
    const uint32_t esArgCount = pEsEntry->arg_size();
    (void(esArgCount)); // unused

    // Set up user data SGPRs
    while (userDataIdx < userDataCount)
    {
        assert(args.size() < esArgCount);

        auto pEsArg = (pEsArgBegin + args.size());
        assert(pEsArg->hasAttribute(Attribute::InReg));

        auto pEsArgTy = pEsArg->getType();
        if (pEsArgTy->isVectorTy())
        {
            assert(pEsArgTy->getVectorElementType()->isIntegerTy());

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
            assert(pEsArgTy->isIntegerTy());

            auto pEsUserData = m_pBuilder->CreateExtractElement(pUserData, userDataIdx);
            args.push_back(pEsUserData);
            ++userDataIdx;
        }
    }

    if (hasTs)
    {
        // Set up system value SGPRs
        if (m_pPipelineState->IsTessOffChip())
        {
            args.push_back(m_hasGs ? pOffChipLdsBase : pIsOffChip);
            args.push_back(m_hasGs ? pIsOffChip : pOffChipLdsBase);
        }

        if (m_hasGs)
        {
            args.push_back(pEsGsOffset);
        }

        // Set up system value VGPRs
        args.push_back(pTessCoordX);
        args.push_back(pTessCoordY);
        args.push_back(pRelPatchId);
        args.push_back(pPatchId);
    }
    else
    {
        // Set up system value SGPRs
        if (m_hasGs)
        {
            args.push_back(pEsGsOffset);
        }

        // Set up system value VGPRs
        args.push_back(pVertexId);
        args.push_back(pRelVertexId);
        args.push_back(pVsPrimitiveId);
        args.push_back(pInstanceId);
    }

    assert(args.size() == esArgCount); // Must have visit all arguments of ES entry point

    if (runEsVariant)
    {
        auto pExpData = EmitCall(entryName,
                                 pEsEntry->getReturnType(),
                                 args,
                                 {},
                                 pInsertAtEnd);

        // Re-construct exported data from the return value
        auto pExpDataTy = pExpData->getType();
        assert(pExpDataTy->isArrayTy());

        const uint32_t expCount = pExpDataTy->getArrayNumElements();
        for (uint32_t i = 0; i < expCount; ++i)
        {
            Value* pExpValue = m_pBuilder->CreateExtractValue(pExpData, i);
            (*pExpDataSet)[i].pExpValue = pExpValue;
        }
    }
    else
    {
        EmitCall(entryName,
                 pEsEntry->getReturnType(),
                 args,
                 {},
                 pInsertAtEnd);
    }
}

// =====================================================================================================================
// Mutates the entry-point (".main") of ES to its variant (".variant").
//
// NOTE: Initially, the return type of ES entry-point is void. After this mutation, position and parameter exporting
// are both removed. Instead, the exported values are returned via either a new entry-point (combined) or two new
// entry-points (separate). Return types is something like this:
//   .variant:       [ POS0: <4 x float>, POS1: <4 x float>, ..., PARAM0: <4 x float>, PARAM1: <4 x float>, ... ]
//   .variant.pos:   [ POS0: <4 x float>, POS1: <4 x float>, ... ]
//   .variant.param: [ PARAM0: <4 x float>, PARAM1: <4 x float>, ... ]
Function* NggPrimShader::MutateEsToVariant(
    Module*               pModule,          // [in] LLVM module
    StringRef             entryName,        // ES entry name
    std::vector<ExpData>& expDataSet)       // [out] Set of exported data
{
    assert(m_hasGs == false); // GS must not be present
    assert(expDataSet.empty());

    const auto pEsEntryPoint = pModule->getFunction(lgcName::NggEsEntryPoint);
    assert(pEsEntryPoint != nullptr);

    const bool doExp      = (entryName == lgcName::NggEsEntryVariant);
    const bool doPosExp   = (entryName == lgcName::NggEsEntryVariantPos);
    const bool doParamExp = (entryName == lgcName::NggEsEntryVariantParam);

    // Calculate export count
    uint32_t expCount = 0;

    for (auto& func : pModule->functions())
    {
        if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_exp))
        {
            for (auto pUser : func.users())
            {
                CallInst* const pCall = dyn_cast<CallInst>(pUser);
                assert(pCall != nullptr);

                if (pCall->getParent()->getParent() != pEsEntryPoint)
                {
                    // Export call doesn't belong to ES, skip
                    continue;
                }

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
    auto pExpDataTy = ArrayType::get(VectorType::get(Type::getFloatTy(*m_pContext), 4), expCount);
    Value* pExpData = UndefValue::get(pExpDataTy);

    auto pEsEntryVariantTy = FunctionType::get(pExpDataTy, pEsEntryPoint->getFunctionType()->params(), false);
    auto pEsEntryVariant = Function::Create(pEsEntryVariantTy, pEsEntryPoint->getLinkage(), "", pModule);
    pEsEntryVariant->copyAttributesFrom(pEsEntryPoint);

    ValueToValueMapTy valueMap;

    Argument* pVariantArg = pEsEntryVariant->arg_begin();
    for (Argument &arg : pEsEntryPoint->args())
    {
        valueMap[&arg] = pVariantArg++;
    }

    SmallVector<ReturnInst*, 8> retInsts;
    CloneFunctionInto(pEsEntryVariant, pEsEntryPoint, valueMap, false, retInsts);

    pEsEntryVariant->setName(entryName);

    auto savedInsertPos = m_pBuilder->saveIP();

    BasicBlock* pRetBlock = &pEsEntryVariant->back();
    m_pBuilder->SetInsertPoint(pRetBlock);

    // Remove old "return" instruction
    assert(isa<ReturnInst>(pRetBlock->getTerminator()));
    ReturnInst* pRetInst = cast<ReturnInst>(pRetBlock->getTerminator());

    pRetInst->dropAllReferences();
    pRetInst->eraseFromParent();

    // Get exported data
    std::vector<Instruction*> expCalls;

    uint32_t lastExport = InvalidValue; // Record last position export that needs "done" flag
    for (auto& func : pModule->functions())
    {
        if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_exp))
        {
            for (auto pUser : func.users())
            {
                CallInst* const pCall = dyn_cast<CallInst>(pUser);
                assert(pCall != nullptr);

                if (pCall->getParent()->getParent() != pEsEntryVariant)
                {
                    // Export call doesn't belong to ES variant, skip
                    continue;
                }

                assert(pCall->getParent() == pRetBlock); // Must in return block

                uint8_t expTarget = cast<ConstantInt>(pCall->getArgOperand(0))->getZExtValue();

                bool expPos = ((expTarget >= EXP_TARGET_POS_0) && (expTarget <= EXP_TARGET_POS_4));
                bool expParam = ((expTarget >= EXP_TARGET_PARAM_0) && (expTarget <= EXP_TARGET_PARAM_31));

                if ((doExp && (expPos || expParam)) ||
                    (doPosExp && expPos) ||
                    (doParamExp && expParam))
                {
                    uint8_t channelMask = cast<ConstantInt>(pCall->getArgOperand(1))->getZExtValue();

                    Value* expValue[4] = {};
                    expValue[0] = pCall->getArgOperand(2);
                    expValue[1] = pCall->getArgOperand(3);
                    expValue[2] = pCall->getArgOperand(4);
                    expValue[3] = pCall->getArgOperand(5);

                    if (func.getName().endswith(".i32"))
                    {
                        expValue[0] = m_pBuilder->CreateBitCast(expValue[0], m_pBuilder->getFloatTy());
                        expValue[1] = m_pBuilder->CreateBitCast(expValue[1], m_pBuilder->getFloatTy());
                        expValue[2] = m_pBuilder->CreateBitCast(expValue[2], m_pBuilder->getFloatTy());
                        expValue[3] = m_pBuilder->CreateBitCast(expValue[3], m_pBuilder->getFloatTy());
                    }

                    Value* pExpValue = UndefValue::get(VectorType::get(Type::getFloatTy(*m_pContext), 4));
                    for (uint32_t i = 0; i < 4; ++i)
                    {
                        pExpValue = m_pBuilder->CreateInsertElement(pExpValue, expValue[i], i);
                    }

                    if (expPos)
                    {
                        // Last position export that needs "done" flag
                        lastExport = expDataSet.size();
                    }

                    ExpData expData = { expTarget, channelMask, false, pExpValue };
                    expDataSet.push_back(expData);
                }

                expCalls.push_back(pCall);
            }
        }
    }
    assert(expDataSet.size() == expCount);

    // Set "done" flag for last position export
    if (lastExport != InvalidValue)
    {
        expDataSet[lastExport].doneFlag = true;
    }

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
// Runs GS variant.
//
// NOTE: The GS variant is derived from original GS main function with some additional special handling added to the
// function body and also mutates its return type.
Value* NggPrimShader::RunGsVariant(
    Module*         pModule,        // [in] LLVM module
    Argument*       pSysValueStart, // Start of system value
    BasicBlock*     pInsertAtEnd)   // [in] Where to insert instructions
{
    assert(m_hasGs); // GS must be present

    Function* pGsEntry = MutateGsToVariant(pModule);

    // Call GS entry
    Argument* pArg = pSysValueStart;

    Value* pGsVsOffset = UndefValue::get(m_pBuilder->getInt32Ty()); // NOTE: For NGG, GS-VS offset is unused

    // NOTE: This argument is expected to be GS wave ID, not wave ID in sub-group, for normal ES-GS merged shader.
    // However, in NGG mode, GS wave ID, sent to GS_EMIT and GS_CUT messages, is no longer required because of NGG
    // handling of such messages. Instead, wave ID in sub-group is required as the substitue.
    auto pWaveId = m_nggFactor.pWaveIdInSubgroup;

    pArg += EsGsSpecialSysValueCount;

    Value* pUserData = pArg++;

    Value* pEsGsOffsets01 = pArg;
    Value* pEsGsOffsets23 = (pArg + 1);
    Value* pGsPrimitiveId = (pArg + 2);
    Value* pInvocationId  = (pArg + 3);
    Value* pEsGsOffsets45 = (pArg + 4);

    // NOTE: For NGG, GS invocation ID is stored in lowest 8 bits ([7:0]) and other higher bits are used for other
    // purposes according to GE-SPI interface.
    pInvocationId = m_pBuilder->CreateAnd(pInvocationId, m_pBuilder->getInt32(0xFF));

    auto pEsGsOffset0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pEsGsOffsets01,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(16)
                                                    });

    auto pEsGsOffset1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pEsGsOffsets01,
                                                        m_pBuilder->getInt32(16),
                                                        m_pBuilder->getInt32(16)
                                                    });

    auto pEsGsOffset2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pEsGsOffsets23,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(16)
                                                    });

    auto pEsGsOffset3 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pEsGsOffsets23,
                                                        m_pBuilder->getInt32(16),
                                                        m_pBuilder->getInt32(16)
                                                    });

    auto pEsGsOffset4 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pEsGsOffsets45,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(16)
                                                    });

    auto pEsGsOffset5 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pEsGsOffsets45,
                                                        m_pBuilder->getInt32(16),
                                                        m_pBuilder->getInt32(16)
                                                    });

    std::vector<Value*> args;

    auto pIntfData = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry);
    const uint32_t userDataCount = pIntfData->userDataCount;

    uint32_t userDataIdx = 0;

    auto pGsArgBegin = pGsEntry->arg_begin();
    const uint32_t gsArgCount = pGsEntry->arg_size();
    (void(gsArgCount)); // unused

    // Set up user data SGPRs
    while (userDataIdx < userDataCount)
    {
        assert(args.size() < gsArgCount);

        auto pGsArg = (pGsArgBegin + args.size());
        assert(pGsArg->hasAttribute(Attribute::InReg));

        auto pGsArgTy = pGsArg->getType();
        if (pGsArgTy->isVectorTy())
        {
            assert(pGsArgTy->getVectorElementType()->isIntegerTy());

            const uint32_t userDataSize = pGsArgTy->getVectorNumElements();

            std::vector<uint32_t> shuffleMask;
            for (uint32_t i = 0; i < userDataSize; ++i)
            {
                shuffleMask.push_back(userDataIdx + i);
            }

            userDataIdx += userDataSize;

            auto pGsUserData = m_pBuilder->CreateShuffleVector(pUserData, pUserData, shuffleMask);
            args.push_back(pGsUserData);
        }
        else
        {
            assert(pGsArgTy->isIntegerTy());

            auto pGsUserData = m_pBuilder->CreateExtractElement(pUserData, userDataIdx);
            args.push_back(pGsUserData);
            ++userDataIdx;
        }
    }

    // Set up system value SGPRs
    args.push_back(pGsVsOffset);
    args.push_back(pWaveId);

    // Set up system value VGPRs
    args.push_back(pEsGsOffset0);
    args.push_back(pEsGsOffset1);
    args.push_back(pGsPrimitiveId);
    args.push_back(pEsGsOffset2);
    args.push_back(pEsGsOffset3);
    args.push_back(pEsGsOffset4);
    args.push_back(pEsGsOffset5);
    args.push_back(pInvocationId);

    assert(args.size() == gsArgCount); // Must have visit all arguments of ES entry point

    return EmitCall(lgcName::NggGsEntryVariant,
                    pGsEntry->getReturnType(),
                    args,
                    {},
                    pInsertAtEnd);
}

// =====================================================================================================================
// Mutates the entry-point (".main") of GS to its variant (".variant").
//
// NOTE: Initially, the return type of GS entry-point is void. After this mutation, GS messages (GS_EMIT, GS_CUT) are
// handled by shader itself. Also, output primitive/vertex count info is calculated and is returned. The return type
// is something like this:
//   { OUT_PRIM_COUNT: i32, OUT_VERT_COUNT: i32, INCLUSIVE_OUT_VERT_COUNT: i32, OUT_VERT_COUNT_IN_WAVE: i32 }
Function* NggPrimShader::MutateGsToVariant(
    Module* pModule)          // [in] LLVM module
{
    assert(m_hasGs); // GS must be present

    auto pGsEntryPoint = pModule->getFunction(lgcName::NggGsEntryPoint);
    assert(pGsEntryPoint != nullptr);

    // Clone new entry-point
    auto pResultTy = StructType::get(*m_pContext,
                                     {
                                         m_pBuilder->getInt32Ty(), // outPrimCount
                                         m_pBuilder->getInt32Ty(), // outVertCount
                                         m_pBuilder->getInt32Ty(), // inclusiveOutVertCount
                                         m_pBuilder->getInt32Ty()  // outVertCountInWave
                                     });
    auto pGsEntryVariantTy = FunctionType::get(pResultTy, pGsEntryPoint->getFunctionType()->params(), false);
    auto pGsEntryVariant = Function::Create(pGsEntryVariantTy, pGsEntryPoint->getLinkage(), "", pModule);
    pGsEntryVariant->copyAttributesFrom(pGsEntryPoint);

    ValueToValueMapTy valueMap;

    Argument* pVariantArg = pGsEntryVariant->arg_begin();
    for (Argument &arg : pGsEntryPoint->args())
    {
        valueMap[&arg] = pVariantArg++;
    }

    SmallVector<ReturnInst*, 8> retInsts;
    CloneFunctionInto(pGsEntryVariant, pGsEntryPoint, valueMap, false, retInsts);

    pGsEntryVariant->setName(lgcName::NggGsEntryVariant);

    // Remove original GS entry-point
    pGsEntryPoint->dropAllReferences();
    pGsEntryPoint->eraseFromParent();
    pGsEntryPoint = nullptr; // No longer available

    auto savedInsertPos = m_pBuilder->saveIP();

    BasicBlock* pRetBlock = &pGsEntryVariant->back();

    // Remove old "return" instruction
    assert(isa<ReturnInst>(pRetBlock->getTerminator()));
    ReturnInst* pRetInst = cast<ReturnInst>(pRetBlock->getTerminator());

    pRetInst->dropAllReferences();
    pRetInst->eraseFromParent();

    std::vector<Instruction*> removeCalls;

    m_pBuilder->SetInsertPoint(&*pGsEntryVariant->front().getFirstInsertionPt());

    // Initialize GS emit counters, GS output vertex counters, GS output primitive counters,
    // GS outstanding vertex counters
    Value* emitCounterPtrs[MaxGsStreams] = {};
    Value* outVertCounterPtrs[MaxGsStreams] = {};
    Value* outPrimCounterPtrs[MaxGsStreams] = {};
    // NOTE: Outstanding vertices are those output vertices that are trying to form a primitive in progress while
    // still do not belong to any already-completed primitives. If GS_CUT is encountered, they are all dropped as
    // invalid vertices.
    Value* outstandingVertCounterPtrs[MaxGsStreams] = {};
    // NOTE: This group of flags are used to decide vertex ordering of an output triangle strip primitive. We
    // expect such ordering: 0 -> 1 -> 2, 1 -> 3 -> 2, 2 -> 3 -> 4, ..., N -> N+1 -> N+2 (or N -> N+2 -> N+1).
    Value* flipVertOrderPtrs[MaxGsStreams] = {};

    for (int i = 0; i < MaxGsStreams; ++i)
    {
        auto pEmitCounterPtr = m_pBuilder->CreateAlloca(m_pBuilder->getInt32Ty());
        m_pBuilder->CreateStore(m_pBuilder->getInt32(0), pEmitCounterPtr); // emitCounter = 0
        emitCounterPtrs[i] = pEmitCounterPtr;

        auto pOutVertCounterPtr = m_pBuilder->CreateAlloca(m_pBuilder->getInt32Ty());
        m_pBuilder->CreateStore(m_pBuilder->getInt32(0), pOutVertCounterPtr); // outVertCounter = 0
        outVertCounterPtrs[i] = pOutVertCounterPtr;

        auto pOutPrimCounterPtr = m_pBuilder->CreateAlloca(m_pBuilder->getInt32Ty());
        m_pBuilder->CreateStore(m_pBuilder->getInt32(0), pOutPrimCounterPtr); // outPrimCounter = 0
        outPrimCounterPtrs[i] = pOutPrimCounterPtr;

        auto pOutstandingVertCounterPtr = m_pBuilder->CreateAlloca(m_pBuilder->getInt32Ty());
        m_pBuilder->CreateStore(m_pBuilder->getInt32(0), pOutstandingVertCounterPtr); // outstandingVertCounter = 0
        outstandingVertCounterPtrs[i] = pOutstandingVertCounterPtr;

        auto pFlipVertOrderPtr = m_pBuilder->CreateAlloca(m_pBuilder->getInt1Ty());
        m_pBuilder->CreateStore(m_pBuilder->getFalse(), pFlipVertOrderPtr); // flipVertOrder = false
        flipVertOrderPtrs[i] = pFlipVertOrderPtr;
    }

    // Initialize thread ID in wave
    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

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

    // Initialzie thread ID in subgroup
    auto& entryArgIdxs = m_pPipelineState->GetShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
    auto pWaveId = GetFunctionArgument(pGsEntryVariant, entryArgIdxs.waveId);

    auto pThreadIdInSubgroup = m_pBuilder->CreateMul(pWaveId, m_pBuilder->getInt32(waveSize));
    pThreadIdInSubgroup = m_pBuilder->CreateAdd(pThreadIdInSubgroup, pThreadIdInWave);

    // Handle GS message and GS output export
    for (auto& func : pModule->functions())
    {
        if (func.getName().startswith(lgcName::NggGsOutputExport))
        {
            // Export GS outputs to GS-VS ring
            for (auto pUser : func.users())
            {
                CallInst* const pCall = dyn_cast<CallInst>(pUser);
                assert(pCall != nullptr);
                m_pBuilder->SetInsertPoint(pCall);

                assert(pCall->getNumArgOperands() == 4);
                const uint32_t location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
                const uint32_t compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
                const uint32_t streamId = cast<ConstantInt>(pCall->getOperand(2))->getZExtValue();
                assert(streamId < MaxGsStreams);
                Value* pOutput = pCall->getOperand(3);

                auto pOutVertCounter = m_pBuilder->CreateLoad(outVertCounterPtrs[streamId]);
                ExportGsOutput(pOutput, location, compIdx, streamId, pThreadIdInSubgroup, pOutVertCounter);

                removeCalls.push_back(pCall);
            }
        }
        else if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg))
        {
            // Handle GS message
            for (auto pUser : func.users())
            {
                CallInst* const pCall = dyn_cast<CallInst>(pUser);
                assert(pCall != nullptr);
                m_pBuilder->SetInsertPoint(pCall);

                uint64_t message = cast<ConstantInt>(pCall->getArgOperand(0))->getZExtValue();
                if ((message == GS_EMIT_STREAM0) || (message == GS_EMIT_STREAM1) ||
                    (message == GS_EMIT_STREAM2) || (message == GS_EMIT_STREAM3))
                {
                    // Handle GS_EMIT, MSG[9:8] = STREAM_ID
                    uint32_t streamId = (message & GS_EMIT_CUT_STREAM_ID_MASK) >> GS_EMIT_CUT_STREAM_ID_SHIFT;
                    assert(streamId < MaxGsStreams);
                    ProcessGsEmit(pModule,
                                 streamId,
                                 pThreadIdInSubgroup,
                                 emitCounterPtrs[streamId],
                                 outVertCounterPtrs[streamId],
                                 outPrimCounterPtrs[streamId],
                                 outstandingVertCounterPtrs[streamId],
                                 flipVertOrderPtrs[streamId]);
                }
                else if ((message == GS_CUT_STREAM0) || (message == GS_CUT_STREAM1) ||
                         (message == GS_CUT_STREAM2) || (message == GS_CUT_STREAM3))
                {
                    // Handle GS_CUT, MSG[9:8] = STREAM_ID
                    uint32_t streamId = (message & GS_EMIT_CUT_STREAM_ID_MASK) >> GS_EMIT_CUT_STREAM_ID_SHIFT;
                    assert(streamId < MaxGsStreams);
                    ProcessGsCut(pModule,
                                 streamId,
                                 pThreadIdInSubgroup,
                                 emitCounterPtrs[streamId],
                                 outVertCounterPtrs[streamId],
                                 outPrimCounterPtrs[streamId],
                                 outstandingVertCounterPtrs[streamId],
                                 flipVertOrderPtrs[streamId]);
                }
                else if (message == GS_DONE)
                {
                    // Handle GS_DONE, do nothing (just remove this call)
                }
                else
                {
                    // Unexpected GS message
                    llvm_unreachable("Should never be called!");
                }

                removeCalls.push_back(pCall);
            }
        }
    }

    // Add additional processing in return block
    m_pBuilder->SetInsertPoint(pRetBlock);

    // NOTE: Only return output primitive/vertex count info for rasterization stream.
    auto rasterStream = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.rasterStream;
    auto pOutPrimCount = m_pBuilder->CreateLoad(outPrimCounterPtrs[rasterStream]);
    auto pOutVertCount = m_pBuilder->CreateLoad(outVertCounterPtrs[rasterStream]);

    Value* pOutVertCountInWave = nullptr;
    auto pInclusiveOutVertCount = DoSubgroupInclusiveAdd(pOutVertCount, &pOutVertCountInWave);

    // NOTE: We use the highest thread (MSB) to get GS output vertex count in this wave (after inclusive-add,
    // the value of this thread stores this info)
    pOutVertCountInWave = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readlane,
                                                      {},
                                                      {
                                                          pOutVertCountInWave,
                                                          m_pBuilder->getInt32(waveSize - 1)
                                                      });

    Value* pResult = UndefValue::get(pResultTy);
    pResult = m_pBuilder->CreateInsertValue(pResult, pOutPrimCount, 0);
    pResult = m_pBuilder->CreateInsertValue(pResult, pOutVertCount, 1);
    pResult = m_pBuilder->CreateInsertValue(pResult, pInclusiveOutVertCount, 2);
    pResult = m_pBuilder->CreateInsertValue(pResult, pOutVertCountInWave, 3);

    m_pBuilder->CreateRet(pResult); // Insert new "return" instruction

    // Clear removed calls
    for (auto pCall : removeCalls)
    {
        pCall->dropAllReferences();
        pCall->eraseFromParent();
    }

    m_pBuilder->restoreIP(savedInsertPos);

    return pGsEntryVariant;
}

// =====================================================================================================================
// Runs copy shader.
void NggPrimShader::RunCopyShader(
    Module*     pModule,        // [in] LLVM module
    BasicBlock* pInsertAtEnd)   // [in] Where to insert instructions
{
    assert(m_hasGs); // GS must be present

    auto pCopyShaderEntryPoint = pModule->getFunction(lgcName::NggCopyShaderEntryPoint);

    // Mutate copy shader entry-point, handle GS output import
    {
        auto pVertexOffset = GetFunctionArgument(pCopyShaderEntryPoint, CopyShaderUserSgprIdxVertexOffset);

        auto savedInsertPos = m_pBuilder->saveIP();

        std::vector<Instruction*> removeCalls;

        for (auto& func : pModule->functions())
        {
            if (func.getName().startswith(lgcName::NggGsOutputImport))
            {
                // Import GS outputs from GS-VS ring
                for (auto pUser : func.users())
                {
                    CallInst* const pCall = dyn_cast<CallInst>(pUser);
                    assert(pCall != nullptr);
                    m_pBuilder->SetInsertPoint(pCall);

                    assert(pCall->getNumArgOperands() == 3);
                    const uint32_t location = cast<ConstantInt>(pCall->getOperand(0))->getZExtValue();
                    const uint32_t compIdx = cast<ConstantInt>(pCall->getOperand(1))->getZExtValue();
                    const uint32_t streamId = cast<ConstantInt>(pCall->getOperand(2))->getZExtValue();
                    assert(streamId < MaxGsStreams);

                    auto pOutput = ImportGsOutput(pCall->getType(), location, compIdx, streamId, pVertexOffset);

                    pCall->replaceAllUsesWith(pOutput);
                    removeCalls.push_back(pCall);
                }
            }
        }

        // Clear removed calls
        for (auto pCall : removeCalls)
        {
            pCall->dropAllReferences();
            pCall->eraseFromParent();
        }

        m_pBuilder->restoreIP(savedInsertPos);
    }

    // Run copy shader
    {
        std::vector<Value*> args;

        static const uint32_t CopyShaderSysValueCount = 11; // Fixed layout: 10 SGPRs, 1 VGPR
        for (uint32_t i = 0; i < CopyShaderSysValueCount; ++i)
        {
            if (i == CopyShaderUserSgprIdxVertexOffset)
            {
                uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutVertOffset);

                auto pLdsOffset = m_pBuilder->CreateShl(m_nggFactor.pThreadIdInSubgroup, 2);
                pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));
                auto pVertexOffset = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);
                args.push_back(pVertexOffset);
            }
            else
            {
                // All SGPRs are not used
                args.push_back(UndefValue::get(GetFunctionArgument(pCopyShaderEntryPoint, i)->getType()));
            }
        }

        EmitCall(lgcName::NggCopyShaderEntryPoint,
                 m_pBuilder->getVoidTy(),
                 args,
                 {},
                 pInsertAtEnd);
    }
}

// =====================================================================================================================
// Exports outputs of geometry shader to GS-VS ring.
void NggPrimShader::ExportGsOutput(
    Value*       pOutput,               // [in] Output value
    uint32_t     location,              // Location of the output
    uint32_t     compIdx,               // Index used for vector element indexing
    uint32_t     streamId,              // ID of output vertex stream
    llvm::Value* pThreadIdInSubgroup,   // [in] Thread ID in sub-group
    Value*       pOutVertCounter)       // [in] GS output vertex counter for this stream
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    if (pResUsage->inOutUsage.gs.rasterStream != streamId)
    {
        // NOTE: Only export those outputs that belong to the rasterization stream.
        assert(pResUsage->inOutUsage.enableXfb == false); // Transform feedback must be disabled
        return;
    }

    // NOTE: We only handle LDS vector/scalar writing, so change [n x Ty] to <n x Ty> for array.
    auto pOutputTy = pOutput->getType();
    if (pOutputTy->isArrayTy())
    {
        auto pOutputElemTy = pOutputTy->getArrayElementType();
        assert(pOutputElemTy->isSingleValueType());

        // [n x Ty] -> <n x Ty>
        const uint32_t elemCount = pOutputTy->getArrayNumElements();
        Value* pOutputVec = UndefValue::get(VectorType::get(pOutputElemTy, elemCount));
        for (uint32_t i = 0; i < elemCount; ++i)
        {
            auto pOutputElem = m_pBuilder->CreateExtractValue(pOutput, i);
            m_pBuilder->CreateInsertElement(pOutputVec, pOutputElem, i);
        }

        pOutputTy = pOutputVec->getType();
        pOutput = pOutputVec;
    }

    const uint32_t bitWidth = pOutput->getType()->getScalarSizeInBits();
    if ((bitWidth == 8) || (bitWidth == 16))
    {
        // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend BYTE/WORD
        // to DWORD. This is because copy shader does not know the actual data type. It only generates output
        // export calls based on number of DWORDs.
        if (pOutputTy->isFPOrFPVectorTy())
        {
            assert(bitWidth == 16);
            Type* pCastTy = m_pBuilder->getInt16Ty();
            if (pOutputTy->isVectorTy())
            {
                pCastTy = VectorType::get(m_pBuilder->getInt16Ty(), pOutputTy->getVectorNumElements());
            }
            pOutput = m_pBuilder->CreateBitCast(pOutput, pCastTy);
        }

        Type* pExtTy = m_pBuilder->getInt32Ty();
        if (pOutputTy->isVectorTy())
        {
            pExtTy = VectorType::get(m_pBuilder->getInt32Ty(), pOutputTy->getVectorNumElements());
        }
        pOutput = m_pBuilder->CreateZExt(pOutput, pExtTy);
    }
    else
    {
        assert((bitWidth == 32) || (bitWidth == 64));
    }

    // gsVsRingOffset = threadIdInSubgroup * gsVsRingItemSize +
    //                  outVertcounter * vertexSize +
    //                  location * 4 + compIdx (in DWORDS)
    const uint32_t gsVsRingItemSize = pResUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize;
    Value* pGsVsRingOffset = m_pBuilder->CreateMul(pThreadIdInSubgroup, m_pBuilder->getInt32(gsVsRingItemSize));

    const uint32_t vertexSize = pResUsage->inOutUsage.gs.outLocCount[streamId] * 4;
    auto pVertexItemOffset = m_pBuilder->CreateMul(pOutVertCounter, m_pBuilder->getInt32(vertexSize));

    pGsVsRingOffset = m_pBuilder->CreateAdd(pGsVsRingOffset, pVertexItemOffset);

    const uint32_t attribOffset = (location * 4) + compIdx;
    pGsVsRingOffset = m_pBuilder->CreateAdd(pGsVsRingOffset, m_pBuilder->getInt32(attribOffset));

    // ldsOffset = gsVsRingStart + gsVsRingOffset * 4 (in BYTES)
    const uint32_t gsVsRingStart = m_pLdsManager->GetLdsRegionStart(LdsRegionGsVsRing);

    auto pLdsOffset = m_pBuilder->CreateShl(pGsVsRingOffset, 2);
    pLdsOffset = m_pBuilder->CreateAdd(m_pBuilder->getInt32(gsVsRingStart), pLdsOffset);

    m_pLdsManager->WriteValueToLds(pOutput, pLdsOffset);
}

// =====================================================================================================================
// Imports outputs of geometry shader from GS-VS ring.
Value* NggPrimShader::ImportGsOutput(
    Type*        pOutputTy,             // [in] Type of the output
    uint32_t     location,              // Location of the output
    uint32_t     compIdx,               // Index used for vector element indexing
    uint32_t     streamId,              // ID of output vertex stream
    Value*       pVertexOffset)         // [in] Start offset of vertex item in GS-VS ring (in BYTES)
{
    auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);
    if (pResUsage->inOutUsage.gs.rasterStream != streamId)
    {
        // NOTE: Only import those outputs that belong to the rasterization stream.
        assert(pResUsage->inOutUsage.enableXfb == false); // Transform feedback must be disabled
        return UndefValue::get(pOutputTy);
    }

    // NOTE: We only handle LDS vector/scalar reading, so change [n x Ty] to <n x Ty> for array.
    auto pOrigOutputTy = pOutputTy;
    if (pOutputTy->isArrayTy())
    {
        auto pOutputElemTy = pOutputTy->getArrayElementType();
        assert(pOutputElemTy->isSingleValueType());

        // [n x Ty] -> <n x Ty>
        const uint32_t elemCount = pOutputTy->getArrayNumElements();
        pOutputTy = VectorType::get(pOutputElemTy, elemCount);
    }

    // ldsOffset = vertexOffset + (location * 4 + compIdx) * 4 (in BYTES)
    const uint32_t attribOffset = (location * 4) + compIdx;
    auto pLdsOffset = m_pBuilder->CreateAdd(pVertexOffset, m_pBuilder->getInt32(attribOffset * 4));
    // Use 128-bit LDS load
    auto pOutput = m_pLdsManager->ReadValueFromLds(
        pOutputTy, pLdsOffset, (pOutputTy->getPrimitiveSizeInBits() == 128));

    if (pOrigOutputTy != pOutputTy)
    {
        assert(pOrigOutputTy->isArrayTy() && pOutputTy->isVectorTy() &&
                    (pOrigOutputTy->getArrayNumElements() == pOutputTy->getVectorNumElements()));

        // <n x Ty> -> [n x Ty]
        const uint32_t elemCount = pOrigOutputTy->getArrayNumElements();
        Value* pOutputArray = UndefValue::get(pOrigOutputTy);
        for (uint32_t i = 0; i < elemCount; ++i)
        {
            auto pOutputElem = m_pBuilder->CreateExtractElement(pOutput, i);
            pOutputArray = m_pBuilder->CreateInsertValue(pOutputArray, pOutputElem, i);
        }

        pOutput = pOutputArray;
    }

    return pOutput;
}

// =====================================================================================================================
// Processes the message GS_EMIT.
void NggPrimShader::ProcessGsEmit(
    Module*  pModule,                       // [in] LLVM module
    uint32_t streamId,                      // ID of output vertex stream
    Value*   pThreadIdInSubgroup,           // [in] Thread ID in subgroup
    Value*   pEmitCounterPtr,               // [in,out] Pointer to GS emit counter for this stream
    Value*   pOutVertCounterPtr,            // [in,out] Pointer to GS output vertex counter for this stream
    Value*   pOutPrimCounterPtr,            // [in,out] Pointer to GS output primitive counter for this stream
    Value*   pOutstandingVertCounterPtr,    // [in,out] Pointer to GS outstanding vertex counter for this stream
    Value*   pFlipVertOrderPtr)             // [in,out] Pointer to flags indicating whether to flip vertex ordering
{
    auto pGsEmitHandler = pModule->getFunction(lgcName::NggGsEmit);
    if (pGsEmitHandler == nullptr)
    {
        pGsEmitHandler = CreateGsEmitHandler(pModule, streamId);
    }

    m_pBuilder->CreateCall(pGsEmitHandler,
                           {
                               pThreadIdInSubgroup,
                               pEmitCounterPtr,
                               pOutVertCounterPtr,
                               pOutPrimCounterPtr,
                               pOutstandingVertCounterPtr,
                               pFlipVertOrderPtr
                           });
}

// =====================================================================================================================
// Processes the message GS_CUT.
void NggPrimShader::ProcessGsCut(
    Module*  pModule,                       // [in] LLVM module
    uint32_t streamId,                      // ID of output vertex stream
    Value*   pThreadIdInSubgroup,           // [in] Thread ID in subgroup
    Value*   pEmitCounterPtr,               // [in,out] Pointer to GS emit counter for this stream
    Value*   pOutVertCounterPtr,            // [in,out] Pointer to GS output vertex counter for this stream
    Value*   pOutPrimCounterPtr,            // [in,out] Pointer to GS output primitive counter for this stream
    Value*   pOutstandingVertCounterPtr,    // [in,out] Pointer to GS outstanding vertex counter for this stream
    Value*   pFlipVertOrderPtr)             // [in,out] Pointer to flags indicating whether to flip vertex ordering
{
    auto pGsCutHandler = pModule->getFunction(lgcName::NggGsCut);
    if (pGsCutHandler == nullptr)
    {
        pGsCutHandler = CreateGsCutHandler(pModule, streamId);
    }

    m_pBuilder->CreateCall(pGsCutHandler,
                           {
                               pThreadIdInSubgroup,
                               pEmitCounterPtr,
                               pOutVertCounterPtr,
                               pOutPrimCounterPtr,
                               pOutstandingVertCounterPtr,
                               pFlipVertOrderPtr
                           });
}

// =====================================================================================================================
// Creates the function that processes GS_EMIT.
Function* NggPrimShader::CreateGsEmitHandler(
    Module*     pModule,    // [in] LLVM module
    uint32_t    streamId)   // ID of output vertex stream
{
    assert(m_hasGs);

    //
    // The processing is something like this:
    //
    //   emitCounter++;
    //   outVertCounter++;
    //   outstandingVertCounter++;
    //   if (emitCounter == outVertsPerPrim)
    //   {
    //       Calculate primitive data and write it to LDS (valid primitive)
    //       outPrimCounter++;
    //       emitCounter--;
    //       outstandingVertCounter = 0;
    //       flipVertOrder = !flipVertOrder;
    //   }
    //
    const auto addrSpace = pModule->getDataLayout().getAllocaAddrSpace();
    auto pFuncTy =
        FunctionType::get(m_pBuilder->getVoidTy(),
                          {
                              m_pBuilder->getInt32Ty(),                                // %threadIdInSubgroup
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %emitCounterPtr
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %outVertCounterPtr
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %outPrimCounterPtr
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %outstandingVertCounterPtr
                              PointerType::get(m_pBuilder->getInt1Ty(),  addrSpace),   // %flipVertOrderPtr
                          },
                          false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggGsEmit, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pThreadIdInSubgroup = argIt++;
    pThreadIdInSubgroup->setName("threadIdInSubgroup");

    Value* pEmitCounterPtr = argIt++;
    pEmitCounterPtr->setName("emitCounterPtr");

    Value* pOutVertCounterPtr = argIt++;
    pOutVertCounterPtr->setName("outVertCounterPtr");

    Value* pOutPrimCounterPtr = argIt++;
    pOutPrimCounterPtr->setName("outPrimCounterPtr");

    Value* pOutstandingVertCounterPtr = argIt++;
    pOutstandingVertCounterPtr->setName("outstandingVertCounterPtr");

    Value* pFlipVertOrderPtr = argIt++; // Used by triangle strip
    pFlipVertOrderPtr->setName("flipVertOrderPtr");

    auto pEntryBlock = CreateBlock(pFunc, ".entry");
    auto pEmitPrimBlock = CreateBlock(pFunc, ".emitPrim");
    auto pEndEmitPrimBlock = CreateBlock(pFunc, ".endEmitPrim");

    auto savedInsertPoint = m_pBuilder->saveIP();

    const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
    const auto& pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);

    // Get GS output vertices per output primitive
    uint32_t outVertsPerPrim = 0;
    switch (geometryMode.outputPrimitive)
    {
    case OutputPrimitives::Points:
        outVertsPerPrim = 1;
        break;
    case OutputPrimitives::LineStrip:
        outVertsPerPrim = 2;
        break;
    case OutputPrimitives::TriangleStrip:
        outVertsPerPrim = 3;
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }
    auto pOutVertsPerPrim = m_pBuilder->getInt32(outVertsPerPrim);

    // Construct ".entry" block
    Value* pEmitCounter = nullptr;
    Value* pOutVertCounter = nullptr;
    Value* pOutPrimCounter = nullptr;
    Value* pOutstandingVertCounter = nullptr;
    Value* pFlipVertOrder = nullptr;
    Value* pPrimComplete = nullptr;
    {
        m_pBuilder->SetInsertPoint(pEntryBlock);

        pEmitCounter = m_pBuilder->CreateLoad(pEmitCounterPtr);
        pOutVertCounter = m_pBuilder->CreateLoad(pOutVertCounterPtr);
        pOutPrimCounter = m_pBuilder->CreateLoad(pOutPrimCounterPtr);
        pOutstandingVertCounter = m_pBuilder->CreateLoad(pOutstandingVertCounterPtr);

        // Flip vertex ordering only for triangle strip
        if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
        {
            pFlipVertOrder = m_pBuilder->CreateLoad(pFlipVertOrderPtr);
        }

        // emitCounter++
        pEmitCounter = m_pBuilder->CreateAdd(pEmitCounter, m_pBuilder->getInt32(1));

        // outVertCounter++
        pOutVertCounter = m_pBuilder->CreateAdd(pOutVertCounter, m_pBuilder->getInt32(1));

        // outstandingVertCounter++
        pOutstandingVertCounter = m_pBuilder->CreateAdd(pOutstandingVertCounter, m_pBuilder->getInt32(1));

        // primComplete = (emitCounter == outVertsPerPrim)
        pPrimComplete = m_pBuilder->CreateICmpEQ(pEmitCounter, pOutVertsPerPrim);
        m_pBuilder->CreateCondBr(pPrimComplete, pEmitPrimBlock, pEndEmitPrimBlock);
    }

    // Construct ".emitPrim" block
    {
        m_pBuilder->SetInsertPoint(pEmitPrimBlock);

        // NOTE: Only calculate GS output primitive data and write it to LDS for rasterization stream.
        if (streamId == pResUsage->inOutUsage.gs.rasterStream)
        {
            // vertexId = outVertCounter
            auto pvertexId = pOutVertCounter;

            // vertexId0 = vertexId - outVertsPerPrim
            auto pVertexId0 = m_pBuilder->CreateSub(pvertexId, pOutVertsPerPrim);

            // vertexId1 = vertexId - (outVertsPerPrim - 1) = vertexId0 + 1
            Value* pVertexId1 = nullptr;
            if (outVertsPerPrim > 1)
            {
                pVertexId1 = m_pBuilder->CreateAdd(pVertexId0, m_pBuilder->getInt32(1));
            }

            // vertexId2 = vertexId - (outVertsPerPrim - 2) = vertexId0 + 2
            Value* pVertexId2 = nullptr;
            if (outVertsPerPrim > 2)
            {
                pVertexId2 = m_pBuilder->CreateAdd(pVertexId0, m_pBuilder->getInt32(2));
            }

            // Primitive data layout [31:0]
            //   [31]    = null primitive flag
            //   [28:20] = vertexId2 (in bytes)
            //   [18:10] = vertexId1 (in bytes)
            //   [8:0]   = vertexId0 (in bytes)
            Value* pPrimData = nullptr;
            if (outVertsPerPrim == 1)
            {
                pPrimData = pVertexId0;
            }
            else if (outVertsPerPrim == 2)
            {
                pPrimData = m_pBuilder->CreateShl(pVertexId1, 10);
                pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId0);
            }
            else if (outVertsPerPrim == 3)
            {
                // Consider vertex ordering (normal: N -> N+1 -> N+2, flip: N -> N+2 -> N+1)
                pPrimData = m_pBuilder->CreateShl(pVertexId2, 10);
                pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId1);
                pPrimData = m_pBuilder->CreateShl(pPrimData, 10);
                pPrimData = m_pBuilder->CreateOr(pPrimData, pVertexId0);

                auto pPrimDataFlip = m_pBuilder->CreateShl(pVertexId1, 10);
                pPrimDataFlip = m_pBuilder->CreateOr(pPrimDataFlip, pVertexId2);
                pPrimDataFlip = m_pBuilder->CreateShl(pPrimDataFlip, 10);
                pPrimDataFlip = m_pBuilder->CreateOr(pPrimDataFlip, pVertexId0);

                pPrimData = m_pBuilder->CreateSelect(pFlipVertOrder, pPrimDataFlip, pPrimData);
            }
            else
            {
                llvm_unreachable("Should never be called!");
            }

            const uint32_t maxOutPrims = pResUsage->inOutUsage.gs.calcFactor.primAmpFactor;

            uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutPrimData);

            // ldsOffset = regionStart + (threadIdInSubgroup * maxOutPrims + outPrimCounter) * 4
            auto pLdsOffset = m_pBuilder->CreateMul(pThreadIdInSubgroup, m_pBuilder->getInt32(maxOutPrims));
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pOutPrimCounter);
            pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, 2);
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

            m_pLdsManager->WriteValueToLds(pPrimData, pLdsOffset);
        }

        m_pBuilder->CreateBr(pEndEmitPrimBlock);
    }

    // Construct ".endEmitPrim" block
    {
        m_pBuilder->SetInsertPoint(pEndEmitPrimBlock);

        // NOTE: We use selection instruction to update values of emit counter and GS output primitive counter. This is
        // friendly to CFG simplification.
        auto pEmitCounterDec = m_pBuilder->CreateSub(pEmitCounter, m_pBuilder->getInt32(1));
        auto pOutPrimCounterInc = m_pBuilder->CreateAdd(pOutPrimCounter, m_pBuilder->getInt32(1));

        // if (primComplete) emitCounter--
        pEmitCounter = m_pBuilder->CreateSelect(pPrimComplete, pEmitCounterDec, pEmitCounter);

        // if (primComplete) outPrimCounter++
        pOutPrimCounter = m_pBuilder->CreateSelect(pPrimComplete, pOutPrimCounterInc, pOutPrimCounter);

        // if (primComplete) outstandingVertCounter = 0
        pOutstandingVertCounter =
            m_pBuilder->CreateSelect(pPrimComplete, m_pBuilder->getInt32(0), pOutstandingVertCounter);

        m_pBuilder->CreateStore(pEmitCounter, pEmitCounterPtr);
        m_pBuilder->CreateStore(pOutVertCounter, pOutVertCounterPtr);
        m_pBuilder->CreateStore(pOutPrimCounter, pOutPrimCounterPtr);
        m_pBuilder->CreateStore(pOutstandingVertCounter, pOutstandingVertCounterPtr);

        // Flip vertex ordering only for triangle strip
        if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
        {
            // if (primComplete) flipVertOrder = !flipVertOrder
            pFlipVertOrder = m_pBuilder->CreateSelect(
                pPrimComplete, m_pBuilder->CreateNot(pFlipVertOrder), pFlipVertOrder);
            m_pBuilder->CreateStore(pFlipVertOrder, pFlipVertOrderPtr);
        }

        m_pBuilder->CreateRetVoid();
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that processes GS_EMIT.
Function* NggPrimShader::CreateGsCutHandler(
    Module*     pModule,    // [in] LLVM module
    uint32_t    streamId)   // ID of output vertex stream
{
    assert(m_hasGs);

    //
    // The processing is something like this:
    //
    //   if ((emitCounter > 0) && (emitCounter != outVertsPerPrim) && (outPrimCounter < maxOutPrims))
    //   {
    //       Write primitive data to LDS (invalid primitive)
    //       outPrimCounter++;
    //   }
    //   emitCounter = 0;
    //   outVertCounter -= outstandingVertCounter;
    //   outstandingVertCounter = 0;
    //   flipVertOrder = false;
    //
    const auto addrSpace = pModule->getDataLayout().getAllocaAddrSpace();
    auto pFuncTy =
        FunctionType::get(m_pBuilder->getVoidTy(),
                          {
                              m_pBuilder->getInt32Ty(),                                // %threadIdInSubgroup
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %emitCounterPtr
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %outVertCounterPtr
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %outPrimCounterPtr
                              PointerType::get(m_pBuilder->getInt32Ty(), addrSpace),   // %outstandingVertCounterPtr
                              PointerType::get(m_pBuilder->getInt1Ty(),  addrSpace),   // %flipVertOrderPtr
                          },
                          false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggGsCut, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pThreadIdInSubgroup = argIt++;
    pThreadIdInSubgroup->setName("threadIdInSubgroup");

    Value* pEmitCounterPtr = argIt++;
    pEmitCounterPtr->setName("emitCounterPtr");

    Value* pOutVertCounterPtr = argIt++;
    pOutVertCounterPtr->setName("outVertCounterPtr");

    Value* pOutPrimCounterPtr = argIt++;
    pOutPrimCounterPtr->setName("outPrimCounterPtr");

    Value* pOutstandingVertCounterPtr = argIt++;
    pOutstandingVertCounterPtr->setName("outstandingVertCounterPtr");

    Value* pFlipVertOrderPtr = argIt++; // Used by triangle strip
    pFlipVertOrderPtr->setName("flipVertOrderPtr");

    auto pEntryBlock = CreateBlock(pFunc, ".entry");
    auto pEmitPrimBlock = CreateBlock(pFunc, ".emitPrim");
    auto pEndEmitPrimBlock = CreateBlock(pFunc, ".endEmitPrim");

    auto savedInsertPoint = m_pBuilder->saveIP();

    const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
    const auto& pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);

    // Get GS output vertices per output primitive
    uint32_t outVertsPerPrim = 0;
    switch (geometryMode.outputPrimitive)
    {
    case OutputPrimitives::Points:
        outVertsPerPrim = 1;
        break;
    case OutputPrimitives::LineStrip:
        outVertsPerPrim = 2;
        break;
    case OutputPrimitives::TriangleStrip:
        outVertsPerPrim = 3;
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }
    auto pOutVertsPerPrim = m_pBuilder->getInt32(outVertsPerPrim);

    const uint32_t maxOutPrims = pResUsage->inOutUsage.gs.calcFactor.primAmpFactor;
    auto pMaxOutPrims = m_pBuilder->getInt32(maxOutPrims);

    // Construct ".entry" block
    Value* pEmitCounter = nullptr;
    Value* pOutPrimCounter = nullptr;
    Value* pPrimIncomplete = nullptr;
    {
        m_pBuilder->SetInsertPoint(pEntryBlock);

        pEmitCounter = m_pBuilder->CreateLoad(pEmitCounterPtr);
        pOutPrimCounter = m_pBuilder->CreateLoad(pOutPrimCounterPtr);

        // hasEmit = (emitCounter > 0)
        auto hasEmit = m_pBuilder->CreateICmpUGT(pEmitCounter, m_pBuilder->getInt32(0));

        // primIncomplete = (emitCounter != outVertsPerPrim)
        pPrimIncomplete = m_pBuilder->CreateICmpNE(pEmitCounter, pOutVertsPerPrim);

        // validPrimCounter = (outPrimCounter < maxOutPrims)
        auto pValidPrimCounter = m_pBuilder->CreateICmpULT(pOutPrimCounter, pMaxOutPrims);

        pPrimIncomplete = m_pBuilder->CreateAnd(hasEmit, pPrimIncomplete);
        pPrimIncomplete = m_pBuilder->CreateAnd(pPrimIncomplete, pValidPrimCounter);

        m_pBuilder->CreateCondBr(pPrimIncomplete, pEmitPrimBlock, pEndEmitPrimBlock);
    }

    // Construct ".emitPrim" block
    {
        m_pBuilder->SetInsertPoint(pEmitPrimBlock);

        // NOTE: Only write incomplete GS output primitive to LDS for rasterization stream.
        if (streamId == pResUsage->inOutUsage.gs.rasterStream)
        {
            uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutPrimData);

            // ldsOffset = regionStart + (threadIdInSubgroup * maxOutPrims + outPrimCounter) * 4
            auto pLdsOffset = m_pBuilder->CreateMul(pThreadIdInSubgroup, m_pBuilder->getInt32(maxOutPrims));
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pOutPrimCounter);
            pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, 2);
            pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

            auto pNullPrim = m_pBuilder->getInt32(NullPrim);
            m_pLdsManager->WriteValueToLds(pNullPrim, pLdsOffset);
        }

        m_pBuilder->CreateBr(pEndEmitPrimBlock);
    }

    // Construct ".endEmitPrim" block
    {
        m_pBuilder->SetInsertPoint(pEndEmitPrimBlock);

        // Reset emit counter
        m_pBuilder->CreateStore(m_pBuilder->getInt32(0), pEmitCounterPtr);

        // NOTE: We use selection instruction to update the value of GS output primitive counter. This is
        // friendly to CFG simplification.

        // if (primComplete) outPrimCounter++
        auto pOutPrimCounterInc = m_pBuilder->CreateAdd(pOutPrimCounter, m_pBuilder->getInt32(1));
        pOutPrimCounter = m_pBuilder->CreateSelect(pPrimIncomplete, pOutPrimCounterInc, pOutPrimCounter);
        m_pBuilder->CreateStore(pOutPrimCounter, pOutPrimCounterPtr);

        // outVertCounter -= outstandingVertCounter
        Value* pOutVertCounter = m_pBuilder->CreateLoad(pOutVertCounterPtr);
        Value* pOutstandingVertCounter = m_pBuilder->CreateLoad(pOutstandingVertCounterPtr);

        pOutVertCounter = m_pBuilder->CreateSub(pOutVertCounter, pOutstandingVertCounter);
        m_pBuilder->CreateStore(pOutVertCounter, pOutVertCounterPtr);

        // Reset outstanding vertex counter
        m_pBuilder->CreateStore(m_pBuilder->getInt32(0), pOutstandingVertCounterPtr);

        // Flip vertex ordering only for triangle strip
        if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
        {
            // flipVertOrder = false
            m_pBuilder->CreateStore(m_pBuilder->getFalse(), pFlipVertOrderPtr);
        }

        m_pBuilder->CreateRetVoid();
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Revises GS output primitive data. The data in LDS region "OutPrimData" contains vertex indices representing the
// connectivity of this primitive. The vertex indices were "thread-view" values before this revising. They are the output
// vertices emitted by this GS thread. After revising, the index values are "subgroup-view" ones, corresponding to the
// output vertices emitted by the whole GS sub-group. Thus, number of output vertices prior to this GS thread is
// counted in.
void NggPrimShader::ReviseOutputPrimitiveData(
    Value* pOutPrimId,       // [in] GS output primitive ID
    Value* pVertexIdAdjust)  // [in] Adjustment of vertex indices corresponding to the GS output primitive
{
    const auto& geometryMode = m_pPipelineState->GetShaderModes()->GetGeometryShaderMode();
    const auto pResUsage = m_pPipelineState->GetShaderResourceUsage(ShaderStageGeometry);

    uint32_t regionStart = m_pLdsManager->GetLdsRegionStart(LdsRegionOutPrimData);

    // ldsOffset = regionStart + (threadIdInSubgroup * maxOutPrims + outPrimId) * 4
    const uint32_t maxOutPrims = pResUsage->inOutUsage.gs.calcFactor.primAmpFactor;
    auto pLdsOffset = m_pBuilder->CreateMul(m_nggFactor.pThreadIdInSubgroup, m_pBuilder->getInt32(maxOutPrims));
    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, pOutPrimId);
    pLdsOffset = m_pBuilder->CreateShl(pLdsOffset, m_pBuilder->getInt32(2));
    pLdsOffset = m_pBuilder->CreateAdd(pLdsOffset, m_pBuilder->getInt32(regionStart));

    auto pPrimData = m_pLdsManager->ReadValueFromLds(m_pBuilder->getInt32Ty(), pLdsOffset);

    // Get GS output vertices per output primitive
    uint32_t outVertsPerPrim = 0;
    switch (geometryMode.outputPrimitive)
    {
    case OutputPrimitives::Points:
        outVertsPerPrim = 1;
        break;
    case OutputPrimitives::LineStrip:
        outVertsPerPrim = 2;
        break;
    case OutputPrimitives::TriangleStrip:
        outVertsPerPrim = 3;
        break;
    default:
        llvm_unreachable("Should never be called!");
        break;
    }

    // Primitive data layout [31:0]
    //   [31]    = null primitive flag
    //   [28:20] = vertexId2 (in bytes)
    //   [18:10] = vertexId1 (in bytes)
    //   [8:0]   = vertexId0 (in bytes)
    Value* pVertexId0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_pBuilder->getInt32Ty(),
                                                    {
                                                        pPrimData,
                                                        m_pBuilder->getInt32(0),
                                                        m_pBuilder->getInt32(9)
                                                    });
    pVertexId0 = m_pBuilder->CreateAdd(pVertexIdAdjust, pVertexId0);

    Value* pVertexId1 = nullptr;
    if (outVertsPerPrim > 1)
    {
        pVertexId1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_pBuilder->getInt32Ty(),
                                                 {
                                                     pPrimData,
                                                     m_pBuilder->getInt32(10),
                                                     m_pBuilder->getInt32(9)
                                                 });
        pVertexId1 = m_pBuilder->CreateAdd(pVertexIdAdjust, pVertexId1);
    }

    Value* pVertexId2 = nullptr;
    if (outVertsPerPrim > 2)
    {
        pVertexId2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_pBuilder->getInt32Ty(),
                                                 {
                                                     pPrimData,
                                                     m_pBuilder->getInt32(20),
                                                     m_pBuilder->getInt32(9)
                                                 });
        pVertexId2 = m_pBuilder->CreateAdd(pVertexIdAdjust, pVertexId2);
    }

    Value* pNewPrimData = nullptr;
    if (outVertsPerPrim == 1)
    {
        pNewPrimData = pVertexId0;
    }
    else if (outVertsPerPrim == 2)
    {
        pNewPrimData = m_pBuilder->CreateShl(pVertexId1, 10);
        pNewPrimData = m_pBuilder->CreateOr(pNewPrimData, pVertexId0);
    }
    else if (outVertsPerPrim == 3)
    {
        pNewPrimData = m_pBuilder->CreateShl(pVertexId2, 10);
        pNewPrimData = m_pBuilder->CreateOr(pNewPrimData, pVertexId1);
        pNewPrimData = m_pBuilder->CreateShl(pNewPrimData, 10);
        pNewPrimData = m_pBuilder->CreateOr(pNewPrimData, pVertexId0);
    }
    else
    {
        llvm_unreachable("Should never be called!");
    }

    auto pIsNullPrim = m_pBuilder->CreateICmpEQ(pPrimData, m_pBuilder->getInt32(NullPrim));
    pNewPrimData = m_pBuilder->CreateSelect(pIsNullPrim, m_pBuilder->getInt32(NullPrim), pNewPrimData);

    m_pLdsManager->WriteValueToLds(pNewPrimData, pLdsOffset);
}

// =====================================================================================================================
// Reads per-thread data from the specified NGG region in LDS.
Value* NggPrimShader::ReadPerThreadDataFromLds(
    Type*             pReadDataTy,  // [in] Data written to LDS
    Value*            pThreadId,    // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType  region)       // NGG LDS region
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
// Writes the per-thread data to the specified NGG region in LDS.
void NggPrimShader::WritePerThreadDataToLds(
    Value*           pWriteData,        // [in] Data written to LDS
    Value*           pThreadId,         // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType region)            // NGG LDS region
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
    Value*      pVertex2)       // [in] Position data of vertex2
{
    assert(m_pNggControl->enableBackfaceCulling);

    auto pBackfaceCuller = pModule->getFunction(lgcName::NggCullingBackface);
    if (pBackfaceCuller == nullptr)
    {
        pBackfaceCuller = CreateBackfaceCuller(pModule);
    }

    uint32_t regOffset = 0;

    // Get register PA_SU_SC_MODE_CNTL
    Value* pPaSuScModeCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paSuScModeCntl);
        pPaSuScModeCntl = FetchCullingControlRegister(pModule, regOffset);
    }
    else
    {
        pPaSuScModeCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paSuScModeCntl);
    }

    // Get register PA_CL_VPORT_XSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    auto pPaClVportXscale = FetchCullingControlRegister(pModule, regOffset);

    // Get register PA_CL_VPORT_YSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    auto pPaClVportYscale = FetchCullingControlRegister(pModule, regOffset);

    // Do backface culling
    return m_pBuilder->CreateCall(pBackfaceCuller,
                                  {
                                      pCullFlag,
                                      pVertex0,
                                      pVertex1,
                                      pVertex2,
                                      m_pBuilder->getInt32(m_pNggControl->backfaceExponent),
                                      pPaSuScModeCntl,
                                      pPaClVportXscale,
                                      pPaClVportYscale
                                  });
}

// =====================================================================================================================
// Frustum culler.
Value* NggPrimShader::DoFrustumCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2)       // [in] Position data of vertex2
{
    assert(m_pNggControl->enableFrustumCulling);

    auto pFrustumCuller = pModule->getFunction(lgcName::NggCullingFrustum);
    if (pFrustumCuller == nullptr)
    {
        pFrustumCuller = CreateFrustumCuller(pModule);
    }

    uint32_t regOffset = 0;

    // Get register PA_CL_CLIP_CNTL
    Value* pPaClClipCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        pPaClClipCntl = FetchCullingControlRegister(pModule, regOffset);
    }
    else
    {
        pPaClClipCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
    }

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto pPaClGbHorzDiscAdj = FetchCullingControlRegister(pModule, regOffset);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto pPaClGbVertDiscAdj = FetchCullingControlRegister(pModule, regOffset);

    // Do frustum culling
    return m_pBuilder->CreateCall(pFrustumCuller,
                                  {
                                      pCullFlag,
                                      pVertex0,
                                      pVertex1,
                                      pVertex2,
                                      pPaClClipCntl,
                                      pPaClGbHorzDiscAdj,
                                      pPaClGbVertDiscAdj
                                  });
}

// =====================================================================================================================
// Box filter culler.
Value* NggPrimShader::DoBoxFilterCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2)       // [in] Position data of vertex2
{
    assert(m_pNggControl->enableBoxFilterCulling);

    auto pBoxFilterCuller = pModule->getFunction(lgcName::NggCullingBoxFilter);
    if (pBoxFilterCuller == nullptr)
    {
        pBoxFilterCuller = CreateBoxFilterCuller(pModule);
    }

    uint32_t regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* pPaClVteCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_CLIP_CNTL
    Value* pPaClClipCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        pPaClClipCntl = FetchCullingControlRegister(pModule, regOffset);
    }
    else
    {
        pPaClClipCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
    }

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto pPaClGbHorzDiscAdj = FetchCullingControlRegister(pModule, regOffset);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto pPaClGbVertDiscAdj = FetchCullingControlRegister(pModule, regOffset);

    // Do box filter culling
    return m_pBuilder->CreateCall(pBoxFilterCuller,
                                  {
                                      pCullFlag,
                                      pVertex0,
                                      pVertex1,
                                      pVertex2,
                                      pPaClVteCntl,
                                      pPaClClipCntl,
                                      pPaClGbHorzDiscAdj,
                                      pPaClGbVertDiscAdj
                                  });
}

// =====================================================================================================================
// Sphere culler.
Value* NggPrimShader::DoSphereCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2)       // [in] Position data of vertex2
{
    assert(m_pNggControl->enableSphereCulling);

    auto pSphereCuller = pModule->getFunction(lgcName::NggCullingSphere);
    if (pSphereCuller == nullptr)
    {
        pSphereCuller = CreateSphereCuller(pModule);
    }

    uint32_t regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* pPaClVteCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_CLIP_CNTL
    Value* pPaClClipCntl = nullptr;
    if (m_pNggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        pPaClClipCntl = FetchCullingControlRegister(pModule, regOffset);
    }
    else
    {
        pPaClClipCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClClipCntl);
    }

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto pPaClGbHorzDiscAdj = FetchCullingControlRegister(pModule, regOffset);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto pPaClGbVertDiscAdj = FetchCullingControlRegister(pModule, regOffset);

    // Do small primitive filter culling
    return m_pBuilder->CreateCall(pSphereCuller,
                                  {
                                      pCullFlag,
                                      pVertex0,
                                      pVertex1,
                                      pVertex2,
                                      pPaClVteCntl,
                                      pPaClClipCntl,
                                      pPaClGbHorzDiscAdj,
                                      pPaClGbVertDiscAdj
                                  });
}

// =====================================================================================================================
// Small primitive filter culler.
Value* NggPrimShader::DoSmallPrimFilterCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pVertex0,       // [in] Position data of vertex0
    Value*      pVertex1,       // [in] Position data of vertex1
    Value*      pVertex2)       // [in] Position data of vertex2
{
    assert(m_pNggControl->enableSmallPrimFilter);

    auto pSmallPrimFilterCuller = pModule->getFunction(lgcName::NggCullingSmallPrimFilter);
    if (pSmallPrimFilterCuller == nullptr)
    {
        pSmallPrimFilterCuller = CreateSmallPrimFilterCuller(pModule);
    }

    uint32_t regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* pPaClVteCntl = m_pBuilder->getInt32(m_pNggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_VPORT_XSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    auto pPaClVportXscale = FetchCullingControlRegister(pModule, regOffset);

    // Get register PA_CL_VPORT_YSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    auto pPaClVportYscale = FetchCullingControlRegister(pModule, regOffset);

    // Do small primitive filter culling
    return m_pBuilder->CreateCall(pSmallPrimFilterCuller,
                                  {
                                      pCullFlag,
                                      pVertex0,
                                      pVertex1,
                                      pVertex2,
                                      pPaClVteCntl,
                                      pPaClVportXscale,
                                      pPaClVportYscale
                                  });
}

// =====================================================================================================================
// Cull distance culler.
Value* NggPrimShader::DoCullDistanceCulling(
    Module*     pModule,        // [in] LLVM module
    Value*      pCullFlag,      // [in] Cull flag before doing this culling
    Value*      pSignMask0,     // [in] Sign mask of cull distance of vertex0
    Value*      pSignMask1,     // [in] Sign mask of cull distance of vertex1
    Value*      pSignMask2)     // [in] Sign mask of cull distance of vertex2
{
    assert(m_pNggControl->enableCullDistanceCulling);

    auto pCullDistanceCuller = pModule->getFunction(lgcName::NggCullingCullDistance);
    if (pCullDistanceCuller == nullptr)
    {
        pCullDistanceCuller = CreateCullDistanceCuller(pModule);
    }

    // Do cull distance culling
    return m_pBuilder->CreateCall(pCullDistanceCuller,
                                  {
                                      pCullFlag,
                                      pSignMask0,
                                      pSignMask1,
                                      pSignMask2
                                  });
}

// =====================================================================================================================
// Fetches culling-control register from primitive shader table.
Value* NggPrimShader::FetchCullingControlRegister(
    Module*     pModule,        // [in] LLVM module
    uint32_t    regOffset)      // Register offset in the primitive shader table (in BYTEs)
{
    auto pFetchCullingRegister = pModule->getFunction(lgcName::NggCullingFetchReg);
    if (pFetchCullingRegister == nullptr)
    {
        pFetchCullingRegister = CreateFetchCullingRegister(pModule);
    }

    return m_pBuilder->CreateCall(pFetchCullingRegister,
                                  {
                                      m_nggFactor.pPrimShaderTableAddrLow,
                                      m_nggFactor.pPrimShaderTableAddrHigh,
                                      m_pBuilder->getInt32(regOffset)
                                  });
}

// =====================================================================================================================
// Creates the function that does backface culling.
Function* NggPrimShader::CreateBackfaceCuller(
    Module* pModule) // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt1Ty(),
                                     {
                                         m_pBuilder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex2
                                         m_pBuilder->getInt32Ty(),                          // %backfaceExponent
                                         m_pBuilder->getInt32Ty(),                          // %paSuScModeCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClVportXscale
                                         m_pBuilder->getInt32Ty()                           // %paClVportYscale
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingBackface, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadNone);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pCullFlag = argIt++;
    pCullFlag->setName("cullFlag");

    Value* pVertex0 = argIt++;
    pVertex0->setName("vertex0");

    Value* pVertex1 = argIt++;
    pVertex1->setName("vertex1");

    Value* pVertex2 = argIt++;
    pVertex2->setName("vertex2");

    Value* pBackfaceExponent = argIt++;
    pBackfaceExponent->setName("backfaceExponent");

    Value* pPaSuScModeCntl = argIt++;
    pPaSuScModeCntl->setName("paSuScModeCntl");

    Value* pPaClVportXscale = argIt++;
    pPaClVportXscale->setName("paClVportXscale");

    Value* pPaClVportYscale = argIt++;
    pPaClVportYscale->setName("paClVportYscale");

    auto pBackfaceEntryBlock = CreateBlock(pFunc, ".backfaceEntry");
    auto pBackfaceCullBlock = CreateBlock(pFunc, ".backfaceCull");
    auto pBackfaceExponentBlock = CreateBlock(pFunc, ".backfaceExponent");
    auto pEndBackfaceCullBlock = CreateBlock(pFunc, ".endBackfaceCull");
    auto pBackfaceExitBlock = CreateBlock(pFunc, ".backfaceExit");

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".backfaceEntry" block
    {
        m_pBuilder->SetInsertPoint(pBackfaceEntryBlock);
        // If cull flag has already been TRUE, early return
        m_pBuilder->CreateCondBr(pCullFlag, pBackfaceExitBlock, pBackfaceCullBlock);
    }

    // Construct ".backfaceCull" block
    Value* pCullFlag1 = nullptr;
    Value* pW0 = nullptr;
    Value* pW1 = nullptr;
    Value* pW2 = nullptr;
    Value* pArea = nullptr;
    {
        m_pBuilder->SetInsertPoint(pBackfaceCullBlock);

        //
        // Backface culling algorithm is described as follow:
        //
        //   if (((area > 0) && (face == CCW)) || ((area < 0) && (face == CW)))
        //       frontFace = true
        //
        //   if (((area < 0) && (face == CCW)) || ((area > 0) && (face == CW)))
        //       backFace = true
        //
        //   if ((area == 0) || (frontFace && cullFront) || (backFace && cullBack))
        //       cullFlag = true
        //

        //        | x0 y0 w0 |
        //        |          |
        // area = | x1 y1 w1 | =  x0 * (y1 * w2 - y2 * w1) - x1 * (y0 * w2 - y2 * w0) + x2 * (y0 * w1 - y1 * w0)
        //        |          |
        //        | x2 y2 w2 |
        //
        auto pX0 = m_pBuilder->CreateExtractElement(pVertex0, static_cast<uint64_t>(0));
        auto pY0 = m_pBuilder->CreateExtractElement(pVertex0, 1);
        pW0 = m_pBuilder->CreateExtractElement(pVertex0, 3);

        auto pX1 = m_pBuilder->CreateExtractElement(pVertex1, static_cast<uint64_t>(0));
        auto pY1 = m_pBuilder->CreateExtractElement(pVertex1, 1);
        pW1 = m_pBuilder->CreateExtractElement(pVertex1, 3);

        auto pX2 = m_pBuilder->CreateExtractElement(pVertex2, static_cast<uint64_t>(0));
        auto pY2 = m_pBuilder->CreateExtractElement(pVertex2, 1);
        pW2 = m_pBuilder->CreateExtractElement(pVertex2, 3);

        auto pY1W2 = m_pBuilder->CreateFMul(pY1, pW2);
        auto pY2W1 = m_pBuilder->CreateFMul(pY2, pW1);
        auto pDet0 = m_pBuilder->CreateFSub(pY1W2, pY2W1);
        pDet0 = m_pBuilder->CreateFMul(pX0, pDet0);

        auto pY0W2 = m_pBuilder->CreateFMul(pY0, pW2);
        auto pY2W0 = m_pBuilder->CreateFMul(pY2, pW0);
        auto pDet1 = m_pBuilder->CreateFSub(pY0W2, pY2W0);
        pDet1 = m_pBuilder->CreateFMul(pX1, pDet1);

        auto pY0W1 = m_pBuilder->CreateFMul(pY0, pW1);
        auto pY1W0 = m_pBuilder->CreateFMul(pY1, pW0);
        auto pDet2 = m_pBuilder->CreateFSub(pY0W1, pY1W0);
        pDet2 = m_pBuilder->CreateFMul(pX2, pDet2);

        pArea = m_pBuilder->CreateFSub(pDet0, pDet1);
        pArea = m_pBuilder->CreateFAdd(pArea, pDet2);

        auto pAreaLtZero = m_pBuilder->CreateFCmpOLT(pArea, ConstantFP::get(m_pBuilder->getFloatTy(), 0.0));
        auto pAreaGtZero = m_pBuilder->CreateFCmpOGT(pArea, ConstantFP::get(m_pBuilder->getFloatTy(), 0.0));

        // xScale ^ yScale
        auto pFrontFace = m_pBuilder->CreateXor(pPaClVportXscale, pPaClVportYscale);

        // signbit(xScale ^ yScale)
        pFrontFace = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_pBuilder->getInt32Ty(),
                                                 {
                                                     pFrontFace,
                                                     m_pBuilder->getInt32(31),
                                                     m_pBuilder->getInt32(1)
                                                 });

        // face = (FACE, PA_SU_SC_MODE_CNTRL[2], 0 = CCW, 1 = CW)
        auto pFace = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_pBuilder->getInt32Ty(),
                                                 {
                                                     pPaSuScModeCntl,
                                                     m_pBuilder->getInt32(2),
                                                     m_pBuilder->getInt32(1)
                                                 });

        // face ^ signbit(xScale ^ yScale)
        pFrontFace = m_pBuilder->CreateXor(pFace, pFrontFace);

        // (face ^ signbit(xScale ^ yScale)) == 0
        pFrontFace = m_pBuilder->CreateICmpEQ(pFrontFace, m_pBuilder->getInt32(0));

        // frontFace = ((face ^ signbit(xScale ^ yScale)) == 0) ? (area < 0) : (area > 0)
        pFrontFace = m_pBuilder->CreateSelect(pFrontFace, pAreaLtZero, pAreaGtZero);

        // backFace = !frontFace
        auto pBackFace = m_pBuilder->CreateNot(pFrontFace);

        // cullFront = (CULL_FRONT, PA_SU_SC_MODE_CNTRL[0], 0 = DONT CULL, 1 = CULL)
        auto pCullFront = m_pBuilder->CreateAnd(pPaSuScModeCntl, m_pBuilder->getInt32(1));
        pCullFront = m_pBuilder->CreateTrunc(pCullFront, m_pBuilder->getInt1Ty());

        // cullBack = (CULL_BACK, PA_SU_SC_MODE_CNTRL[1], 0 = DONT CULL, 1 = CULL)
        Value* pCullBack = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_pBuilder->getInt32Ty(),
                                                       {
                                                           pPaSuScModeCntl,
                                                           m_pBuilder->getInt32(1),
                                                           m_pBuilder->getInt32(1)
                                                       });
        pCullBack = m_pBuilder->CreateTrunc(pCullBack, m_pBuilder->getInt1Ty());

        // cullFront = cullFront ? frontFace : false
        pCullFront = m_pBuilder->CreateSelect(pCullFront, pFrontFace, m_pBuilder->getFalse());

        // cullBack = cullBack ? backFace : false
        pCullBack = m_pBuilder->CreateSelect(pCullBack, pBackFace, m_pBuilder->getFalse());

        // cullFlag = cullFront || cullBack
        pCullFlag1 = m_pBuilder->CreateOr(pCullFront, pCullBack);

        auto pNonZeroBackfaceExp = m_pBuilder->CreateICmpNE(pBackfaceExponent, m_pBuilder->getInt32(0));
        m_pBuilder->CreateCondBr(pNonZeroBackfaceExp, pBackfaceExponentBlock, pEndBackfaceCullBlock);
    }

    // Construct ".backfaceExponent" block
    Value* pCullFlag2 = nullptr;
    {
        m_pBuilder->SetInsertPoint(pBackfaceExponentBlock);

        //
        // Ignore area calculations that are less enough
        //   if (|area| < (10 ^ (-backfaceExponent)) / |w0 * w1 * w2| )
        //       cullFlag = false
        //

        // |w0 * w1 * w2|
        auto pAbsW0W1W2 = m_pBuilder->CreateFMul(pW0, pW1);
        pAbsW0W1W2 = m_pBuilder->CreateFMul(pAbsW0W1W2, pW2);
        pAbsW0W1W2 = m_pBuilder->CreateIntrinsic(Intrinsic::fabs, m_pBuilder->getFloatTy(), pAbsW0W1W2);

        // threeshold = (10 ^ (-backfaceExponent)) / |w0 * w1 * w2|
        auto pThreshold = m_pBuilder->CreateNeg(pBackfaceExponent);
        pThreshold = m_pBuilder->CreateIntrinsic(Intrinsic::powi,
                                                 m_pBuilder->getFloatTy(),
                                                 {
                                                     ConstantFP::get(m_pBuilder->getFloatTy(), 10.0),
                                                     pThreshold
                                                 });

        auto pRcpAbsW0W1W2 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pAbsW0W1W2);
        pThreshold = m_pBuilder->CreateFMul(pThreshold, pRcpAbsW0W1W2);

        // |area|
        auto pAbsArea = m_pBuilder->CreateIntrinsic(Intrinsic::fabs, m_pBuilder->getFloatTy(), pArea);

        // cullFlag = cullFlag && (abs(area) >= threshold)
        pCullFlag2 = m_pBuilder->CreateFCmpOGE(pAbsArea, pThreshold);
        pCullFlag2 = m_pBuilder->CreateAnd(pCullFlag1, pCullFlag2);

        m_pBuilder->CreateBr(pEndBackfaceCullBlock);
    }

    // Construct ".endBackfaceCull" block
    Value* pCullFlag3 = nullptr;
    {
        m_pBuilder->SetInsertPoint(pEndBackfaceCullBlock);

        // cullFlag = cullFlag || (area == 0)
        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag1, pBackfaceCullBlock);
        pCullFlagPhi->addIncoming(pCullFlag2, pBackfaceExponentBlock);

        auto pAreaEqZero = m_pBuilder->CreateFCmpOEQ(pArea, ConstantFP::get(m_pBuilder->getFloatTy(), 0.0));

        pCullFlag3 = m_pBuilder->CreateOr(pCullFlagPhi, pAreaEqZero);

        m_pBuilder->CreateBr(pBackfaceExitBlock);
    }

    // Construct ".backfaceExit" block
    {
        m_pBuilder->SetInsertPoint(pBackfaceExitBlock);

        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag, pBackfaceEntryBlock);
        pCullFlagPhi->addIncoming(pCullFlag3, pEndBackfaceCullBlock);

        // polyMode = (POLY_MODE, PA_SU_SC_MODE_CNTRL[4:3], 0 = DISABLE, 1 = DUAL)
        auto pPolyMode = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                     m_pBuilder->getInt32Ty(),
                                                     {
                                                         pPaSuScModeCntl,
                                                         m_pBuilder->getInt32(3),
                                                         m_pBuilder->getInt32(2),
                                                     });

        // polyMode == 1
        auto pWireFrameMode = m_pBuilder->CreateICmpEQ(pPolyMode, m_pBuilder->getInt32(1));

        // Disable backface culler if POLY_MODE is set to 1 (wireframe)
        // cullFlag = (polyMode == 1) ? false : cullFlag
        pCullFlag = m_pBuilder->CreateSelect(pWireFrameMode, m_pBuilder->getFalse(), pCullFlagPhi);

        m_pBuilder->CreateRet(pCullFlag);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
Function* NggPrimShader::CreateFrustumCuller(
    Module* pModule)    // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt1Ty(),
                                     {
                                         m_pBuilder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex2
                                         m_pBuilder->getInt32Ty(),                          // %paClClipCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                         m_pBuilder->getInt32Ty()                           // %paClGbVertDiscAdj
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingFrustum, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadNone);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pCullFlag = argIt++;
    pCullFlag->setName("cullFlag");

    Value* pVertex0 = argIt++;
    pVertex0->setName("vertex0");

    Value* pVertex1 = argIt++;
    pVertex1->setName("vertex1");

    Value* pVertex2 = argIt++;
    pVertex2->setName("vertex2");

    Value* pPaClClipCntl = argIt++;
    pPaClClipCntl->setName("paClClipCntl");

    Value* pPaClGbHorzDiscAdj = argIt++;
    pPaClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

    Value* pPaClGbVertDiscAdj = argIt++;
    pPaClGbVertDiscAdj->setName("paClGbVertDiscAdj");

    auto pFrustumEntryBlock = CreateBlock(pFunc, ".frustumEntry");
    auto pFrustumCullBlock = CreateBlock(pFunc, ".frustumCull");
    auto pFrustumExitBlock = CreateBlock(pFunc, ".frustumExit");

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".frustumEntry" block
    {
        m_pBuilder->SetInsertPoint(pFrustumEntryBlock);
        // If cull flag has already been TRUE, early return
        m_pBuilder->CreateCondBr(pCullFlag, pFrustumExitBlock, pFrustumCullBlock);
    }

    // Construct ".frustumCull" block
    Value* pNewCullFlag = nullptr;
    {
        m_pBuilder->SetInsertPoint(pFrustumCullBlock);

        //
        // Frustum culling algorithm is described as follow:
        //
        //   if ((x[i] > xDiscAdj * w[i]) && (y[i] > yDiscAdj * w[i]) && (z[i] > zFar * w[i]))
        //       cullFlag = true
        //
        //   if ((x[i] < -xDiscAdj * w[i]) && (y[i] < -yDiscAdj * w[i]) && (z[i] < zNear * w[i]))
        //       cullFlag &= true
        //
        //   i = [0..2]
        //

        // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
        Value* pClipSpaceDef = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                           m_pBuilder->getInt32Ty(),
                                                           {
                                                               pPaClClipCntl,
                                                               m_pBuilder->getInt32(19),
                                                               m_pBuilder->getInt32(1)
                                                           });
        pClipSpaceDef = m_pBuilder->CreateTrunc(pClipSpaceDef, m_pBuilder->getInt1Ty());

        // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
        auto pZNear = m_pBuilder->CreateSelect(pClipSpaceDef,
                                               ConstantFP::get(m_pBuilder->getFloatTy(), -1.0),
                                               ConstantFP::get(m_pBuilder->getFloatTy(), 0.0));

        // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
        auto pXDiscAdj = m_pBuilder->CreateBitCast(pPaClGbHorzDiscAdj, m_pBuilder->getFloatTy());

        // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
        auto pYDiscAdj = m_pBuilder->CreateBitCast(pPaClGbVertDiscAdj, m_pBuilder->getFloatTy());

        auto pX0 = m_pBuilder->CreateExtractElement(pVertex0, static_cast<uint64_t>(0));
        auto pY0 = m_pBuilder->CreateExtractElement(pVertex0, 1);
        auto pZ0 = m_pBuilder->CreateExtractElement(pVertex0, 2);
        auto pW0 = m_pBuilder->CreateExtractElement(pVertex0, 3);

        auto pX1 = m_pBuilder->CreateExtractElement(pVertex1, static_cast<uint64_t>(0));
        auto pY1 = m_pBuilder->CreateExtractElement(pVertex1, 1);
        auto pZ1 = m_pBuilder->CreateExtractElement(pVertex1, 2);
        auto pW1 = m_pBuilder->CreateExtractElement(pVertex1, 3);

        auto pX2 = m_pBuilder->CreateExtractElement(pVertex2, static_cast<uint64_t>(0));
        auto pY2 = m_pBuilder->CreateExtractElement(pVertex2, 1);
        auto pZ2 = m_pBuilder->CreateExtractElement(pVertex2, 2);
        auto pW2 = m_pBuilder->CreateExtractElement(pVertex2, 3);

        // -xDiscAdj
        auto pNegXDiscAdj = m_pBuilder->CreateFNeg(pXDiscAdj);

        // -yDiscAdj
        auto pNegYDiscAdj = m_pBuilder->CreateFNeg(pYDiscAdj);

        Value* pClipMask[6] = {};

        //
        // Get clip mask for vertex0
        //

        // (x0 < -xDiscAdj * w0) ? 0x1 : 0
        pClipMask[0] = m_pBuilder->CreateFMul(pNegXDiscAdj, pW0);
        pClipMask[0] = m_pBuilder->CreateFCmpOLT(pX0, pClipMask[0]);
        pClipMask[0] = m_pBuilder->CreateSelect(pClipMask[0], m_pBuilder->getInt32(0x1), m_pBuilder->getInt32(0));

        // (x0 > xDiscAdj * w0) ? 0x2 : 0
        pClipMask[1] = m_pBuilder->CreateFMul(pXDiscAdj, pW0);
        pClipMask[1] = m_pBuilder->CreateFCmpOGT(pX0, pClipMask[1]);
        pClipMask[1] = m_pBuilder->CreateSelect(pClipMask[1], m_pBuilder->getInt32(0x2), m_pBuilder->getInt32(0));

        // (y0 < -yDiscAdj * w0) ? 0x4 : 0
        pClipMask[2] = m_pBuilder->CreateFMul(pNegYDiscAdj, pW0);
        pClipMask[2] = m_pBuilder->CreateFCmpOLT(pY0, pClipMask[2]);
        pClipMask[2] = m_pBuilder->CreateSelect(pClipMask[2], m_pBuilder->getInt32(0x4), m_pBuilder->getInt32(0));

        // (y0 > yDiscAdj * w0) ? 0x8 : 0
        pClipMask[3] = m_pBuilder->CreateFMul(pYDiscAdj, pW0);
        pClipMask[3] = m_pBuilder->CreateFCmpOGT(pY0, pClipMask[3]);
        pClipMask[3] = m_pBuilder->CreateSelect(pClipMask[3], m_pBuilder->getInt32(0x8), m_pBuilder->getInt32(0));

        // (z0 < zNear * w0) ? 0x10 : 0
        pClipMask[4] = m_pBuilder->CreateFMul(pZNear, pW0);
        pClipMask[4] = m_pBuilder->CreateFCmpOLT(pZ0, pClipMask[4]);
        pClipMask[4] = m_pBuilder->CreateSelect(pClipMask[4], m_pBuilder->getInt32(0x10), m_pBuilder->getInt32(0));

        // (z0 > w0) ? 0x20 : 0
        pClipMask[5] = m_pBuilder->CreateFCmpOGT(pZ0, pW0);
        pClipMask[5] = m_pBuilder->CreateSelect(pClipMask[5], m_pBuilder->getInt32(0x20), m_pBuilder->getInt32(0));

        // clipMask0
        auto pClipMaskX0 = m_pBuilder->CreateOr(pClipMask[0], pClipMask[1]);
        auto pClipMaskY0 = m_pBuilder->CreateOr(pClipMask[2], pClipMask[3]);
        auto pClipMaskZ0 = m_pBuilder->CreateOr(pClipMask[4], pClipMask[5]);
        auto pClipMask0 = m_pBuilder->CreateOr(pClipMaskX0, pClipMaskY0);
        pClipMask0 = m_pBuilder->CreateOr(pClipMask0, pClipMaskZ0);

        //
        // Get clip mask for vertex1
        //

        // (x1 < -xDiscAdj * w1) ? 0x1 : 0
        pClipMask[0] = m_pBuilder->CreateFMul(pNegXDiscAdj, pW1);
        pClipMask[0] = m_pBuilder->CreateFCmpOLT(pX1, pClipMask[0]);
        pClipMask[0] = m_pBuilder->CreateSelect(pClipMask[0], m_pBuilder->getInt32(0x1), m_pBuilder->getInt32(0));

        // (x1 > xDiscAdj * w1) ? 0x2 : 0
        pClipMask[1] = m_pBuilder->CreateFMul(pXDiscAdj, pW1);
        pClipMask[1] = m_pBuilder->CreateFCmpOGT(pX1, pClipMask[1]);
        pClipMask[1] = m_pBuilder->CreateSelect(pClipMask[1], m_pBuilder->getInt32(0x2), m_pBuilder->getInt32(0));

        // (y1 < -yDiscAdj * w1) ? 0x4 : 0
        pClipMask[2] = m_pBuilder->CreateFMul(pNegYDiscAdj, pW1);
        pClipMask[2] = m_pBuilder->CreateFCmpOLT(pY1, pClipMask[2]);
        pClipMask[2] = m_pBuilder->CreateSelect(pClipMask[2], m_pBuilder->getInt32(0x4), m_pBuilder->getInt32(0));

        // (y1 > yDiscAdj * w1) ? 0x8 : 0
        pClipMask[3] = m_pBuilder->CreateFMul(pYDiscAdj, pW1);
        pClipMask[3] = m_pBuilder->CreateFCmpOGT(pY1, pClipMask[3]);
        pClipMask[3] = m_pBuilder->CreateSelect(pClipMask[3], m_pBuilder->getInt32(0x8), m_pBuilder->getInt32(0));

        // (z1 < zNear * w1) ? 0x10 : 0
        pClipMask[4] = m_pBuilder->CreateFMul(pZNear, pW1);
        pClipMask[4] = m_pBuilder->CreateFCmpOLT(pZ1, pClipMask[4]);
        pClipMask[4] = m_pBuilder->CreateSelect(pClipMask[4], m_pBuilder->getInt32(0x10), m_pBuilder->getInt32(0));

        // (z1 > w1) ? 0x20 : 0
        pClipMask[5] = m_pBuilder->CreateFCmpOGT(pZ1, pW1);
        pClipMask[5] = m_pBuilder->CreateSelect(pClipMask[5], m_pBuilder->getInt32(0x20), m_pBuilder->getInt32(0));

        // clipMask1
        auto pClipMaskX1 = m_pBuilder->CreateOr(pClipMask[0], pClipMask[1]);
        auto pClipMaskY1 = m_pBuilder->CreateOr(pClipMask[2], pClipMask[3]);
        auto pClipMaskZ1 = m_pBuilder->CreateOr(pClipMask[4], pClipMask[5]);
        auto pClipMask1 = m_pBuilder->CreateOr(pClipMaskX1, pClipMaskY1);
        pClipMask1 = m_pBuilder->CreateOr(pClipMask1, pClipMaskZ1);

        //
        // Get clip mask for vertex2
        //

        // (x2 < -xDiscAdj * w2) ? 0x1 : 0
        pClipMask[0] = m_pBuilder->CreateFMul(pNegXDiscAdj, pW2);
        pClipMask[0] = m_pBuilder->CreateFCmpOLT(pX2, pClipMask[0]);
        pClipMask[0] = m_pBuilder->CreateSelect(pClipMask[0], m_pBuilder->getInt32(0x1), m_pBuilder->getInt32(0));

        // (x2 > xDiscAdj * w2) ? 0x2 : 0
        pClipMask[1] = m_pBuilder->CreateFMul(pXDiscAdj, pW2);
        pClipMask[1] = m_pBuilder->CreateFCmpOGT(pX2, pClipMask[1]);
        pClipMask[1] = m_pBuilder->CreateSelect(pClipMask[1], m_pBuilder->getInt32(0x2), m_pBuilder->getInt32(0));

        // (y2 < -yDiscAdj * w2) ? 0x4 : 0
        pClipMask[2] = m_pBuilder->CreateFMul(pNegYDiscAdj, pW2);
        pClipMask[2] = m_pBuilder->CreateFCmpOLT(pY2, pClipMask[2]);
        pClipMask[2] = m_pBuilder->CreateSelect(pClipMask[2], m_pBuilder->getInt32(0x4), m_pBuilder->getInt32(0));

        // (y2 > yDiscAdj * w2) ? 0x8 : 0
        pClipMask[3] = m_pBuilder->CreateFMul(pYDiscAdj, pW2);
        pClipMask[3] = m_pBuilder->CreateFCmpOGT(pY2, pClipMask[3]);
        pClipMask[3] = m_pBuilder->CreateSelect(pClipMask[3], m_pBuilder->getInt32(0x8), m_pBuilder->getInt32(0));

        // (z2 < zNear * w2) ? 0x10 : 0
        pClipMask[4] = m_pBuilder->CreateFMul(pZNear, pW2);
        pClipMask[4] = m_pBuilder->CreateFCmpOLT(pZ2, pClipMask[4]);
        pClipMask[4] = m_pBuilder->CreateSelect(pClipMask[4], m_pBuilder->getInt32(0x10), m_pBuilder->getInt32(0));

        // (z2 > zFar * w2) ? 0x20 : 0
        pClipMask[5] = m_pBuilder->CreateFCmpOGT(pZ2, pW2);
        pClipMask[5] = m_pBuilder->CreateSelect(pClipMask[5], m_pBuilder->getInt32(0x20), m_pBuilder->getInt32(0));

        // clipMask2
        auto pClipMaskX2 = m_pBuilder->CreateOr(pClipMask[0], pClipMask[1]);
        auto pClipMaskY2 = m_pBuilder->CreateOr(pClipMask[2], pClipMask[3]);
        auto pClipMaskZ2 = m_pBuilder->CreateOr(pClipMask[4], pClipMask[5]);
        auto pClipMask2 = m_pBuilder->CreateOr(pClipMaskX2, pClipMaskY2);
        pClipMask2 = m_pBuilder->CreateOr(pClipMask2, pClipMaskZ2);

        // clip = clipMask0 & clipMask1 & clipMask2
        auto pClip = m_pBuilder->CreateAnd(pClipMask0, pClipMask1);
        pClip = m_pBuilder->CreateAnd(pClip, pClipMask2);

        // cullFlag = (clip != 0)
        pNewCullFlag = m_pBuilder->CreateICmpNE(pClip, m_pBuilder->getInt32(0));

        m_pBuilder->CreateBr(pFrustumExitBlock);
    }

    // Construct ".frustumExit" block
    {
        m_pBuilder->SetInsertPoint(pFrustumExitBlock);

        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag, pFrustumEntryBlock);
        pCullFlagPhi->addIncoming(pNewCullFlag, pFrustumCullBlock);

        m_pBuilder->CreateRet(pCullFlagPhi);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that does box filter culling.
Function* NggPrimShader::CreateBoxFilterCuller(
    Module* pModule)    // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt1Ty(),
                                     {
                                         m_pBuilder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex2
                                         m_pBuilder->getInt32Ty(),                          // %paClVteCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClClipCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                         m_pBuilder->getInt32Ty()                           // %paClGbVertDiscAdj
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingBoxFilter, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadNone);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pCullFlag = argIt++;
    pCullFlag->setName("cullFlag");

    Value* pVertex0 = argIt++;
    pVertex0->setName("vertex0");

    Value* pVertex1 = argIt++;
    pVertex1->setName("vertex1");

    Value* pVertex2 = argIt++;
    pVertex2->setName("vertex2");

    Value* pPaClVteCntl = argIt++;
    pPaClVteCntl->setName("paClVteCntl");

    Value* pPaClClipCntl = argIt++;
    pPaClVteCntl->setName("paClClipCntl");

    Value* pPaClGbHorzDiscAdj = argIt++;
    pPaClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

    Value* pPaClGbVertDiscAdj = argIt++;
    pPaClGbVertDiscAdj->setName("paClGbVertDiscAdj");

    auto pBoxFilterEntryBlock = CreateBlock(pFunc, ".boxfilterEntry");
    auto pBoxFilterCullBlock = CreateBlock(pFunc, ".boxfilterCull");
    auto pBoxFilterExitBlock = CreateBlock(pFunc, ".boxfilterExit");

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".boxfilterEntry" block
    {
        m_pBuilder->SetInsertPoint(pBoxFilterEntryBlock);
        // If cull flag has already been TRUE, early return
        m_pBuilder->CreateCondBr(pCullFlag, pBoxFilterExitBlock, pBoxFilterCullBlock);
    }

    // Construct ".boxfilterCull" block
    Value* pNewCullFlag = nullptr;
    {
        m_pBuilder->SetInsertPoint(pBoxFilterCullBlock);

        //
        // Box filter culling algorithm is described as follow:
        //
        //   if ((min(x0/w0, x1/w1, x2/w2) > xDiscAdj)  ||
        //       (max(x0/w0, x1/w1, x2/w2) < -xDiscAdj) ||
        //       (min(y0/w0, y1/w1, y2/w2) > yDiscAdj)  ||
        //       (max(y0/w0, y1/w1, y2/w2) < -yDiscAdj) ||
        //       (min(z0/w0, z1/w1, z2/w2) > zFar)      ||
        //       (min(z0/w0, z1/w1, z2/w2) < zNear))
        //       cullFlag = true
        //

        // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
        Value* pVtxXyFmt = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_pBuilder->getInt32Ty(),
                                                       {
                                                           pPaClVteCntl,
                                                           m_pBuilder->getInt32(8),
                                                           m_pBuilder->getInt32(1)
                                                       });
        pVtxXyFmt = m_pBuilder->CreateTrunc(pVtxXyFmt, m_pBuilder->getInt1Ty());

        // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
        Value* pVtxZFmt = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_pBuilder->getInt32Ty(),
                                                       {
                                                           pPaClVteCntl,
                                                           m_pBuilder->getInt32(9),
                                                           m_pBuilder->getInt32(1)
                                                       });
        pVtxZFmt = m_pBuilder->CreateTrunc(pVtxXyFmt, m_pBuilder->getInt1Ty());

        // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
        Value* pClipSpaceDef = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                           m_pBuilder->getInt32Ty(),
                                                           {
                                                               pPaClClipCntl,
                                                               m_pBuilder->getInt32(19),
                                                               m_pBuilder->getInt32(1)
                                                           });
        pClipSpaceDef = m_pBuilder->CreateTrunc(pClipSpaceDef, m_pBuilder->getInt1Ty());

        // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
        auto pZNear = m_pBuilder->CreateSelect(pClipSpaceDef,
                                               ConstantFP::get(m_pBuilder->getFloatTy(), -1.0),
                                               ConstantFP::get(m_pBuilder->getFloatTy(), 0.0));
        auto pZFar = ConstantFP::get(m_pBuilder->getFloatTy(), 1.0);

        // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
        auto pXDiscAdj = m_pBuilder->CreateBitCast(pPaClGbHorzDiscAdj, m_pBuilder->getFloatTy());

        // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
        auto pYDiscAdj = m_pBuilder->CreateBitCast(pPaClGbVertDiscAdj, m_pBuilder->getFloatTy());

        auto pX0 = m_pBuilder->CreateExtractElement(pVertex0, static_cast<uint64_t>(0));
        auto pY0 = m_pBuilder->CreateExtractElement(pVertex0, 1);
        auto pZ0 = m_pBuilder->CreateExtractElement(pVertex0, 2);
        auto pW0 = m_pBuilder->CreateExtractElement(pVertex0, 3);

        auto pX1 = m_pBuilder->CreateExtractElement(pVertex1, static_cast<uint64_t>(0));
        auto pY1 = m_pBuilder->CreateExtractElement(pVertex1, 1);
        auto pZ1 = m_pBuilder->CreateExtractElement(pVertex1, 2);
        auto pW1 = m_pBuilder->CreateExtractElement(pVertex1, 3);

        auto pX2 = m_pBuilder->CreateExtractElement(pVertex2, static_cast<uint64_t>(0));
        auto pY2 = m_pBuilder->CreateExtractElement(pVertex2, 1);
        auto pZ2 = m_pBuilder->CreateExtractElement(pVertex2, 2);
        auto pW2 = m_pBuilder->CreateExtractElement(pVertex2, 3);

        // Convert xyz coordinate to normalized device coordinate (NDC)
        auto pRcpW0 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW0);
        auto pRcpW1 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW1);
        auto pRcpW2 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW2);

        // VTX_XY_FMT ? 1.0 : 1 / w0
        auto pRcpW0ForXy = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW0);
        // VTX_XY_FMT ? 1.0 : 1 / w1
        auto pRcpW1ForXy = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW1);
        // VTX_XY_FMT ? 1.0 : 1 / w2
        auto pRcpW2ForXy = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW2);

        // VTX_Z_FMT ? 1.0 : 1 / w0
        auto pRcpW0ForZ = m_pBuilder->CreateSelect(pVtxZFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW0);
        // VTX_Z_FMT ? 1.0 : 1 / w1
        auto pRcpW1ForZ = m_pBuilder->CreateSelect(pVtxZFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW1);
        // VTX_Z_FMT ? 1.0 : 1 / w2
        auto pRcpW2ForZ = m_pBuilder->CreateSelect(pVtxZFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW2);

        // x0' = x0/w0
        pX0 = m_pBuilder->CreateFMul(pX0, pRcpW0ForXy);
        // y0' = y0/w0
        pY0 = m_pBuilder->CreateFMul(pY0, pRcpW0ForXy);
        // z0' = z0/w0
        pZ0 = m_pBuilder->CreateFMul(pZ0, pRcpW0ForZ);
        // x1' = x1/w1
        pX1 = m_pBuilder->CreateFMul(pX1, pRcpW1ForXy);
        // y1' = y1/w1
        pY1 = m_pBuilder->CreateFMul(pY1, pRcpW1ForXy);
        // z1' = z1/w1
        pZ1 = m_pBuilder->CreateFMul(pZ1, pRcpW1ForZ);
        // x2' = x2/w2
        pX2 = m_pBuilder->CreateFMul(pX2, pRcpW2ForXy);
        // y2' = y2/w2
        pY2 = m_pBuilder->CreateFMul(pY2, pRcpW2ForXy);
        // z2' = z2/w2
        pZ2 = m_pBuilder->CreateFMul(pZ2, pRcpW2ForZ);

        // -xDiscAdj
        auto pNegXDiscAdj = m_pBuilder->CreateFNeg(pXDiscAdj);

        // -yDiscAdj
        auto pNegYDiscAdj = m_pBuilder->CreateFNeg(pYDiscAdj);

        // minX = min(x0', x1', x2')
        auto pMinX = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pX0, pX1 });
        pMinX = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pMinX, pX2 });

        // minX > xDiscAdj
        auto pMinXGtXDiscAdj = m_pBuilder->CreateFCmpOGT(pMinX, pXDiscAdj);

        // maxX = max(x0', x1', x2')
        auto pMaxX = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pX0, pX1 });
        pMaxX = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pMaxX, pX2 });

        // maxX < -xDiscAdj
        auto pMaxXLtNegXDiscAdj = m_pBuilder->CreateFCmpOLT(pMaxX, pNegXDiscAdj);

        // minY = min(y0', y1', y2')
        auto pMinY = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pY0, pY1 });
        pMinY = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pMinY, pY2 });

        // minY > yDiscAdj
        auto pMinYGtYDiscAdj = m_pBuilder->CreateFCmpOGT(pMinY, pYDiscAdj);

        // maxY = max(y0', y1', y2')
        auto pMaxY = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pY0, pY1 });
        pMaxY = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pMaxY, pY2 });

        // maxY < -yDiscAdj
        auto pMaxYLtNegYDiscAdj = m_pBuilder->CreateFCmpOLT(pMaxY, pNegYDiscAdj);

        // minZ = min(z0', z1', z2')
        auto pMinZ = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pZ0, pZ1 });
        pMinZ = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pMinZ, pZ2 });

        // minZ > zFar (1.0)
        auto pMinZGtZFar = m_pBuilder->CreateFCmpOGT(pMinZ, pZFar);

        // maxZ = min(z0', z1', z2')
        auto pMaxZ = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pZ0, pZ1 });
        pMaxZ = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pMaxZ, pZ2 });

        // maxZ < zNear
        auto pMaxZLtZNear = m_pBuilder->CreateFCmpOLT(pMaxZ, pZNear);

        // Get cull flag
        auto pCullX = m_pBuilder->CreateOr(pMinXGtXDiscAdj, pMaxXLtNegXDiscAdj);
        auto pCullY = m_pBuilder->CreateOr(pMinYGtYDiscAdj, pMaxYLtNegYDiscAdj);
        auto pCullZ = m_pBuilder->CreateOr(pMinZGtZFar, pMaxZLtZNear);
        pNewCullFlag = m_pBuilder->CreateOr(pCullX, pCullY);
        pNewCullFlag = m_pBuilder->CreateOr(pNewCullFlag, pCullZ);

        m_pBuilder->CreateBr(pBoxFilterExitBlock);
    }

    // Construct ".boxfilterExit" block
    {
        m_pBuilder->SetInsertPoint(pBoxFilterExitBlock);

        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag, pBoxFilterEntryBlock);
        pCullFlagPhi->addIncoming(pNewCullFlag, pBoxFilterCullBlock);

        m_pBuilder->CreateRet(pCullFlagPhi);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that does sphere culling.
Function* NggPrimShader::CreateSphereCuller(
    Module* pModule)    // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt1Ty(),
                                     {
                                         m_pBuilder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex2
                                         m_pBuilder->getInt32Ty(),                          // %paClVteCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClClipCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                         m_pBuilder->getInt32Ty()                           // %paClGbVertDiscAdj
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingSphere, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadNone);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pCullFlag = argIt++;
    pCullFlag->setName("cullFlag");

    Value* pVertex0 = argIt++;
    pVertex0->setName("vertex0");

    Value* pVertex1 = argIt++;
    pVertex1->setName("vertex1");

    Value* pVertex2 = argIt++;
    pVertex2->setName("vertex2");

    Value* pPaClVteCntl = argIt++;
    pPaClVteCntl->setName("paClVteCntl");

    Value* pPaClClipCntl = argIt++;
    pPaClVteCntl->setName("paClClipCntl");

    Value* pPaClGbHorzDiscAdj = argIt++;
    pPaClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

    Value* pPaClGbVertDiscAdj = argIt++;
    pPaClGbVertDiscAdj->setName("paClGbVertDiscAdj");

    auto pSphereEntryBlock = CreateBlock(pFunc, ".sphereEntry");
    auto pSphereCullBlock = CreateBlock(pFunc, ".sphereCull");
    auto pSphereExitBlock = CreateBlock(pFunc, ".sphereExit");

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".sphereEntry" block
    {
        m_pBuilder->SetInsertPoint(pSphereEntryBlock);
        // If cull flag has already been TRUE, early return
        m_pBuilder->CreateCondBr(pCullFlag, pSphereExitBlock, pSphereCullBlock);
    }

    // Construct ".sphereCull" block
    Value* pNewCullFlag = nullptr;
    {
        m_pBuilder->SetInsertPoint(pSphereCullBlock);

        //
        // Sphere culling algorithm is somewhat complex and is described as following steps:
        //   (1) Transform discard space to -1..1 space;
        //   (2) Project from 3D coordinates to barycentric coordinates;
        //   (3) Solve linear system and find barycentric coordinates of the point closest to the origin;
        //   (4) Do clamping for the closest point if necessary;
        //   (5) Backproject from barycentric coordinates to 3D coordinates;
        //   (6) Compute the distance squared from 3D coordinates of the closest point;
        //   (7) Compare the distance with 3.0 and determine the cull flag.
        //

        // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
        Value* pVtxXyFmt = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_pBuilder->getInt32Ty(),
                                                       {
                                                           pPaClVteCntl,
                                                           m_pBuilder->getInt32(8),
                                                           m_pBuilder->getInt32(1)
                                                       });
        pVtxXyFmt = m_pBuilder->CreateTrunc(pVtxXyFmt, m_pBuilder->getInt1Ty());

        // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
        Value* pVtxZFmt = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_pBuilder->getInt32Ty(),
                                                       {
                                                           pPaClVteCntl,
                                                           m_pBuilder->getInt32(9),
                                                           m_pBuilder->getInt32(1)
                                                       });
        pVtxZFmt = m_pBuilder->CreateTrunc(pVtxXyFmt, m_pBuilder->getInt1Ty());

        // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
        Value* pClipSpaceDef = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                           m_pBuilder->getInt32Ty(),
                                                           {
                                                               pPaClClipCntl,
                                                               m_pBuilder->getInt32(19),
                                                               m_pBuilder->getInt32(1)
                                                           });
        pClipSpaceDef = m_pBuilder->CreateTrunc(pClipSpaceDef, m_pBuilder->getInt1Ty());

        // zNear = clipSpaceDef ? -1.0 : 0.0
        auto pZNear = m_pBuilder->CreateSelect(pClipSpaceDef,
                                               ConstantFP::get(m_pBuilder->getFloatTy(), -1.0),
                                               ConstantFP::get(m_pBuilder->getFloatTy(), 0.0));

        // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
        auto pXDiscAdj = m_pBuilder->CreateBitCast(pPaClGbHorzDiscAdj, m_pBuilder->getFloatTy());

        // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
        auto pYDiscAdj = m_pBuilder->CreateBitCast(pPaClGbVertDiscAdj, m_pBuilder->getFloatTy());

        auto pX0 = m_pBuilder->CreateExtractElement(pVertex0, static_cast<uint64_t>(0));
        auto pY0 = m_pBuilder->CreateExtractElement(pVertex0, 1);
        auto pZ0 = m_pBuilder->CreateExtractElement(pVertex0, 2);
        auto pW0 = m_pBuilder->CreateExtractElement(pVertex0, 3);

        auto pX1 = m_pBuilder->CreateExtractElement(pVertex1, static_cast<uint64_t>(0));
        auto pY1 = m_pBuilder->CreateExtractElement(pVertex1, 1);
        auto pZ1 = m_pBuilder->CreateExtractElement(pVertex1, 2);
        auto pW1 = m_pBuilder->CreateExtractElement(pVertex1, 3);

        auto pX2 = m_pBuilder->CreateExtractElement(pVertex2, static_cast<uint64_t>(0));
        auto pY2 = m_pBuilder->CreateExtractElement(pVertex2, 1);
        auto pZ2 = m_pBuilder->CreateExtractElement(pVertex2, 2);
        auto pW2 = m_pBuilder->CreateExtractElement(pVertex2, 3);

        // Convert xyz coordinate to normalized device coordinate (NDC)
        auto pRcpW0 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW0);
        auto pRcpW1 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW1);
        auto pRcpW2 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW2);

        // VTX_XY_FMT ? 1.0 : 1 / w0
        auto pRcpW0ForXy = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW0);
        // VTX_XY_FMT ? 1.0 : 1 / w1
        auto pRcpW1ForXy = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW1);
        // VTX_XY_FMT ? 1.0 : 1 / w2
        auto pRcpW2ForXy = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW2);

        // VTX_Z_FMT ? 1.0 : 1 / w0
        auto pRcpW0ForZ = m_pBuilder->CreateSelect(pVtxZFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW0);
        // VTX_Z_FMT ? 1.0 : 1 / w1
        auto pRcpW1ForZ = m_pBuilder->CreateSelect(pVtxZFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW1);
        // VTX_Z_FMT ? 1.0 : 1 / w2
        auto pRcpW2ForZ = m_pBuilder->CreateSelect(pVtxZFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW2);

        // x0' = x0/w0
        pX0 = m_pBuilder->CreateFMul(pX0, pRcpW0ForXy);
        // y0' = y0/w0
        pY0 = m_pBuilder->CreateFMul(pY0, pRcpW0ForXy);
        // z0' = z0/w0
        pZ0 = m_pBuilder->CreateFMul(pZ0, pRcpW0ForZ);
        // x1' = x1/w1
        pX1 = m_pBuilder->CreateFMul(pX1, pRcpW1ForXy);
        // y1' = y1/w1
        pY1 = m_pBuilder->CreateFMul(pY1, pRcpW1ForXy);
        // z1' = z1/w1
        pZ1 = m_pBuilder->CreateFMul(pZ1, pRcpW1ForZ);
        // x2' = x2/w2
        pX2 = m_pBuilder->CreateFMul(pX2, pRcpW2ForXy);
        // y2' = y2/w2
        pY2 = m_pBuilder->CreateFMul(pY2, pRcpW2ForXy);
        // z2' = z2/w2
        pZ2 = m_pBuilder->CreateFMul(pZ2, pRcpW2ForZ);

        //
        // === Step 1 ===: Discard space to -1..1 space.
        //

        // x" = x'/xDiscAdj
        // y" = y'/yDiscAdj
        // z" = (zNear + 2.0)z' + (-1.0 - zNear)
        auto pRcpXDiscAdj = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pXDiscAdj);
        auto pRcpYDiscAdj = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pYDiscAdj);
        auto pRcpXyDiscAdj =
            m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pRcpXDiscAdj, pRcpYDiscAdj });

        Value* pX0Y0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pX0, pY0 });
        Value* pX1Y1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pX1, pY1 });
        Value* pX2Y2 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pX2, pY2 });

        pX0Y0 = m_pBuilder->CreateFMul(pX0Y0, pRcpXyDiscAdj);
        pX1Y1 = m_pBuilder->CreateFMul(pX1Y1, pRcpXyDiscAdj);
        pX2Y2 = m_pBuilder->CreateFMul(pX2Y2, pRcpXyDiscAdj);

        // zNear + 2.0
        auto pZNearPlusTwo = m_pBuilder->CreateFAdd(pZNear, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));
        pZNearPlusTwo = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pZNearPlusTwo, pZNearPlusTwo });

        // -1.0 - zNear
        auto pNegOneMinusZNear = m_pBuilder->CreateFSub(ConstantFP::get(m_pBuilder->getFloatTy(), -1.0), pZNear);
        pNegOneMinusZNear =
            m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pNegOneMinusZNear, pNegOneMinusZNear });

        Value* pZ0Z0 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pZ0, pZ0 });
        Value* pZ2Z1 = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { pZ2, pZ1 });

        pZ0Z0 = m_pBuilder->CreateIntrinsic(Intrinsic::fma,
                                            VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                            { pZNearPlusTwo, pZ0Z0, pNegOneMinusZNear });
        pZ2Z1 = m_pBuilder->CreateIntrinsic(Intrinsic::fma,
                                            VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                            { pZNearPlusTwo, pZ2Z1, pNegOneMinusZNear });

        //
        // === Step 2 ===: 3D coordinates to barycentric coordinates.
        //

        // <x20, y20> = <x2", y2"> - <x0", y0">
        auto pX20Y20 = m_pBuilder->CreateFSub(pX2Y2, pX0Y0);

        // <x10, y10> = <x1", y1"> - <x0", y0">
        auto pX10Y10 = m_pBuilder->CreateFSub(pX1Y1, pX0Y0);

        // <z20, z10> = <z2", z1"> - <z0", z0">
        auto pZ20Z10 = m_pBuilder->CreateFSub(pZ2Z1, pZ0Z0);

        //
        // === Step 3 ===: Solve linear system and find the point closest to the origin.
        //

        // a00 = x10 + z10
        auto pX10 = m_pBuilder->CreateExtractElement(pX10Y10, static_cast<uint64_t>(0));
        auto pZ10 = m_pBuilder->CreateExtractElement(pZ20Z10, 1);
        auto pA00 = m_pBuilder->CreateFAdd(pX10, pZ10);

        // a01 = x20 + z20
        auto pX20 = m_pBuilder->CreateExtractElement(pX20Y20, static_cast<uint64_t>(0));
        auto pZ20 = m_pBuilder->CreateExtractElement(pZ20Z10, static_cast<uint64_t>(0));
        auto pA01 = m_pBuilder->CreateFAdd(pX20, pZ20);

        // a10 = y10 + y10
        auto pY10 = m_pBuilder->CreateExtractElement(pX10Y10, 1);
        auto pA10 = m_pBuilder->CreateFAdd(pY10, pY10);

        // a11 = y20 + z20
        auto pY20 = m_pBuilder->CreateExtractElement(pX20Y20, 1);
        auto pA11 = m_pBuilder->CreateFAdd(pY20, pZ20);

        // b0 = -x0" - x2"
        pX0 = m_pBuilder->CreateExtractElement(pX0Y0, static_cast<uint64_t>(0));
        auto pNegX0 = m_pBuilder->CreateFNeg(pX0);
        pX2 = m_pBuilder->CreateExtractElement(pX2Y2, static_cast<uint64_t>(0));
        auto pB0 = m_pBuilder->CreateFSub(pNegX0, pX2);

        // b1 = -x1" - x2"
        pX1 = m_pBuilder->CreateExtractElement(pX1Y1, static_cast<uint64_t>(0));
        auto pNegX1 = m_pBuilder->CreateFNeg(pX1);
        auto pB1 = m_pBuilder->CreateFSub(pNegX1, pX2);

        //     [ a00 a01 ]      [ b0 ]       [ s ]
        // A = [         ], B = [    ], ST = [   ], A * ST = B (crame rules)
        //     [ a10 a11 ]      [ b1 ]       [ t ]

        //           | a00 a01 |
        // det(A) =  |         | = a00 * a11 - a01 * a10
        //           | a10 a11 |
        auto pDetA = m_pBuilder->CreateFMul(pA00, pA11);
        auto pNegA01 = m_pBuilder->CreateFNeg(pA01);
        pDetA = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pNegA01, pA10, pDetA });

        //            | b0 a01 |
        // det(Ab0) = |        | = b0 * a11 - a01 * b1
        //            | b1 a11 |
        auto pDetAB0 = m_pBuilder->CreateFMul(pB0, pA11);
        pDetAB0 = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pNegA01, pB1, pDetAB0 });

        //            | a00 b0 |
        // det(Ab1) = |        | = a00 * b1 - b0 * a10
        //            | a10 b1 |
        auto pDetAB1 = m_pBuilder->CreateFMul(pA00, pB1);
        auto pNegB0 = m_pBuilder->CreateFNeg(pB0);
        pDetAB1 = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pNegB0, pA10, pDetAB1 });

        // s = det(Ab0) / det(A)
        auto pRcpDetA = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getHalfTy(), 1.0), pDetA);
        auto pS = m_pBuilder->CreateFMul(pDetAB0, pRcpDetA);

        // t = det(Ab1) / det(A)
        auto pT = m_pBuilder->CreateFMul(pDetAB1, pRcpDetA);

        //
        // === Step 4 ===: Do clamping for the closest point.
        //

        // <s, t>
        auto pST = m_pBuilder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getHalfTy(*m_pContext), 2)),
                                                   pS,
                                                   static_cast<uint64_t>(0));
        pST = m_pBuilder->CreateInsertElement(pST, pT, 1);

        // <s', t'> = <0.5 - 0.5(t - s), 0.5 + 0.5(t - s)>
        auto pTMinusS = m_pBuilder->CreateFSub(pT, pS);
        auto pST1 = m_pBuilder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getHalfTy(*m_pContext), 2)),
                                                    pTMinusS,
                                                    static_cast<uint64_t>(0));
        pST1 = m_pBuilder->CreateInsertElement(pST1, pTMinusS, 1);

        pST1 = m_pBuilder->CreateIntrinsic(Intrinsic::fma,
                                           VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                           {
                                               ConstantVector::get({ ConstantFP::get(m_pBuilder->getHalfTy(), -0.5),
                                                                     ConstantFP::get(m_pBuilder->getHalfTy(), 0.5) }),
                                               pST1,
                                               ConstantVector::get({ ConstantFP::get(m_pBuilder->getHalfTy(), 0.5),
                                                                     ConstantFP::get(m_pBuilder->getHalfTy(), 0.5) })
                                           });

        // <s", t"> = clamp(<s, t>)
        auto pST2 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                                VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                                {
                                                   pST,
                                                   ConstantVector::get({ ConstantFP::get(m_pBuilder->getHalfTy(), 0.0),
                                                                         ConstantFP::get(m_pBuilder->getHalfTy(), 0.0) })
                                               });
        pST2 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                           VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                           {
                                               pST2,
                                               ConstantVector::get({ ConstantFP::get(m_pBuilder->getHalfTy(), 1.0),
                                                                     ConstantFP::get(m_pBuilder->getHalfTy(), 1.0) })
                                           });

        // <s, t> = (s + t) > 1.0 ? <s', t'> : <s", t">
        auto pSPlusT = m_pBuilder->CreateFAdd(pS, pT);
        auto pSPlusTGtOne = m_pBuilder->CreateFCmpOGT(pSPlusT, ConstantFP::get(m_pBuilder->getHalfTy(), 1.0));
        pST = m_pBuilder->CreateSelect(pSPlusTGtOne, pST1, pST2);

        //
        // === Step 5 ===: Barycentric coordinates to 3D coordinates.
        //

        // x = x0" + s * x10 + t * x20
        // y = y0" + s * y10 + t * y20
        // z = z0" + s * z10 + t * z20
        pS = m_pBuilder->CreateExtractElement(pST, static_cast<uint64_t>(0));
        pT = m_pBuilder->CreateExtractElement(pST, 1);
        auto pSS = m_pBuilder->CreateInsertElement(pST, pS, 1);
        auto pTT = m_pBuilder->CreateInsertElement(pST, pT, static_cast<uint64_t>(0));

        // s * <x10, y10> + <x0", y0">
        auto pXY = m_pBuilder->CreateIntrinsic(Intrinsic::fma,
                                               VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                               { pSS, pX10Y10, pX0Y0 });

        // <x, y> = t * <x20, y20> + (s * <x10, y10> + <x0", y0">)
        pXY = m_pBuilder->CreateIntrinsic(Intrinsic::fma,
                                          VectorType::get(Type::getHalfTy(*m_pContext), 2),
                                          { pTT, pX20Y20, pXY });

        // s * z10 + z0"
        pZ0 = m_pBuilder->CreateExtractElement(pZ0Z0, static_cast<uint64_t>(0));
        auto pZ = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pS, pZ10, pZ0});

        // z = t * z20 + (s * z10 + z0")
        pZ = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pT, pZ20, pZ });

        auto pX = m_pBuilder->CreateExtractElement(pXY, static_cast<uint64_t>(0));
        auto pY = m_pBuilder->CreateExtractElement(pXY, 1);

        //
        // === Step 6 ===: Compute the distance squared of the closest point.
        //

        // r^2 = x^2 + y^2 + z^2
        auto pSquareR = m_pBuilder->CreateFMul(pX, pX);
        pSquareR = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pY, pY, pSquareR });
        pSquareR = m_pBuilder->CreateIntrinsic(Intrinsic::fma, m_pBuilder->getHalfTy(), { pZ, pZ, pSquareR });

        //
        // == = Step 7 == = : Determine the cull flag
        //

        // cullFlag = (r ^ 2 > 3.0)
        pNewCullFlag = m_pBuilder->CreateFCmpOGT(pSquareR, ConstantFP::get(m_pBuilder->getHalfTy(), 3.0));

        m_pBuilder->CreateBr(pSphereExitBlock);
    }

    // Construct ".sphereExit" block
    {
        m_pBuilder->SetInsertPoint(pSphereExitBlock);

        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag, pSphereEntryBlock);
        pCullFlagPhi->addIncoming(pNewCullFlag, pSphereCullBlock);

        m_pBuilder->CreateRet(pCullFlagPhi);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that does small primitive filter culling.
Function* NggPrimShader::CreateSmallPrimFilterCuller(
    Module* pModule)    // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt1Ty(),
                                     {
                                         m_pBuilder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_pContext), 4), // %vertex2
                                         m_pBuilder->getInt32Ty(),                          // %paClVteCntl
                                         m_pBuilder->getInt32Ty(),                          // %paClVportXscale
                                         m_pBuilder->getInt32Ty()                           // %paClVportYscale
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingSmallPrimFilter, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadNone);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pCullFlag = argIt++;
    pCullFlag->setName("cullFlag");

    Value* pVertex0 = argIt++;
    pVertex0->setName("vertex0");

    Value* pVertex1 = argIt++;
    pVertex1->setName("vertex1");

    Value* pVertex2 = argIt++;
    pVertex2->setName("vertex2");

    Value* pPaClVteCntl = argIt++;
    pPaClVteCntl->setName("paClVteCntl");

    Value* pPaClVportXscale = argIt++;
    pPaClVportXscale->setName("paClVportXscale");

    Value* pPaClVportYscale = argIt++;
    pPaClVportYscale->setName("paClVportYscale");

    auto pSmallPrimFilterEntryBlock = CreateBlock(pFunc, ".smallprimfilterEntry");
    auto pSmallPrimFilterCullBlock = CreateBlock(pFunc, ".smallprimfilterCull");
    auto pSmallPrimFilterExitBlock = CreateBlock(pFunc, ".smallprimfilterExit");

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".smallprimfilterEntry" block
    {
        m_pBuilder->SetInsertPoint(pSmallPrimFilterEntryBlock);
        // If cull flag has already been TRUE, early return
        m_pBuilder->CreateCondBr(pCullFlag, pSmallPrimFilterExitBlock, pSmallPrimFilterCullBlock);
    }

    // Construct ".smallprimfilterCull" block
    Value* pNewCullFlag = nullptr;
    {
        m_pBuilder->SetInsertPoint(pSmallPrimFilterCullBlock);

        //
        // Small primitive filter culling algorithm is described as follow:
        //
        //   if ((roundEven(min(scaled(x0/w0), scaled(x1/w1), scaled(x2/w2))) ==
        //        roundEven(max(scaled(x0/w0), scaled(x1/w1), scaled(x2/w2)))) ||
        //       (roundEven(min(scaled(y0/w0), scaled(y1/w1), scaled(y2/w2))) ==
        //        roundEven(max(scaled(y0/w0), scaled(y1/w1), scaled(y2/w2)))))
        //       cullFlag = true
        //

        // vtxXyFmt = (VTX_XY_FMT, PA_CL_VTE_CNTL[8], 0 = 1/W0, 1 = none)
        Value* pVtxXyFmt = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_pBuilder->getInt32Ty(),
                                                       {
                                                           pPaClVteCntl,
                                                           m_pBuilder->getInt32(8),
                                                           m_pBuilder->getInt32(1)
                                                       });
        pVtxXyFmt = m_pBuilder->CreateTrunc(pVtxXyFmt, m_pBuilder->getInt1Ty());

        // xScale = (VPORT_XSCALE, PA_CL_VPORT_XSCALE[31:0])
        auto pXSCale = m_pBuilder->CreateBitCast(pPaClVportXscale, m_pBuilder->getFloatTy());

        // yScale = (VPORT_YSCALE, PA_CL_VPORT_YSCALE[31:0])
        auto pYSCale = m_pBuilder->CreateBitCast(pPaClVportYscale, m_pBuilder->getFloatTy());

        auto pX0 = m_pBuilder->CreateExtractElement(pVertex0, static_cast<uint64_t>(0));
        auto pY0 = m_pBuilder->CreateExtractElement(pVertex0, 1);
        auto pW0 = m_pBuilder->CreateExtractElement(pVertex0, 3);

        auto pX1 = m_pBuilder->CreateExtractElement(pVertex1, static_cast<uint64_t>(0));
        auto pY1 = m_pBuilder->CreateExtractElement(pVertex1, 1);
        auto pW1 = m_pBuilder->CreateExtractElement(pVertex1, 3);

        auto pX2 = m_pBuilder->CreateExtractElement(pVertex2, static_cast<uint64_t>(0));
        auto pY2 = m_pBuilder->CreateExtractElement(pVertex2, 1);
        auto pW2 = m_pBuilder->CreateExtractElement(pVertex2, 3);

        // Convert xyz coordinate to normalized device coordinate (NDC)
        auto pRcpW0 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW0);
        auto pRcpW1 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW1);
        auto pRcpW2 = m_pBuilder->CreateFDiv(ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pW2);

        // VTX_XY_FMT ? 1.0 : 1 / w0
        pRcpW0 = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW0);
        // VTX_XY_FMT ? 1.0 : 1 / w1
        pRcpW1 = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW1);
        // VTX_XY_FMT ? 1.0 : 1 / w2
        pRcpW2 = m_pBuilder->CreateSelect(pVtxXyFmt, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0), pRcpW2);

        // x0' = x0/w0
        pX0 = m_pBuilder->CreateFMul(pX0, pRcpW0);
        // y0' = y0/w0
        pY0 = m_pBuilder->CreateFMul(pY0, pRcpW0);
        // x1' = x1/w1
        pX1 = m_pBuilder->CreateFMul(pX1, pRcpW1);
        // y1' = y1/w1
        pY1 = m_pBuilder->CreateFMul(pY1, pRcpW1);
        // x2' = x2/w2
        pX2 = m_pBuilder->CreateFMul(pX2, pRcpW2);
        // y2' = y2/w2
        pY2 = m_pBuilder->CreateFMul(pY2, pRcpW2);

        // clampX0' = clamp((x0' + 1.0) / 2)
        auto pClampX0 = m_pBuilder->CreateFAdd(pX0, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0));
        pClampX0 = m_pBuilder->CreateFMul(pClampX0, ConstantFP::get(m_pBuilder->getFloatTy(), 0.5));
        pClampX0 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampX0,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 0.0)
                                               });
        pClampX0 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampX0,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 1.0)
                                               });

        // scaledX0' = (clampX0' * xScale) * 2
        auto pScaledX0 = m_pBuilder->CreateFMul(pClampX0, pXSCale);
        pScaledX0 = m_pBuilder->CreateFMul(pScaledX0, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));

        // clampX1' = clamp((x1' + 1.0) / 2)
        auto pClampX1 = m_pBuilder->CreateFAdd(pX1, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0));
        pClampX1 = m_pBuilder->CreateFMul(pClampX1, ConstantFP::get(m_pBuilder->getFloatTy(), 0.5));
        pClampX1 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampX1,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 0.0)
                                               });
        pClampX1 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampX1,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 1.0)
                                               });

        // scaledX1' = (clampX1' * xScale) * 2
        auto pScaledX1 = m_pBuilder->CreateFMul(pClampX1, pXSCale);
        pScaledX1 = m_pBuilder->CreateFMul(pScaledX1, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));

        // clampX2' = clamp((x2' + 1.0) / 2)
        auto pClampX2 = m_pBuilder->CreateFAdd(pX2, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0));
        pClampX2 = m_pBuilder->CreateFMul(pClampX2, ConstantFP::get(m_pBuilder->getFloatTy(), 0.5));
        pClampX2 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampX2,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 0.0)
                                               });
        pClampX2 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampX2,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 1.0)
                                               });

        // scaledX2' = (clampX2' * xScale) * 2
        auto pScaledX2 = m_pBuilder->CreateFMul(pClampX2, pXSCale);
        pScaledX2 = m_pBuilder->CreateFMul(pScaledX2, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));

        // clampY0' = clamp((y0' + 1.0) / 2)
        auto pClampY0 = m_pBuilder->CreateFAdd(pY0, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0));
        pClampY0 = m_pBuilder->CreateFMul(pClampY0, ConstantFP::get(m_pBuilder->getFloatTy(), 0.5));
        pClampY0 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampY0,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 0.0)
                                               });
        pClampY0 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampY0,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 1.0)
                                               });

        // scaledY0' = (clampY0' * yScale) * 2
        auto pScaledY0 = m_pBuilder->CreateFMul(pClampY0, pYSCale);
        pScaledY0 = m_pBuilder->CreateFMul(pScaledY0, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));

        // clampY1' = clamp((y1' + 1.0) / 2)
        auto pClampY1 = m_pBuilder->CreateFAdd(pY1, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0));
        pClampY1 = m_pBuilder->CreateFMul(pClampY1, ConstantFP::get(m_pBuilder->getFloatTy(), 0.5));
        pClampY1 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampY1,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 0.0)
                                               });
        pClampY1 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampY1,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 1.0)
                                               });

        // scaledY1' = (clampY1' * yScale) * 2
        auto pScaledY1 = m_pBuilder->CreateFMul(pClampY1, pYSCale);
        pScaledY1 = m_pBuilder->CreateFMul(pScaledY1, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));

        // clampY2' = clamp((y2' + 1.0) / 2)
        auto pClampY2 = m_pBuilder->CreateFAdd(pY2, ConstantFP::get(m_pBuilder->getFloatTy(), 1.0));
        pClampY2 = m_pBuilder->CreateFMul(pClampY2, ConstantFP::get(m_pBuilder->getFloatTy(), 0.5));
        pClampY2 = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampY2,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 0.0)
                                               });
        pClampY2 = m_pBuilder->CreateIntrinsic(Intrinsic::minnum,
                                               m_pBuilder->getFloatTy(),
                                               {
                                                   pClampY2,
                                                   ConstantFP::get(m_pBuilder->getFloatTy(), 1.0)
                                               });

        // scaledY2' = (clampY2' * yScale) * 2
        auto pScaledY2 = m_pBuilder->CreateFMul(pClampY2, pYSCale);
        pScaledY2 = m_pBuilder->CreateFMul(pScaledY2, ConstantFP::get(m_pBuilder->getFloatTy(), 2.0));

        // minX = roundEven(min(scaledX0', scaledX1', scaledX2') - 1/256.0)
        Value* pMinX =
            m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pScaledX0, pScaledX1 });
        pMinX = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pMinX, pScaledX2 });
        pMinX = m_pBuilder->CreateFSub(pMinX, ConstantFP::get(m_pBuilder->getFloatTy(), 1/256.0));
        pMinX = m_pBuilder->CreateIntrinsic(Intrinsic::rint, m_pBuilder->getFloatTy(), pMinX);

        // maxX = roundEven(max(scaledX0', scaledX1', scaledX2') + 1/256.0)
        Value* pMaxX =
            m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pScaledX0, pScaledX1 });
        pMaxX = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pMaxX, pScaledX2 });
        pMaxX = m_pBuilder->CreateFAdd(pMaxX, ConstantFP::get(m_pBuilder->getFloatTy(), 1 / 256.0));
        pMaxX = m_pBuilder->CreateIntrinsic(Intrinsic::rint, m_pBuilder->getFloatTy(), pMaxX);

        // minY = roundEven(min(scaledY0', scaledY1', scaledY2') - 1/256.0)
        Value* pMinY =
            m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pScaledY0, pScaledY1 });
        pMinY = m_pBuilder->CreateIntrinsic(Intrinsic::minnum, m_pBuilder->getFloatTy(), { pMinY, pScaledY2 });
        pMinY = m_pBuilder->CreateFSub(pMinY, ConstantFP::get(m_pBuilder->getFloatTy(), 1 / 256.0));
        pMinY = m_pBuilder->CreateIntrinsic(Intrinsic::rint, m_pBuilder->getFloatTy(), pMinY);

        // maxX = roundEven(max(scaledX0', scaledX1', scaledX2') + 1/256.0)
        Value* pMaxY =
            m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pScaledY0, pScaledY1 });
        pMaxY = m_pBuilder->CreateIntrinsic(Intrinsic::maxnum, m_pBuilder->getFloatTy(), { pMaxY, pScaledY2 });
        pMaxY = m_pBuilder->CreateFAdd(pMaxY, ConstantFP::get(m_pBuilder->getFloatTy(), 1 / 256.0));
        pMaxY = m_pBuilder->CreateIntrinsic(Intrinsic::rint, m_pBuilder->getFloatTy(), pMaxY);

        // minX == maxX
        auto pMinXEqMaxX = m_pBuilder->CreateFCmpOEQ(pMinX, pMaxX);

        // minY == maxY
        auto pMinYEqMaxY = m_pBuilder->CreateFCmpOEQ(pMinY, pMaxY);

        // Get cull flag
        pNewCullFlag = m_pBuilder->CreateOr(pMinXEqMaxX, pMinYEqMaxY);

        m_pBuilder->CreateBr(pSmallPrimFilterExitBlock);
    }

    // Construct ".smallprimfilterExit" block
    {
        m_pBuilder->SetInsertPoint(pSmallPrimFilterExitBlock);

        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag, pSmallPrimFilterEntryBlock);
        pCullFlagPhi->addIncoming(pNewCullFlag, pSmallPrimFilterCullBlock);

        m_pBuilder->CreateRet(pCullFlagPhi);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
Function* NggPrimShader::CreateCullDistanceCuller(
    Module* pModule)    // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt1Ty(),
                                     {
                                         m_pBuilder->getInt1Ty(),    // %cullFlag
                                         m_pBuilder->getInt32Ty(),   // %signMask0
                                         m_pBuilder->getInt32Ty(),   // %signMask1
                                         m_pBuilder->getInt32Ty()    // %signMask2
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingCullDistance, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadNone);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pCullFlag = argIt++;
    pCullFlag->setName("cullFlag");

    Value* pSignMask0 = argIt++;
    pSignMask0->setName("signMask0");

    Value* pSignMask1 = argIt++;
    pSignMask1->setName("signMask1");

    Value* pSignMask2 = argIt++;
    pSignMask2->setName("signMask2");

    auto pCullDistanceEntryBlock = CreateBlock(pFunc, ".culldistanceEntry");
    auto pCullDistanceCullBlock = CreateBlock(pFunc, ".culldistanceCull");
    auto pCullDistanceExitBlock = CreateBlock(pFunc, ".culldistanceExit");

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct ".culldistanceEntry" block
    {
        m_pBuilder->SetInsertPoint(pCullDistanceEntryBlock);
        // If cull flag has already been TRUE, early return
        m_pBuilder->CreateCondBr(pCullFlag, pCullDistanceExitBlock, pCullDistanceCullBlock);
    }

    // Construct ".culldistanceCull" block
    Value* pCullFlag1 = nullptr;
    {
        m_pBuilder->SetInsertPoint(pCullDistanceCullBlock);

        //
        // Cull distance culling algorithm is described as follow:
        //
        //   vertexSignMask[7:0] = [sign(ClipDistance[0])..sign(ClipDistance[7])]
        //   primSignMask = vertexSignMask0 & vertexSignMask1 & vertexSignMask2
        //   cullFlag = (primSignMask != 0)
        //
        auto pSignMask = m_pBuilder->CreateAnd(pSignMask0, pSignMask1);
        pSignMask = m_pBuilder->CreateAnd(pSignMask, pSignMask2);

        pCullFlag1 = m_pBuilder->CreateICmpNE(pSignMask, m_pBuilder->getInt32(0));

        m_pBuilder->CreateBr(pCullDistanceExitBlock);
    }

    // Construct ".culldistanceExit" block
    {
        m_pBuilder->SetInsertPoint(pCullDistanceExitBlock);

        auto pCullFlagPhi = m_pBuilder->CreatePHI(m_pBuilder->getInt1Ty(), 2);
        pCullFlagPhi->addIncoming(pCullFlag, pCullDistanceEntryBlock);
        pCullFlagPhi->addIncoming(pCullFlag1, pCullDistanceCullBlock);

        m_pBuilder->CreateRet(pCullFlagPhi);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Creates the function that fetches culling control registers.
Function* NggPrimShader::CreateFetchCullingRegister(
    Module* pModule) // [in] LLVM module
{
    auto pFuncTy = FunctionType::get(m_pBuilder->getInt32Ty(),
                                     {
                                         m_pBuilder->getInt32Ty(),  // %primShaderTableAddrLow
                                         m_pBuilder->getInt32Ty(),  // %primShaderTableAddrHigh
                                         m_pBuilder->getInt32Ty()   // %regOffset
                                     },
                                     false);
    auto pFunc = Function::Create(pFuncTy, GlobalValue::InternalLinkage, lgcName::NggCullingFetchReg, pModule);

    pFunc->setCallingConv(CallingConv::C);
    pFunc->addFnAttr(Attribute::ReadOnly);
    pFunc->addFnAttr(Attribute::AlwaysInline);

    auto argIt = pFunc->arg_begin();
    Value* pPrimShaderTableAddrLow = argIt++;
    pPrimShaderTableAddrLow->setName("primShaderTableAddrLow");

    Value* pPrimShaderTableAddrHigh = argIt++;
    pPrimShaderTableAddrHigh->setName("primShaderTableAddrHigh");

    Value* pRegOffset = argIt++;
    pRegOffset->setName("regOffset");

    BasicBlock* pEntryBlock = CreateBlock(pFunc); // Create entry block

    auto savedInsertPoint = m_pBuilder->saveIP();

    // Construct entry block
    {
        m_pBuilder->SetInsertPoint(pEntryBlock);

        Value* pPrimShaderTableAddr = m_pBuilder->CreateInsertElement(
                                                    UndefValue::get(VectorType::get(Type::getInt32Ty(*m_pContext), 2)),
                                                    pPrimShaderTableAddrLow,
                                                    static_cast<uint64_t>(0));

        pPrimShaderTableAddr = m_pBuilder->CreateInsertElement(pPrimShaderTableAddr, pPrimShaderTableAddrHigh, 1);

        pPrimShaderTableAddr = m_pBuilder->CreateBitCast(pPrimShaderTableAddr, m_pBuilder->getInt64Ty());

        auto pPrimShaderTablePtrTy = PointerType::get(ArrayType::get(m_pBuilder->getInt32Ty(), 256),
                                                      ADDR_SPACE_CONST); // [256 x i32]
        auto pPrimShaderTablePtr = m_pBuilder->CreateIntToPtr(pPrimShaderTableAddr, pPrimShaderTablePtrTy);

        // regOffset = regOffset >> 2
        pRegOffset = m_pBuilder->CreateLShr(pRegOffset, 2); // To DWORD offset

        auto pLoadPtr = m_pBuilder->CreateGEP(pPrimShaderTablePtr, { m_pBuilder->getInt32(0), pRegOffset });
        cast<Instruction>(pLoadPtr)->setMetadata(MetaNameUniform, MDNode::get(m_pBuilder->getContext(), {}));

        auto pRegValue = m_pBuilder->CreateAlignedLoad(pLoadPtr, MaybeAlign(4));
        pRegValue->setVolatile(true);
        pRegValue->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_pBuilder->getContext(), {}));

        m_pBuilder->CreateRet(pRegValue);
    }

    m_pBuilder->restoreIP(savedInsertPoint);

    return pFunc;
}

// =====================================================================================================================
// Output a subgroup ballot (always return i64 mask)
Value* NggPrimShader::DoSubgroupBallot(
    Value* pValue) // [in] The value to do the ballot on.
{
    assert(pValue->getType()->isIntegerTy(1)); // Should be i1

    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

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

// =====================================================================================================================
// Output a subgroup inclusive-add (IAdd).
Value* NggPrimShader::DoSubgroupInclusiveAdd(
    Value*   pValue,        // [in] The value to do the inclusive-add on
    Value**  ppWwmResult)   // [out] Result in WWM section (optinal)
{
    assert(pValue->getType()->isIntegerTy(32)); // Should be i32

    const uint32_t waveSize = m_pPipelineState->GetShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    auto pInlineAsmTy = FunctionType::get(m_pBuilder->getInt32Ty(), m_pBuilder->getInt32Ty(), false);
    auto pInlineAsm = InlineAsm::get(pInlineAsmTy, "; %1", "=v,0", true);
    pValue = m_pBuilder->CreateCall(pInlineAsm, pValue);

    // Start the WWM section by setting the inactive lanes
    auto pIdentity = m_pBuilder->getInt32(0); // Identity for IAdd (0)
    pValue =
        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_set_inactive, m_pBuilder->getInt32Ty(), { pValue, pIdentity });

    // Do DPP operations
    enum
    {
        DppRowSr1 = 0x111,
        DppRowSr2 = 0x112,
        DppRowSr3 = 0x113,
        DppRowSr4 = 0x114,
        DppRowSr8 = 0x118,
    };

    Value* pDppUpdate = DoDppUpdate(pIdentity, pValue, DppRowSr1, 0xF, 0xF);
    Value* pResult = m_pBuilder->CreateAdd(pValue, pDppUpdate);

    pDppUpdate = DoDppUpdate(pIdentity, pValue, DppRowSr2, 0xF, 0xF);
    pResult = m_pBuilder->CreateAdd(pResult, pDppUpdate);

    pDppUpdate = DoDppUpdate(pIdentity, pValue, DppRowSr3, 0xF, 0xF);
    pResult = m_pBuilder->CreateAdd(pResult, pDppUpdate);

    pDppUpdate = DoDppUpdate(pIdentity, pResult, DppRowSr4, 0xF, 0xE);
    pResult = m_pBuilder->CreateAdd(pResult, pDppUpdate);

    pDppUpdate = DoDppUpdate(pIdentity, pResult, DppRowSr8, 0xF, 0xC);
    pResult = m_pBuilder->CreateAdd(pResult, pDppUpdate);

    // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2)
    Value* pPermLane = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_permlanex16,
                                                   {},
                                                   {
                                                       pResult,
                                                       pResult,
                                                       m_pBuilder->getInt32(-1),
                                                       m_pBuilder->getInt32(-1),
                                                       m_pBuilder->getTrue(),
                                                       m_pBuilder->getFalse()
                                                   });

    Value* pThreadId = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                   {},
                                                   {
                                                       m_pBuilder->getInt32(-1),
                                                       m_pBuilder->getInt32(0)
                                                   });

    if (waveSize == 64)
    {
        pThreadId = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                {},
                                                {
                                                    m_pBuilder->getInt32(-1),
                                                    pThreadId
                                                });
        pThreadId = m_pBuilder->CreateZExt(pThreadId, m_pBuilder->getInt64Ty());
    }
    auto pThreadMask = m_pBuilder->CreateShl(m_pBuilder->getIntN(waveSize, 1), pThreadId);

    auto pZero = m_pBuilder->getIntN(waveSize, 0);
    auto pAndMask = m_pBuilder->getIntN(waveSize, 0xFFFF0000FFFF0000);
    auto pAndThreadMask = m_pBuilder->CreateAnd(pThreadMask, pAndMask);
    auto pMaskedPermLane =
        m_pBuilder->CreateSelect(m_pBuilder->CreateICmpNE(pAndThreadMask, pZero), pPermLane, pIdentity);

    pResult = m_pBuilder->CreateAdd(pResult, pMaskedPermLane);

    Value* pBroadcast31 =
        m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, { pResult, m_pBuilder->getInt32(31)});

    pAndMask = m_pBuilder->getIntN(waveSize, 0xFFFFFFFF00000000);
    pAndThreadMask = m_pBuilder->CreateAnd(pThreadMask, pAndMask);
    Value* pMaskedBroadcast =
        m_pBuilder->CreateSelect(m_pBuilder->CreateICmpNE(pAndThreadMask, pZero), pBroadcast31, pIdentity);

    // Combine broadcast of 31 with the top two rows only.
    if (waveSize == 64)
    {
        pResult = m_pBuilder->CreateAdd(pResult, pMaskedBroadcast);
    }

    if (ppWwmResult != nullptr)
    {
        // Return the result in WWM section (optional)
        *ppWwmResult = pResult;
    }

    // Finish the WWM section
    return m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_wwm, m_pBuilder->getInt32Ty(), pResult);
}

// =====================================================================================================================
// Does DPP update with specified parameters.
Value* NggPrimShader::DoDppUpdate(
    Value*      pOldValue,  // [in] Old value
    Value*      pSrcValue,  // [in] Source value to update with
    uint32_t    dppCtrl,    // DPP controls
    uint32_t    rowMask,    // Row mask
    uint32_t    bankMask,   // Bank mask
    bool        boundCtrl)  // Whether to do bound control
{
    return m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_update_dpp,
                                       m_pBuilder->getInt32Ty(),
                                       {
                                           pOldValue,
                                           pSrcValue,
                                           m_pBuilder->getInt32(dppCtrl),
                                           m_pBuilder->getInt32(rowMask),
                                           m_pBuilder->getInt32(bankMask),
                                           m_pBuilder->getInt1(boundCtrl)
                                       });
}

// =====================================================================================================================
// Creates a new basic block. Always insert it at the end of the parent function.
BasicBlock* NggPrimShader::CreateBlock(
    Function*    pParent,   // [in] Parent function to which the new block belongs
    const Twine& blockName) // [in] Name of the new block
{
    return BasicBlock::Create(*m_pContext, blockName, pParent);
}

} // lgc
