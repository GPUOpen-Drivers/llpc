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

#include "continuations/CpsStackLowering.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include <functional>

using namespace llvm;
using namespace lgc::cps;

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(CpsStackLowering, TypeLower)

// =====================================================================================================================
// Type lowering rule that lowers cps stack pointer type to corresponding
// backend pointer type.
//
// @param typeLowering : the calling TypeLowering object
// @param type : the type to be converted
SmallVector<Type *>
CpsStackLowering::convertCpsStackPointer(TypeLowering &TypeLower, Type *Ty) {
  SmallVector<Type *> Types;

  if (auto *PtrTy = dyn_cast<PointerType>(Ty)) {
    if (PtrTy->getAddressSpace() == lgc::cps::stackAddrSpace)
      Types.push_back(
          PointerType::get(Ty->getContext(), LoweredCpsStackAddrSpace));
  }

  return Types;
}

// =====================================================================================================================
// Lower continuation stack operations in the function
//
// @param Function : the function to be processed
// @param CpsStorage : the alloca used for the holding the latest continuation
// stack pointer
void CpsStackLowering::lowerCpsStackOps(Function &Function, Value *CpsStorage) {
  Mod = Function.getParent();
  StackSizeInBytes = 0;
  CpsStackAlloca = CpsStorage;
  TypeLower.addRule(std::bind(&CpsStackLowering::convertCpsStackPointer, this,
                              std::placeholders::_1, std::placeholders::_2));
  auto *NewFunc = &Function;
  if (lgc::cps::isCpsFunction(Function))
    NewFunc = TypeLower.lowerFunctionArguments(Function);

  static const auto Visitor = llvm_dialects::VisitorBuilder<CpsStackLowering>()
                                  .nest(&TypeLowering::registerVisitors)
                                  .add(&CpsStackLowering::visitCpsAlloc)
                                  .add(&CpsStackLowering::visitCpsFree)
                                  .add(&CpsStackLowering::visitCpsPeek)
                                  .add(&CpsStackLowering::visitSetVsp)
                                  .add(&CpsStackLowering::visitGetVsp)
                                  .add(&CpsStackLowering::visitGetElementPtr)
                                  .add(&CpsStackLowering::visitPtrToIntInst)
                                  .add(&CpsStackLowering::visitIntToPtrInst)
                                  .add(&CpsStackLowering::visitLoad)
                                  .add(&CpsStackLowering::visitStore)
                                  .build();
  Visitor.visit(*this, *NewFunc);
  TypeLower.finishPhis();
  TypeLower.finishCleanup();
}

