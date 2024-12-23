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
/**
 ***********************************************************************************************************************
 * @file  PipelineState.h
 * @brief Declaration of pipeline state owned by llvmraytracing
 *
 * Some optimizations implemented in llvmraytracing depend on cross-module state.
 * For instance, when compiling the Traversal shader, we need an upper bound on the payload size.
 *
 * This class keeps track of any such state that is owned my llvmraytracing, meaning it is produced
 * and consumed by llvmraytracing passes, and it can be changed without pipeline compiler (e.g. LLPC) changes.
 *
 * It supports importing/exporting from/to module metadata, merging with other pipeline states, and
 * serialization/deserialization to binary blobs via MsgPack.
 *
 * It is intended to be used like this by pipeline compilers (such as LLPC):
 *   * After processing of an app module, its pipeline state is extracted from metadata, and merged with earlier state.
 *   * Before compiling a module with full pipeline knowledge (e.g. when compiling the Traversal shader), the merged
 *     state is exported to the module.
 *   * After having compiled a library/pipeline that might be reused by a child pipeline, its state is serialized.
 *   * When reusing an early-compiled parent library/pipeline, its state is deserialized and merged into the current
 *     pipeline's state.
 *
 * The pipeline compiler is not expected to collect and merge state of early-compiled driver modules (GpuRt),
 * as these are compiled independently per pipeline, and thus compilation of child pipeline driver functions shouldn't
 * depend on parent pipeline driver functions.
 *
 ***********************************************************************************************************************
 */
#pragma once

#include "llvmraytracing/SpecializeDriverShaders.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace llvm {
class Module;
class raw_ostream;
namespace msgpack {
class DocNode;
} // namespace msgpack
} // namespace llvm

namespace llvmraytracing {

class PipelineState {
public:
  // Construct a new trivial pipeline state which can be used to be merged with non-trivial state later.
  PipelineState() = default;

  // (De)serialization to/from MsgPack is both supported standalone, or as part of an outer MsgPack document.
  static llvm::Expected<PipelineState> decodeMsgpack(llvm::StringRef Data);
  // Node is non-const because the const-correct accessors are less convenient to work with
  static llvm::Expected<PipelineState> decodeMsgpack(llvm::msgpack::DocNode &Node);
  std::string encodeMsgpack() const;
  void encodeMsgpack(llvm::msgpack::DocNode &Node) const;

  static llvm::Expected<PipelineState> fromModuleMetadata(const llvm::Module &M);
  void exportModuleMetadata(llvm::Module &M) const;

  void merge(const PipelineState &Other);

  void print(llvm::raw_ostream &OS) const;
#ifndef NDEBUG
  void dump() const;
#endif

private:
  // Actual state is intentionally private, as this interface is intended to be used like opaque state.
  // llvmraytracing passes don't use this interface, and instead directly work on module metadata.

  // The maximum occurring number of payload registers in the pipeline, which will be taken into account for Traversal
  // module so that it sees the correct maximum payload size of a pipeline.
  unsigned MaxUsedPayloadRegisterCount = 0;
  llvm::SpecializeDriverShadersState SDSState;
};

} // namespace llvmraytracing
