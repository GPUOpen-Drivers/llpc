/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcInputUtils.cpp
 * @brief LLPC source file: contains the implementation of input handling utilities for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#include <io.h>
#include <windows.h>
#endif

#include "llpcDebug.h"
#include "llpcFile.h"
#include "llpcInputUtils.h"
#include "vkgcElfReader.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include <cassert>
#include <system_error>

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Split the list of input file paths into groups. Each group will be compiled in its own context.
// Validates the input files and returns Error on failure.
//
// @param inputFiles : Input files to group and validate
// @returns : List of input groups on success, Error on failure
Expected<SmallVector<InputFilesGroup>> groupInputFiles(ArrayRef<std::string> inputFiles) {
  const size_t numInputs = inputFiles.size();
  const size_t numPipe = count_if(inputFiles, isPipelineInfoFile);
  if (numPipe > 0 && numPipe != numInputs)
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "Mixing .pipe and shader inputs is not allowed");

  // Check that all files exist and are accessible.
  for (StringRef filePath : inputFiles) {
    const char *errorMessage = nullptr;
    if (!sys::fs::exists(filePath))
      errorMessage = "Input file does not exist";
    else if (!sys::fs::is_regular_file(filePath))
      errorMessage = "Input path is not a regular file";

    if (errorMessage)
      return createStringError(std::make_error_code(std::errc::file_exists), Twine(errorMessage) + ": " + filePath);
  }

  // All input shaders form one group.
  SmallVector<InputFilesGroup> groups;
  if (numInputs == 0)
    return groups;

  if (numPipe == 0) {
    groups.push_back({inputFiles.begin(), inputFiles.end()});
    return groups;
  }

  // Each .pipe file forms its own group.
  groups.reserve(numInputs);
  for (const std::string &file : inputFiles)
    groups.push_back({file});
  return groups;
}

// =====================================================================================================================
// Checks whether the input data is actually an ELF binary.
//
// @param data : Input data to check
// @param dataSize : Size of the input data
// @returns : true if ELF binary
bool isElfBinary(const void *data, size_t dataSize) {
  assert(data);
  if (dataSize < sizeof(Elf64::FormatHeader))
    return false;

  auto header = reinterpret_cast<const Elf64::FormatHeader *>(data);
  return header->e_ident32[EI_MAG0] == ElfMagic;
}

// =====================================================================================================================
// Checks whether the input data is actually LLVM bitcode.
//
// @param data : Input data to check
// @param dataSize : Size of the input data
// @returns : true if LLVM bitcode
bool isLlvmBitcode(const void *data, size_t dataSize) {
  assert(data);
  const unsigned char magic[] = {'B', 'C', 0xC0, 0xDE};
  if (dataSize < sizeof(magic))
    return false;

  return memcmp(data, magic, sizeof(magic)) == 0;
}

