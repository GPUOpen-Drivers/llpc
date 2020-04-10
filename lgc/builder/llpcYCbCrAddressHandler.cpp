/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  llpcYCbCrAddressHandler.cpp
* @brief LLPC source file: Implementation of LLPC class YCbCrAddressHandler
***********************************************************************************************************************
*/
#include "llpcYCbCrAddressHandler.h"
#include "llpcGfxRegHandler.h"
#include "llpcInternal.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Generate base address for image planes
// Note: If input planeCount == 1, it'll generate the base address for plane 0. This function accepts the concept of
// planeCount rather a specific plane for that the calulation of plane[n+1] is always based on plane[n].
//
// @param planeCount : The plane counts
void YCbCrAddressHandler::genBaseAddress(unsigned planeCount) {
  // For YCbCr, the possible plane counts are among 1 and 3.
  assert(planeCount > 0 && planeCount < 4);

  // PlaneBaseAddresses[0] is the same as original base addressed as passed in SRD
  planeBaseAddresses.push_back(pRegHelper->getReg(SqRsrcRegs::BaseAddress));

  if (planeCount > 1) {
    // PlaneBaseAddresses[1] = PlaneBaseAddresses[0] + addr256B(PitchY * HeightY)
    planeBaseAddresses.push_back(pBuilder->CreateAdd(
        planeBaseAddresses[0], pBuilder->CreateLShr(pBuilder->CreateMul(pPitchY, pHeightY), pBuilder->getInt32(8))));
    if (planeCount > 2) {
      // PlaneBaseAddresses[2] = PlaneBaseAddresses[1] + addr256B(PitchCb * HeightCb)
      planeBaseAddresses.push_back(
          pBuilder->CreateAdd(planeBaseAddresses[1],
                              pBuilder->CreateLShr(pBuilder->CreateMul(pPitchCb, pHeightCb), pBuilder->getInt32(8))));
    }
  }
}

// =====================================================================================================================
// Power2Align operation
//
// @param x : Value needs to be aligned
// @param align : Align base
Value *YCbCrAddressHandler::power2Align(Value *x, unsigned align) {
  // Check if align is a power of 2
  assert(align != 0 && (align & (align - 1)) == 0);

  Value *result = pBuilder->CreateAdd(x, pBuilder->getInt32(align - 1));
  return pBuilder->CreateAnd(result, pBuilder->getInt32(~(align - 1)));
}

