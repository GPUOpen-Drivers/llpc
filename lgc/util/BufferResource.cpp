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
// Get 64/32 bit NUM_RECORDS from the buffer descriptor resource
//
// @param gfxIp : Graphics IP version
// @param builder : Builder for inserting instructions
// @param bufferDesc : Buffer descriptor resource
// @returns : NUM_RECORDS field value in buffer descriptor resource
Value *getBufferNumRecords(const GfxIpVersion &gfxIp, BuilderCommon &builder, Value *const bufferDesc) {
  Value *numRecords = nullptr;
  if (gfxIp.major <= 12) {
    // Extract element 2 which is the NUM_RECORDS field from the buffer descriptor.
    numRecords = builder.CreateExtractElement(bufferDesc, 2);
  } else {
    llvm_unreachable("Unsupported GFX IP!");
  }
  return numRecords;
}

// =====================================================================================================================
// Get 32 bit STRIDE from the buffer descriptor resource
//
// NOTE: This function just returns STRIDE field value from the buffer descriptor resource. The STRIDE_SCALE field value
// is not taken into account (STRIDE_SCALE: 0 = 1x, 1 = 4x, 2 = 8x, 3 = 32x).
//
// @param gfxIp : Graphics IP version
// @param builder : Passed in Builder
// @param bufferDesc : Buffer descriptor resource
// @returns : STRIDE field value in buffer descriptor resource
Value *getBufferStride(const GfxIpVersion &gfxIp, BuilderCommon &builder, Value *const bufferDesc) {
  Value *stride = nullptr;
  if (gfxIp.major <= 12) {
    // STRIDE = [61:48]
    Value *dword1 = builder.CreateExtractElement(bufferDesc, 1);
    stride = builder.CreateAnd(builder.CreateLShr(dword1, builder.getInt32(16)), builder.getInt32(0x3FFF));
  } else {
    llvm_unreachable("Unsupported GFX IP!");
  }

  return stride;
}

// =====================================================================================================================
// Set 32 bit STRIDE to buffer resource descriptor
//
// NOTE: This function just sets STRIDE field value to the buffer descriptor resource. The STRIDE_SCALE field value
// is not taken into account (STRIDE_SCALE: 0 = 1x, 1 = 4x, 2 = 8x, 3 = 32x).
//
// @param gfxIp : Graphics IP version
// @param builder : Passed in Builder
// @param [in/out] bufferDesc : Buffer descriptor resource
// @param stride : Value to set STRIDE field
void setBufferStride(const GfxIpVersion &gfxIp, BuilderCommon &builder, Value *&bufferDesc, Value *const stride) {
  if (gfxIp.major <= 12) {
    // STRIDE = [61:48]
    Value *dword1 = builder.CreateExtractElement(bufferDesc, 1);
    dword1 = builder.CreateAnd(dword1, ~0x3FFF0000);                                             // Clear old STRIDE
    dword1 = builder.CreateOr(dword1, builder.CreateShl(builder.CreateAnd(stride, 0x3FFF), 16)); // Set new STRIDE
    bufferDesc = builder.CreateInsertElement(bufferDesc, dword1, 1);
  }
}

} // namespace lgc
