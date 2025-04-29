/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  AddLoopMetadata.cpp
 * @brief LLPC source file: contains implementation of class lgc::AddLoopMetadata.
 ***********************************************************************************************************************
 */

#include "lgc/lowering/AddLoopMetadata.h"
#include "lgc/LgcDialect.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <vector>

#define DEBUG_TYPE "lgc-add-loop-metadata"

using namespace llvm;
using namespace lgc;

namespace llvm {
// A proxy from a ModuleAnalysisManager to a loop.
typedef OuterAnalysisManagerProxy<ModuleAnalysisManager, Loop, LoopStandardAnalysisResults &>
    ModuleAnalysisManagerLoopProxy;
} // namespace llvm

// =====================================================================================================================
// Get the alloca variable
static Value *getBaseAlloca(Value *v) {
  if (dyn_cast<AllocaInst>(v))
    return v;

  if (auto *gep = dyn_cast<GetElementPtrInst>(v))
    return getBaseAlloca(gep->getPointerOperand());

  return nullptr;
}

// =====================================================================================================================
// Return the loop trip count if it is small constant and there is dynamic indexing of the input variable inside the
// loop, otherwise return zero.
//
// @param [in] loop : Loop object
// @param [in/out] analyzer : ScalarEvolutionAnalysis
// @param inputOps : input instructions
// @returns : Loop unroll count
static unsigned getLoopTripCountIfDynamicImport(Loop *loop, ScalarEvolutionAnalysis::Result &seAnalyzer,
                                                const SmallVectorImpl<Instruction *> &inputOps) {
  // Find indexed variable
  PHINode *inductionVariable = nullptr;
  BasicBlock *Header = loop->getHeader();
  for (PHINode &PN : Header->phis()) {
    if (!seAnalyzer.isSCEVable(PN.getType()))
      continue;
    const SCEV *S = seAnalyzer.getSCEV(&PN);
    if (seAnalyzer.isLoopInvariant(S, loop)) {
      continue;
    }
    if (isa<SCEVAddRecExpr>(S)) {
      inductionVariable = &PN;
      break;
    }
  }

  if (!inductionVariable)
    return 0;

  unsigned tripCount = seAnalyzer.getSmallConstantTripCount(loop);

  // Considering that the size of input variables does not exceed 32, LoopUnrollCount is limited to be less than 32.
  if (tripCount == 0 || tripCount > 32)
    return 0;

  SmallDenseSet<Value *> allocaInsts;
  // Find the AllocaInst
  for (auto *inputOp : inputOps) {
    SmallVector<Value *> worklist(inputOp->users());
    while (!worklist.empty()) {
      Value *curInst = worklist.pop_back_val();
      if (isa<InsertElementInst>(curInst)) {
        // If the vector is loaded component by component, an InsertElementInst instruction is used in front of store
        // instruction.
        worklist.append(curInst->user_begin(), curInst->user_end());
        continue;
      }

      if (auto storeInst = dyn_cast<StoreInst>(curInst)) {
        if (Value *allocaInst = getBaseAlloca(storeInst->getPointerOperand()))
          allocaInsts.insert(allocaInst);
      }
    }
  }

  // Check whether the dynamic index of the gep is derived from induction variable.
  for (Value *allocaInst : allocaInsts) {
    SmallVector<Value *> allocaWorklist(allocaInst->users());
    while (!allocaWorklist.empty()) {
      Value *curInst = allocaWorklist.pop_back_val();
      GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(curInst);
      if (!gep)
        continue;

      if (gep->hasAllConstantIndices()) {
        // Its user may be a GetElementPtrInst that contains a dynamic index.
        allocaWorklist.append(gep->user_begin(), gep->user_end());
        continue;
      }

      if (!loop->contains(gep->getParent()))
        break;

      for (Value *idx : gep->indices()) {
        if (isa<ConstantInt>(idx))
          continue;

        SmallVector<Value *> worklist = {inductionVariable};

        while (!worklist.empty()) {
          Value *inst = worklist.pop_back_val();
          if (inst == idx)
            return tripCount;

          for (User *user : inst->users())
            if (isa<BinaryOperator>(user))
              worklist.push_back(user);
        }
      }
    }
  }
  return 0;
}

