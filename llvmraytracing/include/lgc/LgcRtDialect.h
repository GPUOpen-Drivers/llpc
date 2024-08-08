/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Declarations for the lgc.rt dialect

#pragma once

#include <optional>

#define GET_INCLUDES
#define GET_DIALECT_DECLS
#include "LgcRtDialect.h.inc"

namespace llvm {
class Constant;
class Function;
class Module;
} // namespace llvm

namespace lgc {

namespace rt {
enum class RayTracingShaderStage {
  RayGeneration,
  Intersection,
  AnyHit,
  ClosestHit,
  Miss,
  Callable,
  // Not an input shader stage but we need to annotate it as well
  Traversal,
  KernelEntry,
  Count
};

// These ray flags correspond to the equivalent flags in GLSL_EXT_ray_tracing
enum class RayFlag {
  None = 0x00,
  ForceOpaque = 0x01,
  ForceNonOpaque = 0x02,
  AcceptFirstHitAndEndSearch = 0x04,
  SkipClosestHitShader = 0x08,
  CullBackFacingTriangles = 0x10,
  CullFrontFacingTriangles = 0x20,
  CullOpaque = 0x40,
  CullNonOpaque = 0x80,
  SkipTriangles = 0x100,
  SkipProceduralPrimitives = 0x200,
};

// Set shader stage metadata on a LLVM function and erase it by setting
// std::nullopt.
// func can instead be a GlobalVariable, allowing a front-end to use a
// GlobalVariable to represent a shader retrieved from the cache, and wants to
// mark it with a shader stage.
void setLgcRtShaderStage(llvm::GlobalObject *func, std::optional<RayTracingShaderStage> stage);

// Gets the shader stage from the specified LLVM function or std::nullopt
// if no metadata is apparent.
// func can instead be a GlobalVariable, allowing a front-end to use a
// GlobalVariable to represent a shader retrieved from the cache, and wants to
// mark it with a shader stage.
std::optional<RayTracingShaderStage> getLgcRtShaderStage(const llvm::GlobalObject *func);

// Get the name string of shader subtype for the specified shader stage.
const char *getShaderSubtypeForRtShaderStage(RayTracingShaderStage stage);

// Get the metadata IDs associated with the lgc.rt dialect, so the caller knows
// which ones can be removed when the dialect is processed.
void getLgcRtMetadataIds(llvm::LLVMContext &context, llvm::SmallVectorImpl<unsigned> &ids);

// Get PAQ (payload access qualifier) metadata for a ray-tracing shader
// function, or nullptr if none.
llvm::Constant *getShaderPaq(llvm::Function *func);

// Set PAQ (payload access qualifier) metadata for a ray-tracing shader
// function.
void setShaderPaq(llvm::Function *func, llvm::Constant *paq);

// Get PAQ (payload access qualifier) from size in bytes, for the simple case
// that that is the only information we have on the payload.
llvm::Constant *getPaqFromSize(llvm::LLVMContext &context, size_t size);

// Get arg size (in bytes) metadata for a ray-tracing callable shader function.
size_t getShaderArgSize(llvm::Function *func);

// Set arg size (in bytes) metadata for a ray-tracing callable shader function.
void setShaderArgSize(llvm::Function *func, size_t size);

// Get attribute size (in bytes) metadata for a ray-tracing shader
// function.
std::optional<size_t> getShaderHitAttributeSize(const llvm::Function *func);

// Set attribute size (in bytes) metadata for a ray-tracing shader
// function.
void setShaderHitAttributeSize(llvm::Function *func, size_t size);

// Get max hit attribute size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function hit attribute sizes.
std::optional<size_t> getMaxHitAttributeSize(const llvm::Module *module);

// Set max hit attribute size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function hit attribute sizes.
void setMaxHitAttributeSize(llvm::Module *module, size_t size);

// Get max payload size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function payload sizes.
std::optional<size_t> getMaxPayloadSize(const llvm::Module *module);

// Set max hit attribute size (in bytes) metadata for a ray-tracing module.
// This is a pipeline-wide upper bound on the per-function payload sizes.
void setMaxPayloadSize(llvm::Module *module, size_t size);

} // namespace rt
} // namespace lgc
