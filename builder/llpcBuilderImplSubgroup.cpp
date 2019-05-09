/*
 ***********************************************************************************************************************
 *
 *  Trade secret of Advanced Micro Devices, Inc.
 *  Copyright (c) 2019, Advanced Micro Devices, Inc., (unpublished)
 *
 *  All rights reserved. This notice is intended as a precaution against inadvertent publication and does not imply
 *  publication or any waiver of confidentiality. The year included in the foregoing notice is the year of creation of
 *  the work.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcBuilderImplSubgroup.cpp
 * @brief LLPC source file: implementation of subgroup Builder methods
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcContext.h"
#include "llpcInternal.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"

#define DEBUG_TYPE "llpc-builder-impl-subgroup"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Create a subgroup get subgroup size.
Value* BuilderImplSubgroup::CreateGetSubgroupSize(
    const Twine& instName) // [in] Name to give final instruction.
{
    return getInt32(getContext().GetShaderWaveSize(GetShaderStageFromFunction(GetInsertBlock()->getParent())));
}

// =====================================================================================================================
// Create a subgroup elect call.
Value* BuilderImplSubgroup::CreateSubgroupElect(
    const Twine& instName) // [in] Name to give final instruction.
{
    return CreateICmpEQ(CreateMbcnt(CreateGroupBallot(getTrue())), getInt32(0));
}

// =====================================================================================================================
// Create a subgroup all call.
Value* BuilderImplSubgroup::CreateSubgroupAll(
    Value* const pValue,   // [in] The value to compare across the subgroup. Must be an integer type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* const pResult = CreateICmpEQ(CreateGroupBallot(pValue), CreateGroupBallot(getTrue()));
    return CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, pValue), pValue, pResult);
}

// =====================================================================================================================
// Create a subgroup any call.
Value* BuilderImplSubgroup::CreateSubgroupAny(
    Value* const pValue,   // [in] The value to compare across the subgroup. Must be an integer type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* const pResult = CreateICmpNE(CreateGroupBallot(pValue), getInt64(0));
    return CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, pValue), pValue, pResult);
}

// =====================================================================================================================
// Create a subgroup all equal call.
Value* BuilderImplSubgroup::CreateSubgroupAllEqual(
    Value* const pValue,   // [in] The value to compare across the subgroup. Must be an integer type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Type* const pType = pValue->getType();

    Value* pCompare = CreateSubgroupBroadcastFirst(pValue, instName);

    if (pType->isFPOrFPVectorTy())
    {
        pCompare = CreateFCmpOEQ(pCompare, pValue);
    }
    else
    {
        LLPC_ASSERT(pType->isIntOrIntVectorTy());
        pCompare = CreateICmpEQ(pCompare, pValue);
    }

    if (pType->isVectorTy())
    {
        Value* pResult = CreateExtractElement(pCompare, static_cast<uint64_t>(0));

        for (uint32_t i = 1, compCount = pType->getVectorNumElements(); i < compCount; i++)
        {
            pResult = CreateAnd(pResult, CreateExtractElement(pCompare, i));
        }

        return CreateSubgroupAll(pResult, instName);
    }
    else
    {
        return CreateSubgroupAll(pCompare, instName);
    }
}

// =====================================================================================================================
// Create a subgroup broadcast call.
Value* BuilderImplSubgroup::CreateSubgroupBroadcast(
    Value* const pValue,   // [in] The value to read from the chosen lane to all active lanes.
    Value* const pIndex,   // [in] The index to broadcast from. Must be an i32.
    const Twine& instName) // [in] Name to give final instruction.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        return builder.CreateIntrinsic(Intrinsic::amdgcn_readlane,
                                       {},
                                       {
                                            mappedArgs[0],
                                            passthroughArgs[0]
                                       });
    };

    return CreateMapToInt32(pfnMapFunc, pValue, pIndex);
}

// =====================================================================================================================
// Create a subgroup broadcastfirst call.
Value* BuilderImplSubgroup::CreateSubgroupBroadcastFirst(
    Value* const pValue,   // [in] The value to read from the first active lane into all other active lanes.
    const Twine& instName) // [in] Name to give final instruction.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        return builder.CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, mappedArgs[0]);
    };

    return CreateMapToInt32(pfnMapFunc, pValue, {});
}


// =====================================================================================================================
// Create a subgroup ballot call.
Value* BuilderImplSubgroup::CreateSubgroupBallot(
    Value* const pValue,   // [in] The value to ballot across the subgroup. Must be an integer type.
    const Twine& instName) // [in] Name to give final instruction.
{
    // Check the type is definitely an integer.
    LLPC_ASSERT(pValue->getType()->isIntegerTy());

    Value* pBallot = CreateGroupBallot(pValue);

    // Ballot expects a <4 x i32> return, so we need to turn the i64 into that.
    pBallot = CreateBitCast(pBallot, VectorType::get(getInt32Ty(), 2));

    return CreateShuffleVector(pBallot, ConstantVector::getSplat(2, getInt32(0)), { 0, 1, 2, 3 });
}

// =====================================================================================================================
// Create a subgroup inverseballot call.
Value* BuilderImplSubgroup::CreateSubgroupInverseBallot(
    Value* const pValue,   // [in] The value to inverseballot across the subgroup. Must be a <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
    return CreateSubgroupBallotBitExtract(pValue, CreateMbcnt(getInt64(UINT64_MAX)), instName);
}

// =====================================================================================================================
// Create a subgroup ballotbitextract call.
Value* BuilderImplSubgroup::CreateSubgroupBallotBitExtract(
    Value* const pValue,   // [in] The ballot value to bit extract. Must be an <4 x i32> type.
    Value* const pIndex,   // [in] The bit index to extract. Must be an i32 type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pIndexMask = CreateZExtOrTrunc(pIndex, getInt64Ty());
    pIndexMask = CreateShl(getInt64(1), pIndexMask);
    Value* pValueAsInt64 = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
    pValueAsInt64 = CreateBitCast(pValueAsInt64, getInt64Ty());
    Value* const pResult = CreateAnd(pIndexMask, pValueAsInt64);
    return CreateICmpNE(pResult, getInt64(0));
}

// =====================================================================================================================
// Create a subgroup ballotbitcount call.
Value* BuilderImplSubgroup::CreateSubgroupBallotBitCount(
    Value* const pValue,   // [in] The ballot value to bit count. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
    pResult = CreateBitCast(pResult, getInt64Ty());
    pResult = CreateUnaryIntrinsic(Intrinsic::ctpop, pResult);
    return CreateZExtOrTrunc(pResult, getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup ballotinclusivebitcount call.
Value* BuilderImplSubgroup::CreateSubgroupBallotInclusiveBitCount(
    Value* const pValue,   // [in] The ballot value to inclusively bit count. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* const pExclusiveBitCount = CreateSubgroupBallotExclusiveBitCount(pValue, instName);
    Value* const pInverseBallot = CreateSubgroupInverseBallot(pValue, instName);
    Value* const pInclusiveBitCount = CreateAdd(pExclusiveBitCount, getInt32(1));
    return CreateSelect(pInverseBallot, pInclusiveBitCount, pExclusiveBitCount);
}

// =====================================================================================================================
// Create a subgroup ballotexclusivebitcount call.
Value* BuilderImplSubgroup::CreateSubgroupBallotExclusiveBitCount(
    Value* const pValue,   // [in] The ballot value to exclusively bit count. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
    pResult = CreateBitCast(pResult, getInt64Ty());
    pResult = CreateMbcnt(pResult);
    return CreateZExtOrTrunc(pResult, getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup ballotfindlsb call.
Value* BuilderImplSubgroup::CreateSubgroupBallotFindLsb(
    Value* const pValue, // [in] The ballot value to find the least significant bit of. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
    pResult = CreateBitCast(pResult, getInt64Ty());
    pResult = CreateIntrinsic(Intrinsic::cttz, getInt64Ty(), { pResult, getTrue() });
    return CreateZExtOrTrunc(pResult, getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup ballotfindmsb call.
Value* BuilderImplSubgroup::CreateSubgroupBallotFindMsb(
    Value* const pValue,   // [in] The ballot value to find the most significant bit of. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), {0, 1});
    pResult = CreateBitCast(pResult, getInt64Ty());
    pResult = CreateIntrinsic(Intrinsic::ctlz, getInt64Ty(), { pResult, getTrue() });
    return CreateZExtOrTrunc(pResult, getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup shuffle call.
Value* BuilderImplSubgroup::CreateSubgroupShuffle(
    Value* const pValue,   // [in] The value to shuffle.
    Value* const pIndex,   // [in] The index to shuffle from.
    const Twine& instName) // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
        {
            return builder.CreateIntrinsic(Intrinsic::amdgcn_ds_bpermute,
                                           {},
                                           {
                                                passthroughArgs[0],
                                                mappedArgs[0]
                                           });
        };

        // The ds_bpermute intrinsic requires the index be multiplied by 4.
        return CreateMapToInt32(pfnMapFunc, pValue, CreateMul(pIndex, getInt32(4)));
    }
    else
    {
        auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
        {
            return builder.CreateIntrinsic(Intrinsic::amdgcn_readlane,
                                           {},
                                           {
                                                mappedArgs[0],
                                                passthroughArgs[0]
                                           });
        };

        return CreateMapToInt32(pfnMapFunc, pValue, pIndex);
    }
}

// =====================================================================================================================
// Create a subgroup shufflexor call.
Value* BuilderImplSubgroup::CreateSubgroupShuffleXor(
    Value* const pValue,   // [in] The value to shuffle.
    Value* const pMask,    // [in] The mask to shuffle with.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pIndex = CreateMbcnt(getInt64(UINT64_MAX));
    pIndex = CreateXor(pIndex, pMask);
    return CreateSubgroupShuffle(pValue, pIndex, instName);
}

// =====================================================================================================================
// Create a subgroup shuffleup call.
Value* BuilderImplSubgroup::CreateSubgroupShuffleUp(
    Value* const pValue,   // [in] The value to shuffle.
    Value* const pDelta,   // [in] The delta to shuffle from.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pIndex = CreateMbcnt(getInt64(UINT64_MAX));
    pIndex = CreateSub(pIndex, pDelta);
    return CreateSubgroupShuffle(pValue, pIndex, instName);
}

// =====================================================================================================================
// Create a subgroup shuffledown call.
Value* BuilderImplSubgroup::CreateSubgroupShuffleDown(
    Value* const pValue,   // [in] The value to shuffle.
    Value* const pDelta,   // [in] The delta to shuffle from.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pIndex = CreateMbcnt(getInt64(UINT64_MAX));
    pIndex = CreateAdd(pIndex, pDelta);
    return CreateSubgroupShuffle(pValue, pIndex, instName);
}

// =====================================================================================================================
// Create a subgroup clustered reduction.
Value* BuilderImplSubgroup::CreateSubgroupClusteredReduction(
    GroupArithOp   groupArithOp, // The group arithmetic operation.
    Value* const   pValue,       // [in] An LLVM value.
    Value* const   pClusterSize, // [in] The cluster size.
    const Twine&   instName)     // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        // Start the WWM section by setting the inactive lanes.
        Value* pResult = CreateSetInactive(pValue, CreateGroupArithmeticIdentity(groupArithOp, pValue->getType()));

        // Perform The group arithmetic operation between adjacent lanes in the subgroup, with all masks and rows enabled (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppMov(pResult, DppCtrl::DppQuadPerm1032, 0xF, 0xF, 0)), pResult);

        // Perform The group arithmetic operation between N <-> N+2 lanes in the subgroup, with all masks and rows enabled (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppMov(pResult, DppCtrl::DppQuadPerm2301, 0xF, 0xF, 0)), pResult);

        // Use a row half mirror to make all values in a cluster of 8 the same, with all masks and rows enabled (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(8)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppMov(pResult, DppCtrl::DppRowHalfMirror, 0xF, 0xF, 0)), pResult);

        // Use a row mirror to make all values in a cluster of 16 the same, with all masks and rows enabled (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppMov(pResult, DppCtrl::DppRowMirror, 0xF, 0xF, 0)), pResult);

        // Use a row broadcast to move the 15th element in each cluster of 16 to the next cluster. The row mask is set
        // to 0xa (0b1010) so that only the 2nd and 4th clusters of 16 perform the calculation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppMov(pResult, DppCtrl::DppRowBcast15, 0xA, 0xF, 0)), pResult);

        // Use a row broadcast to move the 31st element from the lower cluster of 32 to the upper cluster. The row mask
        // is set to 0x8 (0b1000) so that only the upper cluster of 32 perform the calculation.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppMov(pResult, DppCtrl::DppRowBcast31, 0x8, 0xF, 0)), pResult);

        Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);
        Value* const pBroadcast63 = CreateSubgroupBroadcast(pResult, getInt32(63), instName);

        // If the cluster size is 64 we always read the value from the last invocation in the subgroup.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)), pBroadcast63, pResult);

        // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
        // invocation 31 or 63's value.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(32)),
            CreateSelect(CreateICmpULT(CreateMbcnt(getInt64(UINT64_MAX)), getInt32(32)), pBroadcast31, pBroadcast63),
                pResult);

        // Finish the WWM section by calling the intrinsic.
        return CreateWwm(pResult);
    }
    else
    {
        // Start the WWM section by setting the inactive lanes.
        Value* pResult = CreateSetInactive(pValue, CreateGroupArithmeticIdentity(groupArithOp, pValue->getType()));

        // The DS swizzle mode is doing a xor of 0x1 to swap values between N <-> N+1, and the and mask of 0x1f means
        // all lanes do the same swap.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x01, 0x00, 0x1F))), pResult);

        // The DS swizzle mode is doing a xor of 0x2 to swap values between N <-> N+2, and the and mask of 0x1f means
        // all lanes do the same swap.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x02, 0x00, 0x1F))), pResult);

        // The DS swizzle mode is doing a xor of 0x4 to swap values between N <-> N+4, and the and mask of 0x1f means
        // all lanes do the same swap.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(8)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x04, 0x00, 0x1F))), pResult);

        // The DS swizzle mode is doing a xor of 0x8 to swap values between N <-> N+8, and the and mask of 0x1f means
        // all lanes do the same swap.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x08, 0x00, 0x1F))), pResult);

        // The DS swizzle mode is doing a xor of 0x10 to swap values between N <-> N+16, and the and mask of 0x1f means
        // all lanes do the same swap.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x10, 0x00, 0x1F))), pResult);

        Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);
        Value* const pBroadcast63 = CreateSubgroupBroadcast(pResult, getInt32(63), instName);

        // If the cluster size is 64 we always compute the value by adding together the two broadcasts.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
            CreateGroupArithmeticOperation(groupArithOp, pBroadcast31, pBroadcast63), pResult);

        // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
        // invocation 31 or 63's value.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(32)),
            CreateSelect(CreateICmpULT(CreateMbcnt(getInt64(UINT64_MAX)), getInt32(32)), pBroadcast31, pBroadcast63),
                pResult);

        // Finish the WWM section by calling the intrinsic.
        return CreateWwm(pResult);
    }
}

// =====================================================================================================================
// Create a subgroup clustered inclusive scan.
Value* BuilderImplSubgroup::CreateSubgroupClusteredInclusive(
    GroupArithOp groupArithOp,   // The group arithmetic operation.
    Value* const pValue,         // [in] An LLVM value.
    Value* const pClusterSize,   // [in] The cluster size.
    const Twine& instName)       // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        Value* const pIdentity = CreateGroupArithmeticIdentity(groupArithOp, pValue->getType());

        // Start the WWM section by setting the inactive invocations.
        Value* const pSetInactive = CreateSetInactive(pValue, pIdentity);

        // The DPP operation has all rows active and all banks in the rows active (0xF).
        Value* pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)),
            CreateGroupArithmeticOperation(groupArithOp, pSetInactive,
                CreateDppUpdate(pIdentity, pSetInactive, DppCtrl::DppRowSr1, 0xF, 0xF, 0)), pSetInactive);

        // The DPP operation has all rows active and all banks in the rows active (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pSetInactive, DppCtrl::DppRowSr2, 0xF, 0xF, 0)), pResult);

        // The DPP operation has all rows active and all banks in the rows active (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pSetInactive, DppCtrl::DppRowSr3, 0xF, 0xF, 0)), pResult);

        // The DPP operation has all rows active (0xF) and the top 3 banks active (0xe, 0b1110) to make sure that in
        // each cluster of 16, only the top 12 lanes perform the operation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(8)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowSr4, 0xF, 0xE, 0)), pResult);

        // The DPP operation has all rows active (0xF) and the top 2 banks active (0xc, 0b1100) to make sure that in
        // each cluster of 16, only the top 8 lanes perform the operation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowSr8, 0xF, 0xC, 0)), pResult);

        // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the operation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast15, 0xA, 0xF, 0)), pResult);

        // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the operation.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast31, 0xC, 0xF, 0)), pResult);

        // Finish the WWM section by calling the intrinsic.
        return CreateWwm(pResult);
    }
    else
    {
        Value* const pClusteredExclusive = CreateSubgroupClusteredExclusive(groupArithOp,
                                                                            pValue,
                                                                            pClusterSize,
                                                                            instName);
        return CreateGroupArithmeticOperation(groupArithOp, pValue, pClusteredExclusive);
    }
}

// =====================================================================================================================
// Create a subgroup clustered exclusive scan.
Value* BuilderImplSubgroup::CreateSubgroupClusteredExclusive(
    GroupArithOp groupArithOp,   // The group arithmetic operation.
    Value* const pValue,         // [in] An LLVM value.
    Value* const pClusterSize,   // [in] The cluster size.
    const Twine& instName)       // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        Value* const pIdentity = CreateGroupArithmeticIdentity(groupArithOp, pValue->getType());

        // Start the WWM section by setting the inactive invocations.
        Value* const pSetInactive = CreateSetInactive(pValue, pIdentity);

        // Shift the whole subgroup right by one, using a DPP update operation. This will ensure that the identity
        // value is in the 0th invocation and all other values are shifted up. All rows and banks are active (0xF).
        Value* const pShiftRight = CreateDppUpdate(pIdentity, pSetInactive, DppCtrl::DppWfSr1, 0xF, 0xF, 0);

        // The DPP operation has all rows active and all banks in the rows active (0xF).
        Value* pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)),
            CreateGroupArithmeticOperation(groupArithOp, pShiftRight,
                CreateDppUpdate(pIdentity, pShiftRight, DppCtrl::DppRowSr1, 0xF, 0xF, 0)), pShiftRight);

        // The DPP operation has all rows active and all banks in the rows active (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pShiftRight, DppCtrl::DppRowSr2, 0xF, 0xF, 0)), pResult);

        // The DPP operation has all rows active and all banks in the rows active (0xF).
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pShiftRight, DppCtrl::DppRowSr3, 0xF, 0xF, 0)), pResult);

        // The DPP operation has all rows active (0xF) and the top 3 banks active (0xe, 0b1110) to make sure that in
        // each cluster of 16, only the top 12 lanes perform the operation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(8)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowSr4, 0xF, 0xE, 0)), pResult);

        // The DPP operation has all rows active (0xF) and the top 2 banks active (0xc, 0b1100) to make sure that in
        // each cluster of 16, only the top 8 lanes perform the operation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowSr8, 0xF, 0xC, 0)), pResult);

        // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the operation.
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast15, 0xA, 0xF, 0)), pResult);

        // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the operation.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
            CreateGroupArithmeticOperation(groupArithOp, pResult,
                CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast31, 0xC, 0xF, 0)), pResult);

        // Finish the WWM section by calling the intrinsic.
        return CreateWwm(pResult);
    }
    else
    {
        Value* const pThreadMask = CreateThreadMask();

        Value* const pIdentity = CreateGroupArithmeticIdentity(groupArithOp, pValue->getType());

        // Start the WWM section by setting the inactive invocations.
        Value* const pSetInactive = CreateSetInactive(pValue, pIdentity);
        Value* pResult = pSetInactive;

        // The DS swizzle is xor'ing by 0x1 with an and mask of 0x1f, which swaps from N <-> N+1. We don't want the N's
        // to perform the operation, only the N+1's, so we use a mask of 0xa (0b1010) to stop the N's doing anything.
        Value* pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xAAAAAAAAAAAAAAAA,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x01, 0x00, 0x1F)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0x1 with an and mask of 0x1c, which swaps from N <-> N+2. We don't want the N's
        // to perform the operation, only the N+2's, so we use a mask of 0xc (0b1100) to stop the N's doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xCCCCCCCCCCCCCCCC,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x00, 0x01, 0x1C)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0x3 with an and mask of 0x18, which swaps from N <-> N+4. We don't want the N's
        // to perform the operation, only the N+4's, so we use a mask of 0xF0 (0b11110000) to stop the N's doing
        // anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xF0F0F0F0F0F0F0F0,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x00, 0x03, 0x18)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(8)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0x7 with an and mask of 0x10, which swaps from N <-> N+8. We don't want the N's
        // to perform the operation, only the N+8's, so we use a mask of 0xFf00 (0b1111111100000000) to stop the N's
        // doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xFF00FF00FF00FF00,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x00, 0x07, 0x10)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
        // to perform the operation, only the N+16's, so we use a mask of 0xFfff0000
        // (0b11111111111111110000000000000000) to stop the N's doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xFFFF0000FFFF0000,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x00, 0x0F, 0x00)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);

        // The mask here is enforcing that only the top 32 lanes of the wavefront perform the final scan operation.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xFFFFFFFF00000000, pBroadcast31, pIdentity);
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // Finish the WWM section by calling the intrinsic.
        return CreateWwm(pResult);
    }
}

// =====================================================================================================================
// Create a subgroup quad broadcast call.
Value* BuilderImplSubgroup::CreateSubgroupQuadBroadcast(
    Value* const pValue,   // [in] The value to broadcast across the quad.
    Value* const pIndex,   // [in] The index in the quad to broadcast the value from.
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = nullptr;

    const uint32_t indexBits = pIndex->getType()->getPrimitiveSizeInBits();

    if (SupportDpp())
    {
        Value* pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 0));
        pResult = CreateSelect(pCompare, CreateDppMov(pIndex, DppCtrl::DppQuadPerm0000, 0xF, 0xF, false), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 1));
        pResult = CreateSelect(pCompare, CreateDppMov(pIndex, DppCtrl::DppQuadPerm1111, 0xF, 0xF, false), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 2));
        pResult = CreateSelect(pCompare, CreateDppMov(pIndex, DppCtrl::DppQuadPerm2222, 0xF, 0xF, false), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 3));
        pResult = CreateSelect(pCompare, CreateDppMov(pIndex, DppCtrl::DppQuadPerm3333, 0xF, 0xF, false), pResult);
    }
    else
    {
        Value* pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 0));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pIndex, GetDsSwizzleQuadMode(0, 0, 0, 0)), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 1));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pIndex, GetDsSwizzleQuadMode(1, 1, 1, 1)), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 2));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pIndex, GetDsSwizzleQuadMode(2, 2, 2, 2)), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 3));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pIndex, GetDsSwizzleQuadMode(3, 3, 3, 3)), pResult);
    }

    return pResult;
}

// =====================================================================================================================
// Create a subgroup quad swap horizontal call.
Value* BuilderImplSubgroup::CreateSubgroupQuadSwapHorizontal(
    Value* const pValue,   // [in] The value to swap.
    const Twine& instName) // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        return CreateDppMov(pValue, DppCtrl::DppQuadPerm1032, 0xF, 0xF, false);
    }
    else
    {
        return CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(2, 3, 0, 1));
    }
}

// =====================================================================================================================
// Create a subgroup quad swap vertical call.
Value* BuilderImplSubgroup::CreateSubgroupQuadSwapVertical(
    Value* const pValue,   // [in] The value to swap.
    const Twine& instName) // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        return CreateDppMov(pValue, DppCtrl::DppQuadPerm2301, 0xF, 0xF, false);
    }
    else
    {
        return CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(1, 0, 3, 2));
    }
}

// =====================================================================================================================
// Create a subgroup quadswapdiagonal call.
Value* BuilderImplSubgroup::CreateSubgroupQuadSwapDiagonal(
    Value* const pValue,   // [in] The value to swap.
    const Twine& instName) // [in] Name to give final instruction.
{
    if (SupportDpp())
    {
        return CreateDppMov(pValue, DppCtrl::DppQuadPerm0123, 0xF, 0xF, false);
    }
    else
    {
        return CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(3, 2, 1, 0));
    }
}

// =====================================================================================================================
// Create The group arithmetic operation identity.
Value* BuilderImplSubgroup::CreateGroupArithmeticIdentity(
    GroupArithOp   groupArithOp, // The group arithmetic operation to get the identity for.
    Type* const    pType)        // [in] The type of the identity.
{
    switch (groupArithOp)
    {
    case GroupArithOp::IAdd:
        return ConstantInt::get(pType, 0);
    case GroupArithOp::FAdd:
        return ConstantFP::get(pType, 0.0);
    case GroupArithOp::IMul:
        return ConstantInt::get(pType, 1);
    case GroupArithOp::FMul:
        return ConstantFP::get(pType, 1.0);
    case GroupArithOp::SMin:
        if (pType->isIntOrIntVectorTy(8))
        {
            return ConstantInt::get(pType, INT8_MAX, true);
        }
        else if (pType->isIntOrIntVectorTy(16))
        {
            return ConstantInt::get(pType, INT16_MAX, true);
        }
        else if (pType->isIntOrIntVectorTy(32))
        {
            return ConstantInt::get(pType, INT32_MAX, true);
        }
        else if (pType->isIntOrIntVectorTy(64))
        {
            return ConstantInt::get(pType, INT64_MAX, true);
        }
        else
        {
            LLPC_NEVER_CALLED();
            return nullptr;
        }
    case GroupArithOp::UMin:
        return ConstantInt::get(pType, UINT64_MAX, false);
    case GroupArithOp::FMin:
        return ConstantFP::getInfinity(pType, false);
    case GroupArithOp::SMax:
        if (pType->isIntOrIntVectorTy(8))
        {
            return ConstantInt::get(pType, INT8_MIN, true);
        }
        else if (pType->isIntOrIntVectorTy(16))
        {
            return ConstantInt::get(pType, INT16_MIN, true);
        }
        else if (pType->isIntOrIntVectorTy(32))
        {
            return ConstantInt::get(pType, INT32_MIN, true);
        }
        else if (pType->isIntOrIntVectorTy(64))
        {
            return ConstantInt::get(pType, INT64_MIN, true);
        }
        else
        {
            LLPC_NEVER_CALLED();
            return nullptr;
        }
    case GroupArithOp::UMax:
        return ConstantInt::get(pType, 0, false);
    case GroupArithOp::FMax:
        return ConstantFP::getInfinity(pType, true);
    case GroupArithOp::And:
        return ConstantInt::get(pType, UINT64_MAX, false);
    case GroupArithOp::Or:
        return ConstantInt::get(pType, 0, false);
    case GroupArithOp::Xor:
        return ConstantInt::get(pType, 0, false);
    default:
        LLPC_NEVER_CALLED();
        return nullptr;
    }
}

// =====================================================================================================================
// Create The group arithmetic operation arithmetic on x and y.
Value* BuilderImplSubgroup::CreateGroupArithmeticOperation(
    GroupArithOp   groupArithOp, // The group arithmetic operation to use for the reduction.
    Value* const   pX,           // [in] The x value to perform the arithmetic on.
    Value* const   pY)           // [in] The y value to perform the arithmetic on.
{
    switch (groupArithOp)
    {
    case GroupArithOp::IAdd:
        return CreateAdd(pX, pY);
    case GroupArithOp::FAdd:
        return CreateFAdd(pX, pY);
    case GroupArithOp::IMul:
        return CreateMul(pX, pY);
    case GroupArithOp::FMul:
        return CreateFMul(pX, pY);
    case GroupArithOp::SMin:
        return CreateSelect(CreateICmpSLT(pX, pY), pX, pY);
    case GroupArithOp::UMin:
        return CreateSelect(CreateICmpULT(pX, pY), pX, pY);
    case GroupArithOp::FMin:
        return CreateMinNum(pX, pY);
    case GroupArithOp::SMax:
        return CreateSelect(CreateICmpSGT(pX, pY), pX, pY);
    case GroupArithOp::UMax:
        return CreateSelect(CreateICmpUGT(pX, pY), pX, pY);
    case GroupArithOp::FMax:
        return CreateMaxNum(pX, pY);
    case GroupArithOp::And:
        return CreateAnd(pX, pY);
    case GroupArithOp::Or:
        return CreateOr(pX, pY);
    case GroupArithOp::Xor:
        return CreateXor(pX, pY);
    default:
        LLPC_NOT_IMPLEMENTED();
        return nullptr;
    }
}

// =====================================================================================================================
// Create an inline assembly call to cause a side effect (used to workaround mis-compiles with convergent).
Value* BuilderImplSubgroup::CreateInlineAsmSideEffect(
    Value* const pValue) // [in] The value to ensure doesn't move in control flow.
{
    Type* const pType = pValue->getType();

    // Some vector types don't play nice with inline assembly, so for those we just do the inline assembly trick on the
    // 0th element of the vector.
    if (pType->isVectorTy())
    {
        Value* pResult = CreateExtractElement(pValue, static_cast<uint64_t>(0));
        pResult = CreateInlineAsmSideEffect(pResult);
        return CreateInsertElement(pValue, pResult, static_cast<uint64_t>(0));
    }
    else if (pType->getPrimitiveSizeInBits() < 32)
    {
        Type* const pVectorType = VectorType::get(pType, (pType->getPrimitiveSizeInBits() == 16) ? 2 : 4);
        Value* pResult = UndefValue::get(pVectorType);
        pResult = CreateInsertElement(pResult, pValue, static_cast<uint64_t>(0));
        pResult = CreateBitCast(pResult, getInt32Ty());
        pResult = CreateInlineAsmSideEffect(pResult);
        pResult = CreateBitCast(pResult, pVectorType);
        return CreateExtractElement(pResult, static_cast<uint64_t>(0));
    }
    else
    {
        FunctionType* const pFuncType = FunctionType::get(pType, pType, false);
        InlineAsm* const pInlineAsm = InlineAsm::get(pFuncType, "; %1", "=v,0", true);
        return CreateCall(pInlineAsm, pValue);
    }
}

// =====================================================================================================================
// Create a call to dpp mov.
Value* BuilderImplSubgroup::CreateDppMov(
    Value* const pValue,    // [in] The value to DPP mov.
    DppCtrl      dppCtrl,   // The dpp_ctrl to use.
    uint32_t     rowMask,   // The row mask.
    uint32_t     bankMask,  // The bank mask.
    bool         boundCtrl) // Whether bound_ctrl is used or not.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        return builder.CreateIntrinsic(Intrinsic::amdgcn_mov_dpp,
                                       builder.getInt32Ty(),
                                       {
                                            mappedArgs[0],
                                            passthroughArgs[0],
                                            passthroughArgs[1],
                                            passthroughArgs[2],
                                            passthroughArgs[3]
                                       });
    };

    return CreateMapToInt32(pfnMapFunc,
                          pValue,
                          {
                                getInt32(static_cast<uint32_t>(dppCtrl)),
                                getInt32(rowMask),
                                getInt32(bankMask),
                                getInt1(boundCtrl)
                          });
}

// =====================================================================================================================
// Create a call to dpp update.
Value* BuilderImplSubgroup::CreateDppUpdate(
    Value* const pOrigValue,   // [in] The original value we are going to update.
    Value* const pUpdateValue, // [in] The value to DPP update.
    DppCtrl      dppCtrl,      // The dpp_ctrl to use.
    uint32_t     rowMask,      // The row mask.
    uint32_t     bankMask,     // The bank mask.
    bool         boundCtrl)    // Whether bound_ctrl is used or not.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        return builder.CreateIntrinsic(Intrinsic::amdgcn_update_dpp,
                                       builder.getInt32Ty(),
                                       {
                                            mappedArgs[0],
                                            mappedArgs[1],
                                            passthroughArgs[0],
                                            passthroughArgs[1],
                                            passthroughArgs[2],
                                            passthroughArgs[3]
                                       });
    };

    return CreateMapToInt32(pfnMapFunc,
                          {
                                pOrigValue,
                                pUpdateValue,
                          },
                          {
                                getInt32(static_cast<uint32_t>(dppCtrl)),
                                getInt32(rowMask),
                                getInt32(bankMask),
                                getInt1(boundCtrl)
                          });
}

// =====================================================================================================================
// Create a call to ds swizzle.
Value* BuilderImplSubgroup::CreateDsSwizzle(
    Value* const pValue,    // [in] The value to swizzle.
    uint16_t     dsPattern) // The pattern to swizzle with.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        return builder.CreateIntrinsic(Intrinsic::amdgcn_ds_swizzle,
                                       {},
                                       {
                                            mappedArgs[0],
                                            passthroughArgs[0]
                                       });
    };

    return CreateMapToInt32(pfnMapFunc, pValue, getInt32(dsPattern));
}

// =====================================================================================================================
// Create a call to WWM (whole wave mode).
Value* BuilderImplSubgroup::CreateWwm(
    Value* const pValue) // [in] The value to pass to the WWM call.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*>) -> Value*
    {
        return builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wwm, mappedArgs[0]);
    };

    return CreateMapToInt32(pfnMapFunc, pValue, {});
}

// =====================================================================================================================
// Create a call to set inactive. Both active and inactive should have the same type.
Value* BuilderImplSubgroup::CreateSetInactive(
    Value* pActive,   // [in] The value active invocations should take.
    Value* pInactive) // [in] The value inactive invocations should take.
{
    Type* const pType = pActive->getType();

    LLPC_ASSERT(pType == pInactive->getType());

    if (pType->isVectorTy())
    {
        Value* pResult = UndefValue::get(pType);

        for (uint32_t i = 0; i < pType->getVectorNumElements(); i++)
        {
            Value* const pActiveComp = CreateExtractElement(pActive, i);
            Value* const pInactiveComp = CreateExtractElement(pInactive, i);
            pResult = CreateInsertElement(pResult, CreateSetInactive(pActiveComp, pInactiveComp), i);
        }

        return pResult;
    }
    else if (pType->isFloatingPointTy())
    {
        pActive = CreateBitCast(pActive, getIntNTy(pType->getPrimitiveSizeInBits()));
        pInactive = CreateBitCast(pInactive, getIntNTy(pType->getPrimitiveSizeInBits()));
        return CreateBitCast(CreateSetInactive(pActive, pInactive), pType);
    }
    else if (pType->getPrimitiveSizeInBits() < 32)
    {
        Type* const pVectorType = VectorType::get(pType, (pType->getPrimitiveSizeInBits() == 16) ? 2 : 4);
        Value* const pUndef = UndefValue::get(pVectorType);
        pActive = CreateInsertElement(pUndef, pActive, static_cast<uint64_t>(0));
        pActive = CreateBitCast(pActive, getInt32Ty());
        pInactive = CreateInsertElement(pUndef, pInactive, static_cast<uint64_t>(0));
        pInactive = CreateBitCast(pInactive, getInt32Ty());
        Value* pResult = CreateSetInactive(pActive, pInactive);
        pResult = CreateBitCast(pResult, pVectorType);
        return CreateExtractElement(pResult, static_cast<uint64_t>(0));
    }
    else
    {
        // TODO: There is a longstanding bug with LLVM's convergent that forces us to use inline assembly with
        // sideffects to stop any hoisting out of control flow.
        pActive = CreateInlineAsmSideEffect(pActive);
        return CreateIntrinsic(Intrinsic::amdgcn_set_inactive, pType, {pActive, pInactive});
    }
}

// =====================================================================================================================
// Create a call to mbcnt.
Value* BuilderImplSubgroup::CreateMbcnt(
    Value* const pMask) // [in] The mask to pass to the mbcnt call, must be an i64.
{
    // Check that the type is definitely an i64.
    LLPC_ASSERT(pMask->getType()->isIntegerTy(64));

    Value* const pMasks = CreateBitCast(pMask, VectorType::get(getInt32Ty(), 2));
    Value* const pMaskLow = CreateExtractElement(pMasks, getInt32(0));
    Value* const pMaskHigh = CreateExtractElement(pMasks, getInt32(1));
    CallInst* const pMbcnt = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, { pMaskLow, getInt32(0) });
    return CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, { pMaskHigh, pMbcnt });
}

// =====================================================================================================================
// Create a ds_swizzle bit mode pattern.
uint16_t BuilderImplSubgroup::GetDsSwizzleBitMode(
    uint8_t xorMask, // The xor mask (bits 10..14).
    uint8_t orMask,  // The or mask (bits 5..9).
    uint8_t andMask) // The and mask (bits 0..4).
{
    return (static_cast<uint16_t>(xorMask & 0x1F) << 10) |
           (static_cast<uint16_t>(orMask & 0x1F) << 5) |
           (andMask & 0x1F);
}

// =====================================================================================================================
// Create a ds_swizzle quad mode pattern.
uint16_t BuilderImplSubgroup::GetDsSwizzleQuadMode(
    uint8_t lane0, // The 0th lane.
    uint8_t lane1, // The 1st lane.
    uint8_t lane2, // The 2nd lane.
    uint8_t lane3) // The 3rd lane.
{
    return 0x8000 | static_cast<uint16_t>((lane3 << 6) | ((lane2 & 0x3) << 4) | ((lane1 & 0x3) << 2) | ((lane0 & 0x3)));
}

// =====================================================================================================================
// Create a thread mask for the current thread, a 64-bit integer with a single bit representing the ID of the thread
// set to 1.
Value* BuilderImplSubgroup::CreateThreadMask()
{
    Value* pThreadId = CreateMbcnt(getInt64(UINT64_MAX));
    pThreadId = CreateZExtOrTrunc(pThreadId, getInt64Ty());
    return CreateShl(getInt64(1), pThreadId);
}

// =====================================================================================================================
// Create a masked operation - taking a thread mask and a mask to and it with, select between the first value and the
// second value if the current thread is active.
Value* BuilderImplSubgroup::CreateThreadMaskedSelect(
    Value* const pThreadMask, // [in] The thread mask, must come from a call to CreateThreadMask. Must be an i64 type.
    uint64_t     andMask,     // The mask to and with the thread mask, must be an i64 type.
    Value* const pValue1,     // [in] The first value to select.
    Value* const pValue2)     // [in] The second value to select.
{
    return CreateSelect(CreateICmpNE(CreateAnd(pThreadMask, getInt64(andMask)), getInt64(0)), pValue1, pValue2);
}

// =====================================================================================================================
// Do group ballot, turning a per-lane boolean value (in a VGPR) into a subgroup-wide shared SGPR.
Value* BuilderImplSubgroup::CreateGroupBallot(
    Value* const pValue) // [in] The value to contribute to the SGPR, must be an boolean type.
{
    // Check the type is definitely an boolean.
    LLPC_ASSERT(pValue->getType()->isIntegerTy(1));

    // Turn value into an i32.
    Value* pValueAsInt32 = CreateSelect(pValue, getInt32(1), getInt32(0));

    // TODO: There is a longstanding bug with LLVM's convergent that forces us to use inline assembly with sideffects to
    // stop any hoisting out of control flow.
    pValueAsInt32 = CreateInlineAsmSideEffect(pValueAsInt32);

    // The not equal predicate for the icmp intrinsic is 33.
    Constant* const pPredicateNE = getInt32(33);
    
    return CreateIntrinsic(Intrinsic::amdgcn_icmp, getInt32Ty(), { pValueAsInt32, getInt32(0), pPredicateNE });
}
