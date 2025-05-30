/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PreparePipelineAbi.h
 * @brief LLPC header file: contains declaration of class lgc::PreparePipelineAbi.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/Analysis/CycleAnalysis.h"
#include "llvm/Analysis/PostDominators.h"

namespace lgc {

// =====================================================================================================================
// Pass to prepare the pipeline ABI
class PreparePipelineAbi final : public LgcLowering, public llvm::PassInfoMixin<PreparePipelineAbi> {
public:
  // A collection of handler functions to get the analysis info of the given function
  struct FunctionAnalysisHandlers {
    // Function to get the post dominator tree of the given function
    std::function<llvm::PostDominatorTree &(llvm::Function &)> getPostDomTree;
    // Function to get the cycle info of the given function
    std::function<llvm::CycleInfo &(llvm::Function &)> getCycleInfo;
  };

  PreparePipelineAbi();

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Preparing pipeline ABI"; }

  static std::pair<llvm::Value *, llvm::Value *> readTessFactors(PipelineState *pipelineState, llvm::Value *relPatchId,
                                                                 llvm::IRBuilder<> &builder);
  static void writeTessFactors(PipelineState *pipelineState, llvm::Value *tfBufferDesc, llvm::Value *tfBufferBase,
                               llvm::Value *relPatchId, llvm::Value *outerTf, llvm::Value *innerTf,
                               BuilderBase &builder);
  static void writeHsOutputs(PipelineState *pipelineState, llvm::Value *offChipLdsDesc, llvm::Value *offChipLdsBase,
                             llvm::Value *relPatchId, llvm::Value *vertexIdx, llvm::Value *outerTf,
                             BuilderBase &builder);

private:
  void mergeShader(llvm::Module &module);

  void setAbiEntryNames(llvm::Module &module);

  void addAbiMetadata(llvm::Module &module);

  void storeTessFactorsAndHsOutputs(llvm::Function *entryPoint);

  PipelineState *m_pipelineState;           // Pipeline state
  PipelineShadersResult *m_pipelineShaders; // API shaders in the pipeline
  FunctionAnalysisHandlers
      *m_analysisHandlers; // A collection of handler functions to get the analysis info of the given function

  bool m_hasVs;   // Whether the pipeline has vertex shader
  bool m_hasTcs;  // Whether the pipeline has tessellation control shader
  bool m_hasTes;  // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;   // Whether the pipeline has geometry shader
  bool m_hasTask; // Whether the pipeline has task shader
  bool m_hasMesh; // Whether the pipeline has mesh shader

  GfxIpVersion m_gfxIp; // Graphics IP version info
};

} // namespace lgc
