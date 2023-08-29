/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  SubgroupBuilder.cpp
 * @brief LLPC source file: implementation of subgroup Builder methods
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-builder-impl-subgroup"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Get shader wave size.
//
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateGetWaveSize(const Twine &instName) {
  return getInt32(getShaderWaveSize());
}

// =====================================================================================================================
// Create a subgroup get subgroup size.
//
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateGetSubgroupSize(const Twine &instName) {
  return getInt32(getShaderSubgroupSize());
}

// =====================================================================================================================
// Get the shader subgroup size for the current shader stage.
//
// @returns : Subgroup size of current shader stage
unsigned BuilderImpl::getShaderSubgroupSize() {
  return getPipelineState()->getShaderSubgroupSize(getShaderStage(GetInsertBlock()->getParent()));
}

// =====================================================================================================================
// Get the shader wave size for the current shader stage.
//
// @returns : Wave size of current shader stage
unsigned BuilderImpl::getShaderWaveSize() {
  return getPipelineState()->getShaderWaveSize(getShaderStage(GetInsertBlock()->getParent()));
}

// =====================================================================================================================
// Create a subgroup elect call.
//
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupElect(const Twine &instName) {
  return CreateICmpEQ(CreateSubgroupMbcnt(createGroupBallot(getTrue()), ""), getInt32(0));
}

// =====================================================================================================================
// Create a subgroup all call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupAll(Value *const value, const Twine &instName) {
  Value *result = CreateICmpEQ(createGroupBallot(value), createGroupBallot(getTrue()));
  result = CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, value), value, result);

  // Helper invocations of whole quad mode should be included in the subgroup vote execution
  if (m_shaderStage == ShaderStageFragment) {
    result = CreateZExt(result, getInt32Ty());
    result = CreateIntrinsic(Intrinsic::amdgcn_softwqm, {getInt32Ty()}, {result});
    result = CreateTrunc(result, getInt1Ty());
  }
  return result;
}

// =====================================================================================================================
// Create a subgroup any call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupAny(Value *const value, const Twine &instName) {
  Value *result = CreateICmpNE(createGroupBallot(value), getInt64(0));
  result = CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, value), value, result);

  // Helper invocations of whole quad mode should be included in the subgroup vote execution
  if (m_shaderStage == ShaderStageFragment) {
    result = CreateZExt(result, getInt32Ty());
    result = CreateIntrinsic(Intrinsic::amdgcn_softwqm, {getInt32Ty()}, {result});
    result = CreateTrunc(result, getInt1Ty());
  }
  return result;
}

// =====================================================================================================================
// Create a subgroup all equal call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupAllEqual(Value *const value, const Twine &instName) {
  Type *const type = value->getType();

  Value *compare = CreateSubgroupBroadcastFirst(value, instName);

  if (type->isFPOrFPVectorTy())
    compare = CreateFCmpOEQ(compare, value);
  else {
    assert(type->isIntOrIntVectorTy());
    compare = CreateICmpEQ(compare, value);
  }

  if (type->isVectorTy()) {
    Value *result = CreateExtractElement(compare, getInt32(0));

    for (unsigned i = 1, compCount = cast<FixedVectorType>(type)->getNumElements(); i < compCount; i++)
      result = CreateAnd(result, CreateExtractElement(compare, i));

    return CreateSubgroupAll(result, instName);
  }
  return CreateSubgroupAll(compare, instName);
}

// =====================================================================================================================
// Create a subgroup rotate call.
//
// @param value : The value to read from the chosen rotated lane to all active lanes.
// @param delta : The delta/offset added to lane id.
// @param clusterSize : The cluster size if exists.
// @param instName : Name to give instruction.
Value *BuilderImpl::CreateSubgroupRotate(Value *const value, Value *const delta, Value *const clusterSize,
                                         const Twine &instName) {
  // LocalId = SubgroupLocalInvocationId
  // RotationGroupSize = hasClusterSize ? ClusterSize : SubgroupSize
  // InvocationId = ((LocalId + Delta) & (RotationGroupSize - 1)) + (LocalId & ~(RotationGroupSize - 1))
  Value *localId = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
  Value *invocationId = CreateAdd(localId, delta);
  if (clusterSize != nullptr) {
    Value *rotationGroupSize = CreateSub(clusterSize, getInt32(1));
    invocationId =
        CreateOr(CreateAnd(invocationId, rotationGroupSize), CreateAnd(localId, CreateNot(rotationGroupSize)));
  }

  return CreateSubgroupShuffle(value, invocationId, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast call.
//
// @param value : The value to read from the chosen lane to all active lanes.
// @param index : The index to broadcast from. Must be an i32.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBroadcast(Value *const value, Value *const index, const Twine &instName) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readlane,
                                   {mappedArgs[0], passthroughArgs[0]});
  };

  return CreateMapToInt32(mapFunc, value, index);
}

// =====================================================================================================================
// Create a subgroup broadcast call using waterfall for non-uniform index
//
// @param value : The value to read from the chosen lane to all active lanes.
// @param index : The index to broadcast from. Must be an i32.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBroadcastWaterfall(Value *const value, Value *const index, const Twine &instName) {
  auto mapFunc = [this](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    Value *const readlane =
        builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readlane, {mappedArgs[0], passthroughArgs[0]});
    return createWaterfallLoop(cast<Instruction>(readlane), 1);
  };
  return CreateMapToInt32(mapFunc, value, index);
}

// =====================================================================================================================
// Create a subgroup broadcastfirst call.
//
// @param value : The value to read from the first active lane into all other active lanes.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBroadcastFirst(Value *const value, const Twine &instName) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readfirstlane, mappedArgs[0]);
  };

  return CreateMapToInt32(mapFunc, {BuilderBase::get(*this).CreateInlineAsmSideEffect(value)}, {});
}

// =====================================================================================================================
// Create a subgroup ballot call.
//
// @param value : The value to ballot across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallot(Value *const value, const Twine &instName) {
  // Check the type is definitely an integer.
  assert(value->getType()->isIntegerTy());

  Value *ballot = createGroupBallot(value);

  // Ballot expects a <4 x i32> return, so we need to turn the i64 into that.
  ballot = CreateBitCast(ballot, FixedVectorType::get(getInt32Ty(), 2));

  ElementCount elementCount = cast<VectorType>(ballot->getType())->getElementCount();
  return CreateShuffleVector(ballot, ConstantVector::getSplat(elementCount, getInt32(0)), ArrayRef<int>{0, 1, 2, 3});
}

// =====================================================================================================================
// Create a subgroup inverseballot call.
//
// @param value : The value to inverseballot across the subgroup. Must be a <4 x i32> type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupInverseBallot(Value *const value, const Twine &instName) {
  return CreateSubgroupBallotBitExtract(value, CreateSubgroupMbcnt(getInt64(UINT64_MAX), ""), instName);
}

