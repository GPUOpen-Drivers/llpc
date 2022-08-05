/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderBase.cpp
 * @brief LLPC source file: implementation of BuilderBase
 ***********************************************************************************************************************
 */
#include "lgc/util/BuilderBase.h"
#include "lgc/state/Defs.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Set the insert point to be just past the initial block of allocas in the given function's entry block.
//
// Use this method if you need to insert code to define values that are accessible in the entire function.
void BuilderCommon::setInsertPointPastAllocas(Function &fn) {
  BasicBlock &bb = fn.getEntryBlock();
  auto it = bb.begin(), end = bb.end();
  while (it != end && (isa<AllocaInst>(*it) || isa<DbgInfoIntrinsic>(*it)))
    ++it;
  SetInsertPoint(&bb, it);
}

// =====================================================================================================================
// Create an LLVM function call to the named function. The callee is built automatically based on return
// type and its parameters.
//
// @param funcName : Name of the callee
// @param retTy : Return type of the callee
// @param args : Arguments to pass to the callee
// @param attribs : Function attributes
// @param instName : Name to give instruction
CallInst *BuilderCommon::CreateNamedCall(StringRef funcName, Type *retTy, ArrayRef<Value *> args,
                                         ArrayRef<Attribute::AttrKind> attribs, const Twine &instName) {
  Module *module = GetInsertBlock()->getParent()->getParent();
  Function *func = dyn_cast_or_null<Function>(module->getFunction(funcName));
  if (!func) {
    SmallVector<Type *, 8> argTys;
    argTys.reserve(args.size());
    for (auto arg : args)
      argTys.push_back(arg->getType());

    auto funcTy = FunctionType::get(retTy, argTys, false);
    func = Function::Create(funcTy, GlobalValue::ExternalLinkage, funcName, module);

    func->setCallingConv(CallingConv::C);
    func->addFnAttr(Attribute::NoUnwind);

    for (auto attrib : attribs)
      func->addFnAttr(attrib);
  }

  auto call = CreateCall(func, args, instName);
  call->setCallingConv(CallingConv::C);
  call->setAttributes(func->getAttributes());

  return call;
}

// =====================================================================================================================
// Emits a amdgcn.reloc.constant intrinsic that represents a relocatable i32 value with the given symbol name
//
// @param symbolName : Name of the relocation symbol associated with this relocation
Value *BuilderBase::CreateRelocationConstant(const Twine &symbolName) {
  auto mdNode = MDNode::get(getContext(), MDString::get(getContext(), symbolName.str()));
  return CreateIntrinsic(Intrinsic::amdgcn_reloc_constant, {}, {MetadataAsValue::get(getContext(), mdNode)});
}

// =====================================================================================================================
// Generate an add of an offset to a byte pointer. This is provided to use in the case that the offset is,
// or might be, a relocatable value, as it implements a workaround to get more efficient code for the load
// that uses the offset pointer.
//
// @param pointer : Pointer to add to
// @param byteOffset : Byte offset to add
// @param instName : Name to give instruction
Value *BuilderBase::CreateAddByteOffset(Value *pointer, Value *byteOffset, const Twine &instName) {
  if (auto call = dyn_cast<CallInst>(byteOffset)) {
    if (call->getIntrinsicID() == Intrinsic::amdgcn_reloc_constant) {
      // Where the offset is the result of CreateRelocationConstant,
      // LLVM's internal handling of GEP instruction results in a lot of junk code and prevented selection
      // of the offset-from-register variant of the s_load_dwordx4 instruction. To workaround this issue,
      // we use integer arithmetic here so the amdgpu backend can pickup the optimal instruction.
      // TODO: Figure out how to fix this properly, then remove this function and switch its users to
      // use a simple CreateGEP instead.
      Type *origPointerTy = pointer->getType();
      pointer = CreatePtrToInt(pointer, getInt64Ty());
      pointer = CreateAdd(pointer, CreateZExt(byteOffset, getInt64Ty()), instName);
      return CreateIntToPtr(pointer, origPointerTy);
    }
  }

  return CreateGEP(getInt8Ty(), pointer, byteOffset, instName);
}

