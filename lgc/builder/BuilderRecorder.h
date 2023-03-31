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
 * @file  BuilderRecorder.h
 * @brief LLPC header file: declaration of lgc::BuilderRecorder
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Builder.h"
#ifndef NDEBUG
#include "llvm/IR/ValueHandle.h"
#endif

namespace lgc {

// Prefix of all recorded Create* calls.
static const char BuilderCallPrefix[] = "lgc.create.";

// LLPC call opcode metadata name.
static const char BuilderCallOpcodeMetadataName[] = "lgc.create.opcode";

// lgc.call.* opcodes
enum BuilderOpcode : unsigned {
  // NOP
  Nop = 0,

  // Base class
  DotProduct,
  IntegerDotProduct,

  // Arithmetic
  CubeFaceCoord,
  CubeFaceIndex,
  FpTruncWithRounding,
  QuantizeToFp16,
  SMod,
  FMod,
  Fma,
  Tan,
  ASin,
  ACos,
  ATan,
  ATan2,
  Sinh,
  Cosh,
  Tanh,
  ASinh,
  ACosh,
  ATanh,
  Power,
  Exp,
  Log,
  Sqrt,
  InverseSqrt,
  SAbs,
  FSign,
  SSign,
  Fract,
  SmoothStep,
  Ldexp,
  ExtractSignificand,
  ExtractExponent,
  CrossProduct,
  NormalizeVector,
  FaceForward,
  Reflect,
  Refract,
  FClamp,
  FMin,
  FMax,
  FMin3,
  FMax3,
  FMid3,
  IsInf,
  IsNaN,
  InsertBitField,
  ExtractBitField,
  FindSMsb,
  FMix,

  // Descriptor
  LoadBufferDesc,
  GetDescStride,
  GetDescPtr,
  LoadPushConstantsPtr,

  // Image
  ImageLoad,
  ImageLoadWithFmask,
  ImageStore,
  ImageSample,
  ImageSampleConvert,
  ImageGather,
  ImageAtomic,
  ImageAtomicCompareSwap,
  ImageQueryLevels,
  ImageQuerySamples,
  ImageQuerySize,
  ImageGetLod,
#if VKI_RAY_TRACING
  ImageBvhIntersectRay,
  Reserved2,
#else
  Reserved2,
  Reserved1,
#endif

  // Input/output
  ReadGenericInput,
  ReadGenericOutput,
  ReadPerVertexInput,
  WriteGenericOutput,
  WriteXfbOutput,
  ReadBaryCoord,
  ReadBuiltInInput,
  ReadBuiltInOutput,
  WriteBuiltInOutput,
  ReadTaskPayload,
  WriteTaskPayload,
  TaskPayloadAtomic,
  TaskPayloadAtomicCompareSwap,

  // Matrix
  TransposeMatrix,
  MatrixTimesScalar,
  VectorTimesMatrix,
  MatrixTimesVector,
  MatrixTimesMatrix,
  OuterProduct,
  Determinant,
  MatrixInverse,

  // Misc.
  EmitVertex,
  EndPrimitive,
  Barrier,
  Kill,
  ReadClock,
  DebugPrintf,
  Derivative,
  DemoteToHelperInvocation,
  IsHelperInvocation,
  EmitMeshTasks,
  SetMeshOutputs,
  GetWaveSize,
  DebugBreak,

  // Subgroup
  GetSubgroupSize,
  SubgroupElect,
  SubgroupAll,
  SubgroupAny,
  SubgroupAllEqual,
  SubgroupBroadcast,
  SubgroupBroadcastWaterfall,
  SubgroupBroadcastFirst,
  SubgroupBallot,
  SubgroupInverseBallot,
  SubgroupBallotBitExtract,
  SubgroupBallotBitCount,
  SubgroupBallotInclusiveBitCount,
  SubgroupBallotExclusiveBitCount,
  SubgroupBallotFindLsb,
  SubgroupBallotFindMsb,
  SubgroupShuffle,
  SubgroupShuffleXor,
  SubgroupShuffleUp,
  SubgroupShuffleDown,
  SubgroupClusteredReduction,
  SubgroupClusteredInclusive,
  SubgroupClusteredExclusive,
  SubgroupQuadBroadcast,
  SubgroupQuadSwapHorizontal,
  SubgroupQuadSwapVertical,
  SubgroupQuadSwapDiagonal,
  SubgroupSwizzleQuad,
  SubgroupSwizzleMask,
  SubgroupWriteInvocation,
  SubgroupMbcnt,

  // Total count of opcodes
  Count
};

// =====================================================================================================================
// Builder recorder/replay utility class containing just a couple of static methods used both in BuilderRecorder.cpp
// and in BuilderReplayer.cpp.
class BuilderRecorder {
public:
  // Given an opcode, get the call name (without the "lgc.create." prefix)
  static llvm::StringRef getCallName(BuilderOpcode opcode);

  // Get the recorded call opcode from the function name. Asserts if not found.
  static BuilderOpcode getOpcodeFromName(llvm::StringRef name);
};

} // namespace lgc
