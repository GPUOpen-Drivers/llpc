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
 * @file  llpcBuilderRecorder.cpp
 * @brief LLPC source file: BuilderRecorder implementation
 ***********************************************************************************************************************
 */
#include "lgc/llpcBuilderContext.h"
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"
#include "llpcIntrinsDefs.h"
#include "llpcPipelineState.h"
#include "llpcShaderModes.h"

#define DEBUG_TYPE "llpc-builder-recorder"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Given an opcode, get the call name (without the "llpc.call." prefix)
StringRef BuilderRecorder::getCallName(
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
    case Opcode::FMix:
        return "fmix";
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
    case Opcode::Reserved1:
        return "reserved1";
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
    llvm_unreachable("Should never be called!");
    return "";
}

// =====================================================================================================================
// BuilderRecordedMetadataKinds constructor : get the metadata kind IDs
BuilderRecorderMetadataKinds::BuilderRecorderMetadataKinds(
    llvm::LLVMContext& context)   // [in] LLVM context
{
    opcodeMetaKindId = context.getMDKindID(BuilderCallOpcodeMetadataName);
}

// =====================================================================================================================
BuilderRecorder::BuilderRecorder(
    BuilderContext* builderContext,// [in] Builder context
    Pipeline*       pipeline)      // [in] PipelineState, or nullptr for shader compile
    : Builder(builderContext),
      BuilderRecorderMetadataKinds(builderContext->getContext()),
      m_pipelineState(reinterpret_cast<PipelineState*>(pipeline))
{
    m_isBuilderRecorder = true;
}

// =====================================================================================================================
// Record shader modes into IR metadata if this is a shader compile (no PipelineState).
// For a pipeline compile with BuilderRecorder, they get recorded by PipelineState.
void BuilderRecorder::recordShaderModes(
    Module* module)    // [in/out] Module to record into
{
    if (!m_pipelineState && m_shaderModes)
        m_shaderModes->record(module);
}

// =====================================================================================================================
// Get the ShaderModes object. If this is a pipeline compile, we get the ShaderModes object from the PipelineState.
// If it is a shader compile, we create our own ShaderModes object.
ShaderModes* BuilderRecorder::getShaderModes()
{
    if (m_pipelineState)
        return m_pipelineState->getShaderModes();
    if (!m_shaderModes)
        m_shaderModes.reset(new ShaderModes());
    return &*m_shaderModes;
}

// =====================================================================================================================
// Create scalar from dot product of vector
Value* BuilderRecorder::CreateDotProduct(
    Value* const vector1,            // [in] The vector 1
    Value* const vector2,            // [in] The vector 2
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Type* const scalarType = vector1->getType()->getVectorElementType();
    return record(Opcode::DotProduct, scalarType, { vector1, vector2 }, instName);
}

// =====================================================================================================================
// In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
// the current output primitive in the specified output-primitive stream number.
Instruction* BuilderRecorder::CreateEmitVertex(
    unsigned                streamId)           // Stream number, 0 if only one stream is present
{
    return record(Opcode::EmitVertex, nullptr, getInt32(streamId), "");
}

// =====================================================================================================================
// In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
Instruction* BuilderRecorder::CreateEndPrimitive(
    unsigned                streamId)           // Stream number, 0 if only one stream is present
{
    return record(Opcode::EndPrimitive, nullptr, getInt32(streamId), "");
}

// =====================================================================================================================
// Create a workgroup control barrier.
Instruction* BuilderRecorder::CreateBarrier()
{
    return record(Opcode::Barrier, nullptr, {}, "");
}

