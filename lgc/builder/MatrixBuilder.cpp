/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  MatrixBuilder.cpp
 * @brief LLPC source file: implementation of matrix Builder methods
 ***********************************************************************************************************************
 */
#include "lgc/builder/BuilderImpl.h"

#define DEBUG_TYPE "lgc-builder-impl-matrix"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a matrix transpose.
//
// @param matrix : Matrix to transpose.
// @param instName : Name to give final instruction
Value *BuilderImpl::CreateTransposeMatrix(Value *const matrix, const Twine &instName) {
  assert(matrix);

  Type *const matrixType = matrix->getType();
  assert(matrixType->isArrayTy());

  Type *const columnVectorType = matrixType->getArrayElementType();
  assert(columnVectorType->isVectorTy());

  const unsigned columnCount = matrixType->getArrayNumElements();
  const unsigned rowCount = cast<FixedVectorType>(columnVectorType)->getNumElements();

  Type *const elementType = cast<VectorType>(columnVectorType)->getElementType();

  Type *const newColumnVectorType = FixedVectorType::get(elementType, columnCount);
  Type *const newMatrixType = ArrayType::get(newColumnVectorType, rowCount);

  SmallVector<Value *, 4> columns;

  for (unsigned column = 0; column < columnCount; column++)
    columns.push_back(CreateExtractValue(matrix, column));

  SmallVector<Value *, 4> newColumns;

  for (unsigned row = 0; row < rowCount; row++)
    newColumns.push_back(PoisonValue::get(newColumnVectorType));

  for (unsigned column = 0; column < columnCount; column++) {
    for (unsigned row = 0; row < rowCount; row++) {
      Value *const element = CreateExtractElement(columns[column], row);
      newColumns[row] = CreateInsertElement(newColumns[row], element, column);
    }
  }

  Value *newMatrix = PoisonValue::get(newMatrixType);

  for (unsigned row = 0; row < rowCount; row++)
    newMatrix = CreateInsertValue(newMatrix, newColumns[row], row);

  newMatrix->setName(instName);
  return newMatrix;
}

