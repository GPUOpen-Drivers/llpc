/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchLoopMetadata.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchLoopMetadata.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchLoopMetadata.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <vector>

#define DEBUG_TYPE "lgc-patch-loop-metadata"

using namespace llvm;
using namespace lgc;

namespace llvm {
// A proxy from a ModuleAnalysisManager to a loop.
typedef OuterAnalysisManagerProxy<ModuleAnalysisManager, Loop, LoopStandardAnalysisResults &>
    ModuleAnalysisManagerLoopProxy;
} // namespace llvm

// =====================================================================================================================
// Update metadata by removing any existing metadata with the specified prefix, and then adding the new metadata if
// existing metadata was removed or conditional is false.
//
// @param loopId : loop
// @param prefixesToRemove : metadata prefixes to be removed
// @param newMetadata : the new metadata to be added
// @param conditional : true if the new metadata is only to be added if one or more prefixes was removed
MDNode *PatchLoopMetadata::updateMetadata(MDNode *loopId, ArrayRef<StringRef> prefixesToRemove, Metadata *newMetadata,
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
                   [mdString](StringRef prefix) -> bool { return mdString->getString().startswith(prefix); }))
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
PatchLoopMetadata::PatchLoopMetadata()
    : m_context(nullptr), m_forceLoopUnrollCount(0), m_disableLoopUnroll(false), m_disableLicmThreshold(0),
      m_unrollHintThreshold(0), m_dontUnrollHintThreshold(0) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] loop : LLVM loop to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @param [in/out] loopAnalysisResult : Loop standard analysis results
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchLoopMetadata::run(Loop &loop, LoopAnalysisManager &analysisManager,
                                         LoopStandardAnalysisResults &loopAnalysisResults, LPMUpdater &) {
  Module *module = loop.getHeader()->getModule();
  const auto &mamProxy = analysisManager.getResult<ModuleAnalysisManagerLoopProxy>(loop, loopAnalysisResults);
  PipelineState *pipelineState = mamProxy.getCachedResult<PipelineStateWrapper>(*module)->getPipelineState();
  if (runImpl(loop, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] loop : LLVM loop to be run on
// @param pipelineState : Pipeline state
// @returns : True if the loop was modified by the transformation and false otherwise
bool PatchLoopMetadata::runImpl(Loop &loop, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass lgc-patch-loop-metadata\n");

  Function *func = loop.getHeader()->getFirstNonPHI()->getFunction();
  PipelineState *mPipelineState = pipelineState;
  m_context = &loop.getHeader()->getContext();

  m_gfxIp = mPipelineState->getTargetInfo().getGfxIpVersion();
  bool changed = false;

  ShaderStage stage = getShaderStage(func);
  if (stage == ShaderStageInvalid)
    return false;
  if (auto shaderOptions = &mPipelineState->getShaderOptions(stage)) {
    m_disableLoopUnroll = shaderOptions->disableLoopUnroll;
    m_forceLoopUnrollCount = shaderOptions->forceLoopUnrollCount;
    m_disableLicmThreshold = shaderOptions->disableLicmThreshold;
    m_unrollHintThreshold = shaderOptions->unrollHintThreshold;
    m_dontUnrollHintThreshold = shaderOptions->dontUnrollHintThreshold;
  }

  MDNode *loopMetaNode = loop.getLoopID();
  if (!loopMetaNode || loopMetaNode->getOperand(0) != loopMetaNode)
    return false;

  LLVM_DEBUG(dbgs() << "loop in " << func->getName() << " at depth " << loop.getLoopDepth() << " has "
                    << loop.getNumBlocks() << " blocks\n");

  if (m_disableLoopUnroll) {
    LLVM_DEBUG(dbgs() << "  disabling loop unroll\n");
    // The disableLoopUnroll option overrides any existing loop metadata (so is
    // subtly different to forceLoopUnrollCount=1 which defers to any existing
    // metadata).
    MDNode *disableLoopUnrollMetaNode = MDNode::get(*m_context, MDString::get(*m_context, "llvm.loop.unroll.disable"));
    loopMetaNode = updateMetadata(loopMetaNode, {"llvm.loop"}, disableLoopUnrollMetaNode, false);
    changed = true;
  } else if (m_forceLoopUnrollCount && loopMetaNode->getNumOperands() <= 1) {
    LLVM_DEBUG(dbgs() << "  forcing loop unroll count to " << m_forceLoopUnrollCount << "\n");
    // We have a loop backedge with !llvm.loop metadata containing just one
    // operand pointing to itself, meaning that the SPIR-V did not have an
    // unroll directive, so we can add the force unroll count metadata.
    Metadata *unrollCountMeta[] = {
        MDString::get(*m_context, "llvm.loop.unroll.count"),
        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), m_forceLoopUnrollCount))};
    MDNode *loopUnrollCountMetaNode = MDNode::get(*m_context, unrollCountMeta);
    loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, loopUnrollCountMetaNode));
    // We also disable all nonforced loop transformations to ensure our
    // transformation is not blocked
    Metadata *nonforcedMeta[] = {MDString::get(*m_context, "llvm.loop.disable_nonforced")};
    MDNode *nonforcedMetaNode = MDNode::get(*m_context, nonforcedMeta);
    loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, nonforcedMetaNode));
    changed = true;
  } else if (m_unrollHintThreshold > 0 || m_dontUnrollHintThreshold > 0) {
    for (unsigned i = 1, operandCount = loopMetaNode->getNumOperands(); i < operandCount; ++i) {
      Metadata *op = loopMetaNode->getOperand(i);
      if (MDNode *mdNode = dyn_cast<MDNode>(op)) {
        if (const MDString *mdString = dyn_cast<MDString>(mdNode->getOperand(0))) {
          if (m_dontUnrollHintThreshold > 0 && mdString->getString().startswith("llvm.loop.unroll.disable")) {
            LLVM_DEBUG(dbgs() << "  relaxing llvm.loop.unroll.disable to amdgpu.loop.unroll.threshold "
                              << m_dontUnrollHintThreshold << "\n");
            Metadata *thresholdMeta[] = {
                MDString::get(*m_context, "amdgpu.loop.unroll.threshold"),
                ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), m_dontUnrollHintThreshold))};
            MDNode *thresholdMetaNode = MDNode::get(*m_context, thresholdMeta);
            loopMetaNode = updateMetadata(loopMetaNode, {"llvm.loop.unroll.disable", "llvm.loop.disable_nonforced"},
                                          thresholdMetaNode, false);
            changed = true;
            break;
          }
          if (m_unrollHintThreshold > 0 && mdString->getString().startswith("llvm.loop.unroll.full")) {
            LLVM_DEBUG(dbgs() << "  relaxing llvm.loop.unroll.full to amdgpu.loop.unroll.threshold "
                              << m_unrollHintThreshold << "\n");
            Metadata *thresholdMeta[] = {
                MDString::get(*m_context, "amdgpu.loop.unroll.threshold"),
                ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), m_unrollHintThreshold))};
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

  if (m_disableLicmThreshold > 0 && loop.getNumBlocks() >= m_disableLicmThreshold) {
    LLVM_DEBUG(dbgs() << "  disabling LICM\n");
    MDNode *licmDisableNode = MDNode::get(*m_context, MDString::get(*m_context, "llvm.licm.disable"));
    loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, licmDisableNode));
    changed = true;
  }
  if (changed) {
    loopMetaNode->replaceOperandWith(0, loopMetaNode);
    loop.setLoopID(loopMetaNode);
  }

  return changed;
}
