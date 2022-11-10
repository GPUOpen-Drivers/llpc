//===- SPIRVUtil.cpp - SPIR-V Utilities -------------------------*- C++ -*-===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file defines utility classes and functions shared by SPIR-V
/// reader/writer.
///
//===----------------------------------------------------------------------===//
#include "SPIRVInternal.h"

#define DEBUG_TYPE "spirv"

namespace SPIRV {

Function *getOrCreateFunction(Module *M, Type *RetTy, ArrayRef<Type *> ArgTypes, StringRef Name, AttributeList *Attrs,
                              bool TakeName) {
  const std::string MangledName(Name);
  bool IsVarArg = false;
  FunctionType *FT = FunctionType::get(RetTy, ArgTypes, IsVarArg);
  Function *F = M->getFunction(MangledName);
  if (!F || F->getFunctionType() != FT) {
    auto NewF = Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    if (F && TakeName) {
      NewF->takeName(F);
      LLVM_DEBUG(dbgs() << "[getOrCreateFunction] Warning: taking function Name\n");
    }
    if (NewF->getName() != MangledName) {
      LLVM_DEBUG(dbgs() << "[getOrCreateFunction] Warning: function Name changed\n");
    }
    LLVM_DEBUG(dbgs() << "[getOrCreateFunction] ");
    if (F)
      LLVM_DEBUG(dbgs() << *F << " => ");
    LLVM_DEBUG(dbgs() << *NewF << '\n');
    F = NewF;
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (Attrs)
      F->setAttributes(*Attrs);
  }
  return F;
}

void dumpUsers(Value *V, StringRef Prompt) {
  if (!V)
    return;
  LLVM_DEBUG(dbgs() << Prompt << " Users of " << *V << " :\n");
  for (auto UI = V->user_begin(), UE = V->user_end(); UI != UE; ++UI)
    LLVM_DEBUG(dbgs() << "  " << **UI << '\n');
}

bool eraseIfNoUse(Function *F) {
  bool Changed = false;
  if (!F)
    return Changed;
  if (!GlobalValue::isInternalLinkage(F->getLinkage()) && !F->isDeclaration())
    return Changed;

  dumpUsers(F, "[eraseIfNoUse] ");
  for (auto UI = F->user_begin(), UE = F->user_end(); UI != UE;) {
    auto U = *UI++;
    if (auto CE = dyn_cast<ConstantExpr>(U)) {
      if (CE->use_empty()) {
        CE->dropAllReferences();
        Changed = true;
      }
    }
  }
  if (F->use_empty()) {
    LLVM_DEBUG(F->printAsOperand(dbgs() << "Erase "));
    LLVM_DEBUG(dbgs() << '\n');
    F->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

void eraseIfNoUse(Value *V) {
  if (!V->use_empty())
    return;
  if (Constant *C = dyn_cast<Constant>(V)) {
    C->destroyConstant();
    return;
  }
  if (Instruction *I = dyn_cast<Instruction>(V)) {
    if (!I->mayHaveSideEffects())
      I->eraseFromParent();
  }
  eraseIfNoUse(dyn_cast<Function>(V));
}

bool eraseUselessFunctions(Module *M) {
  bool Changed = false;
  for (auto I = M->begin(), E = M->end(); I != E;)
    Changed |= eraseIfNoUse(&(*I++));
  return Changed;
}

} // namespace SPIRV
