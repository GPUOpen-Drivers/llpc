/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLower.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLower.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llpcUtil.h"
#include "llvm/Pass.h"

namespace llvm {

class Constant;
class GlobalVariable;
class Timer;
} // namespace llvm

namespace lgc {

class Builder;
class PassManager;

} // namespace lgc

namespace Llpc {

class Context;

union LowerFlag {
  struct {
    unsigned isRayTracing : 1;       // Whether we are lowering a ray tracing pipeline shader
    unsigned isRayQuery : 1;         // Whether we are lowering a ray query library
    unsigned isInternalRtShader : 1; // Whether we are lowering an internal ray tracing shader
    unsigned usesAdvancedBlend : 1;  // Whether we are lowering an advanced blend shader
    unsigned reserved : 28;
  };
  unsigned u32All;
};

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations, as the base class.
class SpirvLower {
public:
  explicit SpirvLower() {}

  // Add per-shader lowering passes to pass manager
  static void addPasses(Context *context, ShaderStage stage, lgc::PassManager &passMgr, llvm::Timer *lowerTimer,
                        LowerFlag lowerFlag);
  // Register all the translation passes into the given pass manager
  static void registerTranslationPasses(lgc::PassManager &passMgr);
  // Register all the lowering passes into the given pass manager
  static void registerLoweringPasses(lgc::PassManager &passMgr);

  static void replaceGlobal(Context *context, llvm::GlobalVariable *original, llvm::GlobalVariable *replacement);

protected:
  void init(llvm::Module *module);

  llvm::Module *m_module = nullptr;               // LLVM module to be run on
  Context *m_context = nullptr;                   // Associated LLPC context of the LLVM module that passes run on
  ShaderStage m_shaderStage = ShaderStageInvalid; // Shader stage
  llvm::Function *m_entryPoint = nullptr;         // Entry point of input module
  lgc::Builder *m_builder = nullptr;              // LGC builder object
};

} // namespace Llpc
