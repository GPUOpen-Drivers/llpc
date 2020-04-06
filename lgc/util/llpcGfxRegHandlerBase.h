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
  * @file  llpcGfxRegHandlerBase.h
  * @brief LLPC header file: contains the definition of LLPC utility class GfxRegHandlerBase.
  ***********************************************************************************************************************
  */

#pragma once
#include "llpcBuilderImpl.h"
#include "llpcPipelineState.h"

namespace lgc
{

// General bits info for indexed DWORD
struct BitsInfo
{
    uint32_t index;
    uint32_t offset;
    uint32_t count;
};

// =====================================================================================================================
// Base class for handling GFX-specific registers
class GfxRegHandlerBase
{
public:
    // Set register
    void SetRegister(llvm::Value* pNewRegister);

    // Get register
    llvm::Value* GetRegister();

protected:
    // Constructor
    inline GfxRegHandlerBase(Builder* pBuilder, llvm::Value* pRegister)
    {
        m_pBuilder = pBuilder;
        SetRegister(pRegister);
    }

    // Return new DWORD with replacing the specific range of bits in old DWORD
    llvm::Value* ReplaceBits(llvm::Value* pDword, uint32_t offset, uint32_t count, llvm::Value* pNewBits);

    // Return the size of registered DWORDs
    inline uint32_t GetDwordsCount() { return m_dwords.size(); }

    // Get indexed DWORD
    inline llvm::Value* GetDword(uint32_t index)
    {
        ExtractDwordIfNecessary(index);
        return m_dwords[index];
    }

    // Set indexed DWORD
    inline void SetDword(uint32_t index, llvm::Value* pDword)
    {
        // Set the whole 32bits data
        m_dwords[index] = pDword;
        // If assign successfully, set corresponding mask bit to 1
        m_dirtyDwords |= 1 << index;
    }

    // Return if the specific DWORD is modified or not
    inline bool IsDwordModified(uint32_t index)
    {
        return (m_dirtyDwords & (1 << index));
    }

    // Get data from a range of bits in indexed DWORD according to BitsInfo
    llvm::Value* GetBits(const BitsInfo& bitsInfo);

    // Set data to a range of bits in indexed DWORD according to BitsInfo
    void SetBits(const BitsInfo& bitsInfo, llvm::Value* pNewBits);

private:
    // Load indexed DWORD from <n x i32> vector, if the specific DWORD is nullptr
    inline void ExtractDwordIfNecessary(uint32_t index)
    {
        if (m_dwords[index] == nullptr)
        {
            m_dwords[index] = m_pBuilder->CreateExtractElement(m_pRegister, m_pBuilder->getInt64(index));
        }
    }

protected:
    Builder* m_pBuilder;

private:
    // Contains (possibly updated) dwords for the register value. Each element will be a nullptr until it
    // is requested or updated for the first time.
    llvm::SmallVector<llvm::Value*, 8> m_dwords;

    // Combined <n x i32> vector containing the register value, which does not yet reflect the dwords
    // that are marked as dirty.
    llvm::Value* m_pRegister;

    // Bit-mask of dwords whose value was changed but is not yet reflected in m_pRegister.
    uint32_t m_dirtyDwords;
};

} // lgc
