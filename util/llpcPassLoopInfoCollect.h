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
 * @file  llpcPassLoopInfoCollect.h
 * @brief LLPC header file: contains declaration of class Llpc::LoopInfoCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Analysis/LoopPass.h"
#include "llpc.h"

namespace Llpc
{

// Represents the info of loop analysis
struct LoopAnalysisInfo
{
    uint32_t numAluInsts;               // Number of ALU instructions
    uint32_t numBasicBlocks;            // Number of basic blocks
    uint32_t nestedLevel;               // Nested loop level
};

// =====================================================================================================================
// Represents the LLVM pass for loop info collecting.
class LoopInfoCollect : public llvm::ModulePass
{
public:
    LoopInfoCollect() : ModulePass(ID) {};
    LoopInfoCollect(std::vector<LoopAnalysisInfo>* pInfo) : ModulePass(ID)
    {
        m_pLoopInfo = pInfo;
    };

    void HandleLoop(llvm::Loop* loop, uint32_t nestedLevel);

    // Gets loop analysis usage
    virtual void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override
    {
        analysisUsage.addRequired<llvm::LoopInfoWrapperPass>();
        analysisUsage.setPreservesAll();
    }

    virtual bool runOnModule(llvm::Module& module);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    std::vector<LoopAnalysisInfo>*  m_pLoopInfo;
};

} // Llpc
