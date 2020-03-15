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
 * @file  llpcBuilder.cpp
 * @brief LLPC source file: implementation of Llpc::Builder
 ***********************************************************************************************************************
 */
#include "llvm/IR/IntrinsicsAMDGPU.h"

#include "llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcInternal.h"
#include "llpcPipelineState.h"
#include "llpcShaderModes.h"

#include <set>

#define DEBUG_TYPE "llpc-builder"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
Builder::Builder(
    BuilderContext* pBuilderContext) // [in] Builder context
    :
    BuilderBase(pBuilderContext->GetContext()),
    m_pBuilderContext(pBuilderContext)
{
}

// =====================================================================================================================
// Set the common shader mode for the current shader, containing hardware FP round and denorm modes.
void Builder::SetCommonShaderMode(
    const CommonShaderMode& commonShaderMode)   // [in] FP round and denorm modes
{
    GetShaderModes()->SetCommonShaderMode(m_shaderStage, commonShaderMode);
}

// =====================================================================================================================
// Get the common shader mode for the current shader.
const CommonShaderMode& Builder::GetCommonShaderMode()
{
    return GetShaderModes()->GetCommonShaderMode(m_shaderStage);
}

// =====================================================================================================================
// Set the tessellation mode
void Builder::SetTessellationMode(
    const TessellationMode& tessellationMode)   // [in] Tessellation mode
{
    GetShaderModes()->SetTessellationMode(tessellationMode);
}

// =====================================================================================================================
// Set the geometry shader mode
void Builder::SetGeometryShaderMode(
    const GeometryShaderMode& geometryShaderMode)   // [in] Geometry shader mode
{
    GetShaderModes()->SetGeometryShaderMode(geometryShaderMode);
}

// =====================================================================================================================
// Set the fragment shader mode
void Builder::SetFragmentShaderMode(
    const FragmentShaderMode& fragmentShaderMode)   // [in] Fragment shader mode
{
    GetShaderModes()->SetFragmentShaderMode(fragmentShaderMode);
}

// =====================================================================================================================
// Set the compute shader mode (workgroup size)
void Builder::SetComputeShaderMode(
    const ComputeShaderMode& computeShaderMode)   // [in] Compute shader mode
{
    GetShaderModes()->SetComputeShaderMode(computeShaderMode);
}

// =====================================================================================================================
// Get the type pElementTy, turned into a vector of the same vector width as pMaybeVecTy if the latter
// is a vector type.
Type* Builder::GetConditionallyVectorizedTy(
    Type* pElementTy,           // [in] Element type
    Type* pMaybeVecTy)          // [in] Possible vector type to get number of elements from
{
    if (auto pVecTy = dyn_cast<VectorType>(pMaybeVecTy))
    {
        return VectorType::get(pElementTy, pVecTy->getNumElements());
    }
    return pElementTy;
}

