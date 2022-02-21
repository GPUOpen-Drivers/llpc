/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchPreparePipelineAbi.h
 * @brief LLPC header file: contains declaration of class lgc::PatchPreparePipelineAbi.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"

namespace lgc {

// =====================================================================================================================
// Pass to prepare the pipeline ABI
class PatchPreparePipelineAbi final : public Patch, public llvm::PassInfoMixin<PatchPreparePipelineAbi> {
public:
  PatchPreparePipelineAbi(bool onlySetCallingConvs = false);

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineShadersResult &pipelineShaders, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch LLVM for preparing pipeline ABI"; }

private:
  void setCallingConvs(llvm::Module &module);

  void setRemainingCallingConvs(llvm::Module &module);

  void mergeShaderAndSetCallingConvs(llvm::Module &module);

  void setCallingConv(ShaderStage stage, llvm::CallingConv::ID callingConv);

  void setAbiEntryNames(llvm::Module &module);

  void addAbiMetadata(llvm::Module &module);

  PipelineState *m_pipelineState;           // Pipeline state
  PipelineShadersResult *m_pipelineShaders; // API shaders in the pipeline

  bool m_hasVs;  // Whether the pipeline has vertex shader
  bool m_hasTcs; // Whether the pipeline has tessellation control shader
  bool m_hasTes; // Whether the pipeline has tessellation evaluation shader
  bool m_hasGs;  // Whether the pipeline has geometry shader

  GfxIpVersion m_gfxIp; // Graphics IP version info

  const bool m_onlySetCallingConvs; // Whether to only set the calling conventions
};

// =====================================================================================================================
// Pass to prepare the pipeline ABI
class LegacyPatchPreparePipelineAbi final : public llvm::ModulePass {
public:
  static char ID; // NOLINT
  LegacyPatchPreparePipelineAbi(bool onlySetCallingConvs = false);

  bool runOnModule(llvm::Module &module) override;

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override {
    analysisUsage.addRequired<LegacyPipelineStateWrapper>();
    analysisUsage.addRequired<LegacyPipelineShaders>();
  }

private:
  LegacyPatchPreparePipelineAbi(const LegacyPatchPreparePipelineAbi &) = delete;
  LegacyPatchPreparePipelineAbi &operator=(const LegacyPatchPreparePipelineAbi &) = delete;

  PatchPreparePipelineAbi m_impl;
};

} // namespace lgc
