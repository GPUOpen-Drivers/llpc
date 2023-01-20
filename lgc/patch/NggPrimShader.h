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
  unsigned compactedVertexIndex;
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
  unsigned compactedVertexIndex;
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
  void loadStreamOutBufferInfo(llvm::Value *userData);

  llvm::Value *doCulling(llvm::Module *module, llvm::Value *vertxIndex0, llvm::Value *vertxIndex1,
                         llvm::Value *vertxIndex2);
  void sendGsAllocReqMessage();
  void exportPassthroughPrimitive();
  void exportPrimitive(llvm::Value *primitiveCulled);
  void exportPrimitiveWithGs(llvm::Value *startingVertexIndex);

  void earlyExitWithDummyExport();

  void runEs(llvm::Function *esEntryPoint, llvm::ArrayRef<llvm::Argument *> args);
  llvm::Value *runPartEs(llvm::Function *partEs, llvm::ArrayRef<llvm::Argument *> args,
                         llvm::Value *position = nullptr);
  void splitEs(llvm::Function *esEntryPoint);

  void runGs(llvm::Function *gsEntryPoint, llvm::ArrayRef<llvm::Argument *> args);
  llvm::Function *mutateGs(llvm::Function *gsEntryPoint);

  void runCopyShader(llvm::Function *copyShader, llvm::ArrayRef<llvm::Argument *> args);
  llvm::Function *mutateCopyShader(llvm::Function *copyShader);

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

  llvm::Value *ballot(llvm::Value *value);

  llvm::Value *fetchVertexPositionData(llvm::Value *vertxIndex);
  llvm::Value *fetchCullDistanceSignMask(llvm::Value *vertxIndex);
  llvm::Value *calcVertexItemOffset(unsigned streamId, llvm::Value *vertxIndex);

  void processVertexAttribExport(llvm::Function *&target);

  void processSwXfb(llvm::Function *target, llvm::ArrayRef<llvm::Argument *> args);
  void processSwXfbWithGs(llvm::Function *target, llvm::ArrayRef<llvm::Argument *> args);
  llvm::Value *fetchXfbOutput(llvm::Function *target, llvm::ArrayRef<llvm::Argument *> args,
                              llvm::SmallVector<XfbOutputExport, 32> &xfbOutputExports);

  llvm::Value *readXfbOutputFromLds(llvm::Type *readDataTy, llvm::Value *vertxIndex, unsigned outputIndex);
  void writeXfbOutputToLds(llvm::Value *writeData, llvm::Value *vertxIndex, unsigned outputIndex);

  // Checks if NGG culling operations are enabled
  bool enableCulling() const {
    return m_nggControl->enableBackfaceCulling || m_nggControl->enableFrustumCulling ||
           m_nggControl->enableBoxFilterCulling || m_nggControl->enableSphereCulling ||
           m_nggControl->enableSmallPrimFilter || m_nggControl->enableCullDistanceCulling;
  }

  llvm::BasicBlock *createBlock(llvm::Function *parent, const llvm::Twine &blockName = "");
  llvm::Value *createUBfe(llvm::Value *value, unsigned offset, unsigned count);
  llvm::PHINode *createPhi(llvm::ArrayRef<std::pair<llvm::Value *, llvm::BasicBlock *>> incomings,
                           const llvm::Twine &name = "");
  void createFenceAndBarrier();

  static const unsigned NullPrim = (1u << 31); // Null primitive data (invalid)

  PipelineState *m_pipelineState = nullptr; // Pipeline state
  GfxIpVersion m_gfxIp;                     // Graphics IP version info

  const NggControl *m_nggControl = nullptr; // NGG control settings

  NggLdsManager *m_ldsManager = nullptr; // NGG LDS manager

  // NGG inputs (from system values or derived from them)
  struct {
    // SGPRs
    llvm::Value *vertCountInSubgroup; // Number of vertices in subgroup
    llvm::Value *primCountInSubgroup; // Number of primitives in subgroup
    llvm::Value *vertCountInWave;     // Number of vertices in wave
    llvm::Value *primCountInWave;     // Number of primitives in wave

    llvm::Value *waveIdInSubgroup; // Wave ID in subgroup
    llvm::Value *orderedWaveId;    // Ordered wave ID

    llvm::Value *attribRingBase;          // Attribute ring base for this subgroup
    llvm::Value *primShaderTableAddrLow;  // Primitive shader table address low
    llvm::Value *primShaderTableAddrHigh; // Primitive shader table address high

    // VGPRs
    llvm::Value *threadIdInWave;     // Thread ID in wave
    llvm::Value *threadIdInSubgroup; // Thread ID in subgroup

    llvm::Value *primData; // Primitive connectivity data (provided by HW)

    llvm::Value *vertexIndex0; // Relative index of vertex0 in NGG subgroup
    llvm::Value *vertexIndex1; // Relative index of vertex1 in NGG subgroup
    llvm::Value *vertexIndex2; // Relative index of vertex2 in NGG subgroup

  } m_nggInputs = {};

  llvm::Value *m_distributedPrimitiveId = nullptr; // Distributed primitive ID (from geomeotry based to vertex based)

  llvm::Value *m_compactVertex = nullptr; // Flag indicating whether to perform vertex compaction (if
                                          // null, we are in vertex compactionless mode)

  bool m_hasVs = false;  // Whether the pipeline has vertex shader
  bool m_hasTes = false; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs = false;  // Whether the pipeline has geometry shader

  llvm::Value *m_streamOutBufDescs[MaxTransformFeedbackBuffers];   // Stream-out buffer descriptors
  llvm::Value *m_streamOutBufOffsets[MaxTransformFeedbackBuffers]; // Stream-out buffer offsets

  bool m_constPositionZ = false; // Whether the Z channel of vertex position data is constant

  // Base offsets (in dwords) of GS output vertex streams in GS-VS ring
  unsigned m_gsStreamBases[MaxGsStreams] = {};

  PrimShaderCbLayoutLookupTable m_cbLayoutTable; // Layout lookup table of primitive shader constant buffer
  VertexCullInfoOffsets m_vertCullInfoOffsets;   // A collection of offsets within an item of vertex cull info

  llvm::IRBuilder<> m_builder; // LLVM IR builder
};

} // namespace lgc
