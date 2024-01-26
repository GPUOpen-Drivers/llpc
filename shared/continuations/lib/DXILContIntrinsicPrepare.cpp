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

//===- DXILContIntrinsicPrepare.cpp - Change signature of functions -------===//
//
// A pass that prepares driver implemented functions for later use.
//
// This pass unmangles function names and changes sret arguments back to
// return values.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "continuations/ContinuationsUtil.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
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
      report_fatal_error(
          Twine("Failed to unmangle function name: Failed to extract from '") +
          Name + "' (start: " + Twine(Start) + ", end: " + Twine(End) + ")");
    }

    // Copy name, otherwise it will be deleted before it's set
    NewName = Name.substr(Start, End - Start).str();
  }

  LLVM_DEBUG(dbgs() << "  Set new name " << NewName << "\n");

  // Change the return type and arguments
  SmallVector<ContArgTy> AllArgTypes;

  Type *NewRetTy = F.getReturnType();

  // Unpack the inner type of @class.matrix types
  bool UnpackMatrixTy = false;

  if (NewRetTy->isStructTy() && NewRetTy->getStructNumElements() == 1) {
    if (Name.contains("ObjectToWorld4x3") ||
        Name.contains("WorldToObject4x3")) {
      NewRetTy = NewRetTy->getStructElementType(0);
      UnpackMatrixTy = true;
    }
  }

  if (NewName == "_cont_Traversal")
    lgc::rt::setLgcRtShaderStage(&F, lgc::rt::RayTracingShaderStage::Traversal);
  else if (NewName == "_cont_KernelEntry")
    lgc::rt::setLgcRtShaderStage(&F,
                                 lgc::rt::RayTracingShaderStage::KernelEntry);

  Argument *RetArg = nullptr;
  AttributeList FAttrs = F.getAttributes();
  SmallVector<AttributeSet> ParamAttrs;

  unsigned ArgNo = 0;
  for (auto &Arg : F.args()) {
    ContArgTy ArgTy = ContArgTy::get(&F, &Arg);

    bool DidHandleArg = false;

    if (Arg.hasStructRetAttr()) {
      NewRetTy = Arg.getParamStructRetType();
      RetArg = &Arg;

      DidHandleArg = true;
    } else if (Arg.getType()->isPointerTy()) {
      StringRef NameRef{NewName};
      if (NameRef.contains("Await") || NameRef.contains("Enqueue") ||
          NameRef.contains("Traversal") ||
          (NewName == "_cont_SetTriangleHitAttributes" &&
           &Arg != F.getArg(0))) {
        // Pass argument data as struct instead of as pointer
        Type *ElemType = ArgTy.getPointerElementType();
        assert(ElemType && "Unable to resolve pointer type for argument");
        AllArgTypes.emplace_back(ElemType);
        ParamAttrs.push_back({});

        DidHandleArg = true;
      }
    }

    // Simply add the argument and its type.
    if (!DidHandleArg) {
      AllArgTypes.push_back(ArgTy);
      ParamAttrs.push_back(FAttrs.getParamAttrs(ArgNo));
    }

    ArgNo++;
  }

  // Create new empty function
  ContFuncTy NewFuncTy(NewRetTy, AllArgTypes);
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
  bool IsDeclaration = NewFunc->empty();

  if (!IsDeclaration) {
    B.emplace(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());

    if (UnpackMatrixTy) {
      // Move values of @class.matrix.x.y into return value of unpacked type
      // Replace the return instruction with a new one, returning the unpacked
      // value
      llvm::forEachTerminator(
          NewFunc, {Instruction::Ret}, [&](Instruction &Terminator) {
            B->SetInsertPoint(&Terminator);
            Value *RetExtractVal =
                B->CreateExtractValue(Terminator.getOperand(0), {0});
            B->CreateRet(RetExtractVal);
            Terminator.eraseFromParent();
          });
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

    if (!IsDeclaration) {
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

  if (RetArg && !IsDeclaration) {
    // Replace sret argument with real return value
    B->SetInsertPoint(&*NewFunc->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    auto *RetAlloca = B->CreateAlloca(NewRetTy);
    RetArg->replaceAllUsesWith(RetAlloca);

    // Replace returns with return value
    llvm::forEachTerminator(
        NewFunc, {Instruction::Ret}, [&](Instruction &Terminator) {
          B->SetInsertPoint(&Terminator);
          Value *RetLoad = B->CreateLoad(NewRetTy, RetAlloca);
          B->CreateRet(RetLoad);
          Terminator.eraseFromParent();
        });
  }

  // Replace all calls
  SmallVector<CallInst *> Uses;
  llvm::forEachCall(F, [&](CallInst &CInst) { Uses.push_back(&CInst); });

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
      "GetRtip",
      "GetShaderKind",
      "GetTriangleHitAttributes",
      "GetUninitialized",
      "I32Count",
      "IsEndSearch",
      "KernelEntry",
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
    bool IsContImpl = Name.contains("_cont_");
    bool ShouldTransform = false;

    if (IsContImpl) {
      if (isGpuRtFuncName(Name))
        ShouldTransform = true;
      else if (isUtilFunction(Name))
        ShouldTransform = true;
    } else if (Name.contains("_Amd") && isUtilFunction(Name)) {
      ShouldTransform = true;
    }

    if (ShouldTransform)
      transformFunction(*F);
  }

  fixupDxilMetadata(M);

  earlyDriverTransform(M);

  return PreservedAnalyses::none();
}
