/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations, as the base class.
class SpirvLower {
public:
  explicit SpirvLower() {}

  // Add per-shader lowering passes to pass manager
  static void addPasses(Context *context, ShaderStage stage, lgc::PassManager &passMgr, llvm::Timer *lowerTimer
#if VKI_RAY_TRACING
                        ,
                        bool rayTracing, bool rayQuery, bool isInternalRtShader
#endif
  );
  // Register all the lowering passes into the given pass manager
  static void registerPasses(lgc::PassManager &passMgr);

  static void removeConstantExpr(Context *context, llvm::GlobalVariable *global);
  static void replaceConstWithInsts(Context *context, llvm::Constant *const constVal);
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
