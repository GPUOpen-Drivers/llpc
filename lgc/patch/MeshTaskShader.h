/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MeshTaskShader.h
 * @brief LLPC header file: contains declaration of class lgc::MeshTaskShader.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/LgcDialect.h"
#include "lgc/patch/PatchPreparePipelineAbi.h"
#include "lgc/patch/SystemValues.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

namespace lgc {

struct FunctionAnalysisHandlers;

// Represents the entry layout of mesh pipeline statistics buffer for a workgroup
struct MeshPipeStatsEntry {
  uint64_t numMeshThreads;
  uint64_t numMeshPrimitives;
  uint64_t numTaskThreads;
};

// Enumerates the LDS regions used by mesh shader
enum class MeshLdsRegion : unsigned {
  VertexCount = 0,   // Vertex count set by SetMeshOutputs
  PrimitiveCount,    // Primitive count set by SetMeshOutputs
  BarrierCompletion, // Barrier completion flag
  FlatWorkgroupId,   // Flat workgroup ID
  PrimitiveIndices,  // Primitive indices set by SetPrimitiveIndices
  VertexOutput,      // Per-vertex outputs
  PrimitiveOutput,   // Per-primitive outputsr
};

// Map: LDS Region -> <Region Offset, Region Size>
typedef std::unordered_map<MeshLdsRegion, std::pair<unsigned, unsigned>> MeshLdsLayout;

// =====================================================================================================================
// Represents the handler of mesh/task shader.
class MeshTaskShader {
public:
  MeshTaskShader(PipelineState *pipelineState, PatchPreparePipelineAbi::FunctionAnalysisHandlers *analysisHandlers);
  ~MeshTaskShader();

  static unsigned layoutMeshShaderLds(PipelineState *pipelineState, llvm::Function *entryPoint,
                                      MeshLdsLayout *ldsLayout = nullptr);

  void process(llvm::Function *taskEntryPoint, llvm::Function *meshEntryPoint);

private:
  static llvm::GlobalVariable *getOrCreateMeshLds(llvm::Module *module, unsigned meshLdsSizeInDwords = 0);
  static unsigned useFlatWorkgroupId(PipelineState *pipelineState);

  void processTaskShader(llvm::Function *entryPoint);
  void processMeshShader(llvm::Function *entryPoint);

  void lowerTaskPayloadPtr(TaskPayloadPtrOp &taskPayloadPtrOp);
  void lowerEmitMeshTasks(EmitMeshTasksOp &emitMeshTasksOp);
  void lowerSetMeshOutputs(SetMeshOutputsOp &setMeshOutputsOp);
  void lowerSetMeshPrimitiveIndices(SetMeshPrimitiveIndicesOp &setMeshPrimitiveIndicesOp);
  void lowerSetMeshPrimitiveCulled(SetMeshPrimitiveCulledOp &setMeshPrimitiveCulledOp);
  void lowerGetMeshBuiltinInput(GetMeshBuiltinInputOp &getMeshBuiltinInputOp);
  void lowerWriteMeshVertexOutput(WriteMeshVertexOutputOp &writeMeshVertexOutputOp);
  void lowerWriteMeshPrimitiveOutput(WriteMeshPrimitiveOutputOp &writeMeshPrimitiveOutputOp);

  void initWaveThreadInfo(llvm::Function *entryPoint);
  llvm::Value *getShaderRingEntryIndex(llvm::Function *entryPoint);

  llvm::Value *getPayloadRingEntryOffset(llvm::Function *entryPoint);
  llvm::Value *getDrawDataRingEntryOffset(llvm::Function *entryPoint);
  llvm::Value *getDrawDataReadyBit(llvm::Function *entryPoint);

  llvm::Value *convertToDivergent(llvm::Value *value);

  llvm::Function *mutateMeshShaderEntryPoint(llvm::Function *entryPoint);
  void lowerMeshShaderBody(llvm::BasicBlock *apiMeshEntryBlock, llvm::BasicBlock *apiMeshExitBlock);

  void exportPrimitive();
  void exportVertex();
  void collectMeshStatsInfo(llvm::Function *entryPoint, llvm::Value *numMeshPrimitives);

