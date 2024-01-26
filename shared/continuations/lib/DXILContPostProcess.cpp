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
#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "continuations/ContinuationsUtil.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
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
  bool run(llvm::ModuleAnalysisManager &AnalysisManager);

  static constexpr unsigned SystemDataArgumentIndexStart = 2;
  static constexpr unsigned SystemDataArgumentIndexContinuation = 1;
  static constexpr unsigned SystemDataArgumentIndexRayGen = 0;

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
  void handleInitialContinuationStackPtr(Function &F);
  void handleLgcRtIntrinsic(Function &F);
  void handleRegisterBufferSetPointerBarrier(Function &F,
                                             GlobalVariable *Payload);
  void handleRegisterBufferGetPointer(Function &F, GlobalVariable *Payload);
  void handleValueI32Count(Function &F);
  void handleValueGetI32(Function &F);
  void handleValueSetI32(Function &F);

  void handleContPayloadRegisterI32Count(Function &F);
  void handleContPayloadRegistersGetI32(Function &F);
  void handleContPayloadRegistersSetI32(Function &F);
  void handleContStackAlloc(FunctionAnalysisManager &FAM, Function &F);

  bool replaceIntrinsicCalls(Function &F, const FunctionData &Data);
  [[nodiscard]] std::pair<bool, Function *>
  insertSetupRayGen(Function &F, const FunctionData &Data);

  void collectProcessableFunctions();
  bool handleIntrinsicCalls();
  bool replaceIntrinsicCallsAndSetupRayGen();
  bool unfoldGlobals();
  bool handleAmdInternals(llvm::ModuleAnalysisManager &AnalysisManager);

  Module *Mod;
  Module *GpurtLibrary;
  GlobalVariable *Registers;
  MapVector<Function *, FunctionData> ToProcess;
  Function *SetupRayGen;
  IRBuilder<> Builder;
  CompilerUtils::CrossModuleInliner CrossInliner;
};

// Collects all calls to continuation.[wait]continue
static void collectContinueCalls(const Module &M,
                                 SmallVectorImpl<CallInst *> &CallInsts) {
  for (const auto &Name :
       {"continuation.continue", "continuation.waitContinue"}) {
    auto *Func = M.getFunction(Name);
    if (!Func)
      continue;

    llvm::forEachCall(*Func,
                      [&](CallInst &CInst) { CallInsts.push_back(&CInst); });
  }
}

static void reportContStateSizes(Module &M) {
  // Determine the set of entry functions which have a continuation function
  // We cannot rely on the state size for this, because functions without a
  // continuation (e.g. a non-recursive CHS) have a state size of 0 in metadata.
  DenseSet<Function *> EntriesWithContinuationFunctions;
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
  for (auto &F : M) {
    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    if (!Stage || F.isDeclaration())
      continue;

    if (!EntriesWithContinuationFunctions.contains(&F))
      continue;

    auto OptStateSize = ContHelper::tryGetContinuationStateByteCount(F);
    if (!OptStateSize.has_value())
      continue;

    DXILShaderKind ShaderKind =
        ShaderStageHelper::shaderStageToDxilShaderKind(*Stage);
    dbgs() << "Continuation state size of \"" << F.getName() << "\" ("
           << ShaderKind << "): " << OptStateSize.value() << " bytes\n";
  }
}

// For every function with incoming or outgoing (or both) payload registers,
// report the incoming size and the max outgoing size in bytes.
static void reportPayloadSizes(Module &M) {
  // For every function with continue calls, determine the max number of
  // outgoing registers
  DenseMap<Function *, unsigned> MaxOutgoingRegisterCounts;
  SmallVector<CallInst *> ContinueCalls;
  collectContinueCalls(M, ContinueCalls);

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
        ShaderStageHelper::shaderStageToDxilShaderKind(*Stage);
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
  // To avoid having multiple copies of the intrinsic for each referenced
  // function type, keep existing inttoptr to convert the function pointer to
  // i64, and pass that i64 to the intrinsic.
  // TODO: With opaque pointers, instead just pass a ptr to the function.
  auto *FuncTy = FunctionType::get(I64, {I64}, false);
  return cast<Function>(M.getOrInsertFunction(Name, FuncTy).getCallee());
}

