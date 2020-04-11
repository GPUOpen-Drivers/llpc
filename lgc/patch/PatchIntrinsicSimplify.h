/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  PatchIntrinsicSimplify.h
 * @brief LLPC header file: contains declaration of class lgc::PatchIntrinsicSimplify.
 ***********************************************************************************************************************
 */
#pragma once

#include "Patch.h"
#include "TargetInfo.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"

namespace llvm {

class AnalysisUsage;
class Function;
class FunctionPass;
class Instruction;
class IntrinsicInst;
class ScalarEvolution;
class Value;

} // namespace llvm

namespace lgc {

class Context;
class PipelineState;

// =====================================================================================================================
// Represents the LLVM pass for intrinsic simplifications.
class PatchIntrinsicSimplify final : public llvm::FunctionPass {
public:
  explicit PatchIntrinsicSimplify();

  void getAnalysisUsage(llvm::AnalysisUsage &analysisUsage) const override;
  bool runOnFunction(llvm::Function &func) override;

  // -----------------------------------------------------------------------------------------------------------------

  static char ID; // ID of this pass

  PatchIntrinsicSimplify(const PatchIntrinsicSimplify &) = delete;
  PatchIntrinsicSimplify &operator=(const PatchIntrinsicSimplify &) = delete;

private:
  bool canSafelyConvertTo16Bit(llvm::Value &value) const;
  llvm::Value *convertTo16Bit(llvm::Value &value, llvm::IRBuilder<> &builder) const;
  llvm::Value *simplifyImage(llvm::IntrinsicInst &intrinsicCall, llvm::ArrayRef<unsigned> coordOperandIndices) const;
  llvm::Value *simplifyTrigonometric(llvm::IntrinsicInst &intrinsicCall) const;
  bool canSimplify(llvm::IntrinsicInst &intrinsicCall) const;
  llvm::Value *simplify(llvm::IntrinsicInst &intrinsicCall) const;

  // -----------------------------------------------------------------------------------------------------------------

  llvm::ScalarEvolution *m_scalarEvolution = nullptr;
  llvm::LLVMContext *m_context = nullptr;
  llvm::Module *m_module = nullptr;
  GfxIpVersion m_gfxIp;
};

} // namespace lgc
