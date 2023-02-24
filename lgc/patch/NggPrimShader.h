/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Module.h"

namespace lgc {

struct NggControl;
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

// Represents a collection of LDS offsets (in dwords) within an item of vertex cull info.
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

// Enumerates the LDS regions used by primitive shader
enum class PrimShaderLdsRegion : unsigned {
  DistributedPrimitiveId, // Distributed primitive ID
  XfbOutput,              // Transform feedback outputs
  VertexPosition,         // Vertex position
  VertexCullInfo,         // Vertex cull info
  XfbStats,               // Transform feedback statistics
  VertexCounts,           // Vertex counts in waves and in NGG subgroup
  VertexIndexMap,         // Vertex index map (compacted -> uncompacted)
  EsGsRing,               // ES-GS ring
  PrimitiveData,          // Primitive connectivity data
  PrimitiveCounts,        // Primitive counts in waves and in NGG subgroup
  PrimitiveIndexMap,      // Primitive index map (compacted -> uncompacted)
  GsVsRing,               // GS-VS ring
};

// Represents LDS usage info of primitive shader
struct PrimShaderLdsUsageInfo {
  bool needsLds;           // Whether primitive shader needs LDS for operations
  unsigned esExtraLdsSize; // ES extra LDS size in dwords
  unsigned gsExtraLdsSize; // GS extra LDS size in dwords
};

// Map: LDS region -> <region Offset, region Size>
typedef std::unordered_map<PrimShaderLdsRegion, std::pair<unsigned, unsigned>> PrimShaderLdsLayout;

// =====================================================================================================================
// Represents the manager of NGG primitive shader.
class NggPrimShader {
public:
  NggPrimShader(PipelineState *pipelineState);

  static unsigned calcEsGsRingItemSize(PipelineState *pipelineState);
  static PrimShaderLdsUsageInfo layoutPrimShaderLds(PipelineState *pipelineState,
                                                    PrimShaderLdsLayout *ldsLayout = nullptr);

  llvm::Function *generate(llvm::Function *esMain, llvm::Function *gsMain, llvm::Function *copyShader);

private:
  NggPrimShader() = delete;
  NggPrimShader(const NggPrimShader &) = delete;
  NggPrimShader &operator=(const NggPrimShader &) = delete;

  static unsigned calcVertexCullInfoSizeAndOffsets(PipelineState *pipelineState,
                                                   VertexCullInfoOffsets &vertCullInfoOffsets);

  llvm::FunctionType *getPrimShaderType(uint64_t &inRegMask);

  void buildPrimShaderCbLayoutLookupTable();

  void buildPassthroughPrimShader(llvm::Function *entryPoint);
  void buildPrimShader(llvm::Function *entryPoint);
  void buildPrimShaderWithGs(llvm::Function *entryPoint);

  void initWaveThreadInfo(llvm::Value *mergedGroupInfo, llvm::Value *mergedWaveInfo);
  void loadStreamOutBufferInfo(llvm::Value *userData);
  void distributePrimitiveId(llvm::Value *primitiveId);

  llvm::Value *cullPrimitive(llvm::Value *vertxIndex0, llvm::Value *vertxIndex1, llvm::Value *vertxIndex2);
  void sendGsAllocReqMessage();
  void exportPassthroughPrimitive();
  void exportPrimitive(llvm::Value *primitiveCulled);
  void exportPrimitiveWithGs(llvm::Value *startingVertexIndex);

  void earlyExitWithDummyExport();

  void runEs(llvm::ArrayRef<llvm::Argument *> args);
  llvm::Value *runPartEs(llvm::ArrayRef<llvm::Argument *> args, llvm::Value *position = nullptr);
  void splitEs();

  void runGs(llvm::ArrayRef<llvm::Argument *> args);
  void mutateGs();

  void runCopyShader(llvm::ArrayRef<llvm::Argument *> args);
  void mutateCopyShader();

  void appendUserData(llvm::SmallVectorImpl<llvm::Value *> &args, llvm::Function *target, llvm::Value *userData,
                      unsigned userDataCount);

  void writeGsOutput(llvm::Value *output, unsigned location, unsigned compIdx, unsigned streamId,
                     llvm::Value *primitiveIndex, llvm::Value *emitVerts);
  llvm::Value *readGsOutput(llvm::Type *outputTy, unsigned location, unsigned streamId, llvm::Value *vertexOffset);

  void processGsEmit(unsigned streamId, llvm::Value *primitiveIndex, llvm::Value *emitVertsPtr,
                     llvm::Value *outVertsPtr);
  void processGsCut(unsigned streamId, llvm::Value *outVertsPtr);

  llvm::Function *createGsEmitHandler();
  llvm::Function *createGsCutHandler();

  llvm::Value *readPerThreadDataFromLds(llvm::Type *readDataTy, llvm::Value *threadId, PrimShaderLdsRegion region,
                                        unsigned offsetInRegion = 0, bool useDs128 = false);
  void writePerThreadDataToLds(llvm::Value *writeData, llvm::Value *threadId, PrimShaderLdsRegion region,
                               unsigned offsetInRegion = 0, bool useDs128 = false);

  llvm::Value *readVertexCullInfoFromLds(llvm::Type *readDataTy, llvm::Value *vertexItemOffset, unsigned dataOffset);
  void writeVertexCullInfoToLds(llvm::Value *writeData, llvm::Value *vertexItemOffset, unsigned dataOffset);

