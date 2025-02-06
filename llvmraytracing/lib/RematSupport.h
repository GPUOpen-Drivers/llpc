/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

//===- RematSupport.h - Support functions for rematerialization -----------===//
//
// This file defines callbacks used during rematerialization by
// the Continuation State Builder.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "lgc/LgcRtDialect.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include <optional>

namespace rematsupport {
/// Returns true if a call to the given function should be rematerialized
/// in a shader of the specified kind.
///
/// If no shader kind is specified, return false.
bool isRematerializableLgcRtOp(llvm::CallInst &CInst,
                               std::optional<lgc::rt::RayTracingShaderStage> Kind = std::nullopt);

// Rematerializable callback specific to DXIL - mainly used to extend what's
// considered rematerializable for continuations
bool DXILMaterializable(llvm::Instruction &I);

// Rematerializable callback specific to LgcCps - mainly used to extend what's
// considered rematerializable for continuations
bool LgcMaterializable(llvm::Instruction &I);

} // namespace rematsupport
