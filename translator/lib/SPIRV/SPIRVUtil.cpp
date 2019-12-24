//===- SPIRVUtil.cpp - SPIR-V Utilities -------------------------*- C++ -*-===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file defines utility classes and functions shared by SPIR-V
/// reader/writer.
///
//===----------------------------------------------------------------------===//

#include "FunctionDescriptor.h"
#include "ManglingUtils.h"
#include "NameMangleAPI.h"
#include "ParameterType.h"
#include "SPIRVInternal.h"
#include "libSPIRV/SPIRVDecorate.h"
#include "libSPIRV/SPIRVValue.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <sstream>

#define DEBUG_TYPE "spirv"

namespace SPIRV {

void addFnAttr(LLVMContext *Context, CallInst *Call, Attribute::AttrKind Attr) {
  Call->addAttribute(AttributeList::FunctionIndex, Attr);
}

void removeFnAttr(LLVMContext *Context, CallInst *Call,
                  Attribute::AttrKind Attr) {
  Call->removeAttribute(AttributeList::FunctionIndex, Attr);
}

bool isVoidFuncTy(FunctionType *FT) {
  return FT->getReturnType()->isVoidTy() && FT->getNumParams() == 0;
}

Function *getOrCreateFunction(Module *M, Type *RetTy, ArrayRef<Type *> ArgTypes,
                              StringRef Name, BuiltinFuncMangleInfo *Mangle,
                              AttributeList *Attrs, bool TakeName) {
  std::string MangledName = Name;
  bool IsVarArg = false;
  if (Mangle) {
    MangledName = mangleBuiltin(Name, ArgTypes, Mangle);
    IsVarArg = 0 <= Mangle->getVarArg();
    if (IsVarArg)
      ArgTypes = ArgTypes.slice(0, Mangle->getVarArg());
  }
  FunctionType *FT = FunctionType::get(RetTy, ArgTypes, IsVarArg);
  Function *F = M->getFunction(MangledName);
  if (!TakeName && F && F->getFunctionType() != FT && Mangle != nullptr) {
    std::string S;
    raw_string_ostream SS(S);
    SS << "Error: Attempt to redefine function: " << *F << " => " << *FT
       << '\n';
    report_fatal_error(SS.str(), false);
  }
  if (!F || F->getFunctionType() != FT) {
    auto NewF =
        Function::Create(FT, GlobalValue::ExternalLinkage, MangledName, M);
    if (F && TakeName) {
      NewF->takeName(F);
      LLVM_DEBUG(
          dbgs() << "[getOrCreateFunction] Warning: taking function Name\n");
    }
    if (NewF->getName() != MangledName) {
      LLVM_DEBUG(
          dbgs() << "[getOrCreateFunction] Warning: function Name changed\n");
    }
    LLVM_DEBUG(dbgs() << "[getOrCreateFunction] ";
               if (F) dbgs() << *F << " => "; dbgs() << *NewF << '\n';);
    F = NewF;
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (Attrs)
      F->setAttributes(*Attrs);
  }
  return F;
}

///////////////////////////////////////////////////////////////////////////////
//
// Functions for getting metadata
//
///////////////////////////////////////////////////////////////////////////////
/// Translates LLVM type to descriptor for mangler.
/// \param Signed indicates integer type should be translated as signed.
/// \param VoidPtr indicates i8* should be translated as void*.
static SPIR::RefParamType transTypeDesc(Type *Ty,
                                        const BuiltinArgTypeMangleInfo &Info) {
  bool Signed = Info.IsSigned;
  unsigned Attr = Info.Attr;
  bool VoidPtr = Info.IsVoidPtr;
  if (Info.IsEnum)
    return SPIR::RefParamType(new SPIR::PrimitiveType(Info.Enum));
  if (Info.IsSampler)
    return SPIR::RefParamType(
        new SPIR::PrimitiveType(SPIR::PRIMITIVE_SAMPLER_T));
  if (Info.IsAtomic && !Ty->isPointerTy()) {
    BuiltinArgTypeMangleInfo DTInfo = Info;
    DTInfo.IsAtomic = false;
    return SPIR::RefParamType(new SPIR::AtomicType(transTypeDesc(Ty, DTInfo)));
  }
  if (auto *IntTy = dyn_cast<IntegerType>(Ty)) {
    switch (IntTy->getBitWidth()) {
    case 1:
      return SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_BOOL));
    case 8:
      return SPIR::RefParamType(new SPIR::PrimitiveType(
          Signed ? SPIR::PRIMITIVE_CHAR : SPIR::PRIMITIVE_UCHAR));
    case 16:
      return SPIR::RefParamType(new SPIR::PrimitiveType(
          Signed ? SPIR::PRIMITIVE_SHORT : SPIR::PRIMITIVE_USHORT));
    case 32:
      return SPIR::RefParamType(new SPIR::PrimitiveType(
          Signed ? SPIR::PRIMITIVE_INT : SPIR::PRIMITIVE_UINT));
    case 64:
      return SPIR::RefParamType(new SPIR::PrimitiveType(
          Signed ? SPIR::PRIMITIVE_LONG : SPIR::PRIMITIVE_ULONG));
    default:
      llvm_unreachable("invliad int size");
    }
  }
  if (Ty->isVoidTy())
    return SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_VOID));
  if (Ty->isHalfTy())
    return SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_HALF));
  if (Ty->isFloatTy())
    return SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_FLOAT));
  if (Ty->isDoubleTy())
    return SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_DOUBLE));
  if (Ty->isVectorTy()) {
    return SPIR::RefParamType(
        new SPIR::VectorType(transTypeDesc(Ty->getVectorElementType(), Info),
                             Ty->getVectorNumElements()));
  }
  if (Ty->isArrayTy()) {
    return SPIR::RefParamType(new SPIR::VectorType(
      transTypeDesc(Ty->getArrayElementType(), Info),
      Ty->getArrayNumElements()));
  }
  if (Ty->isStructTy()) {
    auto Name = cast<StructType>(Ty)->isLiteral() ? StringRef("") : Ty->getStructName();
    std::string Tmp;

    if (Name.startswith(kLLVMTypeName::StructPrefix))
      Name = Name.drop_front(strlen(kLLVMTypeName::StructPrefix));
    if (Name.startswith(kSPIRVTypeName::PrefixAndDelim)) {
      Name = Name.substr(sizeof(kSPIRVTypeName::PrefixAndDelim) - 1);
      Tmp = Name.str();
      auto Pos = Tmp.find(kSPIRVTypeName::Delimiter); // first dot
      while (Pos != std::string::npos) {
        Tmp[Pos] = '_';
        Pos = Tmp.find(kSPIRVTypeName::Delimiter, Pos);
      }
      Name = Tmp = kSPIRVName::Prefix + Tmp;
    }
    // ToDo: Create a better unique Name for struct without Name
    if (Name.empty()) {
      std::ostringstream OS;
      OS << reinterpret_cast<size_t>(Ty);
      Name = Tmp = std::string("struct_") + OS.str();
    }
    return SPIR::RefParamType(new SPIR::UserDefinedType(Name));
  }

  if (Ty->isPointerTy()) {
    auto ET = Ty->getPointerElementType();
    SPIR::ParamType *EPT = nullptr;
    if (isa<FunctionType>(ET)) {
      assert(isVoidFuncTy(cast<FunctionType>(ET)) && "Not supported");
      EPT = new SPIR::BlockType;
    } else if (auto StructTy = dyn_cast<StructType>(ET)) {
      LLVM_DEBUG(dbgs() << "ptr to struct: " << *Ty << '\n');
      auto TyName = StructTy->isLiteral() ? StringRef("") : StructTy->getStructName();

      // TODO: Create a better name that is unique for structures without names.
      std::string Tmp;
      if (TyName.empty()) {
        std::ostringstream OS;
        OS << reinterpret_cast<size_t>(Ty);
        TyName = Tmp = std::string("struct_") + OS.str();
      }
      LLVM_DEBUG(dbgs() << "  type Name: " << TyName << '\n');
    }
    if (EPT)
      return SPIR::RefParamType(EPT);

    if (VoidPtr && ET->isIntegerTy(8))
      ET = Type::getVoidTy(ET->getContext());
    auto PT = new SPIR::PointerType(transTypeDesc(ET, Info));
    PT->setAddressSpace(static_cast<SPIR::TypeAttributeEnum>(
        Ty->getPointerAddressSpace() + (unsigned)SPIR::ATTR_ADDR_SPACE_FIRST));
    for (unsigned I = SPIR::ATTR_QUALIFIER_FIRST, E = SPIR::ATTR_QUALIFIER_LAST;
         I <= E; ++I)
      PT->setQualifier(static_cast<SPIR::TypeAttributeEnum>(I), I & Attr);
    return SPIR::RefParamType(PT);
  }
  LLVM_DEBUG(dbgs() << "[transTypeDesc] " << *Ty << '\n');
  assert(0 && "not implemented");
  return SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_INT));
}

