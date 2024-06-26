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

//===- DXILContPostProcess.cpp - Replace intrinsic calls ------------------===//
//
//  * Insert the initialization of the continuation stack pointer.
//  * Replace dx.op intrinsic calls with calls to the driver implementation
//    and initialize the system data.
//  * Wraps all uses of function pointers into an intrinsic that adds
//    metadata (e.g. VGPR counts) to the function pointer.
//
// The addrspace(20) globals that represent registers are sorted by this pass
// and replaced with indices into a single @REGISTERS global.
//
//===----------------------------------------------------------------------===//

#include "compilerutils/CompilerUtils.h"
#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcIlCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llpc/GpurtEnums.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/CpsStackLowering.h"
#include "llvmraytracing/GpurtContext.h"
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

static cl::opt<bool> ReportContStateSizes(
    "report-cont-state-sizes",
    cl::desc("Report continuation state sizes for entry functions."),
    cl::init(false));

static cl::opt<bool> ReportPayloadRegisterSizes(
    "report-payload-register-sizes",
    cl::desc("Report payload VGPR sizes for functions."), cl::init(false));

static cl::opt<bool> ReportSystemDataSizes(
    "report-system-data-sizes",
    cl::desc("Report incoming system data sizes for functions."),
    cl::init(false));

static cl::opt<bool> ReportAllSizes(
    "report-all-continuation-sizes",
    cl::desc("Report continuation state, payload and system data sizes."),
    cl::init(false));

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
  void lowerGetResumePointAddr(Function &F);
  void handleRegisterBufferGetPointer(Function &F, GlobalVariable *Payload);
  void handleValueI32Count(Function &F);
  void handleValueGetI32(Function &F);
  void handleValueSetI32(Function &F);

  void handleContStackIntrinsic(FunctionAnalysisManager &FAM, Function &F);

  void initializeProcessableFunctionData();
  bool handleRegisterBufferCalls();
  bool replaceIntrinsicCalls(Function &F, const FunctionData &Data);
  bool handleIntrinsicCalls(llvm::ModuleAnalysisManager &AnalysisManager);
  bool lowerCpsOps();
  void lowerJumpOp(lgc::cps::JumpOp &JumpOp);
  bool unfoldGlobals();
  bool handleAmdInternals();

  Module *Mod;
  Module *GpurtLibrary;
  GlobalVariable *Registers;
  MapVector<Function *, FunctionData> ToProcess;
  llvm_dialects::Builder Builder;
  std::optional<ContStackAddrspace> StackAddrspace;
  std::optional<CpsStackLowering> StackLowering;
  CompilerUtils::CrossModuleInliner CrossInliner;

  // For performance reasons, we keep this list of continuation.{wait}Continue
  // calls here and update it when required.
  SmallVector<CallInst *> ContinueCalls;
  Function *GetGlobalMemBase = nullptr;
};

// Collects all calls to continuation.[wait]continue
static void collectContinueCalls(const Module &Mod,
                                 SmallVectorImpl<CallInst *> &ContinueCalls) {
  for (const auto &Name :
       {"continuation.continue", "continuation.waitContinue"}) {
    auto *Func = Mod.getFunction(Name);
    if (!Func)
      continue;

    llvm::forEachCall(
        *Func, [&](CallInst &CInst) { ContinueCalls.push_back(&CInst); });
  }
}

static void reportContStateSizes(Module &M) {
  // Determine the set of entry functions which have a continuation function
  // We cannot rely on the state size for this, because functions without a
  // continuation (e.g. a non-recursive CHS) have a state size of 0 in metadata.
  SetVector<Function *> EntriesWithContinuationFunctions;
  for (auto &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    if (auto *MD = dyn_cast_or_null<MDTuple>(
            F.getMetadata(ContHelper::MDContinuationName))) {
      auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
      if (EntryF != &F)
        EntriesWithContinuationFunctions.insert(EntryF);
    }
  }

  for (auto *F : EntriesWithContinuationFunctions) {
    auto Stage = lgc::rt::getLgcRtShaderStage(F);
    if (!Stage || F->isDeclaration())
      continue;

    auto OptStateSize = ContHelper::tryGetContinuationStateByteCount(*F);
    if (!OptStateSize.has_value())
      continue;

    DXILShaderKind ShaderKind =
        ShaderStageHelper::rtShaderStageToDxilShaderKind(*Stage);
    dbgs() << "Continuation state size of \"" << F->getName() << "\" ("
           << ShaderKind << "): " << OptStateSize.value() << " bytes\n";
  }
}

