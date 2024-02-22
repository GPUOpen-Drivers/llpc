//===- SPIRVtype.cpp - Class to represent a SPIR-V type ----------*- C++ -*-===//
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
/// This file implements the types defined in SPIRV spec with op codes.
///
//===----------------------------------------------------------------------===//

#include "SPIRVType.h"
#include "SPIRVDecorate.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "SPIRVModule.h"
#include "SPIRVValue.h"
#include <cassert>

namespace SPIRV {

SPIRVType *SPIRVType::getArrayElementType() const {
  switch (OpCode) {
  case OpTypeArray:
    return static_cast<const SPIRVTypeArray *const>(this)->getElementType();
  case OpTypeRuntimeArray:
    return static_cast<const SPIRVTypeRuntimeArray *const>(this)->getElementType();
  default:
    assert(!"Not array type");
  };
  return nullptr;
}

uint64_t SPIRVType::getArrayLength() const {
  assert(OpCode == OpTypeArray && "Not array type");
  return static_cast<const SPIRVTypeArray *const>(this)->getLength()->getZExtIntValue();
}

SPIRVWord SPIRVType::getBitWidth() const {
  if (isTypeVector())
    return getVectorComponentType()->getBitWidth();
  if (isTypeMatrix())
    return getMatrixColumnType()->getBitWidth();
  if (isTypeBool())
    return 32;
  return isTypeInt() ? getIntegerBitWidth() : getFloatBitWidth();
}

SPIRVWord SPIRVType::getFloatBitWidth() const {
  assert(OpCode == OpTypeFloat && "Not a float type");
  return static_cast<const SPIRVTypeFloat *const>(this)->getBitWidth();
}

SPIRVWord SPIRVType::getIntegerBitWidth() const {
  assert((OpCode == OpTypeInt || OpCode == OpTypeBool) && "Not an integer type");
  if (isTypeBool())
    return 32;
  return static_cast<const SPIRVTypeInt *const>(this)->getBitWidth();
}

unsigned SPIRVType::getSizeInBytes() const {
  // NOTE: Here, we assume data are tightly packed and alignments are not taken into
  // consideration.
  if (isTypeVector())
    return getVectorComponentType()->getSizeInBytes() * getVectorComponentCount();
  if (isTypeMatrix())
    return getMatrixColumnType()->getSizeInBytes() * getMatrixColumnCount();
  if (isTypeArray())
    return getArrayElementType()->getSizeInBytes() * getArrayLength();
  if (isTypeStruct()) {
    unsigned sizeInBytes = 0;
    for (unsigned member = 0; member < getStructMemberCount(); ++member)
      sizeInBytes += getStructMemberType(member)->getSizeInBytes();
    return sizeInBytes;
  }

  switch (OpCode) {
  case OpTypeBool:
  case OpTypeInt:
  case OpTypeFloat:
    return (getBitWidth() + 7) / 8;
  default:
    llvm_unreachable("Cannot get the size of this type!");
  }

  return 0;
}

SPIRVType *SPIRVType::getFunctionReturnType() const {
  assert(OpCode == OpTypeFunction);
  return static_cast<const SPIRVTypeFunction *const>(this)->getReturnType();
}

SPIRVType *SPIRVType::getPointerElementType() const {
  switch (OpCode) {
  case OpTypePointer:
    return static_cast<const SPIRVTypePointer *const>(this)->getElementType();
  case OpTypeForwardPointer:
    return static_cast<const SPIRVTypeForwardPointer *const>(this)->getPointer()->getElementType();
  default:
    llvm_unreachable("Not a pointer type");
  }
}

SPIRVStorageClassKind SPIRVType::getPointerStorageClass() const {
  switch (OpCode) {
  case OpTypePointer:
    return static_cast<const SPIRVTypePointer *const>(this)->getStorageClass();
  case OpTypeForwardPointer:
    return static_cast<const SPIRVTypeForwardPointer *const>(this)->getPointer()->getPointerStorageClass();
  default:
    llvm_unreachable("Not a pointer type");
  }
}

SPIRVType *SPIRVType::getStructMemberType(size_t Index) const {
  assert(OpCode == OpTypeStruct && "Not struct type");
  return static_cast<const SPIRVTypeStruct *const>(this)->getMemberType(Index);
}

SPIRVWord SPIRVType::getStructMemberCount() const {
  assert(OpCode == OpTypeStruct && "Not struct type");
  return static_cast<const SPIRVTypeStruct *const>(this)->getMemberCount();
}

SPIRVWord SPIRVType::getVectorComponentCount() const {
  assert(OpCode == OpTypeVector && "Not vector type");
  return static_cast<const SPIRVTypeVector *const>(this)->getComponentCount();
}

SPIRVType *SPIRVType::getVectorComponentType() const {
  assert(OpCode == OpTypeVector && "Not vector type");
  return static_cast<const SPIRVTypeVector *const>(this)->getComponentType();
}

SPIRVWord SPIRVType::getMatrixColumnCount() const {
  assert(OpCode == OpTypeMatrix && "Not matrix type");
  return static_cast<const SPIRVTypeMatrix *const>(this)->getColumnCount();
}

SPIRVType *SPIRVType::getMatrixColumnType() const {
  assert(OpCode == OpTypeMatrix && "Not matrix type");
  return static_cast<const SPIRVTypeMatrix *const>(this)->getColumnType();
}

SPIRVType *SPIRVType::getCompositeElementType(size_t Index) const {
  if (OpCode == OpTypeStruct)
    return getStructMemberType(Index);
  if (OpCode == OpTypeArray)
    return getArrayElementType();
  if (OpCode == OpTypeMatrix)
    return getMatrixColumnType();
  if (OpCode == OpTypeVector)
    return getVectorComponentType();
  llvm_unreachable("Not composite type");
  return nullptr;
}

SPIRVWord SPIRVType::getCompositeElementCount() const {
  if (OpCode == OpTypeStruct)
    return getStructMemberCount();
  if (OpCode == OpTypeArray)
    return getArrayLength();
  if (OpCode == OpTypeMatrix)
    return getMatrixColumnCount();
  if (OpCode == OpTypeVector)
    return getVectorComponentCount();
  llvm_unreachable("Not composite type");
  return 1;
}

SPIRVType *SPIRVType::getCooperativeMatrixKHRComponentType() const {
  assert(OpCode == OpTypeCooperativeMatrixKHR && "Not cooperative matrix type");
  return static_cast<const SPIRVTypeCooperativeMatrixKHR *const>(this)->getComponentType();
}

uint32_t SPIRVType::getCooperativeMatrixKHRScope() const {
  assert(OpCode == OpTypeCooperativeMatrixKHR && "Not cooperative matrix type");
  return static_cast<const SPIRVTypeCooperativeMatrixKHR *const>(this)->getScope()->getZExtIntValue();
}

uint32_t SPIRVType::getCooperativeMatrixKHRRows() const {
  assert(OpCode == OpTypeCooperativeMatrixKHR && "Not cooperative matrix type");
  return static_cast<const SPIRVTypeCooperativeMatrixKHR *const>(this)->getRows()->getZExtIntValue();
}

uint32_t SPIRVType::getCooperativeMatrixKHRColumns() const {
  assert(OpCode == OpTypeCooperativeMatrixKHR && "Not cooperative matrix type");
  return static_cast<const SPIRVTypeCooperativeMatrixKHR *const>(this)->getColumns()->getZExtIntValue();
}

uint32_t SPIRVType::getCooperativeMatrixKHRUse() const {
  assert(OpCode == OpTypeCooperativeMatrixKHR && "Not cooperative matrix type");
  return static_cast<const SPIRVTypeCooperativeMatrixKHR *const>(this)->getUse()->getZExtIntValue();
}

bool SPIRVType::isTypeVoid() const {
  return OpCode == OpTypeVoid;
}

bool SPIRVType::isTypeArray() const {
  return OpCode == OpTypeArray || OpCode == OpTypeRuntimeArray;
}

bool SPIRVType::isTypeRuntimeArray() const {
  return OpCode == OpTypeRuntimeArray;
}

bool SPIRVType::isTypeBool() const {
  return OpCode == OpTypeBool;
}

bool SPIRVType::isTypeComposite() const {
  return isTypeVector() || isTypeMatrix() || isTypeArray() || isTypeStruct();
}

bool SPIRVType::isTypeFloat(unsigned Bits) const {
  return isType<SPIRVTypeFloat>(this, Bits);
}

bool SPIRVType::isTypeInt(unsigned Bits) const {
  return isType<SPIRVTypeInt>(this, Bits);
}

bool SPIRVType::isTypePointer() const {
  return OpCode == OpTypePointer;
}

bool SPIRVType::isTypeForwardPointer() const {
  return OpCode == OpTypeForwardPointer;
}

bool SPIRVType::isTypeSampler() const {
  return OpCode == OpTypeSampler;
}

bool SPIRVType::isTypeImage() const {
  return OpCode == OpTypeImage;
}

bool SPIRVType::isTypeSampledImage() const {
  return OpCode == OpTypeSampledImage;
}

bool SPIRVType::isTypeStruct() const {
  return OpCode == OpTypeStruct;
}

bool SPIRVType::isTypeScalar() const {
  return isTypeBool() || isTypeInt() || isTypeFloat();
}

bool SPIRVType::isTypeVector() const {
  return OpCode == OpTypeVector;
}

bool SPIRVType::isTypeMatrix() const {
  return OpCode == OpTypeMatrix;
}

bool SPIRVType::isTypeAccelerationStructureKHR() const {
  return OpCode == OpTypeAccelerationStructureKHR;
}

bool SPIRVType::isTypeRayQueryKHR() const {
  return OpCode == OpTypeRayQueryKHR;
}

bool SPIRVType::isTypeCooperativeMatrixKHR() const {
  return OpCode == OpTypeCooperativeMatrixKHR;
}

bool SPIRVType::isTypeVectorBool() const {
  return isTypeVector() && getVectorComponentType()->isTypeBool();
}

bool SPIRVType::isTypeVectorInt(unsigned Bits) const {
  return isTypeVector() && getVectorComponentType()->isTypeInt(Bits);
}

bool SPIRVType::isTypeVectorFloat(unsigned Bits) const {
  return isTypeVector() && getVectorComponentType()->isTypeFloat(Bits);
}

bool SPIRVType::isTypeVectorOrScalarBool() const {
  return isTypeBool() || isTypeVectorBool();
}

bool SPIRVType::isTypeVectorOrScalarInt(unsigned Bits) const {
  return isTypeInt(Bits) || isTypeVectorInt(Bits);
}

bool SPIRVType::isTypeVectorOrScalarFloat(unsigned Bits) const {
  return isTypeFloat(Bits) || isTypeVectorFloat(Bits);
}

SPIRVTypeArray::SPIRVTypeArray(SPIRVModule *M, SPIRVId TheId, SPIRVType *TheElemType, SPIRVConstant *TheLength)
    : SPIRVType(M, 4, OpTypeArray, TheId), ElemType(TheElemType), Length(TheLength->getId()) {
  validate();
}

void SPIRVTypeArray::validate() const {
  SPIRVEntry::validate();
  ElemType->validate();
  assert(getValue(Length)->getType()->isTypeInt() && getLength()->getZExtIntValue() > 0);
}

SPIRVConstant *SPIRVTypeArray::getLength() const {
  auto BV = getValue(Length);
  if (BV->getOpCode() == OpSpecConstantOp) {
    // NOTE: If the "length" is not a normal constant and is defined through
    // "OpSpecConstantOp", we have to get its literal value from the mapped
    // constant.
    auto MappedConst = static_cast<SPIRVSpecConstantOp *>(BV)->getMappedConstant();
    return static_cast<SPIRVConstant *>(MappedConst);
  }
  return get<SPIRVConstant>(Length);
}

_SPIRV_IMP_DECODE3(SPIRVTypeArray, Id, ElemType, Length)

SPIRVTypeRuntimeArray::SPIRVTypeRuntimeArray(SPIRVModule *M, SPIRVId TheId, SPIRVType *TheElemType)
    : SPIRVType(M, 3, OpTypeRuntimeArray, TheId), ElemType(TheElemType) {
  validate();
}

void SPIRVTypeRuntimeArray::validate() const {
  SPIRVEntry::validate();
  ElemType->validate();
}

_SPIRV_IMP_ENCDEC2(SPIRVTypeRuntimeArray, Id, ElemType)

void SPIRVTypeForwardPointer::decode(std::istream &I) {
  auto Decoder = getDecoder(I);
  Decoder >> Id >> SC;
}

void SPIRVTypeCooperativeMatrixKHR::validate() const {
  SPIRVEntry::validate();
  CompType->validate();
  assert(CompType->isTypeInt() || CompType->isTypeFloat());
  assert(isa<OpConstant>(getValue(Rows)) || isa<OpSpecConstant>(getValue(Rows)));
  assert(getValue(Rows)->getType()->isTypeInt());
  assert(isa<OpConstant>(getValue(Columns)) || isa<OpSpecConstant>(getValue(Columns)));
  assert(getValue(Columns)->getType()->isTypeInt());
  assert(isa<OpConstant>(getValue(Use)) || isa<OpSpecConstant>(getValue(Use)));
  assert(getValue(Use)->getType()->isTypeInt());
  // CompIntp is still under dicussion:
  // assert(isa<OpConstant>(getValue(CompIntp)) || isa<OpSpecConstant>(getValue(CompIntp)));
  // assert(getValue(CompIntp)->getType()->isTypeInt());
}

SPIRVConstant *SPIRVTypeCooperativeMatrixKHR::getScope() const {
  return get<SPIRVConstant>(Scope);
}

SPIRVConstant *SPIRVTypeCooperativeMatrixKHR::getRows() const {
  return get<SPIRVConstant>(Rows);
}

SPIRVConstant *SPIRVTypeCooperativeMatrixKHR::getColumns() const {
  return get<SPIRVConstant>(Columns);
}

SPIRVConstant *SPIRVTypeCooperativeMatrixKHR::getUse() const {
  return get<SPIRVConstant>(Use);
}

SPIRVConstant *SPIRVTypeCooperativeMatrixKHR::getComIntp() const {
  return get<SPIRVConstant>(CompIntp);
}

_SPIRV_IMP_DECODE6(SPIRVTypeCooperativeMatrixKHR, Id, CompType, Scope, Rows, Columns, Use)

} // namespace SPIRV
