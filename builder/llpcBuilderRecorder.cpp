/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderRecorder.cpp
 * @brief LLPC source file: BuilderRecorder implementation
 ***********************************************************************************************************************
 */
#include "llpcBuilderContext.h"
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"
#include "llpcIntrinsDefs.h"
#include "llpcPipelineState.h"
#include "llpcShaderModes.h"

#define DEBUG_TYPE "llpc-builder-recorder"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Given an opcode, get the call name (without the "llpc.call." prefix)
StringRef BuilderRecorder::GetCallName(
    Opcode opcode)    // Opcode
{
    switch (opcode)
    {
    case Opcode::Nop:
        return "nop";
    case Opcode::DotProduct:
        return "dot.product";
    case Opcode::CubeFaceCoord:
        return "cube.face.coord";
    case Opcode::CubeFaceIndex:
        return "cube.face.index";
    case Opcode::FpTruncWithRounding:
        return "fp.trunc.with.rounding";
    case Opcode::QuantizeToFp16:
        return "quantize.to.fp16";
    case Opcode::SMod:
        return "smod";
    case Opcode::FMod:
        return "fmod";
    case Opcode::Fma:
        return "fma";
    case Tan:
        return "tan";
    case ASin:
        return "asin";
    case ACos:
        return "acos";
    case ATan:
        return "atan";
    case ATan2:
        return "atan2";
    case Sinh:
        return "sinh";
    case Cosh:
        return "cosh";
    case Tanh:
        return "tanh";
    case ASinh:
        return "asinh";
    case ACosh:
        return "acosh";
    case ATanh:
        return "atanh";
    case Power:
        return "power";
    case Exp:
        return "exp";
    case Log:
        return "log";
    case InverseSqrt:
        return "inverse.sqrt";
    case SAbs:
        return "sabs";
    case FSign:
        return "fsign";
    case SSign:
        return "ssign";
    case Fract:
        return "fract";
    case SmoothStep:
        return "smooth.step";
    case Ldexp:
        return "ldexp";
    case ExtractSignificand:
        return "extract.significand";
    case ExtractExponent:
        return "extract.exponent";
    case CrossProduct:
        return "cross.product";
    case NormalizeVector:
        return "normalize.vector";
    case FaceForward:
        return "face.forward";
    case Reflect:
        return "reflect";
    case Refract:
        return "refract";
    case Opcode::FClamp:
        return "fclamp";
    case Opcode::FMin:
        return "fmin";
    case Opcode::FMax:
        return "fmax";
    case Opcode::FMin3:
        return "fmin3";
    case Opcode::FMax3:
        return "fmax3";
    case Opcode::FMid3:
        return "fmid3";
    case Opcode::IsInf:
        return "isinf";
    case Opcode::IsNaN:
        return "isnan";
    case Opcode::InsertBitField:
        return "insert.bit.field";
    case Opcode::ExtractBitField:
        return "extract.bit.field";
    case Opcode::FindSMsb:
        return "find.smsb";
    case Opcode::LoadBufferDesc:
        return "load.buffer.desc";
    case Opcode::IndexDescPtr:
        return "index.desc.ptr";
    case Opcode::LoadDescFromPtr:
        return "load.desc.from.ptr";
    case Opcode::GetSamplerDescPtr:
        return "get.sampler.desc.ptr";
    case Opcode::GetImageDescPtr:
        return "get.image.desc.ptr";
    case Opcode::GetTexelBufferDescPtr:
        return "get.texel.buffer.desc.ptr";
    case Opcode::GetFmaskDescPtr:
        return "get.fmask.desc.ptr";
    case Opcode::LoadPushConstantsPtr:
        return "load.push.constants.ptr";
    case Opcode::GetBufferDescLength:
        return "get.buffer.desc.length";
    case Opcode::ReadGenericInput:
        return "read.generic.input";
    case Opcode::ReadGenericOutput:
        return "read.generic.output";
    case Opcode::WriteGenericOutput:
        return "write.generic.output";
    case Opcode::WriteXfbOutput:
        return "write.xfb.output";
    case Opcode::ReadBuiltInInput:
        return "read.builtin.input";
    case Opcode::ReadBuiltInOutput:
        return "read.builtin.output";
    case Opcode::WriteBuiltInOutput:
        return "write.builtin.output";
    case Opcode::TransposeMatrix:
        return "transpose.matrix";
    case Opcode::MatrixTimesScalar:
        return "matrix.times.scalar";
    case Opcode::VectorTimesMatrix:
        return "vector.times.matrix";
    case Opcode::MatrixTimesVector:
        return "matrix.times.vector";
    case Opcode::MatrixTimesMatrix:
        return "matrix.times.matrix";
    case Opcode::OuterProduct:
        return "outer.product";
    case Opcode::Determinant:
        return "determinant";
    case Opcode::MatrixInverse:
        return "matrix.inverse";
    case Opcode::EmitVertex:
        return "emit.vertex";
    case Opcode::EndPrimitive:
        return "end.primitive";
    case Opcode::Barrier:
        return "barrier";
    case Opcode::Kill:
        return "kill";
    case Opcode::ReadClock:
        return "read.clock";
    case Opcode::Derivative:
        return "derivative";
    case Opcode::DemoteToHelperInvocation:
        return "demote.to.helper.invocation";
    case Opcode::IsHelperInvocation:
        return "is.helper.invocation";
    case Opcode::ImageLoad:
        return "image.load";
    case Opcode::ImageLoadWithFmask:
        return "image.load.with.fmask";
    case Opcode::ImageStore:
        return "image.store";
    case Opcode::ImageSample:
        return "image.sample";
    case Opcode::ImageGather:
        return "image.gather";
    case Opcode::ImageAtomic:
        return "image.atomic";
    case Opcode::ImageAtomicCompareSwap:
        return "image.atomic.compare.swap";
    case Opcode::ImageQueryLevels:
        return "image.query.levels";
    case Opcode::ImageQuerySamples:
        return "image.query.samples";
    case Opcode::ImageQuerySize:
        return "image.query.size";
    case Opcode::ImageGetLod:
        return "image.get.lod";
    case GetSubgroupSize:
        return "get.subgroup.size";
    case SubgroupElect:
        return "subgroup.elect";
    case SubgroupAll:
        return "subgroup.all";
    case SubgroupAny:
        return "subgroup.any";
    case SubgroupAllEqual:
        return "subgroup.all.equal";
    case SubgroupBroadcast:
        return "subgroup.broadcast";
    case SubgroupBroadcastFirst:
        return "subgroup.broadcast.first";
    case SubgroupBallot:
        return "subgroup.ballot";
    case SubgroupInverseBallot:
        return "subgroup.inverse.ballot";
    case SubgroupBallotBitExtract:
        return "subgroup.ballot.bit.extract";
    case SubgroupBallotBitCount:
        return "subgroup.ballot.bit.count";
    case SubgroupBallotInclusiveBitCount:
        return "subgroup.ballot.inclusive.bit.count";
    case SubgroupBallotExclusiveBitCount:
        return "subgroup.ballot.exclusive.bit.count";
    case SubgroupBallotFindLsb:
        return "subgroup.ballot.find.lsb";
    case SubgroupBallotFindMsb:
        return "subgroup.ballot.find.msb";
    case SubgroupShuffle:
        return "subgroup.shuffle";
    case SubgroupShuffleXor:
        return "subgroup.shuffle.xor";
    case SubgroupShuffleUp:
        return "subgroup.shuffle.up";
    case SubgroupShuffleDown:
        return "subgroup.shuffle.down";
    case SubgroupClusteredReduction:
        return "subgroup.clustered.reduction";
    case SubgroupClusteredInclusive:
        return "subgroup.clustered.inclusive";
    case SubgroupClusteredExclusive:
        return "subgroup.clustered.exclusive";
    case SubgroupQuadBroadcast:
        return "subgroup.quad.broadcast";
    case SubgroupQuadSwapHorizontal:
        return "subgroup.quad.swap.horizontal";
    case SubgroupQuadSwapVertical:
        return "subgroup.quad.swap.vertical";
    case SubgroupQuadSwapDiagonal:
        return "subgroup.quad.swap.diagonal";
    case SubgroupSwizzleQuad:
        return "subgroup.swizzle.quad";
    case SubgroupSwizzleMask:
        return "subgroup.swizzle.mask";
    case SubgroupWriteInvocation:
        return "subgroup.write.invocation";
    case SubgroupMbcnt:
        return "subgroup.mbcnt";
    }
    LLPC_NEVER_CALLED();
    return "";
}

