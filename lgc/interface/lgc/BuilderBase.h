/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderBase.h
 * @brief LLPC header file: declaration of BuilderBase
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/IRBuilder.h"

namespace lgc {

// =====================================================================================================================
// BuilderBase extends IRBuilder<>, and provides a few utility methods used in both the LLPC front-end and in LGC (the
// LLPC middle-end). LGC code outside of Builder subclasses would use BuilderBase directly; LLPC front-end code gets
// to use BuilderBase methods because it uses Builder, which inherits from BuilderBase.
//
class BuilderBase : public llvm::IRBuilder<> {
public:
  // Constructors
  BuilderBase(llvm::LLVMContext &context) : IRBuilder(context) {}
  BuilderBase(llvm::BasicBlock *block) : IRBuilder(block) {}
  BuilderBase(llvm::Instruction *inst) : IRBuilder(inst) {}

  // Create an LLVM function call to the named function. The callee is built automically based on return
  // type and its parameters.
  //
  // @param funcName : Name of the callee
  // @param retTy : Return type of the callee
  // @param args : Arguments to pass to the callee
  // @param attribs : Function attributes
  // @param instName : Name to give instruction
  llvm::CallInst *CreateNamedCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                                  llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, const llvm::Twine &instName = "");

  // Emits a amdgcn.reloc.constant intrinsic that represents an i32 relocatable value with the given symbol name
  //
  // @param symbolName : Name of the relocation symbol associated with this relocation
  llvm::Value *CreateRelocationConstant(const llvm::Twine &symbolName);

  // Generate an add of an offset to a byte pointer. This is provided to use in the case that the offset is,
  // or might be, a relocatable value, as it implements a workaround to get more efficient code for the load
  // that uses the offset pointer.
  //
  // @param pointer : Pointer to add to
  // @param byteOffset : Byte offset to add
  // @param instName : Name to give instruction
  llvm::Value *CreateAddByteOffset(llvm::Value *pointer, llvm::Value *byteOffset, const llvm::Twine &instName = "");
};

} // namespace lgc
