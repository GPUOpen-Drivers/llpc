/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PatchContinufy.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchContinufy.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchContinufy.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#define DEBUG_TYPE "lgc-patch-continufy"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchContinufy::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  if (runImpl(module, pipelineState))
    return PreservedAnalyses::all(); // Note: this patching never invalidates analysis data
  return PreservedAnalyses::all();
}

static bool NeedsToBePatched(const Function &func) {
  return !func.isIntrinsic() &&
    !func.isDeclaration() &&
    (func.getCallingConv() == CallingConv::SPIR_FUNC);
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param pipelineState : Pipeline state
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchContinufy::runImpl(llvm::Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Continufy\n");

  SmallVector<ReturnInst*> returns;
  SmallVector<CallInst*> calls;
  SmallVector<Function*> functions;

  for (Function &func : module) {
    if (NeedsToBePatched(func)) {
      // add parameters, create new function with extra params, call CloneFunctionInto on it
    }
  }

  for (Function &func : module) {
    // TODO: also visit body of functions which do not need to be patched
    if (!NeedsToBePatched(func))
      continue;

    LLVM_DEBUG(dbgs() << "found function to be patched " << func.getName() << "\n");

    functions.push_back(&func);
  }

  IRBuilder<> builder(module.getContext());

  for (Function *func : functions) {
    LLVM_DEBUG(dbgs() << "patching function " << func->getName() << "\n");

    FunctionType *type = func->getFunctionType();

    SmallVector<Type*> newParams;
    newParams.push_back(builder.getInt32Ty());
    newParams.push_back(builder.getInt32Ty());
    for (Type *paramType : type->params()) {
      newParams.push_back(paramType);
    }

    FunctionType *newType = FunctionType::get(
      type->getReturnType(),
      newParams,
      type->isVarArg());

    Function *newFunc = Function::Create(
      newType,
      func->getLinkage(),
      func->getAddressSpace(),
      func->getName(),
      func->getParent());

    ValueToValueMapTy vmap;
    SmallVector<ReturnInst*> returns;
    CloneFunctionInto(newFunc, func, vmap, CloneFunctionChangeType::LocalChangesOnly, returns);

    for (auto& BB : *newFunc) {
      LLVM_DEBUG(dbgs() << "visiting BB\n");
      for (auto& Instr : BB) {
        LLVM_DEBUG(dbgs() << "visiting instr\n");

        ReturnInst *ret = dyn_cast<ReturnInst>(&Instr);
        if (ret != nullptr) {
          LLVM_DEBUG(dbgs() << "found return\n");
          returns.push_back(ret);
        }

        CallInst *call = dyn_cast<CallInst>(&Instr);
        if (call != nullptr) {
          LLVM_DEBUG(dbgs() << "found call\n");
          calls.push_back(call);
        }
      }
    }

    newFunc->takeName(func);
    func->replaceAllUsesWith(newFunc);
    func->eraseFromParent();
  }

  for (ReturnInst *ret : returns) {
    Function* func = ret->getFunction();

    builder.SetInsertPoint(ret);
    auto fn = func->getParent()->getFunction("lgc.cps.jump");
    SmallVector<Value*> args = {builder.getInt32(0), builder.getInt32(0)};
    if (!func->getReturnType()->isVoidTy()) {
      LLVM_DEBUG(dbgs() << "adding return value\n");

      args.push_back(ret->getReturnValue());
    }
    builder.CreateCall(fn, args);
    auto unreachable = new UnreachableInst(builder.getContext());
    ReplaceInstWithInst(ret, unreachable);
  }

  for (CallInst *call : calls) {
    builder.SetInsertPoint(call);
    Value *fptr = builder.CreatePtrToInt(call->getCalledOperand(), builder.getInt32Ty());
    auto await = module.getFunction("lgc.cps.await.void");
    SmallVector<Value*> args = {fptr, builder.getInt32(0)};
    // for(auto arg : call->args()) {
    //   args.push_back(arg);
    // }
    ReplaceInstWithInst(call, CallInst::Create(await->getFunctionType(), await, args));
  }

  return returns.size() > 0 || calls.size() > 0 || functions.size() > 0;
}

} // namespace lgc
