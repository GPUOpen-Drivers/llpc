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

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : public Builder
{
public:
    BuilderImplBase(llvm::LLVMContext& context) : Builder(context) {}

    // Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
    Llpc::Context& getContext() const;

    // Create a waterfall loop containing the specified instruction.
    // TODO: This will become a protected method once the image rework is complete, as it will
    // no longer be part of the public Builder interface.
    llvm::Instruction* CreateWaterfallLoop(llvm::Instruction*       pNonUniformInst,
                                           llvm::ArrayRef<uint32_t> operandIdxs,
                                           const llvm::Twine&       instName = "") override final;

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
    llvm::BranchInst* CreateIf(llvm::Value* pCondition, bool wantElse, const llvm::Twine& instName);

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplBase)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplBase)
};

// =====================================================================================================================
// Builder implementation subclass for descriptors
class BuilderImplDesc : virtual public BuilderImplBase
{
public:
    BuilderImplDesc(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a load of a buffer descriptor.
    llvm::Value* CreateLoadBufferDesc(uint32_t            descSet,
                                      uint32_t            binding,
                                      llvm::Value*        pDescIndex,
                                      bool                isNonUniform,
                                      llvm::Type*         pPointeeTy,
                                      const llvm::Twine&  instName) override final;

    // Add index onto pointer to image/sampler/texelbuffer/F-mask array of descriptors.
    llvm::Value* CreateIndexDescPtr(llvm::Value*        pDescPtr,
                                    llvm::Value*        pIndex,
                                    bool                isNonUniform,
                                    const llvm::Twine&  instName) override final;

    // Load image/sampler/texelbuffer/F-mask descriptor from pointer.
    llvm::Value* CreateLoadDescFromPtr(llvm::Value*        pDescPtr,
                                       const llvm::Twine&  instName) override final;


    // Create a pointer to sampler descriptor. Returns a value of the type returned by GetSamplerDescPtrTy.
    llvm::Value* CreateGetSamplerDescPtr(uint32_t            descSet,
                                         uint32_t            binding,
                                         const llvm::Twine&  instName) override final;

    // Create a pointer to image descriptor. Returns a value of the type returned by GetImageDescPtrTy.
    llvm::Value* CreateGetImageDescPtr(uint32_t            descSet,
                                       uint32_t            binding,
                                       const llvm::Twine&  instName) override final;

    // Create a pointer to texel buffer descriptor. Returns a value of the type returned by GetTexelBufferDescPtrTy.
    llvm::Value* CreateGetTexelBufferDescPtr(uint32_t            descSet,
                                             uint32_t            binding,
                                             const llvm::Twine&  instName) override final;

    // Create a pointer to F-mask descriptor. Returns a value of the type returned by GetFmaskDescPtrTy.
    llvm::Value* CreateGetFmaskDescPtr(uint32_t            descSet,
                                       uint32_t            binding,
                                       const llvm::Twine&  instName) override final;

    // Create a load of the push constants pointer.
    llvm::Value* CreateLoadPushConstantsPtr(llvm::Type*         pPushConstantsTy,
                                            const llvm::Twine&  instName) override final;

    // Create a buffer length query based on the specified descriptor.
    llvm::Value* CreateGetBufferDescLength(llvm::Value* const pBufferDesc,
                                           const llvm::Twine& instName = "") override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplDesc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplDesc)

    llvm::Value* ScalarizeIfUniform(llvm::Value* pValue, bool isNonUniform);
};

