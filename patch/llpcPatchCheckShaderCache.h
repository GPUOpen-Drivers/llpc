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
 * @file  llpcPatchCheckShaderCache.h
 * @brief LLPC header file: contains declaration of class Llpc::PatchCheckShaderCache
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcPatch.h"
#include "llpcPipelineShaders.h"

namespace Llpc
{

// =====================================================================================================================
// Represents the pass of LLVM patching operations for checking shader cache
class PatchCheckShaderCache:
    public Patch
{
public:
    PatchCheckShaderCache();

    void getAnalysisUsage(llvm::AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
    }

    virtual bool runOnModule(llvm::Module& module) override;

    // Set the callback function that this pass uses to ask the front-end whether it wants to remove
    // any shader stages. The function takes the LLVM IR module and a per-shader-stage array of input/output
    // usage checksums, and it returns the shader stage mask with bits removed for shader stages that it wants
    // removed.
    void SetCallbackFunction(Pipeline::CheckShaderCacheFunc callbackFunc)
    {
        m_callbackFunc = callbackFunc;
    }

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;   // ID of this pass

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchCheckShaderCache);

    // -----------------------------------------------------------------------------------------------------------------

    Pipeline::CheckShaderCacheFunc   m_callbackFunc;
};

} // Llpc
