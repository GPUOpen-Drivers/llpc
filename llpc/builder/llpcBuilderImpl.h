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
 * @file  llpcBuilderImpl.h
 * @brief LLPC header file: declaration of Llpc::Builder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"
#include "llpcPipelineState.h"

namespace Llpc
{

using namespace llvm;

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : public Builder
{
public:
    BuilderImplBase(BuilderContext* pBuilderContext) : Builder(pBuilderContext) {}

    // Create scalar from dot product of vector
    Value* CreateDotProduct(Value* const pVector1,
                            Value* const pVector2,
                            const Twine& instName = "") override final;

protected:
    // Get the ShaderModes object.
    ShaderModes* GetShaderModes() override final;

    // Get the PipelineState object.
    PipelineState* GetPipelineState() const { return m_pPipelineState; }

    // Get whether the context we are building in supports DPP operations.
    bool SupportDpp() const;

    // Get whether the context we are building in support the bpermute operation.
    bool SupportBPermute() const;

    // Get whether the context we are building in supports permute lane DPP operations.
    bool SupportPermLaneDpp() const;

    // Create an "if..endif" or "if..else..endif" structure.
    BranchInst* CreateIf(Value* pCondition, bool wantElse, const Twine& instName);

    // Create a waterfall loop containing the specified instruction.
    Instruction* CreateWaterfallLoop(Instruction*       pNonUniformInst,
                                     ArrayRef<uint32_t> operandIdxs,
                                     const Twine&       instName = "");

    // Helper method to scalarize a possibly vector unary operation
    Value* Scalarize(Value* pValue, std::function<Value*(Value*)> callback);

    // Helper method to scalarize in pairs a possibly vector unary operation.
    Value* ScalarizeInPairs(Value*                        pValue,
                            std::function<Value*(Value*)> callback);

    // Helper method to scalarize a possibly vector binary operation
    Value* Scalarize(Value*                                 pValue0,
                     Value*                                 pValue1,
                     std::function<Value*(Value*, Value*)>  callback);

    // Helper method to scalarize a possibly vector trinary operation
    Value* Scalarize(Value*                                        pValue0,
                     Value*                                        pValue1,
                     Value*                                        pValue2,
                     std::function<Value*(Value*, Value*, Value*)> callback);

    PipelineState*  m_pPipelineState = nullptr;   // Pipeline state

private:
    BuilderImplBase() = delete;
    BuilderImplBase(const BuilderImplBase&) = delete;
    BuilderImplBase& operator=(const BuilderImplBase&) = delete;
};

// =====================================================================================================================
// Builder implementation subclass for arithmetic operations
class BuilderImplArith : virtual public BuilderImplBase
{
public:
    BuilderImplArith(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
    // the given cube map texture coordinates.
    Value* CreateCubeFaceCoord(Value* pCoord, const Twine& instName = "") override final;

    // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
    // the given cube map texture coordinates.
    Value* CreateCubeFaceIndex(Value* pCoord, const Twine& instName = "") override final;

    // Create scalar or vector FP truncate operation with rounding mode.
    Value* CreateFpTruncWithRounding(Value*                                pValue,
                                     Type*                                 pDestTy,
                                     unsigned                              roundingMode,
                                     const Twine&                          instName = "") override final;

    // Create quantize operation.
    Value* CreateQuantizeToFp16(Value* pValue, const Twine& instName = "") override final;

    // Create signed integer or FP modulo operation.
    Value* CreateSMod(Value* pDividend, Value* pDivisor, const Twine& instName = "") override final;
    Value* CreateFMod(Value* pDividend, Value* pDivisor, const Twine& instName = "") override final;

    // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
    Value* CreateFma(Value* pA, Value* pB, Value* pC, const Twine& instName = "") override final;

    // Methods to create trig and exponential operations.
    Value* CreateTan(Value* pX, const Twine& instName = "") override final;
    Value* CreateASin(Value* pX, const Twine& instName = "") override final;
    Value* CreateACos(Value* pX, const Twine& instName = "") override final;
    Value* CreateATan(Value* pYOverX, const Twine& instName = "") override final;
    Value* CreateATan2(Value* pY, Value* pX, const Twine& instName = "") override final;
    Value* CreateSinh(Value* pX, const Twine& instName = "") override final;
    Value* CreateCosh(Value* pX, const Twine& instName = "") override final;
    Value* CreateTanh(Value* pX, const Twine& instName = "") override final;
    Value* CreateASinh(Value* pX, const Twine& instName = "") override final;
    Value* CreateACosh(Value* pX, const Twine& instName = "") override final;
    Value* CreateATanh(Value* pX, const Twine& instName = "") override final;
    Value* CreatePower(Value* pX, Value* pY, const Twine& instName = "") override final;
    Value* CreateExp(Value* pX, const Twine& instName = "") override final;
    Value* CreateLog(Value* pX, const Twine& instName = "") override final;
    Value* CreateInverseSqrt(Value* pX, const Twine& instName = "") override final;

    // General arithmetic operations.
    Value* CreateSAbs(Value* pX, const Twine& instName = "") override final;
    Value* CreateFSign(Value* pX, const Twine& instName = "") override final;
    Value* CreateSSign(Value* pX, const Twine& instName = "") override final;
    Value* CreateFract(Value* pX, const Twine& instName = "") override final;
    Value* CreateSmoothStep(Value* pEdge0, Value* pEdge1, Value* pX, const Twine& instName = "") override final;
    Value* CreateLdexp(Value* pX, Value* pExp, const Twine& instName = "") override final;
    Value* CreateExtractSignificand(Value* pValue, const Twine& instName = "") override final;
    Value* CreateExtractExponent(Value* pValue, const Twine& instName = "") override final;
    Value* CreateCrossProduct(Value* pX, Value* pY, const Twine& instName = "") override final;
    Value* CreateNormalizeVector(Value* pX, const Twine& instName = "") override final;
    Value* CreateFaceForward(Value* pN, Value* pI, Value* pNref, const Twine& instName = "") override final;
    Value* CreateReflect(Value* pI, Value* pN, const Twine& instName = "") override final;
    Value* CreateRefract(Value* pI, Value* pN, Value* pEta, const Twine& instName = "") override final;

    // Create "fclamp" operation.
    Value* CreateFClamp(Value* pX, Value* pMinVal, Value* pMaxVal, const Twine& instName = "") override final;

    // FP min/max
    Value* CreateFMin(Value* pValue1, Value* pValue2, const Twine& instName = "") override final;
    Value* CreateFMax(Value* pValue1, Value* pValue2, const Twine& instName = "") override final;

    // Methods for trinary min/max/mid.
    Value* CreateFMin3(Value* pValue1, Value* pValue2, Value* pValue3, const Twine& instName = "") override final;
    Value* CreateFMax3(Value* pValue1, Value* pValue2, Value* pValue3, const Twine& instName = "") override final;
    Value* CreateFMid3(Value* pValue1, Value* pValue2, Value* pValue3, const Twine& instName = "") override final;

    // Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
    Value* CreateIsInf(Value* pX, const Twine& instName = "") override final;

    // Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
    Value* CreateIsNaN(Value* pX, const Twine& instName = "") override final;

    // Create an "insert bitfield" operation for a (vector of) integer type.
    Value* CreateInsertBitField(Value*        pBase,
                                Value*        pInsert,
                                Value*        pOffset,
                                Value*        pCount,
                                const Twine&  instName = "") override final;

    // Create an "extract bitfield " operation for a (vector of) i32.
    Value* CreateExtractBitField(Value*        pBase,
                                 Value*        pOffset,
                                 Value*        pCount,
                                 bool          isSigned,
                                 const Twine&  instName = "") override final;

    // Create "find MSB" operation for a (vector of) signed int.
    Value* CreateFindSMsb(Value* pValue, const Twine& instName = "") override final;

    // Create "fmix" operation.
    Value* CreateFMix(Value* pX, Value* pY, Value* pA, const Twine& instName = "") override final;

private:
    BuilderImplArith() = delete;
    BuilderImplArith(const BuilderImplArith&) = delete;
    BuilderImplArith& operator=(const BuilderImplArith&) = delete;

    // Common code for asin and acos
    Value* ASinACosCommon(Value* pX, Constant* pCoefP0, Constant* pCoefP1);

    // Generate FP division, using fast fdiv for float to bypass optimization.
    Value* FDivFast(Value* pNumerator, Value* pDenominator);

    // Helper method to create call to llvm.amdgcn.class, scalarizing if necessary. This is not exposed outside of
    // BuilderImplArith.
    Value* CreateCallAmdgcnClass(Value* pValue, uint32_t flags, const Twine& instName = "");

    // Methods to get various FP constants as scalar or vector. Any needed directly by a client should be moved
    // to llpcBuilder.h. Using these (rather than just using for example
    // ConstantFP::get(.., M_PI)) ensures that we always get the same value, independent of the
    // host platform and its compiler.
    // TODO: Use values that are suitable for doubles.

    // Get PI = 3.14159274 scalar or vector
    Constant* GetPi(Type* pTy)
    {
        return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x400921FB60000000)));
    }

    // Get PI/2 = 1.57079637 scalar or vector
    Constant* GetPiByTwo(Type* pTy)
    {
        return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FF921FB60000000)));
    }

    // Get PI/4 - 1 = -0.21460181 scalar or vector
    Constant* GetPiByFourMinusOne(Type* pTy)
    {
        return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFCB781280000000)));
    }

    // Get 1/log(2) = 1.442695 scalar or vector
    Constant* GetRecipLog2(Type* pTy)
    {
        return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FF7154760000000)));
    }

    // Get 0.5 * log(2) = 0.34657359 scalar or vector
    Constant* GetHalfLog2(Type* pTy)
    {
        return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FD62E4300000000)));
    }

    // Get log(2) = 0.6931471824646 scalar or vector
    Constant* GetLog2(Type* pTy)
    {
        return GetFpConstant(pTy, APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FE62E4300000000)));
    }

    // Get 2^-15 (normalized float16 minimum) scalar or vector
    Constant* GetMinNormalizedF16(Type* pTy)
    {
        return ConstantFP::get(pTy, 0.000030517578125);
    }

    // Ensure result is canonicalized if the shader's FP mode is flush denorms.
    Value* Canonicalize(Value* pValue);
};