// =====================================================================================================================
// BuilderRecordedMetadataKinds constructor : get the metadata kind IDs
BuilderRecorderMetadataKinds::BuilderRecorderMetadataKinds(
    llvm::LLVMContext& context)   // [in] LLVM context
{
    m_opcodeMetaKindId = context.getMDKindID(BuilderCallOpcodeMetadataName);
}

// =====================================================================================================================
BuilderRecorder::BuilderRecorder(
    BuilderContext* pBuilderContext,// [in] Builder context
    Pipeline*       pPipeline)      // [in] PipelineState, or nullptr for shader compile
    : Builder(pBuilderContext),
      BuilderRecorderMetadataKinds(pBuilderContext->GetContext()),
      m_pPipelineState(reinterpret_cast<PipelineState*>(pPipeline))
{
}

// =====================================================================================================================
// Record shader modes into IR metadata if this is a shader compile (no PipelineState).
// For a pipeline compile with BuilderRecorder, they get recorded by PipelineState.
void BuilderRecorder::RecordShaderModes(
    Module* pModule)    // [in/out] Module to record into
{
    if ((m_pPipelineState == nullptr) && m_shaderModes)
    {
        m_shaderModes->Record(pModule);
    }
}

// =====================================================================================================================
// Get the ShaderModes object. If this is a pipeline compile, we get the ShaderModes object from the PipelineState.
// If it is a shader compile, we create our own ShaderModes object.
ShaderModes* BuilderRecorder::GetShaderModes()
{
    if (m_pPipelineState)
    {
        return m_pPipelineState->GetShaderModes();
    }
    if (!m_shaderModes)
    {
        m_shaderModes.reset(new ShaderModes());
    }
    return &*m_shaderModes;
}

// =====================================================================================================================
// Create scalar from dot product of vector
Value* BuilderRecorder::CreateDotProduct(
    Value* const pVector1,            // [in] The vector 1
    Value* const pVector2,            // [in] The vector 2
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Type* const pScalarType = pVector1->getType()->getVectorElementType();
    return Record(Opcode::DotProduct, pScalarType, { pVector1, pVector2 }, instName);
}

// =====================================================================================================================
// In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
// the current output primitive in the specified output-primitive stream number.
Instruction* BuilderRecorder::CreateEmitVertex(
    uint32_t                streamId)           // Stream number, 0 if only one stream is present
{
    return Record(Opcode::EmitVertex, nullptr, getInt32(streamId), "");
}

// =====================================================================================================================
// In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
Instruction* BuilderRecorder::CreateEndPrimitive(
    uint32_t                streamId)           // Stream number, 0 if only one stream is present
{
    return Record(Opcode::EndPrimitive, nullptr, getInt32(streamId), "");
}

// =====================================================================================================================
// Create a workgroup control barrier.
Instruction* BuilderRecorder::CreateBarrier()
{
    return Record(Opcode::Barrier, nullptr, {}, "");
}

