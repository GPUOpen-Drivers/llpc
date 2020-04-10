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
 * @brief LLPC header file: declaration of lgc::Builder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/llpcBuilder.h"
#include "llpcPipelineState.h"

namespace lgc
{

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : public Builder
{
public:
    BuilderImplBase(BuilderContext* pBuilderContext) : Builder(pBuilderContext) {}

    // Create scalar from dot product of vector
    llvm::Value* CreateDotProduct(llvm::Value* const pVector1,
                            llvm::Value* const pVector2,
                            const llvm::Twine& instName = "") override final;

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
    llvm::BranchInst* CreateIf(llvm::Value* pCondition, bool wantElse, const llvm::Twine& instName);

    // Create a waterfall loop containing the specified instruction.
    llvm::Instruction* CreateWaterfallLoop(llvm::Instruction*       pNonUniformInst,
                                     llvm::ArrayRef<unsigned> operandIdxs,
                                     const llvm::Twine&       instName = "");

    // Helper method to scalarize a possibly vector unary operation
    llvm::Value* Scalarize(llvm::Value* pValue, std::function<llvm::Value*(llvm::Value*)> callback);

    // Helper method to scalarize in pairs a possibly vector unary operation.
    llvm::Value* ScalarizeInPairs(llvm::Value*                        pValue,
                            std::function<llvm::Value*(llvm::Value*)> callback);

    // Helper method to scalarize a possibly vector binary operation
    llvm::Value* Scalarize(llvm::Value*                                 pValue0,
                     llvm::Value*                                 pValue1,
                     std::function<llvm::Value*(llvm::Value*, llvm::Value*)>  callback);

    // Helper method to scalarize a possibly vector trinary operation
    llvm::Value* Scalarize(llvm::Value*                                        pValue0,
                     llvm::Value*                                        pValue1,
                     llvm::Value*                                        pValue2,
                     std::function<llvm::Value*(llvm::Value*, llvm::Value*, llvm::Value*)> callback);

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
    llvm::Value* CreateCubeFaceCoord(llvm::Value* pCoord, const llvm::Twine& instName = "") override final;

    // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
    // the given cube map texture coordinates.
    llvm::Value* CreateCubeFaceIndex(llvm::Value* pCoord, const llvm::Twine& instName = "") override final;

    // Create scalar or vector FP truncate operation with rounding mode.
    llvm::Value* CreateFpTruncWithRounding(llvm::Value*                                pValue,
                                     llvm::Type*                                 pDestTy,
                                     unsigned                              roundingMode,
                                     const llvm::Twine&                          instName = "") override final;

    // Create quantize operation.
    llvm::Value* CreateQuantizeToFp16(llvm::Value* pValue, const llvm::Twine& instName = "") override final;

    // Create signed integer or FP modulo operation.
    llvm::Value* CreateSMod(llvm::Value* pDividend, llvm::Value* pDivisor, const llvm::Twine& instName = "") override final;
    llvm::Value* CreateFMod(llvm::Value* pDividend, llvm::Value* pDivisor, const llvm::Twine& instName = "") override final;

    // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
    llvm::Value* CreateFma(llvm::Value* pA, llvm::Value* pB, llvm::Value* pC, const llvm::Twine& instName = "") override final;

    // Methods to create trig and exponential operations.
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
    llvm::Value* CreateSmoothStep(llvm::Value* pEdge0, llvm::Value* pEdge1, llvm::Value* pX, const llvm::Twine& instName = "") override final;
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

private:
    BuilderImplArith() = delete;
    BuilderImplArith(const BuilderImplArith&) = delete;
    BuilderImplArith& operator=(const BuilderImplArith&) = delete;

    // Common code for asin and acos
    llvm::Value* ASinACosCommon(llvm::Value* pX, llvm::Constant* pCoefP0, llvm::Constant* pCoefP1);

    // Generate FP division, using fast fdiv for float to bypass optimization.
    llvm::Value* FDivFast(llvm::Value* pNumerator, llvm::Value* pDenominator);

    // Helper method to create call to llvm.amdgcn.class, scalarizing if necessary. This is not exposed outside of
    // BuilderImplArith.
    llvm::Value* CreateCallAmdgcnClass(llvm::Value* pValue, unsigned flags, const llvm::Twine& instName = "");

    // Methods to get various FP constants as scalar or vector. Any needed directly by a client should be moved
    // to llpcBuilder.h. Using these (rather than just using for example
    // ConstantFP::get(.., M_PI)) ensures that we always get the same value, independent of the
    // host platform and its compiler.
    // TODO: Use values that are suitable for doubles.

    // Get PI = 3.14159274 scalar or vector
    llvm::Constant* GetPi(llvm::Type* pTy)
    {
        return GetFpConstant(pTy, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x400921FB60000000)));
    }

    // Get PI/2 = 1.57079637 scalar or vector
    llvm::Constant* GetPiByTwo(llvm::Type* pTy)
    {
        return GetFpConstant(pTy, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FF921FB60000000)));
    }

    // Get PI/4 - 1 = -0.21460181 scalar or vector
    llvm::Constant* GetPiByFourMinusOne(llvm::Type* pTy)
    {
        return GetFpConstant(pTy, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0xBFCB781280000000)));
    }

    // Get 1/log(2) = 1.442695 scalar or vector
    llvm::Constant* GetRecipLog2(llvm::Type* pTy)
    {
        return GetFpConstant(pTy, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FF7154760000000)));
    }

    // Get 0.5 * log(2) = 0.34657359 scalar or vector
    llvm::Constant* GetHalfLog2(llvm::Type* pTy)
    {
        return GetFpConstant(pTy, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FD62E4300000000)));
    }

    // Get log(2) = 0.6931471824646 scalar or vector
    llvm::Constant* GetLog2(llvm::Type* pTy)
    {
        return GetFpConstant(pTy, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FE62E4300000000)));
    }

    // Get 2^-15 (normalized float16 minimum) scalar or vector
    llvm::Constant* GetMinNormalizedF16(llvm::Type* pTy)
    {
        return llvm::ConstantFP::get(pTy, 0.000030517578125);
    }

    // Ensure result is canonicalized if the shader's FP mode is flush denorms.
    llvm::Value* Canonicalize(llvm::Value* pValue);
};

