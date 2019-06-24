/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderImpl.cpp
 * @brief LLPC source file: implementation of Llpc::BuilderImpl
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcContext.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
Context& BuilderImplBase::getContext() const
{
    return *static_cast<Llpc::Context*>(&Builder::getContext());
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP operations.
bool BuilderImplBase::SupportDpp() const
{
    return getContext().GetGfxIpVersion().major >= 8;
}

// =====================================================================================================================
// Get whether the context we are building in support the bpermute operation.
bool BuilderImplBase::SupportBPermute() const
{
    auto gfxIp = getContext().GetGfxIpVersion().major;
    auto supportBPermute = (gfxIp == 8) || (gfxIp == 9);
#if LLPC_BUILD_GFX10
    auto waveSize = getContext().GetShaderWaveSize(GetShaderStageFromFunction(GetInsertBlock()->getParent()));
    supportBPermute = supportBPermute || ((gfxIp == 10) && (waveSize == 32));
#endif
    return supportBPermute;
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Get whether the context we are building in supports permute lane DPP operations.
bool BuilderImplBase::SupportPermLaneDpp() const
{
    return getContext().GetGfxIpVersion().major >= 10;
}
#endif

// =====================================================================================================================
// Create an "if..endif" or "if..else..endif" structure. The current basic block becomes the "endif" block, and all
// instructions in that block before the insert point are moved to the "if" block. The insert point is moved to
// the start of the "then" block; the caller can save the insert point before calling this method then restore it
// afterwards to restore the insert point to where it was just after the endif, and still keep its debug location.
// The method returns the branch instruction, whose first branch target is the "then" block and second branch
// target is the "else" block, or "endif" block if no "else" block.
BranchInst* BuilderImplBase::CreateIf(
    Value*        pCondition,   // [in] The "if" condition
    bool          wantElse,     // Whether to generate an "else" block
    const Twine&  instName)     // Base of name for new basic blocks
{
    // Create "if" block and move instructions in current block to it.
    BasicBlock* pEndIfBlock = GetInsertBlock();
    BasicBlock* pIfBlock = BasicBlock::Create(getContext(), "", pEndIfBlock->getParent(), pEndIfBlock);
    pIfBlock->takeName(pEndIfBlock);
    pEndIfBlock->setName(instName + ".endif");
    pIfBlock->getInstList().splice(pIfBlock->end(),
                                   pEndIfBlock->getInstList(),
                                   pEndIfBlock->begin(),
                                   GetInsertPoint());

    // Replace non-phi uses of the original block with the new "if" block.
    SmallVector<Use*, 4> nonPhiUses;
    for (auto& use : pEndIfBlock->uses())
    {
        if (isa<PHINode>(use.getUser()) == false)
        {
            nonPhiUses.push_back(&use);
        }
    }
    for (auto pUse : nonPhiUses)
    {
        pUse->set(pIfBlock);
    }

    // Create "then" and "else" blocks.
    BasicBlock* pThenBlock = BasicBlock::Create(getContext(),
                                                instName + ".then",
                                                pEndIfBlock->getParent(),
                                                pEndIfBlock);
    BasicBlock* pElseBlock = nullptr;
    if (wantElse)
    {
        pElseBlock = BasicBlock::Create(getContext(),
                                        instName + ".else",
                                        pEndIfBlock->getParent(),
                                        pEndIfBlock);
    }

    // Create the branches.
    BranchInst* pBranch = BranchInst::Create(pThenBlock,
                                             pElseBlock != nullptr ? pElseBlock : pEndIfBlock,
                                             pCondition,
                                             pIfBlock);
    pBranch->setDebugLoc(getCurrentDebugLocation());
    BranchInst::Create(pEndIfBlock, pThenBlock)->setDebugLoc(getCurrentDebugLocation());
    if (pElseBlock != nullptr)
    {
        BranchInst::Create(pEndIfBlock, pElseBlock)->setDebugLoc(getCurrentDebugLocation());
    }

    // Set Builder's insert point to the branch at the end of the "then" block.
    SetInsertPoint(pThenBlock->getTerminator());
    return pBranch;
}

