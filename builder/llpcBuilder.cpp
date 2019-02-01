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
 * @file  llpcBuilder.cpp
 * @brief LLPC source file: implementation of Llpc::Builder
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"

#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "llpc-builder"

using namespace Llpc;
using namespace llvm;

// -use-builder-recorder
static cl::opt<uint32_t> UseBuilderRecorder("use-builder-recorder",
                                            cl::desc("Do lowering via recording and replaying LLPC builder:\n"
                                                     "0: Generate IR directly; no recording\n"
                                                     "1: Do lowering via recording and replaying LLPC builder (default)\n"
                                                     "2: Do lowering via recording; no replaying"),
                                            cl::init(1));

// =====================================================================================================================
// Create a Builder object
// If -use-builder-recorder is 0, this creates a BuilderImpl. Otherwise, it creates a BuilderRecorder.
Builder* Builder::Create(
    LLVMContext& context) // [in] LLVM context
{
    if (UseBuilderRecorder == 0)
    {
        // -use-builder-recorder=0: generate LLVM IR directly without recording
        return CreateBuilderImpl(context);
    }
    // -use-builder-recorder=1: record with BuilderRecorder and replay with BuilderReplayer
    // -use-builder-recorder=2: record with BuilderRecorder and do not replay
    return CreateBuilderRecorder(context, UseBuilderRecorder == 1 /*wantReplay*/);
}

// =====================================================================================================================
// Create a BuilderImpl object
Builder* Builder::CreateBuilderImpl(
    LLVMContext& context) // [in] LLVM context
{
    return new BuilderImpl(context);
}

// =====================================================================================================================
Builder::~Builder()
{
}

