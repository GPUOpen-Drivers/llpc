/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerRayTracingBuiltIn.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerRayTracingBuiltIn.
 ***********************************************************************************************************************
 */

#include "llpcSpirvLowerRayTracingBuiltIn.h"
#include "SPIRVInternal.h"
#include "gpurt-compiler.h"
#include "llpcContext.h"
#include "llpcRayTracingContext.h"
#include "llpcSpirvLowerRayTracing.h"
#include "llpcSpirvLowerUtil.h"
#include "lgc/Builder.h"
#include "lgc/Pipeline.h"

#define DEBUG_TYPE "llpc-spirv-lower-ray-tracing-builtin"

using namespace llvm;
using namespace Llpc;
using namespace spv;

namespace Llpc {
// =====================================================================================================================
SpirvLowerRayTracingBuiltIn::SpirvLowerRayTracingBuiltIn() {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses SpirvLowerRayTracingBuiltIn::run(Module &module, ModuleAnalysisManager &analysisManager) {
  runImpl(module);
  return PreservedAnalyses::none();
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool SpirvLowerRayTracingBuiltIn::runImpl(Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Spirv-Lower-Ray-Tracing-BuiltIn\n");
  m_module = &module;
  m_context = static_cast<Context *>(&m_module->getContext());
  m_builder = m_context->getBuilder();
  m_shaderStage = getShaderStageFromModule(m_module);
  const auto *rtState = m_context->getPipelineContext()->getRayTracingState();

  lgc::ComputeShaderMode mode = {};
  mode.workgroupSizeX = rtState->threadGroupSizeX;
  mode.workgroupSizeY = rtState->threadGroupSizeY;
  mode.workgroupSizeZ = rtState->threadGroupSizeZ;
  lgc::Pipeline::setComputeShaderMode(module, mode);

  for (auto funcIt = module.begin(), funcEnd = module.end(); funcIt != funcEnd;) {
    Function *func = &*funcIt++;
    if (func->getLinkage() == GlobalValue::ExternalLinkage && !func->empty()) {
      if (func->getName().startswith(module.getName()))
        m_entryPoint = func;
      else {
        func->dropAllReferences();
        func->eraseFromParent();
      }
    }
  }

  assert(m_entryPoint);

  LLVM_DEBUG(dbgs() << "After the pass Spirv-Lower-Ray-Tracing-BuiltIn " << module);
  return true;
}
} // namespace Llpc
