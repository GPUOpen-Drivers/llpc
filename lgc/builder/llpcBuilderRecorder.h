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
 * @file  llpcBuilderRecorder.h
 * @brief LLPC header file: declaration of lgc::BuilderRecorder
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/llpcBuilder.h"
#ifndef NDEBUG
#include "llvm/IR/ValueHandle.h"
#endif

namespace llvm
{

class ModulePass;
class PassRegistry;

void initializeBuilderReplayerPass(PassRegistry&);

} // llvm

namespace lgc
{

class PipelineState;

// Prefix of all recorded calls.
static const char BuilderCallPrefix[] = "llpc.call.";

// LLPC call opcode metadata name.
static const char BuilderCallOpcodeMetadataName[] = "llpc.call.opcode";

// =====================================================================================================================
// A class that caches the metadata kind IDs used by BuilderRecorder and BuilderReplayer.
class BuilderRecorderMetadataKinds
{
public:
    BuilderRecorderMetadataKinds() {}
    BuilderRecorderMetadataKinds(llvm::LLVMContext& context);

    unsigned        m_opcodeMetaKindId;                         // Cached metadata kinds for opcode
};

// =====================================================================================================================
// Builder recorder, to record all Builder calls as intrinsics
// Each call to a Builder method causes the insertion of a call to llpc.call.*, so the Builder calls can be replayed
// later on.
class BuilderRecorder final : public Builder, BuilderRecorderMetadataKinds
{
    friend BuilderContext;

public:
    // llpc.call.* opcodes
    enum Opcode : unsigned
    {
        // NOP
        Nop = 0,

        // Base class
        DotProduct,

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
        IndexDescPtr,
        LoadDescFromPtr,
        GetSamplerDescPtr,
        GetImageDescPtr,
        GetTexelBufferDescPtr,
        GetFmaskDescPtr,
        LoadPushConstantsPtr,
        GetBufferDescLength,

        // Image
        ImageLoad,
        ImageLoadWithFmask,
        ImageStore,
        ImageSample,
        ImageGather,
        ImageAtomic,
        ImageAtomicCompareSwap,
        ImageQueryLevels,
        ImageQuerySamples,
        ImageQuerySize,
        ImageGetLod,
        Reserved1,

        // Input/output
        ReadGenericInput,
        ReadGenericOutput,
        WriteGenericOutput,
        WriteXfbOutput,
        ReadBuiltInInput,
        ReadBuiltInOutput,
        WriteBuiltInOutput,

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

        // Subgroup
        GetSubgroupSize,
        SubgroupElect,
        SubgroupAll,
        SubgroupAny,
        SubgroupAllEqual,
        SubgroupBroadcast,
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
    };

    // Given an opcode, get the call name (without the "llpc.call." prefix)
    static llvm::StringRef GetCallName(Opcode opcode);

    // Record shader modes into IR metadata if this is a shader compile (no PipelineState).
    void RecordShaderModes(llvm::Module* pModule) override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Base class operations

