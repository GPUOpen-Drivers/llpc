/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  CodeGenManager.h
 * @brief LLPC header file: contains declaration of class lgc::CodeGenManager.
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace llvm {

namespace legacy {

class PassManager;

} // namespace legacy

class Timer;

} // namespace llvm

namespace lgc {

namespace Gfx6 {
struct PipelineVsFsRegConfig;
struct PipelineCsRegConfig;
} // namespace Gfx6

class PassManager;
class PipelineState;

// Represents data entry in a ELF section, including associated ELF symbols.
struct ElfDataEntry {
  const void *data;     // Data in the section
  unsigned offset;      // Offset of the data
  unsigned size;        // Size of the data
  unsigned padSize;     // Padding size of the data
  const char *pSymName; // Name of associated ELF symbol
};

// =====================================================================================================================
// Represents the manager of GPU ISA code generation.
class CodeGenManager {
public:
  static void setupTargetFeatures(PipelineState *pipelineState, llvm::Module *module);

private:
  CodeGenManager() = delete;
  CodeGenManager(const CodeGenManager &) = delete;
  CodeGenManager &operator=(const CodeGenManager &) = delete;
};

} // namespace lgc
