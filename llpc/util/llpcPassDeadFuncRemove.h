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
 * @file  llpcPassDeadFuncRemove.h
 * @brief LLPC header file: contains declaration of class Llpc::PassDeadFuncRemove.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/InstVisitor.h"

#include "llvm/Pass.h"

#include "llpc.h"
#include "llpcDebug.h"
#include "llpcInternal.h"

namespace Llpc
{

// =====================================================================================================================
// Represents the LLVM pass for dead (uncalled) function removal.
class PassDeadFuncRemove:
    public llvm::ModulePass,
    public llvm::InstVisitor<PassDeadFuncRemove>
{
public:
    PassDeadFuncRemove();

    virtual bool runOnModule(llvm::Module& module);

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    PassDeadFuncRemove(const PassDeadFuncRemove&) = delete;
    PassDeadFuncRemove& operator =(const PassDeadFuncRemove&) = delete;

    static const uint32_t MaxIterCountOfDetection = 16; // Maximum iteration count in dead function detection
};

} // Llpc
