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

//===- DXILContIntrinsicPrepare.cpp - Change signature of functions -------===//
//
// A pass that prepares driver implemented functions for later use.
//
// This pass unmangles function names and changes sret arguments back to
// return values.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include <array>
#include <cassert>
#include <cctype>

using namespace llvm;

#define DEBUG_TYPE "dxil-cont-intrinsic-prepare"

DXILContIntrinsicPreparePass::DXILContIntrinsicPreparePass() {}

/// - Unmangle the function names to be more readable and to prevent confusion
/// with app defined functions later.
/// - Convert sret arguments back to return values
/// - Convert struct pointer arguments to pass structs by value
static Function *transformFunction(Function &F) {
  auto Name = F.getName();
  LLVM_DEBUG(dbgs() << "Transforming function " << Name << "\n");
  std::string NewName = Name.str();
  // Unmangle declarations because they cannot be renamed in the dx api
  if (Name.contains('@')) {
    // Extract unmangled name
    auto Start = Name.find('?') + 1;
    auto End = Name.find('@', Start);
    if (Start == 0 || End == StringRef::npos || Start > Name.size() ||
        End > Name.size()) {
      cantFail(make_error<StringError>(
          Twine("Failed to unmangle function name: Failed to extract from '") +
              Name + "' (start: " + Twine(Start) + ", end: " + Twine(End) + ")",
          inconvertibleErrorCode()));
    }

    // Copy name, otherwise it will be deleted before it's set
    NewName = Name.substr(Start, End - Start).str();
  }

  LLVM_DEBUG(dbgs() << "  Set new name " << NewName << "\n");

  // Change the return type and arguments
  SmallVector<DXILContArgTy> AllArgTypes;

  Type *NewRetTy = F.getReturnType();

  // Unpack the inner type of @class.matrix types
  bool UnpackMatrixTy = false;

  static const std::array<StringRef, 2> FuncsReturningMatrices = {
      "ObjectToWorld4x3", "WorldToObject4x3"};

  if (NewRetTy->isStructTy() && NewRetTy->getStructNumElements() == 1) {
    StringRef FuncName = F.getName();
    for (auto FuncCandidate : FuncsReturningMatrices) {
      if (FuncName.contains(FuncCandidate)) {
        NewRetTy = NewRetTy->getStructElementType(0);
        UnpackMatrixTy = true;
        break;
      }
    }
  }

  Argument *RetArg = nullptr;
  AttributeList FAttrs = F.getAttributes();
  SmallVector<AttributeSet> ParamAttrs;
  unsigned ArgNo = 0;
  for (auto &Arg : F.args()) {
    DXILContArgTy ArgTy = DXILContArgTy::get(&F, &Arg);
    if (Arg.hasStructRetAttr()) {
      NewRetTy = Arg.getParamStructRetType();
      RetArg = &Arg;
    } else if (Arg.getType()->isPointerTy() &&
               (StringRef(NewName).contains("Await") ||
                StringRef(NewName).contains("Enqueue") ||
                StringRef(NewName).contains("Traversal") ||
                (NewName == "_cont_SetTriangleHitAttributes" &&
                 &Arg != F.getArg(0)))) {
      // Pass argument data as struct instead of as pointer
      Type *ElemType = ArgTy.getPointerElementType();
      assert(ElemType && "unable to resolve pointer type for argument");
      AllArgTypes.emplace_back(ElemType);
      ParamAttrs.push_back({});
    } else {
      AllArgTypes.push_back(ArgTy);
      ParamAttrs.push_back(FAttrs.getParamAttrs(ArgNo));
    }
    ArgNo++;
  }

  // Create new empty function
  DXILContFuncTy NewFuncTy(NewRetTy, AllArgTypes);
  Function *NewFunc = cloneFunctionHeaderWithTypes(F, NewFuncTy, ParamAttrs);
  // Remove old name for the case that the new name is the same
  F.setName("");
  NewFunc->setName(NewName);
  NewFunc->addFnAttr(Attribute::AlwaysInline);
  // Set external linkage, so the functions don't get removed, even if they are
  // never referenced at this point
  NewFunc->setLinkage(GlobalValue::LinkageTypes::ExternalLinkage);

  // Transfer code from old function to new function
  llvm::moveFunctionBody(F, *NewFunc);

  // Do not insert code on function declarations
  std::optional<IRBuilder<>> B;
  if (!NewFunc->empty())
    B.emplace(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());

  if (B && UnpackMatrixTy) {
    // Move values of @class.matrix.x.y into return value of unpacked type
    // Replace the return instruction with a new one, returning the unpacked
    // value
    for (auto &BB : *NewFunc) {
      auto *I = BB.getTerminator();
      if (I->getOpcode() == Instruction::Ret) {
        B->SetInsertPoint(I);
        Value *UnpackedVal = B->CreateExtractValue(I->getOperand(0), {0});
        B->CreateRet(UnpackedVal);
        I->eraseFromParent();
      }
    }
  }

  unsigned RetArgIdx = 0;

  // Set arg names for new function
  for (unsigned Idx = 0, NewIdx = 0;
       Idx != F.getFunctionType()->params().size(); ++Idx, ++NewIdx) {
    Argument *OldArg = F.getArg(Idx);
    if (OldArg == RetArg) {
      // Skip return struct
      --NewIdx;
      RetArgIdx = Idx;
      continue;
    }

    Argument *Arg = NewFunc->getArg(NewIdx);
    Arg->setName(OldArg->getName());
    if (B) {
      if (Arg->getType() != OldArg->getType()) {
        // Replace pointer argument with alloca
        auto *Ty = Arg->getType();
        B->SetInsertPoint(
            &*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
        auto *NewArg = B->CreateAlloca(Ty);
        B->CreateStore(Arg, NewArg);
        B->SetInsertPoint(NewArg);
        OldArg->replaceAllUsesWith(NewArg);
      } else {
        OldArg->replaceAllUsesWith(Arg);
      }
    }

    if (OldArg->hasInRegAttr())
      Arg->addAttr(Attribute::InReg);
    else
      Arg->removeAttr(Attribute::AttrKind::InReg);
  }

  if (RetArg && B) {
    // Replace sret argument with real return value
    B->SetInsertPoint(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    auto *RetAlloca = B->CreateAlloca(NewRetTy);
    RetArg->replaceAllUsesWith(RetAlloca);

    // Replace returns with return value
    for (auto &BB : *NewFunc) {
      auto *I = BB.getTerminator();
      if (I->getOpcode() == Instruction::Ret) {
        B->SetInsertPoint(I);
        auto *RetLoad = B->CreateLoad(NewRetTy, RetAlloca);
        B->CreateRet(RetLoad);
        I->eraseFromParent();
      }
    }
  }

  // Replace all calls
  SmallVector<CallInst *> Uses;
  for (auto &Use : F.uses()) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use))
        Uses.push_back(CInst);
    }
  }

  for (auto *CInst : Uses) {
    if (!B)
      B.emplace(CInst);
    else
      B->SetInsertPoint(CInst);

    SmallVector<Value *> Args;
    Value *RetValue = nullptr;
    for (unsigned Idx = 0; Idx != CInst->arg_size(); ++Idx) {
      auto *Arg = CInst->getArgOperand(Idx);
      auto *ArgTy = NewFunc->getArg(Args.size())->getType();
      if (RetArg && RetArgIdx == Idx) {
        RetValue = Arg;
      } else if (Arg->getType() != ArgTy && Arg->getType()->isPointerTy()) {
        auto *Val = B->CreateLoad(ArgTy, Arg);
        Args.push_back(Val);
      } else {
        Args.push_back(Arg);
      }
    }

    auto *NewCall = B->CreateCall(NewFunc, Args);
    if (RetValue)
      B->CreateStore(NewCall, RetValue);

    if (!CInst->getType()->isVoidTy())
      CInst->replaceAllUsesWith(NewCall);
    CInst->eraseFromParent();
  }

  // Remove the old function
  F.replaceAllUsesWith(ConstantExpr::getBitCast(NewFunc, F.getType()));
  F.eraseFromParent();
  return NewFunc;
}

