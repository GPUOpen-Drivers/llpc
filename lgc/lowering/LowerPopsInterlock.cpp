/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerPopsInterlock.cpp
 * @brief LGC source file: contains implementation of class lgc::LowerPopsInterlock.
 ***********************************************************************************************************************
 */
#include "LowerPopsInterlock.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/BuilderBase.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Analysis/CycleAnalysis.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-lower-pops-interlock"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] func : LLVM function to be run on
// @param [in/out] funcAnalysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerPopsInterlock::run(Function &func, FunctionAnalysisManager &funcAnalysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-Pops-Interlock\n");

  // Not fragment shader, skip
  if (getShaderStage(&func) != ShaderStageEnum::Fragment)
    return PreservedAnalyses::all();

  auto &moduleAnalysisManager = funcAnalysisManager.getResult<ModuleAnalysisManagerFunctionProxy>(func);
  m_pipelineState = moduleAnalysisManager.getCachedResult<PipelineStateWrapper>(*func.getParent())->getPipelineState();
  m_entryPoint = &func;

  m_builder.reset(new BuilderBase(m_pipelineState->getContext()));

  legalizeInterlock(funcAnalysisManager);
  lowerInterlock();

  return m_changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

// =====================================================================================================================
// Legalize POPS interlock operations.
//
// In this function, we try to collect all begin_interlock end_interlock operations and merge them to only one pair.
// Although GLSL spec says the two operations must be in main function without any control flow, we use them to support
// DX raster-order-view (ROV) feature. In such case, if we have multiple ROVs, each ROV can have a pair of
// begin/end_interlock to gate them and such pairs may be in conditional path of control flow. Our strategy to find the
// first use of ROVs and insert begin_interlock before it. If the insert block is in a cycle, we try to search up its
// ancestors until we find an appropriate insert point. Likewise, we insert end_interlock after the last use of ROVs.
// We search down descendants of the insert block if it is in a cycle. It is required by HW that the pair of
// begin/end_interlock can only be executed once for each wave.
//
// @funcAnalysisManager : Analysis manager to use for this transformation
void LowerPopsInterlock::legalizeInterlock(FunctionAnalysisManager &funcAnalysisManager) {
  //
  // Collect all begin_interlock and end_interlock operations for further analysis.
  //
  static auto visitor = llvm_dialects::VisitorBuilder<LowerPopsInterlock>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add(&LowerPopsInterlock::collectBeginInterlock)
                            .add(&LowerPopsInterlock::collectEndInterlock)
                            .build();
  visitor.visit(*this, *m_entryPoint);

  // Skip further processing if there are no POPS interlock operations
  if (m_beginInterlocks.empty() && m_endInterlocks.empty())
    return;

  auto &domTree = funcAnalysisManager.getResult<DominatorTreeAnalysis>(*m_entryPoint);
  auto &postDomTree = funcAnalysisManager.getResult<PostDominatorTreeAnalysis>(*m_entryPoint);
  auto &cycleInfo = funcAnalysisManager.getResult<CycleAnalysis>(*m_entryPoint);

  //
  // Legalize begin_interlock by doing two steps:
  //   1. Find the closest common dominator of all begin_interlocks
  //   2. If that is in a cycle, go up the dominator tree until it is not in a cycle.
  //
  assert(!m_beginInterlocks.empty()); // Must have at least one begin_interlock
  auto nearestDom = m_beginInterlocks.front();
  for (unsigned i = 1; i < m_beginInterlocks.size(); ++i)
    nearestDom = domTree.findNearestCommonDominator(nearestDom, m_beginInterlocks[i]);

  if (auto cycle = cycleInfo.getCycle(nearestDom->getParent()))
    nearestDom = cycle->getCyclePredecessor()->getTerminator();

  m_builder->SetInsertPoint(nearestDom);
  m_builder->create<PopsBeginInterlockOp>();

  //
  // Legalize end_interlock by doing two steps:
  //   1. Find the closest common dominator of all end_interlocks
  //   2. If that is in a cycle, go down the dominator tree until it is not in a cycle.
  //
  assert(!m_endInterlocks.empty()); // Must have at least one end_interlock
  auto nearestPostDom = m_endInterlocks.front();
  for (unsigned i = 1; i < m_endInterlocks.size(); ++i) {
    const auto endInterlock = m_endInterlocks[i];
    if (endInterlock->getParent() == nearestPostDom->getParent()) {
      // In the same block, maybe update nearest post dominator
      if (nearestPostDom->comesBefore(endInterlock))
        nearestPostDom = endInterlock;
    } else {
      auto nearestPostDomBlock =
          postDomTree.findNearestCommonDominator(nearestPostDom->getParent(), endInterlock->getParent());
      if (nearestPostDomBlock != nearestPostDom->getParent()) {
        // Block of the nearest post dominator is changed, have to update nearest post dominator
        if (nearestPostDomBlock == endInterlock->getParent()) {
          // In the same block, use current end_interlock as the new nearest post dominator
          nearestPostDom = endInterlock;
        } else {
          nearestPostDom = &*nearestPostDomBlock->getFirstInsertionPt();
        }
      }
    }
  }

  while (auto cycle = cycleInfo.getCycle(nearestPostDom->getParent())) {
    SmallVector<BasicBlock *, 16> succBlocks;
    cycle->getExitBlocks(succBlocks);
    nearestPostDom = &*succBlocks[0]->getFirstInsertionPt();
  };

  m_builder->SetInsertPoint(nearestPostDom);
  m_builder->create<PopsEndInterlockOp>();

  //
  // Clean up
  //
  for (auto beginInterlock : m_beginInterlocks) {
    beginInterlock->dropAllReferences();
    beginInterlock->eraseFromParent();
  }
  m_beginInterlocks.clear();

  for (auto endInterlock : m_endInterlocks) {
    endInterlock->dropAllReferences();
    endInterlock->eraseFromParent();
  }
  m_endInterlocks.clear();

  m_changed = true;
}