// =====================================================================================================================
// Create a map to i32 function. Many AMDGCN intrinsics only take i32's, so we need to massage input data into an i32
// to allow us to call these intrinsics. This helper takes a function pointer, massage arguments, and passthrough
// arguments and massages the mappedArgs into i32's before calling the function pointer. Note that all massage
// arguments must have the same type.
Value* Builder::CreateMapToInt32(
    PFN_MapToInt32Func pfnMapFunc,      // [in] The function to call on each provided i32.
    ArrayRef<Value*>   mappedArgs,      // The arguments to be massaged into i32's and passed to function.
    ArrayRef<Value*>   passthroughArgs) // The arguments to be passed through as is (no massaging).
{
    // We must have at least one argument to massage.
    assert(mappedArgs.size() > 0);

    Type* const pType = mappedArgs[0]->getType();

    // Check the massage types all match.
    for (uint32_t i = 1; i < mappedArgs.size(); i++)
    {
        assert(mappedArgs[i]->getType() == pType);
    }

    if (mappedArgs[0]->getType()->isVectorTy())
    {
        // For vectors we extract each vector component and map them individually.
        const uint32_t compCount = pType->getVectorNumElements();

        SmallVector<Value*, 4> results;

        for (uint32_t i = 0; i < compCount; i++)
        {
            SmallVector<Value*, 4> newMappedArgs;

            for (Value* const pMappedArg : mappedArgs)
            {
                newMappedArgs.push_back(CreateExtractElement(pMappedArg, i));
            }

            results.push_back(CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs));
        }

        Value* pResult = UndefValue::get(VectorType::get(results[0]->getType(), compCount));

        for (uint32_t i = 0; i < compCount; i++)
        {
            pResult = CreateInsertElement(pResult, results[i], i);
        }

        return pResult;
    }
    else if (pType->isIntegerTy() && pType->getIntegerBitWidth() == 1)
    {
        SmallVector<Value*, 4> newMappedArgs;

        for (Value* const pMappedArg : mappedArgs)
        {
            newMappedArgs.push_back(CreateZExt(pMappedArg, getInt32Ty()));
        }

        Value* const pResult = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);
        return CreateTrunc(pResult, getInt1Ty());
    }
    else if (pType->isIntegerTy() && pType->getIntegerBitWidth() < 32)
    {
        SmallVector<Value*, 4> newMappedArgs;

        Type* const pVectorType = VectorType::get(pType, (pType->getPrimitiveSizeInBits() == 16) ? 2 : 4);
        Value* const pUndef = UndefValue::get(pVectorType);

        for (Value* const pMappedArg : mappedArgs)
        {
            Value* const pNewMappedArg = CreateInsertElement(pUndef, pMappedArg, static_cast<uint64_t>(0));
            newMappedArgs.push_back(CreateBitCast(pNewMappedArg, getInt32Ty()));
        }

        Value* const pResult = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);
        return CreateExtractElement(CreateBitCast(pResult, pVectorType), static_cast<uint64_t>(0));
    }
    else if (pType->getPrimitiveSizeInBits() == 64)
    {
        SmallVector<Value*, 4> castMappedArgs;

        for (Value* const pMappedArg : mappedArgs)
        {
            castMappedArgs.push_back(CreateBitCast(pMappedArg, VectorType::get(getInt32Ty(), 2)));
        }

        Value* pResult = UndefValue::get(castMappedArgs[0]->getType());

        for (uint32_t i = 0; i < 2; i++)
        {
            SmallVector<Value*, 4> newMappedArgs;

            for (Value* const pCastMappedArg : castMappedArgs)
            {
                newMappedArgs.push_back(CreateExtractElement(pCastMappedArg, i));
            }

            Value* const pResultComp = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);

            pResult = CreateInsertElement(pResult, pResultComp, i);
        }

        return CreateBitCast(pResult, pType);
    }
    else if (pType->isFloatingPointTy())
    {
        SmallVector<Value*, 4> newMappedArgs;

        for (Value* const pMappedArg : mappedArgs)
        {
            newMappedArgs.push_back(CreateBitCast(pMappedArg, getIntNTy(pMappedArg->getType()->getPrimitiveSizeInBits())));
        }

        Value* const pResult = CreateMapToInt32(pfnMapFunc, newMappedArgs, passthroughArgs);
        return CreateBitCast(pResult, pType);
    }
    else if (pType->isIntegerTy(32))
    {
        return pfnMapFunc(*this, mappedArgs, passthroughArgs);
    }
    else
    {
        llvm_unreachable("Should never be called!");
        return nullptr;
    }
}

// =====================================================================================================================
// Gets new matrix type after doing matrix transposing.
Type* Builder::GetTransposedMatrixTy(
    Type* const pMatrixType // [in] The matrix type to get the transposed type from.
    ) const
{
    assert(pMatrixType->isArrayTy());

    Type* const pColumnVectorType = pMatrixType->getArrayElementType();
    assert(pColumnVectorType->isVectorTy());

    const uint32_t columnCount = pMatrixType->getArrayNumElements();
    const uint32_t rowCount = pColumnVectorType->getVectorNumElements();

    return ArrayType::get(VectorType::get(pColumnVectorType->getVectorElementType(), columnCount), rowCount);
}