// =====================================================================================================================
// Create a subgroup ballotbitextract call.
//
// @param value : The ballot value to bit extract. Must be an <4 x i32> type.
// @param index : The bit index to extract. Must be an i32 type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallotBitExtract(Value *const value, Value *const index, const Twine &instName) {
  if (getShaderSubgroupSize() <= 32) {
    Value *const indexMask = CreateShl(getInt32(1), index);
    Value *const valueAsInt32 = CreateExtractElement(value, getInt32(0));
    Value *const result = CreateAnd(indexMask, valueAsInt32);
    return CreateICmpNE(result, getInt32(0));
  }
  Value *indexMask = CreateZExtOrTrunc(index, getInt64Ty());
  indexMask = CreateShl(getInt64(1), indexMask);
  Value *valueAsInt64 = CreateShuffleVector(value, PoisonValue::get(value->getType()), ArrayRef<int>{0, 1});
  valueAsInt64 = CreateBitCast(valueAsInt64, getInt64Ty());
  Value *const result = CreateAnd(indexMask, valueAsInt64);
  return CreateICmpNE(result, getInt64(0));
}

// =====================================================================================================================
// Create a subgroup ballotbitcount call.
//
// @param value : The ballot value to bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallotBitCount(Value *const value, const Twine &instName) {
  if (getShaderSubgroupSize() <= 32)
    return CreateUnaryIntrinsic(Intrinsic::ctpop, CreateExtractElement(value, getInt32(0)));
  Value *result = CreateShuffleVector(value, PoisonValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = CreateBitCast(result, getInt64Ty());
  result = CreateUnaryIntrinsic(Intrinsic::ctpop, result);
  return CreateZExtOrTrunc(result, getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup ballotinclusivebitcount call.
//
// @param value : The ballot value to inclusively bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallotInclusiveBitCount(Value *const value, const Twine &instName) {
  Value *const exclusiveBitCount = CreateSubgroupBallotExclusiveBitCount(value, instName);
  Value *const inverseBallot = CreateSubgroupInverseBallot(value, instName);
  Value *const inclusiveBitCount = CreateAdd(exclusiveBitCount, getInt32(1));
  return CreateSelect(inverseBallot, inclusiveBitCount, exclusiveBitCount);
}

// =====================================================================================================================
// Create a subgroup ballotexclusivebitcount call.
//
// @param value : The ballot value to exclusively bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallotExclusiveBitCount(Value *const value, const Twine &instName) {
  if (getShaderSubgroupSize() <= 32)
    // Directly invoke the required mbcnt_lo intrinsic since CreateSubgroupMbcnt expects a 64-bit mask
    return CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {CreateExtractElement(value, getInt32(0)), getInt32(0)});
  Value *result = CreateShuffleVector(value, PoisonValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = CreateBitCast(result, getInt64Ty());
  return CreateSubgroupMbcnt(result, "");
}

// =====================================================================================================================
// Create a subgroup ballotfindlsb call.
//
// @param value : The ballot value to find the least significant bit of. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallotFindLsb(Value *const value, const Twine &instName) {
  if (getShaderSubgroupSize() <= 32) {
    Value *const result = CreateExtractElement(value, getInt32(0));
    return CreateIntrinsic(Intrinsic::cttz, getInt32Ty(), {result, getTrue()});
  }
  Value *result = CreateShuffleVector(value, PoisonValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = CreateBitCast(result, getInt64Ty());
  result = CreateIntrinsic(Intrinsic::cttz, getInt64Ty(), {result, getTrue()});
  return CreateZExtOrTrunc(result, getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup ballotfindmsb call.
//
// @param value : The ballot value to find the most significant bit of. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupBallotFindMsb(Value *const value, const Twine &instName) {
  if (getShaderSubgroupSize() <= 32) {
    Value *result = CreateExtractElement(value, getInt32(0));
    result = CreateIntrinsic(Intrinsic::ctlz, getInt32Ty(), {result, getTrue()});
    return CreateSub(getInt32(31), result);
  }
  Value *result = CreateShuffleVector(value, PoisonValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = CreateBitCast(result, getInt64Ty());
  result = CreateIntrinsic(Intrinsic::ctlz, getInt64Ty(), {result, getTrue()});
  result = CreateZExtOrTrunc(result, getInt32Ty());
  return CreateSub(getInt32(63), result);
}

// =====================================================================================================================
// Create a subgroup shuffle call.
//
// @param value : The value to shuffle.
// @param index : The index to shuffle from.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupShuffle(Value *const value, Value *const index, const Twine &instName) {
  if (supportWaveWideBPermute()) {
    auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                      ArrayRef<Value *> passthroughArgs) -> Value * {
      return builder.CreateIntrinsic(Intrinsic::amdgcn_ds_bpermute, {}, {passthroughArgs[0], mappedArgs[0]});
    };

    // The ds_bpermute intrinsic requires the index be multiplied by 4.
    return CreateMapToInt32(mapFunc, value, CreateMul(index, getInt32(4)));
  }

  if (supportPermLane64Dpp()) {
    assert(getShaderWaveSize() == 64);

    // Start the WWM section by setting the inactive lanes.
    Value *const poisonValue = PoisonValue::get(value->getType());
    Value *const poisonIndex = PoisonValue::get(index->getType());
    Value *const scaledIndex = CreateMul(index, getInt32(4));
    Value *wwmValue = BuilderBase::get(*this).CreateSetInactive(value, poisonValue);
    Value *wwmIndex = BuilderBase::get(*this).CreateSetInactive(scaledIndex, poisonIndex);

    auto permuteFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                          ArrayRef<Value *> passthroughArgs) -> Value * {
      return builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_permlane64, {mappedArgs[0]});
    };

    auto swapped = CreateMapToInt32(permuteFunc, wwmValue, {});

    auto bPermFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
      return builder.CreateIntrinsic(Intrinsic::amdgcn_ds_bpermute, {}, {passthroughArgs[0], mappedArgs[0]});
    };

    auto bPermSameHalf = CreateMapToInt32(bPermFunc, wwmValue, wwmIndex);
    auto bPermOtherHalf = CreateMapToInt32(bPermFunc, swapped, wwmIndex);
    bPermOtherHalf = createWwm(bPermOtherHalf);

    auto const threadId = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
    auto const sameOrOtherHalf = CreateAnd(CreateXor(index, threadId), getInt32(32));
    auto const indexInSameHalf = CreateICmpEQ(sameOrOtherHalf, getInt32(0));

    return CreateSelect(indexInSameHalf, bPermSameHalf, bPermOtherHalf);
  }

  auto mapFunc = [this](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    Value *const readlane =
        builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_readlane, {mappedArgs[0], passthroughArgs[0]});
    return createWaterfallLoop(cast<Instruction>(readlane), 1);
  };

  return CreateMapToInt32(mapFunc, value, index);
}

