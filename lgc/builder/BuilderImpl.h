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
 * @file  BuilderImpl.h
 * @brief LLPC header file: declaration of lgc::Builder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Builder.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"

namespace lgc {

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : public Builder {
public:
  BuilderImplBase(LgcContext *builderContext) : Builder(builderContext) {}

  // Create scalar from dot product of vector
  llvm::Value *CreateDotProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                const llvm::Twine &instName = "") override final;

  // Create scalar from integer dot product of vector
  llvm::Value *CreateIntegerDotProduct(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                       unsigned flags, const llvm::Twine &instName = "") override final;

protected:
  // Get the ShaderModes object.
  ShaderModes *getShaderModes() override final;

  // Get the PipelineState object.
  PipelineState *getPipelineState() const { return m_pipelineState; }

  // Get whether the context we are building in supports DPP operations.
  bool supportDpp() const;

  // Get whether the context we are building in supports DPP ROW_XMASK operations.
  bool supportDppRowXmask() const;

  // Get whether the context we are building in support the bpermute operation.
  bool supportBPermute() const;

  // Get whether the context we are building in supports permute lane DPP operations.
  bool supportPermLaneDpp() const;

  // Get whether the context we are building in supports permute lane 64 DPP operations.
  bool supportPermLane64Dpp() const;

  // Create an "if..endif" or "if..else..endif" structure.
  llvm::BranchInst *createIf(llvm::Value *condition, bool wantElse, const llvm::Twine &instName);

  // Create a waterfall loop containing the specified instruction.
  llvm::Instruction *createWaterfallLoop(llvm::Instruction *nonUniformInst, llvm::ArrayRef<unsigned> operandIdxs,
                                         bool scalarizeDescriptorLoads = false, const llvm::Twine &instName = "");

  // Helper method to scalarize a possibly vector unary operation
  llvm::Value *scalarize(llvm::Value *value, const std::function<llvm::Value *(llvm::Value *)> &callback);

  // Helper method to scalarize in pairs a possibly vector unary operation.
  llvm::Value *scalarizeInPairs(llvm::Value *value, const std::function<llvm::Value *(llvm::Value *)> &callback);

  // Helper method to scalarize a possibly vector binary operation
  llvm::Value *scalarize(llvm::Value *value0, llvm::Value *value1,
                         const std::function<llvm::Value *(llvm::Value *, llvm::Value *)> &callback);

  // Helper method to scalarize a possibly vector trinary operation
  llvm::Value *scalarize(llvm::Value *value0, llvm::Value *value1, llvm::Value *value2,
                         const std::function<llvm::Value *(llvm::Value *, llvm::Value *, llvm::Value *)> &callback);

  // Create code to get the lane number within the wave. This depends on whether the shader is wave32 or wave64,
  // and thus on the shader stage it is used from.
  llvm::Value *CreateGetLaneNumber();

  // Forwarding methods for methods in BuilderBase that BuilderImpl subclasses may need to access.
  // We want these methods in BuilderBase to be accessible from anywhere in LGC, both BuilderImpl subclasses
  // and later passes, but not from outside LGC. There is no possible class hierarchy to make that happen, without
  // also making Builder methods visible from later passes in LGC, which we don't want.
  llvm::Value *CreateRelocationConstant(const llvm::Twine &symbolName) {
    return BuilderBase::get(*this).CreateRelocationConstant(symbolName);
  }
  llvm::Value *CreateAddByteOffset(llvm::Value *pointer, llvm::Value *byteOffset, const llvm::Twine &instName = "") {
    return BuilderBase::get(*this).CreateAddByteOffset(pointer, byteOffset, instName);
  }
  llvm::Value *CreateMapToInt32(BuilderBase::MapToInt32Func mapFunc, llvm::ArrayRef<llvm::Value *> mappedArgs,
                                llvm::ArrayRef<llvm::Value *> passthroughArgs) {
    return BuilderBase::get(*this).CreateMapToInt32(mapFunc, mappedArgs, passthroughArgs);
  }

