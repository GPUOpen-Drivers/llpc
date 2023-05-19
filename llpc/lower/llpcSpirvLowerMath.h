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
 * @file  llpcSpirvLowerMath.h
 * @brief LLPC header file: contains declarations of math lowering classes
 ***********************************************************************************************************************
 */
#pragma once

#include "llpcSpirvLower.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {

// =====================================================================================================================
// SPIR-V lowering operations for math transformation.
class SpirvLowerMath : public SpirvLower {
public:
  SpirvLowerMath();

protected:
  void init(llvm::Module &module);

  void flushDenormIfNeeded(llvm::Instruction *inst);
  bool isOperandNoContract(llvm::Value *operand);
  void disableFastMath(llvm::Value *value);

  bool m_changed;                        // Whether the module is changed
  bool m_fp16DenormFlush;                // Whether FP mode wants f16 denorms to be flushed to zero
  bool m_fp32DenormFlush;                // Whether FP mode wants f32 denorms to be flushed to zero
  bool m_fp64DenormFlush;                // Whether FP mode wants f64 denorms to be flushed to zero
  bool m_fp16RoundToZero;                // Whether FP mode wants f16 round-to-zero
  bool m_enableImplicitInvariantExports; // Whether fast math should be disabled
                                         // for gl_Position exports
};

// =====================================================================================================================
// SPIR-V lowering operations for math constant folding.
class SpirvLowerMathConstFolding : public SpirvLowerMath, public llvm::PassInfoMixin<SpirvLowerMathConstFolding> {

public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  // NOTE: We use a function parameter here to get the TargetLibraryInfo object. This is
  // needed because the passes for the legacy and new pass managers use different ways to
  // retrieve it. That also ensures the object is retrieved once the passes are properly
  // initialized. This can be removed once the switch to the new pass manager is completed.
  bool runImpl(llvm::Module &module, const std::function<llvm::TargetLibraryInfo &()> &getTargetLibraryInfo);

  static llvm::StringRef name() { return "Lower SPIR-V math constant folding"; }

  // NOTE: This function is only used by the legacy pass manager wrapper class to retrieve the
  // entry point. The function can be removed once the switch to the new pass manager is completed.
  llvm::Function *getEntryPoint();
};

// =====================================================================================================================
// SPIR-V lowering operations for math floating point optimisation.
class SpirvLowerMathFloatOp : public SpirvLowerMath,
                              public llvm::PassInfoMixin<SpirvLowerMathFloatOp>,
                              public llvm::InstVisitor<SpirvLowerMathFloatOp> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  bool runImpl(llvm::Module &module);

  virtual void visitBinaryOperator(llvm::BinaryOperator &binaryOp);
  virtual void visitUnaryOperator(llvm::UnaryOperator &unaryOp);
  virtual void visitCallInst(llvm::CallInst &callInst);
  virtual void visitFPTruncInst(llvm::FPTruncInst &fptruncInst);

  static llvm::StringRef name() { return "Lower SPIR-V math floating point optimisation"; }
};

} // namespace Llpc
