/*
************************************************************************************************************************
*
*  Copyright (C) 2021 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  LowerSubgroupOperations.h
 * @brief LLPC header file: contains declaration of class lgc::LowerSubgroupOperations
 ***********************************************************************************************************************
 */
#pragma once

#include "map"
#include "lgc/Builder.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"
#include "llvm/Analysis/LegacyDivergenceAnalysis.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

namespace lgc {

// =====================================================================================================================
// Implements subgroup operation lowering
class LowerSubgroupOperations : public llvm::PassInfoMixin<LowerSubgroupOperations> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Lowers subgroup operations"; }

private:
  // Generate a subgroup get subgroup size from it's call intrinsic
  llvm::Value *lowerGetSubgroupSize(llvm::CallInst &call);

  // Generate a subgroup get wave size from it's call intrinsic
  llvm::Value *lowerGetWaveSize(llvm::CallInst &call);

  // generate a subgroup elect from it's call intrinsic
  llvm::Value *lowerSubgroupElect(llvm::CallInst &call);

  // generate a subgroup all from it's call intrinsic
  llvm::Value *lowerSubgroupAll(llvm::CallInst &call);

  // generate a subgroup any from it's call intrinsic
  llvm::Value *lowerSubgroupAny(llvm::CallInst &call);

  // generate a subgroup all equal from it's call intrinsic
  llvm::Value *lowerSubgroupAllEqual(llvm::CallInst &call);

  // generate a subgroup ballot from it's call intrinsic
  llvm::Value *lowerSubgroupBallot(llvm::CallInst &call);

  // generate a subgroup inverse ballot from it's call intrinsic
  llvm::Value *lowerSubgroupInverseBallot(llvm::CallInst &call);

  // Generate a subgroup ballotbitcount from it's call intrinsic
  llvm::Value *lowerSubgroupBallotBitExtract(llvm::CallInst &call);

  // Generate a subgroup ballotbitcount from it's call intrinsic
  llvm::Value *lowerSubgroupBallotBitCount(llvm::CallInst &ucallse);

  // Generate a subgroup ballot inclusive bit count from it's call intrinsic
  llvm::Value *lowerSubgroupBallotInclusiveBitCount(llvm::CallInst &call);

  // Generate a subgroup ballotexclusivebitcount from it's call intrinsic
  llvm::Value *lowerSubgroupBallotExclusiveBitCount(llvm::CallInst &call);

  // Generate a subgroup ballotfindlsb from it's call intrinsic
  llvm::Value *lowerSubgroupBallotFindLsb(llvm::CallInst &call);

  // Generate a subgroup ballotfindmsb from it's call intrinsic
  llvm::Value *lowerSubgroupBallotFindMsb(llvm::CallInst &call);

  // Generate a subgroup subgroup shuffle from it's call intrinsic
  llvm::Value *lowerSubgroupShuffle(llvm::CallInst &call);

  // Generate a subgroup shuffle xor from it's call intrinsic
  llvm::Value *lowerSubgroupShuffleXor(llvm::CallInst &call);

  // Generate a subgroup shuffle up from it's call intrinsic
  llvm::Value *lowerSubgroupShuffleUp(llvm::CallInst &call);

  // Generate a subgroup shuffle down from it's call intrinsic
  llvm::Value *lowerSubgroupShuffleDown(llvm::CallInst &call);

  // Generate a subgroup clustered inclusive scan from it's call intrinsic
  llvm::Value *lowerSubgroupClusteredInclusive(llvm::CallInst &call);

  // Generate a subgroup clustered exclusive scan from it's call intrinsic
  llvm::Value *lowerSubgroupClusteredExclusive(llvm::CallInst &call);

  // Create a subgroup clustered reduction from it's call intrinsic
  llvm::Value *lowerSubgroupClusteredReduction(llvm::CallInst &call);

  // Generate a subgroup mbcnt from it's call intrinsic
  llvm::Value *lowerSubgroupMbcnt(llvm::CallInst &call);