    llvm::Value* CreateDotProduct(llvm::Value* const pVector1,
                            llvm::Value* const pVector2,
                            const llvm::Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Arithmetic operations

    // Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
    // the given cube map texture coordinates.
    llvm::Value* CreateCubeFaceCoord(llvm::Value* pCoord, const llvm::Twine& instName = "") override final;

    // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
    // the given cube map texture coordinates.
    llvm::Value* CreateCubeFaceIndex(llvm::Value* pCoord, const llvm::Twine& instName = "") override final;

    // Create scalar or vector FP truncate operation with rounding mode.
    llvm::Value* CreateFpTruncWithRounding(llvm::Value*            pValue,
                                     llvm::Type*             pDestTy,
                                     unsigned          roundingMode,
                                     const llvm::Twine&      instName = "") override final;

    // Create quantize operation.
    llvm::Value* CreateQuantizeToFp16(llvm::Value* pValue, const llvm::Twine& instName = "") override final;

    // Create signed integer or FP modulo operation.
    llvm::Value* CreateSMod(llvm::Value* pDividend, llvm::Value* pDivisor, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFMod(llvm::Value* pDividend, llvm::Value* pDivisor, const llvm::Twine& instName = "") override final;

    // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
    llvm::Value* CreateFma(llvm::Value* pA, llvm::Value* pB, llvm::Value* pC, const llvm::Twine& instName = "") override final;

    // Trig and exponent operations.
    llvm::Value* CreateTan(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateASin(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateACos(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateATan(llvm::Value* pYOverX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateATan2(llvm::Value* pY, llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateSinh(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateCosh(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateTanh(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateASinh(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateACosh(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateATanh(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreatePower(llvm::Value* pX, llvm::Value* pY, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateExp(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateLog(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateInverseSqrt(llvm::Value* pX, const llvm::Twine& instName = "") override final;

    // General arithmetic operations.
    llvm::Value* CreateSAbs(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFSign(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateSSign(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFract(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateSmoothStep(llvm::Value* pEdge0, llvm::Value* pEdge1, llvm::Value* pXValue, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateLdexp(llvm::Value* pX, llvm::Value* pExp, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateExtractSignificand(llvm::Value* pValue, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateExtractExponent(llvm::Value* pValue, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateCrossProduct(llvm::Value* pX, llvm::Value* pY, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateNormalizeVector(llvm::Value* pX, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFaceForward(llvm::Value* pN, llvm::Value* pI, llvm::Value* pNref, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateReflect(llvm::Value* pI, llvm::Value* pN, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateRefract(llvm::Value* pI, llvm::Value* pN, llvm::Value* pEta, const llvm::Twine& instName = "") override final;

    // Create "fclamp" operation.
    llvm::Value* CreateFClamp(llvm::Value* pX, llvm::Value* pMinVal, llvm::Value* pMaxVal, const llvm::Twine& instName = "") override final;

    // FP min/max
    llvm::Value* CreateFMin(llvm::Value* pValue1, llvm::Value* pValue2, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFMax(llvm::Value* pValue1, llvm::Value* pValue2, const llvm::Twine& instName = "") override final;

    // Methods for trinary min/max/mid.
    llvm::Value* CreateFMin3(llvm::Value* pValue1, llvm::Value* pValue2, llvm::Value* pValue3, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFMax3(llvm::Value* pValue1, llvm::Value* pValue2, llvm::Value* pValue3, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFMid3(llvm::Value* pValue1, llvm::Value* pValue2, llvm::Value* pValue3, const llvm::Twine& instName = "") override final;

    // Create derivative calculation on float or vector of float or half
    llvm::Value* CreateDerivative(llvm::Value* pValue, bool isDirectionY, bool isFine, const llvm::Twine& instName = "") override final;

    // Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
    llvm::Value* CreateIsInf(llvm::Value* pX, const llvm::Twine& instName = "") override final;

    // Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
    llvm::Value* CreateIsNaN(llvm::Value* pX, const llvm::Twine& instName = "") override final;

    // Create an "insert bitfield" operation for a (vector of) integer type.
    llvm::Value* CreateInsertBitField(llvm::Value*        pBase,
                                llvm::Value*        pInsert,
                                llvm::Value*        pOffset,
                                llvm::Value*        pCount,
                                const llvm::Twine&  instName = "") override final;

    // Create an "extract bitfield " operation for a (vector of) i32.
    llvm::Value* CreateExtractBitField(llvm::Value*        pBase,
                                 llvm::Value*        pOffset,
                                 llvm::Value*        pCount,
                                 bool          isSigned,
                                 const llvm::Twine&  instName = "") override final;

    // Create "find MSB" operation for a (vector of) signed int.
    llvm::Value* CreateFindSMsb(llvm::Value* pValue, const llvm::Twine& instName = "") override final;

    // Create "fmix" operation.
    llvm::Value* CreateFMix(llvm::Value* pX, llvm::Value* pY, llvm::Value* pA, const llvm::Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Descriptor operations

    llvm::Value* CreateLoadBufferDesc(unsigned      descSet,
                                unsigned      binding,
                                llvm::Value*        pDescIndex,
                                bool          isNonUniform,
                                bool          isWritten,
                                llvm::Type*         pPointeeTy,
                                const llvm::Twine&  instName) override final;

    llvm::Value* CreateIndexDescPtr(llvm::Value*        pDescPtr,
                              llvm::Value*        pIndex,
                              bool          isNonUniform,
                              const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadDescFromPtr(llvm::Value*        pDescPtr,
                                 const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetSamplerDescPtr(unsigned      descSet,
                                   unsigned      binding,
                                   const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetImageDescPtr(unsigned      descSet,
                                 unsigned      binding,
                                 const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetTexelBufferDescPtr(unsigned      descSet,
                                       unsigned      binding,
                                       const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetFmaskDescPtr(unsigned      descSet,
                                 unsigned      binding,
                                 const llvm::Twine&  instName) override final;

    llvm::Value* CreateLoadPushConstantsPtr(llvm::Type*         pPushConstantsTy,
                                      const llvm::Twine&  instName) override final;

    llvm::Value* CreateGetBufferDescLength(llvm::Value* const pBufferDesc,
                                     const llvm::Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Image operations

    // Create an image load.
    llvm::Value* CreateImageLoad(llvm::Type*               pResultTy,
                           unsigned            dim,
                           unsigned            flags,
                           llvm::Value*              pImageDesc,
                           llvm::Value*              pCoord,
                           llvm::Value*              pMipLevel,
                           const llvm::Twine&        instName = "") override final;

    // Create an image load with F-mask.
    llvm::Value* CreateImageLoadWithFmask(llvm::Type*                   pResultTy,
                                    unsigned                dim,
                                    unsigned                flags,
                                    llvm::Value*                  pImageDesc,
                                    llvm::Value*                  pFmaskDesc,
                                    llvm::Value*                  pCoord,
                                    llvm::Value*                  pSampleNum,
                                    const llvm::Twine&            instName) override final;

    // Create an image store.
    llvm::Value* CreateImageStore(llvm::Value*           pTexel,
                            unsigned         dim,
                            unsigned         flags,
                            llvm::Value*           pImageDesc,
                            llvm::Value*           pCoord,
                            llvm::Value*           pMipLevel,
                            const llvm::Twine&     instName = "") override final;

    // Create an image sample.
    llvm::Value* CreateImageSample(llvm::Type*             pResultTy,
                             unsigned          dim,
                             unsigned          flags,
                             llvm::Value*            pImageDesc,
                             llvm::Value*            pSamplerDesc,
                             llvm::ArrayRef<llvm::Value*>  address,
                             const llvm::Twine&      instName = "") override final;

    // Create an image gather.
    llvm::Value* CreateImageGather(llvm::Type*             pResultTy,
                             unsigned          dim,
                             unsigned          flags,
                             llvm::Value*            pImageDesc,
                             llvm::Value*            pSamplerDesc,
                             llvm::ArrayRef<llvm::Value*>  address,
                             const llvm::Twine&      instName = "") override final;

    // Create an image atomic operation other than compare-and-swap.
    llvm::Value* CreateImageAtomic(unsigned         atomicOp,
                             unsigned         dim,
                             unsigned         flags,
                             llvm::AtomicOrdering   ordering,
                             llvm::Value*           pImageDesc,
                             llvm::Value*           pCoord,
                             llvm::Value*           pInputValue,
                             const llvm::Twine&     instName = "") override final;

    // Create an image atomic compare-and-swap.
    llvm::Value* CreateImageAtomicCompareSwap(unsigned        dim,
                                        unsigned        flags,
                                        llvm::AtomicOrdering  ordering,
                                        llvm::Value*          pImageDesc,
                                        llvm::Value*          pCoord,
                                        llvm::Value*          pInputValue,
                                        llvm::Value*          pComparatorValue,
                                        const llvm::Twine&    instName = "") override final;

    // Create a query of the number of mipmap levels in an image. Returns an i32 value.
    llvm::Value* CreateImageQueryLevels(unsigned                dim,
                                  unsigned                flags,
                                  llvm::Value*                  pImageDesc,
                                  const llvm::Twine&            instName = "") override final;

    // Create a query of the number of samples in an image. Returns an i32 value.
    llvm::Value* CreateImageQuerySamples(unsigned                dim,
                                   unsigned                flags,
                                   llvm::Value*                  pImageDesc,
                                   const llvm::Twine&            instName = "") override final;

    // Create a query of size of an image at the specified LOD
    llvm::Value* CreateImageQuerySize(unsigned          dim,
                                unsigned          flags,
                                llvm::Value*            pImageDesc,
                                llvm::Value*            pLod,
                                const llvm::Twine&      instName = "") override final;

    // Create a get of the LOD that would be used for an image sample with the given coordinates
    // and implicit LOD.
    llvm::Value* CreateImageGetLod(unsigned          dim,
                             unsigned          flags,
                             llvm::Value*            pImageDesc,
                             llvm::Value*            pSamplerDesc,
                             llvm::Value*            pCoord,
                             const llvm::Twine&      instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Shader input/output methods

    // Create a read of (part of) a user input value.
    llvm::Value* CreateReadGenericInput(llvm::Type*         pResultTy,
                                  unsigned      location,
                                  llvm::Value*        pLocationOffset,
                                  llvm::Value*        pElemIdx,
                                  unsigned      locationCount,
                                  InOutInfo     inputInfo,
                                  llvm::Value*        pVertexIndex,
                                  const llvm::Twine&  instName = "") override final;

    // Create a read of (part of) a user output value.
    llvm::Value* CreateReadGenericOutput(llvm::Type*         pResultTy,
                                   unsigned      location,
                                   llvm::Value*        pLocationOffset,
                                   llvm::Value*        pElemIdx,
                                   unsigned      locationCount,
                                   InOutInfo     outputInfo,
                                   llvm::Value*        pVertexIndex,
                                   const llvm::Twine&  instName = "") override final;

    // Create a write of (part of) a user output value.
    llvm::Instruction* CreateWriteGenericOutput(llvm::Value*        pValueToWrite,
                                          unsigned      location,
                                          llvm::Value*        pLocationOffset,
                                          llvm::Value*        pElemIdx,
                                          unsigned      locationCount,
                                          InOutInfo     outputInfo,
                                          llvm::Value*        pVertexIndex) override final;

    // Create a write to an XFB (transform feedback / streamout) buffer.
    llvm::Instruction* CreateWriteXfbOutput(llvm::Value*        pValueToWrite,
                                      bool          isBuiltIn,
                                      unsigned      location,
                                      unsigned      xfbBuffer,
                                      unsigned      xfbStride,
                                      llvm::Value*        pXfbOffset,
                                      InOutInfo     outputInfo) override final;

    // Create a read of (part of) a built-in input value.
    llvm::Value* CreateReadBuiltInInput(BuiltInKind  builtIn,
                                  InOutInfo    inputInfo,
                                  llvm::Value*       pVertexIndex,
                                  llvm::Value*       pIndex,
                                  const llvm::Twine& instName = "") override final;

    // Create a read of (part of) a built-in output value.
    llvm::Value* CreateReadBuiltInOutput(BuiltInKind  builtIn,
                                   InOutInfo    outputInfo,
                                   llvm::Value*       pVertexIndex,
                                   llvm::Value*       pIndex,
                                   const llvm::Twine& instName = "") override final;

    // Create a write of (part of) a built-in output value.
    llvm::Instruction* CreateWriteBuiltInOutput(llvm::Value*        pValueToWrite,
                                          BuiltInKind   builtIn,
                                          InOutInfo     outputInfo,
                                          llvm::Value*        pVertexIndex,
                                          llvm::Value*        pIndex) override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Miscellaneous operations

    // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
    // the current output primitive in the specified output-primitive stream.
    llvm::Instruction* CreateEmitVertex(unsigned streamId) override final;

    // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
    llvm::Instruction* CreateEndPrimitive(unsigned streamId) override final;

    // Create a workgroup control barrier.
    llvm::Instruction* CreateBarrier() override final;

    llvm::Instruction* CreateKill(const llvm::Twine& instName = "") override final;
    llvm::Instruction* CreateReadClock(bool realtime, const llvm::Twine& instName = "") override final;
    llvm::Instruction* CreateDemoteToHelperInvocation(const llvm::Twine& instName) override final;
    llvm::Value* CreateIsHelperInvocation(const llvm::Twine& instName) override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Builder methods implemented in BuilderImplMatrix
    llvm::Value* CreateTransposeMatrix(llvm::Value* const pMatrix, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateMatrixTimesScalar(llvm::Value* const pMatrix,
                                   llvm::Value* const pScalar,
                                   const llvm::Twine& instName = "") override final;
    llvm::Value* CreateVectorTimesMatrix(llvm::Value* const pVector,
                                   llvm::Value* const pMatrix,
                                   const llvm::Twine& instName = "") override final;
    llvm::Value* CreateMatrixTimesVector(llvm::Value* const pMatrix,
                                   llvm::Value* const pVector,
                                   const llvm::Twine& instName = "") override final;
    llvm::Value* CreateMatrixTimesMatrix(llvm::Value* const pMatrix1,
                                   llvm::Value* const pMatrix2,
                                   const llvm::Twine& instName = "") override final;
    llvm::Value* CreateOuterProduct(llvm::Value* const pVector1,
                              llvm::Value* const pVector2,
                              const llvm::Twine& instName = "") override final;
    llvm::Value* CreateDeterminant(llvm::Value* const pMatrix, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateMatrixInverse(llvm::Value* const pMatrix, const llvm::Twine& instName = "") override final;

    // -----------------------------------------------------------------------------------------------------------------
    // Subgroup operations

    llvm::Value* CreateGetSubgroupSize(const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupElect(const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupAll(llvm::Value* const pValue,
                             bool         wqm,
                             const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupAny(llvm::Value* const pValue,
                             bool         wqm,
                             const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupAllEqual(llvm::Value* const pValue,
                                  bool         wqm,
                                  const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBroadcast(llvm::Value* const pValue,
                                   llvm::Value* const pIndex,
                                   const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBroadcastFirst(llvm::Value* const pValue,
                                        const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallot(llvm::Value* const pValue,
                                const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupInverseBallot(llvm::Value* const pValue,
                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotBitExtract(llvm::Value* const pValue,
                                          llvm::Value* const pIndex,
                                          const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotBitCount(llvm::Value* const pValue,
                                        const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotInclusiveBitCount(llvm::Value* const pValue,
                                                 const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotExclusiveBitCount(llvm::Value* const pValue,
                                                 const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotFindLsb(llvm::Value* const pValue,
                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupBallotFindMsb(llvm::Value* const pValue,
                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffle(llvm::Value* const pValue,
                                 llvm::Value* const pIndex,
                                 const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffleXor(llvm::Value* const pValue,
                                    llvm::Value* const pMask,
                                    const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffleUp(llvm::Value* const pValue,
                                   llvm::Value* const pDelta,
                                   const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupShuffleDown(llvm::Value* const pValue,
                                     llvm::Value* const pDelta,
                                     const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupClusteredReduction(GroupArithOp groupArithOp,
                                            llvm::Value* const pValue,
                                            llvm::Value* const pClusterSize,
                                            const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp,
                                            llvm::Value* const pValue,
                                            llvm::Value* const pClusterSize,
                                            const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp,
                                            llvm::Value* const pValue,
                                            llvm::Value* const pClusterSize,
                                            const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadBroadcast(llvm::Value* const pValue,
                                       llvm::Value* const pIndex,
                                       const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadSwapHorizontal(llvm::Value* const pValue,
                                            const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadSwapVertical(llvm::Value* const pValue,
                                          const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupQuadSwapDiagonal(llvm::Value* const pValue,
                                          const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupSwizzleQuad(llvm::Value* const pValue,
                                     llvm::Value* const pOffset,
                                     const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupSwizzleMask(llvm::Value* const pValue,
                                     llvm::Value* const pMask,
                                     const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupWriteInvocation(llvm::Value* const pInputValue,
                                         llvm::Value* const pWriteValue,
                                         llvm::Value* const pIndex,
                                         const llvm::Twine& instName) override final;
    llvm::Value* CreateSubgroupMbcnt(llvm::Value* const pMask,
                               const llvm::Twine& instName ) override final;

protected:
    // Get the ShaderModes object.
    ShaderModes* GetShaderModes() override final;

private:
    BuilderRecorder() = delete;
    BuilderRecorder(const BuilderRecorder&) = delete;
    BuilderRecorder& operator=(const BuilderRecorder&) = delete;

    BuilderRecorder(BuilderContext* pBuilderContext, Pipeline* pPipeline);

    // Record one Builder call
    llvm::Instruction* Record(Opcode                        opcode,
                        llvm::Type*                         pReturnTy,
                        llvm::ArrayRef<llvm::Value*>              args,
                        const llvm::Twine&                  instName,
                        llvm::ArrayRef<llvm::Attribute::AttrKind> attribs = {});

    // -----------------------------------------------------------------------------------------------------------------

    PipelineState*                m_pPipelineState;           // PipelineState; nullptr for shader compile
    std::unique_ptr<ShaderModes>  m_shaderModes;              // ShaderModes for a shader compile
};

// Create BuilderReplayer pass
llvm::ModulePass* CreateBuilderReplayer(Pipeline* pPipeline);

} // lgc
