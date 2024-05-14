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
/**
 ***********************************************************************************************************************
 * @file  RayTracingLibrarySummary.h
 * @brief Declaration of raytracing library summaries
 *
 * LLPC raytracing compiles can be thought of as libraries that may or may not be linked into other raytracing
 * compiles.
 *
 * Raytracing library summaries represent summary information about libraries that can enable certain optimizations.
 * The information is cumulative, i.e. if library A is linked into library B, then the summary of library B takes also
 * the summary of library A into account.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace lgc {

struct RayTracingLibrarySummary {
  // Whether the library contains uses of TraceRay (e.g. OpTraceRay in SPIR-V).
  bool usesTraceRay = false;

  // If the library uses TraceRay, bit masks of ray flags that are statically known to always be set or unset.
  unsigned knownSetRayFlags = 0;
  unsigned knownUnsetRayFlags = 0;

  // The maximum ray payload size in bytes used by any shader in the pipeline (includes outgoing ray payload uses in
  // RGS/CHS/Miss). Must be 0 if the library never uses ray payloads (only callable shaders and RGS without TraceRay
  // calls).
  unsigned maxRayPayloadSize = 0;

  // The maximum hit attribute size in bytes used by any shader in the pipeline. Must be 0 if the library never uses hit
  // attributes (no AHS/IS/CHS).
  unsigned maxHitAttributeSize = 0;

  // The maximum occurring number of payload registers in the pipeline, which will be taken into account for Traversal
  // module so that it sees the correct maximum payload size of a pipeline.
  unsigned maxUsedPayloadRegisterCount = 0;

  // Whether a kernel entry function was built for this library.
  bool hasKernelEntry = false;

  // Whether a suitable traversal / TraceRay module was built for this library.
  //
  // A library that wasn't compiled for pipeline use may be missing such a function even if it uses TraceRay. In that
  // case, compiling a pipeline that includes the library must produce such traversal / TraceRay module.
  //
  // A library that has a suitable traversal module can be included in a larger library or pipeline, and that traversal
  // may no longer be suitable for the larger library or pipeline (e.g. due to incompatibilities in statically known ray
  // flags).
  bool hasTraceRayModule = false;

  static llvm::Expected<RayTracingLibrarySummary> decodeMsgpack(llvm::StringRef data);
  std::string encodeMsgpack() const;

  void merge(const RayTracingLibrarySummary &other);
};

} // namespace lgc
