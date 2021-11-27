/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
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

#include "llpcError.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <system_error>

using namespace llvm;
using Vkgc::Result;

namespace Llpc {
namespace {

using ::testing::StrEq;

// cppcheck-suppress syntaxError
TEST(ResultErrorTest, PlaceholderPass) {
  EXPECT_TRUE(true);
}

TEST(ResultErrorTest, ResultErrorCodeSuccess) {
  std::error_code err = resultToErrorCode(Result::Success);
  EXPECT_FALSE(static_cast<bool>(err)) << err;
  EXPECT_EQ(err.value(), 0);
  EXPECT_THAT(err.category().name(), StrEq("Vkgc::Result"));
  EXPECT_THAT(err.message(), StrEq("Success"));
}

TEST(ResultErrorTest, ResultErrorCodeFailure) {
  std::error_code err = resultToErrorCode(Result::ErrorInvalidShader);
  EXPECT_TRUE(static_cast<bool>(err)) << err;
  EXPECT_EQ(err.value(), static_cast<int>(Result::ErrorInvalidShader));
  EXPECT_THAT(err.category().name(), StrEq("Vkgc::Result"));
  EXPECT_THAT(err.message(), StrEq("ErrorInvalidShader"));
}

TEST(ResultErrorTest, ResultErrorEmptyMessage) {
  Error err = createResultError(Result::NotFound);
  ASSERT_TRUE(err.isA<ResultError>());
  std::string errorMessage;
  raw_string_ostream os(errorMessage);
  os << err;
  os.flush();
  EXPECT_THAT(errorMessage, StrEq("Result::NotFound"));

  auto remainingErrors = handleErrors(
      std::move(err), [](const ResultError &resultError) { EXPECT_EQ(resultError.getResult(), Result::NotFound); });
  EXPECT_THAT_ERROR(std::move(remainingErrors), Succeeded());
}

TEST(ResultErrorTest, ResultErrorCustomMessage) {
  Error err = createResultError(Result::ErrorUnavailable, "My message");
  ASSERT_TRUE(err.isA<ResultError>());
  EXPECT_THAT_ERROR(std::move(err), FailedWithMessage(StrEq("Result::ErrorUnavailable: My message")));
}

TEST(ResultErrorTest, ErrorToResultSuccess) {
  Error err = Error::success();
  EXPECT_EQ(errorToResult(std::move(err)), Result::Success);
}

TEST(ResultErrorTest, ErrorToResultFailure) {
  Error err = createResultError(Result::NotFound);
  EXPECT_EQ(errorToResult(std::move(err)), Result::NotFound);
}

Expected<int> mayFail(int value) {
  if (value == 0)
    return createResultError(Result::ErrorInvalidValue, "Zero passed");
  return value;
}

TEST(ResultErrorTest, ExpectedResult) {
  auto valueOrErr = mayFail(42);
  ASSERT_THAT_EXPECTED(valueOrErr, Succeeded());
  EXPECT_EQ(*valueOrErr, 42);

  valueOrErr = mayFail(0);
  EXPECT_THAT_EXPECTED(valueOrErr, FailedWithMessage(StrEq("Result::ErrorInvalidValue: Zero passed")));
}

} // namespace
} // namespace Llpc
