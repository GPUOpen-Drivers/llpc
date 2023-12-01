/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "compilerutils/TypeLowering.h"
#include "compilerutils/CompilerUtils.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace {

// =====================================================================================================================
// Fallback converter used by all TypeLowering instances for handling aggregate
// types.
//
// @param typeLower : the calling TypeLowering object
// @param ty : the type to be converted
SmallVector<Type *> coreTypeConverter(TypeLowering &typeLower, Type *ty) {
  SmallVector<Type *> result;

  if (auto *arrayTy = dyn_cast<ArrayType>(ty)) {
    Type *elTy = arrayTy->getElementType();
    auto converted = typeLower.convertType(elTy);
    if (converted.size() != 1 || converted[0] != elTy) {
      Type *newElTy;
      if (converted.size() == 1)
        newElTy = converted[0];
      else
        newElTy = StructType::get(elTy->getContext(), converted);
      result.push_back(ArrayType::get(newElTy, arrayTy->getNumElements()));
      return result;
    }
  } else if (auto *structTy = dyn_cast<StructType>(ty)) {
    SmallVector<Type *> newElements;
    newElements.reserve(structTy->getNumElements());

    bool needConversion = false;
    for (Type *elTy : structTy->elements()) {
      auto converted = typeLower.convertType(elTy);
      if (converted.size() != 1 || converted[0] != elTy)
        needConversion = true;
      if (converted.size() == 1) {
        newElements.push_back(converted[0]);
      } else {
        newElements.push_back(StructType::get(structTy->getContext(), converted));
      }
    }
    if (needConversion) {
      assert(!structTy->isPacked());

      if (structTy->isLiteral()) {
        result.push_back(StructType::get(structTy->getContext(), newElements));
      } else {
        result.push_back(StructType::create(structTy->getContext(), newElements, structTy->getName()));
      }
      return result;
    }
  }

  // Since this converter is always called last, we know at this point that the
  // type is not converted.
  result.push_back(ty);
  return result;
}

// =====================================================================================================================
// Fallback converter for constants. Provides default handling for poison,
// undef, and null/zeroinitializer.
//
// @param typeLower : the calling TypeLowering object
// @param constant : the constant to be Converted
// @param types : the types into which the constant is to be converted
SmallVector<Constant *> coreConstantConverter(TypeLowering &typeLower, Constant *constant, ArrayRef<Type *> types) {
  SmallVector<Constant *> result;
  if (isa<PoisonValue>(constant)) {
    for (Type *ty : types)
      result.push_back(PoisonValue::get(ty));
  } else if (isa<UndefValue>(constant)) {
    for (Type *ty : types)
      result.push_back(UndefValue::get(ty));
  } else if (constant->isNullValue()) {
    for (Type *ty : types)
      result.push_back(Constant::getNullValue(ty));
  }
  return result;
}

} // anonymous namespace

// =====================================================================================================================
// Construct a TypeLowering object
//
// @param context : the LLVMContext
TypeLowering::TypeLowering(LLVMContext &context) : m_builder(context) {
  addRule(&coreTypeConverter);
  addConstantRule(&coreConstantConverter);
}

// =====================================================================================================================
// Lower function argument type based on the registered rules. If there is no
// type remapping needed, will just return the old function, otherwise it will
// move all the instructions in the old function to the new function and return
// the new function. So don't operate on the old function if new function was
// returned! The old function will be cleaned up at the time of
// TypeLowering::finishCleanup().
//
Function *TypeLowering::lowerFunctionArguments(Function &fn) {
  SmallVector<Type *> newArgTys;
  SmallVector<unsigned> remappedArgs;
  for (size_t argIdx = 0; argIdx < fn.arg_size(); ++argIdx) {
    auto *arg = fn.getArg(argIdx);
    auto converted = convertType(arg->getType());
    assert(converted.size() == 1 && "Only 1:1 type remapping supported now");
    if (converted[0] == arg->getType()) {
      newArgTys.push_back(arg->getType());
    } else {
      remappedArgs.push_back(argIdx);
      newArgTys.push_back(converted[0]);
    }
  }

  if (remappedArgs.empty())
    return &fn;

  auto *newFn = CompilerUtils::mutateFunctionArguments(fn, fn.getReturnType(), newArgTys, fn.getAttributes());
  fn.replaceAllUsesWith(newFn);
  for (unsigned argIdx : remappedArgs)
    recordValue(fn.getArg(argIdx), {newFn->getArg(argIdx)});

  // Setup names and replace argument uses except the remapped ones.
  // The remapped argument will be handled by later instruction visitor.
  for (unsigned idx = 0; idx < newFn->arg_size(); idx++) {
    Value *oldArg = fn.getArg(idx);
    Value *newArg = newFn->getArg(idx);
    newArg->setName(oldArg->getName());
    if (!llvm::is_contained(remappedArgs, idx))
      oldArg->replaceAllUsesWith(newArg);
  }
  m_functionsToErase.push_back(&fn);
  return newFn;
}