// =====================================================================================================================
// Collect begin_interlock operations.
//
// @param popsBeginInterlockOp : Call instruction op to begin a POPS critical section
void LowerPopsInterlock::collectBeginInterlock(PopsBeginInterlockOp &popsBeginInterlockOp) {
  m_beginInterlocks.push_back(&popsBeginInterlockOp);
}

// =====================================================================================================================
// Collect end_interlock operations.
//
// @param popsEndInterlockOp : Call instruction op to end a POPS critical section
void LowerPopsInterlock::collectEndInterlock(PopsEndInterlockOp &popsEndInterlockOp) {
  m_endInterlocks.push_back(&popsEndInterlockOp);
}

// =====================================================================================================================
// Lower POPS interlock operations.
void LowerPopsInterlock::lowerInterlock() {
  static auto visitor = llvm_dialects::VisitorBuilder<LowerPopsInterlock>()
                            .setStrategy(llvm_dialects::VisitorStrategy::ByFunctionDeclaration)
                            .add(&LowerPopsInterlock::lowerBeginInterlock)
                            .add(&LowerPopsInterlock::lowerEndInterlock)
                            .build();
  visitor.visit(*this, *m_entryPoint);

  if (!m_beginInterlocks.empty()) {
    assert(m_beginInterlocks.size() == 1); // Must have only one begin_interlock after legalization
    m_beginInterlocks[0]->dropAllReferences();
    m_beginInterlocks[0]->eraseFromParent();
    m_beginInterlocks.clear();
  }

  if (!m_endInterlocks.empty()) {
    assert(m_endInterlocks.size() == 1); // Must have only one end_interlock after legalization
    m_endInterlocks[0]->dropAllReferences();
    m_endInterlocks[0]->eraseFromParent();
    m_endInterlocks.clear();
  }
}

