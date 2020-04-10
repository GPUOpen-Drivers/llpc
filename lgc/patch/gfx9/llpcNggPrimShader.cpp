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
#include "lgc/llpcPassManager.h"
#include "llpcShaderMerger.h"

#define DEBUG_TYPE "llpc-ngg-prim-shader"

using namespace llvm;

namespace lgc
{

// =====================================================================================================================
NggPrimShader::NggPrimShader(
    PipelineState*  pipelineState) // [in] Pipeline state
    :
    m_pipelineState(pipelineState),
    m_context(&pipelineState->getContext()),
    m_gfxIp(pipelineState->getTargetInfo().getGfxIpVersion()),
    m_nggControl(m_pipelineState->getNggControl()),
    m_ldsManager(nullptr),
    m_builder(new IRBuilder<>(*m_context))
{
    assert(m_pipelineState->isGraphics());

    memset(&m_nggFactor, 0, sizeof(m_nggFactor));

    m_hasVs = m_pipelineState->hasShaderStage(ShaderStageVertex);
    m_hasTcs = m_pipelineState->hasShaderStage(ShaderStageTessControl);
    m_hasTes = m_pipelineState->hasShaderStage(ShaderStageTessEval);
    m_hasGs = m_pipelineState->hasShaderStage(ShaderStageGeometry);
}

// =====================================================================================================================
NggPrimShader::~NggPrimShader()
{
    if (m_ldsManager != nullptr)
        delete m_ldsManager;
}

// =====================================================================================================================
// Generates NGG primitive shader entry-point.
Function* NggPrimShader::generate(
    Function*  esEntryPoint,           // [in] Entry-point of hardware export shader (ES) (could be null)
    Function*  gsEntryPoint,           // [in] Entry-point of hardware geometry shader (GS) (could be null)
    Function*  copyShaderEntryPoint)   // [in] Entry-point of hardware vertex shader (VS, copy shader) (could be null)
{
    assert(m_gfxIp.major >= 10);

    // ES and GS could not be null at the same time
    assert(((esEntryPoint == nullptr) && (gsEntryPoint == nullptr)) == false);

    Module* module = nullptr;
    if (esEntryPoint != nullptr)
    {
        module = esEntryPoint->getParent();
        esEntryPoint->setName(lgcName::NggEsEntryPoint);
        esEntryPoint->setCallingConv(CallingConv::C);
        esEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        esEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    if (gsEntryPoint != nullptr)
    {
        module = gsEntryPoint->getParent();
        gsEntryPoint->setName(lgcName::NggGsEntryPoint);
        gsEntryPoint->setCallingConv(CallingConv::C);
        gsEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        gsEntryPoint->addFnAttr(Attribute::AlwaysInline);

        assert(copyShaderEntryPoint != nullptr); // Copy shader must be present
        copyShaderEntryPoint->setName(lgcName::NggCopyShaderEntryPoint);
        copyShaderEntryPoint->setCallingConv(CallingConv::C);
        copyShaderEntryPoint->setLinkage(GlobalValue::InternalLinkage);
        copyShaderEntryPoint->addFnAttr(Attribute::AlwaysInline);
    }

    // Create NGG LDS manager
    assert(module != nullptr);
    assert(m_ldsManager == nullptr);
    m_ldsManager = new NggLdsManager(module, m_pipelineState, m_builder.get());

    return generatePrimShaderEntryPoint(module);
}

// =====================================================================================================================
// Generates the type for the new entry-point of NGG primitive shader.
FunctionType* NggPrimShader::generatePrimShaderEntryPointType(
    uint64_t* inRegMask // [out] "Inreg" bit mask for the arguments
    ) const
{
    std::vector<Type*> argTys;

    // First 8 system values (SGPRs)
    for (unsigned i = 0; i < EsGsSpecialSysValueCount; ++i)
    {
        argTys.push_back(m_builder->getInt32Ty());
        *inRegMask |= (1ull << i);
    }

    // User data (SGPRs)
    unsigned userDataCount = 0;

    const auto gsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
    const auto tesIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageTessEval);
    const auto vsIntfData = m_pipelineState->getShaderInterfaceData(ShaderStageVertex);

    bool hasTs = (m_hasTcs || m_hasTes);
    if (m_hasGs)
    {
        // GS is present in primitive shader (ES-GS merged shader)
        userDataCount = gsIntfData->userDataCount;

        if (hasTs)
        {
            if (m_hasTes)
            {
                userDataCount = std::max(tesIntfData->userDataCount, userDataCount);

                assert(tesIntfData->userDataUsage.tes.viewIndex == gsIntfData->userDataUsage.gs.viewIndex);
                if ((gsIntfData->spillTable.sizeInDwords > 0) &&
                    (tesIntfData->spillTable.sizeInDwords == 0))
                {
                    tesIntfData->userDataUsage.spillTable = userDataCount;
                    ++userDataCount;
                    assert(userDataCount <= m_pipelineState->getTargetInfo().getGpuProperty().maxUserDataCount);
                }
            }
        }
        else
        {
            if (m_hasVs)
            {
                userDataCount = std::max(vsIntfData->userDataCount, userDataCount);

                assert(vsIntfData->userDataUsage.vs.viewIndex == gsIntfData->userDataUsage.gs.viewIndex);
                if ((gsIntfData->spillTable.sizeInDwords > 0) &&
                    (vsIntfData->spillTable.sizeInDwords == 0))
                {
                    vsIntfData->userDataUsage.spillTable = userDataCount;
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
                userDataCount = tesIntfData->userDataCount;
        }
        else
        {
            if (m_hasVs)
                userDataCount = vsIntfData->userDataCount;
        }
    }

    assert(userDataCount > 0);
    argTys.push_back(VectorType::get(m_builder->getInt32Ty(), userDataCount));
    *inRegMask |= (1ull << EsGsSpecialSysValueCount);

    // Other system values (VGPRs)
    argTys.push_back(m_builder->getInt32Ty());         // ES to GS offsets (vertex 0 and 1)
    argTys.push_back(m_builder->getInt32Ty());         // ES to GS offsets (vertex 2 and 3)
    argTys.push_back(m_builder->getInt32Ty());         // Primitive ID (GS)
    argTys.push_back(m_builder->getInt32Ty());         // Invocation ID
    argTys.push_back(m_builder->getInt32Ty());         // ES to GS offsets (vertex 4 and 5)

    if (hasTs)
    {
        argTys.push_back(m_builder->getFloatTy());    // X of TessCoord (U)
        argTys.push_back(m_builder->getFloatTy());    // Y of TessCoord (V)
        argTys.push_back(m_builder->getInt32Ty());    // Relative patch ID
        argTys.push_back(m_builder->getInt32Ty());    // Patch ID
    }
    else
    {
        argTys.push_back(m_builder->getInt32Ty());    // Vertex ID
        argTys.push_back(m_builder->getInt32Ty());    // Relative vertex ID (auto index)
        argTys.push_back(m_builder->getInt32Ty());    // Primitive ID (VS)
        argTys.push_back(m_builder->getInt32Ty());    // Instance ID
    }

    return FunctionType::get(m_builder->getVoidTy(), argTys, false);
}

// =====================================================================================================================
// Generates the new entry-point for NGG primitive shader.
Function* NggPrimShader::generatePrimShaderEntryPoint(
    Module* module)  // [in] LLVM module
{
    uint64_t inRegMask = 0;
    auto entryPointTy = generatePrimShaderEntryPointType(&inRegMask);

    Function* entryPoint = Function::Create(entryPointTy,
                                             GlobalValue::ExternalLinkage,
                                             lgcName::NggPrimShaderEntryPoint);

    module->getFunctionList().push_front(entryPoint);

    entryPoint->addFnAttr("amdgpu-flat-work-group-size", "128,128"); // Force s_barrier to be present (ignore optimization)

    for (auto& arg : entryPoint->args())
    {
        auto argIdx = arg.getArgNo();
        if (inRegMask & (1ull << argIdx))
            arg.addAttr(Attribute::InReg);
    }

    auto arg = entryPoint->arg_begin();

    Value* userDataAddrLow         = (arg + EsGsSysValueUserDataAddrLow);
    Value* userDataAddrHigh        = (arg + EsGsSysValueUserDataAddrHigh);
    Value* mergedGroupInfo         = (arg + EsGsSysValueMergedGroupInfo);
    Value* mergedWaveInfo          = (arg + EsGsSysValueMergedWaveInfo);
    Value* offChipLdsBase          = (arg + EsGsSysValueOffChipLdsBase);
    Value* sharedScratchOffset     = (arg + EsGsSysValueSharedScratchOffset);
    Value* primShaderTableAddrLow  = (arg + EsGsSysValuePrimShaderTableAddrLow);
    Value* primShaderTableAddrHigh = (arg + EsGsSysValuePrimShaderTableAddrHigh);

    arg += EsGsSpecialSysValueCount;

    Value* userData = arg++;

    Value* esGsOffsets01 = arg;
    Value* esGsOffsets23 = (arg + 1);
    Value* gsPrimitiveId = (arg + 2);
    Value* invocationId  = (arg + 3);
    Value* esGsOffsets45 = (arg + 4);

    Value* tessCoordX    = (arg + 5);
    Value* tessCoordY    = (arg + 6);
    Value* relPatchId    = (arg + 7);
    Value* patchId       = (arg + 8);

    Value* vertexId      = (arg + 5);
    Value* relVertexId   = (arg + 6);
    Value* vsPrimitiveId = (arg + 7);
    Value* instanceId    = (arg + 8);

    userDataAddrLow->setName("userDataAddrLow");
    userDataAddrHigh->setName("userDataAddrHigh");
    mergedGroupInfo->setName("mergedGroupInfo");
    mergedWaveInfo->setName("mergedWaveInfo");
    offChipLdsBase->setName("offChipLdsBase");
    sharedScratchOffset->setName("sharedScratchOffset");
    primShaderTableAddrLow->setName("primShaderTableAddrLow");
    primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

    userData->setName("userData");
    esGsOffsets01->setName("esGsOffsets01");
    esGsOffsets23->setName("esGsOffsets23");
    gsPrimitiveId->setName("gsPrimitiveId");
    invocationId->setName("invocationId");
    esGsOffsets45->setName("esGsOffsets45");

    if (m_hasTes)
    {
        tessCoordX->setName("tessCoordX");
        tessCoordY->setName("tessCoordY");
        relPatchId->setName("relPatchId");
        patchId->setName("patchId");
    }
    else
    {
        vertexId->setName("vertexId");
        relVertexId->setName("relVertexId");
        vsPrimitiveId->setName("vsPrimitiveId");
        instanceId->setName("instanceId");
    }

    if (m_hasGs)
    {
        // GS is present in primitive shader (ES-GS merged shader)
        constructPrimShaderWithGs(module);
    }
    else
    {
        // GS is not present in primitive shader (ES-only shader)
        constructPrimShaderWithoutGs(module);
    }

    return entryPoint;
}

// =====================================================================================================================
// Constructs primitive shader for ES-only merged shader (GS is not present).
void NggPrimShader::constructPrimShaderWithoutGs(
    Module* module) // [in] LLVM module
{
    assert(m_hasGs == false);

    const bool hasTs = (m_hasTcs || m_hasTes);

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

    auto entryPoint = module->getFunction(lgcName::NggPrimShaderEntryPoint);

    auto arg = entryPoint->arg_begin();

    Value* mergedGroupInfo = (arg + EsGsSysValueMergedGroupInfo);
    Value* mergedWaveInfo = (arg + EsGsSysValueMergedWaveInfo);
    Value* primShaderTableAddrLow = (arg + EsGsSysValuePrimShaderTableAddrLow);
    Value* primShaderTableAddrHigh = (arg + EsGsSysValuePrimShaderTableAddrHigh);

    arg += (EsGsSpecialSysValueCount + 1);

    Value* esGsOffsets01 = arg;
    Value* esGsOffsets23 = (arg + 1);
    Value* gsPrimitiveId = (arg + 2);

    Value* tessCoordX = (arg + 5);
    Value* tessCoordY = (arg + 6);
    Value* relPatchId = (arg + 7);
    Value* patchId = (arg + 8);

    Value* vertexId = (arg + 5);
    Value* instanceId = (arg + 8);

    const auto resUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

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
    const bool distributePrimId = hasTs ? false : resUsage->builtInUsage.vs.primitiveId;

    // No GS in primitive shader (ES only)
    if (m_nggControl->passthroughMode)
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
        auto entryBlock = createBlock(entryPoint, ".entry");

        // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
        BasicBlock* writePrimIdBlock = nullptr;
        BasicBlock* endWritePrimIdBlock = nullptr;
        BasicBlock* readPrimIdBlock = nullptr;
        BasicBlock* endReadPrimIdBlock = nullptr;

        if (distributePrimId)
        {
            writePrimIdBlock = createBlock(entryPoint, ".writePrimId");
            endWritePrimIdBlock = createBlock(entryPoint, ".endWritePrimId");

            readPrimIdBlock = createBlock(entryPoint, ".readPrimId");
            endReadPrimIdBlock = createBlock(entryPoint, ".endReadPrimId");
        }

        auto allocReqBlock = createBlock(entryPoint, ".allocReq");
        auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

        auto expPrimBlock = createBlock(entryPoint, ".expPrim");
        auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

        auto expVertBlock = createBlock(entryPoint, ".expVert");
        auto endExpVertBlock = createBlock(entryPoint, ".endExpVert");

        // Construct ".entry" block
        {
            m_builder->SetInsertPoint(entryBlock);

            initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

            // Record ES-GS vertex offsets info
            m_nggFactor.esGsOffsets01 = esGsOffsets01;

            if (distributePrimId)
            {
                auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
                m_builder->CreateCondBr(primValid, writePrimIdBlock, endWritePrimIdBlock);
            }
            else
            {
                m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto firstWaveInSubgroup =
                    m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
                m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
            }
        }

        if (distributePrimId)
        {
            // Construct ".writePrimId" block
            {
                m_builder->SetInsertPoint(writePrimIdBlock);

                // Primitive data layout
                //   ES_GS_OFFSET01[31]    = null primitive flag
                //   ES_GS_OFFSET01[28:20] = vertexId2 (in bytes)
                //   ES_GS_OFFSET01[18:10] = vertexId1 (in bytes)
                //   ES_GS_OFFSET01[8:0]   = vertexId0 (in bytes)

                // Distribute primitive ID
                auto vertexId0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                              m_builder->getInt32Ty(),
                                                              {
                                                                  m_nggFactor.esGsOffsets01,
                                                                  m_builder->getInt32(0),
                                                                  m_builder->getInt32(9)
                                                              });

                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

                auto ldsOffset = m_builder->CreateShl(vertexId0, 2);
                ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

                auto primIdWriteValue = gsPrimitiveId;
                m_ldsManager->writeValueToLds(primIdWriteValue, ldsOffset);

                BranchInst::Create(endWritePrimIdBlock, writePrimIdBlock);
            }

            // Construct ".endWritePrimId" block
            {
                m_builder->SetInsertPoint(endWritePrimIdBlock);

                m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave,
                                                            m_nggFactor.vertCountInWave);
                m_builder->CreateCondBr(vertValid, readPrimIdBlock, endReadPrimIdBlock);
            }

            // Construct ".readPrimId" block
            Value* primIdReadValue = nullptr;
            {
                m_builder->SetInsertPoint(readPrimIdBlock);

                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

                auto ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
                ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

                primIdReadValue = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

                m_builder->CreateBr(endReadPrimIdBlock);
            }

            // Construct ".endReadPrimId" block
            {
                m_builder->SetInsertPoint(endReadPrimIdBlock);

                auto primitiveId = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);

                primitiveId->addIncoming(primIdReadValue, readPrimIdBlock);
                primitiveId->addIncoming(m_builder->getInt32(0), endWritePrimIdBlock);

                // Record primitive ID
                m_nggFactor.primitiveId = primitiveId;

                m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup,
                                                                     m_builder->getInt32(0));
                m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
            }
        }

        // Construct ".allocReq" block
        {
            m_builder->SetInsertPoint(allocReqBlock);

            doParamCacheAllocRequest();
            m_builder->CreateBr(endAllocReqBlock);
        }

        // Construct ".endAllocReq" block
        {
            m_builder->SetInsertPoint(endAllocReqBlock);

            auto primExp =
                m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
            m_builder->CreateCondBr(primExp, expPrimBlock, endExpPrimBlock);
        }

        // Construct ".expPrim" block
        {
            m_builder->SetInsertPoint(expPrimBlock);

            doPrimitiveExport();
            m_builder->CreateBr(endExpPrimBlock);
        }

        // Construct ".endExpPrim" block
        {
            m_builder->SetInsertPoint(endExpPrimBlock);

            auto vertExp =
                m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
            m_builder->CreateCondBr(vertExp, expVertBlock, endExpVertBlock);
        }

        // Construct ".expVert" block
        {
            m_builder->SetInsertPoint(expVertBlock);

            runEsOrEsVariant(module,
                             lgcName::NggEsEntryPoint,
                             entryPoint->arg_begin(),
                             false,
                             nullptr,
                             expVertBlock);

            m_builder->CreateBr(endExpVertBlock);
        }

        // Construct ".endExpVert" block
        {
            m_builder->SetInsertPoint(endExpVertBlock);

            m_builder->CreateRetVoid();
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

        const bool vertexCompact = (m_nggControl->compactMode == NggCompactVertices);

        // Thread count when the entire sub-group is fully culled
        const unsigned fullyCulledThreadCount =
            m_pipelineState->getTargetInfo().getGpuWorkarounds().gfx10.waNggCullingNoEmptySubgroups ? 1 : 0;

        // Define basic blocks
        auto entryBlock = createBlock(entryPoint, ".entry");

        // NOTE: Those basic blocks are conditionally created on the basis of actual use of primitive ID.
        BasicBlock* writePrimIdBlock = nullptr;
        BasicBlock* endWritePrimIdBlock = nullptr;
        BasicBlock* readPrimIdBlock = nullptr;
        BasicBlock* endReadPrimIdBlock = nullptr;

        if (distributePrimId)
        {
            writePrimIdBlock = createBlock(entryPoint, ".writePrimId");
            endWritePrimIdBlock = createBlock(entryPoint, ".endWritePrimId");

            readPrimIdBlock = createBlock(entryPoint, ".readPrimId");
            endReadPrimIdBlock = createBlock(entryPoint, ".endReadPrimId");
        }

        auto zeroThreadCountBlock = createBlock(entryPoint, ".zeroThreadCount");
        auto endZeroThreadCountBlock = createBlock(entryPoint, ".endZeroThreadCount");

        auto zeroDrawFlagBlock = createBlock(entryPoint, ".zeroDrawFlag");
        auto endZeroDrawFlagBlock = createBlock(entryPoint, ".endZeroDrawFlag");

        auto writePosDataBlock = createBlock(entryPoint, ".writePosData");
        auto endWritePosDataBlock = createBlock(entryPoint, ".endWritePosData");

        auto cullingBlock = createBlock(entryPoint, ".culling");
        auto endCullingBlock = createBlock(entryPoint, ".endCulling");

        auto writeDrawFlagBlock = createBlock(entryPoint, ".writeDrawFlag");
        auto endWriteDrawFlagBlock = createBlock(entryPoint, ".endWriteDrawFlag");

        auto accThreadCountBlock = createBlock(entryPoint, ".accThreadCount");
        auto endAccThreadCountBlock = createBlock(entryPoint, ".endAccThreadCount");

        // NOTE: Those basic blocks are conditionally created on the basis of actual NGG compaction mode.
        BasicBlock* readThreadCountBlock = nullptr;
        BasicBlock* writeCompactDataBlock = nullptr;
        BasicBlock* endReadThreadCountBlock = nullptr;

        if (vertexCompact)
        {
            readThreadCountBlock = createBlock(entryPoint, ".readThreadCount");
            writeCompactDataBlock = createBlock(entryPoint, ".writeCompactData");
            endReadThreadCountBlock = createBlock(entryPoint, ".endReadThreadCount");
        }
        else
        {
            readThreadCountBlock = createBlock(entryPoint, ".readThreadCount");
            endReadThreadCountBlock = createBlock(entryPoint, ".endReadThreadCount");
        }

        auto allocReqBlock = createBlock(entryPoint, ".allocReq");
        auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

        auto earlyExitBlock = createBlock(entryPoint, ".earlyExit");
        auto noEarlyExitBlock = createBlock(entryPoint, ".noEarlyExit");

        auto expPrimBlock = createBlock(entryPoint, ".expPrim");
        auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

        auto expVertPosBlock = createBlock(entryPoint, ".expVertPos");
        auto endExpVertPosBlock = createBlock(entryPoint, ".endExpVertPos");

        auto expVertParamBlock = createBlock(entryPoint, ".expVertParam");
        auto endExpVertParamBlock = createBlock(entryPoint, ".endExpVertParam");

        // Construct ".entry" block
        {
            m_builder->SetInsertPoint(entryBlock);

            initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

            // Record primitive shader table address info
            m_nggFactor.primShaderTableAddrLow  = primShaderTableAddrLow;
            m_nggFactor.primShaderTableAddrHigh = primShaderTableAddrHigh;

            // Record ES-GS vertex offsets info
            m_nggFactor.esGsOffsets01  = esGsOffsets01;
            m_nggFactor.esGsOffsets23  = esGsOffsets23;

            if (distributePrimId)
            {
                auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
                m_builder->CreateCondBr(primValid, writePrimIdBlock, endWritePrimIdBlock);
            }
            else
            {
                auto firstThreadInSubgroup =
                    m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
                m_builder->CreateCondBr(firstThreadInSubgroup, zeroThreadCountBlock, endZeroThreadCountBlock);
            }
        }

        if (distributePrimId)
        {
            // Construct ".writePrimId" block
            {
                m_builder->SetInsertPoint(writePrimIdBlock);

                // Primitive data layout
                //   ES_GS_OFFSET23[15:0]  = vertexId2 (in DWORDs)
                //   ES_GS_OFFSET01[31:16] = vertexId1 (in DWORDs)
                //   ES_GS_OFFSET01[15:0]  = vertexId0 (in DWORDs)

                // Use vertex0 as provoking vertex to distribute primitive ID
                auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                m_builder->getInt32Ty(),
                                                                {
                                                                    m_nggFactor.esGsOffsets01,
                                                                    m_builder->getInt32(0),
                                                                    m_builder->getInt32(16),
                                                                });

                auto vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

                auto ldsOffset = m_builder->CreateShl(vertexId0, 2);
                ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

                auto primIdWriteValue = gsPrimitiveId;
                m_ldsManager->writeValueToLds(primIdWriteValue, ldsOffset);

                m_builder->CreateBr(endWritePrimIdBlock);
            }

            // Construct ".endWritePrimId" block
            {
                m_builder->SetInsertPoint(endWritePrimIdBlock);

                m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto vertValid =
                    m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
                m_builder->CreateCondBr(vertValid, readPrimIdBlock, endReadPrimIdBlock);
            }

            // Construct ".readPrimId" block
            Value* primIdReadValue = nullptr;
            {
                m_builder->SetInsertPoint(readPrimIdBlock);

                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDistribPrimId);

                auto ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
                ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

                primIdReadValue =
                    m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

                m_builder->CreateBr(endReadPrimIdBlock);
            }

            // Construct ".endReadPrimId" block
            {
                m_builder->SetInsertPoint(endReadPrimIdBlock);

                auto primitiveId = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);

                primitiveId->addIncoming(primIdReadValue, readPrimIdBlock);
                primitiveId->addIncoming(m_builder->getInt32(0), endWritePrimIdBlock);

                // Record primitive ID
                m_nggFactor.primitiveId = primitiveId;

                m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

                auto firstThreadInSubgroup =
                    m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
                m_builder->CreateCondBr(firstThreadInSubgroup, zeroThreadCountBlock, endZeroThreadCountBlock);
            }
        }

        // Construct ".zeroThreadCount" block
        {
            m_builder->SetInsertPoint(zeroThreadCountBlock);

            unsigned regionStart = m_ldsManager->getLdsRegionStart(
                vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);

            auto zero = m_builder->getInt32(0);

            // Zero per-wave primitive/vertex count
            auto zeros = ConstantVector::getSplat({Gfx9::NggMaxWavesPerSubgroup, false}, zero);

            auto ldsOffset = m_builder->getInt32(regionStart);
            m_ldsManager->writeValueToLds(zeros, ldsOffset);

            // Zero sub-group primitive/vertex count
            ldsOffset = m_builder->getInt32(regionStart + SizeOfDword * Gfx9::NggMaxWavesPerSubgroup);
            m_ldsManager->writeValueToLds(zero, ldsOffset);

            m_builder->CreateBr(endZeroThreadCountBlock);
        }

        // Construct ".endZeroThreadCount" block
        {
            m_builder->SetInsertPoint(endZeroThreadCountBlock);

            auto firstWaveInSubgroup =
                m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
            m_builder->CreateCondBr(firstWaveInSubgroup, zeroDrawFlagBlock, endZeroDrawFlagBlock);
        }

        // Construct ".zeroDrawFlag" block
        {
            m_builder->SetInsertPoint(zeroDrawFlagBlock);

            Value* ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInWave, 2);

            unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDrawFlag);

            ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

            auto zero = m_builder->getInt32(0);
            m_ldsManager->writeValueToLds(zero, ldsOffset);

            if (waveCountInSubgroup == 8)
            {
                assert(waveSize == 32);
                ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(32 * SizeOfDword));
                m_ldsManager->writeValueToLds(zero, ldsOffset);
            }

            m_builder->CreateBr(endZeroDrawFlagBlock);
        }

        // Construct ".endZeroDrawFlag" block
        {
            m_builder->SetInsertPoint(endZeroDrawFlagBlock);

            auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
            m_builder->CreateCondBr(vertValid, writePosDataBlock, endWritePosDataBlock);
        }

        // Construct ".writePosData" block
        std::vector<ExpData> expDataSet;
        bool separateExp = false;
        {
            m_builder->SetInsertPoint(writePosDataBlock);

            separateExp = (resUsage->resourceWrite == false); // No resource writing

            // NOTE: For vertex compaction, we have to run ES for twice (get vertex position data and
            // get other exported data).
            const auto entryName = (separateExp || vertexCompact) ? lgcName::NggEsEntryVariantPos :
                                                                    lgcName::NggEsEntryVariant;

            runEsOrEsVariant(module,
                             entryName,
                             entryPoint->arg_begin(),
                             false,
                             &expDataSet,
                             writePosDataBlock);

            // Write vertex position data to LDS
            for (const auto& expData : expDataSet)
            {
                if (expData.target == EXP_TARGET_POS_0)
                {
                    const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);
                    assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation

                    Value* ldsOffset =
                        m_builder->CreateMul(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(SizeOfVec4));
                    ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

                    // Use 128-bit LDS store
                    m_ldsManager->writeValueToLds(expData.expValue, ldsOffset, true);

                    break;
                }
            }

            // Write cull distance sign mask to LDS
            if (m_nggControl->enableCullDistanceCulling)
            {
                unsigned clipCullPos = EXP_TARGET_POS_1;
                std::vector<Value*> clipCullDistance;
                std::vector<Value*> cullDistance;

                bool usePointSize     = false;
                bool useLayer         = false;
                bool useViewportIndex = false;
                unsigned clipDistanceCount = 0;
                unsigned cullDistanceCount = 0;

                if (hasTs)
                {
                    const auto& builtInUsage = resUsage->builtInUsage.tes;

                    usePointSize        = builtInUsage.pointSize;
                    useLayer            = builtInUsage.layer;
                    useViewportIndex    = builtInUsage.viewportIndex;
                    clipDistanceCount   = builtInUsage.clipDistance;
                    cullDistanceCount   = builtInUsage.cullDistance;
                }
                else
                {
                    const auto& builtInUsage = resUsage->builtInUsage.vs;

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
                        for (unsigned i = 0; i < 4; ++i)
                        {
                            auto expValue = m_builder->CreateExtractElement(expData.expValue, i);
                            clipCullDistance.push_back(expValue);
                        }
                    }
                }
                assert(clipCullDistance.size() < MaxClipCullDistanceCount);

                for (unsigned i = clipDistanceCount; i < clipDistanceCount + cullDistanceCount; ++i)
                    cullDistance.push_back(clipCullDistance[i]);

                // Calculate the sign mask for cull distance
                Value* signMask = m_builder->getInt32(0);
                for (unsigned i = 0; i < cullDistance.size(); ++i)
                {
                    auto cullDistanceVal = m_builder->CreateBitCast(cullDistance[i], m_builder->getInt32Ty());

                    Value* signBit = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                  m_builder->getInt32Ty(),
                                                                  {
                                                                      cullDistanceVal,
                                                                      m_builder->getInt32(31),
                                                                      m_builder->getInt32(1)
                                                                  });
                    signBit = m_builder->CreateShl(signBit, i);

                    signMask = m_builder->CreateOr(signMask, signBit);
                }

                // Write the sign mask to LDS
                const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionCullDistance);

                Value* ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
                ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

                m_ldsManager->writeValueToLds(signMask, ldsOffset);
            }

            m_builder->CreateBr(endWritePosDataBlock);
        }

        // Construct ".endWritePosData" block
        {
            m_builder->SetInsertPoint(endWritePosDataBlock);

            auto undef = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 4));
            for (auto& expData : expDataSet)
            {
                PHINode* expValue = m_builder->CreatePHI(VectorType::get(Type::getFloatTy(*m_context), 4), 2);
                expValue->addIncoming(expData.expValue, writePosDataBlock);
                expValue->addIncoming(undef, endZeroDrawFlagBlock);

                expData.expValue = expValue; // Update the exportd data
            }

            m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

            auto primValidInWave =
                m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
            auto primValidInSubgroup =
                m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);

            auto primValid = m_builder->CreateAnd(primValidInWave, primValidInSubgroup);
            m_builder->CreateCondBr(primValid, cullingBlock, endCullingBlock);
        }

        // Construct ".culling" block
        Value* doCull = nullptr;
        {
            m_builder->SetInsertPoint(cullingBlock);

            doCull = doCulling(module);
            m_builder->CreateBr(endCullingBlock);
        }

        // Construct ".endCulling" block
        Value* drawFlag = nullptr;
        PHINode* cullFlag = nullptr;
        {
            m_builder->SetInsertPoint(endCullingBlock);

            cullFlag = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);

            cullFlag->addIncoming(m_builder->getTrue(), endWritePosDataBlock);
            cullFlag->addIncoming(doCull, cullingBlock);

            drawFlag = m_builder->CreateNot(cullFlag);
            m_builder->CreateCondBr(drawFlag, writeDrawFlagBlock, endWriteDrawFlagBlock);
        }

        // Construct ".writeDrawFlag" block
        {
            m_builder->SetInsertPoint(writeDrawFlagBlock);

            auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_builder->getInt32Ty(),
                                                            {
                                                                esGsOffsets01,
                                                                m_builder->getInt32(0),
                                                                m_builder->getInt32(16)
                                                            });
            auto vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

            auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_builder->getInt32Ty(),
                                                            {
                                                                esGsOffsets01,
                                                                m_builder->getInt32(16),
                                                                m_builder->getInt32(16)
                                                            });
            auto vertexId1 = m_builder->CreateLShr(esGsOffset1, 2);

            auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_builder->getInt32Ty(),
                                                            {
                                                                esGsOffsets23,
                                                                m_builder->getInt32(0),
                                                                m_builder->getInt32(16)
                                                            });
            auto vertexId2 = m_builder->CreateLShr(esGsOffset2, 2);

            Value* vertexId[3] = { vertexId0, vertexId1, vertexId2 };

            unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDrawFlag);
            auto regionStartVal = m_builder->getInt32(regionStart);

            auto one = m_builder->getInt8(1);

            for (unsigned i = 0; i < 3; ++i)
            {
                auto ldsOffset = m_builder->CreateAdd(regionStartVal, vertexId[i]);
                m_ldsManager->writeValueToLds(one, ldsOffset);
            }

            m_builder->CreateBr(endWriteDrawFlagBlock);
        }

        // Construct ".endWriteDrawFlag" block
        Value* drawCount = nullptr;
        {
            m_builder->SetInsertPoint(endWriteDrawFlagBlock);

            m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

            if (vertexCompact)
            {
                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionDrawFlag);

                auto ldsOffset =
                    m_builder->CreateAdd(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(regionStart));

                drawFlag = m_ldsManager->readValueFromLds(m_builder->getInt8Ty(), ldsOffset);
                drawFlag = m_builder->CreateTrunc(drawFlag, m_builder->getInt1Ty());
            }

            auto drawMask = doSubgroupBallot(drawFlag);

            drawCount = m_builder->CreateIntrinsic(Intrinsic::ctpop, m_builder->getInt64Ty(), drawMask);
            drawCount = m_builder->CreateTrunc(drawCount, m_builder->getInt32Ty());

            auto threadIdUpbound = m_builder->CreateSub(m_builder->getInt32(waveCountInSubgroup),
                                                          m_nggFactor.waveIdInSubgroup);
            auto threadValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, threadIdUpbound);

            Value* primCountAcc = nullptr;
            if (vertexCompact)
                primCountAcc = threadValid;
            else
            {
                auto hasSurviveDraw = m_builder->CreateICmpNE(drawCount, m_builder->getInt32(0));

                primCountAcc = m_builder->CreateAnd(hasSurviveDraw, threadValid);
            }

            m_builder->CreateCondBr(primCountAcc, accThreadCountBlock, endAccThreadCountBlock);
        }

        // Construct ".accThreadCount" block
        {
            m_builder->SetInsertPoint(accThreadCountBlock);

            auto ldsOffset = m_builder->CreateAdd(m_nggFactor.waveIdInSubgroup, m_nggFactor.threadIdInWave);
            ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(1));
            ldsOffset = m_builder->CreateShl(ldsOffset, 2);

            unsigned regionStart = m_ldsManager->getLdsRegionStart(
                vertexCompact ? LdsRegionVertCountInWaves : LdsRegionPrimCountInWaves);

            ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
            m_ldsManager->atomicOpWithLds(AtomicRMWInst::Add, drawCount, ldsOffset);

            m_builder->CreateBr(endAccThreadCountBlock);
        }

        // Construct ".endAccThreadCount" block
        {
            m_builder->SetInsertPoint(endAccThreadCountBlock);

            m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

            if (vertexCompact)
                m_builder->CreateBr(readThreadCountBlock);
            else
            {
                auto firstThreadInWave =
                    m_builder->CreateICmpEQ(m_nggFactor.threadIdInWave, m_builder->getInt32(0));

                m_builder->CreateCondBr(firstThreadInWave, readThreadCountBlock, endReadThreadCountBlock);
            }
        }

        Value* threadCountInWaves = nullptr;
        if (vertexCompact)
        {
            // Construct ".readThreadCount" block
            Value* vertCountInWaves = nullptr;
            Value* vertCountInPrevWaves = nullptr;
            {
                m_builder->SetInsertPoint(readThreadCountBlock);

                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionVertCountInWaves);

                // The DWORD following DWORDs for all waves stores the vertex count of the entire sub-group
                Value* ldsOffset = m_builder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
                vertCountInWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

                // NOTE: We promote vertex count in waves to SGPR since it is treated as an uniform value.
                vertCountInWaves =
                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInWaves);
                threadCountInWaves = vertCountInWaves;

                // Get vertex count for all waves prior to this wave
                ldsOffset = m_builder->CreateShl(m_nggFactor.waveIdInSubgroup, 2);
                ldsOffset = m_builder->CreateAdd(m_builder->getInt32(regionStart), ldsOffset);

                vertCountInPrevWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

                auto vertValid =
                    m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);

                auto compactDataWrite = m_builder->CreateAnd(drawFlag, vertValid);

                m_builder->CreateCondBr(compactDataWrite, writeCompactDataBlock, endReadThreadCountBlock);
            }

            // Construct ".writeCompactData" block
            {
                m_builder->SetInsertPoint(writeCompactDataBlock);

                Value* drawMask = doSubgroupBallot(drawFlag);
                drawMask = m_builder->CreateBitCast(drawMask, VectorType::get(Type::getInt32Ty(*m_context), 2));

                auto drawMaskLow = m_builder->CreateExtractElement(drawMask, static_cast<uint64_t>(0));

                Value* compactThreadIdInSubrgoup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                                                {},
                                                                                {
                                                                                    drawMaskLow,
                                                                                    m_builder->getInt32(0)
                                                                                });

                if (waveSize == 64)
                {
                    auto drawMaskHigh = m_builder->CreateExtractElement(drawMask, 1);

                    compactThreadIdInSubrgoup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                                            {},
                                                                            {
                                                                                drawMaskHigh,
                                                                                compactThreadIdInSubrgoup
                                                                            });
                }

                compactThreadIdInSubrgoup =
                    m_builder->CreateAdd(vertCountInPrevWaves, compactThreadIdInSubrgoup);

                // Write vertex position data to LDS
                for (const auto& expData : expDataSet)
                {
                    if (expData.target == EXP_TARGET_POS_0)
                    {
                        const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);

                        Value* ldsOffset =
                            m_builder->CreateMul(compactThreadIdInSubrgoup, m_builder->getInt32(SizeOfVec4));
                        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

                        m_ldsManager->writeValueToLds(expData.expValue, ldsOffset);

                        break;
                    }
                }

                // Write thread ID in sub-group to LDS
                Value* compactThreadId =
                    m_builder->CreateTrunc(compactThreadIdInSubrgoup, m_builder->getInt8Ty());
                writePerThreadDataToLds(compactThreadId, m_nggFactor.threadIdInSubgroup, LdsRegionVertThreadIdMap);

                if (hasTs)
                {
                    // Write X/Y of tessCoord (U/V) to LDS
                    if (resUsage->builtInUsage.tes.tessCoord)
                    {
                        writePerThreadDataToLds(tessCoordX, compactThreadIdInSubrgoup, LdsRegionCompactTessCoordX);
                        writePerThreadDataToLds(tessCoordY, compactThreadIdInSubrgoup, LdsRegionCompactTessCoordY);
                    }

                    // Write relative patch ID to LDS
                    writePerThreadDataToLds(relPatchId, compactThreadIdInSubrgoup, LdsRegionCompactRelPatchId);

                    // Write patch ID to LDS
                    if (resUsage->builtInUsage.tes.primitiveId)
                        writePerThreadDataToLds(patchId, compactThreadIdInSubrgoup, LdsRegionCompactPatchId);
                }
                else
                {
                    // Write vertex ID to LDS
                    if (resUsage->builtInUsage.vs.vertexIndex)
                        writePerThreadDataToLds(vertexId, compactThreadIdInSubrgoup, LdsRegionCompactVertexId);

                    // Write instance ID to LDS
                    if (resUsage->builtInUsage.vs.instanceIndex)
                        writePerThreadDataToLds(instanceId, compactThreadIdInSubrgoup, LdsRegionCompactInstanceId);

                    // Write primitive ID to LDS
                    if (resUsage->builtInUsage.vs.primitiveId)
                    {
                        assert(m_nggFactor.primitiveId != nullptr);
                        writePerThreadDataToLds(m_nggFactor.primitiveId,
                                                compactThreadIdInSubrgoup,
                                                LdsRegionCompactPrimId);
                    }
                }

                m_builder->CreateBr(endReadThreadCountBlock);
            }

            // Construct ".endReadThreadCount" block
            {
                m_builder->SetInsertPoint(endReadThreadCountBlock);

                Value* hasSurviveVert = m_builder->CreateICmpNE(vertCountInWaves, m_builder->getInt32(0));

                Value* primCountInSubgroup =
                    m_builder->CreateSelect(hasSurviveVert,
                                             m_nggFactor.primCountInSubgroup,
                                             m_builder->getInt32(fullyCulledThreadCount));

                // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
                // as an uniform value later. This is similar to the provided primitive count in sub-group that is
                // a system value.
                primCountInSubgroup =
                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primCountInSubgroup);

                Value* vertCountInSubgroup =
                    m_builder->CreateSelect(hasSurviveVert,
                                             vertCountInWaves,
                                             m_builder->getInt32(fullyCulledThreadCount));

                // NOTE: Here, we have to promote revised vertex count in sub-group to SGPR since it is treated as
                // an uniform value later, similar to what we have done for the revised primitive count in
                // sub-group.
                vertCountInSubgroup =
                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInSubgroup);

                m_nggFactor.primCountInSubgroup = primCountInSubgroup;
                m_nggFactor.vertCountInSubgroup = vertCountInSubgroup;

                auto firstWaveInSubgroup =
                    m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));

                m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
            }
        }
        else
        {
            // Construct ".readThreadCount" block
            Value* primCountInWaves = nullptr;
            {
                m_builder->SetInsertPoint(readThreadCountBlock);

                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPrimCountInWaves);

                // The DWORD following DWORDs for all waves stores the primitive count of the entire sub-group
                auto ldsOffset = m_builder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
                primCountInWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

                m_builder->CreateBr(endReadThreadCountBlock);
            }

            // Construct ".endReadThreadCount" block
            {
                m_builder->SetInsertPoint(endReadThreadCountBlock);

                Value* primCount = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);

                static_cast<PHINode*>(primCount)->addIncoming(m_nggFactor.primCountInSubgroup,
                                                               endAccThreadCountBlock);
                static_cast<PHINode*>(primCount)->addIncoming(primCountInWaves, readThreadCountBlock);

                // NOTE: We promote primitive count in waves to SGPR since it is treated as an uniform value.
                primCount = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primCount);
                threadCountInWaves = primCount;

                Value* hasSurvivePrim = m_builder->CreateICmpNE(primCount, m_builder->getInt32(0));

                Value* primCountInSubgroup =
                    m_builder->CreateSelect(hasSurvivePrim,
                                             m_nggFactor.primCountInSubgroup,
                                             m_builder->getInt32(fullyCulledThreadCount));

                // NOTE: Here, we have to promote revised primitive count in sub-group to SGPR since it is treated
                // as an uniform value later. This is similar to the provided primitive count in sub-group that is
                // a system value.
                primCountInSubgroup =
                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, primCountInSubgroup);

                Value* vertCountInSubgroup =
                    m_builder->CreateSelect(hasSurvivePrim,
                                             m_nggFactor.vertCountInSubgroup,
                                             m_builder->getInt32(fullyCulledThreadCount));

                // NOTE: Here, we have to promote revised vertex count in sub-group to SGPR since it is treated as
                // an uniform value later, similar to what we have done for the revised primitive count in
                // sub-group.
                vertCountInSubgroup =
                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInSubgroup);

                m_nggFactor.primCountInSubgroup = primCountInSubgroup;
                m_nggFactor.vertCountInSubgroup = vertCountInSubgroup;

                auto firstWaveInSubgroup =
                    m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));

                m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
            }
        }

        // Construct ".allocReq" block
        {
            m_builder->SetInsertPoint(allocReqBlock);

            doParamCacheAllocRequest();
            m_builder->CreateBr(endAllocReqBlock);
        }

        // Construct ".endAllocReq" block
        {
            m_builder->SetInsertPoint(endAllocReqBlock);

            m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

            auto noSurviveThread = m_builder->CreateICmpEQ(threadCountInWaves, m_builder->getInt32(0));
            m_builder->CreateCondBr(noSurviveThread, earlyExitBlock, noEarlyExitBlock);
        }

        // Construct ".earlyExit" block
        {
            m_builder->SetInsertPoint(earlyExitBlock);

            unsigned expPosCount = 0;
            for (const auto& expData : expDataSet)
            {
                if ((expData.target >= EXP_TARGET_POS_0) && (expData.target <= EXP_TARGET_POS_4))
                    ++expPosCount;
            }

            doEarlyExit(fullyCulledThreadCount, expPosCount);
        }

        // Construct ".noEarlyExit" block
        {
            m_builder->SetInsertPoint(noEarlyExitBlock);

            auto primExp =
                m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
            m_builder->CreateCondBr(primExp, expPrimBlock, endExpPrimBlock);
        }

        // Construct ".expPrim" block
        {
            m_builder->SetInsertPoint(expPrimBlock);

            doPrimitiveExport(vertexCompact ? cullFlag : nullptr);
            m_builder->CreateBr(endExpPrimBlock);
        }

        // Construct ".endExpPrim" block
        Value* vertExp = nullptr;
        {
            m_builder->SetInsertPoint(endExpPrimBlock);

            vertExp =
                m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
            m_builder->CreateCondBr(vertExp, expVertPosBlock, endExpVertPosBlock);
        }

        // Construct ".expVertPos" block
        {
            m_builder->SetInsertPoint(expVertPosBlock);

            // NOTE: For vertex compaction, we have to run ES to get exported data once again.
            if (vertexCompact)
            {
                expDataSet.clear();

                runEsOrEsVariant(module,
                                 lgcName::NggEsEntryVariant,
                                 entryPoint->arg_begin(),
                                 true,
                                 &expDataSet,
                                 expVertPosBlock);

                // For vertex position, we get the exported data from LDS
                for (auto& expData : expDataSet)
                {
                    if (expData.target == EXP_TARGET_POS_0)
                    {
                        const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);
                        assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation

                        auto ldsOffset =
                            m_builder->CreateMul(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(SizeOfVec4));
                        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

                        // Use 128-bit LDS load
                        auto expValue =
                            m_ldsManager->readValueFromLds(VectorType::get(Type::getFloatTy(*m_context), 4),
                                                            ldsOffset,
                                                            true);
                        expData.expValue = expValue;

                        break;
                    }
                }
            }

            for (const auto& expData : expDataSet)
            {
                if ((expData.target >= EXP_TARGET_POS_0) && (expData.target <= EXP_TARGET_POS_4))
                {
                    std::vector<Value*> args;

                    args.push_back(m_builder->getInt32(expData.target));        // tgt
                    args.push_back(m_builder->getInt32(expData.channelMask));   // en

                    // src0 ~ src3
                    for (unsigned i = 0; i < 4; ++i)
                    {
                        auto expValue = m_builder->CreateExtractElement(expData.expValue, i);
                        args.push_back(expValue);
                    }

                    args.push_back(m_builder->getInt1(expData.doneFlag));       // done
                    args.push_back(m_builder->getFalse());                      // vm

                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getFloatTy(), args);
                }
            }

            m_builder->CreateBr(endExpVertPosBlock);
        }

        // Construct ".endExpVertPos" block
        {
            m_builder->SetInsertPoint(endExpVertPosBlock);

            if (vertexCompact)
            {
                auto undef = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 4));
                for (auto& expData : expDataSet)
                {
                    PHINode* expValue = m_builder->CreatePHI(VectorType::get(Type::getFloatTy(*m_context), 4), 2);

                    expValue->addIncoming(expData.expValue, expVertPosBlock);
                    expValue->addIncoming(undef, endExpPrimBlock);

                    expData.expValue = expValue; // Update the exportd data
                }
            }

            m_builder->CreateCondBr(vertExp, expVertParamBlock, endExpVertParamBlock);
        }

        // Construct ".expVertParam" block
        {
            m_builder->SetInsertPoint(expVertParamBlock);

            // NOTE: For vertex compaction, ES must have been run in ".expVertPos" block.
            if (vertexCompact == false)
            {
                if (separateExp)
                {
                    // Should run ES variant to get exported parameter data
                    expDataSet.clear();

                    runEsOrEsVariant(module,
                                     lgcName::NggEsEntryVariantParam,
                                     entryPoint->arg_begin(),
                                     false,
                                     &expDataSet,
                                     expVertParamBlock);
                }
            }

            for (const auto& expData : expDataSet)
            {
                if ((expData.target >= EXP_TARGET_PARAM_0) && (expData.target <= EXP_TARGET_PARAM_31))
                {
                    std::vector<Value*> args;

                    args.push_back(m_builder->getInt32(expData.target));        // tgt
                    args.push_back(m_builder->getInt32(expData.channelMask));   // en

                                                                                    // src0 ~ src3
                    for (unsigned i = 0; i < 4; ++i)
                    {
                        auto expValue = m_builder->CreateExtractElement(expData.expValue, i);
                        args.push_back(expValue);
                    }

                    args.push_back(m_builder->getInt1(expData.doneFlag));       // done
                    args.push_back(m_builder->getFalse());                      // vm

                    m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp, m_builder->getFloatTy(), args);
                }
            }

            m_builder->CreateBr(endExpVertParamBlock);
        }

        // Construct ".endExpVertParam" block
        {
            m_builder->SetInsertPoint(endExpVertParamBlock);

            m_builder->CreateRetVoid();
        }
    }
}

