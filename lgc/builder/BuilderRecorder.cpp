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
 * @file  BuilderRecorder.cpp
 * @brief LLPC source file: BuilderRecorder implementation
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderRecorder.h"
#include "lgc/LgcContext.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/ShaderModes.h"
#include "lgc/util/Internal.h"

#define DEBUG_TYPE "lgc-builder-recorder"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Given an opcode, get the call name (without the "lgc.call." prefix)
//
// @param opcode : Opcode
StringRef BuilderRecorder::getCallName(Opcode opcode) {
  switch (opcode) {
  case Opcode::Nop:
    return "nop";
  case Opcode::DotProduct:
    return "dot.product";
  case Opcode::IntegerDotProduct:
    return "integer.dot.product";
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
  case Sqrt:
    return "sqrt";
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
  case Opcode::GetDescStride:
    return "get.desc.stride";
  case Opcode::GetDescPtr:
    return "get.desc.ptr";
  case Opcode::LoadPushConstantsPtr:
    return "load.push.constants.ptr";
  case Opcode::ReadGenericInput:
    return "read.generic.input";
  case Opcode::ReadPerVertexInput:
    return "read.per.vertex.input";
  case Opcode::ReadGenericOutput:
    return "read.generic.output";
  case Opcode::WriteGenericOutput:
    return "write.generic.output";
  case Opcode::WriteXfbOutput:
    return "write.xfb.output";
  case Opcode::ReadBaryCoord:
    return "read.bary.coord";
  case Opcode::ReadBuiltInInput:
    return "read.builtin.input";
  case Opcode::ReadBuiltInOutput:
    return "read.builtin.output";
  case Opcode::WriteBuiltInOutput:
    return "write.builtin.output";
  case Opcode::ReadTaskPayload:
    return "read.task.payload";
  case Opcode::WriteTaskPayload:
    return "write.task.payload";
  case Opcode::TaskPayloadAtomic:
    return "task.payload.atomic";
  case Opcode::TaskPayloadAtomicCompareSwap:
    return "task.payload.compare.swap";
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
  case Opcode::DebugBreak:
    return "debug.break";
  case Opcode::ReadClock:
    return "read.clock";
  case Opcode::Derivative:
    return "derivative";
  case Opcode::DemoteToHelperInvocation:
    return "demote.to.helper.invocation";
  case Opcode::IsHelperInvocation:
    return "is.helper.invocation";
  case Opcode::EmitMeshTasks:
    return "emit.mesh.tasks";
  case Opcode::SetMeshOutputs:
    return "set.mesh.outputs";
  case Opcode::ImageLoad:
    return "image.load";
  case Opcode::ImageLoadWithFmask:
    return "image.load.with.fmask";
  case Opcode::ImageStore:
    return "image.store";
  case Opcode::ImageSample:
    return "image.sample";
  case Opcode::ImageSampleConvert:
    return "image.sample.convert";
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
#if VKI_RAY_TRACING
  case Opcode::ImageBvhIntersectRay:
    return "image.bvh.intersect.ray";
  case Opcode::Reserved2:
    return "reserved2";
#else
  case Opcode::Reserved2:
    return "reserved2";
  case Opcode::Reserved1:
    return "reserved1";
#endif
  case GetWaveSize:
    return "get.wave.size";
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
  case SubgroupBroadcastWaterfall:
    return "subgroup.broadcast.waterfall";
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
  case Count:
    break;
  }
  llvm_unreachable("Should never be called!");
  return "";
}

// =====================================================================================================================
// BuilderRecordedMetadataKinds constructor : get the metadata kind IDs
//
// @param context : LLVM context
BuilderRecorderMetadataKinds::BuilderRecorderMetadataKinds(LLVMContext &context) {
  opcodeMetaKindId = context.getMDKindID(BuilderCallOpcodeMetadataName);
}

// =====================================================================================================================
//
// @param builderContext : Builder context
// @param pipeline : PipelineState, or nullptr for shader compile
BuilderRecorder::BuilderRecorder(LgcContext *builderContext, Pipeline *pipeline, bool omitOpcodes)
    : Builder(builderContext), BuilderRecorderMetadataKinds(builderContext->getContext()),
      m_pipelineState(reinterpret_cast<PipelineState *>(pipeline)), m_omitOpcodes(omitOpcodes) {
}

// =====================================================================================================================
// Record shader modes into IR metadata if this is a shader compile (no PipelineState).
// For a pipeline compile with BuilderRecorder, they get recorded by PipelineState.
//
// @param [in/out] module : Module to record into
void BuilderRecorder::recordShaderModes(Module *module) {
  if (!m_pipelineState && m_shaderModes)
    m_shaderModes->record(module);
}

// =====================================================================================================================
// Get the ShaderModes object. If this is a pipeline compile, we get the ShaderModes object from the PipelineState.
// If it is a shader compile, we create our own ShaderModes object.
ShaderModes *BuilderRecorder::getShaderModes() {
  if (m_pipelineState)
    return m_pipelineState->getShaderModes();
  if (!m_shaderModes)
    m_shaderModes = std::make_unique<ShaderModes>();
  return &*m_shaderModes;
}

// =====================================================================================================================
// Create scalar from dot product of vector
//
// @param vector1 : The vector 1
// @param vector2 : The vector 2
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateDotProduct(Value *const vector1, Value *const vector2, const Twine &instName) {
  Type *const scalarType = cast<VectorType>(vector1->getType())->getElementType();
  return record(Opcode::DotProduct, scalarType, {vector1, vector2}, instName);
}

// =====================================================================================================================
// Create code to calculate the dot product of two integer vectors, with optional accumulator, using hardware support
// where available. The factor inputs are always <N x iM> of the same type, N can be arbitrary and M must be 4, 8, 16,
// 32, or 64 Use a value of 0 for no accumulation and the value type is consistent with the result type. The result is
// saturated if there is an accumulator. Only the final addition to the accumulator needs to be saturated.
// Intermediate overflows of the dot product can lead to an undefined result.
//
// @param vector1 : The integer Vector 1
// @param vector2 : The integer Vector 2
// @param accumulator : The accumulator to the scalar of dot product
// @param flags : The first bit marks whether Vector 1 is signed and the second bit marks whether Vector 2 is signed
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateIntegerDotProduct(Value *vector1, Value *vector2, Value *accumulator, unsigned flags,
                                                const Twine &instName) {
  return record(Opcode::IntegerDotProduct, accumulator->getType(), {vector1, vector2, accumulator, getInt32(flags)},
                instName);
}

// =====================================================================================================================
// In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
// the current output primitive in the specified output-primitive stream number.
//
// @param streamId : Stream number, 0 if only one stream is present
Instruction *BuilderRecorder::CreateEmitVertex(unsigned streamId) {
  return record(Opcode::EmitVertex, nullptr, getInt32(streamId), "");
}

// =====================================================================================================================
// In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
//
// @param streamId : Stream number, 0 if only one stream is present
Instruction *BuilderRecorder::CreateEndPrimitive(unsigned streamId) {
  return record(Opcode::EndPrimitive, nullptr, getInt32(streamId), "");
}

// =====================================================================================================================
// Create a workgroup control barrier.
Instruction *BuilderRecorder::CreateBarrier() {
  return record(Opcode::Barrier, nullptr, {}, "");
}

