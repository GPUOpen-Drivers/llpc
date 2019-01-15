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
 * @file  llpcSpirvLowerPushConst.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerPushConst.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Analysis/LoopPass.h"

#include "llpcSpirvLower.h"

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of SPIR-V lowering opertions for push constant.
class SpirvLowerPushConst:
    public llvm::FunctionPass
{
public:
    SpirvLowerPushConst();

    virtual bool runOnFunction(llvm::Function& func) override;

    // Gets loop analysis usage
    virtual void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(SpirvLowerPushConst);

    void HandleLoop(llvm::Loop* pLoop, Instruction* pInsertPos);

    bool                            m_changed;           // Whether the pass has made any modifications
    // Push constant load map, from <component count, load offset> to load call
    std::map<uint32_t, CallInst*>   m_pushConstLoadMap;
};

} // Llpc