// =====================================================================================================================
// Create a subgroup shufflexor call.
//
// @param value : The value to shuffle.
// @param mask : The mask to shuffle with.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupShuffleXor(Value *const value, Value *const mask, const Twine &instName) {
  bool canOptimize = false;
  unsigned maskValue = ~0;
  DppCtrl dppCtrl = DppCtrl::DppQuadPerm0000;

  // issue dpp_mov for some simple quad/row shuffle cases;
  // then issue ds_permlane_x16 if supported or ds_swizzle, if maskValue < 32
  // default to call SubgroupShuffle, which may issue waterfallloops to handle complex cases.
  if (isa<ConstantInt>(mask)) {
    maskValue = cast<ConstantInt>(mask)->getZExtValue();

    if (maskValue < 32) {
      canOptimize = true;
      switch (maskValue) {
      case 0:
        dppCtrl = DppCtrl::DppQuadPerm0123;
        break;
      case 1:
        dppCtrl = DppCtrl::DppQuadPerm1032;
        break;
      case 2:
        dppCtrl = DppCtrl::DppQuadPerm2301;
        break;
      case 3:
        dppCtrl = DppCtrl::DppQuadPerm3210;
        break;
      case 7:
        dppCtrl = DppCtrl::DppRowHalfMirror;
        break;
      case 8:
        dppCtrl = DppCtrl::DppRowRr8;
        break;
      case 15:
        dppCtrl = DppCtrl::DppRowMirror;
        break;
      default:
        canOptimize = false;
        break;
      }

      if (!canOptimize && supportDppRowXmask()) {
        canOptimize = true;
        switch (maskValue) {
        case 4:
          dppCtrl = DppCtrl::DppRowXmask4;
          break;
        case 5:
          dppCtrl = DppCtrl::DppRowXmask5;
          break;
        case 6:
          dppCtrl = DppCtrl::DppRowXmask6;
          break;
        case 9:
          dppCtrl = DppCtrl::DppRowXmask9;
          break;
        case 10:
          dppCtrl = DppCtrl::DppRowXmask10;
          break;
        case 11:
          dppCtrl = DppCtrl::DppRowXmask11;
          break;
        case 12:
          dppCtrl = DppCtrl::DppRowXmask12;
          break;
        case 13:
          dppCtrl = DppCtrl::DppRowXmask13;
          break;
        case 14:
          dppCtrl = DppCtrl::DppRowXmask14;
          break;
        default:
          canOptimize = false;
          break;
        }
      }
    }
  }

  if (maskValue < 32) {
    if (supportDpp() && canOptimize)
      return createDppMov(value, dppCtrl, 0xF, 0xF, true);
    if (supportPermLaneDpp() && (maskValue >= 16)) {
      static const unsigned LaneSelBits[16][2] = {
          {0x76543210, 0xfedcba98}, {0x67452301, 0xefcdab89}, {0x54761032, 0xdcfe98ba}, {0x45670123, 0xcdef89ab},
          {0x32107654, 0xba98fedc}, {0x23016745, 0xab89efcd}, {0x10325476, 0x98badcfe}, {0x1234567, 0x89abcdef},
          {0xfedcba98, 0x76543210}, {0xefcdab89, 0x67452301}, {0xdcfe98ba, 0x54761032}, {0xcdef89ab, 0x45670123},
          {0xba98fedc, 0x32107654}, {0xab89efcd, 0x23016745}, {0x98badcfe, 0x10325476}, {0x89abcdef, 0x1234567}};
      return createPermLaneX16(value, value, LaneSelBits[maskValue - 16][0], LaneSelBits[maskValue - 16][1], false,
                               false);
    }
    return createDsSwizzle(value, getDsSwizzleBitMode(maskValue, 0x00, 0x1F));
  }
  Value *index = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
  index = CreateXor(index, mask);
  return CreateSubgroupShuffle(value, index, instName);
}

// =====================================================================================================================
// Create a subgroup shuffleup call.
//
// @param value : The value to shuffle.
// @param delta : The delta to shuffle from.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupShuffleUp(Value *const value, Value *const delta, const Twine &instName) {
  Value *index = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
  index = CreateSub(index, delta);
  return CreateSubgroupShuffle(value, index, instName);
}

// =====================================================================================================================
// Create a subgroup shuffledown call.
//
// @param value : The value to shuffle.
// @param delta : The delta to shuffle from.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupShuffleDown(Value *const value, Value *const delta, const Twine &instName) {
  Value *index = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");
  index = CreateAdd(index, delta);
  return CreateSubgroupShuffle(value, index, instName);
}

