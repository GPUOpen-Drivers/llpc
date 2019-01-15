/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerAccessChain.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerAccessChain.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-access-chain"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <stack>
#include "SPIRVInternal.h"
#include "llpcSpirvLowerAccessChain.h"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerAccessChain::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for access chain
ModulePass* CreateSpirvLowerAccessChain()
{
    return new SpirvLowerAccessChain();
}

// =====================================================================================================================
SpirvLowerAccessChain::SpirvLowerAccessChain()
    :
    SpirvLower(ID)
{
    initializeSpirvLowerAccessChainPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerAccessChain::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Access-Chain\n");

    SpirvLower::Init(&module);

    // Invoke handling of "getelementptr" instruction
    visit(m_pModule);

    return true;
}

// =====================================================================================================================
// Visits "getelementptr" instruction.
void SpirvLowerAccessChain::visitGetElementPtrInst(
    GetElementPtrInst& getElemPtrInst) // [in] "Getelementptr" instruction
{
    auto pGetElemPtrInst = &getElemPtrInst;

    // NOTE: Here, we try to coalesce chained "getelementptr" instructions (created from multi-level access chain).
    // Because the metadata is always decorated on top-level pointer value (actually a global variable).
    const uint32_t addrSpace = pGetElemPtrInst->getType()->getPointerAddressSpace();
    if ((addrSpace == SPIRAS_Private) ||
        (addrSpace == SPIRAS_Input) || (addrSpace == SPIRAS_Output) ||
        (addrSpace == SPIRAS_Uniform))
    {
        TryToCoalesceChain(&getElemPtrInst, addrSpace);
    }
}

// =====================================================================================================================
// Tries to coalesce chained "getelementptr" instructions (created from multi-level access chain) from bottom to top
// in the type hierarchy.
//
// e.g.
//      %x = getelementptr %blockType, %blockType addrspace(N)* @block, i32 0, i32 L, i32 M
//      %y = getelementptr %fieldType, %fieldType addrspace(N)* %x, i32 0, i32 N
//
//      =>
//
//      %y = getelementptr %blockType, %blockType addrspace(N)* @block, i32 0, i32 L, i32 M, i32 N
//
llvm::GetElementPtrInst* SpirvLowerAccessChain::TryToCoalesceChain(
    GetElementPtrInst* pGetElemPtr, // [in] "getelementptr" instruction in the bottom to do coalescing
    uint32_t           addrSpace)   // Address space of the pointer value of "getelementptr"
{
    GetElementPtrInst* pCoalescedGetElemPtr = pGetElemPtr;

    std::stack<User*>              chainedInsts; // Order: from top to bottom
    std::stack<GetElementPtrInst*> removedInsts; // Order: from botton to top

    // Collect chained "getelementptr" instructions and constants from bottom to top.
    auto pPtrVal = cast<User>(pGetElemPtr);
    for (;;)
    {
        chainedInsts.push(pPtrVal);
        auto pNext = pPtrVal->getOperand(0);
        if (isa<GetElementPtrInst>(pNext))
        {
            pPtrVal = cast<User>(pNext);
            continue;
        }
        auto pConst = dyn_cast<ConstantExpr>(pNext);
        if ((pConst == nullptr) || (pConst->getOpcode() != Instruction::GetElementPtr))
        {
            break;
        }
        pPtrVal = cast<User>(pNext);
    }

    // If there are more than one "getelementptr" instructions/constants, do coalescing
    if (chainedInsts.size() > 1)
    {
        std::vector<Value*> idxs;
        uint32_t startOperand = 1;
        Value* pBlockPtr = nullptr;

        do
        {
            pPtrVal = chainedInsts.top();
            chainedInsts.pop();
            if (pBlockPtr == nullptr)
            {
                pBlockPtr = pPtrVal->getOperand(0);
            }
            for (uint32_t i = startOperand; i != pPtrVal->getNumOperands(); ++i)
            {
                idxs.push_back(pPtrVal->getOperand(i));
            }
            // NOTE: For subsequent "getelementptr" instructions/constants, we skip the first two operands. The first
            // operand is the pointer value from which the element pointer is constructed. And the second one is always
            // 0 to dereference the pointer value.
            startOperand = 2;

            auto pInst = dyn_cast<GetElementPtrInst>(pPtrVal);
            if (pInst != nullptr)
            {
                removedInsts.push(pInst);
            }
        }
        while (chainedInsts.empty() == false);

        // Create the coalesced "getelementptr" instruction (do combining)
        pCoalescedGetElemPtr = GetElementPtrInst::Create(nullptr, pBlockPtr, idxs, "", pGetElemPtr);
        pGetElemPtr->replaceAllUsesWith(pCoalescedGetElemPtr);

        // Remove dead "getelementptr" instructions where possible.
        while (removedInsts.empty() == false)
        {
            GetElementPtrInst* pInst = removedInsts.top();
            if (pInst->user_empty())
            {
                if (pInst == pGetElemPtr)
                {
                    // We cannot remove the current instruction that InstWalker is on. Just stop it using its
                    // pointer operand, and it will be DCEd later.
                    auto& operand = pInst->getOperandUse(0);
                    operand = UndefValue::get(operand->getType());
                }
                else
                {
                    pInst->eraseFromParent();
                }
            }
            removedInsts.pop();
        }
    }

    return pCoalescedGetElemPtr;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for access chain.
INITIALIZE_PASS(SpirvLowerAccessChain, DEBUG_TYPE,
                "Lower SPIR-V access chain", false, false)
