/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "continuations/Continuations.h"
#include "continuations/ContinuationsDialect.h"
#include "continuations/ContinuationsUtil.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
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

// Collects all calls to the given function, and appends them to CallInsts.
static void collectFunctionCalls(const Function &Func,
                                 SmallVectorImpl<CallInst *> &CallInsts) {
  for (const auto &Use : Func.uses()) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        CallInsts.push_back(CInst);
      }
    }
  }
}

// Collects all calls to continuation.[wait]continue
static void collectContinueCalls(const Module &M,
                                 SmallVectorImpl<CallInst *> &CallInsts) {
  for (const auto &Name :
       {"continuation.continue", "continuation.waitContinue"}) {
    auto *Func = M.getFunction(Name);
    if (!Func)
      continue;
    collectFunctionCalls(*Func, CallInsts);
  }
}

static void
reportContStateSizes(Module &M,
                     const MapVector<Function *, DXILShaderKind> ShaderKinds) {
  // Determine the set of entry functions which have a continuation function
  // We cannot rely on the state size for this, because functions without a
  // continuation (e.g. a non-recursive CHS) have a state size of 0 in metadata.
  DenseSet<Function *> EntriesWithContinuationFunctions;
  for (auto &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    if (auto *MD = dyn_cast_or_null<MDTuple>(
            F.getMetadata(DXILContHelper::MDContinuationName))) {
      auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
      if (EntryF != &F)
        EntriesWithContinuationFunctions.insert(EntryF);
    }
  }
  for (auto [F, ShaderKind] : ShaderKinds) {
    if (!EntriesWithContinuationFunctions.contains(F))
      continue;
    auto OptStateSize = DXILContHelper::tryGetContinuationStateByteCount(*F);
    if (!OptStateSize.has_value())
      continue;

    dbgs() << "Continuation state size of \"" << F->getName() << "\" ("
           << ShaderKind << "): " << OptStateSize.value() << " bytes\n";
  }
}

