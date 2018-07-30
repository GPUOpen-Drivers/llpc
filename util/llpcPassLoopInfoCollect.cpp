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
 * @file  llpcPassLoopInfoCollect.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LoopInfoCollect.
 ***********************************************************************************************************************
 */

#define DEBUG_TYPE "llpc-pass-loop-info-collect"

#include "llvm/IR/Verifier.h"

#include "llpcDebug.h"
#include "llpcPassLoopInfoCollect.h"

using namespace llvm;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char LoopInfoCollect::ID = 0;

// =====================================================================================================================
// Gather various infomation for one loop.
// It will also gather infomation for this loop's children by calling itself recursively.
void LoopInfoCollect::HandleLoop(
    Loop*    loop,                          // [in] Loop block to gather infomation
    uint32_t nestedLevel)                   // [in] Nested level of this loop
{
    LoopAnalysisInfo loopInfo = { 0 };
    loopInfo.nestedLevel = nestedLevel;

    for (Loop::block_iterator block = loop->block_begin(); block != loop->block_end(); ++block)
    {
        loopInfo.numBasicBlocks++;

        for (BasicBlock::iterator inst = (*block)->begin(); inst != (*block)->end(); ++inst)
        {
            if (inst->isBinaryOp())
            {
                loopInfo.numAluInsts++;
            }
        }
    }

    m_pLoopInfo->push_back(loopInfo);

    std::vector<Loop*> subLoops = loop->getSubLoops();
    for (Loop* subLoop : subLoops)
    {
        HandleLoop(subLoop, nestedLevel + 1);
    }
}

// =====================================================================================================================
// Executes the get loop info pass on the specified LLVM module.
bool LoopInfoCollect::runOnModule(
    llvm::Module& module)                       // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Pass-Loop-Info-Collect\n");

    uint32_t loopCounter = 0;
    for (Function& function : module)
    {
        if (function.empty())
        {
            continue;
        }

        LoopInfoWrapperPass& loopInfoPass = getAnalysis<LoopInfoWrapperPass>(function);
        LoopInfo& loopInfo = loopInfoPass.getLoopInfo();

        for (LoopInfo::iterator loopIt = loopInfo.begin(), loopItEnd = loopInfo.end();
             loopIt != loopItEnd; ++loopIt)
        {
            HandleLoop(*loopIt, 0);
        }
    }

    LLPC_VERIFY_MODULE_FOR_PASS(module);

    return false;
}

} // Llpc
