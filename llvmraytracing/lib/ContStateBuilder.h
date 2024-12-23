/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- ContStateBuilder.h - A custom ABI for LLVM Coroutines---------------===//
// This file defines Continuations Passing Style Return-Continuation ABI for
// LLVM coroutine transforms that is used to build the cont state buffer.
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/Transforms/Coroutines/ABI.h"

namespace llvmraytracing {

class ContStateBuilder : public llvm::coro::AnyRetconABI {
public:
  ContStateBuilder(llvm::Function &F, llvm::coro::Shape &S, std::function<bool(llvm::Instruction &I)> IsMaterializable);

  // Allocate the coroutine frame and do spill/reload as needed.
  void buildCoroutineFrame(bool OptimizeFrame) override;
};

} // namespace llvmraytracing