// =====================================================================================================================
// Create a "kill". Only allowed in a fragment shader.
Instruction* BuilderRecorder::CreateKill(
    const Twine& instName)  // [in] Name to give final instruction
{
    return Record(Opcode::Kill, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a matrix transpose.
Value* BuilderRecorder::CreateTransposeMatrix(
    Value* const pMatrix,      // [in] Matrix to transpose.
    const Twine& instName)     // [in] Name to give final instruction
{
    return Record(Opcode::TransposeMatrix, GetTransposedMatrixTy(pMatrix->getType()), { pMatrix }, instName);
}

// =====================================================================================================================
// Create matrix from matrix times scalar
Value* BuilderRecorder::CreateMatrixTimesScalar(
    Value* const pMatrix,             // [in] The matrix
    Value* const pScalar,             // [in] The scalar
    const Twine& instName)            // [in] Name to give instruction(s)
{
    return Record(Opcode::MatrixTimesScalar, pMatrix->getType(), { pMatrix, pScalar }, instName);
}

// =====================================================================================================================
// Create vector from vector times matrix
Value* BuilderRecorder::CreateVectorTimesMatrix(
    Value* const pVector,         // [in] The vector
    Value* const pMatrix,         // [in] The matrix
    const Twine& instName)        // [in] Name to give instruction(s)
{
    Type* const pMatrixType = pMatrix->getType();
    Type* const pCompType = pMatrixType->getArrayElementType()->getVectorElementType();
    const uint32_t columnCount = pMatrixType->getArrayNumElements();
    Type* const pResultTy = VectorType::get(pCompType, columnCount);
    return Record(Opcode::VectorTimesMatrix, pResultTy, { pVector, pMatrix }, instName);
}

// =====================================================================================================================
// Create vector from matrix times vector
Value* BuilderRecorder::CreateMatrixTimesVector(
    Value* const pMatrix,             // [in] The matrix
    Value* const pVector,             // [in] The vector
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Type* const pColumnType = pMatrix->getType()->getArrayElementType();
    Type* const pCompType = pColumnType->getVectorElementType();
    const uint32_t rowCount = pColumnType->getVectorNumElements();
    Type* const pVectorType = VectorType::get(pCompType, rowCount);
    return Record(Opcode::MatrixTimesVector, pVectorType, { pMatrix, pVector }, instName);
}

// =====================================================================================================================
// Create matrix from matrix times matrix
Value* BuilderRecorder::CreateMatrixTimesMatrix(
    Value* const pMatrix1,             // [in] The matrix 1
    Value* const pMatrix2,             // [in] The matrix 2
    const Twine& instName)             // [in] Name to give instruction(s)
{
    Type* const pMat1ColumnType = pMatrix1->getType()->getArrayElementType();
    const uint32_t mat2ColCount = pMatrix2->getType()->getArrayNumElements();
    Type* const pResultTy = ArrayType::get(pMat1ColumnType, mat2ColCount);
    return Record(Opcode::MatrixTimesMatrix, pResultTy, { pMatrix1, pMatrix2 }, instName);
}

// =====================================================================================================================
// Create matrix from outer product of vector
Value* BuilderRecorder::CreateOuterProduct(
    Value* const pVector1,            // [in] The vector 1
    Value* const pVector2,            // [in] The vector 2
    const Twine& instName)            // [in] Name to give instruction(s)
{
    const uint32_t colCount = pVector2->getType()->getVectorNumElements();
    Type* const pResultTy = ArrayType::get(pVector1->getType(), colCount);
    return Record(Opcode::OuterProduct, pResultTy, { pVector1, pVector2 }, instName);
}

// =====================================================================================================================
// Create calculation of matrix determinant
Value* BuilderRecorder::CreateDeterminant(
    Value* const pMatrix,             // [in] Matrix
    const Twine& instName)            // [in] Name to give instruction(s)
{
    return Record(Determinant,
                  pMatrix->getType()->getArrayElementType()->getVectorElementType(),
                  pMatrix,
                  instName);
}

// =====================================================================================================================
// Create calculation of matrix inverse
Value* BuilderRecorder::CreateMatrixInverse(
    Value* const pMatrix,             // [in] Matrix
    const Twine& instName)            // [in] Name to give instruction(s)
{
    return Record(MatrixInverse, pMatrix->getType(), pMatrix, instName);
}

// =====================================================================================================================
// Create a "readclock".
Instruction* BuilderRecorder::CreateReadClock(
    bool         realtime,   // Whether to read real-time clock counter
    const Twine& instName)   // [in] Name to give final instruction
{
    return Record(Opcode::ReadClock, getInt64Ty(), getInt1(realtime), instName);
}

// =====================================================================================================================
// Create tan operation
Value* BuilderRecorder::CreateTan(
        Value*        pX,        // [in] Input value X
        const Twine&  instName)  // [in] Name to give final instruction)
{
    return Record(Tan, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create arc sin operation
Value* BuilderRecorder::CreateASin(
        Value*        pX,        // [in] Input value X
        const Twine&  instName)  // [in] Name to give final instruction)
{
    return Record(ASin, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create arc cos operation
Value* BuilderRecorder::CreateACos(
        Value*        pX,        // [in] Input value X
        const Twine&  instName)  // [in] Name to give final instruction)
{
    return Record(ACos, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create arc tan operation
Value* BuilderRecorder::CreateATan(
        Value*        pYOverX,    // [in] Input value Y/X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(ATan, pYOverX->getType(), pYOverX, instName, {});
}

// =====================================================================================================================
// Create arc tan operation with result in the correct quadrant for the signs of the inputs
Value* BuilderRecorder::CreateATan2(
        Value*        pY,         // [in] Input value Y
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(ATan2, pY->getType(), { pY, pX }, instName, {});
}

// =====================================================================================================================
// Create hyperbolic sin operation
Value* BuilderRecorder::CreateSinh(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(Sinh, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create hyperbolic cos operation
Value* BuilderRecorder::CreateCosh(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(Cosh, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create hyperbolic tan operation
Value* BuilderRecorder::CreateTanh(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(Tanh, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create hyperbolic arc sin operation
Value* BuilderRecorder::CreateASinh(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(ASinh, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create hyperbolic arc cos operation
Value* BuilderRecorder::CreateACosh(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(ACosh, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create hyperbolic arc tan operation
Value* BuilderRecorder::CreateATanh(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(ATanh, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create power operation
Value* BuilderRecorder::CreatePower(
        Value*        pX,         // [in] Input value X
        Value*        pY,         // [in] Input value Y
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(Power, pX->getType(), { pX, pY }, instName, {});
}

// =====================================================================================================================
// Create exp operation
Value* BuilderRecorder::CreateExp(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(Exp, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create natural log operation
Value* BuilderRecorder::CreateLog(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(Log, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create inverse square root operation
Value* BuilderRecorder::CreateInverseSqrt(
        Value*        pX,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return Record(InverseSqrt, pX->getType(), pX, instName, {});
}

// =====================================================================================================================
// Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
// the given cube map texture coordinates. Returns <2 x float>.
Value* BuilderRecorder::CreateCubeFaceCoord(
    Value*        pCoord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::CubeFaceCoord, VectorType::get(pCoord->getType()->getScalarType(), 2), pCoord, instName);
}

// =====================================================================================================================
// Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
// the given cube map texture coordinates.
Value* BuilderRecorder::CreateCubeFaceIndex(
    Value*        pCoord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::CubeFaceIndex, pCoord->getType()->getScalarType(), pCoord, instName);
}

// =====================================================================================================================
// Create "signed integer abs" operation for a scalar or vector integer value.
Value* BuilderRecorder::CreateSAbs(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::SAbs, pX->getType(), pX, instName);
}

// =====================================================================================================================
// Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
// value is negative, zero or positive.
Value* BuilderRecorder::CreateFSign(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FSign, pX->getType(), pX, instName);
}

// =====================================================================================================================
// Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
// value is negative, zero or positive.
Value* BuilderRecorder::CreateSSign(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::SSign, pX->getType(), pX, instName);
}

// =====================================================================================================================
// Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
Value* BuilderRecorder::CreateFract(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::Fract, pX->getType(), pX, instName);
}

// =====================================================================================================================
// Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
// interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
// t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
// Result is undefined if edge0 >= edge1.
Value* BuilderRecorder::CreateSmoothStep(
    Value*        pEdge0,     // [in] Edge0 value
    Value*        pEdge1,     // [in] Edge1 value
    Value*        pX,         // [in] X (input) value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::SmoothStep, pX->getType(), { pEdge0, pEdge1, pX }, instName);
}

// =====================================================================================================================
// Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
Value* BuilderRecorder::CreateLdexp(
    Value*        pX,         // [in] Mantissa
    Value*        pExp,       // [in] Exponent
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::Ldexp, pX->getType(), { pX, pExp }, instName);
}

// =====================================================================================================================
// Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
// [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
// the result is undefined.
Value* BuilderRecorder::CreateExtractSignificand(
    Value*        pValue,   // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::ExtractSignificand, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
// If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
// If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
Value* BuilderRecorder::CreateExtractExponent(
    Value*        pValue,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Type* pResultTy = getInt32Ty();
    if (pValue->getType()->getScalarType()->isHalfTy())
    {
        pResultTy = getInt16Ty();
    }
    pResultTy = GetConditionallyVectorizedTy(pResultTy, pValue->getType());
    return Record(Opcode::ExtractExponent, pResultTy, pValue, instName);
}

// =====================================================================================================================
// Create vector cross product operation. Inputs must be <3 x FP>
Value* BuilderRecorder::CreateCrossProduct(
    Value*        pX,         // [in] Input value X
    Value*        pY,         // [in] Input value Y
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::CrossProduct, pX->getType(), { pX, pY }, instName);
}

// =====================================================================================================================
// Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
Value* BuilderRecorder::CreateNormalizeVector(
    Value*        pX,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::NormalizeVector, pX->getType(), pX, instName);
}

// =====================================================================================================================
// Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
// Nref and I is negative, the result is N, otherwise it is -N
Value* BuilderRecorder::CreateFaceForward(
    Value*        pN,         // [in] Input value "N"
    Value*        pI,         // [in] Input value "I"
    Value*        pNref,      // [in] Input value "Nref"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FaceForward, pN->getType(), { pN, pI, pNref }, instName);
}

// =====================================================================================================================
// Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
// the reflection direction:
// I - 2 * dot(N, I) * N
Value* BuilderRecorder::CreateReflect(
    Value*        pI,         // [in] Input value "I"
    Value*        pN,         // [in] Input value "N"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::Reflect, pN->getType(), { pI, pN }, instName);
}

// =====================================================================================================================
// Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
// of indices of refraction eta, the result is the refraction vector:
// k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
// If k < 0.0 the result is 0.0.
// Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
Value* BuilderRecorder::CreateRefract(
    Value*        pI,         // [in] Input value "I"
    Value*        pN,         // [in] Input value "N"
    Value*        pEta,       // [in] Input value "eta"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::Refract, pN->getType(), { pI, pN, pEta }, instName);
}

// =====================================================================================================================
// Create scalar or vector FP truncate operation with the given rounding mode.
// Currently only implemented for float/double -> half conversion.
Value* BuilderRecorder::CreateFpTruncWithRounding(
    Value*            pValue,             // [in] Input value
    Type*             pDestTy,            // [in] Type to convert to
    unsigned          roundingMode,       // Rounding mode
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::FpTruncWithRounding, pDestTy, { pValue, getInt32(roundingMode) }, instName);
}

// =====================================================================================================================
// Create quantize operation.
Value* BuilderRecorder::CreateQuantizeToFp16(
    Value*        pValue,     // [in] Input value (float or float vector)
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::QuantizeToFp16, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create signed integer modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderRecorder::CreateSMod(
    Value*        pDividend,  // [in] Dividend value
    Value*        pDivisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::SMod, pDividend->getType(), { pDividend, pDivisor }, instName);
}

// =====================================================================================================================
// Create FP modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderRecorder::CreateFMod(
    Value*        pDividend,  // [in] Dividend value
    Value*        pDivisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FMod, pDividend->getType(), { pDividend, pDivisor }, instName);
}

// =====================================================================================================================
// Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
Value* BuilderRecorder::CreateFma(
    Value*        pA,         // [in] One value to multiply
    Value*        pB,         // [in] The other value to multiply
    Value*        pC,         // [in] The value to add to the product of A and B
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::Fma, pA->getType(), { pA, pB, pC }, instName);
}

// =====================================================================================================================
// Create derivative calculation on float or vector of float or half
Value* BuilderRecorder::CreateDerivative(
    Value*        pValue,       // [in] Input value
    bool          isDirectionY, // False for derivative in X direction, true for Y direction
    bool          isFine,       // True for "fine" calculation, false for "coarse" calculation.
    const Twine&  instName)     // [in] Name to give instruction(s)
{
    return Record(Opcode::Derivative, pValue->getType(), { pValue, getInt1(isDirectionY), getInt1(isFine) }, instName);
}

// =====================================================================================================================
// Create a demote to helper invocation.
Instruction* BuilderRecorder::CreateDemoteToHelperInvocation(
    const Twine& instName)   // [in] Name to give final instruction
{
    return Record(Opcode::DemoteToHelperInvocation, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a helper invocation query.
Value* BuilderRecorder::CreateIsHelperInvocation(
    const Twine& instName)   // [in] Name to give final instruction
{
    return Record(Opcode::IsHelperInvocation, getInt1Ty(), {}, instName);
}

// =====================================================================================================================
// Create "fclamp" operation.
Value* BuilderRecorder::CreateFClamp(
    Value*        pX,         // [in] Value to clamp
    Value*        pMinVal,    // [in] Minimum of clamp range
    Value*        pMaxVal,    // [in] Maximum of clamp range
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FClamp, pX->getType(), { pX, pMinVal, pMaxVal }, instName);
}

// =====================================================================================================================
// Create "fmin" operation, returning the minimum of two scalar or vector FP values.
Value* BuilderRecorder::CreateFMin(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FMin, pValue1->getType(), { pValue1, pValue2 }, instName);
}

// =====================================================================================================================
// Create "fmax" operation, returning the maximum of three scalar or vector FP values.
Value* BuilderRecorder::CreateFMax(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FMax, pValue1->getType(), { pValue1, pValue2 }, instName);
}

// =====================================================================================================================
// Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
Value* BuilderRecorder::CreateFMin3(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    Value*        pValue3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FMin3, pValue1->getType(), { pValue1, pValue2, pValue3 }, instName);
}

// =====================================================================================================================
// Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
Value* BuilderRecorder::CreateFMax3(
    Value*        pValue1,    // [in] First value
    Value*        pValue2,    // [in] Second value
    Value*        pValue3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FMax3, pValue1->getType(), { pValue1, pValue2, pValue3 }, instName);
}

// =====================================================================================================================
// Create "fmid3" operation, returning the middle one of three float values.
Value* BuilderRecorder::CreateFMid3(
    Value*        pValue1,              // [in] First value
    Value*        pValue2,              // [in] Second value
    Value*        pValue3,              // [in] Third value
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    return Record(Opcode::FMid3, pValue1->getType(), { pValue1, pValue2, pValue3 }, instName);
}

// =====================================================================================================================
// Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
Value* BuilderRecorder::CreateIsInf(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::IsInf, GetConditionallyVectorizedTy(getInt1Ty(), pX->getType()), pX, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
Value* BuilderRecorder::CreateIsNaN(
    Value*        pX,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::IsNaN, GetConditionallyVectorizedTy(getInt1Ty(), pX->getType()), pX, instName);
}

// =====================================================================================================================
// Create an "insert bitfield" operation for a (vector of) integer type.
// Returns a value where the "pCount" bits starting at bit "pOffset" come from the least significant "pCount"
// bits in "pInsert", and remaining bits come from "pBase". The result is undefined if "pCount"+"pOffset" is
// more than the number of bits (per vector element) in "pBase" and "pInsert".
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width.
Value* BuilderRecorder::CreateInsertBitField(
    Value*        pBase,                // [in] Base value
    Value*        pInsert,              // [in] Value to insert (same type as base)
    Value*        pOffset,              // Bit number of least-significant end of bitfield
    Value*        pCount,               // Count of bits in bitfield
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    return Record(Opcode::InsertBitField, pBase->getType(), { pBase, pInsert, pOffset, pCount }, instName);
}

// =====================================================================================================================
// Create an "extract bitfield " operation for a (vector of) i32.
// Returns a value where the least significant "pCount" bits come from the "pCount" bits starting at bit
// "pOffset" in "pBase", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width.
Value* BuilderRecorder::CreateExtractBitField(
    Value*        pBase,                // [in] Base value
    Value*        pOffset,              // Bit number of least-significant end of bitfield
    Value*        pCount,               // Count of bits in bitfield
    bool          isSigned,             // True for a signed int bitfield extract, false for unsigned
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    return Record(Opcode::ExtractBitField, pBase->getType(), { pBase, pOffset, pCount, getInt1(isSigned) }, instName);
}

// =====================================================================================================================
// Create "find MSB" operation for a (vector of) signed int.
Value* BuilderRecorder::CreateFindSMsb(
    Value*        pValue,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return Record(Opcode::FindSMsb, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create a load of a buffer descriptor.
Value* BuilderRecorder::CreateLoadBufferDesc(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    Value*        pDescIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    bool          isWritten,        // Whether the buffer is written to
    Type*         pPointeeTy,       // [in] Type that the returned pointer should point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::LoadBufferDesc,
                  GetBufferDescTy(pPointeeTy),
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      pDescIndex,
                      getInt1(isNonUniform),
                      getInt1(isWritten),
                  },
                  instName);
}

// =====================================================================================================================
// Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
Value* BuilderRecorder::CreateIndexDescPtr(
    Value*        pDescPtr,           // [in] Descriptor pointer, as returned by this function or one of
                                      //    the CreateGet*DescPtr methods
    Value*        pIndex,             // [in] Index value
    bool          isNonUniform,       // Whether the descriptor index is non-uniform
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    LLPC_ASSERT((pDescPtr->getType() == GetImageDescPtrTy()) ||
                (pDescPtr->getType() == GetSamplerDescPtrTy()) ||
                (pDescPtr->getType() == GetFmaskDescPtrTy()) ||
                (pDescPtr->getType() == GetTexelBufferDescPtrTy()));
    return Record(Opcode::IndexDescPtr, pDescPtr->getType(), { pDescPtr, pIndex, getInt1(isNonUniform) }, instName);
}

// =====================================================================================================================
// Load image/sampler/texelbuffer/F-mask descriptor from pointer.
// Returns <8 x i32> descriptor for image, sampler or F-mask, or <4 x i32> descriptor for texel buffer.
Value* BuilderRecorder::CreateLoadDescFromPtr(
    Value*        pDescPtr,           // [in] Descriptor pointer, as returned by CreateIndexDescPtr or one of
                                      //    the CreateGet*DescPtr methods
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    LLPC_ASSERT((pDescPtr->getType() == GetImageDescPtrTy()) ||
                (pDescPtr->getType() == GetSamplerDescPtrTy()) ||
                (pDescPtr->getType() == GetFmaskDescPtrTy()) ||
                (pDescPtr->getType() == GetTexelBufferDescPtrTy()));
    return Record(Opcode::LoadDescFromPtr,
                  cast<StructType>(pDescPtr->getType())->getElementType(0)->getPointerElementType(),
                  pDescPtr,
                  instName);
}

// =====================================================================================================================
// Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
Value* BuilderRecorder::CreateGetSamplerDescPtr(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::GetSamplerDescPtr, GetSamplerDescPtrTy(), { getInt32(descSet), getInt32(binding) }, instName);
}

// =====================================================================================================================
// Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
Value* BuilderRecorder::CreateGetImageDescPtr(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::GetImageDescPtr, GetImageDescPtrTy(), { getInt32(descSet), getInt32(binding) }, instName);
}

// =====================================================================================================================
// Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
Value* BuilderRecorder::CreateGetTexelBufferDescPtr(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::GetTexelBufferDescPtr,
                  GetTexelBufferDescPtrTy(),
                  { getInt32(descSet), getInt32(binding) },
                  instName);
}

// =====================================================================================================================
// Create a load of a F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
Value* BuilderRecorder::CreateGetFmaskDescPtr(
    uint32_t      descSet,          // Descriptor set
    uint32_t      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return Record(Opcode::GetFmaskDescPtr, GetFmaskDescPtrTy(), { getInt32(descSet), getInt32(binding) }, instName);
}

// =====================================================================================================================
// Create a load of the spill table pointer for push constants.
Value* BuilderRecorder::CreateLoadPushConstantsPtr(
    Type*         pPushConstantsTy, // [in] Type of the push constants table that the returned pointer will point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    Type* pResultTy = PointerType::get(pPushConstantsTy, ADDR_SPACE_CONST);
    return Record(Opcode::LoadPushConstantsPtr, pResultTy, {}, instName);
}

// =====================================================================================================================
// Create a buffer length query based on the specified descriptor.
Value* BuilderRecorder::CreateGetBufferDescLength(
    Value* const  pBufferDesc,      // [in] The buffer descriptor to query.
    const Twine&  instName)         // [in] Name to give instruction(s).
{
    return Record(Opcode::GetBufferDescLength, getInt32Ty(), { pBufferDesc }, instName);
}

// =====================================================================================================================
// Create an image load.
Value* BuilderRecorder::CreateImageLoad(
    Type*                   pResultTy,          // [in] Result type
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32
    Value*                  pMipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    SmallVector<Value*, 5> args;
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(pImageDesc);
    args.push_back(pCoord);
    if (pMipLevel != nullptr)
    {
        args.push_back(pMipLevel);
    }
    return Record(Opcode::ImageLoad, pResultTy, args, instName);
}

// =====================================================================================================================
// Create an image load with F-mask.
Value* BuilderRecorder::CreateImageLoadWithFmask(
    Type*                   pResultTy,          // [in] Result type
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pFmaskDesc,         // [in] Fmask descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32, exactly right
                                                //    width for given dimension excluding sample
    Value*                  pSampleNum,         // [in] Sample number, i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ImageLoadWithFmask,
                  pResultTy,
                  { getInt32(dim), getInt32(flags), pImageDesc, pFmaskDesc, pCoord, pSampleNum },
                  instName);
}

// =====================================================================================================================
// Create an image store.
Value* BuilderRecorder::CreateImageStore(
    Value*            pTexel,             // [in] Texel value to store; v4f16 or v4f32
    uint32_t          dim,                // Image dimension
    uint32_t          flags,              // ImageFlag* flags
    Value*            pImageDesc,         // [in] Image descriptor
    Value*            pCoord,             // [in] Coordinates: scalar or vector i32
    Value*            pMipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    SmallVector<Value*, 6> args;
    args.push_back(pTexel);
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(pImageDesc);
    args.push_back(pCoord);
    if (pMipLevel != nullptr)
    {
        args.push_back(pMipLevel);
    }
    return Record(Opcode::ImageStore, nullptr, args, instName);
}

// =====================================================================================================================
// Create an image sample.
Value* BuilderRecorder::CreateImageSample(
    Type*               pResultTy,          // [in] Result type
    uint32_t            dim,                // Image dimension
    uint32_t            flags,              // ImageFlag* flags
    Value*              pImageDesc,         // [in] Image descriptor
    Value*              pSamplerDesc,       // [in] Sampler descriptor
    ArrayRef<Value*>    address,            // Address and other arguments
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    // Gather a mask of address elements that are not nullptr.
    uint32_t addressMask = 0;
    for (uint32_t i = 0; i != address.size(); ++i)
    {
        if (address[i] != nullptr)
        {
            addressMask |= 1U << i;
        }
    }

    SmallVector<Value*, 8> args;
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(pImageDesc);
    args.push_back(pSamplerDesc);
    args.push_back(getInt32(addressMask));
    for (uint32_t i = 0; i != address.size(); ++i)
    {
        if (address[i] != nullptr)
        {
            args.push_back(address[i]);
        }
    }
    return Record(Opcode::ImageSample, pResultTy, args, instName);
}

// =====================================================================================================================
// Create an image gather.
Value* BuilderRecorder::CreateImageGather(
    Type*               pResultTy,          // [in] Result type
    uint32_t            dim,                // Image dimension
    uint32_t            flags,              // ImageFlag* flags
    Value*              pImageDesc,         // [in] Image descriptor
    Value*              pSamplerDesc,       // [in] Sampler descriptor
    ArrayRef<Value*>    address,            // Address and other arguments
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    // Gather a mask of address elements that are not nullptr.
    uint32_t addressMask = 0;
    for (uint32_t i = 0; i != address.size(); ++i)
    {
        if (address[i] != nullptr)
        {
            addressMask |= 1U << i;
        }
    }

    SmallVector<Value*, 8> args;
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(pImageDesc);
    args.push_back(pSamplerDesc);
    args.push_back(getInt32(addressMask));
    for (uint32_t i = 0; i != address.size(); ++i)
    {
        if (address[i] != nullptr)
        {
            args.push_back(address[i]);
        }
    }
    return Record(Opcode::ImageGather, pResultTy, args, instName);
}

// =====================================================================================================================
// Create an image atomic operation other than compare-and-swap.
Value* BuilderRecorder::CreateImageAtomic(
    uint32_t                atomicOp,           // Atomic op to create
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32
    Value*                  pInputValue,        // [in] Input value: i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ImageAtomic,
                  pInputValue->getType(),
                  {
                      getInt32(atomicOp),
                      getInt32(dim),
                      getInt32(flags),
                      getInt32(static_cast<uint32_t>(ordering)),
                      pImageDesc,
                      pCoord,
                      pInputValue
                  },
                  instName);
}

// =====================================================================================================================
// Create an image atomic compare-and-swap.
Value* BuilderRecorder::CreateImageAtomicCompareSwap(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pCoord,             // [in] Coordinates: scalar or vector i32
    Value*                  pInputValue,        // [in] Input value: i32
    Value*                  pComparatorValue,   // [in] Value to compare against: i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ImageAtomicCompareSwap,
                  pInputValue->getType(),
                  {
                      getInt32(dim),
                      getInt32(flags),
                      getInt32(static_cast<uint32_t>(ordering)),
                      pImageDesc,
                      pCoord,
                      pInputValue,
                      pComparatorValue
                  },
                  instName);
}

// =====================================================================================================================
// Create a query of the number of mipmap levels in an image. Returns an i32 value.
Value* BuilderRecorder::CreateImageQueryLevels(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ImageQueryLevels, getInt32Ty(), { getInt32(dim), getInt32(flags), pImageDesc }, instName);
}

// =====================================================================================================================
// Create a query of the number of samples in an image. Returns an i32 value.
Value* BuilderRecorder::CreateImageQuerySamples(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ImageQuerySamples, getInt32Ty(), { getInt32(dim), getInt32(flags), pImageDesc }, instName);
}

// =====================================================================================================================
// Create a query of size of an image.
// Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
Value* BuilderRecorder::CreateImageQuerySize(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor or texel buffer descriptor
    Value*                  pLod,               // [in] LOD
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    uint32_t compCount = GetImageQuerySizeComponentCount(dim);
    Type* pResultTy = getInt32Ty();
    if (compCount > 1)
    {
        pResultTy = VectorType::get(pResultTy, compCount);
    }
    return Record(Opcode::ImageQuerySize, pResultTy, { getInt32(dim), getInt32(flags), pImageDesc, pLod }, instName);
}

// =====================================================================================================================
// Create a get of the LOD that would be used for an image sample with the given coordinates
// and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
// detail relative to the base level.
Value* BuilderRecorder::CreateImageGetLod(
    uint32_t                dim,                // Image dimension
    uint32_t                flags,              // ImageFlag* flags
    Value*                  pImageDesc,         // [in] Image descriptor
    Value*                  pSamplerDesc,       // [in] Sampler descriptor
    Value*                  pCoord,             // [in] Coordinates
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ImageGetLod,
                  VectorType::get(getFloatTy(), 2),
                  { getInt32(dim), getInt32(flags), pImageDesc, pSamplerDesc, pCoord },
                  instName);
}

