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
    Builder* builder,  // [in] The builder handle
    Value*   reg) // [in] The registered target vec <n x i32>
    :
    GfxRegHandlerBase(builder, reg)
{
    m_one = builder->getInt32(1);
}

// =====================================================================================================================
// Common function for getting the current value for the hardware register
Value* GfxRegHandler::getRegCommon(
    unsigned regId) // The register ID, which is the index of BitsInfo and BitsState
{
    // Under two condition, we need to fetch the range of bits
    //  - The register has not being initialized
    //  - The register is being modified
    if ((!m_bitsState[regId].value ) ||
        (m_bitsState[regId].isModified))
    {
        // Fetch bits according to BitsInfo
        m_bitsState[regId].value = getBits(m_bitsInfo[regId]);
    }

    // Since the specified range of bits is cached, set it unmodified
    m_bitsState[regId].isModified = 0;

    // Return the cached value
    return m_bitsState[regId].value;
}

// =====================================================================================================================
// Get combined data from two seperate DWORDs
// Note: The return type is one DWORD, it doesn't support two DWORDs for now.
// TODO: Expand to support 2-DWORDs combination result.
Value* GfxRegHandler::getRegCombine(
    unsigned regIdLo, // The ID of low part register
    unsigned regIdHi) // Reg ID of high part register
{
    Value* regValueLo = getRegCommon(regIdLo);
    Value* regValueHi = getRegCommon(regIdHi);
    return m_builder->CreateOr(m_builder->CreateShl(regValueHi, m_bitsInfo[regIdLo].offset), regValueLo);
}

// =====================================================================================================================
// Set register value into two seperate DWORDs
// Note: The input pRegValue only supports one DWORD
void GfxRegHandler::setRegCombine(
    unsigned regIdLo,   // The ID of low part register
    unsigned regIdHi,   // Reg ID of high part register
    Value*   regValue) // [in] Data used for setting
{
    Value* regValueLo = m_builder->CreateIntrinsic(Intrinsic::amdgcn_ubfe,
                                                     m_builder->getInt32Ty(),
                                                     { regValue,
                                                       m_builder->getInt32(0),
                                                       m_builder->getInt32(m_bitsInfo[regIdLo].offset) });

    Value* regValueHi = m_builder->CreateLShr(regValue, m_builder->getInt32(m_bitsInfo[regIdLo].offset));

    setRegCommon(regIdLo, regValueLo);
    setRegCommon(regIdHi, regValueHi);
}

// =====================================================================================================================
// SqImgSampReg Bits infomation look up table (Gfx9-10)
// Refer to imported/chip/gfx9/gfx9_plus_merged_registers.h : SQ_IMG_SAMP_WORD
static constexpr BitsInfo SqImgSampRegBitsGfx9[static_cast<unsigned>(SqSampRegs::Count)] =
{
    { 0, 30, 2 }, // FilterMode
    { 2, 20, 2 }, // XyMagFilter
    { 2, 22, 2 }, // XyMinFilter
};

// =====================================================================================================================
// Helper class for handling Registers defined in SQ_IMG_SAMP_WORD
SqImgSampRegHandler::SqImgSampRegHandler(
    Builder*      builder,      // [in] Bound builder context
    Value*        reg,     // [in] Bound register vec <n x i32>
    GfxIpVersion* gfxIpVersion) // [in] Target GFX IP version
    :
    GfxRegHandler(builder, reg)
{
    m_gfxIpVersion = gfxIpVersion;

    switch (gfxIpVersion->major)
    {
    case 9:
    case 10:
        m_bitsInfo = SqImgSampRegBitsGfx9;
        break;
    default:
        llvm_unreachable("GFX IP is not supported!");
        break;
    }
    setBitsState(m_bitsState);
}

// =====================================================================================================================
// Get the current value for the hardware register
Value* SqImgSampRegHandler::getReg(
    SqSampRegs regId) // Register ID
{
    switch (regId)
    {
    case SqSampRegs::FilterMode:
    case SqSampRegs::xyMagFilter:
    case SqSampRegs::xyMinFilter:
        return getRegCommon(static_cast<unsigned>(regId));
    default:
        // TODO: More will be implemented.
        llvm_unreachable("Not implemented!");
        break;
    }
    return nullptr;
}

// =====================================================================================================================
// Set the current value for the hardware register
void SqImgSampRegHandler::setReg(
    SqSampRegs regId,      // Register ID
    Value*     regValue ) // [in] Value to set to this register
{
    switch (regId)
    {
    case SqSampRegs::FilterMode:
    case SqSampRegs::xyMagFilter:
    case SqSampRegs::xyMinFilter:
        setRegCommon(static_cast<unsigned>(regId), regValue);
        break;
    default:
        llvm_unreachable("Set \"IsTileOpt\" is not allowed!");
        break;
    }
}

