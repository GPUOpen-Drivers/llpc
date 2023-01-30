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
 * @file  GfxRegHandlerBase.h
 * @brief LLPC header file: contains the definition of LLPC utility class GfxRegHandlerBase.
 ***********************************************************************************************************************
 */

#pragma once
#include "llvm/IR/IRBuilder.h"

namespace lgc {

// General bits info for indexed dword
struct BitsInfo {
  unsigned index;
  unsigned offset;
  unsigned count;
};

// =====================================================================================================================
// Base class for handling GFX-specific registers
class GfxRegHandlerBase {
public:
  // Set register
  void setRegister(llvm::Value *newRegister);

  // Get register
  llvm::Value *getRegister();

protected:
  // Constructor
  inline GfxRegHandlerBase(llvm::IRBuilder<> *builder, llvm::Value *reg) {
    m_builder = builder;
    setRegister(reg);
  }

  // Return new dword with replacing the specific range of bits in old dword
  llvm::Value *replaceBits(llvm::Value *dword, unsigned offset, unsigned count, llvm::Value *newBits);

  // Return the size of registered dwords
  inline unsigned getDwordsCount() { return m_dwords.size(); }

  // Get indexed dword
  inline llvm::Value *getDword(unsigned index) {
    extractDwordIfNecessary(index);
    return m_dwords[index];
  }

  // Set indexed dword
  inline void setDword(unsigned index, llvm::Value *dword) {
    // Set the whole 32bits data
    m_dwords[index] = dword;
    // If assign successfully, set corresponding mask bit to 1
    m_dirtyDwords |= 1 << index;
  }

  // Return if the specific dword is modified or not
  inline bool isDwordModified(unsigned index) { return (m_dirtyDwords & (1 << index)); }

  // Get data from a range of bits in indexed dword according to BitsInfo
  llvm::Value *getBits(const BitsInfo &bitsInfo);

  // Set data to a range of bits in indexed dword according to BitsInfo
  void setBits(const BitsInfo &bitsInfo, llvm::Value *newBits);

private:
  // Load indexed dword from <n x i32> vector, if the specific dword is nullptr
  inline void extractDwordIfNecessary(unsigned index) {
    if (!m_dwords[index])
      m_dwords[index] = m_builder->CreateExtractElement(m_reg, m_builder->getInt64(index));
  }

protected:
  llvm::IRBuilder<> *m_builder;

private:
  // Contains (possibly updated) dwords for the register value. Each element will be a nullptr until it
  // is requested or updated for the first time.
  llvm::SmallVector<llvm::Value *, 8> m_dwords;

  // Combined <n x i32> vector containing the register value, which does not yet reflect the dwords
  // that are marked as dirty.
  llvm::Value *m_reg;

  // Bit-mask of dwords whose value was changed but is not yet reflected in m_pReg.
  unsigned m_dirtyDwords;
};

} // namespace lgc
