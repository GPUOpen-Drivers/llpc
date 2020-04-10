/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "BuilderImpl.h"
#include "BuilderRecorder.h"
#include "Internal.h"
#include "PipelineState.h"
#include "ShaderModes.h"
#include "lgc/LgcContext.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include <set>

#define DEBUG_TYPE "llpc-builder"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
//
// @param builderContext : Builder context
Builder::Builder(LgcContext *builderContext)
    : BuilderBase(builderContext->getContext()), m_builderContext(builderContext) {
}

// =====================================================================================================================
// Set the common shader mode for the current shader, containing hardware FP round and denorm modes.
//
// @param commonShaderMode : FP round and denorm modes
void Builder::setCommonShaderMode(const CommonShaderMode &commonShaderMode) {
  getShaderModes()->setCommonShaderMode(m_shaderStage, commonShaderMode);
}

// =====================================================================================================================
// Get the common shader mode for the current shader.
const CommonShaderMode &Builder::getCommonShaderMode() {
  return getShaderModes()->getCommonShaderMode(m_shaderStage);
}

// =====================================================================================================================
// Set the tessellation mode
//
// @param tessellationMode : Tessellation mode
void Builder::setTessellationMode(const TessellationMode &tessellationMode) {
  getShaderModes()->setTessellationMode(tessellationMode);
}

// =====================================================================================================================
// Set the geometry shader mode
//
// @param geometryShaderMode : Geometry shader mode
void Builder::setGeometryShaderMode(const GeometryShaderMode &geometryShaderMode) {
  getShaderModes()->setGeometryShaderMode(geometryShaderMode);
}

// =====================================================================================================================
// Set the fragment shader mode
//
// @param fragmentShaderMode : Fragment shader mode
void Builder::setFragmentShaderMode(const FragmentShaderMode &fragmentShaderMode) {
  getShaderModes()->setFragmentShaderMode(fragmentShaderMode);
}

// =====================================================================================================================
// Set the compute shader mode (workgroup size)
//
// @param computeShaderMode : Compute shader mode
void Builder::setComputeShaderMode(const ComputeShaderMode &computeShaderMode) {
  getShaderModes()->setComputeShaderMode(computeShaderMode);
}

// =====================================================================================================================
// Get the type pElementTy, turned into a vector of the same vector width as pMaybeVecTy if the latter
// is a vector type.
//
// @param elementTy : Element type
// @param maybeVecTy : Possible vector type to get number of elements from
Type *Builder::getConditionallyVectorizedTy(Type *elementTy, Type *maybeVecTy) {
  if (auto vecTy = dyn_cast<VectorType>(maybeVecTy))
    return VectorType::get(elementTy, vecTy->getNumElements());
  return elementTy;
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
Value *Builder::CreateMapToInt32(PFN_MapToInt32Func mapFunc, ArrayRef<Value *> mappedArgs,
                                 ArrayRef<Value *> passthroughArgs) {
  // We must have at least one argument to massage.
  assert(mappedArgs.size() > 0);

  Type *const type = mappedArgs[0]->getType();

  // Check the massage types all match.
  for (unsigned i = 1; i < mappedArgs.size(); i++)
    assert(mappedArgs[i]->getType() == type);

  if (mappedArgs[0]->getType()->isVectorTy()) {
    // For vectors we extract each vector component and map them individually.
    const unsigned compCount = type->getVectorNumElements();

    SmallVector<Value *, 4> results;

    for (unsigned i = 0; i < compCount; i++) {
      SmallVector<Value *, 4> newMappedArgs;

      for (Value *const mappedArg : mappedArgs)
        newMappedArgs.push_back(CreateExtractElement(mappedArg, i));

      results.push_back(CreateMapToInt32(mapFunc, newMappedArgs, passthroughArgs));
    }

    Value *result = UndefValue::get(VectorType::get(results[0]->getType(), compCount));

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

    Type *const vectorType = VectorType::get(type, type->getPrimitiveSizeInBits() == 16 ? 2 : 4);
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
      castMappedArgs.push_back(CreateBitCast(mappedArg, VectorType::get(getInt32Ty(), 2)));

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

// =====================================================================================================================
// Gets new matrix type after doing matrix transposing.
//
// @param matrixType : The matrix type to get the transposed type from.
Type *Builder::getTransposedMatrixTy(Type *const matrixType) const {
  assert(matrixType->isArrayTy());

  Type *const columnVectorType = matrixType->getArrayElementType();
  assert(columnVectorType->isVectorTy());

  const unsigned columnCount = matrixType->getArrayNumElements();
  const unsigned rowCount = columnVectorType->getVectorNumElements();

  return ArrayType::get(VectorType::get(columnVectorType->getVectorElementType(), columnCount), rowCount);
}

// =====================================================================================================================
// Get the type of pointer returned by CreateLoadBufferDesc.
//
// @param pointeeTy : Type that the returned pointer should point to.
PointerType *Builder::getBufferDescTy(Type *pointeeTy) {
  return PointerType::get(pointeeTy, ADDR_SPACE_BUFFER_FAT_POINTER);
}

// =====================================================================================================================
// Get the type of an image descriptor
VectorType *Builder::getImageDescTy() {
  return VectorType::get(getInt32Ty(), 8);
}

// =====================================================================================================================
// Get the type of an fmask descriptor
VectorType *Builder::getFmaskDescTy() {
  return VectorType::get(getInt32Ty(), 8);
}

// =====================================================================================================================
// Get the type of a texel buffer descriptor
VectorType *Builder::getTexelBufferDescTy() {
  return VectorType::get(getInt32Ty(), 4);
}

// =====================================================================================================================
// Get the type of a sampler descriptor
VectorType *Builder::getSamplerDescTy() {
  return VectorType::get(getInt32Ty(), 4);
}

// =====================================================================================================================
// Get the type of pointer to image descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type *Builder::getImageDescPtrTy() {
  return StructType::get(getContext(), {PointerType::get(getImageDescTy(), ADDR_SPACE_CONST), getInt32Ty()});
}

// =====================================================================================================================
// Get the type of pointer to fmask descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type *Builder::getFmaskDescPtrTy() {
  return StructType::get(getContext(), {PointerType::get(getFmaskDescTy(), ADDR_SPACE_CONST), getInt32Ty()});
}

// =====================================================================================================================
// Get the type of pointer to texel buffer descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type *Builder::getTexelBufferDescPtrTy() {
  return StructType::get(getContext(), {PointerType::get(getTexelBufferDescTy(), ADDR_SPACE_CONST), getInt32Ty()});
}

