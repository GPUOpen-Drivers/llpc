/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  LowerDebugPrintf.h
 * @brief LLPC header file : contains declaration of class lgc::LowerDebugPrintf.h
 ***********************************************************************************************************************
 */
#pragma once
#include "SystemValues.h"
#include "lgc/Builder.h"
#include "lgc/lowering/LgcLowering.h"
#include "lgc/state/PipelineShaders.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/IR/Function.h"

namespace lgc {

class DebugPrintfOp;
class AbortMsgOp;
struct ResourceNode;

// =====================================================================================================================
// Pass to lower debug.printf calls
class LowerDebugPrintf : public llvm::PassInfoMixin<LowerDebugPrintf> {
  struct ElfInfo {
    llvm::StringRef formatString;  // Printf format string
    llvm::SmallBitVector bit64Pos; // 64bit position records output variable 32bit/64bit condition.
  };

public:
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "Lower debug printf calls"; }

private:
  void visitDebugPrintf(DebugPrintfOp &op);
  void writeToDebugPrintfBuffer(uint64_t header, llvm::Value *debugPrintfBuffer,
                                llvm::SmallVectorImpl<llvm::Value *> &varData, BuilderBase &builder);
  void getDwordValues(llvm::Value *val, llvm::SmallVectorImpl<llvm::Value *> &output,
                      llvm::SmallBitVector &output64Bits, BuilderBase &builder);
  void setupElfsPrintfStrings();
  llvm::DenseMap<uint64_t, ElfInfo> m_elfInfos;
  llvm::SmallVector<llvm::Instruction *> m_toErase;
  llvm::Value *m_debugPrintfBuffer = nullptr;
  PipelineState *m_pipelineState = nullptr;
  const ResourceNode *m_topNode = nullptr;
};

} // namespace lgc
