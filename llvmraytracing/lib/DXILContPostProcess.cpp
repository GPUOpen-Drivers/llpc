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
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InstructionSimplify.h"
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

  static constexpr unsigned SystemDataArgumentIndex = 1;
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
  void handleContStackIntrinsic(FunctionAnalysisManager &FAM, Function &F);

  void initializeProcessableFunctionData();
  bool handleContStackIntrinsics(llvm::ModuleAnalysisManager &AnalysisManager);
  bool lowerCpsOps();
  void lowerJumpOp(lgc::cps::JumpOp &JumpOp);
  void lowerAsContinuationReferenceOp(lgc::cps::AsContinuationReferenceOp &AsCrOp);
  bool handleAmdInternals();
  bool cleanupIncomingPayloadMetadata(Function &F);
  bool cleanupOutgoingPayloadMetadata();

  Module *Mod;
  Module *GpurtLibrary;
  MapVector<Function *, FunctionData> ToProcess;
  llvm_dialects::Builder Builder;
  std::optional<ContStackAddrspace> StackAddrspace;
  std::optional<CpsStackLowering> StackLowering;
  CompilerUtils::CrossModuleInliner CrossInliner;

  Function *GetGlobalMemBase = nullptr;
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

static Function *getContinuationGetAddrAndMD(Module &M) {
  auto *Name = "continuation.getAddrAndMD";
  if (auto *F = M.getFunction(Name))
    return F;
  auto &C = M.getContext();
  auto *I64 = Type::getInt64Ty(C);
  auto *FuncTy = FunctionType::get(I64, {PointerType::get(C, 0)}, false);

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

// Replace calls to _AmdContStack* with calls to lgc.cps dialect ops.
// Do some simple constant propagation on the fly.
void DXILContPostProcessPassImpl::handleContStackIntrinsic(FunctionAnalysisManager &FAM, Function &F) {

  // Check if the function is either of void return type or i32 return type and
  // has no arguments or a single integer argument dividable by 32 (to allow
  // storing and loading multiple dwords via AmdContStackLoad /
  // AmdContStackStore).
  Type *ReturnTy = F.getReturnType();
  (void)ReturnTy;
  assert((ReturnTy->isVoidTy() || (ReturnTy->isIntegerTy() && (ReturnTy->getIntegerBitWidth() % 32 == 0))) &&
         "DXILContPostProcessPassImpl::handleContStackIntrinsic: Invalid "
         "return type!");

  Type *FuncTy = F.getFunctionType();
  (void)(FuncTy);
  assert((FuncTy->getFunctionNumParams() == 0 || FuncTy->getFunctionParamType(0)->isIntegerTy()) &&
         "DXILContPostProcessPassImpl::handleContStackIntrinsic: Invalid "
         "argument signature!");

  StringRef FuncName = F.getName();
  FuncName.consume_front("_AmdContStack");

  auto ConstantFoldInstruction = [&](Function *Parent, Value *SizeArg) -> Value * {
    if (!isa<Instruction>(SizeArg))
      return SizeArg;

    if (auto *I = dyn_cast<Instruction>(SizeArg)) {
      // Do some basic constant-propagation
      // This is needed because this pass just replaced the ValueI32Count
      // and ContPayloadRegistersI32Count intrinsics and the allocated size
      // usually depends on these values.
      auto &DT = FAM.getResult<DominatorTreeAnalysis>(*Parent);
      auto &TLI = FAM.getResult<TargetLibraryAnalysis>(*Parent);
      auto &AC = FAM.getResult<AssumptionAnalysis>(*Parent);
      const SimplifyQuery SQ(Parent->getParent()->getDataLayout(), &TLI, &DT, &AC);

      if (auto *NewSize = simplifyInstruction(I, SQ))
        return NewSize;
    }

    return SizeArg;
  };

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Value *Replacement = nullptr;
    Builder.SetInsertPoint(&CInst);

    Type *DestTy = CInst.getType();

    bool IsMemoryAccess = false;
    if (FuncName.starts_with("Alloc")) {
      Value *SizeArg = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Replacement = Builder.create<lgc::cps::AllocOp>(SizeArg);

      if (auto *Size = dyn_cast<ConstantInt>(SizeArg))
        ContHelper::StackSize::inc(CInst.getFunction(), Size->getSExtValue());
    } else if (FuncName.starts_with("Free")) {
      Value *SizeArg = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Replacement = Builder.create<lgc::cps::FreeOp>(SizeArg);
    } else if (FuncName.starts_with("SetPtr")) {
      Value *Vsp = CInst.getArgOperand(0);
      Replacement = Builder.create<lgc::cps::SetVspOp>(
          Builder.CreateIntToPtr(Vsp, PointerType::get(Builder.getInt8Ty(), lgc::cps::stackAddrSpace)));
    } else if (FuncName.starts_with("GetPtr")) {
      Replacement = Builder.create<lgc::cps::GetVspOp>();
    } else if (FuncName.starts_with("Load")) {
      Value *Addr = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Value *Ptr = Builder.CreateIntToPtr(Addr, CInst.getType()->getPointerTo(lgc::cps::stackAddrSpace));
      Replacement = Builder.CreateAlignedLoad(DestTy, Ptr, Align(CpsStackLowering::getContinuationStackAlignment()));

      if (FuncName.starts_with("LoadLastUse"))
        CompilerUtils::setIsLastUseLoad(*cast<LoadInst>(Replacement));

      IsMemoryAccess = true;
    } else if (FuncName.starts_with("Store")) {
      assert(FuncTy->getFunctionNumParams() == 2 && "DXILContPostProcessPassImpl::handleContStackIntrinsic: Invalid "
                                                    "argument signature for AmdContStackStore!");

      Value *Addr = ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Value *Val = CInst.getArgOperand(1);
      Value *Ptr = Builder.CreateIntToPtr(Addr, Val->getType()->getPointerTo(lgc::cps::stackAddrSpace));
      Builder.CreateAlignedStore(Val, Ptr, Align(CpsStackLowering::getContinuationStackAlignment()));

      IsMemoryAccess = true;
    } else {
      llvm_unreachable("DXILContPostProcessPassImpl::handleContStackIntrinsic: "
                       "Unknown intrinsic!");
    }

    if (Replacement) {
      if (!DestTy->isVoidTy() && !IsMemoryAccess)
        Replacement = Builder.CreatePtrToInt(Replacement, DestTy);

      CInst.replaceAllUsesWith(Replacement);
    }

    CInst.eraseFromParent();
  });
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

      Data.SystemDataArgumentIndex = !IsCpsFunction ? SystemDataArgumentIndex : CpsArgIdxSystemData;

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

      Data.SystemDataArgumentIndex = !IsCpsFunction ? SystemDataArgumentIndex : CpsArgIdxSystemData;
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

        Data.SystemDataArgumentIndex = !lgc::cps::isCpsFunction(F) ? SystemDataArgumentIndex : CpsArgIdxSystemData;

        // Extract the actual system data type from the { systemData, padding,
        // payload } struct returned by await.
        Data.SystemDataTy = F.getArg(Data.SystemDataArgumentIndex)->getType()->getStructElementType(0);
        [[maybe_unused]] bool DidInsert = ToProcess.insert({&F, std::move(Data)}).second;
        assert(DidInsert);
      }
    }
  }
}