// =====================================================================================================================
// Builder implementation subclass for descriptors
class BuilderImplDesc : virtual public BuilderImplBase
{
public:
    BuilderImplDesc(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create a load of a buffer descriptor.
    llvm::Value* CreateLoadBufferDesc(unsigned      descSet,
                                unsigned      binding,
                                llvm::Value*        pDescIndex,
                                bool          isNonUniform,
                                bool          isWritten,
                                llvm::Type*         pPointeeTy,
                                const llvm::Twine&  instName) override final;

    // Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
    llvm::Value* CreateIndexDescPtr(llvm::Value*        pDescPtr,
                              llvm::Value*        pIndex,
                              bool          isNonUniform,
                              const llvm::Twine&  instName) override final;

    // Load image/sampler/texelbuffer/F-mask descriptor from pointer.
    llvm::Value* CreateLoadDescFromPtr(llvm::Value*        pDescPtr,
                                 const llvm::Twine&  instName) override final;

    // Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
    llvm::Value* CreateGetSamplerDescPtr(unsigned      descSet,
                                   unsigned      binding,
                                   const llvm::Twine&  instName) override final;

    // Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
    llvm::Value* CreateGetImageDescPtr(unsigned      descSet,
                                 unsigned      binding,
                                 const llvm::Twine&  instName) override final;

    // Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
    llvm::Value* CreateGetTexelBufferDescPtr(unsigned      descSet,
                                       unsigned      binding,
                                       const llvm::Twine&  instName) override final;

    // Create a pointer to F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
    llvm::Value* CreateGetFmaskDescPtr(unsigned      descSet,
                                 unsigned      binding,
                                 const llvm::Twine&  instName) override final;

    // Create a load of the push constants pointer.
    llvm::Value* CreateLoadPushConstantsPtr(llvm::Type*         pPushConstantsTy,
                                      const llvm::Twine&  instName) override final;

    // Create a buffer length query based on the specified descriptor.
    llvm::Value* CreateGetBufferDescLength(llvm::Value* const pBufferDesc,
                                     const llvm::Twine& instName = "") override final;

private:
    BuilderImplDesc() = delete;
    BuilderImplDesc(const BuilderImplDesc&) = delete;
    BuilderImplDesc& operator=(const BuilderImplDesc&) = delete;

    llvm::Value* ScalarizeIfUniform(llvm::Value* pValue, bool isNonUniform);
};

// =====================================================================================================================
// Builder implementation subclass for image operations
class BuilderImplImage : virtual public BuilderImplBase
{
public:
    BuilderImplImage(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // Create an image load.
    llvm::Value* CreateImageLoad(llvm::Type*             pResultTy,
                           unsigned          dim,
                           unsigned          flags,
                           llvm::Value*            pImageDesc,
                           llvm::Value*            pCoord,
                           llvm::Value*            pMipLevel,
                           const llvm::Twine&      instName = "") override final;

    // Create an image load with F-mask.
    llvm::Value* CreateImageLoadWithFmask(llvm::Type*             pResultTy,
                                    unsigned          dim,
                                    unsigned          flags,
                                    llvm::Value*            pImageDesc,
                                    llvm::Value*            pFmaskDesc,
                                    llvm::Value*            pCoord,
                                    llvm::Value*            pSampleNum,
                                    const llvm::Twine&      instName = "") override final;

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

    // Create an image sample with conversion.
    // This is not yet a Builder API, but it could become one if there was to be a new SPIR-V YCbCr
    // converting sampler spec that allows the SPIR-V reader to tell that it has a converting sampler.
    llvm::Value* CreateImageSampleConvert(llvm::Type*             pResultTy,
                                    unsigned          dim,
                                    unsigned          flags,
                                    llvm::Value*            pImageDesc,
                                    llvm::Value*            pConvertingSamplerDesc,
                                    llvm::ArrayRef<llvm::Value*>  address,
                                    const llvm::Twine&      instName = "");

    // Create an image gather
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

private:
    BuilderImplImage() = delete;
    BuilderImplImage(const BuilderImplImage&) = delete;
    BuilderImplImage& operator=(const BuilderImplImage&) = delete;

    // Implement pre-GFX9 integer gather workaround to patch descriptor or coordinate before the gather
    llvm::Value* PreprocessIntegerImageGather(unsigned dim, llvm::Value*& pImageDesc, llvm::Value*& pCoord);

    // Implement pre-GFX9 integer gather workaround to modify result.
    llvm::Value* PostprocessIntegerImageGather(llvm::Value*   pNeedDescPatch,
                                         unsigned flags,
                                         llvm::Value*   pImageDesc,
                                         llvm::Type*    pTexelTy,
                                         llvm::Value*   pResult);

    // Common code to create an image sample or gather.
    llvm::Value* CreateImageSampleGather(llvm::Type*            pResultTy,
                                   unsigned         dim,
                                   unsigned         flags,
                                   llvm::Value*           pCoord,
                                   llvm::Value*           pImageDesc,
                                   llvm::Value*           pSamplerDesc,
                                   llvm::ArrayRef<llvm::Value*> address,
                                   const llvm::Twine&     instName,
                                   bool             isSample);

    // Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
    llvm::Value* CreateImageAtomicCommon(unsigned          atomicOp,
                                   unsigned          dim,
                                   unsigned          flags,
                                   llvm::AtomicOrdering    ordering,
                                   llvm::Value*            pImageDesc,
                                   llvm::Value*            pCoord,
                                   llvm::Value*            pInputValue,
                                   llvm::Value*            pComparatorValue,
                                   const llvm::Twine&      instName);

    // Change 1D or 1DArray dimension to 2D or 2DArray if needed as a workaround on GFX9+
    unsigned Change1DTo2DIfNeeded(unsigned dim);

    // Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
    // modifying if necessary.
    // Returns possibly modified image dimension.
    unsigned PrepareCoordinate(unsigned                  dim,
                               llvm::Value*                    pCoord,
                               llvm::Value*                    pProjective,
                               llvm::Value*                    pDerivativeX,
                               llvm::Value*                    pDerivativeY,
                               llvm::SmallVectorImpl<llvm::Value*>&  outCoords,
                               llvm::SmallVectorImpl<llvm::Value*>&  outDerivatives);

    // For a cubearray with integer coordinates, combine the face and slice into a single component.
    void CombineCubeArrayFaceAndSlice(llvm::Value* pCoord, llvm::SmallVectorImpl<llvm::Value*>& coords);

    // Patch descriptor with cube dimension for image call
    llvm::Value* PatchCubeDescriptor(llvm::Value* pDesc, unsigned dim);

    // Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
    llvm::Value* HandleFragCoordViewIndex(llvm::Value* pCoord, unsigned flags, unsigned& dim);

    // -----------------------------------------------------------------------------------------------------------------

    enum ImgDataFormat
    {
        IMG_DATA_FORMAT_32          = 4,
        IMG_DATA_FORMAT_32_32       = 11,
        IMG_DATA_FORMAT_32_32_32_32 = 14,
    };

    static const unsigned AtomicOpCompareSwap = 1;
};

// =====================================================================================================================
// Builder implementation subclass for input/output operations
class BuilderImplInOut : virtual public BuilderImplBase
{
public:
    BuilderImplInOut(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

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

    // Create a read of (part of) an output built-in value.
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

    // Get name of built-in
    static llvm::StringRef GetBuiltInName(BuiltInKind builtIn);

private:
    BuilderImplInOut() = delete;
    BuilderImplInOut(const BuilderImplInOut&) = delete;
    BuilderImplInOut& operator=(const BuilderImplInOut&) = delete;

    // Read (a part of) a generic (user) input/output value.
    llvm::Value* ReadGenericInputOutput(bool          isOutput,
                                  llvm::Type*         pResultTy,
                                  unsigned      location,
                                  llvm::Value*        pLocationOffset,
                                  llvm::Value*        pElemIdx,
                                  unsigned      locationCount,
                                  InOutInfo     inOutInfo,
                                  llvm::Value*        pVertexIndex,
                                  const llvm::Twine&  instName);

    // Mark usage for a generic (user) input or output
    void MarkGenericInputOutputUsage(bool          isOutput,
                                     unsigned      location,
                                     unsigned      locationCount,
                                     InOutInfo     inOutInfo,
                                     llvm::Value*        pVertexIndex);

    // Mark interpolation info for FS input.
    void MarkInterpolationInfo(InOutInfo interpInfo);

    // Mark fragment output type
    void MarkFsOutputType(llvm::Type* pOutputTy, unsigned location, InOutInfo outputInfo);

    // Modify aux interp value according to custom interp mode, and its helper functions.
    llvm::Value* ModifyAuxInterpValue(llvm::Value* pAuxInterpValue, InOutInfo inputInfo);
    llvm::Value* EvalIJOffsetNoPersp(llvm::Value* pOffset);
    llvm::Value* EvalIJOffsetSmooth(llvm::Value* pOffset);
    llvm::Value* AdjustIJ(llvm::Value* pValue, llvm::Value* pOffset);

    // Read (part of) a built-in value
    llvm::Value* ReadBuiltIn(bool         isOutput,
                       BuiltInKind  builtIn,
                       InOutInfo    inOutInfo,
                       llvm::Value*       pVertexIndex,
                       llvm::Value*       pIndex,
                       const llvm::Twine& instName);

    // Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
    llvm::Type* GetBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo);

