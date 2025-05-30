/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  BuilderCommon.h
 * @brief LLPC header file: declaration of BuilderCommon
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm-dialects/Dialect/Builder.h"

namespace lgc {

enum class ResourceNodeType : unsigned;

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

  // Get the type of a descriptor
  //
  // @param descType : Descriptor type, one of the ResourceNodeType values
  llvm::VectorType *getDescTy(ResourceNodeType descType);

  // Get the type of pointer to descriptor.
  llvm::Type *getDescPtrTy();

  // Get the type of pointer returned by CreateLoadBufferDesc.
  llvm::PointerType *getBufferDescTy();

  // Get a constant of FP or vector of FP type from the given APFloat, converting APFloat semantics where necessary
  llvm::Constant *getFpConstant(llvm::Type *ty, llvm::APFloat value);

  // Return the i64 difference between two pointers, dividing out the size of the pointed-to objects.
  // For buffer fat pointers, delays the translation to LGC lowering phase.
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

  // Create an llvm.assume call to annotate the dereferenceable and alignment attributes of the pointer. We only insert
  // the call if dereferenceable > 0 or align > 1.
  //
  // @param ptr : The pointer to be annotated.
  // @param dereferenceable : the dereferenceable size (in bytes) of the pointer
  // @param align : the alignment of the pointer.
  llvm::CallInst *CreateAssumptionDereferenceableAndAlign(llvm::Value *ptr, unsigned dereferenceable, unsigned align);

  // Create code to build a vector out of a number of scalar elements of the same type.
  llvm::Value *CreateBuildVector(llvm::ArrayRef<llvm::Value *> elements, const llvm::Twine &instName = "");

  // Create an "if..endif" or "if..else..endif" structure.
  llvm::BranchInst *CreateIf(llvm::Value *condition, bool wantElse, const llvm::Twine &instName = "");

  // =====================================================================================================================
  // Create alloca for given input type.
  //
  // @param ty : pointer type.
  llvm::Value *CreateAllocaAtFuncEntry(llvm::Type *ty, const llvm::Twine &allocaName = "");

  // Create a "debug break".
  //
  // @param instName : Name to give instruction(s)
  llvm::Instruction *CreateDebugBreak(const llvm::Twine &instName = "");

  // Create an "s_setreg" to set specified bits of a hardware register.
  //
  // @param waveRegIdx : The wave state register Index
  // @param offset: The starting offset
  // @param size: The size of bits
  // @param value : The value to set to the register
  // @param instName : Name to give instruction(s)
  llvm::Instruction *CreateSetReg(unsigned waveRegIdx, unsigned offset, unsigned size, llvm::Value *value,
                                  const llvm::Twine &instName = "");

  // Create an "s_getreg" to get specified bits of a hardware register.
  //
  // @param waveRegIdx : The wave state register Index
  // @param offset: The starting offset
  // @param size: The size of bits
  // @param instName : Name to give instruction(s)
  llvm::Value *CreateGetReg(unsigned waveRegIdx, unsigned offset, unsigned size, const llvm::Twine &instName = "");
};

} // namespace lgc
