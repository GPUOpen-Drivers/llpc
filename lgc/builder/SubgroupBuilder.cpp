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
 * @file  SubgroupBuilder.cpp
 * @brief LLPC source file: implementation of subgroup Builder methods
 ***********************************************************************************************************************
 */
#include "BuilderImpl.h"
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
Value *SubgroupBuilder::CreateGetWaveSize(const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupGetWaveSize, getInt32Ty(), {}, Attribute::ReadNone,
                         instName);
}

// =====================================================================================================================
// Create a subgroup get subgroup size.
//
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateGetSubgroupSize(const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupGetSubgroupSize, getInt32Ty(), {}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup elect call.
//
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupElect(const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupElect, getInt1Ty(), {}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup all call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupAll(Value *const value, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupAll);
  addTypeMangling(getInt1Ty(), {value}, originalTypeName);
  return CreateNamedCall(originalTypeName, getInt1Ty(), {value}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup any call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupAny(Value *const value, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupAny);
  addTypeMangling(getInt1Ty(), {value}, originalTypeName);
  return CreateNamedCall(originalTypeName, getInt1Ty(), {value}, Attribute::ReadNone,
                         instName);
}

// =====================================================================================================================
// Create a subgroup all equal call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupAllEqual(Value *const value, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupAllEqual);
  addTypeMangling(getInt1Ty(), {value}, originalTypeName);
  return CreateNamedCall(originalTypeName, getInt1Ty(), {value}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast call.
//
// @param value : The value to read from the chosen lane to all active lanes.
// @param index : The index to broadcast from. Must be an i32.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBroadcast(Value *const value, Value *const index, const Twine &instName) {
  return BuilderBase::get(*this).CreateSubgroupBroadcastImpl(value, index, instName);
}

// =====================================================================================================================
// Create a subgroup write invocation.
//
// @param inputValue : The value to return for all but one invocations.
// @param writeValue : The value to return for one invocation.
// @param invocationIndex : The index of the invocation that gets the write value.
// @param instName : Name to give instruction(s)
Value *SubgroupBuilder::CreateSubgroupWriteInvocation(Value *const inputValue, Value *const writeValue,
                                                      Value *const invocationIndex, const Twine &instName) {
  return BuilderBase::get(*this).CreateSubgroupWriteInvocationImpl(inputValue, writeValue, invocationIndex, instName);
}

// =====================================================================================================================
// Create a subgroup broadcast call using waterfall for non-uniform index
//
// @param value : The value to read from the chosen lane to all active lanes.
// @param index : The index to broadcast from. Must be an i32.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBroadcastWaterfall(Value *const value, Value *const index,
                                                         const Twine &instName) {
  auto mapFunc = [this](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    Value *const readlane =
        builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, {mappedArgs[0], passthroughArgs[0]});
    return BuilderBase::get(*this).createWaterfallLoop(cast<Instruction>(readlane), 1);
  };
  return CreateMapToInt32(mapFunc, value, index);
}

// =====================================================================================================================
// Create a subgroup broadcastfirst call.
//
// @param value : The value to read from the first active lane into all other active lanes.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBroadcastFirst(Value *const value, const Twine &instName) {
  return BuilderBase::get(*this).CreateSubgroupBroadcastFirstImpl(value, instName);
}

// =====================================================================================================================
// Create a subgroup ballot call.
//
// @param value : The value to ballot across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallot(Value *const value, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupBallot);
  addTypeMangling(FixedVectorType::get(getInt32Ty(), 4), {}, originalTypeName);
  return CreateNamedCall(originalTypeName, FixedVectorType::get(getInt32Ty(), 4), {value}, Attribute::ReadNone,
                         instName);
}

// =====================================================================================================================
// Create a subgroup inverseballot call.
//
// @param value : The value to inverseballot across the subgroup. Must be a <4 x i32> type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupInverseBallot(Value *const value, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupInverseBallot, getInt1Ty(), {value}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup ballotbitextract call.
//
// @param value : The ballot value to bit extract. Must be an <4 x i32> type.
// @param index : The bit index to extract. Must be an i32 type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallotBitExtract(Value *const value, Value *const index, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupBallotBitExtract, getInt1Ty(), {value, index}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup ballotbitcount call.
//
// @param value : The ballot value to bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallotBitCount(Value *const value, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupBallotBitCount, getInt32Ty(), {value}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup ballotinclusivebitcount call.
//
// @param value : The ballot value to inclusively bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallotInclusiveBitCount(Value *const value, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupBallotInclusiveBitCount, getInt32Ty(), {value}, Attribute::ReadNone,
                         instName);
}

