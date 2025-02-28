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

#include "compilerutils/ValueSpecialization.h"
#include "llvm/IR/Module.h"
#include <cassert>

#define DEBUG_TYPE "value-specialization"

using namespace compilerutils;
using namespace llvm;

namespace compilerutils {

namespace {

struct SpecializationSummary {
  bool AllDwordsAreSpecialized = true;
  bool AnyDwordIsSpecialized = false;
};
static SpecializationSummary
computeSpecializationSummary(ArrayRef<ValueSpecializer::DwordSpecializationInfo> DwordInfos) {
  SpecializationSummary Result = {};
  for (const auto &DWI : DwordInfos) {
    if (DWI.Kind != ValueSpecializer::SpecializationKind::None)
      Result.AnyDwordIsSpecialized = true;
    else
      Result.AllDwordsAreSpecialized = false;
  }
  return Result;
}

} // namespace

ValueSpecializer::ValueSpecializer(Module &M)
    : B{M.getContext(), ConstantFolder{},
        IRBuilderCallbackInserter{[this](Instruction *Inst) { NewInsts.insert(Inst); }}},
      DL{M.getDataLayout()}, I32{Type::getInt32Ty(M.getContext())}, I64{Type::getInt64Ty(M.getContext())},
      NumReplacedDwords{}, NewInsts{} {
}

ValueSpecializer::ReplacementResult
ValueSpecializer::replaceDwords(Value *Val, ArrayRef<DwordSpecializationInfo> DwordInfos, bool ReplaceUses,
                                bool PreservePreviousInsertionPoint, StringRef NameSuffix) {
  assert(divideCeil(DL.getTypeStoreSize(Val->getType()), 4) == DwordInfos.size());
  NewInsts.clear();
  NumReplacedDwords = 0;

  if (IsFirstCall || !PreservePreviousInsertionPoint) {
    if (auto *Arg = dyn_cast<Argument>(Val)) {
      B.SetInsertPoint(Arg->getParent()->getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
    } else {
      // Insert *after* the given instruction, so we can use it
      auto *Inst = cast<Instruction>(Val);
      B.SetInsertPoint(Inst->getInsertionPointAfterDef().value());
    }
  }
  IsFirstCall = false;

  SmallVector<unsigned> Indices;
  Value *Replacement = replaceDwordsImpl(Val, Indices, Val->getType(), DwordInfos, (Val->getName() + NameSuffix).str());
  // Should be nullptr if nothing changed
  assert(Replacement != Val);
  if (Replacement != nullptr && ReplaceUses) {
    Val->replaceUsesWithIf(Replacement, [this](Use &U) -> bool { return !NewInsts.contains(U.getUser()); });
  }
  return {Replacement, NumReplacedDwords};
}

Value *ValueSpecializer::replaceDwordsInNonAggregate(Type *Ty, Value *Val, ArrayRef<DwordSpecializationInfo> DwordInfos,
                                                     StringRef ReplacementName) {
  assert(!Ty->isAggregateType());

  unsigned NumBytes = DL.getTypeStoreSize(Ty);
  if (NumBytes % 4) {
    // Small and misaligned types are not supported for now.
    // We could support specializing prefixes of large, misaligned types later.
    return nullptr;
  }
  [[maybe_unused]] unsigned NumDwords = NumBytes / 4;
  assert(DwordInfos.size() == NumDwords);

  if (Ty->isIntegerTy()) {
    if (Ty->getIntegerBitWidth() < 32)
      return nullptr;
    if (Ty->getIntegerBitWidth() == 32) {
      const DwordSpecializationInfo &DWI = DwordInfos[0];
      if (DWI.Kind == SpecializationKind::Constant) {
        ++NumReplacedDwords;
        return getI32Constant(DWI.ConstantValue);
      }
      if (DWI.Kind == SpecializationKind::FrozenPoison) {
        ++NumReplacedDwords;
        return getFrozenPoison(Ty);
      }
      return nullptr;
    }
    if (Ty->getIntegerBitWidth() == 64) {
      const DwordSpecializationInfo &LowInfo = DwordInfos[0];
      const DwordSpecializationInfo &HighInfo = DwordInfos[1];
      SpecializationKind LowKind = LowInfo.Kind;
      SpecializationKind HighKind = HighInfo.Kind;

      if (LowKind == HighKind) {
        // This can be handled without a bitwise or.
        NumReplacedDwords += 2;
        if (LowKind == SpecializationKind::Constant) {
          // return a single i64 constant.
          uint64_t I64Constant = HighInfo.ConstantValue;
          I64Constant <<= 32;
          I64Constant |= LowInfo.ConstantValue;
          return getI64Constant(I64Constant);
        }
        assert(LowKind == SpecializationKind::FrozenPoison);
        return getFrozenPoison(I64);
      }

      // Create two separate i64s containing the low and high dwords, and OR them together.
      uint64_t SingleDwordMask = ~(uint32_t{0});
      Value *LowDword = nullptr;
      if (LowKind == SpecializationKind::None) {
        assert(Val);
        LowDword = B.CreateAnd(Val, SingleDwordMask);
      } else {
        ++NumReplacedDwords;
        if (LowKind == SpecializationKind::Constant) {
          LowDword = getI64Constant(LowInfo.ConstantValue);
        } else {
          assert(LowKind == SpecializationKind::FrozenPoison);
          LowDword = B.CreateAnd(getFrozenPoison(I64), SingleDwordMask);
        }
      }

      Value *HighDword = nullptr;
      if (HighKind == SpecializationKind::None) {
        assert(Val);
        HighDword = B.CreateAnd(Val, SingleDwordMask << 32);
      } else {
        ++NumReplacedDwords;
        if (HighKind == SpecializationKind::Constant) {
          uint64_t HighDwordConstant = HighInfo.ConstantValue;
          HighDwordConstant <<= 32;
          HighDword = getI64Constant(HighDwordConstant);
        } else {
          assert(HighKind == SpecializationKind::FrozenPoison);
          HighDword = B.CreateAnd(getFrozenPoison(I64), SingleDwordMask << 32);
        }
      }

      return B.CreateOr(LowDword, HighDword, ReplacementName);
    }

    // Give up on other types
    return nullptr;
  }

  bool IsPointer = Ty->isPointerTy();
  if (Ty->isFloatingPointTy() || IsPointer) {
    unsigned BitWidth = 0;
    if (auto *PtrTy = dyn_cast<PointerType>(Ty))
      BitWidth = DL.getPointerSizeInBits(PtrTy->getAddressSpace());
    else
      BitWidth = Ty->getScalarSizeInBits();

    if (BitWidth < 32)
      return nullptr;

    // Reduce this to integer specialization
    Type *IntTy = IntegerType::get(Ty->getContext(), BitWidth);
    Value *BaseValue = nullptr;
    if (Val) {
      // Need to preserve some data, so start with bitcast of original value
      if (IsPointer)
        BaseValue = B.CreatePtrToInt(Val, IntTy);
      else
        BaseValue = B.CreateBitCast(Val, IntTy);
    }
    Value *SpecializedAsInt = replaceDwordsInNonAggregate(IntTy, BaseValue, DwordInfos, {});
    if (!SpecializedAsInt)
      return nullptr;

    if (IsPointer)
      return B.CreateIntToPtr(SpecializedAsInt, Ty, ReplacementName);
    return B.CreateBitCast(SpecializedAsInt, Ty, ReplacementName);
  }

  // Last remaining case: vectors.
  if (isa<ScalableVectorType>(Ty)) {
    // Not supported.
    return nullptr;
  }
  auto *VTy = cast<FixedVectorType>(Ty);
  // Similar to the aggregate case: For small elements, give up.
  // For dword-sized elements, just insert the new value.
  // For larger elements, extract the value, update it, and insert it again.
  Type *ElemTy = VTy->getElementType();
  if (!ElemTy->isIntegerTy() && !ElemTy->isFloatingPointTy()) {
    // E.g. pointers, not supported. Could add support if necessary.
    return nullptr;
  }
  unsigned NumElems = VTy->getNumElements();
  unsigned ElemNumBits = ElemTy->getPrimitiveSizeInBits();
  if (ElemNumBits % 32) {
    // Give up.
    return nullptr;
  }
  unsigned ElemNumDwords = ElemNumBits / 32;

  // While working on the vector elements, keep track of the current replaced full vector value.
  Value *ReplacedVector = Val;
  for (unsigned ElemIdx = 0; ElemIdx < NumElems; ++ElemIdx) {
    unsigned ElemDwordBegin = ElemIdx * ElemNumDwords;
    unsigned ElemDwordEnd = ElemDwordBegin + ElemNumDwords;
    assert(ElemDwordEnd <= DwordInfos.size());

    ArrayRef<DwordSpecializationInfo> ElemDwordInfos{DwordInfos.data() + ElemDwordBegin,
                                                     DwordInfos.data() + ElemDwordEnd};

    auto Summary = computeSpecializationSummary(ElemDwordInfos);
    if (!Summary.AnyDwordIsSpecialized) {
      // Nothing to do on this vector element.
      assert(Val != nullptr);
      continue;
    }

    Value *ElemBaseValue = Summary.AllDwordsAreSpecialized ? nullptr : B.CreateExtractElement(ReplacedVector, ElemIdx);
    Value *ReplacedElem = replaceDwordsInNonAggregate(ElemTy, ElemBaseValue, ElemDwordInfos, {});
    if (ReplacedElem) {
      if (ReplacedVector == nullptr) {
        // Start with a frozen poison value
        ReplacedVector = getFrozenPoison(Ty);
      }
      ReplacedVector = B.CreateInsertElement(ReplacedVector, ReplacedElem, ElemIdx, ReplacementName);
    }
  }

  // Return nullptr if nothing changed.
  return ReplacedVector != Val ? ReplacedVector : nullptr;
}

Value *ValueSpecializer::replaceDwordsImpl(Value *RootVal, SmallVectorImpl<unsigned> &Indices, Type *CurTy,
                                           ArrayRef<DwordSpecializationInfo> DwordInfos, StringRef ReplacementName) {
  assert(RootVal && CurTy);

  auto Summary = computeSpecializationSummary(DwordInfos);
  if (!Summary.AnyDwordIsSpecialized) {
    // Nothing to be done.
    return nullptr;
  }

  if (!CurTy->isAggregateType()) {
    // Base value to perform non-aggregate specialization on. Nullptr if all dwords are replaced.
    // The called specialization function then creates a base frozen poison value if necessary.
    // This might not be necessary in some cases, e.g. for a dword-sized value like an i32.
    Value *BaseValue = nullptr;
    if (!Summary.AllDwordsAreSpecialized) {
      if (Indices.empty()) {
        assert(RootVal->getType() == CurTy);
        BaseValue = RootVal;
      } else {
        // We are part of a (possibly nested) aggregate. Extract our value to work on it.
        BaseValue = B.CreateExtractValue(RootVal, Indices);
      }
    }

    // If the result of this call is going to be the final result, forward the replacement name.
    // Otherwise, we will create an insertvalue instruction that will get the name.
    StringRef NestedReplacementName = Indices.empty() ? ReplacementName : "";
    Value *Replaced = replaceDwordsInNonAggregate(CurTy, BaseValue, DwordInfos, NestedReplacementName);
    if (!Replaced)
      return nullptr;

    if (Indices.empty())
      return Replaced;

    // Insert the replacement into the root value
    return B.CreateInsertValue(RootVal, Replaced, Indices, ReplacementName);
  }

  // Final case: Aggregates
  assert(CurTy->isAggregateType());

  const StructLayout *SL = nullptr;
  ArrayType *ArrTy = dyn_cast<ArrayType>(CurTy);
  StructType *STy = dyn_cast<StructType>(CurTy);
  unsigned NumElements = -1;
  if (ArrTy) {
    NumElements = ArrTy->getNumElements();
  } else {
    NumElements = STy->getNumElements();
    SL = DL.getStructLayout(STy);
  }

  // While working on the aggregate elements, keep track of the current replaced full aggregate value.
  Value *ReplacedRootVal = RootVal;
  for (unsigned ElemIdx = 0; ElemIdx < NumElements; ++ElemIdx) {
    // Determine byte range covered by the element
    unsigned ElemByteOffset = -1;
    Type *ElemTy = nullptr;
    if (ArrTy) {
      ElemTy = ArrTy->getElementType();
      unsigned ElemAllocSize = DL.getTypeAllocSize(ElemTy);
      ElemByteOffset = ElemIdx * ElemAllocSize;
    } else {
      ElemTy = STy->getElementType(ElemIdx);
      ElemByteOffset = SL->getElementOffset(ElemIdx);
    }
    unsigned ElemByteSize = DL.getTypeStoreSize(ElemTy);

    if (ElemByteOffset % 4 != 0 || ElemByteSize % 4 != 0) {
      // Give up on small/misaligned types
      continue;
    }

    // The element corresponds to a sub-range of CurDwordInfos. Determine it.
    unsigned ElemDwordBegin = ElemByteOffset / 4;
    unsigned ElemNumDwords = ElemByteSize / 4;
    unsigned ElemDwordEnd = ElemDwordBegin + ElemNumDwords;
    assert(ElemDwordEnd <= DwordInfos.size());

    ArrayRef<DwordSpecializationInfo> ElemDwordInfos{DwordInfos.data() + ElemDwordBegin,
                                                     DwordInfos.data() + ElemDwordEnd};
    Indices.push_back(ElemIdx);
    Value *Replaced = replaceDwordsImpl(ReplacedRootVal, Indices, ElemTy, ElemDwordInfos, ReplacementName);
    Indices.pop_back();
    if (Replaced) {
      // Replacement was successful. In the next iteration, use Replaced as base value to operate on.
      ReplacedRootVal = Replaced;
    }
  }

  // Return nullptr if nothing changed
  return ReplacedRootVal != RootVal ? ReplacedRootVal : nullptr;
}

} // namespace compilerutils