// =====================================================================================================================
// Builder implementation subclass for descriptors
class BuilderImplDesc : virtual public BuilderImplBase
{
public:
    BuilderImplDesc(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create a load of a buffer descriptor.
    Value* CreateLoadBufferDesc(uint32_t      descSet,
                                uint32_t      binding,
                                Value*        pDescIndex,
                                bool          isNonUniform,
                                bool          isWritten,
                                Type*         pPointeeTy,
                                const Twine&  instName) override final;

    // Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
    Value* CreateIndexDescPtr(Value*        pDescPtr,
                              Value*        pIndex,
                              bool          isNonUniform,
                              const Twine&  instName) override final;

    // Load image/sampler/texelbuffer/F-mask descriptor from pointer.
    Value* CreateLoadDescFromPtr(Value*        pDescPtr,
                                 const Twine&  instName) override final;

    // Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
    Value* CreateGetSamplerDescPtr(uint32_t      descSet,
                                   uint32_t      binding,
                                   const Twine&  instName) override final;

    // Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
    Value* CreateGetImageDescPtr(uint32_t      descSet,
                                 uint32_t      binding,
                                 const Twine&  instName) override final;

    // Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
    Value* CreateGetTexelBufferDescPtr(uint32_t      descSet,
                                       uint32_t      binding,
                                       const Twine&  instName) override final;

    // Create a pointer to F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
    Value* CreateGetFmaskDescPtr(uint32_t      descSet,
                                 uint32_t      binding,
                                 const Twine&  instName) override final;

    // Create a load of the push constants pointer.
    Value* CreateLoadPushConstantsPtr(Type*         pPushConstantsTy,
                                      const Twine&  instName) override final;

    // Create a buffer length query based on the specified descriptor.
    Value* CreateGetBufferDescLength(Value* const pBufferDesc,
                                     const Twine& instName = "") override final;

private:
    BuilderImplDesc() = delete;
    BuilderImplDesc(const BuilderImplDesc&) = delete;
    BuilderImplDesc& operator=(const BuilderImplDesc&) = delete;

    Value* ScalarizeIfUniform(Value* pValue, bool isNonUniform);
};

// =====================================================================================================================
// Builder implementation subclass for image operations
class BuilderImplImage : virtual public BuilderImplBase
{
public:
    BuilderImplImage(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create an image load.
    Value* CreateImageLoad(Type*             pResultTy,
                           uint32_t          dim,
                           uint32_t          flags,
                           Value*            pImageDesc,
                           Value*            pCoord,
                           Value*            pMipLevel,
                           const Twine&      instName = "") override final;

    // Create an image load with F-mask.
    Value* CreateImageLoadWithFmask(Type*             pResultTy,
                                    uint32_t          dim,
                                    uint32_t          flags,
                                    Value*            pImageDesc,
                                    Value*            pFmaskDesc,
                                    Value*            pCoord,
                                    Value*            pSampleNum,
                                    const Twine&      instName = "") override final;

    // Create an image store.
    Value* CreateImageStore(Value*           pTexel,
                            uint32_t         dim,
                            uint32_t         flags,
                            Value*           pImageDesc,
                            Value*           pCoord,
                            Value*           pMipLevel,
                            const Twine&     instName = "") override final;

    // Create an image sample.
    Value* CreateImageSample(Type*             pResultTy,
                             uint32_t          dim,
                             uint32_t          flags,
                             Value*            pImageDesc,
                             Value*            pSamplerDesc,
                             ArrayRef<Value*>  address,
                             const Twine&      instName = "") override final;

    // Create an image sample with conversion.
    // This is not yet a Builder API, but it could become one if there was to be a new SPIR-V YCbCr
    // converting sampler spec that allows the SPIR-V reader to tell that it has a converting sampler.
    Value* CreateImageSampleConvert(Type*             pResultTy,
                                    uint32_t          dim,
                                    uint32_t          flags,
                                    Value*            pImageDesc,
                                    Value*            pConvertingSamplerDesc,
                                    ArrayRef<Value*>  address,
                                    const Twine&      instName = "");

    // Create an image gather
    Value* CreateImageGather(Type*             pResultTy,
                             uint32_t          dim,
                             uint32_t          flags,
                             Value*            pImageDesc,
                             Value*            pSamplerDesc,
                             ArrayRef<Value*>  address,
                             const Twine&      instName = "") override final;

    // Create an image atomic operation other than compare-and-swap.
    Value* CreateImageAtomic(uint32_t         atomicOp,
                             uint32_t         dim,
                             uint32_t         flags,
                             AtomicOrdering   ordering,
                             Value*           pImageDesc,
                             Value*           pCoord,
                             Value*           pInputValue,
                             const Twine&     instName = "") override final;

    // Create an image atomic compare-and-swap.
    Value* CreateImageAtomicCompareSwap(uint32_t        dim,
                                        uint32_t        flags,
                                        AtomicOrdering  ordering,
                                        Value*          pImageDesc,
                                        Value*          pCoord,
                                        Value*          pInputValue,
                                        Value*          pComparatorValue,
                                        const Twine&    instName = "") override final;

    // Create a query of the number of mipmap levels in an image. Returns an i32 value.
    Value* CreateImageQueryLevels(uint32_t                dim,
                                  uint32_t                flags,
                                  Value*                  pImageDesc,
                                  const Twine&            instName = "") override final;

    // Create a query of the number of samples in an image. Returns an i32 value.
    Value* CreateImageQuerySamples(uint32_t                dim,
                                   uint32_t                flags,
                                   Value*                  pImageDesc,
                                   const Twine&            instName = "") override final;

    // Create a query of size of an image at the specified LOD
    Value* CreateImageQuerySize(uint32_t          dim,
                                uint32_t          flags,
                                Value*            pImageDesc,
                                Value*            pLod,
                                const Twine&      instName = "") override final;

    // Create a get of the LOD that would be used for an image sample with the given coordinates
    // and implicit LOD.
    Value* CreateImageGetLod(uint32_t          dim,
                             uint32_t          flags,
                             Value*            pImageDesc,
                             Value*            pSamplerDesc,
                             Value*            pCoord,
                             const Twine&      instName = "") override final;

private:
    BuilderImplImage() = delete;
    BuilderImplImage(const BuilderImplImage&) = delete;
    BuilderImplImage& operator=(const BuilderImplImage&) = delete;

    // Implement pre-GFX9 integer gather workaround to patch descriptor or coordinate before the gather
    Value* PreprocessIntegerImageGather(uint32_t dim, Value*& pImageDesc, Value*& pCoord);

    // Implement pre-GFX9 integer gather workaround to modify result.
    Value* PostprocessIntegerImageGather(Value*   pNeedDescPatch,
                                         uint32_t flags,
                                         Value*   pImageDesc,
                                         Type*    pTexelTy,
                                         Value*   pResult);

    // Common code to create an image sample or gather.
    Value* CreateImageSampleGather(Type*            pResultTy,
                                   uint32_t         dim,
                                   uint32_t         flags,
                                   Value*           pCoord,
                                   Value*           pImageDesc,
                                   Value*           pSamplerDesc,
                                   ArrayRef<Value*> address,
                                   const Twine&     instName,
                                   bool             isSample);

    // Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
    Value* CreateImageAtomicCommon(uint32_t          atomicOp,
                                   uint32_t          dim,
                                   uint32_t          flags,
                                   AtomicOrdering    ordering,
                                   Value*            pImageDesc,
                                   Value*            pCoord,
                                   Value*            pInputValue,
                                   Value*            pComparatorValue,
                                   const Twine&      instName);

    // Change 1D or 1DArray dimension to 2D or 2DArray if needed as a workaround on GFX9+
    uint32_t Change1DTo2DIfNeeded(uint32_t dim);

    // Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
    // modifying if necessary.
    // Returns possibly modified image dimension.
    uint32_t PrepareCoordinate(uint32_t                  dim,
                               Value*                    pCoord,
                               Value*                    pProjective,
                               Value*                    pDerivativeX,
                               Value*                    pDerivativeY,
                               SmallVectorImpl<Value*>&  outCoords,
                               SmallVectorImpl<Value*>&  outDerivatives);

    // For a cubearray with integer coordinates, combine the face and slice into a single component.
    void CombineCubeArrayFaceAndSlice(Value* pCoord, SmallVectorImpl<Value*>& coords);

    // Patch descriptor with cube dimension for image call
    Value* PatchCubeDescriptor(Value* pDesc, uint32_t dim);

    // Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
    Value* HandleFragCoordViewIndex(Value* pCoord, uint32_t flags, uint32_t& dim);

    // -----------------------------------------------------------------------------------------------------------------

    enum ImgDataFormat
    {
        IMG_DATA_FORMAT_32          = 4,
        IMG_DATA_FORMAT_32_32       = 11,
        IMG_DATA_FORMAT_32_32_32_32 = 14,
    };

    static const uint32_t AtomicOpCompareSwap = 1;
};

// =====================================================================================================================
// Builder implementation subclass for input/output operations
class BuilderImplInOut : virtual public BuilderImplBase
{
public:
    BuilderImplInOut(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create a read of (part of) a user input value.
    Value* CreateReadGenericInput(Type*         pResultTy,
                                  uint32_t      location,
                                  Value*        pLocationOffset,
                                  Value*        pElemIdx,
                                  uint32_t      locationCount,
                                  InOutInfo     inputInfo,
                                  Value*        pVertexIndex,
                                  const Twine&  instName = "") override final;

    // Create a read of (part of) a user output value.
    Value* CreateReadGenericOutput(Type*         pResultTy,
                                   uint32_t      location,
                                   Value*        pLocationOffset,
                                   Value*        pElemIdx,
                                   uint32_t      locationCount,
                                   InOutInfo     outputInfo,
                                   Value*        pVertexIndex,
                                   const Twine&  instName = "") override final;

    // Create a write of (part of) a user output value.
    Instruction* CreateWriteGenericOutput(Value*        pValueToWrite,
                                          uint32_t      location,
                                          Value*        pLocationOffset,
                                          Value*        pElemIdx,
                                          uint32_t      locationCount,
                                          InOutInfo     outputInfo,
                                          Value*        pVertexIndex) override final;

    // Create a write to an XFB (transform feedback / streamout) buffer.
    Instruction* CreateWriteXfbOutput(Value*        pValueToWrite,
                                      bool          isBuiltIn,
                                      uint32_t      location,
                                      uint32_t      xfbBuffer,
                                      uint32_t      xfbStride,
                                      Value*        pXfbOffset,
                                      InOutInfo     outputInfo) override final;

    // Create a read of (part of) a built-in input value.
    Value* CreateReadBuiltInInput(BuiltInKind  builtIn,
                                  InOutInfo    inputInfo,
                                  Value*       pVertexIndex,
                                  Value*       pIndex,
                                  const Twine& instName = "") override final;

    // Create a read of (part of) an output built-in value.
    Value* CreateReadBuiltInOutput(BuiltInKind  builtIn,
                                   InOutInfo    outputInfo,
                                   Value*       pVertexIndex,
                                   Value*       pIndex,
                                   const Twine& instName = "") override final;

    // Create a write of (part of) a built-in output value.
    Instruction* CreateWriteBuiltInOutput(Value*        pValueToWrite,
                                          BuiltInKind   builtIn,
                                          InOutInfo     outputInfo,
                                          Value*        pVertexIndex,
                                          Value*        pIndex) override final;

    // Get name of built-in
    static StringRef GetBuiltInName(BuiltInKind builtIn);

private:
    BuilderImplInOut() = delete;
    BuilderImplInOut(const BuilderImplInOut&) = delete;
    BuilderImplInOut& operator=(const BuilderImplInOut&) = delete;

    // Read (a part of) a generic (user) input/output value.
    Value* ReadGenericInputOutput(bool          isOutput,
                                  Type*         pResultTy,
                                  uint32_t      location,
                                  Value*        pLocationOffset,
                                  Value*        pElemIdx,
                                  uint32_t      locationCount,
                                  InOutInfo     inOutInfo,
                                  Value*        pVertexIndex,
                                  const Twine&  instName);

    // Mark usage for a generic (user) input or output
    void MarkGenericInputOutputUsage(bool          isOutput,
                                     uint32_t      location,
                                     uint32_t      locationCount,
                                     InOutInfo     inOutInfo,
                                     Value*        pVertexIndex);

    // Mark interpolation info for FS input.
    void MarkInterpolationInfo(InOutInfo interpInfo);

    // Mark fragment output type
    void MarkFsOutputType(Type* pOutputTy, uint32_t location, InOutInfo outputInfo);

    // Modify aux interp value according to custom interp mode, and its helper functions.
    Value* ModifyAuxInterpValue(Value* pAuxInterpValue, InOutInfo inputInfo);
    Value* EvalIJOffsetNoPersp(Value* pOffset);
    Value* EvalIJOffsetSmooth(Value* pOffset);
    Value* AdjustIJ(Value* pValue, Value* pOffset);

    // Read (part of) a built-in value
    Value* ReadBuiltIn(bool         isOutput,
                       BuiltInKind  builtIn,
                       InOutInfo    inOutInfo,
                       Value*       pVertexIndex,
                       Value*       pIndex,
                       const Twine& instName);

    // Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
    Type* GetBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo);