    // Mark usage of a built-in input
    void MarkBuiltInInputUsage(BuiltInKind builtIn, unsigned arraySize);

    // Mark usage of a built-in output
    void MarkBuiltInOutputUsage(BuiltInKind builtIn, unsigned arraySize, unsigned streamId);

#ifndef NDEBUG
    // Get a bitmask of which shader stages are valid for a built-in to be an input or output of
    unsigned GetBuiltInValidMask(BuiltInKind builtIn, bool isOutput);

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
    llvm::Value* CreateTransposeMatrix(llvm::Value* const pMatrix,
                                 const llvm::Twine& instName = "") override final;

    // Create matrix multiplication: matrix times scalar, resulting in matrix
    llvm::Value* CreateMatrixTimesScalar(llvm::Value* const pMatrix,
                                   llvm::Value* const pScalar,
                                   const llvm::Twine& instName = "") override final;

    // Create matrix multiplication: vector times matrix, resulting in vector
    llvm::Value* CreateVectorTimesMatrix(llvm::Value* const pVector,
                                   llvm::Value* const pMatrix,
                                   const llvm::Twine& instName = "") override final;

    // Create matrix multiplication: matrix times vector, resulting in vector
    llvm::Value* CreateMatrixTimesVector(llvm::Value* const pMatrix,
                                   llvm::Value* const pVector,
                                   const llvm::Twine& instName = "") override final;

