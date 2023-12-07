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
#include "lgc/LgcRtDialect.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#define GPURTMAP_ENTRY(Op, GpurtName, AccessesHitData)                         \
  {                                                                            \
    OpDescription::get<lgc::rt::Op>(), { GpurtName, AccessesHitData }          \
  }

const OpMap<llvm::GpuRtIntrinsicEntry> llvm::LgcRtGpuRtMap = {{
    GPURTMAP_ENTRY(InstanceIdOp, "InstanceID", true),
    GPURTMAP_ENTRY(InstanceIndexOp, "InstanceIndex", true),
    GPURTMAP_ENTRY(HitKindOp, "HitKind", true),
    GPURTMAP_ENTRY(RayFlagsOp, "RayFlags", false),
    GPURTMAP_ENTRY(DispatchRaysIndexOp, "DispatchRaysIndex3", false),
    GPURTMAP_ENTRY(DispatchRaysDimensionsOp, "DispatchRaysDimensions3", false),
    GPURTMAP_ENTRY(WorldRayOriginOp, "WorldRayOrigin3", false),
    GPURTMAP_ENTRY(WorldRayDirectionOp, "WorldRayDirection3", false),
    GPURTMAP_ENTRY(ObjectRayOriginOp, "ObjectRayOrigin3", true),
    GPURTMAP_ENTRY(ObjectRayDirectionOp, "ObjectRayDirection3", true),
    GPURTMAP_ENTRY(ObjectToWorldOp, "ObjectToWorld4x3", true),
    GPURTMAP_ENTRY(WorldToObjectOp, "WorldToObject4x3", true),
    GPURTMAP_ENTRY(RayTminOp, "RayTMin", false),
    GPURTMAP_ENTRY(RayTcurrentOp, "RayTCurrent", true),
    GPURTMAP_ENTRY(IgnoreHitOp, "IgnoreHit", false),
    GPURTMAP_ENTRY(AcceptHitAndEndSearchOp, "AcceptHitAndEndSearch", false),
    GPURTMAP_ENTRY(TraceRayOp, "TraceRay", false),
    GPURTMAP_ENTRY(ReportHitOp, "ReportHit", false),
    GPURTMAP_ENTRY(CallCallableShaderOp, "CallShader", false),
    GPURTMAP_ENTRY(PrimitiveIndexOp, "PrimitiveIndex", true),
    GPURTMAP_ENTRY(GeometryIndexOp, "GeometryIndex", true),
}};

#undef GPURTMAP_ENTRY

llvm::StringRef DialectUtils::getLgcRtDialectOpName(llvm::StringRef FullName) {
  return FullName.substr(std::strlen("lgc.rt."));
}

bool DialectUtils::isLgcRtOp(const llvm::Function *F) {
  return F && F->getName().starts_with("lgc.rt");
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

  auto ImplEntry = LgcRtGpuRtMap.find(*Call);
  if (ImplEntry == LgcRtGpuRtMap.end())
    report_fatal_error("Unhandled lgc.rt op!");

  return *ImplEntry.val();
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

PointerType *llvm::getWithSamePointeeType(PointerType *PtrTy,
                                          unsigned AddressSpace) {
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 482880
  return PointerType::getWithSamePointeeType(PtrTy, AddressSpace);
#else
  // New version of the code (also handles unknown version, which we treat as
  // latest)
  return PointerType::get(PtrTy->getContext(), AddressSpace);
#endif
}