// =====================================================================================================================
// Constructs primitive shader for ES-GS merged shader (GS is present).
void NggPrimShader::constructPrimShaderWithGs(
    Module* module) // [in] LLVM module
{
    assert(m_hasGs);

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    const unsigned waveCountInSubgroup = Gfx9::NggMaxThreadsPerSubgroup / waveSize;

    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    const unsigned rasterStream = resUsage->inOutUsage.gs.rasterStream;
    assert(rasterStream < MaxGsStreams);

    const auto& calcFactor = resUsage->inOutUsage.gs.calcFactor;
    const unsigned maxOutPrims = calcFactor.primAmpFactor;

    auto entryPoint = module->getFunction(lgcName::NggPrimShaderEntryPoint);

    auto arg = entryPoint->arg_begin();

    Value* mergedGroupInfo = (arg + EsGsSysValueMergedGroupInfo);
    Value* mergedWaveInfo = (arg + EsGsSysValueMergedWaveInfo);

    arg += (EsGsSpecialSysValueCount + 1);

    Value* esGsOffsets01 = arg;
    Value* esGsOffsets23 = (arg + 1);
    Value* esGsOffsets45 = (arg + 4);

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
    auto entryBlock = createBlock(entryPoint, ".entry");

    auto beginEsBlock = createBlock(entryPoint, ".beginEs");
    auto endEsBlock = createBlock(entryPoint, ".endEs");

    auto initOutPrimDataBlock = createBlock(entryPoint, ".initOutPrimData");
    auto endInitOutPrimDataBlock = createBlock(entryPoint, ".endInitOutPrimData");

    auto zeroOutVertCountBlock = createBlock(entryPoint, ".zeroOutVertCount");
    auto endZeroOutVertCountBlock = createBlock(entryPoint, ".endZeroOutVertCount");

    auto beginGsBlock = createBlock(entryPoint, ".beginGs");
    auto endGsBlock = createBlock(entryPoint, ".endGs");

    auto accVertCountBlock = createBlock(entryPoint, ".accVertCount");
    auto endAccVertCountBlock = createBlock(entryPoint, ".endAccVertCount");

    auto readVertCountBlock = createBlock(entryPoint, ".readVertCount");
    auto endReadVertCountBlock = createBlock(entryPoint, ".endReadVertCount");

    auto allocReqBlock = createBlock(entryPoint, ".allocReq");
    auto endAllocReqBlock = createBlock(entryPoint, ".endAllocReq");

    auto reviseOutPrimDataBlock = createBlock(entryPoint, ".reviseOutPrimData");
    auto reviseOutPrimDataLoopBlock = createBlock(entryPoint, ".reviseOutPrimDataLoop");
    auto endReviseOutPrimDataBlock = createBlock(entryPoint, ".endReviseOutPrimData");

    auto expPrimBlock = createBlock(entryPoint, ".expPrim");
    auto endExpPrimBlock = createBlock(entryPoint, ".endExpPrim");

    auto writeOutVertOffsetBlock = createBlock(entryPoint, ".writeOutVertOffset");
    auto writeOutVertOffsetLoopBlock = createBlock(entryPoint, ".writeOutVertOffsetLoop");
    auto endWriteOutVertOffsetBlock = createBlock(entryPoint, ".endWriteOutVertOffset");

    auto expVertBlock = createBlock(entryPoint, ".expVert");
    auto endExpVertBlock = createBlock(entryPoint, ".endExpVert");

    // Construct ".entry" block
    {
        m_builder->SetInsertPoint(entryBlock);

        initWaveThreadInfo(mergedGroupInfo, mergedWaveInfo);

        // Record ES-GS vertex offsets info
        m_nggFactor.esGsOffsets01 = esGsOffsets01;
        m_nggFactor.esGsOffsets23 = esGsOffsets23;
        m_nggFactor.esGsOffsets45 = esGsOffsets45;

        auto vertValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.vertCountInWave);
        m_builder->CreateCondBr(vertValid, beginEsBlock, endEsBlock);
    }

    // Construct ".beginEs" block
    {
        m_builder->SetInsertPoint(beginEsBlock);

        runEsOrEsVariant(module,
                         lgcName::NggEsEntryPoint,
                         entryPoint->arg_begin(),
                         false,
                         nullptr,
                         beginEsBlock);

        m_builder->CreateBr(endEsBlock);
    }

    // Construct ".endEs" block
    {
        m_builder->SetInsertPoint(endEsBlock);

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
        m_builder->CreateCondBr(primValid, initOutPrimDataBlock, endInitOutPrimDataBlock);
    }

    // Construct ".initOutPrimData" block
    {
        m_builder->SetInsertPoint(initOutPrimDataBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimData);

        auto ldsOffset = m_builder->CreateMul(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(maxOutPrims));
        ldsOffset = m_builder->CreateShl(ldsOffset, 2);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

        auto nullPrimVal = m_builder->getInt32(NullPrim);
        Value* nullPrims = UndefValue::get(VectorType::get(m_builder->getInt32Ty(), maxOutPrims));
        for (unsigned i = 0; i < maxOutPrims; ++i)
            nullPrims = m_builder->CreateInsertElement(nullPrims, nullPrimVal, i);

        m_ldsManager->writeValueToLds(nullPrims, ldsOffset);

        m_builder->CreateBr(endInitOutPrimDataBlock);
    }

    // Construct ".endInitOutPrimData" block
    {
        m_builder->SetInsertPoint(endInitOutPrimDataBlock);

        auto firstThreadInSubgroup =
            m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstThreadInSubgroup, zeroOutVertCountBlock, endZeroOutVertCountBlock);
    }

    // Construct ".zeroOutVertCount" block
    {
        m_builder->SetInsertPoint(zeroOutVertCountBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

        auto zero = m_builder->getInt32(0);

        for (unsigned i = 0; i < MaxGsStreams; ++i)
        {
            // NOTE: Only do this for rasterization stream.
            if (i == rasterStream)
            {
                // Zero per-wave GS output vertex count
                auto zeros = ConstantVector::getSplat({Gfx9::NggMaxWavesPerSubgroup, false}, zero);

                auto ldsOffset =
                    m_builder->getInt32(regionStart + i * SizeOfDword * (Gfx9::NggMaxWavesPerSubgroup + 1));
                m_ldsManager->writeValueToLds(zeros, ldsOffset);

                // Zero sub-group GS output vertex count
                ldsOffset = m_builder->getInt32(regionStart + SizeOfDword * Gfx9::NggMaxWavesPerSubgroup);
                m_ldsManager->writeValueToLds(zero, ldsOffset);

                break;
            }
        }

        m_builder->CreateBr(endZeroOutVertCountBlock);
    }

    // Construct ".endZeroOutVertCount" block
    {
        m_builder->SetInsertPoint(endZeroOutVertCountBlock);

        auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
        m_builder->CreateCondBr(primValid, beginGsBlock, endGsBlock);
    }

    // Construct ".beginGs" block
    Value* outPrimCount = nullptr;
    Value* outVertCount = nullptr;
    Value* inclusiveOutVertCount = nullptr;
    Value* outVertCountInWave = nullptr;
    {
        m_builder->SetInsertPoint(beginGsBlock);

        Value* outPrimVertCountInfo = runGsVariant(module, entryPoint->arg_begin(), beginGsBlock);

        // Extract output primitive/vertex count info from the return value
        assert(outPrimVertCountInfo->getType()->isStructTy());
        outPrimCount = m_builder->CreateExtractValue(outPrimVertCountInfo, 0);
        outVertCount = m_builder->CreateExtractValue(outPrimVertCountInfo, 1);
        inclusiveOutVertCount = m_builder->CreateExtractValue(outPrimVertCountInfo, 2);
        outVertCountInWave = m_builder->CreateExtractValue(outPrimVertCountInfo, 3);

        m_builder->CreateBr(endGsBlock);
    }

    // Construct ".endGs" block
    {
        m_builder->SetInsertPoint(endGsBlock);

        auto outPrimCountPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        outPrimCountPhi->addIncoming(m_builder->getInt32(0), endZeroOutVertCountBlock);
        outPrimCountPhi->addIncoming(outPrimCount, beginGsBlock);
        outPrimCount = outPrimCountPhi;
        outPrimCount->setName("outPrimCount");

        auto outVertCountPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        outVertCountPhi->addIncoming(m_builder->getInt32(0), endZeroOutVertCountBlock);
        outVertCountPhi->addIncoming(outVertCount, beginGsBlock);
        outVertCount = outVertCountPhi;
        outVertCount->setName("outVertCount");

        auto inclusiveOutVertCountPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        inclusiveOutVertCountPhi->addIncoming(m_builder->getInt32(0), endZeroOutVertCountBlock);
        inclusiveOutVertCountPhi->addIncoming(inclusiveOutVertCount, beginGsBlock);
        inclusiveOutVertCount = inclusiveOutVertCountPhi;
        inclusiveOutVertCount->setName("inclusiveOutVertCount");

        auto outVertCountInWavePhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        outVertCountInWavePhi->addIncoming(m_builder->getInt32(0), endZeroOutVertCountBlock);
        outVertCountInWavePhi->addIncoming(outVertCountInWave, beginGsBlock);
        outVertCountInWave = outVertCountInWavePhi;
        // NOTE: We promote GS output vertex count in wave to SGPR since it is treated as an uniform value. Otherwise,
        // phi node resolving still treats it as VGPR, not as expected.
        outVertCountInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, outVertCountInWave);
        outVertCountInWave->setName("outVertCountInWave");

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto hasSurviveVert = m_builder->CreateICmpNE(outVertCountInWave, m_builder->getInt32(0));

        auto threadIdUpbound = m_builder->CreateSub(m_builder->getInt32(waveCountInSubgroup),
                                                      m_nggFactor.waveIdInSubgroup);
        auto threadValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, threadIdUpbound);

        auto vertCountAcc = m_builder->CreateAnd(hasSurviveVert, threadValid);

        m_builder->CreateCondBr(vertCountAcc, accVertCountBlock, endAccVertCountBlock);
    }

    // Construct ".accVertCount" block
    {
        m_builder->SetInsertPoint(accVertCountBlock);

        auto ldsOffset = m_builder->CreateAdd(m_nggFactor.waveIdInSubgroup, m_nggFactor.threadIdInWave);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(1));
        ldsOffset = m_builder->CreateShl(ldsOffset, 2);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
        m_ldsManager->atomicOpWithLds(AtomicRMWInst::Add, outVertCountInWave, ldsOffset);

        m_builder->CreateBr(endAccVertCountBlock);
    }

    // Construct ".endAccVertCount" block
    {
        m_builder->SetInsertPoint(endAccVertCountBlock);

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto firstThreadInWave = m_builder->CreateICmpEQ(m_nggFactor.threadIdInWave, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstThreadInWave, readVertCountBlock, endReadVertCountBlock);
    }

    // Construct ".readVertCount" block
    Value* outVertCountInWaves = nullptr;
    {
        m_builder->SetInsertPoint(readVertCountBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

        // The DWORD following DWORDs for all waves stores GS output vertex count of the entire sub-group
        auto ldsOffset = m_builder->getInt32(regionStart + waveCountInSubgroup * SizeOfDword);
        outVertCountInWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

        m_builder->CreateBr(endReadVertCountBlock);
    }

    // Construct ".endReadVertCount" block
    {
        m_builder->SetInsertPoint(endReadVertCountBlock);

        Value* vertCountInSubgroup = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        static_cast<PHINode*>(vertCountInSubgroup)->addIncoming(m_builder->getInt32(0), endAccVertCountBlock);
        static_cast<PHINode*>(vertCountInSubgroup)->addIncoming(outVertCountInWaves, readVertCountBlock);

        // NOTE: We promote GS output vertex count in subgroup to SGPR since it is treated as an uniform value.
        vertCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, vertCountInSubgroup);

        m_nggFactor.vertCountInSubgroup = vertCountInSubgroup;

        auto firstWaveInSubgroup = m_builder->CreateICmpEQ(m_nggFactor.waveIdInSubgroup, m_builder->getInt32(0));
        m_builder->CreateCondBr(firstWaveInSubgroup, allocReqBlock, endAllocReqBlock);
    }

    // Construct ".allocReq" block
    {
        m_builder->SetInsertPoint(allocReqBlock);

        doParamCacheAllocRequest();
        m_builder->CreateBr(endAllocReqBlock);
    }

    // Construct ".endAllocReq" block
    {
        m_builder->SetInsertPoint(endAllocReqBlock);

        auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
        m_builder->CreateCondBr(primValid, reviseOutPrimDataBlock, endReviseOutPrimDataBlock);
    }

    // Construct ".reviseOutPrimData" block
    Value* vertexIdAdjust = nullptr;
    {
        m_builder->SetInsertPoint(reviseOutPrimDataBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

        auto ldsOffset = m_builder->CreateShl(m_nggFactor.waveIdInSubgroup, 2);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
        auto outVertCountInPreWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

        // vertexIdAdjust = outVertCountInPreWaves + exclusiveOutVertCount
        auto exclusiveOutVertCount = m_builder->CreateSub(inclusiveOutVertCount, outVertCount);
        vertexIdAdjust = m_builder->CreateAdd(outVertCountInPreWaves, exclusiveOutVertCount);

        auto adjustVertexId = m_builder->CreateICmpNE(vertexIdAdjust, m_builder->getInt32(0));
        m_builder->CreateCondBr(adjustVertexId, reviseOutPrimDataLoopBlock, endReviseOutPrimDataBlock);
    }

    // Construct ".reviseOutPrimDataLoop" block
    {
        m_builder->SetInsertPoint(reviseOutPrimDataLoopBlock);

        //
        // The processing is something like this:
        //   for (outPrimId = 0; outPrimId < outPrimCount; outPrimId++)
        //   {
        //       ldsOffset = regionStart + 4 * (threadIdInSubgroup * maxOutPrims + outPrimId)
        //       Read GS output primitive data from LDS, revise them, and write back to LDS
        //   }
        //
        auto outPrimIdPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        outPrimIdPhi->addIncoming(m_builder->getInt32(0), reviseOutPrimDataBlock); // outPrimId = 0

        reviseOutputPrimitiveData(outPrimIdPhi, vertexIdAdjust);

        auto outPrimId = m_builder->CreateAdd(outPrimIdPhi, m_builder->getInt32(1)); // outPrimId++
        outPrimIdPhi->addIncoming(outPrimId, reviseOutPrimDataLoopBlock);

        auto reviseContinue = m_builder->CreateICmpULT(outPrimId, outPrimCount);
        m_builder->CreateCondBr(reviseContinue, reviseOutPrimDataLoopBlock, endReviseOutPrimDataBlock);
    }

    // Construct ".endReviseOutPrimData" block
    {
        m_builder->SetInsertPoint(endReviseOutPrimDataBlock);

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto primExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.primCountInSubgroup);
        m_builder->CreateCondBr(primExp, expPrimBlock, endExpPrimBlock);
    }

    // Construct ".expPrim" block
    {
        m_builder->SetInsertPoint(expPrimBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimData);

        auto ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

        auto primData = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

        auto undef = UndefValue::get(m_builder->getInt32Ty());
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                    m_builder->getInt32Ty(),
                                    {
                                        m_builder->getInt32(EXP_TARGET_PRIM),      // tgt
                                        m_builder->getInt32(0x1),                  // en
                                        primData,                                  // src0 ~ src3
                                        undef,
                                        undef,
                                        undef,
                                        m_builder->getTrue(),                      // done, must be set
                                        m_builder->getFalse(),                     // vm
                                    });

        m_builder->CreateBr(endExpPrimBlock);
    }

    // Construct ".endExpPrim" block
    {
        m_builder->SetInsertPoint(endExpPrimBlock);

        auto primValid = m_builder->CreateICmpULT(m_nggFactor.threadIdInWave, m_nggFactor.primCountInWave);
        m_builder->CreateCondBr(primValid, writeOutVertOffsetBlock, endWriteOutVertOffsetBlock);
    }

    // Construct ".writeOutVertOffset" block
    Value* writeOffset = nullptr;
    Value* writeValue = nullptr;
    {
        m_builder->SetInsertPoint(writeOutVertOffsetBlock);

        unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertCountInWaves);

        auto ldsOffset = m_builder->CreateShl(m_nggFactor.waveIdInSubgroup, 2);
        ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
        auto outVertCountInPrevWaves = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

        // outVertThreadId = outVertCountInPrevWaves + exclusiveOutVertCount
        auto exclusiveOutVertCount = m_builder->CreateSub(inclusiveOutVertCount, outVertCount);
        auto outVertThreadId = m_builder->CreateAdd(outVertCountInPrevWaves, exclusiveOutVertCount);

        // writeOffset = regionStart (OutVertOffset) + outVertThreadId * 4
        regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertOffset);
        writeOffset = m_builder->CreateShl(outVertThreadId, 2);
        writeOffset = m_builder->CreateAdd(writeOffset, m_builder->getInt32(regionStart));

        // vertexItemOffset = threadIdInSubgroup * gsVsRingItemSize * 4 (in BYTE)
        auto vertexItemOffset = m_builder->CreateMul(m_nggFactor.threadIdInSubgroup,
                                                       m_builder->getInt32(calcFactor.gsVsRingItemSize * 4));

        // writeValue = regionStart (GsVsRing) + vertexItemOffset
        regionStart = m_ldsManager->getLdsRegionStart(LdsRegionGsVsRing);
        writeValue = m_builder->CreateAdd(vertexItemOffset, m_builder->getInt32(regionStart));

        m_builder->CreateBr(writeOutVertOffsetLoopBlock);
    }

    // Construct ".writeOutVertOffsetLoop" block
    {
        m_builder->SetInsertPoint(writeOutVertOffsetLoopBlock);

        //
        // The processing is something like this:
        //   for (outVertIdInPrim = 0; outVertIdInPrim < outVertCount; outVertIdInPrim++)
        //   {
        //       ldsOffset = writeOffset + 4 * outVertIdInPrim
        //       vertexOffset = writeValue + 4 * vertexSize * outVertIdInPrim
        //       Write GS output vertex offset to LDS
        //   }
        //
        auto outVertIdInPrimPhi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
        outVertIdInPrimPhi->addIncoming(m_builder->getInt32(0), writeOutVertOffsetBlock); // outVertIdInPrim = 0

        auto ldsOffset = m_builder->CreateShl(outVertIdInPrimPhi, 2);
        ldsOffset = m_builder->CreateAdd(ldsOffset, writeOffset);

        const unsigned vertexSize = resUsage->inOutUsage.gs.outLocCount[rasterStream] * 4;
        auto vertexoffset = m_builder->CreateMul(outVertIdInPrimPhi, m_builder->getInt32(4 * vertexSize));
        vertexoffset = m_builder->CreateAdd(vertexoffset, writeValue);

        m_ldsManager->writeValueToLds(vertexoffset, ldsOffset);

        auto outVertIdInPrim =
            m_builder->CreateAdd(outVertIdInPrimPhi, m_builder->getInt32(1)); // outVertIdInPrim++
        outVertIdInPrimPhi->addIncoming(outVertIdInPrim, writeOutVertOffsetLoopBlock);

        auto writeContinue = m_builder->CreateICmpULT(outVertIdInPrim, outVertCount);
        m_builder->CreateCondBr(writeContinue, writeOutVertOffsetLoopBlock, endWriteOutVertOffsetBlock);
    }

    // Construct ".endWriteOutVertOffset" block
    {
        m_builder->SetInsertPoint(endWriteOutVertOffsetBlock);

        m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_barrier, {}, {});

        auto vertExp = m_builder->CreateICmpULT(m_nggFactor.threadIdInSubgroup, m_nggFactor.vertCountInSubgroup);
        m_builder->CreateCondBr(vertExp, expVertBlock, endExpVertBlock);
    }

    // Construct ".expVert" block
    {
        m_builder->SetInsertPoint(expVertBlock);

        runCopyShader(module, expVertBlock);
        m_builder->CreateBr(endExpVertBlock);
    }

    // Construct ".endExpVert" block
    {
        m_builder->SetInsertPoint(endExpVertBlock);

        m_builder->CreateRetVoid();
    }
}

