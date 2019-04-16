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
 * @file  llpcPassManager.h
 * @brief LLPC header file: contains declaration of class Llpc::PassManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/LegacyPassManager.h"

namespace Llpc
{

// =====================================================================================================================
// LLPC's legacy::PassManager override
class PassManager final :
    public llvm::legacy::PassManager
{
public:
    PassManager(uint32_t* pPassIndex);

    void add(llvm::Pass* pPass) override;
    void stop();

private:
    bool              m_stopped = false;         // Whether we have already stopped adding new passes.
    llvm::AnalysisID  m_dumpCfgAfter = nullptr;  // -dump-cfg-after pass id
    llvm::AnalysisID  m_printModule = nullptr;   // Pass id of dump pass "Print Module IR"
    llvm::AnalysisID  m_jumpThreading = nullptr; // Pass id of opt pass "Jump Threading"
    uint32_t*         m_pPassIndex;              // Pass Index
};

} // Llpc
