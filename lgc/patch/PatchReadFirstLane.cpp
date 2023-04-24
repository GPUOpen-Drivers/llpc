/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchReadFirstLane.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchReadFirstLane.
 ***********************************************************************************************************************
 */
#include "lgc/patch/PatchReadFirstLane.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineState.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
// Old version of the code
#include "llvm/Analysis/DivergenceAnalysis.h"
#else
// New version of the code (also handles unknown version, which we treat as latest)
#include "llvm/Analysis/UniformityAnalysis.h"
#endif
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include <deque>

#define DEBUG_TYPE "lgc-patch-read-first-lane"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Returns true if all users of the given instruction defined in the given block.
//
// @param inst : The given instruction
// @param block : The given block
static bool isAllUsersDefinedInBlock(Instruction *inst, BasicBlock *block) {
  for (auto user : inst->users()) {
    if (auto userInst = dyn_cast<Instruction>(user))
      if (userInst->getParent() != block)
        return false;
  }
  return true;
}

// =====================================================================================================================
// Returns true if all users of the given instruction are already readfirstlane
//
// @param inst : The given instruction
static bool areAllUserReadFirstLane(Instruction *inst) {
  for (auto user : inst->users()) {
    if (isa<DbgInfoIntrinsic>(user))
      continue;
    auto intrinsic = dyn_cast<IntrinsicInst>(user);
    if (!intrinsic || intrinsic->getIntrinsicID() != Intrinsic::amdgcn_readfirstlane)
      return false;
  }
  return true;
}

// =====================================================================================================================
PatchReadFirstLane::PatchReadFirstLane() : m_targetTransformInfo(nullptr) {
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in/out] function : Function that we will peephole optimize.
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses PatchReadFirstLane::run(Function &function, FunctionAnalysisManager &analysisManager) {
  TargetTransformInfo &targetTransformInfo = analysisManager.getResult<TargetIRAnalysis>(function);

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 458033
  // Old version of the code
  DivergenceInfo &uniformityInfo = analysisManager.getResult<DivergenceAnalysis>(function);
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  UniformityInfo &uniformityInfo = analysisManager.getResult<UniformityInfoAnalysis>(function);
#endif
  auto isDivergentUse = [&](const Use &use) { return uniformityInfo.isDivergentUse(use); };
  if (runImpl(function, isDivergentUse, &targetTransformInfo))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in,out] function : LLVM function to be run on.
// @param isDivergentUse : Function returning true if the given use is divergent.
// @param targetTransformInfo : The TTI to use for this pass.
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchReadFirstLane::runImpl(Function &function, std::function<bool(const Use &)> isDivergentUse,
                                 TargetTransformInfo *targetTransformInfo) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Read-First-Lane\n");

  m_isDivergentUse = std::move(isDivergentUse);
  m_targetTransformInfo = targetTransformInfo;

  bool changed = promoteEqualUniformOps(function);
  changed |= liftReadFirstLane(function);
  return changed;
}

// =====================================================================================================================
// Use the uniform (non-divergent) node instead of a divergent one if they are equal.
//
// Detect a case where a branch condition tests two nodes for equality where one of them
// is divergent and the other uniform. In the true block replace the divergent
// use with the uniform one as it helps instruction selection decide what to put in an SGPR.
//
// For example, in the code snippet below in BB1, %divergent can be replaced with %uniform.
//
// BB0:
// %uniform = call i32 @llvm.amdgcn.readlane(i32 %divergent, i32 %lane)
// %cmp = icmp eq i32 %divergent, %uniform
// br i1 %cmp, label %BB1, label %BB2
//
// BB1:
// ..
// %use = zext i32 %divergent to i64 ; can use %uniform
//
// @param [in,out] function : LLVM function to be run for the optimization.
bool PatchReadFirstLane::promoteEqualUniformOps(Function &function) {

  bool changed = false;
  for (auto &bb : function) {
    // Look for a block that has a single predecessor with conditional branch.
    const auto *pred = bb.getSinglePredecessor();
    if (!pred)
      continue;
    const auto *branchInst = dyn_cast<BranchInst>(pred->getTerminator());
    if (!branchInst || !branchInst->isConditional())
      continue;
    assert(branchInst->getSuccessor(0) != branchInst->getSuccessor(1));

    // Make sure the condition for entering bb is the equality test passing.
    bool match = false;
    const auto *cmpInst = dyn_cast<ICmpInst>(branchInst->getCondition());
    if (cmpInst) {
      switch (cmpInst->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        match = branchInst->getSuccessor(0) == &bb;
        break;
      case ICmpInst::ICMP_NE:
        match = branchInst->getSuccessor(1) == &bb;
        break;
      default:
        break;
      }
    }
    if (!match)
      continue;

    // Check if the condition compares a divergent and uniform value.
    Value *divergentVal = nullptr, *uniformVal = nullptr;
    for (int i = 0; i < 2; ++i) {
      const Use &cmpArg = cmpInst->getOperandUse(i);
      if (m_isDivergentUse(cmpArg))
        divergentVal = cmpArg.get();
      else
        uniformVal = cmpArg.get();
    }
    if (!divergentVal || !uniformVal)
      continue;

    // Replace all occurrences of the divergent value with uniform one in bb.
    SmallVector<User *, 2> users;
    for (auto *user : divergentVal->users()) {
      if (auto *uInst = dyn_cast<Instruction>(user)) {
        if (uInst->getParent() == &bb) {
          users.push_back(user);
        }
      }
    }

    for (auto *user : users)
      user->replaceUsesOfWith(divergentVal, uniformVal);

    changed |= !users.empty();
  }
  return changed;
}