// =====================================================================================================================
// Lower getelementptr instruction
//
// @param function : the instruction
void CpsStackLowering::visitGetElementPtr(GetElementPtrInst &GEP) {
  if (GEP.getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  IRBuilder<> Builder(&GEP);

  SmallVector<Value *, 8> Indices(GEP.idx_begin(), GEP.idx_end());

  Value *NewGEP = nullptr;
  auto Values = TypeLower.getValue(GEP.getPointerOperand());
  auto *GEPVal = Values[0];
  auto *GEPTy = GEP.getSourceElementType();

  if (GEP.isInBounds())
    NewGEP = Builder.CreateInBoundsGEP(GEPTy, GEPVal, Indices);
  else
    NewGEP = Builder.CreateGEP(GEPTy, GEPVal, Indices);

  cast<Instruction>(NewGEP)->copyMetadata(GEP);

  TypeLower.replaceInstruction(&GEP, {NewGEP});
}

// =====================================================================================================================
// Lower load instruction
//
// @param function : the instruction
void CpsStackLowering::visitLoad(LoadInst &Load) {
  if (Load.getPointerAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(Load.getPointerOperand());
  Load.replaceUsesOfWith(Load.getPointerOperand(), Values[0]);
}

// =====================================================================================================================
// Lower store instruction
//
// @param function : the instruction
void CpsStackLowering::visitStore(llvm::StoreInst &Store) {
  if (Store.getPointerAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(Store.getPointerOperand());
  Store.replaceUsesOfWith(Store.getPointerOperand(), Values[0]);
}

// =====================================================================================================================
// Lower ptrtoint instruction
//
// @param function : the instruction
void CpsStackLowering::visitPtrToIntInst(llvm::PtrToIntInst &Ptr2Int) {
  if (Ptr2Int.getPointerAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(Ptr2Int.getOperand(0));
  Ptr2Int.replaceUsesOfWith(Ptr2Int.getOperand(0), Values[0]);
}

// =====================================================================================================================
// Lower inttoptr instruction
//
// @param function : the instruction
void CpsStackLowering::visitIntToPtrInst(llvm::IntToPtrInst &Int2Ptr) {
  if (Int2Ptr.getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  IRBuilder<> Builder(&Int2Ptr);
  auto *NewPtr = Builder.CreateIntToPtr(
      Int2Ptr.getOperand(0),
      PointerType::get(Builder.getContext(), LoweredCpsStackAddrSpace));
  TypeLower.replaceInstruction(&Int2Ptr, NewPtr);
}

// =====================================================================================================================
// Lower lgc.cps.alloc instruction
//
// @param function : the instruction
void CpsStackLowering::visitCpsAlloc(lgc::cps::AllocOp &AllocOp) {
  IRBuilder<> Builder(&AllocOp);

  Value *Size = AllocOp.getSize();
  const DataLayout &Layout = Mod->getDataLayout();
  Value *VSP = Builder.CreateAlignedLoad(
      Builder.getPtrTy(LoweredCpsStackAddrSpace), CpsStackAlloca,
      Align(getLoweredCpsStackPointerSize(Layout)));
  unsigned AlignedSize = alignTo(cast<ConstantInt>(Size)->getZExtValue(),
                                 ContinuationStackAlignment);
  StackSizeInBytes += AlignedSize;

  // update stack pointer
  Value *Ptr =
      Builder.CreateConstGEP1_32(Builder.getInt8Ty(), VSP, AlignedSize);
  Builder.CreateAlignedStore(Ptr, CpsStackAlloca,
                             Align(getLoweredCpsStackPointerSize(Layout)));

  TypeLower.replaceInstruction(&AllocOp, {VSP});
}

// =====================================================================================================================
// Lower lgc.cps.free instruction
//
// @param function : the instruction
void CpsStackLowering::visitCpsFree(lgc::cps::FreeOp &FreeOp) {
  IRBuilder<> Builder(&FreeOp);
  const DataLayout &Layout = Mod->getDataLayout();

  Value *VSP = Builder.CreateAlignedLoad(
      Builder.getPtrTy(LoweredCpsStackAddrSpace), CpsStackAlloca,
      Align(getLoweredCpsStackPointerSize(Layout)));
  Value *Size = FreeOp.getSize();
  unsigned AlignedSize = alignTo(cast<ConstantInt>(Size)->getZExtValue(),
                                 ContinuationStackAlignment);
  Value *Ptr =
      Builder.CreateConstGEP1_32(Builder.getInt8Ty(), VSP, -AlignedSize);
  // Assuming continuation stack grows upward.
  Builder.CreateAlignedStore(Ptr, CpsStackAlloca,
                             Align(getLoweredCpsStackPointerSize(Layout)));
  TypeLower.replaceInstruction(&FreeOp, {});
}

// =====================================================================================================================
// Lower lgc.cps.peek instruction
//
// @param function : the instruction
void CpsStackLowering::visitCpsPeek(lgc::cps::PeekOp &PeekOp) {
  IRBuilder<> Builder(&PeekOp);
  const DataLayout &Layout = Mod->getDataLayout();

  auto *Ptr = Builder.CreateAlignedLoad(
      Builder.getPtrTy(LoweredCpsStackAddrSpace), CpsStackAlloca,
      Align(getLoweredCpsStackPointerSize(Layout)));
  auto *Size = PeekOp.getSize();
  unsigned ImmSize = cast<ConstantInt>(Size)->getZExtValue();
  ImmSize = alignTo(ImmSize, ContinuationStackAlignment);
  // Assuming continuation stack grows upward.
  auto *Result =
      Builder.CreateGEP(Builder.getInt8Ty(), Ptr, {Builder.getInt32(-ImmSize)});
  TypeLower.replaceInstruction(&PeekOp, {Result});
}

// =====================================================================================================================
// Lower lgc.cps.set.VSP instruction
//
// @param function : the instruction
void CpsStackLowering::visitSetVsp(lgc::cps::SetVspOp &SetVsp) {
  IRBuilder<> B(&SetVsp);
  const DataLayout &Layout = Mod->getDataLayout();

  auto *Ptr = SetVsp.getPtr();
  auto Converted = TypeLower.getValue(Ptr);
  B.CreateAlignedStore(Converted[0], CpsStackAlloca,
                       Align(getLoweredCpsStackPointerSize(Layout)));
  TypeLower.replaceInstruction(&SetVsp, {});
}

// =====================================================================================================================
// Lower lgc.cps.get.VSP instruction
//
// @param function : the instruction
void CpsStackLowering::visitGetVsp(lgc::cps::GetVspOp &GetVsp) {
  IRBuilder<> B(&GetVsp);
  const DataLayout &Layout = Mod->getDataLayout();

  auto *Ptr =
      B.CreateAlignedLoad(B.getPtrTy(LoweredCpsStackAddrSpace), CpsStackAlloca,
                          Align(getLoweredCpsStackPointerSize(Layout)));
  TypeLower.replaceInstruction(&GetVsp, {Ptr});
}