// =====================================================================================================================
// Lower begin_interlock operation.
//
// @param popsBeginInterlockOp : Call instruction op to begin a POPS critical section
void LowerPopsInterlock::lowerBeginInterlock(PopsBeginInterlockOp &popsBeginInterlockOp) {
  m_beginInterlocks.push_back(&popsBeginInterlockOp);

  m_builder->SetInsertPoint(&popsBeginInterlockOp);

  //
  // The processing is something like this:
  //
  // Pre-GFX11:
  // The layout of collision wave ID is as follow:
  //
  // +------------+-----------+---------------------------+-----------------+
  // | Overlapped | Packer ID | Newest Overlapped Wave ID | Current Wave ID |
  // | [31]       | [29:28]   | [25:16]                   | [9:0]           |
  // +------------+-----------+---------------------------+-----------------+
  //
  //   POPS_BEGIN_INTERLOCK() {
  //     isOverlapped = collisionWaveId[31]
  //     if (isOverlapped) {
  //       packerId = collisionWaveId[29:28]
  //       s_setreg(HW_REG_POPS_PACKER, (packerId << 1) & 0x1))
  //
  //       currentWaveId = collisionWaveId[9:0]
  //       waveIdRemapOffset = -(currentWaveId + 1) = ~currentWaveId
  //
  //       newestOverlappedWaveId = collisionWaveId[25:16]
  //       newestOverlappedWaveId += waveIdRemapOffset
  //
  //       Load srcPopsExitingWaveId
  //       srcPopsExitingWaveId += waveIdRemapOffset
  //       while (srcPopsExitingWaveId <= newestOverlappedWaveId) {
  //         s_sleep(0xFFFF)
  //         Reload srcPopsExitingWaveId
  //         srcPopsExitingWaveId += waveIdRemapOffset
  //       }
  //     }
  //   }
  //
  // GFX11+:
  //   POPS_BEGIN_INTERLOCK() {
  //     s_wait_event(EXPORT_READY)
  //   }
  //
  const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  if (gfxIp.major >= 11) {
    m_builder->CreateIntrinsic(m_builder->getVoidTy(), Intrinsic::amdgcn_s_wait_event_export_ready, {});
    return;
  }

  auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
  auto collisionWaveId = getFunctionArgument(m_entryPoint, entryArgIdxs.collisionWaveId);

  auto checkOverlapBlock = m_builder->GetInsertBlock();
  auto processOverlapBlock = checkOverlapBlock->splitBasicBlock(&popsBeginInterlockOp, ".processOverlap");
  auto waveWaitingHeaderBlock = processOverlapBlock->splitBasicBlock(&popsBeginInterlockOp, ".waveWaitingHeader");
  auto waveWaitingBodyBlock = waveWaitingHeaderBlock->splitBasicBlock(&popsBeginInterlockOp, ".waveWaitingBody");
  auto endProcessOverlapBlock = waveWaitingBodyBlock->splitBasicBlock(&popsBeginInterlockOp, ".endProcessOverlap");

  // Modify ".checkOverlap" block
  {
    m_builder->SetInsertPoint(checkOverlapBlock->getTerminator());

    auto isOverlapped = m_builder->CreateAnd(m_builder->CreateLShr(collisionWaveId, 31), 0x1);
    isOverlapped = m_builder->CreateTrunc(isOverlapped, m_builder->getInt1Ty());
    m_builder->CreateCondBr(isOverlapped, processOverlapBlock, endProcessOverlapBlock);

    checkOverlapBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".processOverlap" block
  Value *waveIdRemapOffset = nullptr;
  Value *newestOverlappedWaveId = nullptr;
  {
    m_builder->SetInsertPoint(processOverlapBlock->getTerminator());

    // POPS_PACKER: [0] Enable; [2:1] Packer ID
    auto packerId = m_builder->CreateAnd(m_builder->CreateLShr(collisionWaveId, 28), 0x3);
    static const unsigned HwRegPopsPacker = 25;
    auto popsPacker = m_builder->CreateOr(m_builder->CreateShl(packerId, 1), 0x1);
    m_builder->CreateSetReg(HwRegPopsPacker, 0, 3, popsPacker);

    // waveIdRemapOffset = -(currentWaveId + 1) = ~currentWaveId
    auto currentWaveId = m_builder->CreateAnd(collisionWaveId, 0x3FF);
    waveIdRemapOffset = m_builder->CreateNot(currentWaveId);

    // newestOverlappedWaveId += waveIdRemapOffset
    newestOverlappedWaveId = m_builder->CreateAnd(m_builder->CreateLShr(collisionWaveId, 16), 0x3FF);
    newestOverlappedWaveId = m_builder->CreateAdd(newestOverlappedWaveId, waveIdRemapOffset);
  }

  // Construct ".waveWaitingHeader" block
  {
    m_builder->SetInsertPoint(waveWaitingHeaderBlock->getTerminator());

    Value *popsExitingWaveId =
        m_builder->CreateIntrinsic(m_builder->getInt32Ty(), Intrinsic::amdgcn_pops_exiting_wave_id, {});
    popsExitingWaveId = m_builder->CreateAdd(popsExitingWaveId, waveIdRemapOffset);

    Value *needToWait = m_builder->CreateICmpULE(popsExitingWaveId, newestOverlappedWaveId);
    m_builder->CreateCondBr(needToWait, waveWaitingBodyBlock, endProcessOverlapBlock);

    waveWaitingHeaderBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Construct ".waveWaitingBody" block
  {
    m_builder->SetInsertPoint(waveWaitingBodyBlock->getTerminator());

    static const unsigned WaitTime = 0xFFFF;
    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_sleep, {}, m_builder->getInt32(WaitTime));

    m_builder->CreateBr(waveWaitingHeaderBlock);

    waveWaitingBodyBlock->getTerminator()->eraseFromParent(); // Remove old terminator
  }

  // Currently, nothing to do to construct ".endProcessOverlap" block

  m_changed = true;
}

// =====================================================================================================================
// Lower end_interlock operation.
//
// @param popsEndInterlockOp : Call instruction op to end a POPS critical section
void LowerPopsInterlock::lowerEndInterlock(PopsEndInterlockOp &popsEndInterlockOp) {
  m_endInterlocks.push_back(&popsEndInterlockOp);

  m_builder->SetInsertPoint(&popsEndInterlockOp);

  //
  // The processing is something like this:
  //
  // Pre-GFX11:
  //   POPS_END_INTERLOCK() {
  //     s_wait_vscnt null, 0x0
  //     s_sendmsg(MSG_ORDERED_PS_DONE)
  //   }
  //
  // GFX11+:
  //   POPS_END_INTERLOCK() {
  //     s_wait_vscnt null, 0x0
  //   }
  //

  // Add s_wait_vscnt null, 0x0 to make sure the completion of all writes
  SyncScope::ID syncScope = m_builder->getContext().getOrInsertSyncScopeID("agent");
  m_builder->CreateFence(AtomicOrdering::Release, syncScope);

  const auto gfxIp = m_pipelineState->getTargetInfo().getGfxIpVersion();
  if (gfxIp.major < 11) {
    auto &entryArgIdxs = m_pipelineState->getShaderInterfaceData(ShaderStage::Fragment)->entryArgIdxs.fs;
    auto primMask = getFunctionArgument(m_entryPoint, entryArgIdxs.primMask);

    m_builder->CreateIntrinsic(Intrinsic::amdgcn_s_sendmsg, {}, {m_builder->getInt32(OrderedPsDone), primMask});
  }

  m_changed = true;
}

} // namespace lgc
