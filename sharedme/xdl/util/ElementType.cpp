/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ElementType.cpp
 * @brief contains the definition of utility functions of Xdl element types
 ***********************************************************************************************************************
 */

#include "xdl/util/ElementType.h"
#include "lgc/LgcXdlTypes.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ErrorHandling.h"

using namespace lgc::xdl;

namespace {
struct CastOpMapKeyT {
  CooperativeMatrixElementType srcElemType;
  bool srcIsSigned;
  CooperativeMatrixElementType dstElemType;
  bool dstIsSigned;
};
} // anonymous namespace

template <> struct llvm::DenseMapInfo<CastOpMapKeyT> {
  using T = CastOpMapKeyT;

  static T getEmptyKey() {
    return T{CooperativeMatrixElementType::Unknown, false, CooperativeMatrixElementType::Unknown, false};
  }
  static T getTombstoneKey() {
    return T{static_cast<CooperativeMatrixElementType>(DenseMapInfo<unsigned>::getTombstoneKey()), false,
             static_cast<CooperativeMatrixElementType>(DenseMapInfo<unsigned>::getTombstoneKey()), false};
  }
  static unsigned getHashValue(const T &val) {
    return llvm::hash_combine(
        DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(val.srcElemType)), val.srcIsSigned,
        DenseMapInfo<unsigned>::getHashValue(static_cast<unsigned>(val.dstElemType)), val.dstIsSigned);
  }
  static bool isEqual(const T &lhs, const T &rhs) {
    return lhs.srcElemType == rhs.srcElemType && lhs.srcIsSigned == rhs.srcIsSigned &&
           lhs.dstElemType == rhs.dstElemType && lhs.dstIsSigned == rhs.dstIsSigned;
  }
};

// =====================================================================================================================
// Get the bit width of the cooperativeMatrix element type
//
// @param elemType : The matrix element type
static unsigned getBitWidthOfCooperativeMatrixElement(CooperativeMatrixElementType elemType) {
  switch (elemType) {
  case CooperativeMatrixElementType::Float16:
  case CooperativeMatrixElementType::Float16Packed:
  case CooperativeMatrixElementType::BFloat16:
  case CooperativeMatrixElementType::Int16:
    return 16;
  case CooperativeMatrixElementType::Float32:
  case CooperativeMatrixElementType::Int32:
    return 32;
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Float8:
  case CooperativeMatrixElementType::BFloat8:
    return 8;
  case CooperativeMatrixElementType::Int4:
    return 4;
  default:
    llvm_unreachable("Type is not supported!");
  }
}

