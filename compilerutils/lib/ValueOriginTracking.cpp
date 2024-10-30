/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "compilerutils/ValueOriginTracking.h"
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "value-origin-tracking"

using namespace CompilerUtils;
using namespace CompilerUtils::ValueTracking;
using namespace llvm;

namespace CompilerUtils {

namespace {

// Given indices into an aggregate type used in {extract,insert}value instructions,
// compute the byte offset of the value indexed by the indices.
static unsigned computeByteOffsetInAggregate(Type *AggTy, ArrayRef<unsigned> Indices, const DataLayout &DL) {
  Type *I32 = IntegerType::getInt32Ty(AggTy->getContext());
  // Compute the byte offset of the extracted value by essentially interpreting the indices as GEP indices
  // TODO: Can we do this without the GEP hack, and without re-implementing aggregate bit layouts?
  SmallVector<Value *> GEPIndices;
  GEPIndices.reserve(Indices.size());
  GEPIndices.push_back(ConstantInt::getSigned(I32, 0));
  for (auto Idx : Indices)
    GEPIndices.push_back(ConstantInt::getSigned(I32, Idx));

  APInt APOffset{32, 0};
  [[maybe_unused]] bool Success = GEPOperator::accumulateConstantOffset(AggTy, GEPIndices, DL, APOffset);
  // This should always succeed with constant indices
  assert(Success);

  unsigned Offset = APOffset.getZExtValue();
  return Offset;
}

static std::optional<unsigned> computeByteOffsetInVector(Type *VecTy, Value *IndexArg, const DataLayout &DL) {
  auto *ConstantIndex = dyn_cast<ConstantInt>(IndexArg);
  if (!ConstantIndex)
    return std::nullopt;

  Type *ElemTy = cast<VectorType>(VecTy)->getElementType();
  unsigned BitWidth = DL.getTypeSizeInBits(ElemTy);
  if (BitWidth % 8)
    return std::nullopt;

  unsigned Index = ConstantIndex->getZExtValue();
  return Index * (BitWidth / 8);
}

// Combine slice infos for a select or phi instruction, so we know that our slice equals
// one of the given slices, but we don't know which.
std::optional<SliceInfo> static combineSliceInfosForSelect(ArrayRef<const SliceInfo *> Slices) {
  if (Slices.empty())
    return std::nullopt;
  if (Slices.size() == 1)
    return *Slices[0];

  SliceInfo Result{SliceStatus::makeEmpty()};
  auto AddResultStatusBit = [&Result](SliceStatus StatusBit) {
    assert(StatusBit.isSingleStatus());
    Result.Status = (Result.Status | StatusBit);
  };

  // Set constant if there is a consistent one
  {
    std::optional<uint32_t> OptConstantValue;
    for (const SliceInfo *Slice : Slices) {
      if (Slice->Status.contains(SliceStatus::Constant)) {
        if (!OptConstantValue.has_value()) {
          // we are the first to require a specific constant.
          OptConstantValue = Slice->ConstantValue;
        } else {
          // there already is a value. check for consistency.
          if (OptConstantValue.value() != Slice->ConstantValue) {
            // conflict.
            return std::nullopt;
          }
        }
      }
    }
    if (OptConstantValue.has_value()) {
      AddResultStatusBit(SliceStatus::Constant);
      Result.ConstantValue = OptConstantValue.value();
    }
  }

  // Set dynamic info if there is a consistent one
  {
    struct DynInfo {
      Value *V;
      unsigned Offset;
    };
    std::optional<DynInfo> OptDynInfo;
    for (const SliceInfo *Slice : Slices) {
      if (Slice->Status.contains(SliceStatus::Dynamic)) {
        DynInfo CurDynInfo = {Slice->DynamicValue, Slice->DynamicValueByteOffset};
        if (!OptDynInfo.has_value()) {
          // we are the first to require a specific constant.
          OptDynInfo = CurDynInfo;
        } else {
          // there already is a value. check for consistency.
          if (OptDynInfo.value().V != CurDynInfo.V || OptDynInfo->Offset != CurDynInfo.Offset) {
            // conflict.
            return std::nullopt;
          }
        }
      }
    }
    if (OptDynInfo.has_value()) {
      AddResultStatusBit(SliceStatus::Dynamic);
      Result.DynamicValue = OptDynInfo->V;
      Result.DynamicValueByteOffset = OptDynInfo->Offset;
    }
  }

  // Check for UndefOrPoison
  if (std::any_of(Slices.begin(), Slices.end(),
                  [](const SliceInfo *Slice) { return Slice->Status.contains(SliceStatus::UndefOrPoison); }))
    AddResultStatusBit(SliceStatus::UndefOrPoison);
  return Result;
}

} // namespace

// Helper class to create ValueInfos
struct ValueOriginTracker::ValueInfoBuilder {
  ValueInfoBuilder(const DataLayout &DL, Value *V, unsigned BytesPerSlice, unsigned MaxBytesPerValue)
      : V{V}, BytesPerSlice{BytesPerSlice}, MaxBytesPerValue{MaxBytesPerValue},
        NumBits{static_cast<unsigned>(DL.getTypeSizeInBits(V->getType()).getFixedValue())},
        NumBytes{divideCeil(NumBits, 8)}, NumSlices{
                                              llvm::divideCeil(std::min(NumBytes, MaxBytesPerValue), BytesPerSlice)} {}