// =====================================================================================================================
// Create a map to i32 function. Many AMDGCN intrinsics only take i32's, so we need to massage input data into an i32
// to allow us to call these intrinsics. This helper takes a function pointer, massage arguments, and passthrough
// arguments and massages the mappedArgs into i32's before calling the function pointer. Note that all massage
// arguments must have the same type.
//
// @param mapFunc : The function to call on each provided i32.
// @param mappedArgs : The arguments to be massaged into i32's and passed to function.
// @param passthroughArgs : The arguments to be passed through as is (no massaging).
Value *BuilderBase::CreateMapToInt32(MapToInt32Func mapFunc, ArrayRef<Value *> mappedArgs,
                                     ArrayRef<Value *> passthroughArgs) {
  // We must have at least one argument to massage.
  assert(mappedArgs.size() > 0);

  Type *const type = mappedArgs[0]->getType();

  // Check the massage types all match.
  for (unsigned i = 1; i < mappedArgs.size(); i++)
    assert(mappedArgs[i]->getType() == type);

  if (mappedArgs[0]->getType()->isVectorTy()) {
    // For vectors we extract each vector component and map them individually.
    const unsigned compCount = cast<FixedVectorType>(type)->getNumElements();

    SmallVector<Value *, 4> results;

    for (unsigned i = 0; i < compCount; i++) {
      SmallVector<Value *, 4> newMappedArgs;

      for (Value *const mappedArg : mappedArgs)
        newMappedArgs.push_back(CreateExtractElement(mappedArg, i));

      results.push_back(CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs));
    }

    Value *result = UndefValue::get(FixedVectorType::get(results[0]->getType(), compCount));

    for (unsigned i = 0; i < compCount; i++)
      result = CreateInsertElement(result, results[i], i);

    return result;
  }
  if (type->isIntegerTy() && type->getIntegerBitWidth() == 1) {
    SmallVector<Value *, 4> newMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      newMappedArgs.push_back(CreateZExt(mappedArg, getInt32Ty()));

    Value *const result = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);
    return CreateTrunc(result, getInt1Ty());
  }
  if (type->isIntegerTy() && type->getIntegerBitWidth() < 32) {
    SmallVector<Value *, 4> newMappedArgs;

    Type *const vectorType = FixedVectorType::get(type, type->getPrimitiveSizeInBits() == 16 ? 2 : 4);
    Value *const undef = UndefValue::get(vectorType);

    for (Value *const mappedArg : mappedArgs) {
      Value *const newMappedArg = CreateInsertElement(undef, mappedArg, static_cast<uint64_t>(0));
      newMappedArgs.push_back(CreateBitCast(newMappedArg, getInt32Ty()));
    }

    Value *const result = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);
    return CreateExtractElement(CreateBitCast(result, vectorType), static_cast<uint64_t>(0));
  }
  if (type->getPrimitiveSizeInBits() == 64) {
    SmallVector<Value *, 4> castMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      castMappedArgs.push_back(CreateBitCast(mappedArg, FixedVectorType::get(getInt32Ty(), 2)));

    Value *result = UndefValue::get(castMappedArgs[0]->getType());

    for (unsigned i = 0; i < 2; i++) {
      SmallVector<Value *, 4> newMappedArgs;

      for (Value *const castMappedArg : castMappedArgs)
        newMappedArgs.push_back(CreateExtractElement(castMappedArg, i));

      Value *const resultComp = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);

      result = CreateInsertElement(result, resultComp, i);
    }

    return CreateBitCast(result, type);
  }
  if (type->isFloatingPointTy()) {
    SmallVector<Value *, 4> newMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      newMappedArgs.push_back(CreateBitCast(mappedArg, getIntNTy(mappedArg->getType()->getPrimitiveSizeInBits())));

    Value *const result = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);
    return CreateBitCast(result, type);
  }
  if (type->isIntegerTy(32))
    return mapFunc(*this, mappedArgs, passthroughArgs);
  llvm_unreachable("Should never be called!");
  return nullptr;
}

// =====================================================================================================================
// Create an inline assembly call to cause a side effect (used to work around miscompiles with convergent).
//
// @param value : The value to ensure doesn't move in control flow.
Value *BuilderBase::CreateInlineAsmSideEffect(Value *const value) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    Value *const value = mappedArgs[0];
    Type *const type = value->getType();
    FunctionType *const funcType = FunctionType::get(type, type, false);
    InlineAsm *const inlineAsm = InlineAsm::get(funcType, "; %1", "=v,0", true);
    return builder.CreateCall(inlineAsm, value);
  };

  return CreateMapToInt32(mapFunc, value, {});
}

// =====================================================================================================================
// Create a call to set inactive. Both active and inactive should have the same type.
//
// @param active : The value active invocations should take.
// @param inactive : The value inactive invocations should take.
Value *BuilderBase::CreateSetInactive(Value *active, Value *inactive) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    Value *const active = mappedArgs[0];
    Value *const inactive = mappedArgs[1];
    return builder.CreateIntrinsic(Intrinsic::amdgcn_set_inactive, active->getType(), {active, inactive});
  };

  return CreateMapToInt32(mapFunc, {CreateInlineAsmSideEffect(active), inactive}, {});
}