// For every function with incoming or outgoing (or both) payload registers,
// report the incoming size and the max outgoing size in bytes.
static void
reportPayloadSizes(Module &M,
                   const MapVector<Function *, DXILShaderKind> &ShaderKinds) {

  // For every function with continue calls, determine the max number of
  // outgoing registers
  DenseMap<Function *, unsigned> MaxOutgoingRegisterCounts;
  SmallVector<CallInst *> ContinueCalls;
  collectContinueCalls(M, ContinueCalls);

  for (auto *CallInst : ContinueCalls) {
    auto RegCount =
        DXILContHelper::tryGetOutgoingRegisterCount(CallInst).value();
    MaxOutgoingRegisterCounts[CallInst->getFunction()] =
        std::max(MaxOutgoingRegisterCounts[CallInst->getFunction()], RegCount);
  }
  for (auto [F, ShaderKind] : ShaderKinds) {
    auto OptIncomingPayloadRegisterCount =
        DXILContHelper::tryGetIncomingRegisterCount(F);
    bool HasIncomingPayload = OptIncomingPayloadRegisterCount.has_value();
    auto It = MaxOutgoingRegisterCounts.find(F);
    bool HasOutgoingPayload = (It != MaxOutgoingRegisterCounts.end());

    if (!HasIncomingPayload && !HasOutgoingPayload)
      continue;

    dbgs() << "Incoming and max outgoing payload VGPR size of \""
           << F->getName() << "\" (" << ShaderKind << "): ";
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
    const MapVector<Function *, DXILContPostProcessPass::FunctionData>
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

DXILContPostProcessPass::DXILContPostProcessPass() {}

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
      assert((isa<BitCastOperator>(CE) || isa<PtrToIntOperator>(CE)) &&
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
        bool Found = false;
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
static void checkContinuationsModule(const Module &M) {
  // Check that all continuation.continue calls have registercount metadata.
  SmallVector<CallInst *> CallInsts;
  collectContinueCalls(M, CallInsts);
  for (auto *CallInst : CallInsts) {
    if (!DXILContHelper::tryGetOutgoingRegisterCount(CallInst))
      report_fatal_error("Missing registercount metadata on continue call!");
  }

  // Check that every function has at most one setLocalRootIndex call.
  if (const auto *SetF = M.getFunction("amd.dx.setLocalRootIndex")) {
    SmallDenseSet<Function *> HasSetF;

    for (const auto &Use : SetF->uses()) {
      if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
        if (CInst->isCallee(&Use)) {
          // Returns true if it is a new value
          auto Inserted = HasSetF.insert(CInst->getFunction());
          if (!Inserted.second)
            report_fatal_error(
                "Found a function with more than one setLocalRootIndex");
        }
      }
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

  // TODO Can remove i64 handling
  if (G->getValueType()->isIntegerTy(64)) {
    auto *I32 = Type::getInt32Ty(G->getContext());
    auto *I32Ptr = ConstantExpr::getBitCast(
        Gep, I32->getPointerTo(Registers->getAddressSpace()));
    // Special case the return address: Convert i64 loads and stores to i32
    // ones for the translator
    for (auto *U : make_early_inc_range(G->users())) {
      if (auto *Load = dyn_cast<LoadInst>(U)) {
        if (Load->getPointerOperand() != G)
          continue;
        IRBuilder<> B(Load);
        auto *Part0 = B.CreateLoad(I32, I32Ptr);
        auto *Part1 = B.CreateLoad(I32, ConstantExpr::getInBoundsGetElementPtr(
                                            I32, I32Ptr, B.getInt64(1)));
        auto *Vec = B.CreateInsertElement(FixedVectorType::get(I32, 2), Part0,
                                          static_cast<uint64_t>(0));
        Vec = B.CreateInsertElement(Vec, Part1, 1);
        auto *Loaded = B.CreateBitCast(Vec, I64);
        Load->replaceAllUsesWith(Loaded);
        Load->eraseFromParent();
      } else if (auto *Store = dyn_cast<StoreInst>(U)) {
        if (Store->getPointerOperand() != G)
          continue;
        IRBuilder<> B(Store);
        auto *Vec = B.CreateBitCast(Store->getValueOperand(),
                                    FixedVectorType::get(I32, 2));
        auto *Part0 = B.CreateExtractElement(Vec, static_cast<uint64_t>(0));
        auto *Part1 = B.CreateExtractElement(Vec, 1);
        B.CreateStore(Part0, I32Ptr);
        B.CreateStore(Part1, ConstantExpr::getInBoundsGetElementPtr(
                                 I32, I32Ptr, B.getInt64(1)));
        Store->eraseFromParent();
      }
    }
  }

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

bool DXILContPostProcessPass::lowerGetResumePointAddr(
    llvm::Module &M, llvm::IRBuilder<> &B,
    const MapVector<Function *, FunctionData> &ToProcess) {
  auto *GetResumePointAddr = M.getFunction("_AmdGetResumePointAddr");

  if (!GetResumePointAddr)
    return false;

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
    assert((ReturnAddr->getType() == B.getInt64Ty()) &&
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

    B.SetInsertPoint(ContinueCall);
    auto *NewCall = B.CreateCall(ContinueCall->getCalledFunction(), Args);
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

  return true;
}

bool DXILContPostProcessPass::lowerGetCurrentFuncAddr(llvm::Module &M,
                                                      llvm::IRBuilder<> &B) {
  auto *GetCurrentFuncAddr = M.getFunction("_AmdGetCurrentFuncAddr");

  if (!GetCurrentFuncAddr)
    return false;

  assert(GetCurrentFuncAddr->getReturnType()->isIntegerTy(64) &&
         GetCurrentFuncAddr->arg_size() == 0);

  for (auto &Use : make_early_inc_range(GetCurrentFuncAddr->uses())) {
    auto *CInst = dyn_cast<CallInst>(Use.getUser());

    if (!CInst || !CInst->isCallee(&Use)) {
      // Non-call use. This will likely result in a remaining non-lowered use
      // reported as error at the end of this function.
      continue;
    }

    auto *FuncPtrToInt = B.CreatePtrToInt(CInst->getFunction(),
                                          GetCurrentFuncAddr->getReturnType());
    CInst->replaceAllUsesWith(FuncPtrToInt);
    CInst->eraseFromParent();
  }

  if (!GetCurrentFuncAddr->use_empty())
    report_fatal_error("Unknown uses of GetCurrentFuncAddr remain!");

  // Delete the declaration of the intrinsic after lowering, as future calls to
  // it are invalid.
  GetCurrentFuncAddr->eraseFromParent();

  return true;
}

void DXILContPostProcessPass::handleInitialContinuationStackPtr(IRBuilder<> &B,
                                                                Function &F) {
  auto *InitFun = Mod->getFunction("_cont_GetContinuationStackAddr");
  assert(InitFun && "GetContinuationStackAddr not found");
  assert(InitFun->arg_size() == 0 && InitFun->getReturnType()->isIntegerTy(32));
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        B.SetInsertPoint(CInst);
        auto *Init = B.CreateCall(InitFun);
        CInst->replaceAllUsesWith(Init);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleLgcRtIntrinsic(Function &F) {
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

void DXILContPostProcessPass::handleRegisterBufferSetPointerBarrier(
    Function &F, GlobalVariable *Payload) {
  // Remove setpointerbarrier instructions related to payload
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        if (!isCastGlobal(Payload, CInst->getOperand(0)))
          continue;
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleRegisterBufferGetPointer(
    IRBuilder<> &B, Function &F, GlobalVariable *Payload) {
  // Check calls that take the payload as argument
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        if (!isCastGlobal(Payload, CInst->getOperand(0)))
          continue;

        // Replace call with first part of payload
        static_assert(FirstPayloadMemoryPointerRegister == 0,
                      "Need to adjust offset here");
        B.SetInsertPoint(CInst);
        auto *StackOffsetTy = getContinuationStackOffsetType(F.getContext());
        auto *CastPayload = B.CreateBitOrPointerCast(
            Payload, StackOffsetTy->getPointerTo(Payload->getAddressSpace()));
        auto *Offset = B.CreateLoad(StackOffsetTy, CastPayload);
        auto *Ptr = continuationStackOffsetToPtr(B, Offset);
        Ptr = B.CreateBitCast(Ptr, CInst->getType());
        CInst->replaceAllUsesWith(Ptr);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleValueI32Count(IRBuilder<> &B, Function &F) {
  assert(F.arg_size() == 1
         // i32 count
         && F.getFunctionType()->getReturnType()->isIntegerTy(32)
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy());

  auto *Ty = getFuncArgPtrElementType(&F, 0);
  auto *Size =
      B.getInt32(Mod->getDataLayout().getTypeStoreSize(Ty).getFixedValue() / 4);
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        CInst->replaceAllUsesWith(Size);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleValueGetI32(IRBuilder<> &B, Function &F) {
  assert(F.arg_size() == 2
         // value
         && F.getFunctionType()->getReturnType()->isIntegerTy(32)
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // index
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  auto *I32 = B.getInt32Ty();
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        B.SetInsertPoint(CInst);
        Value *Addr =
            B.CreateBitCast(CInst->getArgOperand(0), I32->getPointerTo());
        Addr = B.CreateGEP(I32, Addr, CInst->getArgOperand(1));
        auto *Load = B.CreateLoad(I32, Addr);
        CInst->replaceAllUsesWith(Load);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleValueSetI32(IRBuilder<> &B, Function &F) {
  assert(F.arg_size() == 3 &&
         F.getFunctionType()->getReturnType()->isVoidTy()
         // Pointer to a struct
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // index
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32)
         // value
         && F.getFunctionType()->getParamType(2)->isIntegerTy(32));

  auto *I32 = B.getInt32Ty();
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        B.SetInsertPoint(CInst);
        Value *Addr =
            B.CreateBitCast(CInst->getArgOperand(0), I32->getPointerTo());
        Addr = B.CreateGEP(I32, Addr, CInst->getArgOperand(1));
        B.CreateStore(CInst->getArgOperand(2), Addr);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleContPayloadRegisterI32Count(Function &F) {
  assert(F.arg_size() == 0
         // register count
         && F.getFunctionType()->getReturnType()->isIntegerTy(32));

  auto *RegCount =
      ConstantInt::get(IntegerType::get(F.getContext(), 32),
                       Registers->getValueType()->getArrayNumElements());
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        CInst->replaceAllUsesWith(RegCount);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleContPayloadRegistersGetI32(IRBuilder<> &B,
                                                               Function &F) {
  assert(F.getReturnType()->isIntegerTy(32) &&
         F.arg_size() == 1
         // index
         && F.getFunctionType()->getParamType(0)->isIntegerTy(32));

  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        B.SetInsertPoint(CInst);
        auto *Addr = B.CreateGEP(Registers->getValueType(), Registers,
                                 {B.getInt32(0), CInst->getArgOperand(0)});
        auto *Load = B.CreateLoad(B.getInt32Ty(), Addr);
        CInst->replaceAllUsesWith(Load);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleContPayloadRegistersSetI32(IRBuilder<> &B,
                                                               Function &F) {
  assert(F.getReturnType()->isVoidTy() &&
         F.arg_size() == 2
         // index
         && F.getFunctionType()->getParamType(0)->isIntegerTy(32)
         // value
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        B.SetInsertPoint(CInst);
        auto *Addr = B.CreateGEP(Registers->getValueType(), Registers,
                                 {B.getInt32(0), CInst->getArgOperand(0)});
        B.CreateStore(CInst->getOperand(1), Addr);
        CInst->eraseFromParent();
      }
    }
  }
}

void DXILContPostProcessPass::handleContStackAlloc(FunctionAnalysisManager &FAM,
                                                   IRBuilder<> &B,
                                                   Function &F) {
  assert(F.getReturnType()->isIntegerTy(32) &&
         F.arg_size() == 2
         // csp
         && F.getFunctionType()->getParamType(0)->isPointerTy()
         // size
         && F.getFunctionType()->getParamType(1)->isIntegerTy(32));

  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        B.SetInsertPoint(CInst);
        auto *Func = CInst->getFunction();
        Value *SizeArg = CInst->getArgOperand(1);
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

        auto *OrigVal = B.CreateLoad(B.getInt32Ty(), CInst->getArgOperand(0));

        auto *NewVal = B.CreateAdd(OrigVal, B.getInt32(Size));
        B.CreateStore(NewVal, CInst->getArgOperand(0));
        CInst->replaceAllUsesWith(OrigVal);
        CInst->eraseFromParent();

        // Add allocation to the stack size of this function
        uint64_t CurStackSize = 0;
        if (auto *StackSizeMD =
                Func->getMetadata(DXILContHelper::MDStackSizeName))
          CurStackSize =
              mdconst::extract<ConstantInt>(StackSizeMD->getOperand(0))
                  ->getZExtValue();
        Func->setMetadata(
            DXILContHelper::MDStackSizeName,
            MDTuple::get(Func->getContext(),
                         {ConstantAsMetadata::get(ConstantInt::get(
                             B.getInt32Ty(), Size + CurStackSize))}));
      }
    }
  }
}

llvm::PreservedAnalyses
DXILContPostProcessPass::run(llvm::Module &M,
                             llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the dxil-cont-post-process pass\n");
  AnalysisManager.getResult<DialectContextAnalysis>(M);

  Mod = &M;
  bool Changed = false;
  ToProcess.clear();
  MapVector<Function *, DXILShaderKind> ShaderKinds;
  analyzeShaderKinds(M, ShaderKinds);
  auto *SetupRayGen =
      DXILContHelper::getAliasedFunction(M, "_cont_SetupRayGen");
  for (auto &Entry : ShaderKinds) {
    switch (Entry.second) {
    case DXILShaderKind::RayGeneration:
    case DXILShaderKind::Intersection:
    case DXILShaderKind::AnyHit:
    case DXILShaderKind::ClosestHit:
    case DXILShaderKind::Miss:
    case DXILShaderKind::Callable: {
      Changed = true;
      FunctionData Data;
      Data.Kind = Entry.second;
      if (Data.Kind == DXILShaderKind::RayGeneration) {
        assert(SetupRayGen && "Could not find SetupRayGen function");
        Data.SystemDataTy = SetupRayGen->getReturnType();
      } else {
        Data.SystemDataTy = Entry.first->getFunctionType()->getParamType(2);
      }
      ToProcess[Entry.first] = Data;
      break;
    }
    default:
      break;
    }
  }

  // Also find continuation parts of the functions
  for (auto &F : M.functions()) {
    if (F.isDeclaration())
      continue;
    if (auto *MD = dyn_cast_or_null<MDTuple>(
            F.getMetadata(DXILContHelper::MDContinuationName))) {
      auto *EntryF = extractFunctionOrNull(MD->getOperand(0));
      auto Entry = ShaderKinds.find(EntryF);
      if (Entry != ShaderKinds.end() && &F != Entry->first) {
        FunctionData Data = ToProcess[Entry->first];
        Data.IsStart = false;
        Data.SystemDataTy = F.getArg(1)->getType();
        ToProcess[&F] = Data;
      }
    }
  }

  IRBuilder<> B(M.getContext());
  auto *Payload = M.getGlobalVariable(DXILContHelper::GlobalPayloadName);
  for (auto &F : M.functions()) {
    auto Name = F.getName();
    if (Name == "continuation.initialContinuationStackPtr") {
      Changed = true;
      handleInitialContinuationStackPtr(B, F);
    } else if (Name.startswith("lgc.rt")) {
      Changed = true;
      handleLgcRtIntrinsic(F);
    } else if (Name.startswith("registerbuffer.setpointerbarrier")) {
      Changed = true;
      handleRegisterBufferSetPointerBarrier(F, Payload);
    } else if (Name.startswith("registerbuffer.getpointer")) {
      Changed = true;
      handleRegisterBufferGetPointer(B, F, Payload);
    }
  }

  const static auto Visitor =
      llvm_dialects::VisitorBuilder<MapVector<Function *, FunctionData>>()
          .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
          .add<continuations::GetSystemDataOp>([](auto &ToProcess, auto &Op) {
            // See also the system data documentation at the top of
            // Continuations.h.
            auto Data = ToProcess.find(Op.getFunction());
            if (Data != ToProcess.end())
              Data->second.GetSystemDataCalls.push_back(&Op);
          })
          .build();

  Visitor.visit(ToProcess, M);

  for (auto &FuncData : ToProcess) {
    auto &Data = FuncData.second;
    // Transform SystemData to alloca and load on every use
    B.SetInsertPoint(FuncData.first->getEntryBlock().getFirstNonPHI());
    Data.SystemData = B.CreateAlloca(Data.SystemDataTy);

    // Replace intrinsic calls
    for (auto *Call : Data.IntrinsicCalls)
      replaceIntrinsicCall(B, Data.SystemDataTy, Data.SystemData, Data.Kind,
                           Call);

    // Replace calls to getSystemData
    for (auto *Call : Data.GetSystemDataCalls) {
      B.SetInsertPoint(Call);
      auto *SystemDataTy = Call->getFunctionType()->getReturnType();
      auto *SystemDataPtr = getDXILSystemData(B, Data.SystemData,
                                              Data.SystemDataTy, SystemDataTy);
      auto *SystemData = B.CreateLoad(SystemDataTy, SystemDataPtr);
      Call->replaceAllUsesWith(SystemData);
      Call->eraseFromParent();
    }

    B.SetInsertPoint(
        &*FuncData.first->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    if (FuncData.first->hasMetadata(DXILContHelper::MDEntryName)) {
      // Initialize system data for the start part of the entry shader
      auto *TmpSystemData = B.CreateCall(SetupRayGen);
      B.CreateStore(TmpSystemData, Data.SystemData);
    } else {
      // Initialize the new system data alloca with the passed argument.
      B.CreateStore(FuncData.first->getArg(Data.IsStart ? 2 : 1),
                    Data.SystemData);
    }

    Data.SystemData->setName("system.data");
  }

  Changed |= lowerGetResumePointAddr(M, B, ToProcess);
  Changed |= lowerGetCurrentFuncAddr(M, B);

  // Replace register globals with indices into a bigger global
  const auto &DL = M.getDataLayout();
  GlobalVariable *PayloadGlobal =
      M.getGlobalVariable(DXILContHelper::GlobalPayloadName);
  GlobalVariable *ContStateGlobal =
      M.getGlobalVariable(DXILContHelper::GlobalContStateName);

  if (PayloadGlobal || ContStateGlobal) {
    Changed = true;

    // We use the maximum size for the continuation state and the actual size
    // for the payload, so that the offset of the payload stays the same, but
    // the global is only as big as necessary.
    uint32_t RequiredSize =
        (PayloadGlobal->getValueType()->getArrayNumElements() +
         ContinuationStateRegisterCount) *
        RegisterBytes;

    // Put continuation state first, it's callee save so we need to have it
    // full in all cases. Payload can be truncated, so the backend is free to
    // use registers that are unused in a function

    auto *I32 = Type::getInt32Ty(M.getContext());
    auto *RegistersTy = ArrayType::get(I32, RequiredSize / RegisterBytes);
    Registers = cast<GlobalVariable>(M.getOrInsertGlobal(
        DXILContHelper::GlobalRegistersName, RegistersTy, [&] {
          return new GlobalVariable(
              M, RegistersTy, false, GlobalVariable::ExternalLinkage, nullptr,
              DXILContHelper::GlobalRegistersName, nullptr,
              GlobalVariable::NotThreadLocal, GlobalRegisterAddrspace);
        }));

    if (ContStateGlobal)
      replaceGlobal(DL, Registers, ContStateGlobal, 0);
    if (PayloadGlobal)
      replaceGlobal(DL, Registers, PayloadGlobal,
                    ContinuationStateRegisterCount * RegisterBytes);
  }

  Function *ContStackAlloc = nullptr;

  for (auto &F : M.functions()) {
    auto Name = F.getName();
    if (Name.startswith("_AmdValueI32Count")) {
      Changed = true;
      handleValueI32Count(B, F);
    } else if (Name.startswith("_AmdValueGetI32")) {
      Changed = true;
      handleValueGetI32(B, F);
    } else if (Name.startswith("_AmdValueSetI32")) {
      Changed = true;
      handleValueSetI32(B, F);
    } else if (Name == "_AmdContPayloadRegistersI32Count") {
      Changed = true;
      handleContPayloadRegisterI32Count(F);
    } else if (Name == "_AmdContPayloadRegistersGetI32") {
      Changed = true;
      handleContPayloadRegistersGetI32(B, F);
    } else if (Name == "_AmdContPayloadRegistersSetI32") {
      Changed = true;
      handleContPayloadRegistersSetI32(B, F);
    } else if (Name == "_AmdContStackAlloc") {
      Changed = true;
      ContStackAlloc = &F;
    }
  }

  if (ContStackAlloc) {
    auto &FAM = AnalysisManager.getResult<FunctionAnalysisManagerModuleProxy>(M)
                    .getManager();
    handleContStackAlloc(FAM, B, *ContStackAlloc);
  }

  Changed |= fixupDxilMetadata(M);

  // Change function pointer accesses to include metadata
  Changed |= addGetAddrAndMDIntrinsicCalls(M);

#ifndef NDEBUG
  checkContinuationsModule(M);
#endif

  if (ReportContStateSizes || ReportAllSizes)
    reportContStateSizes(M, ShaderKinds);
  if (ReportPayloadRegisterSizes || ReportAllSizes)
    reportPayloadSizes(M, ShaderKinds);
  if (ReportSystemDataSizes || ReportAllSizes)
    reportSystemDataSizes(M, ToProcess);

  Changed |= llvm::removeUnusedFunctionDecls(&M, false);

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
