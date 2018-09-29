/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerLoopUnrollInfoRectify.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerLoopUnrollInfoRectify.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-loop-unroll-info-rectify"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llpcSpirvLowerLoopUnrollInfoRectify.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Prototypes for static helper functions.
static uint32_t GetLoopUnrollTripCount(ScalarEvolution& scalarEvolution, Loop* const pLoop);
static uint32_t GetConditionTripCount(Value* const pValue);
static uint32_t GetConditionTripCountWithForLikeLoop(ICmpInst* const pCmpInst);
static uint32_t GetConditionTripCountWithMaskAndShiftLoop(ICmpInst* const pCmpInst);

namespace Llpc
{

// =====================================================================================================================
// Define static members (no initializer needed as LLVM only cares about the address of ID, never its value).
char SpirvLowerLoopUnrollInfoRectify::ID;

// =====================================================================================================================
SpirvLowerLoopUnrollInfoRectify::SpirvLowerLoopUnrollInfoRectify()
    :
    FunctionPass(ID)
{
    initializeSpirvLowerLoopUnrollInfoRectifyPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this LLVM pass on the specified LLVM function.
bool SpirvLowerLoopUnrollInfoRectify::runOnFunction(
    Function& function) // [in,out] Function that we will rectify any loop unroll information.
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Loop-Unroll-Info-Rectify\n");

    bool modified = false;

    LoopInfo& loopInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

    auto loops = loopInfo.getLoopsInPreorder();

    ScalarEvolution& scalarEvolution = getAnalysis<ScalarEvolutionWrapperPass>().getSE();

    for (Loop* const pLoop : loops)
    {
        // Check if our loop has user specified loop unroll info, and do no processing if so.
        if (MDNode* const pLoopId = pLoop->getLoopID())
        {
            // If we have more than one operand, it means we have a user specified loop unroll value which we honor.
            if (pLoopId->getNumOperands() > 1)
            {
                continue;
            }
        }

        uint32_t tripCount = GetLoopUnrollTripCount(scalarEvolution, pLoop);

        // If we couldn't work out a good trip count, bail.
        if (tripCount == 0)
        {
            continue;
        }

        // If our trip count is bigger than the maximum we allow, we need to reduce it to a multiple of the trip count
        // we have detected the loop to have. The worst case when we have a prime number of loop iterations is that loop
        // unrolling will be disabled (trip count == 1).
        if (tripCount > MaxLoopUnrollCount)
        {
            for (uint32_t i = MaxLoopUnrollCount; i != 0; i--)
            {
                // If our trip count divides by i with no remainder, we use it as our trip count.
                if ((tripCount % i) == 0)
                {
                    tripCount = i;
                    break;
                }
            }
        }

        Constant* const pTripCount = ConstantInt::get(IntegerType::get(function.getContext(), 32), tripCount);

        Metadata* const unrollCountMetadatas[2] =
        {
            MDString::get(function.getContext(), "llvm.loop.unroll.count"),
            ConstantAsMetadata::get(pTripCount)
        };

        MDNode* const pUnrollCountMetadataNode = MDNode::get(function.getContext(), unrollCountMetadatas);

        Metadata* const loopMetadatas[2] =
        {
            nullptr,
            pUnrollCountMetadataNode
        };

        MDNode* const pLoopIdMetadataNode = MDNode::get(function.getContext(), loopMetadatas);

        pLoopIdMetadataNode->replaceOperandWith(0, pLoopIdMetadataNode);

        pLoop->setLoopID(pLoopIdMetadataNode);

        modified = true;
    }

    LLPC_VERIFY_MODULE_FOR_PASS(*(function.getParent()));

    return modified;
}

// =====================================================================================================================
// Specify what analysis passes this pass depends on.
void SpirvLowerLoopUnrollInfoRectify::getAnalysisUsage(
    AnalysisUsage& analysisUsage // [in,out] The place to record our analysis pass usage requirements.
    ) const
{
    analysisUsage.addRequired<LoopInfoWrapperPass>();
    analysisUsage.addPreserved<LoopInfoWrapperPass>();
    analysisUsage.addRequired<ScalarEvolutionWrapperPass>();
    analysisUsage.addPreserved<ScalarEvolutionWrapperPass>();
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for rectifying unroll information.
INITIALIZE_PASS(SpirvLowerLoopUnrollInfoRectify, "Spirv-lower-loop-unroll-info-rectify",
    "Lower SPIR-V loop unroll info rectifying", false, false)

// =====================================================================================================================
// Definitions for our static helper functions.

// This function takes an llvm::Loop and calculates if there is a known compile time loop trip count that we can tell
// the unroller to use.
uint32_t GetLoopUnrollTripCount(
    ScalarEvolution& scalarEvolution, // [in] Scalar evolution analysis result.
    Loop* const pLoop)                // [in] The LLVM loop to try and calculate a trip count for.
{
    // If the loop backedge is not loop invariant, bail.
    if (scalarEvolution.hasLoopInvariantBackedgeTakenCount(pLoop))
    {
        const SCEVConstant* const pScev = dyn_cast<SCEVConstant>(scalarEvolution.getBackedgeTakenCount(pLoop));

        // If the number of loop iterations is a known constant.
        if (pScev != nullptr)
        {
            APInt backedgeCount = pScev->getAPInt();

            // The backedge count is the number of times the loop branches back to the loop header, which is one less
            // than the actual trip count of the loop - so we thus have to increment it by 1 to set the correct loop
            // unroll amount.
            backedgeCount++;

            const uint64_t tripCount = backedgeCount.getLimitedValue(UINT32_MAX);

            return (tripCount != UINT32_MAX) ? static_cast<uint32_t>(tripCount) : 0;
        }
    }

    SmallVector<BasicBlock*, 8> exitBlocks;
    pLoop->getExitingBlocks(exitBlocks);

    uint32_t tripCount = 0;

    for (BasicBlock* const pExitBlock : exitBlocks)
    {
        BranchInst* const pBranch = dyn_cast<BranchInst>(pExitBlock->getTerminator());

        // If we don't have a conditional branch, bail.
        if ((pBranch == nullptr) || pBranch->isUnconditional())
        {
            continue;
        }

        const uint32_t conditionTripCount = GetConditionTripCount(pBranch->getCondition());

        tripCount = (tripCount < conditionTripCount) ? conditionTripCount : tripCount;
    }

    return tripCount;
}

// =====================================================================================================================
// This function analyses the condition of a conditional branch instruction to see if we can infer a loop trip count
// from common loop patterns in user code.
uint32_t GetConditionTripCount(
    Value* const pCondition) // [in] Conditional branch condition - an i1/boolean value.
{
    if (BinaryOperator* const pBinaryOperator = dyn_cast<BinaryOperator>(pCondition))
    {
        // Loop exit conditions can have multiple clauses, so detect if our loop condition is from a bitwise and/or/xor
        // and skip through it to check its operands for their trip counts.
        switch (pBinaryOperator->getOpcode())
        {
        case BinaryOperator::And:
        case BinaryOperator::Or:
        case BinaryOperator::Xor:
            break;
        default:
            return 0;
        }

        const uint32_t lhs = GetConditionTripCount(pBinaryOperator->getOperand(0));
        const uint32_t rhs = GetConditionTripCount(pBinaryOperator->getOperand(1));

        return (lhs < rhs) ? rhs : lhs;
    }
    else if (ICmpInst* const pCmpInst = dyn_cast<ICmpInst>(pCondition))
    {
        switch (pCmpInst->getPredicate())
        {
        case CmpInst::ICMP_SLT:
        case CmpInst::ICMP_ULT:
            return GetConditionTripCountWithForLikeLoop(pCmpInst);
        case CmpInst::ICMP_EQ:
            return GetConditionTripCountWithMaskAndShiftLoop(pCmpInst);
        default:
            return 0;
        }
    }

    return 0;
}

// =====================================================================================================================
// This function will see if our loop trip condition looks like a for-loop - in that it has a constant integer starting
// value, a constant increment value, and a constant end range value.
uint32_t GetConditionTripCountWithForLikeLoop(
    ICmpInst* const pCmpInst) // [in] The comparison instruction to check if it is for like.
{
    // There are cases where we want to do loop unrolling when a term involved in the loop exit is a constant. To detect
    // these cases we need to:
    // - Check if we have an integer compare less than instruction where the second argument is a constant integer.
    // - Check if the first argument of the compare is an add whose second operand is 1 (EG. an integer increment).
    // - Check that the first operand of the add is a phi.
    // - And lastly check that the phi starts at 0 at the entry to the loop.
    ConstantInt* const pEnd = dyn_cast<ConstantInt>(pCmpInst->getOperand(1));

    // If we don't have x < CONSTANT, bail.
    if (pEnd == nullptr)
    {
        return 0;
    }

    BinaryOperator* const pAdd = dyn_cast<BinaryOperator>(pCmpInst->getOperand(0));

    // If x isn't an integer add, bail.
    if ((pAdd == nullptr) || (pAdd->getOpcode() != Instruction::Add))
    {
        return 0;
    }

    ConstantInt* const pInc = dyn_cast<ConstantInt>(pAdd->getOperand(1));

    // If we are not incrementing by a constant integer, bail.
    if (pInc == nullptr)
    {
        return 0;
    }

    PHINode* const pPhi = dyn_cast<PHINode>(pAdd->getOperand(0));

    // If our add is not operating on a phi, bail.
    if (pPhi == nullptr)
    {
        return 0;
    }

    const int32_t addIndex = pPhi->getBasicBlockIndex(pAdd->getParent());

    // If the phi doesn't have an incoming that is our add, we don't understand the loop iteration stategy so bail.
    if (addIndex == -1)
    {
        return 0;
    }

    ConstantInt* pBeg = nullptr;

    for (uint32_t i = 0; i < pPhi->getNumIncomingValues(); i++)
    {
        // Skip the add node as we've checked it already.
        if (addIndex == i)
        {
            continue;
        }

        ConstantInt* const pIncoming = dyn_cast<ConstantInt>(pPhi->getIncomingValue(i));

        // If the incoming value to the phi was not a constant integer, we don't understand the loop iteration so bail.
        if (pIncoming == nullptr)
        {
            return 0;
        }

        if (pBeg == nullptr)
        {
            pBeg = pIncoming;
        }
        else if (pBeg != pIncoming)
        {
            // If we got here, we had at least 2 phi nodes that had different constant integer inputs, that is confusing
            // so bail.
            return 0;
        }
    }

    // If we didn't get at least one constant, bail.
    if (pBeg == nullptr)
    {
        return 0;
    }

    // If we get here we have identified a loop like:
    //   for (i = beg; i < end; i += inc) {}
    // which is something we can work with!
    if (pCmpInst->isUnsigned())
    {
        const uint64_t begValue = pBeg->getZExtValue();
        const uint64_t endValue = pEnd->getZExtValue();
        const uint64_t incValue = pInc->getZExtValue();

        // If our start value is bigger than the end value, bail (the optimizer should clean this up for us anyway).
        if (begValue > endValue)
        {
            return 0;
        }

        return (endValue - begValue) / incValue;
    }
    else
    {
        const int64_t begValue = pBeg->getSExtValue();
        const int64_t endValue = pEnd->getSExtValue();
        const int64_t incValue = pInc->getSExtValue();

        // If our start value is bigger than the end value, bail (the optimizer should clean this up for us anyway).
        if (begValue > endValue)
        {
            return 0;
        }

        return (endValue - begValue) / incValue;
    }
}

// =====================================================================================================================
// This function will see if our loop is a mask and shift - these types of loops tend to have a starting value of an
// Integer, and they iterate through this integer bit-by-bit, resulting in a number of loop iterations equal to the
// bit-width of the input integer value.
uint32_t GetConditionTripCountWithMaskAndShiftLoop(
    ICmpInst* const pCmpInst) // [in] The comparison instruction to check if it is mask-and-shift like.
{
    // We are looking for a pattern like:
    //   a = phi with an incoming of e
    //   b = countTrailingZeros(a)
    //   c = shl 1, b
    //   d = not c
    //   e = and a, d
    // cmp = cmp e, 0
    // as this is a good target to unroll to the integer width of a, as we are effectively walking through all the bits
    // of the integer and doing a loop iteration for each.
    ConstantInt* const pEnd = dyn_cast<ConstantInt>(pCmpInst->getOperand(1));

    // If we don't have x == 0, bail.
    if ((pEnd == nullptr) || (pEnd->isZero() == false))
    {
        return 0;
    }

    BinaryOperator* const pAnd = dyn_cast<BinaryOperator>(pCmpInst->getOperand(0));

    // If it isn't an integer and, bail.
    if ((pAnd == nullptr) || (pAnd->getOpcode() != Instruction::And))
    {
        return 0;
    }

    BinaryOperator* const pNot = dyn_cast<BinaryOperator>(pAnd->getOperand(1));

    // If it isn't an integer xor, bail.
    if ((pNot == nullptr) || (pNot->getOpcode() != Instruction::Xor))
    {
        return 0;
    }

    ConstantInt* const pNotOperand1 = dyn_cast<ConstantInt>(pNot->getOperand(1));

    // If the second arg of pNot is not -1, the xor isn't a not, bail.
    if ((pNotOperand1 == nullptr) || (pNotOperand1->isMinusOne() == false))
    {
        return 0;
    }

    BinaryOperator* const pShl = dyn_cast<BinaryOperator>(pNot->getOperand(0));

    // If it isn't an integer shl, bail.
    if ((pShl == nullptr) || (pShl->getOpcode() != Instruction::Shl))
    {
        return 0;
    }

    ConstantInt* const pShlOperand0 = dyn_cast<ConstantInt>(pShl->getOperand(0));

    // If the first arg of pShl is not 1, the masking is weird and we bail.
    if ((pShlOperand0 == nullptr) || (pShlOperand0->isOne() == false))
    {
        return 0;
    }

    IntrinsicInst* const pCttz = dyn_cast<IntrinsicInst>(pShl->getOperand(1));

    // If it is not the cttz intrinsic, bail.
    if ((pCttz == nullptr) || (pCttz->getIntrinsicID() != Intrinsic::cttz))
    {
        return 0;
    }

    PHINode* const pPhi = dyn_cast<PHINode>(pCttz->getArgOperand(0));

    // If it is not a PHI node, bail.
    if (pPhi == nullptr)
    {
        return 0;
    }

    bool found = false;

    for (Value* const pIncoming : pPhi->incoming_values())
    {
        if (pIncoming == pAnd)
        {
            found = true;
            break;
        }
    }

    // If the PHI did not have one incoming that was e, bail.
    if (found == false)
    {
        return 0;
    }

    Type* const pType = pAnd->getType();

    return pType->isIntegerTy() ? pType->getPrimitiveSizeInBits() : 0;
}
