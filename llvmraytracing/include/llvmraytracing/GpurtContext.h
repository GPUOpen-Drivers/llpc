/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  GpurtContext.h
 * @brief LLVMContext extension that stores a GPURT library module
 ***********************************************************************************************************************
 */

#pragma once

#include "llvm-dialects/Dialect/ContextExtension.h"
#include <memory>

namespace llvm {
class Module;
}

namespace lgc {

// This extension can be attached to an LLVMContext and queried via the
// GpurtContext::get method inherited from the base class.
//
// Compiler drivers (like LLPC) are expected to set theModule to the GPURT
// library, so that certain raytracing-related passes can cross-module inline
// functions implemented there.
class GpurtContext : public llvm_dialects::ContextExtensionImpl<GpurtContext> {
public:
  explicit GpurtContext(llvm::LLVMContext &) {}
  ~GpurtContext();

  static Key theKey;

  // Pointer to ownedTheModule if exists, or pointer to a module owned
  // externally (e.g., by driver)
  llvm::Module *theModule = nullptr;
  std::unique_ptr<llvm::Module> ownedTheModule;
};

} // namespace lgc
