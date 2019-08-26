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
    return getInt32(GetShaderSubgroupSize());
}

// =====================================================================================================================
// Get the shader subgroup size for the current insertion block.
uint32_t BuilderImplSubgroup::GetShaderSubgroupSize()
{
    return getContext().GetShaderWaveSize(GetShaderStageFromFunction(GetInsertBlock()->getParent()));
}

// =====================================================================================================================
// Create a subgroup elect call.
Value* BuilderImplSubgroup::CreateSubgroupElect(
    const Twine& instName) // [in] Name to give final instruction.
{
    return CreateICmpEQ(CreateSubgroupMbcnt(CreateGroupBallot(getTrue()), ""), getInt32(0));
}

// =====================================================================================================================
// Create a subgroup all call.
Value* BuilderImplSubgroup::CreateSubgroupAll(
    Value* const pValue,   // [in] The value to compare across the subgroup. Must be an integer type.
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = CreateICmpEQ(CreateGroupBallot(pValue), CreateGroupBallot(getTrue()));
    pResult = CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, pValue), pValue, pResult);

    // Helper invocations of whole quad mode should be included in the subgroup vote execution
    if (wqm)
    {
        pResult = CreateZExt(pResult, getInt32Ty());
        pResult = CreateIntrinsic(Intrinsic::amdgcn_softwqm, { getInt32Ty() }, { pResult });
        pResult = CreateTrunc(pResult, getInt1Ty());
    }
    return pResult;
}

// =====================================================================================================================
// Create a subgroup any call.
Value* BuilderImplSubgroup::CreateSubgroupAny(
    Value* const pValue,   // [in] The value to compare across the subgroup. Must be an integer type.
    bool         wqm,      // Executed in WQM (whole quad mode)
    const Twine& instName) // [in] Name to give final instruction.
{
    Value* pResult = CreateICmpNE(CreateGroupBallot(pValue), getInt64(0));
    pResult = CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, pValue), pValue, pResult);

    // Helper invocations of whole quad mode should be included in the subgroup vote execution
    if (wqm)
    {
        pResult = CreateZExt(pResult, getInt32Ty());
        pResult = CreateIntrinsic(Intrinsic::amdgcn_softwqm, { getInt32Ty() }, { pResult });
        pResult = CreateTrunc(pResult, getInt1Ty());
    }
    return pResult;
}

// =====================================================================================================================
// Create a subgroup all equal call.
Value* BuilderImplSubgroup::CreateSubgroupAllEqual(
    Value* const pValue,   // [in] The value to compare across the subgroup. Must be an integer type.
    bool         wqm,      // Executed in WQM (whole quad mode)
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
        Value* pResult = CreateExtractElement(pCompare, getInt32(0));

        for (uint32_t i = 1, compCount = pType->getVectorNumElements(); i < compCount; i++)
        {
            pResult = CreateAnd(pResult, CreateExtractElement(pCompare, i));
        }

        return CreateSubgroupAll(pResult, wqm, instName);
    }
    else
    {
        return CreateSubgroupAll(pCompare, wqm, instName);
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
    return CreateSubgroupBallotBitExtract(pValue, CreateSubgroupMbcnt(getInt64(UINT64_MAX), ""), instName);
}

// =====================================================================================================================
// Create a subgroup ballotbitextract call.
Value* BuilderImplSubgroup::CreateSubgroupBallotBitExtract(
    Value* const pValue,   // [in] The ballot value to bit extract. Must be an <4 x i32> type.
    Value* const pIndex,   // [in] The bit index to extract. Must be an i32 type.
    const Twine& instName) // [in] Name to give final instruction.
{
#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        Value* const pIndexMask = CreateShl(getInt32(1), pIndex);
        Value* const pValueAsInt32 = CreateExtractElement(pValue, getInt32(0));
        Value* const pResult = CreateAnd(pIndexMask, pValueAsInt32);
        return CreateICmpNE(pResult, getInt32(0));
    }
    else
