/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LgcLowering.h
 * @brief LLPC header file: contains declaration of class lgc::LgcLowering.
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/Pipeline.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Pass.h"

namespace llvm {

class PassBuilder;

} // namespace llvm

namespace lgc {

class PipelineState;
class PassManager;

// =====================================================================================================================
// Represents the pass of LGC lowering operations, as the base class.
class LgcLowering {
public:
  LgcLowering() : m_module(nullptr), m_context(nullptr), m_entryPoint(nullptr) {}
  virtual ~LgcLowering() = default;

  static void addPasses(PipelineState *pipelineState, lgc::PassManager &passMgr, llvm::Timer *loweringTimer,
                        llvm::Timer *optTimer, Pipeline::CheckShaderCacheFunc checkShaderCacheFunc, uint32_t optLevel);

  // Register all the LGC lowering passes into the given pass manager
  static void registerPasses(lgc::PassManager &passMgr);

  // Register all the LGC lowering passes into the given pass builder
  static void registerPasses(llvm::PassBuilder &passBuilder);

  static llvm::Constant *getLdsVariable(PipelineState *pipelineState, llvm::Function *func, bool rtStack = false);

protected:
  static void addOptimizationPasses(lgc::PassManager &passMgr, uint32_t optLevel);

  void init(llvm::Module *module);

  llvm::Module *m_module;                       // LLVM module to be run on
  llvm::LLVMContext *m_context;                 // Associated LLVM context of the LLVM module that passes run on
  std::optional<ShaderStageEnum> m_shaderStage; // Shader stage
  llvm::Function *m_entryPoint;                 // Entry-point
};

} // namespace lgc
