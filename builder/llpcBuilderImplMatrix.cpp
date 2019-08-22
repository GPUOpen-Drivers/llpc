/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcBuilderImplMatrix.cpp
 * @brief LLPC source file: implementation of matrix Builder methods
 ***********************************************************************************************************************
 */
#include "llpcBuilderImpl.h"

#define DEBUG_TYPE "llpc-builder-impl-matrix"

using namespace Llpc;
using namespace llvm;

// =====================================================================================================================
// Create a matrix transpose.
Value* BuilderImplMatrix::CreateTransposeMatrix(
    Value* const pMatrix,  // [in] Matrix to transpose.
    const Twine& instName) // [in] Name to give final instruction
{
    LLPC_ASSERT(pMatrix != nullptr);

    Type* const pMatrixType = pMatrix->getType();
    LLPC_ASSERT(pMatrixType->isArrayTy());

    Type* const pColumnVectorType = pMatrixType->getArrayElementType();
    LLPC_ASSERT(pColumnVectorType->isVectorTy());

    const uint32_t columnCount = pMatrixType->getArrayNumElements();
    const uint32_t rowCount = pColumnVectorType->getVectorNumElements();

    Type* const pElementType = pColumnVectorType->getVectorElementType();

    Type* const pNewColumnVectorType = VectorType::get(pElementType, columnCount);
    Type* const pNewMatrixType = ArrayType::get(pNewColumnVectorType, rowCount);

    SmallVector<Value*, 4> columns;

    for (uint32_t column = 0; column < columnCount; column++)
    {
        columns.push_back(CreateExtractValue(pMatrix, column));
    }

    SmallVector<Value*, 4> newColumns;

    for (uint32_t row = 0; row < rowCount; row++)
    {
        newColumns.push_back(UndefValue::get(pNewColumnVectorType));
    }

    for (uint32_t column = 0; column < columnCount; column++)
    {
        for (uint32_t row = 0; row < rowCount; row++)
        {
            Value* const pElement = CreateExtractElement(columns[column], row);
            newColumns[row] = CreateInsertElement(newColumns[row], pElement, column);
        }
    }

    Value* pNewMatrix = UndefValue::get(pNewMatrixType);

    for (uint32_t row = 0; row < rowCount; row++)
    {
        pNewMatrix = CreateInsertValue(pNewMatrix, newColumns[row], row);
    }

    pNewMatrix->setName(instName);
    return pNewMatrix;
}

