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
* @file  llpcGfxRegHandler.cpp
* @brief LLPC source file: Implementation of LLPC utility class GfxRegHandler
***********************************************************************************************************************
*/
#include "llpcGfxRegHandler.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
GfxRegHandler::GfxRegHandler(
    Builder* pBuilder,  // [in] The builder handle
    Value*   pRegister) // [in] The registered target vec <n x i32>
    :
    GfxRegHandlerBase(pBuilder, pRegister)
{
    m_pOne = pBuilder->getInt32(1);
}

// =====================================================================================================================
// Common function for getting the current value for the hardware register
Value* GfxRegHandler::GetRegCommon(
    uint32_t regId) // The register ID, which is the index of BitsInfo and BitsState
{
    // Under two condition, we need to fetch the range of bits
    //  - The register has not being initialized
    //  - The register is being modified
    if ((m_pBitsState[regId].pValue == nullptr) ||
        (m_pBitsState[regId].isModified))
    {
        // Fetch bits according to BitsInfo
        m_pBitsState[regId].pValue = GetBits(m_pBitsInfo[regId]);
    }

    // Since the specified range of bits is cached, set it unmodified
    m_pBitsState[regId].isModified = 0;

    // Return the cached value
    return m_pBitsState[regId].pValue;
}

// =====================================================================================================================
// Get combined data from two seperate DWORDs
// Note: The return type is one DWORD, it doesn't support two DWORDs for now.
// TODO: Expand to support 2-DWORDs combination result.
Value* GfxRegHandler::GetRegCombine(
    uint32_t regIdLo, // The ID of low part register
    uint32_t regIdHi) // Reg ID of high part register
{
    Value* pRegValueLo = GetRegCommon(regIdLo);
    Value* pRegValueHi = GetRegCommon(regIdHi);
    return m_pBuilder->CreateOr(m_pBuilder->CreateShl(pRegValueHi, m_pBitsInfo[regIdLo].offset), pRegValueLo);
}

// =====================================================================================================================
// Set register value into two seperate DWORDs
// Note: The input pRegValue only supports one DWORD
void GfxRegHandler::SetRegCombine(
    uint32_t regIdLo,   // The ID of low part register
    uint32_t regIdHi,   // Reg ID of high part register
    Value*   pRegValue) // [in] Data used for setting
{
    Value* pRegValueLo = m_pBuilder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                     m_pBuilder->getInt32Ty(),
                                                     { pRegValue,
                                                       m_pBuilder->getInt32(0),
                                                       m_pBuilder->getInt32(m_pBitsInfo[regIdLo].offset) });

    Value* pRegValueHi = m_pBuilder->CreateLShr(pRegValue, m_pBuilder->getInt32(m_pBitsInfo[regIdLo].offset));

    SetRegCommon(regIdLo, pRegValueLo);
    SetRegCommon(regIdHi, pRegValueHi);
}

// =====================================================================================================================
// SqImgSampReg Bits infomation look up table (Gfx9-10)
// Refer to imported/chip/gfx9/gfx9_plus_merged_registers.h : SQ_IMG_SAMP_WORD
static constexpr BitsInfo SqImgSampRegBitsGfx9[static_cast<uint32_t>(SqSampRegs::Count)] =
{
    { 0, 30, 2 }, // FilterMode
    { 2, 20, 2 }, // XyMagFilter
    { 2, 22, 2 }, // XyMinFilter
};

// =====================================================================================================================
// Helper class for handling Registers defined in SQ_IMG_SAMP_WORD
SqImgSampRegHandler::SqImgSampRegHandler(
    Builder*      pBuilder,      // [in] Bound builder context
    Value*        pRegister,     // [in] Bound register vec <n x i32>
    GfxIpVersion* pGfxIpVersion) // [in] Target GFX IP version
    :
    GfxRegHandler(pBuilder, pRegister)
{
    m_pGfxIpVersion = pGfxIpVersion;

    switch (pGfxIpVersion->major)
    {
    case 9:
    case 10:
        m_pBitsInfo = SqImgSampRegBitsGfx9;
        break;
    default:
        llvm_unreachable("GFX IP is not supported!");
        break;
    }
    SetBitsState(m_pBitsState);
}

// =====================================================================================================================
// Get the current value for the hardware register
Value* SqImgSampRegHandler::GetReg(
    SqSampRegs regId) // Register ID
{
    switch (regId)
    {
    case SqSampRegs::FilterMode:
    case SqSampRegs::xyMagFilter:
    case SqSampRegs::xyMinFilter:
        return GetRegCommon(static_cast<uint32_t>(regId));
    default:
        // TODO: More will be implemented.
        llvm_unreachable("Not implemented!");
        break;
    }
    return nullptr;
}

// =====================================================================================================================
// Set the current value for the hardware register
void SqImgSampRegHandler::SetReg(
    SqSampRegs regId,      // Register ID
    Value*     pRegValue ) // [in] Value to set to this register
{
    switch (regId)
    {
    case SqSampRegs::FilterMode:
    case SqSampRegs::xyMagFilter:
    case SqSampRegs::xyMinFilter:
        SetRegCommon(static_cast<uint32_t>(regId), pRegValue);
        break;
    default:
        llvm_unreachable("Set \"IsTileOpt\" is not allowed!");
        break;
    }
}

