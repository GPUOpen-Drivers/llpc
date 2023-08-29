/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *deal in the Software without restriction, including without limitation the
 *rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//===----------------------------------------------------------------------===//
//
// Unit tests for checking how many dwords are occupied by specific types
//
//===----------------------------------------------------------------------===//

#include "lgccps/LgcCpsDialect.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "gtest/gtest.h"

using namespace llvm;

// Add a small DSL to add tests.

// Define local context and DL per test.
#define DECLARE_LLVM_LOCALS(TestName)                                          \
  LLVMContext context_##TestName;                                              \
  DataLayout DL_##TestName(                                                    \
      "e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:"             \
      "32:32-p7:160:256:256:32-p8:128:128-i64:64-v16:16-v24:32-v32:"           \
      "32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-"               \
      "v2048:2048-n32:64-S32-A5-G1-ni:7:8");

#define DECLARE_LLVM_LOCALS_SIMPLE(ExpectedCount, TypeName)                    \
  DECLARE_LLVM_LOCALS(ExpectedCount##TypeName)

// Get the context based on a test name.
#define LLVM_CONTEXT(TestName) context_##TestName

// Get the DL based on a test name.
#define LLVM_DL(TestName) DL_##TestName

// Get a type based on a context.
#define GET_TYPE_INITIALIZER(TestName, TypeName)                               \
  Type::get##TypeName##Ty(LLVM_CONTEXT(TestName))

// Test the simple case where we are checking the size of a single type.
#define TEST_DWORD_COUNT(TypeName, ExpectedCount)                              \
  TEST(LgcCpsUnitTests, ExpectedCount##TypeName) {                             \
    DECLARE_LLVM_LOCALS_SIMPLE(ExpectedCount, TypeName)                        \
    unsigned dwordCount = lgc::cps::getArgumentDwordCount(                     \
        LLVM_DL(ExpectedCount##TypeName),                                      \
        GET_TYPE_INITIALIZER(ExpectedCount##TypeName, TypeName));              \
    EXPECT_EQ(dwordCount, ExpectedCount);                                      \
  }

// Test the case where we are checking the size of a vector of elements.
#define TEST_DWORD_COUNT_VECTOR(TestName, TypeName, NumElements,               \
                                ExpectedCount)                                 \
  TEST(LgcCpsUnitTests, TestName) {                                            \
    DECLARE_LLVM_LOCALS(TestName)                                              \
    unsigned dwordCount = lgc::cps::getArgumentDwordCount(                     \
        LLVM_DL(TestName),                                                     \
        FixedVectorType::get(GET_TYPE_INITIALIZER(TestName, TypeName),         \
                             NumElements));                                    \
    EXPECT_EQ(dwordCount, ExpectedCount);                                      \
  }

// Test the case where we are checking the size of struct of arbitrary elements.
#define TEST_DWORD_COUNT_STRUCT(TestName, ExpectedCount, ...)                  \
  TEST(LgcCpsUnitTests, TestName) {                                            \
    DECLARE_LLVM_LOCALS(TestName)                                              \
    unsigned dwordCount = lgc::cps::getArgumentDwordCount(                     \
        LLVM_DL(TestName), StructType::get(__VA_ARGS__));                      \
    EXPECT_EQ(dwordCount, ExpectedCount);                                      \
  }

// Test the case where we are checking a list of arbitrary elements.
#define TEST_DWORD_COUNT_LIST(TestName, ExpectedCount, ...)                    \
  TEST(LgcCpsUnitTests, TestName) {                                            \
    DECLARE_LLVM_LOCALS(TestName)                                              \
    unsigned dwordCount =                                                      \
        lgc::cps::getArgumentDwordCount(LLVM_DL(TestName), {__VA_ARGS__});     \
    EXPECT_EQ(dwordCount, ExpectedCount);                                      \
  }

TEST_DWORD_COUNT(Int1, 1)
TEST_DWORD_COUNT(Int16, 1)
TEST_DWORD_COUNT(Int32, 1)
TEST_DWORD_COUNT(Int64, 2)
TEST_DWORD_COUNT(Half, 1)
TEST_DWORD_COUNT(Float, 1)
TEST_DWORD_COUNT(Double, 2)
TEST_DWORD_COUNT(FP128, 4)
TEST_DWORD_COUNT_VECTOR(VecI64Test, Int64, 3, 6)
TEST_DWORD_COUNT_STRUCT(StructFPTest, 6,
                        Type::getDoubleTy(LLVM_CONTEXT(StructFPTest)),
                        Type::getFP128Ty(LLVM_CONTEXT(StructFPTest)))
TEST_DWORD_COUNT_STRUCT(
    StructPtrTest, 2,
    LLVM_DL(StructPtrTest).getIntPtrType(LLVM_CONTEXT(StructPtrTest), 32))
TEST_DWORD_COUNT_LIST(
    ListFloatStructTest, 6,
    ArrayType::get(Type::getFloatTy(LLVM_CONTEXT(ListFloatStructTest)), 4),
    StructType::get(Type::getInt32Ty(LLVM_CONTEXT(ListFloatStructTest)),
                    Type::getInt16Ty(LLVM_CONTEXT(ListFloatStructTest))))

#undef DECLARE_LLVM_LOCALS
#undef DECLARE_LLVM_LOCALS_SIMPLE
#undef LLVM_CONTEXT
#undef LLVM_DL
#undef GET_TYPE_INITIALIZER
#undef TEST_DWORD_COUNT
#undef TEST_DWORD_COUNT_VECTOR
#undef TEST_DWORD_COUNT_STRUCT
#undef TEST_DWORD_COUNT_LIST
