/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
* @file  llpcPatchSetupTargetFeatures.cpp
* @brief LLPC source file: contains declaration and implementation of class lgc::PatchSetupTargetFeatures.
***********************************************************************************************************************
*/
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#include "llpcCodeGenManager.h"
#include "llpcPatch.h"
#include "llpcPipelineState.h"

#define DEBUG_TYPE "llpc-patch-setup-target-features"

using namespace llvm;
using namespace lgc;

namespace lgc
{

// =====================================================================================================================
// Pass to set up target features on shader entry-points
class PatchSetupTargetFeatures : public Patch
{
public:
    static char ID;
    PatchSetupTargetFeatures() : Patch(ID)
    {
    }

    void getAnalysisUsage(AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineStateWrapper>();
    }

    bool runOnModule(Module& module) override;

private:
    PatchSetupTargetFeatures(const PatchSetupTargetFeatures&) = delete;
    PatchSetupTargetFeatures& operator=(const PatchSetupTargetFeatures&) = delete;
};

char PatchSetupTargetFeatures::ID = 0;

} // lgc

// =====================================================================================================================
// Create pass to set up target features
ModulePass* lgc::CreatePatchSetupTargetFeatures()
{
    return new PatchSetupTargetFeatures();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool PatchSetupTargetFeatures::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Setup-Target-Features\n");

    Patch::Init(&module);

    auto pPipelineState = getAnalysis<PipelineStateWrapper>().GetPipelineState(&module);
    CodeGenManager::SetupTargetFeatures(pPipelineState, &module);

    return true; // Modified the module.
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchSetupTargetFeatures, DEBUG_TYPE, "Patch LLVM to set up target features", false, false)

