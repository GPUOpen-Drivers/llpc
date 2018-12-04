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
* @file  llpcPatchAutoLayoutDesc.cpp
* @brief LLPC source file: pass to auto-layout descriptors when amdllpc is compiling shaders outside a pipeline
***********************************************************************************************************************
*/
#include "llvm/Pass.h"

#include "llpcContext.h"
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"

#define DEBUG_TYPE "llpc-patch-auto-layout-desc"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{
// =====================================================================================================================
// Pass to auto-layout descriptors when AMDLLPC is compiling shaders outside a pipeline
class PatchAutoLayoutDesc : public Patch
{
public:
    static char ID;
    PatchAutoLayoutDesc() : Patch(ID)
    {
        initializePipelineShadersPass(*PassRegistry::getPassRegistry());
        initializePatchAutoLayoutDescPass(*PassRegistry::getPassRegistry());
    }

    void getAnalysisUsage(AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
    }

    bool runOnModule(Module& module) override;
};

char PatchAutoLayoutDesc::ID = 0;

} // Llpc

// =====================================================================================================================
// Create the pass
ModulePass* Llpc::CreatePatchAutoLayoutDesc()
{
    return new PatchAutoLayoutDesc();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool PatchAutoLayoutDesc::runOnModule(
    llvm::Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Auto-Layout-Desc\n");

    Patch::Init(&module);

    // Process each shader, in reverse order.
    auto pPipelineShaders = &getAnalysis<PipelineShaders>();
    for (int32_t stage = ShaderStageCountInternal - 1; stage >= 0; --stage)
    {
        auto pEntryPoint = pPipelineShaders->GetEntryPoint(ShaderStage(stage));
        if (pEntryPoint != nullptr)
        {
            m_pContext->AutoLayoutDescriptor(ShaderStage(stage));
        }
    }

    return false; // Did not modify the module.
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchAutoLayoutDesc, DEBUG_TYPE, "Patch LLVM for descriptor auto layout", false, false)