// =====================================================================================================================
// Create a subgroup clustered reduction.
//
// @param groupArithOp : The group arithmetic operation.
// @param value : An LLVM value.
// @param inClusterSize : The expected cluster size.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, Value *const value,
                                                     Value *const inClusterSize, const Twine &instName) {
  auto waveSize = getInt32(getShaderWaveSize());
  Value *clusterSize = CreateSelect(CreateICmpUGT(inClusterSize, waveSize), waveSize, inClusterSize);
  if (supportDpp()) {
    // Start the WWM section by setting the inactive lanes.
    Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());
    Value *result = BuilderBase::get(*this).CreateSetInactive(value, identity);

    // Perform The group arithmetic operation between adjacent lanes in the subgroup, with all masks and rows enabled
    // (0xF).
    result = CreateSelect(
        CreateICmpUGE(clusterSize, getInt32(2)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppQuadPerm1032, 0xF, 0xF, true)),
        result);

    // Perform The group arithmetic operation between N <-> N+2 lanes in the subgroup, with all masks and rows enabled
    // (0xF).
    result = CreateSelect(
        CreateICmpUGE(clusterSize, getInt32(4)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppQuadPerm2301, 0xF, 0xF, true)),
        result);

    // Use a row half mirror to make all values in a cluster of 8 the same, with all masks and rows enabled (0xF).
    result = CreateSelect(
        CreateICmpUGE(clusterSize, getInt32(8)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowHalfMirror, 0xF, 0xF, true)),
        result);

    // Use a row mirror to make all values in a cluster of 16 the same, with all masks and rows enabled (0xF).
    result = CreateSelect(
        CreateICmpUGE(clusterSize, getInt32(16)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowMirror, 0xF, 0xF, true)),
        result);

    if (supportPermLaneDpp()) {
      // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
      result = CreateSelect(
          CreateICmpUGE(clusterSize, getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createPermLaneX16(result, result, UINT32_MAX, UINT32_MAX, true, false)),
          result);

      if (supportPermLane64Dpp()) {
        result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                              createGroupArithmeticOperation(groupArithOp, result, createPermLane64(result)), result);
      } else {
        Value *const broadcast31 = CreateSubgroupBroadcast(result, getInt32(31), instName);
        Value *const broadcast63 = CreateSubgroupBroadcast(result, getInt32(63), instName);

        // Combine broadcast from the 31st and 63rd for the final result.
        result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                              createGroupArithmeticOperation(groupArithOp, broadcast31, broadcast63), result);
      }
    } else {
      // Use a row broadcast to move the 15th element in each cluster of 16 to the next cluster. The row mask is
      // set to 0xa (0b1010) so that only the 2nd and 4th clusters of 16 perform the calculation.
      result = CreateSelect(
          CreateICmpUGE(clusterSize, getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast15, 0xA, 0xF, true)),
          result);

      // Use a row broadcast to move the 31st element from the lower cluster of 32 to the upper cluster. The row
      // mask is set to 0x8 (0b1000) so that only the upper cluster of 32 perform the calculation.
      result = CreateSelect(
          CreateICmpEQ(clusterSize, getInt32(64)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast31, 0x8, 0xF, true)),
          result);

      Value *const broadcast31 = CreateSubgroupBroadcast(result, getInt32(31), instName);
      Value *const broadcast63 = CreateSubgroupBroadcast(result, getInt32(63), instName);

      // If the cluster size is 64 we always read the value from the last invocation in the subgroup.
      result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)), broadcast63, result);

      Value *const laneIdLessThan32 = CreateICmpULT(CreateSubgroupMbcnt(getInt64(UINT64_MAX), ""), getInt32(32));

      // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
      // invocation 31 or 63's value.
      result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(32)),
                            CreateSelect(laneIdLessThan32, broadcast31, broadcast63), result);
    }

    // Finish the WWM section by calling the intrinsic.
    return createWwm(result);
  }
  // Start the WWM section by setting the inactive lanes.
  Value *result =
      BuilderBase::get(*this).CreateSetInactive(value, createGroupArithmeticIdentity(groupArithOp, value->getType()));

  // The DS swizzle mode is doing a xor of 0x1 to swap values between N <-> N+1, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(2)),
                        createGroupArithmeticOperation(groupArithOp, result,
                                                       createDsSwizzle(result, getDsSwizzleBitMode(0x01, 0x00, 0x1F))),
                        result);

  // The DS swizzle mode is doing a xor of 0x2 to swap values between N <-> N+2, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                        createGroupArithmeticOperation(groupArithOp, result,
                                                       createDsSwizzle(result, getDsSwizzleBitMode(0x02, 0x00, 0x1F))),
                        result);

  // The DS swizzle mode is doing a xor of 0x4 to swap values between N <-> N+4, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(8)),
                        createGroupArithmeticOperation(groupArithOp, result,
                                                       createDsSwizzle(result, getDsSwizzleBitMode(0x04, 0x00, 0x1F))),
                        result);

  // The DS swizzle mode is doing a xor of 0x8 to swap values between N <-> N+8, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(16)),
                        createGroupArithmeticOperation(groupArithOp, result,
                                                       createDsSwizzle(result, getDsSwizzleBitMode(0x08, 0x00, 0x1F))),
                        result);

  // The DS swizzle mode is doing a xor of 0x10 to swap values between N <-> N+16, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(32)),
                        createGroupArithmeticOperation(groupArithOp, result,
                                                       createDsSwizzle(result, getDsSwizzleBitMode(0x10, 0x00, 0x1F))),
                        result);

  Value *const broadcast31 = CreateSubgroupBroadcast(result, getInt32(31), instName);
  Value *const broadcast63 = CreateSubgroupBroadcast(result, getInt32(63), instName);

  // If the cluster size is 64 we always compute the value by adding together the two broadcasts.
  result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                        createGroupArithmeticOperation(groupArithOp, broadcast31, broadcast63), result);

  Value *const threadId = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");

  // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
  // invocation 31 or 63's value.
  result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(32)),
                        CreateSelect(CreateICmpULT(threadId, getInt32(32)), broadcast31, broadcast63), result);

  // Finish the WWM section by calling the intrinsic.
  return createWwm(result);
}

// =====================================================================================================================
// Create a subgroup clustered inclusive scan.
//
// @param groupArithOp : The group arithmetic operation.
// @param value : An LLVM value.
// @param inClusterSize : The expected cluster size.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, Value *const value,
                                                     Value *const inClusterSize, const Twine &instName) {
  auto waveSize = getInt32(getShaderWaveSize());
  Value *clusterSize = CreateSelect(CreateICmpUGT(inClusterSize, waveSize), waveSize, inClusterSize);
  if (supportDpp()) {
    Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());

    // Start the WWM section by setting the inactive invocations.
    Value *const setInactive = BuilderBase::get(*this).CreateSetInactive(value, identity);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    Value *result = CreateSelect(
        CreateICmpUGE(clusterSize, getInt32(2)),
        createGroupArithmeticOperation(groupArithOp, setInactive,
                                       createDppUpdate(identity, setInactive, DppCtrl::DppRowSr1, 0xF, 0xF, 0)),
        setInactive);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result =
        CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                     createGroupArithmeticOperation(
                         groupArithOp, result, createDppUpdate(identity, setInactive, DppCtrl::DppRowSr2, 0xF, 0xF, 0)),
                     result);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result =
        CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                     createGroupArithmeticOperation(
                         groupArithOp, result, createDppUpdate(identity, setInactive, DppCtrl::DppRowSr3, 0xF, 0xF, 0)),
                     result);

    // The DPP operation has all rows active (0xF) and the top 3 banks active (0xe, 0b1110) to make sure that in
    // each cluster of 16, only the top 12 lanes perform the operation.
    result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(8)),
                          createGroupArithmeticOperation(
                              groupArithOp, result, createDppUpdate(identity, result, DppCtrl::DppRowSr4, 0xF, 0xE, 0)),
                          result);

    // The DPP operation has all rows active (0xF) and the top 2 banks active (0xc, 0b1100) to make sure that in
    // each cluster of 16, only the top 8 lanes perform the operation.
    result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(16)),
                          createGroupArithmeticOperation(
                              groupArithOp, result, createDppUpdate(identity, result, DppCtrl::DppRowSr8, 0xF, 0xC, 0)),
                          result);

    if (supportPermLaneDpp()) {
      Value *const threadMask = createThreadMask();

      Value *const maskedPermLane =
          createThreadMaskedSelect(threadMask, 0xFFFF0000FFFF0000,
                                   createPermLaneX16(result, result, UINT32_MAX, UINT32_MAX, true, false), identity);

      // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
      result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(32)),
                            createGroupArithmeticOperation(groupArithOp, result, maskedPermLane), result);

      Value *const broadcast31 = CreateSubgroupBroadcast(result, getInt32(31), instName);

      Value *const maskedBroadcast = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);

      // Combine broadcast of 31 with the top two rows only.
      result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                            createGroupArithmeticOperation(groupArithOp, result, maskedBroadcast), result);
    } else {
      // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the
      // operation.
      result = CreateSelect(
          CreateICmpUGE(clusterSize, getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast15, 0xA, 0xF, true)),
          result);

      // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the
      // operation.
      result = CreateSelect(
          CreateICmpEQ(clusterSize, getInt32(64)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast31, 0xC, 0xF, true)),
          result);
    }

    // Finish the WWM section by calling the intrinsic.
    return createWwm(result);
  }
  Value *const threadMask = createThreadMask();

  Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());

  // Start the WWM section by setting the inactive invocations.
  Value *const setInactive = BuilderBase::get(*this).CreateSetInactive(value, identity);
  Value *result = setInactive;

  // The DS swizzle is or'ing by 0x0 with an and mask of 0x1E, which swaps from N <-> N+1. We don't want the N's
  // to perform the operation, only the N+1's, so we use a mask of 0xA (0b1010) to stop the N's doing anything.
  Value *maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xAAAAAAAAAAAAAAAA, createDsSwizzle(result, getDsSwizzleBitMode(0x00, 0x00, 0x1E)), identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(2)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x1 with an and mask of 0x1C, which swaps from N <-> N+2. We don't want the N's
  // to perform the operation, only the N+2's, so we use a mask of 0xC (0b1100) to stop the N's doing anything.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xCCCCCCCCCCCCCCCC,
                                           createDsSwizzle(result, getDsSwizzleBitMode(0x00, 0x01, 0x1C)), identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x3 with an and mask of 0x18, which swaps from N <-> N+4. We don't want the N's
  // to perform the operation, only the N+4's, so we use a mask of 0xF0 (0b11110000) to stop the N's doing
  // anything.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xF0F0F0F0F0F0F0F0,
                                           createDsSwizzle(result, getDsSwizzleBitMode(0x00, 0x03, 0x18)), identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(8)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x7 with an and mask of 0x10, which swaps from N <-> N+8. We don't want the N's
  // to perform the operation, only the N+8's, so we use a mask of 0xFF00 (0b1111111100000000) to stop the N's
  // doing anything.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xFF00FF00FF00FF00,
                                           createDsSwizzle(result, getDsSwizzleBitMode(0x00, 0x07, 0x10)), identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(16)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
  // to perform the operation, only the N+16's, so we use a mask of 0xFFFF0000
  // (0b11111111111111110000000000000000) to stop the N's doing anything.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xFFFF0000FFFF0000,
                                           createDsSwizzle(result, getDsSwizzleBitMode(0x00, 0x0F, 0x00)), identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(32)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  Value *const broadcast31 = CreateSubgroupBroadcast(result, getInt32(31), instName);

  // The mask here is enforcing that only the top 32 lanes of the wavefront perform the final scan operation.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);
  result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // Finish the WWM section by calling the intrinsic.
  return createWwm(result);
}

