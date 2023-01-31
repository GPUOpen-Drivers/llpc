/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  YCbCrAddressHandler.h
 * @brief LLPC header file: contains the definition of LLPC class YCbCrAddressHandler.
 ***********************************************************************************************************************
 */

#pragma once
#include "lgc/util/GfxRegHandler.h"

namespace lgc {

// =====================================================================================================================
// The class is used for calculating and maintain the base address of each plane in YCbCr image.
// Note: There are at most 3 planes, and the index for plane is start from zero
class YCbCrAddressHandler {
public:
  YCbCrAddressHandler(llvm::IRBuilder<> *builder, SqImgRsrcRegHandler *sqImgRsrcRegHandler, GfxIpVersion *gfxIp) {
    m_builder = builder;
    m_regHandler = sqImgRsrcRegHandler;
    m_gfxIp = gfxIp;
    m_planeBaseAddresses.clear();
    m_one = builder->getInt32(1);
  }

  // Generate base address for image planes
  void genBaseAddress(unsigned planeCount);

  // Generate height and pitch
  void genHeightAndPitch(unsigned bits, unsigned bpp, unsigned xBitCount, unsigned planeNum);

  // Power2Align operation
  llvm::Value *power2Align(llvm::Value *x, unsigned align);

  // Get specific plane
  llvm::Value *getPlane(unsigned idx) {
    assert(idx < 3);
    return m_planeBaseAddresses[idx];
  }

  // Get pitch for Y plane
  llvm::Value *getPitchY() { return m_pitchY; }

  // Get pitch for Cb plane
  llvm::Value *getPitchCb() { return m_pitchCb; }

  // Get height for Y plane
  llvm::Value *getHeightY() { return m_heightY; }

  // Get Height for Cb plane
  llvm::Value *getHeightCb() { return m_heightCb; }

private:
  SqImgRsrcRegHandler *m_regHandler;
  llvm::IRBuilder<> *m_builder;
  llvm::SmallVector<llvm::Value *, 3> m_planeBaseAddresses;
  llvm::Value *m_pitchY = nullptr;
  llvm::Value *m_heightY = nullptr;
  llvm::Value *m_pitchCb = nullptr;
  llvm::Value *m_heightCb = nullptr;
  llvm::Value *m_swizzleMode = nullptr;
  llvm::Value *m_one;
  GfxIpVersion *m_gfxIp;
};

} // namespace lgc
