/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  YCbCrAddressHandler.cpp
* @brief LLPC source file: Implementation of LLPC class YCbCrAddressHandler
***********************************************************************************************************************
*/
#include "YCbCrAddressHandler.h"
#include "chip/gfx9/gfx9_plus_merged_enum.h"
#include "lgc/util/GfxRegHandler.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Generate base address for image planes
// Note: If input planeCount == 1, it'll generate the base address for plane 0. This function accepts the concept of
// planeCount rather a specific plane for that the calculation of plane[n+1] is always based on plane[n].
//
// @param planeCount : The plane counts
void YCbCrAddressHandler::genBaseAddress(unsigned planeCount) {
  // For YCbCr, the possible plane counts are among 1 and 3.
  assert(planeCount > 0 && planeCount < 4);

  // PlaneBaseAddresses[0] is the same as original base addressed as passed in SRD, and is already pipeBankXored
  Value *virtualAddrPlane0 = m_regHandler->getReg(SqRsrcRegs::BaseAddress);
  m_planeBaseAddresses.push_back(virtualAddrPlane0);

  Value *pipeBankXor1 = nullptr;
  Value *pipeBankXor2 = nullptr;
  Value *pipeBankXorNone = m_builder->getInt32(0);

  switch (m_gfxIp->major) {
  case 6:
  case 7:
  case 8:
  case 9:
  case 11: {
    pipeBankXor1 = pipeBankXorNone;
    pipeBankXor2 = pipeBankXorNone;
    break;
  }
  case 10: {
    // Judge if current swizzle mode is SW_64KB_R_X
    Value *isSw64KbRxMode =
        m_builder->CreateICmpEQ(m_swizzleMode, m_builder->getInt32(Pal::Gfx9::Chip::SWIZZLE_MODE_ENUM::SW_64KB_R_X));

    const unsigned pipesLog2 = 3;
    const unsigned columnBits = 2;
    const unsigned xorPatternLen = 8;
    static const unsigned XorBankRot3b[] = {0, 4, 2, 6, 1, 5, 3, 7};
    static_assert((sizeof(XorBankRot3b) / sizeof(unsigned)) == xorPatternLen,
                  "The length of XorBankRot3b is different from xorPatternLen");

    // GFX10 hardware calculate pipe bank xor
    // SEE: Gfx10Lib::HwlComputePipeBankXor in pal\src\core\imported\addrlib\src\gfx10\gfx10addrlib.cpp
    auto gfx10HwlComputePipeBankXor = [&](unsigned surfIdx) -> Value * {
      return m_builder->getInt32(XorBankRot3b[surfIdx % xorPatternLen] << (pipesLog2 + columnBits));
    };

    // Calculate the pipeBankXor for second plane
    pipeBankXor1 = m_builder->CreateSelect(isSw64KbRxMode, gfx10HwlComputePipeBankXor(1), pipeBankXorNone);

    // Calculate the pipeBankXor for third plane
    pipeBankXor2 = m_builder->CreateSelect(isSw64KbRxMode, gfx10HwlComputePipeBankXor(2), pipeBankXorNone);
    break;
  }
  default: {
    llvm_unreachable("GFX IP not supported!");
    break;
  }
  }

  // Begin the calculating of planar 2 base addresses
  if (planeCount > 1) {
    // virtualAddrPlane1 = PlaneBaseAddresses[0] + addr256B(PitchY * HeightY)
    Value *virtualAddrPlane1 = m_builder->CreateAdd(
        virtualAddrPlane0, m_builder->CreateLShr(m_builder->CreateMul(m_pitchY, m_heightY), m_builder->getInt32(8)));
    // PlaneBaseAddresses[1] = (virtualAddrPlane1 | pipeBankXor1)
    m_planeBaseAddresses.push_back(m_builder->CreateOr(virtualAddrPlane1, pipeBankXor1));

    // Begin the calculating of planar 3 base addresses
    if (planeCount > 2) {
      // virtualAddrPlane2 = PlaneBaseAddresses[1] + addr256B(PitchCb * HeightCb)
      Value *virtualAddrPlane2 =
          m_builder->CreateAdd(virtualAddrPlane1, m_builder->CreateLShr(m_builder->CreateMul(m_pitchCb, m_heightCb),
                                                                        m_builder->getInt32(8)));
      // PlaneBaseAddresses[2] = (virtualAddrPlane2 | pipeBankXor2)
      m_planeBaseAddresses.push_back(m_builder->CreateOr(virtualAddrPlane2, pipeBankXor2));
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

  Value *result = m_builder->CreateAdd(x, m_builder->getInt32(align - 1));
  return m_builder->CreateAnd(result, m_builder->getInt32(~(align - 1)));
}

// =====================================================================================================================
// Calculate height and pitch
//
// @param bits : Channel bits
// @param bpp : Bits per pixel
// @param xBitCount : Effective channel bits
// @param planeNum : Number of planes
void YCbCrAddressHandler::genHeightAndPitch(unsigned bits, unsigned bpp, unsigned xBitCount, unsigned planeNum) {
  if (m_gfxIp->major > 8)
    m_swizzleMode = m_regHandler->getReg(SqRsrcRegs::SwizzleMode);

  switch (m_gfxIp->major) {
  case 6:
  case 7:
  case 8:
  case 9: {
    // Height = SqRsrcRegs::Height
    Value *height = m_regHandler->getReg(SqRsrcRegs::Height);
    // HeightHalf = Height * 0.5
    Value *heightHalf = m_builder->CreateLShr(height, m_one);

    m_heightY = height;
    m_heightCb = heightHalf;

    // Pitch = SqRsrcRegs::Pitch
    Value *pitch = m_regHandler->getReg(SqRsrcRegs::Pitch);
    // PitchHalf = Pitch * 0.5
    Value *pitchHalf = m_builder->CreateLShr(pitch, m_one);

    // PitchY * (xBitCount >> 3)
    m_pitchY = m_builder->CreateMul(pitch, m_builder->CreateLShr(m_builder->getInt32(xBitCount), 3));

    // PitchCb = PitchCb * (xBitCount >> 3)
    m_pitchCb = m_builder->CreateMul(pitchHalf, m_builder->CreateLShr(m_builder->getInt32(xBitCount), 3));

    break;
  }
  case 10:
  case 11: {
    const unsigned elementBytes = bpp >> 3;
    const unsigned pitchAlign = (256 / elementBytes);

    // Height = SqRsrcRegs::Height
    Value *height = m_regHandler->getReg(SqRsrcRegs::Height);
    m_heightY = height;

    // Width = SqRsrcRegs::Width
    Value *width = m_regHandler->getReg(SqRsrcRegs::Width);

    m_pitchY = power2Align(width, pitchAlign);
    // PitchY = PitchY * ElementBytes
    m_pitchY = m_builder->CreateMul(m_pitchY, m_builder->getInt32(elementBytes));

    // HeightHalf = Height * 0.5
    Value *heightHalf = m_builder->CreateLShr(height, m_one);
    m_heightCb = heightHalf;

    // WidthHalf = Width * 0.5
    Value *widthHalf = m_builder->CreateLShr(width, m_one);

    m_pitchCb = power2Align(widthHalf, pitchAlign);
    // PitchCb = PitchCb * ElementBytes
    m_pitchCb = m_builder->CreateMul(m_pitchCb, m_builder->getInt32(elementBytes));

    break;
  }
  default:
    llvm_unreachable("GFX IP not supported!");
    break;
  }
}