// =====================================================================================================================
// Get the type of pointer returned by CreateLoadBufferDesc.
PointerType* Builder::GetBufferDescTy(
    Type*         pPointeeTy)         // [in] Type that the returned pointer should point to.
{
    return PointerType::get(pPointeeTy, ADDR_SPACE_BUFFER_FAT_POINTER);
}

// =====================================================================================================================
// Get the type of an image descriptor
VectorType* Builder::GetImageDescTy()
{
    return VectorType::get(getInt32Ty(), 8);
}

// =====================================================================================================================
// Get the type of an fmask descriptor
VectorType* Builder::GetFmaskDescTy()
{
    return VectorType::get(getInt32Ty(), 8);
}

// =====================================================================================================================
// Get the type of a texel buffer descriptor
VectorType* Builder::GetTexelBufferDescTy()
{
    return VectorType::get(getInt32Ty(), 4);
}

// =====================================================================================================================
// Get the type of a sampler descriptor
VectorType* Builder::GetSamplerDescTy()
{
    return VectorType::get(getInt32Ty(), 4);
}

// =====================================================================================================================
// Get the type of pointer to image descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetImageDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetImageDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of pointer to fmask descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetFmaskDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetFmaskDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of pointer to texel buffer descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetTexelBufferDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetTexelBufferDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of pointer to sampler descriptor.
// This is in fact a struct containing the pointer itself plus the stride in dwords.
Type* Builder::GetSamplerDescPtrTy()
{
    return StructType::get(getContext(), { PointerType::get(GetSamplerDescTy(), ADDR_SPACE_CONST), getInt32Ty() });
}

// =====================================================================================================================
// Get the type of a built-in. Where the built-in has a shader-defined array size (ClipDistance,
// CullDistance, SampleMask), inOutInfo.GetArraySize() is used as the array size.
Type* Builder::GetBuiltInTy(
    BuiltInKind   builtIn,            // Built-in kind
    InOutInfo     inOutInfo)          // Extra input/output info (shader-defined array size)
{
    enum TypeCode: uint32_t
    {
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

    uint32_t arraySize = inOutInfo.GetArraySize();
    TypeCode typeCode = TypeCode::i32;
    switch (builtIn)
    {
#define BUILTIN(name, number, out, in, type) \
    case BuiltIn ## name: \
        typeCode = TypeCode:: type; \
        break;
#include "llpcBuilderBuiltInDefs.h"
#undef BUILTIN
    default:
        llvm_unreachable("Should never be called!");
        break;
    }

    switch (typeCode)
    {
    case TypeCode::a2f32: return ArrayType::get(getFloatTy(), 2);
    case TypeCode::a4f32: return ArrayType::get(getFloatTy(), 4);
    // For ClipDistance and CullDistance, the shader determines the array size.
    case TypeCode::af32: return ArrayType::get(getFloatTy(), arraySize);
    // For SampleMask, the shader determines the array size.
    case TypeCode::ai32: return ArrayType::get(getInt32Ty(), arraySize);
    case TypeCode::f32: return getFloatTy();
    case TypeCode::i1: return getInt1Ty();
    case TypeCode::i32: return getInt32Ty();
    case TypeCode::i64: return getInt64Ty();
    case TypeCode::v2f32: return VectorType::get(getFloatTy(), 2);
    case TypeCode::v3f32: return VectorType::get(getFloatTy(), 3);
    case TypeCode::v4f32: return VectorType::get(getFloatTy(), 4);
    case TypeCode::v3i32: return VectorType::get(getInt32Ty(), 3);
    case TypeCode::v4i32: return VectorType::get(getInt32Ty(), 4);
    case TypeCode::a4v3f32: return ArrayType::get(VectorType::get(getFloatTy(), 3), 4);
    default:
        llvm_unreachable("Should never be called!");
        return nullptr;
    }
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
Constant* Builder::GetFpConstant(
    Type*           pTy,    // [in] FP scalar or vector type
    APFloat         value)  // APFloat value
{
    const fltSemantics* pSemantics = &APFloat::IEEEdouble();
    Type* pScalarTy = pTy->getScalarType();
    if (pScalarTy->isHalfTy())
    {
        pSemantics = &APFloat::IEEEhalf();
    }
    else if (pScalarTy->isFloatTy())
    {
        pSemantics = &APFloat::IEEEsingle();
    }
    bool ignored = true;
    value.convert(*pSemantics, APFloat::rmNearestTiesToEven, &ignored);
    return ConstantFP::get(pTy, value);
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value PI/180, for converting radians to degrees.
Constant* Builder::GetPiOver180(
    Type* pTy)    // [in] FP scalar or vector type
{
    // PI/180, 0.017453292
    // TODO: Use a value that works for double as well.
    return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3F91DF46A0000000)));
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value 180/PI, for converting degrees to radians.
Constant* Builder::Get180OverPi(
    Type* pTy)    // [in] FP scalar or vector type
{
    // 180/PI, 57.29577951308232
    // TODO: Use a value that works for double as well.
    return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x404CA5DC20000000)));
}

