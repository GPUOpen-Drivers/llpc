/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchCopyShader.h
 * @brief LLPC header file: contains declaration of class lgc::PatchCopyShader.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/patch/Patch.h"
#include "lgc/patch/SystemValues.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"

namespace lgc {

// =====================================================================================================================
// Pass to generate copy shader if required
class PatchCopyShader : public Patch, public llvm::PassInfoMixin<PatchCopyShader> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  bool runImpl(llvm::Module &module, PipelineShadersResult &pipelineShaders, PipelineState *pipelineState);

  static llvm::StringRef name() { return "Patch LLVM for copy shader generation"; }

private:
  void exportOutput(unsigned streamId, BuilderBase &builder);
  void collectGsGenericOutputInfo(llvm::Function *gsEntryPoint);

  llvm::Value *calcGsVsRingOffsetForInput(unsigned location, unsigned compIdx, unsigned streamId, BuilderBase &builder);

  llvm::Value *loadValueFromGsVsRing(llvm::Type *loadTy, unsigned location, unsigned component, unsigned streamId,
                                     BuilderBase &builder);

  void exportGenericOutput(llvm::Value *outputValue, unsigned location, BuilderBase &builder);
  void exportXfbOutput(llvm::Value *outputValue, const XfbOutInfo &XfbOutInfo, BuilderBase &builder);
  void exportBuiltInOutput(llvm::Value *outputValue, BuiltInKind builtInId, unsigned streamId, BuilderBase &builder);

  // Low part of global internal table pointer
  static const unsigned EntryArgIdxInternalTablePtrLow = 0;

  PipelineState *m_pipelineState = nullptr; // Pipeline state
  PipelineSystemValues m_pipelineSysValues; // Cache of ShaderSystemValues objects
  llvm::GlobalVariable *m_lds = nullptr;    // Global variable representing LDS

  llvm::DenseMap<unsigned, llvm::DenseMap<unsigned, unsigned>>
      m_outputLocCompSizeMap[MaxGsStreams]; // The dword size of the output value at the new mapped <location,
                                            // component> for each stream
};

} // namespace lgc