  // Export kind
  enum class ExportKind : unsigned {
    Pos = 0,
    Prim = 1,
    VertAttr = 2,
    PrimAttr = 3,
  };
  // Export info of a single entry
  struct ExportInfo {
    unsigned index;
    std::array<llvm::Value *, 4> values;
  };
  void doExport(ExportKind kind, llvm::ArrayRef<ExportInfo> exports);

  void prepareAttribRingAccess();

  llvm::Value *getMeshFlatWorkgroupId();
  llvm::Value *getMeshNumWorkgroups();
  llvm::Value *getMeshWorkgroupId();
  llvm::Value *getMeshLocalInvocationId();
  llvm::Value *getMeshLocalInvocationIndex();
  llvm::Value *getMeshGlobalInvocationId();

  llvm::Value *readMeshBuiltInFromLds(BuiltInKind builtIn);
  llvm::Value *convertToHwShadingRate(llvm::Value *primitiveShadingRate);

  bool checkNeedBarrierFlag(llvm::Function *entryPoint);

  unsigned getMeshShaderLdsRegionStart(MeshLdsRegion region) {
    assert(m_ldsLayout.count(region) > 0);
    return m_ldsLayout[region].first;
  }

  llvm::Value *readValueFromLds(llvm::Type *readTy, llvm::Value *ldsOffset);
  void writeValueToLds(llvm::Value *writeValue, llvm::Value *ldsOffset);
  void atomicOpWithLds(llvm::AtomicRMWInst::BinOp atomicOp, llvm::Value *atomicValue, llvm::Value *ldsOffset);
  void createFenceAndBarrier();
  void createBarrier();

  static constexpr unsigned PayloadRingEntrySize = 16 * 1024;    // 16K bytes per group
  static constexpr unsigned DrawDataRingEntrySize = 16;          // 16 bytes per group
  static constexpr unsigned AttribGranularity = 32 * SizeOfVec4; // 32 * 16 bytes

  PipelineState *m_pipelineState = nullptr; // Pipeline state

  PatchPreparePipelineAbi::FunctionAnalysisHandlers
      *m_analysisHandlers; // A collection of handler functions to get the analysis info of the given function

  PipelineSystemValues m_pipelineSysValues; // Cache of ShaderSystemValues objects, one per shader stage

  BuilderBase m_builder; // LLVM IR builder

  // The wave/thread info used for control shader branching
  struct {
    llvm::Value *waveIdInSubgroup;
    llvm::Value *threadIdInWave;
    llvm::Value *threadIdInSubgroup;
    llvm::Value *primOrVertexIndex;
    llvm::Value *rowInSubgroup;
  } m_waveThreadInfo = {};

  bool m_accessTaskPayload = false;                // Whether task shader has payload access operations
  llvm::Value *m_shaderRingEntryIndex = nullptr;   // Shader ring entry index of current workgroup
  llvm::Value *m_payloadRingEntryOffset = nullptr; // Entry offset (in bytes) of the payload ring

  bool m_hasNoVertexAttrib = false;              // Whether mesh shader has vertex attribute export or not
  llvm::Value *m_attribRingBufDesc = nullptr;    // Attribute ring buffer descriptor
  llvm::Value *m_attribRingBaseOffset = nullptr; // Subgroup's attribute ring base offset (in bytes)

  llvm::Value *m_barrierToggle = nullptr;            // Toggle used by calculation of barrier completion flag
  bool m_needBarrierFlag = false;                    // Whether barrier completion flag is needed
  llvm::SmallVector<llvm::CallInst *, 8> m_barriers; // Barriers collected from API mesh shader

  llvm::SmallVector<llvm::CallInst *, 16>
      m_callsToRemove; // Calls relevant to task/mesh shader operations that will be finally removed after lowering

  llvm::GlobalValue *m_lds = nullptr; // Global variable to model mesh shader LDS

  GfxIpVersion m_gfxIp; // Graphics IP version info

  MeshLdsLayout m_ldsLayout; // Mesh shader LDS layout
};

} // namespace lgc
