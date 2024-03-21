/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "compilerutils/CompilerUtils.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#define DEBUG_TYPE "compilerutils"

using namespace llvm;

// =====================================================================================================================
// Create an LLVM function call to the named function. The callee is built
// automatically based on return type and its parameters.
//
// @param funcName : Name of the callee
// @param retTy : Return type of the callee
// @param args : Arguments to pass to the callee
// @param attribs : Function attributes
// @param instName : Name to give instruction
CallInst *CompilerUtils::createNamedCall(IRBuilder<> &builder, StringRef funcName, Type *retTy, ArrayRef<Value *> args,
                                         ArrayRef<Attribute::AttrKind> attribs, const Twine &instName) {
  assert(!funcName.empty());
  Module *mod = builder.GetInsertBlock()->getParent()->getParent();
  Function *func = dyn_cast_or_null<Function>(mod->getFunction(funcName));
  if (!func) {
    SmallVector<Type *, 8> argTys;
    argTys.reserve(args.size());
    for (auto *arg : args)
      argTys.push_back(arg->getType());

    auto *funcTy = FunctionType::get(retTy, argTys, false);
    func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, mod);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::NoUnwind);

    for (auto attrib : attribs) {
      switch (attrib) {
      default:
        func->addFnAttr(attrib);
        break;
      case Attribute::ReadNone:
        func->setDoesNotAccessMemory();
        break;
      case Attribute::ReadOnly:
        func->setOnlyReadsMemory();
        break;
      case Attribute::WriteOnly:
        func->setOnlyWritesMemory();
        break;
      }
    }
  }

  auto *call = builder.CreateCall(func, args, instName);
  call->setCallingConv(CallingConv::C);
  call->setAttributes(func->getAttributes());

  return call;
}

// Modify the function argument types, and return the new function. NOTE: the
// function does not do any uses replacement, so the caller should call
// replaceAllUsesWith() for the function and arguments afterwards.
Function *CompilerUtils::mutateFunctionArguments(Function &fn, Type *retTy, const ArrayRef<Type *> argTys,
                                                 AttributeList attributes) {
  FunctionType *newFnTy = FunctionType::get(retTy, argTys, false);
  auto *newFn = cloneFunctionHeader(fn, newFnTy, attributes);
  newFn->takeName(&fn);
  newFn->splice(newFn->begin(), &fn);
  return newFn;
}

Function *CompilerUtils::cloneFunctionHeader(Function &f, FunctionType *newType, AttributeList attributes,
                                             Module *targetModule) {
  LLVM_DEBUG(dbgs() << "Cloning function " << f.getName() << " with new type " << *newType << "\n");
  Function *newFunc = Function::Create(newType, f.getLinkage(), "", targetModule);

  if (targetModule) {
    assert(targetModule != f.getParent() && "targetModule cannot be the same as the current module");
    newFunc->setName(f.getName());
  } else {
    // Insert new function before f to facilitate writing tests
    f.getParent()->getFunctionList().insert(f.getIterator(), newFunc);
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 489715
    // If targetModule is null then take flag from original function.
    newFunc->setIsNewDbgInfoFormat(f.IsNewDbgInfoFormat);
#endif
  }

  newFunc->copyAttributesFrom(&f);
  newFunc->setSubprogram(f.getSubprogram());
  newFunc->setAttributes(attributes);
  newFunc->copyMetadata(&f, 0);
  return newFunc;
}

Function *CompilerUtils::cloneFunctionHeader(Function &f, FunctionType *newType, ArrayRef<AttributeSet> argAttrs,
                                             Module *targetModule) {
  const AttributeList fAttrs = f.getAttributes();
  const AttributeList attributes =
      AttributeList::get(f.getContext(), fAttrs.getFnAttrs(), fAttrs.getRetAttrs(), argAttrs);
  return cloneFunctionHeader(f, newType, attributes, targetModule);
}

namespace {

// Get the name of a global that is copied to a different module for inlining.
std::string getCrossModuleName(GlobalValue &gv) {
  if (auto *fn = dyn_cast<Function>(&gv)) {
    // Intrinsics should not be renamed since the IR verifier insists on a "correct" name mangling based on any
    // overloaded types. Lgc dialects also require exact name for similar reason.
    if (fn->isIntrinsic() || fn->getName().starts_with("lgc."))
      return fn->getName().str();
  }
  return (Twine(gv.getName()) + ".cloned." + gv.getParent()->getName()).str();
}

class CrossModuleValueMaterializer : public ValueMaterializer {
public:
  CrossModuleValueMaterializer(Module *targetMod, CompilerUtils::CrossModuleInliner &inliner,
                               SmallDenseMap<GlobalValue *, GlobalValue *> &mapped)
      : targetMod(targetMod), inliner(&inliner), mapped(&mapped) {}
  virtual ~CrossModuleValueMaterializer() = default;