    // Create matrix multiplication:  matrix times matrix, resulting in matrix
    llvm::Value* CreateMatrixTimesMatrix(llvm::Value* const pMatrix1,
                                   llvm::Value* const pMatrix2,
                                   const llvm::Twine& instName = "") override final;

    // Create vector outer product operation, resulting in matrix
    llvm::Value* CreateOuterProduct(llvm::Value* const pVector1,
                              llvm::Value* const pVector2,
                              const llvm::Twine& instName = "") override final;

    // Create matrix determinant operation.
    llvm::Value* CreateDeterminant(llvm::Value* const pMatrix, const llvm::Twine& instName = "") override final;

    // Create matrix inverse operation.
    llvm::Value* CreateMatrixInverse(llvm::Value* const pMatrix, const llvm::Twine& instName = "") override final;

private:
    BuilderImplMatrix() = delete;
    BuilderImplMatrix(const BuilderImplMatrix&) = delete;
    BuilderImplMatrix& operator=(const BuilderImplMatrix&) = delete;

    // Helper function for determinant calculation
    llvm::Value* Determinant(llvm::ArrayRef<llvm::Value*> elements, unsigned order);

    // Get submatrix by deleting specified row and column
    void GetSubmatrix(llvm::ArrayRef<llvm::Value*>        matrix,
                      llvm::MutableArrayRef<llvm::Value*> submatrix,
                      unsigned                order,
                      unsigned                rowToDelete,
                      unsigned                columnToDelete);
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class BuilderImplMisc : virtual public BuilderImplBase
{
public:
    BuilderImplMisc(BuilderContext* pBuilderContext) : BuilderImplBase(pBuilderContext) {}

    // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
    // the current output primitive in the specified output-primitive stream.
    llvm::Instruction* CreateEmitVertex(unsigned streamId) override final;

    // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
    llvm::Instruction* CreateEndPrimitive(unsigned streamId) override final;

    // Create a workgroup control barrier.
    llvm::Instruction* CreateBarrier() override final;

    // Create a "kill". Only allowed in a fragment shader.
    llvm::Instruction* CreateKill(const llvm::Twine& instName) override final;

    // Create a "readclock".
    llvm::Instruction* CreateReadClock(bool realtime, const llvm::Twine& instName) override final;

    // Create derivative calculation on float or vector of float or half
    llvm::Value* CreateDerivative(llvm::Value* pValue, bool isDirectionY, bool isFine, const llvm::Twine& instName = "") override final;

    // Create a demote to helper invocation operation. Only allowed in a fragment shader.
    llvm::Instruction* CreateDemoteToHelperInvocation(const llvm::Twine& instName) override final;

    // Create a helper invocation query. Only allowed in a fragment shader.
    llvm::Value* CreateIsHelperInvocation(const llvm::Twine& instName) override final;

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
    llvm::Value* CreateGetSubgroupSize(const llvm::Twine& instName) override final;

    // Create a subgroup elect.
    llvm::Value* CreateSubgroupElect(const llvm::Twine& instName) override final;

    // Create a subgroup all.
    llvm::Value* CreateSubgroupAll(llvm::Value* const pValue,
                             bool         wqm,
                             const llvm::Twine& instName) override final;

    // Create a subgroup any
    llvm::Value* CreateSubgroupAny(llvm::Value* const pValue,
                             bool         wqm,
                             const llvm::Twine& instName) override final;

    // Create a subgroup all equal.
    llvm::Value* CreateSubgroupAllEqual(llvm::Value* const pValue,
                                  bool         wqm,
                                  const llvm::Twine& instName) override final;

    // Create a subgroup broadcast.
    llvm::Value* CreateSubgroupBroadcast(llvm::Value* const pValue,
                                   llvm::Value* const pIndex,
                                   const llvm::Twine& instName) override final;

    // Create a subgroup broadcast first.
    llvm::Value* CreateSubgroupBroadcastFirst(llvm::Value* const pValue,
                                        const llvm::Twine& instName) override final;

    // Create a subgroup ballot.
    llvm::Value* CreateSubgroupBallot(llvm::Value* const pValue,
                                const llvm::Twine& instName) override final;

    // Create a subgroup inverse ballot.
    llvm::Value* CreateSubgroupInverseBallot(llvm::Value* const pValue,
                                       const llvm::Twine& instName) override final;

    // Create a subgroup ballot bit extract.
    llvm::Value* CreateSubgroupBallotBitExtract(llvm::Value* const pValue,
                                          llvm::Value* const pIndex,
                                          const llvm::Twine& instName) override final;

    // Create a subgroup ballot bit count.
    llvm::Value* CreateSubgroupBallotBitCount(llvm::Value* const pValue,
                                        const llvm::Twine& instName) override final;

    // Create a subgroup ballot inclusive bit count.
    llvm::Value* CreateSubgroupBallotInclusiveBitCount(llvm::Value* const pValue,
                                                 const llvm::Twine& instName) override final;

    // Create a subgroup ballot exclusive bit count.
    llvm::Value* CreateSubgroupBallotExclusiveBitCount(llvm::Value* const pValue,
                                                 const llvm::Twine& instName) override final;

    // Create a subgroup ballot find least significant bit.
    llvm::Value* CreateSubgroupBallotFindLsb(llvm::Value* const pValue,
                                       const llvm::Twine& instName) override final;

    // Create a subgroup ballot find most significant bit.
    llvm::Value* CreateSubgroupBallotFindMsb(llvm::Value* const pValue,
                                       const llvm::Twine& instName) override final;

    // Create a subgroup shuffle.
    llvm::Value* CreateSubgroupShuffle(llvm::Value* const pValue,
                                 llvm::Value* const pIndex,
                                 const llvm::Twine& instName) override final;

    // Create a subgroup shuffle xor.
    llvm::Value* CreateSubgroupShuffleXor(llvm::Value* const pValue,
                                    llvm::Value* const pMask,
                                    const llvm::Twine& instName) override final;

    // Create a subgroup shuffle up.
    llvm::Value* CreateSubgroupShuffleUp(llvm::Value* const pValue,
                                   llvm::Value* const pDelta,
                                   const llvm::Twine& instName) override final;

    // Create a subgroup shuffle down.
    llvm::Value* CreateSubgroupShuffleDown(llvm::Value* const pValue,
                                     llvm::Value* const pDelta,
                                     const llvm::Twine& instName) override final;

    // Create a subgroup clustered reduction.
    llvm::Value* CreateSubgroupClusteredReduction(GroupArithOp groupArithOp,
                                            llvm::Value* const pValue,
                                            llvm::Value* const pClusterSize,
                                            const llvm::Twine& instName) override final;

    // Create a subgroup clustered inclusive scan.
    llvm::Value* CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp,
                                            llvm::Value* const pValue,
                                            llvm::Value* const pClusterSize,
                                            const llvm::Twine& instName) override final;

