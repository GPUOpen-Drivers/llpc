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
#include "llvm-dialects/Dialect/OpSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#define DEBUG_TYPE "continuations-util"

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

bool DXILContHelper::isRematerializableLgcRtOp(
    CallInst &CInst, std::optional<DXILShaderKind> Kind) {
  using namespace lgc::rt;
  Function *Callee = CInst.getCalledFunction();
  if (!DialectUtils::isLgcRtOp(Callee))
    return false;

  // Always rematerialize
  static const OpSet RematerializableDialectOps =
      OpSet::get<DispatchRaysDimensionsOp, DispatchRaysIndexOp>();
  if (RematerializableDialectOps.contains(*Callee))
    return true;

  // Rematerialize for Intersection that can only call ReportHit, which keeps
  // the largest system data struct. These cannot be rematerialized in
  // ClosestHit, because if ClosestHit calls TraceRay or CallShader, that
  // information is lost from the system data struct. Also exclude rayTCurrent
  // because ReportHit calls can change that.
  if (!Kind || *Kind == DXILShaderKind::Intersection) {
    static const OpSet RematerializableIntersectionDialectOps =
        OpSet::get<InstanceIdOp, InstanceIndexOp, GeometryIndexOp,
                   ObjectRayDirectionOp, ObjectRayOriginOp, ObjectToWorldOp,
                   PrimitiveIndexOp, RayFlagsOp, RayTminOp, WorldRayDirectionOp,
                   WorldRayOriginOp, WorldToObjectOp>();
    if (RematerializableIntersectionDialectOps.contains(*Callee))
      return true;
  }

  return false;
}

void llvm::replaceAllPointerUses(IRBuilder<> *Builder, Value *OldPointerValue,
                                 Value *NewPointerValue,
                                 SmallVectorImpl<Instruction *> &ToBeRemoved) {
  // Note: The implementation explicitly supports typed pointers, which
  //       complicates some of the code below.

  // Assert that both types are pointers that only differ in the address space.
  PointerType *OldPtrTy = cast<PointerType>(OldPointerValue->getType());
  PointerType *NewPtrTy = cast<PointerType>(NewPointerValue->getType());
  unsigned NewAS = NewPtrTy->getAddressSpace();
  assert(NewAS != OldPtrTy->getAddressSpace());
  assert(getWithSamePointeeType(OldPtrTy, NewAS) == NewPtrTy);

  OldPointerValue->mutateType(NewPtrTy);

  // Traverse through the users and setup the addrspace
  SmallVector<Value *> Worklist(OldPointerValue->users());
  OldPointerValue->replaceAllUsesWith(NewPointerValue);

  // Given a pointer type, get a pointer with the same pointee type (possibly
  // opaque) as the given type that uses the NewAS address space.
  auto GetMutatedPtrTy = [NewAS](Type *Ty) {
    PointerType *PtrTy = cast<PointerType>(Ty);
    // Support typed pointers:
    return getWithSamePointeeType(PtrTy, NewAS);
  };

  while (!Worklist.empty()) {
    Value *Ptr = Worklist.pop_back_val();
    Instruction *Inst = cast<Instruction>(Ptr);
    LLVM_DEBUG(dbgs() << "Visiting " << *Inst << '\n');
    // In the switch below, "break" means to continue with replacing
    // the users of the current value, while "continue" means to stop at
    // the current value, and proceed with next one from the work list.
    switch (Inst->getOpcode()) {
    default:
      LLVM_DEBUG(Inst->dump());
      llvm_unreachable("Unhandled instruction\n");
      break;
    case Instruction::Call: {
      if (Inst->isLifetimeStartOrEnd()) {
        // The lifetime marker is not useful anymore.
        Inst->eraseFromParent();
      } else {
        LLVM_DEBUG(Inst->dump());
        llvm_unreachable("Unhandled call instruction\n");
      }
      // No further processing needed for the users.
      continue;
    }
    case Instruction::Load:
    case Instruction::Store:
      // No further processing needed for the users.
      continue;
    case Instruction::And:
    case Instruction::Add:
    case Instruction::PtrToInt:
      break;
    case Instruction::BitCast: {
      // This can happen with typed pointers
      auto *BC = cast<BitCastOperator>(Inst);
      assert(cast<BitCastOperator>(Inst)->getSrcTy()->isPointerTy() &&
             BC->getDestTy()->isPointerTy());
      Inst->mutateType(GetMutatedPtrTy(Inst->getType()));
      break;
    }
    case Instruction::AddrSpaceCast:
      // Check that the pointer operand has already been fixed
      assert(Inst->getOperand(0)->getType()->getPointerAddressSpace() == NewAS);
      // Push the correct users before RAUW.
      Worklist.append(Ptr->users().begin(), Ptr->users().end());
      Inst->mutateType(GetMutatedPtrTy(Inst->getType()));
      // Since we are mutating the address spaces of users as well,
      // we can just use the (already mutated) cast operand.
      Inst->replaceAllUsesWith(Inst->getOperand(0));
      ToBeRemoved.push_back(Inst);
      continue;
    case Instruction::IntToPtr:
    case Instruction::GetElementPtr: {
      Inst->mutateType(GetMutatedPtrTy(Inst->getType()));
      break;
    }
    case Instruction::Select: {
      auto *OldType = Inst->getType();
      if (OldType->isPointerTy()) {
        Type *NewType = GetMutatedPtrTy(OldType);
        // No further processing if the type has the correct pointer type
        if (NewType == OldType)
          continue;

        Inst->mutateType(NewType);
      }
      break;
    }
    }

    Worklist.append(Ptr->users().begin(), Ptr->users().end());
  }
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
