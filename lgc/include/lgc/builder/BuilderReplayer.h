/*
************************************************************************************************************************
*
*  Copyright (C) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  BuilderReplayer.h
 * @brief LLPC header file: contains declaration of class lgc::BuilderReplayer
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/builder/BuilderRecorder.h"
#include "llvm/IR/PassManager.h"

namespace lgc {

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class BuilderReplayer final : public BuilderRecorderMetadataKinds, public llvm::PassInfoMixin<BuilderReplayer> {
public:
  BuilderReplayer(Pipeline *pipeline);

  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Replay LLPC builder calls"; }

private:
  void replayCall(unsigned opcode, llvm::CallInst *call);

  llvm::Value *processCall(unsigned opcode, llvm::CallInst *call);

  std::unique_ptr<Builder> m_builder;                       // The LLPC builder that the builder
                                                            //  calls are being replayed on.
  std::map<llvm::Function *, ShaderStage> m_shaderStageMap; // Map function -> shader stage
  llvm::Function *m_enclosingFunc = nullptr;                // Last function written with current
                                                            //  shader stage
};

} // namespace lgc
