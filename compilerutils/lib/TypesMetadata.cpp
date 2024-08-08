/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- TypesMetadata.cpp - Generators, decoders and wrappers for metadata --==//
//
// This file implements metadata functions for the DXIL continuations
//
//===----------------------------------------------------------------------===//

// The metadata format is as follows:
//
// A function that has pointer return type or param type has !pointeetys metadata attached, which is a tuple.
// There are two formats, the simple format and the general format.
//
// Simple format, used if there is no more than one pointer param:
// - If the return type is a pointer, an entry for it.
// - If there is a pointer param, an entry for it.
// So the simple format could be one entry (either return type or a single param) or two entries (return type
// plus single param).
//
// General format, used if there is more than one pointer param:
// - An entry for the return type (null if it is not a pointer)
// - An entry for each parameter (null if it is not a pointer).
// Trailing null entries are truncated from the tuple.
//
// In either format, each entry is a poison value of the pointee type, or (for the general format) null
// if the corresponding return type or param is not a pointer.

#include "compilerutils/TypesMetadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"

using namespace llvm;

TypedArgTy::TypedArgTy(Type *Arg) {
  assert(!Arg->isPointerTy() && "pointers are not supported by this constructor");
  ArgTy = Arg;
  ElemTy = nullptr;
}

TypedArgTy::TypedArgTy(Type *Arg, Type *Elem) : ArgTy(Arg), ElemTy(Elem) {
  assert(isa<PointerType>(asType()) == isPointerTy() &&
         "Pointer result must have pointee type, and non-pointer result must not have pointee type");
}

TypedArgTy TypedArgTy::get(const Argument *Arg) {
  // only consult metadata for pointer types
  Type *ArgTy = Arg->getType();
  if (!ArgTy->isPointerTy())
    return TypedArgTy(ArgTy, nullptr);
  return TypedFuncTy::get(Arg->getParent()).getParamType(Arg->getArgNo());
}

TypedArgTy TypedArgTy::get(const Function *F, const unsigned ArgNo) {
  return get(F->getArg(ArgNo));
}

Type *TypedArgTy::getPointerElementType() const {
  assert(ElemTy && "cannot get element type of non-pointer");
  return ElemTy;
}

bool TypedArgTy::isPointerTy() const {
  return !!ElemTy;
}

bool TypedArgTy::isVoidTy() const {
  return ArgTy->isVoidTy();
}

// Get a TypedFuncTy for the given Function, looking up the !pointeetys metadata.
TypedFuncTy TypedFuncTy::get(const Function *F) {
  TypedFuncTy Result;
  Result.FuncTy = F->getFunctionType();
  Result.Meta = dyn_cast_or_null<MDTuple>(F->getMetadata(MDTypesName));
  return Result;
}

// Construct a TypedFuncTy for the given result type and arg types.
// This constructs the !pointeetys metadata; that can then be attached to a function
// using writeMetadata().
TypedFuncTy::TypedFuncTy(TypedArgTy ResultTy, ArrayRef<TypedArgTy> ArgTys) {
  SmallVector<Type *> BareArgTys;
  SmallVector<Metadata *> PointeeTys;
  unsigned SimpleFormatArgIdx = UINT_MAX;
  if (ResultTy.isPointerTy())
    PointeeTys.push_back(ConstantAsMetadata::get(PoisonValue::get(ResultTy.getPointerElementType())));
  for (unsigned ArgIdx = 0; ArgIdx != ArgTys.size(); ++ArgIdx) {
    const TypedArgTy &ArgTy = ArgTys[ArgIdx];
    BareArgTys.push_back(ArgTy.asType());
    if (ArgTy.isPointerTy()) {
      // Pointer arg. Add its pointee type to the array that will form the metadata tuple.
      Metadata *PointeeTy = ConstantAsMetadata::get(PoisonValue::get(ArgTy.getPointerElementType()));
      if (PointeeTys.size() <= 1) {
        if (SimpleFormatArgIdx == UINT_MAX) {
          // In simple format, and we can stay in simple format.
          SimpleFormatArgIdx = ArgIdx;
          PointeeTys.push_back(PointeeTy);
        } else {
          // In simple format, but this is the second pointer arg so we need to transition to general format.
          // That involves moving the first pointer arg pointee type to its proper index.
          unsigned FirstPointerArgIdx = PointeeTys.size() - 1;
          PointeeTys.resize(ArgIdx + 2);
          std::swap(PointeeTys[FirstPointerArgIdx], PointeeTys[SimpleFormatArgIdx + 1]);
          PointeeTys.back() = PointeeTy;
        }
      } else {
        // Already in general format.
        PointeeTys.resize(ArgIdx + 2);
        PointeeTys.back() = PointeeTy;
      }
    }
  }
  FuncTy = FunctionType::get(ResultTy.asType(), BareArgTys, /*isVarArg=*/false);
  if (!PointeeTys.empty())
    Meta = MDTuple::get(FuncTy->getContext(), PointeeTys);
}

