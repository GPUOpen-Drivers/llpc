/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

void initializeLegacyLowerFragColorExportPass(PassRegistry &);
void initializeLegacyLowerVertexFetchPass(PassRegistry &);
void initializeLegacyPatchBufferOpPass(PassRegistry &);
void initializeLegacyPatchCheckShaderCachePass(PassRegistry &);
void initializeLegacyPatchCopyShaderPass(PassRegistry &);
void initializeLegacyPatchEntryPointMutatePass(PassRegistry &);
void initializeLegacyPatchInOutImportExportPass(PassRegistry &);
void initializeLegacyPatchLlvmIrInclusionPass(PassRegistry &);
void initializeLegacyPatchLoadScalarizerPass(PassRegistry &);
void initializeLegacyPatchLoopMetadataPass(PassRegistry &);
void initializeLegacyPatchNullFragShaderPass(PassRegistry &);
void initializeLegacyPatchPeepholeOptPass(PassRegistry &);
void initializeLegacyPatchPreparePipelineAbiPass(PassRegistry &);
void initializeLegacyPatchResourceCollectPass(PassRegistry &);
void initializeLegacyPatchSetupTargetFeaturesPass(PassRegistry &);
void initializeLegacyPatchWorkaroundsPass(PassRegistry &);
void initializeLegacyPatchReadFirstLanePass(PassRegistry &);
void initializeLegacyPatchWaveSizeAdjustPass(PassRegistry &);
void initializeLegacyPatchImageDerivativesPass(PassRegistry &);
void initializeLegacyPatchInitializeWorkgroupMemoryPass(PassRegistry &);
void initializeLegacyPatchInvariantLoadsPass(PassRegistry &);
} // namespace llvm

namespace lgc {

class LegacyPatchCheckShaderCache;

// Initialize passes for patching
//
// @param passRegistry : Pass registry
inline void initializePatchPasses(llvm::PassRegistry &passRegistry) {
  initializeLegacyLowerFragColorExportPass(passRegistry);
  initializeLegacyLowerVertexFetchPass(passRegistry);
  initializeLegacyPatchBufferOpPass(passRegistry);
  initializeLegacyPatchCheckShaderCachePass(passRegistry);
  initializeLegacyPatchCopyShaderPass(passRegistry);
  initializeLegacyPatchEntryPointMutatePass(passRegistry);
  initializeLegacyPatchInOutImportExportPass(passRegistry);
  initializeLegacyPatchLlvmIrInclusionPass(passRegistry);
  initializeLegacyPatchLoadScalarizerPass(passRegistry);
  initializeLegacyPatchLoopMetadataPass(passRegistry);
  initializeLegacyPatchNullFragShaderPass(passRegistry);
  initializeLegacyPatchPeepholeOptPass(passRegistry);
  initializeLegacyPatchPreparePipelineAbiPass(passRegistry);
  initializeLegacyPatchResourceCollectPass(passRegistry);
  initializeLegacyPatchSetupTargetFeaturesPass(passRegistry);
  initializeLegacyPatchWorkaroundsPass(passRegistry);
  initializeLegacyPatchReadFirstLanePass(passRegistry);
  initializeLegacyPatchWaveSizeAdjustPass(passRegistry);
  initializeLegacyPatchImageDerivativesPass(passRegistry);
  initializeLegacyPatchInitializeWorkgroupMemoryPass(passRegistry);
  initializeLegacyPatchInvariantLoadsPass(passRegistry);
}

llvm::ModulePass *createLegacyLowerFragColorExport();
llvm::ModulePass *createLegacyLowerVertexFetch();
llvm::FunctionPass *createLegacyPatchBufferOp();
LegacyPatchCheckShaderCache *createLegacyPatchCheckShaderCache();
llvm::ModulePass *createLegacyPatchCopyShader();
llvm::ModulePass *createLegacyPatchEntryPointMutate();
llvm::ModulePass *createLegacyPatchInOutImportExport();
llvm::ModulePass *createLegacyPatchLlvmIrInclusion();
llvm::FunctionPass *createLegacyPatchLoadScalarizer();
llvm::LoopPass *createLegacyPatchLoopMetadata();
llvm::ModulePass *createLegacyPatchNullFragShader();
llvm::FunctionPass *createLegacyPatchPeepholeOpt();
llvm::ModulePass *createLegacyPatchPreparePipelineAbi();
llvm::ModulePass *createLegacyPatchResourceCollect();
llvm::ModulePass *createLegacyPatchSetupTargetFeatures();
llvm::ModulePass *createLegacyPatchWorkarounds();
llvm::FunctionPass *createLegacyPatchReadFirstLane();
llvm::ModulePass *createLegacyPatchWaveSizeAdjust();
llvm::ModulePass *createLegacyPatchImageDerivatives();
llvm::ModulePass *createLegacyPatchInitializeWorkgroupMemory();
llvm::FunctionPass *createLegacyPatchInvariantLoads();
class PipelineState;
class PassManager;

// =====================================================================================================================
// Represents the pass of LLVM patching operations, as the base class.
class Patch {
public:
  Patch() : m_module(nullptr), m_context(nullptr), m_shaderStage(ShaderStageInvalid), m_entryPoint(nullptr) {}
  virtual ~Patch() {}

  static void addPasses(PipelineState *pipelineState, lgc::PassManager &passMgr, llvm::Timer *patchTimer,
                        llvm::Timer *optTimer, Pipeline::CheckShaderCacheFunc checkShaderCacheFunc,
                        llvm::CodeGenOpt::Level optLevel);

  // Register all the patching passes into the given pass manager
  static void registerPasses(lgc::PassManager &passMgr);

  static llvm::GlobalVariable *getLdsVariable(PipelineState *pipelineState, llvm::Module *module);

protected:
  static void addOptimizationPasses(lgc::PassManager &passMgr, llvm::CodeGenOpt::Level optLevel);

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
                        Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, llvm::CodeGenOpt::Level optLevel);

private:
  static void addOptimizationPasses(llvm::legacy::PassManager &passMgr, llvm::CodeGenOpt::Level optLevel);

  LegacyPatch() = delete;
  LegacyPatch(const LegacyPatch &) = delete;
  LegacyPatch &operator=(const LegacyPatch &) = delete;
};

} // namespace lgc