// =====================================================================================================================
// Lift readfirstlanes in relevant basic blocks to transform VGPR instructions into SGPR instructions as many as
// possible.
//
// @param [in,out] function : LLVM function to be run for readfirstlane optimization.
bool PatchReadFirstLane::liftReadFirstLane(Function &function) {
  // Collect the basic blocks with amdgcn_readfirstlane
  // Build the map between initial readfirstlanes and their corresponding blocks
  DenseMap<BasicBlock *, SmallVector<Instruction *, 2>> blockInitialReadFirstLanesMap;
  Module *module = function.getParent();
  for (auto &func : *module) {
    if (func.getIntrinsicID() == Intrinsic::amdgcn_readfirstlane) {
      for (User *user : func.users()) {
        Instruction *inst = cast<Instruction>(user);
        if (inst->getFunction() != &function)
          continue;
        blockInitialReadFirstLanesMap[inst->getParent()].push_back(inst);
      }
      break;
    }
  }
  bool changed = false;

  // Lift readfirstlanes in each relevant basic block
  for (const auto &blockInitialReadFirstLanes : blockInitialReadFirstLanesMap) {
    BasicBlock *curBb = blockInitialReadFirstLanes.first;

    // Step 1: Collect all instructions that "can be assumed uniform" with its divergent uses in a map
    // (m_uniformDivergentUsesMap)
    collectAssumeUniforms(curBb, blockInitialReadFirstLanes.second);

    // Step 2: Determine the best places to insert readfirstlane according to a heuristic
    findBestInsertLocation(blockInitialReadFirstLanes.second);

    // Step 3: Apply readFirstLane on all determined locations
    assert(m_insertLocations.size() <= blockInitialReadFirstLanes.second.size());
    BuilderBase builder(curBb->getContext());
    for (auto inst : m_insertLocations) {
      // Avoid to insert redundant readfirstlane
      if (auto intrinsic = dyn_cast<IntrinsicInst>(inst))
        if (intrinsic->getIntrinsicID() == Intrinsic::amdgcn_readfirstlane)
          continue;
      if (areAllUserReadFirstLane(inst))
        continue;

      applyReadFirstLane(inst, builder);
      changed = true;
    }

    m_uniformDivergentUsesMap.clear();
  }
  return changed;
}