// =====================================================================================================================
// Update metadata by removing any existing metadata with the specified prefix, and then adding the new metadata if
// existing metadata was removed or conditional is false.
//
// @param loopId : loop
// @param prefixesToRemove : metadata prefixes to be removed
// @param newMetadata : the new metadata to be added
// @param conditional : true if the new metadata is only to be added if one or more prefixes was removed
MDNode *AddLoopMetadata::updateMetadata(MDNode *loopId, ArrayRef<StringRef> prefixesToRemove, Metadata *newMetadata,
                                        bool conditional) {
  bool found = false;
  SmallVector<Metadata *, 4> mds;
  // Reserve first location for self reference to the loopId metadata node.
  TempMDTuple tempNode = MDNode::getTemporary(*m_context, {});
  mds.push_back(tempNode.get());
  for (unsigned i = 1, operandCount = loopId->getNumOperands(); i < operandCount; ++i) {
    Metadata *op = loopId->getOperand(i);
    if (MDNode *mdNode = dyn_cast<MDNode>(op)) {
      if (const MDString *mdString = dyn_cast<MDString>(mdNode->getOperand(0))) {
        if (any_of(prefixesToRemove,
                   [mdString](StringRef prefix) -> bool { return mdString->getString().starts_with(prefix); }))
          found = true;
        else
          mds.push_back(op);
      }
    }
  }
  if (!conditional || found) {
    mds.push_back(newMetadata);
    MDNode *newLoopId = MDNode::getDistinct(*m_context, mds);
    return newLoopId;
  }

  // Return the metadata unmodified
  return loopId;
};

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] function : Function that we will patch.
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses AddLoopMetadata::run(llvm::Function &function, llvm::FunctionAnalysisManager &analysisManager) {
  auto shaderStage = lgc::getShaderStage(&function);
  if (!shaderStage)
    return PreservedAnalyses::all();

  auto &loopInfos = analysisManager.getResult<LoopAnalysis>(function);
  // There are no loops in the function. Return before computing other expensive
  // analyses.
  if (loopInfos.empty())
    return PreservedAnalyses::all();

  SmallVector<Loop *> loopList(loopInfos.begin(), loopInfos.end());
  unsigned idx = 0;
  while (idx != loopList.size()) {
    Loop *currentLoop = loopList[idx++];
    for (Loop *nestedLoop : *currentLoop)
      loopList.push_back(nestedLoop);
  }

  m_context = &function.getParent()->getContext();

  const auto &moduleAnalysisManager = analysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(function);
  PipelineState *pipelineState =
      moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*function.getParent())->getPipelineState();

  LLVM_DEBUG(dbgs() << "Run the pass Add-Loop-Metadata\n");

  bool changed = false;
  unsigned forceLoopUnrollCount = 0;    // Force loop unroll count
  bool disableLoopUnroll = 0;           // Forcibly disable loop unroll
  unsigned disableLicmThreshold = 0;    // Disable LLVM LICM pass loop block count threshold
  unsigned unrollHintThreshold = 0;     // Unroll hint threshold
  unsigned dontUnrollHintThreshold = 0; // DontUnroll hint threshold
  if (auto shaderOptions = &pipelineState->getShaderOptions(shaderStage.value())) {
    disableLoopUnroll = shaderOptions->disableLoopUnroll;
    forceLoopUnrollCount = shaderOptions->forceLoopUnrollCount;
    disableLicmThreshold = shaderOptions->disableLicmThreshold;
    unrollHintThreshold = shaderOptions->unrollHintThreshold;
    dontUnrollHintThreshold = shaderOptions->dontUnrollHintThreshold;
  }

  // If a loop contains a load input variable, this loop is expected to be unrolled.
  SmallVector<Instruction *, 4> inputOps;
  static auto visitor = llvm_dialects::VisitorBuilder<SmallVectorImpl<Instruction *>>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .addSet<InputImportGenericOp, InputImportInterpolatedOp>(
                                [](auto &importOps, Instruction &op) { importOps.push_back(&op); })
                            .build();
  visitor.visit(inputOps, function);

  auto &seAnalysis = analysisManager.getResult<ScalarEvolutionAnalysis>(function);
  for (const auto &loop : loopList) {
    MDNode *loopMetaNode = loop->getLoopID();
    if (!loopMetaNode) {
      auto temp = MDNode::getTemporary(*m_context, {});
      loopMetaNode = MDNode::get(*m_context, temp.get());
      loopMetaNode->replaceOperandWith(0, loopMetaNode);
    } else if (loopMetaNode->getOperand(0) != loopMetaNode)
      continue;

    LLVM_DEBUG(dbgs() << "loop in " << function.getName() << " at depth " << loop->getLoopDepth() << " has "
                      << loop->getNumBlocks() << " blocks\n");

    if (disableLoopUnroll) {
      LLVM_DEBUG(dbgs() << "  disabling loop unroll\n");
      // The disableLoopUnroll option overrides any existing loop metadata (so is
      // subtly different to forceLoopUnrollCount=1 which defers to any existing
      // metadata).
      MDNode *disableLoopUnrollMetaNode =
          MDNode::get(*m_context, MDString::get(*m_context, "llvm.loop.unroll.disable"));
      loopMetaNode = updateMetadata(loopMetaNode, {"llvm.loop"}, disableLoopUnrollMetaNode, false);
      changed = true;
    } else {
      unsigned expectLoopUnrollCount = getLoopTripCountIfDynamicImport(loop, seAnalysis, inputOps);
      forceLoopUnrollCount = std::max(forceLoopUnrollCount, expectLoopUnrollCount);

      if (forceLoopUnrollCount && loopMetaNode->getNumOperands() <= 1) {
        LLVM_DEBUG(dbgs() << "  forcing loop unroll count to " << forceLoopUnrollCount << "\n");
        // We have a loop backedge with !llvm.loop metadata containing just one
        // operand pointing to itself, meaning that the SPIR-V did not have an
        // unroll directive, so we can add the force unroll count metadata.
        Metadata *unrollCountMeta[] = {
            MDString::get(*m_context, "llvm.loop.unroll.count"),
            ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), forceLoopUnrollCount))};
        MDNode *loopUnrollCountMetaNode = MDNode::get(*m_context, unrollCountMeta);
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, loopUnrollCountMetaNode));
        // We also disable all nonforced loop transformations to ensure our
        // transformation is not blocked
        Metadata *nonforcedMeta[] = {MDString::get(*m_context, "llvm.loop.disable_nonforced")};
        MDNode *nonforcedMetaNode = MDNode::get(*m_context, nonforcedMeta);
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, nonforcedMetaNode));
        changed = true;
      } else if (unrollHintThreshold > 0 || dontUnrollHintThreshold > 0) {
        for (unsigned i = 1, operandCount = loopMetaNode->getNumOperands(); i < operandCount; ++i) {
          Metadata *op = loopMetaNode->getOperand(i);
          if (MDNode *mdNode = dyn_cast<MDNode>(op)) {
            if (const MDString *mdString = dyn_cast<MDString>(mdNode->getOperand(0))) {
              if (dontUnrollHintThreshold > 0 && mdString->getString().starts_with("llvm.loop.unroll.disable")) {
                LLVM_DEBUG(dbgs() << "  relaxing llvm.loop.unroll.disable to amdgpu.loop.unroll.threshold "
                                  << dontUnrollHintThreshold << "\n");
                Metadata *thresholdMeta[] = {
                    MDString::get(*m_context, "amdgpu.loop.unroll.threshold"),
                    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), dontUnrollHintThreshold))};
                MDNode *thresholdMetaNode = MDNode::get(*m_context, thresholdMeta);
                loopMetaNode = updateMetadata(loopMetaNode, {"llvm.loop.unroll.disable", "llvm.loop.disable_nonforced"},
                                              thresholdMetaNode, false);
                changed = true;
                break;
              }
              if (unrollHintThreshold > 0 && mdString->getString().starts_with("llvm.loop.unroll.full")) {
                LLVM_DEBUG(dbgs() << "  relaxing llvm.loop.unroll.full to amdgpu.loop.unroll.threshold "
                                  << unrollHintThreshold << "\n");
                Metadata *thresholdMeta[] = {
                    MDString::get(*m_context, "amdgpu.loop.unroll.threshold"),
                    ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), unrollHintThreshold))};
                MDNode *thresholdMetaNode = MDNode::get(*m_context, thresholdMeta);
                loopMetaNode = updateMetadata(loopMetaNode, {"llvm.loop.unroll.full", "llvm.loop.disable_nonforced"},
                                              thresholdMetaNode, false);
                changed = true;
                break;
              }
            }
          }
        }
      }
    }

    if (disableLicmThreshold > 0 && loop->getNumBlocks() >= disableLicmThreshold) {
      LLVM_DEBUG(dbgs() << "  disabling LICM\n");
      MDNode *licmDisableNode = MDNode::get(*m_context, MDString::get(*m_context, "llvm.licm.disable"));
      loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, licmDisableNode));
      changed = true;
    }
    if (changed) {
      loopMetaNode->replaceOperandWith(0, loopMetaNode);
      loop->setLoopID(loopMetaNode);
    }
  }

  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