  Value *V = nullptr; // The value for which we are building a ValueInfo
  unsigned BytesPerSlice = 0;
  unsigned MaxBytesPerValue;
  unsigned NumBits = 0;
  unsigned NumBytes = 0;
  unsigned NumSlices = 0;

  // In cases where we can't reason about a slice, we use a dynamic self-referencing slice.
  SliceInfo getDynamicSlice(unsigned SliceIdx) const {
    SliceInfo SI{SliceStatus::Dynamic};
    SI.DynamicValue = V;
    SI.DynamicValueByteOffset = BytesPerSlice * SliceIdx;
    return SI;
  }

  ValueInfo createUndef() const {
    SliceInfo SI{SliceStatus::UndefOrPoison};
    ValueInfo Result{};
    Result.Slices.resize(NumSlices, SI);
    return Result;
  }

  // Creates a value info for a value that has the given constant on every slice.
  ValueInfo createUniformConstant(uint32_t UniformConstantValue) const {
    SliceInfo SI{SliceStatus::Constant};
    SI.ConstantValue = UniformConstantValue;
    ValueInfo Result{};
    Result.Slices.reserve(NumSlices);

    unsigned BitsPerSlice = 8 * BytesPerSlice;
    unsigned NumRemainingBits = NumBits;

    for (unsigned SliceIdx = 0; SliceIdx < NumSlices; ++SliceIdx) {
      if (NumRemainingBits < BitsPerSlice) {
        // For the last slice, zero out the upper dead bits. This isn't required by the interface,
        // but is simple and leads to nicer tests.
        assert(SliceIdx + 1 == NumSlices);
        SI.ConstantValue &= (~0u >> (BitsPerSlice - NumRemainingBits));
        Result.Slices.push_back(SI);
        break;
      }
      Result.Slices.push_back(SI);
      NumRemainingBits -= BitsPerSlice;
    }
    return Result;
  }

  // Given KnownBits about the value, return a value info that uses constant slices where possible,
  // and fall back to dynamic slices if necessary.
  // This may be required for slices where not all bits are known.
  ValueInfo createConstant(const KnownBits &KB) const {
    assert(KB.One.getBitWidth() == NumBits);
    ValueInfo Result{};
    Result.Slices.reserve(NumSlices);
    unsigned BitsPerSlice = 8 * BytesPerSlice;
    unsigned SliceMask = ~0u >> (8 * (4 - BytesPerSlice));
    unsigned NumRemainingBits = NumBits;
    auto GetSliceFromAPInt = [&](const APInt &AI, unsigned SliceIdx) -> uint32_t {
      assert(BytesPerSlice <= 4);
      unsigned DWIdx = (BytesPerSlice * SliceIdx) / 4;
      unsigned ByteOffsetInDW = (BytesPerSlice * SliceIdx) % 4;
      unsigned QWIdx = DWIdx / 2;
      assert(QWIdx < AI.getNumWords());
      auto QW = AI.getRawData()[QWIdx];
      if (DWIdx % 2) {
        QW >>= 32;
      }
      QW >>= (8 * ByteOffsetInDW);
      return QW & SliceMask;
    };
    for (unsigned SliceIdx = 0; SliceIdx < NumSlices; ++SliceIdx) {
      auto One = GetSliceFromAPInt(KB.One, SliceIdx);
      auto Zero = GetSliceFromAPInt(KB.Zero, SliceIdx);
      if (NumRemainingBits < BitsPerSlice) {
        // For the last slice, accept a partial known mask, because the tail bits are dead
        // and not analyzed by KnownBits
        SliceMask >>= (BitsPerSlice - NumRemainingBits);
        assert(SliceIdx + 1 == NumSlices);
      }
      if ((One | Zero) == SliceMask) {
        SliceInfo SI{SliceStatus::Constant};
        SI.ConstantValue = One;
        Result.Slices.push_back(SI);
      } else {
        // There are unknown bits. Give up on this slice.
        Result.Slices.push_back(getDynamicSlice(SliceIdx));
      }
      NumRemainingBits -= BitsPerSlice;
    }
    return Result;
  }