bool DXILContPostProcessPassImpl::handleContStackIntrinsics(llvm::ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  for (auto &F : Mod->functions()) {
    if (F.getName().contains("ContStack")) {
      Changed = true;

      auto &FAM = AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(*Mod).getManager();

      handleContStackIntrinsic(FAM, F);
    }
  }

  return Changed;
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

  struct CspCandidateInfo {
    bool RequiresCspArgument = false;
    Function *Func = nullptr;
  };

  SmallVector<CspCandidateInfo> CandidateInfo;

  for (Function &Func : *Mod) {
    if (Func.isDeclaration())
      continue;

    if (lgc::rt::getLgcRtShaderStage(&Func) == lgc::rt::RayTracingShaderStage::KernelEntry) {
      CandidateInfo.push_back({false, &Func});
      continue;
    }

    if (Func.hasMetadata(ContHelper::MDContinuationName)) {
      CandidateInfo.push_back({true, &Func});
      continue;
    }

    if (lgc::cps::isCpsFunction(Func)) {
      CandidateInfo.push_back({true, &Func});
      continue;
    }
  }

  for (auto &[RequiresCspArgument, F] : CandidateInfo) {
    // Lower lgc.cps.jump and lgc.cps.as.continuation.reference ops.
    CpsVisitor.visit(State, *F);

    auto Data = std::move(ToProcess[F]);
    ToProcess.erase(F);

    auto *NewFunc = StackLowering->lowerCpsStackOps(F, GetGlobalMemBase, RequiresCspArgument);

    ToProcess.insert({NewFunc, Data});
  }

  return Changed;
}

