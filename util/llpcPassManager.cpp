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
 * @file  llpcPassManager.cpp
 * @brief LLPC source file: legacy::PassManager override
 ***********************************************************************************************************************
 */
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"

#include "llpcPassManager.h"

namespace llvm
{
namespace cl
{

// -verify-ir : verify the IR after each pass
static cl::opt<bool> VerifyIr("verify-ir",
                              cl::desc("Verify IR after each pass"),
                              cl::init(false));

} // cl

} // llvm

using namespace llvm;
using namespace Llpc;

// =====================================================================================================================
// Add a pass to the pass manager.
void Llpc::PassManager::add(
    Pass* pPass)    // [in] Pass to add to the pass manager
{
    // Add the pass to the superclass pass manager.
    legacy::PassManager::add(pPass);

    if (cl::VerifyIr)
    {
        // Add a verify pass after it.
        legacy::PassManager::add(createVerifierPass(true)); // FatalErrors=true
    }
}
