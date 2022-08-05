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
#include "LowerSubgroupOperations.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Function.h"
#include <functional>

#define DEBUG_TYPE "lgc-lower-subgroup-operations"

using namespace llvm;
using namespace lgc;

char LegacyLowerSubgroupOperations::ID = 0;

namespace lgc {

// Pass creator, creates the pass for lowering subgroup operations
ModulePass *createLegacyLowerSubgroupOperations() {
  return new LegacyLowerSubgroupOperations();
}

} // namespace lgc

// =====================================================================================================================
// run this pass on a LLVM module
// @param [in/out] module : the LLVM function to run on
// @returns : if this pass modified the CFG
bool LegacyLowerSubgroupOperations::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  return m_impl.runImpl(module, pipelineState);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The Analyses that are still valid after this pass)
PreservedAnalyses LowerSubgroupOperations::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  if (runImpl(module, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] pipelineState : the pipeline state to use for this transformation
// @returns : True if the module was modified by the transformation and false otherwise
bool LowerSubgroupOperations::runImpl(Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Running the pass of lowering LLPC subgroup calls\n");

  m_pipelineState = pipelineState;
  m_builder.reset(new BuilderBase(module.getContext()));

  for (auto &fDef : module) {
    if (!fDef.isDeclaration() || !fDef.getName().startswith(lgcName::SubgroupPrefix))
      continue;
    auto name = fDef.getName();
    using std::placeholders::_1;
    std::function<Value *(llvm::CallInst &)> lowerer =
        StringSwitch<std::function<Value *(llvm::CallInst &)>>(name)
            .Case(lgcName::SubgroupGetSubgroupSize, std::bind(&LowerSubgroupOperations::lowerGetSubgroupSize, this, _1))
            .Case(lgcName::SubgroupGetWaveSize, std::bind(&LowerSubgroupOperations::lowerGetWaveSize, this, _1))
            .Case(lgcName::SubgroupElect, std::bind(&LowerSubgroupOperations::lowerSubgroupElect, this, _1))
            .StartsWith(lgcName::SubgroupAny, std::bind(&LowerSubgroupOperations::lowerSubgroupAny, this, _1))
            .StartsWith(lgcName::SubgroupAllEqual, std::bind(&LowerSubgroupOperations::lowerSubgroupAllEqual, this, _1))
            .StartsWith(lgcName::SubgroupAll, std::bind(&LowerSubgroupOperations::lowerSubgroupAll, this, _1))
            .Case(lgcName::SubgroupInverseBallot,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupInverseBallot, this, _1))
            .Case(lgcName::SubgroupBallotBitExtract,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupBallotBitExtract, this, _1))
            .Case(lgcName::SubgroupBallotBitCount,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupBallotBitCount, this, _1))
            .Case(lgcName::SubgroupBallotInclusiveBitCount,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupBallotInclusiveBitCount, this, _1))
            .Case(lgcName::SubgroupBallotExclusiveBitCount,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupBallotExclusiveBitCount, this, _1))
            .Case(lgcName::SubgroupBallotFindLsb,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupBallotFindLsb, this, _1))
            .Case(lgcName::SubgroupBallotFindMsb,
                  std::bind(&LowerSubgroupOperations::lowerSubgroupBallotFindMsb, this, _1))
            .StartsWith(lgcName::SubgroupBallot, std::bind(&LowerSubgroupOperations::lowerSubgroupBallot, this, _1))
            .Case(lgcName::SubgroupShuffle, std::bind(&LowerSubgroupOperations::lowerSubgroupShuffle, this, _1))
            .Case(lgcName::SubgroupShuffleXor, std::bind(&LowerSubgroupOperations::lowerSubgroupShuffle, this, _1))
            .Case(lgcName::SubgroupShuffleUp, std::bind(&LowerSubgroupOperations::lowerSubgroupShuffleUp, this, _1))
            .Case(lgcName::SubgroupShuffleDown, std::bind(&LowerSubgroupOperations::lowerSubgroupShuffleDown, this, _1))
            .StartsWith(lgcName::SubgroupClusteredInclusive,
                        std::bind(&LowerSubgroupOperations::lowerSubgroupClusteredInclusive, this, _1))
            .StartsWith(lgcName::SubgroupClusteredExclusive,
                        std::bind(&LowerSubgroupOperations::lowerSubgroupClusteredExclusive, this, _1))
            .StartsWith(lgcName::SubgroupClusteredReduction,
                        std::bind(&LowerSubgroupOperations::lowerSubgroupClusteredReduction, this, _1))
            .Case(lgcName::SubgroupMbcnt, std::bind(&LowerSubgroupOperations::lowerSubgroupMbcnt, this, _1));

    while (!fDef.use_empty()) {
      User *use = *fDef.users().begin();
      CallInst *call = cast<CallInst>(use);
      LLVM_DEBUG(dbgs() << "Replaying " << *call << "\n");
      m_builder->SetInsertPoint(call);

      Value *retValue = lowerer(*call); // lower a single use
      LLVM_DEBUG(dbgs() << "  replacing with: " << *retValue << "\n");
      call->replaceAllUsesWith(retValue);
      call->eraseFromParent();
    }
  }
  return true;
}

// =====================================================================================================================
// Generate a subgroup get subgroup size from its call intrinsic
// @param call : the named call substitute with operands {}
Value *LowerSubgroupOperations::lowerGetSubgroupSize(llvm::CallInst &call) {
  return m_builder->getInt32(getShaderSubgroupSize());
}

// =====================================================================================================================
// Generate a subgroup get wave size from its call intrinsic
// @param call : the named call substitute with operands {}
Value *LowerSubgroupOperations::lowerGetWaveSize(llvm::CallInst &call) {
  return m_builder->getInt32(getShaderWaveSize());
}

// =====================================================================================================================
// Generate a subgroup elect from its call intrinsic
// @param call : the named call substitute with operands {}
Value *LowerSubgroupOperations::lowerSubgroupElect(llvm::CallInst &call) {
  return m_builder->CreateICmpEQ(createSubgroupMbcnt(createGroupBallot(m_builder->getTrue()), ""),
                                 m_builder->getInt32(0));
}

// =====================================================================================================================
// Generate a subgroup all from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupAll(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  return createSubgroupAll(value, call.getName());
}

