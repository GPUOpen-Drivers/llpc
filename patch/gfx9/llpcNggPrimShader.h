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
 * @file  llpcNggPrimShader.h
 * @brief LLPC header file: contains declaration of class Llpc::NggPrimShader.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/Module.h"

#include "llpc.h"
#include "llpcInternal.h"
#include "llpcNggLdsManager.h"
#include "llpcPipelineState.h"

namespace Llpc
{

struct NggControl;
class NggLdsManager;
class PipelineState;

// Represents exported data used in "exp" instruction
struct ExpData
{
    uint8_t         target;         // Export target
    uint8_t         channelMask;    // Channel mask of export value
    bool            doneFlag;       // "Done" flag
    llvm::Value*    pExpValue;      // Export value
};

// =====================================================================================================================
// Represents the manager of NGG primitive shader.
class NggPrimShader
{
public:
    NggPrimShader(PipelineState* pPipelineState);
    ~NggPrimShader();

    llvm::Function* Generate(llvm::Function* pEsEntryPoint,
                             llvm::Function* pGsEntryPoint,
                             llvm::Function* pCopyShaderEntryPoint);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(NggPrimShader);
    LLPC_DISALLOW_COPY_AND_ASSIGN(NggPrimShader);

    llvm::FunctionType* GeneratePrimShaderEntryPointType(uint64_t* pInRegMask) const;
    llvm::Function* GeneratePrimShaderEntryPoint(llvm::Module* pModule);

    void ConstructPrimShaderWithoutGs(llvm::Module* pModule);
    void ConstructPrimShaderWithGs(llvm::Module* pModule);

    void InitWaveThreadInfo(llvm::Value* pMergedGroupInfo, llvm::Value* pMergedWaveInfo);

    llvm::Value* DoCulling(llvm::Module* pModule);
    void DoParamCacheAllocRequest();
    void DoPrimitiveExport(llvm::Value* pCullFlag = nullptr);

    void DoEarlyExit(uint32_t fullyCullThreadCount, uint32_t expPosCount);

    void RunEsOrEsVariant(llvm::Module*         pModule,
                          llvm::StringRef       entryName,
                          llvm::Argument*       pSysValueStart,
                          bool                  sysValueFromLds,
                          std::vector<ExpData>* pExpDataSet,
                          llvm::BasicBlock*     pInsertAtEnd);

    llvm::Function* MutateEsToVariant(llvm::Module*         pModule,
                                      llvm::StringRef       entryName,
                                      std::vector<ExpData>& expDataSet);

    llvm::Value* RunGsVariant(llvm::Module*     pModule,
                              llvm::Argument*   pSysValueStart,
                              llvm::BasicBlock* pInsertAtEnd);

    llvm::Function* MutateGsToVariant(llvm::Module* pModule);

    void RunCopyShader(llvm::Module* pModule, llvm::BasicBlock* pInsertAtEnd);

    void ExportGsOutput(llvm::Value* pOutput,
                        uint32_t     location,
                        uint32_t     compIdx,
                        uint32_t     streamId,
                        llvm::Value* pThreadIdInWave,
                        llvm::Value* pOutVertCounter);

    llvm::Value* ImportGsOutput(llvm::Type*  pOutputTy,
                                uint32_t     location,
                                uint32_t     compIdx,
                                uint32_t     streamId,
                                llvm::Value* pThreadIdInSubgroup);

    void ProcessGsEmit(llvm::Module* pModule,
                        uint32_t     streamId,
                        llvm::Value* pThreadIdInSubgroup,
                        llvm::Value* pEmitCounterPtr,
                        llvm::Value* pOutVertCounterPtr,
                        llvm::Value* pOutPrimCounterPtr,
                        llvm::Value* pOutstandingVertCounterPtr);

    void ProcessGsCut(llvm::Module*  pModule,
                      uint32_t       streamId,
                      llvm::Value*   pThreadIdInSubgroup,
                      llvm::Value*   pEmitCounterPtr,
                      llvm::Value*   pOutVertCounterPtr,
                      llvm::Value*   pOutPrimCounterPtr,
                      llvm::Value*   pOutstandingVertCounterPtr);

    llvm::Function* CreateGsEmitHandler(llvm::Module* pModule, uint32_t streamId);
    llvm::Function* CreateGsCutHandler(llvm::Module* pModule, uint32_t streamId);

    void ReviseOutputPrimitiveData(llvm::Value* pOutPrimId, llvm::Value* pVertexIdAdjust);

    llvm::Value* ReadCompactDataFromLds(llvm::Type*       pReadDataTy,
                                        llvm::Value*      pThreadId,
                                        NggLdsRegionType  region);

    void WriteCompactDataToLds(llvm::Value*      pWriteData,
                               llvm::Value*      pThreadId,
                               NggLdsRegionType  region);

    llvm::Value* DoBackfaceCulling(llvm::Module*     pModule,
                                   llvm::Value*      pCullFlag,
                                   llvm::Value*      pVertex0,
                                   llvm::Value*      pVertex1,
                                   llvm::Value*      pVertex2);

