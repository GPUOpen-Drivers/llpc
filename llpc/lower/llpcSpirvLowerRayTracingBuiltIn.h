/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayTracingBuiltIn.h
 * @brief LLPC header file: contains declaration of Llpc::SpirvLowerRayTracingBuiltIn
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llpcSpirvLowerRayTracing.h"

namespace Llpc {

static const unsigned TraceRayDescriptorSet = 93; // Descriptor set ID in traceRay binary

// Enumerates the ray-tracing resource layout in traceRay binary
enum RayTracingResourceIndex : unsigned {
  RayTracingResourceIndexShaderHitMissId,       // Hit/miss shader identifier data
  RayTracingResourceIndexTraceData,             // Trace data
  RayTracingResourceIndexAttributeData,         // Attribute data buffer
  RayTracingResourceIndexTraversalStack,        // Internal trace rays shader stack
  RayTracingResourceIndexShaderProgressData,    // Shader progress data
  RayTracingResourceIndexRayPayLoad,            // Ray payload buffer
  RayTracingResourceIndexPrimitiveData,         // Primitive data buffer
  RayTracingResourceIndexShaderQueueCounter,    // Shader queue counter buffer
  RayTracingResourceIndexSchedulerData,         // Scheduler data
  RayTracingResourceIndexDynamicRaysInfo = 16,  // Dynamic ray info (constant buffer)
  RayTracingResourceIndexDispatchRaysInfo = 17, // Dispatch ray info (constant buffer)
  RayTracingResourceIndexInvalid = UINT32_MAX,  // Invalid ray tracing resource index
};

// =====================================================================================================================
// Represents the pass of SPIR-V lowering ray tracing.
class SpirvLowerRayTracingBuiltIn : public SpirvLower, public llvm::PassInfoMixin<SpirvLowerRayTracingBuiltIn> {
public:
  SpirvLowerRayTracingBuiltIn();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  virtual bool runImpl(llvm::Module &module);

  static llvm::StringRef name() { return "Lower SPIR-V ray tracing built-ins"; }

private:
  llvm::Value *processBuiltIn(llvm::GlobalVariable *global, llvm::Instruction *insertPos);
  void setShaderTableVariables(llvm::GlobalValue *global, ShaderTable tableKind, llvm::Instruction *insertPos);
  llvm::Value *getDispatchRaysInfoDesc(llvm::Instruction *insertPos);

  llvm::GlobalValue *m_traceParams[TraceParam::Count]; // Trace ray set parameters
  llvm::Value *m_dispatchRaysInfoDesc;                 // Descriptor of the DispatchRaysInfo
};

} // namespace Llpc