// =====================================================================================================================
// Create a read of (part of) a user input value, passed from the previous shader stage.
Value* BuilderRecorder::CreateReadGenericInput(
    Type*         pResultTy,          // [in] Type of value to read
    uint32_t      location,           // Base location (row) of input
    Value*        pLocationOffset,    // [in] Variable location offset; must be within locationCount
    Value*        pElemIdx,           // [in] Vector index
    uint32_t      locationCount,      // Count of locations taken by the input
    InOutInfo     inputInfo,          // Extra input info (FS interp info)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ReadGenericInput,
                  pResultTy,
                  {
                      getInt32(location),
                      pLocationOffset,
                      pElemIdx,
                      getInt32(locationCount),
                      getInt32(inputInfo.GetData()),
                      (pVertexIndex != nullptr) ? pVertexIndex : UndefValue::get(getInt32Ty()),
                  },
                  instName,
                  Attribute::ReadOnly);
}

// =====================================================================================================================
// Create a read of (part of) a user output value, the last written value in the same shader stage.
Value* BuilderRecorder::CreateReadGenericOutput(
    Type*         pResultTy,          // [in] Type of value to read
    uint32_t      location,           // Base location (row) of input
    Value*        pLocationOffset,    // [in] Variable location offset; must be within locationCount
    Value*        pElemIdx,           // [in] Vector index
    uint32_t      locationCount,      // Count of locations taken by the input
    InOutInfo     outputInfo,         // Extra output info
    Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    return Record(Opcode::ReadGenericOutput,
                  pResultTy,
                  {
                      getInt32(location),
                      pLocationOffset,
                      pElemIdx,
                      getInt32(locationCount),
                      getInt32(outputInfo.GetData()),
                      (pVertexIndex != nullptr) ? pVertexIndex : UndefValue::get(getInt32Ty()),
                  },
                  instName,
                  Attribute::ReadOnly);
}

