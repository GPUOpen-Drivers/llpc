/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All Rights Reserved.
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

#pragma once

#include "llpc/GpurtEnums.h"
#include "llvm-dialects/Dialect/Builder.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"
#include <optional>

#define GET_INCLUDES
#define GET_DIALECT_DECLS
#include "LgcCpsDialect.h.inc"

namespace llvm {
class AllocaInst;
class DataLayout;
class Function;
class Type;
class Value;
} // namespace llvm

namespace lgc::rt {
enum class RayTracingShaderStage;
} // namespace lgc::rt

namespace lgc::cps {

constexpr unsigned stackAddrSpace = 32;

// The maximum amount of dwords usable for passing arguments
constexpr unsigned MaxArgumentDwords = 32;

// The maximum allowed number of payload VGPRs to be used by RT lowering. Sizes
// beyond this value should be spilled to memory.
// TODO: Properly choose a value here, such that the total VGPR number is just
// below an allocation boundary.
constexpr unsigned CpsPayloadMaxNumVgprs = MaxArgumentDwords;

unsigned getArgumentDwordCount(const llvm::DataLayout &DL, llvm::Type *type);
unsigned getArgumentDwordCount(const llvm::DataLayout &DL, llvm::ArrayRef<llvm::Type *> types);
std::optional<unsigned> getRemainingArgumentDwords(const llvm::DataLayout &DL, llvm::ArrayRef<llvm::Type *> arguments);

std::optional<unsigned> getMaxArgumentVgprs(const llvm::Module &m);
void setMaxArgumentVgprs(llvm::Module &m, unsigned bound);

bool isCpsFunction(const llvm::Function &fn);
void setCpsFunctionLevel(llvm::Function &fn, CpsSchedulingLevel level);
CpsSchedulingLevel getCpsLevelFromFunction(const llvm::Function &fn);
CpsSchedulingLevel getCpsLevelForShaderStage(lgc::rt::RayTracingShaderStage stage);
uint8_t getPotentialCpsReturnLevels(lgc::rt::RayTracingShaderStage stage);
llvm::Value *lowerAsContinuationReference(llvm::IRBuilder<> &Builder, lgc::cps::AsContinuationReferenceOp &AsCROp,
                                          llvm::Value *Relocation = nullptr);
} // namespace lgc::cps
