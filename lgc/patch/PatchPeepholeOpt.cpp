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
 * @file  PatchPeepholeOpt.cpp
 * @brief LLPC source file: contains implementation of class lgc::PatchPeepholeOpt.
 ***********************************************************************************************************************
 */
#include "PatchPeepholeOpt.h"
#include "lgc/patch/Patch.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "llpc-patch-peephole-opt"

using namespace lgc;
using namespace llvm;

namespace llvm {

namespace cl {
// -enable-discard-opt: Enable the optimization for "kill" intrinsic.
static opt<bool> EnableDiscardOpt("enable-discard-opt", desc("Enable the optimization for \"kill\" intrinsic."),
                                  init(false));
} // namespace cl

} // namespace llvm

namespace lgc {

// =====================================================================================================================
// Define static members (no initializer needed as LLVM only cares about the address of ID, never its value).
char PatchPeepholeOpt::ID;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for peephole optimizations.
//
// @param enableDiscardOpt : Enable the optimization for "kill" intrinsic
FunctionPass *createPatchPeepholeOpt(bool enableDiscardOpt) {
  return new PatchPeepholeOpt(enableDiscardOpt);
}

// =====================================================================================================================
//
// @param enableDiscardOpt : Enable the optimization for "kill" intrinsic
PatchPeepholeOpt::PatchPeepholeOpt(bool enableDiscardOpt) : FunctionPass(ID) {
  m_enableDiscardOpt = enableDiscardOpt;
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
//
// @param [in,out] function : Function that we will peephole optimize.
bool PatchPeepholeOpt::runOnFunction(Function &function) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Peephole-Opt\n");

  visit(function);

  const bool changed = !m_instsToErase.empty();

  for (Instruction *const inst : m_instsToErase) {
    // Lastly delete any instructions we replaced.
    inst->eraseFromParent();
  }
  m_instsToErase.clear();

  return changed;
}

// =====================================================================================================================
// Specify what analysis passes this pass depends on.
//
// @param [in,out] analysisUsage : The place to record our analysis pass usage requirements.
void PatchPeepholeOpt::getAnalysisUsage(AnalysisUsage &analysisUsage) const {
  analysisUsage.setPreservesCFG();
}

// =====================================================================================================================
// Visit a bit cast instruction.
//
// @param bitCast : The "bitcast" instruction to visit.
void PatchPeepholeOpt::visitBitCast(BitCastInst &bitCast) {
  // If the bit cast has no users, no point trying to optimize it!
  if (bitCast.user_empty())
    return;

  // First run through the thing we are bit casting and see if there are multiple bit casts we can combine.
  unsigned numCombinableUsers = 0;

  for (User *const user : bitCast.getOperand(0)->users()) {
    BitCastInst *const otherBitCast = dyn_cast<BitCastInst>(user);

    // If we don't have a bit cast, skip.
    if (!otherBitCast)
      continue;

    // If the other bit cast has no users, no point optimizating it.
    if (otherBitCast->user_empty())
      continue;

    // If the other bit cast doesn't match our type, skip.
    if (otherBitCast->getDestTy() != bitCast.getDestTy())
      continue;

    numCombinableUsers++;

    // If we have at least two bit casts we can combine, skip checking and just do the optimization.
    if (numCombinableUsers > 1)
      break;
  }

  // If we have at least 2 users, optimize!
  if (numCombinableUsers > 1) {
    if (Instruction *const inst = dyn_cast<Instruction>(bitCast.getOperand(0))) {
      moveAfter(bitCast, *inst);

      // Replace all extract element instructions that extract with the same value and index.
      for (User *const user : bitCast.getOperand(0)->users()) {
        // Skip ourselves.
        if (user == &bitCast)
          continue;

        BitCastInst *const otherBitCast = dyn_cast<BitCastInst>(user);

        // If we don't have a bit cast, skip.
        if (!otherBitCast)
          continue;

        // If the other bit cast has no users, no point optimizating it.
        if (otherBitCast->user_empty())
          continue;

        // If the other bit cast doesn't match our type, skip.
        if (otherBitCast->getDestTy() != bitCast.getDestTy())
          continue;

        // Replace the other bit cast with our one.
        otherBitCast->replaceAllUsesWith(&bitCast);

        // Lastly remember to delete the bit cast we just replaced.
        m_instsToErase.push_back(otherBitCast);
      }
    }
  }

  // Check if we are bitcasting a shuffle instruction.
  if (ShuffleVectorInst *const shuffleVector = dyn_cast<ShuffleVectorInst>(bitCast.getOperand(0))) {
    // Only check bit casts where the element types match to make porting the shuffle vector more trivial.
    if (bitCast.getSrcTy()->getScalarSizeInBits() != bitCast.getDestTy()->getScalarSizeInBits())
      return;

    // Bit cast the LHS of the original shuffle.
    Value *const shuffleVectorLhs = shuffleVector->getOperand(0);
    Type *const bitCastLhsType = VectorType::get(bitCast.getDestTy()->getVectorElementType(),
                                                 shuffleVectorLhs->getType()->getVectorNumElements());
    BitCastInst *const bitCastLhs =
        new BitCastInst(shuffleVectorLhs, bitCastLhsType, shuffleVectorLhs->getName() + ".bitcast");
    insertAfter(*bitCastLhs, *shuffleVector);

    // Bit cast the RHS of the original shuffle.
    Value *const shuffleVectorRhs = shuffleVector->getOperand(1);
    Type *const bitCastRhsType = VectorType::get(bitCast.getDestTy()->getVectorElementType(),
                                                 shuffleVectorRhs->getType()->getVectorNumElements());
    BitCastInst *const bitCastRhs =
        new BitCastInst(shuffleVectorRhs, bitCastRhsType, shuffleVectorRhs->getName() + ".bitcast");
    insertAfter(*bitCastRhs, *bitCastLhs);

    // Create our new shuffle instruction.

    // TODO: Simplify by using pShuffleVector->getShuffleMask() (no arguments) when it becomes available.
    SmallVector<int, 8> maskVals;
    shuffleVector->getShuffleMask(maskVals);

    auto int32Type = Type::getInt32Ty(shuffleVector->getContext());
    SmallVector<Constant *, 8> masks;
    for (unsigned i = 0; i < maskVals.size(); ++i) {
      if (maskVals[i] == -1)
        masks.push_back(UndefValue::get(int32Type));
      else
        masks.push_back(ConstantInt::get(int32Type, maskVals[i]));
    }
    ShuffleVectorInst *const newShuffleVector =
        new ShuffleVectorInst(bitCastLhs, bitCastRhs, ConstantVector::get(masks), shuffleVector->getName());
    newShuffleVector->insertAfter(&bitCast);

    // Replace the bit cast with the new shuffle vector.
    bitCast.replaceAllUsesWith(newShuffleVector);

    // Lastly remember to delete the bit cast we just replaced.
    m_instsToErase.push_back(&bitCast);

    // Visit the bit cast instructions we just inserted in case there are optimization opportunities.
    visitBitCast(*bitCastLhs);
    visitBitCast(*bitCastRhs);

    return;
  }

  if (PHINode *const phiNode = dyn_cast<PHINode>(bitCast.getOperand(0))) {
    // We only want to push bitcasts where the PHI node is an i8, as it'll save us PHI nodes later.
    if (phiNode->getType()->getScalarSizeInBits() != 8)
      return;

    // Push the bit cast to each of the PHI's incoming values instead.
    const unsigned numIncomings = phiNode->getNumIncomingValues();

    PHINode *const newPhiNode = PHINode::Create(bitCast.getDestTy(), numIncomings, phiNode->getName(), phiNode);

    // Loop through each incoming edge to the PHI node.
    for (unsigned incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++) {
      Value *const incoming = phiNode->getIncomingValue(incomingIndex);

      BasicBlock *const basicBlock = phiNode->getIncomingBlock(incomingIndex);

      if (Instruction *const inst = dyn_cast<Instruction>(incoming)) {
        BitCastInst *const newBitCast = new BitCastInst(inst, bitCast.getDestTy());

        insertAfter(*newBitCast, *inst);

        newPhiNode->addIncoming(newBitCast, basicBlock);
      } else if (Constant *const constant = dyn_cast<Constant>(incoming)) {
        Constant *const newBitCast = ConstantExpr::getBitCast(constant, bitCast.getDestTy());

        newPhiNode->addIncoming(newBitCast, basicBlock);
      } else
        llvm_unreachable("Should never be called!");
    }

    // Replace the bit cast with the new PHI node.
    bitCast.replaceAllUsesWith(newPhiNode);

    // Lastly remember to delete the bit cast we just replaced.
    m_instsToErase.push_back(&bitCast);

    // If the PHI node that we've just replaced had any other users, make a bit cast for them.
    if (!phiNode->hasOneUse()) {
      BitCastInst *const newBitCast = new BitCastInst(newPhiNode, phiNode->getType());

      insertAfter(*newBitCast, *newPhiNode);

      phiNode->replaceAllUsesWith(newBitCast);

      // Visit the bit cast instructions we just inserted in case there are optimization opportunities.
      visitBitCast(*newBitCast);
    }

    // Lastly remember to delete the PHI node we just replaced.
    m_instsToErase.push_back(phiNode);

    return;
  }
}

// =====================================================================================================================
// Visit an integer comparison instruction.
//
// @param iCmp : The "icmp" instruction to visit.
void PatchPeepholeOpt::visitICmp(ICmpInst &iCmp) {
  switch (iCmp.getPredicate()) {
  case CmpInst::ICMP_UGT:
    break;
  default:
    return;
  }

  ConstantInt *const constantVal = dyn_cast<ConstantInt>(iCmp.getOperand(1));

  // If we don't have a constant we are comparing against, or the constant is the maximum representable, bail.
  if (!constantVal || constantVal->isMaxValue(false))
    return;

  const uint64_t constant = constantVal->getZExtValue();

  ConstantInt *const newConstant = ConstantInt::get(constantVal->getType(), constant + 1, false);

  // Swap the predicate to less than. This helps the loop analysis passes detect more loops that can be trivially
  // unrolled.
  iCmp.setPredicate(CmpInst::ICMP_ULT);

  // Set our new constant to the second operand.
  iCmp.setOperand(1, newConstant);

  // Run through the users of the icmp and if they are branches, switch the branch conditions, otherwise make a not
  // of the icmp and replace the use with the not.

  SmallVector<Instruction *, 4> instsWithOpsToReplace;

  for (User *const user : iCmp.users()) {
    if (BranchInst *const branch = dyn_cast<BranchInst>(user)) {
      // Only conditional branches could use an integer comparison instruction, so we just swap the operands.
      branch->swapSuccessors();
    } else if (Instruction *const inst = dyn_cast<Instruction>(user))
      instsWithOpsToReplace.push_back(inst);
  }

  // If we have no other instructions we need to deal with, bail.
  if (instsWithOpsToReplace.empty())
    return;

  Instruction *const iCmpNot = BinaryOperator::CreateNot(&iCmp);
  insertAfter(*iCmpNot, iCmp);

  for (Instruction *const inst : instsWithOpsToReplace) {
    const unsigned numOperands = inst->getNumOperands();

    for (unsigned operandIndex = 0; operandIndex < numOperands; operandIndex++) {
      if (&iCmp == inst->getOperand(operandIndex))
        inst->setOperand(operandIndex, iCmpNot);
    }
  }
}

// =====================================================================================================================
// Visit an extract element instruction.
//
// @param extractElement : The "extractelement" instruction to visit.
void PatchPeepholeOpt::visitExtractElement(ExtractElementInst &extractElement) {
  // If the extract has no users, no point trying to optimize it!
  if (extractElement.user_empty())
    return;

  Value *const vector = extractElement.getVectorOperand();

  ConstantInt *const indexVal = dyn_cast<ConstantInt>(extractElement.getIndexOperand());

  // We only handle constant indices.
  if (!indexVal)
    return;

  const uint64_t index = indexVal->getZExtValue();

  // Check if the extract is coming from an insert element, and try and track the extract back to see if there is an
  // insert we can forward onto the result of the extract.

  Value *nextVector = vector;

  while (InsertElementInst *const nextInsertElement = dyn_cast<InsertElementInst>(nextVector)) {
    ConstantInt *const nextIndex = dyn_cast<ConstantInt>(nextInsertElement->getOperand(2));

    // If the vector was inserting at a non-constant index, bail.
    if (!nextIndex)
      break;

    // If the index of the insertion matches the index we were extracting, forward the insert!
    if (nextIndex->equalsInt(index)) {
      extractElement.replaceAllUsesWith(nextInsertElement->getOperand(1));

      // Lastly remember to delete the extract we just replaced.
      m_instsToErase.push_back(&extractElement);

      return;
    }

    // Otherwise do another loop iteration and check the vector the insert element was inserting into.
    nextVector = nextInsertElement->getOperand(0);
  }

  unsigned numCombinableUsers = 0;

  for (User *const user : vector->users()) {
    ExtractElementInst *const otherExtractElement = dyn_cast<ExtractElementInst>(user);

    // If we don't have an extract element, skip.
    if (!otherExtractElement)
      continue;

    // If the other extract has no users, no point optimizating it.
    if (otherExtractElement->user_empty())
      continue;

    ConstantInt *const otherIndex = dyn_cast<ConstantInt>(otherExtractElement->getIndexOperand());

    // If the other index is not a constant integer, skip.
    if (!otherIndex)
      continue;

    // If the indices do not match, skip.
    if (!otherIndex->equalsInt(index))
      continue;

    numCombinableUsers++;

    // If we have at least two extracts we can combine, skip checking and just do the optimization.
    if (numCombinableUsers > 1)
      break;
  }

  // If we have at least 2 users, optimize!
  if (numCombinableUsers > 1) {
    if (Instruction *const inst = dyn_cast<Instruction>(vector)) {
      ExtractElementInst *const newExtractElement =
          ExtractElementInst::Create(vector, indexVal, extractElement.getName());

      insertAfter(*newExtractElement, *inst);

      // Replace all extract element instructions that extract with the same value and index.
      for (User *const user : vector->users()) {
        ExtractElementInst *const otherExtractElement = dyn_cast<ExtractElementInst>(user);

        // If we don't have an extract element, skip.
        if (!otherExtractElement)
          continue;

        // If the other extract has no users, no point optimizating it.
        if (otherExtractElement->user_empty())
          continue;

        // If the extract element is the new one we just inserted, skip.
        if (newExtractElement == otherExtractElement)
          continue;

        ConstantInt *const otherIndex = dyn_cast<ConstantInt>(otherExtractElement->getIndexOperand());

        // If the other index is not a constant integer, skip.
        if (!otherIndex)
          continue;

        // If the indices do not match, skip.
        if (!otherIndex->equalsInt(index))
          continue;

        // Replace the other extraction with our new one.
        otherExtractElement->replaceAllUsesWith(newExtractElement);

        // Lastly remember to delete the extract we just replaced.
        m_instsToErase.push_back(otherExtractElement);
      }

      return;
    }
  }
}

// =====================================================================================================================
// Visit a PHI node.
//
// @param phiNode : The PHI node to visit.
void PatchPeepholeOpt::visitPHINode(PHINode &phiNode) {
  // If the PHI has no users, no point trying to optimize it!
  if (phiNode.user_empty())
    return;

  const unsigned numIncomings = phiNode.getNumIncomingValues();

  // Only care about vector PHI nodes whose element size is at least 32 bits.
  if (phiNode.getType()->isVectorTy() && phiNode.getType()->getScalarSizeInBits() >= 32) {
    // The integer type we'll use for our extract & insert elements.
    IntegerType *const int32Type = IntegerType::get(phiNode.getContext(), 32);

    // Where we will insert our vector create that will replace the uses of the PHI node.
    Instruction *const insertPos = phiNode.getParent()->getFirstNonPHI();

    // The type of the vector.
    Type *const type = phiNode.getType();

    // The number of elements in the vector type (which will result in N new scalar PHI nodes).
    const unsigned numElements = type->getVectorNumElements();

    // The element type of the vector.
    Type *const elementType = type->getVectorElementType();

    Value *result = UndefValue::get(type);

    // Loop through each element of the vector.
    for (unsigned elementIndex = 0; elementIndex < numElements; elementIndex++) {
      // We create a new name that is "old name".N, where N is the index of element into the original vector.
      ConstantInt *const elementIndexVal = ConstantInt::get(int32Type, elementIndex, false);

      PHINode *const newPhiNode =
          PHINode::Create(elementType, numIncomings, phiNode.getName() + "." + Twine(elementIndex));
      insertAfter(*newPhiNode, phiNode);

      result = InsertElementInst::Create(result, newPhiNode, elementIndexVal, "", insertPos);

      // Make sure the same incoming blocks have identical incoming values.
      // If we have already inserted an incoming arc for a basic block,
      // reuse the same value in the future incoming arcs from the same block.
      SmallDenseMap<BasicBlock *, Value *, 8> incomingPairMap;
      incomingPairMap.reserve(numIncomings);

      // Loop through each incoming edge to the PHI node.
      for (unsigned incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++) {
        Value *const incoming = phiNode.getIncomingValue(incomingIndex);

        BasicBlock *const basicBlock = phiNode.getIncomingBlock(incomingIndex);

        if (Instruction *const inst = dyn_cast<Instruction>(incoming)) {
          Value *newIncomingValue = nullptr;
          auto it = incomingPairMap.find(basicBlock);
          if (it != incomingPairMap.end())
            newIncomingValue = it->second;
          else {
            ExtractElementInst *const extractElement = ExtractElementInst::Create(incoming, elementIndexVal);

            insertAfter(*extractElement, *inst);
            newIncomingValue = extractElement;
            incomingPairMap.insert({basicBlock, newIncomingValue});
          }

          newPhiNode->addIncoming(newIncomingValue, basicBlock);
        } else if (Constant *const constant = dyn_cast<Constant>(incoming)) {
          Constant *const extractElement = ConstantExpr::getExtractElement(constant, elementIndexVal);
          incomingPairMap.insert({basicBlock, extractElement});
          newPhiNode->addIncoming(extractElement, basicBlock);
        } else
          llvm_unreachable("Should never be called!");
      }
    }

    // Replace all the users of the original PHI node with the new nodes combined using insertions.
    phiNode.replaceAllUsesWith(result);

    // Lastly remember the phi so we can delete it later when it is safe to do so.
    m_instsToErase.push_back(&phiNode);

    return;
  }

  // Optimize PHI nodes that have incoming values that are identical in their parent blocks.
  Instruction *prevIncomingInst = nullptr;

  for (unsigned incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++) {
    Instruction *const incomingInst = dyn_cast<Instruction>(phiNode.getIncomingValue(incomingIndex));

    // If we don't have an instruction, bail.
    if (!incomingInst) {
      prevIncomingInst = nullptr;
      break;
    }

    // If our incoming instruction is a PHI node, we can't move it so bail.
    if (isa<PHINode>(incomingInst)) {
      prevIncomingInst = nullptr;
      break;
    }

    // If we don't have a previous instruction to compare against, store the current one and continue.
    if (!prevIncomingInst) {
      prevIncomingInst = incomingInst;
      continue;
    }

    if (!incomingInst->isIdenticalTo(prevIncomingInst)) {
      prevIncomingInst = nullptr;
      break;
    }
  }

  // Do not clone allocas -- we don't want to potentially introduce them in the middle of the function.
  if (prevIncomingInst && !isa<AllocaInst>(prevIncomingInst)) {
    Instruction *const newInst = prevIncomingInst->clone();
    insertAfter(*newInst, phiNode);

    // Replace all the users of the original PHI node with the incoming value that each incoming references.
    phiNode.replaceAllUsesWith(newInst);

    // Lastly remember the phi so we can delete it later when it is safe to do so.
    m_instsToErase.push_back(&phiNode);

    return;
  }

  if (numIncomings == 3) {
    // Optimize PHI nodes like:
    //   %p = phi [%a, %foo], [%b, %bar], [%b, %yar]
    //   %b = phi [%c, %har], [%p, %fiz]
    // Where we have multiple PHI nodes similar to %p, that take an %a, but are actually passing the same value
    // back and forth (and thus can be collapsed into a single PHI node).
    PHINode *const subPhiNode = dyn_cast<PHINode>(phiNode.getIncomingValue(1));

    if (subPhiNode == phiNode.getIncomingValue(2)) {
      bool subPhiNodeOptimizable = true;

      const unsigned numSubIncomings = subPhiNode->getNumIncomingValues();

      for (unsigned subIncomingIndex = 0; subIncomingIndex < numSubIncomings; subIncomingIndex++) {
        Value *const incoming = subPhiNode->getIncomingValue(subIncomingIndex);

        // We can attempt to optimize the sub PHI node if an incoming is our parent PHI node, or if the
        // incoming is a constant.
        if (incoming != &phiNode && !isa<Constant>(incoming)) {
          subPhiNodeOptimizable = false;
          break;
        }
      }

      // If the sub PHI node was optimizable, lets try and optimize!
      if (subPhiNodeOptimizable) {
        for (User *const user : phiNode.getIncomingValue(0)->users()) {
          PHINode *const otherPhiNode = dyn_cast<PHINode>(user);

          // If its not a PHI node, skip.
          if (!otherPhiNode)
            continue;

          // Skip our PHI node in the user list.
          if (otherPhiNode == &phiNode)
            continue;

          // If both PHI nodes are not in the same parent block, skip.
          if (otherPhiNode->getParent() != phiNode.getParent())
            continue;

          // If the PHI does not match the number of incomings as us, skip.
          if (otherPhiNode->getNumIncomingValues() != numIncomings)
            continue;

          PHINode *const otherSubPhiNode = dyn_cast<PHINode>(otherPhiNode->getIncomingValue(1));

          // If the other incomings don't match, its not like our PHI node, skip.
          if (otherSubPhiNode != otherPhiNode->getIncomingValue(2))
            continue;

          // If both sub PHI nodes are not in the same parent block, skip.
          if (otherSubPhiNode->getParent() != subPhiNode->getParent())
            continue;

          // If the sub PHI nodes don't have the same incomings, we can't fold them so we skip.
          if (otherSubPhiNode->getNumIncomingValues() != numSubIncomings)
            continue;

          for (unsigned subIncomingIndex = 0; subIncomingIndex < numSubIncomings; subIncomingIndex++) {
            if (subPhiNode->getIncomingBlock(subIncomingIndex) != otherSubPhiNode->getIncomingBlock(subIncomingIndex)) {
              subPhiNodeOptimizable = false;
              break;
            }

            Value *const incoming = subPhiNode->getIncomingValue(subIncomingIndex);
            Value *const otherIncoming = otherSubPhiNode->getIncomingValue(subIncomingIndex);

            if (otherIncoming != otherPhiNode && otherIncoming != incoming) {
              subPhiNodeOptimizable = false;
              break;
            }
          }

          if (subPhiNodeOptimizable) {
            // Both our PHI's are actually identical! Optimize away.
            otherPhiNode->replaceAllUsesWith(&phiNode);
            otherSubPhiNode->replaceAllUsesWith(subPhiNode);

            // Lastly remember the phis so we can delete them later when it is safe to do so.
            m_instsToErase.push_back(otherPhiNode);
            m_instsToErase.push_back(otherSubPhiNode);
          }
        }
      }
    }
  }
}

// =====================================================================================================================
// Visits "call" instruction.
//
// @param callInst : "Call" instruction
void PatchPeepholeOpt::visitCallInst(CallInst &callInst) {
  auto callee = callInst.getCalledFunction();
  if (!callee)
    return;

  // Optimization for call @llvm.amdgcn.kill(). Pattern:
  //   %29 = fcmp olt float %28, 0.000000e+00
  //   br i1 % 29, label %30, label %31
  // 30:; preds = %.entry
  //   call void @llvm.amdgcn.kill(i1 false)
  //   br label %73
  //
  // Move the kill call outside and remove the kill call block
  //   %29 = fcmp olt float %28, 0.000000e+00
  //   %nonkill = xor i1 %29, true
  //   call void @llvm.amdgcn.kill(i1 %nonkill)
  //   br i1 false, label %30, label %31
  // 30:; preds = %.entry
  //   call void @llvm.amdgcn.kill(i1 false)
  //   br label %73
  if (cl::EnableDiscardOpt && m_enableDiscardOpt && callee->getIntrinsicID() == Intrinsic::amdgcn_kill) {
    auto block = callInst.getParent();
    if (block->size() > 2) {
      // Apply the optimization to blocks that contains single kill call instruction.
      return;
    }

    for (BasicBlock *predBlock : predecessors(block)) {
      auto terminator = predBlock->getTerminator();
      BranchInst *branch = dyn_cast<BranchInst>(terminator);
      if (branch && branch->isConditional()) {
        auto cond = branch->getCondition();
        auto trueBlock = dyn_cast<BasicBlock>(branch->getSuccessor(0));
        auto newKill = dyn_cast<CallInst>(callInst.clone());
        llvm::LLVMContext *context = &callInst.getContext();

        if (trueBlock == block) {
          // the kill block is the true condition block
          // insert a bitwise not instruction.
          auto notCond = BinaryOperator::CreateNot(cond, "", terminator);
          newKill->setArgOperand(0, notCond);
          // Make the kill block unreachable
          branch->setCondition(ConstantInt::get(Type::getInt1Ty(*context), false));
        } else {
          newKill->setArgOperand(0, cond);
          // make the kill block unreachable
          branch->setCondition(ConstantInt::get(Type::getInt1Ty(*context), true));
        }
        newKill->insertBefore(terminator);
      }
    }
  }
}

// =====================================================================================================================
// Helper function to move an instruction after another.
//
// @param move : Instruction to move.
// @param after : Where to move after.
void PatchPeepholeOpt::moveAfter(Instruction &move, Instruction &after) const {
  // Special case for if the instruction is a PHI node, we need to move after all other PHIs.
  if (isa<PHINode>(&after))
    move.moveBefore(after.getParent()->getFirstNonPHI());
  else
    move.moveAfter(&after);
}

// =====================================================================================================================
// Helper function to insert an instruction after another.
//
// @param insert : Instruction to insert.
// @param after : Where to insert after.
void PatchPeepholeOpt::insertAfter(Instruction &insert, Instruction &after) const {
  // Special case for if the instruction is a PHI node, we need to insert after all other PHIs.
  if (isa<PHINode>(&after))
    insert.insertBefore(after.getParent()->getFirstNonPHI());
  else
    insert.insertAfter(&after);
}

} // namespace lgc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for peephole optimizations.
INITIALIZE_PASS(PatchPeepholeOpt, DEBUG_TYPE, "Patch LLVM for peephole optimizations", false, false)
