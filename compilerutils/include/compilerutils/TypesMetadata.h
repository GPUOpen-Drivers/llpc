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

//===- TypesMetadata.h - Pointee type metadata for processing DXIL ---------==//

#pragma once

#include "llvm/Bitcode/BitcodeReader.h"

namespace llvm {

class MDTuple;

// A function argument type and pointee type.
class TypedArgTy {
private:
  Type *ArgTy = nullptr;
  Type *ElemTy = nullptr;

public:
  TypedArgTy() {}
  TypedArgTy(Type *Arg);
  TypedArgTy(Type *Arg, Type *Elem);

  static TypedArgTy get(const Argument *Arg);
  static TypedArgTy get(const Function *F, const unsigned ArgNo);

  Type *asType() const { return ArgTy; }
  Type *getPointerElementType() const;

  bool isPointerTy() const;
  bool isVoidTy() const;
  Metadata *getTypeMetadata(LLVMContext &Context);

  bool operator==(const TypedArgTy &RHS) const { return (ArgTy == RHS.ArgTy) && (ElemTy == RHS.ElemTy); }
};

// A wrapper round FunctionType and metadata for the pointee type(s) of any pointer return type and parameters.
class TypedFuncTy {
public:
  TypedFuncTy() {}

  // Construct a TypedFuncTy for the given result type and arg types.
  // This constructs the !pointeetys metadata; that can then be attached to a function
  // using writeMetadata().
  TypedFuncTy(TypedArgTy ResultTy, ArrayRef<TypedArgTy> ArgTys);

  // Get a TypedFuncTy for the given Function, looking up the !pointeetys metadata.
  static TypedFuncTy get(const Function *F);

  // Get the IR FunctionType.
  FunctionType *asFunctionType() const { return FuncTy; }

  // Get a TypedArgTy for the return type.
  TypedArgTy getReturnType() const;

  // Get a TypedArgTy for a parameter type.
  TypedArgTy getParamType(unsigned Idx) const;

  // Push a TypedArgTy for each parameter onto the supplied vector.
  void getParamTypes(SmallVectorImpl<TypedArgTy> &ArgTys) const;

  // Write the metadata (if any) onto the specified function. Typically used when creating a new function
  // and using our constructor that takes TypedArgTy for return type and arg types.
  void writeMetadata(Function *F) const;

  static constexpr const char *MDTypesName = "pointeetys";

private:
  // Shared code for getReturnType and getParamType. This decodes the !pointeetys metadata.
  Type *getPointeeType(Type *Ty, unsigned Idx) const;

  FunctionType *FuncTy = nullptr;
  MDTuple *Meta = nullptr;
};

/// Return element type of a function argument resolving opaque pointers
/// via !pointeetys metadata where appropriate.
/// Returns nullptr for non-pointers.
Type *getFuncArgPtrElementType(const Argument *Arg);

/// Return element type of a function argument resolving opaque pointers
/// via !pointeetys metadata where appropriate.
/// Returns nullptr for non-pointers.
Type *getFuncArgPtrElementType(const Function *F, int ArgNo);

/// Get element type of function return type resolving opaque pointers
/// via !pointeetys metadata where appropriate.
Type *getFuncReturnPtrElementType(const Function *F);

/// LLVM parser callback which adds !pointeetys metadata during DXIL parsing
void DXILValueTypeMetadataCallback(Value *V, unsigned TypeID, GetTypeByIDTy GetTypeByID,
                                   GetContainedTypeIDTy GetContainedTypeID);

} // namespace llvm
