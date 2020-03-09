/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchFatPointerArgs.cpp
 * @brief LLPC source file: pass to mutate fat pointer args
 *
 * This pass mutates any subfunction that has a fat pointer arg and/or return value so that instead it has a
 * {desc,offset} struct arg (or return value). Then it mutates any calls similarly.
 *
 * It needs to be a separate pass from PatchBufferOp (which does the rest of the fat pointer processing)
 * because the latter needs to be a function pass to depend on divergence analysis.
 *
 ***********************************************************************************************************************
 */
#include "lgc/BuilderBase.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/Defs.h"
#include "lgc/state/IntrinsDefs.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Pass.h"

#define DEBUG_TYPE "llpc-patch-fat-pointer-args"

using namespace llvm;
using namespace lgc;

namespace {

// =====================================================================================================================
// Pass to mutate fat pointer args
class PatchFatPointerArgs : public ModulePass {
public:
  PatchFatPointerArgs();

  bool runOnModule(Module &module) override;

  // -----------------------------------------------------------------------------------------------------------------

  static char ID; // ID of this pass

  PatchFatPointerArgs(const PatchFatPointerArgs &) = delete;
  PatchFatPointerArgs &operator=(const PatchFatPointerArgs &) = delete;

private:
  void processCall(CallInst *oldCall, Function *newFunc, BuilderBase &builder);
};

} // namespace

// =====================================================================================================================
// Initializes static members.
char PatchFatPointerArgs::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass to mutate fat pointer args
ModulePass *lgc::createPatchFatPointerArgs() {
  return new PatchFatPointerArgs();
}