// =====================================================================================================================
// Create a write of (part of) a user output value, setting the value to pass to the next shader stage.
// The value to write must be a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// A non-constant pLocationOffset is currently only supported for TCS.
Instruction* BuilderRecorder::CreateWriteGenericOutput(
    Value*        pValueToWrite,      // [in] Value to write
    uint32_t      location,           // Base location (row) of output
    Value*        pLocationOffset,    // [in] Location offset; must be within locationCount if variable
    Value*        pElemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                      //      that it is half the component for 64-bit elements.)
    uint32_t      locationCount,      // Count of locations taken by the output. Ignored if pLocationOffset is const
    InOutInfo     outputInfo,         // Extra output info (GS stream ID, FS integer signedness)
    Value*        pVertexIndex)       // [in] For TCS per-vertex output: vertex index; else nullptr
{
    return Record(Opcode::WriteGenericOutput,
                  nullptr,
                  {
                      pValueToWrite,
                      getInt32(location),
                      pLocationOffset,
                      pElemIdx,
                      getInt32(locationCount),
                      getInt32(outputInfo.GetData()),
                      (pVertexIndex != nullptr) ? pVertexIndex : UndefValue::get(getInt32Ty()),
                  },
                  "",
                  {});
}

// =====================================================================================================================
// Create a write to an XFB (transform feedback / streamout) buffer.
Instruction* BuilderRecorder::CreateWriteXfbOutput(
    Value*        pValueToWrite,      // [in] Value to write
    bool          isBuiltIn,          // True for built-in, false for user output (ignored if not GS)
    uint32_t      location,           // Location (row) or built-in kind of output (ignored if not GS)
    uint32_t      xfbBuffer,          // XFB buffer ID
    uint32_t      xfbStride,          // XFB stride
    Value*        pXfbOffset,         // [in] XFB byte offset
    InOutInfo     outputInfo)         // Extra output info (GS stream ID)
{
    return Record(Opcode::WriteXfbOutput,
                  nullptr,
                  {
                      pValueToWrite,
                      getInt1(isBuiltIn),
                      getInt32(location),
                      getInt32(xfbBuffer),
                      getInt32(xfbStride),
                      pXfbOffset, getInt32(outputInfo.GetData())
                  },
                  "",
                  {});
}

