/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BufferResource.cpp
 * @brief LLPC source file: contains implementation of LLPC internal-use utility functions.
 ***********************************************************************************************************************
 */
#include "lgc/util/BufferResource.h"

using namespace llvm;
using namespace lgc;

namespace lgc {
// =====================================================================================================================
// Get 64/32 bit NumRecords from the buffer descriptor resource
//
// @gfxIpVer : GfxIp version
// @builder : Passed in Builder
// @bufferDesc : Buffer descriptor resource
Value *getBufferNumRecords(const GfxIpVersion &gfxIpVer, BuilderCommon &builder, Value *const bufferDesc) {
  Value *numRecords = nullptr;
  if (gfxIpVer.major <= 12) {
    // Extract element 2 which is the NUM_RECORDS field from the buffer descriptor.
    numRecords = builder.CreateExtractElement(bufferDesc, 2);
  } else {
    llvm_unreachable("Unsupported gfxip!");
  }
  return numRecords;
}

// =====================================================================================================================
// Get 32bit Stride from the buffer descriptor resource
//
// @gfxIpVer : GfxIp version
// @builder : Passed in Builder
// @bufferDesc : Buffer descriptor resource
Value *getBufferStride(const GfxIpVersion &gfxIpVer, BuilderCommon &builder, Value *const bufferDesc) {
  Value *stride = nullptr;
  if (gfxIpVer.major <= 12) {
    // stride[61:48]
    Value *desc1 = builder.CreateExtractElement(bufferDesc, 1);
    stride = builder.CreateAnd(builder.CreateLShr(desc1, builder.getInt32(16)), builder.getInt32(0x3fff));
  } else {
    llvm_unreachable("Unsupported gfxip!");
  }
  // TODO:stride is possibly required to updated with stride_scale, stride_scale value:0 = 1x, 1 = 4x, 2 = 8x, 3 = 32x
  return stride;
}

} // namespace lgc
