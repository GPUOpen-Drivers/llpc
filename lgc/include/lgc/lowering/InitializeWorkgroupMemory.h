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
 * @file  InitializeWorkgroupMemory.h
 * @brief LLPC header file: contains declaration of class lgc::InitializeWorkgroupMemory.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/util/BuilderBase.h"

namespace lgc {

// =====================================================================================================================
// Represents the pass of setting up the value for workgroup global variables.
class InitializeWorkgroupMemory final : public LgcLowering, public llvm::PassInfoMixin<InitializeWorkgroupMemory> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Initialize workgroup memory"; }

private:
  void initializeWithZero(llvm::GlobalVariable *lds, BuilderBase &builder);

  llvm::DenseMap<llvm::GlobalVariable *, llvm::Value *> m_globalLdsOffsetMap;
  PipelineState *m_pipelineState = nullptr;
};

} // namespace lgc