// =====================================================================================================================
// Create matrix from matrix Times scalar
Value* BuilderImplMatrix::CreateMatrixTimesScalar(
    Value* const pMatrix,             // [in] The column major matrix, n x <n x float>
    Value* const pScalar,             // [in] The float scalar
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Type* const pMatrixTy = pMatrix->getType();
    Type* const pColumnTy = pMatrixTy->getArrayElementType();
    const uint32_t rowCount = pColumnTy->getVectorNumElements();
    uint32_t columnCount = pMatrixTy->getArrayNumElements();
    auto pSmearScalar = CreateVectorSplat(rowCount, pScalar);

    Value* pResult = UndefValue::get(pMatrixTy);
    for (uint32_t column = 0; column < columnCount; column++)
    {
        auto pColumnVector = CreateExtractValue(pMatrix, column);
        pColumnVector = CreateFMul(pColumnVector, pSmearScalar);
        pResult = CreateInsertValue(pResult, pColumnVector, column);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create vector from vector Times matrix
Value* BuilderImplMatrix::CreateVectorTimesMatrix(
    Value* const pVector,         // [in] The float vector
    Value* const pMatrix,         // [in] The column major matrix, n x <n x float>
    const Twine& instName)        // [in] Name to give instruction(s)
{
    Type* const pMatrixTy = pMatrix->getType();
    Type* const pCompTy = pMatrixTy->getArrayElementType()->getVectorElementType();
    const uint32_t columnCount = pMatrixTy->getArrayNumElements();
    Type* const pResultTy = VectorType::get(pCompTy, columnCount);
    Value* pResult = UndefValue::get(pResultTy);

    for (uint32_t column = 0; column < columnCount; column++)
    {
        auto pColumnVector = CreateExtractValue(pMatrix, column);
        pResult = CreateInsertElement(pResult, CreateDotProduct(pColumnVector, pVector), column);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create vector from matrix times vector
Value* BuilderImplMatrix::CreateMatrixTimesVector(
    Value* const pMatrix,             // [in] The column major matrix, n x <n x float>
    Value* const pVector,             // [in] The vector
    const Twine& instName)            // [in] Name to give instruction(s)
{
    Type* const pColumnTy = pMatrix->getType()->getArrayElementType();
    const uint32_t rowCount = pColumnTy->getVectorNumElements();
    Value* pResult = nullptr;

    for (uint32_t i = 0; i < pMatrix->getType()->getArrayNumElements(); ++i)
    {
        SmallVector<uint32_t, 4> shuffleMask(rowCount, i);
        auto pPartialResult = CreateShuffleVector(pVector, pVector, shuffleMask);
        pPartialResult = CreateFMul(CreateExtractValue(pMatrix, i), pPartialResult);
        if (pResult != nullptr)
        {
            pResult = CreateFAdd(pResult, pPartialResult);
        }
        else
        {
            pResult = pPartialResult;
        }
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create matrix from matrix times matrix
Value* BuilderImplMatrix::CreateMatrixTimesMatrix(
    Value* const pMatrix1,             // [in] The float matrix 1
    Value* const pMatrix2,             // [in] The float matrix 2
    const Twine& instName)             // [in] Name to give instruction(s)
{
    Type* const pMat1ColumnType = pMatrix1->getType()->getArrayElementType();
    const uint32_t mat2ColCount = pMatrix2->getType()->getArrayNumElements();
    Type* const pResultTy = ArrayType::get(pMat1ColumnType, mat2ColCount);
    Value* pResult = UndefValue::get(pResultTy);

    for (uint32_t i = 0; i < mat2ColCount; ++i)
    {
        Value* pNewColumnVector = CreateMatrixTimesVector(pMatrix1, CreateExtractValue(pMatrix2, i));
        pResult = CreateInsertValue(pResult, pNewColumnVector, i);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create matrix from outer product of vector
Value* BuilderImplMatrix::CreateOuterProduct(
    Value* const pVector1,            // [in] The float vector 1
    Value* const pVector2,            // [in] The float vector 2
    const Twine& instName)            // [in] Name to give instruction(s)
{
    const uint32_t rowCount = pVector1->getType()->getVectorNumElements();
    const uint32_t colCount = pVector2->getType()->getVectorNumElements();
    Type* const pResultTy = ArrayType::get(pVector1->getType(), colCount);
    Value* pResult = UndefValue::get(pResultTy);

    for (uint32_t i = 0; i < colCount; ++i)
    {
        SmallVector<uint32_t, 4> shuffleIdx(rowCount, i);
        Value* pColumnVector = CreateFMul(pVector1, CreateShuffleVector(pVector2, pVector2, shuffleIdx));
        pResult = CreateInsertValue(pResult, pColumnVector, i);
    }

    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Create matrix determinant operation. Matrix must be square
Value* BuilderImplMatrix::CreateDeterminant(
    Value* const pMatrix,     // [in] Matrix
    const Twine& instName)    // [in] Name to give instruction(s)
{
    uint32_t order = pMatrix->getType()->getArrayNumElements();
    LLPC_ASSERT(pMatrix->getType()->getArrayElementType()->getVectorNumElements() == order);
    LLPC_ASSERT(order >= 2);

    // Extract matrix elements.
    SmallVector<Value*, 16> elements;
    for (uint32_t columnIdx = 0; columnIdx != order; ++columnIdx)
    {
        Value* pColumn = CreateExtractValue(pMatrix, columnIdx);
        for (uint32_t rowIdx = 0; rowIdx != order; ++rowIdx)
        {
            elements.push_back(CreateExtractElement(pColumn, rowIdx));
        }
    }

    Value* pResult = Determinant(elements, order);
    pResult->setName(instName);
    return pResult;
}

// =====================================================================================================================
// Helper function for determinant calculation
Value* BuilderImplMatrix::Determinant(
    ArrayRef<Value*>    elements,     // Elements of matrix (order*order of them)
    uint32_t            order)        // Order of matrix
{
    if (order == 1)
    {
        return elements[0];
    }

    if (order == 2)
    {
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
    SmallVector<Value*, 9> submatrix;
    submatrix.resize((order - 1) * (order - 1));
    Value* pResult = nullptr;
    for (uint32_t leadRowIdx = 0; leadRowIdx != order; ++leadRowIdx)
    {
        GetSubmatrix(elements, submatrix, order, leadRowIdx, 0);
        Value* pSubdeterminant = CreateFMul(elements[leadRowIdx], Determinant(submatrix, order - 1));
        if ((leadRowIdx & 1) != 0)
        {
            pResult = CreateFSub(pResult, pSubdeterminant);
        }
        else
        {
            if (pResult == nullptr)
            {
                pResult = pSubdeterminant;
            }
            else
            {
                pResult = CreateFAdd(pResult, pSubdeterminant);
            }
        }
    }
    return pResult;
}

// =====================================================================================================================
// Get submatrix by deleting specified row and column
void BuilderImplMatrix::GetSubmatrix(
    ArrayRef<Value*>        matrix,         // Input matrix (as linearized array of values, order*order of them)
    MutableArrayRef<Value*> submatrix,      // Output matrix (ditto, (order-1)*(order-1) of them)
    uint32_t                order,          // Order of input matrix
    uint32_t                rowToDelete,    // Row index to delete
    uint32_t                columnToDelete) // Column index to delete
{
    uint32_t inElementIdx = 0, outElementIdx = 0;
    for (uint32_t columnIdx = 0; columnIdx != order; ++columnIdx)
    {
        for (uint32_t rowIdx = 0; rowIdx != order; ++rowIdx)
        {
            if ((rowIdx != rowToDelete) && (columnIdx != columnToDelete))
            {
                submatrix[outElementIdx++] = matrix[inElementIdx];
            }
            ++inElementIdx;
        }
    }
}

// =====================================================================================================================
// Create matrix inverse operation. Matrix must be square. Result is undefined if the matrix
// is singular or poorly conditioned (nearly singular).
Value* BuilderImplMatrix::CreateMatrixInverse(
    Value* const pMatrix,     // [in] Matrix
    const Twine& instName)    // [in] Name to give instruction(s)
{
    uint32_t order = pMatrix->getType()->getArrayNumElements();
    LLPC_ASSERT(pMatrix->getType()->getArrayElementType()->getVectorNumElements() == order);
    LLPC_ASSERT(order >= 2);

    // Extract matrix elements.
    SmallVector<Value*, 16> elements;
    for (uint32_t columnIdx = 0; columnIdx != order; ++columnIdx)
    {
        Value* pColumn = CreateExtractValue(pMatrix, columnIdx);
        for (uint32_t rowIdx = 0; rowIdx != order; ++rowIdx)
        {
            elements.push_back(CreateExtractElement(pColumn, rowIdx));
        }
    }

    // [ x0   x1   x2 ]                   [ Adj(x0) Adj(x1) Adj(x2) ] T
    // [              ]                   [                         ]
    // [ y0   y1   y2 ]  = (1 / det(M)) * [ Adj(y0) Adj(y1) Adj(y2) ]
    // [              ]                   [                         ]
    // [ z0   z1   z2 ]                   [ Adj(z0) Adj(z1) Adj(z2) ]
    //
    // where Adj(a) is the cofactor of a, which is the determinant of the submatrix obtained by deleting
    // the row and column of a, then negated if row+col is odd.

    SmallVector<Value*, 16> resultElements;
    resultElements.resize(order * order);
    SmallVector<Value*, 9> submatrix;
    submatrix.resize((order - 1) * (order - 1));

    // Calculate reciprocal of determinant, and negated reciprocal of determinant.
    Value* pRcpDet = CreateFDiv(ConstantFP::get(elements[0]->getType(), 1.0), Determinant(elements, order));
    Value* pNegRcpDet = CreateFSub(Constant::getNullValue(elements[0]->getType()), pRcpDet);

    // For each element:
    for (uint32_t columnIdx = 0; columnIdx != order; ++columnIdx)
    {
        for (uint32_t rowIdx = 0; rowIdx != order; ++rowIdx)
        {
            // Calculate cofactor for this element.
            GetSubmatrix(elements, submatrix, order, rowIdx, columnIdx);
            // Calculate its determinant.
            Value* pCofactor = Determinant(submatrix, order - 1);
            // Divide by whole matrix determinant, and negate if row+col is odd.
            pCofactor = CreateFMul(pCofactor,
                                   (((rowIdx + columnIdx) & 1) != 0) ? pNegRcpDet : pRcpDet);
            // Transpose by placing the cofactor in the transpose position.
            resultElements[rowIdx * order + columnIdx] = pCofactor;
        }
    }

    // Create the result matrix.
    Value* pResult = UndefValue::get(pMatrix->getType());
    for (uint32_t columnIdx = 0; columnIdx != order; ++columnIdx)
    {
        Value* pColumn = UndefValue::get(pMatrix->getType()->getArrayElementType());
        for (uint32_t rowIdx = 0; rowIdx != order; ++rowIdx)
        {
            pColumn = CreateInsertElement(pColumn, resultElements[rowIdx + columnIdx * order], rowIdx);
        }
        pResult = CreateInsertValue(pResult, pColumn, columnIdx);
    }

    pResult->setName(instName);
    return pResult;
}