    // Create a subgroup clustered exclusive scan.
    llvm::Value* CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp,
                                            llvm::Value* const pValue,
                                            llvm::Value* const pClusterSize,
                                            const llvm::Twine& instName) override final;

    // Create a subgroup quad broadcast.
    llvm::Value* CreateSubgroupQuadBroadcast(llvm::Value* const pValue,
                                       llvm::Value* const pIndex,
                                       const llvm::Twine& instName) override final;

    // Create a subgroup quad swap horizontal.
    llvm::Value* CreateSubgroupQuadSwapHorizontal(llvm::Value* const pValue,
                                            const llvm::Twine& instName) override final;

    // Create a subgroup quad swap vertical.
    llvm::Value* CreateSubgroupQuadSwapVertical(llvm::Value* const pValue,
                                          const llvm::Twine& instName) override final;

    // Create a subgroup quad swap diagonal.
    llvm::Value* CreateSubgroupQuadSwapDiagonal(llvm::Value* const pValue,
                                          const llvm::Twine& instName) override final;

    // Create a subgroup swizzle quad.
    llvm::Value* CreateSubgroupSwizzleQuad(llvm::Value* const pValue,
                                     llvm::Value* const pOffset,
                                     const llvm::Twine& instName) override final;

    // Create a subgroup swizzle masked.
    llvm::Value* CreateSubgroupSwizzleMask(llvm::Value* const pValue,
                                     llvm::Value* const pMask,
                                     const llvm::Twine& instName) override final;