// If this function returns false, we know that F cannot be used as pointer,
// e.g. because it is an intrinsic.
static bool canBeUsedAsPtr(const Function &F) {
  return !F.getName().starts_with("dx.op");
}

// Collects all function pointers (uses of functions that are not calls),
// and adds metadata to them using the `continuations.getAddrAndMD` intrinsic.
// TODO: In the future, we might instead want to directly insert the intrinsic
//       in places depending on function pointers (resume functions, and
//       traversal). This function here is a stop-gap to enable implementing the
//       intrinsic without having to deal with all corner cases that might arise
//       above. Thus, we are just handling the cases known to occur.
//       One might be tempted to speed this up by instead traversing usages of
//       the continuation.continue intrinsics, getting the passed function
//       pointers, and tracing these back. However, this way we would miss
//       function pointers stored to memory, as we do for the return address
//       stored in system data.
static bool addGetAddrAndMDIntrinsicCalls(Module &M) {
  Function *GetAddrAndMD = getContinuationGetAddrAndMD(M);
  IRBuilder<> B{M.getContext()};

  bool Changed = false;
  // We will first traverse all uses, and resolve everything up to constant
  // expressions. However, there might be nested constant expressions, each
  // having multiple users, so we resolve those using a worklist.
  SmallVector<ConstantExpr *> CEWorkList;
  SmallVector<User *> CurrentCEUsers;

  for (auto &F : M.functions()) {
    // Speed-up: Skip F if it cannot be used as pointer, e.g. dx intrinsics.
    if (!canBeUsedAsPtr(F))
      continue;

    CEWorkList.clear();
    for (auto *U : F.users()) {
      // Ignore calls of the function.
      auto *CI = dyn_cast<CallInst>(U);
      if (CI && CI->getCalledFunction() == &F)
        continue;

      if (auto *GA = dyn_cast<GlobalAlias>(U)) {
        // Ignore global aliases. Check that these have no users,
        // as these would need to be changed.
        assert(GA->user_empty());
        continue;
      }

      // Must be ConstantExpr
      ConstantExpr *CE = cast<ConstantExpr>(U);
      CEWorkList.push_back(CE);
    }

    while (!CEWorkList.empty()) {
      auto *CE = CEWorkList.pop_back_val();
      assert((isa<BitCastOperator, PtrToIntOperator>(CE)) &&
             "Unexpected use of function!");

      // Copy the users of CE into a local SmallVector before traversing it,
      // because we are going to add new users of CE that we do *not* want to
      // traverse.
      CurrentCEUsers.assign(CE->user_begin(), CE->user_end());
      for (User *CEU : CurrentCEUsers) {
        if (auto *NestedCE = dyn_cast<ConstantExpr>(CEU)) {
          CEWorkList.push_back(NestedCE);
          continue;
        }

        if (auto *GA = dyn_cast<GlobalAlias>(CEU)) {
          assert(GA->user_empty());
          continue;
        }

        // Final case: A real instruction using the function. Wrap
        // the value into the intrinsic and pass that one to the instruction
        // Set insertion point, and replace CE with the intrinsic
        auto *I = cast<Instruction>(CEU);
        assert(CE->getType() == Type::getInt64Ty(M.getContext()) &&
               "Function use should be as an i64!");
        B.SetInsertPoint(I);
        auto *AddrWithMD =
            B.CreateCall(GetAddrAndMD, {B.CreatePtrToInt(CE, B.getInt64Ty())});
        // Can't RAUW because the CE might be used by different instructions.
        // Instead, manually replace the instruction's operand.
        [[maybe_unused]] bool Found = false;
        for (unsigned OpIdx = 0, E = I->getNumOperands(); OpIdx < E; ++OpIdx) {
          if (I->getOperand(OpIdx) == CE) {
            I->setOperand(OpIdx, AddrWithMD);
            Found = true;
            Changed = true;
          }
        }
        assert(Found);
      }
    }
  }

  return Changed;
}

