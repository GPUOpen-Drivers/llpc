/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- SpecializeDriverShaders.h ----------------------------------------------------------------------------------===//
//
// This file declares a pass to specialize arguments of driver functions (e.g. Traversal) for known constants using
// full pipeline knowledge.
//
//===--------------------------------------------------------------------------------------------------------------===//

// This pass specializes driver shaders (e.g. the Traversal shader), propagating common known arguments into them.
// For now this only specializes the Traversal shader, but we could later extend it, e.g. for a dedicated Sort shader.
//
// For Traversal, we partition CPS functions into in-Traversal and out-of-Traversal functions.
// In-Traversal functions are Traversal itself, AHS and Intersection functions, including Intersection resume functions.
//
// We analyze all jumps to functions that might be in-Traversal, decompose passed arguments into dword-sized argument
// slots, and determine which argument slots are constant.
// For in-Traversal functions, we additionally analyze which argument slots are not constant, but preserved.
// We combine this information to prove that certain argument slots always have a specific constant value when entering
// the Traversal shader, and specialize Traversal accordingly.
//
// Although this optimization requires full-pipeline knowledge, it can also be applied for non-standalone pipelines,
// because we compile driver functions per pipeline after having processed all contained app shaders.
//
// This mostly aims at optimizing the common pattern of using the payload to pass information from CHS/Miss back to
// RayGen, and leaving the payload uninitialized or zero-initialized during Traversal. However, it also covers
// some common cases of constant TraceRay arguments, e.g. tMin and tMax.
//
// The analysis and specialization part is done by the same pass. We use metadata to store analysis results on app
// shaders, and rely on the pipeline compiler to merge the analysis results across modules accordingly.
// The necessary state is maintained by SpecializeDriverShadersState, which is part of llvmraytracing::PipelineState.
//
// As the analysis and optimization relies on specializing constant argument slots, and as we rely on type punning
// to e.g. pass compatible prefixes of structs, we have to make some assumptions on the calling convention in order
// to determine which values end up in which argument slots.
//
// For that, we assume that aggregate types and vector types are recursively decomposed into scalars, and that the
// scalars are passed in consecutive argument slots without any padding, covering multiple arg slots for large scalars.
// We assume that there is no packing of separate small scalars (e.g. 16-bit) into single registers / argument slots.
// This is the same assumption that is also used in LowerRaytracingPipeline when determining argument padding.
//
// We can only analyze argument slots that correspond to a full, aligned dword in the in-memory representation of a
// type, because our value analysis works on dword slices on the in-memory representation.
// Other argument slots are conservatively treated as unknown / dynamic.
// For instance, this excludes i16 scalars, and misaligned i32 scalars (e.g. as part of a packed struct).
// As of this writing, we don't use such arguments.
//
// All of this even works if the data layout (DL) requires padding in passed types, where there is no longer a 1:1
// correspondence between the dwords in the in-memory layout of args, and the in-register representation.
// This is achieved by maintaining a mapping between the in-memory representation of a type, which is the basis
// for our value origin analysis, and the in-register representation.
// For instance, if i64 is 64-bit aligned, then the type {i32, i64} has a single padding dword in memory, but not as
// in-registers argument.
// A shader that receives such a type, and passes the contained i32 and i64 values as separate arguments to the next
// one is considered to preserve these three argument slots.
//
// We rely on being able to replace undef and poison values by arbitrary constants. For instance, if all TraceRay
// call sites pass in an undef value in a particular argument slot, and the only other shader that does not preserve
// this argument slot instead passes a constant C, then we assume this argument slot to always equal C.
// This may break apps that incorrectly rely on implicit zero-initialization.
// If this becomes an issue, we can make undef/poison behavior configurable, and e.g. treat it as constant zero instead.
//
//===--------------------------------------------------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/PassManager.h"
#include <memory>

