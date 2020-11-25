/*
************************************************************************************************************************
*
*  Copyright (C) 2021 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  PatchNullFragShader.h
 * @brief LLPC header file: contains declaration of class lgc::PatchNullFragShader.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"

namespace lgc {

// =====================================================================================================================
// Pass to generate null fragment shader if required
class PatchNullFragShader : public Patch, public llvm::PassInfoMixin<PatchNullFragShader> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch LLVM for null fragment shader generation"; }
  void updatePipelineState(PipelineState *pipelineState) const;
};

} // namespace lgc