/// Checks some properties guaranteed for a module containing continuations
/// as expected by the backend.
[[maybe_unused]] static void checkContinuationsModule(const Module &M) {
  // Check that all continuation.continue calls have registercount metadata.
  SmallVector<CallInst *> CallInsts;
  collectContinueCalls(M, CallInsts);
  for (auto *CallInst : CallInsts) {
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

/// Look for the continue call that follows the call to GetResumePointAddr.
/// Due to saving the payload before, many basic blocks may have been inserted,
/// traverse them while making sure that this GetResumePointAddr is the only
/// possible predecessor.
static std::optional<CallInst *> findContinueCall(CallInst *GetResPointAddr) {
  SmallDenseSet<BasicBlock *> Visited;
  SmallDenseSet<BasicBlock *> UnknownPreds;
  SmallVector<BasicBlock *> WorkList;
  CallInst *Candidate = nullptr;
  Visited.insert(GetResPointAddr->getParent());
  WorkList.push_back(GetResPointAddr->getParent());

  while (!WorkList.empty()) {
    auto *BB = WorkList.pop_back_val();
    // Check predecessors
    if (BB != GetResPointAddr->getParent()) {
      for (auto *Pred : predecessors(BB)) {
        if (!Visited.contains(Pred))
          UnknownPreds.insert(Pred);
      }
    }

    auto *Terminator = BB->getTerminator();
    if (isa_and_nonnull<UnreachableInst>(Terminator)) {
      auto Before = --Terminator->getIterator();
      if (auto *ContinueCall = dyn_cast<CallInst>(Before)) {
        if (Candidate != nullptr) {
          LLVM_DEBUG(dbgs() << "Found multiple continue candidates after a "
                               "GetResumePointAddr:\n";
                     Candidate->dump(); ContinueCall->dump());
          return {};
        }
        Candidate = ContinueCall;
      } else {
        LLVM_DEBUG(dbgs() << "The BB must end in a (continue) call after a "
                             "GetResumePointAddr, but "
                          << BB->getName() << " doesn't");
        return {};
      }
    }

    for (auto *Succ : successors(BB)) {
      if (Visited.contains(Succ))
        continue;
      Visited.insert(Succ);
      UnknownPreds.erase(Succ);
      WorkList.push_back(Succ);
    }
  }

  if (Candidate == nullptr) {
    LLVM_DEBUG(
        dbgs() << "Did not find a continue call after a GetResumePointAddr\n");
    return {};
  }

  if (!UnknownPreds.empty()) {
    LLVM_DEBUG(dbgs() << "Found more than one predecessor for the continue "
                         "call after a GetResumePointAddr:\n";
               for (auto *Pred
                    : UnknownPreds) Pred->dump(););
    return {};
  }

  return Candidate;
}

void DXILContPostProcessPassImpl::lowerGetResumePointAddr(Function &F) {
  auto *GetResumePointAddr = &F;

  assert(GetResumePointAddr->getReturnType()->isIntegerTy(64) &&
         GetResumePointAddr->arg_size() == 0);

  // Search calls to GetResumePointAddr, and lower it to the argument of the
  // next continue call. Then remove it from that continue call.
  // TODO: The return address being implicitly added to the next continue call,
  //       and then being implicitly removed by the use of this intrinsic feels
  //       a bit fragile.
  //       We are currently planning to move to a scheme where every await
  //       call is preceded by a call to GetResumePointAddr (in order to set
  //       a scheduling priority). If we decide to stick to that scheme, we
  //       could instead move lowering of GetResumePointAddr() to the
  //       continuation cleanup pass before forming continue calls, and then
  //       never add the resume address to the continue call there. I'm leaving
  //       this for later in case we change the scheme again to avoid
  //       unnecessary code churn. For the time being, the resume function
  //       being added to the continue statement is necessary for us to find
  //       it here.
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
    auto FoundContinueCall = findContinueCall(CInst);

    if (!FoundContinueCall) {
      report_fatal_error("Did not find a continue call after a "
                         "GetResumePointAddr");
    }
    auto *ContinueCall = *FoundContinueCall;
    auto Name = ContinueCall->getCalledFunction()->getName();
    if (Name != "continuation.continue" && Name != "continuation.waitContinue")
      report_fatal_error("The BB must end in a continue call after a "
                         "GetResumePointAddr");

    bool HasWaitMask = Name == "continuation.waitContinue";
    unsigned ReturnAddrArgNum = HasWaitMask ? 3 : 2;
    // Move up computation of the resume address
    auto *ReturnAddr = ContinueCall->getArgOperand(ReturnAddrArgNum);
    assert(ReturnAddr->getType() == Builder.getInt64Ty() &&
           "Unexpected return addr type!");

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
    SmallVector<Value *> Args;
    for (unsigned I = 0; I < ContinueCall->arg_size(); I++) {
      if (I != ReturnAddrArgNum)
        Args.push_back(ContinueCall->getArgOperand(I));
    }

    Builder.SetInsertPoint(ContinueCall);
    auto *NewCall = Builder.CreateCall(ContinueCall->getCalledFunction(), Args);
    // Copy metadata
    SmallVector<std::pair<unsigned int, MDNode *>> MDs;
    ContinueCall->getAllMetadata(MDs);
    for (auto &MD : MDs)
      NewCall->setMetadata(MD.first, MD.second);

    CInst->eraseFromParent();
    ContinueCall->eraseFromParent();
  }

  if (!GetResumePointAddr->use_empty())
    report_fatal_error("Unknown uses of GetResumePointAddr remain!");

  // Delete the declaration of the intrinsic after lowering, as future calls to
  // it are invalid.
  GetResumePointAddr->eraseFromParent();
}

void DXILContPostProcessPassImpl::handleInitialContinuationStackPtr(
    Function &F) {
  auto *InitFun = GpurtLibrary->getFunction("_cont_GetContinuationStackAddr");
  assert(InitFun && "GetContinuationStackAddr not found");
  assert(InitFun->arg_size() == 0 && InitFun->getReturnType()->isIntegerTy(32));
  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    auto *Init = Builder.CreateCall(InitFun);
    CInst.replaceAllUsesWith(Init);
    CrossInliner.inlineCall(*Init);
    Builder.SetInsertPoint(&*Builder.GetInsertPoint());
    CInst.eraseFromParent();
  });
}

