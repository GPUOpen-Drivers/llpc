/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PrepareContinuations.cpp
 * @brief LLPC source file: contains implementation of class Llpc::PrepareContinuations.
 ***********************************************************************************************************************
 */
#include "PrepareContinuations.h"
#include "compilerutils/CompilerUtils.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "llvmraytracing/ContinuationsUtil.h"
#include "llvmraytracing/GpurtContext.h"
#include "lgc/Builder.h"
#include "llvm/IR/Module.h"

#define DEBUG_TYPE "prepare-continuations"
using namespace lgc;
using namespace llvm;
using namespace lgc::rt;
using namespace CompilerUtils;

namespace Llpc {
PrepareContinuations::PrepareContinuations() {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses PrepareContinuations::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass PrepareContinuations\n");
  SpirvLower::init(&module);
  const auto *rtState = m_context->getPipelineContext()->getRayTracingState();
  ComputeShaderMode mode = {};
  // NOTE: For continuations, we only support flatten threadgroup (more precisely, numthreads(32, 1, 1)) so far.
  assert(rtState->dispatchRaysThreadGroupSize == 32);
  mode.workgroupSizeX = rtState->dispatchRaysThreadGroupSize;
  mode.workgroupSizeY = 1;
  mode.workgroupSizeZ = 1;
  mode.noLocalInvocationIdInCalls = true;
  Pipeline::setComputeShaderMode(module, mode);
  module.getOrInsertNamedMetadata(ContHelper::MDLgcCpsModuleName);

  if (module.getName().starts_with("main")) {
    m_shaderStage = ShaderStageRayTracingRayGen;
    auto FuncTy = FunctionType::get(m_builder->getVoidTy(), {}, false);
    GpurtContext &gpurtContext = GpurtContext::get(*m_context);
    Function *contKernel = gpurtContext.theModule->getFunction("_cont_KernelEntry");
    Function *entryFunc = Function::Create(FuncTy, GlobalValue::ExternalLinkage, "main", module);
    auto *bb = BasicBlock::Create(entryFunc->getContext(), "entry", entryFunc);
    CrossModuleInliner inliner;
    IRBuilder<> builder(bb);
    builder.SetInsertPoint(builder.CreateRetVoid());
    inliner.inlineCall(builder, contKernel, {});
    setLgcRtShaderStage(entryFunc, RayTracingShaderStage::KernelEntry);
    lgc::Pipeline::markShaderEntryPoint(entryFunc, lgc::ShaderStage::Compute);
  } else {
    m_entryPoint->setName(module.getName());
    auto rtContext = static_cast<RayTracingContext *>(m_context->getPipelineContext());

    ContHelper::setMaxPayloadRegisterCount(module, cps::CpsPayloadMaxNumVgprs);

    setMaxHitAttributeSize(&module, rtContext->getAttributeDataSizeInBytes());
    setMaxPayloadSize(&module, rtContext->getPayloadSizeInBytes());
  }

  return PreservedAnalyses::none();
}

} // namespace Llpc