  PipelineState *m_pipelineState = nullptr; // Pipeline state

private:
  BuilderImplBase() = delete;
  BuilderImplBase(const BuilderImplBase &) = delete;
  BuilderImplBase &operator=(const BuilderImplBase &) = delete;
};

// =====================================================================================================================
// Builder implementation subclass for arithmetic operations
class ArithBuilder : virtual public BuilderImplBase {
public:
  ArithBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

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

  // Methods to create trig and exponential operations.
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
  llvm::Value *CreateSmoothStep(llvm::Value *edge0, llvm::Value *edge1, llvm::Value *x,
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

private:
  ArithBuilder() = delete;
  ArithBuilder(const ArithBuilder &) = delete;
  ArithBuilder &operator=(const ArithBuilder &) = delete;

  // Common code for asin and acos
  llvm::Value *aSinACosCommon(llvm::Value *x, llvm::Constant *coefP0, llvm::Constant *coefP1);

  // Generate FP division, using fast fdiv for float to bypass optimization.
  llvm::Value *fDivFast(llvm::Value *numerator, llvm::Value *denominator);

  // Helper method to create call to llvm.amdgcn.class, scalarizing if necessary. This is not exposed outside of
  // ArithBuilder.
  llvm::Value *createCallAmdgcnClass(llvm::Value *value, unsigned flags, const llvm::Twine &instName = "");

  // Methods to get various FP constants as scalar or vector. Any needed directly by a client should be moved
  // to Builder.h. Using these (rather than just using for example
  // ConstantFP::get(.., M_PI)) ensures that we always get the same value, independent of the
  // host platform and its compiler.
  // TODO: Use values that are suitable for doubles.

  // Get PI = 3.14159274 scalar or vector
  llvm::Constant *getPi(llvm::Type *ty) {
    return getFpConstant(ty, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x400921FB60000000)));
  }

  // Get PI/2 = 1.57079637 scalar or vector
  llvm::Constant *getPiByTwo(llvm::Type *ty) {
    return getFpConstant(ty, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FF921FB60000000)));
  }

  // Get PI/4 - 1 = -0.21460181 scalar or vector
  llvm::Constant *getPiByFourMinusOne(llvm::Type *ty) {
    return getFpConstant(ty, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0xBFCB781280000000)));
  }

  // Get 1/log(2) = 1.442695 scalar or vector
  llvm::Constant *getRecipLog2(llvm::Type *ty) {
    return getFpConstant(ty, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FF7154760000000)));
  }

  // Get 0.5 * log(2) = 0.34657359 scalar or vector
  llvm::Constant *getHalfLog2(llvm::Type *ty) {
    return getFpConstant(ty, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FD62E4300000000)));
  }

  // Get log(2) = 0.6931471824646 scalar or vector
  llvm::Constant *getLog2(llvm::Type *ty) {
    return getFpConstant(ty, llvm::APFloat(llvm::APFloat::IEEEdouble(), llvm::APInt(64, 0x3FE62E4300000000)));
  }

  // Get 2^-15 (normalized float16 minimum) scalar or vector
  llvm::Constant *getMinNormalizedF16(llvm::Type *ty) { return llvm::ConstantFP::get(ty, 0.000030517578125); }

  // Ensure result is canonicalized if the shader's FP mode is flush denorms.
  llvm::Value *canonicalize(llvm::Value *value);
};

// =====================================================================================================================
// Builder implementation subclass for descriptors
class DescBuilder : virtual public BuilderImplBase {
public:
  DescBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

  // Create a load of a buffer descriptor.
  llvm::Value *CreateLoadBufferDesc(unsigned descSet, unsigned binding, llvm::Value *descIndex, unsigned flags,
                                    llvm::Type *pointeeTy, const llvm::Twine &instName) override final;

  // Create a get of the stride (in bytes) of a descriptor.
  llvm::Value *CreateGetDescStride(ResourceNodeType concreteType, ResourceNodeType abstractType, unsigned descSet,
                                   unsigned binding, const llvm::Twine &instName) override final;

