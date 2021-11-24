/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchNullFragShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchNullFragShader.
 ***********************************************************************************************************************
 */
#include "PatchNullFragShader.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/FragColorExport.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PalMetadata.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-null-frag-shader"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Pass to generate null fragment shader if required
class LegacyPatchNullFragShader : public ModulePass {
public:
  static char ID;
  LegacyPatchNullFragShader() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<LegacyPipelineStateWrapper>();
  }

  bool runOnModule(Module &module) override;

private:
  LegacyPatchNullFragShader(const LegacyPatchNullFragShader &) = delete;
  LegacyPatchNullFragShader &operator=(const LegacyPatchNullFragShader &) = delete;

  PatchNullFragShader m_impl;
};

char LegacyPatchNullFragShader::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create the pass that generates a null fragment shader if required.
ModulePass *lgc::createLegacyPatchNullFragShader() {
  return new LegacyPatchNullFragShader();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool LegacyPatchNullFragShader::runOnModule(Module &module) {
  PipelineState *pipelineState = getAnalysis<LegacyPipelineStateWrapper>().getPipelineState(&module);
  return m_impl.runImpl(module, pipelineState);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @param [in/out] analysisManager : Analysis manager to use for this transformation
// @returns : The preserved analyses (The Analyses that are still valid after this pass)
PreservedAnalyses PatchNullFragShader::run(Module &module, ModuleAnalysisManager &analysisManager) {
  PipelineState *pipelineState = analysisManager.getResult<PipelineStateWrapper>(module).getPipelineState();
  if (runImpl(module, pipelineState))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in/out] module : LLVM module to be run on
// @returns : True if the module was modified by the transformation and false otherwise
bool PatchNullFragShader::runImpl(Module &module, PipelineState *pipelineState) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Null-Frag-Shader\n");

  Patch::init(&module);

  // Do not add a null fragment shader if not generating a whole pipeline.
  if (!pipelineState->isWholePipeline())
    return false;

  // If a fragment shader is not needed, then do not generate one.
  const bool hasFs = pipelineState->hasShaderStage(ShaderStageFragment);
  if (hasFs || !pipelineState->isGraphics())
    return false;

  generateNullFragmentShader(module);
  updatePipelineState(pipelineState);
  return true;
}

// =====================================================================================================================
// Updates the the pipeline state with the data for the null fragment shader.
//
// @param [in/out] module : The LLVM module in which to add the shader.
void PatchNullFragShader::updatePipelineState(PipelineState *pipelineState) const {
  auto resUsage = pipelineState->getShaderResourceUsage(ShaderStageFragment);
  pipelineState->setShaderStageMask(pipelineState->getShaderStageMask() | shaderStageToMask(ShaderStageFragment));

  // Add usage info for dummy output
  resUsage->inOutUsage.fs.cbShaderMask = 0;
  resUsage->inOutUsage.fs.isNullFs = true;
  InOutLocationInfo origLocInfo;
  origLocInfo.setLocation(0);
  auto &newOutLocInfo = resUsage->inOutUsage.outputLocInfoMap[origLocInfo];
  newOutLocInfo.setData(InvalidValue);
}

// =====================================================================================================================
// Generate a new fragment shader that has the minimum code needed to make PAL happy.
//
// @param [in/out] module : The LLVM module in which to add the shader.
void PatchNullFragShader::generateNullFragmentShader(Module &module) {
  Function *entryPoint = generateNullFragmentEntryPoint(module);
  generateNullFragmentShaderBody(entryPoint);
}

// =====================================================================================================================
// Generate a new entry point for a null fragment shader.
//
// @param [in/out] module : The LLVM module in which to add the entry point.
// @returns : The new entry point.
Function *PatchNullFragShader::generateNullFragmentEntryPoint(Module &module) {
  FunctionType *entryPointTy = FunctionType::get(Type::getVoidTy(module.getContext()), ArrayRef<Type *>(), false);
  Function *entryPoint =
      Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::NullFsEntryPoint, &module);
  entryPoint->setDLLStorageClass(GlobalValue::DLLExportStorageClass);
  setShaderStage(entryPoint, ShaderStageFragment);
  return entryPoint;
}

// =====================================================================================================================
// Generate the body of the null fragment shader.
//
// @param [in/out] entryPoint : The function in which the code will be inserted.
void PatchNullFragShader::generateNullFragmentShaderBody(llvm::Function *entryPoint) {
  BasicBlock *block = BasicBlock::Create(entryPoint->getContext(), "", entryPoint);
  BuilderBase builder(block);
  builder.CreateRetVoid();
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(LegacyPatchNullFragShader, DEBUG_TYPE, "Patch LLVM for null fragment shader generation", false, false)
