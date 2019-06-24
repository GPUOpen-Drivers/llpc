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

protected:
    // Get whether the context we are building in supports DPP operations.
    bool SupportDpp() const;

    // Get whether the context we are building in support the bpermute operation.
    bool SupportBPermute() const;

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

    // Create a waterfall loop containing the specified instruction.
    llvm::Instruction* CreateWaterfallLoop(llvm::Instruction*       pNonUniformInst,
                                           llvm::ArrayRef<uint32_t> operandIdxs,
                                           const llvm::Twine&       instName = "") override final;

    // Create a load of a buffer descriptor.
    llvm::Value* CreateLoadBufferDesc(uint32_t            descSet,
                                      uint32_t            binding,
                                      llvm::Value*        pDescIndex,
                                      bool                isNonUniform,
                                      llvm::Type*         pPointeeTy,
                                      const llvm::Twine&  instName) override final;

    // Create a load of a sampler descriptor. Returns a <4 x i32> descriptor.
    llvm::Value* CreateLoadSamplerDesc(uint32_t            descSet,
                                       uint32_t            binding,
                                       llvm::Value*        pDescIndex,
                                       bool                isNonUniform,
                                       const llvm::Twine&  instName) override final;

    // Create a load of a resource descriptor. Returns a <8 x i32> descriptor.
    llvm::Value* CreateLoadResourceDesc(uint32_t            descSet,
                                        uint32_t            binding,
                                        llvm::Value*        pDescIndex,
                                        bool                isNonUniform,
                                        const llvm::Twine&  instName) override final;

    // Create a load of a texel buffer descriptor. Returns a <4 x i32> descriptor.
    llvm::Value* CreateLoadTexelBufferDesc(uint32_t            descSet,
                                           uint32_t            binding,
                                           llvm::Value*        pDescIndex,
                                           bool                isNonUniform,
                                           const llvm::Twine&  instName) override final;

    // Create a load of a F-mask descriptor. Returns a <8 x i32> descriptor.
    llvm::Value* CreateLoadFmaskDesc(uint32_t            descSet,
                                     uint32_t            binding,
                                     llvm::Value*        pDescIndex,
                                     bool                isNonUniform,
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
                                 BuilderImplMatrix,
                                 BuilderImplMisc,
                                 BuilderImplSubgroup
{
public:
    BuilderImpl(llvm::LLVMContext& context) : BuilderImplBase(context),
                                              BuilderImplDesc(context),
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