// =====================================================================================================================
// SqImgSampReg Bits infomation look up table (Gfx9)
// Refer to imported/chip/gfx9/gfx9_plus_merged_registers.h : SQ_IMG_RSRC_WORD
static constexpr BitsInfo SqImgRsrcRegBitsGfx9[static_cast<unsigned>(SqRsrcRegs::Count)] =
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
static constexpr BitsInfo SqImgRsrcRegBitsGfx10[static_cast<unsigned>(SqRsrcRegs::Count)] =
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
    Builder*      builder,      // [in] Bound builder context
    Value*        reg,     // [in] Bound register vec <n x i32>
    GfxIpVersion* gfxIpVersion) // [in] Current GFX IP version
    :
    GfxRegHandler(builder, reg)
{
    m_gfxIpVersion = gfxIpVersion;

    switch (gfxIpVersion->major)
    {
    case 9:
        m_bitsInfo = SqImgRsrcRegBitsGfx9;
        break;
    case 10:
        m_bitsInfo = SqImgRsrcRegBitsGfx10;
        break;
    default:
        llvm_unreachable("GFX IP is not supported!");
        break;
    }
    setBitsState(m_bitsState);
}

// =====================================================================================================================
// Get the current value for the hardware register
Value* SqImgRsrcRegHandler::getReg(
    SqRsrcRegs regId) // Register ID
{
    switch (regId)
    {
    case SqRsrcRegs::BaseAddress:
    case SqRsrcRegs::Format:
    case SqRsrcRegs::DstSelXYZW:
    case SqRsrcRegs::Depth:
    case SqRsrcRegs::BcSwizzle:
        return getRegCommon(static_cast<unsigned>(regId));
    case SqRsrcRegs::Height:
    case SqRsrcRegs::Pitch:
        return m_builder->CreateAdd(getRegCommon(static_cast<unsigned>(regId)), m_one);
    case SqRsrcRegs::Width:
        switch (m_gfxIpVersion->major)
        {
        case 9:
            return m_builder->CreateAdd(getRegCommon(static_cast<unsigned>(regId)), m_one);
        case 10:
            return m_builder->CreateAdd(getRegCombine(static_cast<unsigned>(SqRsrcRegs::WidthLo),
                                                       static_cast<unsigned>(SqRsrcRegs::WidthHi)),
                                                       m_one);
        default:
            llvm_unreachable("GFX IP is not supported!");
            break;
        }
    case SqRsrcRegs::IsTileOpt:
        return m_builder->CreateICmpNE(getRegCommon(static_cast<unsigned>(regId)), m_builder->getInt32(0));
    default:
        // TODO: More will be implemented.
        llvm_unreachable("Not implemented!");
    }
    return nullptr;
}

// =====================================================================================================================
// Set the current value for the hardware register
void SqImgRsrcRegHandler::setReg(
    SqRsrcRegs regId,      // Register ID
    Value*     regValue)  // [in] Value to set to this register
{
    switch (regId)
    {
    case SqRsrcRegs::BaseAddress:
    case SqRsrcRegs::BaseAddressHi:
    case SqRsrcRegs::Format:
    case SqRsrcRegs::DstSelXYZW:
    case SqRsrcRegs::Depth:
    case SqRsrcRegs::BcSwizzle:
        setRegCommon(static_cast<unsigned>(regId), regValue);
        break;
    case SqRsrcRegs::Height:
    case SqRsrcRegs::Pitch:
        setRegCommon(static_cast<unsigned>(regId), m_builder->CreateSub(regValue, m_one));
        break;
    case SqRsrcRegs::Width:
        switch (m_gfxIpVersion->major)
        {
        case 9:
            setRegCommon(static_cast<unsigned>(regId), m_builder->CreateSub(regValue, m_one));
            break;
        case 10:
            setRegCombine(static_cast<unsigned>(SqRsrcRegs::WidthLo),
                          static_cast<unsigned>(SqRsrcRegs::WidthHi),
                          m_builder->CreateSub(regValue, m_one));
            break;
        default:
            llvm_unreachable("GFX IP is not supported!");
            break;
        }
        break;
    case SqRsrcRegs::IsTileOpt:
    case SqRsrcRegs::WidthLo:
    case SqRsrcRegs::WidthHi:
    case SqRsrcRegs::Count:
        llvm_unreachable("Bad SqImgRsrcRegHandler::SetReg!");
        break;
    }
}
