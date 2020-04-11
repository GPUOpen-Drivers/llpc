/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ShaderStage.h
 * @brief LLPC header file: declarations and utility functions for shader stage
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace lgc {

// Translates shader stage to corresponding stage mask.
static inline unsigned shaderStageToMask(ShaderStage stage) {
  return 1U << static_cast<unsigned>(stage);
}

// Set shader stage metadata on every defined function in a module
void setShaderStage(llvm::Module *module, ShaderStage stage);

// Set shader stage metadata on a function
void setShaderStage(llvm::Function *func, ShaderStage stage);

// Gets the shader stage from the specified LLVM function.
ShaderStage getShaderStage(const llvm::Function *func);

// Gets name string of the abbreviation for the specified shader stage
const char *getShaderStageAbbreviation(ShaderStage shaderStage);

} // namespace lgc