// =====================================================================================================================
// Add a type conversion rule.
//
// Rules are applied in LIFO order.
//
// @param rule : the rule
void TypeLowering::addRule(std::function<TypeLoweringFn> rule) {
  m_rules.push_back(std::move(rule));
}

// =====================================================================================================================
// Add a constant conversion rule.
//
// Rules are applied in LIFO order.
//
// @param rule : the rule
void TypeLowering::addConstantRule(std::function<ConstantTypeLoweringFn> rule) {
  m_constantRules.push_back(std::move(rule));
}

// =====================================================================================================================
// Determine the type(s) that a given type should be converted to.
//
// For types that *shouldn't* be converted, this returns a singleton array whose
// only entry is the given type.
//
// @param ty : the type
ArrayRef<Type *> TypeLowering::convertType(Type *ty) {
  auto unaryIt = m_unaryTypeConversions.find(ty);
  if (unaryIt != m_unaryTypeConversions.end())
    return ArrayRef(unaryIt->second);

  auto multiIt = m_multiTypeConversions.find(ty);
  if (multiIt != m_multiTypeConversions.end())
    return multiIt->second;

  for (const auto &rule : reverse(m_rules)) {
    SmallVector<Type *> types = rule(*this, ty);
    if (types.empty())
      continue;

    if (types.size() == 1)
      return ArrayRef(m_unaryTypeConversions.try_emplace(ty, types[0]).first->second);

    return m_multiTypeConversions.try_emplace(ty, std::move(types)).first->second;
  }

  llvm_unreachable("core/fallback rule should prevent us from reaching this point");
}

// =====================================================================================================================
// Register visitor functions for the class' handling of generic instructions.
//
// @param builder : the VisitorBuilder
void TypeLowering::registerVisitors(llvm_dialects::VisitorBuilder<TypeLowering> &builder) {
  builder.setStrategy(llvm_dialects::VisitorStrategy::ReversePostOrder)
      .add(&TypeLowering::visitAlloca)
      .add(&TypeLowering::visitExtract)
      .add(&TypeLowering::visitInsert)
      .add(&TypeLowering::visitLoad)
      .add(&TypeLowering::visitPhi)
      .add(&TypeLowering::visitSelect)
      .add(&TypeLowering::visitStore);
}

// =====================================================================================================================
// Lookup the mapping of a value that has previously been added.
//
// In typical uses of this helper function, the lookup must be successful since
// instructions are visited in reverse post-order, and phi nodes are fixed up at
// the end. Therefore, this method should be preferred over getValueOptional.
//
// @param val : the value
SmallVector<Value *> TypeLowering::getValue(Value *val) {
  auto values = getValueOptional(val);
  assert(!values.empty());
  return values;
}

// =====================================================================================================================
// Lookup a previously added mapping of a given value.
//
// Return an empty value list if the given value is unknown, val has not been
// converted. Most users should use getValue instead.
//
// Note that constant conversion is invoked on-the-fly as needed.
//
// @param val : the value
SmallVector<Value *> TypeLowering::getValueOptional(Value *val) {
  auto valueIt = m_valueMap.find(val);
  if (valueIt == m_valueMap.end()) {
    auto *constant = dyn_cast<Constant>(val);
    if (!constant)
      return {};

    auto types = convertType(constant->getType());
    if (types.size() == 1 && types[0] == constant->getType())
      return {};

    SmallVector<Value *> converted;
    if (types.size() == 1 && types[0] == constant->getType()) {
      converted.push_back(constant);
    } else {
      for (const auto &rule : reverse(m_constantRules)) {
        SmallVector<Constant *> constants = rule(*this, constant, types);
        if (!constants.empty()) {
          converted.insert(converted.end(), constants.begin(), constants.end());
          break;
        }
      }
      assert(!converted.empty() && "missing constant conversion rule");
    }

    recordValue(val, converted);

    valueIt = m_valueMap.find(val);
    assert(valueIt != m_valueMap.end());
  }

  if ((valueIt->second & 1) == 0) {
    return SmallVector<Value *>(ArrayRef(reinterpret_cast<Value *>(valueIt->second)));
  }

  size_t begin = valueIt->second >> 1;
  auto typeIt = m_multiTypeConversions.find(val->getType());
  assert(typeIt != m_multiTypeConversions.end());
  size_t count = typeIt->second.size();
  return SmallVector<Value *>(ArrayRef(&m_convertedValueList[begin], count));
}

