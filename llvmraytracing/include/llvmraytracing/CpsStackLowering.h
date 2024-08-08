/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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
#include "lgc/LgcIlCpsDialect.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"

namespace llvm {
class LLVMContext;
class Function;
class BitCastInst;
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

constexpr unsigned ContinuationStackAlignment = 4;

class CpsStackLowering {
public:
  CpsStackLowering(llvm::LLVMContext &Context, unsigned LoweredCpsStackAddrSpace)
      : TypeLower(Context), LoweredCpsStackAddrSpace{LoweredCpsStackAddrSpace} {
    BasePointer = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(llvm::Type::getInt8Ty(Context), LoweredCpsStackAddrSpace));
  }
  llvm::Function *lowerCpsStackOps(llvm::Function *Func, llvm::Function *GetGlobalMemBase, bool RequiresIncomingCsp,
                                   llvm::Value *CspStorage = nullptr);

  // Get continuation stack size (in bytes).
  unsigned getStackSizeInBytes() { return StackSizeInBytes; }

  inline unsigned getLoweredCpsStackAddrSpace() const { return LoweredCpsStackAddrSpace; }

  inline unsigned getLoweredCpsStackPointerSize(const llvm::DataLayout &Layout) {
    return Layout.getPointerSize(LoweredCpsStackAddrSpace);
  }

  static unsigned getContinuationStackAlignment() { return ContinuationStackAlignment; }

  CompilerUtils::TypeLowering TypeLower;

private:
  llvm::SmallVector<llvm::Type *> convertStackPtrToI32(CompilerUtils::TypeLowering &, llvm::Type *);
  void visitCpsAlloc(lgc::cps::AllocOp &);
  void visitCpsFree(lgc::cps::FreeOp &);
  void visitCpsPeek(lgc::cps::PeekOp &);
  void visitSetVsp(lgc::cps::SetVspOp &);
  void visitGetVsp(lgc::cps::GetVspOp &);
  void visitGetElementPtr(llvm::GetElementPtrInst &);
  void visitPtrToIntInst(llvm::PtrToIntInst &);
  void visitIntToPtrInst(llvm::IntToPtrInst &);
  void visitBitCastInst(llvm::BitCastInst &);
  void visitLoad(llvm::LoadInst &);
  void visitStore(llvm::StoreInst &);
  void visitContinue(lgc::ilcps::ContinueOp &);
  void visitWaitContinue(lgc::ilcps::WaitContinueOp &);
  llvm::Value *getRealMemoryAddress(llvm::IRBuilder<> &, llvm::Value *);
  llvm::Function *addOrInitCsp(llvm::Function *F, llvm::Function *GetGlobalMemBase, bool RequiresIncomingCsp);

  // Register a base pointer in the CpsStackLowering.
  // This is used to set the base address when using a stack residing in global
  // memory. BasePointer is by default a zero pointer in the
  // @LoweredCpsStackAddrSpace. During the lowering of load / store
  // instructions, a GEP will be constructed that uses the base pointer and the
  // corresponding CSP as offset for the source / dest addresses. In case
  // @setRealBasePointer never was called, this just creates a pointer out of an
  // offset.
  void setRealBasePointer(llvm::Value *BasePointer) { this->BasePointer = BasePointer; }

  llvm::Value *loadCsp(llvm::IRBuilder<> &Builder);

  llvm::Module *Mod;
  llvm::AllocaInst *CpsStackAlloca = nullptr;
  unsigned LoweredCpsStackAddrSpace;
  unsigned StackSizeInBytes = 0;
  llvm::Value *BasePointer = nullptr;
};