    // Mark usage of a built-in input
    void MarkBuiltInInputUsage(BuiltInKind builtIn, uint32_t arraySize);

    // Mark usage of a built-in output
    void MarkBuiltInOutputUsage(BuiltInKind builtIn, uint32_t arraySize, uint32_t streamId);

#ifndef NDEBUG
    // Get a bitmask of which shader stages are valid for a built-in to be an input or output of
    uint32_t GetBuiltInValidMask(BuiltInKind builtIn, bool isOutput);

    // Determine whether a built-in is an input for a particular shader stage.
    bool IsBuiltInInput(BuiltInKind builtIn);

    // Determine whether a built-in is an output for a particular shader stage.
    bool IsBuiltInOutput(BuiltInKind builtIn);
#endif // NDEBUG
};

// =====================================================================================================================
// Builder implementation subclass for matrix operations
class BuilderImplMatrix : virtual public BuilderImplBase
{
public:
    BuilderImplMatrix(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create a matrix transpose.
    Value* CreateTransposeMatrix(Value* const pMatrix,
                                 const Twine& instName = "") override final;

    // Create matrix multiplication: matrix times scalar, resulting in matrix
    Value* CreateMatrixTimesScalar(Value* const pMatrix,
                                   Value* const pScalar,
                                   const Twine& instName = "") override final;

    // Create matrix multiplication: vector times matrix, resulting in vector
    Value* CreateVectorTimesMatrix(Value* const pVector,
                                   Value* const pMatrix,
                                   const Twine& instName = "") override final;

    // Create matrix multiplication: matrix times vector, resulting in vector
    Value* CreateMatrixTimesVector(Value* const pMatrix,
                                   Value* const pVector,
                                   const Twine& instName = "") override final;

    // Create matrix multiplication:  matrix times matrix, resulting in matrix
    Value* CreateMatrixTimesMatrix(Value* const pMatrix1,
                                   Value* const pMatrix2,
                                   const Twine& instName = "") override final;

    // Create vector outer product operation, resulting in matrix
    Value* CreateOuterProduct(Value* const pVector1,
                              Value* const pVector2,
                              const Twine& instName = "") override final;

    // Create matrix determinant operation.
    Value* CreateDeterminant(Value* const pMatrix, const Twine& instName = "") override final;

    // Create matrix inverse operation.
    Value* CreateMatrixInverse(Value* const pMatrix, const Twine& instName = "") override final;

private:
    BuilderImplMatrix() = delete;
    BuilderImplMatrix(const BuilderImplMatrix&) = delete;
    BuilderImplMatrix& operator=(const BuilderImplMatrix&) = delete;

    // Helper function for determinant calculation
    Value* Determinant(ArrayRef<Value*> elements, uint32_t order);

    // Get submatrix by deleting specified row and column
    void GetSubmatrix(ArrayRef<Value*>        matrix,
                      MutableArrayRef<Value*> submatrix,
                      uint32_t                order,
                      uint32_t                rowToDelete,
                      uint32_t                columnToDelete);
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class BuilderImplMisc : virtual public BuilderImplBase
{
public:
    BuilderImplMisc(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
    // the current output primitive in the specified output-primitive stream.
    Instruction* CreateEmitVertex(uint32_t streamId) override final;

    // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
    Instruction* CreateEndPrimitive(uint32_t streamId) override final;

    // Create a workgroup control barrier.
    Instruction* CreateBarrier() override final;

    // Create a "kill". Only allowed in a fragment shader.
    Instruction* CreateKill(const Twine& instName) override final;

    // Create a "readclock".
    Instruction* CreateReadClock(bool realtime, const Twine& instName) override final;

    // Create derivative calculation on float or vector of float or half
    Value* CreateDerivative(Value* pValue, bool isDirectionY, bool isFine, const Twine& instName = "") override final;

    // Create a demote to helper invocation operation. Only allowed in a fragment shader.
    Instruction* CreateDemoteToHelperInvocation(const Twine& instName) override final;

    // Create a helper invocation query. Only allowed in a fragment shader.
    Value* CreateIsHelperInvocation(const Twine& instName) override final;

private:
    BuilderImplMisc() = delete;
    BuilderImplMisc(const BuilderImplMisc&) = delete;
    BuilderImplMisc& operator=(const BuilderImplMisc&) = delete;
};

// =====================================================================================================================
// Builder implementation subclass for subgroup operations
class BuilderImplSubgroup : virtual public BuilderImplBase
{
public:
    BuilderImplSubgroup(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create a get subgroup size query.
    Value* CreateGetSubgroupSize(const Twine& instName) override final;

    // Create a subgroup elect.
    Value* CreateSubgroupElect(const Twine& instName) override final;

    // Create a subgroup all.
    Value* CreateSubgroupAll(Value* const pValue,
                             bool         wqm,
                             const Twine& instName) override final;

    // Create a subgroup any
    Value* CreateSubgroupAny(Value* const pValue,
                             bool         wqm,
                             const Twine& instName) override final;

    // Create a subgroup all equal.
    Value* CreateSubgroupAllEqual(Value* const pValue,
                                  bool         wqm,
                                  const Twine& instName) override final;

    // Create a subgroup broadcast.
    Value* CreateSubgroupBroadcast(Value* const pValue,
                                   Value* const pIndex,
                                   const Twine& instName) override final;

    // Create a subgroup broadcast first.
    Value* CreateSubgroupBroadcastFirst(Value* const pValue,
                                        const Twine& instName) override final;

    // Create a subgroup ballot.
    Value* CreateSubgroupBallot(Value* const pValue,
                                const Twine& instName) override final;

    // Create a subgroup inverse ballot.
    Value* CreateSubgroupInverseBallot(Value* const pValue,
                                       const Twine& instName) override final;

    // Create a subgroup ballot bit extract.
    Value* CreateSubgroupBallotBitExtract(Value* const pValue,
                                          Value* const pIndex,
                                          const Twine& instName) override final;

    // Create a subgroup ballot bit count.
    Value* CreateSubgroupBallotBitCount(Value* const pValue,
                                        const Twine& instName) override final;

    // Create a subgroup ballot inclusive bit count.
    Value* CreateSubgroupBallotInclusiveBitCount(Value* const pValue,
                                                 const Twine& instName) override final;

    // Create a subgroup ballot exclusive bit count.
    Value* CreateSubgroupBallotExclusiveBitCount(Value* const pValue,
                                                 const Twine& instName) override final;

    // Create a subgroup ballot find least significant bit.
    Value* CreateSubgroupBallotFindLsb(Value* const pValue,
                                       const Twine& instName) override final;

    // Create a subgroup ballot find most significant bit.
    Value* CreateSubgroupBallotFindMsb(Value* const pValue,
                                       const Twine& instName) override final;

    // Create a subgroup shuffle.
    Value* CreateSubgroupShuffle(Value* const pValue,
                                 Value* const pIndex,
                                 const Twine& instName) override final;

    // Create a subgroup shuffle xor.
    Value* CreateSubgroupShuffleXor(Value* const pValue,
                                    Value* const pMask,
                                    const Twine& instName) override final;

    // Create a subgroup shuffle up.
    Value* CreateSubgroupShuffleUp(Value* const pValue,
                                   Value* const pDelta,
                                   const Twine& instName) override final;

    // Create a subgroup shuffle down.
    Value* CreateSubgroupShuffleDown(Value* const pValue,
                                     Value* const pDelta,
                                     const Twine& instName) override final;

    // Create a subgroup clustered reduction.
    Value* CreateSubgroupClusteredReduction(GroupArithOp groupArithOp,
                                            Value* const pValue,
                                            Value* const pClusterSize,
                                            const Twine& instName) override final;

    // Create a subgroup clustered inclusive scan.
    Value* CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp,
                                            Value* const pValue,
                                            Value* const pClusterSize,
                                            const Twine& instName) override final;

    // Create a subgroup clustered exclusive scan.
    Value* CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp,
                                            Value* const pValue,
                                            Value* const pClusterSize,
                                            const Twine& instName) override final;

    // Create a subgroup quad broadcast.
    Value* CreateSubgroupQuadBroadcast(Value* const pValue,
                                       Value* const pIndex,
                                       const Twine& instName) override final;

    // Create a subgroup quad swap horizontal.
    Value* CreateSubgroupQuadSwapHorizontal(Value* const pValue,
                                            const Twine& instName) override final;

    // Create a subgroup quad swap vertical.
    Value* CreateSubgroupQuadSwapVertical(Value* const pValue,
                                          const Twine& instName) override final;

    // Create a subgroup quad swap diagonal.
    Value* CreateSubgroupQuadSwapDiagonal(Value* const pValue,
                                          const Twine& instName) override final;

    // Create a subgroup swizzle quad.
    Value* CreateSubgroupSwizzleQuad(Value* const pValue,
                                     Value* const pOffset,
                                     const Twine& instName) override final;

    // Create a subgroup swizzle masked.
    Value* CreateSubgroupSwizzleMask(Value* const pValue,
                                     Value* const pMask,
                                     const Twine& instName) override final;

    // Create a subgroup write invocation.
    Value* CreateSubgroupWriteInvocation(Value* const pInputValue,
                                         Value* const pWriteValue,
                                         Value* const pIndex,
                                         const Twine& instName) override final;

    // Create a subgroup mbcnt.
    Value* CreateSubgroupMbcnt(Value* const pMask,
                               const Twine& instName) override final;

private:
    BuilderImplSubgroup() = delete;
    BuilderImplSubgroup(const BuilderImplSubgroup&) = delete;
    BuilderImplSubgroup& operator=(const BuilderImplSubgroup&) = delete;

    enum class DppCtrl : uint32_t
    {
        DppQuadPerm0000   = 0x000,
        DppQuadPerm1111   = 0x055,
        DppQuadPerm2222   = 0x0AA,
        DppQuadPerm3333   = 0x0FF,
        DppQuadPerm1032   = 0x0B1,
        DppQuadPerm2301   = 0x04E,
        DppQuadPerm0123   = 0x01B,
        DppRowSr1         = 0x111,
        DppRowSr2         = 0x112,
        DppRowSr3         = 0x113,
        DppRowSr4         = 0x114,
        DppRowSr8         = 0x118,
        DppWfSl1          = 0x130,
        DppWfSr1          = 0x138,
        DppRowMirror      = 0x140,
        DppRowHalfMirror  = 0x141,
        DppRowBcast15     = 0x142,
        DppRowBcast31     = 0x143,
    };

    uint32_t GetShaderSubgroupSize();
    Value* CreateGroupArithmeticIdentity(GroupArithOp   groupArithOp,
                                         Type* const    pType);
    Value* CreateGroupArithmeticOperation(GroupArithOp groupArithOp,
                                          Value* const pX,
                                          Value* const pY);
    Value* CreateInlineAsmSideEffect(Value* const pValue);
    Value* CreateDppMov(Value* const pValue,
                        DppCtrl      dppCtrl,
                        uint32_t     rowMask,
                        uint32_t     bankMask,
                        bool         boundCtrl);
    Value* CreateDppUpdate(Value* const pOrigValue,
                           Value* const pUpdateValue,
                           DppCtrl      dppCtrl,
                           uint32_t     rowMask,
                           uint32_t     bankMask,
                           bool         boundCtrl);

    Value* CreatePermLane16(Value* const pOrigValue,
                            Value* const pUpdateValue,
                            uint32_t     selectBitsLow,
                            uint32_t     selectBitsHigh,
                            bool         fetchInactive,
                            bool         boundCtrl);
    Value* CreatePermLaneX16(Value* const pOrigValue,
                             Value* const pUpdateValue,
                             uint32_t     selectBitsLow,
                             uint32_t     selectBitsHigh,
                             bool         fetchInactive,
                             bool         boundCtrl);

    Value* CreateDsSwizzle(Value* const pValue,
                           uint16_t     dsPattern);
    Value* CreateWwm(Value* const pValue);
    Value* CreateSetInactive(Value* const pActive,
                             Value* const pInactive);
    Value* CreateThreadMask();
    Value* CreateThreadMaskedSelect(
        Value* const pThreadMask,
        uint64_t     andMask,
        Value* const pValue1,
        Value* const pValue2);
    uint16_t GetDsSwizzleBitMode(uint8_t xorMask,
                                 uint8_t orMask,
                                 uint8_t andMask);
    uint16_t GetDsSwizzleQuadMode(uint8_t lane0,
                                  uint8_t lane1,
                                  uint8_t lane2,
                                  uint8_t lane3);
    Value* CreateGroupBallot(Value* const pValue);
};

// =====================================================================================================================
// The Builder implementation, encompassing all the individual builder implementation subclasses
class BuilderImpl final : public BuilderImplArith,
                                 BuilderImplDesc,
                                 BuilderImplImage,
                                 BuilderImplInOut,
                                 BuilderImplMatrix,
                                 BuilderImplMisc,
                                 BuilderImplSubgroup
{
    friend BuilderContext;

public:
    ~BuilderImpl() {}

private:
    BuilderImpl() = delete;
    BuilderImpl(const BuilderImpl&) = delete;
    BuilderImpl& operator=(const BuilderImpl&) = delete;

    BuilderImpl(BuilderContext* pBuilderContext, Pipeline* pPipeline);
};

// Built-ins for fragment input interpolation (I/J)
static const BuiltInKind BuiltInInterpPerspSample     = static_cast<BuiltInKind>(0x10000000);
static const BuiltInKind BuiltInInterpPerspCenter     = static_cast<BuiltInKind>(0x10000001);
static const BuiltInKind BuiltInInterpPerspCentroid   = static_cast<BuiltInKind>(0x10000002);
static const BuiltInKind BuiltInInterpPullMode        = static_cast<BuiltInKind>(0x10000003);
static const BuiltInKind BuiltInInterpLinearSample    = static_cast<BuiltInKind>(0x10000004);
static const BuiltInKind BuiltInInterpLinearCenter    = static_cast<BuiltInKind>(0x10000005);
static const BuiltInKind BuiltInInterpLinearCentroid  = static_cast<BuiltInKind>(0x10000006);

// Built-ins for sample position emulation
static const BuiltInKind BuiltInSamplePosOffset       = static_cast<BuiltInKind>(0x10000007);
static const BuiltInKind BuiltInNumSamples            = static_cast<BuiltInKind>(0x10000008);
static const BuiltInKind BuiltInSamplePatternIdx      = static_cast<BuiltInKind>(0x10000009);
static const BuiltInKind BuiltInWaveId                = static_cast<BuiltInKind>(0x1000000A);

} // Llpc