// =====================================================================================================================
// Create a "kill". Only allowed in a fragment shader.
//
// @param instName : Name to give final instruction
Instruction *BuilderRecorder::CreateKill(const Twine &instName) {
  return record(Opcode::Kill, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a matrix transpose.
//
// @param matrix : Matrix to transpose.
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateTransposeMatrix(Value *const matrix, const Twine &instName) {
  return record(Opcode::TransposeMatrix, getTransposedMatrixTy(matrix->getType()), {matrix}, instName);
}

// =====================================================================================================================
// Create matrix from matrix times scalar
//
// @param matrix : The matrix
// @param scalar : The scalar
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateMatrixTimesScalar(Value *const matrix, Value *const scalar, const Twine &instName) {
  return record(Opcode::MatrixTimesScalar, matrix->getType(), {matrix, scalar}, instName);
}

// =====================================================================================================================
// Create vector from vector times matrix
//
// @param vector : The vector
// @param matrix : The matrix
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateVectorTimesMatrix(Value *const vector, Value *const matrix, const Twine &instName) {
  Type *const matrixType = matrix->getType();
  Type *const compType = cast<VectorType>(cast<ArrayType>(matrixType)->getElementType())->getElementType();
  const unsigned columnCount = matrixType->getArrayNumElements();
  Type *const resultTy = FixedVectorType::get(compType, columnCount);
  return record(Opcode::VectorTimesMatrix, resultTy, {vector, matrix}, instName);
}

// =====================================================================================================================
// Create vector from matrix times vector
//
// @param matrix : The matrix
// @param vector : The vector
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateMatrixTimesVector(Value *const matrix, Value *const vector, const Twine &instName) {
  Type *const columnType = matrix->getType()->getArrayElementType();
  Type *const compType = cast<VectorType>(columnType)->getElementType();
  const unsigned rowCount = cast<FixedVectorType>(columnType)->getNumElements();
  Type *const vectorType = FixedVectorType::get(compType, rowCount);
  return record(Opcode::MatrixTimesVector, vectorType, {matrix, vector}, instName);
}

// =====================================================================================================================
// Create matrix from matrix times matrix
//
// @param matrix1 : The matrix 1
// @param matrix2 : The matrix 2
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateMatrixTimesMatrix(Value *const matrix1, Value *const matrix2, const Twine &instName) {
  Type *const mat1ColumnType = matrix1->getType()->getArrayElementType();
  const unsigned mat2ColCount = matrix2->getType()->getArrayNumElements();
  Type *const resultTy = ArrayType::get(mat1ColumnType, mat2ColCount);
  return record(Opcode::MatrixTimesMatrix, resultTy, {matrix1, matrix2}, instName);
}

// =====================================================================================================================
// Create matrix from outer product of vector
//
// @param vector1 : The vector 1
// @param vector2 : The vector 2
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateOuterProduct(Value *const vector1, Value *const vector2, const Twine &instName) {
  const unsigned colCount = cast<FixedVectorType>(vector2->getType())->getNumElements();
  Type *const resultTy = ArrayType::get(vector1->getType(), colCount);
  return record(Opcode::OuterProduct, resultTy, {vector1, vector2}, instName);
}

// =====================================================================================================================
// Create calculation of matrix determinant
//
// @param matrix : Matrix
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateDeterminant(Value *const matrix, const Twine &instName) {
  return record(Determinant, cast<VectorType>(cast<ArrayType>(matrix->getType())->getElementType())->getElementType(),
                matrix, instName);
}

// =====================================================================================================================
// Create calculation of matrix inverse
//
// @param matrix : Matrix
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateMatrixInverse(Value *const matrix, const Twine &instName) {
  return record(MatrixInverse, matrix->getType(), matrix, instName);
}

// =====================================================================================================================
// Create a "readclock".
//
// @param realtime : Whether to read real-time clock counter
// @param instName : Name to give final instruction
Instruction *BuilderRecorder::CreateReadClock(bool realtime, const Twine &instName) {
  return record(Opcode::ReadClock, getInt64Ty(), getInt1(realtime), instName);
}

// =====================================================================================================================
// Create a "debug break halt"
//
// @param instName : Name to give final instruction
Instruction *BuilderRecorder::CreateDebugBreak(const Twine &instName) {
  return record(Opcode::DebugBreak, getVoidTy(), {}, instName);
}

// =====================================================================================================================
// Create tan operation
//
// @param x : Input value X
// @param instName : Name to give final instruction)
Value *BuilderRecorder::CreateTan(Value *x, const Twine &instName) {
  return record(Tan, x->getType(), x, instName);
}

// =====================================================================================================================
// Create arc sin operation
//
// @param x : Input value X
// @param instName : Name to give final instruction)
Value *BuilderRecorder::CreateASin(Value *x, const Twine &instName) {
  return record(ASin, x->getType(), x, instName);
}

// =====================================================================================================================
// Create arc cos operation
//
// @param x : Input value X
// @param instName : Name to give final instruction)
Value *BuilderRecorder::CreateACos(Value *x, const Twine &instName) {
  return record(ACos, x->getType(), x, instName);
}

// =====================================================================================================================
// Create arc tan operation
//
// @param yOverX : Input value Y/X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateATan(Value *yOverX, const Twine &instName) {
  return record(ATan, yOverX->getType(), yOverX, instName);
}

// =====================================================================================================================
// Create arc tan operation with result in the correct quadrant for the signs of the inputs
//
// @param y : Input value Y
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateATan2(Value *y, Value *x, const Twine &instName) {
  return record(ATan2, y->getType(), {y, x}, instName);
}

// =====================================================================================================================
// Create hyperbolic sin operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateSinh(Value *x, const Twine &instName) {
  return record(Sinh, x->getType(), x, instName);
}

// =====================================================================================================================
// Create hyperbolic cos operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateCosh(Value *x, const Twine &instName) {
  return record(Cosh, x->getType(), x, instName);
}

// =====================================================================================================================
// Create hyperbolic tan operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateTanh(Value *x, const Twine &instName) {
  return record(Tanh, x->getType(), x, instName);
}

// =====================================================================================================================
// Create hyperbolic arc sin operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateASinh(Value *x, const Twine &instName) {
  return record(ASinh, x->getType(), x, instName);
}

// =====================================================================================================================
// Create hyperbolic arc cos operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateACosh(Value *x, const Twine &instName) {
  return record(ACosh, x->getType(), x, instName);
}

// =====================================================================================================================
// Create hyperbolic arc tan operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateATanh(Value *x, const Twine &instName) {
  return record(ATanh, x->getType(), x, instName);
}

// =====================================================================================================================
// Create power operation
//
// @param x : Input value X
// @param y : Input value Y
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreatePower(Value *x, Value *y, const Twine &instName) {
  return record(Power, x->getType(), {x, y}, instName);
}

// =====================================================================================================================
// Create exp operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateExp(Value *x, const Twine &instName) {
  return record(Exp, x->getType(), x, instName);
}

// =====================================================================================================================
// Create natural log operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateLog(Value *x, const Twine &instName) {
  return record(Log, x->getType(), x, instName);
}

// =====================================================================================================================
// Create square root operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateSqrt(Value *x, const Twine &instName) {
  return record(Sqrt, x->getType(), x, instName);
}

// =====================================================================================================================
// Create inverse square root operation
//
// @param x : Input value X
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateInverseSqrt(Value *x, const Twine &instName) {
  return record(InverseSqrt, x->getType(), x, instName);
}

// =====================================================================================================================
// Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
// the given cube map texture coordinates. Returns <2 x float>.
//
// @param coord : Input coordinate <3 x float>
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateCubeFaceCoord(Value *coord, const Twine &instName) {
  return record(Opcode::CubeFaceCoord, FixedVectorType::get(coord->getType()->getScalarType(), 2), coord, instName);
}

// =====================================================================================================================
// Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
// the given cube map texture coordinates.
//
// @param coord : Input coordinate <3 x float>
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateCubeFaceIndex(Value *coord, const Twine &instName) {
  return record(Opcode::CubeFaceIndex, coord->getType()->getScalarType(), coord, instName);
}

