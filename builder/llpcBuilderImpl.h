/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderImpl.h
 * @brief LLPC header file: declaration of Llpc::Builder implementation classes
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcBuilder.h"

namespace Llpc
{

class Context;

// =====================================================================================================================
// Builder implementation base class
class BuilderImplBase : virtual public Builder
{
public:
    BuilderImplBase(llvm::LLVMContext& context) : Builder(context) {}

    // Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
    Llpc::Context &getContext() const;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplBase)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplBase)
};

// =====================================================================================================================
// Builder implementation subclass for misc. operations
class BuilderImplMisc : public BuilderImplBase
{
public:
    BuilderImplMisc(llvm::LLVMContext& context) : Builder(context), BuilderImplBase(context) {}

    // Create a "kill". Only allowed in a fragment shader.
    llvm::Instruction* CreateKill(const llvm::Twine& instName = "") override;

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImplMisc)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImplMisc)
};

// =====================================================================================================================
// The Builder implementation, encompassing all the individual builder implementation subclasses
class BuilderImpl : public BuilderImplMisc
{
public:
    BuilderImpl(llvm::LLVMContext& context) : Builder(context), BuilderImplMisc(context) {}
    ~BuilderImpl() {}

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderImpl)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderImpl)
};

} // Llpc