void DXILContPostProcessPassImpl::handleLgcRtIntrinsic(Function &F) {
  // Look for known HLSL intrinsics
  llvm::forEachCall(F, [&](CallInst &CInst) {
    auto Data = ToProcess.find(CInst.getFunction());
    if (Data != ToProcess.end()) {
      auto IntrImplEntry = llvm::findIntrImplEntryByIntrinsicCall(&CInst);
      if (IntrImplEntry == std::nullopt)
        return;

      Data->second.IntrinsicCalls.push_back(&CInst);
    }
  });
}

void DXILContPostProcessPassImpl::handleRegisterBufferSetPointerBarrier(
    Function &F, GlobalVariable *Payload) {
  // Remove setpointerbarrier instructions related to payload
  llvm::forEachCall(F, [&](CallInst &CInst) {
    if (isCastGlobal(Payload, CInst.getOperand(0)))
      CInst.eraseFromParent();
  });
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
      auto *StackOffsetTy = getContinuationStackOffsetType(F.getContext());
      auto *CastPayload = Builder.CreateBitOrPointerCast(
          Payload, StackOffsetTy->getPointerTo(Payload->getAddressSpace()));
      auto *Offset = Builder.CreateLoad(StackOffsetTy, CastPayload);
      auto *Ptr = continuationStackOffsetToPtr(Builder, Offset, *GpurtLibrary,
                                               CrossInliner);
      Ptr = Builder.CreateBitCast(Ptr, CInst.getType());
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
  llvm::forEachCall(F, [&](CallInst &CInst) {
    CInst.replaceAllUsesWith(Size);
    CInst.eraseFromParent();
  });
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

void DXILContPostProcessPassImpl::handleContPayloadRegisterI32Count(
    Function &F) {
  assert(F.arg_size() == 0
         // register count
         && F.getFunctionType()->getReturnType()->isIntegerTy(32));

  auto *RegCount =
      ConstantInt::get(IntegerType::get(F.getContext(), 32),
                       Registers->getValueType()->getArrayNumElements());
  llvm::forEachCall(F, [&](CallInst &CInst) {
    CInst.replaceAllUsesWith(RegCount);
    CInst.eraseFromParent();
  });
}

void DXILContPostProcessPassImpl::handleContPayloadRegistersGetI32(
    Function &F) {
  assert(F.getReturnType()->isIntegerTy(32) &&
         F.arg_size() == 1
         // index
         && F.getFunctionType()->getParamType(0)->isIntegerTy(32));

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    auto *Addr =
        Builder.CreateGEP(Registers->getValueType(), Registers,
                          {Builder.getInt32(0), CInst.getArgOperand(0)});
    auto *Load = Builder.CreateLoad(Builder.getInt32Ty(), Addr);
    CInst.replaceAllUsesWith(Load);
    CInst.eraseFromParent();
  });
}

