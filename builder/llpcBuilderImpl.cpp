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
 * @file  llpcBuilderImpl.cpp
 * @brief LLPC source file: implementation of Llpc::BuilderImpl
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"
#include "llpcContext.h"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Get the LLPC context. This overrides the IRBuilder method that gets the LLVM context.
Context& BuilderImplBase::getContext() const
{
    return *static_cast<Llpc::Context*>(&Builder::getContext());
}

// =====================================================================================================================
// Get whether the context we are building in supports DPP operations.
bool BuilderImplBase::SupportDpp() const
{
    return getContext().GetGfxIpVersion().major >= 8;
}

// =====================================================================================================================
// Get whether the context we are building in support the bpermute operation.
bool BuilderImplBase::SupportBPermute() const
{
    auto gfxIp = getContext().GetGfxIpVersion().major;
    auto supportBPermute = (gfxIp == 8) || (gfxIp == 9);
#if LLPC_BUILD_GFX10
    auto waveSize = getContext().GetShaderWaveSize(GetShaderStageFromFunction(GetInsertBlock()->getParent()));
    supportBPermute = supportBPermute || ((gfxIp == 10) && (waveSize == 32));
#endif
    return supportBPermute;
}

#if LLPC_BUILD_GFX10
// =====================================================================================================================
// Get whether the context we are building in supports permute lane DPP operations.
bool BuilderImplBase::SupportPermLaneDpp() const
{
    return getContext().GetGfxIpVersion().major >= 10;
}
#endif