// =====================================================================================================================
// Create a subgroup ballotexclusivebitcount call.
//
// @param value : The ballot value to exclusively bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallotExclusiveBitCount(Value *const value, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupBallotExclusiveBitCount, getInt32Ty(), {value}, Attribute::ReadNone,
                         instName);
}

// =====================================================================================================================
// Create a subgroup ballotfindlsb call.
//
// @param value : The ballot value to find the least significant bit of. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallotFindLsb(Value *const value, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupBallotFindLsb, getInt32Ty(), {value}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup ballotfindmsb call.
//
// @param value : The ballot value to find the most significant bit of. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupBallotFindMsb(Value *const value, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupBallotFindMsb, getInt32Ty(), {value}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup shuffle call.
//
// @param value : The value to shuffle.
// @param index : The index to shuffle from.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupShuffle(Value *const value, Value *const index, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupShuffle, value->getType(), {value, index}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup shufflexor call.
//
// @param value : The value to shuffle.
// @param mask : The mask to shuffle with.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupShuffleXor(Value *const value, Value *const mask, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupShuffleXor, value->getType(), {value, mask}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup shuffleup call.
//
// @param value : The value to shuffle.
// @param delta : The delta to shuffle from.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupShuffleUp(Value *const value, Value *const delta, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupShuffleUp, value->getType(), {value, delta}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup shuffledown call.
//
// @param value : The value to shuffle.
// @param delta : The delta to shuffle from.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupShuffleDown(Value *const value, Value *const delta, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupShuffleDown, value->getType(), {value, delta}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup clustered reduction.
//
// @param groupArithOp : The group arithmetic operation.
// @param value : An LLVM value.
// @param inClusterSize : The expected cluster size.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupClusteredReduction(GroupArithOp groupArithOp, Value *const value,
                                                         Value *const inClusterSize, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupClusteredReduction);
  addTypeMangling(value->getType(), {}, originalTypeName);
  return CreateNamedCall(originalTypeName, value->getType(), {getInt32(groupArithOp), value, inClusterSize},
                         Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup clustered inclusive scan.
//
// @param groupArithOp : The group arithmetic operation.
// @param value : An LLVM value.
// @param inClusterSize : The expected cluster size.
// @param instName : Name to give final instruction.
// turn this into intrinsic also so that it uses threadmaskedselect later on
Value *SubgroupBuilder::CreateSubgroupClusteredInclusive(GroupArithOp groupArithOp, Value *const value,
                                                         Value *const inClusterSize, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupClusteredInclusive);
  addTypeMangling(value->getType(), {}, originalTypeName);
  return CreateNamedCall(originalTypeName, value->getType(), {getInt32(groupArithOp), value, inClusterSize},
                         Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup clustered exclusive scan.
//
// @param groupArithOp : The group arithmetic operation.
// @param value : An LLVM value.
// @param inClusterSize : The expected cluster size.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupClusteredExclusive(GroupArithOp groupArithOp, Value *const value,
                                                         Value *const inClusterSize, const Twine &instName) {
  std::string originalTypeName(lgcName::SubgroupClusteredExclusive);
  addTypeMangling(value->getType(), {}, originalTypeName);
  return CreateNamedCall(originalTypeName, value->getType(), {getInt32(groupArithOp), value, inClusterSize},
                         Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a subgroup quad broadcast call.
//
// @param value : The value to broadcast across the quad.
// @param index : The index in the quad to broadcast the value from.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupQuadBroadcast(Value *const value, Value *const index, const Twine &instName) {
  Value *result = UndefValue::get(value->getType());

  const unsigned indexBits = index->getType()->getPrimitiveSizeInBits();

  if (supportDpp()) {
    Value *compare = CreateICmpEQ(index, getIntN(indexBits, 0));
    result = CreateSelect(
        compare, BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm0000, 0xF, 0xF, true), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 1));
    result = CreateSelect(
        compare, BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm1111, 0xF, 0xF, true), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 2));
    result = CreateSelect(
        compare, BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm2222, 0xF, 0xF, true), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 3));
    result = CreateSelect(
        compare, BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm3333, 0xF, 0xF, true), result);
  } else {
    Value *compare = CreateICmpEQ(index, getIntN(indexBits, 0));
    result =
        CreateSelect(compare, BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(0, 0, 0, 0)), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 1));
    result =
        CreateSelect(compare, BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(1, 1, 1, 1)), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 2));
    result =
        CreateSelect(compare, BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(2, 2, 2, 2)), result);

    compare = CreateICmpEQ(index, getIntN(indexBits, 3));
    result =
        CreateSelect(compare, BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(3, 3, 3, 3)), result);
  }

  return createWqm(result);
}

