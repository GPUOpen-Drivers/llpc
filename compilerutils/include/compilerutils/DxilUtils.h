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

//===- DxilUtils.h -  --------------------------------------------------------------------------------------------===//
//
// Shared DXIl-related helpers.
//
//===--------------------------------------------------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/StringRef.h"

namespace compilerutils::dxil {

// Try to demangle function names in the DXIL format:
// ...\01?FuncName@@...
// @param funcName : Name of the callee
// @returns: the original string if the name was not demangleable or the demangled function name.
inline llvm::StringRef tryDemangleFunctionName(llvm::StringRef inputName) {
  assert(!inputName.empty());

  constexpr static llvm::StringRef manglingPrefix = "\01?";

  // Expect both characters to be there, and `\01?` to occur before `@@`
  size_t start = inputName.find(manglingPrefix);
  if (start == llvm::StringRef::npos)
    return inputName;

  // The case start >= end is implicitly checked by the second call to `find`.
  const size_t end = inputName.find("@@", start);
  if (end == llvm::StringRef::npos)
    return inputName;

  start += manglingPrefix.size();

  // Extract unmangled name: Return everything after the first occurrence of `\01?` and before the first occurrence of
  // `@@` after `?`.
  return inputName.substr(start, end - start);
}

} // namespace compilerutils::dxil
