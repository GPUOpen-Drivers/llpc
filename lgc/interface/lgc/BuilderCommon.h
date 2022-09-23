/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderCommon.h
 * @brief LLPC header file: declaration of BuilderCommon
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/IRBuilder.h"

namespace lgc {

// =====================================================================================================================
// BuilderCommon extends IRBuilder<>, and provides a few utility methods used in both the LLPC front-end and in LGC
// (the LLPC middle-end).
// This class is used directly by passes in LGC.
class BuilderCommon : public llvm::IRBuilder<> {
public:
  // Constructors
  BuilderCommon(llvm::LLVMContext &context) : IRBuilder(context) {}
  BuilderCommon(llvm::BasicBlock *block) : IRBuilder(block) {}
  BuilderCommon(llvm::Instruction *inst) : IRBuilder(inst) {}

  // Create an LLVM function call to the named function. The callee is built automatically based on return
  // type and its parameters.
  //
  // @param funcName : Name of the callee
  // @param retTy : Return type of the callee
  // @param args : Arguments to pass to the callee
  // @param attribs : Function attributes
  // @param instName : Name to give instruction
  llvm::CallInst *CreateNamedCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                                  llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, const llvm::Twine &instName = "");
};

} // namespace lgc