// =====================================================================================================================
// Builder implementation subclass for image operations
class BuilderImplImage : virtual public BuilderImplBase
{
public:
    BuilderImplImage(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create an image load.
    llvm::Value* CreateImageLoad(llvm::Type*             pResultTy,
                                 uint32_t                dim,
                                 uint32_t                flags,
                                 llvm::Value*            pImageDesc,
                                 llvm::Value*            pCoord,
                                 llvm::Value*            pMipLevel,
                                 const llvm::Twine&      instName = "") override final;

    // Create an image load with F-mask.
    llvm::Value* CreateImageLoadWithFmask(llvm::Type*             pResultTy,
                                          uint32_t                dim,
                                          uint32_t                flags,
                                          llvm::Value*            pImageDesc,
                                          llvm::Value*            pFmaskDesc,
                                          llvm::Value*            pCoord,
                                          llvm::Value*            pSampleNum,
                                          const llvm::Twine&      instName = "") override final;

    // Create an image store.
    llvm::Value* CreateImageStore(uint32_t               dim,
                                  uint32_t               flags,
                                  llvm::Value*           pImageDesc,
                                  llvm::Value*           pCoord,
                                  llvm::Value*           pMipLevel,
                                  llvm::Value*           pTexel,
                                  const llvm::Twine&     instName = "") override final;

    // Create an image sample.
    llvm::Value* CreateImageSample(llvm::Type*                   pResultTy,
                                   uint32_t                      dim,
                                   uint32_t                      flags,
                                   llvm::Value*                  pImageDesc,
                                   llvm::Value*                  pSamplerDesc,
                                   llvm::ArrayRef<llvm::Value*>  address,
                                   const llvm::Twine&            instName = "") override final;

    // Create an image gather
    llvm::Value* CreateImageGather(llvm::Type*                   pResultTy,
                                   uint32_t                      dim,
                                   uint32_t                      flags,
                                   llvm::Value*                  pImageDesc,
                                   llvm::Value*                  pSamplerDesc,
                                   llvm::ArrayRef<llvm::Value*>  address,
                                   const llvm::Twine&            instName = "") override final;

    // Create an image atomic operation other than compare-and-swap.
    llvm::Value* CreateImageAtomic(uint32_t               atomicOp,
                                   uint32_t               dim,
                                   uint32_t               flags,
                                   llvm::AtomicOrdering   ordering,
                                   llvm::Value*           pImageDesc,
                                   llvm::Value*           pCoord,
                                   llvm::Value*           pInputValue,
                                   const llvm::Twine&     instName = "") override final;

    // Create an image atomic compare-and-swap.
    llvm::Value* CreateImageAtomicCompareSwap(uint32_t              dim,
                                              uint32_t              flags,
                                              llvm::AtomicOrdering  ordering,
                                              llvm::Value*          pImageDesc,
                                              llvm::Value*          pCoord,
                                              llvm::Value*          pInputValue,
                                              llvm::Value*          pComparatorValue,
                                              const llvm::Twine&    instName = "") override final;

    // Create a query of the number of mipmap levels in an image. Returns an i32 value.
    llvm::Value* CreateImageQueryLevels(uint32_t                      dim,
                                        uint32_t                      flags,
                                        llvm::Value*                  pImageDesc,
                                        const llvm::Twine&            instName = "") override final;

    // Create a query of the number of samples in an image. Returns an i32 value.
    llvm::Value* CreateImageQuerySamples(uint32_t                      dim,
                                         uint32_t                      flags,
                                         llvm::Value*                  pImageDesc,
                                         const llvm::Twine&            instName = "") override final;

    // Create a query of size of an image at the specified LOD
    llvm::Value* CreateImageQuerySize(uint32_t                dim,
                                      uint32_t                flags,
                                      llvm::Value*            pImageDesc,
                                      llvm::Value*            pLod,
                                      const llvm::Twine&      instName = "") override final;

    // Create a get of the LOD that would be used for an image sample with the given coordinates
    // and implicit LOD.
    llvm::Value* CreateImageGetLod(uint32_t                dim,
                                   uint32_t                flags,
                                   llvm::Value*            pImageDesc,
                                   llvm::Value*            pSamplerDesc,
                                   llvm::Value*            pCoord,
                                   const llvm::Twine&      instName = "") override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplImage)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplImage)

    // Implement pre-GFX9 integer gather workaround to patch descriptor or coordinate before the gather
    llvm::Value* PreprocessIntegerImageGather(uint32_t dim, llvm::Value*& pImageDesc, llvm::Value*& pCoord);

    // Implement pre-GFX9 integer gather workaround to modify result.
    llvm::Value* PostprocessIntegerImageGather(llvm::Value*  pNeedDescPatch,
                                               uint32_t      flags,
                                               llvm::Value*  pImageDesc,
                                               llvm::Type*   pTexelTy,
                                               llvm::Value*  pResult);

    // Common code to create an image sample or gather.
    llvm::Value* CreateImageSampleGather(llvm::Type*                  pResultTy,
                                         uint32_t                     dim,
                                         uint32_t                     flags,
                                         llvm::Value*                 pCoord,
                                         llvm::Value*                 pImageDesc,
                                         llvm::Value*                 pSamplerDesc,
                                         llvm::ArrayRef<llvm::Value*> address,
                                         const llvm::Twine&           instName,
                                         bool                         isSample);

    // Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
    llvm::Value* CreateImageAtomicCommon(uint32_t                atomicOp,
                                         uint32_t                dim,
                                         uint32_t                flags,
                                         llvm::AtomicOrdering    ordering,
                                         llvm::Value*            pImageDesc,
                                         llvm::Value*            pCoord,
                                         llvm::Value*            pInputValue,
                                         llvm::Value*            pComparatorValue,
                                         const llvm::Twine&      instName);

    // Change 1D or 1DArray dimension to 2D or 2DArray if needed as a workaround on GFX9+
    uint32_t Change1DTo2DIfNeeded(uint32_t dim);

    // Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
    // modifying if necessary.
    // Returns possibly modified image dimension.
    uint32_t PrepareCoordinate(uint32_t                              dim,
                               llvm::Value*                          pCoord,
                               llvm::Value*                          pProjective,
                               llvm::Value*                          pDerivativeX,
                               llvm::Value*                          pDerivativeY,
                               llvm::SmallVectorImpl<llvm::Value*>&  outCoords,
                               llvm::SmallVectorImpl<llvm::Value*>&  outDerivatives);

    // For a cubearray with integer coordinates, combine the face and slice into a single component.
    void CombineCubeArrayFaceAndSlice(llvm::Value* pCoord, llvm::SmallVectorImpl<llvm::Value*>& coords);

    // Patch descriptor with cube dimension for image call
    llvm::Value* PatchCubeDescriptor(llvm::Value* pDesc, uint32_t dim);

    // Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
    llvm::Value* HandleFragCoordViewIndex(llvm::Value* pCoord, uint32_t flags);

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
    BuilderImplMatrix(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a matrix transpose.
    llvm::Value* CreateTransposeMatrix(llvm::Value* const pMatrix,
                                       const llvm::Twine& instName = "") override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMatrix)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMatrix)
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class BuilderImplMisc : virtual public BuilderImplBase
{
public:
    BuilderImplMisc(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a "kill". Only allowed in a fragment shader.
    llvm::Instruction* CreateKill(const llvm::Twine& instName) override final;

    // Create a "readclock".
    llvm::Instruction* CreateReadClock(bool realtime, const llvm::Twine& instName) override final;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMisc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMisc)
};

