/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- RematSupport.cpp - support functions for DXIL continuations ---------===//
//
// This file implements support functions for the DXIL continuations
//
//===----------------------------------------------------------------------===//

#include "RematSupport.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvm-dialects/Dialect/OpSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Coroutines/MaterializationUtils.h"

#define DEBUG_TYPE "remat-support"

using namespace llvm;
using namespace rematsupport;

/// Check if a value is in the given resource list.
/// The metadata argument must be one of the lists from dx.resources, i.e. for
/// SRVs.
static bool isInResources(Value *Handle, Metadata *MD) {
  if (!MD)
    return false;
  auto *ResourceMDs = cast<MDTuple>(MD);
  for (auto &Res : ResourceMDs->operands()) {
    auto *Val = mdconst::extract<Constant>(cast<MDTuple>(Res.get())->getOperand(1));
    // Strip casts
    while (auto *Cast = dyn_cast<ConstantExpr>(Val)) {
      assert(Cast->getOpcode() == Instruction::BitCast);
      Val = Cast->getOperand(0);
    }

    // Check if we found an SRV resource that matches the handle of the load
    if (Val == Handle)
      return true;
  }
  return false;
}

static bool isAnyDxilLoad(StringRef Name) {
  static const char *const LoadFunctions[] = {"dx.op.bufferLoad", "dx.op.rawBufferLoad", "dx.op.sample",
                                              "dx.op.textureLoad"};

  for (const auto *LoadFunc : LoadFunctions)
    if (Name.starts_with(LoadFunc))
      return true;
  return false;
}

/// Check if a load comes from constant memory (SRV or CBV) and can be
/// rematerialized.
///
/// Rematerializing these loads is beneficial if the result of a load is only
/// used after a resume point, or if it is a scalar load. In some cases, like
/// when continuation state is kept in registers and VGPR pressure is low, not
/// rematerializing such a load can be better for performance, but it is hard to
/// check that, so we rematerialize all constant loads.
static bool isRematerializableDxilLoad(CallInst *CInst, StringRef CalledName) {
  // First, check if this is a dxil load
  if (!isAnyDxilLoad(CalledName))
    return false;

  // Get the buffer handle
  auto *Handle = CInst->getArgOperand(1);
  // Unwrap dx.op.annotateHandle and dx.op.createHandleForLib calls.
  while (auto *Call = dyn_cast<CallInst>(Handle)) {
    assert(Call->getCalledFunction()->getName().starts_with("dx.op.annotateHandle") ||
           Call->getCalledFunction()->getName().starts_with("dx.op.createHandle"));
    Handle = Call->getArgOperand(1);
  }

  // For a resource load, this is the load of the resource handle
  if (auto *Load = dyn_cast<LoadInst>(Handle)) {
    Handle = Load->getPointerOperand();

    // Unwrap getelementptrs
    while (auto *Gep = dyn_cast<GEPOperator>(Handle))
      Handle = Gep->getPointerOperand();

    assert(isa<GlobalValue>(Handle) && "A resource should be a global value");

    // Search variable in SRV list
    auto *MD = Load->getModule()->getNamedMetadata("dx.resources")->getOperand(0);
    // in SRVs or CBVs
    if (isInResources(Handle, MD->getOperand(0).get()) || isInResources(Handle, MD->getOperand(2).get()))
      return true;
    else
      return false;
  }

  // If we fail to match the above LoadInst then this is an unhandled pattern
  // or a pattern we do not want to rematerialize. Note, it is always safe to
  // return 'false' in the case of unhandled patterns.

  LLVM_DEBUG({
    auto IsDoNotRematerializeCase = [&]() {
      // These patterns are recognized cases that we don't want to remat:
      //
      // Do not rematerialize an indirect handle load. Doing so would replace a
      // store and N loads (from/to continuation state) by 2N loads (N is the
      // number of resume functions using the value). 2N loads because every
      // resume function would need to load the handle from cont state followed
      // by the buffer load. For example:
      //  %284 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %281, ...
      //  %285 = extractvalue %dx.types.ResRet.i32 %284, 0
      //  %286 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %285, ...
      //  %287 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %286, ...
      //  %289 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %287, ...
      // Where %dx.types.ResRet.i32 is an aggregate like { i32, i32, i32, i32, ...}
      if (auto *EV = dyn_cast<ExtractValueInst>(Handle))
        if (auto *HandleCall = dyn_cast<Instruction>(EV->getAggregateOperand()))
          if (auto *BFCall = dyn_cast<CallInst>(HandleCall))
            if (BFCall->getCalledFunction()->getName().starts_with("dx.op.rawBufferLoad"))
              return true;
      return false;
    };

    if (!IsDoNotRematerializeCase()) {
      dbgs() << "Warning: isRematerializableDxilLoad unhandled pattern: ";
      Handle->dump();
    }
  });

  return false;
}

