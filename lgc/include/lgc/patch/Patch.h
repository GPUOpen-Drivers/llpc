/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Pass.h"

namespace llvm {

class PassRegistry;

namespace legacy {

class PassManager;

} // namespace legacy

void initializeLowerFragColorExportPass(PassRegistry &);
void initializeLowerVertexFetchPass(PassRegistry &);
void initializePatchBufferOpPass(PassRegistry &);
void initializePatchCheckShaderCachePass(PassRegistry &);
void initializePatchCopyShaderPass(PassRegistry &);
void initializePatchEntryPointMutatePass(PassRegistry &);
void initializePatchInOutImportExportPass(PassRegistry &);
void initializePatchLlvmIrInclusionPass(PassRegistry &);
void initializePatchLoadScalarizerPass(PassRegistry &);
void initializePatchLoopMetadataPass(PassRegistry &);
void initializeLegacyPatchNullFragShaderPass(PassRegistry &);
void initializePatchPeepholeOptPass(PassRegistry &);
void initializePatchPreparePipelineAbiPass(PassRegistry &);
void initializePatchResourceCollectPass(PassRegistry &);
void initializePatchSetupTargetFeaturesPass(PassRegistry &);
void initializePatchWorkaroundsPass(PassRegistry &);
void initializePatchReadFirstLanePass(PassRegistry &);
void initializePatchWaveSizeAdjustPass(PassRegistry &);
void initializePatchInitializeWorkgroupMemoryPass(PassRegistry &);

} // namespace llvm

namespace lgc {

class PatchCheckShaderCache;

// Initialize passes for patching
//
// @param passRegistry : Pass registry
inline static void initializePatchPasses(llvm::PassRegistry &passRegistry) {
  initializeLowerFragColorExportPass(passRegistry);
  initializeLowerVertexFetchPass(passRegistry);
  initializePatchBufferOpPass(passRegistry);
  initializePatchCheckShaderCachePass(passRegistry);
  initializePatchCopyShaderPass(passRegistry);
  initializePatchEntryPointMutatePass(passRegistry);
  initializePatchInOutImportExportPass(passRegistry);
  initializePatchLlvmIrInclusionPass(passRegistry);
  initializePatchLoadScalarizerPass(passRegistry);
  initializePatchLoopMetadataPass(passRegistry);
  initializeLegacyPatchNullFragShaderPass(passRegistry);
  initializePatchPeepholeOptPass(passRegistry);
  initializePatchPreparePipelineAbiPass(passRegistry);
  initializePatchResourceCollectPass(passRegistry);
  initializePatchSetupTargetFeaturesPass(passRegistry);
  initializePatchWorkaroundsPass(passRegistry);
  initializePatchReadFirstLanePass(passRegistry);
  initializePatchWaveSizeAdjustPass(passRegistry);
  initializePatchInitializeWorkgroupMemoryPass(passRegistry);
}

llvm::ModulePass *createLowerFragColorExport();
llvm::ModulePass *createLowerVertexFetch();
llvm::FunctionPass *createPatchBufferOp();
PatchCheckShaderCache *createPatchCheckShaderCache();
llvm::ModulePass *createPatchCopyShader();
llvm::ModulePass *createPatchEntryPointMutate();
llvm::ModulePass *createPatchInOutImportExport();
llvm::ModulePass *createPatchLlvmIrInclusion();
llvm::FunctionPass *createPatchLoadScalarizer();
llvm::LoopPass *createPatchLoopMetadata();
llvm::ModulePass *createLegacyPatchNullFragShader();
llvm::FunctionPass *createPatchPeepholeOpt();
llvm::ModulePass *createPatchPreparePipelineAbi(bool onlySetCallingConvs);
llvm::ModulePass *createPatchResourceCollect();
llvm::ModulePass *createPatchSetupTargetFeatures();
llvm::ModulePass *createPatchWorkarounds();
llvm::FunctionPass *createPatchReadFirstLane();
llvm::ModulePass *createPatchWaveSizeAdjust();
llvm::ModulePass *createPatchInitializeWorkgroupMemory();

class PipelineState;
class PassManager;

// =====================================================================================================================
// Represents the pass of LLVM patching operations, as the base class.
class Patch {
public:
  Patch() : m_module(nullptr), m_context(nullptr), m_shaderStage(ShaderStageInvalid), m_entryPoint(nullptr) {}
  virtual ~Patch() {}

  static void addPasses(PipelineState *pipelineState, lgc::PassManager &passMgr, bool addReplayerPass,
                        llvm::Timer *patchTimer, llvm::Timer *optTimer,
                        Pipeline::CheckShaderCacheFunc checkShaderCacheFunc);

  static llvm::GlobalVariable *getLdsVariable(PipelineState *pipelineState, llvm::Module *module);

protected:
  void init(llvm::Module *module);

  llvm::Module *m_module;       // LLVM module to be run on
  llvm::LLVMContext *m_context; // Associated LLVM context of the LLVM module that passes run on
  ShaderStage m_shaderStage;    // Shader stage
  llvm::Function *m_entryPoint; // Entry-point
};

// =====================================================================================================================
// Represents the pass of LLVM patching operations, as the base class.
class LegacyPatch : public llvm::ModulePass, public Patch {
public:
  explicit LegacyPatch(char &pid) : llvm::ModulePass(pid) {}
  virtual ~LegacyPatch() {}

  static void addPasses(PipelineState *pipelineState, llvm::legacy::PassManager &passMgr,
                        llvm::ModulePass *replayerPass, llvm::Timer *patchTimer, llvm::Timer *optTimer,
                        Pipeline::CheckShaderCacheFunc checkShaderCacheFunc);

private:
  static void addOptimizationPasses(llvm::legacy::PassManager &passMgr);

  LegacyPatch() = delete;
  LegacyPatch(const LegacyPatch &) = delete;
  LegacyPatch &operator=(const LegacyPatch &) = delete;
};

} // namespace lgc
