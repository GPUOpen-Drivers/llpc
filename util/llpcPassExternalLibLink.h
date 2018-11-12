/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2018 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PassExternalLibLink.h
 * @brief LLPC header file: contains declaration of class Llpc::PassExternalLibLink.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include "llvm/Pass.h"

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcInternal.h"

namespace llvm
{

class PassRegistry;
void initializePassExternalLibLinkPass(PassRegistry&);

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of linking external library of LLVM IR.
class PassExternalLibLink
    :
    public llvm::ModulePass
{
public:
    PassExternalLibLink(bool nativeOnly = false);

    virtual bool runOnModule(llvm::Module& module);

    // Pass creator, creates the LLVM pass for linking external library of LLVM IR
    static llvm::ModulePass* Create(bool nativeOnly) { return new PassExternalLibLink(nativeOnly); }

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PassExternalLibLink);

    bool m_nativeOnly;  // Whether to only link native functions
};

} // Llpc