#endif
    {
        Value* pIndexMask = CreateZExtOrTrunc(pIndex, getInt64Ty());
        pIndexMask = CreateShl(getInt64(1), pIndexMask);
        Value* pValueAsInt64 = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
        pValueAsInt64 = CreateBitCast(pValueAsInt64, getInt64Ty());
        Value* const pResult = CreateAnd(pIndexMask, pValueAsInt64);
        return CreateICmpNE(pResult, getInt64(0));
    }
}

// =====================================================================================================================
// Create a subgroup ballotbitcount call.
Value* BuilderImplSubgroup::CreateSubgroupBallotBitCount(
    Value* const pValue,   // [in] The ballot value to bit count. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        return CreateUnaryIntrinsic(Intrinsic::ctpop, CreateExtractElement(pValue, getInt32(0)));
    }
    else
#endif
    {
        Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
        pResult = CreateBitCast(pResult, getInt64Ty());
        pResult = CreateUnaryIntrinsic(Intrinsic::ctpop, pResult);
        return CreateZExtOrTrunc(pResult, getInt32Ty());
    }
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
#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        return CreateSubgroupMbcnt(CreateExtractElement(pValue, getInt32(0)), "");
    }
    else
#endif
    {
        Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
        pResult = CreateBitCast(pResult, getInt64Ty());
        return CreateSubgroupMbcnt(pResult, "");
    }
}

// =====================================================================================================================
// Create a subgroup ballotfindlsb call.
Value* BuilderImplSubgroup::CreateSubgroupBallotFindLsb(
    Value* const pValue, // [in] The ballot value to find the least significant bit of. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        Value* const pResult = CreateExtractElement(pValue, getInt32(0));
        return CreateIntrinsic(Intrinsic::cttz, getInt32Ty(), { pResult, getTrue() });
    }
    else
#endif
    {
        Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
        pResult = CreateBitCast(pResult, getInt64Ty());
        pResult = CreateIntrinsic(Intrinsic::cttz, getInt64Ty(), { pResult, getTrue() });
        return CreateZExtOrTrunc(pResult, getInt32Ty());
    }
}

// =====================================================================================================================
// Create a subgroup ballotfindmsb call.
Value* BuilderImplSubgroup::CreateSubgroupBallotFindMsb(
    Value* const pValue,   // [in] The ballot value to find the most significant bit of. Must be an <4 x i32> type.
    const Twine& instName) // [in] Name to give final instruction.
{
#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        Value* pResult = CreateExtractElement(pValue, getInt32(0));
        pResult = CreateIntrinsic(Intrinsic::ctlz, getInt32Ty(), { pResult, getTrue() });
        return CreateSub(getInt32(31), pResult);
    }
    else
#endif
    {
        Value* pResult = CreateShuffleVector(pValue, UndefValue::get(pValue->getType()), { 0, 1 });
        pResult = CreateBitCast(pResult, getInt64Ty());
        pResult = CreateIntrinsic(Intrinsic::ctlz, getInt64Ty(), { pResult, getTrue() });
        pResult = CreateZExtOrTrunc(pResult, getInt32Ty());
        return CreateSub(getInt32(63), pResult);
    }
}

// =====================================================================================================================
// Create a subgroup shuffle call.
Value* BuilderImplSubgroup::CreateSubgroupShuffle(
    Value* const pValue,   // [in] The value to shuffle.
    Value* const pIndex,   // [in] The index to shuffle from.
    const Twine& instName) // [in] Name to give final instruction.
{
    if (SupportBPermute())
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
    Value* pIndex = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
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
    Value* pIndex = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
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
    Value* pIndex = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
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

#if LLPC_BUILD_GFX10
        if (SupportPermLaneDpp())
        {
            // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
            pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreatePermLaneX16(pResult, pResult, UINT32_MAX, UINT32_MAX, true, false)), pResult);

            Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);
            Value* const pBroadcast63 = CreateSubgroupBroadcast(pResult, getInt32(63), instName);

            // Combine broadcast from the 31st and 63rd for the final result.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
                CreateGroupArithmeticOperation(groupArithOp, pBroadcast31, pBroadcast63), pResult);
        }
        else
