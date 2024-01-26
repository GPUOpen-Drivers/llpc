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
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
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

#include "continuations/CpsStackLowering.h"
#include "continuations/ContinuationsUtil.h"
#include "lgc/LgcCpsDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

using namespace llvm;
using namespace lgc::cps;

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(CpsStackLowering, TypeLower)

SmallVector<Type *>
CpsStackLowering::convertStackPtrToI32(TypeLowering &TypeLower, Type *Ty) {
  SmallVector<Type *> Types;

  if (auto *PtrTy = dyn_cast<PointerType>(Ty)) {
    if (PtrTy->getAddressSpace() == lgc::cps::stackAddrSpace)
      Types.push_back(Type::getInt32Ty(TypeLower.getContext()));
  }

  return Types;
}

// =====================================================================================================================
// Lower continuation stack operations in the function
//
// @param Function : the function to be processed
// @param CpsStorage : the alloca used for the holding the latest continuation
// stack pointer
// @return: The new function, if Function was mutated, or the Function argument.
Function *CpsStackLowering::lowerCpsStackOps(Function &Function,
                                             Value *CpsStorage) {
  assert(cast<AllocaInst>(CpsStorage)->getAllocatedType()->isIntegerTy());

  Mod = Function.getParent();
  StackSizeInBytes = 0;
  CpsStackAlloca = cast<AllocaInst>(CpsStorage);
  TypeLower.addRule(std::bind(&CpsStackLowering::convertStackPtrToI32, this,
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
                                  .add(&CpsStackLowering::visitBitCastInst)
                                  .add(&CpsStackLowering::visitLoad)
                                  .add(&CpsStackLowering::visitStore)
                                  .build();
  Visitor.visit(*this, *NewFunc);
  TypeLower.finishPhis();
  TypeLower.finishCleanup();

  return NewFunc;
}

// =====================================================================================================================
// Lower getelementptr instruction
//
// @param GEP: the instruction
void CpsStackLowering::visitGetElementPtr(GetElementPtrInst &GEP) {
  if (GEP.getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  IRBuilder<> Builder(&GEP);

  auto Values = TypeLower.getValue(GEP.getPointerOperand());
  Value *AddChain = Values[0];

  const DataLayout &DL = GEP.getFunction()->getParent()->getDataLayout();
  unsigned BitWidth = DL.getIndexSizeInBits(GEP.getPointerAddressSpace());

  APInt ConstantOffset{BitWidth, 0};
  MapVector<Value *, APInt> VariableOffsets;

  [[maybe_unused]] bool Success =
      GEP.collectOffset(DL, BitWidth, VariableOffsets, ConstantOffset);
  assert(Success && "CpsStackLowering::visitGetElementPtr: GEP.collectOffset "
                    "did not succeed!");

  if (ConstantOffset.getSExtValue() != 0)
    AddChain = Builder.CreateAdd(
        AddChain, Builder.getInt32(ConstantOffset.getSExtValue()));

  for (const auto &[Index, Scaling] : VariableOffsets) {
    Value *ScaledVal = Index;

    if (Scaling.getSExtValue() != 1)
      ScaledVal =
          Builder.CreateMul(Index, Builder.getInt32(Scaling.getSExtValue()));

    AddChain = Builder.CreateAdd(AddChain, ScaledVal);
  }

  TypeLower.replaceInstruction(&GEP, {AddChain});
}

// =====================================================================================================================
// Lower load instruction
//
// @param Load: the instruction
void CpsStackLowering::visitLoad(LoadInst &Load) {
  if (Load.getPointerAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(Load.getPointerOperand());

  IRBuilder<> Builder(&Load);
  Values[0] = getRealMemoryAddress(Builder, Values[0]);

  Load.replaceUsesOfWith(Load.getPointerOperand(), Values[0]);
}

// =====================================================================================================================
// Lower store instruction
//
// @param Store: the instruction
void CpsStackLowering::visitStore(llvm::StoreInst &Store) {
  if (Store.getPointerAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(Store.getPointerOperand());

  IRBuilder<> Builder(&Store);
  Values[0] = getRealMemoryAddress(Builder, Values[0]);

  Store.replaceUsesOfWith(Store.getPointerOperand(), Values[0]);
}

// =====================================================================================================================
// Lower ptrtoint instruction
//
// @param Ptr2Int: the instruction
void CpsStackLowering::visitPtrToIntInst(llvm::PtrToIntInst &Ptr2Int) {
  if (Ptr2Int.getPointerAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(Ptr2Int.getOperand(0));
  Ptr2Int.replaceAllUsesWith(Values[0]);
  TypeLower.eraseInstruction(&Ptr2Int);
}

// =====================================================================================================================
// Lower inttoptr instruction
//
// @param Int2Ptr: the instruction
void CpsStackLowering::visitIntToPtrInst(llvm::IntToPtrInst &Int2Ptr) {
  if (Int2Ptr.getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  TypeLower.replaceInstruction(&Int2Ptr, Int2Ptr.getOperand(0));
}

// =====================================================================================================================
// Lower bitcast instruction
//
// @param BC: the instruction
void CpsStackLowering::visitBitCastInst(llvm::BitCastInst &BC) {
  Type *SrcTy = BC.getOperand(0)->getType();
  if (!SrcTy->isPointerTy() ||
      cast<PointerType>(SrcTy)->getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  Type *DstTy = BC.getType();
  if (!DstTy->isPointerTy() ||
      cast<PointerType>(DstTy)->getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  auto Values = TypeLower.getValue(BC.getOperand(0));
  TypeLower.replaceInstruction(&BC, {Values[0]});
}

// =====================================================================================================================
// Lower lgc.cps.alloc instruction
//
// @param AllocOp: the instruction
void CpsStackLowering::visitCpsAlloc(lgc::cps::AllocOp &AllocOp) {
  IRBuilder<> Builder(&AllocOp);

  Value *VSP =
      Builder.CreateLoad(CpsStackAlloca->getAllocatedType(), CpsStackAlloca);

  Value *Size = AllocOp.getSize();
  int AlignedSize = cast<ConstantInt>(Size)->getSExtValue();
  assert(AlignedSize >= 0);
  AlignedSize = alignTo(AlignedSize, ContinuationStackAlignment);
  StackSizeInBytes += AlignedSize;

  // update stack pointer
  Value *NewVSP = Builder.CreateAdd(VSP, Builder.getInt32(AlignedSize));
  Builder.CreateStore(NewVSP, CpsStackAlloca);

  TypeLower.replaceInstruction(&AllocOp, {VSP});
}

// =====================================================================================================================
// Lower lgc.cps.free instruction
//
// @param FreeOp: the instruction
void CpsStackLowering::visitCpsFree(lgc::cps::FreeOp &FreeOp) {
  IRBuilder<> Builder(&FreeOp);

  Value *VSP =
      Builder.CreateLoad(CpsStackAlloca->getAllocatedType(), CpsStackAlloca);

  Value *Size = FreeOp.getSize();
  int AlignedSize = cast<ConstantInt>(Size)->getSExtValue();
  assert(AlignedSize >= 0);
  AlignedSize = alignTo(AlignedSize, ContinuationStackAlignment);
  Value *Ptr = Builder.CreateAdd(VSP, Builder.getInt32(-AlignedSize));

  // Assuming continuation stack grows upward.
  Builder.CreateStore(Ptr, CpsStackAlloca);
  TypeLower.replaceInstruction(&FreeOp, {});
}

// =====================================================================================================================
// Lower lgc.cps.peek instruction
//
// @param PeekOp: the instruction
void CpsStackLowering::visitCpsPeek(lgc::cps::PeekOp &PeekOp) {
  IRBuilder<> Builder(&PeekOp);

  auto *Ptr =
      Builder.CreateLoad(CpsStackAlloca->getAllocatedType(), CpsStackAlloca);
  auto *Size = PeekOp.getSize();
  int ImmSize = cast<ConstantInt>(Size)->getSExtValue();
  assert(ImmSize >= 0);
  ImmSize = alignTo(ImmSize, ContinuationStackAlignment);
  // Assuming continuation stack grows upward.
  auto *Result = Builder.CreateAdd(Ptr, Builder.getInt32(-ImmSize));

  TypeLower.replaceInstruction(&PeekOp, {Result});
}

// =====================================================================================================================
// Lower lgc.cps.set.VSP instruction
//
// @param function : the instruction
void CpsStackLowering::visitSetVsp(lgc::cps::SetVspOp &SetVsp) {
  auto *Ptr = SetVsp.getPtr();

  IRBuilder<> B(&SetVsp);

  auto Values = TypeLower.getValue(Ptr);
  B.CreateStore(Values[0], CpsStackAlloca);
  TypeLower.replaceInstruction(&SetVsp, {});
}

// =====================================================================================================================
// Lower lgc.cps.get.VSP instruction
//
// @param GetVsp: the instruction
void CpsStackLowering::visitGetVsp(lgc::cps::GetVspOp &GetVsp) {
  IRBuilder<> B(&GetVsp);

  auto *Ptr = B.CreateLoad(CpsStackAlloca->getAllocatedType(), CpsStackAlloca);
  TypeLower.replaceInstruction(&GetVsp, {Ptr});
}

// =====================================================================================================================
// Create a target address space-specific pointer based on an offset pointer
// (@Val) and a given base pointer, that is either the default null base pointer
// or a base pointer injected by calling @setRealBasePointer.
//
// @param Builder: the builder to use.
// @param Val: The offset to the base address, given as integer with bitwidth
// <= 32.
//
Value *CpsStackLowering::getRealMemoryAddress(IRBuilder<> &Builder,
                                              Value *Val) {
  // Since we are using at most 32-bit offsets, assert that we don't put in any
  // offset larger 32 bit.
  assert(Val->getType()->isIntegerTy() &&
         Val->getType()->getIntegerBitWidth() <= 32);
  return Builder.CreateGEP(Type::getInt8Ty(Builder.getContext()), BasePointer,
                           {Val});
}
