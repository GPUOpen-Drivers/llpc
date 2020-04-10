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

namespace llvm
{

namespace cl
{

extern opt<bool> DisableLicm;

} // cl

} // llvm

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
    unsigned forceLoopUnrollCount)    // Force loop unroll count
{
    auto pPass = new SpirvLowerLoopUnrollControl(forceLoopUnrollCount);
    return pPass;
}

// =====================================================================================================================
SpirvLowerLoopUnrollControl::SpirvLowerLoopUnrollControl()
    :
    SpirvLower(ID),
    m_forceLoopUnrollCount(0),
    m_disableLicm(false)
{
}

// =====================================================================================================================
SpirvLowerLoopUnrollControl::SpirvLowerLoopUnrollControl(
    unsigned forceLoopUnrollCount)    // Force loop unroll count
    :
    SpirvLower(ID),
    m_forceLoopUnrollCount(forceLoopUnrollCount),
    m_disableLicm(false)
{
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerLoopUnrollControl::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Unroll-Control\n");

    SpirvLower::Init(&module);

    if (m_pContext->GetPipelineContext() != nullptr)
    {
        auto pShaderOptions = &(m_pContext->GetPipelineShaderInfo(m_shaderStage)->options);
        if (pShaderOptions->forceLoopUnrollCount > 0)
        {
            m_forceLoopUnrollCount = pShaderOptions->forceLoopUnrollCount;
        }
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 35
        m_disableLicm = pShaderOptions->disableLicm | llvm::cl::DisableLicm;
#endif
    }

    if ((m_forceLoopUnrollCount == 0) && (m_disableLicm == false))
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
            if ((pLoopMetaNode == nullptr) ||
                (pLoopMetaNode->getOperand(0) != pLoopMetaNode) ||
                ((pLoopMetaNode->getNumOperands() != 1) && (m_disableLicm == false)))
            {
                continue;
            }

            if (m_forceLoopUnrollCount && (pLoopMetaNode->getNumOperands() <= 1))
            {
                // We have a loop backedge with !llvm.loop metadata containing just
                // one operand pointing to itself, meaning that the SPIR-V did not
                // have an unroll or don't-unroll directive, so we can add the force
                // unroll count metadata.
                llvm::Metadata* unrollCountMeta[] = {
                    MDString::get(*m_pContext, "llvm.loop.unroll.count"),
                    ConstantAsMetadata::get(
                        ConstantInt::get(Type::getInt32Ty(*m_pContext), m_forceLoopUnrollCount))
                };
                MDNode* pLoopUnrollCountMetaNode = MDNode::get(*m_pContext, unrollCountMeta);
                pLoopMetaNode = MDNode::concatenate(pLoopMetaNode,
                                                    MDNode::get(*m_pContext, pLoopUnrollCountMetaNode));

            }
            if (m_disableLicm)
            {
                MDNode* pLicmDisableNode = MDNode::get(*m_pContext,
                                                       MDString::get(*m_pContext, "llvm.licm.disable"));
                pLoopMetaNode = MDNode::concatenate(pLoopMetaNode,
                                                    MDNode::get(*m_pContext, pLicmDisableNode));
            }
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
