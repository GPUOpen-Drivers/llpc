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

//===- ContinuationsUtil.cpp - Continuations utilities -----------------===//
//
// This file defines implementations for helper functions for continuation
// passes.
//
//===----------------------------------------------------------------------===//

#include "continuations/ContinuationsUtil.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

llvm::StringMap<llvm::GpuRtIntrinsicEntry> llvm::LgcRtGpuRtMap = {
    {"instance.id", {"InstanceID", true}},
    {"instance.index", {"InstanceIndex", true}},
    {"hit.kind", {"HitKind", true}},
    {"ray.flags", {"RayFlags", false}},
    {"dispatch.rays.index", {"DispatchRaysIndex3", false}},
    {"dispatch.rays.dimensions", {"DispatchRaysDimensions3", false}},
    {"world.ray.origin", {"WorldRayOrigin3", false}},
    {"world.ray.direction", {"WorldRayDirection3", false}},
    {"object.ray.origin", {"ObjectRayOrigin3", true}},
    {"object.ray.direction", {"ObjectRayDirection3", true}},
    {"object.to.world", {"ObjectToWorld4x3", true}},
    {"world.to.object", {"WorldToObject4x3", true}},
    {"ray.tmin", {"RayTMin", false}},
    {"ray.tcurrent", {"RayTCurrent", true}},
    {"ignore.hit", {"IgnoreHit", false}},
    {"accept.hit.and.end.search", {"AcceptHitAndEndSearch", false}},
    {"trace.ray", {"TraceRay", false}},
    {"report.hit", {"ReportHit", false}},
    {"call.callable.shader", {"CallShader", false}},
    {"primitive.index", {"PrimitiveIndex", true}},
    {"geometry.index", {"GeometryIndex", true}},
};

llvm::StringRef DialectUtils::getLgcRtDialectOpName(llvm::StringRef FullName) {
  return FullName.substr(std::strlen("lgc.rt."));
}

bool DialectUtils::isLgcRtOp(const llvm::Function *F) {
  return F && F->getName().starts_with("lgc.rt");
}

// A small wrapper around that allows to apply a callback on the users (calls)
// of a function
void llvm::forEachCall(Function &F,
                       const std::function<void(CallInst &)> &Callback) {
  for (auto &Use : F.uses()) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser()))
      if (CInst->isCallee(&Use))
        Callback(*CInst);
  }
}

void llvm::forEachCall(Module &M,
                       const std::function<void(CallInst &)> &Callback) {
  for (auto &Func : M) {
    forEachCall(Func, Callback);
  }
}

void llvm::forEachCall(ArrayRef<Function *> Funcs,
                       const std::function<void(CallInst &)> &Callback) {
  for (auto *Func : Funcs) {
    forEachCall(*Func, Callback);
  }
}

void llvm::moveFunctionBody(Function &OldFunc, Function &NewFunc) {
  while (!OldFunc.empty()) {
    BasicBlock *BB = &OldFunc.front();
    BB->removeFromParent();
    BB->insertInto(&NewFunc);
  }
}

std::optional<llvm::GpuRtIntrinsicEntry>
llvm::findIntrImplEntryByIntrinsicCall(CallInst *Call) {
  if (!DialectUtils::isLgcRtOp(Call->getCalledFunction()))
    return std::nullopt;

  auto Name = Call->getCalledFunction()->getName();
  auto ImplEntry =
      LgcRtGpuRtMap.find(DialectUtils::getLgcRtDialectOpName(Name));
  if (ImplEntry == LgcRtGpuRtMap.end())
    report_fatal_error("Unhandled lgc.rt op!");

  return ImplEntry->second;
}

bool llvm::removeUnusedFunctionDecls(Module *Mod, bool OnlyIntrinsics) {
  bool DidChange = false;

  for (Function &F : make_early_inc_range(*Mod)) {
    if (F.isDeclaration() && F.user_empty()) {
      if (!OnlyIntrinsics ||
          (DialectUtils::isLgcRtOp(&F) || F.getName().starts_with("dx.op."))) {
        F.eraseFromParent();
        DidChange = true;
      }
    }
  }

  return DidChange;
}
