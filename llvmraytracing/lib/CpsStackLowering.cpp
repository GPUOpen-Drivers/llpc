/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

using namespace compilerutils;
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
// @return: The new function, if Function was mutated, or the Function argument.
Function *CpsStackLowering::lowerCpsStackOps(Function *Func, Function *GetGlobalMemBase, bool RequiresIncomingCsp) {
  Mod = Func->getParent();
  StackSizeInBytes = 0;

  Func = addOrInitCsp(Func, GetGlobalMemBase, RequiresIncomingCsp);

  TypeLower.addRule(
      std::bind(&CpsStackLowering::convertStackPtrToI32, this, std::placeholders::_1, std::placeholders::_2));
  if (lgc::cps::isCpsFunction(*Func))
    Func = TypeLower.lowerFunctionArguments(*Func);
  SQ.emplace(Func->getDataLayout());

  static const auto Visitor = llvm_dialects::VisitorBuilder<CpsStackLowering>()
                                  .nest(&TypeLowering::registerVisitors)
                                  .add(&CpsStackLowering::visitCpsAlloc)
                                  .add(&CpsStackLowering::visitCpsFree)
                                  .add(&CpsStackLowering::visitCpsPeek)
                                  .add(&CpsStackLowering::visitSetVsp)
                                  .add(&CpsStackLowering::visitGetVsp)
                                  .add(&CpsStackLowering::visitJump)
                                  .add(&CpsStackLowering::visitGetElementPtr)
                                  .add(&CpsStackLowering::visitPtrToIntInst)
                                  .add(&CpsStackLowering::visitIntToPtrInst)
                                  .add(&CpsStackLowering::visitBitCastInst)
                                  .add(&CpsStackLowering::visitLoad)
                                  .add(&CpsStackLowering::visitStore)
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

  Builder.SetInsertPoint(&GEP);

  auto Values = TypeLower.getValue(GEP.getPointerOperand());
  Value *AddChain = Values[0];

  const DataLayout &DL = GEP.getFunction()->getParent()->getDataLayout();
  unsigned BitWidth = DL.getIndexSizeInBits(GEP.getPointerAddressSpace());

  APInt ConstantOffset{BitWidth, 0};
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 513542
  MapVector<Value *, APInt> VariableOffsets;
#else
  SmallMapVector<Value *, APInt, 4> VariableOffsets;
#endif

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

  Builder.SetInsertPoint(&Load);
  Values[0] = getRealMemoryAddress(Values[0]);

  Values[0] = Builder.CreateBitCast(Values[0], Builder.getPtrTy(getLoweredCpsStackAddrSpace()));

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

  Builder.SetInsertPoint(&Store);
  Values[0] = getRealMemoryAddress(Values[0]);

  Values[0] = Builder.CreateBitCast(Values[0], Builder.getPtrTy(getLoweredCpsStackAddrSpace()));

  Store.replaceUsesOfWith(Store.getPointerOperand(), Values[0]);
}

// =====================================================================================================================
// Lower lgc.cps.jump instruction
//
// @param JumpOp: the instruction
void CpsStackLowering::visitJump(lgc::cps::JumpOp &JumpOp) {
  Builder.SetInsertPoint(&JumpOp);
  Value *CSP = loadCsp();

  // Update previously lowered arguments
  SmallVector<Value *> TailArgs{JumpOp.getTail()};
  for (auto &Arg : TailArgs) {
    SmallVector<Value *> Mappings = TypeLower.getValueOptional(Arg);
    if (!Mappings.empty()) {
      assert(Mappings.size() == 1);
      Arg = Mappings[0];
    }
  }

  auto *NewJumpOp = JumpOp.replaceTail(TailArgs);
  NewJumpOp->setCsp(CSP);
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
  Builder.SetInsertPoint(&AllocOp);
  Value *Size = AllocOp.getSize();

  if (Instruction *Inst = dyn_cast<Instruction>(Size))
    if (auto *NewSize = llvm::simplifyInstruction(Inst, *SQ))
      Size = NewSize;

  Value *CSP = loadCsp();

  // align Size to ContinuationStackAlignment
  ConstantInt *Const = cast<ConstantInt>(Size);
  int AlignedSize = Const->getSExtValue();
  assert(AlignedSize >= 0);
  if (AlignedSize > 0) {
    AlignedSize = alignTo(AlignedSize, ContinuationStackAlignment);
    StackSizeInBytes += AlignedSize;
    Size = Builder.getInt32(AlignedSize);
  }

  Value *NewCSP = Builder.CreateAdd(CSP, Size);

  Builder.CreateStore(NewCSP, CpsStackAlloca);

  TypeLower.replaceInstruction(&AllocOp, {CSP});
}