// =====================================================================================================================
// Extracts merged group/wave info and initializes part of NGG calculation factors.
//
// NOTE: This function must be invoked by the entry block of NGG shader module.
void NggPrimShader::initWaveThreadInfo(
    Value* mergedGroupInfo,    // [in] Merged group info
    Value* mergedWaveInfo)     // [in] Merged wave info
{
    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_init_exec, {}, m_builder->getInt64(-1));

    auto threadIdInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                       {},
                                                       {
                                                           m_builder->getInt32(-1),
                                                           m_builder->getInt32(0)
                                                       });

    if (waveSize == 64)
    {
        threadIdInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                     {},
                                                     {
                                                         m_builder->getInt32(-1),
                                                         threadIdInWave
                                                     });
    }

    auto primCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_builder->getInt32Ty(),
                                                            {
                                                                mergedGroupInfo,
                                                                m_builder->getInt32(22),
                                                                m_builder->getInt32(9)
                                                            });

    auto vertCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                            m_builder->getInt32Ty(),
                                                            {
                                                                mergedGroupInfo,
                                                                m_builder->getInt32(12),
                                                                m_builder->getInt32(9)
                                                            });

    auto vertCountInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_builder->getInt32Ty(),
                                                        {
                                                            mergedWaveInfo,
                                                            m_builder->getInt32(0),
                                                            m_builder->getInt32(8)
                                                        });

    auto primCountInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_builder->getInt32Ty(),
                                                        {
                                                            mergedWaveInfo,
                                                            m_builder->getInt32(8),
                                                            m_builder->getInt32(8)
                                                        });

    auto waveIdInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                         m_builder->getInt32Ty(),
                                                         {
                                                             mergedWaveInfo,
                                                             m_builder->getInt32(24),
                                                             m_builder->getInt32(4)
                                                         });

    auto threadIdInSubgroup = m_builder->CreateMul(waveIdInSubgroup, m_builder->getInt32(waveSize));
    threadIdInSubgroup = m_builder->CreateAdd(threadIdInSubgroup, threadIdInWave);

    primCountInSubgroup->setName("primCountInSubgroup");
    vertCountInSubgroup->setName("vertCountInSubgroup");
    primCountInWave->setName("primCountInWave");
    vertCountInWave->setName("vertCountInWave");
    threadIdInWave->setName("threadIdInWave");
    threadIdInSubgroup->setName("threadIdInSubgroup");
    waveIdInSubgroup->setName("waveIdInSubgroup");

    // Record wave/thread info
    m_nggFactor.primCountInSubgroup    = primCountInSubgroup;
    m_nggFactor.vertCountInSubgroup    = vertCountInSubgroup;
    m_nggFactor.primCountInWave        = primCountInWave;
    m_nggFactor.vertCountInWave        = vertCountInWave;
    m_nggFactor.threadIdInWave         = threadIdInWave;
    m_nggFactor.threadIdInSubgroup     = threadIdInSubgroup;
    m_nggFactor.waveIdInSubgroup       = waveIdInSubgroup;

    m_nggFactor.mergedGroupInfo        = mergedGroupInfo;
}

// =====================================================================================================================
// Does various culling for NGG primitive shader.
Value* NggPrimShader::doCulling(
    Module* module)    // [in] LLVM module
{
    Value* cullFlag = m_builder->getFalse();

    // Skip culling if it is not requested
    if (enableCulling() == false)
        return cullFlag;

    auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.esGsOffsets01,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16),
                                                    });
    auto vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

    auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.esGsOffsets01,
                                                        m_builder->getInt32(16),
                                                        m_builder->getInt32(16),
                                                    });
    auto vertexId1 = m_builder->CreateLShr(esGsOffset1, 2);

    auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        m_nggFactor.esGsOffsets23,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16),
                                                    });
    auto vertexId2 = m_builder->CreateLShr(esGsOffset2, 2);

    Value* vertexId[3] = { vertexId0, vertexId1, vertexId2 };
    Value* vertex[3] = {};

    const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionPosData);
    assert(regionStart % SizeOfVec4 == 0); // Use 128-bit LDS operation
    auto regionStartVal = m_builder->getInt32(regionStart);

    for (unsigned i = 0; i < 3; ++i)
    {
        Value* ldsOffset = m_builder->CreateMul(vertexId[i], m_builder->getInt32(SizeOfVec4));
        ldsOffset = m_builder->CreateAdd(ldsOffset, regionStartVal);

        // Use 128-bit LDS load
        vertex[i] = m_ldsManager->readValueFromLds(
            VectorType::get(Type::getFloatTy(*m_context), 4), ldsOffset, true);
    }

    // Handle backface culling
    if (m_nggControl->enableBackfaceCulling)
        cullFlag = doBackfaceCulling(module, cullFlag, vertex[0], vertex[1], vertex[2]);

    // Handle frustum culling
    if (m_nggControl->enableFrustumCulling)
        cullFlag = doFrustumCulling(module, cullFlag, vertex[0], vertex[1], vertex[2]);

    // Handle box filter culling
    if (m_nggControl->enableBoxFilterCulling)
        cullFlag = doBoxFilterCulling(module, cullFlag, vertex[0], vertex[1], vertex[2]);

    // Handle sphere culling
    if (m_nggControl->enableSphereCulling)
        cullFlag = doSphereCulling(module, cullFlag, vertex[0], vertex[1], vertex[2]);

    // Handle small primitive filter culling
    if (m_nggControl->enableSmallPrimFilter)
        cullFlag = doSmallPrimFilterCulling(module, cullFlag, vertex[0], vertex[1], vertex[2]);

    // Handle cull distance culling
    if (m_nggControl->enableCullDistanceCulling)
    {
        Value* signMask[3] = {};

        const auto regionStart = m_ldsManager->getLdsRegionStart(LdsRegionCullDistance);
        auto regionStartVal = m_builder->getInt32(regionStart);

        for (unsigned i = 0; i < 3; ++i)
        {
            Value* ldsOffset = m_builder->CreateShl(vertexId[i], 2);
            ldsOffset = m_builder->CreateAdd(ldsOffset, regionStartVal);

            signMask[i] = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);
        }

        cullFlag = doCullDistanceCulling(module, cullFlag, signMask[0], signMask[1], signMask[2]);
    }

    return cullFlag;
}

// =====================================================================================================================
// Requests that parameter cache space be allocated (send the message GS_ALLOC_REQ).
void NggPrimShader::doParamCacheAllocRequest()
{
    // M0[10:0] = vertCntInSubgroup, M0[22:12] = primCntInSubgroup
    Value* m0 = m_builder->CreateShl(m_nggFactor.primCountInSubgroup, 12);
    m0 = m_builder->CreateOr(m0, m_nggFactor.vertCountInSubgroup);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, { m_builder->getInt32(GsAllocReq), m0 });
}

// =====================================================================================================================
// Does primitive export in NGG primitive shader.
void NggPrimShader::doPrimitiveExport(
    Value* cullFlag)       // [in] Cull flag indicating whether this primitive has been culled (could be null)
{
    const bool vertexCompact = (m_nggControl->compactMode == NggCompactVertices);

    Value* primData = nullptr;

    // Primitive data layout [31:0]
    //   [31]    = null primitive flag
    //   [28:20] = vertexId2 (in bytes)
    //   [18:10] = vertexId1 (in bytes)
    //   [8:0]   = vertexId0 (in bytes)

    if (m_nggControl->passthroughMode)
    {
        // Pass-through mode (primitive data has been constructed)
        primData = m_nggFactor.esGsOffsets01;
    }
    else
    {
        // Non pass-through mode (primitive data has to be constructed)
        auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_builder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.esGsOffsets01,
                                                            m_builder->getInt32(0),
                                                            m_builder->getInt32(16),
                                                        });
        Value* vertexId0 = m_builder->CreateLShr(esGsOffset0, 2);

        auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_builder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.esGsOffsets01,
                                                            m_builder->getInt32(16),
                                                            m_builder->getInt32(16),
                                                        });
        Value* vertexId1 = m_builder->CreateLShr(esGsOffset1, 2);

        auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                        m_builder->getInt32Ty(),
                                                        {
                                                            m_nggFactor.esGsOffsets23,
                                                            m_builder->getInt32(0),
                                                            m_builder->getInt32(16),
                                                        });
        Value* vertexId2 = m_builder->CreateLShr(esGsOffset2, 2);

        if (vertexCompact)
        {
            // NOTE: If the current vertex count in sub-group is less than the original value, then there must be
            // vertex culling. When vertex culling occurs, the vertex IDs should be fetched from LDS (compacted).
            auto vertCountInSubgroup = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                                    m_builder->getInt32Ty(),
                                                                    {
                                                                        m_nggFactor.mergedGroupInfo,
                                                                        m_builder->getInt32(12),
                                                                        m_builder->getInt32(9),
                                                                    });
            auto vertCulled = m_builder->CreateICmpULT(m_nggFactor.vertCountInSubgroup, vertCountInSubgroup);

            auto expPrimBlock = m_builder->GetInsertBlock();

            auto readCompactIdBlock = createBlock(expPrimBlock->getParent(), "readCompactId");
            readCompactIdBlock->moveAfter(expPrimBlock);

            auto expPrimContBlock = createBlock(expPrimBlock->getParent(), "expPrimCont");
            expPrimContBlock->moveAfter(readCompactIdBlock);

            m_builder->CreateCondBr(vertCulled, readCompactIdBlock, expPrimContBlock);

            // Construct ".readCompactId" block
            Value* compactVertexId0 = nullptr;
            Value* compactVertexId1 = nullptr;
            Value* compactVertexId2 = nullptr;
            {
                m_builder->SetInsertPoint(readCompactIdBlock);

                compactVertexId0 = readPerThreadDataFromLds(m_builder->getInt8Ty(),
                                                             vertexId0,
                                                             LdsRegionVertThreadIdMap);
                compactVertexId0 = m_builder->CreateZExt(compactVertexId0, m_builder->getInt32Ty());

                compactVertexId1 = readPerThreadDataFromLds(m_builder->getInt8Ty(),
                                                             vertexId1,
                                                             LdsRegionVertThreadIdMap);
                compactVertexId1 = m_builder->CreateZExt(compactVertexId1, m_builder->getInt32Ty());

                compactVertexId2 = readPerThreadDataFromLds(m_builder->getInt8Ty(),
                                                             vertexId2,
                                                             LdsRegionVertThreadIdMap);
                compactVertexId2 = m_builder->CreateZExt(compactVertexId2, m_builder->getInt32Ty());

                m_builder->CreateBr(expPrimContBlock);
            }

            // Construct part of ".expPrimCont" block (phi nodes)
            {
                m_builder->SetInsertPoint(expPrimContBlock);

                auto vertexId0Phi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
                vertexId0Phi->addIncoming(compactVertexId0, readCompactIdBlock);
                vertexId0Phi->addIncoming(vertexId0, expPrimBlock);

                auto vertexId1Phi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
                vertexId1Phi->addIncoming(compactVertexId1, readCompactIdBlock);
                vertexId1Phi->addIncoming(vertexId1, expPrimBlock);

                auto vertexId2Phi = m_builder->CreatePHI(m_builder->getInt32Ty(), 2);
                vertexId2Phi->addIncoming(compactVertexId2, readCompactIdBlock);
                vertexId2Phi->addIncoming(vertexId2, expPrimBlock);

                vertexId0 = vertexId0Phi;
                vertexId1 = vertexId1Phi;
                vertexId2 = vertexId2Phi;
            }
        }

        primData = m_builder->CreateShl(vertexId2, 10);
        primData = m_builder->CreateOr(primData, vertexId1);

        primData = m_builder->CreateShl(primData, 10);
        primData = m_builder->CreateOr(primData, vertexId0);

        if (vertexCompact)
        {
            assert(cullFlag != nullptr); // Must not be null
            const auto nullPrimVal = m_builder->getInt32(NullPrim);
            primData = m_builder->CreateSelect(cullFlag, nullPrimVal, primData);
        }
    }

    auto undef = UndefValue::get(m_builder->getInt32Ty());

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                m_builder->getInt32Ty(),
                                {
                                    m_builder->getInt32(EXP_TARGET_PRIM),      // tgt
                                    m_builder->getInt32(0x1),                  // en
                                    // src0 ~ src3
                                    primData,
                                    undef,
                                    undef,
                                    undef,
                                    m_builder->getTrue(),                      // done, must be set
                                    m_builder->getFalse(),                     // vm
                                });
}

// =====================================================================================================================
// Early exit NGG primitive shader when we detect that the entire sub-group is fully culled, doing dummy
// primitive/vertex export if necessary.
void NggPrimShader::doEarlyExit(
    unsigned  fullyCulledThreadCount,   // Thread count left when the entire sub-group is fully culled
    unsigned  expPosCount)              // Position export count
{
    if (fullyCulledThreadCount > 0)
    {
        assert(fullyCulledThreadCount == 1); // Currently, if workarounded, this is set to 1

        auto earlyExitBlock = m_builder->GetInsertBlock();

        auto dummyExpBlock = createBlock(earlyExitBlock->getParent(), ".dummyExp");
        dummyExpBlock->moveAfter(earlyExitBlock);

        auto endDummyExpBlock = createBlock(earlyExitBlock->getParent(), ".endDummyExp");
        endDummyExpBlock->moveAfter(dummyExpBlock);

        // Continue to construct ".earlyExit" block
        {
            auto firstThreadInSubgroup =
                m_builder->CreateICmpEQ(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(0));
            m_builder->CreateCondBr(firstThreadInSubgroup, dummyExpBlock, endDummyExpBlock);
        }

        // Construct ".dummyExp" block
        {
            m_builder->SetInsertPoint(dummyExpBlock);

            auto undef = UndefValue::get(m_builder->getInt32Ty());

            m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                        m_builder->getInt32Ty(),
                                        {
                                            m_builder->getInt32(EXP_TARGET_PRIM),          // tgt
                                            m_builder->getInt32(0x1),                      // en
                                            // src0 ~ src3
                                            m_builder->getInt32(0),
                                            undef,
                                            undef,
                                            undef,
                                            m_builder->getTrue(),                          // done
                                            m_builder->getFalse()                          // vm
                                        });

            undef = UndefValue::get(m_builder->getFloatTy());

            for (unsigned i = 0; i < expPosCount; ++i)
            {
                m_builder->CreateIntrinsic(Intrinsic::amdgcn_exp,
                                            m_builder->getFloatTy(),
                                            {
                                                m_builder->getInt32(EXP_TARGET_POS_0 + i), // tgt
                                                m_builder->getInt32(0x0),                  // en
                                                // src0 ~ src3
                                                undef,
                                                undef,
                                                undef,
                                                undef,
                                                m_builder->getInt1(i == expPosCount - 1),  // done
                                                m_builder->getFalse()                      // vm
                                            });
            }

            m_builder->CreateBr(endDummyExpBlock);
        }

        // Construct ".endDummyExp" block
        {
            m_builder->SetInsertPoint(endDummyExpBlock);
            m_builder->CreateRetVoid();
        }
    }
    else
        m_builder->CreateRetVoid();
}