// =====================================================================================================================
// Record that the value produced by the given instruction should be mapped to
// the given new value(s), and that the instruction should be erased.
//
// @param inst : the instruction
// @param mapping : the value(s) that the value defined by the instruction
// should be mapped to
void TypeLowering::replaceInstruction(Instruction *inst, ArrayRef<Value *> mapping) {
  m_instructionsToErase.push_back(inst);

  if (mapping.empty()) {
    assert(inst->getType()->isVoidTy());
    return;
  }

  recordValue(inst, mapping);
}

// =====================================================================================================================
// Record a mapping for a value.
//
// @param val : the value for which a mapping is recorded
// @param mapping : the mapping that is recorded for the value
void TypeLowering::recordValue(Value *val, ArrayRef<Value *> mapping) {
  assert(!m_valueMap.count(val));

  if (mapping.size() == 1) {
    m_valueMap.try_emplace(val, reinterpret_cast<uintptr_t>(mapping[0]));
#ifndef NDEBUG
    auto types = convertType(val->getType());
    assert(types.size() == 1);
    assert(types[0] == mapping[0]->getType());
#endif
    m_valueReverseMap[mapping[0]].emplace_back(reinterpret_cast<uintptr_t>(val));
    return;
  }

  uintptr_t index = m_convertedValueList.size();
  uintptr_t code = (index << 1) | 1;
  m_convertedValueList.insert(m_convertedValueList.end(), mapping.begin(), mapping.end());
  m_valueMap.try_emplace(val, code);
  for (auto e : llvm::enumerate(mapping)) {
    m_valueReverseMap[e.value()].emplace_back(((index + e.index()) << 1) | 1);
  }

  // Unconditionally perform the conversion to ensure that it is available in
  // getValue.
  auto types = convertType(val->getType());
  assert(types.size() == mapping.size());
  for (size_t idx = 0; idx < types.size(); ++idx) {
    assert(types[idx] == mapping[idx]->getType());
  }
}

// =====================================================================================================================
// Record an instruction to be erased at cleanup time.
//
// @param inst : the instruction to be erased
void TypeLowering::eraseInstruction(llvm::Instruction *inst) {
  m_instructionsToErase.push_back(inst);
}

// =====================================================================================================================
// Replace a value that may have previously been recorded as part of a mapping
// with another value.
//
// This can be used if RAUW is performed after the main traversal of the code,
// as in:
// @code
//   toReplace->replaceAllUsesWith(with);
//   typeLower.replaceMappingWith(toReplace, with);
// @endcode
//
// @param toReplace : the mapping value to be replaced
// @param with : the new value to replace it with in all mappings in which
// it appears
void TypeLowering::replaceMappingWith(Value *toReplace, Value *with) {
  if (toReplace == with)
    return;

  auto toReplaceIt = m_valueReverseMap.find(toReplace);
  if (toReplaceIt == m_valueReverseMap.end())
    return;

  SmallVector<uintptr_t> occurrences = std::move(toReplaceIt->second);
  m_valueReverseMap.erase(toReplaceIt);

  for (uintptr_t occurrence : occurrences) {
    if (occurrence & 1) {
      m_convertedValueList[occurrence >> 1] = with;
    } else {
      m_valueMap.find(reinterpret_cast<Value *>(occurrence))->second = reinterpret_cast<uintptr_t>(with);
    }
  }

  auto withIt = m_valueReverseMap.find(with);
  if (withIt != m_valueReverseMap.end()) {
    withIt->second.append(occurrences);
  } else {
    m_valueReverseMap.try_emplace(with, std::move(occurrences));
  }
}

