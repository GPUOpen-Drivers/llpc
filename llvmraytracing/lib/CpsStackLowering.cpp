/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llvmraytracing/CpsStackLowering.h"
#include "compilerutils/CompilerUtils.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

using namespace CompilerUtils;
using namespace llvm;
using namespace lgc::cps;

LLVM_DIALECTS_VISITOR_PAYLOAD_PROJECT_FIELD(CpsStackLowering, TypeLower)

SmallVector<Type *> CpsStackLowering::convertStackPtrToI32(TypeLowering &TypeLower, Type *Ty) {
  SmallVector<Type *> Types;

  if (auto *PtrTy = dyn_cast<PointerType>(Ty)) {
    if (PtrTy->getAddressSpace() == lgc::cps::stackAddrSpace)
      Types.push_back(Type::getInt32Ty(TypeLower.getContext()));
  }

  return Types;
}

// =====================================================================================================================
// @param Func : the function to be processed
// @param GetGlobalMemBase: Get the base address for the stack.
//                          `nullptr` if there is no base address and the csp
//                          can be converted with ptrtoint.
// @param RequiresIncomingCsp: Whether the CSP argument should be appended to
//                             Func's signature.
// @param CpsStorage : the alloca used for the holding the latest continuation
//                     stack pointer. TODO Remove this argument. This function
//                     should be responsible for adding the alloca.
// @return: The new function, if Function was mutated, or the Function argument.
Function *CpsStackLowering::lowerCpsStackOps(Function *Func, Function *GetGlobalMemBase, bool RequiresIncomingCsp,
                                             llvm::Value *CspStorage) {
  Mod = Func->getParent();
  StackSizeInBytes = 0;

  if (CspStorage)
    CpsStackAlloca = cast<AllocaInst>(CspStorage);
  else
    Func = addOrInitCsp(Func, GetGlobalMemBase, RequiresIncomingCsp);

  TypeLower.addRule(
      std::bind(&CpsStackLowering::convertStackPtrToI32, this, std::placeholders::_1, std::placeholders::_2));
  if (lgc::cps::isCpsFunction(*Func))
    Func = TypeLower.lowerFunctionArguments(*Func);

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
                                  .add(&CpsStackLowering::visitContinue)
                                  .add(&CpsStackLowering::visitWaitContinue)
                                  .build();
  Visitor.visit(*this, *Func);
  TypeLower.finishPhis();
  TypeLower.finishCleanup();

  CpsStackAlloca = nullptr;

  return Func;
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

  [[maybe_unused]] bool Success = GEP.collectOffset(DL, BitWidth, VariableOffsets, ConstantOffset);
  assert(Success && "CpsStackLowering::visitGetElementPtr: GEP.collectOffset "
                    "did not succeed!");

  if (ConstantOffset.getSExtValue() != 0)
    AddChain = Builder.CreateAdd(AddChain, Builder.getInt32(ConstantOffset.getSExtValue()));

  for (const auto &[Index, Scaling] : VariableOffsets) {
    Value *ScaledVal = Index;

    if (Scaling.getSExtValue() != 1)
      ScaledVal = Builder.CreateMul(ScaledVal, Builder.getInt32(Scaling.getSExtValue()));

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

  Values[0] = Builder.CreateBitCast(Values[0], Load.getType()->getPointerTo(getLoweredCpsStackAddrSpace()));

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

  Values[0] =
      Builder.CreateBitCast(Values[0], Store.getValueOperand()->getType()->getPointerTo(getLoweredCpsStackAddrSpace()));

  Store.replaceUsesOfWith(Store.getPointerOperand(), Values[0]);
}

// =====================================================================================================================
// Add stack pointer to a lgc.ilcps.continue call
//
// @param Continue: the instruction
void CpsStackLowering::visitContinue(lgc::ilcps::ContinueOp &Continue) {
  IRBuilder<> Builder(&Continue);
  Continue.setCsp(loadCsp(Builder));
}

// =====================================================================================================================
// Add stack pointer to a lgc.ilcps.waitContinue call
//
// @param WaitContinue: the instruction
void CpsStackLowering::visitWaitContinue(lgc::ilcps::WaitContinueOp &WaitContinue) {
  IRBuilder<> Builder(&WaitContinue);
  WaitContinue.setCsp(loadCsp(Builder));
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
  if (!SrcTy->isPointerTy() || cast<PointerType>(SrcTy)->getAddressSpace() != lgc::cps::stackAddrSpace)
    return;

  Type *DstTy = BC.getType();
  if (!DstTy->isPointerTy() || cast<PointerType>(DstTy)->getAddressSpace() != lgc::cps::stackAddrSpace)
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

  Value *VSP = loadCsp(Builder);

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

  Value *VSP = loadCsp(Builder);

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

  auto *Ptr = loadCsp(Builder);
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
  TypeLower.replaceInstruction(&GetVsp, {loadCsp(B)});
}

// =====================================================================================================================
// Create a target address space-specific pointer based on an offset pointer
// (@Val) and a given base pointer, that is either the default null base pointer
// or a base pointer injected by calling @setRealBasePointer.
//
// @param Builder: the builder to use.
// @param Offset: The offset to the base address, given as integer with bitwidth
// <= 32.
//
Value *CpsStackLowering::getRealMemoryAddress(IRBuilder<> &Builder, Value *Offset) {
  // Since we are using at most 32-bit offsets, assert that we don't put in any
  // offset larger 32 bit.
  assert(Offset->getType()->isIntegerTy() && Offset->getType()->getIntegerBitWidth() <= 32);

  // Create a byte-addressed GEP the global memory address + offset or just the
  // offset. Note: Don't currently return a inttoptr because the translator
  // doesn't cope well with addrspace(21) inttoptr instructions.
  Value *GepBase = BasePointer;
  Value *GepIndex = Offset;

  Type *I8 = Builder.getInt8Ty();
  if (isa<ConstantPointerNull>(BasePointer)) {
    GepBase = Builder.CreateIntToPtr(Offset, I8->getPointerTo(getLoweredCpsStackAddrSpace()));
    GepIndex = Builder.getInt32(0);
  }

  return Builder.CreateGEP(I8, GepBase, {GepIndex});
}

// =====================================================================================================================
// Add stack pointer argument to the function or initialize the stack pointer
// from the initializer.
//
// @param GetGlobalMemBase: Get the base address for the stack.
//                          `nullptr` if there is no base address and the csp
//                          can be converted with ptrtoint.
Function *CpsStackLowering::addOrInitCsp(Function *F, Function *GetGlobalMemBase, bool RequiresIncomingCsp) {
  CompilerUtils::CrossModuleInliner CrossInliner;
  auto &GpurtContext = lgc::GpurtContext::get(Mod->getContext());
  auto &GpurtLibrary = GpurtContext.theModule ? *GpurtContext.theModule : *Mod;
  IRBuilder<> Builder(F->getContext());
  Value *Initializer = nullptr;

  Builder.SetInsertPointPastAllocas(F);
  CpsStackAlloca = Builder.CreateAlloca(Builder.getInt32Ty());
  CpsStackAlloca->setName("csp");

  if (RequiresIncomingCsp) {
    auto *FTy = F->getFunctionType();
    SmallVector<Type *> NewArgTys{FTy->params()};

    const size_t CspArgIndex = lgc::cps::isCpsFunction(*F) ? 1 : 0;
    NewArgTys.insert(NewArgTys.begin() + CspArgIndex, Builder.getInt32Ty());

    Function *NewFunc = CompilerUtils::mutateFunctionArguments(*F, F->getReturnType(), NewArgTys, F->getAttributes());

    Argument *CspArg = NewFunc->getArg(CspArgIndex);
    CspArg->setName("cspInit");
    Initializer = CspArg;

    for (unsigned Idx = 0; Idx < F->arg_size(); ++Idx) {
      // Skip the CSP argument during remapping.
      Value *OldArg = F->getArg(Idx);
      Value *NewArg = NewFunc->getArg(Idx >= CspArgIndex ? Idx + 1 : Idx);
      NewArg->takeName(OldArg);
      OldArg->replaceAllUsesWith(NewArg);
    }

    F->replaceAllUsesWith(NewFunc);
    F->eraseFromParent();

    F = NewFunc;
  } else if (lgc::rt::getLgcRtShaderStage(F) != lgc::rt::RayTracingShaderStage::KernelEntry) {
    // Init csp through intrinsic
    auto *InitFun = GpurtLibrary.getFunction(ContDriverFunc::GetContinuationStackAddrName);
    assert(InitFun && "_cont_GetContinuationStackAddr not found.");
    assert(InitFun->arg_size() == 0 && InitFun->getReturnType()->isIntegerTy(32));

    Initializer = CrossInliner.inlineCall(Builder, InitFun).returnValue;
  }

  if (Initializer)
    Builder.CreateStore(Initializer, CpsStackAlloca);

  // Get the global memory base address.
  if (GetGlobalMemBase) {
    auto *Base = CrossInliner.inlineCall(Builder, GetGlobalMemBase).returnValue;
    auto *CspTy = Builder.getInt8Ty()->getPointerTo(getLoweredCpsStackAddrSpace());
    setRealBasePointer(Builder.CreateIntToPtr(Base, CspTy));
  }

  return F;
}

Value *CpsStackLowering::loadCsp(IRBuilder<> &Builder) {
  return Builder.CreateLoad(CpsStackAlloca->getAllocatedType(), CpsStackAlloca);
}