#endif
        {
            // Use a row broadcast to move the 15th element in each cluster of 16 to the next cluster. The row mask is
            // set to 0xa (0b1010) so that only the 2nd and 4th clusters of 16 perform the calculation.
            pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreateDppMov(pResult, DppCtrl::DppRowBcast15, 0xA, 0xF, 0)), pResult);

            // Use a row broadcast to move the 31st element from the lower cluster of 32 to the upper cluster. The row
            // mask is set to 0x8 (0b1000) so that only the upper cluster of 32 perform the calculation.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreateDppMov(pResult, DppCtrl::DppRowBcast31, 0x8, 0xF, 0)), pResult);

            Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);
            Value* const pBroadcast63 = CreateSubgroupBroadcast(pResult, getInt32(63), instName);

            // If the cluster size is 64 we always read the value from the last invocation in the subgroup.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)), pBroadcast63, pResult);

            Value* const pLaneIdLessThan32 = CreateICmpULT(CreateSubgroupMbcnt(getInt64(UINT64_MAX), ""), getInt32(32));

            // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
            // invocation 31 or 63's value.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(32)),
                CreateSelect(pLaneIdLessThan32, pBroadcast31, pBroadcast63),
                    pResult);
        }

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

        Value* const pThreadId = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");

        // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
        // invocation 31 or 63's value.
        pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(32)),
            CreateSelect(CreateICmpULT(pThreadId, getInt32(32)), pBroadcast31, pBroadcast63),
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

#if LLPC_BUILD_GFX10
        if (SupportPermLaneDpp())
        {
            Value* const pThreadMask = CreateThreadMask();

            Value* const pMaskedPermLane = CreateThreadMaskedSelect(pThreadMask, 0xFFFF0000FFFF0000,
                CreatePermLaneX16(pResult, pResult, UINT32_MAX, UINT32_MAX, true, false), pIdentity);

            // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
            pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
                CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedPermLane), pResult);

            Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);

            Value* const pMaskedBroadcast = CreateThreadMaskedSelect(pThreadMask, 0xFFFFFFFF00000000,
                pBroadcast31, pIdentity);

            // Combine broadcast of 31 with the top two rows only.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
                CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedBroadcast), pResult);
        }
        else
#endif
        {
            // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the
            // operation.
            pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast15, 0xA, 0xF, 0)), pResult);

            // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the
            // operation.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast31, 0xC, 0xF, 0)), pResult);
        }

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

        // The DS swizzle is or'ing by 0x0 with an and mask of 0x1E, which swaps from N <-> N+1. We don't want the N's
        // to perform the operation, only the N+1's, so we use a mask of 0xA (0b1010) to stop the N's doing anything.
        Value* pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xAAAAAAAAAAAAAAAA,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x00, 0x00, 0x1E)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0x1 with an and mask of 0x1C, which swaps from N <-> N+2. We don't want the N's
        // to perform the operation, only the N+2's, so we use a mask of 0xC (0b1100) to stop the N's doing anything.
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
        // to perform the operation, only the N+8's, so we use a mask of 0xFF00 (0b1111111100000000) to stop the N's
        // doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xFF00FF00FF00FF00,
            CreateDsSwizzle(pResult, GetDsSwizzleBitMode(0x00, 0x07, 0x10)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
        // to perform the operation, only the N+16's, so we use a mask of 0xFFFF0000
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

        Value* pShiftRight = nullptr;

#if LLPC_BUILD_GFX10
        if (SupportPermLaneDpp())
        {
            Value* const pThreadMask = CreateThreadMask();

            // Shift right within each row:
            // ‭0b0110,0101,0100,0011,0010,0001,0000,1111‬ = 0x‭‭6543210F‬
            // ‭0b1110,1101,1100,1011,1010,1001,1000,0111‬ = 0xEDCBA987
            pShiftRight = CreatePermLane16(pSetInactive,
                                           pSetInactive,
                                           0x6543210F,
                                           0xEDCBA987,
                                           true,
                                           false);

            // Only needed for wave size 64.
            if (GetShaderSubgroupSize() == 64)
            {
                // Need to write the value from the 16th invocation into the 48th.
                pShiftRight = CreateSubgroupWriteInvocation(pShiftRight,
                                                            CreateSubgroupBroadcast(pShiftRight, getInt32(16), ""),
                                                            getInt32(48),
                                                            "");
            }

            pShiftRight = CreateSubgroupWriteInvocation(pShiftRight, pIdentity, getInt32(16), "");

            // Exchange first column value cross rows(row 1<--> row 0, row 3<-->row2)
            // Only first column value from each row join permlanex
            pShiftRight = CreateThreadMaskedSelect(pThreadMask, 0x0001000100010001,
                CreatePermLaneX16(pShiftRight, pShiftRight, 0, UINT32_MAX, true, false), pShiftRight);
        }
        else
#endif
        {
            // Shift the whole subgroup right by one, using a DPP update operation. This will ensure that the identity
            // value is in the 0th invocation and all other values are shifted up. All rows and banks are active (0xF).
            pShiftRight = CreateDppUpdate(pIdentity, pSetInactive, DppCtrl::DppWfSr1, 0xF, 0xF, 0);
        }

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

#if LLPC_BUILD_GFX10
        if (SupportPermLaneDpp())
        {
            Value* const pThreadMask = CreateThreadMask();

            Value* const pMaskedPermLane = CreateThreadMaskedSelect(pThreadMask, 0xFFFF0000FFFF0000,
                CreatePermLaneX16(pResult, pResult, UINT32_MAX, UINT32_MAX, true, false), pIdentity);

            // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
            pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
                CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedPermLane), pResult);

            Value* const pBroadcast31 = CreateSubgroupBroadcast(pResult, getInt32(31), instName);

            Value* const pMaskedBroadcast = CreateThreadMaskedSelect(pThreadMask, 0xFFFFFFFF00000000,
                pBroadcast31, pIdentity);

            // Combine broadcast of 31 with the top two rows only.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
                CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedBroadcast), pResult);
        }
        else