/// Transform enqueue intrinsics to continuation intrinsics
static void replaceIntrinsic(Function &F, Function *NewFunc) {
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser())) {
      if (CInst->isCallee(&Use)) {
        IRBuilder<> B(CInst);
        SmallVector<Value *> Args(CInst->args());
        bool IsEnqueue = F.getName().contains("Enqueue");
        // Add the current function as return address to the call.
        // Used when Traversal calls AnyHit or Intersection.
        if (IsEnqueue && F.getName().contains("EnqueueCall")) {
          bool HasWaitMask = F.getName().contains("WaitEnqueue");
          auto *RetAddr =
              B.CreatePtrToInt(CInst->getFunction(), B.getInt64Ty());
          Args.insert(Args.begin() + (HasWaitMask ? 3 : 2), RetAddr);
        }

        B.CreateCall(NewFunc, Args);
        CInst->eraseFromParent();
      }
    }
  }
}

static bool isGpuRtFuncName(StringRef Name) {
  for (const auto &Intr : LgcRtGpuRtMap) {
    if (Name.contains(Intr.second.Name))
      return true;
  }

  return false;
}

static bool isUtilFunction(StringRef Name) {
  static const char *UtilNames[] = {
      "AcceptHit",
      "Await",
      "Complete",
      "ContinuationStackIsGlobal",
      "ContStackAlloc",
      "Enqueue", // To detect the mangled name of a declaration
      "GetI32",
      "GetCandidateState",
      "GetCommittedState",
      "GetContinuationStackAddr",
      "GetContinuationStackGlobalMemBase",
      "GetCurrentFuncAddr",
      "GetFuncAddr",
      "GetLocalRootIndex",
      "GetResumePointAddr",
      "GetShaderKind",
      "GetTriangleHitAttributes",
      "GetUninitialized",
      "I32Count",
      "IsEndSearch",
      "ReportHit",
      "RestoreSystemData",
      "SetI32",
      "SetTriangleHitAttributes",
      "SetupRayGen",
      "TraceRay",
      "Traversal",
  };

  for (const char *UtilName : UtilNames) {
    if (Name.contains(UtilName))
      return true;
  }
  return false;
}

llvm::PreservedAnalyses DXILContIntrinsicPreparePass::run(
    llvm::Module &M, llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the dxil-cont-intrinsic-prepare pass\n");

  SmallVector<Function *> Funcs(make_pointer_range(M.functions()));
  for (auto *F : Funcs) {
    auto Name = F->getName();
    bool IsContImpl = Name.contains("_cont_") || Name.contains("amd.dx.");
    bool IsAmdIntr = Name.contains("_Amd");
    if ((IsContImpl && isGpuRtFuncName(Name)) ||
        ((IsContImpl || IsAmdIntr) && isUtilFunction(Name)))
      transformFunction(*F);
  }

  // Recollect functions as they may have been replaced
  for (auto &F : M.functions()) {
    Function *Replacement = nullptr;
    auto Name = F.getName();
    if (Name.contains("WaitEnqueue"))
      Replacement = getContinuationWaitContinue(M);
    else if (Name.contains("Enqueue"))
      Replacement = getContinuationContinue(M);
    else if (Name.contains("Complete"))
      Replacement = getContinuationComplete(M);

    if (Replacement)
      replaceIntrinsic(F, Replacement);
  }

  fixupDxilMetadata(M);

  return PreservedAnalyses::none();
}