  // Return a value info that just refers to the value itself on every slice. This can always be used as fallback.
  ValueInfo createDynamic() const {
    ValueInfo Result{};
    Result.Slices.reserve(NumSlices);
    for (unsigned SliceIdx = 0; SliceIdx < NumSlices; ++SliceIdx)
      Result.Slices.push_back(getDynamicSlice(SliceIdx));

    return Result;
  }

  // Obtain the value info for a sub-range of slices.
  ValueInfo createExtraction(const ValueInfo &AggInfo, unsigned ByteOffset) const {
    // Note that NumBytes might not be a multiple of slices, and thus
    // the last slice of Result might cover data outside of our value.
    // But that should be fine, we might just be a bit pessimistic.
    if (ByteOffset % BytesPerSlice) {
      LLVM_DEBUG(dbgs() << "Non-aligned extract " << *V << ", giving up.\n");
      return createDynamic();
    }
    ValueInfo Result{};
    unsigned BeginSlice = ByteOffset / BytesPerSlice;
    unsigned ResultNumSlices = NumSlices;
    if (BeginSlice < AggInfo.Slices.size()) {
      ResultNumSlices = std::min(NumSlices, static_cast<unsigned>(AggInfo.Slices.size() - BeginSlice));
      Result.Slices.append(AggInfo.Slices.begin() + BeginSlice, AggInfo.Slices.begin() + BeginSlice + NumSlices);
    }
    // Fill up with fallback if necessary
    for (unsigned SliceIdx = Result.Slices.size(); SliceIdx < ResultNumSlices; ++SliceIdx)
      Result.Slices.push_back(getDynamicSlice(SliceIdx));

    assert(Result.Slices.size() == ResultNumSlices && ResultNumSlices <= NumSlices);

    return Result;
  }

  // Computes a ValueInfo that is obtained by inserting a value at the given byte offset and size
  // into this value, e.g. in insert{value, element}.
  ValueInfo createInsertion(const ValueInfo &Agg, const ValueInfo &Inserted, unsigned ByteOffset,
                            unsigned InsertedByteCount) const {
    ValueInfo Result = Agg;
    unsigned SliceBegin = ByteOffset / BytesPerSlice;
    unsigned SliceEnd =
        std::min<unsigned>(divideCeil(ByteOffset + InsertedByteCount, BytesPerSlice), Result.Slices.size());
    if (ByteOffset % BytesPerSlice) {
      LLVM_DEBUG(dbgs() << "Insertion with non-aligned offset: " << *V << "\n");
      // We don't support merging misaligned slices. Use the fallback for all affected slices.
      for (unsigned SliceIdx = SliceBegin; SliceIdx < SliceEnd; ++SliceIdx)
        Result.Slices[SliceIdx] = getDynamicSlice(SliceIdx);

      assert(Result.Slices.size() == NumSlices);
      return Result;
    }
    for (unsigned SliceIdx = SliceBegin; SliceIdx < SliceEnd; ++SliceIdx) {
      unsigned OtherSliceIdx = SliceIdx - SliceBegin;
      if (OtherSliceIdx < Inserted.Slices.size())
        Result.Slices[SliceIdx] = Inserted.Slices[OtherSliceIdx];
      else
        Result.Slices.push_back(getDynamicSlice(SliceIdx));
    }
    if (InsertedByteCount % BytesPerSlice && SliceBegin < SliceEnd) {
      LLVM_DEBUG(dbgs() << "Insertion with non-aligned size " << *V << "\n");
      // The last slice is only partially replaced.
      // We don't yet support merging partial slices
      Result.Slices[SliceEnd - 1] = getDynamicSlice(SliceEnd - 1);
    }

    assert(Result.Slices.size() == NumSlices);
    return Result;
  }