// For every function with incoming or outgoing (or both) payload registers,
// report the incoming size and the max outgoing size in bytes.
static void reportPayloadSizes(Module &M, ArrayRef<CallInst *> ContinueCalls) {
  // For every function with continue calls, determine the max number of
  // outgoing registers
  DenseMap<Function *, unsigned> MaxOutgoingRegisterCounts;

  for (auto *CallInst : ContinueCalls) {
    auto RegCount = ContHelper::tryGetOutgoingRegisterCount(CallInst).value();
    MaxOutgoingRegisterCounts[CallInst->getFunction()] =
        std::max(MaxOutgoingRegisterCounts[CallInst->getFunction()], RegCount);
  }

  for (auto &F : M) {
    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    if (!Stage || F.isDeclaration())
      continue;

    DXILShaderKind ShaderKind =
        ShaderStageHelper::rtShaderStageToDxilShaderKind(*Stage);
    auto OptIncomingPayloadRegisterCount =
        ContHelper::tryGetIncomingRegisterCount(&F);
    bool HasIncomingPayload = OptIncomingPayloadRegisterCount.has_value();
    auto It = MaxOutgoingRegisterCounts.find(&F);
    bool HasOutgoingPayload = (It != MaxOutgoingRegisterCounts.end());

    if (!HasIncomingPayload && !HasOutgoingPayload)
      continue;

    dbgs() << "Incoming and max outgoing payload VGPR size of \"" << F.getName()
           << "\" (" << ShaderKind << "): ";
    if (HasIncomingPayload) {
      dbgs() << OptIncomingPayloadRegisterCount.value() * RegisterBytes;
    } else {
      dbgs() << "(no incoming payload)";
    }
    dbgs() << " and ";
    if (HasOutgoingPayload) {
      dbgs() << It->second * RegisterBytes;
    } else {
      dbgs() << "(no outgoing payload)";
    }
    dbgs() << " bytes\n";
  }
}

