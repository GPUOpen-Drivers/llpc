/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchInOutImportExport.h
 * @brief LLPC header file: contains declaration of class lgc::PatchInOutImportExport.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include "llpcIntrinsDefs.h"
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"
#include "llpcPipelineState.h"
#include "llpcSystemValues.h"
#include "llpcTargetInfo.h"

#include <set>

namespace lgc
{

class FragColorExport;
class VertexFetch;

// =====================================================================================================================
// Represents the pass of LLVM patching opertions for input import and output export.
class PatchInOutImportExport:
    public Patch,
    public llvm::InstVisitor<PatchInOutImportExport>
{
public:
    PatchInOutImportExport();
    ~PatchInOutImportExport();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addPreserved<PipelineShaders>();
    }

    bool runOnModule(llvm::Module& module) override;
    void visitCallInst(llvm::CallInst& callInst);
    void visitReturnInst(llvm::ReturnInst& retInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    PatchInOutImportExport(const PatchInOutImportExport&) = delete;
    PatchInOutImportExport& operator=(const PatchInOutImportExport&) = delete;

    void InitPerShader();

    void ProcessShader();

    llvm::Value* PatchVsGenericInputImport(llvm::Type*        pInputTy,
                                           uint32_t           location,
                                           uint32_t           compIdx,
                                           llvm::Instruction* pInsertPos);
    llvm::Value* PatchTcsGenericInputImport(llvm::Type*        pInputTy,
                                            uint32_t           location,
                                            llvm::Value*       pLocOffset,
                                            llvm::Value*       pCompIdx,
                                            llvm::Value*       pVertexIdx,
                                            llvm::Instruction* pInsertPos);
    llvm::Value* PatchTesGenericInputImport(llvm::Type*        pInputTy,
                                            uint32_t           location,
                                            llvm::Value*       pLocOffset,
                                            llvm::Value*       pCompIdx,
                                            llvm::Value*       pVertexIdx,
                                            llvm::Instruction* pInsertPos);
    llvm::Value* PatchGsGenericInputImport(llvm::Type*        pInputTy,
                                           uint32_t           location,
                                           uint32_t           compIdx,
                                           llvm::Value*       pVertexIdx,
                                           llvm::Instruction* pInsertPos);
    llvm::Value* PatchFsGenericInputImport(llvm::Type*        pInputTy,
                                           uint32_t           location,
                                           Value*             pLocOffset,
                                           Value*             pCompIdx,
                                           Value*             pAuxInterpValue,
                                           uint32_t           interpMode,
                                           uint32_t           interpLoc,
                                           llvm::Instruction* pInsertPos);

    llvm::Value* PatchTcsGenericOutputImport(llvm::Type*        pOutputTy,
                                             uint32_t           location,
                                             llvm::Value*       pLocOffset,
                                             llvm::Value*       pCompIdx,
                                             llvm::Value*       pVertexIdx,
                                             llvm::Instruction* pInsertPos);

    void PatchVsGenericOutputExport(llvm::Value*       pOutput,
                                    uint32_t           location,
                                    uint32_t           compIdx,
                                    llvm::Instruction* pInsertPos);
    void PatchTcsGenericOutputExport(llvm::Value*       pOutput,
                                     uint32_t           location,
                                     llvm::Value*       pLocOffset,
                                     llvm::Value*       pCompIdx,
                                     llvm::Value*       pVertexIdx,
                                     llvm::Instruction* pInsertPos);
    void PatchTesGenericOutputExport(llvm::Value*       pOutput,
                                     uint32_t           location,
                                     uint32_t           compIdx,
                                     llvm::Instruction* pInsertPos);
    void PatchGsGenericOutputExport(llvm::Value*       pOutput,
                                    uint32_t           location,
                                    uint32_t           compIdx,
                                    uint32_t           streamId,
                                    llvm::Instruction* pInsertPos);
    void PatchFsGenericOutputExport(llvm::Value*       pOutput,
                                    uint32_t           location,
                                    uint32_t           compIdx,
                                    llvm::Instruction* pInsertPos);

    llvm::Value* PatchVsBuiltInInputImport(llvm::Type* pInputTy, uint32_t builtInId, llvm::Instruction* pInsertPos);
    llvm::Value* PatchTcsBuiltInInputImport(llvm::Type*        pInputTy,
                                            uint32_t           builtInId,
                                            llvm::Value*       pElemIdx,
                                            llvm::Value*       pVertexIdx,
                                            llvm::Instruction* pInsertPos);
    llvm::Value* PatchTesBuiltInInputImport(llvm::Type*        pInputTy,
                                            uint32_t           builtInId,
                                            llvm::Value*       pElemIdx,
                                            llvm::Value*       pVertexIdx,
                                            llvm::Instruction* pInsertPos);
    llvm::Value* PatchGsBuiltInInputImport(llvm::Type*        pInputTy,
                                           uint32_t           builtInId,
                                           llvm::Value*       pVertexIdx,
                                           llvm::Instruction* pInsertPos);
    llvm::Value* PatchFsBuiltInInputImport(llvm::Type*        pInputTy,
                                           uint32_t           builtInId,
                                           Value*             pSampleId,
                                           llvm::Instruction* pInsertPos);
    llvm::Value* GetSamplePosOffset(llvm::Type* pInputTy, llvm::Value* pSampleId, llvm::Instruction* pInsertPos);
    llvm::Value* GetSamplePosition(llvm::Type* pInputTy, llvm::Instruction* pInsertPos);
    llvm::Value* PatchCsBuiltInInputImport(llvm::Type* pInputTy, uint32_t builtInId, llvm::Instruction* pInsertPos);
    llvm::Value* GetGlobalInvocationId(llvm::Type* pInputTy, llvm::Instruction* pInsertPos);
    llvm::Value* GetLocalInvocationIndex(llvm::Type* pInputTy, llvm::Instruction* pInsertPos);
    llvm::Value* GetSubgroupId(llvm::Type* pInputTy, llvm::Instruction* pInsertPos);

    llvm::Value* PatchTcsBuiltInOutputImport(llvm::Type*        pOutputTy,
                                             uint32_t           builtInId,
                                             llvm::Value*       pElemIdx,
                                             llvm::Value*       pVertexIdx,
                                             llvm::Instruction* pInsertPos);

    void PatchVsBuiltInOutputExport(llvm::Value* pOutput, uint32_t builtInId, llvm::Instruction* pInsertPos);
    void PatchTcsBuiltInOutputExport(llvm::Value*       pOutput,
                                     uint32_t           builtInId,
                                     llvm::Value*       pElemIdx,
                                     llvm::Value*       pVertexIdx,
                                     llvm::Instruction* pInsertPos);
    void PatchTesBuiltInOutputExport(llvm::Value* pOutput, uint32_t builtInId, llvm::Instruction* pInsertPos);
    void PatchGsBuiltInOutputExport(llvm::Value*       pOutput,
                                    uint32_t           builtInId,
                                    uint32_t           streamId,
                                    llvm::Instruction* pInsertPos);
    void PatchFsBuiltInOutputExport(llvm::Value* pOutput, uint32_t builtInId, llvm::Instruction* pInsertPos);

    void PatchCopyShaderGenericOutputExport(llvm::Value* pOutput, uint32_t location, llvm::Instruction* pInsertPos);
    void PatchCopyShaderBuiltInOutputExport(llvm::Value* pOutput, uint32_t builtInId, llvm::Instruction* pInsertPos);

    void PatchXfbOutputExport(llvm::Value*       pOutput,
                              uint32_t           xfbBuffer,
                              uint32_t           xfbOffset,
                              uint32_t           locOffset,
                              llvm::Instruction* pInsertPos);

    void StoreValueToStreamOutBuffer(llvm::Value*       pStoreValue,
                                     uint32_t           xfbBuffer,
                                     uint32_t           xfbOffset,
                                     uint32_t           xfbStride,
                                     llvm::Value*       pStreamOutBufDesc,
                                     llvm::Instruction* pInsertPos);

    void CreateStreamOutBufferStoreFunction(llvm::Value*  pStoreValue, uint32_t xfbStrde, std::string& funcName);

    uint32_t CombineBufferStore(const std::vector<llvm::Value*>& storeValues,
                                  uint32_t                         startIdx,
                                  uint32_t                         valueOffset,
                                  llvm::Value*                     pBufDesc,
                                  llvm::Value*                     pStoreOffset,
                                  llvm::Value*                     pBufBase,
                                  CoherentFlag                     coherent,
                                  llvm::Instruction*               pInsertPos);

    uint32_t CombineBufferLoad(std::vector<llvm::Value*>& loadValues,
                                 uint32_t                   startIdx,
                                 llvm::Value*               pBufDesc,
                                 llvm::Value*               pLoadOffset,
                                 llvm::Value*               pBufBase,
                                 CoherentFlag               coherent,
                                 llvm::Instruction*         pInsertPos);

    void StoreValueToEsGsRing(llvm::Value*        pStoreValue,
                              uint32_t            location,
                              uint32_t            compIdx,
                              llvm::Instruction*  pInsertPos);

    llvm::Value* LoadValueFromEsGsRing(llvm::Type*         pLoadType,
                                       uint32_t            location,
                                       uint32_t            compIdx,
                                       llvm::Value*        pVertexIdx,
                                       llvm::Instruction*  pInsertPos);

    void StoreValueToGsVsRing(llvm::Value*        pStoreValue,
                              uint32_t            location,
                              uint32_t            compIdx,
                              uint32_t            streamId,
                              llvm::Instruction*  pInsertPos);

    llvm::Value* CalcEsGsRingOffsetForOutput(uint32_t           location,
                                             uint32_t           compIdx,
                                             llvm::Value*       pEsGsOffset,
                                             llvm::Instruction* pInsertPos);

    llvm::Value* CalcEsGsRingOffsetForInput(uint32_t           location,
                                            uint32_t           compIdx,
                                            llvm::Value*       pVertexIdx,
                                            llvm::Instruction* pInsertPos);

    llvm::Value* CalcGsVsRingOffsetForOutput(uint32_t           location,
                                             uint32_t           compIdx,
                                             uint32_t           streamId,
                                             llvm::Value*       pVertexIdx,
                                             llvm::Value*       pGsVsOffset,
                                             llvm::Instruction* pInsertPos);

    llvm::Value* ReadValueFromLds(bool isOutput, llvm::Type* pReadTy, llvm::Value* pLdsOffset, llvm::Instruction* pInsertPos);
    void WriteValueToLds(llvm::Value* pWriteValue, llvm::Value* pLdsOffset, llvm::Instruction* pInsertPos);

    llvm::Value* CalcTessFactorOffset(bool isOuter, llvm::Value* pElemIdx, llvm::Instruction* pInsertPos);

    void StoreTessFactorToBuffer(const std::vector<llvm::Value*>& tessFactors,
                                 llvm::Value*                     pTessFactorOffset,
                                 llvm::Instruction*               pInsertPos);

    void CreateTessBufferStoreFunction();

    uint32_t CalcPatchCountPerThreadGroup(uint32_t inVertexCount,
                                          uint32_t inVertexStride,
                                          uint32_t outVertexCount,
                                          uint32_t outVertexStride,
                                          uint32_t patchConstCount,
                                          uint32_t tessFactorStride) const;

    llvm::Value* CalcLdsOffsetForVsOutput(Type*              pOutputTy,
                                          uint32_t           location,
                                          uint32_t           compIdx,
                                          llvm::Instruction* pInsertPos);

    llvm::Value* CalcLdsOffsetForTcsInput(Type*              pInputTy,
                                          uint32_t           location,
                                          llvm::Value*       pLocOffset,
                                          llvm::Value*       pCompIdx,
                                          llvm::Value*       pVertexIdx,
                                          llvm::Instruction* pInsertPos);

    llvm::Value* CalcLdsOffsetForTcsOutput(Type*              pOutputTy,
                                           uint32_t           location,
                                           llvm::Value*       pLocOffset,
                                           llvm::Value*       pCompIdx,
                                           llvm::Value*       pVertexIdx,
                                           llvm::Instruction* pInsertPos);

    llvm::Value* CalcLdsOffsetForTesInput(Type*              pInputTy,
                                          uint32_t           location,
                                          llvm::Value*       pLocOffset,
                                          llvm::Value*       pCompIdx,
                                          llvm::Value*       pVertexIdx,
                                          llvm::Instruction* pInsertPos);

    void AddExportInstForGenericOutput(llvm::Value*       pOutput,
                                       uint32_t           location,
                                       uint32_t           compIdx,
                                       llvm::Instruction* pInsertPos);
    void AddExportInstForBuiltInOutput(llvm::Value* pOutput, uint32_t builtInId, llvm::Instruction* pInsertPos);

    llvm::Value* AdjustCentroidIJ(llvm::Value* pCentroidIJ, llvm::Value* pCenterIJ, llvm::Instruction* pInsertPos);

    llvm::Value* GetSubgroupLocalInvocationId(llvm::Instruction* pInsertPos);

    WorkgroupLayout CalculateWorkgroupLayout();
    llvm::Value* ReconfigWorkgroup(llvm::Value* pLocalInvocationId, llvm::Instruction* pInsertPos);
    llvm::Value* GetWorkgroupSize();
    llvm::Value* GetInLocalInvocationId(llvm::Instruction* pInsertPos);

    // -----------------------------------------------------------------------------------------------------------------

    GfxIpVersion            m_gfxIp;                    // Graphics IP version info
    PipelineSystemValues    m_pipelineSysValues;        // Cache of ShaderSystemValues objects, one per shader stage

    VertexFetch*            m_pVertexFetch;             // Vertex fetch manager
    FragColorExport*        m_pFragColorExport;         // Fragment color export manager

    llvm::CallInst*         m_pLastExport;              // Last "export" intrinsic for which "done" flag is valid

    llvm::Value*            m_pClipDistance;            // Correspond to "out float gl_ClipDistance[]"
    llvm::Value*            m_pCullDistance;            // Correspond to "out float gl_CullDistance[]"
    llvm::Value*            m_pPrimitiveId;             // Correspond to "out int gl_PrimitiveID"
    // NOTE: gl_FragDepth, gl_FragStencilRef and gl_SampleMask[] are exported at the same time with one "EXP"
    // instruction. Thus, the export is delayed.
    llvm::Value*            m_pFragDepth;               // Correspond to "out float gl_FragDepth"
    llvm::Value*            m_pFragStencilRef;          // Correspond to "out int gl_FragStencilRef"
    llvm::Value*            m_pSampleMask;              // Correspond to "out int gl_SampleMask[]"
    // NOTE: For GFX9, gl_ViewportIndex and gl_Layer are packed with one channel (gl_ViewpoertInex is 16-bit high part
    // and gl_Layer is 16-bit low part). Thus, the export is delayed with them merged together.
    llvm::Value*            m_pViewportIndex;           // Correspond to "out int gl_ViewportIndex"
    llvm::Value*            m_pLayer;                   // Correspond to "out int gl_Layer"

    bool                    m_hasTs;                    // Whether the pipeline has tessellation shaders

    bool                    m_hasGs;                    // Whether the pipeline has geometry shader

    llvm::GlobalVariable*   m_pLds;                     // Global variable to model LDS
    llvm::Value*            m_pThreadId;                // Thread ID

    std::vector<Value*>     m_expFragColors[MaxColorTargets]; // Exported fragment colors
    std::vector<llvm::CallInst*> m_importCalls; // List of "call" instructions to import inputs
    std::vector<llvm::CallInst*> m_exportCalls; // List of "call" instructions to export outputs
    PipelineState*          m_pPipelineState = nullptr; // Pipeline state from PipelineStateWrapper pass

    std::set<uint32_t>       m_expLocs; // The locations that already have an export instruction for the vertex shader.
};

} // lgc
