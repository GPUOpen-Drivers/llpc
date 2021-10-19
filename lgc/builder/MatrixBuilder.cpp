/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
#include "BuilderImpl.h"

#define DEBUG_TYPE "lgc-builder-impl-matrix"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a matrix transpose.
//
// @param matrix : Matrix to transpose.
// @param instName : Name to give final instruction
Value *MatrixBuilder::CreateTransposeMatrix(Value *const matrix, const Twine &instName) {
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
    newColumns.push_back(UndefValue::get(newColumnVectorType));

  for (unsigned column = 0; column < columnCount; column++) {
    for (unsigned row = 0; row < rowCount; row++) {
      Value *const element = CreateExtractElement(columns[column], row);
      newColumns[row] = CreateInsertElement(newColumns[row], element, column);
    }
  }

  Value *newMatrix = UndefValue::get(newMatrixType);

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
Value *MatrixBuilder::CreateMatrixTimesScalar(Value *const matrix, Value *const scalar, const Twine &instName) {
  Type *const matrixTy = matrix->getType();
  Type *const columnTy = matrixTy->getArrayElementType();
  const unsigned rowCount = cast<FixedVectorType>(columnTy)->getNumElements();
  unsigned columnCount = matrixTy->getArrayNumElements();
  auto smearScalar = CreateVectorSplat(rowCount, scalar);

  Value *result = UndefValue::get(matrixTy);
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
Value *MatrixBuilder::CreateVectorTimesMatrix(Value *const vector, Value *const matrix, const Twine &instName) {
  Type *const matrixTy = matrix->getType();
  Type *const compTy = cast<VectorType>(cast<ArrayType>(matrixTy)->getElementType())->getElementType();
  const unsigned columnCount = matrixTy->getArrayNumElements();
  Type *const resultTy = FixedVectorType::get(compTy, columnCount);
  Value *result = UndefValue::get(resultTy);

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
Value *MatrixBuilder::CreateMatrixTimesVector(Value *const matrix, Value *const vector, const Twine &instName) {
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
Value *MatrixBuilder::CreateMatrixTimesMatrix(Value *const matrix1, Value *const matrix2, const Twine &instName) {
  Type *const mat1ColumnType = matrix1->getType()->getArrayElementType();
  const unsigned mat2ColCount = matrix2->getType()->getArrayNumElements();
  Type *const resultTy = ArrayType::get(mat1ColumnType, mat2ColCount);
  Value *result = UndefValue::get(resultTy);

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
Value *MatrixBuilder::CreateOuterProduct(Value *const vector1, Value *const vector2, const Twine &instName) {
  const unsigned rowCount = cast<FixedVectorType>(vector1->getType())->getNumElements();
  const unsigned colCount = cast<FixedVectorType>(vector2->getType())->getNumElements();
  Type *const resultTy = ArrayType::get(vector1->getType(), colCount);
  Value *result = UndefValue::get(resultTy);

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
Value *MatrixBuilder::CreateDeterminant(Value *const matrix, const Twine &instName) {
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
Value *MatrixBuilder::determinant(ArrayRef<Value *> elements, unsigned order) {
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
void MatrixBuilder::getSubmatrix(ArrayRef<Value *> matrix, MutableArrayRef<Value *> submatrix, unsigned order,
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
Value *MatrixBuilder::CreateMatrixInverse(Value *const matrix, const Twine &instName) {
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
  Value *result = UndefValue::get(matrix->getType());
  for (unsigned columnIdx = 0; columnIdx != order; ++columnIdx) {
    Value *column = UndefValue::get(matrix->getType()->getArrayElementType());
    for (unsigned rowIdx = 0; rowIdx != order; ++rowIdx)
      column = CreateInsertElement(column, resultElements[rowIdx + columnIdx * order], rowIdx);
    result = CreateInsertValue(result, column, columnIdx);
  }

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
// @param stride : The number of elements in the array in memory between the first component of consecutive rows (or
// columns) in the result.
// @param colMaj : Whether the values loaded from memory are arrayed in column-major or row-major.
// @param alignment : The alignment for physical buffer storage operation.
// @param instName : Name to give instruction(s).
Value *MatrixBuilder::CreateCooperativeMatrixLoad(Value *pointer, Value *stride, Value *colMajor, Value *alignment,
                                                  const Twine &instName) {
  CooperativeMatrixInfo coopMatInfo = {};
  calcCooperativeMatrixInfo(coopMatInfo, pointer);

  // Load data from a contigous memroy location into a vector
  Value *loadVal = nullptr;
  doMemoryAccess(pointer, stride, alignment, loadVal);

  // Construct a row of cooperative matrix
  Value *coopMatRow = loadVal;
  if (coopMatInfo.subElemCount > 1) {
    coopMatRow = UndefValue::get(FixedVectorType::get(coopMatInfo.elemType, coopMatInfo.elemCount));
    // An 32-bit element is made up of multiple sub-elements
    Value *elemVec = UndefValue::get(FixedVectorType::get(coopMatInfo.subElemType, coopMatInfo.subElemCount));
    for (unsigned elemIdx = 0; elemIdx < coopMatInfo.elemCount; ++elemIdx) {
      for (unsigned subIdx = 0; subIdx < coopMatInfo.subElemCount; ++subIdx) {
        unsigned locIdx = elemIdx * coopMatInfo.subElemCount + subIdx;
        Value *subElem = CreateExtractElement(loadVal, locIdx);
        elemVec = CreateInsertElement(elemVec, subElem, subIdx);
      }
      Value *elem = CreateBitCast(elemVec, coopMatInfo.elemType);
      coopMatRow = CreateInsertElement(coopMatRow, elem, getInt32(elemIdx));
    }
  }

  coopMatRow->setName(instName);
  return coopMatRow;
}

// =====================================================================================================================
// Create cooperative matrix store.
// We only allow the size 16x16 size for a cooperative matrix. So 16 lanes are responsible for writing matrix elements
// to memory. The layout of a cooperative matrix A in the VGPR under wave32 mode is that each lane writes a row (or
// column) of matrix A from the VGPRs (implemented as a vector) to the memory, where the value of one VGPR is written
// into a memory location if the data format is f32/i32, the value of one VGPR is splitted into two values to store if
// the data format is f16, the value of one VGPR is splitted into four values to store if the data format is i8.
//
// @param pointer : The pointer to a data array.
// @param object : The row of cooperative matrix to store.
// @param stride : The number of elements in the array in memory between the first component of consecutive rows (or
// columns) in the result.
// @param colMaj : Whether the values loaded from memory are arrayed in column-major or row-major.
// @param alignment : The alignment for physical buffer storage operation.
// @param instName : Name to give instruction(s).
Value *MatrixBuilder::CreateCooperativeMatrixStore(Value *pointer, Value *object, Value *stride, Value *colMajor,
                                                   Value *alignment, const Twine &instName) {
  // Object is a row (or column) of a cooperative matrix 16x16 size (a vector).
  assert(object->getType()->isVectorTy());

  CooperativeMatrixInfo coopMatInfo = {};
  calcCooperativeMatrixInfo(coopMatInfo, pointer);

  Value *storeVal = object;
  // The sub-elements of a row are stored in contigous locations as a vector.
  if (coopMatInfo.subElemCount > 1) {
    constexpr unsigned numOfData = 16;
    Type *storeTy = FixedVectorType::get(coopMatInfo.subElemType, numOfData);
    storeVal = UndefValue::get(storeTy);
    // The stored data is an sub-element of the element of a row vector.
    Type *elemVecTy = FixedVectorType::get(coopMatInfo.subElemType, coopMatInfo.subElemCount);
    for (unsigned elemIdx = 0; elemIdx < coopMatInfo.elemCount; ++elemIdx) {
      Value *elem = CreateExtractElement(object, elemIdx);
      elem = CreateBitCast(elem, elemVecTy);
      for (unsigned subIdx = 0; subIdx < coopMatInfo.subElemCount; ++subIdx) {
        Value *subElem = CreateExtractElement(elem, subIdx);
        const unsigned locIdx = elemIdx * coopMatInfo.subElemCount + subIdx;
        storeVal = CreateInsertElement(storeVal, subElem, locIdx);
      }
    }
  }
  // Store the vector to the memory.
  doMemoryAccess(pointer, stride, alignment, storeVal);

  return nullptr;
}

// =====================================================================================================================
// Create cooperative matrix conversion.
//
// @param source : The source cooperative matrix.
// @param target : The convertion target.
// @param instName : Name to give instruction(s).
Value *MatrixBuilder::CreateCooperativeMatrixConvert(Value *source, Value *target, const Twine &instName) {
  assert((source->getType()->getScalarType()->isIntegerTy() && target->getType()->getScalarType()->isIntegerTy()) ||
         (source->getType()->getScalarType()->isFloatTy() && target->getType()->getScalarType()->isFloatTy()));

  if (source->getType() == target->getType())
    return source;

  CooperativeMatrixInfo coopMatInfoSrc = {};
  calcCooperativeMatrixInfo(coopMatInfoSrc, source);

  CooperativeMatrixInfo coopMatInfoTgt = {};
  calcCooperativeMatrixInfo(coopMatInfoTgt, target);

  Value *result = target;

  if (coopMatInfoSrc.elemCount > coopMatInfoTgt.elemCount) {
    // The element of the target row vector is packed by contigous elements of the source row vector.
    // E.g., tgt[0] is made up of src[0] and src[1]
    Type *vecTy = FixedVectorType::get(coopMatInfoTgt.subElemType, coopMatInfoTgt.subElemCount);
    Value *tgtElemVec = UndefValue::get(vecTy);
    for (unsigned tgtElemIdx = 0; tgtElemIdx < coopMatInfoTgt.elemCount; ++tgtElemIdx) {
      for (unsigned subIdx = 0; subIdx < coopMatInfoTgt.subElemCount; ++subIdx) {
        const unsigned srcElemIdx = tgtElemIdx * coopMatInfoTgt.subElemCount + subIdx;
        Value *srcElem = CreateExtractElement(source, srcElemIdx);
        srcElem = coopMatInfoSrc.elemType->isFloatTy() ? CreateFPTrunc(srcElem, coopMatInfoTgt.subElemType)
                                                       : CreateTrunc(srcElem, coopMatInfoTgt.subElemType);
        tgtElemVec = CreateInsertElement(tgtElemVec, srcElem, subIdx);
      }
      Value *tgtElem = CreateBitCast(tgtElemVec, coopMatInfoTgt.elemType);
      result = CreateInsertElement(result, tgtElem, tgtElemIdx);
    }
  } else {
    // The element of source row vector is packed bu sub-elements. The element of the target row vector is source
    // sub-element. E.g., tgt[0] and tgt[1] are from src[0]
    Type *vecTy = FixedVectorType::get(coopMatInfoSrc.subElemType, coopMatInfoSrc.subElemCount);
    for (unsigned srcElemIdx = 0; srcElemIdx < coopMatInfoSrc.elemCount; ++srcElemIdx) {
      Value *srcElem = CreateExtractElement(source, srcElemIdx);
      Value *srcElemVec = CreateBitCast(srcElem, vecTy);
      for (unsigned subIdx = 0; subIdx < coopMatInfoSrc.subElemCount; ++subIdx) {
        const unsigned tgtElemIdx = srcElemIdx * coopMatInfoSrc.subElemCount + subIdx;
        Value *tgtElem = CreateExtractElement(srcElemVec, subIdx);
        tgtElem = coopMatInfoTgt.elemType->isFloatTy() ? CreateFPExt(tgtElem, coopMatInfoTgt.elemType)
                                                       : CreateSExt(tgtElem, coopMatInfoTgt.elemType);
        result = CreateInsertElement(result, tgtElem, tgtElemIdx);
      }
    }
  }
  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create cooparetive matrix binary operation
//
// @param coopMatArithOp : The cooperative matrix arithemtic operation to perform.
// @param operand1 : The first operand and it can be a scalar or a cooperative matrix.
// @param operand2 : The second operand and it should be a cooperative matrix.
// @param instName : Name to give instruction(s).
Value *MatrixBuilder::CreateCooperativeMatrixBinaryOp(CooperativeMatrixArithOp coopMatArithOp, Value *operand1,
                                                      Value *operand2, const Twine &instName) {
  assert(operand1->getType()->isVectorTy() && operand1->getType() == operand2->getType() ||
         operand2->getType()->isVectorTy());

  std::function<Value *(Builder *, Value *, Value *)> mapFunc;
  switch (coopMatArithOp) {
  case CooperativeMatrixArithOp::IAdd:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateAdd(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::FAdd:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateFAdd(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::ISub:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateSub(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::FSub:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateFSub(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::FDiv:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateFDiv(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::SDiv:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateSDiv(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::UDiv:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateUDiv(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::IMul:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateMul(scalar1, scalar2);
    };
    break;
  case CooperativeMatrixArithOp::FMul:
    mapFunc = [](Builder *builder, Value *scalar1, Value *scalar2) -> Value * {
      return builder->CreateFMul(scalar1, scalar2);
    };
    break;
  default:
    llvm_unreachable("unsupported binary operation for cooprative matrix!");
  }

  CooperativeMatrixInfo coopMatInfo = {};
  calcCooperativeMatrixInfo(coopMatInfo, operand2);
  Value *result = UndefValue::get(operand2->getType());

  const bool operand1IsVector = operand1->getType()->isVectorTy();
  assert(operand1IsVector || operand1->getType() == coopMatInfo.subElemType);
  // NOTE: operand1 can be a vector or a scalar for OpMatrixTimesScalar
  if (coopMatInfo.subElemCount > 1) {
    // Do the binary operation on the sub-elements
    for (unsigned elemIdx = 0; elemIdx < coopMatInfo.elemCount; ++elemIdx) {
      // Get elements from operand1 and operand2, respectively.
      Value *elem1 = operand1IsVector ? CreateExtractElement(operand1, elemIdx) : operand1;
      Value *elem2 = CreateExtractElement(operand2, elemIdx);
      Type *vecTy = FixedVectorType::get(coopMatInfo.subElemType, coopMatInfo.subElemCount);
      Value *elemVec1 = operand1IsVector ? CreateBitCast(elem1, vecTy) : nullptr;
      Value *elemVec2 = CreateBitCast(elem2, vecTy);
      Value *resElemVec = UndefValue::get(vecTy);
      for (unsigned subIdx = 0; subIdx < coopMatInfo.subElemCount; ++subIdx) {
        Value *subElem1 = operand1IsVector ? CreateExtractElement(elemVec1, subIdx) : operand1;
        Value *subElem2 = CreateExtractElement(elemVec2, subIdx);
        Value *resSubElem = mapFunc(this, subElem1, subElem2);
        resElemVec = CreateInsertElement(resElemVec, resSubElem, subIdx);
      }
      Value *resElem = CreateBitCast(resElemVec, coopMatInfo.elemType);
      result = CreateInsertElement(result, resElem, elemIdx);
    }
  } else {
    // Do the binary operation on elements.
    for (unsigned elemIdx = 0; elemIdx < coopMatInfo.elemCount; ++elemIdx) {
      Value *elem1 = operand1IsVector ? CreateExtractElement(operand1, elemIdx) : operand1;
      Value *elem2 = CreateExtractElement(operand2, elemIdx);
      Value *elem = mapFunc(this, elem1, elem2);
      result = CreateInsertElement(result, elem, elemIdx);
    }
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create extracted component from a row of cooperative matrix.
//
// @param coopMatRow : The cooperative matrix.
// @param index : The component index of the cooperative matrix.
// @param instName : Name to give instruction(s).
Value *MatrixBuilder::CreateCooperativeMatrixExtract(Value *coopMatRow, Value *index, const Twine &instName) {
  CooperativeMatrixInfo coopMatInfo = {};
  calcCooperativeMatrixInfo(coopMatInfo, coopMatRow);

  // The size of a cooperative matrix is 16x16 so that the range of index is [0~15].
  // The layout in VGPRs of a row of a cooperative matrix may be packed if the component type is f16 or i8.
  if (coopMatInfo.subElemCount > 1) {
    // Calculate the element index and sub-element index for the packed row vector from the input index.
    // index = elemIdx * subElemeCount + subIdx
    Value *divisor = getInt32(coopMatInfo.subElemCount);
    Value *elemIdx = CreateUDiv(index, divisor);
    Value *subIdx = CreateURem(index, divisor);

    // Get the element
    Value *elem = CreateExtractElement(coopMatRow, elemIdx);
    Type *vecTy = FixedVectorType::get(coopMatInfo.subElemType, coopMatInfo.subElemCount);
    Value *elemVec = CreateBitCast(elem, vecTy);
    Value *subElem = CreateExtractElement(elemVec, subIdx);
    return subElem;
  }
  return CreateExtractElement(coopMatRow, index);
}

// =====================================================================================================================
// Create a row of cooperative matrix from a constant.
//
// @param coopMatRow : The cooperative matrix.
// @param constVal : The constant value used to construct a cooperative matrix.
// @param instName : Name to give instruction(s).
Value *MatrixBuilder::CreateCooperativeMatrixConstruct(Value *coopMatRow, Value *constVal, const Twine &instName) {
  CooperativeMatrixInfo coopMatInfo = {};
  calcCooperativeMatrixInfo(coopMatInfo, coopMatRow);

  assert(constVal->getType()->getScalarSizeInBits() == coopMatInfo.subElemType->getScalarSizeInBits());

  Value *elem = constVal;
  if (coopMatInfo.subElemCount > 1) {
    // The constant value is a sub-element and the element of the row vector is packed by sub-elements.
    Type *vecTy = FixedVectorType::get(coopMatInfo.subElemType, coopMatInfo.subElemCount);
    elem = UndefValue::get(vecTy);
    for (unsigned subIdx = 0; subIdx < coopMatInfo.subElemCount; ++subIdx)
      elem = CreateInsertElement(elem, constVal, subIdx);
    elem = CreateBitCast(elem, coopMatInfo.elemType);
  }

  for (unsigned elemIdx = 0; elemIdx < coopMatInfo.elemCount; ++elemIdx)
    coopMatRow = CreateInsertElement(coopMatRow, elem, elemIdx);
  return coopMatRow;
}

// =====================================================================================================================
// Calculate the info of cooperative matrix
//
// @param coopMatInfo [in/out] : The calculated info of a cooperative matrix
// @param data : It can be a pointer to a data array or a coopertive matrix.
void MatrixBuilder::calcCooperativeMatrixInfo(CooperativeMatrixInfo &coopMatInfo, Value *data) {
  // A row (a column) of a cooperative matrix is implemented as a vector and the vector form is determined by the data
  // format: <16 x float> or <16 x i32> if the data format is f32/i32, <8 x float> if the data format is f16, <4 x i32>
  // if the data format is i8.
  assert(data->getType()->isPointerTy() || data->getType()->isVectorTy());
  // subElemCount * elemCount = 16
  if (data->getType()->isPointerTy()) {
    Type *dataTy = data->getType()->getPointerElementType();
    coopMatInfo.subElemCount = 32 / dataTy->getScalarSizeInBits();
    coopMatInfo.elemCount = 16 / coopMatInfo.subElemCount;
    coopMatInfo.elemType = dataTy->isIntegerTy() ? getInt32Ty() : getFloatTy();
  } else {
    coopMatInfo.elemCount = cast<FixedVectorType>(data->getType())->getNumElements();
    coopMatInfo.elemType = data->getType()->getScalarType();
    coopMatInfo.subElemCount = 16 / coopMatInfo.elemCount;
  }

  if (coopMatInfo.elemCount == 16)
    coopMatInfo.subElemType = coopMatInfo.elemType;
  else if (coopMatInfo.elemCount == 8)
    coopMatInfo.subElemType = getHalfTy();
  else
    coopMatInfo.subElemType = getInt8Ty();
}

// =====================================================================================================================
// Load or store a contigous elements from the specified location of the memory.
//
// @param dataPtr : The pointer to a data array.
// @param stride : The number of elements in the array in memory between the first component of consecutive rows (or
// columns) in the result.
// @param alignment : The alignment for physical buffer storage operation.
// @param vecVal : The contigous elements made up of a vector to be loaded or stored.
void MatrixBuilder::doMemoryAccess(Value *dataPtr, Value *stride, Value *alignment, Value *&vecVal) {
  assert(isa<GetElementPtrInst>(dataPtr));
  auto getElemPtrInst = dyn_cast<GetElementPtrInst>(dataPtr);

  // Lanes 0-15 data is replicated into lanes 16-31 (for wave64: also lanes 32-47 into 48-63).
  Value *threadId = CreateGetLaneNumber();
  constexpr unsigned numOfData = 16;
  threadId =
      CreateSelect(CreateICmpULT(threadId, getInt32(numOfData)), threadId, CreateSMod(threadId, getInt32(numOfData)));

  // The elements stored (or loaded) into (or from) LDS (or buffer) in order to contiguous locations starting at
  // dataPtr[element + threadId * stride].
  // Calculate the starting location.
  Value *startLoc = CreateMul(threadId, stride);
  const unsigned addrSpace = dataPtr->getType()->getPointerAddressSpace();
  assert(addrSpace == ADDR_SPACE_LOCAL || addrSpace == ADDR_SPACE_BUFFER_FAT_POINTER || addrSpace == ADDR_SPACE_GLOBAL);
  unsigned elemIdx = addrSpace == ADDR_SPACE_LOCAL ? 1 : 2;
  Value *elemOffset = *(getElemPtrInst->idx_begin() + elemIdx);
  elemOffset = CreateTrunc(elemOffset, getInt32Ty());
  startLoc = CreateAdd(startLoc, elemOffset);

  // Calculate the alignement for the store or load operation.
  assert(isa<ConstantInt>(alignment));
  unsigned align = 0;
  if (!cast<ConstantInt>(alignment)->isNullValue()) {
    align = cast<ConstantInt>(alignment)->getZExtValue(); // Physical buffer storage
  } else {
    const unsigned dataBitwidth = dataPtr->getType()->getPointerElementType()->getScalarSizeInBits();
    align = dataBitwidth / 8;
  }

  // Calculate the pointer to store/load the contigous elements as a vector
  Value *basePtr = getElemPtrInst->getPointerOperand();
  Value *vecPtr = CreateGEP(basePtr->getType()->getPointerElementType(), basePtr,
                            ArrayRef<Value *>{getInt32(0), getInt32(0), startLoc});
  Type *vecTy = FixedVectorType::get(dataPtr->getType()->getPointerElementType(), numOfData);
  vecPtr = CreateBitCast(vecPtr, PointerType::get(vecTy, cast<PointerType>(vecPtr->getType())->getAddressSpace()));
  if (vecVal)
    CreateAlignedStore(vecVal, vecPtr, Align(align * numOfData));
  else
    vecVal = CreateAlignedLoad(vecTy, vecPtr, Align(align * numOfData));
}