    // Create a subgroup write invocation.
    llvm::Value* CreateSubgroupWriteInvocation(llvm::Value* const pInputValue,
                                         llvm::Value* const pWriteValue,
                                         llvm::Value* const pIndex,
                                         const llvm::Twine& instName) override final;

    // Create a subgroup mbcnt.
    llvm::Value* CreateSubgroupMbcnt(llvm::Value* const pMask,
                               const llvm::Twine& instName) override final;

private:
    BuilderImplSubgroup() = delete;
    BuilderImplSubgroup(const BuilderImplSubgroup&) = delete;
    BuilderImplSubgroup& operator=(const BuilderImplSubgroup&) = delete;

    enum class DppCtrl : unsigned
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

    unsigned GetShaderSubgroupSize();
    llvm::Value* CreateGroupArithmeticIdentity(GroupArithOp   groupArithOp,
                                         llvm::Type* const    pType);
    llvm::Value* CreateGroupArithmeticOperation(GroupArithOp groupArithOp,
                                          llvm::Value* const pX,
                                          llvm::Value* const pY);
    llvm::Value* CreateInlineAsmSideEffect(llvm::Value* const pValue);
    llvm::Value* CreateDppMov(llvm::Value* const pValue,
                        DppCtrl      dppCtrl,
                        unsigned     rowMask,
                        unsigned     bankMask,
                        bool         boundCtrl);
    llvm::Value* CreateDppUpdate(llvm::Value* const pOrigValue,
                           llvm::Value* const pUpdateValue,
                           DppCtrl      dppCtrl,
                           unsigned     rowMask,
                           unsigned     bankMask,
                           bool         boundCtrl);

