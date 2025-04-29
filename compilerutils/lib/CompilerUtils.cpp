/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "ValueOriginTrackingTestPass.h"
#include "ValueSpecializationTestPass.h"
#include "compilerutils/DxilToLlvm.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSlotTracker.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#define DEBUG_TYPE "compilerutils"

using namespace llvm;
using namespace compilerutils;

// Whether this is a load instruction that should translate to a last_use
// load.
static constexpr const char *MDIsLastUseName = "amdgpu.last.use";

// =====================================================================================================================
// Create an LLVM function call to the named function. The callee is built
// automatically based on return type and its parameters.
//
// @param funcName : Name of the callee
// @param retTy : Return type of the callee
// @param args : Arguments to pass to the callee
// @param attribs : Function attributes
// @param instName : Name to give instruction
CallInst *compilerutils::createNamedCall(IRBuilder<> &builder, StringRef funcName, Type *retTy, ArrayRef<Value *> args,
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
Function *compilerutils::mutateFunctionArguments(Function &fn, Type *retTy, const ArrayRef<Type *> argTys,
                                                 AttributeList attributes) {
  FunctionType *newFnTy = FunctionType::get(retTy, argTys, false);
  auto *newFn = cloneFunctionHeader(fn, newFnTy, attributes);
  newFn->takeName(&fn);
  newFn->splice(newFn->begin(), &fn);
  return newFn;
}

Function *compilerutils::cloneFunctionHeader(Function &f, FunctionType *newType, AttributeList attributes,
                                             Module *targetModule) {
  LLVM_DEBUG(dbgs() << "Cloning function " << f.getName() << " with new type " << *newType << "\n");
  Function *newFunc = Function::Create(newType, f.getLinkage(), "", targetModule);

  if (targetModule) {
    assert(targetModule != f.getParent() && "targetModule cannot be the same as the current module");
    newFunc->setName(f.getName());
  } else {
    // Insert new function before f to facilitate writing tests
    f.getParent()->getFunctionList().insert(f.getIterator(), newFunc);
    // If targetModule is null then take flag from original function.
    newFunc->setIsNewDbgInfoFormat(f.IsNewDbgInfoFormat);
  }

  newFunc->copyAttributesFrom(&f);
  newFunc->setSubprogram(f.getSubprogram());
  newFunc->setAttributes(attributes);
  newFunc->copyMetadata(&f, 0);
  return newFunc;
}

Function *compilerutils::cloneFunctionHeader(Function &f, FunctionType *newType, ArrayRef<AttributeSet> argAttrs,
                                             Module *targetModule) {
  const AttributeList fAttrs = f.getAttributes();
  const AttributeList attributes =
      AttributeList::get(f.getContext(), fAttrs.getFnAttrs(), fAttrs.getRetAttrs(), argAttrs);
  return cloneFunctionHeader(f, newType, attributes, targetModule);
}

void compilerutils::createUnreachable(llvm::IRBuilder<> &b) {
  auto *unreachable = b.CreateUnreachable();
  auto it = ++unreachable->getIterator();
  auto *bb = unreachable->getParent();
  if (it == bb->end())
    return;

  // Remove rest of BB
  auto *oldCode = BasicBlock::Create(b.getContext(), "", bb->getParent());
  oldCode->splice(oldCode->end(), bb, it, bb->end());
  oldCode->replaceSuccessorsPhiUsesWith(bb, oldCode);
  DeleteDeadBlock(oldCode);
}

void compilerutils::setIsLastUseLoad(llvm::LoadInst &Load) {
  Load.setMetadata(MDIsLastUseName, MDTuple::get(Load.getContext(), {}));
}

// =====================================================================================================================
// Ensures that the given function has a single, unified return point. This function modifies the LLVM IR to create a
// single return block for functions with multiple return statements.
//
// @param function: The Function to modify
// @param builder: An IRBuilder instance used for inserting new instructions
// @param blockName: The name to give to the new unified return block
llvm::ReturnInst *compilerutils::unifyReturns(Function &function, llvm::IRBuilder<> &builder, const Twine &blockName) {
  SmallVector<ReturnInst *> retInsts;

  for (BasicBlock &block : function) {
    if (auto *retInst = dyn_cast<ReturnInst>(block.getTerminator()))
      retInsts.push_back(retInst);
  }

  // It is expected when unifyReturns is called, the input function should not be empty, it is expected to have at
  // least one return instruction.
  assert(!retInsts.empty() && "Function has no return instruction");
  if (retInsts.size() == 1) {
    return retInsts[0];
  }

  // There are more than 2 returns; create a unified return block.
  //
  // Also create a "unified return block" if there are no returns at all. Such a shader will surely hang or otherwise
  // trigger UB if it is ever executed, but we still need to compile it correctly in case it never runs.
  BasicBlock *retBlock = BasicBlock::Create(builder.getContext(), blockName, &function);
  Type *returnType = function.getReturnType();
  PHINode *phiNode = nullptr;

  // If the function has multiple returns and a non-void return type, create a PHI node to collect the return values
  // from different paths.
  if (!returnType->isVoidTy()) {
    builder.SetInsertPoint(retBlock);
    phiNode = builder.CreatePHI(returnType, retInsts.size(), "retval");
  }

  for (ReturnInst *retInst : retInsts) {
    BasicBlock *predBlock = retInst->getParent();
    Value *retVal = retInst->getReturnValue();

    builder.SetInsertPoint(retInst);
    builder.CreateBr(retBlock);

    if (phiNode)
      phiNode->addIncoming(retVal, predBlock);

    retInst->eraseFromParent();
  }

  builder.SetInsertPoint(retBlock);

  if (returnType->isVoidTy())
    return builder.CreateRetVoid();
  else
    return builder.CreateRet(phiNode);
}

namespace {

// Map Types from source to target module.
struct CrossModuleTypeRemapper : public ValueMapTypeRemapper {
  CrossModuleTypeRemapper() = default;

  Type *remapType(Type *SrcTy) override {
    if (auto found = mappedTypes.find(SrcTy); found != mappedTypes.end()) {
      return found->second;
    }
    return SrcTy;
  }

  DenseMap<Type *, Type *> mappedTypes;
};

} // anonymous namespace

class CrossModuleInliner::CrossModuleValueMaterializer : public ValueMaterializer {
public:
  CrossModuleValueMaterializer(CrossModuleInliner &inliner) : inliner(&inliner) {}
  virtual ~CrossModuleValueMaterializer() = default;

  virtual Value *materialize(Value *v) override;

  CrossModuleInliner *inliner;
};

struct CrossModuleInliner::Impl {
  Impl(CrossModuleInliner &inliner, GetGlobalInModuleTy getGlobalInModuleFunc)
      : materializer(inliner), mapper(map, RF_IgnoreMissingLocals, &typeRemapper, &materializer),
        getGlobalInModuleFunc(std::move(getGlobalInModuleFunc)) {}

  CrossModuleTypeRemapper typeRemapper;
  CrossModuleValueMaterializer materializer;
  llvm::ValueToValueMapTy map;
  llvm::ValueMapper mapper;
  GetGlobalInModuleTy getGlobalInModuleFunc;
  llvm::Module *targetMod = nullptr;
};

Value *CrossModuleInliner::CrossModuleValueMaterializer::materialize(Value *v) {
  if (auto *gv = dyn_cast<GlobalValue>(v)) {
    if (gv->getParent() == inliner->impl->targetMod)
      return nullptr;

    GlobalValue *newGv = inliner->findCopiedGlobal(*gv, *inliner->impl->targetMod);
    if (!newGv)
      newGv = &inliner->impl->getGlobalInModuleFunc(*inliner, *gv, *inliner->impl->targetMod);

    // Insert into mappedTypes if there is no entry yet.
    // Ensure recorded type mappings are consistent.
    auto &mappedTypes = inliner->impl->typeRemapper.mappedTypes;
    auto InsertToMappedTypes = [&mappedTypes](Type *sourceType, Type *copiedType) {
      assert((sourceType != nullptr) && (copiedType != nullptr));
      if (sourceType != copiedType) {
        [[maybe_unused]] auto found = mappedTypes.insert(std::make_pair(sourceType, copiedType));
        assert((found.second || copiedType == found.first->second) && "Inconsistent type mapping");
      }
    };
    if (isa<GlobalVariable>(newGv)) {
      Type *sourceType = gv->getValueType();
      Type *copiedType = newGv->getValueType();
      InsertToMappedTypes(sourceType, copiedType);
    } else if (auto *func = dyn_cast<Function>(gv)) {
      // Map type for function arguments and return.
      FunctionType *sourceFuncTy = dyn_cast<FunctionType>(func->getFunctionType());
      FunctionType *copiedFuncTy = dyn_cast<FunctionType>(cast<Function>(newGv)->getFunctionType());
      for (unsigned index = 0; index < sourceFuncTy->getNumParams(); ++index) {
        Type *sourceArgTy = sourceFuncTy->getParamType(index);
        Type *copiedArgTy = copiedFuncTy->getParamType(index);
        InsertToMappedTypes(sourceArgTy, copiedArgTy);
      }
      Type *sourceRetType = func->getReturnType();
      Type *copiedRetType = copiedFuncTy->getReturnType();
      InsertToMappedTypes(sourceRetType, copiedRetType);
    }

    return newGv;
  }
  return nullptr;
}

CrossModuleInliner::CrossModuleInliner(GetGlobalInModuleTy getGlobalInModuleCallback)
    : impl(std::make_unique<Impl>(*this, std::move(getGlobalInModuleCallback))) {
}

CrossModuleInliner::CrossModuleInliner(CrossModuleInliner &&inliner) : impl(std::move(inliner.impl)) {
  if (impl)
    impl->materializer.inliner = this;
}

CrossModuleInliner &CrossModuleInliner::operator=(CrossModuleInliner &&inliner) {
  impl = std::move(inliner.impl);
  if (impl)
    impl->materializer.inliner = this;
  return *this;
}

CrossModuleInliner::~CrossModuleInliner() = default;

iterator_range<Function::iterator> compilerutils::CrossModuleInliner::inlineCall(CallBase &cb) {
  auto *calleeFunc = cb.getCalledFunction();
  assert(calleeFunc && "Cannot find called function");
  checkTargetModule(*cb.getFunction()->getParent());
  LLVM_DEBUG(dbgs() << "Inlining '" << calleeFunc->getName() << "' across modules\n");

  Function *targetFunc = cb.getFunction();
  auto *targetMod = targetFunc->getParent();
  auto callBb = cb.getParent()->getIterator();

  auto callBbSuccessor = callBb;
  ++callBbSuccessor;
  const bool callBbHasSuccessor = callBbSuccessor != targetFunc->end();
  [[maybe_unused]] const size_t bbCount = targetFunc->size();
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
  // calleeFunc is not from targetMod, check if we need to convert it.
  bool shouldConvert = !calleeFunc->IsNewDbgInfoFormat && targetMod->IsNewDbgInfoFormat;
  if (shouldConvert)
    calleeFunc->convertToNewDbgValues();
  auto res = InlineFunction(cb, ifi);
  if (shouldConvert)
    calleeFunc->convertFromNewDbgValues();
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

        impl->mapper.remapInstruction(i);
      }
      assert((bb != firstNewBb || !hasInstBefore || !skipBeforeInsts) && "Did not find first instruction");
    }

    // If the inlined function returned a constant, that gets inlined into the users of the original value. Iterate over
    // these to catch all global values
    for (auto *u : users)
      impl->mapper.remapInstruction(*cast<Instruction>(u));
  }

  return make_range(firstNewBb, lastNewBb);
}

