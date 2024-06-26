/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
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

// Map vkgc - uint32 -1 is zero extended
static constexpr uint64_t InternalDescriptorSetId = 0x00000000FFFFFFFFull;

// =====================================================================================================================
// Builder implementation class
class BuilderImpl : public BuilderDefs {
public:
  BuilderImpl(Pipeline *pipeline);

  // Create scalar from dot product of vector
  llvm::Value *CreateDotProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                const llvm::Twine &instName = "");

  // Create scalar from integer dot product of vector
  llvm::Value *CreateIntegerDotProduct(llvm::Value *vector1, llvm::Value *vector2, llvm::Value *accumulator,
                                       unsigned flags, const llvm::Twine &instName = "");

  // Create a waterfall loop containing the specified instruction.
  llvm::Instruction *createWaterfallLoop(llvm::Instruction *nonUniformInst, llvm::ArrayRef<unsigned> operandIdxs,
                                         bool scalarizeDescriptorLoads = false, bool useVgprForOperands = false,
                                         const llvm::Twine &instName = "");

  // Set the current shader stage, clamp shader stage to the ShaderStage::Compute
  void setShaderStage(std::optional<ShaderStageEnum> stage) {
    m_shaderStage = (!stage || *stage > ShaderStage::Compute) ? ShaderStage::Compute : stage;
  }

  // Get the LgcContext
  LgcContext *getLgcContext() const { return m_builderContext; }

protected:
  // Get the ShaderModes object.
  ShaderModes *getShaderModes();

  // Get the PipelineState object.
  PipelineState *getPipelineState() const { return m_pipelineState; }

  // Get whether the context we are building in supports ds_bpermute or v_bpermute across all lanes in the wave.
  bool supportWaveWideBPermute(ShaderStageEnum shaderStage) const;

  // Get whether the context we are building in supports permute lane 64 DPP operations.
  bool supportPermLane64Dpp() const;

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

  llvm::Value *CreateMapToSimpleType(BuilderBase::MapToSimpleTypeFunc mapFunc, llvm::ArrayRef<llvm::Value *> mappedArgs,
                                     llvm::ArrayRef<llvm::Value *> passthroughArgs,
                                     MapToSimpleMode simpleMode = MapToSimpleMode::Int32) {
    return BuilderBase::get(*this).CreateMapToSimpleType(mapFunc, mappedArgs, passthroughArgs, simpleMode);
  }

  PipelineState *m_pipelineState = nullptr;     // Pipeline state
  std::optional<ShaderStageEnum> m_shaderStage; // Current shader stage being built.

private:
  BuilderImpl() = delete;
  BuilderImpl(const BuilderImpl &) = delete;
  BuilderImpl &operator=(const BuilderImpl &) = delete;

  LgcContext *m_builderContext; // Builder context

  // -------------------------------------------------------------------------------------------------------------------
  // Arithmetic operations