  void setMapper(ValueMapper *mapper) { this->mapper = mapper; }

  virtual Value *materialize(Value *v) override {
    if (auto *gv = dyn_cast<GlobalValue>(v)) {
      if (gv->getParent() == targetMod)
        return nullptr;

      auto *newGv = moveGlobalValueToNewModule(gv);
      return newGv;
    }
    return nullptr;
  }

private:
  GlobalValue *moveGlobalValueToNewModule(GlobalValue *gv) {
    if (auto *existing = inliner->findCopiedGlobal(*gv, *targetMod))
      return existing;

    auto newName = getCrossModuleName(*gv);
    if (auto *callee = dyn_cast<Function>(gv)) {
      if (!callee->isDeclaration()) {
        report_fatal_error(
            Twine("Cross module inlining does not support functions with calls to functions with a body. "
                  "Run the inliner before trying to inline across modules (trying to call '") +
            callee->getName() + "')");
      }

      // Create a function declaration
      auto *newGv =
          CompilerUtils::cloneFunctionHeader(*callee, callee->getFunctionType(), callee->getAttributes(), targetMod);
      newGv->setName(newName);

      (*mapped)[gv] = newGv;
      return newGv;
    }

    if (auto *gVar = dyn_cast<GlobalVariable>(gv)) {
      // Create a global with the correct type
      auto *newGv = new GlobalVariable(*targetMod, gVar->getValueType(), gVar->isConstant(), gVar->getLinkage(),
                                       nullptr, newName, nullptr, gVar->getThreadLocalMode(), gVar->getAddressSpace());
      newGv->copyAttributesFrom(gVar);
      if (gVar->hasInitializer()) {
        // Recursively map initializer
        auto *newInit = mapper->mapConstant(*gVar->getInitializer());
        newGv->setInitializer(newInit);
      }

      (*mapped)[gv] = newGv;
      return newGv;
    }

    report_fatal_error("Encountered unknown global object while inlining");
  }

  Module *targetMod;
  CompilerUtils::CrossModuleInliner *inliner;
  SmallDenseMap<GlobalValue *, GlobalValue *> *mapped;
  ValueMapper *mapper;
};

} // anonymous namespace