// =====================================================================================================================
// Get a constant of FP or vector of FP type for the value 1/(2^n - 1)
Constant* Builder::GetOneOverPower2MinusOne(
    Type*     pTy,  // [in] FP scalar or vector type
    uint32_t  n)    // Power of two to use
{
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
    return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, bits)));
}

// =====================================================================================================================
// Create a call to the specified intrinsic with one operand, mangled on its type.
// This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by pFmfSource.
CallInst* Builder::CreateUnaryIntrinsic(
    Intrinsic::ID id,           // Intrinsic ID
    Value*        pValue,       // [in] Input value
    Instruction*  pFmfSource,   // [in] Instruction to copy fast math flags from; nullptr to get from Builder
    const Twine&  instName)     // [in] Name to give instruction
{
    CallInst* pResult = IRBuilder<>::CreateUnaryIntrinsic(id, pValue, pFmfSource, instName);
    if ((pFmfSource == nullptr) && isa<FPMathOperator>(pResult))
    {
        // There are certain intrinsics with an FP result that we do not want FMF on.
        switch (id)
        {
        case Intrinsic::amdgcn_wqm:
        case Intrinsic::amdgcn_wwm:
            break;
        default:
            pResult->setFastMathFlags(getFastMathFlags());
            break;
        }
    }
    return pResult;
}

// =====================================================================================================================
// Create a call to the specified intrinsic with two operands of the same type, mangled on that type.
// This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by pFmfSource.
CallInst* Builder::CreateBinaryIntrinsic(
    Intrinsic::ID id,           // Intrinsic ID
    Value*        pValue1,      // [in] Input value 1
    Value*        pValue2,      // [in] Input value 2
    Instruction*  pFmfSource,   // [in] Instruction to copy fast math flags from; nullptr to get from Builder
    const Twine&  name)         // [in] Name to give instruction
{
    CallInst* pResult = IRBuilder<>::CreateBinaryIntrinsic(id, pValue1, pValue2, pFmfSource, name);
    if ((pFmfSource == nullptr) && isa<FPMathOperator>(pResult))
    {
        pResult->setFastMathFlags(getFastMathFlags());
    }
    return pResult;
}

// =====================================================================================================================
// Create a call to the specified intrinsic with the specified operands, mangled on the specified types.
// This is an override of the same method in IRBuilder<>; the difference is that this one sets fast math
// flags from the Builder if none are specified by pFmfSource.
CallInst* Builder::CreateIntrinsic(
    Intrinsic::ID    id,         // Intrinsic ID
    ArrayRef<Type*>  types,      // [in] Types
    ArrayRef<Value*> args,       // [in] Input values
    Instruction*     pFmfSource, // [in] Instruction to copy fast math flags from; nullptr to get from Builder
    const Twine&     name)       // [in] Name to give instruction
{
    CallInst* pResult = IRBuilder<>::CreateIntrinsic(id, types, args, pFmfSource, name);
    if ((pFmfSource == nullptr) && isa<FPMathOperator>(pResult))
    {
        pResult->setFastMathFlags(getFastMathFlags());
    }
    return pResult;
}