// =====================================================================================================================
// Create a read of (part of) a built-in input value.
// The type of the returned value is the fixed type of the specified built-in (see llpcBuilderBuiltInDefs.h),
// or the element type if pIndex is not nullptr.
Value* BuilderRecorder::CreateReadBuiltInInput(
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     inputInfo,          // Extra input info (shader-defined array length)
    Value*        pVertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
    Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    Type* pResultTy = GetBuiltInTy(builtIn, inputInfo);
    if (pIndex != nullptr)
    {
        if (isa<ArrayType>(pResultTy))
        {
            pResultTy = pResultTy->getArrayElementType();
        }
        else
        {
            pResultTy = pResultTy->getVectorElementType();
        }
    }
    return Record(Opcode::ReadBuiltInInput,
                  pResultTy,
                  {
                      getInt32(builtIn),
                      getInt32(inputInfo.GetData()),
                      (pVertexIndex != nullptr) ? pVertexIndex : UndefValue::get(getInt32Ty()),
                      (pIndex != nullptr) ? pIndex : UndefValue::get(getInt32Ty()),
                  },
                  instName,
                  Attribute::ReadOnly);
}

// =====================================================================================================================
// Create a read of (part of) a built-in output value.
// The type of the returned value is the fixed type of the specified built-in (see llpcBuilderBuiltInDefs.h),
// or the element type if pIndex is not nullptr.
Value* BuilderRecorder::CreateReadBuiltInOutput(
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     outputInfo,         // Extra output info (shader-defined array length)
    Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    Value*        pIndex,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    Type* pResultTy = GetBuiltInTy(builtIn, outputInfo);
    if (pIndex != nullptr)
    {
        if (isa<ArrayType>(pResultTy))
        {
            pResultTy = pResultTy->getArrayElementType();
        }
        else
        {
            pResultTy = pResultTy->getVectorElementType();
        }
    }
    return Record(Opcode::ReadBuiltInOutput,
                  pResultTy,
                  {
                      getInt32(builtIn),
                      getInt32(outputInfo.GetData()),
                      (pVertexIndex != nullptr) ? pVertexIndex : UndefValue::get(getInt32Ty()),
                      (pIndex != nullptr) ? pIndex : UndefValue::get(getInt32Ty()),
                  },
                  instName,
                  Attribute::ReadOnly);
}