iterator_range<Function::iterator> CompilerUtils::CrossModuleInliner::inlineCall(CallBase &cb) {
  auto *calleeFunc = cb.getCalledFunction();
  assert(calleeFunc && "Cannot find called function");
  LLVM_DEBUG(dbgs() << "Inlining '" << calleeFunc->getName() << "' across modules\n");

  Function *targetFunc = cb.getFunction();
  auto *targetMod = targetFunc->getParent();
  auto callBb = cb.getParent()->getIterator();
  auto callBbSuccessor = callBb;
  ++callBbSuccessor;
  const bool callBbHasSuccessor = callBbSuccessor != targetFunc->end();
  const size_t bbCount = targetFunc->size();
  // Save uses of the return value
  SmallVector<Value *> users(cb.users());

  const bool hasInstBefore = cb.getIterator() != callBb->begin();
  auto instBefore = hasInstBefore ? --cb.getIterator() : cb.getIterator();
  auto instAfter = ++cb.getIterator();
  const bool hasInstAfter = instAfter != callBb->end();

  if (calleeFunc->isDeclaration()) {
    // We cannot inline declarations, but declarations are useful for testing
    // purposes, so we allow declarations if we are not cross-module.
    assert(calleeFunc->getParent() == targetMod && "Cannot inline declarations cross-module");
    return make_range(callBb, callBb);
  }

  // Copy code
  InlineFunctionInfo ifi;
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 489715
  // calleeFunc is not from targetMod, check if we need to convert it.
  bool shouldConvert = !calleeFunc->IsNewDbgInfoFormat && targetMod->IsNewDbgInfoFormat;
  if (shouldConvert)
    calleeFunc->convertToNewDbgValues();
#endif
  auto res = InlineFunction(cb, ifi);
#if !defined(LLVM_MAIN_REVISION) || LLVM_MAIN_REVISION >= 489715
  if (shouldConvert)
    calleeFunc->convertFromNewDbgValues();
#endif
  if (!res.isSuccess())
    report_fatal_error(Twine("Failed to inline ") + calleeFunc->getName() + ": " + res.getFailureReason());

  // The first block of the inlined function gets concatenated into the calling BB,
  // the rest of the inlined function follows the calling BB.
  auto firstNewBb = callBb;
  auto lastNewBb = callBbHasSuccessor ? callBbSuccessor : targetFunc->end();

  // Check that the number of basic blocks is as expected
  assert(bbCount + static_cast<size_t>(std::distance(firstNewBb, lastNewBb)) - 1 == targetFunc->size() &&
         "Did not find all inlined blocks");

  if (calleeFunc->getParent() != targetMod) {
    // The name is important because it is used for copying and re-finding globals
    assert(!calleeFunc->getParent()->getName().empty() && "Can only inline from modules that have a name");

    // Look for references to global values and replace them with global values in the new module
    CrossModuleValueMaterializer materializer{targetMod, *this, mappedGlobals};
    ValueToValueMapTy map;
    ValueMapper mapper{map, RF_IgnoreMissingLocals, nullptr, &materializer};
    materializer.setMapper(&mapper);
    for (auto bb = firstNewBb; bb != lastNewBb; bb++) {
      bool skipBeforeInsts = hasInstBefore && bb == firstNewBb;
      for (auto &i : *bb) {
        // Skip instructions before and after the original call
        if (skipBeforeInsts) {
          if (&i == &*instBefore)
            skipBeforeInsts = false;
          continue;
        }
        if (hasInstAfter && &i == &*instAfter)
          break;

        mapper.remapInstruction(i);
      }
      assert((bb != firstNewBb || !hasInstBefore || !skipBeforeInsts) && "Did not find first instruction");
    }

    // If the inlined function returned a constant, that gets inlined into the users of the original value. Iterate over
    // these to catch all global values
    for (auto *u : users)
      mapper.remapInstruction(*cast<Instruction>(u));
  }

  return make_range(firstNewBb, lastNewBb);
}

CompilerUtils::CrossModuleInlinerResult
CompilerUtils::CrossModuleInliner::inlineCall(IRBuilder<> &b, llvm::Function *callee,
                                              llvm::ArrayRef<llvm::Value *> args) {
  auto *call = b.CreateCall(callee, args);
  // Create a fake use, so we can get the result of the inlined function.
  FreezeInst *fakeUse = nullptr;
  if (!callee->getReturnType()->isVoidTy())
    fakeUse = cast<FreezeInst>(b.CreateFreeze(call));

  // If the builder is at the end of the basic block then we don't have complete IR yet. We need some placeholder to
  // know where to reset the insert point to.
  Instruction *insertPointMarker = nullptr;
  if (b.GetInsertPoint() == b.GetInsertBlock()->end()) {
    assert(!b.GetInsertBlock()->getTerminator());
    if (fakeUse)
      insertPointMarker = fakeUse;
    else
      insertPointMarker = b.CreateUnreachable();
  }

  auto newBBs = inlineCall(*call);

  if (insertPointMarker) {
    b.SetInsertPoint(insertPointMarker->getParent());
    if (insertPointMarker != fakeUse)
      insertPointMarker->eraseFromParent();
  } else {
    b.SetInsertPoint(&*b.GetInsertPoint());
  }

  Value *result = nullptr;
  if (fakeUse) {
    result = fakeUse->getOperand(0);
    fakeUse->eraseFromParent();
  }

  return {result, newBBs};
}

GlobalValue *CompilerUtils::CrossModuleInliner::findCopiedGlobal(GlobalValue &sourceGv, Module &targetModule) {
  assert(sourceGv.getParent() != &targetModule && "This function only finds copies across modules");
  assert(sourceGv.hasName() && "Cannot find a global value that does not have a name");

  if (auto found = mappedGlobals.find(&sourceGv); found != mappedGlobals.end()) {
    assert(found->second->getParent() == &targetModule &&
           "The CrossModuleInliner can only be used with a single target module");
    return found->second;
  }

  GlobalValue *gv = targetModule.getNamedValue(getCrossModuleName(sourceGv));
  if (gv)
    assert(gv->getValueType() == sourceGv.getValueType());
  return gv;
}
