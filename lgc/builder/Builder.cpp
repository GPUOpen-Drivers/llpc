/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Builder.cpp
 * @brief LLPC source file: implementation of lgc::Builder
 ***********************************************************************************************************************
 */
#include "BuilderRecorder.h"
#include "lgc/LgcContext.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/ShaderModes.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include <set>

#define DEBUG_TYPE "lgc-builder"

using namespace lgc;
using namespace llvm;

namespace {
// Descriptor sizes that are not part of hardware. Hardware-defined ones are in TargetInfo.
const unsigned DescriptorSizeBufferCompact = 2 * sizeof(unsigned);
} // anonymous namespace

// =====================================================================================================================
// Get the type of pointer returned by CreateLoadBufferDesc.
PointerType *BuilderCommon::getBufferDescTy() {
  return PointerType::get(getContext(), ADDR_SPACE_BUFFER_FAT_POINTER);
}

// =====================================================================================================================
// Get the type of a descriptor
//
// @param descType : Descriptor type, one of the ResourceNodeType values
VectorType *BuilderCommon::getDescTy(ResourceNodeType descType) {
  unsigned byteSize = 0;
  switch (descType) {
  case ResourceNodeType::DescriptorBuffer:
  case ResourceNodeType::DescriptorConstBuffer:
  case ResourceNodeType::DescriptorTexelBuffer:
    byteSize = DescriptorSizeBuffer;
    break;
  case ResourceNodeType::DescriptorBufferCompact:
  case ResourceNodeType::DescriptorConstBufferCompact:
    byteSize = DescriptorSizeBufferCompact;
    break;
  case ResourceNodeType::DescriptorSampler:
    byteSize = DescriptorSizeSampler;
    break;
  case ResourceNodeType::DescriptorResource:
  case ResourceNodeType::DescriptorFmask:
    byteSize = DescriptorSizeResource;
    break;
  default:
    llvm_unreachable("");
    break;
  }
  return FixedVectorType::get(getInt32Ty(), byteSize / sizeof(uint32_t));
}

// =====================================================================================================================
// Get the type of pointer to descriptor.
//
// @param descType : Descriptor type, one of the ResourceNodeType values
Type *BuilderCommon::getDescPtrTy(ResourceNodeType descType) {
  return getDescTy(descType)->getPointerTo(ADDR_SPACE_CONST);
}

// ====================================================================================================================
// Get address space of constant memory.
unsigned Builder::getAddrSpaceConst() {
  return ADDR_SPACE_CONST;
}

// ====================================================================================================================
// Get address space of local (thread-global) memory.
unsigned Builder::getAddrSpaceLocal() {
  return ADDR_SPACE_LOCAL;
}