// =====================================================================================================================
// Create a write of (part of) a built-in output value.
Instruction* BuilderRecorder::CreateWriteBuiltInOutput(
    Value*        pValueToWrite,      // [in] Value to write
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     outputInfo,         // Extra output info (shader-defined array length; GS stream id)
    Value*        pVertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    Value*        pIndex)             // [in] Array or vector index to access part of an input, else nullptr
{
    return Record(Opcode::WriteBuiltInOutput,
                  nullptr,
                  {
                      pValueToWrite,
                      getInt32(builtIn),
                      getInt32(outputInfo.GetData()),
                      (pVertexIndex != nullptr) ? pVertexIndex : UndefValue::get(getInt32Ty()),
                      (pIndex != nullptr) ? pIndex : UndefValue::get(getInt32Ty()),
                  },
                  "");
}

// =====================================================================================================================
// Create a get subgroup size query.
Value* BuilderRecorder::CreateGetSubgroupSize(
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::GetSubgroupSize, getInt32Ty(), {}, instName);
}

// =====================================================================================================================
// Create a subgroup elect.
Value* BuilderRecorder::CreateSubgroupElect(
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupElect, getInt1Ty(), {}, instName);
}

// =====================================================================================================================
// Create a subgroup all.
Value* BuilderRecorder::CreateSubgroupAll(
    Value* const pValue,   // [in] The value to compare
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupAll, getInt1Ty(), { pValue,  getInt1(wqm) }, instName);
}

// =====================================================================================================================
// Create a subgroup any
Value* BuilderRecorder::CreateSubgroupAny(
    Value* const pValue,   // [in] The value to compare
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupAny, getInt1Ty(), { pValue,  getInt1(wqm) }, instName);
}