public:
  // Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
  // the given cube map texture coordinates.
  llvm::Value *CreateCubeFaceCoord(llvm::Value *coord, const llvm::Twine &instName = "");

  // Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
  // the given cube map texture coordinates.
  llvm::Value *CreateCubeFaceIndex(llvm::Value *coord, const llvm::Twine &instName = "");

  // Create scalar or vector FP truncate operation with rounding mode.
  llvm::Value *CreateFpTruncWithRounding(llvm::Value *value, llvm::Type *destTy, llvm::RoundingMode roundingMode,
                                         const llvm::Twine &instName = "");

  // Create quantize operation.
  llvm::Value *CreateQuantizeToFp16(llvm::Value *value, const llvm::Twine &instName = "");

  // Create signed integer or FP modulo operation.
  llvm::Value *CreateSMod(llvm::Value *dividend, llvm::Value *divisor, const llvm::Twine &instName = "");
  llvm::Value *CreateFMod(llvm::Value *dividend, llvm::Value *divisor, const llvm::Twine &instName = "");

  // Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
  llvm::Value *CreateFma(llvm::Value *a, llvm::Value *b, llvm::Value *c, const llvm::Twine &instName = "");

  // Methods to create trig and exponential operations.
  llvm::Value *CreateTan(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateASin(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateACos(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateATan(llvm::Value *yOverX, const llvm::Twine &instName = "");
  llvm::Value *CreateATan2(llvm::Value *y, llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateSinh(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateCosh(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateTanh(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateASinh(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateACosh(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateATanh(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreatePower(llvm::Value *x, llvm::Value *y, const llvm::Twine &instName = "");
  llvm::Value *CreateExp(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateLog(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateSqrt(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateInverseSqrt(llvm::Value *x, const llvm::Twine &instName = "");

  // General arithmetic operations.
  llvm::Value *CreateSAbs(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateFSign(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateSSign(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateFract(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateSmoothStep(llvm::Value *edge0, llvm::Value *edge1, llvm::Value *x,
                                const llvm::Twine &instName = "");
  llvm::Value *CreateLdexp(llvm::Value *x, llvm::Value *exp, const llvm::Twine &instName = "");
  llvm::Value *CreateExtractSignificand(llvm::Value *value, const llvm::Twine &instName = "");
  llvm::Value *CreateExtractExponent(llvm::Value *value, const llvm::Twine &instName = "");
  llvm::Value *CreateCrossProduct(llvm::Value *x, llvm::Value *y, const llvm::Twine &instName = "");
  llvm::Value *CreateNormalizeVector(llvm::Value *x, const llvm::Twine &instName = "");
  llvm::Value *CreateFaceForward(llvm::Value *n, llvm::Value *i, llvm::Value *nref, const llvm::Twine &instName = "");
  llvm::Value *CreateReflect(llvm::Value *i, llvm::Value *n, const llvm::Twine &instName = "");
  llvm::Value *CreateRefract(llvm::Value *i, llvm::Value *n, llvm::Value *eta, const llvm::Twine &instName = "");

  // Create "fclamp" operation.
  llvm::Value *CreateFClamp(llvm::Value *x, llvm::Value *minVal, llvm::Value *maxVal, const llvm::Twine &instName = "");

  // FP min/max
  llvm::Value *CreateFMin(llvm::Value *value1, llvm::Value *value2, const llvm::Twine &instName = "");
  llvm::Value *CreateFMax(llvm::Value *value1, llvm::Value *value2, const llvm::Twine &instName = "");

  // Methods for trinary min/max/mid.
  llvm::Value *CreateFMin3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                           const llvm::Twine &instName = "");
  llvm::Value *CreateFMax3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                           const llvm::Twine &instName = "");
  llvm::Value *CreateFMid3(llvm::Value *value1, llvm::Value *value2, llvm::Value *value3,
                           const llvm::Twine &instName = "");

  // Create "isInf" operation: return true if the supplied FP (or vector) value is infinity
  llvm::Value *CreateIsInf(llvm::Value *x, const llvm::Twine &instName = "");

  // Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
  llvm::Value *CreateIsNaN(llvm::Value *x, const llvm::Twine &instName = "");

  // Create an "insert bitfield" operation for a (vector of) integer type.
  llvm::Value *CreateInsertBitField(llvm::Value *base, llvm::Value *insert, llvm::Value *offset, llvm::Value *count,
                                    const llvm::Twine &instName = "");

  // Create an "extract bitfield " operation for a (vector of) i32.
  llvm::Value *CreateExtractBitField(llvm::Value *base, llvm::Value *offset, llvm::Value *count, bool isSigned,
                                     const llvm::Twine &instName = "");

  // Create "find MSB" operation for a (vector of) signed int.
  llvm::Value *CreateFindSMsb(llvm::Value *value, const llvm::Twine &instName = "");

  // Create "count leading sign bits" operation for a (vector of) signed int.
  llvm::Value *CreateCountLeadingSignBits(llvm::Value *value, const llvm::Twine &instName = "");

  // Create "Masked Sum of Absolute Differences" operation.
  llvm::Value *CreateMsad4(llvm::Value *src, llvm::Value *ref, llvm::Value *accum, const llvm::Twine &instName = "");

  // Create fdot2_f16 + f32 operation.
  llvm::Value *CreateFDot2(llvm::Value *a, llvm::Value *b, llvm::Value *scalar, llvm::Value *clamp,
                           const llvm::Twine &instName = "");

  // Create "fmix" operation.
  llvm::Value *createFMix(llvm::Value *x, llvm::Value *y, llvm::Value *a, const llvm::Twine &instName = "");

private:
  // Common code for asin and acos
  llvm::Value *aSinACosCommon(llvm::Value *x, llvm::Constant *coefP0, llvm::Constant *coefP1);

  // Generate FP division, using fast fdiv for float to bypass optimization.
  llvm::Value *fDivFast(llvm::Value *numerator, llvm::Value *denominator);

  // Helper method to create call to llvm.is.fpclass. This is not exposed outside of ArithBuilder.
  llvm::Value *createIsFPClass(llvm::Value *value, unsigned flags, const llvm::Twine &instName = "");

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

  // -------------------------------------------------------------------------------------------------------------------
  // Descriptor operations
public:
  // Create a buffer descriptor.
  llvm::Value *CreateBufferDesc(uint64_t descSet, unsigned binding, llvm::Value *descIndex, unsigned flags,
                                const llvm::Twine &instName = "") {
    return createBufferDesc(descSet, binding, descIndex, flags, 0);
  }

  // Create a strided buffer descriptor.
  llvm::Value *CreateStridedBufferDesc(uint64_t descSet, unsigned binding, llvm::Value *descIndex, unsigned flags,
                                       unsigned stride, const llvm::Twine &instName = "") {
    return createBufferDesc(descSet, binding, descIndex, flags, stride);
  }

  // Create a get of the stride (in bytes) of a descriptor.
  llvm::Value *CreateGetDescStride(ResourceNodeType concreteType, ResourceNodeType abstractType, uint64_t descSet,
                                   unsigned binding, const llvm::Twine &instName = "");

  // Create a pointer to a descriptor.
  llvm::Value *CreateGetDescPtr(ResourceNodeType concreteType, ResourceNodeType abstractType, uint64_t descSet,
                                unsigned binding, const llvm::Twine &instName = "");

  // Create a load of the push constants pointer.
  llvm::Value *CreateLoadPushConstantsPtr(const llvm::Twine &instName = "");

  // Calculate a buffer descriptor for an inline buffer
  llvm::Value *buildInlineBufferDesc(llvm::Value *descPtr, unsigned stride);

  // Check whether vertex buffer descriptors are in a descriptor array binding instead of the VertexBufferTable.
  bool useVertexBufferDescArray();

  // Build buffer compact descriptor
  llvm::Value *buildBufferCompactDesc(llvm::Value *desc, unsigned stride);

private:
  // Get a struct containing the pointer and byte stride for a descriptor
  llvm::Value *getDescPtrAndStride(ResourceNodeType resType, uint64_t descSet, unsigned binding,
                                   const ResourceNode *topNode, const ResourceNode *node, bool shadow);

  // Get the stride (in bytes) of a descriptor.
  llvm::Value *getStride(ResourceNodeType descType, const ResourceNode *node);

  // Get a pointer to a descriptor, as a pointer to i8
  llvm::Value *getDescPtr(ResourceNodeType concreteType, const ResourceNode *topNode, const ResourceNode *node,
                          unsigned binding);

  llvm::Value *scalarizeIfUniform(llvm::Value *value, bool isNonUniform);

  // Create a buffer descriptor.
  llvm::Value *createBufferDesc(uint64_t descSet, unsigned binding, llvm::Value *descIndex, unsigned flags,
                                unsigned stride, const llvm::Twine &instName = "");
  // -------------------------------------------------------------------------------------------------------------------
  // Image operations

  friend class YCbCrConverter;

public:
  // Create an image load.
  llvm::Value *CreateImageLoad(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                               llvm::Value *coord, llvm::Value *mipLevel, const llvm::Twine &instName = "");

  // Create an image load with F-mask.
  llvm::Value *CreateImageLoadWithFmask(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                        llvm::Value *fmaskDesc, llvm::Value *coord, llvm::Value *sampleNum,
                                        const llvm::Twine &instName = "");

  // Create an image store.
  llvm::Value *CreateImageStore(llvm::Value *texel, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                llvm::Value *coord, llvm::Value *mipLevel, const llvm::Twine &instName = "");

  // Create an image sample.
  llvm::Value *CreateImageSample(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                 llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                 const llvm::Twine &instName = "");

  // Create an image sample with conversion.
  llvm::Value *CreateImageSampleConvert(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                        llvm::Value *convertingSamplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                        const llvm::Twine &instName = "");

  // Create an image sample with YCbCr conversion.
  llvm::Value *CreateImageSampleConvertYCbCr(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                             llvm::Value *convertingSamplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                             const llvm::Twine &instName = "");

  // Create an image gather
  llvm::Value *CreateImageGather(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                 llvm::Value *samplerDesc, llvm::ArrayRef<llvm::Value *> address,
                                 const llvm::Twine &instName = "");

  // Create an image atomic operation other than compare-and-swap.
  llvm::Value *CreateImageAtomic(unsigned atomicOp, unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                 llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                 const llvm::Twine &instName = "");

  // Create an image atomic compare-and-swap.
  llvm::Value *CreateImageAtomicCompareSwap(unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                            llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                            llvm::Value *comparatorValue, const llvm::Twine &instName = "");

  // Create a query of the number of mipmap levels in an image. Returns an i32 value.
  llvm::Value *CreateImageQueryLevels(unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                      const llvm::Twine &instName = "");

  // Create a query of the number of samples in an image. Returns an i32 value.
  llvm::Value *CreateImageQuerySamples(unsigned dim, unsigned flags, llvm::Value *imageDesc,
                                       const llvm::Twine &instName = "");

  // Create a query of size of an image at the specified LOD
  llvm::Value *CreateImageQuerySize(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *lod,
                                    const llvm::Twine &instName = "");

  // Create a get of the LOD that would be used for an image sample with the given coordinates
  // and implicit LOD.
  llvm::Value *CreateImageGetLod(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *samplerDesc,
                                 llvm::Value *coord, const llvm::Twine &instName = "");

  // Create a query of the sample position of given sample id in an image.
  llvm::Value *CreateImageGetSamplePosition(unsigned dim, unsigned flags, llvm::Value *imageDesc, llvm::Value *sampleId,
                                            const llvm::Twine &instName = "");

  // Create a ray intersect result with specified node in BVH buffer
  llvm::Value *CreateImageBvhIntersectRay(llvm::Value *nodePtr, llvm::Value *extent, llvm::Value *origin,
                                          llvm::Value *direction, llvm::Value *invDirection, llvm::Value *imageDesc,
                                          const llvm::Twine &instName = "");

private:
  // Common code to create an image sample or gather.
  llvm::Value *CreateImageSampleGather(llvm::Type *resultTy, unsigned dim, unsigned flags, llvm::Value *coord,
                                       llvm::Value *imageDesc, llvm::Value *samplerDesc,
                                       llvm::ArrayRef<llvm::Value *> address, const llvm::Twine &instName,
                                       bool isSample);

  // Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
  llvm::Value *CreateImageAtomicCommon(unsigned atomicOp, unsigned dim, unsigned flags, llvm::AtomicOrdering ordering,
                                       llvm::Value *imageDesc, llvm::Value *coord, llvm::Value *inputValue,
                                       llvm::Value *comparatorValue, const llvm::Twine &instName = "");

  // Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
  // modifying if necessary.
  // Returns possibly modified image dimension.
  unsigned prepareCoordinate(unsigned dim, llvm::Value *coord, llvm::Value *projective, llvm::Value *derivativeX,
                             llvm::Value *derivativeY, llvm::SmallVectorImpl<llvm::Value *> &outCoords,
                             llvm::SmallVectorImpl<llvm::Value *> &outDerivatives);

  // For a cubearray with integer coordinates, combine the face and slice into a single component.
  void combineCubeArrayFaceAndSlice(llvm::Value *coord, llvm::SmallVectorImpl<llvm::Value *> &coords);

  // Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
  llvm::Value *handleFragCoordViewIndex(llvm::Value *coord, unsigned flags, unsigned &dim);

  // Fix image descriptor before an operation that reads the image
  llvm::Value *fixImageDescForRead(llvm::Value *imageDesc);

  // Enforce readfirstlane on the image or sampler descriptors
  void enforceReadFirstLane(llvm::Instruction *imageInst, unsigned descIdx);

  // Modify sampler descriptor to force set trunc_coord as 0 for gather4 instruction.
  llvm::Value *modifySamplerDescForGather(llvm::Value *samplerDesc);

  // Transform 32-bit image descriptor pointer to a i32 type or a descriptor load instruction.
  llvm::Value *transformImageDesc(llvm::Value *imageDesc, bool mustLoad, bool isTexelBuffer, llvm::Type *texelType);

  // Transform 32-bit sampler descriptor pointer to a i32 type or a descriptor load instruction.
  llvm::Value *transformSamplerDesc(llvm::Value *samplerDesc);

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

  enum ImgFmtGfx11 {
    IMG_FMT_8_8_8_8_UNORM__GFX104PLUS = 42,
    IMG_FMT_GB_GR_UNORM__GFX104PLUS = 82,
    IMG_FMT_BG_RG_UNORM__GFX104PLUS = 86,
  };

  static const unsigned AtomicOpCompareSwap = 1;

  // -------------------------------------------------------------------------------------------------------------------
  // Input/output operations
public:
  // Create a read of (part of) a user input value.
  llvm::Value *CreateReadGenericInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                      llvm::Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                      llvm::Value *vertexIndex, const llvm::Twine &instName = "");

  // Create a read of (part of) a perVertex input value.
  llvm::Value *CreateReadPerVertexInput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                        llvm::Value *elemIdx, unsigned locationCount, InOutInfo inputInfo,
                                        llvm::Value *vertexIndex, const llvm::Twine &instName = "");

  // Create a read of (part of) a user output value.
  llvm::Value *CreateReadGenericOutput(llvm::Type *resultTy, unsigned location, llvm::Value *locationOffset,
                                       llvm::Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                       llvm::Value *vertexIndex, const llvm::Twine &instName = "");

  // Create a write of (part of) a user output value.
  llvm::Instruction *CreateWriteGenericOutput(llvm::Value *valueToWrite, unsigned location, llvm::Value *locationOffset,
                                              llvm::Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                              llvm::Value *vertexOrPrimitiveIndex);

  // Create a write to an XFB (transform feedback / streamout) buffer.
  llvm::Instruction *CreateWriteXfbOutput(llvm::Value *valueToWrite, bool isBuiltIn, unsigned location,
                                          unsigned xfbBuffer, unsigned xfbStride, llvm::Value *xfbOffset,
                                          InOutInfo outputInfo);

  // Create a read of barycoord input value.
  llvm::Value *CreateReadBaryCoord(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *auxInterpValue,
                                   const llvm::Twine &instName = "");

  // Create a read of (part of) a built-in input value.
  llvm::Value *CreateReadBuiltInInput(BuiltInKind builtIn, InOutInfo inputInfo, llvm::Value *vertexIndex,
                                      llvm::Value *index, const llvm::Twine &instName = "");

  // Create a read of (part of) an output built-in value.
  llvm::Value *CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, llvm::Value *vertexIndex,
                                       llvm::Value *index, const llvm::Twine &instName = "");

  // Create a write of (part of) a built-in output value.
  llvm::Instruction *CreateWriteBuiltInOutput(llvm::Value *valueToWrite, BuiltInKind builtIn, InOutInfo outputInfo,
                                              llvm::Value *vertexOrPrimitiveIndex, llvm::Value *index);

private:
  // Read (a part of) a generic (user) input/output value.
  llvm::Value *readGenericInputOutput(bool isOutput, llvm::Type *resultTy, unsigned location,
                                      llvm::Value *locationOffset, llvm::Value *elemIdx, unsigned locationCount,
                                      InOutInfo inOutInfo, llvm::Value *vertexIndex, const llvm::Twine &instName = "");

  // Mark usage for a generic (user) input or output
  void markGenericInputOutputUsage(bool isOutput, unsigned location, unsigned locationCount, InOutInfo &inOutInfo,
                                   bool hasVertexOrPrimIndex, bool directlyMapLocations = false);

  // Mark interpolation info for FS input.
  void markInterpolationInfo(InOutInfo &interpInfo);

  // Mark fragment output type
  void markFsOutputType(llvm::Type *outputTy, unsigned location, InOutInfo outputInfo);

  // Try to fold constant location offset if possible.
  bool foldConstantLocationOffset(llvm::Type *inOutTy, unsigned &location, llvm::Value *&locationOffset,
                                  llvm::Value *elemIdx, unsigned &locationCount, InOutInfo &inOutInfo);

  std::tuple<unsigned, llvm::Value *> getInterpModeAndValue(InOutInfo inputInfo, llvm::Value *auxInterpValue);
  llvm::Value *evalIjOffsetSmooth(llvm::Value *offset);
  llvm::Value *adjustIj(llvm::Value *value, llvm::Value *offset);

  // Read (part of) a built-in value
  llvm::Value *readBuiltIn(bool isOutput, BuiltInKind builtIn, InOutInfo inOutInfo, llvm::Value *vertexIndex,
                           llvm::Value *index, const llvm::Twine &instName = "");

  // Reorder the barycoord
  llvm::Value *normalizeBaryCoord(InOutInfo inputInfo, llvm::Value *ijCoord);

  // Get provoking vertex value
  void getProvokingVertexInfo(llvm::Value **isOne, llvm::Value **isTwo);

  // Read and directly handle certain built-ins that are common between shader stages
  llvm::Value *readCommonBuiltIn(BuiltInKind builtIn, llvm::Type *resultTy, const llvm::Twine &instName = "");

  // Read compute/task shader input
  llvm::Value *readCsBuiltIn(BuiltInKind builtIn, const llvm::Twine &instName = "");

  // Read vertex shader input
  llvm::Value *readVsBuiltIn(BuiltInKind builtIn, const llvm::Twine &instName = "");

  // Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
  llvm::Type *getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo);

  // Mark usage of a built-in input
  void markBuiltInInputUsage(BuiltInKind &builtIn, unsigned arraySize, InOutInfo inOutInfo);

  // Mark usage of a built-in output
  void markBuiltInOutputUsage(BuiltInKind builtIn, unsigned arraySize, unsigned streamId);

#ifndef NDEBUG
  // Get a bitmask of which shader stages are valid for a built-in to be an input or output of
  ShaderStageMask getBuiltInValidMask(BuiltInKind builtIn, bool isOutput);

  // Determine whether a built-in is an input for a particular shader stage.
  bool isBuiltInInput(BuiltInKind builtIn);

  // Determine whether a built-in is an output for a particular shader stage.
  bool isBuiltInOutput(BuiltInKind builtIn);
#endif

  // -------------------------------------------------------------------------------------------------------------------
  // Matrix operations
public:
  // Create a matrix transpose.
  llvm::Value *CreateTransposeMatrix(llvm::Value *const matrix, const llvm::Twine &instName = "");

  // Create matrix multiplication: matrix times scalar, resulting in matrix
  llvm::Value *CreateMatrixTimesScalar(llvm::Value *const matrix, llvm::Value *const scalar,
                                       const llvm::Twine &instName = "");

  // Create matrix multiplication: vector times matrix, resulting in vector
  llvm::Value *CreateVectorTimesMatrix(llvm::Value *const vector, llvm::Value *const matrix,
                                       const llvm::Twine &instName = "");

  // Create matrix multiplication: matrix times vector, resulting in vector
  llvm::Value *CreateMatrixTimesVector(llvm::Value *const matrix, llvm::Value *const vector,
                                       const llvm::Twine &instName = "");

  // Create matrix multiplication:  matrix times matrix, resulting in matrix
  llvm::Value *CreateMatrixTimesMatrix(llvm::Value *const matrix1, llvm::Value *const matrix2,
                                       const llvm::Twine &instName = "");

  // Create vector outer product operation, resulting in matrix
  llvm::Value *CreateOuterProduct(llvm::Value *const vector1, llvm::Value *const vector2,
                                  const llvm::Twine &instName = "");

  // Create matrix determinant operation.
  llvm::Value *CreateDeterminant(llvm::Value *const matrix, const llvm::Twine &instName = "");

  // Create matrix inverse operation.
  llvm::Value *CreateMatrixInverse(llvm::Value *const matrix, const llvm::Twine &instName = "");

private:
  // Helper function for determinant calculation
  llvm::Value *determinant(llvm::ArrayRef<llvm::Value *> elements, unsigned order);

  // Get submatrix by deleting specified row and column
  void getSubmatrix(llvm::ArrayRef<llvm::Value *> matrix, llvm::MutableArrayRef<llvm::Value *> submatrix,
                    unsigned order, unsigned rowToDelete, unsigned columnToDelete);

  // -------------------------------------------------------------------------------------------------------------------
  // Misc. operations
public:
  // In the GS, emit the current values of outputs (as written by CreateWriteBuiltIn and CreateWriteOutput) to
  // the current output primitive in the specified output-primitive stream.
  llvm::Instruction *CreateEmitVertex(unsigned streamId);

  // In the GS, finish the current primitive and start a new one in the specified output-primitive stream.
  llvm::Instruction *CreateEndPrimitive(unsigned streamId);

  // Create a workgroup control barrier.
  llvm::Instruction *CreateBarrier();

  // Create a "kill". Only allowed in a fragment shader.
  llvm::Instruction *CreateKill(const llvm::Twine &instName = "");

  // Create a "readclock".
  llvm::Instruction *CreateReadClock(bool realtime, const llvm::Twine &instName = "");

  // Create derivative calculation on float or vector of float or half
  llvm::Value *CreateDerivative(llvm::Value *value, bool isDirectionY, bool isFine, const llvm::Twine &instName = "");

  // Create a demote to helper invocation operation. Only allowed in a fragment shader.
  llvm::Instruction *CreateDemoteToHelperInvocation(const llvm::Twine &instName = "");

  // Create a helper invocation query. Only allowed in a fragment shader.
  llvm::Value *CreateIsHelperInvocation(const llvm::Twine &instName = "");

  // -------------------------------------------------------------------------------------------------------------------
  // Builder implementation subclass for subgroup operations
public:
  // Create a get wave size query.
  llvm::Value *CreateGetWaveSize(const llvm::Twine &instName = "");

  // Create a get subgroup size query.
  llvm::Value *CreateGetSubgroupSize(const llvm::Twine &instName = "");

  // Create a subgroup broadcast.
  llvm::Value *CreateSubgroupBroadcast(llvm::Value *const value, llvm::Value *const index,
                                       const llvm::Twine &instName = "");

  // Create a subgroup broadcast that may have non-uniform index.
  llvm::Value *CreateSubgroupBroadcastWaterfall(llvm::Value *const value, llvm::Value *const index,
                                                const llvm::Twine &instName = "");

  // Create a subgroup broadcast first.
  llvm::Value *CreateSubgroupBroadcastFirst(llvm::Value *const value, const llvm::Twine &instName = "") {
    return createSubgroupBroadcastFirst(value, m_shaderStage.value(), instName);
  }

  // Create a subgroup ballot.
  llvm::Value *CreateSubgroupBallot(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup inverse ballot.
  llvm::Value *CreateSubgroupInverseBallot(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup ballot bit extract.
  llvm::Value *CreateSubgroupBallotBitExtract(llvm::Value *const value, llvm::Value *const index,
                                              const llvm::Twine &instName = "");

  // Create a subgroup ballot bit count.
  llvm::Value *CreateSubgroupBallotBitCount(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup ballot inclusive bit count.
  llvm::Value *CreateSubgroupBallotInclusiveBitCount(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup ballot exclusive bit count.
  llvm::Value *CreateSubgroupBallotExclusiveBitCount(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup ballot find least significant bit.
  llvm::Value *CreateSubgroupBallotFindLsb(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup ballot find most significant bit.
  llvm::Value *CreateSubgroupBallotFindMsb(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup shuffle.
  llvm::Value *CreateSubgroupShuffle(llvm::Value *const value, llvm::Value *const index,
                                     const llvm::Twine &instName = "") {
    return createSubgroupShuffle(value, index, m_shaderStage.value(), instName);
  }

  // Create a subgroup shuffle xor.
  llvm::Value *CreateSubgroupShuffleXor(llvm::Value *const value, llvm::Value *const mask,
                                        const llvm::Twine &instName = "");

  // Create a subgroup shuffle up.
  llvm::Value *CreateSubgroupShuffleUp(llvm::Value *const value, llvm::Value *const delta,
                                       const llvm::Twine &instName = "");

  // Create a subgroup shuffle down.
  llvm::Value *CreateSubgroupShuffleDown(llvm::Value *const value, llvm::Value *const delta,
                                         const llvm::Twine &instName = "");

  // Create a subgroup clustered reduction.
  llvm::Value *CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize, const llvm::Twine &instName = "");

  // Create a subgroup clustered inclusive scan.
  llvm::Value *CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize, const llvm::Twine &instName = "");

  // Create a subgroup clustered exclusive scan.
  llvm::Value *CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                llvm::Value *const clusterSize, const llvm::Twine &instName = "");

  // Create a subgroup clustered multi exclusive scan.
  llvm::Value *CreateSubgroupClusteredMultiExclusive(GroupArithOp groupArithOp, llvm::Value *const value,
                                                     llvm::Value *const mask, const llvm::Twine &instName = "");

  // Create a quad broadcast.
  llvm::Value *CreateSubgroupQuadBroadcast(llvm::Value *const value, llvm::Value *const index, bool inWQM = true,
                                           const llvm::Twine &instName = "");

  // Create a quad swap horizontal.
  llvm::Value *CreateSubgroupQuadSwapHorizontal(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a quad swap vertical.
  llvm::Value *CreateSubgroupQuadSwapVertical(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a quad swap diagonal.
  llvm::Value *CreateSubgroupQuadSwapDiagonal(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a subgroup swizzle quad.
  llvm::Value *CreateSubgroupSwizzleQuad(llvm::Value *const value, llvm::Value *const offset,
                                         const llvm::Twine &instName = "");

  // Create a subgroup swizzle masked.
  llvm::Value *CreateSubgroupSwizzleMask(llvm::Value *const value, llvm::Value *const mask,
                                         const llvm::Twine &instName = "");

  // Create a subgroup write invocation.
  llvm::Value *CreateSubgroupWriteInvocation(llvm::Value *const inputValue, llvm::Value *const writeValue,
                                             llvm::Value *const index, const llvm::Twine &instName = "");

  // Create a subgroup mbcnt.
  llvm::Value *CreateSubgroupMbcnt(llvm::Value *const mask, const llvm::Twine &instName = "");

  // Create a subgroup partition.
  llvm::Value *CreateSubgroupPartition(llvm::Value *const value, const llvm::Twine &instName = "");

  // Create a Quad ballot.
  llvm::Value *CreateQuadBallot(llvm::Value *const value, bool requireFullQuads, const llvm::Twine &instName = "");

  // Create a quad all
  llvm::Value *CreateQuadAll(llvm::Value *const value, bool requireFullQuads, const llvm::Twine &instName = "");

  // Create a quad all
  llvm::Value *CreateQuadAny(llvm::Value *const value, bool requireFullQuads, const llvm::Twine &instName = "");

private:
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
  llvm::Value *createReadFirstLane(llvm::Value *const updateValue);

  llvm::Value *createDsSwizzle(llvm::Value *const value, uint16_t dsPattern);
  llvm::Value *createWwm(llvm::Value *const value);
  llvm::Value *createWqm(llvm::Value *const value) { return createWqm(value, m_shaderStage.value()); }
  llvm::Value *createThreadMask();
  llvm::Value *createThreadMaskedSelect(llvm::Value *const threadMask, uint64_t andMask, llvm::Value *const value1,
                                        llvm::Value *const value2);
  uint16_t getDsSwizzleBitMode(uint8_t xorMask, uint8_t orMask, uint8_t andMask);
  uint16_t getDsSwizzleQuadMode(uint8_t lane0, uint8_t lane1, uint8_t lane2, uint8_t lane3);

  llvm::Value *createGroupBallot(llvm::Value *const value);

protected:
  // The subgroup operation with explicit shader stage as parameter.
  llvm::Value *createFindMsb(llvm::Value *const mask);
  llvm::Value *createGroupBallot(llvm::Value *const value, ShaderStageEnum shaderStage);
  llvm::Value *createSubgroupBroadcastFirst(llvm::Value *const value, ShaderStageEnum shaderStage,
                                            const llvm::Twine &instName);
  llvm::Value *createSubgroupShuffle(llvm::Value *const value, llvm::Value *const index, ShaderStageEnum shaderStage,
                                     const llvm::Twine &instName);
  llvm::Value *createWqm(llvm::Value *const value, ShaderStageEnum shaderStage);
};

} // namespace lgc