void dumpUsers(Value *V, StringRef Prompt) {
  if (!V)
    return;
  LLVM_DEBUG(dbgs() << Prompt << " Users of " << *V << " :\n");
  for (auto UI = V->user_begin(), UE = V->user_end(); UI != UE; ++UI)
    LLVM_DEBUG(dbgs() << "  " << **UI << '\n');
}

bool eraseIfNoUse(Function *F) {
  bool Changed = false;
  if (!F)
    return Changed;
  if (!GlobalValue::isInternalLinkage(F->getLinkage()) && !F->isDeclaration())
    return Changed;

  dumpUsers(F, "[eraseIfNoUse] ");
  for (auto UI = F->user_begin(), UE = F->user_end(); UI != UE;) {
    auto U = *UI++;
    if (auto CE = dyn_cast<ConstantExpr>(U)) {
      if (CE->use_empty()) {
        CE->dropAllReferences();
        Changed = true;
      }
    }
  }
  if (F->use_empty()) {
    LLVM_DEBUG(dbgs() << "Erase "; F->printAsOperand(dbgs()); dbgs() << '\n');
    F->eraseFromParent();
    Changed = true;
  }
  return Changed;
}

void eraseIfNoUse(Value *V) {
  if (!V->use_empty())
    return;
  if (Constant *C = dyn_cast<Constant>(V)) {
    C->destroyConstant();
    return;
  }
  if (Instruction *I = dyn_cast<Instruction>(V)) {
    if (!I->mayHaveSideEffects())
      I->eraseFromParent();
  }
  eraseIfNoUse(dyn_cast<Function>(V));
}

