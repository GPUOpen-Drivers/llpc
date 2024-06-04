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
 * @file  ShaderStage.h
 * @brief LLPC header file: declarations and utility functions for shader stage
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Function;
class GlobalObject;
class Module;
class Type;
} // namespace llvm

namespace lgc {

// Set shader stage metadata on every defined function in a module
void setShaderStage(llvm::Module *module, std::optional<ShaderStageEnum> stage);

// Set shader stage metadata on a function.
// This can instead be a GlobalVariable; that functionality is not used by LGC,
// but can be used by a front-end that uses a GlobalVariable to represent a part-pipeline retrieved
// from the cache, and wants to mark it with a shader stage
void setShaderStage(llvm::GlobalObject *func, std::optional<ShaderStageEnum> stage);

// Gets the shader stage from the specified LLVM function.
// This can instead be a GlobalVariable; that functionality is not used by LGC,
// but can be used by a front-end that uses a GlobalVariable to represent a part-pipeline retrieved
// from the cache, and wants to mark it with a shader stage
std::optional<ShaderStageEnum> getShaderStage(const llvm::GlobalObject *func);

// Set a function's shader subtype. Only has an effect on a compute shader or non-shader export function,
// where it causes the .shader_subtype PAL metadata item to be set to the arbitrary string given here.
void setShaderSubtype(llvm::GlobalObject *func, llvm::StringRef subtype);

// Get a function's shader subtype, or "" if none.
llvm::StringRef getShaderSubtype(llvm::GlobalObject *func);

// Determine whether the function is a shader entry-point.
bool isShaderEntryPoint(const llvm::Function *func);

// Gets name string of the abbreviation for the specified shader stage
const char *getShaderStageAbbreviation(ShaderStageEnum shaderStage);

enum AddFunctionArgsFlag : unsigned {
  AddFunctionArgsAppend = 0x1,
  AddFunctionArgsMaybeUndef = 0x2,
};

// Add args to a function. This creates a new function with the added args, then moves everything from the old function
// across to it.
// If this changes the return type, then all the return instructions will be invalid.
// This does not erase the old function, as the caller needs to do something with its uses (if any).
//
// @param oldFunc : Original function
// @param retTy : New return type, nullptr to use the same as in the original function
// @param argTys : Types of new args
// @param inRegMask : Bitmask of which args should be marked "inreg", to be passed in SGPRs
// @param append : Append new arguments if true, prepend new arguments if false
// @returns : The new function
llvm::Function *addFunctionArgs(llvm::Function *oldFunc, llvm::Type *retTy, llvm::ArrayRef<llvm::Type *> argTys,
                                llvm::ArrayRef<std::string> argNames, uint64_t inRegMask = 0, unsigned flags = 0);

// Get the ABI-mandated entry-point name for a shader stage
//
// @param callingConv : Which hardware shader stage
// @param isFetchlessVs : Whether it is (or contains) a fetchless vertex shader
// @returns : The entry-point name, or "" if callingConv not recognized
llvm::StringRef getEntryPointName(unsigned callingConv, bool isFetchlessVs);

} // namespace lgc