void DXILContPostProcessPassImpl::handleContPayloadRegistersSetI32(
    Function &F) {
  assert(F.getReturnType()->isVoidTy() &&
         F.arg_size() == 2
         // index
         && F.getFunctionType()->getParamType(0)->isIntegerTy(32)
         // value
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    auto *Addr =
        Builder.CreateGEP(Registers->getValueType(), Registers,
                          {Builder.getInt32(0), CInst.getArgOperand(0)});
    Builder.CreateStore(CInst.getOperand(1), Addr);
    CInst.eraseFromParent();
  });
}

void DXILContPostProcessPassImpl::handleContStackAlloc(
    FunctionAnalysisManager &FAM, Function &F) {
  assert(F.getReturnType()->isIntegerTy(32) &&
         F.arg_size() == 2
         // csp
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // size
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  llvm::forEachCall(F, [&](CallInst &CInst) {
    Builder.SetInsertPoint(&CInst);
    auto *Func = CInst.getFunction();
    Value *SizeArg = CInst.getArgOperand(1);
    uint32_t Size;

    if (auto *I = dyn_cast<Instruction>(SizeArg)) {
      // Do some basic constant-propagation
      // This is needed because this pass just replaced the ValueI32Count
      // and ContPayloadRegistersI32Count intrinsics and the allocated size
      // usually depends on these values.
      auto &DT = FAM.getResult<DominatorTreeAnalysis>(*Func);
      auto &TLI = FAM.getResult<TargetLibraryAnalysis>(*Func);
      auto &AC = FAM.getResult<AssumptionAnalysis>(*Func);
      const SimplifyQuery SQ(Func->getParent()->getDataLayout(), &TLI, &DT,
                             &AC);

      if (auto *NewSize = simplifyInstruction(I, SQ))
        SizeArg = NewSize;
    }

    if (auto *C = dyn_cast<ConstantInt>(SizeArg))
      Size = C->getZExtValue();
    else
      report_fatal_error("ContStackAlloc must be called with a constant "
                         "that can be computed at compile time");

    auto *OrigVal =
        Builder.CreateLoad(Builder.getInt32Ty(), CInst.getArgOperand(0));

    auto *NewVal = Builder.CreateAdd(OrigVal, Builder.getInt32(Size));
    Builder.CreateStore(NewVal, CInst.getArgOperand(0));
    CInst.replaceAllUsesWith(OrigVal);
    CInst.eraseFromParent();

    // Add allocation to the stack size of this function
    ContHelper::addStackSize(Func, Size);
  });
}

void DXILContPostProcessPassImpl::collectProcessableFunctions() {
  for (Function &F : *Mod) {
    if (F.isDeclaration())
      continue;

    auto Stage = lgc::rt::getLgcRtShaderStage(&F);
    if (!Stage)
      continue;

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
        ShaderStageHelper::shaderStageToDxilShaderKind(*Stage);
    switch (Kind) {
    case DXILShaderKind::RayGeneration: {
      FunctionData Data;
      Data.Kind = Kind;
      Data.SystemDataArgumentIndex = SystemDataArgumentIndexRayGen;
      Data.SystemDataTy =
          F.getFunctionType()->getParamType(SystemDataArgumentIndexRayGen);
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
      Data.SystemDataArgumentIndex = SystemDataArgumentIndexStart;
      Data.SystemDataTy =
          F.getFunctionType()->getParamType(SystemDataArgumentIndexStart);
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
        Data.SystemDataArgumentIndex = SystemDataArgumentIndexContinuation;
        Data.SystemDataTy =
            F.getArg(SystemDataArgumentIndexContinuation)->getType();
        [[maybe_unused]] bool DidInsert =
            ToProcess.insert({&F, std::move(Data)}).second;
        assert(DidInsert);
      }
    }
  }
}

bool DXILContPostProcessPassImpl::handleIntrinsicCalls() {
  bool Changed = false;
  auto *Payload = Mod->getGlobalVariable(ContHelper::GlobalPayloadName);

  // TODO: Dialectify.
  for (auto &F : Mod->functions()) {
    auto Name = F.getName();
    if (Name == "continuation.initialContinuationStackPtr") {
      Changed = true;
      handleInitialContinuationStackPtr(F);
    } else if (Name.starts_with("lgc.rt")) {
      Changed = true;
      handleLgcRtIntrinsic(F);
    } else if (Name.starts_with("registerbuffer.setpointerbarrier")) {
      Changed = true;
      handleRegisterBufferSetPointerBarrier(F, Payload);
    } else if (Name.starts_with("registerbuffer.getpointer")) {
      Changed = true;
      handleRegisterBufferGetPointer(F, Payload);
    }
  }

  return Changed;
}

bool DXILContPostProcessPassImpl::replaceIntrinsicCalls(
    Function &F, const FunctionData &Data) {
  if (Data.IntrinsicCalls.empty())
    return false;

  auto *FuncTy = F.getFunctionType();

  assert(FuncTy->getNumParams() > Data.SystemDataArgumentIndex &&
         "Missing system data argument");
  Builder.SetInsertPointPastAllocas(&F);

  // Intrinsics need a pointer, so allocate and store the system data argument
  Argument *SystemDataArgument = F.getArg(Data.SystemDataArgumentIndex);
  Value *SystemDataPtr = Builder.CreateAlloca(Data.SystemDataTy);
  SystemDataPtr->setName("system.data.alloca");
  Builder.CreateStore(SystemDataArgument, SystemDataPtr);

  for (auto *Call : Data.IntrinsicCalls)
    replaceIntrinsicCall(Builder, Data.SystemDataTy, SystemDataPtr, Data.Kind,
                         Call, GpurtLibrary, CrossInliner);

  return true;
}

std::pair<bool, Function *>
DXILContPostProcessPassImpl::insertSetupRayGen(Function &F,
                                               const FunctionData &Data) {
  // The start part of the RayGen shader is the only occurrence where we need to
  // call SetupRayGen
  if (Data.Kind != DXILShaderKind::RayGeneration || !Data.IsStart)
    return {false, &F};

  auto *FuncTy = F.getFunctionType();
  assert(FuncTy->getNumParams() > Data.SystemDataArgumentIndex &&
         "Missing system data argument");

  Argument *const SystemDataArgument = F.getArg(Data.SystemDataArgumentIndex);

  // Replace usages of the system data argument with the result of SetupRayGen
  Builder.SetInsertPointPastAllocas(&F);

  auto *SystemDataInit = Builder.CreateCall(SetupRayGen);
  assert(SystemDataInit->getType() == Data.SystemDataTy &&
         "SetupRayGen return type does not match system data type");
  SystemDataInit->setName("system.data");
  SystemDataArgument->replaceAllUsesWith(SystemDataInit);
  CrossInliner.inlineCall(*SystemDataInit);

  // Change function signature to remove the system data argument
  SmallVector<Type *> ArgTypes;
  ArgTypes.append(FuncTy->param_begin(),
                  FuncTy->param_begin() + Data.SystemDataArgumentIndex);
  ArgTypes.append(FuncTy->param_begin() + (Data.SystemDataArgumentIndex + 1),
                  FuncTy->param_end());
  auto *NewFuncTy = FunctionType::get(FuncTy->getReturnType(), ArgTypes, false);

  Function *NewFunc = CompilerUtils::cloneFunctionHeader(
      F, NewFuncTy, ArrayRef<AttributeSet>{});
  NewFunc->takeName(&F);

  llvm::moveFunctionBody(F, *NewFunc);

  F.replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F.getType()));
  F.eraseFromParent();

  return {true, NewFunc};
}

