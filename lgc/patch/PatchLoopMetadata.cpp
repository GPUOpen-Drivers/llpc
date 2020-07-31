/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <vector>

#define DEBUG_TYPE "llpc-patch-loop-metadata"

using namespace llvm;
using namespace lgc;

namespace {

// =====================================================================================================================
// Represents the LLVM pass for patching loop metadata.
class PatchLoopMetadata : public Patch {
public:
  PatchLoopMetadata();

  bool runOnModule(Module &module) override;

  static char ID; // ID of this pass

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
  }

private:
  unsigned m_forceLoopUnrollCount; // Force loop unroll count
  bool m_disableLicm;              // Disable LLVM LICM pass
  bool m_disableLoopUnroll;        // Forcibly disable loop unroll
  GfxIpVersion m_gfxIp;
};

} // anonymous namespace

// =====================================================================================================================
// Initializes static members.
char PatchLoopMetadata::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass for patching loop metadata
ModulePass *lgc::createPatchLoopMetadata() {
  return new PatchLoopMetadata();
}

// =====================================================================================================================
PatchLoopMetadata::PatchLoopMetadata()
    : Patch(ID), m_forceLoopUnrollCount(0), m_disableLicm(false), m_disableLoopUnroll(false) {
}

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool PatchLoopMetadata::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass patch-loop-metadata\n");

  Patch::init(&module);
  PipelineState *m_pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);
  m_gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  bool changed = false;

  for (Function &func : module) {
    ShaderStage stage = getShaderStage(&func);
    if (stage == -1)
      continue;
    if (auto shaderOptions = &m_pipelineState->getShaderOptions(stage)) {
      m_disableLoopUnroll = shaderOptions->disableLoopUnroll;
      m_forceLoopUnrollCount = shaderOptions->forceLoopUnrollCount;
      m_disableLicm = shaderOptions->disableLicm;
    }

    for (auto &block : func) {
      auto terminator = block.getTerminator();
      MDNode *loopMetaNode = terminator->getMetadata("llvm.loop");
      if (!loopMetaNode || loopMetaNode->getOperand(0) != loopMetaNode ||
          (loopMetaNode->getNumOperands() != 1 && !m_disableLicm && !m_disableLoopUnroll))
        continue;

      if (m_disableLoopUnroll) {
        // The disableLoopUnroll option overrides any existing loop metadata (so is subtly different to
        // forceLoopUnrollCount=1 which defers to any existing metadata). We can simply concatenate
        // it as it takes precedence over any other metadata that may already be present.
        MDNode *disableLoopUnrollMetaNode =
            MDNode::get(*m_context, MDString::get(*m_context, "llvm.loop.unroll.disable"));
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, disableLoopUnrollMetaNode));
      } else if (m_forceLoopUnrollCount && loopMetaNode->getNumOperands() <= 1) {
        // We have a loop backedge with !llvm.loop metadata containing just one
        // operand pointing to itself, meaning that the SPIR-V did not have an
        // unroll directive, so we can add the force unroll count metadata.
        Metadata *unrollCountMeta[] = {
            MDString::get(*m_context, "llvm.loop.unroll.count"),
            ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), m_forceLoopUnrollCount))};
        MDNode *loopUnrollCountMetaNode = MDNode::get(*m_context, unrollCountMeta);
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, loopUnrollCountMetaNode));
        // We also disable all nonforced loop transformations to ensure our transformation is not blocked
        Metadata *nonforcedMeta[] = {MDString::get(*m_context, "llvm.loop.disable_nonforced")};
        MDNode *nonforcedMetaNode = MDNode::get(*m_context, nonforcedMeta);
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, nonforcedMetaNode));
      }
      if (m_disableLicm) {
        MDNode *licmDisableNode = MDNode::get(*m_context, MDString::get(*m_context, "llvm.licm.disable"));
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, licmDisableNode));
      }
      loopMetaNode->replaceOperandWith(0, loopMetaNode);
      terminator->setMetadata("llvm.loop", loopMetaNode);
      changed = true;
    }
  }

  return changed;
}

// =====================================================================================================================
// Initializes the pass for patching Loop metadata.
INITIALIZE_PASS(PatchLoopMetadata, DEBUG_TYPE, "Set or amend metadata to control loop unrolling", false, false)
