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
 * @file  NggPrimShader.h
 * @brief LLPC header file: contains declaration of class lgc::NggPrimShader.
 ***********************************************************************************************************************
 */
#pragma once

#include "NggLdsManager.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Module.h"

namespace lgc {

struct NggControl;
class NggLdsManager;
class PipelineState;

// Represents exported data used in "exp" instruction
struct ExpData {
  uint8_t target;        // Export target
  uint8_t channelMask;   // Channel mask of export value
  bool doneFlag;         // "Done" flag
  llvm::Value *expValue; // Export value
};

// Represents constant buffer offsets (in bytes) of viewport controls in primitive shader table.
// NOTE: The layout structure is defined by @ref Util::Abi::PrimShaderVportCb.
struct PrimShaderVportCbLookupTable {
  // Viewport transform scale
  unsigned paClVportXscale;
  unsigned paClVportXoffset;
  unsigned paClVportYscale;
  unsigned paClVportYoffset;
  // Viewport width/height
  unsigned vportWidth;
  unsigned vportHeight;
};

// Represents a collection of constant buffer offsets (in bytes) in primitive shader table.
// NOTE: The layout structure is defined by @ref Util::Abi::PrimShaderCbLayout.
struct PrimShaderCbLayoutLookupTable {
  // GS addressed used for jump from ES
  unsigned gsAddressLo;
  unsigned gsAddressHi;
  // Viewport transform controls
  unsigned paClVteCntl;
  // Float-to-fixed-vertex conversion controls
  unsigned paSuVtxCntl;
  // Clip space controls
  unsigned paClClipCntl;
  // Culling controls
  unsigned paSuScModeCntl;
  // Various frustum culling controls
  unsigned paClGbHorzClipAdj; // Horizontal adjacent culling control
  unsigned paClGbVertClipAdj; // Vertical adjacent culling control
  unsigned paClGbHorzDiscAdj; // Horizontal discard culling control
  unsigned paClGbVertDiscAdj; // Vertical discard culling control
  // Run-time handling primitive type
  unsigned vgtPrimitiveType;
  // Number of MSAA samples
  unsigned msaaNumSamples;
  // Render state
  unsigned primitiveRestartEnable;
  unsigned primitiveRestartIndex;
  unsigned matchAllBits;
  unsigned enableConservativeRasterization;
  // Viewport controls
  PrimShaderVportCbLookupTable vportControls[Util::Abi::MaxViewports];
};

// =====================================================================================================================
// Represents the manager of NGG primitive shader.
class NggPrimShader {
public:
  NggPrimShader(PipelineState *pipelineState);
  ~NggPrimShader();

  llvm::Function *generate(llvm::Function *esEntryPoint, llvm::Function *gsEntryPoint,
                           llvm::Function *copyShaderEntryPoint);

private:
  NggPrimShader() = delete;
  NggPrimShader(const NggPrimShader &) = delete;
  NggPrimShader &operator=(const NggPrimShader &) = delete;

  llvm::FunctionType *generatePrimShaderEntryPointType(uint64_t *inRegMask) const;
  llvm::Function *generatePrimShaderEntryPoint(llvm::Module *module);

  void buildPrimShaderCbLayoutLookupTable();

  void constructPrimShaderWithoutGs(llvm::Module *module);
  void constructPrimShaderWithGs(llvm::Module *module);

  void initWaveThreadInfo(llvm::Value *mergedGroupInfo, llvm::Value *mergedWaveInfo);

  llvm::Value *doCulling(llvm::Module *module);
  void doParamCacheAllocRequest();
  void doPrimitiveExport(llvm::Value *cullFlag = nullptr);

  void doEarlyExit(unsigned fullyCullThreadCount, unsigned expPosCount);

  void runEsOrEsVariant(llvm::Module *module, llvm::StringRef entryName, llvm::Argument *sysValueStart,
                        bool sysValueFromLds, std::vector<ExpData> *expDataSet, llvm::BasicBlock *insertAtEnd);

  llvm::Function *mutateEsToVariant(llvm::Module *module, llvm::StringRef entryName, std::vector<ExpData> &expDataSet);

  llvm::Value *runGsVariant(llvm::Module *module, llvm::Argument *sysValueStart, llvm::BasicBlock *insertAtEnd);

  llvm::Function *mutateGsToVariant(llvm::Module *module);

  void runCopyShader(llvm::Module *module, llvm::BasicBlock *insertAtEnd);

  void exportGsOutput(llvm::Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                      llvm::Value *threadIdInSubgroup, llvm::Value *outVertCounter);

  llvm::Value *importGsOutput(llvm::Type *outputTy, unsigned location, unsigned compIdx, unsigned streamId,
                              llvm::Value *threadIdInSubgroup);

  void processGsEmit(llvm::Module *module, unsigned streamId, llvm::Value *threadIdInSubgroup,
                     llvm::Value *emitCounterPtr, llvm::Value *outVertCounterPtr, llvm::Value *outPrimCounterPtr,
                     llvm::Value *outstandingVertCounterPtr, llvm::Value *flipVertOrderPtr);

  void processGsCut(llvm::Module *module, unsigned streamId, llvm::Value *threadIdInSubgroup,
                    llvm::Value *emitCounterPtr, llvm::Value *outVertCounterPtr, llvm::Value *outPrimCounterPtr,
                    llvm::Value *outstandingVertCounterPtr, llvm::Value *flipVertOrderPtr);