// =====================================================================================================================
// Generate a subgroup any from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupAny(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  Value *result = m_builder->CreateICmpNE(createGroupBallot(value), m_builder->getInt64(0));
  result = m_builder->CreateSelect(m_builder->CreateUnaryIntrinsic(Intrinsic::is_constant, value), value, result);

  // Helper invocations of whole quad mode should be included in the subgroup vote execution
  if (getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageFragment) {
    result = m_builder->CreateZExt(result, m_builder->getInt32Ty());
    result = m_builder->CreateIntrinsic(Intrinsic::amdgcn_softwqm, {m_builder->getInt32Ty()}, {result});
    result = m_builder->CreateTrunc(result, m_builder->getInt1Ty());
  }
  return result;
}

// =====================================================================================================================
// Generate a subgroup all equal from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupAllEqual(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  Type *const type = value->getType();
  const auto &instName = call.getName();

  Value *compare = m_builder->CreateSubgroupBroadcastFirstImpl(value, instName);

  if (type->isFPOrFPVectorTy())
    compare = m_builder->CreateFCmpOEQ(compare, value);
  else {
    assert(type->isIntOrIntVectorTy());
    compare = m_builder->CreateICmpEQ(compare, value);
  }

  if (type->isVectorTy()) {
    Value *result = m_builder->CreateExtractElement(compare, m_builder->getInt32(0));

    for (unsigned i = 1, compCount = cast<FixedVectorType>(type)->getNumElements(); i < compCount; i++)
      result = m_builder->CreateAnd(result, m_builder->CreateExtractElement(compare, i));

    return createSubgroupAll(result, instName);
  }
  return createSubgroupAll(compare, instName);
}

// =====================================================================================================================
// Generate a subgroup ballot from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupBallot(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  // Check the type is definitely an integer.
  assert(value->getType()->isIntegerTy());

  Value *ballot = createGroupBallot(value);

  // Ballot expects a <4 x i32> return, so we need to turn the i64 into that.
  ballot = m_builder->CreateBitCast(ballot, FixedVectorType::get(m_builder->getInt32Ty(), 2));

  ElementCount elementCount = cast<VectorType>(ballot->getType())->getElementCount();
  return m_builder->CreateShuffleVector(ballot, ConstantVector::getSplat(elementCount, m_builder->getInt32(0)),
                                        ArrayRef<int>{0, 1, 2, 3});
}

// =====================================================================================================================
// Generate a subgroup inverse ballot from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupInverseBallot(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  return createSubgroupInverseBallot(value, call.getName());
}

// =====================================================================================================================
// Generate a subgroup ballotbitcount from its call intrinsic
// @param call : the named call substitute with operands {Value *value, Value *index}
Value *LowerSubgroupOperations::lowerSubgroupBallotBitExtract(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  Value *index = call.getOperand(1);
  return createSubgroupBallotBitExtract(value, index, call.getName());
}

// =====================================================================================================================
// Generate a subgroup ballotbitcount from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupBallotBitCount(llvm::CallInst &call) {
  Value *value = call.getOperand(0);

  if (getShaderSubgroupSize() <= 32)
    return m_builder->CreateUnaryIntrinsic(Intrinsic::ctpop,
                                           m_builder->CreateExtractElement(value, m_builder->getInt32(0)));
  Value *result = m_builder->CreateShuffleVector(value, UndefValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = m_builder->CreateBitCast(result, m_builder->getInt64Ty());
  result = m_builder->CreateUnaryIntrinsic(Intrinsic::ctpop, result);
  return m_builder->CreateZExtOrTrunc(result, m_builder->getInt32Ty());
}

// =====================================================================================================================
// Create a subgroup ballotinclusivebitcount call.
//
// @param value : The ballot value to inclusively bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *LowerSubgroupOperations::lowerSubgroupBallotInclusiveBitCount(llvm::CallInst &call) {
  Value *value = call.getOperand(0);
  Value *const exclusiveBitCount = createSubgroupBallotExclusiveBitCount(value, call.getName());
  Value *const inverseBallot = createSubgroupInverseBallot(value, call.getName());
  Value *const inclusiveBitCount = m_builder->CreateAdd(exclusiveBitCount, m_builder->getInt32(1));
  return m_builder->CreateSelect(inverseBallot, inclusiveBitCount, exclusiveBitCount);
}

// =====================================================================================================================
// Generate a subgroup ballotexclusivebitcount from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupBallotExclusiveBitCount(llvm::CallInst &call) {
  Value *value = call.getOperand(0);

  return createSubgroupBallotExclusiveBitCount(value, call.getName());
}

// =====================================================================================================================
// Generate a subgroup ballotfindlsb from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupBallotFindLsb(llvm::CallInst &call) {
  Value *value = call.getOperand(0);

  if (getShaderSubgroupSize() <= 32) {
    Value *const result = m_builder->CreateExtractElement(value, m_builder->getInt32(0));
    return m_builder->CreateIntrinsic(Intrinsic::cttz, m_builder->getInt32Ty(), {result, m_builder->getTrue()});
  }
  Value *result = m_builder->CreateShuffleVector(value, UndefValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = m_builder->CreateBitCast(result, m_builder->getInt64Ty());
  result = m_builder->CreateIntrinsic(Intrinsic::cttz, m_builder->getInt64Ty(), {result, m_builder->getTrue()});
  return m_builder->CreateZExtOrTrunc(result, m_builder->getInt32Ty());
}

// =====================================================================================================================
// Generate a subgroup ballotfindmsb from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupBallotFindMsb(llvm::CallInst &call) {
  Value *value = call.getOperand(0);

  if (getShaderSubgroupSize() <= 32) {
    Value *result = m_builder->CreateExtractElement(value, m_builder->getInt32(0));
    result = m_builder->CreateIntrinsic(Intrinsic::ctlz, m_builder->getInt32Ty(), {result, m_builder->getTrue()});
    return m_builder->CreateSub(m_builder->getInt32(31), result);
  }
  Value *result = m_builder->CreateShuffleVector(value, UndefValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = m_builder->CreateBitCast(result, m_builder->getInt64Ty());
  result = m_builder->CreateIntrinsic(Intrinsic::ctlz, m_builder->getInt64Ty(), {result, m_builder->getTrue()});
  result = m_builder->CreateZExtOrTrunc(result, m_builder->getInt32Ty());
  return m_builder->CreateSub(m_builder->getInt32(63), result);
}

// =====================================================================================================================
// Generate a subgroup shuffle from its call intrinsic
// @param call : the named call substitute with operands {Value *value}
Value *LowerSubgroupOperations::lowerSubgroupShuffle(llvm::CallInst &call) {
  Value *const value = call.getOperand(0);
  Value *const index = call.getOperand(1);

  return createSubgroupShuffle(value, index, call.getName());
}

