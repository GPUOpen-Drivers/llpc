/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Internal.cpp
 * @brief LLPC source file: contains implementation of LLPC internal-use utility functions.
 ***********************************************************************************************************************
 */
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_os_ostream.h"

#if !defined(_WIN32)
#include <time.h>
#endif

#if __APPLE__ && __MACH__
#include <mach/mach_time.h>
#endif

#include "lgc/util/BuilderBase.h"
#include "lgc/util/Internal.h"

#define DEBUG_TYPE "lgc-internal"

using namespace llvm;

namespace lgc {

// =====================================================================================================================
// Emits a LLVM function call (inserted before the specified instruction), builds it automatically based on return type
// and its parameters.
//
// NOTE: Prefer BuilderBase::CreateNamedCall where possible.
//
// @param funcName : Name string of the function
// @param retTy : Return type
// @param args : Parameters
// @param attribs : Attributes
// @param insertPos : Where to insert this call
CallInst *emitCall(StringRef funcName, Type *retTy, ArrayRef<Value *> args, ArrayRef<Attribute::AttrKind> attribs,
                   Instruction *insertPos) {
  BuilderBase builder(insertPos);
  return builder.CreateNamedCall(funcName, retTy, args, attribs);
}

// =====================================================================================================================
// Emits a LLVM function call (inserted at the end of the specified basic block), builds it automatically based on
// return type and its parameters.
//
// NOTE: Prefer BuilderBase::CreateNamedCall where possible.
//
// @param funcName : Name string of the function
// @param retTy : Return type
// @param args : Parameters
// @param attribs : Attributes
// @param insertAtEnd : Which block to insert this call at the end
CallInst *emitCall(StringRef funcName, Type *retTy, ArrayRef<Value *> args, ArrayRef<Attribute::AttrKind> attribs,
                   BasicBlock *insertAtEnd) {
  BuilderBase builder(insertAtEnd);
  return builder.CreateNamedCall(funcName, retTy, args, attribs);
}

// =====================================================================================================================
// Gets LLVM-style name for type.
//
// @param ty : Type to get mangle name
// @param [in/out] nameStream : Stream to write the type name into
void getTypeName(Type *ty, raw_ostream &nameStream) {
  for (;;) {
    if (auto pointerTy = dyn_cast<PointerType>(ty)) {
      nameStream << "p" << pointerTy->getAddressSpace();
      if (pointerTy->isOpaque())
        return;
      ty = pointerTy->getPointerElementType();
      continue;
    }
    if (auto arrayTy = dyn_cast<ArrayType>(ty)) {
      nameStream << "a" << arrayTy->getNumElements();
      ty = arrayTy->getElementType();
      continue;
    }
    break;
  }

  if (auto structTy = dyn_cast<StructType>(ty)) {
    nameStream << "s[";
    if (structTy->getNumElements() != 0) {
      getTypeName(structTy->getElementType(0), nameStream);
      for (unsigned i = 1; i < structTy->getNumElements(); ++i) {
        nameStream << ",";
        getTypeName(structTy->getElementType(i), nameStream);
      }
    }
    nameStream << "]";
    return;
  }

  if (auto vectorTy = dyn_cast<FixedVectorType>(ty)) {
    nameStream << "v" << vectorTy->getNumElements();
    ty = vectorTy->getElementType();
  }
  if (ty->isFloatingPointTy())
    nameStream << "f" << ty->getScalarSizeInBits();
  else if (ty->isIntegerTy())
    nameStream << "i" << ty->getScalarSizeInBits();
  else if (ty->isVoidTy())
    nameStream << "V";
  else
    llvm_unreachable("Should never be called!");
}

// =====================================================================================================================
// Gets LLVM-style name for type.
//
// @param ty : Type to get mangle name
std::string getTypeName(Type *ty) {
  std::string name;
  raw_string_ostream nameStream(name);

  getTypeName(ty, nameStream);
  return nameStream.str();
}

// =====================================================================================================================
// Adds LLVM-style type mangling suffix for the specified return type and args to the name.
//
// @param returnTy : Return type (could be null)
// @param args : Arguments
// @param [out] name : String to add mangling to
void addTypeMangling(Type *returnTy, ArrayRef<Value *> args, std::string &name) {
  size_t nameLen = name.length();
  if (name[nameLen - 1] == '.') {
    // NOTE: If the specified name is ended with ".", we remove it in that mangling suffix starts with "." as well.
    name.erase(nameLen - 1, 1);
  }

  raw_string_ostream nameStream(name);
  if (returnTy && !returnTy->isVoidTy()) {
    nameStream << ".";
    getTypeName(returnTy, nameStream);
  }

  for (auto arg : args) {
    nameStream << ".";
    getTypeName(arg->getType(), nameStream);
  }
}

// =====================================================================================================================
// Gets the argument from the specified function according to the argument index.
//
// @param func : LLVM function
// @param idx : Index of the query argument
// @param name : Name to give the argument if currently empty
Argument *getFunctionArgument(Function *func, unsigned idx, const Twine &name) {
  assert(idx < func->arg_end() - func->arg_begin() && "Out of range function argument");
  Argument *arg = &func->arg_begin()[idx];
  if (!name.isTriviallyEmpty() && arg->getName() == "")
    arg->setName(name);
  return arg;
}

// =====================================================================================================================
// Checks if one type can be bitcasted to the other (type1 -> type2, valid for scalar or vector type).
//
// @param ty1 : One type
// @param ty2 : The other type
bool canBitCast(const Type *ty1, const Type *ty2) {
  bool valid = false;

  if (ty1 == ty2)
    valid = true;
  else if (ty1->isSingleValueType() && ty2->isSingleValueType()) {
    const Type *compTy1 = ty1->isVectorTy() ? cast<VectorType>(ty1)->getElementType() : ty1;
    const Type *compTy2 = ty2->isVectorTy() ? cast<VectorType>(ty2)->getElementType() : ty2;
    if ((compTy1->isFloatingPointTy() || compTy1->isIntegerTy()) &&
        (compTy2->isFloatingPointTy() || compTy2->isIntegerTy())) {
      const unsigned compCount1 = ty1->isVectorTy() ? cast<FixedVectorType>(ty1)->getNumElements() : 1;
      const unsigned compCount2 = ty2->isVectorTy() ? cast<FixedVectorType>(ty2)->getNumElements() : 1;

      valid = compCount1 * compTy1->getScalarSizeInBits() == compCount2 * compTy2->getScalarSizeInBits();
    }
  }

  return valid;
}

// =====================================================================================================================
// Checks if the specified value actually represents a don't-care value (0xFFFFFFFF).
//
// @param value : Value to check
bool isDontCareValue(Value *value) {
  bool isDontCare = false;

  if (isa<ConstantInt>(value))
    isDontCare = static_cast<unsigned>(cast<ConstantInt>(value)->getZExtValue()) == InvalidValue;

  return isDontCare;
}

// =====================================================================================================================
// Given a non-aggregate type, get a float type at least as big that can be used to pass a value of that
// type in a return value struct, ensuring it gets into VGPRs.
//
// @param ty : A type
Type *getVgprTy(Type *ty) {
  // The return value from the fetch shader needs to use all floats, as the back-end maps an int in the
  // return value as an SGPR rather than a VGPR. For symmetry, we also use all floats in the input
  // args to the fetchless vertex shader. We use a vector of float, even if the integer element type is
  // not i32.
  unsigned numElements = (ty->getPrimitiveSizeInBits() + 31) / 32;
  ty = Type::getFloatTy(ty->getContext());
  if (numElements > 1)
    ty = FixedVectorType::get(ty, numElements);
  return ty;
}

} // namespace lgc