#if defined(LLVM_HAVE_BRANCH_AMD_GFX)
// =====================================================================================================================
// For a non-uniform input, try and trace back through a descriptor load to
// find the non-uniform index used in it. If that fails, we just use the
// operand value as the index.
//
// Note: this code has to cope with relocs as well, this is why we have to
// have a worklist of instructions to trace back
// through. Something like this:
// %1 = call .... @lgc.descriptor.set(...)          ;; Known uniform base
// %2 = call .... @llvm.amdgcn.reloc.constant(...)  ;; Known uniform reloc constant
// %3 = ptrtoint ... %1 to i64
// %4 = zext ... %2 to i64
// %5 = add i64 %3, %4
// %6 = inttoptr i64 %5 to ....
// %7 = bitcast .... %6 to ....
// %8 = getelementptr .... %7, i64 %offset
//
// As long as the base pointer %7 can be traced back to a descriptor set and
// reloc we can infer that it is truly uniform and use the gep index as the waterfall index safely.
//
// @param nonUniformVal : Value representing non-uniform descriptor
// @return : Value representing the non-uniform index
static Value *traceNonUniformIndex(Value *nonUniformVal) {
  auto load = dyn_cast<LoadInst>(nonUniformVal);
  if (!load) {
    // Workarounds that modify image descriptor can be peeped through, i.e.
    //   %baseValue = load <8 x i32>, <8 x i32> addrspace(4)* %..., align 16
    //   %rawElement = extractelement <8 x i32> %baseValue, i64 6
    //   %updatedElement = and i32 %rawElement, -1048577
    //   %nonUniform = insertelement <8 x i32> %baseValue, i32 %updatedElement, i64 6
    auto insert = dyn_cast<InsertElementInst>(nonUniformVal);
    if (!insert)
      return nonUniformVal;

    load = dyn_cast<LoadInst>(insert->getOperand(0));
    if (!load)
      return nonUniformVal;

    // We found the load, but must verify the chain.
    // Consider updatedElement as a generic instruction or constant.
    if (auto updatedElement = dyn_cast<Instruction>(insert->getOperand(1))) {
      for (Value *operand : updatedElement->operands()) {
        if (auto extract = dyn_cast<ExtractElementInst>(operand)) {
          // Only dynamic value must be ExtractElementInst based on load.
          if (dyn_cast<LoadInst>(extract->getOperand(0)) != load)
            return nonUniformVal;
        } else if (!isa<Constant>(operand)) {
          return nonUniformVal;
        }
      }
    } else if (!isa<Constant>(insert->getOperand(1))) {
      return nonUniformVal;
    }
  }

  SmallVector<Value *, 2> worklist;
  Value *base = load->getOperand(0);
  Value *index = nullptr;

  // Loop until a descriptor table reference or unexpected operation is reached.
  // In the worst case this may visit all instructions in a function.
  for (;;) {
    if (auto bitcast = dyn_cast<BitCastInst>(base)) {
      base = bitcast->getOperand(0);
      continue;
    }
    if (auto gep = dyn_cast<GetElementPtrInst>(base)) {
      if (gep->hasAllConstantIndices()) {
        base = gep->getPointerOperand();
        continue;
      }
      // Variable GEP, to provide the index for the waterfall.
      if (index || gep->getNumIndices() != 1)
        break;
      index = *gep->idx_begin();
      base = gep->getPointerOperand();
      continue;
    }
    if (auto extract = dyn_cast<ExtractValueInst>(base)) {
      if (extract->getIndices().size() == 1 && extract->getIndices()[0] == 0) {
        base = extract->getAggregateOperand();
        continue;
      }
      break;
    }
    if (auto insert = dyn_cast<InsertValueInst>(base)) {
      if (insert->getIndices()[0] != 0) {
        base = insert->getAggregateOperand();
        continue;
      }
      if (insert->getIndices().size() == 1 && insert->getIndices()[0] == 0) {
        base = insert->getInsertedValueOperand();
        continue;
      }
      break;
    }
    if (auto intToPtr = dyn_cast<IntToPtrInst>(base)) {
      base = intToPtr->getOperand(0);
      continue;
    }
    if (auto ptrToInt = dyn_cast<PtrToIntInst>(base)) {
      base = ptrToInt->getOperand(0);
      continue;
    }
    if (auto zExt = dyn_cast<ZExtInst>(base)) {
      base = zExt->getOperand(0);
      continue;
    }
    if (auto call = dyn_cast<CallInst>(base)) {
      if (index) {
        if (auto calledFunc = call->getCalledFunction()) {
          if (calledFunc->getName().startswith(lgcName::DescriptorTableAddr) ||
              calledFunc->getName().startswith("llvm.amdgcn.reloc.constant")) {
            if (!worklist.empty()) {
              base = worklist.pop_back_val();
              continue;
            }
            nonUniformVal = index;
            break;
          }
        }
      }
    }
    if (auto addInst = dyn_cast<Instruction>(base)) {
      // In this case we have to trace back both operands
      // Set one to base for continued processing and put the other onto the worklist
      // Give up if the worklist already has an entry - too complicated
      if (addInst->isBinaryOp() && addInst->getOpcode() == Instruction::BinaryOps::Add) {
        if (!worklist.empty())
          break;
        base = addInst->getOperand(0);
        worklist.push_back(addInst->getOperand(1));
        continue;
      }
    }
    break;
  }

  return nonUniformVal;
}