// =====================================================================================================================
// Generate a subgroup shuffle xor from its call intrinsic
// @param call : the named call substitute with operands {Value *value, Value *mask}
Value *LowerSubgroupOperations::lowerSubgroupShuffleXor(llvm::CallInst &call) {
  Value *const value = call.getOperand(0);
  Value *const mask = call.getOperand(1);
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
      return m_builder->createDppMov(value, dppCtrl, 0xF, 0xF, true);
    if (supportsPermLane16() && (maskValue >= 16)) {
      static const unsigned LaneSelBits[16][2] = {
          {0x76543210, 0xfedcba98}, {0x67452301, 0xefcdab89}, {0x54761032, 0xdcfe98ba}, {0x45670123, 0xcdef89ab},
          {0x32107654, 0xba98fedc}, {0x23016745, 0xab89efcd}, {0x10325476, 0x98badcfe}, {0x1234567, 0x89abcdef},
          {0xfedcba98, 0x76543210}, {0xefcdab89, 0x67452301}, {0xdcfe98ba, 0x54761032}, {0xcdef89ab, 0x45670123},
          {0xba98fedc, 0x32107654}, {0xab89efcd, 0x23016745}, {0x98badcfe, 0x10325476}, {0x89abcdef, 0x1234567}};
      return m_builder->createPermLaneX16(value, value, LaneSelBits[maskValue - 16][0], LaneSelBits[maskValue - 16][1],
                                          false, false);
    }
    return m_builder->createDsSwizzle(value, m_builder->getDsSwizzleBitMode(maskValue, 0x00, 0x1F));
  }
  Value *index = createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), "");
  index = m_builder->CreateXor(index, mask);
  return createSubgroupShuffle(value, index, call.getName());
}

// =====================================================================================================================
// Generate a subgroup shuffle up from its call intrinsic
// @param call : the named call substitute with operands {Value *value, Value *delta}
Value *LowerSubgroupOperations::lowerSubgroupShuffleUp(llvm::CallInst &call) {
  Value *const value = call.getOperand(0);
  Value *const delta = call.getOperand(1);

  Value *index = createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), "");
  index = m_builder->CreateSub(index, delta);
  return createSubgroupShuffle(value, index, call.getName());
}

// =====================================================================================================================
// Generate a subgroup shuffle down from its call intrinsic
// @param call : the named call substitute with operands {Value *value, Value *delta}
Value *LowerSubgroupOperations::lowerSubgroupShuffleDown(llvm::CallInst &call) {
  Value *const value = call.getOperand(0);
  Value *const delta = call.getOperand(1);

  Value *index = createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), "");
  index = m_builder->CreateAdd(index, delta);
  return createSubgroupShuffle(value, index, call.getName());
}

// =====================================================================================================================
// Generate a subgroup clustered inclusive scan from its call intrinsic
// @param call : the named call substitute with operands {ConstantInt *groupArithOp, Value *value, Value *clusterSize}
Value *LowerSubgroupOperations::lowerSubgroupClusteredInclusive(llvm::CallInst &call) {

  Builder::GroupArithOp groupArithOp =
      static_cast<Builder::GroupArithOp>(cast<ConstantInt>(call.getOperand(0))->getSExtValue());
  llvm::StringRef instName(call.getName());
  Value *value = call.getOperand(1);
  Value *inClusterSize = call.getOperand(2);

  auto waveSize = m_builder->getInt32(getShaderWaveSize());
  Value *clusterSize =
      m_builder->CreateSelect(m_builder->CreateICmpUGT(inClusterSize, waveSize), waveSize, inClusterSize);
  if (supportDpp()) {
    Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());

    // Start the WWM section by setting the inactive invocations.
    Value *const setInactive = m_builder->CreateSetInactive(value, identity);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    Value *result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(2)),
        createGroupArithmeticOperation(groupArithOp, setInactive,
                                       createDppUpdate(identity, setInactive, DppCtrl::DppRowSr1, 0xF, 0xF, 0)),
        setInactive);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, setInactive, DppCtrl::DppRowSr2, 0xF, 0xF, 0)),
        result);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, setInactive, DppCtrl::DppRowSr3, 0xF, 0xF, 0)),
        result);

    // The DPP operation has all rows active (0xF) and the top 3 banks active (0xe, 0b1110) to make sure that in
    // each cluster of 16, only the top 12 lanes perform the operation.
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(8)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowSr4, 0xF, 0xE, 0)),
        result);

    // The DPP operation has all rows active (0xF) and the top 2 banks active (0xc, 0b1100) to make sure that in
    // each cluster of 16, only the top 8 lanes perform the operation.
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(16)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowSr8, 0xF, 0xC, 0)),
        result);

    if (supportsPermLane16()) {
      Value *const threadMask = createThreadMask();

      Value *const maskedPermLane = createThreadMaskedSelect(
          threadMask, 0xFFFF0000FFFF0000,
          m_builder->createPermLaneX16(result, result, UINT32_MAX, UINT32_MAX, true, false), identity);

      // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
      result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
                                       createGroupArithmeticOperation(groupArithOp, result, maskedPermLane), result);

      Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(31), instName);

      Value *const maskedBroadcast = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);

      // Combine broadcast of 31 with the top two rows only.
      result = m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
                                       createGroupArithmeticOperation(groupArithOp, result, maskedBroadcast), result);
    } else {
      // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the
      // operation.
      result = m_builder->CreateSelect(
          m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast15, 0xA, 0xF, true)),
          result);

      // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the
      // operation.
      result = m_builder->CreateSelect(
          m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
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
  Value *const setInactive = m_builder->CreateSetInactive(value, identity);
  Value *result = setInactive;

  // The DS swizzle is or'ing by 0x0 with an and mask of 0x1E, which swaps from N <-> N+1. We don't want the N's
  // to perform the operation, only the N+1's, so we use a mask of 0xA (0b1010) to stop the N's doing anything.
  Value *maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xAAAAAAAAAAAAAAAA,
      m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x00, 0x00, 0x1E)), identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(2)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x1 with an and mask of 0x1C, which swaps from N <-> N+2. We don't want the N's
  // to perform the operation, only the N+2's, so we use a mask of 0xC (0b1100) to stop the N's doing anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xCCCCCCCCCCCCCCCC,
      m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x00, 0x01, 0x1C)), identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x3 with an and mask of 0x18, which swaps from N <-> N+4. We don't want the N's
  // to perform the operation, only the N+4's, so we use a mask of 0xF0 (0b11110000) to stop the N's doing
  // anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xF0F0F0F0F0F0F0F0,
      m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x00, 0x03, 0x18)), identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(8)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x7 with an and mask of 0x10, which swaps from N <-> N+8. We don't want the N's
  // to perform the operation, only the N+8's, so we use a mask of 0xFF00 (0b1111111100000000) to stop the N's
  // doing anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xFF00FF00FF00FF00,
      m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x00, 0x07, 0x10)), identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(16)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
  // to perform the operation, only the N+16's, so we use a mask of 0xFFFF0000
  // (0b11111111111111110000000000000000) to stop the N's doing anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xFFFF0000FFFF0000,
      m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x00, 0x0F, 0x00)), identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(31), instName);

  // The mask here is enforcing that only the top 32 lanes of the wavefront perform the final scan operation.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // Finish the WWM section by calling the intrinsic.
  return createWwm(result);
}

