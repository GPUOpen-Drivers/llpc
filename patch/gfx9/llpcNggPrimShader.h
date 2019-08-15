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

namespace Llpc
{

class Context;
class NggLdsManager;

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
    NggPrimShader(Context* pContext);
    ~NggPrimShader();

    llvm::Function* Generate(llvm::Function* pEsEntryPoint, llvm::Function* pGsEntryPoint);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(NggPrimShader);
    LLPC_DISALLOW_COPY_AND_ASSIGN(NggPrimShader);

    llvm::FunctionType* GeneratePrimShaderEntryPointType(uint64_t* pInRegMask) const;
    llvm::Function* GeneratePrimShaderEntryPoint(llvm::Module* pModule);

    llvm::Value* DoCulling(llvm::Module* pModule, llvm::BasicBlock* pInsertAtEnd);
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
                                   llvm::Value*      pVertex2,
                                   llvm::BasicBlock* pInsertAtEnd);

    llvm::Value* DoFrustumCulling(llvm::Module*     pModule,
                                  llvm::Value*      pCullFlag,
                                  llvm::Value*      pVertex0,
                                  llvm::Value*      pVertex1,
                                  llvm::Value*      pVertex2,
                                  llvm::BasicBlock* pInsertAtEnd);

    llvm::Value* DoBoxFilterCulling(llvm::Module*     pModule,
                                    llvm::Value*      pCullFlag,
                                    llvm::Value*      pVertex0,
                                    llvm::Value*      pVertex1,
                                    llvm::Value*      pVertex2,
                                    llvm::BasicBlock* pInsertAtEnd);

    llvm::Value* DoSphereCulling(llvm::Module*     pModule,
                                 llvm::Value*      pCullFlag,
                                 llvm::Value*      pVertex0,
                                 llvm::Value*      pVertex1,
                                 llvm::Value*      pVertex2,
                                 llvm::BasicBlock* pInsertAtEnd);

    llvm::Value* DoSmallPrimFilterCulling(llvm::Module*     pModule,
                                          llvm::Value*      pCullFlag,
                                          llvm::Value*      pVertex0,
                                          llvm::Value*      pVertex1,
                                          llvm::Value*      pVertex2,
                                          llvm::BasicBlock* pInsertAtEnd);

    llvm::Value* DoCullDistanceCulling(llvm::Module*     pModule,
                                       llvm::Value*      pCullFlag,
                                       llvm::Value*      pSignMask0,
                                       llvm::Value*      pSignMask1,
                                       llvm::Value*      pSignMask2,
                                       llvm::BasicBlock* pInsertAtEnd);

    llvm::Value* FetchCullingControlRegister(llvm::Module*     pModule,
                                             uint32_t          regOffset,
                                             llvm::BasicBlock* pInsertAtEnd);

    void CreateBackfaceCuller(llvm::Module* pModule);
    void CreateFrustumCuller(llvm::Module* pModule);
    void CreateBoxFilterCuller(llvm::Module* pModule);
    void CreateSphereCuller(llvm::Module* pModule);
    void CreateSmallPrimFilterCuller(llvm::Module* pModule);
    void CreateCullDistanceCuller(llvm::Module* pModule);

    void CreateFetchCullingRegister(llvm::Module* pModule);

    llvm::Value* DoSubgroupBallot(llvm::Value* pValue);

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

    // -----------------------------------------------------------------------------------------------------------------

    Context*        m_pContext; // LLPC context
    GfxIpVersion    m_gfxIp;    // Graphics IP version info

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
