/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/IR/IRBuilder.h"

namespace lgc {

// =====================================================================================================================
// BuilderCommon extends llvm_dialects::Builder, which extends llvm::IRBuilder<>, and provides a few utility methods
// used in both the LLPC front-end and in LGC (the LLPC middle-end).
// This class is used directly by passes in LGC.
class BuilderCommon : public llvm_dialects::Builder {
public:
  // Constructors
  BuilderCommon(llvm::LLVMContext &context) : llvm_dialects::Builder(context) {}
  BuilderCommon(llvm::BasicBlock *block) : llvm_dialects::Builder(block) {}
  BuilderCommon(llvm::Instruction *inst) : llvm_dialects::Builder(inst) {}

  // Return the i64 difference between two pointers, dividing out the size of the pointed-to objects.
  // For buffer fat pointers, delays the translation to patch phase.
  //
  // @param ty : Element type of the pointers.
  // @param lhs : Left hand side of the subtraction.
  // @param rhs : Reft hand side of the subtraction.
  // @param instName : Name to give instruction(s)
  llvm::Value *CreatePtrDiff(llvm::Type *ty, llvm::Value *lhs, llvm::Value *rhs, const llvm::Twine &instName = "");

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