compilerutils::CrossModuleInlinerResult
compilerutils::CrossModuleInliner::inlineCall(IRBuilder<> &b, llvm::Function *callee,
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

GlobalValue *compilerutils::CrossModuleInliner::findCopiedGlobal(GlobalValue &sourceGv, Module &targetModule) {
  checkTargetModule(targetModule);

  if (auto found = impl->map.find(&sourceGv); found != impl->map.end()) {
    auto *global = cast<GlobalValue>(found->second);
    assert(global->getParent() == &targetModule &&
           "The CrossModuleInliner can only be used with a single target module");
    return global;
  }

  return nullptr;
}

bool CrossModuleInliner::registerTypeRemapping(llvm::Type *sourceType, llvm::Type *targetType) {
  if (sourceType == targetType)
    return false;
  auto [iter, success] = impl->typeRemapper.mappedTypes.try_emplace(sourceType, targetType);
  return success;
}

llvm::GlobalValue &CrossModuleInliner::defaultGetGlobalInModuleFunc(CrossModuleInliner &inliner,
                                                                    llvm::GlobalValue &sourceGv,
                                                                    llvm::Module &targetModule) {
  inliner.checkTargetModule(targetModule);
  assert(inliner.impl && "Called GetGlobalInModule, but the inliner is currently not inlining anything");

  // Try to find by name
  if (auto *existing = targetModule.getNamedValue(compilerutils::CrossModuleInliner::getCrossModuleName(sourceGv)))
    return *existing;

  auto &mappedTypes = inliner.impl->typeRemapper.mappedTypes;
  auto newName = getCrossModuleName(sourceGv);
  if (auto *callee = dyn_cast<Function>(&sourceGv)) {
    if (!callee->isDeclaration()) {
      report_fatal_error(Twine("Cross module inlining does not support functions with calls to functions with a body. "
                               "Run the inliner before trying to inline across modules (trying to call '") +
                         callee->getName() + "')");
    }

    // FunctionType needs to be mapped outside of the ValueMaterializer to avoid failing when
    // setting the function as an operand in the CallInst in remapInstruction.
    FunctionType *sourceFuncTy = dyn_cast<FunctionType>(callee->getFunctionType());
    SmallVector<Type *> params;
    for (unsigned index = 0; index < sourceFuncTy->getNumParams(); ++index) {
      Type *argTy = sourceFuncTy->getParamType(index);
      Type *mappedTy = argTy;
      if (auto found = mappedTypes.find(mappedTy); found != mappedTypes.end())
        mappedTy = found->second;
      params.push_back(mappedTy);
    }

    Type *returnTy = sourceFuncTy->getReturnType();
    Type *mappedTy = returnTy;
    if (auto found = mappedTypes.find(mappedTy); found != mappedTypes.end())
      mappedTy = found->second;

    // Create a function declaration
    FunctionType *targetFuncTy = FunctionType::get(mappedTy, params, sourceFuncTy->isVarArg());
    auto *newGv = compilerutils::cloneFunctionHeader(*callee, targetFuncTy, callee->getAttributes(), &targetModule);
    newGv->setName(newName);
    return *newGv;
  }

  if (auto *gVar = dyn_cast<GlobalVariable>(&sourceGv)) {
    // Create a global with the correct type
    Type *mappedTy = gVar->getValueType();
    if (auto found = mappedTypes.find(mappedTy); found != mappedTypes.end())
      mappedTy = found->second;
    auto *newGv = new GlobalVariable(targetModule, mappedTy, gVar->isConstant(), gVar->getLinkage(), nullptr, newName,
                                     nullptr, gVar->getThreadLocalMode(), gVar->getAddressSpace());
    newGv->copyAttributesFrom(gVar);
    if (gVar->hasInitializer()) {
      // Recursively map initializer
      auto *newInit = inliner.impl->mapper.mapConstant(*gVar->getInitializer());
      newGv->setInitializer(newInit);
    }
    return *newGv;
  }

  report_fatal_error("Encountered unknown global object while inlining");
}

// Get the name of a global that is copied to a different module for inlining.
std::string CrossModuleInliner::getCrossModuleName(GlobalValue &gv) {
  if (auto *fn = dyn_cast<Function>(&gv)) {
    // Intrinsics should not be renamed since the IR verifier insists on a "correct" name mangling based on any
    // overloaded types. Lgc dialects also require exact name for similar reason.
    if (fn->isIntrinsic() || fn->getName().starts_with("lgc.") || fn->getName().starts_with("llpcfe."))
      return fn->getName().str();
  }
  return (Twine(gv.getName()) + ".cloned." + gv.getParent()->getName()).str();
}

void CrossModuleInliner::checkTargetModule(llvm::Module &targetModule) {
  if (impl->targetMod == nullptr)
    impl->targetMod = &targetModule;
  else
    assert(impl->targetMod == &targetModule);
}

void compilerutils::replaceAllPointerUses(Value *oldPointerValue, Value *newPointerValue,
                                          SmallVectorImpl<Instruction *> &toBeRemoved) {
  // Note: The implementation explicitly supports typed pointers, which
  //       complicates some of the code below.

  // Assert that both types are pointers that only differ in the address space.
  PointerType *oldPtrTy = cast<PointerType>(oldPointerValue->getType());
  (void)oldPtrTy;
  PointerType *newPtrTy = cast<PointerType>(newPointerValue->getType());
  unsigned newAS = newPtrTy->getAddressSpace();

  // If a change of address space is not necessary then simply replace uses.
  if (newAS == oldPtrTy->getAddressSpace()) {
    oldPointerValue->replaceAllUsesWith(newPointerValue);
    return;
  }

  // Propagate a change of address space by traversing through the users and setup the addrspace.

  oldPointerValue->mutateType(newPtrTy);

  SmallVector<Use *> worklist(make_pointer_range(oldPointerValue->uses()));
  oldPointerValue->replaceAllUsesWith(newPointerValue);

#ifndef NDEBUG
  DenseSet<Value *> PhiElems;
#endif

  while (!worklist.empty()) {
    Use *ptrUse = worklist.pop_back_val();
    Value *ptr = cast<Value>(ptrUse);
    Instruction *inst = cast<Instruction>(ptrUse->getUser());
    LLVM_DEBUG(dbgs() << "Visiting " << *inst << '\n');
    // In the switch below, "break" means to continue with replacing
    // the users of the current value, while "continue" means to stop at
    // the current value, and proceed with next one from the work list.
    auto usesRange = make_pointer_range(inst->uses());
    switch (inst->getOpcode()) {
    default:
      LLVM_DEBUG(inst->dump());
      llvm_unreachable("Unhandled instruction\n");
      break;
    case Instruction::Call: {
      if (inst->isLifetimeStartOrEnd()) {
        // The lifetime marker is not useful anymore.
        inst->eraseFromParent();
      } else {
        LLVM_DEBUG(inst->dump());
        llvm_unreachable("Unhandled call instruction\n");
      }
      // No further processing needed for the users.
      continue;
    }
    case Instruction::Load:
    case Instruction::Store:
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
      // No further processing needed for the users.
      continue;
    case Instruction::InsertValue:
      // For insertvalue, there could be 2 cases:
      // Assume %ptr = ptrtoint ... to i32
      // (1) %inserted = insertvalue [2 x i32] poison, i32 %ptr, 0
      // (2) %0 = bitcast i32 %ptr to [2 x i16]
      //     %inserted = insertvalue [2 x i16], i32 1, 0
      // For (1), no further handling is needed; For (2), we are modifying the
      // pointer and need to track all users of %inserted.
      if (cast<InsertValueInst>(inst)->getAggregateOperand() == ptr) {
        break;
      }
      continue;
    case Instruction::And:
    case Instruction::Add:
    case Instruction::PtrToInt:
      break;
    case Instruction::BitCast: {
      // This can happen with typed pointers
      assert(cast<BitCastOperator>(inst)->getSrcTy()->isPointerTy() &&
             cast<BitCastOperator>(inst)->getDestTy()->isPointerTy());
      inst->mutateType(newPtrTy);
      break;
    }
    case Instruction::AddrSpaceCast:
      // Check that the pointer operand has already been fixed
      assert(inst->getOperand(0)->getType()->getPointerAddressSpace() == newAS);
      // Push the correct users before RAUW.
      worklist.append(usesRange.begin(), usesRange.end());
      inst->mutateType(newPtrTy);
      // Since we are mutating the address spaces of users as well,
      // we can just use the (already mutated) cast operand.
      inst->replaceAllUsesWith(inst->getOperand(0));
      toBeRemoved.push_back(inst);
      continue;
    case Instruction::IntToPtr:
    case Instruction::GetElementPtr: {
      inst->mutateType(newPtrTy);
      break;
    }
    case Instruction::Select: {
      auto *oldType = inst->getType();
      if (oldType->isPointerTy()) {
        Type *newType = newPtrTy;
        // No further processing if the type has the correct pointer type
        if (newType == oldType)
          continue;

        inst->mutateType(newType);
      }
      break;
    }
    case Instruction::PHI: {
      auto *oldType = inst->getType();
      if (oldType->isPointerTy()) {
#ifndef NDEBUG
        // Check that all inputs to the phi are handled
        if (!PhiElems.erase(ptr)) {
          // Was not in the map, so add the other elements
          for (auto &incoming : cast<PHINode>(inst)->incoming_values()) {
            if (incoming.get() != ptr) {
              PhiElems.insert(incoming.get());
            }
          }
        }
#endif

        Type *newType = newPtrTy;
        // No further processing if the type has the correct pointer type
        if (newType == oldType)
          continue;

        inst->mutateType(newType);
      }
      break;
    }
    }

    worklist.append(usesRange.begin(), usesRange.end());
  }

#ifndef NDEBUG
  if (!PhiElems.empty()) {
    errs() << "Unhandled inputs to phi: ";
    for (auto *phi : PhiElems) {
      phi->dump();
    }
  }
  assert(PhiElems.empty() && "All phi inputs need to be handled, otherwise we end in an inconsistent state");
#endif
}

Value *compilerutils::simplifyingCreateConstGEP1_32(IRBuilder<> &builder, Type *ty, Value *ptr, uint32_t idx) {
  // A GEP with a single zero index is redundant with opaque pointers
  if (idx == 0)
    return ptr;
  return builder.CreateConstGEP1_32(ty, ptr, idx);
}

Value *compilerutils::simplifyingCreateConstInBoundsGEP1_32(IRBuilder<> &builder, Type *ty, Value *ptr, uint32_t idx) {
  if (idx == 0)
    return ptr;
  return builder.CreateConstInBoundsGEP1_32(ty, ptr, idx);
}

void compilerutils::splitIntoI32(const DataLayout &layout, IRBuilder<> &builder, ArrayRef<Value *> input,
                                 SmallVector<Value *> &output) {
  for (auto *x : input) {
    Type *xType = x->getType();
    if (isa<StructType>(xType)) {
      StructType *structTy = cast<StructType>(xType);
      for (unsigned idx = 0; idx < structTy->getNumElements(); idx++)
        splitIntoI32(layout, builder, builder.CreateExtractValue(x, idx), output);
    } else if (auto *arrayTy = dyn_cast<ArrayType>(xType)) {
      auto *elemTy = arrayTy->getElementType();
      assert(layout.getTypeSizeInBits(elemTy) == 32 && "array of non-32bit type not supported");
      for (unsigned idx = 0; idx < arrayTy->getNumElements(); idx++) {
        auto *elem = builder.CreateExtractValue(x, idx);
        if (!elemTy->isIntegerTy())
          elem = builder.CreateBitCast(elem, builder.getInt32Ty());
        output.push_back(elem);
      }
    } else if (auto *vecTy = dyn_cast<FixedVectorType>(xType)) {
      Type *scalarTy = vecTy->getElementType();
      assert((scalarTy->getPrimitiveSizeInBits() & 0x3) == 0);
      unsigned scalarBytes = scalarTy->getPrimitiveSizeInBits() / 8;
      if (scalarBytes < 4) {
        // Use shufflevector for types like <i8 x 6>?
        llvm_unreachable("vector of type smaller than dword not supported yet.");
      } else {
        for (unsigned idx = 0; idx < (unsigned)vecTy->getNumElements(); idx++)
          splitIntoI32(layout, builder, builder.CreateExtractElement(x, idx), output);
      }
    } else {
      // pointer or primitive types
      assert(xType->isPointerTy() || xType->isIntegerTy() || xType->isFloatTy());
      unsigned size = layout.getTypeSizeInBits(xType).getFixedValue();
      if (xType->isPointerTy())
        x = builder.CreatePtrToInt(x, builder.getIntNTy(size));

      if (size > 32) {
        assert(size % 32 == 0);
        Value *vecDword = builder.CreateBitCast(x, FixedVectorType::get(builder.getInt32Ty(), size / 32));
        splitIntoI32(layout, builder, vecDword, output);
      } else {
        x = builder.CreateZExtOrBitCast(x, builder.getInt32Ty());
        output.push_back(x);
      }
    }
  }
}

std::string compilerutils::bb::getLabel(const Function *func) {
  if (func->hasName())
    return func->getName().str();

  ModuleSlotTracker mst(func->getParent());
  mst.incorporateFunction(*func);

  return std::to_string(mst.getLocalSlot(func));
}

std::string compilerutils::bb::getLabel(const BasicBlock *bb) {
  if (bb->hasName())
    return bb->getName().str();

  const Function *func = bb->getParent();

  ModuleSlotTracker mst(func->getParent());
  mst.incorporateFunction(*func);

  return std::to_string(mst.getLocalSlot(bb));
}

std::string compilerutils::bb::getLabel(const Value *v) {
  if (v->hasName())
    return v->getName().str();

  if (!isa<Instruction>(v))
    return "";

  const BasicBlock *bb = dyn_cast<Instruction>(v)->getParent();
  const Function *func = bb->getParent();

  ModuleSlotTracker mst(func->getParent());
  mst.incorporateFunction(*func);

  return std::to_string(mst.getLocalSlot(v));
}

std::string compilerutils::bb::getNamesForBasicBlocks(const ArrayRef<BasicBlock *> blocks, StringRef emptyRetValue,
                                                      StringRef prefix) {
  std::string s;
  if (blocks.empty())
    return emptyRetValue.str();

  for (auto *bb : blocks)
    s += prefix.str() + getLabel(bb);

  return s;
}

std::string compilerutils::bb::getNamesForBasicBlocks(const SmallSet<BasicBlock *, 2> &blocks, StringRef emptyRetValue,
                                                      StringRef prefix) {
  std::string s;
  if (blocks.empty())
    return emptyRetValue.str();

  for (auto *bb : blocks)
    s += prefix.str() + getLabel(bb);

  return s;
}

void compilerutils::RegisterPasses(llvm::PassBuilder &PB) {
#define HANDLE_PASS(NAME, CREATE_PASS)                                                                                 \
  if (innerPipeline.empty() && name == NAME) {                                                                         \
    passMgr.addPass(CREATE_PASS);                                                                                      \
    return true;                                                                                                       \
  }

  PB.registerPipelineParsingCallback(
      [](StringRef name, ModulePassManager &passMgr, ArrayRef<PassBuilder::PipelineElement> innerPipeline) {
        StringRef Params;
        (void)Params;
#define COMPILERUTILS_PASS HANDLE_PASS
#include "PassRegistry.inc"
        return false;
      });
}