// =====================================================================================================================
// Test whether two instructions are identical
// or are the same operation on identical operands.
// @param lhs : First instruction
// @param rhs : Second instruction
// @return Result of equally test
static bool instructionsEqual(Instruction *lhs, Instruction *rhs) {
  if (lhs->isIdenticalTo(rhs))
    return true;

  if (!lhs->isSameOperationAs(rhs))
    return false;

  for (unsigned idx = 0, end = lhs->getNumOperands(); idx != end; ++idx) {
    Value *lhsVal = lhs->getOperand(idx);
    Value *rhsVal = rhs->getOperand(idx);
    if (lhsVal == rhsVal)
      continue;
    Instruction *lhsInst = dyn_cast<Instruction>(lhsVal);
    Instruction *rhsInst = dyn_cast<Instruction>(rhsVal);
    if (!lhsInst || !rhsInst)
      return false;
    if (!lhsInst->isIdenticalTo(rhsInst))
      return false;
  }

  return true;
}
#endif

// =====================================================================================================================
// Create a waterfall loop containing the specified instruction.
// This does not use the current insert point; new code is inserted before and after pNonUniformInst.
//
// @param nonUniformInst : The instruction to put in a waterfall loop
// @param operandIdxs : The operand index/indices for non-uniform inputs that need to be uniform
// @param instName : Name to give instruction(s)
Instruction *BuilderBase::createWaterfallLoop(Instruction *nonUniformInst, ArrayRef<unsigned> operandIdxs,
                                              bool scalarizeDescriptorLoads, const Twine &instName) {
#if !defined(LLVM_HAVE_BRANCH_AMD_GFX)
#warning[!amd-gfx] Waterfall feature disabled
  errs() << "Generating invalid waterfall loop code\n";
  return nonUniformInst;
#else
  assert(operandIdxs.empty() == false);

  SmallVector<Value *, 2> nonUniformIndices;
  for (unsigned operandIdx : operandIdxs) {
    Value *nonUniformIndex = traceNonUniformIndex(nonUniformInst->getOperand(operandIdx));
    nonUniformIndices.push_back(nonUniformIndex);
  }

  // For any index that is 64 bit, change it back to 32 bit for comparison at the top of the
  // waterfall loop.
  for (Value *&nonUniformVal : nonUniformIndices) {
    if (nonUniformVal->getType()->isIntegerTy(64)) {
      auto sExt = dyn_cast<SExtInst>(nonUniformVal);
      // 64-bit index may already be formed from extension of 32-bit value.
      if (sExt && sExt->getOperand(0)->getType()->isIntegerTy(32)) {
        nonUniformVal = sExt->getOperand(0);
      } else {
        nonUniformVal = CreateTrunc(nonUniformVal, getInt32Ty());
      }
    }
  }

  // Find first index instruction and check if index instructions are identical.
  Instruction *firstIndexInst = nullptr;
  bool identicalIndexes = true;
  for (Value *nonUniformVal : nonUniformIndices) {
    Instruction *nuInst = dyn_cast<Instruction>(nonUniformVal);
    if (!nuInst || (firstIndexInst && !instructionsEqual(nuInst, firstIndexInst))) {
      identicalIndexes = false;
      break;
    }
    if (!firstIndexInst || nuInst->comesBefore(firstIndexInst))
      firstIndexInst = nuInst;
  }

  // Save Builder's insert point
  auto savedInsertPoint = saveIP();

  Value *waterfallBegin;
  if (scalarizeDescriptorLoads && firstIndexInst && identicalIndexes) {
    // Attempt to scalarize descriptor loads.

    // Begin waterfall loop just after shared index is computed.
    // This places all dependent instructions within the waterfall loop, including descriptor loads.
    auto nonUniformVal = cast<Value>(firstIndexInst);
    SetInsertPoint(firstIndexInst->getNextNonDebugInstruction(false));
    waterfallBegin = ConstantInt::get(getInt32Ty(), 0);
    waterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, nonUniformVal->getType(),
                                     {waterfallBegin, nonUniformVal}, nullptr, instName);

    // Scalarize shared index.
    auto descTy = nonUniformVal->getType();
    Value *desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy},
                                  {waterfallBegin, nonUniformVal}, nullptr, instName);

    // Replace all references to shared index within the waterfall loop with scalarized index.
    // (Note: this includes the non-uniform instruction itself.)
    // Loads using scalarized index will become scalar loads.
    for (Value *otherNonUniformVal : nonUniformIndices) {
      otherNonUniformVal->replaceUsesWithIf(desc, [desc, waterfallBegin, nonUniformInst](Use &U) {
        Instruction *userInst = cast<Instruction>(U.getUser());
        return U.getUser() != waterfallBegin && U.getUser() != desc &&
               (userInst->comesBefore(nonUniformInst) || userInst == nonUniformInst);
      });
    }
  } else {
    // Insert new code just before pNonUniformInst.
    SetInsertPoint(nonUniformInst);

    // The first begin contains a null token for the previous token argument
    waterfallBegin = ConstantInt::get(getInt32Ty(), 0);
    for (auto nonUniformVal : nonUniformIndices) {
      // Start the waterfall loop using the waterfall index.
      waterfallBegin = CreateIntrinsic(Intrinsic::amdgcn_waterfall_begin, nonUniformVal->getType(),
                                       {waterfallBegin, nonUniformVal}, nullptr, instName);
    }

    // Scalarize each non-uniform operand of the instruction.
    for (unsigned operandIdx : operandIdxs) {
      Value *desc = nonUniformInst->getOperand(operandIdx);
      auto descTy = desc->getType();
      desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_readfirstlane, {descTy, descTy}, {waterfallBegin, desc},
                             nullptr, instName);
      if (nonUniformInst->getType()->isVoidTy()) {
        // The buffer/image operation we are waterfalling is a store with no return value. Use
        // llvm.amdgcn.waterfall.last.use on the descriptor.
        desc = CreateIntrinsic(Intrinsic::amdgcn_waterfall_last_use, descTy, {waterfallBegin, desc}, nullptr, instName);
      }
      // Replace the descriptor operand in the buffer/image operation.
      nonUniformInst->setOperand(operandIdx, desc);
    }
  }

  Instruction *resultValue = nonUniformInst;

  // End the waterfall loop (as long as pNonUniformInst is not a store with no result).
  if (!nonUniformInst->getType()->isVoidTy()) {
    SetInsertPoint(nonUniformInst->getNextNode());
    SetCurrentDebugLocation(nonUniformInst->getDebugLoc());

    Use *useOfNonUniformInst = nullptr;
    Type *waterfallEndTy = resultValue->getType();
    if (auto vecTy = dyn_cast<FixedVectorType>(waterfallEndTy)) {
      if (vecTy->getElementType()->isIntegerTy(8)) {
        // ISel does not like waterfall.end with vector of i8 type, so cast if necessary.
        assert((vecTy->getNumElements() % 4) == 0);
        waterfallEndTy = getInt32Ty();
        if (vecTy->getNumElements() != 4)
          waterfallEndTy = FixedVectorType::get(getInt32Ty(), vecTy->getNumElements() / 4);
        resultValue = cast<Instruction>(CreateBitCast(resultValue, waterfallEndTy, instName));
        useOfNonUniformInst = &resultValue->getOperandUse(0);
      }
    }
    resultValue = CreateIntrinsic(Intrinsic::amdgcn_waterfall_end, waterfallEndTy, {waterfallBegin, resultValue},
                                  nullptr, instName);
    if (!useOfNonUniformInst)
      useOfNonUniformInst = &resultValue->getOperandUse(1);
    if (waterfallEndTy != nonUniformInst->getType())
      resultValue = cast<Instruction>(CreateBitCast(resultValue, nonUniformInst->getType(), instName));

    // Replace all uses of pNonUniformInst with the result of this code.
    *useOfNonUniformInst = UndefValue::get(nonUniformInst->getType());
    nonUniformInst->replaceAllUsesWith(resultValue);
    *useOfNonUniformInst = nonUniformInst;
  }

  // Restore Builder's insert point.
  restoreIP(savedInsertPoint);
  return resultValue;
#endif
}