// =====================================================================================================================
// The decision of whether an instruction should be added to the m_canAssumeUniformDivergentUseMap is only made once all
// later instructions in the basic block have been processed. To avoid scanning all instructions excessively, we
// maintain a basic-block-ordered queue of candidates. An instruction is only added as a candidate if it appears as a
// relevant operand to an instruction that is already in the map.
//
// @param block : The processing basic block
// @param initialReadFirstLanes : The initial amdgcn_readfirstlane vector
void PatchReadFirstLane::collectAssumeUniforms(BasicBlock *block,
                                               const SmallVectorImpl<Instruction *> &initialReadFirstLanes) {
  auto instructionOrder = [](Instruction *lhs, Instruction *rhs) { return lhs->comesBefore(rhs); };
  SmallVector<Instruction *, 16> candidates;

  auto insertCandidate = [&](Instruction *candidate) {
    auto insertPos = llvm::lower_bound(candidates, candidate, instructionOrder);
    if (insertPos == candidates.end() || *insertPos != candidate)
      candidates.insert(insertPos, candidate);
  };

  // The given instruction can be assumed to have a uniform result, i.e., replacing its uses by a use of a
  // readfirstlane of it would be correct. This helper function:
  //  1. Records this fact and
  //  2. Determines whether the assumption of a uniform result could be propagated to the candidate's operands.
  auto tryPropagate = [&](Instruction *candidate, bool isInitialReadFirstLane) {
    bool cannotPropagate = m_targetTransformInfo->isSourceOfDivergence(candidate) || isa<PHINode>(candidate) ||
                           (!isInitialReadFirstLane && isa<CallInst>(candidate));

    SmallVector<Instruction *, 3> operandInsts;
    if (!cannotPropagate) {
      for (Use &use : candidate->operands()) {
        if (!m_isDivergentUse(use))
          continue; // already known to be uniform -- no need to consider this operand

        auto operandInst = dyn_cast<Instruction>(use.get());
        if (!operandInst) {
          // Known to be divergent, but not an instruction. Further propagation is currently not implemented.
          assert(isa<Argument>(use.get()));
          cannotPropagate = true;
          break;
        }

        if (operandInst->getParent() != block || !isAllUsersDefinedInBlock(operandInst, block)) {
          // Further propagation is currently not implemented. Theoretically, we could insert a readfirstlane
          // instruction dedicated for users in this basic block, but it's not clear whether that would be a win.
          cannotPropagate = true;
          break;
        }

        operandInsts.push_back(operandInst);
      }

      if (cannotPropagate)
        operandInsts.clear();
    }

    for (Instruction *operandInst : operandInsts)
      insertCandidate(operandInst);

    assert(m_uniformDivergentUsesMap.count(candidate) == 0);
    m_uniformDivergentUsesMap.try_emplace(candidate, std::move(operandInsts));
  };

  for (auto readfirstlane : initialReadFirstLanes)
    tryPropagate(readfirstlane, true);

  while (!candidates.empty()) {
    Instruction *candidate = candidates.pop_back_val();

    if (isAllUsersAssumedUniform(candidate))
      tryPropagate(candidate, false);
  }
}

