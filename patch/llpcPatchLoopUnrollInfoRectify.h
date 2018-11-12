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
 * @file  llpcPatchLoopUnrollInfoRectify.h
 * @brief LLPC header file: contains declaration of class Llpc::PatchLoopUnrollInfoRectify.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/Pass.h"

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcInternal.h"

namespace llvm
{

class PassRegistry;
void initializePatchLoopUnrollInfoRectifyPass(PassRegistry&);

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of LLVM patching operations for rectifying loop unroll information.
class PatchLoopUnrollInfoRectify final:
    public llvm::FunctionPass
{
public:
    explicit PatchLoopUnrollInfoRectify();

    bool runOnFunction(llvm::Function& function) override;

    void getAnalysisUsage(llvm::AnalysisUsage& AU) const override;

    // Pass creator, creates the pass of LLVM patching operations for rectifying unroll information.
    static llvm::FunctionPass* Create() { return new PatchLoopUnrollInfoRectify(); }

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchLoopUnrollInfoRectify);

    static constexpr uint32_t MaxLoopUnrollCount = 32;
};

} // Llpc
