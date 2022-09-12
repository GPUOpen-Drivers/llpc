/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

namespace llvm {

class ModulePass;
class PassRegistry;

void initializeLegacyBuilderReplayerPass(PassRegistry &);

} // namespace llvm

namespace lgc {

class PipelineState;

// Prefix of all recorded Create* calls.
static const char BuilderCallPrefix[] = "lgc.create.";

// LLPC call opcode metadata name.
static const char BuilderCallOpcodeMetadataName[] = "lgc.create.opcode";

// =====================================================================================================================
// A class that caches the metadata kind IDs used by BuilderRecorder and BuilderReplayer.
class BuilderRecorderMetadataKinds {
public:
  BuilderRecorderMetadataKinds() {}
  BuilderRecorderMetadataKinds(llvm::LLVMContext &context);

  unsigned opcodeMetaKindId; // Cached metadata kinds for opcode
};

// =====================================================================================================================
// Builder recorder, to record all Builder calls as intrinsics
// Each call to a Builder method causes the insertion of a call to lgc.call.*, so the Builder calls can be replayed
// later on.
class BuilderRecorder final : public Builder, BuilderRecorderMetadataKinds {
  friend LgcContext;

public:
  // lgc.call.* opcodes
  enum Opcode : unsigned {
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
    GetBufferDescLength,
    PtrDiff,

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
    ImageBvhIntersectRayAMD,
#else
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
    Derivative,
    DemoteToHelperInvocation,
    IsHelperInvocation,
    EmitMeshTasks,
    SetMeshOutputs,
    GetWaveSize,

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

  // Given an opcode, get the call name (without the "lgc.create." prefix)
  static llvm::StringRef getCallName(Opcode opcode);

  // Get the recorded call opcode from the function name. Asserts if not found.
  static Opcode getOpcodeFromName(llvm::StringRef name);

  // Constructors
  BuilderRecorder(LgcContext *builderContext, Pipeline *pipeline, bool omitOpcodes);
  BuilderRecorder() = delete;
  BuilderRecorder(const BuilderRecorder &) = delete;
  BuilderRecorder &operator=(const BuilderRecorder &) = delete;

  // Record shader modes into IR metadata if this is a shader compile (no PipelineState).
  void recordShaderModes(llvm::Module *module) override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Base class operations

  llvm::Value *CreateDotProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                const llvm::Twine &instName = "") override final;

  llvm::Value *CreateIntegerDotProduct(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                       unsigned flags, const llvm::Twine &instName = "") override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Arithmetic operations

  // Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
  // the given cube map texture coordinates.
  llvm::Value *CreateCubeFaceCoord(llvm::Value *coord, const llvm::Twine &instName = "") override final;

  // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
  // the given cube map texture coordinates.
  llvm::Value *CreateCubeFaceIndex(llvm::Value *coord, const llvm::Twine &instName = "") override final;

  // Create scalar or vector FP truncate operation with rounding mode.
  llvm::Value *CreateFpTruncWithRounding(llvm::Value *value, llvm::Type *destTy, llvm::RoundingMode roundingMode,
                                         const llvm::Twine &instName = "") override final;

  // Create quantize operation.
  llvm::Value *CreateQuantizeToFp16(llvm::Value *value, const llvm::Twine &instName = "") override final;