// =====================================================================================================================
// Generate a subgroup clustered exclusive scan from its call intrinsic
// @param call : the named call substitute with operands {ConstantInt *groupArithOp, Value *value, Value *clusterSize}
Value *LowerSubgroupOperations::lowerSubgroupClusteredExclusive(llvm::CallInst &call) {

  Builder::GroupArithOp groupArithOp =
      static_cast<Builder::GroupArithOp>(cast<ConstantInt>(call.getOperand(0))->getSExtValue());
  llvm::StringRef instName(call.getName());
  Value *value = call.getOperand(1);
  Value *inClusterSize = call.getOperand(2);

  auto waveSize = m_builder->getInt32(getShaderWaveSize());
  Value *clusterSize =
      m_builder->CreateSelect(m_builder->CreateICmpUGT(inClusterSize, waveSize), waveSize, inClusterSize);
  if (supportDpp()) {
    Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());

    // Start the WWM section by setting the inactive invocations.
    Value *const setInactive = m_builder->CreateSetInactive(value, identity);

    Value *shiftRight = nullptr;

    if (supportsPermLane16()) {
      Value *const threadMask = createThreadMask();

      // Shift right within each row:
      // 0b0110,0101,0100,0011,0010,0001,0000,1111 = 0x6543210F
      // 0b1110,1101,1100,1011,1010,1001,1000,0111 = 0xEDCBA987
      shiftRight = m_builder->createPermLane16(setInactive, setInactive, 0x6543210F, 0xEDCBA987, true, false);

      // Only needed for wave size 64.
      if (getShaderWaveSize() == 64) {
        // Need to write the value from the 16th invocation into the 48th.
        shiftRight = m_builder->CreateSubgroupWriteInvocationImpl(
            shiftRight, m_builder->CreateSubgroupBroadcastImpl(shiftRight, m_builder->getInt32(16), ""),
            m_builder->getInt32(48), "");
      }

      shiftRight = m_builder->CreateSubgroupWriteInvocationImpl(shiftRight, identity, m_builder->getInt32(16), "");

      // Exchange first column value cross rows(row 1<--> row 0, row 3<-->row2)
      // Only first column value from each row join permlanex
      shiftRight = createThreadMaskedSelect(
          threadMask, 0x0001000100010001,
          m_builder->createPermLaneX16(shiftRight, shiftRight, 0, UINT32_MAX, true, false), shiftRight);
    } else {
      // Shift the whole subgroup right by one, using a DPP update operation. This will ensure that the identity
      // value is in the 0th invocation and all other values are shifted up. All rows and banks are active (0xF).
      shiftRight = createDppUpdate(identity, setInactive, DppCtrl::DppWfSr1, 0xF, 0xF, 0);
    }

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    Value *result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(2)),
        createGroupArithmeticOperation(groupArithOp, shiftRight,
                                       createDppUpdate(identity, shiftRight, DppCtrl::DppRowSr1, 0xF, 0xF, 0)),
        shiftRight);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, shiftRight, DppCtrl::DppRowSr2, 0xF, 0xF, 0)),
        result);

    // The DPP operation has all rows active and all banks in the rows active (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, shiftRight, DppCtrl::DppRowSr3, 0xF, 0xF, 0)),
        result);

    // The DPP operation has all rows active (0xF) and the top 3 banks active (0xe, 0b1110) to make sure that in
    // each cluster of 16, only the top 12 lanes perform the operation.
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(8)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowSr4, 0xF, 0xE, 0)),
        result);

    // The DPP operation has all rows active (0xF) and the top 2 banks active (0xc, 0b1100) to make sure that in
    // each cluster of 16, only the top 8 lanes perform the operation.
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(16)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowSr8, 0xF, 0xC, 0)),
        result);

    if (supportsPermLane16()) {
      Value *const threadMask = createThreadMask();

      Value *const maskedPermLane = createThreadMaskedSelect(
          threadMask, 0xFFFF0000FFFF0000,
          m_builder->createPermLaneX16(result, result, UINT32_MAX, UINT32_MAX, true, false), identity);

      // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
      result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
                                       createGroupArithmeticOperation(groupArithOp, result, maskedPermLane), result);

      Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(31), instName);

      Value *const maskedBroadcast = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);

      // Combine broadcast of 31 with the top two rows only.
      result = m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
                                       createGroupArithmeticOperation(groupArithOp, result, maskedBroadcast), result);
    } else {
      // The DPP operation has a row mask of 0xa (0b1010) so only the 2nd and 4th clusters of 16 perform the
      // operation.
      result = m_builder->CreateSelect(
          m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast15, 0xA, 0xF, true)),
          result);

      // The DPP operation has a row mask of 0xc (0b1100) so only the 3rd and 4th clusters of 16 perform the
      // operation.
      result = m_builder->CreateSelect(
          m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
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
  Value *const setInactive = m_builder->CreateSetInactive(value, identity);
  Value *result = identity;

  // The DS swizzle is or'ing by 0x0 with an and mask of 0x1E, which swaps from N <-> N+1. We don't want the N's
  // to perform the operation, only the N+1's, so we use a mask of 0xA (0b1010) to stop the N's doing anything.
  Value *maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xAAAAAAAAAAAAAAAA,
      m_builder->createDsSwizzle(setInactive, m_builder->getDsSwizzleBitMode(0x00, 0x00, 0x1E)), identity);
  result =
      m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(2)), maskedSwizzle, result);

  // The DS swizzle is or'ing by 0x1 with an and mask of 0x1C, which swaps from N <-> N+2. We don't want the N's
  // to perform the operation, only the N+2's, so we use a mask of 0xC (0b1100) to stop the N's doing anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xCCCCCCCCCCCCCCCC,
      m_builder->createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                 m_builder->getDsSwizzleBitMode(0x00, 0x01, 0x1C)),
      identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x3 with an and mask of 0x18, which swaps from N <-> N+4. We don't want the N's
  // to perform the operation, only the N+4's, so we use a mask of 0xF0 (0b11110000) to stop the N's doing
  // anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xF0F0F0F0F0F0F0F0,
      m_builder->createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                 m_builder->getDsSwizzleBitMode(0x00, 0x03, 0x18)),
      identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(8)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0x7 with an and mask of 0x10, which swaps from N <-> N+8. We don't want the N's
  // to perform the operation, only the N+8's, so we use a mask of 0xFF00 (0b1111111100000000) to stop the N's
  // doing anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xFF00FF00FF00FF00,
      m_builder->createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                 m_builder->getDsSwizzleBitMode(0x00, 0x07, 0x10)),
      identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(16)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // The DS swizzle is or'ing by 0xF with an and mask of 0x0, which swaps from N <-> N+16. We don't want the N's
  // to perform the operation, only the N+16's, so we use a mask of 0xFFFF0000
  // (0b11111111111111110000000000000000) to stop the N's doing anything.
  maskedSwizzle = createThreadMaskedSelect(
      threadMask, 0xFFFF0000FFFF0000,
      m_builder->createDsSwizzle(createGroupArithmeticOperation(groupArithOp, result, setInactive),
                                 m_builder->getDsSwizzleBitMode(0x00, 0x0F, 0x00)),
      identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(
      createGroupArithmeticOperation(groupArithOp, result, setInactive), m_builder->getInt32(31), instName);

  // The mask here is enforcing that only the top 32 lanes of the wavefront perform the final scan operation.
  maskedSwizzle = createThreadMaskedSelect(threadMask, 0xFFFFFFFF00000000, broadcast31, identity);
  result = m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
                                   createGroupArithmeticOperation(groupArithOp, result, maskedSwizzle), result);

  // Finish the WWM section by calling the intrinsic.
  return createWwm(result);
}

