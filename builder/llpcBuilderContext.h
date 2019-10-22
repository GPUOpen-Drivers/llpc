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
 * @file  llpcBuilderContext.h
 * @brief LLPC header file: declaration of llpc::BuilderContext class for creating and using Llpc::Builder
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcDebug.h"

namespace llvm
{

class LLVMContext;

} // llvm

namespace Llpc
{

using namespace llvm;

class Builder;

// =====================================================================================================================
// BuilderContext class, used to create Builder objects. State shared between Builder objects is kept here.
class BuilderContext
{
public:
    BuilderContext(LLVMContext& context, bool useBuilderRecorder);

    // Get LLVM context
    LLVMContext& GetContext() const { return m_context; }

    // Create a Builder object
    Builder* CreateBuilder();

private:
    LLPC_DISALLOW_DEFAULT_CTOR(BuilderContext)
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderContext)

    // -----------------------------------------------------------------------------------------------------------------
    LLVMContext&  m_context;              // LLVM context
    bool          m_useBuilderRecorder;   // Whether to create BuilderRecorder or BuilderImpl
};

} // Llpc
