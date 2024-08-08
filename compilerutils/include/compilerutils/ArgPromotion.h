/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// Promotion of pointer args to by-value.

#pragma once

namespace llvm {
class Function;
class SmallBitVector;
} // namespace llvm

namespace CompilerUtils {

// Replace struct return type with its first element type.
llvm::Function *unpackStructReturnType(llvm::Function *Fn);

// Turn StructRet argument into return type, modifying pointee type metadata as appropriate.
llvm::Function *lowerStructRetArgument(llvm::Function *Fn);

// Promote pointer (by-ref) arguments to by-value, according to PromotionMask
// and using pointee type metadata.
llvm::Function *promotePointerArguments(llvm::Function *Fn, const llvm::SmallBitVector &PromotionMask);

} // namespace CompilerUtils