// =====================================================================================================================
// Lower lgc.cps.free instruction
//
// @param FreeOp: the instruction
void CpsStackLowering::visitCpsFree(lgc::cps::FreeOp &FreeOp) {
  Builder.SetInsertPoint(&FreeOp);
  Value *Size = FreeOp.getSize();

  if (Instruction *Inst = dyn_cast<Instruction>(Size))
    if (auto *NewSize = llvm::simplifyInstruction(Inst, *SQ))
      Size = NewSize;

  Value *CSP = loadCsp();

  // align Size to ContinuationStackAlignment and subtract from CSP
  ConstantInt *Const = cast<ConstantInt>(Size);
  int AlignedSize = Const->getSExtValue();
  assert(AlignedSize >= 0);
  if (AlignedSize > 0) {
    AlignedSize = alignTo(AlignedSize, ContinuationStackAlignment);
    Size = Builder.getInt32(-AlignedSize);
    CSP = Builder.CreateAdd(CSP, Size);
  }

  // Assuming continuation stack grows upward.
  Builder.CreateStore(CSP, CpsStackAlloca);
  TypeLower.replaceInstruction(&FreeOp, {});
}

// =====================================================================================================================
// Lower lgc.cps.peek instruction
//
// @param PeekOp: the instruction
void CpsStackLowering::visitCpsPeek(lgc::cps::PeekOp &PeekOp) {
  Builder.SetInsertPoint(&PeekOp);

  auto *Ptr = loadCsp();
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

  Builder.SetInsertPoint(&SetVsp);

  auto Values = TypeLower.getValue(Ptr);
  Builder.CreateStore(Values[0], CpsStackAlloca);
  TypeLower.replaceInstruction(&SetVsp, {});
}

// =====================================================================================================================
// Lower lgc.cps.get.VSP instruction
//
// @param GetVsp: the instruction
void CpsStackLowering::visitGetVsp(lgc::cps::GetVspOp &GetVsp) {
  Builder.SetInsertPoint(&GetVsp);
  TypeLower.replaceInstruction(&GetVsp, {loadCsp()});
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
Value *CpsStackLowering::getRealMemoryAddress(Value *Offset) {
  // Since we are using at most 32-bit offsets, assert that we don't put in any
  // offset larger 32 bit.
  assert(Offset->getType()->isIntegerTy() && Offset->getType()->getIntegerBitWidth() <= 32);

  // Create a byte-addressed GEP the global memory address + offset or just the
  // offset. Note: Don't currently return a inttoptr because the translator
  // doesn't cope well with addrspace(21) inttoptr instructions.
  Value *GepBase = BasePointer;
  Value *GepIndex = Offset;

  if (isa<ConstantPointerNull>(BasePointer)) {
    GepBase = Builder.CreateIntToPtr(Offset, Builder.getPtrTy(getLoweredCpsStackAddrSpace()));
    GepIndex = Builder.getInt32(0);
  }

  return Builder.CreateGEP(Builder.getInt8Ty(), GepBase, {GepIndex});
}

// =====================================================================================================================
// Add stack pointer argument to the function or initialize the stack pointer
// from the initializer.
//
// @param GetGlobalMemBase: Get the base address for the stack.
//                          `nullptr` if there is no base address and the csp
//                          can be converted with ptrtoint.
Function *CpsStackLowering::addOrInitCsp(Function *F, Function *GetGlobalMemBase, bool RequiresIncomingCsp) {
  compilerutils::CrossModuleInliner CrossInliner;
  auto &GpurtContext = lgc::GpurtContext::get(Mod->getContext());
  auto &GpurtLibrary = GpurtContext.theModule ? *GpurtContext.theModule : *Mod;
  Value *Initializer = nullptr;

  Builder.SetInsertPointPastAllocas(F);
  CpsStackAlloca = Builder.CreateAlloca(Builder.getInt32Ty());
  CpsStackAlloca->setName("csp");

  if (RequiresIncomingCsp) {
    auto *FTy = F->getFunctionType();
    SmallVector<Type *> NewArgTys{FTy->params()};

    NewArgTys.insert(NewArgTys.begin(), Builder.getInt32Ty());

    Function *NewFunc = compilerutils::mutateFunctionArguments(*F, F->getReturnType(), NewArgTys, F->getAttributes());

    Argument *CspArg = NewFunc->getArg(0);
    CspArg->setName("cspInit");
    Initializer = CspArg;

    for (unsigned Idx = 0; Idx < F->arg_size(); ++Idx) {
      // Skip the CSP argument during remapping.
      Value *OldArg = F->getArg(Idx);
      Value *NewArg = NewFunc->getArg(Idx + 1);
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
    auto *CspTy = Builder.getPtrTy(getLoweredCpsStackAddrSpace());
    setRealBasePointer(Builder.CreateIntToPtr(Base, CspTy));
  }

  return F;
}

Value *CpsStackLowering::loadCsp() {
  return Builder.CreateLoad(CpsStackAlloca->getAllocatedType(), CpsStackAlloca);
}