// =====================================================================================================================
// Create a subgroup clustered exclusive scan.
//
// @param groupArithOp : The group arithmetic operation.
// @param value : An LLVM value.
// @param inClusterSize : The expected cluster size.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, Value *const value,
                                                     Value *const inClusterSize, const Twine &instName) {
  auto waveSize = getInt32(getShaderWaveSize());
  Value *clusterSize = CreateSelect(CreateICmpUGT(inClusterSize, waveSize), waveSize, inClusterSize);
  if (supportDpp()) {
    Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());

    // Start the WWM section by setting the inactive invocations.
    Value *const setInactive = BuilderBase::get(*this).CreateSetInactive(value, identity);

    Value *shiftRight = nullptr;

    if (supportPermLaneDpp()) {
      Value *const threadMask = createThreadMask();

      // Shift right within each row:
      // 0b0110,0101,0100,0011,0010,0001,0000,1111 = 0x6543210F
      // 0b1110,1101,1100,1011,1010,1001,1000,0111 = 0xEDCBA987
      shiftRight = createPermLane16(setInactive, setInactive, 0x6543210F, 0xEDCBA987, true, false);

      // Only needed for wave size 64.
      if (getShaderWaveSize() == 64) {
        // Need to write the value from the 16th invocation into the 48th.
        shiftRight = CreateSubgroupWriteInvocation(shiftRight, CreateSubgroupBroadcast(shiftRight, getInt32(16), ""),
                                                   getInt32(48), "");
      }

      shiftRight = CreateSubgroupWriteInvocation(shiftRight, identity, getInt32(16), "");

      // Exchange first column value cross rows(row 1<--> row 0, row 3<-->row2)
      // Only first column value from each row join permlanex
      shiftRight =
          createThreadMaskedSelect(threadMask, 0x0001000100010001,
                                   createPermLaneX16(shiftRight, shiftRight, 0, UINT32_MAX, true, false), shiftRight);
    } else {
      // Shift the whole subgroup right by one, using a DPP update operation. This will ensure that the identity
      // value is in the 0th invocation and all other values are shifted up. All rows and banks are active (0xF).
      shiftRight = createDppUpdate(identity, setInactive, DppCtrl::DppWfSr1, 0xF, 0xF, 0);
    }

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    Value *result = CreateSelect(
        CreateICmpUGE(clusterSize, getInt32(2)),
        createGroupArithmeticOperation(groupArithOp, shiftRight,
                                       createDppUpdate(identity, shiftRight, DppCtrl::DppRowSr1, 0xF, 0xF, 0)),
        shiftRight);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result =
        CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                     createGroupArithmeticOperation(
                         groupArithOp, result, createDppUpdate(identity, shiftRight, DppCtrl::DppRowSr2, 0xF, 0xF, 0)),
                     result);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result =
        CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                     createGroupArithmeticOperation(
                         groupArithOp, result, createDppUpdate(identity, shiftRight, DppCtrl::DppRowSr3, 0xF, 0xF, 0)),
                     result);

    // The DPP operation has all rows active (0xF) and the top 3 banks active (0xe, 0b1110) to make sure that in
    // each cluster of 16, only the top 12 lanes perform the operation.
    result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(8)),
                          createGroupArithmeticOperation(
                              groupArithOp, result, createDppUpdate(identity, result, DppCtrl::DppRowSr4, 0xF, 0xE, 0)),
                          result);

    // The DPP operation has all rows active (0xF) and the top 2 banks active (0xc, 0b1100) to make sure that in
    // each cluster of 16, only the top 8 lanes perform the operation.
    result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(16)),
                          createGroupArithmeticOperation(
                              groupArithOp, result, createDppUpdate(identity, result, DppCtrl::DppRowSr8, 0xF, 0xC, 0)),
                          result);

    if (supportPermLaneDpp()) {
      Value *const threadMask = createThreadMask();

      Value *const maskedPermLane =
          createThreadMaskedSelect(threadMask, 0xFFFF0000FFFF0000,
                                   createPermLaneX16(result, result, UINT32_MAX, UINT32_MAX, true, false), identity);

      // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
      result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(32)),
                            createGroupArithmeticOperation(groupArithOp, result, maskedPermLane), result);

      Value *const broadcast31 = CreateSubgroupBroadcast(result, getInt32(31), instName);

      Value *const maskedBroadcast = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);

      // Combine broadcast of 31 with the top two rows only.
      result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                            createGroupArithmeticOperation(groupArithOp, result, maskedBroadcast), result);
    } else {
      // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the
      // operation.
      result = CreateSelect(
          CreateICmpUGE(clusterSize, getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast15, 0xA, 0xF, true)),
          result);

      // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the
      // operation.
      result = CreateSelect(
          CreateICmpEQ(clusterSize, getInt32(64)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast31, 0xC, 0xF, true)),
          result);
    }

    // Finish the WWM section by calling the intrinsic.
    return createWwm(result);
  }
  Value *const threadMask = createThreadMask();

  Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());

  // Start the WWM section by setting the inactive invocations.
  Value *const setInactive = BuilderBase::get(*this).CreateSetInactive(value, identity);
  Value *result = identity;

  // The DS swizzle is or'ing by 0x0 with an and mask of 0x1E, which swaps from N <-> N+1. We don't want the N's
  // to perform the operation, only the N+1's, so we use a mask of 0xA (0b1010) to stop the N's doing anything.
  Value *maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xAAAAAAAAAAAAAAAA, createDsSwizzle(setInactive, getDsSwizzleBitMode(0x00, 0x00, 0x1E)), identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(2)), maskedSwizzle, result);

  // The DS swizzle is or'ing by 0x1 with an and mask of 0x1C, which swaps from N <-> N+2. We don't want the N's
  // to perform the operation, only the N+2's, so we use a mask of 0xC (0b1100) to stop the N's doing anything.
  maskedSwizzle =
      createThreadMaskedSelect(threadMask, 0xCCCCCCCCCCCCCCCC,
                               createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                               getDsSwizzleBitMode(0x00, 0x01, 0x1C)),
                               identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(4)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x3 with an and mask of 0x18, which swaps from N <-> N+4. We don't want the N's
  // to perform the operation, only the N+4's, so we use a mask of 0xF0 (0b11110000) to stop the N's doing
  // anything.
  maskedSwizzle =
      createThreadMaskedSelect(threadMask, 0xF0F0F0F0F0F0F0F0,
                               createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                               getDsSwizzleBitMode(0x00, 0x03, 0x18)),
                               identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(8)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x7 with an and mask of 0x10, which swaps from N <-> N+8. We don't want the N's
  // to perform the operation, only the N+8's, so we use a mask of 0xFF00 (0b1111111100000000) to stop the N's
  // doing anything.
  maskedSwizzle =
      createThreadMaskedSelect(threadMask, 0xFF00FF00FF00FF00,
                               createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                               getDsSwizzleBitMode(0x00, 0x07, 0x10)),
                               identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(16)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
  // to perform the operation, only the N+16's, so we use a mask of 0xFFFF0000
  // (0b11111111111111110000000000000000) to stop the N's doing anything.
  maskedSwizzle =
      createThreadMaskedSelect(threadMask, 0xFFFF0000FFFF0000,
                               createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                               getDsSwizzleBitMode(0x00, 0x0F, 0x00)),
                               identity);
  result = CreateSelect(CreateICmpUGE(clusterSize, getInt32(32)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  Value *const broadcast31 = CreateSubgroupBroadcast(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                                     getInt32(31), instName);

  // The mask here is enforcing that only the top 32 lanes of the wavefront perform the final scan operation.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);
  result = CreateSelect(CreateICmpEQ(clusterSize, getInt32(64)),
                        createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // Finish the WWM section by calling the intrinsic.
  return createWwm(result);
}

// =====================================================================================================================
// Create a subgroup quad broadcast call.
//
// @param value : The value to broadcast across the quad.
// @param index : The index in the quad to broadcast the value from.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupQuadBroadcast(Value *const value, Value *const index, const Twine &instName) {
  Value *result = PoisonValue::get(value->getType());

  const unsigned indexBits = index->getType()->getPrimitiveSizeInBits();

  if (supportDpp()) {
    Value *compare = CreateICmpEQ(index, getIntN(indexBits, 0));
    result = CreateSelect(compare, createDppMov(value, DppCtrl::DppQuadPerm0000, 0xF, 0xF, true), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 1));
    result = CreateSelect(compare, createDppMov(value, DppCtrl::DppQuadPerm1111, 0xF, 0xF, true), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 2));
    result = CreateSelect(compare, createDppMov(value, DppCtrl::DppQuadPerm2222, 0xF, 0xF, true), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 3));
    result = CreateSelect(compare, createDppMov(value, DppCtrl::DppQuadPerm3333, 0xF, 0xF, true), result);
  } else {
    Value *compare = CreateICmpEQ(index, getIntN(indexBits, 0));
    result = CreateSelect(compare, createDsSwizzle(value, getDsSwizzleQuadMode(0, 0, 0, 0)), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 1));
    result = CreateSelect(compare, createDsSwizzle(value, getDsSwizzleQuadMode(1, 1, 1, 1)), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 2));
    result = CreateSelect(compare, createDsSwizzle(value, getDsSwizzleQuadMode(2, 2, 2, 2)), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 3));
    result = CreateSelect(compare, createDsSwizzle(value, getDsSwizzleQuadMode(3, 3, 3, 3)), result);
  }

  return createWqm(result);
}

