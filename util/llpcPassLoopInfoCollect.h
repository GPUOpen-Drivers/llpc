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
 * @file  llpcPassLoopInfoCollect.h
 * @brief LLPC header file: contains declaration of class Llpc::PassLoopInfoCollect.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopPass.h"
#include "llpc.h"
#include "llpcPipelineShaders.h"

namespace llvm
{

class PassRegistry;
void initializePassLoopInfoCollectPass(PassRegistry&);

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Represents the LLVM pass for determining whether dynamic loop unroll is needed
class PassLoopInfoCollect : public llvm::ModulePass
{
public:
    PassLoopInfoCollect() : ModulePass(ID) {}
    PassLoopInfoCollect(bool* pNeedDynamicLoopUnroll);

    // Gets loop analysis usage
    virtual void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override
    {
        analysisUsage.addRequired<llvm::CallGraphWrapperPass>();
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addRequired<llvm::LoopInfoWrapperPass>();
        analysisUsage.setPreservesAll();
    }

    virtual bool runOnModule(llvm::Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PassLoopInfoCollect);

    bool NeedDynamicLoopUnroll(const llvm::Loop* pLoop);

    // -----------------------------------------------------------------------------------------------------------------

    bool*                           m_pNeedDynamicLoopUnroll;   // Pointer to flag that this pass writes to say whether
};

} // Llpc