  // Create a pointer to a descriptor.
  llvm::Value *CreateGetDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, unsigned descSet,
                                unsigned binding, const llvm::Twine &instName) override final;

  // Create a load of the push constants pointer.
  llvm::Value *CreateLoadPushConstantsPtr(llvm::Type *returnTy, const llvm::Twine &instName) override final;

  // Create a buffer length query based on the specified descriptor.
  llvm::Value *CreateGetBufferDescLength(llvm::Value *const bufferDesc, llvm::Value *offset,
                                         const llvm::Twine &instName = "") override final;

  // Return the i64 difference between two pointers, dividing out the size of the pointed-to objects.
  llvm::Value *CreatePtrDiff(llvm::Type *ty, llvm::Value *lhs, llvm::Value *rhs,
                             const llvm::Twine &instName = "") override final;

private:
  DescBuilder() = delete;
  DescBuilder(const DescBuilder &) = delete;
  DescBuilder &operator=(const DescBuilder &) = delete;

  // Get a struct containing the pointer and byte stride for a descriptor
  llvm::Value *getDescPtrAndStride(ResourceNodeType resType, unsigned descSet, unsigned binding,
                                   const ResourceNode *topNode, const ResourceNode *node, bool shadow);

  // Get the stride (in bytes) of a descriptor.
  llvm::Value *getStride(ResourceNodeType descType, unsigned descSet, unsigned binding, const ResourceNode *node);

  // Get a pointer to a descriptor, as a pointer to i8
  llvm::Value *getDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, unsigned descSet,
                          unsigned binding, const ResourceNode *topNode, const ResourceNode *node);

  llvm::Value *scalarizeIfUniform(llvm::Value *value, bool isNonUniform);

  // Calculate a buffer descriptor for an inline buffer
  llvm::Value *buildInlineBufferDesc(llvm::Value *descPtr);

  // Build buffer compact descriptor
  llvm::Value *buildBufferCompactDesc(llvm::Value *desc);
};

// =====================================================================================================================
// Builder implementation subclass for image operations
class ImageBuilder : virtual public BuilderImplBase {
  friend class YCbCrConverter;

public:
  ImageBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

  // Create an image load.
  llvm::Value *CreateImageLoad(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                               llvm::Value *coord, llvm::Value *mipLevel,
                               const llvm::Twine &instName = "") override final;

  // Create an image load with F-mask.
  llvm::Value *CreateImageLoadWithFmask(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                        llvm::Value *fmaskDesc, llvm::Value *coord, llvm::Value *sampleNum,
                                        const llvm::Twine &instName = "") override final;

  // Create an image store.
  llvm::Value *CreateImageStore(llvm::Value *texel, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                llvm::Value *coord, llvm::Value *mipLevel,
                                const llvm::Twine &instName = "") override final;

  // Create an image sample.
  llvm::Value *CreateImageSample(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                 llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                 const llvm::Twine &instName = "") override final;

  // Create an image sample with conversion.
  llvm::Value *CreateImageSampleConvert(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                        llvm::Value *convertingSamplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                        const llvm::Twine &instName = "") override final;

  // Create an image sample with YCbCr conversion.
  llvm::Value *CreateImageSampleConvertYCbCr(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                             llvm::Value *convertingSamplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                             const llvm::Twine &instName = "");

  // Create an image gather
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

private:
  ImageBuilder() = delete;
  ImageBuilder(const ImageBuilder &) = delete;
  ImageBuilder &operator=(const ImageBuilder &) = delete;

  // Implement pre-GFX9 integer gather workaround to patch descriptor or coordinate before the gather
  llvm::Value *preprocessIntegerImageGather(unsigned dim, llvm::Value *&imageDesc, llvm::Value *&coord);

  // Implement pre-GFX9 integer gather workaround to modify result.
  llvm::Value *postprocessIntegerImageGather(llvm::Value *needDescPatch, unsigned flags, llvm::Value *imageDesc,
                                             llvm::Type *texelTy, llvm::Value *result);