// =====================================================================================================================
// Create a subgroup clustered reduction from its call intrinsic
// @param call : the named call substitute with operands {ConstantInt *groupArithOp, Value *value, Value *clusterSize}
Value *LowerSubgroupOperations::lowerSubgroupClusteredReduction(llvm::CallInst &call) {

  Builder::GroupArithOp groupArithOp =
      static_cast<Builder::GroupArithOp>(cast<ConstantInt>(call.getOperand(0))->getSExtValue());
  llvm::StringRef instName(call.getName());
  Value *value = call.getOperand(1);
  Value *inClusterSize = call.getOperand(2);

  auto waveSize = m_builder->getInt32(getShaderWaveSize());
  Value *clusterSize =
      m_builder->CreateSelect(m_builder->CreateICmpUGT(inClusterSize, waveSize), waveSize, inClusterSize);
  if (supportDpp()) {
    // Start the WWM section by setting the inactive lanes.
    Value *const identity = createGroupArithmeticIdentity(groupArithOp, value->getType());
    Value *result = m_builder->CreateSetInactive(value, identity);

    // Perform The group arithmetic operation between adjacent lanes in the subgroup, with all masks and rows enabled
    // (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(2)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppQuadPerm1032, 0xF, 0xF, true)),
        result);

    // Perform The group arithmetic operation between N <-> N+2 lanes in the subgroup, with all masks and rows enabled
    // (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppQuadPerm2301, 0xF, 0xF, true)),
        result);

    // Use a row half mirror to make all values in a cluster of 8 the same, with all masks and rows enabled (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(8)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowHalfMirror, 0xF, 0xF, true)),
        result);

    // Use a row mirror to make all values in a cluster of 16 the same, with all masks and rows enabled (0xF).
    result = m_builder->CreateSelect(
        m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(16)),
        createGroupArithmeticOperation(groupArithOp, result,
                                       createDppUpdate(identity, result, DppCtrl::DppRowMirror, 0xF, 0xF, true)),
        result);

    if (supportsPermLane16()) {
      // Use a permute lane to cross rows (row 1 <-> row 0, row 3 <-> row 2).
      result = m_builder->CreateSelect(
          m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
          createGroupArithmeticOperation(
              groupArithOp, result, m_builder->createPermLaneX16(result, result, UINT32_MAX, UINT32_MAX, true, false)),
          result);

#if LLPC_BUILD_GFX11
      if (supportPermLane64()) {
        result = m_builder->CreateSelect(
            m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
            createGroupArithmeticOperation(groupArithOp, result, m_builder->createPermLane64(result)), result);
      } else
#endif
      {
        Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(31), instName);
        Value *const broadcast63 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(63), instName);

        // Combine broadcast from the 31st and 63rd for the final result.
        result =
            m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
                                    createGroupArithmeticOperation(groupArithOp, broadcast31, broadcast63), result);
      }
    } else {
      // Use a row broadcast to move the 15th element in each cluster of 16 to the next cluster. The row mask is
      // set to 0xa (0b1010) so that only the 2nd and 4th clusters of 16 perform the calculation.
      result = m_builder->CreateSelect(
          m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast15, 0xA, 0xF, true)),
          result);

      // Use a row broadcast to move the 31st element from the lower cluster of 32 to the upper cluster. The row
      // mask is set to 0x8 (0b1000) so that only the upper cluster of 32 perform the calculation.
      result = m_builder->CreateSelect(
          m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
          createGroupArithmeticOperation(groupArithOp, result,
                                         createDppUpdate(identity, result, DppCtrl::DppRowBcast31, 0x8, 0xF, true)),
          result);

      Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(31), instName);
      Value *const broadcast63 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(63), instName);

      // If the cluster size is 64 we always read the value from the last invocation in the subgroup.
      result =
          m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)), broadcast63, result);

      Value *const laneIdLessThan32 =
          m_builder->CreateICmpULT(createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), ""), m_builder->getInt32(32));

      // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
      // invocation 31 or 63's value.
      result = m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(32)),
                                       m_builder->CreateSelect(laneIdLessThan32, broadcast31, broadcast63), result);
    }

    // Finish the WWM section by calling the intrinsic.
    return createWwm(result);
  }
  // Start the WWM section by setting the inactive lanes.
  Value *result = m_builder->CreateSetInactive(value, createGroupArithmeticIdentity(groupArithOp, value->getType()));

  // The DS swizzle mode is doing a xor of 0x1 to swap values between N <-> N+1, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = m_builder->CreateSelect(
      m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(2)),
      createGroupArithmeticOperation(
          groupArithOp, result, m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x01, 0x00, 0x1F))),
      result);

  // The DS swizzle mode is doing a xor of 0x2 to swap values between N <-> N+2, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = m_builder->CreateSelect(
      m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(4)),
      createGroupArithmeticOperation(
          groupArithOp, result, m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x02, 0x00, 0x1F))),
      result);

  // The DS swizzle mode is doing a xor of 0x4 to swap values between N <-> N+4, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = m_builder->CreateSelect(
      m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(8)),
      createGroupArithmeticOperation(
          groupArithOp, result, m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x04, 0x00, 0x1F))),
      result);

  // The DS swizzle mode is doing a xor of 0x8 to swap values between N <-> N+8, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = m_builder->CreateSelect(
      m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(16)),
      createGroupArithmeticOperation(
          groupArithOp, result, m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x08, 0x00, 0x1F))),
      result);

  // The DS swizzle mode is doing a xor of 0x10 to swap values between N <-> N+16, and the and mask of 0x1f means
  // all lanes do the same swap.
  result = m_builder->CreateSelect(
      m_builder->CreateICmpUGE(clusterSize, m_builder->getInt32(32)),
      createGroupArithmeticOperation(
          groupArithOp, result, m_builder->createDsSwizzle(result, m_builder->getDsSwizzleBitMode(0x10, 0x00, 0x1F))),
      result);

  Value *const broadcast31 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(31), instName);
  Value *const broadcast63 = m_builder->CreateSubgroupBroadcastImpl(result, m_builder->getInt32(63), instName);

  // If the cluster size is 64 we always compute the value by adding together the two broadcasts.
  result = m_builder->CreateSelect(m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(64)),
                                   createGroupArithmeticOperation(groupArithOp, broadcast31, broadcast63), result);

  Value *const threadId = createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), "");

  // If the cluster size is 32 we need to check where our invocation is in the subgroup, and conditionally use
  // invocation 31 or 63's value.
  result = m_builder->CreateSelect(
      m_builder->CreateICmpEQ(clusterSize, m_builder->getInt32(32)),
      m_builder->CreateSelect(m_builder->CreateICmpULT(threadId, m_builder->getInt32(32)), broadcast31, broadcast63),
      result);

  // Finish the WWM section by calling the intrinsic.
  return createWwm(result);
}

