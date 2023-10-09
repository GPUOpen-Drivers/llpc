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

//===- DXILSupport.cpp - support functions for DXIL continuations ---------===//
//
// This file implements support functions for the DXIL continuations
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "continuations/ContinuationsUtil.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

using namespace llvm;

#define DEBUG_TYPE "dxil-coro-split"

namespace llvm {
namespace coro {
bool defaultMaterializable(Instruction &V);
} // End namespace coro
} // End namespace llvm

/// Check if a value is in the given resource list.
/// The metadata argument must be one of the lists from dx.resources, i.e. for
/// SRVs.
static bool isInResources(Value *Handle, Metadata *MD) {
  if (!MD)
    return false;
  auto *ResourceMDs = cast<MDTuple>(MD);
  for (auto &Res : ResourceMDs->operands()) {
    auto *Val =
        mdconst::extract<Constant>(cast<MDTuple>(Res.get())->getOperand(1));
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
  static const char *const LoadFunctions[] = {
      "dx.op.bufferLoad", "dx.op.rawBufferLoad", "dx.op.sample",
      "dx.op.textureLoad"};

  bool IsLoad = false;
  for (const auto *LoadFunc : LoadFunctions) {
    if (CalledName.startswith(LoadFunc)) {
      IsLoad = true;
      break;
    }
  }
  if (!IsLoad)
    return false;

  // Get the buffer handle
  auto *Handle = CInst->getArgOperand(1);
  // Unwrap dx.op.annotateHandle and dx.op.createHandleForLib calls.
  while (auto *Call = dyn_cast<CallInst>(Handle)) {
    assert(
        Call->getCalledFunction()->getName().startswith(
            "dx.op.annotateHandle") ||
        Call->getCalledFunction()->getName().startswith("dx.op.createHandle"));
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
    auto *MD =
        Load->getModule()->getNamedMetadata("dx.resources")->getOperand(0);
    // in SRVs or CBVs
    if (isInResources(Handle, MD->getOperand(0).get()) ||
        isInResources(Handle, MD->getOperand(2).get()))
      return true;
  } else {
    // Failing the check in release mode is fine, but we still want to know
    // cases where this does not match, so assert in that case.
    assert(false && "A handle should originate from a load instruction");
  }

  // Not found in the lists, so not a constant buffer
  return false;
}

bool llvm::DXILMaterializable(Instruction &OrigI) {
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

  if (coro::defaultMaterializable(*V))
    return true;

  // Loads associated with dx.op.createHandle calls
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    for (auto *LIUse : LI->users()) {
      if (auto *CallI = dyn_cast<CallInst>(LIUse)) {
        auto *CalledF = CallI->getCalledFunction();
        if (!CalledF || !CalledF->getName().startswith("dx.op.createHandle"))
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
      if (CalledName.startswith("dx.op.")) {
        // createHandle and createHandleForLib
        if (CalledName.startswith("dx.op.createHandle"))
          return true;

        // Match by id
        unsigned int IntrId =
            cast<ConstantInt>(CInst->getArgOperand(0))->getZExtValue();
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
