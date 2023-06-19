/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcSpirvLowerUtil.h
 * @brief LLPC header file: utilities for use by LLPC front-end
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"

namespace llvm {

class Function;
class Module;
class BasicBlock;
class StringRef;

} // namespace llvm

namespace Llpc {

// Well-known names in the front-end.
namespace LlpcName {

const static char GlobalProxyPrefix[] = "__llpc_global_proxy_";
const static char InputProxyPrefix[] = "__llpc_input_proxy_";
const static char OutputProxyPrefix[] = "__llpc_output_proxy_";

} // namespace LlpcName

// Gets the shader stage from the specified LLVM module.
ShaderStage getShaderStageFromModule(llvm::Module *module);

// Set the shader stage to the specified LLVM module.
void setShaderStageToModule(llvm::Module *module, ShaderStage shaderStage);

// Gets the entry point (valid for AMD GPU) of a LLVM module.
llvm::Function *getEntryPoint(llvm::Module *module);

// Clears the empty block
llvm::BasicBlock *clearBlock(llvm::Function *func);
// Clear non entry external functions
void clearNonEntryFunctions(llvm::Module *module, llvm::StringRef entryName);

} // namespace Llpc