// Helper function to track the first one of a sequence of insert instructions.
template <typename T> static Instruction *TrackSequenceInsert(Instruction *Insert) {
  static_assert(std::is_base_of<Instruction, T>() && "T must be an llvm::Instruction type!");
  Instruction *FirstInsert = Insert;
  while (isa<T>(FirstInsert->getOperand(0))) {
    FirstInsert = cast<Instruction>(FirstInsert->getOperand(0));
  }

  // Only do this within a basic block, otherwise it might be unreliable.
  if (Insert->getParent() != FirstInsert->getParent())
    return Insert;
  return FirstInsert;
}

// Helper function to query whether an instruction is rematerializable, which is
// shared between both DX and Vulkan path.
static bool commonMaterializable(Instruction &Inst) {
  if (coro::isTriviallyMaterializable(Inst))
    return true;

  // Insert into constant.
  if (isa<InsertElementInst, InsertValueInst>(Inst)) {
    Instruction *FirstInsert = nullptr;
    if (FirstInsert = dyn_cast<InsertElementInst>(&Inst))
      FirstInsert = TrackSequenceInsert<InsertElementInst>(FirstInsert);
    else if (FirstInsert = dyn_cast<InsertValueInst>(&Inst))
      FirstInsert = TrackSequenceInsert<InsertValueInst>(FirstInsert);

    if (isa<Constant>(FirstInsert->getOperand(0)))
      return true;
  }

  if (auto *Shuffle = dyn_cast<ShuffleVectorInst>(&Inst)) {
    if (Shuffle->isSingleSource())
      return true;

    // If either operand is constant, rematerializing will not increase continuation state size.
    if (isa<Constant>(Shuffle->getOperand(0)) || isa<Constant>(Shuffle->getOperand(1)))
      return true;
  }

  return false;
}

bool rematsupport::isRematerializableLgcRtOp(CallInst &CInst, std::optional<lgc::rt::RayTracingShaderStage> Kind) {
  using namespace lgc::rt;
  Function *Callee = CInst.getCalledFunction();
  if (!LgcRtDialect::isDialectOp(*Callee))
    return false;

  // Always rematerialize
  static const llvm_dialects::OpSet RematerializableDialectOps =
      llvm_dialects::OpSet::get<DispatchRaysDimensionsOp, DispatchRaysIndexOp>();
  if (RematerializableDialectOps.contains(*Callee))
    return true;

  // Rematerialize for Intersection that can only call ReportHit, which keeps
  // the largest system data struct. These cannot be rematerialized in
  // ClosestHit, because if ClosestHit calls TraceRay or CallShader, that
  // information is lost from the system data struct. Also exclude rayTCurrent
  // because ReportHit calls can change that.
  if (!Kind || *Kind == RayTracingShaderStage::Intersection) {
    static const llvm_dialects::OpSet RematerializableIntersectionDialectOps =
        llvm_dialects::OpSet::get<InstanceIdOp, InstanceIndexOp, GeometryIndexOp, ObjectRayDirectionOp,
                                  ObjectRayOriginOp, ObjectToWorldOp, PrimitiveIndexOp, RayFlagsOp, RayTminOp,
                                  WorldRayDirectionOp, WorldRayOriginOp, WorldToObjectOp, InstanceInclusionMaskOp>();
    if (RematerializableIntersectionDialectOps.contains(*Callee))
      return true;
  }

  return false;
}

