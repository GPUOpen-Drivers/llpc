/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerAdvancedBlend.cpp
 * @brief LLPC source file: contains implementation of class Llpc::LowerAdvancedBlend.
 ***********************************************************************************************************************
 */
#include "LowerAdvancedBlend.h"
#include "SPIRVInternal.h"
#include "compilerutils/CompilerUtils.h"
#include "llpcContext.h"
#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "vkgcDefs.h"
#include "lgc/Builder.h"
#include "lgc/RuntimeContext.h"

#define DEBUG_TYPE "Lower-advanced-blend"

using namespace lgc;
using namespace llvm;
using namespace Llpc;

namespace Llpc {
static const char *AdvancedBlendInternal = "AmdAdvancedBlendInternal";
static const char *AdvancedBlendModeName = "_mode";
static const char *AdvancedBlendIsMsaaName = "_isMsaa";

// =====================================================================================================================
LowerAdvancedBlend::LowerAdvancedBlend(unsigned binding) : m_binding(binding) {
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on (empty on entry)
// @param [in/out] analysisManager : Analysis manager to use for this transformation
PreservedAnalyses LowerAdvancedBlend::run(Module &module, ModuleAnalysisManager &analysisManager) {
  LLVM_DEBUG(dbgs() << "Run the pass Lower-advanced-blend\n");
  SpirvLower::init(&module);

  if (m_shaderStage == ShaderStageFragment) {
    processFsOutputs(module);
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Apply blending function on outputs of fragment shader
//
// @param [in/out] module : LLVM module to be run on
void LowerAdvancedBlend::processFsOutputs(Module &module) {
  // Get the outputs of FS
  SmallVector<Value *, 8> outputs;
  Value *modeUniform = nullptr;
  Value *isMsaaUniform = nullptr;
  for (auto &global : module.globals()) {
    if (global.getType()->getAddressSpace() == SPIRAS_Output)
      outputs.push_back(&global);
    if (global.getType()->getAddressSpace() == SPIRAS_Uniform && global.getName().ends_with(AdvancedBlendModeName))
      modeUniform = &global;
    if (global.getType()->getAddressSpace() == SPIRAS_Uniform && global.getName().ends_with(AdvancedBlendIsMsaaName))
      isMsaaUniform = &global;
  }
  // Prepare arguments of AmdAdvancedBlend(inColor, imageDescMs, imageDesc, fmaskDesc, mode, isMsaa) from shaderLibrary
  m_builder->SetInsertPointPastAllocas(m_entryPoint);

  // Get the parameters and store them into the allocated parameter points
  unsigned bindings[2] = {m_binding, m_binding + 1};
  Value *imageDesc[2] = {};
  for (unsigned id = 0; id < 2; ++id) {
    unsigned descSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorResource);
    imageDesc[id] = m_builder->CreateGetDescPtr(ResourceNodeType::DescriptorResource,
                                                ResourceNodeType::DescriptorResource, descSet, bindings[id]);
    imageDesc[id] = m_builder->CreatePtrToInt(imageDesc[id], m_builder->getInt64Ty());
  }

  unsigned descSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorFmask);
  Value *fmaskDesc = m_builder->CreateGetDescPtr(ResourceNodeType::DescriptorFmask, ResourceNodeType::DescriptorFmask,
                                                 descSet, m_binding);
  fmaskDesc = m_builder->CreatePtrToInt(fmaskDesc, m_builder->getInt64Ty());

  assert(modeUniform && isMsaaUniform);
  modeUniform = m_builder->CreateLoad(m_builder->getInt32Ty(), modeUniform);

  isMsaaUniform =
      m_builder->CreateTrunc(m_builder->CreateLoad(m_builder->getInt32Ty(), isMsaaUniform), m_builder->getInt1Ty());

  // Link the gfxruntime library module
  GfxRuntimeContext &gfxRuntimeContext = GfxRuntimeContext::get(*m_context);
  auto *advancedBlendFunc = (*gfxRuntimeContext.theModule).getFunction(AdvancedBlendInternal);

  CompilerUtils::CrossModuleInliner inliner;

  // Call AmdAdvancedBlendInternal() for each output
  for (auto [i, outCol] : llvm::enumerate(outputs)) {
    for (auto user : outCol->users()) {
      auto storeInst = cast<StoreInst>(user);
      assert(storeInst);
      Value *srcVal = storeInst->getValueOperand();
      m_builder->SetInsertPoint(storeInst);

      Value *blendColor = inliner
                              .inlineCall(*m_builder, advancedBlendFunc,
                                          {srcVal, imageDesc[0], imageDesc[1], fmaskDesc, modeUniform, isMsaaUniform})
                              .returnValue;

      storeInst->setOperand(0, blendColor);
    }
  }
}

} // namespace Llpc