bool eraseUselessFunctions(Module *M) {
  bool Changed = false;
  for (auto I = M->begin(), E = M->end(); I != E;)
    Changed |= eraseIfNoUse(&(*I++));
  return Changed;
}

std::string mangleBuiltin(const std::string &UniqName,
                          ArrayRef<Type *> ArgTypes,
                          BuiltinFuncMangleInfo *BtnInfo) {
  if (!BtnInfo)
    return UniqName;
  BtnInfo->init(UniqName);
  std::string MangledName;
  LLVM_DEBUG(dbgs() << "[mangle] " << UniqName << " => ");
  SPIR::FunctionDescriptor FD;
  FD.Name = BtnInfo->getUnmangledName();
  bool BIVarArgNegative = BtnInfo->getVarArg() < 0;

  if (ArgTypes.empty()) {
    // Function signature cannot be ()(void, ...) so if there is an ellipsis
    // it must be ()(...)
    if (BIVarArgNegative) {
      FD.Parameters.emplace_back(
          SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_VOID)));
    }
  } else {
    for (unsigned I = 0, E = BIVarArgNegative ? ArgTypes.size()
                                              : (unsigned)BtnInfo->getVarArg();
         I != E; ++I) {
      auto T = ArgTypes[I];
      FD.Parameters.emplace_back(
          transTypeDesc(T, BtnInfo->getTypeMangleInfo(I)));
    }
  }
  // Ellipsis must be the last argument of any function
  if (!BIVarArgNegative) {
    assert((unsigned)BtnInfo->getVarArg() <= ArgTypes.size() &&
           "invalid index of an ellipsis");
    FD.Parameters.emplace_back(
        SPIR::RefParamType(new SPIR::PrimitiveType(SPIR::PRIMITIVE_VAR_ARG)));
  }

  SPIR::NameMangler Mangler(SPIR::SPIR20);
  Mangler.mangle(FD, MangledName);
  LLVM_DEBUG(dbgs() << MangledName << '\n');
  return MangledName;
}

} // namespace SPIRV
