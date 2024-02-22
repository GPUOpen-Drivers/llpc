/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerSubgroupOps.cpp
 * @brief The lgc::LowerSubgroupOps pass lowers subgroup operations represented as dialect ops to LLVM IR
 ***********************************************************************************************************************
 */

#include "lgc/patch/LowerSubgroupOps.h"
#include "ShaderMerger.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/AbiMetadata.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/AddressExtender.h"
#include "lgc/util/BuilderBase.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Analysis/AliasAnalysis.h" // for MemoryEffects
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <optional>

#define DEBUG_TYPE "lgc-lower-subgroup-ops"

using namespace llvm;
using namespace lgc;

namespace lgc {

class SubgroupLoweringBuilder : public BuilderImpl {
public:
  SubgroupLoweringBuilder(Pipeline *pipeline) : BuilderImpl(pipeline) {}

  // =====================================================================================================================
  // Create a subgroup elect.
  //
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateSubgroupElect(const llvm::Twine &instName = "");

  // Create a subgroup any
  //
  // @param value : The value to compare
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateSubgroupAny(llvm::Value *const value, const llvm::Twine &instName = "");
};

// =====================================================================================================================
// Create a subgroup elect call.
//
// @param instName : Name to give final instruction.
Value *SubgroupLoweringBuilder::CreateSubgroupElect(const Twine &instName) {
  return CreateICmpEQ(CreateSubgroupMbcnt(createGroupBallot(getTrue()), ""), getInt32(0));
}

// =====================================================================================================================
// Create a subgroup any call.
//
// @param value : The value to compare across the subgroup. Must be an integer type.
// @param instName : Name to give final instruction.
Value *SubgroupLoweringBuilder::CreateSubgroupAny(Value *const value, const Twine &instName) {
  Value *result = CreateICmpNE(createGroupBallot(value), getInt64(0));
  result = CreateSelect(CreateUnaryIntrinsic(Intrinsic::is_constant, value), value, result);

  // Helper invocations of whole quad mode should be included in the subgroup vote execution
  const auto &fragmentMode = m_pipelineState->getShaderModes()->getFragmentShaderMode();
  if (m_shaderStage == ShaderStage::Fragment && !fragmentMode.waveOpsExcludeHelperLanes) {
    result = CreateZExt(result, getInt32Ty());
    result = CreateIntrinsic(Intrinsic::amdgcn_softwqm, {getInt32Ty()}, {result});
    result = CreateTrunc(result, getInt1Ty());
  }
  return result;
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerSubgroupOps::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  // PipelineShadersResult &pipelineShaders = analysisManager.getResult<PipelineShaders>(module);
  LLVM_DEBUG(dbgs() << "Run the pass lower subgroup ops\n");

  m_pipelineState = pipelineState;

  SubgroupLoweringBuilder builder(m_pipelineState);
  m_builder = &builder;
  static const auto visitor = llvm_dialects::VisitorBuilder<LowerSubgroupOps>()
                                  .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                  .add(&LowerSubgroupOps::visitElect)
                                  .add(&LowerSubgroupOps::visitAny)
                                  .build();
  visitor.visit(*this, module);
  m_builder = nullptr;

  return PreservedAnalyses::none();
}

void LowerSubgroupOps::replace(CallInst &old, Value *op) {
  old.replaceAllUsesWith(op);
  old.dropAllReferences();
  old.eraseFromParent();
}

void LowerSubgroupOps::visitElect(SubgroupElectOp &op) {
  m_builder->SetInsertPoint(&op);
  replace(op, m_builder->CreateSubgroupElect());
}

void LowerSubgroupOps::visitAny(SubgroupAnyOp &op) {
  m_builder->SetInsertPoint(&op);
  replace(op, m_builder->CreateSubgroupAny(op.getValue()));
}

} // namespace lgc