bool DXILContPostProcessPassImpl::replaceIntrinsicCallsAndSetupRayGen() {
  bool Changed = false;

  // We will change some function signatures and populate a new MapVector as we
  // go, to then replace ToProcess
  MapVector<Function *, FunctionData> ToProcessNew;
  ToProcessNew.reserve(ToProcess.size());

  for (auto &[Func, Data] : ToProcess) {
    Changed |= replaceIntrinsicCalls(*Func, Data);

    auto const [DidInsert, NewFunc] = insertSetupRayGen(*Func, Data);
    Changed |= DidInsert;

    // Func could have been changed, but Data is the same
    ToProcessNew.insert({NewFunc, std::move(Data)});
  }

  ToProcess = std::move(ToProcessNew);
  return Changed;
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

bool DXILContPostProcessPassImpl::handleAmdInternals(
    llvm::ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;
  SmallVector<Function *> ContStackAllocs;

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
    } else if (Name.starts_with("_AmdContPayloadRegistersI32Count")) {
      Changed = true;
      handleContPayloadRegisterI32Count(F);
    } else if (Name.starts_with("_AmdContPayloadRegistersGetI32")) {
      Changed = true;
      handleContPayloadRegistersGetI32(F);
    } else if (Name.starts_with("_AmdContPayloadRegistersSetI32")) {
      Changed = true;
      handleContPayloadRegistersSetI32(F);
    } else if (Name.starts_with("_AmdContStackAlloc")) {
      Changed = true;
      ContStackAllocs.push_back(&F);
    }
  }

  if (!ContStackAllocs.empty()) {
    auto &FAM =
        AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(*Mod)
            .getManager();
    for (auto *F : ContStackAllocs)
      handleContStackAlloc(FAM, *F);
  }

  return Changed;
}