// =====================================================================================================================
// Generate a subgroup mbcnt from its call intrinsic
// @param call : the named call substitute with operands {Value *mask}
Value *LowerSubgroupOperations::lowerSubgroupMbcnt(llvm::CallInst &call) {

  Value *mask = call.getOperand(0);

  return createSubgroupMbcnt(mask, call.getName());
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP operations.
bool LowerSubgroupOperations::supportDpp() const {
  return m_pipelineState->getTargetInfo().getGpuProperty().supportsDpp;
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP ROW_XMASK operations.
bool LowerSubgroupOperations::supportDppRowXmask() const {
  return m_pipelineState->getTargetInfo().getGpuProperty().supportsDppRowXmask;
}

bool LowerSubgroupOperations::supportBPermute() const {
  auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion().major;
  auto supportBPermute = gfxIp == 8 || gfxIp == 9;
  auto waveSize = getShaderWaveSize();
  supportBPermute = supportBPermute || (gfxIp == 10 && waveSize == 32);
  return supportBPermute;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane operations.
bool LowerSubgroupOperations::supportsPermLane16() const {
  return m_pipelineState->getTargetInfo().getGpuProperty().supportsPermLane16;
}

// =====================================================================================================================
// Get whether the context we are building in supports permute lane 64 operations.
bool LowerSubgroupOperations::supportPermLane64() const {
  auto gfxip = m_pipelineState->getTargetInfo().getGfxIpVersion().major;
  auto waveSize = getShaderWaveSize();
  return gfxip >= 11 && waveSize == 64;
}

// =====================================================================================================================
// Create a subgroup mbcnt using the current builder and given values
Value *LowerSubgroupOperations::createSubgroupMbcnt(llvm::Value *mask, const llvm::Twine &instName) {
  // Check that the type is definitely an i64.
  assert(mask->getType()->isIntegerTy(64));

  Value *const masks = m_builder->CreateBitCast(mask, FixedVectorType::get(m_builder->getInt32Ty(), 2));
  Value *const maskLow = m_builder->CreateExtractElement(masks, m_builder->getInt32(0));
  Value *const maskHigh = m_builder->CreateExtractElement(masks, m_builder->getInt32(1));
  CallInst *const mbcntLo =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_lo, {}, {maskLow, m_builder->getInt32(0)});

  if (getShaderSubgroupSize() <= 32)
    return mbcntLo;

  return m_builder->CreateIntrinsic(Intrinsic::amdgcn_mbcnt_hi, {}, {maskHigh, mbcntLo});
}

// =====================================================================================================================
// Create a thread mask for the current thread, an integer with a single bit representing the ID of the thread set
// to 1.
Value *LowerSubgroupOperations::createThreadMask() {
  Value *threadId = createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), "");

  Value *threadMask = nullptr;
  if (getShaderSubgroupSize() <= 32)
    threadMask = m_builder->CreateShl(m_builder->getInt32(1), threadId);
  else
    threadMask =
        m_builder->CreateShl(m_builder->getInt64(1), m_builder->CreateZExtOrTrunc(threadId, m_builder->getInt64Ty()));

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
Value *LowerSubgroupOperations::createThreadMaskedSelect(Value *const threadMask, uint64_t andMask, Value *const value1,
                                                         Value *const value2) {
  Value *const andMaskVal = m_builder->getIntN(getShaderSubgroupSize(), andMask);
  Value *const zero = m_builder->getIntN(getShaderSubgroupSize(), 0);
  return m_builder->CreateSelect(m_builder->CreateICmpNE(m_builder->CreateAnd(threadMask, andMaskVal), zero), value1,
                                 value2);
}

// =====================================================================================================================
// get the subgroup size of the function that code is being generated for
unsigned int LowerSubgroupOperations::getShaderSubgroupSize() const {
  BasicBlock *block = m_builder->GetInsertBlock();
  return m_pipelineState->getShaderSubgroupSize(getShaderStage(block->getParent()));
}

// =====================================================================================================================
// get the wave size of the function that code is being generated for
unsigned int LowerSubgroupOperations::getShaderWaveSize() const {
  BasicBlock *block = m_builder->GetInsertBlock();
  return m_pipelineState->getShaderWaveSize(getShaderStage(block->getParent()));
}

// =====================================================================================================================
// Create a subgroup all call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *LowerSubgroupOperations::createSubgroupAll(Value *const value, const Twine &instName) {
  Value *result = m_builder->CreateICmpEQ(createGroupBallot(value), createGroupBallot(m_builder->getTrue()));
  result = m_builder->CreateSelect(m_builder->CreateUnaryIntrinsic(Intrinsic::is_constant, value), value, result);

  // Helper invocations of whole quad mode should be included in the subgroup vote execution
  if (getShaderStage(m_builder->GetInsertBlock()->getParent()) == ShaderStageFragment) {
    result = m_builder->CreateZExt(result, m_builder->getInt32Ty());
    result = m_builder->CreateIntrinsic(Intrinsic::amdgcn_softwqm, {m_builder->getInt32Ty()}, {result});
    result = m_builder->CreateTrunc(result, m_builder->getInt1Ty());
  }
  return result;
}

