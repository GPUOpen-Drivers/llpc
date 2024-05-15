/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
  bool m_changed;         // Whether the module is changed
  bool m_fp16DenormFlush; // Whether FP mode wants f16 denorms to be flushed to zero
  bool m_fp32DenormFlush; // Whether FP mode wants f32 denorms to be flushed to zero
  bool m_fp64DenormFlush; // Whether FP mode wants f64 denorms to be flushed to zero
  bool m_fp16RoundToZero; // Whether FP mode wants f16 round-to-zero
};

// =====================================================================================================================
// SPIR-V lowering operations for math constant folding.
class SpirvLowerMathConstFolding : public SpirvLowerMath, public llvm::PassInfoMixin<SpirvLowerMathConstFolding> {

public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower SPIR-V math constant folding"; }

  // NOTE: This function is only used by the legacy pass manager wrapper class to retrieve the
  // entry point. The function can be removed once the switch to the new pass manager is completed.
  llvm::Function *getEntryPoint();
};

// =====================================================================================================================
// SPIR-V lowering operations to adjust fast math flags.
class SpirvLowerMathPrecision : public SpirvLower, public llvm::PassInfoMixin<SpirvLowerMathPrecision> {

public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Lower SPIR-V for precision (fast math flags)"; }

  bool adjustExports(llvm::Module &module);
  bool propagateNoContract(llvm::Module &module, bool forward, bool backward);
};

// =====================================================================================================================
// SPIR-V lowering operations for math floating point optimisation.
class SpirvLowerMathFloatOp : public SpirvLowerMath,
                              public llvm::PassInfoMixin<SpirvLowerMathFloatOp>,
                              public llvm::InstVisitor<SpirvLowerMathFloatOp> {
public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  virtual void visitBinaryOperator(llvm::BinaryOperator &binaryOp);
  virtual void visitCallInst(llvm::CallInst &callInst);
  virtual void visitFPTruncInst(llvm::FPTruncInst &fptruncInst);
  void emitFFmulzInst(llvm::BinaryOperator &binaryOp);
  void emitFFmazInst(llvm::Instruction *inst);
  std::optional<std::pair<llvm::Value *, llvm::Value *>> isMulDx9Zero(llvm::Value *lhs, llvm::Value *rhs,
                                                                      llvm::FastMathFlags flags);
  static llvm::StringRef name() { return "Lower SPIR-V math floating point optimisation"; }
};

} // namespace Llpc
