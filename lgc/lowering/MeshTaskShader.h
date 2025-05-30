/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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
#include "lgc/lowering/PreparePipelineAbi.h"
#include "lgc/lowering/SystemValues.h"
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
  MeshOutputCounts = 0, // Mesh output counts (vertexCount, primitiveCount) set by SetMeshOutputs
  BarrierCompletion,    // Barrier completion flag
  FlatWorkgroupId,      // Flat workgroup ID
  PrimitiveIndices,     // Primitive indices set by SetPrimitiveIndices
  VertexOutput,         // Per-vertex outputs
  PrimitiveOutput,      // Per-primitive outputsr
};

// Map: LDS Region -> <Region Offset, Region Size>
typedef std::unordered_map<MeshLdsRegion, std::pair<unsigned, unsigned>> MeshLdsLayout;

// Mesh shader outputs layout
struct MeshOutputsLayout {
  std::map<BuiltInKind, unsigned> vertexBuiltInExports; // Map from vertex built-in output ID to export slot
  std::map<unsigned, unsigned> vertexGenericExports;    // Map from vertex output location to export slot
                                                        // (exported as vertex attributes)
  unsigned vertexExportCount = 0;                       // Vertex export count

  std::map<BuiltInKind, unsigned> primitiveBuiltInExports; // Map from primitive built-in output ID to export slot
                                                           // (exported as primitive attributes)
  std::map<unsigned, unsigned> primitiveGenericExports;    // Map from primitive output location to export slot
  unsigned primitiveExportCount = 0;                       // Primitive export count

  bool outputsToAllocas = false;                                 // Write outputs to allocas
  llvm::AllocaInst *primitiveDataAlloca = nullptr;               // Primitive connectivity data alloca
  std::map<unsigned, llvm::AllocaInst *> vertexOutputAllocas;    // Map from vertex output location to output alloca
  std::map<unsigned, llvm::AllocaInst *> primitiveOutputAllocas; // Map from primitive output location to output alloca

  unsigned vertexStride = 0;                    // Vertex stride (in dwords)
  std::map<unsigned, unsigned> offsetsInVertex; // Map from output location to output offset within a vertex (in dwords)

  unsigned primitiveStride = 0;                    // Primitive stride (in dwords)
  std::map<unsigned, unsigned> offsetsInPrimitive; // Map from output location to output offset within a primitive
                                                   // (in dwords)
};

// =====================================================================================================================
// Represents the handler of mesh/task shader.
class MeshTaskShader {
public:
  MeshTaskShader(PipelineState *pipelineState, PreparePipelineAbi::FunctionAnalysisHandlers *analysisHandlers);

  static unsigned layoutMeshShaderLds(PipelineState *pipelineState, llvm::Function *entryPoint,
                                      MeshLdsLayout *ldsLayout = nullptr, MeshOutputsLayout *outputsLayout = nullptr);

  void process(llvm::Function *taskEntryPoint, llvm::Function *meshEntryPoint);

private:
  static llvm::GlobalVariable *getOrCreateMeshLds(llvm::Module *module, unsigned meshLdsSizeInDwords = 0);
  static unsigned useFlatWorkgroupId(PipelineState *pipelineState);
  static bool usesRowExport(PipelineState *pipelineState);
  static bool meshOutputsToAllocas(PipelineState *pipelineState, llvm::Function *entryPoint);

  void processTaskShader(llvm::Function *entryPoint);
  void processMeshShader(llvm::Function *entryPoint);
  void lowerGroupMemcpy(GroupMemcpyOp &groupMemcpyOp);
  void lowerTaskPayloadPtr(TaskPayloadPtrOp &taskPayloadPtrOp);
  void lowerEmitMeshTasks(EmitMeshTasksOp &emitMeshTasksOp);
  void lowerSetMeshOutputs(SetMeshOutputsOp &setMeshOutputsOp);
  void lowerSetMeshPrimitiveIndices(SetMeshPrimitiveIndicesOp &setMeshPrimitiveIndicesOp);
  void lowerSetMeshPrimitiveCulled(SetMeshPrimitiveCulledOp &setMeshPrimitiveCulledOp);
  void lowerGetMeshBuiltinInput(GetMeshBuiltinInputOp &getMeshBuiltinInputOp);
  void lowerWriteMeshOutput(WriteMeshOutputOp &writeMeshOutputOp);

  void initWaveThreadInfo(llvm::Function *entryPoint);
  llvm::Value *getShaderRingEntryIndex(llvm::Function *entryPoint);

  llvm::Value *getPayloadRingEntryOffset(llvm::Function *entryPoint);
  llvm::Value *getDrawDataRingEntryOffset(llvm::Function *entryPoint);
  llvm::Value *getDrawDataReadyBit(llvm::Function *entryPoint);

  llvm::Value *convertToDivergent(llvm::Value *value);

  llvm::Function *mutateMeshShaderEntryPoint(llvm::Function *entryPoint);
  void lowerMeshShaderBody(llvm::BasicBlock *apiMeshEntryBlock, llvm::BasicBlock *apiMeshExitBlock);

