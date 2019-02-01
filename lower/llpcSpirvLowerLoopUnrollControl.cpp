/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerLoopUnrollControl.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerLoopUnrollControl.
 ***********************************************************************************************************************
 */
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/Debug.h"

#include <vector>
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llpcSpirvLowerLoopUnrollControl.h"

#define DEBUG_TYPE "llpc-spirv-lower-loop-unroll-control"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Initializes static members.
char SpirvLowerLoopUnrollControl::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for loop unroll control
ModulePass* CreateSpirvLowerLoopUnrollControl(
    uint32_t forceLoopUnrollCount)    // Force loop unroll count
{
    auto pPass = new SpirvLowerLoopUnrollControl(forceLoopUnrollCount);
    return pPass;
}

// =====================================================================================================================
SpirvLowerLoopUnrollControl::SpirvLowerLoopUnrollControl()
    :
    SpirvLower(ID),
    m_forceLoopUnrollCount(0)
{
    initializeSpirvLowerLoopUnrollControlPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
SpirvLowerLoopUnrollControl::SpirvLowerLoopUnrollControl(
    uint32_t forceLoopUnrollCount)    // Force loop unroll count
    :
    SpirvLower(ID),
    m_forceLoopUnrollCount(forceLoopUnrollCount)
{
    initializeSpirvLowerLoopUnrollControlPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerLoopUnrollControl::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Unroll-Control\n");

    SpirvLower::Init(&module);

    if (m_forceLoopUnrollCount == 0)
    {
        return false;
    }

    if ((m_shaderStage == ShaderStageTessControl) ||
        (m_shaderStage == ShaderStageTessEval) ||
        (m_shaderStage == ShaderStageGeometry))
    {
        // Disabled on above shader types.
        return false;
    }

    bool changed = false;
    for (auto& func : module)
    {
        for (auto& block : func)
        {
            auto pTerminator = block.getTerminator();
            MDNode* pLoopMetaNode = pTerminator->getMetadata("llvm.loop");
            if ((pLoopMetaNode == nullptr) || (pLoopMetaNode->getNumOperands() != 1) ||
                (pLoopMetaNode->getOperand(0) != pLoopMetaNode))
            {
                continue;
            }
            // We have a loop backedge with !llvm.loop metadata containing just
            // one operand pointing to itself, meaning that the SPIR-V did not
            // have an unroll or don't-unroll directive.  Add the force-unroll
            // count here.
            auto pTemp = MDNode::getTemporary(*m_pContext, None);
            Metadata* args[] = { pTemp.get() };
            auto pSelf = MDNode::get(*m_pContext, args);
            pSelf->replaceOperandWith(0, pSelf);

            SmallVector<llvm::Metadata*, 2> opValues;
            opValues.push_back(MDString::get(*m_pContext, "llvm.loop.unroll.count"));
            opValues.push_back(ConstantAsMetadata::get(
                  ConstantInt::get(Type::getInt32Ty(*m_pContext), m_forceLoopUnrollCount)));

            SmallVector<Metadata*, 2> metadata;
            metadata.push_back(MDNode::get(*m_pContext, pSelf));
            metadata.push_back(MDNode::get(*m_pContext, opValues));

            pLoopMetaNode = MDNode::get(*m_pContext, metadata);
            pLoopMetaNode->replaceOperandWith(0, pLoopMetaNode);
            pTerminator->setMetadata("llvm.loop", pLoopMetaNode);
            changed = true;
        }
    }

    return changed;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering operations for loop unroll control.
INITIALIZE_PASS(SpirvLowerLoopUnrollControl, "Spirv-lower-loop-unroll-control",
                "Set metadata to control LLPC loop unrolling", false, false)
