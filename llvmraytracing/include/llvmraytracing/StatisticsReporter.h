/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- StatisticsReporter.h - Reporting utility -----------------------------===//
//
// Helpers to report statistics relevant to Continuations
//
//===----------------------------------------------------------------------===//

#pragma once

#include "lgc/LgcRtDialect.h"

namespace llvm {
class Function;
} // namespace llvm

// A self-contained class that handles per-function statistics reporting.
class StatisticsReporter final {
private:
  struct FunctionData {
    llvm::Function *Func;
    std::optional<lgc::rt::RayTracingShaderStage> Stage = std::nullopt;
  };

  bool shouldReport() const;
  void reportContStateSizes(const FunctionData &FuncData);
  void reportPayloadRegisterSizes(const FunctionData &FuncData);
  void reportSystemDataSizes(const FunctionData &FuncData);

public:
  void report(llvm::Function &Func);
};
