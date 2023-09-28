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
 * @file  AddressExtender.h
 * @brief LGC header file: Utility class to extend an i32 address into a 64-bit pointer
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/IRBuilder.h"

namespace llvm {
class Function;
class Instruction;
class Type;
class Value;
} // namespace llvm

namespace lgc {

// =====================================================================================================================
// Utility class to generate code to extend an i32 address into a 64-bit pointer, using either PC or a given
// constant high half value.
class AddressExtender {
public:
  // Constructor
  AddressExtender(llvm::Function *func) : m_func(func) {}

  // Get first insertion point in the function, after PC-getting code if already inserted.
  llvm::Instruction *getFirstInsertionPt();

  // Extend an i32 into a 64-bit pointer
  //
  // @param addr32 : Address as 32-bit value
  // @param highHalf : Value to use for high half; The constant HighAddrPc to use PC
  // @param ptrTy : Type to cast pointer to
  // @param builder : IRBuilder to use, already set to the required insert point
  // @returns : 64-bit pointer value
  llvm::Instruction *extend(llvm::Value *addr32, llvm::Value *highHalf, llvm::Type *ptrTy, llvm::IRBuilder<> &builder);

  // Extend an i32 into a 64-bit pointer using the high 32-bits of the PC
  //
  // @param addr32 : Address as 32-bit value
  // @param ptrTy : Type to cast pointer to
  // @param builder : IRBuilder to use, already set to the required insert point
  // @returns : 64-bit pointer value
  llvm::Instruction *extendWithPc(llvm::Value *addr32, llvm::Type *ptrTy, llvm::IRBuilder<> &builder);

private:
  // Get PC value as v2i32.
  llvm::Instruction *getPc();

  llvm::Function *m_func;
  llvm::Instruction *m_pc = nullptr;
};

} // namespace lgc