// Shared code for getReturnType and getParamType. This decodes the !pointeetys metadata.
//
// @param Ty : Return type or parameter type, just so we can check it is actually a pointer
// @param Idx : 0 for return type, N+1 for param N
Type *TypedFuncTy::getPointeeType(Type *Ty, unsigned Idx) const {
  if (!isa<PointerType>(Ty))
    return nullptr;
  Type *PointeeTy = nullptr;
  if (Meta) {
    Metadata *Entry = nullptr;
    unsigned NumOperands = Meta->getNumOperands();
    if (Idx == 0) {
      // Getting return pointee type. That works the same in simple or general format.
      if (NumOperands >= 1)
        Entry = Meta->getOperand(0);
    } else {
      if (NumOperands == 1 || (NumOperands == 2 && isa<PointerType>(FuncTy->getReturnType()))) {
        // Simple format (only one entry, or two entries where the first one is the return pointee type).
        Entry = Meta->getOperand(NumOperands - 1);
      } else {
        // General format.
        Entry = Meta->getOperand(Idx);
      }
    }
    if (Entry)
      PointeeTy = dyn_cast<ConstantAsMetadata>(Entry)->getType();
  }
  assert(PointeeTy && "Malformed pointee type metadata");
  return PointeeTy;
}

// Write the metadata (if any) onto the specified function. Typically used when creating a new function
// and using our constructor that takes TypedArgTy for return type and arg types.
void TypedFuncTy::writeMetadata(Function *F) const {
  assert(F->getFunctionType() == FuncTy);
  if (Meta)
    F->setMetadata(MDTypesName, Meta);
}

// Get a TypedArgTy for the return type.
TypedArgTy TypedFuncTy::getReturnType() const {
  Type *Ty = FuncTy->getReturnType();
  return TypedArgTy(Ty, getPointeeType(Ty, 0));
}

// Get a TypedArgTy for a parameter type.
TypedArgTy TypedFuncTy::getParamType(unsigned Idx) const {
  Type *Ty = FuncTy->getParamType(Idx);
  return TypedArgTy(Ty, getPointeeType(Ty, Idx + 1));
}

// Push a TypedArgTy for each parameter onto the supplied vector.
void TypedFuncTy::getParamTypes(SmallVectorImpl<TypedArgTy> &ArgTys) const {
  for (unsigned Idx = 0; Idx != FuncTy->getNumParams(); ++Idx)
    ArgTys.push_back(getParamType(Idx));
}

// Return element type of a function argument resolving opaque pointers
// via !pointeetys metadata where appropriate.
// Returns nullptr for non-pointers.
Type *llvm::getFuncArgPtrElementType(const Argument *Arg) {
  auto *ArgTy = Arg->getType();
  if (!ArgTy->isPointerTy())
    return nullptr;

  return TypedArgTy::get(Arg).getPointerElementType();
}

// Return element type of a function argument resolving opaque pointers
// via !pointeetys metadata where appropriate.
// Returns nullptr for non-pointers.
Type *llvm::getFuncArgPtrElementType(const Function *F, int ArgNo) {
  return getFuncArgPtrElementType(F->getArg(ArgNo));
}

/// Get element type of function return type resolving opaque pointers
/// via !pointeetys metadata where appropriate.
Type *llvm::getFuncReturnPtrElementType(const Function *F) {
  if (!isa<PointerType>(F->getFunctionType()->getReturnType()))
    return nullptr;
  return TypedFuncTy::get(F).getReturnType().getPointerElementType();
}

/// LLVM parser callback which adds !pointeetys metadata during DXIL parsing.
void llvm::DXILValueTypeMetadataCallback(Value *V, unsigned TypeID, GetTypeByIDTy GetTypeByID,
                                         GetContainedTypeIDTy GetContainedTypeID) {
  if (auto FuncTy = dyn_cast<FunctionType>(GetTypeByID(TypeID))) {
    // This is a function. Set up the metadata if there are any pointer types.
    TypedArgTy ReturnTy;
    if (isa<PointerType>(FuncTy->getReturnType()))
      ReturnTy = TypedArgTy(FuncTy->getReturnType(), GetTypeByID(GetContainedTypeID(GetContainedTypeID(TypeID, 0), 0)));
    else
      ReturnTy = FuncTy->getReturnType();
    SmallVector<TypedArgTy> ArgTys;
    for (unsigned Idx = 0; Idx != FuncTy->getNumParams(); ++Idx) {
      Type *ArgTy = FuncTy->getParamType(Idx);
      if (isa<PointerType>(ArgTy))
        ArgTys.push_back(TypedArgTy(ArgTy, GetTypeByID(GetContainedTypeID(GetContainedTypeID(TypeID, Idx + 1), 0))));
      else
        ArgTys.push_back(ArgTy);
    }
    TypedFuncTy(ReturnTy, ArgTys).writeMetadata(cast<Function>(V));
  }
}