// =====================================================================================================================
// Get the cast opcode for cooperative matrix.
//
// @param srcElemType : the element type of source matrix
// @param dstElemType : the element type of target matrix
// @return the cast op
llvm::Instruction::CastOps lgc::xdl::getCooperativeMatrixCastOp(CooperativeMatrixElementType srcElemType,
                                                                bool srcIsSigned,
                                                                CooperativeMatrixElementType dstElemType,
                                                                bool dstIsSigned) {
  using ElemTy = CooperativeMatrixElementType;
  // NOTE: For floating points, we have some rules:
  //  + float8 / bfloat8 will be changed to float32 first, and then cast to target type.
  //  + to cast between float16 and bfloat16, we need to use `FPTrunc`, since we will cast it to float32 first.
  //  + to cast between same 16-bit floating type, we use `0` instead of `BitCast`, that means reshape only.
  static const llvm::DenseMap<CastOpMapKeyT, llvm::Instruction::CastOps> castOpMaps{
      {{ElemTy::Int4, true, ElemTy::Int4, true}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int4, true, ElemTy::Int4, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int4, true, ElemTy::Int8, true}, llvm::Instruction::CastOps::SExt},
      {{ElemTy::Int4, true, ElemTy::Int8, false}, llvm::Instruction::CastOps::SExt},
      {{ElemTy::Int4, true, ElemTy::Float16, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int4, true, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int4, true, ElemTy::Float32, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int4, true, ElemTy::Int32, true}, llvm::Instruction::CastOps::SExt},
      {{ElemTy::Int4, true, ElemTy::Int32, false}, llvm::Instruction::CastOps::SExt},
      {{ElemTy::Int4, true, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int4, true, ElemTy::Float8, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int4, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int4, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int4, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::ZExt},
      {{ElemTy::Int4, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::ZExt},
      {{ElemTy::Int4, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int4, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int4, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int4, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::ZExt},
      {{ElemTy::Int4, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::ZExt},
      {{ElemTy::Int4, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int4, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int8, true, ElemTy::Int4, true}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int8, true, ElemTy::Int4, false}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int8, true, ElemTy::Int8, true}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int8, true, ElemTy::Int8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int8, true, ElemTy::Float16, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int8, true, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int8, true, ElemTy::Float32, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int8, true, ElemTy::Int32, true}, llvm::Instruction::CastOps::SExt},
      {{ElemTy::Int8, true, ElemTy::Int32, false}, llvm::Instruction::CastOps::SExt},
      {{ElemTy::Int8, true, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int8, true, ElemTy::Float8, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int8, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int8, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int8, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int8, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int8, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int8, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int8, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int8, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::ZExt},
      {{ElemTy::Int8, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::ZExt},
      {{ElemTy::Int8, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int8, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Float16, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float16, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float16, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float16, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float16, false, ElemTy::Float16, false}, static_cast<llvm::Instruction::CastOps>(0)},
      {{ElemTy::Float16, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::Float16, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::FPExt},
      {{ElemTy::Float16, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float16, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float16, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::FPExt},
      {{ElemTy::Float16, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::FPExt},
      {{ElemTy::BFloat16, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::BFloat16, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::BFloat16, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::BFloat16, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::BFloat16, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::BFloat16, false, ElemTy::BFloat16, false}, static_cast<llvm::Instruction::CastOps>(0)},
      {{ElemTy::BFloat16, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::FPExt},
      {{ElemTy::BFloat16, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::BFloat16, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::BFloat16, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::FPExt},
      {{ElemTy::BFloat16, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::FPExt},
      {{ElemTy::Float32, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float32, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float32, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float32, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float32, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::Float32, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::Float32, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Float32, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float32, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float32, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Float32, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int32, true, ElemTy::Int4, true}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, true, ElemTy::Int4, false}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, true, ElemTy::Int8, true}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, true, ElemTy::Int8, false}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, true, ElemTy::Float16, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int32, true, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int32, true, ElemTy::Float32, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int32, true, ElemTy::Int32, true}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int32, true, ElemTy::Int32, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int32, true, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int32, true, ElemTy::Float8, false}, llvm::Instruction::CastOps::SIToFP},
      {{ElemTy::Int32, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::Trunc},
      {{ElemTy::Int32, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int32, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int32, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int32, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int32, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Int32, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::Int32, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::UIToFP},
      {{ElemTy::BFloat8, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::BFloat8, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::BFloat8, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::BFloat8, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::BFloat8, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::BFloat8, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::BFloat8, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::BFloat8, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::BFloat8, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::BFloat8, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::BFloat8, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Float8, false, ElemTy::Int4, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float8, false, ElemTy::Int4, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float8, false, ElemTy::Int8, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float8, false, ElemTy::Int8, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float8, false, ElemTy::Float16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::Float8, false, ElemTy::BFloat16, false}, llvm::Instruction::CastOps::FPTrunc},
      {{ElemTy::Float8, false, ElemTy::Float32, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Float8, false, ElemTy::Int32, true}, llvm::Instruction::CastOps::FPToSI},
      {{ElemTy::Float8, false, ElemTy::Int32, false}, llvm::Instruction::CastOps::FPToUI},
      {{ElemTy::Float8, false, ElemTy::BFloat8, false}, llvm::Instruction::CastOps::BitCast},
      {{ElemTy::Float8, false, ElemTy::Float8, false}, llvm::Instruction::CastOps::BitCast},
  };
  auto key = CastOpMapKeyT{
      .srcElemType = srcElemType,
      .srcIsSigned = srcIsSigned,
      .dstElemType = dstElemType,
      .dstIsSigned = dstIsSigned,
  };
  assert(castOpMaps.contains(key) && "Not found the related cast op.");
  return castOpMaps.at(key);
}

// =====================================================================================================================
// Get the LGC type of a cooperative matrix with the given element type and layout.
//
// @param builder : The IR builder
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @param kSize : the matrix K size
llvm::Type *lgc::xdl::getCooperativeMatrixTy(llvm_dialects::Builder &builder, CooperativeMatrixElementType elemType,
                                             CooperativeMatrixLayout layout, unsigned kSize) {
  // Note: the layout currently has no influence on the type. In the long run, we should switch to genuinely opaque
  // types at the LGC level, and parameterize the type using both the element type and the layout.

  llvm::Type *wordTy = isUnderlyingIntegerCooperativeMatrix(elemType) ? builder.getInt32Ty() : builder.getFloatTy();
  [[maybe_unused]] unsigned cntDwords = 0;
  switch (layout) {
  case CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout:
  case CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout:
  case CooperativeMatrixLayout::AccumulatorMatrixLayout:
    return llvm::FixedVectorType::get(wordTy, 8);
  case CooperativeMatrixLayout::FactorMatrixLayout:
    if (elemType == CooperativeMatrixElementType::Int4)
      return llvm::FixedVectorType::get(wordTy, 2);
    if (elemType == CooperativeMatrixElementType::Int8)
      return llvm::FixedVectorType::get(wordTy, 4);
    return llvm::FixedVectorType::get(wordTy, 8);
  case CooperativeMatrixLayout::Gfx12BaseLayout:
    assert(kSize == 16);
    // Total elementNumber * element_bit_width/ (waveSize * vgpr_size_perlane);
    // Use wave32 as default, wave64 will have some poison values in later process.
    cntDwords = (16 * 16 * getBitWidthOfCooperativeMatrixElement(elemType)) / (32 * 32);
    if (cntDwords > 1)
      return llvm::FixedVectorType::get(wordTy, cntDwords);
    return builder.getInt32Ty();
  case CooperativeMatrixLayout::Gfx12SwizzledKX16Layout:
    assert(kSize >= 32);
    cntDwords = (kSize * 16 * getBitWidthOfCooperativeMatrixElement(elemType)) / (32 * 32);
    if (cntDwords > 1)
      return llvm::FixedVectorType::get(wordTy, cntDwords);
    return builder.getInt32Ty();
  default:
    llvm_unreachable("Type is not supported!");
  }
}

// =====================================================================================================================
// Get the LLVM type of a sparse index for the sparseCooperativeMatrix.
//
// @param format : The sparse index for the sparseCooperativeMatrix
llvm::Type *lgc::xdl::getSparseIndexTy(llvm_dialects::Builder &builder, SparseCooperativeMatrixSparsityFormat format) {
  // Note: the layout currently has no influence on the type. In the long run, we should switch to genuinely opaque
  // types at the LGC level, and parameterize the type using both the element type and the layout.
  switch (format) {
  case SparseCooperativeMatrixSparsityFormat::Sparsity2to4AMD:
    return builder.getInt32Ty();
  default:
    llvm_unreachable("The sparsity index type is not supported now.");
  }
}

// =====================================================================================================================
// Whether the underlying type of a cooperative matrix is integer.
//
// @param elemType : The matrix element type
bool lgc::xdl::isUnderlyingIntegerCooperativeMatrix(CooperativeMatrixElementType elemType) {
  switch (elemType) {
  case CooperativeMatrixElementType::Float16:
  case CooperativeMatrixElementType::Float32:
  case CooperativeMatrixElementType::Float16Packed:
    return false;
  case CooperativeMatrixElementType::BFloat16:
  case CooperativeMatrixElementType::Float8:
  case CooperativeMatrixElementType::BFloat8:
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Int16:
  case CooperativeMatrixElementType::Int32:
  case CooperativeMatrixElementType::Int4:
    return true;
  default:
    llvm_unreachable("Type is not supported!");
  }
}

// =====================================================================================================================
// Interpret the cooperative matrix's element as uint32.
//
// @param builder : The IR builder
// @param value : Input data as element type
// @param elemType : The source element type enum value
// @returns The data of uint32
llvm::Value *lgc::xdl::interpretCoopMatElementAsIntegerTy(llvm_dialects::Builder &builder, llvm::Value *value,
                                                          CooperativeMatrixElementType elemType) {
  [[maybe_unused]] llvm::Type *llElemType = transCooperativeMatrixElementType(builder, elemType);
  llvm::Type *targetType = builder.getInt32Ty();
  switch (elemType) {
  case CooperativeMatrixElementType::Float16:
  case CooperativeMatrixElementType::Float16Packed:
    return builder.CreateZExt(builder.CreateBitCast(value, builder.getInt16Ty()), targetType);
  case CooperativeMatrixElementType::BFloat16:
    assert(llElemType->isIntegerTy(16));
    return builder.CreateZExt(value, targetType);
  case CooperativeMatrixElementType::Float32:
    return builder.CreateBitCast(value, targetType);
  case CooperativeMatrixElementType::BFloat8:
  case CooperativeMatrixElementType::Float8:
    assert(llElemType->isIntegerTy(8));
    return builder.CreateZExt(value, targetType);
  case CooperativeMatrixElementType::Int16:
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Int4:
    return builder.CreateZExt(value, targetType);
  case CooperativeMatrixElementType::Int32:
    return value;
  default:
    llvm_unreachable("Unknown cooperative matrix element type.");
  }
  return nullptr;
}

// =====================================================================================================================
// Interpret the value as cooperative matrix's element type.
//
// @param builder : The IR builder
// @param value : Input data as uint32
// @param elemType : The target element type enum value
// @returns The data of correct element type
llvm::Value *lgc::xdl::interpretValueAsCoopMatElementTy(llvm_dialects::Builder &builder, llvm::Value *value,
                                                        CooperativeMatrixElementType elemType) {
  assert(value->getType()->isIntegerTy(32));
  llvm::Type *type = transCooperativeMatrixElementType(builder, elemType);
  switch (elemType) {
  case CooperativeMatrixElementType::BFloat8:
  case CooperativeMatrixElementType::Float8:
    assert(type->isIntegerTy(8));
    return builder.CreateTrunc(value, type);
  case CooperativeMatrixElementType::Float16:
  case CooperativeMatrixElementType::Float16Packed:
    return builder.CreateBitCast(builder.CreateTrunc(value, builder.getInt16Ty()), type);
  case CooperativeMatrixElementType::BFloat16:
    assert(type->isIntegerTy(16));
    return builder.CreateTrunc(value, type);
  case CooperativeMatrixElementType::Float32:
    return builder.CreateBitCast(value, builder.getFloatTy());
  case CooperativeMatrixElementType::Int16:
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Int4:
    return builder.CreateTrunc(value, type);
  case CooperativeMatrixElementType::Int32:
    return value;
  default:
    llvm_unreachable("Unknown cooperative matrix element type.");
  }
  return nullptr;
}

// =====================================================================================================================
// Whether the type of a cooperative matrix is specified bit width.
//
// @param elemType : the matrix element type
// @param bitWidth : the specified bit width
bool lgc::xdl::isTypeNCooperativeMatrix(CooperativeMatrixElementType elemType, unsigned bitWidth) {
  unsigned width = getBitWidthOfCooperativeMatrixElement(elemType);
  return width == bitWidth;
}

// =====================================================================================================================
// Convert the element type enum into the corresponding LLVM type.
//
// @param builder : The IR builder
// @param elemType : The element type enum value
// @returns the corresponding LLVM type
llvm::Type *lgc::xdl::transCooperativeMatrixElementType(llvm_dialects::Builder &builder,
                                                        CooperativeMatrixElementType elemType) {
  llvm::Type *type = nullptr;
  switch (elemType) {
  case CooperativeMatrixElementType::Float16:
  case CooperativeMatrixElementType::Float16Packed:
    type = builder.getHalfTy();
    break;
  case CooperativeMatrixElementType::Float32:
    type = builder.getFloatTy();
    break;
  case CooperativeMatrixElementType::Int16:
  case CooperativeMatrixElementType::BFloat16:
    type = builder.getInt16Ty();
    break;
  case CooperativeMatrixElementType::Int32:
    type = builder.getInt32Ty();
    break;
  case CooperativeMatrixElementType::Int8:
  case CooperativeMatrixElementType::Float8:
  case CooperativeMatrixElementType::BFloat8:
    type = builder.getInt8Ty();
    break;
  case CooperativeMatrixElementType::Int4:
    type = builder.getIntNTy(4);
    break;
  default:
    llvm_unreachable("The element type is not supported.");
  }
  assert(type->isIntegerTy() == isUnderlyingIntegerCooperativeMatrix(elemType));
  return type;
}