  // Create a value info for a value that is obtained by selecting one of the given values,
  // e.g. in a phi or select instruction.
  ValueInfo createSelect(ArrayRef<const ValueInfo *> ValueInfos) {
    if (ValueInfos.empty())
      return createDynamic();
    if (ValueInfos.size() == 1)
      return *ValueInfos[0];
    SmallVector<const SliceInfo *> SliceInfos;
    SliceInfos.reserve(ValueInfos.size());
    bool Stop = false;
    ValueInfo Result;
    Result.Slices.reserve(ValueInfos[0]->Slices.size());
    for (unsigned SliceIdx = 0; SliceIdx < NumSlices; ++SliceIdx) {
      SliceInfos.clear();
      for (const ValueInfo *ValueInfo : ValueInfos) {
        if (SliceIdx < ValueInfo->Slices.size()) {
          SliceInfos.push_back(&ValueInfo->Slices[SliceIdx]);
        } else {
          // Give up on this and higher slices
          Stop = true;
          break;
        }
      }
      if (Stop)
        break;
      std::optional<SliceInfo> OptSliceInfo = combineSliceInfosForSelect(SliceInfos);
      if (OptSliceInfo.has_value()) {
        // We succeeded in combining the slices
        Result.Slices.push_back(OptSliceInfo.value());
      } else {
        // Create dynamic slice
        SliceInfo SI{SliceStatus::Dynamic};
        SI.DynamicValue = V;
        SI.DynamicValueByteOffset = BytesPerSlice * SliceIdx;
        Result.Slices.push_back(SI);
      }
    }
    return Result;
  }

  // For each slice, the assumption either gives us constant/undef values, or references
  // other dynamic values. ReferencedInfos is indexed by slices and gives value infos for these
  // referenced dynamic values.
  // This function then combines all these infos accordingly.
  ValueInfo createFromAssumption(const ValueInfo &Assumption, ArrayRef<const ValueInfo *> ReferencedInfos) {
    ValueInfo Result;
    assert(Assumption.Slices.size() == ReferencedInfos.size());
    for (unsigned SliceIdx = 0; SliceIdx < NumSlices; ++SliceIdx) {
      if (SliceIdx >= Assumption.Slices.size()) {
        // If slices are missing in the assumption, use the dynamic fallback
        Result.Slices.push_back(getDynamicSlice(SliceIdx));
        continue;
      }
      // Start with the assumption, then merge with the referenced info if applicable.
      // For non-dynamic assumptions, we just use the assumption directly.
      SliceInfo AssumptionSI = Assumption.Slices[SliceIdx];
      assert(AssumptionSI.Status.isSingleStatus());
      if (!AssumptionSI.Status.contains(SliceStatus::Dynamic)) {
        Result.Slices.push_back(AssumptionSI);
        continue;
      }
      // No multi-status assumptions are allowed, this would require merging constants here
      assert(AssumptionSI.Status == SliceStatus::Dynamic);
      const ValueInfo *ReferencedInfo = ReferencedInfos[SliceIdx];
      if (ReferencedInfo != nullptr) {

        if (AssumptionSI.DynamicValueByteOffset % BytesPerSlice) {
          // Misaligned assumption, give up on this slice
          Result.Slices.push_back(getDynamicSlice(SliceIdx));
          continue;
        }
        unsigned ReferencedSliceIdx = AssumptionSI.DynamicValueByteOffset / BytesPerSlice;
        if (ReferencedSliceIdx >= ReferencedInfo->Slices.size()) {
          // No referenced slice available
          Result.Slices.push_back(getDynamicSlice(SliceIdx));
          continue;
        }
        // The assumption references an existing slice info. Use that one.
        Result.Slices.push_back(ReferencedInfo->Slices[ReferencedSliceIdx]);
      } else {
        // Missing reference infos are only allowed for self-references
        assert(AssumptionSI.DynamicValue == V);
        Result.Slices.push_back(getDynamicSlice(SliceIdx));
      }
    }
    assert(Result.Slices.size() == NumSlices);
    return Result;
  }