#endif
        {
            // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the
            // operation.
            pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast15, 0xA, 0xF, 0)), pResult);

            // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the
            // operation.
            pResult = CreateSelect(CreateICmpEQ(pClusterSize, getInt32(64)),
                CreateGroupArithmeticOperation(groupArithOp, pResult,
                    CreateDppUpdate(pIdentity, pResult, DppCtrl::DppRowBcast31, 0xC, 0xF, 0)), pResult);
        }

        // Finish the WWM section by calling the intrinsic.
        return CreateWwm(pResult);
    }
    else
    {
        Value* const pThreadMask = CreateThreadMask();

        Value* const pIdentity = CreateGroupArithmeticIdentity(groupArithOp, pValue->getType());

        // Start the WWM section by setting the inactive invocations.
        Value* const pSetInactive = CreateSetInactive(pValue, pIdentity);
        Value* pResult = pIdentity;

        // The DS swizzle is or'ing by 0x0 with an and mask of 0x1E, which swaps from N <-> N+1. We don't want the N's
        // to perform the operation, only the N+1's, so we use a mask of 0xA (0b1010) to stop the N's doing anything.
        Value* pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xAAAAAAAAAAAAAAAA,
            CreateDsSwizzle(pSetInactive, GetDsSwizzleBitMode(0x00, 0x00, 0x1E)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(2)), pMaskedSwizzle, pResult);

        // The DS swizzle is or'ing by 0x1 with an and mask of 0x1C, which swaps from N <-> N+2. We don't want the N's
        // to perform the operation, only the N+2's, so we use a mask of 0xC (0b1100) to stop the N's doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xCCCCCCCCCCCCCCCC,
            CreateDsSwizzle(CreateGroupArithmeticOperation(groupArithOp, pResult, pSetInactive),
                GetDsSwizzleBitMode(0x00, 0x01, 0x1C)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(4)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0x3 with an and mask of 0x18, which swaps from N <-> N+4. We don't want the N's
        // to perform the operation, only the N+4's, so we use a mask of 0xF0 (0b11110000) to stop the N's doing
        // anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xF0F0F0F0F0F0F0F0,
            CreateDsSwizzle(CreateGroupArithmeticOperation(groupArithOp, pResult, pSetInactive),
                GetDsSwizzleBitMode(0x00, 0x03, 0x18)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(8)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0x7 with an and mask of 0x10, which swaps from N <-> N+8. We don't want the N's
        // to perform the operation, only the N+8's, so we use a mask of 0xFF00 (0b1111111100000000) to stop the N's
        // doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xFF00FF00FF00FF00,
            CreateDsSwizzle(CreateGroupArithmeticOperation(groupArithOp, pResult, pSetInactive),
                GetDsSwizzleBitMode(0x00, 0x07, 0x10)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(16)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
        // to perform the operation, only the N+16's, so we use a mask of 0xFFFF0000
        // (0b11111111111111110000000000000000) to stop the N's doing anything.
        pMaskedSwizzle = CreateThreadMaskedSelect(pThreadMask, 0xFFFF0000FFFF0000,
            CreateDsSwizzle(CreateGroupArithmeticOperation(groupArithOp, pResult, pSetInactive),
                GetDsSwizzleBitMode(0x00, 0x0F, 0x00)), pIdentity);
        pResult = CreateSelect(CreateICmpUGE(pClusterSize, getInt32(32)),
            CreateGroupArithmeticOperation(groupArithOp, pResult, pMaskedSwizzle), pResult);

        Value* const pBroadcast31 = CreateSubgroupBroadcast(
            CreateGroupArithmeticOperation(groupArithOp, pResult, pSetInactive), getInt32(31), instName);

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
    Value* pResult = UndefValue::get(pValue->getType());

    const uint32_t indexBits = pIndex->getType()->getPrimitiveSizeInBits();

    if (SupportDpp())
    {
        Value* pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 0));
        pResult = CreateSelect(pCompare, CreateDppMov(pValue, DppCtrl::DppQuadPerm0000, 0xF, 0xF, false), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 1));
        pResult = CreateSelect(pCompare, CreateDppMov(pValue, DppCtrl::DppQuadPerm1111, 0xF, 0xF, false), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 2));
        pResult = CreateSelect(pCompare, CreateDppMov(pValue, DppCtrl::DppQuadPerm2222, 0xF, 0xF, false), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 3));
        pResult = CreateSelect(pCompare, CreateDppMov(pValue, DppCtrl::DppQuadPerm3333, 0xF, 0xF, false), pResult);
    }
    else
    {
        Value* pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 0));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(0, 0, 0, 0)), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 1));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(1, 1, 1, 1)), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 2));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(2, 2, 2, 2)), pResult);

        pCompare = CreateICmpEQ(pIndex, getIntN(indexBits, 3));
        pResult = CreateSelect(pCompare, CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(3, 3, 3, 3)), pResult);
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
        return CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(1, 0, 3, 2));
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
        return CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(2, 3, 0, 1));
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
// Create a subgroup quad swap swizzle.
Value* BuilderImplSubgroup::CreateSubgroupSwizzleQuad(
    Value* const pValue,   // [in] The value to swizzle.
    Value* const pOffset,  // [in] The value to specify the swizzle offsets.
    const Twine& instName) // [in] Name to give instruction(s)
{
    Constant* const pConstOffset = cast<Constant>(pOffset);
    uint8_t lane0 = static_cast<uint8_t>(cast<ConstantInt>(pConstOffset->getAggregateElement(0u))->getZExtValue());
    uint8_t lane1 = static_cast<uint8_t>(cast<ConstantInt>(pConstOffset->getAggregateElement(1u))->getZExtValue());
    uint8_t lane2 = static_cast<uint8_t>(cast<ConstantInt>(pConstOffset->getAggregateElement(2u))->getZExtValue());
    uint8_t lane3 = static_cast<uint8_t>(cast<ConstantInt>(pConstOffset->getAggregateElement(3u))->getZExtValue());

    return CreateDsSwizzle(pValue, GetDsSwizzleQuadMode(lane0, lane1, lane2, lane3));
}