// =====================================================================================================================
// Create a subgroup all equal.
Value* BuilderRecorder::CreateSubgroupAllEqual(
    Value* const pValue,   // [in] The value to compare
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupAllEqual, getInt1Ty(), { pValue,  getInt1(wqm) }, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast.
Value* BuilderRecorder::CreateSubgroupBroadcast(
    Value* const pValue,   // [in] The value to broadcast
    Value* const pIndex,   // [in] The index to broadcast from
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBroadcast, pValue->getType(), { pValue, pIndex }, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast first.
Value* BuilderRecorder::CreateSubgroupBroadcastFirst(
    Value* const pValue,   // [in] The value to broadcast
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBroadcastFirst, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup ballot.
Value* BuilderRecorder::CreateSubgroupBallot(
    Value* const pValue,   // [in] The value to contribute
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallot, VectorType::get(getInt32Ty(), 4), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup inverse ballot.
Value* BuilderRecorder::CreateSubgroupInverseBallot(
    Value* const pValue,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupInverseBallot, getInt1Ty(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup ballot bit extract.
Value* BuilderRecorder::CreateSubgroupBallotBitExtract(
    Value* const pValue,   // [in] The ballot value
    Value* const pIndex,   // [in] The index to extract from the ballot
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallotBitExtract, getInt1Ty(), { pValue, pIndex }, instName);
}

// =====================================================================================================================
// Create a subgroup ballot bit count.
Value* BuilderRecorder::CreateSubgroupBallotBitCount(
    Value* const pValue,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallotBitCount, getInt32Ty(), pValue, instName);
}

// Create a subgroup ballot inclusive bit count.
Value* BuilderRecorder::CreateSubgroupBallotInclusiveBitCount(
    Value* const pValue,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallotInclusiveBitCount, getInt32Ty(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup ballot exclusive bit count.
Value* BuilderRecorder::CreateSubgroupBallotExclusiveBitCount(
    Value* const pValue,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallotExclusiveBitCount, getInt32Ty(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup ballot find least significant bit.
Value* BuilderRecorder::CreateSubgroupBallotFindLsb(
    Value* const pValue,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallotFindLsb, getInt32Ty(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup ballot find most significant bit.
Value* BuilderRecorder::CreateSubgroupBallotFindMsb(
    Value* const pValue,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupBallotFindMsb, getInt32Ty(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle.
Value* BuilderRecorder::CreateSubgroupShuffle(
    Value* const pValue,   // [in] The value to shuffle
    Value* const pIndex,   // [in] The index to shuffle from
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupShuffle, pValue->getType(), { pValue, pIndex }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle xor.
Value* BuilderRecorder::CreateSubgroupShuffleXor(
    Value* const pValue,   // [in] The value to shuffle
    Value* const pMask,    // [in] The mask to shuffle with
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupShuffleXor, pValue->getType(), { pValue, pMask }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle up.
Value* BuilderRecorder::CreateSubgroupShuffleUp(
    Value* const pValue,   // [in] The value to shuffle
    Value* const pOffset,  // [in] The offset to shuffle up to
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupShuffleUp, pValue->getType(), { pValue, pOffset }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle down.
Value* BuilderRecorder::CreateSubgroupShuffleDown(
    Value* const pValue,   // [in] The value to shuffle
    Value* const pOffset,  // [in] The offset to shuffle down to
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupShuffleDown, pValue->getType(), { pValue, pOffset }, instName);
}

// =====================================================================================================================
// Create a subgroup clustered reduction.
Value* BuilderRecorder::CreateSubgroupClusteredReduction(
    GroupArithOp groupArithOp, // The group operation to perform
    Value* const pValue,       // [in] The value to perform on
    Value* const pClusterSize, // [in] The cluster size
    const Twine& instName)     // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupClusteredReduction,
                  pValue->getType(),
                  {
                      getInt32(groupArithOp),
                      pValue,
                      pClusterSize
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup clustered inclusive scan.
Value* BuilderRecorder::CreateSubgroupClusteredInclusive(
    GroupArithOp groupArithOp, // The group operation to perform
    Value* const pValue,       // [in] The value to perform on
    Value* const pClusterSize, // [in] The cluster size
    const Twine& instName)     // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupClusteredInclusive,
                  pValue->getType(),
                  {
                      getInt32(groupArithOp),
                      pValue,
                      pClusterSize
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup clustered exclusive scan.
Value* BuilderRecorder::CreateSubgroupClusteredExclusive(
    GroupArithOp groupArithOp, // The group operation to perform
    Value* const pValue,       // [in] The value to perform on
    Value* const pClusterSize, // [in] The cluster size
    const Twine& instName)     // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupClusteredExclusive,
                  pValue->getType(),
                  {
                      getInt32(groupArithOp),
                      pValue,
                      pClusterSize
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup quad broadcast.
Value* BuilderRecorder::CreateSubgroupQuadBroadcast(
    Value* const pValue,   // [in] The value to broadcast
    Value* const pIndex,   // [in] The index within the quad to broadcast from
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupQuadBroadcast, pValue->getType(), { pValue, pIndex }, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap horizontal.
Value* BuilderRecorder::CreateSubgroupQuadSwapHorizontal(
    Value* const pValue,   // [in] The value to swap
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupQuadSwapHorizontal, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap vertical.
Value* BuilderRecorder::CreateSubgroupQuadSwapVertical(
    Value* const pValue,   // [in] The value to swap
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupQuadSwapVertical, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap diagonal.
Value* BuilderRecorder::CreateSubgroupQuadSwapDiagonal(
    Value* const pValue,   // [in] The value to swap
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupQuadSwapDiagonal, pValue->getType(), pValue, instName);
}

// =====================================================================================================================
// Create a subgroup swizzle quad.
Value* BuilderRecorder::CreateSubgroupSwizzleQuad(
    Value* const pValue,   // [in] The value to swizzle.
    Value* const pOffset,  // [in] The value to specify the swizzle offsets.
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupSwizzleQuad, pValue->getType(), { pValue, pOffset }, instName);
}

// =====================================================================================================================
// Create a subgroup swizzle mask.
Value* BuilderRecorder::CreateSubgroupSwizzleMask(
    Value* const pValue,   // [in] The value to swizzle.
    Value* const pMask,    // [in] The value to specify the swizzle masks.
    const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupSwizzleMask, pValue->getType(), { pValue, pMask }, instName);
}

// =====================================================================================================================
// Create a subgroup write invocation.
Value* BuilderRecorder::CreateSubgroupWriteInvocation(
        Value* const pInputValue, // [in] The value to return for all but one invocations.
        Value* const pWriteValue, // [in] The value to return for one invocation.
        Value* const pIndex,      // [in] The index of the invocation that gets the write value.
        const Twine& instName)    // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupWriteInvocation,
                  pInputValue->getType(),
                  {
                        pInputValue,
                        pWriteValue,
                        pIndex
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup mbcnt.
Value* BuilderRecorder::CreateSubgroupMbcnt(
        Value* const pMask,    // [in] The mask to mbcnt with.
        const Twine& instName) // [in] Name to give instruction(s)
{
    return Record(Opcode::SubgroupMbcnt, getInt32Ty(), pMask, instName);
}

// =====================================================================================================================
// Record one Builder call
Instruction* BuilderRecorder::Record(
    BuilderRecorder::Opcode       opcode,       // Opcode of Builder method call being recorded
    Type*                         pResultTy,    // [in] Return type (can be nullptr for void)
    ArrayRef<Value*>              args,         // Arguments
    const Twine&                  instName,     // [in] Name to give instruction
    ArrayRef<Attribute::AttrKind> attribs)      // Attributes to give the function declaration
{
    // Create mangled name of builder call. This only needs to be mangled on return type.
    std::string mangledName;
    {
        raw_string_ostream mangledNameStream(mangledName);
        mangledNameStream << BuilderCallPrefix;
        mangledNameStream << GetCallName(opcode);
        if (pResultTy != nullptr)
        {
            mangledNameStream << ".";
            GetTypeName(pResultTy, mangledNameStream);
        }
        else
        {
            pResultTy = Type::getVoidTy(getContext());
        }
    }

    // See if the declaration already exists in the module.
    Module* const pModule = GetInsertBlock()->getModule();
    Function* pFunc = dyn_cast_or_null<Function>(pModule->getFunction(mangledName));
    if (pFunc == nullptr)
    {
        // Does not exist. Create it as a varargs function.
        auto pFuncTy = FunctionType::get(pResultTy, {}, true);
        pFunc = Function::Create(pFuncTy, GlobalValue::ExternalLinkage, mangledName, pModule);

        MDNode* const pFuncMeta = MDNode::get(getContext(), ConstantAsMetadata::get(getInt32(opcode)));
        pFunc->setMetadata(m_opcodeMetaKindId, pFuncMeta);
        pFunc->addFnAttr(Attribute::NoUnwind);
        for (auto attrib : attribs)
        {
            pFunc->addFnAttr(attrib);
        }
    }

    // Create the call.
    auto pCall = CreateCall(pFunc, args, instName);

    return pCall;
}

