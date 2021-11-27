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
/**
 ***********************************************************************************************************************
 * @file  llpcError.cpp
 * @brief LLPC source file: contains implementation of LLPC error handling utilities
 ***********************************************************************************************************************
 */
#include "llpcError.h"
#include "llpc.h"
#include "llpcDebug.h"
#include <cassert>
#include <system_error>

using namespace llvm;

namespace Llpc {

// =====================================================================================================================
// Handles passed `result` and checks if it is a Success. Prints `errorMessage` if not.
//
// @param result : Result to check
// @param errorMessage : Optional error message to print on failure
void mustSucceed(Result result, const Twine &errorMessage) {
  if (result == Result::Success)
    return;

  if (errorMessage.isTriviallyEmpty())
    LLPC_ERRS("Unhandled error result\n");
  else
    LLPC_ERRS("Unhandled error result: " << errorMessage << "\n");

  assert(false && "Result is not Success");
}

// =====================================================================================================================
// Prints the error message in `err` to LLPC_ERRS and consumes the error.
//
// @param err : The error to handle
void reportError(Error &&err) {
  // For details on llvm error handling, see https://llvm.org/docs/ProgrammersManual.html#recoverable-errors.
  handleAllErrors(std::move(err), [](const ErrorInfoBase &baseError) { LLPC_ERRS(baseError.message() << "\n"); });
}

char ResultError::ID = 0;

namespace {
// Make `Vkgc::Result` compatible with `std::error_category`. Because `Result` does not clearly map to the `std::errc`
// codes, we create a custom error category. We need this because `llvm::Error` expects custom error values to be
// convertible to `std::error_code`.
// See https://en.cppreference.com/w/cpp/error/error_category for C++ error-handling details.
struct ResultErrorCategory : std::error_category {
  const char *name() const noexcept override { return "Vkgc::Result"; }

  std::string message(int errorValue) const override {
    const Result result = static_cast<Result>(errorValue);
    switch (result) {
    case Result::ErrorInvalidPointer:
      return "ErrorInvalidPointer";
    case Result::ErrorInvalidShader:
      return "ErrorInvalidShader";
    case Result::ErrorInvalidValue:
      return "ErrorInvalidValue";
    case Result::ErrorOutOfMemory:
      return "ErrorOutOfMemory";
    case Result::ErrorUnavailable:
      return "ErrorUnavailable";
    case Result::ErrorUnknown:
      return "ErrorUnknown";
    case Result::Delayed:
      return "Delayed";
    case Result::NotFound:
      return "NotFound";
    case Result::NotReady:
      return "NotReady";
    case Result::Unsupported:
      return "Unsupported";
    case Result::Success:
      return "Success";
    default:
      llvm_unreachable("Invalid Result code");
      return "Invalid Result code";
    }
  }
};
} // namespace

// =====================================================================================================================
// Converts a `Result` to a `std::error_code` with a custom error category.
//
// @param result : The `Result` value to convert
// @returns : A new `std::error_code` with the `ResultErrorCategory` error category.
std::error_code resultToErrorCode(Result result) {
  static ResultErrorCategory GlobalCategoryId{};
  return {static_cast<int>(result), GlobalCategoryId};
}

// =====================================================================================================================
// Extracts the `Result` value from the given `ResultError`.
//
// @param err : The error to handle. This must be either a `ResultError` or `llvm::ErrorSuccess`.
// @returns : `Result::Success` on `llvm::ErrorSuccess` or the underlying `Result` value in case of `ResultError`.
Result errorToResult(Error &&err) {
  if (!err)
    return Result::Success;

  assert(err.isA<ResultError>() && "Not a ResultError");
  Result result = Result::ErrorUnknown;
  handleAllErrors(std::move(err), [&result](const ResultError &err) { result = err.getResult(); });
  return result;
}

} // namespace Llpc
