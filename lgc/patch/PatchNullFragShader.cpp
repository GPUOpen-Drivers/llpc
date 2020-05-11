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
 * @file  PatchNullFragShader.cpp
 * @brief LLPC source file: contains declaration and implementation of class lgc::PatchNullFragShader.
 ***********************************************************************************************************************
 */
#include "lgc/LgcContext.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/IntrinsDefs.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/Internal.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lgc-patch-null-frag-shader"

using namespace lgc;
using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Pass to generate null fragment shader if required
class PatchNullFragShader : public Patch {
public:
  static char ID;
  PatchNullFragShader() : Patch(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<PipelineStateWrapper>();
  }

  bool runOnModule(Module &module) override;

private:
  PatchNullFragShader(const PatchNullFragShader &) = delete;
  PatchNullFragShader &operator=(const PatchNullFragShader &) = delete;
};

char PatchNullFragShader::ID = 0;

} // namespace lgc

// =====================================================================================================================
// Create the pass that generates a null fragment shader if required.
ModulePass *lgc::createPatchNullFragShader() {
  return new PatchNullFragShader();
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
//
// @param [in,out] module : LLVM module to be run on
bool PatchNullFragShader::runOnModule(llvm::Module &module) {
  LLVM_DEBUG(dbgs() << "Run the pass Patch-Null-Frag-Shader\n");

  Patch::init(&module);

  PipelineState *pipelineState = getAnalysis<PipelineStateWrapper>().getPipelineState(&module);

  // Do not add a null fragment shader if generating an unlinked half-pipeline ELF.
  if (pipelineState->isUnlinked())
    return false;

  const bool hasCs = pipelineState->hasShaderStage(ShaderStageCompute);
  const bool hasVs = pipelineState->hasShaderStage(ShaderStageVertex);
  const bool hasTes = pipelineState->hasShaderStage(ShaderStageTessEval);
  const bool hasGs = pipelineState->hasShaderStage(ShaderStageGeometry);
  const bool hasFs = pipelineState->hasShaderStage(ShaderStageFragment);
  if (hasCs || hasFs || (!hasVs && !hasTes && !hasGs)) {
    // This is an incomplete graphics pipeline from the amdllpc command-line tool, or a compute pipeline, or a
    // graphics pipeline that already has a fragment shader. A null fragment shader is not required.
    return false;
  }

  // Create the null fragment shader:
  // define void @llpc.shader.FS.null() !spirv.ExecutionModel !5
  // {
  // .entry:
  //     %0 = tail call float @llpc.input.import.generic.f32(i32 0, i32 0, i32 0, i32 1)
  //     tail call void @llpc.output.export.generic.f32(i32 0, i32 0, float %0)
  //     ret void
  // }

  // Create type of new function: void()
  auto entryPointTy = FunctionType::get(Type::getVoidTy(*m_context), ArrayRef<Type *>(), false);

  // Create function for the null fragment shader entrypoint.
  auto entryPoint = Function::Create(entryPointTy, GlobalValue::ExternalLinkage, lgcName::NullFsEntryPoint, &module);

  // Create its basic block, and terminate it with return.
  auto block = BasicBlock::Create(*m_context, "", entryPoint, nullptr);
  auto insertPos = ReturnInst::Create(*m_context, block);

  // Add its code. First the import.
  auto zero = ConstantInt::get(Type::getInt32Ty(*m_context), 0);
  auto one = ConstantInt::get(Type::getInt32Ty(*m_context), 1);
  Value *importArgs[] = {zero, zero, zero, one};
  auto inputTy = Type::getFloatTy(*m_context);
  std::string importName = lgcName::InputImportGeneric;
  addTypeMangling(inputTy, importArgs, importName);
  auto input = emitCall(importName, inputTy, importArgs, {}, insertPos);

  // Then the export.
  Value *exportArgs[] = {zero, zero, input};
  std::string exportName = lgcName::OutputExportGeneric;
  addTypeMangling(Type::getVoidTy(*m_context), exportArgs, exportName);
  emitCall(exportName, Type::getVoidTy(*m_context), exportArgs, {}, insertPos);

  // Set the shader stage on the new function.
  setShaderStage(entryPoint, ShaderStageFragment);

  // Initialize shader info.
  auto resUsage = pipelineState->getShaderResourceUsage(ShaderStageFragment);
  pipelineState->setShaderStageMask(pipelineState->getShaderStageMask() | shaderStageToMask(ShaderStageFragment));

  // Add usage info for dummy input
  FsInterpInfo interpInfo = {0, false, false, false, false, false};
  resUsage->builtInUsage.fs.smooth = true;
  resUsage->inOutUsage.inputLocMap[0] = InvalidValue;
  resUsage->inOutUsage.fs.interpInfo.push_back(interpInfo);

  // Add usage info for dummy output
  resUsage->inOutUsage.fs.cbShaderMask = 0;
  resUsage->inOutUsage.fs.dummyExport = true;
  resUsage->inOutUsage.fs.isNullFs = true;
  resUsage->inOutUsage.outputLocMap[0] = InvalidValue;

  return true;
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchNullFragShader, DEBUG_TYPE, "Patch LLVM for null fragment shader generation", false, false)