  // Common code to create an image sample or gather.
  llvm::Value *CreateImageSampleGather(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *coord,
                                       llvm::Value *imageDesc, llvm::Value *samplerDesc,
                                       llvm::ArrayRef<llvm::Value *> address, const llvm::Twine &instName,
                                       bool isSample);

  // Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
  llvm::Value *CreateImageAtomicCommon(unsigned atomicOp, unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                       llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                       llvm::Value *comparatorValue, const llvm::Twine &instName);

  // Change 1D or 1DArray dimension to 2D or 2DArray if needed as a workaround on GFX9+
  unsigned change1DTo2DIfNeeded(unsigned dim);

  // Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
  // modifying if necessary.
  // Returns possibly modified image dimension.
  unsigned prepareCoordinate(unsigned dim, llvm::Value *coord, llvm::Value *projective, llvm::Value *derivativeX,
                             llvm::Value *derivativeY, llvm::SmallVectorImpl<llvm::Value *> &outCoords,
                             llvm::SmallVectorImpl<llvm::Value *> &outDerivatives);

  // For a cubearray with integer coordinates, combine the face and slice into a single component.
  void combineCubeArrayFaceAndSlice(llvm::Value *coord, llvm::SmallVectorImpl<llvm::Value *> &coords);

  // Patch descriptor with cube dimension for image call
  llvm::Value *patchCubeDescriptor(llvm::Value *desc, unsigned dim);

  // Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
  llvm::Value *handleFragCoordViewIndex(llvm::Value *coord, unsigned flags, unsigned &dim);

  // Fix image descriptor before an operation that reads the image
  llvm::Value *fixImageDescForRead(llvm::Value *imageDesc);

  // Enforce readfirstlane on the image or sampler descriptors
  void enforceReadFirstLane(llvm::Instruction *imageInst, unsigned descIdx);

  enum ImgDataFormat {
    IMG_DATA_FORMAT_32 = 4,
    IMG_DATA_FORMAT_8_8_8_8 = 10,
    IMG_DATA_FORMAT_32_32 = 11,
    IMG_DATA_FORMAT_32_32_32_32 = 14,
    IMG_DATA_FORMAT_GB_GR__CORE = 32,
    IMG_DATA_FORMAT_BG_RG__CORE = 33,
  };

  enum ImgFmtGfx10 {
    IMG_FMT_8_8_8_8_UNORM__GFX10CORE = 56,
    IMG_FMT_GB_GR_UNORM__GFX10CORE = 147,
    IMG_FMT_BG_RG_UNORM__GFX10CORE = 151,
  };

  static const unsigned AtomicOpCompareSwap = 1;
};

// =====================================================================================================================
// Builder implementation subclass for input/output operations
class InOutBuilder : virtual public BuilderImplBase {
public:
  InOutBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

  // Create a read of (part of) a user input value.
  llvm::Value *CreateReadGenericInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                      llvm::Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                      llvm::Value *vertexIndex, const llvm::Twine &instName = "") override final;

  // Create a read of (part of) a perVertex input value.
  llvm::Value *CreateReadPerVertexInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
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

  // Create a read of (part of) an output built-in value.
  llvm::Value *CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, llvm::Value *vertexIndex,
                                       llvm::Value *index, const llvm::Twine &instName = "") override final;

  // Create a write of (part of) a built-in output value.
  llvm::Instruction *CreateWriteBuiltInOutput(llvm::Value *valueToWrite, BuiltInKind builtIn, InOutInfo outputInfo,
                                              llvm::Value *vertexOrPrimitiveIndex, llvm::Value *index) override final;

  // Create a read from (part of) a task payload.
  llvm::Value *CreateReadTaskPayload(llvm::Type *resultTy, llvm::Value *byteOffset,
                                     const llvm::Twine &instName = "") override final;

  // Create a write to (part of) a task payload.
  llvm::Instruction *CreateWriteTaskPayload(llvm::Value *valueToWrite, llvm::Value *byteOffset,
                                            const llvm::Twine &instName = "") override final;

  // Create a task payload atomic operation other than compare-and-swap.
  llvm::Value *CreateTaskPayloadAtomic(unsigned atomicOp, llvm::AtomicOrdering ordering, llvm::Value *inputValue,
                                       llvm::Value *byteOffset, const llvm::Twine &instName = "") override final;

  // Create a task payload atomic compare-and-swap.
  llvm::Value *CreateTaskPayloadAtomicCompareSwap(llvm::AtomicOrdering ordering, llvm::Value *inputValue,
                                                  llvm::Value *comparatorValue, llvm::Value *byteOffset,
                                                  const llvm::Twine &instName = "") override final;