// =====================================================================================================================
// Runs ES or ES variant (to get exported data).
//
// NOTE: The ES variant is derived from original ES main function with some additional special handling added to the
// function body and also mutates its return type.
void NggPrimShader::runEsOrEsVariant(
    Module*               module,          // [in] LLVM module
    StringRef             entryName,        // ES entry name
    Argument*             sysValueStart,   // Start of system value
    bool                  sysValueFromLds,  // Whether some system values are loaded from LDS (for vertex compaction)
    std::vector<ExpData>* expDataSet,      // [out] Set of exported data (could be null)
    BasicBlock*           insertAtEnd)     // [in] Where to insert instructions
{
    const bool hasTs = (m_hasTcs || m_hasTes);
    if (((hasTs && m_hasTes) || ((hasTs == false) && m_hasVs)) == false)
    {
        // No TES (tessellation is enabled) or VS (tessellation is disabled), don't have to run
        return;
    }

    const bool runEsVariant = (entryName != lgcName::NggEsEntryPoint);

    Function* esEntry = nullptr;
    if (runEsVariant)
    {
        assert(expDataSet != nullptr);
        esEntry = mutateEsToVariant(module, entryName, *expDataSet); // Mutate ES to variant

        if (esEntry == nullptr)
        {
            // ES variant is NULL, don't have to run
            return;
        }
    }
    else
    {
        esEntry = module->getFunction(lgcName::NggEsEntryPoint);
        assert(esEntry != nullptr);
    }

    // Call ES entry
    Argument* arg = sysValueStart;

    Value* esGsOffset = nullptr;
    if (m_hasGs)
    {
        auto& calcFactor = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.calcFactor;
        esGsOffset = m_builder->CreateMul(m_nggFactor.waveIdInSubgroup,
                                            m_builder->getInt32(64 * 4 * calcFactor.esGsRingItemSize));
    }

    Value* offChipLdsBase = (arg + EsGsSysValueOffChipLdsBase);
    Value* isOffChip = UndefValue::get(m_builder->getInt32Ty()); // NOTE: This flag is unused.

    arg += EsGsSpecialSysValueCount;

    Value* userData = arg++;

    // Initialize those system values to undefined ones
    Value* tessCoordX    = UndefValue::get(m_builder->getFloatTy());
    Value* tessCoordY    = UndefValue::get(m_builder->getFloatTy());
    Value* relPatchId    = UndefValue::get(m_builder->getInt32Ty());
    Value* patchId       = UndefValue::get(m_builder->getInt32Ty());

    Value* vertexId      = UndefValue::get(m_builder->getInt32Ty());
    Value* relVertexId   = UndefValue::get(m_builder->getInt32Ty());
    Value* vsPrimitiveId = UndefValue::get(m_builder->getInt32Ty());
    Value* instanceId    = UndefValue::get(m_builder->getInt32Ty());

    if (sysValueFromLds)
    {
        // NOTE: For vertex compaction, system values are from LDS compaction data region rather than from VGPRs.
        assert(m_nggControl->compactMode == NggCompactVertices);

        const auto resUsage = m_pipelineState->getShaderResourceUsage(hasTs ? ShaderStageTessEval : ShaderStageVertex);

        if (hasTs)
        {
            if (resUsage->builtInUsage.tes.tessCoord)
            {
                tessCoordX = readPerThreadDataFromLds(m_builder->getFloatTy(),
                                                       m_nggFactor.threadIdInSubgroup,
                                                       LdsRegionCompactTessCoordX);

                tessCoordY = readPerThreadDataFromLds(m_builder->getFloatTy(),
                                                       m_nggFactor.threadIdInSubgroup,
                                                       LdsRegionCompactTessCoordY);
            }

            relPatchId = readPerThreadDataFromLds(m_builder->getInt32Ty(),
                                                   m_nggFactor.threadIdInSubgroup,
                                                   LdsRegionCompactRelPatchId);

            if (resUsage->builtInUsage.tes.primitiveId)
            {
                patchId = readPerThreadDataFromLds(m_builder->getInt32Ty(),
                                                    m_nggFactor.threadIdInSubgroup,
                                                    LdsRegionCompactPatchId);
            }
        }
        else
        {
            if (resUsage->builtInUsage.vs.vertexIndex)
            {
                vertexId = readPerThreadDataFromLds(m_builder->getInt32Ty(),
                                                     m_nggFactor.threadIdInSubgroup,
                                                     LdsRegionCompactVertexId);
            }

            // NOTE: Relative vertex ID Will not be used when VS is merged to GS.

            if (resUsage->builtInUsage.vs.primitiveId)
            {
                vsPrimitiveId = readPerThreadDataFromLds(m_builder->getInt32Ty(),
                                                          m_nggFactor.threadIdInSubgroup,
                                                          LdsRegionCompactPrimId);
            }

            if (resUsage->builtInUsage.vs.instanceIndex)
            {
                instanceId = readPerThreadDataFromLds(m_builder->getInt32Ty(),
                                                       m_nggFactor.threadIdInSubgroup,
                                                       LdsRegionCompactInstanceId);
            }
        }
    }
    else
    {
        tessCoordX    = (arg + 5);
        tessCoordY    = (arg + 6);
        relPatchId    = (arg + 7);
        patchId       = (arg + 8);

        vertexId      = (arg + 5);
        relVertexId   = (arg + 6);
        // NOTE: VS primitive ID for NGG is specially obtained, not simply from system VGPR.
        if (m_nggFactor.primitiveId != nullptr)
            vsPrimitiveId = m_nggFactor.primitiveId;
        instanceId    = (arg + 8);
    }

    std::vector<Value*> args;

    auto intfData =
        m_pipelineState->getShaderInterfaceData(hasTs ? ShaderStageTessEval : ShaderStageVertex);
    const unsigned userDataCount = intfData->userDataCount;

    unsigned userDataIdx = 0;

    auto esArgBegin = esEntry->arg_begin();
    const unsigned esArgCount = esEntry->arg_size();
    (void(esArgCount)); // unused

    // Set up user data SGPRs
    while (userDataIdx < userDataCount)
    {
        assert(args.size() < esArgCount);

        auto esArg = (esArgBegin + args.size());
        assert(esArg->hasAttribute(Attribute::InReg));

        auto esArgTy = esArg->getType();
        if (esArgTy->isVectorTy())
        {
            assert(esArgTy->getVectorElementType()->isIntegerTy());

            const unsigned userDataSize = esArgTy->getVectorNumElements();

            std::vector<unsigned> shuffleMask;
            for (unsigned i = 0; i < userDataSize; ++i)
                shuffleMask.push_back(userDataIdx + i);

            userDataIdx += userDataSize;

            auto esUserData = m_builder->CreateShuffleVector(userData, userData, shuffleMask);
            args.push_back(esUserData);
        }
        else
        {
            assert(esArgTy->isIntegerTy());

            auto esUserData = m_builder->CreateExtractElement(userData, userDataIdx);
            args.push_back(esUserData);
            ++userDataIdx;
        }
    }

    if (hasTs)
    {
        // Set up system value SGPRs
        if (m_pipelineState->isTessOffChip())
        {
            args.push_back(m_hasGs ? offChipLdsBase : isOffChip);
            args.push_back(m_hasGs ? isOffChip : offChipLdsBase);
        }

        if (m_hasGs)
            args.push_back(esGsOffset);

        // Set up system value VGPRs
        args.push_back(tessCoordX);
        args.push_back(tessCoordY);
        args.push_back(relPatchId);
        args.push_back(patchId);
    }
    else
    {
        // Set up system value SGPRs
        if (m_hasGs)
            args.push_back(esGsOffset);

        // Set up system value VGPRs
        args.push_back(vertexId);
        args.push_back(relVertexId);
        args.push_back(vsPrimitiveId);
        args.push_back(instanceId);
    }

    assert(args.size() == esArgCount); // Must have visit all arguments of ES entry point

    if (runEsVariant)
    {
        auto expData = emitCall(entryName,
                                 esEntry->getReturnType(),
                                 args,
                                 {},
                                 insertAtEnd);

        // Re-construct exported data from the return value
        auto expDataTy = expData->getType();
        assert(expDataTy->isArrayTy());

        const unsigned expCount = expDataTy->getArrayNumElements();
        for (unsigned i = 0; i < expCount; ++i)
        {
            Value* expValue = m_builder->CreateExtractValue(expData, i);
            (*expDataSet)[i].expValue = expValue;
        }
    }
    else
    {
        emitCall(entryName,
                 esEntry->getReturnType(),
                 args,
                 {},
                 insertAtEnd);
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
Function* NggPrimShader::mutateEsToVariant(
    Module*               module,          // [in] LLVM module
    StringRef             entryName,        // ES entry name
    std::vector<ExpData>& expDataSet)       // [out] Set of exported data
{
    assert(m_hasGs == false); // GS must not be present
    assert(expDataSet.empty());

    const auto esEntryPoint = module->getFunction(lgcName::NggEsEntryPoint);
    assert(esEntryPoint != nullptr);

    const bool doExp      = (entryName == lgcName::NggEsEntryVariant);
    const bool doPosExp   = (entryName == lgcName::NggEsEntryVariantPos);
    const bool doParamExp = (entryName == lgcName::NggEsEntryVariantParam);

    // Calculate export count
    unsigned expCount = 0;

    for (auto& func : module->functions())
    {
        if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_exp))
        {
            for (auto user : func.users())
            {
                CallInst* const call = dyn_cast<CallInst>(user);
                assert(call != nullptr);

                if (call->getParent()->getParent() != esEntryPoint)
                {
                    // Export call doesn't belong to ES, skip
                    continue;
                }

                uint8_t expTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();

                bool expPos = ((expTarget >= EXP_TARGET_POS_0) && (expTarget <= EXP_TARGET_POS_4));
                bool expParam = ((expTarget >= EXP_TARGET_PARAM_0) && (expTarget <= EXP_TARGET_PARAM_31));

                if ((doExp && (expPos || expParam)) ||
                    (doPosExp && expPos)            ||
                    (doParamExp && expParam))
                    ++expCount;
            }
        }
    }

    if (expCount == 0)
    {
        // If the export count is zero, return NULL
        return nullptr;
    }

    // Clone new entry-point
    auto expDataTy = ArrayType::get(VectorType::get(Type::getFloatTy(*m_context), 4), expCount);
    Value* expData = UndefValue::get(expDataTy);

    auto esEntryVariantTy = FunctionType::get(expDataTy, esEntryPoint->getFunctionType()->params(), false);
    auto esEntryVariant = Function::Create(esEntryVariantTy, esEntryPoint->getLinkage(), "", module);
    esEntryVariant->copyAttributesFrom(esEntryPoint);

    ValueToValueMapTy valueMap;

    Argument* variantArg = esEntryVariant->arg_begin();
    for (Argument &arg : esEntryPoint->args())
        valueMap[&arg] = variantArg++;

    SmallVector<ReturnInst*, 8> retInsts;
    CloneFunctionInto(esEntryVariant, esEntryPoint, valueMap, false, retInsts);

    esEntryVariant->setName(entryName);

    auto savedInsertPos = m_builder->saveIP();

    // Find the return block and remove old return instruction
    BasicBlock* retBlock = nullptr;
    for (BasicBlock& block : *esEntryVariant)
    {
        auto retInst = dyn_cast<ReturnInst>(block.getTerminator());
        if (retInst != nullptr)
        {
            retInst->dropAllReferences();
            retInst->eraseFromParent();

            retBlock = &block;
            break;
        }
    }

    m_builder->SetInsertPoint(retBlock);

    // Get exported data
    std::vector<Instruction*> expCalls;

    unsigned lastExport = InvalidValue; // Record last position export that needs "done" flag
    for (auto& func : module->functions())
    {
        if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_exp))
        {
            for (auto user : func.users())
            {
                CallInst* const call = dyn_cast<CallInst>(user);
                assert(call != nullptr);

                if (call->getParent()->getParent() != esEntryVariant)
                {
                    // Export call doesn't belong to ES variant, skip
                    continue;
                }

                assert(call->getParent() == retBlock); // Must in return block

                uint8_t expTarget = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();

                bool expPos = ((expTarget >= EXP_TARGET_POS_0) && (expTarget <= EXP_TARGET_POS_4));
                bool expParam = ((expTarget >= EXP_TARGET_PARAM_0) && (expTarget <= EXP_TARGET_PARAM_31));

                if ((doExp && (expPos || expParam)) ||
                    (doPosExp && expPos) ||
                    (doParamExp && expParam))
                {
                    uint8_t channelMask = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();

                    Value* expValues[4] = {};
                    expValues[0] = call->getArgOperand(2);
                    expValues[1] = call->getArgOperand(3);
                    expValues[2] = call->getArgOperand(4);
                    expValues[3] = call->getArgOperand(5);

                    if (func.getName().endswith(".i32"))
                    {
                        expValues[0] = m_builder->CreateBitCast(expValues[0], m_builder->getFloatTy());
                        expValues[1] = m_builder->CreateBitCast(expValues[1], m_builder->getFloatTy());
                        expValues[2] = m_builder->CreateBitCast(expValues[2], m_builder->getFloatTy());
                        expValues[3] = m_builder->CreateBitCast(expValues[3], m_builder->getFloatTy());
                    }

                    Value* expValue = UndefValue::get(VectorType::get(Type::getFloatTy(*m_context), 4));
                    for (unsigned i = 0; i < 4; ++i)
                        expValue = m_builder->CreateInsertElement(expValue, expValues[i], i);

                    if (expPos)
                    {
                        // Last position export that needs "done" flag
                        lastExport = expDataSet.size();
                    }

                    ExpData expData = { expTarget, channelMask, false, expValue };
                    expDataSet.push_back(expData);
                }

                expCalls.push_back(call);
            }
        }
    }
    assert(expDataSet.size() == expCount);

    // Set "done" flag for last position export
    if (lastExport != InvalidValue)
        expDataSet[lastExport].doneFlag = true;

    // Construct exported data
    unsigned i = 0;
    for (auto& expDataElement : expDataSet)
    {
        expData = m_builder->CreateInsertValue(expData, expDataElement.expValue, i++);
        expDataElement.expValue = nullptr;
    }

    // Insert new "return" instruction
    m_builder->CreateRet(expData);

    // Clear export calls
    for (auto expCall : expCalls)
    {
        expCall->dropAllReferences();
        expCall->eraseFromParent();
    }

    m_builder->restoreIP(savedInsertPos);

    return esEntryVariant;
}

// =====================================================================================================================
// Runs GS variant.
//
// NOTE: The GS variant is derived from original GS main function with some additional special handling added to the
// function body and also mutates its return type.
Value* NggPrimShader::runGsVariant(
    Module*         module,        // [in] LLVM module
    Argument*       sysValueStart, // Start of system value
    BasicBlock*     insertAtEnd)   // [in] Where to insert instructions
{
    assert(m_hasGs); // GS must be present

    Function* gsEntry = mutateGsToVariant(module);

    // Call GS entry
    Argument* arg = sysValueStart;

    Value* gsVsOffset = UndefValue::get(m_builder->getInt32Ty()); // NOTE: For NGG, GS-VS offset is unused

    // NOTE: This argument is expected to be GS wave ID, not wave ID in sub-group, for normal ES-GS merged shader.
    // However, in NGG mode, GS wave ID, sent to GS_EMIT and GS_CUT messages, is no longer required because of NGG
    // handling of such messages. Instead, wave ID in sub-group is required as the substitue.
    auto waveId = m_nggFactor.waveIdInSubgroup;

    arg += EsGsSpecialSysValueCount;

    Value* userData = arg++;

    Value* esGsOffsets01 = arg;
    Value* esGsOffsets23 = (arg + 1);
    Value* gsPrimitiveId = (arg + 2);
    Value* invocationId  = (arg + 3);
    Value* esGsOffsets45 = (arg + 4);

    // NOTE: For NGG, GS invocation ID is stored in lowest 8 bits ([7:0]) and other higher bits are used for other
    // purposes according to GE-SPI interface.
    invocationId = m_builder->CreateAnd(invocationId, m_builder->getInt32(0xFF));

    auto esGsOffset0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        esGsOffsets01,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16)
                                                    });

    auto esGsOffset1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        esGsOffsets01,
                                                        m_builder->getInt32(16),
                                                        m_builder->getInt32(16)
                                                    });

    auto esGsOffset2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        esGsOffsets23,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16)
                                                    });

    auto esGsOffset3 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        esGsOffsets23,
                                                        m_builder->getInt32(16),
                                                        m_builder->getInt32(16)
                                                    });

    auto esGsOffset4 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        esGsOffsets45,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(16)
                                                    });

    auto esGsOffset5 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        esGsOffsets45,
                                                        m_builder->getInt32(16),
                                                        m_builder->getInt32(16)
                                                    });

    std::vector<Value*> args;

    auto intfData = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry);
    const unsigned userDataCount = intfData->userDataCount;

    unsigned userDataIdx = 0;

    auto gsArgBegin = gsEntry->arg_begin();
    const unsigned gsArgCount = gsEntry->arg_size();
    (void(gsArgCount)); // unused

    // Set up user data SGPRs
    while (userDataIdx < userDataCount)
    {
        assert(args.size() < gsArgCount);

        auto gsArg = (gsArgBegin + args.size());
        assert(gsArg->hasAttribute(Attribute::InReg));

        auto gsArgTy = gsArg->getType();
        if (gsArgTy->isVectorTy())
        {
            assert(gsArgTy->getVectorElementType()->isIntegerTy());

            const unsigned userDataSize = gsArgTy->getVectorNumElements();

            std::vector<unsigned> shuffleMask;
            for (unsigned i = 0; i < userDataSize; ++i)
                shuffleMask.push_back(userDataIdx + i);

            userDataIdx += userDataSize;

            auto gsUserData = m_builder->CreateShuffleVector(userData, userData, shuffleMask);
            args.push_back(gsUserData);
        }
        else
        {
            assert(gsArgTy->isIntegerTy());

            auto gsUserData = m_builder->CreateExtractElement(userData, userDataIdx);
            args.push_back(gsUserData);
            ++userDataIdx;
        }
    }

    // Set up system value SGPRs
    args.push_back(gsVsOffset);
    args.push_back(waveId);

    // Set up system value VGPRs
    args.push_back(esGsOffset0);
    args.push_back(esGsOffset1);
    args.push_back(gsPrimitiveId);
    args.push_back(esGsOffset2);
    args.push_back(esGsOffset3);
    args.push_back(esGsOffset4);
    args.push_back(esGsOffset5);
    args.push_back(invocationId);

    assert(args.size() == gsArgCount); // Must have visit all arguments of ES entry point

    return emitCall(lgcName::NggGsEntryVariant,
                    gsEntry->getReturnType(),
                    args,
                    {},
                    insertAtEnd);
}

// =====================================================================================================================
// Mutates the entry-point (".main") of GS to its variant (".variant").
//
// NOTE: Initially, the return type of GS entry-point is void. After this mutation, GS messages (GS_EMIT, GS_CUT) are
// handled by shader itself. Also, output primitive/vertex count info is calculated and is returned. The return type
// is something like this:
//   { OUT_PRIM_COUNT: i32, OUT_VERT_COUNT: i32, INCLUSIVE_OUT_VERT_COUNT: i32, OUT_VERT_COUNT_IN_WAVE: i32 }
Function* NggPrimShader::mutateGsToVariant(
    Module* module)          // [in] LLVM module
{
    assert(m_hasGs); // GS must be present

    auto gsEntryPoint = module->getFunction(lgcName::NggGsEntryPoint);
    assert(gsEntryPoint != nullptr);

    // Clone new entry-point
    auto resultTy = StructType::get(*m_context,
                                     {
                                         m_builder->getInt32Ty(), // outPrimCount
                                         m_builder->getInt32Ty(), // outVertCount
                                         m_builder->getInt32Ty(), // inclusiveOutVertCount
                                         m_builder->getInt32Ty()  // outVertCountInWave
                                     });
    auto gsEntryVariantTy = FunctionType::get(resultTy, gsEntryPoint->getFunctionType()->params(), false);
    auto gsEntryVariant = Function::Create(gsEntryVariantTy, gsEntryPoint->getLinkage(), "", module);
    gsEntryVariant->copyAttributesFrom(gsEntryPoint);

    ValueToValueMapTy valueMap;

    Argument* variantArg = gsEntryVariant->arg_begin();
    for (Argument &arg : gsEntryPoint->args())
        valueMap[&arg] = variantArg++;

    SmallVector<ReturnInst*, 8> retInsts;
    CloneFunctionInto(gsEntryVariant, gsEntryPoint, valueMap, false, retInsts);

    gsEntryVariant->setName(lgcName::NggGsEntryVariant);

    // Remove original GS entry-point
    gsEntryPoint->dropAllReferences();
    gsEntryPoint->eraseFromParent();
    gsEntryPoint = nullptr; // No longer available

    auto savedInsertPos = m_builder->saveIP();

    BasicBlock* retBlock = &gsEntryVariant->back();

    // Remove old "return" instruction
    assert(isa<ReturnInst>(retBlock->getTerminator()));
    ReturnInst* retInst = cast<ReturnInst>(retBlock->getTerminator());

    retInst->dropAllReferences();
    retInst->eraseFromParent();

    std::vector<Instruction*> removeCalls;

    m_builder->SetInsertPoint(&*gsEntryVariant->front().getFirstInsertionPt());

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
        auto emitCounterPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
        m_builder->CreateStore(m_builder->getInt32(0), emitCounterPtr); // emitCounter = 0
        emitCounterPtrs[i] = emitCounterPtr;

        auto outVertCounterPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
        m_builder->CreateStore(m_builder->getInt32(0), outVertCounterPtr); // outVertCounter = 0
        outVertCounterPtrs[i] = outVertCounterPtr;

        auto outPrimCounterPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
        m_builder->CreateStore(m_builder->getInt32(0), outPrimCounterPtr); // outPrimCounter = 0
        outPrimCounterPtrs[i] = outPrimCounterPtr;

        auto outstandingVertCounterPtr = m_builder->CreateAlloca(m_builder->getInt32Ty());
        m_builder->CreateStore(m_builder->getInt32(0), outstandingVertCounterPtr); // outstandingVertCounter = 0
        outstandingVertCounterPtrs[i] = outstandingVertCounterPtr;

        auto flipVertOrderPtr = m_builder->CreateAlloca(m_builder->getInt1Ty());
        m_builder->CreateStore(m_builder->getFalse(), flipVertOrderPtr); // flipVertOrder = false
        flipVertOrderPtrs[i] = flipVertOrderPtr;
    }

    // Initialize thread ID in wave
    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    auto threadIdInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                       {},
                                                       {
                                                           m_builder->getInt32(-1),
                                                           m_builder->getInt32(0)
                                                       });

    if (waveSize == 64)
    {
        threadIdInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                      {},
                                                      {
                                                          m_builder->getInt32(-1),
                                                          threadIdInWave
                                                      });
    }

    // Initialzie thread ID in subgroup
    auto& entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStageGeometry)->entryArgIdxs.gs;
    auto waveId = getFunctionArgument(gsEntryVariant, entryArgIdxs.waveId);

    auto threadIdInSubgroup = m_builder->CreateMul(waveId, m_builder->getInt32(waveSize));
    threadIdInSubgroup = m_builder->CreateAdd(threadIdInSubgroup, threadIdInWave);

    // Handle GS message and GS output export
    for (auto& func : module->functions())
    {
        if (func.getName().startswith(lgcName::NggGsOutputExport))
        {
            // Export GS outputs to GS-VS ring
            for (auto user : func.users())
            {
                CallInst* const call = dyn_cast<CallInst>(user);
                assert(call != nullptr);
                m_builder->SetInsertPoint(call);

                assert(call->getNumArgOperands() == 4);
                const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
                const unsigned compIdx = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
                const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
                assert(streamId < MaxGsStreams);
                Value* output = call->getOperand(3);

                auto outVertCounter = m_builder->CreateLoad(outVertCounterPtrs[streamId]);
                exportGsOutput(output, location, compIdx, streamId, threadIdInSubgroup, outVertCounter);

                removeCalls.push_back(call);
            }
        }
        else if (func.isIntrinsic() && (func.getIntrinsicID() == Intrinsic::amdgcn_s_sendmsg))
        {
            // Handle GS message
            for (auto user : func.users())
            {
                CallInst* const call = dyn_cast<CallInst>(user);
                assert(call != nullptr);
                m_builder->SetInsertPoint(call);

                uint64_t message = cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
                if ((message == GsEmitStreaM0) || (message == GsEmitStreaM1) ||
                    (message == GsEmitStreaM2) || (message == GsEmitStreaM3))
                {
                    // Handle GS_EMIT, MSG[9:8] = STREAM_ID
                    unsigned streamId = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
                    assert(streamId < MaxGsStreams);
                    processGsEmit(module,
                                 streamId,
                                 threadIdInSubgroup,
                                 emitCounterPtrs[streamId],
                                 outVertCounterPtrs[streamId],
                                 outPrimCounterPtrs[streamId],
                                 outstandingVertCounterPtrs[streamId],
                                 flipVertOrderPtrs[streamId]);
                }
                else if ((message == GsCutStreaM0) || (message == GsCutStreaM1) ||
                         (message == GsCutStreaM2) || (message == GsCutStreaM3))
                {
                    // Handle GS_CUT, MSG[9:8] = STREAM_ID
                    unsigned streamId = (message & GsEmitCutStreamIdMask) >> GsEmitCutStreamIdShift;
                    assert(streamId < MaxGsStreams);
                    processGsCut(module,
                                 streamId,
                                 threadIdInSubgroup,
                                 emitCounterPtrs[streamId],
                                 outVertCounterPtrs[streamId],
                                 outPrimCounterPtrs[streamId],
                                 outstandingVertCounterPtrs[streamId],
                                 flipVertOrderPtrs[streamId]);
                }
                else if (message == GsDone)
                {
                    // Handle GS_DONE, do nothing (just remove this call)
                }
                else
                {
                    // Unexpected GS message
                    llvm_unreachable("Should never be called!");
                }

                removeCalls.push_back(call);
            }
        }
    }

    // Add additional processing in return block
    m_builder->SetInsertPoint(retBlock);

    // NOTE: Only return output primitive/vertex count info for rasterization stream.
    auto rasterStream = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry)->inOutUsage.gs.rasterStream;
    auto outPrimCount = m_builder->CreateLoad(outPrimCounterPtrs[rasterStream]);
    auto outVertCount = m_builder->CreateLoad(outVertCounterPtrs[rasterStream]);

    Value* outVertCountInWave = nullptr;
    auto inclusiveOutVertCount = doSubgroupInclusiveAdd(outVertCount, &outVertCountInWave);

    // NOTE: We use the highest thread (MSB) to get GS output vertex count in this wave (after inclusive-add,
    // the value of this thread stores this info)
    outVertCountInWave = m_builder->CreateIntrinsic(Intrinsic::amdgcn_readlane,
                                                      {},
                                                      {
                                                          outVertCountInWave,
                                                          m_builder->getInt32(waveSize - 1)
                                                      });

    Value* result = UndefValue::get(resultTy);
    result = m_builder->CreateInsertValue(result, outPrimCount, 0);
    result = m_builder->CreateInsertValue(result, outVertCount, 1);
    result = m_builder->CreateInsertValue(result, inclusiveOutVertCount, 2);
    result = m_builder->CreateInsertValue(result, outVertCountInWave, 3);

    m_builder->CreateRet(result); // Insert new "return" instruction

    // Clear removed calls
    for (auto call : removeCalls)
    {
        call->dropAllReferences();
        call->eraseFromParent();
    }

    m_builder->restoreIP(savedInsertPos);

    return gsEntryVariant;
}

