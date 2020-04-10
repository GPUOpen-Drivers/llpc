/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcPatchLoadScalarizer.h
 * @brief LLPC header file: contains declaration of class lgc::PatchLoadScalarizer.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include "lgc/llpcBuilder.h"
#include "llpcPatch.h"

namespace lgc
{

// =====================================================================================================================
// Represents the pass of LLVM patching operations for scalarize load.
class PatchLoadScalarizer final:
    public llvm::FunctionPass,
    public llvm::InstVisitor<PatchLoadScalarizer>
{
public:
    explicit PatchLoadScalarizer();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override;
    bool runOnFunction(llvm::Function& function) override;

    void visitLoadInst(llvm::LoadInst& loadInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    PatchLoadScalarizer(const PatchLoadScalarizer&) = delete;
    PatchLoadScalarizer& operator=(const PatchLoadScalarizer&) = delete;

    // -----------------------------------------------------------------------------------------------------------------

    llvm::SmallVector<llvm::Instruction*, 8>        m_instsToErase;         // Instructions to erase
    std::unique_ptr<llvm::IRBuilder<>>              m_builder;             // The IRBuilder.
    unsigned                                        m_scalarThreshold;      // The threshold for load scalarizer
};

} // lgc
