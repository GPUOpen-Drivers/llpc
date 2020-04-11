/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Patch.h
 * @brief LLPC header file: contains declaration of class lgc::Patch.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"
#include "llvm/Pass.h"

namespace llvm {

class CallInst;
class PassRegistry;

namespace legacy {

class PassManager;

} // namespace legacy

void initializePatchBufferOpPass(PassRegistry &);
void initializePatchCheckShaderCachePass(PassRegistry &);
void initializePatchCopyShaderPass(PassRegistry &);
void initializePatchDescriptorLoadPass(PassRegistry &);
void initializePatchEntryPointMutatePass(PassRegistry &);
void initializePatchInOutImportExportPass(PassRegistry &);
void initializePatchIntrinsicSimplifyPass(PassRegistry &);
void initializePatchLlvmIrInclusionPass(PassRegistry &);
void initializePatchLoadScalarizerPass(PassRegistry &);
void initializePatchNullFragShaderPass(PassRegistry &);
void initializePatchPeepholeOptPass(PassRegistry &);
void initializePatchPreparePipelineAbiPass(PassRegistry &);
void initializePatchPushConstOpPass(PassRegistry &);
void initializePatchResourceCollectPass(PassRegistry &);
void initializePatchSetupTargetFeaturesPass(PassRegistry &);

} // namespace llvm

namespace lgc {

class PatchCheckShaderCache;

// Initialize passes for patching
//
// @param passRegistry : Pass registry
inline static void initializePatchPasses(llvm::PassRegistry &passRegistry) {
  initializePatchBufferOpPass(passRegistry);
  initializePatchCheckShaderCachePass(passRegistry);
  initializePatchCopyShaderPass(passRegistry);
  initializePatchDescriptorLoadPass(passRegistry);
  initializePatchEntryPointMutatePass(passRegistry);
  initializePatchInOutImportExportPass(passRegistry);
  initializePatchIntrinsicSimplifyPass(passRegistry);
  initializePatchLlvmIrInclusionPass(passRegistry);
  initializePatchLoadScalarizerPass(passRegistry);
  initializePatchNullFragShaderPass(passRegistry);
  initializePatchPeepholeOptPass(passRegistry);
  initializePatchPreparePipelineAbiPass(passRegistry);
  initializePatchPushConstOpPass(passRegistry);
  initializePatchResourceCollectPass(passRegistry);
  initializePatchSetupTargetFeaturesPass(passRegistry);
}

llvm::FunctionPass *createPatchBufferOp();
PatchCheckShaderCache *createPatchCheckShaderCache();
llvm::ModulePass *createPatchCopyShader();
llvm::ModulePass *createPatchDescriptorLoad();
llvm::ModulePass *createPatchEntryPointMutate();
llvm::ModulePass *createPatchInOutImportExport();
llvm::FunctionPass *createPatchIntrinsicSimplify();
llvm::ModulePass *createPatchLlvmIrInclusion();
llvm::FunctionPass *createPatchLoadScalarizer();
llvm::ModulePass *createPatchNullFragShader();
llvm::FunctionPass *createPatchPeepholeOpt(bool enableDiscardOpt = false);
llvm::ModulePass *createPatchPreparePipelineAbi(bool onlySetCallingConvs);
llvm::ModulePass *createPatchPushConstOp();
llvm::ModulePass *createPatchResourceCollect();
llvm::ModulePass *createPatchSetupTargetFeatures();

class PipelineState;

// =====================================================================================================================
// Represents the pass of LLVM patching operations, as the base class.
class Patch : public llvm::ModulePass {
public:
  explicit Patch(char &pid)
      : llvm::ModulePass(pid), m_module(nullptr), m_context(nullptr), m_shaderStage(ShaderStageInvalid),
        m_entryPoint(nullptr) {}
  virtual ~Patch() {}

  static void addPasses(PipelineState *pipelineState, llvm::legacy::PassManager &passMgr,
                        llvm::ModulePass *replayerPass, llvm::Timer *patchTimer, llvm::Timer *optTimer,
                        Pipeline::CheckShaderCacheFunc checkShaderCacheFunc);

  static llvm::GlobalVariable *getLdsVariable(PipelineState *pipelineState, llvm::Module *module);

protected:
  void init(llvm::Module *module);

  // -----------------------------------------------------------------------------------------------------------------

  llvm::Module *m_module;       // LLVM module to be run on
  llvm::LLVMContext *m_context; // Associated LLVM context of the LLVM module that passes run on
  ShaderStage m_shaderStage;    // Shader stage
  llvm::Function *m_entryPoint; // Entry-point

private:
  static void addOptimizationPasses(llvm::legacy::PassManager &passMgr);

  Patch() = delete;
  Patch(const Patch &) = delete;
  Patch &operator=(const Patch &) = delete;
};
} // namespace lgc