// =====================================================================================================================
// Runs copy shader.
void NggPrimShader::runCopyShader(
    Module*     module,        // [in] LLVM module
    BasicBlock* insertAtEnd)   // [in] Where to insert instructions
{
    assert(m_hasGs); // GS must be present

    auto copyShaderEntryPoint = module->getFunction(lgcName::NggCopyShaderEntryPoint);

    // Mutate copy shader entry-point, handle GS output import
    {
        auto vertexOffset = getFunctionArgument(copyShaderEntryPoint, CopyShaderUserSgprIdxVertexOffset);

        auto savedInsertPos = m_builder->saveIP();

        std::vector<Instruction*> removeCalls;

        for (auto& func : module->functions())
        {
            if (func.getName().startswith(lgcName::NggGsOutputImport))
            {
                // Import GS outputs from GS-VS ring
                for (auto user : func.users())
                {
                    CallInst* const call = dyn_cast<CallInst>(user);
                    assert(call != nullptr);
                    m_builder->SetInsertPoint(call);

                    assert(call->getNumArgOperands() == 3);
                    const unsigned location = cast<ConstantInt>(call->getOperand(0))->getZExtValue();
                    const unsigned compIdx = cast<ConstantInt>(call->getOperand(1))->getZExtValue();
                    const unsigned streamId = cast<ConstantInt>(call->getOperand(2))->getZExtValue();
                    assert(streamId < MaxGsStreams);

                    auto output = importGsOutput(call->getType(), location, compIdx, streamId, vertexOffset);

                    call->replaceAllUsesWith(output);
                    removeCalls.push_back(call);
                }
            }
        }

        // Clear removed calls
        for (auto call : removeCalls)
        {
            call->dropAllReferences();
            call->eraseFromParent();
        }

        m_builder->restoreIP(savedInsertPos);
    }

    // Run copy shader
    {
        std::vector<Value*> args;

        static const unsigned CopyShaderSysValueCount = 11; // Fixed layout: 10 SGPRs, 1 VGPR
        for (unsigned i = 0; i < CopyShaderSysValueCount; ++i)
        {
            if (i == CopyShaderUserSgprIdxVertexOffset)
            {
                unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutVertOffset);

                auto ldsOffset = m_builder->CreateShl(m_nggFactor.threadIdInSubgroup, 2);
                ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));
                auto vertexOffset = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);
                args.push_back(vertexOffset);
            }
            else
            {
                // All SGPRs are not used
                args.push_back(UndefValue::get(getFunctionArgument(copyShaderEntryPoint, i)->getType()));
            }
        }

        emitCall(lgcName::NggCopyShaderEntryPoint,
                 m_builder->getVoidTy(),
                 args,
                 {},
                 insertAtEnd);
    }
}

// =====================================================================================================================
// Exports outputs of geometry shader to GS-VS ring.
void NggPrimShader::exportGsOutput(
    Value*       output,               // [in] Output value
    unsigned     location,              // Location of the output
    unsigned     compIdx,               // Index used for vector element indexing
    unsigned     streamId,              // ID of output vertex stream
    llvm::Value* threadIdInSubgroup,   // [in] Thread ID in sub-group
    Value*       outVertCounter)       // [in] GS output vertex counter for this stream
{
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    if (resUsage->inOutUsage.gs.rasterStream != streamId)
    {
        // NOTE: Only export those outputs that belong to the rasterization stream.
        assert(resUsage->inOutUsage.enableXfb == false); // Transform feedback must be disabled
        return;
    }

    // NOTE: We only handle LDS vector/scalar writing, so change [n x Ty] to <n x Ty> for array.
    auto outputTy = output->getType();
    if (outputTy->isArrayTy())
    {
        auto outputElemTy = outputTy->getArrayElementType();
        assert(outputElemTy->isSingleValueType());

        // [n x Ty] -> <n x Ty>
        const unsigned elemCount = outputTy->getArrayNumElements();
        Value* outputVec = UndefValue::get(VectorType::get(outputElemTy, elemCount));
        for (unsigned i = 0; i < elemCount; ++i)
        {
            auto outputElem = m_builder->CreateExtractValue(output, i);
            m_builder->CreateInsertElement(outputVec, outputElem, i);
        }

        outputTy = outputVec->getType();
        output = outputVec;
    }

    const unsigned bitWidth = output->getType()->getScalarSizeInBits();
    if ((bitWidth == 8) || (bitWidth == 16))
    {
        // NOTE: Currently, to simplify the design of load/store data from GS-VS ring, we always extend BYTE/WORD
        // to DWORD. This is because copy shader does not know the actual data type. It only generates output
        // export calls based on number of DWORDs.
        if (outputTy->isFPOrFPVectorTy())
        {
            assert(bitWidth == 16);
            Type* castTy = m_builder->getInt16Ty();
            if (outputTy->isVectorTy())
                castTy = VectorType::get(m_builder->getInt16Ty(), outputTy->getVectorNumElements());
            output = m_builder->CreateBitCast(output, castTy);
        }

        Type* extTy = m_builder->getInt32Ty();
        if (outputTy->isVectorTy())
            extTy = VectorType::get(m_builder->getInt32Ty(), outputTy->getVectorNumElements());
        output = m_builder->CreateZExt(output, extTy);
    }
    else
        assert((bitWidth == 32) || (bitWidth == 64));

    // gsVsRingOffset = threadIdInSubgroup * gsVsRingItemSize +
    //                  outVertcounter * vertexSize +
    //                  location * 4 + compIdx (in DWORDS)
    const unsigned gsVsRingItemSize = resUsage->inOutUsage.gs.calcFactor.gsVsRingItemSize;
    Value* gsVsRingOffset = m_builder->CreateMul(threadIdInSubgroup, m_builder->getInt32(gsVsRingItemSize));

    const unsigned vertexSize = resUsage->inOutUsage.gs.outLocCount[streamId] * 4;
    auto vertexItemOffset = m_builder->CreateMul(outVertCounter, m_builder->getInt32(vertexSize));

    gsVsRingOffset = m_builder->CreateAdd(gsVsRingOffset, vertexItemOffset);

    const unsigned attribOffset = (location * 4) + compIdx;
    gsVsRingOffset = m_builder->CreateAdd(gsVsRingOffset, m_builder->getInt32(attribOffset));

    // ldsOffset = gsVsRingStart + gsVsRingOffset * 4 (in BYTES)
    const unsigned gsVsRingStart = m_ldsManager->getLdsRegionStart(LdsRegionGsVsRing);

    auto ldsOffset = m_builder->CreateShl(gsVsRingOffset, 2);
    ldsOffset = m_builder->CreateAdd(m_builder->getInt32(gsVsRingStart), ldsOffset);

    m_ldsManager->writeValueToLds(output, ldsOffset);
}

// =====================================================================================================================
// Imports outputs of geometry shader from GS-VS ring.
Value* NggPrimShader::importGsOutput(
    Type*        outputTy,             // [in] Type of the output
    unsigned     location,              // Location of the output
    unsigned     compIdx,               // Index used for vector element indexing
    unsigned     streamId,              // ID of output vertex stream
    Value*       vertexOffset)         // [in] Start offset of vertex item in GS-VS ring (in BYTES)
{
    auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);
    if (resUsage->inOutUsage.gs.rasterStream != streamId)
    {
        // NOTE: Only import those outputs that belong to the rasterization stream.
        assert(resUsage->inOutUsage.enableXfb == false); // Transform feedback must be disabled
        return UndefValue::get(outputTy);
    }

    // NOTE: We only handle LDS vector/scalar reading, so change [n x Ty] to <n x Ty> for array.
    auto origOutputTy = outputTy;
    if (outputTy->isArrayTy())
    {
        auto outputElemTy = outputTy->getArrayElementType();
        assert(outputElemTy->isSingleValueType());

        // [n x Ty] -> <n x Ty>
        const unsigned elemCount = outputTy->getArrayNumElements();
        outputTy = VectorType::get(outputElemTy, elemCount);
    }

    // ldsOffset = vertexOffset + (location * 4 + compIdx) * 4 (in BYTES)
    const unsigned attribOffset = (location * 4) + compIdx;
    auto ldsOffset = m_builder->CreateAdd(vertexOffset, m_builder->getInt32(attribOffset * 4));
    // Use 128-bit LDS load
    auto output = m_ldsManager->readValueFromLds(
        outputTy, ldsOffset, (outputTy->getPrimitiveSizeInBits() == 128));

    if (origOutputTy != outputTy)
    {
        assert(origOutputTy->isArrayTy() && outputTy->isVectorTy() &&
                    (origOutputTy->getArrayNumElements() == outputTy->getVectorNumElements()));

        // <n x Ty> -> [n x Ty]
        const unsigned elemCount = origOutputTy->getArrayNumElements();
        Value* outputArray = UndefValue::get(origOutputTy);
        for (unsigned i = 0; i < elemCount; ++i)
        {
            auto outputElem = m_builder->CreateExtractElement(output, i);
            outputArray = m_builder->CreateInsertValue(outputArray, outputElem, i);
        }

        output = outputArray;
    }

    return output;
}

// =====================================================================================================================
// Processes the message GS_EMIT.
void NggPrimShader::processGsEmit(
    Module*  module,                       // [in] LLVM module
    unsigned streamId,                      // ID of output vertex stream
    Value*   threadIdInSubgroup,           // [in] Thread ID in subgroup
    Value*   emitCounterPtr,               // [in,out] Pointer to GS emit counter for this stream
    Value*   outVertCounterPtr,            // [in,out] Pointer to GS output vertex counter for this stream
    Value*   outPrimCounterPtr,            // [in,out] Pointer to GS output primitive counter for this stream
    Value*   outstandingVertCounterPtr,    // [in,out] Pointer to GS outstanding vertex counter for this stream
    Value*   flipVertOrderPtr)             // [in,out] Pointer to flags indicating whether to flip vertex ordering
{
    auto gsEmitHandler = module->getFunction(lgcName::NggGsEmit);
    if (gsEmitHandler == nullptr)
        gsEmitHandler = createGsEmitHandler(module, streamId);

    m_builder->CreateCall(gsEmitHandler,
                           {
                               threadIdInSubgroup,
                               emitCounterPtr,
                               outVertCounterPtr,
                               outPrimCounterPtr,
                               outstandingVertCounterPtr,
                               flipVertOrderPtr
                           });
}

// =====================================================================================================================
// Processes the message GS_CUT.
void NggPrimShader::processGsCut(
    Module*  module,                       // [in] LLVM module
    unsigned streamId,                      // ID of output vertex stream
    Value*   threadIdInSubgroup,           // [in] Thread ID in subgroup
    Value*   emitCounterPtr,               // [in,out] Pointer to GS emit counter for this stream
    Value*   outVertCounterPtr,            // [in,out] Pointer to GS output vertex counter for this stream
    Value*   outPrimCounterPtr,            // [in,out] Pointer to GS output primitive counter for this stream
    Value*   outstandingVertCounterPtr,    // [in,out] Pointer to GS outstanding vertex counter for this stream
    Value*   flipVertOrderPtr)             // [in,out] Pointer to flags indicating whether to flip vertex ordering
{
    auto gsCutHandler = module->getFunction(lgcName::NggGsCut);
    if (gsCutHandler == nullptr)
        gsCutHandler = createGsCutHandler(module, streamId);

    m_builder->CreateCall(gsCutHandler,
                           {
                               threadIdInSubgroup,
                               emitCounterPtr,
                               outVertCounterPtr,
                               outPrimCounterPtr,
                               outstandingVertCounterPtr,
                               flipVertOrderPtr
                           });
}

// =====================================================================================================================
// Creates the function that processes GS_EMIT.
Function* NggPrimShader::createGsEmitHandler(
    Module*     module,    // [in] LLVM module
    unsigned    streamId)   // ID of output vertex stream
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
    const auto addrSpace = module->getDataLayout().getAllocaAddrSpace();
    auto funcTy =
        FunctionType::get(m_builder->getVoidTy(),
                          {
                              m_builder->getInt32Ty(),                                // %threadIdInSubgroup
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %emitCounterPtr
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %outVertCounterPtr
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %outPrimCounterPtr
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %outstandingVertCounterPtr
                              PointerType::get(m_builder->getInt1Ty(),  addrSpace),   // %flipVertOrderPtr
                          },
                          false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggGsEmit, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* threadIdInSubgroup = argIt++;
    threadIdInSubgroup->setName("threadIdInSubgroup");

    Value* emitCounterPtr = argIt++;
    emitCounterPtr->setName("emitCounterPtr");

    Value* outVertCounterPtr = argIt++;
    outVertCounterPtr->setName("outVertCounterPtr");

    Value* outPrimCounterPtr = argIt++;
    outPrimCounterPtr->setName("outPrimCounterPtr");

    Value* outstandingVertCounterPtr = argIt++;
    outstandingVertCounterPtr->setName("outstandingVertCounterPtr");

    Value* flipVertOrderPtr = argIt++; // Used by triangle strip
    flipVertOrderPtr->setName("flipVertOrderPtr");

    auto entryBlock = createBlock(func, ".entry");
    auto emitPrimBlock = createBlock(func, ".emitPrim");
    auto endEmitPrimBlock = createBlock(func, ".endEmitPrim");

    auto savedInsertPoint = m_builder->saveIP();

    const auto& geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    const auto& resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

    // Get GS output vertices per output primitive
    unsigned outVertsPerPrim = 0;
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
    auto outVertsPerPrimVal = m_builder->getInt32(outVertsPerPrim);

    // Construct ".entry" block
    Value* emitCounter = nullptr;
    Value* outVertCounter = nullptr;
    Value* outPrimCounter = nullptr;
    Value* outstandingVertCounter = nullptr;
    Value* flipVertOrder = nullptr;
    Value* primComplete = nullptr;
    {
        m_builder->SetInsertPoint(entryBlock);

        emitCounter = m_builder->CreateLoad(emitCounterPtr);
        outVertCounter = m_builder->CreateLoad(outVertCounterPtr);
        outPrimCounter = m_builder->CreateLoad(outPrimCounterPtr);
        outstandingVertCounter = m_builder->CreateLoad(outstandingVertCounterPtr);

        // Flip vertex ordering only for triangle strip
        if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
            flipVertOrder = m_builder->CreateLoad(flipVertOrderPtr);

        // emitCounter++
        emitCounter = m_builder->CreateAdd(emitCounter, m_builder->getInt32(1));

        // outVertCounter++
        outVertCounter = m_builder->CreateAdd(outVertCounter, m_builder->getInt32(1));

        // outstandingVertCounter++
        outstandingVertCounter = m_builder->CreateAdd(outstandingVertCounter, m_builder->getInt32(1));

        // primComplete = (emitCounter == outVertsPerPrim)
        primComplete = m_builder->CreateICmpEQ(emitCounter, outVertsPerPrimVal);
        m_builder->CreateCondBr(primComplete, emitPrimBlock, endEmitPrimBlock);
    }

    // Construct ".emitPrim" block
    {
        m_builder->SetInsertPoint(emitPrimBlock);

        // NOTE: Only calculate GS output primitive data and write it to LDS for rasterization stream.
        if (streamId == resUsage->inOutUsage.gs.rasterStream)
        {
            // vertexId = outVertCounter
            auto pvertexId = outVertCounter;

            // vertexId0 = vertexId - outVertsPerPrim
            auto vertexId0 = m_builder->CreateSub(pvertexId, outVertsPerPrimVal);

            // vertexId1 = vertexId - (outVertsPerPrim - 1) = vertexId0 + 1
            Value* vertexId1 = nullptr;
            if (outVertsPerPrim > 1)
                vertexId1 = m_builder->CreateAdd(vertexId0, m_builder->getInt32(1));

            // vertexId2 = vertexId - (outVertsPerPrim - 2) = vertexId0 + 2
            Value* vertexId2 = nullptr;
            if (outVertsPerPrim > 2)
                vertexId2 = m_builder->CreateAdd(vertexId0, m_builder->getInt32(2));

            // Primitive data layout [31:0]
            //   [31]    = null primitive flag
            //   [28:20] = vertexId2 (in bytes)
            //   [18:10] = vertexId1 (in bytes)
            //   [8:0]   = vertexId0 (in bytes)
            Value* primData = nullptr;
            if (outVertsPerPrim == 1)
                primData = vertexId0;
            else if (outVertsPerPrim == 2)
            {
                primData = m_builder->CreateShl(vertexId1, 10);
                primData = m_builder->CreateOr(primData, vertexId0);
            }
            else if (outVertsPerPrim == 3)
            {
                // Consider vertex ordering (normal: N -> N+1 -> N+2, flip: N -> N+2 -> N+1)
                primData = m_builder->CreateShl(vertexId2, 10);
                primData = m_builder->CreateOr(primData, vertexId1);
                primData = m_builder->CreateShl(primData, 10);
                primData = m_builder->CreateOr(primData, vertexId0);

                auto primDataFlip = m_builder->CreateShl(vertexId1, 10);
                primDataFlip = m_builder->CreateOr(primDataFlip, vertexId2);
                primDataFlip = m_builder->CreateShl(primDataFlip, 10);
                primDataFlip = m_builder->CreateOr(primDataFlip, vertexId0);

                primData = m_builder->CreateSelect(flipVertOrder, primDataFlip, primData);
            }
            else
                llvm_unreachable("Should never be called!");

            const unsigned maxOutPrims = resUsage->inOutUsage.gs.calcFactor.primAmpFactor;

            unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimData);

            // ldsOffset = regionStart + (threadIdInSubgroup * maxOutPrims + outPrimCounter) * 4
            auto ldsOffset = m_builder->CreateMul(threadIdInSubgroup, m_builder->getInt32(maxOutPrims));
            ldsOffset = m_builder->CreateAdd(ldsOffset, outPrimCounter);
            ldsOffset = m_builder->CreateShl(ldsOffset, 2);
            ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

            m_ldsManager->writeValueToLds(primData, ldsOffset);
        }

        m_builder->CreateBr(endEmitPrimBlock);
    }

    // Construct ".endEmitPrim" block
    {
        m_builder->SetInsertPoint(endEmitPrimBlock);

        // NOTE: We use selection instruction to update values of emit counter and GS output primitive counter. This is
        // friendly to CFG simplification.
        auto emitCounterDec = m_builder->CreateSub(emitCounter, m_builder->getInt32(1));
        auto outPrimCounterInc = m_builder->CreateAdd(outPrimCounter, m_builder->getInt32(1));

        // if (primComplete) emitCounter--
        emitCounter = m_builder->CreateSelect(primComplete, emitCounterDec, emitCounter);

        // if (primComplete) outPrimCounter++
        outPrimCounter = m_builder->CreateSelect(primComplete, outPrimCounterInc, outPrimCounter);

        // if (primComplete) outstandingVertCounter = 0
        outstandingVertCounter =
            m_builder->CreateSelect(primComplete, m_builder->getInt32(0), outstandingVertCounter);

        m_builder->CreateStore(emitCounter, emitCounterPtr);
        m_builder->CreateStore(outVertCounter, outVertCounterPtr);
        m_builder->CreateStore(outPrimCounter, outPrimCounterPtr);
        m_builder->CreateStore(outstandingVertCounter, outstandingVertCounterPtr);

        // Flip vertex ordering only for triangle strip
        if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
        {
            // if (primComplete) flipVertOrder = !flipVertOrder
            flipVertOrder = m_builder->CreateSelect(
                primComplete, m_builder->CreateNot(flipVertOrder), flipVertOrder);
            m_builder->CreateStore(flipVertOrder, flipVertOrderPtr);
        }

        m_builder->CreateRetVoid();
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that processes GS_EMIT.
Function* NggPrimShader::createGsCutHandler(
    Module*     module,    // [in] LLVM module
    unsigned    streamId)   // ID of output vertex stream
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
    const auto addrSpace = module->getDataLayout().getAllocaAddrSpace();
    auto funcTy =
        FunctionType::get(m_builder->getVoidTy(),
                          {
                              m_builder->getInt32Ty(),                                // %threadIdInSubgroup
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %emitCounterPtr
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %outVertCounterPtr
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %outPrimCounterPtr
                              PointerType::get(m_builder->getInt32Ty(), addrSpace),   // %outstandingVertCounterPtr
                              PointerType::get(m_builder->getInt1Ty(),  addrSpace),   // %flipVertOrderPtr
                          },
                          false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggGsCut, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* threadIdInSubgroup = argIt++;
    threadIdInSubgroup->setName("threadIdInSubgroup");

    Value* emitCounterPtr = argIt++;
    emitCounterPtr->setName("emitCounterPtr");

    Value* outVertCounterPtr = argIt++;
    outVertCounterPtr->setName("outVertCounterPtr");

    Value* outPrimCounterPtr = argIt++;
    outPrimCounterPtr->setName("outPrimCounterPtr");

    Value* outstandingVertCounterPtr = argIt++;
    outstandingVertCounterPtr->setName("outstandingVertCounterPtr");

    Value* flipVertOrderPtr = argIt++; // Used by triangle strip
    flipVertOrderPtr->setName("flipVertOrderPtr");

    auto entryBlock = createBlock(func, ".entry");
    auto emitPrimBlock = createBlock(func, ".emitPrim");
    auto endEmitPrimBlock = createBlock(func, ".endEmitPrim");

    auto savedInsertPoint = m_builder->saveIP();

    const auto& geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    const auto& resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

    // Get GS output vertices per output primitive
    unsigned outVertsPerPrim = 0;
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
    auto outVertsPerPrimVal = m_builder->getInt32(outVertsPerPrim);

    const unsigned maxOutPrims = resUsage->inOutUsage.gs.calcFactor.primAmpFactor;
    auto maxOutPrimsVal = m_builder->getInt32(maxOutPrims);

    // Construct ".entry" block
    Value* emitCounter = nullptr;
    Value* outPrimCounter = nullptr;
    Value* primIncomplete = nullptr;
    {
        m_builder->SetInsertPoint(entryBlock);

        emitCounter = m_builder->CreateLoad(emitCounterPtr);
        outPrimCounter = m_builder->CreateLoad(outPrimCounterPtr);

        // hasEmit = (emitCounter > 0)
        auto hasEmit = m_builder->CreateICmpUGT(emitCounter, m_builder->getInt32(0));

        // primIncomplete = (emitCounter != outVertsPerPrim)
        primIncomplete = m_builder->CreateICmpNE(emitCounter, outVertsPerPrimVal);

        // validPrimCounter = (outPrimCounter < maxOutPrims)
        auto validPrimCounter = m_builder->CreateICmpULT(outPrimCounter, maxOutPrimsVal);

        primIncomplete = m_builder->CreateAnd(hasEmit, primIncomplete);
        primIncomplete = m_builder->CreateAnd(primIncomplete, validPrimCounter);

        m_builder->CreateCondBr(primIncomplete, emitPrimBlock, endEmitPrimBlock);
    }

    // Construct ".emitPrim" block
    {
        m_builder->SetInsertPoint(emitPrimBlock);

        // NOTE: Only write incomplete GS output primitive to LDS for rasterization stream.
        if (streamId == resUsage->inOutUsage.gs.rasterStream)
        {
            unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimData);

            // ldsOffset = regionStart + (threadIdInSubgroup * maxOutPrims + outPrimCounter) * 4
            auto ldsOffset = m_builder->CreateMul(threadIdInSubgroup, m_builder->getInt32(maxOutPrims));
            ldsOffset = m_builder->CreateAdd(ldsOffset, outPrimCounter);
            ldsOffset = m_builder->CreateShl(ldsOffset, 2);
            ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

            m_ldsManager->writeValueToLds(m_builder->getInt32(NullPrim), ldsOffset);
        }

        m_builder->CreateBr(endEmitPrimBlock);
    }

    // Construct ".endEmitPrim" block
    {
        m_builder->SetInsertPoint(endEmitPrimBlock);

        // Reset emit counter
        m_builder->CreateStore(m_builder->getInt32(0), emitCounterPtr);

        // NOTE: We use selection instruction to update the value of GS output primitive counter. This is
        // friendly to CFG simplification.

        // if (primComplete) outPrimCounter++
        auto outPrimCounterInc = m_builder->CreateAdd(outPrimCounter, m_builder->getInt32(1));
        outPrimCounter = m_builder->CreateSelect(primIncomplete, outPrimCounterInc, outPrimCounter);
        m_builder->CreateStore(outPrimCounter, outPrimCounterPtr);

        // outVertCounter -= outstandingVertCounter
        Value* outVertCounter = m_builder->CreateLoad(outVertCounterPtr);
        Value* outstandingVertCounter = m_builder->CreateLoad(outstandingVertCounterPtr);

        outVertCounter = m_builder->CreateSub(outVertCounter, outstandingVertCounter);
        m_builder->CreateStore(outVertCounter, outVertCounterPtr);

        // Reset outstanding vertex counter
        m_builder->CreateStore(m_builder->getInt32(0), outstandingVertCounterPtr);

        // Flip vertex ordering only for triangle strip
        if (geometryMode.outputPrimitive == OutputPrimitives::TriangleStrip)
        {
            // flipVertOrder = false
            m_builder->CreateStore(m_builder->getFalse(), flipVertOrderPtr);
        }

        m_builder->CreateRetVoid();
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Revises GS output primitive data. The data in LDS region "OutPrimData" contains vertex indices representing the
// connectivity of this primitive. The vertex indices were "thread-view" values before this revising. They are the output
// vertices emitted by this GS thread. After revising, the index values are "subgroup-view" ones, corresponding to the
// output vertices emitted by the whole GS sub-group. Thus, number of output vertices prior to this GS thread is
// counted in.
void NggPrimShader::reviseOutputPrimitiveData(
    Value* outPrimId,       // [in] GS output primitive ID
    Value* vertexIdAdjust)  // [in] Adjustment of vertex indices corresponding to the GS output primitive
{
    const auto& geometryMode = m_pipelineState->getShaderModes()->getGeometryShaderMode();
    const auto resUsage = m_pipelineState->getShaderResourceUsage(ShaderStageGeometry);

    unsigned regionStart = m_ldsManager->getLdsRegionStart(LdsRegionOutPrimData);

    // ldsOffset = regionStart + (threadIdInSubgroup * maxOutPrims + outPrimId) * 4
    const unsigned maxOutPrims = resUsage->inOutUsage.gs.calcFactor.primAmpFactor;
    auto ldsOffset = m_builder->CreateMul(m_nggFactor.threadIdInSubgroup, m_builder->getInt32(maxOutPrims));
    ldsOffset = m_builder->CreateAdd(ldsOffset, outPrimId);
    ldsOffset = m_builder->CreateShl(ldsOffset, m_builder->getInt32(2));
    ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

    auto primData = m_ldsManager->readValueFromLds(m_builder->getInt32Ty(), ldsOffset);

    // Get GS output vertices per output primitive
    unsigned outVertsPerPrim = 0;
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
    Value* vertexId0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                    m_builder->getInt32Ty(),
                                                    {
                                                        primData,
                                                        m_builder->getInt32(0),
                                                        m_builder->getInt32(9)
                                                    });
    vertexId0 = m_builder->CreateAdd(vertexIdAdjust, vertexId0);

    Value* vertexId1 = nullptr;
    if (outVertsPerPrim > 1)
    {
        vertexId1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_builder->getInt32Ty(),
                                                 {
                                                     primData,
                                                     m_builder->getInt32(10),
                                                     m_builder->getInt32(9)
                                                 });
        vertexId1 = m_builder->CreateAdd(vertexIdAdjust, vertexId1);
    }

    Value* vertexId2 = nullptr;
    if (outVertsPerPrim > 2)
    {
        vertexId2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_builder->getInt32Ty(),
                                                 {
                                                     primData,
                                                     m_builder->getInt32(20),
                                                     m_builder->getInt32(9)
                                                 });
        vertexId2 = m_builder->CreateAdd(vertexIdAdjust, vertexId2);
    }

    Value* newPrimData = nullptr;
    if (outVertsPerPrim == 1)
        newPrimData = vertexId0;
    else if (outVertsPerPrim == 2)
    {
        newPrimData = m_builder->CreateShl(vertexId1, 10);
        newPrimData = m_builder->CreateOr(newPrimData, vertexId0);
    }
    else if (outVertsPerPrim == 3)
    {
        newPrimData = m_builder->CreateShl(vertexId2, 10);
        newPrimData = m_builder->CreateOr(newPrimData, vertexId1);
        newPrimData = m_builder->CreateShl(newPrimData, 10);
        newPrimData = m_builder->CreateOr(newPrimData, vertexId0);
    }
    else
        llvm_unreachable("Should never be called!");

    auto isNullPrim = m_builder->CreateICmpEQ(primData, m_builder->getInt32(NullPrim));
    newPrimData = m_builder->CreateSelect(isNullPrim, m_builder->getInt32(NullPrim), newPrimData);

    m_ldsManager->writeValueToLds(newPrimData, ldsOffset);
}