// =====================================================================================================================
// Finalize phi nodes.
//
// This performs some trivial simplifications but does not actually erase the
// old phi nodes yet.
void TypeLowering::finishPhis() {
  // Process phis in reverse order, so that phis from inner loops are handled
  // before phis from outer loops.
  //
  // Trivial phis are simplified on-the-fly. Trivial phis can occur when a value
  // is replaced by a tuple of values and some of the tuple entries are constant
  // across a loop while others aren't.
  for (const auto &[phi, newPhis] : llvm::reverse(m_phis)) {
    // None means no non-self incoming found. nullptr means multiple non-self
    // incomings found.
    SmallVector<std::optional<Value *>> uniqueNonSelfIncomings;
    uniqueNonSelfIncomings.resize(newPhis.size());

    for (const auto &[block, val] : llvm::zip(phi->blocks(), phi->incoming_values())) {
      auto converted = getValue(val);
      for (auto [newPhi, newValue, uniqueNonSelf] : llvm::zip(newPhis, converted, uniqueNonSelfIncomings)) {
        if (newValue != newPhi) {
          if (!uniqueNonSelf.has_value()) {
            uniqueNonSelf.emplace(newValue);
          } else {
            if (uniqueNonSelf.value() != newValue)
              uniqueNonSelf.emplace(nullptr);
          }
        }
        newPhi->addIncoming(newValue, block);
      }
    }

    for (auto [newPhi, uniqueNonSelf] : llvm::zip(newPhis, uniqueNonSelfIncomings)) {
      if (!uniqueNonSelf.has_value()) {
        // This could happen if there is an unreachable infinite loop.
        continue;
      }

      if (Value *replace = uniqueNonSelf.value()) {
        // All incomings are either the phi itself or some unique value. This
        // means that unique value must dominate the phi and so we can just
        // replace it.
        newPhi->replaceAllUsesWith(replace);
        replaceMappingWith(newPhi, replace);
        eraseInstruction(newPhi);
      }
    }

    // Phis may be visited *before* the incoming values, which means that
    // finishCleanup() will attempt to delete some incoming values *before* the
    // phi. Drop all references so that the incoming values can be deleted
    // without issues.
    phi->dropAllReferences();
  }
  m_phis.clear();
}

// =====================================================================================================================
// Erase converted instructions and related cleanup.
bool TypeLowering::finishCleanup() {
  assert(m_phis.empty());

  bool changed = !m_instructionsToErase.empty();

  // We can just erase instructions in reverse order since we added them in
  // reverse post-order.
  for (Instruction *inst : llvm::reverse(m_instructionsToErase))
    inst->eraseFromParent();
  m_instructionsToErase.clear();

  for (Function *fn : m_functionsToErase)
    fn->eraseFromParent();
  m_functionsToErase.clear();

  m_valueMap.clear();
  m_convertedValueList.clear();
  m_valueReverseMap.clear();

  return changed;
}

// =====================================================================================================================
// Visit an alloca instruction
//
// @param alloca : the instruction
void TypeLowering::visitAlloca(AllocaInst &alloca) {
  auto types = convertType(alloca.getAllocatedType());
  if (types.size() == 1 && types[0] == alloca.getAllocatedType())
    return;

  if (types.size() == 1) {
    alloca.setAllocatedType(types[0]);
  } else {
    alloca.setAllocatedType(StructType::get(m_builder.getContext(), types));
  }
}

// =====================================================================================================================
// Visit an extractvalue instruction
//
// @param extract : the instruction
void TypeLowering::visitExtract(ExtractValueInst &extract) {
  auto values = getValueOptional(extract.getAggregateOperand());
  if (values.empty())
    return;

  assert(values.size() == 1);

  m_builder.SetInsertPoint(&extract);
  Value *newExtract = m_builder.CreateExtractValue(values[0], extract.getIndices());
  newExtract->takeName(&extract);

  SmallVector<Value *> converted;
  auto types = convertType(extract.getType());
  if (types.size() == 1) {
    converted.push_back(newExtract);
  } else {
    for (size_t idx = 0; idx < types.size(); ++idx)
      converted.push_back(m_builder.CreateExtractValue(newExtract, idx));
  }

  replaceInstruction(&extract, converted);
}

// =====================================================================================================================
// Visit an insertvalue instruction
//
// @param insert : the instruction
void TypeLowering::visitInsert(llvm::InsertValueInst &insert) {
  auto aggregateValues = getValueOptional(insert.getAggregateOperand());
  if (aggregateValues.empty())
    return;

  assert(aggregateValues.size() == 1);

  m_builder.SetInsertPoint(&insert);

  auto insertedValues = getValueOptional(insert.getInsertedValueOperand());
  Value *insertedValue;
  if (insertedValues.empty()) {
    insertedValue = insert.getInsertedValueOperand();
  } else if (insertedValues.size() == 1) {
    insertedValue = insertedValues[0];
  } else {
    auto types = convertType(insert.getInsertedValueOperand()->getType());
    insertedValue = PoisonValue::get(StructType::get(m_builder.getContext(), types));

    for (auto e : llvm::enumerate(insertedValues)) {
      insertedValue = m_builder.CreateInsertValue(insertedValue, e.value(), e.index());
    }
  }

  Value *newInsert = m_builder.CreateInsertValue(aggregateValues[0], insertedValue, insert.getIndices());
  newInsert->takeName(&insert);

  replaceInstruction(&insert, newInsert);
}

