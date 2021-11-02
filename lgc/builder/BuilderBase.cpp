/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create an LLVM function call to the named function. The callee is built automically based on return
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
  } else if (type->isIntegerTy() && type->getIntegerBitWidth() == 1) {
    SmallVector<Value *, 4> newMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      newMappedArgs.push_back(CreateZExt(mappedArg, getInt32Ty()));

    Value *const result = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);
    return CreateTrunc(result, getInt1Ty());
  } else if (type->isIntegerTy() && type->getIntegerBitWidth() < 32) {
    SmallVector<Value *, 4> newMappedArgs;

    Type *const vectorType = FixedVectorType::get(type, type->getPrimitiveSizeInBits() == 16 ? 2 : 4);
    Value *const undef = UndefValue::get(vectorType);

    for (Value *const mappedArg : mappedArgs) {
      Value *const newMappedArg = CreateInsertElement(undef, mappedArg, static_cast<uint64_t>(0));
      newMappedArgs.push_back(CreateBitCast(newMappedArg, getInt32Ty()));
    }

    Value *const result = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);
    return CreateExtractElement(CreateBitCast(result, vectorType), static_cast<uint64_t>(0));
  } else if (type->getPrimitiveSizeInBits() == 64) {
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
  } else if (type->isFloatingPointTy()) {
    SmallVector<Value *, 4> newMappedArgs;

    for (Value *const mappedArg : mappedArgs)
      newMappedArgs.push_back(CreateBitCast(mappedArg, getIntNTy(mappedArg->getType()->getPrimitiveSizeInBits())));

    Value *const result = CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs);
    return CreateBitCast(result, type);
  } else if (type->isIntegerTy(32))
    return mapFunc(*this, mappedArgs, passthroughArgs);
  else {
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
}
