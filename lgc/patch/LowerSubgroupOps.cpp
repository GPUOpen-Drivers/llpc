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
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/SubgroupBuilder.h"
#include "lgc/state/PipelineState.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "lgc-lower-subgroup-ops"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerSubgroupOps::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  LLVM_DEBUG(dbgs() << "Run the pass lower subgroup ops\n");

  m_pipelineState = pipelineState;

  SubgroupBuilder builder(m_pipelineState);
  m_builder = &builder;
  static const auto visitor = llvm_dialects::VisitorBuilder<LowerSubgroupOps>()
                                  .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                                  .add(&LowerSubgroupOps::visitElect)
                                  .add(&LowerSubgroupOps::visitAny)
                                  .add(&LowerSubgroupOps::visitAll)
                                  .add(&LowerSubgroupOps::visitAllEqual)
                                  .add(&LowerSubgroupOps::visitRotate)
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

void LowerSubgroupOps::visitAll(SubgroupAllOp &op) {
  m_builder->SetInsertPoint(&op);
  replace(op, m_builder->CreateSubgroupAll(op.getValue()));
}

void LowerSubgroupOps::visitAllEqual(SubgroupAllEqualOp &op) {
  m_builder->SetInsertPoint(&op);
  replace(op, m_builder->CreateSubgroupAllEqual(op.getValue()));
}

void LowerSubgroupOps::visitRotate(SubgroupRotateOp &op) {
  m_builder->SetInsertPoint(&op);
  Value *cs = op.getClusterSize();
  replace(op, m_builder->CreateSubgroupRotate(op.getValue(), op.getDelta(), isa<PoisonValue>(cs) ? nullptr : cs));
}

} // namespace lgc
