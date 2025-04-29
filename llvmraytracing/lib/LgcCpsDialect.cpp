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

#include "lgc/LgcCpsDialect.h"
#include "lgc/LgcRtDialect.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include <bitset>

#define GET_INCLUDES
#define GET_DIALECT_DEFS
#include "LgcCpsDialect.cpp.inc"

using namespace llvm;
using namespace lgc::rt;

constexpr const char CpsMetadata[] = "lgc.cps";
constexpr const char CpsMaxArgumentVgprsMetadata[] = "lgc.cps.maxArgumentVgprs";
constexpr const char CpsMaxOutgoingVgprCountMetadata[] = "lgc.cps.maxOutgoingVgprCount";

// =====================================================================================================================
// Helper to determine how many dwords we require to store a variable of a given
// type. Note that this does not include any padding except for pointers.
unsigned lgc::cps::getArgumentDwordCount(const DataLayout &DL, Type *type) {
  if (type->isSingleValueType()) {
    unsigned numComponents = type->isVectorTy() ? cast<FixedVectorType>(type)->getNumElements() : 1;

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
    return getArgumentDwordCount(DL, type->getArrayElementType()) * type->getArrayNumElements();

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
unsigned lgc::cps::getArgumentDwordCount(const DataLayout &DL, ArrayRef<Type *> types) {
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
std::optional<unsigned> lgc::cps::getRemainingArgumentDwords(const DataLayout &DL, ArrayRef<Type *> arguments) {
  const unsigned currentDwordUsage = getArgumentDwordCount(DL, arguments);

  if (currentDwordUsage > MaxArgumentDwords)
    return std::nullopt;

  return MaxArgumentDwords - currentDwordUsage;
}

// =====================================================================================================================
// Get the maximum number of VGPR registers that can be used as arguments by any
// shader in the pipeline. This includes payload registers and their
// corresponding padding.
std::optional<unsigned> lgc::cps::getMaxArgumentVgprs(const Module &m) {
  NamedMDNode *node = m.getNamedMetadata(CpsMaxArgumentVgprsMetadata);
  if (!node)
    return std::nullopt;

  return mdconst::extract<ConstantInt>(node->getOperand(0)->getOperand(0))->getZExtValue();
}

// Set the maximum number of VGPR registers that can be used as arguments by any
// shader in the pipeline.
void lgc::cps::setMaxArgumentVgprs(Module &module, unsigned maxArgumentVgprs) {
  LLVMContext &context = module.getContext();
  MDNode *node =
      MDNode::get(context, {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), maxArgumentVgprs))});
  module.getOrInsertNamedMetadata(CpsMaxArgumentVgprsMetadata)->addOperand(node);
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
void lgc::cps::setCpsFunctionLevel(Function &fn, CpsSchedulingLevel level) {
  assert(level < CpsSchedulingLevel::Count && "Invalid CPS level!");

  LLVMContext &context = fn.getContext();
  MDNode *node = MDNode::get(
      context, {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), static_cast<unsigned>(level)))});
  fn.setMetadata(CpsMetadata, node);
}

// =====================================================================================================================
// Returns the CPS level of a function, if the function is a CPS function and
// has the level metadata node set. For now, this always expects a function to
// have both the CPS metadata and the level metadata.
CpsSchedulingLevel lgc::cps::getCpsLevelFromFunction(const Function &fn) {
  MDNode *node = fn.getMetadata(fn.getContext().getMDKindID(CpsMetadata));
  if (!node) {
    // Expect that we have set the CPS metadata.
    llvm::report_fatal_error("Cannot call lgc::cps::getCpsLevelFromFunction on non-CPS function!");
  }

  const ConstantAsMetadata *c = cast<ConstantAsMetadata>(node->getOperand(0));
  unsigned level = cast<ConstantInt>(c->getValue())->getZExtValue();
  assert(level < static_cast<unsigned>(CpsSchedulingLevel::Count) && "Invalid CPS level!");
  return static_cast<CpsSchedulingLevel>(level);
}

