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
 * @file  llpcPatchPeepholeOpt.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PatchPeepholeOpt.
 ***********************************************************************************************************************
 */
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcPatch.h"
#include "llpcPatchPeepholeOpt.h"

#define DEBUG_TYPE "llpc-patch-peephole-opt"

using namespace Llpc;
using namespace llvm;

namespace llvm
{

namespace cl
{
// -enable-discard-opt: Enable the optimization for "kill" intrinsic.
static opt<bool> EnableDiscardOpt("enable-discard-opt",
                                  desc("Enable the optimization for \"kill\" intrinsic."),
                                  init(false));
} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Define static members (no initializer needed as LLVM only cares about the address of ID, never its value).
char PatchPeepholeOpt::ID;

// =====================================================================================================================
// Pass creator, creates the pass of LLVM patching operations for peephole optimizations.
FunctionPass* CreatePatchPeepholeOpt(
    bool enableDiscardOpt) // Enable the optimization for "kill" intrinsic
{
    return new PatchPeepholeOpt(enableDiscardOpt);
}

// =====================================================================================================================
PatchPeepholeOpt::PatchPeepholeOpt(
    bool enableDiscardOpt) // Enable the optimization for "kill" intrinsic
    :
    FunctionPass(ID)
{
    m_enableDiscardOpt = enableDiscardOpt;
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
bool PatchPeepholeOpt::runOnFunction(
    Function& function // [in,out] Function that we will peephole optimize.
    )
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Peephole-Opt\n");

    visit(function);

    const bool changed = m_instsToErase.empty() == false;

    for (Instruction* const pInst : m_instsToErase)
    {
        // Lastly delete any instructions we replaced.
        pInst->eraseFromParent();
    }
    m_instsToErase.clear();

    return changed;
}

// =====================================================================================================================
// Specify what analysis passes this pass depends on.
void PatchPeepholeOpt::getAnalysisUsage(
    AnalysisUsage& analysisUsage // [in,out] The place to record our analysis pass usage requirements.
    ) const
{
    analysisUsage.setPreservesCFG();
}

// =====================================================================================================================
// Visit a bit cast instruction.
void PatchPeepholeOpt::visitBitCast(
    BitCastInst& bitCast) // [in] The "bitcast" instruction to visit.
{
    // If the bit cast has no users, no point trying to optimize it!
    if (bitCast.user_empty())
    {
        return;
    }

    // First run through the thing we are bit casting and see if there are multiple bit casts we can combine.
    uint32_t numCombinableUsers = 0;

    for (User* const pUser : bitCast.getOperand(0)->users())
    {
        BitCastInst* const pOtherBitCast = dyn_cast<BitCastInst>(pUser);

        // If we don't have a bit cast, skip.
        if (pOtherBitCast == nullptr)
        {
            continue;
        }

        // If the other bit cast has no users, no point optimizating it.
        if (pOtherBitCast->user_empty())
        {
            continue;
        }

        // If the other bit cast doesn't match our type, skip.
        if (pOtherBitCast->getDestTy() != bitCast.getDestTy())
        {
            continue;
        }

        numCombinableUsers++;

        // If we have at least two bit casts we can combine, skip checking and just do the optimization.
        if (numCombinableUsers > 1)
        {
            break;
        }
    }

    // If we have at least 2 users, optimize!
    if (numCombinableUsers > 1)
    {
        if (Instruction* const pInst = dyn_cast<Instruction>(bitCast.getOperand(0)))
        {
            moveAfter(bitCast, *pInst);

            // Replace all extract element instructions that extract with the same value and index.
            for (User* const pUser : bitCast.getOperand(0)->users())
            {
                // Skip ourselves.
                if (pUser == &bitCast)
                {
                    continue;
                }

                BitCastInst* const pOtherBitCast = dyn_cast<BitCastInst>(pUser);

                // If we don't have a bit cast, skip.
                if (pOtherBitCast == nullptr)
                {
                    continue;
                }

                // If the other bit cast has no users, no point optimizating it.
                if (pOtherBitCast->user_empty())
                {
                    continue;
                }

                // If the other bit cast doesn't match our type, skip.
                if (pOtherBitCast->getDestTy() != bitCast.getDestTy())
                {
                    continue;
                }

                // Replace the other bit cast with our one.
                pOtherBitCast->replaceAllUsesWith(&bitCast);

                // Lastly remember to delete the bit cast we just replaced.
                m_instsToErase.push_back(pOtherBitCast);
            }
        }
    }

    // Check if we are bitcasting a shuffle instruction.
    if (ShuffleVectorInst* const pShuffleVector = dyn_cast<ShuffleVectorInst>(bitCast.getOperand(0)))
    {
        // Only check bit casts where the element types match to make porting the shuffle vector more trivial.
        if (bitCast.getSrcTy()->getScalarSizeInBits() != bitCast.getDestTy()->getScalarSizeInBits())
        {
            return;
        }

        // Bit cast the LHS of the original shuffle.
        Value* const pShuffleVectorLhs = pShuffleVector->getOperand(0);
        Type* const pBitCastLhsType = VectorType::get(bitCast.getDestTy()->getVectorElementType(),
            pShuffleVectorLhs->getType()->getVectorNumElements());
        BitCastInst* const pBitCastLhs = new BitCastInst(pShuffleVectorLhs, pBitCastLhsType,
                                                         pShuffleVectorLhs->getName() + ".bitcast");
        insertAfter(*pBitCastLhs, *pShuffleVector);

        // Bit cast the RHS of the original shuffle.
        Value* const pShuffleVectorRhs = pShuffleVector->getOperand(1);
        Type* const pBitCastRhsType = VectorType::get(bitCast.getDestTy()->getVectorElementType(),
            pShuffleVectorRhs->getType()->getVectorNumElements());
        BitCastInst* const pBitCastRhs = new BitCastInst(pShuffleVectorRhs, pBitCastRhsType,
                                                         pShuffleVectorRhs->getName() + ".bitcast");
        insertAfter(*pBitCastRhs, *pBitCastLhs);

        // Create our new shuffle instruction.
        ShuffleVectorInst* const pNewShuffleVector = new ShuffleVectorInst(
            pBitCastLhs, pBitCastRhs, pShuffleVector->getMask(), pShuffleVector->getName());
        pNewShuffleVector->insertAfter(&bitCast);

        // Replace the bit cast with the new shuffle vector.
        bitCast.replaceAllUsesWith(pNewShuffleVector);

        // Lastly remember to delete the bit cast we just replaced.
        m_instsToErase.push_back(&bitCast);

        // Visit the bit cast instructions we just inserted in case there are optimization opportunities.
        visitBitCast(*pBitCastLhs);
        visitBitCast(*pBitCastRhs);

        return;
    }

    if (PHINode* const pPhiNode = dyn_cast<PHINode>(bitCast.getOperand(0)))
    {
        // We only want to push bitcasts where the PHI node is an i8, as it'll save us PHI nodes later.
        if (pPhiNode->getType()->getScalarSizeInBits() != 8)
        {
            return;
        }

        // Push the bit cast to each of the PHI's incoming values instead.
        const uint32_t numIncomings = pPhiNode->getNumIncomingValues();

        PHINode* const pNewPhiNode = PHINode::Create(bitCast.getDestTy(), numIncomings, pPhiNode->getName(), pPhiNode);

        // Loop through each incoming edge to the PHI node.
        for (uint32_t incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++)
        {
            Value* const pIncoming = pPhiNode->getIncomingValue(incomingIndex);

            BasicBlock* const pBasicBlock = pPhiNode->getIncomingBlock(incomingIndex);

            if (Instruction* const pInst = dyn_cast<Instruction>(pIncoming))
            {
                BitCastInst* const pNewBitCast = new BitCastInst(pInst, bitCast.getDestTy());

                insertAfter(*pNewBitCast, *pInst);

                pNewPhiNode->addIncoming(pNewBitCast, pBasicBlock);
            }
            else if (Constant* const pConstant = dyn_cast<Constant>(pIncoming))
            {
                Constant* const pNewBitCast = ConstantExpr::getBitCast(pConstant, bitCast.getDestTy());

                pNewPhiNode->addIncoming(pNewBitCast, pBasicBlock);
            }
            else
            {
                LLPC_NEVER_CALLED();
            }
        }

        // Replace the bit cast with the new PHI node.
        bitCast.replaceAllUsesWith(pNewPhiNode);

        // Lastly remember to delete the bit cast we just replaced.
        m_instsToErase.push_back(&bitCast);

        // If the PHI node that we've just replaced had any other users, make a bit cast for them.
        if (pPhiNode->hasOneUse() == false)
        {
            BitCastInst* const pNewBitCast = new BitCastInst(pNewPhiNode, pPhiNode->getType());

            insertAfter(*pNewBitCast, *pNewPhiNode);

            pPhiNode->replaceAllUsesWith(pNewBitCast);

            // Visit the bit cast instructions we just inserted in case there are optimization opportunities.
            visitBitCast(*pNewBitCast);
        }

        // Lastly remember to delete the PHI node we just replaced.
        m_instsToErase.push_back(pPhiNode);

        return;
    }
}

// =====================================================================================================================
// Visit an integer comparison instruction.
void PatchPeepholeOpt::visitICmp(
    ICmpInst& iCmp) // [in] The "icmp" instruction to visit.
{
    switch (iCmp.getPredicate())
    {
    case CmpInst::ICMP_UGT:
        break;
    default:
        return;
    }

    ConstantInt* const pConstant = dyn_cast<ConstantInt>(iCmp.getOperand(1));

    // If we don't have a constant we are comparing against, or the constant is the maximum representable, bail.
    if ((pConstant == nullptr) || pConstant->isMaxValue(false))
    {
        return;
    }

    const uint64_t constant = pConstant->getZExtValue();

    ConstantInt* const pNewConstant = ConstantInt::get(pConstant->getType(), constant + 1, false);

    // Swap the predicate to less than. This helps the loop analysis passes detect more loops that can be trivially
    // unrolled.
    iCmp.setPredicate(CmpInst::ICMP_ULT);

    // Set our new constant to the second operand.
    iCmp.setOperand(1, pNewConstant);

    // Run through the users of the icmp and if they are branches, switch the branch conditions, otherwise make a not
    // of the icmp and replace the use with the not.

    SmallVector<Instruction*, 4> instsWithOpsToReplace;

    for (User* const pUser : iCmp.users())
    {
        if (BranchInst* const pBranch = dyn_cast<BranchInst>(pUser))
        {
            // Only conditional branches could use an integer comparison instruction, so we just swap the operands.
            pBranch->swapSuccessors();
        }
        else if (Instruction* const pInst = dyn_cast<Instruction>(pUser))
        {
            instsWithOpsToReplace.push_back(pInst);
        }
    }

    // If we have no other instructions we need to deal with, bail.
    if (instsWithOpsToReplace.empty())
    {
        return;
    }

    Instruction* const pICmpNot = BinaryOperator::CreateNot(&iCmp);
    insertAfter(*pICmpNot, iCmp);

    for (Instruction* const pInst : instsWithOpsToReplace)
    {
        const uint32_t numOperands = pInst->getNumOperands();

        for (uint32_t operandIndex = 0; operandIndex < numOperands; operandIndex++)
        {
            if (&iCmp == pInst->getOperand(operandIndex))
            {
                pInst->setOperand(operandIndex, pICmpNot);
            }
        }
    }
}

// =====================================================================================================================
// Visit an extract element instruction.
void PatchPeepholeOpt::visitExtractElement(
    ExtractElementInst& extractElement) // [in] The "extractelement" instruction to visit.
{
    // If the extract has no users, no point trying to optimize it!
    if (extractElement.user_empty())
    {
        return;
    }

    Value* const pVector = extractElement.getVectorOperand();

    ConstantInt* const pIndex = dyn_cast<ConstantInt>(extractElement.getIndexOperand());

    // We only handle constant indices.
    if (pIndex == nullptr)
    {
        return;
    }

    const uint64_t index = pIndex->getZExtValue();

    // Check if the extract is coming from an insert element, and try and track the extract back to see if there is an
    // insert we can forward onto the result of the extract.

    Value* pNextVector = pVector;

    while (InsertElementInst* const pNextInsertElement = dyn_cast<InsertElementInst>(pNextVector))
    {
        ConstantInt* const pNextIndex = dyn_cast<ConstantInt>(pNextInsertElement->getOperand(2));

        // If the vector was inserting at a non-constant index, bail.
        if (pNextIndex == nullptr)
        {
            break;
        }

        // If the index of the insertion matches the index we were extracting, forward the insert!
        if (pNextIndex->equalsInt(index))
        {
            extractElement.replaceAllUsesWith(pNextInsertElement->getOperand(1));

            // Lastly remember to delete the extract we just replaced.
            m_instsToErase.push_back(&extractElement);

            return;
        }

        // Otherwise do another loop iteration and check the vector the insert element was inserting into.
        pNextVector = pNextInsertElement->getOperand(0);
    }

    uint32_t numCombinableUsers = 0;

    for (User* const pUser : pVector->users())
    {
        ExtractElementInst* const pOtherExtractElement = dyn_cast<ExtractElementInst>(pUser);

        // If we don't have an extract element, skip.
        if (pOtherExtractElement == nullptr)
        {
            continue;
        }

        // If the other extract has no users, no point optimizating it.
        if (pOtherExtractElement->user_empty())
        {
            continue;
        }

        ConstantInt* const pOtherIndex = dyn_cast<ConstantInt>(pOtherExtractElement->getIndexOperand());

        // If the other index is not a constant integer, skip.
        if (pOtherIndex == nullptr)
        {
            continue;
        }

        // If the indices do not match, skip.
        if (pOtherIndex->equalsInt(index) == false)
        {
            continue;
        }

        numCombinableUsers++;

        // If we have at least two extracts we can combine, skip checking and just do the optimization.
        if (numCombinableUsers > 1)
        {
            break;
        }
    }

    // If we have at least 2 users, optimize!
    if (numCombinableUsers > 1)
    {
        if (Instruction* const pInst = dyn_cast<Instruction>(pVector))
        {
            ExtractElementInst* const pNewExtractElement =
                ExtractElementInst::Create(pVector, pIndex, extractElement.getName());

            insertAfter(*pNewExtractElement, *pInst);

            // Replace all extract element instructions that extract with the same value and index.
            for (User* const pUser : pVector->users())
            {
                ExtractElementInst* const pOtherExtractElement = dyn_cast<ExtractElementInst>(pUser);

                // If we don't have an extract element, skip.
                if (pOtherExtractElement == nullptr)
                {
                    continue;
                }

                // If the other extract has no users, no point optimizating it.
                if (pOtherExtractElement->user_empty())
                {
                    continue;
                }

                // If the extract element is the new one we just inserted, skip.
                if (pNewExtractElement == pOtherExtractElement)
                {
                    continue;
                }

                ConstantInt* const pOtherIndex = dyn_cast<ConstantInt>(pOtherExtractElement->getIndexOperand());

                // If the other index is not a constant integer, skip.
                if (pOtherIndex == nullptr)
                {
                    continue;
                }

                // If the indices do not match, skip.
                if (pOtherIndex->equalsInt(index) == false)
                {
                    continue;
                }

                // Replace the other extraction with our new one.
                pOtherExtractElement->replaceAllUsesWith(pNewExtractElement);

                // Lastly remember to delete the extract we just replaced.
                m_instsToErase.push_back(pOtherExtractElement);
            }

            return;
        }
    }
}

// =====================================================================================================================
// Visit a PHI node.
void PatchPeepholeOpt::visitPHINode(
    PHINode& phiNode) // [in] The PHI node to visit.
{
    // If the PHI has no users, no point trying to optimize it!
    if (phiNode.user_empty())
    {
        return;
    }

    const uint32_t numIncomings = phiNode.getNumIncomingValues();

    // Only care about vector PHI nodes whose element size is at least 32 bits.
    if (phiNode.getType()->isVectorTy() && (phiNode.getType()->getScalarSizeInBits() >= 32))
    {
        // The integer type we'll use for our extract & insert elements.
        IntegerType* const pInt32Type = IntegerType::get(phiNode.getContext(), 32);

        // Where we will insert our vector create that will replace the uses of the PHI node.
        Instruction* const pInsertPos = phiNode.getParent()->getFirstNonPHI();

        // The type of the vector.
        Type* const pType = phiNode.getType();

        // The number of elements in the vector type (which will result in N new scalar PHI nodes).
        const uint32_t numElements = pType->getVectorNumElements();

        // The element type of the vector.
        Type* const pElementType = pType->getVectorElementType();

        Value* pResult = UndefValue::get(pType);

        // Loop through each element of the vector.
        for (uint32_t elementIndex = 0; elementIndex < numElements; elementIndex++)
        {
            // We create a new name that is "old name".N, where N is the index of element into the original vector.
            ConstantInt* const pElementIndex = ConstantInt::get(pInt32Type, elementIndex, false);

            PHINode* const pNewPhiNode = PHINode::Create(pElementType, numIncomings,
                                                         phiNode.getName() + "." +
                                                         Twine(elementIndex));
            insertAfter(*pNewPhiNode, phiNode);

            pResult = InsertElementInst::Create(pResult, pNewPhiNode, pElementIndex, "", pInsertPos);

            // Make sure the same incoming blocks have identical incoming values.
            // If we have already inserted an incoming arc for a basic block,
            // reuse the same value in the future incoming arcs from the same block.
            SmallDenseMap<BasicBlock*, Value*, 8> incomingPairMap;
            incomingPairMap.reserve(numIncomings);

            // Loop through each incoming edge to the PHI node.
            for (uint32_t incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++)
            {
                Value* const pIncoming = phiNode.getIncomingValue(incomingIndex);

                BasicBlock* const pBasicBlock = phiNode.getIncomingBlock(incomingIndex);

                if (Instruction* const pInst = dyn_cast<Instruction>(pIncoming))
                {
                    Value* pNewIncomingValue = nullptr;
                    auto it = incomingPairMap.find(pBasicBlock);
                    if (it != incomingPairMap.end())
                    {
                        pNewIncomingValue = it->second;
                    }
                    else
                    {
                        ExtractElementInst* const pExtractElement =
                            ExtractElementInst::Create(pIncoming, pElementIndex);

                        insertAfter(*pExtractElement, *pInst);
                        pNewIncomingValue = pExtractElement;
                        incomingPairMap.insert({pBasicBlock, pNewIncomingValue});
                    }

                    pNewPhiNode->addIncoming(pNewIncomingValue, pBasicBlock);
                }
                else if (Constant* const pConstant = dyn_cast<Constant>(pIncoming))
                {
                    Constant* const pExtractElement = ConstantExpr::getExtractElement(pConstant, pElementIndex);
                    incomingPairMap.insert({pBasicBlock, pExtractElement});
                    pNewPhiNode->addIncoming(pExtractElement, pBasicBlock);
                }
                else
                {
                    LLPC_NEVER_CALLED();
                }
            }
        }

        // Replace all the users of the original PHI node with the new nodes combined using insertions.
        phiNode.replaceAllUsesWith(pResult);

        // Lastly remember the phi so we can delete it later when it is safe to do so.
        m_instsToErase.push_back(&phiNode);

        return;
    }

    // Optimize PHI nodes that have incoming values that are identical in their parent blocks.
    Instruction* pPrevIncomingInst = nullptr;

    for (uint32_t incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++)
    {
        Instruction* const pIncomingInst = dyn_cast<Instruction>(phiNode.getIncomingValue(incomingIndex));

        // If we don't have an instruction, bail.
        if (pIncomingInst == nullptr)
        {
            pPrevIncomingInst = nullptr;
            break;
        }

        // If our incoming instruction is a PHI node, we can't move it so bail.
        if (isa<PHINode>(pIncomingInst))
        {
            pPrevIncomingInst = nullptr;
            break;
        }

        // If we don't have a previous instruction to compare against, store the current one and continue.
        if (pPrevIncomingInst == nullptr)
        {
            pPrevIncomingInst = pIncomingInst;
            continue;
        }

        if (pIncomingInst->isIdenticalTo(pPrevIncomingInst) == false)
        {
            pPrevIncomingInst = nullptr;
            break;
        }
    }

    // Do not clone allocas -- we don't want to potentially introduce them in the middle of the function.
    if ((pPrevIncomingInst != nullptr) && (isa<AllocaInst>(pPrevIncomingInst) == false))
    {
        Instruction* const pNewInst = pPrevIncomingInst->clone();
        insertAfter(*pNewInst, phiNode);

        // Replace all the users of the original PHI node with the incoming value that each incoming references.
        phiNode.replaceAllUsesWith(pNewInst);

        // Lastly remember the phi so we can delete it later when it is safe to do so.
        m_instsToErase.push_back(&phiNode);

        return;
    }

    if (numIncomings == 3)
    {
        // Optimize PHI nodes like:
        //   %p = phi [%a, %foo], [%b, %bar], [%b, %yar]
        //   %b = phi [%c, %har], [%p, %fiz]
        // Where we have multiple PHI nodes similar to %p, that take an %a, but are actually passing the same value
        // back and forth (and thus can be collapsed into a single PHI node).
        PHINode* const pSubPhiNode = dyn_cast<PHINode>(phiNode.getIncomingValue(1));

        if (pSubPhiNode == phiNode.getIncomingValue(2))
        {
            bool subPhiNodeOptimizable = true;

            const uint32_t numSubIncomings = pSubPhiNode->getNumIncomingValues();

            for (uint32_t subIncomingIndex = 0; subIncomingIndex < numSubIncomings; subIncomingIndex++)
            {
                Value* const pIncoming = pSubPhiNode->getIncomingValue(subIncomingIndex);

                // We can attempt to optimize the sub PHI node if an incoming is our parent PHI node, or if the
                // incoming is a constant.
                if ((pIncoming != &phiNode) && (isa<Constant>(pIncoming) == false))
                {
                    subPhiNodeOptimizable = false;
                    break;
                }
            }

            // If the sub PHI node was optimizable, lets try and optimize!
            if (subPhiNodeOptimizable)
            {
                for (User* const pUser : phiNode.getIncomingValue(0)->users())
                {
                    PHINode* const pOtherPhiNode = dyn_cast<PHINode>(pUser);

                    // If its not a PHI node, skip.
                    if (pOtherPhiNode == nullptr)
                    {
                        continue;
                    }

                    // Skip our PHI node in the user list.
                    if (pOtherPhiNode == &phiNode)
                    {
                        continue;
                    }

                    // If both PHI nodes are not in the same parent block, skip.
                    if (pOtherPhiNode->getParent() != phiNode.getParent())
                    {
                        continue;
                    }

                    // If the PHI does not match the number of incomings as us, skip.
                    if (pOtherPhiNode->getNumIncomingValues() != numIncomings)
                    {
                        continue;
                    }

                    PHINode* const pOtherSubPhiNode = dyn_cast<PHINode>(pOtherPhiNode->getIncomingValue(1));

                    // If the other incomings don't match, its not like our PHI node, skip.
                    if (pOtherSubPhiNode != pOtherPhiNode->getIncomingValue(2))
                    {
                        continue;
                    }

                    // If both sub PHI nodes are not in the same parent block, skip.
                    if (pOtherSubPhiNode->getParent() != pSubPhiNode->getParent())
                    {
                        continue;
                    }

                    // If the sub PHI nodes don't have the same incomings, we can't fold them so we skip.
                    if (pOtherSubPhiNode->getNumIncomingValues() != numSubIncomings)
                    {
                        continue;
                    }

                    for (uint32_t subIncomingIndex = 0; subIncomingIndex < numSubIncomings; subIncomingIndex++)
                    {
                        if (pSubPhiNode->getIncomingBlock(subIncomingIndex) !=
                            pOtherSubPhiNode->getIncomingBlock(subIncomingIndex))
                        {
                            subPhiNodeOptimizable = false;
                            break;
                        }

                        Value* const pIncoming = pSubPhiNode->getIncomingValue(subIncomingIndex);
                        Value* const pOtherIncoming = pOtherSubPhiNode->getIncomingValue(subIncomingIndex);

                        if ((pOtherIncoming != pOtherPhiNode) && (pOtherIncoming != pIncoming))
                        {
                            subPhiNodeOptimizable = false;
                            break;
                        }
                    }

                    if (subPhiNodeOptimizable)
                    {
                        // Both our PHI's are actually identical! Optimize away.
                        pOtherPhiNode->replaceAllUsesWith(&phiNode);
                        pOtherSubPhiNode->replaceAllUsesWith(pSubPhiNode);

                        // Lastly remember the phis so we can delete them later when it is safe to do so.
                        m_instsToErase.push_back(pOtherPhiNode);
                        m_instsToErase.push_back(pOtherSubPhiNode);
                    }
                }
            }
        }
    }

    if (numIncomings == 2)
    {
        // Optimize PHI nodes like:
        //   %p = phi [%a, %foo], [%b, %bar]
        // Where %a or %b is a binary operator, such that an operand of the binary operator is the other incoming:
        //   %a = add %b, %c
        // We optimize this by sinking the binary operator and instead make the PHI node pass %c down:
        //   %o = phi [%c, %foo], [0, %bar]
        //   %p = add %b, %o
        for (uint32_t incomingIndex = 0; incomingIndex < numIncomings; incomingIndex++)
        {
            const uint32_t otherIncomingIndex = (incomingIndex + 1) % 2;

            Value* const pIncoming = phiNode.getIncomingValue(incomingIndex);
            Value* const pOtherIncoming = phiNode.getIncomingValue(otherIncomingIndex);

            if (BinaryOperator* const pBinaryOp = dyn_cast<BinaryOperator>(pIncoming))
            {
                Value* const pOperands[2] =
                {
                    pBinaryOp->getOperand(0),
                    pBinaryOp->getOperand(1)
                };

                Value* sinkableValue = nullptr;

                if (pOtherIncoming == pOperands[0])
                {
                    sinkableValue = pOperands[1];
                }
                else if (pOtherIncoming == pOperands[1])
                {
                    sinkableValue = pOperands[0];
                }
                else
                {
                    continue;
                }

                // Create a constant for the other incoming that won't affect the result when the operator is applied.
                Constant* pNoEffectConstant = nullptr;

                const Instruction::BinaryOps opCode = pBinaryOp->getOpcode();

                switch (opCode)
                {
                case BinaryOperator::Add:
                    pNoEffectConstant = ConstantInt::get(sinkableValue->getType(), 0);
                    break;
                case BinaryOperator::Mul:
                    pNoEffectConstant = ConstantInt::get(sinkableValue->getType(), 1);
                    break;
                case BinaryOperator::FAdd:
                    pNoEffectConstant = ConstantFP::get(sinkableValue->getType(), 0.0);
                    break;
                case BinaryOperator::FMul:
                    pNoEffectConstant = ConstantFP::get(sinkableValue->getType(), 1.0);
                    break;
                default:
                    continue;
                }

                phiNode.setIncomingValue(incomingIndex, sinkableValue);
                phiNode.setIncomingValue(otherIncomingIndex, pNoEffectConstant);

                BinaryOperator* const pNewBinaryOp = BinaryOperator::Create(opCode, &phiNode, pOtherIncoming);
                if (isa<FPMathOperator>(pNewBinaryOp))
                    pNewBinaryOp->copyFastMathFlags(pBinaryOp);

                insertAfter(*pNewBinaryOp, phiNode);

                // Replace all the users of the original PHI node with the binary operator.
                phiNode.replaceAllUsesWith(pNewBinaryOp);

                // We just replaced our binary operators use of the phi, so we need to reset the use.
                pNewBinaryOp->setOperand(0, &phiNode);

                // We've optimized the PHI, so we're done!
                return;
            }
        }
    }
}

// =====================================================================================================================
// Visits "call" instruction.
void PatchPeepholeOpt::visitCallInst(
    CallInst& callInst) // [in] "Call" instruction
{
    auto pCallee = callInst.getCalledFunction();
    if (pCallee == nullptr)
    {
        return;
    }

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
    if (cl::EnableDiscardOpt && m_enableDiscardOpt && (pCallee->getIntrinsicID() == Intrinsic::amdgcn_kill))
    {
        auto pBlock = callInst.getParent();
        if (pBlock->size() > 2)
        {
            // Apply the optimization to blocks that contains single kill call instruction.
            return;
        }

        for (BasicBlock *pPredBlock : predecessors(pBlock))
        {
            auto pTerminator = pPredBlock->getTerminator();
            BranchInst* pBranch = dyn_cast<BranchInst>(pTerminator);
            if (pBranch && pBranch->isConditional())
            {
                auto pCond = pBranch->getCondition();
                auto pTrueBlock = dyn_cast<BasicBlock>(pBranch->getSuccessor(0));
                auto pNewKill = dyn_cast<CallInst>(callInst.clone());
                llvm::LLVMContext* pContext = &callInst.getContext();

                if (pTrueBlock == pBlock)
                {
                    // the kill block is the true condition block
                    // insert a bitwise not instruction.
                    auto pNotCond = BinaryOperator::CreateNot(pCond, "", pTerminator);
                    pNewKill->setArgOperand(0, pNotCond);
                    // Make the kill block unreachable
                    pBranch->setCondition(ConstantInt::get(Type::getInt1Ty(*pContext), false));
                }
                else
                {
                    pNewKill->setArgOperand(0, pCond);
                    // make the kill block unreachable
                    pBranch->setCondition(ConstantInt::get(Type::getInt1Ty(*pContext), true));
                }
                pNewKill->insertBefore(pTerminator);
            }
        }
    }
}

// =====================================================================================================================
// Helper function to move an instruction after another.
void PatchPeepholeOpt::moveAfter(
    Instruction& move, // [in] Instruction to move.
    Instruction& after // [in] Where to move after.
    ) const
{
    // Special case for if the instruction is a PHI node, we need to move after all other PHIs.
    if (isa<PHINode>(&after))
    {
        move.moveBefore(after.getParent()->getFirstNonPHI());
    }
    else
    {
        move.moveAfter(&after);
    }
}

// =====================================================================================================================
// Helper function to insert an instruction after another.
void PatchPeepholeOpt::insertAfter(
    Instruction& insert, // [in] Instruction to insert.
    Instruction& after   // [in] Where to insert after.
    ) const
{
    // Special case for if the instruction is a PHI node, we need to insert after all other PHIs.
    if (isa<PHINode>(&after))
    {
        insert.insertBefore(after.getParent()->getFirstNonPHI());
    }
    else
    {
        insert.insertAfter(&after);
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of LLVM patching operations for peephole optimizations.
INITIALIZE_PASS(PatchPeepholeOpt, DEBUG_TYPE,
    "Patch LLVM for peephole optimizations", false, false)