// =====================================================================================================================
// Get the type of a built-in. Where the built-in has a shader-defined array size (ClipDistance,
// CullDistance, SampleMask), inOutInfo.GetArraySize() is used as the array size.
//
// @param builtIn : Built-in kind
// @param inOutInfo : Extra input/output info (shader-defined array size)
// @param context : LLVMContext
Type *BuilderDefs::getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo, LLVMContext &context) {
  enum TypeCode : unsigned {
    a2f32,
    a4f32,
    af32,
    ai32,
    av2i32,
    av3i32,
    f32,
    i1,
    i32,
    i64,
    mask,
    v2f32,
    v3f32,
    v3i32,
    v4f32,
    v4i32,
    a4v3f32
  };

  unsigned arraySize = inOutInfo.getArraySize();
  TypeCode typeCode = TypeCode::i32;
  switch (builtIn) {
#define BUILTIN(name, number, out, in, type)                                                                           \
  case BuiltIn##name:                                                                                                  \
    typeCode = TypeCode::type;                                                                                         \
    break;
#include "lgc/BuiltInDefs.h"
#undef BUILTIN
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  switch (typeCode) {
  case TypeCode::a2f32:
    return ArrayType::get(Type::getFloatTy(context), 2);
  case TypeCode::a4f32:
    return ArrayType::get(Type::getFloatTy(context), 4);
  // For ClipDistance and CullDistance, the shader determines the array size.
  case TypeCode::af32:
    return ArrayType::get(Type::getFloatTy(context), arraySize);
  // For SampleMask and PrimitivePointIndices, the shader determines the array size.
  case TypeCode::ai32:
    return ArrayType::get(Type::getInt32Ty(context), arraySize);
  // For PrimitiveLineIndices, the shader determines the array size.
  case TypeCode::av2i32:
    return ArrayType::get(FixedVectorType::get(Type::getInt32Ty(context), 2), arraySize);
  // For PrimitiveTriangleIndices, the shader determines the array size.
  case TypeCode::av3i32:
    return ArrayType::get(FixedVectorType::get(Type::getInt32Ty(context), 3), arraySize);
  case TypeCode::f32:
    return Type::getFloatTy(context);
  case TypeCode::i1:
    return Type::getInt1Ty(context);
  case TypeCode::i32:
    return Type::getInt32Ty(context);
  case TypeCode::i64:
    return Type::getInt64Ty(context);
  case TypeCode::v2f32:
    return FixedVectorType::get(Type::getFloatTy(context), 2);
  case TypeCode::v3f32:
    return FixedVectorType::get(Type::getFloatTy(context), 3);
  case TypeCode::v4f32:
    return FixedVectorType::get(Type::getFloatTy(context), 4);
  case TypeCode::v3i32:
    return FixedVectorType::get(Type::getInt32Ty(context), 3);
  case TypeCode::v4i32:
    return FixedVectorType::get(Type::getInt32Ty(context), 4);
  case TypeCode::a4v3f32:
    return ArrayType::get(FixedVectorType::get(Type::getFloatTy(context), 3), 4);
  default:
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
//
// @param ty : FP scalar or vector type
// @param value : APFloat value
Constant *BuilderCommon::getFpConstant(Type *ty, APFloat value) {
  const fltSemantics *semantics = &APFloat::IEEEdouble();
  Type *scalarTy = ty->getScalarType();
  if (scalarTy->isHalfTy())
    semantics = &APFloat::IEEEhalf();
  else if (scalarTy->isFloatTy())
    semantics = &APFloat::IEEEsingle();
  bool ignored = true;
  value.convert(*semantics, APFloat::rmNearestTiesToEven, &ignored);
  return ConstantFP::get(ty, value);
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value PI/180, for converting radians to degrees.
//
// @param ty : FP scalar or vector type
Constant *Builder::getPiOver180(Type *ty) {
  // PI/180, 0.017453292
  // TODO: Use a value that works for double as well.
  return getFpConstant(ty, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3F91DF46A0000000)));
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value 180/PI, for converting degrees to radians.
//
// @param ty : FP scalar or vector type
Constant *Builder::get180OverPi(Type *ty) {
  // 180/PI, 57.29577951308232
  // TODO: Use a value that works for double as well.
  return getFpConstant(ty, APFloat(APFloat::IEEEdouble(), APInt(64, 0x404CA5DC20000000)));
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value 1/(2^n - 1)
//
// @param ty : FP scalar or vector type
// @param n : Power of two to use
Constant *Builder::getOneOverPower2MinusOne(Type *ty, unsigned n) {
  // We could calculate this here, using knowledge that 1(2^n - 1) in binary has a repeating bit pattern
  // of {n-1 zeros, 1 one}. But instead we just special case the values of n that we know are
  // used from the frontend.
  uint64_t bits = 0;
  switch (n) {
  case 7: // 1/127
    bits = 0x3F80204081020408;
    break;
  case 8: // 1/255
    bits = 0x3F70101010101010;
    break;
  case 15: // 1/32767
    bits = 0x3F00002000400080;
    break;
  case 16: // 1/65535
    bits = 0x3EF0001000100010;
    break;
  default:
    llvm_unreachable("Should never be called!");
  }
  return getFpConstant(ty, APFloat(APFloat::IEEEdouble(), APInt(64, bits)));
}

// =====================================================================================================================
// Create a call to the specified intrinsic with one operand, mangled on its type.
// This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by fmfSource.
//
// @param id : Intrinsic ID
// @param value : Input value
// @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
// @param instName : Name to give instruction
CallInst *Builder::CreateUnaryIntrinsic(Intrinsic::ID id, Value *value, Instruction *fmfSource, const Twine &instName) {
  CallInst *result = IRBuilder<>::CreateUnaryIntrinsic(id, value, fmfSource, instName);
  if (!fmfSource && isa<FPMathOperator>(result)) {
    // There are certain intrinsics with an FP result that we do not want FMF on.
    switch (id) {
    case Intrinsic::amdgcn_wqm:
    case Intrinsic::amdgcn_wwm:
      break;
    default:
      result->setFastMathFlags(getFastMathFlags());
      break;
    }
  }
  return result;
}

// =====================================================================================================================
// Create a call to the specified intrinsic with two operands of the same type, mangled on that type.
// This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by fmfSource.
//
// @param id : Intrinsic ID
// @param value1 : Input value 1
// @param value2 : Input value 2
// @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
// @param name : Name to give instruction
CallInst *Builder::CreateBinaryIntrinsic(Intrinsic::ID id, Value *value1, Value *value2, Instruction *fmfSource,
                                         const Twine &name) {
  CallInst *result = IRBuilder<>::CreateBinaryIntrinsic(id, value1, value2, fmfSource, name);
  if (!fmfSource && isa<FPMathOperator>(result))
    result->setFastMathFlags(getFastMathFlags());
  return result;
}

// =====================================================================================================================
// Create a call to the specified intrinsic with the specified operands, mangled on the specified types.
// This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by fmfSource.
//
// @param id : Intrinsic ID
// @param types : Types
// @param args : Input values
// @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
// @param name : Name to give instruction
CallInst *Builder::CreateIntrinsic(Intrinsic::ID id, ArrayRef<Type *> types, ArrayRef<Value *> args,
                                   Instruction *fmfSource, const Twine &name) {
  CallInst *result = IRBuilder<>::CreateIntrinsic(id, types, args, fmfSource, name);
  if (!fmfSource && isa<FPMathOperator>(result))
    result->setFastMathFlags(getFastMathFlags());
  return result;
}

// =====================================================================================================================
// Create a call to the specified intrinsic with the specified return type and operands, mangled based on the operand
// types. This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by fmfSource.
//
// @param retTy : Return type
// @param id : Intrinsic ID
// @param args : Input values
// @param fmfSource : Instruction to copy fast math flags from; nullptr to get from Builder
// @param name : Name to give instruction
CallInst *Builder::CreateIntrinsic(Type *retTy, Intrinsic::ID id, ArrayRef<Value *> args, Instruction *fmfSource,
                                   const Twine &name) {
  CallInst *result = IRBuilder<>::CreateIntrinsic(retTy, id, args, fmfSource, name);
  if (!fmfSource && isa<FPMathOperator>(result))
    result->setFastMathFlags(getFastMathFlags());
  return result;
}