// =====================================================================================================================
// Find the best insert locations according to the heuristic.
// The heuristic is: if an instruction that can be assumed to be uniform has multiple divergent operands, then you take
// the definition of the divergent operand that is earliest in basic block order (call it "the earliest divergent
// operand") and propagate up to that instruction; if it turns out that instruction can be assumed to be uniform,
// then we can just insert the readfirstlane there (or propagate).
//
// @param readFirstLaneCount : The initial amdgcn_readfirstlane vector
void PatchReadFirstLane::findBestInsertLocation(const SmallVectorImpl<Instruction *> &initialReadFirstLanes) {
  // Set of instructions from m_uniformDivergentUsesMap which will be forced to become uniform by the
  // instructions we already plan to insert so far. Allows us to break out of searches that would be redundant.
  DenseSet<Instruction *> enforcedUniform;
  SmallVector<Instruction *, 8> enforcedUniformTracker;

  m_insertLocations.clear();

  for (auto &initialReadFirstLane : initialReadFirstLanes) {
    // Find a best insert location for a lifted readfirstlane to obsolete the existing, initial readfirstlane.
    // Conceptually, we trace backwards through the induced data dependency graph (or "cone") of
    // divergent-but-can-assume-uniform instructions feeding into the initialReadFirstLane.
    // Each iteration of the middle loop jumps to the next "bottleneck" in this DAG, that is, `current` always points
    // at a bottleneck where we could insert a single readfirstlane (depending on the type).
    Instruction *bestInsertLocation = nullptr;
    unsigned bestInsertLocationDepth = 0;

    Instruction *current = initialReadFirstLane;

    for (;;) {
      const auto &mapIt = m_uniformDivergentUsesMap.find(current);
      if (mapIt == m_uniformDivergentUsesMap.end())
        break; // no further propagation possible

      const auto &divergentOperands = mapIt->second;
      if (divergentOperands.empty())
        break; // no further propagation possible

      if (divergentOperands.size() == 1) {
        // There is only a single operand, we can jump to it directly.
        current = divergentOperands[0];
      } else {
        // There are multiple operands. Since we don't want to increase the number of readfirstlanes, try to find
        // an earlier bottleneck in the data dependency graph.
        //
        // The search proceeds backwards by instruction order in the basic block, maintaining a sorted queue of
        // instructions that remain to be explored. We use two heuristics to limit the cost of the search:
        //  - We never explore beyond the earliest operand of `current`.
        //  - We limit both the depth and the breadth (i.e., maximum queue size) of the search.
        //
        // We maintain the queue as a vector because it will always be short, and inserting into a short sorted
        // vector is very fast.
        constexpr unsigned MaxSearchBreadth = 4;
        constexpr unsigned MaxSearchDepth = 10;
        auto instructionOrder = [](Instruction *lhs, Instruction *rhs) { return lhs->comesBefore(rhs); };

        if (divergentOperands.size() > MaxSearchBreadth)
          break;

        SmallVector<Instruction *, MaxSearchBreadth> queue;
        queue.insert(queue.begin(), divergentOperands.begin(), divergentOperands.end());
        llvm::sort(queue, instructionOrder);

        bool searchAborted = false;
        unsigned depth = 0;
        do {
          Instruction *candidate = queue.back();
          if (enforcedUniform.count(candidate)) {
            // Candidate is already enforced to be uniform by a previous decision to insert a readfirstlane.
            // We can just skip it.
            queue.pop_back();
            continue;
          }
          const auto &mapIt = m_uniformDivergentUsesMap.find(candidate);
          if (mapIt == m_uniformDivergentUsesMap.end())
            break; // no further propagation possible, need to abort the search
          const auto &candidateOperands = mapIt->second;
          if (candidateOperands.empty())
            break; // no further propagation possible, need to abort the search
          queue.pop_back();

          enforcedUniformTracker.push_back(candidate);

          // Add the operands to the queue if they aren't already contained in it.
          for (Instruction *operand : candidateOperands) {
            auto insertPos = llvm::lower_bound(queue, operand, instructionOrder);
            if (insertPos == queue.end() || *insertPos != operand) {
              // Abort if the search becomes too "wide" or moves beyond the earliest operand of `current`.
              if (queue.size() >= MaxSearchBreadth || insertPos == queue.begin()) {
                searchAborted = true;
                break;
              }
              queue.insert(insertPos, operand);
            }
          }

          if (++depth > MaxSearchDepth)
            break;
        } while (queue.size() >= 2 && !searchAborted);

        if (queue.size() >= 2)
          break; // didn't find a next bottleneck in the data dependency graph, bail out

        current = queue[0]; // move to the found bottleneck
      }

      if (enforcedUniform.count(current)) {
        // Already enforced to be uniform, no need to continue the search or even consider inserting a new
        // readfirstlane.
        bestInsertLocation = nullptr;
        break;
      }

      enforcedUniformTracker.push_back(current);

      if (isSupportedType(current)) {
        bestInsertLocation = current;
        bestInsertLocationDepth = enforcedUniformTracker.size();
      }
    }

    // Record the best (read: earliest) bottleneck that we were able to find in the graph/
    if (bestInsertLocation) {
      m_insertLocations.insert(bestInsertLocation);

      for (unsigned idx = 0; idx < bestInsertLocationDepth; ++idx)
        enforcedUniform.insert(enforcedUniformTracker[idx]);
    }

    enforcedUniformTracker.clear();
  }
}

// =====================================================================================================================
// Return true if all users of the given instruction are "assumed uniform"
//
// @param inst : The instruction to be checked
bool PatchReadFirstLane::isAllUsersAssumedUniform(Instruction *inst) {
  for (auto user : inst->users()) {
    auto userInst = dyn_cast<Instruction>(user);
    if (m_uniformDivergentUsesMap.count(userInst) == 0)
      return false;
  }
  return true;
}

// =====================================================================================================================
// Try to apply readfirstlane on the given instruction
//
// @param inst : The instruction to be applied readfirstlane on
// @param builder : BuildBase to use
void PatchReadFirstLane::applyReadFirstLane(Instruction *inst, BuilderBase &builder) {
  // Guarantee the insert position is behind all PhiNodes
  Instruction *insertPos = inst->getNextNonDebugInstruction();
  while (isa<PHINode>(insertPos))
    insertPos = insertPos->getNextNonDebugInstruction();
  builder.SetInsertPoint(insertPos);

  Type *instTy = inst->getType();
  const bool isFloat = instTy->isFloatTy();
  assert(isFloat || instTy->isIntegerTy(32));
  Value *newInst = inst;
  if (isFloat)
    newInst = builder.CreateBitCast(inst, builder.getInt32Ty());

  Value *readFirstLane = builder.CreateIntrinsic(Intrinsic::amdgcn_readfirstlane, {}, newInst);

  Value *replaceInst = nullptr;
  if (isFloat) {
    replaceInst = builder.CreateBitCast(readFirstLane, instTy);
  } else {
    newInst = readFirstLane;
    replaceInst = readFirstLane;
  }
  inst->replaceUsesWithIf(replaceInst, [newInst](Use &U) { return U.getUser() != newInst; });
}