// =====================================================================================================================
// Create "signed integer abs" operation for a scalar or vector integer value.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSAbs(Value *x, const Twine &instName) {
  return record(Opcode::SAbs, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
// value is negative, zero or positive.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFSign(Value *x, const Twine &instName) {
  return record(Opcode::FSign, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
// value is negative, zero or positive.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSSign(Value *x, const Twine &instName) {
  return record(Opcode::SSign, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFract(Value *x, const Twine &instName) {
  return record(Opcode::Fract, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
// interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
// t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
// Result is undefined if edge0 >= edge1.
//
// @param edge0 : Edge0 value
// @param edge1 : Edge1 value
// @param x : X (input) value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSmoothStep(Value *edge0, Value *edge1, Value *x, const Twine &instName) {
  return record(Opcode::SmoothStep, x->getType(), {edge0, edge1, x}, instName);
}

// =====================================================================================================================
// Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
//
// @param x : Mantissa
// @param exp : Exponent
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateLdexp(Value *x, Value *exp, const Twine &instName) {
  return record(Opcode::Ldexp, x->getType(), {x, exp}, instName);
}

// =====================================================================================================================
// Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
// [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
// the result is undefined.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateExtractSignificand(Value *value, const Twine &instName) {
  return record(Opcode::ExtractSignificand, value->getType(), value, instName);
}

// =====================================================================================================================
// Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
// If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
// If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateExtractExponent(Value *value, const Twine &instName) {
  Type *resultTy = getInt32Ty();
  if (value->getType()->getScalarType()->isHalfTy())
    resultTy = getInt16Ty();
  resultTy = getConditionallyVectorizedTy(resultTy, value->getType());
  return record(Opcode::ExtractExponent, resultTy, value, instName);
}

// =====================================================================================================================
// Create vector cross product operation. Inputs must be <3 x FP>
//
// @param x : Input value X
// @param y : Input value Y
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateCrossProduct(Value *x, Value *y, const Twine &instName) {
  return record(Opcode::CrossProduct, x->getType(), {x, y}, instName);
}

// =====================================================================================================================
// Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
//
// @param x : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateNormalizeVector(Value *x, const Twine &instName) {
  return record(Opcode::NormalizeVector, x->getType(), x, instName);
}

// =====================================================================================================================
// Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
// Nref and I is negative, the result is N, otherwise it is -N
//
// @param n : Input value "N"
// @param i : Input value "I"
// @param nref : Input value "Nref"
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFaceForward(Value *n, Value *i, Value *nref, const Twine &instName) {
  return record(Opcode::FaceForward, n->getType(), {n, i, nref}, instName);
}

// =====================================================================================================================
// Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
// the reflection direction:
// I - 2 * dot(N, I) * N
//
// @param i : Input value "I"
// @param n : Input value "N"
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateReflect(Value *i, Value *n, const Twine &instName) {
  return record(Opcode::Reflect, n->getType(), {i, n}, instName);
}

// =====================================================================================================================
// Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
// of indices of refraction eta, the result is the refraction vector:
// k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
// If k < 0.0 the result is 0.0.
// Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
//
// @param i : Input value "I"
// @param n : Input value "N"
// @param eta : Input value "eta"
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateRefract(Value *i, Value *n, Value *eta, const Twine &instName) {
  return record(Opcode::Refract, n->getType(), {i, n, eta}, instName);
}

// =====================================================================================================================
// Create scalar or vector FP truncate operation with the given rounding mode.
// Currently only implemented for float/double -> half conversion.
//
// @param value : Input value
// @param destTy : Type to convert to
// @param roundingMode : Rounding mode
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFpTruncWithRounding(Value *value, Type *destTy, RoundingMode roundingMode,
                                                  const Twine &instName) {
  return record(Opcode::FpTruncWithRounding, destTy, {value, getInt32(static_cast<unsigned>(roundingMode))}, instName);
}

// =====================================================================================================================
// Create quantize operation.
//
// @param value : Input value (float or float vector)
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateQuantizeToFp16(Value *value, const Twine &instName) {
  return record(Opcode::QuantizeToFp16, value->getType(), value, instName);
}

// =====================================================================================================================
// Create signed integer modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
//
// @param dividend : Dividend value
// @param divisor : Divisor value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSMod(Value *dividend, Value *divisor, const Twine &instName) {
  return record(Opcode::SMod, dividend->getType(), {dividend, divisor}, instName);
}

// =====================================================================================================================
// Create FP modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
//
// @param dividend : Dividend value
// @param divisor : Divisor value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFMod(Value *dividend, Value *divisor, const Twine &instName) {
  return record(Opcode::FMod, dividend->getType(), {dividend, divisor}, instName);
}

// =====================================================================================================================
// Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
//
// @param a : One value to multiply
// @param b : The other value to multiply
// @param c : The value to add to the product of A and B
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFma(Value *a, Value *b, Value *c, const Twine &instName) {
  return record(Opcode::Fma, a->getType(), {a, b, c}, instName);
}

// =====================================================================================================================
// Create derivative calculation on float or vector of float or half
//
// @param value : Input value
// @param isDirectionY : False for derivative in X direction, true for Y direction
// @param isFine : True for "fine" calculation, false for "coarse" calculation.
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateDerivative(Value *value, bool isDirectionY, bool isFine, const Twine &instName) {
  return record(Opcode::Derivative, value->getType(), {value, getInt1(isDirectionY), getInt1(isFine)}, instName);
}

// =====================================================================================================================
// Create a demote to helper invocation.
//
// @param instName : Name to give final instruction
Instruction *BuilderRecorder::CreateDemoteToHelperInvocation(const Twine &instName) {
  return record(Opcode::DemoteToHelperInvocation, nullptr, {}, instName);
}

// =====================================================================================================================
// Create a helper invocation query.
//
// @param instName : Name to give final instruction
Value *BuilderRecorder::CreateIsHelperInvocation(const Twine &instName) {
  return record(Opcode::IsHelperInvocation, getInt1Ty(), {}, instName);
}

// =====================================================================================================================
// In the task shader, emit the current values of all per-task output variables to the current task output by
// specifying the group count XYZ of the launched child mesh tasks.
//
// @param groupCountX : X dimension of the launched child mesh tasks
// @param groupCountY : Y dimension of the launched child mesh tasks
// @param groupCountZ : Z dimension of the launched child mesh tasks
// @param instName : Name to give final instruction
// @returns Instruction to emit mesh tasks
Instruction *BuilderRecorder::CreateEmitMeshTasks(Value *groupCountX, Value *groupCountY, Value *groupCountZ,
                                                  const Twine &instName) {
  return record(Opcode::EmitMeshTasks, nullptr, {groupCountX, groupCountY, groupCountZ}, instName);
}

// =====================================================================================================================
// In the mesh shader, set the actual output size of the primitives and vertices that the mesh shader workgroup will
// emit upon completion.
//
// @param vertexCount : Actual output size of the vertices
// @param primitiveCount : Actual output size of the primitives
// @param instName : Name to give final instruction
// @returns Instruction to set the actual size of mesh outputs
Instruction *BuilderRecorder::CreateSetMeshOutputs(Value *vertexCount, Value *primitiveCount, const Twine &instName) {
  return record(Opcode::SetMeshOutputs, nullptr, {vertexCount, primitiveCount}, instName);
}

// =====================================================================================================================
// Create "fclamp" operation.
//
// @param x : Value to clamp
// @param minVal : Minimum of clamp range
// @param maxVal : Maximum of clamp range
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFClamp(Value *x, Value *minVal, Value *maxVal, const Twine &instName) {
  return record(Opcode::FClamp, x->getType(), {x, minVal, maxVal}, instName);
}

// =====================================================================================================================
// Create "fmin" operation, returning the minimum of two scalar or vector FP values.
//
// @param value1 : First value
// @param value2 : Second value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFMin(Value *value1, Value *value2, const Twine &instName) {
  return record(Opcode::FMin, value1->getType(), {value1, value2}, instName);
}

// =====================================================================================================================
// Create "fmax" operation, returning the maximum of three scalar or vector FP values.
//
// @param value1 : First value
// @param value2 : Second value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFMax(Value *value1, Value *value2, const Twine &instName) {
  return record(Opcode::FMax, value1->getType(), {value1, value2}, instName);
}

// =====================================================================================================================
// Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
//
// @param value1 : First value
// @param value2 : Second value
// @param value3 : Third value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFMin3(Value *value1, Value *value2, Value *value3, const Twine &instName) {
  return record(Opcode::FMin3, value1->getType(), {value1, value2, value3}, instName);
}

// =====================================================================================================================
// Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
//
// @param value1 : First value
// @param value2 : Second value
// @param value3 : Third value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFMax3(Value *value1, Value *value2, Value *value3, const Twine &instName) {
  return record(Opcode::FMax3, value1->getType(), {value1, value2, value3}, instName);
}

