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

// Extraction, merging and inserting reg/stack usage in PAL metadata between different ELFs.
// A front-end can use this to propagate register and stack usage from library ELFs up to a compute
// shader ELF.

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lgc {

class RegStackUsageImpl;

// Class to parse reg/stack usage from PAL metadata and merge it back.
class RegStackUsage {
public:
  ~RegStackUsage();

  // Construct empty, ready to use merge() to accumulate reg/stack usage in "this".
  RegStackUsage();

  // Construct from ELF blob. This reads the reg/stack usage from the ELF's PAL metadata.
  //
  // @param elfBlob : The ELF blob; must remain valid for the lifetime of the RegStackUsage object
  // @param maxTraceRayDepth : Max traceRay recursion depth for this shader as specified by the app; 0 for traversal
  // @param rayGenUsage : bitmap of which rayGens can reach this shader, with bit 63 covering all rayGens
  //                      beyond the first 63; 0 for traversal
  RegStackUsage(llvm::StringRef elfBlob, unsigned maxTraceRayDepth, uint64_t rayGenUsage);

  // Construct from Module. This reads the reg/stack usage from IR metadata, as written by writeMetadata().
  RegStackUsage(const llvm::Module &module);

  // Write the reg/stack usage into IR metadata.
  void writeMetadata(llvm::Module &module) const;

  // Merge reg/stack usage from one shader ELF into the accumulated merged usage in "this".
  void merge(const RegStackUsage &shaderUsage);

  // Finalize merged usage in "this" (that comes from indirect shaders), merge into the supplied ELF's usage,
  // and update the PAL metadata in the ELF.
  //
  // @param (in/out) elfBuffer : Buffer containing ELF to read and update
  // @param startOffset : Start offset of the ELF in the buffer
  // @param Alignment of frontend stack for global CPS; 0 for scratch CPS
  //
  void finalizeAndUpdate(llvm::SmallVectorImpl<char> &elfBuffer, size_t startOffset, unsigned frontendGlobalAlignment);

private:
  std::unique_ptr<RegStackUsageImpl> m_impl;
};

} // namespace lgc