// =====================================================================================================================
// Create a subgroup swizzle mask.
Value* BuilderImplSubgroup::CreateSubgroupSwizzleMask(
    Value* const pValue,   // [in] The value to swizzle.
    Value* const pMask,    // [in] The value to specify the swizzle masks.
    const Twine& instName) // [in] Name to give instruction(s)
{
    Constant* const pConstMask = cast<Constant>(pMask);
    uint8_t andMask = static_cast<uint8_t>(cast<ConstantInt>(pConstMask->getAggregateElement(0u))->getZExtValue());
    uint8_t orMask = static_cast<uint8_t>(cast<ConstantInt>(pConstMask->getAggregateElement(1u))->getZExtValue());
    uint8_t xorMask = static_cast<uint8_t>(cast<ConstantInt>(pConstMask->getAggregateElement(2u))->getZExtValue());

    LLPC_ASSERT((andMask <= 31) && (orMask <= 31) && (xorMask <= 31));

    return CreateDsSwizzle(pValue, GetDsSwizzleBitMode(xorMask, orMask, andMask));
}

// =====================================================================================================================
// Create a subgroup write invocation.
Value* BuilderImplSubgroup::CreateSubgroupWriteInvocation(
    Value* const pInputValue,      // [in] The value to return for all but one invocations.
    Value* const pWriteValue,      // [in] The value to return for one invocation.
    Value* const pInvocationIndex, // [in] The index of the invocation that gets the write value.
    const Twine& instName)         // [in] Name to give instruction(s)
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        return builder.CreateIntrinsic(Intrinsic::amdgcn_writelane,
                                       {},
                                       {
                                            mappedArgs[1],
                                            passthroughArgs[0],
                                            mappedArgs[0],
                                       });
    };

    return CreateMapToInt32(pfnMapFunc, { pInputValue, pWriteValue }, pInvocationIndex);
}

