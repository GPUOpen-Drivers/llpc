/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcInputUtils.h
 * @brief LLPC header file: input file handling for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <tuple>
#include <vector>

namespace Llpc {
namespace StandaloneCompiler {

// Represents a single input specification passed to the standalone compiler. This consists of a filename and,
// optionally, an entry point.
// The raw format is: `filename[,entry_point]`.
struct InputSpec {
  // Keep all chunks as `std::string` to simplify memory management.
  std::string rawInputSpec;
  std::string filename;
  std::string entryPoint;

  bool operator==(const InputSpec &rhs) const {
    return std::tie(rawInputSpec, filename, entryPoint) == std::tie(rhs.rawInputSpec, rhs.filename, rhs.entryPoint);
  }
  bool operator!=(const InputSpec &rhs) const { return !(*this == rhs); }
};

// Takes a raw input spec and attempts to parse it. Returns llvm::Error on failure.
llvm::Expected<InputSpec> parseInputFileSpec(llvm::StringRef inputSpec);

// Takes a list of raw input specs and attempts to parse them. Returns llvm::Error if any of parsing fail on any of the
// inputs.
llvm::Expected<llvm::SmallVector<InputSpec>> parseAndCollectInputFileSpecs(llvm::ArrayRef<std::string> inputSpecs);

using InputSpecGroup = llvm::SmallVector<InputSpec, 2>;
// Split the list of input file paths into groups. Each group will be compiled in its own context.
// Validates the input files and returns Error on failure.
llvm::Expected<llvm::SmallVector<InputSpecGroup, 0>> groupInputSpecs(llvm::ArrayRef<InputSpec> inputSpecs);

// Represents allowed extensions of LLPC source files.
namespace Ext {

constexpr llvm::StringLiteral SpirvBin = ".spv";
constexpr llvm::StringLiteral SpirvText = ".spvasm";
constexpr llvm::StringLiteral PipelineInfo = ".pipe";
constexpr llvm::StringLiteral LlvmBitcode = ".bc";
constexpr llvm::StringLiteral LlvmIr = ".ll";
constexpr llvm::StringLiteral IsaText = ".s";
constexpr llvm::StringLiteral IsaBin = ".elf";

constexpr llvm::StringLiteral GlslShaders[] = {".task", ".vert", ".tesc", ".tese",  ".geom",  ".mesh",  ".frag",
                                               ".comp", ".rgen", ".rint", ".rahit", ".rchit", ".rmiss", ".rcall"};

} // namespace Ext

// Returns true when the buffer is an ELF binary.
bool isElfBinary(const void *data, size_t dataSize);

// Returns true when the buffer is an LLVM bitcode binary.
bool isLlvmBitcode(const void *data, size_t dataSize);

// Returns true when the buffer is an ISA assembler text.
bool isIsaText(const void *data, size_t dataSize);

// Checks whether the specified file name represents a SPIR-V assembly text file (.spvasm).
bool isSpirvTextFile(llvm::StringRef fileName);

// Checks whether the specified file name represents a SPIR-V binary file (.spv).
bool isSpirvBinaryFile(llvm::StringRef fileName);

// Checks whether the specified file name represents a GLSL shader file (.vert, .frag, etc.).
bool isGlslShaderTextFile(llvm::StringRef fileName);

// Checks whether the specified file name represents an LLVM IR file (.ll).
bool isLlvmIrFile(llvm::StringRef fileName);

// Checks whether the specified file name represents an LLPC pipeline info file (.pipe).
bool isPipelineInfoFile(llvm::StringRef fileName);

// Tries to detect the format of binary data and creates a file extension from it.
llvm::StringLiteral fileExtFromBinary(BinaryData pipelineBin);

// Expands all input files in a platform-specific way.
LLPC_NODISCARD Result expandInputFilenames(llvm::ArrayRef<std::string> inputFiles,
                                           std::vector<std::string> &expandedFilenames);

// Reads SPIR-V binary code from the specified binary file.
llvm::Expected<BinaryData> getSpirvBinaryFromFile(llvm::StringRef spvBinFile);

// Write a binary into a file or to stdout. The file will be overwritten if it exists.
llvm::Error writeFile(BinaryData pipelineBin, llvm::StringRef fileName);

} // namespace StandaloneCompiler
} // namespace Llpc