// =====================================================================================================================
// Create a subgroup quad swap horizontal call.
//
// @param value : The value to swap.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupQuadSwapHorizontal(Value *const value, const Twine &instName) {
  if (supportDpp())
    return createWqm(BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm1032, 0xF, 0xF, true));

  return createWqm(BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(1, 0, 3, 2)));
}

// =====================================================================================================================
// Create a subgroup quad swap vertical call.
//
// @param value : The value to swap.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupQuadSwapVertical(Value *const value, const Twine &instName) {
  if (supportDpp())
    return createWqm(BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm2301, 0xF, 0xF, true));

  return createWqm(BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(2, 3, 0, 1)));
}

// =====================================================================================================================
// Create a subgroup quadswapdiagonal call.
//
// @param value : The value to swap.
// @param instName : Name to give final instruction.
Value *SubgroupBuilder::CreateSubgroupQuadSwapDiagonal(Value *const value, const Twine &instName) {
  if (supportDpp())
    return createWqm(BuilderBase::get(*this).createDppMov(value, DppCtrl::DppQuadPerm3210, 0xF, 0xF, true));

  return createWqm(BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(3, 2, 1, 0)));
}

// =====================================================================================================================
// Create a subgroup quad swap swizzle.
//
// @param value : The value to swizzle.
// @param offset : The value to specify the swizzle offsets.
// @param instName : Name to give instruction(s)
Value *SubgroupBuilder::CreateSubgroupSwizzleQuad(Value *const value, Value *const offset, const Twine &instName) {
  Constant *const constOffset = cast<Constant>(offset);
  uint8_t lane0 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(0u))->getZExtValue());
  uint8_t lane1 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(1u))->getZExtValue());
  uint8_t lane2 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(2u))->getZExtValue());
  uint8_t lane3 = static_cast<uint8_t>(cast<ConstantInt>(constOffset->getAggregateElement(3u))->getZExtValue());

  return BuilderBase::get(*this).createDsSwizzle(value, getDsSwizzleQuadMode(lane0, lane1, lane2, lane3));
}

// =====================================================================================================================
// Create a subgroup swizzle mask.
//
// @param value : The value to swizzle.
// @param mask : The value to specify the swizzle masks.
// @param instName : Name to give instruction(s)
Value *SubgroupBuilder::CreateSubgroupSwizzleMask(Value *const value, Value *const mask, const Twine &instName) {
  Constant *const constMask = cast<Constant>(mask);
  uint8_t andMask = static_cast<uint8_t>(cast<ConstantInt>(constMask->getAggregateElement(0u))->getZExtValue());
  uint8_t orMask = static_cast<uint8_t>(cast<ConstantInt>(constMask->getAggregateElement(1u))->getZExtValue());
  uint8_t xorMask = static_cast<uint8_t>(cast<ConstantInt>(constMask->getAggregateElement(2u))->getZExtValue());

  assert(andMask <= 31 && orMask <= 31 && xorMask <= 31);

  return BuilderBase::get(*this).createDsSwizzle(value,
                                                 BuilderBase::get(*this).getDsSwizzleBitMode(xorMask, orMask, andMask));
}

// =====================================================================================================================
// Create a subgroup mbcnt.
//
// @param mask : The mask to mbcnt with.
// @param instName : Name to give instruction(s)
Value *SubgroupBuilder::CreateSubgroupMbcnt(Value *const mask, const Twine &instName) {
  return CreateNamedCall(lgcName::SubgroupMbcnt, getInt32Ty(), {mask}, Attribute::ReadNone, instName);
}

// =====================================================================================================================
// Create a call to WQM (whole quad mode).
// Only in fragment shader stage.
//
// @param value : The value to pass to the soft WQM call.
Value *SubgroupBuilder::createWqm(Value *const value) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    return builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wqm, mappedArgs[0]);
  };

  if (m_shaderStage == ShaderStageFragment)
    return CreateMapToInt32(mapFunc, value, {});

  return value;
}

// =====================================================================================================================
// Create a ds_swizzle quad mode pattern.
//
// @param lane0 : The 0th lane.
// @param lane1 : The 1st lane.
// @param lane2 : The 2nd lane.
// @param lane3 : The 3rd lane.
uint16_t SubgroupBuilder::getDsSwizzleQuadMode(uint8_t lane0, uint8_t lane1, uint8_t lane2, uint8_t lane3) {
  return 0x8000 | static_cast<uint16_t>((lane3 << 6) | ((lane2 & 0x3) << 4) | ((lane1 & 0x3) << 2) | ((lane0 & 0x3)));
}