    llvm::Value* CreatePermLane16(llvm::Value* const pOrigValue,
                            llvm::Value* const pUpdateValue,
                            unsigned     selectBitsLow,
                            unsigned     selectBitsHigh,
                            bool         fetchInactive,
                            bool         boundCtrl);
    llvm::Value* CreatePermLaneX16(llvm::Value* const pOrigValue,
                             llvm::Value* const pUpdateValue,
                             unsigned     selectBitsLow,
                             unsigned     selectBitsHigh,
                             bool         fetchInactive,
                             bool         boundCtrl);

    llvm::Value* CreateDsSwizzle(llvm::Value* const pValue,
                           uint16_t     dsPattern);
    llvm::Value* CreateWwm(llvm::Value* const pValue);
    llvm::Value* CreateSetInactive(llvm::Value* const pActive,
                             llvm::Value* const pInactive);
    llvm::Value* CreateThreadMask();
    llvm::Value* CreateThreadMaskedSelect(
        llvm::Value* const pThreadMask,
        uint64_t     andMask,
        llvm::Value* const pValue1,
        llvm::Value* const pValue2);
    uint16_t GetDsSwizzleBitMode(uint8_t xorMask,
                                 uint8_t orMask,
                                 uint8_t andMask);
    uint16_t GetDsSwizzleQuadMode(uint8_t lane0,
                                  uint8_t lane1,
                                  uint8_t lane2,
                                  uint8_t lane3);
    llvm::Value* CreateGroupBallot(llvm::Value* const pValue);
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

} // lgc
