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
 * @file  ElementType.h
 * @brief contains the declaration of utility functions of Xdl Element types
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/LgcXdlTypes.h"

namespace llvm {
class Type;
class Value;
} // namespace llvm

namespace llvm_dialects {
class Builder;
} // namespace llvm_dialects

namespace lgc::xdl {
// Get the LGC type of a cooperative matrix with the given element type, layout and K size.
llvm::Type *getCooperativeMatrixTy(llvm_dialects::Builder &builder, CooperativeMatrixElementType elemType,
                                   CooperativeMatrixLayout layout, unsigned kSize = 16);

// Whether the type of a cooperative matrix is integer.
bool isUnderlyingIntegerCooperativeMatrix(CooperativeMatrixElementType elemType);

// Interpret the cooperative matrix's element as uint32.
llvm::Value *interpretCoopMatElementAsIntegerTy(llvm_dialects::Builder &builder, llvm::Value *value,
                                                CooperativeMatrixElementType elemType);

// Interpret the value as cooperative matrix's element type.
llvm::Value *interpretValueAsCoopMatElementTy(llvm_dialects::Builder &builder, llvm::Value *value,
                                              CooperativeMatrixElementType elemType);

// Whether the type of a cooperative matrix is specified bit width.
bool isTypeNCooperativeMatrix(CooperativeMatrixElementType elemType, unsigned bitWidth);

// Convert the element type enum into the corresponding LLVM type.
llvm::Type *transCooperativeMatrixElementType(llvm_dialects::Builder &builder, CooperativeMatrixElementType elemType);

} // namespace lgc::xdl