// =====================================================================================================================
// SqImgSampReg Bits infomation look up table (Gfx9)
// Refer to imported/chip/gfx9/gfx9_plus_merged_registers.h : SQ_IMG_RSRC_WORD
static constexpr BitsInfo SqImgRsrcRegBitsGfx9[static_cast<uint32_t>(SqRsrcRegs::Count)] =
{
    { 0,  0, 32 }, // BaseAddress
    { 1,  0,  8 }, // BaseAddressHi
    { 1, 20,  9 }, // Format
    { 2,  0, 14 }, // Width
    { 2, 14, 14 }, // Height
    { 3,  0, 12 }, // DstSelXYZW
    { 3, 20,  5 }, // IsTileOpt
    { 4,  0, 13 }, // Depth
    { 4, 13, 12 }, // Pitch
    { 4, 29,  3 }, // BcSwizzle
    {},            // WidthLo
    {},            // WidthHi
};

// =====================================================================================================================
// SqImgSampReg Bits infomation look up table (Gfx10)
// TODO: update comment when the registers file is available
static constexpr BitsInfo SqImgRsrcRegBitsGfx10[static_cast<uint32_t>(SqRsrcRegs::Count)] =
{
    { 0,  0, 32 }, // BaseAddress
    { 1,  0,  8 }, // BaseAddressHi
    { 1, 20,  9 }, // Format
    {},            // Width
    { 2, 14, 16 }, // Height
    { 3,  0, 12 }, // DstSelXYZW
    { 3, 20,  5 }, // IsTileOpt
    { 4,  0, 16 }, // Depth
    {},            // Pitch
    { 3, 25,  3 }, // BcSwizzle
    { 1, 30,  2 }, // WidthLo
    { 2,  0, 14 }, // WidthHi
};

// =====================================================================================================================
// Helper class for handling Registers defined in SQ_IMG_RSRC_WORD
SqImgRsrcRegHandler::SqImgRsrcRegHandler(
    Builder*      pBuilder,      // [in] Bound builder context
    Value*        pRegister,     // [in] Bound register vec <n x i32>
    GfxIpVersion* pGfxIpVersion) // [in] Current GFX IP version
    :
    GfxRegHandler(pBuilder, pRegister)
{
    m_pGfxIpVersion = pGfxIpVersion;

    switch (pGfxIpVersion->major)
    {
    case 9:
        m_pBitsInfo = SqImgRsrcRegBitsGfx9;
        break;
    case 10:
        m_pBitsInfo = SqImgRsrcRegBitsGfx10;
        break;
    default:
        llvm_unreachable("GFX IP is not supported!");
        break;
    }
    SetBitsState(m_pBitsState);
}

// =====================================================================================================================
// Get the current value for the hardware register
Value* SqImgRsrcRegHandler::GetReg(
    SqRsrcRegs regId) // Register ID
{
    switch (regId)
    {
    case SqRsrcRegs::BaseAddress:
    case SqRsrcRegs::Format:
    case SqRsrcRegs::DstSelXYZW:
    case SqRsrcRegs::Depth:
    case SqRsrcRegs::BcSwizzle:
        return GetRegCommon(static_cast<uint32_t>(regId));
    case SqRsrcRegs::Height:
    case SqRsrcRegs::Pitch:
        return m_pBuilder->CreateAdd(GetRegCommon(static_cast<uint32_t>(regId)), m_pOne);
    case SqRsrcRegs::Width:
        switch (m_pGfxIpVersion->major)
        {
        case 9:
            return m_pBuilder->CreateAdd(GetRegCommon(static_cast<uint32_t>(regId)), m_pOne);
        case 10:
            return m_pBuilder->CreateAdd(GetRegCombine(static_cast<uint32_t>(SqRsrcRegs::WidthLo),
                                                       static_cast<uint32_t>(SqRsrcRegs::WidthHi)),
                                                       m_pOne);
        default:
            llvm_unreachable("GFX IP is not supported!");
            break;
        }
    case SqRsrcRegs::IsTileOpt:
        return m_pBuilder->CreateICmpNE(GetRegCommon(static_cast<uint32_t>(regId)), m_pBuilder->getInt32(0));
    default:
        // TODO: More will be implemented.
        llvm_unreachable("Not implemented!");
    }
    return nullptr;
}

// =====================================================================================================================
// Set the current value for the hardware register
void SqImgRsrcRegHandler::SetReg(
    SqRsrcRegs regId,      // Register ID
    Value*     pRegValue)  // [in] Value to set to this register
{
    switch (regId)
    {
    case SqRsrcRegs::BaseAddress:
    case SqRsrcRegs::BaseAddressHi:
    case SqRsrcRegs::Format:
    case SqRsrcRegs::DstSelXYZW:
    case SqRsrcRegs::Depth:
    case SqRsrcRegs::BcSwizzle:
        SetRegCommon(static_cast<uint32_t>(regId), pRegValue);
        break;
    case SqRsrcRegs::Height:
    case SqRsrcRegs::Pitch:
        SetRegCommon(static_cast<uint32_t>(regId), m_pBuilder->CreateSub(pRegValue, m_pOne));
        break;
    case SqRsrcRegs::Width:
        switch (m_pGfxIpVersion->major)
        {
        case 9:
            SetRegCommon(static_cast<uint32_t>(regId), m_pBuilder->CreateSub(pRegValue, m_pOne));
            break;
        case 10:
            SetRegCombine(static_cast<uint32_t>(SqRsrcRegs::WidthLo),
                          static_cast<uint32_t>(SqRsrcRegs::WidthHi),
                          m_pBuilder->CreateSub(pRegValue, m_pOne));
            break;
        default:
            llvm_unreachable("GFX IP is not supported!");
            break;
        }
        break;
    case SqRsrcRegs::IsTileOpt:
        llvm_unreachable("Set \"IsTileOpt\" is not allowed!");
        break;
    }
}
