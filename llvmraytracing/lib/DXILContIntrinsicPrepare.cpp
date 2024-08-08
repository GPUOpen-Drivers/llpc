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

//===- DXILContIntrinsicPrepare.cpp - Change signature of functions -------===//
//
// A pass that prepares driver implemented functions for later use.
//
// This pass unmangles function names and changes sret arguments back to
// return values.
//
//===----------------------------------------------------------------------===//

#include "compilerutils/ArgPromotion.h"
#include "llvmraytracing/Continuations.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include <cassert>
#include <cctype>

using namespace llvm;

#define DEBUG_TYPE "dxil-cont-intrinsic-prepare"

DXILContIntrinsicPreparePass::DXILContIntrinsicPreparePass() {
}

/// - Unmangle the function names to be more readable and to prevent confusion
/// with app defined functions later.
/// - Convert sret arguments back to return values
/// - Convert struct pointer arguments to pass structs by value
static Function *transformFunction(Function &F) {
  {
    // Local scope for Name which is invalidated at the end.
    auto Name = F.getName();
    LLVM_DEBUG(dbgs() << "Transforming function " << Name << "\n");
    std::string NewName = Name.str();

    // Unmangle declarations because they cannot be renamed in the dx api
    if (Name.contains('@')) {
      // Extract unmangled name
      auto Start = Name.find('?') + 1;
      auto End = Name.find('@', Start);
      if (Start == 0 || End == StringRef::npos || Start > Name.size() || End > Name.size()) {
        report_fatal_error(Twine("Failed to unmangle function name: Failed to extract from '") + Name +
                           "' (start: " + Twine(Start) + ", end: " + Twine(End) + ")");
      }

      // Copy name, otherwise it will be deleted before it's set
      NewName = Name.substr(Start, End - Start).str();
    }

    LLVM_DEBUG(dbgs() << "  Set new name " << NewName << "\n");

    if (NewName == ContDriverFunc::TraversalName)
      lgc::rt::setLgcRtShaderStage(&F, lgc::rt::RayTracingShaderStage::Traversal);
    else if (NewName == ContDriverFunc::KernelEntryName)
      lgc::rt::setLgcRtShaderStage(&F, lgc::rt::RayTracingShaderStage::KernelEntry);
    F.setName(NewName);
  }

  // Unpack the inner type of @class.matrix types
  Type *NewRetTy = F.getReturnType();
  Function *NewFn = &F;
  if (NewRetTy->isStructTy() && NewRetTy->getStructNumElements() == 1) {
    if (F.getName().contains("ObjectToWorld4x3") || F.getName().contains("WorldToObject4x3")) {
      NewFn = CompilerUtils::unpackStructReturnType(NewFn);
    }
  }

  // Lower `StructRet` argument.
  if (NewFn->hasStructRetAttr())
    NewFn = CompilerUtils::lowerStructRetArgument(NewFn);

  SmallBitVector PromotionMask(NewFn->arg_size());

  StringRef NameStr = NewFn->getName();
  for (unsigned ArgNo = 0; ArgNo < NewFn->arg_size(); ArgNo++) {
    auto *Arg = NewFn->getArg(ArgNo);
    TypedArgTy ArgTy = TypedArgTy::get(Arg);
    if (!ArgTy.isPointerTy())
      continue;

    if ((NameStr.contains("Await") || NameStr.contains("Enqueue") || NameStr.contains("Traversal") ||
         (NameStr == ContDriverFunc::SetTriangleHitAttributesName && ArgNo != 0)))
      PromotionMask.set(ArgNo);
  }
  // Promote pointer arguments to their pointee value types.
  NewFn = CompilerUtils::promotePointerArguments(NewFn, PromotionMask);

  NewFn->addFnAttr(Attribute::AlwaysInline);
  // Set external linkage, so the functions don't get removed, even if they are
  // never referenced at this point
  NewFn->setLinkage(GlobalValue::LinkageTypes::ExternalLinkage);
  return NewFn;
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
      "ContStack",
      "Enqueue", // To detect the mangled name of a declaration
      "ExitRayGen",
      "GetCandidateState",
      "GetCommittedState",
      "GetContinuationStackAddr",
      "GetContinuationStackGlobalMemBase",
      "GetCurrentFuncAddr",
      "GetFuncAddr",
      "GetI32",
      "GetLocalRootIndex",
      "GetResumePointAddr",
      "GetRtip",
      "GetSetting",
      "GetShaderKind",
      "GetTriangleHitAttributes",
      "GetUninitialized",
      "GpurtVersionFlags",
      "I32Count",
      "IsEndSearch",
      "KernelEntry",
      "ReportHit",
      "RestoreSystemData",
      "SetI32",
      "SetTriangleHitAttributes",
      "TraceRay",
      "Traversal",
      "ShaderStart",
  };

  for (const char *UtilName : UtilNames) {
    if (Name.contains(UtilName))
      return true;
  }

  return false;
}

llvm::PreservedAnalyses DXILContIntrinsicPreparePass::run(llvm::Module &M,
                                                          llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the dxil-cont-intrinsic-prepare pass\n");

  AnalysisManager.getResult<DialectContextAnalysis>(M);

  SmallVector<Function *> Funcs(make_pointer_range(M.functions()));

  for (auto *F : Funcs) {
    auto Name = F->getName();
    bool ShouldTransform = false;

    if (Name.contains("_cont_")) {
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