  llvm::Value *runBackfaceCuller(llvm::Value *primitiveAlreadyCulled, llvm::Value *vertex0, llvm::Value *vertex1,
                                 llvm::Value *vertex2);
  llvm::Value *runFrustumCuller(llvm::Value *primitiveAlreadyCulled, llvm::Value *vertex0, llvm::Value *vertex1,
                                llvm::Value *vertex2);
  llvm::Value *runBoxFilterCuller(llvm::Value *primitiveAlreadyCulled, llvm::Value *vertex0, llvm::Value *vertex1,
                                  llvm::Value *vertex2);
  llvm::Value *runSphereCuller(llvm::Value *primitiveAlreadyCulled, llvm::Value *vertex0, llvm::Value *vertex1,
                               llvm::Value *vertex2);
  llvm::Value *runSmallPrimFilterCuller(llvm::Value *primitiveAlreadyCulled, llvm::Value *vertex0, llvm::Value *vertex1,
                                        llvm::Value *vertex2);
  llvm::Value *runCullDistanceCuller(llvm::Value *primitiveAlreadyCulled, llvm::Value *signMask0,
                                     llvm::Value *signMask1, llvm::Value *signMask2);
  llvm::Value *fetchCullingControlRegister(unsigned regOffset);

  llvm::Function *createBackfaceCuller();
  llvm::Function *createFrustumCuller();
  llvm::Function *createBoxFilterCuller();
  llvm::Function *createSphereCuller();
  llvm::Function *createSmallPrimFilterCuller();
  llvm::Function *createCullDistanceCuller();
  llvm::Function *createFetchCullingRegister();

  llvm::Value *ballot(llvm::Value *value);

  llvm::Value *fetchVertexPositionData(llvm::Value *vertxIndex);
  llvm::Value *fetchCullDistanceSignMask(llvm::Value *vertxIndex);
  llvm::Value *calcVertexItemOffset(unsigned streamId, llvm::Value *vertxIndex);

  void processVertexAttribExport(llvm::Function *&target);

  void processSwXfb(llvm::ArrayRef<llvm::Argument *> args);
  void processSwXfbWithGs(llvm::ArrayRef<llvm::Argument *> args);
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

  unsigned getLdsRegionStart(PrimShaderLdsRegion region) {
    assert(m_ldsLayout.count(region) > 0);
    return m_ldsLayout[region].first;
  }

  llvm::Value *readValueFromLds(llvm::Type *readTy, llvm::Value *ldsOffset, bool useDs128 = false);
  void writeValueToLds(llvm::Value *writeValue, llvm::Value *ldsOffset, bool useDs128 = false);
  void atomicAdd(llvm::Value *valueToAdd, llvm::Value *ldsOffset);

  static const unsigned NullPrim = (1u << 31); // Null primitive data (invalid)

  PipelineState *m_pipelineState = nullptr; // Pipeline state
  GfxIpVersion m_gfxIp;                     // Graphics IP version info

  const NggControl *m_nggControl = nullptr; // NGG control settings

  // NGG inputs (from system values or derived from them)
  struct {
    // SGPRs
    llvm::Value *vertCountInSubgroup; // Number of vertices in subgroup
    llvm::Value *primCountInSubgroup; // Number of primitives in subgroup
    llvm::Value *vertCountInWave;     // Number of vertices in wave
    llvm::Value *primCountInWave;     // Number of primitives in wave

    llvm::Value *waveIdInSubgroup; // Wave ID in subgroup
    llvm::Value *orderedWaveId;    // Ordered wave ID

    llvm::Value *attribRingBase;                                 // Attribute ring base for this subgroup
    std::pair<llvm::Value *, llvm::Value *> primShaderTableAddr; // Primitive shader table address <low, high>

    // VGPRs
    llvm::Value *threadIdInWave;     // Thread ID in wave
    llvm::Value *threadIdInSubgroup; // Thread ID in subgroup

    llvm::Value *primData; // Primitive connectivity data (provided by HW)

    llvm::Value *vertexIndex0; // Relative index of vertex0 in NGG subgroup
    llvm::Value *vertexIndex1; // Relative index of vertex1 in NGG subgroup
    llvm::Value *vertexIndex2; // Relative index of vertex2 in NGG subgroup
  } m_nggInputs = {};

  // ES handlers
  struct {
    llvm::Function *main;            // ES main function
    llvm::Function *cullDataFetcher; // Part ES to fetch cull data (position and cull distance)
    llvm::Function *vertexExporter;  // Part ES to do deferred vertex exporting
  } m_esHandlers = {};

  // GS handlers
  struct {
    llvm::Function *main;       // GS main function
    llvm::Function *copyShader; // Copy shader
    llvm::Function *emit;       // GS emit handler
    llvm::Function *cut;        // GS cut handler
  } m_gsHandlers = {};

  // Cullers
  struct {
    llvm::Function *backface;        // Backface culller
    llvm::Function *frustum;         // Frustum culler
    llvm::Function *boxFilter;       // Box filter culler
    llvm::Function *sphere;          // Sphere culler
    llvm::Function *smallPrimFilter; // Small primitive filter
    llvm::Function *cullDistance;    // Cull distance culler
    llvm::Function *regFetcher;      // Culling register fetcher
  } m_cullers = {};

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

  llvm::GlobalValue *m_lds = nullptr; // Global variable to model primitive shader LDS
  PrimShaderLdsLayout m_ldsLayout;    // Primitive shader LDS layout
};

} // namespace lgc
