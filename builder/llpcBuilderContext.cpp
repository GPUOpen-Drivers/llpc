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
 * @file  llpcBuilderContext.cpp
 * @brief LLPC source file: implementation of llpc::BuilderContext class for creating and using Llpc::Builder
 ***********************************************************************************************************************
 */
#include "llpcBuilderContext.h"
#include "llpcBuilderImpl.h"
#include "llpcBuilderRecorder.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
BuilderContext::BuilderContext(
    LLVMContext&  context,              // [in] LLVM context to give each Builder
    bool          useBuilderRecorder)   // True to use BuilderRecorder, false to build directly
    : m_context(context), m_useBuilderRecorder(useBuilderRecorder)
{
}

// =====================================================================================================================
// Create a Builder object
Builder* BuilderContext::CreateBuilder()
{
    if (m_useBuilderRecorder == false)
    {
        // Generate LLVM IR directly without recording
        return new BuilderImpl(this);
    }
    // Record with BuilderRecorder
    return new BuilderRecorder(this);
}

// =====================================================================================================================
// Create a BuilderImpl object directly, passing in the PipelineState to use.
Builder* BuilderContext::CreateBuilderImpl(
    PipelineState*  pPipelineState)   // [in] PipelineState to use
{
    // Generate LLVM IR directly without recording
    BuilderImpl* pBuilderImpl = new BuilderImpl(this);
    pBuilderImpl->SetPipelineState(pPipelineState);
    return pBuilderImpl;
}

