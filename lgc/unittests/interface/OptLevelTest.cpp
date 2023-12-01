/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2022 Google LLC. All Rights Reserved.
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

#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Target/TargetMachine.h"
#include "gmock/gmock.h"

using namespace lgc;
using namespace llvm;
using namespace llvm::CodeGenOpt;

TEST(LgcInterfaceTests, DefaultOptLevel) {
  LgcContext::initialize();
  LLVMContext context;
  auto dialectContext = llvm_dialects::DialectContext::make<LgcDialect>(context);

  unsigned palAbiVersion = 0xFFFFFFFF;
  StringRef gpuName = "gfx1010";

#if LLVM_MAIN_REVISION && LLVM_MAIN_REVISION < 474768
  // Old version of the code
  for (auto optLevel : {Level::None, Level::Less, Level::Default, Level::Aggressive}) {
#else
  // New version of the code (also handles unknown version, which we treat as latest)
  // Returns the optimization level for the context.
  for (auto optLevel :
       {CodeGenOptLevel::None, CodeGenOptLevel::Less, CodeGenOptLevel::Default, CodeGenOptLevel::Aggressive}) {
#endif
    std::unique_ptr<TargetMachine> targetMachine = LgcContext::createTargetMachine(gpuName, optLevel);
    std::unique_ptr<LgcContext> lgcContext(LgcContext::create(&*targetMachine, context, palAbiVersion));
    EXPECT_EQ(lgcContext->getOptimizationLevel(), optLevel);
  }
}
