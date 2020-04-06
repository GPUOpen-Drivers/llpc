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
* @file  llpcGfxRegHandlerBase.cpp
* @brief LLPC source file: Implementation of LLPC utility class GfxRegHandlerBase
***********************************************************************************************************************
*/
#include "llpcGfxRegHandlerBase.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llpcTargetInfo.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Set register:
//   - Clear old DWORDs vector
//   - Fill DWORDs vector with nullptr
//   - Init dirty mask to all clean
void GfxRegHandlerBase::SetRegister(
    Value* pNewRegister) // [in] A <n x i32> vector
{
    assert(pNewRegister->getType()->isIntOrIntVectorTy());

    if (auto pVectorTy = dyn_cast<VectorType>(pNewRegister->getType()))
    {
        uint32_t count = pVectorTy->getNumElements();

        // Clear previously stored DWORDs
        m_dwords.clear();

        // Resize to specific number of DWORDs
        for (uint32_t i = 0; i < count; i++)
        {
            m_dwords.push_back(nullptr);
        }
    }
    else
    {
        assert(pNewRegister->getType() == m_pBuilder->getInt32Ty());
        m_dwords.push_back(nullptr);
    }

    m_pRegister = pNewRegister;
    m_dirtyDwords = 0u;
}

// =====================================================================================================================
// Get register
//   - Overwrite DWORDs in <n x i32> register if marked as dirty
Value* GfxRegHandlerBase::GetRegister()
{
    uint32_t dirtyMask = m_dirtyDwords;

    // Overwrite if the specific DWORD is being masked as dirty
    for (uint32_t i = 0; dirtyMask > 0; dirtyMask >>= 1)
    {
        if (dirtyMask & 1)
        {
            m_pRegister = m_pBuilder->CreateInsertElement(m_pRegister,
                                                          m_dwords[i],
                                                          m_pBuilder->getInt64(i));
        }

        i++;
    }

    // Set mask as all clean since we've update the registered <nxi32>
    m_dirtyDwords = 0u;

    // Just return updated m_pRegister
    return m_pRegister;
}

// =====================================================================================================================
// Get data from a range of bits in indexed DWORD according to BitsInfo
Value* GfxRegHandlerBase::GetBits(
    const BitsInfo& bitsInfo)    // [in] The BitsInfo of data
{
    if (bitsInfo.count == 32)
    {
        return GetDword(bitsInfo.index);
    }

    ExtractDwordIfNecessary(bitsInfo.index);

    return m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                       m_pBuilder->getInt32Ty(),
                                       { m_dwords[bitsInfo.index],
                                         m_pBuilder->getInt32(bitsInfo.offset),
                                         m_pBuilder->getInt32(bitsInfo.count) });
}

// =====================================================================================================================
// Set data to a range of bits in indexed DWORD according to BitsInfo
void GfxRegHandlerBase::SetBits(
    const BitsInfo& bitsInfo,    // [in] The BitsInfo of data's high part
    Value*          pNewBits)    // [in] The new bits to set
{
    ExtractDwordIfNecessary(bitsInfo.index);

    if (bitsInfo.count != 32)
    {
        Value* pDwordsNew = ReplaceBits(m_dwords[bitsInfo.index],
                                        bitsInfo.offset,
                                        bitsInfo.count,
                                        pNewBits);
        SetDword(bitsInfo.index, pDwordsNew);
    }
    else
    {
        SetDword(bitsInfo.index, pNewBits);
    }
}

// =====================================================================================================================
// Return new DWORD which is replaced [offset, offset + count) with pNewBits
Value* GfxRegHandlerBase::ReplaceBits(
    Value*   pDword,   // [in] Target DWORD
    uint32_t offset,   // The first bit to be replaced
    uint32_t count,    // The number of bits should be replaced
    Value*   pNewBits) // [in] The new bits to replace specified ones
{
    // mask = ((1 << count) - 1) << offset
    // Result = (pDword & ~mask)|((pNewBits << offset) & mask)
    uint32_t mask = ((1 << count) - 1) << offset;
    Value* pMask = m_pBuilder->getInt32(mask);
    Value* pNotMask = m_pBuilder->getInt32(~mask);
    Value* pBeginBit = m_pBuilder->getInt32(offset);
    pNewBits = m_pBuilder->CreateAnd(m_pBuilder->CreateShl(pNewBits, pBeginBit), pMask);
    return m_pBuilder->CreateOr(m_pBuilder->CreateAnd(pDword, pNotMask), pNewBits);
}
