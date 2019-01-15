/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLower.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLower.
 ***********************************************************************************************************************
 */

#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/FunctionAttrs.h"
#include "llvm/Transforms/IPO/InferFunctionAttrs.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/InstSimplifyPass.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Vectorize.h"

#include "llpcContext.h"
#include "llpcInternal.h"
#include "llpcPassLoopInfoCollect.h"
#include "llpcPassManager.h"
#include "llpcSpirvLower.h"

#define DEBUG_TYPE "llpc-spirv-lower"

using namespace llvm;

namespace llvm
{

namespace cl
{

// -lower-dyn-index: lower SPIR-V dynamic (non-constant) index in access chain
static opt<bool> LowerDynIndex("lower-dyn-index", desc("Lower SPIR-V dynamic (non-constant) index in access chain"));

// -disable-lower-opt: disable optimization for SPIR-V lowering
static opt<bool> DisableLowerOpt("disable-lower-opt", desc("Disable optimization for SPIR-V lowering"));

} // cl

} // llvm

namespace Llpc
{

// =====================================================================================================================
// Add whole-pipeline lowering passes to pass manager
void SpirvLower::AddPasses(
    Context*              pContext,               // [in] LLPC context
    legacy::PassManager&  passMgr,                // [in/out] Pass manager to add passes to
    uint32_t              forceLoopUnrollCount,   // 0 or force loop unroll count
    bool*                 pNeedDynamicLoopUnroll) // nullptr or where to store flag of whether dynamic loop unrolling
                                                  //  is needed
{
    // Check if this module needs dynamic loop unroll. Only do this check when caller has passed
    // in pNeedDynamicLoopUnroll.
    if (pNeedDynamicLoopUnroll != nullptr)
    {
        passMgr.add(new PassLoopInfoCollect(pNeedDynamicLoopUnroll));
    }

    // Lower SPIR-V resource collecting
    passMgr.add(CreateSpirvLowerResourceCollect());

    // Link external native library for constant folding
    passMgr.add(CreatePassExternalLibLink(true)); // Native only
    passMgr.add(CreatePassDeadFuncRemove());

    // Function inlining
    passMgr.add(createFunctionInliningPass(InlineThreshold));
    passMgr.add(CreatePassDeadFuncRemove());

    // Control loop unrolling
    passMgr.add(CreateSpirvLowerLoopUnrollControl(forceLoopUnrollCount));

    // Lower SPIR-V access chain
    passMgr.add(CreateSpirvLowerAccessChain());

    // Lower SPIR-V buffer operations (load and store)
    passMgr.add(CreateSpirvLowerBufferOp());

    // Remove constant exprs that involve global variables that will be lowered by the next pass.
    passMgr.add(CreateSpirvLowerGlobalConstExprRemove());

    // Lower SPIR-V global variables, inputs, and outputs
    passMgr.add(CreateSpirvLowerGlobal());

    // Lower SPIR-V constant immediate store.
    passMgr.add(CreateSpirvLowerConstImmediateStore());

    // Lower SPIR-V algebraic transforms, constant folding must be done before istruction combining pass.
    passMgr.add(CreateSpirvLowerAlgebraTransform(true, false));

    // Remove reduant load/store operations and do minimal optimization
    // It is required by SpirvLowerImageOp.
    passMgr.add(createSROAPass());
    passMgr.add(createGlobalOptimizerPass());
    passMgr.add(createGlobalDCEPass());
    passMgr.add(createPromoteMemoryToRegisterPass());
    passMgr.add(createAggressiveDCEPass());
    passMgr.add(createInstructionCombiningPass(false));
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createSROAPass());
    passMgr.add(createEarlyCSEPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createIPConstantPropagationPass());

    // Lower SPIR-V algebraic transforms
    passMgr.add(CreateSpirvLowerAlgebraTransform(false, true));

    // Lower SPIR-V push constant load
    passMgr.add(CreateSpirvLowerPushConst());

    // Lower SPIR-V image operations (sample, fetch, gather, read/write),
    passMgr.add(CreateSpirvLowerImageOp());

    // Lower SPIR-V dynamic index in access chain
    if (cl::LowerDynIndex)
    {
        passMgr.add(CreateSpirvLowerDynIndex());
    }

    // Lower SPIR-V load/store operations on aggregate type
    passMgr.add(CreateSpirvLowerAggregateLoadStore());

    // Lower SPIR-V instruction metadata remove
    passMgr.add(CreateSpirvLowerInstMetaRemove());

    // Dump the result
    if (EnableOuts())
    {
        passMgr.add(createPrintModulePass(outs(),
                    "===============================================================================\n"
                    "// LLPC pipeline SPIR-V lowering results\n"));
    }
}

// =====================================================================================================================
// Initializes the pass according to the specified module.
//
// NOTE: This function should be called at the beginning of "runOnModule()".
void SpirvLower::Init(
    Module* pModule) // [in] LLVM module
{
    m_pModule  = pModule;
    m_pContext = static_cast<Context*>(&m_pModule->getContext());
    m_shaderStage = ShaderStageInvalid;
    m_pEntryPoint = nullptr;
}

} // Llpc