    llvm::Value* DoFrustumCulling(llvm::Module*     pModule,
                                  llvm::Value*      pCullFlag,
                                  llvm::Value*      pVertex0,
                                  llvm::Value*      pVertex1,
                                  llvm::Value*      pVertex2);

    llvm::Value* DoBoxFilterCulling(llvm::Module*     pModule,
                                    llvm::Value*      pCullFlag,
                                    llvm::Value*      pVertex0,
                                    llvm::Value*      pVertex1,
                                    llvm::Value*      pVertex2);

    llvm::Value* DoSphereCulling(llvm::Module*     pModule,
                                 llvm::Value*      pCullFlag,
                                 llvm::Value*      pVertex0,
                                 llvm::Value*      pVertex1,
                                 llvm::Value*      pVertex2);

    llvm::Value* DoSmallPrimFilterCulling(llvm::Module*     pModule,
                                          llvm::Value*      pCullFlag,
                                          llvm::Value*      pVertex0,
                                          llvm::Value*      pVertex1,
                                          llvm::Value*      pVertex2);

    llvm::Value* DoCullDistanceCulling(llvm::Module*     pModule,
                                       llvm::Value*      pCullFlag,
                                       llvm::Value*      pSignMask0,
                                       llvm::Value*      pSignMask1,
                                       llvm::Value*      pSignMask2);

    llvm::Value* FetchCullingControlRegister(llvm::Module* pModule, uint32_t regOffset);

    llvm::Function* CreateBackfaceCuller(llvm::Module* pModule);
    llvm::Function* CreateFrustumCuller(llvm::Module* pModule);
    llvm::Function* CreateBoxFilterCuller(llvm::Module* pModule);
    llvm::Function* CreateSphereCuller(llvm::Module* pModule);
    llvm::Function* CreateSmallPrimFilterCuller(llvm::Module* pModule);
    llvm::Function* CreateCullDistanceCuller(llvm::Module* pModule);

    llvm::Function* CreateFetchCullingRegister(llvm::Module* pModule);

    llvm::Value* DoSubgroupBallot(llvm::Value* pValue);
    llvm::Value* DoSubgroupInclusiveAdd(llvm::Value* pValue, llvm::Value** ppWwmResult = nullptr);
    llvm::Value* DoDppUpdate(llvm::Value* pOldValue,
                             llvm::Value* pSrcValue,
                             uint32_t     dppCtrl,
                             uint32_t     rowMask,
                             uint32_t     bankMask,
                             bool         boundCtrl = false);

    // Checks if NGG culling operations are enabled
    bool EnableCulling() const
    {
        return (m_pNggControl->enableBackfaceCulling ||
                m_pNggControl->enableFrustumCulling ||
                m_pNggControl->enableBoxFilterCulling ||
                m_pNggControl->enableSphereCulling ||
                m_pNggControl->enableSmallPrimFilter ||
                m_pNggControl->enableCullDistanceCulling);
    }

    llvm::BasicBlock* CreateBlock(llvm::Function* pParent, const llvm::Twine& blockName = "");

    // -----------------------------------------------------------------------------------------------------------------

    static const uint32_t NullPrim = (1u << 31); // Null primitive data (invalid)

    PipelineState*  m_pPipelineState; // Pipeline state
    llvm::LLVMContext*        m_pContext;       // LLVM context
    GfxIpVersion    m_gfxIp;          // Graphics IP version info

    const NggControl* m_pNggControl;  // NGG control settings

    NggLdsManager*    m_pLdsManager;  // NGG LDS manager

    // NGG factors used for calculation (different modes use different factors)
    struct
    {
        llvm::Value*    pVertCountInSubgroup;       // Number of vertices in sub-group
        llvm::Value*    pPrimCountInSubgroup;       // Number of primitives in sub-group
        llvm::Value*    pVertCountInWave;           // Number of vertices in wave
        llvm::Value*    pPrimCountInWave;           // Number of primitives in wave

        llvm::Value*    pThreadIdInWave;            // Thread ID in wave
        llvm::Value*    pThreadIdInSubgroup;        // Thread ID in sub-group

        llvm::Value*    pWaveIdInSubgroup;          // Wave ID in sub-group

        llvm::Value*    pPrimitiveId;               // Primitive ID (for VS)

        // System values, not used in pass-through mode (SGPRs)
        llvm::Value*    pMergedGroupInfo;           // Merged group info
        llvm::Value*    pPrimShaderTableAddrLow;    // Primitive shader table address low
        llvm::Value*    pPrimShaderTableAddrHigh;   // Primitive shader table address high

        // System values (VGPRs)
        llvm::Value*    pEsGsOffsets01;             // ES-GS offset 0 and 1
        llvm::Value*    pEsGsOffsets23;             // ES-GS offset 2 and 3
        llvm::Value*    pEsGsOffsets45;             // ES-GS offset 4 and 5

    } m_nggFactor;

    bool        m_hasVs;        // Whether the pipeline has vertex shader
    bool        m_hasTcs;       // Whether the pipeline has tessellation control shader
    bool        m_hasTes;       // Whether the pipeline has tessellation evaluation shader
    bool        m_hasGs;        // Whether the pipeline has geometry shader

    std::unique_ptr<llvm::IRBuilder<>>  m_pBuilder; // LLVM IR builder
};

} // Llpc
