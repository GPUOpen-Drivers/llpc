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
#include "GfxRuntimeContext.h"
#include "SPIRVInternal.h"
#include "compilerutils/CompilerUtils.h"
#include "llpcContext.h"
#include "llpcSpirvLowerInternalLibraryIntrinsicUtil.h"
#include "vkgcDefs.h"
#include "lgc/Builder.h"

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
  // Prepare arguments of AmdAdvancedBlend(inColor, imageDescMsLow, imageDescMsHigh, imageDescLow, imageDescHigh,
  // fmaskDescLow, fmaskDescHigh, mode, isMsaa) from shaderLibrary
  m_builder->SetInsertPointPastAllocas(m_entryPoint);

  // Get the parameters and store them into the allocated parameter points
  Type *descType = FixedVectorType::get(m_builder->getInt32Ty(), 8);
  unsigned bindings[2] = {m_binding, m_binding + 1};
  Value *imageDescLow[2] = {};
  Value *imageDescHigh[2] = {};
  for (unsigned id = 0; id < 2; ++id) {
    unsigned descSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorResource);
    Value *imageDescPtr = m_builder->CreateGetDescPtr(ResourceNodeType::DescriptorResource,
                                                      ResourceNodeType::DescriptorResource, descSet, bindings[id]);
    Value *imageDesc = m_builder->CreateLoad(descType, imageDescPtr);
    imageDescLow[id] = m_builder->CreateShuffleVector(imageDesc, ArrayRef<int>{0, 1, 2, 3});
    imageDescHigh[id] = m_builder->CreateShuffleVector(imageDesc, ArrayRef<int>{4, 5, 6, 7});
  }

  unsigned descSet = PipelineContext::getGlResourceNodeSetFromType(Vkgc::ResourceMappingNodeType::DescriptorFmask);
  Value *fmaskDescPtr = m_builder->CreateGetDescPtr(ResourceNodeType::DescriptorFmask,
                                                    ResourceNodeType::DescriptorFmask, descSet, m_binding);
  Value *fmaskDesc = m_builder->CreateLoad(descType, fmaskDescPtr);
  Value *fmaskDescLow = m_builder->CreateShuffleVector(fmaskDesc, ArrayRef<int>{0, 1, 2, 3});
  Value *fmaskDescHigh = m_builder->CreateShuffleVector(fmaskDesc, ArrayRef<int>{4, 5, 6, 7});

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
                                          {srcVal, imageDescLow[0], imageDescHigh[0], imageDescLow[1], imageDescHigh[1],
                                           fmaskDescLow, fmaskDescHigh, modeUniform, isMsaaUniform})
                              .returnValue;

      storeInst->setOperand(0, blendColor);
    }
  }
}

} // namespace Llpc
