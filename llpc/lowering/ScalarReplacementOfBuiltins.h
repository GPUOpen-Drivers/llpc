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
 * @file  ScalarReplacementOfBuiltins.h
 * @brief LLPC header file: split and replace global variables that are structures containing built-in values.
 ***********************************************************************************************************************
 */
#pragma once

#include "Lowering.h"
#include "SPIRVInternal.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

namespace Llpc {

// =====================================================================================================================
// Pass that splits and replaces global variables that are structures containing built-in values
class ScalarReplacementOfBuiltins : public Lowering, public llvm::PassInfoMixin<ScalarReplacementOfBuiltins> {
public:
  ScalarReplacementOfBuiltins() {}
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);

  static llvm::StringRef name() { return "Scalar replacement of builtins"; }

private:
  ShaderInOutMetadata getShaderInOutMetadata(Type *elementType, Constant *elementMetadata);
  bool needsSplit(GlobalVariable *builtinGlobalVar);
  StringRef getBuiltinElementName(ShaderInOutMetadata &inOutMeta);
  void cleanUpUnusedGlobals(SmallVector<User *> &elements);
  void replaceGlobalBuiltinVar(GlobalVariable *builtinGlobalVar, SmallVector<User *> &elements);
  void splitBuiltinStructure(GlobalVariable *builtinGlobalVar);
  void splitBuiltinArray(GlobalVariable *builtinGlobalVar);
};

} // namespace Llpc
