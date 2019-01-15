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
 * @file  llpcPatchBufferOp.h
 * @brief LLPC header file: contains declaration of class Llpc::PatchBufferOp.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include <unordered_set>
#include "llpcPatch.h"

namespace Llpc
{

class PipelineShaders;

// =====================================================================================================================
// Represents the pass of LLVM patching operations for buffer operations
class PatchBufferOp:
    public Patch,
    public llvm::InstVisitor<PatchBufferOp>
{
public:
    PatchBufferOp();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
        analysisUsage.addPreserved<PipelineShaders>();
    }

    virtual bool runOnModule(llvm::Module& module) override;
    virtual void visitCallInst(llvm::CallInst& callInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchBufferOp);

    bool IsInlineConst(uint32_t descSet, uint32_t binding);
    void ReplaceCallee(llvm::CallInst* pCallInst, const char* pOrigNamePrefix, const char* pNewNamePrefix);

    // -----------------------------------------------------------------------------------------------------------------

    bool                                m_changed;       // Whether the pass has modified code so far
    std::unordered_set<llvm::CallInst*> m_replacedCalls;
};

} // Llpc