void DXILContPostProcessPassImpl::lowerJumpOp(lgc::cps::JumpOp &JumpOp) {
  Builder.SetInsertPoint(&JumpOp);
  Value *RCR = Builder.CreateZExt(JumpOp.getTarget(), Builder.getInt64Ty());

  CallInst *ContinueOp = nullptr;

  SmallVector<Value *> TailArgs{JumpOp.getTail()};
  Value *RetAddr = Builder.CreateZExt(JumpOp.getRcr(), Builder.getInt64Ty());
  if (ContHelper::isWaitAwaitCall(JumpOp)) {
    ContinueOp =
        Builder.create<lgc::ilcps::WaitContinueOp>(RCR, Builder.getInt64(-1), JumpOp.getCsp(), RetAddr, TailArgs);
    ContHelper::removeWaitMask(JumpOp);
  } else {
    ContinueOp = Builder.create<lgc::ilcps::ContinueOp>(RCR, JumpOp.getCsp(), RetAddr, TailArgs);
  }

  ContinueOp->copyMetadata(JumpOp);
  JumpOp.eraseFromParent();
}

void DXILContPostProcessPassImpl::lowerAsContinuationReferenceOp(lgc::cps::AsContinuationReferenceOp &AsCrOp) {
  Builder.SetInsertPoint(&AsCrOp);
  Value *AddrWithMD = Builder.CreateCall(getContinuationGetAddrAndMD(*Mod), {AsCrOp.getFn()});

  if (AsCrOp.getType()->isIntegerTy(32)) {
    // If we are using 32-bit compact VPC, extract metadata and encode it into VPC.
    auto Vpc = Builder.CreateTruncOrBitCast(AddrWithMD, Builder.getInt32Ty());
    Vpc = Builder.CreateAnd(Vpc, 0xFFFFFFC0);

    // Encode shader priority.
    auto RtStage =
        lgc::rt::getLgcRtShaderStage(cast<Function>(AsCrOp.getFn())).value_or(lgc::rt::RayTracingShaderStage::Count);

    auto GetPriorityFromRtStage = [](lgc::rt::RayTracingShaderStage RtStage) {
      // Shader priorities for continuation scheduling. Higher values mean higher scheduling precedence.
      // Reserve priority 0 as invalid value.
      enum SchedulingPriority : unsigned {
        SchedulingPriorityInvalid = 0,
        SchedulingPriorityRgs = 1,
        SchedulingPriorityChs = 2,
        SchedulingPriorityMiss = 2,
        SchedulingPriorityTraversal = 3,
        SchedulingPriorityAhs = 4,
        SchedulingPriorityIs = 5,
        SchedulingPriorityCallable = 6,
        SchedulingPriorityMaxValid = 7
      };
      switch (RtStage) {
      case lgc::rt::RayTracingShaderStage::RayGeneration:
        return SchedulingPriorityRgs;
      case lgc::rt::RayTracingShaderStage::ClosestHit:
        return SchedulingPriorityChs;
      case lgc::rt::RayTracingShaderStage::Miss:
        return SchedulingPriorityMiss;
      case lgc::rt::RayTracingShaderStage::Traversal:
        return SchedulingPriorityTraversal;
      case lgc::rt::RayTracingShaderStage::AnyHit:
        return SchedulingPriorityAhs;
      case lgc::rt::RayTracingShaderStage::Intersection:
        return SchedulingPriorityIs;
      case lgc::rt::RayTracingShaderStage::Callable:
        return SchedulingPriorityCallable;
      default:
        report_fatal_error("Unknown ray tracing shader stage for resume function");
      }
    };

    Vpc = Builder.CreateOr(Vpc, Builder.getInt32(GetPriorityFromRtStage(RtStage)));
    AddrWithMD = Vpc;
  }

  AsCrOp.replaceAllUsesWith(AddrWithMD);
  AsCrOp.eraseFromParent();
}

bool DXILContPostProcessPassImpl::handleAmdInternals() {
  bool Changed = false;

  for (auto &F : Mod->functions()) {
    auto Name = F.getName();
    if (Name.starts_with("_AmdValueI32Count")) {
      Changed = true;
      ContHelper::handleValueI32Count(F, Builder);
    } else if (Name.starts_with("_AmdValueGetI32")) {
      Changed = true;
      ContHelper::handleValueGetI32(F, Builder);
    } else if (Name.starts_with("_AmdValueSetI32")) {
      Changed = true;
      ContHelper::handleValueSetI32(F, Builder);
    }
  }

  return Changed;
}

DXILContPostProcessPassImpl::DXILContPostProcessPassImpl(Module &M, Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Builder{Mod->getContext()}, StackAddrspace{
                                                                            ContHelper::tryGetStackAddrspace(*Mod)} {
}

PreservedAnalyses DXILContPostProcessPassImpl::run(ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  StackLowering.emplace(Mod->getContext(), static_cast<uint32_t>(StackAddrspace.value()));

  if (*StackAddrspace == ContStackAddrspace::Global)
    GetGlobalMemBase = getContinuationStackGlobalMemBase(*GpurtLibrary);

  initializeProcessableFunctionData();

  Changed |= handleAmdInternals();
  Changed |= handleContStackIntrinsics(AnalysisManager);

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
