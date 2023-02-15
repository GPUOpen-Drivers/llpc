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
 * @file  SystemValues.h
 * @brief LLPC header file: per-shader per-pass generating and cache of shader pointers
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"
#include "lgc/state/Defs.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include <map>

namespace lgc {

class PipelineState;
struct ResourceNode;

// =====================================================================================================================
// "Shader system values" are values set up in a shader entrypoint, such as the ES->GS ring buffer descriptor, or the
// user descriptor table pointer, that some passes need access to. The ShaderSystemValues class has an instance for each
// shader in each pass that needs it, and it implements the on-demand emitting of the code to generate such a value, and
// caches the result for the duration of the pass using it. If multiple passes need the same value, then multiple copies
// of the generating code will be emitted, but that will be fixed by a later CSE pass.
class ShaderSystemValues {
public:
  // Initialize this ShaderSystemValues if it was previously uninitialized.
  void initialize(PipelineState *pipelineState, llvm::Function *entryPoint);

  // Get ES-GS ring buffer descriptor (for VS/TES output or GS input)
  llvm::Value *getEsGsRingBufDesc();

  // Get the descriptor for tessellation factor (TF) buffer (TCS output)
  llvm::Value *getTessFactorBufDesc();

  // Get the descriptor for vertex attribute ring buffer (for VS, TES, and copy shader output)
  llvm::Value *getAttribRingBufDesc();

  // Get the descriptor for task payload ring buffer (for task and mesh shader)
  llvm::Value *getTaskPayloadRingBufDesc();

  // Get the descriptor for task draw data ring buffer (for task and mesh shader)
  llvm::Value *getTaskDrawDataRingBufDesc();

  // Extract value of primitive ID (TCS)
  llvm::Value *getPrimitiveId();

  // Get invocation ID (TCS)
  llvm::Value *getInvocationId();

  // Get relative patchId (TCS)
  llvm::Value *getRelativeId();

  // Get offchip LDS descriptor (TCS and TES)
  llvm::Value *getOffChipLdsDesc();

  // Get tessellated coordinate (TES)
  llvm::Value *getTessCoord();

  // Get ES -> GS offsets (GS in)
  llvm::Value *getEsGsOffsets();

  // Get GS -> VS ring buffer descriptor (GS out and copy shader in)
  llvm::Value *getGsVsRingBufDesc(unsigned streamId);

  // Get pointers to emit counters (GS)
  std::pair<llvm::Type *, llvm::ArrayRef<llvm::Value *>> getEmitCounterPtr();

  // Get global internal table pointer as pointer to i8.
  llvm::Instruction *getInternalGlobalTablePtr();

  // Get the mesh pipeline statistics buffer pointer as pointer to i8
  llvm::Value *getMeshPipeStatsBufPtr();

  // Load descriptor from driver table
  llvm::Instruction *loadDescFromDriverTable(unsigned tableOffset, BuilderBase &builder);

  // Get internal per shader table pointer as pointer to i8.
  llvm::Value *getInternalPerShaderTablePtr();

  // Get stream-out buffer descriptor
  llvm::Value *getStreamOutBufDesc(unsigned xfbBuffer);

  // Test if shadow descriptor table is enabled
  bool isShadowDescTableEnabled() const;

private:
  // Get stream-out buffer table pointer
  std::pair<llvm::Type *, llvm::Instruction *> getStreamOutTablePtr();

  // Make 64-bit pointer of specified type from 32-bit int, extending with the specified value, or PC if InvalidValue
  llvm::Instruction *makePointer(llvm::Value *lowValue, llvm::Type *ptrTy, unsigned highValue);

  // Explicitly set the DATA_FORMAT of ring buffer descriptor.
  llvm::Value *setRingBufferDataFormat(llvm::Value *bufDesc, unsigned dataFormat, BuilderBase &builder) const;

  // Find resource node by descriptor set ID
  unsigned findResourceNodeByDescSet(unsigned descSet);

  llvm::Function *m_entryPoint = nullptr; // Shader entrypoint
  llvm::LLVMContext *m_context;           // LLVM context
  PipelineState *m_pipelineState;         // Pipeline state
  ShaderStage m_shaderStage;              // Shader stage

  llvm::Value *m_esGsRingBufDesc = nullptr;   // ES -> GS ring buffer descriptor (VS, TES, and GS)
  llvm::Value *m_tfBufDesc = nullptr;         // Descriptor for tessellation factor (TF) buffer (TCS)
  llvm::Value *m_offChipLdsDesc = nullptr;    // Descriptor for off-chip LDS buffer (TCS and TES)
  llvm::Value *m_attribRingBufDesc = nullptr; // Descriptor for vertex attribute ring buffer (VS, TES, and copy shader)
  llvm::Value *m_taskPayloadRingBufDesc = nullptr;  // Descriptor for task payload ring buffer (task and mesh shader)
  llvm::Value *m_taskDrawDataRingBufDesc = nullptr; // Descriptor for task draw data ring buffer (task and mesh shader)
  llvm::SmallVector<llvm::Value *, MaxGsStreams>
      m_gsVsRingBufDescs; // GS -> VS ring buffer descriptor (GS out and copy shader in)
  llvm::SmallVector<llvm::Value *, MaxTransformFeedbackBuffers> m_streamOutBufDescs; // Stream-out buffer descriptors

  llvm::Value *m_primitiveId = nullptr;                             // PrimitiveId (TCS)
  llvm::Value *m_invocationId = nullptr;                            // InvocationId (TCS)
  llvm::Value *m_relativeId = nullptr;                              // Relative PatchId (TCS)
  llvm::Value *m_tessCoord = nullptr;                               // Tessellated coordinate (TES)
  llvm::Value *m_esGsOffsets = nullptr;                             // ES -> GS offsets (GS in)
  llvm::SmallVector<llvm::Value *, MaxGsStreams> m_emitCounterPtrs; // Pointers to emit counters (GS)

  llvm::SmallVector<llvm::Value *, 8> m_descTablePtrs;       // Descriptor table pointers
  llvm::SmallVector<llvm::Value *, 8> m_shadowDescTablePtrs; // Shadow descriptor table pointers
  llvm::Instruction *m_internalGlobalTablePtr = nullptr;     // Internal global table pointer
  llvm::Value *m_meshPipeStatsBufPtr = nullptr;              // Mesh pipeline statistics buffer pointer
  llvm::Value *m_internalPerShaderTablePtr = nullptr;        // Internal per shader table pointer
  llvm::Instruction *m_streamOutTablePtr = nullptr;          // Stream-out buffer table pointer
  llvm::Instruction *m_pc = nullptr;                         // Program counter as <2 x i32>
};

// =====================================================================================================================
// A class that provides a mapping from a shader entrypoint to its ShaderSystemValues object
class PipelineSystemValues {
public:
  // Initialize this PipelineSystemValues.
  void initialize(PipelineState *pipelineState) { m_pipelineState = pipelineState; }

  // Get the ShaderSystemValues object for the given shader entrypoint.
  ShaderSystemValues *get(llvm::Function *entryPoint) {
    auto shaderSysValues = &m_shaderSysValuesMap[entryPoint];
    shaderSysValues->initialize(m_pipelineState, entryPoint);
    return shaderSysValues;
  }

  // Clear at the end of a pass run.
  void clear() { m_shaderSysValuesMap.clear(); }

private:
  PipelineState *m_pipelineState;
  std::map<llvm::Function *, ShaderSystemValues> m_shaderSysValuesMap;
};

} // namespace lgc
