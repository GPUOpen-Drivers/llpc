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
#include "llpcSpirvLowerLoopUnrollControl.h"
#include "SPIRVInternal.h"
#include "llpcContext.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/Debug.h"
#include <vector>

#define DEBUG_TYPE "llpc-spirv-lower-loop-unroll-control"

namespace llvm {

namespace cl {

extern opt<bool> DisableLicm;

} // namespace cl

} // namespace llvm

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc {

// =====================================================================================================================
// Initializes static members.
char SpirvLowerLoopUnrollControl::ID = 0;

// =====================================================================================================================
// Pass creator, creates the pass of SPIR-V lowering operations for loop unroll control
//
// @param forceLoopUnrollCount : Force loop unroll count
ModulePass *createSpirvLowerLoopUnrollControl(unsigned forceLoopUnrollCount) {
  auto pass = new SpirvLowerLoopUnrollControl(forceLoopUnrollCount);
  return pass;
}

// =====================================================================================================================
SpirvLowerLoopUnrollControl::SpirvLowerLoopUnrollControl()
    : SpirvLower(ID), m_forceLoopUnrollCount(0), m_disableLicm(false) {
}

// =====================================================================================================================
//
// @param forceLoopUnrollCount : Force loop unroll count
SpirvLowerLoopUnrollControl::SpirvLowerLoopUnrollControl(unsigned forceLoopUnrollCount)
    : SpirvLower(ID), m_forceLoopUnrollCount(forceLoopUnrollCount), m_disableLicm(false) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerLoopUnrollControl::runOnModule(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Unroll-Control\n");

  SpirvLower::init(&module);

  if (m_context->getPipelineContext()) {
    auto shaderOptions = &(m_context->getPipelineShaderInfo(m_shaderStage)->options);
    if (shaderOptions->forceLoopUnrollCount > 0)
      m_forceLoopUnrollCount = shaderOptions->forceLoopUnrollCount;
#if LLPC_CLIENT_INTERFACE_MAJOR_VERSION >= 35
    m_disableLicm = shaderOptions->disableLicm | llvm::cl::DisableLicm;
#endif
  }

  if (m_forceLoopUnrollCount == 0 && !m_disableLicm)
    return false;

  if (m_shaderStage == ShaderStageTessControl || m_shaderStage == ShaderStageTessEval ||
      m_shaderStage == ShaderStageGeometry) {
    // Disabled on above shader types.
    return false;
  }

  bool changed = false;
  for (auto &func : module) {
    for (auto &block : func) {
      auto terminator = block.getTerminator();
      MDNode *loopMetaNode = terminator->getMetadata("llvm.loop");
      if (!loopMetaNode || loopMetaNode->getOperand(0) != loopMetaNode ||
          (loopMetaNode->getNumOperands() != 1 && !m_disableLicm))
        continue;

      if (m_forceLoopUnrollCount && loopMetaNode->getNumOperands() <= 1) {
        // We have a loop backedge with !llvm.loop metadata containing just
        // one operand pointing to itself, meaning that the SPIR-V did not
        // have an unroll or don't-unroll directive, so we can add the force
        // unroll count metadata.
        llvm::Metadata *unrollCountMeta[] = {
            MDString::get(*m_context, "llvm.loop.unroll.count"),
            ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*m_context), m_forceLoopUnrollCount))};
        MDNode *loopUnrollCountMetaNode = MDNode::get(*m_context, unrollCountMeta);
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, loopUnrollCountMetaNode));
      }
      if (m_disableLicm) {
        MDNode *licmDisableNode = MDNode::get(*m_context, MDString::get(*m_context, "llvm.licm.disable"));
        loopMetaNode = MDNode::concatenate(loopMetaNode, MDNode::get(*m_context, licmDisableNode));
      }
      loopMetaNode->replaceOperandWith(0, loopMetaNode);
      terminator->setMetadata("llvm.loop", loopMetaNode);
      changed = true;
    }
  }

  return changed;
}

} // namespace Llpc

// =====================================================================================================================
// Initializes the pass of SPIR-V lowering operations for loop unroll control.
INITIALIZE_PASS(SpirvLowerLoopUnrollControl, "Spirv-lower-loop-unroll-control",
                "Set metadata to control LLPC loop unrolling", false, false)
