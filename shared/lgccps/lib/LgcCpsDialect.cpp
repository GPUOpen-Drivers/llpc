/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *deal in the Software without restriction, including without limitation the
 *rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *IN THE SOFTWARE.
 *
 **********************************************************************************************************************/

#include "lgccps/LgcCpsDialect.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#define GET_INCLUDES
#define GET_DIALECT_DEFS
#include "LgcCpsDialect.cpp.inc"

using namespace llvm;

constexpr const char CpsMetadata[] = "lgc.cps";

// The maximum amount of dwords usable for passing arguments
constexpr int MaxArgumentDwords = 32;

// =====================================================================================================================
// Helper to determine how many dwords we require to store a variable of a given
// type. Note that this does not include any padding except for pointers.
unsigned lgc::cps::getArgumentDwordCount(const DataLayout &DL, Type *type) {
  if (type->isSingleValueType()) {
    unsigned numComponents =
        type->isVectorTy() ? cast<FixedVectorType>(type)->getNumElements() : 1;

    // One VGPR lane can store 32 bit, e. g. 1 dword.
    // Note that this will not take into account that we could possibly store
    // multiple smaller types in one VGPR but assumes that we end up using at
    // least one VGPR lane.
    const unsigned vgprBitWidth = 32;
    unsigned dwordCount = 1;

    unsigned bitWidth = 0;
    if (auto *ptrTy = dyn_cast<PointerType>(type))
      bitWidth = DL.getPointerSizeInBits(ptrTy->getAddressSpace());
    else
      bitWidth = type->getScalarSizeInBits();

    // If the type doesn't fit in one dword, compute how many dwords we require
    // at least by conservatively rounding up.
    dwordCount = llvm::divideCeil(bitWidth, vgprBitWidth);

    // For a vector of n components, assume we need n x dwordCount elements.
    dwordCount *= numComponents;

    return dwordCount;
  }

  if (type->isArrayTy())
    return getArgumentDwordCount(DL, type->getArrayElementType()) *
           type->getArrayNumElements();

  if (auto *structTy = dyn_cast<StructType>(type)) {
    unsigned memberDwordCount = 0;
    for (Type *elementType : structTy->elements())
      memberDwordCount += getArgumentDwordCount(DL, elementType);

    return memberDwordCount;
  }

  report_fatal_error("lgc::cps::getArgumentDwordCount: Unsupported LLVM type");
}

// =====================================================================================================================
// Helper to determine how many dwords are occupied by a given set of types.
unsigned lgc::cps::getArgumentDwordCount(const DataLayout &DL,
                                         ArrayRef<Type *> types) {
  unsigned currentDwordUsage = 0;

  for (Type *type : types)
    currentDwordUsage += getArgumentDwordCount(DL, type);

  return currentDwordUsage;
}

// =====================================================================================================================
// Determine how many dwords / VGPRs can be added to a given argument list.
// Returns
//    0, if we reached the maximum given by MaxArgumentDwords
//    std::nullopt, if we exceeded it.
std::optional<unsigned>
lgc::cps::getRemainingArgumentDwords(const DataLayout &DL,
                                     ArrayRef<Type *> arguments) {
  const unsigned currentDwordUsage = getArgumentDwordCount(DL, arguments);

  if (currentDwordUsage > MaxArgumentDwords)
    return std::nullopt;

  return MaxArgumentDwords - currentDwordUsage;
}

// =====================================================================================================================
// Checks if a function is annotated with !lgc.cps metadata.
bool lgc::cps::isCpsFunction(const Function &fn) {
  MDNode *node = fn.getMetadata(fn.getContext().getMDKindID(CpsMetadata));
  return node != nullptr;
}

// =====================================================================================================================
// Transforms a function into a CPS function by setting the CPS level as
// metadata.
void lgc::cps::setCpsFunctionLevel(Function &fn, CpsLevel level) {
  assert(level < CpsLevel::Count && "Invalid CPS level!");

  LLVMContext &context = fn.getContext();
  MDNode *node = MDNode::get(
      context, {ConstantAsMetadata::get(ConstantInt::get(
                   Type::getInt32Ty(context), static_cast<unsigned>(level)))});
  fn.setMetadata(CpsMetadata, node);
}

// =====================================================================================================================
// Returns the CPS level of a function, if the function is a CPS function and
// has the level metadata node set. For now, this always expects a function to
// have both the CPS metadata and the level metadata.
lgc::cps::CpsLevel lgc::cps::getCpsLevelFromFunction(const Function &fn) {
  MDNode *node = fn.getMetadata(fn.getContext().getMDKindID(CpsMetadata));
  if (!node) {
    // Expect that we have set the CPS metadata.
    llvm::report_fatal_error(
        "Cannot call lgc::cps::getCpsLevelFromFunction on non-CPS function!");
  }

  const ConstantAsMetadata *c = cast<ConstantAsMetadata>(node->getOperand(0));
  unsigned level = cast<ConstantInt>(c->getValue())->getZExtValue();
  assert(level < static_cast<unsigned>(CpsLevel::Count) &&
         "Invalid CPS level!");
  return static_cast<CpsLevel>(level);
}
