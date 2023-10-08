/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *deal in the Software without restriction, including without limitation the
 *rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

//===- DXILMetadata.cpp - Generators, decoders and wrappers for metadata --===//
//
// This file implements metadata functions for the DXIL continuations
//
//===----------------------------------------------------------------------===//

#include "continuations/Continuations.h"

namespace llvm {

DXILContArgTy::DXILContArgTy(Type *Arg) {
  assert(!Arg->isPointerTy() &&
         "pointers are not supported by this constructor");
  ArgTy = Arg;
  ElemTy = nullptr;
}

DXILContArgTy DXILContArgTy::get(const Function *F, const Argument *Arg) {
  // only consult metadata for pointer types
  Type *ArgTy = Arg->getType();
  if (!ArgTy->isPointerTy())
    return DXILContArgTy(ArgTy, nullptr);

  // types metadata of the form { !"function", <return-type>,
  // <argument-0-type>, ... }
  auto *TypesMD = F->getMetadata(DXILContHelper::MDTypesName);
  if (TypesMD) {
    unsigned ArgNo = Arg->getArgNo() + 2;
    assert(ArgNo < TypesMD->getNumOperands() &&
           "insufficient operands in types metadata");

    DXILContArgTy Result = get(&*TypesMD->getOperand(ArgNo), F->getContext());

    return Result;
  }

  report_fatal_error("Missing metadata for pointer type!");
}

DXILContArgTy DXILContArgTy::get(const Function *F, const unsigned ArgNo) {
  return get(F, F->getArg(ArgNo));
}

DXILContArgTy DXILContArgTy::get(const Metadata *MD, LLVMContext &Context) {
  if (const auto *ConstantMD = dyn_cast<ConstantAsMetadata>(MD)) {
    return DXILContArgTy(ConstantMD->getType(), nullptr);
  }
  if (const auto *StringMD = dyn_cast<MDString>(MD)) {
    assert(StringMD->getString() == DXILContHelper::MDTypesVoidName &&
           "unknown string in types metadata");
    return DXILContArgTy(Type::getVoidTy(Context));
  }
  if (const auto *PointerMD = dyn_cast<MDNode>(MD)) {
    assert(PointerMD && PointerMD->getNumOperands() == 2 &&
           "invalid pointer metadata");

    auto *AddressSpaceMD =
        dyn_cast<ConstantAsMetadata>(PointerMD->getOperand(0));
    assert(AddressSpaceMD && "invalid address space metadata");
    auto *AddressSpace = dyn_cast<ConstantInt>(AddressSpaceMD->getValue());
    assert(AddressSpace && "invalid address space metadata");

    if (const auto *ValueMD =
            dyn_cast<ValueAsMetadata>(PointerMD->getOperand(1))) {
      Type *ElemTy = ValueMD->getType();
      Type *PtrTy =
          ElemTy->getPointerTo((unsigned)AddressSpace->getZExtValue());
      return DXILContArgTy(PtrTy, ElemTy);
    }
  }

  assert(false && "unknown node type in types metadata");
  return DXILContArgTy(Type::getVoidTy(Context));
}

Type *DXILContArgTy::asType(LLVMContext &Context) { return ArgTy; }

Type *DXILContArgTy::getPointerElementType() const {
  assert(ElemTy && "cannot get element type of non-pointer");
  return ElemTy;
}

bool DXILContArgTy::isPointerTy() const { return !!ElemTy; }

bool DXILContArgTy::isVoidTy() const { return (!ArgTy || ArgTy->isVoidTy()); }

Metadata *DXILContArgTy::getTypeMetadata(LLVMContext &Context) {
  if (isVoidTy())
    return MDString::get(Context, DXILContHelper::MDTypesVoidName);

  if (!ElemTy) {
    assert(ArgTy && !ArgTy->isPointerTy());
    return ConstantAsMetadata::get(PoisonValue::get(ArgTy));
  }

  assert(!ElemTy->isFunctionTy() && "cannot encode function pointers");

  // Return !{<addrspace>, <inner>} for pointer
  SmallVector<Metadata *, 2> MD;
  MD.push_back(ConstantAsMetadata::get(ConstantInt::get(
      Type::getInt32Ty(Context), ArgTy->getPointerAddressSpace())));
  MD.push_back(ConstantAsMetadata::get(PoisonValue::get(ElemTy)));
  return MDTuple::get(Context, MD);
}

DXILContFuncTy DXILContFuncTy::get(const Function *F) {
  auto *TypesMD = F->getMetadata(DXILContHelper::MDTypesName);
  assert(TypesMD);

  return get(TypesMD, F->getContext());
}

DXILContFuncTy DXILContFuncTy::get(const Metadata *MD, LLVMContext &Context) {
  // Decode types metadata of the form { !"function", <return-type>,
  // <argument-0-type>, ... }
  const MDNode *TypesMD = dyn_cast<MDNode>(MD);
  if (!TypesMD)
    report_fatal_error("Invalid metadata type for function.");

  assert(TypesMD->getNumOperands() >= 2 && "invalid function metadata");
  assert(isa<MDString>(TypesMD->getOperand(0)) &&
         dyn_cast<MDString>(TypesMD->getOperand(0))->getString() ==
             DXILContHelper::MDTypesFunctionName &&
         "metadata is not a function type");

  DXILContFuncTy FuncTy;
  for (unsigned OpNo = 1; OpNo < TypesMD->getNumOperands(); ++OpNo) {
    Metadata *Arg = TypesMD->getOperand(OpNo);
    FuncTy.ArgTys.push_back(DXILContArgTy::get(Arg, Context));
  }
  // FIXME: do something more efficient
  assert(FuncTy.ArgTys.size() >= 1);
  FuncTy.ReturnTy = FuncTy.ArgTys[0];
  FuncTy.ArgTys.erase(FuncTy.ArgTys.begin());
  return FuncTy;
}

FunctionType *DXILContFuncTy::asFunctionType(LLVMContext &Context) {
  SmallVector<Type *> FuncArgTys;
  for (auto Arg : ArgTys)
    FuncArgTys.push_back(Arg.asType(Context));
  return FunctionType::get(ReturnTy.asType(Context), FuncArgTys, false);
}

void DXILContFuncTy::writeMetadata(Function *F) {
  // Don't generate metadata if there are no pointers
  if (!ReturnTy.isPointerTy() &&
      llvm::none_of(ArgTys,
                    [](const DXILContArgTy &Arg) { return Arg.isPointerTy(); }))
    return;

  LLVMContext &Context = F->getContext();
  SmallVector<Metadata *> SignatureMD;

  // Encode types metadata of the form { !"function", <return-type>,
  // <argument-0-type>, ... }
  SignatureMD.push_back(
      MDString::get(Context, DXILContHelper::MDTypesFunctionName));
  SignatureMD.push_back(ReturnTy.getTypeMetadata(Context));
  for (auto ArgTy : ArgTys)
    SignatureMD.push_back(ArgTy.getTypeMetadata(Context));

  assert(SignatureMD.size() >= 2 && "return type must be specified");
  F->setMetadata(DXILContHelper::MDTypesName,
                 MDTuple::get(Context, SignatureMD));
}

static Metadata *getTypeMetadataEntry(unsigned TypeID, LLVMContext &Context,
                                      GetTypeByIDTy GetTypeByID,
                                      GetContainedTypeIDTy GetContainedTypeID);

// Recursively look into a (pointer) type and build metadata description.
// For primitive types it's a poison value of the type, for a pointer it's a
// metadata tuple with the addrspace and the referenced type. For a function,
// it's a tuple where the first element is the string "function", the second
// element is the return type or the string "void" and the following elements
// are the argument types.
static Metadata *
getTypeMetadataEntryImpl(Type *Ty, unsigned TypeID, LLVMContext &Context,
                         GetTypeByIDTy GetTypeByID,
                         GetContainedTypeIDTy GetContainedTypeID) {
  if (auto *FTy = dyn_cast<FunctionType>(Ty)) {
    // Don't generate metadata if there are no pointers
    if (!FTy->getReturnType()->isPointerTy() &&
        llvm::none_of(FTy->params(), [](const Type *ParamTy) {
          return ParamTy->isPointerTy();
        }))
      return nullptr;
    // Save the function signature as metadata
    SmallVector<Metadata *> SignatureMD;
    SignatureMD.push_back(
        MDString::get(Context, DXILContHelper::MDTypesFunctionName));
    // Return type
    if (FTy->getReturnType()->isVoidTy()) {
      SignatureMD.push_back(
          MDString::get(Context, DXILContHelper::MDTypesVoidName));
    } else {
      SignatureMD.push_back(getTypeMetadataEntry(GetContainedTypeID(TypeID, 0),
                                                 Context, GetTypeByID,
                                                 GetContainedTypeID));
    }

    // Arguments
    for (unsigned I = 0; I != FTy->getNumParams(); ++I) {
      SignatureMD.push_back(
          getTypeMetadataEntry(GetContainedTypeID(TypeID, I + 1), Context,
                               GetTypeByID, GetContainedTypeID));
    }

    return MDTuple::get(Context, SignatureMD);
  }

  if (!Ty->isPointerTy())
    return ConstantAsMetadata::get(PoisonValue::get(Ty));

  // Return !{<addrspace>, <inner>} for pointer
  SmallVector<Metadata *, 2> MD;
  MD.push_back(ConstantAsMetadata::get(ConstantInt::get(
      Type::getInt32Ty(Context), Ty->getPointerAddressSpace())));
  MD.push_back(getTypeMetadataEntry(GetContainedTypeID(TypeID, 0), Context,
                                    GetTypeByID, GetContainedTypeID));
  return MDTuple::get(Context, MD);
}

static Metadata *getTypeMetadataEntry(unsigned TypeID, LLVMContext &Context,
                                      GetTypeByIDTy GetTypeByID,
                                      GetContainedTypeIDTy GetContainedTypeID) {
  auto *Ty = GetTypeByID(TypeID);
  Metadata *MD = getTypeMetadataEntryImpl(Ty, TypeID, Context, GetTypeByID,
                                          GetContainedTypeID);
  if (!MD)
    return nullptr;

  assert(((Ty->isFunctionTy() &&
           DXILContFuncTy::get(MD, Context).asFunctionType(Context) == Ty) ||
          (!Ty->isFunctionTy() &&
           DXILContArgTy::get(MD, Context).asType(Context) == Ty)) &&
         "MD Type mismatch");
  return MD;
}

void DXILValueTypeMetadataCallback(Value *V, unsigned TypeID,
                                   GetTypeByIDTy GetTypeByID,
                                   GetContainedTypeIDTy GetContainedTypeID) {
  if (auto *F = dyn_cast<llvm::Function>(V)) {
    auto *MD = getTypeMetadataEntry(TypeID, F->getContext(), GetTypeByID,
                                    GetContainedTypeID);
    if (MD)
      F->setMetadata(DXILContHelper::MDTypesName, llvm::cast<llvm::MDNode>(MD));
  }
}

} // End namespace llvm