private:
  InOutBuilder() = delete;
  InOutBuilder(const InOutBuilder &) = delete;
  InOutBuilder &operator=(const InOutBuilder &) = delete;

  // Read (a part of) a generic (user) input/output value.
  llvm::Value *readGenericInputOutput(bool isOutput, llvm::Type *resultTy, unsigned location,
                                      llvm::Value *locationOffset, llvm::Value *elemIdx, unsigned locationCount,
                                      InOutInfo inOutInfo, llvm::Value *vertexIndex, const llvm::Twine &instName);

  // Mark usage for a generic (user) input or output
  void markGenericInputOutputUsage(bool isOutput, unsigned location, unsigned locationCount, InOutInfo &inOutInfo,
                                   llvm::Value *vertexOrPrimIndex);

  // Mark interpolation info for FS input.
  void markInterpolationInfo(InOutInfo &interpInfo);

  // Mark fragment output type
  void markFsOutputType(llvm::Type *outputTy, unsigned location, InOutInfo outputInfo);

  std::tuple<unsigned, llvm::Value *> getInterpModeAndValue(InOutInfo inputInfo, llvm::Value *auxInterpValue);
  llvm::Value *evalIjOffsetSmooth(llvm::Value *offset);
  llvm::Value *adjustIj(llvm::Value *value, llvm::Value *offset);

  // Read (part of) a built-in value
  llvm::Value *readBuiltIn(bool isOutput, BuiltInKind builtIn, InOutInfo inOutInfo, llvm::Value *vertexIndex,
                           llvm::Value *index, const llvm::Twine &instName);

  // Reorder the barycoord
  llvm::Value *normalizeBaryCoord(llvm::Value *ijCoord);

  // Read and directly handle certain built-ins that are common between shader stages
  llvm::Value *readCommonBuiltIn(BuiltInKind builtIn, llvm::Type *resultTy, const llvm::Twine &instName = "");

  // Read compute/task shader input
  llvm::Value *readCsBuiltIn(BuiltInKind builtIn, const llvm::Twine &instName = "");

  // Read vertex shader input
  llvm::Value *readVsBuiltIn(BuiltInKind builtIn, const llvm::Twine &instName = "");

  // Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
  llvm::Type *getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo);

  // Mark usage of a built-in input
  void markBuiltInInputUsage(BuiltInKind &builtIn, unsigned arraySize);

  // Mark usage of a built-in output
  void markBuiltInOutputUsage(BuiltInKind builtIn, unsigned arraySize, unsigned streamId);

#ifndef NDEBUG
  // Get a bitmask of which shader stages are valid for a built-in to be an input or output of
  unsigned getBuiltInValidMask(BuiltInKind builtIn, bool isOutput);

  // Determine whether a built-in is an input for a particular shader stage.
  bool isBuiltInInput(BuiltInKind builtIn);

  // Determine whether a built-in is an output for a particular shader stage.
  bool isBuiltInOutput(BuiltInKind builtIn);
#endif
};

// =====================================================================================================================
// Builder implementation subclass for matrix operations
class MatrixBuilder : virtual public BuilderImplBase {
public:
  MatrixBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

  // Create a matrix transpose.
  llvm::Value *CreateTransposeMatrix(llvm::Value *const matrix, const llvm::Twine &instName = "") override final;

