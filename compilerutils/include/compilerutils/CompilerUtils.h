/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- CompilerUtils.h - Library for compiler frontends -------------------===//
//
// Implements several shared helper functions.
//
//===----------------------------------------------------------------------===//

#ifndef COMPILERUTILS_H
#define COMPILERUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/IRBuilder.h"

namespace CompilerUtils {

// Create an LLVM function call to the named function. The callee is built
// automatically based on return type and its parameters.
//
// @param funcName : Name of the callee
// @param retTy : Return type of the callee
// @param args : Arguments to pass to the callee
// @param attribs : Function attributes
// @param instName : Name to give instruction
llvm::CallInst *createNamedCall(llvm::IRBuilder<> &, llvm::StringRef, llvm::Type *, llvm::ArrayRef<llvm::Value *>,
                                llvm::ArrayRef<llvm::Attribute::AttrKind>, const llvm::Twine & = "");

// Modify the function argument types, and return the new function. NOTE: the
// function does not do any uses replacement, so the caller should call
// replaceAllUsesWith() for the function and arguments afterwards.
llvm::Function *mutateFunctionArguments(llvm::Function &, llvm::Type *, const llvm::ArrayRef<llvm::Type *>,
                                        llvm::AttributeList);

// Create a new function based on another function, copying attributes and
// other properties.
// Specify targetModule to create the function in a different module than f.
llvm::Function *cloneFunctionHeader(llvm::Function &f, llvm::FunctionType *newType, llvm::AttributeList attributes,
                                    llvm::Module *targetModule = nullptr);

// Overload of cloneFunctionHeader that takes the new attributes for arguments and preserves the rest.
llvm::Function *cloneFunctionHeader(llvm::Function &f, llvm::FunctionType *newType,
                                    llvm::ArrayRef<llvm::AttributeSet> argAttrs, llvm::Module *targetModule = nullptr);

// Add an unreachable at the current position and remove the rest of the basic block.
void createUnreachable(llvm::IRBuilder<> &b);

// Specifies a memory that is loaded is the last use.
void setIsLastUseLoad(llvm::LoadInst &Load);

struct CrossModuleInlinerResult {
  llvm::Value *returnValue;
  llvm::iterator_range<llvm::Function::iterator> newBBs;
};

// The class caches already mapped constants. Reusing an instance of this class is more efficient than creating a new
// instance every time but it does not have an impact on the generated code.
// One CrossModuleInliner instance must only be used for a single target module, otherwise things can go wrong.
class CrossModuleInliner {
public:
  CrossModuleInliner() = default;

  // Inline a call to a function even if the called function is in a different module.
  // If the result of that function call should be used, a use must exist before calling this function.
  // Returns the new created basic blocks. These blocks may also contain instructions that were already
  // there before, if the function got inlined into an existing block.
  //
  // The insertion point of existing IRBuilders may have their insertion point invalidated because this
  // function splits basic blocks.
  // They can be made functional again with b.SetInsertPoint(&*b.GetInsertPoint()).
  llvm::iterator_range<llvm::Function::iterator> inlineCall(llvm::CallBase &cb);

  // Inline a call to a function even if the called function is in a different module.
  // Returns the result of the call and new created basic blocks. These blocks may also contain instructions that were
  // already there before, if the function got inlined into an existing block.
  //
  // This is a convenience wrapper around inlineCall(CallBase&). As users of the callee's return value are not known
  // while inlining, using this function can result in slightly less folding in the IR.
  CrossModuleInlinerResult inlineCall(llvm::IRBuilder<> &b, llvm::Function *callee,
                                      llvm::ArrayRef<llvm::Value *> args = std::nullopt);

  // Find a global value (function or variable) that was copied by the cross-module inliner.
  // Arguments are the global from the source module and the target module. Returns the corresponding global from the
  // target module.
  llvm::GlobalValue *findCopiedGlobal(llvm::GlobalValue &sourceGv, llvm::Module &targetModule);

private:
  // Checks that we haven't processed a different target module earlier.
  void checkTargetModule(llvm::Module &targetModule) {
    if (lastUsedTargetModule == nullptr)
      lastUsedTargetModule = &targetModule;
    else
      assert(lastUsedTargetModule == &targetModule);
  }

  llvm::SmallDenseMap<llvm::GlobalValue *, llvm::GlobalValue *> mappedGlobals;
  llvm::Module *lastUsedTargetModule = nullptr; // used to check that we don't use different target modules
};

// Essentially RAUW for pointers for the case that these use different address
// spaces, rewriting all derived pointers to also use the new address space.
// Writes instructions which are redundant after the replacement into
// the given ToBeRemoved vector.
// The caller has to handle the erasure afterwards.
void replaceAllPointerUses(llvm::IRBuilder<> *builder, llvm::Value *oldPointerValue, llvm::Value *newPointerValue,
                           llvm::SmallVectorImpl<llvm::Instruction *> &toBeRemoved);
} // namespace CompilerUtils

namespace llvm {

// Replacement for PointerType::getWithSamePointeeType that works with new LLVM.
// Returns a typed pointer type if the pointer type is typed.
//
// TODO: Remove this as soon as all internal users of opaque pointers have been
//       fixed.
PointerType *getWithSamePointeeType(PointerType *ptrTy, unsigned addressSpace);

/// Free-standing helpers.

// Helper to visit all calls of a function.
// Expected type for Callback:
//  void(CallInst &)
template <typename CallbackTy> void forEachCall(Function &F, CallbackTy Callback) {
  static_assert(std::is_invocable_v<CallbackTy, CallInst &>);
  for (auto &Use : make_early_inc_range(F.uses())) {
    if (auto *CInst = dyn_cast<CallInst>(Use.getUser()))
      if (CInst->isCallee(&Use))
        Callback(*CInst);
  }
}

// For each basic block in Func, find the terminator. If it is contained in
// TerminatorOpcodes, then apply the callback on the terminator.
template <typename CallbackTy, typename = std::enable_if<std::is_invocable_v<CallbackTy, llvm::Instruction &>>>
void forEachTerminator(Function *Func, ArrayRef<unsigned> TerminatorOpcodes, CallbackTy Callback) {
  for (auto &BB : *Func) {
    auto *Terminator = BB.getTerminator();
    if (llvm::find(TerminatorOpcodes, Terminator->getOpcode()) != TerminatorOpcodes.end())
      Callback(*Terminator);
  }
}

} // namespace llvm

#endif
