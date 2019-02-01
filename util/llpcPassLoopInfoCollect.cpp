/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPassLoopInfoCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PassLoopInfoCollect.
 ***********************************************************************************************************************
 */
#include "llvm/ADT/SetVector.h"

#include "llpcDebug.h"
#include "llpcInternal.h"
#include "llpcPassLoopInfoCollect.h"

#define DEBUG_TYPE "llpc-pass-loop-info-collect"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Initializes static members.
char PassLoopInfoCollect::ID = 0;

// =====================================================================================================================
PassLoopInfoCollect::PassLoopInfoCollect(
    bool* pNeedDynamicLoopUnroll)   // [out] Whether dynamic unrolling is required, set when the pass is run
    :
    ModulePass(ID),
    m_pNeedDynamicLoopUnroll(pNeedDynamicLoopUnroll)
{
    initializeLoopInfoWrapperPassPass(*PassRegistry::getPassRegistry());
    initializePassLoopInfoCollectPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Decide whether one loop satisfies the criteria for needing dynamic loop unrolling.
bool PassLoopInfoCollect::NeedDynamicLoopUnroll(
    const Loop*    pLoop)                          // [in] Loop block to gather infomation
{
    if (pLoop->getLoopDepth() >= 4)
    {
        return true;
    }

    uint32_t numBasicBlocks = 0;
    uint32_t numAluInsts = 0;

    for (Loop::block_iterator block = pLoop->block_begin(); block != pLoop->block_end(); ++block)
    {
        numBasicBlocks++;

        for (BasicBlock::iterator inst = (*block)->begin(); inst != (*block)->end(); ++inst)
        {
            if (inst->isBinaryOp())
            {
                numAluInsts++;
            }
        }
    }

    // These conditions mean the loop is a complex loop.
    return (numAluInsts > 20) || (numBasicBlocks > 8);
}

// =====================================================================================================================
// Executes the get loop info pass on the specified LLVM module
//
// This pass runs only on the fragment shader module. If it contains a loop deemed here to be a
// "complex loop", then we set *m_pNeedDynamicLoopUnroll to true.
bool PassLoopInfoCollect::runOnModule(
    llvm::Module& module)                       // [in] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Pass-Loop-Info-Collect\n");

    for (auto &func : module)
    {
        auto& loopInfo = getAnalysis<LoopInfoWrapperPass>(func).getLoopInfo();

        for (auto loop : loopInfo)
        {
            if (NeedDynamicLoopUnroll(loop))
            {
                *m_pNeedDynamicLoopUnroll = true;
                break;
            }
        }
    }

    return false;
}

// =====================================================================================================================
INITIALIZE_PASS(PassLoopInfoCollect, DEBUG_TYPE,
                "Determine whether dynamic loop unrolling is needed", false, false)
