/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
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

#include "llpcBuilder.h"
#include "llpcContext.h"
#include "llpcDebug.h"
#include "llpcInternal.h"
#include "llpcPassManager.h"
#include "llpcSpirvLower.h"

#define DEBUG_TYPE "llpc-spirv-lower"

using namespace llvm;

namespace Llpc
{
// =====================================================================================================================
// Replace a constant with instructions using a builder.
void SpirvLower::ReplaceConstWithInsts(
    Context*        pContext,      // [in] The context
    Constant* const pConst)        // [in/out] The constant to replace with instructions.

{
    SmallSet<Constant*, 8> otherConsts;
    Builder* pBuilder = pContext->GetBuilder();
    for (User* const pUser : pConst->users())
    {
        if (Constant* const pOtherConst = dyn_cast<Constant>(pUser))
        {
            otherConsts.insert(pOtherConst);
        }
    }

    for (Constant* const pOtherConst : otherConsts)
    {
        ReplaceConstWithInsts(pContext, pOtherConst);
    }

    otherConsts.clear();

    SmallVector<Value*, 8> users;

    for (User* const pUser : pConst->users())
    {
        users.push_back(pUser);
    }

    for (Value* const pUser : users)
    {
        Instruction* const pInst = dyn_cast<Instruction>(pUser);
        assert(pInst != nullptr);

        // If the instruction is a phi node, we have to insert the new instructions in the correct predecessor.
        if (PHINode* const pPhiNode = dyn_cast<PHINode>(pInst))
        {
            const uint32_t incomingValueCount = pPhiNode->getNumIncomingValues();
            for (uint32_t i = 0; i < incomingValueCount; i++)
            {
                if (pPhiNode->getIncomingValue(i) == pConst)
                {
                    pBuilder->SetInsertPoint(pPhiNode->getIncomingBlock(i)->getTerminator());
                    break;
                }
            }
        }
        else
        {
            pBuilder->SetInsertPoint(pInst);
        }

        if (ConstantExpr* const pConstExpr = dyn_cast<ConstantExpr>(pConst))
        {
            Instruction* const pInsertPos = pBuilder->Insert(pConstExpr->getAsInstruction());
            pInst->replaceUsesOfWith(pConstExpr, pInsertPos);
        }
        else if (ConstantVector* const pConstVector = dyn_cast<ConstantVector>(pConst))
        {
            Value* pResultValue = UndefValue::get(pConstVector->getType());
            for (uint32_t i = 0; i < pConstVector->getNumOperands(); i++)
            {
                // Have to not use the builder here because it will constant fold and we are trying to undo that now!
                Instruction* const pInsertPos = InsertElementInst::Create(pResultValue,
                    pConstVector->getOperand(i),
                    pBuilder->getInt32(i));
                pResultValue = pBuilder->Insert(pInsertPos);
            }
            pInst->replaceUsesOfWith(pConstVector, pResultValue);
        }
        else
        {
            llvm_unreachable("Should never be called!");
        }
    }

    pConst->removeDeadConstantUsers();
    pConst->destroyConstant();
}

// =====================================================================================================================
// Removes those constant expressions that reference global variables.
void SpirvLower::RemoveConstantExpr(
    Context*        pContext,   // [in] The context
    GlobalVariable* pGlobal)    // [in] The global variable
{
    SmallVector<Constant*, 8> constantUsers;

    for (User* const pUser : pGlobal->users())
    {
        if (Constant* const pConst = dyn_cast<Constant>(pUser))
        {
            constantUsers.push_back(pConst);
        }
    }

    for (Constant* const pConst : constantUsers)
    {
        ReplaceConstWithInsts(pContext, pConst);
    }
}

// =====================================================================================================================
// Add per-shader lowering passes to pass manager
void SpirvLower::AddPasses(
    Context*              pContext,               // [in] LLPC context
    ShaderStage           stage,                  // Shader stage
    legacy::PassManager&  passMgr,                // [in/out] Pass manager to add passes to
    llvm::Timer*          pLowerTimer,            // [in] Timer to time lower passes with, nullptr if not timing
    uint32_t              forceLoopUnrollCount)   // 0 or force loop unroll count
{
    // Manually add a target-aware TLI pass, so optimizations do not think that we have library functions.
    pContext->GetBuilderContext()->PreparePassManager(&passMgr);

    // Start timer for lowering passes.
    if (pLowerTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pLowerTimer, true));
    }

    // Lower SPIR-V resource collecting
    passMgr.add(CreateSpirvLowerResourceCollect(false));

    // Function inlining. Use the "always inline" pass, since we want to inline all functions, and
    // we marked (non-entrypoint) functions as "always inline" just after SPIR-V reading.
    passMgr.add(createAlwaysInlinerLegacyPass());
    passMgr.add(createGlobalDCEPass());

    // Control loop unrolling
    passMgr.add(CreateSpirvLowerLoopUnrollControl(forceLoopUnrollCount));

    // Lower SPIR-V access chain
    passMgr.add(CreateSpirvLowerAccessChain());

    // Lower SPIR-V global variables, inputs, and outputs
    passMgr.add(CreateSpirvLowerGlobal());

    // Lower SPIR-V constant immediate store.
    passMgr.add(CreateSpirvLowerConstImmediateStore());

    // Lower SPIR-V algebraic transforms, constant folding must be done before instruction combining pass.
    passMgr.add(CreateSpirvLowerAlgebraTransform(true, false));

    // Lower SPIR-V memory operations
    passMgr.add(CreateSpirvLowerMemoryOp());

    // Remove reduant load/store operations and do minimal optimization
    // It is required by SpirvLowerImageOp.
    passMgr.add(createSROAPass());
    passMgr.add(createGlobalOptimizerPass());
    passMgr.add(createGlobalDCEPass());
    passMgr.add(createPromoteMemoryToRegisterPass());
    passMgr.add(createAggressiveDCEPass());
    passMgr.add(createInstructionCombiningPass(false, 3));
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createSROAPass());
    passMgr.add(createEarlyCSEPass());
    passMgr.add(createCFGSimplificationPass());
    passMgr.add(createIPConstantPropagationPass());

    // Lower SPIR-V algebraic transforms
    passMgr.add(CreateSpirvLowerAlgebraTransform(false, true));

    // Lower SPIR-V instruction metadata remove
    passMgr.add(CreateSpirvLowerInstMetaRemove());

    // Stop timer for lowering passes.
    if (pLowerTimer != nullptr)
    {
        passMgr.add(CreateStartStopTimer(pLowerTimer, false));
    }

    // Dump the result
    if (EnableOuts())
    {
        passMgr.add(createPrintModulePass(outs(), "\n"
                    "===============================================================================\n"
                    "// LLPC SPIR-V lowering results\n"));
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
    if (m_pModule->empty())
    {
        m_shaderStage = ShaderStageInvalid;
        m_pEntryPoint = nullptr;
    }
    else
    {
        m_shaderStage = GetShaderStageFromModule(m_pModule);
        m_pEntryPoint = GetEntryPoint(m_pModule);
    }
    m_pBuilder = m_pContext->GetBuilder();
}

} // Llpc