// =====================================================================================================================
// Visit a load instruction
//
// @param load : the instruction
void TypeLowering::visitLoad(LoadInst &load) {
  auto types = convertType(load.getType());
  if (types.size() == 1 && types[0] == load.getType())
    return;

  m_builder.SetInsertPoint(&load);

  Type *loadType;
  if (types.size() == 1) {
    loadType = types[0];
  } else {
    loadType = StructType::get(m_builder.getContext(), types);
  }

  // We create an entirely new load instruction and explicitly make no attempt
  // to preserve any assorted data like alignment, atomicity, and metadata.
  // Since we are replacing the load of a likely "opaque" type whose size (as
  // far as LLVM is concerned) may not even match its replacement, any such data
  // is most likely useless at best and incorrect at worst. We should eventually
  // figure out how to handle this properly, but it likely means LLVM accepting
  // the notion of "opaque" Types to some extent.
  Value *data = m_builder.CreateLoad(loadType, load.getPointerOperand());
  data->takeName(&load);

  SmallVector<Value *> converted;
  if (types.size() == 1) {
    converted.push_back(data);
  } else {
    for (size_t idx = 0; idx < types.size(); ++idx)
      converted.push_back(m_builder.CreateExtractValue(data, idx));
  }

  replaceInstruction(&load, converted);
}

// =====================================================================================================================
// Visit a phi node
//
// @param phi : the instruction
void TypeLowering::visitPhi(PHINode &phi) {
  auto types = convertType(phi.getType());
  if (types.size() == 1 && types[0] == phi.getType())
    return;

  m_builder.SetInsertPoint(&phi);

  SmallVector<Value *> converted;
  SmallVector<PHINode *> newPhis;
  for (auto e : llvm::enumerate(types)) {
    PHINode *newPhi =
        m_builder.CreatePHI(e.value(), phi.getNumIncomingValues(), Twine(phi.getName()) + "." + Twine(e.index()));
    converted.push_back(newPhi);
    newPhis.push_back(newPhi);
  }

  replaceInstruction(&phi, converted);
  m_phis.emplace_back(&phi, std::move(newPhis));
}

// =====================================================================================================================
// Visit a select instruction
//
// @param select : the instruction
void TypeLowering::visitSelect(SelectInst &select) {
  auto trueValues = getValueOptional(select.getTrueValue());
  if (trueValues.empty())
    return;

  auto falseValues = getValueOptional(select.getFalseValue());
  assert(trueValues.size() == falseValues.size());

  m_builder.SetInsertPoint(&select);

  SmallVector<Value *> converted;
  for (auto e : llvm::enumerate(llvm::zip(trueValues, falseValues))) {
    Value *trueValue = std::get<0>(e.value());
    Value *falseValue = std::get<1>(e.value());

    // Simplify selects on the fly. This is relevant when a value is Converted
    // into a tuple of values, where some entries of the tuple may be more
    // likely to be constant than others.
    if (isa<PoisonValue>(trueValue) || isa<UndefValue>(trueValue))
      trueValue = falseValue;
    else if (isa<PoisonValue>(falseValue) || isa<UndefValue>(falseValue))
      falseValue = trueValue;

    if (trueValue == falseValue) {
      converted.push_back(trueValue);
    } else {
      converted.push_back(m_builder.CreateSelect(select.getCondition(), trueValue, falseValue,
                                                 Twine(select.getName()) + "." + Twine(e.index())));
    }
  }
  replaceInstruction(&select, converted);
}

// =====================================================================================================================
// Visit a store instruction
//
// @param store : the instruction
void TypeLowering::visitStore(StoreInst &store) {
  auto values = getValueOptional(store.getValueOperand());
  if (values.empty())
    return;

  m_builder.SetInsertPoint(&store);

  Value *data;
  if (values.size() == 1) {
    data = values[0];
  } else {
    Type *storeTy = StructType::get(m_builder.getContext(), convertType(store.getValueOperand()->getType()));
    data = PoisonValue::get(storeTy);
    for (auto e : llvm::enumerate(values))
      data = m_builder.CreateInsertValue(data, e.value(), e.index());
  }

  // We create an entirely new store instruction and explicitly make no attempt
  // to preserve any assorted data like alignment, atomicity, and metadata.
  // Since we are replacing the load of a likely "opaque" type whose size (as
  // far as LLVM is concerned) may not even match its replacement, any such data
  // is most likely useless at best and incorrect at worst. We should eventually
  // figure out how to handle this properly, but it likely means LLVM accepting
  // the notion of "opaque" Types to some extent.
  m_builder.CreateStore(data, store.getPointerOperand());

  replaceInstruction(&store, {});
}
