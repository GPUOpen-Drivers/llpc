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
/**
 ***********************************************************************************************************************
 * @file  BuilderBase.cpp
 * @brief LLPC source file: implementation of BuilderBase
 ***********************************************************************************************************************
 */

#include "lgc/util/BuilderBase.h"
#include "lgc/LgcDialect.h"
#include "lgc/state/IntrinsDefs.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Return the i64 difference between two pointers, dividing out the size of the pointed-to objects.
// For buffer fat pointers, delays the translation to patch phase.
//
// @param ty : Element type of the pointers.
// @param lhs : Left hand side of the subtraction.
// @param rhs : Reft hand side of the subtraction.
// @param instName : Name to give instruction(s)
// @return : the difference between the two pointers, in units of the given type
Value *BuilderCommon::CreatePtrDiff(Type *ty, Value *lhs, Value *rhs, const Twine &instName) {
  Type *const lhsType = lhs->getType();
  Type *const rhsType = rhs->getType();
  if (!lhsType->isPointerTy() || lhsType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER ||
      !rhsType->isPointerTy() || rhsType->getPointerAddressSpace() != ADDR_SPACE_BUFFER_FAT_POINTER)
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 412285
    // Old version of the code
    return IRBuilderBase::CreatePtrDiff(lhs, rhs, instName);
#else
    // New version of the code (also handles unknown version, which we treat as latest)
    return IRBuilderBase::CreatePtrDiff(ty, lhs, rhs, instName);
#endif

  Value *difference = create<BufferPtrDiffOp>(lhs, rhs);
  return CreateExactSDiv(difference, ConstantExpr::getSizeOf(ty), instName);
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
  assert(!funcName.empty());
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
// Create a map to simple type function. Many AMDGCN intrinsics only take MapToSimpleTypeMode, so we need to massage
// input data into a simple type mode to allow us to call these intrinsics. This helper takes a function pointer,
// massage arguments, and passthrough arguments and massages the mappedArgs into simple type mode before calling the
// function pointer. Note that all massage arguments must have the same type.
//
// @param mapFunc : The function to call on each provided simple type mode.
// @param mappedArgs : The arguments to be massaged into simple type mode and passed to function.
// @param passthroughArgs : The arguments to be passed through as is (no massaging).
// @param simpleMode : The arguments to specify the simple type mode
Value *BuilderBase::CreateMapToSimpleType(MapToSimpleTypeFunc mapFunc, ArrayRef<Value *> mappedArgs,
                                          ArrayRef<Value *> passthroughArgs, MapToSimpleMode simpleMode) {
  // We must have at least one argument to massage.
  assert(mappedArgs.size() > 0);

  Type *const type = mappedArgs[0]->getType();

  // Check the massage types all match.
  for (unsigned i = 1; i < mappedArgs.size(); i++)
    assert(mappedArgs[i]->getType() == type);

  if (type->isStructTy()) {
    assert(simpleMode == MapToSimpleMode::SimpleVector);
    // For struct we extract each member and map them individually.
    const unsigned memberCount = type->getStructNumElements();
    SmallVector<Value *> results;
    for (unsigned i = 0; i < memberCount; ++i) {
      SmallVector<Value *, 4> newMappedArgs;
      for (Value *const mappedArg : mappedArgs)
        newMappedArgs.push_back(CreateExtractValue(mappedArg, i));

      results.push_back(CreateMapToSimpleType(mapFunc, newMappedArgs, passthroughArgs, MapToSimpleMode::SimpleVector));
    }

    Value *result = PoisonValue::get(type);
    for (unsigned i = 0; i < memberCount; ++i)
      result = CreateInsertValue(result, results[i], i);

    return result;
  }
  if (type->isVectorTy()) {
    if (simpleMode == MapToSimpleMode::Int32) {
      // For vectors we extract each vector component and map them individually.
      const unsigned compCount = cast<FixedVectorType>(type)->getNumElements();

      SmallVector<Value *, 4> results;

      for (unsigned i = 0; i < compCount; i++) {
        SmallVector<Value *, 4> newMappedArgs;

        for (Value *const mappedArg : mappedArgs)
          newMappedArgs.push_back(CreateExtractElement(mappedArg, i));

        results.push_back(CreateMapToSimpleType(mapFunc, newMappedArgs, passthroughArgs));
      }

      Value *result = PoisonValue::get(FixedVectorType::get(results[0]->getType(), compCount));

      for (unsigned i = 0; i < compCount; i++)
        result = CreateInsertElement(result, results[i], i);

      return result;
    } else if (simpleMode == MapToSimpleMode::SimpleVector) {
      return mapFunc(*this, mappedArgs, passthroughArgs);
    } else {
      llvm_unreachable("Unhandled simple mode");
    }
  }
  if (type->isIntegerTy() && type->getIntegerBitWidth() == 1) {
    SmallVector<Value *, 4> newMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      newMappedArgs.push_back(CreateZExt(mappedArg, getInt32Ty()));

    Value *const result = CreateMapToSimpleType(mapFunc, newMappedArgs, passthroughArgs);
    return CreateTrunc(result, getInt1Ty());
  }
  if (type->isIntegerTy() && type->getIntegerBitWidth() < 32) {
    SmallVector<Value *, 4> newMappedArgs;

    Type *const vectorType = FixedVectorType::get(type, type->getPrimitiveSizeInBits() == 16 ? 2 : 4);
    Value *const poison = PoisonValue::get(vectorType);

    for (Value *const mappedArg : mappedArgs) {
      Value *const newMappedArg = CreateInsertElement(poison, mappedArg, static_cast<uint64_t>(0));
      newMappedArgs.push_back(CreateBitCast(newMappedArg, getInt32Ty()));
    }

    Value *const result = CreateMapToSimpleType(mapFunc, newMappedArgs, passthroughArgs);
    return CreateExtractElement(CreateBitCast(result, vectorType), static_cast<uint64_t>(0));
  }
  if (type->getPrimitiveSizeInBits() == 64) {
    SmallVector<Value *, 4> castMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      castMappedArgs.push_back(CreateBitCast(mappedArg, FixedVectorType::get(getInt32Ty(), 2)));

    Value *result = PoisonValue::get(castMappedArgs[0]->getType());

    for (unsigned i = 0; i < 2; i++) {
      SmallVector<Value *, 4> newMappedArgs;

      for (Value *const castMappedArg : castMappedArgs)
        newMappedArgs.push_back(CreateExtractElement(castMappedArg, i));

      Value *const resultComp = CreateMapToSimpleType(mapFunc, newMappedArgs, passthroughArgs);

      result = CreateInsertElement(result, resultComp, i);
    }

    return CreateBitCast(result, type);
  }
  if (type->isFloatingPointTy()) {
    SmallVector<Value *, 4> newMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      newMappedArgs.push_back(CreateBitCast(mappedArg, getIntNTy(mappedArg->getType()->getPrimitiveSizeInBits())));

    Value *const result = CreateMapToSimpleType(mapFunc, newMappedArgs, passthroughArgs);
    return CreateBitCast(result, type);
  }
  if (type->isIntegerTy(32))
    return mapFunc(*this, mappedArgs, passthroughArgs);
  llvm_unreachable("Should never be called!");
  return nullptr;
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

  return CreateMapToSimpleType(mapFunc, {active, inactive}, {});
}

// =====================================================================================================================
// Create a waterfall end intrinsic.
//
// @param nonUniform: The instruction to put in a end waterfall loop.
// @param waterfallBegin: The waterfall begin intrinsic.
Instruction *BuilderBase::CreateWaterfallEnd(Value *nonUniform, Value *waterfallBegin) {

  auto nonUniformInst = cast<Instruction>(nonUniform);
  Instruction *resultValue = nonUniformInst;

  // End the waterfall loop (as long as nonUniformInst is not a store with no result).
  if (!nonUniformInst->getType()->isVoidTy()) {
    SetInsertPoint(nonUniformInst->getNextNode());
    SetCurrentDebugLocation(nonUniformInst->getDebugLoc());

    Type *waterfallEndTy = resultValue->getType();
    if (auto vecTy = dyn_cast<FixedVectorType>(waterfallEndTy)) {
      if (vecTy->getElementType()->isIntegerTy(8)) {
        // ISel does not like waterfall.end with vector of i8 type, so cast if necessary.
        assert((vecTy->getNumElements() % 4) == 0);
        waterfallEndTy = getInt32Ty();
        if (vecTy->getNumElements() != 4)
          waterfallEndTy = FixedVectorType::get(getInt32Ty(), vecTy->getNumElements() / 4);
        resultValue = cast<Instruction>(CreateBitCast(resultValue, waterfallEndTy));
      }
    }
    resultValue =
        CreateIntrinsic(Intrinsic::amdgcn_waterfall_end, waterfallEndTy, {waterfallBegin, resultValue}, nullptr);

    if (waterfallEndTy != nonUniformInst->getType())
      resultValue = cast<Instruction>(CreateBitCast(resultValue, nonUniformInst->getType()));
  }

  return resultValue;
}
