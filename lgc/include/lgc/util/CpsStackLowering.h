/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  CpsStackLowering.h
 * @brief LLPC header file: contains declaration of class lgc::CpsStackLowering.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgccps/LgcCpsDialect.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/util/TypeLowering.h"
#include "llvm/ADT/SmallVector.h"

namespace lgc {
constexpr unsigned continuationStackAlignment = 4;

inline unsigned getLoweredCpsStackAddrSpace() {
  return ADDR_SPACE_PRIVATE;
}
inline unsigned getLoweredCpsStackPointerSize(const llvm::DataLayout &layout) {
  return layout.getPointerSize(getLoweredCpsStackAddrSpace());
}

class CpsStackLowering {
public:
  CpsStackLowering(llvm::LLVMContext &context) : m_typeLowering(context) {}
  void lowerCpsStackOps(llvm::Function &function, llvm::Value *);

  TypeLowering m_typeLowering;

private:
  void visitCpsAlloc(cps::AllocOp &alloc);
  void visitCpsFree(cps::FreeOp &freeOp);
  void visitCpsPeek(cps::PeekOp &peekOp);
  void visitSetVsp(cps::SetVspOp &setVsp);
  void visitGetVsp(cps::GetVspOp &getVsp);
  void visitGetElementPtr(llvm::GetElementPtrInst &getElemPtrInst);
  void visitPtrToIntInst(llvm::PtrToIntInst &ptr2Int);
  void visitIntToPtrInst(llvm::IntToPtrInst &int2Ptr);
  void visitLoad(llvm::LoadInst &load);
  void visitStore(llvm::StoreInst &store);

  llvm::Module *m_module;
  llvm::Value *m_cpsStackAlloca;
};

} // namespace lgc
