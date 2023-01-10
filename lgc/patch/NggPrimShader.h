/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/Module.h"

namespace lgc {

struct NggControl;
class NggLdsManager;
class PipelineState;

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

// Represents the layout structure of an item of vertex cull info (this acts as ES-GS ring item from HW's perspective)
struct VertexCullInfo {
  //
  // Vertex transform feedback outputs
  //
  unsigned xfbOutputs[4];
  //
  // Vertex cull data
  //
  unsigned cullDistanceSignMask;
  //
  // Vertex cull result
  //
  unsigned drawFlag;
  //
  // Vertex compaction info (vertex compaction only, must in the end of this structure)
  //
  unsigned compactThreadId;
  union {
    struct {
      unsigned vertexId;
      unsigned instanceId;
      unsigned primitiveId;
    } vs;
    struct {
      float tessCoordX;
      float tessCoordY;
      unsigned patchId;
      unsigned relPatchId;
    } tes;
  };
};

// Represents a collection of LDS offsets (in bytes) within an item of vertex cull info.
struct VertexCullInfoOffsets {
  //
  // Vertex transform feedback outputs
  //
  unsigned xfbOutputs;
  //
  // Vertex cull data
  //
  unsigned cullDistanceSignMask;
  //
  // Vertex cull result
  //
  unsigned drawFlag;
  //
  // Vertex compaction info
  //
  unsigned compactThreadId;
  // VS
  unsigned vertexId;
  unsigned instanceId;
  unsigned primitiveId;
  // TES
  unsigned tessCoordX;
  unsigned tessCoordY;
  unsigned patchId;
  unsigned relPatchId;
};

// Represents export info of a transform feedback output
struct XfbOutputExport {
  unsigned xfbBuffer;   // Transform feedback buffer
  unsigned xfbOffset;   // Transform feedback offset
  unsigned numElements; // Number of output elements, valid range is [1,4]
  bool is16bit;         // Whether the output is 16-bit
  struct {
    unsigned streamId; // Output stream ID
    unsigned loc;      // Output location
  } locInfo;           // Output location info in GS-VS ring (just for GS)
};

// =====================================================================================================================
// Represents the manager of NGG primitive shader.
class NggPrimShader {
public:
  NggPrimShader(PipelineState *pipelineState);
  ~NggPrimShader();

  static unsigned calcEsGsRingItemSize(PipelineState *pipelineState);

  llvm::Function *generate(llvm::Function *esEntryPoint, llvm::Function *gsEntryPoint,
                           llvm::Function *copyShaderEntryPoint);

private:
  NggPrimShader() = delete;
  NggPrimShader(const NggPrimShader &) = delete;
  NggPrimShader &operator=(const NggPrimShader &) = delete;

  static unsigned calcVertexCullInfoSizeAndOffsets(PipelineState *pipelineState,
                                                   VertexCullInfoOffsets &vertCullInfoOffsets);

  llvm::FunctionType *generatePrimShaderEntryPointType(llvm::Module *module, uint64_t *inRegMask);
  llvm::Function *generatePrimShaderEntryPoint(llvm::Module *module);

  void buildPrimShaderCbLayoutLookupTable();

  void buildPassthroughPrimShader(llvm::Function *entryPoint);
  void buildPrimShader(llvm::Function *entryPoint);
  void buildPrimShaderWithGs(llvm::Function *entryPoint);

  void initWaveThreadInfo(llvm::Value *mergedGroupInfo, llvm::Value *mergedWaveInfo);

  llvm::Value *doCulling(llvm::Module *module, llvm::Value *vertexId0, llvm::Value *vertexId1, llvm::Value *vertexId2);
  void doParamCacheAllocRequest();
  void doPrimitiveExportWithoutGs(llvm::Value *cullFlag = nullptr);
  void doPrimitiveExportWithGs(llvm::Value *vertexId);

  void doEarlyExit(unsigned fullyCulledExportCount);

  void runEs(llvm::Module *module, llvm::Argument *sysValueStart);
  llvm::Value *runEsPartial(llvm::Module *module, llvm::Argument *sysValueStart, llvm::Value *position = nullptr);

  void splitEs(llvm::Module *module);

  void runGs(llvm::Module *module, llvm::Argument *sysValueStart);

  llvm::Function *mutateGs(llvm::Module *module);

  void runCopyShader(llvm::Module *module, llvm::Argument *sysValueStart);

  llvm::Function *mutateCopyShader(llvm::Module *module);

  void exportGsOutput(llvm::Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                      llvm::Value *threadIdInSubgroup, llvm::Value *emitVerts);

  llvm::Value *importGsOutput(llvm::Type *outputTy, unsigned location, unsigned streamId, llvm::Value *vertexOffset);

  void processGsEmit(llvm::Module *module, unsigned streamId, llvm::Value *threadIdInSubgroup,
                     llvm::Value *emitVertsPtr, llvm::Value *outVertsPtr);

  void processGsCut(llvm::Module *module, unsigned streamId, llvm::Value *outVertsPtr);

  llvm::Function *createGsEmitHandler(llvm::Module *module);
  llvm::Function *createGsCutHandler(llvm::Module *module);

