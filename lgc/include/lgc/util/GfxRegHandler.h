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
 * @file  GfxRegHandler.h
 * @brief LLPC header file: contains the definition of LLPC utility class GfxRegHandler.
 *
 * @details The class handles symbolic expressions over values read from hardware graphics registers, materialized as
 *          llvm instructions. The code maintains a map from a register ID to an value with the current symbolic
 *          expression describing the updated state of the register.
 *
 * @sa llpc/docs/DdnGraphicsRegisterHandler.md
 ***********************************************************************************************************************
 */

#pragma once
#include "lgc/state/TargetInfo.h"
#include "lgc/util/GfxRegHandlerBase.h"

namespace lgc {

// =====================================================================================================================
// The goal of the helper structs is to maintain the newest symbolic state of a register and eventually produce
// optimized llvm instruction sequences for these states.
//
//    -------------------------------------------------------------------------------
//   | value     | isModified |                       state                          |
//    -------------------------------------------------------------------------------
//   | nullptr   | false/true | The value is never being touched, need load value    |
//   | effective | true       | The value is being modified, need reload value       |
//   | effective | false      | The value is latest cached, can be accessed directly |
//    -------------------------------------------------------------------------------
// Note: the value mentioned above are all in symbolic state and the load/reload are symbolic expressions.
struct BitsState {
  llvm::Value *value = nullptr;
  bool isModified = false;
};

// =====================================================================================================================
// Helper class for handling graphics registers.
// Note: 1) Don't use GfxRegHandler directly, please implement your own register helper class, such as
// SqImgSampRegHelper
//       2) The ID (enum) used in this class is determined by BitsInfo
//       3) The count of BisState used in this class is determined by BitsInfo
// e.g.
//       ID                              BitsInfo                                 BitsState
//       {                               {                                        {
//         FilterMode = 0,                 { 0, 30, 2 }, // FilterMode              {nullptr, false},
//         xyMagFilter,         <==        { 2, 20, 2 }, // XyMagFilter    ==>      {nullptr, false},
//         xyMinFilter,                    { 2, 22, 2 }, // XyMinFilter             {nullptr, false},
//       };                              };                                       };
class GfxRegHandler : public GfxRegHandlerBase {
protected:
  GfxRegHandler(llvm::IRBuilder<> *builder, llvm::Value *reg);

  // Common function for getting the current value for the hardware register
  llvm::Value *getRegCommon(unsigned regId);

  // Common function for setting the current value for the hardware register
  void setRegCommon(unsigned regId, llvm::Value *val) {
    setBits(m_bitsInfo[regId], val);
    // It is assumed the register is being modified
    m_bitsState[regId].isModified = 1;
  }

  // Get combined data from two separate dwords
  llvm::Value *getRegCombine(unsigned regIdLo, unsigned regIdHi);

  // Set data into two separate dwords
  void setRegCombine(unsigned regIdLo, unsigned regIdHi, llvm::Value *reg);

  // Get current value state for the hardware register
  const BitsState *getBitsState() { return m_bitsState; }

  // Set current value state for the hardware register
  void setBitsState(BitsState *bitsState) { m_bitsState = bitsState; }

protected:
  llvm::Value *m_one = nullptr; // Int32 constant one
  GfxIpVersion *m_gfxIpVersion = nullptr;
  const BitsInfo *m_bitsInfo = nullptr;

private:
  BitsState *m_bitsState = nullptr;
};

// SqImgSampRegs ID
// Corresponds to SqImgSampRegBitsGfx9
enum class SqSampRegs {
  FilterMode = 0,
  xyMagFilter,
  xyMinFilter,

  Count,
};

// =====================================================================================================================
// Helper class for handling Registers defined in SQ_IMG_SAMP_WORD
class SqImgSampRegHandler : public GfxRegHandler {
public:
  SqImgSampRegHandler(llvm::IRBuilder<> *builder, llvm::Value *reg, GfxIpVersion *gfxIpVersion);

  // Get the current value for the hardware register
  llvm::Value *getReg(SqSampRegs regId);

  // Set the current value for the hardware register
  void setReg(SqSampRegs regId, llvm::Value *regValue);

private:
  BitsState m_bitsState[static_cast<unsigned>(SqSampRegs::Count)];
};

// SqImgRsrcRegisters ID
// Merged registers index regarding to SqImgRsrcRegBitsGfx9 and SqImgRsrcRegBitsGfx10
enum class SqRsrcRegs {
  BaseAddress = 0,
  BaseAddressHi,
  Format,
  Width, // only gfx9 and before
  Height,
  DstSelXYZW,
  SwizzleMode,
  Depth,
  Pitch,
  BcSwizzle,

  // The following are introduced in gfx10.
  WidthLo,
  WidthHi,

  Count,
};

// =====================================================================================================================
// Helper class for handling Registers defined in SQ_IMG_RSRC_WORD
class SqImgRsrcRegHandler : public GfxRegHandler {
public:
  SqImgRsrcRegHandler(llvm::IRBuilder<> *builder, llvm::Value *reg, GfxIpVersion *gfxIpVersion);

  // Get the current value for the hardware register
  llvm::Value *getReg(SqRsrcRegs regId);

  // Set the current value for the hardware register
  void setReg(SqRsrcRegs regId, llvm::Value *regValue);

private:
  BitsState m_bitsState[static_cast<unsigned>(SqRsrcRegs::Count)];
};

} // namespace lgc