// =====================================================================================================================
// Checks whether the output data is actually ISA assembler text.
//
// @param data : Input data to check
// @param dataSize : Size of the input data
// @returns : true if ISA text
bool isIsaText(const void *data, size_t dataSize) {
  assert(data);
  // This is called by LLPC standalone compilers to help distinguish between its three output types of ELF binary, LLVM
  // IR assembler and ISA assembler. Here we use the fact that ISA assembler is the only one that starts with a tab
  // character.
  return dataSize != 0 && (reinterpret_cast<const char *>(data))[0] == '\t';
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPIR-V assembly text file (.spvasm).
//
// @param fileName : File path to check
// @returns : true when fileName is a SPIR-V text file
bool isSpirvTextFile(StringRef fileName) {
  return fileName.endswith(Ext::SpirvText);
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPIR-V binary file (.spv).
//
// @param fileName : File path to check
// @returns : true when fileName is a SPIR-V binary file
bool isSpirvBinaryFile(StringRef fileName) {
  return fileName.endswith(Ext::SpirvBin);
}

// =====================================================================================================================
// Checks whether the specified file name represents a GLSL shader file (.vert, .frag, etc.).
//
// @param fileName : File path to check
// @returns : true when fileName is an LLVM IR file
bool isGlslShaderTextFile(llvm::StringRef fileName) {
  return any_of(Ext::GlslShaders, [fileName](StringLiteral extension) { return fileName.endswith(extension); });
}

// =====================================================================================================================
// Checks whether the specified file name represents an LLVM IR file (.ll).
//
// @param fileName : File path to check
// @returns : true when fileName is an LLVM IR file
bool isLlvmIrFile(StringRef fileName) {
  return fileName.endswith(Ext::LlvmIr);
}

// =====================================================================================================================
// Checks whether the specified file name represents an LLPC pipeline info file (.pipe).
//
// @param fileName : File path to check
// @returns : true when `fileName` is a pipelien info file
bool isPipelineInfoFile(StringRef fileName) {
  return fileName.endswith(Ext::PipelineInfo);
}

// =====================================================================================================================
// Tries to detect the format of binary data and creates a file extension from it.
//
// @param pipelineBin : Data that should be analyzed
// @returns : The extension of the contained data, e.g. ".elf" or ".s"
StringLiteral fileExtFromBinary(BinaryData pipelineBin) {
  if (isElfBinary(pipelineBin.pCode, pipelineBin.codeSize))
    return Ext::IsaBin;
  if (isLlvmBitcode(pipelineBin.pCode, pipelineBin.codeSize))
    return Ext::LlvmBitcode;
  if (isIsaText(pipelineBin.pCode, pipelineBin.codeSize))
    return Ext::IsaText;

  return Ext::LlvmIr;
}

#ifdef WIN_OS
// =====================================================================================================================
// Finds all filenames which can match input file name
//
// @param       inFil    : Input file name, including a wildcard.
// @param [out] outFiles :  Output vector with matching filenames.
static void findAllMatchFiles(const std::string &inFile, std::vector<std::string> *outFiles) {
  WIN32_FIND_DATAA data = {};

  // Separate folder name.
  std::string folderName;
  auto separatorPos = inFile.find_last_of("/\\");
  if (separatorPos != std::string::npos)
    folderName = inFile.substr(0, separatorPos + 1);

  // Search first file.
  HANDLE searchHandle = FindFirstFileA(inFile.c_str(), &data);
  if (searchHandle == INVALID_HANDLE_VALUE)
    return;

  // Copy first file's name.
  outFiles->push_back(folderName + data.cFileName);

  // Copy other file names.
  while (FindNextFileA(searchHandle, &data))
    outFiles->push_back(folderName + data.cFileName);

  FindClose(searchHandle);
}
#endif

// =====================================================================================================================
// Expands all input files in a platform-specific way.
//
// @param inputFiles : Input paths.
// @param [out] expandedFilenames : Returned expanded input filenames.
// @returns : Result::Success on success, Result::ErrorInvalidValue when expansion fails.
Result expandInputFilenames(ArrayRef<std::string> inputFiles, std::vector<std::string> &expandedFilenames) {
  unsigned i = 0;
  for (const auto &inFile : inputFiles) {
#ifdef WIN_OS
    {
      if (i > 0 && inFile.find_last_of("*?") != std::string::npos) {
        LLPC_ERRS("\nCan't use wilecards with multiple inputs files\n");
        return Result::ErrorInvalidValue;
      }
      size_t initialSize = expandedFilenames.size();
      findAllMatchFiles(inFile, &expandedFilenames);
      if (expandedFilenames.size() == initialSize) {
        LLPC_ERRS("\nNo matching files found\n");
        return Result::ErrorInvalidValue;
      }
    }
#else // WIN_OS
    expandedFilenames.push_back(inFile);
#endif
    ++i;
  }
  return Result::Success;
}

// =====================================================================================================================
// Reads SPIR-V binary code from the specified binary file.
//
// @param spvBinFile : Path to a SPIR-V binary file
// @param [out] spvBin : SPIR-V binary code
// @returns : Result::Success on success, Result::ErrorUnavailable when the input file cannot be accessed.
Result getSpirvBinaryFromFile(StringRef spvBinFile, BinaryData &spvBin) {
  File file;
  Result openRes = file.open(spvBinFile.str().c_str(), FileAccessRead | FileAccessBinary);
  if (openRes != Result::Success)
    return openRes;

  const size_t fileSize = File::getFileSize(spvBinFile.str().c_str());
  char *bin = new char[fileSize]();
  size_t bytesRead = 0;
  Result readRes = file.read(bin, fileSize, &bytesRead);
  if (readRes != Result::Success) {
    delete[] bin;
    return readRes;
  }
  spvBin.codeSize = bytesRead;
  spvBin.pCode = bin;

  return Result::Success;
}

// =====================================================================================================================
// Write a binary into a file or to stdout. The file will be overwritten if it exists.
//
// @param pipelineBin : Data to be written
// @param fileName : Name of the file that should be written or "-" for stdout
// @returns : Result::Success on success, Result::ErrorUnavailable on failure
Result writeFile(BinaryData pipelineBin, StringRef fileName) {
  FILE *outFile = stdout;
  if (fileName != "-")
    outFile = fopen(fileName.str().c_str(), "wb");

  if (!outFile) {
    LLPC_ERRS("Failed to open output file: " << fileName << "\n");
    return Result::ErrorUnavailable;
  }

  auto result = Result::Success;
  if (fwrite(pipelineBin.pCode, 1, pipelineBin.codeSize, outFile) != pipelineBin.codeSize)
    result = Result::ErrorUnavailable;

  if (outFile != stdout && fclose(outFile) != 0)
    result = Result::ErrorUnavailable;

  if (result != Result::Success)
    LLPC_ERRS("Failed to write output file: " << fileName << "\n");

  return result;
}

} // namespace StandaloneCompiler
} // namespace Llpc