  // Create matrix multiplication: matrix times scalar, resulting in matrix
  llvm::Value *CreateMatrixTimesScalar(llvm::Value *const matrix, llvm::Value *const scalar,
                                       const llvm::Twine &instName = "") override final;

  // Create matrix multiplication: vector times matrix, resulting in vector
  llvm::Value *CreateVectorTimesMatrix(llvm::Value *const vector, llvm::Value *const matrix,
                                       const llvm::Twine &instName = "") override final;

  // Create matrix multiplication: matrix times vector, resulting in vector
  llvm::Value *CreateMatrixTimesVector(llvm::Value *const matrix, llvm::Value *const vector,
                                       const llvm::Twine &instName = "") override final;

  // Create matrix multiplication:  matrix times matrix, resulting in matrix
  llvm::Value *CreateMatrixTimesMatrix(llvm::Value *const matrix1, llvm::Value *const matrix2,
                                       const llvm::Twine &instName = "") override final;

  // Create vector outer product operation, resulting in matrix
  llvm::Value *CreateOuterProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                  const llvm::Twine &instName = "") override final;

  // Create matrix determinant operation.
  llvm::Value *CreateDeterminant(llvm::Value *const matrix, const llvm::Twine &instName = "") override final;

  // Create matrix inverse operation.
  llvm::Value *CreateMatrixInverse(llvm::Value *const matrix, const llvm::Twine &instName = "") override final;

private:
  MatrixBuilder() = delete;
  MatrixBuilder(const MatrixBuilder &) = delete;
  MatrixBuilder &operator=(const MatrixBuilder &) = delete;

  // Helper function for determinant calculation
  llvm::Value *determinant(llvm::ArrayRef<llvm::Value *> elements, unsigned order);

  // Get submatrix by deleting specified row and column
  void getSubmatrix(llvm::ArrayRef<llvm::Value *> matrix, llvm::MutableArrayRef<llvm::Value *> submatrix,
                    unsigned order, unsigned rowToDelete, unsigned columnToDelete);
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class MiscBuilder : virtual public BuilderImplBase {
public:
  MiscBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

  // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
  // the current output primitive in the specified output-primitive stream.
  llvm::Instruction *CreateEmitVertex(unsigned streamId) override final;

  // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
  llvm::Instruction *CreateEndPrimitive(unsigned streamId) override final;

  // Create a workgroup control barrier.
  llvm::Instruction *CreateBarrier() override final;

  // Create a "kill". Only allowed in a fragment shader.
  llvm::Instruction *CreateKill(const llvm::Twine &instName) override final;

  // Create a "readclock".
  llvm::Instruction *CreateReadClock(bool realtime, const llvm::Twine &instName) override final;

  // Create derivative calculation on float or vector of float or half
  llvm::Value *CreateDerivative(llvm::Value *value, bool isDirectionY, bool isFine,
                                const llvm::Twine &instName = "") override final;

  // Create a demote to helper invocation operation. Only allowed in a fragment shader.
  llvm::Instruction *CreateDemoteToHelperInvocation(const llvm::Twine &instName) override final;

  // Create a helper invocation query. Only allowed in a fragment shader.
  llvm::Value *CreateIsHelperInvocation(const llvm::Twine &instName) override final;

  // In the task shader, emit the current values of all per-task output variables to the current task output by
  // specifying the group count XYZ of the launched child mesh tasks.
  llvm::Instruction *CreateEmitMeshTasks(llvm::Value *groupCountX, llvm::Value *groupCountY, llvm::Value *groupCountZ,
                                         const llvm::Twine &instName = "") override final;

  // In the mesh shader, set the actual output size of the primitives and vertices that the mesh shader workgroup will
  // emit upon completion.
  llvm::Instruction *CreateSetMeshOutputs(llvm::Value *vertexCount, llvm::Value *primitiveCount,
                                          const llvm::Twine &instName = "") override final;

private:
  MiscBuilder() = delete;
  MiscBuilder(const MiscBuilder &) = delete;
  MiscBuilder &operator=(const MiscBuilder &) = delete;
};