Value *LowerSubgroupOperations::createSubgroupInverseBallot(Value *const value, const Twine &instName) {
  return createSubgroupBallotBitExtract(value, createSubgroupMbcnt(m_builder->getInt64(UINT64_MAX), ""), instName);
}

// =====================================================================================================================
// Create a subgroup ballotbitextract call.
//
// @param value : The ballot value to bit extract. Must be an <4 x i32> type.
// @param index : The bit index to extract. Must be an i32 type.
// @param instName : Name to give final instruction.
Value *LowerSubgroupOperations::createSubgroupBallotBitExtract(Value *const value, Value *const index,
                                                               const Twine &instName) {
  if (getShaderSubgroupSize() <= 32) {
    Value *const indexMask = m_builder->CreateShl(m_builder->getInt32(1), index);
    Value *const valueAsInt32 = m_builder->CreateExtractElement(value, m_builder->getInt32(0));
    Value *const result = m_builder->CreateAnd(indexMask, valueAsInt32);
    return m_builder->CreateICmpNE(result, m_builder->getInt32(0));
  }
  Value *indexMask = m_builder->CreateZExtOrTrunc(index, m_builder->getInt64Ty());
  indexMask = m_builder->CreateShl(m_builder->getInt64(1), indexMask);
  Value *valueAsInt64 = m_builder->CreateShuffleVector(value, UndefValue::get(value->getType()), ArrayRef<int>{0, 1});
  valueAsInt64 = m_builder->CreateBitCast(valueAsInt64, m_builder->getInt64Ty());
  Value *const result = m_builder->CreateAnd(indexMask, valueAsInt64);
  return m_builder->CreateICmpNE(result, m_builder->getInt64(0));
}

// =====================================================================================================================
// Create a subgroup ballotexclusivebitcount call.
//
// @param value : The ballot value to exclusively bit count. Must be an <4 x i32> type.
// @param instName : Name to give final instruction.
Value *LowerSubgroupOperations::createSubgroupBallotExclusiveBitCount(Value *const value, const Twine &instName) {
  if (getShaderSubgroupSize() <= 32)
    return createSubgroupMbcnt(m_builder->CreateExtractElement(value, m_builder->getInt32(0)), "");
  Value *result = m_builder->CreateShuffleVector(value, UndefValue::get(value->getType()), ArrayRef<int>{0, 1});
  result = m_builder->CreateBitCast(result, m_builder->getInt64Ty());
  return createSubgroupMbcnt(result, "");
}

// =====================================================================================================================
// Create a subgroup shuffle call.
//
// @param value : The value to shuffle.
// @param index : The index to shuffle from.
// @param instName : Name to give final instruction.
Value *LowerSubgroupOperations::createSubgroupShuffle(Value *const value, Value *const index, const Twine &instName) {
  if (supportBPermute()) {
    auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                      ArrayRef<Value *> passthroughArgs) -> Value * {
      return builder.CreateIntrinsic(Intrinsic::amdgcn_ds_bpermute, {}, {passthroughArgs[0], mappedArgs[0]});
    };

    // The ds_bpermute intrinsic requires the index be multiplied by 4.
    return m_builder->CreateMapToInt32(mapFunc, value, m_builder->CreateMul(index, m_builder->getInt32(4)));
  }
  auto mapFunc = [this](BuilderBase &builder, ArrayRef<Value *> mappedArgs,
                        ArrayRef<Value *> passthroughArgs) -> Value * {
    Value *const readlane =
        builder.CreateIntrinsic(Intrinsic::amdgcn_readlane, {}, {mappedArgs[0], passthroughArgs[0]});
    return m_builder->createWaterfallLoop(cast<Instruction>(readlane), 1);
  };

  return m_builder->CreateMapToInt32(mapFunc, value, index);
}

