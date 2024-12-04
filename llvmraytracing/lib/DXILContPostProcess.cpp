/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- DXILContPostProcess.cpp - Replace intrinsic calls ------------------===//
//
//  * Insert the initialization of the continuation stack pointer.
//  * Replace dx.op intrinsic calls with calls to the driver implementation
//    and initialize the system data.
//  * Wraps all uses of function pointers into an intrinsic that adds
//    metadata (e.g. VGPR counts) to the function pointer.
//
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "llpc/GpurtEnums.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/CpsStackLowering.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "dxil-cont-post-process"

namespace {
class DXILContPostProcessPassImpl final {
public:
  DXILContPostProcessPassImpl(Module &M, Module &GpurtLibrary);
  PreservedAnalyses run(ModuleAnalysisManager &AnalysisManager);

  static constexpr unsigned SystemDataArgumentIndex = 2;
  struct FunctionData {
    DXILShaderKind Kind = DXILShaderKind::Invalid;
    /// Calls to hlsl intrinsics
    SmallVector<CallInst *> IntrinsicCalls;

    /// If this is the start function part of a split function
    bool IsStart = true;
    Type *SystemDataTy = nullptr;
    unsigned SystemDataArgumentIndex = std::numeric_limits<unsigned>::max();
  };

private:
  void initializeProcessableFunctionData();
  bool lowerCpsOps();
  Value *ensure64BitAddr(Value *Packed32BitAddr);
  void lowerJumpOp(lgc::cps::JumpOp &JumpOp);
  void lowerAsContinuationReferenceOp(lgc::cps::AsContinuationReferenceOp &AsCrOp);
  bool cleanupIncomingPayloadMetadata(Function &F);
  bool cleanupOutgoingPayloadMetadata();