// =====================================================================================================================
// Create a subgroup mbcnt.
Value* BuilderImplSubgroup::CreateSubgroupMbcnt(
    Value* const pMask,    // [in] The mask to mbcnt with.
    const Twine& instName) // [in] Name to give instruction(s)
{
    // Check that the type is definitely an i64.
    LLPC_ASSERT(pMask->getType()->isIntegerTy(64));

    Value* const pMasks = CreateBitCast(pMask, VectorType::get(getInt32Ty(), 2));
    Value* const pMaskLow = CreateExtractElement(pMasks, getInt32(0));
    Value* const pMaskHigh = CreateExtractElement(pMasks, getInt32(1));
    CallInst* const pMbcntLo = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, { pMaskLow, getInt32(0) });

#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        return pMbcntLo;
    }
    else
#endif
    {
        return CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, { pMaskHigh, pMbcntLo });
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
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*>) -> Value*
    {
        Value* const pValue = mappedArgs[0];
        Type* const pType = pValue->getType();
        FunctionType* const pFuncType = FunctionType::get(pType, pType, false);
        InlineAsm* const pInlineAsm = InlineAsm::get(pFuncType, "; %1", "=v,0", true);
        return builder.CreateCall(pInlineAsm, pValue);
    };

    return CreateMapToInt32(pfnMapFunc, pValue, {});
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

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Create a call to permute lane.
Value* BuilderImplSubgroup::CreatePermLane16(
    Value* const pOrigValue,     // [in] The original value we are going to update.
    Value* const pUpdateValue,   // [in] The value to update with.
    uint32_t     selectBitsLow,  // Select bits low.
    uint32_t     selectBitsHigh, // Select bits high.
    bool         fetchInactive,  // FI mode, whether to fetch inactive lane.
    bool         boundCtrl)      // Whether bound_ctrl is used or not.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        Module* const pModule = builder.GetInsertBlock()->getModule();

        Type* const pInt1Ty = builder.getInt1Ty();
        Type* const pInt32Ty = builder.getInt32Ty();

        FunctionCallee function = pModule->getOrInsertFunction("llvm.amdgcn.permlane16",
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt1Ty,
                                                               pInt1Ty);

        // TODO: Once GFX10 intrinsic amdgcn_permlane16 has been upstreamed, used CreateIntrinsic here.
        return builder.CreateCall(function.getCallee(),
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
                                getInt32(selectBitsLow),
                                getInt32(selectBitsHigh),
                                getInt1(fetchInactive),
                                getInt1(boundCtrl)
                          });
}