// =====================================================================================================================
// Create a subgroup quad swap horizontal call.
//
// @param value : The value to swap.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupQuadSwapHorizontal(Value *const value, const Twine &instName) {
  if (supportDpp())
    return createWqm(createDppMov(value, DppCtrl::DppQuadPerm1032, 0xF, 0xF, true));

  return createWqm(createDsSwizzle(value, getDsSwizzleQuadMode(1, 0, 3, 2)));
}

// =====================================================================================================================
// Create a subgroup quad swap vertical call.
//
// @param value : The value to swap.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupQuadSwapVertical(Value *const value, const Twine &instName) {
  if (supportDpp())
    return createWqm(createDppMov(value, DppCtrl::DppQuadPerm2301, 0xF, 0xF, true));

  return createWqm(createDsSwizzle(value, getDsSwizzleQuadMode(2, 3, 0, 1)));
}

// =====================================================================================================================
// Create a subgroup quadswapdiagonal call.
//
// @param value : The value to swap.
// @param instName : Name to give final instruction.
Value *BuilderImpl::CreateSubgroupQuadSwapDiagonal(Value *const value, const Twine &instName) {
  if (supportDpp())
    return createWqm(createDppMov(value, DppCtrl::DppQuadPerm3210, 0xF, 0xF, true));

  return createWqm(createDsSwizzle(value, getDsSwizzleQuadMode(3, 2, 1, 0)));
}

// =====================================================================================================================
// Create a subgroup quad swap swizzle.
//
// @param value : The value to swizzle.
// @param offset : The value to specify the swizzle offsets.
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSubgroupSwizzleQuad(Value *const value, Value *const offset, const Twine &instName) {
  Constant *const constOffset = cast<Constant>(offset);
  uint8_t lane0 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(0u))->getZExtValue());
  uint8_t lane1 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(1u))->getZExtValue());
  uint8_t lane2 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(2u))->getZExtValue());
  uint8_t lane3 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(3u))->getZExtValue());

  return createDsSwizzle(value, getDsSwizzleQuadMode(lane0, lane1, lane2, lane3));
}

// =====================================================================================================================
// Create a subgroup swizzle mask.
//
// @param value : The value to swizzle.
// @param mask : The value to specify the swizzle masks.
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSubgroupSwizzleMask(Value *const value, Value *const mask, const Twine &instName) {
  Constant *const constMask = cast<Constant>(mask);
  uint8_t andMask = static_cast<uint8_t>(cast<ConstantInt>(constMask->getAggregateElement(0u))->getZExtValue());
  uint8_t orMask = static_cast<uint8_t>(cast<ConstantInt>(constMask->getAggregateElement(1u))->getZExtValue());
  uint8_t xorMask = static_cast<uint8_t>(cast<ConstantInt>(constMask->getAggregateElement(2u))->getZExtValue());

  assert(andMask <= 31 && orMask <= 31 && xorMask <= 31);

  return createDsSwizzle(value, getDsSwizzleBitMode(xorMask, orMask, andMask));
}

