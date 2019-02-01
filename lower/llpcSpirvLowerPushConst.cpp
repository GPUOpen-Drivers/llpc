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
 * @file  llpcSpirvLowerPushConst.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerPushConst.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcSpirvLowerPushConst.h"

#define DEBUG_TYPE "llpc-spirv-lower-push-const"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerPushConst::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering opertions for push constant
ModulePass* CreateSpirvLowerPushConst()
{
    return new SpirvLowerPushConst();
}

// =====================================================================================================================
SpirvLowerPushConst::SpirvLowerPushConst()
    :
    SpirvLower(ID)
{
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerPushConst::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Push-Const\n");

    SpirvLower::Init(&module);

    for (Function& func : module)
    {
        if (func.empty())
        {
            continue;
        }

        Instruction& insertPos = func.getEntryBlock().getInstList().back();

        LoopInfoWrapperPass& loopInfoPass = getAnalysis<LoopInfoWrapperPass>(func);
        LoopInfo& loopInfo = loopInfoPass.getLoopInfo();

        for (Loop* pLoop : loopInfo)
        {
            HandleLoop(pLoop, &insertPos);
        }
    }

    return true;
}

// =====================================================================================================================
// Gets loop analysis usage
void SpirvLowerPushConst::getAnalysisUsage(
    AnalysisUsage &analysisUsage                // [in, out] The place to record our analysis pass usage requirements.
    ) const
{
    analysisUsage.addRequired<LoopInfoWrapperPass>();
}

// =====================================================================================================================
// Run for every loop to find push constants load call.
void SpirvLowerPushConst::HandleLoop(
    Loop*        pLoop,                         // [in] Loop block to gather infomation
    Instruction* pInsertPos)                    // [in] all instructions will be inserted before this instruction
{
    for (Loop::block_iterator blockIt = pLoop->block_begin(); blockIt != pLoop->block_end(); ++blockIt)
    {
        for (BasicBlock::iterator instIt = (*blockIt)->begin(); instIt != (*blockIt)->end();)
        {
            CallInst* pCall = dyn_cast<CallInst>(instIt);
            ++instIt;
            if (pCall == nullptr)
            {
                continue;
            }

            if (Function* pCallee = pCall->getCalledFunction())
            {
                if (pCallee->getName().startswith(LlpcName::PushConstLoad))
                {
                    auto pSrc = pCall->getArgOperand(0);
                    if (ConstantInt* pOffset = dyn_cast<ConstantInt>(pSrc))
                    {
                        Type* pLoadTy = pCall->getType();
                        uint32_t offset = static_cast<uint32_t>(pOffset->getZExtValue());

                        LLPC_ASSERT(pLoadTy->isVectorTy());

                        uint32_t compCount = pLoadTy->getVectorNumElements();
                        uint32_t searchKey = (compCount << 16) | (offset & 0xFFFF);

                        auto pMapEntry = m_pushConstLoadMap.find(searchKey);
                        if (pMapEntry != m_pushConstLoadMap.end())
                        {
                            // Data already loaded
                            CallInst* pInst = static_cast<CallInst*>(pMapEntry->second);
                            pCall->replaceAllUsesWith(pInst);
                            pCall->eraseFromParent();
                        }
                        else
                        {
                            // Move the load call outside the loop
                            m_pushConstLoadMap[searchKey] = pCall;
                            pCall->moveBefore(pInsertPos);
                        }
                    }
                }
            }
        }
    }
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering opertions for push constant.
INITIALIZE_PASS(SpirvLowerPushConst, DEBUG_TYPE,
                "Lower SPIR-V push Constant", false, false)