bool rematsupport::DXILMaterializable(Instruction &OrigI) {
  Instruction *V = &OrigI;

  // extract instructions are rematerializable, but increases the size of the
  // continuation state, so as a heuristic only rematerialize this if the source
  // can be rematerialized as well.
  while (true) {
    Instruction *NewInst = nullptr;
    if (auto *Val = dyn_cast<ExtractElementInst>(V))
      NewInst = dyn_cast<Instruction>(Val->getVectorOperand());
    else if (auto *Val = dyn_cast<ExtractValueInst>(V))
      NewInst = dyn_cast<Instruction>(Val->getAggregateOperand());

    if (NewInst)
      V = NewInst;
    else
      break;
  }

  if (commonMaterializable(*V))
    return true;

  // Loads associated with dx.op.createHandle calls
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    for (auto *LIUse : LI->users()) {
      if (auto *CallI = dyn_cast<CallInst>(LIUse)) {
        auto *CalledF = CallI->getCalledFunction();
        if (!CalledF || !CalledF->getName().starts_with("dx.op.createHandle"))
          return false;
      } else {
        return false;
      }
    }
    return true;
  }

  if (auto *CInst = dyn_cast<CallInst>(V)) {
    if (auto *CalledFunc = CInst->getCalledFunction()) {
      // Before rematerialization happens, lgc.rt dialect operations that cannot
      // be rematerialized are replaced by their implementation, so that the
      // necessary values can be put into the coroutine frame. Therefore, we
      // can assume all left-over intrinsics can be rematerialized.
      if (isRematerializableLgcRtOp(*CInst))
        return true;

      auto CalledName = CalledFunc->getName();
      if (CalledName.starts_with("dx.op.")) {
        // createHandle and createHandleForLib
        if (CalledName.starts_with("dx.op.createHandle"))
          return true;

        // Match by id
        unsigned int IntrId = cast<ConstantInt>(CInst->getArgOperand(0))->getZExtValue();
        if ((IntrId >= 6 && // FAbs - Dot4
             IntrId <= 56) ||
            IntrId == 58 ||   // CBufferLoad
            IntrId == 59 ||   // CBufferLoadLegacy
            IntrId == 101 ||  // MakeDouble
            IntrId == 102 ||  // SplitDouble
            (IntrId >= 124 && // Bitcast and Legacy casts
             IntrId <= 134) ||
            (IntrId >= 162 && // Dot-add functions
             IntrId <= 164) ||
            (IntrId >= 216 && // AnnotateHandle, CreateHandleFromBinding,
                              // CreateHandleFromHeap
             IntrId <= 218) ||
            IntrId == 219 || // Unpack4x8
            IntrId == 220 || // Pack4x8
            false) {
          return true;
        }

        // Loads from constant memory
        if (isRematerializableDxilLoad(CInst, CalledName))
          return true;
      }
    }
  }

  return false;
}

bool rematsupport::LgcMaterializable(Instruction &OrigI) {
  Instruction *V = &OrigI;

  // extract instructions are rematerializable, but increases the size of the
  // continuation state, so as a heuristic only rematerialize this if the source
  // can be rematerialized as well.
  while (true) {
    Instruction *NewInst = nullptr;
    if (auto *Val = dyn_cast<ExtractElementInst>(V))
      NewInst = dyn_cast<Instruction>(Val->getVectorOperand());
    else if (auto *Val = dyn_cast<ExtractValueInst>(V))
      NewInst = dyn_cast<Instruction>(Val->getAggregateOperand());

    if (NewInst)
      V = NewInst;
    else
      break;
  }

  if (commonMaterializable(*V))
    return true;

  if (auto *LI = dyn_cast<LoadInst>(V)) {
    // load from constant address space
    if (LI->getPointerAddressSpace() == 4)
      return true;
  }

  if (auto *CInst = dyn_cast<CallInst>(V)) {
    if (auto *CalledFunc = CInst->getCalledFunction()) {
      // Before rematerialization happens, lgc.rt dialect operations that cannot
      // be rematerialized are replaced by their implementation, so that the
      // necessary values can be put into the coroutine frame. Therefore, we
      // can assume all left-over intrinsics can be rematerialized.
      if (isRematerializableLgcRtOp(*CInst))
        return true;

      if (auto *Intrinsic = dyn_cast<IntrinsicInst>(CInst)) {
        switch (Intrinsic->getIntrinsicID()) {
        // Note: s_getpc will return a different value if rematerialized into a
        // different place, but assuming we only care about the high 32bit for
        // all the use cases we have now, it should be ok to do so.
        case Intrinsic::amdgcn_s_getpc:
          return true;
        default:
          break;
        }
      }

      auto CalledName = CalledFunc->getName();
      // FIXME: switch to dialectOp check.
      if (CalledName.starts_with("lgc.user.data") || CalledName.starts_with("lgc.shader.input") ||
          CalledName.starts_with("lgc.create.get.desc.ptr") || CalledName.starts_with("lgc.load.buffer.desc") ||
          CalledName.starts_with("lgc.load.strided.buffer.desc") || CalledName.starts_with("lgc.load.user.data"))
        return true;
    }
  }

  return false;
}
