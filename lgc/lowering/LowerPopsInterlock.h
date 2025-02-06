/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerPopsInterlock.h
 * @brief LGC header file: contains declaration of lgc::LowerPopsInterlock
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/LgcDialect.h"
#include "llvm/IR/PassManager.h"

namespace lgc {

class BuilderBase;
class PipelineState;

class LowerPopsInterlock : public llvm::PassInfoMixin<LowerPopsInterlock> {
public:
  llvm::PreservedAnalyses run(llvm::Function &func, llvm::FunctionAnalysisManager &funcAnalysisManager);

  static llvm::StringRef name() { return "Lower POPS interlock operations"; }

private:
  void legalizeInterlock(llvm::FunctionAnalysisManager &funcAnalysisManager);
  void collectBeginInterlock(PopsBeginInterlockOp &popsBeginInterlockOp);
  void collectEndInterlock(PopsEndInterlockOp &popsEndInterlockOp);

  void lowerInterlock();
  void lowerBeginInterlock(PopsBeginInterlockOp &popsBeginInterlockOp);
  void lowerEndInterlock(PopsEndInterlockOp &popsEndInterlockOp);

  PipelineState *m_pipelineState = nullptr; // Pipeline state
  llvm::Function *m_entryPoint = nullptr;   // Entry-point of fragment shader

  std::unique_ptr<BuilderBase> m_builder; // LLVM IR builder

  // List of POPS interlock operations
  llvm::SmallVector<llvm::Instruction *, 16> m_beginInterlocks;
  llvm::SmallVector<llvm::Instruction *, 16> m_endInterlocks;

  bool m_changed = false; // Whether the IR is changed by this pass
};

} // namespace lgc