// =====================================================================================================================
// Create "fmid3" operation, returning the middle one of three float values.
//
// @param value1 : First value
// @param value2 : Second value
// @param value3 : Third value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFMid3(Value *value1, Value *value2, Value *value3, const Twine &instName) {
  return record(Opcode::FMid3, value1->getType(), {value1, value2, value3}, instName);
}

// =====================================================================================================================
// Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateIsInf(Value *x, const Twine &instName) {
  return record(Opcode::IsInf, getConditionallyVectorizedTy(getInt1Ty(), x->getType()), x, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
//
// @param x : Input value X
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateIsNaN(Value *x, const Twine &instName) {
  return record(Opcode::IsNaN, getConditionallyVectorizedTy(getInt1Ty(), x->getType()), x, instName);
}

// =====================================================================================================================
// Create an "insert bitfield" operation for a (vector of) integer type.
// Returns a value where the "count" bits starting at bit "offset" come from the least significant "count"
// bits in "insert", and remaining bits come from "base". The result is undefined if "count"+"offset" is
// more than the number of bits (per vector element) in "base" and "insert".
// If "base" and "insert" are vectors, "offset" and "count" can be either scalar or vector of the same
// width.
//
// @param base : Base value
// @param insert : Value to insert (same type as base)
// @param offset : Bit number of least-significant end of bitfield
// @param count : Count of bits in bitfield
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateInsertBitField(Value *base, Value *insert, Value *offset, Value *count,
                                             const Twine &instName) {
  return record(Opcode::InsertBitField, base->getType(), {base, insert, offset, count}, instName);
}

// =====================================================================================================================
// Create an "extract bitfield " operation for a (vector of) i32.
// Returns a value where the least significant "count" bits come from the "count" bits starting at bit
// "offset" in "base", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
// If "base" and "insert" are vectors, "offset" and "count" can be either scalar or vector of the same
// width.
//
// @param base : Base value
// @param offset : Bit number of least-significant end of bitfield
// @param count : Count of bits in bitfield
// @param isSigned : True for a signed int bitfield extract, false for unsigned
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateExtractBitField(Value *base, Value *offset, Value *count, bool isSigned,
                                              const Twine &instName) {
  return record(Opcode::ExtractBitField, base->getType(), {base, offset, count, getInt1(isSigned)}, instName);
}

// =====================================================================================================================
// Create "find MSB" operation for a (vector of) signed int.
//
// @param value : Input value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateFindSMsb(Value *value, const Twine &instName) {
  return record(Opcode::FindSMsb, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a load of a buffer descriptor.
//
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param descIndex : Descriptor index
// @param flags : BufferFlag* bit settings
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateLoadBufferDesc(unsigned descSet, unsigned binding, Value *descIndex, unsigned flags,
                                             const Twine &instName) {
  return record(Opcode::LoadBufferDesc, getBufferDescTy(),
                {
                    getInt32(descSet),
                    getInt32(binding),
                    descIndex,
                    getInt32(flags),
                },
                instName);
}

// =====================================================================================================================
// Create a get of the stride (in bytes) of a descriptor. Returns an i32 value.
//
// @param concreteType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
//                   DescriptorTexelBuffer, DescriptorFmask.
// @param abstractType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
//                   DescriptorTexelBuffer, DescriptorFmask.
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateGetDescStride(ResourceNodeType concreteType, ResourceNodeType abstractType,
                                            unsigned descSet, unsigned binding, const Twine &instName) {
  return record(Opcode::GetDescStride, getInt32Ty(),
                {getInt32(static_cast<unsigned>(concreteType)), getInt32(static_cast<unsigned>(abstractType)),
                 getInt32(descSet), getInt32(binding)},
                instName);
}

// =====================================================================================================================
// Create a pointer to a descriptor. Returns a value of the type returned by GetSamplerDescPtrTy, GetImageDescPtrTy,
// GetTexelBufferDescPtrTy or GetFmaskDescPtrTy, depending on descType.
//
// @param concreteType : Descriptor type, one of ResourceNodeType::DescriptorSampler, DescriptorResource,
//                   DescriptorTexelBuffer, DescriptorFmask.
// @param abstractType : Descriptor type to find user resource nodes;
// @param descSet : Descriptor set
// @param binding : Descriptor binding
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateGetDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, unsigned descSet,
                                         unsigned binding, const Twine &instName) {
  return record(Opcode::GetDescPtr, getDescPtrTy(concreteType),
                {getInt32(static_cast<unsigned>(concreteType)), getInt32(static_cast<unsigned>(abstractType)),
                 getInt32(descSet), getInt32(binding)},
                instName);
}

// =====================================================================================================================
// Create a load of the spill table pointer for push constants.
//
// @param returnTy : Return type of the load
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateLoadPushConstantsPtr(Type *returnTy, const Twine &instName) {
  return record(Opcode::LoadPushConstantsPtr, returnTy, {}, instName);
}

// =====================================================================================================================
// Create an image load.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param mipLevel : Mipmap level if doing load_mip, otherwise nullptr
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageLoad(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc, Value *coord,
                                        Value *mipLevel, const Twine &instName) {
  SmallVector<Value *, 5> args;
  args.push_back(getInt32(dim));
  args.push_back(getInt32(flags));
  args.push_back(imageDesc);
  args.push_back(coord);
  if (mipLevel)
    args.push_back(mipLevel);
  return record(Opcode::ImageLoad, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image load with F-mask.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param fmaskDesc : Fmask descriptor
// @param coord : Coordinates: scalar or vector i32, exactly right width for given dimension excluding sample
// @param sampleNum : Sample number, i32
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageLoadWithFmask(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                                 Value *fmaskDesc, Value *coord, Value *sampleNum,
                                                 const Twine &instName) {
  return record(Opcode::ImageLoadWithFmask, resultTy,
                {getInt32(dim), getInt32(flags), imageDesc, fmaskDesc, coord, sampleNum}, instName);
}

// =====================================================================================================================
// Create an image store.
//
// @param texel : Texel value to store; v4f16 or v4f32
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param mipLevel : Mipmap level if doing load_mip, otherwise nullptr
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageStore(Value *texel, unsigned dim, unsigned flags, Value *imageDesc, Value *coord,
                                         Value *mipLevel, const Twine &instName) {
  SmallVector<Value *, 6> args;
  args.push_back(texel);
  args.push_back(getInt32(dim));
  args.push_back(getInt32(flags));
  args.push_back(imageDesc);
  args.push_back(coord);
  if (mipLevel)
    args.push_back(mipLevel);
  return record(Opcode::ImageStore, nullptr, args, instName);
}

// =====================================================================================================================
// Create an image sample.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageSample(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                          Value *samplerDesc, ArrayRef<Value *> address, const Twine &instName) {
  // Gather a mask of address elements that are not nullptr.
  unsigned addressMask = 0;
  for (unsigned i = 0; i != address.size(); ++i) {
    if (address[i])
      addressMask |= 1U << i;
  }

  SmallVector<Value *, 8> args;
  args.push_back(getInt32(dim));
  args.push_back(getInt32(flags));
  args.push_back(imageDesc);
  args.push_back(samplerDesc);
  args.push_back(getInt32(addressMask));
  for (unsigned i = 0; i != address.size(); ++i) {
    if (address[i])
      args.push_back(address[i]);
  }
  return record(Opcode::ImageSample, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image sample with a converting sampler.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDescArray : Image descriptor, or array of up to three descriptors for multi-plane
// @param convertingSamplerDesc : Converting sampler descriptor (constant v10i32)
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageSampleConvert(Type *resultTy, unsigned dim, unsigned flags, Value *imageDescArray,
                                                 Value *convertingSamplerDesc, ArrayRef<Value *> address,
                                                 const Twine &instName) {
  // Gather a mask of address elements that are not nullptr.
  unsigned addressMask = 0;
  for (unsigned i = 0; i != address.size(); ++i) {
    if (address[i])
      addressMask |= 1U << i;
  }

  SmallVector<Value *, 8> args;
  args.push_back(getInt32(dim));
  args.push_back(getInt32(flags));
  args.push_back(imageDescArray);
  args.push_back(convertingSamplerDesc);
  args.push_back(getInt32(addressMask));
  for (unsigned i = 0; i != address.size(); ++i) {
    if (address[i])
      args.push_back(address[i]);
  }
  return record(Opcode::ImageSampleConvert, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image gather.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageGather(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                          Value *samplerDesc, ArrayRef<Value *> address, const Twine &instName) {
  // Gather a mask of address elements that are not nullptr.
  unsigned addressMask = 0;
  for (unsigned i = 0; i != address.size(); ++i) {
    if (address[i])
      addressMask |= 1U << i;
  }

  SmallVector<Value *, 8> args;
  args.push_back(getInt32(dim));
  args.push_back(getInt32(flags));
  args.push_back(imageDesc);
  args.push_back(samplerDesc);
  args.push_back(getInt32(addressMask));
  for (unsigned i = 0; i != address.size(); ++i) {
    if (address[i])
      args.push_back(address[i]);
  }
  return record(Opcode::ImageGather, resultTy, args, instName);
}

// =====================================================================================================================
// Create an image atomic operation other than compare-and-swap.
//
// @param atomicOp : Atomic op to create
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param ordering : Atomic ordering
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param inputValue : Input value: i32
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageAtomic(unsigned atomicOp, unsigned dim, unsigned flags, AtomicOrdering ordering,
                                          Value *imageDesc, Value *coord, Value *inputValue, const Twine &instName) {
  return record(Opcode::ImageAtomic, inputValue->getType(),
                {getInt32(atomicOp), getInt32(dim), getInt32(flags), getInt32(static_cast<unsigned>(ordering)),
                 imageDesc, coord, inputValue},
                instName);
}

// =====================================================================================================================
// Create an image atomic compare-and-swap.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param ordering : Atomic ordering
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param inputValue : Input value: i32
// @param comparatorValue : Value to compare against: i32
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageAtomicCompareSwap(unsigned dim, unsigned flags, AtomicOrdering ordering,
                                                     Value *imageDesc, Value *coord, Value *inputValue,
                                                     Value *comparatorValue, const Twine &instName) {
  return record(Opcode::ImageAtomicCompareSwap, inputValue->getType(),
                {getInt32(dim), getInt32(flags), getInt32(static_cast<unsigned>(ordering)), imageDesc, coord,
                 inputValue, comparatorValue},
                instName);
}

// =====================================================================================================================
// Create a query of the number of mipmap levels in an image. Returns an i32 value.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageQueryLevels(unsigned dim, unsigned flags, Value *imageDesc, const Twine &instName) {
  return record(Opcode::ImageQueryLevels, getInt32Ty(), {getInt32(dim), getInt32(flags), imageDesc}, instName);
}

// =====================================================================================================================
// Create a query of the number of samples in an image. Returns an i32 value.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageQuerySamples(unsigned dim, unsigned flags, Value *imageDesc, const Twine &instName) {
  return record(Opcode::ImageQuerySamples, getInt32Ty(), {getInt32(dim), getInt32(flags), imageDesc}, instName);
}

// =====================================================================================================================
// Create a query of size of an image.
// Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param lod : LOD
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageQuerySize(unsigned dim, unsigned flags, Value *imageDesc, Value *lod,
                                             const Twine &instName) {
  unsigned compCount = getImageQuerySizeComponentCount(dim);
  Type *resultTy = getInt32Ty();
  if (compCount > 1)
    resultTy = FixedVectorType::get(resultTy, compCount);
  return record(Opcode::ImageQuerySize, resultTy, {getInt32(dim), getInt32(flags), imageDesc, lod}, instName);
}

// =====================================================================================================================
// Create a get of the LOD that would be used for an image sample with the given coordinates
// and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
// detail relative to the base level.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param coord : Coordinates
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageGetLod(unsigned dim, unsigned flags, Value *imageDesc, Value *samplerDesc,
                                          Value *coord, const Twine &instName) {
  return record(Opcode::ImageGetLod, FixedVectorType::get(getFloatTy(), 2),
                {getInt32(dim), getInt32(flags), imageDesc, samplerDesc, coord}, instName);
}

// =====================================================================================================================
// Create a read of (part of) a user input value, passed from the previous shader stage.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Vector index
// @param locationCount : Count of locations taken by the input
// @param inputInfo : Extra input info (FS interp info)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateReadGenericInput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                               unsigned locationCount, InOutInfo inputInfo, Value *vertexIndex,
                                               const Twine &instName) {
  return record(Opcode::ReadGenericInput, resultTy,
                {
                    getInt32(location),
                    locationOffset,
                    elemIdx,
                    getInt32(locationCount),
                    getInt32(inputInfo.getData()),
                    vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                },
                instName);
}

// =====================================================================================================================
// Create a read of (part of) a perVertex input value, passed from the previous shader stage.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inputInfo : Extra input info (FS interp info)
// @param vertexIndex : Vertex index, for each vertex, it is unique. the range is 0-2, up to three vertices per
// primitive; for FS custom interpolated input: auxiliary interpolation value;
// @param instName : Name to give instruction(s)
// @returns Value of input
Value *BuilderRecorder::CreateReadPerVertexInput(Type *resultTy, unsigned location, Value *locationOffset,
                                                 Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                                 Value *vertexIndex, const Twine &instName) {
  return record(Opcode::ReadPerVertexInput, resultTy,
                {
                    getInt32(location),
                    locationOffset,
                    elemIdx,
                    getInt32(locationCount),
                    getInt32(inputInfo.getData()),
                    vertexIndex,
                },
                instName);
}

// =====================================================================================================================
// Create a read of (part of) a user output value, the last written value in the same shader stage.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Vector index
// @param locationCount : Count of locations taken by the input
// @param outputInfo : Extra output info
// @param vertexIndex : For TCS per-vertex output: vertex index, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateReadGenericOutput(Type *resultTy, unsigned location, Value *locationOffset,
                                                Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                                Value *vertexIndex, const Twine &instName) {
  return record(Opcode::ReadGenericOutput, resultTy,
                {
                    getInt32(location),
                    locationOffset,
                    elemIdx,
                    getInt32(locationCount),
                    getInt32(outputInfo.getData()),
                    vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                },
                instName);
}

// =====================================================================================================================
// Create a write of (part of) a user output value, setting the value to pass to the next shader stage.
// The value to write must be a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// A non-constant locationOffset is currently only supported for TCS.
//
// @param valueToWrite : Value to write
// @param location : Base location (row) of output
// @param locationOffset : Location offset; must be within locationCount if variable
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the output. Ignored if locationOffset is const
// @param outputInfo : Extra output info (GS stream ID, FS integer signedness)
// @param vertexOrPrimitiveIndex : For TCS/mesh shader per-vertex output: vertex index; for mesh shader per-primitive
//                                 output: primitive index; else nullptr
Instruction *BuilderRecorder::CreateWriteGenericOutput(Value *valueToWrite, unsigned location, Value *locationOffset,
                                                       Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                                       Value *vertexOrPrimitiveIndex) {
  return record(Opcode::WriteGenericOutput, nullptr,
                {
                    valueToWrite,
                    getInt32(location),
                    locationOffset,
                    elemIdx,
                    getInt32(locationCount),
                    getInt32(outputInfo.getData()),
                    vertexOrPrimitiveIndex ? vertexOrPrimitiveIndex : UndefValue::get(getInt32Ty()),
                },
                "");
}

// =====================================================================================================================
// Create a write to an XFB (transform feedback / streamout) buffer.
//
// @param valueToWrite : Value to write
// @param isBuiltIn : True for built-in, false for user output
// @param location : Location (row) or built-in kind of output
// @param component : Component offset of inputs and outputs (ignored if built-in)
// @param xfbBuffer : XFB buffer ID
// @param xfbStride : XFB stride
// @param xfbOffset : XFB byte offset
// @param outputInfo : Extra output info (GS stream ID)
Instruction *BuilderRecorder::CreateWriteXfbOutput(Value *valueToWrite, bool isBuiltIn, unsigned location,
                                                   unsigned component, unsigned xfbBuffer, unsigned xfbStride,
                                                   Value *xfbOffset, InOutInfo outputInfo) {
  return record(Opcode::WriteXfbOutput, nullptr,
                {valueToWrite, getInt1(isBuiltIn), getInt32(location), getInt32(component), getInt32(xfbBuffer),
                 getInt32(xfbStride), xfbOffset, getInt32(outputInfo.getData())},
                "");
}

// =====================================================================================================================
// Create a read of barycoord input value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
//
// @param builtIn : Built-in kind, BuiltInBaryCoord or BuiltInBaryCoordNoPerspKHR
// @param inputInfo : Extra input info
// @param auxInterpValue : Auxiliary value of interpolation
// @param instName : Name to give instruction(s)
llvm::Value *BuilderRecorder::CreateReadBaryCoord(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *auxInterpValue,
                                                  const llvm::Twine &instName) {
  Type *resultTy = getBuiltInTy(builtIn, inputInfo);
  return record(Opcode::ReadBaryCoord, resultTy,
                {
                    getInt32(builtIn),
                    getInt32(inputInfo.getData()),
                    auxInterpValue ? auxInterpValue : PoisonValue::get(getInt32Ty()),
                },
                instName);
}

// =====================================================================================================================
// Create a read of (part of) a built-in input value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if index is not nullptr.
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param inputInfo : Extra input info (shader-defined array length)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateReadBuiltInInput(BuiltInKind builtIn, InOutInfo inputInfo, Value *vertexIndex,
                                               Value *index, const Twine &instName) {
  Type *resultTy = getBuiltInTy(builtIn, inputInfo);
  if (index) {
    if (isa<ArrayType>(resultTy))
      resultTy = cast<ArrayType>(resultTy)->getElementType();
    else
      resultTy = cast<VectorType>(resultTy)->getElementType();
  }
  return record(Opcode::ReadBuiltInInput, resultTy,
                {
                    getInt32(builtIn),
                    getInt32(inputInfo.getData()),
                    vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                    index ? index : UndefValue::get(getInt32Ty()),
                },
                instName);
}

// =====================================================================================================================
// Create a read of (part of) a built-in output value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if index is not nullptr.
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param outputInfo : Extra output info (shader-defined array length)
// @param vertexIndex : For TCS per-vertex output: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, Value *vertexIndex,
                                                Value *index, const Twine &instName) {
  Type *resultTy = getBuiltInTy(builtIn, outputInfo);
  if (index) {
    if (isa<ArrayType>(resultTy))
      resultTy = cast<ArrayType>(resultTy)->getElementType();
    else
      resultTy = cast<VectorType>(resultTy)->getElementType();
  }
  return record(Opcode::ReadBuiltInOutput, resultTy,
                {
                    getInt32(builtIn),
                    getInt32(outputInfo.getData()),
                    vertexIndex ? vertexIndex : UndefValue::get(getInt32Ty()),
                    index ? index : UndefValue::get(getInt32Ty()),
                },
                instName);
}

// =====================================================================================================================
// Create a write of (part of) a built-in output value.
//
// @param valueToWrite : Value to write
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param outputInfo : Extra output info (shader-defined array length; GS stream id)
// @param vertexOrPrimitiveIndex : For TCS/mesh shader per-vertex output: vertex index; for mesh shader per-primitive
//                                 output: primitive index; else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
Instruction *BuilderRecorder::CreateWriteBuiltInOutput(Value *valueToWrite, BuiltInKind builtIn, InOutInfo outputInfo,
                                                       Value *vertexOrPrimitiveIndex, Value *index) {
  return record(Opcode::WriteBuiltInOutput, nullptr,
                {
                    valueToWrite,
                    getInt32(builtIn),
                    getInt32(outputInfo.getData()),
                    vertexOrPrimitiveIndex ? vertexOrPrimitiveIndex : UndefValue::get(getInt32Ty()),
                    index ? index : UndefValue::get(getInt32Ty()),
                },
                "");
}

#if VKI_RAY_TRACING
// =====================================================================================================================
// Create a ray intersect result with specified node in BVH buffer
//
// @param nodePtr : BVH node pointer
// @param extent : The valid range on which intersections can occur
// @param origin : Intersect ray origin
// @param direction : Intersect ray direction
// @param invDirection : The inverse of direction
// @param imageDesc : Image descriptor
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateImageBvhIntersectRay(Value *nodePtr, Value *extent, Value *origin, Value *direction,
                                                   Value *invDirection, Value *imageDesc, const Twine &instName) {
  return record(Opcode::ImageBvhIntersectRay, FixedVectorType::get(getInt32Ty(), 4),
                {nodePtr, extent, origin, direction, invDirection, imageDesc}, instName);
}

#endif
// =====================================================================================================================
// Create a read from (part of) a task payload.
// The result type is as specified by resultTy, a scalar or vector type with no more than four elements.
//
// @param resultTy : Type of value to read
// @param byteOffset : Byte offset within the payload structure
// @param instName : Name to give instruction(s)
// @returns : Value read from the task payload
Value *BuilderRecorder::CreateReadTaskPayload(Type *resultTy, Value *byteOffset, const Twine &instName) {
  return record(Opcode::ReadTaskPayload, resultTy, byteOffset, instName);
}

// =====================================================================================================================
// Create a write to (part of) a task payload.
//
// @param valueToWrite : Value to write
// @param byteOffset : Byte offset within the payload structure
// @param instName : Name to give instruction(s)
// @returns : Instruction to write value to task payload
Instruction *BuilderRecorder::CreateWriteTaskPayload(Value *valueToWrite, Value *byteOffset, const Twine &instName) {
  return record(Opcode::WriteTaskPayload, nullptr, {valueToWrite, byteOffset}, instName);
}

// =====================================================================================================================
// Create a task payload atomic operation other than compare-and-swap. An add of +1 or -1, or a sub
// of -1 or +1, is generated as inc or dec. Result type is the same as the input value type.
//
// @param atomicOp : Atomic op to create
// @param ordering : Atomic ordering
// @param inputValue : Input value
// @param byteOffset : Byte offset within the payload structure
// @param instName : Name to give instruction(s)
// @returns : Original value read from the task payload
Value *BuilderRecorder::CreateTaskPayloadAtomic(unsigned atomicOp, AtomicOrdering ordering, Value *inputValue,
                                                Value *byteOffset, const Twine &instName) {
  return record(Opcode::TaskPayloadAtomic, inputValue->getType(),
                {getInt32(atomicOp), getInt32(static_cast<unsigned>(ordering)), inputValue, byteOffset}, instName);
}

// =====================================================================================================================
// Create a task payload atomic compare-and-swap.
//
// @param ordering : Atomic ordering
// @param inputValue : Input value
// @param comparatorValue : Value to compare against
// @param byteOffset : Byte offset within the payload structure
// @param instName : Name to give instruction(s)
// @returns : Original value read from the task payload
Value *BuilderRecorder::CreateTaskPayloadAtomicCompareSwap(AtomicOrdering ordering, Value *inputValue,
                                                           Value *comparatorValue, Value *byteOffset,
                                                           const Twine &instName) {
  return record(Opcode::TaskPayloadAtomicCompareSwap, inputValue->getType(),
                {getInt32(static_cast<unsigned>(ordering)), inputValue, comparatorValue, byteOffset}, instName);
}

// =====================================================================================================================
// Create a get wave size query.
//
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateGetWaveSize(const Twine &instName) {
  return record(Opcode::GetWaveSize, getInt32Ty(), {}, instName);
}

// =====================================================================================================================
// Create a get subgroup size query.
//
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateGetSubgroupSize(const Twine &instName) {
  return record(Opcode::GetSubgroupSize, getInt32Ty(), {}, instName);
}

// =====================================================================================================================
// Create a subgroup elect.
//
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupElect(const Twine &instName) {
  return record(Opcode::SubgroupElect, getInt1Ty(), {}, instName);
}

// =====================================================================================================================
// Create a subgroup all.
//
// @param value : The value to compare
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupAll(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupAll, getInt1Ty(), {value}, instName);
}

