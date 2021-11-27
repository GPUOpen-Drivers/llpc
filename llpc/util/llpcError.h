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
 * @file  llpcError.h
 * @brief LLPC header file: contains the definition of LLPC error handling utilities
 ***********************************************************************************************************************
 */
#pragma once

#include "vkgcDefs.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include <cassert>

namespace Llpc {

// Handles passed `result` and checks if it is a Success. Prints `errorMessage` if not.
void mustSucceed(Vkgc::Result result, const llvm::Twine &errorMessage = {});

// Prints the error to LLPC_ERRS and consumes it.
void reportError(llvm::Error &&err);

// Converts a `Vkgc::Result` to `std::error_code` with a custom error category.
LLPC_NODISCARD std::error_code resultToErrorCode(Vkgc::Result result);

// A custom LLPC Error class that holds a `Vkgc::Result` and, optionally, and error message.
// ResultError work with the standard LLVM error handling utilities, including:
// *  `llvm::Expected<T>` -- holds either a value or an Error.
// *  `llvm::Error` -- typed-erased error class. You can create one with `createResultError(result, message)`.
// *  `llvm::handleErrors`, `llvm::cantFail`, `llvm::errorToBool`, and other error handling functions.
//
// See https://llvm.org/docs/ProgrammersManual.html#recoverable-errors for LLVM error handling details.
//
// Usage sample:
// ```c++
// Expected<size_t> getFileSize(StringRef path) {
//   if (exists(path))
//     return file_size(path);
//   return createResultError(Result::NotFound, Twine("File ") + path + " does not exist");
// }
//
// // An internal LLPC function.
// Expected<BinaryData> readSpirvFile(StringRef path) {
//   auto sizeOrErr = getFileSize(path);
//   if (Error err = sizeOrErr.takeError())
//     return std::move(err); // Let the caller decide how to handle this error.
//   BinaryData spv = {};
//   spv.size = *sizeOrErr;
//   ...
//   return spv;
// }
//
// // A public function exposed to XGL.
// Result getCacheSize(size_t *outSize) {
//   auto sizeOrErr = getFileSize(m_cacheFile);
//   if (Error err = sizeOrErr.takeError())
//     return errorToResult(std::move(err)); // We do not return LLVM errors to XGL.
//   *outSize = *sizeOrErr;
//   return Result::Success;
// }
// ```
class LLPC_NODISCARD ResultError : public llvm::ErrorInfo<ResultError> {
public:
  // Creates a new `ResultError` with a non-Success `Result` value and an optional error message.
  ResultError(Vkgc::Result result, const llvm::Twine &errorMessage = {})
      : m_message(errorMessage.str()), m_result(result) {
    assert(m_result != Vkgc::Result::Success && "Result::Success is not an error");
  }

  void log(llvm::raw_ostream &os) const override {
    os << "Result::" << convertToErrorCode().message();
    if (!m_message.empty())
      os << ": " << m_message;
  }

  LLPC_NODISCARD std::error_code convertToErrorCode() const override { return resultToErrorCode(m_result); }

  LLPC_NODISCARD const std::string &getMessage() const { return m_message; }
  LLPC_NODISCARD Vkgc::Result getResult() const { return m_result; }

  // Globally unique ID to register this Error type. Required by `llvm::ErrorInfo::classID`.
  static char ID; // NOLINT

private:
  std::string m_message;
  Vkgc::Result m_result = Vkgc::Result::ErrorUnknown;
};

// =====================================================================================================================
// Creates an `llvm::Error` containing a `ResultError`.
//
// @param result : Non-Success Result value
// @param errorMessage : Optional error message
// @returns : New `llvm::Error` containing a `ResultError(result, errorMessage)`
inline llvm::Error createResultError(Vkgc::Result result, const llvm::Twine &errorMessage = {}) {
  return llvm::make_error<ResultError>(result, errorMessage);
}

// Extracts the `Result` value from the given `ResultError`. Assumes that `err` is either a `ResultError` or
// `llvm::ErrorSuccess`.
LLPC_NODISCARD Vkgc::Result errorToResult(llvm::Error &&err);

} // namespace Llpc
