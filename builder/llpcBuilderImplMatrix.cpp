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
// Recorder implementations of MatrixBuilder methods
Value* BuilderImplMatrix::CreateMatrixTranspose(
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

    return pNewMatrix;
}