// =====================================================================================================================
// Create matrix from matrix Times scalar
//
// @param matrix : The column major matrix, n x <n x float>
// @param scalar : The float scalar
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateMatrixTimesScalar(Value *const matrix, Value *const scalar, const Twine &instName) {
  Type *const matrixTy = matrix->getType();
  Type *const columnTy = matrixTy->getArrayElementType();
  const unsigned rowCount = cast<FixedVectorType>(columnTy)->getNumElements();
  unsigned columnCount = matrixTy->getArrayNumElements();
  auto smearScalar = CreateVectorSplat(rowCount, scalar);

  Value *result = PoisonValue::get(matrixTy);
  for (unsigned column = 0; column < columnCount; column++) {
    auto columnVector = CreateExtractValue(matrix, column);
    columnVector = CreateFMul(columnVector, smearScalar);
    result = CreateInsertValue(result, columnVector, column);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create vector from vector Times matrix
//
// @param vector : The float vector
// @param matrix : The column major matrix, n x <n x float>
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateVectorTimesMatrix(Value *const vector, Value *const matrix, const Twine &instName) {
  Type *const matrixTy = matrix->getType();
  Type *const compTy = cast<VectorType>(cast<ArrayType>(matrixTy)->getElementType())->getElementType();
  const unsigned columnCount = matrixTy->getArrayNumElements();
  Type *const resultTy = FixedVectorType::get(compTy, columnCount);
  Value *result = PoisonValue::get(resultTy);

  for (unsigned column = 0; column < columnCount; column++) {
    auto columnVector = CreateExtractValue(matrix, column);
    result = CreateInsertElement(result, CreateDotProduct(columnVector, vector), column);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create vector from matrix times vector
//
// @param matrix : The column major matrix, n x <n x float>
// @param vector : The vector
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateMatrixTimesVector(Value *const matrix, Value *const vector, const Twine &instName) {
  Type *const columnTy = matrix->getType()->getArrayElementType();
  const unsigned rowCount = cast<FixedVectorType>(columnTy)->getNumElements();
  Value *result = nullptr;

  for (unsigned i = 0; i < matrix->getType()->getArrayNumElements(); ++i) {
    SmallVector<int, 4> shuffleMask(rowCount, i);
    auto partialResult = CreateShuffleVector(vector, vector, shuffleMask);
    partialResult = CreateFMul(CreateExtractValue(matrix, i), partialResult);
    if (result)
      result = CreateFAdd(result, partialResult);
    else
      result = partialResult;
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create matrix from matrix times matrix
//
// @param matrix1 : The float matrix 1
// @param matrix2 : The float matrix 2
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateMatrixTimesMatrix(Value *const matrix1, Value *const matrix2, const Twine &instName) {
  Type *const mat1ColumnType = matrix1->getType()->getArrayElementType();
  const unsigned mat2ColCount = matrix2->getType()->getArrayNumElements();
  Type *const resultTy = ArrayType::get(mat1ColumnType, mat2ColCount);
  Value *result = PoisonValue::get(resultTy);

  for (unsigned i = 0; i < mat2ColCount; ++i) {
    Value *newColumnVector = CreateMatrixTimesVector(matrix1, CreateExtractValue(matrix2, i));
    result = CreateInsertValue(result, newColumnVector, i);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create matrix from outer product of vector
//
// @param vector1 : The float vector 1
// @param vector2 : The float vector 2
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateOuterProduct(Value *const vector1, Value *const vector2, const Twine &instName) {
  const unsigned rowCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  const unsigned colCount = cast<FixedVectorType>(vector2->getType())->getNumElements();
  Type *const resultTy = ArrayType::get(vector1->getType(), colCount);
  Value *result = PoisonValue::get(resultTy);

  for (unsigned i = 0; i < colCount; ++i) {
    SmallVector<int, 4> shuffleIdx(rowCount, i);
    Value *columnVector = CreateFMul(vector1, CreateShuffleVector(vector2, vector2, shuffleIdx));
    result = CreateInsertValue(result, columnVector, i);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create matrix determinant operation. Matrix must be square
//
// @param matrix : Matrix
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateDeterminant(Value *const matrix, const Twine &instName) {
  unsigned order = matrix->getType()->getArrayNumElements();
  assert(cast<FixedVectorType>(cast<ArrayType>(matrix->getType())->getElementType())->getNumElements() == order);
  assert(order >= 2);

  // Extract matrix elements.
  SmallVector<Value *, 16> elements;
  for (unsigned columnIdx = 0; columnIdx != order; ++columnIdx) {
    Value *column = CreateExtractValue(matrix, columnIdx);
    for (unsigned rowIdx = 0; rowIdx != order; ++rowIdx)
      elements.push_back(CreateExtractElement(column, rowIdx));
  }

  Value *result = determinant(elements, order);
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Helper function for determinant calculation
//
// @param elements : Elements of matrix (order*order of them)
// @param order : Order of matrix
Value *BuilderImpl::determinant(ArrayRef<Value *> elements, unsigned order) {
  if (order == 1)
    return elements[0];

  if (order == 2) {
    // | x0   x1 |
    // |         | = x0 * y1 - y0 * x1
    // | y0   y1 |
    return CreateFSub(CreateFMul(elements[0], elements[3]), CreateFMul(elements[1], elements[2]));
  }

  // | x0   x1   x2 |
  // |              |        | y1 y2 |        | x1 x2 |        | x1 x2 |
  // | y0   y1   y2 | = x0 * |       | - y0 * |       | + z0 * |       |
  // |              |        | z1 z2 |        | z1 z2 |        | y1 y2 |
  // | z0   z1   z2 |
  SmallVector<Value *, 9> submatrix;
  submatrix.resize((order - 1) * (order - 1));
  Value *result = nullptr;
  for (unsigned leadRowIdx = 0; leadRowIdx != order; ++leadRowIdx) {
    getSubmatrix(elements, submatrix, order, leadRowIdx, 0);
    Value *subdeterminant = CreateFMul(elements[leadRowIdx], determinant(submatrix, order - 1));
    if ((leadRowIdx & 1) != 0)
      result = CreateFSub(result, subdeterminant);
    else {
      if (!result)
        result = subdeterminant;
      else
        result = CreateFAdd(result, subdeterminant);
    }
  }
  return result;
}

// =====================================================================================================================
// Get submatrix by deleting specified row and column
//
// @param matrix : Input matrix (as linearized array of values, order*order of them)
// @param submatrix : Output matrix (ditto, (order-1)*(order-1) of them)
// @param order : Order of input matrix
// @param rowToDelete : Row index to delete
// @param columnToDelete : Column index to delete
void BuilderImpl::getSubmatrix(ArrayRef<Value *> matrix, MutableArrayRef<Value *> submatrix, unsigned order,
                               unsigned rowToDelete, unsigned columnToDelete) {
  unsigned inElementIdx = 0, outElementIdx = 0;
  for (unsigned columnIdx = 0; columnIdx != order; ++columnIdx) {
    for (unsigned rowIdx = 0; rowIdx != order; ++rowIdx) {
      if (rowIdx != rowToDelete && columnIdx != columnToDelete)
        submatrix[outElementIdx++] = matrix[inElementIdx];
      ++inElementIdx;
    }
  }
}

// =====================================================================================================================
// Create matrix inverse operation. Matrix must be square. Result is undefined if the matrix
// is singular or poorly conditioned (nearly singular).
//
// @param matrix : Matrix
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateMatrixInverse(Value *const matrix, const Twine &instName) {
  unsigned order = matrix->getType()->getArrayNumElements();
  assert(cast<FixedVectorType>(cast<ArrayType>(matrix->getType())->getElementType())->getNumElements() == order);
  assert(order >= 2);

  // Extract matrix elements.
  SmallVector<Value *, 16> elements;
  for (unsigned columnIdx = 0; columnIdx != order; ++columnIdx) {
    Value *column = CreateExtractValue(matrix, columnIdx);
    for (unsigned rowIdx = 0; rowIdx != order; ++rowIdx)
      elements.push_back(CreateExtractElement(column, rowIdx));
  }

  // [ x0   x1   x2 ]                   [ Adj(x0) Adj(x1) Adj(x2) ] T
  // [              ]                   [                         ]
  // [ y0   y1   y2 ]  = (1 / det(M)) * [ Adj(y0) Adj(y1) Adj(y2) ]
  // [              ]                   [                         ]
  // [ z0   z1   z2 ]                   [ Adj(z0) Adj(z1) Adj(z2) ]
  //
  // where Adj(a) is the cofactor of a, which is the determinant of the submatrix obtained by deleting
  // the row and column of a, then negated if row+col is odd.

  SmallVector<Value *, 16> resultElements;
  resultElements.resize(order * order);
  SmallVector<Value *, 9> submatrix;
  submatrix.resize((order - 1) * (order - 1));

  // Calculate reciprocal of determinant, and negated reciprocal of determinant.
  Value *rcpDet = CreateFDiv(ConstantFP::get(elements[0]->getType(), 1.0), determinant(elements, order));
  Value *negRcpDet = CreateFSub(Constant::getNullValue(elements[0]->getType()), rcpDet);

  // For each element:
  for (unsigned columnIdx = 0; columnIdx != order; ++columnIdx) {
    for (unsigned rowIdx = 0; rowIdx != order; ++rowIdx) {
      // Calculate cofactor for this element.
      getSubmatrix(elements, submatrix, order, rowIdx, columnIdx);
      // Calculate its determinant.
      Value *cofactor = determinant(submatrix, order - 1);
      // Divide by whole matrix determinant, and negate if row+col is odd.
      cofactor = CreateFMul(cofactor, ((rowIdx + columnIdx) & 1) != 0 ? negRcpDet : rcpDet);
      // Transpose by placing the cofactor in the transpose position.
      resultElements[rowIdx * order + columnIdx] = cofactor;
    }
  }

  // Create the result matrix.
  Value *result = PoisonValue::get(matrix->getType());
  for (unsigned columnIdx = 0; columnIdx != order; ++columnIdx) {
    Value *column = PoisonValue::get(matrix->getType()->getArrayElementType());
    for (unsigned rowIdx = 0; rowIdx != order; ++rowIdx)
      column = CreateInsertElement(column, resultElements[rowIdx + columnIdx * order], rowIdx);
    result = CreateInsertValue(result, column, columnIdx);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Convert the element type enum into the corresponding LLVM type.
//
// @param elemType : The element type enum value
// @returns the corresponding LLVM type
Type *BuilderCommon::transCooperativeMatrixElementType(CooperativeMatrixElementType elemType) {
  switch (elemType) {
  case BuilderCommon::CooperativeMatrixElementType::Float16:
    return getHalfTy();
  case BuilderCommon::CooperativeMatrixElementType::Float32:
    return getFloatTy();
  case BuilderCommon::CooperativeMatrixElementType::Int16:
    return getInt16Ty();
  case BuilderCommon::CooperativeMatrixElementType::Int32:
    return getInt32Ty();
  case BuilderCommon::CooperativeMatrixElementType::Int8:
    return getInt8Ty();
  default:
    llvm_unreachable("The element type is not supported.");
  }
}

// =====================================================================================================================
// Get the LGC type of a cooperative matrix with the given element type and layout.
//
// @param elemType : the matrix element type
// @param layout : the matrix layout
Type *BuilderCommon::getCooperativeMatrixTy(CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout) {
  // Note: the layout currently has no influence on the type. In the long run, we should switch to genuinely opaque
  // types at the LGC level, and parameterize the type using both the element type and the layout.

  Type *wordTy = transCooperativeMatrixElementType(elemType)->isIntOrIntVectorTy() ? getInt32Ty() : getFloatTy();
  switch (layout) {
  case CooperativeMatrixLayout::Gfx10Accumulator16bitMatrixLayout:
  case CooperativeMatrixLayout::Gfx10AccumulatorMatrixLayout:
  case CooperativeMatrixLayout::AccumulatorMatrixLayout:
    return FixedVectorType::get(wordTy, 8);
  case CooperativeMatrixLayout::FactorMatrixLayout:
    if (elemType == CooperativeMatrixElementType::Int8)
      return FixedVectorType::get(wordTy, 4);
    return FixedVectorType::get(wordTy, 8);
  default:
    llvm_unreachable("Type is not supported!");
  }
}

// =====================================================================================================================
// Determine the "length" of a cooperative matrix for purposes of extract/insert operations.
//
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @param instName : name to give instruction(s)
Value *BuilderCommon::CreateCooperativeMatrixLength(CooperativeMatrixElementType elemType,
                                                    CooperativeMatrixLayout layout, const Twine &instName) {
  Type *resultTy = getInt32Ty();
  Value *args[] = {getInt32(static_cast<unsigned>(elemType)), getInt32(static_cast<unsigned>(layout))};
  std::string callName(lgcName::CooperativeMatrixLength);
  addTypeMangling(resultTy, args, callName);

  Value *result =
      CreateNamedCall(callName, resultTy, args, {Attribute::ReadNone, Attribute::Speculatable, Attribute::WillReturn});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create an "extractelement"-equivalent operation for a cooperative matrix value.
//
// @param matrix : the matrix from which to extract an element
// @param index : the index from which to extract
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @param instName : name to give instruction(s)
Value *BuilderCommon::CreateCooperativeMatrixExtract(Value *matrix, Value *index, CooperativeMatrixElementType elemType,
                                                     CooperativeMatrixLayout layout, const Twine &instName) {
  assert(matrix->getType() == getCooperativeMatrixTy(elemType, layout));

  Type *resultTy = transCooperativeMatrixElementType(elemType);
  Value *args[] = {matrix, index, getInt32(static_cast<unsigned>(elemType)), getInt32(static_cast<unsigned>(layout))};
  std::string callName(lgcName::CooperativeMatrixExtract);
  addTypeMangling(resultTy, args, callName);
  Value *result =
      CreateNamedCall(callName, resultTy, args, {Attribute::ReadNone, Attribute::Speculatable, Attribute::WillReturn});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create an "insertelement"-equivalent operation for a cooperative matrix value.
//
// @param matrix : the matrix from which to extract an element
// @param index : the index from which to extract
// @param elemType : the matrix element type
// @param layout : the matrix layout
// @param instName : name to give instruction(s)
Value *BuilderCommon::CreateCooperativeMatrixInsert(Value *matrix, Value *value, Value *index,
                                                    CooperativeMatrixElementType elemType,
                                                    CooperativeMatrixLayout layout, const Twine &instName) {
  assert(matrix->getType() == getCooperativeMatrixTy(elemType, layout));
  assert(value->getType() == transCooperativeMatrixElementType(elemType));
  assert(index->getType() == getInt32Ty());

  Type *resultTy = matrix->getType();
  Value *args[] = {matrix, value, index, getInt32(static_cast<unsigned>(elemType)),
                   getInt32(static_cast<unsigned>(layout))};
  std::string callName(lgcName::CooperativeMatrixInsert);
  addTypeMangling(resultTy, args, callName);
  Value *result =
      CreateNamedCall(callName, resultTy, args, {Attribute::ReadNone, Attribute::Speculatable, Attribute::WillReturn});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create cooperative matrix load.
// We only allow the size 16x16 size for a cooperative matrix. So 16 lanes are responsible for reading all data from
// memory. The layout of a cooperative matrix A in the VGPR under wave32 mode is that . Each lane reads a contiguous
// data from memory as a row (or column) of matrix A into the VGPR (implemented as a vector), where A0_0 in one VGPR if
// the data format is f32/i32, A0_0/A0_1 would be in the same VGPR if the data format is f16, A0_0/A0_1/A0_2/A0_3 would
// be in the same VGPR if the data format is i8.
//
// @param pointer : The pointer to a data array.
// @param stride : The number of bytes in memory between the first component of consecutive rows (or columns) in the
// source data. Must be a multiple of the matrix element size.
// @param colMaj : Whether the values loaded from memory are arrayed in column-major or row-major.
// @param elemType : Element type for the matrix.
// @param layout : Identify whether it's A/B or C/D
// @param memoryAccess : Parsed from memory operation.
// @param instName : Name to give instruction(s).
Value *BuilderCommon::CreateCooperativeMatrixLoad(Value *pointer, Value *stride, bool colMajor,
                                                  CooperativeMatrixElementType elemType, CooperativeMatrixLayout layout,
                                                  unsigned memoryAccess, const Twine &instName) {
  Type *resultTy = getCooperativeMatrixTy(elemType, layout);
  std::string callName(lgcName::CooperativeMatrixLoad);
  Value *args[] = {pointer,
                   stride,
                   getInt1(colMajor),
                   getInt32(static_cast<unsigned>(elemType)),
                   getInt32(static_cast<unsigned>(layout)),
                   getInt32(memoryAccess)};
  addTypeMangling(resultTy, args, callName);
  Value *loadVal = CreateNamedCall(callName, resultTy, args, {Attribute::ReadOnly});
  loadVal->setName(instName);
  return loadVal;
}

// =====================================================================================================================
// Create cooperative matrix store.
// We only allow the size 16x16 size for a cooperative matrix. So 16 lanes are responsible for writing matrix elements
// to memory. The layout of a cooperative matrix A in the VGPR under wave32 mode is that each lane writes a row (or
// column) of matrix A from the VGPRs (implemented as a vector) to the memory, where the value of one VGPR is written
// into a memory location if the data format is f32/i32, the value of one VGPR is split into two values to store if
// the data format is f16, the value of one VGPR is split into four values to store if the data format is i8.
//
// @param pointer : The pointer to a data array.
// @param matrix : The row of cooperative matrix to store.
// @param stride : The number of bytes in memory between the first components of consecutive rows (or columns) in the
// destination. Must be a multiple of the element size.
// @param colMaj : Whether the values loaded from memory are arrayed in column-major or row-major.
// @param elemType : Element type for the matrix.
// @param layout : Identify the matrix type(A/B or C).
// @param memoryAccess : Memoray operands
// @param instName : Name to give instruction(s).
Value *BuilderCommon::CreateCooperativeMatrixStore(Value *pointer, Value *matrix, Value *stride, bool colMajor,
                                                   CooperativeMatrixElementType elemType,
                                                   CooperativeMatrixLayout layout, unsigned memoryAccess,
                                                   const Twine &instName) {
  assert(matrix->getType() == getCooperativeMatrixTy(elemType, layout));

  std::string callName(lgcName::CooperativeMatrixStore);
  Value *args[] = {pointer,
                   stride,
                   getInt1(colMajor),
                   getInt32(static_cast<unsigned>(elemType)),
                   getInt32(static_cast<unsigned>(layout)),
                   getInt32(memoryAccess),
                   matrix};
  addTypeMangling(Type::getVoidTy(getContext()), args, callName);

  Value *storeVal =
      CreateNamedCall(callName, Type::getVoidTy(getContext()), args, {Attribute::WriteOnly, Attribute::WillReturn});
  storeVal->setName(instName);
  return nullptr;
}

// =====================================================================================================================
// Create cooperative matrix conversion.
// Element-wise-conversion
// @param castOp : The cast Opcode.
// @param source : The source cooperative matrix.
// @param srcElemTy : Source matrix's element type.
// @param dstElemTy : Destination matrix's element type.
// @param srcLayout : Layout for source matrix
// @param dstLayout : Layout for target matrix
// @param instName : Name to give instruction(s).
CallInst *BuilderCommon::CreateCooperativeMatrixConvert(CastInst::CastOps castOp, Value *source,
                                                        CooperativeMatrixElementType srcElemTy,
                                                        CooperativeMatrixElementType dstElemTy,
                                                        CooperativeMatrixLayout srcLayout,
                                                        CooperativeMatrixLayout dstLayout, const Twine &instName) {
  assert(source->getType() == getCooperativeMatrixTy(srcElemTy, srcLayout));

  Value *args[] = {getInt32(static_cast<unsigned>(castOp)),    source,
                   getInt32(static_cast<unsigned>(srcElemTy)), getInt32(static_cast<unsigned>(dstElemTy)),
                   getInt32(static_cast<unsigned>(srcLayout)), getInt32(static_cast<unsigned>(dstLayout))};
  Type *resultTy = getCooperativeMatrixTy(dstElemTy, dstLayout);
  std::string callName(lgcName::CooperativeMatrixConvert);
  addTypeMangling(resultTy, args, callName);

  CallInst *dstElems = CreateNamedCall(callName, resultTy, args, {Attribute::ReadOnly, Attribute::WillReturn});
  dstElems->setName(instName);
  return dstElems;
}
// =====================================================================================================================
// Create cooperative matrix binary operation
//
// @param coopMatArithOp : The cooperative matrix arithmetic operation to perform.
// @param lhs : The first operand and it can be a scalar or a cooperative matrix.
// @param rhs : The second operand and it should be a cooperative matrix.
// @param elemType : Element type for the matrix.
// @param layout : Layout for the matrix.
// @param instName : Name to give instruction(s).
Value *BuilderCommon::CreateCooperativeMatrixBinaryOp(CooperativeMatrixArithOp coopMatArithOp, Value *lhs, Value *rhs,
                                                      CooperativeMatrixElementType elemType,
                                                      CooperativeMatrixLayout layout, const Twine &instName) {
  assert(lhs->getType() == getCooperativeMatrixTy(elemType, layout));
  assert(lhs->getType() == rhs->getType());

  std::string callName(lgcName::CooperativeMatrixBinOp);
  Value *args[] = {getInt32(static_cast<unsigned>(coopMatArithOp)), lhs, rhs, getInt32(static_cast<unsigned>(elemType)),
                   getInt32(static_cast<unsigned>(layout))};
  addTypeMangling(rhs->getType(), args, callName);

  Value *result = CreateNamedCall(callName, rhs->getType(), args, {Attribute::ReadOnly, Attribute::WillReturn});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create cooperative matrix MatrixTimesScalar operation
//
// @param matrix : The first operand and it should be a cooperative matrix.
// @param scalar : The second operand and it should be a scalar.
// @param elemType : The component type of the matrix.
// @param layout : Identify whether it's A/B or C/D
// @param instName : Name to give instruction(s).
Value *BuilderCommon::CreateCoopMatrixTimesScalar(Value *matrix, Value *scalar, CooperativeMatrixElementType elemType,
                                                  CooperativeMatrixLayout layout, const Twine &instName) {
  assert(matrix->getType() == getCooperativeMatrixTy(elemType, layout));
  assert(scalar->getType() == transCooperativeMatrixElementType(elemType));

  std::string callName(lgcName::CooperativeMatrixTimesScalar);
  Value *args[] = {matrix, scalar, getInt32(static_cast<unsigned>(elemType)), getInt32(static_cast<unsigned>(layout))};
  addTypeMangling(matrix->getType(), args, callName);

  Value *result = CreateNamedCall(callName, matrix->getType(), args, {Attribute::ReadOnly, Attribute::WillReturn});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create cooperative matrix transpose operation
//
// @param matrix : The first operand and it should be a cooperative matrix.
// @param elemType : The component type of the matrix.
// @param layout : Identify whether it's A/B or C/D
// @param instName : Name to give instruction(s).
CallInst *BuilderCommon::CreateCooperativeMatrixTranspose(llvm::Value *matrix, CooperativeMatrixElementType elemType,
                                                          CooperativeMatrixLayout layout, const Twine &instName) {
  assert(matrix->getType() == getCooperativeMatrixTy(elemType, layout));

  std::string callName(lgcName::CooperativeMatrixTranspose);
  Value *args[] = {matrix, getInt32(static_cast<unsigned>(elemType)), getInt32(static_cast<unsigned>(layout))};
  addTypeMangling(matrix->getType(), args, callName);

  CallInst *result = CreateNamedCall(callName, matrix->getType(), args, {Attribute::ReadOnly, Attribute::WillReturn});
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create cooperative matrix muladd operation
//
// @param matrixA : Factor cooperative matrix.
// @param matrixB : Factor cooperative matrix.
// @param matrixC : Accumulator cooperative matrix.
// @param isSignedA : Identify the signess for matrix A's element type
// @param isSignedB : Identify the signess for matrix B's element type
// @param isSat : SaturatingAccumulation for calculation
// @param accumElemType : The component type of the accumulator matrix.
// @param factorElemType : The component type of the factor matrix.
Value *BuilderCommon::CreateCooperativeMatrixMulAdd(llvm::Value *matrixA, llvm::Value *matrixB, llvm::Value *matrixC,
                                                    bool isSignedA, bool isSignedB, bool isSat,
                                                    CooperativeMatrixElementType accumElemType,
                                                    CooperativeMatrixElementType factorElemType,
                                                    const llvm::Twine &instName) {
  std::string callName(lgcName::CooperativeMatrixMulAdd);
  Value *args[] = {matrixA,
                   matrixB,
                   matrixC,
                   getInt1(isSignedA),
                   getInt1(isSignedB),
                   getInt1(isSat),
                   getInt32(static_cast<unsigned>(accumElemType)),
                   getInt32(static_cast<unsigned>(factorElemType))};
  addTypeMangling(matrixC->getType(), args, callName);

  Value *result = CreateNamedCall(callName, matrixC->getType(), args, {Attribute::ReadOnly, Attribute::WillReturn});
  result->setName(instName);
  return result;
}