// =====================================================================================================================
// Reads per-thread data from the specified NGG region in LDS.
Value* NggPrimShader::readPerThreadDataFromLds(
    Type*             readDataTy,  // [in] Data written to LDS
    Value*            threadId,    // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType  region)       // NGG LDS region
{
    auto sizeInBytes = readDataTy->getPrimitiveSizeInBits() / 8;

    const auto regionStart = m_ldsManager->getLdsRegionStart(region);

    Value* ldsOffset = nullptr;
    if (sizeInBytes > 1)
        ldsOffset = m_builder->CreateMul(threadId, m_builder->getInt32(sizeInBytes));
    else
        ldsOffset = threadId;
    ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

    return m_ldsManager->readValueFromLds(readDataTy, ldsOffset);
}

// =====================================================================================================================
// Writes the per-thread data to the specified NGG region in LDS.
void NggPrimShader::writePerThreadDataToLds(
    Value*           writeData,        // [in] Data written to LDS
    Value*           threadId,         // [in] Thread ID in sub-group to calculate LDS offset
    NggLdsRegionType region)            // NGG LDS region
{
    auto writeDataTy = writeData->getType();
    auto sizeInBytes = writeDataTy->getPrimitiveSizeInBits() / 8;

    const auto regionStart = m_ldsManager->getLdsRegionStart(region);

    Value* ldsOffset = nullptr;
    if (sizeInBytes > 1)
        ldsOffset = m_builder->CreateMul(threadId, m_builder->getInt32(sizeInBytes));
    else
        ldsOffset = threadId;
    ldsOffset = m_builder->CreateAdd(ldsOffset, m_builder->getInt32(regionStart));

    m_ldsManager->writeValueToLds(writeData, ldsOffset);
}

// =====================================================================================================================
// Backface culler.
Value* NggPrimShader::doBackfaceCulling(
    Module*     module,        // [in] LLVM module
    Value*      cullFlag,      // [in] Cull flag before doing this culling
    Value*      vertex0,       // [in] Position data of vertex0
    Value*      vertex1,       // [in] Position data of vertex1
    Value*      vertex2)       // [in] Position data of vertex2
{
    assert(m_nggControl->enableBackfaceCulling);

    auto backfaceCuller = module->getFunction(lgcName::NggCullingBackface);
    if (backfaceCuller == nullptr)
        backfaceCuller = createBackfaceCuller(module);

    unsigned regOffset = 0;

    // Get register PA_SU_SC_MODE_CNTL
    Value* paSuScModeCntl = nullptr;
    if (m_nggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paSuScModeCntl);
        paSuScModeCntl = fetchCullingControlRegister(module, regOffset);
    }
    else
        paSuScModeCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paSuScModeCntl);

    // Get register PA_CL_VPORT_XSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    auto paClVportXscale = fetchCullingControlRegister(module, regOffset);

    // Get register PA_CL_VPORT_YSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    auto paClVportYscale = fetchCullingControlRegister(module, regOffset);

    // Do backface culling
    return m_builder->CreateCall(backfaceCuller,
                                  {
                                      cullFlag,
                                      vertex0,
                                      vertex1,
                                      vertex2,
                                      m_builder->getInt32(m_nggControl->backfaceExponent),
                                      paSuScModeCntl,
                                      paClVportXscale,
                                      paClVportYscale
                                  });
}

// =====================================================================================================================
// Frustum culler.
Value* NggPrimShader::doFrustumCulling(
    Module*     module,        // [in] LLVM module
    Value*      cullFlag,      // [in] Cull flag before doing this culling
    Value*      vertex0,       // [in] Position data of vertex0
    Value*      vertex1,       // [in] Position data of vertex1
    Value*      vertex2)       // [in] Position data of vertex2
{
    assert(m_nggControl->enableFrustumCulling);

    auto frustumCuller = module->getFunction(lgcName::NggCullingFrustum);
    if (frustumCuller == nullptr)
        frustumCuller = createFrustumCuller(module);

    unsigned regOffset = 0;

    // Get register PA_CL_CLIP_CNTL
    Value* paClClipCntl = nullptr;
    if (m_nggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        paClClipCntl = fetchCullingControlRegister(module, regOffset);
    }
    else
        paClClipCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClClipCntl);

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, regOffset);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto paClGbVertDiscAdj = fetchCullingControlRegister(module, regOffset);

    // Do frustum culling
    return m_builder->CreateCall(frustumCuller,
                                  {
                                      cullFlag,
                                      vertex0,
                                      vertex1,
                                      vertex2,
                                      paClClipCntl,
                                      paClGbHorzDiscAdj,
                                      paClGbVertDiscAdj
                                  });
}

// =====================================================================================================================
// Box filter culler.
Value* NggPrimShader::doBoxFilterCulling(
    Module*     module,        // [in] LLVM module
    Value*      cullFlag,      // [in] Cull flag before doing this culling
    Value*      vertex0,       // [in] Position data of vertex0
    Value*      vertex1,       // [in] Position data of vertex1
    Value*      vertex2)       // [in] Position data of vertex2
{
    assert(m_nggControl->enableBoxFilterCulling);

    auto boxFilterCuller = module->getFunction(lgcName::NggCullingBoxFilter);
    if (boxFilterCuller == nullptr)
        boxFilterCuller = createBoxFilterCuller(module);

    unsigned regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* paClVteCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_CLIP_CNTL
    Value* paClClipCntl = nullptr;
    if (m_nggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        paClClipCntl = fetchCullingControlRegister(module, regOffset);
    }
    else
        paClClipCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClClipCntl);

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, regOffset);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto paClGbVertDiscAdj = fetchCullingControlRegister(module, regOffset);

    // Do box filter culling
    return m_builder->CreateCall(boxFilterCuller,
                                  {
                                      cullFlag,
                                      vertex0,
                                      vertex1,
                                      vertex2,
                                      paClVteCntl,
                                      paClClipCntl,
                                      paClGbHorzDiscAdj,
                                      paClGbVertDiscAdj
                                  });
}

// =====================================================================================================================
// Sphere culler.
Value* NggPrimShader::doSphereCulling(
    Module*     module,        // [in] LLVM module
    Value*      cullFlag,      // [in] Cull flag before doing this culling
    Value*      vertex0,       // [in] Position data of vertex0
    Value*      vertex1,       // [in] Position data of vertex1
    Value*      vertex2)       // [in] Position data of vertex2
{
    assert(m_nggControl->enableSphereCulling);

    auto sphereCuller = module->getFunction(lgcName::NggCullingSphere);
    if (sphereCuller == nullptr)
        sphereCuller = createSphereCuller(module);

    unsigned regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* paClVteCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_CLIP_CNTL
    Value* paClClipCntl = nullptr;
    if (m_nggControl->alwaysUsePrimShaderTable)
    {
        regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
        regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClClipCntl);
        paClClipCntl = fetchCullingControlRegister(module, regOffset);
    }
    else
        paClClipCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClClipCntl);

    // Get register PA_CL_GB_HORZ_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbHorzDiscAdj);
    auto paClGbHorzDiscAdj = fetchCullingControlRegister(module, regOffset);

    // Get register PA_CL_GB_VERT_DISC_ADJ
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, pipelineStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderPsoCb, paClGbVertDiscAdj);
    auto paClGbVertDiscAdj = fetchCullingControlRegister(module, regOffset);

    // Do small primitive filter culling
    return m_builder->CreateCall(sphereCuller,
                                  {
                                      cullFlag,
                                      vertex0,
                                      vertex1,
                                      vertex2,
                                      paClVteCntl,
                                      paClClipCntl,
                                      paClGbHorzDiscAdj,
                                      paClGbVertDiscAdj
                                  });
}

// =====================================================================================================================
// Small primitive filter culler.
Value* NggPrimShader::doSmallPrimFilterCulling(
    Module*     module,        // [in] LLVM module
    Value*      cullFlag,      // [in] Cull flag before doing this culling
    Value*      vertex0,       // [in] Position data of vertex0
    Value*      vertex1,       // [in] Position data of vertex1
    Value*      vertex2)       // [in] Position data of vertex2
{
    assert(m_nggControl->enableSmallPrimFilter);

    auto smallPrimFilterCuller = module->getFunction(lgcName::NggCullingSmallPrimFilter);
    if (smallPrimFilterCuller == nullptr)
        smallPrimFilterCuller = createSmallPrimFilterCuller(module);

    unsigned regOffset = 0;

    // Get register PA_CL_VTE_CNTL
    Value* paClVteCntl = m_builder->getInt32(m_nggControl->primShaderTable.pipelineStateCb.paClVteCntl);

    // Get register PA_CL_VPORT_XSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportXscale);
    auto paClVportXscale = fetchCullingControlRegister(module, regOffset);

    // Get register PA_CL_VPORT_YSCALE
    regOffset  = offsetof(Util::Abi::PrimShaderCbLayout, viewportStateCb);
    regOffset += offsetof(Util::Abi::PrimShaderVportCb, vportControls[0].paClVportYscale);
    auto paClVportYscale = fetchCullingControlRegister(module, regOffset);

    // Do small primitive filter culling
    return m_builder->CreateCall(smallPrimFilterCuller,
                                  {
                                      cullFlag,
                                      vertex0,
                                      vertex1,
                                      vertex2,
                                      paClVteCntl,
                                      paClVportXscale,
                                      paClVportYscale
                                  });
}

// =====================================================================================================================
// Cull distance culler.
Value* NggPrimShader::doCullDistanceCulling(
    Module*     module,        // [in] LLVM module
    Value*      cullFlag,      // [in] Cull flag before doing this culling
    Value*      signMask0,     // [in] Sign mask of cull distance of vertex0
    Value*      signMask1,     // [in] Sign mask of cull distance of vertex1
    Value*      signMask2)     // [in] Sign mask of cull distance of vertex2
{
    assert(m_nggControl->enableCullDistanceCulling);

    auto cullDistanceCuller = module->getFunction(lgcName::NggCullingCullDistance);
    if (cullDistanceCuller == nullptr)
        cullDistanceCuller = createCullDistanceCuller(module);

    // Do cull distance culling
    return m_builder->CreateCall(cullDistanceCuller,
                                  {
                                      cullFlag,
                                      signMask0,
                                      signMask1,
                                      signMask2
                                  });
}

// =====================================================================================================================
// Fetches culling-control register from primitive shader table.
Value* NggPrimShader::fetchCullingControlRegister(
    Module*     module,        // [in] LLVM module
    unsigned    regOffset)      // Register offset in the primitive shader table (in BYTEs)
{
    auto fetchCullingRegister = module->getFunction(lgcName::NggCullingFetchReg);
    if (fetchCullingRegister == nullptr)
        fetchCullingRegister = createFetchCullingRegister(module);

    return m_builder->CreateCall(fetchCullingRegister,
                                  {
                                      m_nggFactor.primShaderTableAddrLow,
                                      m_nggFactor.primShaderTableAddrHigh,
                                      m_builder->getInt32(regOffset)
                                  });
}

// =====================================================================================================================
// Creates the function that does backface culling.
Function* NggPrimShader::createBackfaceCuller(
    Module* module) // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                     {
                                         m_builder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                         m_builder->getInt32Ty(),                          // %backfaceExponent
                                         m_builder->getInt32Ty(),                          // %paSuScModeCntl
                                         m_builder->getInt32Ty(),                          // %paClVportXscale
                                         m_builder->getInt32Ty()                           // %paClVportYscale
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingBackface, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadNone);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* cullFlag = argIt++;
    cullFlag->setName("cullFlag");

    Value* vertex0 = argIt++;
    vertex0->setName("vertex0");

    Value* vertex1 = argIt++;
    vertex1->setName("vertex1");

    Value* vertex2 = argIt++;
    vertex2->setName("vertex2");

    Value* backfaceExponent = argIt++;
    backfaceExponent->setName("backfaceExponent");

    Value* paSuScModeCntl = argIt++;
    paSuScModeCntl->setName("paSuScModeCntl");

    Value* paClVportXscale = argIt++;
    paClVportXscale->setName("paClVportXscale");

    Value* paClVportYscale = argIt++;
    paClVportYscale->setName("paClVportYscale");

    auto backfaceEntryBlock = createBlock(func, ".backfaceEntry");
    auto backfaceCullBlock = createBlock(func, ".backfaceCull");
    auto backfaceExponentBlock = createBlock(func, ".backfaceExponent");
    auto endBackfaceCullBlock = createBlock(func, ".endBackfaceCull");
    auto backfaceExitBlock = createBlock(func, ".backfaceExit");

    auto savedInsertPoint = m_builder->saveIP();

    // Construct ".backfaceEntry" block
    {
        m_builder->SetInsertPoint(backfaceEntryBlock);
        // If cull flag has already been TRUE, early return
        m_builder->CreateCondBr(cullFlag, backfaceExitBlock, backfaceCullBlock);
    }

    // Construct ".backfaceCull" block
    Value* cullFlag1 = nullptr;
    Value* w0 = nullptr;
    Value* w1 = nullptr;
    Value* w2 = nullptr;
    Value* area = nullptr;
    {
        m_builder->SetInsertPoint(backfaceCullBlock);

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
        auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
        auto y0 = m_builder->CreateExtractElement(vertex0, 1);
        w0 = m_builder->CreateExtractElement(vertex0, 3);

        auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
        auto y1 = m_builder->CreateExtractElement(vertex1, 1);
        w1 = m_builder->CreateExtractElement(vertex1, 3);

        auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
        auto y2 = m_builder->CreateExtractElement(vertex2, 1);
        w2 = m_builder->CreateExtractElement(vertex2, 3);

        auto y1W2 = m_builder->CreateFMul(y1, w2);
        auto y2W1 = m_builder->CreateFMul(y2, w1);
        auto det0 = m_builder->CreateFSub(y1W2, y2W1);
        det0 = m_builder->CreateFMul(x0, det0);

        auto y0W2 = m_builder->CreateFMul(y0, w2);
        auto y2W0 = m_builder->CreateFMul(y2, w0);
        auto det1 = m_builder->CreateFSub(y0W2, y2W0);
        det1 = m_builder->CreateFMul(x1, det1);

        auto y0W1 = m_builder->CreateFMul(y0, w1);
        auto y1W0 = m_builder->CreateFMul(y1, w0);
        auto det2 = m_builder->CreateFSub(y0W1, y1W0);
        det2 = m_builder->CreateFMul(x2, det2);

        area = m_builder->CreateFSub(det0, det1);
        area = m_builder->CreateFAdd(area, det2);

        auto areaLtZero = m_builder->CreateFCmpOLT(area, ConstantFP::get(m_builder->getFloatTy(), 0.0));
        auto areaGtZero = m_builder->CreateFCmpOGT(area, ConstantFP::get(m_builder->getFloatTy(), 0.0));

        // xScale ^ yScale
        auto frontFace = m_builder->CreateXor(paClVportXscale, paClVportYscale);

        // signbit(xScale ^ yScale)
        frontFace = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_builder->getInt32Ty(),
                                                 {
                                                     frontFace,
                                                     m_builder->getInt32(31),
                                                     m_builder->getInt32(1)
                                                 });

        // face = (FACE, PA_SU_SC_MODE_CNTRL[2], 0 = CCW, 1 = CW)
        auto face = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                 m_builder->getInt32Ty(),
                                                 {
                                                     paSuScModeCntl,
                                                     m_builder->getInt32(2),
                                                     m_builder->getInt32(1)
                                                 });

        // face ^ signbit(xScale ^ yScale)
        frontFace = m_builder->CreateXor(face, frontFace);

        // (face ^ signbit(xScale ^ yScale)) == 0
        frontFace = m_builder->CreateICmpEQ(frontFace, m_builder->getInt32(0));

        // frontFace = ((face ^ signbit(xScale ^ yScale)) == 0) ? (area < 0) : (area > 0)
        frontFace = m_builder->CreateSelect(frontFace, areaLtZero, areaGtZero);

        // backFace = !frontFace
        auto backFace = m_builder->CreateNot(frontFace);

        // cullFront = (CULL_FRONT, PA_SU_SC_MODE_CNTRL[0], 0 = DONT CULL, 1 = CULL)
        auto cullFront = m_builder->CreateAnd(paSuScModeCntl, m_builder->getInt32(1));
        cullFront = m_builder->CreateTrunc(cullFront, m_builder->getInt1Ty());

        // cullBack = (CULL_BACK, PA_SU_SC_MODE_CNTRL[1], 0 = DONT CULL, 1 = CULL)
        Value* cullBack = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_builder->getInt32Ty(),
                                                       {
                                                           paSuScModeCntl,
                                                           m_builder->getInt32(1),
                                                           m_builder->getInt32(1)
                                                       });
        cullBack = m_builder->CreateTrunc(cullBack, m_builder->getInt1Ty());

        // cullFront = cullFront ? frontFace : false
        cullFront = m_builder->CreateSelect(cullFront, frontFace, m_builder->getFalse());

        // cullBack = cullBack ? backFace : false
        cullBack = m_builder->CreateSelect(cullBack, backFace, m_builder->getFalse());

        // cullFlag = cullFront || cullBack
        cullFlag1 = m_builder->CreateOr(cullFront, cullBack);

        auto nonZeroBackfaceExp = m_builder->CreateICmpNE(backfaceExponent, m_builder->getInt32(0));
        m_builder->CreateCondBr(nonZeroBackfaceExp, backfaceExponentBlock, endBackfaceCullBlock);
    }

    // Construct ".backfaceExponent" block
    Value* cullFlag2 = nullptr;
    {
        m_builder->SetInsertPoint(backfaceExponentBlock);

        //
        // Ignore area calculations that are less enough
        //   if (|area| < (10 ^ (-backfaceExponent)) / |w0 * w1 * w2| )
        //       cullFlag = false
        //

        // |w0 * w1 * w2|
        auto absW0W1W2 = m_builder->CreateFMul(w0, w1);
        absW0W1W2 = m_builder->CreateFMul(absW0W1W2, w2);
        absW0W1W2 = m_builder->CreateIntrinsic(Intrinsic::fabs, m_builder->getFloatTy(), absW0W1W2);

        // threeshold = (10 ^ (-backfaceExponent)) / |w0 * w1 * w2|
        auto threshold = m_builder->CreateNeg(backfaceExponent);
        threshold = m_builder->CreateIntrinsic(Intrinsic::powi,
                                                 m_builder->getFloatTy(),
                                                 {
                                                     ConstantFP::get(m_builder->getFloatTy(), 10.0),
                                                     threshold
                                                 });

        auto rcpAbsW0W1W2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), absW0W1W2);
        threshold = m_builder->CreateFMul(threshold, rcpAbsW0W1W2);

        // |area|
        auto absArea = m_builder->CreateIntrinsic(Intrinsic::fabs, m_builder->getFloatTy(), area);

        // cullFlag = cullFlag && (abs(area) >= threshold)
        cullFlag2 = m_builder->CreateFCmpOGE(absArea, threshold);
        cullFlag2 = m_builder->CreateAnd(cullFlag1, cullFlag2);

        m_builder->CreateBr(endBackfaceCullBlock);
    }

    // Construct ".endBackfaceCull" block
    Value* cullFlag3 = nullptr;
    {
        m_builder->SetInsertPoint(endBackfaceCullBlock);

        // cullFlag = cullFlag || (area == 0)
        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag1, backfaceCullBlock);
        cullFlagPhi->addIncoming(cullFlag2, backfaceExponentBlock);

        auto areaEqZero = m_builder->CreateFCmpOEQ(area, ConstantFP::get(m_builder->getFloatTy(), 0.0));

        cullFlag3 = m_builder->CreateOr(cullFlagPhi, areaEqZero);

        m_builder->CreateBr(backfaceExitBlock);
    }

    // Construct ".backfaceExit" block
    {
        m_builder->SetInsertPoint(backfaceExitBlock);

        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag, backfaceEntryBlock);
        cullFlagPhi->addIncoming(cullFlag3, endBackfaceCullBlock);

        // polyMode = (POLY_MODE, PA_SU_SC_MODE_CNTRL[4:3], 0 = DISABLE, 1 = DUAL)
        auto polyMode = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                     m_builder->getInt32Ty(),
                                                     {
                                                         paSuScModeCntl,
                                                         m_builder->getInt32(3),
                                                         m_builder->getInt32(2),
                                                     });

        // polyMode == 1
        auto wireFrameMode = m_builder->CreateICmpEQ(polyMode, m_builder->getInt32(1));

        // Disable backface culler if POLY_MODE is set to 1 (wireframe)
        // cullFlag = (polyMode == 1) ? false : cullFlag
        cullFlag = m_builder->CreateSelect(wireFrameMode, m_builder->getFalse(), cullFlagPhi);

        m_builder->CreateRet(cullFlag);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
