/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- AddTypesMetadata.cpp - Build !types metadata -----------------------===//
//
// A pass that adds !types metadata to functions representing their argument
// types.
// This provides for transitioning IR to opaque pointers by embedding the
// required pointer typing information in metadata.
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/InitializePasses.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "add-types-metadata"

llvm::PreservedAnalyses
AddTypesMetadataPass::run(llvm::Module &M,
                          llvm::ModuleAnalysisManager &AnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run add-types-metadata pass\n");

  bool Changed = false;
  for (Function &F : M) {
    // Skip functions which have already been annotated
    if (F.hasMetadata("types"))
      continue;
    DXILContFuncTy::get(F.getFunctionType()).writeMetadata(&F);
    Changed = true;
  }

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