static void reportSystemDataSizes(
    Module &M,
    const MapVector<Function *, DXILContPostProcessPassImpl::FunctionData>
        &FunctionData) {
  for (const auto &[F, FuncData] : FunctionData) {
    if (FuncData.SystemDataTy == nullptr)
      continue;
    auto SystemDataBytes =
        M.getDataLayout().getTypeStoreSize(FuncData.SystemDataTy);

    dbgs() << "Incoming system data of \"" << F->getName() << "\" ("
           << FuncData.Kind << ") is \""
           << FuncData.SystemDataTy->getStructName()
           << "\", size:  " << SystemDataBytes << " bytes\n";
  }
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
[[maybe_unused]] static void
checkContinuationsModule(const Module &M,
                         const SmallVectorImpl<CallInst *> &ContinueCalls) {
  // Check that all continuation.continue calls have registercount metadata.
  for (auto *CallInst : ContinueCalls) {
    if (!ContHelper::tryGetOutgoingRegisterCount(CallInst))
      report_fatal_error("Missing registercount metadata on continue call!");
  }

  // Check that every function has at most one setLocalRootIndex call.
  if (auto *SetF = M.getFunction("amd.dx.setLocalRootIndex")) {
    SmallDenseSet<Function *> HasSetF;

    llvm::forEachCall(*SetF, [&](CallInst &CInst) {
      // Returns true if it is a new value
      auto Inserted = HasSetF.insert(CInst.getFunction());
      if (!Inserted.second)
        report_fatal_error(
            "Found a function with more than one setLocalRootIndex");
    });
  }

  // Check that resume functions do not have a stack size set.
  for (auto &Func : M) {
    if (auto *MD = dyn_cast_or_null<MDTuple>(
            Func.getMetadata(ContHelper::MDContinuationName))) {
      auto *StartFunc = extractFunctionOrNull(MD->getOperand(0));
      bool IsStart = (&Func == StartFunc);
      bool HasStackSizeMetadata =
          ContHelper::tryGetStackSize(&Func).has_value();
      if (!IsStart && HasStackSizeMetadata)
        report_fatal_error("Found resume function with stack size metadata!");
    }
  }
}

/// Replace a global with a part of another global.
/// Helper method for merging multiple globals into one.
static void replaceGlobal(const DataLayout &DL, GlobalVariable *Registers,
                          GlobalVariable *G, uint64_t Offset) {
  LLVM_DEBUG(dbgs() << "Offset for global " << G->getName()
                    << " in @REGISTERS: " << (Offset / RegisterBytes) << "\n");

  auto *I64 = Type::getInt64Ty(G->getContext());
  SmallVector<Constant *, 2> Indices = {
      ConstantInt::get(I64, 0), ConstantInt::get(I64, Offset / RegisterBytes)};
  Constant *Gep = Offset == 0
                      ? Registers
                      : ConstantExpr::getInBoundsGetElementPtr(
                            Registers->getValueType(), Registers, Indices);
  auto *Repl = ConstantExpr::getBitCast(Gep, G->getType());

  G->replaceAllUsesWith(Repl);
  G->eraseFromParent();
}

void DXILContPostProcessPassImpl::lowerGetResumePointAddr(Function &F) {
  auto *GetResumePointAddr = &F;

  assert(GetResumePointAddr->getReturnType()->isIntegerTy(64) &&
         GetResumePointAddr->arg_size() == 0);

  // Search calls to GetResumePointAddr, and lower it to the argument of the
  // next continue call. Then remove it from that continue call.
  for (auto &Use : make_early_inc_range(GetResumePointAddr->uses())) {
    auto *CInst = dyn_cast<CallInst>(Use.getUser());
    if (!CInst || !CInst->isCallee(&Use) ||
        ToProcess.count(CInst->getFunction()) == 0) {
      // Non-call use, or call in unknown function. This will likely result in a
      // remaining non-lowered call reported as error at the end of this
      // function.
      continue;
    }

    // Instead of passing the resume address to the next continue call,
    // use it as the return value of GetResumePointAddr and remove it from
    // the continue arguments.
    auto FoundContinueCall = findDominatedContinueCall(CInst);

    if (!FoundContinueCall) {
      report_fatal_error("Did not find a continue call after a "
                         "GetResumePointAddr");
    }
    auto *ContinueCall = *FoundContinueCall;

    // Only used for non-cps functions.
    unsigned ReturnAddrArgNum = 1;
    Value *ReturnAddr = nullptr;

    if (auto *Jump = dyn_cast<lgc::cps::JumpOp>(ContinueCall); Jump) {
      ReturnAddr = Jump->getTarget();
    } else {
      auto Name = ContinueCall->getCalledFunction()->getName();

      if (Name != "continuation.continue" &&
          Name != "continuation.waitContinue")
        report_fatal_error("The BB must end in a continue call after a "
                           "GetResumePointAddr");

      bool HasWaitMask = Name == "continuation.waitContinue";
      ReturnAddrArgNum = HasWaitMask ? 2 : 1;
      // Move up computation of the resume address
      ReturnAddr = ContinueCall->getArgOperand(ReturnAddrArgNum);

      assert((ReturnAddr->getType() == Builder.getInt64Ty()) &&
             "Unexpected return addr type!");
    }

    SmallVector<Instruction *> MoveInstrs;
    if (auto *I = dyn_cast<Instruction>(ReturnAddr)) {
      if (!I->comesBefore(CInst))
        MoveInstrs.push_back(I);
    }

    unsigned Done = 0;
    while (Done < MoveInstrs.size()) {
      for (auto &O : MoveInstrs[Done]->operands()) {
        if (auto *I = dyn_cast<Instruction>(O)) {
          if (!I->comesBefore(CInst))
            MoveInstrs.push_back(I);
        }
      }
      ++Done;
    }
    for (auto I = MoveInstrs.rbegin(), E = MoveInstrs.rend(); I != E; ++I)
      (*I)->moveBefore(CInst);

    CInst->replaceAllUsesWith(ReturnAddr);

    // Re-create the continuation.continue call without the return address
    // argument.
    SmallVector<Value *> Args;
    for (unsigned I = 0; I < ContinueCall->arg_size(); I++) {
      if (I != ReturnAddrArgNum)
        Args.push_back(ContinueCall->getArgOperand(I));
    }

    Builder.SetInsertPoint(ContinueCall);
    auto *NewCall = Builder.CreateCall(ContinueCall->getCalledFunction(), Args);
    NewCall->copyMetadata(*ContinueCall);

    CInst->eraseFromParent();
    ContinueCall->eraseFromParent();
  }
}

void DXILContPostProcessPassImpl::handleRegisterBufferGetPointer(
    Function &F, GlobalVariable *Payload) {
  // Check calls that take the payload as argument
  llvm::forEachCall(F, [&](CallInst &CInst) {
    if (isCastGlobal(Payload, CInst.getOperand(0))) {
      // Replace call with first part of payload
      static_assert(FirstPayloadMemoryPointerRegister == 0,
                    "Need to adjust offset here");
      Builder.SetInsertPoint(&CInst);

      Type *StackOffsetTy = CInst.getType();

      // Load an addrspace(32) pointer from the payload global and let stack
      // handling do the conversion into adds/muls and GEPs.
      auto *CastPayload = Builder.CreateBitOrPointerCast(
          Payload, StackOffsetTy->getPointerTo(Payload->getAddressSpace()));
      Value *Ptr = Builder.CreateLoad(StackOffsetTy, CastPayload);
      CInst.replaceAllUsesWith(Ptr);
      CInst.eraseFromParent();
    }
  });
}

void DXILContPostProcessPassImpl::handleValueI32Count(Function &F) {
  assert(F.arg_size() == 1
         // i32 count
         && F.getFunctionType()->getReturnType()->isIntegerTy(32)
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy());

  auto *Ty = getFuncArgPtrElementType(&F, 0);
  auto *Size = Builder.getInt32(
      Mod->getDataLayout().getTypeStoreSize(Ty).getFixedValue() / 4);
  llvm::replaceCallsToFunction(F, *Size);
}

void DXILContPostProcessPassImpl::handleValueGetI32(Function &F) {
  assert(F.arg_size() == 2
         // value
         && F.getFunctionType()->getReturnType()->isIntegerTy(32)
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // index
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  auto *I32 = Builder.getInt32Ty();

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    Value *Addr =
        Builder.CreateBitCast(CInst.getArgOperand(0), I32->getPointerTo());
    Addr = Builder.CreateGEP(I32, Addr, CInst.getArgOperand(1));
    auto *Load = Builder.CreateLoad(I32, Addr);
    CInst.replaceAllUsesWith(Load);
    CInst.eraseFromParent();
  });
}

