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
/**
 ***********************************************************************************************************************
 * @file  ValueSpecialization.h
 * @brief Helpers for changing the dword-wise representation of a value.
 *
 * @details
 * Utility to replace dwords in the byte-wise representation of generic values by known constants or frozen poison.
 *
 * This is equivalent to storing a value to an alloca, then replacing some dwords, and then reading the value
 * back, but does so without introducing an alloca, and instead directly working on the SSA value using
 * {insert,extract}{value,element} instructions, and bit-wise ops for 64-bit values.
 *
 * Replacements are not guaranteed to succeed in the general case. Unsupported cases include:
 *  * dwords covering scalars smaller than a dword (e.g. i16)
 *  * dwords covering non-dword-aligned scalars
 *
 * Thus, this helper is intended for cases where we do not rely on the replacement for functional correctness,
 * but instead apply it as an optimization, e.g. for constant propagation, and prefer to do that
 * without introducing an alloca. This application motivates the name: The value is specialized for known
 * constant contents when used in a particular context.
 *
 * If needed, the mechanism could be extended to allow replacement of dwords by dynamic values.
 *
 ***********************************************************************************************************************
 */

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/IRBuilder.h>

namespace llvm {
class DataLayout;
class Value;
class Module;
class StringRef;
} // namespace llvm

namespace compilerutils {

class ValueSpecializer {
public:
  enum class SpecializationKind {
    None = 0,     // Keep the dword in the value as-is.
    Constant,     // Replace by a constant.
    FrozenPoison, // Replace by a frozen poison value. We specialize with frozen poisons to prevent propagation
                  // of poison into the containing value. For instance, ORing a zext'ed non-frozen i32 poison into an
                  // i64 poisons the whole i64.
    Count
  };

  struct DwordSpecializationInfo {
    SpecializationKind Kind = SpecializationKind::None;
    uint32_t ConstantValue = 0;
  };

  // An instance of this class can be re-used for multiple replacements on multiple values.
  // This allows to re-use the builder insertion point, which can lead to nicer (e.g. for tests) IR.
  ValueSpecializer(llvm::Module &M);

  // The IR builder stores a reference to us, so forbid copy and move.
  ValueSpecializer(const ValueSpecializer &) = delete;
  ValueSpecializer(ValueSpecializer &&) = delete;
  ValueSpecializer &operator=(const ValueSpecializer &) = delete;
  ValueSpecializer &operator=(ValueSpecializer &&) = delete;

  // Replaces dwords in Val according to DwordInfos, and returns the result.
  // Returns nullptr on failure, of if nothing was changed.
  //
  // Val needs to be an instruction or an argument (so we have a function to put new instructions in).
  // For arguments, new instructions for specialization are added to the function entry block.
  // For instructions, new instructions are added immediately after the specialized instruction.
  //
  // If ReplaceUses is set, then all uses of Val are replaced with the result, excluding new instructions that
  // are added as part of the replacement.
  //
  // If PreserveExistingBuilderInsertionPoint is set, and this is not the first call of this function,
  // we preserve the builder insertion point. In that case, it is the caller's responsibility to ensure that
  // the definition of Val dominates the current insertion point.
  // If the insertion point is reset, it is set to immediately after the replaced instruction, or after the last
  // alloca instruction in the function's entry block for arguments.
  // During the replacement, we do not change the insertion point, and just add instructions.
  // Thus, it is e.g. safe to preserve the insertion point when only specializing function arguments.
  //
  // Replacement values of the same type as Val reuse Val's name, plus NameSuffix.
  // Temporaries of nested types are not given names.
  struct ReplacementResult {
    llvm::Value *Replacement; // nullptr if no replacement was done
    unsigned NumReplacedDwords;
  };
  ReplacementResult replaceDwords(llvm::Value *Val, llvm::ArrayRef<DwordSpecializationInfo> DwordInfos,
                                  bool ReplaceUses, bool PreservePreviousInsertionPoint,
                                  llvm::StringRef NameSuffix = ".specialized");

private:
  // We use a callback to keep track of new instructions, which need to be skipped in the final RAUW.
  llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderCallbackInserter> B;
  const llvm::DataLayout &DL;
  llvm::Type *I32 = nullptr;
  llvm::Type *I64 = nullptr;
  bool IsFirstCall = true;

  // Per-run data:
  unsigned NumReplacedDwords = 0;
  llvm::SmallDenseSet<llvm::Value *> NewInsts;

  llvm::Value *getFrozenPoison(llvm::Type *Ty) { return B.CreateFreeze(llvm::PoisonValue::get(Ty)); }
  llvm::Value *getI32Constant(uint32_t ConstantValue) { return llvm::ConstantInt::get(I32, ConstantValue); }
  llvm::Value *getI64Constant(uint64_t ConstantValue) { return llvm::ConstantInt::get(I64, ConstantValue); }

  // Replace dwords in Val according to DwordInfos, and return the result.
  // Val may be nullptr if all dwords in DwordInfos are specialized,
  // meaning the result does not depend on the initial value.
  llvm::Value *replaceDwordsInNonAggregate(llvm::Type *Ty, llvm::Value *Val,
                                           llvm::ArrayRef<DwordSpecializationInfo> DwordInfos,
                                           llvm::StringRef ReplacementName);

  // Replaces dwords in RootVal according to DwordInfos. Handles both aggregate as well as non-aggregate types.
  // Returns the modified value, and nullptr upon failure, or if nothing was changed.
  //
  //  * RootVal: The value we want to replace dwords to constants in.
  //  * Indices: If RootVal is an aggregate, these indices point to a nested value in RootVal
  //             that this recursive function call should handle. In that case,
  //             CurTy and DwordInfos refer to that nested value.
  //  * CurTy: Type of the (possibly nested) value within RootVal to change.
  //  * DwordInfos: Dword-wise infos on what to change.
  //
  // For aggregate types, it recurses into each element, using the same root value,
  // populating Indices and CurTy, and restricting DwordInfos to the sub-range according to the element.
  // Once we reach a non-aggregate type, we extractvalue that element, apply the non-aggregate replacement,
  // and insertvalue the result.
  // In case the whole element is replaced, we skip the extractvalue and start with a frozen poison value instead if
  // necessary.
  //
  // The goal is to emit insertvalue instructions that directly insert into the leaf level,
  // instead of first extracting a nested (possibly aggregate!) value, then extracting nested values,
  // then specializing the nested value, inserting the nested value into the element value, and then
  // inserting the element value into the struct.
  // For example, when specializing dword 1 to 17 in { { i32, i32 }, i32 } %foo, we want to emit
  //    %foo.specialized = insertvalue { { i32, i32 }, i32 } %foo, i32 17, 0, 1
  // instead of the naive
  //    %nested = extractvalue { { i32, i32 }, i32 } %foo, 0
  //    %nested.specialized = insertvalue { i32, i32 } %nested, i32 17, 1
  //    %foo.specialized = insertvalue { { i32, i32 }, i32 } %foo, { i32, i32 } %nested.specialized, 0
  //
  // For non-aggregates, this is just a wrapper around replaceDwordsInNonAggregate.
  llvm::Value *replaceDwordsImpl(llvm::Value *RootVal, llvm::SmallVectorImpl<unsigned> &Indices, llvm::Type *CurTy,
                                 llvm::ArrayRef<DwordSpecializationInfo> DwordInfos, llvm::StringRef ReplacementName);
};

} // namespace compilerutils