// =====================================================================================================================
// Create a subgroup any
//
// @param value : The value to compare
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupAny(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupAny, getInt1Ty(), {value}, instName);
}

// =====================================================================================================================
// Create a subgroup all equal.
//
// @param value : The value to compare
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupAllEqual(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupAllEqual, getInt1Ty(), {value}, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast.
//
// @param value : The value to broadcast
// @param index : The index to broadcast from
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBroadcast(Value *const value, Value *const index, const Twine &instName) {
  return record(Opcode::SubgroupBroadcast, value->getType(), {value, index}, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast that may have a non-uniform index.
//
// @param value : The value to broadcast
// @param index : The index to broadcast from
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBroadcastWaterfall(Value *const value, Value *const index,
                                                         const Twine &instName) {
  return record(Opcode::SubgroupBroadcastWaterfall, value->getType(), {value, index}, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast first.
//
// @param value : The value to broadcast
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBroadcastFirst(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBroadcastFirst, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot.
//
// @param value : The value to contribute
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallot(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBallot, FixedVectorType::get(getInt32Ty(), 4), value, instName);
}

// =====================================================================================================================
// Create a subgroup inverse ballot.
//
// @param value : The ballot value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupInverseBallot(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupInverseBallot, getInt1Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot bit extract.
//
// @param value : The ballot value
// @param index : The index to extract from the ballot
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallotBitExtract(Value *const value, Value *const index, const Twine &instName) {
  return record(Opcode::SubgroupBallotBitExtract, getInt1Ty(), {value, index}, instName);
}

// =====================================================================================================================
// Create a subgroup ballot bit count.
//
// @param value : The ballot value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallotBitCount(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBallotBitCount, getInt32Ty(), value, instName);
}

// Create a subgroup ballot inclusive bit count.
//
// @param value : The ballot value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallotInclusiveBitCount(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBallotInclusiveBitCount, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot exclusive bit count.
//
// @param value : The ballot value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallotExclusiveBitCount(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBallotExclusiveBitCount, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot find least significant bit.
//
// @param value : The ballot value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallotFindLsb(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBallotFindLsb, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot find most significant bit.
//
// @param value : The ballot value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupBallotFindMsb(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupBallotFindMsb, getInt32Ty(), value, instName);
}

// =====================================================================================================================
// Create "fmix" operation, returning ( 1 - A ) * X + A * Y. Result would be FP scalar or vector.
//
// @param x : Left Value
// @param y : Right Value
// @param a : Wight Value
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::createFMix(Value *x, Value *y, Value *a, const Twine &instName) {
  return record(Opcode::FMix, x->getType(), {x, y, a}, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle.
//
// @param value : The value to shuffle
// @param index : The index to shuffle from
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupShuffle(Value *const value, Value *const index, const Twine &instName) {
  return record(Opcode::SubgroupShuffle, value->getType(), {value, index}, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle xor.
//
// @param value : The value to shuffle
// @param mask : The mask to shuffle with
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupShuffleXor(Value *const value, Value *const mask, const Twine &instName) {
  return record(Opcode::SubgroupShuffleXor, value->getType(), {value, mask}, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle up.
//
// @param value : The value to shuffle
// @param offset : The offset to shuffle up to
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupShuffleUp(Value *const value, Value *const offset, const Twine &instName) {
  return record(Opcode::SubgroupShuffleUp, value->getType(), {value, offset}, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle down.
//
// @param value : The value to shuffle
// @param offset : The offset to shuffle down to
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupShuffleDown(Value *const value, Value *const offset, const Twine &instName) {
  return record(Opcode::SubgroupShuffleDown, value->getType(), {value, offset}, instName);
}

// =====================================================================================================================
// Create a subgroup clustered reduction.
//
// @param groupArithOp : The group operation to perform
// @param value : The value to perform on
// @param clusterSize : The cluster size
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, Value *const value,
                                                         Value *const clusterSize, const Twine &instName) {
  return record(Opcode::SubgroupClusteredReduction, value->getType(), {getInt32(groupArithOp), value, clusterSize},
                instName);
}

// =====================================================================================================================
// Create a subgroup clustered inclusive scan.
//
// @param groupArithOp : The group operation to perform
// @param value : The value to perform on
// @param clusterSize : The cluster size
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, Value *const value,
                                                         Value *const clusterSize, const Twine &instName) {
  return record(Opcode::SubgroupClusteredInclusive, value->getType(), {getInt32(groupArithOp), value, clusterSize},
                instName);
}

// =====================================================================================================================
// Create a subgroup clustered exclusive scan.
//
// @param groupArithOp : The group operation to perform
// @param value : The value to perform on
// @param clusterSize : The cluster size
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, Value *const value,
                                                         Value *const clusterSize, const Twine &instName) {
  return record(Opcode::SubgroupClusteredExclusive, value->getType(), {getInt32(groupArithOp), value, clusterSize},
                instName);
}

// =====================================================================================================================
// Create a subgroup quad broadcast.
//
// @param value : The value to broadcast
// @param index : The index within the quad to broadcast from
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupQuadBroadcast(Value *const value, Value *const index, const Twine &instName) {
  return record(Opcode::SubgroupQuadBroadcast, value->getType(), {value, index}, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap horizontal.
//
// @param value : The value to swap
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupQuadSwapHorizontal(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupQuadSwapHorizontal, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap vertical.
//
// @param value : The value to swap
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupQuadSwapVertical(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupQuadSwapVertical, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup quad swap diagonal.
//
// @param value : The value to swap
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupQuadSwapDiagonal(Value *const value, const Twine &instName) {
  return record(Opcode::SubgroupQuadSwapDiagonal, value->getType(), value, instName);
}

// =====================================================================================================================
// Create a subgroup swizzle quad.
//
// @param value : The value to swizzle.
// @param offset : The value to specify the swizzle offsets.
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupSwizzleQuad(Value *const value, Value *const offset, const Twine &instName) {
  return record(Opcode::SubgroupSwizzleQuad, value->getType(), {value, offset}, instName);
}

// =====================================================================================================================
// Create a subgroup swizzle mask.
//
// @param value : The value to swizzle.
// @param mask : The value to specify the swizzle masks.
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupSwizzleMask(Value *const value, Value *const mask, const Twine &instName) {
  return record(Opcode::SubgroupSwizzleMask, value->getType(), {value, mask}, instName);
}

// =====================================================================================================================
// Create a subgroup write invocation.
//
// @param inputValue : The value to return for all but one invocations.
// @param writeValue : The value to return for one invocation.
// @param index : The index of the invocation that gets the write value.
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupWriteInvocation(Value *const inputValue, Value *const writeValue,
                                                      Value *const index, const Twine &instName) {
  return record(Opcode::SubgroupWriteInvocation, inputValue->getType(), {inputValue, writeValue, index}, instName);
}

// =====================================================================================================================
// Create a subgroup mbcnt.
//
// @param mask : The mask to mbcnt with.
// @param instName : Name to give instruction(s)
Value *BuilderRecorder::CreateSubgroupMbcnt(Value *const mask, const Twine &instName) {
  return record(Opcode::SubgroupMbcnt, getInt32Ty(), mask, instName);
}

// =====================================================================================================================
// Record one Builder call
//
// @param opcode : Opcode of Builder method call being recorded
// @param resultTy : Return type (can be nullptr for void)
// @param args : Arguments
// @param instName : Name to give instruction
Instruction *BuilderRecorder::record(BuilderRecorder::Opcode opcode, Type *resultTy, ArrayRef<Value *> args,
                                     const Twine &instName) {
  // Create mangled name of builder call. This only needs to be mangled on return type.
  std::string mangledName;
  {
    raw_string_ostream mangledNameStream(mangledName);
    mangledNameStream << BuilderCallPrefix;
    mangledNameStream << getCallName(opcode);
    if (resultTy) {
      mangledNameStream << ".";
      getTypeName(resultTy, mangledNameStream);
    } else
      resultTy = Type::getVoidTy(getContext());
  }

  // See if the declaration already exists in the module.
  Module *const module = GetInsertBlock()->getModule();
  Function *func = dyn_cast_or_null<Function>(module->getFunction(mangledName));
  if (!func) {
    // Does not exist. Create it as a varargs function.
    auto funcTy = FunctionType::get(resultTy, {}, true);
    func = Function::Create(funcTy, GlobalValue::ExternalLinkage, mangledName, module);

    // Add opcode metadata to the function, so that BuilderReplayer does not need to do a string comparison.
    // We do not add that metadata if doing -emit-lgc, so that a test constructed with -emit-lgc will rely
    // on the more stable lgc.create.* name rather than the less stable opcode.
    if (!m_omitOpcodes) {
      MDNode *const funcMeta = MDNode::get(getContext(), ConstantAsMetadata::get(getInt32(opcode)));
      func->setMetadata(opcodeMetaKindId, funcMeta);
    }

    // Add attributes.
    func->addFnAttr(Attribute::NoUnwind);
    switch (opcode) {
    case ACos:
    case ACosh:
    case ASin:
    case ASinh:
    case ATan:
    case ATan2:
    case ATanh:
    case Cosh:
    case Determinant:
    case Exp:
    case Sqrt:
    case InverseSqrt:
    case Log:
    case MatrixInverse:
    case Opcode::CrossProduct:
    case Opcode::CubeFaceCoord:
    case Opcode::CubeFaceIndex:
    case Opcode::Derivative:
    case Opcode::DotProduct:
    case Opcode::IntegerDotProduct:
    case Opcode::ExtractBitField:
    case Opcode::ExtractExponent:
    case Opcode::ExtractSignificand:
    case Opcode::FClamp:
    case Opcode::FMax:
    case Opcode::FMax3:
    case Opcode::FMid3:
    case Opcode::FMin:
    case Opcode::FMin3:
    case Opcode::FMix:
    case Opcode::FMod:
    case Opcode::FSign:
    case Opcode::FaceForward:
    case Opcode::FindSMsb:
    case Opcode::Fma:
    case Opcode::FpTruncWithRounding:
    case Opcode::Fract:
    case Opcode::GetDescPtr:
    case Opcode::GetDescStride:
    case Opcode::GetWaveSize:
    case Opcode::GetSubgroupSize:
    case Opcode::InsertBitField:
    case Opcode::IsInf:
    case Opcode::IsNaN:
    case Opcode::Ldexp:
    case Opcode::MatrixTimesMatrix:
    case Opcode::MatrixTimesScalar:
    case Opcode::MatrixTimesVector:
    case Opcode::NormalizeVector:
    case Opcode::OuterProduct:
    case Opcode::QuantizeToFp16:
    case Opcode::Reflect:
    case Opcode::Refract:
    case Opcode::SAbs:
    case Opcode::SMod:
    case Opcode::SSign:
    case Opcode::SmoothStep:
    case Opcode::TransposeMatrix:
    case Opcode::VectorTimesMatrix:
    case Power:
    case Sinh:
    case Tan:
    case Tanh:
    case Opcode::SubgroupBallotBitCount:
    case Opcode::SubgroupBallotBitExtract:
    case Opcode::SubgroupBallotExclusiveBitCount:
    case Opcode::SubgroupBallotFindLsb:
    case Opcode::SubgroupBallotFindMsb:
    case Opcode::SubgroupBallotInclusiveBitCount:
      // Functions that don't access memory.
      func->setDoesNotAccessMemory();
      break;
    case Opcode::ImageGather:
    case Opcode::ImageLoad:
    case Opcode::ImageLoadWithFmask:
    case Opcode::ImageSample:
    case Opcode::ImageSampleConvert:
    case Opcode::LoadBufferDesc:
    case Opcode::LoadPushConstantsPtr:
    case Opcode::ReadBaryCoord:
    case Opcode::ReadBuiltInInput:
    case Opcode::ReadBuiltInOutput:
    case Opcode::ReadGenericInput:
    case Opcode::ReadGenericOutput:
    case Opcode::ReadPerVertexInput:
    case Opcode::ReadTaskPayload:
      // Functions that only read memory.
      func->setOnlyReadsMemory();
      // Must be marked as returning for DCE.
      func->addFnAttr(Attribute::WillReturn);
      break;
    case Opcode::ImageStore:
      // Functions that only write memory.
      func->setOnlyWritesMemory();
      break;
    case Opcode::ImageAtomic:
    case Opcode::ImageAtomicCompareSwap:
    case Opcode::WriteXfbOutput:
    case Opcode::WriteTaskPayload:
    case Opcode::TaskPayloadAtomic:
    case Opcode::TaskPayloadAtomicCompareSwap:
      // Functions that read and write memory.
      break;
    case Opcode::SubgroupAll:
    case Opcode::SubgroupAllEqual:
    case Opcode::SubgroupAny:
    case Opcode::SubgroupBallot:
    case Opcode::SubgroupBroadcast:
    case Opcode::SubgroupBroadcastWaterfall:
    case Opcode::SubgroupBroadcastFirst:
    case Opcode::SubgroupClusteredExclusive:
    case Opcode::SubgroupClusteredInclusive:
    case Opcode::SubgroupClusteredReduction:
    case Opcode::SubgroupElect:
    case Opcode::SubgroupInverseBallot:
    case Opcode::SubgroupMbcnt:
    case Opcode::SubgroupQuadBroadcast:
    case Opcode::SubgroupQuadSwapDiagonal:
    case Opcode::SubgroupQuadSwapHorizontal:
    case Opcode::SubgroupQuadSwapVertical:
    case Opcode::SubgroupShuffle:
    case Opcode::SubgroupShuffleDown:
    case Opcode::SubgroupShuffleUp:
    case Opcode::SubgroupShuffleXor:
    case Opcode::SubgroupSwizzleMask:
    case Opcode::SubgroupSwizzleQuad:
    case Opcode::Barrier:
      // TODO: we should mark these functions 'ReadNone' in theory, but that need to wait until we fix all convergent
      // issues in LLVM optimizations.
      func->addFnAttr(Attribute::Convergent);
      break;
    case Opcode::SubgroupWriteInvocation:
    case Opcode::DemoteToHelperInvocation:
    case Opcode::EmitVertex:
    case Opcode::EndPrimitive:
    case Opcode::ImageGetLod:
    case Opcode::ImageQueryLevels:
    case Opcode::ImageQuerySamples:
    case Opcode::ImageQuerySize:
    case Opcode::IsHelperInvocation:
    case Opcode::EmitMeshTasks:
    case Opcode::SetMeshOutputs:
    case Opcode::Kill:
    case Opcode::ReadClock:
    case Opcode::DebugBreak:
    case Opcode::WriteBuiltInOutput:
    case Opcode::WriteGenericOutput:
#if VKI_RAY_TRACING
    case Opcode::ImageBvhIntersectRay:
#endif
      // TODO: These functions have not been classified yet.
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }
  }

  // Create the call.
  auto call = CreateCall(func, args, instName);

  return call;
}

// =====================================================================================================================
// Get the recorded call opcode from the function name. Asserts if not found.
// This does not have to be particularly efficient, as it is only used with the lgc command-line utility.
//
// @param name : Name of function declaration
// @returns : Opcode
BuilderRecorder::Opcode BuilderRecorder::getOpcodeFromName(StringRef name) {
  assert(name.startswith(BuilderCallPrefix));
  name = name.drop_front(strlen(BuilderCallPrefix));
  unsigned bestOpcode = 0;
  unsigned bestLength = 0;
  for (unsigned opcode = 0; opcode != Opcode::Count; ++opcode) {
    StringRef opcodeName = getCallName(static_cast<Opcode>(opcode));
    if (name.startswith(opcodeName)) {
      if (opcodeName.size() > bestLength) {
        bestLength = opcodeName.size();
        bestOpcode = opcode;
      }
    }
  }
  assert(bestLength != 0 && "No matching lgc.create.* name found!");
  return static_cast<Opcode>(bestOpcode);
}