// =====================================================================================================================
// Transform a shader type into the corresponding CPS level.
CpsSchedulingLevel lgc::cps::getCpsLevelForShaderStage(RayTracingShaderStage stage) {
  if (stage == RayTracingShaderStage::RayGeneration)
    return CpsSchedulingLevel::RayGen;

  if (stage == RayTracingShaderStage::Traversal)
    return CpsSchedulingLevel::Traversal;

  if (stage == RayTracingShaderStage::ClosestHit || stage == RayTracingShaderStage::Miss ||
      stage == RayTracingShaderStage::Callable)
    return CpsSchedulingLevel::ClosestHit_Miss_Callable;

  if (stage == RayTracingShaderStage::AnyHit)
    return CpsSchedulingLevel::AnyHit_CombinedIntersection_AnyHit;

  if (stage == RayTracingShaderStage::Intersection)
    return CpsSchedulingLevel::Intersection;

  llvm_unreachable("Cannot determine CPS level.");
}

// =====================================================================================================================
// Tries to convert a shader stage into the corresponding CPS levels in which
// the continued-to function can operate.
uint8_t lgc::cps::getPotentialCpsReturnLevels(RayTracingShaderStage stage) {
  std::bitset<8> CpsLevels;

  auto SetLevel = [&CpsLevels](CpsSchedulingLevel Level) -> void { CpsLevels.set(static_cast<uint8_t>(Level)); };

  switch (stage) {
  case RayTracingShaderStage::RayGeneration:
    llvm_unreachable("RayGen does not return.");
    break;
  case RayTracingShaderStage::Callable:
    // Callable returns to wherever CallShader is called (all stages except AHS
    // and IS).
  case RayTracingShaderStage::ClosestHit:
  case RayTracingShaderStage::Miss:
  case RayTracingShaderStage::Traversal:
    // These stages returns to wherever TraceRay is called (RGS, CHS and miss).
    SetLevel(CpsSchedulingLevel::RayGen);
    SetLevel(CpsSchedulingLevel::ClosestHit_Miss_Callable);
    break;
  case RayTracingShaderStage::AnyHit:
    // AHS returns to Traversal (triangle intersection) or IS (procedural
    // intersection).
    SetLevel(CpsSchedulingLevel::Traversal);
    SetLevel(CpsSchedulingLevel::Intersection);
    break;
  case RayTracingShaderStage::Intersection:
    // IS returns to Traversal only.
    SetLevel(CpsSchedulingLevel::Traversal);
    break;
  default:
    llvm_unreachable("Cannot determine CPS level.");
    break;
  }

  return static_cast<uint8_t>(CpsLevels.to_ulong());
}

// =====================================================================================================================
// Lower lgc.cps.as.continuation.reference operations into an integer
// representation of the pointer or a passed relocation. Return the new
// reference.
Value *lgc::cps::lowerAsContinuationReference(IRBuilder<> &Builder, lgc::cps::AsContinuationReferenceOp &AsCrOp,
                                              Value *Relocation) {
  Builder.SetInsertPoint(&AsCrOp);
  Value *Reference = nullptr;

  if (Relocation)
    Reference = Relocation;
  else
    Reference = Builder.CreatePtrToInt(AsCrOp.getFn(), AsCrOp.getType());

  return Reference;
}

// ====================================================================================================================
// Sets max outgoing VGPR count metadata.
void lgc::cps::setMaxOutgoingVgprCount(Function &fn, unsigned maxOutgoingVgpr) {
  LLVMContext &context = fn.getContext();
  MDNode *node =
      MDNode::get(context, {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), maxOutgoingVgpr))});
  fn.setMetadata(CpsMaxOutgoingVgprCountMetadata, node);
}

// =====================================================================================================================
// Returns the max outgoing VGPR count of a function. Returns std::nullopt if
// not set.
// If this metadata is set, it means that this function will write the number
// into an SGPR; if not, it means that this function will read the number from
// an input SGPR.
std::optional<unsigned> lgc::cps::tryGetMaxOutgoingVgprCount(const Function &fn) {
  MDNode *node = fn.getMetadata(fn.getContext().getMDKindID(CpsMaxOutgoingVgprCountMetadata));
  if (!node) {
    return std::nullopt;
  }

  const ConstantAsMetadata *c = cast<ConstantAsMetadata>(node->getOperand(0));
  return cast<ConstantInt>(c->getValue())->getZExtValue();
}
