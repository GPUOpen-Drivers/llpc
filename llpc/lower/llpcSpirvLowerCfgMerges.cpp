/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerCfgMerges.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerCfgMerges.
 * @details This pass process loop merge operations defined in SPIRV.
 *          It manipulates loops with subgroup operations to ensure the set of lanes leaving a loop through merges,
 *          breaks and returns are maximal reconvergent subsets.
 * @todo There a number of possible improvements for this transform:
 *       - Loops that have uniform exit condition do not need to be transformed.
 *       - Only convergent operations on loop break/return paths need to be considered.
 *       - Blocks on the loop-break path after a convergent operation can be pulled out the loop.
 ***********************************************************************************************************************
 */
#include "llpcSpirvLowerCfgMerges.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcSpirvLower.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-spirv-lower-cfg-merges"

// Silence cppcheck for LLVM_DEBUG by defining macro.
// Note: cppcheck-suppress does not reliably work for this.
#ifndef LLVM_DEBUG
#define LLVM_DEBUG(...)
#endif

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

// -enable-loop-reconvergence: force enable loop reconvergence transform
static cl::opt<bool> EnableLoopReconvergence("enable-loop-reconvergence",
                                             cl::desc("Force enable loop reconvergence transform"), cl::init(false));

namespace Llpc {

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerCfgMerges::run(Module &module, ModuleAnalysisManager &analysisManager) {
  bool changed = runImpl(module);
  // In practice there are unlikely to be any analyses this early, but report accurate status anyway.
  return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

/// Defines helper for print block and function identifiers during debugging
class OpPrinter {
  BasicBlock *m_block;
  Function *m_func;

public:
  OpPrinter(BasicBlock *block) : m_block(block), m_func(nullptr) {}
  OpPrinter(Function *F) : m_block(nullptr), m_func(F) {}

  void print(raw_ostream &OS) const {
    if (m_block) {
      m_block->printAsOperand(OS);
      OS << " (" << m_block << ")";
    }
    if (m_func) {
      m_func->printAsOperand(OS);
      OS << " (" << m_func << ")";
    }
  }
};

raw_ostream &operator<<(raw_ostream &OS, const OpPrinter &V) {
  V.print(OS);
  return OS;
}

/// Represent loop and hold references to its associated blocks and PHI nodes
struct LoopDesc {
  LoopDesc()
      : loopHeader(nullptr), mergeBlock(nullptr), continueBlock(nullptr), backedgeBlock(nullptr), sigmaBlock(nullptr),
        returnPhi(nullptr), returnValuePhi(nullptr), parent(nullptr), function(nullptr), depth(-1),
        convergentOps(false){};

  int computeDepth() {
    if (depth < 0) {
      if (parent)
        depth = parent->computeDepth() + 1;
      else
        depth = 0;
    }
    return depth;
  }

  BasicBlock *loopHeader;
  BasicBlock *mergeBlock;
  BasicBlock *continueBlock;
  BasicBlock *backedgeBlock;
  BasicBlock *sigmaBlock;
  PHINode *returnPhi;
  PHINode *returnValuePhi;
  LoopDesc *parent;
  Function *function;
  int depth;
  bool convergentOps;
  SmallVector<BasicBlock *, 2> returnBlocks;
};

// =====================================================================================================================
// Allocate and setup a LoopDesc structure for a given loop
//
// @param [in] mergeInst : loop merge instruction
// @param [in] loopHeader : loop header block
// @param [in] loopHeader : parent loop (maybe null)
// @param [in] loopDescriptors : loop descriptor storage
// @returns : Pointer to new LoopDesc
static LoopDesc *allocateLoop(CallInst *mergeInst, BasicBlock *loopHeader, LoopDesc *parent,
                              SmallVector<std::unique_ptr<LoopDesc>> &loopDescriptors) {
  // Merge instruction should have two users: continue and merge.
  // However continueBlock can be unreachable in CFG and hence missing after inlining.
  BasicBlock *continueBlock = nullptr;
  BasicBlock *mergeBlock = nullptr;
  for (auto user : mergeInst->users()) {
    CallInst *callInst = dyn_cast<CallInst>(user);
    if (!callInst)
      continue;
    auto callee = callInst->getCalledFunction();
    if (!callee)
      continue;
    if (callee->getName() == "spirv.loop.continue.block") {
      assert(!continueBlock);
      continueBlock = callInst->getParent();
    } else if (callee->getName() == "spirv.loop.merge.block") {
      assert(!mergeBlock);
      mergeBlock = callInst->getParent();
    } else {
      llvm_unreachable("Should never be called!");
    }
  }
  if (!mergeBlock) {
    LLVM_DEBUG(dbgs() << "abort loop convergence; loop missing merge block\n");
    return nullptr;
  }

  loopDescriptors.push_back(std::make_unique<LoopDesc>());
  LoopDesc *loop = loopDescriptors.back().get();

  LLVM_DEBUG(dbgs() << "allocate loop " << loop << " for " << OpPrinter(loopHeader) << "\n");

  loop->loopHeader = loopHeader;
  loop->mergeBlock = mergeBlock;
  loop->continueBlock = continueBlock;
  loop->function = loopHeader->getParent();
  loop->parent = parent;
  loop->computeDepth();

  return loop;
}

// =====================================================================================================================
// Iterate through all loop blocks marking them and locating backedge.
// Recursively maps nested loops.
//
// @param [in/out] loop : loop to be mapped
// @param [in/out] loopBlocks : map of blocks to loops
// @param [in] loopMergeInsts : map of blocks to loop merge instructions
// @param [in/out] loopDescriptors : storage for loop descriptors (in nested loops)
// @param [in] convergentValues : set of blocks and functions with convergent calls
// @returns : true if mapping was successful
bool mapLoop(LoopDesc *loop, DenseMap<BasicBlock *, LoopDesc *> &loopBlocks,
             DenseMap<BasicBlock *, CallInst *> &loopMergeInsts,
             SmallVector<std::unique_ptr<LoopDesc>> &loopDescriptors, const DenseSet<Value *> &convergentValues) {
  LLVM_DEBUG(dbgs() << "mapping loop " << loop << ", parent: " << loop->parent << "\n");

  loopBlocks[loop->loopHeader] = loop;

  SmallSet<BasicBlock *, 16> visited;
  SmallVector<BasicBlock *> worklist;

  worklist.push_back(loop->loopHeader);
  while (!worklist.empty()) {
    BasicBlock *block = worklist.pop_back_val();

    LLVM_DEBUG(dbgs() << "Visit: " << OpPrinter(block) << " for " << loop << "\n");

    if (!visited.insert(block).second)
      continue;

    // TODO: we actually only need to care about convergent operations on break/return path.
    // However this is complex as we have to consider nested loop operations on same path.
    if (!loop->convergentOps)
      loop->convergentOps = convergentValues.count(block) > 0;

    unsigned int successorCount = 0;
    for (BasicBlock *succ : successors(block)) {
      successorCount++;
      if (succ == loop->loopHeader) {
        // Backedge block
        if (loop->backedgeBlock) {
          LLVM_DEBUG(dbgs() << "abort loop convergence; loop with more than one backedge detected\n");
          return false;
        }
        loop->backedgeBlock = block;
      } else if (succ == loop->mergeBlock) {
        // End of this loop
      } else if (loopMergeInsts.count(succ)) {
        // Nested loop
        if (loopBlocks.count(succ))
          continue; // Avoid marking loop twice if there are multiple edges to same block
        CallInst *mergeInst = loopMergeInsts[succ];
        LoopDesc *nestedLoop = allocateLoop(mergeInst, succ, loop, loopDescriptors);
        if (!nestedLoop)
          return false;
        if (!mapLoop(nestedLoop, loopBlocks, loopMergeInsts, loopDescriptors, convergentValues))
          return false;
        // Critically, merge block of nested loop is part of this loop.
        // Add this to work list here as all paths to it may be dominated
        // by the nested loop.
        loopBlocks[nestedLoop->mergeBlock] = loop;
        loop->convergentOps = loop->convergentOps || nestedLoop->convergentOps;
        worklist.push_back(nestedLoop->mergeBlock);
      } else {
        loopBlocks[succ] = loop;
        worklist.push_back(succ);
      }
    }
    if (successorCount == 0) {
      // Return block / unreachable block
      Instruction *termInst = block->getTerminator();
      if (isa<ReturnInst>(termInst))
        loop->returnBlocks.push_back(block);
    }
  }

  return true;
}

// =====================================================================================================================
// Determine all functions and block with a convergent function call.
//
// @param [in/out] module : LLVM module to be run on
void SpirvLowerCfgMerges::mapConvergentValues(Module &module) {
  // Map convergent exposure for blocks and functions

  SmallVector<Function *> worklist;
  SmallSet<Function *, 8> visited;

  // Initial worklist is all convergent functions
  for (Function &func : module) {
    if (func.isConvergent())
      worklist.push_back(&func);
  }

  while (!worklist.empty()) {
    Function *func = worklist.pop_back_val();
    if (visited.count(func))
      continue;
    if (func->getName().startswith("spirv.loop."))
      continue;

    // Record each convergent call block and function
    for (User *user : func->users()) {
      Instruction *userInst = dyn_cast<Instruction>(user);
      if (!userInst)
        continue;

      BasicBlock *userBlock = userInst->getParent();
      Function *userFunc = userBlock->getParent();

      m_convergentValues.insert(userBlock);
      m_convergentValues.insert(userFunc);

      // If a function calls a convergent function, consider it convergent
      if (!visited.count(userFunc))
        worklist.push_back(userFunc);
    }
  }
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
bool SpirvLowerCfgMerges::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-CfgMerges\n");
  LLVM_DEBUG(dbgs() << "Processing module: " << module);

  SpirvLower::init(&module);

  // Check for loops
  Function *loopMergeFunc = module.getFunction("spirv.loop.merge");
  if (!loopMergeFunc)
    return false;

  // Map convergent values
  m_convergentValues.clear();
  mapConvergentValues(module);

  // Map loop merges
  SmallVector<std::unique_ptr<LoopDesc>> loopDescriptors;
  DenseMap<BasicBlock *, LoopDesc *> loopBlocks;
  DenseMap<BasicBlock *, CallInst *> loopMergeInsts;

  for (User *user : loopMergeFunc->users()) {
    CallInst *loopMerge = cast<CallInst>(user);
    assert(loopMergeInsts.count(loopMerge->getParent()) == 0);
    loopMergeInsts[loopMerge->getParent()] = loopMerge;
  }

  // Iterate IR to find loops
  // Note: this visit blocks deterministically and loop headers from outer loops before inner ones
  bool hasConvergentLoops = false;
  bool changed = false;
  bool valid = EnableLoopReconvergence && !m_convergentValues.empty();

  for (Function &F : module) {
    if (F.empty())
      continue;
    for (BasicBlock *block : depth_first(&F)) {
      if (!loopMergeInsts.count(block))
        continue;

      CallInst *mergeInst = loopMergeInsts[block];
      if (valid && !loopBlocks.count(block)) {
        LoopDesc *loop = allocateLoop(mergeInst, block, nullptr, loopDescriptors);
        if (loop) {
          valid = valid && mapLoop(loop, loopBlocks, loopMergeInsts, loopDescriptors, m_convergentValues);
          hasConvergentLoops = hasConvergentLoops || loop->convergentOps;
        } else {
          valid = false;
        }
      }

      for (User *loopUser : make_early_inc_range(mergeInst->users())) {
        Instruction *loopInst = cast<Instruction>(loopUser);
        LLVM_DEBUG(dbgs() << "remove: " << *loopInst << "\n");
        loopInst->eraseFromParent();
      }
      LLVM_DEBUG(dbgs() << "remove: " << *mergeInst << "\n");
      mergeInst->eraseFromParent();

      changed = true;
    }
  }

  if (!changed || !valid || !hasConvergentLoops) {
    m_convergentValues.clear();
    return changed;
  }

  // Output debug information before changing IR structure
  LLVM_DEBUG(dbgs() << "Found " << loopDescriptors.size() << " loop(s)\n");
  LLVM_DEBUG(for (auto &loop
                  : loopDescriptors) {
    dbgs() << "loop " << loop.get() << " in " << OpPrinter(loop->function) << "\n";
    dbgs() << "  header: " << OpPrinter(loop->loopHeader) << "\n";
    dbgs() << "  merge: " << OpPrinter(loop->mergeBlock) << "\n";
    dbgs() << "  continue: " << OpPrinter(loop->continueBlock) << "\n";
    dbgs() << "  backedge: " << OpPrinter(loop->backedgeBlock) << "\n";
    dbgs() << "  depth: " << loop->depth << "\n";
    dbgs() << "  convergentOps: " << loop->convergentOps << "\n";
    if (loop->parent)
      dbgs() << "  parent: " << loop->parent << "\n";
    if (!loop->returnBlocks.empty()) {
      dbgs() << "  returns:";
      for (BasicBlock *returnBlock : loop->returnBlocks)
        dbgs() << " " << OpPrinter(returnBlock);
      dbgs() << "\n";
    }
  });

  // Setup sigma blocks and loop depths
  int maxDepth = 0;
  for (auto &loop : loopDescriptors) {
    if (!loop->convergentOps)
      continue;
    loop->sigmaBlock = BasicBlock::Create(*m_context, "", loop->function, loop->mergeBlock);
    maxDepth = std::max(maxDepth, loop->depth);
  }
  LLVM_DEBUG(dbgs() << "Max loop depth is " << maxDepth << "\n");

  // Process loops, starting with the innermost and working outward
  for (int depth = maxDepth; depth >= 0; --depth) {
    LLVM_DEBUG(dbgs() << "Processing depth: " << depth << "\n");

    for (auto &loop : loopDescriptors) {
      if (loop->depth != depth || !loop->convergentOps)
        continue;

      LLVM_DEBUG(dbgs() << "Processing: " << loop.get() << "\n");

      SmallVector<BasicBlock *, 8> sigmaPreds(pred_begin(loop->sigmaBlock), pred_end(loop->sigmaBlock));
      SmallSetVector<BasicBlock *, 8> mergePreds(pred_begin(loop->mergeBlock), pred_end(loop->mergeBlock));

      // If we did not find the backedge block then it implies it is unreachable in the CFG.
      // Substitute the continueBlock (which might also be unreachable).
      // Lack of a backedge generally suggests this loop is not really a loop,
      // but it is possible that an unreachable block might contribute to phis in reachable blocks.
      if (!loop->backedgeBlock)
        loop->backedgeBlock = loop->continueBlock;
      if (loop->backedgeBlock && !mergePreds.contains(loop->backedgeBlock))
        mergePreds.insert(loop->backedgeBlock);

      // Rewrite loop exits
      // +1 edge from waveHeader to sigmaBlock
      PHINode *breakPhi =
          PHINode::Create(m_builder->getInt1Ty(), sigmaPreds.size() + mergePreds.size() + 1, "", loop->sigmaBlock);

      // Process return blocks
      // Nested loop returns will already be routed to sigma block
      if (loop->returnBlocks.size() > 0 || !sigmaPreds.empty()) {
        unsigned int edgeCount = mergePreds.size() + sigmaPreds.size() + loop->returnBlocks.size();

        loop->returnPhi = PHINode::Create(m_builder->getInt1Ty(), edgeCount, "", loop->sigmaBlock);
        if (loop->function->getReturnType() != m_builder->getVoidTy())
          loop->returnValuePhi = PHINode::Create(loop->function->getReturnType(), edgeCount, "", loop->sigmaBlock);

        // Reroute all returns for this loop level
        for (BasicBlock *block : loop->returnBlocks) {
          ReturnInst *returnInst = static_cast<ReturnInst *>(block->getTerminator());

          loop->returnPhi->addIncoming(m_builder->getTrue(), block);
          if (loop->returnValuePhi)
            loop->returnValuePhi->addIncoming(returnInst->getReturnValue(), block);

          returnInst->dropAllReferences();
          returnInst->eraseFromParent();
          BranchInst::Create(loop->sigmaBlock, block);

          breakPhi->addIncoming(m_builder->getTrue(), block);
        }

        // Connect returns from nested loops
        for (BasicBlock *block : sigmaPreds) {
          LoopDesc *otherLoop = loopBlocks[block];
          assert(otherLoop != loop.get());
          loop->returnPhi->addIncoming(m_builder->getTrue(), block);
          if (loop->returnValuePhi) {
            assert(otherLoop->returnValuePhi);
            loop->returnValuePhi->addIncoming(otherLoop->returnValuePhi, block);
          }
        }
      }

      // Reroute all exits via sigma block
      for (BasicBlock *block : mergePreds) {
        BranchInst *termInst = cast<BranchInst>(block->getTerminator());
        assert(termInst);
        // Note: the only edge that is not a break here is from backedge/continue block
        if (termInst->isUnconditional()) {
          bool isBreak = (termInst->getSuccessor(0) == loop->mergeBlock);
          termInst->eraseFromParent();
          breakPhi->addIncoming(m_builder->getInt1(isBreak), block);
          BranchInst::Create(loop->sigmaBlock, block);
        } else {
          Value *condition = termInst->getCondition();
          BasicBlock *ifTrue = termInst->getSuccessor(0);
          BasicBlock *ifFalse = termInst->getSuccessor(1);
          if (ifTrue != loop->loopHeader && ifFalse != loop->loopHeader) {
            // FIXME: can the other target be the mergeBlock?
            // If so, then we'll need to split this edge.
            // Cover this with assertions for now.
            if (ifTrue == loop->mergeBlock) {
              assert(ifFalse != loop->mergeBlock);
              termInst->setSuccessor(0, loop->sigmaBlock);
            } else {
              assert(ifTrue != loop->mergeBlock);
              assert(ifFalse == loop->mergeBlock);
              termInst->setSuccessor(1, loop->sigmaBlock);
            }
            breakPhi->addIncoming(m_builder->getTrue(), block);
          } else {
            termInst->eraseFromParent();
            if (ifTrue == loop->mergeBlock) {
              breakPhi->addIncoming(condition, block);
            } else {
              auto notCondition = BinaryOperator::CreateNot(condition, "", block);
              breakPhi->addIncoming(notCondition, block);
            }
            BranchInst::Create(loop->sigmaBlock, block);
          }
        }
        if (loop->returnPhi) {
          loop->returnPhi->addIncoming(m_builder->getFalse(), block);
          if (loop->returnValuePhi)
            loop->returnValuePhi->addIncoming(PoisonValue::get(loop->function->getReturnType()), block);
        }
      }

      // Add return edges to break
      for (BasicBlock *block : sigmaPreds)
        breakPhi->addIncoming(m_builder->getTrue(), block);

      // If there are returns then we need another block after sigma
      BasicBlock *postSigmaBlock = loop->mergeBlock;
      if (loop->returnPhi) {
        postSigmaBlock = BasicBlock::Create(*m_context, "", loop->function, loop->mergeBlock);
        if (loop->depth == 0) {
          // Root level loops branch to dedicate return blocks
          BasicBlock *returnBlock = BasicBlock::Create(*m_context, "", loop->function, loop->mergeBlock);
          ReturnInst::Create(*m_context, loop->returnValuePhi ? loop->returnValuePhi : nullptr, returnBlock);
          BranchInst::Create(returnBlock, loop->mergeBlock, loop->returnPhi, postSigmaBlock);
        } else {
          // Inner loops move to outer loop sigma
          BranchInst::Create(loop->parent->sigmaBlock, loop->mergeBlock, loop->returnPhi, postSigmaBlock);
        }
      }

      // Define wave header
      BasicBlock *waveHeader = loop->loopHeader;

      // Store loop predecessors before modifying CFG
      SmallVector<BasicBlock *, 2> wavePreds(pred_begin(waveHeader), pred_end(waveHeader));
      MDNode *loopMetadata = loop->loopHeader->getTerminator()->getMetadata("llvm.loop");

      // Split lane header and wave header
      BasicBlock *laneHeader = waveHeader->splitBasicBlock(waveHeader->getFirstInsertionPt());
      laneHeader->getTerminator()->setMetadata("llvm.loop", loopMetadata);

      // Fix up PHIs in wave header
      for (PHINode &headerPhi : waveHeader->phis()) {
        LLVM_DEBUG(dbgs() << "fix up phi: " << headerPhi << "\n");

        PHINode *sigmaPhi =
            PHINode::Create(headerPhi.getType(), breakPhi->getNumIncomingValues(), "", loop->sigmaBlock);
        Value *poison = PoisonValue::get(headerPhi.getType());

        int backedgeIndex = loop->backedgeBlock ? headerPhi.getBasicBlockIndex(loop->backedgeBlock) : -1;
        assert(!loop->backedgeBlock || backedgeIndex >= 0);

        for (BasicBlock *block : predecessors(loop->sigmaBlock)) {
          assert(block != waveHeader);
          if (block == loop->backedgeBlock) {
            assert(backedgeIndex >= 0);
            sigmaPhi->addIncoming(headerPhi.getIncomingValue(backedgeIndex), block);
          } else {
            sigmaPhi->addIncoming(poison, block);
          }
        }
        // Account for edge from wave header to sigma block
        sigmaPhi->addIncoming(&headerPhi, waveHeader);

        // Update header phi to use sigma value
        headerPhi.setIncomingBlock(backedgeIndex, loop->sigmaBlock);
        headerPhi.setIncomingValue(backedgeIndex, sigmaPhi);
      }

      // Note: phis() requires terminator to function, so cannot be removed until here
      waveHeader->getTerminator()->eraseFromParent();

      // Determine if any lanes continue
      Value *notBreakPhi = BinaryOperator::CreateNot(breakPhi, "", loop->sigmaBlock);
      m_builder->SetInsertPoint(loop->sigmaBlock);
      Value *anyContinue = m_builder->CreateSubgroupAny(notBreakPhi);

      // Connect sigma block to wave header
      BranchInst *loopEnd = BranchInst::Create(waveHeader, postSigmaBlock, anyContinue, loop->sigmaBlock);
      loopEnd->setMetadata("llvm.loop", loopMetadata);

      // Setup wave exit status in wave header
      PHINode *waveExitPhi = PHINode::Create(m_builder->getInt1Ty(), wavePreds.size() + 1, "", waveHeader);
      for (BasicBlock *block : wavePreds)
        waveExitPhi->addIncoming(m_builder->getFalse(), block);
      waveExitPhi->addIncoming(breakPhi, loop->sigmaBlock);

      // Setup wave return status in wave header
      PHINode *waveReturnPhi = nullptr;
      if (loop->returnPhi) {
        waveReturnPhi = PHINode::Create(m_builder->getInt1Ty(), wavePreds.size() + 1, "", waveHeader);
        for (BasicBlock *block : wavePreds)
          waveReturnPhi->addIncoming(m_builder->getFalse(), block);
        waveReturnPhi->addIncoming(loop->returnPhi, loop->sigmaBlock);
      }

      // This is horrible hack to avoid SimplifyCFG from threading through wave header
      // and rotating the loop:
      // - Add an assembly call generated value (0) to wave header,
      //   then use this value with similar assembly in the sigma block.
      // This will not generate any instructions in the final shader.
      Type *const int32Type = m_builder->getInt32Ty();
      FunctionType *const idFuncType = FunctionType::get(int32Type, int32Type, false);
      InlineAsm *const idFuncAsm = InlineAsm::get(idFuncType, "; %1", "=v,0", true);

      m_builder->SetInsertPoint(waveHeader);
      Value *headerValue = m_builder->CreateCall(idFuncAsm, m_builder->getInt32(0));
      m_builder->SetInsertPoint(loop->sigmaBlock->getFirstNonPHIOrDbg());
      m_builder->CreateCall(idFuncAsm, headerValue);

      // Add wave header branch based on exit phi (new loop entry)
      BranchInst *loopEntry = BranchInst::Create(loop->sigmaBlock, laneHeader, waveExitPhi, waveHeader);
      loopEntry->setMetadata("llvm.loop", loopMetadata);

      // Add wave header to break and return phis
      breakPhi->addIncoming(m_builder->getTrue(), waveHeader);
      if (waveReturnPhi)
        loop->returnPhi->addIncoming(waveReturnPhi, waveHeader);

      // Move PHIs in merge block to sigma block
      Instruction *firstSigmaInst = &*loop->sigmaBlock->getFirstInsertionPt();
      for (PHINode &mergePhi : make_early_inc_range(loop->mergeBlock->phis())) {
        LLVM_DEBUG(dbgs() << "move phi: " << mergePhi << "\n");
        mergePhi.moveBefore(firstSigmaInst);

        // Add any missing predecessor references
        Value *poison = PoisonValue::get(mergePhi.getType());
        for (BasicBlock *block : predecessors(loop->sigmaBlock)) {
          // FIXME: use poison here?
          if (mergePhi.getBasicBlockIndex(block) == -1)
            mergePhi.addIncoming(poison, block);
        }
      }
    }
  }

  m_convergentValues.clear();
  return true;
}

} // namespace Llpc
