/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *deal in the Software without restriction, including without limitation the
 *rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  CpsStackLowering.h
 * @brief Contains declaration of class CpsStackLowering.
 ***********************************************************************************************************************
 */
#pragma once

#include "compilerutils/TypeLowering.h"
#include "lgc/LgcCpsDialect.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
class LLVMContext;
class Function;
class GetElementPtrInst;
class PtrToIntInst;
class IntToPtrInst;
class LoadInst;
class StoreInst;
class Module;
class Value;
class Type;
class DataLayout;
} // namespace llvm

using namespace lgc::cps;

constexpr unsigned ContinuationStackAlignment = 4;

class CpsStackLowering {
public:
  CpsStackLowering(llvm::LLVMContext &Context,
                   unsigned LoweredCpsStackAddrSpace)
      : TypeLower(Context), LoweredCpsStackAddrSpace{LoweredCpsStackAddrSpace} {
  }
  void lowerCpsStackOps(llvm::Function &, llvm::Value *);
  // Get continuation stack size (in bytes).
  unsigned getStackSizeInBytes() { return StackSizeInBytes; }

  inline unsigned getLoweredCpsStackAddrSpace() const {
    return LoweredCpsStackAddrSpace;
  }

  inline unsigned
  getLoweredCpsStackPointerSize(const llvm::DataLayout &Layout) {
    return Layout.getPointerSize(LoweredCpsStackAddrSpace);
  }

  TypeLowering TypeLower;

private:
  llvm::SmallVector<llvm::Type *> convertCpsStackPointer(TypeLowering &,
                                                         llvm::Type *);
  void visitCpsAlloc(AllocOp &);
  void visitCpsFree(FreeOp &);
  void visitCpsPeek(PeekOp &);
  void visitSetVsp(SetVspOp &);
  void visitGetVsp(GetVspOp &);
  void visitGetElementPtr(llvm::GetElementPtrInst &);
  void visitPtrToIntInst(llvm::PtrToIntInst &);
  void visitIntToPtrInst(llvm::IntToPtrInst &);
  void visitLoad(llvm::LoadInst &);
  void visitStore(llvm::StoreInst &);

  llvm::Module *Mod;
  llvm::Value *CpsStackAlloca;
  unsigned LoweredCpsStackAddrSpace;
  unsigned StackSizeInBytes = 0;
};