  llvm::Value *readPerThreadDataFromLds(llvm::Type *readDataTy, llvm::Value *threadId, NggLdsRegionType region,
                                        unsigned offsetInRegion = 0, bool useDs128 = false);
  void writePerThreadDataToLds(llvm::Value *writeData, llvm::Value *threadId, NggLdsRegionType region,
                               unsigned offsetInRegion = 0, bool useDs128 = false);

  llvm::Value *readVertexCullInfoFromLds(llvm::Type *readDataTy, llvm::Value *vertexItemOffset, unsigned dataOffset);
  void writeVertexCullInfoToLds(llvm::Value *writeData, llvm::Value *vertexItemOffset, unsigned dataOffset);

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

  llvm::Value *fetchVertexPositionData(llvm::Value *vertexId);
  llvm::Value *fetchCullDistanceSignMask(llvm::Value *vertexId);
  llvm::Value *calcVertexItemOffset(unsigned streamId, llvm::Value *vertexId);

  void processVertexAttribExport(llvm::Function *&targetFunc);

  void processXfbOutputExport(llvm::Module *module, llvm::Argument *sysValueStart);
  void processGsXfbOutputExport(llvm::Module *module, llvm::Argument *sysValueStart);
  llvm::Value *fetchXfbOutput(llvm::Module *module, llvm::Argument *sysValueStart,
                              llvm::SmallVector<XfbOutputExport, 32> &xfbOutputExports);

  llvm::Value *readXfbOutputFromLds(llvm::Type *readDataTy, llvm::Value *vertexId, unsigned outputIndex);
  void writeXfbOutputToLds(llvm::Value *writeData, llvm::Value *vertexId, unsigned outputIndex);

  // Checks if NGG culling operations are enabled
  bool enableCulling() const {
    return m_nggControl->enableBackfaceCulling || m_nggControl->enableFrustumCulling ||
           m_nggControl->enableBoxFilterCulling || m_nggControl->enableSphereCulling ||
           m_nggControl->enableSmallPrimFilter || m_nggControl->enableCullDistanceCulling;
  }

  llvm::BasicBlock *createBlock(llvm::Function *parent, const llvm::Twine &blockName = "");
  llvm::Value *createUBfe(llvm::Value *value, unsigned offset, unsigned count);
  void createFenceAndBarrier();

  static const unsigned NullPrim = (1u << 31); // Null primitive data (invalid)

  PipelineState *m_pipelineState = nullptr; // Pipeline state
  GfxIpVersion m_gfxIp;                     // Graphics IP version info

  const NggControl *m_nggControl = nullptr; // NGG control settings

  NggLdsManager *m_ldsManager = nullptr; // NGG LDS manager

  // NGG factors used for calculation (different modes use different factors)
  struct {
    llvm::Value *vertCountInSubgroup; // Number of vertices in sub-group
    llvm::Value *primCountInSubgroup; // Number of primitives in sub-group
    llvm::Value *vertCountInWave;     // Number of vertices in wave
    llvm::Value *primCountInWave;     // Number of primitives in wave

    llvm::Value *threadIdInWave;     // Thread ID in wave
    llvm::Value *threadIdInSubgroup; // Thread ID in sub-group

    llvm::Value *waveIdInSubgroup; // Wave ID in sub-group
    llvm::Value *orderedWaveId;    // Ordered wave ID

    llvm::Value *primitiveId;   // Primitive ID (for VS)
    llvm::Value *vertCompacted; // Whether vertex compaction is performed (for culling mode)

    // System values (SGPRs)
    llvm::Value *attribRingBase;          // Attribute ring base for this sub-group
    llvm::Value *primShaderTableAddrLow;  // Primitive shader table address low
    llvm::Value *primShaderTableAddrHigh; // Primitive shader table address high

    // System values (VGPRs)
    llvm::Value *primData; // Primitive connectivity data (only for non-GS NGG pass-through mode)

    llvm::Value *esGsOffset0; // ES-GS offset of vertex0
    llvm::Value *esGsOffset1; // ES-GS offset of vertex1
    llvm::Value *esGsOffset2; // ES-GS offset of vertex2
    llvm::Value *esGsOffset3; // ES-GS offset of vertex3
    llvm::Value *esGsOffset4; // ES-GS offset of vertex4
    llvm::Value *esGsOffset5; // ES-GS offset of vertex5

  } m_nggInputs = {};

  bool m_hasVs = false;  // Whether the pipeline has vertex shader
  bool m_hasTes = false; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs = false;  // Whether the pipeline has geometry shader

  bool m_enableSwXfb = false; // Whether SW-emulated stream-out is enabled (GFX11+)

  bool m_constPositionZ = false; // Whether the Z channel of vertex position data is constant

  // Base offsets (in dwords) of GS output vertex streams in GS-VS ring
  unsigned m_gsStreamBases[MaxGsStreams] = {};

  PrimShaderCbLayoutLookupTable m_cbLayoutTable; // Layout lookup table of primitive shader constant buffer
  VertexCullInfoOffsets m_vertCullInfoOffsets;   // A collection of offsets within an item of vertex cull info

  llvm::IRBuilder<> m_builder; // LLVM IR builder
};

} // namespace lgc