  void exportPrimitive();
  void exportPositions();
  void exportPrimitiveAttributes();
  void exportVertexAttributes();
  void collectMeshStatsInfo(llvm::Function *entryPoint, llvm::Value *numMeshPrimitives);

  // Export kind
  enum class ExportKind : unsigned {
    Position = 0,
    Primitive = 1,
    VertexAttribute = 2,
    PrimitiveAttribute = 3,
  };
  // Export info of a single entry
  struct ExportInfo {
    unsigned slot;
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

  llvm::Value *readBackMeshBuiltInOutput(BuiltInKind builtIn);
  llvm::Value *readBackMeshGenericOutput(unsigned location, bool primitive);
  llvm::Value *convertToHwShadingRate(llvm::Value *primitiveShadingRate);
  void updateMeshShaderInOutUsage();

  bool checkNeedBarrierFlag(llvm::Function *entryPoint);

  unsigned getMeshShaderLdsRegionStart(MeshLdsRegion region) {
    assert(m_ldsLayout.count(region) > 0);
    return m_ldsLayout[region].first;
  }

  unsigned getOutputOffsetInPrimOrVertex(unsigned location, bool inPrimitive) {
    if (inPrimitive) {
      assert(m_outputsLayout.offsetsInPrimitive.count(location) > 0); // Must exist
      return m_outputsLayout.offsetsInPrimitive[location];
    }

    assert(m_outputsLayout.offsetsInVertex.count(location) > 0); // Must exist
    return m_outputsLayout.offsetsInVertex[location];
  }

  unsigned getOutputExportSlot(unsigned location, bool primitive) {
    if (primitive) {
      if (m_outputsLayout.primitiveGenericExports.count(location) > 0)
        return m_outputsLayout.primitiveGenericExports[location];
      return InvalidValue; // Not exist
    }

    if (m_outputsLayout.vertexGenericExports.count(location) > 0)
      return m_outputsLayout.vertexGenericExports[location];
    return InvalidValue; // Not exist
  }

  unsigned getOutputExportSlot(BuiltInKind builtIn, bool primitive) {
    if (primitive) {
      if (m_outputsLayout.primitiveBuiltInExports.count(builtIn) > 0)
        return m_outputsLayout.primitiveBuiltInExports[builtIn];
      return InvalidValue; // Not exist
    }

    if (m_outputsLayout.vertexBuiltInExports.count(builtIn) > 0)
      return m_outputsLayout.vertexBuiltInExports[builtIn];
    return InvalidValue; // Not exist
  }

  llvm::Value *getOutputAlloca(unsigned location, bool primitive) {
    assert(m_outputsLayout.outputsToAllocas);
    if (primitive) {
      if (m_outputsLayout.primitiveOutputAllocas.count(location) > 0)
        return m_outputsLayout.primitiveOutputAllocas[location];
      return nullptr;
    }

    if (m_outputsLayout.vertexOutputAllocas.count(location) > 0)
      return m_outputsLayout.vertexOutputAllocas[location];
    return nullptr;
  }

  llvm::Value *readValueFromLds(llvm::Type *readTy, llvm::Value *ldsOffset, unsigned alignment = 4);
  void writeValueToLds(llvm::Value *writeValue, llvm::Value *ldsOffset, unsigned alignment = 4);
  void atomicOpWithLds(llvm::AtomicRMWInst::BinOp atomicOp, llvm::Value *atomicValue, llvm::Value *ldsOffset);
  void createFenceAndBarrier();
  void createBarrier();

  static constexpr unsigned PayloadRingEntrySize = 16 * 1024; // 16K bytes per group
  static constexpr unsigned DrawDataRingEntrySize = 16;       // 16 bytes per group

  PipelineState *m_pipelineState = nullptr; // Pipeline state

  PreparePipelineAbi::FunctionAnalysisHandlers
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

  llvm::Value *m_attribRingBufDesc = nullptr;    // Attribute ring buffer descriptor
  llvm::Value *m_attribRingBaseOffset = nullptr; // Subgroup's attribute ring base offset (in bytes)

  llvm::Value *m_barrierToggle = nullptr;            // Toggle used by calculation of barrier completion flag
  bool m_needBarrierFlag = false;                    // Whether barrier completion flag is needed
  llvm::SmallVector<llvm::CallInst *, 8> m_barriers; // Barriers collected from API mesh shader

  llvm::SmallVector<llvm::CallInst *, 16>
      m_callsToRemove; // Calls relevant to task/mesh shader operations that will be finally removed after lowering

  llvm::GlobalValue *m_lds = nullptr; // Global variable to model mesh shader LDS

  GfxIpVersion m_gfxIp; // Graphics IP version info

  MeshLdsLayout m_ldsLayout;         // Mesh shader LDS layout
  MeshOutputsLayout m_outputsLayout; // Mesh shader outputs layout
};

} // namespace lgc