Function* NggPrimShader::createFrustumCuller(
    Module* module)    // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                     {
                                         m_builder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                         m_builder->getInt32Ty(),                          // %paClClipCntl
                                         m_builder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                         m_builder->getInt32Ty()                           // %paClGbVertDiscAdj
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingFrustum, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadNone);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* cullFlag = argIt++;
    cullFlag->setName("cullFlag");

    Value* vertex0 = argIt++;
    vertex0->setName("vertex0");

    Value* vertex1 = argIt++;
    vertex1->setName("vertex1");

    Value* vertex2 = argIt++;
    vertex2->setName("vertex2");

    Value* paClClipCntl = argIt++;
    paClClipCntl->setName("paClClipCntl");

    Value* paClGbHorzDiscAdj = argIt++;
    paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

    Value* paClGbVertDiscAdj = argIt++;
    paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

    auto frustumEntryBlock = createBlock(func, ".frustumEntry");
    auto frustumCullBlock = createBlock(func, ".frustumCull");
    auto frustumExitBlock = createBlock(func, ".frustumExit");

    auto savedInsertPoint = m_builder->saveIP();

    // Construct ".frustumEntry" block
    {
        m_builder->SetInsertPoint(frustumEntryBlock);
        // If cull flag has already been TRUE, early return
        m_builder->CreateCondBr(cullFlag, frustumExitBlock, frustumCullBlock);
    }

    // Construct ".frustumCull" block
    Value* newCullFlag = nullptr;
    {
        m_builder->SetInsertPoint(frustumCullBlock);

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
        Value* clipSpaceDef = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                           m_builder->getInt32Ty(),
                                                           {
                                                               paClClipCntl,
                                                               m_builder->getInt32(19),
                                                               m_builder->getInt32(1)
                                                           });
        clipSpaceDef = m_builder->CreateTrunc(clipSpaceDef, m_builder->getInt1Ty());

        // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
        auto zNear = m_builder->CreateSelect(clipSpaceDef,
                                               ConstantFP::get(m_builder->getFloatTy(), -1.0),
                                               ConstantFP::get(m_builder->getFloatTy(), 0.0));

        // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
        auto xDiscAdj = m_builder->CreateBitCast(paClGbHorzDiscAdj, m_builder->getFloatTy());

        // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
        auto yDiscAdj = m_builder->CreateBitCast(paClGbVertDiscAdj, m_builder->getFloatTy());

        auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
        auto y0 = m_builder->CreateExtractElement(vertex0, 1);
        auto z0 = m_builder->CreateExtractElement(vertex0, 2);
        auto w0 = m_builder->CreateExtractElement(vertex0, 3);

        auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
        auto y1 = m_builder->CreateExtractElement(vertex1, 1);
        auto z1 = m_builder->CreateExtractElement(vertex1, 2);
        auto w1 = m_builder->CreateExtractElement(vertex1, 3);

        auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
        auto y2 = m_builder->CreateExtractElement(vertex2, 1);
        auto z2 = m_builder->CreateExtractElement(vertex2, 2);
        auto w2 = m_builder->CreateExtractElement(vertex2, 3);

        // -xDiscAdj
        auto negXDiscAdj = m_builder->CreateFNeg(xDiscAdj);

        // -yDiscAdj
        auto negYDiscAdj = m_builder->CreateFNeg(yDiscAdj);

        Value* clipMask[6] = {};

        //
        // Get clip mask for vertex0
        //

        // (x0 < -xDiscAdj * w0) ? 0x1 : 0
        clipMask[0] = m_builder->CreateFMul(negXDiscAdj, w0);
        clipMask[0] = m_builder->CreateFCmpOLT(x0, clipMask[0]);
        clipMask[0] = m_builder->CreateSelect(clipMask[0], m_builder->getInt32(0x1), m_builder->getInt32(0));

        // (x0 > xDiscAdj * w0) ? 0x2 : 0
        clipMask[1] = m_builder->CreateFMul(xDiscAdj, w0);
        clipMask[1] = m_builder->CreateFCmpOGT(x0, clipMask[1]);
        clipMask[1] = m_builder->CreateSelect(clipMask[1], m_builder->getInt32(0x2), m_builder->getInt32(0));

        // (y0 < -yDiscAdj * w0) ? 0x4 : 0
        clipMask[2] = m_builder->CreateFMul(negYDiscAdj, w0);
        clipMask[2] = m_builder->CreateFCmpOLT(y0, clipMask[2]);
        clipMask[2] = m_builder->CreateSelect(clipMask[2], m_builder->getInt32(0x4), m_builder->getInt32(0));

        // (y0 > yDiscAdj * w0) ? 0x8 : 0
        clipMask[3] = m_builder->CreateFMul(yDiscAdj, w0);
        clipMask[3] = m_builder->CreateFCmpOGT(y0, clipMask[3]);
        clipMask[3] = m_builder->CreateSelect(clipMask[3], m_builder->getInt32(0x8), m_builder->getInt32(0));

        // (z0 < zNear * w0) ? 0x10 : 0
        clipMask[4] = m_builder->CreateFMul(zNear, w0);
        clipMask[4] = m_builder->CreateFCmpOLT(z0, clipMask[4]);
        clipMask[4] = m_builder->CreateSelect(clipMask[4], m_builder->getInt32(0x10), m_builder->getInt32(0));

        // (z0 > w0) ? 0x20 : 0
        clipMask[5] = m_builder->CreateFCmpOGT(z0, w0);
        clipMask[5] = m_builder->CreateSelect(clipMask[5], m_builder->getInt32(0x20), m_builder->getInt32(0));

        // clipMask0
        auto clipMaskX0 = m_builder->CreateOr(clipMask[0], clipMask[1]);
        auto clipMaskY0 = m_builder->CreateOr(clipMask[2], clipMask[3]);
        auto clipMaskZ0 = m_builder->CreateOr(clipMask[4], clipMask[5]);
        auto clipMask0 = m_builder->CreateOr(clipMaskX0, clipMaskY0);
        clipMask0 = m_builder->CreateOr(clipMask0, clipMaskZ0);

        //
        // Get clip mask for vertex1
        //

        // (x1 < -xDiscAdj * w1) ? 0x1 : 0
        clipMask[0] = m_builder->CreateFMul(negXDiscAdj, w1);
        clipMask[0] = m_builder->CreateFCmpOLT(x1, clipMask[0]);
        clipMask[0] = m_builder->CreateSelect(clipMask[0], m_builder->getInt32(0x1), m_builder->getInt32(0));

        // (x1 > xDiscAdj * w1) ? 0x2 : 0
        clipMask[1] = m_builder->CreateFMul(xDiscAdj, w1);
        clipMask[1] = m_builder->CreateFCmpOGT(x1, clipMask[1]);
        clipMask[1] = m_builder->CreateSelect(clipMask[1], m_builder->getInt32(0x2), m_builder->getInt32(0));

        // (y1 < -yDiscAdj * w1) ? 0x4 : 0
        clipMask[2] = m_builder->CreateFMul(negYDiscAdj, w1);
        clipMask[2] = m_builder->CreateFCmpOLT(y1, clipMask[2]);
        clipMask[2] = m_builder->CreateSelect(clipMask[2], m_builder->getInt32(0x4), m_builder->getInt32(0));

        // (y1 > yDiscAdj * w1) ? 0x8 : 0
        clipMask[3] = m_builder->CreateFMul(yDiscAdj, w1);
        clipMask[3] = m_builder->CreateFCmpOGT(y1, clipMask[3]);
        clipMask[3] = m_builder->CreateSelect(clipMask[3], m_builder->getInt32(0x8), m_builder->getInt32(0));

        // (z1 < zNear * w1) ? 0x10 : 0
        clipMask[4] = m_builder->CreateFMul(zNear, w1);
        clipMask[4] = m_builder->CreateFCmpOLT(z1, clipMask[4]);
        clipMask[4] = m_builder->CreateSelect(clipMask[4], m_builder->getInt32(0x10), m_builder->getInt32(0));

        // (z1 > w1) ? 0x20 : 0
        clipMask[5] = m_builder->CreateFCmpOGT(z1, w1);
        clipMask[5] = m_builder->CreateSelect(clipMask[5], m_builder->getInt32(0x20), m_builder->getInt32(0));

        // clipMask1
        auto clipMaskX1 = m_builder->CreateOr(clipMask[0], clipMask[1]);
        auto clipMaskY1 = m_builder->CreateOr(clipMask[2], clipMask[3]);
        auto clipMaskZ1 = m_builder->CreateOr(clipMask[4], clipMask[5]);
        auto clipMask1 = m_builder->CreateOr(clipMaskX1, clipMaskY1);
        clipMask1 = m_builder->CreateOr(clipMask1, clipMaskZ1);

        //
        // Get clip mask for vertex2
        //

        // (x2 < -xDiscAdj * w2) ? 0x1 : 0
        clipMask[0] = m_builder->CreateFMul(negXDiscAdj, w2);
        clipMask[0] = m_builder->CreateFCmpOLT(x2, clipMask[0]);
        clipMask[0] = m_builder->CreateSelect(clipMask[0], m_builder->getInt32(0x1), m_builder->getInt32(0));

        // (x2 > xDiscAdj * w2) ? 0x2 : 0
        clipMask[1] = m_builder->CreateFMul(xDiscAdj, w2);
        clipMask[1] = m_builder->CreateFCmpOGT(x2, clipMask[1]);
        clipMask[1] = m_builder->CreateSelect(clipMask[1], m_builder->getInt32(0x2), m_builder->getInt32(0));

        // (y2 < -yDiscAdj * w2) ? 0x4 : 0
        clipMask[2] = m_builder->CreateFMul(negYDiscAdj, w2);
        clipMask[2] = m_builder->CreateFCmpOLT(y2, clipMask[2]);
        clipMask[2] = m_builder->CreateSelect(clipMask[2], m_builder->getInt32(0x4), m_builder->getInt32(0));

        // (y2 > yDiscAdj * w2) ? 0x8 : 0
        clipMask[3] = m_builder->CreateFMul(yDiscAdj, w2);
        clipMask[3] = m_builder->CreateFCmpOGT(y2, clipMask[3]);
        clipMask[3] = m_builder->CreateSelect(clipMask[3], m_builder->getInt32(0x8), m_builder->getInt32(0));

        // (z2 < zNear * w2) ? 0x10 : 0
        clipMask[4] = m_builder->CreateFMul(zNear, w2);
        clipMask[4] = m_builder->CreateFCmpOLT(z2, clipMask[4]);
        clipMask[4] = m_builder->CreateSelect(clipMask[4], m_builder->getInt32(0x10), m_builder->getInt32(0));

        // (z2 > zFar * w2) ? 0x20 : 0
        clipMask[5] = m_builder->CreateFCmpOGT(z2, w2);
        clipMask[5] = m_builder->CreateSelect(clipMask[5], m_builder->getInt32(0x20), m_builder->getInt32(0));

        // clipMask2
        auto clipMaskX2 = m_builder->CreateOr(clipMask[0], clipMask[1]);
        auto clipMaskY2 = m_builder->CreateOr(clipMask[2], clipMask[3]);
        auto clipMaskZ2 = m_builder->CreateOr(clipMask[4], clipMask[5]);
        auto clipMask2 = m_builder->CreateOr(clipMaskX2, clipMaskY2);
        clipMask2 = m_builder->CreateOr(clipMask2, clipMaskZ2);

        // clip = clipMask0 & clipMask1 & clipMask2
        auto clip = m_builder->CreateAnd(clipMask0, clipMask1);
        clip = m_builder->CreateAnd(clip, clipMask2);

        // cullFlag = (clip != 0)
        newCullFlag = m_builder->CreateICmpNE(clip, m_builder->getInt32(0));

        m_builder->CreateBr(frustumExitBlock);
    }

    // Construct ".frustumExit" block
    {
        m_builder->SetInsertPoint(frustumExitBlock);

        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag, frustumEntryBlock);
        cullFlagPhi->addIncoming(newCullFlag, frustumCullBlock);

        m_builder->CreateRet(cullFlagPhi);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that does box filter culling.
Function* NggPrimShader::createBoxFilterCuller(
    Module* module)    // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                     {
                                         m_builder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                         m_builder->getInt32Ty(),                          // %paClVteCntl
                                         m_builder->getInt32Ty(),                          // %paClClipCntl
                                         m_builder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                         m_builder->getInt32Ty()                           // %paClGbVertDiscAdj
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingBoxFilter, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadNone);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* cullFlag = argIt++;
    cullFlag->setName("cullFlag");

    Value* vertex0 = argIt++;
    vertex0->setName("vertex0");

    Value* vertex1 = argIt++;
    vertex1->setName("vertex1");

    Value* vertex2 = argIt++;
    vertex2->setName("vertex2");

    Value* paClVteCntl = argIt++;
    paClVteCntl->setName("paClVteCntl");

    Value* paClClipCntl = argIt++;
    paClVteCntl->setName("paClClipCntl");

    Value* paClGbHorzDiscAdj = argIt++;
    paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

    Value* paClGbVertDiscAdj = argIt++;
    paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

    auto boxFilterEntryBlock = createBlock(func, ".boxfilterEntry");
    auto boxFilterCullBlock = createBlock(func, ".boxfilterCull");
    auto boxFilterExitBlock = createBlock(func, ".boxfilterExit");

    auto savedInsertPoint = m_builder->saveIP();

    // Construct ".boxfilterEntry" block
    {
        m_builder->SetInsertPoint(boxFilterEntryBlock);
        // If cull flag has already been TRUE, early return
        m_builder->CreateCondBr(cullFlag, boxFilterExitBlock, boxFilterCullBlock);
    }

    // Construct ".boxfilterCull" block
    Value* newCullFlag = nullptr;
    {
        m_builder->SetInsertPoint(boxFilterCullBlock);

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
        Value* vtxXyFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_builder->getInt32Ty(),
                                                       {
                                                           paClVteCntl,
                                                           m_builder->getInt32(8),
                                                           m_builder->getInt32(1)
                                                       });
        vtxXyFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

        // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
        Value* vtxZFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_builder->getInt32Ty(),
                                                       {
                                                           paClVteCntl,
                                                           m_builder->getInt32(9),
                                                           m_builder->getInt32(1)
                                                       });
        vtxZFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

        // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
        Value* clipSpaceDef = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                           m_builder->getInt32Ty(),
                                                           {
                                                               paClClipCntl,
                                                               m_builder->getInt32(19),
                                                               m_builder->getInt32(1)
                                                           });
        clipSpaceDef = m_builder->CreateTrunc(clipSpaceDef, m_builder->getInt1Ty());

        // zNear = clipSpaceDef ? -1.0 : 0.0, zFar = 1.0
        auto zNear = m_builder->CreateSelect(clipSpaceDef,
                                               ConstantFP::get(m_builder->getFloatTy(), -1.0),
                                               ConstantFP::get(m_builder->getFloatTy(), 0.0));
        auto zFar = ConstantFP::get(m_builder->getFloatTy(), 1.0);

        // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
        auto xDiscAdj = m_builder->CreateBitCast(paClGbHorzDiscAdj, m_builder->getFloatTy());

        // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
        auto yDiscAdj = m_builder->CreateBitCast(paClGbVertDiscAdj, m_builder->getFloatTy());

        auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
        auto y0 = m_builder->CreateExtractElement(vertex0, 1);
        auto z0 = m_builder->CreateExtractElement(vertex0, 2);
        auto w0 = m_builder->CreateExtractElement(vertex0, 3);

        auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
        auto y1 = m_builder->CreateExtractElement(vertex1, 1);
        auto z1 = m_builder->CreateExtractElement(vertex1, 2);
        auto w1 = m_builder->CreateExtractElement(vertex1, 3);

        auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
        auto y2 = m_builder->CreateExtractElement(vertex2, 1);
        auto z2 = m_builder->CreateExtractElement(vertex2, 2);
        auto w2 = m_builder->CreateExtractElement(vertex2, 3);

        // Convert xyz coordinate to normalized device coordinate (NDC)
        auto rcpW0 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w0);
        auto rcpW1 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w1);
        auto rcpW2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w2);

        // VTX_XY_FMT ? 1.0 : 1 / w0
        auto rcpW0ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
        // VTX_XY_FMT ? 1.0 : 1 / w1
        auto rcpW1ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
        // VTX_XY_FMT ? 1.0 : 1 / w2
        auto rcpW2ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

        // VTX_Z_FMT ? 1.0 : 1 / w0
        auto rcpW0ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
        // VTX_Z_FMT ? 1.0 : 1 / w1
        auto rcpW1ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
        // VTX_Z_FMT ? 1.0 : 1 / w2
        auto rcpW2ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

        // x0' = x0/w0
        x0 = m_builder->CreateFMul(x0, rcpW0ForXy);
        // y0' = y0/w0
        y0 = m_builder->CreateFMul(y0, rcpW0ForXy);
        // z0' = z0/w0
        z0 = m_builder->CreateFMul(z0, rcpW0ForZ);
        // x1' = x1/w1
        x1 = m_builder->CreateFMul(x1, rcpW1ForXy);
        // y1' = y1/w1
        y1 = m_builder->CreateFMul(y1, rcpW1ForXy);
        // z1' = z1/w1
        z1 = m_builder->CreateFMul(z1, rcpW1ForZ);
        // x2' = x2/w2
        x2 = m_builder->CreateFMul(x2, rcpW2ForXy);
        // y2' = y2/w2
        y2 = m_builder->CreateFMul(y2, rcpW2ForXy);
        // z2' = z2/w2
        z2 = m_builder->CreateFMul(z2, rcpW2ForZ);

        // -xDiscAdj
        auto negXDiscAdj = m_builder->CreateFNeg(xDiscAdj);

        // -yDiscAdj
        auto negYDiscAdj = m_builder->CreateFNeg(yDiscAdj);

        // minX = min(x0', x1', x2')
        auto minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { x0, x1 });
        minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { minX, x2 });

        // minX > xDiscAdj
        auto minXGtXDiscAdj = m_builder->CreateFCmpOGT(minX, xDiscAdj);

        // maxX = max(x0', x1', x2')
        auto maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { x0, x1 });
        maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { maxX, x2 });

        // maxX < -xDiscAdj
        auto maxXLtNegXDiscAdj = m_builder->CreateFCmpOLT(maxX, negXDiscAdj);

        // minY = min(y0', y1', y2')
        auto minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { y0, y1 });
        minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { minY, y2 });

        // minY > yDiscAdj
        auto minYGtYDiscAdj = m_builder->CreateFCmpOGT(minY, yDiscAdj);

        // maxY = max(y0', y1', y2')
        auto maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { y0, y1 });
        maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { maxY, y2 });

        // maxY < -yDiscAdj
        auto maxYLtNegYDiscAdj = m_builder->CreateFCmpOLT(maxY, negYDiscAdj);

        // minZ = min(z0', z1', z2')
        auto minZ = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { z0, z1 });
        minZ = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { minZ, z2 });

        // minZ > zFar (1.0)
        auto minZGtZFar = m_builder->CreateFCmpOGT(minZ, zFar);

        // maxZ = min(z0', z1', z2')
        auto maxZ = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { z0, z1 });
        maxZ = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { maxZ, z2 });

        // maxZ < zNear
        auto maxZLtZNear = m_builder->CreateFCmpOLT(maxZ, zNear);

        // Get cull flag
        auto cullX = m_builder->CreateOr(minXGtXDiscAdj, maxXLtNegXDiscAdj);
        auto cullY = m_builder->CreateOr(minYGtYDiscAdj, maxYLtNegYDiscAdj);
        auto cullZ = m_builder->CreateOr(minZGtZFar, maxZLtZNear);
        newCullFlag = m_builder->CreateOr(cullX, cullY);
        newCullFlag = m_builder->CreateOr(newCullFlag, cullZ);

        m_builder->CreateBr(boxFilterExitBlock);
    }

    // Construct ".boxfilterExit" block
    {
        m_builder->SetInsertPoint(boxFilterExitBlock);

        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag, boxFilterEntryBlock);
        cullFlagPhi->addIncoming(newCullFlag, boxFilterCullBlock);

        m_builder->CreateRet(cullFlagPhi);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that does sphere culling.
Function* NggPrimShader::createSphereCuller(
    Module* module)    // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                     {
                                         m_builder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                         m_builder->getInt32Ty(),                          // %paClVteCntl
                                         m_builder->getInt32Ty(),                          // %paClClipCntl
                                         m_builder->getInt32Ty(),                          // %paClGbHorzDiscAdj
                                         m_builder->getInt32Ty()                           // %paClGbVertDiscAdj
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingSphere, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadNone);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* cullFlag = argIt++;
    cullFlag->setName("cullFlag");

    Value* vertex0 = argIt++;
    vertex0->setName("vertex0");

    Value* vertex1 = argIt++;
    vertex1->setName("vertex1");

    Value* vertex2 = argIt++;
    vertex2->setName("vertex2");

    Value* paClVteCntl = argIt++;
    paClVteCntl->setName("paClVteCntl");

    Value* paClClipCntl = argIt++;
    paClVteCntl->setName("paClClipCntl");

    Value* paClGbHorzDiscAdj = argIt++;
    paClGbHorzDiscAdj->setName("paClGbHorzDiscAdj");

    Value* paClGbVertDiscAdj = argIt++;
    paClGbVertDiscAdj->setName("paClGbVertDiscAdj");

    auto sphereEntryBlock = createBlock(func, ".sphereEntry");
    auto sphereCullBlock = createBlock(func, ".sphereCull");
    auto sphereExitBlock = createBlock(func, ".sphereExit");

    auto savedInsertPoint = m_builder->saveIP();

    // Construct ".sphereEntry" block
    {
        m_builder->SetInsertPoint(sphereEntryBlock);
        // If cull flag has already been TRUE, early return
        m_builder->CreateCondBr(cullFlag, sphereExitBlock, sphereCullBlock);
    }

    // Construct ".sphereCull" block
    Value* newCullFlag = nullptr;
    {
        m_builder->SetInsertPoint(sphereCullBlock);

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
        Value* vtxXyFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_builder->getInt32Ty(),
                                                       {
                                                           paClVteCntl,
                                                           m_builder->getInt32(8),
                                                           m_builder->getInt32(1)
                                                       });
        vtxXyFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

        // vtxZFmt = (VTX_Z_FMT, PA_CL_VTE_CNTL[9], 0 = 1/W0, 1 = none)
        Value* vtxZFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_builder->getInt32Ty(),
                                                       {
                                                           paClVteCntl,
                                                           m_builder->getInt32(9),
                                                           m_builder->getInt32(1)
                                                       });
        vtxZFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

        // clipSpaceDef = (DX_CLIP_SPACE_DEF, PA_CL_CLIP_CNTL[19], 0 = OGL clip space, 1 = DX clip space)
        Value* clipSpaceDef = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                           m_builder->getInt32Ty(),
                                                           {
                                                               paClClipCntl,
                                                               m_builder->getInt32(19),
                                                               m_builder->getInt32(1)
                                                           });
        clipSpaceDef = m_builder->CreateTrunc(clipSpaceDef, m_builder->getInt1Ty());

        // zNear = clipSpaceDef ? -1.0 : 0.0
        auto zNear = m_builder->CreateSelect(clipSpaceDef,
                                               ConstantFP::get(m_builder->getFloatTy(), -1.0),
                                               ConstantFP::get(m_builder->getFloatTy(), 0.0));

        // xDiscAdj = (DATA_REGISTER, PA_CL_GB_HORZ_DISC_ADJ[31:0])
        auto xDiscAdj = m_builder->CreateBitCast(paClGbHorzDiscAdj, m_builder->getFloatTy());

        // yDiscAdj = (DATA_REGISTER, PA_CL_GB_VERT_DISC_ADJ[31:0])
        auto yDiscAdj = m_builder->CreateBitCast(paClGbVertDiscAdj, m_builder->getFloatTy());

        auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
        auto y0 = m_builder->CreateExtractElement(vertex0, 1);
        auto z0 = m_builder->CreateExtractElement(vertex0, 2);
        auto w0 = m_builder->CreateExtractElement(vertex0, 3);

        auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
        auto y1 = m_builder->CreateExtractElement(vertex1, 1);
        auto z1 = m_builder->CreateExtractElement(vertex1, 2);
        auto w1 = m_builder->CreateExtractElement(vertex1, 3);

        auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
        auto y2 = m_builder->CreateExtractElement(vertex2, 1);
        auto z2 = m_builder->CreateExtractElement(vertex2, 2);
        auto w2 = m_builder->CreateExtractElement(vertex2, 3);

        // Convert xyz coordinate to normalized device coordinate (NDC)
        auto rcpW0 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w0);
        auto rcpW1 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w1);
        auto rcpW2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w2);

        // VTX_XY_FMT ? 1.0 : 1 / w0
        auto rcpW0ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
        // VTX_XY_FMT ? 1.0 : 1 / w1
        auto rcpW1ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
        // VTX_XY_FMT ? 1.0 : 1 / w2
        auto rcpW2ForXy = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

        // VTX_Z_FMT ? 1.0 : 1 / w0
        auto rcpW0ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
        // VTX_Z_FMT ? 1.0 : 1 / w1
        auto rcpW1ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
        // VTX_Z_FMT ? 1.0 : 1 / w2
        auto rcpW2ForZ = m_builder->CreateSelect(vtxZFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

        // x0' = x0/w0
        x0 = m_builder->CreateFMul(x0, rcpW0ForXy);
        // y0' = y0/w0
        y0 = m_builder->CreateFMul(y0, rcpW0ForXy);
        // z0' = z0/w0
        z0 = m_builder->CreateFMul(z0, rcpW0ForZ);
        // x1' = x1/w1
        x1 = m_builder->CreateFMul(x1, rcpW1ForXy);
        // y1' = y1/w1
        y1 = m_builder->CreateFMul(y1, rcpW1ForXy);
        // z1' = z1/w1
        z1 = m_builder->CreateFMul(z1, rcpW1ForZ);
        // x2' = x2/w2
        x2 = m_builder->CreateFMul(x2, rcpW2ForXy);
        // y2' = y2/w2
        y2 = m_builder->CreateFMul(y2, rcpW2ForXy);
        // z2' = z2/w2
        z2 = m_builder->CreateFMul(z2, rcpW2ForZ);

        //
        // === Step 1 ===: Discard space to -1..1 space.
        //

        // x" = x'/xDiscAdj
        // y" = y'/yDiscAdj
        // z" = (zNear + 2.0)z' + (-1.0 - zNear)
        auto rcpXDiscAdj = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), xDiscAdj);
        auto rcpYDiscAdj = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), yDiscAdj);
        auto rcpXyDiscAdj =
            m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { rcpXDiscAdj, rcpYDiscAdj });

        Value* x0Y0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { x0, y0 });
        Value* x1Y1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { x1, y1 });
        Value* x2Y2 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { x2, y2 });

        x0Y0 = m_builder->CreateFMul(x0Y0, rcpXyDiscAdj);
        x1Y1 = m_builder->CreateFMul(x1Y1, rcpXyDiscAdj);
        x2Y2 = m_builder->CreateFMul(x2Y2, rcpXyDiscAdj);

        // zNear + 2.0
        auto zNearPlusTwo = m_builder->CreateFAdd(zNear, ConstantFP::get(m_builder->getFloatTy(), 2.0));
        zNearPlusTwo = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { zNearPlusTwo, zNearPlusTwo });

        // -1.0 - zNear
        auto negOneMinusZNear = m_builder->CreateFSub(ConstantFP::get(m_builder->getFloatTy(), -1.0), zNear);
        negOneMinusZNear =
            m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { negOneMinusZNear, negOneMinusZNear });

        Value* z0Z0 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { z0, z0 });
        Value* z2Z1 = m_builder->CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz, {}, { z2, z1 });

        z0Z0 = m_builder->CreateIntrinsic(Intrinsic::fma,
                                            VectorType::get(Type::getHalfTy(*m_context), 2),
                                            { zNearPlusTwo, z0Z0, negOneMinusZNear });
        z2Z1 = m_builder->CreateIntrinsic(Intrinsic::fma,
                                            VectorType::get(Type::getHalfTy(*m_context), 2),
                                            { zNearPlusTwo, z2Z1, negOneMinusZNear });

        //
        // === Step 2 ===: 3D coordinates to barycentric coordinates.
        //

        // <x20, y20> = <x2", y2"> - <x0", y0">
        auto x20Y20 = m_builder->CreateFSub(x2Y2, x0Y0);

        // <x10, y10> = <x1", y1"> - <x0", y0">
        auto x10Y10 = m_builder->CreateFSub(x1Y1, x0Y0);

        // <z20, z10> = <z2", z1"> - <z0", z0">
        auto z20Z10 = m_builder->CreateFSub(z2Z1, z0Z0);

        //
        // === Step 3 ===: Solve linear system and find the point closest to the origin.
        //

        // a00 = x10 + z10
        auto x10 = m_builder->CreateExtractElement(x10Y10, static_cast<uint64_t>(0));
        auto z10 = m_builder->CreateExtractElement(z20Z10, 1);
        auto a00 = m_builder->CreateFAdd(x10, z10);

        // a01 = x20 + z20
        auto x20 = m_builder->CreateExtractElement(x20Y20, static_cast<uint64_t>(0));
        auto z20 = m_builder->CreateExtractElement(z20Z10, static_cast<uint64_t>(0));
        auto a01 = m_builder->CreateFAdd(x20, z20);

        // a10 = y10 + y10
        auto y10 = m_builder->CreateExtractElement(x10Y10, 1);
        auto a10 = m_builder->CreateFAdd(y10, y10);

        // a11 = y20 + z20
        auto y20 = m_builder->CreateExtractElement(x20Y20, 1);
        auto a11 = m_builder->CreateFAdd(y20, z20);

        // b0 = -x0" - x2"
        x0 = m_builder->CreateExtractElement(x0Y0, static_cast<uint64_t>(0));
        auto negX0 = m_builder->CreateFNeg(x0);
        x2 = m_builder->CreateExtractElement(x2Y2, static_cast<uint64_t>(0));
        auto b0 = m_builder->CreateFSub(negX0, x2);

        // b1 = -x1" - x2"
        x1 = m_builder->CreateExtractElement(x1Y1, static_cast<uint64_t>(0));
        auto negX1 = m_builder->CreateFNeg(x1);
        auto b1 = m_builder->CreateFSub(negX1, x2);

        //     [ a00 a01 ]      [ b0 ]       [ s ]
        // A = [         ], B = [    ], ST = [   ], A * ST = B (crame rules)
        //     [ a10 a11 ]      [ b1 ]       [ t ]

        //           | a00 a01 |
        // det(A) =  |         | = a00 * a11 - a01 * a10
        //           | a10 a11 |
        auto detA = m_builder->CreateFMul(a00, a11);
        auto negA01 = m_builder->CreateFNeg(a01);
        detA = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { negA01, a10, detA });

        //            | b0 a01 |
        // det(Ab0) = |        | = b0 * a11 - a01 * b1
        //            | b1 a11 |
        auto detAB0 = m_builder->CreateFMul(b0, a11);
        detAB0 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { negA01, b1, detAB0 });

        //            | a00 b0 |
        // det(Ab1) = |        | = a00 * b1 - b0 * a10
        //            | a10 b1 |
        auto detAB1 = m_builder->CreateFMul(a00, b1);
        auto negB0 = m_builder->CreateFNeg(b0);
        detAB1 = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { negB0, a10, detAB1 });

        // s = det(Ab0) / det(A)
        auto rcpDetA = m_builder->CreateFDiv(ConstantFP::get(m_builder->getHalfTy(), 1.0), detA);
        auto s = m_builder->CreateFMul(detAB0, rcpDetA);

        // t = det(Ab1) / det(A)
        auto t = m_builder->CreateFMul(detAB1, rcpDetA);

        //
        // === Step 4 ===: Do clamping for the closest point.
        //

        // <s, t>
        auto st = m_builder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getHalfTy(*m_context), 2)),
                                                   s,
                                                   static_cast<uint64_t>(0));
        st = m_builder->CreateInsertElement(st, t, 1);

        // <s', t'> = <0.5 - 0.5(t - s), 0.5 + 0.5(t - s)>
        auto tMinusS = m_builder->CreateFSub(t, s);
        auto sT1 = m_builder->CreateInsertElement(UndefValue::get(VectorType::get(Type::getHalfTy(*m_context), 2)),
                                                    tMinusS,
                                                    static_cast<uint64_t>(0));
        sT1 = m_builder->CreateInsertElement(sT1, tMinusS, 1);

        sT1 = m_builder->CreateIntrinsic(Intrinsic::fma,
                                           VectorType::get(Type::getHalfTy(*m_context), 2),
                                           {
                                               ConstantVector::get({ ConstantFP::get(m_builder->getHalfTy(), -0.5),
                                                                     ConstantFP::get(m_builder->getHalfTy(), 0.5) }),
                                               sT1,
                                               ConstantVector::get({ ConstantFP::get(m_builder->getHalfTy(), 0.5),
                                                                     ConstantFP::get(m_builder->getHalfTy(), 0.5) })
                                           });

        // <s", t"> = clamp(<s, t>)
        auto sT2 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                                VectorType::get(Type::getHalfTy(*m_context), 2),
                                                {
                                                   st,
                                                   ConstantVector::get({ ConstantFP::get(m_builder->getHalfTy(), 0.0),
                                                                         ConstantFP::get(m_builder->getHalfTy(), 0.0) })
                                               });
        sT2 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                           VectorType::get(Type::getHalfTy(*m_context), 2),
                                           {
                                               sT2,
                                               ConstantVector::get({ ConstantFP::get(m_builder->getHalfTy(), 1.0),
                                                                     ConstantFP::get(m_builder->getHalfTy(), 1.0) })
                                           });

        // <s, t> = (s + t) > 1.0 ? <s', t'> : <s", t">
        auto sPlusT = m_builder->CreateFAdd(s, t);
        auto sPlusTGtOne = m_builder->CreateFCmpOGT(sPlusT, ConstantFP::get(m_builder->getHalfTy(), 1.0));
        st = m_builder->CreateSelect(sPlusTGtOne, sT1, sT2);

        //
        // === Step 5 ===: Barycentric coordinates to 3D coordinates.
        //

        // x = x0" + s * x10 + t * x20
        // y = y0" + s * y10 + t * y20
        // z = z0" + s * z10 + t * z20
        s = m_builder->CreateExtractElement(st, static_cast<uint64_t>(0));
        t = m_builder->CreateExtractElement(st, 1);
        auto ss = m_builder->CreateInsertElement(st, s, 1);
        auto tt = m_builder->CreateInsertElement(st, t, static_cast<uint64_t>(0));

        // s * <x10, y10> + <x0", y0">
        auto xy = m_builder->CreateIntrinsic(Intrinsic::fma,
                                               VectorType::get(Type::getHalfTy(*m_context), 2),
                                               { ss, x10Y10, x0Y0 });

        // <x, y> = t * <x20, y20> + (s * <x10, y10> + <x0", y0">)
        xy = m_builder->CreateIntrinsic(Intrinsic::fma,
                                          VectorType::get(Type::getHalfTy(*m_context), 2),
                                          { tt, x20Y20, xy });

        // s * z10 + z0"
        z0 = m_builder->CreateExtractElement(z0Z0, static_cast<uint64_t>(0));
        auto z = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { s, z10, z0});

        // z = t * z20 + (s * z10 + z0")
        z = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { t, z20, z });

        auto x = m_builder->CreateExtractElement(xy, static_cast<uint64_t>(0));
        auto y = m_builder->CreateExtractElement(xy, 1);

        //
        // === Step 6 ===: Compute the distance squared of the closest point.
        //

        // r^2 = x^2 + y^2 + z^2
        auto squareR = m_builder->CreateFMul(x, x);
        squareR = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { y, y, squareR });
        squareR = m_builder->CreateIntrinsic(Intrinsic::fma, m_builder->getHalfTy(), { z, z, squareR });

        //
        // == = Step 7 == = : Determine the cull flag
        //

        // cullFlag = (r ^ 2 > 3.0)
        newCullFlag = m_builder->CreateFCmpOGT(squareR, ConstantFP::get(m_builder->getHalfTy(), 3.0));

        m_builder->CreateBr(sphereExitBlock);
    }

    // Construct ".sphereExit" block
    {
        m_builder->SetInsertPoint(sphereExitBlock);

        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag, sphereEntryBlock);
        cullFlagPhi->addIncoming(newCullFlag, sphereCullBlock);

        m_builder->CreateRet(cullFlagPhi);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that does small primitive filter culling.