// =====================================================================================================================
// Get the type of pointer to sampler descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type *Builder::getSamplerDescPtrTy() {
  return StructType::get(getContext(), {PointerType::get(getSamplerDescTy(), ADDR_SPACE_CONST), getInt32Ty()});
}

// =====================================================================================================================
// Get the type of a built-in. Where the built-in has a shader-defined array size (ClipDistance,
// CullDistance, SampleMask), inOutInfo.GetArraySize() is used as the array size.
//
// @param builtIn : Built-in kind
// @param inOutInfo : Extra input/output info (shader-defined array size)
Type *Builder::getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo) {
  enum TypeCode : unsigned {
    a2f32,
    a4f32,
    af32,
    ai32,
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
    return ArrayType::get(getFloatTy(), 2);
  case TypeCode::a4f32:
    return ArrayType::get(getFloatTy(), 4);
  // For ClipDistance and CullDistance, the shader determines the array size.
  case TypeCode::af32:
    return ArrayType::get(getFloatTy(), arraySize);
  // For SampleMask, the shader determines the array size.
  case TypeCode::ai32:
    return ArrayType::get(getInt32Ty(), arraySize);
  case TypeCode::f32:
    return getFloatTy();
  case TypeCode::i1:
    return getInt1Ty();
  case TypeCode::i32:
    return getInt32Ty();
  case TypeCode::i64:
    return getInt64Ty();
  case TypeCode::v2f32:
    return VectorType::get(getFloatTy(), 2);
  case TypeCode::v3f32:
    return VectorType::get(getFloatTy(), 3);
  case TypeCode::v4f32:
    return VectorType::get(getFloatTy(), 4);
  case TypeCode::v3i32:
    return VectorType::get(getInt32Ty(), 3);
  case TypeCode::v4i32:
    return VectorType::get(getInt32Ty(), 4);
  case TypeCode::a4v3f32:
    return ArrayType::get(VectorType::get(getFloatTy(), 3), 4);
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
Constant *Builder::getFpConstant(Type *ty, APFloat value) {
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
// flags from the Builder if none are specified by pFmfSource.
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
// flags from the Builder if none are specified by pFmfSource.
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
// flags from the Builder if none are specified by pFmfSource.
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

