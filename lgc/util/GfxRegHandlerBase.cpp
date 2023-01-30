/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  GfxRegHandlerBase.cpp
* @brief LLPC source file: Implementation of LLPC utility class GfxRegHandlerBase
***********************************************************************************************************************
*/
#include "lgc/util/GfxRegHandlerBase.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Set register:
//   - Clear old dwords vector
//   - Fill dwords vector with nullptr
//   - Init dirty mask to all clean
//
// @param newRegister : A <n x i32> vector
void GfxRegHandlerBase::setRegister(Value *newRegister) {
  assert(newRegister->getType()->isIntOrIntVectorTy());

  if (auto vectorTy = dyn_cast<FixedVectorType>(newRegister->getType())) {
    unsigned count = vectorTy->getNumElements();

    // Clear previously stored dwords
    m_dwords.clear();

    // Resize to specific number of dwords
    for (unsigned i = 0; i < count; i++)
      m_dwords.push_back(nullptr);
  } else {
    assert(newRegister->getType() == m_builder->getInt32Ty());
    m_dwords.push_back(nullptr);
  }

  m_reg = newRegister;
  m_dirtyDwords = 0u;
}

// =====================================================================================================================
// Get register
//   - Overwrite dwords in <n x i32> register if marked as dirty
Value *GfxRegHandlerBase::getRegister() {
  unsigned dirtyMask = m_dirtyDwords;

  // Overwrite if the specific dword is being masked as dirty
  for (unsigned i = 0; dirtyMask > 0; dirtyMask >>= 1) {
    if (dirtyMask & 1) {
      m_reg = m_builder->CreateInsertElement(m_reg, m_dwords[i], m_builder->getInt64(i));
    }

    i++;
  }

  // Set mask as all clean since we've update the registered <nxi32>
  m_dirtyDwords = 0u;

  // Just return updated m_pReg
  return m_reg;
}

// =====================================================================================================================
// Get data from a range of bits in indexed dword according to BitsInfo
//
// @param bitsInfo : The BitsInfo of data
Value *GfxRegHandlerBase::getBits(const BitsInfo &bitsInfo) {
  if (bitsInfo.count == 32)
    return getDword(bitsInfo.index);

  extractDwordIfNecessary(bitsInfo.index);

  return m_builder->CreateIntrinsic(
      Intrinsic::amdgcn_ubfe, m_builder->getInt32Ty(),
      {m_dwords[bitsInfo.index], m_builder->getInt32(bitsInfo.offset), m_builder->getInt32(bitsInfo.count)});
}

// =====================================================================================================================
// Set data to a range of bits in indexed dword according to BitsInfo
//
// @param bitsInfo : The BitsInfo of data's high part
// @param newBits : The new bits to set
void GfxRegHandlerBase::setBits(const BitsInfo &bitsInfo, Value *newBits) {
  extractDwordIfNecessary(bitsInfo.index);

  if (bitsInfo.count != 32) {
    Value *dwordsNew = replaceBits(m_dwords[bitsInfo.index], bitsInfo.offset, bitsInfo.count, newBits);
    setDword(bitsInfo.index, dwordsNew);
  } else
    setDword(bitsInfo.index, newBits);
}

// =====================================================================================================================
// Return new dword which is replaced [offset, offset + count) with newBits
//
// @param dword : Target dword
// @param offset : The first bit to be replaced
// @param count : The number of bits should be replaced
// @param newBits : The new bits to replace specified ones
Value *GfxRegHandlerBase::replaceBits(Value *dword, unsigned offset, unsigned count, Value *newBits) {
  // mask = ((1 << count) - 1) << offset
  // Result = (pDword & ~mask)|((pNewBits << offset) & mask)
  unsigned maskBits = ((1 << count) - 1) << offset;
  Value *mask = m_builder->getInt32(maskBits);
  Value *notMask = m_builder->getInt32(~maskBits);
  Value *beginBit = m_builder->getInt32(offset);
  newBits = m_builder->CreateAnd(m_builder->CreateShl(newBits, beginBit), mask);
  return m_builder->CreateOr(m_builder->CreateAnd(dword, notMask), newBits);
}