  // Create signed integer or FP modulo operation.
  llvm::Value *CreateSMod(llvm::Value *dividend, llvm::Value *divisor, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFMod(llvm::Value *dividend, llvm::Value *divisor, const llvm::Twine &instName = "") override final;

  // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
  llvm::Value *CreateFma(llvm::Value *a, llvm::Value *b, llvm::Value *c,
                         const llvm::Twine &instName = "") override final;

  // Trig and exponent operations.
  llvm::Value *CreateTan(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateASin(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateACos(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateATan(llvm::Value *yOverX, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateATan2(llvm::Value *y, llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateSinh(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateCosh(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateTanh(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateASinh(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateACosh(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateATanh(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreatePower(llvm::Value *x, llvm::Value *y, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateExp(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateLog(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateSqrt(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateInverseSqrt(llvm::Value *x, const llvm::Twine &instName = "") override final;

  // General arithmetic operations.
  llvm::Value *CreateSAbs(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFSign(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateSSign(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFract(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateSmoothStep(llvm::Value *edge0, llvm::Value *edge1, llvm::Value *xValue,
                                const llvm::Twine &instName = "") override final;
  llvm::Value *CreateLdexp(llvm::Value *x, llvm::Value *exp, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateExtractSignificand(llvm::Value *value, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateExtractExponent(llvm::Value *value, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateCrossProduct(llvm::Value *x, llvm::Value *y, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateNormalizeVector(llvm::Value *x, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFaceForward(llvm::Value *n, llvm::Value *i, llvm::Value *nref,
                                 const llvm::Twine &instName = "") override final;
  llvm::Value *CreateReflect(llvm::Value *i, llvm::Value *n, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateRefract(llvm::Value *i, llvm::Value *n, llvm::Value *eta,
                             const llvm::Twine &instName = "") override final;

  // Create "fclamp" operation.
  llvm::Value *CreateFClamp(llvm::Value *x, llvm::Value *minVal, llvm::Value *maxVal,
                            const llvm::Twine &instName = "") override final;

  // FP min/max
  llvm::Value *CreateFMin(llvm::Value *value1, llvm::Value *value2, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFMax(llvm::Value *value1, llvm::Value *value2, const llvm::Twine &instName = "") override final;

  // Methods for trinary min/max/mid.
  llvm::Value *CreateFMin3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                           const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFMax3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                           const llvm::Twine &instName = "") override final;
  llvm::Value *CreateFMid3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                           const llvm::Twine &instName = "") override final;

  // Create derivative calculation on float or vector of float or half
  llvm::Value *CreateDerivative(llvm::Value *value, bool isDirectionY, bool isFine,
                                const llvm::Twine &instName = "") override final;

  // Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
  llvm::Value *CreateIsInf(llvm::Value *x, const llvm::Twine &instName = "") override final;

  // Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
  llvm::Value *CreateIsNaN(llvm::Value *x, const llvm::Twine &instName = "") override final;

  // Create an "insert bitfield" operation for a (vector of) integer type.
  llvm::Value *CreateInsertBitField(llvm::Value *base, llvm::Value *insert, llvm::Value *offset, llvm::Value *count,
                                    const llvm::Twine &instName = "") override final;

  // Create an "extract bitfield " operation for a (vector of) i32.
  llvm::Value *CreateExtractBitField(llvm::Value *base, llvm::Value *offset, llvm::Value *count, bool isSigned,
                                     const llvm::Twine &instName = "") override final;

  // Create "find MSB" operation for a (vector of) signed int.
  llvm::Value *CreateFindSMsb(llvm::Value *value, const llvm::Twine &instName = "") override final;

  // Create "fmix" operation.
  llvm::Value *createFMix(llvm::Value *x, llvm::Value *y, llvm::Value *a,
                          const llvm::Twine &instName = "") override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Descriptor operations

  llvm::Value *CreateLoadBufferDesc(unsigned descSet, unsigned binding, llvm::Value *descIndex, unsigned flags,
                                    llvm::Type *pointeeTy, const llvm::Twine &instName) override final;

  llvm::Value *CreateGetDescStride(ResourceNodeType concreteType, ResourceNodeType abstractType, unsigned descSet,
                                   unsigned binding, const llvm::Twine &instName) override final;

  llvm::Value *CreateGetDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, unsigned descSet,
                                unsigned binding, const llvm::Twine &instName) override final;

  llvm::Value *CreateLoadPushConstantsPtr(llvm::Type *returnTy, const llvm::Twine &instName) override final;

  llvm::Value *CreateGetBufferDescLength(llvm::Value *const bufferDesc, llvm::Value *offset,
                                         const llvm::Twine &instName = "") override final;

  llvm::Value *CreatePtrDiff(llvm::Type *ty, llvm::Value *lhs, llvm::Value *rhs,
                             const llvm::Twine &instName = "") override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Image operations

  // Create an image load.
  llvm::Value *CreateImageLoad(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                               llvm::Value *coord, llvm::Value *mipLevel,
                               const llvm::Twine &instName = "") override final;

  // Create an image load with F-mask.
  llvm::Value *CreateImageLoadWithFmask(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                        llvm::Value *fmaskDesc, llvm::Value *coord, llvm::Value *sampleNum,
                                        const llvm::Twine &instName) override final;

  // Create an image store.
  llvm::Value *CreateImageStore(llvm::Value *texel, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                llvm::Value *coord, llvm::Value *mipLevel,
                                const llvm::Twine &instName = "") override final;

  // Create an image sample.
  llvm::Value *CreateImageSample(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                 llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                 const llvm::Twine &instName = "") override final;

  // Create an image sample with conversion.
  llvm::Value *CreateImageSampleConvert(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDescArray,
                                        llvm::Value *convertingSamplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                        const llvm::Twine &instName = "") override final;

  // Create an image gather.
  llvm::Value *CreateImageGather(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                 llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                 const llvm::Twine &instName = "") override final;

  // Create an image atomic operation other than compare-and-swap.
  llvm::Value *CreateImageAtomic(unsigned atomicOp, unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                 llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                 const llvm::Twine &instName = "") override final;

  // Create an image atomic compare-and-swap.
  llvm::Value *CreateImageAtomicCompareSwap(unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                            llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                            llvm::Value *comparatorValue,
                                            const llvm::Twine &instName = "") override final;

  // Create a query of the number of mipmap levels in an image. Returns an i32 value.
  llvm::Value *CreateImageQueryLevels(unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                      const llvm::Twine &instName = "") override final;

  // Create a query of the number of samples in an image. Returns an i32 value.
  llvm::Value *CreateImageQuerySamples(unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                       const llvm::Twine &instName = "") override final;

  // Create a query of size of an image at the specified LOD
  llvm::Value *CreateImageQuerySize(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *lod,
                                    const llvm::Twine &instName = "") override final;

  // Create a get of the LOD that would be used for an image sample with the given coordinates
  // and implicit LOD.
  llvm::Value *CreateImageGetLod(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *samplerDesc,
                                 llvm::Value *coord, const llvm::Twine &instName = "") override final;

#if VKI_RAY_TRACING
  // Create a ray intersect result with specified node in BVH buffer
  llvm::Value *CreateImageBvhIntersectRay(llvm::Value *nodePtr, llvm::Value *extent, llvm::Value *origin,
                                          llvm::Value *direction, llvm::Value *invDirection, llvm::Value *imageDesc,
                                          const llvm::Twine &instName = "") override final;
#endif

  // -----------------------------------------------------------------------------------------------------------------
  // Shader input/output methods

  // Create a read of (part of) a user input value.
  llvm::Value *CreateReadGenericInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                      llvm::Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                      llvm::Value *vertexIndex, const llvm::Twine &instName = "") override final;

  // Create a read of (part of) a user output value.
  llvm::Value *CreateReadGenericOutput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                       llvm::Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                       llvm::Value *vertexIndex, const llvm::Twine &instName = "") override final;

  // Create a write of (part of) a user output value.
  llvm::Instruction *CreateWriteGenericOutput(llvm::Value *valueToWrite, unsigned location, llvm::Value *locationOffset,
                                              llvm::Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                              llvm::Value *vertexOrPrimitiveIndex) override final;

  // Create a write to an XFB (transform feedback / streamout) buffer.
  llvm::Instruction *CreateWriteXfbOutput(llvm::Value *valueToWrite, bool isBuiltIn, unsigned location,
                                          unsigned xfbBuffer, unsigned xfbStride, llvm::Value *xfbOffset,
                                          InOutInfo outputInfo) override final;

  // Create a read of barycoord input value.
  llvm::Value *CreateReadBaryCoord(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *auxInterpValue,
                                   const llvm::Twine &instName = "") override final;

  // Create a read of (part of) a built-in input value.
  llvm::Value *CreateReadBuiltInInput(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *vertexIndex,
                                      llvm::Value *index, const llvm::Twine &instName = "") override final;

  // Create a read of (part of) a built-in output value.
  llvm::Value *CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, llvm::Value *vertexIndex,
                                       llvm::Value *index, const llvm::Twine &instName = "") override final;

  // Create a write of (part of) a built-in output value.
  llvm::Instruction *CreateWriteBuiltInOutput(llvm::Value *valueToWrite, BuiltInKind builtIn, InOutInfo outputInfo,
                                              llvm::Value *vertexOrPrimitiveIndex, llvm::Value *index) override final;

  // Create a read of (part of) a pervertex input value.
  llvm::Value *CreateReadPerVertexInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                        llvm::Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                        llvm::Value *vertexIndex, const llvm::Twine &instName = "") override final;

  // Create a read of (part of) a task payload.
  llvm::Value *CreateReadTaskPayload(llvm::Type *resultTy, llvm::Value *byteOffset,
                                     const llvm::Twine &instName = "") override final;

  // Create a write of (part of) a task payload.
  llvm::Instruction *CreateWriteTaskPayload(llvm::Value *valueToWrite, llvm::Value *byteOffset,
                                            const llvm::Twine &instName = "") override final;

  // Create a task payload atomic operation other than compare-and-swap.
  llvm::Value *CreateTaskPayloadAtomic(unsigned atomicOp, llvm::AtomicOrdering ordering, llvm::Value *inputValue,
                                       llvm::Value *byteOffset, const llvm::Twine &instName = "") override final;

  // Create a task payload atomic compare-and-swap.
  llvm::Value *CreateTaskPayloadAtomicCompareSwap(llvm::AtomicOrdering ordering, llvm::Value *inputValue,
                                                  llvm::Value *comparatorValue, llvm::Value *byteOffset,
                                                  const llvm::Twine &instName = "") override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Miscellaneous operations

  // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
  // the current output primitive in the specified output-primitive stream.
  llvm::Instruction *CreateEmitVertex(unsigned streamId) override final;

  // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
  llvm::Instruction *CreateEndPrimitive(unsigned streamId) override final;

  // Create a workgroup control barrier.
  llvm::Instruction *CreateBarrier() override final;

  llvm::Instruction *CreateKill(const llvm::Twine &instName = "") override final;
  llvm::Instruction *CreateReadClock(bool realtime, const llvm::Twine &instName = "") override final;
  llvm::Instruction *CreateDemoteToHelperInvocation(const llvm::Twine &instName) override final;
  llvm::Value *CreateIsHelperInvocation(const llvm::Twine &instName) override final;
  llvm::Instruction *CreateEmitMeshTasks(llvm::Value *groupCountX, llvm::Value *groupCountY, llvm::Value *groupCountZ,
                                         const llvm::Twine &instName = "") override final;
  llvm::Instruction *CreateSetMeshOutputs(llvm::Value *vertexCount, llvm::Value *primitiveCount,
                                          const llvm::Twine &instName = "") override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Builder methods implemented in MatrixBuilder
  llvm::Value *CreateTransposeMatrix(llvm::Value *const matrix, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateMatrixTimesScalar(llvm::Value *const matrix, llvm::Value *const scalar,
                                       const llvm::Twine &instName = "") override final;
  llvm::Value *CreateVectorTimesMatrix(llvm::Value *const vector, llvm::Value *const matrix,
                                       const llvm::Twine &instName = "") override final;
  llvm::Value *CreateMatrixTimesVector(llvm::Value *const matrix, llvm::Value *const vector,
                                       const llvm::Twine &instName = "") override final;
  llvm::Value *CreateMatrixTimesMatrix(llvm::Value *const matrix1, llvm::Value *const matrix2,
                                       const llvm::Twine &instName = "") override final;
  llvm::Value *CreateOuterProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                  const llvm::Twine &instName = "") override final;
  llvm::Value *CreateDeterminant(llvm::Value *const matrix, const llvm::Twine &instName = "") override final;
  llvm::Value *CreateMatrixInverse(llvm::Value *const matrix, const llvm::Twine &instName = "") override final;

  // -----------------------------------------------------------------------------------------------------------------
  // Subgroup operations
  llvm::Value *CreateGetWaveSize(const llvm::Twine &instName) override final;
  llvm::Value *CreateGetSubgroupSize(const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupElect(const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupAll(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupAny(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupAllEqual(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBroadcast(llvm::Value *const value, llvm::Value *const index,
                                       const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBroadcastWaterfall(llvm::Value *const value, llvm::Value *const index,
                                                const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBroadcastFirst(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallot(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupInverseBallot(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallotBitExtract(llvm::Value *const value, llvm::Value *const index,
                                              const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallotBitCount(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallotInclusiveBitCount(llvm::Value *const value,
                                                     const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallotExclusiveBitCount(llvm::Value *const value,
                                                     const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallotFindLsb(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupBallotFindMsb(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupShuffle(llvm::Value *const value, llvm::Value *const index,
                                     const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupShuffleXor(llvm::Value *const value, llvm::Value *const mask,
                                        const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupShuffleUp(llvm::Value *const value, llvm::Value *const delta,
                                       const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupShuffleDown(llvm::Value *const value, llvm::Value *const delta,
                                         const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize,
                                                const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize,
                                                const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize,
                                                const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupQuadBroadcast(llvm::Value *const value, llvm::Value *const index,
                                           const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupQuadSwapHorizontal(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupQuadSwapVertical(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupQuadSwapDiagonal(llvm::Value *const value, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupSwizzleQuad(llvm::Value *const value, llvm::Value *const offset,
                                         const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupSwizzleMask(llvm::Value *const value, llvm::Value *const mask,
                                         const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupWriteInvocation(llvm::Value *const inputValue, llvm::Value *const writeValue,
                                             llvm::Value *const index, const llvm::Twine &instName) override final;
  llvm::Value *CreateSubgroupMbcnt(llvm::Value *const mask, const llvm::Twine &instName) override final;

protected:
  // Get the ShaderModes object.
  ShaderModes *getShaderModes() override final;

private:
  // Record one Builder call
  llvm::Instruction *record(Opcode opcode, llvm::Type *returnTy, llvm::ArrayRef<llvm::Value *> args,
                            const llvm::Twine &instName);

  PipelineState *m_pipelineState;             // PipelineState; nullptr for shader compile
  std::unique_ptr<ShaderModes> m_shaderModes; // ShaderModes for a shader compile
  bool m_omitOpcodes;                         // Omit opcodes on lgc.create.* function declarations
};

// Create BuilderReplayer pass
llvm::ModulePass *createLegacyBuilderReplayer(Pipeline *pipeline);

} // namespace lgc