  llvm::Function *createGsEmitHandler(llvm::Module *module, unsigned streamId);
  llvm::Function *createGsCutHandler(llvm::Module *module, unsigned streamId);

  void reviseOutputPrimitiveData(llvm::Value *outPrimId, llvm::Value *vertexIdAdjust);

  llvm::Value *readPerThreadDataFromLds(llvm::Type *readDataTy, llvm::Value *threadId, NggLdsRegionType region);

  void writePerThreadDataToLds(llvm::Value *writeData, llvm::Value *threadId, NggLdsRegionType region);

  llvm::Value *doBackfaceCulling(llvm::Module *module, llvm::Value *cullFlag, llvm::Value *vertex0,
                                 llvm::Value *vertex1, llvm::Value *vertex2);

  llvm::Value *doFrustumCulling(llvm::Module *module, llvm::Value *cullFlag, llvm::Value *vertex0, llvm::Value *vertex1,
                                llvm::Value *vertex2);

  llvm::Value *doBoxFilterCulling(llvm::Module *module, llvm::Value *cullFlag, llvm::Value *vertex0,
                                  llvm::Value *vertex1, llvm::Value *vertex2);

  llvm::Value *doSphereCulling(llvm::Module *module, llvm::Value *cullFlag, llvm::Value *vertex0, llvm::Value *vertex1,
                               llvm::Value *vertex2);

  llvm::Value *doSmallPrimFilterCulling(llvm::Module *module, llvm::Value *cullFlag, llvm::Value *vertex0,
                                        llvm::Value *vertex1, llvm::Value *vertex2);

  llvm::Value *doCullDistanceCulling(llvm::Module *module, llvm::Value *cullFlag, llvm::Value *signMask0,
                                     llvm::Value *signMask1, llvm::Value *signMask2);

  llvm::Value *fetchCullingControlRegister(llvm::Module *module, unsigned regOffset);

  llvm::Function *createBackfaceCuller(llvm::Module *module);
  llvm::Function *createFrustumCuller(llvm::Module *module);
  llvm::Function *createBoxFilterCuller(llvm::Module *module);
  llvm::Function *createSphereCuller(llvm::Module *module);
  llvm::Function *createSmallPrimFilterCuller(llvm::Module *module);
  llvm::Function *createCullDistanceCuller(llvm::Module *module);

  llvm::Function *createFetchCullingRegister(llvm::Module *module);

  llvm::Value *doSubgroupBallot(llvm::Value *value);
  llvm::Value *doSubgroupInclusiveAdd(llvm::Value *value, llvm::Value **ppWwmResult = nullptr);
  llvm::Value *doDppUpdate(llvm::Value *oldValue, llvm::Value *srcValue, unsigned dppCtrl, unsigned rowMask,
                           unsigned bankMask, bool boundCtrl = false);

  llvm::Value *fetchVertexPositionData(llvm::Value *vertexId);
  llvm::Value *fetchCullDistanceSignMask(llvm::Value *vertexId);

  // Checks if NGG culling operations are enabled
  bool enableCulling() const {
    return m_nggControl->enableBackfaceCulling || m_nggControl->enableFrustumCulling ||
           m_nggControl->enableBoxFilterCulling || m_nggControl->enableSphereCulling ||
           m_nggControl->enableSmallPrimFilter || m_nggControl->enableCullDistanceCulling;
  }

  llvm::BasicBlock *createBlock(llvm::Function *parent, const llvm::Twine &blockName = "");

  static const unsigned NullPrim = (1u << 31); // Null primitive data (invalid)

  PipelineState *m_pipelineState; // Pipeline state
  llvm::LLVMContext *m_context;   // LLVM context
  GfxIpVersion m_gfxIp;           // Graphics IP version info

  const NggControl *m_nggControl; // NGG control settings

  PrimShaderCbLayoutLookupTable m_cbLayoutTable; // Layout lookup table of primitive shader constant buffer

  NggLdsManager *m_ldsManager; // NGG LDS manager

  // NGG factors used for calculation (different modes use different factors)
  struct {
    llvm::Value *vertCountInSubgroup; // Number of vertices in sub-group
    llvm::Value *primCountInSubgroup; // Number of primitives in sub-group
    llvm::Value *vertCountInWave;     // Number of vertices in wave
    llvm::Value *primCountInWave;     // Number of primitives in wave

    llvm::Value *threadIdInWave;     // Thread ID in wave
    llvm::Value *threadIdInSubgroup; // Thread ID in sub-group

    llvm::Value *waveIdInSubgroup; // Wave ID in sub-group

    llvm::Value *primitiveId; // Primitive ID (for VS)

    // System values, not used in pass-through mode (SGPRs)
    llvm::Value *mergedGroupInfo;         // Merged group info
    llvm::Value *primShaderTableAddrLow;  // Primitive shader table address low
    llvm::Value *primShaderTableAddrHigh; // Primitive shader table address high

    // System values (VGPRs)
    llvm::Value *esGsOffsets01; // ES-GS offset 0 and 1
    llvm::Value *esGsOffsets23; // ES-GS offset 2 and 3
    llvm::Value *esGsOffsets45; // ES-GS offset 4 and 5

  } m_nggFactor;

  bool m_hasVs;  // Whether the pipeline has vertex shader
  bool m_hasTcs; // Whether the pipeline has tessellation control shader
  bool m_hasTes; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;  // Whether the pipeline has geometry shader

  std::unique_ptr<llvm::IRBuilder<>> m_builder; // LLVM IR builder
};

} // namespace lgc