// =====================================================================================================================
// Calculate height and pitch
//
// @param bits : Channel bits
// @param bpp : Bits per pixel
// @param xBitCount : Effective channel bits
// @param isTileOptimal : Is tiling optimal
// @param planeNum : Number of planes
void YCbCrAddressHandler::genHeightAndPitch(unsigned bits, unsigned bpp, unsigned xBitCount, bool isTileOptimal,
                                            unsigned planeNum) {
  switch (pGfxIp->major) {
  case 9: {
    // Height = SqRsrcRegs::Height
    Value *height = pRegHelper->getReg(SqRsrcRegs::Height);
    // HeightHalf = Height * 0.5
    Value *heightHalf = pBuilder->CreateLShr(height, pOne);

    pHeightY = height;
    pHeightCb = heightHalf;

    // Pitch = SqRsrcRegs::Pitch
    Value *pitch = pRegHelper->getReg(SqRsrcRegs::Pitch);
    // PitchHalf = Pitch * 0.5
    Value *pitchHalf = pBuilder->CreateLShr(pitch, pOne);

    // PitchY * (xBitCount >> 3)
    pPitchY = pBuilder->CreateMul(pitch, pBuilder->CreateLShr(pBuilder->getInt32(xBitCount), 3));

    // PitchCb = PitchCb * (xBitCount >> 3)
    pPitchCb = pBuilder->CreateMul(pitchHalf, pBuilder->CreateLShr(pBuilder->getInt32(xBitCount), 3));

    if (isTileOptimal) {
      Value *isTileOpt = pRegHelper->getReg(SqRsrcRegs::IsTileOpt);

      // PtchYOpt = PitchY * (bits[0] >> 3)
      Value *ptchYOpt = pBuilder->CreateMul(pitch, pBuilder->CreateLShr(pBuilder->getInt32(bits), 3));
      // PitchY = IsTileOpt ? (PtchYOpt << 5) : PitchY
      pPitchY = pBuilder->CreateSelect(isTileOpt, pBuilder->CreateShl(ptchYOpt, pBuilder->getInt32(5)), pPitchY);

      // PitchCbOpt = PitchCb * (bits[0] >> 3)
      Value *pitchCbOpt = pBuilder->CreateMul(pitchHalf, pBuilder->CreateLShr(pBuilder->getInt32(bits), 3));
      // PitchCb = IsTileOpt ? (PitchCbOpt << 5) : PitchCb
      pPitchCb = pBuilder->CreateSelect(isTileOpt, pBuilder->CreateShl(pitchCbOpt, pBuilder->getInt32(5)), pPitchCb);
    }
    break;
  }
  case 10: {
    const unsigned elementBytes = bpp >> 3;
    const unsigned pitchAlign = (256 / elementBytes);

    // Height = SqRsrcRegs::Height
    Value *height = pRegHelper->getReg(SqRsrcRegs::Height);
    pHeightY = height;

    // Width = SqRsrcRegs::Width
    Value *width = pRegHelper->getReg(SqRsrcRegs::Width);

    pPitchY = power2Align(width, pitchAlign);
    // PitchY = PitchY * ElementBytes
    pPitchY = pBuilder->CreateMul(pPitchY, pBuilder->getInt32(elementBytes));

    // HeightHalf = Height * 0.5
    Value *heightHalf = pBuilder->CreateLShr(height, pOne);
    pHeightCb = heightHalf;

    // WidthHalf = Width * 0.5
    Value *widthHalf = pBuilder->CreateLShr(width, pOne);

    pPitchCb = power2Align(widthHalf, pitchAlign);
    // PitchCb = PitchCb * ElementBytes
    pPitchCb = pBuilder->CreateMul(pPitchCb, pBuilder->getInt32(elementBytes));

    if (isTileOptimal) {
      const unsigned log2BlkSize = 16;
      const unsigned log2EleBytes = log2(bpp >> 3);
      const unsigned log2NumEle = log2BlkSize - log2EleBytes;
      const bool widthPrecedent = 1;
      const unsigned log2Width = (log2NumEle + (widthPrecedent ? 1 : 0)) / 2;
      const unsigned pitchAlignOpt = 1u << log2Width;
      const unsigned heightAlignOpt = 1u << (log2NumEle - log2Width);

      // PitchY = PitchY * ElementBytes
      Value *ptchYOpt = pBuilder->CreateMul(power2Align(width, pitchAlignOpt), pBuilder->getInt32(elementBytes));

      // PitchCb = PitchCb * ElementBytes
      Value *pitchCbOpt = pBuilder->CreateMul(power2Align(widthHalf, pitchAlignOpt), pBuilder->getInt32(elementBytes));

      Value *isTileOpt = pRegHelper->getReg(SqRsrcRegs::IsTileOpt);
      pPitchY = pBuilder->CreateSelect(isTileOpt, ptchYOpt, pPitchY);
      pHeightY = pBuilder->CreateSelect(isTileOpt, power2Align(height, heightAlignOpt), height);

      pPitchCb = pBuilder->CreateSelect(isTileOpt, pitchCbOpt, pPitchCb);
      pHeightCb = pBuilder->CreateSelect(isTileOpt, power2Align(heightHalf, heightAlignOpt), heightHalf);
    }
    break;
  }
  default:
    llvm_unreachable("GFX IP not supported!");
    break;
  }
}
