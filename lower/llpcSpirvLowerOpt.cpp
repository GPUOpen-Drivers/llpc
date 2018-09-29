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
 * @file  llpcSpirvLowerOpt.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerOpt.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-opt"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"

#include "SPIRVInternal.h"
#include "llpcSpirvLowerOpt.h"
#include "llpcSpirvLowerLoopUnrollInfoRectify.h"
#include "llpcSpirvLowerPeepholeOpt.h"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

extern TimeProfileResult g_timeProfileResult;

// =====================================================================================================================
// Initializes static members.
char SpirvLowerOpt::ID = 0;

// =====================================================================================================================
SpirvLowerOpt::SpirvLowerOpt()
    :
    SpirvLower(ID)
{
    initializeSpirvLowerOptPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerOpt::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    TimeProfiler timeProfiler(&g_timeProfileResult.lowerOptTime);

    bool changed = false;

    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Opt\n");

    SpirvLower::Init(&module);

    // Set up standard optimization passes.
    // NOTE: Doing this here is temporary; really the whole of LLPC should be using the
    // PassManagerBuilder mechanism, adding its own passes at the provided hook points.
    legacy::PassManager passMgr;
    PassManagerBuilder passBuilder;
    passBuilder.OptLevel = 3; // -O3

    passBuilder.addExtension(PassManagerBuilder::EP_Peephole,
        [](const PassManagerBuilder&, legacy::PassManagerBase& passMgr)
        {
            passMgr.add(SpirvLowerPeepholeOpt::Create());
            passMgr.add(createInstSimplifyLegacyPass());
        });
    passBuilder.addExtension(PassManagerBuilder::EP_LoopOptimizerEnd,
        [](const PassManagerBuilder&, legacy::PassManagerBase& passMgr)
        {
            // We run our peephole pass just before the scalarizer to ensure that our simplification optimizations are
            // performed before the scalarizer. One important case this helps with is when you have bit casts whose
            // source is a PHI - we want to make sure that the PHI does not have an i8 type before the scalarizer is
            // called, otherwise a different kind of PHI mess is generated.
            passMgr.add(SpirvLowerPeepholeOpt::Create());

            // Run the scalarizer as it helps our register pressure in the backend significantly. The scalarizer allows
            // us to much more easily identify dead parts of vectors that we do not need to do any computation for.
            passMgr.add(createScalarizerPass());

            // We add an extra inst simplify here to make sure that dead PHI nodes that are easily identified post
            // running the scalarizer can be folded away before instruction combining tries to re-create them.
            passMgr.add(createInstSimplifyLegacyPass());
        });
    passBuilder.addExtension(PassManagerBuilder::EP_LateLoopOptimizations,
        [](const PassManagerBuilder&, legacy::PassManagerBase& passMgr)
        {
            passMgr.add(SpirvLowerLoopUnrollInfoRectify::Create());
        });

    passBuilder.populateModulePassManager(passMgr);

    // Run the other passes.
    passMgr.run(module);

    return true;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of general optimizations for SPIR-V lowering.
INITIALIZE_PASS(SpirvLowerOpt, "Spirv-lower-opt",
                "Lower SPIR-V with general optimizations", false, false)