  Module *Mod;
  Module *GpurtLibrary;
  MapVector<Function *, FunctionData> ToProcess;
  llvm_dialects::Builder Builder;
  std::optional<ContStackAddrspace> StackAddrspace;
  CompilerUtils::CrossModuleInliner CrossInliner;
};

// Removes outgoing payload metadata
bool DXILContPostProcessPassImpl::cleanupOutgoingPayloadMetadata() {
  struct State {
    bool Changed = false;
  };

  static const auto Visitor =
      llvm_dialects::VisitorBuilder<State>()
          .addSet<lgc::ilcps::ContinueOp, lgc::ilcps::WaitContinueOp>([](State &State, Instruction &Op) {
            ContHelper::OutgoingRegisterCount::reset(&Op);
            ContHelper::ReturnedRegisterCount::reset(&Op);
            State.Changed = true;
          })
          .build();

  State S;
  Visitor.visit(S, *Mod);

  return S.Changed;
}

static Function *getContinuationGetAddrAndMD(Module &M, Type *RetTy) {
  auto *Name = "continuation.getAddrAndMD";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *FuncTy = FunctionType::get(RetTy, {PointerType::get(C, 0)}, false);

  return cast<Function>(M.getOrInsertFunction(Name, FuncTy).getCallee());
}

/// Checks some properties guaranteed for a module containing continuations
/// as expected by the backend.
[[maybe_unused]] static void checkContinuationsModule(const Module &M) {
  // Check that resume functions do not have a stack size set.
  for (auto &Func : M) {
    if (auto *MD = dyn_cast_or_null<MDTuple>(Func.getMetadata(ContHelper::MDContinuationName))) {
      auto *StartFunc = extractFunctionOrNull(MD->getOperand(0));
      bool IsStart = (&Func == StartFunc);
      bool HasStackSizeMetadata = ContHelper::StackSize::tryGetValue(&Func).has_value();
      if (!IsStart && HasStackSizeMetadata)
        report_fatal_error("Found resume function with stack size metadata!");
    }
  }
}

void DXILContPostProcessPassImpl::initializeProcessableFunctionData() {
  for (Function &F : *Mod) {
    if (F.isDeclaration())
      continue;

    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    if (!Stage)
      continue;

    // For the kernel entry function in GPURT, we only care about its existence
    // in @ToProcess, since we only want to create an alloca for the
    // continuation stack pointer later (and do the lgc.cps lowering).
    if (Stage == lgc::rt::RayTracingShaderStage::KernelEntry) {
      FunctionData Data;
      Data.Kind = DXILShaderKind::Compute;
      [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);
      continue;
    }

    // Handle start functions first
    if (!llvm::isStartFunc(&F))
      continue;

    DXILShaderKind Kind = ShaderStageHelper::rtShaderStageToDxilShaderKind(*Stage);
    const bool IsCpsFunction = lgc::cps::isCpsFunction(F);

    switch (Kind) {
    case DXILShaderKind::RayGeneration: {
      FunctionData Data;
      Data.Kind = Kind;

      Data.SystemDataArgumentIndex = !IsCpsFunction ? SystemDataArgumentIndex : CpsArgIdxWithStackPtr::SystemData;

      Data.SystemDataTy = F.getFunctionType()->getParamType(Data.SystemDataArgumentIndex);

      [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);
      break;
    }
    case DXILShaderKind::Intersection:
    case DXILShaderKind::AnyHit:
    case DXILShaderKind::ClosestHit:
    case DXILShaderKind::Miss:
    case DXILShaderKind::Callable: {
      FunctionData Data;
      Data.Kind = Kind;

      Data.SystemDataArgumentIndex = !IsCpsFunction ? SystemDataArgumentIndex : CpsArgIdxWithStackPtr::SystemData;
      Data.SystemDataTy = F.getFunctionType()->getParamType(Data.SystemDataArgumentIndex);
      [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);
      break;
    }
    default:
      break;
    }
  }

  // Also find continuation parts of the functions
  for (auto &F : *Mod) {
    if (F.isDeclaration())
      continue;
    if (auto *MD = dyn_cast_or_null<MDTuple>(F.getMetadata(ContHelper::MDContinuationName))) {
      auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
      auto Stage = lgc::rt::getLgcRtShaderStage(EntryF);
      if (Stage && &F != EntryF) {
        FunctionData Data = ToProcess[EntryF];
        Data.IsStart = false;

        Data.SystemDataArgumentIndex =
            !lgc::cps::isCpsFunction(F) ? SystemDataArgumentIndex : CpsArgIdxWithStackPtr::SystemData;

        Data.SystemDataTy = F.getArg(Data.SystemDataArgumentIndex)->getType();
        [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
        assert(DidInsert);
      }
    }
  }
}

//
// Entry point for all lgc.cps lowering.
//
bool DXILContPostProcessPassImpl::lowerCpsOps() {
  bool Changed = false;

  struct CpsVisitorState {
    DXILContPostProcessPassImpl &Self;
    bool &Changed;
    llvm_dialects::Builder &Builder;
  };

  // Note: It is a bit unlucky that we are using both a visitor for
  // lgc.cps.as.continuation.reference and lgc.cps.jump and a loop for the
  // actual stack lowering. It would be nice to use a visitor for both of them,
  // but currently, there seems to be no support in dialects for marrying both
  // approaches: we would need a visitor that supports visiting function
  // definitions as well.
  static const auto CpsVisitor = llvm_dialects::VisitorBuilder<CpsVisitorState>()
                                     .add<lgc::cps::AsContinuationReferenceOp>(
                                         [](CpsVisitorState &State, lgc::cps::AsContinuationReferenceOp &AsCrOp) {
                                           State.Self.lowerAsContinuationReferenceOp(AsCrOp);
                                           State.Changed = true;
                                         })
                                     .add<lgc::cps::JumpOp>([](CpsVisitorState &State, lgc::cps::JumpOp &JumpOp) {
                                       State.Self.lowerJumpOp(JumpOp);
                                       State.Changed = true;
                                     })
                                     .build();

  CpsVisitorState State{*this, Changed, Builder};

  for (Function &Func : *Mod) {
    if (Func.isDeclaration())
      continue;

    if (lgc::rt::getLgcRtShaderStage(&Func) == lgc::rt::RayTracingShaderStage::KernelEntry ||
        Func.hasMetadata(ContHelper::MDContinuationName) || lgc::cps::isCpsFunction(Func)) {
      // Lower lgc.cps.jump and lgc.cps.as.continuation.reference ops.
      CpsVisitor.visit(State, Func);
    }
  }

  return Changed;
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

  Value *JumpTarget = ensure64BitAddr(JumpOp.getTarget());
  Value *RetAddr = JumpOp.getRcr();
  if (ContHelper::isWaitAwaitCall(JumpOp)) {
    ContinueOp = Builder.create<lgc::ilcps::WaitContinueOp>(JumpTarget, Builder.getInt64(-1), JumpOp.getCsp(), RetAddr,
                                                            TailArgs);
    ContHelper::removeWaitMask(JumpOp);
  } else {
    ContinueOp = Builder.create<lgc::ilcps::ContinueOp>(JumpTarget, JumpOp.getCsp(), RetAddr, TailArgs);
  }

  ContinueOp->copyMetadata(JumpOp);
  JumpOp.eraseFromParent();
}

void DXILContPostProcessPassImpl::lowerAsContinuationReferenceOp(lgc::cps::AsContinuationReferenceOp &AsCrOp) {
  Builder.SetInsertPoint(&AsCrOp);

  Value *AddrWithMD = Builder.CreateCall(getContinuationGetAddrAndMD(*Mod, AsCrOp.getType()), {AsCrOp.getFn()});

  AsCrOp.replaceAllUsesWith(AddrWithMD);
  AsCrOp.eraseFromParent();
}

DXILContPostProcessPassImpl::DXILContPostProcessPassImpl(Module &M, Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Builder{Mod->getContext()}, StackAddrspace{
                                                                            ContHelper::tryGetStackAddrspace(*Mod)} {
}

PreservedAnalyses DXILContPostProcessPassImpl::run(ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  initializeProcessableFunctionData();

  for (auto &[Func, Data] : ToProcess) {
    ContHelper::IncomingRegisterCount::reset(Func);
    ContHelper::ContinuationStateByteCount::reset(Func);
  }

  Changed |= lowerCpsOps();

  Changed |= fixupDxilMetadata(*Mod);
  Changed |= cleanupOutgoingPayloadMetadata();

#ifndef NDEBUG
  checkContinuationsModule(*Mod);
#endif

  Changed |= llvm::removeUnusedFunctionDecls(Mod, false);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // anonymous namespace

llvm::PreservedAnalyses DXILContPostProcessPass::run(llvm::Module &Module,
                                                     llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass dxil-cont-post-process\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Module);

  auto &GpurtContext = lgc::GpurtContext::get(Module.getContext());
  DXILContPostProcessPassImpl Impl{Module, GpurtContext.theModule ? *GpurtContext.theModule : Module};
  return Impl.run(AnalysisManager);
}