// =====================================================================================================================
// Create a call to permute lane.
Value* BuilderImplSubgroup::CreatePermLaneX16(
    Value* const pOrigValue,     // [in] The original value we are going to update.
    Value* const pUpdateValue,   // [in] The value to update with.
    uint32_t     selectBitsLow,  // Select bits low.
    uint32_t     selectBitsHigh, // Select bits high.
    bool         fetchInactive,  // FI mode, whether to fetch inactive lane.
    bool         boundCtrl)      // Whether bound_ctrl is used or not.
{
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*> passthroughArgs) -> Value*
    {
        Module* const pModule = builder.GetInsertBlock()->getModule();

        Type* const pInt1Ty = builder.getInt1Ty();
        Type* const pInt32Ty = builder.getInt32Ty();

        FunctionCallee function = pModule->getOrInsertFunction("llvm.amdgcn.permlanex16",
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt32Ty,
                                                               pInt1Ty,
                                                               pInt1Ty);

        // TODO: Once GFX10 intrinsic amdgcn_permlanex16 has been upstreamed, used CreateIntrinsic here.
        return builder.CreateCall(function.getCallee(),
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
                                getInt32(selectBitsLow),
                                getInt32(selectBitsHigh),
                                getInt1(fetchInactive),
                                getInt1(boundCtrl)
                          });
}
#endif

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
    auto pfnMapFunc = [](Builder& builder, ArrayRef<Value*> mappedArgs, ArrayRef<Value*>) -> Value*
    {
        Value* const pActive = mappedArgs[0];
        Value* const pInactive = mappedArgs[1];
        return builder.CreateIntrinsic(Intrinsic::amdgcn_set_inactive, pActive->getType(), { pActive, pInactive });
    };

    return CreateMapToInt32(pfnMapFunc, { CreateInlineAsmSideEffect(pActive), pInactive }, {});
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
// Create a thread mask for the current thread, an integer with a single bit representing the ID of the thread set to 1.
Value* BuilderImplSubgroup::CreateThreadMask()
{
    Value* pThreadId = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");

#if LLPC_BUILD_GFX10
    if (GetShaderSubgroupSize() <= 32)
    {
        pThreadId = CreateShl(getInt32(1), pThreadId);
    }
    else
#endif
    {
        pThreadId = CreateShl(getInt64(1), CreateZExtOrTrunc(pThreadId, getInt64Ty()));
    }

    return pThreadId;
}

// =====================================================================================================================
// Create a masked operation - taking a thread mask and a mask to and it with, select between the first value and the
// second value if the current thread is active.
Value* BuilderImplSubgroup::CreateThreadMaskedSelect(
    Value* const pThreadMask, // [in] The thread mask, must come from a call to CreateThreadMask.
    uint64_t     andMask,     // The mask to and with the thread mask.
    Value* const pValue1,     // [in] The first value to select.
    Value* const pValue2)     // [in] The second value to select.
{
    Value* const pAndMask = getIntN(GetShaderSubgroupSize(), andMask);
    Value* const pZero = getIntN(GetShaderSubgroupSize(), 0);
    return CreateSelect(CreateICmpNE(CreateAnd(pThreadMask, pAndMask), pZero), pValue1, pValue2);
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

    SmallVector<Type*, 2> types;

    // icmp has a new signature (requiring the return type as the first type).
    types.push_back(getIntNTy(GetShaderSubgroupSize()));
    types.push_back(getInt32Ty());

    Value* pResult = CreateIntrinsic(Intrinsic::amdgcn_icmp,
                                     types,
                                     {
                                          pValueAsInt32,
                                          getInt32(0),
                                          pPredicateNE
                                     });

#if LLPC_BUILD_GFX10
    // If we have a 32-bit subgroup size, we need to turn the 32-bit ballot result into a 64-bit result.
    if (GetShaderSubgroupSize() <= 32)
    {
        pResult = CreateZExt(pResult, getInt64Ty(), "");
    }
#endif
    return pResult;
}
