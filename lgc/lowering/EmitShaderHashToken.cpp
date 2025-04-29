/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  EmitShaderHashToken.cpp
* @brief LLPC source file: contains implementation of class lgc::EmitShaderHashToken.
***********************************************************************************************************************
*/
#include "lgc/lowering/EmitShaderHashToken.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "emit-shader-hash-token"

using namespace llvm;
using namespace lgc;

namespace lgc {
// =====================================================================================================================
// Executes this LGC lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses EmitShaderHashToken::run(Module &module, ModuleAnalysisManager &analysisManager) {
  m_PipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();

  static const auto visitor = llvm_dialects::VisitorBuilder<EmitShaderHashToken>()
                                  .setStrategy(llvm_dialects::VisitorStrategy::ByInstruction)
                                  .add(&EmitShaderHashToken::visitEmitShaderHashToken)
                                  .build();

  visitor.visit(*this, module);

  return PreservedAnalyses::all();
}

void EmitShaderHashToken::visitEmitShaderHashToken(EmitShaderHashTokenOp &op) {
  BuilderImpl builder(m_PipelineState);

  builder.SetInsertPoint(&op);

  // Options::hash must have been set to the internal pipeline hash during compilation,
  // the upper portion of which is the unique, per-shader hash which we want to emit.
  uint64_t shaderHash = m_PipelineState->getOptions().hash[1];

  // Highly unlikely to be 0x0, unless there was a bug.
  assert(m_PipelineState->getOptions().hash != 0);

  // The convention is to emit the MSB first, followed by LSB
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_ttracedata, {},
                          builder.getInt32(static_cast<uint32_t>((shaderHash >> 32) & 0xffffffff)));
  builder.CreateIntrinsic(Intrinsic::amdgcn_s_ttracedata, {},
                          builder.getInt32(static_cast<uint32_t>((shaderHash >> 0) & 0xffffffff)));

  op.eraseFromParent();
}
} // namespace lgc