namespace llvm {

namespace msgpack {
class DocNode;
} // namespace msgpack

// Options for SpecializeDriverShadersPass.
// Defined out of class to work around issue with a default-initialized argument.
struct SpecializeDriverShadersOptions {
  // If set, only analysis is done, but not function specialization.
  // Skipping the pass can be potentially unsafe unless one can guarantee to skip it on
  // all modules of a pipeline, including parent pipelines. Otherwise, running the pass
  // on some but not all modules might lead to incorrect specializations.
  bool DisableSpecialization = false;
  // Disable analysis of functions in a module. Allows testing specializations of multiple functions in the same module.
  bool DisableAnalysis = false;

  bool operator==(SpecializeDriverShadersOptions const &Other) const {
    return std::tie(DisableSpecialization, DisableAnalysis) ==
           std::tie(Other.DisableSpecialization, Other.DisableAnalysis);
  }

  bool operator!=(SpecializeDriverShadersOptions const &Other) const { return !(*this == Other); }

  void exportModuleMetadata(llvm::Module &M) const;
  static llvm::Expected<SpecializeDriverShadersOptions> fromModuleMetadata(const llvm::Module &M);
};

class SpecializeDriverShadersPass : public llvm::PassInfoMixin<SpecializeDriverShadersPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &Module, llvm::ModuleAnalysisManager &AnalysisManager);
  static llvm::StringRef name() { return "Specialize driver functions"; }
};

// The whole-pipeline state needed by SpecializeDriverShadersPass to optimize driver functions.
// This implements the interface required by llvmraytracing::PipelineState for serialization.
// Serialization order on app modules is:
//  1. Start with app module without metadata
//  2. Pass runs on module, tries to import from MD, there is none, so start with trivial state
//  3. At the end of the pass, serialize to MD
//  4. At the end of the llvmraytracing pipeline, llvmraytracing::PipelineState deserializes from MD
//  5. The pipeline compiler may merge with the deserialized state from other modules
//  6. The combined state is serialized to the GpuRt module
//  7. The pass runs on the GpuRt module, deserializes the combined pipeline state, and specializes
//     shaders according to that state.
//
// In case of separately compiled libraries or parent pipelines, at the end the combined
// state is serialized to MsgPack, stored as a blob, imported from MsgPack for the child pipeline,
// and combined with the child pipeline's app shader states.
//
// We use the pImpl (pointer to implementation) pattern to prevent exposing implementation details in the header.
class SpecializeDriverShadersState {
public:
  using Self = SpecializeDriverShadersState;
  SpecializeDriverShadersState();
  SpecializeDriverShadersState(const Self &Other);
  SpecializeDriverShadersState(Self &&);
  // User-declared default destructor to avoid header dependency on ~Impl(), as Impl is forward declared only.
  ~SpecializeDriverShadersState() noexcept;

  SpecializeDriverShadersState &operator=(const SpecializeDriverShadersState &Other);
  SpecializeDriverShadersState &operator=(SpecializeDriverShadersState &&Other);

  static llvm::Expected<Self> decodeMsgpack(llvm::msgpack::DocNode &Node);
  void encodeMsgpack(llvm::msgpack::DocNode &Node) const;

  // In case no module metadata is found, e.g. because the SpecializeDriverShadersPass did not run
  // on the module, we return a valid, trivial state object.
  // Errors are only returned in case there is metadata, but using an unexpected format.
  // We only apply the Traversal specialization in case there is an existing nontrivial state,
  // to prevent miscompiles in case the cross-module state merging is not performed.
  static llvm::Expected<Self> fromModuleMetadata(const llvm::Module &M);
  void exportModuleMetadata(llvm::Module &M) const;

  void merge(const Self &Other);

  void print(llvm::raw_ostream &OS) const;

private:
  friend class SpecializeDriverShadersPass;

  struct Impl;
  SpecializeDriverShadersState(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> Pimpl;
};

} // namespace llvm