void DXILContPostProcessPassImpl::handleValueSetI32(Function &F) {
  assert(F.arg_size() == 3 &&
         F.getFunctionType()->getReturnType()->isVoidTy()
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // index
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32)
         // value
         && F.getFunctionType()->getParamType(2)->isIntegerTy(32));

  auto *I32 = Builder.getInt32Ty();
  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    Value *Addr =
        Builder.CreateBitCast(CInst.getArgOperand(0), I32->getPointerTo());
    Addr = Builder.CreateGEP(I32, Addr, CInst.getArgOperand(1));
    Builder.CreateStore(CInst.getArgOperand(2), Addr);
    CInst.eraseFromParent();
  });
}

// Replace calls to _AmdContStack* with calls to lgc.cps dialect ops.
// Do some simple constant propagation on the fly.
void DXILContPostProcessPassImpl::handleContStackIntrinsic(
    FunctionAnalysisManager &FAM, Function &F) {

  // Check if the function is either of void return type or i32 return type and
  // has no arguments or a single integer argument dividable by 32 (to allow
  // storing and loading multiple dwords via AmdContStackLoad /
  // AmdContStackStore).
  Type *ReturnTy = F.getReturnType();
  (void)ReturnTy;
  assert(
      (ReturnTy->isVoidTy() || (ReturnTy->isIntegerTy() &&
                                (ReturnTy->getIntegerBitWidth() % 32 == 0))) &&
      "DXILContPostProcessPassImpl::handleContStackIntrinsic: Invalid "
      "return type!");

  Type *FuncTy = F.getFunctionType();
  (void)(FuncTy);
  assert((FuncTy->getFunctionNumParams() == 0 ||
          FuncTy->getFunctionParamType(0)->isIntegerTy()) &&
         "DXILContPostProcessPassImpl::handleContStackIntrinsic: Invalid "
         "argument signature!");

  StringRef FuncName = F.getName();
  FuncName.consume_front("_AmdContStack");

  auto ConstantFoldInstruction = [&](Function *Parent,
                                     Value *SizeArg) -> Value * {
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
      const SimplifyQuery SQ(Parent->getParent()->getDataLayout(), &TLI, &DT,
                             &AC);

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
      Value *SizeArg =
          ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Replacement = Builder.create<lgc::cps::AllocOp>(SizeArg);

      if (auto *Size = dyn_cast<ConstantInt>(SizeArg))
        ContHelper::addStackSize(CInst.getFunction(), Size->getSExtValue());
    } else if (FuncName.starts_with("Free")) {
      Value *SizeArg =
          ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Replacement = Builder.create<lgc::cps::FreeOp>(SizeArg);
    } else if (FuncName.starts_with("SetPtr")) {
      Value *Vsp = CInst.getArgOperand(0);
      Replacement = Builder.create<lgc::cps::SetVspOp>(Builder.CreateIntToPtr(
          Vsp,
          PointerType::get(Builder.getInt8Ty(), lgc::cps::stackAddrSpace)));
    } else if (FuncName.starts_with("GetPtr")) {
      Replacement = Builder.create<lgc::cps::GetVspOp>();
    } else if (FuncName.starts_with("Load")) {
      Value *Addr =
          ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Value *Ptr = Builder.CreateIntToPtr(
          Addr, CInst.getType()->getPointerTo(lgc::cps::stackAddrSpace));
      Replacement = Builder.CreateAlignedLoad(
          DestTy, Ptr,
          Align(CpsStackLowering::getContinuationStackAlignment()));

      if (FuncName.starts_with("LoadLastUse"))
        CompilerUtils::setIsLastUseLoad(*cast<LoadInst>(Replacement));

      IsMemoryAccess = true;
    } else if (FuncName.starts_with("Store")) {
      assert(FuncTy->getFunctionNumParams() == 2 &&
             "DXILContPostProcessPassImpl::handleContStackIntrinsic: Invalid "
             "argument signature for AmdContStackStore!");

      Value *Addr =
          ConstantFoldInstruction(CInst.getFunction(), CInst.getArgOperand(0));
      Value *Val = CInst.getArgOperand(1);
      Value *Ptr = Builder.CreateIntToPtr(
          Addr, Val->getType()->getPointerTo(lgc::cps::stackAddrSpace));
      Builder.CreateAlignedStore(
          Val, Ptr, Align(CpsStackLowering::getContinuationStackAlignment()));

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
    if (lgc::rt::getLgcRtShaderStage(&F) ==
        lgc::rt::RayTracingShaderStage::KernelEntry) {
      FunctionData Data;
      Data.Kind = DXILShaderKind::Compute;
      [[maybe_unused]] bool DidInsert =
          ToProcess.insert({&F, std::move(Data)}).second;
      assert(DidInsert);
      continue;
    }

    // Handle entry functions first
    if (auto *MD = dyn_cast_or_null<MDTuple>(
            F.getMetadata(ContHelper::MDContinuationName))) {
      auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
      if (&F != EntryF)
        continue;
    } else {
      continue;
    }

    DXILShaderKind Kind =
        ShaderStageHelper::rtShaderStageToDxilShaderKind(*Stage);
    const bool IsCpsFunction = lgc::cps::isCpsFunction(F);

    switch (Kind) {
    case DXILShaderKind::RayGeneration: {
      FunctionData Data;
      Data.Kind = Kind;

      Data.SystemDataArgumentIndex =
          !IsCpsFunction ? SystemDataArgumentIndex : CpsArgIdxSystemData;

      Data.SystemDataTy =
          F.getFunctionType()->getParamType(Data.SystemDataArgumentIndex);

      [[maybe_unused]] bool DidInsert =
          ToProcess.insert({&F, std::move(Data)}).second;
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

      Data.SystemDataArgumentIndex =
          !IsCpsFunction ? SystemDataArgumentIndex : CpsArgIdxSystemData;
      Data.SystemDataTy =
          F.getFunctionType()->getParamType(Data.SystemDataArgumentIndex);
      [[maybe_unused]] bool DidInsert =
          ToProcess.insert({&F, std::move(Data)}).second;
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
    if (auto *MD = dyn_cast_or_null<MDTuple>(
            F.getMetadata(ContHelper::MDContinuationName))) {
      auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
      auto Stage = lgc::rt::getLgcRtShaderStage(EntryF);
      if (Stage && &F != EntryF) {
        FunctionData Data = ToProcess[EntryF];
        Data.IsStart = false;

        Data.SystemDataArgumentIndex = !lgc::cps::isCpsFunction(F)
                                           ? SystemDataArgumentIndex
                                           : CpsArgIdxSystemData;

        Data.SystemDataTy = F.getArg(Data.SystemDataArgumentIndex)->getType();
        [[maybe_unused]] bool DidInsert =
            ToProcess.insert({&F, std::move(Data)}).second;
        assert(DidInsert);
      }
    }
  }
}

bool DXILContPostProcessPassImpl::handleRegisterBufferCalls() {
  bool Changed = false;
  auto *Payload = Mod->getGlobalVariable(ContHelper::GlobalPayloadName);

  for (auto &F : Mod->functions()) {
    auto Name = F.getName();
    if (Name.starts_with("registerbuffer.setpointerbarrier")) {
      // Remove setpointerbarrier instructions related to payload
      llvm::forEachCall(F, [&](CallInst &CInst) {
        if (isCastGlobal(Payload, CInst.getOperand(0))) {
          CInst.eraseFromParent();
          Changed = true;
        }
      });
    } else if (Name.starts_with("registerbuffer.getpointer")) {
      Changed = true;
      handleRegisterBufferGetPointer(F, Payload);
    }
  }

  return Changed;
}

bool DXILContPostProcessPassImpl::handleIntrinsicCalls(
    llvm::ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  for (auto &F : Mod->functions()) {
    auto Name = F.getName();
    if (Name.starts_with("lgc.rt")) {
      // Search for known HLSL intrinsics
      llvm::forEachCall(F, [&](CallInst &CInst) {
        auto Data = ToProcess.find(CInst.getFunction());
        if (Data != ToProcess.end()) {
          auto IntrImplEntry = llvm::findIntrImplEntryByIntrinsicCall(&CInst);
          if (IntrImplEntry == std::nullopt)
            return;

          Data->second.IntrinsicCalls.push_back(&CInst);
          Changed = true;
        }
      });
    } else if (Name.contains("ContStack")) {
      Changed = true;

      auto &FAM =
          AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(*Mod)
              .getManager();

      handleContStackIntrinsic(FAM, F);
    }
  }

  return Changed;
}

bool DXILContPostProcessPassImpl::replaceIntrinsicCalls(
    Function &F, const FunctionData &Data) {
  if (Data.IntrinsicCalls.empty())
    return false;

  [[maybe_unused]] auto *FuncTy = F.getFunctionType();

  assert(FuncTy->getNumParams() > Data.SystemDataArgumentIndex &&
         "Missing system data argument");
  Builder.SetInsertPointPastAllocas(&F);

  // Intrinsics need a pointer, so allocate and store the system data argument
  Argument *SystemDataArgument = F.getArg(Data.SystemDataArgumentIndex);
  Value *SystemDataPtr = Builder.CreateAlloca(Data.SystemDataTy);
  SystemDataPtr->setName("system.data.alloca");
  Builder.CreateStore(SystemDataArgument, SystemDataPtr);

  for (auto *Call : Data.IntrinsicCalls)
    replaceIntrinsicCall(
        Builder, Data.SystemDataTy, SystemDataPtr,
        ShaderStageHelper::dxilShaderKindToRtShaderStage(Data.Kind).value(),
        Call, GpurtLibrary, CrossInliner);

  return true;
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
    Function *GetAddrAndMD;
  };

  // Note: It is a bit unlucky that we are using both a visitor for
  // lgc.cps.as.continuation.reference and lgc.cps.jump and a loop for the
  // actual stack lowering. It would be nice to use a visitor for both of them,
  // but currently, there seems to be no support in dialects for marrying both
  // approaches: we would need a visitor that supports visiting function
  // definitions as well.
  static const auto CpsVisitor =
      llvm_dialects::VisitorBuilder<CpsVisitorState>()
          .add<lgc::cps::AsContinuationReferenceOp>(
              [](CpsVisitorState &State,
                 lgc::cps::AsContinuationReferenceOp &AsCrOp) {
                State.Builder.SetInsertPoint(&AsCrOp);
                auto *AddrWithMD = State.Builder.CreateCall(State.GetAddrAndMD,
                                                            {AsCrOp.getFn()});
                AsCrOp.replaceAllUsesWith(AddrWithMD);
                AsCrOp.eraseFromParent();
                State.Changed = true;
              })
          .add<lgc::cps::JumpOp>(
              [](CpsVisitorState &State, lgc::cps::JumpOp &JumpOp) {
                State.Self.lowerJumpOp(JumpOp);
                State.Changed = true;
              })
          .build();

  CpsVisitorState State{*this, Changed, Builder,
                        getContinuationGetAddrAndMD(*Mod)};

  struct CspCandidateInfo {
    bool RequiresCspArgument = false;
    Function *Func = nullptr;
  };

  SmallVector<CspCandidateInfo> CandidateInfo;

  for (Function &Func : *Mod) {
    if (Func.isDeclaration())
      continue;

    if (lgc::rt::getLgcRtShaderStage(&Func) ==
        lgc::rt::RayTracingShaderStage::KernelEntry) {
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

    auto *NewFunc = StackLowering->lowerCpsStackOps(F, GetGlobalMemBase,
                                                    RequiresCspArgument);

    ToProcess.insert({NewFunc, Data});
  }

  collectContinueCalls(*Mod, ContinueCalls);

  return Changed;
}

void DXILContPostProcessPassImpl::lowerJumpOp(lgc::cps::JumpOp &JumpOp) {
  Builder.SetInsertPoint(&JumpOp);
  Value *RCR = JumpOp.getTarget();

  Function *Continue = ContHelper::isWaitAwaitCall(JumpOp)
                           ? llvm::getContinuationWaitContinue(*Mod)
                           : llvm::getContinuationContinue(*Mod);

  SmallVector<Value *, 4> Args;
  Args.push_back(Builder.CreateZExt(RCR, Builder.getInt64Ty()));

  // If this is a wait call, then the wait mask is at the start of the tail
  // argument list.
  Args.append(JumpOp.getTail().begin(), JumpOp.getTail().end());

  CallInst *ContinueCall = Builder.CreateCall(Continue, Args);
  ContinueCall->copyMetadata(JumpOp);
  ContHelper::removeIsWaitAwaitMetadata(*ContinueCall);
  JumpOp.eraseFromParent();
}

bool DXILContPostProcessPassImpl::unfoldGlobals() {
  // Replace register globals with indices into a bigger global
  const auto &DL = Mod->getDataLayout();
  GlobalVariable *PayloadGlobal =
      Mod->getGlobalVariable(ContHelper::GlobalPayloadName);

  if (PayloadGlobal) {
    // We use the maximum size for the continuation state and the actual size
    // for the payload, so that the offset of the payload stays the same, but
    // the global is only as big as necessary.
    uint32_t RequiredSize =
        PayloadGlobal->getValueType()->getArrayNumElements() * RegisterBytes;

    // Put continuation state first, it's callee save so we need to have it
    // full in all cases. Payload can be truncated, so the backend is free to
    // use registers that are unused in a function.
    auto *I32 = Type::getInt32Ty(Mod->getContext());
    auto *RegistersTy = ArrayType::get(I32, RequiredSize / RegisterBytes);
    Registers = cast<GlobalVariable>(Mod->getOrInsertGlobal(
        ContHelper::GlobalRegistersName, RegistersTy, [&] {
          return new GlobalVariable(
              *Mod, RegistersTy, false, GlobalVariable::ExternalLinkage,
              nullptr, ContHelper::GlobalRegistersName, nullptr,
              GlobalVariable::NotThreadLocal, GlobalRegisterAddrspace);
        }));

    replaceGlobal(DL, Registers, PayloadGlobal, 0);

    return true;
  }

  return false;
}

bool DXILContPostProcessPassImpl::handleAmdInternals() {
  bool Changed = false;

  for (auto &F : Mod->functions()) {
    auto Name = F.getName();
    if (Name.starts_with("_AmdValueI32Count")) {
      Changed = true;
      handleValueI32Count(F);
    } else if (Name.starts_with("_AmdValueGetI32")) {
      Changed = true;
      handleValueGetI32(F);
    } else if (Name.starts_with("_AmdValueSetI32")) {
      Changed = true;
      handleValueSetI32(F);
    }
  }

  return Changed;
}

DXILContPostProcessPassImpl::DXILContPostProcessPassImpl(Module &M,
                                                         Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary}, Builder{Mod->getContext()},
      StackAddrspace{ContHelper::tryGetStackAddrspace(*Mod)} {}

PreservedAnalyses
DXILContPostProcessPassImpl::run(ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  StackLowering.emplace(Mod->getContext(),
                        static_cast<uint32_t>(StackAddrspace.value()));

  if (*StackAddrspace == ContStackAddrspace::Global)
    GetGlobalMemBase = getContinuationStackGlobalMemBase(*GpurtLibrary);

  initializeProcessableFunctionData();

  Changed |= handleRegisterBufferCalls();
  Changed |= unfoldGlobals();
  Changed |= handleAmdInternals();
  Changed |= handleIntrinsicCalls(AnalysisManager);

  for (auto &[Func, Data] : ToProcess)
    Changed |= replaceIntrinsicCalls(*Func, Data);

  for (auto &F : make_early_inc_range(*Mod)) {
    auto FuncName = F.getName();
    if (FuncName.starts_with("_AmdGetResumePointAddr")) {
      Changed = true;
      lowerGetResumePointAddr(F);
    } else if (FuncName.starts_with("_AmdComplete")) {
      Changed = true;
      llvm::forEachCall(F, [&](llvm::CallInst &CInst) {
        llvm::terminateShader(Builder, &CInst);
      });
    }
  }

  Changed |= lowerCpsOps();

  Changed |= fixupDxilMetadata(*Mod);

#ifndef NDEBUG
  checkContinuationsModule(*Mod, ContinueCalls);
#endif

  if (ReportContStateSizes || ReportAllSizes)
    reportContStateSizes(*Mod);

  if (ReportPayloadRegisterSizes || ReportAllSizes)
    reportPayloadSizes(*Mod, ContinueCalls);

  if (ReportSystemDataSizes || ReportAllSizes)
    reportSystemDataSizes(*Mod, ToProcess);

  Changed |= llvm::removeUnusedFunctionDecls(Mod, false);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // anonymous namespace

llvm::PreservedAnalyses
DXILContPostProcessPass::run(llvm::Module &Module,
                             llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass dxil-cont-post-process\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Module);

  auto &GpurtContext = lgc::GpurtContext::get(Module.getContext());
  DXILContPostProcessPassImpl Impl{
      Module, GpurtContext.theModule ? *GpurtContext.theModule : Module};
  return Impl.run(AnalysisManager);
}
