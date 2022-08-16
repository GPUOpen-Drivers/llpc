/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2022 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  Internal.h
 * @brief LLPC header file: contains LLPC internal-use definitions (including data types and utility functions).
 ***********************************************************************************************************************
 */
#pragma once

#include "lgc/CommonDefs.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Attributes.h"

namespace llvm {

class Argument;
class BasicBlock;
class CallInst;
class Function;
class Instruction;
class PassRegistry;
class Type;
class Value;

void initializeLegacyStartStopTimerPass(PassRegistry &);

} // namespace llvm

namespace lgc {

// Invalid value
static const unsigned InvalidValue = ~0u;

// Size of vec4
static const unsigned SizeOfVec4 = sizeof(float) * 4;

// Initialize helper passes
//
// @param passRegistry : Pass registry
inline static void initializeUtilPasses(llvm::PassRegistry &passRegistry) {
  initializeLegacyStartStopTimerPass(passRegistry);
}

// Emits a LLVM function call (inserted before the specified instruction), builds it automatically based on return type
// and its parameters.
llvm::CallInst *emitCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                         llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, llvm::Instruction *insertPos);

// Emits a LLVM function call (inserted at the end of the specified basic block), builds it automatically based on
// return type and its parameters.
llvm::CallInst *emitCall(llvm::StringRef funcName, llvm::Type *retTy, llvm::ArrayRef<llvm::Value *> args,
                         llvm::ArrayRef<llvm::Attribute::AttrKind> attribs, llvm::BasicBlock *insertAtEnd);

// Adds LLVM-style type mangling suffix for the specified return type and args to the name.
void addTypeMangling(llvm::Type *returnTy, llvm::ArrayRef<llvm::Value *> args, std::string &name);

// Gets LLVM-style name for type.
void getTypeName(llvm::Type *ty, llvm::raw_ostream &nameStream);
std::string getTypeName(llvm::Type *ty);

// Gets the argument from the specified function according to the argument index.
llvm::Argument *getFunctionArgument(llvm::Function *func, unsigned idx, const llvm::Twine &name = "");

// Checks if one type can be bitcasted to the other (type1 -> type2).
bool canBitCast(const llvm::Type *ty1, const llvm::Type *ty2);

// Checks if the specified value actually represents a don't-care value (0xFFFFFFFF).
bool isDontCareValue(llvm::Value *value);

// Given a non-aggregate type, get a float type at least as big that can be used to pass a value of that
// type in a return value struct, ensuring it gets into VGPRs.
llvm::Type *getVgprTy(llvm::Type *ty);

} // namespace lgc
