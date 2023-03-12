/*
************************************************************************************************************************
*
*  Copyright (C) 2017-2022 Advanced Micro Devices, Inc. All rights reserved.
*
***********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  LowerDebugPrintf.h
 * @brief LLPC header file : contains declaration of class lgc::LowerDebugPrintf.h
 ***********************************************************************************************************************
 */
#pragma once
#include "SystemValues.h"
#include "lgc/Builder.h"
#include "lgc/patch/Patch.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/IR/Function.h"

namespace lgc {
// =====================================================================================================================
// Pass to lower debug.printf calls
class LowerDebugPrintf : public Patch, public llvm::PassInfoMixin<LowerDebugPrintf> {
  struct ElfInfo {
    llvm::StringRef formatString;        // Printf format string
    llvm::SmallVector<bool, 4> bit64Pos; // 64bit position records output variable 32bit/64bit condition.
  };

public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "Lower debug printf calls"; }

private:
  llvm::Value *createDebugPrintf(llvm::Value *debugPrintfBuffer, llvm::Value *formatStr,
                                 llvm::iterator_range<llvm::User::op_iterator> vars, lgc::BuilderBase &builder);
  void getDwordValues(llvm::Value *val, llvm::SmallVector<llvm::Value *, 4> &output,
                      llvm::SmallVector<bool, 4> &output64Bits);
  void setupElfsPrintfStrings();
  llvm::DenseMap<uint64_t, ElfInfo> m_elfInfos;
  PipelineState *m_pipelineState = nullptr;
  PipelineShadersResult *m_pipelineShaders = nullptr;
  GfxIpVersion m_gfxIp;
};

} // namespace lgc