// =====================================================================================================================
// Create a subgroup write invocation.
//
// @param inputValue : The value to return for all but one invocations.
// @param writeValue : The value to return for one invocation.
// @param invocationIndex : The index of the invocation that gets the write value.
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSubgroupWriteInvocation(Value *const inputValue, Value *const writeValue,
                                                  Value *const invocationIndex, const Twine &instName) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_writelane,
                                   {
                                       mappedArgs[1],
                                       passthroughArgs[0],
                                       mappedArgs[0],
                                   });
  };

  return CreateMapToInt32(mapFunc, {inputValue, writeValue}, invocationIndex);
}

// =====================================================================================================================
// Create a subgroup mbcnt.
//
// @param mask : The mask to mbcnt with.
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateSubgroupMbcnt(Value *const mask, const Twine &instName) {
  // Check that the type is definitely an i64.
  assert(mask->getType()->isIntegerTy(64));

  Value *const masks = CreateBitCast(mask, FixedVectorType::get(getInt32Ty(), 2));
  Value *const maskLow = CreateExtractElement(masks, getInt32(0));
  Value *const maskHigh = CreateExtractElement(masks, getInt32(1));
  CallInst *const mbcntLo = CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {maskLow, getInt32(0)});

  if (getShaderSubgroupSize() <= 32)
    return mbcntLo;
  return CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {maskHigh, mbcntLo});
}

// =====================================================================================================================
// Create The group arithmetic operation identity.
//
// @param groupArithOp : The group arithmetic operation to get the identity for.
// @param type : The type of the identity.
Value *BuilderImpl::createGroupArithmeticIdentity(GroupArithOp groupArithOp, Type *const type) {
  switch (groupArithOp) {
  case GroupArithOp::IAdd:
    return ConstantInt::get(type, 0);
  case GroupArithOp::FAdd:
    return ConstantFP::get(type, 0.0);
  case GroupArithOp::IMul:
    return ConstantInt::get(type, 1);
  case GroupArithOp::FMul:
    return ConstantFP::get(type, 1.0);
  case GroupArithOp::SMin:
    if (type->isIntOrIntVectorTy(8))
      return ConstantInt::get(type, INT8_MAX, true);
    else if (type->isIntOrIntVectorTy(16))
      return ConstantInt::get(type, INT16_MAX, true);
    else if (type->isIntOrIntVectorTy(32))
      return ConstantInt::get(type, INT32_MAX, true);
    else if (type->isIntOrIntVectorTy(64))
      return ConstantInt::get(type, INT64_MAX, true);
    else {
      llvm_unreachable("Should never be called!");
      return nullptr;
    }
  case GroupArithOp::UMin:
    return ConstantInt::get(type, UINT64_MAX, false);
  case GroupArithOp::FMin:
    return ConstantFP::getInfinity(type, false);
  case GroupArithOp::SMax:
    if (type->isIntOrIntVectorTy(8))
      return ConstantInt::get(type, INT8_MIN, true);
    else if (type->isIntOrIntVectorTy(16))
      return ConstantInt::get(type, INT16_MIN, true);
    else if (type->isIntOrIntVectorTy(32))
      return ConstantInt::get(type, INT32_MIN, true);
    else if (type->isIntOrIntVectorTy(64))
      return ConstantInt::get(type, INT64_MIN, true);
    else {
      llvm_unreachable("Should never be called!");
      return nullptr;
    }
  case GroupArithOp::UMax:
    return ConstantInt::get(type, 0, false);
  case GroupArithOp::FMax:
    return ConstantFP::getInfinity(type, true);
  case GroupArithOp::And:
    return ConstantInt::get(type, UINT64_MAX, false);
  case GroupArithOp::Or:
    return ConstantInt::get(type, 0, false);
  case GroupArithOp::Xor:
    return ConstantInt::get(type, 0, false);
  default:
    llvm_unreachable("Should never be called!");
    return nullptr;
  }
}

// =====================================================================================================================
// Create The group arithmetic operation arithmetic on x and y.
//
// @param groupArithOp : The group arithmetic operation to use for the reduction.
// @param x : The x value to perform the arithmetic on.
// @param y : The y value to perform the arithmetic on.
Value *BuilderImpl::createGroupArithmeticOperation(GroupArithOp groupArithOp, Value *const x, Value *const y) {
  switch (groupArithOp) {
  case GroupArithOp::IAdd:
    return CreateAdd(x, y);
  case GroupArithOp::FAdd:
    return CreateFAdd(x, y);
  case GroupArithOp::IMul:
    return CreateMul(x, y);
  case GroupArithOp::FMul:
    return CreateFMul(x, y);
  case GroupArithOp::SMin:
    return CreateBinaryIntrinsic(Intrinsic::smin, x, y);
  case GroupArithOp::UMin:
    return CreateBinaryIntrinsic(Intrinsic::umin, x, y);
  case GroupArithOp::FMin:
    return CreateMinNum(x, y);
  case GroupArithOp::SMax:
    return CreateBinaryIntrinsic(Intrinsic::smax, x, y);
  case GroupArithOp::UMax:
    return CreateBinaryIntrinsic(Intrinsic::umax, x, y);
  case GroupArithOp::FMax:
    return CreateMaxNum(x, y);
  case GroupArithOp::And:
    return CreateAnd(x, y);
  case GroupArithOp::Or:
    return CreateOr(x, y);
  case GroupArithOp::Xor:
    return CreateXor(x, y);
  default:
    llvm_unreachable("Not implemented!");
    return nullptr;
  }
}