  // Create a value info for a freeze instruction.
  // For freeze, we must be careful to preserve freeze semantics on UndefOrPoison slices:
  // In contrast to undef/poison, all uses of a freeze instruction are guaranteed to observe the same value.
  ValueInfo createFreeze(const ValueInfo &FrozenValueInfo, Options::FreezeHandlingMode FreezeMode) const {
    using Mode = Options::FreezeHandlingMode;
    if (FreezeMode == Mode::Forward)
      return FrozenValueInfo;

    assert(FreezeMode == Mode::Dynamic);

    ValueInfo Result = FrozenValueInfo;
    for (unsigned SliceIdx = 0; SliceIdx < Result.Slices.size(); ++SliceIdx) {
      SliceInfo &SI = Result.Slices[SliceIdx];
      if (SI.Status.contains(SliceStatus::UndefOrPoison))
        SI = getDynamicSlice(SliceIdx);
    }

    return Result;
  }
};

// Implement status printing also here, because for multi-bit status we want to interleave the printing
// with the referenced values.
void SliceInfo::print(llvm::raw_ostream &OS, bool Compact) const {
  bool IsFirst = true;
  auto Sep = Compact ? "|" : " | ";
  if (!Status.isSingleStatus())
    OS << "(";
  if (Status.contains(SliceStatus::UndefOrPoison)) {
    if (!IsFirst)
      OS << Sep;
    IsFirst = false;
    OS << (Compact ? "U" : "UndefOrPoison");
  }
  if (Status.contains(SliceStatus::Constant)) {
    if (!IsFirst)
      OS << Sep;
    IsFirst = false;
    if (Compact) {
      OS << "C";
    } else {
      OS << "Constant: 0x";
      OS.write_hex(ConstantValue);
    }
  }
  if (Status.contains(SliceStatus::Dynamic)) {
    if (!IsFirst)
      OS << Sep;
    IsFirst = false;
    bool IsArg = isa<Argument>(DynamicValue);
    if (Compact) {
      OS << (IsArg ? "A" : "D");
    } else {
      OS << "Dynamic" << (IsArg ? " (argument): " : ": ") << *DynamicValue << " (offset " << DynamicValueByteOffset
         << ")";
    }
  }
  if (!Status.isSingleStatus())
    OS << ")";
}

llvm::raw_ostream &CompilerUtils::ValueTracking::operator<<(llvm::raw_ostream &OS, const SliceInfo &SI) {
  SI.print(OS);
  return OS;
}

void ValueTracking::ValueInfo::print(llvm::raw_ostream &OS, bool Compact) const {
  if (Compact) {
    for (const auto &Slice : Slices) {
      Slice.print(OS, true);
    }
  } else {
    for (const auto &[Idx, Slice] : enumerate(Slices)) {
      if (Idx)
        OS << "; ";
      OS << Slice;
    }
  }
}

llvm::raw_ostream &CompilerUtils::ValueTracking::operator<<(llvm::raw_ostream &OS, const ValueInfo &VI) {
  VI.print(OS);
  return OS;
}

ValueInfo ValueOriginTracker::computeConstantValueInfo(ValueInfoBuilder &VIB, llvm::Constant *CV) {
  if (CV->isNullValue())
    return VIB.createUniformConstant(0);

  // Don't bother with globals we can't reason about
  if (isa<Function>(CV) || isa<GlobalVariable>(CV) || isa<PointerType>(CV->getType()))
    return VIB.createDynamic();

  auto Ty = CV->getType();
  unsigned BitsPerSlice = 8 * Opts.BytesPerSlice;
  // Don't bother with dynamic vectors
  auto *VectorTy = dyn_cast<FixedVectorType>(Ty);
  auto *ArrayTy = dyn_cast<ArrayType>(Ty);
  Type *ElemTy = nullptr;
  unsigned NumElements = 0;
  if (VectorTy) {
    ElemTy = VectorTy->getElementType();
    NumElements = VectorTy->getNumElements();
  } else if (ArrayTy) {
    ElemTy = ArrayTy->getElementType();
    NumElements = ArrayTy->getNumElements();
  }

  // For integer constants, FP constants, and vector-of-integer constants, use computeKnownBits.
  // It does not support vector of FP, or arrays.
  if (isa<ConstantInt>(CV) || isa<ConstantFP>(CV) || (VectorTy && ElemTy->isIntegerTy())) {
    // computeKnownBits only supports integers and integer vector types.
    // For vector types, it returns common known bits merged across all elements, as wide as single
    // element, instead of known bits of the whole value. Thus, cast non-integers to integers first.
    Value *ToBeAnalyzed = CV;
    if (!CV->getType()->isIntegerTy()) {
      unsigned NumBits = DL.getTypeSizeInBits(CV->getType());
      llvm::Type *IntTy = IntegerType::get(CV->getContext(), NumBits);
      ToBeAnalyzed = ConstantExpr::getBitCast(CV, IntTy);
    }
    auto KnownBits = computeKnownBits(ToBeAnalyzed, DL, 2);
    return VIB.createConstant(KnownBits);
  }

  // The remainder of this function deals with arrays and vectors only.
  if (VectorTy == nullptr && ArrayTy == nullptr)
    return VIB.createDynamic();

  auto *ConstDataSeq = dyn_cast<ConstantDataSequential>(CV);
  auto *ConstArr = dyn_cast<ConstantArray>(CV);
  auto *ConstVec = dyn_cast<ConstantVector>(CV);
  assert(ConstDataSeq == nullptr || ConstDataSeq->getNumElements() == NumElements);
  assert(ConstArr == nullptr || ConstArr->getNumOperands() == NumElements);
  assert(ConstVec == nullptr || ConstVec->getNumOperands() == NumElements);

  if (ConstDataSeq != nullptr || ConstArr != nullptr || ConstVec != nullptr) {
    // Array or vector. Try to concatenate the elements infos if possible.
    // This is possible if element sizes are slice-aligned, and no padding needs to be considered.
    // We could maybe extend the below to structs, but that's even more complicated because
    // we need to account for padding on every element, and there can be nested structs, so ignore them for now.
    unsigned BitsPerElement = ElemTy->getPrimitiveSizeInBits();
    unsigned AlignedBitsPerElement = VectorTy ? BitsPerElement : 8 * DL.getTypeAllocSize(ElemTy).getFixedValue();
    if (BitsPerElement != AlignedBitsPerElement || BitsPerElement % BitsPerSlice != 0)
      return VIB.createDynamic();

    // Handle constant vector of values whose sizes are integer-multiples of the slice size,
    // so we can just concatenate slices element-wise
    unsigned SlicesPerElement = BitsPerElement / BitsPerSlice;
    ValueInfo Result;
    Result.Slices.reserve(SlicesPerElement * NumElements);
    for (unsigned ElemIdx = 0; ElemIdx < NumElements; ++ElemIdx) {
      // Accessing the element as constant is slightly less efficient, but allows to use the
      // computeKnownBits() machinery to obtain bit layouts of floats
      llvm::Constant *ElemAsConstant = nullptr;
      if (ConstDataSeq) {
        ElemAsConstant = ConstDataSeq->getElementAsConstant(ElemIdx);
      } else if (ConstArr) {
        ElemAsConstant = ConstArr->getOperand(ElemIdx);
      } else {
        assert(ConstVec != nullptr);
        ElemAsConstant = ConstVec->getOperand(ElemIdx);
      }
      const auto &ValueInfo = getOrComputeValueInfo(ElemAsConstant);
      Result.Slices.append(ValueInfo.Slices);
    }
    return Result;
  }

  return VIB.createDynamic();
}

ValueInfo ValueOriginTracker::computeValueInfoFromAssumption(ValueInfoBuilder &VIB, const ValueInfo &OriginAssumption) {
  SmallVector<const ValueInfo *> ReferencedValueInfos;
  ReferencedValueInfos.reserve(OriginAssumption.Slices.size());
  for (const auto &AssumptionSliceInfo : OriginAssumption.Slices) {
    const ValueInfo *ReferencedValueInfo = nullptr;
    if (AssumptionSliceInfo.DynamicValue) {
      if (AssumptionSliceInfo.DynamicValue != VIB.V) {
        auto ReferencedIt = ValueInfos.find(AssumptionSliceInfo.DynamicValue);
        assert(ReferencedIt != ValueInfos.end());
        ReferencedValueInfo = &ReferencedIt->second;
      } else {
        // The assumption on this slice is trivial, referring to the value itself.
        // Leave the nullptr as-is, and handle it in createFromAssumption
      }
    }
    ReferencedValueInfos.push_back(ReferencedValueInfo);
  }
  return VIB.createFromAssumption(OriginAssumption, ReferencedValueInfos);
}

// Analyze a value, creating a ValueInfo for it.
// If V is an instruction, this asserts the ValueInfos of dependencies have already been created.
// An exception are PHI nodes: We only support propagation in a single pass, and thus handle loops conservatively,
// treating dependencies on earlier loop iterations as dynamic. Thus, for PHI nodes, if dependencies have not yet
// been analyzed, we assume loop dependencies and give up.
ValueInfo ValueOriginTracker::computeValueInfo(llvm::Value *V) {
  ValueInfoBuilder VIB{DL, V, Opts.BytesPerSlice, Opts.MaxBytesPerValue};
  if (isa<UndefValue>(V)) {
    return VIB.createUndef();
  }
  if (auto *CV = dyn_cast<llvm::Constant>(V))
    return computeConstantValueInfo(VIB, CV);

  Instruction *Inst = dyn_cast<Instruction>(V);
  if (!Inst)
    return VIB.createDynamic();

  auto OriginAssumptionIt = OriginAssumptions.find(Inst);
  if (OriginAssumptionIt != OriginAssumptions.end()) {
    // There is an origin assumption on this instruction. Collect and combine the value infos of referenced values.
    // Note: This does not combine with an analysis of V that we would have done without an assumption.
    // This can be pessimistic if there are assumptions on values we can analyze, but for now
    // this suffices as we only plan to add assumptions on values that are otherwise completely opaque.
    return computeValueInfoFromAssumption(VIB, OriginAssumptionIt->second);
  }

  switch (Inst->getOpcode()) {
  case Instruction::AddrSpaceCast:
  case Instruction::BitCast: {
    // Just forward the operand for size-preserving type conversions
    auto *Op = Inst->getOperand(0);
    auto It = ValueInfos.find(Op);
    assert(It != ValueInfos.end());
    return It->second;
  }
  case Instruction::Freeze: {
    auto *Op = Inst->getOperand(0);
    auto It = ValueInfos.find(Op);
    assert(It != ValueInfos.end());
    return VIB.createFreeze(It->second, Opts.FreezeMode);
  }
  case Instruction::ExtractElement: {
    auto *EE = cast<ExtractElementInst>(Inst);
    auto *Vec = EE->getVectorOperand();
    auto *IndexArg = EE->getIndexOperand();

    std::optional<unsigned> Offset = computeByteOffsetInVector(Vec->getType(), IndexArg, DL);
    if (!Offset.has_value())
      return VIB.createDynamic();

    // Obtain ValueInfo for the source aggregate
    auto It = ValueInfos.find(Vec);
    assert(It != ValueInfos.end());
    const ValueInfo &SrcInfo = It->second;

    // Extract extracted slices
    return VIB.createExtraction(SrcInfo, *Offset);
  }
  case Instruction::ExtractValue: {
    auto *EV = cast<ExtractValueInst>(Inst);
    auto *Src = EV->getAggregateOperand();

    unsigned Offset = computeByteOffsetInAggregate(Src->getType(), EV->getIndices(), DL);

    // Obtain ValueInfo for the source aggregate
    auto It = ValueInfos.find(Src);
    assert(It != ValueInfos.end());
    const ValueInfo &SrcInfo = It->second;

    // Extract extracted slices
    return VIB.createExtraction(SrcInfo, Offset);
  }
  case Instruction::InsertElement: {
    // TODO: Support shufflevector
    auto *IE = cast<InsertElementInst>(Inst);
    auto *Vec = IE->getOperand(0);
    auto *Inserted = IE->getOperand(1);
    auto *IndexArg = IE->getOperand(2);

    std::optional<unsigned> Offset = computeByteOffsetInVector(Vec->getType(), IndexArg, DL);
    if (!Offset.has_value())
      return VIB.createDynamic();

    auto VecIt = ValueInfos.find(Vec);
    auto InsertedIt = ValueInfos.find(Inserted);
    assert(VecIt != ValueInfos.end() && InsertedIt != ValueInfos.end());
    const auto &VecInfo = VecIt->second;
    const auto &InsertedInfo = InsertedIt->second;
    unsigned NumInsertedBits = Inserted->getType()->getPrimitiveSizeInBits();
    assert(NumInsertedBits % 8 == 0 && NumInsertedBits == 8 * DL.getTypeStoreSize(Inserted->getType()));
    unsigned NumInsertedBytes = NumInsertedBits / 8;

    // Combine AggInfo and InsertedInfo
    return VIB.createInsertion(VecInfo, InsertedInfo, *Offset, NumInsertedBytes);
  }
  case Instruction::InsertValue: {
    auto *IV = cast<InsertValueInst>(Inst);
    auto *Agg = IV->getAggregateOperand();
    auto *Inserted = IV->getInsertedValueOperand();
    auto AggIt = ValueInfos.find(Agg);
    auto InsertedIt = ValueInfos.find(Inserted);
    assert(AggIt != ValueInfos.end() && InsertedIt != ValueInfos.end());

    const auto &AggInfo = AggIt->second;
    const auto &InsertedInfo = InsertedIt->second;

    unsigned Offset = computeByteOffsetInAggregate(Agg->getType(), IV->getIndices(), DL);
    unsigned NumInsertedBytes = DL.getTypeStoreSize(Inserted->getType());

    // Combine AggInfo and InsertedInfo
    return VIB.createInsertion(AggInfo, InsertedInfo, Offset, NumInsertedBytes);
  }
  case Instruction::PHI: {
    auto *PN = cast<PHINode>(Inst);
    SmallVector<const ValueInfo *, 2> ArgValueInfos;
    for (Value *Val : PN->incoming_values()) {
      auto It = ValueInfos.find(Val);
      if (It == ValueInfos.end()) {
        // The incoming value has not been analyzed yet.
        // This can be caused by a loop, which we currently don't support.
        // We could repeatedly propagate through the loop until a stable state is reached.
        return VIB.createDynamic();
      }
      ArgValueInfos.push_back(&It->second);
    }
    return VIB.createSelect(ArgValueInfos);
  }
  case Instruction::Select: {
    auto *SI = cast<SelectInst>(Inst);
    auto *TrueVal = SI->getTrueValue();
    auto *FalseVal = SI->getFalseValue();
    auto TrueIt = ValueInfos.find(TrueVal);
    auto FalseIt = ValueInfos.find(FalseVal);
    assert(TrueIt != ValueInfos.end() && FalseIt != ValueInfos.end());

    const auto &TrueInfo = TrueIt->second;
    const auto &FalseInfo = FalseIt->second;

    return VIB.createSelect({&TrueInfo, &FalseInfo});
  }
  // For these instructions, don't waste time trying to compute known bits
  case Instruction::Call:
  case Instruction::GetElementPtr:
  case Instruction::Load:
  case Instruction::PtrToInt: // PtrToInt and IntToPtr could be supported, but modeling the trunc/zext
  case Instruction::IntToPtr: //  part is annoying, and we don't need it now.
  case Instruction::Store: {
    return VIB.createDynamic();
  }
  default: {
    // As last option, try to use computeKnownBits if possible.
    // computeKnownBits also supports vector type, but in that case returns bits common bits of all elements.
    // We are however interested in bits of the whole value. Working on the full vector would require a bitcast
    // to an integer, but we don't wan't to add instructions in the analysis.
    if (V->getType()->isIntegerTy()) {
      auto KnownBits = computeKnownBits(V, DL);
      return VIB.createConstant(KnownBits);
    }
    return VIB.createDynamic();
  }
  }
  llvm_unreachable("unexpected case");
}

ValueInfo &ValueOriginTracker::getOrComputeValueInfo(llvm::Value *V, bool KnownToBeNew) {
  if (!KnownToBeNew) {
    auto It = ValueInfos.find(V);
    if (It != ValueInfos.end())
      return It->second;
  }
  auto InsertionResult = ValueInfos.insert({V, computeValueInfo(V)});
  assert(InsertionResult.second);
  return InsertionResult.first->second;
}

ValueInfo ValueOriginTracker::getValueInfo(llvm::Value *V) {
  analyzeValues(V);
  assert(ValueInfos.contains(V));
  return ValueInfos[V];
}

void ValueOriginTracker::analyzeValues(ArrayRef<Value *> Values) {
  SmallVector<Instruction *> WorkList;
  SetVector<Function *> PendingFunctions;
  DenseSet<BasicBlock *> PendingBBs;
  DenseSet<Instruction *> PendingInstructions;

  // Collect all values that the passed values depend on, by working through
  // all operands. Instructions are marked in PendingInstructions for later
  // processing, other values are directly processed.

  auto AddToWorkList = [&](Value *V) {
    if (ValueInfos.contains(V)) {
      // Already analyzed, nothing to do
      return;
    }
    if (auto *Inst = dyn_cast<Instruction>(V)) {
      bool Inserted = PendingInstructions.insert(Inst).second;
      if (Inserted) {
        WorkList.push_back(Inst);
        if (PendingBBs.insert(Inst->getParent()).second)
          PendingFunctions.insert(Inst->getFunction());
      }
    } else {
      // With general value assumptions, we'd need to add something here to ensure processing of dependencies.
      static_assert(std::is_same_v<ValueOriginAssumptions::key_type, Instruction *>);
      getOrComputeValueInfo(V, true);
    }
  };

  for (auto *V : Values)
    AddToWorkList(V);

  while (!WorkList.empty()) {
    // Add instruction operands to the work list
    auto *Inst = WorkList.pop_back_val();
    for (auto &Op : Inst->operands())
      AddToWorkList(Op);

    // Add any instructions referenced by origin assumptions to the work list as well
    auto OriginAssumptionIt = OriginAssumptions.find(Inst);
    if (OriginAssumptionIt != OriginAssumptions.end()) {
      const ValueInfo &VI = OriginAssumptionIt->second;
      for (const SliceInfo &SI : VI.Slices) {
        if (SI.DynamicValue)
          AddToWorkList(SI.DynamicValue);
      }
    }
  }

  for (auto *F : PendingFunctions) {
    // Traverse BBs of the function in RPO order.
    // This ensures instruction dependencies are analyzed before depending instructions, except for loops.
    ReversePostOrderTraversal<Function *> RPOT(F);
    for (auto &BB : RPOT) {
      if (!PendingBBs.contains(BB))
        continue;
      for (auto &Inst : *BB) {
        bool WasPending = PendingInstructions.erase(&Inst);
        if (WasPending)
          getOrComputeValueInfo(&Inst, true);
      }
    }
  }
}

} // namespace CompilerUtils