DXILContPostProcessPassImpl::DXILContPostProcessPassImpl(Module &M,
                                                         Module &GpurtLibrary)
    : Mod{&M}, GpurtLibrary{&GpurtLibrary},
      SetupRayGen{GpurtLibrary.getFunction("_cont_SetupRayGen")},
      Builder{Mod->getContext()} {}

bool DXILContPostProcessPassImpl::run(
    llvm::ModuleAnalysisManager &AnalysisManager) {
  bool Changed = false;

  collectProcessableFunctions();

  Changed |= handleIntrinsicCalls();
  Changed |= replaceIntrinsicCallsAndSetupRayGen();
  for (auto &F : make_early_inc_range(*Mod)) {
    if (F.getName().starts_with("_AmdGetResumePointAddr")) {
      Changed = true;
      lowerGetResumePointAddr(F);
    }
  }
  Changed |= unfoldGlobals();
  Changed |= handleAmdInternals(AnalysisManager);

  Changed |= fixupDxilMetadata(*Mod);

  // Change function pointer accesses to include metadata
  Changed |= addGetAddrAndMDIntrinsicCalls(*Mod);

#ifndef NDEBUG
  checkContinuationsModule(*Mod);
#endif

  if (ReportContStateSizes || ReportAllSizes)
    reportContStateSizes(*Mod);

  if (ReportPayloadRegisterSizes || ReportAllSizes)
    reportPayloadSizes(*Mod);

  if (ReportSystemDataSizes || ReportAllSizes)
    reportSystemDataSizes(*Mod, ToProcess);

  Changed |= llvm::removeUnusedFunctionDecls(Mod, false);

  return Changed;
}
} // anonymous namespace

llvm::PreservedAnalyses
DXILContPostProcessPass::run(llvm::Module &Module,
                             llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass dxil-cont-post-process\n");
  AnalysisManager.getResult<DialectContextAnalysis>(Module);

  DXILContPostProcessPassImpl Impl{Module,
                                   GpurtLibrary ? *GpurtLibrary : Module};
  bool Changed = Impl.run(AnalysisManager);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