// =====================================================================================================================
// Builder implementation subclass for subgroup operations
class SubgroupBuilder : virtual public BuilderImplBase {
public:
  SubgroupBuilder(LgcContext *builderContext) : BuilderImplBase(builderContext) {}

  // Create a get wave size query.
  llvm::Value *CreateGetWaveSize(const llvm::Twine &instName) override final;

  // Create a get subgroup size query.
  llvm::Value *CreateGetSubgroupSize(const llvm::Twine &instName) override final;

  // Create a subgroup elect.
  llvm::Value *CreateSubgroupElect(const llvm::Twine &instName) override final;

  // Create a subgroup all.
  llvm::Value *CreateSubgroupAll(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup any
  llvm::Value *CreateSubgroupAny(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup all equal.
  llvm::Value *CreateSubgroupAllEqual(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup broadcast.
  llvm::Value *CreateSubgroupBroadcast(llvm::Value *const value, llvm::Value *const index,
                                       const llvm::Twine &instName) override final;

  // Create a subgroup broadcast that may have non-uniform index.
  llvm::Value *CreateSubgroupBroadcastWaterfall(llvm::Value *const value, llvm::Value *const index,
                                                const llvm::Twine &instName) override final;

  // Create a subgroup broadcast first.
  llvm::Value *CreateSubgroupBroadcastFirst(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup ballot.
  llvm::Value *CreateSubgroupBallot(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup inverse ballot.
  llvm::Value *CreateSubgroupInverseBallot(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup ballot bit extract.
  llvm::Value *CreateSubgroupBallotBitExtract(llvm::Value *const value, llvm::Value *const index,
                                              const llvm::Twine &instName) override final;

  // Create a subgroup ballot bit count.
  llvm::Value *CreateSubgroupBallotBitCount(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup ballot inclusive bit count.
  llvm::Value *CreateSubgroupBallotInclusiveBitCount(llvm::Value *const value,
                                                     const llvm::Twine &instName) override final;

  // Create a subgroup ballot exclusive bit count.
  llvm::Value *CreateSubgroupBallotExclusiveBitCount(llvm::Value *const value,
                                                     const llvm::Twine &instName) override final;

  // Create a subgroup ballot find least significant bit.
  llvm::Value *CreateSubgroupBallotFindLsb(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup ballot find most significant bit.
  llvm::Value *CreateSubgroupBallotFindMsb(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup shuffle.
  llvm::Value *CreateSubgroupShuffle(llvm::Value *const value, llvm::Value *const index,
                                     const llvm::Twine &instName) override final;

  // Create a subgroup shuffle xor.
  llvm::Value *CreateSubgroupShuffleXor(llvm::Value *const value, llvm::Value *const mask,
                                        const llvm::Twine &instName) override final;

  // Create a subgroup shuffle up.
  llvm::Value *CreateSubgroupShuffleUp(llvm::Value *const value, llvm::Value *const delta,
                                       const llvm::Twine &instName) override final;

  // Create a subgroup shuffle down.
  llvm::Value *CreateSubgroupShuffleDown(llvm::Value *const value, llvm::Value *const delta,
                                         const llvm::Twine &instName) override final;

  // Create a subgroup clustered reduction.
  llvm::Value *CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize,
                                                const llvm::Twine &instName) override final;

  // Create a subgroup clustered inclusive scan.
  llvm::Value *CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize,
                                                const llvm::Twine &instName) override final;

  // Create a subgroup clustered exclusive scan.
  llvm::Value *CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize,
                                                const llvm::Twine &instName) override final;

  // Create a subgroup quad broadcast.
  llvm::Value *CreateSubgroupQuadBroadcast(llvm::Value *const value, llvm::Value *const index,
                                           const llvm::Twine &instName) override final;

  // Create a subgroup quad swap horizontal.
  llvm::Value *CreateSubgroupQuadSwapHorizontal(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup quad swap vertical.
  llvm::Value *CreateSubgroupQuadSwapVertical(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup quad swap diagonal.
  llvm::Value *CreateSubgroupQuadSwapDiagonal(llvm::Value *const value, const llvm::Twine &instName) override final;

  // Create a subgroup swizzle quad.
  llvm::Value *CreateSubgroupSwizzleQuad(llvm::Value *const value, llvm::Value *const offset,
                                         const llvm::Twine &instName) override final;

  // Create a subgroup swizzle masked.
  llvm::Value *CreateSubgroupSwizzleMask(llvm::Value *const value, llvm::Value *const mask,
                                         const llvm::Twine &instName) override final;

  // Create a subgroup write invocation.
  llvm::Value *CreateSubgroupWriteInvocation(llvm::Value *const inputValue, llvm::Value *const writeValue,
                                             llvm::Value *const index, const llvm::Twine &instName) override final;

  // Create a subgroup mbcnt.
  llvm::Value *CreateSubgroupMbcnt(llvm::Value *const mask, const llvm::Twine &instName) override final;

private:
  SubgroupBuilder() = delete;
  SubgroupBuilder(const SubgroupBuilder &) = delete;
  SubgroupBuilder &operator=(const SubgroupBuilder &) = delete;

  unsigned getShaderSubgroupSize();
  unsigned getShaderWaveSize();
  llvm::Value *createGroupArithmeticIdentity(GroupArithOp groupArithOp, llvm::Type *const type);
  llvm::Value *createGroupArithmeticOperation(GroupArithOp groupArithOp, llvm::Value *const x, llvm::Value *const y);
  llvm::Value *createDppMov(llvm::Value *const value, DppCtrl dppCtrl, unsigned rowMask, unsigned bankMask,
                            bool boundCtrl);
  llvm::Value *createDppUpdate(llvm::Value *const origValue, llvm::Value *const updateValue, DppCtrl dppCtrl,
                               unsigned rowMask, unsigned bankMask, bool boundCtrl);

  llvm::Value *createPermLane16(llvm::Value *const origValue, llvm::Value *const updateValue, unsigned selectBitsLow,
                                unsigned selectBitsHigh, bool fetchInactive, bool boundCtrl);
  llvm::Value *createPermLaneX16(llvm::Value *const origValue, llvm::Value *const updateValue, unsigned selectBitsLow,
                                 unsigned selectBitsHigh, bool fetchInactive, bool boundCtrl);
  llvm::Value *createPermLane64(llvm::Value *const updateValue);

  llvm::Value *createDsSwizzle(llvm::Value *const value, uint16_t dsPattern);
  llvm::Value *createWwm(llvm::Value *const value);
  llvm::Value *createWqm(llvm::Value *const value);
  llvm::Value *createThreadMask();
  llvm::Value *createThreadMaskedSelect(llvm::Value *const threadMask, uint64_t andMask, llvm::Value *const value1,
                                        llvm::Value *const value2);
  uint16_t getDsSwizzleBitMode(uint8_t xorMask, uint8_t orMask, uint8_t andMask);
  uint16_t getDsSwizzleQuadMode(uint8_t lane0, uint8_t lane1, uint8_t lane2, uint8_t lane3);
  llvm::Value *createGroupBallot(llvm::Value *const value);
};

// =====================================================================================================================
// The Builder implementation, encompassing all the individual builder implementation subclasses
class BuilderImpl final : public ArithBuilder,
                          DescBuilder,
                          ImageBuilder,
                          InOutBuilder,
                          MatrixBuilder,
                          MiscBuilder,
                          SubgroupBuilder {
  friend LgcContext;

public:
  BuilderImpl(LgcContext *builderContext, Pipeline *pipeline);
  ~BuilderImpl() {}

  BuilderImpl() = delete;
  BuilderImpl(const BuilderImpl &) = delete;
  BuilderImpl &operator=(const BuilderImpl &) = delete;
};

} // namespace lgc