// =====================================================================================================================
// Create The group arithmetic operation identity.
//
// @param groupArithOp : The group arithmetic operation to get the identity for.
// @param type : The type of the identity.
Value *LowerSubgroupOperations::createGroupArithmeticIdentity(Builder::GroupArithOp groupArithOp, Type *const type) {
  switch (groupArithOp) {
  case Builder::GroupArithOp::IAdd:
    return ConstantInt::get(type, 0);
  case Builder::GroupArithOp::FAdd:
    return ConstantFP::get(type, 0.0);
  case Builder::GroupArithOp::IMul:
    return ConstantInt::get(type, 1);
  case Builder::GroupArithOp::FMul:
    return ConstantFP::get(type, 1.0);
  case Builder::GroupArithOp::SMin:
    if (type->isIntOrIntVectorTy(8))
      return ConstantInt::get(type, INT8_MAX, true);
    else if (type->isIntOrIntVectorTy(16))
      return ConstantInt::get(type, INT16_MAX, true);
    else if (type->isIntOrIntVectorTy(32))
      return ConstantInt::get(type, INT32_MAX, true);
    else if (type->isIntOrIntVectorTy(64))
      return ConstantInt::get(type, INT64_MAX, true);
    else {
      llvm_unreachable("Unsupported integer type for SMin!");
      return nullptr;
    }
  case Builder::GroupArithOp::UMin:
    return ConstantInt::get(type, UINT64_MAX, false);
  case Builder::GroupArithOp::FMin:
    return ConstantFP::getInfinity(type, false);
  case Builder::GroupArithOp::SMax:
    if (type->isIntOrIntVectorTy(8))
      return ConstantInt::get(type, INT8_MIN, true);
    else if (type->isIntOrIntVectorTy(16))
      return ConstantInt::get(type, INT16_MIN, true);
    else if (type->isIntOrIntVectorTy(32))
      return ConstantInt::get(type, INT32_MIN, true);
    else if (type->isIntOrIntVectorTy(64))
      return ConstantInt::get(type, INT64_MIN, true);
    else {
      llvm_unreachable("Unsupported integer type for SMax!");
      return nullptr;
    }
  case Builder::GroupArithOp::UMax:
    return ConstantInt::get(type, 0, false);
  case Builder::GroupArithOp::FMax:
    return ConstantFP::getInfinity(type, true);
  case Builder::GroupArithOp::And:
    return ConstantInt::get(type, UINT64_MAX, false);
  case Builder::GroupArithOp::Or:
    return ConstantInt::get(type, 0, false);
  case Builder::GroupArithOp::Xor:
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
Value *LowerSubgroupOperations::createGroupArithmeticOperation(Builder::GroupArithOp groupArithOp, Value *const x,
                                                               Value *const y) {
  switch (groupArithOp) {
  case Builder::GroupArithOp::IAdd:
    return m_builder->CreateAdd(x, y);
  case Builder::GroupArithOp::FAdd:
    return m_builder->CreateFAdd(x, y);
  case Builder::GroupArithOp::IMul:
    return m_builder->CreateMul(x, y);
  case Builder::GroupArithOp::FMul:
    return m_builder->CreateFMul(x, y);
  case Builder::GroupArithOp::SMin:
    return m_builder->CreateSelect(m_builder->CreateICmpSLT(x, y), x, y);
  case Builder::GroupArithOp::UMin:
    return m_builder->CreateSelect(m_builder->CreateICmpULT(x, y), x, y);
  case Builder::GroupArithOp::FMin:
    return m_builder->CreateMinNum(x, y);
  case Builder::GroupArithOp::SMax:
    return m_builder->CreateSelect(m_builder->CreateICmpSGT(x, y), x, y);
  case Builder::GroupArithOp::UMax:
    return m_builder->CreateSelect(m_builder->CreateICmpUGT(x, y), x, y);
  case Builder::GroupArithOp::FMax:
    return m_builder->CreateMaxNum(x, y);
  case Builder::GroupArithOp::And:
    return m_builder->CreateAnd(x, y);
  case Builder::GroupArithOp::Or:
    return m_builder->CreateOr(x, y);
  case Builder::GroupArithOp::Xor:
    return m_builder->CreateXor(x, y);
  default:
    llvm_unreachable("Not implemented!");
    return nullptr;
  }
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
Value *LowerSubgroupOperations::createDppUpdate(Value *const origValue, Value *const updateValue, DppCtrl dppCtrl,
                                                unsigned rowMask, unsigned bankMask, bool boundCtrl) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *> passthroughArgs) -> Value * {
    return builder.CreateIntrinsic(
        Intrinsic::amdgcn_update_dpp, builder.getInt32Ty(),
        {mappedArgs[0], mappedArgs[1], passthroughArgs[0], passthroughArgs[1], passthroughArgs[2], passthroughArgs[3]});
  };

  return m_builder->CreateMapToInt32(mapFunc,
                                     {
                                         origValue,
                                         updateValue,
                                     },
                                     {m_builder->getInt32(static_cast<unsigned>(dppCtrl)), m_builder->getInt32(rowMask),
                                      m_builder->getInt32(bankMask), m_builder->getInt1(boundCtrl)});
}

// =====================================================================================================================
// Create a call to WWM (whole wave mode).
//
// @param value : The value to pass to the WWM call.
Value *LowerSubgroupOperations::createWwm(Value *const value) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    return builder.CreateUnaryIntrinsic(Intrinsic::amdgcn_wwm, mappedArgs[0]);
  };

  return m_builder->CreateMapToInt32(mapFunc, value, {});
}

// =====================================================================================================================
// Create a call to set inactive. Both active and inactive should have the same type.
//
// @param active : The value active invocations should take.
// @param inactive : The value inactive invocations should take.
Value *LowerSubgroupOperations::createSetInactive(Value *active, Value *inactive) {
  auto mapFunc = [](BuilderBase &builder, ArrayRef<Value *> mappedArgs, ArrayRef<Value *>) -> Value * {
    Value *const active = mappedArgs[0];
    Value *const inactive = mappedArgs[1];
    return builder.CreateIntrinsic(Intrinsic::amdgcn_set_inactive, active->getType(), {active, inactive});
  };

  return m_builder->CreateMapToInt32(mapFunc, {m_builder->CreateInlineAsmSideEffect(active), inactive}, {});
}

// =====================================================================================================================
// Do group ballot, turning a per-lane boolean value (in a VGPR) into a subgroup-wide shared SGPR.
//
// @param value : The value to contribute to the SGPR, must be an boolean type.
Value *LowerSubgroupOperations::createGroupBallot(Value *const value) {
  // Check the type is definitely an boolean.
  assert(value->getType()->isIntegerTy(1));

  // Turn value into an i32.
  Value *valueAsInt32 = m_builder->CreateSelect(value, m_builder->getInt32(1), m_builder->getInt32(0));

  // TODO: There is a longstanding bug with LLVM's convergent that forces us to use inline assembly with sideffects
  // to stop any hoisting out of control flow.
  valueAsInt32 = m_builder->CreateInlineAsmSideEffect(valueAsInt32);

  // The not equal predicate for the icmp intrinsic is 33.
  Constant *const predicateNe = m_builder->getInt32(33);

  // icmp has a new signature (requiring the return type as the first type).
  unsigned waveSize = getShaderWaveSize();
  Value *result =
      m_builder->CreateIntrinsic(Intrinsic::amdgcn_icmp, {m_builder->getIntNTy(waveSize), m_builder->getInt32Ty()},
                                 {valueAsInt32, m_builder->getInt32(0), predicateNe});

  // If we have a 32-bit subgroup size, we need to turn the 32-bit ballot result into a 64-bit result.
  if (waveSize <= 32)
    result = m_builder->CreateZExt(result, m_builder->getInt64Ty(), "");

  return result;
}

// =====================================================================================================================
// Initializes the pass of LLVM patch operations for checking shader cache
INITIALIZE_PASS(LegacyLowerSubgroupOperations, DEBUG_TYPE, "Lower subgroup operations after wavesize is decided", false,
                false)
