/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcShaderModuleHelper.h
 * @brief LLPC header file: contains the definition of LLPC utility class ShaderModuleHelper.
 ***********************************************************************************************************************
 */

#pragma once
#include "llpc.h"
#include <llvm/ADT/ArrayRef.h>
#include <vector>

namespace Llpc {

// Represents the information of one shader entry in ShaderModuleData
struct ShaderModuleEntry {
  unsigned entryNameHash[4]; // Hash code of entry name
  unsigned entryOffset;      // Byte offset of the entry data in the binCode of ShaderModuleData
  unsigned entrySize;        // Byte size of the entry data
  unsigned passIndex;        // Indices of passes, It is only for internal debug.
};

// Represents the name map <stage, name> of shader entry-point
struct ShaderEntryName {
  ShaderStage stage; // Shader stage
  const char *name;  // Entry name
};

// =====================================================================================================================
// Represents LLPC shader module helper class
class ShaderModuleHelper {
public:
  static ShaderModuleUsage getShaderModuleUsageInfo(const BinaryData *spvBinCode);

  static unsigned trimSpirvDebugInfo(const BinaryData *spvBin, llvm::MutableArrayRef<unsigned> codeBuffer);

  static Result optimizeSpirv(const BinaryData *spirvBinIn, BinaryData *spirvBinOut);

  static void cleanOptimizedSpirv(BinaryData *spirvBin);

  static unsigned getStageMaskFromSpirvBinary(const BinaryData *spvBin, const char *entryName);

  static Result verifySpirvBinary(const BinaryData *spvBin);

  static bool isLlvmBitcode(const BinaryData *shaderBin);
  static BinaryType getShaderBinaryType(BinaryData shaderBinary);
  static Result getModuleData(const ShaderModuleBuildInfo *shaderInfo, llvm::MutableArrayRef<unsigned> codeBuffer,
                              Vkgc::ShaderModuleData &moduleData);
  static unsigned getCodeSize(const ShaderModuleBuildInfo *shaderInfo);
  static BinaryData getShaderCode(const ShaderModuleBuildInfo *shaderInfo,
                                  llvm::MutableArrayRef<unsigned int> &codeBuffer);
};

} // namespace Llpc
