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
 * @file  AddressExtender.cpp
 * @brief LGC source file: Utility class to extend an i32 address into a 64-bit pointer
 ***********************************************************************************************************************
 */
#include "lgc/util/AddressExtender.h"
#include "lgc/state/Defs.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Get first insertion point in the function, after PC-getting code if already inserted.
Instruction *AddressExtender::getFirstInsertionPt() {
  if (m_pc)
    return m_pc->getNextNode();
  return &*m_func->front().getFirstNonPHIOrDbgOrAlloca();
}

// =====================================================================================================================
// Extend an i32 into a 64-bit pointer
//
// @param addr32 : Address as 32-bit value
// @param highHalf : Value to use for high half; The constant HighAddrPc to use PC
// @param ptrTy : Type to cast pointer to
// @param builder : IRBuilder to use, already set to the required insert point
// @returns : 64-bit pointer value
Instruction *AddressExtender::extend(Value *addr32, Value *highHalf, Type *ptrTy, IRBuilder<> &builder) {
  Value *ptr = nullptr;
  ConstantInt *highHalfConst = dyn_cast<ConstantInt>(highHalf);
  if (highHalfConst && highHalfConst->getZExtValue() == HighAddrPc) {
    // Extend with PC.
    ptr = builder.CreateInsertElement(getPc(), addr32, uint64_t(0));
  } else {
    // Extend with given value
    ptr = builder.CreateInsertElement(PoisonValue::get(FixedVectorType::get(builder.getInt32Ty(), 2)), addr32,
                                      uint64_t(0));
    ptr = builder.CreateInsertElement(ptr, highHalf, 1);
  }
  ptr = builder.CreateBitCast(ptr, builder.getInt64Ty());
  return cast<Instruction>(builder.CreateIntToPtr(ptr, ptrTy));
}

// =====================================================================================================================
// Get PC value as v2i32. The caller is only using the high half, so this only writes a single instance of the
// code at the start of the function.
Instruction *AddressExtender::getPc() {
  if (!m_pc) {
    // This uses its own builder, as it wants to insert at the start of the function, whatever the caller
    // is doing.
    IRBuilder<> builder(m_func->getContext());
    builder.SetInsertPointPastAllocas(m_func);
    Value *pc = builder.CreateIntrinsic(llvm::Intrinsic::amdgcn_s_getpc, {}, {});
    pc = cast<Instruction>(builder.CreateBitCast(pc, FixedVectorType::get(builder.getInt32Ty(), 2)));
    m_pc = cast<Instruction>(pc);
  }
  return m_pc;
}