  // Get whether the context we are building in supports DPP operations.
  bool supportDpp() const;
  // Get whether the context we are building in supports DPP ROW_XMASK operations.
  bool supportDppRowXmask() const;
  // Get whether context we are building in support the bpermute operation.
  bool supportBPermute() const;
  // Get whether the context we are building in supports permute lane DPP operations.
  bool supportPermLaneDpp() const;
  // Get whether the context we are building in supports permute lane 64 DPP operations.
  bool supportPermLane64Dpp() const;
  // Create a thread mask for the current thread, an integer with a single bit representing the ID of the thread set
  // to 1.
  llvm::Value *createSubgroupMbcnt(llvm::Value *const mask, const llvm::Twine &instName = "");
  // Create a thread mask for the current thread, an integer with a single bit representing the ID of the thread set
  // to 1.
  llvm::Value *createThreadMask();
  // Create a masked operation - taking a thread mask and a mask to and it with, select between the first value and the
  // second value if the current thread is active.
  llvm::Value *createThreadMaskedSelect(llvm::Value *const threadMask, uint64_t andMask, llvm::Value *const value1,
                                        llvm::Value *const value2);
  // get the wave size of the function that code is being generated for
  unsigned int getShaderSubgroupSize() const;
  // get the wave size of the function that code is being generated for
  unsigned int getShaderWaveSize() const;
  // Create a subgroup all
  llvm::Value *createSubgroupAll(llvm::Value *const value, const llvm::Twine &instName);
  // Create a subgroup inverse ballot
  llvm::Value *createSubgroupInverseBallot(llvm::Value *const value, const llvm::Twine &instName);
  // Create a subgroup ballot bit extract
  llvm::Value *createSubgroupBallotBitExtract(llvm::Value *const value, llvm::Value *const index,
                                              const llvm::Twine &instName);
  // Create a subgroup ballot exclusive bit count
  llvm::Value *createSubgroupBallotExclusiveBitCount(llvm::Value *const value, const llvm::Twine &instName);
  // Create a subgroup shuffle
  llvm::Value *createSubgroupShuffle(llvm::Value *const value, llvm::Value *const index, const llvm::Twine &instName);
  // Create The group arithmetic operation identity.
  llvm::Value *createGroupArithmeticIdentity(Builder::GroupArithOp groupArithOp, llvm::Type *const type);
  // Create The group arithmetic operation arithmetic on x and y.
  llvm::Value *createGroupArithmeticOperation(Builder::GroupArithOp groupArithOp, llvm::Value *const x,
                                              llvm::Value *const y);
  // Create a call to dpp update.
  llvm::Value *createDppUpdate(llvm::Value *const origValue, llvm::Value *const updateValue, DppCtrl dppCtrl,
                               unsigned rowMask, unsigned bankMask, bool boundCtrl);
  // Create a call to WWM (whole wave mode).
  llvm::Value *createWwm(llvm::Value *const value);
  // Create a call to set inactive. Both active and inactive should have the same type.
  llvm::Value *createSetInactive(llvm::Value *const active, llvm::Value *const inactive);
  // Do group ballot, turning a per-lane boolean value (in a VGPR) into a subgroup-wide shared SGPR.
  llvm::Value *createGroupBallot(llvm::Value *const value);

  PipelineState *m_pipelineState;
  std::unique_ptr<BuilderBase> m_builder;
};

// =====================================================================================================================
// Represents the pass of lowering subgroup operations
class LegacyLowerSubgroupOperations : public llvm::ModulePass {
public:
  static char ID; // ID of this pass

  LegacyLowerSubgroupOperations() : ModulePass(ID){};

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<LegacyPipelineStateWrapper>();
    analysisUsage.addRequired<LegacyPipelineShaders>();
  }

  bool runOnModule(llvm::Module &module) override;

private:
  LegacyLowerSubgroupOperations(const LegacyLowerSubgroupOperations &) = delete;
  LegacyLowerSubgroupOperations &operator=(const LegacyLowerSubgroupOperations &) = delete;

  LowerSubgroupOperations m_impl;
};

} // namespace lgc
