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
 * @file  llpcSpirvLowerAlgebraTransform.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerAlgebraTransform.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

#include "llpcSpirvLower.h"

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of SPIR-V lowering opertions for algebraic transformation.
class SpirvLowerAlgebraTransform:
    public SpirvLower,
    public llvm::InstVisitor<SpirvLowerAlgebraTransform>
{
public:
    SpirvLowerAlgebraTransform(bool enableConstFolding = true, bool enableFloatOpt = true);

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const
    {
        analysisUsage.addRequired<llvm::TargetLibraryInfoWrapperPass>();
    }

    virtual bool runOnModule(llvm::Module& module);
    virtual void visitBinaryOperator(llvm::BinaryOperator& binaryOp);
    virtual void visitCallInst(llvm::CallInst& callInst);
    virtual void visitFPTruncInst(llvm::FPTruncInst& fptruncInst);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    SpirvLowerAlgebraTransform(const SpirvLowerAlgebraTransform&) = delete;
    SpirvLowerAlgebraTransform& operator=(const SpirvLowerAlgebraTransform&) = delete;

    bool IsOperandNoContract(llvm::Value* pOperand);
    void DisableFastMath(Value* pValue);

    bool m_enableConstFolding; // Whether enable constant folding in this pass
    bool m_enableFloatOpt;     // Whether enable floating point optimization in this pass
    bool m_changed;  // Whether the module is changed
    bool m_fp16DenormFlush;    // Whether FP mode wants f16 denorms to be flushed to zero
    bool m_fp32DenormFlush;    // Whether FP mode wants f32 denorms to be flushed to zero
    bool m_fp64DenormFlush;    // Whether FP mode wants f64 denorms to be flushed to zero
    bool m_fp16Rtz;            // Whether FP mode wants f16 round-to-zero
};

} // Llpc