// =====================================================================================================================
// Builder implementation subclass for subgroup operations
class BuilderImplSubgroup : virtual public BuilderImplBase
{
public:
    BuilderImplSubgroup(llvm::LLVMContext& context) : BuilderImplBase(context) {}

    // Create a get subgroup size query.
    llvm::Value* CreateGetSubgroupSize(const llvm::Twine& instName) override final;

    // Create a subgroup elect.
    llvm::Value* CreateSubgroupElect(const llvm::Twine& instName) override final;

    // Create a subgroup all.
    llvm::Value* CreateSubgroupAll(llvm::Value* const pValue,
                                   const llvm::Twine& instName) override final;

    // Create a subgroup any
    llvm::Value* CreateSubgroupAny(llvm::Value* const pValue,
                                   const llvm::Twine& instName) override final;

    // Create a subgroup all equal.
    llvm::Value* CreateSubgroupAllEqual(llvm::Value* const pValue,
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
    llvm::Value* CreateSubgroupClusteredReduction(GroupArithOp       groupArithOp,
                                                  llvm::Value* const pValue,
                                                  llvm::Value* const pClusterSize,
                                                  const llvm::Twine& instName) override final;

    // Create a subgroup clustered inclusive scan.
    llvm::Value* CreateSubgroupClusteredInclusive(GroupArithOp       groupArithOp,
                                                  llvm::Value* const pValue,
                                                  llvm::Value* const pClusterSize,
                                                  const llvm::Twine& instName) override final;

    // Create a subgroup clustered exclusive scan.
    llvm::Value* CreateSubgroupClusteredExclusive(GroupArithOp       groupArithOp,
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
    llvm::Value* CreateGroupArithmeticIdentity(GroupArithOp      groupArithOp,
                                               llvm::Type* const pType);
    llvm::Value* CreateGroupArithmeticOperation(GroupArithOp       groupArithOp,
                                                llvm::Value* const pX,
                                                llvm::Value* const pY);
    llvm::Value* CreateInlineAsmSideEffect(llvm::Value* const pValue);
    llvm::Value* CreateDppMov(llvm::Value* const pValue,
                              DppCtrl            dppCtrl,
                              uint32_t           rowMask,
                              uint32_t           bankMask,
                              bool               boundCtrl);
    llvm::Value* CreateDppUpdate(llvm::Value* const pOrigValue,
                                 llvm::Value* const pUpdateValue,
                                 DppCtrl            dppCtrl,
                                 uint32_t           rowMask,
                                 uint32_t           bankMask,
                                 bool               boundCtrl);

#if LLPC_BUILD_GFX10
    llvm::Value* CreatePermLane16(llvm::Value* const pOrigValue,
                                  llvm::Value* const pUpdateValue,
                                  uint32_t           selectBitsLow,
                                  uint32_t           selectBitsHigh,
                                  bool               fetchInactive,
                                  bool               boundCtrl);
    llvm::Value* CreatePermLaneX16(llvm::Value* const pOrigValue,
                                   llvm::Value* const pUpdateValue,
                                   uint32_t           selectBitsLow,
                                   uint32_t           selectBitsHigh,
                                   bool               fetchInactive,
                                   bool               boundCtrl);
#endif

    llvm::Value* CreateDsSwizzle(llvm::Value* const pValue,
                                 uint16_t           dsPattern);
    llvm::Value* CreateWwm(llvm::Value* const pValue);
    llvm::Value* CreateSetInactive(llvm::Value* const pActive,
                                   llvm::Value* const pInactive);
    llvm::Value* CreateThreadMask();
    llvm::Value* CreateThreadMaskedSelect(
        llvm::Value* const pThreadMask,
        uint64_t           andMask,
        llvm::Value* const pValue1,
        llvm::Value* const pValue2);
    uint16_t GetDsSwizzleBitMode(uint8_t xorMask,
                                 uint8_t orMask,
                                 uint8_t andMask);
    uint16_t GetDsSwizzleQuadMode(uint8_t lane0,
                                  uint8_t lane1,
                                  uint8_t lane2,
                                  uint8_t lane3);
    llvm::Value* CreateGroupBallot(
        llvm::Value* const pValue);
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
    BuilderImpl(llvm::LLVMContext& context) : BuilderImplBase(context),
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
