/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcInputUtils.cpp
 * @brief LLPC source file: contains the implementation of input handling utilities for standalone LLPC compilers.
 ***********************************************************************************************************************
 */
#ifdef WIN_OS
// NOTE: Disable Windows-defined min()/max() because we use STL-defined std::min()/std::max() in LLPC.
#define NOMINMAX
#include <io.h>
#endif

#include "llpcDebug.h"
#include "llpcInputUtils.h"
#include "vkgcElfReader.h"
#include <cassert>

using namespace llvm;
using namespace Vkgc;

namespace Llpc {
namespace StandaloneCompiler {

// =====================================================================================================================
// Checks whether the input data is actually an ELF binary.
//
// @param data : Input data to check
// @param dataSize : Size of the input data
// @returns : true if ELF binary
bool isElfBinary(const void *data, size_t dataSize) {
  bool isElfBin = false;
  if (dataSize >= sizeof(Elf64::FormatHeader)) {
    auto header = reinterpret_cast<const Elf64::FormatHeader *>(data);
    isElfBin = header->e_ident32[EI_MAG0] == ElfMagic;
  }
  return isElfBin;
}

// =====================================================================================================================
// Checks whether the input data is actually LLVM bitcode.
//
// @param data : Input data to check
// @param dataSize : Size of the input data
// @returns : true if LLVM bitcode
bool isLlvmBitcode(const void *data, size_t dataSize) {
  const unsigned char magic[] = {'B', 'C', 0xC0, 0xDE};
  return dataSize >= sizeof magic && memcmp(data, magic, sizeof magic) == 0;
}

// =====================================================================================================================
// Checks whether the output data is actually ISA assembler text.
//
// @param data : Input data to check
// @param dataSize : Size of the input data
// @returns : true if ISA text
bool isIsaText(const void *data, size_t dataSize) {
  // This is called by amdllpc to help distinguish between its three output types of ELF binary, LLVM IR assembler
  // and ISA assembler. Here we use the fact that ISA assembler is the only one that starts with a tab character.
  return dataSize != 0 && (reinterpret_cast<const char *>(data))[0] == '\t';
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPIR-V assembly text file (.spvasm).
//
// @param fileName : File path to check
// @returns : true when fileName is a SPIR-V text file
bool isSpirvTextFile(const std::string &fileName) {
  bool isSpirvText = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == Ext::SpirvText)
    isSpirvText = true;

  return isSpirvText;
}

// =====================================================================================================================
// Checks whether the specified file name represents a SPIR-V binary file (.spv).
//
// @param fileName : File name to check
// @returns : true when fileName is a SPIR-V binary file
bool isSpirvBinaryFile(const std::string &fileName) {
  bool isSpirvBin = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == Ext::SpirvBin)
    isSpirvBin = true;

  return isSpirvBin;
}

// =====================================================================================================================
// Checks whether the specified file name represents an LLPC pipeline info file (.pipe).
//
// @param fileName : File name to check
// @returns : true when `fileName` is a pipelien info file
bool isPipelineInfoFile(const std::string &fileName) {
  bool isPipelineInfo = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == Ext::PipelineInfo)
    isPipelineInfo = true;

  return isPipelineInfo;
}

// =====================================================================================================================
// Checks whether the specified file name represents an LLVM IR file (.ll).
//
// @param fileName : File name to check
// @returns : true when `fileName` is an LLVM IR file
bool isLlvmIrFile(const std::string &fileName) {
  bool isLlvmIr = false;

  size_t extPos = fileName.find_last_of(".");
  std::string extName;
  if (extPos != std::string::npos)
    extName = fileName.substr(extPos, fileName.size() - extPos);

  if (!extName.empty() && extName == Ext::LlvmIr)
    isLlvmIr = true;

  return isLlvmIr;
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
Result getSpirvBinaryFromFile(const std::string &spvBinFile, BinaryData &spvBin) {
  Result result = Result::Success;

  FILE *binFile = fopen(spvBinFile.c_str(), "rb");
  if (!binFile) {
    LLPC_ERRS("Fails to open SPIR-V binary file: " << spvBinFile << "\n");
    result = Result::ErrorUnavailable;
  }

  if (result == Result::Success) {
    fseek(binFile, 0, SEEK_END);
    size_t binSize = ftell(binFile);
    fseek(binFile, 0, SEEK_SET);

    char *bin = new char[binSize];
    assert(bin);
    memset(bin, 0, binSize);
    binSize = fread(bin, 1, binSize, binFile);

    spvBin.codeSize = binSize;
    spvBin.pCode = bin;

    fclose(binFile);
  }

  return result;
}

// =====================================================================================================================
// Write a binary into a file or to stdout. The file will be overwritten if it exists.
//
// @param pipelineBin : Data to be written
// @param fileName : Name of the file that should be written or "-" for stdout
// @returns : Result::Success on success, Result::ErrorUnavailable on failure
Result writeFile(BinaryData pipelineBin, StringRef fileName) {
  Result result = Result::Success;
  FILE *outFile = stdout;
  if (fileName != "-")
    outFile = fopen(fileName.str().c_str(), "wb");

  if (!outFile) {
    LLPC_ERRS("Failed to open output file: " << fileName << "\n");
    result = Result::ErrorUnavailable;
  }

  if (result == Result::Success) {
    if (fwrite(pipelineBin.pCode, 1, pipelineBin.codeSize, outFile) != pipelineBin.codeSize)
      result = Result::ErrorUnavailable;

    if (outFile != stdout && fclose(outFile) != 0)
      result = Result::ErrorUnavailable;

    if (result != Result::Success) {
      LLPC_ERRS("Failed to write output file: " << fileName << "\n");
    }
  }
  return result;
}

} // namespace StandaloneCompiler
} // namespace Llpc