// =====================================================================================================================
PatchFatPointerArgs::PatchFatPointerArgs() : ModulePass(ID) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchFatPointerArgs::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Mutate-Fat-Pointer-Args\n");

  // Identify the functions that need mutating.
  SmallVector<Function *, 4> oldFuncs;
  for (Function &func : module) {
    if (!func.isDeclaration()) {
      bool needsMutation = false;
      Type *retTy = func.getFunctionType()->getReturnType();
      if (isa<PointerType>(retTy) && retTy->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
        needsMutation = true;
      else {
        for (Type *paramTy : func.getFunctionType()->params()) {
          if (isa<PointerType>(paramTy) && paramTy->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
            needsMutation = true;
            break;
          }
        }
      }
      if (needsMutation)
        oldFuncs.push_back(&func);
    }
  }

  if (oldFuncs.empty())
    return false;

  ValueMap<Function *, Function *> mFuncMap;
  BuilderBase builder(module.getContext());
  auto descOffsetTy =
      StructType::get(builder.getContext(), {VectorType::get(builder.getInt32Ty(), 4), builder.getInt32Ty()});

  // Process each function, remembering call sites.
  SmallVector<CallInst *, 4> calls;
  bool haveIndirectCalls = false;
  for (Function *oldFunc : oldFuncs) {
    // Create new function type.
    Type *oldRetTy = oldFunc->getFunctionType()->getReturnType();
    Type *newRetTy = oldRetTy;
    if (isa<PointerType>(oldRetTy) && oldRetTy->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
      newRetTy = descOffsetTy;
    SmallVector<Type *, 4> newParamTys;
    for (Type *oldParamTy : oldFunc->getFunctionType()->params()) {
      Type *newParamTy = oldParamTy;
      if (isa<PointerType>(oldParamTy) && oldParamTy->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
        newParamTy = descOffsetTy;
      newParamTys.push_back(newParamTy);
    }
    auto newFuncTy = FunctionType::get(newRetTy, newParamTys, oldFunc->getFunctionType()->isVarArg());

    // Create new function.
    auto newFunc = Function::Create(newFuncTy, GlobalValue::InternalLinkage, oldFunc->getType()->getAddressSpace(), "",
                                    oldFunc->getParent());
    newFunc->takeName(oldFunc);
    newFunc->setCallingConv(oldFunc->getCallingConv());
    mFuncMap[oldFunc] = newFunc;

    // Copy attributes from the old function. That includes copying the argument "inreg" attributes.
    newFunc->setAttributes(oldFunc->getAttributes());

    // Transfer the code onto the new function.
    while (!oldFunc->empty()) {
      BasicBlock *block = &oldFunc->front();
      block->removeFromParent();
      block->insertInto(newFunc);
    }

    // Transfer uses of old args to new args. For a fat pointer arg, add code to convert from
    // {desc,offset} to fat pointer, unless the arg is unused.
    for (unsigned idx = 0, end = oldFunc->arg_size(); idx != end; ++idx) {
      Value *oldArg = oldFunc->getArg(idx);
      Value *newArg = newFunc->getArg(idx);
      newArg->setName(oldArg->getName());
      if (!oldArg->use_empty()) {
        if (oldArg->getType() != newArg->getType()) {
          builder.SetInsertPoint(&*newFunc->front().getFirstInsertionPt());
          newArg = builder.createNamedCall(
              lgcName::LateLaunderFatPointer, oldArg->getType(),
              {builder.CreateExtractValue(newArg, 0), builder.CreateExtractValue(newArg, 1)}, Attribute::ReadNone);
          // It might still have the wrong type, a fat pointer pointing to the wrong type.
          if (oldArg->getType() != newArg->getType())
            newArg = builder.CreateBitCast(newArg, oldArg->getType());
        }
        oldArg->replaceAllUsesWith(newArg);
      }
    }

    // Find uses, and insert a bitcast for non-call uses (which occur for indirect calls).
    SmallVector<Use *, 4> nonCallUses;
    for (auto &use : oldFunc->uses()) {
      User *user = use.getUser();
      auto call = dyn_cast<CallInst>(user);
      if (!call || !call->isCallee(&use)) {
        nonCallUses.push_back(&use);
        haveIndirectCalls = true;
      } else
        calls.push_back(call);
    }
    auto castNewFunc = ConstantExpr::getBitCast(newFunc, oldFunc->getType());
    for (Use *use : nonCallUses)
      *use = castNewFunc;

    if (isa<PointerType>(oldRetTy) && oldRetTy->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
      // Return type was fat pointer. Mutate return instruction(s).
      for (BasicBlock &block : *newFunc) {
        auto pReturn = dyn_cast<ReturnInst>(block.getTerminator());
        if (!pReturn)
          continue;
        builder.SetInsertPoint(pReturn);
        Value *castRetVal = builder.CreateBitCast(pReturn->getOperand(0),
                                                  builder.getInt8Ty()->getPointerTo(ADDR_SPACE_BUFFER_FAT_POINTER));
        builder.CreateRet(
            builder.createNamedCall(lgcName::LateUnlaunderFatPointer, newRetTy, castRetVal, Attribute::ReadNone));
        pReturn->eraseFromParent();
      }
    }
  }

  // Process direct calls to pass/return a {desc,offset} instead of a fat pointer.
  for (CallInst *call : calls)
    processCall(call, mFuncMap[call->getCalledFunction()], builder);

  // If there were any indirect calls, we have to scan the whole code to find and mutate them.
  if (haveIndirectCalls) {
    for (Function &func : module) {
      for (BasicBlock &block : func) {
        // For this basic block, first find indirect calls that need mutating.
        SmallVector<CallInst *, 4> indirectCalls;
        for (Instruction &inst : block) {
          if (auto call = dyn_cast<CallInst>(&inst)) {
            if (!call->getCalledFunction()) {
              // Indirect call. Only add it to the list if it actually has a fat pointer arg or
              // return value.
              if (isa<PointerType>(call->getType()) &&
                  call->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
                indirectCalls.push_back(call);
              else {
                for (Value *callArg : call->args()) {
                  if (isa<PointerType>(callArg->getType()) &&
                      callArg->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {
                    indirectCalls.push_back(call);
                    break;
                  }
                }
              }
            }
          }
        }

        // Now mutate the indirect calls that we found.
        for (CallInst *call : indirectCalls)
          processCall(call, nullptr, builder);
      }
    }
  }

  // Erase old functions.
  for (Function *func : oldFuncs)
    func->eraseFromParent();

  return true;
}

// =====================================================================================================================
// Process a call, replacing any fat pointer args or return value with a {desc,offset} struct.
// It is already known that the call needs mutating. For an indirect call, the callee needs to be changed
// to the appropriate bitcast of the original callee.
//
// @param [in/out] oldCall : Original call instruction, is erased if it is replaced
// @param newFunc : New mutated function to call, nullptr for indirect call
// @param [in/out] builder : Builder to use
void PatchFatPointerArgs::processCall(CallInst *oldCall, Function *newFunc, BuilderBase &builder) {
  builder.SetInsertPoint(oldCall);

  // Create the args for the new call.
  auto descOffsetTy =
      StructType::get(builder.getContext(), {VectorType::get(builder.getInt32Ty(), 4), builder.getInt32Ty()});
  SmallVector<Value *, 4> newArgs;
  for (Value *oldArg : oldCall->args()) {
    if (isa<PointerType>(oldArg->getType()) &&
        oldArg->getType()->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER) {

      Value *castOldArg =
          builder.CreateBitCast(oldArg, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_BUFFER_FAT_POINTER));
      newArgs.push_back(
          builder.createNamedCall(lgcName::LateUnlaunderFatPointer, descOffsetTy, castOldArg, Attribute::ReadNone));
    } else
      newArgs.push_back(oldArg);
  }

  Value *newCallee = newFunc;
  FunctionType *newFuncTy = nullptr;
  if (!newFunc) {
    // For an indirect call, get the new type of the callee and bitcast the function pointer to it.
    Type *newRetTy = oldCall->getType();
    if (isa<PointerType>(newRetTy) && newRetTy->getPointerAddressSpace() == ADDR_SPACE_BUFFER_FAT_POINTER)
      newRetTy = descOffsetTy;
    SmallVector<Type *, 4> newArgTys;
    for (Value *newArg : newArgs)
      newArgTys.push_back(newArg->getType());
    Type *newFuncTy = FunctionType::get(newRetTy, newArgTys, false);
    Type *newFuncPtrTy = newFuncTy->getPointerTo(oldCall->getCalledOperand()->getType()->getPointerAddressSpace());
    newCallee = builder.CreateBitCast(oldCall->getCalledOperand(), newFuncPtrTy);
  } else
    newFuncTy = newFunc->getFunctionType();

  // Create the new call.
  CallInst *newCall = builder.CreateCall(newFuncTy, newCallee, newArgs);
  newCall->setCallingConv(oldCall->getCallingConv());
  newCall->takeName(oldCall);

  // If the return value was a fat pointer, and is not unused, convert the new return value back to fat pointer.
  if (!oldCall->use_empty()) {
    Value *newVal = newCall;
    if (newVal->getType() != oldCall->getType()) {
      newVal = builder.createNamedCall(
          lgcName::LateLaunderFatPointer, builder.getInt8Ty()->getPointerTo(ADDR_SPACE_BUFFER_FAT_POINTER),
          {builder.CreateExtractValue(newCall, 0), builder.CreateExtractValue(newCall, 1)}, Attribute::ReadNone);
      newVal = builder.CreateBitCast(newVal, oldCall->getType());
    }

    // Replace uses and erase the old call.
    oldCall->replaceAllUsesWith(newVal);
  }
  oldCall->eraseFromParent();
}

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for buffer operations.
INITIALIZE_PASS(PatchFatPointerArgs, DEBUG_TYPE, "Patch LLVM for fat pointer args", false, false)
