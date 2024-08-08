/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerDesc.cpp
 * @brief LLPC source file: contains implementation of class lgc::LowerDesc.
 ***********************************************************************************************************************
 */
#include "lgc/patch/LowerDesc.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/util/AddressExtender.h"
#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lower-desc"

using namespace llvm;
using namespace lgc;

namespace lgc {

// =====================================================================================================================
// Executes this LLVM patching pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The analyses that are still valid after this pass)
PreservedAnalyses LowerDesc::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass " DEBUG_TYPE "\n");
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  m_pipelineState = pipelineState;

  static const auto visitor = llvm_dialects::VisitorBuilder<LowerDesc>()
                                  .add(&LowerDesc::visitExtendAddress)
                                  .add(&LowerDesc::visitLoadBufferDesc)
                                  .add(&LowerDesc::visitLoadStridedBufferDesc)
                                  .build();

  visitor.visit(*this, module);

  for (auto inst : m_toErase)
    inst->eraseFromParent();

  if (m_toErase.empty())
    return PreservedAnalyses::all();
  return PreservedAnalyses::allInSet<CFGAnalyses>();
}

// =====================================================================================================================
// Lower an extend.address op, to extend a 32-bit address to i64.
void LowerDesc::visitExtendAddress(ExtendAddressOp &op) {
  IRBuilder<> builder(&op);
  AddressExtender extender(op.getFunction());
  Value *addr64 = extender.extendWithPc(op.getAddr32(), /*ptrTy=*/nullptr, builder);
  op.replaceAllUsesWith(addr64);
  op.eraseFromParent();
}

// =====================================================================================================================
// Lower a load.buffer.desc operation
//
// @param op : the operation
void LowerDesc::visitLoadBufferDesc(LoadBufferDescOp &op) {
  BuilderImpl builder(m_pipelineState);
  builder.setShaderStage(getShaderStage(op.getFunction()));
  builder.SetInsertPoint(&op);

  unsigned flags = op.getFlags();
  // Anyone who wants to get a 64-bit buffer descriptor address should call `CreateBufferDesc` directly. (This is only
  // available in LGC as we don't expect front-end would required such usage.)
  assert(!(flags & Builder::BufferFlagAddress) && "Returning a 64-bit address is unsupported by lgc.load.buffer.desc");

  Value *desc = builder.CreateBufferDesc(op.getDescSet(), op.getBinding(), op.getDescIndex(), flags, true);

  m_toErase.push_back(&op);

  // Convert to fat pointer.
  op.replaceAllUsesWith(desc);
}

// =====================================================================================================================
// Lower a load.strided.buffer.desc operation
//
// @param op : the operation
void LowerDesc::visitLoadStridedBufferDesc(LoadStridedBufferDescOp &op) {
  BuilderImpl builder(m_pipelineState);
  builder.setShaderStage(getShaderStage(op.getFunction()));
  builder.SetInsertPoint(&op);

  unsigned flags = op.getFlags();
  // Anyone who wants to get a 64-bit buffer descriptor address should call `CreateBufferDesc` directly. (This is only
  // available in LGC as we don't expect front-end would required such usage.)
  assert(!(flags & Builder::BufferFlagAddress) &&
         "Returning a 64-bit address is unsupported by lgc.load.strided.buffer.desc");

  Value *desc =
      builder.CreateStridedBufferDesc(op.getDescSet(), op.getBinding(), op.getDescIndex(), flags, op.getStride());

  m_toErase.push_back(&op);

  op.replaceAllUsesWith(desc);
}
} // namespace lgc