// =====================================================================================================================
// Create a "kill". Only allowed in a fragment shader.
Instruction* BuilderRecorder::CreateKill(
    const Twine& instName)  // [in] Name to give final instruction
{
    return record(Opcode::Kill, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a matrix transpose.
Value* BuilderRecorder::CreateTransposeMatrix(
    Value* const matrix,      // [in] Matrix to transpose.
    const Twine& instName)     // [in] Name to give final instruction
{
    return record(Opcode::TransposeMatrix, getTransposedMatrixTy(matrix->getType()), { matrix }, instName);
}

// =====================================================================================================================
// Create matrix from matrix times scalar
Value* BuilderRecorder::CreateMatrixTimesScalar(
    Value* const matrix,             // [in] The matrix
    Value* const scalar,             // [in] The scalar
    const Twine& instName)            // [in] Name to give instruction(s)
{
    return record(Opcode::MatrixTimesScalar, matrix->getType(), { matrix, scalar }, instName);
}

// =====================================================================================================================
// Create vector from vector times matrix
Value* BuilderRecorder::CreateVectorTimesMatrix(
    Value* const vector,         // [in] The vector
    Value* const matrix,         // [in] The matrix
    const Twine& instName)        // [in] Name to give instruction(s)
{
    Type* const matrixType = matrix->getType();
    Type* const compType = matrixType->getArrayElementType()->getVectorElementType();
    const unsigned columnCount = matrixType->getArrayNumElements();
    Type* const resultTy = VectorType::get(compType, columnCount);
    return record(Opcode::VectorTimesMatrix, resultTy, { vector, matrix }, instName);
}

// =====================================================================================================================
// Create vector from matrix times vector
Value* BuilderRecorder::CreateMatrixTimesVector(
    Value* const matrix,             // [in] The matrix
    Value* const vector,             // [in] The vector
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Type* const columnType = matrix->getType()->getArrayElementType();
    Type* const compType = columnType->getVectorElementType();
    const unsigned rowCount = columnType->getVectorNumElements();
    Type* const vectorType = VectorType::get(compType, rowCount);
    return record(Opcode::MatrixTimesVector, vectorType, { matrix, vector }, instName);
}

// =====================================================================================================================
// Create matrix from matrix times matrix
Value* BuilderRecorder::CreateMatrixTimesMatrix(
    Value* const matrix1,             // [in] The matrix 1
    Value* const matrix2,             // [in] The matrix 2
    const Twine& instName)             // [in] Name to give instruction(s)
{
    Type* const mat1ColumnType = matrix1->getType()->getArrayElementType();
    const unsigned mat2ColCount = matrix2->getType()->getArrayNumElements();
    Type* const resultTy = ArrayType::get(mat1ColumnType, mat2ColCount);
    return record(Opcode::MatrixTimesMatrix, resultTy, { matrix1, matrix2 }, instName);
}

// =====================================================================================================================
// Create matrix from outer product of vector
Value* BuilderRecorder::CreateOuterProduct(
    Value* const vector1,            // [in] The vector 1
    Value* const vector2,            // [in] The vector 2
    const Twine& instName)            // [in] Name to give instruction(s)
{
    const unsigned colCount = vector2->getType()->getVectorNumElements();
    Type* const resultTy = ArrayType::get(vector1->getType(), colCount);
    return record(Opcode::OuterProduct, resultTy, { vector1, vector2 }, instName);
}

// =====================================================================================================================
// Create calculation of matrix determinant
Value* BuilderRecorder::CreateDeterminant(
    Value* const matrix,             // [in] Matrix
    const Twine& instName)            // [in] Name to give instruction(s)
{
    return record(Determinant,
                  matrix->getType()->getArrayElementType()->getVectorElementType(),
                  matrix,
                  instName);
}

// =====================================================================================================================
// Create calculation of matrix inverse
Value* BuilderRecorder::CreateMatrixInverse(
    Value* const matrix,             // [in] Matrix
    const Twine& instName)            // [in] Name to give instruction(s)
{
    return record(MatrixInverse, matrix->getType(), matrix, instName);
}

// =====================================================================================================================
// Create a "readclock".
Instruction* BuilderRecorder::CreateReadClock(
    bool         realtime,   // Whether to read real-time clock counter
    const Twine& instName)   // [in] Name to give final instruction
{
    return record(Opcode::ReadClock, getInt64Ty(), getInt1(realtime), instName);
}

// =====================================================================================================================
// Create tan operation
Value* BuilderRecorder::CreateTan(
        Value*        x,        // [in] Input value X
        const Twine&  instName)  // [in] Name to give final instruction)
{
    return record(Tan, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create arc sin operation
Value* BuilderRecorder::CreateASin(
        Value*        x,        // [in] Input value X
        const Twine&  instName)  // [in] Name to give final instruction)
{
    return record(ASin, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create arc cos operation
Value* BuilderRecorder::CreateACos(
        Value*        x,        // [in] Input value X
        const Twine&  instName)  // [in] Name to give final instruction)
{
    return record(ACos, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create arc tan operation
Value* BuilderRecorder::CreateATan(
        Value*        yOverX,    // [in] Input value Y/X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(ATan, yOverX->getType(), yOverX, instName, {});
}

// =====================================================================================================================
// Create arc tan operation with result in the correct quadrant for the signs of the inputs
Value* BuilderRecorder::CreateATan2(
        Value*        y,         // [in] Input value Y
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(ATan2, y->getType(), { y, x }, instName, {});
}

// =====================================================================================================================
// Create hyperbolic sin operation
Value* BuilderRecorder::CreateSinh(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(Sinh, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create hyperbolic cos operation
Value* BuilderRecorder::CreateCosh(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(Cosh, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create hyperbolic tan operation
Value* BuilderRecorder::CreateTanh(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(Tanh, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create hyperbolic arc sin operation
Value* BuilderRecorder::CreateASinh(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(ASinh, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create hyperbolic arc cos operation
Value* BuilderRecorder::CreateACosh(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(ACosh, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create hyperbolic arc tan operation
Value* BuilderRecorder::CreateATanh(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(ATanh, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create power operation
Value* BuilderRecorder::CreatePower(
        Value*        x,         // [in] Input value X
        Value*        y,         // [in] Input value Y
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(Power, x->getType(), { x, y }, instName, {});
}

// =====================================================================================================================
// Create exp operation
Value* BuilderRecorder::CreateExp(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(Exp, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create natural log operation
Value* BuilderRecorder::CreateLog(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(Log, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create inverse square root operation
Value* BuilderRecorder::CreateInverseSqrt(
        Value*        x,         // [in] Input value X
        const Twine&  instName)   // [in] Name to give final instruction
{
    return record(InverseSqrt, x->getType(), x, instName, {});
}

// =====================================================================================================================
// Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
// the given cube map texture coordinates. Returns <2 x float>.
Value* BuilderRecorder::CreateCubeFaceCoord(
    Value*        coord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::CubeFaceCoord, VectorType::get(coord->getType()->getScalarType(), 2), coord, instName);
}

// =====================================================================================================================
// Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
// the given cube map texture coordinates.
Value* BuilderRecorder::CreateCubeFaceIndex(
    Value*        coord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::CubeFaceIndex, coord->getType()->getScalarType(), coord, instName);
}

// =====================================================================================================================
// Create "signed integer abs" operation for a scalar or vector integer value.
Value* BuilderRecorder::CreateSAbs(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::SAbs, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
// value is negative, zero or positive.
Value* BuilderRecorder::CreateFSign(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FSign, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
// value is negative, zero or positive.
Value* BuilderRecorder::CreateSSign(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::SSign, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
Value* BuilderRecorder::CreateFract(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::Fract, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
// interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
// t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
// Result is undefined if edge0 >= edge1.
Value* BuilderRecorder::CreateSmoothStep(
    Value*        edge0,     // [in] Edge0 value
    Value*        edge1,     // [in] Edge1 value
    Value*        x,         // [in] X (input) value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::SmoothStep, x->getType(), { edge0, edge1, x }, instName);
}

// =====================================================================================================================
// Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
Value* BuilderRecorder::CreateLdexp(
    Value*        x,         // [in] Mantissa
    Value*        exp,       // [in] Exponent
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::Ldexp, x->getType(), { x, exp }, instName);
}

// =====================================================================================================================
// Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
// [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
// the result is undefined.
Value* BuilderRecorder::CreateExtractSignificand(
    Value*        value,   // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::ExtractSignificand, value->getType(), value, instName);
}

// =====================================================================================================================
// Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
// If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
// If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
Value* BuilderRecorder::CreateExtractExponent(
    Value*        value,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Type* resultTy = getInt32Ty();
    if (value->getType()->getScalarType()->isHalfTy())
        resultTy = getInt16Ty();
    resultTy = getConditionallyVectorizedTy(resultTy, value->getType());
    return record(Opcode::ExtractExponent, resultTy, value, instName);
}

// =====================================================================================================================
// Create vector cross product operation. Inputs must be <3 x FP>
Value* BuilderRecorder::CreateCrossProduct(
    Value*        x,         // [in] Input value X
    Value*        y,         // [in] Input value Y
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::CrossProduct, x->getType(), { x, y }, instName);
}

// =====================================================================================================================
// Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
Value* BuilderRecorder::CreateNormalizeVector(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::NormalizeVector, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
// Nref and I is negative, the result is N, otherwise it is -N
Value* BuilderRecorder::CreateFaceForward(
    Value*        n,         // [in] Input value "N"
    Value*        i,         // [in] Input value "I"
    Value*        nref,      // [in] Input value "Nref"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FaceForward, n->getType(), { n, i, nref }, instName);
}

// =====================================================================================================================
// Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
// the reflection direction:
// I - 2 * dot(N, I) * N
Value* BuilderRecorder::CreateReflect(
    Value*        i,         // [in] Input value "I"
    Value*        n,         // [in] Input value "N"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::Reflect, n->getType(), { i, n }, instName);
}

// =====================================================================================================================
// Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
// of indices of refraction eta, the result is the refraction vector:
// k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
// If k < 0.0 the result is 0.0.
// Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
Value* BuilderRecorder::CreateRefract(
    Value*        i,         // [in] Input value "I"
    Value*        n,         // [in] Input value "N"
    Value*        eta,       // [in] Input value "eta"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::Refract, n->getType(), { i, n, eta }, instName);
}

// =====================================================================================================================
// Create scalar or vector FP truncate operation with the given rounding mode.
// Currently only implemented for float/double -> half conversion.
Value* BuilderRecorder::CreateFpTruncWithRounding(
    Value*            value,             // [in] Input value
    Type*             destTy,            // [in] Type to convert to
    unsigned          roundingMode,       // Rounding mode
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::FpTruncWithRounding, destTy, { value, getInt32(roundingMode) }, instName);
}

// =====================================================================================================================
// Create quantize operation.
Value* BuilderRecorder::CreateQuantizeToFp16(
    Value*        value,     // [in] Input value (float or float vector)
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::QuantizeToFp16, value->getType(), value, instName);
}

// =====================================================================================================================
// Create signed integer modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderRecorder::CreateSMod(
    Value*        dividend,  // [in] Dividend value
    Value*        divisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::SMod, dividend->getType(), { dividend, divisor }, instName);
}

// =====================================================================================================================
// Create FP modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderRecorder::CreateFMod(
    Value*        dividend,  // [in] Dividend value
    Value*        divisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FMod, dividend->getType(), { dividend, divisor }, instName);
}

// =====================================================================================================================
// Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
Value* BuilderRecorder::CreateFma(
    Value*        a,         // [in] One value to multiply
    Value*        b,         // [in] The other value to multiply
    Value*        c,         // [in] The value to add to the product of A and B
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::Fma, a->getType(), { a, b, c }, instName);
}

// =====================================================================================================================
// Create derivative calculation on float or vector of float or half
Value* BuilderRecorder::CreateDerivative(
    Value*        value,       // [in] Input value
    bool          isDirectionY, // False for derivative in X direction, true for Y direction
    bool          isFine,       // True for "fine" calculation, false for "coarse" calculation.
    const Twine&  instName)     // [in] Name to give instruction(s)
{
    return record(Opcode::Derivative, value->getType(), { value, getInt1(isDirectionY), getInt1(isFine) }, instName);
}

// =====================================================================================================================
// Create a demote to helper invocation.
Instruction* BuilderRecorder::CreateDemoteToHelperInvocation(
    const Twine& instName)   // [in] Name to give final instruction
{
    return record(Opcode::DemoteToHelperInvocation, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a helper invocation query.
Value* BuilderRecorder::CreateIsHelperInvocation(
    const Twine& instName)   // [in] Name to give final instruction
{
    return record(Opcode::IsHelperInvocation, getInt1Ty(), {}, instName);
}

// =====================================================================================================================
// Create "fclamp" operation.
Value* BuilderRecorder::CreateFClamp(
    Value*        x,         // [in] Value to clamp
    Value*        minVal,    // [in] Minimum of clamp range
    Value*        maxVal,    // [in] Maximum of clamp range
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FClamp, x->getType(), { x, minVal, maxVal }, instName);
}

// =====================================================================================================================
// Create "fmin" operation, returning the minimum of two scalar or vector FP values.
Value* BuilderRecorder::CreateFMin(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FMin, value1->getType(), { value1, value2 }, instName);
}

// =====================================================================================================================
// Create "fmax" operation, returning the maximum of three scalar or vector FP values.
Value* BuilderRecorder::CreateFMax(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FMax, value1->getType(), { value1, value2 }, instName);
}

// =====================================================================================================================
// Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
Value* BuilderRecorder::CreateFMin3(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    Value*        value3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FMin3, value1->getType(), { value1, value2, value3 }, instName);
}

// =====================================================================================================================
// Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
Value* BuilderRecorder::CreateFMax3(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    Value*        value3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FMax3, value1->getType(), { value1, value2, value3 }, instName);
}

// =====================================================================================================================
// Create "fmid3" operation, returning the middle one of three float values.
Value* BuilderRecorder::CreateFMid3(
    Value*        value1,              // [in] First value
    Value*        value2,              // [in] Second value
    Value*        value3,              // [in] Third value
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    return record(Opcode::FMid3, value1->getType(), { value1, value2, value3 }, instName);
}

// =====================================================================================================================
// Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
Value* BuilderRecorder::CreateIsInf(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::IsInf, getConditionallyVectorizedTy(getInt1Ty(), x->getType()), x, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
Value* BuilderRecorder::CreateIsNaN(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::IsNaN, getConditionallyVectorizedTy(getInt1Ty(), x->getType()), x, instName);
}

// =====================================================================================================================
// Create an "insert bitfield" operation for a (vector of) integer type.
// Returns a value where the "pCount" bits starting at bit "pOffset" come from the least significant "pCount"
// bits in "pInsert", and remaining bits come from "pBase". The result is undefined if "pCount"+"pOffset" is
// more than the number of bits (per vector element) in "pBase" and "pInsert".
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width.
Value* BuilderRecorder::CreateInsertBitField(
    Value*        base,                // [in] Base value
    Value*        insert,              // [in] Value to insert (same type as base)
    Value*        offset,              // Bit number of least-significant end of bitfield
    Value*        count,               // Count of bits in bitfield
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    return record(Opcode::InsertBitField, base->getType(), { base, insert, offset, count }, instName);
}

// =====================================================================================================================
// Create an "extract bitfield " operation for a (vector of) i32.
// Returns a value where the least significant "pCount" bits come from the "pCount" bits starting at bit
// "pOffset" in "pBase", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width.
Value* BuilderRecorder::CreateExtractBitField(
    Value*        base,                // [in] Base value
    Value*        offset,              // Bit number of least-significant end of bitfield
    Value*        count,               // Count of bits in bitfield
    bool          isSigned,             // True for a signed int bitfield extract, false for unsigned
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    return record(Opcode::ExtractBitField, base->getType(), { base, offset, count, getInt1(isSigned) }, instName);
}

// =====================================================================================================================
// Create "find MSB" operation for a (vector of) signed int.
Value* BuilderRecorder::CreateFindSMsb(
    Value*        value,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FindSMsb, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a load of a buffer descriptor.
Value* BuilderRecorder::CreateLoadBufferDesc(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    Value*        descIndex,       // [in] Descriptor index
    bool          isNonUniform,     // Whether the descriptor index is non-uniform
    bool          isWritten,        // Whether the buffer is written to
    Type*         pointeeTy,       // [in] Type that the returned pointer should point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return record(Opcode::LoadBufferDesc,
                  getBufferDescTy(pointeeTy),
                  {
                      getInt32(descSet),
                      getInt32(binding),
                      descIndex,
                      getInt1(isNonUniform),
                      getInt1(isWritten),
                  },
                  instName);
}

// =====================================================================================================================
// Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
Value* BuilderRecorder::CreateIndexDescPtr(
    Value*        descPtr,           // [in] Descriptor pointer, as returned by this function or one of
                                      //    the CreateGet*DescPtr methods
    Value*        index,             // [in] Index value
    bool          isNonUniform,       // Whether the descriptor index is non-uniform
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    assert(descPtr->getType() == getImageDescPtrTy() ||
                descPtr->getType() == getSamplerDescPtrTy() ||
                descPtr->getType() == getFmaskDescPtrTy() ||
                descPtr->getType() == getTexelBufferDescPtrTy());
    return record(Opcode::IndexDescPtr, descPtr->getType(), { descPtr, index, getInt1(isNonUniform) }, instName);
}

// =====================================================================================================================
// Load image/sampler/texelbuffer/F-mask descriptor from pointer.
// Returns <8 x i32> descriptor for image, sampler or F-mask, or <4 x i32> descriptor for texel buffer.
Value* BuilderRecorder::CreateLoadDescFromPtr(
    Value*        descPtr,           // [in] Descriptor pointer, as returned by CreateIndexDescPtr or one of
                                      //    the CreateGet*DescPtr methods
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    assert(descPtr->getType() == getImageDescPtrTy() ||
                descPtr->getType() == getSamplerDescPtrTy() ||
                descPtr->getType() == getFmaskDescPtrTy() ||
                descPtr->getType() == getTexelBufferDescPtrTy());
    return record(Opcode::LoadDescFromPtr,
                  cast<StructType>(descPtr->getType())->getElementType(0)->getPointerElementType(),
                  descPtr,
                  instName);
}

// =====================================================================================================================
// Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
Value* BuilderRecorder::CreateGetSamplerDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return record(Opcode::GetSamplerDescPtr, getSamplerDescPtrTy(), { getInt32(descSet), getInt32(binding) }, instName);
}

// =====================================================================================================================
// Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
Value* BuilderRecorder::CreateGetImageDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return record(Opcode::GetImageDescPtr, getImageDescPtrTy(), { getInt32(descSet), getInt32(binding) }, instName);
}

// =====================================================================================================================
// Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
Value* BuilderRecorder::CreateGetTexelBufferDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return record(Opcode::GetTexelBufferDescPtr,
                  getTexelBufferDescPtrTy(),
                  { getInt32(descSet), getInt32(binding) },
                  instName);
}

// =====================================================================================================================
// Create a load of a F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
Value* BuilderRecorder::CreateGetFmaskDescPtr(
    unsigned      descSet,          // Descriptor set
    unsigned      binding,          // Descriptor binding
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    return record(Opcode::GetFmaskDescPtr, getFmaskDescPtrTy(), { getInt32(descSet), getInt32(binding) }, instName);
}

// =====================================================================================================================
// Create a load of the spill table pointer for push constants.
Value* BuilderRecorder::CreateLoadPushConstantsPtr(
    Type*         pushConstantsTy, // [in] Type of the push constants table that the returned pointer will point to
    const Twine&  instName)         // [in] Name to give instruction(s)
{
    Type* resultTy = PointerType::get(pushConstantsTy, ADDR_SPACE_CONST);
    return record(Opcode::LoadPushConstantsPtr, resultTy, {}, instName);
}

// =====================================================================================================================
// Create a buffer length query based on the specified descriptor.
Value* BuilderRecorder::CreateGetBufferDescLength(
    Value* const  bufferDesc,      // [in] The buffer descriptor to query.
    const Twine&  instName)         // [in] Name to give instruction(s).
{
    return record(Opcode::GetBufferDescLength, getInt32Ty(), { bufferDesc }, instName);
}

// =====================================================================================================================
// Create an image load.
Value* BuilderRecorder::CreateImageLoad(
    Type*                   resultTy,          // [in] Result type
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    Value*                  imageDesc,         // [in] Image descriptor
    Value*                  coord,             // [in] Coordinates: scalar or vector i32
    Value*                  mipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    SmallVector<Value*, 5> args;
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(imageDesc);
    args.push_back(coord);
    if (mipLevel )
        args.push_back(mipLevel);
    return record(Opcode::ImageLoad, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image load with F-mask.
Value* BuilderRecorder::CreateImageLoadWithFmask(
    Type*                   resultTy,          // [in] Result type
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    Value*                  imageDesc,         // [in] Image descriptor
    Value*                  fmaskDesc,         // [in] Fmask descriptor
    Value*                  coord,             // [in] Coordinates: scalar or vector i32, exactly right
                                                //    width for given dimension excluding sample
    Value*                  sampleNum,         // [in] Sample number, i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ImageLoadWithFmask,
                  resultTy,
                  { getInt32(dim), getInt32(flags), imageDesc, fmaskDesc, coord, sampleNum },
                  instName);
}

// =====================================================================================================================
// Create an image store.
Value* BuilderRecorder::CreateImageStore(
    Value*            texel,             // [in] Texel value to store; v4f16 or v4f32
    unsigned          dim,                // Image dimension
    unsigned          flags,              // ImageFlag* flags
    Value*            imageDesc,         // [in] Image descriptor
    Value*            coord,             // [in] Coordinates: scalar or vector i32
    Value*            mipLevel,          // [in] Mipmap level if doing load_mip, otherwise nullptr
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    SmallVector<Value*, 6> args;
    args.push_back(texel);
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(imageDesc);
    args.push_back(coord);
    if (mipLevel )
        args.push_back(mipLevel);
    return record(Opcode::ImageStore, nullptr, args, instName);
}

// =====================================================================================================================
// Create an image sample.
Value* BuilderRecorder::CreateImageSample(
    Type*               resultTy,          // [in] Result type
    unsigned            dim,                // Image dimension
    unsigned            flags,              // ImageFlag* flags
    Value*              imageDesc,         // [in] Image descriptor
    Value*              samplerDesc,       // [in] Sampler descriptor
    ArrayRef<Value*>    address,            // Address and other arguments
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    // Gather a mask of address elements that are not nullptr.
    unsigned addressMask = 0;
    for (unsigned i = 0; i != address.size(); ++i)
    {
        if (address[i] )
            addressMask |= 1U << i;
    }

    SmallVector<Value*, 8> args;
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(imageDesc);
    args.push_back(samplerDesc);
    args.push_back(getInt32(addressMask));
    for (unsigned i = 0; i != address.size(); ++i)
    {
        if (address[i] )
            args.push_back(address[i]);
    }
    return record(Opcode::ImageSample, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image gather.
Value* BuilderRecorder::CreateImageGather(
    Type*               resultTy,          // [in] Result type
    unsigned            dim,                // Image dimension
    unsigned            flags,              // ImageFlag* flags
    Value*              imageDesc,         // [in] Image descriptor
    Value*              samplerDesc,       // [in] Sampler descriptor
    ArrayRef<Value*>    address,            // Address and other arguments
    const Twine&        instName)           // [in] Name to give instruction(s)
{
    // Gather a mask of address elements that are not nullptr.
    unsigned addressMask = 0;
    for (unsigned i = 0; i != address.size(); ++i)
    {
        if (address[i] )
            addressMask |= 1U << i;
    }

    SmallVector<Value*, 8> args;
    args.push_back(getInt32(dim));
    args.push_back(getInt32(flags));
    args.push_back(imageDesc);
    args.push_back(samplerDesc);
    args.push_back(getInt32(addressMask));
    for (unsigned i = 0; i != address.size(); ++i)
    {
        if (address[i] )
            args.push_back(address[i]);
    }
    return record(Opcode::ImageGather, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image atomic operation other than compare-and-swap.
Value* BuilderRecorder::CreateImageAtomic(
    unsigned                atomicOp,           // Atomic op to create
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  imageDesc,         // [in] Image descriptor
    Value*                  coord,             // [in] Coordinates: scalar or vector i32
    Value*                  inputValue,        // [in] Input value: i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ImageAtomic,
                  inputValue->getType(),
                  {
                      getInt32(atomicOp),
                      getInt32(dim),
                      getInt32(flags),
                      getInt32(static_cast<unsigned>(ordering)),
                      imageDesc,
                      coord,
                      inputValue
                  },
                  instName);
}

// =====================================================================================================================
// Create an image atomic compare-and-swap.
Value* BuilderRecorder::CreateImageAtomicCompareSwap(
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    AtomicOrdering          ordering,           // Atomic ordering
    Value*                  imageDesc,         // [in] Image descriptor
    Value*                  coord,             // [in] Coordinates: scalar or vector i32
    Value*                  inputValue,        // [in] Input value: i32
    Value*                  comparatorValue,   // [in] Value to compare against: i32
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ImageAtomicCompareSwap,
                  inputValue->getType(),
                  {
                      getInt32(dim),
                      getInt32(flags),
                      getInt32(static_cast<unsigned>(ordering)),
                      imageDesc,
                      coord,
                      inputValue,
                      comparatorValue
                  },
                  instName);
}

// =====================================================================================================================
// Create a query of the number of mipmap levels in an image. Returns an i32 value.
Value* BuilderRecorder::CreateImageQueryLevels(
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    Value*                  imageDesc,         // [in] Image descriptor or texel buffer descriptor
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ImageQueryLevels, getInt32Ty(), { getInt32(dim), getInt32(flags), imageDesc }, instName);
}

// =====================================================================================================================
// Create a query of the number of samples in an image. Returns an i32 value.
Value* BuilderRecorder::CreateImageQuerySamples(
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    Value*                  imageDesc,         // [in] Image descriptor or texel buffer descriptor
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ImageQuerySamples, getInt32Ty(), { getInt32(dim), getInt32(flags), imageDesc }, instName);
}

// =====================================================================================================================
// Create a query of size of an image.
// Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
Value* BuilderRecorder::CreateImageQuerySize(
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    Value*                  imageDesc,         // [in] Image descriptor or texel buffer descriptor
    Value*                  lod,               // [in] LOD
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    unsigned compCount = getImageQuerySizeComponentCount(dim);
    Type* resultTy = getInt32Ty();
    if (compCount > 1)
        resultTy = VectorType::get(resultTy, compCount);
    return record(Opcode::ImageQuerySize, resultTy, { getInt32(dim), getInt32(flags), imageDesc, lod }, instName);
}

// =====================================================================================================================
// Create a get of the LOD that would be used for an image sample with the given coordinates
// and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
// detail relative to the base level.
Value* BuilderRecorder::CreateImageGetLod(
    unsigned                dim,                // Image dimension
    unsigned                flags,              // ImageFlag* flags
    Value*                  imageDesc,         // [in] Image descriptor
    Value*                  samplerDesc,       // [in] Sampler descriptor
    Value*                  coord,             // [in] Coordinates
    const Twine&            instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ImageGetLod,
                  VectorType::get(getFloatTy(), 2),
                  { getInt32(dim), getInt32(flags), imageDesc, samplerDesc, coord },
                  instName);
}

// =====================================================================================================================
// Create a read of (part of) a user input value, passed from the previous shader stage.
Value* BuilderRecorder::CreateReadGenericInput(
    Type*         resultTy,          // [in] Type of value to read
    unsigned      location,           // Base location (row) of input
    Value*        locationOffset,    // [in] Variable location offset; must be within locationCount
    Value*        elemIdx,           // [in] Vector index
    unsigned      locationCount,      // Count of locations taken by the input
    InOutInfo     inputInfo,          // Extra input info (FS interp info)
    Value*        vertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ReadGenericInput,
                  resultTy,
                  {
                      getInt32(location),
                      locationOffset,
                      elemIdx,
                      getInt32(locationCount),
                      getInt32(inputInfo.getData()),
                      vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                  },
                  instName,
                  Attribute::ReadOnly);
}

// =====================================================================================================================
// Create a read of (part of) a user output value, the last written value in the same shader stage.
Value* BuilderRecorder::CreateReadGenericOutput(
    Type*         resultTy,          // [in] Type of value to read
    unsigned      location,           // Base location (row) of input
    Value*        locationOffset,    // [in] Variable location offset; must be within locationCount
    Value*        elemIdx,           // [in] Vector index
    unsigned      locationCount,      // Count of locations taken by the input
    InOutInfo     outputInfo,         // Extra output info
    Value*        vertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    return record(Opcode::ReadGenericOutput,
                  resultTy,
                  {
                      getInt32(location),
                      locationOffset,
                      elemIdx,
                      getInt32(locationCount),
                      getInt32(outputInfo.getData()),
                      vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
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
    Value*        valueToWrite,      // [in] Value to write
    unsigned      location,           // Base location (row) of output
    Value*        locationOffset,    // [in] Location offset; must be within locationCount if variable
    Value*        elemIdx,           // [in] Element index in vector. (This is the SPIR-V "component", except
                                      //      that it is half the component for 64-bit elements.)
    unsigned      locationCount,      // Count of locations taken by the output. Ignored if pLocationOffset is const
    InOutInfo     outputInfo,         // Extra output info (GS stream ID, FS integer signedness)
    Value*        vertexIndex)       // [in] For TCS per-vertex output: vertex index; else nullptr
{
    return record(Opcode::WriteGenericOutput,
                  nullptr,
                  {
                      valueToWrite,
                      getInt32(location),
                      locationOffset,
                      elemIdx,
                      getInt32(locationCount),
                      getInt32(outputInfo.getData()),
                      vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                  },
                  "",
                  {});
}

// =====================================================================================================================
// Create a write to an XFB (transform feedback / streamout) buffer.
Instruction* BuilderRecorder::CreateWriteXfbOutput(
    Value*        valueToWrite,      // [in] Value to write
    bool          isBuiltIn,          // True for built-in, false for user output (ignored if not GS)
    unsigned      location,           // Location (row) or built-in kind of output (ignored if not GS)
    unsigned      xfbBuffer,          // XFB buffer ID
    unsigned      xfbStride,          // XFB stride
    Value*        xfbOffset,         // [in] XFB byte offset
    InOutInfo     outputInfo)         // Extra output info (GS stream ID)
{
    return record(Opcode::WriteXfbOutput,
                  nullptr,
                  {
                      valueToWrite,
                      getInt1(isBuiltIn),
                      getInt32(location),
                      getInt32(xfbBuffer),
                      getInt32(xfbStride),
                      xfbOffset, getInt32(outputInfo.getData())
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
    Value*        vertexIndex,       // [in] For TCS/TES/GS per-vertex input: vertex index, else nullptr
    Value*        index,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    Type* resultTy = getBuiltInTy(builtIn, inputInfo);
    if (index )
    {
        if (isa<ArrayType>(resultTy))
            resultTy = resultTy->getArrayElementType();
        else
            resultTy = resultTy->getVectorElementType();
    }
    return record(Opcode::ReadBuiltInInput,
                  resultTy,
                  {
                      getInt32(builtIn),
                      getInt32(inputInfo.getData()),
                      vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                      index ? index : UndefValue::get(getInt32Ty()),
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
    Value*        vertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    Value*        index,             // [in] Array or vector index to access part of an input, else nullptr
    const Twine&  instName)           // [in] Name to give instruction(s)
{
    Type* resultTy = getBuiltInTy(builtIn, outputInfo);
    if (index )
    {
        if (isa<ArrayType>(resultTy))
            resultTy = resultTy->getArrayElementType();
        else
            resultTy = resultTy->getVectorElementType();
    }
    return record(Opcode::ReadBuiltInOutput,
                  resultTy,
                  {
                      getInt32(builtIn),
                      getInt32(outputInfo.getData()),
                      vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                      index ? index : UndefValue::get(getInt32Ty()),
                  },
                  instName,
                  Attribute::ReadOnly);
}

// =====================================================================================================================
// Create a write of (part of) a built-in output value.
Instruction* BuilderRecorder::CreateWriteBuiltInOutput(
    Value*        valueToWrite,      // [in] Value to write
    BuiltInKind   builtIn,            // Built-in kind, one of the BuiltIn* constants
    InOutInfo     outputInfo,         // Extra output info (shader-defined array length; GS stream id)
    Value*        vertexIndex,       // [in] For TCS per-vertex output: vertex index, else nullptr
    Value*        index)             // [in] Array or vector index to access part of an input, else nullptr
{
    return record(Opcode::WriteBuiltInOutput,
                  nullptr,
                  {
                      valueToWrite,
                      getInt32(builtIn),
                      getInt32(outputInfo.getData()),
                      vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                      index ? index : UndefValue::get(getInt32Ty()),
                  },
                  "");
}

// =====================================================================================================================
// Create a get subgroup size query.
Value* BuilderRecorder::CreateGetSubgroupSize(
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::GetSubgroupSize, getInt32Ty(), {}, instName);
}

// =====================================================================================================================
// Create a subgroup elect.
Value* BuilderRecorder::CreateSubgroupElect(
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupElect, getInt1Ty(), {}, instName);
}

// =====================================================================================================================
// Create a subgroup all.
Value* BuilderRecorder::CreateSubgroupAll(
    Value* const value,   // [in] The value to compare
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupAll, getInt1Ty(), { value,  getInt1(wqm) }, instName);
}

// =====================================================================================================================
// Create a subgroup any
Value* BuilderRecorder::CreateSubgroupAny(
    Value* const value,   // [in] The value to compare
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupAny, getInt1Ty(), { value,  getInt1(wqm) }, instName);
}

// =====================================================================================================================
// Create a subgroup all equal.
Value* BuilderRecorder::CreateSubgroupAllEqual(
    Value* const value,   // [in] The value to compare
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupAllEqual, getInt1Ty(), { value,  getInt1(wqm) }, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast.
Value* BuilderRecorder::CreateSubgroupBroadcast(
    Value* const value,   // [in] The value to broadcast
    Value* const index,   // [in] The index to broadcast from
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBroadcast, value->getType(), { value, index }, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast first.
Value* BuilderRecorder::CreateSubgroupBroadcastFirst(
    Value* const value,   // [in] The value to broadcast
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBroadcastFirst, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot.
Value* BuilderRecorder::CreateSubgroupBallot(
    Value* const value,   // [in] The value to contribute
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallot, VectorType::get(getInt32Ty(), 4), value, instName);
}

// =====================================================================================================================
// Create a subgroup inverse ballot.
Value* BuilderRecorder::CreateSubgroupInverseBallot(
    Value* const value,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupInverseBallot, getInt1Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot bit extract.
Value* BuilderRecorder::CreateSubgroupBallotBitExtract(
    Value* const value,   // [in] The ballot value
    Value* const index,   // [in] The index to extract from the ballot
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallotBitExtract, getInt1Ty(), { value, index }, instName);
}

// =====================================================================================================================
// Create a subgroup ballot bit count.
Value* BuilderRecorder::CreateSubgroupBallotBitCount(
    Value* const value,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallotBitCount, getInt32Ty(), value, instName);
}

// Create a subgroup ballot inclusive bit count.
Value* BuilderRecorder::CreateSubgroupBallotInclusiveBitCount(
    Value* const value,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallotInclusiveBitCount, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot exclusive bit count.
Value* BuilderRecorder::CreateSubgroupBallotExclusiveBitCount(
    Value* const value,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallotExclusiveBitCount, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot find least significant bit.
Value* BuilderRecorder::CreateSubgroupBallotFindLsb(
    Value* const value,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallotFindLsb, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot find most significant bit.
Value* BuilderRecorder::CreateSubgroupBallotFindMsb(
    Value* const value,   // [in] The ballot value
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupBallotFindMsb, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create "fmix" operation, returning ( 1 - A ) * X + A * Y. Result would be FP scalar or vector.
Value* BuilderRecorder::createFMix(
    Value*        x,        // [in] left Value
    Value*        y,        // [in] right Value
    Value*        a,        // [in] wight Value
    const Twine& instName)   // [in] Name to give instruction(s)
{
    return record(Opcode::FMix, x->getType(), { x, y, a }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle.
Value* BuilderRecorder::CreateSubgroupShuffle(
    Value* const value,   // [in] The value to shuffle
    Value* const index,   // [in] The index to shuffle from
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupShuffle, value->getType(), { value, index }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle xor.
Value* BuilderRecorder::CreateSubgroupShuffleXor(
    Value* const value,   // [in] The value to shuffle
    Value* const mask,    // [in] The mask to shuffle with
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupShuffleXor, value->getType(), { value, mask }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle up.
Value* BuilderRecorder::CreateSubgroupShuffleUp(
    Value* const value,   // [in] The value to shuffle
    Value* const offset,  // [in] The offset to shuffle up to
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupShuffleUp, value->getType(), { value, offset }, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle down.
Value* BuilderRecorder::CreateSubgroupShuffleDown(
    Value* const value,   // [in] The value to shuffle
    Value* const offset,  // [in] The offset to shuffle down to
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupShuffleDown, value->getType(), { value, offset }, instName);
}

// =====================================================================================================================
// Create a subgroup clustered reduction.
Value* BuilderRecorder::CreateSubgroupClusteredReduction(
    GroupArithOp groupArithOp, // The group operation to perform
    Value* const value,       // [in] The value to perform on
    Value* const clusterSize, // [in] The cluster size
    const Twine& instName)     // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupClusteredReduction,
                  value->getType(),
                  {
                      getInt32(groupArithOp),
                      value,
                      clusterSize
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup clustered inclusive scan.
Value* BuilderRecorder::CreateSubgroupClusteredInclusive(
    GroupArithOp groupArithOp, // The group operation to perform
    Value* const value,       // [in] The value to perform on
    Value* const clusterSize, // [in] The cluster size
    const Twine& instName)     // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupClusteredInclusive,
                  value->getType(),
                  {
                      getInt32(groupArithOp),
                      value,
                      clusterSize
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup clustered exclusive scan.
Value* BuilderRecorder::CreateSubgroupClusteredExclusive(
    GroupArithOp groupArithOp, // The group operation to perform
    Value* const value,       // [in] The value to perform on
    Value* const clusterSize, // [in] The cluster size
    const Twine& instName)     // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupClusteredExclusive,
                  value->getType(),
                  {
                      getInt32(groupArithOp),
                      value,
                      clusterSize
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup quad broadcast.
Value* BuilderRecorder::CreateSubgroupQuadBroadcast(
    Value* const value,   // [in] The value to broadcast
    Value* const index,   // [in] The index within the quad to broadcast from
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupQuadBroadcast, value->getType(), { value, index }, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap horizontal.
Value* BuilderRecorder::CreateSubgroupQuadSwapHorizontal(
    Value* const value,   // [in] The value to swap
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupQuadSwapHorizontal, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap vertical.
Value* BuilderRecorder::CreateSubgroupQuadSwapVertical(
    Value* const value,   // [in] The value to swap
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupQuadSwapVertical, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap diagonal.
Value* BuilderRecorder::CreateSubgroupQuadSwapDiagonal(
    Value* const value,   // [in] The value to swap
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupQuadSwapDiagonal, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup swizzle quad.
Value* BuilderRecorder::CreateSubgroupSwizzleQuad(
    Value* const value,   // [in] The value to swizzle.
    Value* const offset,  // [in] The value to specify the swizzle offsets.
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupSwizzleQuad, value->getType(), { value, offset }, instName);
}

// =====================================================================================================================
// Create a subgroup swizzle mask.
Value* BuilderRecorder::CreateSubgroupSwizzleMask(
    Value* const value,   // [in] The value to swizzle.
    Value* const mask,    // [in] The value to specify the swizzle masks.
    const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupSwizzleMask, value->getType(), { value, mask }, instName);
}

// =====================================================================================================================
// Create a subgroup write invocation.
Value* BuilderRecorder::CreateSubgroupWriteInvocation(
        Value* const inputValue, // [in] The value to return for all but one invocations.
        Value* const writeValue, // [in] The value to return for one invocation.
        Value* const index,      // [in] The index of the invocation that gets the write value.
        const Twine& instName)    // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupWriteInvocation,
                  inputValue->getType(),
                  {
                        inputValue,
                        writeValue,
                        index
                  },
                  instName);
}

// =====================================================================================================================
// Create a subgroup mbcnt.
Value* BuilderRecorder::CreateSubgroupMbcnt(
        Value* const mask,    // [in] The mask to mbcnt with.
        const Twine& instName) // [in] Name to give instruction(s)
{
    return record(Opcode::SubgroupMbcnt, getInt32Ty(), mask, instName);
}

// =====================================================================================================================
// Record one Builder call
Instruction* BuilderRecorder::record(
    BuilderRecorder::Opcode       opcode,       // Opcode of Builder method call being recorded
    Type*                         resultTy,    // [in] Return type (can be nullptr for void)
    ArrayRef<Value*>              args,         // Arguments
    const Twine&                  instName,     // [in] Name to give instruction
    ArrayRef<Attribute::AttrKind> attribs)      // Attributes to give the function declaration
{
    // Create mangled name of builder call. This only needs to be mangled on return type.
    std::string mangledName;
    {
        raw_string_ostream mangledNameStream(mangledName);
        mangledNameStream << BuilderCallPrefix;
        mangledNameStream << getCallName(opcode);
        if (resultTy )
        {
            mangledNameStream << ".";
            getTypeName(resultTy, mangledNameStream);
        }
        else
            resultTy = Type::getVoidTy(getContext());
    }

    // See if the declaration already exists in the module.
    Module* const module = GetInsertBlock()->getModule();
    Function* func = dyn_cast_or_null<Function>(module->getFunction(mangledName));
    if (!func )
    {
        // Does not exist. Create it as a varargs function.
        auto funcTy = FunctionType::get(resultTy, {}, true);
        func = Function::Create(funcTy, GlobalValue::ExternalLinkage, mangledName, module);

        MDNode* const funcMeta = MDNode::get(getContext(), ConstantAsMetadata::get(getInt32(opcode)));
        func->setMetadata(opcodeMetaKindId, funcMeta);
        func->addFnAttr(Attribute::NoUnwind);
        for (auto attrib : attribs)
            func->addFnAttr(attrib);
    }

    // Create the call.
    auto call = CreateCall(func, args, instName);

    return call;
}

