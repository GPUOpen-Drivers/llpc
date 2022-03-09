/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "lgc/patch/SystemValues.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

namespace lgc {

// Represents the entry layout of mesh pipeline statistics buffer for a workgroup
struct MeshPipeStatsEntry {
  uint64_t numMeshThreads;
  uint64_t numMeshPrimitives;
  uint64_t numTaskThreads;
};

// =====================================================================================================================
// Represents the handler of mesh/task shader.
class MeshTaskShader {
public:
  MeshTaskShader(PipelineState *pipelineState);
  ~MeshTaskShader();

  void process(llvm::Function *taskEntryPoint, llvm::Function *meshEntryPoint);

private:
  void processTaskShader(llvm::Function *entryPoint);
  void processMeshShader(llvm::Function *entryPoint);

  llvm::Value *readTaskPayload(llvm::Function *entryPoint, llvm::Type *readTy, llvm::Value *byteOffset);
  void writeTaskPayload(llvm::Function *entryPoint, llvm::Value *writeValue, llvm::Value *byteOffset);

  void initWaveThreadInfo(llvm::Function *entryPoint);
  llvm::Value *getShaderRingEntryIndex(llvm::Function *entryPoint);

  llvm::Value *getPayloadRingEntryOffset(llvm::Function *entryPoint);
  llvm::Value *getDrawDataRingEntryOffset(llvm::Function *entryPoint);
  llvm::Value *getDrawDataReadyBit(llvm::Function *entryPoint);
  void emitTaskMeshs(llvm::Function *entryPoint, llvm::Value *groupCountX, llvm::Value *groupCountY,
                     llvm::Value *groupCountZ);

  llvm::Value *convertToDivergent(llvm::Value *value);

  static constexpr unsigned PayloadRingEntrySize = 16 * 1024; // 16K bytes per group
  static constexpr unsigned DrawDataRingEntrySize = 16;       // 16 bytes per group

  PipelineState *m_pipelineState = nullptr; // Pipeline state

  PipelineSystemValues m_pipelineSysValues; // Cache of ShaderSystemValues objects, one per shader stage

  std::unique_ptr<llvm::IRBuilder<>> m_builder; // LLVM IR builder

  // The wave/thread info used for control shader branching
  struct {
    llvm::Value *waveIdInSubgroup;
    llvm::Value *threadIdInWave;
    llvm::Value *threadIdInSubgroup;
  } m_waveThreadInfo = {};

  bool m_accessTaskPayload = false;                 // Whether task shader has payload access operations
  llvm::Value *m_shaderRingEntryIndex = nullptr;    // Shader ring entry index of current workgroup
  llvm::Value *m_payloadRingEntryOffset = nullptr;  // Entry offset (in bytes) of the payload ring
  llvm::Value *m_drawDataRingEntryOffset = nullptr; // Entry offset (in bytes) of the draw data ring
  llvm::Value *m_drawDataReadyBit = nullptr;        // Flag indicating whether the draw data is ready for CP to fetch
};

} // namespace lgc
