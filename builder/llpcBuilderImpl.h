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
 * @file  llpcBuilderImpl.h
 * @brief LLPC header file: declaration of Llpc::Builder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"

namespace Llpc
{

using namespace llvm;

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : public Builder
{
public:
    BuilderImplBase(LLVMContext& context) : Builder(context) {}

    // Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
    Llpc::Context& getContext() const;

    // Create scalar from dot product of vector
    Value* CreateDotProduct(Value* const pVector1,
                            Value* const pVector2,
                            const Twine& instName = "") override final;

protected:
    // Get whether the context we are building in supports DPP operations.
    bool SupportDpp() const;

    // Get whether the context we are building in support the bpermute operation.
    bool SupportBPermute() const;

#if LLPC_BUILD_GFX10
    // Get whether the context we are building in supports permute lane DPP operations.
    bool SupportPermLaneDpp() const;
#endif

    // Create an "if..endif" or "if..else..endif" structure.
    BranchInst* CreateIf(Value* pCondition, bool wantElse, const Twine& instName);

    // Create a waterfall loop containing the specified instruction.
    Instruction* CreateWaterfallLoop(Instruction*       pNonUniformInst,
                                     ArrayRef<uint32_t> operandIdxs,
                                     const Twine&       instName = "");

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplBase)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplBase)
};

// =====================================================================================================================
// Builder implementation subclass for descriptors
class BuilderImplDesc : virtual public BuilderImplBase
{
public:
    BuilderImplDesc(LLVMContext& context) : BuilderImplBase(context) {}

    // Create a load of a buffer descriptor.
    Value* CreateLoadBufferDesc(uint32_t      descSet,
                                uint32_t      binding,
                                Value*        pDescIndex,
                                bool          isNonUniform,
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
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplDesc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplDesc)

    Value* ScalarizeIfUniform(Value* pValue, bool isNonUniform);
};

// =====================================================================================================================
// Builder implementation subclass for image operations
class BuilderImplImage : virtual public BuilderImplBase
{
public:
    BuilderImplImage(LLVMContext& context) : BuilderImplBase(context) {}

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
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplImage)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplImage)

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
// Builder implementation subclass for matrix operations
class BuilderImplMatrix : virtual public BuilderImplBase
{
public:
    BuilderImplMatrix(LLVMContext& context) : BuilderImplBase(context) {}

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

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMatrix)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMatrix)
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class BuilderImplMisc : virtual public BuilderImplBase
{
public:
    BuilderImplMisc(LLVMContext& context) : BuilderImplBase(context) {}

    // Create a "kill". Only allowed in a fragment shader.
    Instruction* CreateKill(const Twine& instName) override final;

    // Create a "readclock".
    Instruction* CreateReadClock(bool realtime, const Twine& instName) override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMisc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMisc)
};

// =====================================================================================================================
// Builder implementation subclass for subgroup operations
class BuilderImplSubgroup : virtual public BuilderImplBase
{
public:
    BuilderImplSubgroup(LLVMContext& context) : BuilderImplBase(context) {}

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
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplSubgroup)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplSubgroup)

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

#if LLPC_BUILD_GFX10
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
#endif

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
class BuilderImpl final : public BuilderImplDesc,
                                 BuilderImplImage,
                                 BuilderImplMatrix,
                                 BuilderImplMisc,
                                 BuilderImplSubgroup
{
public:
    BuilderImpl(LLVMContext& context) : BuilderImplBase(context),
                                        BuilderImplDesc(context),
                                        BuilderImplImage(context),
                                        BuilderImplMatrix(context),
                                        BuilderImplMisc(context),
                                        BuilderImplSubgroup(context)
    {}
    ~BuilderImpl() {}

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImpl)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImpl)
};

} // Llpc
