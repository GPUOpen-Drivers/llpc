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
 * @file  TypeLowering.h
 * @brief Helpers for substituting values of one LLVM type with values of another (and possibly multiple) LLVM types.
 *
 * @details
 * Some lowerings need to replace values of one type by one or more values of another type. An example is the lowering
 * of fat buffer pointers from a `ptr addrspace(7)` to a `<4 x i32>` and a `ptr addrspace(3)`, but we anticipate more
 * such examples as we start using dialect types.
 *
 * The helpers provided here handle generic tasks such as:
 *
 *  * Handling `phi`, `alloca`, and `select`
 *  * Replacing values in function arguments
 *  * Converting aggregate types (structs and arrays) that contain converted types, and adjusting `extractvalue`,
 *    `insertvalue`, `extractelement`, and `insertelement`.
 *
 * Note: Vectors are explicitly *not* handled. Vectors can only contain basic scalar types, and it's unclear why one
 *       would want to convert basic scalar types. (Presumably one would do this if one were to implement backend-style
 *       legalization in LLVM IR, but that's clearly out of scope here.)
 *
 * TODO: While there is already some support for aggregate types, it notably does *not* handle GEPs. The fundamental
 * reason is that GEP is an untyped operation, and so it is fundamentally an error to create a GEP with a type that
 * will later be converted. If pointers to aggregates that will be converted are desired, we'll need some kind of
 * explicitly structural GEP.
 *
 ***********************************************************************************************************************
 */

#pragma once

#include "llvm-dialects/Dialect/Visitor.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"

class TypeLowering;

/// Given a type, check if it should be replaced.
///
/// Return an empty vector if this function doesn't know how to handle the given
/// type. Subsequent conversion rules will then be considered.
///
/// Otherwise, return a vector with the replacement type(s). If the type is
/// known to remain unchanged, return a singleton vector containing just the
/// original type.
using TypeLoweringFn = llvm::SmallVector<llvm::Type *>(TypeLowering &, llvm::Type *);

/// Given a constant that is known to be meant to be replaced based on its type,
/// attempt to replace it.
///
/// Return a non-empty vector if this function was able to handle the constant.
///
/// Otherwise, return an empty vector, and subsequent rules will be applied.
/// Default rules exist for poison, undef, and "null-like" (zeroinitializer
/// etc.).
using ConstantTypeLoweringFn = llvm::SmallVector<llvm::Constant *>(TypeLowering &, llvm::Constant *,
                                                                   llvm::ArrayRef<llvm::Type *>);

// =====================================================================================================================
/// Helper for lowerings that need to replace values of one type by one or more
/// values of another type.
///
/// This helper really has two parts:
///
///  - A type-level part that applies @ref TypeLoweringFn rules and caches the
///  result
///  - A value-level part that maintains a mapping of replaced values and
///  provides generic handlers for core
///    instructions like phi, select, and alloca
///
/// The type-level part can be reused even as the value-level part is cleared by
/// @ref finishCleanup, assuming that the type replacements are consistent
/// (which they might not always be, e.g. where the replacement depends on the
/// target architecture).
///
/// The value-level part is meant to be used as a nested @ref
/// llvm_dialects::Visitor client. It requires RPO traversal order. Its intended
/// use is along the following lines:
/// @code
///   struct MyPayload {
///     TypeLowering lowering;
///
///     MyPayload(LLVMContext &context) : lowering(context) {}
///   };
///
///   ...
///
///   MyPayload payload;
///
///   // Reverse post order traversal through functions, replacing instructions
///   with converted types as we go. static const auto visitor =
///   VisitorBuilder<MyPayload>
///       .add(...)
///       .nest(&TypeLowering::registerVisitors)
///       .build();
///   visitor.visit(payload, module);
///
///   // Fixup phi nodes.
///   payload.lowering.finishPhis();
///
///   // Erase all instructions that "have been replaced" (by calling
///   replaceInstruction for them). payload.lowering.finishCleanup();
/// @endcode
class TypeLowering {
public:
  TypeLowering(llvm::LLVMContext &);

  llvm::LLVMContext &getContext() const { return m_builder.getContext(); }

  void addRule(std::function<TypeLoweringFn>);
  void addConstantRule(std::function<ConstantTypeLoweringFn>);

  llvm::ArrayRef<llvm::Type *> convertType(llvm::Type *);

  static void registerVisitors(llvm_dialects::VisitorBuilder<TypeLowering> &);

  llvm::SmallVector<llvm::Value *> getValue(llvm::Value *);
  llvm::SmallVector<llvm::Value *> getValueOptional(llvm::Value *);
  void replaceInstruction(llvm::Instruction *, llvm::ArrayRef<llvm::Value *>);
  void eraseInstruction(llvm::Instruction *);

  llvm::Function *lowerFunctionArguments(llvm::Function &);
  void finishPhis();
  bool finishCleanup();

private:
  void recordValue(llvm::Value *, llvm::ArrayRef<llvm::Value *>);
  void replaceMappingWith(llvm::Value *, llvm::Value *);

  void visitAlloca(llvm::AllocaInst &);
  void visitExtract(llvm::ExtractValueInst &);
  void visitInsert(llvm::InsertValueInst &);
  void visitLoad(llvm::LoadInst &);
  void visitPhi(llvm::PHINode &);
  void visitSelect(llvm::SelectInst &);
  void visitStore(llvm::StoreInst &);

  /// Type conversion rules.
  llvm::SmallVector<std::function<TypeLoweringFn>> m_rules;
  llvm::SmallVector<std::function<ConstantTypeLoweringFn>> m_constantRules;

  /// Cache 1-1 mappings of types (including no-op mappings).
  llvm::DenseMap<llvm::Type *, llvm::Type *> m_unaryTypeConversions;

  /// Cache 1-N mappings of types.
  llvm::DenseMap<llvm::Type *, llvm::SmallVector<llvm::Type *, 2>> m_multiTypeConversions;

  llvm::IRBuilder<> m_builder;

  /// Map original values to type-converted values.
  ///
  /// For 1-1 mappings, this stores a value pointer.
  /// For 1-N mappings, this stores ((index << 1) | 1), where index is the index
  /// into m_convertedValueList at which the converted values can be found.
  llvm::DenseMap<llvm::Value *, uintptr_t> m_valueMap;
  std::vector<llvm::Value *> m_convertedValueList;

  /// Reverse map of values that occur as type-converted values to where they
  /// occur. The vector elements are either a value pointer (for 1-1 mapped
  /// values) or ((index << 1) | 1), where index is the index into
  /// m_convertedValueList.
  llvm::DenseMap<llvm::Value *, llvm::SmallVector<uintptr_t>> m_valueReverseMap;

  std::vector<std::pair<llvm::PHINode *, llvm::SmallVector<llvm::PHINode *>>> m_phis;
  std::vector<llvm::Instruction *> m_instructionsToErase;
  llvm::SmallVector<llvm::Function *> m_functionsToErase;
};
