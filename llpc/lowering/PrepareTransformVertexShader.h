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
 * @file  PrepareTransformVertexShader.h
 * @brief LLPC header file: contains declaration of Llpc::PrepareTransformVertexShader
 ***********************************************************************************************************************
 */
#pragma once
#include "Lowering.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class Value;
class User;
class ReturnInst;
} // namespace llvm

namespace Llpc {
class PrepareTransformVertexShader : public Lowering, public llvm::PassInfoMixin<PrepareTransformVertexShader> {
public:
  PrepareTransformVertexShader() {}
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

private:
  enum VertexOutputBuiltIn : unsigned {
    Position = 0,
    ClipDistance0 = 1,
    ClipDistance1 = 2,
    FrontColor = 3,
    TexCoord = 4,
    OutputCount = 5
  };

  enum VertexInputBuiltIn : unsigned {
    VertexIndex = 0,
    InstanceIndex = 1,
    DrawID = 2,
    BaseVertex = 3,
    BaseInstance = 4,
    InputCount = 5,
  };

  void collectVtxBuiltInSymbols(llvm::Module &module);
  void genFunTransformVertex(llvm::Function &function);
  llvm::Value *loadClipDistanceComponent(unsigned index, unsigned component);
  llvm::User *m_outputBuiltIns[OutputCount] = {nullptr};
  llvm::User *m_inputBuiltIns[InputCount] = {nullptr};
  llvm::ReturnInst *m_unifiedReturn = nullptr;
};
} // namespace Llpc