Function* NggPrimShader::createSmallPrimFilterCuller(
    Module* module)    // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                     {
                                         m_builder->getInt1Ty(),                           // %cullFlag
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex0
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex1
                                         VectorType::get(Type::getFloatTy(*m_context), 4), // %vertex2
                                         m_builder->getInt32Ty(),                          // %paClVteCntl
                                         m_builder->getInt32Ty(),                          // %paClVportXscale
                                         m_builder->getInt32Ty()                           // %paClVportYscale
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingSmallPrimFilter, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadNone);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* cullFlag = argIt++;
    cullFlag->setName("cullFlag");

    Value* vertex0 = argIt++;
    vertex0->setName("vertex0");

    Value* vertex1 = argIt++;
    vertex1->setName("vertex1");

    Value* vertex2 = argIt++;
    vertex2->setName("vertex2");

    Value* paClVteCntl = argIt++;
    paClVteCntl->setName("paClVteCntl");

    Value* paClVportXscale = argIt++;
    paClVportXscale->setName("paClVportXscale");

    Value* paClVportYscale = argIt++;
    paClVportYscale->setName("paClVportYscale");

    auto smallPrimFilterEntryBlock = createBlock(func, ".smallprimfilterEntry");
    auto smallPrimFilterCullBlock = createBlock(func, ".smallprimfilterCull");
    auto smallPrimFilterExitBlock = createBlock(func, ".smallprimfilterExit");

    auto savedInsertPoint = m_builder->saveIP();

    // Construct ".smallprimfilterEntry" block
    {
        m_builder->SetInsertPoint(smallPrimFilterEntryBlock);
        // If cull flag has already been TRUE, early return
        m_builder->CreateCondBr(cullFlag, smallPrimFilterExitBlock, smallPrimFilterCullBlock);
    }

    // Construct ".smallprimfilterCull" block
    Value* newCullFlag = nullptr;
    {
        m_builder->SetInsertPoint(smallPrimFilterCullBlock);

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
        Value* vtxXyFmt = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                       m_builder->getInt32Ty(),
                                                       {
                                                           paClVteCntl,
                                                           m_builder->getInt32(8),
                                                           m_builder->getInt32(1)
                                                       });
        vtxXyFmt = m_builder->CreateTrunc(vtxXyFmt, m_builder->getInt1Ty());

        // xScale = (VPORT_XSCALE, PA_CL_VPORT_XSCALE[31:0])
        auto xsCale = m_builder->CreateBitCast(paClVportXscale, m_builder->getFloatTy());

        // yScale = (VPORT_YSCALE, PA_CL_VPORT_YSCALE[31:0])
        auto ysCale = m_builder->CreateBitCast(paClVportYscale, m_builder->getFloatTy());

        auto x0 = m_builder->CreateExtractElement(vertex0, static_cast<uint64_t>(0));
        auto y0 = m_builder->CreateExtractElement(vertex0, 1);
        auto w0 = m_builder->CreateExtractElement(vertex0, 3);

        auto x1 = m_builder->CreateExtractElement(vertex1, static_cast<uint64_t>(0));
        auto y1 = m_builder->CreateExtractElement(vertex1, 1);
        auto w1 = m_builder->CreateExtractElement(vertex1, 3);

        auto x2 = m_builder->CreateExtractElement(vertex2, static_cast<uint64_t>(0));
        auto y2 = m_builder->CreateExtractElement(vertex2, 1);
        auto w2 = m_builder->CreateExtractElement(vertex2, 3);

        // Convert xyz coordinate to normalized device coordinate (NDC)
        auto rcpW0 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w0);
        auto rcpW1 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w1);
        auto rcpW2 = m_builder->CreateFDiv(ConstantFP::get(m_builder->getFloatTy(), 1.0), w2);

        // VTX_XY_FMT ? 1.0 : 1 / w0
        rcpW0 = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW0);
        // VTX_XY_FMT ? 1.0 : 1 / w1
        rcpW1 = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW1);
        // VTX_XY_FMT ? 1.0 : 1 / w2
        rcpW2 = m_builder->CreateSelect(vtxXyFmt, ConstantFP::get(m_builder->getFloatTy(), 1.0), rcpW2);

        // x0' = x0/w0
        x0 = m_builder->CreateFMul(x0, rcpW0);
        // y0' = y0/w0
        y0 = m_builder->CreateFMul(y0, rcpW0);
        // x1' = x1/w1
        x1 = m_builder->CreateFMul(x1, rcpW1);
        // y1' = y1/w1
        y1 = m_builder->CreateFMul(y1, rcpW1);
        // x2' = x2/w2
        x2 = m_builder->CreateFMul(x2, rcpW2);
        // y2' = y2/w2
        y2 = m_builder->CreateFMul(y2, rcpW2);

        // clampX0' = clamp((x0' + 1.0) / 2)
        auto clampX0 = m_builder->CreateFAdd(x0, ConstantFP::get(m_builder->getFloatTy(), 1.0));
        clampX0 = m_builder->CreateFMul(clampX0, ConstantFP::get(m_builder->getFloatTy(), 0.5));
        clampX0 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampX0,
                                                   ConstantFP::get(m_builder->getFloatTy(), 0.0)
                                               });
        clampX0 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampX0,
                                                   ConstantFP::get(m_builder->getFloatTy(), 1.0)
                                               });

        // scaledX0' = (clampX0' * xScale) * 2
        auto scaledX0 = m_builder->CreateFMul(clampX0, xsCale);
        scaledX0 = m_builder->CreateFMul(scaledX0, ConstantFP::get(m_builder->getFloatTy(), 2.0));

        // clampX1' = clamp((x1' + 1.0) / 2)
        auto clampX1 = m_builder->CreateFAdd(x1, ConstantFP::get(m_builder->getFloatTy(), 1.0));
        clampX1 = m_builder->CreateFMul(clampX1, ConstantFP::get(m_builder->getFloatTy(), 0.5));
        clampX1 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampX1,
                                                   ConstantFP::get(m_builder->getFloatTy(), 0.0)
                                               });
        clampX1 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampX1,
                                                   ConstantFP::get(m_builder->getFloatTy(), 1.0)
                                               });

        // scaledX1' = (clampX1' * xScale) * 2
        auto scaledX1 = m_builder->CreateFMul(clampX1, xsCale);
        scaledX1 = m_builder->CreateFMul(scaledX1, ConstantFP::get(m_builder->getFloatTy(), 2.0));

        // clampX2' = clamp((x2' + 1.0) / 2)
        auto clampX2 = m_builder->CreateFAdd(x2, ConstantFP::get(m_builder->getFloatTy(), 1.0));
        clampX2 = m_builder->CreateFMul(clampX2, ConstantFP::get(m_builder->getFloatTy(), 0.5));
        clampX2 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampX2,
                                                   ConstantFP::get(m_builder->getFloatTy(), 0.0)
                                               });
        clampX2 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampX2,
                                                   ConstantFP::get(m_builder->getFloatTy(), 1.0)
                                               });

        // scaledX2' = (clampX2' * xScale) * 2
        auto scaledX2 = m_builder->CreateFMul(clampX2, xsCale);
        scaledX2 = m_builder->CreateFMul(scaledX2, ConstantFP::get(m_builder->getFloatTy(), 2.0));

        // clampY0' = clamp((y0' + 1.0) / 2)
        auto clampY0 = m_builder->CreateFAdd(y0, ConstantFP::get(m_builder->getFloatTy(), 1.0));
        clampY0 = m_builder->CreateFMul(clampY0, ConstantFP::get(m_builder->getFloatTy(), 0.5));
        clampY0 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampY0,
                                                   ConstantFP::get(m_builder->getFloatTy(), 0.0)
                                               });
        clampY0 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampY0,
                                                   ConstantFP::get(m_builder->getFloatTy(), 1.0)
                                               });

        // scaledY0' = (clampY0' * yScale) * 2
        auto scaledY0 = m_builder->CreateFMul(clampY0, ysCale);
        scaledY0 = m_builder->CreateFMul(scaledY0, ConstantFP::get(m_builder->getFloatTy(), 2.0));

        // clampY1' = clamp((y1' + 1.0) / 2)
        auto clampY1 = m_builder->CreateFAdd(y1, ConstantFP::get(m_builder->getFloatTy(), 1.0));
        clampY1 = m_builder->CreateFMul(clampY1, ConstantFP::get(m_builder->getFloatTy(), 0.5));
        clampY1 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampY1,
                                                   ConstantFP::get(m_builder->getFloatTy(), 0.0)
                                               });
        clampY1 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampY1,
                                                   ConstantFP::get(m_builder->getFloatTy(), 1.0)
                                               });

        // scaledY1' = (clampY1' * yScale) * 2
        auto scaledY1 = m_builder->CreateFMul(clampY1, ysCale);
        scaledY1 = m_builder->CreateFMul(scaledY1, ConstantFP::get(m_builder->getFloatTy(), 2.0));

        // clampY2' = clamp((y2' + 1.0) / 2)
        auto clampY2 = m_builder->CreateFAdd(y2, ConstantFP::get(m_builder->getFloatTy(), 1.0));
        clampY2 = m_builder->CreateFMul(clampY2, ConstantFP::get(m_builder->getFloatTy(), 0.5));
        clampY2 = m_builder->CreateIntrinsic(Intrinsic::maxnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampY2,
                                                   ConstantFP::get(m_builder->getFloatTy(), 0.0)
                                               });
        clampY2 = m_builder->CreateIntrinsic(Intrinsic::minnum,
                                               m_builder->getFloatTy(),
                                               {
                                                   clampY2,
                                                   ConstantFP::get(m_builder->getFloatTy(), 1.0)
                                               });

        // scaledY2' = (clampY2' * yScale) * 2
        auto scaledY2 = m_builder->CreateFMul(clampY2, ysCale);
        scaledY2 = m_builder->CreateFMul(scaledY2, ConstantFP::get(m_builder->getFloatTy(), 2.0));

        // minX = roundEven(min(scaledX0', scaledX1', scaledX2') - 1/256.0)
        Value* minX =
            m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { scaledX0, scaledX1 });
        minX = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { minX, scaledX2 });
        minX = m_builder->CreateFSub(minX, ConstantFP::get(m_builder->getFloatTy(), 1/256.0));
        minX = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), minX);

        // maxX = roundEven(max(scaledX0', scaledX1', scaledX2') + 1/256.0)
        Value* maxX =
            m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { scaledX0, scaledX1 });
        maxX = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { maxX, scaledX2 });
        maxX = m_builder->CreateFAdd(maxX, ConstantFP::get(m_builder->getFloatTy(), 1 / 256.0));
        maxX = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), maxX);

        // minY = roundEven(min(scaledY0', scaledY1', scaledY2') - 1/256.0)
        Value* minY =
            m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { scaledY0, scaledY1 });
        minY = m_builder->CreateIntrinsic(Intrinsic::minnum, m_builder->getFloatTy(), { minY, scaledY2 });
        minY = m_builder->CreateFSub(minY, ConstantFP::get(m_builder->getFloatTy(), 1 / 256.0));
        minY = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), minY);

        // maxX = roundEven(max(scaledX0', scaledX1', scaledX2') + 1/256.0)
        Value* maxY =
            m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { scaledY0, scaledY1 });
        maxY = m_builder->CreateIntrinsic(Intrinsic::maxnum, m_builder->getFloatTy(), { maxY, scaledY2 });
        maxY = m_builder->CreateFAdd(maxY, ConstantFP::get(m_builder->getFloatTy(), 1 / 256.0));
        maxY = m_builder->CreateIntrinsic(Intrinsic::rint, m_builder->getFloatTy(), maxY);

        // minX == maxX
        auto minXEqMaxX = m_builder->CreateFCmpOEQ(minX, maxX);

        // minY == maxY
        auto minYEqMaxY = m_builder->CreateFCmpOEQ(minY, maxY);

        // Get cull flag
        newCullFlag = m_builder->CreateOr(minXEqMaxX, minYEqMaxY);

        m_builder->CreateBr(smallPrimFilterExitBlock);
    }

    // Construct ".smallprimfilterExit" block
    {
        m_builder->SetInsertPoint(smallPrimFilterExitBlock);

        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag, smallPrimFilterEntryBlock);
        cullFlagPhi->addIncoming(newCullFlag, smallPrimFilterCullBlock);

        m_builder->CreateRet(cullFlagPhi);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that does frustum culling.
Function* NggPrimShader::createCullDistanceCuller(
    Module* module)    // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt1Ty(),
                                     {
                                         m_builder->getInt1Ty(),    // %cullFlag
                                         m_builder->getInt32Ty(),   // %signMask0
                                         m_builder->getInt32Ty(),   // %signMask1
                                         m_builder->getInt32Ty()    // %signMask2
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingCullDistance, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadNone);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* cullFlag = argIt++;
    cullFlag->setName("cullFlag");

    Value* signMask0 = argIt++;
    signMask0->setName("signMask0");

    Value* signMask1 = argIt++;
    signMask1->setName("signMask1");

    Value* signMask2 = argIt++;
    signMask2->setName("signMask2");

    auto cullDistanceEntryBlock = createBlock(func, ".culldistanceEntry");
    auto cullDistanceCullBlock = createBlock(func, ".culldistanceCull");
    auto cullDistanceExitBlock = createBlock(func, ".culldistanceExit");

    auto savedInsertPoint = m_builder->saveIP();

    // Construct ".culldistanceEntry" block
    {
        m_builder->SetInsertPoint(cullDistanceEntryBlock);
        // If cull flag has already been TRUE, early return
        m_builder->CreateCondBr(cullFlag, cullDistanceExitBlock, cullDistanceCullBlock);
    }

    // Construct ".culldistanceCull" block
    Value* cullFlag1 = nullptr;
    {
        m_builder->SetInsertPoint(cullDistanceCullBlock);

        //
        // Cull distance culling algorithm is described as follow:
        //
        //   vertexSignMask[7:0] = [sign(ClipDistance[0])..sign(ClipDistance[7])]
        //   primSignMask = vertexSignMask0 & vertexSignMask1 & vertexSignMask2
        //   cullFlag = (primSignMask != 0)
        //
        auto signMask = m_builder->CreateAnd(signMask0, signMask1);
        signMask = m_builder->CreateAnd(signMask, signMask2);

        cullFlag1 = m_builder->CreateICmpNE(signMask, m_builder->getInt32(0));

        m_builder->CreateBr(cullDistanceExitBlock);
    }

    // Construct ".culldistanceExit" block
    {
        m_builder->SetInsertPoint(cullDistanceExitBlock);

        auto cullFlagPhi = m_builder->CreatePHI(m_builder->getInt1Ty(), 2);
        cullFlagPhi->addIncoming(cullFlag, cullDistanceEntryBlock);
        cullFlagPhi->addIncoming(cullFlag1, cullDistanceCullBlock);

        m_builder->CreateRet(cullFlagPhi);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Creates the function that fetches culling control registers.
Function* NggPrimShader::createFetchCullingRegister(
    Module* module) // [in] LLVM module
{
    auto funcTy = FunctionType::get(m_builder->getInt32Ty(),
                                     {
                                         m_builder->getInt32Ty(),  // %primShaderTableAddrLow
                                         m_builder->getInt32Ty(),  // %primShaderTableAddrHigh
                                         m_builder->getInt32Ty()   // %regOffset
                                     },
                                     false);
    auto func = Function::Create(funcTy, GlobalValue::InternalLinkage, lgcName::NggCullingFetchReg, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::ReadOnly);
    func->addFnAttr(Attribute::AlwaysInline);

    auto argIt = func->arg_begin();
    Value* primShaderTableAddrLow = argIt++;
    primShaderTableAddrLow->setName("primShaderTableAddrLow");

    Value* primShaderTableAddrHigh = argIt++;
    primShaderTableAddrHigh->setName("primShaderTableAddrHigh");

    Value* regOffset = argIt++;
    regOffset->setName("regOffset");

    BasicBlock* entryBlock = createBlock(func); // Create entry block

    auto savedInsertPoint = m_builder->saveIP();

    // Construct entry block
    {
        m_builder->SetInsertPoint(entryBlock);

        Value* primShaderTableAddr = m_builder->CreateInsertElement(
                                                    UndefValue::get(VectorType::get(Type::getInt32Ty(*m_context), 2)),
                                                    primShaderTableAddrLow,
                                                    static_cast<uint64_t>(0));

        primShaderTableAddr = m_builder->CreateInsertElement(primShaderTableAddr, primShaderTableAddrHigh, 1);

        primShaderTableAddr = m_builder->CreateBitCast(primShaderTableAddr, m_builder->getInt64Ty());

        auto primShaderTablePtrTy = PointerType::get(ArrayType::get(m_builder->getInt32Ty(), 256),
                                                      ADDR_SPACE_CONST); // [256 x i32]
        auto primShaderTablePtr = m_builder->CreateIntToPtr(primShaderTableAddr, primShaderTablePtrTy);

        // regOffset = regOffset >> 2
        regOffset = m_builder->CreateLShr(regOffset, 2); // To DWORD offset

        auto loadPtr = m_builder->CreateGEP(primShaderTablePtr, { m_builder->getInt32(0), regOffset });
        cast<Instruction>(loadPtr)->setMetadata(MetaNameUniform, MDNode::get(m_builder->getContext(), {}));

        auto regValue = m_builder->CreateAlignedLoad(loadPtr, MaybeAlign(4));
        regValue->setVolatile(true);
        regValue->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(m_builder->getContext(), {}));

        m_builder->CreateRet(regValue);
    }

    m_builder->restoreIP(savedInsertPoint);

    return func;
}

// =====================================================================================================================
// Output a subgroup ballot (always return i64 mask)
Value* NggPrimShader::doSubgroupBallot(
    Value* value) // [in] The value to do the ballot on.
{
    assert(value->getType()->isIntegerTy(1)); // Should be i1

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    value = m_builder->CreateSelect(value, m_builder->getInt32(1), m_builder->getInt32(0));

    auto inlineAsmTy = FunctionType::get(m_builder->getInt32Ty(), m_builder->getInt32Ty(), false);
    auto inlineAsm = InlineAsm::get(inlineAsmTy, "; %1", "=v,0", true);
    value = m_builder->CreateCall(inlineAsm, value);

    static const unsigned PredicateNE = 33; // 33 = predicate NE
    Value* ballot = m_builder->CreateIntrinsic(Intrinsic::amdgcn_icmp,
                                                 {
                                                     m_builder->getIntNTy(waveSize),  // Return type
                                                     m_builder->getInt32Ty()          // Argument type
                                                 },
                                                 {
                                                     value,
                                                     m_builder->getInt32(0),
                                                     m_builder->getInt32(PredicateNE)
                                                 });

    if (waveSize == 32)
        ballot = m_builder->CreateZExt(ballot, m_builder->getInt64Ty());

    return ballot;
}

// =====================================================================================================================
// Output a subgroup inclusive-add (IAdd).
Value* NggPrimShader::doSubgroupInclusiveAdd(
    Value*   value,        // [in] The value to do the inclusive-add on
    Value**  ppWwmResult)   // [out] Result in WWM section (optinal)
{
    assert(value->getType()->isIntegerTy(32)); // Should be i32

    const unsigned waveSize = m_pipelineState->getShaderWaveSize(ShaderStageGeometry);
    assert((waveSize == 32) || (waveSize == 64));

    auto inlineAsmTy = FunctionType::get(m_builder->getInt32Ty(), m_builder->getInt32Ty(), false);
    auto inlineAsm = InlineAsm::get(inlineAsmTy, "; %1", "=v,0", true);
    value = m_builder->CreateCall(inlineAsm, value);

    // Start the WWM section by setting the inactive lanes
    auto identity = m_builder->getInt32(0); // Identity for IAdd (0)
    value =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_set_inactive, m_builder->getInt32Ty(), { value, identity });

    // Do DPP operations
    enum
    {
        DppRowSr1 = 0x111,
        DppRowSr2 = 0x112,
        DppRowSr3 = 0x113,
        DppRowSr4 = 0x114,
        DppRowSr8 = 0x118,
    };

    Value* dppUpdate = doDppUpdate(identity, value, DppRowSr1, 0xF, 0xF);
    Value* result = m_builder->CreateAdd(value, dppUpdate);

    dppUpdate = doDppUpdate(identity, value, DppRowSr2, 0xF, 0xF);
    result = m_builder->CreateAdd(result, dppUpdate);

    dppUpdate = doDppUpdate(identity, value, DppRowSr3, 0xF, 0xF);
    result = m_builder->CreateAdd(result, dppUpdate);

    dppUpdate = doDppUpdate(identity, result, DppRowSr4, 0xF, 0xE);
    result = m_builder->CreateAdd(result, dppUpdate);

    dppUpdate = doDppUpdate(identity, result, DppRowSr8, 0xF, 0xC);
    result = m_builder->CreateAdd(result, dppUpdate);

    // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2)
    Value* permLane = m_builder->CreateIntrinsic(Intrinsic::amdgcn_permlanex16,
                                                   {},
                                                   {
                                                       result,
                                                       result,
                                                       m_builder->getInt32(-1),
                                                       m_builder->getInt32(-1),
                                                       m_builder->getTrue(),
                                                       m_builder->getFalse()
                                                   });

    Value* threadId = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo,
                                                   {},
                                                   {
                                                       m_builder->getInt32(-1),
                                                       m_builder->getInt32(0)
                                                   });

    if (waveSize == 64)
    {
        threadId = m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi,
                                                {},
                                                {
                                                    m_builder->getInt32(-1),
                                                    threadId
                                                });
        threadId = m_builder->CreateZExt(threadId, m_builder->getInt64Ty());
    }
    auto threadMask = m_builder->CreateShl(m_builder->getIntN(waveSize, 1), threadId);

    auto zero = m_builder->getIntN(waveSize, 0);
    auto andMask = m_builder->getIntN(waveSize, 0xFFFF0000FFFF0000);
    auto andThreadMask = m_builder->CreateAnd(threadMask, andMask);
    auto maskedPermLane =
        m_builder->CreateSelect(m_builder->CreateICmpNE(andThreadMask, zero), permLane, identity);

    result = m_builder->CreateAdd(result, maskedPermLane);

    Value* broadcast31 =
        m_builder->CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, { result, m_builder->getInt32(31)});

    andMask = m_builder->getIntN(waveSize, 0xFFFFFFFF00000000);
    andThreadMask = m_builder->CreateAnd(threadMask, andMask);
    Value* maskedBroadcast =
        m_builder->CreateSelect(m_builder->CreateICmpNE(andThreadMask, zero), broadcast31, identity);

    // Combine broadcast of 31 with the top two rows only.
    if (waveSize == 64)
        result = m_builder->CreateAdd(result, maskedBroadcast);

    if (ppWwmResult != nullptr)
    {
        // Return the result in WWM section (optional)
        *ppWwmResult = result;
    }

    // Finish the WWM section
    return m_builder->CreateIntrinsic(Intrinsic::amdgcn_wwm, m_builder->getInt32Ty(), result);
}

// =====================================================================================================================
// Does DPP update with specified parameters.
Value* NggPrimShader::doDppUpdate(
    Value*      oldValue,  // [in] Old value
    Value*      srcValue,  // [in] Source value to update with
    unsigned    dppCtrl,    // DPP controls
    unsigned    rowMask,    // Row mask
    unsigned    bankMask,   // Bank mask
    bool        boundCtrl)  // Whether to do bound control
{
    return m_builder->CreateIntrinsic(Intrinsic::amdgcn_update_dpp,
                                       m_builder->getInt32Ty(),
                                       {
                                           oldValue,
                                           srcValue,
                                           m_builder->getInt32(dppCtrl),
                                           m_builder->getInt32(rowMask),
                                           m_builder->getInt32(bankMask),
                                           m_builder->getInt1(boundCtrl)
                                       });
}

// =====================================================================================================================
// Creates a new basic block. Always insert it at the end of the parent function.
BasicBlock* NggPrimShader::createBlock(
    Function*    parent,   // [in] Parent function to which the new block belongs
    const Twine& blockName) // [in] Name of the new block
{
    return BasicBlock::Create(*m_context, blockName, parent);
}

} // lgc