// =====================================================================================================================
// Create a call to dpp mov.
//
// @param value : The value to DPP mov.
// @param dppCtrl : The dpp_ctrl to use.
// @param rowMask : The row mask.
// @param bankMask : The bank mask.
// @param boundCtrl : Whether bound_ctrl is used or not.
Value *BuilderImpl::createDppMov(Value *const value, DppCtrl dppCtrl, unsigned rowMask, unsigned bankMask,
                                 bool boundCtrl) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(
        Intrinsic::amdgcn_mov_dpp, builder.getInt32Ty(),
        {mappedArgs[0], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  return CreateMapToInt32(
      mapFunc, value,
      {getInt32(static_cast<unsigned>(dppCtrl)), getInt32(rowMask), getInt32(bankMask), getInt1(boundCtrl)});
}

// =====================================================================================================================
// Create a call to dpp update.
//
// @param origValue : The original value we are going to update.
// @param updateValue : The value to DPP update.
// @param dppCtrl : The dpp_ctrl to use.
// @param rowMask : The row mask.
// @param bankMask : The bank mask.
// @param boundCtrl : Whether bound_ctrl is used or not.
Value *BuilderImpl::createDppUpdate(Value *const origValue, Value *const updateValue, DppCtrl dppCtrl, unsigned rowMask,
                                    unsigned bankMask, bool boundCtrl) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(
        Intrinsic::amdgcn_update_dpp, builder.getInt32Ty(),
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  return CreateMapToInt32(
      mapFunc,
      {
          origValue,
          updateValue,
      },
      {getInt32(static_cast<unsigned>(dppCtrl)), getInt32(rowMask), getInt32(bankMask), getInt1(boundCtrl)});
}

// =====================================================================================================================
// Create a call to permute lane.
//
// @param origValue : The original value we are going to update.
// @param updateValue : The value to update with.
// @param selectBitsLow : Select bits low.
// @param selectBitsHigh : Select bits high.
// @param fetchInactive : FI mode, whether to fetch inactive lane.
// @param boundCtrl : Whether bound_ctrl is used or not.
Value *BuilderImpl::createPermLane16(Value *const origValue, Value *const updateValue, unsigned selectBitsLow,
                                     unsigned selectBitsHigh, bool fetchInactive, bool boundCtrl) {
  auto mapFunc = [this](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(
        getInt32Ty(), Intrinsic::amdgcn_permlane16,
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  return CreateMapToInt32(
      mapFunc,
      {
          origValue,
          updateValue,
      },
      {getInt32(selectBitsLow), getInt32(selectBitsHigh), getInt1(fetchInactive), getInt1(boundCtrl)});
}

// =====================================================================================================================
// Create a call to permute lane.
//
// @param origValue : The original value we are going to update.
// @param updateValue : The value to update with.
// @param selectBitsLow : Select bits low.
// @param selectBitsHigh : Select bits high.
// @param fetchInactive : FI mode, whether to fetch inactive lane.
// @param boundCtrl : Whether bound_ctrl is used or not.
Value *BuilderImpl::createPermLaneX16(Value *const origValue, Value *const updateValue, unsigned selectBitsLow,
                                      unsigned selectBitsHigh, bool fetchInactive, bool boundCtrl) {
  auto mapFunc = [this](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(
        getInt32Ty(), Intrinsic::amdgcn_permlanex16,
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  return CreateMapToInt32(
      mapFunc,
      {
          origValue,
          updateValue,
      },
      {getInt32(selectBitsLow), getInt32(selectBitsHigh), getInt1(fetchInactive), getInt1(boundCtrl)});
}

// =====================================================================================================================
// Create a call to permute lane 64.
//
// @param updateValue : The value to update with.
Value *BuilderImpl::createPermLane64(Value *const updateValue) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(builder.getInt32Ty(), Intrinsic::amdgcn_permlane64, {mappedArgs[0]});
  };

  return CreateMapToInt32(mapFunc, updateValue, {});
}

// =====================================================================================================================
// Create a call to ds swizzle.
//
// @param value : The value to swizzle.
// @param dsPattern : The pattern to swizzle with.
Value *BuilderImpl::createDsSwizzle(Value *const value, uint16_t dsPattern) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(Intrinsic::amdgcn_ds_swizzle, {}, {mappedArgs[0], passthroughArgs[0]});
  };

  return CreateMapToInt32(mapFunc, value, getInt32(dsPattern));
}

// =====================================================================================================================
// Create a call to WWM (whole wave mode).
//
// @param value : The value to pass to the WWM call.
Value *BuilderImpl::createWwm(Value *const value) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    return builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wwm, mappedArgs[0]);
  };

  return CreateMapToInt32(mapFunc, value, {});
}

// =====================================================================================================================
// Create a call to WQM (whole quad mode).
// Only in fragment shader stage.
//
// @param value : The value to pass to the soft WQM call.
Value *BuilderImpl::createWqm(Value *const value) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    return builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, mappedArgs[0]);
  };

  if (m_shaderStage == ShaderStageFragment)
    return CreateMapToInt32(mapFunc, value, {});

  return value;
}

// =====================================================================================================================
// Create a ds_swizzle bit mode pattern.
//
// @param xorMask : The xor mask (bits 10..14).
// @param orMask : The or mask (bits 5..9).
// @param andMask : The and mask (bits 0..4).
uint16_t BuilderImpl::getDsSwizzleBitMode(uint8_t xorMask, uint8_t orMask, uint8_t andMask) {
  return (static_cast<uint16_t>(xorMask & 0x1F) << 10) | (static_cast<uint16_t>(orMask & 0x1F) << 5) | (andMask & 0x1F);
}

// =====================================================================================================================
// Create a ds_swizzle quad mode pattern.
//
// @param lane0 : The 0th lane.
// @param lane1 : The 1st lane.
// @param lane2 : The 2nd lane.
// @param lane3 : The 3rd lane.
uint16_t BuilderImpl::getDsSwizzleQuadMode(uint8_t lane0, uint8_t lane1, uint8_t lane2, uint8_t lane3) {
  return 0x8000 | static_cast<uint16_t>((lane3 << 6) | ((lane2 & 0x3) << 4) | ((lane1 & 0x3) << 2) | ((lane0 & 0x3)));
}

// =====================================================================================================================
// Create a thread mask for the current thread, an integer with a single bit representing the ID of the thread set to 1.
Value *BuilderImpl::createThreadMask() {
  Value *threadId = CreateSubgroupMbcnt(getInt64(UINT64_MAX), "");

  Value *threadMask = nullptr;
  if (getShaderSubgroupSize() <= 32)
    threadMask = CreateShl(getInt32(1), threadId);
  else
    threadMask = CreateShl(getInt64(1), CreateZExtOrTrunc(threadId, getInt64Ty()));

  return threadMask;
}

// =====================================================================================================================
// Create a masked operation - taking a thread mask and a mask to and it with, select between the first value and the
// second value if the current thread is active.
//
// @param threadMask : The thread mask, must come from a call to CreateThreadMask.
// @param andMask : The mask to and with the thread mask.
// @param value1 : The first value to select.
// @param value2 : The second value to select.
Value *BuilderImpl::createThreadMaskedSelect(Value *const threadMask, uint64_t andMask, Value *const value1,
                                             Value *const value2) {
  Value *const andMaskVal = getIntN(getShaderSubgroupSize(), andMask);
  Value *const zero = getIntN(getShaderSubgroupSize(), 0);
  return CreateSelect(CreateICmpNE(CreateAnd(threadMask, andMaskVal), zero), value1, value2);
}

// =====================================================================================================================
// Do group ballot, turning a per-lane boolean value (in a VGPR) into a subgroup-wide shared SGPR.
//
// @param value : The value to contribute to the SGPR, must be an boolean type.
Value *BuilderImpl::createGroupBallot(Value *const value) {
  // Check the type is definitely an boolean.
  assert(value->getType()->isIntegerTy(1));

  unsigned waveSize = getShaderWaveSize();
  Value *result = CreateIntrinsic(getIntNTy(waveSize), Intrinsic::amdgcn_ballot, value);

  // If we have a 32-bit subgroup size, we need to turn the 32-bit ballot result into a 64-bit result.
  if (waveSize <= 32)
    result = CreateZExt(result, getInt64Ty());

  return result;
}
