/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- DXILContPostProcess.cpp - Finalize IR ------------------===//
//
//  * Unpack 32-bit to 64-bit jump addresses
//  * Translate lgc.cps.jumps to lgc.ilcps.continue / waitContinue calls
//  * Cleanup unused metadata
//
//===----------------------------------------------------------------------===//

#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "dxil-cont-post-process"

namespace {
class DXILContPostProcessPassImpl final {
public:
  DXILContPostProcessPassImpl(Module &M);
  PreservedAnalyses run(ModuleAnalysisManager &AnalysisManager);

private:
  Value *ensure64BitAddr(Value *Packed32BitAddr);
  void lowerJumpOp(lgc::cps::JumpOp &JumpOp);
  void lowerAsContinuationReferenceOp(lgc::cps::AsContinuationReferenceOp &AsCrOp, Function *GetContinuationAddrAndMD);
  bool cleanupIncomingPayloadMetadata(Function &F);

  Module *Mod;
  llvm_dialects::Builder Builder;
};

static Function *getContinuationGetAddrAndMD(Module &M) {
  auto *Name = "continuation.getAddrAndMD";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *FuncTy = FunctionType::get(Type::getInt32Ty(M.getContext()), {PointerType::get(C, 0)}, false);

  return cast<Function>(M.getOrInsertFunction(Name, FuncTy).getCallee());
}

Value *DXILContPostProcessPassImpl::ensure64BitAddr(Value *Src) {
  Type *SrcTy = Src->getType();
  Type *I64 = Builder.getInt64Ty();
  if (SrcTy == I64)
    return Src;

  assert(SrcTy->isIntegerTy(32));

  Value *Addr64 = Builder.CreateZExt(Src, I64);
  Addr64 = Builder.CreateAnd(Addr64, 0xFFFFFFC0);

  Value *Priority = Builder.CreateAnd(Src, Builder.getInt32(0x7));
  // firstMetadataBit = 32
  // firstPriorityBitInMetadata = 16
  // vpc = vpc | (prio64 << (firstMetadataBit + firstPriorityBitInMetadata))
  Priority = Builder.CreateShl(Builder.CreateZExt(Priority, I64), 48);
  Addr64 = Builder.CreateOr(Addr64, Priority);

  return Addr64;
}

void DXILContPostProcessPassImpl::lowerJumpOp(lgc::cps::JumpOp &JumpOp) {
  Builder.SetInsertPoint(&JumpOp);

  CallInst *ContinueOp = nullptr;

  SmallVector<Value *> TailArgs{JumpOp.getTail()};

  Value *JumpTarget = JumpOp.getTarget();
  if (!ContHelper::tryGetDeferVpcUnpacking(*Mod))
    JumpTarget = ensure64BitAddr(JumpTarget);

  Value *ShaderIndex = JumpOp.getShaderIndex();
  Value *RetAddr = JumpOp.getRcr();
  if (ContHelper::isWaitAwaitCall(JumpOp)) {
    ContinueOp = Builder.create<lgc::ilcps::WaitContinueOp>(JumpTarget, Builder.getInt64(-1), JumpOp.getCsp(),
                                                            ShaderIndex, RetAddr, TailArgs);
    ContHelper::removeWaitMask(JumpOp);
  } else {
    ContinueOp = Builder.create<lgc::ilcps::ContinueOp>(JumpTarget, JumpOp.getCsp(), ShaderIndex, RetAddr, TailArgs);
  }

  ContinueOp->copyMetadata(JumpOp);
  JumpOp.eraseFromParent();

  ContHelper::OutgoingRegisterCount::reset(ContinueOp);
  ContHelper::ReturnedRegisterCount::reset(ContinueOp);
}

void DXILContPostProcessPassImpl::lowerAsContinuationReferenceOp(lgc::cps::AsContinuationReferenceOp &AsCrOp,
                                                                 Function *GetContinuationAddrAndMD) {
  Builder.SetInsertPoint(&AsCrOp);

  Value *AddrWithMD = Builder.CreateCall(GetContinuationAddrAndMD, {AsCrOp.getFn()});

  AsCrOp.replaceAllUsesWith(AddrWithMD);
  AsCrOp.eraseFromParent();
}

DXILContPostProcessPassImpl::DXILContPostProcessPassImpl(Module &M) : Mod{&M}, Builder{Mod->getContext()} {
}

PreservedAnalyses DXILContPostProcessPassImpl::run(ModuleAnalysisManager &AnalysisManager) {
  struct ProcessingState {
    DXILContPostProcessPassImpl &Self;
    bool Changed;
    Function *GetContinuationAddrAndMD;

    llvm::PreservedAnalyses getPreservedAnalyses() {
      if (Changed)
        return PreservedAnalyses::none();

      return PreservedAnalyses::all();
    }
  };

  ProcessingState State{*this, false, getContinuationGetAddrAndMD(*Mod)};

  static const auto CpsVisitor =
      llvm_dialects::VisitorBuilder<ProcessingState>()
          .add<lgc::cps::AsContinuationReferenceOp>(
              [](ProcessingState &State, lgc::cps::AsContinuationReferenceOp &AsCrOp) {
                State.Self.lowerAsContinuationReferenceOp(AsCrOp, State.GetContinuationAddrAndMD);
                State.Changed = true;
              })
          .add<lgc::cps::JumpOp>([](ProcessingState &State, lgc::cps::JumpOp &JumpOp) {
            State.Self.lowerJumpOp(JumpOp);
            State.Changed = true;
          })
          .build();

  for (Function &F : *Mod) {
    if (F.isDeclaration())
      continue;

    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    if (!Stage)
      continue;

    if (Stage == lgc::rt::RayTracingShaderStage::KernelEntry || F.hasMetadata(ContHelper::MDContinuationName) ||
        lgc::cps::isCpsFunction(F)) {
      // Lower lgc.cps.jump and lgc.cps.as.continuation.reference ops.
      CpsVisitor.visit(State, F);
    }

    if (Stage == lgc::rt::RayTracingShaderStage::Traversal)
      continue;

    ContHelper::IncomingRegisterCount::reset(&F);
    ContHelper::ContinuationStateByteCount::reset(&F);
  }

  State.Changed |= fixupDxilMetadata(*Mod);

  State.Changed |= llvm::removeUnusedFunctionDecls(Mod, false);

  return State.getPreservedAnalyses();
}
} // anonymous namespace

llvm::PreservedAnalyses DXILContPostProcessPass::run(llvm::Module &Module,
                                                     llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass dxil-cont-post-process\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Module);

  DXILContPostProcessPassImpl Impl{Module};
  return Impl.run(AnalysisManager);
}
